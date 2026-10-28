#include <limits>
#include <cctype>
#include <boost/log/trivial.hpp>
#include <Eigen/Dense>

#include <tbb/parallel_for.h>

#include "ClipperUtils.hpp"
#include "ElephantFootCompensation.hpp"
#include "Exception.hpp"
#include "Geometry.hpp"
#include "I18N.hpp"
#include "Layer.hpp"
#include "MultiMaterialSegmentation.hpp"
#include "Print.hpp"
#include "CurvedPlaneProgram.hpp"
#include "CurvedMesh.hpp"
#include "CurvedRegions.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "CurvedFixedNozzle.hpp"
#include "AutomaticCurvedSelection.hpp"
#include "UndersideAnalysis.hpp"
#include "CurvedUnderside.hpp"
#include "CurvedJointPlanning.hpp"
#include "GCodeWriter.hpp"
#include "AutoPlanes.hpp"
//BBS
#include "ShortestPath.hpp"
#include "Support/SegmentSupportBounds.hpp"
#include "libslic3r/Feature/Interlocking/InterlockingGenerator.hpp"

//! macro used to mark string used at localization, return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

bool PrintObject::clip_multipart_objects = true;
bool PrintObject::infill_only_where_needed = false;

static coordf_t compute_slice_z(PrintObject* print_object, size_t i_layer, coordf_t lo, coordf_t hi)
{
    bool zaa_active   = false;
    coordf_t z_offset = 0.0;

    size_t num_regions = print_object->num_printing_regions();
    for (size_t rid = 0; rid < num_regions; ++rid) {
        const auto& rcfg = print_object->printing_region(rid).config();
        if (rcfg.zaa_enabled) {
            if (!zaa_active || rcfg.zaa_min_z < z_offset)
                z_offset = rcfg.zaa_min_z;
            zaa_active = true;
        }
    }

    if (!zaa_active || i_layer == 0) {
        return 0.5 * (lo + hi);
    }

    coordf_t slice_z = lo + z_offset;
    if ((slice_z < lo && !is_approx(slice_z, lo)) || (slice_z > hi && !is_approx(slice_z, hi))) {
        throw RuntimeError("Bad min Z value");
    }
    return slice_z;
}

LayerPtrs new_layers(
    PrintObject                 *print_object,
    // Object layers (pairs of bottom/top Z coordinate), without the raft.
    const std::vector<coordf_t> &object_layers)
{
    LayerPtrs out;
    out.reserve(object_layers.size());
    auto     id   = int(print_object->slicing_parameters().raft_layers());
    coordf_t zmin = print_object->slicing_parameters().object_print_z_min;
    Layer   *prev = nullptr;
    for (size_t i_layer = 0; i_layer < object_layers.size(); i_layer += 2) {
        coordf_t lo = object_layers[i_layer];
        coordf_t hi = object_layers[i_layer + 1];
        coordf_t slice_z = compute_slice_z(print_object, i_layer, lo, hi);

        Layer *layer = new Layer(id ++, print_object, hi - lo, hi + zmin, slice_z);
        out.emplace_back(layer);
        if (prev != nullptr) {
            prev->upper_layer = layer;
            layer->lower_layer = prev;
        }
        prev = layer;
    }
    return out;
}

// Slice single triangle mesh.
static std::vector<ExPolygons> slice_volume(
    const ModelVolume             &volume,
    const std::vector<float>      &zs,
    const MeshSlicingParamsEx     &params,
    const std::function<void()>   &throw_on_cancel_callback,
    const CurvedTransition::Field *curved_field = nullptr,
    const indexed_triangle_set   *curved_mesh = nullptr)
{
    std::vector<ExPolygons> layers;
    if (! zs.empty()) {
        indexed_triangle_set its = volume.mesh().its;
        if (its.indices.size() > 0) {
            MeshSlicingParamsEx params2 { params };
            params2.trafo = params2.trafo * volume.get_matrix();
            if (params2.trafo.rotation().determinant() < 0.)
                its_flip_triangles(its);
            if (curved_mesh != nullptr) {
                its=*curved_mesh;
                params2.trafo=Transform3d::Identity();
            } else if (curved_field != nullptr) {
                // The certified rectangular ramp flattens EXACTLY to a cuboid.
                // Transform a private copy; the user's model and Prepare stay intact.
                for (auto& v : its.vertices) {
                    Vec3d p = params2.trafo * v.cast<double>();
                    p.z() = curved_field->slice_z(p.x(), p.y(), p.z());
                    v = p.cast<float>();
                }
                params2.trafo = Transform3d::Identity();
            }
            layers = slice_mesh_ex(its, zs, params2, throw_on_cancel_callback);
            throw_on_cancel_callback();
        }
    }
    return layers;
}

// Slice single triangle mesh.
// Filter the zs not inside the ranges. The ranges are closed at the bottom and open at the top, they are sorted lexicographically and non overlapping.
static std::vector<ExPolygons> slice_volume(
    const ModelVolume                           &volume,
    const std::vector<float>                    &z,
    const std::vector<t_layer_height_range>     &ranges,
    const MeshSlicingParamsEx                   &params,
    const std::function<void()>                 &throw_on_cancel_callback)
{
    std::vector<ExPolygons> out;
    if (! z.empty() && ! ranges.empty()) {
        if (ranges.size() == 1 && z.front() >= ranges.front().first && z.back() < ranges.front().second) {
            // All layers fit into a single range.
            out = slice_volume(volume, z, params, throw_on_cancel_callback);
        } else {
            std::vector<float>                     z_filtered;
            std::vector<std::pair<size_t, size_t>> n_filtered;
            z_filtered.reserve(z.size());
            n_filtered.reserve(2 * ranges.size());
            size_t i = 0;
            for (const t_layer_height_range &range : ranges) {
                for (; i < z.size() && z[i] < range.first; ++ i) ;
                size_t first = i;
                for (; i < z.size() && z[i] < range.second; ++ i)
                    z_filtered.emplace_back(z[i]);
                if (i > first)
                    n_filtered.emplace_back(std::make_pair(first, i));
            }
            if (! n_filtered.empty()) {
                std::vector<ExPolygons> layers = slice_volume(volume, z_filtered, params, throw_on_cancel_callback);
                out.assign(z.size(), ExPolygons());
                i = 0;
                for (const std::pair<size_t, size_t> &span : n_filtered)
                    for (size_t j = span.first; j < span.second; ++ j)
                        out[j] = std::move(layers[i ++]);
            }
        }
    }
    return out;
}
static inline bool model_volume_needs_slicing(const ModelVolume &mv)
{
    ModelVolumeType type = mv.type();
    return type == ModelVolumeType::MODEL_PART || type == ModelVolumeType::NEGATIVE_VOLUME || type == ModelVolumeType::PARAMETER_MODIFIER;
}


//using Rotation3D = Eige
// Slice printable volumes, negative volumes and modifier volumes, sorted by ModelVolume::id().
// Apply closing radius.
// Apply positive XY compensation to ModelVolumeType::MODEL_PART and ModelVolumeType::PARAMETER_MODIFIER, not to ModelVolumeType::NEGATIVE_VOLUME.
// Apply contour simplification.
static std::vector<VolumeSlices> slice_volumes_inner(
    const PrintConfig                                        &print_config,
    const PrintObjectConfig                                  &print_object_config,
    const Transform3d                                        &object_trafo,
    ModelVolumePtrs                                           model_volumes,
    const std::vector<PrintObjectRegions::LayerRangeRegions> &layer_ranges,
    const std::vector<float>                                 &zs,
    const std::function<void()>                              &throw_on_cancel_callback,
    const CurvedTransition::Field                            *curved_field = nullptr,
    const indexed_triangle_set                               *curved_mesh = nullptr)
{


    model_volumes_sort_by_id(model_volumes); 

    
    //Transform3d        trans(rot);

    std::vector<VolumeSlices> out;
    out.reserve(model_volumes.size());

    std::vector<t_layer_height_range> slicing_ranges;
    if (layer_ranges.size() > 1)
        slicing_ranges.reserve(layer_ranges.size());

    MeshSlicingParamsEx params_base;
    //params_base.closing_radius = print_object_config.slice_closing_radius.value;
    //params_base.extra_offset   = 0;
    //// Apply a 3D rotation of 0.8 radians about the Y axis and multiply it
    //// with the object's transform so slicing uses the rotated transform.
    //{
    //    const double angle = -0.8; // radians
    //    Eigen::Matrix3d R;
    //    R << std::cos(angle), 0.0,  std::sin(angle),
    //         0.0,             1.0,  0.0,
    //        -std::sin(angle), 0.0,  std::cos(angle);

    //    Transform3d rot = Transform3d::Identity();
    //    rot.linear() = R;

    //    // Multiply rotation by object transform. This applies the Y-rotation
    //    // before the object's existing transform.
    //    params_base.trafo = rot * object_trafo;
    //    //this does not work since the 
    //}
    params_base.closing_radius = print_object_config.slice_closing_radius.value;
    params_base.extra_offset   = 0;
    // Apply a 3D rotation of 0.8 radians about the Y axis and multiply it
    // with the object's transform so slicing uses the rotated transform.
    // Rotate about the center of the plate (use the translation from object_trafo).
    {
        const double    angle = -0.8; // radians
        Eigen::Matrix3d R;
        R << std::cos(angle), 0.0, std::sin(angle), 0.0, 1.0, 0.0, -std::sin(angle), 0.0, std::cos(angle);

        // Rotation about origin
        Transform3d rot = Transform3d::Identity();
        rot.linear()    = R;

        // Compute plate center from the incoming transform and build
        // T(center) * R * T(-center) to rotate about that point.
        const Eigen::Vector3d center = object_trafo.translation();

        Transform3d trans_to_origin   = Transform3d::Identity();
        trans_to_origin.translation() = -center;

        Transform3d trans_back   = Transform3d::Identity();
        trans_back.translation() = center;

        Transform3d existing_rot = object_trafo;
        //rot.pretranslate(object_trafo.translation());
       
          

        Transform3d rot_about_plate_center = rot;//        *existing_rot;
        //params_base
            

            // Apply the rotation about plate center before the object's transform.
        params_base.trafo = object_trafo;//        *this->trafo(); // rot * object_trafo;
    }
    params_base.resolution = print_config.resolution <= 0.001 ? 0.0f : 0.0025;
    switch (print_object_config.slicing_mode.value) {
    case SlicingMode::Regular:    params_base.mode = MeshSlicingParams::SlicingMode::Regular; break;
    case SlicingMode::EvenOdd:    params_base.mode = MeshSlicingParams::SlicingMode::EvenOdd; break;
    case SlicingMode::CloseHoles: params_base.mode = MeshSlicingParams::SlicingMode::Positive; break;
    }

    params_base.mode_below     = params_base.mode;

    // BBS
    const size_t num_extruders = print_config.filament_diameter.size();
    const bool   is_mm_painted = num_extruders > 1 && std::any_of(model_volumes.cbegin(), model_volumes.cend(), [](const ModelVolume *mv) { return mv->is_mm_painted(); });
    // BBS: don't do size compensation when slice volume.
    // Will handle contour and hole size compensation seperately later.
    //const auto   extra_offset  = is_mm_painted ? 0.f : std::max(0.f, float(print_object_config.xy_contour_compensation.value));
    const auto   extra_offset = 0.f;

    for (const ModelVolume *model_volume : model_volumes)
        if (model_volume_needs_slicing(*model_volume)) {
            MeshSlicingParamsEx params { params_base };
            if (! model_volume->is_negative_volume())
                params.extra_offset = extra_offset;
            if (layer_ranges.size() == 1) {
                if (const PrintObjectRegions::LayerRangeRegions &layer_range = layer_ranges.front(); layer_range.has_volume(model_volume->id())) {
                    if (model_volume->is_model_part() && print_config.spiral_mode) {
                        auto it = std::find_if(layer_range.volume_regions.begin(), layer_range.volume_regions.end(),
                            [model_volume](const auto &slice){ return model_volume == slice.model_volume; });
                        params.mode = MeshSlicingParams::SlicingMode::PositiveLargestContour;
                        // Slice the bottom layers with SlicingMode::Regular.
                        // This needs to be in sync with LayerRegion::make_perimeters() spiral_mode!
                        const PrintRegionConfig &region_config = it->region->config();
                        params.slicing_mode_normal_below_layer = size_t(region_config.bottom_shell_layers.value);
                        for (; params.slicing_mode_normal_below_layer < zs.size() && zs[params.slicing_mode_normal_below_layer] < region_config.bottom_shell_thickness - EPSILON;
                            ++ params.slicing_mode_normal_below_layer);
                    }
                    out.push_back({
                        model_volume->id(),
                        slice_volume(*model_volume, zs, params, throw_on_cancel_callback, curved_field, curved_mesh)
                    });
                }
            } else {
                assert(! print_config.spiral_mode);
                slicing_ranges.clear();
                for (const PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges)
                    if (layer_range.has_volume(model_volume->id()))
                        slicing_ranges.emplace_back(layer_range.layer_height_range);
                if (! slicing_ranges.empty())
                    out.push_back({
                        model_volume->id(),
                        slice_volume(*model_volume, zs, slicing_ranges, params, throw_on_cancel_callback)
                    });
            }
            if (! out.empty() && out.back().slices.empty())
                out.pop_back();
        }

    return out;
}

static inline VolumeSlices& volume_slices_find_by_id(std::vector<VolumeSlices> &volume_slices, const ObjectID id)
{
    auto it = lower_bound_by_predicate(volume_slices.begin(), volume_slices.end(), [id](const VolumeSlices &vs) { return vs.volume_id < id; });
    assert(it != volume_slices.end() && it->volume_id == id);
    return *it;
}

static inline bool overlap_in_xy(const PrintObjectRegions::BoundingBox &l, const PrintObjectRegions::BoundingBox &r)
{
    return ! (l.max().x() < r.min().x() || l.min().x() > r.max().x() ||
              l.max().y() < r.min().y() || l.min().y() > r.max().y());
}

static std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator layer_range_first(const std::vector<PrintObjectRegions::LayerRangeRegions> &layer_ranges, double z)
{
    auto  it = lower_bound_by_predicate(layer_ranges.begin(), layer_ranges.end(),
        [z](const PrintObjectRegions::LayerRangeRegions &lr) {
            return lr.layer_height_range.second < z && abs(lr.layer_height_range.second - z) > EPSILON;
        });
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z <= it->layer_height_range.second);
    if (z == it->layer_height_range.second)
        if (auto it_next = it; ++ it_next != layer_ranges.end() && it_next->layer_height_range.first == z)
            it = it_next;
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z <= it->layer_height_range.second);
    return it;
}

static std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator layer_range_next(
    const std::vector<PrintObjectRegions::LayerRangeRegions>            &layer_ranges,
    std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator   it,
    double                                                               z)
{
    for (; it->layer_height_range.second <= z + EPSILON; ++ it)
        assert(it != layer_ranges.end());
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z < it->layer_height_range.second);
    return it;
}

