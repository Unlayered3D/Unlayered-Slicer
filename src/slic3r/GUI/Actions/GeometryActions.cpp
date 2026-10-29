// Automation actions: geometry.query / geometry.measure, object.brimEars.
// Owner: work package S6 (paint). See docs/bridge-actions/paint.md.
//
// Read-only geometry the automation can reason with before painting or placing things (which
// side is the back, where the overhangs are, the flat faces, the height under a point, the gap
// between two objects), all in plate (world) millimetres, from libslic3r/FacetSelectors.hpp.

#include "PaintActions.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "libslic3r/BrimEarsAuto.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/FacetSelectors.hpp"
#include "libslic3r/Measure.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "Register.hpp"
#include "../WebAppBridge.hpp"

#include "../GUI_App.hpp"
#include "../GLCanvas3D.hpp"
#include "../Plater.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;
namespace FS = Slic3r::FacetSelectors;

namespace {

json obj(json properties, json required = json::array())
{
    json o = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        o["required"] = std::move(required);
    return o;
}

json num(double lo, double hi, const char* description)
{
    return {{"type", "number"}, {"minimum", lo}, {"maximum", hi}, {"description", description}};
}

json object_ref(const Model& model, int obj_idx)
{
    const ModelObject* mo = model.objects[size_t(obj_idx)];
    return {{"id", mo->id().id}, {"index", obj_idx}, {"name", mo->name}};
}

size_t instance_of(const json& params, const char* key = "instance")
{
    return params.contains(key) && params[key].is_number_integer() && params[key].get<long long>() >= 0 ? params[key].get<size_t>() : 0;
}

std::vector<size_t> volume_ids_of(const json& params)
{
    std::vector<size_t> ids;
    if (params.contains("volumes") && params["volumes"].is_array())
        for (const json& v : params["volumes"])
            if (v.is_number_integer() && v.get<long long>() > 0)
                ids.push_back(v.get<size_t>());
    return ids;
}

std::string needs_objects(const InvokeContext&)
{
    if (!wxGetApp().plater() || wxGetApp().model().objects.empty())
        return "the plate is empty";
    return {};
}

json example(json target, json params) { return {{"target", std::move(target)}, {"params", std::move(params)}}; }
json first_object() { return {{"objects", json::array({{{"index", 0}}})}}; }

} // namespace

// ============================================================================= schemas

json geometry_query_params_schema()
{
    return obj({{"query", {{"type", "string"},
                           {"enum", {"bbox", "overhangs", "sides", "planarRegions", "heightAt", "features"}},
                           {"description", "bbox: object and part boxes. overhangs: downward-facing area and its largest regions. sides: area "
                                           "facing each side and the outermost coordinate. planarRegions: flat faces, largest first (with facet "
                                           "indices for small ones). heightAt: surfaces crossing the vertical line at x, y. features: the Measure "
                                           "tool's circles (holes, bosses: centre, diameter, axis) and straight edges (ends, length), largest first."}}},
                {"maxAngleDeg", num(0, 90, "overhangs: angle from straight down (default 45).")},
                {"withinDeg", num(0, 90, "sides: angle from each side's direction (default 45).")},
                {"toleranceDeg", num(0, 90, "planarRegions: flatness tolerance (default 5).")},
                {"minAreaMm2", num(0, 1e9, "planarRegions: smallest region reported (default 1).")},
                {"limit", {{"type", "integer"}, {"minimum", 0}, {"maximum", 200}, {"description", "Regions returned (overhangs 10, planarRegions 20, features 20 of each kind)."}}},
                {"x", num(-1e6, 1e6, "heightAt: plate x in mm.")},
                {"y", num(-1e6, 1e6, "heightAt: plate y in mm.")},
                {"volumes", {{"type", "array"}, {"maxItems", 256}, {"items", {{"type", "integer"}, {"minimum", 1}}},
                             {"description", "Only these volume ids; default every printable part."}}},
                {"includeModifiers", {{"type", "boolean"}, {"default", false}, {"description", "Also include modifiers, negative parts and support blockers/enforcers."}}},
                {"instance", {{"type", "integer"}, {"minimum", 0}, {"default", 0}}}},
               {"query"});
}

