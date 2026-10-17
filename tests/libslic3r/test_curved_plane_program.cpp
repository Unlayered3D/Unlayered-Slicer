#ifdef CURVED_TRANSITION_STANDALONE
#include "../catch2/extras/catch_amalgamated.hpp"
#else
#include <catch2/catch_test_macros.hpp>
#endif
#include "libslic3r/CurvedPlaneProgram.hpp"
#include "libslic3r/CurvedRamp.hpp"
#include "libslic3r/AutomaticCurvedSelection.hpp"

using namespace Slic3r::CurvedTransition;

TEST_CASE("Automatic curved direction requires a useful bounded finish estimate", "[curved-blend][automatic]")
{
    const std::vector<Ramp::Triangle> roof{{{{0.,0.,3.},{20.,0.,4.},{0.,20.,3.}}}};
    const auto target=automatic_target(roof,.08);
    REQUIRE(target.useful);REQUIRE(target.estimated_reduction>.99);
    REQUIRE(std::abs(target.gx-.05)<1e-12);REQUIRE(std::abs(target.gy)<1e-12);
    REQUIRE_FALSE(automatic_target(roof,0.).useful);
    const auto capped=automatic_target(roof,.04);
    REQUIRE(capped.useful);REQUIRE(std::hypot(capped.gx,capped.gy)<=.030000001);
    auto flat=roof;flat[0][1][2]=3.;
    REQUIRE_FALSE(automatic_target(flat,.08).useful);
    auto symmetric=roof;symmetric.push_back({{{0.,0.,3.},{20.,0.,2.},{0.,20.,3.}}});
    REQUIRE_FALSE(automatic_target(symmetric,.08).useful);
}

TEST_CASE("Automatic direction does not mistake an unsupported underside for finish benefit", "[curved-blend][automatic]")
{
    std::vector<Ramp::Triangle> surfaces{{{{0.,0.,3.},{20.,0.,4.},{0.,20.,3.}}},
                                       {{{0.,0.,1.},{0.,20.,1.},{20.,0.,1.}}}};
    const auto unsupported=automatic_target(surfaces,.08);
    REQUIRE_FALSE(unsupported.useful);REQUIRE(unsupported.needs_support_planning);
    for (auto& p:surfaces[1]) p[2]=0.;
    REQUIRE(automatic_target(surfaces,.08).useful);
}

TEST_CASE("Plane field blends successive directions without crossing layers", "[curved-blend][planes]")
{
    ModelDomain d{-10.,10.,-8.,8.,20.,.07};
    PlaneProgram p(d,.2,.2,{{5.,.07,0.},{13.,-.04,.04}});
    REQUIRE(p.blends().size()==2);
    REQUIRE(p.inverse_hessian_bound()>0.);
    // A single intermediate layer curves in XY, rather than being one tilted
    // flat plane. Its midpoint is not the chord between opposing sides.
    const auto& blend=p.blends().front();
    const double w=(blend.lo+blend.hi)*.5;
    REQUIRE(std::abs(p.world_z(-10.,0.,w)+p.world_z(10.,0.,w)-2.*p.world_z(0.,0.,w))>.001);
    for(double x:{-10.,-5.,0.,10.}) for(double y:{-8.,0.,8.}) {
        REQUIRE(p.world_z(x,y,.2)==.2);
        double last=.2;
        for(int i=1;i<=200;++i) {
            const double w=.2+i*.1,z=p.world_z(x,y,w);
            REQUIRE(z>last);
            REQUIRE(z-last>=.05-1e-9);
            REQUIRE(z-last<=.15+1e-9);
            REQUIRE(std::abs(p.slice_z(x,y,z)-w)<1e-10);
            const auto g=p.gradient(x,y,w);
            REQUIRE(std::hypot(g.dx,g.dy)<=p.slope_bound()+1e-10);
            const double eps=1e-4;
            if(x>-9. && x<9.) REQUIRE(std::abs((p.world_z(x+eps,y,w)-p.world_z(x-eps,y,w))/(2.*eps)-g.dx)<1e-8);
            last=z;
        }
    }
    for(const auto& b:p.blends()) {
        const auto a=p.gradient(0.,0.,b.lo),z=p.gradient(0.,0.,b.hi);
        REQUIRE(std::abs(a.dx-b.from_x)<1e-12);
        REQUIRE(std::abs(z.dx-b.from_x-b.delta_x)<1e-12);
        const double epsilon=1e-5;
        REQUIRE(std::abs(p.gradient(0.,0.,b.lo+epsilon).dx-a.dx)<1e-12);
        REQUIRE(std::abs(p.gradient(0.,0.,b.hi-epsilon).dx-z.dx)<1e-12);
    }
    Ramp scheduler{};
    const auto layers=scheduler.schedule(p,.2,.2,.08,.28);
    REQUIRE(std::abs(layers.back()-p.reference_top_mm())<1e-10);
}

TEST_CASE("Plane field refuses infeasible directions and bounds", "[curved-blend][planes]")
{
    const ModelDomain d{-10.,10.,-8.,8.,10.,.07};
    REQUIRE_THROWS(PlaneProgram(d,.2,.2,{}));
    REQUIRE_THROWS(PlaneProgram(d,.2,.2,{{.1,.07,0.}}));
    REQUIRE_THROWS(PlaneProgram(d,.2,.2,{{3.,.5,0.}}));
    REQUIRE_THROWS(PlaneProgram(d,.2,.2,{{3.,.07,0.},{3.1,-.07,0.}}));
    PlaneProgram p(d,.2,.2,{{4.,.07,0.}});
    REQUIRE_THROWS(p.world_z(11.,0.,5.));
}

TEST_CASE("Plane field certifies exact affine physical slabs without skipping transitions", "[curved-blend][planes]")
{
    const PlaneProgram p({-10.,10.,-8.,8.,30.,.1},.2,.2,
        {{6.,.03,0.},{20.,-.02,.01}},.1);
    const auto a=p.blends().front(),b=p.blends().back();
    REQUIRE(p.inverse_hessian_bound_box(-10.,10.,-8.,8.,.2,a.lo)==0.);
    REQUIRE(p.inverse_hessian_bound_box(-10.,10.,-8.,8.,a.hi,b.lo)==0.);
    REQUIRE(p.inverse_hessian_bound_box(-10.,10.,-8.,8.,b.hi,30.)==0.);
    REQUIRE(p.inverse_hessian_bound_box(-10.,10.,-8.,8.,a.lo,a.hi)>0.);
    REQUIRE(p.inverse_hessian_bound_box(-10.,10.,-8.,8.,a.lo-.01,a.lo+.01)>0.);
    // Independently sample a physical chord wholly in the middle affine slab.
    const double z=(a.hi+b.lo)*.5;
    const double start=p.slice_z(-8.,-5.,z),end=p.slice_z(8.,5.,z+.1);
    for(int i=0;i<=20;++i) {
        const double t=i/20.;
        REQUIRE(std::abs(p.slice_z(-8.+16.*t,-5.+10.*t,z+.1*t)-
            (start+t*(end-start)))<1e-12);
    }
    const double w=b.hi+p.max_displacement_mm()+1.;
    REQUIRE(p.affine_on_box(-10.,10.,-8.,8.,w-.2,w));
    REQUIRE_FALSE(p.affine_on_box(-10.,10.,-8.,8.,a.lo,a.hi));
}
