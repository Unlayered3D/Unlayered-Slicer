#ifndef slic3r_MainFrame_hpp_
#define slic3r_MainFrame_hpp_

#include "libslic3r/PrintConfig.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <wx/frame.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <wx/filehistory.h>
#ifdef __APPLE__
#include <wx/taskbar.h>
#endif // __APPLE__

#include <string>
#include <map>
#include <set>
#include <vector>

#include "GUI_Utils.hpp"
#include "Event.hpp"
//BBS: GUI refactor
#include "ParamsPanel.hpp"
#include "Monitor.hpp"
#include "Auxiliary.hpp"
#include "Project.hpp"
#include "CalibrationPanel.hpp"
#include "UnsavedChangesDialog.hpp"
#include "Widgets/SideButton.hpp"
#include "Widgets/SideMenuPopup.hpp"
#include "FilamentGroupPopup.hpp"


#include <boost/property_tree/ptree_fwd.hpp>

// BBS
#include "BBLTopbar.hpp"
#include "PrinterWebView.hpp"
#include "calib_dlg.hpp"
#include "MultiMachinePage.hpp"
#include "SlicePrepare.hpp" // complete type for the unique_ptr: ~MainFrame is defaulted here
#include "slic3r/plugin/host/PluginPages.hpp"

// Stable identifiers for MainFrame::m_tabpanel's built-in pages. These are
// names rather than positional indices so optional pages cannot shift them.
#define TAB_ID_HOME          "home"
#define TAB_ID_PREPARE       "prepare"
#define TAB_ID_PREVIEW       "preview"
#define TAB_ID_MONITOR       "monitor"
// Printer-agents mode shows the legacy web page alongside the native Device tab, so it needs an
// id of its own: sharing TAB_ID_MONITOR makes every name lookup resolve to whichever of the two
// comes first, which silently defeats PluginPages' selection round-trip across a tab relayout.
#define TAB_ID_MONITOR_WEB   "monitor_web"
#define TAB_ID_MULTI_DEVICE  "multi_device"
#define TAB_ID_PROJECT       "project"
#define TAB_ID_CALIBRATION   "calibration"
// Unlayered: optional trailing tab hosting an embedded web application (WebAppPanel).
#define TAB_ID_WEB_APP       "web_app"
// Unlayered: the third WebAppPanel slot, home of the Unlayered Universe tab. (The second slot,
// the side panel, is not a tab: it is the sidebar beside the notebook, m_side_panel_view.)
#define TAB_ID_UNIVERSE      "universe_app"

#define ENABEL_PRINT_ALL 0

class Notebook;
class wxBookCtrlBase;
class wxProgressDialog;

namespace Slic3r {

namespace GUI
{

class Tab;
class PrintHostQueueDialog;
class Plater;
class MainFrame;
class ParamsDialog;
class WebAppPanel;
class SidebarSash;
struct WebAppSlot;
class SlicePrepare;
#ifdef __WXGTK__
class ResizeEdgePanel;
#endif

enum QuickSlice
{
    qsUndef = 0,
    qsReslice = 1,
    qsSaveAs = 2,
    qsExportSVG = 4,
    qsExportPNG = 8
};

struct PresetTab {
    std::string       name;
    Tab*              panel;
    PrinterTechnology technology;
};

// ----------------------------------------------------------------------------
// SettingsDialog
// ----------------------------------------------------------------------------

class SettingsDialog : public DPIDialog//DPIDialog
{
    //wxNotebook* m_tabpanel { nullptr };
    Notebook* m_tabpanel{ nullptr };
    wxMenuBar*      m_menubar{ nullptr };
public:
    SettingsDialog(MainFrame* mainframe);
    ~SettingsDialog() = default;
    //void set_tabpanel(wxNotebook* tabpanel) { m_tabpanel = tabpanel; }
    void set_tabpanel(Notebook* tabpanel) { m_tabpanel = tabpanel; }
    wxMenuBar* menubar() { return m_menubar; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
};

class MainFrame : public DPIFrame
{
#ifdef __APPLE__
    bool     m_mac_fullscreen{false};
#endif
    bool     m_loaded {false};
    wxTimer* m_reset_title_text_colour_timer{ nullptr };

