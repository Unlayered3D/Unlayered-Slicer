#ifndef slic3r_WebAppBridge_hpp_
#define slic3r_WebAppBridge_hpp_

#include <memory>
#include <string>

#include <nlohmann/json_fwd.hpp>
#include <wx/string.h>

class wxWebViewEvent;

namespace Slic3r {
namespace GUI {

class WebAppPanel;

// Error codes returned to the page. Stable across releases -- see WEB_APP_BRIDGE.md.
enum WebAppBridgeError
{
    WAB_OK                   = 0,
    WAB_BAD_REQUEST          = 1000, // unparseable JSON, or no method
    WAB_UNKNOWN_METHOD       = 1001,
    WAB_BAD_PARAMS           = 1002,
    WAB_FORBIDDEN_ORIGIN     = 1003, // page origin, or download host, not allow-listed
    WAB_BAD_TOKEN            = 1004, // missing or stale session token
    WAB_CAPABILITY_DISABLED  = 1005, // method exists but is gated off for this origin
    WAB_USER_CANCELLED       = 1006,
    WAB_BUSY                 = 1007, // background slicing in progress
    WAB_NOT_FOUND            = 1008,
    WAB_UNSUPPORTED_TYPE     = 1009,
    WAB_IO                   = 1010,
    WAB_TOO_LARGE            = 1011,
    WAB_INTERNAL             = 1012,
    WAB_NEEDS_CONFIRMATION   = 1013, // the call would discard unsaved preset edits; retry with ifDirty
    WAB_SERVICE_MODE_SELECTED = 1014, // slicer.slice: a slicing-service (cloud) mode is selected; only the person starts it
    // 1015 is retired and reserved: the slicer makes no entitlement checks, so it never answers
    // with it. A slicing service refuses a job itself (SLICE_SERVICE_PROTOCOL.md, refusals) and a
    // page shows its own upgrade UI.
    WAB_ACTION_DISABLED      = 1016, // the catalog action exists but is disabled right now (data.reason)
};

// The slicer's side of the Web App bridge: a small, versioned, general-purpose JSON-RPC surface
// that any embedded web application can drive. The protocol is documented in
// WEB_APP_BRIDGE.md and exercised by resources/web/webapp-example.
//
// ---------------------------------------------------------------------------------------------
// Methods that must NOT be added here, and why. Please argue with this list before extending it.
//
//   slicer.readFile / writeFile / listDir / deleteFile
//       Arbitrary filesystem access for a remote page. The whole point of openProjectFromUrl is
//       that the page never needs it. The export family below is not a loophole in this: the
//       page cannot name a path, only read back a file the slicer itself just wrote in its own
//       cache, identified by a handle the slicer minted.
//   slicer.exec / spawn / openPath
//       Arbitrary process execution, or shell-opening an arbitrary path, on behalf of a page.
//   any AppConfig read or write
//       Writing would let a page set print_host, printhost_apikey, download_path -- or the
//       web_app_url / side_panel_url it is itself loaded from, which would make a pivot to an
//       attacker's origin self-perpetuating. Reading exposes slicer_uuid, print host addresses
//       and cloud tokens. Application settings are the user's; the bridge sees none of them.
//   anything touching login or cloud tokens; the network agent only through 4.6 below.
//
// Exception, a deliberate design decision (WEB_APP_BRIDGE.md 4.6): the printer family --
// listPrinters, printerStatus, exportGcode, print, printerControl -- reaches printers the slicer
// already prints to, Bambu ones through the network agent. It is held to: its own per-tab opt-in
// (<slot>_allow_printer, off), a native confirmation for every export / print / control the page
// cannot press (pause and stop excepted), and opaque printer ids -- never an address, access code
// or API key. Reading print_host / printhost_apikey stays inside the slicer for exactly that.
//   any passthrough to GUI_App::handle_web_request
//       That dispatcher belongs to the Home tab and includes the login commands. Routing this
//       bridge through it would collapse the isolation between the two web views.
//
// The open PROJECT can be handed to the page, deliberately and narrowly. Publishing what is on
// the plate otherwise means saving a 3MF by hand and finding it again in a file picker inside an
// embedded web view, which is worse for the user and no safer. The shape is:
//   - three methods: exportProject, readExportChunk, discardExport. Nothing else moves bytes out,
//     except exportThumbnail (a picture of the plate) and exportPrinterPreset (below), which
//     hands back a handle for the same two.
//   - behind their own per-tab opt-in (<slot>_allow_export, default off, and a separate key from
//     allow_settings because it is a different thing to consent to -- the user's model leaving,
//     rather than the slicer's settings being read). Absent from `capabilities` while it is off.
//   - the slicer chooses the path and the name; the page gets an opaque handle. Reads are bounded
//     per call and the export is deleted on discardExport, or when the tab goes away.
//   - it exports what is already on the plate. It cannot reach a file the user has not opened.
//
// Preset SETTINGS -- the values in the Print / Filament / Printer tabs -- ARE exposed,
// deliberately and narrowly. The policy lives in WebAppSettings.hpp; the shape is:
//   - nine methods: listPresets, getSettingsSchema, getSettings, setSettings, getPlate,
//     setCutPlanes (declarative multiplanar cut planes on one object -- they decide how it is
//     sliced exactly as a print setting does, so they are gated with the settings), slice,
//     getSliceResult, plus the widened selectPreset. Nothing else touches presets or the model.
//   - all of them behind a per-tab opt-in (web_app_allow_settings / side_panel_allow_settings,
//     both default off): absent from `capabilities` and answered with 1005 while it is off, so a
//     slicer with the opt-ins off behaves exactly as before they existed.
//   - three deny lists no configuration widens: print-host addresses and credentials are neither
//     read nor written; custom G-code and post_process are never written and read only on
//     request; preset identity (inherits, printer_model, *_settings_id, compatibility ...) is
//     never written. Read-only and plugin-backed options are never written either.
//   - values cross the bridge as the .ini serialisation of a NAMED key, never as a
//     DynamicPrintConfig dump: the page asks for keys and gets strings.
//   - writes land through Tab::load_config, the entry the cut-plane gizmo uses, so they are
//     ordinary unsaved edits: the tab shows "(modified)", nothing is saved to disk, and the
//     user's own dirty edits are never discarded without ifDirty:"discard" saying so (1013).
//
// slicer.selectPreset EXISTS on those terms (see handle_select_preset): an already-installed
// preset, by name or alias, in the printer, print or filament slot -- no create/modify/delete --
// and it behaves like the user choosing the entry from the dropdown. The printer form is the
// original v1 method and works without the settings opt-in, because a page producing G-code for
// a particular printer otherwise gets a preview drawn on the wrong bed; the print and filament
// forms belong to the settings family and are gated with it.
//
// PRINTER PROFILES can be shared, in both directions, and import is the one way a page can add a
// preset (WEB_APP_BRIDGE.md 4.4; policy in WebAppPresetShare.cpp):
//   - slicer.exportPrinterPreset belongs to the export family and its opt-in: the edited printer
//     preset, flattened, read back through the same handle/readExportChunk path as a project.
//     A system or default preset is refused -- it is the vendor's, not the user's, to share.
//   - slicer.installPrinterPreset has its own opt-in (<slot>_allow_install, on for Universe
//     only), downloads on openProjectFromUrl's terms, and installs nothing until the user says so
//     in a native, modal confirmation showing the machine, the parent, and every custom-G-code
//     line that updates firmware, touches files, the network or safety limits, or restarts the
//     controller. That per-install confirmation is the consent; the opt-in only lets a page ask.
//   - a shared profile can never redirect the installer's uploads: print-host addresses,
//     credentials and upload switches (WebAppSettings::WAB_SECRET_KEYS) and local file paths
//     (bed_custom_texture / bed_custom_model) are stripped on export AND again on install,
//     whatever the file says. It is imported as a user preset; nothing else (print or filament
//     presets, bundles, physical printers) can arrive this way.
// ---------------------------------------------------------------------------------------------
class WebAppBridge
{
public:
    explicit WebAppBridge(WebAppPanel& owner);
    ~WebAppBridge();

