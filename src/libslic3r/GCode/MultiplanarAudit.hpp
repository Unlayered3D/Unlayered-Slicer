#pragma once

// UNLAYERED_MOD -- post-slice audit of multiplanar toolpaths.
//
// Plane placement alone says nothing about whether the result prints. This inspects the deposition
// SEQUENCE the slicer actually produced (GCodeProcessorResult, i.e. the internal preview data) and the
// written G-code, and reports, numerically:
//
//   * foundation -- for every extruded sample, how far its bead bottom sits above the highest material
//     deposited EARLIER within a bead's reach in XY (or above the plate). A sample with nothing under
//     it beyond the allowed contact distance is "unfounded". Bridges and overhang walls are counted
//     separately, because the slicer models them as spans on purpose.
//   * transitions -- the same gap measured for the first layers of every non-root cut-plane section,
//     against the section below it: voids (gap > tolerance) and overlap (negative gap).
//   * seams -- where each layer's first outer wall starts, in machine (= physical part) coordinates,
//     on both sides of every transition.
//   * rotary words -- any A/B/C axis word in a G0/G1/G2/G3 line, which a fixed-nozzle printer must
//     never receive.
//
// What it is NOT: a bead-shape, sag, adhesion or thermal model. Gaps are between line-centre nozzle
// tip heights and nominal layer thicknesses. Clearance is the job of check_hotend_collisions(), whose
// result is reported alongside, not re-derived here.

#include "GCodeProcessor.hpp"

#include <string>
#include <vector>

namespace Slic3r {

struct MultiplanarAuditParams
{
    double sample_step_mm       = 0.25;  // spacing of samples along extrusions
    double reach_factor         = 0.75;  // XY search radius = reach_factor * bead width
    double gap_tolerance_mm     = 0.05;  // gap beyond the allowed contact distance before it counts
    double support_bottom_z_mm  = 0.2;   // allowed gap: support resting on model
    double support_top_z_mm     = 0.2;   // allowed gap: model resting on support
    double bed_tolerance_mm     = 0.05;  // a bead bottom this close to z = 0 rests on the plate
    int    transition_layers    = 3;     // layers of a non-root section examined at its transition
};

struct UnfoundedSample
{
    Vec3f    at = Vec3f::Zero();
    double   gap_mm = 0.;
    int      object = -1;     // object label, or spatial cluster when the G-code carries no labels
    int      segment = -1;
    int      role = 0;        // ExtrusionRole
    unsigned gcode_line = 0;
    unsigned layer_id = 0;
};

struct FoundationStats
{
    size_t samples          = 0;
    size_t unfounded        = 0;   // gap above allowance + tolerance, not a modelled bridge
    size_t bridged          = 0;   // gap above allowance on a bridge/overhang-wall role (intended span)
    double worst_gap_mm     = 0.;
    Vec3f  worst_at         = Vec3f::Zero();
    unsigned worst_gcode_id = 0;
    std::vector<UnfoundedSample> worst;   // the largest gaps, up to 25, worst first
    double unfounded_ratio() const { return samples ? double(unfounded) / double(samples) : 0.; }
};

struct TransitionStats
{
    int    object_label   = -1;
    int    segment        = -1;
    size_t samples        = 0;     // samples of the first layers that lie over earlier material or empty space
    size_t voids          = 0;     // bead bottom more than tolerance above what is below
    size_t overlaps       = 0;     // bead bottom more than tolerance into what is below
    double max_gap_mm     = 0.;    // largest void
    double max_overlap_mm = 0.;    // deepest overlap (positive number)
    double p95_gap_mm     = 0.;
    std::vector<UnfoundedSample> worst_voids;   // up to 10, largest first
};

struct SeamStats
{
    int    object_label = -1;
    int    segment_below = -1, segment_above = -1;
    Vec3f  seam_below = Vec3f::Zero(), seam_above = Vec3f::Zero();
    double xy_jump_mm = 0.;        // horizontal distance between the last seam below and the first above
    // Over ALL layers of the section above: how far each layer's seam sits (horizontally) from the
    // last seam below. The first tilted layers are often strips that do not reach the seam line at
    // all, so the first-layer jump alone overstates a discontinuity; this is the line the seam follows.
    size_t layers_above = 0;
    double median_offset_mm = 0.;
    double share_within_2mm = 0.;  // fraction of the layers above whose seam is within 2 mm of the line
};

struct MultiplanarAuditResult
{
    FoundationStats              model;
    FoundationStats              support;
    std::vector<TransitionStats> transitions;
    std::vector<SeamStats>       seams;
    size_t                       rotary_words = 0;    // A/B/C words on motion lines
    std::string                  first_rotary_line;
    bool                         multiplanar = false; // more than one tool axis in the result

    std::string to_json() const;
    std::string to_text() const;
};

// `gcode_path` may be empty, in which case rotary words are not scanned.
MultiplanarAuditResult audit_multiplanar_toolpaths(const GCodeProcessorResult &result, const std::string &gcode_path,
                                                   const MultiplanarAuditParams &params = MultiplanarAuditParams());

} // namespace Slic3r
