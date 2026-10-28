#ifdef CURVED_TRANSITION_STANDALONE
#include "../catch2/extras/catch_amalgamated.hpp"
#else
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#endif
#include "libslic3r/CurvedJointPlanning.hpp"
#include "libslic3r/CurvedPathOptimization.hpp"
#include "libslic3r/CurvedTravelClearance.hpp"
#include "libslic3r/CurvedTravelPlan.hpp"
using namespace Slic3r::CurvedTransition;

TEST_CASE("Shared travel costing measures raised detours and retains fallback clearance", "[curved-blend][motion][cost]")
{
    TravelClearance map;map.configure(125.,{7.,10.,15.,6.},14.);
    map.add({190.,190.,30.},{195.,190.,30.},.225,0.);
    map.add({10.,10.,2.},{15.,10.,2.},.225,0.);
    TravelState state{{15.,10.,2.},true,30.,0.,45.};
    const TravelSettings settings{2.,200.,80.,3.};
    const std::vector<TravelClearance::Point> detour{{15.,12.,2.},{10.,12.,2.}};
    const auto local=plan_travel(state,detour,settings,map);
    REQUIRE(local.local_route);REQUIRE(local.xy_mm==Catch::Approx(7.));
    REQUIRE(local.safe_z>=18.);REQUIRE(local.safe_z<32.);
    REQUIRE(local.seconds==Catch::Approx((2.*local.safe_z-4.)/3.+7./80.));
    state.position_known=false;
    const auto unknown=plan_travel(state,detour,settings,map);
    REQUIRE_FALSE(unknown.local_route);REQUIRE(unknown.safe_z>=45.);
    REQUIRE(unknown.seconds>local.seconds);
    state.position_known=true;state.position[2]=48.007;
    REQUIRE(plan_travel(state,detour,settings,map).safe_z==48.007);
    state.position={15.,10.,2.};map.invalidate();
    const auto incomplete=plan_travel(state,detour,settings,map);
    REQUIRE_FALSE(incomplete.local_route);REQUIRE(incomplete.safe_z>=32.001);
    REQUIRE_THROWS(plan_travel(state,detour,{2.,20.,80.,3.},map));
    REQUIRE(plan_travel(state,{state.position},settings,map).no_op);
}

TEST_CASE("Benefit breakdown separates signed surface gains from time and material costs", "[curved-blend][joint][cost]")
{
    const SurfaceBenefit before{10.,20.,100.},after{5.,25.,100.};
    GeneratedCost planar;planar.model_mm3=100.;planar.support_mm3=20.;planar.motion_seconds=100.;
    auto candidate=planar;candidate.model_mm3=110.;candidate.support_mm3=10.;candidate.motion_seconds=120.;
    const auto b=planning_breakdown(before,after,planar,candidate);
    REQUIRE(b.roof==Catch::Approx(.5));REQUIRE(b.under==Catch::Approx(-.25));
    REQUIRE(b.time==Catch::Approx(.2));REQUIRE(b.time_seconds==20.);
    REQUIRE(b.support_mm3==10.);REQUIRE(b.material_mm3==10.);
    REQUIRE(b.score()==Catch::Approx(.46));
    REQUIRE(planning_score(before,after,planar,candidate)==b.score());
    const auto zero=planning_breakdown({}, {}, {}, {});
    REQUIRE(zero.score()==0.);REQUIRE(std::isfinite(zero.time));
    candidate.motion_seconds=80.;REQUIRE(planning_breakdown(before,after,planar,candidate).time_seconds==-20.);
}

