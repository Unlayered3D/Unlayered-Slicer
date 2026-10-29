#ifndef slic3r_NonplanarSliceJob_hpp_
#define slic3r_NonplanarSliceJob_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "Job.hpp"
#include "slic3r/Utils/NonplanarSliceClient.hpp"

namespace Slic3r {
class UnlayeredAccount;
namespace GUI {

// Hands an already-exported plate to an external slicing service and gives the G-code it returns
// back to that plate as its slice result (Plater::accept_slice_service_result). The protocol is in
// SLICE_SERVICE_PROTOCOL.md; the client does the talking. A refusal from the service (or from the
// account provider for its token) is shown as the service worded it, with its action link.
//
// The export has to happen before this job starts -- it touches the model and the plate list, so
// it belongs on the GUI thread (see Plater::slice_nonplanar). From here on it is submit, poll,
// download: all blocking, all on the worker thread, with the result handed over in finalize()
// where the GUI thread is ours again.
class NonplanarSliceJob : public Job
{
public:
    NonplanarSliceJob(std::string             service_url,
                      boost::filesystem::path mesh,
                      boost::filesystem::path settings,
                      bool                    settings_from_mesh,
                      std::string             options_json,
                      std::string             printer,
                      std::string             process,
                      std::string             filament,
                      boost::filesystem::path out_dir,
                      UnlayeredAccount       *account = nullptr);

    // Which plate this is (0-based; sent 1-based as the `plate` field) and what it looked like when
    // it was exported, for the plate to check the result against (Plater::slice_nonplanar).
    void set_target(int serial, int plate_index, uint64_t fingerprint, std::string options_key)
    {
        m_serial      = serial;
        m_plate_index = plate_index;
        m_fingerprint = fingerprint;
        m_options_key = std::move(options_key);
    }
    // The selected mode (`mode` form field) and the printer's axes (`machine_axes`), taken on the
    // GUI thread when the plate was exported.
    void set_service_fields(std::string mode, int machine_axes)
    {
        m_mode         = std::move(mode);
        m_machine_axes = machine_axes;
    }

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &e) override;

    // The latest progress the running job reported (any thread): its serial (0 = no job has
    // reported since the last one ended), percent and status text. For the web-app bridge, which
    // reports a service job's progress without the progress bar (Plater::slice_service_plate_state).
    static void last_status(int &serial, int &percent, std::string &text);
    // A refusal's message + action link (+ "Sign in" for a 401), as a dialog. GUI thread.
    static void show_refusal(const ServiceRefusal &refusal);

    int status_range() const { return 100; }

private:
    void run(Ctl &ctl); // process(), reporting through a Ctl that records last_status()

    std::string             m_url;
    // Both are couriers, written by Plater::slice_nonplanar and deleted when the job ends.
    boost::filesystem::path m_mesh;      // the exported plate
    boost::filesystem::path m_settings;  // the resolved print configuration, as JSON (may be empty)
    bool                    m_settings_from_mesh = false;
    std::string             m_options_json;
    std::string             m_printer;
    std::string             m_process;
    std::string             m_filament;
    boost::filesystem::path m_out_dir;
    // The account the application is signed in as (lives as long as the application), for a
    // provider token where the token rules allow one (NonplanarSliceClient::authorize).
    UnlayeredAccount       *m_account = nullptr;
    std::string             m_mode;
    int                     m_machine_axes = 0;
    int                     m_serial      = 0;
    int                     m_plate_index = -1;
    uint64_t                m_fingerprint = 0;
    std::string             m_options_key;

    // Filled by process(), consumed by finalize() on the GUI thread.
    boost::filesystem::path  m_gcode;
    std::string              m_error;
    std::optional<ServiceRefusal> m_refusal; // set instead of m_error when the job was refused
    std::vector<std::string> m_warnings;
    bool                     m_canceled = false;
};

}} // namespace Slic3r::GUI

#endif // slic3r_NonplanarSliceJob_hpp_
