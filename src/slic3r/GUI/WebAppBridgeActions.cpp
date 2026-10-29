#include "WebAppBridgeActions.hpp"

#include <algorithm>
#include <map>
#include <set>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>

#include <wx/glcanvas.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "AutoUpdater.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "Tab.hpp"
#include "WebAppBridge.hpp"
#include "WebAppSettings.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/Utils/UnlayeredAccount.hpp"

#include "Actions/ActionCatalog.hpp"
#include "Actions/ActionTargets.hpp"

namespace Slic3r { namespace GUI {

using json = nlohmann::json;
using namespace Actions;

namespace {

// The methods served here (besides typed methods with catalog runners).
const char* const OWN_METHODS[] = {"slicer.listActions", "slicer.invokeAction", "slicer.getSelection", "slicer.select",
                                   "slicer.undoTurn"};

bool contains_string(const json& arr, const std::string& s)
{
    if (!arr.is_array())
        return false;
    for (const json& v : arr)
        if (v.is_string() && v.get_ref<const std::string&>() == s)
            return true;
    return false;
}

// Every method this tab may call: the bridge's own list plus ours.
json all_capabilities(const BridgeHost& host)
{
    json caps = host.base_capabilities ? host.base_capabilities() : json::array();
    append_action_capabilities(host, caps);
    return caps;
}

// Would listActions show this entry to this tab at all?
bool visible(const BridgeHost& host, const ActionEntry& e, const json& caps)
{
    const ActionDescriptor& d = e.descriptor;
    if (!host.allows_gate(d.gate))
        return false;
    if (!d.bridge_method.empty() && !contains_string(caps, d.bridge_method))
        return false;
    return true;
}

json selection_json(const ResolvedTargets& t)
{
    Plater* plater = wxGetApp().plater();
    json    out    = {{"objects", json::array()}, {"volumes", json::array()}, {"instances", json::array()}, {"plate", t.plate}};
    if (plater == nullptr)
        return out;
    const Model& model = plater->model();
    for (int o : t.objects)
        out["objects"].push_back(object_ref_json(model, o));
    for (const auto& v : t.volumes)
        out["volumes"].push_back(volume_ref_json(model, v.first, v.second));
    for (const auto& k : t.instances) {
        if (k.first < 0 || k.first >= int(model.objects.size()) || k.second < 0 ||
            k.second >= int(model.objects[k.first]->instances.size()))
            continue;
        out["instances"].push_back({{"id", model.objects[k.first]->instances[k.second]->id().id},
                                    {"index", k.second},
                                    {"object", object_ref_json(model, k.first)}});
    }
    return out;
}

void handle_list_actions(BridgeHost& host, const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.listActions";
    std::string        domain, query, detail = "summary";
    bool               include_disabled = true;
    std::set<std::string> ids;
    if (params.contains("domain")) {
        if (!params["domain"].is_string())
            return host.respond(id, METHOD, WAB_BAD_PARAMS, "domain must be a string", {{"path", "/domain"}});
        domain = params["domain"].get<std::string>();
    }
    if (params.contains("query")) {
        if (!params["query"].is_string())
            return host.respond(id, METHOD, WAB_BAD_PARAMS, "query must be a string", {{"path", "/query"}});
        query = boost::algorithm::to_lower_copy(params["query"].get<std::string>());
    }
    if (params.contains("ids")) {
        if (!params["ids"].is_array())
            return host.respond(id, METHOD, WAB_BAD_PARAMS, "ids must be an array of action ids", {{"path", "/ids"}});
        for (const json& v : params["ids"])
            if (v.is_string())
                ids.insert(v.get<std::string>());
    }
    if (params.contains("detail")) {
        if (!params["detail"].is_string() || (params["detail"] != "summary" && params["detail"] != "full"))
            return host.respond(id, METHOD, WAB_BAD_PARAMS, "detail must be \"summary\" or \"full\"", {{"path", "/detail"}});
        detail = params["detail"].get<std::string>();
    }
    if (params.contains("includeDisabled")) {
        if (!params["includeDisabled"].is_boolean())
            return host.respond(id, METHOD, WAB_BAD_PARAMS, "includeDisabled must be a boolean", {{"path", "/includeDisabled"}});
        include_disabled = params["includeDisabled"].get<bool>();
    }

    const Catalog& c    = catalog();
    const json     caps = all_capabilities(host);
    json           actions = json::array();
    size_t         total   = 0;
    for (const ActionEntry& e : c.entries()) {
        if (!visible(host, e, caps))
            continue;
        ++total;
        const ActionDescriptor& d = e.descriptor;
        if (!domain.empty() && d.domain != domain)
            continue;
        if (!ids.empty() && ids.count(d.id) == 0 && ids.count(d.bridge_method) == 0)
            continue;
        if (!query.empty()) {
            const std::string hay = boost::algorithm::to_lower_copy(d.id + " " + d.label + " " + d.summary);
            if (hay.find(query) == std::string::npos)
                continue;
        }
        const std::string reason = enabled_reason(e, &host);
        if (!include_disabled && !reason.empty())
            continue;
        json j              = to_json(d, detail == "full" ? Detail::Full : Detail::Summary);
        j["enabled"]        = reason.empty();
        j["disabledReason"] = reason.empty() ? json(nullptr) : json(reason);
        actions.push_back(std::move(j));
    }
    host.reported_catalog_version = c.version();
    host.respond(id, METHOD, WAB_OK, "success", {{"catalogVersion", c.version()}, {"total", total}, {"actions", actions}});
}

void handle_get_selection(BridgeHost& host, const std::string& id)
{
    static const char* METHOD = "slicer.getSelection";
    if (!host.allows_gate(Gate::Settings))
        return host.respond(id, METHOD, WAB_CAPABILITY_DISABLED, "print settings access is not enabled for this tab", json::object());
    host.respond(id, METHOD, WAB_OK, "success", selection_json(current_selection()));
}

void handle_select(BridgeHost& host, const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.select";
    if (!host.allows_gate(Gate::Settings))
        return host.respond(id, METHOD, WAB_CAPABILITY_DISABLED, "print settings access is not enabled for this tab", json::object());
    int  code = WAB_OK;
    json data;
    if (std::string why = gui_guard(&host, false, code, data); !why.empty())
        return host.respond(id, METHOD, code, why, data);
    Plater* plater = wxGetApp().plater();
    json    target = params;
    target.erase("turn");
    ResolvedTargets t;
    std::string     message;
    if (const int rc = resolve_targets(plater->model(), plater->get_partplate_list().get_plate_count(),
                                       json{{"kind", "none"}, {"required", false}}, target, nullptr, t, message, data);
        rc != WAB_OK)
        return host.respond(id, METHOD, rc, message, data);
    if (t.plate >= 0 && t.plate != plater->get_partplate_list().get_curr_plate_index())
        plater->select_plate(t.plate);
    if (t.objects.empty() && t.volumes.empty() && t.instances.empty())
        plater->deselect_all();
    else
        select_targets(t);
    host.respond(id, METHOD, WAB_OK, "success", selection_json(current_selection()));
}

// Puts back the turn's settings writes whose current value is still what the turn wrote.
json restore_settings(BridgeHost& host, const TurnLog::Turn& turn, json& skipped)
{
    json restored = json::array();
    // Per tab, last write first (a key written twice keeps its first previous).
    std::map<std::string, std::vector<const TurnLog::SettingWrite*>> by_type;
    for (const TurnLog::SettingWrite& w : turn.settings)
        by_type[w.type].push_back(&w);
    for (auto& [type_name, writes] : by_type) {
        if (type_name == "project") {
            // Project configuration (filament colours, flush volumes, bed type ...): not a tab.
            if (wxGetApp().preset_bundle == nullptr || wxGetApp().plater() == nullptr)
                continue;
            DynamicPrintConfig& project = wxGetApp().preset_bundle->project_config;
            DynamicPrintConfig  changed;
            for (auto* w : writes) {
                auto skip = [&](const char* why) { skipped.push_back({{"type", type_name}, {"key", w->key}, {"reason", why}}); };
                if (!project.has(w->key)) {
                    skip("not a project setting");
                    continue;
                }
                if (project.opt_serialize(w->key) != w->written) {
                    skip("changed since");
                    continue;
                }
                try {
                    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
                    project.set_deserialize(w->key, w->previous, context);
                } catch (const std::exception&) {
                    skip("the previous value no longer parses");
                    continue;
                }
                changed.set_key_value(w->key, project.option(w->key)->clone());
                restored.push_back({{"type", type_name}, {"key", w->key}, {"value", w->previous}});
            }
            if (!changed.empty()) {
                Plater* plater = wxGetApp().plater();
                plater->on_config_change(changed);
                plater->update_project_dirty_from_presets();
                plater->schedule_background_process();
            }
            continue;
        }
        Preset::Type type = Preset::TYPE_INVALID;
        if (!WebAppSettings::preset_type_from_string(type_name, type)) {
            for (auto* w : writes)
                skipped.push_back({{"type", type_name}, {"key", w->key}, {"reason", "unknown preset type"}});
            continue;
        }
        Tab* tab = wxGetApp().get_tab(type);
        if (tab == nullptr || tab->get_config() == nullptr || tab->get_presets() == nullptr)
            continue;
        const std::string   current_preset = tab->get_presets()->get_edited_preset().name;
        DynamicPrintConfig  copy           = *tab->get_config();
        std::vector<std::string> keys;
        json                changes        = json::array();
        for (auto* w : writes) {
            auto skip = [&](const char* why) { skipped.push_back({{"type", type_name}, {"key", w->key}, {"reason", why}}); };
            if (w->preset != current_preset) {
                skip("another preset is selected now");
                continue;
            }
            const ConfigOption* now = copy.option(w->key);
            if (now == nullptr) {
                skip("not a setting of this preset");
                continue;
            }
            if (now->serialize() != w->written) {
                skip("changed since");
                continue;
            }
            try {
                ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
                copy.set_deserialize(w->key, w->previous, context);
            } catch (const std::exception&) {
                skip("the previous value no longer parses");
                continue;
            }
            keys.push_back(w->key);
            restored.push_back({{"type", type_name}, {"key", w->key}, {"value", w->previous}});
        }
        if (keys.empty())
            continue;
        {
            WebAppBridgeWriteScope scope;
            tab->load_config(copy);
            WebAppSettings::apply_tab_couplings(*tab, copy, keys); // as the writes being undone ran them
        }
        for (const std::string& k : keys)
            changes.push_back(tab->describe_setting_change(k));
        host.emit_event("slicer.settingsChanged", {{"type", type_name},
                                                   {"keys", keys},
                                                   {"source", "bridge"},
                                                   {"preset", tab->get_presets()->get_selected_preset_name()},
                                                   {"changes", changes}});
    }
    return restored;
}

void handle_undo_turn(BridgeHost& host, const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.undoTurn";
    if (!host.allows_gate(Gate::Settings))
        return host.respond(id, METHOD, WAB_CAPABILITY_DISABLED, "print settings access is not enabled for this tab", json::object());
    if (!params.contains("turn") || !params["turn"].is_string())
        return host.respond(id, METHOD, WAB_BAD_PARAMS, "turn is required", {{"path", "/turn"}, {"needs", "turn"}});
    const std::string     turn = params["turn"].get<std::string>();
    const TurnLog::Turn*  t    = host.turn_log.find(turn);
    if (t == nullptr)
        return host.respond(id, METHOD, WAB_NOT_FOUND, "nothing is recorded for that turn", {{"turn", turn}});
    int  code = WAB_OK;
    json data;
    if (std::string why = gui_guard(&host, true, code, data); !why.empty())
        return host.respond(id, METHOD, code, why, data);
    for (const auto& job : host.running_jobs)
        if (job.second == turn)
            return host.respond(id, METHOD, WAB_BUSY, "an action of that turn is still running", {{"jobId", job.first}});

    Plater*                   plater = wxGetApp().plater();
    const std::vector<size_t> stack  = undo_stack_timestamps();
    size_t                    undo_to = 0;
    std::string               why;
    if (!host.turn_log.can_roll_back(turn, stack, undo_to, why))
        return host.respond(id, METHOD, WAB_NEEDS_CONFIRMATION,
                            "the turn cannot be undone as a whole: " + why + "; use edit.undo step by step",
                            {{"laterEdits", true}, {"turn", turn}});
    // The stack must be at its top (nothing undone since): undo_to counts back from the active snapshot.
    const UndoRedo::Stack& st        = plater->undo_redo_stack_main();
    const auto&            snapshots = st.snapshots();
    if (undo_to != 0 && (snapshots.empty() || st.active_snapshot_time() != snapshots.back().timestamp))
        return host.respond(id, METHOD, WAB_NEEDS_CONFIRMATION,
                            "edits were undone since that turn; use edit.undo / edit.redo step by step",
                            {{"laterEdits", true}, {"turn", turn}});

    // The restore below must not be recorded as a write of the turn itself.
    const std::string saved_turn = host.current_turn;
    host.current_turn.clear();

    int undone = 0;
    if (undo_to != 0) {
        int active = -1, target = -1;
        for (size_t i = 0; i < snapshots.size(); ++i) {
            if (snapshots[i].timestamp == st.active_snapshot_time())
                active = int(i);
            if (snapshots[i].timestamp == undo_to)
                target = int(i);
        }
        if (active < 0 || target < 0 || active - target - 1 < 0) {
            host.current_turn = saved_turn;
            return host.respond(id, METHOD, WAB_INTERNAL, "the undo stack is not in the expected state", json::object());
        }
        undone = active - target;
        plater->undo_to(active - target - 1);
    }
    json skipped  = json::array();
    json restored = restore_settings(host, *t, skipped);
    host.current_turn = saved_turn;
    host.turn_log.forget(turn);
    host.respond(id, METHOD, WAB_OK, "success",
                 {{"turn", turn}, {"undoneSnapshots", undone},
                  {"restoredSettings", restored}, {"skipped", skipped}});
}

} // namespace

bool dispatch_action_method(BridgeHost& host, const std::string& id, const std::string& method, const json& params)
{
    const bool own = std::find_if(std::begin(OWN_METHODS), std::end(OWN_METHODS),
                                  [&method](const char* m) { return method == m; }) != std::end(OWN_METHODS);
    if (own && !host.allows_gate(Gate::Settings)) {
        host.respond(id, method, WAB_CAPABILITY_DISABLED, "print settings access is not enabled for this tab", json::object());
        return true;
    }
    if (method == "slicer.listActions") {
        handle_list_actions(host, id, params);
        return true;
    }
    if (method == "slicer.invokeAction") {
        invoke(host, id, params);
        return true;
    }
    if (method == "slicer.getSelection") {
        handle_get_selection(host, id);
        return true;
    }
    if (method == "slicer.select") {
        handle_select(host, id, params);
        return true;
    }
    if (method == "slicer.undoTurn") {
        handle_undo_turn(host, id, params);
        return true;
    }
    if (const ActionEntry* e = catalog().find_by_method(method); e != nullptr && e->runner) {
        invoke_typed(host, *e, id, params);
        return true;
    }
    return false;
}

void append_action_capabilities(const BridgeHost& host, json& capabilities)
{
    if (!host.allows_gate(Gate::Settings))
        return;
    for (const char* m : OWN_METHODS)
        if (!contains_string(capabilities, m))
            capabilities.push_back(m);
    for (const ActionEntry& e : catalog().entries())
        if (e.runner && !e.descriptor.bridge_method.empty() && host.allows_gate(e.descriptor.gate) &&
            !contains_string(capabilities, e.descriptor.bridge_method))
            capabilities.push_back(e.descriptor.bridge_method);
}

void append_action_info(const BridgeHost& host, json& info)
{
    if (!info.contains("features") || !info["features"].is_array())
        info["features"] = json::array();
    info["features"].push_back("actions.v1");
    if (host.allows_gate(Gate::Settings)) {
        const json caps  = all_capabilities(host);
        size_t     count = 0;
        for (const ActionEntry& e : catalog().entries())
            count += visible(host, e, caps) ? 1 : 0;
        info["actions"] = {{"version", catalog().version()}, {"count", count}};
        // Several filament slots removed behind one native confirmation (filament.removeSlots, 0.1.8).
        if (const ActionEntry* e = catalog().find("filament.removeSlots"); e != nullptr && visible(host, *e, caps))
            info["features"].push_back("filament.removeSlots");
    } else {
        info["actions"] = nullptr;
    }
    // Whether the app is signed in to an account provider, and which one (its origin; no names, no
    // tokens, no entitlements -- a page asks its own backend about those).
    bool        signed_in = false;
    std::string provider;
    if (AutoUpdater* u = wxGetApp().auto_updater(); u != nullptr) {
        UnlayeredAccount& acc = u->account();
        signed_in             = acc.signed_in();
        provider              = acc.provider_origin();
    }
    info["account"] = {{"signedIn", signed_in}, {"provider", provider}};
}

void on_bridge_event(BridgeHost& host, const std::string& method, const json& data)
{
    if (method == "slicer.settingsChanged") {
        // A bridge write inside a request carrying a turn: remember (key, previous, written) for undoTurn.
        if (host.current_turn.empty() || !data.is_object() || data.value("source", std::string()) != "bridge")
            return;
        const std::string type   = data.value("type", std::string());
        const std::string preset = data.value("preset", std::string());
        if (!data.contains("changes") || !data["changes"].is_array())
            return;
        for (const json& c : data["changes"]) {
            if (!c.is_object() || !c.contains("key") || !c["key"].is_string())
                continue;
            TurnLog::SettingWrite w;
            w.type     = type;
            w.preset   = preset;
            w.key      = c["key"].get<std::string>();
            w.previous = c.contains("previous") && c["previous"].is_string() ? c["previous"].get<std::string>() : std::string();
            w.written  = c.contains("value") && c["value"].is_string() ? c["value"].get<std::string>() : std::string();
            if (!(c.contains("previous") && c["previous"].is_string()))
                continue; // no previous value to go back to
            host.turn_log.note_setting(host.current_turn, std::move(w));
        }
    } else if (method == "slicer.capabilitiesChanged") {
        host.emit_event("slicer.actionsChanged", {{"catalogVersion", catalog().version()}});
    }
}

void watch_selection(const std::shared_ptr<BridgeHost>& host)
{
    if (!host)
        return;
    static std::vector<std::weak_ptr<BridgeHost>> watchers;
    static bool                                   bound = false;
    // Drop the dead, add this one once.
    watchers.erase(std::remove_if(watchers.begin(), watchers.end(), [](const std::weak_ptr<BridgeHost>& w) { return w.expired(); }),
                   watchers.end());
    bool known = false;
    for (const auto& w : watchers)
        known = known || w.lock() == host;
    if (!known)
        watchers.push_back(host);
    if (bound)
        return;
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->canvas3D() == nullptr || plater->canvas3D()->get_wxglcanvas() == nullptr ||
        wxGetApp().obj_list() == nullptr)
        return;
    bound = true;
    // Coalesce the bursts a click produces (canvas + list events) into one event per turn of the loop.
    static bool pending = false;
    auto        on_change = [](wxEvent& evt) {
        evt.Skip(); // the Plater's own handlers still run
        if (pending)
            return;
        pending = true;
        wxGetApp().CallAfter([]() {
            pending = false;
            const json sel = selection_json(current_selection());
            for (const auto& w : watchers)
                if (auto h = w.lock())
                    h->emit_event("slicer.selectionChanged", sel);
        });
    };
    plater->canvas3D()->get_wxglcanvas()->Bind(EVT_GLCANVAS_OBJECT_SELECT, on_change);
    wxGetApp().obj_list()->Bind(EVT_OBJ_LIST_OBJECT_SELECT, on_change);
}

}} // namespace Slic3r::GUI
