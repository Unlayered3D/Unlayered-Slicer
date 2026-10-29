// Automation actions: paint.apply / paint.clear / paint.stats (FacetSelectors).
// Owner: work package S6 (paint). See docs/bridge-actions/paint.md.
//
// Painting is driven by geometric selectors instead of a brush: libslic3r/FacetSelectors.hpp
// evaluates them per original facet in the world frame and writes the volume's FacetsAnnotation
// through the same TriangleSelector serialization the painter gizmos use. Whole facets are
// painted (v1); brush-subdivided facets that end up uniform are merged back.

#include "PaintActions.hpp"

#include <algorithm>
#include <cmath>

#include "libslic3r/FacetSelectors.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "Register.hpp"
#include "../WebAppBridge.hpp"

#include "../GUI_App.hpp"
#include "../GUI_ObjectList.hpp"
#include "../GLCanvas3D.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;
namespace FS = Slic3r::FacetSelectors;

namespace {

json vec3_schema(const char* description)
{
    return {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}, {"description", description}};
}

json test_schema()
{
    return {{"type", "string"},
            {"enum", {"centroid", "all", "any"}},
            {"description", "Which points of a facet must be inside: its centroid (default), all corners, or any corner."}};
}

json obj(json properties, json required = json::array(), const char* description = nullptr)
{
    json o = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        o["required"] = std::move(required);
    if (description)
        o["description"] = description;
    return o;
}

json side_enum(const char* description)
{
    return {{"type", "string"}, {"enum", {"rear", "front", "left", "right", "top", "bottom"}}, {"description", description}};
}

json number_schema(double lo, double hi, const char* description, const json& def = json())
{
    json s = {{"type", "number"}, {"minimum", lo}, {"maximum", hi}, {"description", description}};
    if (!def.is_null())
        s["default"] = def;
    return s;
}

// The leaf selectors, as properties of one selector object (exactly one present).
json leaf_properties()
{
    json p = json::object();
    p["overhang"] = obj({{"maxAngleDeg", number_schema(0, 90, "Facets facing down within this angle of straight down (the gizmo's overhang "
                                                              "angle). Support where the overhang exceeds 50 degrees from vertical = 40.")}},
                        {"maxAngleDeg"}, "Downward-facing facets.");
    json normal = obj({{"side", side_enum("Named direction: rear=+Y, front=-Y, right=+X, left=-X, top=+Z, bottom=-Z.")},
                       {"direction", vec3_schema("Any direction [x,y,z] in plate coordinates.")},
                       {"withinDeg", number_schema(0, 180, "Largest angle between the facet normal and the direction.", 45)}},
                      json::array(), "Facets facing a direction.");
    normal["x-exactlyOne"] = json::array({json::array({"side"}), json::array({"direction"})});
    p["normal"] = std::move(normal);
    p["zRange"] = {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                   {"description", "[zMin, zMax] in mm above the plate; a facet is in when its centroid is."}};
    p["box"] = obj({{"min", vec3_schema("Lower corner.")}, {"max", vec3_schema("Upper corner.")}, {"test", test_schema()}},
                   {"min", "max"}, "Facets inside an axis-aligned box (plate coordinates, mm).");
    p["sphere"] = obj({{"center", vec3_schema("Centre.")}, {"radius", number_schema(0, 1e6, "Radius in mm.")}, {"test", test_schema()}},
                      {"center", "radius"}, "Facets inside a sphere.");
    p["halfspace"] = obj({{"point", vec3_schema("A point on the plane.")},
                          {"normal", vec3_schema("Facets on the side this normal points to are selected.")},
                          {"test", test_schema()}},
                         {"point", "normal"}, "Facets on one side of a plane.");
    p["facets"] = obj({{"volume", {{"type", "integer"}, {"minimum", 1}, {"description", "Volume id from getModelTree."}}},
                       {"volumeIndex", {{"type", "integer"}, {"minimum", 0}, {"description", "Index of the volume in its object."}}},
                       {"indices", {{"type", "array"}, {"maxItems", 200000}, {"items", {{"type", "integer"}, {"minimum", 0}}},
                                    {"description", "Original facet indices (e.g. from geometry.query planarRegions)."}}}},
                      {"indices"}, "Explicit facets.");
    json planar = obj({{"seedSide", side_enum("Start from the outermost facet facing this side.")},
                       {"seedPoint", vec3_schema("Start from the facet closest to this point.")},
                       {"toleranceDeg", number_schema(0, 90, "Largest angle from the seed facet's normal.", 5)}},
                      json::array(), "A flat (or nearly flat) face grown from a seed facet.");
    planar["x-exactlyOne"] = json::array({json::array({"seedSide"}), json::array({"seedPoint"})});
    p["planarRegion"] = std::move(planar);
    json smooth = obj({{"seedSide", side_enum("Start from the outermost facet facing this side.")},
                       {"seedPoint", vec3_schema("Start from the facet closest to this point.")},
                       {"maxNeighbourAngleDeg", number_schema(0, 90, "Largest angle between neighbouring facets (the tool's smart fill angle).", 30)}},
                      json::array(), "The painting tools' Smart fill: a surface grown from a seed while neighbouring facets bend less "
                                     "than the angle, so a curved wall fills as one region.");
    smooth["x-exactlyOne"] = json::array({json::array({"seedSide"}), json::array({"seedPoint"})});
    p["smoothRegion"] = std::move(smooth);
    return p;
}

