#include "NonplanarSliceJob.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/AutoUpdater.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UnlayeredAccount.hpp"

#include "libslic3r/miniz_extension.hpp"

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

#include <wx/utils.h>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {
// A v1 service carries no progress figure, so the bar creeps toward a ceiling while polling and
// only the real steps (submit, download, done) move it decisively. Honest about being a guess:
// it never reaches 100 until the file is actually in hand. A v2 service reports its own 0..1
// progress and step label, which are mapped onto the same submitted..ceiling span.
const int NPS_PCT_SUBMITTED = 10;
const int NPS_PCT_POLL_CEIL = 85;
const int NPS_PCT_DOWNLOAD  = 90;

const int NPS_POLL_INTERVAL_MS = 1500;
// A slice can legitimately take minutes; this only stops a wedged service hanging forever.
const int NPS_POLL_TIMEOUT_S = 3600;

// A service packages its G-code as a ".gcode.3mf" for printers that take a sliced 3MF. The plate's
// slice result is the G-code itself -- this program packages its own 3MF around a plate's G-code
// when such a printer is sent one -- so it is taken out: Metadata/plate_1.gcode (the courier is a
// one-plate project), else the first .gcode entry.
bool extract_gcode(const fs::path &archive_path, const fs::path &out, std::string &error)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (! open_zip_reader(&archive, into_u8(from_path(archive_path)))) {
        error = _u8L("The sliced file from the slicing service could not be opened.");
        return false;
    }
    int index = -1;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
        mz_zip_archive_file_stat stat;
        if (! mz_zip_reader_file_stat(&archive, i, &stat) || ! boost::algorithm::iends_with(stat.m_filename, ".gcode"))
            continue;
        if (boost::algorithm::iequals(stat.m_filename, "Metadata/plate_1.gcode")) {
            index = int(i);
            break;
        }
        if (index < 0)
            index = int(i);
    }
    bool ok = false;
    if (index < 0)
        error = _u8L("The sliced file from the slicing service holds no G-code.");
    else {
        boost::nowide::ofstream os(into_u8(from_path(out)).c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        ok = os.good() &&
             mz_zip_reader_extract_to_callback(&archive, mz_uint(index),
                                               [](void *opaque, mz_uint64, const void *buf, size_t n) -> size_t {
                                                   auto *stream = static_cast<std::ostream *>(opaque);
                                                   stream->write(static_cast<const char *>(buf), std::streamsize(n));
                                                   return stream->good() ? n : 0;
                                               },
                                               static_cast<std::ostream *>(&os), 0);
        os.close();
        if (! ok)
            error = _u8L("The G-code could not be taken out of the sliced file from the slicing service.");
    }
    close_zip_reader(&archive);
    return ok;
}
} // namespace

NonplanarSliceJob::NonplanarSliceJob(std::string service_url,
                                     fs::path    mesh,
                                     fs::path    settings,
                                     bool        settings_from_mesh,
                                     std::string options_json,
                                     std::string printer,
                                     std::string process,
                                     std::string filament,
                                     fs::path    out_dir,
                                     UnlayeredAccount *account)
    : m_url(std::move(service_url))
    , m_mesh(std::move(mesh))
    , m_settings(std::move(settings))
    , m_settings_from_mesh(settings_from_mesh)
    , m_options_json(std::move(options_json))
    , m_printer(std::move(printer))
    , m_process(std::move(process))
    , m_filament(std::move(filament))
    , m_out_dir(std::move(out_dir))
    , m_account(account)
{}

