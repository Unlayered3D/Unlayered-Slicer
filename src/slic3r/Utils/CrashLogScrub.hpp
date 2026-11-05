#ifndef slic3r_Utils_CrashLogScrub_hpp_
#define slic3r_Utils_CrashLogScrub_hpp_

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

// Unlayered 0.1.10: the end of a crashed session's log, sent with its crash report
// (UsageTelemetry, POST /api/slicer/crash-log). Everything here runs on the person's computer,
// before anything is sent: the log is scrubbed on the device, never on the site.
//
// What scrub_log_for_upload() removes, line by line:
//  - the home folder, in every spelling: the ones this computer uses (ScrubContext::home_dirs, with
//    either slash and with doubled backslashes) and any C:\Users\<name>, C:/Users/<name>,
//    /Users/<name>, /home/<name>, %USERPROFILE%, %HOMEPATH%, $HOME -> "<home>" (case-insensitive);
//  - e-mail addresses -> "<email>";
//  - credentials: "Bearer xxx" / "Basic xxx", the value of anything named like a token, secret,
//    password, api key, auth, cookie or credential ("token=...", "\"api_key\": \"...\""), a
//    "key=..." parameter (unless its value is a setting's name, "key=wall_loops"), the user and
//    password in a URL (scheme://user:pass@), and any run of more than 32 letters and digits that
//    has both (hex digests, base64 keys, JWTs, uuids) -> "<redacted>";
//  - model, project, G-code and image file names (.stl .3mf .obj .step .stp .amf .ply .gcode .bgcode
//    .gcode.3mf .svg .png .jpg .jpeg .drc), also with spaces in them after a folder separator or
//    inside quotes -> "<file>" plus the extension ("<file>.3mf", "<file>.gcode.3mf");
//  - IPv4 addresses (except 127.x.x.x, 0.0.0.0 and anything with leading zeros, which is a version),
//    IPv6 addresses (except ::1) and MAC addresses -> "<ip>";
//  - the computer's name (ScrubContext::host_names) anywhere -> "<host>";
//  - the login name (ScrubContext::user_names) anywhere -> "<user>".
// Names and host names of 3 characters or more are replaced wherever they occur (case-insensitive);
// shorter ones only as a whole word. A line longer than 8 KB is replaced whole with
// "<long line removed>", and a line the scrubber cannot process is replaced with "<line removed>":
// when in doubt, a line is dropped rather than sent.
struct ScrubContext
{
    std::vector<std::string> home_dirs;  // "C:\Users\Jane Smith", "/Users/jane"
    std::vector<std::string> user_names; // "jane", "Jane Smith"
    std::vector<std::string> host_names; // "JANE-PC", "janes-macbook.local"

    // This computer: USERPROFILE / HOME / HOMEDRIVE+HOMEPATH, USERNAME / USER / LOGNAME, the login
    // name, the home folder's own name, COMPUTERNAME / HOSTNAME and the host name.
    static ScrubContext from_environment();
};

std::string scrub_log_for_upload(std::string text, const ScrubContext &context);

// The last `max_bytes` of a log file, starting at a complete line (the partial first line of the
// cut is dropped). False when the file cannot be read. `path_utf8` is a UTF-8 path.
struct LogTail
{
    std::string text;
    uint64_t    original_bytes{0}; // the file's whole size
    bool        truncated{false};  // the file was longer than max_bytes
};
constexpr size_t CRASH_LOG_MAX_BYTES = 256 * 1024;
bool read_log_tail(const std::string &path_utf8, LogTail &out, size_t max_bytes = CRASH_LOG_MAX_BYTES);

// The crashed session's log file in `log_dir_utf8` (data_dir()/log). `file_name` is the name the
// session's crash marker recorded ("debug_Mon_Nov_02_10_00_00_1234.log.0"), possibly empty for a
// marker from before 0.1.10. When that file has rotated (".1", ".2" beside it) the newest part is
// taken. Without a name, the newest "debug_*_<pid>.log.<n>" written since `started_ms` (Unix ms)
// is. Empty when there is none. Returns a UTF-8 path.
std::string find_session_log(const std::string &log_dir_utf8, const std::string &file_name, long pid, int64_t started_ms);

// The crash-log endpoint on the same site as the usage diagnostics endpoint:
// "https://unlayered3d.com/api/slicer/telemetry" -> "https://unlayered3d.com/api/slicer/crash-log",
// "http://127.0.0.1:3000/x" -> "http://127.0.0.1:3000/api/slicer/crash-log". Empty for a URL
// without a scheme and host.
std::string crash_log_url(const std::string &telemetry_url);

} // namespace Slic3r

#endif // slic3r_Utils_CrashLogScrub_hpp_
