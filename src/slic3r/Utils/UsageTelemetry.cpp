#include "UsageTelemetry.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include <cstdio>
#include <cstring>
#include <exception>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "DiagnosticsAllowlist.hpp"
#include "Http.hpp"
#include "PrivacyConsent.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winevt.h>
#pragma comment(lib, "wevtapi.lib")
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/ucontext.h>
#endif

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r {
namespace UsageTelemetry {

namespace {

// Undocumented override, like "version_check_url": "usage_diagnostics_url" in the app config,
// for pointing a test copy at a local site.
const char *DEFAULT_URL = "https://unlayered3d.com/api/slicer/telemetry";

constexpr size_t FLUSH_AT_EVENTS      = 50;
constexpr size_t MAX_EVENTS_PER_BATCH = 100; // the site rejects more
// The site also rejects a body over 64 KB. Since 0.1.6 a slice event carries setting values and part
// measurements (up to ~7 KB), so a batch is cut by size as well, with room left for the envelope.
constexpr size_t MAX_BATCH_EVENT_BYTES = 48 * 1024;
constexpr size_t MAX_QUEUED           = 400; // offline for a long session: drop, never grow
constexpr size_t MAX_PENDING_BATCHES  = 20;  // unsent batches kept on disk between launches
constexpr double EXIT_BUDGET_S        = 6.0; // the longest shutdown() may hold up closing the app
// The main thread has not beaten (heartbeat()) for this long: the marker says "hang" until it does.
constexpr int64_t HANG_AFTER_MS = 30 * 1000;
// How often the watchdog refreshes the marker's "alive", so a dead session's uptime is known to
// the minute rather than to the 5-minute flush.
constexpr int64_t ALIVE_EVERY_MS = 60 * 1000;

struct State
{
    std::mutex        mutex;
    bool              started{false};
    std::atomic<bool> on{false}; // the privacy choice permits diagnostics (refresh())
    // Slicing improvement program member (PrivacyConsent::program_permitted): stamped into every batch
    // as it is built, so the site keeps AI-training records only from members (notice 2026-10-v3).
    bool              program{false};
    bool              env_off{false};
    bool              first_run{false};

    std::string url, install, session, version, channel, os;
    std::chrono::steady_clock::time_point started_at;
    int64_t                               started_ms{0};
    std::string                           phase{"startup"};

    std::vector<json>          queue;
    std::map<std::string, int> counts;
    // Crashes found at start() that diagnostics did not cover: offered to the person, once
    // (unreported_crashes / answer_unreported_crashes). Allow-listed "crash" props, newest last.
    std::vector<json>          crash_offers;
    std::vector<std::string>   pending;   // batch bodies a failed send left for later
    std::map<int, std::string> in_flight; // bodies being sent in the background
    int                        next_id{0};

    // Hang watchdog (heartbeat(), watchdog_loop()).
    std::atomic<int64_t>    last_beat_ms{0}; // steady clock; 0 until the first beat
    int64_t                 hang_after_ms{HANG_AFTER_MS};
    std::atomic<bool>       hung{false};
    std::string             hang_at; // where the main thread was when it stopped beating (Windows)
    bool                    watchdog_running{false};
    std::condition_variable watchdog_wake;
    int64_t                 alive_written_ms{0}; // steady clock
};

// Never destroyed. On macOS a Quit ends the process with exit() from inside AppKit, without
// shutdown() (see GUI_App::shutdown): the watchdog thread is still running then, and a State torn
// down by the static destructors under it would be a crash at exit, reported as one.
State &state()
{
    static State *s = new State;
    return *s;
}

int64_t steady_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string new_uuid() { return boost::uuids::to_string(boost::uuids::random_generator()()); }

bool is_uuid(const std::string &s)
{
    static const std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    return std::regex_match(s, re);
}

fs::path usage_dir() { return fs::path(data_dir()) / "usage"; }
fs::path pending_path() { return usage_dir() / "pending.json"; }
fs::path marker_path(const std::string &session) { return usage_dir() / ("session-" + session + ".json"); }

std::string os_name()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#else
    return "linux";
#endif
}

// The site groups by channel so a developer's own copy never reads as a customer.
std::string channel_of(std::string version)
{
    std::transform(version.begin(), version.end(), version.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (version.find("dev") != std::string::npos)
        return "dev";
    for (const char *pre : {"alpha", "beta", "rc"})
        if (version.find(pre) != std::string::npos)
            return "beta";
    return "release";
}

// UNLAYERED_MOD -- whether this executable is an installed copy. Only installers put the slicer
// where a customer runs it; anything else is a developer's own build.
bool runs_installed()
{
    boost::system::error_code ec;
    const boost::filesystem::path exe = boost::dll::program_location(ec);
    if (ec || exe.empty())
        return true; // cannot tell: count it as a customer's, as before
#if defined(_WIN32)
    // The NSIS installer (per-user or per-machine) puts Uninstall.exe beside the executable.
    return boost::filesystem::exists(exe.parent_path() / "Uninstall.exe", ec);
#elif defined(__APPLE__)
    // Inside an .app, but not one in a build or source tree: a Mac build makes .app bundles too
    // (build/<arch>/src/Release/UnlayeredSlicer.app, build/universal/..., the slic3rutils_tests
    // test host), and those sit below a CMakeCache.txt or CMakeLists.txt, which no customer's
    // copy does.
    bool in_bundle = false;
    for (boost::filesystem::path p = exe.parent_path(); ! p.empty() && p != p.root_path(); p = p.parent_path()) {
        if (p.extension() == ".app")
            in_bundle = true;
        if (boost::filesystem::exists(p / "CMakeCache.txt", ec) || boost::filesystem::exists(p / "CMakeLists.txt", ec))
            return false;
    }
    return in_bundle;
#else
    const char *appimage = std::getenv("APPIMAGE");
    const char *flatpak  = std::getenv("FLATPAK_ID");
    return (appimage && *appimage) || (flatpak && *flatpak);
#endif
}

long current_pid()
{
#ifdef _WIN32
    return long(::GetCurrentProcessId());
#else
    return long(::getpid());
#endif
}

bool process_alive(long pid)
{
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        // Access denied means it exists (another user's process); anything else means it is gone.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code  = 0;
    bool  alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string read_file(const fs::path &path)
{
    std::ifstream in(path.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

#ifdef _WIN32
std::string narrow_utf8(const wchar_t *w)
{
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string out(size_t(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    out.resize(size_t(n) - 1);
    return out;
}

// Windows' own record of how an earlier copy of this app ended, from the Application event log:
// "Application Error" (1000, written by Windows Error Reporting for any crash, including the
// fast-fail and stack-overflow kinds that never reach our crash filter) or "Application Hang"
// (1002, the person closed it while it was not responding). Looked up only for a session marker
// that has no exception of its own. Reads the newest few hundred entries of the last 14 days.
bool windows_crash_record(long pid, int64_t started_ms, std::string &exception, std::string &at)
{
    wchar_t exe[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::string exe_name = fs::path(exe).filename().string();

    const wchar_t *query = L"*[System[Provider[@Name='Application Error' or @Name='Application Hang'] and "
                           L"(EventID=1000 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= 1209600000]]]";
    EVT_HANDLE results = ::EvtQuery(nullptr, L"Application", query, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results)
        return false;
    bool              found   = false;
    int               scanned = 0;
    EVT_HANDLE        events[16];
    DWORD             returned = 0;
    std::vector<wchar_t> buffer;
    while (!found && scanned < 400 && ::EvtNext(results, 16, events, 1000, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            if (!found) {
                DWORD used = 0, count = 0;
                if (!::EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &used, &count) &&
                    ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                    buffer.assign(used / sizeof(wchar_t) + 1, L'\0');
                    if (::EvtRender(nullptr, events[i], EvtRenderEventXml, DWORD(buffer.size() * sizeof(wchar_t)),
                                    buffer.data(), &used, &count))
                        found = match_crash_event(narrow_utf8(buffer.data()), pid, started_ms, exe_name, exception, at);
                }
            }
            ::EvtClose(events[i]);
        }
        scanned += int(returned);
    }
    ::EvtClose(results);
    return found;
}
#endif

void write_file(const fs::path &path, const std::string &content)
{
    boost::system::error_code ec;
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        out << content;
        if (!out)
            return;
    }
    fs::rename(tmp, path, ec);
}

// What the crash handlers write, prepared ahead because they run in a dying process: plain char
// buffers, no allocation, no locks. Kept current by write_marker_locked().
struct CrashMarker
{
    char path[1024]   = {0}; // UTF-8 path of this session's marker
    char version[32]  = {0};
    char phase[32]    = {0};
    long long started = 0;
    long long alive   = 0;
    bool consented    = false; // see write_marker_locked()
};
CrashMarker g_crash_marker;

void copy_cstr(char *dst, size_t size, const std::string &src)
{
    std::snprintf(dst, size, "%s", src.c_str());
}

// This session's crash marker; see start(). Caller holds the mutex.
void write_marker_locked(State &s)
{
    const int64_t alive = now_ms();
    // "consented": diagnostics were permitted while this session ran. Only such a session's crash is
    // ever reported; one that ran before the person said yes stays theirs.
    json marker = {{"pid", current_pid()}, {"version", s.version}, {"started", s.started_ms},
                   {"alive", alive},       {"phase", s.phase},     {"consented", s.on.load()}};
    // A session that dies while frozen (typically force-quit) is a hang, not a crash. A crash
    // handler that runs later rewrites the marker with the signal or exception instead.
    if (s.hung) {
        marker["exception"] = "hang";
        if (!s.hang_at.empty())
            marker["at"] = s.hang_at;
    }
    write_file(marker_path(s.session), marker.dump());
    s.alive_written_ms = steady_ms();
    copy_cstr(g_crash_marker.path, sizeof(g_crash_marker.path), marker_path(s.session).string());
    copy_cstr(g_crash_marker.version, sizeof(g_crash_marker.version), s.version);
    copy_cstr(g_crash_marker.phase, sizeof(g_crash_marker.phase), s.phase);
    g_crash_marker.started = s.started_ms;
    g_crash_marker.alive   = alive;
    g_crash_marker.consented = s.on.load();
}

void hang_location(char *out, size_t size);
void sanitize_token(char *s);

// Marks the marker "hang" while the main thread has not called heartbeat() for HANG_AFTER_S, and
// refreshes "alive" every minute. Detached on purpose: see state().
void watchdog_loop()
{
    State &s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    while (s.started) {
        s.watchdog_wake.wait_for(lock, std::chrono::seconds(5));
        if (!s.started)
            break;
        const int64_t now = steady_ms(), beat = s.last_beat_ms.load();
        const bool    hung = beat > 0 && now - beat > s.hang_after_ms;
        if (hung != s.hung.load()) {
            if (hung)
                BOOST_LOG_TRIVIAL(warning) << "usage diagnostics: the main thread has not responded for "
                                           << (now - beat) / 1000 << " s";
            s.hung = hung;
            s.hang_at.clear();
            write_marker_locked(s);
            if (hung) {
                // Then where the main thread is stuck (Windows). Not under the mutex: if the walk itself
                // blocks (the main thread holding the loader lock), the marker already says "hang", and
                // a heartbeat that comes back can still take the mutex.
                char at[200];
                lock.unlock();
                hang_location(at, sizeof(at));
                sanitize_token(at);
                lock.lock();
                if (s.started && s.hung && at[0]) {
                    s.hang_at = at;
                    write_marker_locked(s);
                }
            }
        } else if (now - s.alive_written_ms >= ALIVE_EVERY_MS)
            write_marker_locked(s);
    }
    s.watchdog_running = false;
}

// Keep only characters that are safe inside the JSON string and say nothing about the person.
void sanitize_token(char *s)
{
    for (; *s; ++s)
        if (!(std::isalnum(static_cast<unsigned char>(*s)) || *s == '.' || *s == '_' || *s == '-' || *s == '+' || *s == '>'))
            *s = '_';
}

// Rewrites this session's marker with what ended it. Runs inside a crash handler.
void write_crash_marker(const char *exception, const char *at)
{
    if (g_crash_marker.path[0] == 0)
        return;
    char exc[48] = {0}, where[200] = {0};
    std::snprintf(exc, sizeof(exc), "%s", exception ? exception : "");
    std::snprintf(where, sizeof(where), "%s", at ? at : "");
    sanitize_token(exc);
    sanitize_token(where);
    char body[768];
    const int n = std::snprintf(body, sizeof(body),
                                "{\"pid\":%ld,\"version\":\"%s\",\"started\":%lld,\"alive\":%lld,\"phase\":\"%s\","
                                "\"exception\":\"%s\",\"at\":\"%s\",\"consented\":%s}",
                                current_pid(), g_crash_marker.version, g_crash_marker.started, g_crash_marker.alive,
                                g_crash_marker.phase, exc, where, g_crash_marker.consented ? "true" : "false");
    if (n <= 0)
        return;
    const size_t len = std::min<size_t>(size_t(n), sizeof(body) - 1);
#ifdef _WIN32
    if (FILE *f = std::fopen(g_crash_marker.path, "wb")) {
        std::fwrite(body, 1, len, f);
        std::fclose(f);
    }
#else
    // open/write, not stdio: this runs in a signal handler.
    const int fd = ::open(g_crash_marker.path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const ssize_t written = ::write(fd, body, len);
        (void) written;
        ::close(fd);
    }
#endif
}

// Once per process: the first crash handler to run writes the marker, later ones (an abort()
// raised by a terminate handler, a second fault) leave it alone.
std::atomic<bool> g_crash_written{false};

#ifdef _WIN32
// Module file name and offset for an address, into `out`. No allocation.
void code_location_raw(const void *address, char *out, size_t size)
{
    out[0]   = 0;
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        static_cast<LPCWSTR>(address), &module) || !module) {
        std::snprintf(out, size, "unknown+0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
        return;
    }
    wchar_t wpath[MAX_PATH] = {0};
    const DWORD len = GetModuleFileNameW(module, wpath, MAX_PATH);
    const wchar_t *name = wpath;
    for (DWORD i = 0; i < len; ++i)
        if (wpath[i] == L'\\' || wpath[i] == L'/')
            name = wpath + i + 1;
    char cname[128] = {0};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, cname, int(sizeof(cname)) - 1, nullptr, nullptr);
    const unsigned long long offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
    std::snprintf(out, size, "%s+0x%llx", cname[0] ? cname : "unknown", offset);
}

#if defined(_M_X64)
// One frame up from `ctx`, from the unwind tables. False when there is nothing more to read.
bool unwind_one(CONTEXT &ctx)
{
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (!entry) {
        // A leaf function (strlen, a tiny accessor, a system call stub) has no unwind data and has
        // not touched the stack pointer: its return address is at the top of the stack.
        __try {
            ctx.Rip = *reinterpret_cast<const DWORD64 *>(ctx.Rsp);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        ctx.Rsp += sizeof(DWORD64);
    } else {
        PVOID   handler_data = nullptr;
        DWORD64 establisher  = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, entry, &ctx, &handler_data, &establisher, nullptr);
    }
    return ctx.Rip != 0;
}

// Appends ">module+0xoffset" for the callers of the faulting function (up to 5), so a crash inside a
// library call (imgui, wx, the GL driver) names who made it. Walks the unwind tables from the
// exception's own CONTEXT: no allocation, nothing is read through a guessed pointer.
void append_caller_chain(const CONTEXT *record, char *out, size_t size)
{
    if (!record)
        return;
    CONTEXT ctx = *record;
    for (int frame = 0; frame < 5; ++frame) {
        if (!unwind_one(ctx))
            return;
        char one[80];
        code_location_raw(reinterpret_cast<const void *>(ctx.Rip), one, sizeof(one));
        const size_t used = std::strlen(out);
        if (used + std::strlen(one) + 2 >= size)
            return;
        std::snprintf(out + used, size - used, ">%s", one);
    }
}

// The thread heartbeat() runs on (the main thread), for where a hang is; set by its first beat.
std::atomic<HANDLE> g_beat_thread{nullptr};
wchar_t             g_windows_dir[MAX_PATH] = {0};

// Code in a module under the Windows folder: ntdll, user32, a GPU driver from the DriverStore.
bool in_windows_module(DWORD64 address)
{
    HMODULE module = nullptr;
    if (g_windows_dir[0] == 0 ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) || !module)
        return false;
    wchar_t path[MAX_PATH] = {0};
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return false;
    return _wcsnicmp(path, g_windows_dir, wcslen(g_windows_dir)) == 0;
}

// The main thread's top frame, then up to 5 callers from the first frame outside Windows' own
// modules (a hung thread's top frames are nearly always a wait inside ntdll or user32), with ">.."
// where frames were skipped. Only reads: no allocation, faults while walking are caught.
void walk_hang_stack(const CONTEXT &start, char *out, size_t size)
{
    CONTEXT ctx = start;
    __try {
        code_location_raw(reinterpret_cast<const void *>(ctx.Rip), out, size);
        bool own = !in_windows_module(ctx.Rip), skipped = false;
        for (int frame = 0, emitted = 0; frame < 64 && emitted < 5 && unwind_one(ctx); ++frame) {
            if (!own) {
                if (in_windows_module(ctx.Rip)) {
                    skipped = true;
                    continue;
                }
                own = true;
            }
            char one[80];
            code_location_raw(reinterpret_cast<const void *>(ctx.Rip), one, sizeof(one));
            const char  *sep  = skipped && emitted == 0 ? ">..>" : ">";
            const size_t used = std::strlen(out);
            if (used + std::strlen(sep) + std::strlen(one) + 1 >= size)
                break;
            std::snprintf(out + used, size - used, "%s%s", sep, one);
            ++emitted;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Where the main thread is right now. It is suspended only for GetThreadContext (nothing that could
// wait on a lock it holds runs meanwhile), then walked running: a hung thread's stack stays put.
void hang_location(char *out, size_t size)
{
    out[0]        = 0;
    HANDLE thread = g_beat_thread.load();
    if (!thread || ::GetThreadId(thread) == ::GetCurrentThreadId())
        return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (::SuspendThread(thread) == DWORD(-1))
        return;
    const BOOL got = ::GetThreadContext(thread, &ctx);
    ::ResumeThread(thread);
    if (got)
        walk_hang_stack(ctx, out, size);
}

void remember_beat_thread()
{
    if (g_beat_thread.load())
        return;
    if (g_windows_dir[0] == 0) {
        const UINT n = ::GetWindowsDirectoryW(g_windows_dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 1)
            g_windows_dir[0] = 0;
        else if (g_windows_dir[n - 1] != L'\\') {
            g_windows_dir[n]     = L'\\';
            g_windows_dir[n + 1] = 0;
        }
    }
    HANDLE thread = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &thread,
                          THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
        g_beat_thread = thread; // kept for the life of the process
}
#else
void append_caller_chain(const CONTEXT *, char *, size_t) {}
#endif

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

LONG WINAPI crash_filter(EXCEPTION_POINTERS *info)
{
    if (info && info->ExceptionRecord && !g_crash_written.exchange(true)) {
        char exc[16], at[200];
        std::snprintf(exc, sizeof(exc), "0x%08lX", static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode));
        code_location_raw(info->ExceptionRecord->ExceptionAddress, at, sizeof(at));
        append_caller_chain(info->ContextRecord, at, sizeof(at));
        write_crash_marker(exc, at);
    }
    return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

#if !defined(_WIN32) || !defined(_M_X64)
void hang_location(char *out, size_t) { out[0] = 0; }
void remember_beat_thread() {}
#endif

#ifndef _WIN32
// macOS and Linux: a fatal signal writes its name and, on macOS, where it happened into the
// marker, then goes on exactly as it would have (the previous handler, or the default action), so
// the process still dies and macOS still writes its own crash report.
constexpr int CRASH_SIGNALS[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
struct sigaction g_previous_actions[NSIG];
// The handler's own stack, so a stack overflow on the main thread is still recorded.
char g_alt_stack[64 * 1024];

const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    default: return "signal";
    }
}

#ifdef __APPLE__
// The executable's __TEXT segment. All of the slicer's own code is in the one executable (only
// Python is a separate library), so an offset into it is a complete location.
uintptr_t g_main_begin = 0, g_main_end = 0;

void find_main_image()
{
    const auto *header = reinterpret_cast<const mach_header_64 *>(_dyld_get_image_header(0));
    if (!header || header->magic != MH_MAGIC_64)
        return;
    const auto *cmd = reinterpret_cast<const load_command *>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (cmd->cmd == LC_SEGMENT_64) {
            const auto *seg = reinterpret_cast<const segment_command_64 *>(cmd);
            if (std::strncmp(seg->segname, "__TEXT", sizeof(seg->segname)) == 0) {
                g_main_begin = reinterpret_cast<uintptr_t>(header);
                g_main_end   = g_main_begin + uintptr_t(seg->vmsize);
                return;
            }
        }
        cmd = reinterpret_cast<const load_command *>(reinterpret_cast<const char *>(cmd) + cmd->cmdsize);
    }
}

bool in_main_image(uintptr_t address) { return address >= g_main_begin && address < g_main_end; }

// No snprintf: appends "0x<hex>" to [p, end).
void append_hex(char *&p, char *end, uintptr_t value)
{
    char digits[2 * sizeof(uintptr_t)];
    int  n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value & 0xf];
        value >>= 4;
    } while (value && n < int(sizeof(digits)));
    for (const char *c = "0x"; *c && p < end; ++c)
        *p++ = *c;
    while (n > 0 && p < end)
        *p++ = digits[--n];
}

void append_str(char *&p, char *end, const char *s)
{
    while (*s && p < end)
        *p++ = *s++;
}

// "UnlayeredSlicer.arm64+0x2f1a3c>+0x2e1b00>+0x1c2d44": the faulting instruction, then the return
// addresses of up to five of the slicer's own frames below it, as offsets into the executable (the
// same ">" chain the Windows filter writes). The faulting instruction comes from the signal's saved
// context: backtrace() from inside the handler would start at the handler and skip it.
// "ext.<arch>" first when the fault itself is outside it (a system library, Python): abort() is one.
// Symbolicate an offset with
//   atos -arch arm64 -o UnlayeredSlicer.app/Contents/MacOS/UnlayeredSlicer -l 0x100000000 <0x100000000 + offset>
// using the very build that crashed (the version and arch are in the event).
void crash_location(const ucontext_t *uc, char *out, size_t size)
{
    out[0] = 0;
    if (!uc || !uc->uc_mcontext || g_main_begin == 0)
        return;
    // Pointer-authentication bits off (return addresses saved by system code are signed).
    constexpr uintptr_t ADDRESS_MASK = 0x00007fffffffffffULL;
    uintptr_t pc = 0, lr = 0, fp = 0;
#if defined(__arm64__)
    pc = uintptr_t(arm_thread_state64_get_pc(uc->uc_mcontext->__ss)) & ADDRESS_MASK;
    lr = uintptr_t(arm_thread_state64_get_lr(uc->uc_mcontext->__ss)) & ADDRESS_MASK;
    fp = uintptr_t(arm_thread_state64_get_fp(uc->uc_mcontext->__ss));
#elif defined(__x86_64__)
    pc = uintptr_t(uc->uc_mcontext->__ss.__rip);
    fp = uintptr_t(uc->uc_mcontext->__ss.__rbp);
#endif
    char *p = out, *end = out + size - 1;
    if (in_main_image(pc)) {
#if defined(__arm64__)
        append_str(p, end, "UnlayeredSlicer.arm64+");
#else
        append_str(p, end, "UnlayeredSlicer.x86_64+");
#endif
        append_hex(p, end, pc - g_main_begin);
    } else {
        // Which slice of the universal binary the offsets that follow belong to: without it the
        // offsets of an "ext" crash cannot be symbolized.
#if defined(__arm64__)
        append_str(p, end, "ext.arm64");
#else
        append_str(p, end, "ext.x86_64");
#endif
    }

    int       frames = 0;
    uintptr_t last   = pc;
    auto      add    = [&](uintptr_t ret) {
        if (frames < 5 && in_main_image(ret) && ret != last) {
            append_str(p, end, ">+");
            append_hex(p, end, ret - g_main_begin);
            ++frames;
            last = ret;
        }
    };
    add(lr); // a leaf function may not have saved it on the stack yet
    // Walk the frame-pointer chain within this thread's stack (macOS keeps frame pointers).
    pthread_t       self   = pthread_self();
    const uintptr_t top    = uintptr_t(pthread_get_stackaddr_np(self));
    const uintptr_t bottom = top - uintptr_t(pthread_get_stacksize_np(self));
    for (int i = 0; i < 64 && frames < 5; ++i) {
        if (fp < bottom || fp + 2 * sizeof(uintptr_t) > top || (fp & (sizeof(uintptr_t) - 1)) != 0)
            break;
        const uintptr_t next = reinterpret_cast<const uintptr_t *>(fp)[0];
        add(reinterpret_cast<const uintptr_t *>(fp)[1] & ADDRESS_MASK);
        if (next <= fp)
            break;
        fp = next;
    }
    *p = 0;
}
#endif // __APPLE__

void crash_signal(int sig, siginfo_t *info, void *context)
{
    if (!g_crash_written.exchange(true)) {
        char at[200] = {0};
#ifdef __APPLE__
        crash_location(static_cast<const ucontext_t *>(context), at, sizeof(at));
#endif
        write_crash_marker(signal_name(sig), at);
    }
    // Hand it on as if we had never been here.
    const struct sigaction &previous = g_previous_actions[sig];
    sigaction(sig, &previous, nullptr);
    if ((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction) {
        previous.sa_sigaction(sig, info, context);
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
        previous.sa_handler(sig);
        return;
    }
    // Default action. A fault returns to the faulting instruction and dies on it, so macOS reports
    // the original fault. A signal that was sent (abort(), kill) would let the process carry on,
    // so it is raised again. macOS gives a sent SIGSEGV the same si_code as a fault and no si_pid;
    // only si_addr tells them apart (null when sent). A fault at address 0 is raised too, and
    // dies just the same.
    if (sig == SIGABRT || !info || info->si_code <= 0 || info->si_code == SI_USER || info->si_addr == nullptr)
        raise(sig);
}
#endif // !_WIN32

std::terminate_handler g_previous_terminate = nullptr;

void on_terminate()
{
    if (!g_crash_written.exchange(true))
        write_crash_marker("terminate", "");
    if (g_previous_terminate)
        g_previous_terminate();
    std::abort();
}

bool g_handlers_installed = false;

void install_crash_handlers()
{
    if (g_handlers_installed)
        return;
    g_handlers_installed = true;
#ifdef _WIN32
    g_previous_filter = SetUnhandledExceptionFilter(crash_filter);
#else
#ifdef __APPLE__
    find_main_image();
#endif
    stack_t alt{};
    alt.ss_sp    = g_alt_stack;
    alt.ss_size  = sizeof(g_alt_stack);
    alt.ss_flags = 0;
    sigaltstack(&alt, nullptr); // this (the main) thread's; other threads' overflows go unrecorded
    struct sigaction action{};
    action.sa_sigaction = crash_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig : CRASH_SIGNALS) {
        sigaction(sig, &action, &g_previous_actions[sig]);
        // Someone else's handler runs after ours (chained). Say whose, once: it decides what
        // happens to the process after the marker is written.
        const struct sigaction &previous = g_previous_actions[sig];
        const void *fn = (previous.sa_flags & SA_SIGINFO) ? reinterpret_cast<const void *>(previous.sa_sigaction)
                                                          : reinterpret_cast<const void *>(previous.sa_handler);
        if (fn != reinterpret_cast<const void *>(SIG_DFL) && fn != reinterpret_cast<const void *>(SIG_IGN)) {
            Dl_info dl{};
            const char *module = dladdr(fn, &dl) && dl.dli_fname ? std::strrchr(dl.dli_fname, '/') : nullptr;
            BOOST_LOG_TRIVIAL(info) << "usage diagnostics: " << signal_name(sig) << " already had a handler, in "
                                    << (module ? module + 1 : "an unknown module") << " ("
                                    << (dl.dli_sname ? dl.dli_sname : "?") << "); it runs after ours";
        }
    }
#endif
    g_previous_terminate = std::set_terminate(on_terminate);
}

// After a clean shutdown there is no marker to write into, so hand the process back.
void uninstall_crash_handlers()
{
    if (!g_handlers_installed)
        return;
    g_handlers_installed = false;
#ifdef _WIN32
    if (SetUnhandledExceptionFilter(g_previous_filter) != crash_filter)
        SetUnhandledExceptionFilter(crash_filter); // someone installed theirs after ours: leave it
#else
    for (int sig : CRASH_SIGNALS) {
        struct sigaction current{};
        if (sigaction(sig, nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) &&
            current.sa_sigaction == crash_signal)
            sigaction(sig, &g_previous_actions[sig], nullptr); // someone installed theirs after ours: leave it
    }
#endif
    if (std::set_terminate(g_previous_terminate) != on_terminate)
        std::set_terminate(on_terminate);
    g_crash_marker.path[0] = 0;
}

json make_event(const std::string &name, json props)
{
    if (!props.is_object())
        props = json::object();
    return json{{"name", name}, {"t", now_ms()}, {"props", std::move(props)}};
}

std::string batch_body(const State &s, std::vector<json> events)
{
    json body = {{"v", 1},
                 {"install", s.install},
                 {"session", s.session},
                 {"app", {{"version", s.version}, {"channel", s.channel}, {"os", s.os}}},
                 {"program", s.program},
                 {"events", std::move(events)}};
    return body.dump();
}

// Moves up to one batch's worth of queued events into a body. Caller holds the mutex.
std::string take_batch_locked(State &s)
{
    // As many events as fit, and always at least one.
    size_t n = 0, bytes = 0;
    while (n < s.queue.size() && n < MAX_EVENTS_PER_BATCH) {
        const size_t size = s.queue[n].dump().size();
        if (n > 0 && bytes + size > MAX_BATCH_EVENT_BYTES)
            break;
        bytes += size;
        ++n;
    }
    std::vector<json> events(std::make_move_iterator(s.queue.begin()), std::make_move_iterator(s.queue.begin() + n));
    s.queue.erase(s.queue.begin(), s.queue.begin() + n);
    return batch_body(s, std::move(events));
}

void save_pending_locked(State &s)
{
    boost::system::error_code ec;
    if (s.pending.size() > MAX_PENDING_BATCHES)
        s.pending.erase(s.pending.begin(), s.pending.end() - MAX_PENDING_BATCHES);
    if (s.pending.empty()) {
        fs::remove(pending_path(), ec);
        return;
    }
    write_file(pending_path(), json(s.pending).dump());
}

void finish(int id, bool keep)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.in_flight.find(id);
    if (it == s.in_flight.end())
        return;
    if (keep && s.on) {
        s.pending.push_back(std::move(it->second));
        save_pending_locked(s);
    }
    s.in_flight.erase(it);
}

// Returns whether it was delivered (meaningful for a synchronous send only).
bool send(const std::string &body, bool sync)
{
    State &s = state();
    int id;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        // Asked again for every request, not once at start-up: a withdrawal between queueing and
        // sending stops the send.
        if (!s.on)
            return false;
        id = s.next_id++;
        s.in_flight[id] = body;
        url = s.url;
    }
    auto delivered = std::make_shared<std::atomic<bool>>(false);
    auto http = Http::post(url);
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .timeout_connect(sync ? 3 : 10)
        .timeout_max(sync ? 5 : 30)
        .tls_verify(true)
        .on_complete([id, delivered](std::string, unsigned) {
            delivered->store(true);
            finish(id, false);
        })
        .on_error([id](std::string, std::string error, unsigned status) {
            // Offline, rate-limited or the site down: keep it for later. Any other 4xx means the
            // site will never take this batch, so retrying would only repeat the refusal.
            bool keep = status == 0 || status == 429 || status >= 500;
            BOOST_LOG_TRIVIAL(info) << "usage diagnostics: send failed, status " << status << ", " << error
                                    << (keep ? " (kept for later)" : " (dropped)");
            finish(id, keep);
        });
    if (sync)
        http.perform_sync();
    else
        http.perform();
    return delivered->load();
}

} // namespace

namespace {
// Event data fields as (Name, value), in order. Older Windows writes them without names.
std::vector<std::pair<std::string, std::string>> event_data_fields(const std::string &xml)
{
    std::vector<std::pair<std::string, std::string>> fields;
    static const std::regex data_re(R"(<Data(?:\s+Name=['"]([^'"]*)['"])?\s*>([^<]*)</Data>)");
    for (std::sregex_iterator it(xml.begin(), xml.end(), data_re), end; it != end; ++it)
        fields.emplace_back((*it)[1].str(), (*it)[2].str());
    return fields;
}

// A field by name, or by position when the event carries no names.
std::string event_field(const std::vector<std::pair<std::string, std::string>> &fields, const char *name, size_t position)
{
    bool named = false;
    for (const auto &f : fields) {
        if (!f.first.empty())
            named = true;
        if (boost::iequals(f.first, name))
            return f.second;
    }
    return !named && position < fields.size() ? fields[position].second : std::string();
}

// "0x1a2c", "1a2c" or "00001a2c" -> 0x1a2c; false when it is not hex.
bool parse_hex(std::string s, unsigned long long &out)
{
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    if (s.empty() || s.size() > 16 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return false;
    out = std::stoull(s, nullptr, 16);
    return true;
}
} // namespace

bool match_crash_event(const std::string &event_xml, long pid, int64_t started_ms, const std::string &exe_name,
                       std::string &exception, std::string &at)
{
    std::smatch m;
    static const std::regex id_re(R"(<EventID[^>]*>\s*(\d+)\s*</EventID>)");
    if (pid <= 0 || !std::regex_search(event_xml, m, id_re))
        return false;
    const int  id    = std::stoi(m[1].str());
    const bool crash = id == 1000;
    if (!crash && id != 1002)
        return false;
    const auto fields = event_data_fields(event_xml);
    if (!boost::iequals(event_field(fields, "AppName", 0), exe_name))
        return false;

    // The same process: its id, and (when the marker knows it) the same start time, so a later
    // process that happened to reuse the id is never taken for it.
    unsigned long long value = 0;
    if (!parse_hex(event_field(fields, crash ? "ProcessId" : "ProcessID", crash ? 8 : 2), value) ||
        value != static_cast<unsigned long long>(pid))
        return false;
    if (started_ms > 0) {
        unsigned long long filetime = 0;
        if (!parse_hex(event_field(fields, crash ? "ProcessCreationTime" : "StartTime", crash ? 9 : 3), filetime))
            return false;
        // FILETIME: 100 ns ticks since 1601-01-01.
        const int64_t start_ms = int64_t(filetime / 10000ULL) - 11644473600000LL;
        if (std::llabs(start_ms - started_ms) > 10000)
            return false;
    }

    if (!crash) {
        exception = "hang_closed";
        at.clear();
        return true;
    }
    unsigned long long code = 0, offset = 0;
    if (!parse_hex(event_field(fields, "ExceptionCode", 6), code))
        return false;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%08llX", code);
    exception = buf;
    const std::string module = event_field(fields, "ModuleName", 3);
    if (!module.empty() && parse_hex(event_field(fields, "FaultingOffset", 7), offset)) {
        std::snprintf(buf, sizeof(buf), "+0x%llx", offset);
        at = module + buf;
    } else
        at = module;
    return true;
}

// The random per-install id exists only once diagnostics have been permitted (policy 2026-10-v2,
// section 7): it is created the first time they are, and then kept in the app config, also after a
// withdrawal, so the person can still quote it in a privacy request. Never created while they are off.
static void load_install_id_locked(State &s, AppConfig &config)
{
    s.install = config.get("usage_diagnostics_id");
    if (s.on && !is_uuid(s.install)) {
        s.install = new_uuid();
        config.set("usage_diagnostics_id", s.install);
    }
}

void start(AppConfig &config, bool first_run)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.started)
        return;
    // Whatever an earlier start()/shutdown() pair left is on disk now, and is read back below.
    s.queue.clear();
    s.counts.clear();
    s.pending.clear();

    const char *env = std::getenv("UNLAYERED_NO_TELEMETRY");
    s.env_off    = env && *env;
    s.first_run  = first_run;
    s.version    = UNLAYERED_VERSION;
    s.channel    = channel_for(s.version, is_internal_copy());
    s.os         = os_name();
    s.started_at = std::chrono::steady_clock::now();
    s.session    = new_uuid();

    std::string url = config.get("usage_diagnostics_url");
    s.url = url.empty() ? DEFAULT_URL : url;
    // Undocumented too: "usage_diagnostics_hang_s", a shorter hang threshold for testing the watchdog.
    s.hang_after_ms = HANG_AFTER_MS;
    try {
        const std::string hang_s = config.get("usage_diagnostics_hang_s");
        if (!hang_s.empty() && std::stoi(hang_s) > 0)
            s.hang_after_ms = int64_t(std::stoi(hang_s)) * 1000;
    } catch (...) {}

    // A config from before 0.1.6 keeps an explicit "off" as a declined choice; the old default "on"
    // is not a choice, so those installations are asked (GUI_App) and send nothing until then.
    PrivacyConsent::migrate_legacy_setting(config);
    PrivacyConsent::queue_earlier_choice_report(config);
    s.on      = PrivacyConsent::diagnostics_permitted(PrivacyConsent::load(config), s.env_off, PrivacyConsent::legal_approvals());
    s.program = s.on && PrivacyConsent::program_permitted(PrivacyConsent::load(config), s.env_off, PrivacyConsent::legal_approvals());
    load_install_id_locked(s, config);

    boost::system::error_code ec;
    fs::create_directories(usage_dir(), ec);

    // A marker left by a process that no longer runs is a session that never reached shutdown().
    // A marker whose process still runs is a second copy of the app, open right now.
    std::vector<fs::path> markers; // collected first: they are deleted below
    for (fs::directory_iterator it(usage_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string file = it->path().filename().string();
        if (file.rfind("session-", 0) == 0 && it->path().extension() == ".json")
            markers.push_back(it->path());
    }
    for (const fs::path &marker_file : markers) {
        json marker = json::parse(read_file(marker_file), nullptr, false);
        long pid    = marker.is_object() ? marker.value("pid", 0L) : 0L;
        if (pid != 0 && pid != current_pid() && process_alive(pid))
            continue;
        // The operating system ended that session (end_session_by_os): not a crash.
        const bool os_ended  = marker.is_object() && marker.value("phase", std::string()) == "os_session_end";
        const bool consented = marker.is_object() && marker.value("consented", false);
        // Covered: diagnostics were shared in that session and are shared now - reported as before.
        // Otherwise the person is asked once whether to send this one report (unreported_crashes).
        const bool covered = s.on && consented;
        const bool ask     = !covered && !s.env_off && PrivacyConsent::legal_approvals().crash_report_prompt;
        if (!os_ended && (covered || ask)) {
#ifdef _WIN32
            // Our crash filter never ran (a fast fail, a stack overflow, heap corruption, or the
            // person ended it while it hung): ask Windows how that process ended.
            if (marker.value("exception", std::string()).empty() && pid > 0) {
                std::string exception, where;
                if (windows_crash_record(pid, marker.value("started", int64_t(0)), exception, where)) {
                    marker["exception"]   = exception;
                    marker["at"]          = where;
                    marker["recorded_by"] = "windows";
                }
            }
#endif
            json props = {{"version", marker.value("version", std::string())}};
            // Where it was and what ended it (older markers have neither).
            for (const char *key : {"phase", "exception", "at", "recorded_by"})
                if (marker.contains(key) && marker[key].is_string() && !marker[key].get<std::string>().empty()) {
                    std::string value = marker[key].get<std::string>();
                    if (value.size() > 190) {
                        // A caller chain is cut at a frame boundary, never inside "module+0x...".
                        const size_t frame = value.rfind('>', 190);
                        value.resize(frame != std::string::npos && frame > 0 ? frame : 190);
                    }
                    props[key] = std::move(value);
                }
            // "alive" is refreshed by every flush, so this is the uptime to within a few minutes.
            int64_t started = marker.value("started", int64_t(0)), alive = marker.value("alive", started);
            if (started > 0 && alive >= started)
                props["uptime_s"] = (alive - started) / 1000;
            json clean, no_counts;
            if (DiagnosticsAllowlist::filter("crash", props, json(), clean, no_counts)) {
                if (covered)
                    s.queue.push_back(make_event("crash", std::move(clean)));
                else
                    s.crash_offers.push_back(std::move(clean));
            }
        }
        boost::system::error_code rm_ec;
        fs::remove(marker_file, rm_ec);
    }
    s.started_ms = now_ms();
    s.phase      = "startup";
    write_marker_locked(s);
    // Recorded locally for everyone the organisation has not opted out: the marker says whether
    // diagnostics were shared, and a crash outside them is only ever sent if the person says so.
    if (!s.env_off)
        install_crash_handlers();
    // Keep the newest few; older ones say nothing the newest does not.
    if (s.crash_offers.size() > 3)
        s.crash_offers.erase(s.crash_offers.begin(), s.crash_offers.end() - 3);

    if (s.on) {
        json saved = json::parse(read_file(pending_path()), nullptr, false);
        if (saved.is_array())
            for (const auto &b : saved)
                if (b.is_string())
                    s.pending.push_back(b.get<std::string>());
    } else
        fs::remove(pending_path(), ec);

    s.started = true;
    s.hung    = false;
    s.last_beat_ms = 0; // no hang until the app has started beating (heartbeat())
    if (!s.watchdog_running) {
        s.watchdog_running = true;
        std::thread(watchdog_loop).detach();
    }
    BOOST_LOG_TRIVIAL(info) << "usage diagnostics: " << (s.on ? "on" : (s.env_off ? "off (UNLAYERED_NO_TELEMETRY)" : "off"))
                            << ", channel " << s.channel;
}

void heartbeat()
{
    State &s = state();
    remember_beat_thread();
    s.last_beat_ms = steady_ms();
    if (s.hung.load()) {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.started && s.hung.load()) {
            BOOST_LOG_TRIVIAL(info) << "usage diagnostics: the main thread is responding again";
            s.hung = false;
            write_marker_locked(s);
        }
    }
}

bool enabled() { return state().on; }

void set_phase(const char *phase)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started || !phase || s.phase == phase)
        return;
    s.phase = phase;
    write_marker_locked(s);
}

