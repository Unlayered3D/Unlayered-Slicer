// UNLAYERED_MOD -- headless driver for automatic plane selection and multiplanar toolpath audits.
//
// Development and validation tool, not shipped. It never talks to a printer: it loads a model or
// project, optionally plans/strips cut planes, slices to a G-code file in a directory you name, and
// reports the planner's prediction and the audit of what was actually sliced, as JSON.
//
//   auto_planes_cli --in part.stl|project.3mf [--config printer.json]... [--set key=value]...
//                   [--object N] [--plan] [--apply] [--strip-planes] [--rotate rx,ry,rz] [--mirror-x]
//                   [--slice out.gcode] [--json report.json] [--seam-anchor]
//
// --plan     run the planner on every (or the --object) object and print the plan
// --apply    replace each object's cut planes with the chosen plan (or none, if planar was kept)
// --strip-planes   remove every cut plane first (the planar baseline of a project)
// --slice    slice and export G-code to that path, then audit the toolpaths

#include "libslic3r/libslic3r.h"
#include "libslic3r/AutoPlanes.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/MultiplanarAudit.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace Slic3r;

namespace {

std::string json_escape(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

std::string metrics_json(const AutoPlanes::PredictedMetrics &m)
{
    std::ostringstream ss;
    ss << "{\"overhang_area_mm2\":" << m.overhang_area_mm2 << ",\"support_volume_proxy_mm3\":" << m.support_volume_mm3
       << ",\"terrace_length_mm\":" << m.terrace_length_mm << ",\"stepped_area_mm2\":" << m.stepped_area_mm2
       << ",\"layers\":" << m.layers << ",\"transitions\":" << m.transitions << ",\"effort_index_s\":" << m.effort_index_s << "}";
    return ss.str();
}

std::string candidate_json(const AutoPlanes::Candidate &c)
{
    std::ostringstream ss;
    ss << "{\"name\":\"" << json_escape(c.name) << "\",\"feasible\":" << (c.feasible ? "true" : "false")
       << ",\"net_benefit\":" << c.net_benefit << ",\"predicted\":" << metrics_json(c.predicted) << ",\"planes\":[";
    for (size_t i = 0; i < c.planes.size(); ++ i) {
        const auto &p = c.planes[i];
        ss << (i ? "," : "") << "{\"origin\":\"" << json_escape(p.origin) << "\",\"tilt_deg\":" << p.tilt_deg
           << ",\"azimuth_deg\":" << p.azimuth_deg << ",\"limit_deg\":" << p.limit_deg << ",\"reach_mm\":" << p.reach_mm
           << ",\"cut_vs_layers_deg\":" << p.cut_vs_build_deg << "}";
    }
    ss << "],\"rejections\":[";
    for (size_t i = 0; i < c.rejections.size(); ++ i) ss << (i ? "," : "") << "\"" << json_escape(c.rejections[i]) << "\"";
    ss << "]}";
    return ss.str();
}

} // namespace

int main(int argc, char **argv)
{
    boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::warning);

