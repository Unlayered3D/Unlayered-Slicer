#include "Duet.hpp"

#include <algorithm>
#include <cassert>
#include <ctime>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <utility>
#include <wx/frame.h>
#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/BedMesh.hpp"
#include <nlohmann/json.hpp>
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "Http.hpp"

namespace Slic3r {

Duet::Duet(DynamicPrintConfig *config) :
	host(config->opt_string("print_host")),
	password(config->opt_string("printhost_apikey"))
{}

const char* Duet::get_name() const { return "Duet"; }

bool Duet::test(wxString &msg) const
{
	auto connectionType = connect(msg);
	disconnect(connectionType);

	return connectionType != ConnectionType::error;
}

wxString Duet::get_test_ok_msg () const
{
	return _(L("Connection to Duet is working correctly."));
}

wxString Duet::get_test_failed_msg (wxString &msg) const
{
    return GUI::from_u8((boost::format("%s: %s")
                    % _utf8(L("Could not connect to Duet"))
                    % std::string(msg.ToUTF8())).str());
}

// Unlayered -- see the note in Duet.hpp. Only the naming differs from the base class.
const char* Unlayered::get_name() const { return "Unlayered"; }

wxString Unlayered::get_test_ok_msg () const
{
	return _(L("Connection to Unlayered is working correctly."));
}

wxString Unlayered::get_test_failed_msg (wxString &msg) const
{
    return GUI::from_u8((boost::format("%s: %s")
                    % _utf8(L("Could not connect to Unlayered"))
                    % std::string(msg.ToUTF8())).str());
}

bool Duet::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
	wxString connect_msg;
	auto connectionType = connect(connect_msg);
	if (connectionType == ConnectionType::error) {
		error_fn(std::move(connect_msg));
		return false;
	}

	bool res = true;
	bool dsf = (connectionType == ConnectionType::dsf);

	auto upload_cmd = get_upload_url(upload_data.upload_path.string(), connectionType);
	BOOST_LOG_TRIVIAL(info) << boost::format("Duet: Uploading file %1%, filepath: %2%, post_action: %3%, command: %4%")
		% upload_data.source_path
		% upload_data.upload_path
		% int(upload_data.post_action)
		% upload_cmd;

	auto http = (dsf ? Http::put(std::move(upload_cmd)) : Http::post(std::move(upload_cmd)));
	if (dsf) {
		http.set_put_body(upload_data.source_path);
	} else {
		http.set_post_body(upload_data.source_path);
	}
	http.on_complete([&](std::string body, unsigned status) {
			BOOST_LOG_TRIVIAL(debug) << boost::format("Duet: File uploaded: HTTP %1%: %2%") % status % body;

			int err_code = dsf ? (status == 201 ? 0 : 1) : get_err_code_from_body(body);
			if (err_code != 0) {
				BOOST_LOG_TRIVIAL(error) << boost::format("Duet: Request completed but error code was received: %1%") % err_code;
				error_fn(format_error(body, _u8L("Unknown error occurred"), 0));
				res = false;
			} else if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
				wxString errormsg;
				res = start_print(errormsg, upload_data.upload_path.string(), connectionType, false);
				if (! res) {
					error_fn(std::move(errormsg));
				}
			} else if (upload_data.post_action == PrintHostPostUploadAction::StartSimulation) {
				wxString errormsg;
				res = start_print(errormsg, upload_data.upload_path.string(), connectionType, true);
				if (! res) {
					error_fn(std::move(errormsg));
				}
			}
		})
		.on_error([&](std::string body, std::string error, unsigned status) {
			BOOST_LOG_TRIVIAL(error) << boost::format("Duet: Error uploading file: %1%, HTTP %2%, body: `%3%`") % error % status % body;
			error_fn(format_error(body, error, status));
			res = false;
		})
		.on_progress([&](Http::Progress progress, bool &cancel) {
			prorgess_fn(std::move(progress), cancel);
			if (cancel) {
				// Upload was canceled
				BOOST_LOG_TRIVIAL(info) << "Duet: Upload canceled";
				res = false;
			}
		})
		.perform_sync();

	disconnect(connectionType);

	return res;
}

