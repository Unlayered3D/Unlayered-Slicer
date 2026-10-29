#include "DiagnosticsAllowlist.hpp"

#include <map>
#include <regex>
#include <set>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>

using json = nlohmann::json;

namespace Slic3r {
namespace DiagnosticsAllowlist {

namespace {

// Event -> the properties it may have. Keep in step with USAGE_DIAGNOSTICS.md.
const std::map<std::string, std::set<std::string>> &events()
{
    static const std::map<std::string, std::set<std::string>> table = {
        // System and runtime (GUI_App record_usage_app_start).
        {"app_start", {"os_version", "arch", "language", "cpu_cores", "ram_gb", "first_run", "stealth", "dark_mode",
                       "bambu_plugin", "bambu_plugin_version"}},
        // A previous session that ended without closing (UsageTelemetry::start): the module file name
        // and offset, the exception code, what the app was doing. Never a path or a dump.
        {"crash", {"version", "phase", "exception", "at", "recorded_by", "uptime_s"}},
        {"session_end", {"duration_s"}},
        // Slicing (Plater record_usage_slice): presets by their system names, setting names and values
        // (private keys and path-like values already dropped there), part measurements (never a mesh),
        // results, timings and redacted errors.
        {"slice", {"result", "duration_ms", "plates", "objects", "instances", "printer", "printer_vendor", "process",
                   "filaments", "filament_types", "modified_keys", "settings", "settings_more", "parts", "slice_id",
                   "print_s", "model_mm3", "support_mm3", "features", "error_type", "error", "crash_at"}},
        {"export", {"kind", "removable", "all_plates", "host_type"}},
        {"preset_selected", {"kind", "name", "system"}},
        {"print_rating", {"slice_id", "rating", "via", "problems"}},
        // The updater (AutoUpdater).
        {"update_applied", {"to", "from", "outcome", "launch"}},
        {"update_offered", {"to", "by_user", "signed_in", "action", "why"}},
        {"update_download", {"to", "result", "seconds", "reason", "http_status"}},
        {"account_signin", {"result", "from"}},
        // The printer Config updater (SysFilesUpdateJob): the configuration versions, never the
        // printer's address, name or files.
        {"printer_config_update", {"to", "from", "outcome", "files", "rolled_back", "firmware"}},
    };
    return table;
}

// Longer strings are allowed only where a value is naturally long.
size_t max_len_for(const std::string &event, const std::string &key)
{
    if (event == "slice" && (key == "settings" || key == "parts"))
        return 100;
    // A crash's "at" is the faulting frame plus up to five callers ("UnlayeredSlicer.dll+0x4862937>..."),
    // about 30 characters a frame; at 120 the whole value was dropped and no 0.1.6/0.1.7 crash inside our
    // own code reported where it happened. UsageTelemetry cuts it to 190 at a frame boundary.
    if (key == "error" || key == "why" || (event == "crash" && key == "at"))
        return 190;
    return 120;
}

// List-valued properties may have this many items at most.
constexpr size_t MAX_LIST = 80;

bool sanitize(const json &value, size_t max_len, json &out)
{
    if (value.is_boolean() || value.is_number()) {
        out = value;
        return true;
    }
    if (value.is_string()) {
        if (!safe_string(value.get<std::string>(), max_len))
            return false;
        out = value;
        return true;
    }
    if (value.is_array()) {
        out = json::array();
        for (const json &item : value) {
            if (out.size() >= MAX_LIST)
                break;
            json clean;
            // Lists hold plain values only; an unsafe item is dropped, the rest stay.
            if (!item.is_array() && !item.is_object() && sanitize(item, max_len, clean))
                out.push_back(std::move(clean));
        }
        return true;
    }
    return false; // objects and nulls are never sent as values
}

} // namespace

bool known_event(const std::string &name) { return events().count(name) > 0; }

bool safe_string(const std::string &value, size_t max_len)
{
    if (value.size() > max_len)
        return false;
    if (value.find('\n') != std::string::npos || value.find('\r') != std::string::npos)
        return false;
    // Paths: Windows drive or UNC, any backslash, a Unix absolute path or home directory (also as a
    // "key=/value" setting item), a URL.
    static const std::regex path(R"((^[A-Za-z]:)|([=\s][A-Za-z]:/)|\\|(^/)|(^~/)|(=/)|(=~/)|(://)|(/Users/)|(/home/))");
    if (std::regex_search(value, path))
        return false;
    // File names of the person's work (models, projects, G-code, images).
    static const std::regex file(
        R"(\.(stl|3mf|obj|step|stp|amf|ply|svg|drc|gcode|bgcode|gco|png|jpe?g|bmp|zip|ini|json|f3d|iges?|sldprt|scad)($|[\s"'),;]))",
        std::regex::icase);
    if (std::regex_search(value, file))
        return false;
    // E-mail addresses (a preset name like "Generic PLA @BBL X1C" is not one).
    static const std::regex email(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z0-9.-]+)");
    if (std::regex_search(value, email))
        return false;
    return true;
}

bool allowed_count_key(const std::string &key)
{
    static const std::regex re(R"(^(tab|gizmo|import|parts):[A-Za-z0-9_.-]{1,60}$)");
    return std::regex_match(key, re);
}

bool filter(const std::string &name, const json &props, const json &counts, json &props_out, json &counts_out)
{
    props_out  = json::object();
    counts_out = json::object();
    auto it    = events().find(name);
    if (it == events().end()) {
        BOOST_LOG_TRIVIAL(warning) << "usage diagnostics: event \"" << name << "\" is not on the allowlist; dropped";
        return false;
    }
    if (props.is_object())
        for (const auto &[key, value] : props.items()) {
            if (!it->second.count(key)) {
                BOOST_LOG_TRIVIAL(warning) << "usage diagnostics: " << name << "." << key << " is not on the allowlist; dropped";
                continue;
            }
            json clean;
            if (sanitize(value, max_len_for(name, key), clean))
                props_out[key] = std::move(clean);
            else
                BOOST_LOG_TRIVIAL(info) << "usage diagnostics: " << name << "." << key << " did not look safe to send; dropped";
        }
    if (counts.is_object())
        for (const auto &[key, value] : counts.items())
            if (allowed_count_key(key) && value.is_number_integer() && value.get<long long>() >= 0)
                counts_out[key] = value;
    return true;
}

} // namespace DiagnosticsAllowlist
} // namespace Slic3r
