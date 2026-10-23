#ifdef CURVED_TRANSITION_STANDALONE
#include "../catch2/extras/catch_amalgamated.hpp"
#else
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#endif
#include "libslic3r/CurvedRegions.hpp"
using namespace Slic3r::CurvedTransition;

TEST_CASE("Independent roof regions preserve planar bridge zones and invert continuously", "[curved-blend][regions]")
{
    Regions f({-20.,20.,-10.,10.,12.,0.},{{-8.,0.,4.,3.,4.,.04,0.},{8.,0.,4.,5.,6.,0.,-.04}},.08);
    REQUIRE(f.identity_on_box(-20.,20.,-10.,10.,0.,3.));
    REQUIRE(f.identity_on_box(-2.,2.,-10.,10.,0.,12.));
    REQUIRE_FALSE(f.identity_on_box(-10.,-6.,-1.,1.,3.,4.));
    REQUIRE(f.world_z(0.,0.,10.)==10.);
    REQUIRE(f.gradient(-8.,0.,8.).dx==Catch::Approx(.04));
    REQUIRE(f.gradient(8.,0.,8.).dy==Catch::Approx(-.04));
    for(double x=-20.;x<=20.;x+=.5) for(double y=-5.;y<=5.;y+=.5) for(double z=0.;z<=12.;z+=.4) {
        const double w=f.slice_z(x,y,z);
        REQUIRE(std::abs(f.world_z(x,y,w)-z)<1e-10);
        REQUIRE(std::abs(w-z)<=f.max_displacement_mm()+1e-12);
        const auto g=f.gradient(x,y,w);
        REQUIRE(std::hypot(g.dx,g.dy)<=f.slope_bound()+1e-10);
        const double dz=(f.slice_z(x,y,z+1e-5)-f.slice_z(x,y,z-1e-5))/2e-5;
        REQUIRE(dz>=f.jacobian_min()-1e-8);REQUIRE(dz<=f.jacobian_max()+1e-8);
    }
    f.require_spacing(.2,.08,.32);
    REQUIRE_THROWS(Regions({-10.,10.,-10.,10.,12.,0.},{{0.,0.,4.,3.,3.01,.04,0.}},.08));
    REQUIRE_THROWS(Regions({-10.,10.,-10.,10.,12.,0.},{{0.,0.,4.,3.,4.,.04,0.},{2.,0.,4.,3.,4.,0.,.04}},.08));
}

TEST_CASE("Regional selection retains useful roofs above protected undersides", "[curved-blend][regions][automatic]")
{
    std::vector<Ramp::Triangle> faces{{{{-10.,-10.,10.},{10.,-10.,11.},{0.,10.,10.5}}},
        {{{-10.,-10.,3.},{0.,10.,3.},{10.,-10.,3.}}}};
    const auto p=select_regions(faces,.2,.2,.08,1.,false);
    REQUIRE_FALSE(p.empty());
    Regions f({-10.,10.,-10.,10.,11.,0.},p,.08);
    REQUIRE(f.identity_on_box(-10.,10.,-10.,10.,0.,3.6));
    REQUIRE(f.slope_bound()<=.08);
    // Smaller local angles can now fit this shell. They still must not reach
    // below it, including where a disk covers a higher point than its seed.
    const auto sparse=select_regions(faces,.2,.2,.08,1.,true);
    REQUIRE_FALSE(sparse.empty());
    Regions shell({-10.,10.,-10.,10.,11.,0.},sparse,.08);
    for(int i=0;i<40;++i) for(int j=0;j<40-i;++j) {
        const double u=(i+.3)/40.,v=(j+.3)/40.;
        const double x=-10.+20.*u+10.*v,y=-10.+20.*v,z=10.+u+.5*v;
        REQUIRE(shell.world_z(x,y,z-1.)==z-1.);
    }
    for(auto& v:faces[1]) v[2]=10.5;
    REQUIRE(select_regions(faces,.2,.2,.08,1.,false).empty());
}

TEST_CASE("Regional sparse selection ranks whole roof alignment instead of patch centre alone", "[curved-blend][regions][automatic]")
{
    std::vector<Ramp::Triangle> narrow{{{{0.,0.,10.},{2.,0.,10.1},{2.,2.,10.1}}},
        {{{0.,0.,10.},{2.,2.,10.1},{0.,2.,10.}}},
        {{{0.,0.,3.},{2.,2.,3.},{2.,0.,3.}}},{{{0.,0.,3.},{0.,2.,3.},{2.,2.,3.}}}};
    const auto p=select_regions(narrow,.2,.2,.08,1.,true);
    REQUIRE_FALSE(p.empty());
    Regions f({0.,2.,0.,2.,10.1,0.},p,.08);
    double before=0.,after=0.;
    // Independent denser XY integration across the complete roof, including taper.
    for(int i=0;i<100;++i) for(int j=0;j<100;++j) {
        const double x=(i+.5)*.02,y=(j+.5)*.02;
        const auto g=f.logical_roof_gradient(x,y,10.+.05*x,.05,0.);
        before+=.05*.05;after+=g.dx*g.dx+g.dy*g.dy;
    }
    REQUIRE(after<before);
    REQUIRE(f.identity_on_box(0.,2.,0.,2.,0.,9.));
}

