// Automation actions: prefs.get / prefs.set over the Preferences allowlist (decision D4), and
// app.checkUpdates.
// Owner: work package S8 (filament/UI). See docs/HLSD/bridge-actions.md for how to write an action
// and docs/bridge-actions/filament-ui.md for these.
//
// Only cosmetic and behaviour preferences are reachable, plus the settings mode (simple / advanced /
// expert, the settings tabs' mode switch). "Remove mixed temperature restriction" lifts a check that
// refuses to slice: prefs.get reads it, and only prefs.allowMixedTemperatures changes it, behind a
// native confirmation. Language, single instance, file
// associations, login region, stealth mode, privacy and diagnostics, the update channel, preset and
// user sync, token storage, the network plug-in, developer/debug/log level, local web apps, SSL,
// hosts and "clear my choice" are not: the schema has no property for them, so prefs.set refuses
// them by path (1002) before anything is written.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "FilamentUiRules.hpp"
#include "Register.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include "../GCodeViewer.hpp"
#include "../GLCanvas3D.hpp"
#include "../GUI_App.hpp"
#include "../ImGuiWrapper.hpp"
#include "../Plater.hpp"
#include "../WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using nlohmann::json;

// ---- pure rules (FilamentUiRules.hpp) --------------------------------------------------------

