#ifndef slic3r_SysFilesUpdater_hpp_
#define slic3r_SysFilesUpdater_hpp_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "DuetClient.hpp"
#include "SysFilesBundle.hpp"

namespace Slic3r {

// One run of the Config updater against one printer: connect, read what is installed, fetch the
// package, and - unless only asked to look - check it, back up what it replaces, write it, record
// it and restart the board. GUI/Jobs/SysFilesUpdateJob runs this on a worker thread for the dialog;
// it has no wx in it so that the same code can be driven by a test against a fake board.
//
// Nothing on an SD card is atomic: every file is a separate request. So before the first write the
// run reads every file it is about to replace (when the transport can read files back exactly), and
// if a write fails or the run is cancelled part way, it puts those back and removes the files it had
// created. A printer is then left as it was, not with half a release on it.

struct SysFilesUpdateResult
{
    enum class Outcome { Success, UpToDate, Failed, Cancelled, QueryOk };

    // One entry of the package, for the dialog's file list. The payload stays in the run.
    struct FileInfo
    {
        std::string path;
        uint64_t    size = 0;
    };

    Outcome     outcome = Outcome::Failed;
    std::string message;

    // Filled by every run, so the dialog can describe the printer.
    DuetClient::BoardInfo board;
    bool                  board_known = false;
    SysFilesInstalled     installed;

    // What the source offered.
    std::string           available_version;
    std::string           available_date;
    std::string           available_machine;
    std::string           available_notes;
    std::string           available_origin;
    std::vector<FileInfo> available_files;
    // path -> why it was left out of the package.
    std::vector<std::pair<std::string, std::string>> skipped;
    // The source was a whole SD card image; only its configuration folders were offered.
    bool from_full_sd_card = false;
    // The firmware the whole package needs (sys_required_firmware), where that came from, and
    // whether the board is known to be older. An install checks again on the ticked files.
    std::string required_firmware;
    std::string required_firmware_reason;
    bool        firmware_too_old = false;

    // Filled by an install run.
    std::vector<std::string> written;
    // Files that did not exist on the card before this run (a rollback deletes them).
    std::vector<std::string> created;
    std::string              backup_dir;
    bool                     restarted = false;
    // A write failed or the run was cancelled after something had been written, and the previous
    // files were put back. rollback_failed lists any that could not be (the backup still has them).
    bool                     rolled_back = false;
    std::vector<std::string> rollback_failed;
};

struct SysFilesUpdateOptions
{
    // Empty means "everything in the package". Paths are card-relative, e.g. "sys/config.g".
    std::vector<std::string> selected;
    // Save each file that is about to be overwritten to this computer first. There is no undo on the
    // card, so this is the way back to a working configuration after a successful but unwanted update.
    bool backup = true;
    // Folder the per-printer backup folders go in; empty means <data_dir>/printer_backups.
    std::string backup_root;
    // Put the previous files back when a write fails or the run is cancelled part way.
    bool rollback = true;
    // Send M999 afterwards. config.g is only read at startup, so without this the printer keeps
    // running its old configuration until somebody power cycles it.
    bool restart = true;
    int  restart_timeout_seconds = 120;
    // Install even when the card already reports this version.
    bool force = false;
    // Connect, read what is installed and resolve what is available, then stop.
    bool query_only = false;
};

// Progress and cancellation for a run; the job maps it onto its Ctl.
class SysFilesUpdateCtl
{
public:
    virtual ~SysFilesUpdateCtl() = default;
    virtual void status(int percent, const std::string &message) = 0;
    virtual bool cancelled() const = 0;
};

using SysFileSourceFactory = std::function<std::unique_ptr<SysFileSource>()>;

// The whole run. `client` is not yet connected; the run connects it and disconnects it again.
void run_sys_files_update(DuetClient                  &client,
                          const SysFileSourceFactory  &source_factory,
                          const SysFilesUpdateOptions &options,
                          SysFilesUpdateCtl           &ctl,
                          SysFilesUpdateResult        &result);

// The card files a rollback would restore and remove, given what was read before the first write.
// Exposed for tests. `attempted` is every file a write was attempted for, in order.
struct SysFilesRollbackPlan
{
    std::vector<std::string> restore; // existed before: upload the saved bytes again
    std::vector<std::string> remove;  // did not exist before: delete
};
SysFilesRollbackPlan plan_sys_files_rollback(const std::vector<std::string> &attempted,
                                             const std::vector<std::string> &created_before);

} // namespace Slic3r

#endif // slic3r_SysFilesUpdater_hpp_
