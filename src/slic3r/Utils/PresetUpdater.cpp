#include "PresetUpdater.hpp"

#include <algorithm>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <exception>
#include <functional>
#include <atomic>
#include "libslic3r/Exception.hpp"
#include <map>
#include <ios>
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Config.hpp"
#include <set>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <ostream>
#include <utility>
#include <stdexcept>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include <vector>
#include <wx/app.h>
#include <wx/event.h>
#include <wx/msgdlg.h>

#include <nlohmann/json.hpp>
#include "libslic3r/format.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetCacheFormat.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/UpdateDialogs.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "slic3r/Config/Version.hpp"
#include "slic3r/GUI/MarkdownTip.hpp"
#include "libslic3r/miniz_extension.hpp"

using json = nlohmann::json;

namespace Slic3r::GUI::Config { class Snapshot; }
namespace Slic3r::GUI::Config { class SnapshotDB; }

namespace fs = boost::filesystem;
using Slic3r::GUI::Config::Index;
using Slic3r::GUI::Config::Version;


// FIXME: Incompat bundle resolution doesn't deal with inherited user presets

namespace Slic3r {


static const char *INDEX_FILENAME = "index.idx";
static const char *TMP_EXTENSION = ".data";


void copy_file_fix(const fs::path &source, const fs::path &target)
{
	BOOST_LOG_TRIVIAL(debug) << format("PresetUpdater: Copying %1% -> %2%", source, target);
	std::string error_message;
	//CopyFileResult cfr = Slic3r::GUI::copy_file_gui(source.string(), target.string(), error_message, false);
	CopyFileResult cfr = copy_file(source.string(), target.string(), error_message, false);
	if (cfr != CopyFileResult::SUCCESS) {
		BOOST_LOG_TRIVIAL(error) << "Copying failed(" << cfr << "): " << error_message;
		throw Slic3r::CriticalException(GUI::format(
				_L("Copying of file %1% to %2% failed: %3%"),
				source, target, error_message));
	}
	// Permissions should be copied from the source file by copy_file(). We are not sure about the source
	// permissions, let's rewrite them with 644.
	static constexpr const auto perms = fs::owner_read | fs::owner_write | fs::group_read | fs::others_read;
	fs::permissions(target, perms);
}

struct Update
{
	fs::path source;
	fs::path target;

	Version version;
	std::string vendor;
	//BBS: use changelog string instead of url
	std::string change_log;
	std::string descriptions;
    // Orca: add file filter support
    std::function<bool(const std::string)> file_filter;

	bool forced_update;
	//BBS: add directory support
	bool is_directory {false};
	// Orca: a vendor update may be the cache-only form.
	bool is_opc {false};

	Update() {}
	//BBS: add directory support
	//BBS: use changelog string instead of url
	Update(fs::path &&source, fs::path &&target, const Version &version, std::string vendor, std::string changelog, std::string description, bool forced = false, bool is_dir = false)
		: source(std::move(source))
		, target(std::move(target))
		, version(version)
		, vendor(std::move(vendor))
		, change_log(std::move(changelog))
		, descriptions(std::move(description))
		, forced_update(forced)
		, is_directory(is_dir)
	{}

    Update(fs::path &&source, fs::path &&target, const Version &version, std::string vendor, std::string changelog, std::string description, std::function<bool(const std::string)> file_filter,  bool forced = false, bool is_dir = false)
		: source(std::move(source))
		, target(std::move(target))
		, version(version)
		, vendor(std::move(vendor))
		, change_log(std::move(changelog))
		, descriptions(std::move(description))
        , file_filter(file_filter)
		, forced_update(forced)
		, is_directory(is_dir)
	{}

	//BBS: add directory support
	void install() const
	{
        if (is_directory) {
            copy_directory_recursively(source, target, file_filter);
        } else {
            copy_file_fix(source, target);

            // A vendor must be installed in exactly one form. Remove the
            // representation that would otherwise be stale or shadow this one.
            boost::system::error_code ec;
            if (is_opc) {
                fs::remove(target.parent_path() / (vendor + ".json"), ec);
                ec.clear();
                fs::remove_all(target.parent_path() / vendor, ec);
            }
            else {
                fs::remove(target.parent_path() / (vendor + ".opc"), ec);
            }
        }
    }

