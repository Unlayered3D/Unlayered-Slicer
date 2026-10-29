#ifndef slic3r_WebAppPanel_hpp_
#define slic3r_WebAppPanel_hpp_

#include "WebAppSlot.hpp"

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/webview.h>

class wxStaticText;
class wxWindow;

namespace Slic3r {
namespace GUI {

class WebAppBridge;

// A generic host for a third-party web application, embedded as a top-level tab -- or, for the
// side panel slot, as a persistent sidebar beside the tabs (MainFrame decides; see
// set_hosted_as_sidebar).
//
// The application is identified only by a URL the user configures; nothing in this class, or
// anywhere else in this source tree, is specific to a particular web application. The slicer
// exposes a documented, general-purpose JSON-RPC surface to whatever page is loaded here --
// see WEB_APP_BRIDGE.md for the protocol and resources/web/webapp-example for a reference
// client. Any page that speaks that protocol works; the slicer neither knows nor cares which.
//
// Modelled on PrinterWebView, which embeds a printer's own web interface (Duet Web Control,
// Fluidd, Mainsail) the same way.
//
// A panel is instantiated over a WebAppSlot: the AppConfig keys, environment overrides and seed
// file it is configured from (WebAppSlot.hpp). There are three slots -- the generic web
// application tab, the Universe tab and the side panel -- and everything in this class is written
// once, per slot.
class WebAppPanel : public wxPanel
{
public:
    explicit WebAppPanel(wxWindow* parent, const WebAppSlot& slot = WebAppSlot::generic());
    ~WebAppPanel() override;

    const WebAppSlot& slot() const { return m_slot; }

    // How MainFrame hosts this panel: as a notebook page (the default) or as the persistent
    // sidebar beside the tabs. Set once, right after construction; the bridge reports it to the
    // page as `tab.layout` in slicer.getInfo so one page can serve both arrangements.
    void set_hosted_as_sidebar(bool sidebar) { m_hosted_as_sidebar = sidebar; }
    bool hosted_as_sidebar() const { return m_hosted_as_sidebar; }

    // --- configuration ------------------------------------------------------
    // Precedence: environment variable > AppConfig > seed file. The environment overrides are
    // for development and are never written back to AppConfig. Each reads the given slot's keys.
    static wxString configured_url(const WebAppSlot& slot = WebAppSlot::generic());
    static wxString configured_title(const WebAppSlot& slot = WebAppSlot::generic());
    static bool     is_enabled(const WebAppSlot& slot = WebAppSlot::generic());

    // Normalised "scheme://host[:port]", lowercased, with the default port elided. Empty when
    // the URL cannot be parsed.
    static wxString origin_of(const wxString& url);
    // Extra https origins the tab may navigate to. NAVIGABLE ONLY -- never bridge-trusted; see
    // origin_is_allowed(), which stays an exact match against the configured origin.
    static std::vector<wxString> extra_origins(const WebAppSlot& slot = WebAppSlot::generic());
    static bool     is_loopback_origin(const wxString& origin);
    // https always; http only for loopback, which is a secure context by W3C definition.
    static bool     is_secure_context(const wxString& url);

    // --- lifecycle ----------------------------------------------------------
    void load_home();
    void load_url(const wxString& url);
    void reload();
    void update_mode();
    bool Show(bool show = true) override;

    // --- state consumed by WebAppBridge and by Plater ------------------------
    wxWebView*      browser() const { return m_browser; }
    const wxString& current_origin() const { return m_current_origin; }
    bool            origin_is_allowed() const;
    bool            origin_is_loopback() const;
    bool            is_showing_fallback() const { return m_showing_fallback; }
    WebAppBridge*   bridge() const { return m_bridge.get(); }
    // Whether the page in this tab may read and change print settings through the bridge. Read
    // live from AppConfig, so a Preferences toggle takes effect on the next call.
    bool            allows_settings() const;
    // Whether the page in this tab may have the slicer export the open project and read the bytes
    // back. Read live from AppConfig, like allows_settings().
    bool            allows_export() const;
    // Whether the page in this tab may offer a printer profile for installation
    // (slicer.installPrinterPreset). Read live from AppConfig, like the two above. Even when true,
    // each install still waits on the user's own confirmation in a native dialog.
    bool            allows_install() const;
    bool            allows_printer() const; // WEB_APP_BRIDGE.md 4.6