TEST_CASE("Regional contact bounds use the facet height inside the patch", "[curved-blend][regions][automatic]")
{
    const Ramp::Triangle contact{{{-10.,-10.,1.},{0.,10.,20.},{10.,-10.,1.}}};
    REQUIRE(regional_contact_height(contact,-2.,2.,-9.,-8.)==Catch::Approx(2.9));
    REQUIRE(regional_contact_height(contact,-2.,2.,8.,9.)==Catch::Approx(19.05));
    REQUIRE(std::isinf(regional_contact_height(contact,20.,21.,20.,21.)));
    // A high remote end of this underside used to suppress the lower roof.
    std::vector<Ramp::Triangle> faces{{{{-10.,-10.,10.},{10.,-10.,10.},{0.,-2.,10.4}}},contact};
    REQUIRE_FALSE(select_regions(faces,.2,.2,.08,1.,false).empty());
    // A genuinely close local contact must still suppress that same roof.
    faces[1]={{{-10.,-10.,10.3},{0.,-2.,10.3},{10.,-10.,10.3}}};
    REQUIRE(select_regions(faces,.2,.2,.08,1.,false).empty());
}

TEST_CASE("Regional selection chooses a useful lower local angle inside a thin roof shell", "[curved-blend][regions][automatic]")
{
    const double allowed=std::tan(5.*3.14159265358979323846/180.);
    const std::vector<Ramp::Triangle> roof{{{{0.,0.,10.},{40.,0.,12.},{40.,20.,12.}}},
        {{{0.,0.,10.},{40.,20.,12.},{0.,20.,10.}}}};
    const auto patches=select_regions(roof,.2,.2,allowed,1.,true);
    REQUIRE_FALSE(patches.empty());
    Regions field({0.,40.,0.,20.,12.,0.},patches,allowed);
    // The selected local tilt is below both the roof slope and printer ceiling.
    REQUIRE(field.slope_bound()<.05);
    REQUIRE(field.slope_bound()>std::tan(.5*3.14159265358979323846/180.));
    double before=0.,after=0.;size_t changed=0;
    // Denser independent Cartesian integration includes the taper and all of
    // the roof, rather than reusing the selector's per-triangle samples.
    for(int i=0;i<200;++i) for(int j=0;j<100;++j) {
        const double x=(i+.5)*.2,y=(j+.5)*.2,z=10.+.05*x;
        const auto q=field.logical_roof_gradient(x,y,z,.05,0.);
        before+=.05*.05;after+=q.dx*q.dx+q.dy*q.dy;
        if(std::abs(field.slice_z(x,y,z)-z)>1e-5) ++changed;
        REQUIRE(field.world_z(x,y,z-1.)==z-1.);
    }
    REQUIRE(changed>100);
    REQUIRE(after<before);
}

TEST_CASE("Regional logical-height blends reach five degrees without transition slope overshoot", "[curved-blend][regions][clearance]")
{
    const double g=std::tan(5.*3.14159265358979323846/180.);
    Regions f({-4.,4.,-4.,4.,8.,0.},{{0.,0.,3.,2.,4.,0.,-g}},g);
    REQUIRE(std::abs(f.gradient(0.,0.,5.).dy+g)<1e-12);
    for(int i=-12;i<=12;++i) for(int j=-12;j<=12;++j) for(int k=0;k<=24;++k) {
        const double x=i*.25,y=j*.25,w=1.5+k*.125,z=f.world_z(x,y,w);
        const auto q=f.gradient(x,y,w);
        REQUIRE(std::hypot(q.dx,q.dy)<=g+1e-12);
        REQUIRE(std::abs(f.slice_z(x,y,z)-w)<1e-10);
        const double e=1e-4;
        REQUIRE(std::abs((f.world_z(x+e,y,w)-f.world_z(x-e,y,w))/(2.*e)-q.dx)<1e-8);
        REQUIRE(std::abs((f.world_z(x,y+e,w)-f.world_z(x,y-e,w))/(2.*e)-q.dy)<1e-8);
        const double jac=(f.slice_z(x,y,z+e)-f.slice_z(x,y,z-e))/(2.*e);
        REQUIRE(jac>=f.jacobian_min()-1e-8);REQUIRE(jac<=f.jacobian_max()+1e-8);
    }
    f.require_spacing(.2,.08,.32);
}