	friend std::ostream& operator<<(std::ostream& os, const Update &self)
	{
		os << "Update(" << self.source.string() << " -> " << self.target.string() << ')';
		return os;
	}
};

struct Incompat
{
	fs::path bundle;
	Version version;
	std::string vendor;
	//BBS: add directory support
	bool is_directory {false};

	Incompat(fs::path &&bundle, const Version &version, std::string vendor, bool is_dir = false)
		: bundle(std::move(bundle))
		, version(version)
		, vendor(std::move(vendor))
		, is_directory(is_dir)
	{}

	void remove() {
		// Remove the bundle file
		if (is_directory) {
			if (fs::exists(bundle))
                fs::remove_all(bundle);
		}
		else {
			if (fs::exists(bundle))
				fs::remove(bundle);
		}
	}

	friend std::ostream& operator<<(std::ostream& os , const Incompat &self) {
		os << "Incompat(" << self.bundle.string() << ')';
		return os;
	}
};

struct Updates
{
	std::vector<Incompat> incompats;
	std::vector<Update> updates;
};

static bool reload_configs_update_gui();


wxDEFINE_EVENT(EVT_SLIC3R_VERSION_ONLINE, wxCommandEvent);
wxDEFINE_EVENT(EVT_SLIC3R_EXPERIMENTAL_VERSION_ONLINE, wxCommandEvent);


struct PresetUpdater::priv
{
	std::vector<Index> index_db;

	bool enabled_version_check;
	// Local: install and upgrade the vendor bundles shipped in resources/profiles into the user's
	// data dir. Unlayered: always on. This must NOT be tied to any remote URL - see
	// set_download_prefs() for why.
	bool enabled_config_update;
	// Network: fetch vendor profiles from a remote profile service. Unlayered: always off.
	bool enabled_remote_profile_fetch;
	std::string version_check_url;

	fs::path cache_path;
	fs::path rsrc_path;
	fs::path vendor_path;

	bool cancel;
	std::thread thread;

	bool has_waiting_updates { false };
	Updates waiting_updates;
	bool has_waiting_printer_updates { false };
    Updates waiting_printer_updates;

    priv();

	void set_download_prefs(AppConfig *app_config);
	void prune_tmps() const;

