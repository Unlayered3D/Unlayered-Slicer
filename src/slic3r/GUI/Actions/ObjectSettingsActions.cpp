// Automation actions: object.getSettings / object.setSettings / part.getSettings / part.setSettings,
// object.setBuildDirection / part.setBuildDirection -- the per-object, per-part and per-height-range overrides of the object list's "Add settings",
// "Edit Process Settings", "Edit in Parameter Table" and "Flush Options" menus.
// Owner: work package S4 (object settings). Docs: docs/bridge-actions/object-settings.md.
//
// The first part of this file is the pure rule set (ObjectSettingsRules.hpp, unit-tested); then
// the GUI helpers shared with LayerHeightActions.cpp; then the runners.

#include "ObjectSettingsRules.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <memory>
#include <sstream>

#include "libslic3r/CutPlaneSegmentation.hpp" // object_has_directed_parts
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "ActionTargets.hpp"
#include "CutPlaneRules.hpp"
#include "Register.hpp"

#include "../GUI_App.hpp"
#include "../GUI_ObjectList.hpp"
#include "../ObjectDataViewModel.hpp"
#include "../Plater.hpp"
#include "../WebAppBridge.hpp"
#include "../WebAppSettings.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;

// =============================================================================================
// Pure rules
// =============================================================================================

