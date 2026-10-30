#include "WebAppBridge.hpp"
#include "WebAppBridgeActions.hpp" // the action catalog (docs/HLSD/bridge-actions.md); one hook, marked "Actions:"
#include "WebAppDialogs.hpp" // the modal dialogs a page's own request opened (getDialogs / answerDialog)
#include "Actions/NativeConfirm.hpp"
#include "Actions/PresetLogic.hpp"
#include "Actions/BedDrop.hpp"
#include "Actions/CutPlaneRules.hpp"
#include "Actions/ModelTree.hpp" // cut_planes_json
#include "Actions/ActionRuntime.hpp" // gui_guard: typed model edits refuse under an open gizmo as catalog runners do
#include "WebAppPanel.hpp"
#include "WebAppPresetShare.hpp"
#include "WebAppPrinters.hpp"
#include "Actions/NativeConfirm.hpp"
#include "Actions/PrinterRules.hpp"
#include "PrintHostDialogs.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "WebAppSettings.hpp"
#include "WebAppSlot.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/GUI.hpp" // from_path
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp" // validate(): why a slice did not start
#include "slic3r/GUI/ProjectAside.hpp"
#include "slic3r/GUI/StepMeshDialog.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/UnsavedChangesDialog.hpp" // ActionButtons
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/GUI/ModelUploadConsentDialog.hpp"
#include "slic3r/Utils/PrivacyConsent.hpp"
#include "slic3r/Utils/DuetRrfClient.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevConfig.h"
#include "slic3r/GUI/DeviceCore/DevFan.h"
#include "slic3r/GUI/GLToolbar.hpp" // EVT_GLTOOLBAR_PRINT_PLATE
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/Utils/FirmwareBundle.hpp" // sha256_hex
#include "libslic3r/GCode/Thumbnails.hpp"     // compress_thumbnail
#include "libslic3r/GCode/ThumbnailData.hpp" // ThumbnailData
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/PlatePrintability.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include <wx/utils.h> // wxCheckOsVersion (getInfo printerAccessSetting, macOS)

#include <boost/beast/core/detail/base64.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/any.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <vector>
#include <atomic>
#include <cfloat>
#include <functional>
#include <iomanip>
#include <memory>
#include <random>
#include <thread>
#include <boost/algorithm/string/join.hpp>
#include <sstream>
#include <vector>

#include <wx/timer.h>
#include <wx/uri.h>
#include <wx/webview.h>

using json = nlohmann::json;
namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

constexpr int    WAB_PROTOCOL_VERSION   = 1;
constexpr size_t WAB_MAX_DOWNLOAD_BYTES = 256u * 1024u * 1024u;
constexpr size_t WAB_PROGRESS_STEP      = 1024u * 1024u; // emit a progress event per MiB
// A printer profile is a few dozen KiB of JSON; anything near this is not one.
constexpr size_t WAB_MAX_PRESET_BYTES   = 4u * 1024u * 1024u;

// The public home of this program's Corresponding Source. Reported by slicer.getInfo so that an
// embedded application -- including one hosted on a server that runs this slicer for remote
// users -- can render the offer required by AGPL-3.0 sections 6 and 13 without hard-coding it.
const char* const WAB_SOURCE_URL = "https://github.com/Unlayered3D/Unlayered-Slicer";

// "gco"/"g" used to be listed too, but Slic3r::is_gcode_file() (libslic3r/utils.cpp) accepts only
// ".gcode", so Plater::load_gcode() silently refused them: advertising a type that can never open
// is a lie to the page. Keep this list to what open_resolved_file() can actually route.
const char* const WAB_FILE_TYPES[] = {"3mf", "stl", "step", "stp", "obj", "amf", "svg", "drc", "gcode"};

std::string json_string(const json& node, const char* key)
{
    auto it = node.find(key);
    return (it != node.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// Defined further down with json_bool; forward-declared here because the preset handlers above that
// point call it.
int json_int(const json& node, const char* key, int fallback);
bool json_bool(const json& node, const char* key, bool fallback);
PresetCollection* collection_for(PresetBundle& bundle, Preset::Type type);
bool resolve_hidden_alias(PresetCollection& collection, std::string& name);
bool prefer_compatible_filament(PresetCollection& collection, std::string& name);
void load_installed_filaments_for(PresetBundle& bundle, AppConfig& cfg);

std::string dump_json(const json& node) { return node.dump(-1, ' ', false, json::error_handler_t::replace); }

fs::path path_from_utf8(const std::string& utf8_path)
{
#ifdef _WIN32
    const wxString wide_path = wxString::FromUTF8(utf8_path.c_str());
    return fs::path(wide_path.ToStdWstring());
#else
    return fs::path(utf8_path);
#endif
}

std::string path_to_utf8(const fs::path& path)
{
#ifdef _WIN32
    return std::string(wxString(path.wstring()).ToUTF8().data());
#else
    return path.string();
#endif
}

std::string make_session_token()
{
    std::random_device rd;
    std::ostringstream oss;
    for (int i = 0; i < 8; ++i)
        oss << std::hex << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(rd());
    return oss.str(); // 64 hex characters, 256 bits
}

bool extension_allowed(const std::string& lowercase_ext)
{
    for (const char* ext : WAB_FILE_TYPES)
        if (lowercase_ext == ext)
            return true;
    return false;
}

std::string lowercase_extension(const fs::path& path)
{
    std::string ext = path_to_utf8(path.extension());
    if (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    return boost::algorithm::to_lower_copy(ext);
}

// Reduce a client-supplied name to a bare filename. Never trusted as a path.
std::string sanitise_filename(const std::string& raw)
{
    std::string name = raw;
    for (char& c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '\0')
            c = '_';
    while (!name.empty() && name.front() == '.') // a leading dot is how ".." gets through
        name.erase(name.begin());
    if (name.size() > 128)
        name = name.substr(name.size() - 128);
    return name;
}

fs::path webapp_cache_dir() { return path_from_utf8(data_dir()) / "cache" / "webapp"; }

// Roots that a local path handed to slicer.openProject must sit under. Configurable per tab
// (slot.key_path_roots); when the setting is empty a conservative default applies. There is
// deliberately no "unrestricted" value -- an application wanting to hand over files from
// elsewhere should serve them over HTTP and use slicer.openProjectFromUrl.
std::vector<fs::path> allowed_path_roots(const WebAppSlot& slot)
{
    std::vector<fs::path> roots;
    AppConfig*            cfg = wxGetApp().app_config;

    const std::string configured = cfg != nullptr ? cfg->get(slot.key_path_roots) : std::string();
    if (!configured.empty()) {
        std::vector<std::string> parts;
        boost::algorithm::split(parts, configured, boost::algorithm::is_any_of(";"));
        for (std::string& part : parts) {
            boost::algorithm::trim(part);
            if (part.empty())
                continue;
            boost::system::error_code ec;
            const fs::path            canonical = fs::weakly_canonical(path_from_utf8(part), ec);
            if (!ec)
                roots.push_back(canonical);
        }
        if (!roots.empty())
            return roots;
    }

    boost::system::error_code ec;
    roots.push_back(fs::weakly_canonical(webapp_cache_dir(), ec));
    const fs::path temp = fs::temp_directory_path(ec);
    if (!ec)
        roots.push_back(fs::weakly_canonical(temp, ec));
    if (cfg != nullptr) {
        const std::string downloads = cfg->get("download_path");
        if (!downloads.empty()) {
            const fs::path d = fs::weakly_canonical(path_from_utf8(downloads), ec);
            if (!ec)
                roots.push_back(d);
        }
    }
    return roots;
}

bool path_is_under(const fs::path& candidate, const fs::path& root)
{
    auto c = candidate.begin();
    auto r = root.begin();
    for (; r != root.end(); ++r, ++c) {
        if (c == candidate.end())
            return false;
#ifdef _WIN32
        if (!boost::algorithm::iequals(path_to_utf8(*c), path_to_utf8(*r)))
            return false;
#else
        if (*c != *r)
            return false;
#endif
    }
    return true;
}

std::vector<std::string> configured_download_hosts(const WebAppSlot& slot)
{
    std::vector<std::string> hosts;
    AppConfig*               cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return hosts;
    // Configuration wins; otherwise the slot's own default, so a tab whose storage host the build
    // already knows works without anyone having to type it in.
    std::string configured = cfg->get(slot.key_download_hosts);
    if (configured.empty() && slot.download_hosts != nullptr)
        configured = slot.download_hosts;
    if (configured.empty())
        return hosts;
    boost::algorithm::split(hosts, configured, boost::algorithm::is_any_of(";,"));
    for (std::string& h : hosts) {
        boost::algorithm::trim(h);
        boost::algorithm::to_lower(h);
    }
    hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [](const std::string& h) { return h.empty(); }), hosts.end());
    return hosts;
}

std::string host_of(const wxString& url)
{
    const wxURI uri(url);
    if (!uri.HasServer())
        return std::string();
    return std::string(uri.GetServer().Lower().ToUTF8().data());
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Impl
//
// Lifetime: Impl is held by shared_ptr so that an Http worker thread can keep it alive for the
// duration of a callback. The WebAppPanel it refers to has a *shorter* lifetime -- the panel owns
// the bridge -- so `owner` may only ever be touched on the GUI thread, after checking `detached`.
// Worker threads do local work and marshal everything else through wxGetApp().CallAfter.
// ---------------------------------------------------------------------------------------------

struct WebAppBridge::Impl : public std::enable_shared_from_this<WebAppBridge::Impl>
{
    explicit Impl(WebAppPanel& owner_) : owner(owner_) {}

    WebAppPanel&      owner;
    std::string       session_token;                // GUI thread only
    std::atomic<bool> detached{false};              // set by ~WebAppBridge on the GUI thread
    std::atomic<bool> download_busy{false};
    std::atomic<size_t> progress_mark{0};
    // Actions: the catalog runtime's view of this bridge (WebAppBridgeActions.hpp). Built by the
    // WebAppBridge constructor, which can hand it weak references to this Impl.
    std::shared_ptr<Actions::BridgeHost> action_host;
    int dialog_listener = 0; // WebAppDialogs: slicer.dialogOpened / dialogClosed to this page

    void send_envelope(const json& body);
    void send_response(const std::string& id, const std::string& method, int code, const std::string& message,
                       const json& data = json::object());
    void send_event(const std::string& id, const std::string& method, const json& data);

    void inject_bridge_script();

    // The method list slicer.getInfo reports and slicer.capabilitiesChanged re-reports.
    json capabilities() const;
    // Event delivery with the token and settings-opt-in checks; WebAppBridge::notify forwards here.
    void notify(const std::string& method, const json& data);

    void handle_get_info(const std::string& id);
    void handle_open_project(const std::string& id, const json& params);
    void handle_open_project_from_url(const std::string& id, const json& params);
    void handle_select_preset(const std::string& id, const json& params);

    // Printer-profile sharing (WEB_APP_BRIDGE.md section 4.4; policy in WebAppPresetShare.cpp).
    void handle_export_printer_preset(const std::string& id, const json& params);
    void handle_install_printer_preset(const std::string& id, const json& params);
    // From an accepted installPrinterPreset until its slicer.presetInstalled event is sent.
    std::atomic<bool> install_in_progress{false};

    // Printers (WEB_APP_BRIDGE.md 4.6), behind owner.allows_printer(). Ids are opaque and live
    // for this bridge: "bbl:<dev_id>" or "host" never leaves the slicer.
    std::map<std::string, std::string> printer_id_of;  // key -> opaque id
    std::map<std::string, std::string> printer_key_of; // opaque id -> key
    bool                               print_in_progress{false};
    std::string printer_id_for(const std::string& key);
    bool        resolve_printer(const std::string& id, const char* method, const json& params, std::string& key);
    bool        printer_gate(const std::string& id, const char* method);
    void        handle_list_printers(const std::string& id, const json& params);
    void        handle_printer_status(const std::string& id, const json& params);
    void        handle_export_gcode(const std::string& id, const json& params);
    void        handle_print(const std::string& id, const json& params);
    void        handle_press_print(const std::string& id, const json& params);
    void        handle_printer_control(const std::string& id, const json& params);
    // Send to the printer's storage, select a printer, and the print-host upload queue.
    void        handle_send_to_printer(const std::string& id, const json& params);
    void        handle_select_printer(const std::string& id, const json& params);
    void        handle_print_host_jobs(const std::string& id, const json& params);
    void        handle_cancel_print_host_job(const std::string& id, const json& params);
    // Checks slicer.print / sendToPrinter's `plate` against the slice results; answers and returns false.
    bool        check_print_plate(const std::string& id, const char* method, const json& params, bool& all);

    // The download machinery openProjectFromUrl and installPrinterPreset share. The URL rules: http
    // or https, and the page's own host unless the slot's download-host list names it. Returns
    // WAB_OK, or the code and message to answer with.
    int  check_download_url(const std::string& url, std::string& message) const;
    // One download at a time per tab: claim the slot before answering "accepted", and on failure
    // answer 1007 instead.
    bool claim_download();
    struct DownloadJob
    {
        std::string id;
        std::string url;
        fs::path    destination;
        std::string expected_sha;   // lowercase hex; empty = not checked
        size_t      size_limit{WAB_MAX_DOWNLOAD_BYTES};
        std::string progress_event; // empty = no progress events
        // Worker thread, slot released, tab still alive: the request failed (network, HTTP status,
        // size limit). May only use what is thread-safe here -- send_event.
        std::function<void(Impl& self, const std::string& message, unsigned status)> on_http_error;
        // GUI thread, slot released, tab still alive: the body failed its checksum or could not be
        // written. Nothing is left at `destination`.
        std::function<void(Impl& self, const std::string& error)> on_save_error;
        // GUI thread, slot released, tab still alive: the verified file is at `destination`.
        std::function<void(Impl& self, const fs::path& destination)> on_ready;
    };
    // Runs `job` on an Http worker thread. The slot must have been claimed.
    void run_download(DownloadJob job);

    // Settings family (WEB_APP_BRIDGE.md section 4; policy in WebAppSettings.cpp).
    void handle_list_presets(const std::string& id, const json& params);
    void handle_get_settings_schema(const std::string& id, const json& params);
    void handle_get_settings(const std::string& id, const json& params);
    void handle_set_settings(const std::string& id, const json& params);
    void handle_get_plate(const std::string& id);
    // Declarative multiplanar cut planes on one object (Model.hpp CutPlaneStack).
    void handle_set_cut_planes(const std::string& id, const json& params);
    // Rotate one object (a face down, or degrees about the plate axes) and drop it onto the bed.
    void handle_set_orientation(const std::string& id, const json& params);
    // Assign one object to a filament slot (the object list's filament column).
    void handle_set_object_filament(const std::string& id, const json& params);
    // Native plate edits (scale, move, arrange, copies, fill bed) -- what the sidebar's Scale/Move
    // gizmos, Arrange, +/- instance and Fill Bed do, one object at a time, each undoable.
    void handle_set_scale(const std::string& id, const json& params);
    void handle_move_object(const std::string& id, const json& params);
    void handle_arrange(const std::string& id, const json& params);
    void handle_set_copies(const std::string& id, const json& params);
    void handle_fill_bed(const std::string& id, const json& params);
    // Arrange and Fill Bed are Plater jobs. This calls done() on the main thread once the job worker
    // is idle again (timed_out = true after timeout_ms), so those methods answer on completion.
    // 55 s: inside the page shim's fixed 60 s call timeout, so the page always hears the outcome.
    void when_plate_job_done(std::function<void(bool timed_out)> done, int timeout_ms = 55000);
    // One bridge-started plate job at a time; a second is refused with 1007.
    bool plate_job_pending{false};
    void handle_export_project(const std::string& id, const json& params);
    void handle_read_export_chunk(const std::string& id, const json& params);
    void handle_discard_export(const std::string& id, const json& params);
    void handle_export_thumbnail(const std::string& id, const json& params);
    // Exports this bridge has handed out, by opaque handle. The page never learns or supplies a
    // path: it can only read back a file the slicer itself just wrote for it, which is what keeps
    // this from being the slicer.readFile the header forbids.
    std::map<std::string, boost::filesystem::path> exports;
    void discard_all_exports();
    // Exports made by a user hand-off (hand_off_plate): readable without the export opt-in.
    std::set<std::string> handoff_handles;
    // The hand-off the page has not collected yet (slicer.takeHandoff), or null.
    json pending_handoff;
    // plate_fingerprint() of the last plate handed to this tab, so showing the tab again with
    // nothing changed does not export the same plate again. 0 = never.
    size_t last_handoff_fp = 0;
    void handle_take_handoff(const std::string& id);
    // The page asks for the plate itself (e.g. a side panel, before it acts on the plate).
    void handle_request_handoff(const std::string& id, const json& params);
    void handle_slice(const std::string& id, const json& params);
    bool refuse_nothing_printable(const std::string& id, const char* method, int plate);
    void handle_get_slice_modes(const std::string& id);
    void handle_set_slice_mode(const std::string& id, const json& params);
    json slice_modes_json() const;
    void handle_get_slice_result(const std::string& id, const json& params);

    // Common front matter of every settings-family call: the opt-in, "a dialog is open", "the
    // slicer is slicing" (writes only) and the tab for `type` (TYPE_INVALID: none needed).
    // Returns WAB_OK or the code to answer with; `message` is the reply text.
    // `require_settings` is the per-tab print-settings opt-in. The export family does its own
    // gate check against a DIFFERENT permission and passes false, so a tab allowed to publish the
    // plate is not silently refused for lacking a settings permission it never needed.
    int  preflight(bool write, Preset::Type type, Tab*& tab, std::string& message,
                   bool require_settings = true) const;
    // The ifDirty contract of every call that switches `type` to `target`
    // (Actions::preset_switch_preflight, shared with slicer.invokeAction): the switched preset's
    // edits and its dependents' (a printer switch asks about print and filament edits too).
    // "ask" (default) answers 1013 with the dirty keys, "discard" drops the edits, "save" saves
    // them into the person's own preset, "transfer" carries them over (not for printers),
    // "prompt" leaves the usual dialog to Tab::select_preset. `data` receives the error payload.
    int  dirty_preflight(Preset::Type type, const std::string& target, const std::string& if_dirty, json& data,
                         std::string& message) const;
    // A typed method that edits the model: refuses (and answers) while a gizmo is open or a plate
    // job runs, with the same code, message and data.gizmo as a catalog runner. True = refused.
    bool refuse_model_edit(const std::string& id, const char* method);
    // Name or alias, "(modified)" suffix removed, to the installed preset's name. False if none.
    bool resolve_preset_name(PresetBundle& bundle, Preset::Type type, std::string& name) const;
    // A hidden SYSTEM preset becomes visible -- what ticking it in the filament selection (filament)
    // or the printer's model + nozzle in the setup wizard (printer) does -- and the dropdowns are
    // refreshed. False (nothing changed) for a preset that is not a hidden system preset.
    bool enable_hidden_preset(PresetBundle& bundle, Preset::Type type, const std::string& name);

    // Vendor catalog (2026-10-21): find a vendor's own preset, add a
    // filament slot, keep an edited filament as a named user preset, add a shipped printer.
    void handle_add_filament_slot(const std::string& id, const json& params);
    void handle_save_filament_preset(const std::string& id, const json& params);
    void handle_list_printer_models(const std::string& id, const json& params);
    void handle_add_printer(const std::string& id, const json& params);

    int  validate_local_path(const std::string& raw_path, fs::path& resolved, std::string& message);
    bool slicer_is_busy() const;
    // `printer` (G-code only, already resolved): selected after the project is set aside, before the open.
    void open_resolved_file(const std::string& id, const std::string& method, const fs::path& path,
                            const std::string& printer = {});
};

// Thread-safe: builds the script from locals, then hops to the GUI thread to run it.
void WebAppBridge::Impl::send_envelope(const json& body)
{
    const wxString payload = wxString::FromUTF8(dump_json(body).c_str());
    const wxString script  = "(function(p){"
                             "  if (window.__slicerBridge && typeof window.__slicerBridge._recv === 'function')"
                             "    return window.__slicerBridge._recv(p);"
                             "  if (typeof HandleStudio === 'function') return HandleStudio(p);"
                             "  window.postMessage(p, window.location.origin);"
                             "})(" + payload + ");";

    std::weak_ptr<Impl> weak = weak_from_this();
    wxGetApp().CallAfter([weak, script]() {
        auto self = weak.lock();
        if (!self || self->detached.load())
            return;
        if (self->owner.browser() == nullptr)
            return;
        // UNLAYERED_MOD (2026-10-25): a big reply (a 4 MB export chunk is ~5.6 MB of script) is
        // handed to WebView2 on this thread; say when that is slow, for the freeze hunt.
        const auto start = std::chrono::steady_clock::now();
        WebView::RunScript(self->owner.browser(), script);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (ms >= 250)
            BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: posting " << script.length() << " characters to the page took " << ms << " ms";
    });
}

void WebAppBridge::Impl::send_response(const std::string& id, const std::string& method, int code,
                                       const std::string& message, const json& data)
{
    if (id.empty())
        return; // fire-and-forget request: nothing to correlate a response with

    json body       = json::object();
    body["type"]    = "response";
    body["v"]       = WAB_PROTOCOL_VERSION;
    body["id"]      = id;
    body["method"]  = method;
    body["code"]    = code;
    body["message"] = message;
    body["data"]    = data;
    send_envelope(body);
}

void WebAppBridge::Impl::send_event(const std::string& id, const std::string& method, const json& data)
{
    json body    = json::object();
    body["type"] = "event";
    body["v"]    = WAB_PROTOCOL_VERSION;
    if (!id.empty())
        body["id"] = id;
    body["method"] = method;
    body["data"]   = data;
    send_envelope(body);
}

void WebAppBridge::Impl::inject_bridge_script()
{
    wxWebView* browser = owner.browser();
    if (browser == nullptr || session_token.empty())
        return;

    // Injected with RunScript rather than AddUserScript on purpose: it lands only in the main
    // frame of a document whose origin we have already checked, and it means we never need
    // RemoveAllUserScripts(), which would also drop the "wx" script message handler.
    const wxString script = wxString::Format(
        "(function(){"
        "  var TOKEN=%s;"
        "  var seq=0, pending={}, listeners={};"
        "  function send(method, params){"
        "    return new Promise(function(resolve, reject){"
        "      if(!window.wx || typeof window.wx.postMessage!=='function'){"
        "        reject({code:'no-bridge', message:'not running inside the slicer'}); return;"
        "      }"
        "      var id='wa-'+(++seq);"
        "      var t=setTimeout(function(){ if(pending[id]){ delete pending[id];"
        "        reject({code:'timeout', message:'no response from the slicer', method:method}); } }, 60000);"
        "      pending[id]={resolve:resolve, reject:reject, timer:t};"
        "      window.wx.postMessage(JSON.stringify({v:1, id:id, method:method, token:TOKEN, params:params||{}}));"
        "    });"
        "  }"
        "  function emit(evt, data){"
        "    var fns=listeners[evt]||[];"
        "    for(var i=0;i<fns.length;i++){ try{ fns[i](data); }catch(e){} }"
        "  }"
        "  function recv(p){"
        "    if(!p) return;"
        "    if(typeof p==='string'){ try{ p=JSON.parse(p); }catch(e){ return; } }"
        "    if(p.type==='event'){ emit(p.method, p.data); return; }"
        "    var entry=pending[p.id];"
        "    if(!entry) return;"
        "    delete pending[p.id]; clearTimeout(entry.timer);"
        "    if(p.code===0) entry.resolve(p.data||{});"
        "    else entry.reject({code:p.code, message:p.message||'error', method:p.method, data:p.data||{}});"
        "  }"
        "  function on(evt, fn){ (listeners[evt]=listeners[evt]||[]).push(fn); }"
        "  function off(evt, fn){ var a=listeners[evt]||[]; var i=a.indexOf(fn); if(i>=0) a.splice(i,1); }"
        "  window.__slicerBridge=Object.freeze({version:1, token:TOKEN, send:send, on:on, off:off, _recv:recv});"
        "  try{ window.dispatchEvent(new Event('slicerbridgeready')); }catch(e){}"
        "})();",
        wxString::FromUTF8(json(session_token).dump().c_str()));

    WebView::RunScript(browser, script);
}

bool WebAppBridge::Impl::slicer_is_busy() const
{
    Plater* plater = wxGetApp().plater();
    return plater != nullptr && plater->is_background_process_slicing();
}

// ---------------------------------------------------------------------------------------------
// slicer.getInfo
// ---------------------------------------------------------------------------------------------

json WebAppBridge::Impl::capabilities() const
{
    const bool loopback    = owner.origin_is_loopback();
    AppConfig* cfg         = wxGetApp().app_config;
    const bool allow_local = loopback || (cfg != nullptr && cfg->get_bool(owner.slot().key_allow_local));

    // Computed at runtime so the page feature-detects rather than hard-coding what it may call.
    // With the settings opt-in off this is exactly the list protocol v1 shipped with.
    json capabilities = json::array({"slicer.getInfo", "slicer.openProjectFromUrl", "slicer.selectPreset",
                                     "slicer.takeHandoff"});
    if (allow_local)
        capabilities.push_back("slicer.openProject");
    if (owner.allows_settings())
        for (const char* method : {"slicer.listPresets", "slicer.getSettingsSchema", "slicer.getSettings", "slicer.setSettings",
                                   "slicer.getPlate", "slicer.setCutPlanes", "slicer.setOrientation", "slicer.setObjectFilament", "slicer.setScale",
                                   "slicer.moveObject", "slicer.arrange", "slicer.setCopies", "slicer.fillBed", "slicer.slice",
                                   "slicer.getSliceResult", "slicer.getSliceModes", "slicer.setSliceMode",
                                   "slicer.addFilamentSlot", "slicer.saveFilamentPreset", "slicer.listPrinterModels",
                                   "slicer.addPrinter", "slicer.getDialogs", "slicer.answerDialog",
                                   "slicer.pressPrint"})
            capabilities.push_back(method);
    if (owner.allows_export())
        for (const char* method : {"slicer.exportProject", "slicer.readExportChunk", "slicer.discardExport",
                                   "slicer.exportThumbnail", "slicer.exportPrinterPreset"})
            capabilities.push_back(method);
    if (owner.allows_install())
        capabilities.push_back("slicer.installPrinterPreset");
    if (owner.allows_printer())
        for (const char* method : {"slicer.listPrinters", "slicer.printerStatus", "slicer.exportGcode", "slicer.print",
                                   "slicer.printerControl", "slicer.sendToPrinter", "slicer.selectPrinter", "slicer.printHostJobs",
                                   "slicer.cancelPrintHostJob"})
            capabilities.push_back(method);
    // The page may pull the plate on either permission (handle_request_handoff explains why).
    if (owner.allows_settings() || owner.allows_export())
        capabilities.push_back("slicer.requestHandoff");
    if (action_host) // Actions: listActions, invokeAction, getSelection, select, undoTurn + typed catalog methods
        append_action_capabilities(*action_host, capabilities);
    return capabilities;
}

void WebAppBridge::Impl::handle_get_info(const std::string& id)
{
    const WebAppSlot& slot = owner.slot();

    json types = json::array();
    for (const char* ext : WAB_FILE_TYPES)
        types.push_back(ext);

    json data    = json::object();
    data["api"]  = {{"version", WAB_PROTOCOL_VERSION}, {"envelope", "slicer-rpc/1"}};
    data["app"]  = {{"name", SLIC3R_APP_NAME},
                    {"version", UNLAYERED_VERSION},
                    {"coreVersion", SLIC3R_VERSION},
                    {"sourceUrl", WAB_SOURCE_URL},
                    {"license", "AGPL-3.0"}};
    data["platform"] = {{"os",
#if defined(_WIN32)
                         "windows"
#elif defined(__APPLE__)
                         "macos"
#else
                         "linux"
#endif
                        },
                        {"locale", std::string(wxGetApp().current_language_code().ToUTF8().data())},
                        {"darkMode", GUI_App::dark_mode()}};
    data["capabilities"] = capabilities();
    data["limits"]       = {{"maxDownloadBytes", WAB_MAX_DOWNLOAD_BYTES}, {"fileTypes", types}};
    data["origin"]       = {{"configured", std::string(WebAppPanel::configured_url(slot).ToUTF8().data())},
                            {"trusted", owner.origin_is_allowed()},
                            {"loopback", owner.origin_is_loopback()}};
    // Which of the slots this bridge serves, how MainFrame hosts it (a notebook page, or the
    // sidebar beside the tabs -- the side panel), and whether the settings family is open to it.
    data["tab"]            = {{"slot", slot.id},
                              {"title", std::string(WebAppPanel::configured_title(slot).ToUTF8().data())},
                              {"layout", owner.hosted_as_sidebar() ? "sidebar" : "tab"}};
    data["settingsAccess"] = owner.allows_settings();
    data["printerAccess"]  = owner.allows_printer(); // WEB_APP_BRIDGE.md 4.6
    // Where the person turns printerAccess on, in the dialog's own (translated) words and this
    // platform's menu path, so a page never gives stale directions (feature getInfo.printerAccessSetting).
    // `path` leads to the section holding `label`; null when this build has no such switch for this
    // slot (the switch is in the dev edition only, and only the side panel has one).
    data["printerAccessSetting"] = nullptr;
    // The person's privacy choice (0.1.6 consent), read live on every call. Under notice 2026-10-v3
    // the ONLY consent for AI use is the "Slicing improvement program" box, so `aiTraining` and
    // `program` are PrivacyConsent::program_permitted (diagnostics permitted AND the box ticked),
    // never the diagnostics choice alone; a page that keeps a person's records or ratings for
    // training should do so only when it is true. `status`: notAsked / declined / granted /
    // needsNewChoice.
    {
        const AppConfig* cfg    = wxGetApp().app_config;
        const char*      status = "notAsked";
        bool             permitted = false;
        bool             program   = false;
        if (cfg != nullptr) {
            switch (PrivacyConsent::status(PrivacyConsent::load(*cfg), PrivacyConsent::legal_approvals())) {
            case PrivacyConsent::Status::Declined: status = "declined"; break;
            case PrivacyConsent::Status::Granted: status = "granted"; break;
            case PrivacyConsent::Status::NeedsNewChoice: status = "needsNewChoice"; break;
            default: break;
            }
            permitted = PrivacyConsent::diagnostics_permitted(*cfg);
            program   = PrivacyConsent::program_permitted(*cfg);
        }
        data["privacy"] = {{"status", status}, {"diagnosticsPermitted", permitted}, {"aiTraining", program},
                           {"program", program}};
    }
    // Which slicer window this is, and which project in it (UNLAYERED_MOD, 0.2.0). A reload of the
    // page in the same window reads the same instanceId; a new window or a second launch reads a
    // different one. projectGeneration counts the project switches slicer.projectChanged announced.
    data["instanceId"]        = WebAppPanel::instance_id();
    data["projectGeneration"] = WebAppPanel::project_generation();
    // Behaviours of existing methods a page cannot infer from `capabilities` (method names only).
    data["features"] = json::array({"openProjectFromUrl.printer", "openGcode.keepsProject", "getInfo.privacy",
                                    "settingsChanged.bridgeChanges", "listPresets.includeHidden", "selectPreset.allowHidden",
                                    "getInfo.instanceId", "getInfo.projectGeneration", "projectChanged",
                                    "event.projectChanged", "dialogs.v1", "getInfo.printerAccessSetting"});
    if (action_host) // Actions: features += actions.v1, "actions" {version, count}, "account" {signedIn, provider}
    {
        append_action_info(*action_host, data);
        watch_selection(action_host); // the Plater exists by the time a page asks
    }

    send_response(id, "slicer.getInfo", WAB_OK, "success", data);
}

// ---------------------------------------------------------------------------------------------
// Opening files
// ---------------------------------------------------------------------------------------------

int WebAppBridge::Impl::validate_local_path(const std::string& raw_path, fs::path& resolved, std::string& message)
{
    if (raw_path.empty() || raw_path.size() > 4096) {
        message = "path is empty or absurdly long";
        return WAB_BAD_PARAMS;
    }

#ifdef _WIN32
    // A UNC path would make the slicer authenticate to a server the page named, leaking an NTLM
    // hash to it. Never follow one on a page's say-so.
    if (raw_path.rfind("\\\\", 0) == 0 || raw_path.rfind("//", 0) == 0) {
        message = "UNC paths are not accepted";
        return WAB_FORBIDDEN_ORIGIN;
    }
#endif

    boost::system::error_code ec;
    resolved = fs::weakly_canonical(fs::absolute(path_from_utf8(raw_path)), ec);
    if (ec) {
        message = "path could not be resolved";
        return WAB_NOT_FOUND;
    }

    if (!fs::is_regular_file(resolved, ec) || ec) {
        message = "not a regular file";
        return WAB_NOT_FOUND;
    }

    const std::string ext = lowercase_extension(resolved);
    if (!extension_allowed(ext)) {
        message = "file type '" + ext + "' is not accepted";
        return WAB_UNSUPPORTED_TYPE;
    }

    const auto size = fs::file_size(resolved, ec);
    if (!ec && size > WAB_MAX_DOWNLOAD_BYTES) {
        message = "file is too large";
        return WAB_TOO_LARGE;
    }

    const std::vector<fs::path> roots  = allowed_path_roots(owner.slot());
    const bool                  inside = std::any_of(roots.begin(), roots.end(), [&resolved](const fs::path& root) {
        return path_is_under(resolved, root);
    });
    if (!inside) {
        message = "path is outside the folders this application may open from; serve the file over HTTP and use "
                  "slicer.openProjectFromUrl, or add the folder to " + std::string(owner.slot().key_path_roots);
        return WAB_FORBIDDEN_ORIGIN;
    }

    return WAB_OK;
}

// Reports slicer.projectOpened. Note precisely what ok:true means, because it is
// weaker than it looks: the file has been fetched, verified and handed to the
// slicer's own open path, which then runs asynchronously and may still put a
// "discard the current project?" modal in front of the user. There is no
// completion callback to hook -- request_open_project() and load_files() do not
// offer one -- so a user who cancels that modal still sees ok:true. Everything
// that can actually go wrong before that point (bad host, bad type, missing file,
// failed download, checksum mismatch, busy slicer) is reported accurately.
// WEB_APP_BRIDGE.md documents this limitation; do not let a client build on a
// stronger reading of it.
//
// G-code is its own case. Plater::load_files(std::vector<std::string>) is the MODEL
// import path and does not understand G-code at all -- only the wxArrayString
// drag-and-drop overload splits it off -- so a .gcode handed over the bridge used to
// fail even though fileTypes advertised it. Plater::load_gcode() is the right entry:
// it enters G-code-preview mode (m_only_gcode; sidebar and Slice disabled until a new
// project) and selects the Preview tab itself. Two of its behaviours reach the page:
//   - stock load_gcode() closes the project first (the "unsaved changes, save?" dialog). Here
//     the project is SET ASIDE instead (ProjectAside.hpp): stashed to a 3MF and closed with no
//     prompt, restored from the Prepare tab or the preview's notification. Only when the stash
//     cannot be written does the old dialog come back (Cancel there still reports ok:true).
//     The optional printer switch happens in between, on the emptied plate -- a selectPreset
//     sent BEFORE the open would switch the printer of the user's project instead.
//   - it is a no-op when m_last_loaded_gcode == filename && m_only_gcode, i.e. a second
//     file with the SAME NAME is silently ignored. Clients must send a unique filename
//     per result (a re-slice of the same model included). WEB_APP_BRIDGE.md says so.
void WebAppBridge::Impl::open_resolved_file(const std::string& id, const std::string& method, const fs::path& path,
                                            const std::string& printer)
{
    const std::string utf8 = path_to_utf8(path);
    const std::string ext  = lowercase_extension(path);
    // Also reached from a download callback, outside on_script_message's dispatch: whatever new/open
    // project this causes is the bridge's (slicer.projectChanged source "bridge").
    Plater::BridgeRequestScope bridge_open;

    if (ext == "3mf") {
        // A SLICED 3MF handed back by the page (a ".gcode.3mf" -- a page may package its G-code that
        // way for printers that can only be sent one) opens like a G-code result: the user's
        // project is SET ASIDE first, not closed behind a save-changes prompt. It loads as an
        // exported sliced file (no objects), straight to Preview with Print plate available.
        if (boost::algorithm::iends_with(utf8, ".gcode.3mf")) {
            if (Plater* plater = wxGetApp().plater(); plater != nullptr && !set_project_aside(*plater))
                BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: could not set the project aside before a sliced 3MF";
        }
        else // a project to work on: its slicer.projectChanged says it was this tab's doing
            Plater::note_bridge_project_open(utf8, owner.slot().id);
        // Inherits the "not allowed during slicing" guard and the recent-project plumbing.
        wxGetApp().request_open_project(utf8);
    } else if (ext == "gcode") {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            send_event(id, "slicer.projectOpened", {{"ok", false}, {"error", "no plater"}});
            return;
        }
        if (!set_project_aside(*plater))
            BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: could not set the project aside; opening G-code the stock way";
        json result = {{"ok", true}, {"path", utf8}};
        if (!printer.empty()) {
            PresetBundle* bundle = wxGetApp().preset_bundle;
            Tab*          tab    = wxGetApp().get_tab(Preset::TYPE_PRINTER);
            if (bundle != nullptr && tab != nullptr && bundle->printers.get_selected_preset_name() != printer)
                tab->select_preset(printer);
            if (bundle != nullptr)
                result["printer"] = bundle->printers.get_selected_preset_name();
        }
        // Selects MainFrame::tpPreview on its own; no tab switch here.
        plater->load_gcode(from_path(path));
        result["projectAside"] = has_project_aside(*plater);
        send_event(id, "slicer.projectOpened", result);
        return;
    } else {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            send_event(id, "slicer.projectOpened", {{"ok", false}, {"error", "no plater"}});
            return;
        }
        {
            // A page cannot answer the STEP meshing dialog: mesh with the Preferences values instead.
            NoStepImportDialogs no_step_dialogs;
            plater->load_files(std::vector<std::string>{utf8});
        }
        if (wxGetApp().mainframe != nullptr)
            wxGetApp().mainframe->select_tab(static_cast<wxPanel*>(plater));
    }

    send_event(id, "slicer.projectOpened", {{"ok", true}, {"path", utf8}});
}

