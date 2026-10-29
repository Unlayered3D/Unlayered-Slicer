#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <catch2/catch_all.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Support/SegmentSupportBounds.hpp"
#include "libslic3r/BeltTransform.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>
#include <fstream>
#include <iterator>

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("Changing the number of solid shell layers does not make all surfaces internal", "[Print]") {
    GIVEN("sliced 20mm cube and config with top_shell_layers = 2 and bottom_shell_layers = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
            { "top_shell_layers",           2 },
            { "bottom_shell_layers",        1 },
            { "layer_height",               0.25 }, // get a known number of layers
            { "initial_layer_print_height", 0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({cube(20)}, print, model, config);
        // Precondition: Ensure that the model has 2 solid top layers (79, 78)
        // and one solid bottom layer (0).
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    // iterate over all of the regions in the layer
		    for (const LayerRegion *region : layer.regions()) {
		        // for each region, iterate over the fill surfaces
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0); // should be solid
        test_is_solid_infill(0, 79); // should be solid
        test_is_solid_infill(0, 78); // should be solid
        WHEN("Model is re-sliced with top_shell_layers == 3") {
			config.set("top_shell_layers", 3);
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Print::validate() warning collection
//
// validate() returns its warnings in a vector. The warning paths deliberately
// differ in how many entries they produce; these tests pin down each behaviour:
//   * independent checks    -> stack (one entry each)
//   * motion-ability        -> coalesce into one (mutually exclusive, gated)
//   * clumping detection    -> one independent warning
//   * layered clearance     -> many collisions concatenated into one entry
//   * null warnings pointer -> no-op, no crash, no blocking error
// ---------------------------------------------------------------------------
namespace {

// Build `n` 20mm cubes (spread apart, or stacked at the origin when `overlap`) into
// `model`/`print` and apply `config`, leaving the print ready to validate(). No slicing needed.
void build_cubes(Slic3r::Model& model, Slic3r::Print& print,
                 DynamicPrintConfig config, int n, bool overlap)
{
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n")); // validate() relative-E reset

    for (int i = 0; i < n; ++i) {
        ModelObject* object = model.add_object();
        object->add_volume(cube(20));
        ModelInstance* inst = object->add_instance();
        inst->set_offset(Vec3d(overlap ? 0.0 : i * 60.0, 0.0, 0.0));
    }
    for (ModelObject* mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
}

// Build cubes and run validate(), collecting warnings; returns the blocking error.
StringObjectException validate_cubes(const DynamicPrintConfig& config,
                                     std::vector<StringObjectException>& warnings,
                                     int n = 1, bool overlap = false)
{
    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, n, overlap);
    return print.validate(&warnings);
}

size_t count_opt_key(const std::vector<StringObjectException>& warnings, const std::string& key)
{
    return std::count_if(warnings.begin(), warnings.end(),
        [&](const StringObjectException& w) { return w.opt_key == key; });
}

// Make `default_acceleration` exceed the machine's extruding-acceleration limit.
void trigger_acceleration_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats{ 100. });
    c.set_key_value("default_acceleration", new ConfigOptionFloatsNullable{ 100000. });
}

// Make `default_jerk` exceed the machine's jerk limit (junction deviation off so
// the jerk check is not skipped).
void trigger_jerk_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats{ 0. });
    c.set_key_value("machine_max_jerk_x", new ConfigOptionFloats{ 1. });
    c.set_key_value("machine_max_jerk_y", new ConfigOptionFloats{ 1. });
    c.set_key_value("default_jerk", new ConfigOptionFloatsNullable{ 9999. });
}

// Precise outer wall is ignored unless the wall sequence is inner-outer.
void trigger_precise_wall_warning(DynamicPrintConfig& c)
{
    c.set_key_value("precise_outer_wall", new ConfigOptionBool(true));
    c.set_key_value("wall_sequence", new ConfigOptionEnum<WallSequence>(WallSequence::OuterInner));
}

} // namespace

// ---------------------------------------------------------------------------
// {first_object_name} filename placeholder
// ---------------------------------------------------------------------------
namespace {

// Add a printable 20mm cube named `name` to `model`; returns it so the caller can tweak it.
ModelObject* add_named_cube(Model& model, const std::string& name)
{
    ModelObject* obj = model.add_object();
    obj->name = name;
    obj->add_volume(make_cube(20.0, 20.0, 20.0));
    obj->add_instance();
    obj->ensure_on_bed();
    return obj;
}

// Resolve `format` to an output file name for a print of `model`. `filename_base`, when set,
// is the saved-project name passed to output_filename().
std::string resolved_output_name(Model& model, const std::string& format, const std::string& filename_base = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filename_format", new ConfigOptionString(format));

    Print print;
    for (ModelObject* obj : model.objects)
        print.auto_assign_extruders(obj);
    print.apply(model, config);
    return print.output_filename(filename_base);
}

} // namespace

TEST_CASE("Print: {first_object_name} names the first printable object on the plate", "[Print]")
{
    Model model;

    SECTION("uses the object's name") {
        add_named_cube(model, "WidgetPart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "WidgetPart.gcode");
    }

    SECTION("picks the first when several objects are printable") {
        add_named_cube(model, "FirstPart");
        add_named_cube(model, "SecondPart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "FirstPart.gcode");
    }

    SECTION("skips objects outside the print volume (e.g. on another plate)") {
        // First in model order, but not on the current plate, so is_printable() is false.
        add_named_cube(model, "OtherPlatePart")->instances.front()->print_volume_state = ModelInstancePVS_Fully_Outside;
        add_named_cube(model, "OnPlatePart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "OnPlatePart.gcode");
    }

    SECTION("is empty when the object has no name") {
        add_named_cube(model, "");
        CHECK(resolved_output_name(model, "part_{first_object_name}") == "part_.gcode");
    }
}

TEST_CASE("Print: {first_object_name} is not replaced by the saved-project file name", "[Print]")
{
    // Passing a saved-project file name as the filename_base must not change {first_object_name}.
    Model model;
    add_named_cube(model, "WidgetPart");
    CHECK(resolved_output_name(model, "{first_object_name}", "SavedProject") == "WidgetPart.gcode");
}

TEST_CASE("Print::validate stacks independent warnings", "[Print][validate]")
{
    // Two unrelated checks (region precise-wall + machine acceleration) must each
    // contribute their own entry.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(warnings.size() >= 2);
    CHECK(count_opt_key(warnings, "precise_outer_wall") == 1);  // jump-to key is preserved
    for (const auto& w : warnings)
        CHECK(w.is_warning);                                   // every collected entry is a warning
}

TEST_CASE("Print::validate coalesces motion-ability warnings into one", "[Print][validate]")
{
    // The jerk/junction/acceleration checks are mutually exclusive (gated on a shared
    // key), so adding a second motion trigger must NOT add a second warning.
    DynamicPrintConfig accel_only = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_only);
    std::vector<StringObjectException> w_accel;
    CHECK(validate_cubes(accel_only, w_accel).string.empty());

    DynamicPrintConfig accel_and_jerk = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_and_jerk);
    trigger_jerk_warning(accel_and_jerk);
    std::vector<StringObjectException> w_both;
    CHECK(validate_cubes(accel_and_jerk, w_both).string.empty());

    CHECK(w_accel.size() >= 1);
    CHECK(w_both.size() == w_accel.size());  // the extra motion trigger collapses into the same warning
}

TEST_CASE("Print::validate reports the clumping-detection warning", "[Print][validate]")
{
    // A distinct single-shot path: clumping/wrapping detection without a prime tower warns
    // (and carries the enable_prime_tower jump-to key). enable_prime_tower must be off, as
    // the warning lives in the no-prime-tower branch.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(true));

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(count_opt_key(warnings, "enable_prime_tower") == 1);
}

TEST_CASE("Print::validate concatenates layered-clearance collisions into one warning", "[Print][validate]")
{
    // In by-layer mode, layered_print_cleareance_valid folds every too-close pair into a
    // single warning entry (newline-joined), unlike the per-check stacking above. Isolate
    // that entry by type so unrelated default-config warnings don't affect the assertion.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings, /*n=*/3, /*overlap=*/true);

    CHECK(err.string.empty());
    auto is_layered = [](const StringObjectException& w) {
        return w.type == STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT; };
    REQUIRE(std::count_if(warnings.begin(), warnings.end(), is_layered) == 1);  // 3 objects, 2 collisions, 1 entry
    auto it = std::find_if(warnings.begin(), warnings.end(), is_layered);
    CHECK(it->string.find('\n') != std::string::npos);  // the collisions were concatenated
}

TEST_CASE("Print::validate tolerates a null warnings pointer", "[Print][validate]")
{
    // Callers may pass no warnings sink: a warning-producing config must not crash
    // and must still return without a blocking error.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, /*n=*/1, /*overlap=*/false);

    StringObjectException err = print.validate();  // warnings == nullptr
    CHECK(err.string.empty());
}

TEST_CASE("A default slice emits perimeter, infill, and skirt", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 } // keep recorded Z at the printed height
    });
    CHECK(role_passes(gcode, "perimeter") > 0);
    CHECK(role_passes(gcode, "infill")    > 0);
    CHECK(role_passes(gcode, "skirt")     > 0);
    CHECK_THAT(max_z(gcode), Catch::Matchers::WithinAbs(20.0, 1e-4));
}

