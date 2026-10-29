#include "PrinterUpdateCheck.hpp"

#include <cctype>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "libslic3r/Semver.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/SysFilesBundle.hpp"

namespace Slic3r {

namespace {

// "https" from "HTTPS://host/...", empty when the URL names no scheme at all.
std::string scheme_of(const std::string &url)
{
    const size_t colon = url.find("://");
    if (colon == std::string::npos || colon == 0)
        return {};
    return boost::algorithm::to_lower_copy(url.substr(0, colon));
}

// Lowercased host and the port, with an omitted port read as the scheme's default so
// "https://unlayered3d.com" and "https://unlayered3d.com:443" are the same origin.
bool origin_of(const std::string &url, std::string &scheme, std::string &host, std::string &port)
{
    scheme = scheme_of(url);
    if (scheme != "https" && scheme != "http")
        return false;
    host = Http::get_host_from_url(url, &port);
    // get_host_from_url returns its input unchanged when it cannot parse it; a host never
    // contains a slash, so that is how a failure shows.
    if (host.empty() || host.find('/') != std::string::npos)
        return false;
    boost::algorithm::to_lower(host);
    if (port.empty())
        port = scheme == "https" ? "443" : "80";
    return true;
}

} // namespace

bool feed_token_allowed(const std::string &url, const std::string &site_url)
{
    std::string us, uh, up, ss, sh, sp;
    if (!origin_of(url, us, uh, up) || !origin_of(site_url, ss, sh, sp))
        return false;
    return us == ss && uh == sh && up == sp;
}

std::string FeedCredential::token_for(const std::string &url) const
{
    return !token.empty() && feed_token_allowed(url, site_url) ? token : std::string();
}

bool apply_feed_credential(Http &http, const std::string &url, const FeedCredential &credential)
{
    const std::string token = credential.token_for(url);
    if (token.empty())
        return false;
    http.tls_verify(true).header("Authorization", "Bearer " + token);
    return true;
}

bool firmware_is_newer(const std::string &running, const std::string &available)
{
    if (running.empty() || available.empty() || running == available)
        return false;
    const int have = unlayered_fork_number(running);
    const int got  = unlayered_fork_number(available);
    if (got >= 0)
        return have < 0 || got > have;
    if (have >= 0)
        return false; // stock firmware is never an update for a fork build
    const auto r = Semver::parse(running);
    const auto a = Semver::parse(available);
    return r && a && *a > *r;
}

int natural_version_compare(const std::string &a, const std::string &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            // Compare the digit runs by value without converting: skip leading zeros, then the
            // longer run is the bigger number, and equal lengths compare digit by digit.
            while (i < a.size() && a[i] == '0') ++i;
            while (j < b.size() && b[j] == '0') ++j;
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            if (ei - i != ej - j)
                return ei - i < ej - j ? -1 : 1;
            for (; i < ei; ++i, ++j)
                if (a[i] != b[j])
                    return a[i] < b[j] ? -1 : 1;
            continue;
        }
        if (da != db)
            return da ? 1 : -1; // "1.0.1" after "1.0-rc": a number outranks a suffix
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
        if (ca != cb)
            return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    return 0;
}

bool config_is_newer(bool installed_known, const std::string &installed, const std::string &available)
{
    if (available.empty())
        return false;
    if (!installed_known || installed.empty())
        return true;
    return natural_version_compare(available, installed) > 0;
}

std::string PrinterUpdateSkips::key_for(const std::string &board_id)
{
    std::string key = board_id;
    for (char &c : key)
        if (c == '=' || c == ';' || c == '\n' || c == '\r' || c == '.' || c == '[' || c == ']')
            c = '_';
    return key;
}

void PrinterUpdateSkips::skip(const std::string &board_id, const std::string &firmware, const std::string &config)
{
    if (board_id.empty() || !m_set)
        return;
    if (!firmware.empty())
        m_set(FIRMWARE_SECTION, key_for(board_id), firmware);
    if (!config.empty())
        m_set(CONFIG_SECTION, key_for(board_id), config);
}

bool PrinterUpdateSkips::firmware_skipped(const std::string &board_id, const std::string &version) const
{
    return !board_id.empty() && !version.empty() && m_get && m_get(FIRMWARE_SECTION, key_for(board_id)) == version;
}

bool PrinterUpdateSkips::config_skipped(const std::string &board_id, const std::string &version) const
{
    return !board_id.empty() && !version.empty() && m_get && m_get(CONFIG_SECTION, key_for(board_id)) == version;
}

PrinterUpdateDecision decide_printer_update(const PrinterUpdateFacts &facts, const PrinterUpdateSkips &skips)
{
    PrinterUpdateDecision d;
    d.firmware = firmware_is_newer(facts.running_firmware, facts.available_firmware) &&
                 !skips.firmware_skipped(facts.board_id, facts.available_firmware);
    d.config = config_is_newer(facts.config_known, facts.installed_config, facts.available_config) &&
               !skips.config_skipped(facts.board_id, facts.available_config);
    return d;
}

PrinterUpdateSkips no_printer_update_skips()
{
    return PrinterUpdateSkips([](const std::string &, const std::string &) { return std::string(); },
                              [](const std::string &, const std::string &, const std::string &) {});
}

PrinterUpdateCheckOutcome classify_printer_update_check(bool reachable, const PrinterUpdateFacts &facts)
{
    if (!reachable)
        return PrinterUpdateCheckOutcome::Unreachable;
    if (decide_printer_update(facts, no_printer_update_skips()).any())
        return PrinterUpdateCheckOutcome::Available;
    if (facts.available_firmware.empty() && facts.available_config.empty())
        return PrinterUpdateCheckOutcome::NoFeed;
    return PrinterUpdateCheckOutcome::UpToDate;
}

bool printer_update_check_after_setup(PrinterUpdateCheckOutcome outcome) { return outcome != PrinterUpdateCheckOutcome::UpToDate; }

} // namespace Slic3r
