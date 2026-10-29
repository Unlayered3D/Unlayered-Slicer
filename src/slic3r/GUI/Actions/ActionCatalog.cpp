#include "ActionCatalog.hpp"

#include <cstdio>
#include <mutex>
#include <set>
#include <stdexcept>

#include "ActionSchema.hpp"
#include "Register.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;

const char* const EFFECTS[] = {"geometry", "structure", "settings", "presets", "plates", "project",
                               "view",     "file",      "printer",  "paint",   "selection", nullptr};

const char* const TARGET_KINDS[] = {"objects", "object", "volumes", "volume", "instances", "plate", "none", nullptr};

const char* to_string(Risk r)
{
    switch (r) {
    case Risk::Read: return "read";
    case Risk::Ui: return "ui";
    case Risk::Mutate: return "mutate";
    case Risk::Destructive: return "destructive";
    case Risk::OutwardFile: return "outwardFile";
    case Risk::OutwardNetwork: return "outwardNetwork";
    case Risk::OutwardPrinter: return "outwardPrinter";
    }
    return "mutate";
}

const char* to_string(Gate g)
{
    switch (g) {
    case Gate::Settings: return "settings";
    case Gate::Export: return "export";
    case Gate::Printer: return "printer";
    case Gate::Install: return "install";
    }
    return "settings";
}

const char* to_string(Confirm c) { return c == Confirm::Native ? "native" : "none"; }

bool risk_from_string(const std::string& s, Risk& out)
{
    for (Risk r : {Risk::Read, Risk::Ui, Risk::Mutate, Risk::Destructive, Risk::OutwardFile, Risk::OutwardNetwork,
                   Risk::OutwardPrinter})
        if (s == to_string(r)) {
            out = r;
            return true;
        }
    return false;
}

bool gate_from_string(const std::string& s, Gate& out)
{
    for (Gate g : {Gate::Settings, Gate::Export, Gate::Printer, Gate::Install})
        if (s == to_string(g)) {
            out = g;
            return true;
        }
    return false;
}

Confirm default_confirm(Risk r)
{
    switch (r) {
    case Risk::Destructive:
    case Risk::OutwardFile:
    case Risk::OutwardNetwork:
    case Risk::OutwardPrinter: return Confirm::Native;
    default: return Confirm::None;
    }
}

bool is_known_effect(const std::string& e)
{
    for (const char* const* p = EFFECTS; *p != nullptr; ++p)
        if (e == *p)
            return true;
    return false;
}

static bool is_target_kind(const std::string& k)
{
    for (const char* const* p = TARGET_KINDS; *p != nullptr; ++p)
        if (k == *p)
            return true;
    return false;
}

bool is_forbidden_domain(const std::string& domain)
{
    // Never automatable (docs/HLSD/bridge-actions.md "What is not automated"): plugins, account and
    // sign-in, credentials, process execution, the app's lifecycle, firmware and OTA updates.
    static const std::set<std::string> forbidden = {"plugin",  "plugins", "account", "auth",    "login",  "token",
                                                    "network", "exec",    "process", "shell",   "system", "firmware",
                                                    "ota",     "privacy", "quit",    "restart", "host"};
    return forbidden.count(domain) > 0;
}

bool is_valid_action_id(const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    size_t segments = 0;
    size_t i        = 0;
    while (i <= id.size()) {
        size_t end = id.find('.', i);
        if (end == std::string::npos)
            end = id.size();
        if (end == i)
            return false;
        if (!(id[i] >= 'a' && id[i] <= 'z'))
            return false;
        for (size_t k = i + 1; k < end; ++k) {
            const char c = id[k];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
                return false;
        }
        ++segments;
        i = end + 1;
    }
    return segments >= 2;
}

