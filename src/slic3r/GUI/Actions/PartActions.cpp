// Automation actions: part.* actions: add / setType / setFilament / setTransform / split / merge,
// plus the "Add Primitive" and "Add Handy models" object menus (object.addShape,
// object.addHandyModel).
// Owner: work package S3 (parts). See docs/HLSD/bridge-actions.md for how to write an action and
// docs/bridge-actions/parts.md for these.
//
// Every runner follows the menu's own code path where it has no dialog in it, and re-implements
// the few lines around a dialog where it does (load_generic_subobject shows a tips dialog,
// set_volume_type an error box, split a message box): params replace what the dialog would ask.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "Register.hpp"
#include "PartsCommon.hpp"
#include "PartsCommonGui.hpp"

#include <cctype>
#include <map>
#include <set>
#include <tuple>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

// ---------------------------------------------------------------- schema pieces

json type_schema(const std::string& description, const std::string& dflt = "")
{
    json s = {{"type", "string"}, {"enum", parts::volume_type_names()}, {"description", description}};
    if (!dflt.empty())
        s["default"] = dflt;
    return s;
}

json size_schema()
{
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", 0.01}, {"maximum", 2000}}},
            {"minItems", 1},
            {"maxItems", 3},
            {"description", "Size in mm: one value = the largest dimension (uniform), three = the bounding box [x,y,z]. "
                            "Default: a tenth of the bed's largest side, as the menu does."}};
}

const std::vector<std::string> MODEL_EXTS = {"stl", "obj", "3mf", "amf"};

// The handy models the "Add Handy models" submenu offers (GUI_Factories.cpp), by wire name.
struct HandyModel
{
    const char*              name;
    std::vector<std::string> files;
    bool                     arrange;
};
const std::vector<HandyModel>& handy_models()
{
    static const std::vector<HandyModel> models = {
        {"orcaCube", {"OrcaCube_v2.drc", "OrcaPlug_v2.drc"}, true},
        {"orcaSlicedCombo", {"OrcaSliced.3mf", "OrcaCube_v2.drc", "OrcaPlug_v2.drc"}, true},
        {"orcaBadge", {"OrcaBadge.3mf"}, false},
        {"orcaToleranceTest", {"OrcaToleranceTest.drc"}, false},
        {"3dBenchy", {"3DBenchy.drc"}, false},
        {"caliCat", {"calicat.drc"}, false},
        {"autodeskFdmTest", {"ksr_fdmtest_v4.drc"}, false},
        {"voronCube", {"Voron_Design_Cube_v7.drc"}, false},
        {"stanfordBunny", {"Stanford_Bunny.drc"}, false},
        {"orcaStringHell", {"Orca_stringhell.drc"}, false},
    };
    return models;
}

std::vector<std::string> handy_model_names()
{
    std::vector<std::string> out;
    for (const HandyModel& m : handy_models())
        out.emplace_back(m.name);
    return out;
}

std::vector<double> size_param(const json& params)
{
    std::vector<double> out;
    if (auto it = params.find("sizeMm"); it != params.end() && it->is_array())
        for (const auto& v : *it)
            out.push_back(v.get<double>());
    return out;
}

double default_primitive_side()
{
    Plater* plater = wxGetApp().plater();
    if (plater != nullptr && plater->canvas3D() != nullptr)
        return plater->canvas3D()->get_size_proportional_to_max_bed_size(0.1);
    return 20.;
}

std::string shape_label(const std::string& shape)
{
    std::string s = shape;
    if (!s.empty())
        s[0] = char(std::toupper((unsigned char) s[0]));
    return s;
}

// Adds `mesh` to object `obj_idx` as a volume of `type`, placed (plate frame, instance 0) at
// params.positionMm / rotationDeg or, without a position, where the menu puts a new primitive.
// Takes no snapshot; the caller has. Returns the new volume.
ModelVolume* add_mesh_as_volume(Model& model, int obj_idx, TriangleMesh&& mesh, ModelVolumeType type, const std::string& name,
                                const json& params, bool from_builtin)
{
    ModelObject&        object  = *model.objects[obj_idx];
    const BoundingBoxf3 inst_bb = object.instance_bounding_box(0);
    const Vec3d         size    = mesh.bounding_box().size();
    const Transform3d   inst    = object.instances.front()->get_matrix();

    // The mesh is centered when added; the volume's origin is then its bounding-box center.
    ModelVolume* v = object.add_volume(std::move(mesh), type);

    Vec3d center = parts::default_part_center(inst_bb, size);
    if (auto pos = parts::vec3_param(params, "positionMm"))
        center = params.value("relativeTo", "plate") == "object" ? Vec3d(inst_bb.center() + *pos) : *pos;
    const Vec3d rotation = parts::vec3_param(params, "rotationDeg").value_or(Vec3d::Zero());
    // Undo the instance's rotation and scale so the new volume is aligned with the plate and has
    // its size in mm on the plate, as ObjectList::load_generic_subobject does.
    v->set_transformation(Geometry::Transformation(parts::volume_matrix_for_world(inst, center, rotation)));

    v->name = name;
    // A part inherits the object's filament, anything else starts with none.
    int extruder_id = 0;
    if (v->type() == ModelVolumeType::MODEL_PART && object.config.has("extruder"))
        extruder_id = object.config.opt_int("extruder");
    v->config.set_key_value("extruder", new ConfigOptionInt(extruder_id));
    v->source.is_from_builtin_objects = from_builtin;
    object.invalidate_bounding_box();
    // BBS: backup
    Slic3r::save_object_mesh(object);
    return v;
}

