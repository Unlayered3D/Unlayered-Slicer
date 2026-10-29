#ifndef slic3r_ServiceRefusal_hpp_
#define slic3r_ServiceRefusal_hpp_

#include <optional>
#include <string>

namespace Slic3r {

// A remote service saying "no" to a request, in the one shape this program understands from any
// slicing service or account provider (SLICE_SERVICE_PROTOCOL.md, "Refusals"):
//
//   HTTP 401 | 402 | 403
//   {"error": {"code": "...", "message": "...", "action_url": "https://...", "action_label": "..."}}
//
// The program shows `message` in its usual error UI with one button that opens `action_url` in the
// system browser. What the service refuses, and why, is entirely the service's business: nothing
// here knows about plans, subscriptions or accounts beyond "a 401 may be solved by signing in".
//
// Parsing is deliberately tolerant of the shapes services send today (all optional):
//   - `error` as a plain string (= message);
//   - FastAPI's default `{"detail": "..."}` (a string; = message);
//   - `upgradeUrl` (= action_url).
// An action_url that is a path ("/pricing") is resolved against the origin that answered. Anything
// that is not http(s) once resolved is dropped, so a refusal can never make the program open a
// file, a custom scheme or a script.
struct ServiceRefusal
{
    unsigned    status = 0; // 401, 402 or 403
    std::string code;
    std::string message;      // never empty after parse_service_refusal
    std::string action_url;   // http(s) only, or empty
    std::string action_label; // may be empty: the UI says "Open"
};

// The refusal in `body`, when `status` is 401/402/403; nullopt for any other status.
// `responding_url` is the URL the request went to, for resolving a relative action_url.
std::optional<ServiceRefusal> parse_service_refusal(unsigned status, const std::string &body,
                                                    const std::string &responding_url);

// "HTTPS://Example.com:443/x?y" -> "https://example.com"; "http://localhost:8000/" ->
// "http://localhost:8000". Empty for anything that is not an http(s) URL with a host.
std::string url_origin(const std::string &url);

// True when the two URLs are http(s) with the same scheme and the same registrable domain
// ("site"): slice.example.com and example.com are same-site, example.com and example.org are not.
// The registrable domain is approximated without a public-suffix list: the last two labels, or the
// last three under a two-letter country code whose second level is a common generic one
// (co.uk, com.au, ...). An IP literal or a single-label host (localhost) is its own site.
bool same_site(const std::string &url_a, const std::string &url_b);

// True for an absolute http:// or https:// URL with a host.
bool is_http_url(const std::string &url);

} // namespace Slic3r

#endif // slic3r_ServiceRefusal_hpp_
