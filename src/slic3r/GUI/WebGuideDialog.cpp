#include "WebGuideDialog.hpp"
#include "OtherSlicerImportDialog.hpp"
#include "AutoUpdater.hpp"
#include "PrinterUpdateOffer.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"
#include "libslic3r/libslic3r.h"
#include "ConfigWizard.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iostreams/detail/select.hpp>
#include <boost/log/trivial.hpp>
#include <string.h>
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r_version.h"

#include <wx/sizer.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <wx/wx.h>
#include <wx/display.h>
#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/wfstream.h>

#include <boost/cast.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>
#include <unordered_map>

#include "MainFrame.hpp"
#include <boost/dll.hpp>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/Utils/Http.hpp>
#include <libslic3r/miniz_extension.hpp>
#include <libslic3r/Utils.hpp>
#include "CreatePresetsDialog.hpp"
#include <wx/filesys.h>
#include <wx/filename.h>

using namespace nlohmann;

namespace Slic3r { namespace GUI {

json m_ProfileJson;

static wxString update_custom_filaments()
{
    json m_Res                                                                     = json::object();
    m_Res["command"]                                                               = "update_custom_filaments";
    m_Res["sequence_id"]                                                           = "2000";
    json                                               m_CustomFilaments           = json::array();
    PresetBundle *                                     preset_bundle               = wxGetApp().preset_bundle;
    std::map<std::string, std::vector<Preset const *>> temp_filament_id_to_presets = preset_bundle->filaments.get_filament_presets();

    std::vector<std::pair<std::string, std::string>>   need_sort;
    bool                                             need_delete_some_filament = false;
    for (std::pair<std::string, std::vector<Preset const *>> filament_id_to_presets : temp_filament_id_to_presets) {
        std::string filament_id = filament_id_to_presets.first;
        if (filament_id.empty()) continue;
        if (filament_id == "null") {
            need_delete_some_filament = true;
        }
        bool filament_with_base_id = false;
        bool not_need_show = false;
        std::string filament_name;
        for (const Preset *preset : filament_id_to_presets.second) {
            if (preset->is_system || preset->is_project_embedded) {
                not_need_show = true;
                break;
            }
            if (preset->inherits() != "") continue;
            if (!preset->base_id.empty()) filament_with_base_id = true;

            if (!not_need_show) {
                auto filament_vendor = dynamic_cast<ConfigOptionStrings *>(const_cast<Preset *>(preset)->config.option("filament_vendor", false));
                if (filament_vendor && filament_vendor->values.size() && filament_vendor->values[0] == "Generic") not_need_show = true;
            }

            if (filament_name.empty()) {
                std::string preset_name = preset->name;
                size_t      index_at    = preset_name.find(" @");
                if (std::string::npos != index_at) { preset_name = preset_name.substr(0, index_at); }
                filament_name = preset_name;
            }
        }
        if (not_need_show) continue;
        if (!filament_name.empty()) {
            if (filament_with_base_id) {
                need_sort.push_back(std::make_pair(into_u8(_L("[Action Required] ")) + filament_name, filament_id));
            } else {

                need_sort.push_back(std::make_pair(filament_name, filament_id));
            }
        }
    }
    std::sort(need_sort.begin(), need_sort.end(), [](const std::pair<std::string, std::string> &a, const std::pair<std::string, std::string> &b) { return a.first < b.first; });
    if (need_delete_some_filament) {
        need_sort.push_back(std::make_pair(into_u8(_L("[Action Required]")), "null"));
    }
    json temp_j;
    for (std::pair<std::string, std::string> &filament_name_to_id : need_sort) {
        temp_j["name"] = filament_name_to_id.first;
        temp_j["id"]   = filament_name_to_id.second;
        m_CustomFilaments.push_back(temp_j);
    }
    m_Res["data"]  = m_CustomFilaments;
    wxString strJS = wxString::Format("HandleStudio(%s)", wxString::FromUTF8(m_Res.dump(-1, ' ', false, json::error_handler_t::ignore)));
    return strJS;
}

GuideFrame::GuideFrame(GUI_App *pGUI, long style)
    : DPIDialog((wxWindow *) (pGUI->mainframe), wxID_ANY, SLIC3R_APP_FULL_NAME, wxDefaultPosition, wxDefaultSize, style),
	m_appconfig_new()
{
    SetBackgroundColour(*wxWHITE);
    // INI
    m_SectionName = "firstguide";
    PrivacyUse    = false;
    StealthMode   = false;
    InstallNetplugin = false;

    m_MainPtr = pGUI;
    // A first setup, or the wizard opened again from the menu (which counts as changing the choice).
    m_privacy = std::make_unique<PrivacyFlow>(false, pGUI->app_config->get("firstguide", "finish") == "1" ? "preferences" : "initial_setup");

    // set the frame icon
    wxBoxSizer *topsizer = new wxBoxSizer(wxVERTICAL);

    wxString TargetUrl = SetStartPage(BBL_WELCOME, false);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  set start page to welcome ");

    // Create the webview
    m_browser = WebView::CreateWebView(this, TargetUrl);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    m_browser->Hide();
    m_browser->SetSize(0, 0);

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    // Log backend information
    // wxLogMessage(wxWebView::GetBackendVersionInfo().ToString());
    // wxLogMessage("Backend: %s Version: %s",
    // m_browser->GetClassInfo()->GetClassName(),wxWebView::GetBackendVersionInfo().ToString());
    // wxLogMessage("User Agent: %s", m_browser->GetUserAgent());

    // Set a more sensible size for web browsing
    wxSize pSize = FromDIP(wxSize(820, 660));
    SetSize(pSize);

    int screenheight = wxSystemSettings::GetMetric(wxSYS_SCREEN_Y, NULL);
    int screenwidth  = wxSystemSettings::GetMetric(wxSYS_SCREEN_X, NULL);
    int MaxY         = (screenheight - pSize.y) > 0 ? (screenheight - pSize.y) / 2 : 0;
    wxPoint tmpPT((screenwidth - pSize.x) / 2, MaxY);
    Move(tmpPT);
#ifdef __WXMSW__
    this->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if ((m_page == BBL_FILAMENT_ONLY || m_page == BBL_MODELS_ONLY) && e.GetKeyCode() == WXK_ESCAPE) {
            if (this->IsModal())
                this->EndModal(wxID_CANCEL);
            else
                this->Close();
        }
        else
            e.Skip();
        });
#endif
    // Connect the webview events
    Bind(wxEVT_WEBVIEW_NAVIGATING, &GuideFrame::OnNavigationRequest, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATED, &GuideFrame::OnNavigationComplete, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &GuideFrame::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &GuideFrame::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NEWWINDOW, &GuideFrame::OnNewWindow, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_TITLE_CHANGED, &GuideFrame::OnTitleChanged, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_FULLSCREEN_CHANGED, &GuideFrame::OnFullScreenChanged, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &GuideFrame::OnScriptMessage, this, m_browser->GetId());

    // Connect the idle events
    // Bind(wxEVT_IDLE, &GuideFrame::OnIdle, this);
    // Bind(wxEVT_CLOSE_WINDOW, &GuideFrame::OnClose, this);

    // UI
    SetStartPage(BBL_REGION);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  finished");
    wxGetApp().UpdateDlgDarkUI(this);
}

void GuideFrame::DetectOtherSlicers()
{
    wxBusyCursor busy;
    m_other_slicers         = std::make_unique<OtherSlicersFound>(find_other_slicers());
    m_other_slicers_offered = m_other_slicers->total() > 0;
    m_bambu_printers_in     = into_u8(m_other_slicers->bambu_printers_in());
    BOOST_LOG_TRIVIAL(info) << "GuideFrame: other slicers " << (m_other_slicers_offered ? "have" : "have nothing") << " to import"
                            << (m_bambu_printers_in.empty() ? std::string() : ", Bambu Lab printers in " + m_bambu_printers_in);
}

// ---- Unlayered 0.1.8: "Update your Unlayered printer" (guide 21update) ----------------------------

namespace {

bool is_unlayered_host(const DynamicPrintConfig &cfg)
{
    const auto *host_type = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return host_type && host_type->value == htUnlayered;
}

// The Unlayered printer the step checks: the selected printer when it is one with an address,
// otherwise the first set-up Unlayered printer that has one. The same keys PrinterUpdateOffer reads.
struct UnlayeredPrinterTarget
{
    std::string name;
    std::string host;
    std::string password;
};

bool find_unlayered_printer(UnlayeredPrinterTarget &out)
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return false;
    auto take = [&out](const Preset &preset) {
        const DynamicPrintConfig &cfg = preset.config;
        if (!is_unlayered_host(cfg) || !cfg.has("print_host"))
            return false;
        const std::string host = cfg.opt_string("print_host");
        if (host.empty())
            return false;
        out.name     = preset.name;
        out.host     = host;
        out.password = cfg.has("printhost_apikey") ? cfg.opt_string("printhost_apikey") : std::string();
        return true;
    };
    if (take(bundle->printers.get_edited_preset()))
        return true;
    for (const Preset &preset : bundle->printers)
        if (preset.is_visible && take(preset))
            return true;
    return false;
}

