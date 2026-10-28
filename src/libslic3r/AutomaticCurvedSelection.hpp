#pragma once

#include "CurvedRamp.hpp"
#include <string>

namespace Slic3r::CurvedTransition {

// A bounded finish heuristic, not a prediction of print quality or support
// savings. Planning still runs through PlaneProgram's geometric certificates.
struct AutomaticTarget {
    bool useful = false;
    double gx = 0., gy = 0., estimated_reduction = 0.;
    std::string reason;
    bool needs_support_planning = false;
};

inline AutomaticTarget automatic_target(const std::vector<Ramp::Triangle>& triangles,
                                        double allowed_slope)
{
    if (!std::isfinite(allowed_slope) || allowed_slope <= 0.)
        return {false,0.,0.,0.,"the printer has no usable configured clearance envelope"};
    struct Roof { double area,gx,gy; };
    std::vector<Roof> roofs;
    double area=0.,sx=0.,sy=0.,baseline=0.;
    for (const auto& t:triangles) {
        const auto& a=t[0];const auto& b=t[1];const auto& c=t[2];
        const double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
        const double ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
        const double nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        const double magnitude=std::hypot(std::hypot(nx,ny),nz);
        if (!(std::isfinite(magnitude) && magnitude>0.))
            return {false,0.,0.,0.,"the mesh contains invalid or degenerate facets"};
        // Automatic mode excludes all downward faces above the bed. This is
        // conservative: such shapes need bridge/support planning, not merely
        // a promising top-surface score.
        if (nz < -1e-8*magnitude && std::max({a[2],b[2],c[2]}) > 2e-5)
            return {false,0.,0.,0.,"downward surfaces need bridge or support planning",true};
        if (nz <= 0. || nz/magnitude < std::cos(20.*3.14159265358979323846/180.)) continue;
        const double weight=nz*.5,gx=-nx/nz,gy=-ny/nz;
        roofs.push_back({weight,gx,gy});area+=weight;sx+=weight*gx;sy+=weight*gy;
        baseline+=weight*std::hypot(gx,gy);
    }
    if (area<=0. || baseline<area*std::tan(.5*3.14159265358979323846/180.))
        return {false,0.,0.,0.,"the roof is flat or has too little shallow slope to benefit"};
    double gx=sx/area,gy=sy/area;
    const double slope=std::hypot(gx,gy);
    if (slope>allowed_slope*.75) {gx*=allowed_slope*.75/slope;gy*=allowed_slope*.75/slope;}
    double proposed=0.;
    for (const auto& r:roofs) proposed+=r.area*std::hypot(r.gx-gx,r.gy-gy);
    const double reduction=1.-proposed/baseline;
    if (std::hypot(gx,gy)<std::tan(.5*3.14159265358979323846/180.) || reduction<.10)
        return {false,0.,0.,reduction,"no single continuous direction offers enough estimated finish benefit"};
    return {true,gx,gy,reduction,"a shallow roof direction offers estimated finish benefit"};
}
} // namespace Slic3r::CurvedTransition
