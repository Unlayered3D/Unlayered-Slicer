#include "CrashLogScrub.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <regex>

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r {

namespace {

// While a line is being scrubbed, what has been replaced is held as \x01<code>\x02, so a later rule
// (a user name "home", a file rule) can never match inside an earlier replacement. The tags become
// the readable "<home>", "<file>", ... at the very end. The input loses any \x01 / \x02 of its own.
constexpr char OPEN = '\x01', CLOSE = '\x02';

std::string tag(char code)
{
    std::string s;
    s += OPEN;
    s += code;
    s += CLOSE;
    return s;
}

const std::string HOME = tag('H'), USER = tag('U'), EMAIL = tag('E'), REDACTED = tag('R'), FILE_ = tag('F'), IP = tag('I'),
                  HOST = tag('N');

std::string untag(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 64);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == OPEN && i + 2 < s.size() && s[i + 2] == CLOSE) {
            switch (s[i + 1]) {
            case 'H': out += "<home>"; break;
            case 'U': out += "<user>"; break;
            case 'E': out += "<email>"; break;
            case 'R': out += "<redacted>"; break;
            case 'F': out += "<file>"; break;
            case 'I': out += "<ip>"; break;
            case 'N': out += "<host>"; break;
            default: out += "<redacted>"; break;
            }
            i += 2;
        } else
            out += s[i];
    }
    return out;
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// Part of a word for whole-word matching. Bytes of multi-byte UTF-8 characters count as letters, and
// so do the tag markers, so a short name never matches a tag's code letter.
bool word_char(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == OPEN || c == CLOSE || u >= 0x80;
}

// Every occurrence of `needle` (ASCII case-insensitive) becomes `with`.
void replace_ci(std::string &s, const std::string &needle, const std::string &with, bool whole_word)
{
    if (needle.empty() || needle.size() > s.size())
        return;
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        bool match = i + needle.size() <= s.size();
        for (size_t k = 0; match && k < needle.size(); ++k)
            if (ascii_lower(s[i + k]) != ascii_lower(needle[k]))
                match = false;
        if (match && whole_word) {
            const size_t end = i + needle.size();
            if ((i > 0 && word_char(s[i - 1])) || (end < s.size() && word_char(s[end])))
                match = false;
        }
        if (match) {
            out += with;
            i += needle.size();
        } else
            out += s[i++];
    }
    s.swap(out);
}

// Each match of `re` in `in` replaced by fn(match).
template<class Fn> std::string replace_each(const std::string &in, const std::regex &re, Fn fn)
{
    std::string out;
    size_t      last = 0;
    for (std::sregex_iterator it(in.begin(), in.end(), re), end; it != end; ++it) {
        const std::smatch &m = *it;
        const size_t       at = size_t(m.position(0));
        out.append(in, last, at - last);
        out += fn(m);
        last = at + size_t(m.length(0));
    }
    out.append(in, last, std::string::npos);
    return out;
}

bool has_digit(const std::string &s) { return std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); }
bool has_alpha(const std::string &s) { return std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isalpha(c); }); }

