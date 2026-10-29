// UNLAYERED_MOD: nonplanar layers -- the slice-level part (see NonplanarTop.hpp). Included only by the .cpp
// files that call it, so this header can change without a full rebuild.
#pragma once

#include <string>

namespace Slic3r {

class PrintObject;

// Right after slicing, before perimeters and infill: finds the object's curved top surface, checks it is ONE
// top surface the shell can follow (otherwise returns why not, and the object prints planar), builds the
// curved shells (kept aside for make_nonplanar_top) and clips each layer's slices so the planar part is the
// model as if its top were the shell's thickness lower. Returns "" when nonplanar layers apply or are off.
std::string nonplanar_top_clip_slices(PrintObject &object);

// After detect_surfaces_type(): the planar tops under the shells are internal solid, not top (no top shells,
// no ironing under a shell).
void nonplanar_top_retype_surfaces(PrintObject &object);
// After bridge_over_infill(): solid and bridge infill in a shell's shadow goes back to sparse (bottom shells kept).
void nonplanar_top_shell_authority(PrintObject &object);

} // namespace Slic3r