namespace {
std::mutex  s_status_mutex;
int         s_status_serial  = 0;
int         s_status_percent = -1;
std::string s_status_text;

// Forwards to the worker's Ctl and keeps the latest status for last_status().
class RecordingCtl : public Job::Ctl
{
public:
    RecordingCtl(Job::Ctl &inner, int serial) : m_inner(inner), m_serial(serial) {}
    void update_status(int st, const std::string &msg) override
    {
        {
            std::lock_guard<std::mutex> lock(s_status_mutex);
            s_status_serial  = m_serial;
            s_status_percent = st;
            s_status_text    = msg;
        }
        m_inner.update_status(st, msg);
    }
    bool was_canceled() const override { return m_inner.was_canceled(); }
    void clear_percent() override { m_inner.clear_percent(); }
    void show_error_info(const std::string &msg, int code, const std::string &description, const std::string &extra) override
    {
        m_inner.show_error_info(msg, code, description, extra);
    }
    std::future<void> call_on_main_thread(std::function<void()> fn) override { return m_inner.call_on_main_thread(std::move(fn)); }

private:
    Job::Ctl &m_inner;
    int       m_serial;
};

void clear_status(int serial)
{
    std::lock_guard<std::mutex> lock(s_status_mutex);
    if (s_status_serial == serial) {
        s_status_serial  = 0;
        s_status_percent = -1;
        s_status_text.clear();
    }
}
} // namespace

void NonplanarSliceJob::last_status(int &serial, int &percent, std::string &text)
{
    std::lock_guard<std::mutex> lock(s_status_mutex);
    serial  = s_status_serial;
    percent = s_status_percent;
    text    = s_status_text;
}

void NonplanarSliceJob::process(Ctl &ctl)
{
    RecordingCtl recording(ctl, m_serial);
    run(recording);
}

