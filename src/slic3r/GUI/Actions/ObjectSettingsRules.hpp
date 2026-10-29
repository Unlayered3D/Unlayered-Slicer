#ifndef slic3r_GUI_Actions_ObjectSettingsRules_hpp_
#define slic3r_GUI_Actions_ObjectSettingsRules_hpp_

// The pure rules behind the per-object / per-part / per-layer-range settings actions
// (ObjectSettingsActions.cpp, LayerHeightActions.cpp; docs/bridge-actions/object-settings.md):
// which option keys a person can override at each level in the object list, how a value given as
// text is parsed and range-checked, and when a new height range fits an object. No wx, no
// GUI_App: unit-tested in tests/slic3rutils/test_actions_objectsettings.cpp.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"

namespace Slic3r {
class ModelObject;
class ModelVolume;
} // namespace Slic3r

namespace Slic3r { namespace GUI { namespace Actions { namespace ObjectSettings {

// Where an override lives: ModelObject::config, ModelVolume::config, or one entry of
// ModelObject::layer_config_ranges.
enum class Level
{
    Object,
    Part,
    LayerRange,
};

const char* to_string(Level level);

// The option keys the object list offers at `level` for FFF printers, before the per-key checks
// of check_key(): every PrintRegionConfig key, plus every PrintObjectConfig key for an object,
// plus "layer_height" and "extruder" for a height range (SettingsFactory::get_options and
// get_bundle(..., is_layer_settings)). Sorted, unique.
const std::vector<std::string>& level_keys(Level level);

struct KeyVerdict
{
    bool        ok   = false;
    int         code = 0;   // bridge error code when !ok: 1008 unknown, 1003 deny-listed, 1002 not at this level
    std::string why;        // one line for the response's rejected[] entry
};

// May `key` be overridden (write = true) or reported (write = false) at `level`?
//   1008  unknown option;
//   1003  the bridge deny lists (secrets, custom G-code, preset identity, read-only, plugin-backed)
//         -- WebAppSettings::key_writable / key_readable, the same rules as slicer.setSettings;
//   1002  not an option of that level (an object-only key on a part or range, a printer or
//         filament key anywhere), "extruder" on an object or part (the filament column owns it),
//         a key the object list never offers (no category), or a multi-filament key while only
//         one filament is loaded -- the same filter as the object list's "Add settings" menu.
// `filament_count` is the number of filament slots (>= 1).
KeyVerdict check_key(const std::string& key, Level level, int filament_count, bool write);

// The keys check_key(write = true) accepts at `level`, optionally only those whose key, label or
// category contains `query` (case-insensitive) or whose category equals `category`.
std::vector<std::string> allowed_keys(Level level, int filament_count, const std::string& query = {},
                                      const std::string& category = {});

// Parses `text` (the option's .ini form, e.g. "3", "15%", "gyroid", "1") into `cfg[key]` and
// range-checks numeric values against the option's min/max. Returns "" on success; otherwise the
// reason, with `cfg` left as it was.
std::string set_value(DynamicPrintConfig& cfg, const std::string& key, const std::string& text);

// For a height range: is `layer_height` within [min_layer_height, max_layer_height]? A max of 0
// means 75 % of the nozzle diameter (the object list's rule). "" when fine.
std::string check_layer_height(double layer_height, double min_layer_height, double max_layer_height, double nozzle_diameter);

// Can [z_min, z_max] be added to an object `object_height` mm tall that already has `ranges`?
// It must be non-empty (z_max - z_min >= min_span), start inside the object, and not overlap
// another range (touching is fine). `ignore` (nullable) is a range to leave out of the overlap
// check -- the one being moved. "" when it fits.
std::string check_range(double z_min, double z_max, double object_height, const t_layer_config_ranges& ranges,
                        double min_span = 0.01, const t_layer_height_range* ignore = nullptr);

// The range of `ranges` whose bounds equal (z_min, z_max) within `tolerance`, or ranges.end().
t_layer_config_ranges::const_iterator find_range(const t_layer_config_ranges& ranges, double z_min, double z_max,
                                                  double tolerance = 1e-4);

// ---------------------------------------------------------------------------------------------
// GUI side, shared by ObjectSettingsActions.cpp and LayerHeightActions.cpp (defined in the former).
// These read the running application (filament count, edited presets) or refresh the object list;
// they are not for unit tests.
// ---------------------------------------------------------------------------------------------

int         filament_count();
// "" for an FFF printer, else why per-object settings are unavailable.
std::string fff_only_reason();
// After an override or height range changed: the object list's settings / layer items, the
// settings panel, and Plater::changed_object (re-slice scheduling).
void        refresh_after_change(int obj_idx, ModelObject* mo, ModelVolume* mv, bool ranges_changed);
// check_layer_height() against the edited printer preset, for filament `extruder` (0 = first).
std::string check_range_layer_height(double layer_height, int extruder);
// The layer height a new height range starts with (the object's, else the print preset's).
double      default_range_layer_height(const ModelObject& mo);

nlohmann::json overrides_json(const DynamicPrintConfig& cfg);   // {key: text}
nlohmann::json layer_ranges_json(const ModelObject& mo);        // [{zMin, zMax, overrides}]
nlohmann::json layer_height_profile_json(const ModelObject& mo); // {variable, points, minLayerHeight?, maxLayerHeight?}

// Applies params.clear / params.remove / params.values to `copy` at `level` with every per-key
// check; fills applied / removed and rejected[] ({key, code, message}).
void apply_edits(DynamicPrintConfig& copy, Level level, const nlohmann::json& params, int range_extruder,
                 std::vector<std::string>& applied, std::vector<std::string>& removed, nlohmann::json& rejected);

// Shared parameter schemas.
nlohmann::json layer_range_schema(); // {zMin, zMax}
nlohmann::json values_schema();      // [{key, value}]

}}}} // namespace Slic3r::GUI::Actions::ObjectSettings

#endif // slic3r_GUI_Actions_ObjectSettingsRules_hpp_
