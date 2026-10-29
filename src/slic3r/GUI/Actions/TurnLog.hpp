#ifndef slic3r_GUI_Actions_TurnLog_hpp_
#define slic3r_GUI_Actions_TurnLog_hpp_

// Turn-level undo (slicer.undoTurn). A page groups the calls one user request caused under a
// `turn` id it sends on every mutating call. Per bridge, the log records for each turn:
//   - the undo snapshots its calls took (timestamps on the Plater's undo stack), and
//   - the preset settings it wrote (type, key, the value before, the value written).
// undoTurn rolls the undo stack back to before the turn's first snapshot -- only while the
// turn's last snapshot is still the newest, so the user's later edits are never undone with it --
// and puts back each written setting whose current value is still what the turn wrote.
//
// Pure bookkeeping: no GUI. The runtime feeds it (ActionRuntime.cpp RequestScope).

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Actions {

class TurnLog
{
public:
    struct SettingWrite
    {
        std::string type;     // "print" | "filament" | "printer"
        std::string preset;   // the preset the write landed in
        std::string key;
        std::string previous; // .ini serialisation before the write
        std::string written;  // .ini serialisation the write produced
    };

    struct Turn
    {
        std::string               id;
        std::vector<size_t>       snapshots; // undo-stack timestamps, ascending
        std::vector<SettingWrite> settings;  // in write order; a key written twice keeps the FIRST previous
    };

    // Turn ids are the page's; at most kMaxTurns are remembered (oldest dropped).
    static constexpr size_t kMaxTurns    = 64;
    static constexpr size_t kMaxIdLength = 128;

    static bool valid_turn_id(const std::string& turn);

    // Snapshots a request of `turn` took: every timestamp in `after` that is not in `before`
    // (both: the real snapshots on the stack, without the topmost placeholder).
    void note_snapshots(const std::string& turn, const std::vector<size_t>& before, const std::vector<size_t>& after);
    void note_setting(const std::string& turn, SettingWrite write);

    const Turn* find(const std::string& turn) const;
    void        forget(const std::string& turn);
    void        clear() { m_turns.clear(); }

    // The undo decision, pure: given the stack's real snapshot timestamps now, may `turn` be rolled
    // back? True when its last snapshot is the newest real snapshot on the stack (nothing since).
    // `undo_to` receives the timestamp to load (the turn's first snapshot: the state before it).
    // A turn with no snapshots returns true with undo_to = 0 (nothing to roll back).
    bool can_roll_back(const std::string& turn, const std::vector<size_t>& stack_now, size_t& undo_to,
                       std::string& why) const;

private:
    Turn& touch(const std::string& turn);
    std::deque<Turn> m_turns;
};

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_TurnLog_hpp_
