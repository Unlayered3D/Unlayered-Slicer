// Automation actions: object.* structure actions, plate.clear and edit.cut / edit.copy / edit.paste.
// Owner: work package S2 (objects). See docs/HLSD/bridge-actions.md for how to write an action and
// docs/bridge-actions/objects.md for what each of these does.
//
// Every runner here acts on what the runtime resolved and SELECTED (ctx.targets), then calls the
// same Plater / ObjectList / Selection function the menu item calls, inside one "(automated)"
// snapshot with the callee's own snapshots suppressed, so one action is exactly one undo step.
// Where the menu asks something in a dialog (clone count, rename, split's "keep Z?", delete of a
// cut part), the answer is a parameter, and conditions that would make the callee pop a message
// box are refused up front with the reason instead.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Format/bbs_3mf.hpp" // save_object_mesh
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "../GLCanvas3D.hpp"
#include "BedDrop.hpp"
#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../GUI_ObjectList.hpp"
#include "../Jobs/Job.hpp"
#include "../Jobs/Worker.hpp"
#include "../ObjectDataViewModel.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../Selection.hpp"
#include "../WebAppBridge.hpp"

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "ActionTargets.hpp"
#include "ObjectListLogic.hpp"
#include "PartsCommon.hpp"
#include "Register.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;
namespace fs = boost::filesystem;

namespace {

// ── Descriptor building ───────────────────────────────────────────────────────────────────────

json params_schema(json properties = json::object(), std::vector<std::string> required = {})
{
    json s = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = std::move(required);
    return s;
}

json target_spec(const char* kind, bool required) { return {{"kind", kind}, {"required", required}}; }

// An object action that also takes parts and/or single instances (ActionTargets.hpp "accepts").
json target_spec_accepting(const char* kind, bool required, std::vector<std::string> accepts)
{
    json t       = target_spec(kind, required);
    t["accepts"] = std::move(accepts);
    return t;
}

json first_part() { return {{"volumes", json::array({{{"object", {{"index", 0}}}, {"index", 0}}})}}; }

json first_instance() { return {{"instances", json::array({{{"object", {{"index", 0}}}, {"index", 0}}})}}; }

json vec3_schema(const std::string& description, double min = -1e5, double max = 1e5)
{
    return {{"type", "array"},
            {"minItems", 3},
            {"maxItems", 3},
            {"items", {{"type", "number"}, {"minimum", min}, {"maximum", max}}},
            {"description", description}};
}

json example(json target, json params = json::object()) { return {{"target", std::move(target)}, {"params", std::move(params)}}; }

json first_object() { return {{"objects", json::array({{{"index", 0}}})}}; }

ActionDescriptor make(const std::string& id, const std::string& label, const std::string& summary, Risk risk, json target,
                      json params, std::vector<std::string> effects, std::vector<std::string> ui, json examples)
{
    ActionDescriptor d;
    d.id       = id;
    d.label    = label;
    d.summary  = summary;
    d.risk     = risk;
    d.target   = std::move(target);
    d.params   = std::move(params);
    d.effects  = std::move(effects);
    d.ui       = std::move(ui);
    d.examples = std::move(examples);
    return d;
}

// ── Runner helpers ────────────────────────────────────────────────────────────────────────────

InvokeResult no_plater() { return InvokeResult::error(WAB_INTERNAL, "the plate is not ready"); }

InvokeResult disabled(const std::string& reason) { return InvokeResult::error(WAB_ACTION_DISABLED, reason, {{"reason", reason}}); }

InvokeResult bad_param(const std::string& path, const std::string& message, json extra = json::object())
{
    extra["path"] = path;
    return InvokeResult::error(WAB_BAD_PARAMS, message, std::move(extra));
}

// The 3D scene's selection: what the runtime selected and what the Plater functions act on.
Selection& scene_selection(Plater* p) { return p->get_view3D_canvas3D()->get_selection(); }

json object_refs(const Model& model, const std::vector<int>& idxs)
{
    json out = json::array();
    for (int o : idxs)
        if (o >= 0 && o < int(model.objects.size()))
            out.push_back(object_ref_json(model, o));
    return out;
}

json volume_refs(const Model& model, const std::vector<std::pair<int, int>>& vols)
{
    json out = json::array();
    for (const auto& [o, v] : vols)
        if (json r = volume_ref_json(model, o, v); !r.is_null())
            out.push_back(std::move(r));
    return out;
}

json instance_ref_json(const Model& model, int o, int i)
{
    if (o < 0 || o >= int(model.objects.size()) || i < 0 || i >= int(model.objects[o]->instances.size()))
        return nullptr;
    return {{"id", model.objects[o]->instances[i]->id().id}, {"index", i}, {"object", object_ref_json(model, o)}};
}

// The ids of every part of every object, by object id: what a paste or a delete added or removed.
std::map<size_t, std::set<size_t>> volume_ids_by_object(const Model& model)
{
    std::map<size_t, std::set<size_t>> out;
    for (const ModelObject* mo : model.objects)
        for (const ModelVolume* v : mo->volumes)
            out[mo->id().id].insert(v->id().id);
    return out;
}

// Indices of the objects added since `count_before` (new objects are appended).
std::vector<int> appended_since(const Model& model, size_t count_before)
{
    std::vector<int> out;
    for (size_t o = count_before; o < model.objects.size(); ++o)
        out.push_back(int(o));
    return out;
}

// After a direct model edit: the plate list re-files every instance of the object and the object
// list's plate column follows (what GLCanvas3D::do_move does after a gizmo move).
void refile_object(Plater* p, int obj_idx)
{
    PartPlateList& plates = p->get_partplate_list();
    for (size_t i = 0; i < p->model().objects[obj_idx]->instances.size(); ++i)
        plates.notify_instance_update(obj_idx, int(i));
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
        list->update_plate_values_for_items();
}

double deg(double rad) { return Geometry::rad2deg(rad); }

json instance_state(const ModelObject& mo, size_t i)
{
    const ModelInstance* mi  = mo.instances[i];
    const Vec3d          off = mi->get_offset(), rot = mi->get_rotation(), sc = mi->get_scaling_factor();
    const BoundingBoxf3  bb  = mo.instance_bounding_box(i);
    return {{"index", int(i)},
            {"id", mi->id().id},
            {"offset", {off.x(), off.y(), off.z()}},
            {"rotationDeg", {deg(rot.x()), deg(rot.y()), deg(rot.z())}},
            {"scale", {sc.x(), sc.y(), sc.z()}},
            {"bboxMm", {{"min", {bb.min.x(), bb.min.y(), bb.min.z()}}, {"max", {bb.max.x(), bb.max.y(), bb.max.z()}}}}};
}

bool read_vec3(const json& params, const char* key, Vec3d& out)
{
    if (!params.contains(key) || !params[key].is_array() || params[key].size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!params[key][i].is_number())
            return false;
        out[i] = params[key][i].get<double>();
        if (!std::isfinite(out[i]))
            return false;
    }
    return true;
}

bool has(const json& params, const char* key) { return params.contains(key) && !params[key].is_null(); }

// Rotation about the instance's bounding-box centre, so it stays where it sits on the plate.
void rotate_instance_about_center(ModelObject& mo, size_t i, const Matrix3d& rot)
{
    const Vec3d centre = mo.instance_bounding_box(i).center();
    Transform3d about  = Transform3d::Identity();
    about.translate(centre);
    about.rotate(rot);
    about.translate(-centre);
    mo.instances[i]->set_transformation(Geometry::Transformation(about * mo.instances[i]->get_matrix()));
}

// The world-frame normal of the largest flat face of the instance's convex hull (what the
// Flatten gizmo offers first): facets grouped by normal within ~1 degree, largest total area wins.
bool largest_face_normal(const ModelObject& mo, size_t inst, Vec3d& out_normal)
{
    TriangleMesh mesh = mo.raw_mesh();
    if (mesh.empty())
        return false;
    TriangleMesh hull = mesh.convex_hull_3d();
    Transform3d  lin  = Transform3d::Identity();
    lin.linear()      = mo.instances[inst]->get_matrix().linear();
    hull.transform(lin);

    struct Group { Vec3d normal_sum; double area; };
    std::vector<Group> groups;
    const auto&        its = hull.its;
    for (const auto& f : its.indices) {
        const Vec3d  a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(), c = its.vertices[f[2]].cast<double>();
        const Vec3d  cr   = (b - a).cross(c - a);
        const double area = 0.5 * cr.norm();
        if (area < 1e-9)
            continue;
        const Vec3d n = cr.normalized();
        Group*      g = nullptr;
        for (Group& cand : groups)
            if (cand.normal_sum.normalized().dot(n) > 0.99985) { // ~1 degree
                g = &cand;
                break;
            }
        if (g == nullptr) {
            groups.push_back({Vec3d::Zero(), 0.});
            g = &groups.back();
        }
        g->normal_sum += n * area;
        g->area += area;
    }
    if (groups.empty())
        return false;
    const auto best = std::max_element(groups.begin(), groups.end(), [](const Group& l, const Group& r) { return l.area < r.area; });
    out_normal      = best->normal_sum.normalized();
    return true;
}

// Selection-based Plater edits run with the callee's own snapshots suppressed: the runner already
// took the one "(automated)" snapshot.
template<class F> void quietly(Plater* p, F&& f)
{
    Plater::SuppressSnapshots suppress(p);
    f();
}

// ── Runners ───────────────────────────────────────────────────────────────────────────────────