// The G-code carries a config-comment block describing the resolved settings. The
// per-region width lines are always present; the support and first-layer lines appear
// only when those features are configured.
TEST_CASE("G-code lists the resolved extrusion-width settings", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, { { "initial_layer_line_width", 0 } });
    CHECK(gcode.find("; external perimeters extrusion width") != std::string::npos);
    CHECK(gcode.find("; perimeters extrusion width")          != std::string::npos);
    CHECK(gcode.find("; infill extrusion width")              != std::string::npos);
    CHECK(gcode.find("; solid infill extrusion width")        != std::string::npos);
    CHECK(gcode.find("; top infill extrusion width")          != std::string::npos);
    CHECK(gcode.find("; support material extrusion width")    == std::string::npos);
    CHECK(gcode.find("; first layer extrusion width")         == std::string::npos);
    CHECK(gcode.find("; layer_height")                        != std::string::npos);
    CHECK(gcode.find("; sparse_infill_density")               != std::string::npos);

    const std::string with_support = slice({ cube(20) }, {
        { "initial_layer_line_width", 0 }, { "enable_support", true }, { "raft_layers", 3 },
    });
    CHECK(with_support.find("; support material extrusion width") != std::string::npos);

    const std::string with_first_layer = slice({ cube(20) }, { { "initial_layer_line_width", "0.5" } });
    CHECK(with_first_layer.find("; first layer extrusion width") != std::string::npos);
}

// gcode_skip_config_block suppresses the resolved-settings block while leaving the
// header and executable blocks intact.
TEST_CASE("gcode_skip_config_block omits the resolved-settings comment block", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "gcode_skip_config_block", true },
        { "gcode_comments",         true },
    });
    CHECK(gcode.find("; CONFIG_BLOCK_START")     == std::string::npos);
    CHECK(gcode.find("; CONFIG_BLOCK_END")       == std::string::npos);
    CHECK(gcode.find("; layer_height =")         == std::string::npos);
    CHECK(gcode.find("; fill_density =")         == std::string::npos);
    CHECK(gcode.find("; HEADER_BLOCK_START")     != std::string::npos);
    CHECK(gcode.find("; EXECUTABLE_BLOCK_START") != std::string::npos);
}

// Custom G-code templates substitute placeholders during export.
TEST_CASE("Custom G-code placeholders are substituted", "[Print]")
{
    // [current_extruder] in the start G-code.
    CHECK(slice({ cube(20) }, { { "machine_start_gcode", "; Extruder [current_extruder]" } })
              .find("; Extruder 0") != std::string::npos);

    // [layer_num] / [layer_z] in the end G-code (a 20mm cube at 0.1mm is 200 layers).
    const std::string end_gcode = slice({ cube(20) }, {
        { "machine_end_gcode",          "; Layer_num [layer_num]\n; Layer_z [layer_z]" },
        { "layer_height",               0.1 },
        { "initial_layer_print_height", 0.1 },
    });
    CHECK(end_gcode.find("; Layer_num 199") != std::string::npos);
    CHECK(end_gcode.find("; Layer_z 20")    != std::string::npos);

    // printing_by_object_gcode is emitted between sequentially printed objects.
    CHECK(slice_two_cubes_arranged({
                    { "print_sequence",           "by object" },
                    { "printing_by_object_gcode", "; between-object-gcode" },
                })
              .find("; between-object-gcode") != std::string::npos);

    // [layer_num] keeps counting across sequentially printed objects (199 then 399).
    const std::string per_layer = slice_two_cubes_arranged({
        { "print_sequence",             "by object" },
        { "layer_change_gcode",         ";Layer:[layer_num] ([layer_z] mm)" },
        { "layer_height",               0.1 },
        { "initial_layer_print_height", 0.1 },
    });
    CHECK(per_layer.find(";Layer:199 ") != std::string::npos);
    CHECK(per_layer.find(";Layer:399 ") != std::string::npos);
}

TEST_CASE("export_gcode writes G-code without a result pointer", "[Print][export_gcode]")
{
    Print print;
    Model model;
    Slic3r::Test::init_print({cube(20)}, print, model);
    print.process();

    SECTION("non-BBL printer") {}
    SECTION("BBL printer") { print.is_BBL_printer() = true; }

    ScopedTemporaryFile temp(".gcode");
    REQUIRE_NOTHROW(print.export_gcode(temp.string(), nullptr, nullptr));

    std::ifstream in(temp.string());
    const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE_FALSE(gcode.empty());
}

TEST_CASE("Sequential printing follows model order", "[Print]")
{
    // Two objects of different heights, taller one added first. Orca prints
    // sequential objects in model order, so the taller one is printed first.
    const std::string gcode = Slic3r::Test::slice({ cube(20), Slic3r::make_cube(20, 20, 10) }, {
        { "print_sequence",             "by object" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 }
    });

    // The first object's height is the peak Z reached before Z drops back to the
    // first layer (the object change). With by-object printing only an object
    // change returns Z to the bottom.
    double first_object_peak_z = 0.0;
    double running_peak        = 0.0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (first_object_peak_z != 0.0 || !line.extruding(self)) return; // ignore travels (e.g. start-gcode Z lift)
        if (running_peak > 1.0 && self.z() < 1.0)
            first_object_peak_z = running_peak;
        else
            running_peak = std::max(running_peak, static_cast<double>(self.z()));
    });

    REQUIRE_THAT(first_object_peak_z, Catch::Matchers::WithinAbs(20.0, 0.3));
}

// A sequential (by-object) print must publish the print-level nozzle group result just
// like a by-layer print, so custom g-code can index the per-nozzle placeholder tables
// (e.g. nozzle_diameter_at_nozzle_id[]) instead of failing on an empty vector.
TEST_CASE("Sequential printing publishes the nozzle group result", "[Print][MultiNozzle]")
{
    SECTION("process() publishes the result") {
        Print print;
        Model model;
        place_two_cubes_apart(60.0, { { "print_sequence", "by object" } }, print, model);
        print.process();
        REQUIRE(print.get_layered_nozzle_group_result() != nullptr);
    }

    SECTION("start g-code can index the per-nozzle diameter table") {
        const std::string gcode = slice_two_cubes_arranged({
            { "print_sequence",      "by object" },
            { "machine_start_gcode", "{if nozzle_diameter_at_nozzle_id[0] > 0}; SEQ-ND-OK\n{endif}" },
        });
        CHECK(gcode.find("; SEQ-ND-OK") != std::string::npos);
    }
}

// BELT_MOD -- belt printer: the belt tilt is the machine's build direction, so an ordinary
// object slices rotated by it (PrintObject::update_slice_frame via layer_rotation_trafo), with
// the sliced height taken from the same rotated frame (PrintObject::update_slicing_parameters).
static DynamicPrintConfig belt_print_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "belt_printer",               1 },
        { "belt_slice_rotation",        "x" },
        { "belt_slice_rotation_angle",  45 },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops",                0 },
        { "brim_type",                  "no_brim" },
        { "enable_prime_tower",         0 },
    });
    return config;
}

TEST_CASE("Belt printer: an ordinary object slices along the tilted gantry", "[Print][belt]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    build_cubes(model, print, belt_print_config(), /*n=*/1, /*overlap=*/false);
    {
        const StringObjectException err = print.validate();
        INFO("validate: " << err.string);
        REQUIRE(err.string.empty());
    }
    print.process();

    const PrintObject &object = *print.objects().front();
    // A 20 mm cube tilted 45 degrees spans 20 (sin 45 + cos 45) = 28.28 mm along the build normal.
    CHECK(object.slicing_parameters().object_print_z_max == Catch::Approx(20. * std::sqrt(2.)).margin(0.05));
    CHECK(object.layers().size() >= 140);
    CHECK(object.layers().size() <= 142);
    // The frame puts the leading tip on slice z = 0 and the schedule ends at the trailing tip, so
    // there are no empty layers at either end.
    for (const Layer *layer : object.layers())
        CHECK(! layer->lslices.empty());
    CHECK_FALSE(print.has_skirt());
    CHECK_FALSE(print.has_brim());
    CHECK_FALSE(print.has_wipe_tower());
}