    wxString    m_qs_last_input_file = wxEmptyString;
    wxString    m_qs_last_output_file = wxEmptyString;
    wxString    m_last_config = wxEmptyString;

    wxMenuBar*  m_menubar{ nullptr };
    //wxMenu* publishMenu{ nullptr };
    wxMenu *    m_calib_menu{nullptr};
    bool        enable_multi_machine{ false };

#if 0
    wxMenuItem* m_menu_item_repeat { nullptr }; // doesn't used now
#endif
    wxMenuItem* m_menu_item_reslice_now { nullptr };
    wxSizer*    m_main_sizer{ nullptr };

    wxString    m_last_selected_tab;

    std::string     get_base_name(const wxString &full_name, const char *extension = nullptr) const;
    std::string     get_dir_name(const wxString &full_name) const;

    void on_presets_changed(SimpleEvent&);
    void on_value_changed(wxCommandEvent&);

    bool can_start_new_project() const;
    bool can_open_project() const;
    bool can_add_models() const;
    bool can_export_model() const;
    bool can_export_toolpaths() const;
    bool can_export_supports() const;
    bool can_export_gcode() const;
    bool can_export_all_gcode() const;
    bool can_print_3mf() const;
    bool can_send_gcode() const;
    //bool can_export_gcode_sd() const;
    //bool can_eject() const;
    bool can_slice() const;
    bool can_change_view() const;
    bool can_select() const;
    bool can_deselect() const;
    bool can_clone() const;
    bool can_delete() const;
    bool can_delete_all() const;
    bool can_reslice() const;
    void bind_diff_dialog();

    // BBS
    wxBoxSizer* create_side_tools();

    // MenuBar items changeable in respect to printer technology
    enum MenuItems
    {                   //   FFF                  SLA
        miExport = 0,   // Export G-code        Export
        miSend,         // Send G-code          Send to print
        miMaterialTab,  // Filament Settings    Material Settings
        miPrinterTab,   // Different bitmap for Printer Settings
    };

    // vector of a MenuBar items changeable in respect to printer technology
    std::vector<wxMenuItem*> m_changeable_menu_items;

    struct FileHistory : wxFileHistory
    {
        FileHistory(int max) : wxFileHistory(max) {}
        ~FileHistory();
        std::wstring GetThumbnailUrl(int index) const;
        bool        GetPublished(int index) const;

        virtual void AddFileToHistory(const wxString &file);
        virtual void RemoveFileFromHistory(size_t i);
        size_t FindFileInHistory(const wxString &file);

        // Reads the thumbnails off the main thread, then calls on_loaded on it. Opening a project
        // file can block for as long as macOS shows a folder-access prompt, and the frame must not
        // wait for that: until then the entries have no picture.
        void LoadThumbnails(std::function<void()> on_loaded);

        void SetMaxFiles(int max);
    private:
        std::deque<std::string> m_thumbnails;
        std::deque<bool>        m_published_files; // parallel to m_thumbnails: is it a published 3mf?
        bool m_load_called = false;
        struct Pending; // shared with the thumbnail thread; see LoadThumbnails
        std::shared_ptr<Pending> m_pending;
    };

    FileHistory m_recent_projects;

    // Unlayered 2026-10-22: other slicers' recent projects on the home page, with a filter by
    // slicer. Read once per session (OtherSlicers::recent_projects); thumbnails as for our own.
    struct OtherRecent
    {
        std::string app;   // OtherSlicers::app_name(), what the filter shows
        std::string path;
        std::string thumbnail; // png bytes from the 3mf, "" for none
    };
    std::vector<OtherRecent> m_other_recent;
    bool                     m_other_recent_loaded = false;
    void                     load_other_recent();
    // Their thumbnails are read off the main thread (load_other_recent). The token tells that thread
    // whether this frame is still there; declared after m_other_recent so it goes first.
    struct OtherRecentToken
    {
        std::mutex mutex;
        bool       alive = true;
    };
    struct OtherRecentGuard
    {
        std::shared_ptr<OtherRecentToken> token = std::make_shared<OtherRecentToken>();
        ~OtherRecentGuard()
        {
            std::lock_guard<std::mutex> lock(token->mutex);
            token->alive = false;
        }
    } m_other_recent_guard;
    // Hidden by "Clear all" or a delete on the home page: app config "other_slicer_recent_hidden".
    std::set<std::string> other_recent_hidden() const;
    void                  hide_other_recent(const std::vector<std::string> &paths);

