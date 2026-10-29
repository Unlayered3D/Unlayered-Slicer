#include "AutoUpdater.hpp"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <thread>

#include <wx/button.h>
#include <wx/cursor.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r_version.h"
#include "libslic3r/libslic3r.h"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "OtherSlicerImportDialog.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "ReleaseNote.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

wxString app_title_name()
{
    AutoUpdater *u = wxGetApp().auto_updater();
    const auto   p = u != nullptr ? u->account().profile() : std::nullopt;
    // profile() is empty once signed out, so a sign-out drops the label.
    if (p && !p->edition_label.empty())
        return wxString(SLIC3R_APP_FULL_NAME) + " " + wxString::FromUTF8(p->edition_label);
    return SLIC3R_APP_FULL_NAME;
}

void refresh_app_title()
{
    if (Plater *plater = wxGetApp().plater())
        plater->update_title_dirty_status();
}

namespace {

// Where the application signs in: the configured account provider (Preferences > Advanced >
// "Web apps & services"; AppConfig seeds Unlayered's site when the key is absent). Blank means no
// account at all. Read once at start-up, so a change applies after a restart.
std::string site_url_from(AppConfig *config)
{
    return config ? boost::algorithm::trim_copy(config->get(SETTING_ACCOUNT_PROVIDER_URL)) : std::string();
}

// The app config key that carries "an update to X was about to be installed" across the restart,
// so the next launch can say it worked.
constexpr const char *PENDING_UPDATE_KEY = "pending_update_version";

// The newest version in which a slicer that was not signed in was asked to sign in (after an update,
// or by the setup wizard).
constexpr const char *SIGN_IN_ASKED_KEY = "account_signin_asked_version";

// The newest version whose "What's new" has been dealt with (shown, or skipped on a first run).
constexpr const char *WHATS_NEW_KEY = "whats_new_version";
// UNLAYERED_MOD -- usage diagnostics for the updater. The version the update came FROM, and how
// handing over to the installer went, are kept here across the restart: on_app_exit() runs after
// diagnostics have shut down, so the next launch reports them (update_applied).
constexpr const char *UPDATE_FROM_KEY   = "update_from_version";
constexpr const char *UPDATE_LAUNCH_KEY = "update_launch_result";
// Where that update's installer was downloaded ("unlayered-update-<pid>/<file>" under the temp
// directory) and its sha256, so a session that ended before installing it does not cost the next
// one a second download.
constexpr const char *PENDING_INSTALLER_KEY = "pending_update_installer";
constexpr const char *PENDING_SHA256_KEY    = "pending_update_sha256";

// A download failure as a fixed category: the error text itself can hold file paths.
std::string download_failure_reason(const std::string &error, unsigned http_status)
{
    if (http_status == 401) return "not_signed_in";
    if (http_status == 403) return "no_access";
    if (http_status == 404) return "no_installer";
    if (http_status >= 500) return "server_error";
    const std::string e = boost::algorithm::to_lower_copy(error);
    if (e.find("replaced") != std::string::npos) return "replaced_mid_update";
    if (e.find("sha") != std::string::npos || e.find("checksum") != std::string::npos || e.find("hash") != std::string::npos)
        return "checksum";
    if (e.find("space") != std::string::npos || e.find("disk") != std::string::npos) return "disk";
    if (http_status == 0) return "network";
    return "other";
}

// "## 0.1.2 — 2026-10-10" -> 0.1.2. Not a version heading -> none.
boost::optional<Semver> heading_version(const std::string &line)
{
    std::string token = line.substr(3);
    const size_t end  = token.find_first_of(" \t\r");
    if (end != std::string::npos)
        token.resize(end);
    if (!token.empty() && (token.front() == 'v' || token.front() == 'V'))
        token.erase(0, 1);
    if (token.empty() || !isdigit(static_cast<unsigned char>(token.front())))
        return boost::none;
    return Semver::parse(token);
}

// ------------------------------------------------------------------------------------------------
// The browser sign-in, as a small modal dialog. The handoff (UnlayeredAccount) runs on a worker
// thread; everything that touches a window is marshalled back through the application object and
// re-checks that the dialog is still alive, because it may have been cancelled meanwhile.
// ------------------------------------------------------------------------------------------------
class SignInDialog : public DPIDialog
{
public:
    SignInDialog(wxWindow *parent, std::shared_ptr<UnlayeredAccount> account)
        : DPIDialog(parent, wxID_ANY, _L("Sign in to Unlayered"), wxDefaultPosition, wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE)
        , m_account(std::move(account))
        , m_alive(std::make_shared<std::atomic<bool>>(true))
    {
        auto *main = new wxBoxSizer(wxVERTICAL);

        auto *intro = new wxStaticText(
            this, wxID_ANY,
            _L("Your browser opens so you can sign in to your Unlayered account and confirm. "
               "Come back here when it says the slicer is signed in."));
        intro->Wrap(FromDIP(420));
        main->Add(intro, 0, wxALL, FromDIP(16));

        m_status = new wxStaticText(this, wxID_ANY, _L("Starting..."));
        main->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        m_reopen      = new wxButton(this, wxID_ANY, _L("Open the browser again"));
        m_reopen->Disable();
        m_reopen->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (!m_browser_url.empty())
                wxLaunchDefaultBrowser(wxString::FromUTF8(m_browser_url));
        });
        auto *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        buttons->Add(m_reopen, 0, wxRIGHT, FromDIP(8));
        buttons->AddStretchSpacer();
        buttons->Add(cancel);
        main->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        SetSizerAndFit(main);
        Centre(wxBOTH);
        wxGetApp().UpdateDlgDarkUI(this);

