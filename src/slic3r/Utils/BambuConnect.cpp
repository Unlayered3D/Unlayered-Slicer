#include "BambuConnect.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <random>
#include <stdexcept>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <boost/nowide/convert.hpp>
#elif defined(__APPLE__)
#include <wx/string.h>
#include <wx/utils.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r {
namespace BambuConnect {

namespace {

std::string to_hex(const unsigned char* md, unsigned int len)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(hex[md[i] >> 4]);
        out.push_back(hex[md[i] & 0x0F]);
    }
    return out;
}

// Streams chunks into SHA-256; `feed` calls the given sink until the input is exhausted and
// returns false on a read error.
template<typename Feed> std::string sha256_stream(Feed&& feed)
{
    EVP_MD_CTX* ctx = ::EVP_MD_CTX_new();
    if (ctx == nullptr)
        return {};
    std::string out;
    if (::EVP_DigestInit_ex(ctx, ::EVP_sha256(), nullptr) == 1 &&
        feed([ctx](const void* data, size_t len) { return ::EVP_DigestUpdate(ctx, data, len) == 1; })) {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int  md_len = 0;
        if (::EVP_DigestFinal_ex(ctx, md, &md_len) == 1)
            out = to_hex(md, md_len);
    }
    ::EVP_MD_CTX_free(ctx);
    return out;
}

std::string utc_now(const char* format)
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

} // namespace

bool is_supported_platform()
{
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

std::string encode_query_value(const std::string& utf8)
{
    static const char* hex = "0123456789ABCDEF";
    std::string        out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(char(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string build_import_uri(const fs::path& package, const std::string& display_name)
{
    // generic_string(): forward slashes on Windows too (C:/Users/...), the starting candidate.
    // On Windows boost's narrow strings are UTF-8 here (the app imbues a UTF-8 path locale).
    const std::string path_u8 = package.generic_string();
    return "bambu-connect://import-file?path=" + encode_query_value(path_u8) + "&name=" + encode_query_value(display_name) +
           "&version=1.0.0";
}

std::string sha256_file(const fs::path& file)
{
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    if (!in)
        return {};
    return sha256_stream([&in](auto&& sink) {
        std::array<char, 1 << 16> buf;
        while (in) {
            in.read(buf.data(), buf.size());
            const std::streamsize n = in.gcount();
            if (n > 0 && !sink(buf.data(), size_t(n)))
                return false;
        }
        return in.eof();
    });
}

std::vector<GcodeEntry> gcode_entries(const fs::path& package, std::string& error)
{
    std::vector<GcodeEntry> entries;
    mz_zip_archive          zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, package.string())) {
        error = "cannot open the package as a ZIP archive";
        return entries;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory)
            continue;
        const std::string name = stat.m_filename;
        if (!boost::iends_with(name, ".gcode"))
            continue;
        size_t size = 0;
        void*  data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (data == nullptr) {
            error = "cannot read archive member " + name;
            entries.clear();
            break;
        }
        GcodeEntry entry;
        entry.name   = name;
        entry.bytes  = size;
        entry.sha256 = sha256_stream([data, size](auto&& sink) { return sink(data, size); });
        mz_free(data);
        entries.push_back(std::move(entry));
    }
    close_zip_reader(&zip);
    if (entries.empty() && error.empty())
        error = "the package holds no G-code";
    return entries;
}

fs::path jobs_root() { return fs::path(data_dir()) / "bambu_connect_jobs"; }

fs::path create_job_dir()
{
    const fs::path root = jobs_root();
    fs::create_directories(root);
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        char suffix[8];
        snprintf(suffix, sizeof(suffix), "%04x", unsigned(rd() & 0xFFFF));
        const fs::path dir = root / (utc_now("%Y%m%d-%H%M%S") + "-" + suffix);
        // create_directory() is false when it already exists: a completed job is never reused.
        if (fs::create_directory(dir))
            return dir;
    }
    throw std::runtime_error("could not create a Bambu Connect job directory under " + root.string());
}

void write_job_record(const fs::path& job_dir, const JobRecord& record)
{
    nlohmann::json gcode = nlohmann::json::array();
    for (const GcodeEntry& e : record.gcode_entries)
        gcode.push_back({{"name", e.name}, {"bytes", e.bytes}, {"sha256", e.sha256}});

    const nlohmann::json j = {
        {"job_id", record.job_id},
        {"created_at", record.created_at.empty() ? utc_now("%Y-%m-%dT%H:%M:%SZ") : record.created_at},
        {"slicer_version", record.slicer_version},
        {"plate_index", record.plate_index},
        {"display_name", record.display_name},
        {"target_summary",
         {{"printer_model", record.printer_model},
          {"nozzle", record.nozzle},
          {"build_plate", record.build_plate},
          {"filaments", record.filaments}}},
        {"package_path", record.package_path.generic_string()},
        {"package_sha256", record.package_sha256},
        {"gcode_entries", gcode},
        {"handoff_state", record.handoff_state},
        {"uri_format", "import-file?path&name&version=1.0.0"},
    };
    boost::nowide::ofstream out((job_dir / "job.json").string(), std::ios::binary | std::ios::trunc);
    out << j.dump(2);
}

bool record_launch_attempt(const fs::path& job_dir, const std::string& handoff_state)
{
    const fs::path file = job_dir / "job.json";
    nlohmann::json j;
    {
        boost::nowide::ifstream in(file.string(), std::ios::binary);
        if (!in)
            return false;
        j = nlohmann::json::parse(in, nullptr, false);
    }
    if (!j.is_object())
        return false;
    j["handoff_state"]          = handoff_state;
    j["last_launch_attempt_at"] = utc_now("%Y-%m-%dT%H:%M:%SZ");
    boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
    out << j.dump(2);
    return bool(out);
}

bool is_installed()
{
#ifdef _WIN32
    // The protocol registration: HKCR\bambu-connect with a "URL Protocol" value.
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, L"bambu-connect", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    const bool is_protocol = ::RegQueryValueExW(key, L"URL Protocol", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    ::RegCloseKey(key);
    return is_protocol;
#elif defined(__APPLE__)
    return !mac_handler_app_path().empty();
#else
    return false;
#endif
}

bool is_enabled(const std::string& stored_switch)
{
    if (!is_supported_platform())
        return false;
    return stored_switch.empty() || stored_switch == "1" || stored_switch == "true";
}

LaunchResult launch(const std::string& uri)
{
#ifdef _WIN32
    // Precheck the protocol registration. Presence is not proof that the import will work, only
    // that something will receive the URI.
    if (!is_installed())
        return LaunchResult::NoHandler;

    const std::wstring wuri = boost::nowide::widen(uri);
    const auto         rc   = reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", wuri.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (rc > 32)
        return LaunchResult::Requested;
    BOOST_LOG_TRIVIAL(warning) << "Bambu Connect: ShellExecuteW failed with " << rc;
    return (rc == SE_ERR_NOASSOC) ? LaunchResult::NoHandler : LaunchResult::Failed;
#elif defined(__APPLE__)
    // Launch Services first: wxLaunchDefaultBrowser() ([NSWorkspace openURL:]) only returns NO when
    // nothing opens the scheme, which cannot be told apart from another failure.
    if (!is_installed())
        return LaunchResult::NoHandler;
    return wxLaunchDefaultBrowser(wxString::FromUTF8(uri)) ? LaunchResult::Requested : LaunchResult::Failed;
#else
    (void) uri;
    return LaunchResult::Failed;
#endif
}

} // namespace BambuConnect
} // namespace Slic3r
