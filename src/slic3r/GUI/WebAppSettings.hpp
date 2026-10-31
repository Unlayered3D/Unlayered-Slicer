#pragma once

// The policy behind the settings-access family of Web App Bridge methods (WEB_APP_BRIDGE.md
// section 4, docs/HLSD/web-app-bridge.md): which preset keys a page may never see or never
// change, and the JSON shapes those methods share. Kept apart from WebAppBridge.cpp so the deny
// lists are one short file that can be reviewed on their own, and so Tab.cpp and Plater.cpp can
// use the pieces they need (the write-in-progress latch, the slice summary) without depending on
// the bridge itself.

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"

namespace Slic3r {

class ConfigBase;
class Print;

namespace GUI {

class PartPlate;
class Tab;

namespace WebAppSettings {

// Keys a page may neither read nor write: print-host addresses, credentials and the switches
// that decide where a G-code upload goes. PresetBundle::full_config_secure() erases the first
// seven before writing a project; the other four are the same category and held to the same
// rule. No configuration widens this list.
extern const std::vector<std::string> WAB_SECRET_KEYS;

// Custom G-code and post-processing: text the slicer emits into a print, or runs, verbatim.
// Never written over the bridge; readable only when the caller asks for it (includeCode).
extern const std::vector<std::string> WAB_CODE_KEYS;

// What a preset IS rather than what it does. Readable, never written: changing one turns the
// edited preset into a different preset, which is what slicer.selectPreset is for.
extern const std::vector<std::string> WAB_IDENTITY_KEYS;

bool key_is_secret(const std::string& key);
bool key_is_code(const std::string& key);
bool key_is_identity(const std::string& key);

// False for an unknown key, a secret key and -- unless include_code -- a custom-G-code key.
bool key_readable(const std::string& key, bool include_code);

// False for an unknown key and for every secret, code, identity, *_settings_id, read-only or
// plugin-backed key. `why` receives a one-line reason fit for the response's rejected[] entry.
bool key_writable(const std::string& key, std::string& why);

// "printer" | "print" | "filament" <-> Preset::Type. Nothing else is bridge-visible.
bool        preset_type_from_string(const std::string& name, Preset::Type& type);
const char* preset_type_name(Preset::Type type);

const char* type_name(ConfigOptionType type);
const char* mode_name(ConfigOptionMode mode);

// The schema entry for one option, from print_config_def. `cfg` (nullable) supplies `value` as
// the option's .ini serialisation when it holds the key. Null JSON for an unknown key.
nlohmann::json option_schema_json(const std::string& key, const ConfigBase* cfg);

// The couplings Tab::on_value_change keeps for the GUI's field handlers (filament_diameter and
// pellet_flow_coefficient, enable_prime_tower, single_extruder_multi_material,
// purge_in_prime_tower, gcode_flavor), which Tab::load_config does not run: called after a write
// of `keys` that `config` holds the new values of, so every automated write path -- setSettings,
// revertKeys, createFromTemplate, copyExtruder -- leaves the tab as a hand edit would. Returns
// whether any ran (the caller's tab then needs update_dirty + reload_config, done here).
bool apply_tab_couplings(Tab& tab, const ConfigBase& config, const std::vector<std::string>& keys);

// Per-variant settings (multi-extruder printers, high-flow nozzles): a vector option of the
// print, filament or printer preset holds one value per extruder variant, and the tabs' extruder
// switch / variant combo picks which index a page edits (Tab::switch_excluder). These map an
// extruder (0-based, as the switch counts) to that index with the same lookup.
struct VariantKeys
{
    const char*                  id_name;      // "print_extruder_id"; "" for filaments
    const char*                  variant_name; // "print_extruder_variant"
    const std::set<std::string>* keys;         // the per-variant options of that preset type
};
VariantKeys variant_keys_for(Preset::Type type);
// Values per variant of `key` in a preset of `type`: 0 (not per-variant), 1, or 2 (the printer's
// machine limits: normal and silent mode).
int variant_stride(Preset::Type type, const std::string& key);
// The edited printer's extruder count (its extruder_type list).
int printer_extruder_count();
// The variant index `extruder` edits in `cfg` (a config of `tab`'s type), or -1.
int variant_index_for(Tab& tab, const DynamicPrintConfig& cfg, int extruder);
// [{extruder, index, variant}] for every extruder of the printer.
nlohmann::json variants_json(Tab& tab, const DynamicPrintConfig& cfg);
// The serialised value(s) of one variant: `stride` values from index * stride, comma-joined.
std::string variant_value_text(const ConfigOption* opt, int index, int stride);

// What a page wants to know after a slice, from the plate's G-code result and its Print's
// statistics, plus its current slicing warnings (`warnings`). Null JSON when the plate has no
// valid slice result.
nlohmann::json slice_summary_json(PartPlate* plate);
// [{message, level: "critical"|"warning", object?}] for every current warning of the Print's and
// its objects' steps; `object` names the model object for object-step warnings.
nlohmann::json print_warnings_json(const Print& print);

} // namespace WebAppSettings

// Alive while slicer.setSettings pushes values into a Tab. Tab::on_value_change consults it so
// a bridge write is not echoed back to the page as a {source:"user"} settingsChanged event --
// the bridge reports the same write once, as {source:"bridge"}. GUI thread only.
class WebAppBridgeWriteScope
{
public:
    WebAppBridgeWriteScope();
    ~WebAppBridgeWriteScope();
    WebAppBridgeWriteScope(const WebAppBridgeWriteScope&) = delete;
    WebAppBridgeWriteScope& operator=(const WebAppBridgeWriteScope&) = delete;

    static bool active();
};

} // namespace GUI
} // namespace Slic3r