        run();
    }

    ~SignInDialog() override { *m_alive = false; }

    const std::string &error() const { return m_error; }

protected:
    void on_dpi_changed(const wxRect &) override { Fit(); Refresh(); }

private:
    void run()
    {
        auto               alive   = m_alive;
        auto               account = m_account;
        SignInDialog      *self    = this; // only dereferenced on the GUI thread, after an alive check

        std::thread([alive, account, self]() {
            auto on_gui = [alive, self](std::function<void(SignInDialog *)> fn) {
                wxGetApp().CallAfter([alive, self, fn]() {
                    if (*alive)
                        fn(self);
                });
            };

            UnlayeredAccount::Handoff handoff;
            std::string               error;
            if (!account->start_handoff(handoff, error)) {
                on_gui([error](SignInDialog *d) { d->fail(error); });
                return;
            }
            on_gui([url = handoff.browser_url](SignInDialog *d) {
                d->m_browser_url = url;
                d->m_reopen->Enable();
                d->m_status->SetLabel(_L("Waiting for you to confirm in your browser..."));
                d->Layout();
                wxLaunchDefaultBrowser(wxString::FromUTF8(url));
            });

            // The site keeps a handoff for five minutes; poll a little inside that.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(290);
            while (*alive && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!*alive)
                    return;
                switch (account->collect(handoff, error)) {
                case UnlayeredAccount::CollectResult::SignedIn:
                    on_gui([](SignInDialog *d) { d->EndModal(wxID_OK); });
                    return;
                case UnlayeredAccount::CollectResult::Failed:
                    // collect() already turns "no answer" into Pending; this is the site refusing.
                    on_gui([error](SignInDialog *d) { d->fail(error); });
                    return;
                case UnlayeredAccount::CollectResult::Pending: break;
                }
            }
            on_gui([](SignInDialog *d) {
                d->fail(_u8L("The sign-in request expired. Close this window and try again."));
            });
        }).detach();
    }

    void fail(const std::string &error)
    {
        m_error = error;
        m_status->SetLabel(wxString::Format(_L("Sign-in did not complete: %s"), wxString::FromUTF8(error)));
        m_status->Wrap(FromDIP(420));
        m_reopen->Disable();
        Fit();
    }

    std::shared_ptr<UnlayeredAccount>  m_account;
    std::shared_ptr<std::atomic<bool>> m_alive;
    wxStaticText                      *m_status{nullptr};
    wxButton                          *m_reopen{nullptr};
    std::string                        m_browser_url;
    std::string                        m_error;
};

