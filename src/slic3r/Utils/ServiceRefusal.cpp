#include "ServiceRefusal.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <vector>

using json = nlohmann::json;

namespace Slic3r {

namespace {

struct UrlParts
{
    std::string scheme; // lowercased
    std::string host;   // lowercased, brackets kept off an IPv6 literal
    std::string port;   // empty for the scheme's default
};

bool split_url(const std::string &url, UrlParts &out)
{
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        return false;
    out.scheme = boost::to_lower_copy(url.substr(0, scheme_end));
    if (out.scheme != "http" && out.scheme != "https")
        return false;
    std::string rest = url.substr(scheme_end + 3);
    rest             = rest.substr(0, rest.find_first_of("/?#"));
    if (const auto at = rest.rfind('@'); at != std::string::npos)
        rest = rest.substr(at + 1);
    std::string host, port;
    if (!rest.empty() && rest.front() == '[') {
        const auto close = rest.find(']');
        if (close == std::string::npos)
            return false;
        host = rest.substr(1, close - 1);
        if (close + 1 < rest.size() && rest[close + 1] == ':')
            port = rest.substr(close + 2);
    } else {
        const auto colon = rest.find(':');
        host             = rest.substr(0, colon);
        if (colon != std::string::npos)
            port = rest.substr(colon + 1);
    }
    boost::to_lower(host);
    if (host.empty())
        return false;
    if ((out.scheme == "http" && port == "80") || (out.scheme == "https" && port == "443"))
        port.clear();
    out.host = host;
    out.port = port;
    return true;
}

bool is_ip_literal(const std::string &host)
{
    if (host.find(':') != std::string::npos)
        return true; // IPv6
    return !host.empty() && std::all_of(host.begin(), host.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

std::string registrable_domain(const std::string &host)
{
    if (is_ip_literal(host))
        return host;
    std::vector<std::string> labels;
    size_t                   start = 0;
    while (true) {
        const auto dot = host.find('.', start);
        labels.push_back(host.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    if (labels.size() <= 2)
        return host;
    static const char *generic_second[] = {"co", "com", "net", "org", "gov", "ac", "edu", "ne", "or", "go"};
    const std::string &tld    = labels[labels.size() - 1];
    const std::string &second = labels[labels.size() - 2];
    size_t             keep   = 2;
    if (tld.size() == 2 && std::any_of(std::begin(generic_second), std::end(generic_second),
                                       [&second](const char *g) { return second == g; }))
        keep = 3;
    std::string out;
    for (size_t i = labels.size() - keep; i < labels.size(); ++i)
        out += (out.empty() ? "" : ".") + labels[i];
    return out;
}

std::string string_at(const json &j, const char *key)
{
    if (j.is_object() && j.contains(key) && j.at(key).is_string())
        return boost::trim_copy(j.at(key).get<std::string>());
    return {};
}

} // namespace

std::string url_origin(const std::string &url)
{
    UrlParts p;
    if (!split_url(url, p))
        return {};
    const std::string host = p.host.find(':') != std::string::npos ? "[" + p.host + "]" : p.host;
    return p.scheme + "://" + host + (p.port.empty() ? "" : ":" + p.port);
}

bool is_http_url(const std::string &url)
{
    UrlParts p;
    return split_url(url, p);
}

bool same_site(const std::string &url_a, const std::string &url_b)
{
    UrlParts a, b;
    if (!split_url(url_a, a) || !split_url(url_b, b))
        return false;
    return a.scheme == b.scheme && registrable_domain(a.host) == registrable_domain(b.host);
}

std::optional<ServiceRefusal> parse_service_refusal(unsigned status, const std::string &body,
                                                    const std::string &responding_url)
{
    if (status != 401 && status != 402 && status != 403)
        return std::nullopt;

    ServiceRefusal r;
    r.status = status;
    try {
        const json j = json::parse(body);
        if (j.is_object()) {
            if (j.contains("error") && j.at("error").is_object()) {
                const json &e  = j.at("error");
                r.code         = string_at(e, "code");
                r.message      = string_at(e, "message");
                r.action_url   = string_at(e, "action_url");
                r.action_label = string_at(e, "action_label");
            } else if (j.contains("error") && j.at("error").is_string()) {
                r.message = string_at(j, "error");
            }
            if (r.message.empty())
                r.message = string_at(j, "detail"); // FastAPI's HTTPException default
            if (r.action_url.empty())
                r.action_url = string_at(j, "upgradeUrl");
            if (r.code.empty())
                r.code = string_at(j, "code");
        }
    } catch (const std::exception &) {
        // Not JSON (a proxy's page): the status alone says what happened.
    }

    if (r.message.size() > 500)
        r.message = r.message.substr(0, 500) + "...";
    if (r.message.empty())
        r.message = status == 401 ? "The service needs you to sign in."
                  : status == 402 ? "The service needs payment before it can do this."
                                  : "The service refused this request.";

    // A path is relative to whoever answered.
    if (boost::starts_with(r.action_url, "/") && !boost::starts_with(r.action_url, "//")) {
        const std::string origin = url_origin(responding_url);
        r.action_url             = origin.empty() ? std::string() : origin + r.action_url;
    }
    if (!is_http_url(r.action_url) || r.action_url.find_first_of(" \r\n\t") != std::string::npos) {
        r.action_url.clear();
        r.action_label.clear();
    }
    if (r.action_label.size() > 60)
        r.action_label = r.action_label.substr(0, 60);
    return r;
}

} // namespace Slic3r