	//BBS: refine preset update logic
	bool install_bundles_rsrc(const std::vector<std::string>& bundles, bool snapshot) const;
	void check_installed_vendor_profiles() const;
    Updates get_printer_config_updates(bool update = false) const;
	Updates get_config_updates(const Semver& old_slic3r_version) const;
	bool perform_updates(Updates &&updates, bool snapshot = true) const;
	void set_waiting_updates(Updates u);
};

//BBS: change directories by design
PresetUpdater::priv::priv()
	: cache_path(fs::path(Slic3r::data_dir()) / "ota")
	, rsrc_path(fs::path(resources_dir()) / "profiles")
	, vendor_path(fs::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR)
	, cancel(false)
{
	//BBS: refine preset updater logic
	enabled_version_check = true;
	set_download_prefs(GUI::wxGetApp().app_config);
	// Unlayered: this used to run on the background sync thread. That thread is gone along with the
	// remote sync, but leftover partial downloads still have to be cleaned up.
	prune_tmps();
	// Install indicies from resources. Only installs those that are either missing or older than in resources.
	check_installed_vendor_profiles();
    perform_updates(get_printer_config_updates(), false);
	// Load indices from the cache directory.
	//index_db = Index::load_db();
}

// Pull relevant preferences from AppConfig
void PresetUpdater::priv::set_download_prefs(AppConfig *app_config)
{
	version_check_url = app_config->version_check_url();

	// Unlayered: these two used to be one flag, derived from whether a remote profile URL was
	// configured. They are not the same concern, and conflating them is a trap: enabled_config_update
	// also gates check_installed_vendor_profiles() and get_config_updates(), which install and
	// upgrade the bundles shipped in resources/profiles. Turning it off along with the remote fetch
	// would quietly lose the resource-newer-than-installed upgrade path and the disabled-vendor
	// cleanup, and would leave every enabled vendor being wiped and recopied on every launch.
	enabled_config_update        = true;
	enabled_remote_profile_fetch = false;
}

// Remove a leftover partial archive for the vendor about to be synchronized.
void PresetUpdater::priv::prune_tmps() const
{
	// Called from the constructor now, so the cache dir may not exist yet on a fresh install;
	// directory_iterator would throw.
	if (!fs::exists(cache_path))
		return;
    for (auto &dir_entry : boost::filesystem::directory_iterator(cache_path))
		if (is_plain_file(dir_entry) && dir_entry.path().extension() == TMP_EXTENSION) {
			BOOST_LOG_TRIVIAL(debug) << "[Orca Updater]remove old cached files: " << dir_entry.path().string();
			fs::remove(dir_entry.path());
		}
}

bool PresetUpdater::priv::install_bundles_rsrc(const std::vector<std::string>& bundles, bool snapshot) const
{
	// Use the Core library function to install bundles
	// This function is now in libslic3r so both Core and GUI can use it
	if (!Slic3r::install_vendor_bundles_from_resources(bundles)) {
		BOOST_LOG_TRIVIAL(error) << "Failed to install bundles from resources";
		return false;
	}

	// Snapshot logic is currently commented out in perform_updates, so we don't need to handle it here
	// If snapshot logic is needed in the future, it can be added here

	return true;
}


// Orca: copy/update the vendor profiles from resource to system folder
void PresetUpdater::priv::check_installed_vendor_profiles() const
{
    BOOST_LOG_TRIVIAL(info) << "[Orca Updater]:Checking whether the profile from resource is newer";

    AppConfig *app_config = GUI::wxGetApp().app_config;

    const auto enabled_vendors = app_config->vendors();

    std::set<std::string> bundles;
    // A vendor is named by its profile or, where the build ships preset caches
    // instead of the raw profile JSONs, by its cache alone.
    for (const std::string &vendor_name : vendor_names_in(rsrc_path)) {
        // enabled_vendors lists the vendors whose printer models the user picked, and
        // neither of these two is ever in it.
        const auto is_vendor_enabled = (vendor_name == PresetBundle::ORCA_DEFAULT_BUNDLE)
                                       || (vendor_name == PresetBundle::ORCA_FILAMENT_LIBRARY)
                                       || (enabled_vendors.find(vendor_name) != enabled_vendors.end());
        if (is_vendor_installed(vendor_name)) {
            if (is_vendor_enabled) {
                // Orca: whichever form of the vendor resources ships at the newer
                // version is the one installing lays down, and the one to judge
                // what is installed against.
                Semver resource_ver = resource_vendor_version(vendor_name);
                // Orca: a vendor installed as a preset cache has no profile
                // beside it; the version it was installed at is in the cache.
                Semver vendor_ver = installed_vendor_version(vendor_name);

                if (vendor_ver < resource_ver) {
                    BOOST_LOG_TRIVIAL(info) << "[Orca Updater]:found vendor " << vendor_name << " newer version "
                                            << resource_ver.to_string() << " from resource, old version " << vendor_ver.to_string();
                    bundles.insert(vendor_name);
                }
            } else {
                // need to be removed because not installed
                remove_installed_vendor(vendor_name);
            }
        } else if (is_vendor_enabled) {
            bundles.insert(vendor_name);
        }
    }

    if (bundles.size() > 0) {
        install_bundles_rsrc(std::vector(bundles.begin(), bundles.end()), false);
    }
}

Updates PresetUpdater::priv::get_printer_config_updates(bool update) const
{
    std::string             data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    boost::filesystem::path resc_dir_path(resources_dir());
    auto                    config_folder = data_dir_path / "printers";
    auto                    resc_folder   = (update ? cache_path : resc_dir_path) / "printers";
    std::string             curr_version;
    std::string             resc_version;
    try {
        Slic3r::load_string_file(resc_folder / "version.txt", resc_version);
        boost::algorithm::trim(resc_version);
    } catch (...) {}
    try {
        Slic3r::load_string_file(config_folder / "version.txt", curr_version);
        boost::algorithm::trim(curr_version);
    } catch (...) {}

    if (!curr_version.empty()) {
        Semver curr_ver = curr_version;
        Semver resc_ver   = resc_version;

        bool version_match = ((resc_ver.maj() == curr_ver.maj()) && (resc_ver.min() == curr_ver.min()));

        if (!version_match || (curr_ver < resc_ver)) {
            BOOST_LOG_TRIVIAL(info) << "[Orca Updater]:found newer version " << resc_version << " from resource, old version " << curr_version;
        } else {
            return {};
        }
    }
    Updates updates;
    Version version;
    version.config_version = resc_version;
    std::string change_log;
    if (update) {
        std::string changelog_file = (resc_folder / "printer.json").string();
        try {
            boost::nowide::ifstream ifs(changelog_file);
            json                    j;
            ifs >> j;
            version.comment = j["description"];
        } catch (...) {}
    }
    updates.updates.emplace_back(std::move(resc_folder), std::move(config_folder), version, "bbl", change_log, version.comment, false, true);
    return updates;
}

// Generates a list of bundle updates that are to be performed.
// Version of slic3r that was running the last time and which was read out from PrusaSlicer.ini is provided
// as a parameter.
// Orca: OTA profile updates should be loacated in ota/profiles folder
Updates PresetUpdater::priv::get_config_updates(const Semver &old_slic3r_version) const
{
	Updates updates;

	BOOST_LOG_TRIVIAL(info) << "[Orca Updater]:Checking for cached configuration updates...";
    auto cache_profile_path =  cache_path / "profiles";
    if (!fs::exists(cache_profile_path))
        return updates;

	// Unlayered: upstream fills this from its vendor-profile download; nothing is downloaded here,
	// so cached bundles never carry a changelog.
	const std::unordered_map<std::string, std::string> changelogs;

	for (auto &dir_entry : boost::filesystem::directory_iterator(cache_profile_path)) {
		const auto &path = dir_entry.path();
		std::string file_path = path.string();
		const bool is_opc_file = boost::iequals(path.extension().string(), ".opc");
		if (!is_json_file(file_path) && !is_opc_file)
			continue;

		const std::string vendor_name = path.stem().string();
		auto print_in_cache = (cache_profile_path / vendor_name / PRESET_PRINT_NAME);
		auto filament_in_cache = (cache_profile_path / vendor_name / PRESET_FILAMENT_NAME);
		auto machine_in_cache = (cache_profile_path / vendor_name / PRESET_PRINTER_NAME);

		// Orca (PR #130): a JSON cache entry is only meaningful next to a non-empty
		// <vendor>/ preset directory; a stray or half-downloaded <vendor>.json is
		// skipped. An .opc cache is a single self-contained file (validated below),
		// so this check does not apply to it.
		if (!is_opc_file) {
			const auto vendor_folder_in_cache = cache_profile_path / vendor_name;
			if (!fs::is_regular_file(path) || !fs::is_directory(vendor_folder_in_cache) ||
			    fs::is_empty(vendor_folder_in_cache)) {
				BOOST_LOG_TRIVIAL(warning) << "[Orca Updater]:ignoring invalid cached update for "
				                           << vendor_name << ": expected " << vendor_name
				                           << ".json and a non-empty " << vendor_name << " directory";
				continue;
			}
		}

		if (is_vendor_installed(vendor_name)
			|| is_opc_file
			|| fs::exists(print_in_cache)
			|| fs::exists(filament_in_cache)
			|| fs::exists(machine_in_cache)) {
			// Orca: a vendor installed as a preset cache carries its version there.
			Semver vendor_ver = installed_vendor_version(vendor_name);

			Semver cache_ver;
			std::string description;
			bool force_update = false;
			if (is_opc_file) {
				cache_ver = VendorCacheFile::usable_version(file_path, vendor_name);
				if (!cache_ver.valid()) {
					BOOST_LOG_TRIVIAL(warning) << "[Orca Updater]:ignoring unreadable vendor cache " << file_path;
					continue;
				}
			}
			else {
				std::map<std::string, std::string> key_values;
				std::vector<std::string> keys(3);
				keys[0] = BBL_JSON_KEY_VERSION;
				keys[1] = BBL_JSON_KEY_DESCRIPTION;
				keys[2] = BBL_JSON_KEY_FORCE_UPDATE;
				get_values_from_json(file_path, keys, key_values);
				description = key_values[BBL_JSON_KEY_DESCRIPTION];
				if (key_values.find(BBL_JSON_KEY_FORCE_UPDATE) != key_values.end())
					force_update = (key_values[BBL_JSON_KEY_FORCE_UPDATE] == "1")?true:false;
				auto config_version = Semver::parse(key_values[BBL_JSON_KEY_VERSION]);
				if (config_version)
					cache_ver = *config_version;
			}

			// Orca (PR #130): changelog for this vendor was captured in memory at sync time.
			const auto changelog_it = changelogs.find(vendor_name);
			std::string changelog = changelog_it != changelogs.end() ? changelog_it->second : std::string();

			if (vendor_ver < cache_ver) {
				BOOST_LOG_TRIVIAL(info) << "[Orca Updater]:need to update settings from " << vendor_ver.to_string()
				                        << " to newer version " << cache_ver.to_string() << ", app version " << SLIC3R_VERSION;
				Version version;
				version.config_version = cache_ver;
				version.comment        = description;
				if (is_opc_file) {
					// A cache contains the vendor profile and all presets.
					// Install it directly; Update::install removes any
					// superseded JSON representation.
					auto &update = updates.updates.emplace_back(
					    std::move(file_path), vendor_path / (vendor_name + ".opc"),
					    std::move(version), vendor_name, changelog, "", force_update, false);
					update.is_opc = true;
				}
				else {
					// JSON profile and its preset directory are installed
					// separately, as before.
					updates.updates.emplace_back(std::move(file_path),
					    vendor_path / (vendor_name + ".json"), std::move(version),
					    vendor_name, changelog, "", force_update, false);
					updates.updates.emplace_back(cache_profile_path / vendor_name,
					    vendor_path / vendor_name, Version(), vendor_name,
					    "", "", force_update, true);
				}
			} else {
				BOOST_LOG_TRIVIAL(info) << "[Orca Updater]:cached settings for " << vendor_name
				                        << " are not newer than installed version, installed " << vendor_ver.to_string()
				                        << ", cached " << cache_ver.to_string();
			}
		}
	}

	return updates;
}

//BBS: switch to new BBL.json configs
bool PresetUpdater::priv::perform_updates(Updates &&updates, bool snapshot) const
{
    //std::string vendor_path;
    //std::string vendor_name;
    if (updates.incompats.size() > 0) {
        //if (snapshot) {
        //	BOOST_LOG_TRIVIAL(info) << "Taking a snapshot...";
        //	if (! GUI::Config::take_config_snapshot_cancel_on_error(*GUI::wxGetApp().app_config, Snapshot::SNAPSHOT_DOWNGRADE, "",
        //		_u8L("Continue and install configuration updates?")))
        //		return false;
        //}
        BOOST_LOG_TRIVIAL(info) << format("[Orca Updater]:Deleting %1% incompatible bundles", updates.incompats.size());

        for (auto &incompat : updates.incompats) {
            BOOST_LOG_TRIVIAL(info) << '\t' << incompat;
            incompat.remove();
        }
    } else if (updates.updates.size() > 0) {
        //if (snapshot) {
        //	BOOST_LOG_TRIVIAL(info) << "Taking a snapshot...";
        //	if (! GUI::Config::take_config_snapshot_cancel_on_error(*GUI::wxGetApp().app_config, Snapshot::SNAPSHOT_UPGRADE, "",
        //		_u8L("Continue and install configuration updates?")))
        //		return false;
        //}

        BOOST_LOG_TRIVIAL(info) << format("[Orca Updater]:Performing %1% updates", updates.updates.size());

        for (const auto &update : updates.updates) {
            BOOST_LOG_TRIVIAL(info) << '\t' << update;

            update.install();
            //if (!update.is_directory) {
            //    vendor_path = update.source.parent_path().string();
            //    vendor_name = update.vendor;
            //}
        }

        //if (!vendor_path.empty()) {
        //    PresetBundle bundle;
        //    // Throw when parsing invalid configuration. Only valid configuration is supposed to be provided over the air.
        //    bundle.load_vendor_configs_from_json(vendor_path, vendor_name, PresetBundle::LoadConfigBundleAttribute::LoadSystem, ForwardCompatibilitySubstitutionRule::Disable);

        //    BOOST_LOG_TRIVIAL(info) << format("Deleting %1% conflicting presets", bundle.prints.size() + bundle.filaments.size() + bundle.printers.size());

        //    auto preset_remover = [](const Preset& preset) {
        //        BOOST_LOG_TRIVIAL(info) << '\t' << preset.file;
        //        fs::remove(preset.file);
        //    };

        //    for (const auto &preset : bundle.prints)    { preset_remover(preset); }
        //    for (const auto &preset : bundle.filaments) { preset_remover(preset); }
        //    for (const auto &preset : bundle.printers)  { preset_remover(preset); }
        //}
    }

    return true;
}

void PresetUpdater::priv::set_waiting_updates(Updates u)
{
	waiting_updates = u;
	has_waiting_updates = true;
}

PresetUpdater::PresetUpdater() :
	p(new priv())
{}


// Public

PresetUpdater::~PresetUpdater()
{
	if (p && p->thread.joinable()) {
		// This will stop transfers being done by the thread, if any.
		// Cancelling takes some time, but should complete soon enough.
		p->cancel = true;
		p->thread.join();
	}
}

// Unlayered: PresetUpdater::sync() and check_vendor_update() are gone. They were the only outbound
// network traffic this class produced - vendor profiles from OrcaSlicer's profile service, and the
// network plug-in plus printer configs from Bambu's resource service. Vendor profiles now come
// exclusively from the bundles shipped in resources/profiles, installed by
// check_installed_vendor_profiles() from the constructor.

void PresetUpdater::slic3r_update_notify()
{
	if (! p->enabled_version_check)
		return;
}

static bool reload_configs_update_gui()
{
	wxString header = _L("Please check any unsaved changes before updating the configuration.");
	if (!GUI::wxGetApp().check_and_save_current_preset_changes(_L("Configuration updates"), header, false ))
		return false;

	// Reload global configuration
	auto* app_config = GUI::wxGetApp().app_config;
	// System profiles should not trigger any substitutions, user profiles may trigger substitutions, but these substitutions
	// were already presented to the user on application start up. Just do substitutions now and keep quiet about it.
	// However throw on substitutions in system profiles, those shall never happen with system profiles installed over the air.
	GUI::wxGetApp().preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem);
	GUI::wxGetApp().load_current_presets();
	GUI::wxGetApp().plater()->set_bed_shape();

