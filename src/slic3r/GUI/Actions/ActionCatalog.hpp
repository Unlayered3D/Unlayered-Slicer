#ifndef slic3r_GUI_Actions_ActionCatalog_hpp_
#define slic3r_GUI_Actions_ActionCatalog_hpp_

// The automation action catalog (docs/HLSD/bridge-actions.md): one descriptor per thing a user can
// do in the slicer that an embedded web application may ask the slicer to do, with a restricted
// JSON-Schema for its parameters, its risk class, which per-tab permission opens it and what it
// changes. The catalog is the single source of truth the bridge's slicer.listActions reports and
// slicer.invokeAction executes; tools/check_action_coverage.py checks it against every menu,
// toolbar, gizmo, sidebar control and Preferences row in the GUI sources.
//
// This header is pure data: no wx, no GUI_App. Registration (Register.cpp) only builds
// descriptors and stores runner lambdas; nothing here may touch the GUI, so the catalog can be
// built and checked in unit tests without a running application.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace Actions {

// What invoking the action can do, which decides how it is confirmed (docs/HLSD/bridge-actions.md
// "Safety"). Forbidden things are never in the catalog at all.
enum class Risk
{
    Read,           // reads state; changes nothing
    Ui,             // changes only what is shown (camera, panels, toggles)
    Mutate,         // an undoable model edit, or an unsaved preset edit
    Destructive,    // not undoable, or discards the user's data: native confirmation
    OutwardFile,    // writes a file the user did not pick yet: native Save dialog
    OutwardNetwork, // fetches from / sends to the network
    OutwardPrinter, // reaches a printer
};

// Which per-tab permission opens the action, on top of <slot>_allow_settings, which every
// action needs.
enum class Gate
{
    Settings, // <slot>_allow_settings only
    Export,   // also <slot>_allow_export
    Printer,  // also <slot>_allow_printer
    Install,  // also <slot>_allow_install
};

enum class Confirm
{
    None,
    Native, // the one native confirmation dialog (NativeConfirm.hpp) the page cannot press
};

const char* to_string(Risk r);
const char* to_string(Gate g);
const char* to_string(Confirm c);
bool        risk_from_string(const std::string& s, Risk& out);
bool        gate_from_string(const std::string& s, Gate& out);
// Native for Destructive and every Outward* risk; None otherwise.
Confirm     default_confirm(Risk r);

// Effects an action may declare. The page re-syncs what it shows from them (geometry/structure/
// plates -> re-read the model tree), and the generic E2E test asserts the declared ones changed.
extern const char* const EFFECTS[];   // nullptr-terminated
bool                     is_known_effect(const std::string& e);

// Target kinds (descriptor.target["kind"]).
//   objects   one or more ModelObjects        object    exactly one ModelObject
//   volumes   one or more ModelVolumes        volume    exactly one ModelVolume
//   instances one or more ModelInstances      plate     one plate (index)
//   none      the action takes no target
// An objects/object action that also acts on parts or single instances lists them in
// target["accepts"] (["volumes"], ["instances"] or both): a target naming only those then
// satisfies "required" too, and the runner works on what was named (ActionTargets.hpp).
extern const char* const TARGET_KINDS[]; // nullptr-terminated

struct ActionDescriptor
{
    std::string id;      // "<domain>.<verb>", e.g. "object.mirror"; stable once shipped
    std::string domain;  // the part of id before the first '.'
    std::string label;   // untranslated UI string ("Mirror")
    std::string summary; // one sentence for a tool description; no product names

    Risk    risk{Risk::Mutate};
    Gate    gate{Gate::Settings};
    Confirm confirm{Confirm::None}; // add() fills default_confirm(risk) when left None for a Destructive/Outward risk
    // Destructive/Outward actions that are confirmed some other way (the native Save dialog is the
    // consent, the edits being discarded are this turn's own, ...) say so here; it documents the
    // exception that lets confirm stay None and the catalog test accepts it.
    std::string confirm_skip_rule;

    nlohmann::json target = {{"kind", "none"}, {"required", false}};
    // Restricted JSON Schema (ActionSchema.hpp): always {"type":"object","additionalProperties":false,...}.
    nlohmann::json params = {{"type", "object"}, {"additionalProperties", false}, {"properties", nlohmann::json::object()}};

    std::vector<std::string> effects;     // EFFECTS
    bool                     long_running = false;
    // Runs while a gizmo is open (the gizmo guard skips it): only actions about the gizmo itself,
    // such as ui.closeGizmo.
    bool                     allow_with_gizmo = false;
    int                      timeout_ms   = 50000;
    std::vector<std::string> ui;          // coverage-manifest keys this action implements
    // >= 1 example {"target": {...}|null, "params": {...}, "setup"?: [...]}; each params must validate.
    // They drive the generic E2E test (invoke, check effects, undoTurn, check restored). "setup" is
    // a list of slicer.invokeAction requests {id, target?, params?} that create the example's
    // preconditions (a height range to edit, a second plate to delete); they run first, under their
    // own turn, and are undone afterwards. Not validated here.
    nlohmann::json examples = nlohmann::json::array();