    enum class ESettingsLayout
    {
        //BBS GUI refactor: remove unused layout
        Unknown,
        Old,
        //New,
        //Dlg,
        GCodeViewer
    };

    ESettingsLayout m_layout{ ESettingsLayout::Unknown };

    enum SliceSelectType
    {
        eSliceAll = 0,
        eSlicePlate = 1,
        // Hand the plate to the external nonplanar slicing service instead of slicing locally
        // (SLICE_SERVICE_PROTOCOL.md). Offered only when a service address is configured.
        eSliceNonplanar = 2,
    };

    //jump to editor under preview only mode
    bool preview_only_to_editor = false;

protected:
    virtual void on_dpi_changed(const wxRect &suggested_rect) override;
    virtual void on_sys_color_changed() override;

#ifdef __WXMSW__
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

public:
    MainFrame();
    ~MainFrame() = default;
#ifdef __APPLE__
    bool get_mac_full_screen() { return m_mac_fullscreen; }
#endif

    //BBS: add slice&&print status update logic
    enum SlicePrintEventType
    {
        eEventObjectUpdate = 0,
        eEventPlateUpdate = 1,
        eEventParamUpdate = 2,
        eEventSliceUpdate = 3,
        eEventPrintUpdate = 4
    };

    // BBS GUI refactor
    enum PrintSelectType {
        ePrintAll            = 0,
        ePrintPlate          = 1,
        eExportSlicedFile    = 2,
        eExportGcode         = 3,
        eSendGcode           = 4,
        eSendToPrinter       = 5,
        eSendToPrinterAll    = 6,
        eUploadGcode         = 7,
        eExportAllSlicedFile = 8,
        ePrintMultiMachine   = 9
    };

    void update_layout();

	// Called when closing the application and when switching the application language.
	void 		shutdown();

    Plater*     plater() { return m_plater; }

    // BBS
    BBLTopbar* topbar() { return m_topbar; }

    // for cali to update tab when save new preset
    void update_filament_tab_ui();

    void        update_title();
    void        set_max_recent_count(int max);

    void        show_publish_button(bool show);

	void        update_title_colour_after_set_title();
    void        show_option(bool show);
    void        init_tabpanel();
    void        create_preset_tabs();
    //BBS: GUI refactor
    void        add_created_tab(Tab* panel, const std::string& bmp_name = "");
    bool        is_active_and_shown_tab(wxPanel* panel);

    // Register Win32 RawInput callbacks (3DConnexion) and removable media insert / remove callbacks.
    // Called from wxEVT_ACTIVATE, as wxEVT_CREATE was not reliable (bug in wxWidgets?).
    void        register_win32_callbacks();
    void        init_menubar_as_editor();
    void        init_menubar_as_gcodeviewer();
    void        update_menubar();
    // Open item in menu by menu and item name (in actual language)
    void        open_menubar_item(const wxString& menu_name,const wxString& item_name);
#ifdef _WIN32
    void        show_tabs_menu(bool show);
#endif
    //BBS
    void        show_log_window();

    void        update_ui_from_settings();
    //BBS
    void        show_sync_dialog();
    void        update_side_preset_ui();
    void        on_select_default_preset(SimpleEvent& evt);

    bool        is_loaded() const { return m_loaded; }
    bool        is_last_input_file() const  { return !m_qs_last_input_file.IsEmpty(); }
    //BBS GUI refactor: remove unused layout new/dlg
    //bool        is_dlg_layout() const { return m_layout == ESettingsLayout::Dlg; }

    void        reslice_now();
    void        export_config();
    // Query user for the config file and open it.
    void        load_config_file();
    // Open a config file. Return true if loaded.
    bool        load_config_file(const std::string &path);

