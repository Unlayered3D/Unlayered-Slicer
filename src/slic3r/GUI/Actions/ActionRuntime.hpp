#ifndef slic3r_GUI_Actions_ActionRuntime_hpp_
#define slic3r_GUI_Actions_ActionRuntime_hpp_

// Executing catalog actions for one bridge (one embedded web tab): validation, gates, the
// gizmo / dialog / slicing guards, target resolution and selection, undo snapshots, the ifDirty
// and ifProjectDirty preflights, native confirmation, long-running jobs, and the turn log.
//
// The contract for runner authors (docs/HLSD/bridge-actions.md "Writing an action"):
//   - Params arrive validated against the descriptor's schema with defaults filled in, and the
//     target resolved (ctx.targets) and selected in the GUI.
//   - A Mutate runner calls ctx.snapshot() exactly once before changing anything, then the same
//     Plater / ObjectList function the menu item calls. It never opens a modal dialog: where the
//     menu asks for something, the runner takes it as a param.
//   - A Destructive / Outward runner asks ctx.confirm(...) (unless its confirm_skip_rule applies)
//     and returns WAB_USER_CANCELLED when declined, having changed nothing.
//   - A runner that replaces the project calls ctx.project_dirty_preflight first; one that switches
//     a preset calls ctx.preset_dirty_preflight.
//   - A long-running runner returns ctx.after_plate_jobs(...) or finishes a ctx.defer() handle.

#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Preset.hpp"

#include "ActionCatalog.hpp"
#include "ActionTargets.hpp"
#include "NativeConfirm.hpp"
#include "TurnLog.hpp"

namespace Slic3r {
class Model;
namespace GUI {
class Plater;
namespace Actions {

// What the runtime needs from the bridge that owns it. Built by WebAppBridge (one per tab); every
// callback is GUI-thread only. Held by shared_ptr so a finishing job can tell the tab went away.
struct BridgeHost
{
    // A response to request `id` (code 0 = success).
    std::function<void(const std::string& id, const std::string& method, int code, const std::string& message,
                       const nlohmann::json& data)>
        respond;
    // An event to the page. Settings-family gating is applied by the caller (emit_event).
    std::function<void(const std::string& method, const nlohmann::json& data)> event;
    // Is the per-tab permission behind `gate` on? Gate::Settings = <slot>_allow_settings.
    std::function<bool(Gate gate)> allows;
    // The tab's capability list as WebAppBridge computes it, WITHOUT the action methods.
    std::function<nlohmann::json()> base_capabilities;
    std::function<std::string()>    slot_id;
    // Background slicing in progress.
    std::function<bool()> busy;
    // Downloads a file on openProjectFromUrl's terms (URL rules, the tab's download hosts, one
    // download per tab, the size limit) into a temporary folder. `allowed_exts` are lowercase
    // extensions without the dot ("stl", "svg"). Exactly one callback runs, on the GUI thread, and
    // only while the tab is alive. Null when the owning bridge offers no downloads.
    std::function<void(const std::string& url, const std::vector<std::string>& allowed_exts,
                       std::function<void(const std::string& local_path)> on_ready,
                       std::function<void(int code, const std::string& message)> on_error)>
        download;

    TurnLog     turn_log;
    std::string current_turn;   // the turn of the request being dispatched, "" outside one
    int         next_job = 1;
    // A job id -> its turn, while running.
    std::map<std::string, std::string> running_jobs;
    // The catalog version last reported with actionsChanged / getInfo.
    std::string reported_catalog_version;
    // False once the owning bridge is gone; Deferred completions check it.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    bool        allows_gate(Gate g) const { return allows && allows(Gate::Settings) && (g == Gate::Settings || allows(g)); }
    // Settings-gated event delivery (actionFinished, selectionChanged, actionsChanged).
    void        emit_event(const std::string& method, const nlohmann::json& data) const;
};

// The completion handle of a long-running action. finish() exactly once, on the GUI thread.
class Deferred
{
public:
    void               finish(InvokeResult result);
    bool               finished() const { return m_finished; }
    const std::string& job_id() const { return m_job_id; }

    // Runtime side: where the result goes. A result that arrived before this was attached (the
    // runner finished its handle synchronously, before returning started()) is delivered now.
    void on_finish(std::function<void(InvokeResult&&)> cb);

    // Runtime-owned; runners do not touch these.
    bool                                m_finished = false;
    std::string                         m_job_id;
    std::function<void(InvokeResult&&)> m_on_finish;
    std::optional<InvokeResult>         m_early; // finished before on_finish() was attached
};

// Everything a runner gets.
struct InvokeContext
{
    const ActionDescriptor* action = nullptr;
    BridgeHost*             host   = nullptr; // nullptr in unit tests
    std::string             request_id;
    nlohmann::json          target = nullptr;               // as sent
    nlohmann::json          params = nlohmann::json::object(); // validated, defaults applied
    ResolvedTargets         targets;
    std::string             turn;
    std::string             if_dirty = "ask";         // ask | discard | prompt | save | transfer
    std::string             if_project_dirty = "ask"; // ask | save | discard | prompt
    bool                    wait = true;
    int                     snapshots = 0; // taken through snapshot()
    // Runtime-owned: the context lives on the heap for as long as a long-running action needs it.
    std::weak_ptr<InvokeContext> self;
    std::weak_ptr<Deferred>      deferred_; // the handle defer() made, if any

    Plater* plater() const;
    Model&  model() const;

    // One undo snapshot named "<label> (automated)" (label: the descriptor's when empty).
    void snapshot(const std::string& label = {});