json leaf_schema(const json& props, const char* description)
{
    json s      = obj(props, json::array(), description);
    json groups = json::array();
    for (auto it = props.begin(); it != props.end(); ++it)
        groups.push_back(json::array({it.key()}));
    s["x-exactlyOne"] = std::move(groups);
    return s;
}

json layer_enum(bool with_all)
{
    json e = json::array({"support", "seam", "fuzzy", "color"});
    if (with_all)
        e.push_back("all");
    return e;
}

json volumes_schema()
{
    return {{"type", "array"}, {"maxItems", 256}, {"items", {{"type", "integer"}, {"minimum", 1}}},
            {"description", "Only these volume ids (parts) of the objects; default every part."}};
}

double round3(double v) { return std::round(v * 1000.) / 1000.; }

json object_ref(const Model& model, int obj_idx)
{
    const ModelObject* mo = model.objects[size_t(obj_idx)];
    return {{"id", mo->id().id}, {"index", obj_idx}, {"name", mo->name}};
}

const ConfigOption* config_option(const ModelObject& mo, const DynamicPrintConfig* print, const char* key)
{
    if (const ConfigOption* o = mo.config.get().option(key))
        return o;
    return print ? print->option(key) : nullptr;
}

// Instance index and the optional volume-id filter.
void parse_scope(const json& params, size_t& instance, std::vector<size_t>& volume_ids)
{
    instance = params.contains("instance") && params["instance"].is_number_integer() && params["instance"].get<long long>() >= 0 ?
                   params["instance"].get<size_t>() :
                   0;
    volume_ids.clear();
    if (params.contains("volumes") && params["volumes"].is_array())
        for (const json& v : params["volumes"])
            if (v.is_number_integer() && v.get<long long>() > 0)
                volume_ids.push_back(v.get<size_t>());
}

std::vector<FS::PaintLayer> layers_of(const json& params)
{
    FS::PaintLayer    one;
    const std::string s = params.contains("layer") && params["layer"].is_string() ? params["layer"].get<std::string>() : "all";
    if (s != "all" && FS::parse_paint_layer(s, one))
        return {one};
    return {FS::PaintLayer::Support, FS::PaintLayer::Seam, FS::PaintLayer::Fuzzy, FS::PaintLayer::Color};
}

bool volume_in_scope(const ModelVolume& mv, const std::vector<size_t>& ids)
{
    return mv.is_model_part() && (ids.empty() || std::find(ids.begin(), ids.end(), mv.id().id) != ids.end());
}

} // namespace