// ---------------------------------------------------------------------------------------------
// Selecting an installed preset
//
// A page that produces G-code for a particular printer needs the application to be ON that
// printer, or the preview draws the wrong bed and puts the toolpaths in the wrong place: the bed
// shape comes from the selected printer preset (Plater::set_bed_shape reads its printable_area),
// and loading G-code does not change the selection. So this switches the selection, and only
// that: an ALREADY-INSTALLED preset, named (or by alias), with no create, modify, delete or
// import.
//
// The PRINTER form is protocol v1's method and works without the settings opt-in; with the
// opt-in off it behaves exactly as it always has -- like the user picking the entry from the
// printer dropdown, "transfer or discard your changes?" dialog included. The PRINT and FILAMENT
// forms belong to the settings family: gated, and under the ifDirty contract (1013 by default)
// because a page cannot see the user's unsaved edits before it asks to replace them.
//
// The reply reports the selection actually in force afterwards rather than the one asked for.
void WebAppBridge::Impl::handle_select_preset(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.selectPreset";

    std::string type_text = json_string(params, "type");
    if (type_text.empty())
        type_text = "printer";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(type_text, type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "\"type\" must be \"printer\", \"print\" or \"filament\"");
        return;
    }
    std::string name = json_string(params, "name");
    if (name.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "\"name\" is required");
        return;
    }
    // Without the opt-in a page has no ifDirty: the printer form keeps its v1 dialog.
    const std::string if_dirty = owner.allows_settings() ? json_string(params, "ifDirty") : std::string("prompt");
    // allowHidden (settings family only): a hidden system preset is enabled -- ticked, as the person
    // would in the filament selection or the setup wizard -- and then selected. Without it a
    // hidden filament or print preset is refused (1008, data.hidden) rather than selected into a
    // slot whose dropdown cannot show it.
    const bool allow_hidden = owner.allows_settings() && json_bool(params, "allowHidden", false);

    if (type == Preset::TYPE_PRINTER) {
        if (slicer_is_busy()) {
            send_response(id, METHOD, WAB_BUSY, "the slicer is busy slicing");
            return;
        }
        PresetBundle* bundle = wxGetApp().preset_bundle;
        Tab*          tab    = wxGetApp().get_tab(Preset::TYPE_PRINTER);
        if (bundle == nullptr || tab == nullptr) {
            send_response(id, METHOD, WAB_INTERNAL, "no preset bundle");
            return;
        }
        const std::string current = bundle->printers.get_selected_preset_name();
        if (name != current && !resolve_preset_name(*bundle, type, name) && !(allow_hidden && resolve_hidden_alias(bundle->printers, name))) {
            send_response(id, METHOD, WAB_NOT_FOUND, "no installed printer preset is named \"" + name + "\"");
            return;
        }
        if (name == current) { // already there: answer without touching the selection at all
            send_response(id, METHOD, WAB_OK, "success", {{"type", "printer"}, {"selected", current}, {"changed", false}});
            return;
        }
        json        data;
        std::string message;
        if (const int code = dirty_preflight(Preset::TYPE_PRINTER, name, if_dirty, data, message); code != WAB_OK) {
            send_response(id, METHOD, code, message, data);
            return;
        }
        const bool enabled = allow_hidden && enable_hidden_preset(*bundle, type, name);
        tab->select_preset(name);
        const std::string now = bundle->printers.get_selected_preset_name();
        send_response(id, METHOD, WAB_OK, "success",
                      {{"type", "printer"}, {"selected", now}, {"changed", now != current}, {"enabled", enabled}});
        return;
    }

    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    PresetBundle&     bundle     = *wxGetApp().preset_bundle;
    PresetCollection& collection = *collection_for(bundle, type);
    Plater*           plater     = wxGetApp().plater();
    const char*       type_name  = WebAppSettings::preset_type_name(type);
    if (!resolve_preset_name(bundle, type, name) && !(allow_hidden && resolve_hidden_alias(collection, name))) {
        send_response(id, METHOD, WAB_NOT_FOUND, std::string("no installed ") + type_name + " preset is named \"" + name + "\"");
        return;
    }
    if (const std::string asked = name; type == Preset::TYPE_FILAMENT && !prefer_compatible_filament(collection, name)) {
        send_response(id, METHOD, WAB_BAD_PARAMS,
                      "the filament preset \"" + asked + "\" is not compatible with the selected printer, and no preset for this printer shares its name",
                      {{"reason", "incompatible"}, {"name", asked}});
        return;
    }
    const Preset* target      = collection.find_preset(name, false);
    const bool    hidden      = target != nullptr && !target->is_visible;
    if (hidden && !allow_hidden) {
        send_response(id, METHOD, WAB_NOT_FOUND,
                      std::string("the ") + type_name + " preset \"" + name +
                          "\" is installed but hidden (not enabled in the slicer's preset list); retry with allowHidden:true to enable and select it",
                      {{"hidden", true}, {"name", name}, {"isSystem", target->is_system}});
        return;
    }
    if (hidden && !target->is_system) {
        send_response(id, METHOD, WAB_NOT_FOUND, std::string("the ") + type_name + " preset \"" + name + "\" is hidden and not a system preset");
        return;
    }

    if (type == Preset::TYPE_PRINT) {
        const std::string current = bundle.prints.get_selected_preset_name();
        if (name == current) {
            send_response(id, METHOD, WAB_OK, "success", {{"type", "print"}, {"selected", current}, {"changed", false}});
            return;
        }
        json data;
        if (const int code = dirty_preflight(Preset::TYPE_PRINT, name, if_dirty, data, message); code != WAB_OK) {
            send_response(id, METHOD, code, message, data);
            return;
        }
        const bool enabled = hidden && enable_hidden_preset(bundle, type, name);
        tab->select_preset(name);
        plater->on_config_change(bundle.full_config()); // as Plater::priv::on_select_preset does after the tab
        const std::string now = bundle.prints.get_selected_preset_name();
        send_response(id, METHOD, WAB_OK, "success",
                      {{"type", "print"}, {"selected", now}, {"changed", now != current}, {"enabled", enabled}});
        return;
    }

    // Filament: one slot of the sidebar, through the sequence the sidebar combo runs.
    const int slot = json_int(params, "slot", 0);
    if (slot < 0 || static_cast<size_t>(slot) >= bundle.filament_presets.size()) {
        send_response(id, METHOD, WAB_BAD_PARAMS,
                      "slot out of range (" + std::to_string(bundle.filament_presets.size()) + " slots)");
        return;
    }
    const std::string current = bundle.filament_presets[slot];
    if (name == current) {
        send_response(id, METHOD, WAB_OK, "success", {{"type", "filament"}, {"slot", slot}, {"selected", current}, {"changed", false}});
        return;
    }
    if (!plater->sidebar().is_multifilament()) {
        // A single-filament printer routes the pick through the filament tab, which owns the
        // dirty edits; a multi-filament one only renames the slot.
        json data;
        if (const int code = dirty_preflight(Preset::TYPE_FILAMENT, name, if_dirty, data, message); code != WAB_OK) {
            send_response(id, METHOD, code, message, data);
            return;
        }
    }
    const bool enabled = hidden && enable_hidden_preset(bundle, type, name);
    plater->select_filament_preset(static_cast<size_t>(slot), name);
    const std::string now = bundle.filament_presets[slot];
    if (now != name) { // the sidebar put something else in the slot: never report that as success
        send_response(id, METHOD, WAB_INTERNAL, "the slot now holds \"" + now + "\", not \"" + name + "\"",
                      {{"reason", "replaced"}, {"type", "filament"}, {"slot", slot}, {"requested", name}, {"selected", now}});
        return;
    }
    send_response(id, METHOD, WAB_OK, "success",
                  {{"type", "filament"}, {"slot", slot}, {"selected", now}, {"changed", now != current}, {"enabled", enabled}});
}

bool WebAppBridge::Impl::enable_hidden_preset(PresetBundle& bundle, Preset::Type type, const std::string& name)
{
    AppConfig*        cfg        = wxGetApp().app_config;
    PresetCollection* collection = collection_for(bundle, type);
    if (cfg == nullptr || collection == nullptr)
        return false;
    Preset* preset = collection->find_preset(name, false);
    if (preset == nullptr || preset->is_visible || !preset->is_system)
        return false;

    if (type == Preset::TYPE_FILAMENT) {
        // The filament selection's tick (GuideFrame filament page -> AppConfig "filaments").
        cfg->set(AppConfig::SECTION_FILAMENTS, preset->name, "true");
        preset->set_visible_from_appconfig(*cfg);
    } else if (type == Preset::TYPE_PRINTER) {
        // The setup wizard's model + nozzle tick, then the visibility pass load_selections runs.
        const std::string model   = preset->config.opt_string("printer_model");
        const std::string variant = preset->config.opt_string("printer_variant");
        if (preset->vendor == nullptr || model.empty() || variant.empty())
            return false;
        cfg->set_variant(preset->vendor->id, model, variant, true);
        bundle.load_installed_printers(*cfg);
        load_installed_filaments_for(bundle, *cfg);
    } else {
        preset->is_visible = true; // print presets carry no visibility of their own in the app config
    }
    if (!preset->is_visible)
        return false;

    BOOST_LOG_TRIVIAL(info) << "WebAppBridge: enabled hidden " << Preset::get_type_string(type) << " preset \"" << name << "\"";
    if (Tab* tab = wxGetApp().get_tab(type); tab != nullptr)
        tab->update_tab_ui();
    if (Plater* plater = wxGetApp().plater(); plater != nullptr) {
        plater->sidebar().update_presets(type);
        if (type == Preset::TYPE_PRINTER)
            plater->sidebar().update_presets(Preset::TYPE_FILAMENT); // the model's default filaments
    }
    return true;
}

void WebAppBridge::Impl::handle_open_project(const std::string& id, const json& params)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!owner.origin_is_loopback() && (cfg == nullptr || !cfg->get_bool(owner.slot().key_allow_local))) {
        send_response(id, "slicer.openProject", WAB_CAPABILITY_DISABLED,
                      "opening local files is available to loopback applications only, unless " +
                          std::string(owner.slot().key_allow_local) + " is enabled");
        return;
    }

    if (slicer_is_busy()) {
        send_response(id, "slicer.openProject", WAB_BUSY, "the slicer is busy slicing");
        return;
    }

    fs::path    resolved;
    std::string message;
    const int   code = validate_local_path(json_string(params, "path"), resolved, message);
    if (code != WAB_OK) {
        send_response(id, "slicer.openProject", code, message);
        return;
    }

    // Accepted, not finished: opening may put a modal in front of the user. The outcome arrives
    // as a slicer.projectOpened event carrying the same id.
    send_response(id, "slicer.openProject", WAB_OK, "accepted");
    open_resolved_file(id, "slicer.openProject", resolved);
}

int WebAppBridge::Impl::check_download_url(const std::string& url, std::string& message) const
{
    const wxString wx_url = wxString::FromUTF8(url.c_str());
    const wxURI    uri(wx_url);
    const wxString scheme = uri.HasScheme() ? uri.GetScheme().Lower() : wxString();
    if (scheme != "http" && scheme != "https") {
        message = "only http and https URLs are accepted";
        return WAB_BAD_PARAMS;
    }

    // Same host as the page by default; anything else must be named in web_app_download_hosts.
    const std::string url_host  = host_of(wx_url);
    const std::string page_host = host_of(owner.current_origin());
    if (url_host.empty()) {
        message = "url has no host";
        return WAB_BAD_PARAMS;
    }
    if (url_host != page_host) {
        const std::vector<std::string> extra = configured_download_hosts(owner.slot());
        // An entry beginning with a dot matches the whole family: ".example.com" accepts
        // "a.example.com" and "example.com" itself, but never "notexample.com". Exact hosts stay
        // exact. Sub-domain matching exists because object storage hands out a host that belongs
        // to the deployment rather than to the product, so no fixed name can be shipped.
        const bool allowed = std::any_of(extra.begin(), extra.end(), [&url_host](const std::string& h) {
            if (h.empty())
                return false;
            if (h.front() != '.')
                return h == url_host;
            if (url_host.size() < h.size())
                return url_host == h.substr(1);
            return url_host.compare(url_host.size() - h.size(), h.size(), h) == 0;
        });
        if (!allowed) {
            message = "downloads must come from the application's own host unless that host is listed in " +
                      std::string(owner.slot().key_download_hosts);
            return WAB_FORBIDDEN_ORIGIN;
        }
    }
    return WAB_OK;
}

bool WebAppBridge::Impl::claim_download()
{
    bool expected = false;
    if (!download_busy.compare_exchange_strong(expected, true))
        return false;
    progress_mark.store(0);
    return true;
}

void WebAppBridge::Impl::run_download(DownloadJob job)
{
    // Everything below runs on an Http worker thread. It may only touch Impl through the weak
    // pointer, and must never touch Impl::owner -- that is the GUI thread's alone.
    std::weak_ptr<Impl> weak = weak_from_this();
    const std::string   id   = job.id;
    // Shared by the callbacks, which each get a copy of the closure.
    auto shared = std::make_shared<DownloadJob>(std::move(job));

    Http::get(shared->url)
        .size_limit(shared->size_limit)
        .timeout_connect(10)
        .on_progress([weak, id, shared](Http::Progress progress, bool& cancel) {
            auto self = weak.lock();
            if (!self || self->detached.load()) {
                cancel = true;
                return;
            }
            if (shared->progress_event.empty())
                return;
            // One event per MiB, plus the final one: on_progress fires far too often to forward
            // every tick into the GUI thread's event queue.
            const size_t step = progress.dlnow / WAB_PROGRESS_STEP;
            if (step == self->progress_mark.load() && !(progress.dltotal > 0 && progress.dlnow >= progress.dltotal))
                return;
            self->progress_mark.store(step);
            self->send_event(id, shared->progress_event, {{"received", progress.dlnow}, {"total", progress.dltotal}});
        })
        .on_error([weak, shared](std::string /*body*/, std::string error, unsigned status) {
            auto self = weak.lock();
            if (!self)
                return;
            self->download_busy.store(false);
            const std::string message = error.empty() ? ("HTTP " + std::to_string(status)) : error;
            BOOST_LOG_TRIVIAL(info) << "WebAppBridge: download failed: " << message;
            if (!self->detached.load())
                shared->on_http_error(*self, message, status);
        })
        .on_complete([weak, shared](std::string body, unsigned /*status*/) {
            auto self = weak.lock();
            if (!self)
                return;

            const fs::path&    destination  = shared->destination;
            const std::string& expected_sha = shared->expected_sha;
            std::string        error;
            if (!expected_sha.empty()) {
                const std::vector<uint8_t> bytes(body.begin(), body.end());
                const std::string          actual = sha256_hex(bytes);
                if (!boost::algorithm::iequals(actual, expected_sha))
                    error = "checksum mismatch: expected " + expected_sha + ", got " + actual;
            }

            if (error.empty()) {
                boost::system::error_code ec;
                fs::create_directories(destination.parent_path(), ec);
                fs::ofstream out(destination, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!out.good()) {
                    error = "could not write " + path_to_utf8(destination);
                } else {
                    out.write(body.data(), static_cast<std::streamsize>(body.size()));
                    out.close();
                }
            }

            if (!error.empty()) {
                boost::system::error_code ec;
                fs::remove(destination, ec); // never leave a partial or unverified file behind
                BOOST_LOG_TRIVIAL(info) << "WebAppBridge: " << error;
            }

            wxGetApp().CallAfter([weak, shared, error]() {
                auto self = weak.lock();
                if (!self)
                    return;
                self->download_busy.store(false);
                if (self->detached.load())
                    return;
                if (!error.empty()) {
                    shared->on_save_error(*self, error);
                    return;
                }
                shared->on_ready(*self, shared->destination);
            });
        })
        .perform();
}

void WebAppBridge::Impl::handle_open_project_from_url(const std::string& id, const json& params)
{
    const std::string url = json_string(params, "url");
    if (url.empty()) {
        send_response(id, "slicer.openProjectFromUrl", WAB_BAD_PARAMS, "url is required");
        return;
    }

    std::string url_message;
    if (const int code = check_download_url(url, url_message); code != WAB_OK) {
        send_response(id, "slicer.openProjectFromUrl", code, url_message);
        return;
    }

    std::string name = sanitise_filename(json_string(params, "filename"));
    if (name.empty())
        name = sanitise_filename(Http::get_filename_from_url(url));
    if (name.empty()) {
        send_response(id, "slicer.openProjectFromUrl", WAB_BAD_PARAMS, "filename is required");
        return;
    }
    const std::string ext = lowercase_extension(path_from_utf8(name));
    if (!extension_allowed(ext)) {
        send_response(id, "slicer.openProjectFromUrl", WAB_UNSUPPORTED_TYPE, "file type '" + ext + "' is not accepted");
        return;
    }

    // G-code only: the printer it was sliced for, checked now so a bad name fails before the
    // download. open_resolved_file says why this is not a selectPreset of its own.
    std::string printer = ext == "gcode" ? json_string(params, "printer") : std::string();
    if (!printer.empty()) {
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr ||
            (printer != bundle->printers.get_selected_preset_name() && !resolve_preset_name(*bundle, Preset::TYPE_PRINTER, printer))) {
            send_response(id, "slicer.openProjectFromUrl", WAB_NOT_FOUND, "no installed printer preset is named \"" + printer + "\"");
            return;
        }
    }

    if (slicer_is_busy()) {
        send_response(id, "slicer.openProjectFromUrl", WAB_BUSY, "the slicer is busy slicing");
        return;
    }

    if (!claim_download()) {
        send_response(id, "slicer.openProjectFromUrl", WAB_BUSY, "another download is already in progress");
        return;
    }

    DownloadJob job;
    job.id             = id;
    job.url            = url;
    job.expected_sha   = boost::algorithm::to_lower_copy(json_string(params, "sha256"));
    job.destination    = webapp_cache_dir() / path_from_utf8(name);
    job.size_limit     = WAB_MAX_DOWNLOAD_BYTES;
    job.progress_event = "slicer.openProjectFromUrl.progress";
    job.on_http_error  = [id](Impl& self, const std::string& message, unsigned status) {
        const int code = (status == 404 || status == 410) ? WAB_NOT_FOUND : WAB_IO;
        self.send_event(id, "slicer.projectOpened", {{"ok", false}, {"error", message}, {"code", code}});
    };
    job.on_save_error = [id](Impl& self, const std::string& error) {
        self.send_event(id, "slicer.projectOpened", {{"ok", false}, {"error", error}});
    };
    job.on_ready = [id, printer](Impl& self, const fs::path& destination) {
        self.open_resolved_file(id, "slicer.openProjectFromUrl", destination, printer);
    };

    send_response(id, "slicer.openProjectFromUrl", WAB_OK, "accepted");
    run_download(std::move(job));
}

// ---------------------------------------------------------------------------------------------
// Preset settings (WEB_APP_BRIDGE.md section 4; policy in WebAppSettings.cpp)
//
// Eight methods behind one per-tab opt-in, all on the GUI thread, all through preflight(). The
// write path is Tab::load_config(), the entry the cut-plane gizmo uses: a value becomes an
// ordinary unsaved edit of the tab's preset, nothing touches disk, and Undo works.
// ---------------------------------------------------------------------------------------------