void end_session_by_os()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started)
        return;
    s.phase = "os_session_end";
    write_marker_locked(s);
}

std::string code_location(const void *address)
{
#ifdef _WIN32
    char out[160];
    code_location_raw(address, out, sizeof(out));
    return out;
#else
    (void) address;
    return {};
#endif
}

bool disabled_by_environment() { return state().env_off; }

std::vector<json> unreported_crashes()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.crash_offers;
}

void answer_unreported_crashes(bool send_them)
{
    State &s = state();
    std::vector<json> crashes;
    std::string       url, body;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        crashes.swap(s.crash_offers);
        if (!send_them || crashes.empty())
            return;
        std::vector<json> events;
        for (json &props : crashes)
            events.push_back(make_event("crash", std::move(props)));
        // Its own batch, with ids made for it alone: never this installation's id, so the report
        // cannot be tied to anything else from this computer.
        json batch = {{"v", 1},
                      {"install", new_uuid()},
                      {"session", new_uuid()},
                      {"app", {{"version", s.version}, {"channel", s.channel}, {"os", s.os}}},
                      {"program", false},
                      {"events", std::move(events)}};
        body = batch.dump();
        url  = s.url;
    }
    // One attempt, in the background; not kept for a retry (the person agreed to this send only).
    auto http = Http::post(url);
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .timeout_connect(10)
        .timeout_max(30)
        .tls_verify(true)
        .on_complete([](std::string, unsigned) { BOOST_LOG_TRIVIAL(info) << "crash report: sent at the person's request"; })
        .on_error([](std::string, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << "crash report: not delivered, status " << status << ", " << error;
        })
        .perform();
}

