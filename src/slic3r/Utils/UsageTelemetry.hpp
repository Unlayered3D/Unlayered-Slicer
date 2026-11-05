#ifndef slic3r_Utils_UsageTelemetry_hpp_
#define slic3r_Utils_UsageTelemetry_hpp_

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class AppConfig;
class Preset;
class PresetCollection;

// Usage diagnostics: what people use, which machines they run on, what fails.
//
// Events are batched in memory and POSTed as JSON to https://unlayered3d.com/api/slicer/telemetry
// (the site's app/lib/slicerTelemetry.ts validates and aggregates them; /admin/telemetry shows
// them).
//
// OPTIONAL, and off until the person has said yes (0.1.6). Whether they may run is decided by the
// privacy choice record, never by this module: PrivacyConsent::diagnostics_permitted() (the setup
// wizard's privacy page, the privacy dialog after an update, Preferences > Online). Before a
// choice, after a "no", and whenever the record is unreadable, nothing is collected, queued, kept
// or sent. The environment variable UNLAYERED_NO_TELEMETRY (anything non-empty) switches them off
// on a machine whatever anyone chose, for organisations that deploy the slicer centrally.
//
// What an event may carry is enforced by DiagnosticsAllowlist: unknown events and properties are
// dropped, and every string must look like a value rather than a path, file name or address.
//
// The rule every caller keeps: the person's work itself never leaves the machine. No model
// geometry, meshes, thumbnails or G-code, no file, object, project or user-preset names, no paths,
// no free text, no account. Since 0.1.6 the values of changed settings are sent (custom G-code,
// notes, hosts, credentials, paths and multi-line values excluded: Plater usage_private_key) and
// coarse per-part measurements (rounded sizes and volumes, never the shape). A batch carries a
// random per-install id, a random per-session id, the app version and platform, and named events
// whose properties are short strings, numbers, booleans and lists of short strings.
//
// Crashes are detected on the next launch: each running session keeps a small marker file with
// its process id, deleted on a clean exit; a marker whose process is gone means that session died.
// The marker also holds what the app was doing (set_phase) and what ended it:
//  - Windows, an unhandled exception: its code and the module and offset it happened at
//    ("UnlayeredSlicer.dll+0x1a2b3c": a file name, never a path);
//  - macOS / Linux, a fatal signal: its name ("SIGSEGV"); on macOS also where, as offsets into the
//    executable ("UnlayeredSlicer.arm64+0x2f1a3c>+0x2e1b00": the fault, then the slicer's own
//    frames below it);
//  - anywhere, an uncaught C++ exception: "terminate";
//  - anywhere, "hang": the main thread had stopped responding (heartbeat()) when the process
//    died, which is what a force-quit of a frozen app looks like.
// No exception at all means the process was killed from outside while responsive (Force Quit,
// kill -9), or the power went. A session the operating system ended (log off, shut down, and on
// macOS an ordinary Quit, which never reaches shutdown()) is not reported as a crash.
//
// Since 0.1.10 the marker also names the session's log file ("log", a file name in data_dir()/log),
// and every crash report that is sent (unasked for a sharer, or after "Send" in the crash report
// dialog) also sends the end of that log: its last 256 KB, from a complete line, scrubbed on this
// computer by scrub_log_for_upload() (CrashLogScrub.hpp), POSTed once in the background, never
// retried, to /api/slicer/crash-log on the diagnostics URL's own site, as
//   {"v":1, "report_id", "version", "os", "channel", "crash":{phase, exception, at, uptime_s},
//    "log", "log_bytes_original", "truncated"}.
// The crash event carries the same random report_id. Nothing is read or sent when the person
// declines, under UNLAYERED_NO_TELEMETRY, or when that session's log cannot be found.
//
// Thread safety: record()/count() may be called from any thread. start(), refresh() and
// shutdown() run on the main thread (they touch AppConfig).
namespace UsageTelemetry {

// Once, on the main thread, after AppConfig is loaded. `first_run`: the app config did not exist
// before this launch (a new install). Detects a previous crash and loads unsent batches.
void start(AppConfig &config, bool first_run);

bool enabled();
// Reads the privacy choice again and applies it at once: called after every change to it. Turning
// off (a withdrawal, a "no", an organisation opt-out) drops everything queued, kept on disk or not
// yet answered by the site; turning on starts collecting from now on (nothing from before).
void refresh(AppConfig &config);
// True when UNLAYERED_NO_TELEMETRY switched it off, so Preferences can say why the box is locked.
bool disabled_by_environment();

// Crashes start() found that usage diagnostics did not cover (not shared in that session, or not
// now), as the allow-listed "crash" properties that would be sent. Empty when there were none, when
// the organisation switched diagnostics off, or before LegalApprovals::crash_report_prompt.
std::vector<nlohmann::json> unreported_crashes();
// The person's answer (GUI: ask_crash_report_once). Yes: those reports are sent once, in a batch of
// their own with fresh random ids, whatever the diagnostics choice. Either way they are forgotten.
void answer_unreported_crashes(bool send);

bool is_first_run();

// The random per-install id, only while diagnostics run and are permitted; empty otherwise (no
// choice yet, declined, UNLAYERED_NO_TELEMETRY, or not started yet). The update check sends it so the site can count
// different installs; switching diagnostics off stops that too.
std::string install_id();

// True for Unlayered's own copies: a dev-edition build, a copy run from a build tree rather than
// installed (Windows: no Uninstall.exe beside it; macOS: not inside an .app; Linux: neither an
// AppImage nor a Flatpak), or UNLAYERED_INTERNAL set in the environment (to mark an installed
// release copy). Their diagnostics report channel "dev" and their update check says
// X-Unlayered-Edition: dev, so the site's customer counts leave them out.
bool is_internal_copy();

// The channel diagnostics report: "dev" for an internal copy or a dev version, "beta" for an
// alpha/beta/rc version, else "release". Pure, for tests; the app passes is_internal_copy().
std::string channel_for(const std::string &version, bool internal);

// Queues one event, if diagnostics are permitted. `props` must be a JSON object of short strings,
// numbers, booleans and lists. `counts` (optional) is an object of non-negative integer tallies the
// site adds into its feature totals, e.g. {"parts:zaa_enabled": 4}. Both pass through
// DiagnosticsAllowlist::filter first; an event that is not on the allowlist is not queued.
void record(const std::string &name, nlohmann::json props = nlohmann::json::object(), nlohmann::json counts = nlohmann::json());

// Bumps a per-session tally (sent once, with session_end): "tab:preview", "gizmo:cut", ...
void count(const std::string &feature, int by = 1);

// Sends what is queued, in the background. Called every few minutes by the app.
void flush();

// An exception the main loop caught (GUI_App::OnExceptionInMainLoop, UiException): its type and a
// short, path-free message, kept for the crash marker so a crash that follows names its cause (the
// "crash" event's "what"). Sends nothing itself.
void note_exception(const std::string &type, const std::string &what);

// What the app is doing, kept in the crash marker: "startup", "running", "slicing", "closing".
// A crash reported at the next launch says which phase it happened in.
void set_phase(const char *phase);

// The operating system is ending the session (log off, shut down) with the app still open. The
// process may be killed without reaching shutdown(); the next launch does not count it as a crash.
void end_session_by_os();

// From the main thread every few seconds (a timer): the event loop is running. When the beats
// stop for 30 s the crash marker says "hang" until they resume.
void heartbeat();

// "module.dll+0x1a2b" for a code address: the module's file name (never its path) and the offset
// into it. Empty when it cannot be told (or off Windows). For reporting where a crash happened.
std::string code_location(const void *address);

// At exit: records session_end, sends it with a short timeout (or saves it for the next launch),
// and removes this session's crash marker.
void shutdown();

// What a preset may be called in a report: its own name if it is a system preset, else the system
// preset it inherits from, else "custom". The names people give their own presets are never sent.
std::string preset_base_name(const PresetCollection &collection, const Preset &preset);

// A slicing error message made safe to send: its first line, without quoted text, paths, or any
// of `names` (the model's object and volume names), cut to 160 characters.
std::string redact_message(const std::string &message, const std::vector<std::string> &names);

// One Windows Application-log event as XML (EvtRenderEventXml): if it is "Application Error" (1000)
// or "Application Hang" (1002) for `exe_name`, process `pid`, started at `started_ms` (Unix ms; 0 =
// do not check), sets `exception` ("0xC0000409", or "hang_closed") and `at` ("module.dll+0x2a53fd")
// and returns true. Public for the tests; start() uses it on Windows for a session marker that has
// no exception of its own.
bool match_crash_event(const std::string &event_xml, long pid, int64_t started_ms, const std::string &exe_name,
                       std::string &exception, std::string &at);

} // namespace UsageTelemetry
} // namespace Slic3r

#endif // slic3r_Utils_UsageTelemetry_hpp_
