// Automation actions: mesh operations: part.boolean, part.simplify, part.smooth, part.fix,
// object.cut, object.invalidateCutInfo, object.skeletonCutPlanes.
// Owner: work package S3 (parts). See docs/HLSD/bridge-actions.md for how to write an action and
// docs/bridge-actions/parts.md for these.
//
// The gizmos behind these (Mesh boolean, Simplify, Cut) are interactive: the runners do what their
// "apply" buttons do, with the gizmo's choices as params, and never open the gizmo. Simplify,
// subdivision and repair run on a worker thread (a copy of the mesh) and are applied on the GUI
// thread; their undo point is taken when they start, so undoing the turn restores the old mesh.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "Register.hpp"
#include "PartsCommon.hpp"
#include "PartsCommonGui.hpp"
#include "CutPlaneRules.hpp"

#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <thread>

#include <boost/log/trivial.hpp>

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include "libslic3r/SkeletonBuild.hpp"
#include "libslic3r/TriangleMeshDeal.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using nlohmann::json;

namespace {

// ---------------------------------------------------------------- targets

// The volumes a mesh action works on: the named parts, or every solid part of the named objects.
std::vector<std::pair<int, int>> mesh_targets(const InvokeContext& ctx)
{
    std::vector<std::pair<int, int>> out = ctx.targets.volumes;
    const Model&                     model = ctx.model();
    for (int o : ctx.targets.objects) {
        const ModelObject* mo = model.objects[o];
        for (size_t v = 0; v < mo->volumes.size(); ++v)
            if (mo->volumes[v]->is_model_part())
                out.emplace_back(o, int(v));
    }
    return out;
}

// ---------------------------------------------------------------- background mesh jobs

struct MeshJobItem
{
    ObjectID                            object_id;
    ObjectID                            volume_id;
    std::shared_ptr<const TriangleMesh> source;
    std::optional<TriangleMesh>         result; // empty: unchanged
    std::string                         error;
    std::string                         name;
};

// Computes a new mesh for one volume on the worker thread. Return std::nullopt to leave the
// volume as it is; set `error` to report a failure for it.
using MeshCompute = std::function<std::optional<TriangleMesh>(const TriangleMesh& source, std::string& error)>;

// Replaces a volume's mesh as the gizmos' apply steps do (GLGizmoSimplify::apply_simplify).
void apply_mesh(int obj_idx, ModelVolume& volume, TriangleMesh&& mesh)
{
    Plater*    plater        = wxGetApp().plater();
    const bool keep_painting = wxGetApp().app_config->get_bool("keep_painting");
    if (!keep_painting)
        plater->clear_before_change_mesh(obj_idx);
    const std::optional<TriangleSelector::SavedPainting> saved =
        keep_painting ? volume.save_painting() : std::optional<TriangleSelector::SavedPainting>{};
    volume.set_mesh(std::move(mesh));
    volume.restore_painting(saved);
    volume.calculate_convex_hull();
    volume.invalidate_convex_hull_2d();
    volume.set_new_unique_id();
}

InvokeResult start_mesh_job(InvokeContext& ctx, const std::string& label, const std::vector<std::pair<int, int>>& targets, MeshCompute compute)
{
    Model& model = ctx.model();
    auto   items = std::make_shared<std::vector<MeshJobItem>>();
    for (const auto& ov : targets) {
        const ModelObject* mo = model.objects[ov.first];
        const ModelVolume* mv = mo->volumes[ov.second];
        MeshJobItem        it;
        it.object_id = mo->id();
        it.volume_id = mv->id();
        it.source    = std::make_shared<const TriangleMesh>(mv->mesh());
        it.name      = mv->name;
        items->push_back(std::move(it));
    }
    if (items->empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "no part to work on", {{"path", "/target"}});

    ctx.snapshot(label);
    std::shared_ptr<Deferred> done = ctx.defer();
    std::thread([items, compute, done]() {
        for (MeshJobItem& it : *items) {
            try {
                it.result = compute(*it.source, it.error);
            } catch (const std::exception& ex) {
                it.error = ex.what();
            }
        }
        wxGetApp().CallAfter([items, done]() {
            Model& model = wxGetApp().model();
            json   out   = json::array();
            std::set<int> changed_objects;
            int    changed = 0;
            for (MeshJobItem& it : *items) {
                const int obj_idx = parts::object_index_by_object_id(model, it.object_id);
                const int vol_idx = obj_idx < 0 ? -1 : parts::volume_index_by_object_id(*model.objects[obj_idx], it.volume_id);
                json      j       = {{"name", it.name}};
                if (vol_idx < 0) {
                    j["error"] = "the part was removed while the job ran";
                } else if (!it.error.empty()) {
                    j        = volume_ref_json(model, obj_idx, vol_idx);
                    j["error"] = it.error;
                } else if (!it.result.has_value()) {
                    j            = volume_ref_json(model, obj_idx, vol_idx);
                    j["changed"] = false;
                } else {
                    const size_t before = it.source->facets_count();
                    ModelVolume& v      = *model.objects[obj_idx]->volumes[vol_idx];
                    apply_mesh(obj_idx, v, std::move(*it.result));
                    changed_objects.insert(obj_idx);
                    ++changed;
                    j                 = volume_ref_json(model, obj_idx, vol_idx);
                    j["changed"]      = true;
                    j["facetsBefore"] = before;
                    j["facetsAfter"]  = v.mesh().facets_count();
                }
                out.push_back(j);
            }
            Plater* plater = wxGetApp().plater();
            for (int obj_idx : changed_objects) {
                ModelObject* mo = model.objects[obj_idx];
                mo->invalidate_bounding_box();
                mo->ensure_on_bed();
                // fix hollowing, sla support points, modifiers, ...
                plater->changed_mesh(obj_idx);
                if (ObjectList* list = parts::object_list()) {
                    list->update_item_error_icon(obj_idx, -1);
                    list->update_info_items(size_t(obj_idx));
                }
            }
            InvokeResult r = InvokeResult::ok({{"changed", changed}, {"volumes", out}});
            if (changed == 0)
                r.with_effects({});
            done->finish(std::move(r));
        });
    }).detach();
    return InvokeResult::started();
}

// ---------------------------------------------------------------- part.simplify

InvokeResult run_part_simplify(InvokeContext& ctx)
{
    const auto  targets = mesh_targets(ctx);
    const json& p       = ctx.params;
    double      ratio   = p.contains("ratio") ? p["ratio"].get<double>() : 0.;
    int         count   = p.contains("triangles") ? p["triangles"].get<int>() : 0;
    // The tool's "Detail level" (GLGizmoSimplify): collapse edges while the quadric error stays
    // under max_error, with no target count.
    float max_error = -1.f;
    if (p.contains("detail")) {
        const std::string d = p["detail"].get<std::string>();
        max_error           = d == "extraHigh" ? 1e-3f : d == "high" ? 1e-2f : d == "medium" ? 0.1f : d == "low" ? 0.5f : 1.f;
    } else if (p.contains("maxError"))
        max_error = float(p["maxError"].get<double>());
    if (ratio <= 0. && count <= 0 && max_error < 0.f)
        return InvokeResult::error(WAB_BAD_PARAMS, "give ratio, triangles, detail or maxError", {{"path", "/params"}, {"needs", "ratio"}});
    return start_mesh_job(ctx, "Simplify", targets, [ratio, count, max_error](const TriangleMesh& src, std::string&) -> std::optional<TriangleMesh> {
        indexed_triangle_set its    = src.its;
        const size_t         before = its.indices.size();
        if (max_error >= 0.f) {
            float err = max_error;
            its_quadric_edge_collapse(its, 0, &err, nullptr, nullptr);
            if (its.indices.size() >= before)
                return std::nullopt;
            return TriangleMesh(std::move(its));
        }
        uint32_t wanted = count > 0 ? uint32_t(count) : uint32_t(std::max<double>(4., std::round(double(before) * ratio)));
        if (wanted >= before)
            return std::nullopt; // already at or below the target
        float no_limit = std::numeric_limits<float>::max();
        its_quadric_edge_collapse(its, wanted, &no_limit, nullptr, nullptr);
        return TriangleMesh(std::move(its));
    });
}

// ---------------------------------------------------------------- part.smooth

InvokeResult run_part_smooth(InvokeContext& ctx)
{
    const auto targets     = mesh_targets(ctx);
    const bool allow_large = ctx.params.value("allowLarge", false);
    // The menu warns past a million faces after subdivision (each pass quadruples them).
    if (!allow_large)
        for (const auto& ov : targets) {
            const ModelVolume* v = ctx.model().objects[ov.first]->volumes[ov.second];
            if (v->mesh().facets_count() * 4 > 1000000)
                return InvokeResult::error(WAB_TOO_LARGE, "\"" + v->name + "\" would exceed one million faces; pass allowLarge to go ahead",
                                           {{"path", "/params/allowLarge"}, {"facetsAfter", v->mesh().facets_count() * 4}});
        }
    return start_mesh_job(ctx, "Subdivision mesh", targets, [](const TriangleMesh& src, std::string& error) -> std::optional<TriangleMesh> {
        bool         ok     = false;
        TriangleMesh result = TriangleMeshDeal::smooth_triangle_mesh(src, ok);
        if (!ok) {
            error = "the mesh has errors; repair it first (part.fix)";
            return std::nullopt;
        }
        return result;
    });
}

// ---------------------------------------------------------------- part.fix

InvokeResult run_part_fix(InvokeContext& ctx)
{
    const auto targets = mesh_targets(ctx);
    return start_mesh_job(ctx, "Repairing model object", targets, [](const TriangleMesh& src, std::string& error) -> std::optional<TriangleMesh> {
        // The same repair as Fix Model (FixModelByCgal): nothing to do when the mesh already is a closed,
        // consistently oriented solid without self-intersections.
        TriangleMesh    mesh = src;
        MeshRepairStats stats;
        std::string     err;
        if (!repair_mesh(mesh, &stats, &err)) {
            error = err.empty() ? "repair failed" : err;
            return std::nullopt;
        }
        if (!stats.modified)
            return std::nullopt;
        return mesh;
    });
}

// ---------------------------------------------------------------- part.boolean

InvokeResult run_part_boolean(InvokeContext& ctx)
{
    Model& model = ctx.model();
    if (ctx.targets.volumes.size() != 2)
        return InvokeResult::error(WAB_BAD_PARAMS, "part.boolean needs exactly two parts of one object: the first is kept, the second is the tool",
                                   {{"path", "/target"}});
    const auto a = ctx.targets.volumes[0];
    const auto b = ctx.targets.volumes[1];
    if (a.first != b.first || a.second == b.second)
        return InvokeResult::error(WAB_BAD_PARAMS, "the two parts must be different parts of the same object", {{"path", "/target"}});

    const std::string op        = ctx.params.value("op", "union");
    // The tool's "Delete input" is off by default: the tool part stays (a union always consumes it).
    const bool        keep_tool = op != "union" && ctx.params.value("keepTool", true);
    const char*       mcut_op   = op == "union" ? "UNION" : op == "difference" ? "A_NOT_B" : op == "intersection" ? "INTERSECTION" : nullptr;
    if (mcut_op == nullptr)
        return InvokeResult::error(WAB_BAD_PARAMS, "unknown op", {{"path", "/params/op"}});

    const int    obj_idx = a.first;
    ModelObject& object  = *model.objects[obj_idx];
    ModelVolume* src     = object.volumes[a.second];
    ModelVolume* tool    = object.volumes[b.second];

    // As GLGizmoMeshBoolean: both meshes in the object's frame.
    TriangleMesh src_mesh = src->mesh();
    src_mesh.transform(src->get_matrix());
    TriangleMesh tool_mesh = tool->mesh();
    tool_mesh.transform(tool->get_matrix());
    std::vector<TriangleMesh> results;
    try {
        MeshBoolean::mcut::make_boolean(src_mesh, tool_mesh, results, mcut_op);
    } catch (const std::exception& ex) {
        return InvokeResult::error(WAB_INTERNAL, std::string("the boolean operation failed: ") + ex.what());
    }
    if (results.empty() || results.front().empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "the boolean operation produced nothing; the parts may not overlap or a mesh is not closed",
                                   {{"path", "/target"}});

    // Paint survives: from both meshes for union and intersection, from the kept part for a difference.
    auto saved_of = [](const ModelVolume* v) {
        std::optional<TriangleSelector::SavedPainting> s = v->save_painting();
        if (s)
            s->mesh.transform(v->get_matrix(), true);
        return s;
    };
    std::vector<std::optional<TriangleSelector::SavedPainting>> paintings{saved_of(src)};
    if (op != "difference")
        paintings.push_back(saved_of(tool));

    ctx.snapshot("Mesh Boolean");
    Plater::SuppressSnapshots suppress(ctx.plater());

    // add_volume centers the mesh and puts the volume's origin at its center, in the object frame.
    ModelVolume* nv = object.add_volume(std::move(results.front()));
    for (const auto& s : paintings)
        nv->restore_painting(s, true);
    nv->name = src->name + " - " + op;
    nv->set_new_unique_id();
    nv->config.apply(src->config);
    nv->set_type(src->type());
    nv->set_material_id(src->material_id());

    // Remove the source and, unless kept, the tool (higher index first).
    std::vector<int> to_delete{a.second};
    if (!keep_tool)
        to_delete.push_back(b.second);
    std::sort(to_delete.rbegin(), to_delete.rend());
    for (int idx : to_delete)
        object.delete_volume(size_t(idx));
    object.invalidate_bounding_box();
    Slic3r::save_object_mesh(object);

    parts::refresh_object_after_volume_change(obj_idx, nv);
    const int vol_idx = parts::volume_index_in(object, nv);
    return InvokeResult::ok({{"volume", parts::volume_result_json(model, obj_idx, vol_idx)}, {"toolKept", keep_tool}});
}

// ---------------------------------------------------------------- object.cut

// What GLGizmoCut3D's update_object_cut_id does: cut info is kept only when every piece stays.
void update_cut_id(CutObjectBase& cut_id, ModelObjectCutAttributes attributes, int dowels)
{
    if (!attributes.has(ModelObjectCutAttribute::KeepUpper) || !attributes.has(ModelObjectCutAttribute::KeepLower) ||
        attributes.has(ModelObjectCutAttribute::InvalidateCutInfo))
        return;
    if (cut_id.id().invalid())
        cut_id.init();
    // two pieces (plus one object per dowel) from one
    cut_id.increase_check_sum(size_t(1 + (attributes.has(ModelObjectCutAttribute::CreateDowels) ? dowels : 0)));
}

// GLGizmoCut3D's synchronize_model_after_cut.
void sync_cut_ids(Model& model, const CutObjectBase& cut_id)
{
    for (ModelObject* obj : model.objects)
        if (obj->is_cut() && obj->cut_id.has_same_id(cut_id) && !obj->cut_id.is_equal(cut_id))
            obj->cut_id.copy(cut_id);
}

// GLGizmoCut3D::get_connector_mesh. A snap's proportions are per connector here (the tool has one
// pair for all of them).
indexed_triangle_set connector_mesh(const CutConnectorAttributes& a, float snap_space, float snap_bulge)
{
    int sectors = 1;
    switch (a.shape) {
    case CutConnectorShape::Triangle: sectors = 3; break;
    case CutConnectorShape::Square: sectors = 4; break;
    case CutConnectorShape::Circle: sectors = 360; break;
    case CutConnectorShape::Hexagon: sectors = 6; break;
    default: break;
    }
    if (a.type == CutConnectorType::Snap)
        return its_make_snap(1.0, 1.0, snap_space, snap_bulge);
    if (a.style == CutConnectorStyle::Prism)
        return its_make_cylinder(1.0, 1.0, (2 * PI / sectors));
    if (a.type == CutConnectorType::Plug)
        return its_make_frustum(1.0, 1.0, (2 * PI / sectors));
    return its_make_frustum_dowel(1.0, 1.0, sectors);
}

struct ConnectorSpec
{
    Vec3d                  point; // on the plane, plate frame
    CutConnectorAttributes attribs;
    float                  radius{2.5f}, height{5.f}, radius_tolerance{0.f}, height_tolerance{0.1f};
    float                  z_angle{0.f}; // radians, the tool's connector Rotation
    float                  snap_space{CutConnectorParas().snap_space_proportion};
    float                  snap_bulge{CutConnectorParas().snap_bulge_proportion};
};

// Adds the connectors as negative volumes, as GLGizmoCut3D::apply_connectors_in_model +
// apply_cut_connectors do: plugs and snaps sit half their depth past the plane on the upper side
// (the cut adds them to one piece and cuts them from the other); a dowel becomes its own object.
int add_connectors(ModelObject& mo, const std::vector<ConnectorSpec>& specs, const Vec3d& inst_offset, const Vec3d& normal,
                   const Transform3d& rotation_m)
{
    int    dowels = 0;
    size_t id     = mo.cut_id.connectors_cnt();
    for (const ConnectorSpec& s : specs) {
        // The tool's connector frame: plate coordinates relative to the instance offset.
        Vec3d pos    = s.point - inst_offset;
        float height = s.height;
        if (s.attribs.type == CutConnectorType::Dowel) {
            if (s.attribs.style == CutConnectorStyle::Prism)
                height *= 2;
            ++dowels;
        } else
            pos += normal * 0.5 * double(height);
        ModelVolume* v = mo.add_volume(TriangleMesh(connector_mesh(s.attribs, s.snap_space, s.snap_bulge)), ModelVolumeType::NEGATIVE_VOLUME);
        v->set_transformation(Geometry::translation_transform(pos) * rotation_m * Geometry::rotation_transform(-double(s.z_angle) * Vec3d::UnitZ()) *
                              Geometry::scale_transform(Vec3d(s.radius, s.radius, height)));
        v->cut_info = {s.attribs.type, s.radius_tolerance, s.height_tolerance};
        v->name     = "Connector-" + std::to_string(++id);
    }
    mo.cut_id.increase_connectors_cnt(specs.size());
    return dowels;
}

// Cut by contour (the tool's right-click on a part): what GLGizmoCut3D::PartSelection builds. The
// object is cut into parts, each part split into its connected pieces, and every piece starts on
// the side of the plane it lies on. `assign` then moves the pieces nearest the given points to the
// side asked for. Returns the object the cut then runs on (inside `holder`) and the pieces' sides.
struct ContourSelection
{
    Model                  holder;
    std::vector<Cut::Part> parts;
    ModelObject*           object() { return holder.objects.empty() ? nullptr : holder.objects.front(); }
};

std::string build_contour_selection(const ModelObject& mo, int inst, const Transform3d& cut_matrix, const Vec3d& point,
                                    const Vec3d& normal, const json& assign, ContourSelection& sel, json& report)
{
    {
        Cut cut(&mo, inst, cut_matrix); // KeepUpper | KeepLower | KeepAsParts
        const ModelObjectPtrs& res = cut.perform_with_plane();
        if (res.empty())
            return "the cut produced nothing";
        sel.holder.add_object(*res.front());
    }
    ModelObject*           so      = sel.object();
    const ModelVolumePtrs& volumes = so->volumes;
    for (int id = int(volumes.size()) - 1; id >= 0; --id)
        if (volumes[id]->is_splittable())
            volumes[id]->split(1, false); // paint is remapped by perform_by_contour
    const Transform3d inst_m = so->instances[inst]->get_matrix();
    for (const ModelVolume* v : volumes) {
        Cut::Part part{true, !v->is_model_part()};
        const Transform3d tr   = (inst_m * v->get_matrix()).inverse();
        const Vec3f       pos  = (tr * point).cast<float>();
        const Vec3f       norm = (tr.linear().inverse().transpose() * normal).cast<float>();
        for (const Vec3f& vx : v->mesh().its.vertices) {
            const double d = (vx - pos).dot(norm);
            if (std::abs(d) > EPSILON) {
                part.selected = d > 0.;
                break;
            }
        }
        sel.parts.push_back(part);
    }
    report = json::array();
    for (size_t i = 0; i < assign.size(); ++i) {
        const json&                a  = assign[i];
        const std::optional<Vec3d> pt = parts::vec3_param(a, "pointMm");
        if (!pt)
            return "assign[" + std::to_string(i) + "] needs pointMm [x, y, z]";
        int    best   = -1;
        double best_d = std::numeric_limits<double>::max();
        for (size_t k = 0; k < volumes.size(); ++k) {
            if (sel.parts[k].is_modifier)
                continue;
            const Transform3d tr = inst_m * volumes[k]->get_matrix();
            const AABBMesh    aabb(volumes[k]->mesh());
            const Vec3d       local = tr.inverse() * *pt;
            // Distance in the plate frame: map the closest local point back.
            int         fi = -1;
            Vec3d       closest;
            aabb.squared_distance(local, fi, closest);
            const double d = ((tr * closest) - *pt).norm();
            if (d < best_d) {
                best_d = d;
                best   = int(k);
            }
        }
        if (best < 0)
            return "the cut left no part to assign";
        const bool upper    = a.value("side", "upper") == "upper";
        sel.parts[best].selected = upper;
        report.push_back({{"piece", best}, {"side", upper ? "upper" : "lower"}, {"distanceMm", best_d}});
    }
    return {};
}

InvokeResult run_object_cut(InvokeContext& ctx)
{
    Model& model = ctx.model();
    if (ctx.targets.objects.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "object.cut needs exactly one object", {{"path", "/target"}});
    const int    obj_idx  = ctx.targets.objects.front();
    ModelObject* mo       = model.objects[obj_idx];
    const json&  p        = ctx.params;
    const int    inst_idx = p.value("instance", 0);
    if (inst_idx < 0 || inst_idx >= int(mo->instances.size()))
        return InvokeResult::error(WAB_BAD_PARAMS, "no such instance", {{"path", "/params/instance"}});
    // Connectors placed in the Cut tool stay on the object after the tool closes (only its Reset
    // clears them). The gizmo guard means the tool is not open now, so they are leftovers: this
    // cut uses the connectors in params and drops those (after the snapshot, so undo restores them).
    const size_t pending_connectors = mo->cut_connectors.size();

    // The plane, in plate coordinates.
    Vec3d point, normal;
    if (p.contains("z")) {
        point  = Vec3d(0., 0., p["z"].get<double>());
        normal = Vec3d::UnitZ();
        const BoundingBoxf3 bb = mo->instance_bounding_box(size_t(inst_idx));
        point.x()              = bb.center().x();
        point.y()              = bb.center().y();
    } else {
        const json& plane = p["plane"];
        auto pt = parts::vec3_param(plane, "point");
        auto n  = parts::vec3_param(plane, "normal");
        if (!pt || !n || n->norm() < 1e-9)
            return InvokeResult::error(WAB_BAD_PARAMS, "plane needs point and a non-zero normal", {{"path", "/params/plane"}});
        point  = *pt;
        normal = n->normalized();
    }
    // The plane must cross the object or one side would be empty.
    {
        const BoundingBoxf3 bb = mo->instance_bounding_box(size_t(inst_idx));
        double lo = std::numeric_limits<double>::max(), hi = -lo;
        for (int i = 0; i < 8; ++i) {
            const Vec3d c((i & 1) ? bb.max.x() : bb.min.x(), (i & 2) ? bb.max.y() : bb.min.y(), (i & 4) ? bb.max.z() : bb.min.z());
            const double d = (c - point).dot(normal);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
        if (lo >= 0. || hi <= 0.)
            return InvokeResult::error(WAB_BAD_PARAMS, "the cut plane does not cross the object", {{"path", p.contains("z") ? "/params/z" : "/params/plane"}});
    }

    const bool dovetail    = p.value("mode", "planar") == "dovetail";
    const bool has_assign  = p.contains("assign") && !p["assign"].empty();
    const auto connectors_p = p.find("connectors");
    const bool has_connectors = connectors_p != p.end() && !connectors_p->empty();
    if (dovetail && has_connectors)
        return InvokeResult::error(WAB_BAD_PARAMS, "a dovetail cut takes no connectors (the tool disables them in that mode)",
                                   {{"path", "/params/connectors"}});
    if (dovetail && p.value("keepAsParts", false))
        return InvokeResult::error(WAB_BAD_PARAMS, "a dovetail cut keeps the pieces as separate objects; keepAsParts does not apply",
                                   {{"path", "/params/keepAsParts"}});
    if (dovetail && has_assign)
        return InvokeResult::error(WAB_BAD_PARAMS, "assign (cut by contour) is for planar cuts", {{"path", "/params/assign"}});
    if (has_assign && p.value("keepAsParts", false))
        return InvokeResult::error(WAB_BAD_PARAMS, "assign moves parts between two objects; keepAsParts does not apply",
                                   {{"path", "/params/keepAsParts"}});

    const double            spin_deg   = p.value("spinDeg", 0.);
    const Transform3d       rotation_m = parts::cut_rotation(normal, spin_deg);

    // Connectors: positions are projected onto the plane.
    std::vector<ConnectorSpec> connectors;
    if (has_connectors) {
        for (size_t i = 0; i < connectors_p->size(); ++i) {
            const json& c   = (*connectors_p)[i];
            const Vec3d pos = parts::vec3_param(c, "positionMm").value_or(mo->instance_bounding_box(size_t(inst_idx)).center());
            ConnectorSpec     s;
            s.point                 = pos - normal * (pos - point).dot(normal);
            const std::string type  = c.value("type", "plug");
            const std::string style = c.value("style", "prism");
            const std::string shape = c.value("shape", "circle");
            s.attribs = CutConnectorAttributes(type == "dowel" ? CutConnectorType::Dowel :
                                               type == "snap"  ? CutConnectorType::Snap :
                                                                 CutConnectorType::Plug,
                                               style == "frustum" ? CutConnectorStyle::Frustum : CutConnectorStyle::Prism,
                                               shape == "square"   ? CutConnectorShape::Square :
                                               shape == "hexagon"  ? CutConnectorShape::Hexagon :
                                               shape == "triangle" ? CutConnectorShape::Triangle :
                                                                     CutConnectorShape::Circle);
            s.radius           = float(0.5 * c.value("diameterMm", 5.));
            s.height           = float(c.value("depthMm", 5.));
            s.radius_tolerance = float(0.5 * c.value("diameterToleranceMm", 0.));
            s.height_tolerance = float(c.value("depthTolerance", 0.1));
            s.z_angle          = float(c.value("rotationDeg", 0.) * M_PI / 180.);
            s.snap_space       = float(c.value("spacePct", 30.) / 100.);
            s.snap_bulge       = float(c.value("bulgePct", 15.) / 100.);
            // The tool caps Bulge at the Space value (render_snap_specific_input).
            if (s.snap_bulge > s.snap_space)
                return InvokeResult::error(WAB_BAD_PARAMS, "a snap's bulgePct cannot exceed its spacePct",
                                           {{"path", "/params/connectors/" + std::to_string(i) + "/bulgePct"}});
            connectors.push_back(s);
        }
    }

    // With connectors the tool always keeps both pieces as separate objects.
    const std::string keep          = has_connectors ? std::string("both") : p.value("keep", "both");
    const bool        keep_as_parts = !has_connectors && p.value("keepAsParts", false) && keep == "both";
    const bool        place_both    = p.value("placeOnCut", false);
    parts::CutPieceFlags upper{keep != "lower", place_both || p.value("placeOnCutUpper", false), p.value("flipUpper", false)};
    parts::CutPieceFlags lower{keep != "upper", place_both || p.value("placeOnCutLower", false), p.value("flipLower", false)};
    for (const auto& [flags, name] : {std::make_pair(upper, "upper"), std::make_pair(lower, "lower")})
        if (std::string why = parts::check_cut_piece(flags, name, keep_as_parts); !why.empty())
            return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params"}});
    const bool keep_painting = wxGetApp().app_config->get_bool("keep_painting");

    // Dovetail: the tool's groove, defaults from the object's size (GLGizmoCut3D::update_bb).
    Cut::Groove groove;
    int         groove_count = 1;
    float       groove_gap   = 10.f;
    const BoundingBoxf3 inst_bb = mo->instance_bounding_box(size_t(inst_idx));
    if (dovetail) {
        double depth = 0., width = 0.;
        parts::default_groove(inst_bb.size(), depth, width);
        const json g = p.value("groove", json::object());
        groove.depth = groove.depth_init = float(g.value("depthMm", depth));
        groove.width = groove.width_init = float(g.value("widthMm", g.contains("depthMm") ? 4. * groove.depth : width));
        groove.flaps_angle = groove.flaps_angle_init = float(g.value("flapAngleDeg", 60.) * M_PI / 180.);
        groove.angle = groove.angle_init = float(g.value("angleDeg", 0.) * M_PI / 180.);
        groove.depth_tolerance = float(g.value("depthTolMm", 0.1));
        groove.width_tolerance = float(g.value("widthTolMm", 0.1));
        groove_count           = g.value("count", 1);
        groove_gap             = float(g.value("gapMm", 10.));
        if (!parts::groove_flaps_fit(groove.depth, groove.width, groove.flaps_angle))
            return InvokeResult::error(WAB_BAD_PARAMS, "the groove's flaps do not fit its width: widen it, make it shallower or raise flapAngleDeg",
                                       {{"path", "/params/groove"}});
    }

    ModelObjectCutAttributes attributes = only_if(upper.keep, ModelObjectCutAttribute::KeepUpper) |
                                          only_if(lower.keep, ModelObjectCutAttribute::KeepLower) |
                                          only_if(keep_as_parts, ModelObjectCutAttribute::KeepAsParts) |
                                          only_if(upper.place_on_cut, ModelObjectCutAttribute::PlaceOnCutUpper) |
                                          only_if(lower.place_on_cut, ModelObjectCutAttribute::PlaceOnCutLower) |
                                          only_if(upper.flip, ModelObjectCutAttribute::FlipUpper) |
                                          only_if(lower.flip, ModelObjectCutAttribute::FlipLower) |
                                          only_if(keep_painting, ModelObjectCutAttribute::KeepPaint);

    // GLGizmoCut3D::get_cut_matrix: the plane's center relative to the instance offset, +Z along
    // the normal (the "upper" piece is on the normal's side), X as spun.
    const Vec3d       inst_offset = mo->instances[inst_idx]->get_offset();
    const Transform3d cut_matrix  = Geometry::translation_transform(point - inst_offset) * rotation_m;

    // Cut by contour: the object the cut runs on is the part selection's (GLGizmoCut3D::perform_cut).
    ContourSelection contour;
    json             assigned;
    if (has_assign) {
        if (std::string why = build_contour_selection(*mo, inst_idx, cut_matrix, point, normal, p["assign"], contour, assigned); !why.empty())
            return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params/assign"}});
        const bool one_side = std::all_of(contour.parts.begin(), contour.parts.end(), [&](const Cut::Part& part) {
            return part.is_modifier || part.selected == contour.parts.front().selected;
        });
        if (contour.parts.size() < 2 || one_side)
            return InvokeResult::error(WAB_BAD_PARAMS, "with that assignment every piece is on one side, so nothing would be cut",
                                       {{"path", "/params/assign"}, {"assigned", assigned}});
    }
    ModelObject* cut_mo = has_assign ? contour.object() : mo;

    ctx.snapshot("Cut by Plane");
    Plater::SuppressSnapshots suppress(ctx.plater());
    mo->cut_connectors.clear();

    int dowels = 0;
    if (has_connectors) {
        dowels = add_connectors(*cut_mo, connectors, inst_offset, normal, rotation_m);
        if (dowels > 0)
            attributes = attributes | ModelObjectCutAttribute::CreateDowels;
    }
    if (!has_connectors && !dovetail && cut_mo->cut_id.id().invalid())
        attributes = attributes | ModelObjectCutAttribute::InvalidateCutInfo;
    update_cut_id(cut_mo->cut_id, attributes, dowels);
    Cut                    cut(cut_mo, inst_idx, cut_matrix, attributes);
    const ModelObjectPtrs& new_objects = has_assign ? cut.perform_by_contour(mo, contour.parts, dowels) :
                                         dovetail   ? cut.perform_with_groove(groove, rotation_m, groove_count, groove_gap,
                                                                              float(inst_bb.radius())) :
                                                      cut.perform_with_plane();
    if (new_objects.empty())
        return InvokeResult::error(WAB_INTERNAL, "the cut produced nothing");

    // Report what the cut tool would offer to repair instead of asking.
    int open_edges = 0;
    for (const ModelObject* o : new_objects)
        for (const ModelVolume* v : o->volumes)
            open_edges += its_num_open_edges(v->mesh().its);

    const CutObjectBase cut_id = cut_mo->cut_id;
    const size_t        count  = new_objects.size();
    ctx.plater()->apply_cut_object_to_model(size_t(obj_idx), new_objects);
    sync_cut_ids(model, cut_id);

    json objs = json::array();
    for (size_t i = model.objects.size() - count; i < model.objects.size(); ++i)
        objs.push_back(object_ref_json(model, int(i)));
    const Vec3d x_axis = rotation_m.linear() * Vec3d::UnitX();
    json r = {{"objects", objs}, {"planeXAxis", parts::vec3_json(x_axis)}};
    if (has_assign)
        r["assigned"] = assigned;
    if (pending_connectors > 0)
        r["pendingConnectorsDiscarded"] = pending_connectors;
    if (open_edges > 0) {
        r["openEdges"] = open_edges;
        r["hint"]      = "some pieces have open edges; part.fix repairs them";
    }
    return InvokeResult::ok(r);
}

// ---------------------------------------------------------------- object.invalidateCutInfo

InvokeResult run_object_invalidate_cut_info(InvokeContext& ctx)
{
    Model& model = ctx.model();
    if (ctx.targets.objects.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "object.invalidateCutInfo needs exactly one object", {{"path", "/target"}});
    const int obj_idx = ctx.targets.objects.front();
    if (!model.objects[obj_idx]->is_cut())
        return InvokeResult::error(WAB_BAD_PARAMS, "the object is not the result of a cut", {{"path", "/target"}});
    ObjectList* list = parts::object_list();
    if (list == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the object list is not available");
    ctx.snapshot("Invalidate cut info");
    {
        Plater::SuppressSnapshots suppress(ctx.plater());
        list->invalidate_cut_info_for_object(obj_idx);
    }
    return InvokeResult::ok({{"object", object_ref_json(model, obj_idx)}});
}

// ---------------------------------------------------------------- object.skeletonCutPlanes

// Running skeleton builds, so object.cancelSkeleton can stop them: job id -> (owning tab, flag).
// GUI thread only.
struct SkeletonJob
{
    const BridgeHost*                  host;
    std::shared_ptr<std::atomic<bool>> cancel;
};
std::map<std::string, SkeletonJob>& skeleton_jobs()
{
    static std::map<std::string, SkeletonJob> jobs;
    return jobs;
}

// The Skeleton tool's generator settings (GLGizmoCut3D::skeleton_params), from params, with the
// tool's defaults and slider ranges (the schema holds the ranges).
SkeletonParams skeleton_params_from(const json& p)
{
    SkeletonParams s;
    s.method              = p.value("method", "mcf") == "voxel" ? 1 : 0;
    s.quality_speed       = float(p.value("contractionRate", double(s.quality_speed)));
    s.voxel_resolution    = p.value("voxelResolution", s.voxel_resolution);
    s.resample_edge_len   = float(p.value("resampleEdgeLenMm", double(s.resample_edge_len)));
    const std::string bores = p.value("traceBores", "threshold");
    s.tunnel_fill         = bores == "off" ? TunnelFill::Off : bores == "always" ? TunnelFill::On : TunnelFill::Threshold;
    const std::string root = p.value("bedRoot", "auto");
    s.bed_root            = root == "middle" ? BedRootMode::Middle : root == "tips" ? BedRootMode::Tips : BedRootMode::Auto;
    s.collapse_to_spine   = p.value("collapseToSpine", s.collapse_to_spine);
    s.correct_convergence = p.value("correctConvergence", s.correct_convergence);
    s.smooth_iterations   = p.value("smoothIterations", s.smooth_iterations);
    s.smooth_alpha        = float(p.value("smoothAlpha", double(s.smooth_alpha)));
    s.min_edge_factor     = float(p.value("minEdgeFactor", double(s.min_edge_factor)));
    s.tunnel_min_aspect   = float(p.value("boreAspectMin", double(s.tunnel_min_aspect)));
    s.decimate_faces      = p.value("faceBudget", s.decimate_faces);
    s.prune_leaves        = float(p.value("pruneLeavesMm", double(s.prune_leaves)));
    s.tangent_W           = float(p.value("tangentBlend", double(s.tangent_W)));
    s.root_lean           = float(p.value("rootLeanDeg", double(s.root_lean)));
    s.root_ramp           = float(p.value("rootRampMm", double(s.root_ramp)));
    return s;
}

// The Skeleton tool: skeletonize the object (centreline, root, cut order) and author one
// declarative cut plane per skeleton edge ("Cut along pipe", GLGizmoSkeleton::perform_skeleton_cut),
// or only the plane of one edge in the cut order ("Cut here" after Step), or just report the planes
// (preview: the tool's Step through them without committing). The object stays whole; the planes
// are the ones slicer.setCutPlanes edits. Skeletonizing can take minutes on dense meshes, so it runs
// on a worker thread that object.cancelSkeleton can stop at the next stage boundary. The undo point
// is taken when the planes are written, so a cancelled or failed build leaves no empty undo step.
InvokeResult run_object_skeleton_cut_planes(InvokeContext& ctx)
{
    Model& model = ctx.model();
    if (ctx.targets.objects.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "object.skeletonCutPlanes needs exactly one object", {{"path", "/target"}});
    const int          obj_idx = ctx.targets.objects.front();
    const ModelObject* mo      = model.objects[obj_idx];
    const TriangleMesh mesh    = mo->raw_mesh();
    if (mesh.its.vertices.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "the object has no solid part", {{"path", "/target"}});
    const bool  replace = ctx.params.value("replace", false);
    const bool  preview = ctx.params.value("preview", false);
    const int   cut_at  = ctx.params.value("cutAt", -1);
    if (preview && (replace || cut_at >= 0))
        return InvokeResult::error(WAB_BAD_PARAMS, "preview changes nothing; it takes no replace or cutAt", {{"path", "/params/preview"}});
    const SkeletonParams params    = skeleton_params_from(ctx.params);
    const Transform3d    world     = mo->instances.front()->get_matrix();
    const ObjectID       object_id = mo->id();

    std::shared_ptr<InvokeContext> keep = ctx.self.lock();
    if (!preview && !keep)
        ctx.snapshot("Cut along pipe"); // no runtime to hold the context: take the undo point now
    std::shared_ptr<Deferred> done   = ctx.defer();
    auto                      cancel = std::make_shared<std::atomic<bool>>(false);
    skeleton_jobs()[done->job_id()]  = {ctx.host, cancel};
    auto its = std::make_shared<indexed_triangle_set>(mesh.its);
    std::thread([its, params, world, object_id, replace, preview, cut_at, done, cancel, keep]() {
        auto        result = std::make_shared<SkeletonResult>();
        std::string error;
        try {
            SkeletonProgress progress;
            progress.canceled = [cancel]() { return cancel->load(); };
            *result           = build_skeleton(*its, params, world, &progress);
        } catch (const std::exception& ex) {
            error = ex.what();
        }
        wxGetApp().CallAfter([result, error, object_id, replace, preview, cut_at, done, cancel, keep]() {
            skeleton_jobs().erase(done->job_id());
            if (cancel->load())
                return done->finish(InvokeResult::error(WAB_USER_CANCELLED, "the skeleton build was cancelled; nothing changed",
                                                        {{"reason", "cancelled by object.cancelSkeleton"}}));
            if (!error.empty())
                return done->finish(InvokeResult::error(WAB_INTERNAL, "skeletonizing failed: " + error));
            Model&    model   = wxGetApp().model();
            const int obj_idx = parts::object_index_by_object_id(model, object_id);
            if (obj_idx < 0)
                return done->finish(InvokeResult::error(WAB_NOT_FOUND, "the object was removed while its skeleton was computed"));
            if (result->cut_edges.empty())
                return done->finish(InvokeResult::error(WAB_BAD_PARAMS, "the skeleton has no edge to cut along (a compact part needs no cut planes)"));
            ModelObject*      mo = model.objects[obj_idx];
            const Transform3d m  = mo->instances.front()->get_matrix();
            const size_t      n  = result->cut_edges.size();
            auto plane_of = [&](size_t i) {
                const auto&            e  = result->cut_edges[i];
                const SkeletonCutPlane sp = skeleton_edge_cut_plane(result->dag, result->tangents, e.first, e.second);
                CutPlane               cp;
                cp.point  = sp.point;
                cp.normal = sp.normal;
                return cp;
            };
            json diag = {{"edges", n}, {"roots", result->roots.size()}, {"buildMs", result->build_ms}};
            if (!result->root_mode.empty())
                diag["rootMode"] = result->root_mode;
            if (!result->note.empty())
                diag["note"] = result->note;

            if (preview) {
                json planes = json::array();
                for (size_t i = 0; i < n; ++i) {
                    const CutPlane cp = plane_of(i);
                    planes.push_back({{"edge", i},
                                      {"point", parts::vec3_json(m * cp.point)},
                                      {"normal", parts::vec3_json(CutPlaneRules::world_normal(m, cp.normal))},
                                      {"tiltDeg", i < result->cut_tilt_deg.size() ? json(result->cut_tilt_deg[i]) : json(nullptr)}});
                }
                InvokeResult r = InvokeResult::ok({{"object", object_ref_json(model, obj_idx)}, {"frame", "world"}, {"planes", planes}, {"skeleton", diag}});
                return done->finish(std::move(r.with_effects({})));
            }
            if (cut_at >= int(n))
                return done->finish(InvokeResult::error(WAB_BAD_PARAMS, "cutAt is past the last edge; the skeleton has " + std::to_string(n),
                                                        {{"path", "/params/cutAt"}, {"edges", n}}));
            if (!parts::gizmos_closed())
                return done->finish(InvokeResult::error(WAB_BUSY, "a tool (gizmo) was opened while the skeleton was computed; close it and run again"));
            const size_t adding   = cut_at >= 0 ? 1 : n;
            const size_t existing = replace ? 0 : mo->cut_planes.planes.size();
            // The segmentation's arrangement mask is a uint64_t: at most 64 planes per object.
            if (existing + adding > 64)
                return done->finish(InvokeResult::error(WAB_TOO_LARGE, "the centreline would need " + std::to_string(adding) +
                                                                           " cut planes; at most 64 per object are supported",
                                                        {{"planes", adding}, {"existing", existing}}));
            if (keep)
                keep->snapshot(cut_at >= 0 ? "Cut at pipe step" : "Cut along pipe");
            const bool was_empty = mo->cut_planes.planes.empty() || replace;
            if (replace)
                mo->cut_planes.planes.clear();
            if (cut_at >= 0)
                mo->cut_planes.planes.push_back(plane_of(size_t(cut_at)));
            else
                for (size_t i = 0; i < n; ++i)
                    mo->cut_planes.planes.push_back(plane_of(i));
            if (was_empty)
                mo->cut_planes.enabled = true; // authoring the first planes implies the feature is wanted on, as in the tool

            Plater* plater = wxGetApp().plater();
            plater->schedule_background_process();
            if (ObjectList* list = parts::object_list())
                list->update_info_items(size_t(obj_idx));
            if (GLCanvas3D* canvas = plater->get_view3D_canvas3D(); canvas != nullptr) {
                canvas->set_as_dirty();
                canvas->request_extra_frame();
            }
            const bool by_group = wxGetApp().global_print_sequence() == PrintSequence::ByGroup;
            json       r        = {{"object", object_ref_json(model, obj_idx)},
                                   {"planesAdded", adding},
                                   {"planeCount", mo->cut_planes.planes.size()},
                                   {"printSequenceByGroup", by_group},
                                   {"skeleton", diag}};
            if (cut_at >= 0)
                r["cutAt"] = cut_at;
            if (!result->note.empty())
                r["note"] = result->note;
            if (!by_group)
                r["hint"] = "cut-plane printing needs the print setting print_sequence = \"by group\"; set it with slicer.setSettings";
            done->finish(InvokeResult::ok(r));
        });
    }).detach();
    return InvokeResult::started();
}

// ---------------------------------------------------------------- object.cancelSkeleton

// The Skeleton tool's Cancel while its centreline is being computed: stops this tab's running
// object.skeletonCutPlanes builds (or the one named) at the next stage boundary. The build's own
// request then finishes with WAB_USER_CANCELLED, having changed nothing.
InvokeResult run_object_cancel_skeleton(InvokeContext& ctx)
{
    const std::string job = ctx.params.value("jobId", "");
    json              ids = json::array();
    for (auto& [id, j] : skeleton_jobs())
        if (j.host == ctx.host && (job.empty() || job == id)) {
            j.cancel->store(true);
            ids.push_back(id);
        }
    if (ids.empty())
        return InvokeResult::error(WAB_NOT_FOUND, job.empty() ? "no skeleton build of this tab is running" : "no running skeleton build " + job,
                                   {{"reason", "nothing to cancel"}});
    return InvokeResult::ok({{"cancelled", ids}, {"note", "the build stops when its current stage ends"}}).with_effects({});
}

json volumes_target_spec() { return {{"kind", "volumes"}, {"required", true}}; }

} // namespace

void register_mesh_actions(Catalog& c)
{
    const json one_part_example = {{"target", {{"volume", {{"object", {{"index", 0}}}, {"index", 0}}}}}};

    // part.boolean
    {
        ActionDescriptor d;
        d.id      = "part.boolean";
        d.label   = "Mesh boolean";
        d.summary = "Combine two parts of one object into one: union, difference (the second part cut out of the first) "
                    "or intersection. The first part is replaced by the result; the second stays unless keepTool is false (a union consumes it).";
        d.target  = volumes_target_spec();
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"required", {"op"}},
                    {"properties",
                     {{"op", {{"type", "string"}, {"enum", {"union", "difference", "intersection"}}}},
                      {"keepTool", {{"type", "boolean"}, {"default", true},
                                    {"description", "Keep the second part after a difference or intersection, as the tool does unless "
                                                    "\"Delete input\" is ticked (a union always consumes it)."}}}}}};
        d.effects = {"geometry", "structure"};
        d.ui      = {"GLGizmosManager.cpp::gizmo::GLGizmoMeshBoolean",
                     "GLGizmoMeshBoolean.cpp::gizmoctl::expr:label.c_str()@on_render_input_window"};
        d.examples = json::parse(R"([{"setup": [{"id": "part.add", "target": {"object": {"index": 0}},
                      "params": {"type": "part", "shape": "sphere", "sizeMm": [16], "positionMm": [10, 10, 10], "relativeTo": "object"}}],
           "target": {"volumes": [{"object": {"index": 0}, "index": 0}, {"object": {"index": 0}, "index": 1}]},
           "params": {"op": "difference"}}])");
        c.add(d, run_part_boolean);
    }

    // part.simplify
    {
        ActionDescriptor d;
        d.id      = "part.simplify";
        d.label   = "Simplify Model";
        d.summary = "Reduce the triangle count of parts (or of every part of an object): to a fraction of the current count, "
                    "to a target count, or to a detail level (how far the surface may move). Runs in the background.";
        d.target  = volumes_target_spec();
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"x-exactlyOne", {{"ratio"}, {"triangles"}, {"detail"}, {"maxError"}}},
                    {"properties",
                     {{"ratio", {{"type", "number"}, {"minimum", 0.01}, {"maximum", 1},
                                 {"description", "Fraction of the triangles to KEEP. The Simplify tool's \"Decimate ratio\" is the percent "
                                                 "REMOVED: its 30% is ratio 0.7."}}},
                      {"triangles", {{"type", "integer"}, {"minimum", 4}, {"maximum", 50000000}, {"description", "Triangles to keep."}}},
                      {"detail", {{"type", "string"}, {"enum", {"extraHigh", "high", "medium", "low", "extraLow"}},
                                  {"description", "The tool's Detail level: the quadric error allowed (1e-3, 1e-2, 0.1, 0.5, 1); the count follows."}}},
                      {"maxError", {{"type", "number"}, {"minimum", 0.00001}, {"maximum", 10},
                                    {"description", "The quadric error allowed, as detail but any value."}}}}}};
        d.effects      = {"geometry"};
        d.long_running = true;
        d.timeout_ms   = 50000;
        d.ui           = {"GUI_Factories.cpp::menu::Simplify Model", "GLGizmosManager.cpp::gizmo::GLGizmoSimplify"};
        d.examples     = json::array({{{"target", one_part_example["target"]}, {"params", {{"ratio", 0.5}}}},
                                      {{"target", one_part_example["target"]}, {"params", {{"detail", "medium"}}}}});
        c.add(d, run_part_simplify);
    }

    // part.smooth
    {
        ActionDescriptor d;
        d.id      = "part.smooth";
        d.label   = "Subdivision mesh";
        d.summary = "Subdivide and smooth parts (each pass makes four triangles of one). Refuses past a million faces "
                    "unless allowLarge. Runs in the background.";
        d.target  = volumes_target_spec();
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"properties", {{"allowLarge", {{"type", "boolean"}, {"default", false}, {"description", "Go ahead past one million faces."}}}}}};
        d.effects      = {"geometry"};
        d.long_running = true;
        d.ui           = {"GUI_Factories.cpp::menu::Subdivision mesh"};
        d.examples     = json::array({{{"target", one_part_example["target"]}, {"params", json::object()}}});
        c.add(d, run_part_smooth);
    }

    // part.fix
    {
        ActionDescriptor d;
        d.id      = "part.fix";
        d.label   = "Fix Model";
        d.summary = "Repair parts whose meshes are not closed (open edges, holes) so they slice cleanly. Parts without "
                    "errors are left alone. Runs in the background.";
        d.target       = volumes_target_spec();
        d.effects      = {"geometry"};
        d.long_running = true;
        d.ui           = {"GUI_Factories.cpp::menu::Fix Model"};
        d.examples     = json::array({{{"target", one_part_example["target"]}, {"params", json::object()}}});
        c.add(d, run_part_fix);
    }

    // object.cut
    {
        ActionDescriptor d;
        d.id      = "object.cut";
        d.label   = "Cut";
        d.summary = "Cut an object with a plane: at a height (z) or through any point with any normal, flat (planar) or with a "
                    "dovetail groove. Keep both pieces (as two objects, or as parts of one), or only the upper / lower one; place "
                    "each piece on its cut face or flip it; join them with plug, dowel or snap connectors; or move disconnected "
                    "parts to the other side (assign, the tool's cut by contour).";
        d.target  = {{"kind", "object"}, {"required", true}};
        const json piece_note = "Per piece, placeOnCut and flip exclude each other (as in the tool); neither applies with keepAsParts.";
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"x-exactlyOne", {{"z"}, {"plane"}}},
                    {"description", piece_note},
                    {"properties",
                     {{"z", {{"type", "number"}, {"minimum", -1000}, {"maximum", 10000}, {"description", "Height of a horizontal cut above the bed, mm."}}},
                      {"plane", {{"type", "object"},
                                 {"additionalProperties", false},
                                 {"required", {"point", "normal"}},
                                 {"properties",
                                  {{"point", parts::schema_vec3("A point on the plane, plate coordinates in mm.")},
                                   {"normal", parts::schema_vec3("The plane's normal; the \"upper\" piece is on its side.")}}}}},
                      {"spinDeg", {{"type", "number"}, {"minimum", -360}, {"maximum", 360}, {"default", 0},
                                   {"description", "Turn the plane about its normal (the tool's rotation grabbers): orients a dovetail groove and "
                                                   "square, hexagon or triangle connectors. The reply gives the resulting planeXAxis."}}},
                      {"mode", {{"type", "string"}, {"enum", {"planar", "dovetail"}}, {"default", "planar"},
                                {"description", "dovetail: a tongue-and-groove joint along the plane (no connectors, no keepAsParts)."}}},
                      {"groove", {{"type", "object"},
                                  {"additionalProperties", false},
                                  {"description", "Dovetail groove; defaults as the tool picks them from the object's size."},
                                  {"properties",
                                   {{"depthMm", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 500}}},
                                    {"depthTolMm", {{"type", "number"}, {"minimum", 0}, {"maximum", 10}, {"default", 0.1}}},
                                    {"widthMm", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 1000}, {"description", "Default 4 x depth."}}},
                                    {"widthTolMm", {{"type", "number"}, {"minimum", 0}, {"maximum", 10}, {"default", 0.1}}},
                                    {"flapAngleDeg", {{"type", "number"}, {"minimum", 30}, {"maximum", 120}, {"default", 60}}},
                                    {"angleDeg", {{"type", "number"}, {"minimum", 0}, {"maximum", 15}, {"default", 0}, {"description", "Groove taper."}}},
                                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}, {"default", 1}, {"description", "Several grooves side by side."}}},
                                    {"gapMm", {{"type", "number"}, {"minimum", 0}, {"maximum", 1000}, {"default", 10}, {"description", "Between grooves."}}}}}}},
                      {"keep", {{"type", "string"}, {"enum", {"both", "upper", "lower"}}, {"default", "both"}}},
                      {"keepAsParts", {{"type", "boolean"}, {"default", false}, {"description", "Keep both pieces as parts of one object."}}},
                      {"placeOnCut", {{"type", "boolean"}, {"default", false}, {"description", "Lay both pieces on their cut faces."}}},
                      {"placeOnCutUpper", {{"type", "boolean"}, {"default", false}, {"description", "Lay the upper piece on its cut face."}}},
                      {"placeOnCutLower", {{"type", "boolean"}, {"default", false}, {"description", "Lay the lower piece on its cut face."}}},
                      {"flipUpper", {{"type", "boolean"}, {"default", false}, {"description", "Turn the upper piece over so its cut face is on the bed."}}},
                      {"flipLower", {{"type", "boolean"}, {"default", false}, {"description", "Turn the lower piece upside down."}}},
                      {"instance", {{"type", "integer"}, {"minimum", 0}, {"default", 0}, {"description", "Which instance's position the plane refers to."}}},
                      {"assign",
                       {{"type", "array"},
                        {"maxItems", 64},
                        {"description", "Cut by contour: after cutting, the object's disconnected pieces each go with the side of the "
                                        "plane they lie on; each entry moves the piece nearest pointMm to `side` (the tool's right-click)."},
                        {"items",
                         {{"type", "object"},
                          {"additionalProperties", false},
                          {"required", {"pointMm", "side"}},
                          {"properties",
                           {{"pointMm", parts::schema_vec3("A point on or near the piece, plate coordinates.")},
                            {"side", {{"type", "string"}, {"enum", {"upper", "lower"}}}}}}}}}},
                      {"connectors",
                       {{"type", "array"},
                        {"maxItems", 32},
                        {"description", "Plugs, dowels or snaps joining the two pieces (then both pieces are kept, as separate objects)."},
                        {"items",
                         {{"type", "object"},
                          {"additionalProperties", false},
                          {"properties",
                           {{"positionMm", parts::schema_vec3("Where on the cut face, plate coordinates (projected onto the plane); "
                                                               "default: where the plane crosses the object's center.")},
                            {"type", {{"type", "string"}, {"enum", {"plug", "dowel", "snap"}}, {"default", "plug"}}},
                            {"style", {{"type", "string"}, {"enum", {"prism", "frustum"}}, {"default", "prism"}}},
                            {"shape", {{"type", "string"}, {"enum", {"circle", "square", "hexagon", "triangle"}}, {"default", "circle"}}},
                            {"diameterMm", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 200}, {"default", 5}}},
                            {"depthMm", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 200}, {"default", 5}}},
                            {"diameterToleranceMm", {{"type", "number"}, {"minimum", 0}, {"maximum", 5}, {"default", 0}}},
                            {"depthTolerance", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"default", 0.1}}},
                            {"rotationDeg", {{"type", "number"}, {"minimum", 0}, {"maximum", 180}, {"default", 0},
                                             {"description", "Turn the connector about the plane normal (from the plane's X axis)."}}},
                            {"bulgePct", {{"type", "number"}, {"minimum", 5}, {"maximum", 50}, {"default", 15},
                                          {"description", "Snap: bulge, % of the radius; at most spacePct."}}},
                            {"spacePct", {{"type", "number"}, {"minimum", 10}, {"maximum", 50}, {"default", 30},
                                          {"description", "Snap: slit space, % of the radius."}}}}}}}}}}}};
        d.effects = {"geometry", "structure", "plates"};
        d.ui      = {"GLGizmosManager.cpp::gizmo::GLGizmoCut3D", "GLGizmoCut.cpp::gizmoctl::Perform cut",
                     "GLGizmoCut.cpp::gizmoctl::Flip cut plane", "GLGizmoCut.cpp::gizmoctl::Add connectors",
                     "GLGizmoCut.cpp::gizmoctl::Edit connectors", "GLGizmoCut.cpp::gizmoctl::Confirm connectors"};
        d.examples = json::array({{{"target", {{"object", {{"index", 0}}}}}, {"params", {{"z", 10}}}},
                                  {{"target", {{"object", {{"index", 0}}}}},
                                   {"params", {{"plane", {{"point", {128, 128, 10}}, {"normal", {1, 0, 1}}}}, {"keep", "lower"}}}},
                                  {{"target", {{"object", {{"index", 0}}}}},
                                   {"params", {{"z", 10}, {"connectors", {{{"positionMm", {128, 128, 10}}, {"type", "dowel"}, {"diameterMm", 4}}}}}}},
                                  {{"target", {{"object", {{"index", 0}}}}},
                                   {"params", {{"z", 10}, {"mode", "dovetail"}, {"spinDeg", 90}, {"groove", {{"depthMm", 3}, {"flapAngleDeg", 70}}}}}},
                                  {{"target", {{"object", {{"index", 0}}}}},
                                   {"params", {{"z", 10}, {"placeOnCutUpper", true}, {"flipLower", true}}}}});
        c.add(d, run_object_cut);
    }

    // object.skeletonCutPlanes
    {
        ActionDescriptor d;
        d.id      = "object.skeletonCutPlanes";
        d.label   = "Cut along pipe";
        d.summary = "Find an object's centreline (its skeleton) and add one cut plane per skeleton edge, so a bent "
                    "tube or limb prints in segments along its length; or add only one edge's plane (cutAt), or list the "
                    "planes without adding them (preview). Takes the Skeleton tool's generator settings. Runs in the "
                    "background; can take minutes (object.cancelSkeleton stops it).";
        d.target  = {{"kind", "object"}, {"required", true}};
        auto num  = [](double lo, double hi, double def, const char* what) {
            return json{{"type", "number"}, {"minimum", lo}, {"maximum", hi}, {"default", def}, {"description", what}};
        };
        auto integer = [](int lo, int hi, int def, const char* what) {
            return json{{"type", "integer"}, {"minimum", lo}, {"maximum", hi}, {"default", def}, {"description", what}};
        };
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"properties",
                     {{"method", {{"type", "string"}, {"enum", {"mcf", "voxel"}}, {"default", "mcf"},
                                  {"description", "mcf = mean-curvature flow (default, smoother), voxel = voxel thinning (faster on dense meshes)."}}},
                      {"replace", {{"type", "boolean"}, {"default", false}, {"description", "Replace the object's cut planes instead of adding to them."}}},
                      {"cutAt", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000},
                                 {"description", "Add only the plane of this edge in the cut order (0 = first; preview lists them): the tool's Step + Cut here."}}},
                      {"preview", {{"type", "boolean"}, {"default", false},
                                   {"description", "Only report the planes (world frame, with each one's tilt), changing nothing."}}},
                      {"contractionRate", num(0.01, 1, 0.1, "mcf: contraction rate (quality vs speed).")},
                      {"voxelResolution", integer(32, 256, 64, "voxel: grid resolution; cost is cubic in it.")},
                      {"resampleEdgeLenMm", num(0, 1000, 5, "Target edge length after resampling, mm (the segment height); 0 = off.")},
                      {"traceBores", {{"type", "string"}, {"enum", {"off", "threshold", "always"}}, {"default", "threshold"},
                                      {"description", "Trace the centreline of bores (pipes): never, when deep relative to their width, or always."}}},
                      {"bedRoot", {{"type", "string"}, {"enum", {"auto", "middle", "tips"}}, {"default", "auto"},
                                   {"description", "Where the root stands on the bed."}}},
                      {"collapseToSpine", {{"type", "boolean"}, {"default", false}, {"description", "Collapse side branches onto the main spine."}}},
                      {"correctConvergence", {{"type", "boolean"}, {"default", true}, {"description", "Fix branches that converge upward."}}},
                      {"smoothIterations", integer(0, 50, 10, "Smoothing passes over the centreline.")},
                      {"smoothAlpha", num(0, 1, 0.5, "Smoothing strength.")},
                      {"minEdgeFactor", num(0.0001, 0.02, 0.002, "mcf: minimum edge length, fraction of the bounding-box diagonal.")},
                      {"boreAspectMin", num(0.2, 5, 1.2, "traceBores threshold: bore depth / width needed.")},
                      {"faceBudget", integer(0, 40000, 0, "Decimate denser meshes to this many faces first (0 = off). Faster, but can change "
                                                          "the skeleton of organic parts; raise it when a build would not finish in time.")},
                      {"pruneLeavesMm", num(0, 1000, 0, "Remove branch tips shorter than this, mm (0 = off).")},
                      {"tangentBlend", num(0, 1, 0.5, "Blend of the cut normals toward the local tangent.")},
                      {"rootLeanDeg", num(0, 90, 45, "How far the cuts beside a middle root may lean toward flat.")},
                      {"rootRampMm", num(0, 1000, 0, "Distance over which that hold releases, mm (0 = auto).")}}}};
        d.effects      = {"geometry"};
        d.long_running = true;
        d.timeout_ms   = 50000;
        d.ui           = {"GLGizmosManager.cpp::gizmo::GLGizmoSkeleton", "GLGizmoSkeleton.cpp::gizmoctl::Cut along pipe",
                          "GLGizmoSkeleton.cpp::gizmoctl::Recompute centreline", "GLGizmoSkeleton.cpp::gizmoctl::Step",
                          "GLGizmoSkeleton.cpp::gizmoctl::Cut here", "GLGizmoSkeleton.cpp::gizmoctl::<", "GLGizmoSkeleton.cpp::gizmoctl::>"};
        d.examples     = json::array({{{"target", {{"object", {{"index", 0}}}}}, {"params", {{"method", "voxel"}}}},
                                      {{"target", {{"object", {{"index", 0}}}}}, {"params", {{"method", "voxel"}, {"preview", true}}}},
                                      {{"target", {{"object", {{"index", 0}}}}},
                                       {"params", {{"method", "voxel"}, {"cutAt", 0}, {"resampleEdgeLenMm", 8}, {"traceBores", "off"}}}}});
        c.add(d, run_object_skeleton_cut_planes);
    }

    // object.cancelSkeleton
    {
        ActionDescriptor d;
        d.id      = "object.cancelSkeleton";
        d.label   = "Cancel";
        d.summary = "Stop this tab's running object.skeletonCutPlanes build (or the one jobId names); it ends when its current "
                    "stage does and changes nothing.";
        d.risk    = Risk::Ui;
        d.target  = {{"kind", "none"}, {"required", false}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"properties", {{"jobId", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64},
                                               {"description", "The jobId object.skeletonCutPlanes answered with \"running\"; default: all of this tab's."}}}}}};
        d.effects  = {};
        d.ui       = {"GLGizmoSkeleton.cpp::gizmoctl::Cancel"};
        d.examples = json::array({{{"target", nullptr}, {"params", json::object()}}});
        c.add(d, run_object_cancel_skeleton, [](const InvokeContext& ctx) -> std::string {
            for (const auto& [id, j] : skeleton_jobs())
                if (j.host == ctx.host)
                    return {};
            return "no skeleton build is running";
        });
    }

    // object.invalidateCutInfo
    {
        ActionDescriptor d;
        d.id      = "object.invalidateCutInfo";
        d.label   = "Invalidate cut info";
        d.summary = "Forget that an object came from a cut, so its pieces can be edited (scaled, split) independently.";
        d.target  = {{"kind", "object"}, {"required", true}};
        d.effects = {"structure"};
        d.ui      = {"GUI_Factories.cpp::menu::expr:menu_name@append_menu_item_invalidate_cut_info"};
        d.examples = json::parse(R"([{"setup": [{"id": "object.cut", "target": {"object": {"index": 0}}, "params": {"z": 10, "connectors": [{}]}}],
           "target": {"object": {"index": 0}}, "params": {}}])");
        c.add(d, run_object_invalidate_cut_info,
              [](const InvokeContext& ctx) -> std::string {
                  // Live state needs a target; without one the runner answers instead.
                  if (ctx.targets.objects.size() == 1 && !ctx.model().objects[ctx.targets.objects.front()]->is_cut())
                      return "the object is not the result of a cut";
                  return {};
              });
    }
}

}}} // namespace Slic3r::GUI::Actions