Duet::ConnectionType Duet::connect(wxString &msg) const
{
	auto res = ConnectionType::error;
	auto url = get_connect_url(false);

	auto http = Http::get(std::move(url));
	http.on_error([&](std::string body, std::string error, unsigned status) {
			auto dsfUrl = get_connect_url(true);
			auto dsfHttp = Http::get(std::move(dsfUrl));
			dsfHttp.on_error([&](std::string body, std::string error, unsigned status) {
					BOOST_LOG_TRIVIAL(error) << boost::format("Duet: Error connecting: %1%, HTTP %2%, body: `%3%`") % error % status % body;
					msg = format_error(body, error, status);
				})
				.on_complete([&](std::string body, unsigned) {
					res = ConnectionType::dsf;
				})
				.perform_sync();
		})
		.on_complete([&](std::string body, unsigned) {
			BOOST_LOG_TRIVIAL(debug) << boost::format("Duet: Got: %1%") % body;

			int err_code = get_err_code_from_body(body);
			switch (err_code) {
				case 0:
					res = ConnectionType::rrf;
					break;
				case 1:
					msg = format_error(body, _u8L("Wrong password"), 0);
					break;
				case 2:
					msg = format_error(body, _u8L("Could not get resources to create a new connection"), 0);
					break;
				default:
					msg = format_error(body, _u8L("Unknown error occurred"), 0);
					break;
			}

		})
		.perform_sync();

	return res;
}

// Bed mesh readback, logged step by step under "Duet: bed mesh:":
//   1. rr_connect (standalone) or machine/status (DSF), as for any request.
//   2. rr_model?key=move.compensation  or  machine/status .move.compensation
//      -> "file" is the loaded height map's path, null when none is loaded (RRF 3; RRF 2 has no
//         object model and the default path is assumed).
//   3. rr_download?name=<file>  or  machine/file/<file>  -> the CSV, parsed by BedMesh.
bool Duet::get_bed_mesh(BedMesh &mesh, wxString &error) const
{
	using json = nlohmann::json;
	wxString msg;
	const ConnectionType type = connect(msg);
	if (type == ConnectionType::error) {
		error = msg;
		return false;
	}
	const bool        dsf  = type == ConnectionType::dsf;
	const std::string base = get_base_url();
	auto request = [&](const std::string& url, const char* step, std::string& body, unsigned& status) -> bool {
		bool ok = true;
		status  = 0;
		auto http = Http::get(url);
		http.timeout_connect(5).timeout_max(20)
			.on_error([&](std::string b, std::string err, unsigned s) {
				ok     = false;
				status = s;
				body   = std::move(b);
				BOOST_LOG_TRIVIAL(error) << "Duet: bed mesh: " << step << " failed: " << err << ", HTTP " << s << ", body: `" << body.substr(0, 300) << "`";
			})
			.on_complete([&](std::string b, unsigned s) {
				status = s;
				body   = std::move(b);
				BOOST_LOG_TRIVIAL(info) << "Duet: bed mesh: " << step << " HTTP " << s << ", " << body.size() << " bytes";
				BOOST_LOG_TRIVIAL(debug) << "Duet: bed mesh: " << step << " body: " << body.substr(0, 600);
			})
			.perform_sync();
		return ok;
	};
	std::string body;
	unsigned    status = 0;

	std::string file   = "0:/sys/heightmap.csv";
	bool        loaded = true;
	{
		const std::string url = dsf ? base + "machine/status" : base + "rr_model?key=move.compensation";
		if (request(url, dsf ? "GET machine/status" : "GET rr_model?key=move.compensation", body, status)) {
			try {
				json        j    = json::parse(body);
				const json* comp = nullptr;
				if (dsf) {
					if (j.contains("move") && j["move"].is_object() && j["move"].contains("compensation"))
						comp = &j["move"]["compensation"];
				}
				else if (j.contains("result"))
					comp = &j["result"];
				if (comp != nullptr && comp->is_object()) {
					auto f = comp->find("file");
					if (f != comp->end() && f->is_string() && !f->get<std::string>().empty()) {
						file   = f->get<std::string>();
						loaded = true;
					}
					else if (f != comp->end() && f->is_null())
						loaded = false;
					BOOST_LOG_TRIVIAL(info) << "Duet: bed mesh: move.compensation.file = " << (loaded ? file : std::string("null")) << ", type = " << comp->value("type", std::string("?"));
				}
			} catch (const std::exception& ex) {
				BOOST_LOG_TRIVIAL(warning) << "Duet: bed mesh: object model reply not understood (" << ex.what() << "); assuming " << file;
			}
		}
		else
			BOOST_LOG_TRIVIAL(warning) << "Duet: bed mesh: no object model (RRF 2?); assuming " << file;
	}

	const std::string url = dsf ? base + "machine/file/" + Http::url_encode(file) : base + "rr_download?name=" + Http::url_encode(file);
	if (!request(url, dsf ? "GET machine/file" : "GET rr_download", body, status)) {
		disconnect(type);
		error = GUI::from_u8(status == 404 ? "There is no height map file " + file + " on the printer. Run G29 to probe one." :
		                                     "Could not download " + file + ", HTTP " + std::to_string(status) + ".");
		return false;
	}
	disconnect(type);
	std::string perr;
	if (!BedMesh::from_rrf_heightmap_csv(body, mesh, perr)) {
		BOOST_LOG_TRIVIAL(error) << "Duet: bed mesh: " << file << " not understood: " << perr;
		error = GUI::from_u8("Could not read " + file + ": " + perr);
		return false;
	}
	mesh.profile_name = file;
	mesh.loaded       = loaded;
	if (!loaded)
		mesh.note = "on the printer but not loaded; G29 S1 loads it";
	BOOST_LOG_TRIVIAL(info) << "Duet: bed mesh: " << file << " parsed, " << mesh.probed_cols() << " x " << mesh.probed_rows() << ", loaded = " << loaded;
	return true;
}

