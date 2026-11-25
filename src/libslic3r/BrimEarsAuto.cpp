#include "BrimEarsAuto.hpp"

#include "Brim.hpp"
#include "ClipperUtils.hpp"
#include "Model.hpp"
#include "TriangleMeshSlicer.hpp"

namespace Slic3r {

ExPolygons brim_ears_first_layer(const ModelObject& object, size_t instance)
{
    if (instance >= object.instances.size())
        return {};
    const std::vector<float> slice_height(1, 0.1f);
    MeshSlicingParamsEx      params;
    params.mode           = MeshSlicingParams::SlicingMode::Regular;
    params.closing_radius = 0.1f;
    params.extra_offset   = 0.05f;
    params.resolution     = 0.01;
    ExPolygons part_ex, negative_ex;
    const Transform3d inst = object.instances[instance]->get_matrix();
    for (const ModelVolume* v : object.volumes) {
        if (!v->is_model_part() && !v->is_negative_volume())
            continue;
        indexed_triangle_set its = v->mesh().its;
        if (its.indices.empty())
            continue;
        MeshSlicingParamsEx p(params);
        p.trafo = p.trafo * inst * v->get_matrix();
        if (p.trafo.rotation().determinant() < 0.)
            its_flip_triangles(its);
        const ExPolygons layer = slice_mesh_ex(its, slice_height, p).front();
        if (v->is_model_part())
            part_ex = union_ex(part_ex, layer);
        else
            negative_ex = union_ex(negative_ex, layer);
    }
    return diff_ex(part_ex, negative_ex);
}

Points brim_ears_corner_points(const Polygon& polygon, float detection_length_mm, float max_angle_deg, bool is_outer)
{
    // Upstream's detection (#17163), which the tool's Auto-generate uses: independent of where the outline starts.
    return make_brim_ear_points(polygon, scale_(detection_length_mm), max_angle_deg, is_outer);
}

Points brim_ears_auto_points(const ExPolygons& first_layer, float detection_length_mm, float max_angle_deg)
{
    Points out;
    for (const ExPolygon& ex : first_layer) {
        append(out, brim_ears_corner_points(ex.contour, detection_length_mm, max_angle_deg, true));
        Polygons holes = ex.holes;
        polygons_reverse(holes);
        for (const Polygon& h : holes)
            append(out, brim_ears_corner_points(h, detection_length_mm, max_angle_deg, false));
    }
    return out;
}

} // namespace Slic3r
