#ifndef slic3r_SysFilesBundle_hpp_
#define slic3r_SysFilesBundle_hpp_

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "PrinterUpdateCheck.hpp"

namespace Slic3r {

// The printer's configuration - config.g, the homing and tool-change macros, the macros the user
// presses in DWC, filament scripts - lives as ordinary files on the board's SD card. This is the
// firmware updater's counterpart for those: it takes a set of files from a release or from a
// folder on disk and writes them to the card over the same rr_* HTTP API.
//
// Deliberately not part of FirmwareBundle: an image is flashed by the board and identified by the
// object model, whereas these files are just copied, and nothing on the board describes what
// version of them is installed. That last gap is closed by a marker file this code writes.

// Card-relative path of the marker recording what was last installed. It is written after a
// successful install and read back to answer "what is on this printer?"; nothing depends on it
// being present, so a hand-loaded card simply reports an unknown version.
extern const char *SYS_MARKER_PATH; // "sys/unlayered-sys.json"

// One file destined for the SD card.
struct SysFile
{
    // Card-relative, always forward slashes, always under one of the allowed roots, e.g.
    // "sys/config.g" or "macros/Load Filament".
    std::string          path;
    std::string          sha256;  // lowercase hex; empty means "not declared"
    std::vector<uint8_t> data;
    // Where this came from, for the UI and the log: a zip entry name or a path on disk.
    std::string          origin;

    std::string remote_path() const { return "0:/" + path; }
};

struct SysFilePackage
{
    // Free-form, whatever the package calls itself. Empty means "unknown", which is never treated
    // as up to date - the same rule the firmware updater applies to a hand-picked file.
    std::string version;
    std::string date;
    // Which machine the package was written for. Shown to the user and recorded in the marker,
    // never enforced: one Duet board model covers every Unlayered machine, so there is nothing on
    // the printer that could be checked against it without lying about the strength of the check.
    std::string machine;
    std::string notes;
    std::string origin;   // human readable source description
    // Oldest board firmware these files work on, e.g. "3.7.0-beta.1+unlayered.15". Unlike
    // "machine" this one IS enforced: a config.g that uses a command the running firmware does
    // not know stops at that line and skips everything after it, heaters and tools included.
    // Empty when the manifest does not say; sys_required_firmware() also reads it off the files.
    std::string min_firmware;

    std::vector<SysFile> files;

    // Files that were found but left out, with the reason. Surfaced rather than dropped quietly:
    // "it did not upload my file" is otherwise impossible to diagnose.
    std::vector<std::pair<std::string, std::string>> skipped;

    // The source was a whole SD card image (sys_looks_like_full_sd_card): only its configuration
    // folders were read, and its firmware, print files and web interface were left out (listed in
    // `skipped`). The dialog says so before anything is installed.
    bool from_full_sd_card = false;

    uint64_t       total_bytes() const;
    const SysFile *find(const std::string &path) const;
};

// What the marker file on the card says. All fields are empty/zero when there is no marker.
struct SysFilesInstalled
{
    bool        known = false;
    std::string version;
    std::string date;         // the package's own date
    std::string machine;
    std::string installed_at; // when this slicer wrote the files
    std::string origin;
    int         file_count = 0;
};

// The directories a package is allowed to write. 0:/firmware is deliberately absent: that is the
// firmware updater's business, and 0:/gcodes is not configuration.
const std::vector<std::string> &sys_allowed_roots(); // {"sys", "macros", "filaments"}

// Normalises a path taken from an archive or from disk (separators, "./", duplicate slashes) and
// maps it onto a card-relative path: a path already rooted at one of the allowed directories is
// kept, anything else is placed in sys/. Returns false, with a reason, for a path that escapes the
// card (".." components, absolute paths, drive letters).
bool sys_map_path(const std::string &relative, std::string &card_path_out, std::string &reason_out);

// True when a path is a file the packaging should ignore rather than upload: editor and backup
// droppings, VCS metadata, and the marker file, which this code owns.
bool sys_is_ignored_file(const std::string &relative_path);

// File names (matched case-insensitively, in any folder) that describe ONE physical printer rather
// than the machine model: its measured calibration, plate offsets, name, network, setup progress,
// logs and odometer. tools/make_sys_bundle.py leaves the same names out of a package, and the
// printer's own sys/factoryreset.g erases them; tools/verify_sys_updater.py checks the lists agree.
const std::vector<std::string> &sys_machine_specific_names();

// True for a card path whose file name is one of sys_machine_specific_names(). Such a file is never
// installed, from any source: copying it from another card hands this printer another one's
// calibration or identity, and no release has a reason to carry one.
bool sys_is_machine_specific(const std::string &card_path);

// True when a package (paths relative to its root, any wrapper folder already removed) is a whole
// SD card image rather than a configuration package: it has sys/ or macros/ AND something only a
// card carries - firmware/, gcodes/, www/, menu/, or a README / START HERE note at the root. Such
// a package is read for its configuration folders (sys_allowed_roots()) only; everything else is
// left out BEFORE the size limits, which still apply to every file that is read.
bool sys_looks_like_full_sd_card(const std::vector<std::string> &paths);

// Files a whole SD card image carries that are never installed from one, on top of
// sys_machine_specific_names(): today sys/config.g, because a card's is the generic ship
// config.g and would replace the printer's tuned one. Configuration releases leave it out too.
// An ordinary configuration zip or folder may still install sys/config.g.
bool sys_is_card_kept_config(const std::string &card_path);

// True for a path that sits inside one of sys_allowed_roots(), e.g. "macros/Load Filament".
bool sys_is_config_folder_path(const std::string &relative_path);

// Parses the manifest.json carried by a package:
//   { "version": "...", "date": "...", "machine": "...", "notes": "...",
//     "minFirmware": "3.7.0-beta.1+unlayered.15",
//     "files": [ { "path": "sys/config.g", "sha256": "..." }, ... ] }
// Everything except "version" is optional. "files" is optional too; when it is present it is
// authoritative and anything else in the package is left alone.
bool parse_sys_manifest(const std::string &json, SysFilePackage &pkg_out, std::string &error_out);

// The N of an Unlayered firmware version "...+unlayered.N", or -1 when there is none (stock RRF).
// The fork number is the fork's one monotonic counter, so it is what versions are ordered by -
// semver ignores build metadata and would call every +unlayered.N equal.
int unlayered_fork_number(const std::string &firmware_version);

// The firmware a set of files needs: the manifest's min_firmware, raised by anything the files
// themselves use that only a later release understands (today: M558 P12, the differential Z
// probe, from +unlayered.12). Reading the files as well is what covers a folder, or a zip built
// before manifests carried the field. Empty when nothing is known. reason_out, when given, says
// where the requirement came from.
std::string sys_required_firmware(const SysFilePackage &pkg, std::string *reason_out = nullptr);

// False only when `running` is known to be older than `required`. An empty requirement, or a
// pair that cannot be compared, is not a reason to refuse.
bool sys_firmware_satisfies(const std::string &running, const std::string &required);

// Verifies declared checksums and that every target path is inside an allowed root.
bool validate_sys_package(const SysFilePackage &pkg, std::string &error_out);

// The JSON written to SYS_MARKER_PATH after a successful install.
std::string make_sys_marker_json(const SysFilePackage &pkg, const std::vector<std::string> &installed_paths);
bool        parse_sys_marker(const std::string &json, SysFilesInstalled &out);

// Reports progress. Returning false cancels the fetch.
using SysProgressFn = std::function<bool(const std::string &stage, uint64_t done, uint64_t total)>;

class SysFileSource
{
public:
    virtual ~SysFileSource() = default;
    // Retrieves the metadata and every file payload. Payloads here are a few kilobytes of G-code,
    // so unlike firmware there is no reason to separate a metadata-only pass.
    virtual bool fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out) = 0;
    virtual std::string description() const = 0;
};