    //BBS: export current config bundle as BBL default reference
    //void        export_current_configbundle();
    //BBS: export all the system preset configs to seperate files
    //void        export_system_configs();
    //void        export_configbundle(bool export_physical_printers = false);
    //void        load_configbundle(wxString file = wxEmptyString);
    void        load_config(const DynamicPrintConfig& config);
    //BBS: jump to monitor
    void        jump_to_monitor(std::string dev_id = "");
    void        jump_to_multipage();
    //BBS: hint when jump to 3Deditor under preview only mode
    bool        preview_only_hint();
    // Select tab in m_tabpanel
    // When tab == -1, will be selected last selected tab
    //BBS: GUI refactor
    // "Slice with <web-app tab title>": hand the current plate to the web-app tab and show it.
    void        hand_off_to_web_app();
    wxString    slice_with_label() const;
    void        select_tab(wxPanel* panel);
    void        select_tab(const wxString& id = wxString());
    void        request_select_tab(const wxString& id);
    // Re-label and reload an embedded web application tab after its preferences change. The
    // no-argument form is the generic slot.
    void        refresh_web_app();
    void        refresh_web_app(const WebAppSlot& slot);
    // Resolve a slot to the panel hosting it and the notebook page name it was added under.
    // One place knows the mapping, so adding a fourth slot is a single entry rather than another
    // ternary in every caller. False when the slot is unknown; `panel` is still null when the
    // slot is known but its tab is disabled, which callers must treat as "nothing to do".
    // `tab_id` is null for the side panel slot, which is the sidebar and not a page.
    bool        web_app_slot_view(const WebAppSlot& slot, WebAppPanel*& panel, const char*& tab_id);
    // Unlayered: the side panel -- a WebAppPanel hosted beside the notebook, visible in every
    // tab, collapsible and resizable (see init_tabpanel / update_layout). State is persisted in
    // SETTING_SIDE_PANEL_SIDEBAR_OPEN / _WIDTH.
    void        toggle_side_panel();
    void        toggle_side_panel(bool open);
    bool        is_side_panel_shown() const;
    int         get_calibration_curr_tab();
    void        select_view(const std::string& direction);
    // Propagate changed configuration from the Tab to the Plater and save changes to the AppConfig
    void        on_config_changed(DynamicPrintConfig* cfg) const ;
    void        set_print_button_to_default(PrintSelectType select_type);

    bool can_save() const;
    bool can_save_as() const;
    //BBS
    bool can_upload() const;
    void save_project();
    bool save_project_as(const wxString& filename = wxString());
    // Open the Publish dialog and export the selected settings as a published 3MF.
    void publish_project();

    void        add_to_recent_projects(const wxString& filename);
    void        get_recent_projects(boost::property_tree::wptree &tree, int images);
    void        open_recent_project(size_t file_id, wxString const & filename);
    void        remove_recent_project(size_t file_id, wxString const &filename);

    void        technology_changed();


    //BBS
    void        load_url(wxString url);
    void        load_printer_url(wxString url, wxString apikey = "");
    void        load_printer_url();
    bool        is_printer_view() const;
    void        refresh_plugin_tips();
    void RunScript(wxString js);
    void retheme_home_page(); // the theme's colours onto the home page (WebViewPanel::apply_theme_style)

    //SoftFever
    void show_device(bool should_use_native);
    void fit_tab_labels(); // ORCA
    // True while either of the two tabs backed by m_plater is selected.
    bool is_prepare_or_preview_tab() const;
    // Ctrl+Tab / Ctrl+Shift+Tab: step m_tabpanel by `direction` (+1 / -1), wrapping.
    void select_adjacent_tab(int direction);
    // Side panel internals (toggle_side_panel is the public entry).
    void   apply_side_panel_state(bool layout);
    void   on_side_panel_sash_drag(int dx);
    int    clamp_side_panel_width_dip(int width_dip) const;
    void   persist_side_panel_width();
    // The frame's minimum size, widened while the side panel is open so the 3D canvas can
    // never be squeezed to nothing between the two sidebars.
    wxSize frame_min_size() const;
    PluginPages& plugin_pages() { return m_plugin_pages; }

    PA_Calibration_Dlg* m_pa_calib_dlg{ nullptr };
    FlowRateCalibrationDialog* m_flow_rate_calib_dlg{ nullptr };
    Temp_Calibration_Dlg* m_temp_calib_dlg{ nullptr };
    MaxVolumetricSpeed_Test_Dlg* m_vol_test_dlg { nullptr };
    VFA_Test_Dlg* m_vfa_test_dlg { nullptr };
    Retraction_Test_Dlg* m_retraction_calib_dlg{ nullptr };
    Input_Shaping_Freq_Test_Dlg* m_IS_freq_calib_dlg{ nullptr };
    Input_Shaping_Damp_Test_Dlg* m_IS_damp_calib_dlg{ nullptr };
    Cornering_Test_Dlg* m_cornering_calib_dlg{ nullptr };

