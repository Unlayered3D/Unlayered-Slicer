#ifndef slic3r_Duet_hpp_
#define slic3r_Duet_hpp_

#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"
#include "libslic3r/enum_bitmask.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;

class Duet : public PrintHost
{
public:
    explicit Duet(DynamicPrintConfig *config);
	~Duet() override = default;

	const char* get_name() const override;

	bool test(wxString &curl_msg) const override;
	wxString get_test_ok_msg() const override;
	wxString get_test_failed_msg(wxString &msg) const override;
	bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
	bool has_auto_discovery() const override { return false; }
	bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint | PrintHostPostUploadAction::StartSimulation; }
	std::string get_host() const override { return host; }
	// Downloads the height map file the firmware has loaded (0:/sys/heightmap.csv unless the
	// object model names another), over rr_download or DSF's machine/file.
	bool can_get_bed_mesh() const override { return true; }
	bool get_bed_mesh(BedMesh &mesh, wxString &error) const override;

private:
	enum class ConnectionType { rrf, dsf, error };
	std::string host;
	std::string password;

	std::string get_upload_url(const std::string &filename, ConnectionType connectionType) const;
	std::string get_connect_url(const bool dsfUrl) const;
	std::string get_base_url() const;
	std::string timestamp_str() const;
	ConnectionType connect(wxString &msg) const;
	void disconnect(ConnectionType connectionType) const;
	bool start_print(wxString &msg, const std::string &filename, ConnectionType connectionType, bool simulationMode) const;
};

// A Duet board running Unlayered firmware. The wire protocol is identical -- RepRapFirmware's
// rr_/machine endpoints -- so everything is inherited; this exists so that the host type can be
// told apart from a stock Duet. Two things hang off that distinction:
//   * PhysicalPrinterDialog offers the firmware updater only for this type, because the images it
//     installs are Unlayered builds for a specific board and have no business being pushed at
//     somebody's stock Duet.
//   * The user-facing strings say "Unlayered", so a connection test does not report on a brand the
//     user did not pick.
class Unlayered : public Duet
{
public:
	explicit Unlayered(DynamicPrintConfig *config) : Duet(config) {}
	~Unlayered() override = default;

	const char* get_name() const override;
	wxString get_test_ok_msg() const override;
	wxString get_test_failed_msg(wxString &msg) const override;
};

}

#endif