// "192.0.2.10": a real address, not 127.x.x.x or 0.0.0.0 (they say nothing about anyone).
bool private_ipv4(const std::string &s)
{
    int    parts[4] = {0, 0, 0, 0};
    size_t n = 0, start = 0;
    while (n < 4) {
        const size_t dot = s.find('.', start);
        const std::string part = s.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        // An address is never written with leading zeros; a version ("01.09.07.52") often is.
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0'))
            return false;
        parts[n++] = std::atoi(part.c_str());
        if (parts[n - 1] > 255)
            return false;
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    if (n != 4)
        return false;
    if (parts[0] == 127 || (parts[0] == 0 && parts[1] == 0 && parts[2] == 0 && parts[3] == 0))
        return false;
    return true;
}

// Something shaped like an IPv6 (or MAC) address, rather than a time ("13:45:22") or a C++ scope.
bool looks_like_ipv6(const std::string &s)
{
    if (s == "::1" || s == "::")
        return false;
    const size_t colons = size_t(std::count(s.begin(), s.end(), ':'));
    const bool   double_colon = s.find("::") != std::string::npos;
    size_t       groups = 0;
    bool         in_group = false;
    for (char c : s) {
        if (c == ':')
            in_group = false;
        else if (!in_group) {
            in_group = true;
            ++groups;
        }
    }
    if (colons == 7 && !double_colon)
        return true;
    if (colons == 5 && !double_colon && s.size() == 17)
        return true; // a MAC address, aa:bb:cc:dd:ee:ff
    // Shorter forms need a digit: "Add::Bed::Cab" is a C++ name, not an address.
    if (!has_digit(s))
        return false;
    if (double_colon && colons >= 2 && groups >= 2)
        return true;
    return colons >= 5;
}

// ".gcode.3mf" is tried first, so it is kept whole rather than read as a ".gcode" file.
const char *FILE_EXT = R"((\.gcode\.3mf|\.(?:stl|3mf|obj|step|stp|amf|ply|gcode|bgcode|svg|png|jpe?g|drc)))";

const std::regex::flag_type CI = std::regex::ECMAScript | std::regex::icase;

struct Rules
{
    using R = std::regex;

    // Any Windows profile folder, with \, \\ or / (and spaces in the name when a separator follows).
    R home_windows{R"([A-Za-z]:(?:\\{1,2}|/)Users(?:\\{1,2}|/)(?:[^\\/\r\n"'<>|:*?\x01\x02]+(?=[\\/])|[^\s\\/"'<>|:*?\x01\x02]+))", CI};
    // macOS /Users/<name>, Linux /home/<name>.
    R home_unix{R"(/(?:Users|home)/(?:[^\\/\r\n"'<>|:*?\x01\x02]+(?=[\\/])|[^\s\\/"'<>|:*?\x01\x02]+))", CI};
    R url_userinfo{R"(([A-Za-z][A-Za-z0-9+.-]*://)[^/\s:@\x01\x02]+:[^/\s@\x01\x02]+@)", CI};
    R email{R"([A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,})", CI};
    R bearer{R"(\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]{6,})", CI};
    R named_secret{R"(\b([A-Za-z0-9_-]*(?:token|secret|passwd|password|pwd|apikey|api_key|auth|authorization|cookie|credentials?|private_key|access_key))(\s*["']?\s*[=:]\s*["']?)([^\s"'&,;)}\]\x01\x02]+))", CI};
    // A bare "key" only as a parameter ("?key=..."): "metadata key = Title" names a field, not a secret.
    R bare_key{R"(\b(key)(=)([^\s"'&,;)}\]\x01\x02]+))", CI};
    R file_quoted{std::string(R"((["'])[^"'\r\n\x01\x02\\/]*?)") + FILE_EXT + R"((?=["']))", CI};
    R file_after_separator{std::string(R"(([\\/])[^\\/\r\n"'<>|:*?\x01\x02]*?)") + FILE_EXT + R"((?![A-Za-z0-9]))", CI};
    // Not straight after a replacement: "<file>.gcode" must not become "<file><file>.3mf".
    R file_bare{std::string(R"((^|[\s\\/"'<>|:*?\x01])[^\s\\/"'<>|:*?\x01\x02]+?)") + FILE_EXT + R"((?![A-Za-z0-9]))", CI};
    R long_run{R"([A-Za-z0-9+_=-]{33,})"};
    R ipv4{R"((^|[^0-9A-Za-z.])((?:[0-9]{1,3}\.){3}[0-9]{1,3})(?![0-9]|\.[0-9]))"};
    R ipv6{R"((^|[^0-9A-Za-z:_.])([0-9A-Fa-f]{0,4}(?::[0-9A-Fa-f]{0,4}){2,7})(?![0-9A-Za-z:_]))"};
};

// Every spelling of a home folder a log may hold: as given, with / or \, and with \\ (JSON).
std::vector<std::string> home_spellings(const std::vector<std::string> &homes)
{
    std::vector<std::string> out;
    for (std::string h : homes) {
        while (h.size() > 1 && (h.back() == '\\' || h.back() == '/'))
            h.pop_back();
        // "C:", "/", "~": too short to be anyone's folder.
        if (h.size() < 4)
            continue;
        std::string fwd = h, back = h, doubled;
        std::replace(fwd.begin(), fwd.end(), '\\', '/');
        std::replace(back.begin(), back.end(), '/', '\\');
        for (char c : back) {
            doubled += c;
            if (c == '\\')
                doubled += '\\';
        }
        for (const std::string &s : {h, fwd, back, doubled})
            if (std::find(out.begin(), out.end(), s) == out.end())
                out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    return out;
}

std::vector<std::string> longest_first(std::vector<std::string> names)
{
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string &n) {
                                   return n.empty() || n.find(OPEN) != std::string::npos || n.find(CLOSE) != std::string::npos;
                               }),
                names.end());
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::string scrub_line(std::string line, const Rules &r, const std::vector<std::string> &homes,
                       const std::vector<std::string> &hosts, const std::vector<std::string> &users)
{
    // 1. The home folder: this computer's own spellings first, then anyone's.
    for (const std::string &h : homes)
        replace_ci(line, h, HOME, false);
    line = std::regex_replace(line, r.home_windows, HOME);
    line = std::regex_replace(line, r.home_unix, HOME);
    for (const char *var : {"%USERPROFILE%", "%HOMEPATH%", "${HOME}", "$HOME"})
        replace_ci(line, var, HOME, false);

    // 2. Credentials and addresses, before e-mail (a URL's user:password@host looks like one).
    line = std::regex_replace(line, r.url_userinfo, "$1" + REDACTED + "@");
    line = std::regex_replace(line, r.email, EMAIL);
    line = std::regex_replace(line, r.bearer, "$1 " + REDACTED);
    line = std::regex_replace(line, r.named_secret, "$1$2" + REDACTED);
    // ... unless its value is a setting's name ("Print::apply, key=wall_loops"): no key looks like that.
    line = replace_each(line, r.bare_key, [](const std::smatch &m) {
        const std::string v = m.str(3);
        const bool setting  = std::all_of(v.begin(), v.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
        return m.str(1) + m.str(2) + (setting ? v : REDACTED);
    });

    // 3. File names: keep the extension only.
    line = std::regex_replace(line, r.file_quoted, "$1" + FILE_ + "$2");
    line = std::regex_replace(line, r.file_after_separator, "$1" + FILE_ + "$2");
    line = std::regex_replace(line, r.file_bare, "$1" + FILE_ + "$2");

    // 4. Long runs of letters and digits: digests, keys, tokens, ids.
    line = replace_each(line, r.long_run, [](const std::smatch &m) {
        const std::string s = m.str(0);
        return has_digit(s) && has_alpha(s) ? REDACTED : s;
    });

    // 5. IP addresses.
    line = replace_each(line, r.ipv4, [](const std::smatch &m) {
        return m.str(1) + (private_ipv4(m.str(2)) ? IP : m.str(2));
    });
    line = replace_each(line, r.ipv6, [](const std::smatch &m) {
        return m.str(1) + (looks_like_ipv6(m.str(2)) ? IP : m.str(2));
    });

    // 6. The computer's name, then the person's (a host name often contains it).
    for (const std::string &h : hosts)
        replace_ci(line, h, HOST, h.size() < 3);
    for (const std::string &u : users)
        replace_ci(line, u, USER, u.size() < 3);
    return line;
}

#ifdef _WIN32
std::string env_utf8(const wchar_t *name)
{
    const wchar_t *v = _wgetenv(name);
    return v && *v ? boost::nowide::narrow(v) : std::string();
}
#else
std::string env_utf8(const char *name)
{
    const char *v = std::getenv(name);
    return v && *v ? std::string(v) : std::string();
}
#endif

void add_unique(std::vector<std::string> &list, const std::string &value)
{
    if (!value.empty() && std::find(list.begin(), list.end(), value) == list.end())
        list.push_back(value);
}

std::string leaf_name(std::string path)
{
    while (!path.empty() && (path.back() == '\\' || path.back() == '/'))
        path.pop_back();
    const size_t sep = path.find_last_of("\\/");
    return sep == std::string::npos ? std::string() : path.substr(sep + 1);
}

fs::path to_path(const std::string &utf8)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string from_path(const fs::path &p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.wstring());
#else
    return p.string();
#endif
}

} // namespace

ScrubContext ScrubContext::from_environment()
{
    ScrubContext c;
#ifdef _WIN32
    add_unique(c.home_dirs, env_utf8(L"USERPROFILE"));
    add_unique(c.home_dirs, env_utf8(L"HOME"));
    {
        const std::string drive = env_utf8(L"HOMEDRIVE"), path = env_utf8(L"HOMEPATH");
        if (!drive.empty() && !path.empty())
            add_unique(c.home_dirs, drive + path);
    }
    add_unique(c.user_names, env_utf8(L"USERNAME"));
    add_unique(c.user_names, env_utf8(L"USER"));
    {
        wchar_t name[256] = {0};
        DWORD   size      = 256;
        if (::GetUserNameW(name, &size))
            add_unique(c.user_names, boost::nowide::narrow(name));
    }
    add_unique(c.host_names, env_utf8(L"COMPUTERNAME"));
    for (COMPUTER_NAME_FORMAT format : {ComputerNameNetBIOS, ComputerNameDnsHostname, ComputerNameDnsFullyQualified}) {
        wchar_t name[256] = {0};
        DWORD   size      = 256;
        if (::GetComputerNameExW(format, name, &size))
            add_unique(c.host_names, boost::nowide::narrow(name));
    }
#else
    add_unique(c.home_dirs, env_utf8("HOME"));
    add_unique(c.user_names, env_utf8("USER"));
    add_unique(c.user_names, env_utf8("LOGNAME"));
    if (const passwd *pw = ::getpwuid(::getuid())) {
        if (pw->pw_dir)
            add_unique(c.home_dirs, pw->pw_dir);
        if (pw->pw_name)
            add_unique(c.user_names, pw->pw_name);
        // The full name ("Jane Smith") as macOS and many Linux setups keep it.
        if (pw->pw_gecos && *pw->pw_gecos) {
            std::string gecos = pw->pw_gecos;
            gecos = gecos.substr(0, gecos.find(','));
            add_unique(c.user_names, gecos);
        }
    }
    add_unique(c.host_names, env_utf8("HOSTNAME"));
    char host[256] = {0};
    if (::gethostname(host, sizeof(host) - 1) == 0 && host[0]) {
        std::string h = host;
        add_unique(c.host_names, h);
        const size_t dot = h.find('.');
        if (dot != std::string::npos && dot > 0)
            add_unique(c.host_names, h.substr(0, dot)); // "janes-macbook" from "janes-macbook.local"
    }
#endif
    // The home folder's own name is often the login name in another form.
    for (const std::string &h : std::vector<std::string>(c.home_dirs))
        add_unique(c.user_names, leaf_name(h));
    return c;
}

std::string scrub_log_for_upload(std::string text, const ScrubContext &context)
{
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == OPEN || c == CLOSE; }), text.end());

    const Rules                    rules;
    const std::vector<std::string> homes = home_spellings(context.home_dirs);
    std::vector<std::string>       users = context.user_names;
    for (const std::string &h : context.home_dirs)
        users.push_back(leaf_name(h));
    users                                = longest_first(std::move(users));
    const std::vector<std::string> hosts = longest_first(context.host_names);

    std::string out;
    out.reserve(text.size());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        const bool newline = end != std::string::npos;
        if (!newline)
            end = text.size();
        std::string line = text.substr(start, end - start);
        bool        cr   = !line.empty() && line.back() == '\r';
        if (cr)
            line.pop_back();
        if (line.size() > 8 * 1024)
            line = "<long line removed>";
        else {
            try {
                line = untag(scrub_line(std::move(line), rules, homes, hosts, users));
            } catch (...) {
                // std::regex gives up on some inputs (error_complexity / error_stack): drop the line.
                line = "<line removed>";
            }
        }
        out += line;
        if (cr)
            out += '\r';
        if (newline)
            out += '\n';
        start = end + 1;
    }
    return out;
}