namespace {

bool json_bool(const json& node, const char* key, bool fallback)
{
    auto it = node.find(key);
    return (it != node.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

int json_int(const json& node, const char* key, int fallback)
{
    auto it = node.find(key);
    return (it != node.end() && it->is_number()) ? static_cast<int>(it->get<double>()) : fallback;
}

// Events a page sees only with the settings opt-in: they describe the user's presets and plate.
bool is_settings_event(const std::string& method)
{
    return method == "slicer.presetChanged" || method == "slicer.settingsChanged" || method == "slicer.plateChanged" ||
           method == "slicer.sliceStarted" || method == "slicer.sliceFinished" || method == "slicer.dialogOpened" ||
           method == "slicer.dialogClosed";
}

PresetCollection* collection_for(PresetBundle& bundle, Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT:    return &bundle.prints;
    case Preset::TYPE_FILAMENT: return &bundle.filaments;
    case Preset::TYPE_PRINTER:  return &bundle.printers;
    default:                    return nullptr;
    }
}

// A page may send a string, a number, a bool or an array of those; set_deserialize() wants the
// option's .ini form. Strings pass through untouched, so "35%" and "0.2" both work as typed.
bool value_to_ini_string(const json& value, const ConfigOptionDef& def, std::string& out)
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
    if (value.is_array()) {
        if (def.type == coStrings) {
            std::vector<std::string> items;
            for (const json& item : value) {
                if (!item.is_string())
                    return false;
                items.push_back(item.get<std::string>());
            }
            out = escape_strings_cstyle(items);
            return true;
        }
        std::string joined;
        for (const json& item : value) {
            std::string text;
            if (item.is_array() || item.is_object() || !value_to_ini_string(item, def, text))
                return false;
            if (!joined.empty())
                joined += ',';
            joined += text;
        }
        out = joined;
        return true;
    }
    return false;
}

// ConfigOptionDef::is_value_valid() for the numeric types, scalar and vector. Other types are
// accepted as set_deserialize() left them; a percentage in a float-or-percent is not
// range-checked, as the GUI field does not check it either.
bool numeric_value_valid(const ConfigOptionDef& def, const ConfigOption* opt)
{
    if (opt == nullptr)
        return true;
    switch (def.type) {
    case coFloat:
    case coPercent:
        return def.is_value_valid(opt->getFloat());
    case coFloatOrPercent:
        if (auto* fp = dynamic_cast<const ConfigOptionFloatOrPercent*>(opt); fp != nullptr && fp->percent)
            return true;
        return def.is_value_valid(opt->getFloat());
    case coInt:
        return def.is_value_valid(static_cast<double>(opt->getInt()));
    case coFloats:
    case coPercents:
        if (def.nullable)
            return true;
        if (auto* floats = dynamic_cast<const ConfigOptionFloats*>(opt); floats != nullptr)
            for (double v : floats->values)
                if (!def.is_value_valid(v))
                    return false;
        return true;
    case coInts:
        if (def.nullable)
            return true;
        if (auto* ints = dynamic_cast<const ConfigOptionInts*>(opt); ints != nullptr)
            for (int v : ints->values)
                if (!def.is_value_valid(static_cast<double>(v)))
                    return false;
        return true;
    default:
        return true;
    }
}

std::string range_text(const ConfigOptionDef& def)
{
    std::ostringstream oss;
    oss << "value out of range [";
    if (def.min > -FLT_MAX)
        oss << def.min;
    else
        oss << "-inf";
    oss << ", ";
    if (def.max < FLT_MAX)
        oss << def.max;
    else
        oss << "inf";
    oss << "]";
    return oss.str();
}

// A per-extruder-variant filament option (filament_options_with_variant) holds one value per entry
// of the preset's filament_extruder_variant list, in that order. The project config appends the
// filaments' lists and finds each filament's values again by counting its variants, so one list of
// the wrong length shifts that option for every later filament (one nozzle_temperature value on a
// two-variant Bambu H2S gave every later filament its neighbour's temperature). One value on a
// preset with several variants means "all of them" and is repeated (ConfigOptionVector::resize);
// any other length is refused, as is a change to the length of the variant list itself.
// `original` is the preset before this write, `copy` holds the parsed value of `key`. Returns the
// error text, empty when the value fits; `resized` says the single value was repeated.
std::string fit_filament_variants(const DynamicPrintConfig& original, DynamicPrintConfig& copy, const std::string& key, bool& resized)
{
    resized = false;
    if (filament_options_with_variant.count(key) == 0)
        return {};
    const ConfigOptionStrings* variants = original.option<ConfigOptionStrings>("filament_extruder_variant");
    ConfigOptionVectorBase*    value    = dynamic_cast<ConfigOptionVectorBase*>(copy.option(key));
    if (variants == nullptr || variants->values.empty() || value == nullptr)
        return {};
    const size_t count = variants->values.size();
    const size_t given = value->size();
    if (given == count)
        return {};
    if (key != "filament_extruder_variant" && count > 1 && given == 1) {
        value->resize(count); // repeats the one value
        resized = true;
        return {};
    }
    std::string names;
    for (const std::string& variant : variants->values)
        names += (names.empty() ? "\"" : ", \"") + variant + "\"";
    if (key == "filament_extruder_variant")
        return "the preset has " + std::to_string(count) + " extruder variant(s) (" + names +
               "); the length of this list cannot be changed";
    return "this option holds one value per extruder variant of the preset (" + names + "): give 1 value for all of them or exactly " +
           std::to_string(count) + ", not " + std::to_string(given);
}

const char* TYPE_PARAM_ERROR = "\"type\" must be \"printer\", \"print\" or \"filament\"";

// The brand a person would name: a filament's filament_vendor ("Bambu Lab", "Polymaker"), else the
// vendor bundle the preset ships in ("BBL", "OrcaFilamentLibrary"), else nothing (user, default).
std::string preset_vendor_text(const Preset& preset)
{
    if (preset.type == Preset::TYPE_FILAMENT)
        if (auto* v = preset.config.option<ConfigOptionStrings>("filament_vendor"); v != nullptr && !v->values.empty() &&
                                                                                  !v->values.front().empty())
            return v->values.front();
    return preset.vendor != nullptr ? preset.vendor->name : std::string();
}

std::string preset_filament_type(const Preset& preset)
{
    if (preset.type != Preset::TYPE_FILAMENT)
        return std::string();
    auto* t = preset.config.option<ConfigOptionStrings>("filament_type");
    return t != nullptr && !t->values.empty() ? t->values.front() : std::string();
}

// A hidden preset by its alias: get_preset_name_by_alias looks only at visible ones. Compatible
// ones first, as the dropdown would. False if none.
bool resolve_hidden_alias(PresetCollection& collection, std::string& name)
{
    const Preset* fallback = nullptr;
    for (auto it = collection.begin(); it != collection.end(); ++it) {
        if (it->is_visible || it->alias != name)
            continue;
        if (it->is_compatible) {
            name = it->name;
            return true;
        }
        if (fallback == nullptr)
            fallback = &*it;
    }
    if (fallback == nullptr)
        return false;
    name = fallback->name;
    return true;
}

// The selected printer's own copy of a filament. An exact name can be one the printer cannot use
// (the "@System" copy of a Bambu filament on a Bambu printer), and select_filament_preset would then
// put a compatible default in the slot without saying so. Same alias (or the name before " @"),
// compatible, visible first. False when the named preset is incompatible and nothing replaces it.
bool prefer_compatible_filament(PresetCollection& collection, std::string& name)
{
    const Preset* named = collection.find_preset(name, false);
    if (named == nullptr || named->is_compatible)
        return true;
    const std::string key = !named->alias.empty() ? named->alias : name.substr(0, name.find(" @"));
    const Preset* hidden_match = nullptr;
    for (auto it = collection.begin(); it != collection.end(); ++it) {
        if (!it->is_compatible || (it->alias != key && it->name.substr(0, it->name.find(" @")) != key))
            continue;
        if (it->is_visible) {
            name = it->name;
            return true;
        }
        if (hidden_match == nullptr)
            hidden_match = &*it;
    }
    if (hidden_match == nullptr)
        return false;
    name = hidden_match->name;
    return true;
}

// Letters and digits only, lower case: "Bambu Lab" and "Bambulab" (BBL.json's name) compare equal.
std::string squash(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A printer model the slicer ships in resources/profiles, whether or not its vendor is installed.
struct ShippedModel
{
    std::string              vendor;      // the bundle id: <vendor>.json ("BBL"); the app config's vendor key
    std::string              vendor_name; // that file's "name" ("Bambulab")
    std::string              model;       // "Bambu Lab H2S": printer_model, and the app config's model key
    std::string              family;
    std::vector<std::string> nozzles;     // the variants, as the app config spells them ("0.4")
    bool                     shipped = true; // false: only known from an installed (updated) bundle
};

bool read_json_file(const fs::path& path, json& out)
{
    boost::nowide::ifstream in(path_to_utf8(path).c_str(), std::ios::binary);
    if (!in)
        return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    out = json::parse(buffer.str(), nullptr, false);
    return !out.is_discarded();
}

std::vector<std::string> split_nozzles(const std::string& text)
{
    std::vector<std::string> parts;
    boost::algorithm::split(parts, text, boost::algorithm::is_any_of(";"));
    std::vector<std::string> out;
    for (std::string& part : parts) {
        boost::algorithm::trim(part);
        if (!part.empty())
            out.push_back(part);
    }
    return out;
}

// Every FFF printer model of every vendor bundle in resources/profiles. Read once: the resources
// do not change while the slicer runs. GUI thread only.
const std::vector<ShippedModel>& shipped_printer_models()
{
    static std::vector<ShippedModel> s_models;
    static bool                      s_loaded = false;
    if (s_loaded)
        return s_models;
    s_loaded = true;

    const fs::path root = fs::path(resources_dir()) / "profiles";
    boost::system::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& file = it->path();
        if (file.extension() != ".json" || !fs::is_regular_file(file))
            continue;
        json vendor_json;
        if (!read_json_file(file, vendor_json) || !vendor_json.is_object())
            continue;
        auto list = vendor_json.find("machine_model_list");
        if (list == vendor_json.end() || !list->is_array())
            continue;
        const std::string vendor      = path_to_utf8(file.stem());
        const std::string vendor_name = json_string(vendor_json, "name");
        for (const json& item : *list) {
            const std::string name     = json_string(item, "name");
            const std::string sub_path = json_string(item, "sub_path");
            if (name.empty() || sub_path.empty())
                continue;
            json model_json;
            if (!read_json_file(root / vendor / path_from_utf8(sub_path), model_json) || !model_json.is_object())
                continue;
            const std::string tech = json_string(model_json, "machine_tech");
            if (!tech.empty() && tech != "FFF")
                continue;
            ShippedModel m;
            m.vendor      = vendor;
            m.vendor_name = vendor_name.empty() ? vendor : vendor_name;
            m.model       = name;
            m.family      = json_string(model_json, "family");
            m.nozzles     = split_nozzles(json_string(model_json, "nozzle_diameter"));
            s_models.push_back(std::move(m));
        }
    }
    std::sort(s_models.begin(), s_models.end(), [](const ShippedModel& a, const ShippedModel& b) {
        return a.vendor != b.vendor ? a.vendor < b.vendor : a.model < b.model;
    });
    BOOST_LOG_TRIVIAL(info) << "WebAppBridge: " << s_models.size() << " printer models ship in " << path_to_utf8(root);
    return s_models;
}

// The shipped models plus any an installed bundle knows that the resources do not (a vendor
// updated online): what the setup wizard would offer.
std::vector<ShippedModel> known_printer_models(const PresetBundle& bundle)
{
    std::vector<ShippedModel> models = shipped_printer_models();
    std::set<std::pair<std::string, std::string>> seen;
    for (const ShippedModel& m : models)
        seen.emplace(m.vendor, m.model);
    for (const auto& [vendor_id, profile] : bundle.vendors) {
        for (const VendorProfile::PrinterModel& pm : profile.models) {
            if (pm.technology != ptFFF || seen.count({vendor_id, pm.id}) != 0)
                continue;
            ShippedModel m;
            m.vendor      = vendor_id;
            m.vendor_name = profile.name;
            m.model       = pm.id;
            m.family      = pm.family;
            for (const auto& v : pm.variants)
                m.nozzles.push_back(v.name);
            m.shipped = false;
            models.push_back(std::move(m));
        }
    }
    return models;
}

// "0.4" when the model has it (the standard nozzle), else its first.
std::string default_nozzle(const ShippedModel& m)
{
    for (const std::string& n : m.nozzles)
        if (n == "0.4")
            return n;
    return m.nozzles.empty() ? std::string() : m.nozzles.front();
}

// The nozzle a page named ("0.6", 0.6, "0.60 mm") as the model spells it. Empty if it has none such.
std::string match_nozzle(const ShippedModel& m, const json& value)
{
    double wanted = 0.;
    try {
        if (value.is_number())
            wanted = value.get<double>();
        else if (value.is_string()) {
            std::string text = value.get<std::string>();
            boost::algorithm::ireplace_all(text, "mm", "");
            boost::algorithm::trim(text);
            wanted = std::stod(text);
        } else
            return std::string();
    } catch (const std::exception&) {
        return std::string();
    }
    for (const std::string& n : m.nozzles) {
        try {
            if (std::abs(std::stod(n) - wanted) < 1e-6)
                return n;
        } catch (const std::exception&) {
        }
    }
    return std::string();
}

// Does every word of `query` occur in the model's vendor, vendor name, model name or family?
bool model_matches(const ShippedModel& m, const std::vector<std::string>& words)
{
    const std::string hay = squash(m.vendor + m.vendor_name + m.model + m.family);
    return std::all_of(words.begin(), words.end(), [&](const std::string& w) { return hay.find(w) != std::string::npos; });
}

// PresetBundle::load_installed_filaments (private) for printers just enabled: when none of the
// person's enabled filaments suits a newly visible printer, its model's default materials are
// ticked, then every filament's visibility is re-read from the app config.
void load_installed_filaments_for(PresetBundle& bundle, AppConfig& cfg)
{
    for (const Preset& printer : bundle.printers) {
        if (!printer.is_visible || printer.vendor == nullptr || printer.printer_technology() != ptFFF || printer.vendor->models.empty())
            continue;
        bool has_compatible = false;
        if (cfg.has_section(AppConfig::SECTION_FILAMENTS))
            for (const auto& entry : cfg.get_section(AppConfig::SECTION_FILAMENTS)) {
                const Preset* filament = bundle.filaments.find_preset(entry.first, false, true);
                if (filament != nullptr && is_compatible_with_printer(PresetWithVendorProfile(*filament, filament->vendor),
                                                                      PresetWithVendorProfile(printer, printer.vendor))) {
                    has_compatible = true;
                    break;
                }
            }
        if (has_compatible)
            continue;
        if (const VendorProfile::PrinterModel* model = PresetUtils::system_printer_model(printer); model != nullptr)
            for (const std::string& name : model->default_materials)
                if (const Preset* filament = bundle.filaments.find_preset(name, false, true); filament != nullptr && filament->is_system)
                    cfg.set(AppConfig::SECTION_FILAMENTS, filament->name, "true");
    }
    for (Preset& filament : bundle.filaments)
        filament.set_visible_from_appconfig(cfg);
}

std::vector<std::string> query_words(const std::string& query)
{
    std::vector<std::string> parts, words;
    boost::algorithm::split(parts, query, boost::algorithm::is_any_of(" ,;/"), boost::algorithm::token_compress_on);
    for (const std::string& p : parts)
        if (std::string w = squash(p); !w.empty())
            words.push_back(w);
    return words;
}

} // namespace

void WebAppBridge::Impl::notify(const std::string& method, const json& data)
{
    if (session_token.empty())
        return; // no trusted document loaded: nobody to tell
    if (is_settings_event(method) && !owner.allows_settings())
        return;
    send_event("", method, data);
    if (action_host) // Actions: settings writes of a turn (undoTurn); capabilitiesChanged -> actionsChanged
        on_bridge_event(*action_host, method, data);
}

int WebAppBridge::Impl::preflight(bool write, Preset::Type type, Tab*& tab, std::string& message,
                                  bool require_settings) const
{
    tab = nullptr;
    if (require_settings && !owner.allows_settings()) {
        message = "print settings access is not enabled for this tab";
        return WAB_CAPABILITY_DISABLED;
    }
    // A modal dialog disables the frame; changing presets under one would nest a second modal
    // inside it. A heuristic against a bad experience, not a security check.
    MainFrame* mainframe = wxGetApp().mainframe;
    if (mainframe == nullptr || !mainframe->IsEnabled()) {
        message = WebAppDialogs::busy_message();
        return WAB_BUSY;
    }
    if (write && slicer_is_busy()) {
        message = "the slicer is busy slicing";
        return WAB_BUSY;
    }
    if (wxGetApp().preset_bundle == nullptr || wxGetApp().plater() == nullptr) {
        message = "no preset bundle";
        return WAB_INTERNAL;
    }
    if (type != Preset::TYPE_INVALID) {
        tab = wxGetApp().get_tab(type);
        if (tab == nullptr) {
            message = "no settings tab for this preset type";
            return WAB_INTERNAL;
        }
    }
    return WAB_OK;
}

bool WebAppBridge::Impl::refuse_model_edit(const std::string& id, const char* method)
{
    int  code = WAB_OK;
    json data;
    if (const std::string why = Actions::gui_guard(action_host.get(), /*write=*/true, code, data); !why.empty()) {
        data["reason"] = why;
        send_response(id, method, code, why, data);
        return true;
    }
    return false;
}

int WebAppBridge::Impl::dirty_preflight(Preset::Type type, const std::string& target, const std::string& if_dirty, json& data,
                                        std::string& message) const
{
    return Actions::preset_switch_preflight(type, target, if_dirty, data, message);
}

bool WebAppBridge::Impl::resolve_preset_name(PresetBundle& bundle, Preset::Type type, std::string& name) const
{
    PresetCollection* collection = collection_for(bundle, type);
    if (collection == nullptr)
        return false;
    name = Preset::remove_suffix_modified(name);
    if (collection->find_preset(name) != nullptr)
        return true;
    const std::string by_alias = bundle.get_preset_name_by_alias(type, name);
    if (by_alias != name && collection->find_preset(by_alias) != nullptr) {
        name = by_alias;
        return true;
    }
    return false;
}

void WebAppBridge::Impl::handle_list_presets(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.listPresets";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(json_string(params, "type"), type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, TYPE_PARAM_ERROR);
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    PresetBundle&     bundle          = *wxGetApp().preset_bundle;
    PresetCollection& collection      = *collection_for(bundle, type);
    const std::string query           = json_string(params, "query");
    const bool        compatible_only = json_bool(params, "compatibleOnly", true);
    // Hidden = a system preset the person has not ticked in the filament / printer selection: it
    // is installed (its vendor bundle is loaded) but not in the dropdowns. A page searches them
    // to find a vendor's own profile for a product, then selects it with allowHidden.
    const bool        include_hidden  = json_bool(params, "includeHidden", false);
    const size_t      limit           = static_cast<size_t>(std::clamp(json_int(params, "limit", 500), 1, 5000));

    json   presets      = json::array();
    size_t total        = 0;
    size_t hidden_total = 0;
    for (auto it = collection.begin(); it != collection.end(); ++it) { // begin() skips "- default -"
        const Preset& preset = *it;
        if (!preset.is_visible && !include_hidden)
            continue;
        if (compatible_only && !preset.is_compatible)
            continue;
        const std::string vendor        = preset_vendor_text(preset);
        const std::string filament_type = preset_filament_type(preset);
        if (!query.empty() && !boost::algorithm::icontains(preset.name, query) && !boost::algorithm::icontains(preset.alias, query) &&
            !boost::algorithm::icontains(vendor, query) && !boost::algorithm::icontains(filament_type, query))
            continue;
        ++total;
        if (!preset.is_visible)
            ++hidden_total;
        if (presets.size() < limit) {
            json entry = {{"name", preset.name},
                          {"alias", preset.alias},
                          {"isSystem", preset.is_system},
                          {"isUser", preset.is_user()},
                          {"isCompatible", preset.is_compatible},
                          {"isVisible", preset.is_visible},
                          {"vendor", vendor},
                          {"inherits", preset.inherits()}};
            entry["isProjectEmbedded"] = preset.is_project_embedded;
            entry["isFromBundle"]      = preset.is_from_bundle();
            if (preset.is_from_bundle())
                entry["bundleId"] = preset.bundle_id;
            if (type == Preset::TYPE_FILAMENT) {
                entry["filamentType"] = filament_type;
                entry["filamentId"]   = preset.filament_id; // shared by one filament's presets for each printer
            }
            if (type != Preset::TYPE_PRINTER)
                if (auto* cp = preset.config.option<ConfigOptionStrings>("compatible_printers"); cp != nullptr)
                    entry["compatiblePrinters"] = cp->values; // empty: every printer (subject to the condition)
            presets.push_back(std::move(entry));
        }
    }

    json data = {{"type", WebAppSettings::preset_type_name(type)},
                 {"selected", collection.get_selected_preset_name()},
                 {"edited", {{"name", collection.get_edited_preset().name}, {"dirty", collection.current_is_dirty()}}},
                 {"total", total},
                 {"presets", presets}};
    if (include_hidden)
        data["hiddenTotal"] = hidden_total;
    if (type == Preset::TYPE_FILAMENT) {
        data["slots"] = bundle.filament_presets;
        if (auto* colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour"); colours != nullptr)
            data["slotColours"] = colours->values;
    }
    send_response(id, METHOD, WAB_OK, "success", data);
}

// ---------------------------------------------------------------------------------------------
// Vendor catalog: filament slots, saved filament presets, shipped printers
// (WEB_APP_BRIDGE.md 4.1, "Vendor catalog"). Settings family: gated, 1007 under a dialog or
// while slicing (the writers), and a preset switch never drops the person's unsaved edits
// without ifDirty saying so.
//
// The one deliberate exception to WebAppBridge.hpp's "no AppConfig write":
// the preset-VISIBILITY sections -- AppConfig::SECTION_FILAMENTS (an existing system filament,
// by name) and the vendor model/nozzle ticks (a model the slicer ships) -- written through the
// same calls the filament selection and the setup wizard make. No other key is reachable, and
// nothing is read back to the page.
// ---------------------------------------------------------------------------------------------

// The sidebar's "+" next to the filament list (Sidebar::add_filament): a new physical slot with
// the next colour, then -- when `preset` names one -- that preset selected into it the way the
// slot's dropdown would.
void WebAppBridge::Impl::handle_add_filament_slot(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.addFilamentSlot";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_FILAMENT, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    Plater*       plater = wxGetApp().plater();
    // Sidebar::add_custom_filament would show a modal and start a new project here.
    if (plater->using_exported_file()) {
        send_response(id, METHOD, WAB_BUSY, "a sliced file is open for preview; open or start a project first");
        return;
    }
    // The "+" exists only where the filament count is free: one extruder (or a BBL machine)
    // feeding several filaments. On a multi-extruder printer with fixed materials the count is
    // the extruder count, a printer setting.
    if (!Sidebar::should_show_SEMM_buttons()) {
        send_response(id, METHOD, WAB_BAD_PARAMS,
                      "this printer has one filament per extruder: the number of filament slots is its extruder count "
                      "(Printer settings), so a slot cannot be added",
                      {{"reason", "fixedExtruders"}, {"slots", bundle.num_physical_filaments()}});
        return;
    }
    const size_t physical = bundle.num_physical_filaments();
    if (physical >= MAXIMUM_EXTRUDER_NUMBER || bundle.filament_presets.size() >= MAXIMUM_EXTRUDER_NUMBER) {
        send_response(id, METHOD, WAB_BAD_PARAMS,
                      "the slicer allows at most " + std::to_string(MAXIMUM_EXTRUDER_NUMBER) + " filaments",
                      {{"reason", "maximum"}, {"maximum", MAXIMUM_EXTRUDER_NUMBER}});
        return;
    }

    // Resolve the preset BEFORE adding anything, so a bad name changes nothing.
    std::string preset_name = json_string(params, "preset");
    const bool  allow_hidden = json_bool(params, "allowHidden", false);
    bool        hidden       = false;
    if (!preset_name.empty()) {
        if (!resolve_preset_name(bundle, Preset::TYPE_FILAMENT, preset_name) &&
            !(allow_hidden && resolve_hidden_alias(bundle.filaments, preset_name))) {
            send_response(id, METHOD, WAB_NOT_FOUND, "no installed filament preset is named \"" + preset_name + "\"");
            return;
        }
        if (const std::string asked = preset_name; !prefer_compatible_filament(bundle.filaments, preset_name)) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "the filament preset \"" + asked + "\" is not compatible with the selected printer, and no preset for this printer shares its name",
                          {{"reason", "incompatible"}, {"name", asked}});
            return;
        }
        const Preset* target = bundle.filaments.find_preset(preset_name, false);
        hidden               = target != nullptr && !target->is_visible;
        if (hidden && (!allow_hidden || !target->is_system)) {
            send_response(id, METHOD, WAB_NOT_FOUND,
                          "the filament preset \"" + preset_name +
                              "\" is installed but hidden; retry with allowHidden:true to enable it",
                          {{"hidden", true}, {"name", preset_name}});
            return;
        }
    }

    plater->sidebar().add_filament();
    if (bundle.num_physical_filaments() != physical + 1) {
        send_response(id, METHOD, WAB_INTERNAL, "the slicer did not add a filament slot");
        return;
    }
    const size_t slot = physical; // add_custom_filament inserts after the last physical slot
    bool         enabled = false;
    if (!preset_name.empty()) {
        enabled = hidden && enable_hidden_preset(bundle, Preset::TYPE_FILAMENT, preset_name);
        plater->select_filament_preset(slot, preset_name);
        if (bundle.filament_presets[slot] != preset_name) { // added, but the sidebar put something else in it
            send_response(id, METHOD, WAB_INTERNAL,
                          "a slot was added but holds \"" + bundle.filament_presets[slot] + "\", not \"" + preset_name + "\"",
                          {{"reason", "replaced"}, {"slot", slot}, {"requested", preset_name}, {"selected", bundle.filament_presets[slot]}});
            return;
        }
    }
    json colour;
    if (auto* colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour"); colours != nullptr && slot < colours->values.size())
        colour = colours->values[slot];
    send_response(id, METHOD, WAB_OK, "success",
                  {{"slot", slot},
                   {"selected", bundle.filament_presets[slot]},
                   {"colour", colour},
                   {"enabled", enabled},
                   {"slots", bundle.filament_presets}});
}

// The Filament tab's Save button with the name typed in: the slot's preset, with its unsaved
// edits, written as a user preset (Tab::save_preset -> PresetCollection::save_current_preset ->
// update_compatible), which then takes the slot's place. The one bridge call that writes a file:
// a user preset in the person's own preset folder, exactly what Save writes. A new preset made
// from a system one is limited to the selected printer, as Save does.
void WebAppBridge::Impl::handle_save_filament_preset(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.saveFilamentPreset";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_FILAMENT, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    PresetBundle&     bundle     = *wxGetApp().preset_bundle;
    PresetCollection& collection = *tab->get_presets();
    Plater*           plater     = wxGetApp().plater();

    // The name, held to SavePresetDialog::Item::update()'s rules -- the one validator preset.save
    // uses too (Actions/PresetLogic.hpp).
    const std::string name = json_string(params, "name");
    if (const std::string why = Actions::PresetLogic::check_preset_name(name, PresetCollection::get_suffix_modified()); !why.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, why, {{"path", "/name"}});
        return;
    }
    const bool    overwrite = json_bool(params, "overwrite", false);
    const Preset* existing  = collection.find_preset(name, false);
    if (existing != nullptr && !existing->can_overwrite()) {
        send_response(id, METHOD, WAB_FORBIDDEN_ORIGIN, "\"" + name + "\" is a system preset; choose another name",
                      {{"exists", true}, {"isSystem", true}});
        return;
    }
    if (existing != nullptr && !overwrite) {
        send_response(id, METHOD, WAB_BAD_PARAMS,
                      "a filament preset named \"" + name + "\" already exists; pass overwrite:true to replace it, or choose another name",
                      {{"exists", true}, {"name", name}});
        return;
    }
    if (existing == nullptr && collection.get_preset_name_by_alias(name) != name) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "the name is the display name (alias) of another preset; choose another name");
        return;
    }

    // The slot whose filament is saved; the Filament tab must be on it, which is a preset switch
    // under ifDirty when it is on another slot's preset (as setSettings does).
    int slot = -1;
    if (params.contains("slot")) {
        slot = json_int(params, "slot", -1);
        if (slot < 0 || static_cast<size_t>(slot) >= bundle.filament_presets.size()) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "slot out of range (" + std::to_string(bundle.filament_presets.size()) + " slots)");
            return;
        }
        const std::string slot_name = bundle.filament_presets[slot];
        if (slot_name != collection.get_edited_preset().name) {
            if (collection.find_preset(slot_name) == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "the preset of slot " + std::to_string(slot) + " is not installed");
                return;
            }
            json data;
            if (const int code = dirty_preflight(Preset::TYPE_FILAMENT, slot_name, json_string(params, "ifDirty"), data, message); code != WAB_OK) {
                send_response(id, METHOD, code, message, data);
                return;
            }
            tab->select_preset(slot_name);
            if (collection.get_edited_preset().name != slot_name) {
                send_response(id, METHOD, WAB_USER_CANCELLED, "the user kept the current preset",
                              {{"selected", collection.get_edited_preset().name}});
                return;
            }
        }
    }

    // Never write a per-variant filament list of the wrong length into a user preset: joined into
    // the project config it shifts that option for every later filament. setSettings already
    // refuses or expands such a value; this catches one that reached the edited preset another way.
    // Short lists repeat their first value, long ones are cut; the Tab is updated the way
    // setSettings updates it, so the fix is part of the saved edits.
    std::vector<std::string> variant_resized;
    {
        DynamicPrintConfig fixed = *tab->get_config();
        variant_resized          = normalize_filament_variant_lengths(fixed, collection.get_edited_preset().name);
        if (!variant_resized.empty()) {
            WebAppBridgeWriteScope write_scope;
            tab->load_config(fixed);
        }
    }

    const std::string              from       = collection.get_edited_preset().name;
    // Replacing ANOTHER saved preset loses what it held: the person says so, natively -- as
    // preset.save {type:"filament"} asks.
    if (existing != nullptr && existing->name != from) {
        Actions::ConfirmRequest req;
        req.title        = "Overwrite preset";
        req.message      = "Replace the saved filament preset \"" + name + "\" with the settings of \"" + from + "\"?";
        req.accept_label = "Overwrite";
        if (!Actions::native_confirm(req)) {
            send_response(id, METHOD, WAB_USER_CANCELLED, "the overwrite was declined", {{"name", name}});
            return;
        }
    }
    const bool                     was_dirty  = collection.current_is_dirty();
    const std::vector<std::string> json_dirty = collection.current_dirty_options();
    const std::vector<std::string> slots_before = bundle.filament_presets;

    // No dialog: the name is given. The only prompt left is the tab's own first-layer temperature
    // safety check, which stays the person's to answer.
    tab->save_preset(name, false /*detach*/, false /*save_to_project*/);

    const Preset* saved = collection.find_preset(name, false);
    if (saved == nullptr || collection.get_edited_preset().name != name) {
        send_response(id, METHOD, WAB_USER_CANCELLED, "the preset was not saved (the temperature safety check was declined)",
                      {{"selected", collection.get_edited_preset().name}});
        return;
    }
    // Save renames EVERY slot that held the old preset (Sidebar::update_presets_from_to). Only the
    // slot asked for changes: the others get their preset back.
    if (slot >= 0) {
        for (size_t i = 0; i < slots_before.size() && i < bundle.filament_presets.size(); ++i)
            if (static_cast<int>(i) != slot && slots_before[i] == from && bundle.filament_presets[i] == name)
                plater->select_filament_preset(i, from);
        if (bundle.filament_presets[slot] != name)
            plater->select_filament_preset(static_cast<size_t>(slot), name);
    }

    json data = {{"name", name},
                 {"from", from},
                 {"overwritten", existing != nullptr},
                 {"savedEdits", was_dirty ? json(json_dirty) : json::array()},
                 {"slots", bundle.filament_presets}};
    if (slot >= 0)
        data["slot"] = slot;
    if (!variant_resized.empty())
        data["variantResized"] = variant_resized;
    if (auto* printers = saved->config.option<ConfigOptionStrings>("compatible_printers"); printers != nullptr)
        data["compatiblePrinters"] = printers->values;
    send_response(id, METHOD, WAB_OK, "success", data);
}