    return true;
}

PresetUpdater::UpdateResult PresetUpdater::config_update(const Semver& old_slic3r_version, UpdateParams params) const
{
    if (! p->enabled_config_update) { return R_NOOP; }

    auto updates = p->get_config_updates(old_slic3r_version);

    if (updates.updates.size() > 0) {

        bool force_update = false;
        for (const auto& update : updates.updates) {
            force_update = (update.forced_update ? true : force_update);
        }

        //forced update
        if (force_update)
        {
            BOOST_LOG_TRIVIAL(info) << format("[Orca Updater]:Force updating will start, size %1% ", updates.updates.size());
            std::vector<std::string> bundles;
            for (const auto& update : updates.updates) {
                if (update.is_directory)
                    continue;
                bundles.push_back(update.vendor);
            }
            bool ret = p->perform_updates(std::move(updates));
            if (!ret) {
                BOOST_LOG_TRIVIAL(warning) << format("[Orca Updater]:perform_updates failed");
                return R_INCOMPAT_EXIT;
            }

            ret = reload_configs_update_gui();
            if (!ret) {
                BOOST_LOG_TRIVIAL(warning) << format("[Orca Updater]:reload_configs_update_gui failed");
                return R_INCOMPAT_EXIT;
            }
            for(auto b : bundles){
            Semver cur_ver = GUI::wxGetApp().preset_bundle->get_vendor_profile_version(b);
            GUI::wxGetApp()
                .plater()
                ->get_notification_manager()
                ->push_notification(GUI::NotificationType::PresetUpdateFinished,
                                    GUI::NotificationManager::NotificationLevel::ImportantNotificationLevel,
                                    Slic3r::format(_u8L("Configuration package: %1% updated to %2%"), b, cur_ver.to_string()));
            }
            return R_UPDATE_INSTALLED;
        }

        // regular update
        if (params == UpdateParams::SHOW_NOTIFICATION) {
            p->set_waiting_updates(updates);
            GUI::wxGetApp().plater()->get_notification_manager()->push_notification(GUI::NotificationType::PresetUpdateAvailable);
        }
        else {
            BOOST_LOG_TRIVIAL(info) << format("[Orca Updater]:Configuration package available. size %1%, need to confirm...", p->waiting_updates.updates.size());

            std::vector<GUI::MsgUpdateConfig::Update> updates_msg;
            for (const auto& update : updates.updates) {
                if (update.is_directory)
                    continue;
                std::string changelog = update.change_log;
                updates_msg.emplace_back(update.vendor, update.version.config_version, update.descriptions, std::move(changelog));
            }

            GUI::MsgUpdateConfig dlg(updates_msg, params == UpdateParams::FORCED_BEFORE_WIZARD);

            const auto res = dlg.ShowModal();
            if (res == wxID_OK) {
                BOOST_LOG_TRIVIAL(debug) << "[Orca Updater]:selected yes to update";
                if (! p->perform_updates(std::move(updates)) ||
                    ! reload_configs_update_gui())
                    return R_ALL_CANCELED;
                return R_UPDATE_INSTALLED;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << "[Orca Updater]:selected no for updating";
                if (params == UpdateParams::FORCED_BEFORE_WIZARD && res == wxID_CANCEL)
                    return R_ALL_CANCELED;
                return R_UPDATE_REJECT;
            }
        }

        // MsgUpdateConfig will show after the notificaation is clicked
    } else {
        BOOST_LOG_TRIVIAL(info) << "[Orca Updater]:No configuration updates available.";
    }

	return R_NOOP;
}

