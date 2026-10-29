#ifndef slic3r_GUI_Actions_ObjectListLogic_hpp_
#define slic3r_GUI_Actions_ObjectListLogic_hpp_

// Pure rules behind the object-list actions (ObjectActions.cpp): where an object or a part may move
// in the list order, when deleting parts or instances would make the object list refuse with a
// modal message, and how a plate move renumbers the plates. They mirror the ObjectList / PartPlate
// code they stand in front of, so a runner can refuse up front instead of letting the GUI pop a
// dialog the page cannot answer. No wx, no GUI_App: tests/slic3rutils/test_actions_objects.cpp
// exercises them directly.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace Actions { namespace objlist {

// ---------------------------------------------------------------------------------------------
// List order
// ---------------------------------------------------------------------------------------------

// ObjectList::can_move_volume over the parts' types: may part `from` take the place of part `to`?
// Parts move only within their own type group; slot 0 is kept for a solid part; support blockers,
// enforcers and infill meshes keep their place (except among their own type, as the list allows).
inline bool can_move_part(const std::vector<ModelVolumeType>& types, int from, int to)
{
    const int n = int(types.size());
    if (from < 0 || to < 0 || from >= n || to >= n)
        return false;
    if (from == to)
        return true;
    const ModelVolumeType ft = types[size_t(from)], tt = types[size_t(to)];
    if (ft == tt && ft != ModelVolumeType::MODEL_PART)
        return true;
    if (ft != tt || tt >= ModelVolumeType::SUPPORT_BLOCKER)
        return false;
    const bool only_one_solid_part = std::count(types.begin(), types.end(), ModelVolumeType::MODEL_PART) < 2;
    if (ft == ModelVolumeType::MODEL_PART) {
        if (only_one_solid_part)
            return false;
        return to == 0 || (from == 0 && n > 1 && types[1] == ModelVolumeType::MODEL_PART) ||
               (from != 0 && types[0] == ModelVolumeType::MODEL_PART);
    }
    return false;
}

// The slots (part indices, ascending, including its own) part `self` may occupy, as
// ObjectList::order_target lists them.
inline std::vector<int> part_slots(const std::vector<ModelVolumeType>& types, int self)
{
    std::vector<int> out;
    for (int i = 0; i < int(types.size()); ++i)
        if (i == self || can_move_part(types, self, i))
            out.push_back(i);
    return out;
}

// The model index an item moves to so that it ends at position `position` (0-based) of `slots`
// (the model indices of the items it shares an order with, ascending, its own included). -1 when
// the position is out of range.
inline int slot_at(const std::vector<int>& slots, int position)
{
    return (position >= 0 && position < int(slots.size())) ? slots[size_t(position)] : -1;
}

// The model index item `self` moves to so that it ends right before item `before` (both in
// `slots`); -1 when either is not in `slots` or they are the same item. When self is already right
// before it, that is self's own index (no move).
inline int slot_before(const std::vector<int>& slots, int self, int before)
{
    const auto s = std::find(slots.begin(), slots.end(), self);
    const auto b = std::find(slots.begin(), slots.end(), before);
    if (s == slots.end() || b == slots.end() || s == b)
        return -1;
    // Moving down: everything between shifts up one, so the item lands in the slot before `before`.
    return s < b ? *std::prev(b) : *b;
}

// ---------------------------------------------------------------------------------------------
// Deleting parts and instances
// ---------------------------------------------------------------------------------------------

enum class PartDeleteBlock
{
    None,
    LastSolidPart, // ObjectList::del_subobject_from_object shows "Deleting the last solid part is not allowed."
    CutObject,     // a solid part or negative volume of a cut object: the cut-info dialog would ask
};

// What stops deleting parts `which` (indices) of one object. `is_cut` = ModelObject::is_cut().
inline PartDeleteBlock part_delete_block(const std::vector<ModelVolumeType>& types, const std::vector<int>& which, bool is_cut)
{
    int solid = 0, solid_deleted = 0;
    bool cut_piece = false;
    for (int i = 0; i < int(types.size()); ++i) {
        const bool picked = std::find(which.begin(), which.end(), i) != which.end();
        if (types[size_t(i)] == ModelVolumeType::MODEL_PART) {
            ++solid;
            solid_deleted += picked ? 1 : 0;
        }
        if (picked && (types[size_t(i)] == ModelVolumeType::MODEL_PART || types[size_t(i)] == ModelVolumeType::NEGATIVE_VOLUME))
            cut_piece = true;
    }
    if (solid_deleted > 0 && solid_deleted >= solid)
        return PartDeleteBlock::LastSolidPart;
    if (is_cut && cut_piece)
        return PartDeleteBlock::CutObject;
    return PartDeleteBlock::None;
}

inline std::vector<ModelVolumeType> volume_types(const ModelObject& mo)
{
    std::vector<ModelVolumeType> out;
    out.reserve(mo.volumes.size());
    for (const ModelVolume* v : mo.volumes)
        out.push_back(v->type());
    return out;
}

// ---------------------------------------------------------------------------------------------
// Plates
// ---------------------------------------------------------------------------------------------

// PartPlateList::move_plate_to_index(from, to) as a permutation: element i of the result is the
// index plate i had before the move.
inline std::vector<int> plate_order_after_move(int count, int from, int to)
{
    std::vector<int> order;
    for (int i = 0; i < count; ++i)
        order.push_back(i);
    if (from < 0 || to < 0 || from >= count || to >= count || from == to)
        return order;
    const int moved = order[size_t(from)];
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, moved);
    return order;
}

}}}} // namespace Slic3r::GUI::Actions::objlist

#endif // slic3r_GUI_Actions_ObjectListLogic_hpp_
