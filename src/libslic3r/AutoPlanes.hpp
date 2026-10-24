#pragma once

// UNLAYERED_MOD -- automatic, printer-aware selection of multiplanar cut planes.
//
// Given one object instance and the selected printer/process, propose a small set of declarative
// cut planes (ModelObject::cut_planes) that reduce support or terracing -- or conclude that ordinary
// planar printing should stay, with the reason. The result is a SUGGESTION: it is shown, the user
// accepts or edits it, and only then does it become part of the model.
//
// What it builds on, deliberately, rather than re-implementing:
//   * the partition is always derived with derive_cut_segments() -- the exact function Print::apply
//     uses -- so a proposed plan cannot describe segments the slicer would not produce;
//   * the tilt limit is virtual_tilt_limit_enforced_deg(), the one Print::validate() refuses a slice
//     with, evaluated per segment with that segment's own reach and lean direction;
//   * the optional branch analysis is build_skeleton()/skeleton_edge_cut_plane(), the same pipeline the
//     skeleton gizmo uses -- used only to PROPOSE planes, which then face the same limits as any other.
//
// Two kinds of numbers come out and they are kept apart on purpose:
//   * PredictedMetrics: mesh-based estimates (overhang area, a support-column volume proxy, terrace
//     edge length, layer count, a relative effort index). Cheap, approximate, used for ranking.
//   * measured numbers, which only come from actually slicing a candidate (see
//     MultiplanarAudit.hpp and the auto_planes sandbox / GUI job). The planner never claims those.
//
// Machine support: only fixed-nozzle three-axis printers ("Printer type = Virtual" with
// enable_nonplanar on) are planned for. Every other machine kind is recognised and reported as
// unsupported instead of being planned with assumptions it does not meet. Piecewise-planar regions
// only: a tilted segment prints FLAT tilted layers, never curved conformal ones.

#include "libslic3r.h"
#include "Point.hpp"
#include "BoundingBox.hpp"
#include "PrintConfig.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace AutoPlanes {

enum class MachineKind
{
    PlanarOnly,        // enable_nonplanar off: cut planes are inert on this printer
    FixedNozzle3Axis,  // axis_selection = Virtual: upright nozzle, tilted flat layers
    Rotary5Axis,       // axis_selection AB/AC/BC: real rotary axes -- not planned for yet
    Belt,              // belt printer -- not planned for yet
};

const char *machine_kind_name(MachineKind kind);

// What the selected printer profile says it can do, read from the resolved config -- never from
// hard-coded printer values.
struct MachineCapability
{
    MachineKind    kind { MachineKind::PlanarOnly };
    // The planner proposes tilted segments only when this is true. False carries the reason.
    bool           can_tilt { false };
    std::string    reason;

    HotendEnvelope envelope;
    double         gantry_height_mm { 0. };   // nonplanar_gantry_height, 0 = never measured
    double         manual_max_tilt_deg { 0. };// nonplanar_virtual_max_angle, 0 = not applied
    double         max_z_mm { 0. };           // nonplanar_virtual_max_z, 0 = not applied
    double         departure_mm { 0. };       // nonplanar_departure_distance
    double         approach_mm { 0. };        // nonplanar_approach_distance
    BoundingBoxf   bed;                       // printable_area bounds (XY)
    double         printable_height { 0. };

    // The enforced limit for a segment reaching `reach_mm` horizontally and leaning toward
    // `azimuth_rad` -- exactly what Print::validate() checks. 0 = nothing characterised: the planner
    // treats that as "no tilt may be proposed", never as "any tilt is fine".
    double tilt_limit_deg(double reach_mm, std::optional<double> azimuth_rad) const;

    static MachineCapability from_config(const ConfigBase &config);
};

struct ProcessParams
{
    double layer_height        { 0.2 };
    double nozzle_diameter     { 0.4 };
    double line_width          { 0.42 };
    bool   support_enabled     { true };
    double support_threshold_deg { 30. };   // overhangs flatter than this (from horizontal) need support
    double support_top_z_mm    { 0.2 };
    double support_bottom_z_mm { 0.2 };

    // Process values with the object's own overrides applied on top of `config`.
    static ProcessParams from_config(const ConfigBase &config, const ModelObject *object = nullptr);
};

enum class Preference { Balanced, Supports, Finish };

struct Options
{
    Preference preference      { Preference::Balanced };
    // Net benefit (fraction, see Candidate::net_benefit) a plan must reach before it replaces
    // planar printing. Below it the answer is "keep planar".
    double     min_net_benefit { 0.10 };
    int        max_planes      { 2 }; // maximum new planes; existing manual planes do not count
    // Branch analysis: Auto runs the skeleton only on parts that look branched and are small enough
    // for mean-curvature flow to finish quickly; On forces it; Off never runs it.
    enum class Skeleton { Off, Auto, On } skeleton { Skeleton::Auto };
    int        skeleton_max_faces { 20000 };
    // Keep the object's existing (manual) planes and add to them, or plan from scratch.
    bool       keep_existing   { true };
};

