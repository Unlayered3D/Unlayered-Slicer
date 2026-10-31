#include "PlatePrintability.hpp"

#include "I18N.hpp"
#include "PartPlate.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include <boost/format.hpp>

namespace Slic3r {
namespace GUI {

const char* instance_placement_name(InstancePlacement placement)
{
    switch (placement) {
    case InstancePlacement::Printable: return "printable";
    case InstancePlacement::OffPlate: return "offPlate";
    case InstancePlacement::OutsideBed: return "outsideBed";
    case InstancePlacement::ExcludeArea: return "excludeArea";
    case InstancePlacement::Disabled: return "notPrintable";
    }
    return "unknown";
}

const char* print_volume_state_name(ModelInstanceEPrintVolumeState state)
{
    // calc_print_volume_state only yields these three.
    switch (state) {
    case ModelInstancePVS_Inside: return "inside";
    case ModelInstancePVS_Partly_Outside: return "partlyOutside";
    default: return "fullyOutside";
    }
}

// ModelInstance::calc_print_volume_state, with a shortcut for convex beds that are not rectangles
// (round, convex, or a custom outline that happens to be convex). There the exact test runs per
// triangle; but when a volume's transformed convex-hull box lies inside the bed outline and between
// the bed surface and the printable height, the mesh is inside too, so only volumes near the rim or
// the top need the exact test. Rectangular beds already use a box test; beds split into extruder
// areas and non-convex outlines always take the exact test.
static ModelInstanceEPrintVolumeState instance_print_volume_state(const ModelInstance& instance, const BuildVolume& bed)
{
    const BuildVolume_Type type = bed.type();
    const Polygon&         outline = bed.polygon();
    if (type != BuildVolume_Type::Rectangle && type != BuildVolume_Type::Invalid && bed.extruder_areas().size() <= 1 &&
        outline.size() >= 3 && polygon_is_convex(outline)) {
        const double         eps        = BuildVolume::SceneEpsilon;
        const BoundingBoxf3& bed_volume = bed.bounding_volume();
        bool                 any_part   = false;
        bool                 all_inside = true;
        for (const ModelVolume* vol : instance.get_object()->volumes) {
            if (!vol->is_model_part())
                continue;
            const BoundingBoxf3 raw  = vol->get_convex_hull().bounding_box();
            const Vec3d         size = raw.size();
            if (size.x() == 0. || size.y() == 0. || size.z() == 0.)
                continue; // skipped by calc_print_volume_state as well
            any_part              = true;
            const BoundingBoxf3 bb = vol->get_convex_hull().transformed_bounding_box(instance.get_matrix() * vol->get_matrix());
            if (bb.min.z() < -eps || bb.max.z() > bed_volume.max.z() - eps ||
                !outline.contains(Point::new_scale(bb.min.x(), bb.min.y())) || !outline.contains(Point::new_scale(bb.max.x(), bb.min.y())) ||
                !outline.contains(Point::new_scale(bb.max.x(), bb.max.y())) || !outline.contains(Point::new_scale(bb.min.x(), bb.max.y()))) {
                all_inside = false;
                break;
            }
        }
        if (any_part && all_inside)
            return ModelInstancePVS_Inside;
    }
    return instance.calc_print_volume_state(bed);
}

// The instance's footprint overlaps one of the plate's exclude areas, as PartPlate::check_outside
// tests it. Used to name the cause once check_outside has refused the instance.
static bool overlaps_exclude_area(PartPlate& plate, ModelInstance& instance)
{
    const std::vector<BoundingBoxf3>& areas = plate.get_exclude_areas();
    if (areas.empty())
        return false;
    const Polygon hull = instance.convex_hull_2d();
    for (const BoundingBoxf3& area : areas)
        if (!intersection(Polygons{area.polygon(true)}, Polygons{hull}).empty())
            return true;
    return false;
}

PlatePrintability assess_plate_printability(PartPlateList& plates, const Model& model)
{
    PlatePrintability result;

    int    width = 0, depth = 0;
    double height = 0.;
    plates.get_plate_size(width, depth, height);
    const int plate_count = std::max(plates.get_plate_count(), 0);

    std::vector<BuildVolume> beds(static_cast<size_t>(plate_count));
    for (int i = 0; i < plate_count; ++i)
        if (PartPlate* plate = plates.get_plate(i); plate != nullptr)
            beds[size_t(i)] = BuildVolume(plate->get_shape(), height, plate->get_extruder_areas(), plate->get_extruder_heights());
    result.plates.assign(static_cast<size_t>(plate_count), PlateCounts{});
    result.instances.resize(model.objects.size());

    for (size_t obj_idx = 0; obj_idx < model.objects.size(); ++obj_idx) {
        ModelObject* object = model.objects[obj_idx];
        result.instances[obj_idx].resize(object->instances.size());
        for (size_t inst_idx = 0; inst_idx < object->instances.size(); ++inst_idx) {
            InstancePrintability& out = result.instances[obj_idx][inst_idx];
            ++result.instance_count;

            PartPlate* plate = nullptr;
            for (int i = 0; i < plate_count && plate == nullptr; ++i)
                if (PartPlate* p = plates.get_plate(i); p != nullptr && p->contain_instance(int(obj_idx), int(inst_idx))) {
                    plate     = p;
                    out.plate = i;
                }
            if (plate == nullptr) {
                out.placement = InstancePlacement::OffPlate;
                ++result.off_plate_count;
                continue;
            }

            PlateCounts&   counts   = result.plates[size_t(out.plate)];
            ModelInstance& instance = *object->instances[inst_idx];
            ++counts.instances;
            out.state        = instance_print_volume_state(instance, beds[size_t(out.plate)]);
            BoundingBoxf3 bb = object->instance_convex_hull_bounding_box(inst_idx);
            // Outside the bed shape or height first; an exclude area is named only for an instance the
            // shape test accepts.
            if (out.state != ModelInstancePVS_Inside)
                out.placement = InstancePlacement::OutsideBed;
            else if (plate->check_outside(int(obj_idx), int(inst_idx), &bb))
                out.placement = overlaps_exclude_area(*plate, instance) ? InstancePlacement::ExcludeArea : InstancePlacement::OutsideBed;
            else if (!object->printable || !instance.printable)
                out.placement = InstancePlacement::Disabled;
            else
                out.placement = InstancePlacement::Printable;

            switch (out.placement) {
            case InstancePlacement::Printable: ++counts.printable; ++result.printable_count; break;
            case InstancePlacement::ExcludeArea: ++counts.exclude_area; break;
            case InstancePlacement::Disabled: ++counts.disabled; break;
            default: ++counts.outside_bed; break;
            }
        }
    }
    return result;
}

bool explain_nothing_printable(const PlatePrintability& assessment, int plate, std::string& reason, std::string& message)
{
    PlateCounts counts;
    if (plate >= 0) {
        if (plate < int(assessment.plates.size()))
            counts = assessment.plates[size_t(plate)];
    } else {
        for (const PlateCounts& c : assessment.plates) {
            counts.instances += c.instances;
            counts.printable += c.printable;
            counts.outside_bed += c.outside_bed;
            counts.exclude_area += c.exclude_area;
            counts.disabled += c.disabled;
        }
    }
    if (counts.printable > 0)
        return false;

    // Whole translatable sentences only (with plural forms), joined with spaces.
    auto sentence = [&message](const std::string& s) {
        if (!message.empty())
            message += " ";
        message += s;
    };
    auto counted = [](const char* one, const char* many, int n) {
        return (boost::format(std::string(I18N::translate(one, many, unsigned(n)).ToUTF8().data())) % n).str();
    };
    message.clear();

    if (counts.instances == 0) {
        reason = "empty_plate";
        sentence(plate >= 0 ? _u8L("There is no object on this plate.") : _u8L("There is no object on any plate."));
        if (assessment.off_plate_count > 0)
            sentence(counted(L("%1% object is outside every plate. Move or arrange it onto a plate."),
                             L("%1% objects are outside every plate. Move or arrange them onto a plate."), assessment.off_plate_count));
        return true;
    }

    const int causes = int(counts.outside_bed > 0) + int(counts.exclude_area > 0) + int(counts.disabled > 0);
    reason           = causes > 1 ? "nothing_printable" :
                       counts.outside_bed > 0 ? "outside_bed" :
                       counts.exclude_area > 0 ? "exclude_area" : "not_printable";

    sentence(plate >= 0 ? _u8L("Nothing on this plate can be printed.") : _u8L("Nothing on any plate can be printed."));
    if (counts.outside_bed > 0)
        sentence(counted(L("%1% object is outside the printable area of the bed. Arrange or move it onto the bed."),
                         L("%1% objects are outside the printable area of the bed. Arrange or move them onto the bed."), counts.outside_bed));
    if (counts.exclude_area > 0)
        sentence(counted(L("%1% object overlaps an excluded area of the bed. Move it off the excluded area."),
                         L("%1% objects overlap an excluded area of the bed. Move them off the excluded area."), counts.exclude_area));
    if (counts.disabled > 0)
        sentence(counted(L("%1% object is set not printable. Mark it printable to print it."),
                         L("%1% objects are set not printable. Mark them printable to print them."), counts.disabled));
    return true;
}

std::vector<int> refresh_plate_outside_status(PartPlateList& plates, const Model& model)
{
    std::vector<int> changed;
    const int        plate_count = plates.get_plate_count();
    for (int i = 0; i < plate_count; ++i) {
        PartPlate* plate = plates.get_plate(i);
        if (plate == nullptr)
            continue;
        bool plate_changed = false;
        for (size_t obj_idx = 0; obj_idx < model.objects.size(); ++obj_idx) {
            const ModelObject* object = model.objects[obj_idx];
            for (size_t inst_idx = 0; inst_idx < object->instances.size(); ++inst_idx) {
                const int o = int(obj_idx), n = int(inst_idx);
                if (!plate->contain_instance(o, n))
                    continue;
                // One check_outside per instance: update the status, then compare.
                const bool    was_inside = plate->contain_instance_totally(o, n);
                BoundingBoxf3 bb         = object->instance_convex_hull_bounding_box(inst_idx);
                plate->update_instance_exclude_status(o, n, &bb);
                plate_changed = plate_changed || plate->contain_instance_totally(o, n) != was_inside;
            }
        }
        if (plate_changed) {
            // What PartPlateList::notify_instance_update does for a moved instance, once per plate.
            plate->update_states();
            plate->update_slice_result_valid_state(false);
            plate->thumbnail_data.reset();
            plate->no_light_thumbnail_data.reset();
            plate->top_thumbnail_data.reset();
            plate->pick_thumbnail_data.reset();
            changed.push_back(i);
        }
    }
    return changed;
}

} // namespace GUI
} // namespace Slic3r
