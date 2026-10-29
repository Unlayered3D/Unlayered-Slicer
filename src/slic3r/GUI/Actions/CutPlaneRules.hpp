#ifndef slic3r_GUI_Actions_CutPlaneRules_hpp_
#define slic3r_GUI_Actions_CutPlaneRules_hpp_

// The pure rules behind authoring multiplanar cut planes and build directions over the bridge
// (slicer.setCutPlanes, object.setBuildDirection / part.setBuildDirection, part.setSettings for
// layer_rotation_b/c): the frames, the tilt clamp and how a replace keeps the build direction of a
// plane it rewrites unchanged. They copy what GLGizmoCutPlanes does so automation stores what the
// gizmo would. No wx, no GUI_App: unit-tested in tests/slic3rutils/test_actions_parts.cpp. The
// tilt LIMIT itself (virtual_max_tilt_deg) reads the edited printer preset and is passed in.

#include <cmath>
#include <optional>
#include <vector>

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace Actions { namespace CutPlaneRules {

// Object-local <-> world for cut-plane data, as GLGizmoCutPlanes: points by the instance matrix,
// stored normals by the transpose of its linear part (add_plane / commit), shown normals by the
// inverse-transpose (sync_gizmo_from_active_plane, render_build_direction_editor).
inline Vec3d local_normal(const Transform3d& instance, const Vec3d& n_world) { return (instance.linear().transpose() * n_world).normalized(); }
inline Vec3d world_normal(const Transform3d& instance, const Vec3d& n_local) { return (instance.linear().inverse().transpose() * n_local).normalized(); }

// GLGizmoCutPlanes::tilt_azimuth: the direction a normal leans in, none at the pole.
inline std::optional<double> tilt_azimuth(const Vec3d& n_world)
{
    if (std::hypot(n_world.x(), n_world.y()) < 1e-6)
        return std::nullopt;
    return std::atan2(n_world.y(), n_world.x());
}

// B (tilt from vertical, -180..0) of a world direction, as a positive angle.
inline double tilt_deg(const Vec3d& n_world)
{
    double b = 0., c = 0.;
    Geometry::bc_from_build_normal(n_world.normalized(), b, c);
    return std::abs(b);
}

// GLGizmoCutPlanes::clamp_build_normal_to_tilt: cap |B| at `limit_deg` keeping the azimuth C.
// limit_deg <= 0 means "no limit" (not a virtual 5-axis printer).
inline Vec3d clamp_to_tilt(const Vec3d& n_world, double limit_deg)
{
    if (limit_deg <= 0.)
        return n_world;
    double b = 0., c = 0.;
    Geometry::bc_from_build_normal(n_world.normalized(), b, c);
    if (std::abs(b) <= limit_deg)
        return n_world;
    return Geometry::build_normal_from_bc(b < 0. ? -limit_deg : limit_deg, c).normalized();
}

// The build direction for machine angles B/C (degrees), as the gizmo's B/C fields commit it: at the
// pole (B = 0) C is meaningless and taken as 0.
inline Vec3d normal_from_bc(double b_deg, double c_deg)
{
    return Geometry::build_normal_from_bc(b_deg, std::abs(b_deg) < 1e-6 ? 0. : c_deg).normalized();
}

// Is `b` the same plane as `a` (both object-local): parallel normals within 0.05 degrees and b's
// point on a's plane within 0.01 mm. Tolerant of the rounding a read-modify-write goes through.
inline bool same_plane(const CutPlane& a, const CutPlane& b)
{
    const Vec3d na = a.normal.normalized(), nb = b.normal.normalized();
    return na.dot(nb) > std::cos(0.05 * M_PI / 180.) && std::abs((b.point - a.point).dot(na)) < 0.01;
}

// A replace rewrites the whole list. For each new plane that does not set its build direction
// itself (`explicit_build[i]` false) the override of an unchanged old plane (same_plane) is carried
// over, so reading the planes and writing them back never drops what the person set in the gizmo.
// Returns how many were carried; `dropped` counts old overrides that found no unchanged plane.
inline int carry_build_normals(const std::vector<CutPlane>& old, std::vector<CutPlane>& fresh, const std::vector<bool>& explicit_build,
                               int* dropped = nullptr)
{
    int               kept = 0;
    std::vector<bool> used(old.size(), false);
    for (size_t i = 0; i < fresh.size(); ++i) {
        if (i < explicit_build.size() && explicit_build[i])
            continue;
        for (size_t k = 0; k < old.size(); ++k)
            if (!used[k] && old[k].has_build_normal && same_plane(old[k], fresh[i])) {
                fresh[i].build_normal     = old[k].build_normal;
                fresh[i].has_build_normal = true;
                used[k]                   = true;
                ++kept;
                break;
            }
    }
    if (dropped != nullptr) {
        *dropped = 0;
        for (size_t k = 0; k < old.size(); ++k)
            if (old[k].has_build_normal && !used[k]) {
                // Not dropped when a new plane re-states the same plane with its own direction.
                bool restated = false;
                for (size_t i = 0; i < fresh.size() && !restated; ++i)
                    restated = i < explicit_build.size() && explicit_build[i] && same_plane(old[k], fresh[i]);
                if (!restated)
                    ++*dropped;
            }
    }
    return kept;
}

}}}} // namespace Slic3r::GUI::Actions::CutPlaneRules

#endif // slic3r_GUI_Actions_CutPlaneRules_hpp_
