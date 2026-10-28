#pragma once
#include "CurvedTravelClearance.hpp"
#include <stdexcept>

namespace Slic3r::CurvedTransition {

// The writer and candidate cost pass use this SAME route calculation. Points
// are physical nozzle coordinates; intermediate Z values do not command a
// descent. Only the final point is approached after the raised XY route.
struct TravelState {
    TravelClearance::Point position{};
    bool position_known=false;
    double deposited_top=0.,bead_rise=0.,unknown_safe_z=0.;
};
struct TravelSettings {
    double clearance=2.,max_z=0.,xy_speed=0.,z_speed=0.;
};
struct TravelPlan {
    double safe_z=0.,xy_mm=0.,vertical_mm=0.,seconds=0.;
    bool local_route=false,no_op=false;
};
inline TravelPlan plan_travel(const TravelState& state,const std::vector<TravelClearance::Point>& route,
    const TravelSettings& settings,const TravelClearance& obstacles)
{
    const auto finite=[](const auto& p) {return std::all_of(p.begin(),p.end(),[](double v){return std::isfinite(v);});};
    if(!finite(state.position) || !std::all_of(route.begin(),route.end(),finite) ||
       !std::isfinite(settings.clearance) || settings.clearance<=0. ||
       !std::isfinite(state.deposited_top) || state.deposited_top<0. ||
       !std::isfinite(state.bead_rise) || state.bead_rise<0. ||
       !std::isfinite(state.unknown_safe_z) || state.unknown_safe_z<0. || !(settings.max_z>0.) ||
       !(settings.xy_speed>0.) || !std::isfinite(settings.xy_speed) ||
       !(settings.z_speed>0.) || !std::isfinite(settings.z_speed))
        throw std::invalid_argument("Invalid curved travel state or machine limits");
    TravelPlan result;
    if(route.empty() || (state.position_known && route.size()==1 &&
        std::hypot(std::hypot(route.back()[0]-state.position[0],route.back()[1]-state.position[1]),
                   route.back()[2]-state.position[2])<1e-9)) {
        result.safe_z=state.position[2];result.no_op=true;return result;
    }
    const auto& target=route.back();
    const double bound=std::max(state.deposited_top,target[2])+state.bead_rise+settings.clearance+.001;
    result.safe_z=std::max({std::ceil(bound*1000.)/1000.,state.position[2],
        state.position_known?0.:std::ceil(state.unknown_safe_z*1000.)/1000.});
    // Retain the writer's global fallback/Z-limit contract even if a local
    // route might be lower. An incomplete map never authorizes a shortcut.
    if(!std::isfinite(result.safe_z) || result.safe_z>settings.max_z+1e-9)
        throw std::invalid_argument("Curved travel exceeds the configured machine/Virtual or active-nozzle Z limit");
    if(state.position_known) if(const auto local=obstacles.height(state.position,route,settings.clearance+.001)) {
        const double lower=std::ceil(std::max({*local,target[2]+settings.clearance+.001,state.position[2]})*1000.)/1000.;
        if(lower<result.safe_z) {result.safe_z=lower;result.local_route=true;}
    }
    auto previous=state.position;
    for(const auto& p:route) {result.xy_mm+=std::hypot(p[0]-previous[0],p[1]-previous[1]);previous=p;}
    result.vertical_mm=2.*result.safe_z-state.position[2]-target[2];
    result.seconds=result.vertical_mm/settings.z_speed+result.xy_mm/settings.xy_speed;
    return result;
}
} // namespace Slic3r::CurvedTransition