// BELT_MOD -- a multi-part object. PrintObjectSlice decides which part is present at a layer
// from per-volume bounding boxes measured in the UNROTATED instance frame; a belt object's layers
// run along the tilted build direction, so on a belt those boxes must not be consulted (the
// slices themselves are asked instead, as for cut-plane segments). Measured on a two-part Benchy:
// every layer above its untilted height came out empty.
TEST_CASE("Belt printer: a multi-part object keeps its material above its untilted height", "[Print][belt][multipart]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    Slic3r::Print print;
    Slic3r::Model model;
    {
        ModelObject *object = model.add_object();
        // Two 20 mm cubes stacked: 40 mm tall, 20 mm deep. Tilted 45 degrees about X the object
        // spans (40 + 20) sin 45 = 42.4 mm along the build direction, well past 40.
        object->add_volume(cube(20));
        object->add_volume(cube(20))->translate(0., 0., 20.);   // add_volume centers the mesh; move it, don't place it
        object->add_instance()->set_offset(Vec3d(50., 50., 0.));
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
        print.apply(model, config);
    }
    print.set_status_silent();
    REQUIRE(print.validate().string.empty());
    print.process();

    const PrintObject &object = *print.objects().front();
    REQUIRE(object.model_object()->volumes.size() == 2);
    CHECK(object.slicing_parameters().object_print_z_max == Catch::Approx(60. * std::sin(M_PI / 4.)).margin(0.05));
    double last_nonempty = 0.;
    for (const Layer *layer : object.layers())
        if (! layer->lslices.empty())
            last_nonempty = layer->print_z;
    // Material all the way to the trailing tip, not cut off at the untilted 40 mm.
    CHECK(last_nonempty > 42.);
    for (const Layer *layer : object.layers())
        CHECK(! layer->lslices.empty());
}

TEST_CASE("Belt printer: validation refuses what the port does not support yet", "[Print][belt][validate]")
{
    std::vector<StringObjectException> warnings;
    {
        // Non-planar printing composes with the belt now (multi-planar belt): a plain cube with the
        // switch on is just a belt object.
        DynamicPrintConfig c = belt_print_config();
        c.set_deserialize_strict({ { "enable_nonplanar", 1 } });
        CHECK(validate_cubes(c, warnings).string.empty());
    }
    {
        DynamicPrintConfig c = belt_print_config();
        c.set_deserialize_strict({ { "spiral_mode", 1 } });
        CHECK_FALSE(validate_cubes(c, warnings).string.empty());
    }
    {
        // The slicing angle is the print's; it may stray from the machine's tilt only within the
        // printer's tolerance (default 10 degrees).
        DynamicPrintConfig c = belt_print_config();
        c.set_deserialize_strict({ { "belt_slice_rotation_angle", 52 }, { "belt_frame_tilt_angle", 45 } });
        CHECK(validate_cubes(c, warnings).string.empty());
        c.set_deserialize_strict({ { "belt_slice_rotation_angle", 60 } });
        CHECK(validate_cubes(c, warnings).opt_key == "belt_slice_rotation_angle");
        c.set_deserialize_strict({ { "belt_slice_angle_tolerance", 20 } });
        CHECK(validate_cubes(c, warnings).string.empty());
        c.set_deserialize_strict({ { "belt_slice_rotation_angle", 0 } });
        CHECK_FALSE(validate_cubes(c, warnings).string.empty());
    }
    {
        // Two objects interleave in belt order under the by-layer sequence (the default); any other
        // sequence would drive the belt backwards between them and is refused.
        CHECK(validate_cubes(belt_print_config(), warnings, /*n=*/2, /*overlap=*/false).string.empty());
        DynamicPrintConfig c = belt_print_config();
        c.set_deserialize_strict({ { "print_sequence", "by object" } });
        CHECK_FALSE(validate_cubes(c, warnings, /*n=*/2, /*overlap=*/false).string.empty());
    }
    {
        // A brim on a belt is the belt brim (BeltBrim.cpp): accepted without a word.
        DynamicPrintConfig c = belt_print_config();
        c.set_deserialize_strict({ { "brim_type", "outer_only" }, { "brim_width", 5 } });
        warnings.clear();
        CHECK(validate_cubes(c, warnings).string.empty());
        CHECK(count_opt_key(warnings, "brim_type") == 0);
        // A leading apron with a brim-object gap is warned about: the gap would detach the apron.
        c.set_deserialize_strict({ { "leading_brim_length", 6 }, { "brim_object_gap", 0.2 } });
        warnings.clear();
        CHECK(validate_cubes(c, warnings).string.empty());
        CHECK(count_opt_key(warnings, "brim_object_gap") == 1);
    }
    {
        // The same cube on a Cartesian printer is untouched by any of this.
        DynamicPrintConfig c = belt_print_config();
        c.set_deserialize_strict({ { "belt_printer", 0 } });
        warnings.clear();
        CHECK(validate_cubes(c, warnings, /*n=*/2, /*overlap=*/false).string.empty());
    }
}

// BELT_MOD -- support on a belt: the belt is a tilted plane in the object's slice frame, so support
// must be clipped to the belt side of it and carried down onto it (SegmentSupportBounds with the
// belt as the only floor), exactly as for a non-root cut-plane segment.
static TriangleMesh belt_tee_mesh()
{
    // 10 x 10 post, 16.5 tall, under a 30 x 10 x 4 top plate overhanging 10 mm each side.
    TriangleMesh top  = make_cube(30., 10., 4.);
    top.translate(-15.f, -5.f, 16.f);
    TriangleMesh post = make_cube(10., 10., 16.5);
    post.translate(-5.f, -5.f, 0.f);
    indexed_triangle_set its = top.its;
    its_merge(its, post.its);
    return TriangleMesh(its);
}

static void check_belt_support(const std::string &support_type, const std::string &support_style, bool on_plate_only, bool expect_extension)
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "enable_support",              1 },
        { "support_type",                support_type },
        { "support_style",               support_style },
        { "support_threshold_angle",     30 },
        { "support_on_build_plate_only", on_plate_only ? 1 : 0 },
        { "layer_change_gcode",          "G92 E0\n" },
    });
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({belt_tee_mesh()}, print, model, config);
    {
        const StringObjectException err = print.validate();
        INFO("validate: " << err.string);
        REQUIRE(err.string.empty());
    }
    print.process();

    const PrintObject &object = *print.objects().front();
    REQUIRE(object.support_floor_is_tilted());
    const SegmentSupportBounds bounds(object, object.slicing_parameters().first_print_layer_height);
    REQUIRE(bounds.is_active());
    REQUIRE_FALSE(object.support_layers().empty());

    double min_support_z = std::numeric_limits<double>::max();
    for (const SupportLayer *layer : object.support_layers()) {
        min_support_z = std::min(min_support_z, layer->print_z);
        // Nothing may be laid on the far side of the belt.
        const Polygons material = layer->support_fills.polygons_covered_by_spacing(0.f);
        if (material.empty())
            continue;
        const Polygons allowed = bounds.valid_region_polygon(layer->print_z);
        const double outside   = area(diff(material, allowed));
        CAPTURE(layer->print_z, outside);
        CHECK(outside < scale_(0.5) * scale_(0.5));
    }
    // The overhangs' columns reach the belt, which lies BELOW the slice frame's zero over most of
    // the footprint: the floor extension must have produced layers with negative print_z.
    std::ostringstream diag;
    {
        std::vector<const SupportLayer*> sl(object.support_layers().begin(), object.support_layers().end());
        std::sort(sl.begin(), sl.end(), [](const SupportLayer *a, const SupportLayer *b) { return a->print_z < b->print_z; });
        for (size_t i = 0; i < std::min<size_t>(4, sl.size()); ++ i) {
            const SupportLayer *l = sl[i];
            diag << "\n  z=" << l->print_z << " base_areas=" << l->base_areas.size() << " islands=" << l->support_islands.size()
                 << " fills=" << l->support_fills.entities.size();
            const ExPolygons &src = l->base_areas.empty() ? l->support_islands : l->base_areas;
            if (! src.empty()) {
                double deepest = 0.;
                for (const ExPolygon &ex : src) for (const Point &pt : ex.contour.points) deepest = std::min(deepest, bounds.floor_z(pt));
                diag << " floor_min=" << deepest;
            }
        }
    }
    INFO("support layers: " << object.support_layers().size() << ", lowest print_z " << min_support_z << diag.str());
    if (expect_extension)
        CHECK(min_support_z < -1.);
}

TEST_CASE("Belt printer: support is clipped to the belt and carried down onto it", "[Print][belt][support]")
{
    SECTION("normal support")       { check_belt_support("normal(auto)", "default",     false, true); }
    // The tree styles only get the belt-side clip asserted here. The classic tree rests its branches
    // on the T's post (hybrid) or fails to route around the tilted post when forced to the plate --
    // a pre-existing classic-tree limitation in rotated frames -- and organic is non-deterministic
    // on a part this small ("Potentially lost branch"). The CLI corpus (tee30x10 on the BabyBelt Pro
    // presets) covers organic's floor extension down to the belt.
    SECTION("classic tree support") { check_belt_support("tree(auto)",   "tree_hybrid", false, false); }
    SECTION("organic tree support") { check_belt_support("tree(auto)",   "organic",     false, false); }
}

