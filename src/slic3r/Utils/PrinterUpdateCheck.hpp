#ifndef slic3r_PrinterUpdateCheck_hpp_
#define slic3r_PrinterUpdateCheck_hpp_

#include <functional>
#include <string>
#include <utility>

namespace Slic3r {

class Http;

// The decisions behind "Printer update available", kept free of wx and of the network so they can
// be tested on their own. The GUI half - reading the board, asking the feeds, the notification and
// the sequenced dialogs - is GUI/PrinterUpdateOffer.

// ------------------------------------------------------------------------------------------------
// Who may see the account token
// ------------------------------------------------------------------------------------------------

// True when a request to `url` may carry the Unlayered account's credential, which belongs to
// `site_url` (e.g. "https://unlayered3d.com"). Same scheme, same host, same port - nothing else. In
// particular never api.github.com: "github_api_base" can point the feeds anywhere, and a release's
// browser_download_url can name any host, so the check is made on every request URL, not once per
// feed. An https site never lets the token travel over plain http.
bool feed_token_allowed(const std::string &url, const std::string &site_url);

// A credential for the printer-update feeds together with the one site it may be sent to.
struct FeedCredential
{
    std::string token;
    std::string site_url;

    bool empty() const { return token.empty(); }
    // The token when `url` is on site_url (feed_token_allowed), otherwise empty.
    std::string token_for(const std::string &url) const;
};

// Adds "Authorization: Bearer" to a request for `url` when the credential may go there, and then
// also turns certificate verification on (the shared Http client defaults it off), as
// UnlayeredAccount does for every request that carries the token. Otherwise leaves it untouched.
// Returns whether the credential was attached.
bool apply_feed_credential(Http &http, const std::string &url, const FeedCredential &credential);

// ------------------------------------------------------------------------------------------------
// Is the feed's version newer than what the printer has?
// ------------------------------------------------------------------------------------------------

// For the automatic offer only - the Firmware dialog still compares exact strings and allows a
// reinstall. Ordered by the "+unlayered.N" fork number, the fork's one monotonic counter: semver
// ignores build metadata and would call every fork build equal. Stock RRF is never offered over a
// fork build, a fork build is always offered over stock RRF, and two stock versions use semver.
// A rebuild that keeps the version string is not offered automatically (the board's firmwareDate
// would be the only difference, and a bench build would then be "updated" back to the release).
bool firmware_is_newer(const std::string &running, const std::string &available);

// Configuration versions are free-form ("2026.10-1"), so they are compared naturally: runs of
// digits as numbers, everything else as text. A card that has never been written by this slicer
// (installed_known false) has no version, and any published configuration is offered for it.
bool config_is_newer(bool installed_known, const std::string &installed, const std::string &available);

// <0, 0, >0 like strcmp, with digit runs compared by value: "2026.9-1" < "2026.10-1".
int natural_version_compare(const std::string &a, const std::string &b);

// ------------------------------------------------------------------------------------------------
// "Skip this version", remembered per machine
// ------------------------------------------------------------------------------------------------

// Stored in the application config under one section per component, keyed by the board's
// uniqueId - the only per-machine identity a Duet has (several machines answer "Unlayered 5X").
// Behind two callbacks so the application can put it on AppConfig and a test on a std::map.
class PrinterUpdateSkips
{
public:
    using Getter = std::function<std::string(const std::string &section, const std::string &key)>;
    using Setter = std::function<void(const std::string &section, const std::string &key, const std::string &value)>;

    static constexpr const char *FIRMWARE_SECTION = "printer_update_skip_firmware";
    static constexpr const char *CONFIG_SECTION   = "printer_update_skip_config";

    PrinterUpdateSkips(Getter get, Setter set) : m_get(std::move(get)), m_set(std::move(set)) {}

    // An empty version leaves that component's remembered skip alone.
    void skip(const std::string &board_id, const std::string &firmware, const std::string &config);

    bool firmware_skipped(const std::string &board_id, const std::string &version) const;
    bool config_skipped(const std::string &board_id, const std::string &version) const;

    // The config store is a flat key/value map; this keeps an id from breaking it.
    static std::string key_for(const std::string &board_id);

private:
    Getter m_get;
    Setter m_set;
};

// ------------------------------------------------------------------------------------------------
// The whole decision
// ------------------------------------------------------------------------------------------------

struct PrinterUpdateFacts
{
    std::string board_id;            // boards[0].uniqueId, or the host when the board reports none
    std::string running_firmware;    // boards[0].firmwareVersion
    bool        config_known = false; // 0:/sys/unlayered-sys.json was there and parsed
    std::string installed_config;
    std::string available_firmware;  // empty when the feed was not asked or did not answer
    std::string available_config;    // empty when not signed in, not entitled, or no answer
};

struct PrinterUpdateDecision
{
    bool firmware = false;
    bool config   = false;
    bool any() const { return firmware || config; }
};

PrinterUpdateDecision decide_printer_update(const PrinterUpdateFacts &facts, const PrinterUpdateSkips &skips);

// ------------------------------------------------------------------------------------------------
// The setup wizard's "Check for updates" (guide 21update)
// ------------------------------------------------------------------------------------------------

// What one asked-for check found, for the wizard page to say in words. Skips are not consulted:
// the person pressed the button, so a version they once skipped is still reported (and, after the
// wizard, offered once more - see PrinterUpdateOffer's after-setup key).
enum class PrinterUpdateCheckOutcome
{
    Unreachable, // the printer did not answer
    NoFeed,      // the printer answered, but no feed returned a version (offline, server down)
    UpToDate,    // nothing newer than what the printer has (of what could be asked)
    Available,   // newer firmware, newer configuration, or both
};

PrinterUpdateCheckOutcome classify_printer_update_check(bool reachable, const PrinterUpdateFacts &facts);

// A skip store that remembers nothing: decide_printer_update() with it ignores "Skip this version".
PrinterUpdateSkips no_printer_update_skips();

// Whether the outcome should leave the after-setup check pending, so the first check of that
// printer after the wizard runs regardless of skips and says what it found: everything except
// "up to date", which the page already said.
bool printer_update_check_after_setup(PrinterUpdateCheckOutcome outcome);

} // namespace Slic3r

#endif // slic3r_PrinterUpdateCheck_hpp_
