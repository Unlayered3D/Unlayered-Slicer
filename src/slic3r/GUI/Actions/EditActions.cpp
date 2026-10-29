// Automation actions: edit.undo / edit.redo (Edit menu, Ctrl+Z / Ctrl+Y). Owner: S1 (actions core).
//
// They step the plate's undo stack exactly as the menu items do. They take no snapshot of their
// own, so they are not part of a turn's undo (slicer.undoTurn); a page undoing its own turn uses
// slicer.undoTurn, and edit.undo is for "undo that" about the user's own last edit.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "Register.hpp"

#include "../Plater.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

void register_edit_actions(Catalog& c)
{
    const nlohmann::json no_params = {{"type", "object"}, {"additionalProperties", false}, {"properties", nlohmann::json::object()}};
    const nlohmann::json example   = nlohmann::json::array({{{"target", nullptr}, {"params", nlohmann::json::object()}}});

    ActionDescriptor undo;
    undo.id       = "edit.undo";
    undo.label    = "Undo";
    undo.summary  = "Undo the last edit on the plate, as Edit > Undo does. Not itself undoable by slicer.undoTurn; use edit.redo.";
    undo.risk     = Risk::Mutate;
    undo.params   = no_params;
    undo.effects  = {"geometry", "structure", "plates", "paint"};
    undo.ui       = {"MainFrame.cpp::menu::Undo"};
    undo.examples = example;
    c.add(
        undo,
        [](InvokeContext& ctx) {
            Plater* p = ctx.plater();
            p->undo();
            return InvokeResult::ok({{"canUndo", p->can_undo()}, {"canRedo", p->can_redo()}});
        },
        [](const InvokeContext& ctx) -> std::string {
            Plater* p = ctx.plater();
            return p != nullptr && p->can_undo() ? std::string() : std::string("nothing to undo");
        });

    ActionDescriptor redo = undo;
    redo.id               = "edit.redo";
    redo.label            = "Redo";
    redo.summary          = "Redo the edit last undone, as Edit > Redo does.";
    redo.ui               = {"MainFrame.cpp::menu::Redo"};
    c.add(
        redo,
        [](InvokeContext& ctx) {
            Plater* p = ctx.plater();
            p->redo();
            return InvokeResult::ok({{"canUndo", p->can_undo()}, {"canRedo", p->can_redo()}});
        },
        [](const InvokeContext& ctx) -> std::string {
            Plater* p = ctx.plater();
            return p != nullptr && p->can_redo() ? std::string() : std::string("nothing to redo");
        });
}

}}} // namespace Slic3r::GUI::Actions