    // Deliver a bridge event to every live panel. Each bridge decides for itself whether its page
    // gets it (no session token, or a gated family, means it is dropped there). This is how
    // slicer-side state changes -- preset, settings, plate, slicing -- reach the pages.
    static void notify_all(const std::string& method, const nlohmann::json& data);

    // UNLAYERED_MOD -- which window, and which project in it (WEB_APP_BRIDGE.md 4 getInfo, 4.5).
    // Every slicer window is its own process, so the id is random per process: a page reload in
    // the same window reads the same id, a new window or a second launch a different one.
    static const std::string& instance_id();
    // Starts at 0 and counts the project switches announced in this process.
    static unsigned           project_generation();
    // A new or opened project (Plater): bumps project_generation and sends slicer.projectChanged
    // { reason, source, generation[, slot] } to every page. `slot` only for source "bridge".
    static void project_changed(const std::string& reason, const std::string& source, const std::string& slot);

private:
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void OnNavigating(wxWebViewEvent& evt);
    void OnNewWindow(wxWebViewEvent& evt);
    // Hand a URL to the system browser, at most once per URL per couple of seconds. Both
    // OnNavigating and OnNewWindow can fire for one window.open, and launching from each opened
    // two browser tabs for a single click.
    void launch_external(const wxString& url);
    void OnScriptMessage(wxWebViewEvent& evt);
    void OnWebViewRecreated(wxCommandEvent& evt);
    void OnClose(wxCloseEvent& evt);

    void show_unavailable_page(const wxString& failed_url, const wxString& reason);
    void show_runtime_missing_panel();
    // Shown while the tab is on an allow-listed third-party origin. Without it the tab is a
    // one-way trip: there is no address bar, no back button and no way to tell whose site you
    // are looking at.
    void build_offsite_bar();
    void update_offsite_bar();

    // True for URLs the webview is permitted to navigate to at all: the configured origin and
    // our own local fallback page. Everything else is vetoed and handed to the system browser.
    bool is_navigable(const wxString& url) const;

    // --- the application's own account, for a tab hosted on the account provider's origin ------
    // Such a tab signs in with a cookie on the provider's site, which the application cannot set
    // and which its native sign-in (UnlayeredAccount, the browser hand-off that updates use) never
    // gave it. So the application hands the tab a session of its own: a one-time link from the
    // provider (POST /api/slicer/web-session) that sets the cookie and redirects to where the tab
    // was going. Only ever for the configured provider's own origin (account_provider_url).
    //
    // The provider's origin when this tab's home is on it; empty otherwise (or with no provider).
    wxString account_site_origin() const;
    // First load of the home page while the application is signed in: open it through a one-time
    // link instead, so the tab starts signed in. False when that does not apply (and nothing
    // happened); true when the load is under way, by link or, if the link fails, plainly.
    bool open_signed_in(const wxString& url);
    // The provider's /login page, about to load in this tab: veto it and sign in natively instead (the
    // sign-in dialog first if the application is signed out), then follow the link to the page
    // the login would have returned to. False to let the page load.
    bool intercept_site_sign_in(const wxString& url);
    void sign_in_natively(const wxString& next, const wxString& login_url);
    // Fetches a link for `next` in the background and loads it, or `fallback_url` if that fails.
    void follow_session_link(const wxString& next, const wxString& fallback_url);

    bool       m_session_offered{false}; // open_signed_in ran once
    bool       m_session_pending{false}; // a link request is in flight
    wxLongLong m_session_link_ms{0};     // when the last link was followed
    // Cleared in the destructor; a link request finishing late checks it before touching the panel.
    std::shared_ptr<bool> m_alive{std::make_shared<bool>(true)};

    const WebAppSlot& m_slot;
    bool              m_hosted_as_sidebar{false};

    wxWebView*    m_browser{nullptr};
    wxWindow*     m_offsite_bar{nullptr};
    wxStaticText* m_offsite_label{nullptr};
    wxString      m_home_url;
    wxString   m_url_deferred;
    wxString   m_current_origin;
    bool       m_showing_fallback{false};
    // Last URL handed to the system browser, for the de-duplication in launch_external.
    wxString   m_last_external_url;
    wxLongLong m_last_external_ms{0};

    std::unique_ptr<WebAppBridge> m_bridge;

    // Every panel whose bridge exists, for notify_all. Maintained by the constructor and the
    // destructor only.
    static std::vector<WebAppPanel*> s_live;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_WebAppPanel_hpp_