    // Called after every successful top-level load. Mints and injects a fresh session token when
    // the loaded origin is the configured one.
    void on_document_loaded(const wxString& url);
    void on_script_message(wxWebViewEvent& evt);
    void on_tab_shown(bool shown);

    // Fire-and-forget event to the page (WEB_APP_BRIDGE.md section 4.5). A no-op until a trusted
    // document holds a session token. The settings-family events -- presetChanged,
    // settingsChanged, plateChanged, sliceStarted, sliceFinished -- are also dropped unless this
    // tab's settings opt-in is on. GUI thread; the envelope itself is delivered via CallAfter.
    void notify(const std::string& method, const nlohmann::json& data);
    // slicer.capabilitiesChanged with the current list, for Preferences to call after a toggle.
    void notify_capabilities();

    // "Slice with <tab>" (MainFrame), opening the generic tab, and slicer.requestHandoff (the
    // page): export plate `plate_idx` (-1 = the current one) for this tab's page. With `park`
    // (the user-initiated forms) it is parked as the pending hand-off and announced
    // (slicer.handoff); the page collects the details with slicer.takeHandoff, which also works
    // for a page that loads only afterwards. Without `park` (the page asked, and is waiting on the
    // reply) the details go straight back in `*body` and nothing is announced, so the page never
    // sees the same plate twice. Either way the handle it creates is readable with
    // readExportChunk/discardExport even while <slot>_allow_export is off -- the click, or the
    // settings permission requestHandoff is gated on, is the consent for that one export.
    // GUI thread. Returns an error message, empty on success.
    std::string hand_off_plate(int plate_idx, const std::string& action, bool park = true,
                               nlohmann::json* body = nullptr);

private:
    // shared_ptr, not unique_ptr: an Http worker thread keeps the state alive for the duration of
    // a download callback. See the lifetime note in WebAppBridge.cpp.
    struct Impl;
    std::shared_ptr<Impl> p;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_WebAppBridge_hpp_
