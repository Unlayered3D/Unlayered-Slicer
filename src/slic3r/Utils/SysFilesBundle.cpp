#include "SysFilesBundle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/Semver.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "DuetRrfClient.hpp"
#include "FirmwareBundle.hpp" // sha256_hex
#include "Http.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

const char *SYS_MARKER_PATH = "sys/unlayered-sys.json";

// A configuration file is a few kilobytes; a height map is a few tens. These limits exist so that
// pointing the updater at the wrong zip fails immediately instead of pushing megabytes at a board
// that has to write them one FAT sector at a time.
static const uint64_t MAX_FILE_BYTES    = 4ull * 1024 * 1024;
static const uint64_t MAX_PACKAGE_BYTES = 32ull * 1024 * 1024;

const std::vector<std::string> &sys_allowed_roots()
{
    static const std::vector<std::string> roots = {"sys", "macros", "filaments"};
    return roots;
}

static bool is_allowed_root(const std::string &component)
{
    for (const auto &r : sys_allowed_roots())
        if (boost::iequals(r, component))
            return true;
    return false;
}

static std::string canonical_root(const std::string &component)
{
    for (const auto &r : sys_allowed_roots())
        if (boost::iequals(r, component))
            return r;
    return component;
}

// Separators, "./" and duplicate slashes only. Nothing here decides where a file goes.
static std::string normalize_separators(const std::string &in)
{
    std::string s = in;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (boost::starts_with(s, "./"))
        s = s.substr(2);
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '/' && !out.empty() && out.back() == '/')
            continue;
        out.push_back(c);
    }
    return out;
}

static std::vector<std::string> split_path(const std::string &path)
{
    std::vector<std::string> parts;
    boost::split(parts, path, boost::is_any_of("/"));
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const std::string &p) { return p.empty() || p == "."; }),
                parts.end());
    return parts;
}

bool sys_map_path(const std::string &relative, std::string &card_path_out, std::string &reason_out)
{
    const std::string norm = normalize_separators(relative);

    if (norm.empty()) {
        reason_out = "the path is empty";
        return false;
    }
    // Anything that could resolve outside the card is refused outright rather than sanitised: a
    // package that contains such a path is wrong, and quietly rewriting it hides that.
    if (norm.front() == '/' || (norm.size() > 1 && norm[1] == ':')) {
        reason_out = "absolute paths are not allowed in a package";
        return false;
    }

    std::vector<std::string> parts = split_path(norm);
    if (parts.empty()) {
        reason_out = "the path is empty";
        return false;
    }
    for (const auto &p : parts) {
        if (p == "..") {
            reason_out = "the path leaves the package";
            return false;
        }
    }

    // A path already rooted at one of the destinations keeps it; anything else is configuration
    // for the machine, which is what 0:/sys is.
    if (!is_allowed_root(parts.front()))
        parts.insert(parts.begin(), "sys");
    parts.front() = canonical_root(parts.front());

    if (parts.size() < 2) {
        reason_out = "a package cannot write a directory itself";
        return false;
    }

    card_path_out = boost::join(parts, "/");
    return true;
}

bool sys_is_ignored_file(const std::string &relative_path)
{
    const std::string        norm  = normalize_separators(relative_path);
    const std::vector<std::string> parts = split_path(norm);
    if (parts.empty())
        return true;

    for (const auto &p : parts)
        if (boost::iequals(p, ".git") || boost::iequals(p, ".svn") || boost::iequals(p, "__MACOSX"))
            return true;

    const std::string name  = parts.back();
    const std::string lower = boost::algorithm::to_lower_copy(name);

    if (lower == "thumbs.db" || lower == ".ds_store" || lower == "desktop.ini")
        return true;
    if (!name.empty() && (name.front() == '~' || name.front() == '.'))
        return true;
    for (const char *ext : {".bak", ".part", ".tmp", ".old", ".orig", ".swp"})
        if (boost::iends_with(lower, ext))
            return true;

    // The marker is written by the installer itself; carrying one in from a card image would make
    // the printer claim to be running whatever that card last had.
    if (boost::iequals(norm, SYS_MARKER_PATH) || boost::iequals(name, "unlayered-sys.json"))
        return true;

    return false;
}

const std::vector<std::string> &sys_machine_specific_names()
{
    // Keep in step with MACHINE_SPECIFIC in tools/make_sys_bundle.py (verify_sys_updater.py checks).
    static const std::vector<std::string> names = {
        // Calibration measured on one printer, and the state DWC and RRF keep about it.
        "heightmap.csv", "probePoints.csv", "config-override.g", "dwc-settings.json", "dwc-cache.json",
        "resurrect.g", "resurrect-prologue.g",
        // What a printer accumulates about itself: the odometer and the logs.
        "printerstats.json", "eventlog.txt", "piezolog.csv",
        // Written by the Duet configurator and by DWC to describe one installation.
        "configtool.json", "dwc-plugins.json",
        // The printer's own overlays, written on the machine by the panels, the setup wizard and
        // calibration: plate offsets, name, network, display, probe polarity, measured deflection.
        "plates.g", "machinename.g", "panelnet.g", "display.g", "setupdone.g", "piezopol.g", "callast.g", "cdefl.g",
        "filaments.csv", "panelfw.g", "fwflash.g", "panelimage.g",
        // Setup progress and the measured Z-stop height, both erased by sys/factoryreset.g.
        "setupskip.g", "setupprog.g", "zstall.g",
    };
    return names;
}