TEST_CASE("Planning v2 metadata preserves separate metrics and safely imports legacy reports", "[curved-blend][joint][report]")
{
    RegionDecision a{3,0,false,12.,-4.,20.,6.,2.5,-.03,64.,20.,"Roof candidate"},b;
    a.has_breakdown=true;a.roof_gain_percent=5.;a.underside_gain_percent=-2.;
    a.time_delta_seconds=12.;a.time_delta_percent=23.;a.support_saved_mm3=-4.;a.model_added_mm3=2.;
    a.travel_seconds=30.;a.travel_routes=10;a.local_routes=7;
    a.rejection_reason="Lower score than \"Planar\"; candidate is valid";
    REQUIRE(RegionDecision::decode(a.encode(),b));
    REQUIRE(b.has_breakdown);REQUIRE(b.roof_gain_percent==5.);REQUIRE(b.underside_gain_percent==-2.);
    REQUIRE(b.time_delta_seconds==12.);REQUIRE(b.time_delta_percent==23.);
    REQUIRE(b.support_saved_mm3==-4.);REQUIRE(b.local_routes==7);
    REQUIRE(b.rejection_reason==a.rejection_reason);
    REQUIRE_FALSE(RegionDecision::decode(a.encode()+" unexpected",b));
    auto bad=a;bad.local_routes=11;REQUIRE_FALSE(RegionDecision::decode(bad.encode(),b));
    bad=a;bad.time_delta_seconds=INFINITY;REQUIRE_FALSE(RegionDecision::decode(bad.encode(),b));
    REQUIRE(RegionDecision::decode("v1 4 0 1 10 20 4 3 2.5 0.25 45 12 chosen legacy roof",b));
    REQUIRE_FALSE(b.has_breakdown);REQUIRE(b.reason=="chosen legacy roof");
    a.reason=std::string(400,'"');a.rejection_reason=std::string(400,'\\');
    REQUIRE(a.encode().size()<=1024);REQUIRE(RegionDecision::decode(a.encode(),b));
}

TEST_CASE("Curved ordering refines travel cost without reversing or dropping paths", "[curved-blend][motion]")
{
    const std::vector<PathEnds> paths{{{10,0,1},{11,0,1}},{{100,0,3},{101,0,3}},
        {{20,0,2},{21,0,2}},{{30,0,1},{31,0,1}}};
    const std::array<double,3> start{0,0,1};
    const auto cost=[&](const std::vector<size_t>& order) {
        auto previous=start;double sum=0.;
        for(const auto i:order) {sum+=route_cost(previous,paths[i].first,80.,5.,5.);previous=paths[i].last;}
        return sum;
    };
    const auto order=refine_path_order(paths,start,80.,5.,5.);
    auto sorted=order;std::sort(sorted.begin(),sorted.end());
    REQUIRE(sorted==std::vector<size_t>{0,1,2,3});
    REQUIRE(cost(order)<cost({0,1,2,3}));
    REQUIRE(order==refine_path_order(paths,start,80.,5.,5.));
    REQUIRE(route_cost(start,start,80.,5.,5.)==0.);
}

TEST_CASE("Adaptive curved chords retain a certified positional error", "[curved-blend][motion]")
{
    const ModelDomain domain{-10,10,-10,10,30,.08};
    Regions field(domain,{{0,0,9,1,4,.02,0}},.08);
    const double w=8.;const size_t fine_count=160;
    std::vector<std::array<double,3>> points;
    for(size_t i=0;i<=fine_count;++i) {const double x=-8.+16.*i/fine_count;points.push_back({x,0,field.world_z(x,0,w)});}
    const auto ends=bounded_chord_ends(field,points);
    REQUIRE(ends.size()<fine_count/2);
    size_t begin=0;
    for(size_t end:ends) {
        for(int i=0;i<=100;++i) {
            const double t=i/100.;const auto& a=points[begin];const auto& b=points[end];
            const double x=a[0]+t*(b[0]-a[0]);
            REQUIRE(std::abs(field.world_z(x,0,w)-(a[2]+t*(b[2]-a[2])))<=.00500001);
        }
        begin=end;
    }
    REQUIRE(begin==fine_count);
    Identity flat;REQUIRE(bounded_chord_ends(flat,points)==std::vector<size_t>{fine_count});
    Options o;o.enabled=true;o.reference_top_mm=5.;o.top_min_mm=4.;o.top_max_mm=6.;o.max_slope_deg=5.;
    Field unknown(o,[](double,double){return TopSample{5.,0.,0.};});
    REQUIRE(bounded_chord_ends(unknown,points).size()==fine_count);
}

