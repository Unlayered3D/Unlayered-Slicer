#include "ActionRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <set>

#include <boost/log/trivial.hpp>

#include <wx/timer.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "../GLCanvas3D.hpp"
#include "../GUI_App.hpp"
#include "../Gizmos/GLGizmosManager.hpp"
#include "../Jobs/Worker.hpp"
#include "../MainFrame.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../WebAppDialogs.hpp"
#include "../Tab.hpp"
#include "../WebAppBridge.hpp"
#include "../WebAppSettings.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "ActionSchema.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;

static const char* const INVOKE_METHOD = "slicer.invokeAction";

// ---------------------------------------------------------------------------------------------
// BridgeHost / Deferred
// ---------------------------------------------------------------------------------------------

void BridgeHost::emit_event(const std::string& method, const json& data) const
{
    if (event && allows && allows(Gate::Settings))
        event(method, data);
}

void Deferred::finish(InvokeResult result)
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_on_finish) {
        auto cb     = std::move(m_on_finish);
        m_on_finish = nullptr;
        cb(std::move(result));
    } else {
        m_early = std::move(result);
    }
}

void Deferred::on_finish(std::function<void(InvokeResult&&)> cb)
{
    if (m_early) {
        InvokeResult r = std::move(*m_early);
        m_early.reset();
        if (cb)
            cb(std::move(r));
        return;
    }
    if (!m_finished)
        m_on_finish = std::move(cb);
}

// ---------------------------------------------------------------------------------------------
// The ifDirty contract
// ---------------------------------------------------------------------------------------------

const char* const IF_DIRTY_VALUES[] = {"ask", "discard", "prompt", "save", "transfer", nullptr};

bool is_if_dirty_value(const std::string& v)
{
    for (const char* const* p = IF_DIRTY_VALUES; *p != nullptr; ++p)
        if (v == *p)
            return true;
    return false;
}

json if_dirty_schema()
{
    json values = json::array();
    for (const char* const* p = IF_DIRTY_VALUES; *p != nullptr; ++p)
        values.push_back(*p);
    return {{"type", "string"},
            {"enum", values},
            {"description", "Unsaved preset edits: ask (refuse, 1013), discard, prompt (the slicer asks the person), save (into "
                            "the person's own preset), transfer (carry them to the new preset; not for printers)."}};
}

std::vector<Preset::Type> preset_switch_scope(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINTER: return {Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT};
    case Preset::TYPE_PRINT: return {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT};
    default: return {type};
    }
}

