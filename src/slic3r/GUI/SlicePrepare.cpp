#include "SlicePrepare.hpp"

#include "AutoUpdater.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "SliceServiceOptionsDialog.hpp"
#include "slic3r/Utils/NonplanarSliceClient.hpp"
#include "slic3r/Utils/UnlayeredAccount.hpp"

#include "libslic3r/Model.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

namespace {

// How often the plate is looked at while a service mode is selected. A prepare goes out once the key
// has been the same on two ticks in a row, so a drag or a run of edits sends one request at the end.
const int TICK_MS = 3000;

struct Fnv
{
    uint64_t h = 1469598103934665603ull;
    void     bytes(const void *data, size_t n)
    {
        const unsigned char *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    template<class T> void value(const T &v) { bytes(&v, sizeof(T)); }
};

UnlayeredAccount *account()
{
    AutoUpdater *u = wxGetApp().auto_updater();
    return u != nullptr ? &u->account() : nullptr;
}

std::string account_email()
{
    UnlayeredAccount *acc = account();
    if (acc == nullptr)
        return {};
    auto p = acc->profile();
    return p ? p->email : std::string();
}

} // namespace

SlicePrepare::SlicePrepare(Plater *plater) : m_plater(plater), m_timer(this)
{
    Bind(wxEVT_TIMER, &SlicePrepare::on_tick, this);
}

SlicePrepare::~SlicePrepare()
{
    m_timer.Stop();
    m_quit = true; // the upload's progress hook aborts on this
    if (m_thread.joinable())
        m_thread.join();
}

void SlicePrepare::set_active(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (!active) {
        m_timer.Stop();
        m_pending = 0;
        return;
    }
    m_timer.Start(TICK_MS);
    // Selecting the mode is the moment the model is known: send right away if nothing is moving.
    check(true);
}

void SlicePrepare::on_tick(wxTimerEvent &) { check(false); }

// The geometry the service's mesh preparation sees -- the current plate's printable instances and
// every volume's mesh in plate coordinates (moving or rotating a part is a different preparation;
// other plates are not sent) -- plus the selected mode, which a service may prepare differently.
// Settings and option changes leave it alone: a service caches its preparation by geometry.
uint64_t SlicePrepare::plate_key() const
{
    Fnv f;
    f.value(m_plater->slice_service_fingerprint(true));
    const std::string mode = SliceService::current_mode();
    f.bytes(mode.data(), mode.size());
    return f.h == 0 ? 1 : f.h;
}

bool SlicePrepare::may_send() const
{
    if (!m_active || m_unsupported || m_busy || m_quit)
        return false;
    if (m_plater->model().objects.empty())
        return false;
    // A service slice (or any other job) is running: it is doing the work itself.
    if (!m_plater->get_ui_job_worker().is_idle())
        return false;
    if (!NonplanarSliceClient::is_configured())
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_refused || m_refused_for != account_email();
}

void SlicePrepare::check(bool send_now)
{
    if (!may_send())
        return;
    const uint64_t key = plate_key();
    if (key == m_last_sent) {
        m_pending = 0;
        return;
    }
    if (send_now || key == m_pending)
        send(key);
    else
        m_pending = key;
}

void SlicePrepare::send(uint64_t key)
{
    Plater::SliceServiceHandoff handoff;
    wxString                    error;
    if (!m_plater->export_slice_service_handoff(handoff, error)) {
        BOOST_LOG_TRIVIAL(info) << "SlicePrepare: export failed: " << into_u8(error);
        return;
    }
    m_last_sent = key;
    m_pending   = 0;

    const std::string url    = NonplanarSliceClient::configured_url();
    const std::string email  = account_email();
    UnlayeredAccount *acc    = account();

    if (m_thread.joinable())
        m_thread.join(); // finished: m_busy was false
    m_busy   = true;
    m_thread = std::thread([this, url, email, acc, handoff]() {
        NonplanarSliceClient client(url);
        client.set_plate(handoff.plate_index + 1);
        client.set_mode(handoff.mode);
        client.set_machine_axes(handoff.machine_axes);
        std::string          error;
        unsigned             status = 0;
        std::string          status_text;
        // A provider token only where the token rules allow one; a refusal by the provider is
        // treated like one by the service.
        long       refresh_after = 0;
        const auto auth          = client.authorize(acc, error, refresh_after);
        const bool tokened       = auth == NonplanarSliceClient::TokenResult::Attached ||
                                   auth == NonplanarSliceClient::TokenResult::NotNeeded;
        if (auth == NonplanarSliceClient::TokenResult::Refused) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_refused     = true;
            m_refused_for = email;
        }
        if (tokened && !m_quit) {
            status = client.prepare(handoff.plate_3mf, true, handoff.options, handoff.printer, handoff.process,
                                    handoff.filament, status_text, error,
                                    [this](uint64_t, uint64_t) { return !m_quit.load(); });
            if (status == 404 || status == 405)
                m_unsupported = true;
            else if (status == 401 || status == 402 || status == 403) {
                // Refused; the service says why when the person slices (NonplanarSliceJob).
                std::lock_guard<std::mutex> lock(m_mutex);
                m_refused     = true;
                m_refused_for = email;
            }
        }
        BOOST_LOG_TRIVIAL(info) << "SlicePrepare: " << handoff.plate_3mf.filename().string() << " -> HTTP " << status
                                << (status_text.empty() ? "" : " " + status_text)
                                << (error.empty() ? "" : " (" + error + ")");
        boost::system::error_code ec;
        boost::filesystem::remove(handoff.plate_3mf, ec);
        m_busy = false;
    });
}

}} // namespace Slic3r::GUI
