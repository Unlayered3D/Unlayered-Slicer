#ifndef slic3r_GUI_WebAppDialogs_hpp_
#define slic3r_GUI_WebAppDialogs_hpp_

// The modal dialogs a page's own request put in front of the person (WEB_APP_BRIDGE.md 4.8).
//
// A bridge call such as slicer.addPrinter {ifDirty:"prompt"} can end in one of the slicer's own
// modal dialogs ("unsaved changes: Transfer / Discard / Save"). While it is open the frame is
// disabled, every other call answers 1007 "a dialog is open", and a page that cannot see the
// dialog can only wait for a person who may not be there. So the page may now
//   - read every open modal dialog (slicer.getDialogs): title, text, buttons, check boxes; and
//   - press a button in, or close, one its OWN request opened (slicer.answerDialog).
// It may never press:
//   - a dialog the person opened themselves (Preferences, a print dialog they started ...);
//   - the person's own confirmation: native_confirm, the Save file dialog, the print / send
//     dialogs (HumanOnlyScope) -- those ARE the consent the printer and export families rest on;
//   - any dialog kind outside the allow-list in WebAppDialogs.cpp (unsaved changes, save preset,
//     the slicer's message boxes). Login, plugin, print-host and preference dialogs are not on it.
// Every press is a click the person could have made, delivered after the call returns, and is
// logged; the dialog's own outcome reaches the page as slicer.dialogClosed and as the response
// of the call that opened it.

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace WebAppDialogs {

// Installs the modal-dialog hook (once; later calls do nothing).
void install();

// While one is alive, a modal dialog that opens was opened by a page's request.
struct BridgeScope
{
    BridgeScope();
    ~BridgeScope();
    BridgeScope(const BridgeScope&)            = delete;
    BridgeScope& operator=(const BridgeScope&) = delete;
};

// While one is alive, a modal dialog that opens is the person's own confirmation: a page may
// read it but never answer it.
struct HumanOnlyScope
{
    HumanOnlyScope();
    ~HumanOnlyScope();
    HumanOnlyScope(const HumanOnlyScope&)            = delete;
    HumanOnlyScope& operator=(const HumanOnlyScope&) = delete;
};

// slicer.getDialogs: {"dialogs": [...]}, outermost first; the last one is the one in front.
nlohmann::json list();

// The 1007 message for a frame disabled by a dialog: "a dialog is open" plus, when the hook saw
// it, its title and buttons and whether the page may answer it.
std::string busy_message();

// slicer.answerDialog {dialog?, button}: `button` is a button's label, or "close" (= the title
// bar's X: cancel). Returns a WebAppBridgeError; the press itself runs right after the response.
int answer(const nlohmann::json& params, std::string& message, nlohmann::json& data);

// slicer.dialogOpened / slicer.dialogClosed for every open bridge. Returns a handle for remove.
using Listener = std::function<void(const std::string& method, const nlohmann::json& data)>;
int  add_listener(Listener listener);
void remove_listener(int handle);

}}} // namespace Slic3r::GUI::WebAppDialogs

#endif // slic3r_GUI_WebAppDialogs_hpp_