namespace ObjectSettings {

const char* to_string(Level level)
{
    switch (level) {
    case Level::Object:     return "object";
    case Level::Part:       return "part";
    case Level::LayerRange: return "layerRange";
    }
    return "object";
}

namespace {

std::vector<std::string> sorted_unique(std::vector<std::string> v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

bool contains(const std::vector<std::string>& sorted, const std::string& key)
{
    return std::binary_search(sorted.begin(), sorted.end(), key);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// ConfigOptionDef::is_value_valid() for the numeric types, scalar and vector: the rule
// slicer.setSettings applies. A percentage in a float-or-percent is not range-checked, as the GUI
// field does not check it either.
bool numeric_value_valid(const ConfigOptionDef& def, const ConfigOption* opt)
{
    if (opt == nullptr)
        return true;
    switch (def.type) {
    case coFloat:
    case coPercent: return def.is_value_valid(opt->getFloat());
    case coFloatOrPercent:
        if (auto* fp = dynamic_cast<const ConfigOptionFloatOrPercent*>(opt); fp != nullptr && fp->percent)
            return true;
        return def.is_value_valid(opt->getFloat());
    case coInt: return def.is_value_valid(static_cast<double>(opt->getInt()));
    case coFloats:
    case coPercents:
        if (def.nullable)
            return true;
        if (auto* floats = dynamic_cast<const ConfigOptionFloats*>(opt); floats != nullptr)
            for (double v : floats->values)
                if (!def.is_value_valid(v))
                    return false;
        return true;
    case coInts:
        if (def.nullable)
            return true;
        if (auto* ints = dynamic_cast<const ConfigOptionInts*>(opt); ints != nullptr)
            for (int v : ints->values)
                if (!def.is_value_valid(static_cast<double>(v)))
                    return false;
        return true;
    default: return true;
    }
}

std::string range_text(const ConfigOptionDef& def)
{
    std::ostringstream oss;
    oss << "value out of range [";
    if (def.min > -FLT_MAX)
        oss << def.min;
    else
        oss << "-inf";
    oss << ", ";
    if (def.max < FLT_MAX)
        oss << def.max;
    else
        oss << "inf";
    oss << "]";
    return oss.str();
}

} // namespace

const std::vector<std::string>& level_keys(Level level)
{
    // A part also takes the two build-direction angles of a directed part (the Cut Planes tool's
    // "Use plane as build direction" with one part selected; DECLARATIVE_CUT_PLANES.md).
    static const std::vector<std::string> part_keys = [] {
        std::vector<std::string> v = PrintRegionConfig().keys();
        v.push_back("layer_rotation_b");
        v.push_back("layer_rotation_c");
        return sorted_unique(std::move(v));
    }();
    static const std::vector<std::string> object_keys = [] {
        std::vector<std::string> v = PrintRegionConfig().keys();
        std::vector<std::string> o = PrintObjectConfig().keys();
        v.insert(v.end(), o.begin(), o.end());
        return sorted_unique(std::move(v));
    }();
    static const std::vector<std::string> range_keys = [] {
        std::vector<std::string> v = PrintRegionConfig().keys();
        v.push_back("layer_height");
        v.push_back("extruder");
        return sorted_unique(std::move(v));
    }();
    switch (level) {
    case Level::Object:     return object_keys;
    case Level::Part:       return part_keys;
    case Level::LayerRange: return range_keys;
    }
    return object_keys;
}

KeyVerdict check_key(const std::string& key, Level level, int filament_count, bool write)
{
    KeyVerdict             v;
    const ConfigOptionDef* def = print_config_def.get(key);
    if (def == nullptr) {
        v.code = WAB_NOT_FOUND;
        v.why  = "unknown option";
        return v;
    }
    if (write) {
        std::string why;
        if (!WebAppSettings::key_writable(key, why)) {
            v.code = WAB_FORBIDDEN_ORIGIN;
            v.why  = why;
            return v;
        }
    } else if (!WebAppSettings::key_readable(key, false)) {
        v.code = WAB_FORBIDDEN_ORIGIN;
        v.why  = "never exposed over the bridge";
        return v;
    }
    if (!contains(level_keys(level), key)) {
        v.code = WAB_BAD_PARAMS;
        if (level != Level::Object && contains(level_keys(Level::Object), key))
            v.why = "an object-wide option: set it on the object, not on a part or height range";
        else
            v.why = "not a per-object option (printer, filament or whole-plate setting): change it with slicer.setSettings";
        return v;
    }
    const bool range_own_key = level == Level::LayerRange && (key == "layer_height" || key == "extruder");
    if (key == "extruder" && !range_own_key) {
        v.code = WAB_BAD_PARAMS;
        v.why  = "the filament of an object or part is set through its filament, not as a setting";
        return v;
    }
    if (!range_own_key) {
        const std::string& category = def->category;
        if (category.empty()) {
            v.code = WAB_BAD_PARAMS;
            v.why  = "the slicer does not offer this option per object";
            return v;
        }
        if (filament_count <= 1 && (category == "Extruders" || category == "Wipe options")) {
            v.code = WAB_BAD_PARAMS;
            v.why  = "only offered per object when more than one filament is loaded";
            return v;
        }
        if (level == Level::Part && category == "Support material") {
            v.code = WAB_BAD_PARAMS;
            v.why  = "support options apply to whole objects";
            return v;
        }
    }
    v.ok = true;
    return v;
}

std::vector<std::string> allowed_keys(Level level, int filament_count, const std::string& query, const std::string& category)
{
    const std::string        q = lower(query);
    std::vector<std::string> out;
    for (const std::string& key : level_keys(level)) {
        if (!check_key(key, level, filament_count, true).ok)
            continue;
        const ConfigOptionDef* def = print_config_def.get(key);
        if (!category.empty() && (def == nullptr || def->category != category))
            continue;
        if (!q.empty()) {
            const bool hit = lower(key).find(q) != std::string::npos ||
                             (def != nullptr && (lower(def->label).find(q) != std::string::npos ||
                                                 lower(def->full_label).find(q) != std::string::npos ||
                                                 lower(def->category).find(q) != std::string::npos));
            if (!hit)
                continue;
        }
        out.push_back(key);
    }
    return out;
}

std::string set_value(DynamicPrintConfig& cfg, const std::string& key, const std::string& text)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    if (def == nullptr)
        return "unknown option";
    std::unique_ptr<ConfigOption> before;
    if (const ConfigOption* opt = cfg.option(key); opt != nullptr)
        before.reset(opt->clone());
    std::string error;
    try {
        ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
        cfg.set_deserialize(key, text, context); // runs handle_legacy(); throws on a value it cannot parse
        if (!numeric_value_valid(*def, cfg.option(key)))
            error = range_text(*def);
    } catch (const std::exception& e) {
        error = e.what();
        if (error.empty())
            error = "cannot parse the value";
    }
    if (!error.empty()) {
        if (before)
            cfg.set_key_value(key, before.release());
        else
            cfg.erase(key);
    }
    return error;
}

std::string check_layer_height(double layer_height, double min_layer_height, double max_layer_height, double nozzle_diameter)
{
    if (max_layer_height < EPSILON)
        max_layer_height = 0.75 * nozzle_diameter;
    if (!(layer_height > 0.))
        return "layer height must be positive";
    if (layer_height < min_layer_height - EPSILON || (max_layer_height > EPSILON && layer_height > max_layer_height + EPSILON)) {
        std::ostringstream oss;
        oss << "layer height out of the printer's range [" << min_layer_height << ", " << max_layer_height << "]";
        return oss.str();
    }
    return {};
}

std::string check_range(double z_min, double z_max, double object_height, const t_layer_config_ranges& ranges, double min_span,
                        const t_layer_height_range* ignore)
{
    if (!(z_min >= 0.))
        return "zMin must be 0 or more";
    if (!(z_max - z_min >= min_span - 1e-9))
        return "zMax must be above zMin";
    if (object_height > 0. && z_min >= object_height - 1e-9) {
        std::ostringstream oss;
        oss << "zMin is at or above the top of the object (" << object_height << " mm)";
        return oss.str();
    }
    for (const auto& entry : ranges) {
        const t_layer_height_range& range = entry.first;
        if (ignore != nullptr && std::abs(range.first - ignore->first) < 1e-6 && std::abs(range.second - ignore->second) < 1e-6)
            continue;
        if (z_min < range.second - 1e-6 && z_max > range.first + 1e-6) {
            std::ostringstream oss;
            oss << "overlaps the height range " << range.first << "-" << range.second << " mm";
            return oss.str();
        }
    }
    return {};
}

t_layer_config_ranges::const_iterator find_range(const t_layer_config_ranges& ranges, double z_min, double z_max, double tolerance)
{
    for (auto it = ranges.begin(); it != ranges.end(); ++it)
        if (std::abs(it->first.first - z_min) <= tolerance && std::abs(it->first.second - z_max) <= tolerance)
            return it;
    return ranges.end();
}

// =============================================================================================
// GUI helpers (also used by LayerHeightActions.cpp)
// =============================================================================================

int filament_count()
{
    const int n = int(wxGetApp().filaments_cnt());
    return n < 1 ? 1 : n;
}

std::string fff_only_reason()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return "the plater is not ready";
    return plater->printer_technology() == ptFFF ? std::string() : std::string("per-object settings need an FFF printer");
}

void refresh_after_change(int obj_idx, ModelObject* mo, ModelVolume* mv, bool ranges_changed)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || mo == nullptr || obj_idx < 0)
        return;
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr) {
        if (ranges_changed) {
            // Rebuild the object's "Layers" node from layer_config_ranges, as pasting ranges does.
            if (ObjectDataViewModel* model = list->GetModel(); model != nullptr) {
                wxDataViewItem obj_item = model->GetItemById(obj_idx);
                if (obj_item.IsOk()) {
                    wxDataViewItem layers_item = model->GetLayerRootItem(obj_item);
                    if (layers_item.IsOk())
                        model->Delete(layers_item);
                    if (!mo->layer_config_ranges.empty())
                        list->add_layer_root_item(obj_item);
                }
            }
        } else {
            list->object_config_options_changed(ObjectVolumeID{mo, mv});
        }
        list->update_info_items(size_t(obj_idx));
        list->part_selection_changed(); // the settings panel shows the new overrides
    }
    plater->changed_object(obj_idx);
}

