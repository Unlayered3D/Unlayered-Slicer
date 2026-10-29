#include "WebAppPresetShare.hpp"
#include "WebAppDialogs.hpp"

#include "WebAppBridge.hpp" // WebAppBridgeError
#include "WebAppSettings.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {
namespace WebAppPresetShare {

// ---------------------------------------------------------------------------------------------
// Pure policy
// ---------------------------------------------------------------------------------------------

namespace {

// The M-codes a printer profile's custom G-code may contain but should never run unannounced:
// they reach past the print into the firmware, the SD card, the network or the machine's safety
// limits. The install dialog lists every line that uses one. Adding to this table is cheap.
struct RiskyMCode
{
    int         code;
    const char* reason;
};
const RiskyMCode RISKY_MCODES[] = {
    {997, "firmware update"},
    {30, "deletes a file"},
    {28, "writes a file"},
    {29, "writes a file"},
    {559, "uploads/writes a file"},
    {560, "uploads/writes a file"},
    {500, "saves settings permanently"},
    {502, "resets settings to defaults"},
    {552, "changes network settings"},
    {553, "changes network settings"},
    {554, "changes network settings"},
    {587, "changes network settings"},
    {588, "changes network settings"},
    {589, "changes network settings"},
    {564, "changes axis-limit enforcement"},
    {302, "allows cold extrusion"},
    {143, "changes a heater's temperature limit"},
    {906, "changes motor current"},
    {913, "changes motor current"},
    {208, "changes axis travel limits"},
    {669, "changes kinematics"},
    {584, "changes drive mapping"},
    {999, "restarts the controller"},
};

const char* risky_reason(int code)
{
    for (const RiskyMCode& entry : RISKY_MCODES)
        if (entry.code == code)
            return entry.reason;
    return nullptr;
}

// An M-code as a whole token: not glued to a preceding word character, leading zeros allowed, and
// the number not continuing into more digits or a decimal part. "{endif}M500" and "m0500" match;
// "XM500", "M5000" and "M500.1" do not.
const std::regex& mcode_regex()
{
    static const std::regex re(R"((^|[^A-Za-z0-9_])[Mm]0*(\d+)(?![0-9.]))");
    return re;
}

// M98 followed by its P parameter, quoted (RRF 3) or bare.
const std::regex& m98_regex()
{
    static const std::regex re(R"((^|[^A-Za-z0-9_])[Mm]0*98(?![0-9.])(.*))");
    return re;
}
const std::regex& p_param_regex()
{
    static const std::regex re(R"re([Pp]\s*"([^"]*)"|[Pp]([^\s"{};]+))re");
    return re;
}

// The text of a G-code option, whatever its shape: one string, or one per extruder/filament.
bool gcode_option_text(const ConfigBase& config, const std::string& key, std::string& text)
{
    if (!boost::algorithm::ends_with(key, "_gcode"))
        return false;
    const ConfigOption* opt = config.option(key);
    if (opt == nullptr)
        return false;
    if (const auto* str = dynamic_cast<const ConfigOptionString*>(opt); str != nullptr) {
        text = str->value;
        return true;
    }
    if (const auto* strs = dynamic_cast<const ConfigOptionStrings*>(opt); strs != nullptr) {
        text.clear();
        for (const std::string& value : strs->values) {
            text += value;
            text += '\n';
        }
        return true;
    }
    return false;
}

// Split into lines, each with its ';' comment removed and trimmed; empty lines are dropped.
std::vector<std::string> code_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream       in(text);
    std::string              line;
    while (std::getline(in, line)) {
        if (const size_t semicolon = line.find(';'); semicolon != std::string::npos)
            line.erase(semicolon);
        boost::algorithm::trim(line);
        if (!line.empty())
            lines.push_back(line);
    }
    return lines;
}

std::string shorten(const std::string& line, size_t max_len = 160)
{
    if (line.size() <= max_len)
        return line;
    return line.substr(0, max_len) + "...";
}

} // namespace

const std::vector<std::string>& stripped_keys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k = WebAppSettings::WAB_SECRET_KEYS;
        // Absolute paths on the author's disk ("C:\Users\<them>\Downloads\bed.png"): meaningless on
        // the installer's machine, and a path leak on the author's.
        k.emplace_back("bed_custom_texture");
        k.emplace_back("bed_custom_model");
        return k;
    }();
    return keys;
}

size_t sanitise_printer_preset(DynamicPrintConfig& config)
{
    size_t erased = 0;
    for (const std::string& key : stripped_keys())
        if (config.erase(key))
            ++erased;
    return erased;
}

void scan_gcode_text(const std::string& key, const std::string& text, std::vector<FlaggedLine>& out)
{
    for (const std::string& line : code_lines(text)) {
        std::vector<int> codes;
        for (std::sregex_iterator it(line.begin(), line.end(), mcode_regex()), end; it != end; ++it) {
            int code = -1;
            try {
                code = std::stoi((*it)[2].str());
            } catch (...) {
                continue; // absurdly long digit run: not a code in the table
            }
            if (risky_reason(code) != nullptr && std::find(codes.begin(), codes.end(), code) == codes.end())
                codes.push_back(code);
        }
        if (codes.empty())
            continue;
        std::string reasons;
        for (int code : codes) {
            if (!reasons.empty())
                reasons += "; ";
            reasons += std::string(risky_reason(code)) + " (M" + std::to_string(code) + ")";
        }
        out.push_back({key, shorten(line), reasons});
    }
}

std::vector<FlaggedLine> scan_custom_gcode(const ConfigBase& config)
{
    std::vector<std::string> keys = config.keys();
    std::sort(keys.begin(), keys.end());
    std::vector<FlaggedLine> flagged;
    std::string              text;
    for (const std::string& key : keys)
        if (gcode_option_text(config, key, text))
            scan_gcode_text(key, text, flagged);
    return flagged;
}

void collect_called_macros(const std::string& text, std::vector<std::string>& out)
{
    for (const std::string& line : code_lines(text)) {
        for (std::sregex_iterator it(line.begin(), line.end(), m98_regex()), end; it != end; ++it) {
            const std::string rest = (*it)[2].str();
            std::smatch       p;
            if (!std::regex_search(rest, p, p_param_regex()))
                continue;
            std::string file = p[1].matched ? p[1].str() : p[2].str();
            boost::algorithm::trim(file);
            if (!file.empty() && std::find(out.begin(), out.end(), file) == out.end())
                out.push_back(file);
        }
    }
    std::sort(out.begin(), out.end());
}

std::vector<std::string> called_macros(const ConfigBase& config)
{
    std::vector<std::string> macros;
    std::string              text;
    for (const std::string& key : config.keys())
        if (gcode_option_text(config, key, text))
            collect_called_macros(text, macros);
    return macros;
}

std::vector<std::string> format_flagged(const std::vector<FlaggedLine>& flagged, size_t cap)
{
    static const char* const EM_DASH = "\xE2\x80\x94";
    std::vector<std::string> lines;
    for (size_t i = 0; i < flagged.size() && i < cap; ++i)
        lines.push_back(flagged[i].key + ": " + flagged[i].line + " " + EM_DASH + " " + flagged[i].reasons);
    if (flagged.size() > cap)
        lines.push_back("and " + std::to_string(flagged.size() - cap) + " more");
    return lines;
}

bool preset_name_usable(const std::string& name)
{
    if (name.empty() || name.size() > 200 || name == "." || name == "..")
        return false;
    if (boost::algorithm::trim_copy(name).empty())
        return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || std::strchr("<>:\"/\\|?*", c) != nullptr)
            return false;
    return true;
}

// ---------------------------------------------------------------------------------------------
// GUI thread
// ---------------------------------------------------------------------------------------------

namespace {

std::string path_to_utf8(const fs::path& path)
{
#ifdef _WIN32
    return std::string(wxString(path.wstring()).ToUTF8().data());
#else
    return path.string();
#endif
}

fs::path path_from_utf8(const std::string& utf8_path)
{
#ifdef _WIN32
    return fs::path(wxString::FromUTF8(utf8_path.c_str()).ToStdWstring());
#else
    return fs::path(utf8_path);
#endif
}

std::string random_hex(size_t bytes)
{
    std::random_device rd;
    std::ostringstream oss;
    for (size_t i = 0; i < bytes; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (rd() & 0xFF);
    return oss.str();
}

wxString num(double value)
{
    // At most two decimals, no trailing zeros: 0.4, 250, 0.25.
    wxString text = wxString::Format("%.2f", value);
    while (text.Contains(".") && (text.EndsWith("0") || text.EndsWith(".")))
        text.RemoveLast();
    return text;
}

double opt_float(const DynamicPrintConfig& config, const char* key, double fallback = 0.)
{
    const ConfigOption* opt = config.option(key);
    return opt != nullptr ? opt->getFloat() : fallback;
}

// The label an enum option shows in the GUI for the value `config` holds, or the stored value.
wxString enum_label(const DynamicPrintConfig& config, const char* key)
{
    if (config.option(key) == nullptr)
        return wxString();
    const std::string      value = config.opt_serialize(key);
    const ConfigOptionDef* def   = print_config_def.get(key);
    if (def != nullptr) {
        const auto it = std::find(def->enum_values.begin(), def->enum_values.end(), value);
        if (it != def->enum_values.end()) {
            const size_t idx = size_t(it - def->enum_values.begin());
            if (idx < def->enum_labels.size() && !def->enum_labels[idx].empty())
                return _L(def->enum_labels[idx]);
        }
    }
    return wxString::FromUTF8(value.c_str());
}

// What the confirmation dialog says about the machine, from the profile as it will be installed.
wxString machine_summary(const DynamicPrintConfig& cfg)
{
    wxString text;
    const bool nonplanar = cfg.has("enable_nonplanar") && cfg.opt_bool("enable_nonplanar");

    wxString type = _L("3-axis");
    if (nonplanar) {
        const std::string axes = cfg.has("axis_selection") ? cfg.opt_serialize("axis_selection") : std::string();
        type = axes == "virtual" ? _L("virtual 5-axis") : wxString::Format(_L("5-axis (%s)"), enum_label(cfg, "axis_selection"));
    }
    text += wxString::Format(_L("Printer type: %s"), type) + "\n";

    if (const auto* area = cfg.option<ConfigOptionPoints>("printable_area"); area != nullptr && !area->values.empty()) {
        const BoundingBoxf bb(area->values);
        const Vec2d        size = bb.size();
        text += wxString::Format(_L("Bed: %s x %s mm, %s mm tall"), num(size.x()), num(size.y()),
                                 num(opt_float(cfg, "printable_height"))) + "\n";
    }

    if (const auto* nozzles = cfg.option<ConfigOptionFloats>("nozzle_diameter"); nozzles != nullptr && !nozzles->values.empty()) {
        wxString list;
        for (double d : nozzles->values) {
            if (!list.empty())
                list += ", ";
            list += num(d);
        }
        text += wxString::Format(_L("Nozzle diameter: %s mm"), list) + "\n";
    }

    if (cfg.option("gcode_flavor") != nullptr)
        text += wxString::Format(_L("G-code flavor: %s"), enum_label(cfg, "gcode_flavor")) + "\n";

    if (nonplanar) {
        const double height = opt_float(cfg, "nonplanar_hotend_cone_height");
        if (height <= 0.) {
            text += _L("Hotend envelope: off") + "\n";
        } else {
            text += wxString::Format(_L("Hotend envelope: +X %s°, -X %s°, +Y %s°, -Y %s°, %s mm tall"),
                                     num(opt_float(cfg, "nonplanar_hotend_angle_xpos")),
                                     num(opt_float(cfg, "nonplanar_hotend_angle_xneg")),
                                     num(opt_float(cfg, "nonplanar_hotend_angle_ypos")),
                                     num(opt_float(cfg, "nonplanar_hotend_angle_yneg")), num(height)) + "\n";
        }
        const double gantry = opt_float(cfg, "nonplanar_gantry_height");
        text += (gantry > 0. ? wxString::Format(_L("Gantry height: %s mm"), num(gantry)) : _L("Gantry height: not measured")) + "\n";
    }
    return text;
}

// The one confirmation an install waits on. A MsgDialog, so it looks like every other question
// the application asks; the long part goes in a scrolling text control rather than a label tall
// enough to push the buttons off the screen.
class PrinterProfileInstallDialog : public MsgDialog
{
public:
    PrinterProfileInstallDialog(wxWindow*       parent,
                                const wxString& title,
                                const wxString& summary,
                                const wxString& review,
                                const wxString& option_label)
        : MsgDialog(parent, title, wxEmptyString, wxICON_QUESTION)
    {
        auto* text = new wxStaticText(this, wxID_ANY, summary);
        text->Wrap(FromDIP(520));
        content_sizer->Add(text, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

        auto* code = new wxTextCtrl(this, wxID_ANY, review, wxDefaultPosition, wxSize(FromDIP(520), FromDIP(170)),
                                    wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxTE_RICH2);
        code->SetFont(wxGetApp().code_font());
        content_sizer->Add(code, 1, wxEXPAND | wxBOTTOM, FromDIP(10));

        if (!option_label.empty())
            show_dsa_button(option_label);
        add_button(wxID_OK, true, _L("Install"));
        add_button(wxID_CANCEL, false, _L("Cancel"));
        finalize();
    }

    bool option_checked() { return get_checkbox_state(); }
};

// A parent a user preset may inherit from: a vendor (system) preset, or one from an installed
// bundle. Anything else -- another user preset, the profile itself -- is no parent at all.
const Preset* usable_parent(PresetCollection& printers, const std::string& inherits)
{
    if (inherits.empty())
        return nullptr;
    const Preset* parent = printers.find_preset2(inherits, true);
    if (parent == nullptr || !(parent->is_system || parent->is_from_bundle()))
        return nullptr;
    return parent;
}

} // namespace

std::string edited_printer_preset_name()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    return bundle != nullptr ? get_preset_bare_name(bundle->printers.get_edited_preset().name) : std::string();
}

ExportResult export_edited_printer_preset(const fs::path& out_path)
{
    ExportResult  result;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr) {
        result.code    = WAB_INTERNAL;
        result.message = "no preset bundle";
        return result;
    }
    PresetCollection& printers = bundle->printers;
    const Preset&     edited   = printers.get_edited_preset();
    if (edited.is_system || edited.is_default) {
        result.code    = WAB_NOT_FOUND;
        result.message = "Save this printer as your own preset first: system printer presets can't be shared.";
        return result;
    }