static std::vector<std::vector<ExPolygons>> slices_to_regions(
    const PrintConfig                                        &print_config,
    const PrintObject                                        &print_object,
    ModelVolumePtrs                                           model_volumes,
    const PrintObjectRegions                                 &print_object_regions,
    const std::vector<float>                                 &zs,
    std::vector<VolumeSlices>                               &&volume_slices,
    // If clipping is disabled, then ExPolygons produced by different volumes will never be merged, thus they will be allowed to overlap.
    // It is up to the model designer to handle these overlaps.
    const bool                                                clip_multipart_objects,
    const std::function<void()>                              &throw_on_cancel_callback)
{
    model_volumes_sort_by_id(model_volumes);

    // Declarative cut planes: which volume is present at a layer cannot be decided from
    // VolumeRegion::bbox for a segment. Those boxes are built by PrintApply's trafo_for_bbox, which
    // composes object and volume transforms and then DELETES the resulting XY translation, in a
    // frame captured once as PrintObjectRegions::trafo_bboxes -- while a segment's `zs` are in its
    // own frame, rotated onto its build normal and rebased to its own material. No single matrix
    // recovers the deleted translation, so mapping the box is approximate at best, and getting it
    // wrong is severe: judge a volume absent everywhere and the segment ends up with no layers at
    // all.
    //
    // The slices themselves are the ground truth and are already in hand, so ask them instead:
    // a volume contributes at a layer exactly when its slice there is non-empty. Exact, frame-free,
    // and it costs an empty() check. Ordinary objects keep the bbox fast path untouched.
    //
    // BELT_MOD: the same holds for ANY object sliced in a rotated frame -- a belt-tilted object, or a
    // multi-planar object with its own B/C rotation. The boxes are z-extents in the unrotated
    // instance frame, while `zs` run along the rotated build direction. Measured on a belt: a
    // 48 mm tall two-part Benchy tilted 45 degrees slices to 70 mm, yet every layer above 48 came
    // out empty, because "is this part present at z" was answered from its untilted height. The
    // XY-overlap shortcut is in the same frame and is skipped for the same reason.
    const bool segment_frame = print_object.is_segment()
        || bool(print_object.curved_field())
        || ! layer_rotation_trafo(print_config, print_object.config()).linear().isIdentity(1e-9);
    // Height ranges are authored against OBJECT height, but a segment's zs are rebased to its own
    // zero, so a lookup with the raw value asks about the wrong part of the model -- an upper
    // segment physically at 10..20mm would be read as 0..10mm and pick up another range's
    // temperatures, wall counts or modifier volumes. Shift back into object coordinates for range
    // lookups only; everything else here stays in the slice frame.
    const float range_offset = segment_frame ? float(print_object.segment_schedule_base()) : 0.f;
    auto volume_present_at = [&volume_slices, segment_frame](const PrintObjectRegions::VolumeRegion &region, size_t z_idx, float z) {
        if (! segment_frame)
            return region.bbox->min().z() <= z && region.bbox->max().z() >= z;
        const VolumeSlices &slices = volume_slices_find_by_id(volume_slices, region.model_volume->id());
        return z_idx < slices.slices.size() && ! slices.slices[z_idx].empty();
    };

    std::vector<std::vector<ExPolygons>> slices_by_region(print_object_regions.all_regions.size(), std::vector<ExPolygons>(zs.size(), ExPolygons()));

    // First shuffle slices into regions if there is no overlap with another region possible, collect zs of the complex cases.
    std::vector<std::pair<size_t, float>> zs_complex;
    {
        size_t z_idx = 0;
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
            for (; z_idx < zs.size() && zs[z_idx] + range_offset < layer_range.layer_height_range.first; ++ z_idx) ;
            if (layer_range.volume_regions.empty()) {
            } else if (layer_range.volume_regions.size() == 1) {
                const ModelVolume *model_volume = layer_range.volume_regions.front().model_volume;
                assert(model_volume != nullptr);
                if (model_volume->is_model_part()) {
                    VolumeSlices &slices_src = volume_slices_find_by_id(volume_slices, model_volume->id());
                    auto         &slices_dst = slices_by_region[layer_range.volume_regions.front().region->print_object_region_id()];
                    for (; z_idx < zs.size() && zs[z_idx] + range_offset < layer_range.layer_height_range.second; ++ z_idx)
                        slices_dst[z_idx] = std::move(slices_src.slices[z_idx]);
                }
            } else {
                zs_complex.reserve(zs.size());
                for (; z_idx < zs.size() && zs[z_idx] + range_offset < layer_range.layer_height_range.second; ++ z_idx) {
                    float z                          = zs[z_idx];
                    int   idx_first_printable_region = -1;
                    bool  complex                    = false;
                    std::vector<int> printable_region_ids;
                    for (int idx_region = 0; idx_region < int(layer_range.volume_regions.size()); ++ idx_region) {
                        const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_region];
                        if (volume_present_at(region, z_idx, z)) {
                            if (region.model_volume->is_model_part())
                                printable_region_ids.push_back(idx_region);

                            if (idx_first_printable_region == -1 && region.model_volume->is_model_part()) {
                                idx_first_printable_region = idx_region;
                            }
                            else if (idx_first_printable_region != -1) {
                                // Test for overlap with some other region.
                                for (int idx_region2 = idx_first_printable_region; idx_region2 < idx_region; ++ idx_region2) {
                                    const PrintObjectRegions::VolumeRegion &region2 = layer_range.volume_regions[idx_region2];
                                    // For a segment the XY-overlap shortcut is skipped too: those boxes
                                    // live in the same unrecoverable frame, and a false "no overlap"
                                    // takes the simple path, which assigns only the FIRST present
                                    // volume and silently drops the other. Two present volumes always
                                    // go down the complex path, which clips from the real slices.
                                    if (volume_present_at(region2, z_idx, z) &&
                                        (segment_frame || overlap_in_xy(*region.bbox, *region2.bbox))) {
                                        complex = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (complex)
                        zs_complex.push_back({ z_idx, z });
                    else if (idx_first_printable_region >= 0) {
                        for (int printable_region_id : printable_region_ids) {
                            const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[printable_region_id];
                            append(slices_by_region[region.region->print_object_region_id()][z_idx], std::move(volume_slices_find_by_id(volume_slices, region.model_volume->id()).slices[z_idx]));
                        }
                    }
                }
            }
            throw_on_cancel_callback();
        }
    }

    // Second perform region clipping and assignment in parallel.
    if (! zs_complex.empty()) {
        std::vector<std::vector<VolumeSlices*>> layer_ranges_regions_to_slices(print_object_regions.layer_ranges.size(), std::vector<VolumeSlices*>());
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
            std::vector<VolumeSlices*> &layer_range_regions_to_slices = layer_ranges_regions_to_slices[&layer_range - print_object_regions.layer_ranges.data()];
            layer_range_regions_to_slices.reserve(layer_range.volume_regions.size());
            for (const PrintObjectRegions::VolumeRegion &region : layer_range.volume_regions)
                layer_range_regions_to_slices.push_back(&volume_slices_find_by_id(volume_slices, region.model_volume->id()));
        }
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, zs_complex.size()),
            [&slices_by_region, &print_object_regions, &zs_complex, &layer_ranges_regions_to_slices, clip_multipart_objects, range_offset, segment_frame, &throw_on_cancel_callback]
                (const tbb::blocked_range<size_t> &range) {
                float z              = zs_complex[range.begin()].second;
                auto  it_layer_range = layer_range_first(print_object_regions.layer_ranges, z + range_offset);
                // Per volume_regions slices at this Z height.
                struct RegionSlice {
                    ExPolygons  expolygons;
                    // Identifier of this region in PrintObjectRegions::all_regions
                    int         region_id;
                    ObjectID    volume_id;
                    bool operator<(const RegionSlice &rhs) const {
                        bool this_empty = this->region_id < 0 || this->expolygons.empty();
                        bool rhs_empty  = rhs.region_id < 0 || rhs.expolygons.empty();
                        // Sort the empty items to the end of the list.
                        // Sort by region_id & volume_id lexicographically.
                        return ! this_empty && (rhs_empty || (this->region_id < rhs.region_id || (this->region_id == rhs.region_id && volume_id < rhs.volume_id)));
                    }
                };

                // BBS
                auto trim_overlap = [](ExPolygons& expolys_a, ExPolygons& expolys_b) {
                    ExPolygons trimming_a;
                    ExPolygons trimming_b;

                    for (ExPolygon& expoly_a : expolys_a) {
                        BoundingBox bbox_a = get_extents(expoly_a);
                        ExPolygons expolys_new;
                        for (ExPolygon& expoly_b : expolys_b) {
                            BoundingBox bbox_b = get_extents(expoly_b);
                            if (!bbox_a.overlap(bbox_b))
                                continue;

                            ExPolygons temp = intersection_ex(expoly_b, expoly_a, ApplySafetyOffset::Yes);
                            if (temp.empty())
                                continue;

                            if (expoly_a.contour.length() > expoly_b.contour.length())
                                trimming_a.insert(trimming_a.end(), temp.begin(), temp.end());
                            else
                                trimming_b.insert(trimming_b.end(), temp.begin(), temp.end());
                        }
                    }

                    expolys_a = diff_ex(expolys_a, trimming_a);
                    expolys_b = diff_ex(expolys_b, trimming_b);
                };

                std::vector<RegionSlice> temp_slices;
                for (size_t zs_complex_idx = range.begin(); zs_complex_idx < range.end(); ++ zs_complex_idx) {
                    auto [z_idx, z] = zs_complex[zs_complex_idx];
                    it_layer_range = layer_range_next(print_object_regions.layer_ranges, it_layer_range, z);
                    const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;
                    {
                        std::vector<VolumeSlices*> &layer_range_regions_to_slices = layer_ranges_regions_to_slices[it_layer_range - print_object_regions.layer_ranges.begin()];
                        // Per volume_regions slices at thiz Z height.
                        temp_slices.clear();
                        temp_slices.reserve(layer_range.volume_regions.size());
                        for (VolumeSlices* &slices : layer_range_regions_to_slices) {
                            const PrintObjectRegions::VolumeRegion &volume_region = layer_range.volume_regions[&slices - layer_range_regions_to_slices.data()];
                            temp_slices.push_back({ std::move(slices->slices[z_idx]), volume_region.region ? volume_region.region->print_object_region_id() : -1, volume_region.model_volume->id() });
                        }
                    }
                    for (int idx_region = 0; idx_region < int(layer_range.volume_regions.size()); ++ idx_region)
                        if (! temp_slices[idx_region].expolygons.empty()) {
                            const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_region];
                            if (region.model_volume->is_modifier()) {
                                assert(region.parent > -1);
                                bool next_region_same_modifier = idx_region + 1 < int(temp_slices.size()) && layer_range.volume_regions[idx_region + 1].model_volume == region.model_volume;
                                RegionSlice &parent_slice = temp_slices[region.parent];
                                RegionSlice &this_slice   = temp_slices[idx_region];
                                ExPolygons   source       = std::move(this_slice.expolygons);
                                if (parent_slice.expolygons.empty()) {
                                    this_slice  .expolygons.clear();
                                } else {
                                    this_slice  .expolygons = intersection_ex(parent_slice.expolygons, source);
                                    parent_slice.expolygons = diff_ex        (parent_slice.expolygons, source);
                                }
                                if (next_region_same_modifier)
                                    // To be used in the following iteration.
                                    temp_slices[idx_region + 1].expolygons = std::move(source);
                            } else if ((region.model_volume->is_model_part() && clip_multipart_objects) || region.model_volume->is_negative_volume()) {
                                // Clip every non-zero region preceding it.
                                for (int idx_region2 = 0; idx_region2 < idx_region; ++ idx_region2)
                                    if (! temp_slices[idx_region2].expolygons.empty()) {
                                        // Skip trim_overlap for now, because it slow down the performace so much for some special cases
#if 1
                                        if (const PrintObjectRegions::VolumeRegion& region2 = layer_range.volume_regions[idx_region2];
                                            !region2.model_volume->is_negative_volume() && (segment_frame || overlap_in_xy(*region.bbox, *region2.bbox)))
                                            temp_slices[idx_region2].expolygons = diff_ex(temp_slices[idx_region2].expolygons, temp_slices[idx_region].expolygons);
#else
                                        const PrintObjectRegions::VolumeRegion& region2 = layer_range.volume_regions[idx_region2];
                                        if (!region2.model_volume->is_negative_volume() && overlap_in_xy(*region.bbox, *region2.bbox))
                                            //BBS: handle negative_volume seperately, always minus the negative volume and don't need to trim overlap
                                            if (!region.model_volume->is_negative_volume())
                                                trim_overlap(temp_slices[idx_region2].expolygons, temp_slices[idx_region].expolygons);
                                            else
                                                temp_slices[idx_region2].expolygons = diff_ex(temp_slices[idx_region2].expolygons, temp_slices[idx_region].expolygons);
#endif
                                    }
                            }
                        }
                    // Sort by region_id, push empty slices to the end.
                    std::sort(temp_slices.begin(), temp_slices.end());
                    // Remove the empty slices.
                    temp_slices.erase(std::find_if(temp_slices.begin(), temp_slices.end(), [](const auto &slice) { return slice.region_id == -1 || slice.expolygons.empty(); }), temp_slices.end());
                    // Merge slices and store them to the output.
                    for (int i = 0; i < int(temp_slices.size());) {
                        // Find a range of temp_slices with the same region_id.
                        int j = i;
                        bool merged = false;
                        ExPolygons &expolygons = temp_slices[i].expolygons;
                        for (++ j; j < int(temp_slices.size()) && temp_slices[i].region_id == temp_slices[j].region_id; ++ j)
                            if (ExPolygons &expolygons2 = temp_slices[j].expolygons; ! expolygons2.empty()) {
                                if (expolygons.empty()) {
                                    expolygons = std::move(expolygons2);
                                } else {
                                    append(expolygons, std::move(expolygons2));
                                    merged = true;
                                }
                            }
                        // Don't unite the regions if ! clip_multipart_objects. In that case it is user's responsibility
                        // to handle region overlaps. Indeed, one may intentionally let the regions overlap to produce crossing perimeters
                        // for example.
                        if (merged && clip_multipart_objects)
                            expolygons = closing_ex(expolygons, float(scale_(EPSILON)));
                        slices_by_region[temp_slices[i].region_id][z_idx] = std::move(expolygons);
                        i = j;
                    }
                    throw_on_cancel_callback();
                }
            });
    }

    return slices_by_region;
}

//BBS: justify whether a volume is connected to another one
bool doesVolumeIntersect(VolumeSlices& vs1, VolumeSlices& vs2)
{
    if (vs1.volume_id == vs2.volume_id) return true;
    // two volumes in the same object should have same number of layers, otherwise the slicing is incorrect.
    if (vs1.slices.size() != vs2.slices.size()) return false;

    auto& vs1s = vs1.slices;
    auto& vs2s = vs2.slices;
    bool is_intersect = false;

    tbb::parallel_for(tbb::blocked_range<int>(0, vs1s.size()),
        [&vs1s, &vs2s, &is_intersect](const tbb::blocked_range<int>& range) {
            for (auto i = range.begin(); i != range.end(); ++i) {
                if (vs1s[i].empty()) continue;

                if (overlaps(vs1s[i], vs2s[i])) {
                    is_intersect = true;
                    break;
                }
                if (i + 1 != vs2s.size() && overlaps(vs1s[i], vs2s[i + 1])) {
                    is_intersect = true;
                    break;
                }
                if (i - 1 >= 0 && overlaps(vs1s[i], vs2s[i - 1])) {
                    is_intersect = true;
                    break;
                }
            }
        });
    return is_intersect;
}

//BBS: grouping the volumes of an object according to their connection relationship
bool groupingVolumes(std::vector<VolumeSlices> objSliceByVolume, std::vector<groupedVolumeSlices>& groups, double resolution, int firstLayerReplacedBy)
{
    std::vector<int> groupIndex(objSliceByVolume.size(), -1);
    double offsetValue = 0.05 / SCALING_FACTOR;

    std::vector<std::vector<int>> osvIndex;
    for (int i = 0; i != objSliceByVolume.size(); ++i) {
        for (int j = 0; j != objSliceByVolume[i].slices.size(); ++j) {
            osvIndex.push_back({ i,j });
        }
    }

    tbb::parallel_for(tbb::blocked_range<int>(0, osvIndex.size()),
        [&osvIndex, &objSliceByVolume, &resolution](const tbb::blocked_range<int>& range) {
            for (auto k = range.begin(); k != range.end(); ++k) {
                for (ExPolygon& poly_ex : objSliceByVolume[osvIndex[k][0]].slices[osvIndex[k][1]])
                    poly_ex.douglas_peucker(resolution);
            }
        });

    tbb::parallel_for(tbb::blocked_range<int>(0, osvIndex.size()),
        [&osvIndex, &objSliceByVolume,&offsetValue](const tbb::blocked_range<int>& range) {
            for (auto k = range.begin(); k != range.end(); ++k) {
                objSliceByVolume[osvIndex[k][0]].slices[osvIndex[k][1]] = offset_ex(objSliceByVolume[osvIndex[k][0]].slices[osvIndex[k][1]], offsetValue);
            }
        });

    for (int i = 0; i != objSliceByVolume.size(); ++i) {
        if (groupIndex[i] < 0) {
            groupIndex[i] = i;
        }
        for (int j = i + 1; j != objSliceByVolume.size(); ++j) {
            if (doesVolumeIntersect(objSliceByVolume[i], objSliceByVolume[j])) {
                if (groupIndex[j] < 0) groupIndex[j] = groupIndex[i];
                if (groupIndex[j] != groupIndex[i]) {
                    int retain = std::min(groupIndex[i], groupIndex[j]);
                    int cover = std::max(groupIndex[i], groupIndex[j]);
                    for (int k = 0; k != objSliceByVolume.size(); ++k) {
                        if (groupIndex[k] == cover) groupIndex[k] = retain;
                    }
                }
            }

        }
    }

    std::vector<int> groupVector{};
    for (int gi : groupIndex) {
        bool exist = false;
        for (int gv : groupVector) {
            if (gv == gi) {
                exist = true;
                break;
            }
        }
        if (!exist) groupVector.push_back(gi);
    }

    // group volumes and their slices according to the grouping Vector
    groups.clear();

    for (int gv : groupVector) {
        groupedVolumeSlices gvs;
        gvs.groupId = gv;
        for (int i = 0; i != objSliceByVolume.size(); ++i) {
            if (groupIndex[i] == gv) {
                gvs.volume_ids.push_back(objSliceByVolume[i].volume_id);
                append(gvs.slices, objSliceByVolume[i].slices[firstLayerReplacedBy]);
            }
        }

        // the slices of a group should be unioned
        gvs.slices = offset_ex(union_ex(gvs.slices), -offsetValue);
        for (ExPolygon& poly_ex : gvs.slices)
            poly_ex.douglas_peucker(resolution);

        groups.push_back(gvs);
    }
    return true;
}

//BBS: filter the members of "objSliceByVolume" such that only "model_part" are included
std::vector<VolumeSlices> findPartVolumes(const std::vector<VolumeSlices>& objSliceByVolume, ModelVolumePtrs model_volumes) {
    std::vector<VolumeSlices> outPut;
    for (const auto& vs : objSliceByVolume) {
        for (const auto& mv : model_volumes) {
            if (vs.volume_id == mv->id() && mv->is_model_part()) outPut.push_back(vs);
        }
    }
    return outPut;
}

void applyNegtiveVolumes(ModelVolumePtrs model_volumes, const std::vector<VolumeSlices>& objSliceByVolume, std::vector<groupedVolumeSlices>& groups, double resolution) {
    ExPolygons negTotal;
    for (const auto& vs : objSliceByVolume) {
        for (const auto& mv : model_volumes) {
            if (vs.volume_id == mv->id() && mv->is_negative_volume()) {
                if (vs.slices.size() > 0) {
                    append(negTotal, vs.slices.front());
                }
            }
        }
    }

    for (auto& g : groups) {
        g.slices = diff_ex(g.slices, negTotal);
        for (ExPolygon& poly_ex : g.slices)
            poly_ex.douglas_peucker(resolution);
    }
}

void reGroupingLayerPolygons(std::vector<groupedVolumeSlices>& gvss, ExPolygons &eps, double resolution)
{
    std::vector<int> epsIndex;
    epsIndex.resize(eps.size(), -1);

    auto gvssc = gvss;
    auto epsc = eps;

    for (ExPolygon& poly_ex : epsc)
        poly_ex.douglas_peucker(resolution);

    for (int i = 0; i != gvssc.size(); ++i) {
        for (ExPolygon& poly_ex : gvssc[i].slices)
            poly_ex.douglas_peucker(resolution);
    }

    tbb::parallel_for(tbb::blocked_range<int>(0, epsc.size()),
        [&epsc, &gvssc, &epsIndex](const tbb::blocked_range<int>& range) {
            for (auto ie = range.begin(); ie != range.end(); ++ie) {
                if (epsc[ie].area() <= 0)
                    continue;

                double minArea = epsc[ie].area();
                for (int iv = 0; iv != gvssc.size(); iv++) {
                    auto clipedExPolys = diff_ex(epsc[ie], gvssc[iv].slices);
                    double area = 0;
                    for (const auto& ce : clipedExPolys) {
                        area += ce.area();
                    }
                    if (area < minArea) {
                        minArea = area;
                        epsIndex[ie] = iv;
                    }
                }
            }
        });

    for (int iv = 0; iv != gvss.size(); iv++)
        gvss[iv].slices.clear();

    for (int ie = 0; ie != eps.size(); ie++) {
        if (epsIndex[ie] >= 0)
            gvss[epsIndex[ie]].slices.push_back(eps[ie]);
    }
}

/*
std::string fix_slicing_errors(PrintObject* object, LayerPtrs &layers, const std::function<void()> &throw_if_canceled, int &firstLayerReplacedBy)
{
    std::string error_msg;//BBS

    if (layers.size() == 0) return error_msg;

    // Collect layers with slicing errors.
    // These layers will be fixed in parallel.
    std::vector<size_t> buggy_layers;
    buggy_layers.reserve(layers.size());
    // BBS: get largest external perimenter width of all layers
    auto get_ext_peri_width = [](Layer* layer) {return layer->m_regions.empty() ? 0 : layer->m_regions[0]->flow(frExternalPerimeter).scaled_width(); };
    auto it = std::max_element(layers.begin(), layers.end(), [get_ext_peri_width](auto& a, auto& b) {return get_ext_peri_width(a) < get_ext_peri_width(b); });
    coord_t thresh = get_ext_peri_width(*it) * 0.5;// half of external perimeter width  // 0.5 * scale_(this->config().line_width);
    for (size_t idx_layer = 0; idx_layer < layers.size(); ++idx_layer) {
        // BBS: detect empty layers (layers with very small regions) and mark them as problematic, then these layers will copy the nearest good layer
        auto layer = layers[idx_layer];
        ExPolygons lslices;
        for (size_t region_id = 0; region_id < layer->m_regions.size(); ++region_id) {
            LayerRegion* layerm = layer->m_regions[region_id];
            for (auto& surface : layerm->slices.surfaces) {
                auto expoly = offset_ex(surface.expolygon, -thresh);
                lslices.insert(lslices.begin(), expoly.begin(), expoly.end());
            }
        }
        if (lslices.empty()) {
            layer->slicing_errors = true;
        }

        if (layers[idx_layer]->slicing_errors) {
            buggy_layers.push_back(idx_layer);
        }
        else
            break; // only detect empty layers near bed
    }

    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - fixing slicing errors in parallel - begin";
    std::atomic<bool> is_replaced = false;
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, buggy_layers.size()),
        [&layers, &throw_if_canceled, &buggy_layers, &is_replaced](const tbb::blocked_range<size_t>& range) {
            for (size_t buggy_layer_idx = range.begin(); buggy_layer_idx < range.end(); ++ buggy_layer_idx) {
                throw_if_canceled();
                size_t idx_layer = buggy_layers[buggy_layer_idx];
                // BBS: only replace empty layers lower than 1mm
                const coordf_t thresh_empty_layer_height = 1;
                Layer* layer = layers[idx_layer];
                if (layer->print_z>= thresh_empty_layer_height)
                    continue;
                assert(layer->slicing_errors);
                // Try to repair the layer surfaces by merging all contours and all holes from neighbor layers.
                // BOOST_LOG_TRIVIAL(trace) << "Attempting to repair layer" << idx_layer;
                for (size_t region_id = 0; region_id < layer->region_count(); ++ region_id) {
                    LayerRegion *layerm = layer->get_region(region_id);
                    // Find the first valid layer below / above the current layer.
                    const Surfaces *upper_surfaces = nullptr;
                    const Surfaces *lower_surfaces = nullptr;
                    //BBS: only repair empty layers lowers than 1mm
                    for (size_t j = idx_layer + 1; j < layers.size(); ++j) {
                        if (!layers[j]->slicing_errors) {
                            upper_surfaces = &layers[j]->regions()[region_id]->slices.surfaces;
                            break;
                        }
                        if (layers[j]->print_z >= thresh_empty_layer_height) break;
                    }
                    for (int j = int(idx_layer) - 1; j >= 0; --j) {
                        if (layers[j]->print_z >= thresh_empty_layer_height) continue;
                        if (!layers[j]->slicing_errors) {
                            lower_surfaces = &layers[j]->regions()[region_id]->slices.surfaces;
                            break;
                        }
                    }
                    // Collect outer contours and holes from the valid layers above & below.
                    ExPolygons expolys;
                    expolys.reserve(
                        ((upper_surfaces == nullptr) ? 0 : upper_surfaces->size()) +
                        ((lower_surfaces == nullptr) ? 0 : lower_surfaces->size()));
                    if (upper_surfaces)
                        for (const auto &surface : *upper_surfaces) {
                            expolys.emplace_back(surface.expolygon);
                        }
                    if (lower_surfaces)
                        for (const auto &surface : *lower_surfaces) {
                            expolys.emplace_back(surface.expolygon);
                        }
                    if (!expolys.empty()) {
                        //BBS
                        is_replaced = true;
                        layerm->slices.set(union_ex(expolys), stInternal);
                    }
                }
                // Update layer slices after repairing the single regions.
                layer->make_slices();
            }
        });
    throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - fixing slicing errors in parallel - end";

    if(is_replaced)
        error_msg = L("Empty layers around bottom are replaced by nearest normal layers.");

    // remove empty layers from bottom
    while (! layers.empty() && (layers.front()->lslices.empty() || layers.front()->empty())) {
        delete layers.front();
        layers.erase(layers.begin());
        layers.front()->lower_layer = nullptr;
        for (size_t i = 0; i < layers.size(); ++ i)
            layers[i]->set_id(layers[i]->id() - 1);
    }

    //BBS
    if(error_msg.empty() && !buggy_layers.empty())
        error_msg = L("The model has too many empty layers.");

    // BBS: first layer slices are sorted by volume group, if the first layer is empty and replaced by the 2nd layer
// the later will be stored in "object->firstLayerObjGroupsMod()"
    if (!buggy_layers.empty() && buggy_layers.front() == 0 && layers.size() > 1)
        firstLayerReplacedBy = 1;

    return error_msg;
}
*/

void groupingVolumesForBrim(PrintObject* object, LayerPtrs& layers, int firstLayerReplacedBy)
{
    // An object with no layers has no first layer to group: `layers.front()` below would be a
    // straight out-of-bounds read. Reachable for a cut-plane segment whose cropped layer schedule
    // ends up empty, which must surface as an empty segment downstream -- not as a segfault inside
    // brim preparation.
    if (layers.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "groupingVolumesForBrim: object"
                                   << (object->is_segment() ? " (cut-plane segment " + std::to_string(object->segment_index()) + ")" : "")
                                   << " has no layers; skipping first-layer volume grouping";
        return;
    }

    const auto           scaled_resolution = scaled<double>(object->print()->config().resolution.value);
    auto partsObjSliceByVolume = findPartVolumes(object->firstLayerObjSliceMod(), object->model_object()->volumes);
    groupingVolumes(partsObjSliceByVolume, object->firstLayerObjGroupsMod(), scaled_resolution, firstLayerReplacedBy);
    applyNegtiveVolumes(object->model_object()->volumes, object->firstLayerObjSliceMod(), object->firstLayerObjGroupsMod(), scaled_resolution);

    // BBS: the actual first layer slices stored in layers are re-sorted by volume group and will be used to generate brim
    reGroupingLayerPolygons(object->firstLayerObjGroupsMod(), layers.front()->lslices, scaled_resolution);
}

