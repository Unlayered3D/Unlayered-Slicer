#include "AutoPlanesJob.hpp"

#include "slic3r/GUI/I18N.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

void AutoPlanesJob::process(Ctl &ctl)
{
    if (! m_state || ! m_model || m_model->objects.empty())
        return;

    ctl.update_status(0, _u8L("Suggesting cut planes"));

    AutoPlanes::Progress prog;
    prog.canceled = [&ctl]() { return ctl.was_canceled(); };
    prog.report   = [this, &ctl](int pct, const std::string &stage) {
        m_state->percent.store(pct);
        {
            std::lock_guard<std::mutex> lk(m_state->mtx);
            m_state->stage = stage;
        }
        ctl.update_status(pct, _u8L("Suggesting cut planes"));
    };

    AutoPlanes::PlanResult res;
    try {
        res = AutoPlanes::plan(*m_model->objects.front(), m_trafo, m_config, m_options, &prog);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "AutoPlanesJob: plan() threw: " << e.what();
        std::lock_guard<std::mutex> lk(m_state->mtx);
        m_state->error = e.what();
        m_state->failed.store(true);
        return;
    }

    if (ctl.was_canceled() || res.canceled) {
        m_state->canceled.store(true);
        return;
    }

    std::lock_guard<std::mutex> lk(m_state->mtx);
    m_state->result = std::move(res);
}

void AutoPlanesJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (! m_state)
        return;
    if (canceled)
        m_state->canceled.store(true);
    if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lk(m_state->mtx);
            m_state->error = e.what();
            m_state->failed.store(true);
        } catch (...) {
            std::lock_guard<std::mutex> lk(m_state->mtx);
            m_state->error = "unknown error";
            m_state->failed.store(true);
        }
        eptr = nullptr; // handled: never let a planning failure take the app down
    }
    // Set last: the gizmo reads everything else once this flips.
    m_state->percent.store(100);
    m_state->done.store(true);
}

}} // namespace Slic3r::GUI
