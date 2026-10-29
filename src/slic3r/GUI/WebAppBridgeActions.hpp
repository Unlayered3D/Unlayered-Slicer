#ifndef slic3r_GUI_WebAppBridgeActions_hpp_
#define slic3r_GUI_WebAppBridgeActions_hpp_

// The action-catalog half of the Web App bridge (docs/HLSD/bridge-actions.md, WEB_APP_BRIDGE.md
// 4.7). WebAppBridge.cpp keeps one hook into it:
//   - every request runs inside an Actions::RequestScope (turn bookkeeping for slicer.undoTurn);
//   - methods it does not handle itself go to dispatch_action_method before "unknown method";
//   - capabilities() appends append_action_capabilities, getInfo appends append_action_info;
//   - Impl::notify forwards every event to on_bridge_event (settings writes for undoTurn,
//     capabilitiesChanged -> actionsChanged).
//
// Methods served here, all behind <slot>_allow_settings:
//   slicer.listActions  {domain?, query?, ids?, detail?: "summary"|"full", includeDisabled?: true}
//   slicer.invokeAction {id, target?, params?, turn?, wait?: true, ifDirty?, ifProjectDirty?}
//   slicer.getSelection {}
//   slicer.select       {objects?, volumes?, instances?, plate?}   (a target, ActionTargets.hpp)
//   slicer.undoTurn     {turn}
//   plus every typed method a catalog entry serves with a runner (descriptor.bridge_method).
// Events: slicer.actionFinished, slicer.selectionChanged, slicer.actionsChanged.

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "Actions/ActionRuntime.hpp"

namespace Slic3r { namespace GUI {

// True when `method` was handled (a response has been or will be sent).
bool dispatch_action_method(Actions::BridgeHost& host, const std::string& id, const std::string& method,
                            const nlohmann::json& params);

// Adds the action methods this tab may call to `capabilities` (a JSON array of method names).
void append_action_capabilities(const Actions::BridgeHost& host, nlohmann::json& capabilities);

// getInfo additions: features += "actions.v1"; "actions": {version, count}; "account": {signedIn, provider}.
void append_action_info(const Actions::BridgeHost& host, nlohmann::json& info);

// Every event the bridge sends passes through here (after its own gating).
void on_bridge_event(Actions::BridgeHost& host, const std::string& method, const nlohmann::json& data);

// Starts forwarding GUI selection changes as slicer.selectionChanged for this host (once per host;
// harmless before the Plater exists -- it retries on the next call).
void watch_selection(const std::shared_ptr<Actions::BridgeHost>& host);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_WebAppBridgeActions_hpp_
