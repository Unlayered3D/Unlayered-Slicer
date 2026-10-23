// UNLAYERED_MOD -- automatic, printer-aware cut-plane selection (AutoPlanes) and the multiplanar
// toolpath audit (MultiplanarAudit). The planning tests run on synthetic parts generated in
// auto_planes_fixtures.hpp; the slicing tests run the real Print pipeline on the plans.

#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AutoPlanes.hpp"
#include "libslic3r/CutPlaneSegmentation.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/MultiplanarAudit.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include <cereal/archives/binary.hpp>
#include <boost/filesystem.hpp>

#include <sstream>

#include "auto_planes_fixtures.hpp"
#include "test_helpers.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using namespace AutoPlanesFixtures;
using Catch::Matchers::WithinAbs;

namespace {

// A synthetic fixed-nozzle profile with asymmetric clearance limits and a 5 degree manual ceiling.
// These fixture values exercise the planner; they are not measurements or a Centauri printer preset.
DynamicPrintConfig virtual_printer_config(double max_angle = 5.)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",             "1" },
        { "axis_selection",               "virtual" },
        { "nonplanar_hotend_cone_height", "125" },
        { "nonplanar_hotend_angle_xpos",  "7" },
        { "nonplanar_hotend_angle_xneg",  "10" },
        { "nonplanar_hotend_angle_ypos",  "15" },
        { "nonplanar_hotend_angle_yneg",  "6" },
        { "nonplanar_gantry_height",      "14" },
        { "nonplanar_virtual_max_angle",  std::to_string(max_angle) },
        { "nonplanar_virtual_max_z",      "100" },
        { "print_sequence",               "by group" },
        { "layer_height",                 "0.2" },
        { "initial_layer_print_height",   "0.2" },
        { "enable_support",               "1" },
        { "support_threshold_angle",      "30" },
        { "layer_change_gcode",           "G92 E0" },
        { "printable_area",               "0x0,256x0,256x256,0x256" },
        { "printable_height",             "256" },
    });
    return config;
}

ModelObject *add_part(Model &model, const indexed_triangle_set &its, const Vec3d &offset = Vec3d(128., 128., 0.),
                      const Vec3d &rotation_deg = Vec3d::Zero(), bool mirror_x = false)
{
    ModelObject *mo = model.add_object();
    mo->name = "part";
    mo->add_volume(TriangleMesh(its));
    ModelInstance *mi = mo->add_instance();
    mi->set_offset(offset);
    if (rotation_deg != Vec3d::Zero())
        mi->set_rotation(Vec3d(rotation_deg * (PI / 180.)));
    if (mirror_x)
        mi->set_mirror(Vec3d(-1., 1., 1.));
    // Centre the mesh on the object origin, then put the part ON the bed explicitly. (Centring after
    // ensure_on_bed() sinks the part by half its height, and ensure_on_bed() then leaves a part that
    // deep alone as deliberately sunk -- which is how earlier versions of these tests ran on
    // half-buried parts.)
    mo->center_around_origin();
    const double min_z = mo->instance_bounding_box(0).min.z();
    mi->set_offset(Z, mi->get_offset(Z) - min_z);
    mo->invalidate_bounding_box();
    REQUIRE(std::abs(mo->instance_bounding_box(0).min.z()) < 1e-6);
    return mo;
}

AutoPlanes::PlanResult plan_of(const ModelObject &mo, const DynamicPrintConfig &config,
                               AutoPlanes::Options opt = AutoPlanes::Options())
{
    opt.skeleton = AutoPlanes::Options::Skeleton::Off;
    return AutoPlanes::plan(mo, mo.instances.front()->get_matrix(), config, opt);
}

// Every tilted plane of a candidate is within the limit the slicer would enforce for it.
void check_within_limits(const AutoPlanes::Candidate &c)
{
    for (const AutoPlanes::PlannedPlane &p : c.planes) {
        INFO("plane origin " << p.origin << " tilt " << p.tilt_deg << " limit " << p.limit_deg);
        if (p.tilt_deg > 1e-6) {
            CHECK(p.limit_deg > 0.);
            CHECK(p.tilt_deg <= p.limit_deg + 1e-6);
        }
    }
}

} // namespace

