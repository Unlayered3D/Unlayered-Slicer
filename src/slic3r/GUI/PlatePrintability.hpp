#ifndef slic3r_GUI_PlatePrintability_hpp_
#define slic3r_GUI_PlatePrintability_hpp_

// One verdict on "what on the plates would actually be printed", computed fresh from the model and
// the current bed. Shared by slicer.getPlate, the slice / export / send refusals and their
// messages, so they cannot disagree.
//
// An instance is printable when all of these hold:
//  - it is assigned to a plate (PartPlate::contain_instance);
//  - the object and the instance are marked printable;
//  - ModelInstance::calc_print_volume_state against the plate's real bed shape (round, convex,
//    custom, rectangular) is Inside -- the test slicing uses (ModelInstance::is_printable());
//  - PartPlate::check_outside is false for it, evaluated now: the plate list's own test (the one
//    the Slice button is gated on), with the bed exclude areas, the bed bottom and sinking objects.
// An instance entirely below the bed surface never meets a plate's volume, so it is not on any
// plate (OffPlate).
// The assessment does not change what is on the plates or any setting; it may fill the model's
// lazily computed caches (convex hulls), as any other read of them does.

#include <string>
#include <vector>

#include "libslic3r/Model.hpp"

namespace Slic3r {
namespace GUI {

class PartPlateList;

enum class InstancePlacement : unsigned char
{
    Printable,   // will be printed
    OffPlate,    // not on any plate
    OutsideBed,  // (partly) outside the bed shape or above the printable height
    ExcludeArea, // inside the bed shape, but overlapping an exclude area of the bed
    Disabled,    // object or instance set not printable
};

const char* instance_placement_name(InstancePlacement placement);
// "inside", "partlyOutside" or "fullyOutside".
const char* print_volume_state_name(ModelInstanceEPrintVolumeState state);

struct InstancePrintability
{
    int                            plate     = -1;
    ModelInstanceEPrintVolumeState state     = ModelInstancePVS_Fully_Outside;
    InstancePlacement              placement = InstancePlacement::OffPlate;
    bool printable() const { return placement == InstancePlacement::Printable; }
};

struct PlateCounts
{
    int instances    = 0; // assigned to the plate
    int printable    = 0;
    int outside_bed  = 0;
    int exclude_area = 0;
    int disabled     = 0;
};

struct PlatePrintability
{
    // [object][instance]
    std::vector<std::vector<InstancePrintability>> instances;
    // [plate]
    std::vector<PlateCounts> plates;
    int instance_count  = 0;
    int printable_count = 0;
    int off_plate_count = 0;

    bool plate_has_printable(int plate) const { return plate >= 0 && plate < int(plates.size()) && plates[size_t(plate)].printable > 0; }
    bool any_plate_has_printable() const { return printable_count > 0; }
};

// Evaluate every instance of `model` against the plates of `plates`. GUI thread only.
// Convex beds that are not rectangles take a bounding-box shortcut for instances well inside the bed
// and only run the per-triangle test near the rim, so this stays cheap enough to call on every request.
PlatePrintability assess_plate_printability(PartPlateList& plates, const Model& model);

// When the plate (or every plate, plate < 0) has nothing printable: a short machine-readable reason
// ("empty_plate", "outside_bed", "exclude_area", "not_printable", or "nothing_printable" for a mix)
// and an explanation for a person, in the current UI language (UTF-8). Returns false when there is
// something to print.
bool explain_nothing_printable(const PlatePrintability& assessment, int plate, std::string& reason, std::string& message);

// Bring the plates' cached outside status (PartPlate::instance_outside_set, which the Slice button
// and has_printable_instances() read) up to date with the current exclude areas, for every instance
// already on a plate; plate membership is left alone. Only plates where some instance's status
// changed get their state updated, their slice result invalidated and their thumbnails reset;
// returns those plate indices.
std::vector<int> refresh_plate_outside_status(PartPlateList& plates, const Model& model);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_PlatePrintability_hpp_
