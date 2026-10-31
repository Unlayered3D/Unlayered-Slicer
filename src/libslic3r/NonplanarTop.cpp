// UNLAYERED_MOD: nonplanar layers -- curved top shells. See NonplanarTop.hpp for the overview.
//
// Design notes (why it is done this way):
//  * Clipping happens on finished extrusions (after infill and ironing, in the Z contouring step),
//    not on surfaces in prepare_infill. The material to remove is bounded by a curved surface,
//    S(x,y) - T, which differs on every layer, and a layer's surfaces are shared by perimeter, infill
//    and ironing generation; cutting extrusions by a per-layer footprint is local, cannot change
//    anything outside a patch (option off or no patch = untouched entities), and is the approach
//    already proven on prints.
//  * Walls: inside a patch footprint every extrusion in the top T is removed, walls included, and
//    each curved layer carries its own edge loop half a bead inside the footprint. Where the patch
//    reaches the object's outer wall those loops ARE the top of the wall, following the surface, so
//    nothing sticks up through the skin and there is no gap; where the patch ends inside the top
//    (steeper area around it) the loop is the seam against the planar stairs.
//  * The planar layers keep material at (x,y) while their top is at most S - T + h/2, so the first
//    curved layer sits on a gap between h/2 and 3h/2; its flow is metered to that gap.
//  * Shell paths go into the HIGHEST layer that intersects the patch, so nothing printed later is
//    under them; their Z offsets are relative to that layer's print_z.

#include "NonplanarTop.hpp"
#include "NonplanarTopSlice.hpp"

#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Fill/FillBase.hpp"
#include "Flow.hpp"
#include "Geometry.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "SLA/IndexedMesh.hpp"
#include "Surface.hpp"
#include "TriangleMeshSlicer.hpp"
#include "libslic3r.h"

#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Slic3r {

namespace {

constexpr double kFlatSlopeDeg = 0.5;   // a max angle below this is "planar only"
constexpr double kResample     = 0.15;  // mm between lifted samples
constexpr double kCollinearTol = 0.005; // mm, collinear merge of lifted samples
constexpr double kFlowStep     = 0.03;  // a new first-layer piece when flow drifts by this much
constexpr double kMinFlow      = 0.5;   // first curved layer flow clamp (x nominal)
constexpr double kMaxFlow      = 1.6;

struct Params
{
    const PrintRegionConfig *region = nullptr; // the first region that enables the feature
    double h          = 0.2;  // nominal layer height
    int    n          = 1;    // curved layers
    double T          = 0.2;  // shell depth = n * h
    double max_angle  = 12.;  // effective, degrees
    double max_height = 5.;
    double env_tan    = 1.;   // tan of the head envelope's smallest side angle (45 deg if unknown)
    double env_reach  = 40.;  // envelope radius, mm
    double flow_min   = kMinFlow; // first curved layer gap clamp, x h (also min/max layer height)
    double flow_max   = kMaxFlow;
    double tip_r      = 0.5;  // radius of the nozzle's flat tip face, mm
};

struct Cell
{
    float s     = 0.f;  // top surface Z, print frame
    float slope = 90.f; // degrees
    bool  hit   = false;
    bool  ok    = false; // eligible: printable curved
    int   patch = -1;
    // Lowest curved layer printed over this cell (0 = the whole shell). Near the edge of a patch
    // the deeper curved layers would put the nozzle tip below the planar material next to it, so
    // there the shell gets thinner and the planar layers reach correspondingly higher: a ramp.
    int   kmin  = 0;
    // kmin for clipping the planar layers: the smallest kmin within one cell (about half a bead),
    // so the outer half of a curved layer's edge bead never overhangs a planar step left higher.
    int   kclip = 0;
    // Why a cell is not curved (debug map): 0 curved, 1 no surface, 2 cut face, 3 too steep, 4 too thin,
    // 5 patch flatter than a layer, 6 deeper than max height, 7 head clearance.
    uint8_t why = 1;
};

struct Grid
{
    double x0 = 0, y0 = 0, g = 0.25; // mm, cell centres at x0 + i*g
    int    nx = 0, ny = 0;
    std::vector<Cell> cells;
    Cell       &at(int i, int j)       { return cells[size_t(j) * nx + i]; }
    const Cell &at(int i, int j) const { return cells[size_t(j) * nx + i]; }
    double cx(int i) const { return x0 + i * g; }
    double cy(int j) const { return y0 + j * g; }
};

struct Patch
{
    std::vector<std::pair<int, int>> cells;
    double smin = 0, smax = 0;   // print frame
    int    host = -1;            // layer index the shell is printed in
    double host_ref = 0;         // max(host print_z, smax): the highest material when the shell prints
    ExPolygons footprint;        // of the top curved layer
    std::vector<ExPolygons> layer_footprint; // per curved layer k (cells with kmin <= k)
};

// Top envelope from above, in mesh coordinates, bounded by a segment's ceilings.
struct Surface3
{
    const sla::IndexedMesh                    &mesh;
    const std::vector<Layer::ContourZCeiling> &ceilings;
    double top;    // ray origin Z, mesh frame
    double ground; // mesh.ground_level()
    double zmin;   // object_print_z_min: print = mesh - ground + zmin

    struct Hit { bool hit = false; bool cut = false; double z = 0; Vec3d n = Vec3d::UnitZ(); double floor = -1e9; };

    Hit down(double x, double y) const
    {
        Hit r;
        sla::IndexedMesh::hit_result h = mesh.query_ray_hit(Vec3d(x, y, top), -Vec3d::UnitZ());
        // A ray exactly on a mesh edge can slip between the two facets; nudge it off the edge.
        if (! h.is_hit())
            h = mesh.query_ray_hit(Vec3d(x + 1.3e-4, y + 0.7e-4, top), -Vec3d::UnitZ());
        if (! h.is_hit())
            return r;
        r.hit = true;
        r.z   = top - h.distance();
        r.n   = h.normal();
        for (const Layer::ContourZCeiling &c : ceilings) {
            if (std::abs(c.normal.z()) < EPSILON)
                continue;
            const double zc = (c.offset - c.normal.x() * x - c.normal.y() * y) / c.normal.z();
            if (c.normal.z() > 0) {
                if (zc < r.z - 1e-4) { r.z = zc; r.n = c.normal; r.cut = true; }
            } else
                r.floor = std::max(r.floor, zc);
        }
        return r;
    }
    double to_print(double z_mesh) const { return z_mesh - ground + zmin; }
};

double slope_deg(const Vec3d &n)
{
    const double len = n.norm();
    if (len < EPSILON)
        return 90.;
    return std::acos(std::clamp(n.z() / len, -1., 1.)) * 180. / M_PI;
}

ExPolygons cells_to_expolygons(const Grid &grid, const std::function<bool(const Cell &)> &pred)
{
    // Row runs as rectangles, then one union. Rectangles are grown by a micron so neighbours merge.
    Polygons rects;
    const coord_t half = coord_t(scale_(grid.g * 0.5)) + 1000;
    for (int j = 0; j < grid.ny; ++ j) {
        int i = 0;
        while (i < grid.nx) {
            if (! pred(grid.at(i, j))) { ++ i; continue; }
            int k = i;
            while (k + 1 < grid.nx && pred(grid.at(k + 1, j))) ++ k;
            const coord_t xa = coord_t(scale_(grid.cx(i))) - half, xb = coord_t(scale_(grid.cx(k))) + half;
            const coord_t ya = coord_t(scale_(grid.cy(j))) - half, yb = coord_t(scale_(grid.cy(j))) + half;
            rects.emplace_back(Points{ Point(xa, ya), Point(xb, ya), Point(xb, yb), Point(xa, yb) });
            i = k + 1;
        }
    }
    return union_ex(rects);
}

// Grows `tiles` out to the outline where it is within `reach` of it, so the cell raster does not
// leave a strip of planar material along the wall.
ExPolygons reach_outline(const ExPolygons &tiles, const ExPolygons &outline, double reach)
{
    const float r = float(scale_(reach));
    const ExPolygons band = diff_ex(outline, offset_ex(outline, -r));
    ExPolygons grown = intersection_ex(offset_ex(tiles, r), band);
    append(grown, tiles);
    return intersection_ex(union_ex(grown), outline);
}

void collect_paths(const ExtrusionEntity *e, std::vector<const ExtrusionPath *> &out)
{
    if (const auto *p = dynamic_cast<const ExtrusionPath *>(e))
        out.push_back(p);
    else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(e))
        for (const ExtrusionPath &p : mp->paths) out.push_back(&p);
    else if (const auto *lp = dynamic_cast<const ExtrusionLoop *>(e))
        for (const ExtrusionPath &p : lp->paths) out.push_back(&p);
}

bool is_shell(const ExtrusionEntity *e)
{
    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        if (c->entities.empty())
            return false;
        for (const ExtrusionEntity *x : c->entities)
            if (! is_shell(x)) return false;
        return true;
    }
    std::vector<const ExtrusionPath *> paths;
    collect_paths(e, paths);
    return ! paths.empty() && std::all_of(paths.begin(), paths.end(), [](const ExtrusionPath *p) { return p->nonplanar_shell; });
}

// Removes `cut` (scaled) from every extrusion under `e`, appending the result to `out`.
// Entities that do not touch `cut` are copied unchanged. Returns true if anything was cut.
bool clip_entity(const ExtrusionEntity *e, const ExPolygons &cut, const BoundingBox &cut_bb, ExtrusionEntityCollection &out)
{
    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        auto sub = std::make_unique<ExtrusionEntityCollection>();
        sub->no_sort = c->no_sort;
        bool changed = false;
        for (const ExtrusionEntity *x : c->entities)
            changed |= clip_entity(x, cut, cut_bb, *sub);
        if (! changed)
            out.append(*c);
        else if (! sub->entities.empty())
            out.entities.push_back(sub.release());
        return changed;
    }
    std::vector<const ExtrusionPath *> paths;
    collect_paths(e, paths);
    if (paths.empty()) {
        out.append(*e);
        return false;
    }
    BoundingBox bb;
    for (const ExtrusionPath *p : paths)
        for (const Point3 &pt : p->polyline.points) bb.merge(Point(pt.x(), pt.y()));
    bool touches = bb.defined && bb.overlap(cut_bb);
    if (touches)
        touches = std::any_of(paths.begin(), paths.end(), [&cut](const ExtrusionPath *p) {
            return ! intersection_pl(Polylines{ p->polyline.to_polyline() }, cut).empty();
        });
    if (! touches) {
        out.append(*e);
        return false;
    }
    for (const ExtrusionPath *p : paths) {
        Polylines kept = diff_pl(Polylines{ p->polyline.to_polyline() }, cut);
        for (Polyline &pl : kept) {
            // Slivers left by the cut (shorter than ~2 beads) are not worth printing and would stand in
            // the way of the curved layers printed later over the same spot.
            if (pl.points.size() < 2 || pl.length() < scale_(0.8))
                continue;
            out.entities.push_back(new ExtrusionPath(Polyline3(pl), *p));
        }
    }
    return true;
}

// Largest layer print_z <= z (the planar top under a point), or -1 if none.
double planar_top_below(const std::vector<double> &print_zs, double z)
{
    auto it = std::upper_bound(print_zs.begin(), print_zs.end(), z + 1e-6);
    return it == print_zs.begin() ? -1. : *(it - 1);
}