wxString describe_printer_check(const PrinterUpdateOffer::CheckResult &r, const wxString &who, const std::string &host)
{
    const PrinterUpdateFacts &f     = r.facts;
    AppConfig                *config = wxGetApp().app_config;
    wxString                  text;
    switch (r.outcome) {
    case PrinterUpdateCheckOutcome::Unreachable:
        return wxString::Format(_L("Could not reach %s at %s (%s). Check that the printer is switched on and on the same network "
                                   "as this computer, then press Check for updates again. Or go on: Unlayered Slicer checks "
                                   "again once the printer answers."),
                                who, from_u8(host), from_u8(r.error));
    case PrinterUpdateCheckOutcome::NoFeed:
        return wxString::Format(_L("%s answered, but the update server could not be reached. Check this computer's internet "
                                   "connection and try again, or go on: Unlayered Slicer checks again when it starts."),
                                who);
    case PrinterUpdateCheckOutcome::UpToDate:
        text = wxString::Format(_L("%s is up to date: firmware %s."), who, from_u8(f.running_firmware));
        if (r.config_asked && !f.available_config.empty())
            text += " " + wxString::Format(_L("Printer settings %s."), from_u8(f.installed_config));
        else if (r.config_asked)
            text += " " + _L("The printer settings could not be checked right now.");
        break;
    case PrinterUpdateCheckOutcome::Available: {
        text = wxString::Format(_L("Updates are waiting for %s:"), who);
        const PrinterUpdateDecision d = decide_printer_update(f, no_printer_update_skips());
        if (d.firmware)
            text += "\n" + wxString::Format(_L("Firmware %s (it runs %s)"), from_u8(f.available_firmware), from_u8(f.running_firmware));
        if (d.config)
            text += "\n" + (f.config_known ? wxString::Format(_L("Printer settings %s (it has %s)"), from_u8(f.available_config),
                                                              from_u8(f.installed_config)) :
                                             wxString::Format(_L("Printer settings %s"), from_u8(f.available_config)));
        text += "\n" + _L("When you finish setup, the \"Printer update available\" notification installs them: the firmware "
                          "first, then the settings, and you confirm each one.");
        break;
    }
    }
    if (!r.config_asked) {
        const bool signed_in = AutoUpdater::stored_sign_in(config);
        text += "\n" + (signed_in && config && config->get_stealth_mode() ?
                            _L("Stealth mode is on, so the printer settings were not checked.") :
                            _L("Sign in to your free Unlayered account to check the printer settings too."));
    }
    return text;
}

} // namespace

bool GuideFrame::UnlayeredPrinterInFlow() const
{
    // Ticked in the picker (save_userguide_models has just written nozzle_selected).
    if (m_ProfileJson.is_object() && m_ProfileJson.contains("model") && m_ProfileJson["model"].is_array())
        for (const json &model : m_ProfileJson["model"]) {
            if (!model.is_object() || !model.contains("vendor") || !model["vendor"].is_string() ||
                model["vendor"].get<std::string>() != "Unlayered")
                continue;
            if (model.contains("nozzle_selected") && model["nozzle_selected"].is_string() &&
                !model["nozzle_selected"].get<std::string>().empty())
                return true;
        }
    // Already set up: an installed Unlayered printer, or any printer that connects as one.
    if (PresetBundle *bundle = wxGetApp().preset_bundle)
        for (const Preset &preset : bundle->printers)
            if (preset.is_visible && (is_unlayered_host(preset.config) || (preset.vendor && preset.vendor->name == "Unlayered")))
                return true;
    return false;
}

void GuideFrame::SendPrinterUpdateState()
{
    json res           = json::object();
    res["command"]     = "printer_update_state";
    res["sequence_id"] = "10002";
    res["signed_in"]   = AutoUpdater::stored_sign_in(wxGetApp().app_config);
    res["printer"]     = m_printer_check_printer;
    res["result"]      = m_printer_check_result;
    res["checked"]     = m_printer_checked;
    res["busy"]        = m_printer_check_busy;
    const wxString strJS = wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true));
    wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
}

void GuideFrame::CheckPrinterUpdate()
{
    if (m_printer_check_busy)
        return;
    AppConfig *config = wxGetApp().app_config;
    auto       send   = [this](const char *command, const std::string &text) {
        json res           = json::object();
        res["command"]     = command;
        res["sequence_id"] = "10002";
        res["printer"]     = m_printer_check_printer;
        res["text"]        = text;
        const wxString strJS = wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true));
        wxGetApp().CallAfter([this, strJS, alive = m_alive] {
            if (*alive)
                RunScript(strJS);
        });
    };

    UnlayeredPrinterTarget target;
    if (!find_unlayered_printer(target)) {
        // A new printer has no address yet. The check after the wizard runs as soon as it has one.
        if (config)
            config->set(PrinterUpdateOffer::CHECK_AFTER_SETUP_KEY, "1");
        BOOST_LOG_TRIVIAL(info) << "GuideFrame: printer update step: no Unlayered printer with an address; checking after setup";
        m_printer_checked       = true;
        m_printer_check_printer.clear();
        m_printer_check_result  = into_u8(_L("Unlayered Slicer does not know your printer's network address yet. When setup is "
                                             "done, press Connection (the signal icon next to Printer in the Prepare tab) and "
                                             "enter the printer's address. Unlayered Slicer then checks for updates and offers them."));
        send("printer_update_result", m_printer_check_result);
        return;
    }

    m_printer_check_busy    = true;
    m_printer_check_printer = into_u8(wxString::Format(_L("%s at %s"), from_u8(target.name), from_u8(target.host)));
    send("printer_update_checking", into_u8(_L("Checking...")));
    PrinterUpdateOffer::check_now(target.host, target.password, m_alive, [this, target, send](const PrinterUpdateOffer::CheckResult &r) {
        // check_now() calls this only while the frame is alive, on the UI thread.
        m_printer_check_busy = false;
        m_printer_checked    = true;
        const wxString who   = from_u8(!r.machine_name.empty() ? r.machine_name : target.name);
        m_printer_check_result = into_u8(describe_printer_check(r, who, target.host));
        if (AppConfig *config = wxGetApp().app_config)
            config->set(PrinterUpdateOffer::CHECK_AFTER_SETUP_KEY, printer_update_check_after_setup(r.outcome) ? "1" : "");
        send("printer_update_result", m_printer_check_result);
    });
}

GuideFrame::~GuideFrame()
{
    m_destroy = true;
    *m_alive  = false;
    // Test the pointer, not joinable(): a thread that already ran to completion is no longer
    // joinable but is still allocated.
    if (m_load_task) {
        if (m_load_task->joinable())
            m_load_task->join();
        delete m_load_task;
        m_load_task = nullptr;
    }
    if (m_browser) {
        delete m_browser;
        m_browser = nullptr;
    }
}

void GuideFrame::load_url(wxString &url)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__<< " enter, url=" << url.ToStdString();
    WebView::LoadUrl(m_browser, url);
    m_browser->SetFocus();
    UpdateState();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< " exit";
}