TEST_CASE("AutoPlanes reads the machine capability from the profile", "[autoplanes]")
{
    SECTION("non-planar off: planar only") {
        DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
        const auto m = AutoPlanes::MachineCapability::from_config(c);
        CHECK(m.kind == AutoPlanes::MachineKind::PlanarOnly);
        CHECK_FALSE(m.can_tilt);
        CHECK(m.tilt_limit_deg(10., std::nullopt) == 0.);
    }
    SECTION("rotary axes: recognised, not planned for") {
        DynamicPrintConfig c = virtual_printer_config();
        c.set_deserialize_strict({ { "axis_selection", "bc" } });
        const auto m = AutoPlanes::MachineCapability::from_config(c);
        CHECK(m.kind == AutoPlanes::MachineKind::Rotary5Axis);
        CHECK_FALSE(m.can_tilt);
        CHECK_FALSE(m.reason.empty());
    }
    SECTION("virtual with no hotend envelope: no unrestricted default") {
        DynamicPrintConfig c = virtual_printer_config(0.);
        c.set_deserialize_strict({ { "nonplanar_hotend_cone_height", "0" }, { "nonplanar_gantry_height", "0" } });
        const auto m = AutoPlanes::MachineCapability::from_config(c);
        CHECK(m.kind == AutoPlanes::MachineKind::FixedNozzle3Axis);
        CHECK_FALSE(m.can_tilt);
        // The enforced limit reports "nothing characterised" -- and the planner must not read that as permission.
        CHECK(m.tilt_limit_deg(10., 0.) == 0.);
    }
    SECTION("characterised virtual printer: same limit Print::validate() enforces") {
        const DynamicPrintConfig c = virtual_printer_config();
        const auto m = AutoPlanes::MachineCapability::from_config(c);
        REQUIRE(m.can_tilt);
        for (double az : { 0., 0.5 * PI, PI, -0.5 * PI, 0.25 * PI })
            for (double reach : { 0., 20., 80., 200. }) {
                const double expect = virtual_tilt_limit_enforced_deg(HotendEnvelope::from_config(c), 14., 5., reach, az);
                CHECK_THAT(m.tilt_limit_deg(reach, az), WithinAbs(expect, 1e-12));
                CHECK(m.tilt_limit_deg(reach, az) <= 5. + 1e-12);
            }
        // A long segment is gantry-bound below the manual 5 degree ceiling: atan(14 / 200) = 4.0 deg.
        CHECK(m.tilt_limit_deg(200., 0.) < 4.1);
    }
}

TEST_CASE("Enforced tilt limit is the stricter of the switched-on terms", "[autoplanes][virtualtilt]")
{
    HotendEnvelope env;
    env.height = 40.;
    env.angle  = { 30., 30., 30., 30. };
    const double derived = virtual_tilt_limit_deg(env, 0., 0., 0.);
    CHECK_THAT(virtual_tilt_limit_enforced_deg(env, 0., 0., 0., 0.), WithinAbs(derived, 1e-12));
    CHECK_THAT(virtual_tilt_limit_enforced_deg(env, 0., 5., 0., 0.), WithinAbs(5., 1e-12));
    CHECK_THAT(virtual_tilt_limit_enforced_deg(env, 0., 50., 0., 0.), WithinAbs(derived, 1e-12));
    HotendEnvelope off; off.height = 0.;
    CHECK(virtual_tilt_limit_enforced_deg(off, 0., 0., 0., 0.) == 0.);     // nothing characterised
    CHECK_THAT(virtual_tilt_limit_enforced_deg(off, 0., 7., 0., 0.), WithinAbs(7., 1e-12));
}

TEST_CASE("AutoPlanes keeps planar printing for a flat box", "[autoplanes]")
{
    Model model;
    ModelObject *mo = add_part(model, flat_box());
    const auto r = plan_of(*mo, virtual_printer_config());
    CHECK(r.chosen == -1);
    CHECK(r.decision.find("Keep planar") != std::string::npos);
    CHECK(r.planar.predicted.support_volume_mm3 < 1.);
    CHECK(r.planar.predicted.terrace_length_mm < 1.);
}