TEST_CASE("Route clearance includes off-axis material, asymmetric head faces and gantry", "[curved-blend][motion]")
{
    TravelClearance map;map.configure(125.,{7.,10.,15.,6.},14.);
    const TravelClearance::Point start{0,0,2};
    const std::vector<TravelClearance::Point> route{{10,0,2}};
    REQUIRE_FALSE(map.height(start,route,2.).has_value());
    map.add({80,80,10},{82,80,10},.25,.05);
    const auto far=map.height(start,route,2.);REQUIRE(far.has_value());REQUIRE(*far<10.);
    map.add({5,1,12},{5,2,12},.25,.1);
    const auto near=map.height(start,route,2.);REQUIRE(near.has_value());REQUIRE(*near>=14.);
    map.add({1000,1000,50},{1001,1000,50},.25,0.);
    REQUIRE(*map.height(start,route,2.)>=38.);
    map.invalidate();REQUIRE_FALSE(map.height(start,route,2.).has_value());
}

TEST_CASE("Route clearance bounds every sampled head position over deposited beads", "[curved-blend][motion]")
{
    TravelClearance map;const std::array<double,4> angles{12.,35.,18.,55.};
    map.configure(40.,angles,14.);
    std::vector<TravelClearance::Point> obstacles;
    for(int i=0;i<20;++i) {TravelClearance::Point p{double((i*17)%61-30),double((i*23)%59-29),double(i%13+1)};
        obstacles.push_back(p);map.add(p,p,.25,.04);}
    const TravelClearance::Point start{-20,-10,2};
    const std::vector<TravelClearance::Point> route{{20,15,2},{-5,20,2}};
    const double bound=*map.height(start,route,2.);
    auto previous=start;
    for(const auto& end:route) {
        for(int step=0;step<=100;++step) {
            const double t=step/100.,x=previous[0]+t*(end[0]-previous[0]),y=previous[1]+t*(end[1]-previous[1]);
            for(const auto& p:obstacles) {
                const double dx=p[0]-x,dy=p[1]-y;
                const double lower=std::max(std::max(dx*std::tan(angles[0]*3.14159265358979323846/180.),
                    -dx*std::tan(angles[1]*3.14159265358979323846/180.)),
                    std::max(dy*std::tan(angles[2]*3.14159265358979323846/180.),
                    -dy*std::tan(angles[3]*3.14159265358979323846/180.)));
                REQUIRE(bound+lower>=p[2]+2.);
            }
        }
        previous=end;
    }
    TravelClearance unknown;unknown.configure(0.,angles,14.);unknown.add(start,start,.25,0.);
    REQUIRE_FALSE(unknown.height(start,route,2.).has_value());
}

TEST_CASE("Joint underside and roof fields share a monotone bounded coordinate map", "[curved-blend][joint]")
{
    const ModelDomain domain{-10.,10.,-10.,10.,30.,0.};
    auto under=std::make_shared<UndersideProgram>(domain,.2,.2,PlaneProgram::Target{7.,.015,0.},.08);
    auto roof=std::make_shared<Regions>(domain,std::vector<Regions::Patch>{{0.,0.,5.,15.,17.,0.,-.02}},.04);
    JointField joint(under,roof,.08);
    REQUIRE(joint.maps_supports());
    REQUIRE(joint.identity_on_box(-10.,10.,-10.,10.,0.,.2));
    REQUIRE(joint.affine_on_box(-2.,2.,-2.,2.,10.,11.));
    REQUIRE_FALSE(joint.affine_on_box(-2.,2.,-2.,2.,15.,18.));
    for(double x=-9.;x<=9.;x+=1.5) for(double y=-9.;y<=9.;y+=1.5) for(double z=.1;z<=30.;z+=.65) {
        const double w=joint.slice_z(x,y,z),h=1e-4;
        REQUIRE(joint.world_z(x,y,w)==Catch::Approx(z).margin(1e-10));
        const auto g=joint.gradient(x,y,w);
        REQUIRE(std::hypot(g.dx,g.dy)<=joint.slope_bound()+1e-9);
        REQUIRE(g.dx==Catch::Approx((joint.world_z(x+h,y,w)-joint.world_z(x-h,y,w))/(2*h)).margin(1e-7));
        REQUIRE(g.dy==Catch::Approx((joint.world_z(x,y+h,w)-joint.world_z(x,y-h,w))/(2*h)).margin(1e-7));
        const double j=(joint.slice_z(x,y,z+h)-joint.slice_z(x,y,z-h))/(2*h);
        REQUIRE(j>=joint.jacobian_min()-1e-7);
        REQUIRE(j<=joint.jacobian_max()+1e-7);
        REQUIRE(std::abs(z-w)<=joint.max_displacement_mm()+1e-9);
        for(const std::array<double,3> d:std::vector<std::array<double,3>>{{1.,0.,0.},{0.,1.,0.},{0.,0.,1.},{.6,0.,.8}}) {
            const double second=(joint.slice_z(x+h*d[0],y+h*d[1],z+h*d[2])-2*w+
                joint.slice_z(x-h*d[0],y-h*d[1],z-h*d[2]))/(h*h);
            REQUIRE(std::abs(second)<=joint.inverse_hessian_bound_box(x-h,x+h,y-h,y+h,z-h,z+h)+1e-5);
        }
    }
    REQUIRE_THROWS(JointField(under,roof,.02));
    joint.require_spacing(.2,.08,.32);
}