bool read_log_tail(const std::string &path_utf8, LogTail &out, size_t max_bytes)
{
    out = LogTail();
    boost::nowide::ifstream in(path_utf8.c_str(), std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0)
        return false;
    out.original_bytes = uint64_t(size);
    out.truncated      = uint64_t(size) > max_bytes;
    const std::streamoff from = out.truncated ? size - std::streamoff(max_bytes) : 0;
    in.seekg(from, std::ios::beg);
    out.text.resize(size_t(size - from));
    in.read(out.text.data(), std::streamsize(out.text.size()));
    out.text.resize(size_t(in.gcount()));
    if (out.truncated) {
        // Start at a complete line.
        const size_t nl = out.text.find('\n');
        out.text.erase(0, nl == std::string::npos ? out.text.size() : nl + 1);
    }
    return true;
}

std::string find_session_log(const std::string &log_dir_utf8, const std::string &file_name, long pid, int64_t started_ms)
{
    boost::system::error_code ec;
    const fs::path            dir = to_path(log_dir_utf8);
    if (!fs::is_directory(dir, ec))
        return {};

    // "debug_..._1234.log.0" -> stem "debug_..._1234.log." and the part number.
    auto split_part = [](const std::string &name, std::string &stem, long &part) {
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos || dot + 1 >= name.size() ||
            name.find_first_not_of("0123456789", dot + 1) != std::string::npos)
            return false;
        stem = name.substr(0, dot + 1);
        part = std::atol(name.c_str() + dot + 1);
        return true;
    };

    std::string wanted_stem;
    long        wanted_part = -1;
    const bool  by_name     = !file_name.empty() && file_name.find_first_of("\\/") == std::string::npos &&
                         split_part(file_name, wanted_stem, wanted_part);
    const std::string pid_suffix = "_" + std::to_string(pid) + ".log.";

    std::string best;
    long        best_part = -1;
    std::time_t best_time = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!fs::is_regular_file(it->path(), ec))
            continue;
        const std::string name = from_path(it->path().filename());
        std::string       stem;
        long              part = -1;
        if (!split_part(name, stem, part))
            continue;
        if (by_name) {
            // The recorded file, or a later part of it after rotation.
            if (stem == wanted_stem && part >= wanted_part && part > best_part) {
                best      = from_path(it->path());
                best_part = part;
            }
        } else if (pid > 0 && name.rfind("debug_", 0) == 0 && stem.size() > pid_suffix.size() &&
                   stem.compare(stem.size() - pid_suffix.size(), pid_suffix.size(), pid_suffix) == 0) {
            // Same process id: the one written to since that session started (ids are reused).
            const std::time_t written = fs::last_write_time(it->path(), ec);
            if (ec)
                continue;
            if (started_ms > 0 && int64_t(written) * 1000 < started_ms - 60 * 1000)
                continue;
            if (written > best_time || (written == best_time && part > best_part)) {
                best      = from_path(it->path());
                best_time = written;
                best_part = part;
            }
        }
    }
    return best;
}

std::string crash_log_url(const std::string &telemetry_url)
{
    const size_t scheme = telemetry_url.find("://");
    if (scheme == std::string::npos || scheme == 0)
        return {};
    const size_t path = telemetry_url.find_first_of("/?#", scheme + 3);
    const std::string origin = telemetry_url.substr(0, path);
    if (origin.size() <= scheme + 3)
        return {};
    return origin + "/api/slicer/crash-log";
}

} // namespace Slic3r