// Called by make_perimeters()
// 1) Decides Z positions of the layers,
// 2) Initializes layers and their regions
// 3) Slices the object meshes
// 4) Slices the modifier meshes and reclassifies the slices of the object meshes by the slices of the modifier meshes
// 5) Applies size compensation (offsets the slices in XY plane)
// 6) Replaces bad slices by the slices reconstructed from the upper/lower layer
// Resulting expolygons of layer regions are marked as Internal.
// Build this object's slice frame. Kept OUT of slice() deliberately: the frame must be valid
// whenever slicing parameters are prepared, including when posSlice is already marked done and
// slice() early-returns. Leaving it as a side effect of the slicing step means any path that
// completes posSlice without calling slice() sees an identity frame -- which would silently
// mis-place cascade geometry, support bounds and the G-code un-rotation with no error anywhere.
// Idempotent; called from slice() for every object and, for cut-plane segments only, from
// update_slicing_parameters() (ordinary objects skip that call: it costs a full mesh merge per
// Print::apply and nothing reads their frame before slice()).
// UNLAYERED_MOD -- an ordinary object (not a cut segment) sliced in a tilted frame on a flat bed: a
// whole-object layer rotation (layer_rotation_b/c) that is not about Z alone. Same test as
// support_floor_is_tilted(), minus the belt.
static bool tilted_on_flat_bed(const PrintObject &object)
{
    if (object.is_segment() || object.print()->config().belt_printer.value)
        return false;
    return (layer_rotation_trafo(object.print()->config(), object.config()).linear() * Vec3d::UnitZ()).z() < 1. - EPSILON;
}

// A tilted object (above) that reaches below the plate: its frame is rebased onto its own minimum and
// slice_volumes() clips it at the plate.
static bool tilted_and_sunk(const PrintObject &object)
{
    return tilted_on_flat_bed(object) && object.model_object()->bounding_box_exact().min.z() < - EPSILON;
}

void PrintObject::update_slice_frame()
{
    // Compute plate center from the incoming transform and build
    // T(center) * R * T(-center) to rotate about that point.
    // UNLAYERED_MOD -- THIS PrintObject's instance placement, not the ModelObject's bounding box:
    // that box spans every instance of the object, so with several copies on the plate the pivot
    // (and the z floor below) drifted by the spread between them and the un-rotated G-code of a
    // rotated object landed tens of millimetres off, partly below the bed. Measured on a belt with
    // three copies of a tee. Without instances (never during slicing) the old derivation stands.
    // For a single copy the two derivations agree to well under a micron; the ModelObject's box is
    // kept there so planar output stays byte-identical (the corpus check), the instance shift is
    // used only when the object has several copies.
    const bool several_copies = this->model_object()->instances.size() > 1;
    Eigen::Vector3d center;
    if (several_copies && ! this->instances().empty()) {
        // shift = instance XY offset + m_center_offset (set_instances), so this is the offset.
        center = Eigen::Vector3d(unscale<double>(this->instances().front().shift.x() - m_center_offset.x()),
                                 unscale<double>(this->instances().front().shift.y() - m_center_offset.y()), 0.);
    } else {
        center = this->model_object()->bounding_box_exact().center();
        //allows for corrections when coordiante frames get slightly out of sync
        center -= Eigen::Vector3d(unscale<double>(m_center_offset[0]), unscale<double>(m_center_offset[1]), 0);
        center[2] = 0;
    }

    Transform3d trans_to_origin   = Transform3d::Identity();
    trans_to_origin.translation() = center;

    Transform3d trans_back   = Transform3d::Identity();
    trans_back.translation() = -center;

    // Combined rotation. An ordinary object goes through the shared helper -- config B/C, gated on
    // the machine's non-planar master switch (layer_rotation_trafo() in PrintConfig.cpp). A
    // declarative cut-plane segment must NOT: it derives its B/C from the segment's build normal
    // rather than the per-object config, so it goes through effective_layer_rotation_bc(). Handing a
    // segment to the config-reading helper would slice every non-root segment unrotated.
    Transform3d tempTrafo = Transform3d::Identity();
    if (this->is_segment()) {
        double b_deg = 0., c_deg = 0.;
        this->effective_layer_rotation_bc(b_deg, c_deg);
        tempTrafo.linear() = Geometry::layer_rotation_matrix(b_deg, c_deg);
    } else {
        // BELT_MOD: on a belt printer this is the belt tilt. An object's own B/C rotation on top of
        // it is refused by Print::validate() (the belt schedule measures along the gantry only);
        // cut-plane segments take the is_segment() branch above.
        tempTrafo = layer_rotation_trafo(m_print->config(), m_config);
    }

    if (this->is_segment()) {
        // Place THIS SEGMENT's own minimum at slice z = 0, not the whole object's, so it slices
        // over its own range (no leading empty layers) and its floor sits where support
        // generation expects the ground to be.
        //
        // Exact, not empirical: R maps the segment's build normal onto +Z, so for any
        // object-local vertex v
        //     (R * m_trafo * v).z  ==  build_normal_world . (m_trafo * v)
        // because (R y).z == (R^T z_hat) . y == build_normal_world . y. That right-hand side is
        // precisely what CutSegmentDescriptor::build_z_min minimises over the segment's
        // materialized piece, so -build_z_min lands that minimum on zero.
        //
        // Deliberately NOT derived from the full-object bounding-box line below: that one measures
        // model_object()->mesh() (object-local, WITHOUT m_trafo) while post_trafo also carries
        // m_trafo, so the two are different frames. It is left untouched for ordinary objects,
        // whose behaviour must not change.
        //
        // UNLAYERED_MOD -- but do not rebase the ROOT segment UP out of the bed. The root is the only
        // segment that rests on the build plate, and it slices along +Z exactly, so its build_z_min is
        // a genuine bed height. When the object sits low, an unconditional -build_z_min lifted that
        // below-plate material into positive slice z, where it was sliced and printed: measured 2496
        // extruding moves down to -1.132 mm on a real project, all of them in this upright root chunk.
        // Clamping at 0 preserves the sinking instead, exactly as the ordinary-object branch below
        // does with std::min(box_min.z(), 0.0), so the layer generator -- which starts above z = 0 --
        // simply never slices what is under the plate. That is how stock copes with a model clipped
        // into the bed, and it leaves no empty leading layers for the preview to trip over.
        //
        // Non-root segments keep the unconditional rebase: they rest on their parent, their build_z_min
        // is measured along their own tilted build normal rather than bed height, and the rebase is what
        // keeps them from growing leading empty layers. One that also reaches the plate measures that
        // build_z_min over its material ABOVE the plate only, and slice_volumes() clips the rest away
        // (CutClipRole::Bed).
        tempTrafo.translation() = Eigen::Vector3d(0., 0., - m_segment->slice_z_base());
    } else {
        // UNLAYERED_MOD -- this instance's own extent (see the pivot above): bounding_box_exact()
        // and mesh() cover every instance of the object.
        Eigen::Vector3d box_min, new_box_min;
        if (several_copies && ! this->instances().empty()) {
            box_min     = this->model_object()->instance_bounding_box(*this->instances().front().model_instance).min;
            // R * m_trafo rotates about the instance's own origin: identical to rotating the placed
            // mesh about `center`, without touching the other instances.
            new_box_min = this->model_object()->raw_mesh().transformed_bounding_box(tempTrafo * m_trafo).min;
        } else {
            box_min     = this->model_object()->bounding_box_exact().min;
            //calculate the minimum of the print correctly!
            new_box_min = this->model_object()->mesh().transformed_bounding_box(trans_to_origin * tempTrafo * trans_back).min;
        }
        //offset so it sits flat on bed. Also subtract out any amount it sits under bed at the beginning
        // BELT_MOD: not on a belt. The planar rule leaves the sunk depth unsliced below z = 0, but in
        // the tilted frame that slab is a gantry-normal slice through above-belt material, and it
        // also lowered a sunk object's print_z against every other object on the belt. The belt
        // clip (bed_clip below) removes exactly the material under the belt instead.
        // UNLAYERED_MOD -- nor for a whole-object layer rotation (layer_rotation_b/c) on a flat bed, for
        // the same reason: the slab left unsliced is measured along the TILTED axis, so it kept part of
        // the material under the plate (walls printed to Z -8.3 on a sunk 45 degree benchy, the kind of
        // print that scraped a customer's A1 bed) and dropped material above it. Rebase onto the
        // object's own minimum; slice_volumes() clips what is under the plate.
        const double sink = m_print->config().belt_printer.value || tilted_and_sunk(*this) ? 0. : std::min(box_min.z(), 0.0);
        tempTrafo.translation() = Eigen::Vector3d(0, 0, sink - new_box_min.z());
        // BELT_MOD -- belt schedule: raise this object's frame so print_z is comparable across the
        // objects on the belt (Print::update_belt_schedule()). Undone exactly by post_trafo_inverse.
        tempTrafo.translation().z() += m_belt_schedule_offset;
    }

    m_post_trafo     = tempTrafo * m_trafo;
    m_post_trafo_inv = trans_to_origin * tempTrafo.inverse() * trans_back;

    // Authoritative pair: object-local -> the exact centered coordinates handed to
    // slice_volumes_inner(), and its true inverse. See PrintObject::object_to_slice().
    m_object_to_slice = m_post_trafo;
    m_object_to_slice.pretranslate(Vec3d(- unscale<double>(m_center_offset.x()),
                                         - unscale<double>(m_center_offset.y()), 0.));
    m_slice_to_object = m_object_to_slice.inverse();
    assert((m_object_to_slice * m_slice_to_object).isApprox(Transform3d::Identity(), 1e-9));
}

size_t PrintObject::curved_nozzle_index() const
{
    const auto& pc=m_print->config();
    const auto used=m_print->extruders();
    if(used.size()!=1) throw std::invalid_argument("Curved printing requires one used filament and one fixed active nozzle.");
    if(pc.nozzle_diameter.size()==1) return 0;
    // Reuse the existing BBL filament/nozzle protocol. Generic firmware tool
    // routing, independent heads and clustered/toolchanger nozzles are outside
    // this fixed dual-carriage integration.
    if(pc.nozzle_diameter.size()!=2 || !m_print->is_BBL_printer() ||
       pc.nonplanar_fixed_nozzle.value<1 || pc.nonplanar_fixed_nozzle.value>2)
        throw std::invalid_argument("Curved printing on this dual-nozzle printer needs the fixed-nozzle BBL mode; keep the real two-nozzle profile.");
    if(pc.filament_map_mode.value<fmmManual || m_print->is_dynamic_group_reorder() ||
       m_print->full_print_config().opt_bool("enable_filament_dynamic_map") || pc.manual_filament_change.value)
        throw std::invalid_argument("Fixed-nozzle curved printing requires manual filament mapping and physical nozzle selection, with automatic reassignment off.");
    if(std::any_of(pc.extruder_max_nozzle_count.values.begin(),pc.extruder_max_nozzle_count.values.end(),[](int n){return n>1;}))
        throw std::invalid_argument("Fixed-nozzle curved printing does not support nozzle clusters or toolchangers.");
    const auto map=m_print->get_filament_maps();
    if(used.front()>=map.size() || map[used.front()]!=pc.nonplanar_fixed_nozzle.value)
        throw std::invalid_argument("The used filament must be manually mapped to the selected fixed nozzle.");
    return size_t(pc.nonplanar_fixed_nozzle.value-1);
}

std::string PrintObject::curved_blend_error(bool automatic) const
{
    if (!automatic && !m_config.curved_layer_blend.value) return {};
    const auto& pc = m_print->config();
    if (!pc.enable_nonplanar.value || !pc.virtual_5axis.value || pc.belt_printer.value)
        return "Curved blend requires nonplanar printing enabled and a Virtual printer with belt printing off.";
    if (is_segment() ||
        m_model_object->volumes.size()!=1 || !m_model_object->volumes.front()->is_model_part() ||
        m_model_object->has_custom_layering() || m_model_object->volumes.front()->is_mm_painted() || object_has_directed_parts(*m_model_object))
        return "Each curved object requires one closed mesh volume, without modifiers, painting, per-part build directions or variable layer heights.";
    size_t active=0;
    try {active=curved_nozzle_index();(void)CurvedTransition::FixedNozzleLimits::from_config(pc,active);}
    catch(const std::invalid_argument& e) {return e.what();}
    if ((!automatic && (has_support_material() || m_config.enforce_support_layers.value>0)) || m_config.raft_layers.value>0 ||
        std::abs(m_config.layer_rotation_b.value)>EPSILON || std::abs(m_config.layer_rotation_c.value)>EPSILON ||
        m_config.slicing_mode.value!=SlicingMode::Regular)
        return "Curved blend requires supports and raft off, normal slicing, no make-overhang-printable and layer rotations B/C at zero.";
    if (std::abs(m_config.xy_contour_compensation.value)>EPSILON || std::abs(m_config.xy_hole_compensation.value)>EPSILON ||
        std::abs(m_config.elefant_foot_compensation.value)>EPSILON)
        return "Curved blend currently requires zero XY contour/hole and elephant-foot compensation.";
    if (pc.skirt_height.value>1 || pc.draft_shield.value!=dsDisabled || pc.machine_max_speed_z.get_at(0)<=0.)
        return "Curved blend requires skirt height at most one layer, no draft shield and a positive configured machine Z speed.";
    if (std::abs(pc.z_offset.value)>EPSILON || !m_print->shrinkage_compensation().isApprox(Vec3d::Ones(),1e-8))
        return "Curved blend currently requires zero Z offset and 100% XYZ shrinkage compensation.";
    if (pc.spiral_mode.value || pc.enable_arc_fitting.value || pc.enable_prime_tower.value ||
        pc.enable_mixed_color_sublayer.value || pc.timelapse_type.value!=tlTraditional)
        return "Curved blend requires vase, arc fitting, prime tower, mixed sublayers and timelapse motion off.";
    if (std::any_of(pc.wipe.values.begin(),pc.wipe.values.end(),[](bool v){return v;}))
        return "Curved blend currently requires Wipe while retracting off; ordinary retraction remains available.";
    if (std::any_of(pc.adaptive_pressure_advance.values.begin(),pc.adaptive_pressure_advance.values.end(),[](bool v){return v;}))
        return "Curved blend currently requires adaptive pressure advance off; its model does not account for varying physical bead area.";
    if (pc.gcode_remap_x.value!=RemapAxis::PosX || pc.gcode_remap_y.value!=RemapAxis::PosY || pc.gcode_remap_z.value!=RemapAxis::PosZ)
        return "Curved blend currently requires ordinary XYZ axis mapping.";
    if (!m_post_trafo_inv.linear().isApprox(Matrix3d::Identity(),1e-8) || std::abs(m_post_trafo_inv.translation().z())>1e-4)
        return "Curved blend requires the model's flat bottom on the bed, without a tilted build frame.";
    // Expanded layer scripts are checked in GCode before emission. Only fan,
    // progress and G92 E0 commands are allowed; raw axis/tool changes fail export.
    for(const auto* script:{&pc.layer_change_gcode,&pc.before_layer_change_gcode})
        if(script->value.find('{')==std::string::npos && !CurvedTransition::layer_code_safe(script->value))
            return "Curved blend layer code permits only cooling/progress and G92 E0; axis, frame and tool changes are forbidden.";
    if (!pc.time_lapse_gcode.value.empty() || pc.farthest_point_timelapse.value || !m_print->model().get_curr_plate_custom_gcodes().gcodes.empty())
        return "Curved blend currently requires timelapse and model height-change custom G-code off.";
    if (pc.nozzle_diameter.size()==1 && std::any_of(pc.extruder_offset.values.begin(),pc.extruder_offset.values.end(),[](const Vec2d& p){return !p.isZero(1e-8);}))
        return "Curved blend currently requires zero nozzle XY offset.";
    if (std::any_of(pc.slicing_pipeline_plugin.values.begin(),pc.slicing_pipeline_plugin.values.end(),[](const std::string& s){return !s.empty();}) ||
        std::any_of(pc.post_process.values.begin(),pc.post_process.values.end(),[](const std::string& s){return !s.empty();}))
        return "Curved blend currently requires slicing-pipeline and external G-code post-processing scripts off.";
    for (size_t i=0;i<num_printing_regions();++i) {
        const auto& rc=printing_region(i).config();
        if (rc.infill_combination.value ||
            m_config.wall_generator.value!=PerimeterGeneratorType::Classic || rc.seam_slope_type.value!=SeamScarfType::None ||
            rc.wipe_on_loops.value || rc.wipe_before_external_loop.value ||
            rc.ironing_type.value!=IroningType::NoIroning ||
            (rc.fuzzy_skin.value!=FuzzySkinType::None && rc.fuzzy_skin.value!=FuzzySkinType::Disabled_fuzzy) ||
            rc.zaa_enabled.value || rc.wave_overhangs.value || rc.make_overhang_printable.value)
            return "Curved blend currently requires classic walls, no combined infill, surface wipe, scarf, ironing, fuzzy skin, Z anti-aliasing or wave overhangs. Bridge/support beads must stay in planar zones.";
    }
    return {};
}

static void validate_underside_deposition(const PrintObject& object,const std::function<void()>& cancel)
{
    const auto& field=*object.curved_field();const auto& domain=*object.curved_domain();
    const auto limits=CurvedTransition::FixedNozzleLimits::from_config(object.print()->config(),object.curved_nozzle_index());
    struct Beads {double z;Polygons footprint;};
    std::vector<Beads> deposited;
    std::function<void(const ExtrusionEntity&,const std::function<void(const ExtrusionPath&)>&)> paths;
    paths=[&](const ExtrusionEntity& e,const auto& f) {
        if(const auto* p=dynamic_cast<const ExtrusionPath*>(&e)) f(*p);
        else if(const auto* c=dynamic_cast<const ExtrusionEntityCollection*>(&e)) for(const auto* child:c->entities) paths(*child,f);
        else if(const auto* l=dynamic_cast<const ExtrusionLoop*>(&e)) for(const auto& p:l->paths) f(p);
        else if(const auto* m=dynamic_cast<const ExtrusionMultiPath*>(&e)) for(const auto& p:m->paths) f(p);
    };
    const auto covered=[](const ExtrusionEntity& e) {Polygons p;e.polygons_covered_by_width(p,float(scale_(.01)));return p;};
    for(const auto* layer:object.layers()) {
        Polygons p;
        for(const auto* r:layer->regions()) {append(p,covered(r->perimeters));append(p,covered(r->fills));}
        if(!p.empty()) deposited.push_back({layer->print_z,union_(p)});
    }
    for(const auto* layer:object.support_layers()) {
        auto p=covered(layer->support_fills);
        if(!p.empty()) deposited.push_back({layer->print_z,union_(p)});
    }
    const auto anchors=[&](double w,double height,double gap) {
        Polygons p;
        // Use already lower logical layers only. The monotone mapping preserves
        // this column order; a support gap remains the owner's configured gap.
        for(const auto& b:deposited) if(b.z<w-1e-5 && b.z>=w-height-gap-.05) append(p,b.footprint);
        return union_(p);
    };
    const auto check=[&](const ExtrusionPath& path,double w,const Polygons& prior,const Polygons& perimeter_anchors) {
        cancel();
        const auto xy=path.polyline.to_polyline();if(xy.points.size()<2) return;
        const auto bb=get_extents(xy).inflated(scale_(path.width*.5+.02));
        const Vec2d lo=unscale(bb.min),hi=unscale(bb.max);
        if(lo.x()<domain.xmin-1e-4 || hi.x()>domain.xmax+1e-4 || lo.y()<domain.ymin-1e-4 || hi.y()>domain.ymax+1e-4)
            throw SlicingError("Underside support paths exceed the bounded field domain; keep planar or reduce support expansion.");
        if(is_bridge(path.role()) && !field.affine_on_box(lo.x(),hi.x(),lo.y(),hi.y(),w-path.height,w))
            throw SlicingError("An underside bridge crosses a changing layer direction; no curved air bridge will be exported.");
        if(is_support(path.role())) {
            const auto spacing=field.spacing_bounds(path.height);
            if(spacing.max_vertical_mm>limits.max_layer+1e-4 || spacing.min_graph_distance_mm<limits.min_layer-1e-4)
                throw SlicingError("Mapped underside support spacing exceeds the configured nozzle layer-height limit.");
            if(w-path.height>1e-4) {
                for(const auto& island:union_ex(covered(path)))
                    if(intersection(to_polygons(island),prior).empty())
                        throw SlicingError("A generated underside support starts without a lower deposition foundation.");
            }
        } else if(is_bridge(path.role())) {
            const double contact_gap=object.has_support_material()?
                std::max(0.,object.config().support_top_z_distance.value):0.;
            Polygons anchor=anchors(w,path.height,contact_gap);
            append(anchor,perimeter_anchors);anchor=union_(anchor);
            if(path.role()==erOverhangPerimeter) {
                if(intersection(covered(path),anchor).empty())
                    throw SlicingError("An underside overhang perimeter has no deposited anchor.");
            } else {
                const coord_t r=scale_(path.width*.5+.02);
                for(const Point& point:{xy.points.front(),xy.points.back()}) {
                    Polygon end(Points{{point.x()-r,point.y()-r},{point.x()+r,point.y()-r},
                        {point.x()+r,point.y()+r},{point.x()-r,point.y()+r}});
                    if(intersection(Polygons{end},anchor).empty())
                        throw SlicingError("An underside bridge is missing a deposited end anchor.");
                }
            }
        }
    };
    for(const auto* layer:object.layers()) {
        const auto prior=anchors(layer->print_z,limits.max_layer,
            std::max(0.,object.config().support_top_z_distance.value));
        Polygons perimeter_anchors;
        for(const auto* r:layer->regions()) if(!r->region().config().is_infill_first.value)
            paths(r->perimeters,[&](const ExtrusionPath& p){if(!is_bridge(p.role())) append(perimeter_anchors,covered(p));});
        perimeter_anchors=union_(perimeter_anchors);
        for(const auto* r:layer->regions()) {
            paths(r->perimeters,[&](const ExtrusionPath& p){check(p,layer->print_z,prior,{});});
            paths(r->fills,[&](const ExtrusionPath& p){check(p,layer->print_z,prior,perimeter_anchors);});
        }
    }
    for(const auto* layer:object.support_layers()) {
        std::map<double,Polygons> by_height;
        paths(layer->support_fills,[&](const ExtrusionPath& p) {
            auto [it,inserted]=by_height.try_emplace(p.height);
            if(inserted) it->second=anchors(layer->print_z,p.height,std::max(0.,object.config().support_bottom_z_distance.value));
            check(p,layer->print_z,it->second,{});
        });
    }
}