// After an update, while the slicer is not signed in (ask_sign_in_after_update).
// One Sign In button; "Not now" and the close box decline.
class AskSignInDialog : public DPIDialog
{
public:
    explicit AskSignInDialog(wxWindow *parent)
        : DPIDialog(parent, wxID_ANY, _L("Sign in to Unlayered"), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU)
    {
        auto *main = new wxBoxSizer(wxVERTICAL);

        auto *text = new wxStaticText(
            this, wxID_ANY,
            wxString::Format(_L("Sign in with your Unlayered account. %s then keeps itself up to date and offers "
                                "configuration updates for your printers."),
                             SLIC3R_APP_FULL_NAME));
        text->Wrap(FromDIP(420));
        main->Add(text, 0, wxALL, FromDIP(16));

        auto *sign_in = new wxButton(this, wxID_OK, _L("Sign In"), wxDefaultPosition, wxSize(FromDIP(200), FromDIP(36)));
        sign_in->SetDefault();
        sign_in->SetFocus();
        main->Add(sign_in, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(16));

        auto *later = new wxStaticText(this, wxID_ANY, _L("Not now"));
        later->SetFont(later->GetFont().Smaller());
        later->SetCursor(wxCursor(wxCURSOR_HAND));
        later->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { EndModal(wxID_CANCEL); });
        main->Add(later, 0, wxALIGN_RIGHT | wxALL, FromDIP(10));

        SetSizerAndFit(main);
        Centre(wxBOTH);
        wxGetApp().UpdateDlgDarkUI(this);
        // After the dark-mode pass, which would otherwise give it the normal text colour.
        later->SetForegroundColour(wxColour(150, 150, 150));
    }

protected:
    void on_dpi_changed(const wxRect &) override { Fit(); Refresh(); }
};

} // namespace

AutoUpdater::AutoUpdater(GUI_App &app)
    : m_app(app)
    , m_account(std::make_shared<UnlayeredAccount>(site_url_from(app.app_config)))
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{}

AutoUpdater::~AutoUpdater() { *m_alive = false; }

void AutoUpdater::notify(const std::string &text, const std::string &link, std::function<void()> on_link)
{
    Plater *plater = m_app.plater();
    if (!plater || !plater->get_notification_manager())
        return;
    plater->get_notification_manager()->close_notification_of_type(NotificationType::NewAppAvailable);
    plater->get_notification_manager()->push_notification(
        NotificationType::NewAppAvailable, NotificationManager::NotificationLevel::ImportantNotificationLevel, text, link,
        on_link ? std::function<bool(wxEvtHandler *)>([on_link](wxEvtHandler *) {
            // Deferred: the callback runs inside the notification's own render pass, and the
            // actions here open dialogs or close the main window.
            wxGetApp().CallAfter(on_link);
            return true;
        })
                : std::function<bool(wxEvtHandler *)>());
}