// BELT_MOD -- contact-aware first layer. On a belt every slice layer has a strip lying on the belt,
// and that strip -- not just slice layer 0 -- must get the first-layer speed. Counted per
// ;LAYER_CHANGE block with the feedrate tracked statefully (Orca emits F on its own line).
static size_t layers_extruding_at(const std::string &gcode, const std::string &feed_word)
{
    size_t layers = 0;
    bool   counted = false, at_feed = false;
    std::istringstream in(gcode);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0) { counted = false; continue; }
        if (line.rfind("G1 ", 0) != 0) continue;
        const size_t f = line.find(" F");
        if (f != std::string::npos) {
            const size_t end = line.find_first_of(" ;\r", f + 2);
            at_feed = line.substr(f + 1, (end == std::string::npos ? line.size() : end) - (f + 1)) == feed_word;
        }
        if (at_feed && ! counted && line.find(" E") != std::string::npos && line.find_first_of("XYZ") != std::string::npos) {
            ++ layers;
            counted = true;
        }
    }
    return layers;
}

static std::string belt_cube_gcode(const char *first_layer_plane)
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "initial_layer_speed",          7 },   // F420: a value no other role uses
        { "initial_layer_infill_speed",   7 },
        { "outer_wall_speed",            40 },
        { "inner_wall_speed",            40 },
        { "sparse_infill_speed",         40 },
        { "internal_solid_infill_speed", 40 },
        { "top_surface_speed",           40 },
        { "gap_infill_speed",            40 },
        { "slow_down_for_layer_cooling",  0 },   // filament option: the cooling slowdown masks speeds
        { "filament_max_volumetric_speed", 100 }, // and the volumetric cap would clip the 40 mm/s roles
        { "first_layer_plane",           first_layer_plane },
        { "layer_change_gcode",          "G92 E0\n" },
    });
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, config);
    return Slic3r::Test::gcode(print);
}

TEST_CASE("Belt printer: first-layer speed follows height above the belt", "[Print][belt][contact]")
{
    const std::string legacy  = belt_cube_gcode("xy");
    const std::string contact = belt_cube_gcode("auto");
    CHECK(layers_extruding_at(legacy, "F420") == 1);
    // Of the 141 tilted layers, those whose plane still meets the belt (slice z up to
    // 20 cos 45 = 14.1 mm, about 70 layers) each carry a strip on it.
    CHECK(layers_extruding_at(contact, "F420") > 60);
    // Away from the belt the normal speed is still used.
    CHECK(layers_extruding_at(contact, "F2400") > 100);
}

// BELT_MOD -- preview: the processor hands the viewer the exact machine -> bed mapping.
TEST_CASE("Belt printer: the processor maps machine coordinates back onto the model", "[Print][belt][preview]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "gcode_remap_x", "rev_x" },   // the BabyBelt convention: Y is the gantry, Z feeds the belt
        { "gcode_remap_y", "pos_z" },
        { "gcode_remap_z", "pos_y" },
        { "printable_area", "0x0,95x0,95x500,0x500" },
        { "printable_height", 100 },
    });
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    Slic3r::Print print;
    Slic3r::Model model;
    {
        ModelObject *object = model.add_object();
        object->add_volume(cube(20));
        object->add_instance()->set_offset(Vec3d(40., 40., 0.));   // inside the 95 mm wide belt
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
        print.apply(model, config);
    }
    print.set_status_silent();
    print.process();
    // In the bed frame every extrusion lies inside the cube, give or take the extrusion width.
    const BoundingBoxf3 cube = print.objects().front()->model_object()->instance_bounding_box(0);

    const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("belt-preview-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(), &result, nullptr);
    REQUIRE(result.belt_printer);
    CHECK(result.belt_infinite_y);
    // No rotary axes: the preview's nozzle stands along the gantry, 45 degrees off the bed normal.
    CHECK(result.tool_axis_fixed);
    CHECK(std::abs(result.fixed_tool_axis.z()) == Catch::Approx(std::cos(M_PI / 4.)).margin(1e-4));
    CHECK(std::abs(result.fixed_tool_axis.y()) == Catch::Approx(std::sin(M_PI / 4.)).margin(1e-4));

    size_t extrusions = 0;
    double raw_z_max = 0.;
    BoundingBoxf3 mapped;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role == erCustom)
            continue;
        ++extrusions;
        raw_z_max = std::max(raw_z_max, double(move.position.z()));
        mapped.merge(result.belt_to_bed * move.position.cast<double>());
    }
    REQUIRE(extrusions > 1000);
    // Raw machine Z feeds the belt: the cube ends about 20 + 20 cot 45 mm down it, well past its height.
    CHECK(raw_z_max > 30.);
    // Mapped back, the toolpaths are the cube: 20 mm tall, sitting on the belt, within the outline.
    CHECK(mapped.max.z() == Catch::Approx(cube.max.z()).margin(0.3));
    CHECK(mapped.min.z() >= -0.05);
    CHECK(mapped.min.x() >= cube.min.x() - 0.5);
    CHECK(mapped.max.x() <= cube.max.x() + 0.5);
    CHECK(mapped.min.y() >= cube.min.y() - 0.5);
    CHECK(mapped.max.y() <= cube.max.y() + 0.5);

    // The bounds check judges the bed frame too: 100 mm of clearance passes a 20 mm cube, 10 mm does not.
    GCodeProcessor processor;
    processor.apply_config(print.config());
    processor.process_file(temp.string());
    GCodeProcessorResult &checked = processor.result();
    auto check = [&](double height) {
        const bool ok = processor.check_multi_extruder_gcode_valid(1, print.config().printable_area.values, height, Pointfs{},
                                                                   std::vector<Polygons>{}, std::vector<double>{ height },
                                                                   std::vector<int>{ 1 }, std::vector<std::set<int>>{ {} });
        return std::make_pair(ok, checked.gcode_check_result.error_code);
    };
    CHECK(check(100.).first);
    CHECK((check(100.).second & (1 << 3)) == 0);
    CHECK_FALSE(check(10.).first);
    CHECK((check(10.).second & (1 << 3)) != 0);
    boost::filesystem::remove(temp);
}

// BELT_MOD -- the belt is a floor: the leading strip of every tilted layer rests on it, so it is
// neither a floating region nor an overhang. A cube needs no support; a genuine overhang still does.
// BELT_MOD -- several copies of one object. Each belt instance is its own PrintObject, but they
// share the ModelObject, and the un-rotation pivot used to be derived from the ModelObject's
// bounding box -- the box of ALL copies -- so every copy came out shifted by the spread between
// them, partly below the belt. The pivot is this object's own instance now.
TEST_CASE("Belt printer: copies of an object each land on their own footprint", "[Print][belt][instances]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "gcode_remap_x", "rev_x" },
        { "gcode_remap_y", "pos_z" },
        { "gcode_remap_z", "pos_y" },
        { "printable_area", "0x0,95x0,95x500,0x500" },
        { "printable_height", 100 },
    });
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    Slic3r::Print print;
    Slic3r::Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->add_instance()->set_offset(Vec3d(30., 40., 0.));
    object->add_instance()->set_offset(Vec3d(60., 120., 0.));
    object->add_instance()->set_offset(Vec3d(40., 200., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE(print.validate().string.empty());
    REQUIRE(print.objects().size() == 3);
    print.process();

    const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("belt-copies-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(), &result, nullptr);
    REQUIRE(result.belt_printer);
    // Every extrusion, mapped back to the bed, lies inside one of the three cubes and on the belt.
    std::vector<BoundingBoxf3> cubes;
    for (size_t i = 0; i < 3; ++ i) {
        BoundingBoxf3 bb = object->instance_bounding_box(i);
        bb.offset(0.5);
        cubes.push_back(bb);
    }
    size_t extrusions = 0, stray = 0; double lowest = 1e9;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role == erCustom)
            continue;
        ++ extrusions;
        const Vec3d p = result.belt_to_bed * move.position.cast<double>();
        lowest = std::min(lowest, p.z());
        if (! std::any_of(cubes.begin(), cubes.end(), [&p](const BoundingBoxf3 &bb) { return bb.contains(p); }))
            ++ stray;
    }
    INFO("extrusions " << extrusions << " stray " << stray << " lowest bed z " << lowest);
    REQUIRE(extrusions > 3000);
    CHECK(stray == 0);
    CHECK(lowest >= -0.05);
    boost::filesystem::remove(temp);
}