void NonplanarSliceJob::run(Ctl &ctl)
{
    // The worker hides its progress bar only at 100, like the other jobs end; every early return
    // here (an error, Stop) used to leave it sitting at the last percentage next to the error.
    struct CloseBar
    {
        Ctl                     &ctl;
        const NonplanarSliceJob &self;
        bool                     done = false;
        ~CloseBar()
        {
            if (! done)
                ctl.update_status(100, self.m_canceled || ctl.was_canceled() ? _u8L("Slicing cancelled.")
                                                                             : _u8L("Slicing on the service failed."));
        }
    } close_bar{ctl, *this};

    ctl.update_status(0, _u8L("Sending the plate to the slicing service..."));
    if (ctl.was_canceled()) { m_canceled = true; return; }

    NonplanarSliceClient      client(m_url);
    NonplanarSliceClient::Job job;
    client.set_plate(m_plate_index + 1);
    client.set_mode(m_mode);
    client.set_machine_axes(m_machine_axes);
    // Every failure below goes through here, so a refusal (from the service or the provider) is
    // shown as one rather than as a bare error line.
    auto take_refusal = [&]() {
        if (client.refusal()) {
            m_refusal = client.refusal();
            m_error.clear();
        }
    };

    // A provider token goes along only where the token rules allow one (authorize); otherwise
    // the service authenticates the request itself, and may refuse it.
    auto token_at      = std::chrono::steady_clock::now();
    long token_refresh = 0;
    bool tokened       = false;
    auto get_token = [&]() -> bool {
        std::string err;
        switch (client.authorize(m_account, err, token_refresh)) {
        case NonplanarSliceClient::TokenResult::Attached: tokened = true; break;
        case NonplanarSliceClient::TokenResult::NotNeeded: break;
        case NonplanarSliceClient::TokenResult::Refused: take_refusal(); return false;
        case NonplanarSliceClient::TokenResult::Failed:
            m_error = _u8L("Could not get a token for the slicing service from the account provider") + ": " + err;
            return false;
        }
        token_at = std::chrono::steady_clock::now();
        return true;
    };
    if (! get_token())
        return;
    if (ctl.was_canceled()) { m_canceled = true; return; }

    const std::string sending = _u8L("Sending the plate to the slicing service...");
    if (! client.submit(m_mesh, m_settings, m_settings_from_mesh, m_options_json, m_printer, m_process,
                        m_filament, job, m_error,
                        [&ctl, &sending](uint64_t sent, uint64_t total) {
                            if (ctl.was_canceled())
                                return false;
                            if (total > 0)
                                ctl.update_status(int(NPS_PCT_SUBMITTED * double(sent) / double(total)), sending);
                            return true;
                        })) {
        // Stopped mid-upload: the service never got a whole job, so there is nothing to cancel there.
        if (m_error == "cancelled") {
            m_canceled = true;
            m_error.clear();
        }
        take_refusal();
        return;
    }
    // From here a cancel also tells the service, so it stops spending minutes on a slice nobody
    // will open.
    auto cancel = [&]() { m_canceled = true; client.cancel(job.id); };
    if (ctl.was_canceled()) { cancel(); return; }

    m_warnings = job.warnings;
    ctl.update_status(NPS_PCT_SUBMITTED, _u8L("Slicing..."));

    const auto  started  = std::chrono::steady_clock::now();
    int         pct      = NPS_PCT_SUBMITTED;
    while (! job.finished()) {
        if (ctl.was_canceled()) { cancel(); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(NPS_POLL_INTERVAL_MS));
        if (ctl.was_canceled()) { cancel(); return; }

        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started).count() > NPS_POLL_TIMEOUT_S) {
            m_error = _u8L("The slicing service did not finish in time.");
            return;
        }
        if (tokened && std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - token_at).count() > token_refresh &&
            ! get_token())
            return;
        if (! client.poll(job.id, job, m_error)) {
            take_refusal();
            return;
        }
        if (! job.warnings.empty())
            m_warnings = job.warnings;

        if (job.progress >= 0.) {
            // The service's own figure, never moving backwards.
            pct = std::max(pct, NPS_PCT_SUBMITTED +
                                    int((NPS_PCT_POLL_CEIL - NPS_PCT_SUBMITTED) * job.progress));
        } else if (pct < NPS_PCT_POLL_CEIL) {
            // Creep, never arrive: the service tells us nothing about how far along it is.
            ++pct;
        }
        const std::string text = job.status == NonplanarSliceClient::Status::Queued
                                     ? _u8L("Waiting for the slicing service...")
                                 : ! job.stage.empty() ? _u8L("Slicing") + ": " + job.stage + "..."
                                                       : _u8L("Slicing...");
        // Always with the text: every update REPLACES it, so a bare percentage blanked the
        // notification to an empty grey box.
        ctl.update_status(pct, text);
    }

    if (job.status == NonplanarSliceClient::Status::Failed) {
        m_error = job.error.empty() ? _u8L("The slicing service could not slice this plate.")
                                    : job.error;
        return;
    }

    // A unique local name per job, always: the preview silently ignores a G-code file whose path
    // matches the one it is already showing, so re-slicing would appear to do nothing.
    std::string name = job.filename;
    if (name.empty())
        name = "nonplanar.gcode";
    m_gcode = m_out_dir / (job.id.substr(0, 8) + "-" + name);

    const std::string downloading = _u8L("Downloading the G-code...");
    ctl.update_status(NPS_PCT_DOWNLOAD, downloading);
    if (! client.download_gcode(job.id, m_gcode, m_error,
                                [&ctl, &downloading](uint64_t received, uint64_t total) {
                                    if (ctl.was_canceled())
                                        return false;
                                    if (total > 0) {
                                        const int span = 100 - NPS_PCT_DOWNLOAD;
                                        ctl.update_status(NPS_PCT_DOWNLOAD +
                                                              int(span * double(received) / double(total)),
                                                          downloading);
                                    }
                                    return true;
                                },
                                job.sha256)) {
        if (m_error == "cancelled") {
            m_canceled = true;
            m_error.clear();
        }
        take_refusal();
        m_gcode.clear();
        return;
    }
    if (boost::algorithm::iends_with(m_gcode.filename().string(), ".gcode.3mf")) {
        fs::path gcode = m_gcode;
        gcode.replace_extension(); // name.gcode
        const bool extracted = extract_gcode(m_gcode, gcode, m_error);
        boost::system::error_code ec;
        fs::remove(m_gcode, ec);
        m_gcode = extracted ? gcode : fs::path();
        if (! extracted)
            return;
    }
    ctl.update_status(100, _u8L("Loading the G-code..."));
    close_bar.done = true;
}