TEST_CASE("AutoPlanes on a shallow ramp", "[autoplanes]")
{
    const DynamicPrintConfig config = virtual_printer_config();

    SECTION("suggests a tilted section within the limit and shortens terraces") {
        Model model;
        ModelObject *mo = add_part(model, shallow_ramp(4.));
        const auto r = plan_of(*mo, config);
        INFO(AutoPlanes::to_text(r));
        REQUIRE(r.chosen >= 0);
        const AutoPlanes::Candidate &c = *r.chosen_candidate();
        CHECK(c.feasible);
        check_within_limits(c);
        CHECK(c.predicted.terrace_length_mm < 0.5 * r.planar.predicted.terrace_length_mm);
        CHECK(c.predicted.transitions == 1);
        // The partition the plan implies is exactly what derive_cut_segments() produces.
        const CutPlaneStack st = AutoPlanes::stack_for(CutPlaneStack(), c, false);
        CutSegmentationStatus status;
        const auto segs = derive_cut_segments(merged_model_part_mesh(*mo), st.planes, mo->instances.front()->get_matrix(), 0.4, false, &status);
        CHECK(status == CutSegmentationStatus::Ok);
        CHECK(segs.size() == 2);
    }

    SECTION("instance rotation about Z turns the lean with the part") {
        Model m0, m90;
        ModelObject *a = add_part(m0,  shallow_ramp(4.));
        ModelObject *b = add_part(m90, shallow_ramp(4.), Vec3d(128., 128., 0.), Vec3d(0., 0., 90.));
        const auto ra = plan_of(*a, config), rb = plan_of(*b, config);
        REQUIRE(ra.chosen >= 0);
        REQUIRE(rb.chosen >= 0);
        const auto &pa = ra.chosen_candidate()->planes.back(), &pb = rb.chosen_candidate()->planes.back();
        // Same physical tilt, azimuth turned by 90 degrees (lean toward the ramp's rising side).
        CHECK_THAT(pa.tilt_deg, WithinAbs(pb.tilt_deg, 0.6));
        double d = std::fmod(pb.azimuth_deg - pa.azimuth_deg + 540., 360.) - 180.;
        CHECK_THAT(d, WithinAbs(90., 1.));
        check_within_limits(*rb.chosen_candidate());
    }

    SECTION("a mirrored instance is judged in the world, after the mirror") {
        Model model;
        ModelObject *mo = add_part(model, shallow_ramp(4.), Vec3d(128., 128., 0.), Vec3d::Zero(), true);
        const auto r = plan_of(*mo, config);
        INFO(AutoPlanes::to_text(r));
        REQUIRE(r.chosen >= 0);
        const auto &p = r.chosen_candidate()->planes.back();
        // Mirrored in X, the ramp rises toward -X. A slope's surface normal leans AWAY from the way it
        // rises, and the build direction follows the surface normal, so it leans toward +X -- the mirror
        // image of the unmirrored part, whose build direction leans toward -X.
        CHECK(p.world_build_normal.x() > 0.);
        check_within_limits(*r.chosen_candidate());
        // The object-local plane maps back to the reported world direction.
        CHECK((AutoPlanes::normal_to_world(mo->instances.front()->get_matrix(), p.plane.effective_build_normal()) - p.world_build_normal).norm() < 1e-9);
    }

    SECTION("a printer that cannot tilt gets planar with the reason") {
        DynamicPrintConfig c = virtual_printer_config();
        c.set_deserialize_strict({ { "nonplanar_hotend_cone_height", "0" }, { "nonplanar_gantry_height", "0" }, { "nonplanar_virtual_max_angle", "0" } });
        Model model;
        ModelObject *mo = add_part(model, shallow_ramp(4.));
        const auto r = plan_of(*mo, c);
        CHECK(r.chosen == -1);
        CHECK(r.decision.find("hotend") != std::string::npos);
    }
}