// ============================================================================= schemas

json selector_schema()
{
    const json leaf0 = leaf_properties();
    json       leaf1 = leaf0;
    leaf1["not"]     = leaf_schema(leaf0, "Facets the inner selector does NOT pick.");
    json root        = leaf1;
    root["all"]      = {{"type", "array"}, {"maxItems", 16}, {"items", leaf_schema(leaf1, "One condition.")},
                        {"description", "Facets every listed selector picks. [] = every facet."}};
    root["any"]      = {{"type", "array"}, {"maxItems", 16}, {"items", leaf_schema(leaf1, "One alternative.")},
                        {"description", "Facets at least one listed selector picks."}};
    return leaf_schema(root, "Which facets: one selector (combine with all / any / not). Coordinates are plate (world) mm.");
}

json paint_apply_params_schema()
{
    return obj(
        {{"layer", {{"type", "string"}, {"enum", layer_enum(false)},
                    {"description", "Paint layer: support enforcers/blockers, seam enforcers/blockers, fuzzy skin, or filament colour."}}},
         {"state", {{"type", "string"}, {"enum", {"enforce", "block", "erase", "filament"}},
                    {"description", "enforce / block (support, seam), enforce (fuzzy), filament (color, with `filament`), erase (any)."}}},
         {"filament", {{"type", "integer"}, {"minimum", 1}, {"maximum", 32}, {"description", "1-based filament slot, for state filament."}}},
         {"mode", {{"type", "string"}, {"enum", {"add", "replace", "clear"}}, {"default", "add"},
                   {"description", "add: paint the matched facets. replace: remove this state everywhere first. clear: erase the "
                                   "matched facets whatever their state."}}},
         {"where", selector_schema()},
         {"volumes", volumes_schema()},
         {"instance", {{"type", "integer"}, {"minimum", 0}, {"default", 0}, {"description", "Instance whose placement defines the world frame."}}},
         {"dryRun", {{"type", "boolean"}, {"default", false}, {"description", "Report what would be painted without changing anything."}}}},
        {"layer", "state", "where"});
}

json paint_clear_params_schema()
{
    return obj({{"layer", {{"type", "string"}, {"enum", layer_enum(true)}, {"default", "all"}}}, {"volumes", volumes_schema()}});
}