// The service's (or the provider's) refusal, worded by it: its message, a button for its action
// link (http/https only, checked when parsed), and for a 401 a way to sign in when an account
// provider is configured. The slicer adds no judgement of its own about why.
void NonplanarSliceJob::show_refusal(const ServiceRefusal &refusal)
{
    wxWindow     *parent   = wxGetApp().mainframe;
    AutoUpdater  *updater  = wxGetApp().auto_updater();
    const bool    can_sign = refusal.status == 401 && updater != nullptr && updater->account().has_provider();
    const bool    has_link = ! refusal.action_url.empty();
    const wxString link    = refusal.action_label.empty() ? _L("Open") : from_u8(refusal.action_label);
    const wxString text    = from_u8(refusal.message);

    if (! has_link && ! can_sign) {
        show_error(parent, text);
        return;
    }
    long style = wxICON_WARNING | (has_link && can_sign ? (wxYES_NO | wxCANCEL) : wxYES_NO);
    MessageDialog dlg(parent, text, _L("Slicing service"), style);
    if (has_link && can_sign) {
        dlg.SetButtonLabel(wxID_YES, link, true);
        dlg.SetButtonLabel(wxID_NO, _L("Sign in"));
        dlg.SetButtonLabel(wxID_CANCEL, _L("Close"));
    } else {
        dlg.SetButtonLabel(wxID_YES, has_link ? link : _L("Sign in"), true);
        dlg.SetButtonLabel(wxID_NO, _L("Close"));
    }
    const int answer = dlg.ShowModal();
    const bool open_link = has_link && answer == wxID_YES;
    const bool sign_in   = can_sign && ((has_link && answer == wxID_NO) || (! has_link && answer == wxID_YES));
    if (open_link)
        wxLaunchDefaultBrowser(from_u8(refusal.action_url));
    else if (sign_in)
        updater->sign_in(parent);
}

void NonplanarSliceJob::finalize(bool canceled, std::exception_ptr &e)
{
    clear_status(m_serial);
    // The exported plate and settings were only ever couriers.
    boost::system::error_code ec;
    if (! m_mesh.empty())
        fs::remove(m_mesh, ec);
    if (! m_settings.empty())
        fs::remove(m_settings, ec);

    if (e) {
        // Let the worker rethrow: an exception here is a bug, not a service failure, and the
        // usual handler will report it rather than this job swallowing it.
        if (Plater *plater = wxGetApp().plater(); plater != nullptr)
            plater->slice_service_job_ended(m_serial);
        return;
    }

    NotificationManager *notify = wxGetApp().plater() ? wxGetApp().plater()->get_notification_manager()
                                                      : nullptr;
    Plater *plater = wxGetApp().plater();
    if (canceled || m_canceled) {
        BOOST_LOG_TRIVIAL(info) << "NonplanarSliceJob: cancelled";
        if (plater != nullptr)
            plater->slice_service_job_ended(m_serial);
        return;
    }

    if (m_refusal) {
        BOOST_LOG_TRIVIAL(warning) << "NonplanarSliceJob: refused (HTTP " << m_refusal->status << ", "
                                   << m_refusal->code << "): " << m_refusal->message;
        if (plater != nullptr)
            plater->slice_service_job_ended(m_serial);
        show_refusal(*m_refusal);
        return;
    }

    if (! m_error.empty() || m_gcode.empty()) {
        const std::string msg = m_error.empty() ? _u8L("Slicing on the service failed.") : m_error;
        BOOST_LOG_TRIVIAL(error) << "NonplanarSliceJob: " << msg;
        if (plater != nullptr)
            plater->slice_service_job_ended(m_serial);
        if (notify != nullptr)
            notify->push_notification(NotificationType::CustomNotification,
                                      NotificationManager::NotificationLevel::ErrorNotificationLevel,
                                      _u8L("Slicing service") + ": " + msg);
        return;
    }

    if (notify != nullptr && ! m_warnings.empty())
        notify->push_notification(NotificationType::CustomNotification,
                                  NotificationManager::NotificationLevel::WarningNotificationLevel,
                                  _u8L("Slicing service") + ": " +
                                      boost::algorithm::join(m_warnings, " · "));

    // The G-code becomes the slice result of the plate it was made from, in this project: shown in
    // Preview, exported and printed from there like a local slice, until an edit invalidates it.
    if (plater != nullptr)
        plater->accept_slice_service_result(m_serial, m_plate_index, m_fingerprint, m_options_key, m_gcode);
}

}} // namespace Slic3r::GUI
