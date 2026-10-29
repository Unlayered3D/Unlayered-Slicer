// Automation actions: preview.getStats / preview.setView / preview.getMove / view.capture.
// Owner: work package S5 (plates). See docs/HLSD/bridge-actions.md for how to write an action and
// docs/bridge-actions/plates.md for what these do.
//
// preview.getStats sums the plate's slice result per feature, option row and layer (what the
// preview's legend shows). preview.setView does what the legend (colouring combo, feature/option/
// section rows, time-mode and belt buttons) and the layer and moves sliders do, through the
// GCodeViewer setters that mirror those clicks; it checks everything first and changes nothing on
// a refusal. preview.getMove reads the tool-position window (and, behind the printer permission,
// the G-code window's lines). view.capture returns a PNG of the plate: the Prepare picture is
// rendered off-screen from the camera asked for (the plate-thumbnail renderer, so it works while a
// web tab covers the canvas); the Preview picture is what the preview canvas shows, so it must be
// on screen.
//
// Layer numbers are the preview slider's everywhere (PP::preview_layers): layer N is the slider's
// Nth layer, here, in layer.* and in preview.getStats.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "PlatesPreviewDescriptors.hpp"
#include "PlatesPreviewLogic.hpp"
#include "Register.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>

#include <glad/gl.h>

#include <wx/glcanvas.h>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include "../Camera.hpp"
#include "../GCodeViewer.hpp"
#include "../GLCanvas3D.hpp"
#include "../GUI_App.hpp"
#include "../IMSlider.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../WebAppBridge.hpp"
#include "../WebAppSettings.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

namespace PP = PlatesPreview;
using nlohmann::json;

namespace {

int target_plate(const InvokeContext& ctx)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return -1;
    PartPlateList& plates = plater->get_partplate_list();
    const int      idx    = ctx.targets.plate >= 0 ? ctx.targets.plate : plates.get_curr_plate_index();
    return (idx >= 0 && idx < plates.get_plate_count()) ? idx : -1;
}

InvokeResult no_plate() { return InvokeResult::error(WAB_NOT_FOUND, "no such plate", {{"missing", "plate"}}); }

// The preview's colourings: PP::view_type_ids() lists them in libvgcode::EViewType order.
static_assert(size_t(libvgcode::EViewType::COUNT) == 18, "a preview colouring was added: extend PP::view_type_ids()");

const char* view_type_id(libvgcode::EViewType t)
{
    const size_t i = size_t(t);
    return i < PP::view_type_ids().size() ? PP::view_type_ids()[i].c_str() : "featureType";
}

bool view_type_from_id(const std::string& id, libvgcode::EViewType& out)
{
    const auto& ids = PP::view_type_ids();
    auto        it  = std::find(ids.begin(), ids.end(), id);
    if (it == ids.end())
        return false;
    out = libvgcode::EViewType(it - ids.begin());
    return true;
}

// Capture cameras: PP::camera_ids() -> the thumbnail renderer's angle and the View menu's direction.
struct CameraName { Camera::ViewAngleType angle; const char* id; const char* direction; };
const std::vector<CameraName>& camera_names()
{
    using A = Camera::ViewAngleType;
    static const std::vector<CameraName> names = {
        {A::Iso, "iso", "iso"},          {A::Top_Front, "topFront", "topfront"}, {A::Top, "top", "top"},
        {A::Bottom, "bottom", "bottom"}, {A::Front, "front", "front"},           {A::Rear, "rear", "rear"},
        {A::Left, "left", "left"},       {A::Right, "right", "right"},
    };
    return names;
}

