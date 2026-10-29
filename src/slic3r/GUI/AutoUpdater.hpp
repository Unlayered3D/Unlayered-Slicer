#ifndef slic3r_GUI_AutoUpdater_hpp_
#define slic3r_GUI_AutoUpdater_hpp_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <boost/filesystem/path.hpp>

#include "slic3r/Utils/AppUpdater.hpp"
#include "slic3r/Utils/UnlayeredAccount.hpp"

#include <wx/string.h>

class wxWindow;

namespace Slic3r {
class AppConfig;
namespace GUI {

class GUI_App;

// The application's name for the window title: the plain name, plus the edition label the account
// provider reports for the signed-in account (UnlayeredAccount::Profile::edition_label), if any.
// Cosmetic only -- nothing in the application is enabled or disabled by it. No round trip.
wxString app_title_name();
// Re-applies the window title after the account or its profile changed. GUI thread only.
void refresh_app_title();

// The account-based self-update ("Restart to update").
//
// When the update manifest offers an installer behind the Unlayered account (its
// `unlayered_downloads`), and this copy is a per-user Windows install that can replace itself, the
// update happens with no installer wizard at all:
//
//   1. download in the background with the account's device token, verify the sha256;
//   2. a notification says the version is ready - "Restart to update";
//   3. restarting, or simply closing the application later, runs the installer silently once this
//      process has exited, and a restart brings the new version straight back up.
//
// Signed out, the update dialog offers "Sign in and Update" instead, which runs the browser sign-in
// (UnlayeredAccount) and then step 1. Anything this cannot handle - another platform, a per-machine
// install, a portable copy, no account download in the manifest - is left to the classic dialog,
// unchanged.
class AutoUpdater
{
public:
    // The version a just-finished update came from, which the next update check reports once as
    // ?updated= (GUI_App::check_new_version_sf), so a successful install is counted even with usage
    // diagnostics off. Only a version number; the site adds one to a per-day counter. Cleared once
    // the site has answered.
    static constexpr const char *UPDATED_REPORT_KEY = "update_check_report_updated";

    explicit AutoUpdater(GUI_App &app);
    ~AutoUpdater();

    UnlayeredAccount &account() { return *m_account; }
    // For a worker thread that must keep the account alive until its request returns.
    std::shared_ptr<UnlayeredAccount> account_ptr() { return m_account; }

    // Once, after the main window exists: loads the stored sign-in, re-checks it with the site in the
    // background, sweeps old downloads and announces a just-finished update.
    void on_startup();

    // Once per version, after the startup wizard: on the first launch of a newer version - however it
    // was installed - shows that version's CHANGELOG.md sections in a "What's new" dialog, like
    // OrcaSlicer's release-note view. A first run (`first_run`, the setup wizard showed) only records
    // the version, so a fresh install never opens with a changelog.
    void show_whats_new(bool first_run);

    // From the "new version" event. Returns true when it has taken the update over, false to let the
    // classic dialog run. `skipped` is whether the person chose "Skip this Version" for it before.
    bool handle_new_version(const std::string &version, const std::string &notes, const std::string &page_url,
                            const std::optional<AccountDownload> &download, bool by_user, bool skipped);

    // Help menu actions.
    void sign_in(wxWindow *parent, std::function<void(bool signed_in)> done = {});
    void sign_out();

    // Unlayered 0.1.7: the setup wizard asks for a sign-in every time it runs while
    // the slicer is not signed in (guide 13signin). It runs before any AutoUpdater exists, so these
    // use an account object of their own; the token is stored as usual, and the AutoUpdater created
    // after the wizard loads it. No network for stored_sign_in().
    static bool stored_sign_in(AppConfig *config, std::string *email = nullptr);
    static bool sign_in_for_setup(wxWindow *parent, AppConfig *config, std::string *email = nullptr);

    // Once per version, after on_startup(): on the first launch of a newer version (0.1.6 -> 0.1.7
    // included) a slicer that is not signed in asks for a sign-in. It can be skipped. A run that
    // showed the setup wizard only records the version - the wizard asked already.
    void ask_sign_in_after_update(bool wizard_showed);

    // From GUI_App::OnExit: starts the silent install of a downloaded update, if there is one. The
    // installer waits for this process to exit before it touches anything.
    void on_app_exit();

    bool update_ready() const { return m_state == State::Ready; }
    const std::string &ready_version() const { return m_version; }
    // Closes the application (with the usual save prompts) and installs + relaunches.
    void restart_to_update();

private:
    enum class State { Idle, Downloading, Ready };

    // Re-checks the sign-in with the site off the GUI thread, then re-applies the window title
    // (which carries the provider's edition label, see app_title_name).
    void refresh_profile_in_background();
    void start_download();
    void on_download_finished(bool ok, const boost::filesystem::path &installer, const std::string &error,
                              unsigned http_status);
    // An installer the previous session downloaded but never ran: checks it in the background and,
    // if it is intact, makes it this session's ready update instead of downloading it again.
    void reuse_download(const std::string &version, const boost::filesystem::path &installer, const std::string &sha256,
                        bool interrupted);
    void on_reuse_checked(bool ok, const std::string &version, const boost::filesystem::path &installer,
                          const std::string &error, bool interrupted);
    // Ready to install on exit: remembers it across a restart and says so. `interrupted`: the last
    // session ended before it could install this, so the notice offers to install it now.
    void mark_ready(const boost::filesystem::path &installer, bool interrupted);
    void notify_ready(bool interrupted = false);
    void notify(const std::string &text, const std::string &link = {}, std::function<void()> on_link = {});

    GUI_App                          &m_app;
    // Shared, not unique: worker threads hold a reference so a request still in flight at exit
    // never outlives the object it writes to.
    std::shared_ptr<UnlayeredAccount> m_account;
    // Cleared in the destructor so a download thread finishing late never touches a dead object.
    std::shared_ptr<std::atomic<bool>> m_alive;

    State                          m_state{State::Idle};
    std::string                    m_version;
    AccountDownload                m_download;
    boost::filesystem::path        m_installer;
    // The sha256 m_installer was checked against: a ready installer is only good while that is
    // still what the site publishes for m_version.
    std::string                    m_installer_sha256;
    // m_installer came from the previous session (reuse_download); while m_reusing it is still being
    // checked and m_state is Downloading.
    bool                           m_reused{false};
    bool                           m_reusing{false};
    std::optional<boost::filesystem::path> m_install_dir;
    bool                           m_relaunch{false};
    // UNLAYERED_MOD -- for the update_download diagnostics event: how long the download took.
    std::chrono::steady_clock::time_point m_download_started;
};

// The "## <version>" sections of a CHANGELOG.md that are newer than `after` and no newer than
// `up_to`, in file order, headings included. An empty `after` means the `up_to` section alone (an
// update from a version that predates this, so its own last-seen version is unknown). Every "## "
// heading ends a section, but only one starting with a version can be selected; returns "" when
// nothing matches.
std::string changelog_sections(const std::string &markdown, const std::string &after, const std::string &up_to);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AutoUpdater_hpp_