// The Measure tool's features (Measure::Measuring, what it highlights under the cursor): the circles
// and straight edges bounding each flat face, in the plate frame, largest first.
json measure_features(const FS::EvalContext& ctx, size_t limit)
{
    struct Circle { Vec3d c; double r; Vec3d n; };
    struct Edge { Vec3d a, b; };
    std::vector<Circle> circles;
    std::vector<Edge>   edges;
    size_t              planes = 0;
    for (const FS::VolumeGeometry& g : ctx.volumes) {
        indexed_triangle_set its = g.its();
        for (stl_vertex& v : its.vertices)
            v = (g.world() * v.cast<double>()).cast<float>();
        const Measure::Measuring m(its);
        planes += size_t(m.get_num_of_planes());
        for (int i = 0; i < m.get_num_of_planes(); ++i)
            for (const Measure::SurfaceFeature& f : m.get_plane_features(unsigned(i))) {
                if (f.get_type() == Measure::SurfaceFeatureType::Circle) {
                    const auto   circle = f.get_circle();
                    const Vec3d  c = std::get<0>(circle), n = std::get<2>(circle);
                    const double r = std::get<1>(circle);
                    // A hole's rim bounds the faces on both sides of it: report it once.
                    if (std::none_of(circles.begin(), circles.end(), [&](const Circle& o) {
                            return (o.c - c).norm() < 1e-3 && std::abs(o.r - r) < 1e-3; }))
                        circles.push_back({c, r, n});
                } else if (f.get_type() == Measure::SurfaceFeatureType::Edge) {
                    const Vec3d a = f.get_edge().first, b = f.get_edge().second;
                    if (std::none_of(edges.begin(), edges.end(), [&](const Edge& o) {
                            return ((o.a - a).norm() < 1e-3 && (o.b - b).norm() < 1e-3) || ((o.a - b).norm() < 1e-3 && (o.b - a).norm() < 1e-3); }))
                        edges.push_back({a, b});
                }
            }
    }
    std::sort(circles.begin(), circles.end(), [](const Circle& l, const Circle& r) { return l.r > r.r; });
    std::sort(edges.begin(), edges.end(), [](const Edge& l, const Edge& r) { return (l.b - l.a).squaredNorm() > (r.b - r.a).squaredNorm(); });
    auto v3 = [](const Vec3d& v) { return json::array({v.x(), v.y(), v.z()}); };
    json jc = json::array(), je = json::array();
    for (size_t i = 0; i < circles.size() && i < limit; ++i)
        jc.push_back({{"center", v3(circles[i].c)}, {"diameterMm", 2. * circles[i].r}, {"axis", v3(circles[i].n.normalized())}});
    for (size_t i = 0; i < edges.size() && i < limit; ++i)
        je.push_back({{"from", v3(edges[i].a)}, {"to", v3(edges[i].b)}, {"lengthMm", (edges[i].b - edges[i].a).norm()}});
    return {{"query", "features"}, {"planeCount", planes}, {"circleCount", circles.size()}, {"edgeCount", edges.size()},
            {"circles", jc}, {"edges", je}};
}

json geometry_measure_params_schema()
{
    return obj({{"instanceA", {{"type", "integer"}, {"minimum", 0}, {"default", 0}, {"description", "Instance of the first object."}}},
                {"instanceB", {{"type", "integer"}, {"minimum", 0}, {"default", 0}, {"description", "Instance of the second object."}}}});
}