InvokeResult finish_added_volume(Model& model, int obj_idx, ModelVolume* v)
{
    ModelObject& object = *model.objects[obj_idx];
    if (v->is_model_part() && object.instances.front()->auto_drop)
        object.ensure_on_bed();
    parts::refresh_object_after_volume_change(obj_idx, v);
    const int vol_idx = parts::volume_index_in(object, v);
    return InvokeResult::ok({{"volume", parts::volume_result_json(model, obj_idx, vol_idx)}});
}

// ---------------------------------------------------------------- part.add

InvokeResult run_part_add(InvokeContext& ctx)
{
    if (ctx.targets.objects.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "part.add needs exactly one object", {{"path", "/target"}});
    Model&                    model   = ctx.model();
    const int                 obj_idx = ctx.targets.objects.front();
    const json&               p       = ctx.params;
    const std::string         shape   = p.value("shape", "cube");
    const ModelVolumeType     type    = parts::volume_type_from_name(p.value("type", "modifier"));
    const std::vector<double> size    = size_param(p);

    if (shape != "url") {
        TriangleMesh mesh = parts::make_primitive(shape, default_primitive_side());
        if (mesh.empty())
            return InvokeResult::error(WAB_BAD_PARAMS, "unknown shape", {{"path", "/params/shape"}});
        if (!size.empty() && !parts::fit_mesh_to_size(mesh, size))
            return InvokeResult::error(WAB_BAD_PARAMS, "sizeMm takes one value or three, all positive", {{"path", "/params/sizeMm"}});
        ctx.snapshot("Add primitive");
        Plater::SuppressSnapshots suppress(ctx.plater());
        const std::string name = p.contains("name") ? p["name"].get<std::string>() : "Generic-" + shape_label(shape);
        ModelVolume*      v    = add_mesh_as_volume(model, obj_idx, std::move(mesh), type, name, p, true);
        return finish_added_volume(model, obj_idx, v);
    }

    // shape "url": fetch the file on openProjectFromUrl's terms, then add it as a part.
    if (!p.contains("url"))
        return InvokeResult::error(WAB_BAD_PARAMS, "shape \"url\" needs url", {{"path", "/params/url"}, {"needs", "url"}});
    if (ctx.host == nullptr || !ctx.host->download)
        return InvokeResult::error(WAB_BAD_PARAMS, "downloads are unavailable in this tab", {{"path", "/params/url"}});

    // The undo point is taken now, so undoing this turn also removes what the download adds.
    ctx.snapshot("Load Part");
    const ObjectID            object_id = model.objects[obj_idx]->id();
    std::shared_ptr<Deferred> done      = ctx.defer();
    const json                params    = p;
    ctx.host->download(
        p["url"].get<std::string>(), MODEL_EXTS,
        [done, object_id, params, type, size](const std::string& path) {
            Model&    model   = wxGetApp().model();
            const int obj_idx = parts::object_index_by_object_id(model, object_id);
            if (obj_idx < 0)
                return parts::finish_later(done, InvokeResult::error(WAB_NOT_FOUND, "the object was removed while the file downloaded"));
            if (!parts::gizmos_closed())
                return parts::finish_later(done, InvokeResult::error(WAB_BUSY, "a gizmo was opened while the file downloaded"));
            Model loaded;
            try {
                loaded = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::LoadModel);
            } catch (const std::exception& ex) {
                return parts::finish_later(done, InvokeResult::error(WAB_UNSUPPORTED_TYPE, std::string("the file could not be read: ") + ex.what()));
            }
            loaded.add_default_instances();
            TriangleMesh mesh = loaded.mesh();
            if (mesh.empty())
                return parts::finish_later(done, InvokeResult::error(WAB_UNSUPPORTED_TYPE, "the file holds no mesh"));
            if (!size.empty() && !parts::fit_mesh_to_size(mesh, size))
                return parts::finish_later(done, InvokeResult::error(WAB_BAD_PARAMS, "sizeMm takes one value or three, all positive", {{"path", "/params/sizeMm"}}));
            Plater::SuppressSnapshots suppress(wxGetApp().plater());
            const std::string name = params.contains("name") ? params["name"].get<std::string>() :
                                                                fs::path(path).filename().string();
            ModelVolume* v = add_mesh_as_volume(model, obj_idx, std::move(mesh), type, name, params, false);
            parts::finish_later(done, finish_added_volume(model, obj_idx, v));
        },
        [done](int code, const std::string& message) { parts::finish_later(done, InvokeResult::error(code, message)); });
    return InvokeResult::started();
}

// ---------------------------------------------------------------- object.addShape