InvokeResult run_rename(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    const int         o    = ctx.targets.objects.front();
    const std::string name = ctx.params.value("name", std::string());
    if (name.empty() || name.find_first_not_of(" \t") == std::string::npos)
        return bad_param("/params/name", "name must not be blank");
    if (Plater::has_illegal_filename_characters(name))
        return bad_param("/params/name", "name contains characters that are not allowed in a file name (\\ / : * ? \" < > |)");

    ModelObject* mo = ctx.model().objects[o];
    const std::string before = mo->name;
    ctx.snapshot("Rename Object");
    mo->name = name;
    if (mo->volumes.size() == 1)
        mo->volumes[0]->name = name; // as ObjectList::update_name_in_model
    Slic3r::save_object_mesh(*mo);
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr) {
        if (const wxDataViewItem item = list->GetModel()->GetItemById(o); item.IsOk())
            list->GetModel()->SetName(from_u8(name), item);
        list->Refresh();
    }
    p->set_current_canvas_as_dirty();
    json r = object_ref_json(ctx.model(), o);
    r["previousName"] = before;
    return InvokeResult::ok({{"object", r}});
}

InvokeResult run_delete(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    Model&     model     = ctx.model();
    const bool break_cut = ctx.params.value("breakCutLink", false);
    json       cut       = json::array();
    for (int o : ctx.targets.objects)
        if (model.objects[o]->is_cut())
            cut.push_back(object_ref_json(model, o));
    // Deleting one piece of a cut object asks the person (InfoDialog) because it breaks the link
    // between the pieces; here that answer is the breakCutLink param.
    if (!cut.empty() && !break_cut)
        return InvokeResult::error(WAB_BAD_PARAMS,
                                   "these objects are pieces of a cut object; deleting them breaks the cut link between the pieces. "
                                   "Retry with breakCutLink: true to delete anyway",
                                   {{"path", "/params/breakCutLink"}, {"needs", "breakCutLink"}, {"cutObjects", cut}});
    if (!p->can_delete())
        return disabled("the selection cannot be deleted now");

    const json deleted = object_refs(model, ctx.targets.objects);
    const size_t before = model.objects.size();
    ctx.snapshot("Delete Selected Objects");
    quietly(p, [&] {
        if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
            for (int o : ctx.targets.objects)
                if (model.objects[o]->is_cut())
                    list->invalidate_cut_info_for_object(o);
        p->remove_selected();
    });
    if (model.objects.size() == before)
        return InvokeResult::error(WAB_INTERNAL, "nothing was deleted");
    return InvokeResult::ok({{"deleted", deleted}, {"objectCount", model.objects.size()}});
}

// Renaming a part in the object list (in-place edit of a part row, or the Parameter Table's name
// cell): ObjectList::update_name_in_model for a volume item. Text and SVG parts take a name from
// their text / file again on the next text.edit / svg.edit.
InvokeResult run_part_rename(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    if (ctx.targets.volumes.size() != 1)
        return bad_param("/target/volumes", "name exactly one part");
    const auto [o, v]      = ctx.targets.volumes.front();
    const std::string name = ctx.params.value("name", std::string());
    if (name.empty() || name.find_first_not_of(" \t") == std::string::npos)
        return bad_param("/params/name", "name must not be blank");
    if (Plater::has_illegal_filename_characters(name))
        return bad_param("/params/name", "name contains characters that are not allowed in a file name (\\ / : * ? \" < > |)");

    ModelVolume*      mv     = ctx.model().objects[o]->volumes[v];
    const std::string before = mv->name;
    if (before == name)
        return InvokeResult::ok({{"volume", volume_ref_json(ctx.model(), o, v)}, {"previousName", before}}).with_effects({});
    ctx.snapshot("Rename Part");
    mv->name = name;
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr) {
        if (const wxDataViewItem item = list->GetModel()->GetItemByVolumeId(o, v); item.IsOk())
            list->GetModel()->SetName(from_u8(name), item);
        list->Refresh();
    }
    p->set_current_canvas_as_dirty();
    json r            = volume_ref_json(ctx.model(), o, v);
    r["previousName"] = before;
    return InvokeResult::ok({{"volume", r}});
}

// Del on a part / modifier / negative / enforcer row, or on a part selected in the 3D scene: the
// selection's delete (Selection::erase -> ObjectList::delete_from_model_and_list), which removes the
// parts from the highest index down and moves a last part's settings onto its object. The two
// conditions under which the list would pop a modal message are refused first: the last solid part
// of an object, and a solid part / negative volume of a cut object (whose dialog's answer is
// breakCutLink).
InvokeResult run_part_delete(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    ObjectList* list = wxGetApp().obj_list();
    if (list == nullptr)
        return no_plater();
    Model&     model     = ctx.model();
    const bool break_cut = ctx.params.value("breakCutLink", false);

    std::map<int, std::vector<int>> by_object;
    for (const auto& [o, v] : ctx.targets.volumes)
        by_object[o].push_back(v);
    json cut = json::array();
    for (const auto& [o, vols] : by_object) {
        const ModelObject* mo = model.objects[o];
        switch (objlist::part_delete_block(objlist::volume_types(*mo), vols, mo->is_cut())) {
        case objlist::PartDeleteBlock::LastSolidPart:
            return disabled("deleting the last solid part of '" + mo->name + "' is not allowed; delete the object instead (object.delete)");
        case objlist::PartDeleteBlock::CutObject: cut.push_back(object_ref_json(model, o)); break;
        case objlist::PartDeleteBlock::None: break;
        }
    }
    if (!cut.empty() && !break_cut)
        return InvokeResult::error(WAB_BAD_PARAMS,
                                   "these parts belong to a cut object; deleting a solid part or negative volume breaks the cut link "
                                   "between its pieces. Retry with breakCutLink: true to delete anyway",
                                   {{"path", "/params/breakCutLink"}, {"needs", "breakCutLink"}, {"cutObjects", cut}});
    if (!p->can_delete())
        return disabled("the selection cannot be deleted now");

    const json deleted = volume_refs(model, ctx.targets.volumes);
    std::vector<size_t> object_ids;
    for (const auto& entry : by_object)
        object_ids.push_back(model.objects[entry.first]->id().id);
    ctx.snapshot("Delete part");
    quietly(p, [&] {
        for (const auto& entry : by_object)
            if (model.objects[entry.first]->is_cut())
                list->invalidate_cut_info_for_object(entry.first);
        p->remove_selected();
    });
    json objects = json::array();
    for (size_t id : object_ids)
        if (const int o = object_index_by_id(model, id); o >= 0) {
            json r     = object_ref_json(model, o);
            r["parts"] = model.objects[o]->volumes.size();
            objects.push_back(r);
        }
    return InvokeResult::ok({{"deleted", deleted}, {"objects", objects}, {"cutLinkBroken", !cut.empty()}});
}

// Clone (Edit > Clone Selected, the CloneDialog): copies of the selection as new objects, then an
// optional arrange of the current plate.
InvokeResult run_clone(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    if (!p->get_ui_job_worker().is_idle())
        return InvokeResult::error(WAB_BUSY, "an arrange or fill-bed job is running");
    Model&       model   = ctx.model();
    const int    count   = ctx.params.value("count", 1);
    const bool   arrange = ctx.params.value("arrange", false);
    const size_t before  = model.objects.size();

    ctx.snapshot("Clone");
    quietly(p, [&] {
        // Selection::clone is the dialog's loop: copy to the 3D clipboard, paste `count` times.
        scene_selection(p).clone(count);
    });
    const std::vector<int> added = appended_since(model, before);
    if (added.empty())
        return InvokeResult::error(WAB_INTERNAL, "nothing was cloned");
    if (!arrange)
        return InvokeResult::ok({{"added", object_refs(model, added)}, {"arranged", false}});

    // Arrange the current plate as the dialog's checkbox does; its "Arrange" snapshot is folded
    // into this action's by the suppression.
    std::vector<size_t> added_ids;
    for (int o : added)
        added_ids.push_back(model.objects[o]->id().id);
    const int saved_state = p->get_prepare_state();
    quietly(p, [&] {
        p->set_prepare_state(Job::PREPARE_STATE_MENU);
        p->arrange();
    });
    return ctx.after_plate_jobs([added_ids, saved_state](InvokeContext& c, bool timed_out) {
        if (Plater* pl = c.plater(); pl != nullptr)
            pl->set_prepare_state(saved_state);
        if (timed_out)
            return InvokeResult::error(WAB_INTERNAL, "arrange did not finish in time; the copies were added");
        json refs = json::array();
        for (size_t id : added_ids)
            if (const int o = object_index_by_id(c.model(), id); o >= 0)
                refs.push_back(object_ref_json(c.model(), o));
        return InvokeResult::ok({{"added", refs}, {"arranged", true}});
    });
}

InvokeResult run_add_instance(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    const int o     = ctx.targets.objects.front();
    const int count = ctx.params.value("count", 1);
    if (!p->can_increase_instances())
        return disabled("instances cannot be added to this object now (a cut object, or a gizmo is open)");
    if (int(ctx.model().objects[o]->instances.size()) + count > 1000)
        return bad_param("/params/count", "an object may have at most 1000 instances");
    ctx.snapshot("Increase Instances");
    quietly(p, [&] { p->increase_instances(size_t(count)); });
    const ModelObject* mo = ctx.model().objects[o];
    return InvokeResult::ok({{"object", object_ref_json(ctx.model(), o)}, {"instances", mo->instances.size()}});
}