// Mesh-based estimates for one plan. APPROXIMATE by construction -- see the file comment.
struct PredictedMetrics
{
    double overhang_area_mm2   { 0. }; // downward area flatter than the support threshold, in each segment's own frame
    double support_volume_mm3  { 0. }; // overhang area x column length to material/bed below (proxy)
    double terrace_length_mm   { 0. }; // total length of layer-step edges on upward surfaces
    double stepped_area_mm2    { 0. }; // upward area that is neither vertical nor flat in its frame
    double upward_area_mm2     { 0. }; // all upward-facing surface below wall steepness (the finish reference)
    int    layers              { 0 };  // summed over segments
    int    transitions         { 0 };  // non-root segments
    double effort_index_s      { 0. }; // relative effort, NOT a time estimate (see evaluate())
};

// One proposed plane, with everything the UI needs to show about it.
struct PlannedPlane
{
    CutPlane plane;                  // object-local, ready to store in ModelObject::cut_planes
    Vec3d    world_build_normal { Vec3d::UnitZ() };
    Vec3d    world_cut_normal   { Vec3d::UnitZ() };
    double   tilt_deg       { 0. };  // build normal from world vertical, after all transforms
    double   azimuth_deg    { 0. };
    double   limit_deg      { 0. };  // enforced limit for this segment's reach and lean
    double   reach_mm       { 0. };
    double   cut_vs_build_deg { 0. };// angle between the cut plane and the segment's layers (0 = parallel)
    bool     from_existing  { false };// a manual plane the user already had
    std::string origin;              // "slope region", "overhang region", "skeleton branch", "existing"
};

struct Candidate
{
    std::string               name;
    std::vector<PlannedPlane> planes;   // empty == planar baseline
    bool                     user_ordered { false }; // the ordering used to evaluate this partition
    PredictedMetrics          predicted;
    bool                      feasible { true };
    std::vector<std::string>  rejections;  // why it cannot be printed on this machine (feasible == false)
    std::vector<std::string>  benefits;    // human-readable, relative to the planar baseline
    std::vector<std::string>  limitations; // always shown, even for the chosen plan
    double                    net_benefit { 0. }; // >0 better than planar; see plan()

    bool is_planar() const { return planes.empty(); }
};

struct PlanResult
{
    MachineCapability       machine;
    ProcessParams           process;
    Candidate               planar;          // the baseline every candidate is compared with
    std::optional<Candidate> existing;       // the object's current manual plan, when it has one
    std::vector<Candidate>  candidates;      // ranked best first; rejected ones last
    int                     chosen { -1 };   // index into candidates; -1 = keep the current/planar plan
    std::string             decision;        // one paragraph the UI shows verbatim
    std::string             skeleton_note;   // what the branch analysis did and why
    bool                    canceled { false };

    const Candidate *chosen_candidate() const { return chosen >= 0 ? &candidates[size_t(chosen)] : nullptr; }
};

struct Progress
{
    std::function<bool()>                        canceled;
    std::function<void(int, const std::string&)> report;
};

// Plan for `object` as placed by `instance_trafo` (the instance's full matrix: placement, rotation,
// mirror and scale all count -- tilt is judged in the world). `config` is the resolved
// print+printer+filament config; object overrides are applied internally.
PlanResult plan(const ModelObject &object, const Transform3d &instance_trafo, const DynamicPrintConfig &config,
                const Options &options = Options(), const Progress *progress = nullptr);

// Evaluate one explicit plane list (object-local) the way plan() evaluates its own candidates. Used
// for the planar baseline, the user's existing plan, and by tests.
Candidate evaluate(const ModelObject &object, const Transform3d &instance_trafo, const MachineCapability &machine,
                   const ProcessParams &process, const std::vector<CutPlane> &planes, const std::string &name,
                   const std::vector<bool> &from_existing = {}, bool user_ordered = false);

// The stack to store when the user accepts `candidate`: its planes, enabled. With keep_existing the
// object's current planes are kept (candidates are evaluated with them included); otherwise they
// are replaced. Never touches anything but the cut-plane stack.
CutPlaneStack stack_for(const CutPlaneStack &existing, const Candidate &candidate, bool keep_existing);

// World <-> object-local conversion of plane normals and build normals, the same mapping
// PrintObject::build_normal_world() uses (inverse transpose of the instance's linear part).
Vec3d normal_to_world(const Transform3d &instance_trafo, const Vec3d &local_normal);
Vec3d normal_to_local(const Transform3d &instance_trafo, const Vec3d &world_normal);

// Plain-text report of a plan (for the CLI, logs and tests).
std::string to_text(const PlanResult &result);

} // namespace AutoPlanes
} // namespace Slic3r