TEST_CASE("Joint planning compares full roof and underside benefit with generated costs", "[curved-blend][joint]")
{
    SurfaceBenefit planar{10.,20.,100.},good{5.,10.,100.},bad{12.,22.,100.};
    GeneratedCost cost{100.,20.,500.,60.,20};
    REQUIRE(planning_score(planar,planar,cost,cost)==0.);
    const double gain=planning_score(planar,good,cost,cost);
    REQUIRE(gain>0.);
    auto expensive=cost;expensive.motion_seconds=600.;
    REQUIRE(planning_score(planar,good,cost,expensive)<0.);
    REQUIRE(planning_score(planar,bad,cost,cost)<0.);
    auto saved=cost;saved.support_mm3=10.;
    REQUIRE(planning_score(planar,good,cost,saved)>gain);
    const std::vector<Ramp::Triangle> roof{{{{0.,0.,20.},{8.,0.,20.4},{8.,8.,20.4}}},
        {{{0.,0.,20.},{8.,8.,20.4},{0.,8.,20.}}}};
    const auto before=surface_benefit(roof,nullptr,30.);
    PlaneProgram aligned({0.,8.,0.,8.,21.,0.},.2,.2,{{5.,.05,0.}},.1);
    const auto after=surface_benefit(roof,&aligned,30.);
    REQUIRE(after.roof_error<before.roof_error*.001);
    REQUIRE(after.roof_area_mm2==before.roof_area_mm2);
    REQUIRE(after.underside_deficit_mm2==0.);
}

TEST_CASE("Region decision metadata is bounded and survives an imported preview", "[curved-blend][joint][metadata]")
{
    RegionDecision a{3,0,true,12.,-4.,20.,6.,2.5,.15,64.,20.,"Joint roof: finish gain exceeds added motion cost"},b;
    REQUIRE(RegionDecision::decode(a.encode(),b));
    REQUIRE(b.encode()==a.encode());
    REQUIRE_FALSE(RegionDecision::decode("v9 "+a.encode().substr(3),b));
    REQUIRE_FALSE(RegionDecision::decode(std::string(1025,'a'),b));
    auto invalid=a;invalid.radius=-1.;REQUIRE_FALSE(RegionDecision::decode(invalid.encode(),b));
    invalid=a;invalid.x=INFINITY;REQUIRE_FALSE(RegionDecision::decode(invalid.encode(),b));
    invalid=a;invalid.reason="line\nbreak";REQUIRE_FALSE(RegionDecision::decode(invalid.encode(),b));
    invalid=a;invalid.selected=false;invalid.reason="Planar: generated support contact overlaps this roof";
    REQUIRE(RegionDecision::decode(invalid.encode(),b));REQUIRE_FALSE(b.selected);
}
