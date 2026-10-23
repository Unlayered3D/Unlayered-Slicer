#pragma once
#include "CurvedTransition.hpp"
#include <array>
#include <numeric>
#include <vector>

namespace Slic3r::CurvedTransition {

// Bounded relocation of already chained, direction-preserving paths. Callers
// must restrict this to sortable paths of one interchangeable extrusion role.
struct PathEnds {std::array<double,3> first,last;};
inline double route_cost(const std::array<double,3>& a,const std::array<double,3>& b,
                         double xy_speed,double z_speed,double safe_z) {
    if(!std::isfinite(xy_speed) || xy_speed<=0. || !std::isfinite(z_speed) || z_speed<=0.)
        throw std::invalid_argument("Invalid curved ordering speeds");
    if(a==b) return 0.;
    const double safe=std::max({safe_z,a[2],b[2]});
    return (2.*safe-a[2]-b[2])/z_speed+std::hypot(a[0]-b[0],a[1]-b[1])/xy_speed;
}
inline std::vector<size_t> refine_path_order(const std::vector<PathEnds>& paths,
    const std::array<double,3>& start,double xy_speed,double z_speed,double safe_z) {
    std::vector<size_t> order(paths.size());std::iota(order.begin(),order.end(),0);
    if(paths.size()<3 || paths.size()>2048) return order;
    const auto cost=[&](const auto& a,const auto& b){return route_cost(a,b,xy_speed,z_speed,safe_z);};
    // Move a later path before i; only three old/new inter-path edges differ.
    // Direction and each path's extrusion, flow and endpoints remain unchanged.
    for(int pass=0;pass<4;++pass) {
        bool changed=false;
        for(size_t i=0;i+1<order.size();++i) {
            const auto& before=i?paths[order[i-1]].last:start;
            double best=0.;size_t take=i;
            for(size_t j=i+1;j<std::min(order.size(),i+9);++j) {
                const auto& first=paths[order[i]];const auto& p=paths[order[j-1]];const auto& q=paths[order[j]];
                double old=cost(before,first.first)+cost(p.last,q.first);
                double next=cost(before,q.first)+cost(q.last,first.first);
                if(j+1<order.size()) {
                    old+=cost(q.last,paths[order[j+1]].first);
                    next+=cost(p.last,paths[order[j+1]].first);
                }
                if(next-old<best-1e-9) {best=next-old;take=j;}
            }
            if(take!=i) {const auto id=order[take];order.erase(order.begin()+take);order.insert(order.begin()+i,id);changed=true;}
        }
        if(!changed) break;
    }
    return order;
}

// Retain the old fine integration grid, but emit fewer chords where a WHOLE
// interval's curvature proves the same 0.005 mm positional tolerance. Endpoint
// and midpoint samples alone never authorize merging across a narrow feature.
template<class Point>
std::vector<size_t> bounded_chord_ends(const Field& field,const std::vector<Point>& points,
                                     double tolerance=.005) {
    if(points.size()<2 || !std::isfinite(tolerance) || tolerance<=0.)
        throw std::invalid_argument("Invalid adaptive curved path");
    std::vector<size_t> result;
    std::function<void(size_t,size_t)> split=[&](size_t a,size_t b) {
        if(b==a+1) {result.push_back(b);return;}
        const auto& p=points[a];const auto& q=points[b];
        const double length=std::hypot(q[0]-p[0],q[1]-p[1]);
        const double rise=field.slope_bound()*length*.5;
        const double h=field.world_hessian_bound_box(std::min(p[0],q[0]),std::max(p[0],q[0]),
            std::min(p[1],q[1]),std::max(p[1],q[1]),std::min(p[2],q[2])-rise,std::max(p[2],q[2])+rise);
        // h L^2 / 8 bounds vertical chord error along a line; XY is unchanged.
        if(std::isfinite(h) && h>=0. && h*length*length/8.<=tolerance*(1.-1e-9)) result.push_back(b);
        else {const size_t mid=a+(b-a)/2;split(a,mid);split(mid,b);}
    };
    split(0,points.size()-1);return result;
}
} // namespace Slic3r::CurvedTransition