// Del on instance rows / instances selected in the 3D scene: exactly those instances go
// (Selection::erase -> ObjectList::delete_from_model_and_list, highest index first). Deleting
// every instance of an object would delete the object, which is object.delete's job, and the list
// refuses the last one with a modal message; both are refused here.
InvokeResult remove_named_instances(InvokeContext& ctx, Plater* p)
{
    Model&                      model = ctx.model();
    std::map<int, std::set<int>> by_object;
    for (const auto& [o, i] : ctx.targets.instances)
        by_object[o].insert(i);
    for (const auto& [o, picked] : by_object) {
        const ModelObject* mo = model.objects[o];
        if (picked.size() >= mo->instances.size())
            return bad_param("/target/instances", "'" + mo->name + "' would lose every instance; at least one must stay (use object.delete to remove the object)");
    }
    if (!p->can_delete())
        return disabled("the selection cannot be deleted now");
    json removed = json::array();
    for (const auto& [o, i] : ctx.targets.instances)
        removed.push_back(instance_ref_json(model, o, i));
    std::vector<size_t> ids;
    for (const auto& entry : by_object)
        ids.push_back(model.objects[entry.first]->id().id);
    ctx.snapshot("Delete instance");
    quietly(p, [&] { p->remove_selected(); });
    // Instance indices shifted: re-file every instance on its plate, as after a move.
    p->get_partplate_list().reload_all_objects(false);
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
        list->update_plate_values_for_items();
    json objects = json::array();
    for (size_t id : ids)
        if (const int o = object_index_by_id(model, id); o >= 0) {
            json r         = object_ref_json(model, o);
            r["instances"] = model.objects[o]->instances.size();
            objects.push_back(r);
        }
    return InvokeResult::ok({{"removed", removed}, {"objects", objects}});
}

InvokeResult run_remove_instance(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    if (!ctx.targets.instances.empty()) {
        if (!ctx.targets.objects.empty() || !ctx.targets.volumes.empty())
            return bad_param("/target", "name either one object (count removes its last instances) or the instances to delete");
        if (ctx.params.contains("count") && ctx.params["count"] != 1)
            return bad_param("/params/count", "count goes with an object target; with target.instances exactly the named ones go");
        return remove_named_instances(ctx, p);
    }
    if (ctx.targets.objects.size() != 1)
        return bad_param("/target/objects", "name exactly one object, or the instances to delete in target.instances");
    const int    o     = ctx.targets.objects.front();
    const int    count = ctx.params.value("count", 1);
    const size_t have  = ctx.model().objects[o]->instances.size();
    // Removing the last instance deletes the object (Plater::decrease_instances); that is
    // object.delete's job, so it is refused here rather than done silently.
    if (size_t(count) >= have)
        return bad_param("/params/count",
                         "the object has " + std::to_string(have) + " instance(s); at least one must stay (use object.delete to remove the object)");
    if (!p->can_decrease_instances())
        return disabled("instances cannot be removed from this object now");
    ctx.snapshot("Decrease Instances");
    quietly(p, [&] { p->decrease_instances(size_t(count)); });
    return InvokeResult::ok({{"object", object_ref_json(ctx.model(), o)}, {"instances", ctx.model().objects[o]->instances.size()}});
}

// "Set as An Individual Object": instances become objects of their own.
InvokeResult run_instances_to_objects(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    ObjectList* list = wxGetApp().obj_list();
    if (list == nullptr)
        return no_plater();
    Model&       model = ctx.model();
    const int    o     = ctx.targets.objects.front();
    const size_t n     = model.objects[o]->instances.size();
    if (n < 2)
        return disabled("the object has a single instance");

    std::set<int> picked;
    if (has(ctx.params, "instances"))
        for (const json& v : ctx.params["instances"]) {
            const int i = v.get<int>();
            if (i < 0 || size_t(i) >= n)
                return bad_param("/params/instances", "instance index out of range (the object has " + std::to_string(n) + ")");
            picked.insert(i);
        }

    const size_t before  = model.objects.size();
    const size_t keep_id = model.objects[o]->id().id;
    ctx.snapshot("Instances to Separated Objects");
    quietly(p, [&] {
        if (picked.empty() || picked.size() == n)
            list->instances_to_separated_objects(o);
        else
            list->instances_to_separated_object(o, picked);
    });
    p->update();
    std::vector<int> result;
    if (const int still = object_index_by_id(model, keep_id); still >= 0)
        result.push_back(still);
    for (int a : appended_since(model, before))
        result.push_back(a);
    return InvokeResult::ok({{"objects", object_refs(model, result)}, {"added", int(model.objects.size() - before)}});
}

// What a selection-based placement edit (center, drop, mirror) reports: the objects, parts and
// instances it acted on, each with where it is now.
json placed_json(const Model& model, const ResolvedTargets& t, bool with_mirror)
{
    json objects = json::array(), volumes = json::array(), instances = json::array();
    for (int o : t.objects) {
        json r = object_ref_json(model, o);
        r["instances"] = {instance_state(*model.objects[o], 0)};
        r["minZ"]      = model.objects[o]->instance_bounding_box(0).min.z();
        if (with_mirror)
            r["mirrored"] = model.objects[o]->instances.front()->is_left_handed();
        objects.push_back(r);
    }
    for (const auto& [o, v] : t.volumes) {
        json r              = volume_ref_json(model, o, v);
        const Vec3d off     = model.objects[o]->volumes[v]->get_offset();
        r["offset"]         = {off.x(), off.y(), off.z()};
        if (with_mirror)
            r["mirrored"] = model.objects[o]->volumes[v]->get_transformation().is_left_handed();
        volumes.push_back(r);
    }
    for (const auto& [o, i] : t.instances) {
        json r = instance_state(*model.objects[o], size_t(i));
        r["object"] = object_ref_json(model, o);
        if (with_mirror)
            r["mirrored"] = model.objects[o]->instances[i]->is_left_handed();
        instances.push_back(r);
    }
    return {{"objects", objects}, {"volumes", volumes}, {"instances", instances}};
}

// Center / Drop / Mirror act on the selection, whatever it is: objects (the object menu), a part
// (the part menu, the 3D scene with a part selected) or single instances.
InvokeResult run_center(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    ctx.snapshot("Move object");
    quietly(p, [&] { p->center_selection(); });
    return InvokeResult::ok(placed_json(ctx.model(), ctx.targets, false));
}

InvokeResult run_drop(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    ctx.snapshot("Drop object");
    quietly(p, [&] { p->drop_selection(); });
    return InvokeResult::ok(placed_json(ctx.model(), ctx.targets, false));
}

// Auto Drop, set explicitly rather than toggled (ObjectList::toggle_auto_drop).
InvokeResult run_set_auto_drop(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    const bool enabled = ctx.params.value("enabled", true);
    ctx.snapshot("Auto Drop");
    for (int o : ctx.targets.objects) {
        ModelObject* mo = ctx.model().objects[o];
        for (ModelInstance* mi : mo->instances)
            mi->auto_drop = enabled;
        if (enabled)
            mo->ensure_on_bed();
    }
    quietly(p, [&] {
        p->update();
        p->reload_paint_after_background_process_apply();
    });
    for (int o : ctx.targets.objects)
        refile_object(p, o);
    return InvokeResult::ok({{"objects", object_refs(ctx.model(), ctx.targets.objects)}, {"autoDrop", enabled}});
}

// Printable, set explicitly rather than toggled: the object list's Printable column / 'p' / the
// canvas 'V' (ObjectList::toggle_printable_state), which acts on object rows (every instance) and on
// single instance rows. The GUI toggles from the first selected row's state; here the state is a
// param.
InvokeResult run_set_printable(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    const bool printable = ctx.params.value("printable", true);
    Model&     model     = ctx.model();
    std::set<std::pair<int, int>> which;
    for (int o : ctx.targets.objects)
        for (size_t i = 0; i < model.objects[o]->instances.size(); ++i)
            which.insert({o, int(i)});
    for (const auto& oi : ctx.targets.instances)
        which.insert(oi);
    if (which.empty())
        return bad_param("/target", "name the objects or instances to change");

    ctx.snapshot(printable ? "Set Printable" : "Set Unprintable");
    std::set<int> objs;
    for (const auto& [o, i] : which) {
        model.objects[o]->instances[i]->printable = printable;
        objs.insert(o);
    }
    std::vector<ModelObject*> changed;
    for (int o : objs)
        changed.push_back(model.objects[o]);
    quietly(p, [&] {
        if (ObjectList* list = wxGetApp().obj_list(); list != nullptr) {
            list->printable_state_changed(changed); // list icons, canvas printable state, Plater::update
            for (int o : objs)
                for (size_t i = 0; i < model.objects[o]->instances.size(); ++i)
                    list->update_printable_state(o, int(i));
        } else
            p->update();
        p->reload_paint_after_background_process_apply();
    });
    json instances = json::array();
    for (const auto& oi : ctx.targets.instances)
        instances.push_back(instance_ref_json(model, oi.first, oi.second));
    return InvokeResult::ok({{"objects", object_refs(model, ctx.targets.objects)}, {"instances", instances}, {"printable", printable}});
}