json brim_ears_params_schema()
{
    return obj({{"mode", {{"type", "string"}, {"enum", {"points", "auto", "remove", "clear"}}, {"default", "points"},
                          {"description", "points: add ears at the given plate positions. auto: add ears at the sharp corners of the "
                                          "first layer's outlines and holes (the tool's Auto-generate). remove: remove the ears at the given "
                                          "points. clear: remove every ear of the object."}}},
                {"maxAngleDeg", num(0, 180, "auto: corners whose interior angle is at most this get an ear (default 125).")},
                {"detectionRadiusMm", num(0, 100, "auto: simplify the outline at this tolerance first, so small wiggles are not corners (default 1).")},
                {"toleranceMm", num(0, 100, "remove: how close to a point an ear's centre must be (default: the ear's radius).")},
                {"points", {{"type", "array"},
                            {"maxItems", 200},
                            {"items", obj({{"x", num(-1e6, 1e6, "Plate x, mm.")}, {"y", num(-1e6, 1e6, "Plate y, mm.")}}, {"x", "y"})},
                            {"description", "Ear centres on the plate, normally at sharp outer corners of the first layer."}}},
                {"radiusMm", num(0, 100, "Ear radius; 0 or absent = 8 first-layer line widths, as the gizmo uses.")},
                {"replace", {{"type", "boolean"}, {"default", false}, {"description", "Remove existing ears first."}}},
                {"instance", {{"type", "integer"}, {"minimum", 0}, {"default", 0}}}});
}

// ============================================================================= cores

InvokeResult geometry_query_model(const Model& model, const std::vector<int>& objects, const json& params)
{
    const std::string query = params.contains("query") && params["query"].is_string() ? params["query"].get<std::string>() : "";
    const size_t      inst  = instance_of(params);
    const bool        mods  = params.contains("includeModifiers") && params["includeModifiers"].is_boolean() && params["includeModifiers"].get<bool>();
    json              out   = json::array();
    for (int oi : objects) {
        if (oi < 0 || size_t(oi) >= model.objects.size())
            return InvokeResult::error(WAB_NOT_FOUND, "object not found", {{"missing", oi}});
        const ModelObject* mo = model.objects[size_t(oi)];
        if (inst >= mo->instances.size())
            return InvokeResult::error(WAB_BAD_PARAMS, "instance out of range", {{"path", "/params/instance"}});
        FS::EvalContext ctx = FS::object_geometry(*mo, inst, !mods, volume_ids_of(params));
        json            q   = query == "features" ?
                                  json(measure_features(ctx, params.contains("limit") && params["limit"].is_number_integer() ? params["limit"].get<size_t>() : 20)) :
                                  FS::geometry_query(ctx, query, params);
        if (query == "features")
            q["ok"] = true;
        if (!q.value("ok", false)) {
            std::string path = q.value("path", std::string("params"));
            std::replace(path.begin(), path.end(), '.', '/');
            return InvokeResult::error(WAB_BAD_PARAMS, q.value("error", std::string("bad query")), {{"path", "/" + path}});
        }
        q.erase("ok");
        json o     = object_ref(model, oi);
        o["result"] = std::move(q);
        out.push_back(std::move(o));
    }
    return InvokeResult::ok({{"query", query}, {"frame", "plate (world) millimetres; +Y is the rear"}, {"objects", std::move(out)}});
}

InvokeResult geometry_measure_model(const Model& model, const std::vector<int>& objects, const json& params)
{
    if (objects.size() != 2)
        return InvokeResult::error(WAB_BAD_PARAMS, "geometry.measure needs exactly two objects", {{"path", "/target/objects"}});
    const size_t ia = instance_of(params, "instanceA"), ib = instance_of(params, "instanceB");
    for (int oi : objects)
        if (oi < 0 || size_t(oi) >= model.objects.size())
            return InvokeResult::error(WAB_NOT_FOUND, "object not found", {{"missing", oi}});
    const ModelObject *a = model.objects[size_t(objects[0])], *b = model.objects[size_t(objects[1])];
    if (ia >= a->instances.size())
        return InvokeResult::error(WAB_BAD_PARAMS, "instance out of range", {{"path", "/params/instanceA"}});
    if (ib >= b->instances.size())
        return InvokeResult::error(WAB_BAD_PARAMS, "instance out of range", {{"path", "/params/instanceB"}});
    json m = FS::geometry_measure(FS::object_geometry(*a, ia), FS::object_geometry(*b, ib));
    if (!m.value("ok", false))
        return InvokeResult::error(WAB_BAD_PARAMS, m.value("error", std::string("cannot measure")), {{"path", "/target"}});
    m.erase("ok");
    m["a"] = object_ref(model, objects[0]);
    m["b"] = object_ref(model, objects[1]);
    return InvokeResult::ok(std::move(m));
}