bool sys_is_machine_specific(const std::string &card_path)
{
    const std::vector<std::string> parts = split_path(normalize_separators(card_path));
    if (parts.empty())
        return false;
    for (const auto &n : sys_machine_specific_names())
        if (boost::iequals(parts.back(), n))
            return true;
    return false;
}

// Top-level folders of an Unlayered SD card image that are not printer configuration: the
// firmware updater's images, print files, the web interface and the 12864 display's menus.
static const char *const SD_CARD_ONLY_DIRS[] = {"firmware", "gcodes", "www", "menu"};

// The README / START HERE notes a card image carries at its root.
static bool is_sd_card_note(const std::string &root_file_name)
{
    const std::string lower = boost::algorithm::to_lower_copy(root_file_name);
    return boost::starts_with(lower, "readme") || boost::starts_with(lower, "start here") ||
           boost::starts_with(lower, "start_here") || boost::starts_with(lower, "start-here");
}

bool sys_looks_like_full_sd_card(const std::vector<std::string> &paths)
{
    bool has_config = false, has_card_only = false;
    for (const auto &path : paths) {
        const std::vector<std::string> parts = split_path(normalize_separators(path));
        if (parts.empty())
            continue;
        if (parts.size() == 1) {
            has_card_only = has_card_only || is_sd_card_note(parts.front());
            continue;
        }
        if (boost::iequals(parts.front(), "sys") || boost::iequals(parts.front(), "macros"))
            has_config = true;
        for (const char *dir : SD_CARD_ONLY_DIRS)
            if (boost::iequals(parts.front(), dir))
                has_card_only = true;
    }
    return has_config && has_card_only;
}

bool sys_is_card_kept_config(const std::string &card_path)
{
    // Keep in step with what the configuration release stage leaves out beyond
    // sys_machine_specific_names() (see the release notes: "never touches config.g").
    return boost::iequals(normalize_separators(card_path), "sys/config.g");
}

bool sys_is_config_folder_path(const std::string &relative_path)
{
    const std::vector<std::string> parts = split_path(normalize_separators(relative_path));
    return parts.size() >= 2 && is_allowed_root(parts.front());
}

// -----------------------------------------------------------------------------------------------
// Package
// -----------------------------------------------------------------------------------------------

uint64_t SysFilePackage::total_bytes() const
{
    uint64_t n = 0;
    for (const auto &f : files)
        n += f.data.size();
    return n;
}

const SysFile *SysFilePackage::find(const std::string &path) const
{
    auto it = std::find_if(files.begin(), files.end(),
                           [&path](const SysFile &f) { return boost::iequals(f.path, path); });
    return it == files.end() ? nullptr : &*it;
}

bool parse_sys_manifest(const std::string &json, SysFilePackage &pkg_out, std::string &error_out)
{
    try {
        pt::ptree root;
        std::istringstream iss(json);
        pt::read_json(iss, root);

        pkg_out.version = root.get<std::string>("version", "");
        // "buildDate" is what the firmware manifest calls it; accept both so one generator can
        // produce either without a surprise.
        pkg_out.date    = root.get<std::string>("date", root.get<std::string>("buildDate", ""));
        pkg_out.machine = root.get<std::string>("machine", "");
        pkg_out.notes   = root.get<std::string>("notes", "");
        pkg_out.min_firmware = boost::algorithm::trim_copy(root.get<std::string>("minFirmware", ""));

        if (pkg_out.version.empty()) {
            error_out = "The manifest does not declare a version";
            return false;
        }

        pkg_out.files.clear();
        if (auto files = root.get_child_optional("files")) {
            for (const auto &kv : *files) {
                // Accept both { "path": "...", "sha256": "..." } and a bare "sys/config.g".
                const std::string declared = kv.second.get<std::string>("path", kv.second.data());
                if (declared.empty()) {
                    error_out = "A manifest entry has no path";
                    return false;
                }
                SysFile f;
                std::string reason;
                if (!sys_map_path(declared, f.path, reason)) {
                    error_out = (boost::format("The manifest lists %1%, which cannot be installed: %2%")
                                 % declared % reason).str();
                    return false;
                }
                f.sha256 = boost::algorithm::to_lower_copy(kv.second.get<std::string>("sha256", ""));
                pkg_out.files.push_back(std::move(f));
            }
        }
        return true;
    } catch (const std::exception &e) {
        error_out = (boost::format("Could not parse the manifest: %1%") % e.what()).str();
        return false;
    }
}

int unlayered_fork_number(const std::string &firmware_version)
{
    static const std::string tag = "+unlayered.";
    const size_t at = firmware_version.find(tag);
    if (at == std::string::npos)
        return -1;
    size_t i = at + tag.size();
    if (i >= firmware_version.size() || !std::isdigit(static_cast<unsigned char>(firmware_version[i])))
        return -1;
    long n = 0;
    for (; i < firmware_version.size() && std::isdigit(static_cast<unsigned char>(firmware_version[i])); ++i)
        n = std::min<long>(n * 10 + (firmware_version[i] - '0'), 1000000);
    return int(n);
}