// Every FFF printer model the slicer ships (resources/profiles/<vendor>.json), installed or not:
// what the setup wizard's printer page lists. Read-only.
void WebAppBridge::Impl::handle_list_printer_models(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.listPrinterModels";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    PresetBundle&                  bundle                = *wxGetApp().preset_bundle;
    const AppConfig*               cfg                   = wxGetApp().app_config;
    const std::vector<std::string> words                 = query_words(json_string(params, "query"));
    const bool                     include_not_installed = json_bool(params, "includeNotInstalled", true);
    const size_t                   limit = static_cast<size_t>(std::clamp(json_int(params, "limit", 50), 1, 500));

    const Preset&     current       = bundle.printers.get_selected_preset();
    const std::string current_model = current.config.opt_string("printer_model");
    const std::string squashed      = squash(json_string(params, "query"));

    std::vector<ShippedModel> matches;
    for (ShippedModel& m : known_printer_models(bundle)) {
        if (!words.empty() && !model_matches(m, words))
            continue;
        if (!include_not_installed && !is_vendor_installed(m.vendor))
            continue;
        matches.push_back(std::move(m));
    }
    // Exact model name first ("Bambu Lab H2S" before "Bambu Lab H2S Pro"), then the shortest name.
    std::stable_sort(matches.begin(), matches.end(), [&](const ShippedModel& a, const ShippedModel& b) {
        const bool ea = squash(a.model) == squashed || squash(a.vendor_name + a.model) == squashed;
        const bool eb = squash(b.model) == squashed || squash(b.vendor_name + b.model) == squashed;
        if (ea != eb)
            return ea;
        return a.model.size() < b.model.size();
    });

    json models = json::array();
    for (const ShippedModel& m : matches) {
        if (models.size() >= limit)
            break;
        const bool installed = is_vendor_installed(m.vendor);
        json       enabled   = json::array();
        json       presets   = json::object();
        for (const std::string& n : m.nozzles) {
            if (cfg != nullptr && cfg->get_variant(m.vendor, m.model, n))
                enabled.push_back(n);
            if (installed)
                if (const Preset* p = bundle.printers.find_system_preset_by_model_and_variant(m.model, n); p != nullptr)
                    presets[n] = p->name;
        }
        models.push_back({{"vendor", m.vendor},
                          {"vendorName", m.vendor_name},
                          {"model", m.model},
                          {"family", m.family},
                          {"nozzles", m.nozzles},
                          {"defaultNozzle", default_nozzle(m)},
                          {"installed", installed},
                          {"enabled", !enabled.empty()},
                          {"enabledNozzles", enabled},
                          {"presets", presets},
                          {"selected", !current_model.empty() && current_model == m.model},
                          {"shipped", m.shipped}});
    }
    send_response(id, METHOD, WAB_OK, "success",
                  {{"total", matches.size()},
                   {"models", models},
                   {"current", {{"printer", current.name},
                                {"model", current_model},
                                {"nozzle", current.config.opt_string("printer_variant")}}}});
}

// The setup wizard's printer pick, for one model + nozzle: the vendor's profiles installed from
// the resources when they are not yet (PresetBundle::apply_vendor_config, which GuideFrame
// calls), the model's nozzle ticked in the app config's vendor section, presets made visible,
// and the printer selected. A vendor that must be installed reloads every preset, so the
// person's unsaved edits in ANY tab are under ifDirty first; one already installed only becomes
// visible and is selected like a pick from the printer dropdown.
void WebAppBridge::Impl::handle_add_printer(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.addPrinter";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_PRINTER, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    AppConfig*    cfg    = wxGetApp().app_config;
    if (cfg == nullptr) {
        send_response(id, METHOD, WAB_INTERNAL, "no app config");
        return;
    }
    const std::string vendor_param = json_string(params, "vendor");
    const std::string model_param  = json_string(params, "model");
    if (model_param.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "\"model\" is required (a model name from slicer.listPrinterModels)");
        return;
    }
    const bool        select   = json_bool(params, "select", true);
    const std::string if_dirty = json_string(params, "ifDirty");

    // The model: exact name (letters and digits compared), within the vendor when one is given;
    // else the one model every word of `model` matches. Two or more = say which.
    const std::vector<ShippedModel> known = known_printer_models(bundle);
    auto vendor_ok = [&](const ShippedModel& m) {
        if (vendor_param.empty())
            return true;
        const std::string v = squash(vendor_param);
        return v == squash(m.vendor) || v == squash(m.vendor_name);
    };
    std::vector<const ShippedModel*> found;
    for (const ShippedModel& m : known)
        if (vendor_ok(m) && (squash(m.model) == squash(model_param) || squash(m.vendor_name + m.model) == squash(model_param)))
            found.push_back(&m);
    if (found.empty()) {
        const std::vector<std::string> words = query_words(model_param);
        for (const ShippedModel& m : known)
            if (vendor_ok(m) && model_matches(m, words))
                found.push_back(&m);
    }
    if (found.size() != 1) {
        json candidates = json::array();
        for (const ShippedModel* m : found) {
            if (candidates.size() >= 10)
                break;
            candidates.push_back({{"vendor", m->vendor}, {"model", m->model}, {"nozzles", m->nozzles}});
        }
        send_response(id, METHOD, found.empty() ? WAB_NOT_FOUND : WAB_BAD_PARAMS,
                      found.empty() ? "no printer model the slicer ships matches \"" + model_param + "\""
                                    : "\"" + model_param + "\" matches " + std::to_string(found.size()) +
                                          " models; name one exactly (vendor + model from slicer.listPrinterModels)",
                      {{"candidates", candidates}});
        return;
    }
    const ShippedModel& m = *found.front();

    std::string nozzle;
    if (auto it = params.find("nozzle"); it != params.end() && !it->is_null()) {
        nozzle = match_nozzle(m, *it);
        if (nozzle.empty()) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "the " + m.model + " has no such nozzle", {{"nozzles", m.nozzles}});
            return;
        }
    } else
        nozzle = default_nozzle(m);
    if (nozzle.empty()) {
        send_response(id, METHOD, WAB_INTERNAL, "the " + m.model + " lists no nozzle");
        return;
    }

    const std::string before            = bundle.printers.get_selected_preset_name();
    const bool        vendor_installed  = is_vendor_installed(m.vendor) && bundle.vendors.count(m.vendor) != 0;
    const bool        was_enabled       = cfg->get_variant(m.vendor, m.model, nozzle);
    const Preset*     preset            = vendor_installed ? bundle.printers.find_system_preset_by_model_and_variant(m.model, nozzle) : nullptr;
    bool              installed_now     = false;

    if (preset != nullptr) {
        // Installed vendor: tick the nozzle and make the presets visible (load_selections' own
        // visibility pass), then select it like a dropdown pick -- the printer tab's ifDirty.
        if (select && preset->name != before) {
            json data;
            if (const int code = dirty_preflight(Preset::TYPE_PRINTER, preset->name, if_dirty, data, message); code != WAB_OK) {
                send_response(id, METHOD, code, message, data);
                return;
            }
        }
        if (!was_enabled || !preset->is_visible) {
            cfg->set_variant(m.vendor, m.model, nozzle, true);
            bundle.load_installed_printers(*cfg);
            load_installed_filaments_for(bundle, *cfg);
            tab->update_tab_ui();
            wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_PRINTER);
            wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);
        }
    } else {
        // Vendor not installed: the wizard's whole path. It reloads every preset, so any unsaved
        // edit in any tab is the person's call first.
        json dirty_keys = json::object();
        std::vector<Tab*> dirty_tabs;
        for (Preset::Type t : {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER})
            if (Tab* tt = wxGetApp().get_tab(t); tt != nullptr && tt->get_presets() != nullptr && tt->get_presets()->current_is_dirty()) {
                dirty_tabs.push_back(tt);
                dirty_keys[WebAppSettings::preset_type_name(t)] = tt->get_presets()->current_dirty_options();
            }
        bool keep_edits = false;
        if (!dirty_tabs.empty()) {
            if (if_dirty.empty() || if_dirty == "ask") {
                send_response(id, METHOD, WAB_NEEDS_CONFIRMATION,
                              "adding this printer installs the " + m.vendor_name +
                                  " profiles and reloads every preset, and there are unsaved edits; retry with ifDirty:\"discard\" "
                                  "to drop them, \"save\" to keep them in your own presets, or ifDirty:\"prompt\" to let the user "
                                  "keep or save them",
                              {{"dirty", true}, {"dirtyKeys", dirty_keys}});
                return;
            }
            if (if_dirty == "discard" || if_dirty == "save" || if_dirty == "transfer") {
                json data;
                if (const int code = Actions::presets_replace_preflight({Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER},
                                                                        if_dirty, data, message);
                    code != WAB_OK) {
                    send_response(id, METHOD, code, message, data);
                    return;
                }
            } else if (if_dirty == "prompt") {
                if (!wxGetApp().check_and_keep_current_preset_changes(_L("Add printer"),
                                                                      _L("Adding a printer reloads all presets."),
                                                                      ActionButtons::KEEP | ActionButtons::SAVE, &keep_edits)) {
                    send_response(id, METHOD, WAB_USER_CANCELLED, "the user cancelled");
                    return;
                }
            } else {
                send_response(id, METHOD, WAB_BAD_PARAMS, "ifDirty must be \"ask\", \"discard\", \"prompt\", \"save\" or \"transfer\"");
                return;
            }
        }
        bundle.export_selections(*cfg); // as GuideFrame::run does before applying
        const std::map<std::string, std::map<std::string, std::set<std::string>>> vendors = {{m.vendor, {{m.model, {nozzle}}}}};
        const std::map<std::string, std::string>                                   no_filaments;
        bool        ok = false;
        std::string error;
        try {
            // overwrite = false: merged into what the person already has enabled.
            ok = bundle.apply_vendor_config(vendors, no_filaments, cfg, false, select ? m.model : std::string(),
                                            select ? nozzle : std::string());
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (keep_edits)
            wxGetApp().apply_keeped_preset_modifications();
        wxGetApp().load_current_presets(); // as GUI_App::run_wizard does after the wizard
        if (!ok) {
            send_response(id, METHOD, WAB_IO,
                          "could not install the " + m.vendor_name + " profiles" + (error.empty() ? std::string() : ": " + error));
            return;
        }
        installed_now = true;
        preset        = bundle.printers.find_system_preset_by_model_and_variant(m.model, nozzle);
        if (preset == nullptr) {
            send_response(id, METHOD, WAB_INTERNAL, "the profiles were installed but no " + m.model + " " + nozzle + " nozzle preset loaded");
            return;
        }
    }

    const std::string name = preset->name;
    if (select && bundle.printers.get_selected_preset_name() != name)
        tab->select_preset(name);
    const std::string now = bundle.printers.get_selected_preset_name();
    if (select && now != name) {
        send_response(id, METHOD, WAB_USER_CANCELLED, "the printer was added but the user kept the current one",
                      {{"preset", name}, {"selected", now}});
        return;
    }
    send_response(id, METHOD, WAB_OK, "success",
                  {{"type", "printer"},
                   {"vendor", m.vendor},
                   {"model", m.model},
                   {"nozzle", nozzle},
                   {"preset", name},
                   {"installedVendor", installed_now},
                   {"enabled", true},
                   {"wasEnabled", was_enabled},
                   {"selected", now},
                   {"changed", now != before}});
}

void WebAppBridge::Impl::handle_get_settings_schema(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.getSettingsSchema";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(json_string(params, "type"), type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, TYPE_PARAM_ERROR);
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    const bool                include_code = json_bool(params, "includeCode", false);
    const size_t              limit        = static_cast<size_t>(std::clamp(json_int(params, "limit", 200), 1, 5000));
    const std::string         category     = json_string(params, "category");
    const std::string         query        = json_string(params, "query");
    const DynamicPrintConfig* cfg          = tab->get_config();
    // The query is words, any of which may match ("support brim skirt"): a single substring matched
    // nothing for a multi-word query, and a page concluded the settings did not exist.
    std::vector<std::string> terms;
    boost::algorithm::split(terms, query, boost::algorithm::is_any_of(" ,;"), boost::algorithm::token_compress_on);
    terms.erase(std::remove_if(terms.begin(), terms.end(), [](const std::string& t) { return t.empty(); }), terms.end());

    std::vector<std::string> keys;
    if (auto it = params.find("keys"); it != params.end() && it->is_array()) {
        for (const json& item : *it) {
            if (!item.is_string()) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "\"keys\" must be an array of option names");
                return;
            }
            const std::string key = item.get<std::string>();
            if (print_config_def.get(key) == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "unknown option \"" + key + "\"");
                return;
            }
            keys.push_back(key);
        }
    } else {
        keys = cfg->keys(); // what this tab's preset actually holds
    }

    json   options = json::array();
    size_t total   = 0;
    for (const std::string& key : keys) {
        if (!WebAppSettings::key_readable(key, include_code))
            continue;
        const ConfigOptionDef* def = print_config_def.get(key);
        if (!category.empty() && !boost::algorithm::iequals(def->category, category))
            continue;
        if (!terms.empty() && std::none_of(terms.begin(), terms.end(), [&](const std::string& t) {
                return boost::algorithm::icontains(key, t) || boost::algorithm::icontains(def->label, t) ||
                       boost::algorithm::icontains(def->full_label, t) || boost::algorithm::icontains(def->tooltip, t);
            }))
            continue;
        ++total;
        if (options.size() < limit) {
            json o = WebAppSettings::option_schema_json(key, cfg);
            if (const int stride = WebAppSettings::variant_stride(type, key); stride > 0) {
                o["perVariant"] = true; // one value per extruder variant: get/set with extruder
                if (stride > 1)
                    o["valuesPerVariant"] = stride;
            }
            options.push_back(std::move(o));
        }
    }
    json reply = {{"type", WebAppSettings::preset_type_name(type)},
                  {"total", total},
                  {"truncated", total > options.size()},
                  {"options", options}};
    if (WebAppSettings::printer_extruder_count() > 0 && cfg->has(WebAppSettings::variant_keys_for(type).variant_name))
        reply["variants"] = WebAppSettings::variants_json(*tab, *cfg);
    send_response(id, METHOD, WAB_OK, "success", reply);
}

void WebAppBridge::Impl::handle_get_settings(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.getSettings";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(json_string(params, "type"), type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, TYPE_PARAM_ERROR);
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    const bool                include_code = json_bool(params, "includeCode", false);
    const char*               type_name    = WebAppSettings::preset_type_name(type);
    PresetBundle&             bundle       = *wxGetApp().preset_bundle;
    PresetCollection&         collection   = *tab->get_presets();
    const DynamicPrintConfig* cfg          = tab->get_config();
    std::string               source       = collection.get_edited_preset().name;
    int                       slot         = -1;

    if (type == Preset::TYPE_FILAMENT && params.contains("slot")) {
        slot = json_int(params, "slot", -1);
        if (slot < 0 || static_cast<size_t>(slot) >= bundle.filament_presets.size()) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "slot out of range (" + std::to_string(bundle.filament_presets.size()) + " slots)");
            return;
        }
        const std::string& slot_name = bundle.filament_presets[slot];
        if (slot_name != source) {
            // Another slot's preset: its saved values. Only the tab's own preset carries edits.
            const Preset* preset = collection.find_preset(slot_name);
            if (preset == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "the preset of slot " + std::to_string(slot) + " is not installed");
                return;
            }
            cfg    = &preset->config;
            source = slot_name;
        }
    }

    // extruder: per-variant settings read for that extruder's variant only (the tab's extruder switch).
    int variant = -1;
    if (params.contains("extruder")) {
        const int extruder = json_int(params, "extruder", -1);
        variant            = WebAppSettings::variant_index_for(*tab, *cfg, extruder);
        if (variant < 0) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "no " + std::string(type_name) + " variant for extruder " + std::to_string(extruder) + " (the printer has " +
                              std::to_string(WebAppSettings::printer_extruder_count()) + " extruders)",
                          {{"path", "/extruder"}});
            return;
        }
    }
    auto value_of = [&](const std::string& key) -> json {
        if (variant >= 0)
            if (const int stride = WebAppSettings::variant_stride(type, key); stride > 0)
                return WebAppSettings::variant_value_text(cfg->option(key), variant, stride);
        return cfg->opt_serialize(key);
    };
    json values = json::object();
    if (auto it = params.find("keys"); it != params.end() && it->is_array()) {
        for (const json& item : *it) {
            if (!item.is_string()) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "\"keys\" must be an array of option names");
                return;
            }
            const std::string key = item.get<std::string>();
            if (print_config_def.get(key) == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "unknown option \"" + key + "\"");
                return;
            }
            if (!WebAppSettings::key_readable(key, include_code)) {
                send_response(id, METHOD, WAB_FORBIDDEN_ORIGIN, "\"" + key + "\" is not readable over the bridge");
                return;
            }
            if (cfg->option(key) == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "\"" + key + "\" is not a " + type_name + " setting");
                return;
            }
            values[key] = value_of(key);
        }
    } else {
        for (const std::string& key : cfg->keys())
            if (WebAppSettings::key_readable(key, include_code))
                values[key] = value_of(key);
    }

    json preset = {{"selected", collection.get_selected_preset_name()},
                   {"edited", collection.get_edited_preset().name},
                   {"dirty", collection.current_is_dirty()},
                   {"dirtyKeys", collection.current_dirty_options()},
                   {"source", source}};
    if (slot >= 0)
        preset["slot"] = slot;
    json reply = {{"type", type_name}, {"preset", preset}, {"values", values}};
    if (variant >= 0)
        reply["variant"] = {{"extruder", json_int(params, "extruder", 0)}, {"index", variant}};
    send_response(id, METHOD, WAB_OK, "success", reply);
}

void WebAppBridge::Impl::handle_set_settings(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.setSettings";
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(json_string(params, "type"), type)) {
        send_response(id, METHOD, WAB_BAD_PARAMS, TYPE_PARAM_ERROR);
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, type, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    const auto values_it = params.find("values");
    if (values_it == params.end() || !values_it->is_object() || values_it->empty() || values_it->size() > 64) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "\"values\" must be an object with 1 to 64 entries");
        return;
    }

    const char*       type_name  = WebAppSettings::preset_type_name(type);
    PresetBundle&     bundle     = *wxGetApp().preset_bundle;
    PresetCollection& collection = *tab->get_presets();

    // A filament slot names a preset; to edit that preset the filament tab must be on it. Getting
    // there is a preset switch, so the user's dirty edits get the same ifDirty protection as
    // slicer.selectPreset.
    if (type == Preset::TYPE_FILAMENT && params.contains("slot")) {
        const int slot = json_int(params, "slot", -1);
        if (slot < 0 || static_cast<size_t>(slot) >= bundle.filament_presets.size()) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "slot out of range (" + std::to_string(bundle.filament_presets.size()) + " slots)");
            return;
        }
        const std::string slot_name = bundle.filament_presets[slot];
        if (slot_name != collection.get_edited_preset().name) {
            if (collection.find_preset(slot_name) == nullptr) {
                send_response(id, METHOD, WAB_NOT_FOUND, "the preset of slot " + std::to_string(slot) + " is not installed");
                return;
            }
            json data;
            if (const int code = dirty_preflight(Preset::TYPE_FILAMENT, slot_name, json_string(params, "ifDirty"), data, message); code != WAB_OK) {
                send_response(id, METHOD, code, message, data);
                return;
            }
            tab->select_preset(slot_name);
            if (collection.get_edited_preset().name != slot_name) {
                send_response(id, METHOD, WAB_USER_CANCELLED, "the user kept the current preset",
                              {{"selected", collection.get_edited_preset().name}});
                return;
            }
        }
    }

    const DynamicPrintConfig& original = *tab->get_config();
    DynamicPrintConfig        copy     = original;
    // extruder: per-variant settings written at that extruder's variant only (Tab::switch_excluder's
    // index); the other variants keep their values. Other keys are written whole.
    int variant = -1;
    if (params.contains("extruder")) {
        const int extruder = json_int(params, "extruder", -1);
        variant            = WebAppSettings::variant_index_for(*tab, original, extruder);
        if (variant < 0) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "no " + std::string(type_name) + " variant for extruder " + std::to_string(extruder) + " (the printer has " +
                              std::to_string(WebAppSettings::printer_extruder_count()) + " extruders)",
                          {{"path", "/extruder"}});
            return;
        }
    }
    std::vector<std::string>  applied;
    // Each applied key's value before this write: `original` is the Tab's live config, so it holds
    // the NEW values once load_config() has run.
    std::map<std::string, std::string> before_text;
    json                      rejected = json::array();
    auto reject = [&rejected](const std::string& key, int code, const std::string& why) {
        rejected.push_back({{"key", key}, {"code", code}, {"message", why}});
    };
    // Per-variant filament keys whose single value was repeated for every extruder variant.
    std::vector<std::string> variant_resized;

    for (auto it = values_it->begin(); it != values_it->end(); ++it) {
        const std::string&     key = it.key();
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def == nullptr) {
            reject(key, WAB_NOT_FOUND, "unknown option");
            continue;
        }
        std::string why;
        if (!WebAppSettings::key_writable(key, why)) {
            reject(key, WAB_FORBIDDEN_ORIGIN, why);
            continue;
        }
        const ConfigOption* before = original.option(key);
        if (before == nullptr) {
            reject(key, WAB_BAD_PARAMS, std::string("not a ") + type_name + " setting");
            continue;
        }
        std::string text;
        if (!value_to_ini_string(it.value(), *def, text)) {
            reject(key, WAB_BAD_PARAMS, "value must be a string, number, bool or an array of those");
            continue;
        }
        std::string error;
        try {
            ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
            if (const int stride = variant >= 0 ? WebAppSettings::variant_stride(type, key) : 0; stride > 0) {
                // One variant's value(s), into a copy of the whole vector.
                DynamicPrintConfig one;
                one.set_deserialize(key, text, context);
                auto*       dst = dynamic_cast<ConfigOptionVectorBase*>(copy.option(key));
                const auto* src = dynamic_cast<const ConfigOptionVectorBase*>(one.option(key));
                if (dst == nullptr || src == nullptr || src->size() < 1 || int(src->size()) > stride ||
                    dst->size() < size_t((variant + 1) * stride))
                    throw std::runtime_error("give " + std::string(stride > 1 ? "one or two values" : "one value") +
                                             " for this extruder's variant");
                for (int j = 0; j < int(src->size()); ++j)
                    dst->set_at(src, size_t(variant * stride + j), size_t(j));
            } else {
                copy.set_deserialize(key, text, context); // runs handle_legacy(); throws on a value it cannot parse
            }
            if (!numeric_value_valid(*def, copy.option(key)))
                error = range_text(*def);
            else if (type == Preset::TYPE_FILAMENT) {
                // One value per extruder variant, or the project config shifts later filaments.
                bool resized = false;
                error        = fit_filament_variants(original, copy, key, resized);
                if (resized) {
                    variant_resized.push_back(key);
                    BOOST_LOG_TRIVIAL(info) << METHOD << ": " << key << ": one value repeated for every extruder variant -> "
                                            << copy.option(key)->serialize();
                }
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!error.empty()) {
            copy.set_key_value(key, before->clone()); // undo whatever a failed parse left behind
            reject(key, WAB_BAD_PARAMS, error);
            continue;
        }
        applied.push_back(key);
        before_text[key] = before->serialize();
    }

    if (applied.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "no value was applied", {{"applied", json::array()}, {"rejected", rejected}});
        return;
    }

    {
        // Tab::on_value_change must not echo these back as user edits; the event below is the one
        // report of this write.
        WebAppBridgeWriteScope write_scope;
        tab->load_config(copy); // update_dirty + reload_config + update: the sidebar path

        // The couplings Tab::on_value_change keeps for the GUI's field handlers; load_config does
        // not run them.
        WebAppSettings::apply_tab_couplings(*tab, copy, applied);
    }

    json result = {{"applied", applied},
                   {"rejected", rejected},
                   {"preset", {{"type", type_name}, {"selected", collection.get_selected_preset_name()}, {"dirty", collection.current_is_dirty()}}}};
    if (!variant_resized.empty())
        result["variantResized"] = variant_resized;
    send_response(id, METHOD, WAB_OK, "success", result);
    // The same `changes` a hand edit reports (Tab::describe_setting_change), with the exact value
    // from before this write as `previous`, so a page can show what it changed, and the shared cache keeps the next hand edit's "previous" right.
    json changes = json::array();
    for (const std::string& key : applied) {
        std::string previous = before_text[key];
        if (previous.size() > 2000)
            previous.resize(2000);
        changes.push_back(tab->describe_setting_change(key, previous));
    }
    notify("slicer.settingsChanged", {{"type", type_name},
                                      {"keys", applied},
                                      {"source", "bridge"},
                                      {"preset", collection.get_selected_preset_name()},
                                      {"changes", changes}});
}

static bool read_vec3(const json& obj, const char* key, Vec3d& out)
{
    if (!obj.contains(key) || !obj[key].is_array() || obj[key].size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!obj[key][i].is_number())
            return false;
        out[i] = obj[key][i].get<double>();
        if (!std::isfinite(out[i]))
            return false;
    }
    return true;
}

// A page authoring multiplanar cut planes: "a cut plane parallel to the roof". The planes
// decide how the object is sliced exactly as a print setting does, so this sits in the settings
// family and behind its gate; it takes an undo snapshot like every gizmo edit, so Ctrl+Z takes it
// back; and it does the gizmo's own aftermath (stale slice, object-list badge, redraw). The
// object is never physically split -- the stack is declarative (Model.hpp).
// Which object a plate edit targets: `object` as an index or an exact name, implied when the plate
// holds one. The object must have an instance on the plate.
static int resolve_object(const json& params, const Model& model, int& obj_idx, std::string& message)
{
    obj_idx = -1;
    if (params.contains("object") && !params["object"].is_null()) {
        const json& o = params["object"];
        if (o.is_number_integer()) {
            obj_idx = o.get<int>();
        } else if (o.is_string()) {
            const std::string name = o.get<std::string>();
            for (size_t i = 0; i < model.objects.size() && obj_idx < 0; ++i)
                if (model.objects[i]->name == name)
                    obj_idx = int(i);
            if (obj_idx < 0) {
                message = "no object named '" + name + "'";
                return WAB_NOT_FOUND;
            }
        } else {
            message = "object must be an index or a name";
            return WAB_BAD_PARAMS;
        }
    } else if (model.objects.size() == 1) {
        obj_idx = 0;
    } else {
        message = "object is required when the plate holds more than one object";
        return WAB_BAD_PARAMS;
    }
    if (obj_idx < 0 || obj_idx >= int(model.objects.size())) {
        message = "no object with that index";
        return WAB_NOT_FOUND;
    }
    if (model.objects[obj_idx]->instances.empty()) {
        message = "that object has no instance on the plate";
        return WAB_NOT_FOUND;
    }
    return WAB_OK;
}