const std::vector<PrefSpec>& pref_allowlist()
{
    auto boolean = [](const char* name, const char* key, const char* def, const char* label, const char* desc) {
        PrefSpec s;
        s.name = name, s.config_key = key, s.type = "boolean", s.default_stored = def, s.label = label, s.description = desc;
        return s;
    };
    auto enumeration = [](const char* name, const char* key, std::vector<std::string> values, std::vector<std::string> stored,
                          const char* def, const char* label, const char* desc) {
        PrefSpec s;
        s.name = name, s.config_key = key, s.type = "enum", s.values = std::move(values), s.stored = std::move(stored);
        s.default_stored = def, s.label = label, s.description = desc;
        return s;
    };
    auto integer = [](const char* name, const char* key, int min, int max, const char* def, const char* label, const char* desc) {
        PrefSpec s;
        s.name = name, s.config_key = key, s.type = "integer", s.minimum = min, s.maximum = max;
        s.default_stored = def, s.label = label, s.description = desc;
        return s;
    };
    auto number = [](const char* name, const char* key, double min, double max, int decimals, const char* def, const char* label,
                     const char* desc) {
        PrefSpec s;
        s.name = name, s.config_key = key, s.type = "number", s.min_number = min, s.max_number = max, s.decimals = decimals;
        s.default_stored = def, s.label = label, s.description = desc;
        return s;
    };
    static const std::vector<PrefSpec> list = {
        enumeration("units", "use_inches", {"metric", "imperial"}, {"0", "1"}, "0", "Units", "Units shown in the interface."),
        enumeration("defaultPage", "default_page", {"home", "prepare"}, {"0", "1"}, "0", "Default page", "The page opened on startup."),
        enumeration("loadBehaviour", SETTING_PROJECT_LOAD_BEHAVIOUR, {"loadAll", "askWhenRelevant", "alwaysAsk", "loadGeometryOnly"},
                    {OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL, OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT,
                     OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK, OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY},
                    OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL, "Load behaviour",
                    "Whether opening a 3MF also loads its printer, filament and process settings."),
        enumeration("colorScheme", "color_scheme", {"system", "light", "dark"}, {"system", "light", "dark"}, "system", "Colour scheme",
                    "Follow the system's light or dark appearance, or force one."),
        enumeration("colourVision", "preview_color_vision", {"normal", "protanopia", "deuteranopia", "tritanopia"},
                    {"normal", "protanopia", "deuteranopia", "tritanopia"}, "normal", "Colour vision",
                    "Palettes of the G-code preview and the bed mesh map for colour-vision deficiencies."),
        boolean("opaqueOverlays", "opaque_overlays", "false", "Opaque overlays", "Draw the 3D view's floating panels and toolbars fully opaque."),
        boolean("dimPreviousLayers", "preview_dim_previous_layers", "false", "Dim lower layers",
                "In the preview, dim the layers below the current one."),
        enumeration("cameraStyle", "camera_navigation_style", {"default", "touchpad"}, {"0", "1"}, "0", "Camera style",
                    "Default: left drag rotates, right/middle drag pans. Touchpad: Alt+move rotates, Shift+move pans."),
        boolean("zoomToMouse", "zoom_to_mouse", "false", "Zoom to mouse position", "Zoom towards the mouse pointer instead of the view centre."),
        boolean("useFreeCamera", "use_free_camera", "false", "Use free camera", "A free camera instead of the constrained one."),
        boolean("reverseMouseZoom", "reverse_mouse_wheel_zoom", "false", "Reverse mouse zoom", "Reverse the mouse wheel's zoom direction."),
        enumeration("leftMouseDrag", "left_mouse_drag_action", {"none", "pan", "rotate"}, {"0", "1", "2"}, "2", "Left Mouse Drag",
                    "What dragging with the left mouse button does."),
        enumeration("middleMouseDrag", "middle_mouse_drag_action", {"none", "pan", "rotate"}, {"0", "1", "2"}, "1", "Middle Mouse Drag",
                    "What dragging with the middle mouse button does."),
        enumeration("rightMouseDrag", "right_mouse_drag_action", {"none", "pan", "rotate"}, {"0", "1", "2"}, "1", "Right Mouse Drag",
                    "What dragging with the right mouse button does."),
        boolean("autoArrangeAfterClone", "auto_arrange", "true", "Auto arrange plate after cloning", "Arrange the plate after cloning objects."),
        enumeration("autoFlush", "auto_calculate_flush", {"all", "colorChange", "disabled"}, {"all", "color change", "disabled"}, "all",
                    "Auto flush after changing", "Recalculate flushing volumes when filaments or their colours change."),
        boolean("autoSlice", "auto_slice_after_change", "false", "Auto slice after changes", "Re-slice automatically after slicing-related changes."),
        integer("autoSliceDelaySeconds", "auto_slice_change_delay_seconds", 0, 600, "1", "Auto slice after changes",
                "Seconds to wait after a change before auto slicing, so several edits slice once."),
        // The dialog's spin box stops at 99: 100 would be the same as not dimming.
        integer("dimmedLayerBrightness", "preview_dim_previous_layers_brightness", 0, 99, "40", "Dimmed layer brightness",
                "Brightness in percent of the layers dimLowerLayers darkens (0 = black, 99 = barely darkened)."),
        boolean("showStepImportOptions", "enable_step_mesh_setting", "true", "Show options when importing STEP file",
                "Ask for the meshing options when the person imports a STEP file. Automated imports never ask: they mesh with "
                "stepLinearDeflection, stepAngleDeflection and stepSplitCompound."),
        number("stepLinearDeflection", "linear_deflection", 0.001, 0.1, 3, "0.003", "STEP importing: linear deflection",
               "Linear deflection in mm when meshing imported STEP files; smaller is finer and slower."),
        number("stepAngleDeflection", "angle_deflection", 0.01, 1.0, 2, "0.5", "STEP importing: angle deflection",
               "Angle deflection when meshing imported STEP files; smaller is finer and slower."),
        boolean("stepSplitCompound", "is_split_compound", "false", "STEP importing: Split into multiple objects",
                "Split compound and compsolid shapes of imported STEP files into separate objects."),
        boolean("rememberPrinterConfig", "remember_printer_config", "true", "Remember printer configuration",
                "Remember the filament and process chosen for each printer and switch back to them with the printer."),
        boolean("autoBackup", "backup_switch", "true", "Auto backup", "Back up the open project periodically, for crash recovery."),
        integer("autoBackupSeconds", "backup_interval", 10, 3600, "10", "Auto backup", "Seconds between automatic project backups."),
        boolean("noWarnModifiedGcode", "no_warn_when_modified_gcodes", "false", "Don't warn when loading 3MF with modified G-code",
                "Open a 3MF whose custom G-code was modified without the warning dialog."),
        boolean("filamentGroupingPopup", "pop_up_filament_map_dialog", "false", "Pop up to select filament grouping mode",
                "On multi-nozzle printers, ask for the filament grouping mode before slicing (a dialog the person answers)."),
        [&] {
            PrefSpec s = boolean("removeMixedTemperatureRestriction", "enable_high_low_temp_mixed_printing", "false",
                                 "Remove mixed temperature restriction",
                                 "Slice plates that mix filaments with very different temperatures. Read-only here: change it with "
                                 "prefs.allowMixedTemperatures, which asks the person first.");
            s.set_by = "prefs.allowMixedTemperatures";
            return s;
        }(),
        [&] {
            PrefSpec s = enumeration("settingsMode", "user_mode", {"simple", "advanced", "expert"}, {"simple", "advanced", "expert"},
                                     "simple", "Settings mode",
                                     "Which settings the print, filament and printer tabs show (the tabs' mode switch). Not "
                                     "changeable while developer mode is on.");
            s.preferences_row = false;
            return s;
        }(),
    };
    return list;
}