// Curved shells built at slice time (with the slice-level clip), put into the fills by make_nonplanar_top() after
// infill. Keyed by the PrintObject; valid while its layers are the ones they were built for.
struct NpCached
{
    struct Shell { size_t layer; size_t region; std::unique_ptr<ExtrusionEntityCollection> shell; };
    std::vector<Shell>   shells;
    std::vector<const Layer *> layers;
    std::vector<ExPolygons>    cut; // per layer: what the slice-level clip took away (the shells' band)
    ExPolygons                 cover; // what the top curved layers cover (their beads, closed)
};
std::mutex                                                          g_np_cache_mutex;
std::unordered_map<const PrintObject *, std::shared_ptr<NpCached>> g_np_cache;

} // namespace

// The planar top shell's layer count (see nonplanar_layer_count).
static int planar_top_layer_count(const PrintRegionConfig &region, double layer_height)
{
    // PrintObject::discover_vertical_shells() keeps a layer solid while a
    // top surface is less than top_shell_layers layers OR less than top_shell_thickness above it, so the
    // count is the larger of the two (0.7 mm at 0.2 mm layers = 4 layers, not 3).
    int n = region.top_shell_layers.value;
    if (n > 0 && region.top_shell_thickness.value > 0. && layer_height > EPSILON)
        n = std::max(n, int(std::ceil(region.top_shell_thickness.value / layer_height - 1e-6)));
    return n;
}

int nonplanar_layer_count(const PrintRegionConfig &region, double layer_height)
{
    if (region.nonplanar_layer_count.value > 0)
        return region.nonplanar_layer_count.value;
    // As many as the planar top shell: PrintObject::discover_vertical_shells() keeps a layer solid while a
    // top surface is less than top_shell_layers layers OR less than top_shell_thickness above it, so the
    // count is the larger of the two (0.7 mm at 0.2 mm layers = 4 layers, not 3).
    int n = region.top_shell_layers.value;
    if (n > 0 && region.top_shell_thickness.value > 0. && layer_height > EPSILON)
        n = std::max(n, int(std::ceil(region.top_shell_thickness.value / layer_height - 1e-6)));
    return std::max(1, n);
}

// Internal solid planar layers directly under a shell: the planar top shell's layer count minus the curved layer count,
// never negative, so solid + curved layers make a normal top shell (0 with the automatic curved layer count). Taken from
// the first region that asks for nonplanar layers, like the shell itself.
static int solid_layers_under_shell(const PrintObject &object)
{
    const double h = object.config().layer_height.value;
    for (size_t r = 0; r < object.num_printing_regions(); ++ r) {
        const PrintRegionConfig &rc = object.printing_region(r).config();
        if (rc.nonplanar_layers.value)
            return h > EPSILON ? std::max(0, planar_top_layer_count(rc, h) - nonplanar_layer_count(rc, h)) : 0;
    }
    return 0;
}

bool nonplanar_top_requested(const PrintObject &object)
{
    for (size_t r = 0; r < object.num_printing_regions(); ++ r)
        if (object.printing_region(r).config().nonplanar_layers.value)
            return true;
    return false;
}

double nonplanar_effective_max_angle(const PrintRegionConfig &region, const ConfigBase &printer)
{
    double a = region.nonplanar_max_angle.value;
    const auto *en = printer.option<ConfigOptionBool>("enable_nonplanar");
    if (en != nullptr && en->value) {
        const HotendEnvelope env = HotendEnvelope::from_config(printer);
        if (env.enabled())
            for (double side : env.angle)
                a = std::min(a, side);
    }
    return std::clamp(a, 0., 89.);
}

