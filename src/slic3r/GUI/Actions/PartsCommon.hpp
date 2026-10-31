#ifndef slic3r_GUI_Actions_PartsCommon_hpp_
#define slic3r_GUI_Actions_PartsCommon_hpp_

// Helpers shared by the parts / mesh / text action files (work package S3): volume types by name,
// vectors out of params, primitive meshes, world <-> object frames, the object-list refresh after a
// volume was added or changed, and placing a new volume on a side of an object.
//
// Pure: no GUI dependency, unit tested in tests/slic3rutils/test_actions_parts.cpp. The GUI-side
// helpers are in PartsCommonGui.hpp.

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace GUI { namespace Actions { namespace parts {

// ---------------------------------------------------------------- volume types

// Wire names of the volume types an action may create or set ("part" is a solid model part).
inline const std::vector<std::string>& volume_type_names()
{
    static const std::vector<std::string> names = {"part", "negative", "modifier", "blocker", "enforcer", "infillMesh"};
    return names;
}

inline ModelVolumeType volume_type_from_name(const std::string& name)
{
    if (name == "part")       return ModelVolumeType::MODEL_PART;
    if (name == "negative")   return ModelVolumeType::NEGATIVE_VOLUME;
    if (name == "modifier")   return ModelVolumeType::PARAMETER_MODIFIER;
    if (name == "blocker")    return ModelVolumeType::SUPPORT_BLOCKER;
    if (name == "enforcer")   return ModelVolumeType::SUPPORT_ENFORCER;
    if (name == "infillMesh") return ModelVolumeType::INFILL_MESH;
    return ModelVolumeType::INVALID;
}

inline std::string volume_type_name(ModelVolumeType t)
{
    switch (t) {
    case ModelVolumeType::MODEL_PART:         return "part";
    case ModelVolumeType::NEGATIVE_VOLUME:    return "negative";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER:    return "blocker";
    case ModelVolumeType::SUPPORT_ENFORCER:   return "enforcer";
    case ModelVolumeType::INFILL_MESH:        return "infillMesh";
    default:                                  return "invalid";
    }
}

// ---------------------------------------------------------------- params

// A 3-vector param ([x,y,z]); nullopt when absent or not a 3-array of numbers.
inline std::optional<Vec3d> vec3_param(const nlohmann::json& params, const char* key)
{
    auto it = params.find(key);
    if (it == params.end() || !it->is_array() || it->size() != 3)
        return std::nullopt;
    for (const auto& v : *it)
        if (!v.is_number())
            return std::nullopt;
    return Vec3d((*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>());
}

inline nlohmann::json vec3_json(const Vec3d& v)
{
    auto r = [](double x) { return std::round(x * 1000.) / 1000.; };
    return nlohmann::json::array({r(v.x()), r(v.y()), r(v.z())});
}

inline Vec3d deg2rad(const Vec3d& d) { return d * (M_PI / 180.); }

// Schema fragments.
inline nlohmann::json schema_vec3(const std::string& description)
{
    return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", description}};
}

// ---------------------------------------------------------------- primitive meshes

inline const std::vector<std::string>& shape_names()
{
    static const std::vector<std::string> names = {"cube", "cylinder", "sphere", "cone", "disc", "torus"};
    return names;
}

// The primitive the "Add part / modifier / ..." menus create, for an edge `side` (mm), the same
// proportions as ObjectList's create_mesh(). Empty mesh for an unknown name.
inline TriangleMesh make_primitive(const std::string& shape, double side)
{
    if (shape == "cube")     return TriangleMesh(its_make_cube(side, side, side));
    if (shape == "cylinder") return TriangleMesh(its_make_cylinder(0.5 * side, side));
    if (shape == "sphere")   return TriangleMesh(its_make_sphere(0.5 * side, PI / 90));
    if (shape == "cone")     return TriangleMesh(its_make_cone(0.5 * side, side));
    if (shape == "disc")     return TriangleMesh(its_make_cylinder(0.5 * side, 0.2f));
    if (shape == "torus")    return TriangleMesh(its_make_torus(0.5 * side, 0.125 * side, PI / 60, PI / 60));
    return {};
}

// Scales `mesh` so its bounding box matches `size_mm`: one value = the largest dimension (uniform),
// three values = the box exactly (non-uniform). Returns false for a degenerate request.
inline bool fit_mesh_to_size(TriangleMesh& mesh, const std::vector<double>& size_mm)
{
    if (mesh.empty() || size_mm.empty())
        return false;
    const Vec3d bb = mesh.bounding_box().size();
    if (size_mm.size() == 1) {
        const double mx = std::max({bb.x(), bb.y(), bb.z()});
        if (mx <= EPSILON || size_mm[0] <= 0.)
            return false;
        mesh.scale(float(size_mm[0] / mx));
        return true;
    }
    if (size_mm.size() != 3)
        return false;
    Vec3f f;
    for (int i = 0; i < 3; ++i) {
        if (size_mm[i] <= 0.)
            return false;
        f[i] = bb[i] > EPSILON ? float(size_mm[i] / bb[i]) : 1.f;
    }
    mesh.scale(f);
    return true;
}

// ---------------------------------------------------------------- frames

// Volume matrix (object frame) that puts a volume's origin at `world_center`, rotated by
// `world_rotation_deg` (XYZ, plate axes) and scaled by `scale`, for an instance with matrix
// `instance_matrix`.
inline Transform3d volume_matrix_for_world(const Transform3d& instance_matrix, const Vec3d& world_center,
                                           const Vec3d& world_rotation_deg, const Vec3d& scale = Vec3d::Ones())
{
    const Transform3d world = Geometry::translation_transform(world_center) * Geometry::rotation_transform(deg2rad(world_rotation_deg)) *
                              Geometry::scale_transform(scale);
    return instance_matrix.inverse() * world;
}

// The outward normal of a named side of an object's bounding box (plate axes; front = -Y, the
// side facing the user).
inline std::optional<Vec3d> side_normal(const std::string& side)
{
    if (side == "front")  return Vec3d(0., -1., 0.);
    if (side == "back")   return Vec3d(0., 1., 0.);
    if (side == "left")   return Vec3d(-1., 0., 0.);
    if (side == "right")  return Vec3d(1., 0., 0.);
    if (side == "top")    return Vec3d(0., 0., 1.);
    if (side == "bottom") return Vec3d(0., 0., -1.);
    return std::nullopt;
}

// A rotation taking +Z onto `normal` (cut planes: the "upper" part is on the normal's side).
inline Transform3d rotation_z_to(const Vec3d& normal)
{
    Transform3d r = Transform3d::Identity();
    const Vec3d  n = normal.normalized();
    r.linear()     = Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), n).toRotationMatrix();
    return r;
}

// The Cut tool's plane rotation: +Z onto `normal`, then `spin_deg` about it. The spin sets the
// plane's in-plane X axis, which orients a dovetail groove and square / hexagon / triangle
// connectors (the tool's rotation grabbers); a normal alone leaves it at rotation_z_to's choice.
inline Transform3d cut_rotation(const Vec3d& normal, double spin_deg)
{
    Transform3d r = rotation_z_to(normal);
    r.rotate(Eigen::AngleAxisd(spin_deg * M_PI / 180., Vec3d::UnitZ()));
    return r;
}

// A relative move / turn / scale of a placement `m` (plate frame), as the Move / Rotate / Scale
// gizmos do with the coordinate combo on World or Object: with `own_axes` false the offset, the
// rotation axes and the scale axes are the plate's; with it true they are the placement's own
// (its rotation, scale removed). Rotation (about X, then Y, then Z, degrees) and scale act about
// `pivot` (plate frame); the offset is applied last.
inline Transform3d relative_transform(const Transform3d& m, const std::optional<Vec3d>& offset, const std::optional<Vec3d>& rot_deg,
                                      const std::optional<Vec3d>& scale, bool own_axes, const Vec3d& pivot)
{
    const Matrix3d axes = own_axes ? Geometry::Transformation(m).get_rotation_matrix().linear() : Matrix3d(Matrix3d::Identity());
    auto about_pivot = [&pivot](const Matrix3d& l) {
        Transform3d t = Transform3d::Identity();
        t.translate(pivot);
        t.linear() = l;
        t.translate(-pivot);
        return t;
    };
    Transform3d out = m;
    if (rot_deg) {
        const Matrix3d r = (Eigen::AngleAxisd(rot_deg->z() * M_PI / 180., Vec3d::UnitZ()) *
                            Eigen::AngleAxisd(rot_deg->y() * M_PI / 180., Vec3d::UnitY()) *
                            Eigen::AngleAxisd(rot_deg->x() * M_PI / 180., Vec3d::UnitX()))
                               .toRotationMatrix();
        out = about_pivot(axes * r * axes.transpose()) * out;
    }
    if (scale)
        out = about_pivot(axes * scale->asDiagonal() * axes.transpose()) * out;
    if (offset)
        out.translation() += axes * *offset;
    return out;
}

// What one piece of a cut does afterwards (the Cut tool's "After cut" rows).
struct CutPieceFlags
{
    bool keep         = true;
    bool place_on_cut = false;
    bool flip         = false;
};

// The tool's per-piece rules: Place on cut and Flip exclude each other (ticking one unticks the
// other), both need the piece kept, and Cut to parts disables both. "" when fine, else the reason.
inline std::string check_cut_piece(const CutPieceFlags& f, const char* piece, bool keep_as_parts)
{
    if (f.place_on_cut && f.flip)
        return std::string("the ") + piece + " piece cannot both be placed on its cut face and flipped; pick one";
    if ((f.place_on_cut || f.flip) && !f.keep)
        return std::string("the ") + piece + " piece is not kept, so it cannot be placed on its cut or flipped";
    if ((f.place_on_cut || f.flip) && keep_as_parts)
        return "with keepAsParts the pieces stay in place; placeOnCut and flip do not apply";
    return {};
}

// The Dovetail mode's groove defaults for an object of `size` (GLGizmoCut3D::update_bb, with the
// grabber size of the unfixed-grabber build: the fixed one depends on the camera zoom).
inline void default_groove(const Vec3d& size, double& depth, double& width)
{
    depth = std::max(1., 0.5 * (size.x() + size.y() + size.z()) / 30.);
    width = 4. * depth;
}

// GLGizmoCut3D::has_valid_groove's shape test: the flaps must fit inside the groove width.
inline bool groove_flaps_fit(double depth, double width, double flaps_angle_rad)
{
    return !(-2. * depth / std::tan(flaps_angle_rad) > width);
}

// Where a new volume of size `size` goes when no position is given: what the menus do -- the
// left-front corner of the instance's bounding box, on the bed (ObjectList::load_generic_subobject).
inline Vec3d default_part_center(const BoundingBoxf3& instance_bb, const Vec3d& size)
{
    return Vec3d(instance_bb.max.x(), instance_bb.min.y(), instance_bb.min.z()) + 0.5 * size;
}

}}}} // namespace Slic3r::GUI::Actions::parts

#endif // slic3r_GUI_Actions_PartsCommon_hpp_