const PrefSpec* find_pref(const std::string& name)
{
    for (const PrefSpec& s : pref_allowlist())
        if (s.name == name)
            return &s;
    return nullptr;
}

bool pref_value_to_stored(const PrefSpec& spec, const json& value, std::string& stored, std::string& message)
{
    if (spec.type == "boolean") {
        if (!value.is_boolean()) {
            message = spec.name + " takes true or false";
            return false;
        }
        stored = value.get<bool>() ? "true" : "false";
        return true;
    }
    if (spec.type == "integer") {
        if (!value.is_number_integer() || value.get<long long>() < spec.minimum || value.get<long long>() > spec.maximum) {
            message = spec.name + " takes a whole number from " + std::to_string(spec.minimum) + " to " + std::to_string(spec.maximum);
            return false;
        }
        stored = std::to_string(value.get<long long>());
        return true;
    }
    if (spec.type == "number") {
        if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < spec.min_number ||
            value.get<double>() > spec.max_number) {
            std::ostringstream range;
            range.imbue(std::locale::classic());
            range << spec.min_number << " to " << spec.max_number;
            message = spec.name + " takes a number from " + range.str();
            return false;
        }
        // As the dialog's decimal field stores it (wxString::FromCDouble(value, decimals)).
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(spec.decimals) << value.get<double>();
        stored = out.str();
        return true;
    }
    if (value.is_string()) {
        auto it = std::find(spec.values.begin(), spec.values.end(), value.get<std::string>());
        if (it != spec.values.end()) {
            stored = spec.stored[size_t(it - spec.values.begin())];
            return true;
        }
    }
    message = spec.name + " takes one of: ";
    for (size_t i = 0; i < spec.values.size(); ++i)
        message += (i ? ", " : "") + spec.values[i];
    return false;
}

json pref_value_from_stored(const PrefSpec& spec, const std::string& stored_in)
{
    const std::string stored = stored_in.empty() ? spec.default_stored : stored_in;
    if (spec.type == "boolean")
        return stored == "true" || stored == "1";
    if (spec.type == "integer") {
        try {
            return std::clamp(std::stoi(stored), spec.minimum, spec.maximum);
        } catch (...) {
            return std::stoi(spec.default_stored);
        }
    }
    if (spec.type == "number") {
        auto parse = [](const std::string& text, double& v) {
            std::istringstream in(text);
            in.imbue(std::locale::classic());
            return bool(in >> v) && std::isfinite(v);
        };
        double v = 0.;
        if (!parse(stored, v))
            parse(spec.default_stored, v);
        return std::clamp(v, spec.min_number, spec.max_number);
    }
    auto it = std::find(spec.stored.begin(), spec.stored.end(), stored);
    if (it == spec.stored.end())
        it = std::find(spec.stored.begin(), spec.stored.end(), spec.default_stored);
    return it == spec.stored.end() ? json(spec.values.front()) : json(spec.values[size_t(it - spec.stored.begin())]);
}

static json pref_value_schema(const PrefSpec& s)
{
    json v = {{"description", s.label + ": " + s.description}};
    if (s.type == "boolean")
        v["type"] = "boolean";
    else if (s.type == "integer") {
        v["type"]    = "integer";
        v["minimum"] = s.minimum;
        v["maximum"] = s.maximum;
    } else if (s.type == "number") {
        v["type"]    = "number";
        v["minimum"] = s.min_number;
        v["maximum"] = s.max_number;
    } else {
        v["type"] = "string";
        v["enum"] = s.values;
    }
    return v;
}