// True when a G-code line, comment already removed, is an M558 selecting probe type 12. Parameters
// are whitespace separated in every file we ship; P{expr} is not recognised, and does not occur.
static bool is_m558_p12(const std::string &code)
{
    std::vector<std::string> words;
    boost::split(words, code, boost::is_any_of(" \t"), boost::token_compress_on);
    if (words.empty() || !boost::iequals(words.front(), "M558"))
        return false;
    for (size_t i = 1; i < words.size(); ++i)
        if (boost::iequals(words[i], "P12"))
            return true;
    return false;
}

std::string sys_required_firmware(const SysFilePackage &pkg, std::string *reason_out)
{
    std::string required = pkg.min_firmware;
    std::string reason   = required.empty() ? std::string() : "the package declares it";

    // What the files say and use, whatever the manifest says, keeping the highest:
    //  - sys/config.g's `var fwNeed = N`, the number its own first-boot firmware install tests;
    //  - M558 P12 anywhere, the differential Z probe, from +unlayered.12.
    // tools/make_sys_bundle.py applies the same two rules; this is where the next firmware-only
    // command goes when the configuration starts relying on one.
    static const int P12_FORK = 12;
    int              best     = unlayered_fork_number(required);
    for (const auto &f : pkg.files) {
        // Every file here is G-code; skip anything binary rather than scan it.
        if (std::find(f.data.begin(), f.data.end(), uint8_t(0)) != f.data.end())
            continue;
        const bool         is_config = boost::iequals(f.path, "sys/config.g");
        std::istringstream in(std::string(f.data.begin(), f.data.end()));
        std::string        line;
        while (std::getline(in, line)) {
            const size_t semi = line.find(';');
            if (semi != std::string::npos)
                line.erase(semi);
            boost::algorithm::trim(line);
            if (is_config && boost::istarts_with(line, "var fwNeed")) {
                const size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    const std::string num = boost::algorithm::trim_copy(line.substr(eq + 1));
                    if (!num.empty() && std::all_of(num.begin(), num.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
                        const int n = std::min(std::atoi(num.c_str()), 1000000);
                        if (n > best) {
                            best     = n;
                            required = "+unlayered." + std::to_string(n);
                            reason   = "sys/config.g declares fwNeed";
                        }
                    }
                }
            } else if (best < P12_FORK && is_m558_p12(line)) {
                best     = P12_FORK;
                required = "+unlayered." + std::to_string(P12_FORK);
                reason   = f.path + " uses M558 P12, the differential Z probe";
            }
        }
    }

    if (reason_out)
        *reason_out = reason;
    return required;
}

bool sys_firmware_satisfies(const std::string &running, const std::string &required)
{
    if (required.empty())
        return true;
    const int need = unlayered_fork_number(required);
    if (need >= 0)
        // Only an Unlayered build has the fork's commands, so a stock board is always too old.
        return unlayered_fork_number(running) >= need;
    // A plain RRF version: semver order, ignoring build metadata, is correct here.
    const auto r = Semver::parse(running);
    const auto q = Semver::parse(required);
    if (!r || !q)
        return true; // cannot compare; not a reason to refuse
    return *r >= *q;
}

