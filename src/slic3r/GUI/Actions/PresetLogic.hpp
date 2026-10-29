#pragma once

// Pure helpers behind the preset.*, project.* and file.* actions (docs/bridge-actions/presets.md):
// the config diff, the revert plan, preset-name rules and the export-kind table. No wxWidgets, no
// GUI_App: tests/slic3rutils/test_actions_presets.cpp exercises them directly.

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Config.hpp"

namespace Slic3r { namespace GUI { namespace Actions { namespace PresetLogic {

// One key that differs between two configs. A key present on one side only has the other
// side's value empty and its in_* flag false.
struct KeyDiff
{
    std::string key;
    std::string a;
    std::string b;
    bool        in_a = true;
    bool        in_b = true;
};

// Every key whose value differs between `a` and `b`, sorted by key. `visible` (nullable) drops keys
// the caller may not show (secrets, code). `only` (nullable, non-empty) limits the keys compared.
// Values are the options' .ini serialisation, cut to `max_value_chars`.
inline std::vector<KeyDiff> diff_configs(const ConfigBase& a, const ConfigBase& b,
                                         const std::function<bool(const std::string&)>& visible = {},
                                         const std::vector<std::string>* only = nullptr, size_t max_value_chars = 2000)
{
    std::set<std::string> keys;
    if (only != nullptr && !only->empty()) {
        keys.insert(only->begin(), only->end());
    } else {
        for (const std::string& k : a.keys())
            keys.insert(k);
        for (const std::string& k : b.keys())
            keys.insert(k);
    }
    auto cut = [max_value_chars](std::string s) {
        if (s.size() > max_value_chars)
            s.resize(max_value_chars);
        return s;
    };
    std::vector<KeyDiff> out;
    for (const std::string& key : keys) {
        if (visible && !visible(key))
            continue;
        const ConfigOption* oa = a.option(key);
        const ConfigOption* ob = b.option(key);
        if (oa == nullptr && ob == nullptr)
            continue;
        if (oa != nullptr && ob != nullptr && *oa == *ob)
            continue;
        KeyDiff d;
        d.key  = key;
        d.in_a = oa != nullptr;
        d.in_b = ob != nullptr;
        if (oa != nullptr)
            d.a = cut(oa->serialize());
        if (ob != nullptr)
            d.b = cut(ob->serialize());
        out.push_back(std::move(d));
    }
    return out;
}

// "key#3" -> ("key", 3): one element of a per-extruder / per-variant vector option, as the tabs'
// per-option undo arrows address it. False for a plain key (base = key) or a malformed index.
inline bool split_indexed_key(const std::string& key, std::string& base, size_t& index)
{
    const size_t hash = key.find('#');
    base              = key.substr(0, hash);
    if (hash == std::string::npos)
        return false;
    const std::string digits = key.substr(hash + 1);
    if (digits.empty() || digits.size() > 4 || digits.find_first_not_of("0123456789") != std::string::npos) {
        base.clear();
        return false;
    }
    index = size_t(std::stoul(digits));
    return true;
}

// Which of `keys` a revert to `reference` would rewrite in `edited`, and why the others are left.
// A key may be "key#i": element i of a vector option only.
struct RevertPlan
{
    std::vector<std::string>                         apply;     // differs, writable, reference has it
    std::vector<std::string>                         unchanged; // already equal to the reference
    std::vector<std::pair<std::string, std::string>> skipped;   // (key, reason)
};

inline RevertPlan plan_revert(const ConfigBase& edited, const ConfigBase& reference, const std::vector<std::string>& keys,
                              const std::function<bool(const std::string&, std::string&)>& writable = {})
{
    RevertPlan            plan;
    std::set<std::string> seen;
    for (const std::string& key : keys) {
        if (!seen.insert(key).second)
            continue;
        std::string base;
        size_t      index   = 0;
        const bool  indexed = split_indexed_key(key, base, index);
        if (base.empty()) {
            plan.skipped.emplace_back(key, "write \"key\" or \"key#index\" (an extruder or variant index)");
            continue;
        }
        const ConfigOption* now = edited.option(base);
        if (now == nullptr) {
            plan.skipped.emplace_back(key, "not a setting of this preset");
            continue;
        }
        std::string why;
        if (writable && !writable(base, why)) {
            plan.skipped.emplace_back(key, why.empty() ? std::string("not writable") : why);
            continue;
        }
        const ConfigOption* ref = reference.option(base);
        if (ref == nullptr) {
            plan.skipped.emplace_back(key, "the reference preset has no value for it");
            continue;
        }
        if (indexed) {
            const auto* vnow = dynamic_cast<const ConfigOptionVectorBase*>(now);
            const auto* vref = dynamic_cast<const ConfigOptionVectorBase*>(ref);
            if (vnow == nullptr || vref == nullptr) {
                plan.skipped.emplace_back(key, "not a per-extruder setting: revert it without an index");
                continue;
            }
            if (index >= vnow->size() || index >= vref->size()) {
                plan.skipped.emplace_back(key, "index out of range (" + std::to_string(vnow->size()) + " values)");
                continue;
            }
            std::unique_ptr<ConfigOption> copy(now->clone());
            static_cast<ConfigOptionVectorBase*>(copy.get())->set_at(ref, index, index);
            (*copy == *now ? plan.unchanged : plan.apply).push_back(key);
            continue;
        }
        if (*now == *ref)
            plan.unchanged.push_back(key);
        else
            plan.apply.push_back(key);
    }
    return plan;
}

// Applies a RevertPlan's `apply` keys from `reference` to `config` (a whole key, or one element
// of "key#i"). Returns the base keys written, each once, in order.
inline std::vector<std::string> apply_revert(ConfigBase& config, const ConfigBase& reference, const std::vector<std::string>& apply)
{
    std::vector<std::string> written;
    for (const std::string& key : apply) {
        std::string base;
        size_t      index = 0;
        const bool  indexed = split_indexed_key(key, base, index);
        ConfigOption*       now = config.option(base);
        const ConfigOption* ref = reference.option(base);
        if (now == nullptr || ref == nullptr)
            continue;
        if (indexed) {
            if (auto* v = dynamic_cast<ConfigOptionVectorBase*>(now); v != nullptr)
                v->set_at(ref, index, index);
        } else {
            now->set(ref);
        }
        if (std::find(written.begin(), written.end(), base) == written.end())
            written.push_back(base);
    }
    return written;
}

// SavePresetDialog::Item::update()'s rules for a typed preset name. Empty when acceptable, else
// the reason. `suffix_modified` is PresetCollection::get_suffix_modified().
inline std::string check_preset_name(const std::string& name, const std::string& suffix_modified)
{
    static const char* unusable = "<>[]:/\\|?*\"";
    if (name.empty() || name.size() > 200)
        return "the name is required (at most 200 characters)";
    if (name.find_first_of(unusable) != std::string::npos)
        return std::string("the name may not contain any of ") + unusable;
    if (name.front() == ' ' || name.back() == ' ')
        return "the name may not start or end with a space";
    if (!suffix_modified.empty() && name.find(suffix_modified) != std::string::npos)
        return "the name may not contain \"" + suffix_modified + "\"";
    for (const char* reserved : {"Default Setting", "Default Printer", "Default Filament", "- default -"})
        if (name == reserved)
            return "that name is reserved";
    return std::string();
}

// file.export kinds: the File menu's export entries.
struct ExportKind
{
    const char* id;
    const char* extension;   // ".stl"; empty for a folder of files
    bool        per_object;  // one file per object into a folder
    bool        needs_slice; // needs the preview's toolpaths
    bool        mesh;        // a mesh export (needs objects)
};

inline const std::vector<ExportKind>& export_kinds()
{
    static const std::vector<ExportKind> kinds = {
        {"stl", ".stl", false, false, true},       {"stls", "", true, false, true},
        {"drc", ".drc", false, false, true},       {"drcs", "", true, false, true},
        {"3mf", ".3mf", false, false, false},      {"publish3mf", ".3mf", false, false, false},
        {"toolpathsObj", ".obj", false, true, false},
    };
    return kinds;
}

inline const ExportKind* find_export_kind(const std::string& id)
{
    for (const ExportKind& k : export_kinds())
        if (id == k.id)
            return &k;
    return nullptr;
}

// A file name for one object of a per-object export: the object name with the characters a file
// system refuses replaced, made unique against `taken` (compared case-insensitively), " (n)" style.
inline std::string per_object_file_name(const std::string& object_name, const std::string& extension, std::set<std::string>& taken)
{
    std::string base;
    for (char c : object_name)
        base += (std::strchr("<>:\"/\\|?*", c) != nullptr || static_cast<unsigned char>(c) < 32) ? '_' : c;
    while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
        base.pop_back();
    if (base.empty())
        base = "object";
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        return s;
    };
    std::string name = base + extension;
    for (int n = 1; taken.count(lower(name)) != 0; ++n)
        name = base + " (" + std::to_string(n) + ")" + extension;
    taken.insert(lower(name));
    return name;
}

// A JSON value as the .ini text ConfigBase::set_deserialize() takes for an option of `def`'s type,
// the same conversion slicer.setSettings applies: strings pass through ("35%", "0.2"), booleans
// become 1/0, arrays join with commas (string vectors are C-style escaped). False for objects and
// nested arrays.
inline bool json_to_ini(const nlohmann::json& value, const ConfigOptionDef& def, std::string& out)
{
    if (value.is_string()) {
        out = value.get<std::string>();
        return true;
    }
    if (value.is_boolean()) {
        out = value.get<bool>() ? "1" : "0";
        return true;
    }
    if (value.is_number()) {
        out = value.dump();
        return true;
    }
    if (!value.is_array())
        return false;
    if (def.type == coStrings) {
        std::vector<std::string> items;
        for (const nlohmann::json& item : value) {
            if (!item.is_string())
                return false;
            items.push_back(item.get<std::string>());
        }
        out = escape_strings_cstyle(items);
        return true;
    }
    std::string joined;
    for (const nlohmann::json& item : value) {
        std::string text;
        if (item.is_array() || item.is_object() || !json_to_ini(item, def, text))
            return false;
        if (!joined.empty())
            joined += ',';
        joined += text;
    }
    out = joined;
    return true;
}

// Whether `turn_keys` (what this turn's automated writes set) covers every key in `dirty`: the
// unsaved edits are entirely the automation's own, so discarding them loses nothing of the person's.
inline bool dirty_keys_all_written(const std::vector<std::string>& dirty, const std::vector<std::string>& turn_keys)
{
    const std::set<std::string> written(turn_keys.begin(), turn_keys.end());
    return std::all_of(dirty.begin(), dirty.end(), [&written](const std::string& k) { return written.count(k) > 0; });
}

// Removes every key of `keys` from a preset JSON object (a preset file as save_to_json writes it:
// "name", "inherits", then the settings) and returns the ones that were there, in `keys` order.
// The bundle export and import run every preset through this with
// WebAppPresetShare::stripped_keys(): print-host addresses, credentials and upload switches never
// leave the preset folder, and never arrive in it from a downloaded bundle.
inline std::vector<std::string> strip_preset_json(nlohmann::json& preset, const std::vector<std::string>& keys)
{
    std::vector<std::string> removed;
    if (!preset.is_object())
        return removed;
    for (const std::string& k : keys)
        if (preset.erase(k) > 0)
            removed.push_back(k);
    return removed;
}

// Which collection a preset JSON loads into, as PresetBundle::import_json_presets decides it:
// "printer", "print", "filament", or "" for a JSON that is not a preset.
inline std::string preset_json_type(const nlohmann::json& preset)
{
    if (!preset.is_object())
        return std::string();
    if (preset.contains("printer_settings_id"))
        return "printer";
    if (preset.contains("print_settings_id"))
        return "print";
    if (preset.contains("filament_settings_id"))
        return "filament";
    return std::string();
}

// The text of a code option in a preset JSON: one string, or one string per extruder/filament
// joined by newlines. Empty when absent or blank.
inline std::string preset_json_text(const nlohmann::json& preset, const std::string& key)
{
    auto it = preset.find(key);
    if (it == preset.end())
        return std::string();
    std::string text;
    if (it->is_string())
        text = it->get<std::string>();
    else if (it->is_array())
        for (const nlohmann::json& v : *it)
            if (v.is_string() && !v.get<std::string>().empty())
                text += v.get<std::string>() + "\n";
    return text.find_first_not_of(" \t\r\n") == std::string::npos ? std::string() : text;
}

// What a downloaded preset file brings in, after the import has made it safe: `strip` keys (print
// hosts, credentials, local paths, post-processing scripts) removed from `preset`, and the
// custom-G-code keys of `code_keys` it still carries with a non-blank value, for the person to
// see in the confirmation before it lands in their preset folder.
struct ImportReview
{
    std::string              name;
    std::string              type; // preset_json_type()
    std::vector<std::string> removed;
    std::vector<std::string> code_keys;
};

inline ImportReview review_import_json(nlohmann::json& preset, const std::vector<std::string>& strip,
                                       const std::vector<std::string>& code_keys)
{
    ImportReview r;
    r.type = preset_json_type(preset);
    if (r.type.empty())
        return r;
    if (auto it = preset.find("name"); it != preset.end() && it->is_string())
        r.name = it->get<std::string>();
    r.removed = strip_preset_json(preset, strip);
    for (const std::string& k : code_keys)
        if (!preset_json_text(preset, k).empty())
            r.code_keys.push_back(k);
    return r;
}

// File types the preset imports accept: the Import Configs dialog's filter. Bundles are the zip
// forms. `ext` is lower case without the dot.
inline bool config_import_extension(const std::string& ext, bool bundle_only)
{
    if (ext == "zip" || ext == "orca_bundle" || ext == "orca_printer" || ext == "orca_filament")
        return true;
    return !bundle_only && ext == "json";
}

}}}} // namespace Slic3r::GUI::Actions::PresetLogic