TEST_CASE("AutoPlanes rejects a plane past the selected tilt or clearance limit", "[autoplanes]")
{
    const DynamicPrintConfig config = virtual_printer_config();
    const auto machine = AutoPlanes::MachineCapability::from_config(config);
    const auto process = AutoPlanes::ProcessParams::from_config(config);
    Model model;
    ModelObject *mo = add_part(model, shallow_ramp(4.));
    const Transform3d T = mo->instances.front()->get_matrix();

    auto plane_at = [&](double tilt_deg, double az_deg) {
        CutPlane p;
        const Vec3d dw(std::cos(az_deg * PI / 180.) * std::sin(tilt_deg * PI / 180.),
                       std::sin(az_deg * PI / 180.) * std::sin(tilt_deg * PI / 180.), std::cos(tilt_deg * PI / 180.));
        p.normal = AutoPlanes::normal_to_local(T, dw);
        p.point  = T.inverse() * Vec3d(128., 128., 3.);
        return p;
    };
    SECTION("past the manual 5 degree ceiling") {
        const auto c = AutoPlanes::evaluate(*mo, T, machine, process, { plane_at(8., 0.) }, "too steep");
        CHECK_FALSE(c.feasible);
        REQUIRE_FALSE(c.rejections.empty());
        CHECK(c.rejections.front().find("limit") != std::string::npos);
    }
    SECTION("toward the -Y face, which allows only 6 degrees even without the ceiling") {
        DynamicPrintConfig c2 = config;
        c2.set_deserialize_strict({ { "nonplanar_virtual_max_angle", "0" } });
        const auto m2 = AutoPlanes::MachineCapability::from_config(c2);
        // Leaning toward +Y makes the layer climb toward -Y.
        const auto bad  = AutoPlanes::evaluate(*mo, T, m2, process, { plane_at(9., 90.) }, "toward yneg face");
        const auto good = AutoPlanes::evaluate(*mo, T, m2, process, { plane_at(9., -90.) }, "toward ypos face");
        CHECK_FALSE(bad.feasible);
        CHECK(good.feasible);
    }
    SECTION("within the limit") {
        const auto c = AutoPlanes::evaluate(*mo, T, machine, process, { plane_at(4., 0.) }, "ok");
        CHECK(c.feasible);
        check_within_limits(c);
    }
}

TEST_CASE("AutoPlanes on overhangs, curved tops, arms and a bent rod", "[autoplanes]")
{
    const DynamicPrintConfig config = virtual_printer_config();

    SECTION("an overhang a little flatter than the threshold is tilted past it") {
        Model model;
        ModelObject *mo = add_part(model, overhang_coupon(27.));
        AutoPlanes::Options supports;
        supports.preference = AutoPlanes::Preference::Supports;
        const auto r = plan_of(*mo, config, supports);
        INFO(AutoPlanes::to_text(r));
        REQUIRE(r.planar.predicted.support_volume_mm3 > 10.);
        REQUIRE(r.chosen >= 0);
        check_within_limits(*r.chosen_candidate());
        CHECK(r.chosen_candidate()->predicted.support_volume_mm3 < 0.5 * r.planar.predicted.support_volume_mm3);
        // The coupon's flat top turns into a staircase on tilted layers, and the plan says so.
        CHECK(r.chosen_candidate()->predicted.terrace_length_mm > r.planar.predicted.terrace_length_mm);
    }
    SECTION("balanced weighs the support saved against the flat top it steps") {
        Model model;
        ModelObject *mo = add_part(model, overhang_coupon(27.));
        const auto r = plan_of(*mo, config);
        INFO(AutoPlanes::to_text(r));
        // Either answer is legitimate; what matters is that the candidate's net score reflects both terms.
        REQUIRE_FALSE(r.candidates.empty());
        const auto &best = r.candidates.front();
        CHECK(best.predicted.support_volume_mm3 < r.planar.predicted.support_volume_mm3);
        CHECK(best.predicted.terrace_length_mm > r.planar.predicted.terrace_length_mm);
        // The stepped top is charged: the net score is below what the support saving alone would give.
        const double support_gain = (r.planar.predicted.support_volume_mm3 - best.predicted.support_volume_mm3) /
                                    r.planar.predicted.support_volume_mm3;
        CHECK(best.net_benefit < support_gain - 0.05);
    }
    SECTION("an overhang far flatter than the threshold cannot be cured by 5 degrees") {
        Model model;
        ModelObject *mo = add_part(model, overhang_coupon(12.));
        const auto r = plan_of(*mo, config);
        INFO(AutoPlanes::to_text(r));
        if (r.chosen >= 0) {
            check_within_limits(*r.chosen_candidate());
            CHECK(r.chosen_candidate()->net_benefit >= 0.10);
        }
    }
    SECTION("curved top: any suggestion is within limits and justified") {
        Model model;
        ModelObject *mo = add_part(model, curved_top_block());
        const auto r = plan_of(*mo, config);
        INFO(AutoPlanes::to_text(r));
        if (r.chosen >= 0) {
            check_within_limits(*r.chosen_candidate());
            CHECK(r.chosen_candidate()->net_benefit >= 0.10);
        } else
            CHECK_FALSE(r.decision.empty());
    }
    SECTION("bracket arms leaning both ways") {
        Model model;
        ModelObject *mo = add_part(model, arm_bracket(27.));
        AutoPlanes::Options supports;
        supports.preference = AutoPlanes::Preference::Supports;
        const auto r = plan_of(*mo, config, supports);
        INFO(AutoPlanes::to_text(r));
        REQUIRE(r.chosen >= 0);
        const auto &c = *r.chosen_candidate();
        check_within_limits(c);
        CHECK(c.predicted.support_volume_mm3 < r.planar.predicted.support_volume_mm3);
    }
    SECTION("bent rod with branch analysis: skeleton cuts face the same limits") {
        Model model;
        ModelObject *mo = add_part(model, bent_rod());
        AutoPlanes::Options opt;
        opt.skeleton = AutoPlanes::Options::Skeleton::On;
        const auto r = AutoPlanes::plan(*mo, mo->instances.front()->get_matrix(), config, opt);
        INFO(AutoPlanes::to_text(r));
        CHECK_FALSE(r.skeleton_note.empty());
        for (const auto &c : r.candidates)
            if (c.feasible)
                check_within_limits(c);
        if (r.chosen >= 0)
            check_within_limits(*r.chosen_candidate());
    }
}