// Hide / Show (assembly view): a display state of the 3D scene, not of the model -- nothing to undo.
InvokeResult run_set_visible(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    const bool  visible = ctx.params.value("visible", true);
    GLCanvas3D* canvas  = p->get_current_canvas3D();
    if (canvas == nullptr)
        return no_plater();
    canvas->set_selected_visible(visible);
    canvas->set_as_dirty();
    canvas->request_extra_frame();
    return InvokeResult::ok({{"objects", object_refs(ctx.model(), ctx.targets.objects)}, {"visible", visible}});
}

InvokeResult run_mirror(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    const std::string axis = ctx.params.value("axis", std::string("x"));
    if (!p->can_mirror())
        return disabled("the selection cannot be mirrored now: a cut object, or several instances (name single instances in "
                        "target.instances, or one part in target.volumes)");
    ctx.snapshot("Mirror");
    quietly(p, [&] { p->mirror(axis == "x" ? X : axis == "y" ? Y : Z); });
    json out    = placed_json(ctx.model(), ctx.targets, true);
    out["axis"] = axis;
    return InvokeResult::ok(out);
}

// "before": an object reference {id}|{index}|{name} in params. -1 + message when it names nothing.
int object_param(const Model& model, const json& ref, std::string& why)
{
    int out = -1;
    if (ref.contains("id"))
        out = object_index_by_id(model, ref["id"].get<size_t>());
    else if (ref.contains("index")) {
        const int i = ref["index"].get<int>();
        out         = (i >= 0 && i < int(model.objects.size())) ? i : -1;
    } else if (ref.contains("name")) {
        const std::string name = ref["name"].get<std::string>();
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (model.objects[i]->name == name) {
                if (out >= 0) {
                    why = "more than one object is named '" + name + "'; use its id";
                    return -1;
                }
                out = int(i);
            }
    }
    if (out < 0 && why.empty())
        why = "no such object";
    return out;
}

// Object list order (the Order submenu, and dragging a row by its grip): an object among the objects
// of its plate, or a part among the parts of its object under the list's rules (parts stay within
// their type group, the first slot is kept for a solid part, blockers and enforcers stay put).
InvokeResult run_order(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    ObjectList* list = wxGetApp().obj_list();
    if (list == nullptr)
        return no_plater();
    Model&     model  = ctx.model();
    const bool is_part = !ctx.targets.volumes.empty();
    if (ctx.targets.objects.size() + ctx.targets.volumes.size() + ctx.targets.instances.size() != 1 || !ctx.targets.instances.empty())
        return bad_param("/target", "name exactly one object (target.objects) or one part (target.volumes)");
    using OrderMove = ObjectList::OrderMove;

    if (ctx.params.contains("move")) {
        const std::string move_s = ctx.params["move"].get<std::string>();
        const OrderMove   move   = move_s == "top" ? OrderMove::Top : move_s == "bottom" ? OrderMove::Bottom : move_s == "down" ? OrderMove::Down : OrderMove::Up;
        if (!list->can_move_in_order(move))
            return disabled(std::string("the ") + (is_part ? "part" : "object") + " is already " +
                            (move == OrderMove::Top || move == OrderMove::Up ? "first" : "last") +
                            (is_part ? " among the parts it may change places with" : " on its plate"));
        if (is_part) {
            const auto [o, v] = ctx.targets.volumes.front();
            const size_t vid  = model.objects[o]->volumes[v]->id().id;
            ctx.snapshot("Object order changed");
            quietly(p, [&] { list->move_in_order(move); });
            const auto now = volume_index_by_id(model, vid);
            return InvokeResult::ok({{"volume", volume_ref_json(model, now.first, now.second)}});
        }
        const size_t id = model.objects[ctx.targets.objects.front()]->id().id;
        ctx.snapshot("Object order changed");
        quietly(p, [&] { list->move_in_order(move); });
        return InvokeResult::ok({{"object", object_ref_json(model, object_index_by_id(model, id))}});
    }

    if (is_part) {
        // A part to a position: step it there one allowed slot at a time, as Move up / Move down do.
        if (ctx.params.contains("before"))
            return bad_param("/params/before", "before names an object; move a part with toIndex or move");
        const auto [o, v]   = ctx.targets.volumes.front();
        const int  to_index = ctx.params["toIndex"].get<int>();
        const std::vector<int> slots = objlist::part_slots(objlist::volume_types(*model.objects[o]), v);
        if (std::find(slots.begin(), slots.end(), to_index) == slots.end()) {
            json allowed = json::array();
            for (int s : slots)
                allowed.push_back(s);
            return bad_param("/params/toIndex", "the part cannot take that place: parts move only among parts of their own type, and "
                             "the first place is kept for a solid part",
                             {{"allowed", allowed}});
        }
        const size_t vid = model.objects[o]->volumes[v]->id().id;
        if (to_index == v)
            return InvokeResult::ok({{"volume", volume_ref_json(model, o, v)}}).with_effects({});
        ctx.snapshot("Object order changed");
        quietly(p, [&] {
            for (size_t guard = 0; guard < slots.size(); ++guard) {
                const int cur = volume_index_by_id(model, vid).second;
                if (cur == to_index)
                    break;
                const OrderMove step = to_index < cur ? OrderMove::Up : OrderMove::Down;
                if (!list->can_move_in_order(step))
                    break;
                list->move_in_order(step);
            }
        });
        p->set_current_canvas_as_dirty();
        const auto now = volume_index_by_id(model, vid);
        return InvokeResult::ok({{"volume", volume_ref_json(model, now.first, now.second)}});
    }

    // An object to a place among the objects of its plate (what dropping its row there does).
    const int      o      = ctx.targets.objects.front();
    PartPlateList& plates = p->get_partplate_list();
    const int      plate  = plates.find_instance(o, 0);
    if (plate < 0)
        return disabled("the object is on no plate; move it onto a plate first (object.toPlate)");
    std::vector<int> slots;
    for (int i = 0; i < int(model.objects.size()); ++i)
        if (plates.find_instance(i, 0) == plate)
            slots.push_back(i);
    int target = -1;
    if (ctx.params.contains("toIndex")) {
        target = objlist::slot_at(slots, ctx.params["toIndex"].get<int>());
        if (target < 0)
            return bad_param("/params/toIndex", "plate " + std::to_string(plate) + " holds " + std::to_string(slots.size()) +
                                                    " object(s); toIndex is 0-based among them");
    } else {
        std::string why;
        const int   before = object_param(model, ctx.params["before"], why);
        if (before < 0)
            return InvokeResult::error(WAB_NOT_FOUND, why, {{"missing", "/params/before"}});
        if (before == o)
            return bad_param("/params/before", "an object cannot move before itself");
        if (plates.find_instance(before, 0) != plate)
            return bad_param("/params/before", "objects reorder only within their plate; '" + model.objects[before]->name + "' is on another plate");
        target = objlist::slot_before(slots, o, before);
    }
    const size_t id = model.objects[o]->id().id;
    json         r  = {{"object", object_ref_json(model, o)}, {"plate", plate}};
    if (target == o) {
        r["plateOrder"] = object_refs(model, slots);
        return InvokeResult::ok(r).with_effects({});
    }
    ctx.snapshot("Object order changed");
    quietly(p, [&] { list->reorder_object(o, target); });
    p->set_current_canvas_as_dirty();
    std::vector<int> order;
    for (int i = 0; i < int(model.objects.size()); ++i)
        if (plates.find_instance(i, 0) == plate)
            order.push_back(i);
    r["object"]     = object_ref_json(model, object_index_by_id(model, id));
    r["plateOrder"] = object_refs(model, order);
    return InvokeResult::ok(r);
}

// Split > To Objects. The menu path may ask "disable Auto-Drop to preserve Z positioning?" when
// a piece floats; here that is keepZ. It shows a warning when the object is one piece; refused.
InvokeResult run_split_to_objects(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    Model&       model = ctx.model();
    const int    o     = ctx.targets.objects.front();
    ModelObject* mo    = model.objects[o];
    if (mo->instances.empty())
        return disabled("the object has no instance");
    if (!p->can_split(true))
        return disabled("the object cannot be split to objects (a cut object, several instances, or a gizmo is open)");
    {
        // The same split on a throw-away copy: how many pieces would there be?
        Model        scratch;
        ModelObject* copy = scratch.add_object(*mo);
        ModelObjectPtrs pieces;
        copy->split(&pieces, false);
        if (pieces.size() < 2)
            return disabled("the object is a single connected piece; there is nothing to split");
    }
    // Default: keep Z when the object does not auto-drop (it was placed deliberately), drop
    // every piece onto the bed when it does -- the dialog's two answers.
    const bool   was_auto_drop = mo->instances.front()->auto_drop;
    const bool   keep_z        = has(ctx.params, "keepZ") ? ctx.params["keepZ"].get<bool>() : !was_auto_drop;
    const size_t before        = model.objects.size();
    ctx.snapshot("Split to Objects");
    quietly(p, [&] {
        // With auto-drop off on the source no question is asked, and the pieces' auto-drop is the
        // argument. The source object is replaced by its pieces, so this changes nothing that stays.
        mo->instances.front()->auto_drop = false;
        p->split_object(o, !keep_z);
    });
    // split_object removed the source (indices shift down by one) and appended the pieces.
    std::vector<int> pieces;
    for (size_t i = before - 1; i < model.objects.size(); ++i)
        pieces.push_back(int(i));
    return InvokeResult::ok({{"objects", object_refs(model, pieces)}, {"keepZ", keep_z}});
}

