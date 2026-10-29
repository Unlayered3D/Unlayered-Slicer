#include "PrinterUpdateOffer.hpp"

#include <thread>

#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "AutoUpdater.hpp"
#include "FirmwareUpdateDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "SysFilesUpdateDialog.hpp"
#include "slic3r/Utils/DuetRrfClient.hpp"
#include "slic3r/Utils/FirmwareBundle.hpp"
#include "slic3r/Utils/SysFilesBundle.hpp"

namespace Slic3r {
namespace GUI {

FeedCredential printer_feed_credential()
{
    FeedCredential cred;
    AutoUpdater *updater = wxGetApp().auto_updater();
    if (!updater) {
        // Before post_init, i.e. inside the first-run setup wizard (guide 21update): no account
        // object yet. Read the stored sign-in the way AutoUpdater::stored_sign_in() does - no
        // network - and bind the token to the same configured site.
        AppConfig *config = wxGetApp().app_config;
        if (!config)
            return cred;
        UnlayeredAccount account(boost::algorithm::trim_copy(config->get(SETTING_ACCOUNT_PROVIDER_URL)));
        account.load();
        cred.site_url = account.site_url();
        cred.token    = account.device_token();
        return cred;
    }
    UnlayeredAccount &account = updater->account();
    cred.site_url             = account.site_url();
    cred.token                = account.device_token();
    return cred;
}

struct PrinterUpdateOffer::Answer
{
    bool                  reachable = false;
    std::string           error;
    DuetClient::BoardInfo board;
    PrinterUpdateFacts    facts;
    bool                  config_asked = false;
    unsigned              config_status = 0; // HTTP status of the configuration feed
    std::string           config_machine;    // the package's "machine": shown, never enforced
};

PrinterUpdateOffer::PrinterUpdateOffer(GUI_App &app) : m_app(app), m_alive(std::make_shared<std::atomic<bool>>(true)) {}

PrinterUpdateOffer::~PrinterUpdateOffer() { *m_alive = false; }

PrinterUpdateSkips PrinterUpdateOffer::skips() const
{
    AppConfig *config = m_app.app_config;
    return PrinterUpdateSkips(
        [config](const std::string &section, const std::string &key) {
            return config ? config->get(section, key) : std::string();
        },
        [config](const std::string &section, const std::string &key, const std::string &value) {
            if (config)
                config->set(section, key, value);
        });
}

PrinterUpdateOffer::Request PrinterUpdateOffer::make_request(const std::string &host, const std::string &password)
{
    Request req;
    req.host          = host;
    req.password      = password;
    AppConfig *config = wxGetApp().app_config;
    req.api_base      = config ? config->github_api_base() : std::string();
    // Stealth mode keeps the anonymous checks only, as AutoUpdater does: the firmware feed is
    // public and asks nothing about who is asking, the configuration feed needs the account.
    req.credential = printer_feed_credential();
    req.ask_config = !req.credential.empty() && config && !config->get_stealth_mode();
    return req;
}

void PrinterUpdateOffer::on_printer_selected(const DynamicPrintConfig &printer_config)
{
    const auto *host_type = printer_config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    if (!host_type || host_type->value != htUnlayered || !printer_config.has("print_host"))
        return;
    const std::string host = printer_config.opt_string("print_host");
    if (host.empty())
        return; // a USB-only printer is updated from its dialog; nothing here scans ports
    // Asked for by the setup wizard (guide 21update): checked again even when this session already
    // did, one at a time. The retry delay for a printer that did not answer still applies.
    const bool after_setup = m_app.app_config && m_app.app_config->get(CHECK_AFTER_SETUP_KEY) == "1";
    if (after_setup ? m_after_setup_busy : m_hosts.count(host) > 0)
        return;
    if (auto it = m_retry_at.find(host); it != m_retry_at.end() && std::chrono::steady_clock::now() < it->second)
        return;
    m_hosts.insert(host);
    if (after_setup)
        m_after_setup_busy = true;

    Request req = make_request(host, printer_config.has("printhost_apikey") ? printer_config.opt_string("printhost_apikey") : std::string());
    req.preset_name = wxGetApp().preset_bundle ? wxGetApp().preset_bundle->printers.get_edited_preset().name : std::string();
    req.after_setup = after_setup;

    BOOST_LOG_TRIVIAL(info) << "printer updates: checking " << req.host << (req.ask_config ? " (firmware and configuration)" : " (firmware)")
                            << (req.after_setup ? ", asked for by the setup wizard" : "");
    auto alive = m_alive;
    std::thread([this, alive, req]() {
        Answer answer = check(req);
        wxGetApp().CallAfter([this, alive, req, answer]() {
            // Not while closing: m_alive stays true until OnExit, but on_checked uses the plater,
            // which the main frame's destruction has already freed by then.
            if (*alive && !wxGetApp().is_closing())
                on_checked(req, answer);
        });
    }).detach();
}

// Worker thread. Touches nothing but its arguments.
PrinterUpdateOffer::Answer PrinterUpdateOffer::check(const Request &req)
{
    Answer      a;
    std::string err;

    // ---- The board: one session, closed again before the feeds are asked -------------------------
    {
        DuetRrfClient client(req.host, req.password);
        if (client.connect(err) != DuetClient::ConnectResult::ok) {
            a.error = err.empty() ? std::string("no connection") : err;
            return a;
        }
        if (!client.read_board_info(a.board, err)) {
            client.disconnect();
            a.error = err;
            return a;
        }
        std::vector<uint8_t> marker;
        bool                 missing = false;
        std::string          marker_err;
        SysFilesInstalled    installed;
        if (client.download(std::string("0:/") + SYS_MARKER_PATH, marker, missing, marker_err) && !missing)
            parse_sys_marker(std::string(marker.begin(), marker.end()), installed);
        client.disconnect();

        a.reachable                = true;
        a.facts.board_id           = a.board.unique_id.empty() ? req.host : a.board.unique_id;
        a.facts.running_firmware   = a.board.firmware_version;
        a.facts.config_known       = installed.known;
        a.facts.installed_config   = installed.version;
    }

    // ---- One firmware-feed query: the release, and its manifest when it carries one --------------
    {
        GitHubReleaseFirmwareSource::Options opts;
        opts.api_base                = req.api_base;
        opts.board_firmware_filename = a.board.firmware_filename;
        opts.board_iap_filename      = a.board.iap_filename;
        opts.board_wifi_filename     = a.board.wifi_filename;
        opts.board_short_name        = a.board.short_name;
        GitHubReleaseFirmwareSource source(std::move(opts));
        FirmwarePackage             pkg;
        if (source.fetch_metadata(pkg, err))
            a.facts.available_firmware = pkg.version;
        else
            BOOST_LOG_TRIVIAL(info) << "printer updates: firmware feed: " << err;
    }

    // ---- One configuration-feed query --------------------------------------------------------------
    // The release list, then its sys.zip: the feed's tags are free-form and the version that the
    // card's marker is compared with is the one in the archive's manifest.json. The archive is a
    // few kilobytes of G-code and is not written anywhere.
    if (req.ask_config) {
        GitHubSysFileSource::Options opts;
        opts.api_base   = req.api_base;
        opts.credential = req.credential;
        GitHubSysFileSource source(std::move(opts));
        SysFilePackage      pkg;
        a.config_asked = true;
        if (source.fetch(pkg, nullptr, err)) {
            a.facts.available_config = pkg.version;
            a.config_machine         = pkg.machine;
        } else {
            a.config_status = source.last_http_status();
            BOOST_LOG_TRIVIAL(info) << "printer updates: configuration feed (HTTP " << a.config_status << "): " << err;
        }
    }
    return a;
}

void PrinterUpdateOffer::check_now(const std::string &host, const std::string &password, std::shared_ptr<std::atomic<bool>> alive,
                                   std::function<void(const CheckResult &)> done)
{
    const Request req = make_request(host, password);
    BOOST_LOG_TRIVIAL(info) << "printer updates: setup wizard checks " << req.host
                            << (req.ask_config ? " (firmware and configuration)" : " (firmware)");
    std::thread([alive, req, done]() {
        const Answer a = check(req);
        CheckResult  r;
        r.reachable    = a.reachable;
        r.error        = a.error;
        r.machine_name = a.board.machine_name;
        r.facts        = a.facts;
        r.config_asked = a.config_asked;
        r.outcome      = classify_printer_update_check(a.reachable, a.facts);
        BOOST_LOG_TRIVIAL(info) << "printer updates: setup wizard check of " << req.host << ": outcome " << int(r.outcome)
                                << ", firmware " << a.facts.running_firmware << " -> '" << a.facts.available_firmware
                                << "', configuration " << (a.facts.config_known ? a.facts.installed_config : std::string("unknown"))
                                << " -> '" << a.facts.available_config << "'" << (a.error.empty() ? "" : " (" + a.error + ")");
        wxGetApp().CallAfter([alive, r, done]() {
            if (alive && *alive && !wxGetApp().is_closing())
                done(r);
        });
    }).detach();
}

void PrinterUpdateOffer::on_checked(const Request &req, const Answer &a)
{
    if (req.after_setup)
        m_after_setup_busy = false;
    if (!a.reachable) {
        // Not a check: the printer may simply be off. Let a later selection try again, not soon.
        BOOST_LOG_TRIVIAL(info) << "printer updates: " << req.host << " did not answer (" << a.error << ")";
        m_hosts.erase(req.host);
        m_retry_at[req.host] = std::chrono::steady_clock::now() + RETRY_AFTER;
        return;
    }
    const PrinterUpdateFacts &f = a.facts;
    BOOST_LOG_TRIVIAL(info) << "printer updates: " << req.host << " board " << f.board_id << " runs " << f.running_firmware
                            << ", configuration " << (f.config_known ? f.installed_config : std::string("unknown"))
                            << "; feeds offer firmware '" << f.available_firmware << "', configuration '"
                            << f.available_config << "'";
    if (!m_boards.insert(f.board_id).second && !req.after_setup)
        return; // the same machine, reached at another address this session

    // Asked for by the setup wizard: done once a feed has answered, whatever it said. "Skip this
    // version" is not consulted - the person asked - and "up to date" is said too.
    if (req.after_setup) {
        if (classify_printer_update_check(true, f) == PrinterUpdateCheckOutcome::NoFeed) {
            BOOST_LOG_TRIVIAL(info) << "printer updates: no feed answered; the setup wizard's check stays pending";
            return;
        }
        m_app.app_config->set(CHECK_AFTER_SETUP_KEY, "");
    }

    const PrinterUpdateDecision decision = decide_printer_update(f, req.after_setup ? no_printer_update_skips() : skips());

    Plater *plater = m_app.plater();
    if (!plater || !plater->get_notification_manager())
        return;

    if (!decision.any()) {
        if (req.after_setup) {
            const std::string who  = !a.board.machine_name.empty() ? a.board.machine_name : req.preset_name;
            std::string       text = (boost::format(_u8L("Your Unlayered printer %1% is up to date: firmware %2%")) %
                                (who.empty() ? req.host : who) % f.running_firmware).str();
            if (f.config_known && !f.available_config.empty())
                text += "\n" + (boost::format(_u8L("Printer settings %1%")) % f.installed_config).str();
            else if (!a.config_asked)
                text += "\n" + _u8L("Sign in to Unlayered (Help menu) to check its printer settings too.");
            plater->get_notification_manager()->push_notification(NotificationType::UnlayeredPrinterUpdateAvailable,
                                                                  NotificationManager::NotificationLevel::RegularNotificationLevel, text);
        }
        return;
    }

    std::string text;
    if (decision.firmware && decision.config)
        text = (boost::format(_u8L("Printer update available: firmware %1% / configuration %2%")) % f.available_firmware %
                f.available_config).str();
    else if (decision.firmware)
        text = (boost::format(_u8L("Printer update available: firmware %1%")) % f.available_firmware).str();
    else
        text = (boost::format(_u8L("Printer update available: configuration %1%")) % f.available_config).str();

    // Which printer: every Unlayered machine is the same board, so its M550 name is what a person
    // recognises, then the preset they picked.
    const std::string who = !a.board.machine_name.empty() ? a.board.machine_name : req.preset_name;
    text += "\n" + (who.empty() ? req.host : (boost::format(_u8L("%1% at %2%")) % who % req.host).str());

    // One configuration stream serves every model today, and nothing on the board can be checked
    // against its "machine", so the offer says who it was made for and lets the owner judge.
    if (decision.config && !a.config_machine.empty())
        text += "\n" + (boost::format(_u8L("The configuration is for: %1%")) % a.config_machine).str();

    // Signed out, the configuration feed was never asked. Say what signing in would add - but only
    // alongside an offer that is happening anyway, never as a notification of its own.
    if (!a.config_asked && req.credential.empty())
        text += "\n" + _u8L("Sign in to Unlayered (Help menu) to get configuration updates too.");

    const std::string host     = req.host;
    const std::string password = req.password;
    const std::string board_id = f.board_id;
    const std::string skip_fw  = decision.firmware ? f.available_firmware : std::string();
    const std::string skip_cfg = decision.config ? f.available_config : std::string();
    const bool        want_fw  = decision.firmware;
    const bool        want_cfg = decision.config;
    auto              alive    = m_alive;

    std::vector<NotificationManager::ActionLink> actions;
    actions.emplace_back(_u8L("Update"), [host, password, want_fw, want_cfg](wxEvtHandler *) {
        // Deferred: this runs inside the notification's render pass, and the flow opens dialogs.
        wxGetApp().CallAfter([host, password, want_fw, want_cfg]() {
            run_update_flow(wxGetApp().mainframe, host, password, want_fw, want_cfg);
        });
        return true;
    });
    actions.emplace_back(_u8L("Not now"), [](wxEvtHandler *) { return true; });
    actions.emplace_back(_u8L("Skip this version"), [this, alive, board_id, skip_fw, skip_cfg](wxEvtHandler *) {
        if (*alive) {
            skips().skip(board_id, skip_fw, skip_cfg);
            BOOST_LOG_TRIVIAL(info) << "printer updates: " << board_id << " skips firmware '" << skip_fw
                                    << "', configuration '" << skip_cfg << "'";
        }
        return true;
    });
    plater->get_notification_manager()->push_action_row_notification(NotificationType::UnlayeredPrinterUpdateAvailable,
                                                                      NotificationManager::NotificationLevel::ImportantNotificationLevel,
                                                                      text, std::move(actions));
}

void PrinterUpdateOffer::run_update_flow(wxWindow *parent, const std::string &host, const std::string &password,
                                         bool firmware, bool config)
{
    // The dialogs read the same two keys the physical printer dialog hands them.
    DynamicPrintConfig printer;
    printer.set_key_value("print_host", new ConfigOptionString(host));
    printer.set_key_value("printhost_apikey", new ConfigOptionString(password));

    // Firmware first. If it is declined or fails, the configuration dialog still opens: its
    // minFirmware gate refuses files the running firmware cannot run, and says to update first.
    if (firmware) {
        FirmwareUpdateDialog dlg(parent, &printer);
        dlg.begin_when_ready();
        dlg.ShowModal();
    }
    if (config) {
        SysFilesUpdateDialog dlg(parent, &printer);
        dlg.begin_when_ready();
        dlg.ShowModal();
    }
}

} // namespace GUI
} // namespace Slic3r