wxString GuideFrame::SetStartPage(GuidePage startpage, bool load)
{
    m_page = startpage;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(" enter, load=%1%, start_page=%2%")%load%int(startpage);
    //wxLogMessage("GUIDE: webpage_1  %s", (boost::filesystem::path(resources_dir()) / "web\\guide\\1\\index.html").make_preferred().string().c_str() );
    wxString TargetUrl = from_u8( (boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=1").make_preferred().string() );
    //wxLogMessage("GUIDE: webpage_2  %s", TargetUrl.mb_str());

    if (startpage == BBL_WELCOME){
        SetTitle(_L("Setup Wizard"));
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=1").make_preferred().string());
    } else if (startpage == BBL_REGION) {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=11").make_preferred().string());
    } else if (startpage == BBL_MODELS) {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=21").make_preferred().string());
    } else if (startpage == BBL_FILAMENTS) {
        SetTitle(_L("Setup Wizard"));

        int nSize = m_ProfileJson["model"].size();

        if (nSize>0)
            TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=22&wait=detail").make_preferred().string());
        else
            TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=21").make_preferred().string());
    } else if (startpage == BBL_FILAMENT_ONLY) {
        SetTitle("");
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=23&wait=detail").make_preferred().string());
    } else if (startpage == BBL_MODELS_ONLY) {
        SetTitle("");
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=24").make_preferred().string());
    }
    else {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=21").make_preferred().string());
    }

    wxString strlang = wxGetApp().current_language_code_safe();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(", strlang=%1%") % into_u8(strlang);
    if (strlang != "")
        TargetUrl = wxString::Format("%s&lang=%s", w2s(TargetUrl), strlang);

    TargetUrl = "file://" + TargetUrl;
    if (load)
        load_url(TargetUrl);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< " exit";
    return TargetUrl;
}

/**
 * Method that retrieves the current state from the web control and updates
 * the GUI the reflect this current state.
 */
void GuideFrame::UpdateState()
{
    // SetTitle(m_browser->GetCurrentTitle());
}

void GuideFrame::OnIdle(wxIdleEvent &WXUNUSED(evt))
{
    if (m_browser->IsBusy()) {
        wxSetCursor(wxCURSOR_ARROWWAIT);
    } else {
        wxSetCursor(wxNullCursor);
    }
}

// void GuideFrame::OnClose(wxCloseEvent& evt)
//{
//    this->Hide();
//}

/**
 * Callback invoked when there is a request to load a new page (for instance
 * when the user clicks a link)
 */
void GuideFrame::OnNavigationRequest(wxWebViewEvent &evt)
{
    // wxLogMessage("%s", "Navigation request to '" + evt.GetURL() + "'
    // (target='" + evt.GetTarget() + "')");

    UpdateState();
}

/**
 * Callback invoked when a navigation request was accepted
 */
void GuideFrame::OnNavigationComplete(wxWebViewEvent &evt)
{
    //wxLogMessage("%s", "Navigation complete; url='" + evt.GetURL() + "'");
    if (!bFirstComplete) {
        m_load_task = new boost::thread(boost::bind(&GuideFrame::LoadProfileData, this));
       // boost::thread LoadProfileThread(boost::bind(&GuideFrame::LoadProfileData, this));
        //LoadProfileThread.detach();

        bFirstComplete = true;
    }

    m_browser->Show();
    Layout();

    wxString NewUrl = evt.GetURL();

    UpdateState();
}

/**
 * Callback invoked when a page is finished loading
 */
void GuideFrame::OnDocumentLoaded(wxWebViewEvent &evt)
{
    // Only notify if the document is the main frame, not a subframe
    wxString tmpUrl = evt.GetURL();
    wxString NowUrl = m_browser->GetCurrentURL();

    if (evt.GetURL() == m_browser->GetCurrentURL()) {
        // wxLogMessage("%s", "Document loaded; url='" + evt.GetURL() + "'");
    }
    UpdateState();

    // wxCommandEvent *event = new
    // wxCommandEvent(EVT_WEB_RESPONSE_MESSAGE,this->GetId()); wxQueueEvent(this,
    // event);
}

/**
 * On new window, we veto to stop extra windows appearing
 */
void GuideFrame::OnNewWindow(wxWebViewEvent &evt)
{
    wxString flag = " (other)";

    wxString NewUrl= evt.GetURL();
    wxLaunchDefaultBrowser(NewUrl);
    //if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER) { flag = " (user)"; }
    // wxLogMessage("%s", "New window; url='" + evt.GetURL() + "'" + flag);

    // If we handle new window events then just load them in this window as we
    // are a single window browser
    // if (m_tools_handle_new_window->IsChecked())
    //    m_browser->LoadURL(evt.GetURL());

    UpdateState();
}

void GuideFrame::OnTitleChanged(wxWebViewEvent &evt)
{
    // SetTitle(evt.GetString());
    // wxLogMessage("%s", "Title changed; title='" + evt.GetString() + "'");
}

void GuideFrame::OnFullScreenChanged(wxWebViewEvent &evt)
{
    // wxLogMessage("Full screen changed; status = %d", evt.GetInt());
    ShowFullScreen(evt.GetInt() != 0);
}

bool GuideFrame::MergeDetailData()
{
    if (m_detail_loaded)
        return true;

    json detail;
    {
        std::lock_guard<std::mutex> lock(m_detail_mutex);
        if (!m_detail_ready)
            return false;
        detail        = std::move(m_detail_json);
        m_detail_json = json::object();
    }

    m_ProfileJson["machine"]  = std::move(detail["machine"]);
    m_ProfileJson["filament"] = std::move(detail["filament"]);
    m_ProfileJson["process"]  = std::move(detail["process"]);
    m_detail_loaded           = true;

    // Only the filament side is re-applied. SaveProfileData() must NOT run here: it recomputes
    // nozzle_selected from app_config, which would wipe the printer picks the user has already
    // made in the picker while stage 2 was still loading.
    ApplyInstalledFilamentSelection();

    for (const std::string &material : m_pending_default_materials)
        if (m_ProfileJson["filament"].contains(material))
            m_ProfileJson["filament"][material]["selected"] = 1;
    m_pending_default_materials.clear();

    BOOST_LOG_TRIVIAL(info) << "GuideFrame::MergeDetailData, " << m_ProfileJson["filament"].size() << " filaments, "
                            << m_ProfileJson["machine"].size() << " machines, " << m_ProfileJson["process"].size()
                            << " processes";
    return true;
}

void GuideFrame::SendProfileToWeb()
{
    json m_Res           = json::object();
    m_Res["command"]     = "response_userguide_profile";
    m_Res["sequence_id"] = "10001";
    m_Res["response"]    = m_ProfileJson;

    wxString strJS = wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', true));

    BOOST_LOG_TRIVIAL(trace) << "GuideFrame::SendProfileToWeb:" << strJS.c_str();
    wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
}

void GuideFrame::OnScriptMessage(wxWebViewEvent &evt)
{
    try {
        wxString strInput = evt.GetString();
        BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;OnRecv:" << strInput.c_str();
        json     j        = json::parse(strInput.utf8_string());

        wxString strCmd = j["command"];
        BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;Command:" << strCmd;

        // The privacy pages (guide 11 / 12 / 13) talk to the privacy flow.
        std::string privacy_reply;
        if (m_privacy && m_privacy->handle(j, privacy_reply)) {
            // The region page names a place; its printer-profile region is the wizard's region.
            if (const std::string region = m_privacy->guide_region(); !region.empty())
                m_Region = region;
            if (!privacy_reply.empty()) {
                const wxString js = wxString::FromUTF8(privacy_reply);
                wxGetApp().CallAfter([this, js] { RunScript(js); });
            }
            return;
        }

        if (strCmd == "close_page") {
            this->EndModal(wxID_CANCEL);
        }
        if (strCmd == "user_clause") {
            wxString strAction = j["data"]["action"];

            if (strAction == "refuse") {
                // CloseTheApp
                this->EndModal(wxID_OK);

                m_MainPtr->mainframe->Close(); // Refuse Clause, App quit immediately
            }
        } else if (strCmd == "user_private_choice") {
            wxString strAction = j["data"]["action"];

            if (strAction == "agree") {
                PrivacyUse = true;
            } else {
                PrivacyUse = false;
            }
        }
        else if (strCmd == "request_userguide_profile") {
            // The filament pages (guide 22/23) cannot render until the stage-2 load has landed.
            // Hold their request rather than answering with an empty list; LoadProfileData replies
            // when the data arrives. The printer picker does not set need_detail and is answered
            // straight away, which is the whole point of the split.
            const bool need_detail = j.contains("need_detail") && j["need_detail"].get<bool>();
            if (need_detail && !m_detail_loaded) {
                m_detail_request_pending = true;
                BOOST_LOG_TRIVIAL(info) << "GuideFrame::OnScriptMessage;request_userguide_profile held until filament data is ready";
            } else {
                SendProfileToWeb();
            }
        }
        else if (strCmd == "open_multiplanar_tutorial") {
            // Same bundled offline page Help > Multiplanar Tutorial and the Cut Planes gizmo open
            // (MainFrame.cpp, GLGizmoCutPlanes.cpp). Launched in the real browser rather than in
            // this WebView so it survives the wizard closing, and so the user keeps it open
            // alongside the slicer afterwards.
            // FileNameToURL escapes spaces ("Unlayered Slicer.app", "Program Files") and writes file:///;
            // a raw "file://" + path did not open on macOS.
            wxLaunchDefaultBrowser(wxFileSystem::FileNameToURL(wxFileName(from_u8(Slic3r::resources_dir() + "/web/tutorial/multiplanar.html"))), wxBROWSER_NEW_WINDOW);
        }
        else if (strCmd == "request_userguide_detail_status") {
            // The loading page asks this so it can hold its spinner until the filament data is in.
            // If stage 2 already landed the event has been and gone, so answer now; otherwise the
            // stage-2 CallAfter broadcasts the same message and the page picks it up there.
            if (m_detail_loaded) {
                json res            = json::object();
                res["command"]      = "userguide_profile_detail_load_finish";
                res["sequence_id"]  = "10001";
                wxString strJS      = wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true));
                wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
            }
        }
        else if (strCmd == "request_custom_filaments") {
            wxString strJS = update_custom_filaments();
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        }
        else if (strCmd == "create_custom_filament") {
            this->EndModal(wxID_OK);
            wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_CREATE_FILAMENT));
        } else if (strCmd == "modify_custom_filament") {
            m_editing_filament_id = j["id"];
            this->EndModal(wxID_EDIT);
        }
        else if (strCmd == "save_userguide_models")
        {
            json MSelected = j["data"];

            int nModel = m_ProfileJson["model"].size();
            for (int m = 0; m < nModel; m++) {
                json TmpModel = m_ProfileJson["model"][m];
                m_ProfileJson["model"][m]["nozzle_selected"] = "";

                for (auto it = MSelected.begin(); it != MSelected.end(); ++it) {
                    json OneSelect = it.value();

                    wxString s1 = TmpModel["model"];
                    wxString s2 = OneSelect["model"];
                    if (s1.compare(s2) == 0) {
                        m_ProfileJson["model"][m]["nozzle_selected"] = m_ProfileJson["model"][m]["nozzle_diameter"];

                        // Automatically select default materials for this printer model
                        // This mirrors the behavior of the old ConfigWizard::select_default_materials_for_printer_model()
                        if (TmpModel.contains("materials") && !TmpModel["materials"].is_null()) {
                            std::string materials_str;

                            // Handle both string and JSON array formats for materials
                            if (TmpModel["materials"].is_string()) {
                                materials_str = TmpModel["materials"].get<std::string>();
                            } else if (TmpModel["materials"].is_array()) {
                                // Convert JSON array to semicolon-separated string for unescape_strings_cstyle
                                for (const auto& material : TmpModel["materials"]) {
                                    if (!materials_str.empty()) materials_str += ";";
                                    materials_str += material.get<std::string>();
                                }
                            } else {
                                materials_str = "";
                            }

                            boost::trim(materials_str);
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Processing default_materials for printer: " << s1.ToStdString() << " - materials: " << materials_str;

                            // Use the same parsing logic as ConfigWizard::select_default_materials_for_printer_model()
                            // This calls unescape_strings_cstyle() just like Preset.cpp:298 does
                            std::vector<std::string> materials;
                            if (Slic3r::unescape_strings_cstyle(materials_str, materials)) {
                                for (const std::string& material : materials) {
                                    if (!material.empty()) {
                                        // Mark this filament as selected if it exists in our filament list
                                        // This mirrors appconfig_new.set(section, material, "true") from ConfigWizard.cpp:2150
                                        // The filament list only exists once stage 2 has landed. Rather than
                                        // block the UI thread on the loader, remember the name and apply it
                                        // from MergeDetailData when the data arrives.
                                        if (!m_detail_loaded) {
                                            m_pending_default_materials.insert(material);
                                        } else if (m_ProfileJson["filament"].contains(material)) {
                                            m_ProfileJson["filament"][material]["selected"] = 1;
                                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Automatically selected default filament: " << material;
                                        } else {
                                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Default filament '" << material << "' not found in available filaments for printer: " << s1.ToStdString();
                                        }
                                    }
                                }
                            } else {
                                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Malformed default_materials field: " << materials_str << " for printer: " << s1.ToStdString();
                            }
                        } else {
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " No default_materials defined for printer: " << s1.ToStdString();
                        }
                        break;
                    }
                }
            }
        }
        else if (strCmd == "save_userguide_filaments") {
            //reset
            for (auto it = m_ProfileJson["filament"].begin(); it != m_ProfileJson["filament"].end(); ++it)
            {
                m_ProfileJson["filament"][it.key()]["selected"] = 0;
            }

            json fSelected = j["data"]["filament"];
            int nF = fSelected.size();
            for (int m = 0; m < nF; m++)
            {
                std::string fName = fSelected[m];

                m_ProfileJson["filament"][fName]["selected"] = 1;
            }
        }
        else if (strCmd == "user_guide_finish") {
            SaveProfile();

            std::string oldregion = m_ProfileJson["region"];
            if (m_Region != oldregion) {
                AppConfig* config = GUI::wxGetApp().app_config;
                std::string country_code = config->get_country_code();
                NetworkAgent* agent = wxGetApp().getAgent();
                if (agent) {
                    agent->set_country_code(country_code);
                    if (wxGetApp().is_user_login()) {
                        BOOST_LOG_TRIVIAL(info) << "logout: user_logout on user_guide_finish";
                        // agent->user_logout();
                        wxGetApp().request_user_logout();
                    }
                }
            }

            this->EndModal(wxID_OK);

            // Unlayered: the wizard used to offer to install the Bambu network plug-in here.
        }
        else if (strCmd == "user_guide_create_printer") {
            this->EndModal(wxID_CANCEL);
            this->Close();
            GUI::wxGetApp().CallAfter([this] {GUI::wxGetApp().sidebar().create_printer_preset();});
        }
        else if (strCmd == "user_guide_cancel") {
            this->EndModal(wxID_CANCEL);
            this->Close();
        } else if (strCmd == "save_region") {
            m_Region = j["region"];
        }
        else if (strCmd == "network_plugin_install") {
            std::string sAction = j["data"]["action"];
            m_netplugin_answered = true;

            if (sAction == "yes") {
                if (!network_plugin_ready)
                    InstallNetplugin = true;
                else //already ready
                    InstallNetplugin = false;
            }
            else
                InstallNetplugin = false;
        }
        else if (strCmd == "request_page_after_privacy" || strCmd == "request_page_before_models") {
            // Unlayered 0.1.7: the "Settings from other slicers" step (guide 5import) sits between
            // the privacy page (13) and the printer picker (21), and only when something can be
            // imported. The privacy page asks where Continue goes (detecting the other slicers
            // there, once); the picker asks where Back goes.
            // Unlayered 0.1.7: before both, "Sign in to Unlayered" (guide 13signin)
            // whenever the slicer is not signed in - every run of the wizard, not once.
            const bool after_privacy = strCmd == "request_page_after_privacy";
            if (after_privacy && !m_other_slicers)
                DetectOtherSlicers();
            if (after_privacy && !m_signin_decided) {
                m_signin_decided = true;
                m_signin_shown   = !AutoUpdater::stored_sign_in(wxGetApp().app_config);
            }
            json res           = json::object();
            res["command"]     = after_privacy ? "page_after_privacy" : "page_before_models";
            res["sequence_id"] = "10002";
            if (after_privacy)
                res["page"] = m_signin_shown ? "13signin" : (m_other_slicers_offered ? "5import" : "21");
            else
                res["page"] = m_other_slicers_offered ? "5import" : (m_signin_shown ? "13signin" : "13");
            const wxString strJS = wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true));
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        }
        else if (strCmd == "request_page_after_signin" || strCmd == "request_page_before_import") {
            // Guide 13signin's Next/Skip, and Back on guide 5import.
            const bool after_signin = strCmd == "request_page_after_signin";
            if (after_signin && !m_other_slicers)
                DetectOtherSlicers();
            json res           = json::object();
            res["command"]     = after_signin ? "page_after_signin" : "page_before_import";
            res["sequence_id"] = "10002";
            res["page"]        = after_signin ? (m_other_slicers_offered ? "5import" : "21") : (m_signin_shown ? "13signin" : "13");
            const wxString strJS = wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true));
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        }
        else if (strCmd == "request_page_after_models" || strCmd == "request_page_before_filaments") {
            // Unlayered 0.1.8: "Update your Unlayered printer" (guide 21update) sits between the
            // printer picker (21) and the filament page (22) when an Unlayered printer is ticked or
            // already set up. The picker's Next asks where it goes, the filament page's Back too.
            const bool after_models = strCmd == "request_page_after_models";
            const bool show         = UnlayeredPrinterInFlow();
            BOOST_LOG_TRIVIAL(info) << "GuideFrame: printer update step " << (show ? "shown" : "not shown");
            json res           = json::object();
            res["command"]     = after_models ? "page_after_models" : "page_before_filaments";
            res["sequence_id"] = "10002";
            res["page"]        = show ? "21update" : (after_models ? "22" : "21");
            const wxString strJS = wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true));
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        }
        else if (strCmd == "request_printer_update_state") {
            // Guide 21update: signed in or not, and the last check's result (Back from page 22).
            SendPrinterUpdateState();
        }
        else if (strCmd == "check_printer_update") {
            // Guide 21update pressed Check for updates. Reads only; installs nothing.
            CheckPrinterUpdate();
        }
        else if (strCmd == "request_signin_state") {
            // Guide 13signin: its texts, translated here, and whether the slicer is signed in already
            // (Back from a later page after signing in).
            std::string email;
            const bool  in     = AutoUpdater::stored_sign_in(wxGetApp().app_config, &email);
            json        res    = json::object();
            res["command"]     = "signin_state";
            res["sequence_id"] = "10002";
            res["title"]       = into_u8(_L("Sign in to Unlayered"));
            res["text"]        = into_u8(_L("Sign in with your Unlayered account. Unlayered Slicer then keeps itself up to date "
                                            "and offers configuration updates for your printers."));
            res["button"]      = into_u8(_L("Sign in"));
            res["skip"]        = into_u8(_L("Skip for now"));
            res["signed_in"]   = in;
            if (in)
                res["signed_in_text"] = into_u8(wxString::Format(_L("Signed in as %s."), from_u8(email)));
            const wxString strJS = wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true));
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        }
        else if (strCmd == "account_sign_in") {
            // Guide 13signin pressed Sign in: the browser sign-in, modal over the wizard. Not from
            // inside the script handler.
            wxGetApp().CallAfter([this] {
                if (m_destroy)
                    return;
                std::string email;
                const bool  in   = AutoUpdater::sign_in_for_setup(this, wxGetApp().app_config, &email);
                json        res  = json::object();
                res["command"]   = "signin_result";
                res["sequence_id"] = "10002";
                res["signed_in"] = in;
                if (in)
                    res["signed_in_text"] = into_u8(wxString::Format(_L("Signed in as %s."), from_u8(email)));
                else
                    res["failed_text"] = into_u8(_L("Not signed in. Press Sign in to try again."));
                RunScript(wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true)));
            });
        }
        else if (strCmd == "request_other_slicers") {
            // Guide 5import: what was found, per app, with the texts translated here.
            if (!m_other_slicers)
                DetectOtherSlicers();
            const OtherSlicersFound &found = *m_other_slicers;
            json res           = json::object();
            res["command"]     = "other_slicers";
            res["sequence_id"] = "10002";
            res["total"]       = found.total();
            res["title"]       = into_u8(_L("Settings from other slicers"));
            res["text"]        = into_u8(_L("We found these slicers on this computer. Import their printers, filaments and presets now, "
                                            "and they are already ticked on the next pages. Nothing in those apps is changed."));
            res["note"]        = into_u8(_L("Printer network addresses come along; API keys and passwords do not."));
            res["import"]      = into_u8(_L("Import"));
            res["importing"]   = into_u8(_L("Importing..."));
            res["imported"]    = m_other_slicers_imported;
            res["summary"]     = m_other_slicers_summary;
            json apps          = json::array();
            for (size_t i = 0; i < found.scans.size(); ++i)
                if (found.counts[i].total() > 0)
                    apps.push_back({{"index", int(i)},
                                    {"label", into_u8(from_u8(OtherSlicers::app_name(found.scans[i].location.app)) + ": " +
                                                      OtherSlicersFound::describe(found.counts[i]))}});
            res["apps"]          = apps;
            const wxString strJS = wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true));
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        }
        else if (strCmd == "import_other_slicers") {
            // Guide 5import pressed Import: import the ticked apps now, then refresh what the printer
            // picker and the filament page show from the app config the import just changed.
            std::vector<int> indices;
            if (j.contains("apps") && j["apps"].is_array())
                for (const json &v : j["apps"])
                    if (v.is_number_integer())
                        indices.push_back(v.get<int>());
            wxGetApp().CallAfter([this, indices] {
                if (m_destroy || !m_other_slicers)
                    return;
                std::vector<OtherSlicers::Scan> selected;
                for (int i : indices)
                    if (i >= 0 && size_t(i) < m_other_slicers->scans.size())
                        selected.push_back(m_other_slicers->scans[size_t(i)]);
                const OtherSlicersImportResult result = import_other_slicers(this, selected);
                m_other_slicers_imported = true;
                m_other_slicers_summary  = into_u8(result.summary.empty() ? _L("Nothing was imported.") : result.summary);
                // The enabled printer models (nozzle_selected) and filaments come from the app config.
                // SaveProfileData() also reloads the region from it, which the region page may have
                // changed without saving yet: keep that.
                const std::string region = m_Region;
                SaveProfileData();
                m_Region = region;
                json res           = json::object();
                res["command"]     = "other_slicers_imported";
                res["sequence_id"] = "10002";
                res["summary"]     = m_other_slicers_summary;
                res["next"]        = into_u8(_L("Next"));
                RunScript(wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true)));
            });
        }
        else if (strCmd == "request_network_plugin_hint") {
            // The last page: the Bambu Lab network plug-in box stays unticked (it is
            // Bambu's closed DLL) unless OrcaSlicer or Bambu Studio has Bambu Lab printers in its
            // printer list, imported or not.
            if (!m_other_slicers)
                DetectOtherSlicers();
            json res           = json::object();
            res["command"]     = "network_plugin_hint";
            res["sequence_id"] = "10002";
            res["tick"]        = !m_bambu_printers_in.empty() && !network_plugin_ready;
            if (!m_bambu_printers_in.empty())
                res["text"] = into_u8(wxString::Format(_L("We found Bambu Lab printers in %s, so this is ticked."), from_u8(m_bambu_printers_in)));
            const wxString strJS = wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true));
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        }
        else if (strCmd == "save_stealth_mode") {
            wxString strAction = j["data"]["action"];
            m_stealth_answered = true;

            if (strAction == "yes") {
                StealthMode = true;
            } else {
                StealthMode = false;
            }
        }
    } catch (std::exception &e) {
        // wxMessageBox(e.what(), "json Exception", MB_OK);
        BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;Error:" << e.what();
    }

    wxString strAll = m_ProfileJson.dump(-1,' ',false, json::error_handler_t::ignore);
}