// Assemble: several objects into one multi-part object ("multipart"), or one multi-part object's
// parts into a single part ("single").
InvokeResult run_merge(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    ObjectList* list = wxGetApp().obj_list();
    if (list == nullptr)
        return no_plater();
    Model&            model = ctx.model();
    const std::string mode  = ctx.params.value("mode", std::string("multipart"));
    if (mode == "multipart") {
        if (ctx.targets.objects.size() < 2)
            return bad_param("/target/objects", "assembling needs at least two objects");
        if (!list->can_merge_to_multipart_object())
            return disabled("these objects cannot be assembled (a cut object is selected)");
        ctx.snapshot("Assemble");
        quietly(p, [&] { list->merge(true); });
        const int merged = int(model.objects.size()) - 1;
        return InvokeResult::ok({{"object", object_ref_json(model, merged)}, {"parts", model.objects[merged]->volumes.size()}});
    }
    if (ctx.targets.objects.size() != 1)
        return bad_param("/target/objects", "mode \"single\" takes exactly one object");
    const int o = ctx.targets.objects.front();
    if (!list->can_merge_to_single_object())
        return disabled("the object has a single part");
    ctx.snapshot("Merge parts to an object");
    quietly(p, [&] { list->merge(false); });
    return InvokeResult::ok({{"object", object_ref_json(model, o)}, {"parts", model.objects[o]->volumes.size()}});
}

InvokeResult run_convert_units(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    const std::string    c    = ctx.params.value("conversion", std::string("fromInch"));
    const ConversionType type = c == "fromInch"  ? ConversionType::CONV_FROM_INCH :
                                c == "toInch"    ? ConversionType::CONV_TO_INCH :
                                c == "fromMeter" ? ConversionType::CONV_FROM_METER :
                                                   ConversionType::CONV_TO_METER;
    // The menu only offers a conversion no selected part has already had (or, for a restore,
    // that every part has had): the same rule.
    for (int o : ctx.targets.objects)
        for (const ModelVolume* v : ctx.model().objects[o]->volumes) {
            const bool respects = (type == ConversionType::CONV_FROM_INCH && v->source.is_converted_from_inches) ||
                                  (type == ConversionType::CONV_TO_INCH && !v->source.is_converted_from_inches) ||
                                  (type == ConversionType::CONV_FROM_METER && v->source.is_converted_from_meters) ||
                                  (type == ConversionType::CONV_TO_METER && !v->source.is_converted_from_meters);
            if (respects)
                return disabled(type == ConversionType::CONV_FROM_INCH || type == ConversionType::CONV_FROM_METER ?
                                    "already converted" :
                                    "there is no conversion to restore");
        }
    std::vector<size_t> ids;
    for (int o : ctx.targets.objects)
        ids.push_back(ctx.model().objects[o]->id().id);
    const size_t before = ctx.model().objects.size();
    ctx.snapshot("Convert units");
    quietly(p, [&] { p->convert_unit(type); });
    // convert_unit replaces the objects with converted copies at the end of the list.
    std::vector<int> now = appended_since(ctx.model(), before - ctx.targets.objects.size());
    json             out = json::array();
    for (int o : now) {
        json r    = object_ref_json(ctx.model(), o);
        r["size"] = {ctx.model().objects[o]->instance_bounding_box(0).size().x(), ctx.model().objects[o]->instance_bounding_box(0).size().y(),
                     ctx.model().objects[o]->instance_bounding_box(0).size().z()};
        out.push_back(r);
    }
    return InvokeResult::ok({{"objects", out}, {"conversion", c}});
}

InvokeResult run_reload_from_disk(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    // Plater::reload_from_disk opens a file dialog for a source it cannot find and a colour
    // dialog for OBJ sources; both are checked here first and refused instead.
    json missing   = json::array();
    int  reloadable = 0;
    bool obj_source = false;
    for (int o : ctx.targets.objects) {
        const ModelObject* mo = ctx.model().objects[o];
        for (const ModelVolume* v : mo->volumes) {
            if (v->source.is_from_builtin_objects || v->source.input_file.empty() || fs::path(v->source.input_file).extension().string().empty())
                continue;
            ++reloadable;
            bool found = fs::exists(v->source.input_file);
            if (!found && !mo->input_file.empty()) {
                fs::path beside = fs::path(mo->input_file).remove_filename() / fs::path(v->source.input_file).filename();
                found           = !fs::path(mo->input_file).remove_filename().empty() && fs::exists(beside);
            }
            if (!found)
                missing.push_back(fs::path(v->source.input_file).filename().string());
            if (boost::iends_with(v->source.input_file, ".obj"))
                obj_source = true;
        }
    }
    if (reloadable == 0)
        return disabled("no part of these objects was loaded from a file");
    if (!missing.empty())
        return InvokeResult::error(WAB_NOT_FOUND, "the source file is no longer where it was loaded from", {{"missing", missing}});
    if (obj_source)
        return disabled("OBJ sources ask about colours when reloaded; reload them from the object's menu");
    ctx.snapshot("Reload from disk");
    quietly(p, [&] { p->reload_from_disk(); });
    return InvokeResult::ok({{"objects", object_refs(ctx.model(), ctx.targets.objects)}, {"reloadedParts", reloadable}});
}

InvokeResult run_copy_settings(InvokeContext& ctx)
{
    ObjectList* list = wxGetApp().obj_list();
    if (ctx.plater() == nullptr || list == nullptr)
        return no_plater();
    list->copy_settings_to_clipboard();
    const int o = ctx.targets.objects.front();
    return InvokeResult::ok({{"object", object_ref_json(ctx.model(), o)}, {"keys", ctx.model().objects[o]->config.keys()}});
}

InvokeResult run_paste_settings(InvokeContext& ctx)
{
    Plater*     p    = ctx.plater();
    ObjectList* list = wxGetApp().obj_list();
    if (p == nullptr || list == nullptr)
        return no_plater();
    if (!list->can_paste_settings_into_list())
        return disabled("no object settings have been copied (object.copySettings first)");
    ctx.snapshot("Paste settings");
    quietly(p, [&] { list->paste_settings_into_list(); });
    json out = json::array();
    for (int o : ctx.targets.objects) {
        json r         = object_ref_json(ctx.model(), o);
        r["overrides"] = ctx.model().objects[o]->config.keys();
        out.push_back(r);
    }
    return InvokeResult::ok({{"objects", out}});
}

// Instance position, rotation and (non-uniform) scale: what the Move / Rotate / Scale gizmos and
// the object manipulation panel set, as numbers.
InvokeResult run_set_transform(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    Model&       model    = ctx.model();
    const int    o        = ctx.targets.objects.front();
    ModelObject* mo       = model.objects[o];
    const int    instance = ctx.params.value("instance", -1);
    const bool   relative = ctx.params.value("relative", false);
    Vec3d        offset, rot_deg, scale;
    const bool   has_offset = read_vec3(ctx.params, "offset", offset);
    const bool   has_rot    = read_vec3(ctx.params, "rotationDeg", rot_deg);
    const bool   has_scale  = read_vec3(ctx.params, "scale", scale);
    if (!has_offset && !has_rot && !has_scale)
        return InvokeResult::error(WAB_BAD_PARAMS, "give at least one of offset, rotationDeg or scale", {{"path", "/params"}, {"needs", "offset"}});
    if (instance >= int(mo->instances.size()))
        return bad_param("/params/instance", "instance out of range (the object has " + std::to_string(mo->instances.size()) + ")");
    if (has_offset && !relative && instance < 0 && mo->instances.size() > 1)
        return bad_param("/params/instance", "an absolute offset for an object with several instances needs instance (which one to place)",
                         {{"needs", "instance"}});

    // frame: relative changes along the plate's axes (world) or the object's own (object), as the
    // gizmos' coordinate combo. Absent keeps the original mix: offset and rotation along the
    // plate's axes, scale along the object's own.
    const std::string frame = ctx.params.value("frame", "");
    if (!frame.empty() && !relative)
        return bad_param("/params/frame", "frame applies to relative changes; add relative: true");

    std::vector<size_t> which;
    for (size_t i = 0; i < mo->instances.size(); ++i)
        if (instance < 0 || int(i) == instance)
            which.push_back(i);

    ctx.snapshot("Set transform");
    // A turn or scale about the centre can push a part into the bed; one that stood on the bed goes
    // back onto it unless the call set its position itself (BedDrop.hpp).
    const std::vector<double> min_z_before = instance_min_zs(*mo);
    for (size_t i : which) {
        ModelInstance* mi = mo->instances[i];
        if (!frame.empty()) {
            const Vec3d pivot = mo->instance_bounding_box(i).center();
            auto        opt   = [](bool has, const Vec3d& v) { return has ? std::optional<Vec3d>(v) : std::optional<Vec3d>(); };
            mi->set_transformation(Geometry::Transformation(parts::relative_transform(
                mi->get_matrix(), opt(has_offset, offset), opt(has_rot, rot_deg), opt(has_scale, scale), frame == "object", pivot)));
            if (has_scale && !has_offset) { // keep it where it stands on the plate, as without frame
                const Vec3d after = mo->instance_bounding_box(i).center();
                mi->set_offset(mi->get_offset() + Vec3d(pivot.x() - after.x(), pivot.y() - after.y(), 0.));
            }
            continue;
        }
        if (has_rot) {
            if (relative) {
                // About the plate's X, then Y, then Z, around the instance's own centre.
                const Matrix3d r = (Eigen::AngleAxisd(Geometry::deg2rad(rot_deg.z()), Vec3d::UnitZ()) *
                                    Eigen::AngleAxisd(Geometry::deg2rad(rot_deg.y()), Vec3d::UnitY()) *
                                    Eigen::AngleAxisd(Geometry::deg2rad(rot_deg.x()), Vec3d::UnitX()))
                                       .toRotationMatrix();
                rotate_instance_about_center(*mo, i, r);
            } else
                mi->set_rotation(Vec3d(Geometry::deg2rad(rot_deg.x()), Geometry::deg2rad(rot_deg.y()), Geometry::deg2rad(rot_deg.z())));
        }
        if (has_scale) {
            const Vec3d centre = mo->instance_bounding_box(i).center();
            mi->set_scaling_factor(relative ? Vec3d(mi->get_scaling_factor().cwiseProduct(scale)) : scale);
            const Vec3d after = mo->instance_bounding_box(i).center();
            if (!has_offset) // keep it where it stands on the plate
                mi->set_offset(mi->get_offset() + Vec3d(centre.x() - after.x(), centre.y() - after.y(), 0.));
        }
        if (has_offset)
            mi->set_offset(relative ? Vec3d(mi->get_offset() + offset) : offset);
    }
    if (!has_offset)
        drop_to_bed_unless_sunk(*mo, min_z_before);
    mo->invalidate_bounding_box();
    p->changed_object(o); // re-drops auto-drop objects onto the bed, redraws, re-arms slicing
    refile_object(p, o);

    json inst = json::array();
    for (size_t i : which)
        inst.push_back(instance_state(*mo, i));
    return InvokeResult::ok({{"object", object_ref_json(model, o)}, {"instances", inst}, {"autoDrop", mo->instances.front()->auto_drop}});
}