void WebAppBridge::Impl::handle_set_cut_planes(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.setCutPlanes";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (refuse_model_edit(id, METHOD))
        return;

    Plater* plater = wxGetApp().plater();
    Model&  model  = plater->model();
    if (model.objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }

    int obj_idx = -1;
    if (const int code = resolve_object(params, model, obj_idx, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    ModelObject* mo = model.objects[obj_idx];

    const std::string frame        = json_string(params, "frame");
    const bool        object_frame = frame == "object";
    if (!frame.empty() && !object_frame && frame != "world") {
        send_response(id, METHOD, WAB_BAD_PARAMS, "frame must be \"world\" or \"object\"");
        return;
    }
    const std::string mode   = json_string(params, "mode");
    const bool        append = mode == "append";
    if (!mode.empty() && !append && mode != "replace") {
        send_response(id, METHOD, WAB_BAD_PARAMS, "mode must be \"replace\" or \"append\"");
        return;
    }

    if (params.contains("userOrdered") && !params["userOrdered"].is_null() && !params["userOrdered"].is_boolean()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "userOrdered must be a boolean");
        return;
    }

    namespace CPR = Actions::CutPlaneRules;
    constexpr size_t      max_planes = 64;
    std::vector<CutPlane> planes;
    std::vector<bool>     explicit_build; // the plane sets (or resets) its own build direction
    json                  clamped = json::array();
    // The authoring-time tilt limit of the Cut Planes gizmo (0 = none: not a virtual 5-axis
    // printer). A plane's chunk is not measured here, so this is the gizmo's limit for an
    // unmeasured chunk -- the hotend term, never stricter than the gizmo; Print::validate stays
    // the backstop for the chunk's own reach.
    auto plane_limit = [](const Vec3d& n_world) { return virtual_max_tilt_deg(0., CPR::tilt_azimuth(n_world)); };
    if (params.contains("planes") && !params["planes"].is_null()) {
        const json& arr = params["planes"];
        if (!arr.is_array()) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "planes must be an array");
            return;
        }
        // World -> object-local exactly as GLGizmoCutPlanes::add_plane stores a world pose.
        const Transform3d m     = mo->instances.front()->get_matrix();
        const Transform3d m_inv = m.inverse();
        for (size_t i = 0; i < arr.size(); ++i) {
            const json& p = arr[i];
            const std::string at = "planes[" + std::to_string(i) + "]";
            Vec3d point, normal;
            if (!p.is_object() || !read_vec3(p, "point", point) || !read_vec3(p, "normal", normal)) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "each plane needs point [x,y,z] and normal [x,y,z] (finite numbers)",
                              {{"path", "/" + at}});
                return;
            }
            if (normal.norm() < 1e-9) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "a plane normal must not be zero", {{"path", "/" + at + "/normal"}});
                return;
            }
            // Everything below works on world directions, as the gizmo's limit and B/C do.
            Vec3d n_world = object_frame ? CPR::world_normal(m, normal) : Vec3d(normal.normalized());
            if (const double limit = plane_limit(n_world); limit > 0.) {
                const Vec3d c = CPR::clamp_to_tilt(n_world, limit);
                if (!c.isApprox(n_world, 1e-9)) {
                    clamped.push_back({{"plane", i}, {"what", "normal"}, {"tiltDeg", CPR::tilt_deg(n_world)}, {"limitDeg", limit}});
                    n_world = c;
                }
            }
            CutPlane cp;
            cp.point  = object_frame ? point : Vec3d(m_inv * point);
            cp.normal = CPR::local_normal(m, n_world);
            if (p.contains("enabled") && !p["enabled"].is_null()) {
                if (!p["enabled"].is_boolean()) {
                    send_response(id, METHOD, WAB_BAD_PARAMS, "enabled must be a boolean", {{"path", "/" + at + "/enabled"}});
                    return;
                }
                cp.enabled = p["enabled"].get<bool>();
            }

            // The build direction of the plane's far side: buildNormal [x,y,z] (in `frame`),
            // buildB/buildC machine angles in degrees (world, as the gizmo's B/C fields), or
            // buildNormal: null to reset it to the plane normal. Absent: a replace keeps the one
            // an unchanged plane had (CutPlaneRules::carry_build_normals).
            const bool has_bn    = p.contains("buildNormal");
            const bool has_bc    = p.contains("buildB") || p.contains("buildC");
            if (has_bn && has_bc) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "give buildNormal or buildB/buildC, not both", {{"path", "/" + at}});
                return;
            }
            explicit_build.push_back(has_bn || has_bc);
            std::optional<Vec3d> build_world;
            if (has_bn && !p["buildNormal"].is_null()) {
                Vec3d bn;
                if (!read_vec3(p, "buildNormal", bn) || bn.norm() < 1e-9) {
                    send_response(id, METHOD, WAB_BAD_PARAMS, "buildNormal must be a non-zero [x, y, z], or null to reset it",
                                  {{"path", "/" + at + "/buildNormal"}});
                    return;
                }
                build_world = object_frame ? CPR::world_normal(m, bn) : Vec3d(bn.normalized());
            } else if (has_bc) {
                const json& jb = p.contains("buildB") ? p["buildB"] : json(0.);
                const json& jc = p.contains("buildC") ? p["buildC"] : json(0.);
                if (!jb.is_number() || !jc.is_number() || jb.get<double>() < -180. || jb.get<double>() > 0. ||
                    jc.get<double>() < -180. || jc.get<double>() > 180.) {
                    send_response(id, METHOD, WAB_BAD_PARAMS, "buildB must be -180..0 and buildC -180..180 degrees",
                                  {{"path", "/" + at + (p.contains("buildB") ? "/buildB" : "/buildC")}});
                    return;
                }
                build_world = CPR::normal_from_bc(jb.get<double>(), jc.get<double>());
            }
            if (build_world) {
                if (const double limit = plane_limit(*build_world); limit > 0.) {
                    const Vec3d c = CPR::clamp_to_tilt(*build_world, limit);
                    if (!c.isApprox(*build_world, 1e-9)) {
                        clamped.push_back({{"plane", i}, {"what", "buildNormal"}, {"tiltDeg", CPR::tilt_deg(*build_world)}, {"limitDeg", limit}});
                        build_world = c;
                    }
                }
                cp.build_normal     = CPR::local_normal(m, *build_world); // world -> object-local, as commit_bc
                cp.has_build_normal = true;
            }
            planes.push_back(cp);
        }
    }
    const size_t resulting = (append ? mo->cut_planes.planes.size() : 0) + planes.size();
    if (resulting > max_planes) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "an object may carry at most 64 cut planes", {{"path", "/planes"}});
        return;
    }
    int kept_build = 0, dropped_build = 0;
    if (!append)
        kept_build = CPR::carry_build_normals(mo->cut_planes.planes, planes, explicit_build, &dropped_build);

    plater->take_snapshot(std::string("Set cut planes (automated)")); // undoable, like the gizmo's edits
    if (!append && planes.empty())
        mo->cut_planes = CutPlaneStack(); // clearing is the full reset the "Cut planes" item's Delete does (enabled off too)
    else if (!append)
        mo->cut_planes.planes.clear();
    mo->cut_planes.planes.insert(mo->cut_planes.planes.end(), planes.begin(), planes.end());
    if (params.contains("enabled") && params["enabled"].is_boolean())
        mo->cut_planes.enabled = params["enabled"].get<bool>();
    else if (!mo->cut_planes.planes.empty())
        mo->cut_planes.enabled = true; // authoring planes implies the feature is wanted on, as in the gizmo
    if (params.contains("userOrdered") && params["userOrdered"].is_boolean())
        mo->cut_planes.user_ordered = params["userOrdered"].get<bool>(); // the gizmo's "Automatic order" checkbox, inverted

    // The gizmo's aftermath: the current slice is stale and the Slice button re-arms; the
    // object list's cut-planes badge appears or goes; the scene redraws.
    plater->schedule_background_process();
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
        list->update_info_items(size_t(obj_idx));
    if (GLCanvas3D* canvas = plater->get_view3D_canvas3D(); canvas != nullptr) {
        canvas->set_as_dirty();
        canvas->request_extra_frame();
    }

    const bool by_group = wxGetApp().global_print_sequence() == PrintSequence::ByGroup;
    json       data     = {{"object", obj_idx},
                           {"name", mo->name},
                           {"planeCount", mo->cut_planes.planes.size()},
                           {"printSequenceByGroup", by_group},
                           {"cutPlanes", Actions::cut_planes_json(*mo)}};
    if (mo->cut_planes.active() && !by_group)
        data["hint"] = "cut-plane printing needs the print setting print_sequence = \"by group\"; set it with slicer.setSettings";
    if (!clamped.empty())
        data["tiltClamped"] = clamped; // held at this printer's tilt limit, as the gizmo holds a drag
    if (kept_build > 0)
        data["buildNormalsKept"] = kept_build;
    if (dropped_build > 0) {
        data["buildNormalsDropped"] = dropped_build;
        data["note"] = "planes whose build direction had been set were moved or removed; their override went with them";
    }
    send_response(id, METHOD, WAB_OK, "success", data);
}

static void refile_object_instances(int obj_idx); // below, with the other plate edits

void WebAppBridge::Impl::handle_set_orientation(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.setOrientation";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (refuse_model_edit(id, METHOD))
        return;

    Plater* plater = wxGetApp().plater();
    Model&  model  = plater->model();
    if (model.objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    int obj_idx = -1;
    if (const int code = resolve_object(params, model, obj_idx, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    ModelObject* mo = model.objects[obj_idx];

    // The world-frame rotation to apply on top of the current pose.
    const bool has_face = params.contains("face") && !params["face"].is_null();
    const bool has_rot  = params.contains("rotation") && !params["rotation"].is_null();
    if (has_face == has_rot) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "give exactly one of face or rotation");
        return;
    }
    Matrix3d rot = Matrix3d::Identity();
    if (has_face) {
        // The rotation that turns that side's outward direction to -Z, i.e. puts it on the bed.
        const std::string face = params["face"].is_string() ? params["face"].get<std::string>() : std::string();
        if (face == "+x")
            rot = Eigen::AngleAxisd(0.5 * M_PI, Vec3d::UnitY()).toRotationMatrix();
        else if (face == "-x")
            rot = Eigen::AngleAxisd(-0.5 * M_PI, Vec3d::UnitY()).toRotationMatrix();
        else if (face == "+y")
            rot = Eigen::AngleAxisd(-0.5 * M_PI, Vec3d::UnitX()).toRotationMatrix();
        else if (face == "-y")
            rot = Eigen::AngleAxisd(0.5 * M_PI, Vec3d::UnitX()).toRotationMatrix();
        else if (face == "+z")
            rot = Eigen::AngleAxisd(M_PI, Vec3d::UnitX()).toRotationMatrix();
        else if (face != "-z") {
            send_response(id, METHOD, WAB_BAD_PARAMS, "face must be one of +x, -x, +y, -y, +z, -z");
            return;
        }
    } else {
        Vec3d deg;
        if (!read_vec3(params, "rotation", deg)) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "rotation must be [x, y, z] degrees (finite numbers)");
            return;
        }
        // About the plate's X, then Y, then Z.
        rot = (Eigen::AngleAxisd(Geometry::deg2rad(deg.z()), Vec3d::UnitZ()) *
               Eigen::AngleAxisd(Geometry::deg2rad(deg.y()), Vec3d::UnitY()) *
               Eigen::AngleAxisd(Geometry::deg2rad(deg.x()), Vec3d::UnitX()))
                  .toRotationMatrix();
    }

    plater->take_snapshot(std::string("Set orientation (automated)")); // undoable, like a rotate in the gizmo
    // Each instance turns about its own bounding-box centre, so it stays where it sits on the plate;
    // then one that stood on the bed goes back onto it (Actions/BedDrop.hpp: changed_object alone
    // leaves a part that turned partly below the bed sunk in it), and changed_object redraws and
    // re-arms slicing.
    const std::vector<double> min_z_before = Actions::instance_min_zs(*mo);
    for (size_t i = 0; i < mo->instances.size(); ++i) {
        const Vec3d centre = mo->instance_bounding_box(i).center();
        Transform3d about  = Transform3d::Identity();
        about.translate(centre);
        about.rotate(rot);
        about.translate(-centre);
        mo->instances[i]->set_transformation(Geometry::Transformation(about * mo->instances[i]->get_matrix()));
    }
    Actions::drop_to_bed_unless_sunk(*mo, min_z_before);
    plater->changed_object(obj_idx);
    refile_object_instances(obj_idx);

    const Vec3d size  = mo->instance_bounding_box(0).size();
    const Vec3d euler = mo->instances.front()->get_rotation();
    send_response(id, METHOD, WAB_OK, "success",
                  {{"object", obj_idx},
                   {"name", mo->name},
                   {"size", {size.x(), size.y(), size.z()}},
                   {"rotation", {Geometry::rad2deg(euler.x()), Geometry::rad2deg(euler.y()), Geometry::rad2deg(euler.z())}}});
}

void WebAppBridge::Impl::handle_set_object_filament(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.setObjectFilament";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (refuse_model_edit(id, METHOD))
        return;

    Plater* plater = wxGetApp().plater();
    Model&  model  = plater->model();
    if (model.objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    int obj_idx = -1;
    if (const int code = resolve_object(params, model, obj_idx, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    // The slot as the sidebar numbers it (1-based), like the object list's filament column.
    const int    filament = json_int(params, "filament", 0);
    const size_t slots    = plater->get_extruder_colors_from_plater_config().size();
    if (filament < 1 || size_t(filament) > slots) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "filament must be a slot number from 1 to " + std::to_string(slots));
        return;
    }

    // What ObjectList::set_extruder_for_selected_items does for an object row: the object takes
    // the slot and its parts drop their own, so the whole object prints (and shows) in that colour.
    ModelObject* mo = model.objects[obj_idx];
    plater->take_snapshot(std::string("Change Filaments (automated)"));
    if (mo->config.has("extruder"))
        mo->config.set("extruder", filament);
    else
        mo->config.set_key_value("extruder", new ConfigOptionInt(filament));
    for (ModelVolume* mv : mo->volumes)
        if (mv->type() == ModelVolumeType::MODEL_PART && mv->config.has("extruder"))
            mv->config.erase("extruder");

    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr) {
        if (const wxDataViewItem item = list->GetModel()->GetItemById(obj_idx); item.IsOk())
            list->GetModel()->SetExtruder(wxString::Format("%d", filament), item);
        list->Refresh();
    }
    plater->update(); // recolours the object and re-arms slicing

    send_response(id, METHOD, WAB_OK, "success",
                  {{"object", obj_idx}, {"name", mo->name}, {"filament", filament},
                   {"preset", filament <= int(wxGetApp().preset_bundle->filament_presets.size())
                                  ? wxGetApp().preset_bundle->filament_presets[filament - 1] : std::string()}});
}

// ── Native plate edits ────────────────────────────────────────────────────────────────────────

static json bbox_json(const BoundingBoxf3& bb)
{
    return {{"min", {bb.min.x(), bb.min.y(), bb.min.z()}}, {"max", {bb.max.x(), bb.max.y(), bb.max.z()}}};
}

static bool read_vec2(const json& obj, const char* key, Vec2d& out)
{
    if (!obj.contains(key) || !obj[key].is_array() || obj[key].size() != 2)
        return false;
    for (int i = 0; i < 2; ++i) {
        if (!obj[key][i].is_number())
            return false;
        out[i] = obj[key][i].get<double>();
        if (!std::isfinite(out[i]))
            return false;
    }
    return true;
}

// The plate whose bed holds the centre of `bb` (XY), else -1: off every bed.
static int plate_holding(const BoundingBoxf3& bb)
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    const Vec3d    c      = bb.center();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        const BoundingBoxf3 vol = plates.get_plate(i)->get_build_volume();
        if (c.x() >= vol.min.x() && c.x() <= vol.max.x() && c.y() >= vol.min.y() && c.y() <= vol.max.y())
            return i;
    }
    return -1;
}

// Wholly inside a plate's print volume (bed area and printable height). Geometric, so it is right
// at once -- the canvas's own outside-the-plate state updates on its next redraw.
static bool inside_a_plate(const BoundingBoxf3& bb)
{
    const int plate = plate_holding(bb);
    if (plate < 0)
        return false;
    const BoundingBoxf3 vol = wxGetApp().plater()->get_partplate_list().get_plate(plate)->get_build_volume();
    return vol.contains(bb.min) && vol.contains(bb.max);
}

// After a direct model edit: the plate list re-files every instance of the object (which plate it is
// on, whether it hangs over the edge) and the object list's plate column follows -- what
// GLCanvas3D::do_move does after a gizmo move.
static void refile_object_instances(int obj_idx)
{
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    for (size_t i = 0; i < plater->model().objects[obj_idx]->instances.size(); ++i)
        plates.notify_instance_update(obj_idx, int(i));
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
        list->update_plate_values_for_items();
}

// Make the object THE selection, as a click on it would: the Plater operations below (fit to
// volume, +/- instance, fill bed) act on the selection.
static void select_only_object(int obj_idx)
{
    wxGetApp().plater()->canvas3D()->get_selection().add_object(unsigned(obj_idx), true);
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
        list->update_selections();
}

static json object_state_json(int obj_idx)
{
    const ModelObject* mo = wxGetApp().plater()->model().objects[obj_idx];
    bool               in = true;
    for (size_t i = 0; i < mo->instances.size(); ++i)
        in = in && inside_a_plate(mo->instance_bounding_box(i));
    return {{"object", obj_idx}, {"name", mo->name}, {"instances", mo->instances.size()}, {"insidePlate", in}};
}

void WebAppBridge::Impl::when_plate_job_done(std::function<void(bool timed_out)> done, int timeout_ms)
{
    std::weak_ptr<Impl> weak    = weak_from_this();
    auto*               timer   = new wxTimer();
    const auto          started = std::chrono::steady_clock::now();
    timer->Bind(wxEVT_TIMER, [weak, timer, done, started, timeout_ms](wxTimerEvent&) {
        Plater*    plater = wxGetApp().plater();
        const bool idle   = plater == nullptr || plater->get_ui_job_worker().is_idle();
        const bool late   = std::chrono::steady_clock::now() - started > std::chrono::milliseconds(timeout_ms);
        if (!idle && !late)
            return;
        timer->Stop();
        // One more turn of the event loop, so the job's finalize() (it runs on this thread, after
        // the worker reports idle) has re-filed the instances before the reply reads them.
        wxGetApp().CallAfter([weak, timer, done, idle]() {
            delete timer;
            if (auto self = weak.lock())
                done(!idle);
        });
    });
    timer->Start(150);
}

void WebAppBridge::Impl::handle_set_scale(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.setScale";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (refuse_model_edit(id, METHOD))
        return;
    Plater* plater = wxGetApp().plater();
    Model&  model  = plater->model();
    if (model.objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    if (plate_job_pending || !plater->get_ui_job_worker().is_idle()) {
        send_response(id, METHOD, WAB_BUSY, "an arrange or fill-bed job is running");
        return;
    }
    int obj_idx = -1;
    if (const int code = resolve_object(params, model, obj_idx, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    ModelObject* mo = model.objects[obj_idx];

    const bool has_factor = params.contains("factor") && !params["factor"].is_null();
    const bool has_size   = params.contains("sizeMm") && !params["sizeMm"].is_null();
    const bool has_fit    = json_bool(params, "fit", false);
    if (int(has_factor) + int(has_size) + int(has_fit) != 1) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "give exactly one of factor, sizeMm or fit");
        return;
    }

    if (has_fit) {
        // The sidebar's "Scale to fit": largest uniform scale inside the print volume. It takes
        // its own "Scale To Fit" snapshot and drops the part onto the bed.
        select_only_object(obj_idx);
        plater->scale_selection_to_fit_print_volume();
    } else {
        double factor = 0.;
        if (has_factor) {
            if (!params["factor"].is_number()) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "factor must be a number > 0");
                return;
            }
            factor = params["factor"].get<double>();
        } else {
            const json& s    = params["sizeMm"];
            const std::string axis = s.is_object() ? json_string(s, "axis") : std::string();
            if (!s.is_object() || !s.contains("mm") || !s["mm"].is_number() ||
                (axis != "x" && axis != "y" && axis != "z" && axis != "max")) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "sizeMm must be {axis: \"x\"|\"y\"|\"z\"|\"max\", mm: number}");
                return;
            }
            const Vec3d  size    = mo->instance_bounding_box(0).size();
            const double current = axis == "x" ? size.x() : axis == "y" ? size.y() : axis == "z" ? size.z() : size.maxCoeff();
            if (current < 1e-6) {
                send_response(id, METHOD, WAB_BAD_PARAMS, "the object has no extent along that axis");
                return;
            }
            factor = s["mm"].get<double>() / current;
        }
        if (!std::isfinite(factor) || factor < 1e-3 || factor > 1e3) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "scale factor out of range (0.001 to 1000)");
            return;
        }

        plater->take_snapshot(std::string("Scale (automated)"));
        // Uniform: every axis of each instance's scale times `factor`, about the instance's own
        // bounding-box centre in XY so it stays where it is; then back onto the bed (BedDrop.hpp).
        const std::vector<double> min_z_before = Actions::instance_min_zs(*mo);
        for (size_t i = 0; i < mo->instances.size(); ++i) {
            ModelInstance* inst   = mo->instances[i];
            const Vec3d    before = mo->instance_bounding_box(i).center();
            inst->set_scaling_factor(inst->get_scaling_factor() * factor);
            const Vec3d after = mo->instance_bounding_box(i).center();
            inst->set_offset(inst->get_offset() + Vec3d(before.x() - after.x(), before.y() - after.y(), 0.));
        }
        Actions::drop_to_bed_unless_sunk(*mo, min_z_before);
        plater->changed_object(obj_idx);
        refile_object_instances(obj_idx);
    }

    const BoundingBoxf3 bb    = mo->instance_bounding_box(0);
    const Vec3d         scale = mo->instances.front()->get_scaling_factor() * 100.;
    const int           plate = plate_holding(bb);
    PartPlateList&      plates = plater->get_partplate_list();
    const Vec3d vol = plates.get_plate(plate >= 0 ? plate : plates.get_curr_plate_index())->get_build_volume().size();
    const Vec3d size = bb.size();
    send_response(id, METHOD, WAB_OK, "success",
                  {{"object", obj_idx},
                   {"name", mo->name},
                   {"scalePercent", {scale.x(), scale.y(), scale.z()}},
                   {"sizeMm", {size.x(), size.y(), size.z()}},
                   {"bboxMm", bbox_json(bb)},
                   {"fitsBed", size.x() <= vol.x() && size.y() <= vol.y() && size.z() <= vol.z()}});
}

void WebAppBridge::Impl::handle_move_object(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.moveObject";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (refuse_model_edit(id, METHOD))
        return;
    Plater* plater = wxGetApp().plater();
    Model&  model  = plater->model();
    if (model.objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    if (plate_job_pending || !plater->get_ui_job_worker().is_idle()) {
        send_response(id, METHOD, WAB_BUSY, "an arrange or fill-bed job is running");
        return;
    }
    int obj_idx = -1;
    if (const int code = resolve_object(params, model, obj_idx, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    ModelObject* mo       = model.objects[obj_idx];
    const int    instance = json_int(params, "instance", 0);
    if (instance < 0 || instance >= int(mo->instances.size())) {
        send_response(id, METHOD, WAB_NOT_FOUND, "instance out of range (" + std::to_string(mo->instances.size()) + " instances)");
        return;
    }

    const bool has_to     = params.contains("to") && !params["to"].is_null();
    const bool has_by     = params.contains("by") && !params["by"].is_null();
    const bool has_center = json_bool(params, "center", false);
    if (int(has_to) + int(has_by) + int(has_center) != 1) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "give exactly one of to, by or center");
        return;
    }
    const BoundingBoxf3 bb = mo->instance_bounding_box(size_t(instance));
    Vec2d               delta;
    if (has_to || has_by) {
        Vec2d v;
        if (!read_vec2(params, has_to ? "to" : "by", v)) {
            send_response(id, METHOD, WAB_BAD_PARAMS, std::string(has_to ? "to" : "by") + " must be [x, y] in mm (finite numbers)");
            return;
        }
        delta = has_to ? Vec2d(v.x() - bb.center().x(), v.y() - bb.center().y()) : v;
    } else {
        // Centre on the plate it is on (the current plate when it is off every bed).
        PartPlateList& plates = plater->get_partplate_list();
        const int      plate  = plate_holding(bb);
        const Vec3d    c      = plates.get_plate(plate >= 0 ? plate : plates.get_curr_plate_index())->get_build_volume().center();
        delta                 = Vec2d(c.x() - bb.center().x(), c.y() - bb.center().y());
    }

    plater->take_snapshot(std::string("Move (automated)"));
    ModelInstance* inst = mo->instances[size_t(instance)];
    inst->set_offset(inst->get_offset() + Vec3d(delta.x(), delta.y(), 0.)); // Z never changes
    mo->invalidate_bounding_box();
    plater->changed_object(obj_idx);
    refile_object_instances(obj_idx);

    const BoundingBoxf3 after = mo->instance_bounding_box(size_t(instance));
    send_response(id, METHOD, WAB_OK, "success",
                  {{"object", obj_idx}, {"name", mo->name}, {"instance", instance},
                   {"bboxMm", bbox_json(after)}, {"insidePlate", inside_a_plate(after)}});
}

void WebAppBridge::Impl::handle_arrange(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.arrange";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (refuse_model_edit(id, METHOD))
        return;
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (plater->model().objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    if (plate_job_pending || !plater->get_ui_job_worker().is_idle()) {
        send_response(id, METHOD, WAB_BUSY, "an arrange or fill-bed job is running");
        return;
    }
    const bool all = json_bool(params, "all", false);
    if (!all && params.contains("plate") && !params["plate"].is_null()) {
        const int plate = json_int(params, "plate", -1);
        if (plate < 0 || plate >= plates.get_plate_count()) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)");
            return;
        }
        if (plate != plates.get_curr_plate_index())
            plater->select_plate(plate);
    }
    GLCanvas3D::ArrangeSettings& settings = plater->canvas3D()->get_arrange_settings();
    const GLCanvas3D::ArrangeSettings saved = settings;
    if (params.contains("spacingMm") && !params["spacingMm"].is_null()) {
        if (!params["spacingMm"].is_number() || params["spacingMm"].get<double>() < 0. || params["spacingMm"].get<double>() > 100.) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "spacingMm must be a number from 0 to 100");
            return;
        }
        settings.distance = float(params["spacingMm"].get<double>());
    }
    if (params.contains("allowRotation") && params["allowRotation"].is_boolean())
        settings.enable_rotation = params["allowRotation"].get<bool>();
    // The rest of the Arrange options popup (GLCanvas3D::_render_arrange_menu), for this run only.
    if (params.contains("allowMultiMaterials") && params["allowMultiMaterials"].is_boolean())
        settings.allow_multi_materials_on_same_plate = params["allowMultiMaterials"].get<bool>();
    if (params.contains("avoidCalibrationRegion") && params["avoidCalibrationRegion"].is_boolean()) {
        // Offered only where the printer scans the first layer (a built-in-vendor printer with
        // scan_first_layer); elsewhere the popup forces it off.
        const DynamicPrintConfig& printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        const ConfigOption*       scan    = printer.option("scan_first_layer");
        const bool offered = wxGetApp().preset_bundle->is_bbl_vendor() && scan != nullptr && scan->getBool();
        if (!offered && params["avoidCalibrationRegion"].get<bool>()) {
            settings = saved;
            send_response(id, METHOD, WAB_BAD_PARAMS, "avoidCalibrationRegion applies only to printers that scan the first layer");
            return;
        }
        settings.avoid_extrusion_cali_region = offered && params["avoidCalibrationRegion"].get<bool>();
    }
    if (params.contains("alignToYAxis") && params["alignToYAxis"].is_boolean()) {
        if (params["alignToYAxis"].get<bool>() && settings.enable_rotation) {
            settings = saved;
            send_response(id, METHOD, WAB_BAD_PARAMS, "alignToYAxis needs rotation off: pass allowRotation: false");
            return;
        }
        settings.align_to_y_axis = params["alignToYAxis"].get<bool>();
    } else if (settings.enable_rotation)
        settings.align_to_y_axis = false; // as the popup does while rotation is on

    // Arrange reads its settings and mode when the job starts; both go back after it finishes, so
    // this run's spacing/rotation never reach the user's own Arrange settings.
    const int saved_state = plater->get_prepare_state();
    plater->set_prepare_state(all ? Job::PREPARE_STATE_DEFAULT : Job::PREPARE_STATE_MENU);
    plate_job_pending = true;
    plater->arrange(); // the Arrange button: its own "Arrange" snapshot, then ArrangeJob
    when_plate_job_done([this, id, saved, saved_state](bool timed_out) {
        Plater* plater                             = wxGetApp().plater();
        plater->canvas3D()->get_arrange_settings() = saved;
        plater->set_prepare_state(saved_state);
        plate_job_pending = false;
        if (timed_out) {
            send_response(id, METHOD, WAB_INTERNAL, "arrange did not finish in time");
            return;
        }
        json objects  = json::array();
        json unplaced = json::array();
        std::set<int> used;
        const Model& model = plater->model();
        for (size_t o = 0; o < model.objects.size(); ++o) {
            const ModelObject* mo = model.objects[o];
            if (mo->instances.empty())
                continue;
            const BoundingBoxf3 bb    = mo->instance_bounding_box(0);
            const int           plate = plate_holding(bb);
            bool                in    = true;
            for (size_t i = 0; i < mo->instances.size(); ++i)
                in = in && inside_a_plate(mo->instance_bounding_box(i));
            if (plate >= 0)
                used.insert(plate);
            if (!in)
                unplaced.push_back(int(o));
            objects.push_back({{"index", int(o)}, {"name", mo->name}, {"plate", plate}, {"instances", mo->instances.size()},
                               {"bboxMm", bbox_json(bb)}});
        }
        send_response(id, METHOD, WAB_OK, "success",
                      {{"plates", json(std::vector<int>(used.begin(), used.end()))}, {"objects", objects}, {"unplaced", unplaced}});
    });
}