void GuideFrame::RunScript(const wxString &javascript)
{
    // Remember the script we run in any case, so the next time the user opens
    // the "Run Script" dialog box, it is shown there for convenient updating.
    //m_javascript = javascript;

    // wxLogMessage("Running JavaScript:\n%s\n", javascript);

    if (!m_browser) return;

    WebView::RunScript(m_browser, javascript);
}

#if wxUSE_WEBVIEW_IE
void GuideFrame::OnRunScriptObjectWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var person = new Object();person.name = 'Foo'; \
    person.lastName = 'Bar';return person;}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void GuideFrame::OnRunScriptDateWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var d = new Date('10/08/2017 21:30:40'); \
    var tzoffset = d.getTimezoneOffset() * 60000; return \
    new Date(d.getTime() - tzoffset);}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void GuideFrame::OnRunScriptArrayWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){ return [\"foo\", \"bar\"]; }f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}
#endif

/**
 * Callback invoked when a loading error occurs
 */
void GuideFrame::OnError(wxWebViewEvent &evt)
{
#define WX_ERROR_CASE(type) \
    case type: category = #type; break;

    wxString category;
    switch (evt.GetInt()) {
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CONNECTION);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CERTIFICATE);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_AUTH);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_SECURITY);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_NOT_FOUND);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_REQUEST);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_USER_CANCELLED);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_OTHER);
    }

    // wxLogMessage("%s", "Error; url='" + evt.GetURL() + "', error='" +
    // category + " (" + evt.GetString() + ")'");

    // Show the info bar with an error
    // m_info->ShowMessage(_L("An error occurred loading ") + evt.GetURL() +
    // "\n" + "'" + category + "'", wxICON_ERROR);
    BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnError: An error occurred loading " << evt.GetURL() << category;

    UpdateState();
}