bool validate_sys_package(const SysFilePackage &pkg, std::string &error_out)
{
    if (pkg.files.empty()) {
        error_out = "The package contains no files to install";
        return false;
    }

    uint64_t total = 0;
    for (const auto &f : pkg.files) {
        std::string reason;
        std::string mapped;
        if (!sys_map_path(f.path, mapped, reason) || !boost::iequals(mapped, f.path)) {
            error_out = (boost::format("%1% is not a path this can write") % f.path).str();
            return false;
        }
        if (f.data.empty()) {
            error_out = (boost::format("%1% was not read") % f.path).str();
            return false;
        }
        if (f.data.size() > MAX_FILE_BYTES) {
            error_out = (boost::format("%1% is %2% bytes, which is far larger than a configuration "
                                       "file; this does not look like a configuration package")
                         % f.path % f.data.size()).str();
            return false;
        }
        if (!f.sha256.empty()) {
            const std::string actual = sha256_hex(f.data);
            if (!boost::iequals(actual, f.sha256)) {
                error_out = (boost::format("Checksum mismatch for %1%: expected %2%, got %3%")
                             % f.path % f.sha256 % actual).str();
                return false;
            }
        }
        total += f.data.size();
    }

    if (total > MAX_PACKAGE_BYTES) {
        error_out = (boost::format("The package is %1% bytes, which is too large to be printer "
                                   "configuration") % total).str();
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------------------------
// Marker
// -----------------------------------------------------------------------------------------------

static std::string local_timestamp()
{
    char       buffer[32];
    const auto t  = std::time(nullptr);
    const auto tm = *std::localtime(&t);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buffer);
}

std::string make_sys_marker_json(const SysFilePackage &pkg, const std::vector<std::string> &installed_paths)
{
    pt::ptree root;
    root.put("version", pkg.version);
    root.put("date", pkg.date);
    root.put("machine", pkg.machine);
    root.put("origin", pkg.origin);
    root.put("installedAt", local_timestamp());
    root.put("fileCount", installed_paths.size());

    pt::ptree files;
    for (const auto &p : installed_paths) {
        pt::ptree entry;
        entry.put("", p);
        files.push_back(std::make_pair("", entry));
    }
    root.add_child("files", files);

    std::ostringstream oss;
    pt::write_json(oss, root, /* pretty */ true);
    return oss.str();
}

bool parse_sys_marker(const std::string &json, SysFilesInstalled &out)
{
    try {
        pt::ptree root;
        std::istringstream iss(json);
        pt::read_json(iss, root);

        out.version      = root.get<std::string>("version", "");
        out.date         = root.get<std::string>("date", "");
        out.machine      = root.get<std::string>("machine", "");
        out.origin       = root.get<std::string>("origin", "");
        out.installed_at = root.get<std::string>("installedAt", "");
        out.file_count   = root.get<int>("fileCount", 0);
        out.known        = !out.version.empty() || !out.installed_at.empty();
        return out.known;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "SysFiles: could not parse the marker on the card: " << e.what();
        return false;
    }
}

// -----------------------------------------------------------------------------------------------
// Assembling a package out of raw (path, bytes) pairs
// -----------------------------------------------------------------------------------------------

namespace {

struct RawEntry
{
    std::string          path; // relative to the package root
    std::vector<uint8_t> data;
};

// The directory, if any, that wraps the whole package - the "repo-1.2/" a GitHub archive adds, or
// the folder somebody zipped by right-clicking it - as a prefix ending in '/'. A wrapper that is
// itself a destination (sys/) is not one, because that one carries meaning. Paths are normalised.
std::string wrapper_prefix(const std::vector<std::string> &paths)
{
    std::string prefix;
    for (int pass = 0; pass < 3 && !paths.empty(); ++pass) {
        std::string common;
        for (const auto &full : paths) {
            const std::string p     = full.substr(prefix.size());
            const auto        slash = p.find('/');
            if (slash == std::string::npos)
                return prefix; // something sits at this level, so there is no wrapper
            const std::string first = p.substr(0, slash);
            if (common.empty())
                common = first;
            else if (!boost::iequals(common, first))
                return prefix;
        }
        if (common.empty() || is_allowed_root(common))
            return prefix;
        prefix += paths.front().substr(prefix.size(), common.size() + 1);
    }
    return prefix;
}

// Removes the wrapper directory (see wrapper_prefix) from every entry.
void strip_wrapper_dirs(std::vector<RawEntry> &entries)
{
    std::vector<std::string> paths;
    paths.reserve(entries.size());
    for (const auto &e : entries)
        paths.push_back(e.path);
    const std::string prefix = wrapper_prefix(paths);
    if (prefix.empty())
        return;
    BOOST_LOG_TRIVIAL(info) << "SysFiles: stripping wrapper directory " << prefix;
    for (auto &e : entries)
        e.path = e.path.substr(prefix.size());
}

// What was decided about a whole SD card image before any payload was read: that it is one, and
// what was left on the card because of it.
struct CardFilter
{
    bool                                             full_sd_card = false;
    std::vector<std::pair<std::string, std::string>> skipped;
};

// For a package that is a whole SD card image (sys_looks_like_full_sd_card), marks every path
// outside the configuration folders as not to be read, and reports them one line per top-level
// folder rather than one per print file and web page. Any other package is left exactly as it
// was; in particular a loose file in an ordinary package still lands in sys/. `paths` are
// normalised and wrapper-free. Returns one flag per path: true = read it.
std::vector<bool> filter_full_sd_card(const std::vector<std::string> &paths, CardFilter &filter)
{
    std::vector<bool> keep(paths.size(), true);
    filter.full_sd_card = sys_looks_like_full_sd_card(paths);
    if (!filter.full_sd_card)
        return keep;

    std::vector<std::pair<std::string, int>> folders; // top-level folder -> files left out
    for (size_t i = 0; i < paths.size(); ++i) {
        if (sys_is_config_folder_path(paths[i]))
            continue;
        keep[i] = false;
        if (sys_is_ignored_file(paths[i]))
            continue; // __MACOSX, .DS_Store and the like: not worth a line
        const std::vector<std::string> parts = split_path(paths[i]);
        if (parts.size() < 2) {
            filter.skipped.emplace_back(paths[i], "part of the SD card, not printer configuration");
            continue;
        }
        auto it = std::find_if(folders.begin(), folders.end(),
                               [&parts](const std::pair<std::string, int> &f) { return boost::iequals(f.first, parts.front()); });
        if (it == folders.end())
            folders.emplace_back(parts.front(), 1);
        else
            ++it->second;
    }
    for (const auto &f : folders) {
        const bool firmware = boost::iequals(f.first, "firmware");
        filter.skipped.emplace_back(
            f.first + "/",
            (boost::format("part of the SD card, not printer configuration (%1% file%2%)%3%") % f.second
             % (f.second == 1 ? "" : "s") % (firmware ? "; update firmware with the Firmware button" : "")).str());
    }
    BOOST_LOG_TRIVIAL(info) << "SysFiles: a whole SD card image; only its configuration folders are read";
    return keep;
}

bool build_package(std::vector<RawEntry> entries,
                   const std::string    &origin,
                   const CardFilter     &filter,
                   SysFilePackage       &pkg_out,
                   std::string          &error_out)
{
    for (auto &e : entries)
        e.path = normalize_separators(e.path);
    strip_wrapper_dirs(entries);

    SysFilePackage pkg;
    pkg.origin            = origin;
    pkg.from_full_sd_card = filter.full_sd_card;
    pkg.skipped           = filter.skipped;

    // A manifest at the package root, if there is one, is authoritative about what gets installed.
    bool have_manifest = false;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!boost::iequals(it->path, "manifest.json") && !boost::iequals(it->path, "sys-manifest.json"))
            continue;
        const std::string json(it->data.begin(), it->data.end());
        if (!parse_sys_manifest(json, pkg, error_out))
            return false;
        pkg.origin   = origin;
        // parse_sys_manifest() does not touch these today; set again so that never matters.
        pkg.from_full_sd_card = filter.full_sd_card;
        pkg.skipped           = filter.skipped;
        have_manifest = true;
        entries.erase(it);
        break;
    }

    // A file that belongs to one printer is never installed, whatever the manifest says about it.
    // Dropped from the declared list here, so the declared-but-missing check below does not read it
    // as a broken package. Whether the manifest enumerated files is decided BEFORE that, so a list
    // left empty by it can never fall through to "install everything that was found".
    static const char *MACHINE_SPECIFIC_REASON =
        "belongs to one printer (its calibration, name, network, setup or logs) and is never replaced";
    // A whole SD card always carries the ship image's sys/config.g (ship header, DHCP, factory
    // geometry). Installing it would replace this printer's tuned one with the generic one, so from
    // a card it is kept - as the configuration releases do, whose stage leaves config.g out.
    static const char *CARD_CONFIG_REASON = "this printer's own setup, kept";
    const bool         full_card          = filter.full_sd_card;
    auto protected_reason = [full_card](const std::string &card_path) -> const char * {
        if (sys_is_machine_specific(card_path))
            return MACHINE_SPECIFIC_REASON;
        if (full_card && sys_is_card_kept_config(card_path))
            return CARD_CONFIG_REASON;
        return nullptr;
    };
    const bool manifest_lists_files = have_manifest && !pkg.files.empty();
    if (manifest_lists_files) {
        auto keep = std::stable_partition(pkg.files.begin(), pkg.files.end(),
                                          [&](const SysFile &f) { return protected_reason(f.path) == nullptr; });
        for (auto it = keep; it != pkg.files.end(); ++it)
            pkg.skipped.emplace_back(it->path, protected_reason(it->path));
        pkg.files.erase(keep, pkg.files.end());
    }

    // Everything the package actually carries, keyed by where it would land on the card.
    std::vector<SysFile> found;
    for (auto &e : entries) {
        if (sys_is_ignored_file(e.path)) {
            pkg.skipped.emplace_back(e.path, "not a configuration file");
            continue;
        }
        std::string card_path, reason;
        if (!sys_map_path(e.path, card_path, reason)) {
            pkg.skipped.emplace_back(e.path, reason);
            continue;
        }
        if (const char *why = protected_reason(card_path)) {
            // Reported once: a manifest that declared it has already been answered above.
            const bool reported = std::any_of(pkg.skipped.begin(), pkg.skipped.end(),
                                              [&card_path](const auto &s) { return boost::iequals(s.first, card_path); });
            if (!reported)
                pkg.skipped.emplace_back(e.path, why);
            continue;
        }
        if (e.data.empty()) {
            // rr_upload has no way to create a zero length file, and an empty config macro is
            // almost always an accident rather than a deliberate no-op.
            pkg.skipped.emplace_back(e.path, "the file is empty");
            continue;
        }
        SysFile f;
        f.path   = card_path;
        f.origin = e.path;
        f.data   = std::move(e.data);
        found.push_back(std::move(f));
    }

    if (manifest_lists_files) {
        // Take the declared list, in the declared order, and fill it from what was found.
        std::vector<SysFile> resolved;
        for (auto &declared : pkg.files) {
            auto it = std::find_if(found.begin(), found.end(),
                                   [&declared](const SysFile &f) { return boost::iequals(f.path, declared.path); });
            if (it == found.end()) {
                error_out = (boost::format("The manifest lists %1%, but the package does not contain it")
                             % declared.path).str();
                return false;
            }
            it->sha256 = declared.sha256;
            resolved.push_back(std::move(*it));
            found.erase(it);
        }
        for (auto &leftover : found)
            pkg.skipped.emplace_back(leftover.origin, "not listed in manifest.json");
        pkg.files = std::move(resolved);
    } else {
        // No manifest, or one that does not enumerate files: install everything that was found.
        std::sort(found.begin(), found.end(),
                  [](const SysFile &a, const SysFile &b) { return a.path < b.path; });
        pkg.files = std::move(found);
    }

    if (pkg.files.empty()) {
        error_out = (boost::format("%1% contains no configuration files") % origin).str();
        return false;
    }

    pkg_out = std::move(pkg);
    return true;
}