namespace {

std::string filament_type_of(const Preset& p)
{
    const auto* t = p.config.option<ConfigOptionStrings>("filament_type");
    return t != nullptr && !t->values.empty() ? t->values.front() : std::string();
}

const std::string RETRY_HINT = "retry with ifDirty:\"discard\" to drop them, \"save\" to keep them in your own preset, \"transfer\" to "
                               "carry them over (print and filament switches), or \"prompt\" to let the user decide";

// switched: the type being switched (TYPE_INVALID: no switch). `target`: what it switches to.
int dirty_contract(const std::vector<Preset::Type>& types, Preset::Type switched, const std::string& target, const std::string& if_dirty_in,
                   json& data, std::string& message)
{
    const std::string if_dirty = if_dirty_in.empty() ? std::string("ask") : if_dirty_in;
    if (!is_if_dirty_value(if_dirty)) {
        message = "ifDirty must be \"ask\", \"discard\", \"prompt\", \"save\" or \"transfer\"";
        data    = {{"path", "/ifDirty"}};
        return WAB_BAD_PARAMS;
    }
    struct Dirty
    {
        Preset::Type type;
        Tab*         tab;
    };
    std::vector<Dirty> dirty;
    json               dirty_types = json::object();
    for (Preset::Type t : types) {
        Tab* tab = wxGetApp().get_tab(t);
        if (tab == nullptr || tab->get_presets() == nullptr || !tab->get_presets()->current_is_dirty())
            continue;
        dirty.push_back({t, tab});
        dirty_types[WebAppSettings::preset_type_name(t)] = tab->get_presets()->current_dirty_options();
    }
    if (dirty.empty() || if_dirty == "prompt")
        return WAB_OK; // "prompt": Tab::select_preset shows the usual transfer / discard dialog
    const char* first = WebAppSettings::preset_type_name(dirty.front().type);
    auto        refuse_dirty = [&](const std::string& why, json extra) {
        data = {{"dirty", true}, {"type", first}, {"dirtyKeys", dirty_types[first]}, {"dirtyTypes", dirty_types}};
        for (auto it = extra.begin(); it != extra.end(); ++it)
            data[it.key()] = it.value();
        message = why;
        return WAB_NEEDS_CONFIRMATION;
    };
    if (if_dirty == "ask") {
        std::string which;
        for (const Dirty& d : dirty)
            which += std::string(which.empty() ? "" : " and ") + WebAppSettings::preset_type_name(d.type);
        return refuse_dirty("the edited " + which + " preset" + (dirty.size() > 1 ? "s have" : " has") + " unsaved changes; " + RETRY_HINT,
                            json::object());
    }

    // Everything is checked before anything is touched.
    for (const Dirty& d : dirty) {
        const Preset& edited = d.tab->get_presets()->get_edited_preset();
        const char*   name   = WebAppSettings::preset_type_name(d.type);
        if (if_dirty == "save" && (edited.is_system || edited.is_default || !edited.can_overwrite()))
            return refuse_dirty(std::string("the unsaved changes are on the ") + name + " preset \"" + edited.name +
                                    "\", a system preset; save them under a name of your own with preset.save first, or use "
                                    "ifDirty:\"discard\" or \"transfer\"",
                                {{"needsName", true}, {"type", name}, {"dirtyKeys", dirty_types[name]}});
        if (if_dirty == "transfer") {
            if (switched == Preset::TYPE_INVALID) {
                message = "ifDirty:\"transfer\" carries edits to the preset a switch selects, and nothing is switched here; use "
                          "\"save\", \"discard\" or \"prompt\"";
                data    = {{"path", "/ifDirty"}};
                return WAB_BAD_PARAMS;
            }
            if (switched == Preset::TYPE_PRINTER) {
                message = "a printer switch cannot carry edits over (the slicer does not offer it); use ifDirty:\"save\", \"discard\" "
                          "or \"prompt\"";
                data    = {{"path", "/ifDirty"}, {"type", "printer"}};
                return WAB_BAD_PARAMS;
            }
            if (d.type == Preset::TYPE_FILAMENT && switched == Preset::TYPE_FILAMENT) {
                const Preset* to = d.tab->get_presets()->find_preset(target, false, true);
                if (to != nullptr && filament_type_of(*to) != filament_type_of(edited)) {
                    message = "edits cannot be carried between filament types (" + filament_type_of(edited) + " to " +
                              filament_type_of(*to) + "); use ifDirty:\"save\", \"discard\" or \"prompt\"";
                    data    = {{"path", "/ifDirty"}, {"type", "filament"}};
                    return WAB_BAD_PARAMS;
                }
            }
        }
    }

    for (const Dirty& d : dirty) {
        PresetCollection* presets = d.tab->get_presets();
        if (if_dirty == "discard") {
            presets->discard_current_changes();
            d.tab->update_dirty();
            d.tab->reload_config();
        } else if (if_dirty == "save") {
            // Save over the person's own preset, where it lives (preset folder or project), as the
            // dialog's Save does with the name left as it is.
            const Preset& edited = presets->get_edited_preset();
            d.tab->save_preset(edited.name, false, edited.is_project_embedded);
            if (presets->current_is_dirty()) {
                message = std::string("the ") + WebAppSettings::preset_type_name(d.type) +
                          " preset was not saved (the filament temperature check was declined)";
                data    = {{"type", WebAppSettings::preset_type_name(d.type)}};
                return WAB_USER_CANCELLED;
            }
        } else { // transfer: the dialog's Transfer, cached in the tab the switch applies it from
            d.tab->cache_config_diff(presets->current_dirty_options());
            presets->discard_current_changes();
            d.tab->update_dirty();
        }
    }
    return WAB_OK;
}

} // namespace

