// Automation actions: layer.addRange / layer.removeRange / layer.editRange (the object list's
// Height Range Modifier) and object.layerHeightProfile (the variable layer height tool's
// Adaptive / Smooth / Reset). Settings inside a height range are object.setSettings with
// params.layerRange (ObjectSettingsActions.cpp).
// Owner: work package S4 (object settings). Docs: docs/bridge-actions/object-settings.md.

#include "ObjectSettingsRules.hpp"

#include <algorithm>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "ActionTargets.hpp"
#include "Register.hpp"

#include "../GLCanvas3D.hpp"
#include "../GUI_App.hpp"
#include "../GUI_ObjectList.hpp"
#include "../Plater.hpp"
#include "../WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;
using namespace ObjectSettings;

namespace {

bool one_object(InvokeContext& ctx, int& obj_idx, ModelObject*& mo, InvokeResult& err)
{
    if (ctx.plater() == nullptr) {
        err = InvokeResult::error(WAB_INTERNAL, "the plater is not ready");
        return false;
    }
    if (ctx.targets.objects.size() != 1) {
        err = InvokeResult::error(WAB_BAD_PARAMS, "name exactly one object", {{"path", "/target"}, {"needs", "object"}});
        return false;
    }
    obj_idx = ctx.targets.objects.front();
    Model& model = ctx.model();
    if (obj_idx < 0 || size_t(obj_idx) >= model.objects.size()) {
        err = InvokeResult::error(WAB_NOT_FOUND, "object not found");
        return false;
    }
    mo = model.objects[size_t(obj_idx)];
    return true;
}

json range_json(const t_layer_height_range& r) { return {{"zMin", r.first}, {"zMax", r.second}}; }

InvokeResult run_add_range(InvokeContext& ctx)
{
    int          obj_idx = -1;
    ModelObject* mo      = nullptr;
    InvokeResult err;
    if (!one_object(ctx, obj_idx, mo, err))
        return err;
    const double z_min = ctx.params.value("zMin", 0.);
    const double z_max = ctx.params.value("zMax", 0.);
    if (std::string why = check_range(z_min, z_max, mo->max_z(), mo->layer_config_ranges); !why.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params/zMin"}, {"layerRanges", layer_ranges_json(*mo)}});

    // The object list's default range config (ObjectList::get_default_layer_config): the object's
    // layer height and the object's filament (0).
    DynamicPrintConfig cfg;
    const double       layer_height = ctx.params.contains("layerHeight") ? ctx.params["layerHeight"].get<double>() : default_range_layer_height(*mo);
    if (std::string why = check_range_layer_height(layer_height, 0); !why.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params/layerHeight"}});
    cfg.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    cfg.set_key_value("extruder", new ConfigOptionInt(0));

    std::vector<std::string> applied, removed;
    json                     rejected = json::array();
    json                     edits    = json::object();
    if (ctx.params.contains("values"))
        edits["values"] = ctx.params["values"];
    apply_edits(cfg, Level::LayerRange, edits, 0, applied, removed, rejected);

    ctx.snapshot();
    const t_layer_height_range range{z_min, z_max};
    mo->layer_config_ranges[range].assign_config(std::move(cfg));
    refresh_after_change(obj_idx, mo, nullptr, true);

    return InvokeResult::ok({{"object", object_ref_json(ctx.model(), obj_idx)},
                             {"layerRange", range_json(range)},
                             {"overrides", overrides_json(mo->layer_config_ranges[range].get())},
                             {"applied", applied},
                             {"rejected", rejected},
                             {"layerRanges", layer_ranges_json(*mo)},
                             {"undoable", true}});
}

InvokeResult run_remove_range(InvokeContext& ctx)
{
    int          obj_idx = -1;
    ModelObject* mo      = nullptr;
    InvokeResult err;
    if (!one_object(ctx, obj_idx, mo, err))
        return err;
    auto& ranges = mo->layer_config_ranges;
    if (ranges.empty())
        return InvokeResult::error(WAB_NOT_FOUND, "the object has no height ranges");

    json removed = json::array();
    if (ctx.params.value("all", false)) {
        ctx.snapshot();
        for (const auto& entry : ranges)
            removed.push_back(range_json(entry.first));
        ranges.clear();
    } else {
        if (!ctx.params.contains("zMin") || !ctx.params.contains("zMax"))
            return InvokeResult::error(WAB_BAD_PARAMS, "give zMin and zMax, or all", {{"path", "/params/zMin"}, {"needs", "zMin"}});
        auto found = find_range(ranges, ctx.params["zMin"].get<double>(), ctx.params["zMax"].get<double>());
        if (found == ranges.end())
            return InvokeResult::error(WAB_NOT_FOUND, "the object has no height range with those bounds", {{"layerRanges", layer_ranges_json(*mo)}});
        ctx.snapshot();
        removed.push_back(range_json(found->first));
        ranges.erase(found->first);
    }
    refresh_after_change(obj_idx, mo, nullptr, true);
    return InvokeResult::ok({{"object", object_ref_json(ctx.model(), obj_idx)},
                             {"removed", removed},
                             {"layerRanges", layer_ranges_json(*mo)},
                             {"undoable", true}});
}

InvokeResult run_edit_range(InvokeContext& ctx)
{
    int          obj_idx = -1;
    ModelObject* mo      = nullptr;
    InvokeResult err;
    if (!one_object(ctx, obj_idx, mo, err))
        return err;
    auto& ranges = mo->layer_config_ranges;
    auto  found  = find_range(ranges, ctx.params.value("zMin", 0.), ctx.params.value("zMax", 0.));
    if (found == ranges.end())
        return InvokeResult::error(WAB_NOT_FOUND, "the object has no height range with those bounds", {{"layerRanges", layer_ranges_json(*mo)}});
    const t_layer_height_range old_range = found->first;
    const t_layer_height_range new_range{ctx.params.value("newZMin", 0.), ctx.params.value("newZMax", 0.)};
    if (std::string why = check_range(new_range.first, new_range.second, mo->max_z(), ranges, 0.01, &old_range); !why.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params/newZMin"}, {"layerRanges", layer_ranges_json(*mo)}});

    ctx.snapshot();
    ModelConfig config = std::move(ranges[old_range]);
    ranges.erase(old_range);
    ranges[new_range] = std::move(config);
    refresh_after_change(obj_idx, mo, nullptr, true);
    return InvokeResult::ok({{"object", object_ref_json(ctx.model(), obj_idx)},
                             {"layerRange", range_json(new_range)},
                             {"layerRanges", layer_ranges_json(*mo)},
                             {"undoable", true}});
}

InvokeResult run_layer_height_profile(InvokeContext& ctx)
{
    Plater* plater = ctx.plater();
    if (plater == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the plater is not ready");
    if (ctx.targets.objects.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "name one or more objects", {{"path", "/target"}, {"needs", "objects"}});
    const std::string mode = ctx.params.value("mode", std::string());
    Model&            model = ctx.model();
    for (int obj_idx : ctx.targets.objects)
        if (obj_idx < 0 || size_t(obj_idx) >= model.objects.size() || model.objects[size_t(obj_idx)]->instances.empty())
            return InvokeResult::error(WAB_NOT_FOUND, "object not found");

    // The same slicing parameters the variable layer height tool computes
    // (GLCanvas3D::LayersEditing::update_slicing_parameters).
    const DynamicPrintConfig full   = wxGetApp().preset_bundle->full_config();
    const Vec3d              shrink = plater->fff_print().shrinkage_compensation();
    const float              quality = float(ctx.params.value("quality", 0.5));
    const HeightProfileSmoothingParams smoothing(unsigned(std::clamp(ctx.params.value("radius", 5), 1, 10)), ctx.params.value("keepMin", false));

    ctx.snapshot();
    json results = json::array();
    for (int obj_idx : ctx.targets.objects) {
        ModelObject* mo = model.objects[size_t(obj_idx)];
        if (mode == "reset") {
            mo->layer_height_profile.clear();
        } else {
            const SlicingParameters sp = PrintObject::slicing_parameters(full, *mo, float(mo->max_z()), shrink);
            if (mode == "adaptive") {
                mo->layer_height_profile.set(layer_height_profile_adaptive(sp, *mo, quality));
            } else { // smooth: the current profile (or the one the ranges give), smoothed
                std::vector<coordf_t> profile;
                PrintObject::update_layer_height_profile(*mo, sp, profile);
                mo->layer_height_profile.set(smooth_height_profile(profile, sp, smoothing));
            }
        }
        if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
            list->update_info_items(size_t(obj_idx));
        results.push_back({{"object", object_ref_json(model, obj_idx)}, {"layerHeightProfile", layer_height_profile_json(*mo)}});
    }
    // Re-slices, and (through GLCanvas3D::set_config) drops the layer editing tool's cached profile.
    plater->schedule_background_process();
    if (GLCanvas3D* canvas = plater->canvas3D(); canvas != nullptr)
        canvas->set_as_dirty();
    return InvokeResult::ok({{"mode", mode}, {"objects", results}, {"undoable", true}});
}

std::string enabled_fff(const InvokeContext&) { return fff_only_reason(); }

json object0() { return {{"objects", json::array({{{"index", 0}}})}}; }

json number(double min, double max, const char* description)
{
    return {{"type", "number"}, {"minimum", min}, {"maximum", max}, {"description", description}};
}

} // namespace

void register_layer_height_actions(Catalog& c)
{
    {
        ActionDescriptor d;
        d.id      = "layer.addRange";
        d.label   = "Height Range Modifier";
        d.summary = "Add a height range to an object (zMin-zMax mm from its bottom) with its own layer height and settings, e.g. "
                    "more infill only in the top 10 mm. Change its settings later with object.setSettings + layerRange. Undoable.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "object"}, {"required", true}};
        d.params  = {{"type", "object"},
                     {"additionalProperties", false},
                     {"required", json::array({"zMin", "zMax"})},
                     {"properties",
                      {{"zMin", number(0, 10000, "Bottom of the range, mm above the object's bottom.")},
                       {"zMax", number(0, 10000, "Top of the range, mm; may be above the object's top.")},
                       {"layerHeight", number(0.01, 5, "Layer height inside the range, mm (default: the object's).")},
                       {"values", values_schema()}}}};
        d.effects  = {"settings"};
        d.ui       = {"GUI_Factories.cpp::menu::Height Range Modifier", "GUI_ObjectLayers.cpp::layerctl::add_layer_range_after_current"};
        d.examples = json::array({{{"target", object0()},
                                   {"params", {{"zMin", 0}, {"zMax", 2}, {"values", json::array({{{"key", "sparse_infill_density"}, {"value", "40%"}}})}}}}});
        c.add(std::move(d), run_add_range, enabled_fff);
    }
    {
        ActionDescriptor d;
        d.id      = "layer.removeRange";
        d.label   = "Delete height range";
        d.summary = "Remove one height range of an object (by its zMin/zMax), or all of them. Undoable.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "object"}, {"required", true}};
        d.params  = {{"type", "object"},
                     {"additionalProperties", false},
                     {"x-exactlyOne", json::array({json::array({"zMin", "zMax"}), json::array({"all"})})},
                     {"properties",
                      {{"zMin", number(0, 10000, "Bottom of the range to remove, mm.")},
                       {"zMax", number(0, 10000, "Top of the range to remove, mm.")},
                       {"all", {{"type", "boolean"}, {"description", "Remove every height range of the object."}}}}}};
        d.effects  = {"settings"};
        d.ui       = {"GUI_ObjectLayers.cpp::layerctl::del_layer_range"};
        d.examples = json::array({{{"target", object0()}, {"params", {{"all", true}}}}});
        c.add(std::move(d), run_remove_range, enabled_fff);
    }
    {
        ActionDescriptor d;
        d.id      = "layer.editRange";
        d.label   = "Edit height range";
        d.summary = "Move a height range of an object to new bounds, keeping its settings. Undoable.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "object"}, {"required", true}};
        d.params  = {{"type", "object"},
                     {"additionalProperties", false},
                     {"required", json::array({"zMin", "zMax", "newZMin", "newZMax"})},
                     {"properties",
                      {{"zMin", number(0, 10000, "Current bottom of the range, mm.")},
                       {"zMax", number(0, 10000, "Current top of the range, mm.")},
                       {"newZMin", number(0, 10000, "New bottom, mm.")},
                       {"newZMax", number(0, 10000, "New top, mm.")}}}};
        d.effects  = {"settings"};
        d.ui       = {"GUI_ObjectLayers.cpp::layerctl::etMinZ", "GUI_ObjectLayers.cpp::layerctl::etMaxZ"};
        d.examples = json::array({{{"target", object0()}, {"params", {{"zMin", 0}, {"zMax", 2}, {"newZMin", 1}, {"newZMax", 3}}}}});
        c.add(std::move(d), run_edit_range, enabled_fff);
    }
    {
        ActionDescriptor d;
        d.id      = "object.layerHeightProfile";
        d.label   = "Variable layer height";
        d.summary = "Variable layer height for objects: adaptive (finer layers where the surface is sloped; quality 0 = fast, 1 = fine), "
                    "smooth the current profile, or reset to the uniform layer height. Undoable.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "objects"}, {"required", true}};
        d.params  = {{"type", "object"},
                     {"additionalProperties", false},
                     {"required", json::array({"mode"})},
                     {"properties",
                      {{"mode", {{"type", "string"}, {"enum", json::array({"adaptive", "smooth", "reset"})}}},
                       {"quality", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"default", 0.5}, {"description", "adaptive: 0 = speed, 1 = quality."}}},
                       {"radius", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}, {"default", 5}, {"description", "smooth: radius in layers."}}},
                       {"keepMin", {{"type", "boolean"}, {"default", false}, {"description", "smooth: keep the minimum layer heights."}}}}}};
        d.effects  = {"settings"};
        d.ui       = {"GLCanvas3D.cpp::toolbar::layersediting", "GUI_ObjectList.cpp::column::colHeight"};
        d.examples = json::array({{{"target", object0()}, {"params", {{"mode", "adaptive"}, {"quality", 0.7}}}},
                                  {{"target", object0()}, {"params", {{"mode", "reset"}}}}});
        c.add(std::move(d), run_layer_height_profile, enabled_fff);
    }
}

}}} // namespace Slic3r::GUI::Actions