// The Cut Planes tool's object_tilt_limit_deg: the object (or a part of it) builds as one tilted
// piece, so the run the gantry clears is the object's footprint. 0 = no limit.
static double object_tilt_limit(const ModelObject& mo, const Vec3d& n_world)
{
    if (mo.instances.empty())
        return virtual_max_tilt_deg(0., CutPlaneRules::tilt_azimuth(n_world));
    const BoundingBoxf3 bb = mo.raw_mesh_bounding_box().transformed(mo.instances.front()->get_matrix());
    return virtual_max_tilt_deg(std::hypot(bb.size().x(), bb.size().y()), CutPlaneRules::tilt_azimuth(n_world));
}

static bool is_build_direction_key(const std::string& key) { return key == "layer_rotation_b" || key == "layer_rotation_c"; }

static void finish_build_direction(const ModelObject& mo, ModelVolume* part, bool was_directed, DynamicPrintConfig& cfg, json& out)
{
    if (cfg.has("layer_rotation_b") || cfg.has("layer_rotation_c")) {
        const double b = cfg.has("layer_rotation_b") ? cfg.opt_float("layer_rotation_b") : 0.;
        const double c = cfg.has("layer_rotation_c") ? cfg.opt_float("layer_rotation_c") : 0.;
        const Vec3d  n = Geometry::build_normal_from_bc(b, c);
        if (const double limit = object_tilt_limit(mo, n); limit > 0.) {
            const Vec3d clamped = CutPlaneRules::clamp_to_tilt(n, limit);
            if (!clamped.isApprox(n, 1e-9)) {
                double nb = 0., nc = 0.;
                Geometry::bc_from_build_normal(clamped, nb, nc);
                cfg.set_key_value("layer_rotation_b", new ConfigOptionFloat(nb));
                cfg.set_key_value("layer_rotation_c", new ConfigOptionFloat(nc));
                out["tiltClamped"] = {{"tiltDeg", std::abs(b)}, {"limitDeg", limit}, {"b", nb}, {"c", nc}};
            }
        }
    }
    const bool by_group = wxGetApp().global_print_sequence() == PrintSequence::ByGroup;
    out["printSequenceByGroup"] = by_group;
    // The tool offers "By group" for the FIRST directed part of an object (offer_by_group_sequence);
    // a page cannot answer that dialog, so it gets the hint instead.
    if (part != nullptr && !was_directed && !by_group && (cfg.has("layer_rotation_b") || cfg.has("layer_rotation_c")))
        out["hint"] = "per-part build directions need the print setting print_sequence = \"by group\"; set it with slicer.setSettings";
    if (part == nullptr && mo.cut_planes.active() && (cfg.has("layer_rotation_b") || cfg.has("layer_rotation_c")))
        out["note"] = "not used while this object's cut planes are enabled: each chunk builds along its own plane";
}

namespace {

double printer_option(const char* key, int extruder)
{
    const DynamicPrintConfig& cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    if (!cfg.has(key))
        return 0.;
    return cfg.opt_float(key, unsigned(std::max(0, extruder - 1)));
}

} // namespace

std::string check_range_layer_height(double layer_height, int extruder)
{
    return check_layer_height(layer_height, printer_option("min_layer_height", extruder), printer_option("max_layer_height", extruder),
                              printer_option("nozzle_diameter", extruder));
}

double default_range_layer_height(const ModelObject& mo)
{
    if (mo.config.has("layer_height"))
        return mo.config.opt_float("layer_height");
    return wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_float("layer_height");
}

json overrides_json(const DynamicPrintConfig& cfg)
{
    json out = json::object();
    for (const std::string& key : cfg.keys()) {
        if (!WebAppSettings::key_readable(key, false))
            continue;
        std::string text = cfg.opt_serialize(key);
        if (text.size() > 2000)
            text.resize(2000);
        out[key] = text;
    }
    return out;
}

json layer_ranges_json(const ModelObject& mo)
{
    json out = json::array();
    for (const auto& entry : mo.layer_config_ranges)
        out.push_back({{"zMin", entry.first.first}, {"zMax", entry.first.second}, {"overrides", overrides_json(entry.second.get())}});
    return out;
}