json prefs_set_schema()
{
    json props = json::object();
    for (const PrefSpec& s : pref_allowlist())
        if (s.set_by.empty())
            props[s.name] = pref_value_schema(s);
    return {{"type", "object"},
            {"additionalProperties", false},
            {"required", json::array({"values"})},
            {"properties",
             {{"values", {{"type", "object"}, {"additionalProperties", false}, {"properties", props},
                          {"description", "Preferences to change, by name; only these names exist."}}}}}};
}

json prefs_get_schema()
{
    std::vector<std::string> names;
    for (const PrefSpec& s : pref_allowlist())
        names.push_back(s.name);
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"names", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}, {"enum", names}}},
                         {"description", "Which preferences to read; absent = all of them."}}}}}};
}

namespace {

// What AppConfig holds for `s`. The settings mode reads through GUI_App, which maps an old "develop"
// entry to advanced, as the mode switch shows it.
std::string stored_value(const PrefSpec& s, const AppConfig& cfg)
{
    if (s.config_key == "user_mode")
        return wxGetApp().get_saved_mode_str();
    return cfg.get(s.config_key);
}

bool developer_mode() { return wxGetApp().get_mode() == comDevelop; }

json describe(const PrefSpec& s, const AppConfig& cfg)
{
    json d = {{"value", pref_value_from_stored(s, stored_value(s, cfg))}, {"label", s.label}, {"type", s.type}, {"description", s.description}};
    if (s.type == "enum")
        d["values"] = s.values;
    if (s.type == "integer") {
        d["minimum"] = s.minimum;
        d["maximum"] = s.maximum;
    }
    if (s.type == "number") {
        d["minimum"] = s.min_number;
        d["maximum"] = s.max_number;
    }
    if (!s.set_by.empty())
        d["setWith"] = s.set_by;
    if (s.config_key == "user_mode" && developer_mode()) {
        // Developer mode shows every setting and locks the switch.
        d["effective"]      = "develop";
        d["disabledReason"] = "developer mode is on; turn it off in Preferences to change the settings mode";
    }
    return d;
}

InvokeResult run_get(InvokeContext& ctx)
{
    const AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "preferences are not loaded");
    json out = json::object();
    if (ctx.params.contains("names")) {
        for (const json& n : ctx.params["names"])
            if (const PrefSpec* s = find_pref(n.get<std::string>()))
                out[s->name] = describe(*s, *cfg);
    } else {
        for (const PrefSpec& s : pref_allowlist())
            out[s.name] = describe(s, *cfg);
    }
    return InvokeResult::ok({{"prefs", out}});
}

// What the Preferences dialog does when the row changes (Preferences.cpp), beyond storing it.
void apply_side_effect(const PrefSpec& s, const std::string& stored, bool& refresh_ui)
{
    GUI_App& app    = wxGetApp();
    Plater*  plater = app.plater();
    if (s.config_key == "color_scheme") {
        app.apply_color_scheme(stored);
    } else if (s.config_key == "opaque_overlays") {
        if (app.imgui())
            app.imgui()->set_opaque_overlays(stored == "true");
    } else if (s.config_key == "preview_color_vision") {
        if (plater && plater->get_preview_canvas3D())
            plater->get_preview_canvas3D()->get_gcode_viewer().apply_color_vision_palette();
    } else if (s.config_key == "preview_dim_previous_layers") {
        if (plater && plater->get_preview_canvas3D())
            plater->get_preview_canvas3D()->get_gcode_viewer().set_dim_previous_layers(stored == "true");
    } else if (s.config_key == "preview_dim_previous_layers_brightness") {
        if (plater && plater->get_preview_canvas3D())
            plater->get_preview_canvas3D()->get_gcode_viewer().set_dim_previous_layers_brightness(0.01f * float(std::stoi(stored)));
    } else if (s.config_key == "use_inches" || s.config_key == "use_free_camera" || s.config_key == "camera_navigation_style") {
        refresh_ui = true;
    }
}