void WebAppBridge::Impl::handle_set_copies(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.setCopies";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (refuse_model_edit(id, METHOD))
        return;
    Plater* plater = wxGetApp().plater();
    Model&  model  = plater->model();
    if (model.objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    if (plate_job_pending || !plater->get_ui_job_worker().is_idle()) {
        send_response(id, METHOD, WAB_BUSY, "an arrange or fill-bed job is running");
        return;
    }
    int obj_idx = -1;
    if (const int code = resolve_object(params, model, obj_idx, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    const int count = json_int(params, "count", 0);
    if (count < 1 || count > 200) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "count must be the total number of copies, 1 to 200");
        return;
    }
    const int current = int(model.objects[obj_idx]->instances.size());
    if (count != current) {
        // The +/- buttons: each takes its own "Increase/Decrease Instances" snapshot.
        select_only_object(obj_idx);
        if (count > current) {
            if (!plater->can_increase_instances()) {
                send_response(id, METHOD, WAB_BUSY, "copies cannot be added to this object now (cut object, a non-printable copy, or a gizmo open)");
                return;
            }
            plater->increase_instances(size_t(count - current));
        } else {
            if (!plater->can_decrease_instances()) {
                send_response(id, METHOD, WAB_BUSY, "copies cannot be removed from this object now");
                return;
            }
            plater->decrease_instances(size_t(current - count));
        }
    }
    if (!json_bool(params, "arrange", false)) {
        send_response(id, METHOD, WAB_OK, "success", object_state_json(obj_idx));
        return;
    }
    // Arrange the current plate afterwards, answering when that finishes (as slicer.arrange).
    const int saved_state = plater->get_prepare_state();
    plater->set_prepare_state(Job::PREPARE_STATE_MENU);
    plate_job_pending = true;
    plater->arrange();
    when_plate_job_done([this, id, obj_idx, saved_state](bool timed_out) {
        wxGetApp().plater()->set_prepare_state(saved_state);
        plate_job_pending = false;
        if (timed_out) {
            send_response(id, METHOD, WAB_INTERNAL, "arrange did not finish in time");
            return;
        }
        if (obj_idx >= int(wxGetApp().plater()->model().objects.size())) {
            send_response(id, METHOD, WAB_INTERNAL, "the object went away during arrange");
            return;
        }
        send_response(id, METHOD, WAB_OK, "success", object_state_json(obj_idx));
    });
}

void WebAppBridge::Impl::handle_fill_bed(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.fillBed";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (refuse_model_edit(id, METHOD))
        return;
    Plater* plater = wxGetApp().plater();
    Model&  model  = plater->model();
    if (model.objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    if (plate_job_pending || !plater->get_ui_job_worker().is_idle()) {
        send_response(id, METHOD, WAB_BUSY, "an arrange or fill-bed job is running");
        return;
    }
    int obj_idx = -1;
    if (const int code = resolve_object(params, model, obj_idx, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    // Fill Bed acts on the selection, as from the object's right-click menu: its own snapshot, then
    // FillBedJob. The object stays selected afterwards, as it would by hand.
    const int before = int(model.objects[obj_idx]->instances.size());
    select_only_object(obj_idx);
    plate_job_pending = true;
    plater->fill_bed_with_instances();
    when_plate_job_done([this, id, obj_idx, before](bool timed_out) {
        plate_job_pending = false;
        if (timed_out) {
            send_response(id, METHOD, WAB_INTERNAL, "fill bed did not finish in time");
            return;
        }
        const Model& model = wxGetApp().plater()->model();
        if (obj_idx >= int(model.objects.size())) {
            send_response(id, METHOD, WAB_INTERNAL, "the object went away during fill bed");
            return;
        }
        const int now = int(model.objects[obj_idx]->instances.size());
        send_response(id, METHOD, WAB_OK, "success",
                      {{"object", obj_idx}, {"name", model.objects[obj_idx]->name}, {"instances", now}, {"added", now - before}});
    });
}

// The plate (or every plate, plate < 0) has nothing that would be printed, by the same fresh test
// slicer.getPlate reports: refuse the request with the reason. Returns true when it refused.
bool WebAppBridge::Impl::refuse_nothing_printable(const std::string& id, const char* method, int plate)
{
    Plater*     plater = wxGetApp().plater();
    std::string reason, why;
    if (!explain_nothing_printable(assess_plate_printability(plater->get_partplate_list(), plater->model()), plate, reason, why))
        return false;
    // data.reason stays "nothing_printable" for every cause; data.cause says which.
    send_response(id, method, WAB_NOT_FOUND, "the plate has no printable object: " + why,
                  {{"reason", "nothing_printable"}, {"cause", reason}});
    return true;
}

void WebAppBridge::Impl::handle_get_plate(const std::string& id)
{
    static const char* METHOD = "slicer.getPlate";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    Plater*        plater  = wxGetApp().plater();
    PartPlateList& plates  = plater->get_partplate_list();
    const Model&   model   = plater->model();
    const int      current = plates.get_curr_plate_index();

    int    width = 0, depth = 0;
    double height = 0.;
    plates.get_plate_size(width, depth, height);

    // Per-instance placement: the single verdict the slice / export / send refusals also use
    // (assess_plate_printability: real bed shape, exclude areas, bed bottom, printable flags),
    // computed fresh on every call. Nothing in the model is modified.
    const PlatePrintability assessment  = assess_plate_printability(plates, model);
    const int               plate_count = int(assessment.plates.size());
    int                     outside_count = 0;

    json plate_list = json::array();
    for (int i = 0; i < plate_count; ++i) {
        PartPlate* plate = plates.get_plate(i);
        if (plate == nullptr)
            continue;
        const PlateCounts& counts = assessment.plates[size_t(i)];
        outside_count += counts.outside_bed + counts.exclude_area;
        plate_list.push_back({{"index", i},
                              {"isCurrent", i == current},
                              {"name", plate->get_plate_name()},
                              {"hasPrintableInstances", counts.printable > 0},
                              {"sliceResultValid", plate->is_slice_result_valid()},
                              {"printableCount", counts.printable},
                              {"outsideCount", counts.outside_bed + counts.exclude_area},
                              {"excludeAreaCount", counts.exclude_area},
                              {"notPrintableCount", counts.disabled},
                              // The plate list's own cached flag (rectangular bounds, updated on
                              // instance moves), for diagnostics only.
                              {"plateListHasPrintable", plate->has_printable_instances()}});
    }
    outside_count += assessment.off_plate_count;

    constexpr size_t max_objects   = 100;
    constexpr size_t max_instances = 200; // per object, in instanceStates
    json             objects       = json::array();
    for (size_t obj_idx = 0; obj_idx < model.objects.size() && objects.size() < max_objects; ++obj_idx) {
        const ModelObject* object   = model.objects[obj_idx];
        int                on_plate = -1;
        for (int i = 0; i < plates.get_plate_count() && on_plate < 0; ++i)
            if (PartPlate* plate = plates.get_plate(i); plate != nullptr && plate->contain_instance(int(obj_idx), 0))
                on_plate = i;
        json entry = {{"index", obj_idx},
                      {"name", object->name},
                      {"plate", on_plate},
                      {"printable", object->printable},
                      {"instances", object->instances.size()}};
        if (!object->instances.empty()) {
            const BoundingBoxf3 bb   = object->instance_bounding_box(0);
            const Vec3d         size = bb.size();
            entry["bboxMm"] = {{"min", {bb.min.x(), bb.min.y(), bb.min.z()}},
                               {"max", {bb.max.x(), bb.max.y(), bb.max.z()}},
                               {"size", {size.x(), size.y(), size.z()}}};
        }
        const std::vector<InstancePrintability>& inst_states   = assessment.instances[obj_idx];
        json                                     states        = json::array();
        int                                      obj_outside   = 0;
        bool                                     any_printable = false;
        for (size_t inst_idx = 0; inst_idx < inst_states.size(); ++inst_idx) {
            const InstancePrintability& st      = inst_states[inst_idx];
            const bool                  outside = !st.printable() && st.placement != InstancePlacement::Disabled;
            if (outside)
                ++obj_outside;
            any_printable = any_printable || st.printable();
            if (inst_idx < max_instances)
                states.push_back({{"index", inst_idx},
                                  {"plate", st.plate},
                                  {"placement", instance_placement_name(st.placement)},
                                  {"volumeState", st.plate >= 0 ? print_volume_state_name(st.state) : "fullyOutside"},
                                  {"outside", outside},
                                  {"printable", st.printable()}});
        }
        entry["instanceStates"]          = std::move(states);
        entry["instanceStatesTruncated"] = inst_states.size() > max_instances;
        entry["outsideCount"]            = obj_outside;
        entry["anyPrintable"]            = any_printable;
        // Multiplanar cut planes, when the object has any (world frame; see Actions::cut_planes_json).
        if (!object->cut_planes.planes.empty())
            entry["cutPlanes"] = Actions::cut_planes_json(*object);
        objects.push_back(entry);
    }

    json response = {{"printerTechnology", plater->printer_technology() == ptSLA ? "SLA" : "FFF"},
                     {"projectName", std::string(plater->get_project_name().ToUTF8().data())},
                     {"projectDirty", plater->is_project_dirty()},
                     {"isEmpty", model.objects.empty()},
                     {"currentPlate", current},
                     {"plateCount", plates.get_plate_count()},
                     {"bedMm", {{"width", width}, {"depth", depth}, {"height", height}}},
                     {"plates", plate_list},
                     {"objects", objects},
                     {"objectsTruncated", model.objects.size() > max_objects},
                     {"hasPrintableInstances", assessment.plate_has_printable(current)},
                     {"instanceCount", assessment.instance_count},
                     {"printableCount", assessment.printable_count},
                     {"outsideCount", outside_count},
                     {"offPlateCount", assessment.off_plate_count}};
    // Why the current plate has nothing to print, in the words the slice / export refusals use.
    if (std::string reason, why; !model.objects.empty() && explain_nothing_printable(assessment, current, reason, why))
        response["nothingPrintable"] = {{"reason", reason}, {"message", why}};
    send_response(id, METHOD, WAB_OK, "success", response);
}

// How much of an export the page may pull in one call. Base64 inflates by a third and the whole
// chunk crosses the process boundary as a single JSON string, so this is a latency budget rather
// than a capacity one: big enough that a typical project is a handful of round trips, small enough
// that none of them visibly stalls the GUI thread.
constexpr size_t WAB_EXPORT_CHUNK_BYTES = 4u * 1024u * 1024u;
// At most this many exports alive at once per tab, so a page that never calls discardExport cannot
// quietly fill the cache directory.
constexpr size_t WAB_MAX_LIVE_EXPORTS = 3;

void WebAppBridge::Impl::discard_all_exports()
{
    boost::system::error_code ec;
    for (const auto& entry : exports)
        fs::remove(entry.second, ec);
    exports.clear();
}

void WebAppBridge::Impl::handle_export_project(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.exportProject";
    if (!owner.allows_export()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export the project");
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    Plater* plater = wxGetApp().plater();
    if (plater->model().objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    if (exports.size() >= WAB_MAX_LIVE_EXPORTS) {
        send_response(id, METHOD, WAB_BUSY,
                      "too many exports are still open; discard the ones you have finished with");
        return;
    }

    // One plate, or the whole project. -1 is what export_3mf reads as "everything".
    int plate_idx = -1;
    if (params.contains("plate")) {
        PartPlateList& plates = plater->get_partplate_list();
        plate_idx             = json_int(params, "plate", -1);
        if (plate_idx < 0 || plate_idx >= plates.get_plate_count()) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)");
            return;
        }
    }

    // Named after the project, so what the user publishes carries the name they gave it, plus a
    // random part because several exports can be alive at once.
    std::string stem = sanitise_filename(std::string(plater->get_project_name().ToUTF8().data()));
    if (stem.empty())
        stem = "project";
    const std::string handle   = make_session_token().substr(0, 16);
    const fs::path    out_path = webapp_cache_dir() / path_from_utf8(stem + "-" + handle + ".3mf");

    // 0.1.6 privacy: handing the project to a web page is a model upload, so it is asked every time,
    // for this file, with what / why / to whom / how long (PrivacyConsent::ModelUploadRequest). Usage
    // diagnostics consent never covers it, and a "no" changes nothing else in the slicer. The page
    // learns whether AI training was allowed for this upload and must keep to it.
    PrivacyConsent::ModelUploadRequest upload;
    upload.file_label    = stem + ".3mf";
    upload.file_id       = handle;
    upload.included_data = plate_idx < 0 ? _u8L("The whole project as a 3MF file: every model's geometry, the settings, and plate thumbnails.")
                                         : _u8L("This plate as a 3MF file: its models' geometry, the settings, and the plate thumbnail.");
    upload.purpose       = _u8L("Sharing or publishing it on Unlayered Universe, as you asked in the Universe tab.");
    upload.ai_training_proposed = true;
    upload.recipients    = _u8L("Unlayered 3D, Inc., through Unlayered Universe (unlayered3d.com).");
    upload.retention     = _u8L("As set out in the Unlayered Universe terms and privacy policy; you can delete what you publish there.");
    PrivacyConsent::ModelUploadAuthorization authorization = ModelUploadConsentDialog::ask(wxGetApp().mainframe, upload);
    const bool ai_training = authorization.ai_training_allowed();
    if (!authorization.consume(handle)) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "the user declined to upload this project");
        return;
    }

    boost::system::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);

    // Silence: this runs behind a click in the page, so it must not raise a dialog of its own.
    if (plater->export_3mf(out_path, SaveStrategy::Silence, plate_idx) < 0) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, WAB_INTERNAL, "the slicer could not export the project");
        return;
    }

    const std::uintmax_t size = fs::file_size(out_path, ec);
    if (ec || size == 0) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, WAB_INTERNAL, "the export produced no file");
        return;
    }
    if (size > WAB_MAX_DOWNLOAD_BYTES) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, WAB_BAD_PARAMS,
                      "the exported project is larger than the " +
                          std::to_string(WAB_MAX_DOWNLOAD_BYTES / (1024 * 1024)) + " MB limit");
        return;
    }

    exports[handle] = out_path;
    send_response(id, METHOD, WAB_OK, "success",
                  {{"handle", handle},
                   {"filename", stem + ".3mf"},
                   {"sizeBytes", size},
                   {"chunkBytes", WAB_EXPORT_CHUNK_BYTES},
                   // The person's answer for this upload; false unless they ticked the box.
                   {"aiTrainingAllowed", ai_training}});
}

void WebAppBridge::Impl::handle_read_export_chunk(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.readExportChunk";
    const std::string handle = json_string(params, "handle");
    if (!owner.allows_export() && handoff_handles.count(handle) == 0) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export the project");
        return;
    }

    const auto        it     = exports.find(handle);
    if (handle.empty() || it == exports.end()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "unknown export handle");
        return;
    }

    boost::system::error_code ec;
    const std::uintmax_t      size = fs::file_size(it->second, ec);
    if (ec) {
        send_response(id, METHOD, WAB_INTERNAL, "the export is gone");
        return;
    }

    const long long offset = params.contains("offset") ? json_int(params, "offset", 0) : 0;
    if (offset < 0 || std::uintmax_t(offset) > size) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "offset is outside the export");
        return;
    }
    size_t want = WAB_EXPORT_CHUNK_BYTES;
    if (params.contains("length")) {
        const long long asked = json_int(params, "length", 0);
        if (asked <= 0) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "length must be positive");
            return;
        }
        want = (std::min)(size_t(asked), WAB_EXPORT_CHUNK_BYTES);
    }
    want = (std::min)(want, size_t(size - std::uintmax_t(offset)));

    std::vector<char> buffer(want);
    if (want > 0) {
        boost::nowide::ifstream in(path_to_utf8(it->second), std::ios::binary);
        if (!in) {
            send_response(id, METHOD, WAB_INTERNAL, "the export could not be read");
            return;
        }
        in.seekg(offset);
        in.read(buffer.data(), std::streamsize(want));
        if (in.gcount() != std::streamsize(want)) {
            send_response(id, METHOD, WAB_INTERNAL, "the export was truncated while being read");
            return;
        }
    }

    std::string  encoded(boost::beast::detail::base64::encoded_size(want), char(0));
    const size_t written = boost::beast::detail::base64::encode(encoded.data(), buffer.data(), want);
    encoded.resize(written);

    const std::uintmax_t next = std::uintmax_t(offset) + want;
    send_response(id, METHOD, WAB_OK, "success",
                  {{"handle", handle},
                   {"offset", offset},
                   {"length", want},
                   {"nextOffset", next},
                   {"eof", next >= size},
                   {"base64", encoded}});
}

void WebAppBridge::Impl::handle_discard_export(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.discardExport";
    const std::string handle = json_string(params, "handle");
    if (!owner.allows_export() && handoff_handles.count(handle) == 0) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export the project");
        return;
    }
    handoff_handles.erase(handle);
    const auto        it     = exports.find(handle);
    // Idempotent on purpose: a page cleaning up after a failure should not have to work out
    // whether the export survived.
    if (it != exports.end()) {
        boost::system::error_code ec;
        fs::remove(it->second, ec);
        exports.erase(it);
    }
    send_response(id, METHOD, WAB_OK, "success", {{"handle", handle}});
}

static size_t plate_fingerprint(); // below, with on_tab_shown
static std::string handoff_state_token(); // below, with plate_fingerprint

// A hand-off is the user, not the page, choosing to send the plate: "Slice with <tab>" in the
// slice menu. So it needs no export opt-in, and only the handle it creates becomes readable.
std::string WebAppBridge::hand_off_plate(int plate_idx, const std::string& action, bool park, json* body)
{
    Impl&   d      = *p;
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->model().objects.empty())
        return std::string("There is nothing on the plate.");
    PartPlateList& plates = plater->get_partplate_list();
    if (plate_idx < 0)
        plate_idx = plates.get_curr_plate_index();
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count())
        return std::string("That plate does not exist.");

    // An uncollected earlier hand-off is superseded: drop its file rather than let it pile up.
    boost::system::error_code ec;
    if (d.pending_handoff.is_object() && d.pending_handoff.contains("handle")) {
        const std::string old = d.pending_handoff["handle"].get<std::string>();
        if (auto it = d.exports.find(old); it != d.exports.end()) {
            fs::remove(it->second, ec);
            d.exports.erase(it);
        }
        d.handoff_handles.erase(old);
    }
    // Room for it: the oldest hand-offs go first (they are ours to discard), then refuse.
    while (d.exports.size() >= WAB_MAX_LIVE_EXPORTS && !d.handoff_handles.empty()) {
        const std::string old = *d.handoff_handles.begin();
        if (auto it = d.exports.find(old); it != d.exports.end()) {
            fs::remove(it->second, ec);
            d.exports.erase(it);
        }
        d.handoff_handles.erase(old);
    }
    if (d.exports.size() >= WAB_MAX_LIVE_EXPORTS)
        return std::string("The page still holds too many exports; reload it and try again.");

    std::string stem = sanitise_filename(std::string(plater->get_project_name().ToUTF8().data()));
    if (stem.empty())
        stem = "project";
    const std::string handle   = make_session_token().substr(0, 16);
    const fs::path    out_path = webapp_cache_dir() / path_from_utf8(stem + "-" + handle + ".3mf");
    fs::create_directories(out_path.parent_path(), ec);
    // One self-contained .3mf (no split-out object files, which browser 3MF loaders cannot
    // follow), with the project's settings embedded like any saved project -- the same form
    // slicer.exportProject produces.
    if (plater->export_3mf(out_path, SaveStrategy::Silence, plate_idx) < 0) {
        fs::remove(out_path, ec);
        return std::string("The plate could not be exported.");
    }
    const std::uintmax_t size = fs::file_size(out_path, ec);
    if (ec || size == 0 || size > WAB_MAX_DOWNLOAD_BYTES) {
        fs::remove(out_path, ec);
        return ec || size == 0 ? std::string("The export produced no file.")
                               : std::string("The exported plate is too large to hand over.");
    }

    d.exports[handle] = out_path;
    d.handoff_handles.insert(handle);
    PresetBundle& bundle  = *wxGetApp().preset_bundle;
    // UNLAYERED_MOD -- the filament the plate's part prints with, not always slot 1: a Benchy set to
    // filament 2 handed the page filament 1 (2026-10-10). The first object on the plate decides
    // (its own "extruder", else its first part's), as a service reading the 3MF would see it.
    std::string filament = bundle.filament_presets.empty() ? std::string() : bundle.filament_presets.front();
    {
        const Model& model = plater->model();
        PartPlate*   plate = plates.get_plate(plate_idx);
        for (size_t obj_idx = 0; plate != nullptr && obj_idx < model.objects.size(); ++obj_idx) {
            if (!plate->contain_instance(int(obj_idx), 0))
                continue;
            const ModelObject* object = model.objects[obj_idx];
            int                ext    = 0;
            if (const ConfigOption* opt = object->config.option("extruder"))
                ext = opt->getInt();
            if (ext <= 0 && !object->volumes.empty())
                if (const ConfigOption* opt = object->volumes.front()->config.option("extruder"))
                    ext = opt->getInt();
            if (ext >= 1 && size_t(ext) <= bundle.filament_presets.size())
                filament = bundle.filament_presets[size_t(ext) - 1];
            break;
        }
    }
    json          handoff = {{"action", action},
                             {"handle", handle},
                             {"filename", stem + ".3mf"},
                             {"sizeBytes", size},
                             {"chunkBytes", WAB_EXPORT_CHUNK_BYTES},
                             {"plate", plate_idx},
                             {"printer", bundle.printers.get_selected_preset_name()},
                             {"process", bundle.prints.get_selected_preset_name()},
                             {"filament", filament}};
    d.last_handoff_fp = plate_fingerprint();
    if (!park) {
        // The page asked and is waiting on the reply: hand the details straight back. Nothing is
        // parked and nothing announced, or the page would collect the same plate a second time.
        if (body != nullptr)
            *body = std::move(handoff);
        return {};
    }
    d.pending_handoff = std::move(handoff);
    // Tell a page that is already listening; one that loads later collects it on start-up.
    d.notify("slicer.handoff", d.pending_handoff);
    return {};
}

// The page pulls the plate rather than being handed it: a side panel may do this right before
// it acts on the plate, and only when the plate changed since it last did (it keeps its own
// snapshot). Gated on the settings OR the export permission -- a page trusted to rewrite every
// print setting of the plate may read the plate it is editing -- and never on a tab with neither.
void WebAppBridge::Impl::handle_request_handoff(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.requestHandoff";
    if (!owner.allows_settings() && !owner.allows_export()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to request the plate");
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    const int plate_idx = params.contains("plate") && params["plate"].is_number_integer() ? params["plate"].get<int>() : -1;

    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->model().objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }
    // UNLAYERED_MOD (2026-10-25, hand-off freeze): `token` names the plate and settings this
    // hand-off carries. A page that passes the token of the plate it last uploaded as
    // `ifChangedSince` gets {unchanged: true} -- no export, no file, nothing to read -- when nothing
    // has changed since; `probe: true` only asks (never exports). A page that passes neither gets
    // the hand-off as before, now with its token.
    const std::string token = handoff_state_token();
    const bool        same  = (plate_idx < 0 || plate_idx == plater->get_partplate_list().get_curr_plate_index()) &&
                       params.contains("ifChangedSince") && params["ifChangedSince"].is_string() &&
                       params["ifChangedSince"].get<std::string>() == token;
    if (same || json_bool(params, "probe", false)) {
        send_response(id, METHOD, WAB_OK, "success", {{"unchanged", same}, {"token", token}});
        return;
    }
    json              body;
    const std::string err = owner.bridge()->hand_off_plate(plate_idx, "request", /*park=*/false, &body);
    if (!err.empty()) {
        send_response(id, METHOD, WAB_IO, err);
        return;
    }
    body["unchanged"] = false;
    body["token"]     = token;
    send_response(id, METHOD, WAB_OK, "success", body);
}

void WebAppBridge::Impl::handle_take_handoff(const std::string& id)
{
    // Once: the page that takes it owns it, so a reload does not import the same plate twice.
    json data = pending_handoff.is_object() ? pending_handoff : json{{"pending", false}};
    if (pending_handoff.is_object())
        data["pending"] = true;
    pending_handoff = json();
    send_response(id, "slicer.takeHandoff", WAB_OK, "success", data);
}

void WebAppBridge::Impl::handle_export_thumbnail(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.exportThumbnail";
    if (!owner.allows_export()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export the project");
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (plater->model().objects.empty()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate is empty");
        return;
    }

    int plate_idx = plates.get_curr_plate_index();
    if (params.contains("plate")) {
        plate_idx = json_int(params, "plate", plate_idx);
        if (plate_idx < 0 || plate_idx >= plates.get_plate_count()) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)");
            return;
        }
    }

    // Renders only the plates whose picture is stale, so the usual case costs nothing.
    plater->update_all_plate_thumbnails(false);

    PartPlate* plate = plates.get_plate(plate_idx);
    if (plate == nullptr || !plate->thumbnail_data.is_valid()) {
        // Rendering needs the 3D canvas, and the canvas is hidden while a web-application tab is
        // in front. Rather than hand back a blank or half-drawn image, say what is missing: the
        // Prepare tab has to have been shown at least once since the plate last changed.
        send_response(id, METHOD, WAB_NOT_FOUND,
                      "no picture of the plate is available yet -- open the Prepare tab once, then try again");
        return;
    }

    const ThumbnailData& thumb = plate->thumbnail_data;
    auto                 png   = GCodeThumbnails::compress_thumbnail(thumb, GCodeThumbnailsFormat::PNG);
    if (!png || png->data == nullptr || png->size == 0) {
        send_response(id, METHOD, WAB_INTERNAL, "the plate picture could not be encoded");
        return;
    }

    std::string  encoded(boost::beast::detail::base64::encoded_size(png->size), char(0));
    const size_t written = boost::beast::detail::base64::encode(encoded.data(), png->data, png->size);
    encoded.resize(written);

    send_response(id, METHOD, WAB_OK, "success",
                  {{"plate", plate_idx},
                   {"mime", "image/png"},
                   {"width", thumb.width},
                   {"height", thumb.height},
                   {"sizeBytes", png->size},
                   {"base64", encoded}});
}

// The Slice dropdown's modes. A web app may read and pick them (preselect one for the person), but a slicing-service mode runs in the cloud, so it never starts one: slicer.slice
// refuses while one is selected, and Plater::reslice() starts a service run only on the person's
// own request (Plater::note_user_slice_request), never during a bridge call (ServiceSliceBlock).
// A plate's slicing-service state (Plater::slice_service_plate_state) for getSliceModes and
// getSliceResult: so a page can say "press Slice" instead of waiting for a run it cannot start.
static json service_plate_json(Plater& plater, int plate)
{
    const Plater::SliceServicePlateState st = plater.slice_service_plate_state(plate);
    json out = {{"plate", plate},
                {"serviceResult", st.service_result},
                {"serviceResultCurrent", st.matches_options},
                {"resultWaiting", st.result_waiting},
                {"jobRunning", st.job_running},
                {"waitingForSlice", st.waiting_for_person}};
    if (st.job_running && st.progress >= 0) {
        out["progress"] = st.progress;
        out["stage"]    = st.stage;
    }
    return out;
}

json WebAppBridge::Impl::slice_modes_json() const
{
    MainFrame* frame = wxGetApp().mainframe;
    json       modes = json::array();
    if (frame == nullptr)
        return {{"modes", modes}, {"selected", nullptr}};
    for (const MainFrame::SliceModeChoice& choice : frame->slice_mode_choices())
        modes.push_back({{"id", choice.id},
                         {"label", std::string(choice.label.ToUTF8().data())},
                         {"kind", choice.service ? "service" : "local"}});
    json out = {{"modes", modes}, {"selected", frame->selected_slice_mode()}};
    if (Plater* plater = wxGetApp().plater(); plater != nullptr && frame->slice_mode_choices().size() > 2) {
        json plates = json::array();
        for (int i = 0; i < plater->get_partplate_list().get_plate_count(); ++i)
            plates.push_back(service_plate_json(*plater, i));
        out["service"] = {{"prepare", !frame->slice_mode_is_service() ? "off" : frame->slice_prepare_deferred() ? "deferred" : "active"},
                          {"plates", plates}};
    }
    return out;
}

void WebAppBridge::Impl::handle_get_slice_modes(const std::string& id)
{
    static const char* METHOD = "slicer.getSliceModes";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    send_response(id, METHOD, WAB_OK, "success", slice_modes_json());
}

