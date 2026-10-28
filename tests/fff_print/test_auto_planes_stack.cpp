#include <catch2/catch_all.hpp>

#include "libslic3r/AutoPlanes.hpp"

using namespace Slic3r;

TEST_CASE("Applying auto planes preserves the evaluated enabled planes", "[autoplanes][stack]")
{
    CutPlane manual;
    manual.point = Vec3d(0., 0., 2.);
    CutPlane proposed;
    proposed.point = Vec3d(0., 0., 5.);
    AutoPlanes::PlannedPlane pp;
    pp.plane = proposed;
    AutoPlanes::Candidate candidate;
    candidate.planes.push_back(pp);

    CutPlaneStack existing;
    existing.planes.push_back(manual);

    SECTION("a switched-off stack stays inert when adding a suggestion") {
        const CutPlaneStack applied = AutoPlanes::stack_for(existing, candidate, true);
        REQUIRE(applied.enabled);
        REQUIRE(applied.planes.size() == 2);
        CHECK_FALSE(applied.planes.front().enabled);
        CHECK(applied.planes.front().point == manual.point);
        CHECK(applied.planes.back() == proposed);
        CHECK(existing.planes.front().enabled); // applying does not mutate its input
        CHECK_FALSE(existing.enabled);
    }
    SECTION("an individually disabled plane remains disabled") {
        existing.enabled = true;
        existing.planes.front().enabled = false;
        const CutPlaneStack applied = AutoPlanes::stack_for(existing, candidate, true);
        CHECK_FALSE(applied.planes.front().enabled);
        CHECK(applied.planes.back() == proposed);
    }
    SECTION("a replacement contains only the newly evaluated partition") {
        existing.enabled = true;
        const CutPlaneStack applied = AutoPlanes::stack_for(existing, candidate, false);
        REQUIRE(applied.planes.size() == 1);
        CHECK(applied.planes.front() == proposed);
    }
}

TEST_CASE("Applying auto planes retains the ordering used for evaluation", "[autoplanes][stack]")
{
    CutPlaneStack existing;
    existing.enabled = true;
    existing.user_ordered = true;
    existing.planes.emplace_back();
    AutoPlanes::Candidate candidate;
    candidate.planes.emplace_back();

    SECTION("manual ordering survives a plan evaluated in manual order") {
        candidate.user_ordered = true;
        CHECK(AutoPlanes::stack_for(existing, candidate, true).user_ordered);
        CHECK(AutoPlanes::stack_for(existing, candidate, false).user_ordered);
    }
    SECTION("a replacement evaluated in automatic order uses automatic order") {
        CHECK_FALSE(AutoPlanes::stack_for(existing, candidate, false).user_ordered);
    }
}

TEST_CASE("Auto plane normals round-trip through a scaled mirrored instance", "[autoplanes][stack]")
{
    Transform3d transform = Transform3d::Identity();
    transform.rotate(Eigen::AngleAxisd(0.4, Vec3d::UnitZ()));
    transform.scale(Vec3d(-2., 0.5, 1.5));
    const Vec3d world_normal = Vec3d(0.05, -0.03, 1.).normalized();
    const Vec3d local_normal = AutoPlanes::normal_to_local(transform, world_normal);
    CHECK((AutoPlanes::normal_to_world(transform, local_normal) - world_normal).norm() < 1e-12);
}