// A folder or a .zip on disk. This is the bench workflow: point it at a machine's sys directory,
// or at a zip built by tools/make_sys_bundle.py, and push.
class LocalSysFileSource : public SysFileSource
{
public:
    explicit LocalSysFileSource(std::string path);
    bool        fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out) override;
    std::string description() const override;

private:
    std::string m_path;

    bool fetch_from_zip(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out);
    bool fetch_from_directory(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out);
};

// A GitHub release carrying a zip of the configuration. Same reasoning as the firmware source: it
// decouples a config fix from a slicer release.
//
// The release is expected to have one zip asset (named sys.zip when there are several). A
// manifest.json inside the zip supplies the version, notes and checksums; without one the release
// tag stands in for the version and every file in the zip is installed.
class GitHubSysFileSource : public SysFileSource
{
public:
    struct Options
    {
        // Base of the releases API. Overridable so the updater can be pointed at a local server
        // for testing, or at our own host if these artifacts stop being public. Anything serving
        // the GitHub releases JSON shape works; no trailing slash.
        std::string api_base = "https://api.github.com";
        std::string owner = "Unlayered3D";
        std::string repo  = "printer-config";
        // Empty means "the newest published release".
        std::string tag;
        bool        allow_prerelease = true;
        // The customer's Unlayered credential: the configuration feed needs their account sign-in.
        // Sent only to requests on its own site (FeedCredential::token_for), never to GitHub.
        FeedCredential credential;
        // Preferred asset name; any .zip asset is accepted when this one is not present.
        std::string asset_name = "sys.zip";
        // Refuse a release whose archive does not declare a SHA-256 for every file it installs
        // (a manifest.json from tools/make_sys_bundle.py always does). A release is written to a
        // customer's printer without anyone looking at it first, so it must prove it arrived
        // intact; a folder or zip picked by hand is the bench workflow and is not held to this.
        bool require_checksums = true;
    };

    // The release the feed offers, resolved without downloading its archive.
    struct Release
    {
        // Free-form on the Unlayered feed. The version that counts is the one in the archive's
        // manifest.json, which is only known after fetch().
        std::string tag;
        // The tag without a leading "v": the version only for an archive with no manifest.
        std::string version;
        std::string notes;
        std::string asset_name;
        std::string asset_url;
        // From the asset's "digest" ("sha256:<hex>"; lowercase hex here) when the feed gives one.
        // The downloaded archive must match it before anything in it is read.
        std::string asset_sha256;
    };

    explicit GitHubSysFileSource(Options opts);
    bool        fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out) override;
    std::string description() const override;

    // One request: which release is current and where its archive is. fetch() starts with it.
    bool fetch_release(Release &out, std::string &error_out);

    // The release list's HTTP status from the last fetch_release()/fetch(), 0 when the feed never
    // answered - so a caller can tell "sign in" (401) and "no access" (403) from "unreachable".
    unsigned last_http_status() const { return m_last_status; }

private:
    Options  m_opts;
    unsigned m_last_status = 0;
};

} // namespace Slic3r

#endif // slic3r_SysFilesBundle_hpp_