int preset_switch_preflight(Preset::Type type, const std::string& target, const std::string& if_dirty, json& data, std::string& message)
{
    return dirty_contract(preset_switch_scope(type), type, target, if_dirty, data, message);
}

int presets_replace_preflight(const std::vector<Preset::Type>& types, const std::string& if_dirty, json& data, std::string& message)
{
    return dirty_contract(types, Preset::TYPE_INVALID, std::string(), if_dirty, data, message);
}

// ---------------------------------------------------------------------------------------------
// InvokeContext
// ---------------------------------------------------------------------------------------------

Plater* InvokeContext::plater() const { return wxGetApp().plater(); }

Model& InvokeContext::model() const { return wxGetApp().plater()->model(); }

void InvokeContext::snapshot(const std::string& label)
{
    const std::string base = label.empty() ? (action != nullptr ? action->label : std::string("Edit")) : label;
    if (Plater* p = plater(); p != nullptr)
        p->take_snapshot(base + " (automated)");
    ++snapshots;
}

bool InvokeContext::preset_dirty_preflight(Preset::Type type, InvokeResult& refusal) const
{
    json        data;
    std::string message;
    if (const int code = presets_replace_preflight({type}, if_dirty, data, message); code != WAB_OK) {
        refusal = InvokeResult::error(code, message, data);
        return false;
    }
    return true;
}

bool InvokeContext::preset_switch_preflight(Preset::Type type, const std::string& target, InvokeResult& refusal) const
{
    json        data;
    std::string message;
    if (const int code = Actions::preset_switch_preflight(type, target, if_dirty, data, message); code != WAB_OK) {
        refusal = InvokeResult::error(code, message, data);
        return false;
    }
    return true;
}

bool InvokeContext::project_dirty_preflight(InvokeResult& refusal) const
{
    Plater* p = plater();
    if (p == nullptr || !p->is_project_dirty())
        return true;
    if (if_project_dirty == "discard" || if_project_dirty == "prompt")
        return true; // "prompt": the replacement shows the slicer's own save / discard dialog
    if (if_project_dirty == "save") {
        if (p->get_project_filename(".3mf").IsEmpty()) {
            refusal = InvokeResult::error(WAB_NEEDS_CONFIRMATION,
                                          "the project has unsaved changes and has never been saved; retry with "
                                          "ifProjectDirty:\"prompt\" so the user can choose where, or \"discard\"",
                                          {{"projectDirty", true}, {"needsPath", true}});
            return false;
        }
        p->save_project(false);
        if (p->is_project_dirty()) {
            refusal = InvokeResult::error(WAB_IO, "saving the project failed", {{"projectDirty", true}});
            return false;
        }
        return true;
    }
    refusal = InvokeResult::error(WAB_NEEDS_CONFIRMATION,
                                  "the project has unsaved changes; retry with ifProjectDirty:\"save\", \"discard\" or "
                                  "\"prompt\" (let the user decide)",
                                  {{"projectDirty", true}});
    return false;
}

bool InvokeContext::confirm(const ConfirmRequest& request) const { return native_confirm(request); }

std::shared_ptr<Deferred> InvokeContext::defer()
{
    auto d      = std::make_shared<Deferred>();
    d->m_job_id = "job-" + std::to_string(host != nullptr ? host->next_job++ : 0);
    deferred_   = d;
    return d;
}

InvokeResult InvokeContext::after_plate_jobs(std::function<InvokeResult(InvokeContext& ctx, bool timed_out)> then, int timeout_ms)
{
    std::shared_ptr<Deferred>      d    = defer();
    std::shared_ptr<InvokeContext> keep = self.lock();
    if (timeout_ms <= 0)
        timeout_ms = action != nullptr ? action->timeout_ms : 50000;
    auto*      timer   = new wxTimer();
    const auto started = std::chrono::steady_clock::now();
    timer->Bind(wxEVT_TIMER, [timer, started, timeout_ms, d, keep, then](wxTimerEvent&) {
        Plater*    plater = wxGetApp().plater();
        const bool idle   = plater == nullptr || plater->get_ui_job_worker().is_idle();
        const bool late   = std::chrono::steady_clock::now() - started > std::chrono::milliseconds(timeout_ms);
        if (!idle && !late)
            return;
        timer->Stop();
        // One more turn of the event loop, so the job's finalize() has run before `then` reads the result.
        wxGetApp().CallAfter([timer, d, keep, then, idle]() {
            delete timer;
            if (!keep) {
                d->finish(InvokeResult::error(WAB_INTERNAL, "the action context went away"));
                return;
            }
            InvokeResult r;
            try {
                r = then(*keep, !idle);
            } catch (const std::exception& e) {
                r = InvokeResult::error(WAB_INTERNAL, e.what());
            }
            d->finish(std::move(r));
        });
    });
    timer->Start(150);
    return InvokeResult::started();
}

