#ifndef slic3r_BambuConnect_hpp_
#define slic3r_BambuConnect_hpp_

#include <cstdint>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace BambuConnect {

// Hand-off of a sliced plate to Bambu Connect, Bambu's desktop app for third-party slicers.
// Unlayered exports the selected plate as a .gcode.3mf into a job directory it owns and asks the
// OS to open a bambu-connect:// URI; Connect then picks the printer and starts the print, and the
// person monitors it in Bambu Handy. Unlayered gets no callback: a successful launch only means
// the OS accepted the URI, never that the file was imported or that anything is printing.
//
// The plan this follows: claude-comms files/bambu-connect/Unlayered_Bambu_Connect_Implementation_Test_Plan.md

// The app-config key of the experimental switch (Preferences).
inline constexpr const char* CONFIG_KEY = "enable_bambu_connect";

// The action is offered only where a Bambu Connect app exists: Windows and macOS.
bool is_supported_platform();

// UTF-8 percent encoding of one query value: everything but ALPHA / DIGIT / "-" / "." / "_" / "~"
// becomes %XX (upper-case hex), so "/", ":", " ", "#", "&", "+" and "%" are all encoded.
std::string encode_query_value(const std::string& utf8);

// bambu-connect://import-file?path=<abs path>&name=<display name>&version=1.0.0
// The path is the absolute local path with forward slashes, never a file:// URL. "version" is the
// integration version of this URI format, not Unlayered's version. The format is the candidate
// from the plan and still has to be confirmed against an installed Connect (phase zero, test D06):
// keep it in this one function, and never try several variants in a row (duplicate imports).
std::string build_import_uri(const boost::filesystem::path& package, const std::string& display_name);

struct GcodeEntry
{
    std::string   name;
    std::uint64_t bytes = 0;
    std::string   sha256;
};

// SHA-256 of a whole file, lower-case hex; empty on a read error.
std::string sha256_file(const boost::filesystem::path& file);

// The embedded *.gcode members of a sliced package with their hashes. Compare these, not the whole
// archive, between two exports: ZIP timestamps change the archive hash. Empty with `error` set if
// the archive cannot be read or holds no G-code.
std::vector<GcodeEntry> gcode_entries(const boost::filesystem::path& package, std::string& error);

// <data_dir>/bambu_connect_jobs. Jobs live here, outside the temp directory, and are never removed
// automatically: Connect may read the file long after the launch (sign-in, printer choice) and
// after Unlayered has exited.
boost::filesystem::path jobs_root();

// A new, empty, uniquely named job directory under jobs_root(), e.g. 20261102-231500-3f9a.
// Throws on failure.
boost::filesystem::path create_job_dir();

struct JobRecord
{
    std::string             job_id;
    std::string             created_at; // UTC, ISO 8601
    std::string             slicer_version;
    int                     plate_index = 0;
    std::string             display_name;
    std::string             printer_model;
    std::string             nozzle;
    std::string             build_plate;
    std::vector<std::string> filaments;
    boost::filesystem::path package_path;
    std::string             package_sha256;
    std::vector<GcodeEntry> gcode_entries;
    std::string             handoff_state; // "ready", "handoff_requested", "launch_failed"
};

// Writes <job dir>/job.json. No credentials or tokens are ever part of a record.
void write_job_record(const boost::filesystem::path& job_dir, const JobRecord& record);

// After launch(): sets "handoff_state" ("handoff_requested" or "launch_failed") and
// "last_launch_attempt_at" (UTC, ISO 8601) in an existing <job dir>/job.json, keeping the rest.
// A reopened job updates the same record. False if the record cannot be read or written.
bool record_launch_attempt(const boost::filesystem::path& job_dir, const std::string& handoff_state);

enum class LaunchResult {
    Requested, // the OS accepted the URI; nothing more is known
    NoHandler, // no bambu-connect:// handler is registered (Connect not installed)
    Failed,    // the OS refused or failed to open the URI
};

// Whether something is registered for bambu-connect:// (Connect installed): the same precheck
// launch() makes, so the slicer can explain what to install before it exports anything. Not proof
// that an import will work.
bool is_installed();

// Whether "Open in Bambu Connect" is offered, given the stored value of CONFIG_KEY ("" when the person never
// set the switch). A switch the person set wins; otherwise it is ON on Windows and macOS, whether or not
// Connect is installed: without Connect the item opens the guide that says where to get it (11/02,
// 2026-11-03 for 0.1.10 -- "only when installed" hid it from exactly the people who need the guide).
bool is_enabled(const std::string& stored_switch);

// Opens the URI with the OS URL handler: ShellExecuteW on Windows, NSWorkspace (through
// wxLaunchDefaultBrowser) on macOS. The URI goes straight to the API, never through a shell.
LaunchResult launch(const std::string& uri);

#ifdef __APPLE__
// The application Launch Services would open a bambu-connect:// URI with, or "" when none is
// registered (Connect not installed). A precheck only: a handler is no proof the import works.
// BambuConnectMac.mm.
std::string mac_handler_app_path();
#endif

} // namespace BambuConnect
} // namespace Slic3r

#endif // slic3r_BambuConnect_hpp_