    // Non-empty: this capability is served by a typed bridge method of that name ("slicer.setSettings").
    // A descriptor with a bridge_method and NO runner describes a method WebAppBridge.cpp already
    // handles; one WITH a runner is a typed method dispatched by WebAppBridgeActions (its reply is the
    // runner's result as-is). Either way slicer.listActions shows it only when the method is in this
    // tab's capabilities, so the catalog never advertises something the tab cannot call.
    std::string bridge_method;
    std::string since = "0.2.0";
};

// What a runner returns. code 0 (WAB_OK) = success.
struct InvokeResult
{
    int            code = 0;
    std::string    message;
    nlohmann::json result = nlohmann::json::object(); // success payload ("result" in the reply)
    nlohmann::json data   = nlohmann::json::object(); // error payload (1002 path/needs, 1013 dirty ...)
    // Overrides the descriptor's effects for this invocation (e.g. a dry run changes nothing). Empty
    // vector with effects_set = true means "changed nothing".
    std::vector<std::string> effects;
    bool                     effects_set = false;
    // A long-running action started and will finish through its Deferred (ActionRuntime.hpp).
    bool pending = false;

    static InvokeResult ok(nlohmann::json result = nlohmann::json::object());
    static InvokeResult error(int code, std::string message, nlohmann::json data = nlohmann::json::object());
    static InvokeResult started(); // pending = true
    InvokeResult&       with_effects(std::vector<std::string> e);
    bool                is_ok() const { return code == 0 && !pending; }
};

struct InvokeContext; // ActionRuntime.hpp

// Runs the action. GUI thread. Params are already validated against the schema (defaults applied)
// and the target resolved and selected; see ActionRuntime.hpp for what the context offers.
using Runner = std::function<InvokeResult(InvokeContext&)>;
// "" = enabled, else a short reason ("nothing is selected", "a gizmo is open"). GUI thread; cheap.
using EnabledFn = std::function<std::string(const InvokeContext&)>;

struct ActionEntry
{
    ActionDescriptor descriptor;
    Runner           runner;
    EnabledFn        enabled;
};

// The registry. One process-wide instance (catalog()); tests may build their own.
class Catalog
{
public:
    // Adds an action. Fills domain from id and the default confirm; throws std::invalid_argument on
    // a duplicate id, an id that is not "<domain>.<verb>", a forbidden domain, or a params schema
    // outside the dialect (so a bad descriptor fails the first test run, not a user's session).
    void add(ActionDescriptor descriptor, Runner runner = {}, EnabledFn enabled = {});

    const ActionEntry*              find(const std::string& id) const;
    const ActionEntry*              find_by_method(const std::string& bridge_method) const;
    const std::vector<ActionEntry>& entries() const { return m_entries; }
    size_t                          size() const { return m_entries.size(); }
    void                            clear();

    // Stable hex digest of every descriptor's full JSON (not of live state): the page and the
    // server cache the catalog by it.
    std::string version() const;

private:
    std::vector<ActionEntry>      m_entries;
    std::map<std::string, size_t> m_by_id;
    mutable std::string           m_version; // cached; reset by add/clear
};

// The process-wide catalog, filled once by register_all() (Register.hpp) on first use.
Catalog& catalog();

// Convenience for domain register functions: catalog().add(...).
void add(ActionDescriptor descriptor, Runner runner = {}, EnabledFn enabled = {});

enum class Detail { Summary, Full };
// The wire form of one descriptor (docs/bridge-actions/REFERENCE.md). Live fields (enabled,
// disabledReason) are added by the bridge, not here.
nlohmann::json to_json(const ActionDescriptor& d, Detail detail = Detail::Full);
// {"catalogVersion", "total", "actions": [...full...]}: the snapshot format
// (resources/actions/catalog.snapshot.json).
nlohmann::json catalog_snapshot(const Catalog& c);

// Domains that may never appear (plugins, account/login, process execution, app lifecycle,
// firmware). Checked by Catalog::add and by the catalog test.
bool is_forbidden_domain(const std::string& domain);
// "<domain>.<verb>[.<more>]", lowerCamel segments, ASCII.
bool is_valid_action_id(const std::string& id);

// 64-bit FNV-1a of a string, as 16 lowercase hex digits. Deterministic on every platform.
std::string fnv1a_hex(const std::string& s);

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_ActionCatalog_hpp_