// ---------------------------------------------------------------------------------------------
// Guards and enabled state
// ---------------------------------------------------------------------------------------------

static bool is_write(Risk r) { return r != Risk::Read && r != Risk::Ui; }

std::string gui_guard(const BridgeHost* host, bool write, int& code, json& data, bool gizmo_ok)
{
    code                 = WAB_OK;
    data                 = json::object();
    MainFrame* mainframe = wxGetApp().mainframe;
    Plater*    plater    = wxGetApp().plater();
    if (mainframe == nullptr || plater == nullptr) {
        code = WAB_INTERNAL;
        return "the slicer is not ready";
    }
    // A modal dialog disables the frame; acting under one would nest a second modal inside it.
    if (!mainframe->IsEnabled()) {
        code = WAB_BUSY;
        return WebAppDialogs::busy_message();
    }
    if (!write)
        return {};
    if (GLCanvas3D* canvas = plater->canvas3D(); canvas != nullptr && !gizmo_ok) {
        const GLGizmosManager& gizmos = canvas->get_gizmos_manager();
        if (gizmos.get_current_type() != GLGizmosManager::Undefined) {
            code          = WAB_BUSY;
            data["gizmo"] = get_name_from_gizmo_etype(gizmos.get_current_type());
            return "a tool (gizmo) is open; close it first";
        }
    }
    if (host != nullptr && host->busy && host->busy()) {
        code = WAB_BUSY;
        return "the slicer is busy slicing";
    }
    if (!plater->get_ui_job_worker().is_idle()) {
        code = WAB_BUSY;
        return "a job (arrange, orient, fill bed ...) is running";
    }
    return {};
}