    // BBS. Replace title bar and menu bar with top bar.
    BBLTopbar*            m_topbar{ nullptr };
    PrintHostQueueDialog* printhost_queue_dlg() { return m_printhost_queue_dlg; }
    Plater*               m_plater { nullptr };
    //BBS: GUI refactor
    MonitorPanel*         m_monitor{ nullptr };

    //AuxiliaryPanel*       m_auxiliary{ nullptr };
    MultiMachinePage*     m_multi_machine{ nullptr };
    ProjectPanel*         m_project{ nullptr };

    CalibrationPanel*     m_calibration{ nullptr };
    WebViewPanel*         m_webview { nullptr };
    PrinterWebView*       m_printer_view{nullptr};
    WebAppPanel*          m_web_app_view{nullptr};
    WebAppPanel*          m_side_panel_view{nullptr};     // the side panel: a frame child, not a page
    WebAppPanel*          m_universe_view{nullptr};
    // Side panel: the horizontal sizer [notebook | sash | side panel] under m_main_sizer, the drag
    // sash, the tab-bar toggle, the View-menu check item and the width (DIP, clamped live).
    wxBoxSizer*           m_content_sizer{nullptr};
    SidebarSash*          m_side_panel_sash{nullptr};
    SideButton*           m_side_panel_toggle_btn{nullptr};
    wxMenuItem*           m_side_panel_menu_item{nullptr};
    int                   m_side_panel_width_dip{380};
    PluginPages           m_plugin_pages;
    wxLogWindow*          m_log_window { nullptr };
    // BBS
    //wxBookCtrlBase*       m_tabpanel { nullptr };
    Notebook*             m_tabpanel{ nullptr };
    wxBoxSizer*           m_side_tools{ nullptr };
    ParamsPanel*          m_param_panel{ nullptr };
    ParamsDialog*         m_param_dialog{ nullptr };
    //BBS
    SettingsDialog        m_settings_dialog;
    DiffPresetDialog      diff_dialog;
    wxWindow*             m_plater_page{ nullptr };
    PrintHostQueueDialog* m_printhost_queue_dlg;

    
    mutable int m_print_select{ ePrintAll };
    mutable int m_slice_select{ eSliceAll };
    // Button* m_publish_btn{ nullptr };
    SideButton* m_slice_btn{ nullptr };
    SideButton* m_slice_option_btn{ nullptr };
    SideButton* m_print_btn{ nullptr };
    SideButton* m_print_option_btn{ nullptr };

    SidePopup*  m_slice_option_pop_up{ nullptr };
    // Prepares the plate on the slicing service while a service mode is selected (SlicePrepare).
    std::unique_ptr<SlicePrepare> m_slice_prepare;
    void set_slice_prepare_active(bool active);
    // A service mode preselected by automation (select_slice_mode): its background prepare upload
    // waits for the person to act on it (activate_deferred_slice_prepare), so automation never
    // starts slicing-service work (decision D7).
    bool m_slice_prepare_deferred{ false };
    // What picking a Slice dropdown entry does to the button and the slicing state. `by_person`
    // false (automation) leaves a service mode's background prepare deferred.
    void apply_slice_mode(int select, const wxString& label, const std::string& mode = std::string(), bool by_person = true);