const CameraName* find_camera(const std::string& id)
{
    for (const CameraName& n : camera_names())
        if (id == n.id)
            return &n;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Shared preview state
// ---------------------------------------------------------------------------------------------

// The legend's option rows, in PP::preview_option_ids() order.
const std::array<libvgcode::EOptionType, 6>& option_types()
{
    static const std::array<libvgcode::EOptionType, 6> types = {libvgcode::EOptionType::Travels,       libvgcode::EOptionType::Wipes,
                                                                libvgcode::EOptionType::Retractions,   libvgcode::EOptionType::Unretractions,
                                                                libvgcode::EOptionType::Seams,         libvgcode::EOptionType::ToolChanges};
    return types;
}

const char* time_mode_id(libvgcode::ETimeMode m) { return m == libvgcode::ETimeMode::Stealth ? "stealth" : "normal"; }

const char* move_type_id(libvgcode::EMoveType t)
{
    using T = libvgcode::EMoveType;
    switch (t) {
    case T::Retract: return "retract";
    case T::Unretract: return "unretract";
    case T::Seam: return "seam";
    case T::ToolChange: return "toolChange";
    case T::ColorChange: return "colorChange";
    case T::PausePrint: return "pause";
    case T::CustomGCode: return "customGcode";
    case T::Travel: return "travel";
    case T::Wipe: return "wipe";
    case T::Extrude: return "extrude";
    default: return "noop";
    }
}

// Is the current plate's G-code loaded in the preview canvas?
bool preview_has_gcode(Plater* plater, GLCanvas3D* preview)
{
    return preview != nullptr && plater->is_preview_loaded() && preview->get_gcode_viewer().has_data() &&
           !preview->get_gcode_layers_zs().empty() && preview->get_gcode_viewer().get_layers_slider() != nullptr;
}

InvokeResult preview_not_loaded()
{
    return InvokeResult::error(WAB_ACTION_DISABLED,
                               "the preview has no sliced toolpaths to show; slice the plate and show the preview first "
                               "(preview.setView {show:\"preview\"})",
                               {{"reason", "preview_not_loaded"}});
}

// What GLCanvas3D::_render_gcode does with dirty sliders at the next frame, done now: the layers
// slider's range goes to the viewer (and the shells' clip range), which resets the moves slider;
// the moves slider's position becomes the current vertex. So a reply -- and a move set after a
// layer change -- sees the state the person will see, even while the preview is covered. Needs
// the GL context (libvgcode uploads the colour buffer); false when it cannot be made current.
bool sync_sliders(GLCanvas3D* preview)
{
    GCodeViewer& gv = preview->get_gcode_viewer();
    IMSlider*    layers = gv.get_layers_slider();
    IMSlider*    moves  = gv.get_moves_slider();
    if (!gv.has_data() || layers == nullptr || moves == nullptr)
        return false;
    if (!layers->is_dirty() && !moves->is_dirty())
        return true;
    if (!preview->make_current_for_postinit())
        return false;
    if (layers->is_dirty()) {
        preview->set_volumes_z_range(gv.get_layers_z_extent(size_t(std::max(0, layers->GetLowerValue())),
                                                            size_t(std::max(0, layers->GetHigherValue()))));
        gv.set_layers_z_range({unsigned(std::max(0, layers->GetLowerValue())), unsigned(std::max(0, layers->GetHigherValue()))});
        layers->set_as_dirty(false);
    }
    if (moves->is_dirty()) {
        moves->set_as_dirty(false);
        gv.update_sequential_view_current(static_cast<unsigned int>(std::max(0.0, moves->GetLowerValueD() - 1.0)),
                                          static_cast<unsigned int>(std::max(0.0, moves->GetHigherValueD() - 1.0)));
    }
    return true;
}

// The vertex the tool-position window describes. A seam vertex carries no meaningful data, so the
// window shows the last visible non-seam vertex instead (Marker::render_position_window).
size_t described_vertex_id(const GCodeViewer& gv, bool& seam)
{
    size_t id = gv.get_current_vertex_id();
    seam      = gv.get_gcode_vertex_at(id).type == libvgcode::EMoveType::Seam;
    while (id > 0 && gv.get_gcode_vertex_at(id).type == libvgcode::EMoveType::Seam)
        --id;
    return id;
}

json vertex_json(const GCodeViewer& gv, size_t id)
{
    const libvgcode::PathVertex& v    = gv.get_gcode_vertex_at(id);
    const bool                   extr = v.is_extrusion();
    const size_t                 mode = size_t(gv.get_time_mode());
    json                         j    = {{"vertex", id},
                                         {"type", move_type_id(v.type)},
                                         {"position", {v.position[0], v.position[1], v.position[2]}},
                                         {"layer", v.layer_id + 1},
                                         {"tool", int(v.extruder_id) + 1},
                                         {"speedMmS", v.feedrate},
                                         {"actualSpeedMmS", v.actual_feedrate},
                                         {"accelerationMmS2", v.acceleration},
                                         {"jerkMmS", v.jerk},
                                         {"fanPercent", v.fan_speed},
                                         {"temperatureC", v.temperature},
                                         {"pressureAdvance", v.pressure_advance},
                                         {"moveTimeS", mode < v.times.size() ? v.times[mode] : 0.f},
                                         {"elapsedTimeS", gv.get_estimated_time_at(id)},
                                         {"layerTimeS", v.layer_duration},
                                         {"gcodeLine", v.gcode_id}};
    if (extr) {
        j["feature"]    = PP::extrusion_role_id(libvgcode::convert(v.role));
        j["widthMm"]    = v.width;
        j["heightMm"]   = v.height;
        j["flowMm3S"]   = v.volumetric_rate();
    }
    if (v.cut_segment_id >= 0)
        j["section"] = int(v.cut_segment_id) + 1;
    // Length of the G-code move ending here: an arc is several vertices with one G-code line.
    if (id > 0 && (extr || v.is_travel() || v.is_wipe())) {
        size_t first = id, last = id;
        const size_t count = gv.get_vertices_count();
        while (first > 0 && gv.get_gcode_vertex_at(first - 1).gcode_id == v.gcode_id)
            --first;
        while (last + 1 < count && gv.get_gcode_vertex_at(last + 1).gcode_id == v.gcode_id)
            ++last;
        float length = 0.f;
        for (size_t i = std::max<size_t>(first, 1); i <= last; ++i)
            length += (libvgcode::convert(gv.get_gcode_vertex_at(i).position) - libvgcode::convert(gv.get_gcode_vertex_at(i - 1).position)).norm();
        j["lengthMm"] = length;
    }
    return j;
}

json move_slider_json(GCodeViewer& gv)
{
    IMSlider* moves = gv.get_moves_slider();
    if (moves == nullptr)
        return nullptr;
    return {{"index", moves->GetHigherValue() - moves->GetMinValue() + 1}, {"count", moves->GetMaxValue() - moves->GetMinValue() + 1}};
}

// ---------------------------------------------------------------------------------------------
// preview.getStats
// ---------------------------------------------------------------------------------------------

size_t time_mode_index(const InvokeContext& ctx) { return ctx.params.value("timeMode", std::string("normal")) == "stealth" ? 1 : 0; }

InvokeResult no_stealth_times()
{
    return InvokeResult::error(WAB_ACTION_DISABLED, "this G-code has no stealth-mode time estimate; ask for timeMode \"normal\"",
                               {{"reason", "no_stealth_times"}, {"path", "/params/timeMode"}});
}

// All Plates Stats (GCodeViewer::render_all_plates_stats): every plate with objects, all sliced.
InvokeResult run_all_plates_stats(InvokeContext& ctx, size_t mode)
{
    PartPlateList&               plates = ctx.plater()->get_partplate_list();
    std::vector<PP::PlateFigures> figures;
    json                         unsliced = json::array();
    const GCodeProcessorResult*  first    = nullptr;
    for (PartPlate* plate : plates.get_nonempty_plate_list()) {
        const GCodeProcessorResult* result = plate->get_slice_result();
        if (!plate->is_slice_result_valid() || result == nullptr || result->moves.empty()) {
            unsliced.push_back(plate->get_index());
            continue;
        }
        if (first == nullptr)
            first = result;
        PP::PlateFigures f;
        f.plate  = plate->get_index();
        f.stats  = &result->print_statistics;
        f.time_s = result->print_statistics.modes[mode].time;
        if (const Print* print = plate->fff_print(); print != nullptr)
            f.cost = print->print_statistics().total_cost;
        figures.push_back(f);
    }
    if (!unsliced.empty())
        return InvokeResult::error(WAB_ACTION_DISABLED, "not every plate with objects is sliced; slice them first (slice.all)",
                                   {{"reason", "not_sliced"}, {"unsliced", unsliced}});
    if (figures.empty())
        return InvokeResult::error(WAB_ACTION_DISABLED, "no plate has objects", {{"reason", "no_objects"}});
    if (mode == 1 && std::all_of(figures.begin(), figures.end(), [](const PP::PlateFigures& f) { return f.time_s <= 0.; }))
        return no_stealth_times();
    json out = PP::all_plates_json(figures, first->filament_diameters, first->filament_densities, first->filament_costs);
    out["allPlates"] = true;
    out["timeMode"]  = mode == 1 ? "stealth" : "normal";
    return InvokeResult::ok(out);
}

InvokeResult run_get_stats(InvokeContext& ctx)
{
    const size_t mode = time_mode_index(ctx);
    if (ctx.params.value("allPlates", false))
        return run_all_plates_stats(ctx, mode);
    const int idx = target_plate(ctx);
    if (idx < 0)
        return no_plate();
    PartPlate* plate = ctx.plater()->get_partplate_list().get_plate(idx);
    if (!plate->is_slice_result_valid())
        return InvokeResult::error(WAB_ACTION_DISABLED, "plate " + std::to_string(idx) + " has no valid slice result; slice it first",
                                   {{"reason", "not_sliced"}});
    json out     = WebAppSettings::slice_summary_json(plate);
    out["plate"] = idx;
    out["timeMode"] = mode == 1 ? "stealth" : "normal";
    const GCodeProcessorResult* result = plate->get_slice_result();
    if (result == nullptr || result->moves.empty()) {
        out["features"] = nullptr;
        out["note"]     = "per-feature figures are not available for this result";
        return InvokeResult::ok(out);
    }
    const PrintEstimatedStatistics& st = result->print_statistics;
    if (mode == 1) {
        if (st.modes[1].time <= 0.f)
            return no_stealth_times();
        out["timeSeconds"]        = st.modes[1].time;
        out["prepareTimeSeconds"] = st.modes[1].prepare_time;
    }
    const bool   layers     = ctx.params.value("layers", false);
    const size_t max_layers = size_t(ctx.params.value("maxLayers", 2000));
    const auto   summary    = PP::summarize_moves(result->moves, mode);
    json         moves      = PP::move_summary_json(summary, layers, max_layers);
    for (auto it = moves.begin(); it != moves.end(); ++it)
        out[it.key()] = it.value();
    // The legend's Seams row shows the seam gap + scarf length when the G-code has one.
    out["options"]["seams"]["gapMm"]   = st.total_seam_gap_distance;
    out["options"]["seams"]["scarfMm"] = st.total_seam_scarf_distance;
    out["filamentChangeTime"]          = PP::filament_change_time_json(st);
    out["ranges"]                      = PP::color_ranges_json(result->moves, mode);

    // Filament per slot (1-based), as the legend's filament table: mm3, m and g per use, and cost.
    json per_filament = json::array();
    for (const auto& [extruder, volume] : st.total_volumes_per_extruder)
        per_filament.push_back(PP::filament_slot_json(st, extruder, result->filament_diameters, result->filament_densities,
                                                      result->filament_costs));
    out["filaments"] = per_filament;
    return InvokeResult::ok(out);
}

// ---------------------------------------------------------------------------------------------
// preview.setView
// ---------------------------------------------------------------------------------------------

json view_state()
{
    Plater*     plater  = wxGetApp().plater();
    GLCanvas3D* preview = plater->get_preview_canvas3D();
    json        out     = {{"tab", plater->is_preview_shown() ? "preview" : (plater->is_view3D_shown() ? "prepare" : "other")},
                           {"previewLoaded", plater->is_preview_loaded()}};
    if (preview == nullptr)
        return out;
    GCodeViewer& gv              = preview->get_gcode_viewer();
    out["viewType"]              = view_type_id(preview->get_gcode_view_type());
    json offered                 = json::array();
    for (libvgcode::EViewType t : gv.get_offered_view_types())
        offered.push_back(view_type_id(t));
    out["viewTypes"]             = offered;
    const std::vector<double> zs = preview->get_gcode_layers_zs();
    IMSlider*                 sl = gv.get_layers_slider();
    out["layerCount"]            = zs.size();
    if (zs.empty() || sl == nullptr || !gv.has_data())
        return out;
    const int lo = std::clamp(sl->GetLowerValue(), 0, int(zs.size()) - 1);
    const int hi = std::clamp(sl->GetHigherValue(), 0, int(zs.size()) - 1);
    out["layerRange"] = {lo + 1, hi + 1};
    out["zRange"]     = {zs[size_t(lo)], zs[size_t(hi)]};
    out["oneLayer"]   = sl->is_one_layer();
    out["move"]       = move_slider_json(gv);
    json options      = json::object();
    for (size_t i = 0; i < option_types().size(); ++i)
        options[PP::preview_option_ids()[i]] = gv.is_option_visible(option_types()[i]);
    out["options"] = options;
    json features  = json::object();
    for (libvgcode::EGCodeExtrusionRole role : gv.get_extrusion_roles())
        if (role < libvgcode::EGCodeExtrusionRole::COUNT && role != libvgcode::EGCodeExtrusionRole::None &&
            role != libvgcode::EGCodeExtrusionRole::Mixed)
            features[PP::extrusion_role_id(libvgcode::convert(role))] = gv.is_extrusion_role_visible(role);
    out["features"] = features;
    if (const std::vector<int16_t> sections = gv.get_used_cut_segments_ids(); sections.size() > 1) {
        json arr = json::array();
        for (int16_t id : sections)
            arr.push_back({{"section", int(id) + 1}, {"visible", gv.is_cut_segment_visible(id)}});
        out["sections"] = arr;
    }
    out["timeMode"] = time_mode_id(gv.get_time_mode());
    const std::vector<libvgcode::ETimeMode> modes = gv.get_time_modes();
    out["stealthTimes"] = std::find(modes.begin(), modes.end(), libvgcode::ETimeMode::Stealth) != modes.end();
    if (gv.is_belt_view())
        out["beltRaw"] = !gv.is_belt_show_designed();
    return out;
}

// Validated, resolved preview.setView request. Nothing is changed until everything checks out.
struct ViewRequest
{
    std::string                                        show;
    std::optional<libvgcode::EViewType>                view_type;
    std::optional<std::array<int, 2>>                  range; // 0-based layers
    bool                                               range_clamped = false;
    std::optional<bool>                                one_layer;
    std::optional<std::pair<std::string, int>>         move;  // index|to|step, value
    std::vector<std::pair<libvgcode::EGCodeExtrusionRole, bool>> features;
    std::vector<std::pair<libvgcode::EOptionType, bool>> options;
    std::optional<std::set<int16_t>>                   sections;
    std::optional<libvgcode::ETimeMode>                time_mode;
    std::optional<bool>                                belt_raw;

    bool needs_gcode() const
    {
        return range || one_layer || move || !features.empty() || !options.empty() || sections || time_mode || belt_raw;
    }
};

// The checks that need no preview: ids and shapes.
std::optional<InvokeResult> parse_view_request(const json& p, ViewRequest& r)
{
    r.show = p.value("show", std::string());
    if (p.contains("viewType")) {
        libvgcode::EViewType type = libvgcode::EViewType::FeatureType;
        if (!view_type_from_id(p["viewType"].get<std::string>(), type))
            return InvokeResult::error(WAB_BAD_PARAMS, "unknown view type", {{"path", "/params/viewType"}});
        r.view_type = type;
    }
    if (p.contains("oneLayer"))
        r.one_layer = p["oneLayer"].get<bool>();
    if (p.contains("layerRange") && p.contains("zRange"))
        return InvokeResult::error(WAB_BAD_PARAMS, "give layerRange or zRange, not both", {{"path", "/params/zRange"}});
    if (p.contains("move")) {
        const json& m = p["move"];
        if (m.contains("index"))
            r.move = std::make_pair(std::string("index"), m["index"].get<int>());
        else if (m.contains("step"))
            r.move = std::make_pair(std::string("step"), m["step"].get<int>());
        else
            r.move = std::make_pair(m["to"].get<std::string>(), 0);
    }
    if (p.contains("features"))
        for (auto it = p["features"].begin(); it != p["features"].end(); ++it) {
            const auto role = PP::extrusion_role_from_id(it.key());
            if (!role)
                return InvokeResult::error(WAB_BAD_PARAMS, "unknown feature '" + it.key() + "'", {{"path", "/params/features/" + it.key()}});
            r.features.emplace_back(libvgcode::convert(*role), it.value().get<bool>());
        }
    if (p.contains("options"))
        for (auto it = p["options"].begin(); it != p["options"].end(); ++it) {
            const auto& ids = PP::preview_option_ids();
            const auto  at  = std::find(ids.begin(), ids.end(), it.key());
            if (at == ids.end())
                return InvokeResult::error(WAB_BAD_PARAMS, "unknown option '" + it.key() + "'", {{"path", "/params/options/" + it.key()}});
            r.options.emplace_back(option_types()[size_t(at - ids.begin())], it.value().get<bool>());
        }
    if (p.contains("sections")) {
        std::set<int16_t> ids;
        for (const json& n : p["sections"]) {
            const int v = n.get<int>();
            if (v < 1 || v > int(std::numeric_limits<int16_t>::max()))
                return InvokeResult::error(WAB_BAD_PARAMS, "sections are 1-based", {{"path", "/params/sections"}});
            ids.insert(int16_t(v - 1));
        }
        r.sections = ids;
    }
    if (p.contains("timeMode"))
        r.time_mode = p["timeMode"].get<std::string>() == "stealth" ? libvgcode::ETimeMode::Stealth : libvgcode::ETimeMode::Normal;
    if (p.contains("beltRaw"))
        r.belt_raw = p["beltRaw"].get<bool>();
    return std::nullopt;
}

// The checks against the loaded G-code. Fills r.range.
std::optional<InvokeResult> check_view_request(const json& p, Plater* plater, GLCanvas3D* preview, ViewRequest& r)
{
    if (preview == nullptr)
        return InvokeResult::error(WAB_ACTION_DISABLED, "the preview is not ready yet", {{"reason", "gl_not_ready"}});
    GCodeViewer& gv = preview->get_gcode_viewer();
    const bool   loaded = preview_has_gcode(plater, preview);
    if (r.needs_gcode() && !loaded)
        return preview_not_loaded();
    if (r.view_type) {
        const auto offered = gv.get_offered_view_types();
        if (std::find(offered.begin(), offered.end(), *r.view_type) == offered.end()) {
            json ids = json::array();
            for (libvgcode::EViewType t : offered)
                ids.push_back(view_type_id(t));
            return InvokeResult::error(WAB_ACTION_DISABLED, "the legend does not offer this colouring for this G-code",
                                       {{"reason", "view_type_unavailable"}, {"path", "/params/viewType"}, {"offered", ids}});
        }
    }
    if (r.sections) {
        const libvgcode::EViewType effective = r.view_type ? *r.view_type : gv.get_view_type();
        if (effective != libvgcode::EViewType::CutSegment)
            return InvokeResult::error(WAB_BAD_PARAMS, "sections apply to the section colouring; add viewType \"section\"",
                                       {{"path", "/params/sections"}});
        const std::vector<int16_t> used = gv.get_used_cut_segments_ids();
        for (int16_t id : *r.sections)
            if (std::find(used.begin(), used.end(), id) == used.end())
                return InvokeResult::error(WAB_BAD_PARAMS,
                                           "the print has no section " + std::to_string(int(id) + 1) + " (it has " +
                                               std::to_string(used.size()) + ")",
                                           {{"path", "/params/sections"}});
    }
    if (r.time_mode == libvgcode::ETimeMode::Stealth) {
        const auto modes = gv.get_time_modes();
        if (std::find(modes.begin(), modes.end(), libvgcode::ETimeMode::Stealth) == modes.end())
            return InvokeResult::error(WAB_ACTION_DISABLED, "this G-code has no stealth-mode time estimate",
                                       {{"reason", "no_stealth_times"}, {"path", "/params/timeMode"}});
    }
    if (r.belt_raw && !gv.is_belt_view())
        return InvokeResult::error(WAB_ACTION_DISABLED, "the raw G-code view is only for a belt printer's G-code",
                                   {{"reason", "not_belt_print"}, {"path", "/params/beltRaw"}});
    if (p.contains("layerRange") || p.contains("zRange")) {
        const std::vector<double> zs = preview->get_gcode_layers_zs();
        int lo = 0, hi = int(zs.size()) - 1;
        if (p.contains("layerRange")) {
            lo = p["layerRange"][0].get<int>() - 1;
            hi = p["layerRange"][1].get<int>() - 1;
        } else {
            lo = PP::nearest_layer_index(zs, p["zRange"][0].get<double>());
            hi = PP::nearest_layer_index(zs, p["zRange"][1].get<double>());
        }
        const int clo = std::clamp(lo, 0, int(zs.size()) - 1), chi = std::clamp(hi, 0, int(zs.size()) - 1);
        r.range_clamped = clo != lo || chi != hi;
        lo = clo;
        hi = chi;
        if (lo > hi)
            std::swap(lo, hi);
        r.range = std::array<int, 2>{lo, hi};
    }
    if (r.one_layer.value_or(false) && r.range && (*r.range)[0] != (*r.range)[1])
        return InvokeResult::error(WAB_BAD_PARAMS, "oneLayer shows a single layer; give layerRange [n, n] (or leave it out)",
                                   {{"path", "/params/oneLayer"}});
    // The sliders are applied now (sync_sliders), which needs the GL context.
    if ((r.move || r.range || r.one_layer) && !preview->make_current_for_postinit())
        return InvokeResult::error(WAB_ACTION_DISABLED, "the preview is not ready yet", {{"reason", "gl_not_ready"}});
    return std::nullopt;
}

InvokeResult run_set_view(InvokeContext& ctx)
{
    Plater*     plater  = ctx.plater();
    GLCanvas3D* preview = plater->get_preview_canvas3D();
    const json& p       = ctx.params;
    json        notes   = json::array();

    ViewRequest r;
    if (auto bad = parse_view_request(p, r))
        return *bad;

    // Switching tabs never slices (no_slice): a stale plate shows its shells until sliced. Switching
    // to Preview is what loads the plate's toolpaths, so it goes first; if the rest is refused the
    // tab is switched back, so a refusal changes nothing.
    const bool        was_preview = plater->is_preview_shown();
    const bool        was_prepare = plater->is_view3D_shown();
    const std::string want_tab    = r.show == "preview" ? "Preview" : (r.show == "prepare" ? "3D" : "");
    const bool        switch_tab  = !want_tab.empty() && (want_tab == "Preview" ? !was_preview : !was_prepare);
    if (switch_tab)
        plater->select_view_3D(want_tab, true);
    if (auto bad = check_view_request(p, plater, preview, r)) {
        if (switch_tab && (was_preview || was_prepare))
            plater->select_view_3D(was_preview ? "Preview" : "3D", true);
        return *bad;
    }

    GCodeViewer& gv = preview->get_gcode_viewer();
    IMSlider*    sl = gv.get_layers_slider();
    // One-layer mode first: the slider refuses the switch while its custom G-code popup is open.
    if (r.one_layer && sl != nullptr && sl->is_one_layer() != *r.one_layer && !sl->switch_one_layer_mode()) {
        if (switch_tab && (was_preview || was_prepare))
            plater->select_view_3D(was_preview ? "Preview" : "3D", true);
        return InvokeResult::error(WAB_BUSY, "the layer slider's custom G-code box is open; close it and retry",
                                   {{"reason", "slider_menu_open"}});
    }
    if (r.view_type)
        gv.select_view_type(*r.view_type); // as a pick in the legend's combo: label, visibility, moves slider
    if (r.time_mode)
        gv.set_time_mode(*r.time_mode);
    if (p.value("showAllFeatures", false))
        for (libvgcode::EGCodeExtrusionRole role : gv.get_extrusion_roles())
            gv.set_extrusion_role_visible(role, true);
    for (const auto& [role, visible] : r.features)
        gv.set_extrusion_role_visible(role, visible);
    for (const auto& [type, visible] : r.options)
        gv.set_option_visible(type, visible);
    if (r.sections)
        for (int16_t id : gv.get_used_cut_segments_ids())
            gv.set_cut_segment_visible(id, r.sections->count(id) > 0);
    if (r.range && sl != nullptr) {
        if (sl->is_one_layer() && (*r.range)[0] == (*r.range)[1])
            sl->SetHigherValue((*r.range)[1]); // one-layer mode: the lower thumb follows
        // (a span of several layers leaves one-layer mode, as SetSelectionSpan does)
        else
            sl->SetSelectionSpan((*r.range)[0], (*r.range)[1]);
        sl->set_as_dirty();
        if (r.range_clamped)
            notes.push_back("the layer range was clamped to the print's layers");
    }
    if (r.move || r.range || r.one_layer)
        sync_sliders(preview); // layer range -> moves slider, now
    if (r.move) {
        IMSlider* moves = gv.get_moves_slider();
        const int lo = moves->GetMinValue(), hi = moves->GetMaxValue();
        int       pos = moves->GetHigherValue();
        if (r.move->first == "index")
            pos = lo + r.move->second - 1;
        else if (r.move->first == "step")
            pos += r.move->second;
        else
            pos = r.move->first == "start" ? lo : hi;
        if (pos < lo || pos > hi)
            notes.push_back("the move was clamped to the shown layer's moves");
        moves->SetHigherValue(std::clamp(pos, lo, hi));
        moves->set_as_dirty();
        sync_sliders(preview);
    }
    if (r.belt_raw && *r.belt_raw == gv.is_belt_show_designed()) {
        // As the legend checkbox / B key: the mapping is baked into the vertices, so the preview reloads.
        gv.toggle_belt_show_designed();
        plater->CallAfter([plater]() { plater->refresh_belt_view(); });
        notes.push_back("the belt view reloads the preview; the layer range and move may reset");
    }

    preview->set_as_dirty();
    if (wxWindow* w = preview->get_wxglcanvas(); w != nullptr)
        w->Refresh();
    plater->get_current_canvas3D()->set_as_dirty();
    json out = view_state();
    if (r.move && preview_has_gcode(plater, preview)) {
        bool seam = false;
        out["vertex"] = vertex_json(gv, described_vertex_id(gv, seam));
    }
    if (!notes.empty())
        out["notes"] = notes;
    return InvokeResult::ok(out);
}

// ---------------------------------------------------------------------------------------------
// preview.getMove
// ---------------------------------------------------------------------------------------------

// The G-code text around the move. Gated exactly like G-code export (slicer.exportGcode,
// WebAppBridge::Impl::printer_gate): the tab's printer permission, on top of the settings
// permission every action has.
std::optional<InvokeResult> gcode_lines_json(const InvokeContext& ctx, const GCodeProcessorResult& result, size_t line_id,
                                             size_t before, size_t after, json& out)
{
    if (ctx.host == nullptr || !ctx.host->allows_gate(Gate::Printer))
        return InvokeResult::error(WAB_CAPABILITY_DISABLED,
                                   "the G-code text is gated like G-code export: printer access is off for this tab "
                                   "(Preferences > Developer); ask again without gcodeLines",
                                   {{"reason", "printer_permission_off"}, {"path", "/params/gcodeLines"}});
    const auto span = PP::gcode_line_span(result.lines_ends.size(), line_id, before, after);
    json       lines = json::array();
    if (span[0] != 0 && !result.filename.empty()) {
        boost::nowide::ifstream in(result.filename, std::ios::binary);
        for (size_t id = span[0]; in && id <= span[1]; ++id) {
            const auto  bytes = PP::gcode_line_bytes(result.lines_ends, id);
            std::string text(std::min(bytes[1], PP::GCODE_LINE_MAX_BYTES + 8), '\0');
            in.seekg(std::streamoff(bytes[0]));
            in.read(text.data(), std::streamsize(text.size()));
            text.resize(size_t(std::max<std::streamsize>(0, in.gcount()))); // a longer line is cut by clip_gcode_line
            lines.push_back({{"line", id}, {"text", PP::clip_gcode_line(text)}, {"current", id == line_id}});
            in.clear();
        }
    }
    if (lines.empty())
        out["gcodeLinesNote"] = "the G-code file of this slice is not available";
    out["gcodeLines"] = lines;
    return std::nullopt;
}

InvokeResult run_get_move(InvokeContext& ctx)
{
    Plater*     plater  = ctx.plater();
    GLCanvas3D* preview = plater->get_preview_canvas3D();
    if (!preview_has_gcode(plater, preview))
        return preview_not_loaded();
    GCodeViewer& gv = preview->get_gcode_viewer();
    json         out;
    if (!sync_sliders(preview))
        out["note"] = "the preview has not drawn its last slider change yet; this is the move before it";
    bool         seam = false;
    const size_t id   = described_vertex_id(gv, seam);
    out["move"]       = vertex_json(gv, id);
    if (seam)
        out["move"]["atSeam"] = true; // the window shows the last non-seam move at a seam
    out["slider"]   = move_slider_json(gv);
    out["viewType"] = view_type_id(gv.get_view_type());
    out["timeMode"] = time_mode_id(gv.get_time_mode());
    out["plate"]    = plater->get_partplate_list().get_curr_plate_index();
    if (ctx.params.value("gcodeLines", false)) {
        PartPlate*                  plate  = plater->get_partplate_list().get_curr_plate();
        const GCodeProcessorResult* result = plate != nullptr && plate->is_slice_result_valid() ? plate->get_slice_result() : nullptr;
        if (result == nullptr)
            return InvokeResult::error(WAB_ACTION_DISABLED, "the plate's slice result is out of date; slice it again",
                                       {{"reason", "not_sliced"}});
        if (auto bad = gcode_lines_json(ctx, *result, gv.get_gcode_vertex_at(id).gcode_id, size_t(ctx.params.value("before", 5)),
                                        size_t(ctx.params.value("after", 5)), out))
            return *bad;
    }
    return InvokeResult::ok(out);
}

// ---------------------------------------------------------------------------------------------
// view.capture
// ---------------------------------------------------------------------------------------------

// PNG-encodes `img` (RGBA8, bottom-up rows as GL reads them), halving it until it fits the size
// cap. False when encoding fails.
bool encode_png(ThumbnailData img, json& out)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto png = GCodeThumbnails::compress_thumbnail(img, GCodeThumbnailsFormat::PNG);
        if (!png || png->data == nullptr || png->size == 0)
            return false;
        if (png->size <= PP::CAPTURE_MAX_BYTES || img.width <= PP::CAPTURE_MIN_SIDE || img.height <= PP::CAPTURE_MIN_SIDE) {
            std::string  encoded(boost::beast::detail::base64::encoded_size(png->size), char(0));
            const size_t written = boost::beast::detail::base64::encode(encoded.data(), png->data, png->size);
            encoded.resize(written);
            out = {{"mime", "image/png"}, {"width", img.width}, {"height", img.height}, {"sizeBytes", png->size}, {"base64", encoded}};
            return true;
        }
        const unsigned w = std::max(1u, img.width / 2), h = std::max(1u, img.height / 2);
        ThumbnailData  smaller;
        smaller.set(w, h);
        smaller.pixels = PP::downscale_rgba(img.pixels, img.width, img.height, w, h);
        img            = std::move(smaller);
    }
    return false;
}