json paint_remap_params_schema()
{
    json pair = obj({{"from", {{"type", "integer"}, {"minimum", 1}, {"maximum", 32}}}, {"to", {{"type", "integer"}, {"minimum", 1}, {"maximum", 32}}}},
                    {"from", "to"});
    return obj({{"pairs", {{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", pair},
                           {"description", "Painted filament `from` becomes `to` (1-based). Pairs apply at once, so swaps work."}}},
                {"volumes", volumes_schema()}},
               {"pairs"});
}

json paint_stats_params_schema()
{
    return obj({{"layer", {{"type", "string"}, {"enum", layer_enum(true)}, {"default", "all"}}},
                {"instance", {{"type", "integer"}, {"minimum", 0}, {"default", 0}}}});
}

// ============================================================================= cores

InvokeResult paint_apply_model(Model& model, const std::vector<int>& objects, const json& params, const std::function<void()>& before_write,
                               std::vector<int>& changed, const PaintHintContext& hints)
{
    changed.clear();
    FS::PaintLayer layer;
    if (!params.contains("layer") || !params["layer"].is_string() || !FS::parse_paint_layer(params["layer"].get<std::string>(), layer))
        return InvokeResult::error(WAB_BAD_PARAMS, "layer must be support, seam, fuzzy or color", {{"path", "/params/layer"}});

    // State: the wire form is {state, filament}; FacetSelectors takes "enforce" | {"filament": n}.
    const std::string state_s = params.contains("state") && params["state"].is_string() ? params["state"].get<std::string>() : "";
    json              state_j = state_s;
    if (state_s == "filament") {
        if (!params.contains("filament"))
            return InvokeResult::error(WAB_BAD_PARAMS, "state filament needs `filament`", {{"path", "/params/filament"}, {"needs", "filament"}});
        state_j = {{"filament", params["filament"]}};
    }
    EnforcerBlockerType state;
    std::string         state_err;
    if (!FS::parse_paint_state(state_j, layer, state, state_err))
        return InvokeResult::error(WAB_BAD_PARAMS, state_err, {{"path", "/params/state"}});

    FS::PaintMode     mode   = FS::PaintMode::Add;
    const std::string mode_s = params.contains("mode") && params["mode"].is_string() ? params["mode"].get<std::string>() : "add";
    if (!FS::parse_paint_mode(mode_s, mode))
        return InvokeResult::error(WAB_BAD_PARAMS, "mode must be add, replace or clear", {{"path", "/params/mode"}});

    FS::ParseError  perr;
    FS::SelectorPtr selector = FS::parse_selector(params.contains("where") ? params["where"] : json(), &perr, "params.where");
    if (!selector) {
        // "params.where.all[0].normal.side" -> "/params/where/all/0/normal/side" (the schema validator's form).
        std::string path = "/";
        for (char ch : perr.path)
            if (ch == '.' || ch == '[')
                path += '/';
            else if (ch != ']')
                path += ch;
        return InvokeResult::error(WAB_BAD_PARAMS, perr.message, {{"path", path}});
    }

    size_t              instance = 0;
    std::vector<size_t> volume_ids;
    parse_scope(params, instance, volume_ids);
    const bool dry_run = params.contains("dryRun") && params["dryRun"].is_boolean() && params["dryRun"].get<bool>();

    // Pass 1: evaluate everything as a dry run, so a bad target fails before any write and the undo
    // snapshot is taken only when something will change.
    struct Job
    {
        int                          obj;
        FS::EvalContext              ctx;
        std::vector<FS::PaintResult> dry;
    };
    std::vector<Job> jobs;
    size_t           total_hits  = 0;
    bool             will_change = false;
    for (int oi : objects) {
        if (oi < 0 || size_t(oi) >= model.objects.size())
            return InvokeResult::error(WAB_NOT_FOUND, "object not found", {{"missing", oi}});
        ModelObject* mo = model.objects[size_t(oi)];
        if (instance >= mo->instances.size())
            return InvokeResult::error(WAB_BAD_PARAMS, "instance out of range", {{"path", "/params/instance"}});
        Job job{oi, FS::object_geometry(*mo, instance, true, volume_ids), {}};
        for (size_t v = 0; v < job.ctx.volumes.size(); ++v) {
            ModelVolume* mv = mo->volumes[size_t(job.ctx.volumes[v].volume_index())];
            job.dry.push_back(FS::paint_volume(*mv, job.ctx, v, layer, state, mode, *selector, true));
            total_hits += job.dry.back().hits;
            will_change |= job.dry.back().would_change;
        }
        jobs.push_back(std::move(job));
    }

    if (dry_run)
        will_change = false;
    if (will_change && before_write)
        before_write();

    json   out_objects = json::array();
    double hit_area    = 0.;
    for (Job& job : jobs) {
        ModelObject* mo          = model.objects[size_t(job.obj)];
        json         vols        = json::array();
        bool         obj_changed = false;
        for (size_t v = 0; v < job.ctx.volumes.size(); ++v) {
            const int       vi = job.ctx.volumes[v].volume_index();
            ModelVolume*    mv = mo->volumes[size_t(vi)];
            FS::PaintResult r  = will_change && job.dry[v].would_change ?
                                     FS::paint_volume(*mv, job.ctx, v, layer, state, mode, *selector, false) :
                                     job.dry[v];
            obj_changed |= r.changed;
            hit_area += r.hit_area_mm2;
            json vj = {{"id", mv->id().id}, {"index", vi}, {"name", mv->name}, {"hits", r.hits}, {"hitAreaMm2", round3(r.hit_area_mm2)},
                       {"changed", r.changed}, {"states", FS::stats_to_json(layer, r.states)}};
            if (dry_run)
                vj["wouldChange"] = r.would_change;
            vols.push_back(std::move(vj));
        }
        if (obj_changed)
            changed.push_back(job.obj);
        json o       = object_ref(model, job.obj);
        o["volumes"] = std::move(vols);
        if (job.ctx.volumes.empty())
            o["note"] = "no printable parts in scope (modifiers and negative volumes are not painted)";
        out_objects.push_back(std::move(o));
    }

    // Hints: why the paint might not show in the print.
    json hint_list = json::array();
    if (total_hits == 0 && mode != FS::PaintMode::Replace)
        hint_list.push_back("The selector matched no facets. geometry.query (sides, overhangs, planarRegions, bbox) shows what is there.");
    for (const Job& job : jobs) {
        const ModelObject& mo = *model.objects[size_t(job.obj)];
        if (layer == FS::PaintLayer::Support && state == EnforcerBlockerType::ENFORCER) {
            const ConfigOption* en = config_option(mo, hints.print_config, "enable_support");
            const ConfigOption* st = config_option(mo, hints.print_config, "support_type");
            if (en && !en->getBool())
                hint_list.push_back("Supports are off for '" + mo.name + "' (enable_support); painted enforcers generate support only when it is on.");
            else if (en && st && is_auto(SupportType(st->getInt())))
                hint_list.push_back("support_type is automatic for '" + mo.name +
                                    "': support is also generated outside the painted areas. A manual support type gives painted-only support.");
        }
        if (layer == FS::PaintLayer::Fuzzy && state != EnforcerBlockerType::NONE) {
            const ConfigOption* fz = config_option(mo, hints.print_config, "fuzzy_skin");
            if (fz && FuzzySkinType(fz->getInt()) == FuzzySkinType::Disabled_fuzzy)
                hint_list.push_back("fuzzy_skin is set to disable painted fuzzy skin for '" + mo.name + "'.");
        }
    }
    if (layer == FS::PaintLayer::Color && state != EnforcerBlockerType::NONE && hints.filament_count > 0) {
        if (int(state) > hints.filament_count)
            hint_list.push_back("Filament " + std::to_string(int(state)) + " does not exist: the project has " +
                                std::to_string(hints.filament_count) + " filament slot(s). Add slots first.");
        else if (hints.filament_count < 2)
            hint_list.push_back("Colour painting needs at least two filament slots to make a difference.");
    }

    InvokeResult r = InvokeResult::ok({{"layer", FS::paint_layer_name(layer)},
                                       {"mode", mode_s},
                                       {"dryRun", dry_run},
                                       {"changed", !changed.empty()},
                                       {"undoable", true},
                                       {"hits", total_hits},
                                       {"hitAreaMm2", round3(hit_area)},
                                       {"objects", std::move(out_objects)},
                                       {"hints", std::move(hint_list)}});
    if (changed.empty())
        r.with_effects({});
    return r;
}

InvokeResult paint_clear_model(Model& model, const std::vector<int>& objects, const json& params, const std::function<void()>& before_write,
                               std::vector<int>& changed)
{
    changed.clear();
    const std::vector<FS::PaintLayer> layers = layers_of(params);
    size_t                            inst   = 0;
    std::vector<size_t>               ids;
    parse_scope(params, inst, ids);

    bool any = false;
    for (int oi : objects) {
        if (oi < 0 || size_t(oi) >= model.objects.size())
            return InvokeResult::error(WAB_NOT_FOUND, "object not found", {{"missing", oi}});
        for (const ModelVolume* mv : model.objects[size_t(oi)]->volumes)
            if (volume_in_scope(*mv, ids))
                for (FS::PaintLayer l : layers)
                    any |= !FS::annotation(*mv, l).empty();
    }
    json cleared = json::array();
    if (any) {
        if (before_write)
            before_write();
        for (int oi : objects) {
            bool obj_changed = false;
            for (ModelVolume* mv : model.objects[size_t(oi)]->volumes) {
                if (!volume_in_scope(*mv, ids))
                    continue;
                for (FS::PaintLayer l : layers) {
                    FacetsAnnotation& a = FS::annotation(*mv, l);
                    if (a.empty())
                        continue;
                    a.reset();
                    obj_changed = true;
                    cleared.push_back({{"object", model.objects[size_t(oi)]->id().id}, {"volume", mv->id().id}, {"layer", FS::paint_layer_name(l)}});
                }
            }
            if (obj_changed)
                changed.push_back(oi);
        }
    }
    InvokeResult r = InvokeResult::ok({{"changed", any}, {"undoable", true}, {"cleared", std::move(cleared)}});
    if (!any)
        r.with_effects({});
    return r;
}

InvokeResult paint_remap_model(Model& model, const std::vector<int>& objects, const json& params, const std::function<void()>& before_write,
                               std::vector<int>& changed)
{
    changed.clear();
    EnforcerBlockerStateMap map;
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = EnforcerBlockerType(i);
    if (!params.contains("pairs") || !params["pairs"].is_array() || params["pairs"].empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "pairs is required", {{"path", "/params/pairs"}, {"needs", "pairs"}});
    for (size_t i = 0; i < params["pairs"].size(); ++i) {
        const json& p = params["pairs"][i];
        auto ok = [](const json& v) { return v.is_number_integer() && v.get<long long>() >= 1 && v.get<long long>() <= (long long) EnforcerBlockerType::ExtruderMax; };
        if (!p.is_object() || !p.contains("from") || !p.contains("to") || !ok(p["from"]) || !ok(p["to"]))
            return InvokeResult::error(WAB_BAD_PARAMS, "each pair needs from and to between 1 and 32", {{"path", "/params/pairs/" + std::to_string(i)}});
        map[p["from"].get<size_t>()] = EnforcerBlockerType(p["to"].get<int>());
    }
    size_t              inst = 0;
    std::vector<size_t> ids;
    parse_scope(params, inst, ids);

    // Work on copies first so the snapshot is taken only when something changes.
    struct Change { int obj; ModelVolume* mv; TriangleSelector::TriangleSplittingData data; };
    std::vector<Change> changes;
    for (int oi : objects) {
        if (oi < 0 || size_t(oi) >= model.objects.size())
            return InvokeResult::error(WAB_NOT_FOUND, "object not found", {{"missing", oi}});
        for (ModelVolume* mv : model.objects[size_t(oi)]->volumes) {
            if (!volume_in_scope(*mv, ids) || mv->mmu_segmentation_facets.empty())
                continue;
            TriangleSelector sel(mv->mesh());
            sel.deserialize(mv->mmu_segmentation_facets.get_data(), false);
            sel.remap_triangle_state(map);
            TriangleSelector::TriangleSplittingData data = sel.serialize();
            if (data != mv->mmu_segmentation_facets.get_data())
                changes.push_back({oi, mv, std::move(data)});
        }
    }
    if (!changes.empty() && before_write)
        before_write();
    for (Change& c : changes) {
        c.mv->mmu_segmentation_facets.set_data(std::move(c.data));
        if (std::find(changed.begin(), changed.end(), c.obj) == changed.end())
            changed.push_back(c.obj);
    }
    InvokeResult r = InvokeResult::ok({{"changed", !changes.empty()}, {"undoable", true}, {"volumesChanged", changes.size()}});
    if (changes.empty())
        r.with_effects({});
    return r;
}

InvokeResult paint_stats_model(const Model& model, const std::vector<int>& objects, const json& params)
{
    const std::vector<FS::PaintLayer> layers = layers_of(params);
    size_t                            inst   = 0;
    std::vector<size_t>               ids;
    parse_scope(params, inst, ids);
    json out = json::array();
    for (int oi : objects) {
        if (oi < 0 || size_t(oi) >= model.objects.size())
            return InvokeResult::error(WAB_NOT_FOUND, "object not found", {{"missing", oi}});
        const ModelObject* mo = model.objects[size_t(oi)];
        if (inst >= mo->instances.size())
            return InvokeResult::error(WAB_BAD_PARAMS, "instance out of range", {{"path", "/params/instance"}});
        const Transform3d& im   = mo->instances[inst]->get_matrix();
        json               vols = json::array();
        for (size_t vi = 0; vi < mo->volumes.size(); ++vi) {
            const ModelVolume* mv = mo->volumes[vi];
            if (!mv->is_model_part())
                continue;
            json per_layer = json::object();
            for (FS::PaintLayer l : layers)
                per_layer[FS::paint_layer_name(l)] =
                    FS::stats_to_json(l, FS::annotation_stats(mv->mesh(), im * mv->get_matrix(), FS::annotation(*mv, l).get_data()));
            vols.push_back({{"id", mv->id().id},
                            {"index", vi},
                            {"name", mv->name},
                            {"facets", mv->mesh().its.indices.size()},
                            {"layers", std::move(per_layer)}});
        }
        json o       = object_ref(model, oi);
        o["volumes"] = std::move(vols);
        out.push_back(std::move(o));
    }
    return InvokeResult::ok({{"objects", std::move(out)}});
}

// ============================================================================= GUI glue

namespace {

// What the painter gizmos do after writing (GLGizmoMmuSegmentation::update_model_object).
void notify_painted(const std::vector<int>& changed, bool color)
{
    Plater* plater = wxGetApp().plater();
    if (changed.empty() || !plater)
        return;
    for (int oi : changed) {
        wxGetApp().obj_list()->update_info_items(size_t(oi));
        if (color)
            plater->get_partplate_list().notify_instance_update(oi, 0);
    }
    if (GLCanvas3D* canvas = plater->get_view3D_canvas3D()) {
        canvas->post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        canvas->set_as_dirty();
    }
}

PaintHintContext gui_hints()
{
    PaintHintContext h;
    if (PresetBundle* pb = wxGetApp().preset_bundle) {
        h.print_config   = &pb->prints.get_edited_preset().config;
        h.filament_count = int(pb->filament_presets.size());
    }
    return h;
}

std::string needs_objects(const InvokeContext&)
{
    if (!wxGetApp().plater() || wxGetApp().model().objects.empty())
        return "the plate is empty";
    return {};
}

json first_object() { return {{"objects", json::array({{{"index", 0}}})}}; }

json example(json params) { return {{"target", first_object()}, {"params", std::move(params)}}; }

} // namespace

void register_paint_actions(Catalog& c)
{
    {
        ActionDescriptor d;
        d.id      = "paint.apply";
        d.label   = "Paint";
        d.summary = "Paint support enforcers/blockers, seam enforcers/blockers, fuzzy skin or filament colour onto facets chosen by "
                    "geometry (overhang angle, facing side, height range, box, plane, flat face). Undoable.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "objects"}, {"required", true}};
        d.params  = paint_apply_params_schema();
        d.effects = {"paint"};
        d.ui      = {"GLGizmosManager.cpp::gizmo::GLGizmoFdmSupports", "GLGizmosManager.cpp::gizmo::GLGizmoSeam",
                     "GLGizmosManager.cpp::gizmo::GLGizmoFuzzySkin",   "GLGizmosManager.cpp::gizmo::GLGizmoMmuSegmentation"};
        d.examples = json::array(
            {example({{"layer", "seam"},
                      {"state", "enforce"},
                      {"where", {{"all", json::array({{{"normal", {{"side", "rear"}, {"withinDeg", 60}}}},
                                                      {{"not", {{"normal", {{"side", "top"}, {"withinDeg", 30}}}}}}})}}}}),
             example({{"layer", "support"}, {"state", "enforce"}, {"where", {{"overhang", {{"maxAngleDeg", 40}}}}}}),
             example({{"layer", "support"}, {"state", "block"}, {"where", {{"zRange", {0, 30}}}}}),
             example({{"layer", "color"}, {"state", "filament"}, {"filament", 2}, {"where", {{"planarRegion", {{"seedSide", "top"}}}}}})});
        c.add(
            std::move(d),
            [](InvokeContext& ctx) -> InvokeResult {
                std::vector<int> changed;
                InvokeResult     r = paint_apply_model(
                    ctx.model(), ctx.targets.objects, ctx.params, [&ctx]() { ctx.snapshot(); }, changed, gui_hints());
                notify_painted(changed, ctx.params.value("layer", std::string()) == "color");
                return r;
            },
            needs_objects);
    }
    {
        ActionDescriptor d;
        d.id       = "paint.clear";
        d.label    = "Erase all painting";
        d.summary  = "Erase one paint layer (support, seam, fuzzy, color) or all of them from objects or chosen parts. Undoable.";
        d.risk     = Risk::Mutate;
        d.target   = {{"kind", "objects"}, {"required", true}};
        d.params   = paint_clear_params_schema();
        d.effects  = {"paint"};
        d.ui       = {"GLGizmoFdmSupports.cpp::gizmoctl::desc:remove_all", "GLGizmoSeam.cpp::gizmoctl::desc:remove_all",
                      "GLGizmoFuzzySkin.cpp::gizmoctl::desc:remove_all", "GLGizmoMmuSegmentation.cpp::gizmoctl::desc:remove_all"};
        d.examples = json::array({example({{"layer", "seam"}})});
        c.add(
            std::move(d),
            [](InvokeContext& ctx) -> InvokeResult {
                std::vector<int>  changed;
                InvokeResult      r     = paint_clear_model(ctx.model(), ctx.targets.objects, ctx.params, [&ctx]() { ctx.snapshot(); }, changed);
                const std::string layer = ctx.params.value("layer", std::string("all"));
                notify_painted(changed, layer == "color" || layer == "all");
                return r;
            },
            needs_objects);
    }
    {
        ActionDescriptor d;
        d.id       = "paint.remapFilaments";
        d.label    = "Remap painted filaments";
        d.summary  = "Change which filament colour-painted areas use (e.g. painted filament 2 becomes 3, or swap two). Undoable.";
        d.risk     = Risk::Mutate;
        d.target   = {{"kind", "objects"}, {"required", true}};
        d.params   = paint_remap_params_schema();
        d.effects  = {"paint"};
        d.ui       = {"GLGizmoMmuSegmentation.cpp::gizmoctl::desc:remap"};
        d.examples = json::array({example({{"pairs", json::array({{{"from", 1}, {"to", 2}}, {{"from", 2}, {"to", 1}}})}})});
        c.add(
            std::move(d),
            [](InvokeContext& ctx) -> InvokeResult {
                std::vector<int> changed;
                InvokeResult     r = paint_remap_model(ctx.model(), ctx.targets.objects, ctx.params, [&ctx]() { ctx.snapshot(); }, changed);
                notify_painted(changed, true);
                return r;
            },
            needs_objects);
    }
    {
        ActionDescriptor d;
        d.id       = "paint.stats";
        d.label    = "Painting statistics";
        d.summary  = "How much of each object and part is painted, per layer and state (facets and area in mm2).";
        d.risk     = Risk::Read;
        d.target   = {{"kind", "objects"}, {"required", true}};
        d.params   = paint_stats_params_schema();
        d.examples = json::array({example(json::object())});
        c.add(
            std::move(d), [](InvokeContext& ctx) -> InvokeResult { return paint_stats_model(ctx.model(), ctx.targets.objects, ctx.params); },
            needs_objects);
    }
}

}}} // namespace Slic3r::GUI::Actions