void AutoUpdater::on_startup()
{
    m_account->load();

    // Did the previous session hand over to an installer, and did it work?
    if (AppConfig *config = m_app.app_config) {
        const std::string pending = config->get(PENDING_UPDATE_KEY);
        if (!pending.empty()) {
            const std::string launch    = config->get(UPDATE_LAUNCH_KEY);
            const std::string installer = config->get(PENDING_INSTALLER_KEY);
            const std::string sha256    = config->get(PENDING_SHA256_KEY);
            const std::string from      = config->get(UPDATE_FROM_KEY);
            config->set(PENDING_UPDATE_KEY, "");
            // Usage diagnostics: did the update the previous session downloaded actually install?
            UsageTelemetry::record("update_applied", {{"to", pending},
                                                      {"from", from},
                                                      {"outcome", pending == UNLAYERED_VERSION ? "updated" : "not_updated"},
                                                      {"launch", launch}});
            config->set(UPDATE_FROM_KEY, "");
            config->set(UPDATE_LAUNCH_KEY, "");
            config->set(PENDING_INSTALLER_KEY, "");
            config->set(PENDING_SHA256_KEY, "");
            if (pending == UNLAYERED_VERSION) {
                BOOST_LOG_TRIVIAL(info) << "updater: now running the update to " << pending;
                if (!from.empty())
                    config->set(UPDATED_REPORT_KEY, from);
                notify((boost::format(_u8L("%1% was updated to %2%.")) % SLIC3R_APP_FULL_NAME % pending).str());
            } else {
                BOOST_LOG_TRIVIAL(warning) << "updater: an update to " << pending << " was due but this is still "
                                           << UNLAYERED_VERSION << " (" << launch << ")";
                // The installer never ran: the session ended before it could hand over (a crash, or
                // the process was ended), or another copy was still open. Its download is still on
                // disk; take it over before the sweep below removes it. Only for a version newer than
                // this one: a newer copy may have been installed by hand in the meantime.
                const auto due         = Semver::parse(pending);
                const auto current     = Semver::parse(UNLAYERED_VERSION);
                const bool interrupted = launch == "not_launched";
                if ((interrupted || launch == "other_instance_running") && due && current && *current < *due &&
                    !installer.empty() && !sha256.empty()) {
                    fs::path    claimed;
                    std::string error;
                    if (claim_previous_installer(installer, claimed, error))
                        reuse_download(pending, claimed, sha256, interrupted);
                    else
                        BOOST_LOG_TRIVIAL(info) << "updater: cannot reuse the previous download: " << error;
                }
            }
        }
    }
    cleanup_stale_download_dirs();

    // Stealth mode keeps only the anonymous update check. The sign-in is re-validated lazily instead,
    // by the download itself, which forgets a token the site refuses.
    if (!m_account->signed_in() || (m_app.app_config && m_app.app_config->get_stealth_mode()))
        return;
    // Confirms the stored sign-in still works (the person may have signed out on the website).
    // Off the GUI thread; the result only changes what the menus and the next update do.
    refresh_profile_in_background();
}

void AutoUpdater::refresh_profile_in_background()
{
    auto account = m_account;
    auto alive   = m_alive;
    std::thread([account, alive]() {
        if (!*alive)
            return;
        account->refresh();
        // The answer carries the account's edition label, which the window title shows. Through the
        // app, not m_app: this object may be gone by the time the site answers.
        wxGetApp().CallAfter([alive]() {
            if (*alive && !wxGetApp().is_closing()) // the main frame may already be gone
                refresh_app_title();
        });
    }).detach();
}

std::string changelog_sections(const std::string &markdown, const std::string &after, const std::string &up_to)
{
    const auto top    = Semver::parse(up_to);
    const auto bottom = after.empty() ? boost::optional<Semver>() : Semver::parse(after);
    if (!top || (!after.empty() && !bottom))
        return {};

    std::string out;
    bool        keep = false;
    size_t      pos  = 0;
    while (pos < markdown.size()) {
        size_t eol = markdown.find('\n', pos);
        if (eol == std::string::npos)
            eol = markdown.size();
        const std::string line = markdown.substr(pos, eol - pos);
        pos = eol + 1;

        if (boost::algorithm::starts_with(line, "## ")) {
            const auto v = heading_version(line);
            keep = v && *v <= *top && (bottom ? *bottom < *v : *v == *top);
        } else if (boost::algorithm::starts_with(line, "# ")) {
            keep = false; // the document title
        }
        if (keep)
            out += line + "\n";
    }
    return out;
}