// Analysis, eligibility, shells and the slice-level clip of one object (see nonplanar_top_clip_slices).
// `its` is the same mesh as `mesh` (slice frame), for slicing it at the shell's floor.
static NonplanarTopStats np_build(PrintObject &object, const sla::IndexedMesh &mesh, const indexed_triangle_set &its,
                                  const std::vector<Layer::ContourZCeiling> &ceilings, NpCached &out, std::string &skip)
{
    NonplanarTopStats stats;
    const auto t_begin = std::chrono::steady_clock::now();
    const LayerPtrs &layers = object.layers();
    if (layers.size() < 2 || ! nonplanar_top_requested(object) || object.print()->config().spiral_mode.value)
        return stats;

    // ---- parameters (object level: the first region that asks for it)
    Params P;
    for (size_t r = 0; r < object.num_printing_regions(); ++ r)
        if (object.printing_region(r).config().nonplanar_layers.value) {
            P.region = &object.printing_region(r).config();
            break;
        }
    const PrintRegionConfig &rc = *P.region;
    const PrintConfig       &pc = object.print()->config();
    P.h = object.config().layer_height.value;
    if (P.h <= EPSILON)
        return stats;
    P.n          = nonplanar_layer_count(rc, P.h);
    P.T          = P.n * P.h;
    P.max_angle  = nonplanar_effective_max_angle(rc, pc);
    P.max_height = rc.nonplanar_max_height.value;
    // A nozzle's flat tip is about 2.5 orifice diameters across.
    if (! pc.nozzle_diameter.values.empty())
        P.tip_r = 1.25 * pc.nozzle_diameter.values.front();
    if (! pc.max_layer_height.values.empty() && pc.max_layer_height.values.front() > EPSILON)
        P.flow_max = std::max(1., std::min(kMaxFlow, pc.max_layer_height.values.front() / P.h));
    if (! pc.min_layer_height.values.empty() && pc.min_layer_height.values.front() > EPSILON)
        P.flow_min = std::min(1., std::max(kMinFlow, pc.min_layer_height.values.front() / P.h));
    {
        const HotendEnvelope env = HotendEnvelope::from_config(pc);
        double a = 45.;
        if (pc.enable_nonplanar.value && env.enabled()) {
            a = *std::min_element(env.angle.begin(), env.angle.end());
            P.env_reach = env.height;
        }
        P.env_tan = std::tan(std::clamp(a, 1., 89.) * M_PI / 180.);
    }
    if (P.max_angle < kFlatSlopeDeg || P.max_height < P.T + P.h)
        return stats;

    const double zmin = object.slicing_parameters().object_print_z_min;
    std::vector<double> print_zs;
    for (const Layer *l : layers) print_zs.push_back(l->print_z);

    // ---- height field
    BoundingBox obb;
    for (const Layer *l : layers)
        for (const ExPolygon &ex : l->lslices) obb.merge(get_extents(ex.contour));
    if (! obb.defined)
        return stats;
    const double area = unscale<double>(obb.size().x()) * unscale<double>(obb.size().y());
    Grid G;
    G.g  = std::max(0.25, std::sqrt(area / 4e6));
    G.x0 = unscale<double>(obb.min.x()) + 0.5 * G.g;
    G.y0 = unscale<double>(obb.min.y()) + 0.5 * G.g;
    G.nx = std::max(1, int(std::ceil(unscale<double>(obb.size().x()) / G.g)));
    G.ny = std::max(1, int(std::ceil(unscale<double>(obb.size().y()) / G.g)));
    G.cells.assign(size_t(G.nx) * G.ny, Cell());

    double mesh_top = -std::numeric_limits<double>::max();
    for (const Vec3f &v : mesh.vertices()) mesh_top = std::max(mesh_top, double(v.z()));
    const Surface3 surf{ mesh, ceilings, mesh_top + 1., mesh.ground_level(), zmin };

    std::vector<char> thick_ok(G.cells.size(), 0); // enough material under the cell for the shell
    tbb::parallel_for(tbb::blocked_range<int>(0, G.ny), [&](const tbb::blocked_range<int> &range) {
        for (int j = range.begin(); j < range.end(); ++ j)
            for (int i = 0; i < G.nx; ++ i) {
                Cell &c = G.at(i, j);
                const Surface3::Hit h = surf.down(G.cx(i), G.cy(j));
                if (! h.hit)
                    continue;
                c.hit   = true;
                c.s     = float(surf.to_print(h.z));
                c.slope = float(slope_deg(h.n));
                c.why = h.cut ? 2 : 3;
                if (h.cut || h.n.z() <= 0.)
                    continue;
                // Material under the shell: the ray continues from just under the surface.
                const sla::IndexedMesh::hit_result below = mesh.query_ray_hit(Vec3d(G.cx(i), G.cy(j), h.z - 1e-3), -Vec3d::UnitZ());
                double thick = below.is_hit() ? below.distance() + 1e-3 : h.z - surf.ground;
                thick = std::min(thick, h.z - h.floor);
                thick_ok[size_t(j) * G.nx + i] = thick >= P.T + P.h - 1e-6;
                if (c.slope > P.max_angle)
                    continue;
                c.ok  = thick_ok[size_t(j) * G.nx + i];
                c.why = c.ok ? 0 : 4;
            }
    });

    // A column on the very outline can hit the side wall where it meets the top, and so reads as
    // vertical. It is not a steep spot: if a neighbour inside is eligible, so is it. (Left out, it
    // stays planar, its stair overshoot fails the clearance of the curved cell downhill of it, and the
    // exclusion cascades down the slope along the edge: tt_5 lost four 15 mm triangles that way.)
    {
        std::vector<char> edge_ok(G.cells.size(), 0);
        for (int j = 0; j < G.ny; ++ j)
            for (int i = 0; i < G.nx; ++ i) {
                const Cell &c = G.at(i, j);
                if (! c.hit || c.ok || c.why != 3)
                    continue;
                bool outline = false, inner_ok = false;
                const int nb[4][2] = { {i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1} };
                for (const auto &q : nb) {
                    if (q[0] < 0 || q[0] >= G.nx || q[1] < 0 || q[1] >= G.ny) { outline = true; continue; }
                    const Cell &qc = G.at(q[0], q[1]);
                    if (! qc.hit) outline = true;
                    else if (qc.ok) inner_ok = true;
                }
                edge_ok[size_t(j) * G.nx + i] = outline && inner_ok;
            }
        for (size_t i = 0; i < G.cells.size(); ++ i)
            if (edge_ok[i]) { G.cells[i].ok = true; G.cells[i].why = 0; }
    }

    // (The rim-tread absorption of the first version is gone: the planar part is now clipped at slice level and
    // gets its own walls and top layers, so nothing of it shows past the curved edge.)
    std::vector<char> rim(G.cells.size(), 0);

    // ---- patches (iterated: removing cells can change a patch's host and its neighbours)
    // A cliff between two gentle cells (the side of a ramp standing on a gentle base, a tower wall one cell wide)
    // must not lie inside a top: a curved line across it would drop through material printed beside it (angle
    // test: 0.33 mm). Both sides of it stay planar, so the curved tops end there.
    {
        // a step steeper than the max angle between two gentle neighbours (+ 15 um for ray noise)
        const double dzc = G.g * std::tan(P.max_angle * M_PI / 180.) + 0.015;
        std::vector<char> cliff(G.cells.size(), 0);
        for (int j = 0; j < G.ny; ++ j)
            for (int i = 0; i < G.nx; ++ i) {
                const Cell &c = G.at(i, j);
                if (! c.ok) continue;
                const int nb[4][2] = { {i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1} };
                for (const auto &q : nb)
                    if (q[0] >= 0 && q[0] < G.nx && q[1] >= 0 && q[1] < G.ny) {
                        const Cell &qc = G.at(q[0], q[1]);
                        if (qc.ok && std::abs(qc.s - c.s) > dzc)
                            cliff[size_t(j) * G.nx + i] = 1;
                    }
            }
        for (size_t i = 0; i < G.cells.size(); ++ i)
            if (cliff[i]) { G.cells[i].ok = false; G.cells[i].why = 3; }
    }
    // Columns with more than one top (an overhang over a surface, or a surface under another one): a top whose
    // footprint (or 1 mm around it) has one is not printed curved.
    std::vector<char> multi_up(G.cells.size(), 0);
    tbb::parallel_for(tbb::blocked_range<int>(0, G.ny), [&](const tbb::blocked_range<int> &range) {
        for (int j = range.begin(); j < range.end(); ++ j)
            for (int i = 0; i < G.nx; ++ i) {
                if (! G.at(i, j).hit) continue;
                std::vector<sla::IndexedMesh::hit_result> hits = mesh.query_ray_hits(Vec3d(G.cx(i) + 1.3e-4, G.cy(j) + 0.7e-4, surf.top), -Vec3d::UnitZ());
                int ups = 0; double last = 1e30;
                for (const auto &hh : hits) {
                    if (! hh.is_hit() || hh.normal().z() <= 1e-6 * hh.normal().norm()) continue;
                    if (std::abs(hh.distance() - last) < 1e-4) continue;
                    last = hh.distance();
                    ++ ups;
                }
                multi_up[size_t(j) * G.nx + i] = ups > 1;
            }
    });
    const double nozzle_d  = pc.nozzle_diameter.values.empty() ? 0.4 : pc.nozzle_diameter.values.front();
    const double need_wide = (std::max(1, rc.wall_loops.value) + 1) * 1.05 * nozzle_d;

    std::vector<Patch> patches;
    size_t      dropped_cells = 0;   // the largest top left planar, and why (for the message)
    const char *dropped_why   = nullptr;
    const double dz_cliff = G.g * std::tan(P.max_angle * M_PI / 180.) + 0.015;
    for (int iter = 0; iter < 12; ++ iter) {
        for (Cell &c : G.cells) c.patch = -1;
        patches.clear();
        for (int j = 0; j < G.ny; ++ j)
            for (int i = 0; i < G.nx; ++ i) {
                if (! G.at(i, j).ok || G.at(i, j).patch >= 0)
                    continue;
                Patch p;
                const int id = int(patches.size());
                std::vector<std::pair<int, int>> stack{ {i, j} };
                G.at(i, j).patch = id;
                p.smin = p.smax = G.at(i, j).s;
                while (! stack.empty()) {
                    const auto [a, b] = stack.back();
                    stack.pop_back();
                    p.cells.emplace_back(a, b);
                    p.smin = std::min(p.smin, double(G.at(a, b).s));
                    p.smax = std::max(p.smax, double(G.at(a, b).s));
                    const int nb[4][2] = { {a + 1, b}, {a - 1, b}, {a, b + 1}, {a, b - 1} };
                    for (const auto &q : nb)
                        if (q[0] >= 0 && q[0] < G.nx && q[1] >= 0 && q[1] < G.ny) {
                            Cell &qc = G.at(q[0], q[1]);
                            // A cliff between two gentle cells (a tower wall one cell wide) separates two tops.
                            if (qc.ok && qc.patch < 0 && std::abs(qc.s - G.at(a, b).s) <= dz_cliff) { qc.patch = id; stack.emplace_back(q[0], q[1]); }
                        }
                }
                patches.push_back(std::move(p));
            }

        bool changed = false;
        for (Patch &p : patches) {
            auto drop_all = [&] { for (auto [a, b] : p.cells) { G.at(a, b).ok = false; G.at(a, b).why = 5; } changed = true; };
            // A patch must span more than about one layer, or it is a flat top: leave it planar.
            // (Over its gentle cells: the rim treads around a flat top must not make it a patch, and a rim tread
            // whose gentle patch is gone is not one either.)
            double gmin = std::numeric_limits<double>::max(), gmax = -gmin;
            for (auto [a, b] : p.cells)
                if (! rim[size_t(b) * G.nx + a]) { gmin = std::min(gmin, double(G.at(a, b).s)); gmax = std::max(gmax, double(G.at(a, b).s)); }
            if (gmax - gmin <= 0.75 * P.h) { drop_all(); continue; }
            // A speck of gentle surface (a fillet facet) is not a top surface of its own.
            if (double(p.cells.size()) * G.g * G.g < 2.) { drop_all(); continue; }
            // Host: the highest layer whose bottom is under the patch top.
            p.host = -1;
            for (int l = int(layers.size()) - 1; l >= 0; -- l)
                if (layers[l]->print_z - layers[l]->height < p.smax - 1e-6) { p.host = l; break; }
            if (p.host < 0) { drop_all(); continue; }
            p.host_ref = std::max(layers[p.host]->print_z, p.smax);
            // Each top on its own (requirement: several tops, each with nothing above it). A top that does not qualify
            // as a whole stays planar; the others are still curved.
            auto drop_why = [&](uint8_t why) {
                for (auto [a, b] : p.cells) { G.at(a, b).ok = false; G.at(a, b).why = why; }
                changed = true;
                if (p.cells.size() > dropped_cells) {
                    dropped_cells = p.cells.size();
                    dropped_why   = why == 6 ? "the top rises more than the maximum nonplanar height" :
                                    why == 4 ? "the top surface is too narrow for the shell's walls" :
                                               "the top surface has more of the model over or under it (overhang or second surface)";
                }
            };
            // Depth: the nozzle may reach at most max_height under the highest printed material. A taller top is
            // curved where it fits (requirement: "curve what fits"); its low part stays planar like any other stairs.
            {
                const double s_floor = p.host_ref - P.max_height + P.T;
                bool cut_low = false;
                for (auto [a, b] : p.cells)
                    if (G.at(a, b).s < s_floor) { G.at(a, b).ok = false; G.at(a, b).why = 6; cut_low = true; }
                if (cut_low) { changed = true; continue; }
            }
            // Nothing over or under it, 1 mm around included.
            {
                const int rr = int(std::ceil(1.0 / G.g));
                bool second = false;
                for (size_t ci = 0; ci < p.cells.size() && ! second; ++ ci)
                    for (int dj = -rr; dj <= rr && ! second; ++ dj)
                        for (int di = -rr; di <= rr && ! second; ++ di) {
                            const int qa = p.cells[ci].first + di, qb = p.cells[ci].second + dj;
                            if (qa >= 0 && qa < G.nx && qb >= 0 && qb < G.ny && di * di + dj * dj <= rr * rr && multi_up[size_t(qb) * G.nx + qa]) second = true;
                        }
                if (second) { drop_why(1); continue; }
            }
            // Wide enough everywhere for the shell's walls and one fill line.
            {
                const int pid = int(&p - patches.data());
                const ExPolygons tiles = cells_to_expolygons(G, [pid](const Cell &c) { return c.ok && c.patch == pid; });
                double fa = 0, ta = 0;
                for (const ExPolygon &ex : tiles) fa += ex.area();
                for (const ExPolygon &ex : diff_ex(tiles, opening_ex(tiles, float(scale_(0.5 * need_wide))))) ta += ex.area();
                fa *= SCALING_FACTOR * SCALING_FACTOR; ta *= SCALING_FACTOR * SCALING_FACTOR;
                if (ta > std::max(4., 0.05 * fa)) { drop_why(4); continue; }
            }
        }
        if (changed) {
            if (iter == 11) patches.clear(); // did not settle: stay planar rather than guess
            continue;
        }

        // Head clearance, per curved layer. When curved layer k prints over cell p the nozzle tip is
        // at S_p - (n-1-k) h, and everything printed before must stay under the head: the tip face
        // (radius tip_r) and, beyond it, the envelope rising at env_tan. Already printed are planar
        // columns (to S, capped by the host layer), the planar part of shell columns
        // (up to S - (n - kmin) h + h/2) and their curved layers below k. kmin only grows, so this
        // settles; a cell where even the top curved layer collides leaves the patch.
        //
        // "Everything printed stays under the head" is max_q [top(q) - env_tan * max(0, |pq| - tip_r)] <= tip(p)
        // + allow: a grey-scale dilation of the printed-height field by a flat disk (the tip face) and then a
        // cone. Both are O(N): a small max filter and a two-pass chamfer cone. The (1, sqrt 2) chamfer
        // distance overestimates Euclidean distance by at most 8.24 %, so the cone slope is divided by
        // that to stay conservative. Up to h/2 (+0.05) of contact is accepted: the first curved layer over
        // a column sits as little as h/2 above its planar top by design, and a stair corner stands up to
        // h/2 above S. Without it a ramp could never step by one layer inside the tip face.
        for (Cell &c : G.cells) c.kmin = 0;
        {
            const float   kNone    = -1e30f;
            const double  allow    = 0.5 * P.h + 0.05;
            // The top curved layer may brush a planar stair corner by up to a layer: a stair stands up to
            // h/2 above S, plus the rise of the surface across one cell (0.12 mm at 25 deg). With only h/2
            // the top layer next to any planar cell fails, and the exclusion cascades downhill.
            const double  allow_top = P.h + 0.05;
            const double  tan_eff  = P.env_tan / 1.0824;
            const float   step1    = float(tan_eff * G.g), step2 = float(tan_eff * G.g * std::sqrt(2.));
            const int     flat_r   = int(std::floor(P.tip_r / G.g + 1e-9));
            const size_t  N        = G.cells.size();
            // Patches grouped by host_ref: planar columns stand to min(S + h/2, host_ref) of the patch printing.
            std::vector<double> hosts;
            for (const Patch &p : patches) hosts.push_back(p.host_ref);
            std::sort(hosts.begin(), hosts.end());
            hosts.erase(std::unique(hosts.begin(), hosts.end()), hosts.end());
            std::vector<float> top(N), flat(N), dil(N), rel(N), flat_rel(N);
            for (int sweep = 0; sweep < 4 * P.n + 4; ++ sweep) {
                // pass[k][cell]: curved layer k clears at that cell.
                std::vector<std::vector<char>> pass(P.n, std::vector<char>(N, 0));
                for (double H : hosts) {
                    for (int k = 0; k < P.n; ++ k) {
                        tbb::parallel_for(tbb::blocked_range<size_t>(0, N), [&](const tbb::blocked_range<size_t> &r) {
                            for (size_t i = r.begin(); i < r.end(); ++ i) {
                                const Cell &q = G.cells[i];
                                float t = kNone;
                                rel[i] = kNone;
                                if (q.hit) {
                                    if (q.ok && q.patch >= 0 && q.kmin < P.n) {
                                        double v = q.s - (P.n - q.kmin) * P.h + 0.5 * P.h;
                                        if (q.kmin <= k - 1)
                                            v = std::max(v, double(q.s) - (P.n - k) * P.h);
                                        // Shell columns are measured against the surface plane under the tip
                                        // (rel, exact within the tip face); the cone beyond takes them lowered
                                        // by the surface's own rise across the tip face.
                                        rel[i] = float(v - q.s);
                                        t = float(v - P.tip_r * std::tan(std::min(double(q.slope), P.max_angle) * M_PI / 180.));
                                    } else
                                        t = float(std::min(double(q.s) + 0.5 * P.h, H));
                                }
                                top[i] = t;
                            }
                        });
                        // Flat disk max filter (tip face).
                        tbb::parallel_for(tbb::blocked_range<int>(0, G.ny), [&](const tbb::blocked_range<int> &r) {
                            for (int b = r.begin(); b < r.end(); ++ b)
                                for (int a = 0; a < G.nx; ++ a) {
                                    float m = kNone, mr = kNone;
                                    for (int dj = -flat_r; dj <= flat_r; ++ dj)
                                        for (int di = -flat_r; di <= flat_r; ++ di) {
                                            if (di * di + dj * dj > flat_r * flat_r) continue;
                                            const int qa = a + di, qb = b + dj;
                                            if (qa < 0 || qa >= G.nx || qb < 0 || qb >= G.ny) continue;
                                            m  = std::max(m, top[size_t(qb) * G.nx + qa]);
                                            mr = std::max(mr, rel[size_t(qb) * G.nx + qa]);
                                        }
                                    flat[size_t(b) * G.nx + a]    = m;
                                    flat_rel[size_t(b) * G.nx + a] = mr;
                                }
                        });
                        // Chamfer cone dilation, forward then backward.
                        dil = flat;
                        auto at = [&](int a, int b) -> float & { return dil[size_t(b) * G.nx + a]; };
                        for (int b = 0; b < G.ny; ++ b)
                            for (int a = 0; a < G.nx; ++ a) {
                                float &v = at(a, b);
                                if (a > 0)                 v = std::max(v, at(a - 1, b) - step1);
                                if (b > 0)                 v = std::max(v, at(a, b - 1) - step1);
                                if (a > 0 && b > 0)        v = std::max(v, at(a - 1, b - 1) - step2);
                                if (a + 1 < G.nx && b > 0) v = std::max(v, at(a + 1, b - 1) - step2);
                            }
                        for (int b = G.ny - 1; b >= 0; -- b)
                            for (int a = G.nx - 1; a >= 0; -- a) {
                                float &v = at(a, b);
                                if (a + 1 < G.nx)               v = std::max(v, at(a + 1, b) - step1);
                                if (b + 1 < G.ny)               v = std::max(v, at(a, b + 1) - step1);
                                if (a + 1 < G.nx && b + 1 < G.ny) v = std::max(v, at(a + 1, b + 1) - step2);
                                if (a > 0 && b + 1 < G.ny)      v = std::max(v, at(a - 1, b + 1) - step2);
                            }
                        for (size_t i = 0; i < N; ++ i) {
                            const Cell &c = G.cells[i];
                            if (! c.ok || c.patch < 0 || patches[c.patch].host_ref != H)
                                continue;
                            const double tip = c.s - (P.n - 1 - k) * P.h;
                            const double hi  = std::max(double(dil[i]), flat_rel[i] > kNone ? c.s + double(flat_rel[i]) : -1e30);
                            pass[k][i] = hi - tip <= (k == P.n - 1 ? allow_top : allow);
                        }
                    }
                }
                bool moved = false;
                for (size_t i = 0; i < N; ++ i) {
                    Cell &c = G.cells[i];
                    if (! c.ok || c.patch < 0 || c.kmin >= P.n)
                        continue;
                    int k = c.kmin;
                    while (k < P.n && ! pass[k][i]) ++ k;
                    if (k != c.kmin) { c.kmin = k; moved = true; }
                }
                if (! moved)
                    break;
            }
        }
        for (Cell &c : G.cells)
            if (c.ok && c.kmin >= P.n) { c.ok = false; c.why = 7; changed = true; }
        if (! changed)
            break;
        if (iter == 11)
            patches.clear();
    }
    if (patches.empty()) {
        skip = dropped_why ? dropped_why : "no top surface is gentle enough";
        return stats;
    }
    // Top surfaces each with nothing above them (reference case: two slopes either side of a flat-topped tower). A
    // taller neighbour is fine: the clearance analysis above already keeps the shell clear of it. Anything else
    // prints 100 % planar, and the slicer says why.
    if (std::getenv("NPTOP_DEBUG") != nullptr) {
        for (const Patch &p : patches)
            std::fprintf(stderr, "np patch: cells %zu s %.3f..%.3f host %d ref %.3f\n", p.cells.size(), p.smin, p.smax, p.host, p.host_ref);
        size_t n6 = 0; double s6min = 1e9, s6max = -1e9;
        for (const Cell &c : G.cells) if (c.why == 6) { ++ n6; s6min = std::min(s6min, double(c.s)); s6max = std::max(s6max, double(c.s)); }
        std::fprintf(stderr, "np max-height cells %zu s %.3f..%.3f\n", n6, s6min, s6max);
    }
    for (int b = 0; b < G.ny; ++ b)
        for (int a = 0; a < G.nx; ++ a) {
            Cell &c = G.at(a, b);
            c.kclip = c.kmin;
            if (! c.ok)
                continue;
            for (int dj = -1; dj <= 1; ++ dj)
                for (int di = -1; di <= 1; ++ di) {
                    const int qa = a + di, qb = b + dj;
                    if (qa < 0 || qa >= G.nx || qb < 0 || qb >= G.ny)
                        continue;
                    const Cell &q = G.at(qa, qb);
                    if (q.ok && q.patch == c.patch)
                        c.kclip = std::min(c.kclip, q.kmin);
                }
        }

    // ---- footprints
    for (Patch &p : patches) {
        const int pid = int(&p - patches.data());
        // The outline under the patch: the lslices of the lowest layer the patch touches.
        const double zbot = p.smin - P.T + 0.5 * P.h;
        int lb = 0;
        while (lb < int(layers.size()) - 1 && layers[lb]->print_z <= zbot) ++ lb;
        // The cell raster is a staircase; its 0.25 mm steps showed as jogs in every wall loop (reported: "the loops
        // step at the seam", where the flattest point of a loop is such a step). Simplified to straight edges,
        // and pinholes left by the raster are closed.
        auto smooth = [&G](const ExPolygons &in) {
            ExPolygons out = expolygons_simplify(in, scale_(0.6 * G.g));
            for (ExPolygon &ex : out)
                ex.holes.erase(std::remove_if(ex.holes.begin(), ex.holes.end(),
                                              [&G](const Polygon &h) { return std::abs(h.area()) < sqr(scale_(2. * G.g)); }),
                               ex.holes.end());
            return out;
        };
        const ExPolygons tiles = smooth(cells_to_expolygons(G, [pid](const Cell &c) { return c.ok && c.patch == pid; }));
        p.footprint = reach_outline(tiles, layers[lb]->lslices, 1.5 * G.g);
        p.footprint = smooth(opening_ex(p.footprint, float(scale_(0.5 * G.g))));
        p.layer_footprint.assign(P.n, ExPolygons());
        for (int k = 0; k < P.n; ++ k) {
            const ExPolygons tk = smooth(cells_to_expolygons(G, [pid, k](const Cell &c) { return c.ok && c.patch == pid && c.kmin <= k; }));
            p.layer_footprint[k] = k == P.n - 1 ? p.footprint :
                smooth(opening_ex(intersection_ex(reach_outline(tk, layers[lb]->lslices, 1.5 * G.g), p.footprint), float(scale_(0.5 * G.g))));
        }
    }

    if (const char *map_path = std::getenv("NPTOP_DEBUG_MAP")) {
        // Binary PPM, one pixel per cell, +Y up: reason colour; curved cells shaded by kmin.
        static const unsigned char col[8][3] = { {40, 200, 60}, {0, 0, 0}, {120, 120, 255}, {230, 60, 60},
                                                 {240, 200, 40}, {150, 150, 150}, {200, 80, 220}, {255, 140, 0} };
        if (FILE *f = std::fopen(map_path, "wb")) {
            std::fprintf(f, "P6 %d %d 255 ", G.nx, G.ny);
            for (int b = G.ny - 1; b >= 0; -- b)
                for (int a = 0; a < G.nx; ++ a) {
                    const Cell &c = G.at(a, b);
                    unsigned char px[3] = { col[c.why][0], col[c.why][1], col[c.why][2] };
                    if (c.why == 0 && c.ok) px[1] = (unsigned char)std::max(60, 200 - 40 * c.kmin);
                    std::fwrite(px, 1, 3, f);
                }
            std::fclose(f);
        }
    }

    if (std::getenv("NPTOP_DEBUG") != nullptr)
        for (const Patch &p : patches) {
            const int pid = int(&p - patches.data());
            std::vector<size_t> kh(P.n + 1, 0);
            size_t ncells = 0;
            for (const Cell &c : G.cells)
                if (c.ok && c.patch == pid) { ++ ncells; ++ kh[std::min(c.kmin, P.n)]; }
            double a = 0;
            for (const ExPolygon &ex : p.footprint) a += ex.area();
            std::fprintf(stderr, "np patch %d: cells %zu (list %zu) s %.3f..%.3f host %d ref %.3f area %.1f grid %dx%d g %.3f kmin:",
                         pid, ncells, p.cells.size(), p.smin, p.smax, p.host, p.host_ref, a * SCALING_FACTOR * SCALING_FACTOR, G.nx, G.ny, G.g);
            for (size_t v : kh) std::fprintf(stderr, " %zu", v);
            std::fprintf(stderr, "\n");
        }

    const auto t_shell = std::chrono::steady_clock::now();

    // ---- curved shells. Built BEFORE the clipping: planar material is removed only where the top
    // curved layer really covers it (its beads, not just the footprint), so no clipped area is ever
    // left open even if a fill fails or a footprint sliver is too thin to fill.
    auto surface_at = [&surf](const Vec2d &p, double fallback) -> double {
        const Surface3::Hit h = surf.down(p.x(), p.y());
        return h.hit ? surf.to_print(h.z) : fallback;
    };

    // mm3 / width / height > 0 override the flow (ironing).
    struct Job { Points pts; int k; ExtrusionRole role; const Flow *flow; double flow_scale; bool closed; double mm3 = 0.; float width = 0.f; float height = 0.f; };
    std::vector<std::pair<LayerRegion *, std::unique_ptr<ExtrusionEntityCollection>>> shells;

    for (const Patch &p : patches) {
        if (p.footprint.empty())
            continue;
        const int    pid    = int(&p - patches.data());
        Layer       *host   = layers[p.host];
        LayerRegion *layerm = nullptr;
        for (LayerRegion *lr : host->regions())
            if (lr->region().config().nonplanar_layers.value) { layerm = lr; break; }
        if (layerm == nullptr && ! host->regions().empty())
            layerm = host->regions().front();
        if (layerm == nullptr)
            continue;
        const PrintRegionConfig &cfg = layerm->region().config();
        const double hz = host->print_z;

        const Flow ext_flow   = layerm->flow(frExternalPerimeter, P.h);
        const Flow perim_flow = layerm->flow(frPerimeter, P.h);
        const Flow solid_flow = layerm->flow(frSolidInfill, P.h);
        const Flow top_flow   = layerm->flow(frTopSolidInfill, P.h);

        // Every curved layer is built like a planar top-shell layer: wall_loops loops along its footprint
        // (outer wall + inner walls, so a patch that reaches the object's side carries the same walls as
        // the planar layers under it, lifted onto the surface) and solid lines inside them.
        const int n_loops = std::max(1, cfg.wall_loops.value);
        std::vector<double> loop_offset; // centreline inset from the footprint edge, scaled
        {
            double d = 0.5 * ext_flow.scaled_width();
            loop_offset.push_back(d);
            for (int i = 1; i < n_loops; ++ i) {
                d += i == 1 ? 0.5 * (ext_flow.scaled_spacing() + perim_flow.scaled_spacing()) : perim_flow.scaled_spacing();
                loop_offset.push_back(d);
            }
        }
        const Flow &last_loop_flow = n_loops == 1 ? ext_flow : perim_flow;
        // Fill bead edge overlaps the last loop's bead edge by 15 % of a solid line.
        auto fill_inset_for = [&](const Flow &fill) {
            return float(loop_offset.back() + 0.5 * last_loop_flow.scaled_width() + 0.5 * fill.scaled_width() - 0.15 * fill.scaled_width());
        };

        double top_angle = cfg.top_layer_direction.value >= 0. ? cfg.top_layer_direction.value : cfg.solid_infill_direction.value;
        if (cfg.nonplanar_top_surface_direction_enable.value)
            top_angle = cfg.nonplanar_top_surface_direction.value;
        auto line_pattern = [](InfillPattern p) {
            return (p == ipMonotonic || p == ipMonotonicLine || p == ipRectilinear || p == ipAlignedRectilinear || p == ipConcentric) ? p : ipMonotonic;
        };
        const InfillPattern top_pattern   = line_pattern(cfg.top_surface_pattern.value);
        const InfillPattern solid_pattern = line_pattern(cfg.internal_solid_infill_pattern.value);
        double top_flow_scale = 1.;
        if (cfg.nonplanar_top_surface_flow_ratio_enable.value && cfg.top_solid_infill_flow_ratio.get_at(0) > EPSILON)
            top_flow_scale = cfg.nonplanar_top_surface_flow_ratio.value / cfg.top_solid_infill_flow_ratio.get_at(0);

        // 2D toolpaths of every curved layer, in print order. Only the outermost curved layer is the top
        // surface (top pattern, speed and flow); the ones under it are internal solid infill, alternating
        // direction, exactly like the planar top shell they replace.
        std::vector<Job> jobs;
        Polylines        top_fill_lines, top_loop_lines;
        // Walls only along the patch's outer edge (the object's side or the seam against the planar stairs).
        // A deeper curved layer ends early where the kmin ramp starts; that edge runs across the inside of the
        // surface, and loops along it showed through the top layer as a diagonal band of inner wall.
        const ExPolygons wall_band = diff_ex(offset_ex(p.footprint, float(scale_(0.05))),
                                             offset_ex(p.footprint, -float(loop_offset.back() + 0.75 * perim_flow.scaled_spacing())));
        for (int k = 0; k < P.n; ++ k) {
            const bool top = k == P.n - 1;
            for (int li = 0; li < n_loops; ++ li) {
                const ExPolygons    loops = offset_ex(p.layer_footprint[k], -float(loop_offset[li]));
                const ExtrusionRole role  = li == 0 ? erExternalPerimeter : erPerimeter;
                const Flow         *lf    = li == 0 ? &ext_flow : &perim_flow;
                auto add_loop = [&](const Polygon &poly) {
                    if (top) { jobs.push_back({ poly.points, k, role, lf, 1., true }); return; }
                    Polyline closed(poly.points);
                    closed.points.push_back(poly.points.front());
                    Polylines kept = intersection_pl(Polylines{ closed }, wall_band);
                    if (kept.size() == 1 && std::abs(kept.front().length() - closed.length()) < scale_(0.01)) {
                        jobs.push_back({ poly.points, k, role, lf, 1., true });
                        return;
                    }
                    for (Polyline &pl : kept)
                        if (pl.length() > scale_(0.8))
                            jobs.push_back({ pl.points, k, role, lf, 1., false });
                };
                for (const ExPolygon &ex : loops) {
                    add_loop(ex.contour);
                    for (const Polygon &hole : ex.holes)
                        add_loop(hole);
                    if (top) {
                        Polyline c(ex.contour.points); c.points.push_back(ex.contour.points.front()); top_loop_lines.push_back(c);
                        for (const Polygon &hole : ex.holes) { Polyline hl(hole.points); hl.points.push_back(hole.points.front()); top_loop_lines.push_back(hl); }
                    }
                }
            }
            const Flow      &flow      = top ? top_flow : solid_flow;
            // Inside the wall band the fill stops at the last loop; at an inner (kmin ramp) edge it runs to the edge.
            const ExPolygons fill_area = top ? offset_ex(p.layer_footprint[k], -fill_inset_for(flow)) :
                intersection_ex(offset_ex(p.layer_footprint[k], -float(0.5 * flow.scaled_spacing())), offset_ex(p.footprint, -fill_inset_for(flow)));
            const double     angle     = top_angle + (((P.n - 1 - k) % 2) ? 90. : 0.);
            for (const ExPolygon &ex : fill_area) {
                Polylines lines;
                // The configured pattern first; a fill that fails falls back to plain monotonic lines.
                for (InfillPattern pat : { top ? top_pattern : solid_pattern, ipMonotonic, ipRectilinear }) {
                    std::unique_ptr<Fill> f(Fill::new_from_type(pat));
                    f->set_bounding_box(object.bounding_box());
                    f->layer_id        = host->id();
                    f->z               = hz;
                    f->spacing         = flow.spacing();
                    f->angle           = float(Geometry::deg2rad(angle));
                    f->fixed_angle     = true;
                    f->link_max_length = coord_t(scale_(3. * f->spacing));
                    f->overlap         = 0;
                    FillParams fp;
                    fp.density   = 1.f;
                    fp.monotonic = true;
                    Surface s(top ? stTop : stInternalSolid, ex);
                    try { lines = f->fill_surface(&s, fp); } catch (InfillFailedException &) { lines.clear(); }
                    if (! lines.empty())
                        break;
                }
                for (const Polyline &pl : lines) {
                    jobs.push_back({ pl.points, k, top ? erTopSolidInfill : erSolidInfill, &flow, top ? top_flow_scale : 1., false });
                    if (top) top_fill_lines.push_back(pl);
                }
            }
        }

        // Gap fill on the top curved layer: a neck or a small hole of the footprint leaves strips between the
        // loops and the fill lines too narrow for a line, and the planar top under them is cut (tt_5: 20 mm2 of
        // internal solid showing). Medial-axis lines, metered to the strip width, like a planar gap fill.
        {
            Polygons beads = offset(top_fill_lines, float(0.5 * top_flow.scaled_width()));
            append(beads, offset(top_loop_lines, float(0.5 * perim_flow.scaled_width())));
            const ExPolygons gaps = opening_ex(diff_ex(p.footprint, union_(beads)), float(scale_(0.05)));
            for (const ExPolygon &gap : gaps) {
                ThickPolylines tps;
                gap.medial_axis(scale_(0.1), scale_(1.2), &tps);
                for (const ThickPolyline &tp : tps) {
                    if (tp.points.size() < 2 || tp.length() < scale_(0.3))
                        continue;
                    double w = 0.;
                    for (coordf_t v : tp.width) w += v;
                    w = tp.width.empty() ? top_flow.scaled_width() : w / tp.width.size();
                    const double scale_w = std::clamp(w / top_flow.scaled_width(), 0.25, 1.5);
                    jobs.push_back({ tp.points, P.n - 1, erTopSolidInfill, &top_flow, top_flow_scale * scale_w, false });
                    top_fill_lines.push_back(Polyline(tp.points));
                }
            }
        }

        // Ironing over the top curved layer, on the surface itself, after it (in its layer). Same parameters as
        // planar ironing (Layer::make_ironing): spacing, inset, flow as a fraction of the layer height, angle.
        if (cfg.ironing_type.value != IroningType::NoIroning) {
            const double nozzle  = pc.nozzle_diameter.values.empty() ? 0.4 : pc.nozzle_diameter.values.front();
            const double spacing = std::max(0.02, cfg.ironing_spacing.value);
            const double ih      = P.h * 0.01 * cfg.ironing_flow.value * spacing / nozzle;
            if (ih > EPSILON) {
                const double inset = cfg.ironing_inset.value == 0. ? 0.5 * nozzle : cfg.ironing_inset.value;
                const double angle = (cfg.ironing_angle_fixed.value ? 0. : top_angle) + cfg.ironing_angle.value;
                for (const ExPolygon &ex : offset_ex(p.footprint, -float(scale_(inset)))) {
                    std::unique_ptr<Fill> f(Fill::new_from_type(line_pattern(cfg.ironing_pattern.value)));
                    f->set_bounding_box(object.bounding_box());
                    f->layer_id    = host->id();
                    f->z           = hz;
                    f->spacing     = spacing;
                    f->angle       = float(Geometry::deg2rad(angle));
                    f->fixed_angle = true;
                    f->link_max_length = coord_t(scale_(3. * spacing));
                    FillParams fp;
                    fp.density   = 1.f;
                    fp.monotonic = true;
                    Surface sf(stTop, ex);
                    Polylines lines;
                    try { lines = f->fill_surface(&sf, fp); } catch (InfillFailedException &) { lines.clear(); }
                    for (const Polyline &pl : lines) {
                        Job j{ pl.points, P.n - 1, erIroning, &top_flow, 1., false };
                        j.mm3    = nozzle * ih;
                        j.width  = Flow::rounded_rectangle_extrusion_width_from_spacing(float(nozzle), float(ih));
                        j.height = float(ih);
                        jobs.push_back(std::move(j));
                    }
                }
            }
        }

        // Seams (audit P2): a closed loop starts near the seam of the loop printed before it (aligned / nearest / back), or
        // at its rearmost point for the first one and for "rear"; it ends a seam gap short of its start, like a planar loop.
        {
            const SeamPosition sp     = object.config().seam_position.value;
            const double       nozzle = pc.nozzle_diameter.values.empty() ? 0.4 : pc.nozzle_diameter.values.front();
            const double       gap    = std::max(0., cfg.seam_gap.get_abs_value(nozzle));
            bool  have_prev = false;
            Point prev;
            for (Job &job : jobs) {
                if (! job.closed || job.pts.size() < 3)
                    continue;
                size_t best = 0;
                if (sp == spRear || ! have_prev) {
                    for (size_t i = 1; i < job.pts.size(); ++ i)
                        if (job.pts[i].y() > job.pts[best].y() || (job.pts[i].y() == job.pts[best].y() && job.pts[i].x() < job.pts[best].x())) best = i;
                } else if (sp != spRandom) {
                    double bd = std::numeric_limits<double>::max();
                    for (size_t i = 0; i < job.pts.size(); ++ i) {
                        const double d = (job.pts[i] - prev).cast<double>().squaredNorm();
                        if (d < bd) { bd = d; best = i; }
                    }
                }
                std::rotate(job.pts.begin(), job.pts.begin() + best, job.pts.end());
                prev = job.pts.front(); have_prev = true;
                // Open it: the closing point, minus the seam gap.
                Polyline pl(job.pts);
                pl.points.push_back(job.pts.front());
                if (gap > EPSILON && pl.length() > scale_(4. * gap))
                    pl.clip_end(scale_(gap));
                job.pts    = pl.points;
                job.closed = false;
            }
        }

        // Lift every toolpath (in parallel): one path per toolpath, metered per segment.
        std::vector<std::unique_ptr<ExtrusionPath>> lifted(jobs.size());
        std::vector<double>                         job_top(jobs.size(), -1e30);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, jobs.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t j = range.begin(); j < range.end(); ++ j) {
                const Job   &job   = jobs[j];
                const int    k     = job.k;
                const double depth = (P.n - 1 - k) * P.h;
                Points pts = job.pts;
                if (job.closed && ! pts.empty()) pts.push_back(pts.front());
                if (pts.size() < 2)
                    continue;
                std::vector<Vec2d> xy;
                for (size_t i = 0; i + 1 < pts.size(); ++ i) {
                    const Vec2d a = unscale(pts[i]), b = unscale(pts[i + 1]);
                    const int   ns = std::max(1, int(std::ceil((b - a).norm() / kResample)));
                    for (int s = 0; s < ns; ++ s) xy.push_back(a + (b - a) * (double(s) / ns));
                }
                xy.push_back(unscale(pts.back()));
                // A point on the very outline can graze the side wall and hit the BOTTOM face (seen on a test part:
                // a loop point at the object's edge read S = 0 and was lifted to z = -0.6). Only a hit within
                // this patch's own surface range counts; a miss or a stray hit takes the nearest good one.
                std::vector<double> z(xy.size());
                std::vector<char>   good(xy.size(), 0);
                const double        s_lo = p.smin - P.h, s_hi = p.smax + P.h;
                for (size_t i = 0; i < xy.size(); ++ i) {
                    const double s = surface_at(xy[i], std::numeric_limits<double>::quiet_NaN());
                    if (! std::isnan(s) && s >= s_lo && s <= s_hi) { z[i] = s; good[i] = 1; }
                }
                {
                    double last = std::numeric_limits<double>::quiet_NaN();
                    for (size_t i = 0; i < xy.size(); ++ i) {          // forward: carry the last good height
                        if (good[i]) last = z[i]; else z[i] = last;
                    }
                    last = std::numeric_limits<double>::quiet_NaN();
                    for (size_t i = xy.size(); i -- > 0;) {           // backward: fill a bad start
                        if (good[i]) last = z[i]; else if (std::isnan(z[i])) z[i] = last;
                    }
                }
                for (double &v : z)
                    if (std::isnan(v)) v = p.smax;
                // Spikes (audit P0: an outline sample hit a higher part of the same top, 0.33-0.40 mm over 0.1 mm): a sample
                // is checked against the top's own cells around it and against its neighbours on the path, and an outlier
                // is re-interpolated from the good samples either side.
                {
                    const double tmax = std::tan(P.max_angle * M_PI / 180.);
                    std::vector<char> bad(xy.size(), 0);
                    for (size_t i = 0; i < xy.size(); ++ i) {
                        const int ci = int(std::lround((xy[i].x() - G.x0) / G.g)), cj = int(std::lround((xy[i].y() - G.y0) / G.g));
                        double best_d = 1e30, ref = 0.;
                        for (int dj = -2; dj <= 2; ++ dj)
                            for (int di = -2; di <= 2; ++ di) {
                                const int qa = ci + di, qb = cj + dj;
                                if (qa < 0 || qa >= G.nx || qb < 0 || qb >= G.ny) continue;
                                const Cell &q = G.at(qa, qb);
                                if (! q.ok || q.patch != pid) continue;
                                const double d = std::hypot(G.cx(qa) - xy[i].x(), G.cy(qb) - xy[i].y());
                                if (d < best_d) { best_d = d; ref = q.s; }
                            }
                        if (best_d < 1e29 && std::abs(z[i] - ref) > 0.5 * P.h + tmax * best_d)
                            bad[i] = 1;
                        // Past the top's edge the surface falls away more steeply than the top; a sample there stays
                        // within the top's slope of its nearest cell (dome: a gap-fill end dipped 0.05 mm under the
                        // neighbouring line).
                        else if (best_d < 1e29 && best_d > 0.5 * G.g)
                            z[i] = std::max(z[i], ref - tmax * best_d);
                    }
                    for (size_t i = 1; i + 1 < xy.size(); ++ i) {
                        if (bad[i]) continue;
                        const double d0 = (xy[i] - xy[i - 1]).norm(), d1 = (xy[i + 1] - xy[i]).norm();
                        const double e0 = z[i] - z[i - 1], e1 = z[i] - z[i + 1];
                        // above (or below) both neighbours by more than the slope allows: a spike
                        if ((e0 > tmax * d0 + 0.1 * P.h && e1 > tmax * d1 + 0.1 * P.h) || (e0 < -(tmax * d0 + 0.1 * P.h) && e1 < -(tmax * d1 + 0.1 * P.h)))
                            bad[i] = 1;
                    }
                    if (std::count(bad.begin(), bad.end(), char(1)) < std::ptrdiff_t(xy.size())) {
                        std::vector<double> s(xy.size(), 0.);
                        for (size_t i = 1; i < xy.size(); ++ i) s[i] = s[i - 1] + (xy[i] - xy[i - 1]).norm();
                        for (size_t i = 0; i < xy.size(); ++ i) {
                            if (! bad[i]) continue;
                            size_t a = i, b = i;
                            while (a > 0 && bad[a]) -- a;
                            while (b + 1 < xy.size() && bad[b]) ++ b;
                            if (bad[a]) z[i] = z[b];
                            else if (bad[b]) z[i] = z[a];
                            else z[i] = z[a] + (z[b] - z[a]) * (s[i] - s[a]) / std::max(1e-9, s[b] - s[a]);
                        }
                    }
                }
                for (double &v : z)
                    v -= depth;
                // A closed loop starts (and ends) at its flattest point: a seam on a slope shows as a step
                // where the start and the end of the bead meet at different heights in the preview.
                if (job.closed && xy.size() > 3) {
                    const size_t m = xy.size() - 1; // xy.back() == xy.front()
                    size_t best = 0;
                    double best_s = std::numeric_limits<double>::max();
                    for (size_t i = 0; i < m; ++ i) {
                        const double s = std::abs(z[(i + 1) % m] - z[(i + m - 1) % m]);
                        if (s < best_s - 1e-9) { best_s = s; best = i; }
                    }
                    if (best != 0) {
                        std::vector<Vec2d>  xy2;
                        std::vector<double> z2;
                        for (size_t i = 0; i <= m; ++ i) { xy2.push_back(xy[(best + i) % m]); z2.push_back(z[(best + i) % m]); }
                        xy.swap(xy2);
                        z.swap(z2);
                    }
                }
                // Constant flow (metering the first layer is complicated for very little gain). The planar
                // part is the model sliced as if its top were T lower, so the gap under the first curved layer is one
                // layer on average (h/2 .. 3h/2).
                auto seg_ratio = [](size_t) { return 1.; };
                // Vertices + per-segment flow; a vertex is dropped only if every sample its chord replaces
                // stays within kCollinearTol and the flow on both sides agrees within kFlowStep.
                std::vector<Vec3d> run{ Vec3d(xy[0].x(), xy[0].y(), z[0]) };
                std::vector<float> flows;
                std::vector<Vec3d> skipped;
                for (size_t i = 0; i + 1 < xy.size(); ++ i) {
                    const double r = seg_ratio(i);
                    const Vec3d  q(xy[i + 1].x(), xy[i + 1].y(), z[i + 1]);
                    bool merge = false;
                    if (run.size() >= 2 && skipped.size() < 400 && std::abs(flows.back() - r) <= kFlowStep) {
                        const Vec3d &anchor = run[run.size() - 2];
                        if ((q - anchor).dot(run.back() - anchor) > 0 && (q - anchor).squaredNorm() > 0.) {
                            merge = Linef3::distance_to_infinite_squared(run.back(), anchor, q) < kCollinearTol * kCollinearTol;
                            for (size_t s = 0; merge && s < skipped.size(); ++ s)
                                merge = Linef3::distance_to_infinite_squared(skipped[s], anchor, q) < kCollinearTol * kCollinearTol;
                        }
                    }
                    if (merge) {
                        skipped.push_back(run.back());
                        run.back() = q;
                    } else {
                        run.push_back(q);
                        flows.push_back(float(r));
                        skipped.clear();
                    }
                }
                if (run.size() < 2)
                    continue;
                const double base_mm3 = job.mm3 > 0. ? job.mm3 : job.flow->mm3_per_mm() * job.flow_scale;
                auto path = std::make_unique<ExtrusionPath>(job.role, base_mm3, job.width > 0.f ? job.width : float(job.flow->width()),
                                                            job.height > 0.f ? job.height : float(P.h));
                Polyline3 pl3;
                double    zt = -1e30;
                for (const Vec3d &q : run) {
                    pl3.append(Point3(coord_t(scale_(q.x())), coord_t(scale_(q.y())), coord_t(scale_(q.z() - hz))));
                    zt = std::max(zt, q.z());
                }
                path->polyline        = std::move(pl3);
                path->z_contoured     = true;
                path->nonplanar_shell = true;
                if (std::any_of(flows.begin(), flows.end(), [](float f) { return std::abs(f - 1.f) > 1e-4f; }))
                    path->np_flow = std::move(flows);
                lifted[j] = std::move(path);
                job_top[j] = zt;
            }
        });

        // One child collection per curved layer k, in print order: the G-code writes each as its own layer.
        auto shell = std::make_unique<ExtrusionEntityCollection>();
        shell->no_sort = true;
        for (int k = 0; k < P.n; ++ k) {
            auto layer_k = new ExtrusionEntityCollection();
            layer_k->no_sort = true;
            shell->entities.push_back(layer_k);
        }
        bool any = false;
        for (size_t j = 0; j < lifted.size(); ++ j)
            if (lifted[j]) {
                stats.max_shell_z = std::max(stats.max_shell_z, job_top[j]);
                static_cast<ExtrusionEntityCollection *>(shell->entities[jobs[j].k])->entities.push_back(lifted[j].release());
                ++ stats.shell_paths;
                any = true;
            }
        if (! any)
            continue;
        {
            // What the top curved layer covers: only there is the planar part "under the shell" (no top of its own).
            Polygons cov = offset(top_fill_lines, float(0.5 * top_flow.scaled_width()));
            append(cov, offset(top_loop_lines, float(0.5 * ext_flow.scaled_width())));
            append(out.cover, closing_ex(cov, float(0.5 * top_flow.scaled_spacing())));
        }
        shells.emplace_back(layerm, std::move(shell));
        ++ stats.patches;
        stats.curved_layers = std::max<size_t>(stats.curved_layers, size_t(P.n));
    }

    const auto t_clip = std::chrono::steady_clock::now();

    // ---- the planar part: the model as if its top were T lower, clipped at SLICE level (before perimeters and
    // infill, so every planar layer gets its normal closed walls, infill and its own top layers). Requirement: "all the
    // printing before that must be the same as if a thickness were simply taken off the top of the model".
    // Inside the footprint, a layer keeps material where the surface is at least (n - kclip) h above its slice
    // plane: that is the model's cross-section (n - kclip) h higher up, which for one top surface with nothing
    // above it is exactly the slice of the model offset down by T (kclip > 0 only on the clearance ramp).
    std::vector<ExPolygons> cut(layers.size());
    for (const Patch &p : patches) {
        const int    pid = int(&p - patches.data());
        std::vector<int> kvals;
        for (auto [a, b] : p.cells) kvals.push_back(G.at(a, b).kclip);
        sort_remove_duplicates(kvals);
        // The cell raster per kclip; kclip 0 takes the whole footprint (grown past the outline) less the others.
        const ExPolygons fp_plus = offset_ex(p.footprint, float(scale_(0.5 * G.g)));
        std::vector<ExPolygons> zone(kvals.size());
        ExPolygons ramp;
        for (size_t q = 0; q < kvals.size(); ++ q) {
            if (kvals[q] == 0) continue;
            const int kq = kvals[q];
            zone[q] = intersection_ex(cells_to_expolygons(G, [pid, kq](const Cell &c) { return c.ok && c.patch == pid && c.kclip == kq; }), fp_plus);
            append(ramp, zone[q]);
        }
        for (size_t q = 0; q < kvals.size(); ++ q)
            if (kvals[q] == 0) zone[q] = diff_ex(fp_plus, ramp);
        // Slice planes to cut at.
        std::vector<float>               zs;
        std::vector<std::pair<int, int>> want; // (layer, zone)
        for (int l = 0; l <= p.host && l < int(layers.size()); ++ l)
            for (size_t q = 0; q < kvals.size(); ++ q) {
                const double zc = layers[l]->slice_z + (P.n - kvals[q]) * P.h;
                if (zc <= p.smin - 1e-6 || zone[q].empty()) continue; // the whole footprint is above: nothing to cut
                zs.push_back(float(zc));
                want.emplace_back(l, int(q));
            }
        std::vector<float> zs_sorted = zs;
        sort_remove_duplicates(zs_sorted);
        MeshSlicingParamsEx sp;
        sp.closing_radius = object.config().slice_closing_radius.value;
        const std::vector<ExPolygons> sec = zs_sorted.empty() ? std::vector<ExPolygons>() : slice_mesh_ex(its, zs_sorted, sp);
        for (size_t w = 0; w < want.size(); ++ w) {
            const auto [l, q] = want[w];
            const size_t si = size_t(std::lower_bound(zs_sorted.begin(), zs_sorted.end(), zs[w]) - zs_sorted.begin());
            append(cut[l], diff_ex(zone[q], si < sec.size() ? sec[si] : ExPolygons()));
        }
    }
    // Right beside a top, a planar stair standing higher than the curved top's edge is trimmed to that edge
    // (+0.05 mm) within the outer half of the edge bead plus its core (~0.4 mm): the nozzle tracing the curved edge
    // would otherwise brush the stair corner (angle test: 0.08 mm). Off the tops, the part is untouched.
    {
        const double nozzle = pc.nozzle_diameter.values.empty() ? 0.4 : pc.nozzle_diameter.values.front();
        const int    R      = std::max(1, int(std::ceil(0.95 * nozzle / G.g)));
        std::vector<float> s_near(G.cells.size(), 1e30f);
        std::vector<int>   d_near(G.cells.size(), 1 << 20), p_near(G.cells.size(), -1);
        std::vector<size_t> frontier;
        for (size_t i = 0; i < G.cells.size(); ++ i)
            if (G.cells[i].ok && G.cells[i].patch >= 0) { s_near[i] = G.cells[i].s; d_near[i] = 0; p_near[i] = G.cells[i].patch; frontier.push_back(i); }
        for (int d = 1; d <= R && ! frontier.empty(); ++ d) {
            std::vector<size_t> next;
            for (size_t i : frontier) {
                const int a = int(i % G.nx), b = int(i / G.nx);
                const int nb[4][2] = { {a + 1, b}, {a - 1, b}, {a, b + 1}, {a, b - 1} };
                for (const auto &q : nb) {
                    if (q[0] < 0 || q[0] >= G.nx || q[1] < 0 || q[1] >= G.ny) continue;
                    const size_t qi = size_t(q[1]) * G.nx + q[0];
                    if (d_near[qi] <= d) { if (d_near[qi] == d && s_near[i] < s_near[qi]) { s_near[qi] = s_near[i]; p_near[qi] = p_near[i]; } continue; }
                    d_near[qi] = d; s_near[qi] = s_near[i]; p_near[qi] = p_near[i]; next.push_back(qi);
                }
            }
            frontier.swap(next);
        }
        const Cell *c0 = G.cells.data();
        for (int l = 0; l < int(layers.size()); ++ l) {
            const double pz = layers[l]->print_z;
            const ExPolygons band = cells_to_expolygons(G, [&](const Cell &c) {
                const size_t i = size_t(&c - c0);
                // only material printed before that top's shell (its host layer and below)
                return d_near[i] > 0 && d_near[i] <= R && pz > s_near[i] + 0.01 && l <= patches[p_near[i]].host;
            });
            if (! band.empty())
                append(cut[l], band);
        }
    }
    out.cut.assign(layers.size(), ExPolygons());
    if (std::getenv("NPTOP_PROBE") != nullptr && ! layers.empty()) {
        const BoundingBox b0 = get_extents(layers.front()->lslices);
        std::fprintf(stderr, "np probe: layer 0 bbox %.2f,%.2f .. %.2f,%.2f\n", unscale<double>(b0.min.x()), unscale<double>(b0.min.y()), unscale<double>(b0.max.x()), unscale<double>(b0.max.y()));
    }
    {
        std::atomic<size_t> nclipped(0);
        tbb::parallel_for(tbb::blocked_range<int>(0, int(layers.size())), [&](const tbb::blocked_range<int> &range) {
            for (int l = range.begin(); l < range.end(); ++ l) {
                if (cut[l].empty()) continue;
                // Hairline cuts and slivers where the footprint edge and the section nearly coincide are dropped:
                // they would only give walls around nothing.
                const float      eps = float(scale_(0.05));
                const ExPolygons c   = opening_ex(union_ex(cut[l]), eps);
                if (c.empty()) continue;
                out.cut[l] = c;
                bool any = false;
                for (LayerRegion *layerm : layers[l]->regions()) {
                    Surfaces kept;
                    for (const Surface &surf : layerm->slices.surfaces)
                        for (ExPolygon &ex : diff_ex(surf.expolygon, c))
                            // a fragment the cut leaves (less than a bead square) is not printable
                            if (std::abs(ex.area()) * SCALING_FACTOR * SCALING_FACTOR >= 0.1 || intersection_ex(offset_ex(ex, eps), c).empty())
                                kept.emplace_back(surf, std::move(ex));
                    if (kept.size() != layerm->slices.surfaces.size() || true) {
                        layerm->slices.surfaces = std::move(kept);
                        any = true;
                    }
                }
                if (any) {
                    layers[l]->make_slices();
                    ++ nclipped;
                }
                if (const char *probe = std::getenv("NPTOP_PROBE")) {
                    double px = 0, py = 0;
                    if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
                        const Point pp = Point::new_scale(px, py);
                        bool in_cut = false, in_kept = false;
                        for (const ExPolygon &ex : c) in_cut |= ex.contains(pp);
                        for (const ExPolygon &ex : layers[l]->lslices) in_kept |= ex.contains(pp);
                        std::fprintf(stderr, "np probe layer %d z %.2f: cut %d kept %d\n", l, layers[l]->print_z, int(in_cut), int(in_kept));
                    }
                }
            }
        });
        stats.clipped_layers = nclipped;
    }

    // Shells: the patches of one layer region merge into one shell, layer k of every patch together, so curved
    // layer k is one layer in the G-code. Kept aside; make_nonplanar_top() puts them into the fills after infill.
    for (auto &[layerm, shell] : shells) {
        const size_t li = size_t(layerm->layer()->id());
        size_t ri = 0;
        for (size_t r = 0; r < layerm->layer()->regions().size(); ++ r)
            if (layerm->layer()->regions()[r] == layerm) ri = r;
        auto it = std::find_if(out.shells.begin(), out.shells.end(), [&](const NpCached::Shell &s) { return s.layer == li && s.region == ri; });
        if (it == out.shells.end()) {
            out.shells.push_back({ li, ri, std::move(shell) });
            continue;
        }
        for (size_t k = 0; k < shell->entities.size(); ++ k) {
            auto *src = static_cast<ExtrusionEntityCollection *>(shell->entities[k]);
            auto *dst = static_cast<ExtrusionEntityCollection *>(it->shell->entities[k]);
            dst->entities.insert(dst->entities.end(), src->entities.begin(), src->entities.end());
            src->entities.clear();
        }
    }
    for (NpCached::Shell &sh : out.shells) {
        auto &ents = sh.shell->entities;
        for (auto it = ents.begin(); it != ents.end();) {
            if (static_cast<ExtrusionEntityCollection *>(*it)->entities.empty()) { delete *it; it = ents.erase(it); }
            else ++ it;
        }
    }

    const auto t_end = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    stats.ms_analysis = ms(t_begin, t_shell);
    stats.ms_shells   = ms(t_shell, t_clip);
    stats.ms_clip     = ms(t_clip, t_end);
    BOOST_LOG_TRIVIAL(info) << "Nonplanar layers: " << stats.patches << " patch(es), " << stats.curved_layers << " curved layers, "
                            << stats.shell_paths << " shell paths, " << stats.clipped_layers << " clipped layers, max shell z "
                            << stats.max_shell_z << ", planar kept (no curved cover) " << stats.kept_planar_mm2 << " mm2; ms: analysis "
                            << stats.ms_analysis << ", shells " << stats.ms_shells << ", clip " << stats.ms_clip;
    if (std::getenv("NPTOP_DEBUG") != nullptr)
        std::fprintf(stderr, "np stats: patches %zu paths %zu clipped %zu kept_planar %.2f mm2 ms analysis %.0f shells %.0f clip %.0f\n",
                     stats.patches, stats.shell_paths, stats.clipped_layers, stats.kept_planar_mm2, stats.ms_analysis, stats.ms_shells, stats.ms_clip);
    return stats;
}

