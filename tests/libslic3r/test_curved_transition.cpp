#ifdef CURVED_TRANSITION_STANDALONE
#include "../catch2/extras/catch_amalgamated.hpp"
#else
#include <catch2/catch_test_macros.hpp>
#endif
#include "libslic3r/CurvedTransition.hpp"
#include <limits>

using namespace Slic3r::CurvedTransition;
namespace {
Field ramp() {
    Options o;
    o.enabled = true;
    o.reference_top_mm = 4.36;
    o.top_min_mm = 3.;
    o.top_max_mm = 3. + 40. * std::tan(4. * 3.14159265358979323846 / 180.);
    o.max_slope_deg = 4.;
    return Field(o, [](double x, double) {
        const double slope = std::tan(4. * 3.14159265358979323846 / 180.);
        return TopSample{3. + slope*x, slope, 0.};
    });
}
}

TEST_CASE("Curved transition preserves bed and final roof", "[curved_transition]") {
    const auto f = ramp();
    for (double x : {0., 10., 20., 30., 40.}) {
        CHECK(f.world_z(x, 0., 0.) == 0.);
        CHECK(f.world_z(x, 0., 0.2) == 0.2);
        CHECK(std::abs(f.world_z(x, 0., 4.36) - f.roof(x, 0.).z) < 1e-12);
        for (double w : {0.5, 1., 2., 3., 4.})
            CHECK(std::abs(f.slice_z(x, 0., f.world_z(x, 0., w)) - w) < 1e-12);
    }
    // A fan of flat tilted layers would have zero second difference here.
    CHECK(std::abs(f.world_z(0., 0., 2.) - 2.*f.world_z(20., 0., 2.) + f.world_z(40., 0., 2.)) > 0.02);
}

TEST_CASE("Curved transition bounds layer separation and fails closed", "[curved_transition]") {
    const auto f = ramp();
    CHECK_NOTHROW(f.require_spacing(0.16, 0.08, 0.32));
    CHECK_THROWS_AS(f.require_spacing(0.2, 0.08, 0.32), std::invalid_argument);
    const auto b = f.spacing_bounds(0.16);
    for (int xi=0;xi<=80;++xi) for(int k=1;k<=26;++k) {
        const double x=xi*0.5, w=0.2+k*0.16;
        const double gap=f.world_z(x,0.,w)-f.world_z(x,0.,w-0.16);
        CHECK(gap >= b.min_vertical_mm-1e-10);
        CHECK(gap <= b.max_vertical_mm+1e-10);
    }
}

TEST_CASE("Curved transition changes slope continuously without overshoot", "[curved_transition]") {
    const auto f=ramp();
    const double slope=std::tan(4.*3.14159265358979323846/180.);
    CHECK(std::abs(f.gradient(20.,0.,0.20001).dx) < 1e-10);
    CHECK(std::abs(f.gradient(20.,0.,4.35999).dx-slope) < 1e-10);
    for(int xi=0;xi<=40;++xi) for(int k=0;k<=100;++k) {
        const auto g=f.gradient(xi,0.,4.36*k/100.);
        CHECK(g.dx >= -1e-12);
        CHECK(g.dx <= slope+1e-10);
    }
}

TEST_CASE("Disabled curved transition is an exact identity", "[curved_transition]") {
    Field f(Options{}, {});
    for(double z : {-5.,0.,0.2,1.,10.,100.}) {
        CHECK(f.slice_z(10.,10.,z)==z);
        CHECK(f.world_z(10.,10.,z)==z);
    }
}

TEST_CASE("Curved transition rejects invalid fields and nonfinite input", "[curved_transition]") {
    auto f=ramp();
    CHECK_THROWS_AS(f.world_z(0.,0.,std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    Options o; o.enabled=true; o.reference_top_mm=3.; o.top_min_mm=3.; o.top_max_mm=6.;
    CHECK_THROWS_AS(Field(o, [](double,double){return TopSample{3.,0.,0.};}), std::invalid_argument);
    o.reference_top_mm=4.5;
    Field bad(o, [](double,double){return TopSample{4.,1.,0.};});
    CHECK_THROWS_AS(bad.world_z(0.,0.,2.), std::invalid_argument);
}
