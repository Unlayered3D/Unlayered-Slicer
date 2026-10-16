#include <catch2/catch_all.hpp>
#include "libslic3r/GCode/SeamPlacer.hpp"

using namespace Slic3r;
using namespace Slic3r::SeamPlacerImpl;

TEST_CASE("Section seam continuation respects painted seams and placement constraints", "[cutplanes][seam]")
{
    PrintObjectSeamData::LayerSeams layer;
    layer.perimeters.emplace_back();
    Perimeter &per = layer.perimeters.back();
    per.start_index = 0;
    per.end_index = 4; // exclusive, as used by the seam placer
    per.seam_index = 0;
    for (const Vec3f &p : { Vec3f(0.f, 0.f, 1.f), Vec3f(10.f, 0.f, 1.f),
                           Vec3f(10.f, 10.f, 1.f), Vec3f(0.f, 10.f, 1.f) })
        layer.points.emplace_back(p, per, 0.f, EnforcedBlockedSeamPoint::Neutral);
    const std::vector<Vec2f> target { Vec2f(10.f, 10.f) };

    SECTION("unpainted seam follows the parent seam line") {
        REQUIRE(continuation_seam_candidate(layer, per, target, 60.f) == 2);
    }
    SECTION("a painted enforcer retains the seam chosen by the regular placer") {
        layer.points[0].type = EnforcedBlockedSeamPoint::Enforced;
        layer.points[0].central_enforcer = true;
        per.finalized = true;
        per.final_seam_position = Vec3f(0.f, 0.f, 1.f);
        CHECK_FALSE(continuation_seam_candidate(layer, per, target, 60.f));
        CHECK(per.seam_index == 0);
        CHECK(per.final_seam_position == Vec3f(0.f, 0.f, 1.f));
    }
    SECTION("painting still wins if the nearest candidate to a finalized seam is neutral") {
        layer.points[1].type = EnforcedBlockedSeamPoint::Enforced;
        per.finalized = true;
        per.final_seam_position = Vec3f(4.f, 0.f, 1.f);
        CHECK_FALSE(continuation_seam_candidate(layer, per, target, 60.f));
    }
    SECTION("a blocker cannot become the continuation seam") {
        layer.points[2].type = EnforcedBlockedSeamPoint::Blocked;
        const auto choice = continuation_seam_candidate(layer, per, target, 60.f);
        REQUIRE(choice);
        CHECK(*choice != 2);
    }
    SECTION("continuation cannot select a worse overhang") {
        layer.points[2].overhang = 1.f;
        const auto choice = continuation_seam_candidate(layer, per, target, 60.f);
        REQUIRE(choice);
        CHECK(*choice != 2);
    }
    SECTION("an out of reach seam keeps its original position") {
        CHECK_FALSE(continuation_seam_candidate(layer, per, { Vec2f(100.f, 100.f) }, 60.f));
    }
    SECTION("candidates outside this perimeter do not affect its painting decision") {
        layer.points.emplace_back(Vec3f(0.f, 0.f, 1.f), per, 0.f, EnforcedBlockedSeamPoint::Enforced);
        CHECK(continuation_seam_candidate(layer, per, target, 60.f) == 2);
    }
}