// The mesh the analysis casts rays on and slices, in the slice frame, and the cut-plane ceilings (as in
// PrintObject::contour_z()).
static void np_object_mesh(const PrintObject &object, TriangleMesh &mesh, std::vector<Layer::ContourZCeiling> &ceilings)
{
    for (const ModelVolume *v : object.model_object()->volumes)
        if (v->is_model_part() && (! object.is_segment() || segment_includes_volume(&*object.segment(), *v))) {
            TriangleMesh vol_mesh(v->mesh());
            vol_mesh.transform(v->get_matrix());
            mesh.merge(vol_mesh);
        }
    const Transform3d &trafo = object.object_to_slice();
    mesh.transform(trafo);
    if (object.is_segment()) {
        const Eigen::Matrix3d a_inv_t = trafo.linear().inverse().transpose();
        for (const CutSegmentClip &clip : object.segment()->clips) {
            const Vec3d  n_s = a_inv_t * clip.normal;
            const double len = n_s.norm();
            if (len > EPSILON)
                ceilings.push_back({ n_s / len, (clip.offset + n_s.dot(trafo.translation())) / len });
        }
    }
}

std::string nonplanar_top_clip_slices(PrintObject &object)
{
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        g_np_cache.erase(&object);
    }
    if (! nonplanar_top_requested(object) || object.layers().size() < 2)
        return std::string();
    TriangleMesh                        tmesh;
    std::vector<Layer::ContourZCeiling> ceilings;
    np_object_mesh(object, tmesh, ceilings);
    const Transform3d &trafo = object.object_to_slice();
    if ((trafo.linear() * Vec3d::UnitZ()).normalized().z() < 1. - 1e-9 || (trafo.linear().transpose() * Vec3d::UnitZ()).normalized().z() < 1. - 1e-9)
        return "the object is sliced in a tilted frame";
    if (tmesh.empty())
        return std::string();
    sla::IndexedMesh imesh(tmesh);
    imesh.ground_level_offset(-tmesh.bounding_box().min.z());
    auto        cached = std::make_shared<NpCached>();
    std::string skip;
    np_build(object, imesh, tmesh.its, ceilings, *cached, skip);
    if (! skip.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Nonplanar layers skipped for " << object.model_object()->name << ": " << skip;
        if (std::getenv("NPTOP_DEBUG") != nullptr)
            std::fprintf(stderr, "np skipped for %s: %s\n", object.model_object()->name.c_str(), skip.c_str());
        return skip;
    }
    if (cached->shells.empty())
        return std::string();
    for (const Layer *l : object.layers()) cached->layers.push_back(l);
    std::lock_guard<std::mutex> lock(g_np_cache_mutex);
    g_np_cache[&object] = std::move(cached);
    return std::string();
}