json layer_height_profile_json(const ModelObject& mo)
{
    const std::vector<coordf_t> profile = mo.layer_height_profile.get();
    json                        out     = {{"variable", profile.size() > 4}, {"points", profile.size() / 2}};
    if (profile.size() >= 2) {
        double lo = DBL_MAX, hi = 0.;
        for (size_t i = 1; i < profile.size(); i += 2) {
            lo = std::min(lo, double(profile[i]));
            hi = std::max(hi, double(profile[i]));
        }
        out["minLayerHeight"] = lo;
        out["maxLayerHeight"] = hi;
    }
    return out;
}

void apply_edits(DynamicPrintConfig& copy, Level level, const json& params, int range_extruder, std::vector<std::string>& applied,
                 std::vector<std::string>& removed, json& rejected)
{
    auto reject = [&rejected](const std::string& key, int code, const std::string& why) {
        rejected.push_back({{"key", key}, {"code", code}, {"message", why}});
    };
    auto is_kept = [level](const std::string& key) { return key == "extruder" || (level == Level::LayerRange && key == "layer_height"); };

    if (params.value("clear", false)) {
        for (const std::string& key : copy.keys())
            if (!is_kept(key) && copy.erase(key))
                removed.push_back(key);
    }
    if (auto it = params.find("remove"); it != params.end() && it->is_array()) {
        for (const json& k : *it) {
            const std::string key = k.is_string() ? k.get<std::string>() : std::string();
            if (is_kept(key)) {
                reject(key, WAB_BAD_PARAMS,
                       level == Level::LayerRange ? "a height range always keeps its own layer height and filament" :
                                                    "the filament of an object or part is not a removable setting");
                continue;
            }
            if (!copy.has(key)) {
                if (std::find(removed.begin(), removed.end(), key) == removed.end())
                    reject(key, WAB_NOT_FOUND, "not overridden here");
                continue;
            }
            copy.erase(key);
            removed.push_back(key);
        }
    }
    const int fc = filament_count();
    if (auto it = params.find("values"); it != params.end() && it->is_array()) {
        for (const json& entry : *it) {
            const std::string key  = entry.value("key", std::string());
            const std::string text = entry.value("value", std::string());
            const KeyVerdict  v    = check_key(key, level, fc, true);
            if (!v.ok) {
                reject(key, v.code, v.why);
                continue;
            }
            DynamicPrintConfig trial = copy;
            if (std::string error = set_value(trial, key, text); !error.empty()) {
                reject(key, WAB_BAD_PARAMS, error);
                continue;
            }
            if (level == Level::LayerRange && key == "layer_height") {
                if (std::string error = check_range_layer_height(trial.opt_float("layer_height"), range_extruder); !error.empty()) {
                    reject(key, WAB_BAD_PARAMS, error);
                    continue;
                }
            }
            if (level == Level::LayerRange && key == "extruder") {
                const int e = trial.opt_int("extruder");
                if (e < 0 || e > fc) {
                    reject(key, WAB_BAD_PARAMS, "filament must be 0 (the object's) to " + std::to_string(fc));
                    continue;
                }
            }
            copy = std::move(trial);
            removed.erase(std::remove(removed.begin(), removed.end(), key), removed.end());
            if (std::find(applied.begin(), applied.end(), key) == applied.end())
                applied.push_back(key);
        }
    }
}

json layer_range_schema()
{
    return {{"type", "object"},
            {"additionalProperties", false},
            {"required", json::array({"zMin", "zMax"})},
            {"description", "A height range of the object (Height Range Modifier), by its bounds in mm from the object's bottom."},
            {"properties", {{"zMin", {{"type", "number"}, {"minimum", 0}, {"maximum", 10000}}}, {"zMax", {{"type", "number"}, {"minimum", 0}, {"maximum", 10000}}}}}};
}

json values_schema()
{
    return {{"type", "array"},
            {"minItems", 0},
            {"maxItems", 64},
            {"description", "Options to override: the key and the value in the option's text form (\"3\", \"15%\", \"gyroid\", \"1\" for on)."},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", json::array({"key", "value"})},
              {"properties", {{"key", {{"type", "string"}, {"minLength", 1}, {"maxLength", 100}}}, {"value", {{"type", "string"}, {"maxLength", 4000}}}}}}}};
}

} // namespace ObjectSettings

// =============================================================================================
// Runners
// =============================================================================================

