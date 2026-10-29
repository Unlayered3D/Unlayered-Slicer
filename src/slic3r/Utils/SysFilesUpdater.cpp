#include "SysFilesUpdater.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

namespace {

// Machine states in which rewriting the configuration would interfere with a running job. RRF does
// not stop us: macros are read from the card while a print runs, so replacing one mid-print can
// change behaviour halfway through.
bool is_busy_status(const std::string &status)
{
    static const std::set<std::string> busy = {"processing", "pausing", "paused", "resuming", "cancelling", "simulating"};
    return busy.count(boost::algorithm::to_lower_copy(status)) > 0;
}

// Host names go into a directory name, so anything a file system might object to is flattened.
std::string sanitize_for_path(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_');
    while (!out.empty() && out.front() == '_')
        out.erase(out.begin());
    return out.empty() ? std::string("printer") : out;
}

std::string local_time(const char *format)
{
    char       buffer[32];
    const auto t  = std::time(nullptr);
    const auto tm = *std::localtime(&t);
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return std::string(buffer);
}

// What a file on the card held before this run touched it.
struct Original
{
    bool                 existed = false;
    std::vector<uint8_t> data;
};

class Run
{
public:
    Run(DuetClient &client, const SysFilesUpdateOptions &opts, SysFilesUpdateCtl &ctl, SysFilesUpdateResult &r)
        : m_client(client), m_opts(opts), m_ctl(ctl), m_r(r)
    {}

    void execute(const SysFileSourceFactory &source_factory);

private:
    DuetClient                  &m_client;
    const SysFilesUpdateOptions &m_opts;
    SysFilesUpdateCtl           &m_ctl;
    SysFilesUpdateResult        &m_r;

    std::vector<uint8_t>            m_marker_before; // the card's unlayered-sys.json, raw
    std::map<std::string, Original> m_originals;     // keyed by card path
    bool                            m_have_originals = false;

    void status(int percent, const std::string &msg)
    {
        BOOST_LOG_TRIVIAL(info) << "SysFilesUpdate: " << percent << "% " << msg;
        m_ctl.status(percent, msg);
    }
    void fail(const std::string &msg)
    {
        BOOST_LOG_TRIVIAL(error) << "SysFilesUpdate failed: " << msg;
        m_r.outcome = SysFilesUpdateResult::Outcome::Failed;
        m_r.message = msg;
        status(100, msg);
    }
    bool cancelled() const { return m_ctl.cancelled(); }
    void mark_cancelled() { m_r.outcome = SysFilesUpdateResult::Outcome::Cancelled; }

    bool read_originals(const SysFilePackage &pkg, std::string &error_out);
    bool save_backup(const SysFilePackage &pkg, std::string &error_out);
    // Puts back everything a write was attempted for. Returns true when every file was restored.
    bool rollback(const std::vector<std::string> &attempted);
};

bool Run::read_originals(const SysFilePackage &pkg, std::string &error_out)
{
    unsigned index = 0;
    for (const auto &f : pkg.files) {
        if (cancelled()) {
            error_out = "Cancelled";
            return false;
        }
        ++index;
        status(int(35 + 10.0 * double(index) / double(std::max<size_t>(pkg.files.size(), 1))),
               (boost::format(_u8L("Reading the printer's current %1%")) % f.path).str());

        Original    orig;
        bool        missing = false;
        std::string err;
        if (!m_client.download(f.remote_path(), orig.data, missing, err)) {
            if (!missing) {
                error_out = (boost::format("Could not read %1% from the printer: %2%") % f.path % err).str();
                return false;
            }
        }
        orig.existed = !missing;
        if (!orig.existed) {
            orig.data.clear();
            m_r.created.push_back(f.path);
        }
        m_originals[f.path] = std::move(orig);
    }
    m_have_originals = true;
    return true;
}

bool Run::save_backup(const SysFilePackage &pkg, std::string &error_out)
{
    // Name the folder after the machine rather than the connection. An IP address is a reasonable
    // identity but a COM port is not - it changes with the USB socket, and it changes again when a
    // firmware update alters how many ports the board presents. network.name (what M550 set) is the
    // only field that tells two Unlayered machines apart, so prefer it and fall back to the endpoint.
    const std::string key  = (m_r.board_known && !m_r.board.machine_name.empty()) ? m_r.board.machine_name
                                                                                   : m_client.endpoint();
    const fs::path    root = m_opts.backup_root.empty() ? fs::path(data_dir()) / "printer_backups" : fs::path(m_opts.backup_root);
    const fs::path    dir  = root / sanitize_for_path(key) / local_time("%Y-%m-%d-%H%M%S");

    std::vector<std::string> replaced;
    for (const auto &f : pkg.files) {
        auto it = m_originals.find(f.path);
        if (it != m_originals.end() && it->second.existed)
            replaced.push_back(f.path);
    }
    // An install onto a fresh card replaces nothing, so there is nothing to keep.
    if (replaced.empty())
        return true;

    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error_out = (boost::format("Could not create the backup folder %1%: %2%") % dir.string() % ec.message()).str();
        return false;
    }