InvokeResult capture_prepare(InvokeContext& ctx, int plate_idx, unsigned w, unsigned h, const CameraName& cam)
{
    Plater*     plater = ctx.plater();
    GLCanvas3D* canvas = plater->get_view3D_canvas3D();
    if (canvas == nullptr || !canvas->make_current_for_postinit() || !wxGetApp().init_opengl())
        return InvokeResult::error(WAB_ACTION_DISABLED, "the 3D view is not ready yet", {{"reason", "gl_not_ready"}});
    const bool       show_bed    = ctx.params.value("showBed", true);
    const bool       transparent = ctx.params.value("transparent", false);
    ThumbnailsParams params{{}, false, true, show_bed, transparent, plate_idx};
    const Camera::EType proj = ctx.params.value("projection", std::string("ortho")) == "perspective" ? Camera::EType::Perspective
                                                                                                       : Camera::EType::Ortho;
    ThumbnailData data;
    canvas->render_thumbnail(data, w, h, params, proj, cam.angle);
    if (!data.is_valid())
        return InvokeResult::error(WAB_INTERNAL, "the plate could not be rendered");
    json png;
    if (!encode_png(std::move(data), png))
        return InvokeResult::error(WAB_INTERNAL, "the picture could not be encoded");
    png["canvas"] = "prepare";
    png["plate"]  = plate_idx;
    png["camera"] = cam.id;
    return InvokeResult::ok(png);
}