InvokeResult brim_ears_model(Model& model, int object, const json& params, float default_radius, const std::function<void()>& before_write,
                             bool& changed)
{
    changed = false;
    if (object < 0 || size_t(object) >= model.objects.size())
        return InvokeResult::error(WAB_NOT_FOUND, "object not found", {{"missing", object}});
    ModelObject*      mo   = model.objects[size_t(object)];
    const std::string mode = params.contains("mode") && params["mode"].is_string() ? params["mode"].get<std::string>() : "points";
    const size_t      inst = instance_of(params);
    if (inst >= mo->instances.size())
        return InvokeResult::error(WAB_BAD_PARAMS, "instance out of range", {{"path", "/params/instance"}});

    if (mode == "clear") {
        if (!mo->brim_points.empty()) {
            if (before_write)
                before_write();
            mo->brim_points.clear();
            changed = true;
        }
        return InvokeResult::ok({{"changed", changed}, {"ears", 0}});
    }
    if (mode == "remove") {
        // The tool's "Remove selected": the ears near the given plate points.
        const json& list = params.contains("points") ? params["points"] : json();
        if (!list.is_array() || list.empty())
            return InvokeResult::error(WAB_BAD_PARAMS, "points is required for mode remove", {{"path", "/params/points"}, {"needs", "points"}});
        const double      tol = params.contains("toleranceMm") && params["toleranceMm"].is_number() ? params["toleranceMm"].get<double>() : -1.;
        const Transform3d m   = mo->instances[inst]->get_matrix();
        std::vector<BrimPoint> kept;
        size_t                 removed = 0;
        for (const BrimPoint& bp : mo->brim_points) {
            const Vec3d w   = m * bp.pos.cast<double>();
            bool        hit = false;
            for (const json& p : list)
                if (p.is_object() && p.contains("x") && p.contains("y") && p["x"].is_number() && p["y"].is_number()) {
                    // Default reach: the ear's own radius, i.e. a point on the ear removes it.
                    const double reach = tol >= 0. ? tol : double(bp.head_front_radius);
                    hit = hit || std::hypot(w.x() - p["x"].get<double>(), w.y() - p["y"].get<double>()) <= reach;
                }
            if (hit)
                ++removed;
            else
                kept.push_back(bp);
        }
        if (removed > 0) {
            if (before_write)
                before_write();
            mo->brim_points = std::move(kept);
            changed         = true;
        }
        return InvokeResult::ok({{"changed", changed}, {"removed", removed}, {"ears", mo->brim_points.size()}});
    }
    if (mode != "points" && mode != "auto")
        return InvokeResult::error(WAB_BAD_PARAMS, "mode must be points, auto, remove or clear", {{"path", "/params/mode"}});

    json point_list = params.contains("points") ? params["points"] : json();
    if (mode == "auto") {
        // The tool's Auto-generate: the sharp corners of the first layer's outlines and holes.
        const float max_angle = params.contains("maxAngleDeg") && params["maxAngleDeg"].is_number() ? params["maxAngleDeg"].get<float>() : 125.f;
        const float detection = params.contains("detectionRadiusMm") && params["detectionRadiusMm"].is_number() ?
                                    params["detectionRadiusMm"].get<float>() : 1.f;
        point_list = json::array();
        for (const Point& p : brim_ears_auto_points(brim_ears_first_layer(*mo, inst), detection, max_angle))
            point_list.push_back({{"x", unscaled(p.x())}, {"y", unscaled(p.y())}});
        if (point_list.empty())
            return InvokeResult::ok({{"changed", false}, {"added", 0}, {"ears", mo->brim_points.size()},
                                     {"note", "the first layer has no corner sharper than maxAngleDeg"}});
    } else if (!point_list.is_array() || point_list.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "points is required for mode points", {{"path", "/params/points"}, {"needs", "points"}});

    float radius = default_radius;
    if (params.contains("radiusMm") && params["radiusMm"].is_number() && params["radiusMm"].get<double>() > 0.)
        radius = std::clamp(float(params["radiusMm"].get<double>()), 0.1f, 100.f);
    const bool replace = params.contains("replace") && params["replace"].is_boolean() && params["replace"].get<bool>();

    // Same frame as GLGizmoBrimEars: a plate point just under the bed, mapped into object coordinates
    // through the instance transformation.
    const Transform3d      inv = mo->instances[inst]->get_matrix().inverse();
    std::vector<BrimPoint> pts = replace ? std::vector<BrimPoint>() : mo->brim_points;
    size_t                 added = 0;
    for (size_t i = 0; i < point_list.size(); ++i) {
        const json& p = point_list[i];
        if (!p.is_object() || !p.contains("x") || !p.contains("y") || !p["x"].is_number() || !p["y"].is_number())
            return InvokeResult::error(WAB_BAD_PARAMS, "each point needs numeric x and y", {{"path", "/params/points/" + std::to_string(i)}});
        const Vec3d     world(p["x"].get<double>(), p["y"].get<double>(), -0.0001);
        const BrimPoint bp((inv * world).cast<float>(), radius);
        if (std::find(pts.begin(), pts.end(), bp) == pts.end()) {
            pts.push_back(bp);
            ++added;
        }
    }
    if (pts != mo->brim_points) {
        if (before_write)
            before_write();
        mo->brim_points = std::move(pts);
        changed         = true;
    }
    return InvokeResult::ok({{"changed", changed}, {"added", added}, {"ears", mo->brim_points.size()}, {"radiusMm", radius}});
}