    FilamentGroupPopup* m_filament_group_popup{ nullptr };
    mutable bool          m_slice_enable{ true };
    mutable bool          m_print_enable{ true };
    bool get_enable_slice_status();
    bool get_enable_print_status();
    // A slicing-service mode is selected in the Slice dropdown: Plater::reslice() slices the plate
    // there instead of here.
    bool slice_mode_is_service() const { return m_slice_select == eSliceNonplanar; }
    // The Slice dropdown's slicing modes, for the web-app bridge (slicer.getSliceModes): "local"
    // (Slice plate), "local_all" (Slice all), then one per layer mode the slicing service offers
    // on this printer -- its own value from the service's form, labelled as the dropdown labels it
    // -- or a single "service" entry while no form is cached. None without a configured service.
    struct SliceModeChoice
    {
        std::string id;
        wxString    label;
        bool        service = false;
    };
    std::vector<SliceModeChoice> slice_mode_choices() const;
    std::string                  selected_slice_mode() const;
    // Selects a dropdown entry by id exactly as picking it does, minus every dialog, and slices
    // nothing. "" on success, else "unknown_mode".
    // A service mode picked this way does not start the background prepare upload (SlicePrepare):
    // that waits until the person presses Slice or picks the mode themselves (decision D7).
    std::string select_slice_mode(const std::string& id);
    // The person asked to slice (Plater::note_user_slice_request): a service mode preselected by
    // automation now prepares in the background as if the person had picked it.
    void activate_deferred_slice_prepare();
    // The slicing service's options changed (the slicing-service options dialog, or slice.setOptions): while a
    // service mode is selected the button takes the (possibly new) mode's name and Slice is available
    // again (the plate's service result is now out of date). A mode changed by automation
    // (`by_person` false) defers the background prepare again, as select_slice_mode does.
    void slice_service_options_changed(bool by_person, bool mode_changed = false);
    // A service mode is selected and its background prepare is waiting for the person (see above).
    bool slice_prepare_deferred() const { return m_slice_prepare_deferred && m_slice_select == eSliceNonplanar; }
    // The Print button, for the web-app bridge (slicer.pressPrint): its label now (Print plate,
    // Print all, Send, Export G-code ... whatever its dropdown is set to), whether pressing it
    // would do anything, and pressing it exactly as the person does. Every choice opens the
    // slicer's own window (print / send / save dialog), so nothing leaves until the person
    // confirms there. `wait` runs that window before returning (it is modal), for a caller that
    // reports how it was closed; the button itself posts.
    wxString print_button_label() const;
    bool     can_press_print();
    void     press_print_button(bool wait);
    //BBS
    void update_side_button_style();
    void update_slice_print_status(SlicePrintEventType event, bool can_slice = true, bool can_print = true);

#ifdef __APPLE__
    std::unique_ptr<wxTaskBarIcon> m_taskbar_icon;
#endif // __APPLE__

#ifdef _WIN32
    void*				m_hDeviceNotify { nullptr };
    uint32_t  			m_ulSHChangeNotifyRegister { 0 };
	static constexpr int WM_USER_MEDIACHANGED { 0x7FFF }; // WM_USER from 0x0400 to 0x7FFF, picking the last one to not interfere with wxWidgets allocation
#endif // _WIN32

#ifdef __WXGTK__
    friend class ResizeEdgePanel;
    ResizeEdgePanel* m_edge_bottom{nullptr};
    ResizeEdgePanel* m_edge_left{nullptr};
    ResizeEdgePanel* m_edge_right{nullptr};
    void update_edge_panels();
#endif // __WXGTK__
};

wxDECLARE_EVENT(EVT_HTTP_ERROR, wxCommandEvent);
wxDECLARE_EVENT(EVT_USER_LOGIN, wxCommandEvent);
wxDECLARE_EVENT(EVT_USER_LOGIN_HANDLE, wxCommandEvent);
wxDECLARE_EVENT(EVT_CHECK_PRIVACY_VER, wxCommandEvent);
wxDECLARE_EVENT(EVT_CHECK_PRIVACY_SHOW, wxCommandEvent);
wxDECLARE_EVENT(EVT_SHOW_IP_DIALOG, wxCommandEvent);
wxDECLARE_EVENT(EVT_UPDATE_MACHINE_LIST, wxCommandEvent);
wxDECLARE_EVENT(EVT_UPDATE_PRESET_CB, SimpleEvent);

// Unlayered: installs, from the profiles shipped with the application, the vendors the given preset
// parents ((type subdir, preset name) pairs) come from, enables their printer models as the setup
// wizard would, and reloads the presets. `ask`: ask before installing each vendor (File > Import >
// Import Configs); false installs without asking (the import from other slicers, already asked for).
// True when the presets were reloaded.
bool install_preset_parents_from_resources(wxWindow *parent, const std::vector<std::pair<std::string, std::string>> &parents, bool ask);


} // GUI
} //Slic3r

#endif // slic3r_MainFrame_hpp_