    // The ifDirty contract before a preset switch on `type` (same semantics as the typed
    // methods): "ask" -> false with 1013 {dirty, dirtyKeys} in `refusal`; "discard" drops the
    // edits; "prompt" leaves the usual dialog to the switch. True = go ahead.
    // Not a switch: "transfer" is refused (1002). Use preset_switch_preflight before a switch.
    bool preset_dirty_preflight(Preset::Type type, InvokeResult& refusal) const;
    // Before a switch of `type` to `target`: the contract for that preset and its dependents
    // (preset_switch_scope), so no UnsavedChangesDialog opens unless ifDirty is "prompt".
    bool preset_switch_preflight(Preset::Type type, const std::string& target, InvokeResult& refusal) const;
    // The ifProjectDirty contract before replacing the project: "ask" -> false with 1013
    // {projectDirty:true}; "save" saves silently when the project has a path (else 1013
    // {projectDirty:true, needsPath:true}); "discard" goes ahead; "prompt" leaves the slicer's own
    // dialog to the replacement. True = go ahead (nothing dirty, or allowed).
    bool project_dirty_preflight(InvokeResult& refusal) const;

    // The native confirmation (NativeConfirm.hpp). True = accepted.
    bool confirm(const ConfirmRequest& request) const;

    // Long-running work: a handle to finish later. Return InvokeResult::started() from the runner.
    std::shared_ptr<Deferred> defer();
    // Convenience for Plater jobs (arrange, orient, fill bed, simplify ...): runs `then` on the GUI
    // thread once the plate job worker is idle again (timed_out after timeout_ms; 0 = the action's
    // timeout) and finishes with its result. Returns InvokeResult::started().
    InvokeResult after_plate_jobs(std::function<InvokeResult(InvokeContext& ctx, bool timed_out)> then, int timeout_ms = 0);
};

// The ifDirty contract, shared by slicer.invokeAction (ifDirty on the envelope) and the typed
// settings methods (ifDirty inside their params): what a preset's unsaved edits get before
// something replaces them.
//   ask      -> 1013 {dirty, type, dirtyKeys, dirtyTypes}; nothing changes (the default)
//   discard  -> the edits are dropped
//   prompt   -> the slicer's own UnsavedChangesDialog asks the person during the switch
//   save     -> the edits are saved into the preset being edited when it is the person's own (a
//               user or project preset); a system preset's edits refuse 1013 {needsName:true}
//   transfer -> the edits are carried over to the preset being switched to (the dialog's
//               Transfer); only for a print or filament switch, never for a printer one (the
//               dialog does not offer it), nor between two filament types
// Every value is checked for every dirty preset before any of them is touched.
extern const char* const IF_DIRTY_VALUES[]; // nullptr-terminated: ask, discard, prompt, save, transfer
bool is_if_dirty_value(const std::string& v);
// The schema of an ifDirty param (the typed methods document it in their params).
nlohmann::json if_dirty_schema();

// The presets whose unsaved edits Tab::select_preset puts a question to when `type` switches:
// its own, then its dependents' (a printer: print and filament; a print: filament).
std::vector<Preset::Type> preset_switch_scope(Preset::Type type);

// The ifDirty contract for a switch of `type` to `target`: every preset of preset_switch_scope().
// Returns WAB_OK (go ahead) or the code to answer with, `data` / `message` filled.
int preset_switch_preflight(Preset::Type type, const std::string& target, const std::string& if_dirty, nlohmann::json& data,
                            std::string& message);
// The ifDirty contract for `types` when no switch follows (a project load, a reload): transfer
// has nowhere to go and is refused.
int presets_replace_preflight(const std::vector<Preset::Type>& types, const std::string& if_dirty, nlohmann::json& data,
                              std::string& message);

// Guards shared by every invoke, exposed for typed methods that do their own dispatch.
// "" when clear; else the reason, and `code`/`data` to answer with (1007 + data.gizmo when a
// gizmo is open unless gizmo_ok, 1007 when a dialog is open or (for writes) the slicer is slicing).
std::string gui_guard(const BridgeHost* host, bool write, int& code, nlohmann::json& data, bool gizmo_ok = false);

// The live enabled state: "" = enabled, else the reason. Includes the guards above for
// non-Read actions and the entry's EnabledFn.
std::string enabled_reason(const ActionEntry& entry, BridgeHost* host);

// slicer.invokeAction {id, target?, params?, turn?, wait?, ifDirty?, ifProjectDirty?}: answers
// `request_id` through host.respond (now, or when a long-running action finishes or the wait
// expires).
void invoke(BridgeHost& host, const std::string& request_id, const nlohmann::json& request);

// A typed method served by a catalog runner (descriptor.bridge_method): same checks as invoke,
// but `params` are the method's params and the reply is the runner's result object as-is.
void invoke_typed(BridgeHost& host, const ActionEntry& entry, const std::string& request_id, const nlohmann::json& params);

// Real (non-placeholder) snapshot timestamps on the Plater's undo stack, ascending. Empty without a Plater.
std::vector<size_t> undo_stack_timestamps();

// Records, for TurnLog, what one bridge request carrying params.turn did to the undo stack.
// Construct before dispatching any request, destroy after: covers typed methods too.
class RequestScope
{
public:
    RequestScope(BridgeHost& host, const std::string& method, const nlohmann::json& params);
    ~RequestScope();
    RequestScope(const RequestScope&)            = delete;
    RequestScope& operator=(const RequestScope&) = delete;

private:
    BridgeHost&         m_host;
    std::string         m_turn;
    std::string         m_previous_turn;
    std::vector<size_t> m_before;
};

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_ActionRuntime_hpp_
