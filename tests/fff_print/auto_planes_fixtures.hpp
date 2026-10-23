#pragma once

// UNLAYERED_MOD -- small parametric meshes for the automatic-plane and multiplanar-audit tests.
// Generated in code so no binary fixtures have to be kept in sync with the tests.

#include "libslic3r/TriangleMesh.hpp"

#include <cmath>
#include <functional>
#include <vector>

namespace AutoPlanesFixtures {

using namespace Slic3r;

// A block extruded along Y (depth `depth`) whose cross-section in X spans [0, length] and, at each x,
// runs from bottom(x) up to top(x). Both are sampled at `n` columns. bottom() > 0 makes an overhang.
inline indexed_triangle_set heightfield_block(double length, double depth, int n,
                                              const std::function<double(double)> &top,
                                              const std::function<double(double)> &bottom = [](double) { return 0.; })
{
    indexed_triangle_set its;
    // Vertex layout per column i: 0 = front-bottom, 1 = front-top, 2 = back-top, 3 = back-bottom.
    for (int i = 0; i <= n; ++ i) {
        const double x = length * double(i) / double(n);
        const float  zb = float(bottom(x)), zt = float(top(x));
        its.vertices.emplace_back(float(x), 0.f,          zb);
        its.vertices.emplace_back(float(x), 0.f,          zt);
        its.vertices.emplace_back(float(x), float(depth), zt);
        its.vertices.emplace_back(float(x), float(depth), zb);
    }
    auto V = [](int col, int k) { return col * 4 + k; };
    for (int i = 0; i < n; ++ i) {
        const int a = i, b = i + 1;
        // front (y = 0), outward -Y
        its.indices.emplace_back(V(a, 0), V(b, 0), V(b, 1));
        its.indices.emplace_back(V(a, 0), V(b, 1), V(a, 1));
        // top, outward +Z
        its.indices.emplace_back(V(a, 1), V(b, 1), V(b, 2));
        its.indices.emplace_back(V(a, 1), V(b, 2), V(a, 2));
        // back (y = depth), outward +Y
        its.indices.emplace_back(V(a, 2), V(b, 2), V(b, 3));
        its.indices.emplace_back(V(a, 2), V(b, 3), V(a, 3));
        // bottom, outward -Z
        its.indices.emplace_back(V(a, 3), V(b, 3), V(b, 0));
        its.indices.emplace_back(V(a, 3), V(b, 0), V(a, 0));
    }
    // end caps: x = 0 outward -X, x = length outward +X
    its.indices.emplace_back(V(0, 0), V(0, 1), V(0, 2));
    its.indices.emplace_back(V(0, 0), V(0, 2), V(0, 3));
    its.indices.emplace_back(V(n, 0), V(n, 2), V(n, 1));
    its.indices.emplace_back(V(n, 0), V(n, 3), V(n, 2));
    return its;
}

constexpr double kPi = 3.14159265358979323846;

// Flat box: planar printing should stay preferable.
inline indexed_triangle_set flat_box() { return its_make_cube(30., 20., 6.); }

// Shallow ramp: base 3 mm, top rising `slope_deg` along +X over 40 mm.
inline indexed_triangle_set shallow_ramp(double slope_deg = 4.)
{
    const double t = std::tan(slope_deg * kPi / 180.);
    return heightfield_block(40., 20., 8, [t](double x) { return 3. + t * x; });
}

// Curved-top block: a shallow circular arc across X, like the Centauri curved finish block.
inline indexed_triangle_set curved_top_block()
{
    return heightfield_block(30., 24., 30, [](double x) {
        const double R = 60., c = 15.;
        return 2. + std::sqrt(R * R - (x - c) * (x - c)) - std::sqrt(R * R - c * c);
    });
}

// Overhang coupon: solid foot for x < 15, then the underside rises at `overhang_deg` from horizontal
// out to x = 45, under a flat top. The rising underside needs support below the threshold angle.
// The coupon ends exactly where the underside reaches top - 2, so the whole underside is one inclined
// face -- no flat ceiling that no tilt could cure.
inline indexed_triangle_set overhang_coupon(double overhang_deg = 26., double top = 10.)
{
    const double t   = std::tan(overhang_deg * kPi / 180.);
    const double len = 15. + (top - 2.) / t;
    return heightfield_block(len, 20., int(std::ceil(len)), [top](double) { return top; },
                             [t, top](double x) { return x < 15. ? 0. : std::min(top - 2., (x - 15.) * t); });
}

// Bracket with two arms projecting from a central post, undersides at `arm_deg` from horizontal,
// leaning in opposite X directions.
inline indexed_triangle_set arm_bracket(double arm_deg = 27.)
{
    const double t = std::tan(arm_deg * kPi / 180.);
    return heightfield_block(60., 16., 60,
        [](double) { return 14.; },
        [t](double x) {
            if (x >= 25. && x <= 35.) return 0.;                 // post
            const double d = x < 25. ? 25. - x : x - 35.;        // distance out along the arm
            return std::min(11., d * t);
        });
}

// A solid rod of radius r swept along a quarter arc of radius R in the XZ plane: it stands up from
// the bed and bends over to horizontal -- the "bent pipe".
inline indexed_triangle_set bent_rod(double R = 25., double r = 5., int arc_steps = 24, int ring = 24)
{
    indexed_triangle_set its;
    // Centreline: c(s) = (R - R cos s, 0, R sin s) for s in [0, pi/2]; starts vertical at the origin.
    for (int i = 0; i <= arc_steps; ++ i) {
        const double s = 0.5 * kPi * double(i) / double(arc_steps);
        const Vec3d  c(R - R * std::cos(s), 0., R * std::sin(s));
        const Vec3d  tng(std::sin(s), 0., std::cos(s));      // tangent
        const Vec3d  u(std::cos(s), 0., -std::sin(s));       // in-plane normal (perpendicular to tangent)
        const Vec3d  v(0., 1., 0.);
        for (int k = 0; k < ring; ++ k) {
            const double a = 2. * kPi * double(k) / double(ring);
            const Vec3d  p = c + r * (std::cos(a) * u + std::sin(a) * v);
            its.vertices.emplace_back(p.cast<float>());
        }
        (void)tng;
    }
    // Lift so the lowest ring sits on z = 0.
    float zmin = 1e9f;
    for (const auto &v : its.vertices) zmin = std::min(zmin, v.z());
    for (auto &v : its.vertices) v.z() -= zmin;
    for (int i = 0; i < arc_steps; ++ i)
        for (int k = 0; k < ring; ++ k) {
            const int a = i * ring + k, b = i * ring + (k + 1) % ring;
            const int c = a + ring,     d = b + ring;
            its.indices.emplace_back(a, c, b);
            its.indices.emplace_back(b, c, d);
        }
    // Caps (fans around a centre vertex).
    auto cap = [&its, ring](int first, bool flip) {
        Vec3f c = Vec3f::Zero();
        for (int k = 0; k < ring; ++ k) c += its.vertices[size_t(first + k)];
        c /= float(ring);
        const int ci = int(its.vertices.size());
        its.vertices.push_back(c);
        for (int k = 0; k < ring; ++ k) {
            const int a = first + k, b = first + (k + 1) % ring;
            if (flip) its.indices.emplace_back(ci, b, a); else its.indices.emplace_back(ci, a, b);
        }
    };
    cap(0, false);
    cap(arc_steps * ring, true);
    // Fix orientation: if the volume came out negative, flip every face.
    if (its_volume(its) < 0.f)
        for (auto &f : its.indices) std::swap(f[1], f[2]);
    return its;
}

} // namespace AutoPlanesFixtures