bool PrintObject::refine_curved_regions_for_paths()
{
    if(!m_config.automatic_layer_strategy.value) return false;
    using namespace CurvedTransition;
    const auto regions=roof_part(m_curved_field);
    std::vector<std::array<double,6>> boxes;
    const auto path_box=[&](const ExtrusionPath& path,double z) {
        if(!is_bridge(path.role()) && !is_support(path.role())) return;
        const auto bb=get_extents(path.polyline.to_polyline()).inflated(scale_(path.width*.5+.02));
        const Vec2d lo=unscale(bb.min),hi=unscale(bb.max);
        const double a=regions->max_displacement_mm();
        if(!regions->identity_on_box(lo.x(),hi.x(),lo.y(),hi.y(),z-path.height-a,z+a))
            boxes.push_back({lo.x(),hi.x(),lo.y(),hi.y(),z-path.height-a,z+a});
    };
    std::function<void(const ExtrusionEntity&,double)> visit;
    visit=[&](const ExtrusionEntity& e,double z) {
        if(const auto* p=dynamic_cast<const ExtrusionPath*>(&e)) path_box(*p,z);
        else if(const auto* c=dynamic_cast<const ExtrusionEntityCollection*>(&e)) for(const auto* child:c->entities) visit(*child,z);
        else if(const auto* l=dynamic_cast<const ExtrusionLoop*>(&e)) for(const auto& p:l->paths) path_box(p,z);
        else if(const auto* m=dynamic_cast<const ExtrusionMultiPath*>(&e)) for(const auto& p:m->paths) path_box(p,z);
    };
    if(regions) {
      for(const auto* layer:m_layers) for(const auto* r:layer->regions()) {visit(r->perimeters,layer->print_z);visit(r->fills,layer->print_z);}
      for(const auto* layer:m_support_layers) visit(layer->support_fills,layer->print_z);
    }
    for(const auto& p:regions?regions->patches():std::vector<Regions::Patch>{}) for(const auto& b:boxes) {
        const double dx=std::max({b[0]-p.x,0.,p.x-b[1]}),dy=std::max({b[2]-p.y,0.,p.y-b[3]});
        if(b[5]>p.lo && dx*dx+dy*dy<p.radius*p.radius) {
            const Vec2d centre(p.x,p.y);
            if(std::find(m_curved_rejected_centres.begin(),m_curved_rejected_centres.end(),centre)==m_curved_rejected_centres.end())
                m_curved_rejected_centres.push_back(centre);
            break;
        }
    }
    const auto retry=[&] {m_curved_planning_retry=true;this->invalidate_step(posSlice);return true;};
    if(!boxes.empty()) {
        if(m_curved_plan && m_curved_plan->final) throw SlicingError("A selected roof now intersects generated bridge/support beads");
        if(m_curved_plan) {
            auto& trial=m_curved_plan->trials.at(m_curved_plan->current);
            std::vector<std::array<double,2>> rejected;
            for(const auto& xy:m_curved_rejected_centres) {
                rejected.push_back({xy.x(),xy.y()});
                for(const auto& p:regions->patches()) if(p.x==xy.x() && p.y==xy.y())
                    trial.rejected.push_back({0,0,false,p.x,p.y,p.hi,p.radius,
                        std::atan(std::hypot(p.gx,p.gy))*180./PI,0.,0.,0.,"Planar contact zone: generated bridge/support bead overlaps this roof"});
            }
            trial.field=without_roof_centres(m_curved_field,trial.domain,rejected,m_curved_plan->allowed);
        }
        return retry();
    }
    std::string invalid;
    try {if(m_curved_field && m_curved_field->maps_supports())
        validate_underside_deposition(*this,[this]{m_print->throw_if_canceled();});}
    catch(const SlicingError& e) {if(!m_curved_plan || m_curved_plan->final) throw;invalid=e.what();}
    if(!m_curved_plan || m_curved_plan->trials.empty()) return false;
    auto& plan=*m_curved_plan;
    const auto publish=[&] {
        m_layer_region_records.clear();plan.decisions.clear();
        std::ostringstream report;report.imbue(std::locale::classic());report<<std::fixed<<std::setprecision(2);
        report<<" Generated benefit/cost choice: "<<plan.trials[plan.chosen].name<<". ";
        for(size_t k=0;k<plan.trials.size();++k) {
            const auto& t=plan.trials[k];
            report<<t.name<<": ";
            if(!t.valid) report<<"rejected ("<<t.reason<<"); ";
            else {
                const auto b=planning_breakdown(plan.trials.back().surface,t.surface,plan.trials.back().cost,t.cost);
                report<<"score "<<t.score<<", support "<<t.cost.support_mm3<<" mm3, motion "<<t.cost.motion_seconds<<" s"
                      <<", roof gain "<<100.*b.roof<<"%, underside gain "<<100.*b.under
                      <<"%, time change "<<b.time_seconds<<" s ("<<100.*b.time<<"%); ";
            }
            std::vector<RegionDecision> records;
            if(const auto roof=roof_part(t.field)) for(const auto& p:roof->patches()) {
                double z=p.hi;
                if(auto joint=std::dynamic_pointer_cast<const JointField>(t.field)) z=joint->underside()->world_z(p.x,p.y,z);
                records.push_back({0,0,k==plan.chosen,p.x,p.y,z,p.radius,std::atan(std::hypot(p.gx,p.gy))*180./PI,
                    t.valid?t.score:0.,t.cost.motion_seconds,t.cost.support_mm3,{}});
            }
            // A joint choice has TWO region kinds; its roof patches must not
            // hide the independently chosen underside field in Preview.
            const auto joint=std::dynamic_pointer_cast<const JointField>(t.field);
            if(t.field && (!roof_part(t.field) || joint)) records.push_back({0,t.field->maps_supports()?1:0,k==plan.chosen,
                (t.domain.xmin+t.domain.xmax)*.5,(t.domain.ymin+t.domain.ymax)*.5,t.domain.top_max,
                .5*std::hypot(t.domain.xmax-t.domain.xmin,t.domain.ymax-t.domain.ymin),
                std::atan(t.field->slope_bound())*180./PI,t.valid?t.score:0.,t.cost.motion_seconds,t.cost.support_mm3,{}});
            if(!t.field) records.push_back({0,2,k==plan.chosen,(t.domain.xmin+t.domain.xmax)*.5,
                (t.domain.ymin+t.domain.ymax)*.5,t.domain.top_max,
                .5*std::hypot(t.domain.xmax-t.domain.xmin,t.domain.ymax-t.domain.ymin),0.,t.valid?t.score:0.,
                t.cost.motion_seconds,t.cost.support_mm3,{}});
            for(auto& r:records) {
                r.reason=t.name+": "+(k==plan.chosen?
                    (t.field?"best net roof/underside candidate after estimated motion and material cost":"no valid curved candidate had a positive net score"):
                    "candidate not selected");
                if(k!=plan.chosen) r.rejection_reason=!t.valid?t.reason:
                    "Lower net score than "+plan.trials[plan.chosen].name+" after surface gain, time and material costs.";
                if(t.valid) {
                    const auto b=planning_breakdown(plan.trials.back().surface,t.surface,plan.trials.back().cost,t.cost);
                    r.has_breakdown=true;r.roof_gain_percent=100.*b.roof;r.underside_gain_percent=100.*b.under;
                    r.time_delta_seconds=b.time_seconds;r.time_delta_percent=100.*b.time;
                    r.support_saved_mm3=b.support_mm3;r.model_added_mm3=b.material_mm3;
                    r.travel_seconds=t.cost.travel_seconds;r.travel_routes=t.cost.travel_routes;r.local_routes=t.cost.local_routes;
                }
                if(plan.decisions.size()<256) {r.id=int(plan.decisions.size());plan.decisions.push_back(std::move(r));}
            }
            for(auto r:t.rejected) if(plan.decisions.size()<256) {
                r.id=int(plan.decisions.size());r.rejection_reason=r.reason;r.reason="Roof patch retained planar";
                plan.decisions.push_back(std::move(r));
            }
        }
        for(const auto& r:plan.decisions) m_layer_region_records.push_back(r.encode());
        // Keep the bounded import report below the processor's per-line limit.
        // A second process/export pass must not append the same report again.
        const auto old_report=m_layer_strategy_decision.find(" Generated benefit/cost choice:");
        if(old_report!=std::string::npos) m_layer_strategy_decision.resize(old_report);
        m_layer_strategy_decision+=report.str().substr(0,1400)+
            " Travel uses the production route-clearance planner on native candidate path order. Time excludes final seam/XY-detour ordering, acceleration, cooling and startup. Surface gains are geometry estimates, not measured finish or printability.";
    };
    if(plan.final) {
        if(plan.trials.at(plan.current).field && !m_curved_field)
            throw SlicingError("The selected generated-path candidate could not be reconstructed");
        publish();return false;
    }
    auto& trial=plan.trials.at(plan.current);
    trial.assessed=true;trial.reason=invalid;
    if(trial.field && !m_curved_field) trial.reason=m_layer_strategy_decision;
    trial.valid=trial.reason.empty();
    if(trial.valid) try {
        trial.surface=surface_benefit(plan.mesh,m_curved_field.get(),plan.threshold,[this]{m_print->throw_if_canceled();});
        // Measure the generated paths, including regenerated native supports.
        // Replay the candidate's deposited beads through the production writer:
        // its shared route planner sees exactly the same chord-width map,
        // asymmetric envelope, gantry, rounding and unknown-motion fallback.
        // The cost pass visits native paths before final seam/XY-detour ordering;
        // timings remain estimates, excluding startup, cooling and acceleration.
        GeneratedCost cost;std::optional<Vec3d> last;
        const auto& pc=m_print->config();const size_t nozzle=curved_nozzle_index();
        // Machine limit arrays include normal/silent mode entries. They are
        // not indexed by active tip; a silent-mode zero would make cost infinite.
        const double vz=CurvedTransition::FixedNozzleLimits::from_config(pc,nozzle).z_speed;
        const double travel=std::max(1.,pc.travel_speed.get_at(nozzle));
        const double configured_z=pc.travel_speed_z.get_at(nozzle);
        const double travel_z=std::min(vz,configured_z>0.?configured_z:travel);
        GCodeWriter route_writer;
        if(m_curved_field) {
            route_writer.apply_print_config(pc);route_writer.set_is_bbl_machine(m_print->is_BBL_printer());
            const auto used=m_print->extruders();route_writer.set_extruders(used);route_writer.set_extruder(used.front());
            const double clearance=std::max(2.,std::max(pc.nonplanar_approach_distance.value,pc.nonplanar_departure_distance.value));
            // Match final G-code: a per-tip height is meaningful only on a
            // fixed dual carriage. Single-nozzle presets may retain a zero
            // placeholder there; their common machine/Virtual Z limit applies.
            route_writer.set_curved_blend(m_curved_field,*m_curved_domain,Vec2d::Zero(),clearance,vz,
                pc.nozzle_diameter.size()==2?int(nozzle):-1);
        }
        const PrintRegionConfig* cost_region=&printing_region(0).config();
        const auto physical=[&](double x,double y,double w) {return Vec3d(x,y,m_curved_field?m_curved_field->world_z(x,y,w):w);};
        const auto measure=[&](const ExtrusionPath& p,double w) {
            m_print->throw_if_canceled();const auto points=p.polyline.to_polyline().points;if(points.size()<2) return;
            double speed=cost_region->inner_wall_speed.get_at(nozzle);
            if(p.role()==erSupportMaterialInterface) speed=m_config.support_interface_speed.get_at(nozzle);
            else if(is_support(p.role())) speed=m_config.support_speed.get_at(nozzle);
            else if(is_bridge(p.role())) speed=cost_region->bridge_speed.get_at(nozzle);
            else if(p.role()==erExternalPerimeter) speed=cost_region->outer_wall_speed.get_at(nozzle);
            else if(p.role()==erInternalInfill) speed=cost_region->sparse_infill_speed.get_at(nozzle);
            else if(p.role()==erSolidInfill) speed=cost_region->internal_solid_infill_speed.get_at(nozzle);
            else if(p.role()==erTopSolidInfill) speed=cost_region->top_surface_speed.get_at(nozzle);
            speed=std::max(1.,speed);++cost.paths;
            Vec3d previous=physical(unscale<double>(points.front().x()),unscale<double>(points.front().y()),w);
            if(m_curved_field) {
                route_writer.set_curved_path(w,p.width,p.height,is_bridge(p.role()));
                if(!last) {
                    // The common startup/first approach is outside this object's
                    // cost comparison. Begin at its first deposition point.
                    route_writer.set_position(previous);route_writer.set_current_position_clear(true);
                }
                const auto route=route_writer.plan_curved_travel({previous});
                cost.motion_seconds+=route.seconds;cost.travel_seconds+=route.seconds;
                if(!route.no_op) {++cost.travel_routes;if(route.local_route) ++cost.local_routes;}
                route_writer.travel_to_xyz(Vec3d(unscale<double>(points.front().x()),unscale<double>(points.front().y()),w));
            }
            if(last) {
                const double xy=(previous.head<2>()-last->head<2>()).norm();
                if(!m_curved_field && (xy>1e-5 || std::abs(previous.z()-last->z())>1e-5)) {
                    const double seconds=std::max(xy/travel,std::abs(previous.z()-last->z())/travel_z);
                    cost.motion_seconds+=seconds;cost.travel_seconds+=seconds;++cost.travel_routes;
                }
            }
            for(size_t i=1;i<points.size();++i) {
                const Vec2d a=unscale(points[i-1]),b=unscale(points[i]);
                const double logical=(b-a).norm();if(logical<1e-12) continue;
                const int divisions=m_curved_field?std::min(16,Ramp::chord_segments(logical,m_curved_field->slope_bound())):1;
                for(int j=1;j<=divisions;++j) {
                    const Vec2d xy=a+(b-a)*(double(j)/divisions),mid=a+(b-a)*((double(j)-.5)/divisions);
                    const Vec3d next=physical(xy.x(),xy.y(),w);const double length=(next-previous).norm();
                    double ratio=1.;
                    if(m_curved_field && !is_bridge(p.role())) {
                        const auto g=m_curved_field->gradient(mid.x(),mid.y(),w);
                        const double height=(m_curved_field->world_z(mid.x(),mid.y(),w)-
                            m_curved_field->world_z(mid.x(),mid.y(),w-p.height))/std::hypot(1.,std::hypot(g.dx,g.dy));
                        ratio=Ramp::bead_area(p.width,height)/Ramp::bead_area(p.width,p.height);
                    }
                    const double volume=p.mm3_per_mm*length*ratio;
                    (is_support(p.role())?cost.support_mm3:cost.model_mm3)+=volume;
                    cost.path_mm+=length;cost.motion_seconds+=std::max(length/speed,std::abs(next.z()-previous.z())/vz);
                    previous=next;
                }
                if(m_curved_field) route_writer.extrude_to_xy(b,p.mm3_per_mm*logical*route_writer.filament()->e_per_mm3());
            }
            last=previous;
        };
        std::function<void(const ExtrusionEntity&,double)> accumulate;
        accumulate=[&](const ExtrusionEntity& e,double z) {
            if(const auto* p=dynamic_cast<const ExtrusionPath*>(&e)) measure(*p,z);
            else if(const auto* c=dynamic_cast<const ExtrusionEntityCollection*>(&e)) for(const auto* child:c->entities) accumulate(*child,z);
            else if(const auto* l=dynamic_cast<const ExtrusionLoop*>(&e)) for(const auto& p:l->paths) measure(p,z);
            else if(const auto* m=dynamic_cast<const ExtrusionMultiPath*>(&e)) for(const auto& p:m->paths) measure(p,z);
        };
        // Supports must enter the obstacle map when they are printed, rather
        // than after the entire model. Equal-height supports precede the model.
        size_t model=0,support=0;
        while(model<m_layers.size() || support<m_support_layers.size()) {
            if(support<m_support_layers.size() && (model==m_layers.size() ||
                m_support_layers[support]->print_z<=m_layers[model]->print_z+1e-6)) {
                const auto* layer=m_support_layers[support++];
                cost_region=&printing_region(0).config();accumulate(layer->support_fills,layer->print_z);
            } else {
                const auto* layer=m_layers[model++];
                for(const auto* r:layer->regions()) {
                    cost_region=&r->region().config();
                    if(r->region().config().is_infill_first.value) {accumulate(r->fills,layer->print_z);accumulate(r->perimeters,layer->print_z);}
                    else {accumulate(r->perimeters,layer->print_z);accumulate(r->fills,layer->print_z);}
                }
            }
        }
        trial.cost=cost;
    } catch(const RuntimeError& e) {trial.valid=false;trial.reason=e.what();}
    if(++plan.current<plan.trials.size()) {m_curved_rejected_centres.clear();return retry();}
    const auto& baseline=plan.trials.back();
    if(!baseline.valid || baseline.field) throw SlicingError("Automatic comparison could not generate its planar reference");
    plan.chosen=plan.trials.size()-1;double best=1e-4;
    for(size_t i=0;i<plan.trials.size();++i) if(plan.trials[i].valid) {
        auto& t=plan.trials[i];t.score=planning_score(baseline.surface,t.surface,baseline.cost,t.cost);
        if(t.score>best) {best=t.score;plan.chosen=i;}
    }
    plan.final=true;plan.current=plan.chosen;m_curved_rejected_centres.clear();
    if(plan.chosen!=plan.trials.size()-1) return retry();
    publish();return false;
}