TEST_CASE("Regional clearance bounds every taper direction for asymmetric and different printer envelopes", "[curved-blend][regions][clearance]")
{
    const double pi=3.14159265358979323846,g=std::tan(5.*pi/180.);
    const auto clearance=[&](double xp,double xn,double yp,double yn) {
        RegionalClearance c;
        for(double x:{1./std::tan(xp*pi/180.),-1./std::tan(xn*pi/180.)})
            for(double y:{1./std::tan(yp*pi/180.),-1./std::tan(yn*pi/180.)}) c.corners.push_back({x,y});
        return c;
    };
    // Synthetic instances of configured envelopes, not physical qualification.
    for(const auto& c:{clearance(7.,10.,15.,6.),clearance(12.,12.,12.,12.),clearance(3.,3.,3.,3.)}) {
        for(int heading=0;heading<16;++heading) {
            const double angle=heading*pi/8.,gx=std::cos(angle),gy=std::sin(angle);
            const double cap=c.target_limit(gx,gy,g);
            Regions f({-4.,4.,-4.,4.,8.,0.},{{0.,0.,3.,2.,4.,gx*cap,gy*cap}},g,c);
            for(int r=0;r<=20;++r) for(int a=0;a<48;++a) for(double w:{2.1,3.,3.9,5.}) {
                const double x=3.*r/20.*std::cos(a*pi/24.),y=3.*r/20.*std::sin(a*pi/24.);
                const auto q=f.gradient(x,y,w);
                REQUIRE(std::hypot(q.dx,q.dy)<=g+1e-12);
                for(const auto& corner:c.corners) REQUIRE(corner.dx*q.dx+corner.dy*q.dy<=1.+1e-12);
            }
        }
    }
    const auto centauri=clearance(7.,10.,15.,6.);
    REQUIRE(std::abs(centauri.target_limit(0.,-1.,g)-g)<1e-12);
    const auto tight=clearance(3.,3.,3.,3.);
    REQUIRE(tight.target_limit(0.,-1.,g)<g);
    REQUIRE_THROWS(Regions({-4.,4.,-4.,4.,8.,0.},{{0.,0.,3.,2.,4.,0.,-g}},g,tight));
}

TEST_CASE("Regional inverse curvature bounds stay local to each patch and blend slab", "[curved-blend][regions][mesh]")
{
    Regions f({-12.,20.,-9.,9.,12.,0.},{{-10.,0.,1.,3.,3.2,.04,0.},
        {10.,0.,8.,3.,5.,0.,-.04}},.08);
    const double global=f.inverse_hessian_bound();
    const double wide=f.inverse_hessian_bound_box(9.,11.,-1.,1.,3.,5.);
    const double saturated=f.inverse_hessian_bound_box(9.,11.,-1.,1.,6.,10.);
    REQUIRE(wide<global);
    REQUIRE(saturated<wide);
    REQUIRE(saturated==Catch::Approx(22.*.04/8.));
    REQUIRE(f.inverse_hessian_bound_box(-12.,20.,-9.,9.,0.,3.)==0.);
    REQUIRE(f.inverse_hessian_bound_box(-1.,1.,-1.,1.,3.,10.)==0.);
    // Independent physical-space directional second differences check the
    // inverse map, including mixed XY/Z curvature within both transitions.
    const double e=2e-4;
    const std::array<std::array<double,3>,6> directions{{{{1.,0.,0.}},{{0.,1.,0.}},{{0.,0.,1.}},
        {{.7071067811865475,.7071067811865475,0.}},{{.7071067811865475,0.,.7071067811865475}},
        {{0.,.7071067811865475,.7071067811865475}}}};
    for(const auto& p:f.patches()) for(int i=-6;i<=6;++i) for(int j=-6;j<=6;++j) for(int k=0;k<=12;++k) {
        const double x=p.x+i*p.radius/6.,y=p.y+j*p.radius/6.;
        const double z=p.lo-.05+k*(p.hi-p.lo+.2)/12.;
        const double bound=f.inverse_hessian_bound_box(x-e,x+e,y-e,y+e,z-e,z+e);
        for(const auto& v:directions) {
            const double difference=(f.slice_z(x+e*v[0],y+e*v[1],z+e*v[2])-
                2.*f.slice_z(x,y,z)+f.slice_z(x-e*v[0],y-e*v[1],z-e*v[2]))/(e*e);
            REQUIRE(std::abs(difference)<=bound+2e-4);
        }
    }
}