// Flatten ("Place on face"): turn the object so a face lies on the bed.
InvokeResult run_lay_on_face(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    Model&       model = ctx.model();
    const int    o     = ctx.targets.objects.front();
    ModelObject* mo    = model.objects[o];
    if (mo->instances.empty())
        return disabled("the object has no instance");

    Vec3d world_n;
    if (has(ctx.params, "normal")) {
        if (!read_vec3(ctx.params, "normal", world_n) || world_n.norm() < 1e-9)
            return bad_param("/params/normal", "normal must be a non-zero [x, y, z]");
        world_n.normalize();
    } else if (!ctx.params.value("largestFace", false))
        return bad_param("/params/largestFace", "largestFace must be true (or give normal)");
    else if (!largest_face_normal(*mo, 0, world_n))
        return disabled("the object has no flat face to lay down");

    // The face is the same face of the OBJECT on every copy: carry instance 0's world normal into
    // object space (normals map by the inverse transpose; back by the transpose).
    const Matrix3d m0    = mo->instances.front()->get_matrix().linear();
    const Vec3d    obj_n = (m0.transpose() * world_n).normalized();

    ctx.snapshot("Place on face");
    for (size_t i = 0; i < mo->instances.size(); ++i) {
        const Matrix3d mi = mo->instances[i]->get_matrix().linear();
        const Vec3d    n  = (mi.inverse().transpose() * obj_n).normalized();
        const Matrix3d r  = Eigen::Quaterniond::FromTwoVectors(n, -Vec3d::UnitZ()).toRotationMatrix();
        rotate_instance_about_center(*mo, i, r);
    }
    mo->invalidate_bounding_box();
    mo->ensure_on_bed();
    p->changed_object(o);
    refile_object(p, o);
    return InvokeResult::ok({{"object", object_ref_json(model, o)},
                             {"faceNormal", {world_n.x(), world_n.y(), world_n.z()}},
                             {"instances", {instance_state(*mo, 0)}}});
}

// Plate context menu "Delete All" (the current or a named plate), or Edit > Delete All (every plate).
InvokeResult run_plate_clear(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    Model&            model  = ctx.model();
    const std::string scope  = ctx.params.value("scope", std::string("plate"));
    const size_t      before = model.objects.size();
    if (before == 0)
        return disabled("there are no objects");
    PartPlateList& plates = p->get_partplate_list();
    int            plate  = ctx.targets.plate >= 0 ? ctx.targets.plate : plates.get_curr_plate_index();
    if (scope == "plate") {
        if (plate >= plates.get_plate_count())
            return InvokeResult::error(WAB_NOT_FOUND, "no such plate", {{"plateCount", plates.get_plate_count()}});
        if (plates.get_plate(plate)->get_objects().empty())
            return disabled("the plate is empty");
    }
    ctx.snapshot(scope == "all" ? "Delete All Objects" : "Delete All");
    quietly(p, [&] {
        if (scope == "all")
            p->delete_all_objects_from_model();
        else {
            if (plate != plates.get_curr_plate_index())
                p->select_plate(plate);
            p->remove_curr_plate_all();
        }
    });
    json r = {{"removed", int(before - model.objects.size())}, {"objectCount", model.objects.size()}, {"scope", scope}};
    if (scope == "plate")
        r["plate"] = plate;
    return InvokeResult::ok(r);
}

InvokeResult run_edit_cut(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    if (!p->can_cut_to_clipboard())
        return disabled("nothing that can be cut is selected");
    const json   cut    = object_refs(ctx.model(), ctx.targets.objects.empty() ? current_selection().objects : ctx.targets.objects);
    const size_t before = ctx.model().objects.size();
    ctx.snapshot("Cut Selected Objects");
    quietly(p, [&] { p->cut_selection_to_clipboard(); });
    return InvokeResult::ok({{"cut", cut}, {"removed", int(before - ctx.model().objects.size())}});
}

InvokeResult run_edit_copy(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    if (!p->can_copy_to_clipboard())
        return disabled("nothing that can be copied is selected");
    p->copy_selection_to_clipboard();
    // Objects, or parts (Ctrl+C on part rows): what the clipboard now holds.
    const ResolvedTargets what = ctx.targets.objects.empty() && ctx.targets.volumes.empty() ? current_selection() : ctx.targets;
    return InvokeResult::ok({{"copied", object_refs(ctx.model(), what.objects)}, {"copiedParts", volume_refs(ctx.model(), what.volumes)}});
}

// Paste: objects onto the plate, or copied parts into the target object (Ctrl+V with an object
// selected, which the runtime selects from the target first).
InvokeResult run_edit_paste(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    if (!p->can_paste_from_clipboard())
        return disabled("the clipboard is empty, or what it holds cannot be pasted onto this selection (copied parts paste into one "
                        "object: name it in target.objects)");
    Model&       model      = ctx.model();
    const size_t before     = model.objects.size();
    const auto   vols_before = volume_ids_by_object(model);
    ctx.snapshot("Paste From Clipboard");
    quietly(p, [&] { p->paste_from_clipboard(); });
    json parts = json::array();
    for (size_t o = 0; o < model.objects.size(); ++o) {
        const auto it = vols_before.find(model.objects[o]->id().id);
        if (it == vols_before.end())
            continue; // a new object: reported in "added"
        for (size_t v = 0; v < model.objects[o]->volumes.size(); ++v)
            if (it->second.count(model.objects[o]->volumes[v]->id().id) == 0)
                parts.push_back(volume_ref_json(model, int(o), int(v)));
    }
    return InvokeResult::ok({{"added", object_refs(model, appended_since(model, before))}, {"addedParts", parts}});
}

// The "Cut connectors" info item's Delete -> "Delete all connectors" (ObjectList::
// delete_all_connectors_for_object): every connector of every piece of the cut goes; a piece left
// with no solid mesh is deleted. Undoable.
InvokeResult run_delete_connectors(InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return no_plater();
    ObjectList* list = wxGetApp().obj_list();
    if (list == nullptr)
        return no_plater();
    Model&             model = ctx.model();
    const int          o     = ctx.targets.objects.front();
    const ModelObject* mo    = model.objects[o];
    if (!mo->is_cut())
        return disabled("'" + mo->name + "' is not a piece of a cut object");
    std::vector<size_t> related;
    json                related_refs = json::array();
    bool                connectors   = false;
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->cut_id.is_equal(mo->cut_id)) {
            related.push_back(model.objects[i]->id().id);
            related_refs.push_back(object_ref_json(model, int(i)));
            for (const ModelVolume* v : model.objects[i]->volumes)
                connectors = connectors || v->is_cut_connector();
        }
    if (!connectors)
        return disabled("the cut has no connectors");
    ctx.snapshot("Delete all connectors");
    quietly(p, [&] { list->delete_all_connectors_for_object(o); });
    // Pieces left without a solid mesh were removed from the model: re-file the plates and redraw.
    p->get_partplate_list().reload_all_objects(false);
    p->update();
    p->schedule_background_process();
    json removed = json::array(), kept = json::array();
    for (size_t k = 0; k < related.size(); ++k) {
        const int now = object_index_by_id(model, related[k]);
        if (now < 0)
            removed.push_back(related_refs[k]);
        else
            kept.push_back(object_ref_json(model, now));
    }
    return InvokeResult::ok({{"objects", kept}, {"removedObjects", removed}});
}

// ── Enabled predicates (cheap; the runner re-checks what depends on the target) ─────────────

std::string needs_objects(const InvokeContext& ctx)
{
    Plater* p = ctx.plater();
    if (p == nullptr)
        return "the plate is not ready";
    return p->model().objects.empty() ? "there are no objects" : "";
}

} // namespace