void GuideFrame::OnScriptResponseMessage(wxCommandEvent &WXUNUSED(evt))
{
}

bool GuideFrame::IsFirstUse()
{
    wxString    strUse;
    std::string strVal = wxGetApp().app_config->get(std::string(m_SectionName.mb_str()), "finish");
    if (strVal == "1")
        return false;

    if (orca_bundle_rsrc == true)
        return true;

    return true;
}

int GuideFrame::SaveProfile()
{
    // SoftFever: don't collect info
    //privacy
    // if (PrivacyUse == true) {
    //     m_MainPtr->app_config->set(std::string(m_SectionName.mb_str()), "privacyuse", "1");
    // } else
    //     m_MainPtr->app_config->set(std::string(m_SectionName.mb_str()), "privacyuse", "0");

    m_MainPtr->app_config->set("region", m_Region);
    // Unlayered: since 2026-10-22 no wizard page asks about stealth mode (off by default, Preferences
    // only), so nothing reports save_stealth_mode and this never writes. Kept so a page that asks
    // again only has to report the answer.
    if (m_stealth_answered)
        m_MainPtr->app_config->set_bool("stealth_mode", StealthMode);
    // Usage diagnostics are not decided here: the privacy page saved the person's choice the moment
    // they pressed Continue (PrivacyFlow), and nothing is sent while the wizard is open.

    //finish
    m_MainPtr->app_config->set(std::string(m_SectionName.mb_str()), "finish", "1");

    m_MainPtr->app_config->save();

    std::string strAll = m_ProfileJson.dump(-1, ' ', false, json::error_handler_t::ignore);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "before save to app_config: "<< std::endl<<strAll;

    // Set filaments to app_config -- but only if stage 2 actually loaded them. Finishing from a
    // printers-only page (guide 24) before the filament data lands must leave the installed list
    // alone; rebuilding it from an empty m_ProfileJson would uninstall every filament the user
    // has. SaveProfileData() already seeded m_appconfig_new with the current list.
    if (m_detail_loaded) {
        const std::string &section_name = AppConfig::SECTION_FILAMENTS;
        std::map<std::string, std::string> section_new;
        m_appconfig_new.clear_section(section_name);
        for (auto it = m_ProfileJson["filament"].begin(); it != m_ProfileJson["filament"].end(); ++it) {
            if (it.value()["selected"] == 1){
                section_new[it.key()] = "true";
            }
        }
        m_appconfig_new.set_section(section_name, section_new);
    } else {
        BOOST_LOG_TRIVIAL(info) << "GuideFrame::SaveProfile, filament data not loaded -- leaving the installed filament list untouched";
    }

    //set vendors to app_config
    Slic3r::AppConfig::VendorMap empty_vendor_map;
    m_appconfig_new.set_vendors(empty_vendor_map);
    for (auto it = m_ProfileJson["model"].begin(); it != m_ProfileJson["model"].end(); ++it)
    {
        if (it.value().is_object()) {
            json temp_model = it.value();
            std::string model_name = temp_model["model"];
            std::string vendor_name = temp_model["vendor"];
            std::string selected = temp_model["nozzle_selected"];
            boost::trim(selected);
            std::string nozzle;
            while (selected.size() > 0) {
                auto pos = selected.find(';');
                if (pos != std::string::npos) {
                    nozzle   = selected.substr(0, pos);
                    m_appconfig_new.set_variant(vendor_name, model_name, nozzle, "true");
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("vendor_name %1%, model_name %2%, nozzle %3% selected")%vendor_name %model_name %nozzle;
                    selected = selected.substr(pos + 1);
                    boost::trim(selected);
                }
                else {
                    m_appconfig_new.set_variant(vendor_name, model_name, selected, "true");
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("vendor_name %1%, model_name %2%, nozzle %3% selected")%vendor_name %model_name %selected;
                    break;
                }
            }
        }
    }

    //m_appconfig_new

    return 0;
}

static std::set<std::string> get_new_added_presets(const std::map<std::string, std::string>& old_data, const std::map<std::string, std::string>& new_data)
{
    auto get_aliases = [](const std::map<std::string, std::string>& data) {
        std::set<std::string> old_aliases;
        for (auto item : data) {
            const std::string& name = item.first;
            size_t pos = name.find("@");
            old_aliases.emplace(pos == std::string::npos ? name : name.substr(0, pos-1));
        }
        return old_aliases;
    };

    std::set<std::string> old_aliases = get_aliases(old_data);
    std::set<std::string> new_aliases = get_aliases(new_data);
    std::set<std::string> diff;
    std::set_difference(new_aliases.begin(), new_aliases.end(), old_aliases.begin(), old_aliases.end(), std::inserter(diff, diff.begin()));

    return diff;
}

static std::string get_first_added_preset(const std::map<std::string, std::string>& old_data, const std::map<std::string, std::string>& new_data)
{
    std::set<std::string> diff = get_new_added_presets(old_data, new_data);
    if (diff.empty())
        return std::string();
    return *diff.begin();
}