void PrintObject::slice()
{
    //update the transformation UNLAYERED_MOD
    this->update_slice_frame();

    BOOST_LOG_TRIVIAL(info) << "slice frame: segment=" << (this->is_segment() ? this->segment_index() : -1)
                            << " post_trafo translation = (" << m_post_trafo.translation().x() << ", "
                            << m_post_trafo.translation().y() << ", " << m_post_trafo.translation().z()
                            << ") center offset = (" << unscale<double>(m_center_offset[0]) << ", "
                            << unscale<double>(m_center_offset[1]) << ")";


    if (! this->set_started(posSlice))
        return;
    if(!m_curved_planning_retry) {
        m_curved_rejected_centres.clear();m_curved_plan.reset();m_layer_region_records.clear();
    }
    else this->update_slicing_parameters();
    m_curved_planning_retry=false;
    //BBS: add flag to reload scene for shell rendering
    m_print->set_status(5, L("Slicing mesh"), PrintBase::SlicingStatus::RELOAD_SCENE);
    std::vector<coordf_t> layer_height_profile;
    //m_model_object->rotate(0.1, Y);

    //what this would do is make sure that the object starts printing at the bottom of the bounding 
    //coordf_t old_min                    = m_slicing_params.object_print_z_min;
    //coordf_t old_max                    = m_slicing_params.object_print_z_max;
    //m_slicing_params.object_print_z_min = m_model_object->bounding_box_exact().transformed(m_post_trafo).min.z();
    //m_slicing_params.object_print_z_max = m_model_object->bounding_box_exact().transformed(m_post_trafo).max.z();

  /*  m_model_object->scale(2);
    m_model_object->scale(0.5);*/
    //m_model_object->
    //if we wanted to do a better approach, we would allow the "layer_height_profile" to embed the angle of the layers.
    //this means angle would be set in the slicing parameters
    std::vector<coordf_t> object_layers;
    m_curved_field.reset();
    m_curved_ramp.reset();
    m_curved_domain.reset();
    m_curved_mesh.reset();
    m_layer_strategy_decision.clear();
    const bool automatic=m_config.automatic_layer_strategy.value;
    std::optional<CurvedTransition::UndersideAnalysis> underside_analysis;
    bool use_curved=automatic || m_config.curved_layer_blend.value;
    if (use_curved) {
        std::string error = this->curved_blend_error(automatic);
        if (!error.empty()) {
            if (!automatic) throw SlicingError(error);
            use_curved=false;m_layer_strategy_decision="Automatic selection: planar. "+error;
        }
    }
    if (use_curved) {
        const ModelVolume& volume = *m_model_object->volumes.front();
        const Transform3d transform = m_object_to_slice * volume.get_matrix();
        std::vector<CurvedTransition::Ramp::Triangle> triangles;
        triangles.reserve(volume.mesh().its.indices.size());
        for (const auto& face : volume.mesh().its.indices) {
            CurvedTransition::Ramp::Triangle t;
            for (int k=0;k<3;++k) {
                const Vec3d p = transform * volume.mesh().its.vertices[face[k]].cast<double>();
                t[k] = {{p.x(),p.y(),p.z()}};
            }
            if (transform.linear().determinant()<0.) std::swap(t[1],t[2]);
            triangles.push_back(t);
        }
        try {
            const auto limits=CurvedTransition::FixedNozzleLimits::from_config(m_print->config(),curved_nozzle_index());
            if(m_print->config().nozzle_diameter.size()==2) {
                double xmin=INFINITY,xmax=-INFINITY,ymin=INFINITY,ymax=-INFINITY;
                for(const auto& t:triangles) for(const auto& p:t) {
                    xmin=std::min(xmin,p[0]);xmax=std::max(xmax,p[0]);
                    ymin=std::min(ymin,p[1]);ymax=std::max(ymax,p[1]);
                }
                const auto& pc=m_print->config();Points common,active;
                for(const auto& p:pc.printable_area.values) common.push_back(Point::new_scale(p));
                for(const auto& p:pc.extruder_printable_area.values[limits.nozzle]) active.push_back(Point::new_scale(p));
                for(const auto& instance:m_instances) {
                    const Vec2d shift=unscale(instance.shift)-m_print->get_plate_origin().head<2>();
                    Polygon footprint;for(const Vec2d& p:std::vector<Vec2d>{{xmin,ymin},{xmax,ymin},{xmax,ymax},{xmin,ymax}})
                        footprint.points.push_back(Point::new_scale(p+shift));
                    const auto fits=[&](const Points& area) {return area.size()>=3 && diff(Polygons{footprint},Polygons{Polygon(area)}).empty();};
                    if(!fits(common) || !fits(active)) throw std::invalid_argument("Curved model and travel footprint exceed the fixed active nozzle's printable area.");
                }
            }
            if (its_num_open_edges(volume.mesh().its) != 0)
                throw std::invalid_argument("Curved blend requires a closed mesh; repair the model first.");
            std::vector<CutPlane> selected_planes;
            std::vector<std::vector<CutPlane>> proposals;
            std::string proposal_reason;
            if (m_model_object->has_cut_planes())
                for (const auto& plane:m_model_object->cut_planes.planes) if (plane.enabled) selected_planes.push_back(plane);
            if (automatic) {
                const auto& pc=m_print->config();
                const HotendEnvelope envelope=HotendEnvelope::from_config(pc);
                double xmin=INFINITY,xmax=-INFINITY,ymin=INFINITY,ymax=-INFINITY,top=-INFINITY;
                for (const auto& t:triangles) for (const auto& p:t) {
                    xmin=std::min(xmin,p[0]);xmax=std::max(xmax,p[0]);
                    ymin=std::min(ymin,p[1]);ymax=std::max(ymax,p[1]);top=std::max(top,p[2]);
                }
                const double limit=virtual_tilt_limit_enforced_deg(envelope,pc.nonplanar_gantry_height.value,
                    pc.nonplanar_virtual_max_angle.value,std::hypot(xmax-xmin,ymax-ymin));
                const double allowed=std::tan(limit*PI/180.);
                // Regional logical-height fields certify their entire compact
                // footprint against every hotend corner. Keep the independent
                // gantry/user angle cap, without imposing the unrelated worst
                // azimuth a second time. Other field types retain that cap.
                HotendEnvelope gantry_only=envelope;gantry_only.height=0.;
                const double regional_limit=virtual_tilt_limit_enforced_deg(gantry_only,
                    pc.nonplanar_gantry_height.value,pc.nonplanar_virtual_max_angle.value,
                    std::hypot(xmax-xmin,ymax-ymin));
                const double regional_allowed=std::tan(regional_limit*PI/180.);
                CurvedTransition::RegionalClearance regional_clearance;
                if(envelope.enabled()) for(double x:{envelope.spread(HotendEnvelope::XPos),-envelope.spread(HotendEnvelope::XNeg)})
                    for(double y:{envelope.spread(HotendEnvelope::YPos),-envelope.spread(HotendEnvelope::YNeg)})
                        regional_clearance.corners.push_back({x,y});
                underside_analysis=CurvedTransition::analyze_undersides(triangles,allowed,m_config.support_threshold_angle.value,
                    [this]{m_print->throw_if_canceled();});
                const auto target=CurvedTransition::automatic_target(triangles,allowed);
                bool sparse=false;double solid_depth=INFINITY;
                for(size_t i=0;i<num_printing_regions();++i) {
                    const auto& rc=printing_region(i).config();sparse|=rc.sparse_infill_density.value<100.;
                    solid_depth=std::min(solid_depth,std::max(rc.top_shell_thickness.value,rc.top_shell_layers.value*m_config.layer_height.value));
                }
                // An authored stack supplies its own target even on a flat roof.
                // Support/contact and sparse-infill constraints still require
                // the regional strategy before considering a whole-object stack.
                const bool regional=bool(m_curved_plan) || target.needs_support_planning ||
                    (selected_planes.empty() && !target.useful) || sparse || has_support_material();
                if(regional) {
                    using namespace CurvedTransition;
                    if(!envelope.enabled() || pc.nonplanar_gantry_height.value<=0.)
                        throw std::invalid_argument("Regional curved layers require a measured configured hotend and gantry envelope");
                    ModelDomain domain{xmin,xmax,ymin,ymax,top,0.};
                    std::shared_ptr<Field> program;
                    if(!m_curved_plan) {
                      m_curved_plan=std::make_shared<PlanningState>();auto& plan=*m_curved_plan;
                      plan.mesh=triangles;plan.allowed=allowed;plan.threshold=m_config.support_threshold_angle.value;
                      const auto add=[&](std::shared_ptr<Field> field,const ModelDomain& d,const std::string& name) {
                          PlanningTrial trial;trial.field=std::move(field);trial.domain=d;trial.name=name;
                          plan.trials.push_back(std::move(trial));
                      };
                      auto patches=select_regions(triangles,m_slicing_params.first_object_layer_height,
                          m_config.layer_height.value,regional_allowed,solid_depth,sparse,
                          [this]{m_print->throw_if_canceled();},regional_clearance);
                      if(!patches.empty()) add(std::make_shared<Regions>(domain,std::move(patches),regional_allowed,regional_clearance),domain,"Roof");
                      if(selected_planes.empty() && (!has_support_material() || m_config.support_type.value==stNormalAuto)) {
                        const double support_limit=virtual_tilt_limit_enforced_deg(envelope,pc.nonplanar_gantry_height.value,
                            pc.nonplanar_virtual_max_angle.value,std::hypot(xmax-xmin+20.,ymax-ymin+20.));
                        const double combined_limit=std::min(allowed,std::tan(support_limit*PI/180.));
                        // Full underside-only plus two allocations that reserve
                        // a real slope budget for simultaneous roof correction.
                        for(double allocation:{1.,.5,.25}) {
                          auto underside=select_underside_program(triangles,*underside_analysis,
                            m_slicing_params.first_object_layer_height,m_config.layer_height.value,limits.max_layer,
                            combined_limit*allocation,10.,
                            has_support_material()?std::max({0.,m_config.support_top_z_distance.value,m_config.support_bottom_z_distance.value}):0.,
                            [this]{m_print->throw_if_canceled();});
                          if(!underside) continue;
                          if(allocation==1.) {add(underside,underside->domain(),"Underside");continue;}
                          const double headroom=(combined_limit-underside->slope_bound())*underside->jacobian_min();
                          if(headroom<std::tan(.5*PI/180.)) continue;
                          auto logical=triangles;
                          for(auto& t:logical) for(auto& p:t) p[2]=underside->slice_z(p[0],p[1],p[2]);
                          auto joint_patches=select_regions(logical,m_slicing_params.first_object_layer_height,
                              m_config.layer_height.value,headroom,solid_depth,sparse,[this]{m_print->throw_if_canceled();});
                          if(joint_patches.empty()) continue;
                          auto roof=std::make_shared<Regions>(underside->domain(),std::move(joint_patches),headroom);
                          add(std::make_shared<JointField>(underside,roof,combined_limit),underside->domain(),
                              allocation==.5?"Joint half budget":"Joint quarter budget");
                        }
                      }
                      // Always compare against a genuinely planar native slice,
                      // rather than assuming extra supports or runtime from angles.
                      if(!plan.trials.empty()) add({},domain,"Planar");
                    }
                    auto& plan=*m_curved_plan;
                    if(plan.trials.empty()) {
                        if(underside_analysis->has_candidate && has_support_material() && m_config.support_type.value!=stNormalAuto)
                            throw std::invalid_argument("an underside direction was found, but mapped support generation currently supports Normal (auto), not the selected support type; support settings were retained");
                        throw std::invalid_argument("no useful regional roof or underside field fits the foundation, support and transition bounds");
                    }
                    auto& trial=plan.trials.at(plan.current);program=trial.field;domain=trial.domain;
                    if(!program) throw std::invalid_argument("Planar reference for generated benefit/cost comparison");
                    domain.slope_max=program->slope_bound();
                    indexed_triangle_set placed=volume.mesh().its;
                    for(auto& p:placed.vertices) p=(transform*p.cast<double>()).cast<float>();
                    if(transform.linear().determinant()<0.) its_flip_triangles(placed);
                    auto mapped=map_mesh(std::move(placed),*program,[this]{m_print->throw_if_canceled();});
                    double mapped_top=0.;for(const auto& p:mapped.vertices) mapped_top=std::max(mapped_top,double(p.z()));
                    program->set_reference_top(mapped_top);
                    object_layers=Ramp{}.schedule(*program,m_slicing_params.first_object_layer_height,m_config.layer_height.value,limits.min_layer,limits.max_layer);
                    double step=0.;for(size_t i=0;i+1<object_layers.size();i+=2) step=std::max(step,object_layers[i+1]-object_layers[i]);
                    domain.top_max+=program->spacing_bounds(step).max_vertical_mm+.02;
                    const double approach=std::max(2.,std::max(pc.nonplanar_approach_distance.value,pc.nonplanar_departure_distance.value));
                    if(domain.top_max+approach>limits.max_z) throw std::invalid_argument("Regional curved layers and travel exceed the configured Z limit");
                    m_curved_field=program;m_curved_domain=domain;m_curved_mesh=std::make_shared<const indexed_triangle_set>(std::move(mapped));
                }
                if (selected_planes.empty() && !m_curved_field) {
                    proposal_reason=target.reason;
                    if (target.useful) {
                        const double slope=std::hypot(target.gx,target.gy);
                        const double budget=std::min(.25,.95*(1.-slope/allowed));
                        const double projection=std::abs(target.gx)*((xmax-xmin)*.5+1e-4)+
                                                std::abs(target.gy)*((ymax-ymin)*.5+1e-4);
                        const double depth=std::max(3.*m_config.layer_height.value,1.875*projection/budget);
                        const double first=m_slicing_params.first_object_layer_height;
                        if (first+depth+m_config.layer_height.value*.5<=top) {
                            CutPlane plane;
                            plane.point=m_object_to_slice.inverse()*Vec3d((xmin+xmax)*.5,(ymin+ymax)*.5,first+depth*.5);
                            plane.normal=(m_object_to_slice.linear().transpose()*Vec3d(-target.gx,-target.gy,1.)).normalized();
                            proposals.push_back({plane});
                        } else proposal_reason="not enough model height for the inferred curved transition";
                    }
                    // The existing planner supplies LOCAL surface-region and
                    // multi-direction proposals. Its piecewise-planar scores
                    // are only a ranking heuristic here, not a curved result.
                    AutoPlanes::Options options;
                    options.preference=AutoPlanes::Preference::Finish;
                    options.keep_existing=false;options.max_planes=2;
                    options.skeleton=AutoPlanes::Options::Skeleton::Off;
                    AutoPlanes::Progress progress;
                    progress.canceled=[this]{return m_print->canceled();};
                    const auto regional=AutoPlanes::plan(*m_model_object,
                        m_model_object->instances.front()->get_matrix(),m_print->full_print_config(),options,&progress);
                    m_print->throw_if_canceled();
                    for (const auto& candidate:regional.candidates) {
                        if (!candidate.feasible || candidate.is_planar() || candidate.net_benefit<options.min_net_benefit) continue;
                        // Do not infer support savings from the planner's proxy.
                        const double base=regional.planar.predicted.terrace_length_mm;
                        if (base<=0. || candidate.predicted.terrace_length_mm>base*.90) continue;
                        std::vector<CutPlane> planes;
                        for (const auto& plane:candidate.planes) planes.push_back(plane.plane);
                        proposals.push_back(std::move(planes));
                        if (proposals.size()>=8) break;
                    }
                    if (proposals.empty()) throw std::invalid_argument(proposal_reason);
                }
            }
            if (!selected_planes.empty() || !automatic) proposals.push_back(selected_planes);
            auto build_candidate=[&](const std::vector<CutPlane>& selected_planes) {
            if (!selected_planes.empty()) {
                using namespace CurvedTransition;
                ModelDomain domain{INFINITY,-INFINITY,INFINITY,-INFINITY,-INFINITY,0.};
                double bottom=INFINITY;
                for (const auto& t:triangles) for (const auto& p:t) {
                    domain.xmin=std::min(domain.xmin,p[0]); domain.xmax=std::max(domain.xmax,p[0]);
                    domain.ymin=std::min(domain.ymin,p[1]); domain.ymax=std::max(domain.ymax,p[1]);
                    domain.top_max=std::max(domain.top_max,p[2]); bottom=std::min(bottom,p[2]);
                }
                if (std::abs(bottom)>2e-5) throw std::invalid_argument("Curved blend model must rest on the bed; floating or sunk geometry is not supported");
                const auto& pc=m_print->config();
                const HotendEnvelope envelope=HotendEnvelope::from_config(pc);
                if (!envelope.enabled() || pc.nonplanar_gantry_height.value<=0.)
                    throw std::invalid_argument("Curved blend needs a measured configured hotend and gantry envelope");
                struct PlacedPlane { PlaneProgram::Target target; double gx,gy; };
                std::vector<PlacedPlane> planes;
                const double cx=(domain.xmin+domain.xmax)*.5,cy=(domain.ymin+domain.ymax)*.5;
                for (const CutPlane& plane:selected_planes) if (plane.enabled) {
                    const Vec3d point=m_object_to_slice*plane.point;
                    Vec3d normal=(m_object_to_slice.linear().inverse().transpose()*plane.normal).normalized();
                    Vec3d build=(m_object_to_slice.linear().inverse().transpose()*plane.effective_build_normal()).normalized();
                    if (normal.z()<0.) normal=-normal;
                    if (build.z()<0.) build=-build;
                    if (!normal.allFinite() || !build.allFinite() || normal.z()<.1 || build.z()<.1)
                        throw std::invalid_argument("Curved blend requires upward nonvertical plane and build normals");
                    const double gx=-build.x()/build.z(),gy=-build.y()/build.z();
                    const double slope=std::hypot(gx,gy),angle=std::atan(slope)*180./PI;
                    const double limit=virtual_tilt_limit_enforced_deg(envelope,pc.nonplanar_gantry_height.value,
                        pc.nonplanar_virtual_max_angle.value,std::hypot(domain.xmax-domain.xmin,domain.ymax-domain.ymin),std::atan2(-gy,-gx));
                    if (angle>limit+1e-5) throw std::invalid_argument("Curved blend plane exceeds the configured directional toolhead/gantry tilt limit");
                    domain.slope_max=std::max(domain.slope_max,slope);
                    const double height=point.z()-(normal.x()*(cx-point.x())+normal.y()*(cy-point.y()))/normal.z();
                    planes.push_back({{height,gx,gy},-normal.x()/normal.z(),-normal.y()/normal.z()});
                }
                if (planes.size()>64) throw std::invalid_argument("Curved blend accepts at most 64 enabled planes");
                std::sort(planes.begin(),planes.end(),[](const PlacedPlane& a,const PlacedPlane& b){return a.target.height<b.target.height;});
                for (size_t i=1;i<planes.size();++i) for (double x:{domain.xmin,domain.xmax}) for (double y:{domain.ymin,domain.ymax})
                    if (planes[i].target.height-planes[i-1].target.height+
                        (planes[i].gx-planes[i-1].gx)*(x-cx)+(planes[i].gy-planes[i-1].gy)*(y-cy)<=1e-5)
                        throw std::invalid_argument("Curved blend cannot yet blend intersecting branch planes; use a nonintersecting height-ordered stack");
                std::vector<PlaneProgram::Target> targets;
                for (const auto& p:planes) targets.push_back(p.target);
                const double approach=std::max(2.,std::max(pc.nonplanar_approach_distance.value,pc.nonplanar_departure_distance.value));
                if (limits.max_z<=0. || domain.top_max+approach>limits.max_z ||
                    domain.top_max+approach>pc.printable_height.value)
                    throw std::invalid_argument("Curved blend model and travel exceed the configured machine/Virtual Z limit");
                const double first=m_slicing_params.first_object_layer_height;
                const double all_direction_limit=virtual_tilt_limit_enforced_deg(envelope,pc.nonplanar_gantry_height.value,
                    pc.nonplanar_virtual_max_angle.value,std::hypot(domain.xmax-domain.xmin,domain.ymax-domain.ymin));
                auto program=std::make_shared<PlaneProgram>(domain,first,m_config.layer_height.value,std::move(targets),std::tan(all_direction_limit*PI/180.));
                domain.slope_max=program->slope_bound();
                indexed_triangle_set placed=volume.mesh().its;
                for (auto& p:placed.vertices) p=(transform*p.cast<double>()).cast<float>();
                if (transform.linear().determinant()<0.) its_flip_triangles(placed);
                auto mapped=map_mesh(std::move(placed),*program,[this]{m_print->throw_if_canceled();});
                double top=0.; for (const auto& p:mapped.vertices) top=std::max(top,double(p.z()));
                if (domain.top_max<program->blends().back().hi+m_config.layer_height.value*.5)
                    throw std::invalid_argument("Curved blend cannot finish the selected direction transition before the model ends; move the plane or use a gentler change");
                program->set_reference_top(top);
                Ramp schedule_bounds{};
                object_layers=schedule_bounds.schedule(*program,first,m_config.layer_height.value,limits.min_layer,limits.max_layer);
                // Contours are sampled at the layer midpoint and deposited at
                // its top. A roof not aligned with this field can therefore
                // print above its CAD height. Reserve a whole maximum physical
                // layer (conservative versus the half-layer centre offset),
                // plus mesh/chord/rounding error, for all travel and Z checks.
                double largest_step=0.;
                for (size_t i=0;i+1<object_layers.size();i+=2)
                    largest_step=std::max(largest_step,object_layers[i+1]-object_layers[i]);
                domain.top_max+=program->spacing_bounds(largest_step).max_vertical_mm+.02;
                if (domain.top_max+approach>limits.max_z ||
                    domain.top_max+approach>pc.printable_height.value)
                    throw std::invalid_argument("Curved blend printed layers and travel exceed the configured machine/Virtual Z limit");
                m_curved_field=program; m_curved_domain=domain;
                m_curved_mesh=std::make_shared<const indexed_triangle_set>(std::move(mapped));
            } else {
            m_curved_ramp = CurvedTransition::Ramp::certify(triangles);
            const auto& r = *m_curved_ramp;
            const auto& pc = m_print->config();
            const double reach = std::hypot(r.xmax-r.xmin,r.ymax-r.ymin);
            const HotendEnvelope envelope = HotendEnvelope::from_config(pc);
            // Build-normal azimuth, opposite to the uphill roof gradient.
            const double limit = virtual_tilt_limit_enforced_deg(envelope, pc.nonplanar_gantry_height.value,
                pc.nonplanar_virtual_max_angle.value, reach, std::atan2(-r.gy,-r.gx));
            if (!envelope.enabled() || pc.nonplanar_gantry_height.value<=0. || limit<=0. || r.angle_deg()>limit+1e-5)
                throw std::invalid_argument("Curved blend roof exceeds the configured hotend/gantry/maximum-angle clearance; a measured envelope is required");
            // Deliberately bounded first release. 10 mm is a scope restriction,
            // NOT a measured safe nonplanar printing height.
            const double approach = std::max(2.,std::max(pc.nonplanar_approach_distance.value,pc.nonplanar_departure_distance.value));
            if (r.top_max>10. || r.top_max+approach>pc.printable_height.value ||
                limits.max_z<=0. || r.top_max+approach>limits.max_z)
                throw std::invalid_argument("Curved blend ramp/travel exceeds the experimental 10 mm part limit or configured machine Z limit");
            const double first = m_slicing_params.first_object_layer_height;
            // Search a bounded set of reference heights. Each candidate is
            // independently certified; success is never inferred from sampling.
            for (int trial=0;trial<=32;++trial) {
                const double reference=(r.top_min+r.top_max)*.5+(r.top_max-r.top_min)*.5*trial/32.;
                try {
                    const auto field=r.field(first,reference);
                    object_layers=r.schedule(*field,first,m_config.layer_height.value,
                        limits.min_layer,limits.max_layer);
                    m_curved_field=field; break;
                } catch (const std::invalid_argument&) { }
            }
            if (!m_curved_field) throw std::invalid_argument("Curved blend cannot meet the configured minimum/maximum layer heights at the requested layer step");
            m_curved_domain=r.domain();
            }
            };
            std::string last_rejection;
            for (const auto& proposal:proposals) {
                if(m_curved_field) break;
                m_print->throw_if_canceled();
                m_curved_field.reset();m_curved_ramp.reset();m_curved_domain.reset();m_curved_mesh.reset();object_layers.clear();
                try {build_candidate(proposal);break;}
                catch (const std::invalid_argument& e) {if (!automatic) throw;last_rejection=e.what();}
                catch (const SlicingError& e) {if (!automatic) throw;last_rejection=e.what();}
            }
            if (!m_curved_field) throw std::invalid_argument(last_rejection.empty()?"no useful curved direction was found":last_rejection);
            if(automatic && selected_planes.empty() && !m_curved_plan) {
                // Explicit authored targets remain the user's choice. The
                // generated-cost policy chooses only inferred strategies.
                // The exact ramp/global plane paths also pay their measured
                // native path cost; regional proposals are not the only ones
                // that can lose to a planar control.
                m_curved_plan=std::make_shared<CurvedTransition::PlanningState>();
                auto& plan=*m_curved_plan;plan.mesh=triangles;plan.threshold=m_config.support_threshold_angle.value;
                CurvedTransition::PlanningTrial curved;curved.field=std::const_pointer_cast<CurvedTransition::Field>(m_curved_field);
                curved.domain=*m_curved_domain;curved.name="Global roof/plane";plan.trials.push_back(std::move(curved));
                CurvedTransition::PlanningTrial planar;planar.domain=*m_curved_domain;planar.name="Planar";
                plan.trials.push_back(std::move(planar));
            }
            m_slicing_params.object_print_z_max = m_curved_field->reference_top_mm();
            m_slicing_params.object_print_z_uncompensated_max = m_slicing_params.object_print_z_max;
            if(m_curved_field->maps_supports()) {
                // Native normal-support layers are generated in logical space.
                // Bound their physical vertical spacing before generating them.
                m_slicing_params.max_suport_layer_height=std::min(m_slicing_params.max_suport_layer_height,
                    limits.max_layer*m_curved_field->jacobian_min());
            }
            if (automatic) {
                std::string selection;
                const auto joint=std::dynamic_pointer_cast<const CurvedTransition::JointField>(m_curved_field);
                if(const auto regions=CurvedTransition::roof_part(m_curved_field)) {
                    double low=INFINITY,high=0.;
                    for(const auto& p:regions->patches()) {
                        const double angle=std::atan(std::hypot(p.gx,p.gy))*180./PI;
                        low=std::min(low,angle);high=std::max(high,angle);
                    }
                    selection=(joint?std::string("Joint roof and underside field; "):std::string{})+
                        std::to_string(regions->patches().size())+" local roof regions; selected centre tilts "+
                        std::to_string(low)+" to "+std::to_string(high)+
                        " deg after comparing local angles and region sizes. "+
                        (joint?(has_support_material()?
                                std::string("Native supports regenerated with both fields; generated bridge/contact checks follow. "):
                                std::string("supports off; no support paths requested. Generated bridge/anchor checks follow. ")):
                               std::string("Bridge and support-contact zones remain planar. "));
                } else if(m_curved_field->maps_supports()) {
                    selection="Underside field applied at "+std::to_string(std::dynamic_pointer_cast<const CurvedTransition::UndersideProgram>(m_curved_field)->angle_deg())+
                        " deg; "+(has_support_material()?std::string("native normal supports regenerated in the same field. "):
                            std::string("supports off; no support paths requested. "))+
                        "Bridge and foundation checks follow path generation. ";
                }
                m_layer_strategy_decision="Automatic selection: curved. "+selection+
                    "Continuous transitions passed configured geometry and layer-spacing checks; physical clearance and finish are unverified.";
            }
            if(automatic && !m_curved_rejected_centres.empty()) m_layer_strategy_decision+=" "+std::to_string(m_curved_rejected_centres.size())+" regions left planar after inspecting generated bridge/support beads.";
        } catch (const std::invalid_argument& e) {
            if (!automatic) throw SlicingError(e.what());
            m_curved_field.reset();m_curved_ramp.reset();m_curved_domain.reset();m_curved_mesh.reset();
            object_layers.clear();m_layer_strategy_decision="Automatic selection: planar. "+std::string(e.what());
        } catch (const SlicingError& e) {
            if (!automatic) throw;
            m_curved_field.reset();m_curved_ramp.reset();m_curved_domain.reset();m_curved_mesh.reset();
            object_layers.clear();m_layer_strategy_decision="Automatic selection: planar. "+std::string(e.what());
        }
        if (automatic && !m_curved_field) {
            this->update_layer_height_profile(*this->model_object(),m_slicing_params,layer_height_profile);
            object_layers=generate_object_layers(m_slicing_params,layer_height_profile,m_config.precise_z_height.value);
        }
        if(automatic && underside_analysis) m_layer_strategy_decision+=" "+CurvedTransition::underside_analysis_summary(
            *underside_analysis,m_curved_field && m_curved_field->maps_supports());
    } else if (this->is_segment()) {
        // A segment's slicing parameters are sized to its own extent (see
        // update_slicing_parameters), so validating the object's layer-height profile against that
        // shortened height would fail and silently DISCARD it -- taking any painted variable-layer
        // profile with it, and re-deriving height ranges against segment-local z, which lands
        // ranged settings (temperatures, speeds, wall counts) on the wrong part of the model.
        //
        // Instead: build the schedule for the FULL object in this segment's build frame, then crop
        // and rebase it. Layer boundaries and height-range transitions are therefore inherited from
        // the object's real schedule rather than regenerated.
        SlicingParameters full_params = m_slicing_params;
        // slice_z_base(), not build_z_min: the crop is rebased to the segment's own zero to match
        // the slice frame, and for a sunk ROOT that zero is the build plate, not the material's
        // lowest point (see CutSegmentDescriptor::slice_z_base).
        const coordf_t seg_lo = m_segment->slice_z_base();
        // Up to the support reach, not just the material: the layers above the material stay
        // empty and exist for the column carrying a later segment (segment_slice_top()). The full
        // schedule below caps this at the object's own extent, which every cut face lies within.
        const coordf_t seg_hi = this->segment_slice_top();
        // Full object extent along the same build axis. R maps the build normal onto +Z, so this
        // is the same projection build_z_min/max were measured with.
        Transform3d rot = Transform3d::Identity();
        {
            double b_deg = 0., c_deg = 0.;
            this->effective_layer_rotation_bc(b_deg, c_deg);
            rot.linear() = Geometry::layer_rotation_matrix(b_deg, c_deg);
        }
        // raw_mesh(), NOT mesh(): mesh() bakes in the INSTANCE transform (and merges every
        // instance), so transforming it again by m_trafo applies the instance twice and measures
        // this extent in a different frame than CutSegmentDescriptor::build_z_min/max, which
        // derive_cut_segments takes from model-part volumes under their volume matrices and the
        // same m_trafo (PrintApply.cpp:184, with XY already zeroed at :166). The two agree only
        // when the build normal is +Z and the instance sits at z=0 -- i.e. exactly the
        // axis-aligned cases -- and diverge by n . d for a tilted segment, which shifts the crop
        // window off the schedule and can leave the segment with no layers at all.
        const BoundingBoxf3 full_bb  = this->model_object()->raw_mesh().transformed_bounding_box(rot * m_trafo);
        const coordf_t      full_lo  = full_bb.min.z();
        full_params.object_print_z_min = 0.;
        // Plus the support slack: a ceiling that leaves the object through its top face still
        // needs the layers above it that a flush demand contact sits in (segment_slice_top()).
        full_params.object_print_z_max = full_bb.max.z() - full_lo + this->segment_support_slack();
        full_params.object_print_z_uncompensated_max = full_params.object_print_z_max;

        this->update_layer_height_profile(*this->model_object(), full_params, layer_height_profile);
        const std::vector<coordf_t> full_layers =
            generate_object_layers(full_params, layer_height_profile, m_config.precise_z_height.value);

        // Keep the (lo, hi) pairs that overlap this segment, then rebase so the first retained
        // boundary sits at the segment's own zero -- matching the slice-frame rebase.
        const coordf_t lo_in_schedule = seg_lo - full_lo;
        const coordf_t hi_in_schedule = seg_hi - full_lo;
        coordf_t       base           = 0.;
        bool           have_base      = false;
        for (size_t i = 0; i + 1 < full_layers.size(); i += 2) {
            const coordf_t lo = full_layers[i], hi = full_layers[i + 1];
            if (hi <= lo_in_schedule + EPSILON || lo >= hi_in_schedule - EPSILON)
                continue;   // entirely below or above this segment
            if (! have_base) { base = lo; have_base = true; m_segment_schedule_base = base; }
            object_layers.push_back(lo - base);
            object_layers.push_back(hi - base);
        }
        BOOST_LOG_TRIVIAL(info) << "segment " << this->segment_index() << ": cropped "
                                << (full_layers.size() / 2) << " object layers to "
                                << (object_layers.size() / 2) << " over ["
                                << lo_in_schedule << ", " << hi_in_schedule << "]"
                                << " seg_lo=" << seg_lo << " seg_hi=" << seg_hi
                                << " full_lo=" << full_lo << " full_hi=" << full_bb.max.z()
                                << " full_layers_max=" << (full_layers.empty() ? -1. : full_layers.back());
    } else {
        this->update_layer_height_profile(*this->model_object(), m_slicing_params, layer_height_profile); //ok so this line of code would need to be replaced. All it does is update layer height profile
        object_layers = generate_object_layers(m_slicing_params, layer_height_profile, m_config.precise_z_height.value);
    }
    if (automatic && !m_layer_strategy_decision.empty())
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,m_layer_strategy_decision);
    m_print->throw_if_canceled();
    m_typed_slices = false;
    this->clear_layers();


    //lots of important things in here too
    m_layers = new_layers(this, object_layers);

    
    //m_slicing_params.object_print_z_min = old_min;
    //m_slicing_params.object_print_z_max = old_max;
    this->slice_volumes(); 
    m_print->throw_if_canceled();
    int firstLayerReplacedBy = 0;