void Duet::disconnect(ConnectionType connectionType) const
{
	// we don't need to disconnect from DSF or if it failed anyway
	if (connectionType != ConnectionType::rrf) {
		return;
	}
	auto url =  (boost::format("%1%rr_disconnect")
			% get_base_url()).str();

	auto http = Http::get(std::move(url));
	http.on_error([&](std::string body, std::string error, unsigned status) {
		// we don't care about it, if disconnect is not working Duet will disconnect automatically after some time
		BOOST_LOG_TRIVIAL(error) << boost::format("Duet: Error disconnecting: %1%, HTTP %2%, body: `%3%`") % error % status % body;
	})
	.perform_sync();
}

std::string Duet::get_upload_url(const std::string &filename, ConnectionType connectionType) const
{
    assert(connectionType != ConnectionType::error);

	if (connectionType == ConnectionType::dsf) {
		return (boost::format("%1%machine/file/gcodes/%2%")
				% get_base_url()
				% Http::url_encode(filename)).str();
	} else {
		return (boost::format("%1%rr_upload?name=0:/gcodes/%2%&%3%")
				% get_base_url()
				% Http::url_encode(filename)
				% timestamp_str()).str();
	}
}

std::string Duet::get_connect_url(const bool dsfUrl) const
{
	if (dsfUrl)	{
		return (boost::format("%1%machine/status")
				% get_base_url()).str();
	} else {
		return (boost::format("%1%rr_connect?password=%2%&%3%")
				% get_base_url()
				% Http::url_encode(password.empty() ? "reprap" : password) // url_encode is needed because password can contain special characters like `&`, "#", etc.
				% timestamp_str()).str();
	}
}

std::string Duet::get_base_url() const
{
	if (host.find("http://") == 0 || host.find("https://") == 0) {
		if (host.back() == '/') {
			return host;
		} else {
			return (boost::format("%1%/") % host).str();
		}
	} else {
		return (boost::format("http://%1%/") % host).str();
	}
}

std::string Duet::timestamp_str() const
{
	enum { BUFFER_SIZE = 32 };

	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);

	char buffer[BUFFER_SIZE];
	std::strftime(buffer, BUFFER_SIZE, "time=%Y-%m-%dT%H:%M:%S", &tm);

	return std::string(buffer);
}

bool Duet::start_print(wxString &msg, const std::string &filename, ConnectionType connectionType, bool simulationMode) const
{
    assert(connectionType != ConnectionType::error);

	bool res = false;
	bool dsf = (connectionType == ConnectionType::dsf);

	auto url = dsf
		? (boost::format("%1%machine/code")
			% get_base_url()).str()
		: (boost::format(simulationMode
				? "%1%rr_gcode?gcode=M37%%20P\"0:/gcodes/%2%\""
				: "%1%rr_gcode?gcode=M32%%20\"0:/gcodes/%2%\"")
			% get_base_url()
			% Http::url_encode(filename)).str();

	auto http = (dsf ? Http::post(std::move(url)) : Http::get(std::move(url)));
	if (dsf) {
		http.set_post_body(
				(boost::format(simulationMode
						? "M37 P\"0:/gcodes/%1%\""
						: "M32 \"0:/gcodes/%1%\"")
					% filename).str()
				);
	}
	http.on_error([&](std::string body, std::string error, unsigned status) {
			BOOST_LOG_TRIVIAL(error) << boost::format("Duet: Error starting print: %1%, HTTP %2%, body: `%3%`") % error % status % body;
			msg = format_error(body, error, status);
		})
		.on_complete([&](std::string body, unsigned) {
			BOOST_LOG_TRIVIAL(debug) << boost::format("Duet: Got: %1%") % body;
			res = true;
		})
		.perform_sync();

	return res;
}

}