    for (const auto &path : replaced) {
        const Original &orig = m_originals[path];
        const fs::path  out  = dir / fs::path(path);
        fs::create_directories(out.parent_path(), ec);
        std::ofstream os(out.string(), std::ios::binary);
        if (!os) {
            error_out = (boost::format("Could not write %1%") % out.string()).str();
            return false;
        }
        if (!orig.data.empty())
            os.write(reinterpret_cast<const char *>(orig.data.data()), std::streamsize(orig.data.size()));
        os.close();
        if (!os) {
            error_out = (boost::format("Could not write %1%") % out.string()).str();
            return false;
        }
    }

    // What this backup is, beside the files. Its name starts with a dot so that picking this folder
    // in Config > From Folder restores exactly the files above and never uploads the description.
    pt::ptree info;
    info.put("printer", m_r.board.machine_name);
    info.put("endpoint", m_client.endpoint());
    info.put("boardId", m_r.board.unique_id);
    info.put("firmware", m_r.board.firmware_version);
    info.put("configurationBefore", m_r.installed.known ? m_r.installed.version : std::string());
    info.put("configurationInstalled", pkg.version);
    info.put("savedAt", local_time("%Y-%m-%dT%H:%M:%S"));
    pt::ptree replaced_list, created_list;
    for (const auto &p : replaced) {
        pt::ptree e;
        e.put("", p);
        replaced_list.push_back(std::make_pair("", e));
    }
    for (const auto &p : m_r.created) {
        pt::ptree e;
        e.put("", p);
        created_list.push_back(std::make_pair("", e));
    }
    info.add_child("replaced", replaced_list);
    // New files are not in the folder; a full restore deletes them from the card by hand.
    info.add_child("created", created_list);
    {
        std::ofstream os((dir / ".unlayered-backup.json").string(), std::ios::binary);
        pt::write_json(os, info, true);
    }
    if (!m_marker_before.empty()) {
        std::ofstream os((dir / ".unlayered-sys.before.json").string(), std::ios::binary);
        os.write(reinterpret_cast<const char *>(m_marker_before.data()), std::streamsize(m_marker_before.size()));
    }

    m_r.backup_dir = dir.string();
    BOOST_LOG_TRIVIAL(info) << boost::format("SysFilesUpdate: backed up %1% file(s) to %2%") % replaced.size() % dir.string();
    return true;
}

bool Run::rollback(const std::vector<std::string> &attempted)
{
    const SysFilesRollbackPlan plan = plan_sys_files_rollback(attempted, m_r.created);
    status(95, _u8L("Putting the printer's previous files back"));

    bool all_ok = true;
    for (const auto &path : plan.restore) {
        std::string err;
        const Original &orig = m_originals[path];
        if (orig.data.empty() || !m_client.upload("0:/" + path, orig.data, nullptr, err)) {
            // An empty original cannot be re-created over rr_upload; leave the new one and say so.
            BOOST_LOG_TRIVIAL(error) << "SysFilesUpdate: rollback could not restore " << path << ": " << err;
            m_r.rollback_failed.push_back(path);
            all_ok = false;
        }
    }
    for (const auto &path : plan.remove) {
        std::string err;
        if (!m_client.remove("0:/" + path, err)) {
            // A write that never landed leaves nothing to delete, which is also what this wanted.
            std::vector<uint8_t> probe;
            bool                 missing = false;
            std::string          probe_err;
            if (m_client.download("0:/" + path, probe, missing, probe_err) || !missing) {
                BOOST_LOG_TRIVIAL(error) << "SysFilesUpdate: rollback could not delete " << path << ": " << err;
                m_r.rollback_failed.push_back(path);
                all_ok = false;
            }
        }
    }
    m_r.rolled_back = true;
    return all_ok;
}

