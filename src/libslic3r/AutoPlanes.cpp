#include "AutoPlanes.hpp"

#include "AABBMesh.hpp"
#include "CutPlaneSegmentation.hpp"
#include "Geometry.hpp"
#include "SkeletonBuild.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

namespace Slic3r {
namespace AutoPlanes {

namespace {

constexpr double kFlatDeg      = 0.5;   // a face this close to the layer plane prints without visible steps
constexpr double kTopLikeDeg   = 60.;   // upward faces steeper than this are walls, not terraced tops
constexpr double kBedEps       = 0.05;  // mm: faces this close to the plate rest on it
constexpr double kPlaneEps     = 1e-3;  // mm: a face lying on a cut plane is an internal cut face
constexpr double kFlowMm3PerS  = 8.;    // effort index only: a nominal volumetric rate
constexpr double kLayerS       = 2.;    // effort index only: fixed cost per layer
constexpr double kTransitionS  = 20.;   // effort index only: cost of one inter-segment transition

double clamp1(double v) { return std::clamp(v, -1., 1.); }
double deg(double rad)  { return rad * 180. / PI; }
double rad(double deg)  { return deg * PI / 180.; }

std::string fmt1(double v) { return (boost::format("%.1f") % v).str(); }
std::string fmt2(double v) { return (boost::format("%.2f") % v).str(); }
std::string fmt0(double v) { return (boost::format("%.0f") % v).str(); }

template<class T> const T *opt(const ConfigBase &c, const char *key) { return c.option<T>(key); }

// Numeric scalar option as double. getFloat() throws on an int option (support_threshold_angle is
// one), so dispatch on the type; anything else falls back to the default rather than throwing.
double get_float(const ConfigBase &c, const char *key, double dflt)
{
    const ConfigOption *o = c.option(key);
    if (o == nullptr)
        return dflt;
    switch (o->type()) {
    case coFloat:
    case coPercent:
    case coFloatOrPercent: return o->getFloat();
    case coInt:            return double(o->getInt());
    case coBool:           return o->getBool() ? 1. : 0.;
    default:               return dflt;
    }
}

double get_first_float(const ConfigBase &c, const char *key, double dflt)
{
    if (const auto *o = c.option<ConfigOptionFloats>(key); o != nullptr && ! o->values.empty())
        return o->values.front();
    if (const auto *o = c.option<ConfigOptionFloat>(key))
        return o->value;
    return dflt;
}

// One face of a world-frame mesh.
struct Face { Vec3d n; Vec3d c; double area; std::array<Vec3d, 3> v; };

std::vector<Face> faces_of(const indexed_triangle_set &its)
{
    std::vector<Face> out;
    out.reserve(its.indices.size());
    for (const stl_triangle_vertex_indices &f : its.indices) {
        Face face;
        for (int i = 0; i < 3; ++ i)
            face.v[size_t(i)] = its.vertices[size_t(f[i])].cast<double>();
        const Vec3d cr = (face.v[1] - face.v[0]).cross(face.v[2] - face.v[0]);
        const double l = cr.norm();
        if (l < 1e-12)
            continue;
        face.n    = cr / l;
        face.area = 0.5 * l;
        face.c    = (face.v[0] + face.v[1] + face.v[2]) / 3.;
        out.push_back(face);
    }
    return out;
}

indexed_triangle_set transformed(indexed_triangle_set its, const Transform3d &t)
{
    // fix_left_handed: a mirrored instance would otherwise turn every face normal inward, and the
    // up/down classification below would read tops as overhangs.
    its_transform(its, t, true);
    return its;
}

// Every enabled cut plane of a candidate, in WORLD coordinates, to recognise internal cut faces.
struct WorldPlane { Vec3d n; double d; };

bool on_any_plane(const Face &f, const std::vector<WorldPlane> &planes)
{
    for (const WorldPlane &p : planes) {
        if (std::abs(std::abs(f.n.dot(p.n)) - 1.) > 1e-4)
            continue;
        bool all = true;
        for (const Vec3d &v : f.v)
            if (std::abs(p.n.dot(v) - p.d) > kPlaneEps) { all = false; break; }
        if (all)
            return true;
    }
    return false;
}

// Column length below a point along -d: to the object's own material or to the plate (z = 0).
double column_below(const AABBMesh &world_mesh, const Vec3d &p, const Vec3d &d)
{
    const Vec3d down  = -d;
    const Vec3d start = p + down * 1e-3;
    double      to_bed = std::numeric_limits<double>::infinity();
    if (down.z() < -1e-9)
        to_bed = std::max(0., p.z() / -down.z());
    const AABBMesh::hit_result hit = world_mesh.query_ray_hit(start, down);
    double len = to_bed;
    if (hit.is_hit())
        len = std::min(len, hit.distance() + 1e-3);
    return std::isfinite(len) ? len : 0.;
}

// Face metrics of one segment printed along world direction `d`.
void accumulate(PredictedMetrics &m, const std::vector<Face> &faces, const Vec3d &d,
                const std::vector<WorldPlane> &cut_planes, const AABBMesh &world_mesh,
                const ProcessParams &process)
{
    const double h = std::max(0.01, process.layer_height);
    for (const Face &f : faces) {
        if (on_any_plane(f, cut_planes))
            continue;                                // internal cut face, never visible or overhanging
        const double c = f.n.dot(d);
        if (c > 0.) {
            // Upward in this segment's frame.
            const double theta = deg(std::acos(clamp1(c)));   // face from the layer plane
            if (theta < kTopLikeDeg)
                m.upward_area_mm2 += f.area;
            if (theta > kFlatDeg && theta < kTopLikeDeg) {
                m.stepped_area_mm2  += f.area;
                // Layer boundaries cross the surface every h / sin(theta) along it, so a face of area A
                // carries A * sin(theta) / h of terrace edge.
                m.terrace_length_mm += f.area * std::sin(rad(theta)) / h;
            }
        } else if (c < 0.) {
            if (f.v[0].z() < kBedEps && f.v[1].z() < kBedEps && f.v[2].z() < kBedEps)
                continue;                            // rests on the plate
            const double beta = deg(std::acos(clamp1(-c)));   // overhang angle from the layer plane
            if (process.support_threshold_deg > 0. && beta < process.support_threshold_deg) {
                const double proj = f.area * (-c);   // area projected onto the layer plane
                m.overhang_area_mm2  += proj;
                m.support_volume_mm3 += proj * column_below(world_mesh, f.c, d);
            }
        }
    }
}

double angle_between_deg(const Vec3d &a, const Vec3d &b)
{
    return deg(std::acos(clamp1(a.normalized().dot(b.normalized()))));
}

std::string side_name_for(const MachineCapability &m, double reach, double azimuth)
{
    const double hotend = virtual_tilt_hotend_term_deg(m.envelope, azimuth);
    const double derived = virtual_tilt_limit_deg(m.envelope, m.gantry_height_mm, reach, azimuth);
    if (m.manual_max_tilt_deg > 0. && (derived <= 0. || m.manual_max_tilt_deg <= derived + EPSILON))
        return "the printer's maximum layer inclination (" + fmt1(m.manual_max_tilt_deg) + " deg)";
    if (m.gantry_height_mm > 0. && reach > 0. && derived < hotend - EPSILON)
        return "gantry clearance (" + fmt0(m.gantry_height_mm) + " mm over a " + fmt0(reach) + " mm segment)";
    return std::string("the hotend's ") + HotendEnvelope::side_name(virtual_tilt_binding_face(m.envelope, azimuth)) + " side";
}

} // namespace

const char *machine_kind_name(MachineKind kind)
{
    switch (kind) {
    case MachineKind::PlanarOnly:       return "planar only";
    case MachineKind::FixedNozzle3Axis: return "fixed-nozzle 3-axis (virtual 5-axis)";
    case MachineKind::Rotary5Axis:      return "rotary 5-axis";
    case MachineKind::Belt:             return "belt";
    }
    return "unknown";
}

double MachineCapability::tilt_limit_deg(double reach_mm, std::optional<double> azimuth_rad) const
{
    if (kind != MachineKind::FixedNozzle3Axis)
        return 0.;
    return virtual_tilt_limit_enforced_deg(envelope, gantry_height_mm, manual_max_tilt_deg, reach_mm, azimuth_rad);
}

MachineCapability MachineCapability::from_config(const ConfigBase &c)
{
    MachineCapability m;
    m.envelope            = HotendEnvelope::from_config(c);
    m.gantry_height_mm    = get_float(c, "nonplanar_gantry_height", 0.);
    m.manual_max_tilt_deg = std::max(0., get_float(c, "nonplanar_virtual_max_angle", 0.));
    m.max_z_mm            = get_float(c, "nonplanar_virtual_max_z", 0.);
    m.departure_mm        = get_float(c, "nonplanar_departure_distance", 0.);
    m.approach_mm         = get_float(c, "nonplanar_approach_distance", 0.);
    m.printable_height    = get_float(c, "printable_height", 0.);
    if (const auto *area = c.option<ConfigOptionPoints>("printable_area"); area != nullptr && ! area->values.empty())
        m.bed = BoundingBoxf(area->values);

    const auto *np   = c.option<ConfigOptionBool>("enable_nonplanar");
    const auto *belt = c.option<ConfigOptionBool>("belt_printer");
    const auto *sel  = c.option<ConfigOptionEnum<AxisSelection>>("axis_selection");
    if (np == nullptr || ! np->value) {
        m.kind   = MachineKind::PlanarOnly;
        m.reason = "Non-planar printing is off in this printer profile, so cut planes would have no effect.";
        return m;
    }
    if (belt != nullptr && belt->value) {
        m.kind   = MachineKind::Belt;
        m.reason = "Belt printers are not supported by automatic plane selection yet.";
        return m;
    }
    if (sel == nullptr || sel->value != AxisSelection::Virtual) {
        m.kind   = MachineKind::Rotary5Axis;
        m.reason = "This profile drives rotary axes. Automatic plane selection only plans for fixed-nozzle "
                   "3-axis printers so far; rotary toolpaths and their constraints are not implemented in it.";
        return m;
    }
    m.kind = MachineKind::FixedNozzle3Axis;
    if (! m.envelope.enabled()) {
        m.reason = "The hotend envelope is not set (Hotend cone height is 0), so the clearance of a tilted "
                   "segment cannot be checked. Only planar printing is suggested until the hotend is measured.";
        return m;
    }
    if (m.tilt_limit_deg(0., std::nullopt) <= 0.) {
        m.reason = "No tilt limit is characterised for this printer, so no tilt can be validated.";
        return m;
    }
    m.can_tilt = true;
    return m;
}

ProcessParams ProcessParams::from_config(const ConfigBase &config, const ModelObject *object)
{
    DynamicPrintConfig c;
    c.apply(config, true);
    if (object != nullptr)
        c.apply(object->config.get(), true);
    ProcessParams p;
    p.layer_height    = get_float(c, "layer_height", 0.2);
    p.nozzle_diameter = get_first_float(c, "nozzle_diameter", 0.4);
    p.line_width      = p.nozzle_diameter * 1.05;
    if (const auto *lw = c.option<ConfigOptionFloatOrPercent>("line_width"); lw != nullptr && lw->value > 0.)
        p.line_width = lw->get_abs_value(p.nozzle_diameter);
    if (const auto *es = c.option<ConfigOptionBool>("enable_support"))
        p.support_enabled = es->value;
    const double th = get_float(c, "support_threshold_angle", 30.);
    p.support_threshold_deg = th > 0. ? std::min(89., th) : 30.;   // 0 = auto: the tree generator's 30
    p.support_top_z_mm      = get_float(c, "support_top_z_distance", 0.2);
    p.support_bottom_z_mm   = get_float(c, "support_bottom_z_distance", 0.2);
    return p;
}

Vec3d normal_to_world(const Transform3d &t, const Vec3d &n)
{
    return (t.linear().inverse().transpose() * n).normalized();
}

Vec3d normal_to_local(const Transform3d &t, const Vec3d &n)
{
    return (t.linear().transpose() * n).normalized();
}

Candidate evaluate(const ModelObject &object, const Transform3d &T, const MachineCapability &machine,
                   const ProcessParams &process, const std::vector<CutPlane> &planes, const std::string &name,
                   const std::vector<bool> &from_existing, bool user_ordered)
{
    Candidate cand;
    cand.name = name;
    cand.user_ordered = user_ordered;

    const indexed_triangle_set merged_local = merged_model_part_mesh(object, true);
    const indexed_triangle_set merged_world = transformed(merged_local, T);
    const AABBMesh             world_mesh(merged_world);
    const double               h = std::max(0.01, process.layer_height);

    std::vector<CutSegmentDescriptor> segs;
    std::vector<indexed_triangle_set> seg_meshes;
    if (! planes.empty()) {
        CutSegmentationStatus status = CutSegmentationStatus::Ok;
        segs = derive_cut_segments(merged_local, planes, T, 2. * h, user_ordered, &status, &seg_meshes, object.name);
        if (segs.empty() || status != CutSegmentationStatus::Ok) {
            cand.feasible = false;
            switch (status) {
            case CutSegmentationStatus::NoSplit:        cand.rejections.push_back("The planes do not split the part."); break;
            case CutSegmentationStatus::NoRootCell:     cand.rejections.push_back("No section would rest on the bed."); break;
            case CutSegmentationStatus::SegmentTooThin: cand.rejections.push_back("A section would be thinner than two layers."); break;
            case CutSegmentationStatus::TooManyPlanes:  cand.rejections.push_back("Too many planes."); break;
            default:                                    cand.rejections.push_back("The planes do not produce a valid partition."); break;
            }
            return cand;
        }
    }

    std::vector<WorldPlane> world_planes;
    for (const CutPlane &p : planes) {
        if (! p.enabled) continue;
        const Vec3d n = normal_to_world(T, p.normal);
        world_planes.push_back({ n, n.dot(T * p.point) });
    }

    // Planned planes, one per authored plane, annotated from the segment that builds on it.
    for (size_t i = 0; i < planes.size(); ++ i) {
        PlannedPlane pp;
        pp.plane            = planes[i];
        pp.world_cut_normal = normal_to_world(T, planes[i].normal);
        pp.world_build_normal = normal_to_world(T, planes[i].effective_build_normal());
        pp.tilt_deg         = angle_between_deg(pp.world_build_normal, Vec3d::UnitZ());
        pp.azimuth_deg      = deg(std::atan2(pp.world_build_normal.y(), pp.world_build_normal.x()));
        pp.cut_vs_build_deg = angle_between_deg(pp.world_cut_normal, pp.world_build_normal);
        pp.from_existing    = i < from_existing.size() && from_existing[i];
        cand.planes.push_back(pp);
    }

    PredictedMetrics &m = cand.predicted;
    double model_volume = std::abs(its_volume(merged_world));

    if (segs.empty()) {
        // Planar: the whole part along +Z.
        accumulate(m, faces_of(merged_world), Vec3d::UnitZ(), {}, world_mesh, process);
        const BoundingBoxf3 bb = bounding_box(merged_world);
        m.layers = int(std::ceil(std::max(0., bb.max.z()) / h));
    } else {
        for (const CutSegmentDescriptor &s : segs) {
            const size_t idx = size_t(s.index);
            if (idx >= seg_meshes.size() || seg_meshes[idx].empty())
                continue;
            const indexed_triangle_set w = transformed(seg_meshes[idx], T);
            const Vec3d d = s.is_root() ? Vec3d::UnitZ() : normal_to_world(T, s.build_normal);
            accumulate(m, faces_of(w), d, world_planes, world_mesh, process);
            m.layers += int(std::ceil(std::max(0., s.build_z_max - s.slice_z_base()) / h));
            if (! s.is_root()) {
                ++ m.transitions;
                // Feasibility of this tilted segment on this machine.
                const double tilt = angle_between_deg(d, Vec3d::UnitZ());
                const double az   = std::atan2(d.y(), d.x());
                const double reach = s.build_reach_max;
                const double limit = machine.tilt_limit_deg(reach, tilt > 1e-6 ? std::optional<double>(az) : std::nullopt);
                if (s.base_plane >= 0 && size_t(s.base_plane) < cand.planes.size()) {
                    cand.planes[size_t(s.base_plane)].reach_mm  = reach;
                    cand.planes[size_t(s.base_plane)].limit_deg = limit;
                }
                if (tilt > 1e-6) {
                    if (! machine.can_tilt) {
                        cand.feasible = false;
                        cand.rejections.push_back("Section " + std::to_string(s.index) + " tilts " + fmt1(tilt) +
                                                  " deg, but " + (machine.reason.empty() ? std::string("this printer cannot print tilted sections.") : machine.reason));
                    } else if (limit <= 0. || tilt > limit + 1e-6) {
                        cand.feasible = false;
                        cand.rejections.push_back("Section " + std::to_string(s.index) + " tilts " + fmt1(tilt) +
                                                  " deg; the limit here is " + fmt1(limit) + " deg, set by " +
                                                  side_name_for(machine, reach, az) + ".");
                    }
                }
                if (! s.touches_base_plane()) {
                    cand.feasible = false;
                    cand.rejections.push_back("Section " + std::to_string(s.index) + " would start " +
                                              fmt2(s.base_plane_distance_below_material) +
                                              " mm above its cut plane, with nothing printed under it.");
                }
            }
        }
    }

    // Build volume and the machine's own total-height limit. Transition hops lift above the part, so
    // the height check is an estimate here and is only settled by slicing.
    const BoundingBoxf3 wb = bounding_box(merged_world);
    if (machine.bed.defined) {
        const double tol = 0.01;
        if (wb.min.x() < machine.bed.min.x() - tol || wb.min.y() < machine.bed.min.y() - tol ||
            wb.max.x() > machine.bed.max.x() + tol || wb.max.y() > machine.bed.max.y() + tol) {
            cand.feasible = false;
            cand.rejections.push_back("The part extends outside the printable area.");
        }
    }
    if (machine.printable_height > 0. && wb.max.z() > machine.printable_height + 0.01) {
        cand.feasible = false;
        cand.rejections.push_back("The part is taller than the printable height.");
    }
    if (m.transitions > 0 && machine.max_z_mm > 0. && wb.max.z() + std::max(machine.departure_mm, machine.approach_mm) > machine.max_z_mm) {
        cand.feasible = false;
        cand.rejections.push_back("Transitions between sections would lift above the printer's maximum Z for non-planar work (" +
                                  fmt0(machine.max_z_mm) + " mm).");
    }

    // Relative effort index: deposited volume at a nominal rate, a fixed cost per layer, a fixed cost
    // per transition. Ranks candidates; it is not a print-time estimate.
    m.effort_index_s = (model_volume + 0.5 * m.support_volume_mm3) / kFlowMm3PerS + kLayerS * m.layers + kTransitionS * m.transitions;

    // Limitations every tilted plan carries.
    for (const PlannedPlane &pp : cand.planes) {
        if (pp.tilt_deg < 1e-6)
            continue;
        if (pp.cut_vs_build_deg > 0.1)
            cand.limitations.push_back("Plane " + std::string(pp.from_existing ? "(existing) " : "") +
                "cuts at " + fmt1(pp.cut_vs_build_deg) + " deg to its layers: the tilted layers start as strips " +
                fmt1(h / std::tan(rad(pp.cut_vs_build_deg))) + " mm wide that rest unevenly on the section below.");
        else
            cand.limitations.push_back("Plane is parallel to its layers: the section below ends in "
                "stair-steps up to half a layer above and below the tilted first layer.");
    }
    if (m.transitions > 0) {
        cand.limitations.push_back("Seams are placed separately in each section and can jump at the transition.");
        cand.limitations.push_back("Tilted sections print flat tilted layers (piecewise planar), not curved layers.");
    }
    return cand;
}

namespace {

// Signed benefit of `c` over the planar baseline under the chosen preference. Positive = better.
// Each term is a fraction of the baseline's own value, so they are comparable; a term whose baseline
// is negligible is ignored rather than turned into a division by nothing.
double net_benefit(const Candidate &c, const Candidate &base, Preference pref, std::vector<std::string> *benefits)
{
    const PredictedMetrics &b = base.predicted, &m = c.predicted;
    double ws = 1., wf = 1.;
    if (pref == Preference::Supports) { ws = 1.5; wf = 0.5; }
    if (pref == Preference::Finish)   { ws = 0.5; wf = 1.5; }

    double total = 0.;
    if (b.support_volume_mm3 > 1.) {
        const double f = (b.support_volume_mm3 - m.support_volume_mm3) / b.support_volume_mm3;
        total += ws * f;
        if (benefits && std::abs(f) > 0.005)
            benefits->push_back((f > 0 ? "Predicted support volume " + fmt0(100. * f) + "% lower"
                                       : "Predicted support volume " + fmt0(-100. * f) + "% higher") +
                                " (" + fmt0(b.support_volume_mm3) + " -> " + fmt0(m.support_volume_mm3) + " mm3, estimate).");
    }
    // Finish: the change in the SHARE of upward-facing surface that prints stepped. A tilt can remove
    // steps (a slope printed parallel to its layers) and create them (a flat top printed on tilted
    // layers turns into a staircase); measuring both against the part's whole upward surface keeps
    // the penalty proportional to how much of the part is affected, even when planar had no steps.
    const double up_ref = std::max(b.upward_area_mm2, m.upward_area_mm2);
    if (up_ref > 1.) {
        const double f = (b.stepped_area_mm2 - m.stepped_area_mm2) / up_ref;
        total += wf * f;
        if (benefits && std::abs(f) > 0.005)
            benefits->push_back((f > 0 ? "Stepped upper surface reduced by " + fmt0(100. * f) + "% of the part's upward area"
                                       : "Stepped upper surface increased by " + fmt0(-100. * f) + "% of the part's upward area") +
                                " (" + fmt0(b.stepped_area_mm2) + " -> " + fmt0(m.stepped_area_mm2) + " mm2, estimate).");
    }
    if (b.effort_index_s > 0.) {
        const double f = (m.effort_index_s - b.effort_index_s) / b.effort_index_s;
        total -= std::max(0., f);
        if (benefits && f > 0.02)
            benefits->push_back("Relative effort index " + fmt0(100. * f) + "% higher (more layers and transitions).");
    }
    // Each transition is a joint with real risks (uneven first strips, seam jump): charge for it so a
    // marginal gain never buys one.
    for (const PlannedPlane &pp : c.planes)
        if (! pp.from_existing)
            total -= pp.cut_vs_build_deg > 0.1 ? 0.06 : 0.04;
    return total;
}

struct Seed
{
    Vec3d       normal;     // world, unit, upward-leaning: the build direction to try
    Vec3d       low_point;  // lowest point (world) of the region that has to sit in the tilted section
    double      area;
    std::string origin;
};

// Regions whose faces would gain from a tilt: upward faces just off flat (terraces) and downward faces
// a little flatter than the support threshold. Both are helped by leaning the build direction toward
// the face normal's horizontal component, which is the only lean a fixed upright nozzle can trade for.
std::vector<Seed> region_seeds(const indexed_triangle_set &world, const ProcessParams &process, double max_useful_tilt_deg)
{
    std::vector<Seed> seeds;
    const std::vector<Face> faces = faces_of(world);
    if (faces.size() != world.indices.size())
        return seeds;                    // degenerate faces present: neighbour indices would not line up
    const std::vector<Vec3i32> nb = its_face_neighbors(world);

    enum Kind : int { None = 0, Slope = 1, Overhang = 2 };
    std::vector<int> kind(faces.size(), None);
    for (size_t i = 0; i < faces.size(); ++ i) {
        const Face &f = faces[i];
        const double c = f.n.z();
        if (c > 0.) {
            const double theta = deg(std::acos(clamp1(c)));
            if (theta > kFlatDeg && theta < std::min(kTopLikeDeg, 3. * max_useful_tilt_deg + 5.))
                kind[i] = Slope;
        } else if (c < 0.) {   // also with supports off: an unsupported overhang sags, so a tilt matters more
            if (f.v[0].z() < kBedEps && f.v[1].z() < kBedEps && f.v[2].z() < kBedEps)
                continue;
            const double beta = deg(std::acos(clamp1(-c)));
            if (beta < process.support_threshold_deg && beta > 0.5)
                kind[i] = Overhang;
        }
    }

    // Connected components of same-kind faces whose normals lean the same way (within 30 deg azimuth).
    std::vector<int> comp(faces.size(), -1);
    int ncomp = 0;
    for (size_t s = 0; s < faces.size(); ++ s) {
        if (kind[s] == None || comp[s] >= 0)
            continue;
        const Vec2d a0 = faces[s].n.head<2>().normalized();
        std::vector<size_t> stack { s };
        comp[s] = ncomp;
        Vec3d  nsum = Vec3d::Zero();
        double area = 0.;
        Vec3d  low  = faces[s].c;
        while (! stack.empty()) {
            const size_t i = stack.back(); stack.pop_back();
            nsum += faces[i].n * faces[i].area;
            area += faces[i].area;
            for (const Vec3d &v : faces[i].v)
                if (v.z() < low.z()) low = v;
            for (int k = 0; k < 3; ++ k) {
                const int j = nb[i][k];
                if (j < 0 || comp[size_t(j)] >= 0 || kind[size_t(j)] != kind[s])
                    continue;
                const Vec2d aj = faces[size_t(j)].n.head<2>().normalized();
                if (a0.dot(aj) < std::cos(rad(30.)))
                    continue;
                comp[size_t(j)] = ncomp;
                stack.push_back(size_t(j));
            }
        }
        ++ ncomp;
        if (nsum.head<2>().norm() < 1e-9)
            continue;
        Seed seed;
        seed.area      = area;
        seed.low_point = low;
        if (kind[s] == Slope) {
            seed.normal = nsum.normalized();               // lean toward the slope's own normal
            seed.origin = "slope region";
        } else {
            // Lean toward the overhang's horizontal component (away from the material under it).
            const Vec2d xy = nsum.head<2>().normalized();
            const double need = std::max(0., process.support_threshold_deg - deg(std::acos(clamp1(-nsum.normalized().z())))) + 0.5;
            seed.normal = Vec3d(xy.x() * std::sin(rad(need)), xy.y() * std::sin(rad(need)), std::cos(rad(need)));
            seed.origin = "overhang region";
        }
        seeds.push_back(seed);
    }
    std::sort(seeds.begin(), seeds.end(), [](const Seed &l, const Seed &r) { return l.area > r.area; });
    return seeds;
}

// Clamp a desired world build direction to what the machine allows for a segment of `reach`.
Vec3d clamp_to_limit(const Vec3d &desired, const MachineCapability &m, double reach, double *limit_out)
{
    const double az    = std::atan2(desired.y(), desired.x());
    const double tilt  = angle_between_deg(desired, Vec3d::UnitZ());
    const double limit = m.tilt_limit_deg(reach, az);
    if (limit_out) *limit_out = limit;
    const double t = std::min(tilt, std::max(0., limit - 0.05));     // stay a hair inside the limit
    return Vec3d(std::cos(az) * std::sin(rad(t)), std::sin(az) * std::sin(rad(t)), std::cos(rad(t)));
}

bool looks_branched(const indexed_triangle_set &world)
{
    const BoundingBoxf3 bb = bounding_box(world);
    const Vec3d sz = bb.size();
    const double box = sz.x() * sz.y() * sz.z();
    return box > 0. && std::abs(its_volume(world)) / box < 0.35;
}

} // namespace

CutPlaneStack stack_for(const CutPlaneStack &existing, const Candidate &candidate, bool keep_existing)
{
    if (keep_existing && candidate.planes.empty())
        return existing;
    CutPlaneStack out;
    out.enabled      = ! candidate.planes.empty() || (keep_existing && existing.enabled);
    out.user_ordered = candidate.user_ordered;
    if (keep_existing) {
        out.planes = existing.planes;
        // Enabling the new plan must not activate old planes that were excluded from its evaluation
        // because the entire stored stack was switched off. Keep them available, but individually off.
        if (! existing.enabled)
            for (CutPlane &p : out.planes)
                p.enabled = false;
        for (const PlannedPlane &pp : candidate.planes)
            if (! pp.from_existing)
                out.planes.push_back(pp.plane);
    } else {
        for (const PlannedPlane &pp : candidate.planes)
            out.planes.push_back(pp.plane);
    }
    if (out.planes.empty())
        out.enabled = keep_existing ? existing.enabled : false;
    return out;
}

PlanResult plan(const ModelObject &object, const Transform3d &T, const DynamicPrintConfig &config,
                const Options &options, const Progress *progress)
{
    PlanResult res;
    res.machine = MachineCapability::from_config(config);
    res.process = ProcessParams::from_config(config, &object);
    auto canceled = [progress]() { return progress && progress->canceled && progress->canceled(); };
    auto report   = [progress](int pct, const std::string &s) { if (progress && progress->report) progress->report(pct, s); };
    auto stop_if_canceled = [&]() {
        if (! canceled()) return false;
        res.canceled = true;
        res.chosen = -1;
        res.decision = "Canceled.";
        return true;
    };

    if (stop_if_canceled()) return res;

    report(2, "Evaluating planar baseline");
    res.planar = evaluate(object, T, res.machine, res.process, {}, "Planar (no cut planes)");

    // The user's own planes, if any: kept as a comparison and, with keep_existing, as a fixed base.
    std::vector<CutPlane> fixed;
    std::vector<bool>     fixed_flags;
    if (object.cut_planes.active()) {
        for (const CutPlane &p : object.cut_planes.planes)
            if (p.enabled) { fixed.push_back(p); fixed_flags.push_back(true); }
        if (! fixed.empty()) {
            res.existing = evaluate(object, T, res.machine, res.process, fixed, "Current planes", fixed_flags,
                                    object.cut_planes.user_ordered);
            res.existing->net_benefit = net_benefit(*res.existing, res.planar, options.preference, &res.existing->benefits);
        }
    }
    if (! options.keep_existing) { fixed.clear(); fixed_flags.clear(); }
    const bool user_ordered = options.keep_existing && object.cut_planes.user_ordered;
    if (stop_if_canceled()) return res;
    if (options.max_planes <= 0) {
        res.decision = "No new planes requested. Keep the current setup.";
        return res;
    }

    if (object_has_directed_parts(object)) {
        res.decision = "This object has parts with their own build direction; automatic planes are not combined with them. Keep the current setup.";
        return res;
    }
    if (! res.machine.can_tilt) {
        res.decision = "Keep planar printing. " + res.machine.reason;
        return res;
    }

    const indexed_triangle_set world = transformed(merged_model_part_mesh(object, true), T);
    const BoundingBoxf3        wbb   = bounding_box(world);
    const double               h     = std::max(0.01, res.process.layer_height);
    const double               reach_all = std::hypot(wbb.size().x(), wbb.size().y());
    const double               worst_limit = res.machine.tilt_limit_deg(reach_all, std::nullopt);

    report(10, "Finding sloped and overhanging regions");
    std::vector<Seed> seeds = region_seeds(world, res.process, std::max(1., worst_limit));
    // Drop only regions too small to print a visible difference on (an absolute floor, not a share of
    // the part: a share made every feature of a large part disappear -- each 56 mm2 ramp of a 76 mm
    // test part was under 2%). Then merge regions of the same kind leaning the same way (within 15
    // degrees of azimuth): a fixed nozzle tilts them by the same few degrees anyway, so one section
    // serves them all, and eight ramps on one side need one plane, not eight.
    seeds.erase(std::remove_if(seeds.begin(), seeds.end(), [](const Seed &s) { return s.area < 10.; }), seeds.end());
    {
        std::vector<Seed> merged;
        for (const Seed &s : seeds) {
            const double az = std::atan2(s.normal.y(), s.normal.x());
            auto it = std::find_if(merged.begin(), merged.end(), [&](const Seed &m) {
                if (m.origin != s.origin) return false;
                double d = std::abs(std::atan2(m.normal.y(), m.normal.x()) - az);
                d = std::min(d, 2. * PI - d);
                return d < rad(15.);
            });
            if (it == merged.end()) { merged.push_back(s); continue; }
            const double a = it->area + s.area;
            it->normal    = ((it->normal * it->area + s.normal * s.area) / a).normalized();
            it->area      = a;
            if (s.low_point.z() < it->low_point.z()) it->low_point = s.low_point;
        }
        seeds = std::move(merged);
    }
    std::sort(seeds.begin(), seeds.end(), [](const Seed &l, const Seed &r) { return l.area > r.area; });
    if (seeds.size() > 6) seeds.resize(6);

    // Branch analysis -- only proposes; the proposals face the same limits as everything else.
    std::vector<std::pair<CutPlane, std::string>> skeleton_planes;
    const bool want_skeleton = options.skeleton == Options::Skeleton::On ||
        (options.skeleton == Options::Skeleton::Auto && looks_branched(world) && int(world.indices.size()) <= options.skeleton_max_faces);
    if (want_skeleton && ! canceled()) {
        report(20, "Analysing branches (skeleton)");
        try {
            SkeletonParams sp;
            SkeletonProgress skp;
            skp.canceled = [&canceled]() { return canceled(); };
            const indexed_triangle_set local = merged_model_part_mesh(object, true);
            const SkeletonResult sk = build_skeleton(local, sp, T, &skp);
            int ok = 0, rejected = 0;
            double worst_tilt = 0.;
            for (size_t e = 0; e < sk.cut_edges.size(); ++ e) {
                const SkeletonCutPlane cp = skeleton_edge_cut_plane(sk.dag, sk.tangents, sk.cut_edges[e].first, sk.cut_edges[e].second);
                const Vec3d nw   = normal_to_world(T, cp.normal);
                const double tilt = angle_between_deg(nw, Vec3d::UnitZ());
                const double lim  = res.machine.tilt_limit_deg(0., std::atan2(nw.y(), nw.x()));
                worst_tilt = std::max(worst_tilt, tilt);
                if (tilt <= lim) {
                    CutPlane p; p.point = cp.point; p.normal = cp.normal; p.enabled = true;
                    skeleton_planes.emplace_back(p, "skeleton branch");
                    ++ ok;
                } else
                    ++ rejected;
            }
            res.skeleton_note = "Skeleton: " + std::to_string(sk.graph.num_nodes()) + " nodes, " +
                std::to_string(sk.cut_edges.size()) + " branch cuts; " + std::to_string(ok) + " within the tilt limit, " +
                std::to_string(rejected) + " rejected (steepest " + fmt1(worst_tilt) + " deg). Branch cuts follow the "
                "centreline, which on a fixed upright nozzle is usually far steeper than the printer allows.";
        } catch (const std::exception &ex) {
            res.skeleton_note = std::string("Skeleton analysis failed and was skipped: ") + ex.what();
        }
    } else if (options.skeleton == Options::Skeleton::Off) {
        res.skeleton_note = "Skeleton analysis off.";
    } else {
        res.skeleton_note = "Skeleton analysis not used: the part does not look branched (or is too dense to analyse quickly).";
    }

    // Single-plane candidates.
    struct Proposal { std::vector<CutPlane> planes; std::string name; std::string origin; };
    std::vector<Proposal> proposals;
    const Transform3d     Tinv = T.inverse();
    int seed_no = 0;
    for (const Seed &seed : seeds) {
        ++ seed_no;
        // Reach of the tilted section is not known until it is cut; start from the whole part and let
        // evaluate() check the exact one.
        double limit = 0.;
        const Vec3d dir = clamp_to_limit(seed.normal, res.machine, reach_all, &limit);
        if (angle_between_deg(dir, Vec3d::UnitZ()) < 0.2)
            continue;
        const Vec3d dir_local = normal_to_local(T, dir);
        const Vec2d lean_xy   = dir.head<2>().norm() > 1e-9 ? Vec2d(dir.head<2>().normalized()) : Vec2d::Zero();
        for (int orient = 0; orient < 3; ++ orient) {
            // orient 0: cut parallel to the tilted layers; 1: horizontal cut, tilted layers above it;
            // 2: vertical cut facing the lean, so only what lies beyond the region's root is tilted --
            //    the shape that lets two features leaning opposite ways (bracket arms) each get their own
            //    section, where any plane spanning the part would tilt one of them the wrong way.
            if (orient == 2 && lean_xy.squaredNorm() < 0.5)
                continue;
            const Vec3d cut_world = orient == 0 ? dir : orient == 1 ? Vec3d::UnitZ() : Vec3d(lean_xy.x(), lean_xy.y(), 0.);
            // Lowest point of the region along the cut normal, then a little below so the whole region
            // is in the tilted section.
            for (double drop : { h, 3. * h }) {
                const Vec3d  p_world = seed.low_point - cut_world * drop;
                CutPlane     p;
                p.point   = Tinv * p_world;
                p.enabled = true;
                if (orient == 0) {
                    // Cut and layers share one orientation: the plane's own normal is the build direction.
                    p.normal = dir_local;
                } else {
                    // Horizontal cut, tilted layers above it: the build direction is a separate override,
                    // which the data model keeps distinct from the cut orientation.
                    p.normal           = normal_to_local(T, cut_world);
                    p.build_normal     = dir_local;
                    p.has_build_normal = true;
                }
                std::vector<CutPlane> planes = fixed;
                planes.push_back(p);
                proposals.push_back({ planes, seed.origin + " " + std::to_string(seed_no) +
                                      (orient == 0 ? ", cut parallel to layers" : orient == 1 ? ", horizontal cut" : ", side cut") +
                                      (drop > 1.5 * h ? ", lower" : ""), seed.origin });
            }
        }
    }
    for (const auto &sp : skeleton_planes) {
        std::vector<CutPlane> planes = fixed;
        planes.push_back(sp.first);
        proposals.push_back({ planes, sp.second, sp.second });
    }

    auto make = [&](const Proposal &pr) {
        std::vector<bool> flags(pr.planes.size(), false);
        for (size_t i = 0; i < fixed.size() && i < flags.size(); ++ i) flags[i] = true;
        Candidate c = evaluate(object, T, res.machine, res.process, pr.planes, pr.name, flags, user_ordered);
        for (size_t i = fixed.size(); i < c.planes.size(); ++ i) c.planes[i].origin = pr.origin;
        for (size_t i = 0; i < fixed.size() && i < c.planes.size(); ++ i) c.planes[i].origin = "existing";
        c.net_benefit = net_benefit(c, res.planar, options.preference, &c.benefits);
        return c;
    };

    const size_t n = proposals.size();
    for (size_t i = 0; i < n; ++ i) {
        if (canceled()) { res.canceled = true; res.decision = "Canceled."; return res; }
        report(30 + int(50. * double(i) / double(std::max<size_t>(1, n))), "Evaluating candidate " + std::to_string(i + 1) + " of " + std::to_string(n));
        res.candidates.push_back(make(proposals[i]));
    }

    // Two-plane combinations of the best feasible single-plane candidates from different regions.
    if (options.max_planes >= 2 && ! canceled()) {
        // The best feasible variant of each REGION, whatever its sign: two features leaning opposite
        // ways only pay off together, so each alone can score below planar.
        std::map<std::string, size_t> best_per_region;
        for (size_t i = 0; i < res.candidates.size(); ++ i) {
            const Candidate &c = res.candidates[i];
            if (! c.feasible || c.planes.empty())
                continue;
            const std::string region = c.name.substr(0, c.name.find(','));
            auto it = best_per_region.find(region);
            if (it == best_per_region.end() || c.net_benefit > res.candidates[it->second].net_benefit)
                best_per_region[region] = i;
        }
        std::vector<size_t> good;
        for (const auto &kv : best_per_region)
            good.push_back(kv.second);
        std::sort(good.begin(), good.end(), [&](size_t l, size_t r) { return res.candidates[l].net_benefit > res.candidates[r].net_benefit; });
        if (good.size() > 4) good.resize(4);
        for (size_t a = 0; a < good.size(); ++ a)
            for (size_t b = a + 1; b < good.size(); ++ b) {
                if (canceled()) break;
                const Candidate &ca = res.candidates[good[a]], &cb = res.candidates[good[b]];
                Proposal pr;
                pr.planes = fixed;
                pr.planes.push_back(ca.planes.back().plane);
                pr.planes.push_back(cb.planes.back().plane);
                pr.name   = ca.name + " + " + cb.name;
                pr.origin = "combined";
                res.candidates.push_back(make(pr));
            }
    }

    if (stop_if_canceled()) return res;
    std::stable_sort(res.candidates.begin(), res.candidates.end(), [](const Candidate &l, const Candidate &r) {
        if (l.feasible != r.feasible) return l.feasible;
        return l.net_benefit > r.net_benefit;
    });

    report(95, "Choosing");
    if (stop_if_canceled()) return res;
    const double reference = res.existing && res.existing->feasible ? std::max(0., res.existing->net_benefit) : 0.;
    if (! res.candidates.empty() && res.candidates.front().feasible &&
        res.candidates.front().net_benefit >= options.min_net_benefit + reference) {
        res.chosen = 0;
        const Candidate &c = res.candidates.front();
        res.decision = "Suggested: " + c.name + ". Predicted net benefit " + fmt0(100. * c.net_benefit) +
                       "% over planar printing (mesh-based estimate; slice to measure).";
    } else {
        std::string why;
        if (seeds.empty() && skeleton_planes.empty())
            why = "No sloped or overhanging region large enough to gain from a tilt was found.";
        else if (res.candidates.empty())
            why = "Every region would need a tilt below the minimum worth cutting for.";
        else if (! res.candidates.front().feasible)
            why = "Every candidate breaks a printer limit: " + res.candidates.front().rejections.front();
        else
            why = "The best candidate (" + res.candidates.front().name + ") gains only " +
                  fmt0(100. * res.candidates.front().net_benefit) + "%, below the " + fmt0(100. * (options.min_net_benefit + reference)) +
                  "% needed to justify a transition.";
        res.decision = (res.existing ? "Keep the current planes. " : "Keep planar printing. ") + why;
    }
    report(100, "Done");
    stop_if_canceled();
    return res;
}

std::string to_text(const PlanResult &r)
{
    std::ostringstream ss;
    auto metrics = [&ss](const PredictedMetrics &m) {
        ss << "    predicted: overhang " << fmt0(m.overhang_area_mm2) << " mm2, support-volume proxy " << fmt0(m.support_volume_mm3)
           << " mm3, terrace edges " << fmt0(m.terrace_length_mm) << " mm, stepped area " << fmt0(m.stepped_area_mm2)
           << " mm2, layers " << m.layers << ", transitions " << m.transitions << ", effort index " << fmt0(m.effort_index_s) << "\n";
    };
    auto cand = [&](const Candidate &c) {
        ss << "  " << c.name << (c.feasible ? "" : "  [REJECTED]") << "  net " << fmt0(100. * c.net_benefit) << "%\n";
        metrics(c.predicted);
        for (const PlannedPlane &p : c.planes)
            ss << "    plane (" << p.origin << "): tilt " << fmt2(p.tilt_deg) << " deg toward az " << fmt0(p.azimuth_deg)
               << ", limit " << fmt2(p.limit_deg) << " deg, reach " << fmt0(p.reach_mm) << " mm, cut-vs-layers " << fmt1(p.cut_vs_build_deg) << " deg\n";
        for (const std::string &s : c.rejections)  ss << "    x " << s << "\n";
        for (const std::string &s : c.benefits)    ss << "    + " << s << "\n";
        for (const std::string &s : c.limitations) ss << "    ! " << s << "\n";
    };
    ss << "Machine: " << machine_kind_name(r.machine.kind) << (r.machine.can_tilt ? "" : " (no tilts)") << "\n";
    if (! r.machine.reason.empty()) ss << "  " << r.machine.reason << "\n";
    ss << "Process: layer " << fmt2(r.process.layer_height) << " mm, support " << (r.process.support_enabled ? "on" : "off")
       << ", threshold " << fmt0(r.process.support_threshold_deg) << " deg\n";
    ss << "Baseline:\n"; cand(r.planar);
    if (r.existing) { ss << "Existing:\n"; cand(*r.existing); }
    ss << "Candidates (" << r.candidates.size() << "):\n";
    for (size_t i = 0; i < r.candidates.size() && i < 8; ++ i) cand(r.candidates[i]);
    if (! r.skeleton_note.empty()) ss << r.skeleton_note << "\n";
    ss << "Decision: " << r.decision << "\n";
    return ss.str();
}

} // namespace AutoPlanes
} // namespace Slic3r