void register_object_actions(Catalog& c)
{
    const json objects_req = target_spec("objects", true);
    const json object_req  = target_spec("object", true);
    const json placeable   = target_spec_accepting("objects", true, {"volumes", "instances"});

    c.add(make("object.rename", "Rename", "Rename an object (a single-part object's part takes the same name).", Risk::Mutate,
               object_req,
               params_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 200}, {"description", "The new name."}}}}, {"name"}),
               {"structure"}, {"GUI_Factories.cpp::menu::Rename", "GUI_ObjectList.cpp::column::colName", "GUI_ObjectTable.cpp::tablecol::col_name"}, json::array({example(first_object(), {{"name", "Renamed part"}})})),
          run_rename, needs_objects);

    c.add(make("object.delete", "Delete", "Delete objects from the plate (undoable).", Risk::Mutate, objects_req,
               params_schema({{"breakCutLink",
                               {{"type", "boolean"},
                                {"default", false},
                                {"description", "Required true to delete a piece of a cut object, which breaks the link between the pieces."}}}}),
               {"structure", "plates"},
               {"GUI_Factories.cpp::menu::Delete", "MainFrame.cpp::menu::Delete Selected"},
               json::array({example(first_object())})),
          run_delete, needs_objects);

    c.add(make("part.rename", "Rename part",
               "Rename one part (solid part, modifier, negative part, support blocker/enforcer) of an object, as editing its name in the "
               "object list does. Text and SVG parts take their name from the text or file again when edited.",
               Risk::Mutate, target_spec("volume", true),
               params_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 200}, {"description", "The new name."}}}}, {"name"}),
               {"structure"}, {"GUI_ObjectList.cpp::column::colName", "GUI_ObjectTable.cpp::tablecol::col_name"},
               json::array({example(first_part(), {{"name", "Main body"}})})),
          run_part_rename, needs_objects);

    {
        ActionDescriptor d = make("part.delete", "Delete part",
                                  "Delete parts, modifiers, negative parts or support blockers/enforcers from their objects (undoable). The "
                                  "last solid part of an object cannot be deleted; a part left alone takes over its object's settings.",
                                  Risk::Mutate, target_spec("volumes", true),
                                  params_schema({{"breakCutLink",
                                                  {{"type", "boolean"},
                                                   {"default", false},
                                                   {"description", "Required true to delete a solid part or negative part of a cut object, "
                                                                   "which breaks the link between the cut pieces."}}}}),
                                  {"structure", "geometry"}, {"GUI_Factories.cpp::menu::Delete"},
                                  json::array({example(first_part())}));
        d.examples[0]["target"] = {{"volumes", json::array({{{"object", {{"index", 0}}}, {"index", 1}}})}};
        d.examples[0]["setup"]  = json::array({{{"id", "part.add"},
                                                {"target", {{"object", {{"index", 0}}}}},
                                                {"params", {{"type", "modifier"}, {"sizeMm", {10}}}}}});
        c.add(std::move(d), run_part_delete, needs_objects);
    }

    {
        ActionDescriptor d = make(
            "object.clone", "Clone", "Add copies of objects as new, independent objects; optionally arrange the plate afterwards.",
            Risk::Mutate, objects_req,
            params_schema({{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}, {"default", 1}, {"description", "How many copies to add."}}},
                           {"arrange", {{"type", "boolean"}, {"default", false}, {"description", "Arrange the current plate afterwards."}}}}),
            {"structure", "plates"}, {"GUI_Factories.cpp::menu::Clone", "MainFrame.cpp::menu::Clone Selected"},
            json::array({example(first_object(), {{"count", 1}}), example(first_object(), {{"count", 2}, {"arrange", true}})}));
        d.long_running = true;
        c.add(std::move(d), run_clone, needs_objects);
    }

    c.add(make("object.addInstance", "Add instance",
               "Add instances (linked copies sharing the object's mesh and settings) to an object, as the + button does.", Risk::Mutate,
               object_req,
               params_schema({{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"default", 1}, {"description", "Instances to add."}}}}),
               {"structure", "plates"}, {"GUI_Factories.cpp::menu::Add instance", "GLCanvas3D.cpp::toolbar::more"},
               json::array({example(first_object(), {{"count", 1}})})),
          run_add_instance, needs_objects);

    {
        ActionDescriptor d = make("object.removeInstance", "Remove instance",
                                  "Remove instances: the last ones of one object (count, as the - button does), or exactly the instances "
                                  "named in target.instances (Del on instance rows). An object always keeps one.",
                                  Risk::Mutate, target_spec_accepting("objects", true, {"instances"}),
                                  params_schema({{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 999}, {"default", 1},
                                                            {"description", "With an object target: how many of its last instances to remove."}}}}),
                                  {"structure", "plates"}, {"GUI_Factories.cpp::menu::Remove instance", "GLCanvas3D.cpp::toolbar::fewer"},
                                  json::array({example(first_object(), {{"count", 1}}), example(first_instance())}));
        const json add_one     = json::array({{{"id", "object.addInstance"}, {"target", first_object()}, {"params", {{"count", 1}}}}});
        d.examples[0]["setup"] = add_one;
        d.examples[1]["setup"] = add_one;
        c.add(std::move(d), run_remove_instance, needs_objects);
    }

    c.add(make("object.instancesToObjects", "Set as individual objects",
               "Turn an object's instances (all, or the listed ones) into separate objects that can then be edited independently.",
               Risk::Mutate, object_req,
               params_schema({{"instances",
                               {{"type", "array"},
                                {"minItems", 1},
                                {"maxItems", 1000},
                                {"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 999}}},
                                {"description", "Instance indices to separate; omitted = every instance."}}}}),
               {"structure"}, {"GUI_Factories.cpp::menu::Set as An Individual Object"}, json::array({example(first_object())})),
          run_instances_to_objects, needs_objects);

    c.add(make("object.center", "Center",
               "Move objects to the centre of their plate (XY); also one part within its object (target.volumes) or single instances "
               "(target.instances).",
               Risk::Mutate, placeable, params_schema(), {"geometry"}, {"GUI_Factories.cpp::menu::Center"},
               json::array({example(first_object()), example(first_part())})),
          run_center, needs_objects);

    c.add(make("object.drop", "Drop",
               "Drop objects straight down onto the bed; also one part (target.volumes) or single instances (target.instances).",
               Risk::Mutate, placeable, params_schema(), {"geometry"}, {"GUI_Factories.cpp::menu::Drop", "GUI_ObjectList.cpp::column::colSinking"},
               json::array({example(first_object())})),
          run_drop, needs_objects);

    c.add(make("object.setAutoDrop", "Auto Drop",
               "Turn Auto Drop on or off for objects: when on, an object always rests on the bed after edits.", Risk::Mutate, objects_req,
               params_schema({{"enabled", {{"type", "boolean"}, {"description", "On or off."}}}}, {"enabled"}), {"geometry"},
               {"GUI_Factories.cpp::menu::expr:menu_text@append_menu_item_auto_drop",
                "GUI_Factories.cpp::menu::expr:menu_text@append_menu_item_set_auto_drop"},
               json::array({example(first_object(), {{"enabled", false}})})),
          run_set_auto_drop, needs_objects);

    c.add(make("object.setPrintable", "Printable",
               "Mark objects (every instance) or single instances (target.instances) printable or not; non-printable ones stay on the "
               "plate but are not sliced.",
               Risk::Mutate, target_spec_accepting("objects", true, {"instances"}),
               params_schema({{"printable", {{"type", "boolean"}, {"description", "Printable or not."}}}}, {"printable"}), {"structure"},
               {"GUI_Factories.cpp::menu::Printable", "GUI_ObjectList.cpp::column::colPrint", "GUI_ObjectTable.cpp::tablecol::col_printable"},
               json::array({example(first_object(), {{"printable", false}}), example(first_instance(), {{"printable", false}})})),
          run_set_printable, needs_objects);

    c.add(make("object.setVisible", "Hide / Show",
               "Hide or show objects in the assembly view (display only; nothing about the print changes).", Risk::Ui, objects_req,
               params_schema({{"visible", {{"type", "boolean"}, {"description", "Shown or hidden."}}}}, {"visible"}), {"view"},
               {"GUI_Factories.cpp::menu::Hide", "GUI_Factories.cpp::menu::Show"},
               json::array({example(first_object(), {{"visible", true}})})),
          run_set_visible, needs_objects);

    c.add(make("object.mirror", "Mirror",
               "Mirror objects along a plate axis; also one part within its object (target.volumes) or single instances "
               "(target.instances). An object with several instances mirrors only by instance.",
               Risk::Mutate, placeable,
               params_schema({{"axis", {{"type", "string"}, {"enum", {"x", "y", "z"}}, {"description", "Plate axis to mirror along."}}}}, {"axis"}),
               {"geometry"},
               {"GUI_Factories.cpp::menu::Along X Axis", "GUI_Factories.cpp::menu::Along Y Axis", "GUI_Factories.cpp::menu::Along Z Axis",
                "GUI_Factories.cpp::submenu::Mirror"},
               json::array({example(first_object(), {{"axis", "x"}}), example(first_part(), {{"axis", "y"}})})),
          run_mirror, needs_objects);

    {
        json before = params_schema({{"id", {{"type", "integer"}, {"minimum", 0}}},
                                     {"index", {{"type", "integer"}, {"minimum", 0}}},
                                     {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 200}}}});
        before["description"]  = "Objects only: the object (on the same plate) to move in front of; {id}, {index} or {name}.";
        before["x-exactlyOne"] = json::array({json::array({"id"}), json::array({"index"}), json::array({"name"})});
        ActionDescriptor d = make(
            "object.order", "Order",
            "Move one object within its plate's list order, or one part (target.volumes) among its object's parts: first, up, down, "
            "last, to a position, or in front of another object. Objects print in list order when printing by object.",
            Risk::Mutate, target_spec_accepting("objects", true, {"volumes"}),
            params_schema({{"move", {{"type", "string"}, {"enum", {"top", "up", "down", "bottom"}}, {"description", "One step or to an end."}}},
                           {"toIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", 9999},
                                        {"description", "Objects: 0-based place among the objects on its plate. Parts: the part index to take "
                                                        "(parts stay among parts of their type; index 0 is kept for a solid part)."}}},
                           {"before", before}}),
            {"structure"}, {"GUI_Factories.cpp::menu::expr:label@append_menu_items_order", "GUI_Factories.cpp::submenu::Order",
                            "GUI_ObjectList.cpp::column::colDragHandle"},
            json::array({example(first_object(), {{"move", "bottom"}}), example(first_object(), {{"toIndex", 0}})}));
        d.params["x-exactlyOne"] = json::array({json::array({"move"}), json::array({"toIndex"}), json::array({"before"})});
        c.add(std::move(d), run_order, needs_objects);
    }

    c.add(make("object.splitToObjects", "Split to objects",
               "Split an object made of several disconnected pieces into one object per piece.", Risk::Mutate, object_req,
               params_schema({{"keepZ",
                               {{"type", "boolean"},
                                {"description", "Keep each piece's height (turns Auto Drop off for them); false drops every piece onto the bed. "
                                                "Default: keep when the object was not auto-dropping."}}}}),
               {"structure", "geometry"},
               {"GUI_Factories.cpp::menu::To Objects", "GLCanvas3D.cpp::toolbar::splitobjects"},
               json::array({example(first_object())})),
          run_split_to_objects, needs_objects);

    c.add(make("object.merge", "Assemble",
               "Assemble several objects into one multi-part object (multipart), or merge one object's parts into a single part (single).",
               Risk::Mutate, objects_req,
               params_schema({{"mode",
                               {{"type", "string"},
                                {"enum", {"multipart", "single"}},
                                {"default", "multipart"},
                                {"description", "multipart: objects become parts of one new object; single: one object's parts fuse into one part."}}}}),
               {"structure", "geometry"}, {"GUI_Factories.cpp::menu::Assemble"},
               json::array({example({{"objects", json::array({{{"index", 0}}, {{"index", 1}}})}}, {{"mode", "multipart"}})})),
          run_merge, needs_objects);

    c.add(make("object.convertUnits", "Convert units",
               "Rescale objects that were modelled in inches or metres (x25.4 / x1000), or undo that conversion.", Risk::Mutate,
               objects_req,
               params_schema({{"conversion",
                               {{"type", "string"}, {"enum", {"fromInch", "toInch", "fromMeter", "toMeter"}}, {"description", "Which conversion."}}}},
                             {"conversion"}),
               {"geometry", "structure"}, {"GUI_Factories.cpp::menu::expr:item.second@append_menu_items_convert_unit"},
               json::array({example(first_object(), {{"conversion", "fromInch"}})})),
          run_convert_units, needs_objects);

    c.add(make("object.reloadFromDisk", "Reload from disk", "Reload objects' meshes from the files they were loaded from.", Risk::Mutate,
               objects_req, params_schema(), {"geometry"}, {"GUI_Factories.cpp::menu::Reload from disk"},
               json::array({example(first_object())})),
          run_reload_from_disk, needs_objects);

    c.add(make("object.copySettings", "Copy process settings",
               "Copy an object's own process settings (its per-object overrides) for object.pasteSettings.", Risk::Ui, object_req,
               params_schema(), {}, {}, json::array({example(first_object())})),
          run_copy_settings, needs_objects);

    c.add(make("object.pasteSettings", "Paste process settings",
               "Replace objects' own process settings with the ones copied by object.copySettings.", Risk::Mutate, objects_req,
               params_schema(), {"settings"}, {}, json::array({example(first_object())})),
          run_paste_settings, needs_objects);

    c.add(make("object.setTransform", "Move / rotate / scale",
               "Set an object's position (instance offset, mm), rotation (degrees, XYZ Euler) and scale (factor per axis, may differ "
               "per axis) -- absolute, or relative to the current values.",
               Risk::Mutate, object_req,
               params_schema({{"offset", vec3_schema("Instance position [x, y, z] in mm, plate frame (relative: added).")},
                              {"rotationDeg", vec3_schema("Rotation [x, y, z] in degrees (absolute: Euler XYZ; relative: about the plate's X, then Y, then Z, around the object's centre).", -3600., 3600.)},
                              {"scale", vec3_schema("Scale factor per axis, 1 = 100% (relative: multiplied).", 0.001, 1000.)},
                              {"relative", {{"type", "boolean"}, {"default", false}, {"description", "Change by these amounts instead of setting them."}}},
                              {"frame", {{"type", "string"}, {"enum", {"world", "object"}},
                                         {"description", "relative: offset, rotation and scale all along the plate's axes (world) or all along "
                                                         "the object's own (object), about its centre, as the gizmos' coordinate combo. "
                                                         "Absent: offset and rotation along the plate's, scale along the object's own."}}},
                              {"instance", {{"type", "integer"}, {"minimum", -1}, {"maximum", 999}, {"default", -1}, {"description", "Which instance; -1 = all."}}}}),
               {"geometry"},
               {"GLGizmosManager.cpp::gizmo::GLGizmoMove3D", "GLGizmosManager.cpp::gizmo::GLGizmoRotate3D",
                "GLGizmosManager.cpp::gizmo::GLGizmoScale3D"},
               json::array({example(first_object(), {{"rotationDeg", {0, 0, 45}}, {"relative", true}}),
                            example(first_object(), {{"scale", {1.5, 1, 1}}}),
                            example(first_object(), {{"offset", {10, 0, 0}}, {"relative", true}, {"frame", "object"}})})),
          run_set_transform, needs_objects);

    {
        ActionDescriptor d = make(
            "object.layOnFace", "Lay on face",
            "Turn an object so a flat face lies on the bed: the face pointing along a direction, or its largest flat face.", Risk::Mutate,
            object_req,
            params_schema({{"normal", vec3_schema("World direction [x, y, z] the face currently points to (e.g. [1,0,0] = the side facing +X).", -1., 1.)},
                           {"largestFace", {{"type", "boolean"}, {"description", "true: the largest flat face (of the convex hull)."}}}}),
            {"geometry"}, {"GLGizmosManager.cpp::gizmo::GLGizmoFlatten"},
            json::array({example(first_object(), {{"largestFace", true}}), example(first_object(), {{"normal", {1, 0, 0}}})}));
        d.params["x-exactlyOne"] = json::array({json::array({"normal"}), json::array({"largestFace"})});
        c.add(std::move(d), run_lay_on_face, needs_objects);
    }

    c.add(make("plate.clear", "Delete all", "Delete every object on a plate (default: the current one) or on every plate (undoable).",
               Risk::Mutate, target_spec("plate", false),
               params_schema({{"scope", {{"type", "string"}, {"enum", {"plate", "all"}}, {"default", "plate"}, {"description", "One plate or all plates."}}}}),
               {"structure", "plates"}, {"MainFrame.cpp::menu::Delete All"},
               json::array({example(nullptr, {{"scope", "plate"}})})),
          run_plate_clear, needs_objects);

    c.add(make("edit.cut", "Cut", "Cut objects to the slicer's clipboard (they leave the plate until pasted).", Risk::Mutate,
               target_spec("objects", false), params_schema(), {"structure", "plates"}, {"MainFrame.cpp::menu::Cut"},
               json::array({example(first_object())})),
          run_edit_cut, needs_objects);

    c.add(make("edit.copy", "Copy", "Copy objects, or parts (target.volumes), to the slicer's clipboard.", Risk::Ui,
               target_spec_accepting("objects", false, {"volumes"}), params_schema(), {}, {"MainFrame.cpp::menu::Copy"},
               json::array({example(first_object())})),
          run_edit_copy, needs_objects);

    {
        ActionDescriptor d = make("edit.paste", "Paste",
                                  "Paste what the slicer's clipboard holds: copied objects onto the plate, copied parts into the object named "
                                  "in the target. Reports the objects and parts added.",
                                  Risk::Mutate, target_spec("object", false), params_schema(), {"structure", "plates"},
                                  {"MainFrame.cpp::menu::Paste"}, json::array({example(nullptr)}));
        d.examples[0]["setup"] = json::array({{{"id", "edit.copy"}, {"target", first_object()}}});
        c.add(std::move(d), run_edit_paste);
    }

    {
        ActionDescriptor d = make("object.deleteConnectors", "Delete all connectors",
                                  "Delete every connector (plug or dowel) of a cut, on all of its pieces, keeping the cut link; a piece left "
                                  "with no solid part is deleted too (reported in removedObjects). Undoable.",
                                  Risk::Mutate, object_req, params_schema(), {"structure", "geometry"}, {},
                                  json::array({example(first_object())}));
        d.examples[0]["setup"] = json::array({{{"id", "object.cut"},
                                               {"target", {{"object", {{"index", 0}}}}},
                                               {"params", {{"z", 10}, {"connectors", json::array({json::object()})}}}}});
        c.add(std::move(d), run_delete_connectors, needs_objects);
    }
}

}}} // namespace Slic3r::GUI::Actions