#if 0
    // Fix the model.
    //FIXME is this the right place to do? It is done repeateadly at the UI and now here at the backend.
    std::string warning = fix_slicing_errors(this, m_layers, [this](){ m_print->throw_if_canceled(); }, firstLayerReplacedBy);
    m_print->throw_if_canceled();
    //BBS: send warning message to slicing callback
    // This warning is inaccurate, because the empty layers may have been replaced, or the model has supports.
    //if (!warning.empty()) {
    //    BOOST_LOG_TRIVIAL(info) << warning;
    //    this->active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL, warning, PrintStateBase::SlicingReplaceInitEmptyLayers);
    //}
#endif

    // Detect and process holes that should be converted to polyholes
    this->_transform_hole_to_polyholes();

    // BBS: the actual first layer slices stored in layers are re-sorted by volume group and will be used to generate brim
    groupingVolumesForBrim(this, m_layers, firstLayerReplacedBy);

    // Update bounding boxes, back up raw slices of complex models.
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, m_layers.size()),
        [this](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                m_print->throw_if_canceled();
                Layer &layer = *m_layers[layer_idx];
                layer.lslices_bboxes.clear();
                layer.lslices_bboxes.reserve(layer.lslices.size());
                for (const ExPolygon &expoly : layer.lslices)
                	layer.lslices_bboxes.emplace_back(get_extents(expoly));
                layer.backup_untyped_slices();
            }
        });
    // A declarative plane may create a geometrically valid sliver thinner than the first
    // printable layer. It contributes no toolpaths; keep it as an empty segment so the remaining
    // partition can finish normally. Ordinary empty objects still indicate broken input.
    if (m_layers.empty() && ! this->is_segment())
        throw Slic3r::SlicingError(L("No layers were detected. You might want to repair your STL file(s) or check their size or thickness and retry.\n"));
    
    this->set_done(posSlice);
    //m_model_object->rotate(-0.1, Y);
}

template<typename ThrowOnCancel>
static inline void apply_mm_segmentation(PrintObject &print_object, ThrowOnCancel throw_on_cancel)
{
    // Returns MM segmentation based on painting in MM segmentation gizmo
    std::vector<std::vector<ExPolygons>> segmentation = multi_material_segmentation_by_painting(print_object, throw_on_cancel);
    assert(segmentation.size() == print_object.layer_count());
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, segmentation.size(), std::max(segmentation.size() / 128, size_t(1))),
        [&print_object, &segmentation, throw_on_cancel](const tbb::blocked_range<size_t> &range) {
            const auto  &layer_ranges   = print_object.shared_regions()->layer_ranges;
            double       z              = print_object.get_layer(int(range.begin()))->slice_z;
            auto         it_layer_range = layer_range_first(layer_ranges, z);
            // BBS
            const size_t num_extruders = print_object.print()->config().filament_diameter.size();

            struct ByExtruder {
                ExPolygons  expolygons;
                BoundingBox bbox;
            };

            struct ByRegion {
                ExPolygons expolygons;
                bool       needs_merge { false };
            };

            std::vector<ByExtruder> by_extruder;
            std::vector<ByRegion>   by_region;
            for (size_t layer_id = range.begin(); layer_id < range.end(); ++layer_id) {
                throw_on_cancel();
                Layer &layer = *print_object.get_layer(int(layer_id));
                it_layer_range = layer_range_next(layer_ranges, it_layer_range, layer.slice_z);
                const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;
                // Gather per extruder expolygons.
                by_extruder.assign(num_extruders, ByExtruder());
                by_region.assign(layer.region_count(), ByRegion());
                bool layer_split = false;
                // UNLAYERED_MOD -- the segmentation has one entry per filament_colour
                // (multi_material_segmentation_by_painting), and a project can carry fewer colours
                // than filament_diameter has filaments. Reading past it crashed Slice Plate in
                // get_extents(); the filaments without a colour simply get no painted area.
                const size_t num_segmented = std::min(num_extruders, segmentation[layer_id].size());
                for (size_t extruder_id = 0; extruder_id < num_segmented; ++ extruder_id) {
                    ByExtruder &region = by_extruder[extruder_id];
                    append(region.expolygons, std::move(segmentation[layer_id][extruder_id]));
                    if (! region.expolygons.empty()) {
                        region.bbox = get_extents(region.expolygons);
                        layer_split = true;
                    }
                }

                if (!layer_split)
                    continue;

                // Split LayerRegions by by_extruder regions.
                // layer_range.painted_regions are sorted by extruder ID and parent PrintObject region ID.
                auto it_painted_region_begin = layer_range.painted_regions.cbegin();
                for (int parent_layer_region_idx = 0; parent_layer_region_idx < layer.region_count(); ++parent_layer_region_idx) {
                    if (it_painted_region_begin == layer_range.painted_regions.cend())
                        continue;

                    const LayerRegion &parent_layer_region = *layer.get_region(parent_layer_region_idx);
                    const PrintRegion &parent_print_region = parent_layer_region.region();
                    assert(parent_print_region.print_object_region_id() == parent_layer_region_idx);
                    if (parent_layer_region.slices.empty())
                        continue;

                    // Find the first PaintedRegion, which overrides the parent PrintRegion.
                    auto it_first_painted_region = std::find_if(it_painted_region_begin, layer_range.painted_regions.cend(), [&layer_range, &parent_print_region](const auto &painted_region) {
                        return layer_range.volume_regions[painted_region.parent].region->print_object_region_id() == parent_print_region.print_object_region_id();
                    });

                    if (it_first_painted_region == layer_range.painted_regions.cend())
                        continue; // This LayerRegion isn't overrides by any PaintedRegion.

                    assert(&parent_print_region == layer_range.volume_regions[it_first_painted_region->parent].region);

                    // Update the beginning PaintedRegion iterator for the next iteration.
                    it_painted_region_begin = it_first_painted_region;

                    const BoundingBox parent_layer_region_bbox = get_extents(parent_layer_region.slices.surfaces);
                    bool              self_trimmed             = false;
                    int               self_extruder_id         = -1; // 1-based extruder ID
                    for (int extruder_id = 1; extruder_id <= int(by_extruder.size()); ++extruder_id) {
                        const ByExtruder &segmented = by_extruder[extruder_id - 1];
                        if (!segmented.bbox.defined || !parent_layer_region_bbox.overlap(segmented.bbox))
                            continue;

                        // Find the first target region iterator.
                        auto it_target_region = std::find_if(it_painted_region_begin, layer_range.painted_regions.cend(), [extruder_id](const auto &painted_region) {
                            return int(painted_region.extruder_id) >= extruder_id;
                        });

                        assert(it_target_region != layer_range.painted_regions.end());
                        assert(layer_range.volume_regions[it_target_region->parent].region == &parent_print_region && int(it_target_region->extruder_id) == extruder_id);

                        // Update the beginning PaintedRegion iterator for the next iteration.
                        it_painted_region_begin = it_target_region;

                        // FIXME: Don't trim by self, it is not reliable.
                        if (it_target_region->region == &parent_print_region) {
                            self_extruder_id = extruder_id;
                            continue;
                        }

                        // Steal from this region.
                        int        target_region_id = it_target_region->region->print_object_region_id();
                        ExPolygons stolen           = intersection_ex(parent_layer_region.slices.surfaces, segmented.expolygons);
                        if (!stolen.empty()) {
                            ByRegion &dst = by_region[target_region_id];
                            if (dst.expolygons.empty()) {
                                dst.expolygons = std::move(stolen);
                            } else {
                                append(dst.expolygons, std::move(stolen));
                                dst.needs_merge = true;
                            }
                        }
                    }

                    if (!self_trimmed) {
                        // Trim slices of this LayerRegion with all the MM regions.
                        Polygons mine = to_polygons(parent_layer_region.slices.surfaces);
                        for (auto &segmented : by_extruder) {
                            if (&segmented - by_extruder.data() + 1 != self_extruder_id && segmented.bbox.defined && parent_layer_region_bbox.overlap(segmented.bbox)) {
                                mine = diff(mine, segmented.expolygons);
                                if (mine.empty())
                                    break;
                            }
                        }

                        // Filter out unprintable polygons produced by subtraction multi-material painted regions from layerm.region().
                        // ExPolygon returned from multi-material segmentation does not precisely match ExPolygons in layerm.region()
                        // (because of preprocessing of the input regions in multi-material segmentation). Therefore, subtraction from
                        // layerm.region() could produce a huge number of small unprintable regions for the model's base extruder.
                        // This could, on some models, produce bulges with the model's base color (#7109).
                        if (!mine.empty()) {
                            mine = opening(union_ex(mine), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
                        }

                        if (!mine.empty()) {
                            ByRegion &dst = by_region[parent_print_region.print_object_region_id()];
                            if (dst.expolygons.empty()) {
                                dst.expolygons = union_ex(mine);
                            } else {
                                append(dst.expolygons, union_ex(mine));
                                dst.needs_merge = true;
                            }
                        }
                    }
                }

                // Re-create Surfaces of LayerRegions.
                for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
                    ByRegion &src = by_region[region_id];
                    if (src.needs_merge) {
                        // Multiple regions were merged into one.
                        src.expolygons = closing_ex(src.expolygons, scaled<float>(10. * EPSILON));
                    }

                    layer.get_region(region_id)->slices.set(std::move(src.expolygons), stInternal);
                }
            }
        });
}