void WebAppBridge::Impl::handle_set_slice_mode(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.setSliceMode";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    const std::string mode = json_string(params, "id");
    if (mode.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "id is required (see slicer.getSliceModes)", {{"reason", "unknown_mode"}});
        return;
    }
    // Picks the entry as the person would, minus every dialog; slices nothing, and a service mode
    // starts no prepare on the service (MainFrame::select_slice_mode).
    const std::string error = wxGetApp().mainframe->select_slice_mode(mode);
    if (!error.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "unknown slicing mode '" + mode + "' (see slicer.getSliceModes)",
                      {{"reason", error}, {"id", mode}});
        return;
    }
    json data = slice_modes_json();
    send_response(id, METHOD, WAB_OK, "success", {{"selected", data["selected"]}, {"modes", data["modes"]}});
}

void WebAppBridge::Impl::handle_slice(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.slice";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    // A slicing-service mode is a cloud run: only the person starts it. Refused before anything
    // (the plate switch included) changes.
    if (MainFrame* frame = wxGetApp().mainframe; frame != nullptr && frame->slice_mode_is_service()) {
        send_response(id, METHOD, WAB_SERVICE_MODE_SELECTED,
                      "a slicing-service (cloud) mode is selected; the person must press Slice themselves "
                      "(or pick a local mode with slicer.setSliceMode)",
                      {{"reason", "service_mode_selected"}, {"selected", frame->selected_slice_mode()}});
        return;
    }

    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (plater->only_gcode_mode()) {
        send_response(id, METHOD, WAB_NOT_FOUND, "a G-code file is open; there is no model to slice (File > New Project first)");
        return;
    }
    if (params.contains("plate")) {
        const int plate = json_int(params, "plate", -1);
        if (plate < 0 || plate >= plates.get_plate_count()) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)");
            return;
        }
        if (plate != plates.get_curr_plate_index())
            plater->select_plate(plate);
    }
    PartPlate* plate = plates.get_curr_plate();
    if (plate == nullptr) {
        send_response(id, METHOD, WAB_NOT_FOUND, "the plate has no printable object", {{"reason", "nothing_printable"}});
        return;
    }
    if (refuse_nothing_printable(id, METHOD, plates.get_curr_plate_index()))
        return;

    // Plater::reslice() is the Slice button: it validates, may leave a notification instead of
    // starting, and reports nothing back. The outcome arrives as slicer.sliceStarted /
    // slicer.sliceFinished events; `accepted` says whether a slice is actually under way.
    plater->reslice();
    const bool started = plater->is_background_process_slicing();
    json       data    = {{"accepted", started}, {"plate", plates.get_curr_plate_index()}, {"sliceResultValid", plate->is_slice_result_valid()}};
    if (!started && !plate->is_slice_result_valid()) {
        // Why the Slice button did nothing: the validation error its notification shows (layer height
        // over the nozzle's limit, an object off the bed ...), with the setting it names. Without it a
        // page only sees that no sliceStarted ever came.
        const StringObjectException err = plater->background_process().validate();
        if (!err.string.empty() && !err.is_warning) {
            data["reason"] = err.string;
            if (!err.opt_key.empty())
                data["optKey"] = err.opt_key;
        }
    }
    send_response(id, METHOD, WAB_OK, started ? "accepted" : "not started", data);
}

void WebAppBridge::Impl::handle_get_slice_result(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.getSliceResult";
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(false, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }

    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    int            index  = plates.get_curr_plate_index();
    if (params.contains("plate")) {
        index = json_int(params, "plate", -1);
        if (index < 0 || index >= plates.get_plate_count()) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)");
            return;
        }
    }
    PartPlate* plate   = plates.get_plate(index);
    json       service = service_plate_json(*wxGetApp().plater(), index);
    if (plate == nullptr || !plate->is_slice_result_valid()) {
        send_response(id, METHOD, WAB_NOT_FOUND,
                      "plate " + std::to_string(index) + " has no valid slice result" +
                          (service["waitingForSlice"].get<bool>() ? "; a service mode is selected: the person must press Slice" :
                           service["jobRunning"].get<bool>()      ? "; a service slice of it is running" : ""),
                      {{"plate", index}, {"service", service}});
        return;
    }
    json data       = WebAppSettings::slice_summary_json(plate);
    data["plate"]   = index;
    data["source"]  = service["serviceResult"].get<bool>() ? "service" : "local";
    data["service"] = service;
    send_response(id, METHOD, WAB_OK, "success", data);
}

// ---------------------------------------------------------------------------------------------
// Printer profiles (WEB_APP_BRIDGE.md section 4.4; policy, dialog and import in
// WebAppPresetShare.cpp)
//
// Export is the publish direction of a printer, under the same opt-in as exporting the plate: the
// slicer writes the edited printer preset -- flattened, print host and local paths removed -- into
// its cache and hands back an export handle that readExportChunk / discardExport serve unchanged.
// Install is the other direction under its own opt-in, and is never the page's decision alone:
// the file is downloaded on the same terms as openProjectFromUrl, sanitised again, and installed
// only if the user says so in a native dialog the page can neither skip nor pre-answer.
// ---------------------------------------------------------------------------------------------

void WebAppBridge::Impl::handle_export_printer_preset(const std::string& id, const json& /*params*/)
{
    static const char* METHOD = "slicer.exportPrinterPreset";
    if (!owner.allows_export()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to export");
        return;
    }
    Tab*        tab = nullptr;
    std::string message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (exports.size() >= WAB_MAX_LIVE_EXPORTS) {
        send_response(id, METHOD, WAB_BUSY, "too many exports are still open; discard the ones you have finished with");
        return;
    }

    std::string stem = sanitise_filename(WebAppPresetShare::edited_printer_preset_name());
    if (stem.empty())
        stem = "printer";
    const std::string handle   = make_session_token().substr(0, 16);
    const fs::path    out_path = webapp_cache_dir() / path_from_utf8(stem + "-" + handle + ".json");

    const WebAppPresetShare::ExportResult result = WebAppPresetShare::export_edited_printer_preset(out_path);
    boost::system::error_code             ec;
    if (result.code != WAB_OK) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, result.code, result.message);
        return;
    }
    const std::uintmax_t size = fs::file_size(out_path, ec);
    if (ec || size == 0) {
        fs::remove(out_path, ec);
        send_response(id, METHOD, WAB_INTERNAL, "the export produced no file");
        return;
    }

    exports[handle] = out_path;
    send_response(id, METHOD, WAB_OK, "success",
                  {{"handle", handle},
                   {"filename", stem + ".json"},
                   {"sizeBytes", size},
                   {"chunkBytes", WAB_EXPORT_CHUNK_BYTES},
                   {"name", result.name},
                   {"dirty", result.dirty},
                   {"inherits", result.inherits}});
}

void WebAppBridge::Impl::handle_install_printer_preset(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.installPrinterPreset";
    static const char* EVENT  = "slicer.presetInstalled";
    if (!owner.allows_install()) {
        send_response(id, METHOD, WAB_CAPABILITY_DISABLED, "this tab is not allowed to install printer profiles");
        return;
    }
    const std::string url = json_string(params, "url");
    if (url.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "url is required");
        return;
    }
    std::string message;
    if (const int code = check_download_url(url, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    std::string name = sanitise_filename(json_string(params, "filename"));
    if (name.empty())
        name = sanitise_filename(Http::get_filename_from_url(url));
    if (name.empty()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "filename is required");
        return;
    }
    if (const std::string ext = lowercase_extension(path_from_utf8(name)); ext != "json") {
        send_response(id, METHOD, WAB_UNSUPPORTED_TYPE, "file type '" + ext + "' is not accepted: a printer profile is a .json file");
        return;
    }
    // A dialog already open, or a slice running: the install ends in a modal of its own.
    Tab* tab = nullptr;
    if (const int code = preflight(true, Preset::TYPE_PRINTER, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (install_in_progress.load()) {
        send_response(id, METHOD, WAB_BUSY, "a printer profile is already being installed");
        return;
    }
    if (!claim_download()) {
        send_response(id, METHOD, WAB_BUSY, "another download is already in progress");
        return;
    }
    install_in_progress.store(true);

    DownloadJob job;
    job.id           = id;
    job.url          = url;
    job.expected_sha = boost::algorithm::to_lower_copy(json_string(params, "sha256"));
    // Never the page's name on disk: a profile must not overwrite a file openProjectFromUrl fetched.
    job.destination = webapp_cache_dir() / path_from_utf8("printer-profile-" + make_session_token().substr(0, 16) + ".json");
    job.size_limit  = WAB_MAX_PRESET_BYTES;
    job.on_http_error = [id](Impl& self, const std::string& message, unsigned status) {
        self.install_in_progress.store(false);
        int         code  = (status == 404 || status == 410) ? WAB_NOT_FOUND : WAB_IO;
        std::string error = message;
        if (status == 0 && boost::algorithm::starts_with(message, "HTTP body data size exceeded")) {
            code  = WAB_TOO_LARGE;
            error = "the file is larger than the " + std::to_string(WAB_MAX_PRESET_BYTES / (1024 * 1024)) +
                    " MiB a printer profile may be";
        }
        self.send_event(id, EVENT, {{"ok", false}, {"error", error}, {"code", code}});
    };
    job.on_save_error = [id](Impl& self, const std::string& error) {
        self.install_in_progress.store(false);
        self.send_event(id, EVENT, {{"ok", false}, {"error", error}, {"code", WAB_IO}});
    };
    job.on_ready = [id](Impl& self, const fs::path& destination) {
        // GUI thread with the tab alive (run_download checked); the dialog below is modal, and the
        // shared_ptr the caller holds keeps this Impl alive through it.
        const wxString                   tab_title = WebAppPanel::configured_title(self.owner.slot());
        WebAppPresetShare::InstallResult outcome;
        try {
            outcome = WebAppPresetShare::install_printer_preset(destination, tab_title, webapp_cache_dir());
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "WebAppBridge: installing a printer profile failed: " << e.what();
            outcome       = WebAppPresetShare::InstallResult();
            outcome.code  = WAB_INTERNAL;
            outcome.error = std::string("the profile could not be installed: ") + e.what();
        }
        boost::system::error_code ec;
        fs::remove(destination, ec);
        self.install_in_progress.store(false);

        json data = {{"ok", outcome.ok}};
        if (outcome.ok) {
            data["name"]     = outcome.name;
            data["selected"] = outcome.selected;
        } else if (outcome.cancelled) {
            data["cancelled"] = true;
        } else {
            data["error"] = outcome.error;
            data["code"]  = outcome.code;
        }
        self.send_event(id, EVENT, data);
    };

    // Accepted, not finished: the outcome -- installed, declined, or why not -- arrives as a
    // slicer.presetInstalled event carrying this id.
    send_response(id, METHOD, WAB_OK, "accepted", {{"accepted", true}});
    run_download(std::move(job));
}

// ---------------------------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------------------------

WebAppBridge::WebAppBridge(WebAppPanel& owner) : p(std::make_shared<Impl>(owner))
{
    // Actions: what the catalog runtime may ask of this bridge. Every callback holds the Impl weakly.
    auto                host = std::make_shared<Actions::BridgeHost>();
    std::weak_ptr<Impl> weak = p;
    host->respond = [weak](const std::string& id, const std::string& method, int code, const std::string& message, const json& data) {
        if (auto self = weak.lock(); self && !self->detached.load())
            self->send_response(id, method, code, message, data);
    };
    host->event = [weak](const std::string& method, const json& data) {
        if (auto self = weak.lock(); self && !self->detached.load())
            self->notify(method, data);
    };
    host->allows = [weak](Actions::Gate gate) {
        auto self = weak.lock();
        if (!self || self->detached.load())
            return false;
        switch (gate) {
        case Actions::Gate::Settings: return self->owner.allows_settings();
        case Actions::Gate::Export: return self->owner.allows_export();
        case Actions::Gate::Printer: return self->owner.allows_printer();
        case Actions::Gate::Install: return self->owner.allows_install();
        }
        return false;
    };
    host->base_capabilities = [weak]() {
        auto self = weak.lock();
        return self && !self->detached.load() ? self->capabilities() : json::array();
    };
    host->slot_id = [weak]() {
        auto self = weak.lock();
        return self && !self->detached.load() ? std::string(self->owner.slot().id) : std::string();
    };
    host->busy = [weak]() {
        auto self = weak.lock();
        return self && self->slicer_is_busy();
    };
    // openProjectFromUrl's download rules for actions that take a url (svg.add, part.add{url} ...).
    host->download = [weak](const std::string& url, const std::vector<std::string>& allowed_exts,
                            std::function<void(const std::string&)> on_ready, std::function<void(int, const std::string&)> on_error) {
        auto self = weak.lock();
        if (!self || self->detached.load())
            return;
        std::string message;
        if (const int code = self->check_download_url(url, message); code != WAB_OK)
            return on_error(code, message);
        const std::string name = sanitise_filename(Http::get_filename_from_url(url));
        const std::string ext  = name.empty() ? std::string() : lowercase_extension(path_from_utf8(name));
        if (name.empty() || std::find(allowed_exts.begin(), allowed_exts.end(), ext) == allowed_exts.end())
            return on_error(WAB_UNSUPPORTED_TYPE, "file type '" + ext + "' is not accepted here");
        if (!self->claim_download())
            return on_error(WAB_BUSY, "another download is already in progress");
        Impl::DownloadJob job;
        job.url           = url;
        job.destination   = webapp_cache_dir() / path_from_utf8("action-" + name);
        job.size_limit    = WAB_MAX_DOWNLOAD_BYTES;
        job.on_http_error = [on_error](Impl&, const std::string& message, unsigned status) {
            const int code = (status == 404 || status == 410) ? WAB_NOT_FOUND : WAB_IO;
            wxGetApp().CallAfter([on_error, code, message]() { on_error(code, message); }); // worker thread -> GUI
        };
        job.on_save_error = [on_error](Impl&, const std::string& error) { on_error(WAB_IO, error); };
        job.on_ready      = [on_ready](Impl&, const fs::path& destination) { on_ready(destination.string()); };
        self->run_download(std::move(job));
    };
    p->action_host = host;

    WebAppDialogs::install();
    p->dialog_listener = WebAppDialogs::add_listener([weak](const std::string& method, const json& data) {
        if (auto self = weak.lock(); self && !self->detached.load())
            self->notify(method, data);
    });
}

WebAppBridge::~WebAppBridge()
{
    // Runs on the GUI thread. Any in-flight Http callback may still hold Impl alive through its
    // weak pointer, but from here on it must not touch the panel.
    if (p) {
        // Exports live in the cache directory until the page discards them. A page that closed
        // mid-publish never will, so the tab going away is the backstop.
        p->discard_all_exports();
        p->detached.store(true);
        WebAppDialogs::remove_listener(p->dialog_listener);
        if (p->action_host)
            *p->action_host->alive = false; // Actions: running jobs report to nobody now
    }
}

// What is on the plates, cheaply: every object, volume mesh size and transform, the current plate
// and the selected presets -- all a hand-off carries. Equal fingerprints mean the page already has
// this plate.
static size_t plate_fingerprint()
{
    Plater* plater = wxGetApp().plater();
    size_t  seed   = 0x5eed;
    auto    mix    = [&seed](size_t v) { seed ^= v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2); };
    auto    mix_m  = [&mix](const Transform3d& t) {
        const double* d = t.matrix().data();
        for (int i = 0; i < 16; ++i)
            mix(std::hash<double>{}(d[i]));
    };
    for (const ModelObject* o : plater->model().objects) {
        mix(o->id().id);
        for (const ModelVolume* v : o->volumes) {
            mix(v->id().id);
            mix(v->mesh().facets_count());
            mix_m(v->get_matrix());
        }
        for (const ModelInstance* inst : o->instances) {
            mix_m(inst->get_matrix());
            mix(size_t(inst->printable));
        }
    }
    PartPlateList& plates = plater->get_partplate_list();
    mix(size_t(plates.get_curr_plate_index()));
    mix(size_t(plates.get_plate_count()));
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    mix(std::hash<std::string>{}(bundle.printers.get_selected_preset_name()));
    mix(std::hash<std::string>{}(bundle.prints.get_selected_preset_name()));
    if (!bundle.filament_presets.empty())
        mix(std::hash<std::string>{}(bundle.filament_presets.front()));
    return seed == 0 ? 1 : seed;
}

// UNLAYERED_MOD (2026-10-25, hand-off freeze) -- everything a hand-off 3MF carries, as an opaque
// token: plate_fingerprint() plus the full print/printer/filament configuration (the project
// settings the 3MF embeds, unsaved edits included), each object's and part's own settings, layer
// ranges, painting, variable layer height and cut planes. Equal tokens mean the 3MF the page
// already holds is still current, so slicer.requestHandoff {ifChangedSince} can skip the export:
// a side-panel page re-exported the whole plate before every message (0.3-0.6 s on the GUI thread
// each, then several MB through readExportChunk) even when nothing had changed. A few ms here.
static std::string handoff_state_token()
{
    size_t seed = plate_fingerprint();
    auto   mix  = [&seed](size_t v) { seed ^= v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2); };
    auto   mix_s = [&mix](const std::string& s) { mix(std::hash<std::string>{}(s)); };
    auto   mix_cfg = [&mix_s](const ConfigBase& cfg) {
        for (const std::string& key : cfg.keys()) {
            mix_s(key);
            mix_s(cfg.opt_serialize(key));
        }
    };
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    mix_cfg(bundle.full_config());
    for (const std::string& name : bundle.filament_presets)
        mix_s(name);
    Plater* plater = wxGetApp().plater();
    for (const ModelObject* o : plater->model().objects) {
        mix(size_t(static_cast<const ModelConfig&>(o->config).timestamp()));
        mix(size_t(o->layer_height_profile.timestamp()));
        mix(o->layer_config_ranges.size());
        for (const auto& range : o->layer_config_ranges) {
            mix(std::hash<double>{}(range.first.first));
            mix(std::hash<double>{}(range.first.second));
            mix(size_t(range.second.timestamp()));
        }
        mix(size_t(o->cut_planes.enabled) | (size_t(o->cut_planes.user_ordered) << 1));
        for (const CutPlane& plane : o->cut_planes.planes) {
            for (int i = 0; i < 3; ++i) {
                mix(std::hash<double>{}(plane.point(i)));
                mix(std::hash<double>{}(plane.normal(i)));
                mix(std::hash<double>{}(plane.build_normal(i)));
            }
            mix(size_t(plane.has_build_normal) | (size_t(plane.enabled) << 1));
        }
        mix_s(o->name);
        for (const ModelVolume* v : o->volumes) {
            mix(size_t(static_cast<const ModelConfig&>(v->config).timestamp()));
            mix(size_t(v->type()));
            mix(size_t(v->supported_facets.timestamp()));
            mix(size_t(v->seam_facets.timestamp()));
            mix(size_t(v->mmu_segmentation_facets.timestamp()));
            mix(size_t(v->fuzzy_skin_facets.timestamp()));
            mix(std::hash<const void*>{}(&v->mesh()));
            mix_s(v->name);
        }
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(seed));
    return buf;
}

// Opening the generic web-app tab brings the current plate along: the page shows what is
// on the bed without an import step. EVERY time the tab opens -- the page re-takes it even when the plate looks
// unchanged, since the fingerprint misses preset and placement edits. Best effort: a plate that
// cannot be exported now simply is not handed over (the page keeps what it had). The agent
// sidebar is NOT handed the plate this way: it is shown most of the time, and its page pulls the
// plate itself (slicer.requestHandoff) only when a message is about to go out.
void WebAppBridge::on_tab_shown(bool shown)
{
    if (!shown || !p)
        return;
    if (p->owner.slot().id != WebAppSlot::generic().id)
        return;
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->model().objects.empty() || plater->only_gcode_mode())
        return;
    hand_off_plate(-1, "show");
}

void WebAppBridge::notify(const std::string& method, const json& data)
{
    if (p)
        p->notify(method, data);
}

void WebAppBridge::notify_capabilities()
{
    if (p)
        p->notify("slicer.capabilitiesChanged", {{"capabilities", p->capabilities()}});
}

void WebAppBridge::on_document_loaded(const wxString& /*url*/)
{
    // A fresh token per document: it cannot be replayed after a navigation or reload, and a
    // cross-origin frame that can reach window.wx cannot read the main frame's JS to steal it.
    if (!p->owner.origin_is_allowed()) {
        p->session_token.clear();
        return;
    }
    p->session_token = make_session_token();
    p->inject_bridge_script();
}

// ---------------------------------------------------------------------------------------------
// Printers: export, print and control (WEB_APP_BRIDGE.md 4.6). A page with this tab's printer
// permission may export, print and control printers. Every method needs this tab's printer permission; every
// export, print and control waits on a native window the page cannot press (pause and stop
// excepted, they are the safety actions); the page sees opaque printer ids, never an address,
// access code or API key.
// ---------------------------------------------------------------------------------------------

namespace {

std::string printer_state_of_rrf(const std::string& status)
{
    if (status == "idle" || status == "off")
        return "idle";
    if (status == "processing" || status == "busy" || status == "simulating" || status == "resuming" || status == "pausing")
        return "printing";
    if (status == "paused")
        return "paused";
    return status.empty() ? "unknown" : status;
}

const char* host_type_name(PrintHostType t)
{
    switch (t) {
    case htDuet: return "duet";
    case htUnlayered: return "duet";
    case htOctoPrint: return "octoprint";
    case htMoonraker: return "moonraker";
    case htPrusaLink: return "prusalink";
    case htPrusaConnect: return "prusaconnect";
    case htRepetier: return "repetier";
    default: return "other";
    }
}

bool is_rrf_host(PrintHostType t) { return t == htDuet || t == htUnlayered; }

// The selected printer preset's print host, when it names one.
const DynamicPrintConfig* host_printer_config()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return nullptr;
    const DynamicPrintConfig& cfg = bundle->printers.get_edited_preset().config;
    if (!cfg.has("print_host") || cfg.opt_string("print_host").empty())
        return nullptr;
    return &cfg;
}

PrintHostType host_type_of(const DynamicPrintConfig& cfg)
{
    const auto* opt = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return opt != nullptr ? opt->value : htOctoPrint;
}


} // namespace

std::string WebAppBridge::Impl::printer_id_for(const std::string& key)
{
    if (auto it = printer_id_of.find(key); it != printer_id_of.end())
        return it->second;
    std::random_device rd;
    char               buf[32];
    snprintf(buf, sizeof(buf), "pr-%04x%04x%02u", unsigned(rd() & 0xffff), unsigned(rd() & 0xffff), unsigned(printer_id_of.size() % 100));
    printer_id_of[key]  = buf;
    printer_key_of[buf] = key;
    return buf;
}

// Resolves params.printer to "bbl:<dev_id>" or "host", or answers the request and returns false.
bool WebAppBridge::Impl::resolve_printer(const std::string& id, const char* method, const json& params, std::string& key)
{
    const std::string pid = json_string(params, "printer");
    auto              it  = printer_key_of.find(pid);
    if (pid.empty() || it == printer_key_of.end()) {
        send_response(id, method, WAB_NOT_FOUND, "unknown printer; call slicer.listPrinters for current ids");
        return false;
    }
    key = it->second;
    return true;
}

bool WebAppBridge::Impl::printer_gate(const std::string& id, const char* method)
{
    if (!owner.allows_printer()) {
        send_response(id, method, WAB_CAPABILITY_DISABLED,
                      "printer access is off for this tab (Preferences > Developer: the printer access setting)");
        return false;
    }
    return true;
}

void WebAppBridge::Impl::handle_list_printers(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.listPrinters";
    if (!printer_gate(id, METHOD))
        return;
    json list = json::array();
    if (DeviceManager* dev = wxGetApp().getDeviceManager(); dev != nullptr) {
        MachineObject* selected = dev->get_selected_machine();
        for (auto& [dev_id, obj] : dev->get_my_machine_list()) {
            if (obj == nullptr)
                continue;
            list.push_back({{"id", printer_id_for("bbl:" + dev_id)},
                            {"name", obj->get_dev_name()},
                            {"kind", "bambu"},
                            {"model", std::string(obj->get_printer_type_display_str().ToUTF8().data())},
                            {"online", obj->is_online()},
                            {"state", WebAppPrinters::bambu_state(obj)},
                            {"selected", obj == selected},
                            {"controllable", true},
                            {"statusReadable", true}});
        }
    }
    if (const DynamicPrintConfig* cfg = host_printer_config(); cfg != nullptr) {
        const PrintHostType t = host_type_of(*cfg);
        list.push_back({{"id", printer_id_for("host")},
                        {"name", wxGetApp().preset_bundle->printers.get_edited_preset().name},
                        {"kind", "host"},
                        {"hostType", host_type_name(t)},
                        {"model", cfg->has("printer_model") ? cfg->opt_string("printer_model") : std::string()},
                        {"online", nullptr},
                        {"state", "unknown"},
                        {"selected", true},
                        {"controllable", is_rrf_host(t)},
                        {"statusReadable", is_rrf_host(t)}});
    }
    send_response(id, METHOD, WAB_OK, "success", {{"printers", list}});
}

void WebAppBridge::Impl::handle_printer_status(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.printerStatus";
    std::string        key;
    if (!printer_gate(id, METHOD) || !resolve_printer(id, METHOD, params, key))
        return;

    if (boost::starts_with(key, "bbl:")) {
        DeviceManager* dev = wxGetApp().getDeviceManager();
        MachineObject* obj = dev != nullptr ? dev->get_my_machine(key.substr(4)) : nullptr;
        if (obj == nullptr) {
            send_response(id, METHOD, WAB_NOT_FOUND, "that printer is no longer known to the slicer");
            return;
        }
        // Everything the Device tab shows: job, temperatures and limits, fans, light, AMS, HMS.
        send_response(id, METHOD, WAB_OK, "success", WebAppPrinters::bambu_status(obj));
        return;
    }

    const DynamicPrintConfig* cfg = host_printer_config();
    if (cfg == nullptr || !is_rrf_host(host_type_of(*cfg))) {
        send_response(id, METHOD, WAB_UNSUPPORTED_TYPE, "this print host does not report its status to the slicer");
        return;
    }
    // Network: off the GUI thread. send_response is thread-safe.
    std::weak_ptr<Impl> weak     = weak_from_this();
    const std::string   host     = cfg->opt_string("print_host");
    const std::string   password = cfg->has("printhost_apikey") ? cfg->opt_string("printhost_apikey") : std::string();
    std::thread([weak, id, host, password]() {
        DuetRrfClient client(host, password);
        std::string   error;
        json          data = json::object();
        int           code = WAB_OK;
        if (client.connect(error) != DuetClient::ConnectResult::ok) {
            code = WAB_IO;
        } else {
            // State, progress, layers and temperatures from the object model.
            if (WebAppPrinters::rrf_status(client, data, error))
                data["state"] = printer_state_of_rrf(data.value("state", std::string()));
            else
                code = WAB_IO;
            client.disconnect();
        }
        if (auto self = weak.lock())
            self->send_response(id, "slicer.printerStatus", code, code == WAB_OK ? "success" : ("printer unreachable: " + error), data);
    }).detach();
}

void WebAppBridge::Impl::handle_export_gcode(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.exportGcode";
    Tab*               tab    = nullptr;
    std::string        message;
    if (!printer_gate(id, METHOD))
        return;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    // plate: "current" (default) | "all"; format: "gcode" (default) | "3mf" (a sliced .gcode.3mf,
    // the "Export plate sliced file" / "Export all sliced file" entries). All plates exist only as 3MF.
    const std::string which  = params.contains("plate") ? json_string(params, "plate") : std::string("current");
    const std::string format = params.contains("format") ? json_string(params, "format") : std::string("gcode");
    if (which != "current" && which != "all") {
        send_response(id, METHOD, WAB_BAD_PARAMS, "plate is \"current\" or \"all\"", {{"path", "/params/plate"}});
        return;
    }
    if (format != "gcode" && format != "3mf") {
        send_response(id, METHOD, WAB_BAD_PARAMS, "format is \"gcode\" or \"3mf\"", {{"path", "/params/format"}});
        return;
    }
    if (which == "all" && format != "3mf") {
        send_response(id, METHOD, WAB_BAD_PARAMS, "every plate is exported as one sliced 3MF: pass format \"3mf\"", {{"path", "/params/format"}});
        return;
    }
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    PartPlate*     plate  = plates.get_curr_plate();
    if (plate == nullptr || !plate->is_slice_result_valid()) {
        send_response(id, METHOD, WAB_BAD_PARAMS, "no valid slice on the current plate; call slicer.slice first");
        return;
    }
    if (refuse_nothing_printable(id, METHOD, which == "all" ? -1 : plates.get_curr_plate_index()))
        return;
    if (which == "all") {
        json unsliced = json::array();
        for (int i = 0; i < plates.get_plate_count(); ++i)
            if (plates.get_plate(i)->has_printable_instances() && !plates.get_plate(i)->is_slice_result_valid())
                unsliced.push_back(i);
        if (!unsliced.empty()) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "not every plate is sliced; slice them first (slice.all)", {{"unsliced", unsliced}});
            return;
        }
    }
    send_response(id, METHOD, WAB_OK, "accepted", {{"accepted", true}});
    std::weak_ptr<Impl> weak = weak_from_this();
    wxGetApp().CallAfter([weak, id, which, format]() {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return;
        const unsigned before = plater->gcode_exports_started();
        // The slicer's own Save dialog: the person's confirmation.
        WebAppDialogs::HumanOnlyScope persons_save;
        if (format == "3mf")
            plater->export_gcode_3mf(which == "all");
        else
            plater->export_gcode(false);
        const bool saved = plater->gcode_exports_started() > before;
        if (auto self = weak.lock()) {
            json data = {{"ok", saved}, {"format", format}, {"plate", which}};
            std::string reason, why;
            if (saved)
                data["path"] = plater->last_gcode_export_path();
            else if (explain_nothing_printable(assess_plate_printability(plater->get_partplate_list(), plater->model()),
                                               which == "all" ? -1 : plater->get_partplate_list().get_curr_plate_index(), reason, why)) {
                // The plate changed after the request was accepted; the export refused it.
                data["reason"]  = "nothing_printable";
                data["cause"]   = reason;
                data["message"] = why;
            } else
                data["cancelled"] = true;
            self->send_event(id, "slicer.exportFinished", data);
        }
    });
}