bool is_first_run() { return state().first_run; }

std::string install_id()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.started && s.on ? s.install : std::string();
}

bool is_internal_copy()
{
    const char *internal = std::getenv("UNLAYERED_INTERNAL");
    if (internal && *internal)
        return true;
    return ! runs_installed();
}

std::string channel_for(const std::string &version, bool internal)
{
    return internal ? std::string("dev") : channel_of(version);
}

void refresh(AppConfig &config)
{
    State &s = state();
    const bool on = PrivacyConsent::diagnostics_permitted(PrivacyConsent::load(config), PrivacyConsent::organization_opt_out(),
                                                          PrivacyConsent::legal_approvals());
    const bool program = on && PrivacyConsent::program_permitted(PrivacyConsent::load(config), PrivacyConsent::organization_opt_out(),
                                                                 PrivacyConsent::legal_approvals());
    bool install_handlers = false, remove_handlers = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const bool was = s.on.load();
        s.on           = on && !s.env_off;
        remove_handlers = was && !s.on && s.started;
        if (s.on && s.program != program) {
            // Joined or left the improvement program: what was collected under the other answer is
            // stamped with it, so none of it may leave now (a failed send of it is not kept either).
            s.queue.clear();
            s.counts.clear();
            s.pending.clear();
            s.in_flight.clear();
            save_pending_locked(s);
        }
        s.program = s.on && program;
        if (!s.on) {
            // Withdrawn (or never given): nothing collected so far may leave. Requests already
            // under way cannot be recalled; finish() no longer keeps their bodies for a retry.
            s.queue.clear();
            s.counts.clear();
            s.pending.clear();
            save_pending_locked(s);
        } else if (!was && s.started) {
            load_install_id_locked(s, config);
            install_handlers = true;
            s.started_at     = std::chrono::steady_clock::now(); // session_end counts from consent
        }
        if (s.started)
            write_marker_locked(s);
    }
    if (install_handlers)
        install_crash_handlers();
    else if (remove_handlers && s.env_off)
        uninstall_crash_handlers();
    // A withdrawal keeps the local recorder: the marker now says "not consented", so a later crash
    // is asked about instead of sent (write_marker_locked above).
    BOOST_LOG_TRIVIAL(info) << "usage diagnostics " << (s.on ? "permitted" : "not permitted") << " by the privacy choice";
}