namespace {

using namespace ObjectSettings;

// The config an invocation reads or edits, resolved from the target and params.layerRange.
struct Where
{
    Level                level   = Level::Object;
    int                  obj_idx = -1;
    int                  vol_idx = -1;
    ModelObject*         mo      = nullptr;
    ModelVolume*         mv      = nullptr;
    t_layer_height_range range{0., 0.};
    ModelConfig*         config = nullptr;
};

bool resolve_where(InvokeContext& ctx, bool part, Where& w, InvokeResult& err)
{
    Model& model = ctx.model();
    if (part) {
        if (ctx.targets.volumes.size() != 1) {
            err = InvokeResult::error(WAB_BAD_PARAMS, "name exactly one part", {{"path", "/target"}, {"needs", "volume"}});
            return false;
        }
        w.level   = Level::Part;
        w.obj_idx = ctx.targets.volumes.front().first;
        w.vol_idx = ctx.targets.volumes.front().second;
    } else {
        if (ctx.targets.objects.size() != 1) {
            err = InvokeResult::error(WAB_BAD_PARAMS, "name exactly one object", {{"path", "/target"}, {"needs", "object"}});
            return false;
        }
        w.obj_idx = ctx.targets.objects.front();
    }
    if (w.obj_idx < 0 || size_t(w.obj_idx) >= model.objects.size()) {
        err = InvokeResult::error(WAB_NOT_FOUND, "object not found");
        return false;
    }
    w.mo = model.objects[size_t(w.obj_idx)];
    if (part) {
        if (w.vol_idx < 0 || size_t(w.vol_idx) >= w.mo->volumes.size()) {
            err = InvokeResult::error(WAB_NOT_FOUND, "part not found");
            return false;
        }
        w.mv = w.mo->volumes[size_t(w.vol_idx)];
        if (!w.mv->is_model_part() && !w.mv->is_modifier()) {
            err = InvokeResult::error(WAB_BAD_PARAMS, std::string("only parts and modifiers carry settings; this is a ") +
                                                          (w.mv->is_negative_volume() ? "negative part" : "support blocker, enforcer or infill mesh"));
            return false;
        }
        w.config = &w.mv->config;
        return true;
    }
    if (auto it = ctx.params.find("layerRange"); it != ctx.params.end() && it->is_object()) {
        const double z_min = it->value("zMin", 0.);
        const double z_max = it->value("zMax", 0.);
        auto         found = find_range(w.mo->layer_config_ranges, z_min, z_max);
        if (found == w.mo->layer_config_ranges.end()) {
            err = InvokeResult::error(WAB_NOT_FOUND, "the object has no height range with those bounds",
                                      {{"layerRanges", layer_ranges_json(*w.mo)}});
            return false;
        }
        w.level  = Level::LayerRange;
        w.range  = found->first;
        w.config = &w.mo->layer_config_ranges[found->first];
        return true;
    }
    w.config = &w.mo->config;
    return true;
}

json where_json(const Model& model, const Where& w)
{
    json out = {{"level", to_string(w.level)}, {"object", object_ref_json(model, w.obj_idx)}};
    if (w.level == Level::Part)
        out["volume"] = volume_ref_json(model, w.obj_idx, w.vol_idx);
    if (w.level == Level::LayerRange)
        out["layerRange"] = {{"zMin", w.range.first}, {"zMax", w.range.second}};
    return out;
}

InvokeResult run_get(InvokeContext& ctx, bool part)
{
    if (ctx.plater() == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the plater is not ready");
    Where        w;
    InvokeResult err;
    if (!resolve_where(ctx, part, w, err))
        return err;
    const Model& model = ctx.model();
    json         out   = where_json(model, w);
    out["overrides"]   = overrides_json(w.config->get());

    if (auto it = ctx.params.find("keys"); it != ctx.params.end() && it->is_array() && !it->empty()) {
        // Effective values: the override here, else the object's (for a part or range), else the
        // edited presets'.
        const DynamicPrintConfig preset    = wxGetApp().preset_bundle->full_config();
        json                     effective = json::object();
        json                     rejected  = json::array();
        for (const json& k : *it) {
            const std::string key = k.get<std::string>();
            const KeyVerdict  v   = check_key(key, w.level, filament_count(), false);
            if (!v.ok && v.code != WAB_BAD_PARAMS) {
                rejected.push_back({{"key", key}, {"code", v.code}, {"message", v.why}});
                continue;
            }
            const ConfigOption* opt    = nullptr;
            std::string         source = "preset";
            if (w.config->get().has(key)) {
                opt    = w.config->get().option(key);
                source = to_string(w.level);
            } else if (w.level != Level::Object && w.mo->config.has(key)) {
                opt    = w.mo->config.option(key);
                source = "object";
            } else {
                opt = preset.option(key);
            }
            json entry = {{"value", opt != nullptr ? json(opt->serialize()) : json(nullptr)}, {"source", source}};
            if (!v.ok)
                entry["settableHere"] = false;
            effective[key] = entry;
        }
        out["effective"] = effective;
        if (!rejected.empty())
            out["rejected"] = rejected;
    }

    if (w.level == Level::Object) {
        out["layerRanges"]        = layer_ranges_json(*w.mo);
        out["layerHeightProfile"] = layer_height_profile_json(*w.mo);
        json parts                = json::array();
        for (size_t i = 0; i < w.mo->volumes.size(); ++i) {
            const ModelVolume*             mv   = w.mo->volumes[i];
            const std::vector<std::string> keys = mv->config.keys();
            if (std::any_of(keys.begin(), keys.end(), [](const std::string& k) { return k != "extruder"; })) {
                json ref         = volume_ref_json(model, w.obj_idx, int(i));
                ref["overrides"] = overrides_json(mv->config.get());
                parts.push_back(ref);
            }
        }
        out["partsWithOverrides"] = parts;
    }

    if (ctx.params.value("includeAllowed", false)) {
        const std::vector<std::string> keys = allowed_keys(w.level, filament_count(), ctx.params.value("allowedQuery", std::string()),
                                                           ctx.params.value("allowedCategory", std::string()));
        const size_t cap   = 200;
        const bool   brief = keys.size() > 60; // labels only for a short list: the reply stays small
        json         list  = json::array();
        for (size_t i = 0; i < keys.size() && i < cap; ++i) {
            if (brief) {
                list.push_back(keys[i]);
            } else {
                const ConfigOptionDef* def = print_config_def.get(keys[i]);
                list.push_back({{"key", keys[i]}, {"label", def ? def->label : std::string()}, {"category", def ? def->category : std::string()}});
            }
        }
        out["allowed"] = {{"total", keys.size()}, {"keys", list}, {"truncated", keys.size() > cap}};
    }
    return InvokeResult::ok(out);
}

InvokeResult run_set(InvokeContext& ctx, bool part)
{
    if (ctx.plater() == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the plater is not ready");
    const bool has_values = ctx.params.contains("values") && !ctx.params["values"].empty();
    const bool has_remove = ctx.params.contains("remove") && !ctx.params["remove"].empty();
    if (!has_values && !has_remove && !ctx.params.value("clear", false))
        return InvokeResult::error(WAB_BAD_PARAMS, "give values, remove or clear", {{"path", "/params/values"}, {"needs", "values"}});
    Where        w;
    InvokeResult err;
    if (!resolve_where(ctx, part, w, err))
        return err;

    DynamicPrintConfig       copy = w.config->get();
    std::vector<std::string> applied, removed;
    json                     rejected       = json::array();
    const int                range_extruder = (w.level == Level::LayerRange && copy.has("extruder")) ? copy.opt_int("extruder") : 0;
    apply_edits(copy, w.level, ctx.params, range_extruder, applied, removed, rejected);

    if (applied.empty() && removed.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "no value was applied",
                                   {{"applied", json::array()}, {"removed", json::array()}, {"rejected", rejected}});

    // The build direction: held at the printer's tilt limit, as the Cut Planes tool writes it.
    json direction = json::object();
    if (w.mv != nullptr && !w.mv->is_model_part() &&
        std::any_of(applied.begin(), applied.end(), [](const std::string& k) { return is_build_direction_key(k); }))
        return InvokeResult::error(WAB_BAD_PARAMS, "only a solid part can have its own build direction, not a modifier",
                                   {{"path", "/params/values"}});
    if (w.level != Level::LayerRange &&
        std::any_of(applied.begin(), applied.end(), [](const std::string& k) { return is_build_direction_key(k); }))
        finish_build_direction(*w.mo, w.mv, object_has_directed_parts(*w.mo), copy, direction);

    ctx.snapshot();
    w.config->assign_config(std::move(copy));
    refresh_after_change(w.obj_idx, w.mo, w.mv, w.level == Level::LayerRange);

    json out         = where_json(ctx.model(), w);
    out.update(direction);
    out["applied"]   = applied;
    out["removed"]   = removed;
    out["rejected"]  = rejected;
    out["overrides"] = overrides_json(w.config->get());
    out["undoable"]  = true;
    return InvokeResult::ok(out);
}

// object.setBuildDirection / part.setBuildDirection: the Cut Planes tool's "Use plane as build
// direction" and "Reset direction" (render_object_build_direction), on the object or on one part.
InvokeResult run_set_build_direction(InvokeContext& ctx, bool part)
{
    if (ctx.plater() == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the plater is not ready");
    Where        w;
    InvokeResult err;
    if (!resolve_where(ctx, part, w, err))
        return err;
    if (w.mv != nullptr && !w.mv->is_model_part())
        return InvokeResult::error(WAB_BAD_PARAMS, "only a solid part can have its own build direction, not a modifier", {{"path", "/target"}});
    if (w.mo->instances.empty())
        return InvokeResult::error(WAB_NOT_FOUND, "the object has no instance");
    const json& p = ctx.params;

    DynamicPrintConfig copy         = w.config->get();
    const bool         was_directed = object_has_directed_parts(*w.mo);
    json               out          = where_json(ctx.model(), w);
    if (p.contains("reset") && !p["reset"].get<bool>())
        return InvokeResult::error(WAB_BAD_PARAMS, "reset must be true; give normal, bDeg/cDeg or fromPlane to set a direction",
                                   {{"path", "/params/reset"}});
    if (p.value("reset", false)) {
        if (!copy.has("layer_rotation_b") && !copy.has("layer_rotation_c"))
            return InvokeResult::ok({{"changed", false}, {"note", part ? "the part already builds with the object" : "the object already builds along +Z"}})
                .with_effects({});
        copy.erase("layer_rotation_b");
        copy.erase("layer_rotation_c");
        ctx.snapshot(part ? "Reset part build direction" : "Reset object build direction");
        w.config->assign_config(std::move(copy));
        refresh_after_change(w.obj_idx, w.mo, w.mv, false);
        out["changed"] = true;
        out["reset"]   = true;
        return InvokeResult::ok(out);
    }

    Vec3d n_world;
    if (p.contains("normal")) {
        const json& a = p["normal"];
        n_world       = Vec3d(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
        if (n_world.norm() < 1e-9)
            return InvokeResult::error(WAB_BAD_PARAMS, "normal must not be zero", {{"path", "/params/normal"}});
        n_world.normalize();
    } else if (p.contains("fromPlane")) {
        const int i = p["fromPlane"].get<int>();
        if (i < 0 || i >= int(w.mo->cut_planes.planes.size()))
            return InvokeResult::error(WAB_NOT_FOUND, "the object has no cut plane " + std::to_string(i),
                                       {{"path", "/params/fromPlane"}, {"planes", w.mo->cut_planes.planes.size()}});
        n_world = CutPlaneRules::world_normal(w.mo->instances.front()->get_matrix(), w.mo->cut_planes.planes[size_t(i)].normal);
    } else {
        n_world = CutPlaneRules::normal_from_bc(p.value("bDeg", 0.), p.value("cDeg", 0.));
    }
    double b = 0., c = 0.;
    Geometry::bc_from_build_normal(n_world, b, c);
    copy.set_key_value("layer_rotation_b", new ConfigOptionFloat(b));
    copy.set_key_value("layer_rotation_c", new ConfigOptionFloat(c));
    finish_build_direction(*w.mo, w.mv, was_directed, copy, out);

    ctx.snapshot(part ? "Set part build direction" : "Set object build direction");
    w.config->assign_config(std::move(copy));
    refresh_after_change(w.obj_idx, w.mo, w.mv, false);
    const double fb = w.config->get().opt_float("layer_rotation_b"), fc = w.config->get().opt_float("layer_rotation_c");
    const Vec3d  fn = Geometry::build_normal_from_bc(fb, fc);
    out["changed"]     = true;
    out["b"]           = fb;
    out["c"]           = fc;
    out["buildNormal"] = {fn.x(), fn.y(), fn.z()};
    return InvokeResult::ok(out);
}

json build_direction_params()
{
    return {{"type", "object"},
            {"additionalProperties", false},
            {"x-exactlyOne", json::array({json::array({"normal"}), json::array({"bDeg", "cDeg"}), json::array({"fromPlane"}), json::array({"reset"})})},
            {"description", "Exactly one of: normal, bDeg/cDeg, fromPlane, reset. On a virtual 5-axis printer the direction is held at the "
                            "printer's tilt limit for the object's footprint (reported in tiltClamped)."},
            {"properties",
             {{"normal", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}, {"minimum", -1e3}, {"maximum", 1e3}}},
                          {"description", "World direction that becomes \"up\" when slicing, e.g. a face's outward normal."}}},
              {"bDeg", {{"type", "number"}, {"minimum", -180}, {"maximum", 0}, {"description", "Machine angle B (tilt from vertical), degrees."}}},
              {"cDeg", {{"type", "number"}, {"minimum", -180}, {"maximum", 180}, {"description", "Machine angle C (about Z), degrees."}}},
              {"fromPlane", {{"type", "integer"}, {"minimum", 0}, {"maximum", 63},
                             {"description", "Use this cut plane's normal (its arrow side), as \"Use plane as build direction\" does."}}},
              {"reset", {{"type", "boolean"}, {"description", "true: back to the default direction (Reset direction)."}}}}}};
}

json keys_schema(const char* description)
{
    return {{"type", "array"}, {"minItems", 0}, {"maxItems", 64}, {"description", description}, {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 100}}}};
}