// `plate`: "current" (default) or "all", ready for print as MainFrame::get_enable_print_status
// requires before it enables Print plate / Print all / Send / Send all.
bool WebAppBridge::Impl::check_print_plate(const std::string& id, const char* method, const json& params, bool& all)
{
    const std::string plate = json_string(params, "plate");
    if (!plate.empty() && plate != "current" && plate != "all") {
        send_response(id, method, WAB_BAD_PARAMS, "plate must be \"current\" or \"all\"");
        return false;
    }
    all                   = plate == "all";
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    if (refuse_nothing_printable(id, method, all ? -1 : plates.get_curr_plate_index()))
        return false;
    if (all) {
        if (!plates.is_all_slice_results_ready_for_print()) {
            send_response(id, method, WAB_BAD_PARAMS, "not every plate has a slice ready to print; slice all plates first (slicer.slice)");
            return false;
        }
        return true;
    }
    PartPlate* plate_ptr = plates.get_curr_plate();
    if (plate_ptr == nullptr || !plate_ptr->is_slice_result_ready_for_print()) {
        send_response(id, method, WAB_BAD_PARAMS, "no slice ready to print on the current plate; call slicer.slice first");
        return false;
    }
    if (GLCanvas3D* preview = wxGetApp().plater()->get_preview_canvas3D(); preview != nullptr && preview->is_all_plates_selected()) {
        send_response(id, method, WAB_ACTION_DISABLED, "the preview shows all plates; select one plate or use plate \"all\"",
                      {{"reason", "allPlatesShown"}});
        return false;
    }
    return true;
}

namespace {

// slicer.print `options` (Bambu): what SelectMachineDialog::set_automation_options takes.
std::string check_print_options(const json& options, json& out)
{
    out = json::object();
    if (!options.is_object())
        return "options must be an object";
    for (auto it = options.begin(); it != options.end(); ++it) {
        const std::string& k = it.key();
        if (k == "timelapse" || k == "bedLeveling" || k == "flowCalibration" || k == "nozzleOffsetCalibration" || k == "sharedPaProfile") {
            std::string v = it->is_boolean() ? (it->get<bool>() ? "on" : "off") : it->is_string() ? it->get<std::string>() : std::string();
            if (v != "on" && v != "off" && v != "auto")
                return k + " must be \"on\", \"off\" or \"auto\"";
            out[k] = v;
        } else if (k == "jobName") {
            std::string name;
            if (!it->is_string())
                return "jobName must be a string";
            if (std::string why = Actions::PrinterRules::job_name_refusal(it->get<std::string>(), name); !why.empty())
                return why;
            out[k] = name;
        } else if (k == "timelapseStorage") {
            if (!it->is_string() || (it->get<std::string>() != "internal" && it->get<std::string>() != "external"))
                return "timelapseStorage must be \"internal\" or \"external\"";
            out[k] = *it;
        } else if (k == "extChangeAssist") {
            if (!it->is_boolean())
                return "extChangeAssist must be true or false";
            out[k] = *it;
        } else {
            return "unknown print option \"" + k + "\"";
        }
    }
    return {};
}

// slicer.print `upload` (print hosts): what PrintHostSendDialog::set_automation_preset takes.
std::string check_upload_options(const json& upload, const DynamicPrintConfig& cfg, json& out)
{
    out = json::object();
    if (!upload.is_object())
        return "upload must be an object";
    for (auto it = upload.begin(); it != upload.end(); ++it) {
        const std::string& k = it.key();
        if (k != "action" && k != "path" && k != "group" && k != "storage")
            return "unknown upload option \"" + k + "\"";
        if (!it->is_string() || it->get<std::string>().size() > 200)
            return k + " must be a string of at most 200 characters";
    }
    const std::string action = upload.value("action", std::string());
    if (!action.empty() && action != "upload" && action != "print")
        return "upload.action must be \"upload\" or \"print\"";
    if (action == "print") {
        DynamicPrintConfig               copy = cfg;
        std::unique_ptr<PrintHost>       host(PrintHost::get_print_host(&copy));
        if (!host || !host->get_post_upload_actions().has(PrintHostPostUploadAction::StartPrint))
            return "this print host cannot start a print after the upload; use upload.action \"upload\"";
    }
    const std::string path = upload.value("path", std::string());
    if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos)
        return "upload.path is a folder on the host, with / between folders and no ..";
    out = upload;
    return {};
}

} // namespace

void WebAppBridge::Impl::handle_print(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.print";
    std::string        key;
    Tab*               tab = nullptr;
    std::string        message;
    bool               all = false;
    if (!printer_gate(id, METHOD) || !resolve_printer(id, METHOD, params, key))
        return;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (print_in_progress) {
        send_response(id, METHOD, WAB_BUSY, "a print dialog from this tab is already open");
        return;
    }
    if (!check_print_plate(id, METHOD, params, all))
        return;
    const bool bbl_flow = wxGetApp().preset_bundle->use_bbl_network() || wxGetApp().app_config->get_bool("use_printer_agents");
    json       dialog_options = json::object();
    if (boost::starts_with(key, "bbl:")) {
        if (!bbl_flow) {
            send_response(id, METHOD, WAB_BAD_PARAMS,
                          "the selected printer preset does not print to Bambu printers; select a Bambu printer preset first");
            return;
        }
        DeviceManager* dev = wxGetApp().getDeviceManager();
        MachineObject* obj = dev != nullptr ? dev->get_my_machine(key.substr(4)) : nullptr;
        if (obj == nullptr) {
            send_response(id, METHOD, WAB_NOT_FOUND, "that printer is no longer known to the slicer");
            return;
        }
        // SelectMachineDialog shows "not supported" for print all on these printers.
        if (all && !obj->GetConfig()->SupportPrintAllPlates()) {
            send_response(id, METHOD, WAB_UNSUPPORTED_TYPE, "this printer cannot print all plates in one job; print them one at a time");
            return;
        }
        if (params.contains("upload")) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "upload applies to print hosts; Bambu printers take options");
            return;
        }
        if (params.contains("options")) {
            json opts;
            if (std::string why = check_print_options(params["options"], opts); !why.empty()) {
                send_response(id, METHOD, WAB_BAD_PARAMS, why);
                return;
            }
            dialog_options["bambu"] = opts;
        }
        dev->set_selected_machine(key.substr(4));
    } else {
        const DynamicPrintConfig* cfg = host_printer_config();
        if (bbl_flow || cfg == nullptr) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "the selected printer preset has no print host");
            return;
        }
        // MainFrame offers Print all to a print host only for SimplyPrint.
        if (all && host_type_of(*cfg) != htSimplyPrint) {
            send_response(id, METHOD, WAB_UNSUPPORTED_TYPE, "this print host takes one plate at a time; print the plates one by one");
            return;
        }
        if (params.contains("options")) {
            send_response(id, METHOD, WAB_BAD_PARAMS, "options apply to Bambu printers; print hosts take upload");
            return;
        }
        if (params.contains("upload")) {
            json upload;
            if (std::string why = check_upload_options(params["upload"], *cfg, upload); !why.empty()) {
                send_response(id, METHOD, WAB_BAD_PARAMS, why);
                return;
            }
            dialog_options["upload"] = upload;
        }
    }

    print_in_progress = true;
    send_response(id, METHOD, WAB_OK, "accepted", {{"accepted", true}});
    std::weak_ptr<Impl> weak    = weak_from_this();
    const std::string   printer = json_string(params, "printer");
    wxGetApp().CallAfter([weak, id, printer, all, dialog_options]() {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return;
        const unsigned before = plater->print_sends_started();
        // The Print plate / Print all button itself (MainFrame): the slicer's own print / send
        // dialog for the selected printer, modal, so this returns once the person has sent or
        // closed it. The page's options preset that dialog, for this print only.
        plater->apply_background_progress();
        plater->set_next_print_dialog_options(dialog_options);
        SimpleEvent evt(all ? EVT_GLTOOLBAR_PRINT_ALL : EVT_GLTOOLBAR_PRINT_PLATE);
        {
            WebAppDialogs::HumanOnlyScope persons_print; // the print dialog is the person's confirmation
            plater->GetEventHandler()->ProcessEvent(evt);
        }
        plater->set_next_print_dialog_options(json::object());
        const bool sent = plater->print_sends_started() > before;
        if (auto self = weak.lock()) {
            self->print_in_progress = false;
            json data               = {{"printer", printer}, {"ok", sent}, {"plate", all ? "all" : "current"}};
            if (!sent)
                data["cancelled"] = true;
            self->send_event(id, "slicer.printSent", data);
        }
    });
}

// The Print button itself, as it is set (requirement: a page may press the
// Print button, since that sends nothing by itself, it only opens a window). Unlike slicer.print
// it names no printer and presets nothing: the button's own dialog opens for the printer preset
// the person has selected (print / send / save), and nothing leaves until they confirm there. So
// it needs only the settings permission, not the printer one. Answers once the window opened,
// then slicer.printDialogClosed when it closed: sent true = a print or upload job started.
void WebAppBridge::Impl::handle_press_print(const std::string& id, const json& /*params*/)
{
    static const char* METHOD = "slicer.pressPrint";
    Tab*               tab = nullptr;
    std::string        message;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (print_in_progress) {
        send_response(id, METHOD, WAB_BUSY, "a print dialog from this tab is already open");
        return;
    }
    MainFrame* mainframe = wxGetApp().mainframe;
    const std::string button = mainframe->print_button_label().ToUTF8().data();
    if (!mainframe->can_press_print()) {
        send_response(id, METHOD, WAB_ACTION_DISABLED,
                      "the " + button + " button is disabled: slice the plate first (slicer.slice), and fix any slicing error",
                      {{"reason", "not_sliced"}, {"button", button}});
        return;
    }

    print_in_progress = true;
    send_response(id, METHOD, WAB_OK, "accepted", {{"accepted", true}, {"button", button}});
    std::weak_ptr<Impl> weak = weak_from_this();
    wxGetApp().CallAfter([weak, id, button]() {
        Plater*    plater    = wxGetApp().plater();
        MainFrame* mainframe = wxGetApp().mainframe;
        unsigned   before    = plater != nullptr ? plater->print_sends_started() : 0;
        if (plater != nullptr && mainframe != nullptr)
            mainframe->press_print_button(/*wait=*/true);
        const bool sent = plater != nullptr && plater->print_sends_started() > before;
        if (auto self = weak.lock()) {
            self->print_in_progress = false;
            self->send_event(id, "slicer.printDialogClosed", {{"button", button}, {"sent", sent}});
        }
    });
}

// Send / Send all (MainFrame, Bambu printers and printer agents): the slicer's own Send dialog
// (SendToPrinterDialog), which saves the sliced file to the printer's storage without printing.
void WebAppBridge::Impl::handle_send_to_printer(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.sendToPrinter";
    std::string        key;
    Tab*               tab = nullptr;
    std::string        message;
    bool               all = false;
    if (!printer_gate(id, METHOD) || !resolve_printer(id, METHOD, params, key))
        return;
    if (const int code = preflight(true, Preset::TYPE_INVALID, tab, message, /*require_settings=*/false); code != WAB_OK) {
        send_response(id, METHOD, code, message);
        return;
    }
    if (print_in_progress) {
        send_response(id, METHOD, WAB_BUSY, "a print dialog from this tab is already open");
        return;
    }
    if (!check_print_plate(id, METHOD, params, all))
        return;
    const bool bbl_flow = wxGetApp().preset_bundle->use_bbl_network() || wxGetApp().app_config->get_bool("use_printer_agents");
    if (!boost::starts_with(key, "bbl:") || !bbl_flow) {
        send_response(id, METHOD, WAB_UNSUPPORTED_TYPE, "only Bambu printers (or printer agents) take a file into their storage from the slicer");
        return;
    }
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (dev == nullptr || dev->get_my_machine(key.substr(4)) == nullptr) {
        send_response(id, METHOD, WAB_NOT_FOUND, "that printer is no longer known to the slicer");
        return;
    }
    dev->set_selected_machine(key.substr(4));

    print_in_progress = true;
    send_response(id, METHOD, WAB_OK, "accepted", {{"accepted", true}});
    std::weak_ptr<Impl> weak    = weak_from_this();
    const std::string   printer = json_string(params, "printer");
    wxGetApp().CallAfter([weak, id, printer, all]() {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return;
        const unsigned before = plater->send_jobs_finished();
        // Modal; it closes itself when the send finishes (Plater::send_job_finished).
        SimpleEvent evt(all ? EVT_GLTOOLBAR_SEND_TO_PRINTER_ALL : EVT_GLTOOLBAR_SEND_TO_PRINTER);
        {
            WebAppDialogs::HumanOnlyScope persons_send; // the Send dialog is the person's confirmation
            plater->GetEventHandler()->ProcessEvent(evt);
        }
        const bool sent = plater->send_jobs_finished() > before;
        if (auto self = weak.lock()) {
            self->print_in_progress = false;
            json data               = {{"printer", printer}, {"ok", sent}, {"plate", all ? "all" : "current"}};
            if (!sent)
                data["cancelled"] = true;
            self->send_event(id, "slicer.sendFinished", data);
        }
    });
}

// The Device tab's printer list (MonitorPanel::on_select_printer): the printer the Device tab,
// AMS sync and the Print dialog follow. A LAN printer connects when selected.
void WebAppBridge::Impl::handle_select_printer(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.selectPrinter";
    std::string        key;
    if (!printer_gate(id, METHOD) || !resolve_printer(id, METHOD, params, key))
        return;
    if (!boost::starts_with(key, "bbl:")) {
        send_response(id, METHOD, WAB_UNSUPPORTED_TYPE, "a print host is selected with its printer preset (slicer.selectPreset)");
        return;
    }
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (dev == nullptr || dev->get_my_machine(key.substr(4)) == nullptr) {
        send_response(id, METHOD, WAB_NOT_FOUND, "that printer is no longer known to the slicer");
        return;
    }
    const bool changed = dev->get_selected_machine() == nullptr || dev->get_selected_machine()->get_dev_id() != key.substr(4);
    if (changed)
        dev->set_selected_machine(key.substr(4));
    send_response(id, METHOD, WAB_OK, "success", {{"printer", json_string(params, "printer")}, {"selected", true}, {"changed", changed}});
}

// The print-host upload queue (MainFrame, Ctrl+J): what was uploaded this session, how far it got
// and why it failed. No host addresses.
void WebAppBridge::Impl::handle_print_host_jobs(const std::string& id, const json& /*params*/)
{
    static const char* METHOD = "slicer.printHostJobs";
    if (!printer_gate(id, METHOD))
        return;
    PrintHostQueueDialog* dlg = wxGetApp().mainframe ? wxGetApp().mainframe->printhost_queue_dlg() : nullptr;
    send_response(id, METHOD, WAB_OK, "success", {{"jobs", dlg ? dlg->jobs_json() : json::array()}});
}

// The queue window's Cancel selected, after the person confirms.
void WebAppBridge::Impl::handle_cancel_print_host_job(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.cancelPrintHostJob";
    if (!printer_gate(id, METHOD))
        return;
    const int job = json_int(params, "id", -1);
    PrintHostQueueDialog* dlg = wxGetApp().mainframe ? wxGetApp().mainframe->printhost_queue_dlg() : nullptr;
    json jobs = dlg ? dlg->jobs_json() : json::array();
    if (job < 0 || job >= int(jobs.size())) {
        send_response(id, METHOD, WAB_NOT_FOUND, "no upload with that id; read slicer.printHostJobs");
        return;
    }
    const std::string state = jobs[size_t(job)].value("state", std::string());
    if (state != "queued" && state != "uploading") {
        send_response(id, METHOD, WAB_ACTION_DISABLED, "that upload has already ended (" + state + ")", {{"reason", "ended"}});
        return;
    }
    send_response(id, METHOD, WAB_OK, "accepted", {{"accepted", true}});
    std::weak_ptr<Impl> weak = weak_from_this();
    const std::string   file = jobs[size_t(job)].value("file", std::string());
    wxGetApp().CallAfter([weak, id, job, file]() {
        Actions::ConfirmRequest request;
        request.title        = _u8L("Cancel upload");
        request.message      = _u8L("A page in the slicer wants to cancel an upload to your printer.");
        request.details      = {file};
        request.accept_label = _u8L("Cancel upload");
        const bool accepted  = Actions::native_confirm(request);
        PrintHostQueueDialog* dlg = wxGetApp().mainframe ? wxGetApp().mainframe->printhost_queue_dlg() : nullptr;
        const bool            ok  = accepted && dlg != nullptr && dlg->cancel_job(job);
        if (auto self = weak.lock()) {
            json data = {{"id", job}, {"ok", ok}};
            if (!accepted)
                data["cancelled"] = true;
            else if (!ok)
                data["error"] = "the upload had already ended";
            self->send_event(id, "slicer.printHostJobCancelled", data);
        }
    });
}

void WebAppBridge::Impl::handle_printer_control(const std::string& id, const json& params)
{
    static const char* METHOD = "slicer.printerControl";
    std::string        key;
    if (!printer_gate(id, METHOD) || !resolve_printer(id, METHOD, params, key))
        return;
    const std::string action  = json_string(params, "action");
    const std::string printer = json_string(params, "printer");
    const bool        bambu   = boost::starts_with(key, "bbl:");

    MachineObject*            obj = nullptr;
    const DynamicPrintConfig* cfg = nullptr;
    if (bambu) {
        DeviceManager* dev = wxGetApp().getDeviceManager();
        obj                = dev != nullptr ? dev->get_my_machine(key.substr(4)) : nullptr;
        if (obj == nullptr) {
            send_response(id, METHOD, WAB_NOT_FOUND, "that printer is no longer known to the slicer");
            return;
        }
    } else {
        cfg = host_printer_config();
        if (cfg == nullptr || !is_rrf_host(host_type_of(*cfg))) {
            send_response(id, METHOD, WAB_UNSUPPORTED_TYPE, "this print host cannot be controlled from the slicer, only printed to");
            return;
        }
    }

    // Checked and described before anything is shown (WebAppPrinters.cpp): the same states the
    // Device tab refuses in are refused here, with the reason.
    const WebAppPrinters::ControlPlan plan = bambu ? WebAppPrinters::plan_bambu_control(obj, action, params) :
                                                     WebAppPrinters::plan_rrf_control(action, params);
    if (plan.code != WAB_OK) {
        json data = json::object();
        if (!plan.reason.empty())
            data["reason"] = plan.reason;
        send_response(id, METHOD, plan.code, plan.message, data);
        return;
    }
    send_response(id, METHOD, WAB_OK, "accepted", {{"accepted", true}});

    const std::string   host     = cfg != nullptr ? cfg->opt_string("print_host") : std::string();
    const std::string   password = cfg != nullptr && cfg->has("printhost_apikey") ? cfg->opt_string("printhost_apikey") : std::string();
    const std::string   dev_id   = bambu ? key.substr(4) : std::string();
    std::weak_ptr<Impl> weak     = weak_from_this();
    wxGetApp().CallAfter([weak, id, printer, action, plan, bambu, dev_id, host, password, params]() {
        auto done = [weak, id, printer, action](bool ok, bool cancelled, const std::string& command, const std::string& error) {
            if (auto self = weak.lock()) {
                json data = {{"printer", printer}, {"action", action}, {"ok", ok}};
                if (cancelled)
                    data["cancelled"] = true;
                if (ok)
                    data["command"] = command;
                if (!error.empty())
                    data["error"] = error;
                self->send_event(id, "slicer.printerControlDone", data);
            }
        };
        if (!plan.immediate) {
            // The native confirmation: the action, the Device tab's own warning, and what is sent.
            Actions::ConfirmRequest request;
            request.title        = _u8L("Printer control");
            request.message      = _u8L("An automated request wants to control your printer.") + "\n\n" + plan.summary +
                                   (plan.warning.empty() ? std::string() : "\n\n" + plan.warning);
            request.accept_label = _u8L("Send");
            request.destructive  = false;
            boost::split(request.details, plan.command, boost::is_any_of("\n"));
            if (!Actions::native_confirm(request)) {
                done(false, true, std::string(), std::string());
                return;
            }
        }
        if (bambu) {
            DeviceManager* dev = wxGetApp().getDeviceManager();
            MachineObject* obj = dev != nullptr ? dev->get_my_machine(dev_id) : nullptr;
            // The printer may have moved on while the confirmation was open: check again.
            const WebAppPrinters::ControlPlan now = WebAppPrinters::plan_bambu_control(obj, action, params);
            if (now.code != WAB_OK || !now.run) {
                done(false, false, std::string(), obj == nullptr ? "that printer is no longer known to the slicer" : now.message);
                return;
            }
            const int rc = now.run(obj);
            done(rc == 0, false, now.command, rc == 0 ? std::string() : "the printer did not take the command (" + std::to_string(rc) + ")");
            return;
        }
        // RepRapFirmware over HTTP, off the GUI thread.
        std::thread([done, host, password, rrf = plan.rrf, command = plan.command]() {
            DuetRrfClient client(host, password);
            std::string   error;
            if (client.connect(error) != DuetClient::ConnectResult::ok) {
                done(false, false, std::string(), "printer unreachable: " + error);
                return;
            }
            bool ok = true;
            for (const std::string& line : rrf)
                if (!client.gcode(line, error)) {
                    ok = false;
                    break;
                }
            client.disconnect();
            done(ok, false, command, ok ? std::string() : error);
        }).detach();
    });
}

void WebAppBridge::on_script_message(wxWebViewEvent& evt)
{
    const wxString message = evt.GetString();
    if (message.empty())
        return;

    json root = json::parse(message.ToUTF8().data(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: ignoring unparseable script message";
        return;
    }

    const std::string id     = json_string(root, "id");
    std::string       method = json_string(root, "method");
    json params = root.contains("params") && root["params"].is_object() ? root["params"] : json::object();

    // Legacy aliases, matching the envelope PrinterWebViewHandler already accepts.
    if (method.empty()) {
        method = json_string(root, "command");
        if (params.empty() && root.contains("data") && root["data"].is_object())
            params = root["data"];
    }

    // Anything outside our namespace belongs to another consumer of the shared "wx" channel.
    if (method.rfind("slicer.", 0) != 0)
        return;

    if (!p->owner.origin_is_allowed()) {
        BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: rejecting '" << method << "' from untrusted origin '"
                                   << p->owner.current_origin().ToUTF8().data() << "'";
        p->send_response(id, method, WAB_FORBIDDEN_ORIGIN, "this origin may not use the slicer bridge");
        return;
    }

    if (p->session_token.empty() || json_string(root, "token") != p->session_token) {
        BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: rejecting '" << method << "' with a missing or stale token";
        p->send_response(id, method, WAB_BAD_TOKEN, "missing or stale session token");
        return;
    }

    // UNLAYERED_MOD (2026-10-25, hand-off freeze): a request that holds the GUI thread says so.
    // A 0.1.8 build froze for 44 s after a side-panel session with nothing in the log to say which bridge
    // call (if any) was running; handlers log nothing of their own. Declared first, so it is
    // destroyed last and times everything below, the scopes' own teardown included.
    struct SlowRequestLog
    {
        const std::string&                    method;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~SlowRequestLog()
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (ms >= 250)
                BOOST_LOG_TRIVIAL(warning) << "WebAppBridge: " << method << " held the GUI thread for " << ms << " ms";
        }
    } slow_request_log{method};
    // Nothing a page does starts a slicing-service (cloud) run, whatever the handler below ends up
    // calling (Plater::ServiceSliceBlock).
    Plater::ServiceSliceBlock no_service_runs;
    // A new/open project during this request is the bridge's (slicer.projectChanged source "bridge").
    Plater::BridgeRequestScope bridge_request;
    // A modal dialog this request opens is the page's to answer (slicer.answerDialog).
    WebAppDialogs::BridgeScope dialogs_from_request;
    // Actions: a request carrying params.turn records what it did to the undo stack (slicer.undoTurn).
    Actions::RequestScope action_request(*p->action_host, method, params);

    if (method == "slicer.getInfo") {
        p->handle_get_info(id);
    } else if (method == "slicer.openProject") {
        p->handle_open_project(id, params);
    } else if (method == "slicer.openProjectFromUrl") {
        p->handle_open_project_from_url(id, params);
    } else if (method == "slicer.selectPreset") {
        p->handle_select_preset(id, params);
    } else if (method == "slicer.listPresets") {
        p->handle_list_presets(id, params);
    } else if (method == "slicer.addFilamentSlot") {
        p->handle_add_filament_slot(id, params);
    } else if (method == "slicer.saveFilamentPreset") {
        p->handle_save_filament_preset(id, params);
    } else if (method == "slicer.listPrinterModels") {
        p->handle_list_printer_models(id, params);
    } else if (method == "slicer.addPrinter") {
        p->handle_add_printer(id, params);
    } else if (method == "slicer.getSettingsSchema") {
        p->handle_get_settings_schema(id, params);
    } else if (method == "slicer.getSettings") {
        p->handle_get_settings(id, params);
    } else if (method == "slicer.setSettings") {
        p->handle_set_settings(id, params);
    } else if (method == "slicer.getPlate") {
        p->handle_get_plate(id);
    } else if (method == "slicer.setCutPlanes") {
        p->handle_set_cut_planes(id, params);
    } else if (method == "slicer.setOrientation") {
        p->handle_set_orientation(id, params);
    } else if (method == "slicer.setObjectFilament") {
        p->handle_set_object_filament(id, params);
    } else if (method == "slicer.setScale") {
        p->handle_set_scale(id, params);
    } else if (method == "slicer.moveObject") {
        p->handle_move_object(id, params);
    } else if (method == "slicer.arrange") {
        p->handle_arrange(id, params);
    } else if (method == "slicer.setCopies") {
        p->handle_set_copies(id, params);
    } else if (method == "slicer.fillBed") {
        p->handle_fill_bed(id, params);
    } else if (method == "slicer.exportProject") {
        p->handle_export_project(id, params);
    } else if (method == "slicer.readExportChunk") {
        p->handle_read_export_chunk(id, params);
    } else if (method == "slicer.discardExport") {
        p->handle_discard_export(id, params);
    } else if (method == "slicer.exportThumbnail") {
        p->handle_export_thumbnail(id, params);
    } else if (method == "slicer.exportPrinterPreset") {
        p->handle_export_printer_preset(id, params);
    } else if (method == "slicer.installPrinterPreset") {
        p->handle_install_printer_preset(id, params);
    } else if (method == "slicer.takeHandoff") {
        p->handle_take_handoff(id);
    } else if (method == "slicer.requestHandoff") {
        p->handle_request_handoff(id, params);
    } else if (method == "slicer.slice") {
        p->handle_slice(id, params);
    } else if (method == "slicer.getSliceResult") {
        p->handle_get_slice_result(id, params);
    } else if (method == "slicer.getSliceModes") {
        p->handle_get_slice_modes(id);
    } else if (method == "slicer.setSliceMode") {
        p->handle_set_slice_mode(id, params);
    } else if (method == "slicer.listPrinters") {
        p->handle_list_printers(id, params);
    } else if (method == "slicer.printerStatus") {
        p->handle_printer_status(id, params);
    } else if (method == "slicer.exportGcode") {
        p->handle_export_gcode(id, params);
    } else if (method == "slicer.print") {
        p->handle_print(id, params);
    } else if (method == "slicer.pressPrint") {
        p->handle_press_print(id, params);
    } else if (method == "slicer.printerControl") {
        p->handle_printer_control(id, params);
    } else if (method == "slicer.sendToPrinter") {
        p->handle_send_to_printer(id, params);
    } else if (method == "slicer.selectPrinter") {
        p->handle_select_printer(id, params);
    } else if (method == "slicer.printHostJobs") {
        p->handle_print_host_jobs(id, params);
    } else if (method == "slicer.cancelPrintHostJob") {
        p->handle_cancel_print_host_job(id, params);
    } else if (method == "slicer.getDialogs" || method == "slicer.answerDialog") {
        // WebAppDialogs.hpp: read every open modal; answer only one this page's own request opened.
        if (!p->owner.allows_settings()) {
            p->send_response(id, method, WAB_CAPABILITY_DISABLED, "print settings access is not enabled for this tab");
        } else if (method == "slicer.getDialogs") {
            p->send_response(id, method, WAB_OK, "success", WebAppDialogs::list());
        } else {
            std::string message;
            json        data;
            const int   code = WebAppDialogs::answer(params, message, data);
            p->send_response(id, method, code, message, data);
        }
    } else if (dispatch_action_method(*p->action_host, id, method, params)) {
        // Actions: listActions / invokeAction / getSelection / select / undoTurn / typed catalog methods
    } else {
        p->send_response(id, method, WAB_UNKNOWN_METHOD, "unknown method");
    }
}

} // namespace GUI
} // namespace Slic3r