void record(const std::string &name, json props, json counts)
{
    State &s = state();
    if (!s.on)
        return;
    json clean_props, clean_counts;
    if (!DiagnosticsAllowlist::filter(name, props, counts, clean_props, clean_counts))
        return;
    json event = make_event(name, std::move(clean_props));
    if (!clean_counts.empty())
        event["counts"] = std::move(clean_counts);
    bool full;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started || s.queue.size() >= MAX_QUEUED)
            return;
        s.queue.push_back(std::move(event));
        full = s.queue.size() >= FLUSH_AT_EVENTS;
    }
    if (full)
        flush();
}

void count(const std::string &feature, int by)
{
    State &s = state();
    if (!s.on || !DiagnosticsAllowlist::allowed_count_key(feature))
        return;
    std::lock_guard<std::mutex> lock(s.mutex);
    // The site takes at most 200 tallies per event; past that only known ones keep counting.
    if (s.started && (s.counts.size() < 200 || s.counts.count(feature)))
        s.counts[feature] += by;
}

void flush()
{
    State &s = state();
    if (!s.on)
        return;
    std::vector<std::string> bodies;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started)
            return;
        write_marker_locked(s);
        bodies.swap(s.pending);
        if (!bodies.empty())
            save_pending_locked(s);
        while (!s.queue.empty())
            bodies.push_back(take_batch_locked(s));
    }
    for (const std::string &body : bodies)
        send(body, false);
}