// BELT_MOD -- an object dropped below the plate: the part under the belt is never printed.
TEST_CASE("Belt printer: a sunk object is cut at the belt", "[Print][belt][sunk]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "gcode_remap_x", "rev_x" }, { "gcode_remap_y", "pos_z" }, { "gcode_remap_z", "pos_y" },
        { "printable_area", "0x0,95x0,95x500,0x500" }, { "printable_height", 100 },
    });
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    Slic3r::Print print;
    Slic3r::Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->add_instance()->set_offset(Vec3d(40., 60., 0.));
    object->ensure_on_bed();
    object->translate_instances(Vec3d(0., 0., -1.5));   // 1.5 mm under the belt
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE(print.validate().string.empty());
    print.process();
    const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("belt-sunk-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(), &result, nullptr);
    double lowest = 1e9, highest = -1e9; size_t extrusions = 0;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role == erCustom)
            continue;
        ++ extrusions;
        const Vec3d p = result.belt_to_bed * move.position.cast<double>();
        lowest = std::min(lowest, p.z()); highest = std::max(highest, p.z());
    }
    INFO("extrusions " << extrusions << " bed z [" << lowest << ", " << highest << "]");
    REQUIRE(extrusions > 1000);
    CHECK(lowest >= -0.05);
    CHECK(highest == Catch::Approx(18.5).margin(0.3));
    boost::filesystem::remove(temp);
}

// UNLAYERED_MOD -- a multi-planar (B/C rotated) object with several copies on a PLANAR printer:
// the copies used to share one PrintObject and one un-rotation pivot, so all but the first landed
// off their footprint. Each copy of a rotated object is its own PrintObject now.
TEST_CASE("Multi-planar: copies of a rotated object each land on their own footprint", "[Print][multiplanar][instances]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar", 1 },
        { "layer_rotation_b", 20 },
        { "layer_rotation_c", 30 },
        { "layer_height", 0.2 }, { "initial_layer_print_height", 0.2 },
        { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "enable_prime_tower", 0 }, { "enable_support", 0 },
        { "printable_area", "0x0,200x0,200x200,0x200" }, { "printable_height", 200 },
    });
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    Slic3r::Print print;
    Slic3r::Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->add_instance()->set_offset(Vec3d(40., 40., 0.));
    object->add_instance()->set_offset(Vec3d(120., 130., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE(print.validate().string.empty());
    REQUIRE(print.objects().size() == 2);
    print.process();
    const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("mp-copies-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(), &result, nullptr);
    std::vector<BoundingBoxf3> cubes;
    for (size_t i = 0; i < 2; ++ i) { BoundingBoxf3 bb = object->instance_bounding_box(i); bb.offset(0.6); cubes.push_back(bb); }
    size_t extrusions = 0, stray = 0;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role == erCustom)
            continue;
        ++ extrusions;
        const Vec3d p = move.position.cast<double>();
        if (! std::any_of(cubes.begin(), cubes.end(), [&p](const BoundingBoxf3 &bb) { return bb.contains(p); }))
            ++ stray;
    }
    INFO("extrusions " << extrusions << " stray " << stray);
    REQUIRE(extrusions > 2000);
    CHECK(stray == 0);
    boost::filesystem::remove(temp);
}

// UNLAYERED_MOD -- a tilted object on a printer with a negative Z offset (a Prusa MINI at -0.63)
// was refused with "GCode: very low z" for most orientations: the floor test included z_offset,
// the printer's own calibration, so the first tilted layer looked 0.4 mm into the bed.
TEST_CASE("Multi-planar: a negative Z offset does not make a tilted object very low z", "[Print][multiplanar][z_offset]")
{
    auto slice = [](double z_offset, double b) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_nonplanar", 1 },
            { "layer_rotation_b", b },
            { "layer_rotation_c", 90 },
            { "layer_height", 0.2 }, { "initial_layer_print_height", 0.2 },
            { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "enable_prime_tower", 0 }, { "enable_support", 0 },
            { "printable_area", "0x0,180x0,180x180,0x180" }, { "printable_height", 180 },
        });
        config.set_key_value("z_offset", new ConfigOptionFloat(z_offset));
        config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
        Slic3r::Print print;
        Slic3r::Model model;
        ModelObject *object = model.add_object();
        object->add_volume(cube(20));
        object->add_instance()->set_offset(Vec3d(90., 90., 0.));
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.set_status_silent();
        REQUIRE(print.validate().string.empty());
        print.process();
        const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("mp-zoffset-%%%%-%%%%.gcode");
        GCodeProcessorResult result;
        INFO("z_offset " << z_offset << " B " << b);
        CHECK_NOTHROW(print.export_gcode(temp.string(), &result, nullptr));
        boost::filesystem::remove(temp);
    };
    for (double b : { -10., -45., -175. }) {
        slice(0., b);
        slice(-0.63, b);
    }
}

TEST_CASE("Belt printer: material on the belt is not a floating region", "[Print][belt][overhang]")
{
    {
        Slic3r::Print print;
        Slic3r::Model model;
        build_cubes(model, print, belt_print_config(), /*n=*/1, /*overlap=*/false);
        print.set_status_silent();
        print.process();
        // The probe behind the "floating regions" warning.
        CHECK(print.objects_mutable().front()->is_support_necessary() == NoNeedSupp);
        // And the perimeter generator: the strip on the belt is not an overhang perimeter either.
        size_t overhang_paths = 0, perimeter_paths = 0;
        auto count_path = [&](const ExtrusionPath &path) {
            ++ perimeter_paths;
            if (path.role() == erOverhangPerimeter)
                ++ overhang_paths;
        };
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *e : region->perimeters.flatten().entities) {
                    if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(e))
                        for (const ExtrusionPath &path : loop->paths) count_path(path);
                    else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath*>(e))
                        for (const ExtrusionPath &path : multi->paths) count_path(path);
                    else if (const auto *path = dynamic_cast<const ExtrusionPath*>(e))
                        count_path(*path);
                }
        CHECK(perimeter_paths > 100);
        CHECK(overhang_paths < 5);
    }
    {
        // The T's arms overhang the stem on a belt as they do on a flat bed.
        DynamicPrintConfig config = belt_print_config();
        Slic3r::Print print;
        Slic3r::Model model;
        ModelObject *object = model.add_object();
        object->add_volume(belt_tee_mesh());
        object->add_instance();
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.set_status_silent();
        print.process();
        CHECK(print.objects_mutable().front()->is_support_necessary() != NoNeedSupp);
    }
}

// BELT_MOD -- belt brim: bands on the belt over many layers, plus an apron ahead of the part.
TEST_CASE("Belt printer: the brim lies on the belt and reaches ahead of the part", "[Print][belt][brim]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "brim_type",           "outer_only" },
        { "brim_width",          4 },
        { "leading_brim_length", 8 },
        { "brim_object_gap",     0 },
        { "gcode_remap_x", "rev_x" }, { "gcode_remap_y", "pos_z" }, { "gcode_remap_z", "pos_y" },
        { "printable_area", "0x0,95x0,95x500,0x500" },
        { "printable_height", 100 },
    });
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    Slic3r::Print print;
    Slic3r::Model model;
    {
        ModelObject *object = model.add_object();
        object->add_volume(cube(20));
        object->add_instance()->set_offset(Vec3d(40., 60., 0.));
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
        print.apply(model, config);
    }
    {
        std::vector<StringObjectException> warnings;
        const StringObjectException err = print.validate(&warnings);
        INFO("validate: " << err.string);
        REQUIRE(err.string.empty());
    }
    print.set_status_silent();
    print.process();
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.has_belt_brim());
    CHECK_FALSE(print.has_brim());   // the plate brim stays off on a belt
    // The apron: brim-only bands below the object's first layer, lowest first.
    REQUIRE(! object.belt_brim_prologue().empty());
    CHECK(object.belt_brim_prologue().back().print_z < object.layers().front()->print_z);
    CHECK(object.belt_brim_prologue().front().print_z < object.belt_brim_prologue().back().print_z);
    // Bands on the early layers, where the part still meets the belt; none near the trailing top.
    size_t layers_with_brim = 0;
    for (const ExtrusionEntityCollection &c : object.belt_brim_by_layer())
        if (! c.empty())
            ++ layers_with_brim;
    CHECK(layers_with_brim > 20);
    CHECK(object.belt_brim_by_layer().back().empty());

    // Every brim move maps back onto the belt surface and ahead of / around the cube's footprint.
    const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("belt-brim-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(), &result, nullptr);
    const BoundingBoxf3 cube_bb = object.model_object()->instance_bounding_box(0);
    size_t brim_moves = 0;
    double z_max = 0., y_min = 1e9;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role != erBrim)
            continue;
        ++ brim_moves;
        const Vec3d p = result.belt_to_bed * move.position.cast<double>();
        z_max = std::max(z_max, p.z());
        y_min = std::min(y_min, p.y());
        CHECK(p.x() >= cube_bb.min.x() - 5.);
        CHECK(p.x() <= cube_bb.max.x() + 5.);
    }
    REQUIRE(brim_moves > 100);
    CHECK(z_max < 0.3);                          // a sheet on the belt, never above the first layer
    CHECK(y_min < cube_bb.min.y() - 6.);         // the apron reaches ahead of the part
    boost::filesystem::remove(temp);
}

