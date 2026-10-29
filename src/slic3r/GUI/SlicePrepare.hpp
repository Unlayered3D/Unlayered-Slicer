#ifndef slic3r_GUI_SlicePrepare_hpp_
#define slic3r_GUI_SlicePrepare_hpp_

// Start the slow first step of a slicing-service slice while the person is still setting up.
//
// A slicing service may cache its mesh preparation by the plate's geometry and the selected mode
// (POST <service>/prepare, SLICE_SERVICE_PROTOCOL.md). So while a service mode is selected in the
// Slice dropdown and a service address is configured, this sends the same hand-off a service slice
// would to /prepare whenever the geometry settles, and the later Slice finds the work done (or
// waits for the preparation already running).
//
// Strictly best effort: no dialogs, no notifications, no progress bar, never on the UI job worker,
// one request in flight. A service without the endpoint (404/405) switches it off for the session;
// a refusal (401/402/403) switches it off until a different account signs in. Whether the service
// takes the work is entirely the service's decision; this never asks anyone first.

#include <wx/event.h>
#include <wx/timer.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r { namespace GUI {

class Plater;

class SlicePrepare : public wxEvtHandler
{
public:
    explicit SlicePrepare(Plater *plater);
    ~SlicePrepare() override; // aborts an upload in flight and waits for the thread

    // MainFrame: a service mode was selected in the Slice dropdown (true) or another entry (false).
    void set_active(bool active);

private:
    void     on_tick(wxTimerEvent &);
    void     check(bool send_now);
    uint64_t plate_key() const;
    bool     may_send() const;
    void     send(uint64_t key);

    Plater  *m_plater;
    wxTimer  m_timer;
    bool     m_active     = false;
    uint64_t m_last_sent  = 0; // the geometry + build key of the last prepare that went out
    uint64_t m_pending    = 0; // seen on the previous tick, not sent yet

    std::thread       m_thread;
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_unsupported{false}; // 404/405: an older service, off for the session

    mutable std::mutex m_mutex;
    bool               m_refused = false; // a refusal came back...
    std::string        m_refused_for;     // ...for this account (email; empty = signed out)
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SlicePrepare_hpp_