bool read_zip_entries(mz_zip_archive       &arch,
                      std::vector<RawEntry> &out,
                      CardFilter            &filter,
                      SysProgressFn          progress,
                      std::string           &error_out)
{
    const mz_uint count = mz_zip_reader_get_num_files(&arch);
    uint64_t      total = 0;

    // Names first: whether this is a whole SD card image decides which entries are read at all, so
    // its print files and firmware stay in the archive instead of tripping the size limits below.
    std::vector<mz_uint>     indices;
    std::vector<std::string> names;
    for (mz_uint i = 0; i < count; ++i) {
        if (mz_zip_reader_is_file_a_directory(&arch, i))
            continue;
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&arch, i, &stat)) {
            error_out = "The archive could not be read";
            return false;
        }
        indices.push_back(i);
        names.push_back(normalize_separators(stat.m_filename));
    }
    const std::string        prefix = wrapper_prefix(names);
    std::vector<std::string> unwrapped;
    unwrapped.reserve(names.size());
    for (const auto &n : names)
        unwrapped.push_back(n.substr(prefix.size()));
    const std::vector<bool> keep = filter_full_sd_card(unwrapped, filter);

    for (size_t k = 0; k < indices.size(); ++k) {
        if (!keep[k])
            continue;
        const mz_uint i = indices[k];

        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&arch, i, &stat)) {
            error_out = "The archive could not be read";
            return false;
        }
        if (stat.m_uncomp_size > MAX_FILE_BYTES) {
            error_out = (boost::format("%1% in the archive is %2% bytes, which is far too large to be "
                                       "a configuration file") % stat.m_filename % uint64_t(stat.m_uncomp_size)).str();
            return false;
        }
        total += stat.m_uncomp_size;
        if (total > MAX_PACKAGE_BYTES) {
            error_out = "The archive is too large to be printer configuration";
            return false;
        }

        RawEntry entry;
        entry.path = stat.m_filename;
        entry.data.resize(size_t(stat.m_uncomp_size));
        if (stat.m_uncomp_size > 0 &&
            !mz_zip_reader_extract_to_mem(&arch, i, entry.data.data(), entry.data.size(), 0)) {
            error_out = (boost::format("Could not extract %1% from the archive") % stat.m_filename).str();
            return false;
        }

        out.push_back(std::move(entry));
        if (progress && !progress("Reading the archive", i + 1, count)) {
            error_out = "Cancelled";
            return false;
        }
    }

    if (out.empty()) {
        error_out = filter.full_sd_card ? "The SD card image has no files in sys or macros" : "The archive is empty";
        return false;
    }
    return true;
}

} // namespace