// BELT_MOD -- a part further along the belt is raised by the belt schedule, so its frame starts with
// empty layers below its material. The apron hangs off its first layer WITH material; anchored on
// the empty front layer it came out empty, and the brim started at the part.
TEST_CASE("Belt printer: a trailing part's brim still reaches ahead of it", "[Print][belt][brim]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "brim_type",           "outer_only" },
        { "brim_width",          4 },
        { "leading_brim_length", 8 },
        { "brim_object_gap",     0 },
        { "gcode_remap_x", "rev_x" }, { "gcode_remap_y", "pos_z" }, { "gcode_remap_z", "pos_y" },
        { "printable_area", "0x0,95x0,95x500,0x500" },
        { "printable_height", 100 },
    });
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    Slic3r::Print print;
    Slic3r::Model model;
    // Two cubes along the belt, 60 mm apart: the trailing one gets a schedule offset.
    for (double y : { 60., 140. }) {
        ModelObject *object = model.add_object();
        object->add_volume(cube(20));
        object->add_instance()->set_offset(Vec3d(40., y, 0.));
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
    }
    print.apply(model, config);
    REQUIRE(print.validate().string.empty());
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().size() == 2);

    const PrintObject *trailing = print.objects().front();
    for (const PrintObject *object : print.objects())
        if (object->model_object()->instance_bounding_box(0).min.y() > trailing->model_object()->instance_bounding_box(0).min.y())
            trailing = object;
    const BoundingBoxf3 trailing_bb = trailing->model_object()->instance_bounding_box(0);
    // The precondition: the trailing part's frame starts with empty layers under its material.
    REQUIRE(trailing->layers().front()->lslices.empty());
    REQUIRE(! trailing->belt_brim_prologue().empty());

    const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("belt-brim-trailing-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(), &result, nullptr);
    // Brim moves past the leading cube's brim belong to the trailing cube.
    double y_min = 1e9;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role != erBrim)
            continue;
        const Vec3d p = result.belt_to_bed * move.position.cast<double>();
        if (p.y() > trailing_bb.min.y() - 30.)
            y_min = std::min(y_min, p.y());
    }
    INFO("trailing part starts at y " << trailing_bb.min.y() << ", its brim at y " << y_min);
    CHECK(y_min < trailing_bb.min.y() - 6.);
    boost::filesystem::remove(temp);
}

// BELT_MOD -- the belt brim with a thick first layer (found by fuzzing: every belt print with a
// 0.28-0.3 mm first layer and a brim was refused with "GCode: very low z" -- the apron bands came
// out almost on the belt instead of at their designed clearance).
TEST_CASE("Belt printer: the brim keeps its clearance with a thick first layer", "[Print][belt][brim][thick]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "brim_type",           "outer_only" },
        { "brim_width",          4 },
        { "leading_brim_length", 8 },
        { "brim_object_gap",     0 },
        { "layer_height",        0.3 },
        { "initial_layer_print_height", 0.3 },
        { "gcode_remap_x", "rev_x" }, { "gcode_remap_y", "pos_z" }, { "gcode_remap_z", "pos_y" },
        { "printable_area", "0x0,95x0,95x500,0x500" },
        { "printable_height", 100 },
    });
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    Slic3r::Print print;
    Slic3r::Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->add_instance()->set_offset(Vec3d(40., 60., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    REQUIRE(print.validate().string.empty());
    print.set_status_silent();
    print.process();
    const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("belt-brim-thick-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    REQUIRE_NOTHROW(print.export_gcode(temp.string(), &result, nullptr));
    size_t brim_moves = 0; double z_min = 1e9, z_max = -1e9;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role != erBrim)
            continue;
        ++ brim_moves;
        const Vec3d p = result.belt_to_bed * move.position.cast<double>();
        z_min = std::min(z_min, p.z()); z_max = std::max(z_max, p.z());
    }
    INFO("brim moves " << brim_moves << " bed z [" << z_min << ", " << z_max << "]");
    REQUIRE(brim_moves > 100);
    // Designed clearance is 0.75 x first layer height; never scraping the belt.
    CHECK(z_min > 0.1);
    CHECK(z_max < 0.45);
    boost::filesystem::remove(temp);
}

// BELT_MOD -- belt purge tower: a marked prism absorbs the filament changes of the parts beside it.
// BELT_MOD -- multi-material painting on a belt. The painted facets must be projected in the
// frame the layers were sliced in (PrintObject::object_to_slice(), which carries the belt tilt);
// projecting them with the unrotated instance transform put the paint on the wrong layers and the
// wrong side of the part (reported on a painted Benchy: the colours did not follow the model).
static void painted_cube_lands_on_its_surface(bool belt)
{
    DynamicPrintConfig config = belt_print_config();
    if (! belt)
        config.set_deserialize_strict({ { "belt_printer", 0 } });
    config.set_deserialize_strict({
        { "printable_area", "0x0,95x0,95x500,0x500" },
        { "printable_height", 100 },
        { "filament_max_volumetric_speed", 100 },
        { "top_shell_layers", 3 },
        { "bottom_shell_layers", 3 },
        // On the belt the painted top face crosses the tilted layers, so the paint is a band next
        // to the face rather than the top shells; keep that band narrow so the check is sharp.
        { "mmu_segmented_region_max_width", 1.0 },
    });
    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map",             new ConfigOptionInts({1, 2}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
    config.set_key_value("flush_multiplier",         new ConfigOptionFloats({1}));
    config.set_key_value("flush_volumes_matrix",     new ConfigOptionFloats({0, 0, 0, 0}));
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(true));
    config.set_key_value("layer_change_gcode",       new ConfigOptionString("G92 E0\n"));

    Slic3r::Model model;
    Slic3r::Print print;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(cube(20));
    object->add_instance()->set_offset(Vec3d(40., 60., 0.));
    object->ensure_on_bed();
    // Paint the top face (the two facets whose vertices all sit at the cube's maximum z) with the
    // second filament.
    {
        const TriangleMesh &mesh = volume->mesh();
        const float z_top = mesh.bounding_box().max.z();
        TriangleSelector selector(mesh);
        int painted = 0;
        for (size_t i = 0; i < mesh.its.indices.size(); ++ i) {
            const auto &tri = mesh.its.indices[i];
            if (mesh.its.vertices[tri(0)].z() > z_top - 1e-3f && mesh.its.vertices[tri(1)].z() > z_top - 1e-3f && mesh.its.vertices[tri(2)].z() > z_top - 1e-3f) {
                selector.set_facet(int(i), EnforcerBlockerType::Extruder2);
                ++ painted;
            }
        }
        REQUIRE(painted == 2);
        REQUIRE(volume->mmu_segmentation_facets.set(selector));
        REQUIRE(volume->is_mm_painted());
    }
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE(print.validate().string.empty());
    print.process();

    const PrintObject &po = *print.objects().front();
    // Bed-frame height of a slice-frame point (layer-local XY, print_z): the same map the belt clip
    // and the support floor use.
    Vec3d n_bed; double k_bed;
    po.bed_plane_in_slice_frame(n_bed, k_bed);
    const double top_z = object->instance_bounding_box(0).max.z();
    size_t painted_points = 0; double lowest = 1e9, highest = -1e9;
    for (const Layer *layer : po.layers())
        for (const LayerRegion *layerm : layer->regions()) {
            if (layerm->region().extruder(frPerimeter) != 2)
                continue;
            for (const Surface &surface : layerm->slices.surfaces)
                for (const Point &pt : surface.expolygon.contour.points) {
                    const double bed_z = n_bed.x() * unscale<double>(pt.x()) + n_bed.y() * unscale<double>(pt.y()) + n_bed.z() * layer->print_z + k_bed;
                    lowest = std::min(lowest, bed_z); highest = std::max(highest, bed_z);
                    ++ painted_points;
                }
        }
    std::string regions_desc;
    for (size_t i = 0; i < po.num_printing_regions(); ++ i)
        regions_desc += " region " + std::to_string(i) + " outer/inner filament " + std::to_string(po.printing_region(i).config().outer_wall_filament_id.value)
                      + "/" + std::to_string(po.printing_region(i).config().inner_wall_filament_id.value);
    INFO("painted points " << painted_points << " bed z [" << lowest << ", " << highest << "] top " << top_z << " regions:" << regions_desc);
    REQUIRE(painted_points > 0);
    // The second filament covers the top face and nothing far below it: with the paint projected
    // in the unrotated frame it ran down a 45 degree band through the whole cube (and on the belt
    // there was no painted region at all). On the belt the paint is propagated down the TILTED
    // layers -- the slices at the cube's top edge are all "top surface" in the slice frame -- so it
    // reaches a few millimetres further down the painted face's edge than on a planar printer.
    CHECK(highest > top_z - 1.0);
    CHECK(lowest > top_z - (belt ? 8.0 : 3.0));
}