void shutdown()
{
    State &s = state();
    std::vector<std::string> bodies;
    std::string session;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started)
            return;
        s.started = false; // nothing queued after this point; also ends watchdog_loop()
        session   = s.session;
        if (s.on) {
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.started_at).count();
            json   end     = make_event("session_end", {{"duration_s", int64_t(seconds)}});
            json   counts  = json::object();
            for (const auto &[feature, n] : s.counts)
                counts[feature] = n;
            end["counts"] = std::move(counts);
            s.queue.push_back(std::move(end));
            while (!s.queue.empty())
                bodies.push_back(take_batch_locked(s));
            // This session's newest batch first: if time runs out, older ones wait for next launch.
            std::reverse(bodies.begin(), bodies.end());
            bodies.insert(bodies.end(), s.pending.begin(), s.pending.end());
            s.pending.clear();
            // Background sends that have not answered yet would be lost with the process.
            for (auto &[id, body] : s.in_flight)
                bodies.push_back(body);
            s.in_flight.clear();
        }
    }

    if (!bodies.empty()) {
        auto   began = std::chrono::steady_clock::now();
        size_t next  = 0;
        while (next < bodies.size()) {
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() > EXIT_BUDGET_S)
                break;
            // A failed send has already been kept or dropped by finish(). The rest would most
            // likely fail the same way (offline), so they wait for the next launch untried.
            if (!send(bodies[next++], true))
                break;
        }
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pending.insert(s.pending.end(), bodies.begin() + next, bodies.end());
        save_pending_locked(s);
    }

    state().watchdog_wake.notify_all();
    boost::system::error_code ec;
    fs::remove(marker_path(session), ec);
    uninstall_crash_handlers();
}