json get_params(bool with_range)
{
    json props = {
        {"keys", keys_schema("Options whose effective value to report: the override here, else the object's, else the presets'.")},
        {"includeAllowed", {{"type", "boolean"}, {"default", false}, {"description", "Also list the options settable at this level."}}},
        {"allowedQuery", {{"type", "string"}, {"maxLength", 100}, {"description", "List only allowed options whose key, label or category contains this."}}},
        {"allowedCategory", {{"type", "string"}, {"maxLength", 100}, {"description", "List only allowed options of this category (Quality, Strength, Speed, Support, ...)."}}}};
    if (with_range)
        props["layerRange"] = layer_range_schema();
    return {{"type", "object"}, {"additionalProperties", false}, {"properties", props}};
}

json set_params(bool with_range)
{
    json props = {{"values", values_schema()},
                  {"remove", keys_schema("Overrides to remove, so the value is inherited again.")},
                  {"clear", {{"type", "boolean"}, {"default", false}, {"description", "Remove every override first (a height range keeps its layer height and filament)."}}}};
    if (with_range)
        props["layerRange"] = layer_range_schema();
    return {{"type", "object"}, {"additionalProperties", false}, {"properties", props}};
}

std::string enabled_fff(const InvokeContext&) { return fff_only_reason(); }