TEST_CASE("Belt printer: painted colour lands on the painted surface", "[Print][belt][painting]")
{
    SECTION("planar")  { painted_cube_lands_on_its_surface(false); }
    SECTION("belt")    { painted_cube_lands_on_its_surface(true); }
}

// The multi-material segmentation has one entry per filament_colour, while apply_mm_segmentation
// walks filament_diameter. A project carrying fewer colours than filaments read past the end of the
// segmentation and crashed in get_extents() (Slice Plate on macOS).
TEST_CASE("A colour-painted object slices with fewer filament colours than filaments", "[Print][painting][Regression]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "belt_printer", 0 },
        { "printable_area", "0x0,95x0,95x500,0x500" },
        { "printable_height", 100 },
        { "filament_max_volumetric_speed", 100 },
    });
    // Four filaments, but only one colour.
    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75, 1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA", "PLA", "PLA"}));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map",             new ConfigOptionInts({1, 2, 1, 2}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210, 210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190, 190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240, 240, 240}));
    config.set_key_value("flush_multiplier",         new ConfigOptionFloats({1}));
    config.set_key_value("flush_volumes_matrix",     new ConfigOptionFloats(std::vector<double>(16, 0.)));
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(true));
    config.set_key_value("layer_change_gcode",       new ConfigOptionString("G92 E0\n"));

    Slic3r::Model model;
    Slic3r::Print print;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(cube(20));
    object->add_instance()->set_offset(Vec3d(40., 60., 0.));
    // Paint every facet above mid-height with the first filament, the only one that has a colour.
    {
        const TriangleMesh &mesh = volume->mesh();
        const float z_mid = float(mesh.bounding_box().center().z());
        TriangleSelector selector(mesh);
        for (size_t i = 0; i < mesh.its.indices.size(); ++ i) {
            const auto &tri = mesh.its.indices[i];
            if (mesh.its.vertices[tri(0)].z() > z_mid && mesh.its.vertices[tri(1)].z() > z_mid && mesh.its.vertices[tri(2)].z() > z_mid)
                selector.set_facet(int(i), EnforcerBlockerType::Extruder1);
        }
        REQUIRE(volume->mmu_segmentation_facets.set(selector));
        REQUIRE(volume->is_mm_painted());
    }
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE(print.config().filament_colour.size() < print.config().filament_diameter.size());
    REQUIRE(print.validate().string.empty());
    print.process();

    const PrintObject &po = *print.objects().front();
    REQUIRE(po.layer_count() > 0);
    size_t sliced = 0;
    for (const Layer *layer : po.layers())
        sliced += layer->lslices.size();
    CHECK(sliced > 0);
}

// BELT_MOD -- purge prism with a first layer thinner than the layer height (found by fuzzing:
// the writer was handed a single filament and the first tool change crashed in
// GCodeWriter::toolchange).
TEST_CASE("Belt printer: the purge prism works with a thinner first layer", "[Print][belt][purge][firstlayer]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "enable_belt_purge_tower", 1 },
        { "printable_area", "0x0,95x0,95x500,0x500" },
        { "printable_height", 100 },
        { "filament_max_volumetric_speed", 100 },
        { "initial_layer_print_height", 0.1 },
        { "brim_type", "outer_only" },
        { "brim_width", 3 },
    });
    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map",             new ConfigOptionInts({1, 2}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
    config.set_key_value("flush_multiplier",         new ConfigOptionFloats({1}));
    config.set_key_value("flush_volumes_matrix",     new ConfigOptionFloats({0, 0, 0, 0}));
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(true));
    config.set_key_value("layer_change_gcode",       new ConfigOptionString("G92 E0\n"));

    Slic3r::Model model;
    Slic3r::Print print;
    for (int i = 0; i < 2; ++ i) {
        ModelObject *object = model.add_object();
        object->add_volume(cube(20));
        object->add_instance()->set_offset(Vec3d(20., 20. + 10. * i, 0.));
        if (i == 1)
            object->config.set_key_value("extruder", new ConfigOptionInt(2));
    }
    ModelObject *prism = model.add_object();
    prism->add_volume(make_cube(10., 60., 3.));   // a thin bar, like the GUI's smallest prism
    prism->add_instance()->set_offset(Vec3d(70., 30., 0.));
    prism->config.set_key_value("belt_purge_tower_object", new ConfigOptionBool(true));
    prism->config.set_key_value("flush_into_objects", new ConfigOptionBool(true));
    prism->config.set_key_value("wall_loops", new ConfigOptionInt(1));
    prism->config.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    prism->config.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    prism->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(100));
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    REQUIRE(print.validate().string.empty());
    print.set_status_silent();
    print.process();
    const ToolOrdering &ordering = print.tool_ordering();
    INFO("all_extruders size " << ordering.all_extruders().size());
    CHECK(ordering.all_extruders().size() == 2);
    const boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("belt-purge-thin-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    REQUIRE_NOTHROW(print.export_gcode(temp.string(), &result, nullptr));
    boost::filesystem::remove(temp);
}

TEST_CASE("Belt printer: the purge prism absorbs filament changes and stops after the last one", "[Print][belt][purge]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "enable_belt_purge_tower", 1 },
        { "printable_area", "0x0,95x0,95x500,0x500" },
        { "printable_height", 100 },
        { "filament_max_volumetric_speed", 100 },
        { "brim_type", "outer_only" },   // the parts get a belt brim; the prism never does
        { "brim_width", 3 },
    });
    // Two extruders (the pattern of the multi-extruder writer test).
    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map",             new ConfigOptionInts({1, 2}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
    config.set_key_value("flush_multiplier",         new ConfigOptionFloats({1}));
    config.set_key_value("flush_volumes_matrix",     new ConfigOptionFloats({0, 0, 0, 0}));
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(true));
    config.set_key_value("layer_change_gcode",       new ConfigOptionString("G92 E0\n"));   // validate() relative-E reset

    Slic3r::Model model;
    Slic3r::Print print;
    // Two cubes 10 mm apart along the belt so their layers overlap: every shared layer swaps filament.
    for (int i = 0; i < 2; ++ i) {
        ModelObject *object = model.add_object();
        object->add_volume(cube(20));
        object->add_instance()->set_offset(Vec3d(20., 20. + 10. * i, 0.));
        if (i == 1)
            object->config.set_key_value("extruder", new ConfigOptionInt(2));
    }
    // The prism the GUI would generate: a bar beside the parts along the belt, marked and flushable.
    ModelObject *prism = model.add_object();
    prism->add_volume(make_cube(10., 60., 10.));
    prism->add_instance()->set_offset(Vec3d(70., 30., 0.));
    prism->config.set_key_value("belt_purge_tower_object", new ConfigOptionBool(true));
    prism->config.set_key_value("flush_into_objects", new ConfigOptionBool(true));
    prism->config.set_key_value("wall_loops", new ConfigOptionInt(1));
    prism->config.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    prism->config.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    prism->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(100));
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    {
        const StringObjectException err = print.validate();
        INFO("validate: " << err.string);
        REQUIRE(err.string.empty());
    }
    REQUIRE(print.has_belt_purge_tower());
    CHECK_FALSE(print.has_wipe_tower());
    for (const PrintObject *po : print.objects())
        CHECK(po->has_belt_brim() == ! po->config().belt_purge_tower_object.value);
    print.set_status_silent();
    print.process();

    // One lattice: every object's schedule offset is a multiple of the layer height.
    const PrintObject *prism_po = nullptr;
    for (const PrintObject *po : print.objects()) {
        CHECK(std::abs(po->belt_schedule_offset() / 0.2 - std::round(po->belt_schedule_offset() / 0.2)) < 1e-6);
        if (po->config().belt_purge_tower_object.value)
            prism_po = po;
    }
    REQUIRE(prism_po != nullptr);
    // There are filament changes, and every one of them lands on a prism layer.
    ToolOrdering &ordering = const_cast<ToolOrdering&>(print.tool_ordering());   // layer_tools() is not const
    // (has_wipe_tower() reads the first layer, a brim apron band here; the change scan below is the test.)
    unsigned int cur = ordering.first_extruder();
    double last_tc_z = -1.;
    int    changes = 0;
    for (const LayerTools &lt : ordering.layer_tools())
        for (unsigned int e : lt.extruders)
            if (e != cur) {
                ++ changes;
                cur = e;
                last_tc_z = lt.print_z;
                CHECK(prism_po->get_layer_at_printz(lt.print_z, EPSILON) != nullptr);
            }
    CHECK(changes > 10);
    // The prism stops at the last change: a 60 mm bar would otherwise run to about 49 mm of slice z.
    CHECK(prism_po->layers().back()->print_z <= last_tc_z + EPSILON);
    CHECK(prism_po->layers().back()->print_z < 40.);
    // And the plan claimed extrusions in the prism for the swaps.
    bool any_override = false;
    for (const LayerTools &lt : ordering.layer_tools())
        if (const_cast<LayerTools&>(lt).wiping_extrusions().is_anything_overridden())
            any_override = true;
    CHECK(any_override);
}