    // The edited copy is the whole configuration (a user preset is held flattened in memory; only
    // its file on disk is a diff against the parent), so this is what ExportConfigsDialog's
    // earse_preset_fields_for_safe writes too -- minus its dialog, and minus the .info sidecar.
    DynamicPrintConfig config = edited.config;
    sanitise_printer_preset(config);
    result.name     = get_preset_bare_name(edited.name);
    result.inherits = edited.inherits();
    result.dirty    = printers.current_is_dirty();
    config.set_key_value("printer_settings_id", new ConfigOptionString(result.name));
    config.set_key_value("inherits", new ConfigOptionString(result.inherits));

    // The importer refuses a file whose version it cannot parse. A user preset carries the version
    // of the profile it was made from; fall back to this build's when it has none.
    std::string version = edited.version.valid() ? edited.version.to_string() : std::string(SLIC3R_VERSION);
    if (!Semver::parse(version))
        version = SLIC3R_VERSION;

    boost::system::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);
    try {
        // Throws on a string that is not valid UTF-8 (nlohmann::json::dump), which an old preset can hold.
        config.save_to_json(path_to_utf8(out_path), result.name, "User", version);
    } catch (const std::exception& e) {
        result.code    = WAB_INTERNAL;
        result.message = std::string("the printer preset could not be written: ") + e.what();
        return result;
    }
    result.code = WAB_OK;
    return result;
}