void Run::execute(const SysFileSourceFactory &source_factory)
{
    std::string err;

    // ---- 1. Connect -----------------------------------------------------------------------
    status(2, _u8L("Connecting to the printer"));
    switch (m_client.connect(err)) {
    case DuetClient::ConnectResult::ok: break;
    case DuetClient::ConnectResult::wrong_password:
        fail(_u8L("The printer rejected the password. Check the printer's password in its settings."));
        return;
    case DuetClient::ConnectResult::no_free_session:
        fail(_u8L("The printer has no free connection slots. Close any open Duet Web Control tabs and try again."));
        return;
    case DuetClient::ConnectResult::unreachable:
    default:
        fail((boost::format(_u8L("Could not reach the printer at %1%: %2%")) % m_client.endpoint() % err).str());
        return;
    }

    // ---- 2. Identify the printer ------------------------------------------------------------
    status(5, _u8L("Reading printer information"));
    DuetClient::BoardInfo board;
    if (m_client.read_board_info(board, err)) {
        m_r.board       = board;
        m_r.board_known = true;
    } else {
        // Not fatal: the board description labels the dialog and gates firmware; files can be
        // written without it.
        BOOST_LOG_TRIVIAL(warning) << "SysFilesUpdate: could not read board info: " << err;
    }

    // ---- 3. What is on the card already? ------------------------------------------------------
    status(7, _u8L("Checking what is installed on the printer"));
    {
        bool        missing = false;
        std::string marker_err;
        if (m_client.download(std::string("0:/") + SYS_MARKER_PATH, m_marker_before, missing, marker_err) && !missing)
            parse_sys_marker(std::string(m_marker_before.begin(), m_marker_before.end()), m_r.installed);
        else {
            m_marker_before.clear();
            if (!missing && !marker_err.empty())
                BOOST_LOG_TRIVIAL(info) << "SysFilesUpdate: no installed-version marker: " << marker_err;
        }
    }

    // ---- 4. Preflight: do not rewrite a printer's configuration mid-job -----------------------
    std::string machine_status;
    if (!m_client.read_status(machine_status, err)) {
        fail((boost::format(_u8L("Could not read the printer's status: %1%")) % err).str());
        return;
    }
    if (!m_opts.query_only && is_busy_status(machine_status)) {
        fail((boost::format(_u8L("The printer is busy (%1%). Stop or finish the current job before changing its "
                                 "configuration.")) % machine_status).str());
        return;
    }
    if (cancelled()) { mark_cancelled(); return; }

    // ---- 5. Fetch the package -----------------------------------------------------------------
    std::unique_ptr<SysFileSource> source = source_factory ? source_factory() : nullptr;
    if (!source) {
        fail(_u8L("No source of configuration files is configured."));
        return;
    }
    status(8, (boost::format(_u8L("Looking for configuration files (%1%)")) % source->description()).str());

    auto fetch_progress = [&](const std::string &stage, uint64_t done, uint64_t total) -> bool {
        if (cancelled())
            return false;
        const int pct = total > 0 ? int(8 + (27.0 * double(done) / double(total))) : 8;
        status(std::min(pct, 35), stage);
        return true;
    };

    SysFilePackage pkg;
    if (!source->fetch(pkg, fetch_progress, err)) {
        if (cancelled() || err == "Cancelled") { mark_cancelled(); return; }
        fail((boost::format(_u8L("Could not get the configuration files: %1%")) % err).str());
        return;
    }

    m_r.available_version = pkg.version;
    m_r.available_date    = pkg.date;
    m_r.available_machine = pkg.machine;
    m_r.available_notes   = pkg.notes;
    m_r.available_origin  = pkg.origin.empty() ? source->description() : pkg.origin;
    m_r.skipped           = pkg.skipped;
    m_r.from_full_sd_card = pkg.from_full_sd_card;
    for (const auto &f : pkg.files)
        m_r.available_files.push_back({f.path, uint64_t(f.data.size())});

    m_r.required_firmware = sys_required_firmware(pkg, &m_r.required_firmware_reason);
    m_r.firmware_too_old  = m_r.board_known && !sys_firmware_satisfies(m_r.board.firmware_version, m_r.required_firmware);

    if (m_opts.query_only) {
        m_r.outcome = SysFilesUpdateResult::Outcome::QueryOk;
        status(100, _u8L("Ready"));
        m_client.disconnect();
        return;
    }

    // ---- 6. Narrow it down to what the user ticked ---------------------------------------------
    if (!m_opts.selected.empty()) {
        std::vector<SysFile> kept;
        for (const auto &want : m_opts.selected) {
            auto it = std::find_if(pkg.files.begin(), pkg.files.end(),
                                   [&want](const SysFile &f) { return boost::iequals(f.path, want); });
            if (it == pkg.files.end()) {
                // The package can legitimately have changed between the query and the install.
                BOOST_LOG_TRIVIAL(warning) << "SysFilesUpdate: " << want << " is no longer in the package";
                m_r.skipped.emplace_back(want, "no longer in the package");
                continue;
            }
            kept.push_back(std::move(*it));
            pkg.files.erase(it);
        }
        pkg.files = std::move(kept);
    }
    if (pkg.files.empty()) {
        fail(_u8L("There are no files selected to install."));
        return;
    }

    // Configuration newer than the firmware is worse than no update: config.g stops at the first
    // command the board does not know and skips the rest, so the machine comes up with no heaters,
    // fans or tool (e.g. an M558 parameter older firmware does not know). Judged
    // on the ticked files, so leaving the offending file out is a way through. Not overridable by
    // force: the fix is always to update the firmware first, and that is always possible.
    {
        std::string       reason;
        const std::string need = sys_required_firmware(pkg, &reason);
        if (!m_r.board_known) {
            if (!need.empty())
                BOOST_LOG_TRIVIAL(warning) << "SysFilesUpdate: these files need firmware " << need << " (" << reason
                                           << ") and the board's version could not be read";
        } else if (!sys_firmware_satisfies(m_r.board.firmware_version, need)) {
            fail((boost::format(_u8L("These files need printer firmware %1% or newer (%2%), and the printer is "
                                     "running %3%. Nothing has been changed. Update the firmware first with the "
                                     "Firmware button, then install the configuration."))
                  % need % reason % m_r.board.firmware_version).str());
            return;
        }
    }

    // A package that states no version is never "already installed" - the same rule the firmware
    // updater applies to a hand-picked image.
    if (!m_opts.force && !pkg.version.empty() && m_r.installed.known && pkg.version == m_r.installed.version &&
        (pkg.date.empty() || m_r.installed.date.empty() || pkg.date == m_r.installed.date)) {
        m_r.outcome = SysFilesUpdateResult::Outcome::UpToDate;
        m_r.message = (boost::format(_u8L("The printer already has configuration %1%.")) % pkg.version).str();
        status(100, m_r.message);
        m_client.disconnect();
        return;
    }

    status(34, _u8L("Checking the configuration files"));
    if (!validate_sys_package(pkg, err)) {
        fail(err);
        return;
    }
    if (cancelled()) { mark_cancelled(); return; }

    // ---- 7. Read what is about to be replaced, and keep a copy -----------------------------------
    // Read whenever the transport can give the bytes back exactly, backup or not: they are what a
    // rollback writes back. Over a transport that cannot, there is neither a backup nor a rollback.
    const DuetClient::Capabilities caps = m_client.capabilities();
    if (caps.read_files && caps.read_files_exact) {
        if (!read_originals(pkg, err)) {
            if (cancelled() || err == "Cancelled") { mark_cancelled(); return; }
            fail((boost::format(_u8L("Could not read the printer's current files: %1%. Nothing has been changed."))
                  % err).str());
            return;
        }
        if (m_opts.backup && !save_backup(pkg, err)) {
            fail((boost::format(_u8L("Could not back up the printer's current files: %1%. Nothing has been changed."))
                  % err).str());
            return;
        }
    } else {
        BOOST_LOG_TRIVIAL(warning) << "SysFilesUpdate: " << m_client.endpoint()
                                   << " cannot read files back exactly: no backup and no rollback for this run";
    }
    if (cancelled()) { mark_cancelled(); return; }

    // ---- 8. Make sure the destinations exist -----------------------------------------------------
    // rr_upload cannot create a missing directory, and 0:/filaments or a macro subfolder may well
    // not exist yet. rr_mkdir answers err=1 for "already there" too, so its result is only logged.
    {
        std::set<std::string> dirs;
        for (const auto &f : pkg.files) {
            const std::string remote = f.remote_path();
            const auto        slash  = remote.find_last_of('/');
            if (slash != std::string::npos && slash > 2)
                dirs.insert(remote.substr(0, slash));
        }
        // Parents before children, so a nested macro folder can be created a level at a time.
        for (const auto &d : dirs) {
            std::string mkdir_err;
            if (!m_client.mkdir(d, mkdir_err))
                BOOST_LOG_TRIVIAL(debug) << "SysFilesUpdate: mkdir " << d << ": " << mkdir_err << " (expected when it already exists)";
        }
    }

    // ---- 9. Upload -------------------------------------------------------------------------------
    const uint64_t           total_bytes = std::max<uint64_t>(pkg.total_bytes(), 1);
    uint64_t                 sent_bytes  = 0;
    std::vector<std::string> attempted;
    const bool               can_roll_back = m_opts.rollback && m_have_originals;

    // After a failed or cancelled write: put the card back the way it was, or say exactly how not.
    auto undo = [&](const std::string &what_happened) -> std::string {
        if (attempted.empty())
            return what_happened + " " + _u8L("Nothing was changed on the printer.");
        if (!can_roll_back) {
            std::string msg = what_happened + " " +
                (boost::format(_u8L("%1% file(s) had already been written; the printer's configuration is part way "
                                    "through an update.")) % m_r.written.size()).str();
            if (!m_r.backup_dir.empty())
                msg += " " + (boost::format(_u8L("The previous files are in %1%.")) % m_r.backup_dir).str();
            return msg;
        }
        if (rollback(attempted))
            return what_happened + " " +
                (boost::format(_u8L("The %1% file(s) already written were put back as they were; nothing was "
                                    "changed on the printer.")) % m_r.written.size()).str();
        std::string msg = what_happened + " " +
            (boost::format(_u8L("Putting the previous files back failed for: %1%.")) % boost::join(m_r.rollback_failed, ", ")).str();
        if (!m_r.backup_dir.empty())
            msg += " " + (boost::format(_u8L("The previous files are in %1%.")) % m_r.backup_dir).str();
        return msg;
    };

    for (const auto &f : pkg.files) {
        if (cancelled()) {
            m_r.message = undo(_u8L("Cancelled."));
            mark_cancelled();
            status(100, m_r.message);
            return;
        }

        const uint64_t base = sent_bytes;
        status(int(50 + 40.0 * double(base) / double(total_bytes)), (boost::format(_u8L("Writing %1%")) % f.path).str());

        auto on_progress = [&](uint64_t sent, uint64_t, bool &cancel) {
            if (cancelled()) { cancel = true; return; }
            const int pct = int(50 + 40.0 * double(base + sent) / double(total_bytes));
            status(std::min(pct, 90), (boost::format(_u8L("Writing %1%")) % f.path).str());
        };

        // Counted before the request: a write that fails part way may still have changed the file.
        attempted.push_back(f.path);
        if (!m_client.upload(f.remote_path(), f.data, on_progress, err)) {
            if (cancelled() || err == "Cancelled") {
                m_r.message = undo(_u8L("Cancelled."));
                mark_cancelled();
                status(100, m_r.message);
                return;
            }
            fail(undo((boost::format(_u8L("Could not write %1%: %2%.")) % f.path % err).str()));
            return;
        }
        m_r.written.push_back(f.path);
        sent_bytes += f.data.size();
    }

    // ---- 10. Record what was installed -------------------------------------------------------------
    // Best effort: the files are already on the card, and a missing marker only costs the next
    // query its "installed version" line.
    status(92, _u8L("Recording the installed version"));
    {
        const std::string    json = make_sys_marker_json(pkg, m_r.written);
        std::vector<uint8_t> data(json.begin(), json.end());
        std::string          marker_err;
        if (!m_client.upload(std::string("0:/") + SYS_MARKER_PATH, data, nullptr, marker_err))
            BOOST_LOG_TRIVIAL(warning) << "SysFilesUpdate: could not write the version marker: " << marker_err;
    }

    m_r.outcome = SysFilesUpdateResult::Outcome::Success;

    // ---- 11. Restart ---------------------------------------------------------------------------------
    if (!m_opts.restart) {
        m_r.message = (boost::format(_u8L("Wrote %1% file(s). Restart the printer for the new configuration to take "
                                          "effect.")) % m_r.written.size()).str();
        status(100, m_r.message);
        m_client.disconnect();
        return;
    }

    status(93, _u8L("Restarting the printer"));
    if (!m_client.gcode("M999", err)) {
        // The files are installed; only the restart failed, so this is a warning rather than a
        // failed update.
        m_r.message = (boost::format(_u8L("Wrote %1% file(s), but the printer did not accept the restart command: "
                                          "%2%. Restart it yourself for the new configuration to take effect."))
                       % m_r.written.size() % err).str();
        status(100, m_r.message);
        m_client.disconnect();
        return;
    }

    auto should_cancel = [this]() { return cancelled(); };
    auto on_tick       = [&](int elapsed) {
        status(93 + std::min(elapsed / 20, 5), (boost::format(_u8L("Waiting for the printer to restart (%1%s)")) % elapsed).str());
    };
    if (!m_client.wait_for_reboot(m_opts.restart_timeout_seconds, should_cancel, on_tick, err)) {
        m_r.message = cancelled()
            ? (boost::format(_u8L("Wrote %1% file(s). Stopped waiting for the restart; the printer is restarting on "
                                  "its own.")) % m_r.written.size()).str()
            : (boost::format(_u8L("Wrote %1% file(s), but the printer did not come back after %2% seconds. Check it: "
                                  "a configuration file it cannot parse will stop it starting up."))
               % m_r.written.size() % m_opts.restart_timeout_seconds).str();
        // The files did get written, so this is not a failed install - but the user has to look at
        // the machine, so do not call it a success either.
        m_r.outcome = cancelled() ? SysFilesUpdateResult::Outcome::Cancelled : SysFilesUpdateResult::Outcome::Failed;
        status(100, m_r.message);
        return;
    }

    m_r.restarted = true;
    m_r.message   = (boost::format(_u8L("Wrote %1% file(s) and restarted the printer.")) % m_r.written.size()).str();
    if (!m_r.backup_dir.empty())
        m_r.message += " " + (boost::format(_u8L("The previous files were saved to %1%.")) % m_r.backup_dir).str();
    status(100, m_r.message);
    m_client.disconnect();
}

} // namespace

SysFilesRollbackPlan plan_sys_files_rollback(const std::vector<std::string> &attempted,
                                             const std::vector<std::string> &created_before)
{
    SysFilesRollbackPlan plan;
    // Newest first, so a later file that depends on an earlier one is undone before it.
    for (auto it = attempted.rbegin(); it != attempted.rend(); ++it) {
        const bool was_new = std::any_of(created_before.begin(), created_before.end(),
                                         [&it](const std::string &p) { return boost::iequals(p, *it); });
        (was_new ? plan.remove : plan.restore).push_back(*it);
    }
    return plan;
}

void run_sys_files_update(DuetClient                  &client,
                          const SysFileSourceFactory  &source_factory,
                          const SysFilesUpdateOptions &options,
                          SysFilesUpdateCtl           &ctl,
                          SysFilesUpdateResult        &result)
{
    result = SysFilesUpdateResult();
    Run(client, options, ctl, result).execute(source_factory);
}

} // namespace Slic3r
