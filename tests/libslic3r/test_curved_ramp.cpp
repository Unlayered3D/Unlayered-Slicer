#ifdef CURVED_TRANSITION_STANDALONE
#include "../catch2/extras/catch_amalgamated.hpp"
#else
#include <catch2/catch_test_macros.hpp>
#endif
#include "libslic3r/CurvedRamp.hpp"

using namespace Slic3r::CurvedTransition;

static std::vector<Ramp::Triangle> ramp_triangles(double slope=.07)
{
    std::array<Ramp::Vertex,8> p{{{{0,0,0}},{{20,0,0}},{{20,20,0}},{{0,20,0}},
        {{0,0,3}},{{20,0,3+20*slope}},{{20,20,3+20*slope}},{{0,20,3}}}};
    const int faces[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
        {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    std::vector<Ramp::Triangle> result;
    for (const auto& f:faces) result.push_back({{p[f[0]],p[f[1]],p[f[2]]}});
    return result;
}

TEST_CASE("Curved ramp certifies all boundary faces and exact roof", "[curved-blend]")
{
    const Ramp r=Ramp::certify(ramp_triangles());
    REQUIRE(std::abs(r.gx-.07)<1e-12);
    REQUIRE(std::abs(r.gy)<1e-12);
    auto f=r.field(.2);
    for (double x:{0.,10.,20.}) {
        REQUIRE(std::abs(f->slice_z(x,10.,r.roof(x,10.))-3.7)<1e-12);
        REQUIRE(std::abs(f->world_z(x,10.,3.7)-r.roof(x,10.))<1e-12);
    }
    auto bad=ramp_triangles(); bad.erase(bad.begin());
    REQUIRE_THROWS(Ramp::certify(bad));
    bad=ramp_triangles(); bad.push_back(bad.front());
    REQUIRE_THROWS(Ramp::certify(bad));
    bad=ramp_triangles(); bad[2][0][2]+=.1;
    REQUIRE_THROWS(Ramp::certify(bad));
    REQUIRE_THROWS(f->world_z(21.,10.,1.));
}

TEST_CASE("Curved ramp schedule lands on the roof within process spacing", "[curved-blend]")
{
    const auto r=Ramp::certify(ramp_triangles(std::tan(5.*3.14159265358979323846/180.)));
    auto f=r.field(.2);
    const auto schedule=r.schedule(*f,.2,.2,.08,.32);
    REQUIRE(schedule.front()==0.);
    REQUIRE(schedule[1]==.2);
    REQUIRE(std::abs(schedule.back()-(r.top_min+r.top_max)*.5)<1e-12);
    for (size_t i=2;i<schedule.size();i+=2) {
        const double step=schedule[i+1]-schedule[i];
        REQUIRE(step<=.2+1e-12);
        REQUIRE(f->spacing_bounds(step).min_graph_distance_mm>=.08-1e-10);
        REQUIRE(f->spacing_bounds(step).max_vertical_mm<=.32+1e-10);
    }
    REQUIRE_THROWS(r.schedule(*f,.2,.2,.18,.21));
    REQUIRE_THROWS(r.schedule(*f,.2,.2,0.,.32));
    // The longer 4-degree user ramp can require a different reference height
    // to respect a tighter printer height range. Every chosen candidate is
    // certified analytically, not accepted merely because it was sampled.
    auto longer=ramp_triangles(std::tan(4.*3.14159265358979323846/180.));
    for(auto& t:longer) for(auto& p:t) p[0]*=2.;
    // Restore the intended 4-degree roof after stretching the X domain.
    for(auto& t:longer) for(auto& p:t) if(p[2]>0.) p[2]=3.+p[0]*std::tan(4.*3.14159265358979323846/180.);
    const auto long_ramp=Ramp::certify(longer);
    bool found=false;
    for(int i=0;i<=32 && !found;++i) {
        const double ref=(long_ramp.top_min+long_ramp.top_max)*.5+(long_ramp.top_max-long_ramp.top_min)*.5*i/32.;
        auto candidate=long_ramp.field(.2,ref);
        try { long_ramp.schedule(*candidate,.2,.2,.08,.28); found=true; }
        catch(const std::invalid_argument&) { }
    }
    REQUIRE(found);
}

TEST_CASE("Curved ramp includes mirrored and diagonal uphill directions", "[curved-blend]")
{
    auto triangles=ramp_triangles();
    for (auto& t:triangles) for (auto& p:t) if (p[2]>0.) p[2] += .02*p[1];
    auto r=Ramp::certify(triangles);
    REQUIRE(std::abs(r.gy-.02)<1e-12);
    for (auto& t:triangles) { for (auto& p:t) p[0]=-p[0]; std::swap(t[1],t[2]); }
    r=Ramp::certify(triangles);
    REQUIRE(std::abs(r.gx+.07)<1e-12);
    REQUIRE(std::abs(r.top_max-4.8)<1e-12);
}

TEST_CASE("Curved ramp subdivision has an analytical chord bound", "[curved-blend]")
{
    const auto r=Ramp::certify(ramp_triangles(.08));
    const auto f=r.field(.2);
    const double length=20.; const int n=Ramp::chord_segments(length,r.slope());
    REQUIRE(r.slope()*length/n/2.<=.005+1e-12);
    for (double w:{.2,1.,2.,3.}) for (int i=0;i<n;++i) {
        const double a=length*i/n,b=length*(i+1)/n;
        for (double t:{.1,.25,.5,.75,.9}) {
            const double linear=(1.-t)*f->world_z(a,5.,w)+t*f->world_z(b,5.,w);
            REQUIRE(std::abs(f->world_z(a+(b-a)*t,5.,w)-linear)<=.005+1e-12);
        }
    }
    REQUIRE(std::abs(Ramp::bead_area(.45,.2)-(.45*.2-.2*.2*(1.-3.14159265358979323846/4.)))<1e-12);
    REQUIRE(Ramp::bead_area(.2,.3)>0.); // upstream narrow-gap convention
    REQUIRE_THROWS(Ramp::bead_area(.02,.3));
    REQUIRE_THROWS(Ramp::bead_area(INFINITY,.2));
    const double bound=Ramp::max_bead_area(.09,.35);
    REQUIRE(bound>Ramp::bead_area(.09,.35)); // the maximum is inside the interval
    for (int i=1;i<=3500;++i)
        REQUIRE(Ramp::bead_area(.09,i*.0001)<=bound+1e-12);
}