bool GuideFrame::apply_config(AppConfig *app_config, PresetBundle *preset_bundle, const PresetUpdater *updater, bool& apply_keeped_changes)
{
    const auto enabled_vendors = m_appconfig_new.vendors();
    const auto old_enabled_vendors = app_config->vendors();

    const auto enabled_filaments = m_appconfig_new.has_section(AppConfig::SECTION_FILAMENTS) ? m_appconfig_new.get_section(AppConfig::SECTION_FILAMENTS) : std::map<std::string, std::string>();
    const auto old_enabled_filaments = app_config->has_section(AppConfig::SECTION_FILAMENTS) ? app_config->get_section(AppConfig::SECTION_FILAMENTS) : std::map<std::string, std::string>();

    bool check_unsaved_preset_changes = false;
    std::vector<std::string> install_bundles;
    std::vector<std::string> remove_bundles;
    const auto vendor_dir = (boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
    for (const auto &it : enabled_vendors) {
        if (it.second.size() > 0) {
            auto vendor_file = vendor_dir/(it.first + ".json");
            if (!fs::exists(vendor_file)) {
                install_bundles.emplace_back(it.first);
            }
        }
    }

    //add the removed vendor bundles
    for (const auto &it : old_enabled_vendors) {
        if (it.second.size() > 0) {
            if (enabled_vendors.find(it.first) != enabled_vendors.end())
                continue;
            auto vendor_file = vendor_dir/(it.first + ".json");
            if (fs::exists(vendor_file)) {
                remove_bundles.emplace_back(it.first);
            }
        }
    }

    check_unsaved_preset_changes = (enabled_vendors != old_enabled_vendors) || (enabled_filaments != old_enabled_filaments);
    wxString header = _L("The configuration package is changed in previous Config Guide");
    wxString caption = _L("Configuration package changed");
    int act_btns = ActionButtons::KEEP|ActionButtons::SAVE;

    if (check_unsaved_preset_changes &&
        !wxGetApp().check_and_keep_current_preset_changes(caption, header, act_btns, &apply_keeped_changes))
        return false;

    // If there are bundles to install, they will be installed by apply_vendor_config
    if (!install_bundles.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Will install " << install_bundles.size() << " vendor bundles from resources";
    } else {
        BOOST_LOG_TRIVIAL(info) << "No bundles need to be installed from resource directory";
    }

    // Not remove, because these bundles may be updated
    //if (remove_bundles.size() > 0) {
    //    //remove unused bundles
    //    for (const auto &it : remove_bundles) {
    //        auto vendor_file = vendor_dir/(it + ".json");
    //        auto sub_dir = vendor_dir/(it);
    //        if (fs::exists(vendor_file))
    //            fs::remove(vendor_file);
    //        if (fs::exists(sub_dir))
    //            fs::remove_all(sub_dir);
    //    }
    //} else {
    //    BOOST_LOG_TRIVIAL(info) << "No bundles need to be removed";
    //}

    std::string preferred_model;
    std::string preferred_variant;
    PrinterTechnology preferred_pt = ptFFF;
    auto get_preferred_printer_model = [preset_bundle, enabled_vendors, old_enabled_vendors, preferred_pt](const std::string& bundle_name, std::string& variant) {
        const auto config = enabled_vendors.find(bundle_name);
        if (config == enabled_vendors.end())
            return std::string();

        const VendorProfile & printer_profile = preset_bundle->vendors[bundle_name];
        const std::map<std::string, std::set<std::string>>& model_maps = config->second;
        //for (const auto& vendor_profile : preset_bundle->vendors) {
        for (const auto& model_it: model_maps) {
            if (model_it.second.size() > 0) {
                variant = *model_it.second.begin();
                if (model_it.second.size() > 1) {
                    if (printer_profile.models.size() > 0) {
                        const VendorProfile::PrinterModel& printer_model = *std::find_if(printer_profile.models.begin(), printer_profile.models.end(),
                            [id = model_it.first](auto& m) { return m.id == id; });
                        for (auto& vt : printer_model.variants) {
                            if (std::find(model_it.second.begin(), model_it.second.end(), vt.name) != model_it.second.end()) { variant = vt.name; break; }
                        }
                    }
                    else if (variant != PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT){
                        if (std::find(model_it.second.begin(), model_it.second.end(), PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT) != model_it.second.end())
                            variant = PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT;
                    }
                }

                const auto config_old = old_enabled_vendors.find(bundle_name);
                if (config_old == old_enabled_vendors.end())
                    return model_it.first;
                const auto model_it_old = config_old->second.find(model_it.first);
                if (model_it_old == config_old->second.end())
                    return model_it.first;
                else if (model_it_old->second != model_it.second) {
                    for (const auto& var : model_it.second)
                        if (model_it_old->second.find(var) == model_it_old->second.end()) {
                            variant = var;
                            return model_it.first;
                        }
                }
            }
        }
        //}
        if (!variant.empty())
            variant.clear();
        return std::string();
    };
    // Orca "custom" printers are considered first, then 3rd party.
    if (preferred_model = get_preferred_printer_model(PresetBundle::ORCA_DEFAULT_BUNDLE, preferred_variant);
        preferred_model.empty()) {
        for (const auto& bundle : enabled_vendors) {
            if (bundle.first == PresetBundle::ORCA_DEFAULT_BUNDLE) { continue; }
            if (preferred_model = get_preferred_printer_model(bundle.first, preferred_variant);
                !preferred_model.empty())
                    break;
        }
    }

    std::string first_added_filament;
    auto get_first_added_material_preset = [this, app_config](const std::string& section_name, std::string& first_added_preset) {
        if (m_appconfig_new.has_section(section_name)) {
            // get first of new added preset names
            const std::map<std::string, std::string>& old_presets = app_config->has_section(section_name) ? app_config->get_section(section_name) : std::map<std::string, std::string>();
            first_added_preset = get_first_added_preset(old_presets, m_appconfig_new.get_section(section_name));
        }
    };
    // Not switch filament
    //get_first_added_material_preset(AppConfig::SECTION_FILAMENTS, first_added_filament);

    // ORCA: functionality moved to PresetBundle::apply_vendor_config; keeping for future reference
    // // For each @System filament, check if a vendor-specific override exists
    // // in the loaded profiles. If so, replace the @System variant with the
    // // override (e.g. replace "Generic ABS @System" with BBL "Generic ABS").
    // // When printers from the default bundle are also selected, keep @System
    // // too since those printers need it.
    // static const std::string system_suffix              = " @System";
    // auto                     it_default                 = enabled_vendors.find(PresetBundle::ORCA_DEFAULT_BUNDLE);
    // bool                     has_default_bundle_printer = it_default != enabled_vendors.end() && !it_default->second.empty();
    // bool                     has_filament_profiles      = m_ProfileJson.contains("filament");

    // // Check if any non-default vendor has selected printers
    // bool has_vendor_printer = false;
    // for (const auto& [vendor, models] : enabled_vendors) {
    //     if (vendor != PresetBundle::ORCA_DEFAULT_BUNDLE && !models.empty()) {
    //         has_vendor_printer = true;
    //         break;
    //     }
    // }

    // std::map<std::string, std::string> supplemented_filaments;
    // for (const auto& [name, value] : enabled_filaments) {
    //     if (name.size() > system_suffix.size() &&
    //         name.compare(name.size() - system_suffix.size(), system_suffix.size(), system_suffix) == 0) {
    //         std::string short_name = name.substr(0, name.size() - system_suffix.size());
    //         if (has_vendor_printer && has_filament_profiles && m_ProfileJson["filament"].contains(short_name)) {
    //             supplemented_filaments[short_name] = value;
    //             BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Replacing @System filament: '" << name << "' -> '" << short_name << "'";
    //             if (has_default_bundle_printer) {
    //                 supplemented_filaments[name] = value;
    //                 BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Also keeping '" << name << "' for default bundle printers";
    //             }
    //             continue;
    //         }
    //     }
    //     supplemented_filaments[name] = value;
    // }

    // //update the app_config
    // app_config->set_section(AppConfig::SECTION_FILAMENTS, supplemented_filaments);
    // app_config->set_vendors(m_appconfig_new);

    // if (check_unsaved_preset_changes)
    //     preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::Enable,
    //                                 {preferred_model, preferred_variant, first_added_filament, std::string()});

    // // If the active filament is not in the wizard-selected filaments, switch to the first
    // // compatible wizard-selected filament. This handles the first-run case where load_presets
    // // falls back to "Generic PLA" even though the user selected a different filament.
    // bool active_filament_selected = supplemented_filaments.empty()
    //     || supplemented_filaments.count(preset_bundle->filament_presets.front()) > 0;
    // if (!active_filament_selected) {
    //     for (const auto& [filament_name, _] : supplemented_filaments) {
    //         const Preset* preset = preset_bundle->filaments.find_preset(filament_name);
    //         if (preset && preset->is_visible && preset->is_compatible) {
    //             preset_bundle->filaments.select_preset_by_name(filament_name, true);
    //             preset_bundle->filament_presets.front() = preset_bundle->filaments.get_selected_preset_name();
    //             break;
    //         }
    //     }
    // }

    // // Update the selections from the compatibilty.
    // preset_bundle->export_selections(*app_config);

    BOOST_LOG_TRIVIAL(info) << "calling apply_vendor_config from WebGuideDialog";
    // Call the Core library function to apply vendor configuration
    // This handles bundle installation, filament @System substitution, AppConfig updates, and preset loading
    if (!preset_bundle->apply_vendor_config(
            enabled_vendors,
            enabled_filaments,
            app_config,
            true,
            preferred_model,
            preferred_variant))
        return false;

    return true;
}

bool GuideFrame::run()
{
    //BOOST_LOG_TRIVIAL(info) << boost::format("Running ConfigWizard, reason: %1%, start_page: %2%") % reason % start_page;

    GUI_App &app = wxGetApp();

    //p->set_run_reason(reason);
    //p->set_start_page(start_page);
    app.preset_bundle->export_selections(*app.app_config);

    BOOST_LOG_TRIVIAL(info) << "GuideFrame before ShowModal";
    // display position
    int main_frame_display_index = wxDisplay::GetFromWindow(wxGetApp().mainframe);
    int guide_display_index = wxDisplay::GetFromWindow(this);
    if (main_frame_display_index != guide_display_index) {
        wxDisplay display    = wxDisplay(main_frame_display_index);
        wxRect    screenRect = display.GetGeometry();
        int       guide_x    = screenRect.x + (screenRect.width - this->GetSize().GetWidth()) / 2;
        int       guide_y    = screenRect.y + (screenRect.height - this->GetSize().GetHeight()) / 2;
        this->SetPosition(wxPoint(guide_x, guide_y));
    }

    int result = this->ShowModal();
    if (result == wxID_OK) {
        bool apply_keeped_changes = false;
        BOOST_LOG_TRIVIAL(info) << "GuideFrame returned ok";
        if (! this->apply_config(app.app_config, app.preset_bundle, app.preset_updater, apply_keeped_changes))
            return false;

        if (apply_keeped_changes)
            app.apply_keeped_preset_modifications();

        app.app_config->set_legacy_datadir(false);
        app.update_mode();
        // BBS
        //app.obj_manipul()->update_ui_from_settings();
        BOOST_LOG_TRIVIAL(info) << "GuideFrame applied";
        this->Close();
        return true;
    } else if (result == wxID_CANCEL) {
        BOOST_LOG_TRIVIAL(info) << "GuideFrame cancelled";
        if (app.preset_bundle->printers.only_default_printers()) {
            //we install the default here
            bool apply_keeped_changes = false;
            //clear filament section and use default materials
            app.app_config->set_variant(PresetBundle::ORCA_DEFAULT_BUNDLE,
                PresetBundle::ORCA_DEFAULT_PRINTER_MODEL, PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT, "true");
            app.app_config->clear_section(AppConfig::SECTION_FILAMENTS);
            app.preset_bundle->load_selections(*app.app_config, {PresetBundle::ORCA_DEFAULT_PRINTER_MODEL, PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT, PresetBundle::ORCA_DEFAULT_FILAMENT, std::string()});

            app.app_config->set_legacy_datadir(false);
            app.update_mode();
            return true;
        }
        else
            return false;
    } else if (result == wxID_EDIT) {
        this->Close();
        Filamentinformation *filament_info = new Filamentinformation();
        filament_info->filament_id        = m_editing_filament_id;
        wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_MODIFY_FILAMENT, filament_info));
        return false;
    }
    else
        return false;
}

int GuideFrame::GetFilamentInfo( std::string VendorDirectory, json & pFilaList, std::string filepath, std::string &sVendor, std::string &sType)
{
    //GetStardardFilePath(filepath);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " GetFilamentInfo:VendorDirectory - " << VendorDirectory << ", Filepath - "<<filepath;

    const auto cache_it = filament_info_cache.find(filepath);
    if (cache_it != filament_info_cache.end()) {
        if (sVendor.empty()) {
            sVendor = cache_it->second.vendor;
        }
        if (sType.empty()) {
            sType = cache_it->second.type;
        }
        return cache_it->second.status;
    }

    try {
        std::string contents;
        LoadFile(filepath, contents);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Json Contents: " << contents;
        json jLocal = json::parse(contents);

        if (sVendor == "") {
            if (jLocal.contains("filament_vendor"))
                sVendor = jLocal["filament_vendor"][0];
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << filepath << " - Not Contains filament_vendor";
            }
        }

        if (sType == "") {
            if (jLocal.contains("filament_type"))
                sType = jLocal["filament_type"][0];
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << filepath << " - Not Contains filament_type";
            }
        }

        if (sVendor == "" || sType == "")
        {
            if (jLocal.contains("inherits")) {
                std::string FName = jLocal["inherits"];

                if (!pFilaList.contains(FName)) {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "pFilaList - Not Contains inherits filaments: " << FName;
                    filament_info_cache[filepath] = CachedFilamentInfo{-1, sVendor, sType};
                    return -1;
                }

                std::string FPath = pFilaList[FName]["sub_path"];
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Before Format Inherits Path: VendorDirectory - " << VendorDirectory << ", sub_path - " << FPath;
                wxString strNewFile = wxString::Format("%s%c%s", wxString(VendorDirectory.c_str(), wxConvUTF8), boost::filesystem::path::preferred_separator, FPath);
                boost::filesystem::path inherits_path(w2s(strNewFile));
                if (!boost::filesystem::exists(inherits_path))
                    inherits_path = (boost::filesystem::path(m_OrcaFilaLibPath) / boost::filesystem::path(FPath)).make_preferred();

                //boost::filesystem::path nf(strNewFile.c_str());
                if (boost::filesystem::exists(inherits_path)) {
                    const int ret = GetFilamentInfo(VendorDirectory,pFilaList, inherits_path.string(), sVendor, sType);
                    filament_info_cache[filepath] = CachedFilamentInfo{ret, sVendor, sType};
                    return ret;
                } else {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " inherits File Not Exist: " << inherits_path;
                    filament_info_cache[filepath] = CachedFilamentInfo{-1, sVendor, sType};
                    return -1;
                }
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << filepath << " - Not Contains inherits";
                if (sType == "") {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "sType is Empty";
                    filament_info_cache[filepath] = CachedFilamentInfo{-1, sVendor, sType};
                    return -1;
                }
                else
                    sVendor = "Generic";
                    filament_info_cache[filepath] = CachedFilamentInfo{0, sVendor, sType};
                    return 0;
            }
        }
        else {
            filament_info_cache[filepath] = CachedFilamentInfo{0, sVendor, sType};
            return 0;
        }
    }
    catch(nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<filepath <<" got a nlohmann::detail::parse_error, reason = " << err.what();
        filament_info_cache[filepath] = CachedFilamentInfo{-1, sVendor, sType};
        return -1;
    }
    catch (std::exception &e)
    {
        // wxLogMessage("GUIDE: load_profile_error  %s ", e.what());
        // wxMessageBox(e.what(), "", MB_OK);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse " << filepath <<" got exception: "<<e.what();
        filament_info_cache[filepath] = CachedFilamentInfo{-1, sVendor, sType};
        return -1;
    }

    return 0;
}

