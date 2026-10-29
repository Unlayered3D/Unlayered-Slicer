// Automation actions: selection.set and the slicer.getModelTree typed method (ModelTree.hpp).
// Owner: work package S2 (objects). See docs/HLSD/bridge-actions.md for how to write an action.
//
// slicer.getSelection / slicer.select themselves are bridge methods of the actions core
// (WebAppBridgeActions); selection.set is the menu verbs Select All / Select All Plates /
// Deselect All and "select exactly these objects", as one catalog action.

#include "libslic3r/Model.hpp"

#include "../GUI_App.hpp"
#include "../GUI_ObjectList.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../WebAppBridge.hpp"

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "ActionTargets.hpp"
#include "ModelTree.hpp"
#include "Register.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;

namespace {

json selection_json(const Model& model, const ResolvedTargets& t)
{
    json objects = json::array(), volumes = json::array(), instances = json::array();
    for (int o : t.objects)
        objects.push_back(object_ref_json(model, o));
    for (const auto& [o, v] : t.volumes)
        volumes.push_back(volume_ref_json(model, o, v));
    for (const auto& [o, i] : t.instances)
        if (o >= 0 && o < int(model.objects.size()) && i >= 0 && i < int(model.objects[o]->instances.size()))
            instances.push_back({{"id", model.objects[o]->instances[i]->id().id}, {"object", object_ref_json(model, o)}, {"index", i}});
    return {{"objects", objects}, {"volumes", volumes}, {"instances", instances}};
}

InvokeResult run_get_model_tree(InvokeContext& ctx)
{
    Plater* p     = ctx.plater();
    const int plate = ctx.params.value("plate", -1);
    if (p == nullptr)
        return InvokeResult::ok(build_model_tree(ctx.model(), nullptr, plate));
    PartPlateList& plates = p->get_partplate_list();
    if (plate >= plates.get_plate_count())
        return InvokeResult::error(WAB_NOT_FOUND, "no such plate", {{"path", "/params/plate"}, {"plateCount", plates.get_plate_count()}});
    return InvokeResult::ok(build_model_tree(p->model(), &plates, plate));
}

InvokeResult run_selection_set(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the plate is not ready");
    const std::string mode = ctx.params.value("mode", std::string("target"));
    if (mode == "target") {
        // The runtime has already selected exactly the target: objects, parts or instances, as a
        // click (Alt-click for a part) or a row selection in the object list does.
        if (ctx.targets.objects.empty() && ctx.targets.volumes.empty() && ctx.targets.instances.empty())
            return InvokeResult::error(WAB_BAD_PARAMS, "mode \"target\" needs a target (objects, volumes or instances to select)",
                                       {{"path", "/target"}, {"needs", "target"}});
    } else if (mode == "none") {
        p->deselect_all();
        if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
            list->update_selections();
    } else if (mode == "plate") {
        PartPlateList& plates = p->get_partplate_list();
        const int      asked  = ctx.params.value("plate", -1);
        const int      plate  = asked >= 0 ? asked : plates.get_curr_plate_index();
        if (plate >= plates.get_plate_count())
            return InvokeResult::error(WAB_NOT_FOUND, "no such plate", {{"plateCount", plates.get_plate_count()}});
        if (plate != plates.get_curr_plate_index())
            p->select_plate(plate);
        p->select_curr_plate_all();
    } else // "all": every object on every plate
        p->select_all();
    return InvokeResult::ok({{"mode", mode}, {"selection", selection_json(p->model(), current_selection())}});
}

std::string needs_objects(const InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return "the plate is not ready";
    return p->model().objects.empty() ? "there are no objects" : "";
}

} // namespace

void register_selection_actions(Catalog& c)
{
    {
        ActionDescriptor d;
        d.id      = "model.getTree";
        d.label   = "Model tree";
        d.summary = "Every object on the plates with its stable id, instances (position, rotation, scale), parts (type, filament, "
                    "paint), per-object setting overrides, layer ranges and cut planes.";
        d.risk    = Risk::Read;
        d.target  = {{"kind", "none"}, {"required", false}};
        d.params  = {{"type", "object"},
                     {"additionalProperties", false},
                     {"properties",
                      {{"plate", {{"type", "integer"}, {"minimum", -1}, {"maximum", 1000}, {"default", -1},
                                  {"description", "Only the objects on this plate (0-based); -1 = every plate."}}}}}};
        d.bridge_method = "slicer.getModelTree";
        d.examples      = json::array({{{"target", nullptr}, {"params", json::object()}}, {{"target", nullptr}, {"params", {{"plate", 0}}}}});
        c.add(std::move(d), run_get_model_tree);
    }
    {
        ActionDescriptor d;
        d.id      = "selection.set";
        d.label   = "Select";
        d.summary = "Select exactly the target objects, parts (target.volumes) or instances (target.instances), every object on a "
                    "plate, every object on every plate, or nothing.";
        d.risk    = Risk::Ui;
        d.target  = {{"kind", "objects"}, {"required", false}, {"accepts", json::array({"volumes", "instances"})}};
        d.params  = {{"type", "object"},
                     {"additionalProperties", false},
                     {"properties",
                      {{"mode", {{"type", "string"},
                                 {"enum", {"target", "plate", "all", "none"}},
                                 {"default", "target"},
                                 {"description", "target: the target objects, parts or instances; plate: all on a plate; all: all plates; "
                                                 "none: deselect."}}},
                       {"plate", {{"type", "integer"}, {"minimum", -1}, {"maximum", 1000}, {"default", -1},
                                  {"description", "mode plate: which plate (0-based); -1 = the current one."}}}}}};
        d.effects       = {"selection"};
        d.ui            = {"MainFrame.cpp::menu::Select All", "MainFrame.cpp::menu::Deselect All", "MainFrame.cpp::menu::Esc"};
        d.examples      = json::array({{{"target", {{"objects", json::array({{{"index", 0}}})}}}, {"params", {{"mode", "target"}}}},
                                       {{"target", {{"volumes", json::array({{{"object", {{"index", 0}}}, {"index", 0}}})}}},
                                        {"params", {{"mode", "target"}}}},
                                       {{"target", nullptr}, {"params", {{"mode", "all"}}}},
                                       {{"target", nullptr}, {"params", {{"mode", "none"}}}}});
        c.add(std::move(d), run_selection_set, needs_objects);
    }
}

}}} // namespace Slic3r::GUI::Actions