void AutoUpdater::show_whats_new(bool first_run)
{
    AppConfig *config = m_app.app_config;
    if (!config)
        return;
    const std::string seen = config->get(WHATS_NEW_KEY);
    config->set(WHATS_NEW_KEY, UNLAYERED_VERSION);
    if (first_run)
        return;

    const auto current = Semver::parse(UNLAYERED_VERSION);
    const auto last    = seen.empty() ? boost::optional<Semver>() : Semver::parse(seen);
    if (!current || (last && !(*last < *current)))
        return;

    // Installed beside the other resources; a build-tree run has none and quietly shows nothing.
    const fs::path path = fs::path(resources_dir()) / "CHANGELOG.md";
    std::string    markdown;
    try {
        if (fs::exists(path))
            load_string_file(path, markdown);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "updater: could not read " << path.string() << ": " << e.what();
    }
    const std::string notes = changelog_sections(markdown, last ? seen : std::string(), UNLAYERED_VERSION);
    if (notes.empty()) {
        BOOST_LOG_TRIVIAL(info) << "updater: no changelog for " << UNLAYERED_VERSION << " in " << path.string();
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "updater: showing what's new in " << UNLAYERED_VERSION
                            << (seen.empty() ? std::string() : " since " + seen);
    // Deferred, so the update check started after this call is not held up behind the dialog.
    auto alive = m_alive;
    m_app.CallAfter([this, alive, notes]() {
        if (!*alive || !m_app.mainframe)
            return;
        UpdateVersionDialog dialog(m_app.mainframe);
        dialog.update_version_info(wxString::FromUTF8(notes), wxString::FromUTF8(UNLAYERED_VERSION));
        dialog.set_whats_new_mode();
        dialog.ShowModal();
    });
}

void AutoUpdater::sign_in(wxWindow *parent, std::function<void(bool)> done)
{
    SignInDialog dlg(parent ? parent : m_app.mainframe, m_account);
    const bool   ok = dlg.ShowModal() == wxID_OK && m_account->signed_in();
    // Usage diagnostics: "update" when an update asked for the sign-in, "menu" from Help.
    UsageTelemetry::record("account_signin", {{"result", ok ? "ok" : (dlg.error().empty() ? "cancelled" : "failed")},
                                              {"from", done ? "update" : "menu"}});
    if (ok) {
        const auto p = m_account->profile();
        notify((boost::format(_u8L("Signed in to Unlayered as %1%.")) % (p ? p->email : std::string())).str());
        // The sign-in answer carries no edition label; ask for it (for the window title).
        refresh_profile_in_background();
    } else if (!dlg.error().empty()) {
        BOOST_LOG_TRIVIAL(warning) << "account: sign-in failed: " << dlg.error();
    }
    if (done)
        done(ok);
}

bool AutoUpdater::stored_sign_in(AppConfig *config, std::string *email)
{
    UnlayeredAccount account(site_url_from(config));
    account.load();
    if (!account.signed_in())
        return false;
    if (email) {
        const auto p = account.profile();
        *email       = p ? p->email : std::string();
    }
    return true;
}

bool AutoUpdater::sign_in_for_setup(wxWindow *parent, AppConfig *config, std::string *email)
{
    auto account = std::make_shared<UnlayeredAccount>(site_url_from(config));
    account->load();
    if (!account->signed_in()) {
        // No usage diagnostics here: nothing is recorded while the setup wizard is open.
        SignInDialog dlg(parent, account);
        if (dlg.ShowModal() != wxID_OK || !account->signed_in()) {
            if (!dlg.error().empty())
                BOOST_LOG_TRIVIAL(warning) << "account: sign-in from setup failed: " << dlg.error();
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "account: signed in from setup";
    }
    if (email) {
        const auto p = account->profile();
        *email       = p ? p->email : std::string();
    }
    return true;
}

void AutoUpdater::ask_sign_in_after_update(bool wizard_showed)
{
    AppConfig *config = m_app.app_config;
    if (config == nullptr)
        return;
    if (wizard_showed) {
        config->set(SIGN_IN_ASKED_KEY, UNLAYERED_VERSION);
        return;
    }
    const std::string asked = config->get(SIGN_IN_ASKED_KEY);
    if (!asked.empty()) {
        const auto then = Semver::parse(asked);
        const auto now  = Semver::parse(UNLAYERED_VERSION);
        if (!then || !now || !(*then < *now))
            return; // asked in this version already
    }
    run_when_no_dialog(m_app, [this]() {
        AppConfig *config = m_app.app_config;
        config->set(SIGN_IN_ASKED_KEY, UNLAYERED_VERSION);
        config->save();
        if (m_account->signed_in())
            return;
        AskSignInDialog dlg(m_app.mainframe);
        const bool      yes = dlg.ShowModal() == wxID_OK;
        BOOST_LOG_TRIVIAL(info) << "account: asked to sign in after the update to " << UNLAYERED_VERSION << ", "
                                << (yes ? "signing in" : "not now");
        if (yes)
            sign_in(m_app.mainframe);
    });
}

void AutoUpdater::sign_out()
{
    const auto p = m_account->profile();
    MessageDialog confirm(m_app.mainframe,
                          wxString::Format(_L("Sign out of Unlayered (%s)?\n\n%s will stop updating itself until you "
                                              "sign in again."),
                                           wxString::FromUTF8(p ? p->email : std::string()), SLIC3R_APP_FULL_NAME),
                          _L("Sign Out"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
    if (confirm.ShowModal() == wxID_YES) {
        m_account->sign_out();
        refresh_app_title();
    }
}

bool AutoUpdater::handle_new_version(const std::string &version, const std::string &notes, const std::string &page_url,
                                     const std::optional<AccountDownload> &download, bool by_user, bool skipped)
{
    // Usage diagnostics: what happened when the update check found a newer version.
    auto offered = [&](const char *action, const std::string &why = {}) {
        nlohmann::json props = {{"to", version}, {"by_user", by_user}, {"signed_in", m_account->signed_in()}, {"action", action}};
        if (!why.empty())
            props["why"] = why;
        UsageTelemetry::record("update_offered", std::move(props));
    };
    if (!download) {
        offered("no_account_download"); // the site has no installer for this platform: download page
        return false;
    }

    m_install_dir = self_updatable_install_dir();
    if (!m_install_dir) {
        offered("not_self_updatable", self_update_blocker());
        BOOST_LOG_TRIVIAL(info) << "updater: " << version
                                << " is available with the account, but this copy is not a per-user installation "
                                   "that can replace itself; using the update dialog";
        return false;
    }

    if (m_version == version && m_state == State::Ready && !download->sha256.empty() &&
        !boost::algorithm::iequals(download->sha256, m_installer_sha256)) {
        // Published again since that installer was downloaded (a reused one from the last session):
        // download the one the site has now.
        BOOST_LOG_TRIVIAL(info) << "updater: " << version << " was published again; downloading it again";
        m_state = State::Idle;
    }
    if (m_version == version && m_state != State::Idle) {
        if (m_reusing)
            m_download = *download; // should the kept installer fail its check, download this one
        if (m_reused && !by_user) {
            offered("reused_download"); // the check found what the previous session already downloaded
            m_reused = false;
        }
        if (by_user) {
            if (m_state == State::Ready)
                notify_ready();
            else
                notify((boost::format(_u8L("Version %1% is still downloading.")) % version).str());
        }
        return true;
    }
    if (m_state == State::Downloading)
        return true; // finish the one in flight; the next launch looks again

    if (skipped && !by_user) {
        offered("skipped");
        return true; // "Skip this Version" means silence, for the automatic check
    }

    m_state    = State::Idle;
    m_version  = version;
    m_download = *download;

    const bool signed_in = m_account->signed_in();
    if (signed_in && !by_user) {
        BOOST_LOG_TRIVIAL(info) << "updater: downloading " << version << " in the background";
        offered("background_download");
        start_download();
        return true;
    }

    // A manual check, or signed out: show what is new and ask.
    UpdateVersionDialog dialog(m_app.mainframe);
    dialog.update_version_info(wxString::FromUTF8(notes), wxString::FromUTF8(version));
    // After update_version_info(), which resets the button to the classic modes.
    dialog.set_account_mode(signed_in ? UpdateVersionDialog::AccountMode::UpdateInBackground
                                      : UpdateVersionDialog::AccountMode::SignInToUpdate);
    if (by_user)
        dialog.m_button_skip_version->Hide();

    const int choice = dialog.ShowModal();
    offered(choice == wxID_YES ? (signed_in ? "dialog_update" : "dialog_sign_in")
                               : choice == wxID_OPEN ? "dialog_download_page" : "dialog_dismissed");
    switch (choice) {
    case wxID_YES:
        if (signed_in)
            start_download();
        else
            sign_in(m_app.mainframe, [this](bool ok) {
                if (ok)
                    start_download();
            });
        break;
    case wxID_OPEN: wxLaunchDefaultBrowser(wxString::FromUTF8(page_url)); break;
    default: break;
    }
    return true;
}

void AutoUpdater::start_download()
{
    if (m_state == State::Downloading)
        return;
    m_state            = State::Downloading;
    m_download_started = std::chrono::steady_clock::now();

    auto       account  = m_account;
    auto       alive    = m_alive;
    const auto download = m_download;

    std::thread([this, account, alive, download]() {
        fs::path    installer;
        std::string error;
        unsigned    status = 0;
        bool        ok     = false;

        UnlayeredAccount::DownloadLink link;
        if (account->fetch_download_link(download.url, link, status, error)) {
            if (!link.sha256.empty() && !boost::algorithm::iequals(link.sha256, download.sha256)) {
                // Published again between the manifest and this request. Do not mix the two.
                error = "The installer was replaced while this update was starting. It will be retried on "
                        "the next launch.";
            } else {
                const AppUpdateAsset asset{download.name, link.url, download.sha256, download.size};
                ok = download_installer(asset, installer, {}, error);
            }
        }
        wxGetApp().CallAfter([this, alive, ok, installer, error, status]() {
            if (*alive)
                on_download_finished(ok, installer, error, status);
        });
    }).detach();
}

void AutoUpdater::on_download_finished(bool ok, const fs::path &installer, const std::string &error, unsigned http_status)
{
    {
        // Usage diagnostics. The reason is a fixed category, never the error text (it can hold paths).
        nlohmann::json props = {{"to", m_version},
                                {"result", ok ? "ok" : "failed"},
                                {"seconds", int64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                                        std::chrono::steady_clock::now() - m_download_started).count())}};
        if (!ok) {
            props["reason"]      = download_failure_reason(error, http_status);
            props["http_status"] = int(http_status);
        }
        UsageTelemetry::record("update_download", std::move(props));
    }
    if (ok) {
        m_installer_sha256 = m_download.sha256;
        mark_ready(installer, false);
        return;
    }

    m_state = State::Idle;
    BOOST_LOG_TRIVIAL(error) << "updater: background download of " << m_version << " failed (HTTP " << http_status
                             << "): " << error;
    if (http_status == 401) {
        // The site no longer accepts the sign-in (expired, or signed out on the website).
        notify((boost::format(_u8L("Version %1% is available. Sign in to Unlayered again to update.")) % m_version).str(),
               _u8L("Sign in"), [this]() {
                   sign_in(m_app.mainframe, [this](bool signed_in) {
                       if (signed_in)
                           start_download();
                   });
               });
    } else if (http_status == 403) {
        const auto p = m_account->profile();
        notify((boost::format(_u8L("Version %1% is available, but %2% does not have access to %3% downloads.")) %
                m_version % (p ? p->email : std::string("this account")) % SLIC3R_APP_FULL_NAME)
                   .str());
    }
    // Anything else (offline, a checksum mismatch) stays in the log: the next launch tries again,
    // and Help > Check for Updates is always there.
}

void AutoUpdater::reuse_download(const std::string &version, const fs::path &installer, const std::string &sha256,
                                 bool interrupted)
{
    m_install_dir = self_updatable_install_dir();
    if (!m_install_dir) {
        cleanup_download_dir(installer); // the update check decides what this copy can do
        return;
    }
    m_state            = State::Downloading; // so the update check does not start a second download
    m_version          = version;
    m_download         = {};
    m_installer_sha256 = sha256;
    m_reusing          = true;
    BOOST_LOG_TRIVIAL(info) << "updater: checking the " << version << " installer the last session downloaded";

    auto alive = m_alive;
    std::thread([this, alive, version, installer, sha256, interrupted]() {
        std::string error;
        const bool  ok = verify_installer(installer, sha256, error);
        wxGetApp().CallAfter([this, alive, ok, version, installer, error, interrupted]() {
            if (*alive)
                on_reuse_checked(ok, version, installer, error, interrupted);
        });
    }).detach();
}

void AutoUpdater::on_reuse_checked(bool ok, const std::string &version, const fs::path &installer,
                                   const std::string &error, bool interrupted)
{
    m_reusing = false;
    // The update check may have answered meanwhile, with an installer published since.
    const bool republished = !m_download.url.empty() && !m_download.sha256.empty() &&
                             !boost::algorithm::iequals(m_download.sha256, m_installer_sha256);
    if (ok && !republished) {
        if (m_download.url.empty())
            m_reused = true; // handle_new_version() reports it when the update check answers
        else // the check has already answered
            UsageTelemetry::record("update_offered", {{"to", version},
                                                      {"by_user", false},
                                                      {"signed_in", m_account->signed_in()},
                                                      {"action", "reused_download"}});
        mark_ready(installer, interrupted);
        return;
    }
    if (!ok)
        BOOST_LOG_TRIVIAL(warning) << "updater: not reusing the " << version << " installer: " << error;
    cleanup_download_dir(installer);
    m_state = State::Idle;
    if (!m_download.url.empty())
        start_download();
    // Otherwise the update check, still to come, downloads it the usual way.
}

void AutoUpdater::mark_ready(const fs::path &installer, bool interrupted)
{
    m_installer = installer;
    m_state     = State::Ready;
    if (m_app.app_config) {
        m_app.app_config->set(PENDING_UPDATE_KEY, m_version);
        m_app.app_config->set(UPDATE_FROM_KEY, UNLAYERED_VERSION);
        m_app.app_config->set(UPDATE_LAUNCH_KEY, "not_launched"); // until on_app_exit() says otherwise
        m_app.app_config->set(PENDING_INSTALLER_KEY, download_relative_path(installer));
        m_app.app_config->set(PENDING_SHA256_KEY, m_installer_sha256);
    }
    BOOST_LOG_TRIVIAL(info) << "updater: " << m_version << " is ready at " << installer.string();
    notify_ready(interrupted);
}

void AutoUpdater::notify_ready(bool interrupted)
{
    if (interrupted) {
        // The last session crashed or was ended with this update downloaded, so it never installed.
        // Someone whose sessions keep ending that way would otherwise never get it.
        notify((boost::format(_u8L("%1% %2% did not install because the app closed unexpectedly. Install it now, or "
                                   "it installs when you close the app.")) %
                SLIC3R_APP_FULL_NAME % m_version)
                   .str(),
               _u8L("Install now"), [this]() { restart_to_update(); });
        return;
    }
    notify((boost::format(_u8L("%1% %2% is ready. It installs when you close the app.")) % SLIC3R_APP_FULL_NAME %
            m_version)
               .str(),
           _u8L("Restart to update"), [this]() { restart_to_update(); });
}

void AutoUpdater::restart_to_update()
{
    if (m_state != State::Ready)
        return;
    if (other_instances_running()) {
        MessageDialog dlg(m_app.mainframe,
                          wxString::Format(_L("Close the other %s windows first, then restart to update."),
                                           SLIC3R_APP_FULL_NAME),
                          _L("Restart to update"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    m_relaunch = true;
    // Close(false) so the save prompts run and a Cancel is honoured; the install itself happens in
    // on_app_exit(), only once the application really is going away.
    if (m_app.mainframe && !m_app.mainframe->Close(false))
        m_relaunch = false;
}

void AutoUpdater::on_app_exit()
{
    if (m_state != State::Ready || !m_install_dir || m_installer.empty())
        return;
    // Diagnostics have already shut down here, so how the hand-over went is kept for the next
    // launch's update_applied event. Saved explicitly: this runs at the very end of OnExit.
    auto keep_launch_result = [this](const char *result) {
        if (m_app.app_config) {
            m_app.app_config->set(UPDATE_LAUNCH_KEY, result);
            m_app.app_config->save();
        }
    };
    if (other_instances_running()) {
        // Its files are in use, so the installer would fail part-way. The next launch offers the
        // update again.
        BOOST_LOG_TRIVIAL(info) << "updater: another instance is running; not installing " << m_version << " now";
        keep_launch_result("other_instance_running");
        return;
    }
    std::string error;
    if (!launch_silent_update(m_installer, *m_install_dir, m_relaunch, error)) {
        BOOST_LOG_TRIVIAL(error) << "updater: could not start the update: " << error;
        keep_launch_result("launch_failed");
    } else
        keep_launch_result(m_relaunch ? "launched_restart" : "launched_on_exit");
}

} // namespace GUI
} // namespace Slic3r