//BBS: add json related logic
bool PresetUpdater::install_bundles_rsrc(std::vector<std::string> bundles, bool snapshot) const
{
	return p->install_bundles_rsrc(bundles, snapshot);
}

void PresetUpdater::on_update_notification_confirm()
{
	if (!p->has_waiting_updates)
		return;
	BOOST_LOG_TRIVIAL(info) << format("Update of %1% bundles available. Asking for confirmation ...", p->waiting_updates.updates.size());

	std::vector<GUI::MsgUpdateConfig::Update> updates_msg;
	for (const auto& update : p->waiting_updates.updates) {
		//BBS: skip directory
		if (update.is_directory)
			continue;
		std::string changelog = update.change_log;
		updates_msg.emplace_back(update.vendor, update.version.config_version, update.descriptions, std::move(changelog));
	}

	GUI::MsgUpdateConfig dlg(updates_msg);

	const auto res = dlg.ShowModal();
	if (res == wxID_OK) {
		BOOST_LOG_TRIVIAL(debug) << "User agreed to perform the update";
		if (p->perform_updates(std::move(p->waiting_updates)) &&
			reload_configs_update_gui()) {
			p->has_waiting_updates = false;
		}
	}
	else {
		BOOST_LOG_TRIVIAL(info) << "User refused the update";
	}
}

void PresetUpdater::do_printer_config_update()
{
    if (!p->has_waiting_printer_updates)
        return;
    BOOST_LOG_TRIVIAL(info) << "Update of printer configs available. Asking for confirmation ...";

    std::vector<GUI::MsgUpdateConfig::Update> updates_msg;
    for (const auto &update : p->waiting_printer_updates.updates) {
        std::string changelog = update.change_log;
        updates_msg.emplace_back(update.vendor, update.version.config_version, update.descriptions, std::move(changelog));
    }

    GUI::MsgUpdateConfig dlg(updates_msg);

    const auto res = dlg.ShowModal();
    if (res == wxID_OK) {
        BOOST_LOG_TRIVIAL(debug) << "User agreed to perform the update";
        if (p->perform_updates(std::move(p->waiting_printer_updates)))
            p->has_waiting_printer_updates = false;
    } else {
        BOOST_LOG_TRIVIAL(info) << "User refused the update";
    }
}

bool PresetUpdater::version_check_enabled() const
{
	return p->enabled_version_check;
}

}