void nonplanar_top_retype_surfaces(PrintObject &object)
{
    std::shared_ptr<NpCached> cached;
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        auto it = g_np_cache.find(&object);
        if (it != g_np_cache.end()) cached = it->second;
    }
    const LayerPtrs &layers = object.layers();
    if (! cached || cached->layers.size() != layers.size() || ! std::equal(layers.begin(), layers.end(), cached->layers.begin()) ||
        cached->cut.size() != layers.size()) {
        if (std::getenv("NPTOP_DEBUG") != nullptr)
            std::fprintf(stderr, "np retype: no cache (%d)\n", int(cached != nullptr));
        return;
    }
    if (std::getenv("NPTOP_DEBUG") != nullptr) {
        size_t n = 0; for (const ExPolygons &c : cached->cut) n += ! c.empty();
        std::fprintf(stderr, "np retype: %zu layers with a cut\n", n);
    }
    // "The shell is the top": a planar top that only exists because the layer above was taken away for a shell
    // is not a top surface. It becomes internal: sparse infill runs up to the shell, which sits on it the way a planar
    // top shell sits on infill; no top shells grow down from it and nothing irons it. With little or no infill there
    // would be nothing to carry the first curved layer, so then it is one internal solid layer instead. (Internal
    // solid everywhere was +34 % filament on the coaster: Orca widens solid over sparse by its anchoring margins.)
    // When the curved layers are fewer than the planar top shell's layers, the difference is made up by that many internal
    // solid planar layers directly under the shell (not top surface, not ironed), so the whole top is as thick as a planar
    // top shell. The lowest of them is bridged over sparse infill by the normal bridge-over-infill step.
    const int m = solid_layers_under_shell(object);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size() > 0 ? layers.size() - 1 : 0), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++ l) {
            // The shell's band starts within the next max(1, m) layers up from here.
            ExPolygons above;
            for (int j = 1; j <= std::max(1, m) && l + j < layers.size(); ++ j)
                append(above, cached->cut[l + j]);
            if (above.empty()) continue;
            const ExPolygons under = intersection_ex(offset_ex(union_ex(above), float(scale_(0.1))), cached->cover);
            if (under.empty()) continue;
            for (LayerRegion *layerm : layers[l]->regions()) {
                const SurfaceType st = m > 0 || layerm->region().config().sparse_infill_density.value < 10. ? stInternalSolid : stInternal;
                for (SurfaceCollection *sc : { &layerm->slices, &layerm->fill_surfaces }) {
                    Surfaces out;
                    bool changed = false;
                    for (Surface &s : sc->surfaces) {
                        if (s.surface_type != stTop && (m == 0 || s.surface_type != stInternal)) { out.push_back(std::move(s)); continue; }
                        ExPolygons in = intersection_ex(s.expolygon, under);
                        if (in.empty()) { out.push_back(std::move(s)); continue; }
                        changed = true;
                        for (ExPolygon &ex : diff_ex(s.expolygon, under)) out.emplace_back(s, std::move(ex));
                        for (ExPolygon &ex : in) { out.emplace_back(s, std::move(ex)); out.back().surface_type = st; }
                    }
                    sc->surfaces = std::move(out); // always: every surface was moved into `out`
                }
            }
        }
    });
}