// -----------------------------------------------------------------------------------------------
// LocalSysFileSource
// -----------------------------------------------------------------------------------------------

LocalSysFileSource::LocalSysFileSource(std::string path) : m_path(std::move(path)) {}

std::string LocalSysFileSource::description() const
{
    return (boost::format("Local files (%1%)") % m_path).str();
}

bool LocalSysFileSource::fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out)
{
    boost::system::error_code ec;
    const fs::path            p = fs::path(m_path);

    if (!fs::exists(p, ec)) {
        error_out = (boost::format("%1% does not exist") % m_path).str();
        return false;
    }
    const bool ok = fs::is_directory(p, ec) ? fetch_from_directory(pkg_out, progress, error_out)
                                            : fetch_from_zip(pkg_out, progress, error_out);
    if (!ok)
        return false;

    // Nothing about a hand-picked folder states a version, so the caller treats it the way the
    // firmware updater treats a hand-picked image: always offered, never called up to date.
    if (pkg_out.notes.empty())
        pkg_out.notes = (boost::format("Configuration files from %1%.") % m_path).str();
    return true;
}

bool LocalSysFileSource::fetch_from_zip(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out)
{
    MZ_Archive zip;
    if (!open_zip_reader(&zip.arch, m_path)) {
        error_out = (boost::format("%1% could not be opened as a zip archive") % m_path).str();
        return false;
    }

    std::vector<RawEntry> entries;
    CardFilter            filter;
    const bool            read_ok = read_zip_entries(zip.arch, entries, filter, progress, error_out);
    close_zip_reader(&zip.arch);
    if (!read_ok)
        return false;

    return build_package(std::move(entries), description(), filter, pkg_out, error_out);
}

bool LocalSysFileSource::fetch_from_directory(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out)
{
    const fs::path root = fs::path(m_path);
    // Picking the machine's sys folder itself has to mean "these are the sys files", so the folder
    // name is kept as the root when it names a destination.
    const std::string root_name   = root.filename().string();
    const std::string path_prefix = is_allowed_root(root_name) ? canonical_root(root_name) + "/" : std::string();

    // Names first, as for a zip: the root of a whole SD card (or a copy of one) is read for its
    // configuration folders only, so its print files never count against the size limits.
    std::vector<fs::path>    found_paths;
    std::vector<std::string> found_rel;
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            error_out = (boost::format("Could not read %1%: %2%") % m_path % ec.message()).str();
            return false;
        }
        if (fs::is_directory(it->path(), ec))
            continue;

        // Relative to the folder the user picked.
        std::string rel = it->path().string().substr(root.string().size());
        while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\'))
            rel = rel.substr(1);
        found_paths.push_back(it->path());
        found_rel.push_back(path_prefix + normalize_separators(rel));
    }
    const std::string        prefix = wrapper_prefix(found_rel);
    std::vector<std::string> unwrapped;
    unwrapped.reserve(found_rel.size());
    for (const auto &r : found_rel)
        unwrapped.push_back(r.substr(prefix.size()));
    CardFilter              filter;
    const std::vector<bool> keep = filter_full_sd_card(unwrapped, filter);

    std::vector<RawEntry> entries;
    uint64_t              total = 0;
    for (size_t k = 0; k < found_paths.size(); ++k) {
        if (!keep[k])
            continue;
        RawEntry entry;
        entry.path = found_rel[k];

        const uint64_t size = uint64_t(fs::file_size(found_paths[k], ec));
        if (ec) {
            error_out = (boost::format("Could not read %1%: %2%") % found_paths[k].string() % ec.message()).str();
            return false;
        }
        if (size > MAX_FILE_BYTES) {
            // Not fatal for a folder: the user may simply have a stray archive sitting in it.
            BOOST_LOG_TRIVIAL(info) << "SysFiles: ignoring oversized file " << found_paths[k].string();
            continue;
        }
        total += size;
        if (total > MAX_PACKAGE_BYTES) {
            error_out = (boost::format("%1% holds more data than printer configuration ever should; "
                                       "check that it is the right folder") % m_path).str();
            return false;
        }

        std::string read_err;
        if (!DuetRrfClient::read_file_binary(found_paths[k].string(), entry.data, read_err)) {
            error_out = read_err;
            return false;
        }
        entries.push_back(std::move(entry));

        if (progress && !progress("Reading the folder", entries.size(), entries.size())) {
            error_out = "Cancelled";
            return false;
        }
    }

    if (entries.empty()) {
        error_out = filter.full_sd_card ? (boost::format("%1% has no files in sys or macros") % m_path).str()
                                        : (boost::format("%1% is empty") % m_path).str();
        return false;
    }
    return build_package(std::move(entries), description(), filter, pkg_out, error_out);
}