    std::string in_path, slice_path, json_path;
    std::vector<std::string> config_paths, sets;
    int  only_object = -1;
    bool do_plan = false, do_apply = false, strip = false, mirror_x = false;
    Vec3d rot_deg = Vec3d::Zero();
    for (int i = 1; i < argc; ++ i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; std::exit(2); } return argv[++ i]; };
        if      (a == "--in")           in_path = next();
        else if (a == "--config")       config_paths.push_back(next());
        else if (a == "--set")          sets.push_back(next());
        else if (a == "--object")       only_object = std::stoi(next());
        else if (a == "--plan")         do_plan = true;
        else if (a == "--apply")        { do_plan = true; do_apply = true; }
        else if (a == "--strip-planes") strip = true;
        else if (a == "--mirror-x")     mirror_x = true;
        else if (a == "--rotate")       { std::vector<std::string> v; boost::split(v, next(), boost::is_any_of(",")); for (int k = 0; k < 3 && k < int(v.size()); ++ k) rot_deg[k] = std::stod(v[size_t(k)]); }
        else if (a == "--slice")        slice_path = next();
        else if (a == "--json")         json_path = next();
        else { std::cerr << "unknown argument " << a << "\n"; return 2; }
    }
    if (in_path.empty()) { std::cerr << "--in is required\n"; return 2; }

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    Model model;
    try {
        DynamicPrintConfig loaded;
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
        const bool project = boost::iends_with(in_path, ".3mf");
        model = Model::read_from_file(in_path, &loaded, &ctx,
                                      project ? (LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances)
                                              : LoadStrategy::AddDefaultInstances);
        config.apply(loaded, true);
    } catch (const std::exception &ex) {
        std::cerr << "load failed: " << ex.what() << "\n";
        return 3;
    }
    for (const std::string &cp : config_paths) {
        DynamicPrintConfig c;
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
        std::map<std::string, std::string> kv;
        std::string reason;
        if (c.load_from_json(cp, ctx, true, kv, reason) != 0) {
            std::cerr << "config " << cp << ": " << reason << "\n";
            return 3;
        }
        config.apply(c, true);
    }
    for (const std::string &kv : sets) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) { std::cerr << "bad --set " << kv << "\n"; return 2; }
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
        config.set_deserialize(kv.substr(0, eq), kv.substr(eq + 1), ctx);
    }

    for (ModelObject *mo : model.objects) {
        if (mo->instances.empty())
            mo->add_instance();
        if (strip)
            mo->cut_planes = CutPlaneStack();
        if (rot_deg != Vec3d::Zero() || mirror_x) {
            for (ModelInstance *mi : mo->instances) {
                if (rot_deg != Vec3d::Zero())
                    mi->set_rotation(Vec3d(rot_deg * (PI / 180.)));
                if (mirror_x)
                    mi->set_mirror(Vec3d(-1., 1., 1.));
            }
            mo->ensure_on_bed();
        }
    }
    if (model.objects.size() == 1 && model.objects.front()->instances.size() == 1) {
        // A bare mesh: centre it on the bed.
        BoundingBoxf bed;
        if (const auto *area = config.option<ConfigOptionPoints>("printable_area"); area != nullptr && ! area->values.empty())
            bed = BoundingBoxf(area->values);
        if (bed.defined && ! boost::iends_with(in_path, ".3mf")) {
            ModelInstance *mi = model.objects.front()->instances.front();
            const Vec2d c = bed.center();
            mi->set_offset(Vec3d(c.x(), c.y(), mi->get_offset().z()));
            model.objects.front()->ensure_on_bed();
        }
    }

    std::ostringstream json;
    json << "{\"input\":\"" << json_escape(in_path) << "\",\"objects\":[";
    bool first_obj = true;
    for (size_t oi = 0; oi < model.objects.size(); ++ oi) {
        ModelObject *mo = model.objects[oi];
        if (only_object >= 0 && int(oi) != only_object)
            continue;
        json << (first_obj ? "" : ",") << "{\"index\":" << oi << ",\"name\":\"" << json_escape(mo->name) << "\"";
        first_obj = false;
        if (do_plan) {
            AutoPlanes::Options opt;
            const auto t0 = std::chrono::steady_clock::now();
            AutoPlanes::PlanResult r = AutoPlanes::plan(*mo, mo->instances.front()->get_matrix(), config, opt);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::cout << "=== object " << oi << " \"" << mo->name << "\" (" << int(ms) << " ms)\n" << AutoPlanes::to_text(r);
            json << ",\"plan_ms\":" << ms << ",\"machine\":\"" << AutoPlanes::machine_kind_name(r.machine.kind)
                 << "\",\"can_tilt\":" << (r.machine.can_tilt ? "true" : "false")
                 << ",\"decision\":\"" << json_escape(r.decision) << "\",\"chosen\":" << r.chosen
                 << ",\"planar\":" << candidate_json(r.planar);
            if (r.existing) json << ",\"existing\":" << candidate_json(*r.existing);
            json << ",\"candidates\":[";
            for (size_t i = 0; i < r.candidates.size() && i < 6; ++ i) json << (i ? "," : "") << candidate_json(r.candidates[i]);
            json << "],\"skeleton\":\"" << json_escape(r.skeleton_note) << "\"";
            if (do_apply) {
                const AutoPlanes::Candidate *c = r.chosen_candidate();
                mo->cut_planes = c ? AutoPlanes::stack_for(CutPlaneStack(), *c, false) : CutPlaneStack();
            }
        }
        json << ",\"cut_planes\":" << (mo->cut_planes.active() ? mo->cut_planes.planes.size() : 0) << "}";
    }
    json << "]";

    int rc = 0;
    if (! slice_path.empty()) {
        Print print;
        try {
            print.apply(model, config);
            const auto v = print.validate();
            if (! v.string.empty()) {
                std::cerr << "validate: " << v.string << "\n";
                json << ",\"validate\":\"" << json_escape(v.string) << "\"";
                rc = 4;
            } else {
                const auto t0 = std::chrono::steady_clock::now();
                print.process();
                GCodeProcessorResult result;
                print.export_gcode(slice_path, &result, nullptr);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                MultiplanarAuditParams ap;
                if (const auto *o = config.option("support_top_z_distance"))    ap.support_top_z_mm    = o->getFloat();
                if (const auto *o = config.option("support_bottom_z_distance")) ap.support_bottom_z_mm = o->getFloat();
                const MultiplanarAuditResult audit = audit_multiplanar_toolpaths(result, slice_path, ap);
                const auto &stats = result.print_statistics;
                double filament_mm = 0., support_mm = 0.;
                for (const auto &m : result.moves)
                    if (m.type == EMoveType::Extrude && m.delta_extruder > 0.f) {
                        filament_mm += m.delta_extruder;
                        if (m.extrusion_role == erSupportMaterial || m.extrusion_role == erSupportMaterialInterface || m.extrusion_role == erSupportTransition)
                            support_mm += m.delta_extruder;
                    }
                const ConflictResultOpt &conflict = print.get_conflict_result();
                std::cout << "=== slice (" << int(ms) << " ms): " << print.objects().size() << " print objects\n"
                          << audit.to_text()
                          << "Measured filament " << filament_mm << " mm, support " << support_mm << " mm, time "
                          << stats.modes[0].time << " s\n"
                          << "Hotend collision check: " << (conflict ? ("COLLISION " + conflict->_objName1 + " vs " + conflict->_objName2) : std::string(result.tool_axes.size() > 1 ? "clear" : "not run (single tool axis)")) << "\n";
                json << ",\"slice\":{\"ms\":" << ms << ",\"print_objects\":" << print.objects().size()
                     << ",\"filament_mm\":" << filament_mm << ",\"support_filament_mm\":" << support_mm
                     << ",\"time_s\":" << stats.modes[0].time << ",\"tool_axes\":" << result.tool_axes.size()
                     << ",\"hotend_check\":\"" << (conflict ? "collision" : (result.tool_axes.size() > 1 ? "clear" : "not run")) << "\""
                     << ",\"audit\":" << audit.to_json() << "}";
            }
        } catch (const std::exception &ex) {
            std::cerr << "slice failed: " << ex.what() << "\n";
            json << ",\"error\":\"" << json_escape(ex.what()) << "\"";
            rc = 5;
        }
    }
    json << "}";
    if (! json_path.empty()) {
        std::ofstream f(json_path);
        f << json.str() << "\n";
    }
    return rc;
}