void nonplanar_top_shell_authority(PrintObject &object)
{
    if (std::getenv("NPTOP_NO_AUTH") != nullptr)
        return;
    std::shared_ptr<NpCached> cached;
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        auto it = g_np_cache.find(&object);
        if (it != g_np_cache.end()) cached = it->second;
    }
    const LayerPtrs &layers = object.layers();
    if (! cached || cached->layers.size() != layers.size() || ! std::equal(layers.begin(), layers.end(), cached->layers.begin()) ||
        cached->cut.size() != layers.size())
        return;
    // Orca makes the area under a removed top solid for the top-shell depth (vertical shells, horizontal shells, bridges
    // over infill) as if it were a planar top; under a curved shell that was +34 % filament on the coaster. Within the
    // shadow of a shell (its band in the next top_shell_layers layers up) solid and bridge infill go back to sparse,
    // except where a bottom surface needs its own shells.
    if (const char *probe = std::getenv("NPTOP_PROBE")) {
        double px = 0, py = 0;
        if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
            const Point pp = Point::new_scale(px, py);
            for (const Layer *ly : layers)
                for (const LayerRegion *lr : ly->regions())
                    for (const Surface &s : lr->fill_surfaces.surfaces)
                        if (s.expolygon.contains(pp))
                            std::fprintf(stderr, "np auth-start probe layer %zu: type %d\n", size_t(ly->id()), int(s.surface_type));
        }
    }
    const int solid_under = solid_layers_under_shell(object);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++ l) {
            for (LayerRegion *layerm : layers[l]->regions()) {
                const PrintRegionConfig &rc = layerm->region().config();
                const int depth  = std::max(1, rc.top_shell_layers.value + 1);
                const int bdepth = std::max(0, rc.bottom_shell_layers.value);
                ExPolygons shadow;
                for (int j = 1; j <= depth && l + j < layers.size(); ++ j)
                    append(shadow, cached->cut[l + j]);
                if (shadow.empty()) continue;
                shadow = intersection_ex(union_ex(shadow), cached->cover);
                // Bottom shells keep their solid layers.
                ExPolygons bottoms;
                for (int j = 0; j <= bdepth && j <= int(l); ++ j)
                    for (const LayerRegion *lr : layers[l - j]->regions())
                        for (const Surface &s : lr->slices.surfaces)
                            if (s.surface_type == stBottom || s.surface_type == stBottomBridge) bottoms.push_back(s.expolygon);
                if (! bottoms.empty())
                    shadow = diff_ex(shadow, offset_ex(union_ex(bottoms), float(scale_(1.))));
                if (shadow.empty()) continue;
                // With little or no infill the layer right under the shell stays solid, to carry it.
                if (const char *probe = std::getenv("NPTOP_PROBE")) {
                    double px = 0, py = 0;
                    if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
                        const Point pp = Point::new_scale(px, py);
                        for (const Surface &s : layerm->fill_surfaces.surfaces)
                            if (s.expolygon.contains(pp))
                                std::fprintf(stderr, "np auth probe layer %zu z %.2f: fill surface type %d\n", l, layers[l]->print_z, int(s.surface_type));
                        bool in_sl = false;
                        for (const ExPolygon &ex : layers[l]->lslices) in_sl |= ex.contains(pp);
                        if (in_sl) std::fprintf(stderr, "np auth probe layer %zu: in lslices\n", l);
                    }
                }
                ExPolygons keep_solid;
                if (rc.sparse_infill_density.value < 10. && l + 1 < layers.size())
                    keep_solid = cached->cut[l + 1];
                // The solid layers under the shell (see nonplanar_top_retype_surfaces) stay solid.
                for (int j = 1; j <= solid_under && l + j < layers.size(); ++ j)
                    append(keep_solid, cached->cut[l + j]);
                if (solid_under > 0 && ! keep_solid.empty())
                    keep_solid = union_ex(keep_solid);
                Surfaces out;
                bool changed = false;
                for (Surface &s : layerm->fill_surfaces.surfaces) {
                    if (s.surface_type != stInternalSolid && s.surface_type != stInternalBridge && s.surface_type != stTop) {
                        out.push_back(std::move(s));
                        continue;
                    }
                    ExPolygons in = intersection_ex(s.expolygon, keep_solid.empty() ? shadow : diff_ex(shadow, keep_solid));
                    if (in.empty()) { out.push_back(std::move(s)); continue; }
                    changed = true;
                    for (ExPolygon &ex : diff_ex(s.expolygon, in)) out.emplace_back(s, std::move(ex));
                    for (ExPolygon &ex : in) {
                        out.emplace_back(s, std::move(ex));
                        out.back().surface_type = stInternal;
                    }
                }
                // (always put them back: the loop above moved every surface into `out`)
                layerm->fill_surfaces.surfaces = std::move(out);
                if (changed) {
                    if (const char *probe = std::getenv("NPTOP_PROBE")) {
                        double px = 0, py = 0;
                        if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
                            const Point pp = Point::new_scale(px, py);
                            for (const Surface &s : layerm->fill_surfaces.surfaces)
                                if (s.expolygon.contains(pp))
                                    std::fprintf(stderr, "np auth probe AFTER layer %zu z %.2f: fill surface type %d\n", l, layers[l]->print_z, int(s.surface_type));
                        }
                    }
                }
            }
        }
    });
}

