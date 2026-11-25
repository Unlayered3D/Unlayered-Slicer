#ifndef slic3r_BrimEarsAuto_hpp_
#define slic3r_BrimEarsAuto_hpp_

// The Brim ears tool's "Auto-generate", without the GUI: the object's first layer and the sharp
// corners of its outlines where an ear goes (GLGizmoBrimEars::first_layer_slicer / generate_points /
// auto_generate). Shared by the gizmo and the object.brimEars action.

#include "ExPolygon.hpp"
#include "Point.hpp"
#include "Polygon.hpp"

namespace Slic3r {

class ModelObject;

// The instance's first layer in the plate frame (scaled): model parts minus negative volumes,
// sliced 0.1 mm above the bed as the tool slices it.
ExPolygons brim_ears_first_layer(const ModelObject& object, size_t instance);

// The corners of one outline that get an ear: convex corners of an outer contour or concave
// corners of a hole (`is_outer` false; pass the hole reversed, as the tool does) whose turn is
// sharper than `max_angle_deg` (an interior angle of at most that), measured over a window of
// `detection_length_mm` of outline on each side that widens while the outline keeps turning
// (make_brim_ear_points() in Brim.hpp).
Points brim_ears_corner_points(const Polygon& polygon, float detection_length_mm, float max_angle_deg, bool is_outer);

// Every ear position of a first layer (scaled, plate frame), outer contours and holes.
Points brim_ears_auto_points(const ExPolygons& first_layer, float detection_length_mm, float max_angle_deg);

} // namespace Slic3r

#endif // slic3r_BrimEarsAuto_hpp_