std::string fnv1a_hex(const std::string& s)
{
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// ---------------------------------------------------------------------------------------------
// InvokeResult
// ---------------------------------------------------------------------------------------------

InvokeResult InvokeResult::ok(json result)
{
    InvokeResult r;
    r.result = result.is_null() ? json::object() : std::move(result);
    return r;
}

InvokeResult InvokeResult::error(int code, std::string message, json data)
{
    InvokeResult r;
    r.code    = code;
    r.message = std::move(message);
    r.data    = data.is_null() ? json::object() : std::move(data);
    return r;
}

InvokeResult InvokeResult::started()
{
    InvokeResult r;
    r.pending = true;
    return r;
}

InvokeResult& InvokeResult::with_effects(std::vector<std::string> e)
{
    effects     = std::move(e);
    effects_set = true;
    return *this;
}

// ---------------------------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------------------------

void Catalog::add(ActionDescriptor d, Runner runner, EnabledFn enabled)
{
    auto fail = [&d](const std::string& why) { throw std::invalid_argument("action '" + d.id + "': " + why); };

    if (!is_valid_action_id(d.id))
        fail("id must be <domain>.<verb> in lowerCamel ASCII");
    d.domain = d.id.substr(0, d.id.find('.'));
    if (is_forbidden_domain(d.domain))
        fail("domain '" + d.domain + "' may not be automated");
    if (m_by_id.count(d.id) > 0)
        fail("duplicate id");
    if (d.label.empty() || d.summary.empty())
        fail("label and summary are required");
    if (d.confirm == Confirm::None && d.confirm_skip_rule.empty())
        d.confirm = default_confirm(d.risk);

    if (!d.target.is_object() || !d.target.contains("kind") || !d.target["kind"].is_string() ||
        !is_target_kind(d.target["kind"].get<std::string>()))
        fail("target.kind must be one of objects|object|volumes|volume|instances|plate|none");
    if (!d.target.contains("required"))
        d.target["required"] = d.target["kind"] != "none";
    if (d.target.contains("accepts")) {
        // Only object actions widen their target, and only to parts and single instances.
        const std::string kind = d.target["kind"].get<std::string>();
        if (kind != "objects" && kind != "object")
            fail("target.accepts is for kind objects|object only");
        if (!d.target["accepts"].is_array() || d.target["accepts"].empty())
            fail("target.accepts must be a non-empty array");
        for (const auto& k : d.target["accepts"])
            if (!k.is_string() || (k != "volumes" && k != "instances"))
                fail("target.accepts may list only volumes and instances");
    }

    SchemaError err;
    if (!check_schema(d.params, err, "/params"))
        fail("params schema: " + err.path + ": " + err.message);
    if (d.params.value("type", "") != "object")
        fail("params must be an object schema");

    for (const std::string& e : d.effects)
        if (!is_known_effect(e))
            fail("unknown effect '" + e + "'");
    if (!d.examples.is_array())
        fail("examples must be an array");
    for (size_t i = 0; i < d.examples.size(); ++i) {
        const json& ex = d.examples[i];
        if (!ex.is_object())
            fail("example " + std::to_string(i) + " is not an object");
        const json params = ex.contains("params") ? ex["params"] : json::object();
        if (!validate(d.params, apply_defaults(d.params, params), err, "/params"))
            fail("example " + std::to_string(i) + " does not validate: " + err.path + ": " + err.message);
    }
    if (d.timeout_ms <= 0 || d.timeout_ms > 50000)
        fail("timeout_ms must be in (0, 50000]");

    m_by_id[d.id] = m_entries.size();
    m_entries.push_back({std::move(d), std::move(runner), std::move(enabled)});
    m_version.clear();
}

const ActionEntry* Catalog::find(const std::string& id) const
{
    auto it = m_by_id.find(id);
    return it == m_by_id.end() ? nullptr : &m_entries[it->second];
}

const ActionEntry* Catalog::find_by_method(const std::string& bridge_method) const
{
    if (bridge_method.empty())
        return nullptr;
    for (const ActionEntry& e : m_entries)
        if (e.descriptor.bridge_method == bridge_method)
            return &e;
    return nullptr;
}

void Catalog::clear()
{
    m_entries.clear();
    m_by_id.clear();
    m_version.clear();
}

std::string Catalog::version() const
{
    if (m_version.empty()) {
        std::string all;
        for (const ActionEntry& e : m_entries) {
            all += to_json(e.descriptor, Detail::Full).dump();
            all += '\n';
        }
        m_version = fnv1a_hex(all);
    }
    return m_version;
}

Catalog& catalog()
{
    static Catalog        instance;
    static std::once_flag once;
    std::call_once(once, [] { register_all(instance); });
    return instance;
}

void add(ActionDescriptor descriptor, Runner runner, EnabledFn enabled)
{
    catalog().add(std::move(descriptor), std::move(runner), std::move(enabled));
}

json to_json(const ActionDescriptor& d, Detail detail)
{
    json j = {{"id", d.id},
              {"domain", d.domain},
              {"label", d.label},
              {"summary", d.summary},
              {"risk", to_string(d.risk)},
              {"confirm", to_string(d.confirm)},
              {"gate", to_string(d.gate)},
              {"longRunning", d.long_running},
              {"bridgeMethod", d.bridge_method.empty() ? json(nullptr) : json(d.bridge_method)}};
    if (detail == Detail::Full) {
        j["target"]    = d.target;
        j["params"]    = d.params;
        j["effects"]   = d.effects;
        j["timeoutMs"] = d.timeout_ms;
        j["ui"]        = d.ui;
        j["examples"]  = d.examples;
        j["since"]     = d.since;
        if (!d.confirm_skip_rule.empty())
            j["confirmSkipRule"] = d.confirm_skip_rule;
        if (d.allow_with_gizmo)
            j["allowWithGizmo"] = true;
    }
    return j;
}

json catalog_snapshot(const Catalog& c)
{
    json actions = json::array();
    for (const ActionEntry& e : c.entries())
        actions.push_back(to_json(e.descriptor, Detail::Full));
    return {{"catalogVersion", c.version()}, {"total", c.size()}, {"actions", actions}};
}

}}} // namespace Slic3r::GUI::Actions