json object0() { return {{"objects", json::array({{{"index", 0}}})}}; }
json volume0() { return {{"volumes", json::array({{{"object", {{"index", 0}}}, {"index", 0}}})}}; }
json kv(const char* key, const char* value) { return {{"key", key}, {"value", value}}; }

} // namespace

void register_object_settings_actions(Catalog& c)
{
    {
        ActionDescriptor d;
        d.id      = "object.setBuildDirection";
        d.label   = "Object build direction";
        d.summary = "Slice a whole object along a tilted build direction (multiplanar printers): a world direction, machine angles "
                    "B/C or one of its cut planes' normals becomes \"up\"; or reset it. Stored as the object's layer_rotation_b/c. Undoable.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "object"}, {"required", true}};
        d.params  = build_direction_params();
        d.effects = {"settings"};
        d.ui      = {"GLGizmoCutPlanes.cpp::gizmoctl::Use plane as build direction", "GLGizmoCutPlanes.cpp::gizmoctl::Reset direction"};
        d.examples = json::array({{{"target", {{"object", {{"index", 0}}}}}, {"params", {{"bDeg", -30}, {"cDeg", 90}}}},
                                  {{"target", {{"object", {{"index", 0}}}}}, {"params", {{"normal", {0, -0.5, 0.866}}}}},
                                  {{"target", {{"object", {{"index", 0}}}}}, {"params", {{"reset", true}}}}});
        c.add(std::move(d), [](InvokeContext& ctx) { return run_set_build_direction(ctx, false); }, enabled_fff);
    }
    {
        ActionDescriptor d;
        d.id      = "part.setBuildDirection";
        d.label   = "Part build direction";
        d.summary = "Give one solid part its own build direction: it is sliced as its own section along that direction, printed after "
                    "the rest of the object (needs print_sequence \"by group\"); or reset it to build with the object. Undoable.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "volume"}, {"required", true}};
        d.params  = build_direction_params();
        d.effects = {"settings"};
        d.examples = json::array({{{"target", volume0()}, {"params", {{"bDeg", -45}, {"cDeg", 0}}}},
                                  {{"target", volume0()}, {"params", {{"reset", true}}}}});
        c.add(std::move(d), [](InvokeContext& ctx) { return run_set_build_direction(ctx, true); }, enabled_fff);
    }
    {
        ActionDescriptor d;
        d.id       = "object.getSettings";
        d.label    = "Object settings";
        d.summary  = "Read one object's own print-setting overrides (or one height range's), its height ranges, variable layer "
                     "height and parts with overrides; optionally effective values and the options settable per object.";
        d.risk     = Risk::Read;
        d.target   = {{"kind", "object"}, {"required", true}};
        d.params   = get_params(true);
        d.ui       = {"GUI_Factories.cpp::menu::expr:names2[0]@append_menu_item_per_object_process"};
        d.examples = json::array({{{"target", object0()}, {"params", {{"keys", json::array({"wall_loops", "sparse_infill_density"})}}}},
                                  {{"target", object0()}, {"params", {{"includeAllowed", true}, {"allowedQuery", "infill"}}}}});
        c.add(std::move(d), [](InvokeContext& ctx) { return run_get(ctx, false); }, enabled_fff);
    }
    {
        ActionDescriptor d;
        d.id      = "object.setSettings";
        d.label   = "Edit Process Settings";
        d.summary = "Override print settings for one object only, or for one of its height ranges (layerRange): more walls or "
                    "infill, flush into this object's infill, supports for this object... Remove overrides to inherit again. Undoable.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "object"}, {"required", true}};
        d.params  = set_params(true);
        d.effects = {"settings"};
        d.ui      = {"GUI_Factories.cpp::menu::expr:names[0]@append_menu_item_per_object_process",
                     "GUI_Factories.cpp::menu::expr:names3[0]@append_menu_item_per_object_process",
                     "GUI_Factories.cpp::menu::expr:names[0]@append_menu_item_per_object_settings",
                     "GUI_Factories.cpp::menu::expr:_(cat.first)@create_settings_popupmenu",
                     "GUI_Factories.cpp::menu::expr:_(category.first)@create_freq_settings_popupmenu",
                     "GUI_Factories.cpp::menu::Flush into objects' infill",
                     "GUI_Factories.cpp::menu::Flush into objects' support",
                     "GUI_Factories.cpp::menu::Flush into this object",
                     "GUI_ObjectList.cpp::column::colEditing",
                     "GUI_ObjectTable.cpp::tablecol::col_layer_height",
                     "GUI_ObjectTable.cpp::tablecol::col_wall_loops",
                     "GUI_ObjectTable.cpp::tablecol::col_fill_density",
                     "GUI_ObjectTable.cpp::tablecol::col_enable_support",
                     "GUI_ObjectTable.cpp::tablecol::col_brim_type",
                     "GUI_ObjectTable.cpp::tablecol::col_speed_perimeter",
                     "GUI_ObjectTable.cpp::tablecol::col_print_group"};
        d.examples = json::array({{{"target", object0()}, {"params", {{"values", json::array({kv("wall_loops", "4")})}}}},
                                  {{"target", object0()},
                                   {"params", {{"values", json::array({kv("flush_into_infill", "1"), kv("sparse_infill_density", "30%")})}}}}});
        c.add(std::move(d), [](InvokeContext& ctx) { return run_set(ctx, false); }, enabled_fff);
    }
    {
        ActionDescriptor d;
        d.id       = "part.getSettings";
        d.label    = "Part settings";
        d.summary  = "Read one part's or modifier's own print-setting overrides; optionally effective values and the options settable per part.";
        d.risk     = Risk::Read;
        d.target   = {{"kind", "volume"}, {"required", true}};
        d.params   = get_params(false);
        d.examples = json::array({{{"target", volume0()}, {"params", {{"keys", json::array({"sparse_infill_density"})}}}}});
        c.add(std::move(d), [](InvokeContext& ctx) { return run_get(ctx, true); }, enabled_fff);
    }
    {
        ActionDescriptor d;
        d.id       = "part.setSettings";
        d.label    = "Part settings";
        d.summary  = "Override print settings inside one part or modifier (e.g. a modifier cube with 50% infill). Object-wide options "
                     "such as supports or layer height belong on the object. Undoable.";
        d.risk     = Risk::Mutate;
        d.target   = {{"kind", "volume"}, {"required", true}};
        d.params   = set_params(false);
        d.effects  = {"settings"};
        d.examples = json::array({{{"target", volume0()}, {"params", {{"values", json::array({kv("sparse_infill_density", "50%")})}}}}});
        c.add(std::move(d), [](InvokeContext& ctx) { return run_set(ctx, true); }, enabled_fff);
    }
}

}}} // namespace Slic3r::GUI::Actions