InvokeResult run_object_add_shape(InvokeContext& ctx)
{
    const json&       p     = ctx.params;
    const std::string shape = p.value("shape", "cube");
    TriangleMesh      mesh  = parts::make_primitive(shape, default_primitive_side());
    if (mesh.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "unknown shape", {{"path", "/params/shape"}});
    const std::vector<double> size = size_param(p);
    if (!size.empty() && !parts::fit_mesh_to_size(mesh, size))
        return InvokeResult::error(WAB_BAD_PARAMS, "sizeMm takes one value or three, all positive", {{"path", "/params/sizeMm"}});

    ObjectList* list = parts::object_list();
    if (list == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the object list is not available");
    // The menu offers this only with nothing selected; the new object is selected afterwards.
    select_targets(ResolvedTargets{});
    ctx.snapshot("Add Primitive");
    Plater::SuppressSnapshots suppress(ctx.plater());
    Model&            model  = ctx.model();
    const size_t      before = model.objects.size();
    const std::string name   = p.contains("name") ? p["name"].get<std::string>() : shape_label(shape);
    list->load_mesh_object(mesh, wxString::FromUTF8(name), true);
    if (model.objects.size() != before + 1)
        return InvokeResult::error(WAB_INTERNAL, "the shape was not added");
    const int obj_idx = int(model.objects.size()) - 1;
    if (auto pos = parts::vec3_param(p, "positionMm")) {
        ModelObject* o   = model.objects[obj_idx];
        Vec3d        off = o->instances.front()->get_offset();
        off.x()          = pos->x();
        off.y()          = pos->y();
        o->instances.front()->set_offset(off);
        o->invalidate_bounding_box();
        o->ensure_on_bed();
        list->notify_instance_updated(obj_idx);
        ctx.plater()->changed_object(obj_idx);
    }
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->update_title();
    return InvokeResult::ok({{"object", object_ref_json(model, obj_idx)}});
}

// ---------------------------------------------------------------- object.addHandyModel

InvokeResult run_object_add_handy_model(InvokeContext& ctx)
{
    const std::string wanted = ctx.params.value("name", "");
    const HandyModel* found  = nullptr;
    for (const HandyModel& m : handy_models())
        if (wanted == m.name)
            found = &m;
    if (found == nullptr)
        return InvokeResult::error(WAB_BAD_PARAMS, "unknown handy model", {{"path", "/params/name"}});

    std::vector<fs::path> files;
    for (const std::string& f : found->files)
        files.push_back(fs::path(Slic3r::resources_dir()) / "handy_models" / f);
    for (const fs::path& f : files)
        if (!fs::exists(f))
            return InvokeResult::error(WAB_NOT_FOUND, "a handy model file is missing from the resources: " + f.filename().string());

    Model&                model = ctx.model();
    std::vector<ObjectID> before;
    for (const ModelObject* o : model.objects)
        before.push_back(o->id());

    ctx.snapshot("Add Handy model");
    {
        Plater::SuppressSnapshots suppress(ctx.plater());
        ctx.plater()->load_files(files, LoadStrategy::LoadModel);
    }
    auto added = [before]() {
        const Model& model = wxGetApp().model();
        json         out   = json::array();
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (std::find(before.begin(), before.end(), model.objects[i]->id()) == before.end())
                out.push_back(object_ref_json(model, int(i)));
        return out;
    };
    const bool arrange = ctx.params.value("arrange", found->arrange);
    if (!arrange)
        return InvokeResult::ok({{"objects", added()}});

    ctx.plater()->set_prepare_state(Job::PREPARE_STATE_MENU);
    ctx.plater()->arrange();
    return ctx.after_plate_jobs([added](InvokeContext&, bool timed_out) {
        return InvokeResult::ok({{"objects", added()}, {"arranged", !timed_out}});
    });
}

// ---------------------------------------------------------------- part.setType

InvokeResult run_part_set_type(InvokeContext& ctx)
{
    Model&                model    = ctx.model();
    const ModelVolumeType new_type = parts::volume_type_from_name(ctx.params.value("type", ""));
    if (new_type == ModelVolumeType::INVALID)
        return InvokeResult::error(WAB_BAD_PARAMS, "unknown type", {{"path", "/params/type"}});
    if (ctx.targets.volumes.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "part.setType needs one or more parts", {{"path", "/target"}});

    std::vector<std::pair<int, ModelVolume*>> vols;
    for (const auto& ov : ctx.targets.volumes)
        vols.emplace_back(ov.first, model.objects[ov.first]->volumes[ov.second]);

    // Text and SVG volumes carry emboss data only parts, negative parts and modifiers understand
    // (the Change type menu hides the other entries for them, see ObjectList::set_volume_type).
    if (new_type == ModelVolumeType::SUPPORT_BLOCKER || new_type == ModelVolumeType::SUPPORT_ENFORCER ||
        new_type == ModelVolumeType::INFILL_MESH)
        for (const auto& ov : vols)
            if (ov.second->is_text() || ov.second->is_svg())
                return InvokeResult::error(WAB_BAD_PARAMS, "a text or SVG part can only be a part, a negative part or a modifier",
                                           {{"path", "/params/type"},
                                            {"volume", volume_ref_json(model, ov.first, parts::volume_index_in(*model.objects[ov.first], ov.second))}});

    // The last solid part of an object cannot change type.
    if (new_type != ModelVolumeType::MODEL_PART) {
        std::map<int, int> total, selected;
        for (const auto& ov : vols) {
            if (total.find(ov.first) == total.end()) {
                int n = 0;
                for (const ModelVolume* mv : model.objects[ov.first]->volumes)
                    n += mv->is_model_part() ? 1 : 0;
                total[ov.first] = n;
            }
            if (ov.second->is_model_part())
                ++selected[ov.first];
        }
        for (const auto& on : selected)
            if (total[on.first] > 0 && on.second == total[on.first])
                return InvokeResult::error(WAB_BAD_PARAMS, "the type of the last solid part of an object cannot be changed",
                                           {{"path", "/target"}, {"object", object_ref_json(model, on.first)}});
    }

    bool any_diff = false;
    for (const auto& ov : vols)
        any_diff |= ov.second->type() != new_type;
    if (!any_diff)
        return InvokeResult::ok({{"changed", 0}}).with_effects({});

    ctx.snapshot("Change part type");
    Plater::SuppressSnapshots suppress(ctx.plater());
    std::map<int, const ModelVolume*> touched; // object -> first changed volume (selected after)
    for (const auto& ov : vols) {
        ov.second->set_type(new_type);
        touched.emplace(ov.first, ov.second);
    }
    for (const auto& t : touched)
        parts::refresh_object_after_volume_change(t.first, t.second);
    json out = json::array();
    for (const auto& ov : vols)
        out.push_back(parts::volume_result_json(model, ov.first, parts::volume_index_in(*model.objects[ov.first], ov.second)));
    return InvokeResult::ok({{"changed", int(vols.size())}, {"volumes", out}});
}

// ---------------------------------------------------------------- part.setFilament

InvokeResult run_part_set_filament(InvokeContext& ctx)
{
    Model&    model    = ctx.model();
    const int filament = ctx.params.value("filament", -1);
    const int count    = int(ctx.plater()->get_extruder_colors_from_plater_config().size());
    if (filament < 0 || filament > count)
        return InvokeResult::error(WAB_BAD_PARAMS, "filament must be 0 (the object's) or 1.." + std::to_string(count),
                                   {{"path", "/params/filament"}, {"count", count}});
    if (ctx.targets.volumes.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "part.setFilament needs one or more parts", {{"path", "/target"}});
    for (const auto& ov : ctx.targets.volumes) {
        const ModelVolume* mv = model.objects[ov.first]->volumes[ov.second];
        if (!mv->is_model_part() && !mv->is_modifier())
            return InvokeResult::error(WAB_BAD_PARAMS, "only parts and modifiers take a filament",
                                       {{"path", "/target"}, {"volume", volume_ref_json(model, ov.first, ov.second)}});
    }
    ObjectList* list = parts::object_list();
    if (list == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the object list is not available");
    ctx.snapshot("Change Filaments");
    {
        // The object list's filament column, on the selection the runtime made from the target.
        Plater::SuppressSnapshots suppress(ctx.plater());
        list->set_extruder_for_selected_items(filament);
    }
    json out = json::array();
    for (const auto& ov : ctx.targets.volumes) {
        json        j = volume_ref_json(model, ov.first, ov.second);
        const auto& c = model.objects[ov.first]->volumes[ov.second]->config;
        j["filament"] = c.has("extruder") ? c.opt_int("extruder") : 0;
        out.push_back(j);
    }
    return InvokeResult::ok({{"volumes", out}});
}

// ---------------------------------------------------------------- part.setTransform

InvokeResult run_part_set_transform(InvokeContext& ctx)
{
    Model& model = ctx.model();
    if (ctx.targets.volumes.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "part.setTransform needs exactly one part", {{"path", "/target"}});
    const int    obj_idx = ctx.targets.volumes.front().first;
    const int    vol_idx = ctx.targets.volumes.front().second;
    ModelObject& object  = *model.objects[obj_idx];
    ModelVolume& volume  = *object.volumes[vol_idx];
    const json&  p       = ctx.params;

    const auto pos   = parts::vec3_param(p, "positionMm");
    const auto rot   = parts::vec3_param(p, "rotationDeg");
    const auto scale = parts::vec3_param(p, "scale");
    if (!pos && !rot && !scale)
        return InvokeResult::error(WAB_BAD_PARAMS, "give positionMm, rotationDeg or scale", {{"path", "/params"}, {"needs", "positionMm"}});
    if (scale && (scale->x() <= 0. || scale->y() <= 0. || scale->z() <= 0.))
        return InvokeResult::error(WAB_BAD_PARAMS, "scale factors must be positive", {{"path", "/params/scale"}});

    const bool        relative = p.value("relative", false);
    const std::string frame    = p.value("frame", "world");
    if (frame == "object" && !relative)
        return InvokeResult::error(WAB_BAD_PARAMS, "frame \"object\" applies to relative changes; add relative: true",
                                   {{"path", "/params/frame"}});
    const Transform3d inst = object.instances.front()->get_matrix();
    // Work in the plate frame: the part's current placement there, with the given components
    // replaced, or changed by them (relative) along the plate's or the part's own axes, about the
    // part's centre -- the Move / Rotate / Scale gizmos with a part selected.
    Geometry::Transformation world(inst * volume.get_matrix());
    if (relative) {
        const Vec3d pivot = volume.mesh().transformed_bounding_box(world.get_matrix()).center();
        world = Geometry::Transformation(parts::relative_transform(world.get_matrix(), pos, rot, scale, frame == "object", pivot));
    } else {
        if (pos)
            world.set_offset(*pos);
        if (rot)
            world.set_rotation(parts::deg2rad(*rot));
        if (scale)
            world.set_scaling_factor(*scale);
    }

    ctx.snapshot("Set part transformation");
    Plater::SuppressSnapshots suppress(ctx.plater());
    volume.set_transformation(Geometry::Transformation(inst.inverse() * world.get_matrix()));
    object.invalidate_bounding_box();
    if (object.instances.front()->auto_drop && volume.is_model_part())
        object.ensure_on_bed();
    if (ObjectList* list = parts::object_list())
        list->notify_instance_updated(obj_idx);
    ctx.plater()->changed_object(obj_idx);

    const Geometry::Transformation now(inst * volume.get_matrix());
    json r           = parts::volume_result_json(model, obj_idx, vol_idx);
    r["positionMm"]  = parts::vec3_json(now.get_offset());
    r["rotationDeg"] = parts::vec3_json(now.get_rotation() * (180. / M_PI));
    r["scale"]       = parts::vec3_json(now.get_scaling_factor());
    return InvokeResult::ok({{"volume", r}});
}

// ---------------------------------------------------------------- part.splitToParts

InvokeResult run_part_split(InvokeContext& ctx)
{
    Model& model   = ctx.model();
    int    obj_idx = -1, vol_idx = -1;
    if (ctx.targets.volumes.size() == 1) {
        obj_idx = ctx.targets.volumes.front().first;
        vol_idx = ctx.targets.volumes.front().second;
    } else if (ctx.targets.objects.size() == 1 && model.objects[ctx.targets.objects.front()]->volumes.size() == 1) {
        obj_idx = ctx.targets.objects.front();
        vol_idx = 0;
    } else
        return InvokeResult::error(WAB_BAD_PARAMS, "part.splitToParts needs one part, or an object with a single part", {{"path", "/target"}});

    ModelObject& object = *model.objects[obj_idx];
    ModelVolume* volume = object.volumes[vol_idx];
    if (!volume->is_splittable())
        return InvokeResult::error(WAB_BAD_PARAMS, "this part is one piece and cannot be split", {{"path", "/target"}});

    // ObjectList::split acts on the list selection: the object row for a single-part object (its
    // part has no row of its own), else the part row.
    ResolvedTargets sel;
    if (object.volumes.size() == 1)
        sel.objects = {obj_idx};
    else
        sel.volumes = {{obj_idx, vol_idx}};
    select_targets(sel);

    const size_t before = object.volumes.size();
    ctx.snapshot("Split to parts");
    {
        Plater::SuppressSnapshots suppress(ctx.plater());
        ctx.plater()->split_volume();
    }
    json out = json::array();
    for (size_t i = 0; i < object.volumes.size(); ++i)
        out.push_back(volume_ref_json(model, obj_idx, int(i)));
    return InvokeResult::ok({{"object", object_ref_json(model, obj_idx)}, {"partsBefore", before}, {"volumes", out}});
}

// ---------------------------------------------------------------- part.mergeToSinglePart

InvokeResult run_part_merge(InvokeContext& ctx)
{
    Model& model = ctx.model();
    if (ctx.targets.objects.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "part.mergeToSinglePart needs exactly one object", {{"path", "/target"}});
    const int    obj_idx = ctx.targets.objects.front();
    ModelObject* object  = model.objects[obj_idx];
    if (!(object->volumes.size() > 1 || (object->volumes.size() == 1 && object->volumes[0]->is_splittable())))
        return InvokeResult::error(WAB_BAD_PARAMS, "the object has a single part already", {{"path", "/target"}});
    ObjectList* list = parts::object_list();
    if (list == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the object list is not available");

    ctx.snapshot("Mesh boolean");
    {
        // "Mesh boolean" in the object menu: every part, negative parts subtracted, into one new
        // object with the old one's name and settings, which replaces it.
        Plater::SuppressSnapshots suppress(ctx.plater());
        list->boolean();
    }
    const int new_idx = int(model.objects.size()) - 1;
    json      r       = {{"object", object_ref_json(model, new_idx)}};
    if (new_idx >= 0)
        r["facets"] = model.objects[new_idx]->facets_count();
    return InvokeResult::ok(r);
}

// ---------------------------------------------------------------- part.replace

// Plater::priv::replace_volume_with_stl, with the file from a URL instead of a file dialog: the new
// mesh takes the old part's place, transformation, type, settings and (remapped) paint.
InvokeResult replace_volume(Model& model, int obj_idx, int vol_idx, const std::string& path)
{
    Model loaded;
    try {
        loaded = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel);
        for (ModelObject* mo : loaded.objects) {
            mo->center_around_origin();
            mo->ensure_on_bed();
        }
    } catch (const std::exception& ex) {
        return InvokeResult::error(WAB_UNSUPPORTED_TYPE, std::string("the file could not be read: ") + ex.what());
    }
    if (loaded.objects.empty() || loaded.objects.front()->volumes.empty())
        return InvokeResult::error(WAB_UNSUPPORTED_TYPE, "the file holds no mesh");
    if (loaded.objects.size() > 1 || loaded.objects.front()->volumes.size() > 1)
        return InvokeResult::error(WAB_UNSUPPORTED_TYPE, "the file holds more than one part; a part can only be replaced by one");

    ModelObject* old_object = model.objects[obj_idx];
    ModelVolume* old_volume = old_object->volumes[vol_idx];
    const bool   sinking    = old_object->min_z() < SINKING_Z_THRESHOLD;

    old_object->add_volume(*loaded.objects.front()->volumes.front());
    ModelVolume* nv = old_object->volumes.back();
    nv->set_new_unique_id();
    nv->config.apply(old_volume->config);
    nv->set_type(old_volume->type());
    nv->set_material_id(old_volume->material_id());
    nv->set_transformation(old_volume->get_transformation());
    nv->translate(nv->get_transformation().get_matrix_no_offset() * (nv->source.mesh_offset - old_volume->source.mesh_offset));
    if (old_volume->source.is_converted_from_inches)
        nv->convert_from_imperial_units();
    else if (old_volume->source.is_converted_from_meters)
        nv->convert_from_meters();
    if (wxGetApp().app_config->get_bool("keep_painting")) {
        auto saved = old_volume->save_painting();
        if (saved) {
            saved->mesh.transform(Geometry::translation_transform(nv->mesh().get_init_shift()));
            nv->restore_painting(saved);
        }
    } else {
        nv->supported_facets.assign(old_volume->supported_facets);
        nv->seam_facets.assign(old_volume->seam_facets);
        nv->mmu_segmentation_facets.assign(old_volume->mmu_segmentation_facets);
        nv->fuzzy_skin_facets.assign(old_volume->fuzzy_skin_facets);
    }
    std::swap(old_object->volumes[vol_idx], old_object->volumes.back());
    old_object->delete_volume(old_object->volumes.size() - 1);
    if (!sinking)
        old_object->ensure_on_bed();
    old_object->sort_volumes(true);
    if (old_object->volumes.size() == 1)
        old_object->name = old_object->volumes.front()->name;

    const int new_idx = parts::volume_index_in(*old_object, nv);
    if (ObjectList* list = parts::object_list())
        list->update_name_in_list(obj_idx, new_idx);
    Plater* plater = wxGetApp().plater();
    plater->update();
    for (size_t i = 0; i < model.objects.size(); ++i)
        plater->get_view3D_canvas3D()->update_instance_printable_state_for_object(i);
    parts::refresh_object_after_volume_change(obj_idx, nv);
    return InvokeResult::ok({{"volume", parts::volume_result_json(model, obj_idx, parts::volume_index_in(*old_object, nv))}});
}

InvokeResult run_part_replace(InvokeContext& ctx)
{
    Model& model = ctx.model();
    int    obj_idx = -1, vol_idx = -1;
    if (ctx.targets.volumes.size() == 1) {
        obj_idx = ctx.targets.volumes.front().first;
        vol_idx = ctx.targets.volumes.front().second;
    } else
        return InvokeResult::error(WAB_BAD_PARAMS, "part.replace needs exactly one part", {{"path", "/target"}});
    if (ctx.host == nullptr || !ctx.host->download)
        return InvokeResult::error(WAB_BAD_PARAMS, "downloads are unavailable in this tab", {{"path", "/params/url"}});

    const ObjectID object_id = model.objects[obj_idx]->id();
    const ObjectID volume_id = model.objects[obj_idx]->volumes[vol_idx]->id();
    ctx.snapshot("Replace with 3D file");
    std::shared_ptr<Deferred> done = ctx.defer();
    ctx.host->download(
        ctx.params.value("url", ""), MODEL_EXTS,
        [done, object_id, volume_id](const std::string& path) {
            Model&    model = wxGetApp().model();
            const int o     = parts::object_index_by_object_id(model, object_id);
            const int v     = o < 0 ? -1 : parts::volume_index_by_object_id(*model.objects[o], volume_id);
            if (v < 0)
                return parts::finish_later(done, InvokeResult::error(WAB_NOT_FOUND, "the part was removed while the file downloaded"));
            if (!parts::gizmos_closed())
                return parts::finish_later(done, InvokeResult::error(WAB_BUSY, "a gizmo was opened while the file downloaded"));
            Plater::SuppressSnapshots suppress(wxGetApp().plater());
            parts::finish_later(done, replace_volume(model, o, v, path));
        },
        [done](int code, const std::string& message) { parts::finish_later(done, InvokeResult::error(code, message)); });
    return InvokeResult::started();
}

} // namespace

void register_part_actions(Catalog& c)
{
    const json vec3_pos = parts::schema_vec3("Center of the part on the plate, in mm [x,y,z] (plate frame, Z up from the bed).");
    const json vec3_rot = parts::schema_vec3("Rotation in degrees about the plate X, Y and Z axes, applied in that order.");

    // part.add
    {
        ActionDescriptor d;
        d.id      = "part.add";
        d.label   = "Add part";
        d.summary = "Add a part, negative part, modifier, support blocker/enforcer or infill mesh to an object: a primitive "
                    "shape (cube, cylinder, sphere, cone, disc, torus) or a model file from a URL, with size, position and rotation.";
        d.risk    = Risk::Mutate;
        d.target  = {{"kind", "object"}, {"required", true}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"properties",
                     {{"type", type_schema("What the new volume is: a solid part, a negative part (subtracted), a settings "
                                           "modifier, a support blocker/enforcer, or an infill mesh.",
                                           "modifier")},
                      {"shape", {{"type", "string"},
                                 {"enum", {"cube", "cylinder", "sphere", "cone", "disc", "torus", "url"}},
                                 {"default", "cube"},
                                 {"description", "A primitive, or \"url\" to load a model file (STL/OBJ/3MF/AMF) from params.url."}}},
                      {"url", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048},
                               {"description", "http(s) URL of the model file when shape is \"url\"; the tab's download rules apply."}}},
                      {"sizeMm", size_schema()},
                      {"positionMm", vec3_pos},
                      {"relativeTo", {{"type", "string"}, {"enum", {"plate", "object"}}, {"default", "plate"},
                                      {"description", "positionMm is in plate coordinates, or an offset from the center of the object's bounding box."}}},
                      {"rotationDeg", vec3_rot},
                      {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}, {"description", "Name shown in the object list."}}}}}};
        d.effects = {"geometry", "structure"};
        d.ui      = {"GUI_Factories.cpp::menu::Cube", "GUI_Factories.cpp::menu::Cylinder", "GUI_Factories.cpp::menu::Sphere",
                     "GUI_Factories.cpp::menu::Cone", "GUI_Factories.cpp::menu::Disc",     "GUI_Factories.cpp::menu::Torus",
                     "GUI_Factories.cpp::menu::Load", "GUI_Factories.cpp::submenu::expr:_(item.first)@append_menu_items_add_volume"};
        d.examples = json::array(
            {{{"target", {{"object", {{"index", 0}}}}}, {"params", {{"type", "modifier"}, {"shape", "cube"}, {"sizeMm", {10}}}}},
             {{"target", {{"object", {{"index", 0}}}}},
              {"params", {{"type", "negative"}, {"shape", "cylinder"}, {"sizeMm", {5, 5, 30}}, {"positionMm", {0, 0, 0}}, {"relativeTo", "object"}}}}});
        c.add(d, run_part_add);
    }

    // part.setType
    {
        ActionDescriptor d;
        d.id      = "part.setType";
        d.label   = "Change type";
        d.summary = "Change what parts are: solid part, negative part, modifier, support blocker, support enforcer or infill mesh.";
        d.target  = {{"kind", "volumes"}, {"required", true}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"required", {"type"}},
                    {"properties", {{"type", type_schema("The new type.")}}}};
        d.effects = {"structure"};
        d.ui      = {"GUI_Factories.cpp::menu::expr:info.label@append_menu_item_change_type"};
        d.examples = json::parse(R"([{"setup": [{"id": "part.add", "target": {"object": {"index": 0}}, "params": {"type": "modifier", "sizeMm": [10]}}],
           "target": {"volume": {"object": {"index": 0}, "index": 1}}, "params": {"type": "blocker"}}])");
        c.add(d, run_part_set_type);
    }

    // part.setFilament
    {
        ActionDescriptor d;
        d.id      = "part.setFilament";
        d.label   = "Change filament";
        d.summary = "Assign parts or modifiers to a filament slot (1-based); 0 makes a part use its object's filament.";
        d.target  = {{"kind", "volumes"}, {"required", true}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"required", {"filament"}},
                    {"properties", {{"filament", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64}, {"description", "Filament slot, 1-based; 0 = the object's."}}}}}};
        d.effects = {"structure"};
        d.ui      = {"GUI_ObjectList.cpp::column::colFilament", "GUI_ObjectTable.cpp::tablecol::col_filaments"};
        d.examples = json::array({{{"target", {{"volume", {{"object", {{"index", 0}}}, {"index", 0}}}}}, {"params", {{"filament", 1}}}}});
        c.add(d, run_part_set_filament);
    }

    // part.setTransform
    {
        ActionDescriptor d;
        d.id      = "part.setTransform";
        d.label   = "Move / rotate / scale part";
        d.summary = "Place one part of an object: its position on the plate, rotation about the plate axes and scale "
                    "(per axis); anything left out keeps its current value. Or, relative, move / turn / scale it by amounts "
                    "along the plate's axes or its own (frame).";
        d.target  = {{"kind", "volume"}, {"required", true}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"properties",
                     {{"positionMm", parts::schema_vec3("Where the part's origin (its center when added) goes on the plate, mm "
                                                        "(relative: how far to move it).")},
                      {"rotationDeg", vec3_rot},
                      {"scale", parts::schema_vec3("Scale factors along the part's X, Y, Z (1 = original size; relative: multiplied).")},
                      {"relative", {{"type", "boolean"}, {"default", false},
                                    {"description", "Change by these amounts, about the part's centre, instead of setting them."}}},
                      {"frame", {{"type", "string"}, {"enum", {"world", "object"}}, {"default", "world"},
                                 {"description", "relative: along the plate's axes (world) or the part's own (object), as the gizmos' "
                                                 "coordinate combo."}}}}}};
        d.effects = {"geometry"};
        d.examples = json::array({{{"target", {{"volume", {{"object", {{"index", 0}}}, {"index", 0}}}}},
                                   {"params", {{"rotationDeg", {0, 0, 45}}, {"scale", {1, 1, 2}}}}},
                                  {{"target", {{"volume", {{"object", {{"index", 0}}}, {"index", 0}}}}},
                                   {"params", {{"positionMm", {0, 0, 5}}, {"relative", true}, {"frame", "object"}}}}});
        c.add(d, run_part_set_transform);
    }

    // part.splitToParts
    {
        ActionDescriptor d;
        d.id      = "part.splitToParts";
        d.label   = "Split to parts";
        d.summary = "Split a part made of several disconnected pieces into one part per piece, inside the same object.";
        d.target  = {{"kind", "volume"}, {"required", true}};
        d.effects = {"structure", "geometry"};
        d.ui      = {"GUI_Factories.cpp::menu::To Parts", "GLCanvas3D.cpp::toolbar::splitvolumes"};
        d.examples = json::parse(R"([{"setup": [{"id": "part.add", "target": {"object": {"index": 0}},
                      "params": {"type": "part", "sizeMm": [5], "positionMm": [30, 0, 0], "relativeTo": "object"}},
                     {"id": "part.mergeToSinglePart", "target": {"object": {"index": 0}}}],
           "target": {"volume": {"object": {"index": 0}, "index": 0}}, "params": {}}])");
        c.add(d, run_part_split, [](const InvokeContext& ctx) -> std::string {
            const Model& model = ctx.model();
            for (const auto& ov : ctx.targets.volumes)
                if (!model.objects[ov.first]->volumes[ov.second]->is_splittable())
                    return "the part is one piece";
            return {};
        });
    }

    // part.mergeToSinglePart
    {
        ActionDescriptor d;
        d.id      = "part.mergeToSinglePart";
        d.label   = "Mesh boolean";
        d.summary = "Merge every part of an object into one solid part (negative parts are subtracted, modifiers dropped); "
                    "the object is replaced by the merged one.";
        d.target  = {{"kind", "object"}, {"required", true}};
        d.effects = {"structure", "geometry"};
        d.ui      = {"GUI_Factories.cpp::menu::Mesh boolean"};
        d.examples = json::parse(R"([{"setup": [{"id": "part.add", "target": {"object": {"index": 0}},
                      "params": {"type": "part", "shape": "sphere", "sizeMm": [16], "positionMm": [10, 10, 10], "relativeTo": "object"}}],
           "target": {"object": {"index": 0}}, "params": {}}])");
        c.add(d, run_part_merge, [](const InvokeContext& ctx) -> std::string {
            const Model& model = ctx.model();
            for (int o : ctx.targets.objects) {
                const ModelObject* mo = model.objects[o];
                if (!(mo->volumes.size() > 1 || (mo->volumes.size() == 1 && mo->volumes[0]->is_splittable())))
                    return "the object has a single part already";
            }
            return {};
        });
    }

    // part.replace
    {
        ActionDescriptor d;
        d.id      = "part.replace";
        d.label   = "Replace 3D file";
        d.summary = "Replace one part's mesh with a model file from a URL (STL/OBJ/3MF/AMF holding one part); the part keeps "
                    "its place, type, settings and paint.";
        d.target  = {{"kind", "volume"}, {"required", true}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"required", {"url"}},
                    {"properties", {{"url", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048},
                                             {"description", "http(s) URL of the model file; the tab's download rules apply."}}}}}};
        d.effects      = {"geometry"};
        d.long_running = true;
        d.ui           = {"GUI_Factories.cpp::menu::Replace 3D file"};
        d.examples     = json::array({{{"target", {{"volume", {{"object", {{"index", 0}}}, {"index", 0}}}}},
                                       {"params", {{"url", "http://127.0.0.1:8799/files/cube.stl"}}}}});
        c.add(d, run_part_replace);
    }

    // object.addShape
    {
        ActionDescriptor d;
        d.id      = "object.addShape";
        d.label   = "Add Primitive";
        d.summary = "Add a new object that is a primitive shape (cube, cylinder, sphere, cone, disc, torus) to the plate.";
        d.target  = {{"kind", "none"}, {"required", false}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"properties",
                     {{"shape", {{"type", "string"}, {"enum", parts::shape_names()}, {"default", "cube"}}},
                      {"sizeMm", size_schema()},
                      {"positionMm", parts::schema_vec3("Where on the plate, mm; only x and y are used, the object sits on the bed.")},
                      {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}}}};
        d.effects = {"structure", "plates"};
        d.ui      = {"GUI_Factories.cpp::submenu::Add Primitive"};
        d.examples = json::array({{{"target", nullptr}, {"params", {{"shape", "cylinder"}, {"sizeMm", {20, 20, 40}}}}}});
        c.add(d, run_object_add_shape);
    }

    // object.addHandyModel
    {
        ActionDescriptor d;
        d.id      = "object.addHandyModel";
        d.label   = "Add Handy models";
        d.summary = "Add one of the bundled test models (3DBenchy, calibration cubes, Stanford bunny, tolerance and stringing tests) to the plate.";
        d.target  = {{"kind", "none"}, {"required", false}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"required", {"name"}},
                    {"properties",
                     {{"name", {{"type", "string"}, {"enum", handy_model_names()}}},
                      {"arrange", {{"type", "boolean"}, {"description", "Arrange the plate afterwards (default: what the menu does for that model)."}}}}}};
        d.effects = {"structure", "plates"};
        d.ui      = {"GUI_Factories.cpp::submenu::Add Handy models", "GUI_Factories.cpp::menu::expr:_(model.label)@append_submenu_add_handy_model"};
        d.examples = json::array({{{"target", nullptr}, {"params", {{"name", "3dBenchy"}}}}});
        c.add(d, run_object_add_handy_model);
    }
}

}}} // namespace Slic3r::GUI::Actions