std::string preset_base_name(const PresetCollection &collection, const Preset &preset)
{
    if (preset.is_system)
        return preset.name;
    const Preset *parent = collection.get_preset_parent(preset);
    return parent && parent->is_system ? parent->name : "custom";
}

std::string redact_message(const std::string &message, const std::vector<std::string> &names)
{
    std::string out = message.substr(0, message.find('\n'));

    // Longest names first, so "Bracket" does not leave "_v2" behind from "Bracket_v2".
    std::vector<std::string> sorted(names.begin(), names.end());
    std::sort(sorted.begin(), sorted.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    for (const std::string &name : sorted) {
        if (name.size() < 2)
            continue;
        for (size_t at = out.find(name); at != std::string::npos; at = out.find(name, at + 6))
            out.replace(at, name.size(), "<name>");
    }

    static const std::regex quoted(R"("[^"]*"|'[^']*'|\xE2\x80\x9C.*?\xE2\x80\x9D|\xC2\xAB.*?\xC2\xBB)");
    out = std::regex_replace(out, quoted, "<...>");
    // A path runs on through directory names with spaces in them (C:\Users\Jane Smith\...): every
    // part followed by a separator is still the path. Hiding too much is the safe direction. Paths
    // from the home folder (~/...), relative ones (./, ../) and any word with two separators in it
    // (Clouds/MEGA/ArcWelder) are paths too.
    static const std::regex path(R"([A-Za-z]:[\\/](?:[^\\/\n]*[\\/])*\S*|\\\\(?:[^\\/\n]*[\\/])*\S*|(^|\s)(?:/(?:[^/\n]*/)+\S*|(?:~|\.\.?)[\\/](?:[^\\/\n]*[\\/])*\S*|[^\s\\/:]+(?:[\\/][^\s\\/]*){2,}))");
    out = std::regex_replace(out, path, "$1<path>");
    // A model, project or G-code file named without a path.
    static const std::regex file(R"(\S*\.(?:stl|3mf|obj|step|stp|amf|ply|gcode|bgcode|gco|svg|drc|zip)\b)", std::regex::icase);
    out = std::regex_replace(out, file, "<file>");

    // Trim, then cut on a UTF-8 character boundary.
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), not_space));
    out.erase(std::find_if(out.rbegin(), out.rend(), not_space).base(), out.end());
    if (out.size() > 160) {
        size_t cut = 160;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    return out;
}

} // namespace UsageTelemetry
} // namespace Slic3r
