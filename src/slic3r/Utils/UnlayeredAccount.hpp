#ifndef slic3r_UnlayeredAccount_hpp_
#define slic3r_UnlayeredAccount_hpp_

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace Slic3r {

struct ServiceRefusal;

// The account the application itself is signed in as at its configured account provider
// (AppConfig account_provider_url; Unlayered's site unless the user changes it), for requests made
// by native code: the in-app update download, a short-lived token for a slicing service on the
// provider's site, and signed-in web pages from the provider. Authentication only -- the slicer
// draws no conclusions about what the account may do; every service decides that itself.
//
// This is NOT a web tab's sign-in. That one lives in the web view's cookie jar as an httpOnly
// cookie that neither the page nor this code can read, and native HTTP sends no cookies anyway. So
// the application signs in a second time through a browser handoff (the person confirms in their
// own browser) and collects a device token instead of a cookie. The provider protocol
// (SLICE_SERVICE_PROTOCOL.md, "Account provider"):
//
//   1. start_handoff()   POST /api/login/handoff/start            -> {id, secret}
//   2. the caller opens  <provider>/auth/slicer?id=<id>             in the default browser
//   3. collect()         POST /api/slicer/device/collect (polled)  -> {token, account}
//      refresh()         GET  /api/slicer/device/me                -> {name, email, edition_label?}
//      sign_out()        POST /api/slicer/device/signout
//      fetch_service_token()    POST /api/slicer/slice-token {"audience": origin} -> {token, expiresAt | expires_in}
//      fetch_web_session_path() POST /api/slicer/web-session {"next": path}       -> {path}
//
// The token is kept in the operating system's credential store (Windows Credential Manager, the
// macOS keychain, libsecret), never in the plain-text config file, keyed by the provider's host,
// and is sent only to the provider it came from, as `Authorization: Bearer`.
//
// All network calls are synchronous; call them from a worker thread. The object is thread safe.
class UnlayeredAccount
{
public:
    struct Profile
    {
        std::string name;
        std::string email;
        bool        software_access{false};
        // A label the provider gives the account's edition (/api/slicer/device/me `edition_label`),
        // appended to the application's name in the window title and nothing else: the slicer
        // grants and withholds nothing on it. A provider that still sends only the deprecated
        // `pro: true` is read as "Pro". Empty until refresh() has asked; never stored.
        std::string edition_label;
    };

    struct Handoff
    {
        std::string id;
        std::string secret;
        std::string browser_url; // where to send the person to confirm
    };

    enum class CollectResult { Pending, SignedIn, Failed };
    enum class RefreshResult { SignedIn, SignedOut, Unreachable };

    // A signed link to an update installer, from /api/slicer/update/download.
    struct DownloadLink
    {
        std::string url; // stands on its own; fetch it WITHOUT credentials
        std::string filename;
        std::string sha256;
        uint64_t    size{0};
        std::string version;
    };

    // site_url: the provider, e.g. "https://unlayered3d.com", no trailing slash required; empty for
    // no provider.
    explicit UnlayeredAccount(std::string site_url);

    const std::string &site_url() const { return m_site; }

    // Reads a stored token, if any. Cheap; no network.
    void load();

    bool                   signed_in() const;
    std::optional<Profile> profile() const;

    // The stored device token, empty when signed out. For the printer-update feeds, which take it
    // inside a FeedCredential bound to site_url() and attach it only to requests on that site;
    // nothing may send it anywhere without that check.
    std::string device_token() const { return bearer(); }

    // Step 1 of signing in.
    bool start_handoff(Handoff &out, std::string &error) const;
    // Step 3, polled every couple of seconds while the person confirms in their browser. On
    // SignedIn the token is stored and profile() is filled.
    CollectResult collect(const Handoff &handoff, std::string &error);

    // Confirms the stored token still works and refreshes profile(). SignedOut means the site
    // refused it (expired, revoked, signed out on the website) and it has been forgotten.
    RefreshResult refresh();

    // Forgets the token here and revokes it on the site (best effort; the local half always
    // happens).
    void sign_out();

    // Exchanges the stored token for a signed installer link. `route_url` is the manifest's
    // `unlayered_downloads[].url`, which must be on this account's site - the token is never sent
    // anywhere else. On a 401 the token is forgotten, as in refresh().
    bool fetch_download_link(const std::string &route_url, DownloadLink &out, unsigned &http_status,
                             std::string &error);

    // A short-lived token for a slicing service at `audience` (its origin), from POST
    // /api/slicer/slice-token {"audience": ...}. The device token itself never leaves the provider.
    // `expires_in_s` is the lifetime the provider quoted (`expires_in`, or `expiresAt` in ms since
    // the epoch), 0 when it quoted none. A provider refusal (401/402/403) fills `refusal` when
    // given. On a 401 the token is forgotten, as in refresh(). Only call this for a service that
    // may_receive_token() allows.
    bool fetch_service_token(const std::string &audience, std::string &token, long &expires_in_s,
                             unsigned &http_status, std::string &error, ServiceRefusal *refusal = nullptr);

    // Whether a token from this provider may be sent to the service at `service_url`: only when the
    // service is same-site with the provider (same scheme and registrable domain) or its origin is
    // listed in `extra_origins` (comma separated, AppConfig service_token_origins). Anywhere else a
    // request goes without Authorization and the service does its own authentication.
    bool may_receive_token(const std::string &service_url, const std::string &extra_origins) const;

    // False when no provider is configured: then nobody is ever signed in.
    bool has_provider() const { return !m_site.empty(); }
    // The provider's origin ("https://unlayered3d.com"), or empty without one.
    std::string provider_origin() const;

    // A one-time link that signs the application's web views in to this site as this account, from
    // POST /api/slicer/web-session, so a web tab hosted on the site never shows a sign-in form of its
    // own. `next` is the same-site path the web view should end up on. `path` comes back relative to
    // site_url() and stops working after one use or a minute. On a 401 the token is forgotten, as in
    // refresh().
    bool fetch_web_session_path(const std::string &next, std::string &path, unsigned &http_status,
                                std::string &error);

    // "https://unlayered3d.com" -> "unlayered3d.com", "http://localhost:3111" -> "localhost:3111";
    // used to keep the token on its own host and port.
    static std::string host_of(const std::string &url);

private:
    std::string bearer() const;
    void        store_token(const std::string &token, const Profile &profile);
    void        forget_token();
    std::string secret_service() const;

    const std::string      m_site;
    mutable std::mutex     m_mutex;
    std::string            m_token;
    std::optional<Profile> m_profile;
};

} // namespace Slic3r

#endif // slic3r_UnlayeredAccount_hpp_