InvokeResult run_set(InvokeContext& ctx)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "preferences are not loaded");
    const json& values = ctx.params.at("values");
    if (!values.is_object() || values.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "values must name at least one preference", {{"path", "/params/values"}});

    // Check every value before writing any, so a refused one leaves all preferences as they were.
    std::vector<std::pair<const PrefSpec*, std::string>> writes;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const PrefSpec* s = find_pref(it.key());
        if (s == nullptr) // the schema already refuses these; kept for direct callers
            return InvokeResult::error(WAB_BAD_PARAMS, "\"" + it.key() + "\" is not a preference that can be changed automatically",
                                       {{"path", "/params/values/" + it.key()}});
        if (!s->set_by.empty()) // likewise
            return InvokeResult::error(WAB_BAD_PARAMS, "\"" + it.key() + "\" is changed with " + s->set_by,
                                       {{"path", "/params/values/" + it.key()}});
        // The tabs' mode switch is disabled in developer mode (Tab.cpp, ParamsPanel.cpp).
        if (s->config_key == "user_mode" && developer_mode())
            return InvokeResult::error(WAB_ACTION_DISABLED, "developer mode is on, which shows every setting; the settings mode cannot change",
                                       {{"path", "/params/values/" + it.key()}, {"reason", "developer-mode"}});
        std::string stored, message;
        if (!pref_value_to_stored(*s, it.value(), stored, message))
            return InvokeResult::error(WAB_BAD_PARAMS, message, {{"path", "/params/values/" + it.key()}});
        writes.emplace_back(s, stored);
    }

    json changed    = json::object();
    bool refresh_ui = false, backup = false;
    for (const auto& [s, stored] : writes) {
        const std::string before = stored_value(*s, *cfg);
        if (before == stored)
            continue;
        if (s->config_key == "user_mode") {
            // The mode switch: stores user_mode and refreshes every tab, the sidebar and the gizmos.
            wxGetApp().save_mode(stored == "expert" ? comExpert : stored == "advanced" ? comAdvanced : comSimple);
        } else {
            cfg->set(s->config_key, stored);
            apply_side_effect(*s, stored, refresh_ui);
        }
        backup = backup || s->config_key == "backup_switch" || s->config_key == "backup_interval";
        changed[s->name] = {{"previous", pref_value_from_stored(*s, before)}, {"value", pref_value_from_stored(*s, stored)}};
    }
    if (backup) {
        // As the Auto backup row does: the interval runs only while the switch is on.
        long interval = 10;
        try {
            interval = std::stol(cfg->get("backup_interval"));
        } catch (...) {
        }
        Slic3r::set_backup_interval(cfg->get_bool("backup_switch") ? interval : 0);
    }
    if (!changed.empty())
        cfg->save();
    if (refresh_ui)
        wxGetApp().update_ui_from_settings();
    if (Plater* plater = wxGetApp().plater())
        if (GLCanvas3D* canvas = plater->get_current_canvas3D()) {
            canvas->set_as_dirty();
            canvas->request_extra_frame();
        }
    InvokeResult r = InvokeResult::ok({{"changed", changed}});
    if (changed.empty())
        r.with_effects({});
    return r;
}

// Preferences > "Remove mixed temperature restriction": the same switch, and the same warning the
// dialog shows before turning it on, as the native confirmation. Turning it off restores the check
// and needs no confirmation.
InvokeResult run_allow_mixed_temperatures(InvokeContext& ctx)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "preferences are not loaded");
    const std::string key    = "enable_high_low_temp_mixed_printing";
    const bool        allow  = ctx.params.value("allow", false);
    const bool        before = cfg->get_bool(key);
    if (allow == before)
        return InvokeResult::ok({{"allow", allow}, {"changed", false}}).with_effects({});
    if (allow) {
        ConfirmRequest req;
        req.title        = "Remove mixed temperature restriction";
        req.message      = "Let plates mix filaments with very different printing temperatures? The slicer stops refusing them. "
                           "Such prints can clog the extruder, damage the nozzle or bond layers poorly.";
        req.accept_label = "Allow";
        req.destructive  = false;
        if (!ctx.confirm(req))
            return InvokeResult::error(WAB_USER_CANCELLED, "the person kept the mixed temperature restriction");
    }
    cfg->set_bool(key, allow);
    cfg->save();
    // The check runs when the plate is validated (Plater reads the preference there): validate again
    // so a plate refused for it is offered for slicing, or refused again.
    if (Plater* plater = wxGetApp().plater())
        plater->schedule_background_process();
    return InvokeResult::ok({{"allow", allow}, {"changed", true}});
}

