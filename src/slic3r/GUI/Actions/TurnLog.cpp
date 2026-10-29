#include "TurnLog.hpp"

#include <algorithm>

namespace Slic3r { namespace GUI { namespace Actions {

bool TurnLog::valid_turn_id(const std::string& turn)
{
    if (turn.empty() || turn.size() > kMaxIdLength)
        return false;
    for (unsigned char c : turn)
        if (c < 0x20 || c == 0x7f)
            return false;
    return true;
}

TurnLog::Turn& TurnLog::touch(const std::string& turn)
{
    for (Turn& t : m_turns)
        if (t.id == turn)
            return t;
    if (m_turns.size() >= kMaxTurns)
        m_turns.pop_front();
    m_turns.push_back(Turn{turn, {}, {}});
    return m_turns.back();
}

void TurnLog::note_snapshots(const std::string& turn, const std::vector<size_t>& before, const std::vector<size_t>& after)
{
    if (!valid_turn_id(turn))
        return;
    std::vector<size_t> added;
    for (size_t ts : after)
        if (std::find(before.begin(), before.end(), ts) == before.end())
            added.push_back(ts);
    if (added.empty())
        return;
    Turn& t = touch(turn);
    for (size_t ts : added)
        if (std::find(t.snapshots.begin(), t.snapshots.end(), ts) == t.snapshots.end())
            t.snapshots.push_back(ts);
    std::sort(t.snapshots.begin(), t.snapshots.end());
}

void TurnLog::note_setting(const std::string& turn, SettingWrite write)
{
    if (!valid_turn_id(turn))
        return;
    Turn& t = touch(turn);
    for (SettingWrite& w : t.settings)
        if (w.type == write.type && w.preset == write.preset && w.key == write.key) {
            w.written = std::move(write.written); // keep the FIRST previous: undo goes back to before the turn
            return;
        }
    t.settings.push_back(std::move(write));
}

const TurnLog::Turn* TurnLog::find(const std::string& turn) const
{
    for (const Turn& t : m_turns)
        if (t.id == turn)
            return &t;
    return nullptr;
}

void TurnLog::forget(const std::string& turn)
{
    m_turns.erase(std::remove_if(m_turns.begin(), m_turns.end(), [&turn](const Turn& t) { return t.id == turn; }),
                  m_turns.end());
}

bool TurnLog::can_roll_back(const std::string& turn, const std::vector<size_t>& stack_now, size_t& undo_to,
                            std::string& why) const
{
    undo_to       = 0;
    const Turn* t = find(turn);
    if (t == nullptr) {
        why = "unknown turn";
        return false;
    }
    if (t->snapshots.empty())
        return true;
    // Every snapshot of the turn must still be on the stack (an undo past it, or a new edit after
    // an undo, drops them) and the turn's last one must be the newest: nothing happened since.
    for (size_t ts : t->snapshots)
        if (std::find(stack_now.begin(), stack_now.end(), ts) == stack_now.end()) {
            why = "the turn's edits are no longer on the undo stack";
            return false;
        }
    if (stack_now.empty() || stack_now.back() != t->snapshots.back()) {
        why = "there were later edits";
        return false;
    }
    undo_to = t->snapshots.front();
    return true;
}

}}} // namespace Slic3r::GUI::Actions