TEST_CASE("AutoPlanes preserves manual planes and round-trips", "[autoplanes]")
{
    const DynamicPrintConfig config = virtual_printer_config();
    Model model;
    ModelObject *mo = add_part(model, shallow_ramp(4.));
    // A manual horizontal plane the user authored.
    CutPlane manual;
    manual.point  = Vec3d(0., 0., 1.5) + mo->bounding_box_exact().min - mo->instances.front()->get_offset();
    manual.normal = Vec3d::UnitZ();
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes  = { manual };
    const CutPlaneStack before = mo->cut_planes;

    const auto r = plan_of(*mo, config);
    INFO(AutoPlanes::to_text(r));
    REQUIRE(r.existing.has_value());
    // Planning never edits the model by itself.
    CHECK(mo->cut_planes == before);

    if (r.chosen >= 0) {
        const auto &c = *r.chosen_candidate();
        const CutPlaneStack kept     = AutoPlanes::stack_for(mo->cut_planes, c, true);
        const CutPlaneStack replaced = AutoPlanes::stack_for(mo->cut_planes, c, false);
        REQUIRE(kept.planes.size() >= 2);
        CHECK(kept.planes.front() == manual);
        // "Replace" with keep_existing planning still carries the fixed planes inside the candidate.
        CHECK(replaced.planes.size() == c.planes.size());

        // Undo/redo snapshots and project backups serialise ModelObject cut planes with cereal.
        std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
        { cereal::BinaryOutputArchive oa(ss); oa(kept); }
        CutPlaneStack back;
        { cereal::BinaryInputArchive ia(ss); ia(back); }
        CHECK(back == kept);

        // Project save/reload: Metadata/cut_planes.xml through store_bbs_3mf / load_bbs_3mf.
        mo->cut_planes = kept;
        ScopedTemporaryDir backup("autoplanes_3mf");
        model.set_backup_path(backup.string());
        ScopedTemporaryFile tmp(".3mf");
        PlateData *plate = new PlateData();
        plate->plate_index = 0;
        StoreParams sp;
        const std::string path = tmp.string();
        sp.path = path.c_str();
        sp.model = &model;
        sp.config = const_cast<DynamicPrintConfig*>(&config);
        sp.plate_data_list.push_back(plate);
        sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
        REQUIRE(store_bbs_3mf(sp));
        Model dst; DynamicPrintConfig dcfg; ConfigSubstitutionContext ctx{ ForwardCompatibilitySubstitutionRule::Enable };
        PlateDataPtrs plates; std::vector<Preset*> presets; bool is_bbl = false, is_orca = false; Semver ver;
        REQUIRE(load_bbs_3mf(path.c_str(), &dcfg, &ctx, &dst, &plates, &presets, &is_bbl, &is_orca, &ver, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
        REQUIRE(dst.objects.size() == 1);
        const CutPlaneStack &rt = dst.objects.front()->cut_planes;
        CHECK(rt.enabled == kept.enabled);
        REQUIRE(rt.planes.size() == kept.planes.size());
        for (size_t i = 0; i < rt.planes.size(); ++ i) {
            CHECK((rt.planes[i].normal - kept.planes[i].normal).norm() < 1e-6);
            CHECK(rt.planes[i].has_build_normal == kept.planes[i].has_build_normal);
            CHECK((rt.planes[i].effective_build_normal() - kept.planes[i].effective_build_normal()).norm() < 1e-6);
        }
        release_PlateData_list(plates);
        delete plate;
    }
}

TEST_CASE("AutoPlanes evaluates add and replace with the intended plane order", "[autoplanes]")
{
    const DynamicPrintConfig config = virtual_printer_config();
    Model model;
    ModelObject *mo = add_part(model, shallow_ramp(4.));
    const Transform3d T = mo->instances.front()->get_matrix();
    CutPlane manual;
    manual.point = T.inverse() * Vec3d(128., 128., 1.5);
    manual.normal = AutoPlanes::normal_to_local(T, Vec3d::UnitZ());
    mo->cut_planes.enabled = true;
    mo->cut_planes.user_ordered = true;
    mo->cut_planes.planes = { manual };

    AutoPlanes::Options options;
    options.max_planes = 1;
    SECTION("adding evaluates the existing manual order") {
        const auto result = plan_of(*mo, config, options);
        REQUIRE(result.existing.has_value());
        CHECK(result.existing->user_ordered);
        REQUIRE_FALSE(result.candidates.empty());
        for (const auto &candidate : result.candidates) {
            CHECK(candidate.user_ordered);
            if (! candidate.feasible) continue;
            REQUIRE(candidate.planes.size() == 2);
            CHECK(candidate.planes.front().from_existing);
            const auto stack = AutoPlanes::stack_for(mo->cut_planes, candidate, true);
            CHECK(stack.user_ordered);
            CHECK(stack.planes.front() == manual);
        }
    }
    SECTION("replacing plans from scratch in automatic order") {
        options.keep_existing = false;
        const auto result = plan_of(*mo, config, options);
        REQUIRE_FALSE(result.candidates.empty());
        for (const auto &candidate : result.candidates) {
            CHECK_FALSE(candidate.user_ordered);
            if (! candidate.feasible) continue;
            REQUIRE(candidate.planes.size() == 1);
            CHECK_FALSE(candidate.planes.front().from_existing);
            const auto stack = AutoPlanes::stack_for(mo->cut_planes, candidate, false);
            CHECK_FALSE(stack.user_ordered);
            REQUIRE(stack.planes.size() == 1);
            CHECK(stack.planes.front() == candidate.planes.front().plane);
        }
    }
    SECTION("a zero new-plane budget keeps the current setup") {
        options.max_planes = 0;
        const auto result = plan_of(*mo, config, options);
        CHECK(result.chosen == -1);
        CHECK(result.candidates.empty());
        CHECK(result.existing.has_value());
    }
}

TEST_CASE("AutoPlanes honors cancellation before work and during final selection", "[autoplanes]")
{
    const DynamicPrintConfig config = virtual_printer_config();
    Model model;
    ModelObject *mo = add_part(model, shallow_ramp(4.));
    AutoPlanes::Options options;
    options.skeleton = AutoPlanes::Options::Skeleton::Off;
    AutoPlanes::Progress progress;
    bool canceled = false;
    progress.canceled = [&] { return canceled; };
    SECTION("already canceled") {
        canceled = true;
        progress.report = [](int, const std::string &) { FAIL("A canceled request must not start work"); };
    }
    SECTION("canceled while choosing") {
        progress.report = [&](int percent, const std::string &) { if (percent == 95) canceled = true; };
    }
    const auto result = AutoPlanes::plan(*mo, mo->instances.front()->get_matrix(), config, options, &progress);
    CHECK(result.canceled);
    CHECK(result.chosen == -1);
    CHECK(result.chosen_candidate() == nullptr);
}

// Regression: support of a section whose floor is TILTED in its own slice frame (a horizontal cut under
// layers tilted by a build-direction override) must be handed down to the section below everywhere it
// lands on that floor -- not only in the strip where the floor is lowest. Before the fix the hand-off
// sampled a fixed two-layer window, the parent printed no column under the rest, and the child's
// support started in mid-air (reproduced on the saved Centauri project: gaps up to 2.5 mm).
TEST_CASE("Tilted-floor section support has a foundation", "[autoplanes][cascade]")
{
    DynamicPrintConfig config = virtual_printer_config();
    config.set_deserialize_strict({ { "support_threshold_angle", "30" }, { "support_type", "normal(auto)" },
                                    { "support_on_build_plate_only", "0" }, { "enable_arc_fitting", "0" } });
    Model model;
    // 15 deg overhang: still below the 30 deg threshold after a 5 deg tilt, so the upper section needs
    // support, and its support has to continue down through the lower section to the bed.
    ModelObject *mo = add_part(model, overhang_coupon(15., 10.));
    const Transform3d T = mo->instances.front()->get_matrix();
    CutPlane cp;
    cp.point            = T.inverse() * Vec3d(128., 128., 2.);
    cp.normal           = AutoPlanes::normal_to_local(T, Vec3d::UnitZ());
    const double a      = 5. * PI / 180.;
    cp.build_normal     = AutoPlanes::normal_to_local(T, Vec3d(std::sin(a), 0., std::cos(a)));
    cp.has_build_normal = true;
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes  = { cp };

    Print print;
    print.apply(model, config);
    REQUIRE(print.objects().size() == 2);
    INFO("validate: " << print.validate().string);
    REQUIRE(print.validate().string.empty());
    print.process();
    ScopedTemporaryFile tmp(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(tmp.string(), &result, nullptr);

    MultiplanarAuditParams ap;
    const MultiplanarAuditResult audit = audit_multiplanar_toolpaths(result, tmp.string(), ap);
    INFO(audit.to_text());
    REQUIRE(audit.support.samples > 100);
    // A stair step of up to half a layer where tilted support meets flat support is expected; a
    // column missing underneath shows up as a gap of millimetres.
    CHECK(audit.support.worst_gap_mm < 0.3);
    CHECK(audit.rotary_words == 0);   // fixed-nozzle output carries no rotary axis words
    // Floor gap fill: the bonding layer's beads reach the section below without doubling up on it.
    for (const TransitionStats &t : audit.transitions)
        CHECK(t.overlaps == 0);
    CHECK(audit.model.unfounded == 0);
}

// The seam of a tilted section continues the seam line of the section below instead of restarting
// wherever that section's own placement puts it (reproduced as a 36.8 mm jump on a saved project).
TEST_CASE("Seam continues across a cut-plane transition", "[autoplanes][seam]")
{
    DynamicPrintConfig config = virtual_printer_config();
    config.set_deserialize_strict({ { "seam_position", "aligned" }, { "enable_support", "0" }, { "enable_arc_fitting", "0" } });
    Model model;
    ModelObject *mo = add_part(model, its_make_cube(20., 20., 20.));
    const Transform3d T = mo->instances.front()->get_matrix();
    CutPlane cp;
    cp.point            = T.inverse() * Vec3d(128., 128., 10.);
    cp.normal           = AutoPlanes::normal_to_local(T, Vec3d::UnitZ());
    const double a      = 4. * PI / 180.;
    cp.build_normal     = AutoPlanes::normal_to_local(T, Vec3d(std::sin(a), 0., std::cos(a)));
    cp.has_build_normal = true;
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes  = { cp };

    Print print;
    print.apply(model, config);
    REQUIRE(print.objects().size() == 2);
    REQUIRE(print.validate().string.empty());
    print.process();
    ScopedTemporaryFile tmp(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(tmp.string(), &result, nullptr);
    const MultiplanarAuditResult audit = audit_multiplanar_toolpaths(result, tmp.string(), MultiplanarAuditParams());
    INFO(audit.to_text());
    REQUIRE(audit.seams.size() == 1);
    // A cube's upper section is a full square on every tilted layer, so the seam can sit on the
    // vertical line of the one below throughout.
    CHECK(audit.seams.front().share_within_2mm >= 0.8);
}