template<typename ThrowOnCancel>
void apply_fuzzy_skin_segmentation(PrintObject &print_object, ThrowOnCancel throw_on_cancel)
{
    // Returns fuzzy skin segmentation based on painting in the fuzzy skin painting gizmo.
    std::vector<std::vector<ExPolygons>> segmentation = fuzzy_skin_segmentation_by_painting(print_object, throw_on_cancel);
    assert(segmentation.size() == print_object.layer_count());

    struct ByRegion
    {
        ExPolygons expolygons;
        bool       needs_merge { false };
    };

    tbb::parallel_for(tbb::blocked_range<size_t>(0, segmentation.size(), std::max(segmentation.size() / 128, size_t(1))), [&print_object, &segmentation, throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        const auto &layer_ranges   = print_object.shared_regions()->layer_ranges;
        auto        it_layer_range = layer_range_first(layer_ranges, print_object.get_layer(int(range.begin()))->slice_z);

        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            throw_on_cancel();

            Layer &layer = *print_object.get_layer(int(layer_idx));
            it_layer_range = layer_range_next(layer_ranges, it_layer_range, layer.slice_z);
            const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;

            assert(segmentation[layer_idx].size() == 1);
            const ExPolygons &fuzzy_skin_segmentation      = segmentation[layer_idx][0];
            const BoundingBox fuzzy_skin_segmentation_bbox = get_extents(fuzzy_skin_segmentation);
            if (fuzzy_skin_segmentation.empty())
                continue;

            // Split LayerRegions by painted fuzzy skin regions.
            // layer_range.fuzzy_skin_painted_regions are sorted by parent PrintObject region ID.
            std::vector<ByRegion> by_region(layer.region_count());
            auto                  it_fuzzy_skin_region_begin = layer_range.fuzzy_skin_painted_regions.cbegin();
            for (int parent_layer_region_idx = 0; parent_layer_region_idx < layer.region_count(); ++parent_layer_region_idx) {
                if (it_fuzzy_skin_region_begin == layer_range.fuzzy_skin_painted_regions.cend())
                    continue;

                const LayerRegion &parent_layer_region = *layer.get_region(parent_layer_region_idx);
                const PrintRegion &parent_print_region = parent_layer_region.region();
                assert(parent_print_region.print_object_region_id() == parent_layer_region_idx);
                if (parent_layer_region.slices.empty())
                    continue;

                // Find the first FuzzySkinPaintedRegion, which overrides the parent PrintRegion.
                auto it_fuzzy_skin_region = std::find_if(it_fuzzy_skin_region_begin, layer_range.fuzzy_skin_painted_regions.cend(), [&layer_range, &parent_print_region](const auto &fuzzy_skin_region) {
                    return fuzzy_skin_region.parent_print_object_region_id(layer_range) == parent_print_region.print_object_region_id();
                });

                if (it_fuzzy_skin_region == layer_range.fuzzy_skin_painted_regions.cend())
                    continue; // This LayerRegion isn't overrides by any FuzzySkinPaintedRegion.

                assert(it_fuzzy_skin_region->parent_print_object_region(layer_range) == &parent_print_region);

                // Update the beginning FuzzySkinPaintedRegion iterator for the next iteration.
                it_fuzzy_skin_region_begin = std::next(it_fuzzy_skin_region);

                const BoundingBox parent_layer_region_bbox        = get_extents(parent_layer_region.slices.surfaces);
                Polygons          layer_region_remaining_polygons = to_polygons(parent_layer_region.slices.surfaces);
                // Don't trim by self, it is not reliable.
                if (parent_layer_region_bbox.overlap(fuzzy_skin_segmentation_bbox) && it_fuzzy_skin_region->region != &parent_print_region) {
                    // Steal from this region.
                    const int  target_region_id = it_fuzzy_skin_region->region->print_object_region_id();
                    ExPolygons stolen           = intersection_ex(parent_layer_region.slices.surfaces, fuzzy_skin_segmentation);
                    if (!stolen.empty()) {
                        ByRegion &dst = by_region[target_region_id];
                        if (dst.expolygons.empty()) {
                            dst.expolygons = std::move(stolen);
                        } else {
                            append(dst.expolygons, std::move(stolen));
                            dst.needs_merge = true;
                        }
                    }

                    // Trim slices of this LayerRegion by the fuzzy skin region.
                    layer_region_remaining_polygons = diff(layer_region_remaining_polygons, fuzzy_skin_segmentation);

                    // Filter out unprintable polygons. Detailed explanation is inside apply_mm_segmentation.
                    if (!layer_region_remaining_polygons.empty()) {
                        layer_region_remaining_polygons = opening(union_ex(layer_region_remaining_polygons), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
                    }
                }

                if (!layer_region_remaining_polygons.empty()) {
                    ByRegion &dst = by_region[parent_print_region.print_object_region_id()];
                    if (dst.expolygons.empty()) {
                        dst.expolygons = union_ex(layer_region_remaining_polygons);
                    } else {
                        append(dst.expolygons, union_ex(layer_region_remaining_polygons));
                        dst.needs_merge = true;
                    }
                }
            }

            // Re-create Surfaces of LayerRegions.
            for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
                ByRegion &src = by_region[region_id];
                if (src.needs_merge) {
                    // Multiple regions were merged into one.
                    src.expolygons = closing_ex(src.expolygons, scaled<float>(10. * EPSILON));
                }

                layer.get_region(region_id)->slices.set(std::move(src.expolygons), stInternal);
            }
        }
    }); // end of parallel_for
}

// 1) Decides Z positions of the layers,
// 2) Initializes layers and their regions
// 3) Slices the object meshes
// 4) Slices the modifier meshes and reclassifies the slices of the object meshes by the slices of the modifier meshes
// 5) Applies size compensation (offsets the slices in XY plane)
// 6) Replaces bad slices by the slices reconstructed from the upper/lower layer
// Resulting expolygons of layer regions are marked as Internal.
//
// this should be idempotent
void PrintObject::slice_volumes()
{
    BOOST_LOG_TRIVIAL(info) << "Slicing volumes..." << log_memory_info();
    const Print *print                      = this->print();
    const auto   throw_on_cancel_callback   = std::function<void()>([print](){ print->throw_if_canceled(); });

    // Clear old LayerRegions, allocate for new PrintRegions.
    for (Layer* layer : m_layers) { //m_layers is global
        //BBS: should delete all LayerRegionPtr to avoid memory leak
        while (!layer->m_regions.empty()) {
            if (layer->m_regions.back())
                delete layer->m_regions.back();
            layer->m_regions.pop_back();
        }
        layer->m_regions.reserve(m_shared_regions->all_regions.size());
        for (const std::unique_ptr<PrintRegion> &pr : m_shared_regions->all_regions)
            layer->m_regions.emplace_back(new LayerRegion(layer, pr.get())); //this creates a "layer region"
    }

    std::vector<float>                   slice_zs      = zs_from_layers(m_layers);
    std::vector<VolumeSlices> objSliceByVolume;
    if (!slice_zs.empty()) {

       
        //what needs to be done is the object needs to be rotated in place temporarily
        // then the bounding box needs to be found to get the amount above or below the build plate when rotated
        // this is then used to find an offset to the rotation that allows the object to always have a first layer. Top bounds can be defined later
        // but we arent doing this since an easier method is probably to set min and max Z
        //float height_offset = this->model_object()->bounding_box_exact().transformed(m_post_trafo).min.z();




        //need this to deal with offcenter origins. It has been tested at various xy but not z
        // The authoritative slice frame (see PrintObject::object_to_slice()). It is post_trafo
        // pre-translated to center the object in XY, exactly like trafo_centered() and contour_z():
        // slice_volumes_inner must receive centered coordinates, because set_instances() adds
        // +m_center_offset to each instance's shift and G-code set_origin(shift) relies on the
        // slices being centered by -m_center_offset. Without it, a part whose rotated bounding-box
        // center is offset from its origin (e.g. after using the rotate tool) is placed off by
        // +m_center_offset between Prepare and Slice. Taken from the shared member rather than
        // rebuilt here so cascade geometry and the slicer cannot drift apart.
        const Transform3d &newTrafo = m_object_to_slice;

        objSliceByVolume = slice_volumes_inner(
            print->config(), this->config(), newTrafo,
            this->model_object()->volumes, m_shared_regions->layer_ranges, slice_zs, throw_on_cancel_callback, m_curved_field.get(),m_curved_mesh.get());

        // Declarative cut planes: confine this PrintObject's slices to its segment. Clipping a
        // solid by half-spaces and then slicing at z is identical to slicing the full solid and
        // intersecting each layer with the planes' 2D half-plane traces, so the mesh is never cut.
        // All volume slices (parts, negatives, modifiers) are clipped uniformly BEFORE
        // firstLayerObjSliceByVolume is copied, so brim/region/paint logic downstream sees
        // segment-confined geometry with no further changes.
        //
        // Part build directions: a segment owns either exactly one directed part or all the
        // undirected ones (segment_includes_volume). The volume list is shared by every segment of
        // the object, so the parts outside this segment were sliced too; they simply keep no
        // material here. Negative volumes and modifiers stay in every segment.
        if (this->is_segment()) {
            for (VolumeSlices &vs : objSliceByVolume) {
                const ModelVolume *mv = nullptr;
                for (const ModelVolume *candidate : this->model_object()->volumes)
                    if (candidate->id() == vs.volume_id) {
                        mv = candidate;
                        break;
                    }
                if (mv != nullptr && ! segment_includes_volume(&*m_segment, *mv))
                    for (ExPolygons &expolys : vs.slices)
                        expolys.clear();
            }
        }
        if (this->is_segment() && ! m_segment->clips.empty()) {
            // Clips are object-local; map them into the sliced frame q = A p + t (A = linear part
            // of newTrafo): n_s = A^-T n, d_s = d + n_s . t.
            struct SlicedClip { Vec3d n; double d; bool bed; bool floor; };
            std::vector<SlicedClip> sliced_clips;
            sliced_clips.reserve(m_segment->clips.size());
            const Eigen::Matrix3d a_inv_t = newTrafo.linear().inverse().transpose();
            for (const CutSegmentClip &clip : m_segment->clips) {
                const Vec3d n_s = a_inv_t * clip.normal;
                sliced_clips.push_back({ n_s, clip.offset + n_s.dot(newTrafo.translation()),
                                         clip.role == CutClipRole::Bed, clip.role == CutClipRole::Floor });
            }
            const auto is_part_slices = [this](const VolumeSlices &vs) {
                for (const ModelVolume *mv : this->model_object()->volumes)
                    if (mv->id() == vs.volume_id)
                        return mv->is_model_part();
                return false;
            };

            // UNLAYERED_MOD -- Layer::segment_floor_support, taken from the UNCLIPPED slices before
            // the clips below discard it: on each layer, the model material lying past this
            // segment's Floor clips, i.e. on the side of its base plane that printed earlier. Built
            // whether or not wave overhangs are on: switching them on only re-runs perimeters, not
            // slicing, so a slice-time gate would leave it empty exactly when it is needed.
            std::vector<ExPolygons> floor_support;
            bool has_floor_clip = false;
            for (const SlicedClip &clip : sliced_clips)
                has_floor_clip |= clip.floor;
            if (has_floor_clip) {
                floor_support.assign(slice_zs.size(), ExPolygons());
                tbb::parallel_for(tbb::blocked_range<size_t>(0, slice_zs.size()),
                    [&](const tbb::blocked_range<size_t> &range) {
                        for (size_t i = range.begin(); i < range.end(); ++ i) {
                            ExPolygons unclipped;
                            for (const VolumeSlices &vs : objSliceByVolume)
                                if (i < vs.slices.size() && is_part_slices(vs))
                                    append(unclipped, vs.slices[i]);
                            if (unclipped.empty())
                                continue;
                            unclipped = union_ex(unclipped);
                            const double z = double(slice_zs[i]);
                            ExPolygons   support;
                            for (const SlicedClip &clip : sliced_clips) {
                                if (! clip.floor)
                                    continue;
                                const Vec2d  ab(clip.n.x(), clip.n.y());
                                const double c = clip.d - clip.n.z() * z;
                                if (ab.norm() < 1e-9 * clip.n.norm()) {
                                    // Horizontal floor: the whole layer lies past it, or none of it.
                                    if (c < 0.)
                                        append(support, unclipped);
                                } else
                                    append(support, clip_expolygons_by_half_plane(ExPolygons(unclipped), -ab, -c, get_extents(unclipped)));
                            }
                            if (! support.empty())
                                floor_support[i] = union_ex(support);
                        }
                    });
            }
            // UNLAYERED_MOD -- margin for the Bed clip, per layer. The clip is evaluated on the
            // mid-layer slice plane. A bead spans h along the layer normal and w within the layer;
            // the walls along the clip line are inset w/2 in the plane, which already lifts their
            // bead by w/2 * sin(tilt) in world z, so what is left to cover is the h/2 * cos(tilt)
            // the bead reaches below its slice plane. Keeping world z >= h/2 * |cos(tilt)| at the
            // slice plane keeps every bead's bottom on or above the plate -- and costs a segment
            // built sideways (tilt 90) nothing. cos(tilt) is the plate normal's component along
            // this frame's z. The clip's offset is in world-z units whatever the instance scale
            // (its normal is the unnormalised third row of the instance matrix), so the margin
            // simply comes off d.
            double bed_cos_tilt = 0.;
            for (const SlicedClip &clip : sliced_clips)
                if (clip.bed && clip.n.norm() > 0.)
                    bed_cos_tilt = std::abs(clip.n.z()) / clip.n.norm();
            std::vector<double> bed_margin(m_layers.size());
            for (size_t i = 0; i < m_layers.size(); ++ i)
                bed_margin[i] = 0.5 * m_layers[i]->height * bed_cos_tilt;
            for (VolumeSlices &vs : objSliceByVolume)
                tbb::parallel_for(tbb::blocked_range<size_t>(0, vs.slices.size()),
                    [&vs, &sliced_clips, &slice_zs, &bed_margin](const tbb::blocked_range<size_t> &range) {
                        for (size_t i = range.begin(); i < range.end(); ++ i) {
                            ExPolygons &expolys = vs.slices[i];
                            const double z = double(slice_zs[i]);
                            for (const SlicedClip &clip : sliced_clips) {
                                if (expolys.empty())
                                    break;
                                const Vec2d ab(clip.n.x(), clip.n.y());
                                double c = clip.d - clip.n.z() * z;
                                const bool horizontal = ab.norm() < 1e-9 * clip.n.norm();
                                if (clip.bed) {
                                    // A plate that is horizontal in this frame means the segment
                                    // builds straight up, and its schedule already starts at the
                                    // plate (build_z_min is measured above it). Leave it alone
                                    // rather than risk dropping the first layer on a rounding tie
                                    // between its mid-layer z and the margin.
                                    if (horizontal)
                                        continue;
                                    c -= bed_margin[i];
                                    // Layer wholly above the plate: skip the Clipper round trip,
                                    // so a segment that merely comes NEAR the plate slices
                                    // byte-identically to before.
                                    const BoundingBox bb = get_extents(expolys);
                                    const double x0 = unscale<double>(bb.min.x()), x1 = unscale<double>(bb.max.x());
                                    const double y0 = unscale<double>(bb.min.y()), y1 = unscale<double>(bb.max.y());
                                    if (ab.x() * (ab.x() > 0. ? x1 : x0) + ab.y() * (ab.y() > 0. ? y1 : y0) <= c)
                                        continue;
                                }
                                if (horizontal) {
                                    // Horizontal plane in the sliced frame: this layer is wholly
                                    // kept or wholly dropped.
                                    if (c < 0.)
                                        expolys.clear();
                                } else
                                    expolys = clip_expolygons_by_half_plane(std::move(expolys), ab, c, get_extents(expolys));
                            }
                        }
                    });

            // UNLAYERED_MOD -- the plate can empty a tilted segment's first layers outright. Its
            // schedule was cropped to the material above the plate, so this is rare, but an empty
            // leading layer must go rather than stay: it prints nothing yet still emits a layer
            // marker (the reason the old per-layer bed clip was reverted), and everything below
            // assumes layer 0 has material. Dropped HERE, before the per-volume slices are copied
            // for brim grouping and handed to the regions, so every per-layer array stays aligned.
            if (m_segment->has_bed_clip()) {
                size_t lead = 0;
                for (; lead + 1 < slice_zs.size(); ++ lead) {
                    bool empty = true;
                    for (const VolumeSlices &vs : objSliceByVolume)
                        if (lead < vs.slices.size() && ! vs.slices[lead].empty() && is_part_slices(vs)) {
                            empty = false;
                            break;
                        }
                    if (! empty)
                        break;
                }
                if (lead > 0) {
                    for (VolumeSlices &vs : objSliceByVolume)
                        vs.slices.erase(vs.slices.begin(), vs.slices.begin() + std::min(lead, vs.slices.size()));
                    slice_zs.erase(slice_zs.begin(), slice_zs.begin() + lead);
                    if (! floor_support.empty())
                        floor_support.erase(floor_support.begin(), floor_support.begin() + lead);
                    for (size_t i = 0; i < lead; ++ i)
                        delete m_layers[i];
                    m_layers.erase(m_layers.begin(), m_layers.begin() + lead);
                    m_layers.front()->lower_layer = nullptr;
                    for (Layer *layer : m_layers)
                        layer->set_id(layer->id() - lead);
                    BOOST_LOG_TRIVIAL(info) << "cut planes: segment " << this->segment_index() << " dropped " << lead
                                            << " leading layer(s) lying wholly under the build plate";
                }
            }

            for (size_t i = 0; i < floor_support.size() && i < m_layers.size(); ++ i)
                m_layers[i]->segment_floor_support = std::move(floor_support[i]);
        }
    }

    // UNLAYERED_MOD -- the build plate as a clip for a SUNK object with a whole-object layer rotation
    // (see update_slice_frame). Keeps world z >= h/2 * cos(tilt) at each slice plane, so no bead
    // reaches below the plate, the same margin as a cut segment's Bed clip. Leading layers it empties
    // are dropped HERE, before the per-volume slices are copied for brim grouping and handed to the
    // regions, so every per-layer array stays aligned. An object resting on the plate skips all this
    // and keeps its exact old toolpaths.
    if (tilted_and_sunk(*this) && ! m_layers.empty()) {
        Vec3d  n_bed;
        double k_bed;
        this->bed_plane_in_slice_frame(n_bed, k_bed);
        for (VolumeSlices &vs : objSliceByVolume)
            tbb::parallel_for(tbb::blocked_range<size_t>(0, vs.slices.size()),
                [this, &vs, &slice_zs, n_bed, k_bed](const tbb::blocked_range<size_t> &range) {
                    for (size_t i = range.begin(); i < range.end() && i < slice_zs.size(); ++ i) {
                        if (vs.slices[i].empty())
                            continue;
                        const double margin = 0.5 * m_layers[i]->height * std::abs(n_bed.z());
                        vs.slices[i] = intersection_ex(vs.slices[i],
                            SegmentSupportBounds::half_plane_at(n_bed, margin - k_bed, slice_zs[i], true));
                    }
                });
        const auto is_part_slices = [this](const VolumeSlices &vs) {
            for (const ModelVolume *mv : this->model_object()->volumes)
                if (mv->id() == vs.volume_id)
                    return mv->is_model_part();
            return false;
        };
        size_t lead = 0;
        for (; lead + 1 < slice_zs.size(); ++ lead) {
            bool empty = true;
            for (const VolumeSlices &vs : objSliceByVolume)
                if (lead < vs.slices.size() && ! vs.slices[lead].empty() && is_part_slices(vs)) {
                    empty = false;
                    break;
                }
            if (! empty)
                break;
        }
        if (lead > 0) {
            for (VolumeSlices &vs : objSliceByVolume)
                vs.slices.erase(vs.slices.begin(), vs.slices.begin() + std::min(lead, vs.slices.size()));
            slice_zs.erase(slice_zs.begin(), slice_zs.begin() + lead);
            for (size_t i = 0; i < lead; ++ i)
                delete m_layers[i];
            m_layers.erase(m_layers.begin(), m_layers.begin() + lead);
            m_layers.front()->lower_layer = nullptr;
            for (Layer *layer : m_layers)
                layer->set_id(layer->id() - lead);
            BOOST_LOG_TRIVIAL(info) << "layer rotation: dropped " << lead << " leading layer(s) lying wholly under the build plate";
        }
    }

    //BBS: "model_part" volumes are grouded according to their connections
    //const auto           scaled_resolution = scaled<double>(print->config().resolution.value);
    //firstLayerObjSliceByVolume = findPartVolumes(objSliceByVolume, this->model_object()->volumes);
    //groupingVolumes(objSliceByVolumeParts, firstLayerObjSliceByGroups, scaled_resolution);
    //applyNegtiveVolumes(this->model_object()->volumes, objSliceByVolume, firstLayerObjSliceByGroups, scaled_resolution);
    firstLayerObjSliceByVolume = objSliceByVolume;

    std::vector<std::vector<ExPolygons>> region_slices =
        slices_to_regions(print->config(), *this, this->model_object()->volumes, *m_shared_regions, slice_zs,
                          std::move(objSliceByVolume), PrintObject::clip_multipart_objects, throw_on_cancel_callback);

    for (size_t region_id = 0; region_id < region_slices.size(); ++ region_id) {
        std::vector<ExPolygons> &by_layer = region_slices[region_id];
        for (size_t layer_id = 0; layer_id < by_layer.size(); ++ layer_id)
            m_layers[layer_id]->regions()[region_id]->slices.append(std::move(by_layer[layer_id]), stInternal);
    }
    region_slices.clear();

    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - removing top empty layers";
    // A cut-plane segment sliced past its own material (segment_slice_top()) keeps its empty top
    // layers on purpose: they hold the support column carrying a later segment whose cut face
    // rises above this one. G-code export tolerates support-only and trailing empty layers; it
    // only objects to extrusions stacked over a gap.
    const bool keep_empty_top = this->is_segment() && this->segment_slice_top() > m_segment->build_z_max + EPSILON;
    while (! keep_empty_top && ! m_layers.empty()) {
        const Layer *layer = m_layers.back();
        if (! layer->empty())
            break;
        delete layer;
        m_layers.pop_back();
    }
    if (! m_layers.empty())
        m_layers.back()->upper_layer = nullptr;

    // A segment that sliced to nothing is an anomaly, not a normal outcome: its descriptor claims
    // material over a range the slicer then found empty. Downstream code (brim preparation, the
    // support generators) assumes at least one layer, so say so loudly rather than letting it
    // surface as a crash somewhere else.
    if (this->is_segment() && m_layers.empty())
        BOOST_LOG_TRIVIAL(warning) << "cut planes: segment " << this->segment_index()
                                   << " sliced to ZERO layers over build range ["
                                   << m_segment->build_z_min << ", " << m_segment->build_z_max
                                   << "], base-plane gap " << m_segment->base_plane_distance_below_material;

    // Declarative cut planes: segments now slice over their OWN extent (the slice frame is rebased
    // so the segment's minimum lands at z=0, see update_slice_frame), so the run of empty leading
    // layers this used to strip should no longer exist. A remaining one means the rebase and the
    // clipped geometry disagree -- surface it rather than papering over it.
    assert(! this->is_segment() || m_layers.empty() || ! m_layers.front()->empty());
    m_print->throw_if_canceled();

    this->apply_conical_overhang();

    // Is any ModelVolume multi-material painted?
    if (const auto& volumes = this->model_object()->volumes;
        m_print->config().filament_diameter.size() > 1 && // BBS
        std::find_if(volumes.begin(), volumes.end(), [](const ModelVolume* v) { return !v->mmu_segmentation_facets.empty(); }) != volumes.end()) {

        // If XY Size compensation is also enabled, notify the user that XY Size compensation
        // would not be used because the object is multi-material painted.
        if (m_config.xy_hole_compensation.value != 0.f || m_config.xy_contour_compensation.value != 0.f) {
            this->active_step_add_warning(
                PrintStateBase::WarningLevel::CRITICAL,
                L("An object's XY size compensation will not be used because it is also color-painted.\nXY Size "
                  "compensation cannot be combined with color-painting."));
            BOOST_LOG_TRIVIAL(info) << "xy compensation will not work for object " << this->model_object()->name << " for multi filament.";
        }

        BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - MMU segmentation";
        apply_mm_segmentation(*this, [print]() { print->throw_if_canceled(); });
    }

    // Is any ModelVolume fuzzy skin painted?
    if (this->model_object()->is_fuzzy_skin_painted()) {
        // If XY Size compensation is also enabled, notify the user that XY Size compensation
        // would not be used because the object has custom fuzzy skin painted.
        if (m_config.xy_hole_compensation.value != 0.f || m_config.xy_contour_compensation.value != 0.f) {
            this->active_step_add_warning(
                PrintStateBase::WarningLevel::CRITICAL,
                _u8L("An object has enabled XY Size compensation which will not be used because it is also fuzzy skin painted.\nXY Size "
                     "compensation cannot be combined with fuzzy skin painting.") +
                    "\n" + (_u8L("Object name")) + ": " + this->model_object()->name);
        }

        BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - Fuzzy skin segmentation";
        apply_fuzzy_skin_segmentation(*this, [print]() { print->throw_if_canceled(); });
    }

    InterlockingGenerator::generate_interlocking_structure(this, [print]() { print->throw_if_canceled(); });
    m_print->throw_if_canceled();

    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - make_slices in parallel - begin";
    {
        // Compensation value, scaled. Only applying the negative scaling here, as the positive scaling has already been applied during slicing.
        const size_t num_extruders = print->config().filament_diameter.size();
        const auto   xy_hole_scaled = (num_extruders > 1 && this->is_mm_painted()) ? scaled<float>(0.f) : scaled<float>(m_config.xy_hole_compensation.value);
        const auto   xy_contour_scaled            = (num_extruders > 1 && this->is_mm_painted()) ? scaled<float>(0.f) : scaled<float>(m_config.xy_contour_compensation.value);
        const float  elephant_foot_compensation_scaled = (m_config.raft_layers == 0 && this->is_root_segment()) ?
        	// Only enable Elephant foot compensation if printing directly on the print bed.
            // (A non-root cut-plane segment rests on the segment below it, never on the bed.)
            float(scale_(m_config.elefant_foot_compensation.value)) :
        	0.f;
        // Uncompensated slices for the layers in case the Elephant foot compensation is applied.
        std::vector<ExPolygons> lslices_elfoot_uncompensated;
        lslices_elfoot_uncompensated.resize(elephant_foot_compensation_scaled > 0 ? std::min(m_config.elefant_foot_compensation_layers.value, (int)m_layers.size()) : 0);
        // BELT_MOD -- build-plate clip, belt printers only; see the use inside the loop below.
        //
        // An object sunk below the plate is sliced in the belt's tilted frame from its own minimum, so
        // without this the sunk part was emitted below the belt (measured: walls 1.3 mm under the belt
        // for an object dropped 1.5 mm). The plane is PrintObject::bed_plane_in_slice_frame(), the same
        // one a belt object's support floor uses (post_trafo_inverse(), so it cannot disagree with what
        // is written).
        //
        // Deliberately NOT applied off a belt: there a sunk root keeps its sinking and is never
        // sliced below the plate (CutSegmentDescriptor::slice_z_base), and a tilted segment reaching
        // the plate is bounded by its own cut clips. The per-layer clip that used to run here for
        // every non-planar print left empty layer markers at the bottom and was removed.
        Vec3d  n_bed;
        double k_bed;
        this->bed_plane_in_slice_frame(n_bed, k_bed);
        const bool   bed_clip = print->config().belt_printer.value;
        //BBS: this part has been changed a lot to support seperated contour and hole size compensation
	    tbb::parallel_for(
	        tbb::blocked_range<size_t>(0, m_layers.size()),
			[this, xy_hole_scaled, xy_contour_scaled, elephant_foot_compensation_scaled, &lslices_elfoot_uncompensated, bed_clip, n_bed, k_bed](const tbb::blocked_range<size_t>& range) {
	            for (size_t layer_id = range.begin(); layer_id < range.end(); ++ layer_id) {
	                m_print->throw_if_canceled();
	                Layer *layer = m_layers[layer_id];
	                // Apply size compensation and perform clipping of multi-part objects.
	                float elfoot = elephant_foot_compensation_scaled > 0 && layer_id < m_config.elefant_foot_compensation_layers.value ? 
                        elephant_foot_compensation_scaled - (elephant_foot_compensation_scaled / m_config.elefant_foot_compensation_layers.value) * layer_id : 
                        0.f;
	                if (layer->m_regions.size() == 1) {
	                    // Optimized version for a single region layer.
	                    // Single region, growing or shrinking.
	                    LayerRegion *layerm = layer->m_regions.front();
                        if (elfoot > 0) {
		                    // Apply the elephant foot compensation and store the original layer slices without the Elephant foot compensation applied.
                            ExPolygons expolygons_to_compensate = to_expolygons(std::move(layerm->slices.surfaces));
                            if (xy_contour_scaled > 0 || xy_hole_scaled > 0) {
                                expolygons_to_compensate = _shrink_contour_holes(std::max(0.f, xy_contour_scaled),
                                                                   std::max(0.f, xy_hole_scaled),
                                                                   expolygons_to_compensate);
                            }
                            if (xy_contour_scaled < 0 || xy_hole_scaled < 0) {
                                expolygons_to_compensate = _shrink_contour_holes(std::min(0.f, xy_contour_scaled),
                                                                   std::min(0.f, xy_hole_scaled),
                                                                   expolygons_to_compensate);
                            }
                            lslices_elfoot_uncompensated[layer_id] = expolygons_to_compensate;
							layerm->slices.set(
								union_ex(
									Slic3r::elephant_foot_compensation(expolygons_to_compensate,
	                            		layerm->flow(frExternalPerimeter), unscale<double>(elfoot))),
								stInternal);
	                    } else {
	                        // Apply the XY contour and hole size compensation.
                            if (xy_contour_scaled != 0.0f || xy_hole_scaled != 0.0f) {
                                ExPolygons expolygons = to_expolygons(std::move(layerm->slices.surfaces));
                                if (xy_contour_scaled > 0 || xy_hole_scaled > 0) {
                                    expolygons = _shrink_contour_holes(std::max(0.f, xy_contour_scaled),
                                                                       std::max(0.f, xy_hole_scaled),
                                                                       expolygons);
                                }
                                if (xy_contour_scaled < 0 || xy_hole_scaled < 0) {
                                    expolygons = _shrink_contour_holes(std::min(0.f, xy_contour_scaled),
                                                                       std::min(0.f, xy_hole_scaled),
                                                                       expolygons);
                                }
                                layerm->slices.set(std::move(expolygons), stInternal);
                            }
	                    }
	                } else {
                        float max_growth = std::max(xy_hole_scaled, xy_contour_scaled);
                        float min_growth = std::min(xy_hole_scaled, xy_contour_scaled);
                        ExPolygons merged_poly_for_holes_growing;
                        if (max_growth > 0) {
                            //BBS: merge polygons because region can cut "holes".
                            //Then, cut them to give them again to their region
                            merged_poly_for_holes_growing = layer->merged(float(SCALED_EPSILON));
                            merged_poly_for_holes_growing = _shrink_contour_holes(std::max(0.f, xy_contour_scaled),
                                                                                  std::max(0.f, xy_hole_scaled),
                                                                                  union_ex(merged_poly_for_holes_growing));

                            // BBS: clipping regions, priority is given to the first regions.
                            Polygons processed;
                            for (size_t region_id = 0; region_id < layer->regions().size(); ++region_id) {
                                ExPolygons slices = to_expolygons(std::move(layer->m_regions[region_id]->slices.surfaces));
                                if (max_growth > 0.f) {
                                    slices = intersection_ex(offset_ex(slices, max_growth), merged_poly_for_holes_growing);
                                }
                                


                                //BBS: Trim by the slices of already processed regions.
                                if (region_id > 0)
                                    slices = diff_ex(to_polygons(std::move(slices)), processed);
                                if (region_id + 1 < layer->regions().size())
                                    // Collect the already processed regions to trim the to be processed regions.
                                    polygons_append(processed, slices);
                                layer->m_regions[region_id]->slices.set(std::move(slices), stInternal);
                            }
                        }
                        if (min_growth < 0.f || elfoot > 0.f) {
                            // Apply the negative XY compensation. (the ones that is <0)
                            ExPolygons trimming;
                            static const float eps = float(scale_(m_config.slice_closing_radius.value) * 1.5);
                            if (elfoot > 0.f) {
                                ExPolygons expolygons_to_compensate = offset_ex(layer->merged(eps), -eps);
                                lslices_elfoot_uncompensated[layer_id] = expolygons_to_compensate;
                                trimming = Slic3r::elephant_foot_compensation(expolygons_to_compensate,
                                    layer->m_regions.front()->flow(frExternalPerimeter), unscale<double>(elfoot));
                            } else {
                                trimming = layer->merged(float(SCALED_EPSILON));
                            }
                            if (min_growth < 0.0f)
                                trimming = _shrink_contour_holes(std::min(0.f, xy_contour_scaled),
                                                                 std::min(0.f, xy_hole_scaled),
                                                                 trimming);
                            //BBS: trim surfaces
                            for (size_t region_id = 0; region_id < layer->regions().size(); ++region_id) {
                                // BBS: split trimming result by region
                                ExPolygons contour_exp = to_expolygons(std::move(layer->regions()[region_id]->slices.surfaces));

                                layer->regions()[region_id]->slices.set(intersection_ex(contour_exp, to_polygons(trimming)), stInternal);
                            }
                        }
	                }
	                // BELT_MOD -- drop whatever would print below the belt (see the plane above). Done here,
	                // before Layer::make_slices(), so islands, perimeters, infill, supports and the G-code all
	                // see the same clipped geometry. print_z, not slice_z: the emitter places extrusions at the
	                // layer's print_z.
	                if (bed_clip) {
	                    // Plate horizontal in this slice frame and the whole layer above it: settled without
	                    // touching Clipper.
	                    const bool whole_layer_above = n_bed.head<2>().norm() < 1e-9
	                                                && n_bed.z() * layer->print_z + k_bed >= 0.;
	                    if (! whole_layer_above) {
	                        const Polygons above = SegmentSupportBounds::half_plane_at(n_bed, - k_bed, layer->print_z, true);
	                        for (LayerRegion *layerm : layer->m_regions)
	                            layerm->slices.set(intersection_ex(to_expolygons(std::move(layerm->slices.surfaces)), above), stInternal);
	                    }
	                }
	                // Merge all regions' slices to get islands, chain them by a shortest path.
	                layer->make_slices();

	            }
	        });
	    if (elephant_foot_compensation_scaled > 0.f && ! m_layers.empty()) {
	    	// The Elephant foot has been compensated, therefore the elefant_foot_compensation_layers layer's lslices are shrank with the Elephant foot compensation value.
	    	// Store the uncompensated value there.
	    	assert(m_layers.front()->id() == 0);
            //BBS: sort the lslices_elfoot_uncompensated according to shortest path before saving
            //Otherwise the travel of the layer layer would be mess.
            for (int i = 0; i < lslices_elfoot_uncompensated.size(); i++) {
                ExPolygons &expolygons_uncompensated = lslices_elfoot_uncompensated[i];
                Points ordering_points;
                ordering_points.reserve(expolygons_uncompensated.size());
                for (const ExPolygon &ex : expolygons_uncompensated)
                    ordering_points.push_back(ex.contour.first_point());
                std::vector<Points::size_type> order = chain_points(ordering_points);
                ExPolygons lslices_sorted;
                lslices_sorted.reserve(expolygons_uncompensated.size());
                for (size_t i : order)
                    lslices_sorted.emplace_back(std::move(expolygons_uncompensated[i]));
                m_layers[i]->lslices = std::move(lslices_sorted);
            }
		}
	}

    m_print->throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - make_slices in parallel - end";
}