// BELT_MOD -- belt schedule: several objects interleave in belt order. Each object's frame is raised
// by its lead along the gantry normal so print_z means the same belt position for all of them.
TEST_CASE("Belt printer: several objects interleave in belt order", "[Print][belt][schedule]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "print_sequence",     "by layer" },
        { "layer_change_gcode", "G92 E0\n" },
    });
    Slic3r::Model model;
    Slic3r::Print print;
    // Two 20 mm cubes, 40 mm apart along the belt (Y).
    for (int i = 0; i < 2; ++ i) {
        ModelObject *object = model.add_object();
        object->add_volume(cube(20));
        ModelInstance *inst = object->add_instance();
        inst->set_offset(Vec3d(0., 40. * i, 0.));
    }
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    {
        const StringObjectException err = print.validate();
        INFO("validate: " << err.string);
        REQUIRE(err.string.empty());
    }
    REQUIRE(print.objects().size() == 2);
    const PrintObject *lead = print.objects()[0], *trail = print.objects()[1];
    if (lead->belt_schedule_offset() > trail->belt_schedule_offset())
        std::swap(lead, trail);
    CHECK(lead->belt_schedule_offset() == Catch::Approx(0.).margin(1e-6));
    // 40 mm along the belt is 40 sin 45 along the gantry normal.
    CHECK(trail->belt_schedule_offset() == Catch::Approx(40. * std::sin(M_PI / 4.)).margin(0.05));

    print.process();
    // The trailing cube's material starts at its offset, its leading empty layers are not written,
    // and the by-layer merge spans both stacks: 28.28 + 28.28 mm of belt-ordered layers.
    CHECK(trail->layers().front()->print_z <= trail->belt_schedule_offset() + 0.4);
    const std::string gcode = Slic3r::Test::gcode(print);
    size_t layer_changes = 0;
    for (size_t pos = gcode.find(";LAYER_CHANGE"); pos != std::string::npos; pos = gcode.find(";LAYER_CHANGE", pos + 1))
        ++ layer_changes;
    CHECK(layer_changes > 270);
    CHECK(layer_changes < 300);

    // The other sequences would drive the belt backwards between objects.
    config.set_deserialize_strict({ { "print_sequence", "by object" } });
    print.apply(model, config);
    CHECK(count_opt_key({ print.validate() }, "print_sequence") == 1);
}

// BELT_MOD -- multi-planar belt: a cut-plane object on a belt printer. The root builds along the
// gantry normal (rotary home), the upper segment along its own normal, B/C relative to the gantry.
// BELT_MOD -- the schedule measures each object's lead along the belt at its instance placement.
// The PrintInstance shift also carries the centre offset add_volume() leaves on an off-centre
// mesh; measuring at the shift pushed a 60 mm bar half its length down the belt, so the GUI's purge
// prism started 60 mm after the parts and its purges landed on the wrong stretch of belt.
TEST_CASE("Belt printer: the schedule lead of an off-centre mesh is measured at its instance", "[Print][belt][schedule][offcentre]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    Slic3r::Model model;
    Slic3r::Print print;
    // A cube (centre offset 10 mm) and a 60 mm bar (centre offset 30 mm) whose leading edge sits
    // 5 mm ahead of the cube's along the belt.
    ModelObject *cube_obj = model.add_object();
    cube_obj->add_volume(cube(20));
    cube_obj->add_instance()->set_offset(Vec3d(20., 40., 0.));
    ModelObject *bar_obj = model.add_object();
    bar_obj->add_volume(make_cube(10., 60., 10.));
    bar_obj->add_instance()->set_offset(Vec3d(60., 5., 0.));
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE(print.validate().string.empty());
    // The two centre offsets differ (10 mm vs 30 mm along the belt), which is what used to skew
    // the lead. The expected lead is the plain distance between the leading edges on the belt.
    const double cube_lead_y = cube_obj->instance_bounding_box(0).min.y();
    const double bar_lead_y  = bar_obj->instance_bounding_box(0).min.y();
    REQUIRE(bar_lead_y < cube_lead_y);
    const PrintObject *cube_po = nullptr, *bar_po = nullptr;
    for (const PrintObject *po : print.objects())   // the Print works on its own copy of the model: match by id
        (po->model_object()->id() == cube_obj->id() ? cube_po : bar_po) = po;
    REQUIRE((cube_po != nullptr && bar_po != nullptr));
    REQUIRE(cube_po->center_offset() != bar_po->center_offset());
    // The bar leads, so its frame is not raised; the cube's is raised by its lag along the belt
    // projected onto the gantry normal (both bottoms sit on the belt).
    CHECK(bar_po->belt_schedule_offset() == Catch::Approx(0.).margin(1e-6));
    CHECK(cube_po->belt_schedule_offset() == Catch::Approx((cube_lead_y - bar_lead_y) * std::sin(M_PI / 4.)).margin(1e-6));
}

TEST_CASE("Belt printer: cut-plane segments compose with the belt tilt", "[Print][belt][multiplanar]")
{
    DynamicPrintConfig config = belt_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",   1 },
        { "axis_selection",     "bc" },
        { "print_sequence",     "by group" },
        { "layer_change_gcode", "G92 E0\n" },
    });
    Slic3r::Model model;
    Slic3r::Print print;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->add_instance();
    {
        // One cut 10 mm up, the upper half building straight up in the world (45 degrees off the gantry).
        CutPlane cp;
        cp.point  = Vec3d(10., 10., 10.);
        cp.normal = Vec3d::UnitZ();
        object->cut_planes.planes.push_back(cp);
        object->cut_planes.enabled = true;
    }
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    {
        const StringObjectException err = print.validate();
        INFO("validate: " << err.string);
        REQUIRE(err.string.empty());
    }
    REQUIRE(print.objects().size() == 2);
    const PrintObject *root = nullptr, *upper = nullptr;
    for (const PrintObject *o : print.objects())
        (o->is_root_segment() ? root : upper) = o;
    REQUIRE(root != nullptr);
    REQUIRE(upper != nullptr);
    const Vec3d gantry = BeltTransformPipeline::build_rotation_matrix(print.config()).transpose() * Vec3d::UnitZ();
    CHECK(root->build_normal_world().dot(gantry) == Catch::Approx(1.).margin(1e-6));
    CHECK(upper->build_normal_world().z() == Catch::Approx(1.).margin(1e-6));
    double b = 0., c = 0.;
    root->effective_layer_rotation_bc(b, c);
    CHECK(std::abs(b) == Catch::Approx(45.).margin(1e-6));

    const std::string gcode = Slic3r::Test::gcode(print);
    // Rotary words are written on a belt too; the root is at the rotary home (tool axis = gantry
    // normal, B = -90 in this fork's convention) and the upper segment 45 degrees away from it.
    // Every B word on a G0/G1 line: the segment transitions carry theirs on their own moves.
    std::vector<double> b_words;
    {
        std::istringstream in(gcode);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("G0 ", 0) != 0 && line.rfind("G1 ", 0) != 0) continue;
            const size_t pos = line.find(" B");
            if (pos != std::string::npos && pos + 2 < line.size() && (std::isdigit(line[pos + 2]) || line[pos + 2] == '-'))
                b_words.push_back(std::atof(line.c_str() + pos + 2));
        }
    }
    std::ostringstream seen;
    for (double b : b_words) seen << b << ' ';
    INFO("rotary B words (" << b_words.size() << "): " << seen.str());
    REQUIRE_FALSE(b_words.empty());
    const auto has_b = [&b_words](double b) {
        return std::any_of(b_words.begin(), b_words.end(), [b](double v) { return std::abs(v - b) < 1e-3; });
    };
    CHECK(has_b(-90.));
    CHECK(has_b(-45.));

    // A second object next to a multi-planar one would need the belt to run backwards.
    ModelObject *other = model.add_object();
    other->add_volume(cube(20));
    other->add_instance()->set_offset(Vec3d(0., 60., 0.));
    other->ensure_on_bed();
    print.auto_assign_extruders(other);
    print.apply(model, config);
    CHECK_FALSE(print.validate().string.empty());
}