InvokeResult capture_preview(InvokeContext& ctx, unsigned max_side, const CameraName* cam)
{
    Plater*     plater = ctx.plater();
    GLCanvas3D* canvas = plater->get_preview_canvas3D();
    // is_preview_shown() is only the plater's own panel choice: it stays true while the Home page or
    // a web tab covers the plater, and GL_FRONT of a covered canvas is not what anyone sees.
    wxWindow* widget = canvas != nullptr ? canvas->get_wxglcanvas() : nullptr;
    if (!plater->is_preview_shown() || widget == nullptr || !widget->IsShownOnScreen())
        return InvokeResult::error(WAB_ACTION_DISABLED,
                                   "the Preview tab is not on screen; show it first (ui.showTab {tab:\"preview\"} or "
                                   "preview.setView {show:\"preview\"})",
                                   {{"reason", "preview_not_shown"}});
    if (cam != nullptr)
        plater->select_view(cam->direction); // as View > <direction>: the person's preview camera moves too

    // Draw one frame now and read back what the person sees.
    canvas->set_as_dirty();
    canvas->render();
    if (!canvas->make_current_for_postinit())
        return InvokeResult::error(WAB_ACTION_DISABLED, "the preview is not ready yet", {{"reason", "gl_not_ready"}});
    const Size     size = canvas->get_canvas_size();
    const unsigned sw   = unsigned(std::max(0, size.get_width()));
    const unsigned sh   = unsigned(std::max(0, size.get_height()));
    if (sw < 8 || sh < 8)
        return InvokeResult::error(WAB_ACTION_DISABLED, "the preview canvas is too small to capture", {{"reason", "canvas_hidden"}});
    std::vector<unsigned char> frame(size_t(sw) * sh * 4, 0);
    ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
    ::glReadBuffer(GL_FRONT);
    ::glReadPixels(0, 0, GLsizei(sw), GLsizei(sh), GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
    ::glReadBuffer(GL_BACK);
    for (size_t i = 3; i < frame.size(); i += 4)
        frame[i] = 255; // the window's alpha is meaningless

    const auto    fit = PP::fit_capture_size(sw, sh, max_side);
    ThumbnailData img;
    img.set(fit[0], fit[1]);
    img.pixels = (fit[0] == sw && fit[1] == sh) ? frame : PP::downscale_rgba(frame, sw, sh, fit[0], fit[1]);
    json png;
    if (!encode_png(std::move(img), png))
        return InvokeResult::error(WAB_INTERNAL, "the picture could not be encoded");
    png["canvas"] = "preview";
    png["plate"]  = plater->get_partplate_list().get_curr_plate_index();
    png["camera"] = cam != nullptr ? json(cam->id) : json("current");
    json v        = view_state();
    png["view"]   = v;
    return InvokeResult::ok(png);
}

InvokeResult run_capture(InvokeContext& ctx)
{
    const std::string canvas = ctx.params.value("canvas", std::string("prepare"));
    const std::string cam_id = ctx.params.value("camera", std::string());
    const CameraName* cam    = cam_id.empty() ? nullptr : find_camera(cam_id);
    if (!cam_id.empty() && cam == nullptr)
        return InvokeResult::error(WAB_BAD_PARAMS, "unknown camera", {{"path", "/params/camera"}});
    const unsigned w = unsigned(ctx.params.value("width", 512));
    const unsigned h = unsigned(ctx.params.value("height", 512));

    if (canvas == "preview") {
        // The preview picture is the canvas as drawn: its projection, bed and background are the person's.
        for (const char* key : {"projection", "showBed", "transparent"})
            if (ctx.params.contains(key))
                return InvokeResult::error(WAB_BAD_PARAMS, std::string(key) + " applies to canvas \"prepare\" only",
                                           {{"path", std::string("/params/") + key}});
        if (ctx.targets.plate >= 0 && ctx.targets.plate != ctx.plater()->get_partplate_list().get_curr_plate_index())
            return InvokeResult::error(WAB_BAD_PARAMS, "the preview shows the current plate; select the plate first (plate.select)",
                                       {{"path", "/target/plate"}});
        return capture_preview(ctx, std::max(w, h), cam);
    }
    const int idx = target_plate(ctx);
    if (idx < 0)
        return no_plate();
    return capture_prepare(ctx, idx, w, h, cam != nullptr ? *cam : camera_names().front());
}

} // namespace

void register_preview_actions(Catalog& c)
{
    PP::add_bound(c, PP::preview_descriptors(),
                  {{"preview.getStats", {run_get_stats, {}}},
                   {"preview.setView", {run_set_view, {}}},
                   {"preview.getMove", {run_get_move, {}}},
                   {"view.capture", {run_capture, {}}}});
}

}}} // namespace Slic3r::GUI::Actions