NonplanarTopStats make_nonplanar_top(PrintObject &object, const sla::IndexedMesh &, const std::vector<Layer::ContourZCeiling> &)
{
    NonplanarTopStats stats;
    const LayerPtrs &layers = object.layers();
    if (const char *probe = std::getenv("NPTOP_PROBE")) {
        double px = 0, py = 0;
        if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
            const Point pp = Point::new_scale(px, py);
            for (const Layer *l : layers) {
                bool fs = false, sl = false;
                for (const LayerRegion *lr : l->regions()) {
                    for (const Surface &s : lr->fill_surfaces.surfaces) fs |= s.expolygon.contains(pp);
                    for (const Surface &s : lr->slices.surfaces) sl |= s.expolygon.contains(pp);
                }
                size_t np_close = 0;
                std::vector<const ExtrusionPath *> paths;
                for (const LayerRegion *lr : l->regions())
                    for (const ExtrusionEntity *e : lr->fills.entities) {
                        std::function<void(const ExtrusionEntity *)> v = [&](const ExtrusionEntity *x) {
                            if (auto *c = dynamic_cast<const ExtrusionEntityCollection *>(x)) { for (auto *y : c->entities) v(y); return; }
                            paths.clear(); collect_paths(x, paths);
                            for (const ExtrusionPath *p : paths)
                                for (const Point3 &q : p->polyline.points)
                                    np_close += (Point(q.x(), q.y()) - pp).cast<double>().norm() < scale_(0.8);
                        };
                        v(e);
                    }
                if (fs || sl || np_close)
                    std::fprintf(stderr, "np fills probe layer %zu z %.2f: fill_surface %d slices %d fill points np_close %zu\n", size_t(l->id()), l->print_z, int(fs), int(sl), np_close);
            }
        }
    }
    // Idempotent: drop shells a previous run left in the fills (ironing can re-run without infill).
    for (Layer *layer : layers)
        for (LayerRegion *layerm : layer->regions()) {
            ExtrusionEntitiesPtr &ents = layerm->fills.entities;
            for (auto it = ents.begin(); it != ents.end();) {
                if (is_shell(*it)) { delete *it; it = ents.erase(it); }
                else ++ it;
            }
        }
    std::shared_ptr<NpCached> cached;
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        auto it = g_np_cache.find(&object);
        if (it != g_np_cache.end()) cached = it->second;
    }
    if (! cached || cached->layers.size() != layers.size() || ! std::equal(layers.begin(), layers.end(), cached->layers.begin()))
        return stats;
    for (const NpCached::Shell &sh : cached->shells) {
        if (sh.layer >= layers.size() || sh.region >= layers[sh.layer]->regions().size()) continue;
        layers[sh.layer]->regions()[sh.region]->fills.entities.push_back(sh.shell->clone());
        ++ stats.patches;
        std::vector<const ExtrusionPath *> paths;
        for (const ExtrusionEntity *k : sh.shell->entities)
            for (const ExtrusionEntity *e : static_cast<const ExtrusionEntityCollection *>(k)->entities) { paths.clear(); collect_paths(e, paths); stats.shell_paths += paths.size(); }
    }
    return stats;
}

} // namespace Slic3r
