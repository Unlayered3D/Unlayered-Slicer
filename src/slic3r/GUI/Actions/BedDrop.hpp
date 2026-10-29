// Keep an automated rotate or scale from leaving a part inside the bed (reported: an automated
// rotation sometimes clipped objects into the build plate).
//
// The bridge and the catalog turn and scale an instance about its own bounding-box centre, then
// call Plater::changed_object, whose ModelObject::ensure_on_bed(allow_negative_z = true) treats a
// single-part object that ends up partly below Z = 0 as deliberately sunk and leaves it there --
// so standing a flat part up, or scaling one up, pushed its lower half into the plate. The rotate
// and scale gizmos avoid this with the rule used here (GLCanvas3D::do_rotate / do_scale): an
// instance that rested on or above the bed BEFORE the change is dropped onto it afterwards; one
// the person had sunk on purpose stays as it is.
#pragma once

#include <vector>

#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

// Each instance's lowest point, taken before the change.
inline std::vector<double> instance_min_zs(const ModelObject& mo)
{
    std::vector<double> out;
    out.reserve(mo.instances.size());
    for (size_t i = 0; i < mo.instances.size(); ++i)
        out.push_back(mo.get_instance_min_z(i));
    return out;
}

// After the change: put every instance that was not sunk before back on the bed (Z min = 0).
// `keep` (optional, per instance) leaves out instances whose Z the caller set explicitly.
inline void drop_to_bed_unless_sunk(ModelObject& mo, const std::vector<double>& min_z_before,
                                    const std::vector<bool>& keep = {})
{
    mo.invalidate_bounding_box();
    for (size_t i = 0; i < mo.instances.size() && i < min_z_before.size(); ++i) {
        // auto_drop off (the object list's Auto Drop): the person keeps this one where it is.
        if (!mo.instances[i]->auto_drop || min_z_before[i] < SINKING_Z_THRESHOLD || (i < keep.size() && keep[i]))
            continue;
        const double z = mo.get_instance_min_z(i);
        if (z != 0.0)
            mo.instances[i]->set_offset(Z, mo.instances[i]->get_offset(Z) - z);
    }
    mo.invalidate_bounding_box();
}

}}} // namespace Slic3r::GUI::Actions