std::string enabled_reason(const ActionEntry& entry, BridgeHost* host)
{
    int  code = 0;
    json data;
    if (std::string why = gui_guard(host, is_write(entry.descriptor.risk), code, data, entry.descriptor.allow_with_gizmo); !why.empty())
        return why;
    if (entry.enabled) {
        InvokeContext ctx;
        ctx.action  = &entry.descriptor;
        ctx.host    = host;
        ctx.targets = current_selection();
        try {
            return entry.enabled(ctx);
        } catch (const std::exception& e) {
            return e.what();
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Undo stack
// ---------------------------------------------------------------------------------------------

std::vector<size_t> undo_stack_timestamps()
{
    std::vector<size_t> out;
    Plater*             plater = wxGetApp().plater();
    if (plater == nullptr)
        return out;
    const std::vector<UndoRedo::Snapshot>& snapshots = plater->undo_redo_stack_main().snapshots();
    // The last entry is the "topmost" placeholder for the current, uncaptured state.
    for (size_t i = 0; i + 1 < snapshots.size(); ++i)
        out.push_back(snapshots[i].timestamp);
    return out;
}

RequestScope::RequestScope(BridgeHost& host, const std::string& method, const json& params) : m_host(host)
{
    (void) method;
    if (params.is_object() && params.contains("turn") && params["turn"].is_string()) {
        const std::string turn = params["turn"].get<std::string>();
        if (TurnLog::valid_turn_id(turn)) {
            m_turn            = turn;
            m_before          = undo_stack_timestamps();
            m_previous_turn   = host.current_turn;
            host.current_turn = turn;
        }
    }
}

RequestScope::~RequestScope()
{
    if (m_turn.empty())
        return;
    try {
        m_host.turn_log.note_snapshots(m_turn, m_before, undo_stack_timestamps());
    } catch (...) {
    }
    m_host.current_turn = m_previous_turn;
}

// ---------------------------------------------------------------------------------------------
// Invoke
// ---------------------------------------------------------------------------------------------

namespace {

json error_data_with(json data, const std::string& id)
{
    if (!data.is_object())
        data = json::object();
    if (!id.empty())
        data["action"] = id;
    return data;
}

// Validation, gates, guards, targets and selection. On failure answers and returns null.
std::shared_ptr<InvokeContext> prepare(BridgeHost& host, const ActionEntry& entry, const std::string& request_id,
                                       const std::string& method, const json& target, const json& params_in,
                                       const json& request)
{
    const ActionDescriptor& d     = entry.descriptor;
    auto                    reply = [&](int code, const std::string& message, json data = json::object()) {
        host.respond(request_id, method, code, message, error_data_with(std::move(data), d.id));
    };

    if (!host.allows_gate(d.gate)) {
        reply(WAB_CAPABILITY_DISABLED, std::string("this action needs the ") + to_string(d.gate) + " permission for this tab");
        return nullptr;
    }
    if (!entry.runner) {
        reply(WAB_BAD_PARAMS, "this capability is a typed method; call " + d.bridge_method + " directly",
              {{"bridgeMethod", d.bridge_method}});
        return nullptr;
    }
    const json params_raw = params_in.is_null() ? json::object() : params_in;
    if (!params_raw.is_object()) {
        reply(WAB_BAD_PARAMS, "params must be an object", {{"path", "/params"}});
        return nullptr;
    }
    json        params = apply_defaults(d.params, params_raw);
    SchemaError err;
    if (!validate(d.params, params, err, "/params")) {
        json data = {{"path", err.path}};
        if (!err.needs.empty())
            data["needs"] = err.needs;
        reply(WAB_BAD_PARAMS, err.message, data);
        return nullptr;
    }

    auto ctx        = std::make_shared<InvokeContext>();
    ctx->self       = ctx;
    ctx->action     = &d;
    ctx->host       = &host;
    ctx->request_id = request_id;
    ctx->target     = target;
    ctx->params     = std::move(params);
    ctx->turn       = host.current_turn;
    if (request.is_object()) {
        if (request.contains("turn") && request["turn"].is_string() && TurnLog::valid_turn_id(request["turn"].get<std::string>()))
            ctx->turn = request["turn"].get<std::string>();
        if (request.contains("wait")) {
            if (!request["wait"].is_boolean()) {
                reply(WAB_BAD_PARAMS, "wait must be a boolean", {{"path", "/wait"}});
                return nullptr;
            }
            ctx->wait = request["wait"].get<bool>();
        }
        if (request.contains("ifDirty")) {
            const json& v = request["ifDirty"];
            if (!v.is_string() || !is_if_dirty_value(v.get<std::string>())) {
                reply(WAB_BAD_PARAMS, "ifDirty must be \"ask\", \"discard\", \"prompt\", \"save\" or \"transfer\"", {{"path", "/ifDirty"}});
                return nullptr;
            }
            ctx->if_dirty = v.get<std::string>();
        }
        if (request.contains("ifProjectDirty")) {
            const json& v = request["ifProjectDirty"];
            if (!v.is_string() || (v != "ask" && v != "save" && v != "discard" && v != "prompt")) {
                reply(WAB_BAD_PARAMS, "ifProjectDirty must be \"ask\", \"save\", \"discard\" or \"prompt\"",
                      {{"path", "/ifProjectDirty"}});
                return nullptr;
            }
            ctx->if_project_dirty = v.get<std::string>();
        }
    }

    int  code = WAB_OK;
    json data;
    if (std::string why = gui_guard(&host, is_write(d.risk), code, data, d.allow_with_gizmo); !why.empty()) {
        reply(code, why, data);
        return nullptr;
    }

    Plater*               plater    = wxGetApp().plater();
    const ResolvedTargets selection = current_selection();
    std::string           message;
    if (const int rc = resolve_targets(plater->model(), plater->get_partplate_list().get_plate_count(), d.target, target,
                                       &selection, ctx->targets, message, data);
        rc != WAB_OK) {
        reply(rc, message, data);
        return nullptr;
    }

    if (entry.enabled) {
        std::string why;
        try {
            why = entry.enabled(*ctx);
        } catch (const std::exception& e) {
            why = e.what();
        }
        if (!why.empty()) {
            reply(WAB_ACTION_DISABLED, why, {{"reason", why}});
            return nullptr;
        }
    }

    const std::string kind = d.target.value("kind", std::string("none"));
    if (kind != "none" && kind != "plate")
        select_targets(ctx->targets);
    return ctx;
}

std::vector<std::string> effects_of(const InvokeContext& ctx, const InvokeResult& r)
{
    return r.effects_set ? r.effects : ctx.action->effects;
}

json undo_json(const InvokeContext& ctx)
{
    return {{"snapshots", ctx.snapshots}, {"label", ctx.snapshots > 0 ? json(ctx.action->label + " (automated)") : json(nullptr)}};
}

json done_body(const InvokeContext& ctx, const InvokeResult& r)
{
    json body = {{"id", ctx.action->id}, {"status", "done"}, {"result", r.result}, {"effects", effects_of(ctx, r)}, {"undo", undo_json(ctx)}};
    if (!ctx.turn.empty())
        body["turn"] = ctx.turn;
    return body;
}

InvokeResult run(InvokeContext& ctx, const ActionEntry& entry)
{
    try {
        return entry.runner(ctx);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Actions: " << entry.descriptor.id << " threw: " << e.what();
        return InvokeResult::error(WAB_INTERNAL, e.what());
    }
}

// A runner returned pending: answer when it finishes (wait) or now with status "running" and
// report the end as slicer.actionFinished.
void handle_pending(BridgeHost& host, const std::shared_ptr<InvokeContext>& ctx, const std::string& method, bool typed,
                    const std::vector<size_t>& before)
{
    std::shared_ptr<Deferred> d = ctx->deferred_.lock();
    if (!d) {
        host.respond(ctx->request_id, method, WAB_INTERNAL, "the action reported a job but made none",
                     error_data_with(json::object(), ctx->action->id));
        return;
    }
    struct State
    {
        bool responded = false;
    };
    auto                state  = std::make_shared<State>();
    std::weak_ptr<bool> alive  = host.alive;
    BridgeHost*         hostp  = &host;
    const std::string   job_id = d->job_id();
    host.running_jobs[job_id]  = ctx->turn;

    auto running_reply = [hostp, ctx, method, job_id]() {
        hostp->respond(ctx->request_id, method, WAB_OK, "running", {{"id", ctx->action->id}, {"status", "running"}, {"jobId", job_id}});
    };

    auto deliver = [alive, hostp, ctx, state, method, typed, job_id, before](InvokeResult&& r) {
        auto a = alive.lock();
        if (!a || !*a)
            return;
        BridgeHost& h = *hostp;
        h.running_jobs.erase(job_id);
        if (!ctx->turn.empty())
            h.turn_log.note_snapshots(ctx->turn, before, undo_stack_timestamps());
        if (!state->responded) {
            state->responded = true;
            if (r.code != WAB_OK)
                h.respond(ctx->request_id, method, r.code, r.message, error_data_with(r.data, ctx->action->id));
            else
                h.respond(ctx->request_id, method, WAB_OK, "success", typed ? r.result : done_body(*ctx, r));
            return;
        }
        json ev = {{"jobId", job_id}, {"id", ctx->action->id}, {"ok", r.code == WAB_OK}, {"effects", effects_of(*ctx, r)}};
        if (r.code == WAB_OK) {
            ev["result"] = r.result;
            ev["undo"]   = undo_json(*ctx);
        } else {
            ev["error"] = {{"code", r.code}, {"message", r.message}, {"data", r.data}};
        }
        if (!ctx->turn.empty())
            ev["turn"] = ctx->turn;
        h.emit_event("slicer.actionFinished", ev);
    };

    if (d->finished()) {
        // Finished synchronously (e.g. a download refused before any request): answer now.
        d->on_finish(std::move(deliver));
        return;
    }
    d->on_finish(std::move(deliver));

    if (!ctx->wait) {
        state->responded = true;
        running_reply();
        return;
    }
    // Wait inside the page shim's 60 s call timeout, then hand over to the actionFinished event.
    auto* timer = new wxTimer();
    timer->Bind(wxEVT_TIMER, [timer, alive, state, running_reply, d](wxTimerEvent&) {
        timer->Stop();
        wxGetApp().CallAfter([timer]() { delete timer; });
        auto a = alive.lock();
        if (!a || !*a || state->responded || d->finished())
            return;
        state->responded = true;
        running_reply();
    });
    timer->StartOnce(std::min(ctx->action->timeout_ms, 50000));
}

void finish_sync(BridgeHost& host, const std::shared_ptr<InvokeContext>& ctx, const std::string& method, bool typed,
                 const InvokeResult& r)
{
    if (r.code != WAB_OK)
        host.respond(ctx->request_id, method, r.code, r.message, error_data_with(r.data, ctx->action->id));
    else
        host.respond(ctx->request_id, method, WAB_OK, "success", typed ? r.result : done_body(*ctx, r));
}

} // namespace

void invoke(BridgeHost& host, const std::string& request_id, const json& request)
{
    auto reply = [&](int code, const std::string& message, json data = json::object()) {
        host.respond(request_id, INVOKE_METHOD, code, message, data);
    };
    if (!host.allows_gate(Gate::Settings)) {
        reply(WAB_CAPABILITY_DISABLED, "print settings access is not enabled for this tab");
        return;
    }
    if (!request.is_object() || !request.contains("id") || !request["id"].is_string()) {
        reply(WAB_BAD_PARAMS, "id (the action id) is required", {{"path", "/id"}, {"needs", "id"}});
        return;
    }
    static const std::set<std::string> keys = {"id", "target", "params", "turn", "wait", "ifDirty", "ifProjectDirty"};
    for (auto it = request.begin(); it != request.end(); ++it)
        if (keys.count(it.key()) == 0) {
            reply(WAB_BAD_PARAMS, "unknown key '" + it.key() + "'", {{"path", "/" + it.key()}});
            return;
        }
    const std::string  id    = request["id"].get<std::string>();
    const ActionEntry* entry = catalog().find(id);
    if (entry == nullptr) {
        reply(WAB_NOT_FOUND, "no action '" + id + "'; slicer.listActions lists them", {{"action", id}});
        return;
    }
    auto ctx = prepare(host, *entry, request_id, INVOKE_METHOD, request.contains("target") ? request["target"] : json(nullptr),
                       request.contains("params") ? request["params"] : json::object(), request);
    if (!ctx)
        return;

    const std::vector<size_t> before = undo_stack_timestamps();
    InvokeResult              r      = run(*ctx, *entry);
    if (r.pending)
        handle_pending(host, ctx, INVOKE_METHOD, false, before);
    else
        finish_sync(host, ctx, INVOKE_METHOD, false, r);
}

void invoke_typed(BridgeHost& host, const ActionEntry& entry, const std::string& request_id, const json& params)
{
    const std::string& method = entry.descriptor.bridge_method;
    if (!host.allows_gate(Gate::Settings)) {
        host.respond(request_id, method, WAB_CAPABILITY_DISABLED, "print settings access is not enabled for this tab", json::object());
        return;
    }
    // The envelope keys every method accepts are not the method's own params.
    json own     = params.is_object() ? params : json::object();
    json request = json::object();
    for (const char* k : {"turn", "wait", "ifDirty", "ifProjectDirty", "target"})
        if (own.contains(k)) {
            request[k] = own[k];
            own.erase(k);
        }
    auto ctx = prepare(host, entry, request_id, method, request.contains("target") ? request["target"] : json(nullptr), own, request);
    if (!ctx)
        return;
    const std::vector<size_t> before = undo_stack_timestamps();
    InvokeResult              r      = run(*ctx, entry);
    if (r.pending)
        handle_pending(host, ctx, method, true, before);
    else
        finish_sync(host, ctx, method, true, r);
}

}}} // namespace Slic3r::GUI::Actions
