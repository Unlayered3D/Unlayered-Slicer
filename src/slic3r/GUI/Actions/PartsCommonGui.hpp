#ifndef slic3r_GUI_Actions_PartsCommonGui_hpp_
#define slic3r_GUI_Actions_PartsCommonGui_hpp_

// GUI-side helpers shared by PartActions.cpp, MeshActions.cpp and TextActions.cpp (work package S3).
// Only included by those .cpp files; everything is inline and touches wxGetApp() only when called
// from a runner (GUI thread), never at registration.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/Emboss.hpp"
#include "libslic3r/Model.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "ActionRuntime.hpp"
#include "ActionTargets.hpp"
#include "PartsCommon.hpp"

namespace Slic3r { namespace GUI { namespace Actions { namespace parts {

inline ObjectList* object_list() { return wxGetApp().obj_list(); }

// After a volume was added to object `obj_idx` (or its type changed): refile the object's volumes
// in the object list, select `select` (a volume of that object; may be null), refresh the printable
// state and the plate, and let the 3D scene reload. What ObjectList::load_generic_subobject does
// after adding a primitive, minus its tips dialog.
inline void refresh_object_after_volume_change(int obj_idx, const ModelVolume* select)
{
    ObjectList* list = object_list();
    Plater*     plater = wxGetApp().plater();
    if (list != nullptr) {
        const ModelObject* object = wxGetApp().model().objects[obj_idx];
        // reorder_volumes_and_get_selection leaves the rows alone for a single-volume object, so
        // rows of volumes just deleted would linger; rebuild them directly then.
        wxDataViewItemArray items = object->volumes.size() > 1 ?
                                        list->reorder_volumes_and_get_selection(obj_idx, [select](const ModelVolume* v) { return v == select; }) :
                                        list->add_volumes_to_object_in_list(size_t(obj_idx));
        if (select != nullptr && !items.IsEmpty())
            list->select_item(items.front());
        if (plater != nullptr && plater->get_view3D_canvas3D() != nullptr)
            plater->get_view3D_canvas3D()->update_instance_printable_state_for_object(size_t(obj_idx));
        list->selection_changed();
        list->notify_instance_updated(obj_idx);
        list->update_info_items(size_t(obj_idx));
    }
    if (plater != nullptr)
        plater->changed_object(obj_idx);
}

// The volume's bounding box on the plate (first instance), for results.
inline nlohmann::json volume_world_bbox_json(const ModelObject& object, const ModelVolume& volume)
{
    if (object.instances.empty())
        return nullptr;
    const Transform3d   m  = object.instances.front()->get_matrix() * volume.get_matrix();
    const BoundingBoxf3 bb = volume.mesh().transformed_bounding_box(m);
    return {{"min", vec3_json(bb.min)}, {"max", vec3_json(bb.max)}};
}

inline nlohmann::json volume_result_json(const Model& model, int obj_idx, int vol_idx)
{
    nlohmann::json j = volume_ref_json(model, obj_idx, vol_idx);
    if (obj_idx >= 0 && obj_idx < int(model.objects.size())) {
        const ModelObject* o = model.objects[obj_idx];
        if (vol_idx >= 0 && vol_idx < int(o->volumes.size())) {
            const ModelVolume* v = o->volumes[vol_idx];
            j["type"]   = volume_type_name(v->type());
            j["bboxMm"] = volume_world_bbox_json(*o, *v);
            j["facets"] = v->mesh().facets_count();
        }
    }
    return j;
}

inline int volume_index_in(const ModelObject& object, const ModelVolume* volume)
{
    for (size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i] == volume)
            return int(i);
    return -1;
}

inline int volume_index_by_object_id(const ModelObject& object, const ObjectID& id)
{
    for (size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i]->id() == id)
            return int(i);
    return -1;
}

inline int object_index_by_object_id(const Model& model, const ObjectID& id)
{
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id() == id)
            return int(i);
    return -1;
}

// Where a surface-attached volume (text, SVG) goes on a named side of an object: a ray from
// outside the instance's bounding box toward the object, shifted by `offset_mm` (x = along the
// text, y = up the text) in the side's plane, hits the object's model parts; the result is the
// world transformation whose +Z is the surface normal at the hit (or the side's normal when the
// ray misses), rotated by `rotation_deg` about that normal. Instance 0.
struct SurfacePlacement
{
    Transform3d world = Transform3d::Identity();
    bool        hit   = false;
    Vec3d       position = Vec3d::Zero();
    Vec3d       normal   = Vec3d::UnitZ();
};

inline SurfacePlacement place_on_side(const ModelObject& object, const Vec3d& side_n, const Vec2d& offset_mm, double rotation_deg)
{
    SurfacePlacement out;
    if (object.instances.empty())
        return out;
    const Transform3d inst = object.instances.front()->get_matrix();

    indexed_triangle_set world_its;
    for (const ModelVolume* v : object.volumes) {
        if (!v->is_model_part())
            continue;
        indexed_triangle_set its = v->mesh().its;
        its_transform(its, inst * v->get_matrix());
        its_merge(world_its, its);
    }
    const BoundingBoxf3 bb     = object.instance_bounding_box(0);
    const Vec3d         center = bb.center();
    const Vec3d         n      = side_n.normalized();
    // The side's own frame (as the emboss code would orient text there) gives the in-plane axes.
    const Transform3d   side_tr = Emboss::create_transformation_onto_surface(center, n);
    const Vec3d         ax      = (side_tr.linear() * Vec3d::UnitX()).normalized();
    const Vec3d         ay      = (side_tr.linear() * Vec3d::UnitY()).normalized();
    const double        reach   = bb.size().norm() + 10.;
    const Vec3d         source  = center + ax * offset_mm.x() + ay * offset_mm.y() + n * reach;

    Vec3d position = center + ax * offset_mm.x() + ay * offset_mm.y() + n * (0.5 * std::abs(bb.size().dot(n)));
    Vec3d normal   = n;
    if (!world_its.indices.empty()) {
        AABBMesh                  mesh(world_its);
        const AABBMesh::hit_result hit = mesh.query_ray_hit(source, -n);
        if (hit.is_hit()) {
            out.hit  = true;
            position = hit.position();
            Vec3d hn = hit.normal();
            if (hn.norm() > EPSILON) {
                hn.normalize();
                if (hn.dot(n) > 0.)
                    normal = hn;
            }
        }
    }
    out.position = position;
    out.normal   = normal;
    out.world    = Emboss::create_transformation_onto_surface(position, normal) *
                Eigen::AngleAxisd(rotation_deg * M_PI / 180., Vec3d::UnitZ());
    return out;
}

// Finishes a long-running action on a later turn of the event loop. A download callback may run
// before the runner has returned (a URL refused at once), when the runtime has not attached its
// completion yet; finishing from CallAfter always lands after it has.
inline void finish_later(const std::shared_ptr<Deferred>& d, InvokeResult r)
{
    wxGetApp().CallAfter([d, r = std::move(r)]() mutable { d->finish(std::move(r)); });
}

// A gizmo that owns the object's state (cut, emboss, boolean, simplify, painting) must be closed
// before a mesh changes under it. The runtime refuses mutate actions while any gizmo is open; this
// is the belt-and-braces check for runners that finish later (long-running jobs).
inline bool gizmos_closed()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->get_view3D_canvas3D() == nullptr)
        return true;
    return plater->get_view3D_canvas3D()->get_gizmos_manager().get_current_type() == GLGizmosManager::Undefined;
}

}}}} // namespace Slic3r::GUI::Actions::parts

#endif // slic3r_GUI_Actions_PartsCommonGui_hpp_