InstallResult install_printer_preset(const fs::path& file, const wxString& tab_title, const fs::path& work_dir)
{
    InstallResult result;
    auto          fail = [&result](int code, const std::string& message) {
        result.ok    = false;
        result.code  = code;
        result.error = message;
        return result;
    };

    GUI_App&      app       = wxGetApp();
    PresetBundle* bundle    = app.preset_bundle;
    MainFrame*    mainframe = app.mainframe;
    Tab*          tab       = app.get_tab(Preset::TYPE_PRINTER);
    if (bundle == nullptr || mainframe == nullptr || tab == nullptr || app.app_config == nullptr)
        return fail(WAB_INTERNAL, "no preset bundle");
    // The download took a while; the world may have moved on since the call was accepted.
    if (!mainframe->IsEnabled())
        return fail(WAB_BUSY, WebAppDialogs::busy_message());
    if (app.plater() != nullptr && app.plater()->is_background_process_slicing())
        return fail(WAB_BUSY, "the slicer is busy slicing");

    // a. Parse. load_from_json runs set_deserialize's handle_legacy per key and then
    //    handle_legacy_composite (virtual_5axis -> axis_selection, migrate_legacy_hotend_cone, ...),
    //    exactly as loading a preset from disk does.
    DynamicPrintConfig                 config;
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    ConfigSubstitutionContext          context(ForwardCompatibilitySubstitutionRule::Enable);
    const int                          loaded  = config.load_from_json(path_to_utf8(file), context, true, key_values, reason);
    const std::string                  name    = key_values[BBL_JSON_KEY_NAME];
    const std::string                  version = key_values[BBL_JSON_KEY_VERSION];
    if (loaded != 0 || !config.has("printer_settings_id") || name.empty() || !Semver::parse(version)) {
        BOOST_LOG_TRIVIAL(info) << "WebAppPresetShare: rejected a download as a printer profile: " << reason;
        return fail(WAB_UNSUPPORTED_TYPE, "not a printer profile");
    }
    // This slicer drives FFF printers only; an SLA "printer" would be built on the SLA defaults.
    if (config.has("printer_technology") && config.opt_serialize("printer_technology") != "FFF")
        return fail(WAB_UNSUPPORTED_TYPE, "not a printer profile");
    if (!preset_name_usable(name))
        return fail(WAB_BAD_PARAMS, "the profile's name can't be used as a preset name");

    // b. Sanitise, whatever the file says. A shared profile never sets the installer's print host.
    sanitise_printer_preset(config);
    config.set_key_value("printer_settings_id", new ConfigOptionString(name));

    PresetCollection& printers = bundle->printers;
    const Preset*     existing = printers.find_preset(name, false);
    if (existing != nullptr && (existing->is_system || existing->is_default))
        return fail(WAB_BAD_PARAMS, "a system printer preset already has this name");
    const bool replaces          = existing != nullptr;
    const bool replaces_selected = replaces && printers.get_selected_preset_name() == name;

    // c. The parent. Kept when it is installed here; offered when a vendor this build ships has
    //    it; otherwise dropped, which is safe because a shared profile is flattened -- complete
    //    without its parent -- and imports as a custom root.
    const std::string inherits = config.has("inherits") ? config.opt_string("inherits") : std::string();
    const Preset*     parent   = usable_parent(printers, inherits);
    std::string       vendor, model, variant;
    bool              vendor_installed = false;
    if (!inherits.empty() && parent == nullptr) {
        vendor           = PresetBundle::find_resource_vendor_for_preset(PRESET_PRINTER_NAME, inherits, &model, &variant);
        vendor_installed = !vendor.empty() && bundle->vendors.find(vendor) != bundle->vendors.end();
        if (vendor_installed && model.empty())
            vendor.clear(); // the vendor is here and simply has no such printer: nothing to offer
    }

    // d. The one confirmation.
    DynamicPrintConfig effective = parent != nullptr ? parent->config : printers.default_preset_for(config).config;
    effective.apply(config);

    wxString summary = wxString::Format(_L("Name: %s"), wxString::FromUTF8(name.c_str())) + "\n";
    if (replaces) {
        summary += _L("Replaces your existing printer preset of the same name.");
        if (replaces_selected && printers.current_is_dirty())
            summary += " " + _L("Your unsaved changes to it will be discarded.");
        summary += "\n";
    }
    summary += "\n" + machine_summary(effective) + "\n";
    const wxString parent_name = wxString::FromUTF8(inherits.c_str());
    if (parent != nullptr)
        summary += wxString::Format(_L("Based on \"%s\"."), parent_name);
    else if (!inherits.empty() && !vendor.empty())
        summary += wxString::Format(_L("\"%s\" isn't installed here, so it installs as a standalone printer, unless you also "
                                       "install the %s printer profiles it is based on (below)."),
                                    parent_name, wxString::FromUTF8(vendor.c_str()));
    else if (!inherits.empty())
        summary += wxString::Format(_L("\"%s\" isn't installed here, so it installs as a standalone printer."), parent_name);
    else
        summary += _L("A standalone printer (not based on a vendor profile).");
    summary += "\n" + _L("Print-host addresses and credentials are never installed from a shared profile.");

    wxString                       review  = _L("Custom G-code review") + "\n";
    const std::vector<FlaggedLine> flagged = scan_custom_gcode(config);
    if (flagged.empty()) {
        review += _L("Nothing in this profile's custom G-code updates firmware, writes or deletes files, changes saved, "
                     "network, motor or safety settings, or restarts the controller.") + "\n";
    } else {
        review += _L("Look at these lines before you install:") + "\n";
        for (const std::string& line : format_flagged(flagged))
            review += wxString::FromUTF8(line.c_str()) + "\n";
    }
    if (const std::vector<std::string> macros = called_macros(config); !macros.empty()) {
        wxString list;
        for (const std::string& macro : macros) {
            if (!list.empty())
                list += ", ";
            list += wxString::FromUTF8(macro.c_str());
        }
        review += "\n" + wxString::Format(_L("Needs these macros on your printer: %s"), list) + "\n";
    }

    const wxString option = vendor.empty() ? wxString() :
                                             wxString::Format(_L("Also install the %s printer profiles"), wxString::FromUTF8(vendor.c_str()));
    bool install_vendor = false;
    {
        WebAppDialogs::HumanOnlyScope persons_install; // the per-install confirmation is the consent
        PrinterProfileInstallDialog dlg(mainframe, wxString::Format(_L("Install printer profile from %s"), tab_title), summary, review,
                                        option);
        if (dlg.ShowModal() != wxID_OK) {
            result.cancelled = true;
            return result;
        }
        install_vendor = !vendor.empty() && dlg.option_checked();
    }
    existing = nullptr; // the pointers above do not survive what follows
    parent   = nullptr;

    // e. Install. First the parent's vendor if the user asked for it -- which reloads every preset,
    //    so unsaved edits get the usual save/discard question before they would be lost.
    std::string final_inherits = inherits;
    if (install_vendor) {
        if (app.has_current_preset_changes() &&
            !app.check_and_save_current_preset_changes(_L("Install printer profiles"),
                                                       _L("Installing printer profiles reloads every preset."), false)) {
            result.cancelled = true;
            return result;
        }
        AppConfig& cfg = *app.app_config;
        if (vendor_installed || Slic3r::install_vendor_bundles_from_resources({vendor})) {
            if (!model.empty() && !cfg.get_variant(vendor, model, variant))
                cfg.set_variant(vendor, model, variant, true);
            cfg.save();
            bundle->load_presets(cfg, ForwardCompatibilitySubstitutionRule::EnableSilent);
        } else {
            BOOST_LOG_TRIVIAL(warning) << "WebAppPresetShare: could not install the " << vendor << " profiles; installing standalone";
        }
    }
    if (usable_parent(printers, final_inherits) == nullptr)
        final_inherits.clear();
    config.set_key_value("inherits", new ConfigOptionString(final_inherits));

    boost::system::error_code ec;
    fs::create_directories(work_dir, ec);
    const fs::path temp = work_dir / path_from_utf8("install-" + random_hex(8) + ".json");
    try {
        config.save_to_json(path_to_utf8(temp), name, "User", version);
    } catch (const std::exception& e) {
        fs::remove(temp, ec);
        return fail(WAB_INTERNAL, std::string("the profile could not be prepared for import: ") + e.what());
    }

    // Whether the preset being replaced is the one in force is decided again here: installing a
    // vendor above reloaded the selection from the configuration.
    const bool                 was_selected = printers.get_selected_preset_name() == name;
    std::vector<std::string>   files{path_to_utf8(temp)};
    PresetBundle::ImportReport report;
    // 1 = "yes": the dialog the user just confirmed already said this replaces their preset.
    bundle->import_presets(files, [](const std::string&) { return 1; }, ForwardCompatibilitySubstitutionRule::Enable,
                           *app.app_config, &report);
    fs::remove(temp, ec);
    if (std::find(report.imported.begin(), report.imported.end(), name) == report.imported.end()) {
        std::string why = "the profile could not be imported";
        if (!report.missing_parents.empty())
            why += ": the printer preset it is based on (\"" + report.missing_parents.front().second + "\") is not installed";
        return fail(WAB_INTERNAL, why);
    }

    // The refresh MainFrame::load_config_file does after an import. When the replaced preset is the
    // one in force, its edited copy still holds the old values (the dialog said they would go).
    if (was_selected)
        printers.discard_current_changes();
    app.load_current_presets();
    bundle->update_compatible(PresetSelectCompatibleType::Always);
    mainframe->update_side_preset_ui();

    // Select it the way slicer.selectPreset does for a printer: Tab::select_preset, which asks the
    // usual save/discard question when the current printer has unsaved edits. The user may cancel
    // that; `selected` then reports the printer that stayed.
    if (!was_selected && printers.get_selected_preset_name() != name)
        tab->select_preset(name);

    result.ok       = true;
    result.code     = WAB_OK;
    result.name     = name;
    result.selected = printers.get_selected_preset_name();
    BOOST_LOG_TRIVIAL(info) << "WebAppPresetShare: installed printer profile \"" << name << "\" (based on \""
                            << final_inherits << "\"), selected \"" << result.selected << "\"";
    return result;
}

} // namespace WebAppPresetShare
} // namespace GUI
} // namespace Slic3r