void PrintObject::apply_conical_overhang() {
    BOOST_LOG_TRIVIAL(info) << "Make overhang printable...";

    if (m_layers.empty()) {
        return;
    }
    
    const double conical_overhang_angle = this->config().make_overhang_printable_angle;
    if (conical_overhang_angle == 90.0) {
        return;
    }
    const double angle_radians = conical_overhang_angle * M_PI / 180.;
    const double max_hole_area = this->config().make_overhang_printable_hole_size; // in MM^2
    const double tan_angle = tan(angle_radians); // the XY-component of the angle
    BOOST_LOG_TRIVIAL(info) << "angle " << angle_radians << " maxHoleArea " << max_hole_area << " tan_angle "
                            << tan_angle;
    const coordf_t layer_thickness = m_config.layer_height.value;
    const coordf_t max_dist_from_lower_layer = tan_angle * layer_thickness; // max dist which can be bridged, in MM
    BOOST_LOG_TRIVIAL(info) << "layer_thickness " << layer_thickness << " max_dist_from_lower_layer "
                            << max_dist_from_lower_layer;

    // Pre-scale config
    const coordf_t scaled_max_dist_from_lower_layer = -float(scale_(max_dist_from_lower_layer));
    const coordf_t scaled_max_hole_area = float(scale_(scale_(max_hole_area)));


    for (auto i = m_layers.rbegin() + 1; i != m_layers.rend(); ++i) {
        m_print->throw_if_canceled();
        Layer *layer = *i;
        Layer *upper_layer = layer->upper_layer;

        if (upper_layer->empty()) {
          continue;
        }

        // Skip if entire layer has this disabled
        if (std::all_of(layer->m_regions.begin(), layer->m_regions.end(),
                        [](const LayerRegion *r) { return  r->slices.empty() || !r->region().config().make_overhang_printable; })) {
            continue;
        }

        //layer->export_region_slices_to_svg_debug("layer_before_conical_overhang");
        //upper_layer->export_region_slices_to_svg_debug("upper_layer_before_conical_overhang");


        // Merge the upper layer because we want to offset the entire layer uniformly, otherwise
        // the model could break at the region boundary.
        auto upper_poly = upper_layer->merged(float(SCALED_EPSILON));
        upper_poly = union_ex(upper_poly);

        // Merge layer for the same reason
        auto current_poly = layer->merged(float(SCALED_EPSILON));
        current_poly = union_ex(current_poly);

        // Avoid closing up of recessed holes in the base of a model.
        // Detects when a hole is completely covered by the layer above and removes the hole from the layer above before
        // adding it in.
        // This should have no effect any time a hole in a layer interacts with any polygon in the layer above
        if (scaled_max_hole_area > 0.0) {

            // Now go through all the holes in the current layer and check if they intersect anything in the layer above
            // If not, then they're the top of a hole and should be cut from the layer above
            for (auto layer_polygon : current_poly) {
                for (auto hole : layer_polygon.holes) {
                    if (std::abs(hole.area()) < scaled_max_hole_area) {
                        ExPolygon hole_poly(hole);
                        auto hole_with_above = intersection_ex(upper_poly, hole_poly);
                        if (!hole_with_above.empty()) {
                            // The hole had some intersection with the above layer, check if it's a complete overlap
                            auto hole_difference = xor_ex(hole_with_above, hole_poly);
                            if (hole_difference.empty()) {
                                // The layer above completely cover it, remove it from the layer above
                                upper_poly = diff_ex(upper_poly, hole_poly);
                            }
                        }
                    }
                }
            }
        }

        // Now offset the upper layer to be added into current layer
        upper_poly = offset_ex(upper_poly, scaled_max_dist_from_lower_layer);

        for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
            // export_to_svg(debug_out_path("Surface-obj-%d-layer-%d-region-%d.svg", id().id, layer->id(), region_id).c_str(),
            //               layer->m_regions[region_id]->slices.surfaces);

            // Disable on given region
            if (!upper_layer->m_regions[region_id]->region().config().make_overhang_printable) {
                continue;
            }

            // Calculate the scaled upper poly that belongs to current region
            auto p = union_ex(intersection_ex(upper_layer->m_regions[region_id]->slices.surfaces, upper_poly));

            // Remove all islands that have already been fully covered by current layer
            p.erase(std::remove_if(p.begin(), p.end(), [&current_poly](const ExPolygon& ex) {
                return diff_ex(ex, current_poly).empty();
            }), p.end());

            // And now union it with current region
            ExPolygons layer_polygons = to_expolygons(layer->m_regions[region_id]->slices.surfaces);
            layer->m_regions[region_id]->slices.set(union_ex(layer_polygons, p), stInternal);

            // Then remove it from all other regions, to avoid overlapping regions
            for (size_t other_region = 0; other_region < this->num_printing_regions(); ++other_region) {
                if (other_region == region_id) {
                    continue;
                }
                ExPolygons s = to_expolygons(layer->m_regions[other_region]->slices.surfaces);
                layer->m_regions[other_region]->slices.set(diff_ex(s, p, ApplySafetyOffset::Yes), stInternal);
            }
        }
        //layer->export_region_slices_to_svg_debug("layer_after_conical_overhang");
    }
}

//BBS: this function is used to offset contour and holes of expolygons seperately by different value
ExPolygons PrintObject::_shrink_contour_holes(double contour_delta, double hole_delta, const ExPolygons& polys) const
{
    ExPolygons new_ex_polys;
    for (const ExPolygon& ex_poly : polys) {
        Polygons contours;
        Polygons holes;
        //BBS: modify hole
        for (const Polygon& hole : ex_poly.holes) {
            if (hole_delta != 0) {
                for (Polygon& newHole : offset(hole, -hole_delta)) {
                    newHole.make_counter_clockwise();
                    holes.emplace_back(std::move(newHole));
                }
            } else {
                holes.push_back(hole);
                holes.back().make_counter_clockwise();
            }
        }
        //BBS: modify contour
        if (contour_delta != 0) {
            Polygons new_contours = offset(ex_poly.contour, contour_delta);
            if (new_contours.size() == 0)
                continue;
            contours.insert(contours.end(), std::make_move_iterator(new_contours.begin()), std::make_move_iterator(new_contours.end()));
        } else {
            contours.push_back(ex_poly.contour);
        }
        ExPolygons temp = diff_ex(union_(contours), union_(holes));
        new_ex_polys.insert(new_ex_polys.end(), std::make_move_iterator(temp.begin()), std::make_move_iterator(temp.end()));
    }
    return union_ex(new_ex_polys);
}

std::vector<Polygons> PrintObject::slice_support_volumes(const ModelVolumeType model_volume_type) const
{
    auto it_volume     = this->model_object()->volumes.begin();
    auto it_volume_end = this->model_object()->volumes.end();
    for (; it_volume != it_volume_end && (*it_volume)->type() != model_volume_type; ++ it_volume) ;
    std::vector<Polygons> slices;
    if (it_volume != it_volume_end) {
        // Found at least a single support volume of model_volume_type.
        std::vector<float> zs = zs_from_layers(this->layers());
        std::vector<char>  merge_layers;
        bool               merge = false;
        const Print       *print = this->print();
        auto               throw_on_cancel_callback = std::function<void()>([print](){ print->throw_if_canceled(); });
        MeshSlicingParamsEx params;
        // UNLAYERED_MOD -- the painted volumes must be cut in the SAME frame as the model parts
        // (slice_volumes uses object_to_slice(), which carries the multiplanar rotation and the
        // segment rebase); trafo_centered() has neither, so on a rotated object or segment the
        // enforcers and blockers landed somewhere else entirely.
        params.trafo = this->object_to_slice();
        for (; it_volume != it_volume_end; ++ it_volume)
            if ((*it_volume)->type() == model_volume_type) {
                std::vector<ExPolygons> slices2 = slice_volume(*(*it_volume), zs, params, throw_on_cancel_callback);
                if (slices.empty()) {
                    slices.reserve(slices2.size());
                    for (ExPolygons &src : slices2)
                        slices.emplace_back(to_polygons(std::move(src)));
                } else if (!slices2.empty()) {
                    if (merge_layers.empty())
                        merge_layers.assign(zs.size(), false);
                    for (size_t i = 0; i < zs.size(); ++ i) {
                        if (slices[i].empty())
                            slices[i] = to_polygons(std::move(slices2[i]));
                        else if (! slices2[i].empty()) {
                            append(slices[i], to_polygons(std::move(slices2[i])));
                            merge_layers[i] = true;
                            merge = true;
                        }
                    }
                }
            }
        if (merge) {
            std::vector<Polygons*> to_merge;
            to_merge.reserve(zs.size());
            for (size_t i = 0; i < zs.size(); ++ i)
                if (merge_layers[i])
                    to_merge.emplace_back(&slices[i]);
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, to_merge.size()),
                [&to_merge](const tbb::blocked_range<size_t> &range) {
                    for (size_t i = range.begin(); i < range.end(); ++ i)
                        *to_merge[i] = union_(*to_merge[i]);
            });
        }
        // UNLAYERED_MOD -- while support is generated in a sheared frame the painted volumes must
        // follow the layers they are matched against. zs is indexed 1:1 with layers().
        if (m_support_lean.active())
            for (size_t i = 0; i < slices.size() && i < m_layers.size(); ++ i) {
                const Point shift = m_support_lean.shift_at(m_layers[i]->print_z);
                for (Polygon &p : slices[i])
                    p.translate(shift);
            }
    }
    return slices;
}

} // namespace Slic3r