// -----------------------------------------------------------------------------------------------
// GitHubSysFileSource
// -----------------------------------------------------------------------------------------------

GitHubSysFileSource::GitHubSysFileSource(Options opts) : m_opts(std::move(opts)) {}

std::string GitHubSysFileSource::description() const
{
    return (boost::format("GitHub release %1%/%2% (%3%)")
            % m_opts.owner % m_opts.repo % (m_opts.tag.empty() ? std::string("latest") : m_opts.tag)).str();
}

bool GitHubSysFileSource::fetch_release(Release &out, std::string &error_out)
{
    m_last_status = 0;

    // /releases/latest skips pre-releases, so list them and take the newest published one instead.
    std::string url;
    if (!m_opts.tag.empty())
        url = (boost::format("%1%/repos/%2%/%3%/releases/tags/%4%") % m_opts.api_base
               % m_opts.owner % m_opts.repo % Http::url_encode(m_opts.tag)).str();
    else if (m_opts.allow_prerelease)
        url = (boost::format("%1%/repos/%2%/%3%/releases?per_page=20") % m_opts.api_base
               % m_opts.owner % m_opts.repo).str();
    else
        url = (boost::format("%1%/repos/%2%/%3%/releases/latest") % m_opts.api_base
               % m_opts.owner % m_opts.repo).str();

    std::string body;
    bool        ok   = false;
    auto        http = Http::get(url);
    http.timeout_connect(10)
        .timeout_max(30)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28")
        .header("User-Agent", "UnlayeredSlicer");
    const bool sent_credential = apply_feed_credential(http, url, m_opts.credential);
    http.on_error([&](std::string b, std::string error, unsigned status) {
            m_last_status = status;
            BOOST_LOG_TRIVIAL(error) << boost::format("SysFiles: release request failed: %1%, HTTP %2%") % error % status;
            if (status == 404)
                error_out = (boost::format("No configuration releases found at %1%/%2%") % m_opts.owner % m_opts.repo).str();
            // The feed needs the customer's Unlayered account sign-in. It distinguishes the two cases
            // and so must this. 401: no sign-in was sent, or the site no longer knows it (revoked,
            // expired, signed out on the website).
            // 403: the site knows the account, and the account has no printer-update access. Both
            // are things a customer can act on, and neither reads that way as a bare HTTP code.
            else if (status == 401)
                error_out = sent_credential
                    ? "The Unlayered website no longer accepts this computer's sign-in. Sign out and "
                      "sign in again to your Unlayered account from the Help menu, then try again."
                    : "Configuration updates come with your Unlayered account. Sign in with "
                      "Help > Sign in to Unlayered, then try again.";
            else if (status == 403 && sent_credential)
                error_out = "Your Unlayered account does not have access to printer configuration "
                            "updates. Contact Unlayered if it should.";
            else if (status == 403)
                error_out = "The update feed rejected the request (rate limited, or the repository is private)";
            else
                error_out = error.empty() ? (boost::format("HTTP %1%") % status).str() : error;
        })
        .on_complete([&](std::string b, unsigned status) {
            m_last_status = status;
            body = std::move(b);
            ok   = true;
        })
        .perform_sync();

    if (!ok)
        return false;

    out = Release();
    try {
        pt::ptree root;
        std::istringstream iss(body);
        pt::read_json(iss, root);

        // A tag or "latest" query returns one object; a list query returns an array.
        const pt::ptree *release = nullptr;
        pt::ptree        picked;
        if (m_opts.tag.empty() && m_opts.allow_prerelease) {
            for (const auto &kv : root) {
                if (kv.second.get<bool>("draft", false))
                    continue;
                picked  = kv.second;
                release = &picked;
                break;
            }
            if (!release) {
                error_out = (boost::format("No published configuration releases at %1%/%2%")
                             % m_opts.owner % m_opts.repo).str();
                return false;
            }
        } else {
            release = &root;
        }

        out.tag     = release->get<std::string>("tag_name", "");
        out.version = boost::starts_with(out.tag, "v") ? out.tag.substr(1) : out.tag;
        out.notes   = release->get<std::string>("body", "");

        // Prefer the asset the options name; fall back to any zip, so a release that calls it
        // something else still works.
        if (auto assets = release->get_child_optional("assets")) {
            for (const auto &kv : *assets) {
                const std::string name = kv.second.get<std::string>("name", "");
                const std::string durl = kv.second.get<std::string>("browser_download_url", "");
                if (name.empty() || durl.empty())
                    continue;
                // GitHub's own field, which unlayered3d.com sends too: "sha256:<hex>". Anything else
                // (another algorithm, a malformed value) is treated as absent, not as a failure.
                std::string digest = boost::algorithm::to_lower_copy(kv.second.get<std::string>("digest", ""));
                if (boost::starts_with(digest, "sha256:"))
                    digest = digest.substr(7);
                else
                    digest.clear();
                if (digest.size() != 64 ||
                    !std::all_of(digest.begin(), digest.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
                    digest.clear();
                if (boost::iequals(name, m_opts.asset_name)) {
                    out.asset_name   = name;
                    out.asset_url    = durl;
                    out.asset_sha256 = digest;
                    break;
                }
                if (out.asset_url.empty() && boost::iends_with(name, ".zip")) {
                    out.asset_name   = name;
                    out.asset_url    = durl;
                    out.asset_sha256 = digest;
                }
            }
        }
        if (out.asset_url.empty()) {
            error_out = (boost::format("Release %1% has no configuration archive attached") % out.tag).str();
            return false;
        }
    } catch (const std::exception &e) {
        error_out = (boost::format("Could not parse the release response: %1%") % e.what()).str();
        return false;
    }
    return true;
}

bool GitHubSysFileSource::fetch(SysFilePackage &pkg_out, SysProgressFn progress, std::string &error_out)
{
    Release release;
    if (!fetch_release(release, error_out))
        return false;
    const std::string &tag        = release.tag;
    const std::string &asset_name = release.asset_name;
    const std::string &asset_url  = release.asset_url;
    std::vector<uint8_t> zip_bytes;
    bool                 download_ok = false;
    bool                 cancelled   = false;
    auto                 dl          = Http::get(asset_url);
    dl.timeout_connect(10)
      .timeout_max(120)
      .header("Accept", "application/octet-stream");
    dl.header("User-Agent", "UnlayeredSlicer");
    apply_feed_credential(dl, asset_url, m_opts.credential);
    dl.on_error([&](std::string b, std::string error, unsigned status) {
            if (!cancelled)
                error_out = (boost::format("Could not download %1%: %2%") % asset_name
                             % (error.empty() ? (boost::format("HTTP %1%") % status).str() : error)).str();
        })
        .on_complete([&](std::string b, unsigned) {
            zip_bytes.assign(b.begin(), b.end());
            download_ok = true;
        })
        .on_progress([&](Http::Progress p, bool &cancel) {
            if (progress && !progress((boost::format("Downloading %1%") % asset_name).str(), p.dlnow, p.dltotal)) {
                cancel      = true;
                cancelled   = true;
                error_out   = "Cancelled";
            }
        })
        .perform_sync();

    if (!download_ok)
        return false;
    if (zip_bytes.empty()) {
        error_out = (boost::format("%1% is empty") % asset_name).str();
        return false;
    }
    // Checked before the archive is opened: a download cut short, or changed on the way, is refused
    // as a whole rather than half read.
    if (!release.asset_sha256.empty()) {
        const std::string actual = sha256_hex(zip_bytes);
        if (!boost::iequals(actual, release.asset_sha256)) {
            BOOST_LOG_TRIVIAL(error) << "SysFiles: " << asset_name << " has sha256 " << actual << ", the release says "
                                     << release.asset_sha256;
            error_out = (boost::format("%1% did not arrive intact (its checksum does not match the release). Nothing "
                                       "was installed; try again.") % asset_name).str();
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "SysFiles: " << asset_name << " matches the release digest sha256:" << actual;
    }

    MZ_Archive zip;
    if (!mz_zip_reader_init_mem(&zip.arch, zip_bytes.data(), zip_bytes.size(), 0)) {
        error_out = (boost::format("%1% is not a readable zip archive") % asset_name).str();
        return false;
    }
    std::vector<RawEntry> entries;
    CardFilter            filter;
    const bool            read_ok = read_zip_entries(zip.arch, entries, filter, progress, error_out);
    mz_zip_reader_end(&zip.arch);
    if (!read_ok)
        return false;

    const std::string origin = (boost::format("GitHub release %1% (%2%/%3%)") % tag % m_opts.owner % m_opts.repo).str();
    if (!build_package(std::move(entries), origin, filter, pkg_out, error_out))
        return false;

    // A manifest inside the archive is authoritative; without one the tag stands in, exactly as it
    // does for a firmware release with no manifest.
    if (pkg_out.version.empty())
        pkg_out.version = release.version;
    if (pkg_out.notes.empty())
        pkg_out.notes = release.notes;

    // validate_sys_package() checks every DECLARED checksum before an install; this makes declaring
    // them compulsory for a release, so a package that would be written unverified never gets that far.
    if (m_opts.require_checksums) {
        auto unchecked = std::find_if(pkg_out.files.begin(), pkg_out.files.end(),
                                      [](const SysFile &f) { return f.sha256.empty(); });
        if (unchecked != pkg_out.files.end()) {
            error_out = (boost::format("Release %1% does not declare a checksum for %2%, so it cannot be verified and "
                                       "was not used. Configuration releases are built with tools/make_sys_bundle.py.")
                         % tag % unchecked->path).str();
            pkg_out = SysFilePackage();
            return false;
        }
    }
    return true;
}

} // namespace Slic3r