// ============================================================================= registration

namespace {

float gui_default_brim_radius()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (!pb)
        return 3.2f;
    const auto*  nozzles = pb->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    const double nozzle  = nozzles && !nozzles->values.empty() ? nozzles->get_at(0) : 0.4;
    const DynamicPrintConfig& print = pb->prints.get_edited_preset().config;
    const double width = print.has("initial_layer_line_width") ? print.get_abs_value("initial_layer_line_width", nozzle) : nozzle;
    return std::clamp(float((width > 0. ? width : nozzle) * 8.0), 0.1f, 100.f);
}

} // namespace

void register_geometry_actions(Catalog& c)
{
    {
        ActionDescriptor d;
        d.id      = "geometry.query";
        d.label   = "Inspect geometry";
        d.summary = "Read an object's geometry in plate millimetres: bounding boxes, overhang area and regions, area facing each side, "
                    "flat faces, the surface heights under a point, or the Measure tool's circles and edges (hole diameters, edge "
                    "lengths). Use it before painting or placing parts.";
        d.risk    = Risk::Read;
        d.target  = {{"kind", "objects"}, {"required", true}};
        d.params  = geometry_query_params_schema();
        d.ui      = {"GLGizmosManager.cpp::gizmo::GLGizmoMeasure"};
        d.examples = json::array({example(first_object(), {{"query", "sides"}}),
                                  example(first_object(), {{"query", "overhangs"}, {"maxAngleDeg", 40}}),
                                  example(first_object(), {{"query", "planarRegions"}, {"limit", 5}}),
                                  example(first_object(), {{"query", "features"}, {"limit", 10}})});
        c.add(
            std::move(d),
            [](InvokeContext& ctx) -> InvokeResult { return geometry_query_model(ctx.model(), ctx.targets.objects, ctx.params); },
            needs_objects);
    }
    {
        ActionDescriptor d;
        d.id      = "geometry.measure";
        d.label   = "Measure";
        d.summary = "Distance between two objects: closest surface distance with the two closest points, bounding-box gap per axis, "
                    "and centroid distance, in plate millimetres.";
        d.risk    = Risk::Read;
        d.target  = {{"kind", "objects"}, {"required", true}};
        d.params  = geometry_measure_params_schema();
        d.ui      = {"GLGizmoMeasure.cpp::gizmoctl::Done", "GLGizmoMeasure.cpp::gizmoctl::Cancel"};
        d.examples = json::array({example({{"objects", json::array({{{"index", 0}}, {{"index", 1}}})}}, json::object())});
        c.add(
            std::move(d),
            [](InvokeContext& ctx) -> InvokeResult { return geometry_measure_model(ctx.model(), ctx.targets.objects, ctx.params); },
            [](const InvokeContext&) -> std::string {
                if (!wxGetApp().plater() || wxGetApp().model().objects.size() < 2)
                    return "needs two objects on the plate";
                return {};
            });
    }
    {
        ActionDescriptor d;
        d.id      = "object.brimEars";
        d.label   = "Brim ears";
        d.summary = "Add brim ears at given plate positions or automatically at the first layer's sharp corners, remove the ears at given "
                    "points, or remove all of an object's ears (model.getTree lists them). Ears print "
                    "when the brim type is \"painted\" (brim_type). Undoable.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "object"}, {"required", true}};
        d.params  = brim_ears_params_schema();
        d.effects = {"paint"};
        d.ui      = {"GLGizmosManager.cpp::gizmo::GLGizmoBrimEars", "GLGizmoBrimEars.cpp::gizmoctl::desc:remove_all",
                     "GLGizmoBrimEars.cpp::gizmoctl::desc:auto_generate", "GLGizmoBrimEars.cpp::gizmoctl::desc:remove_selected"};
        d.examples = json::array({example({{"object", {{"index", 0}}}}, {{"mode", "points"}, {"points", json::array({{{"x", 100}, {"y", 100}}})}}),
                                  example({{"object", {{"index", 0}}}}, {{"mode", "auto"}}),
                                  example({{"object", {{"index", 0}}}}, {{"mode", "remove"}, {"points", json::array({{{"x", 100}, {"y", 100}}})}}),
                                  example({{"object", {{"index", 0}}}}, {{"mode", "clear"}})});
        c.add(
            std::move(d),
            [](InvokeContext& ctx) -> InvokeResult {
                if (ctx.targets.objects.size() != 1)
                    return InvokeResult::error(WAB_BAD_PARAMS, "brim ears need exactly one object", {{"path", "/target"}});
                bool         changed = false;
                const int    oi      = ctx.targets.objects.front();
                InvokeResult r = brim_ears_model(ctx.model(), oi, ctx.params, gui_default_brim_radius(), [&ctx]() { ctx.snapshot(); }, changed);
                if (!r.is_ok())
                    return r;
                // Hint when the ears will not print.
                const ModelObject&  mo = *ctx.model().objects[size_t(oi)];
                const ConfigOption* bt = mo.config.get().option("brim_type");
                if (!bt && wxGetApp().preset_bundle)
                    bt = wxGetApp().preset_bundle->prints.get_edited_preset().config.option("brim_type");
                if (bt && BrimType(bt->getInt()) != btPainted && ctx.params.value("mode", std::string("points")) == "points")
                    r.result["hints"] = json::array({"brim_type is not \"painted\" for this object, so the ears are not printed until it is."});
                if (changed) {
                    if (Plater* plater = wxGetApp().plater()) {
                        plater->set_plater_dirty(true);
                        if (GLCanvas3D* canvas = plater->get_view3D_canvas3D()) {
                            canvas->post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
                            canvas->set_as_dirty();
                        }
                    }
                } else
                    r.with_effects({});
                return r;
            },
            needs_objects);
    }
}

}}} // namespace Slic3r::GUI::Actions