int GuideFrame::LoadProfileData()
{
    try {
        m_ProfileJson             = json::parse("{}");
        m_ProfileJson["model"]    = json::array();
        m_ProfileJson["machine"]  = json::object();
        m_ProfileJson["filament"] = json::object();
        m_ProfileJson["process"]  = json::array();

        vendor_dir      = (boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
        rsrc_vendor_dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();

        // Orca: add custom as default
        // Orca: add json logic for vendor bundle
        orca_bundle_rsrc = true;

        // search if there exists a .json file in vendor_dir folder, if exists, set orca_bundle_rsrc to false
        for (const auto& entry : boost::filesystem::directory_iterator(vendor_dir)) {
            if (!boost::filesystem::is_directory(entry) && boost::iequals(entry.path().extension().string(), ".json") && !boost::iequals(entry.path().stem().string(), PresetBundle::ORCA_FILAMENT_LIBRARY)) {
                orca_bundle_rsrc = false;
                break;
            }
        }

        // Build the vendor list once and walk it twice. The filament library has to come first:
        // other vendors' filaments inherit from it via GetFilamentInfo/m_OrcaFilaList.
        std::set<std::string>                            loaded_vendors;
        std::vector<std::pair<std::string, std::string>> vendor_list;

        auto filament_library_name = boost::filesystem::path(PresetBundle::ORCA_FILAMENT_LIBRARY).replace_extension(".json");
        if (boost::filesystem::exists(vendor_dir / filament_library_name)) {
            m_OrcaFilaLibPath = (vendor_dir / PresetBundle::ORCA_FILAMENT_LIBRARY).string();
            vendor_list.emplace_back(PresetBundle::ORCA_FILAMENT_LIBRARY, (vendor_dir / filament_library_name).string());
        } else {
            m_OrcaFilaLibPath = (rsrc_vendor_dir / PresetBundle::ORCA_FILAMENT_LIBRARY).string();
            vendor_list.emplace_back(PresetBundle::ORCA_FILAMENT_LIBRARY, (rsrc_vendor_dir / filament_library_name).string());
        }
        loaded_vendors.insert(PresetBundle::ORCA_FILAMENT_LIBRARY);

        // custom bundles from the user data path first, then the ones shipped in resources
        for (const auto *dir : {&vendor_dir, &rsrc_vendor_dir}) {
            boost::filesystem::directory_iterator endIter;
            for (boost::filesystem::directory_iterator iter(*dir); iter != endIter; iter++) {
                if (boost::filesystem::is_directory(*iter))
                    continue;

                wxString strVendor = from_u8(iter->path().string()).BeforeLast('.');
                strVendor          = strVendor.AfterLast('\\');
                strVendor          = strVendor.AfterLast('/');

                wxString strExtension = from_u8(iter->path().string()).AfterLast('.').Lower();
                if (strExtension.CmpNoCase("json") != 0 || loaded_vendors.find(w2s(strVendor)) != loaded_vendors.end())
                    continue;

                vendor_list.emplace_back(w2s(strVendor), iter->path().string());
                loaded_vendors.insert(w2s(strVendor));
            }
        }

        // Stage 1: machine models only. This is what the printer picker renders, and it is a small
        // fraction of the profile data, so the guide can leave its loading page almost immediately
        // instead of waiting on every vendor's filament and process presets.
        for (const auto &vendor : vendor_list) {
            LoadProfileFamily(vendor.first, vendor.second, ProfileStage::Models, m_ProfileJson);
            if (m_destroy)
                return 0;
        }

        wxGetApp().CallAfter([this] {
            if (m_destroy)
                return;

            // sync to appconfig first to populate current selections
            SaveProfileData();

            BOOST_LOG_TRIVIAL(info) << "GuideFrame::LoadProfileData, models ready, " << m_ProfileJson["model"].size() << " printer models";

            json m_Res           = json::object();
            m_Res["command"]     = "userguide_profile_load_finish";
            m_Res["sequence_id"] = "10001";

            RunScript(wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', true)));
        });

        // Stage 2: machine / filament / process presets. Accumulated into a local json on this
        // worker thread and merged on the UI thread, so it never races the picker reading
        // m_ProfileJson. Pages that need filaments (22/23) ask with need_detail and are answered
        // from OnScriptMessage once this lands.
        json detail;
        detail["machine"]  = json::object();
        detail["filament"] = json::object();
        detail["process"]  = json::array();

        for (const auto &vendor : vendor_list) {
            LoadProfileFamily(vendor.first, vendor.second, ProfileStage::Details, detail);
            if (m_destroy)
                return 0;
        }

        {
            std::lock_guard<std::mutex> lock(m_detail_mutex);
            m_detail_json  = std::move(detail);
            m_detail_ready = true;
        }

        wxGetApp().CallAfter([this] {
            if (m_destroy)
                return;

            MergeDetailData();

            // a filament page asked before the data existed -- answer it now
            if (m_detail_request_pending) {
                m_detail_request_pending = false;
                SendProfileToWeb();
            }

            json m_Res           = json::object();
            m_Res["command"]     = "userguide_profile_detail_load_finish";
            m_Res["sequence_id"] = "10001";

            RunScript(wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', true)));
        });
    } catch (std::exception& e) {
        // wxLogMessage("GUIDE: load_profile_error  %s ", e.what());
        //  wxMessageBox(e.what(), "", MB_OK);
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ", error: " << e.what() << std::endl;
    }

    filament_info_cache.clear();
    return 0;
}

void GuideFrame::ApplyInstalledFilamentSelection()
{
    const auto enabled_filaments = wxGetApp().app_config->has_section(AppConfig::SECTION_FILAMENTS) ?
                                       wxGetApp().app_config->get_section(AppConfig::SECTION_FILAMENTS) :
                                       std::map<std::string, std::string>();

    for (auto it = m_ProfileJson["filament"].begin(); it != m_ProfileJson["filament"].end(); ++it)
        if (enabled_filaments.find(it.key()) != enabled_filaments.end())
            m_ProfileJson["filament"][it.key()]["selected"] = 1;
}

int GuideFrame::SaveProfileData()
{
    try {
        const auto enabled_filaments = wxGetApp().app_config->has_section(AppConfig::SECTION_FILAMENTS) ? wxGetApp().app_config->get_section(AppConfig::SECTION_FILAMENTS) : std::map<std::string, std::string>();
        m_appconfig_new.set_vendors(*wxGetApp().app_config);
        m_appconfig_new.set_section(AppConfig::SECTION_FILAMENTS, enabled_filaments);

        for (auto it = m_ProfileJson["model"].begin(); it != m_ProfileJson["model"].end(); ++it)
        {
            if (it.value().is_object()) {
                json& temp_model = it.value();
                std::string model_name = temp_model["model"];
                std::string vendor_name = temp_model["vendor"];
                std::string nozzle_diameter = temp_model["nozzle_diameter"];
                std::string selected;
                boost::trim(nozzle_diameter);
                std::string nozzle;
                bool enabled = false, first=true;
                while (nozzle_diameter.size() > 0) {
                    auto pos = nozzle_diameter.find(';');
                    if (pos != std::string::npos) {
                        nozzle   = nozzle_diameter.substr(0, pos);
                        enabled = m_appconfig_new.get_variant(vendor_name, model_name, nozzle);
                        if (enabled) {
                            if (!first)
                                selected += ";";
                            selected += nozzle;
                            first = false;
                        }
                        nozzle_diameter = nozzle_diameter.substr(pos + 1);
                        boost::trim(nozzle_diameter);
                    }
                    else {
                        enabled = m_appconfig_new.get_variant(vendor_name, model_name, nozzle_diameter);
                        if (enabled) {
                            if (!first)
                                selected += ";";
                            selected += nozzle_diameter;
                        }
                        break;
                    }
                }
                temp_model["nozzle_selected"] = selected;
                //m_ProfileJson["model"][a]["nozzle_selected"]
            }
        }

        if (m_ProfileJson["model"].size() == 1) {
            std::string strNozzle = m_ProfileJson["model"][0]["nozzle_diameter"];
            m_ProfileJson["model"][0]["nozzle_selected"]=strNozzle;
        }


        ApplyInstalledFilamentSelection();

        //----region
        m_Region = wxGetApp().app_config->get("region");
        m_ProfileJson["region"] = m_Region;

        // Unlayered: always off - the network plug-in cannot be installed, so the wizard must not
        // present it as a choice.
        m_ProfileJson["network_plugin_install"] = "0";
        m_ProfileJson["network_plugin_compability"] = wxGetApp().is_compatibility_version() ? "1" : "0";
        network_plugin_ready = wxGetApp().is_compatibility_version();

        StealthMode = wxGetApp().app_config->get_bool("app","stealth_mode");
        m_ProfileJson["stealth_mode"] = StealthMode;
    }
    catch (std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ", error: "<< e.what() <<std::endl;
    }

    return 0;
}

void StringReplace(string &strBase, string strSrc, string strDes)
{
    string::size_type pos    = 0;
    string::size_type srcLen = strSrc.size();
    string::size_type desLen = strDes.size();
    pos                      = strBase.find(strSrc, pos);
    while ((pos != string::npos)) {
        strBase.replace(pos, srcLen, strDes);
        pos = strBase.find(strSrc, (pos + desLen));
    }
}


int GuideFrame::LoadProfileFamily(std::string strVendor, std::string strFilePath, ProfileStage stage, json &out)
{
    // wxString strFolder = strFilePath.BeforeLast(boost::filesystem::path::preferred_separator);
    boost::filesystem::path file_path(strFilePath);
    boost::filesystem::path vendor_dir = boost::filesystem::absolute(file_path.parent_path() / strVendor).make_preferred();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  vendor path %1%.") % vendor_dir.string();
    try {
        // wxLogMessage("GUIDE: json_path1  %s", w2s(strFilePath));

        std::string contents;
        LoadFile(strFilePath, contents);
        // wxLogMessage("GUIDE: json_path1 content: %s", contents);
        json jLocal = json::parse(contents);
        // wxLogMessage("GUIDE: json_path1 Loaded");

        // BBS:models
        // Stage Models stops after this loop -- it is all the printer picker (guide pages 21/24/31)
        // needs, and it is ~450 files against ~11500 for the machine/filament/process lists below.
        json pmodels = json::array();
        if (stage == ProfileStage::Models)
            pmodels = jLocal["machine_model_list"];
        int  nsize   = pmodels.size();

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% machine models") % nsize;

        for (int n = 0; n < nsize; n++) {
            json OneModel = pmodels.at(n);

            OneModel["model"] = OneModel["name"];
            OneModel.erase("name");

            std::string s1 = OneModel["model"];
            std::string s2 = OneModel["sub_path"];

            boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
            if (!boost::filesystem::exists(sub_path)) continue;

            std::string             sub_file = sub_path.string();

            // wxLogMessage("GUIDE: json_path2  %s", w2s(ModelFilePath));
            LoadFile(sub_file, contents);
            // wxLogMessage("GUIDE: json_path2 content: %s", contents);
            json pm = json::parse(contents);
            // wxLogMessage("GUIDE: json_path2  loaded");

            OneModel["name"]      = pm["name"];
            OneModel["vendor"]    = strVendor;
            std::string NozzleOpt = pm["nozzle_diameter"];
            StringReplace(NozzleOpt, " ", "");
            OneModel["nozzle_diameter"] = NozzleOpt;
            OneModel["materials"]       = pm["default_materials"];

            // wxString strCoverPath = wxString::Format("%s\\%s\\%s_cover.png", strFolder, strVendor, std::string(s1.mb_str()));
            std::string             cover_file = s1 + "_cover.png";
            boost::filesystem::path cover_path = boost::filesystem::absolute(boost::filesystem::path(resources_dir()) / "/profiles/" / strVendor / cover_file).make_preferred();
            if (!boost::filesystem::exists(cover_path)) {
                cover_path =
                    (boost::filesystem::absolute(boost::filesystem::path(resources_dir()) / "/web/image/printer/") /
                     cover_file)
                        .make_preferred();
            }
            OneModel["cover"]                  = cover_path.string();

            OneModel["nozzle_selected"] = "";

            out["model"].push_back(OneModel);
        }

        if (stage == ProfileStage::Models)
            return 0;

        // BBS:Machine
        json pmachine = jLocal["machine_list"];
        nsize         = pmachine.size();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% machines") % nsize;
        for (int n = 0; n < nsize; n++) {
            json OneMachine = pmachine.at(n);

            std::string s1 = OneMachine["name"];
            std::string s2 = OneMachine["sub_path"];

            // wxString ModelFilePath = wxString::Format("%s\\%s\\%s", strFolder, strVendor, s2);
            boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
            if (!boost::filesystem::exists(sub_path)) continue;

            std::string             sub_file = sub_path.string();
            LoadFile(sub_file, contents);
            json pm = json::parse(contents);

            std::string strInstant = pm["instantiation"];
            if (strInstant.compare("true") == 0) {
                OneMachine["model"] = pm["printer_model"];
                OneMachine["nozzle"] = pm["nozzle_diameter"][0];

                out["machine"][s1]=OneMachine;
            }
        }

        // BBS:Filament
        json pFilament = jLocal["filament_list"];
        json tFilaList = m_OrcaFilaList;
        nsize          = pFilament.size();

        for (int n = 0; n < nsize; n++) {
            json OneFF = pFilament.at(n);

            std::string s1    = OneFF["name"];
            std::string s2    = OneFF["sub_path"];

            tFilaList[s1] = OneFF;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Vendor: " << strVendor <<", tFilaList Add: " << s1;
        }

        int nFalse  = 0;
        int nModel  = 0;
        int nFinish = 0;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% filaments") % nsize;
        for (int n = 0; n < nsize; n++) {
            json OneFF = pFilament.at(n);

            std::string s1 = OneFF["name"];
            std::string s2 = OneFF["sub_path"];

            if (!out["filament"].contains(s1)) {
                // wxString ModelFilePath = wxString::Format("%s\\%s\\%s", strFolder, strVendor, s2);
                boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
                if (!boost::filesystem::exists(sub_path)) continue;

                std::string             sub_file = sub_path.string();
                LoadFile(sub_file, contents);
                json pm = json::parse(contents);

                std::string strInstant = pm["instantiation"];
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Load Filament:" << s1 << ",Path:" << sub_file << ",instantiation?" << strInstant;

                if (strInstant == "true") {
                    std::string sV;
                    std::string sT;

                    int nRet = GetFilamentInfo(vendor_dir.string(),tFilaList, sub_file, sV, sT);
                    if (nRet != 0) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Load Filament:" << s1 << ",GetFilamentInfo Failed, Vendor:" << sV << ",Type:"<< sT;
                        continue;
                    }

                    OneFF["vendor"] = sV;
                    OneFF["type"]   = sT;

                    OneFF["models"]   = "";

                    json pPrinters = pm["compatible_printers"];
                    int nPrinter   = pPrinters.size();
                    std::string ModelList = "";
                    for (int i = 0; i < nPrinter; i++)
                    {
                        std::string sP = pPrinters.at(i);
                        if (out["machine"].contains(sP))
                        {
                            std::string mModel = out["machine"][sP]["model"];
                            std::string mNozzle = out["machine"][sP]["nozzle"];
                            std::string NewModel = mModel + "++" + mNozzle;

                            ModelList = (boost::format("%1%[%2%]") % ModelList % NewModel).str();
                        }
                    }

                    OneFF["models"]    = ModelList;
                    OneFF["selected"] = 0;

                    out["filament"][s1] = OneFF;
                } else
                    continue;

            }
        }
        if(strVendor == PresetBundle::ORCA_FILAMENT_LIBRARY)
            m_OrcaFilaList = tFilaList;

        // process
        json pProcess = jLocal["process_list"];
        nsize         = pProcess.size();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% processes") % nsize;
        for (int n = 0; n < nsize; n++) {
            json OneProcess = pProcess.at(n);

            std::string s2 = OneProcess["sub_path"];
            // wxString ModelFilePath = wxString::Format("%s\\%s\\%s", strFolder, strVendor, s2);
            boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
            if (!boost::filesystem::exists(sub_path)) continue;

            std::string             sub_file = sub_path.string();
            LoadFile(sub_file, contents);
            json pm = json::parse(contents);

            std::string bInstall = pm["instantiation"];
            if (bInstall == "true") { out["process"].push_back(OneProcess); }
        }

    } catch (nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << strFilePath << " got a nlohmann::detail::parse_error, reason = " << err.what();
        return -1;
    } catch (std::exception &e) {
        // wxMessageBox(e.what(), "", MB_OK);
        // wxLogMessage("GUIDE: LoadFamily Error: %s", e.what());
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << strFilePath << " got exception: " << e.what();
        return -1;
    }

    return 0;
}



void GuideFrame::StrReplace(std::string &strBase, std::string strSrc, std::string strDes)
{
    int pos    = 0;
    int srcLen = strSrc.size();
    int desLen = strDes.size();
    pos = strBase.find(strSrc, pos);
    while ((pos != std::string::npos)) {
        strBase.replace(pos, srcLen, strDes);
        pos = strBase.find(strSrc, (pos + desLen));
    }
}

std::string GuideFrame::w2s(wxString sSrc)
{
    return std::string(sSrc.mb_str());
}

void GuideFrame::GetStardardFilePath(std::string &FilePath) {
    StrReplace(FilePath, "\\", w2s(wxString::Format("%c", boost::filesystem::path::preferred_separator)));
    StrReplace(FilePath, "/" , w2s(wxString::Format("%c", boost::filesystem::path::preferred_separator)));
}

bool GuideFrame::LoadFile(std::string jPath, std::string &sContent)
{
    try {
        boost::nowide::ifstream t(jPath);
        std::stringstream buffer;
        buffer << t.rdbuf();
        sContent=buffer.str();
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << boost::format(", load %1% into buffer")% jPath;
    }
    catch (std::exception &e)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",  got exception: "<<e.what();
        return false;
    }

    return true;
}

int GuideFrame::DownloadPlugin()
{
    return wxGetApp().download_plugin(
        "plugins", "network_plugin.zip",
        [this](int status, int percent, bool& cancel) {
            return ShowPluginStatus(status, percent, cancel);
        }
    , nullptr);
}

int GuideFrame::InstallPlugin()
{
    return wxGetApp().install_plugin("plugins", "network_plugin.zip",
        [this](int status, int percent, bool &cancel) {
            return ShowPluginStatus(status, percent, cancel);
        }
    );
}

int GuideFrame::ShowPluginStatus(int status, int percent, bool& cancel)
{
    //TODO
    return 0;
}



}} // namespace Slic3r::GUI