InvokeResult run_check_updates(InvokeContext& ctx)
{
    ConfirmRequest req;
    req.title        = "Check for Updates";
    req.message      = "Check the Unlayered update site for a newer version of the slicer now?";
    req.accept_label = "Check";
    req.destructive  = false;
    if (!ctx.confirm(req))
        return InvokeResult::error(WAB_USER_CANCELLED, "the person declined the update check");
    // Help > Check for Updates: by_user = 1, so the slicer tells the person the outcome itself
    // ("up to date", or the update offer with its own buttons). Under the dev-only E2E auto-answer
    // nobody could close that window, so the check runs as the silent startup one.
    wxGetApp().check_new_version_sf(e2e_autoconfirm() == AutoConfirm::Off ? 1 : 0);
    return InvokeResult::ok({{"started", true}, {"note", "the slicer shows the result to the person"}});
}

} // namespace

void register_preference_actions(Catalog& c)
{
    std::vector<std::string> pref_rows;
    for (const PrefSpec& s : pref_allowlist())
        if (s.preferences_row && s.set_by.empty() &&
            std::find(pref_rows.begin(), pref_rows.end(), "Preferences.cpp::pref::" + s.label) == pref_rows.end())
            pref_rows.push_back("Preferences.cpp::pref::" + s.label);
    // The menu item itself opens the dialog; both actions together stand for it.
    {
        ActionDescriptor d;
        d.id       = "prefs.get";
        d.label    = "Preferences";
        d.summary  = "Read the preferences that can be changed automatically (units, appearance, camera and mouse, behaviour), with their allowed values.";
        d.risk     = Risk::Read;
        d.params   = prefs_get_schema();
        d.effects  = {};
        d.ui       = {"MainFrame.cpp::menu::Preferences"};
        d.examples = json::array({json{{"target", nullptr}, {"params", json::object()}}});
        c.add(std::move(d), run_get);
    }
    {
        ActionDescriptor d;
        d.id      = "prefs.set";
        d.label   = "Preferences";
        d.summary = "Change cosmetic and behaviour preferences (allowlist only: units, default page, load behaviour, colour scheme, "
                    "colour vision, overlays, preview dimming, camera and mouse, auto arrange, auto flush, auto slice, STEP import "
                    "meshing, remember printer configuration, auto backup, two warning dialogs) and the settings mode "
                    "(simple/advanced/expert). Saved at once.";
        d.risk     = Risk::Ui;
        d.params   = prefs_set_schema();
        d.effects  = {"view"};
        d.ui       = pref_rows;
        d.examples = json::array({json{{"target", nullptr}, {"params", {{"values", {{"zoomToMouse", true}}}}}}});
        c.add(std::move(d), run_set);
    }
    {
        ActionDescriptor d;
        d.id      = "prefs.allowMixedTemperatures";
        d.label   = "Remove mixed temperature restriction";
        d.summary = "Allow or forbid slicing plates that mix filaments with very different temperatures (the Preferences switch). "
                    "Allowing asks the person in a slicer dialog first.";
        d.risk     = Risk::Mutate;
        d.confirm  = Confirm::Native;
        d.params   = {{"type", "object"},
                      {"additionalProperties", false},
                      {"required", json::array({"allow"})},
                      {"properties", {{"allow", {{"type", "boolean"}, {"description", "true lifts the check; false restores it."}}}}}};
        d.effects  = {"settings"};
        d.ui       = {"Preferences.cpp::pref::Remove mixed temperature restriction"};
        d.examples = json::array({json{{"target", nullptr}, {"params", {{"allow", false}}}}});
        c.add(std::move(d), run_allow_mixed_temperatures);
    }
    {
        ActionDescriptor d;
        d.id      = "app.checkUpdates";
        d.label   = "Check for Updates";
        d.summary = "Ask the update site whether a newer version exists, as Help > Check for Updates does; the slicer shows the person the outcome.";
        d.risk     = Risk::OutwardNetwork;
        d.confirm  = Confirm::Native;
        d.effects  = {};
        d.ui       = {"MainFrame.cpp::menu::Check for Updates"};
        d.examples = json::array({json{{"target", nullptr}, {"params", json::object()}}});
        c.add(std::move(d), run_check_updates);
    }
}

}}} // namespace Slic3r::GUI::Actions
