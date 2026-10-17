#pragma once

#include "CurvedTransition.hpp"
#include <array>
#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

namespace Slic3r::CurvedTransition {

// First engine integration: one rectangular, vertical-sided, bed-mounted solid
// with a planar height-field roof. Restricting the domain makes flattening exact:
// the inverse image of its entire boundary is a cuboid, with no facet error.
// No arbitrary mesh is accepted on the strength of a few sampled normals.
struct Ramp {
    using Vertex = std::array<double, 3>;
    using Triangle = std::array<Vertex, 3>;
    double xmin, xmax, ymin, ymax, intercept, gx, gy, top_min, top_max;

    double roof(double x, double y) const { return intercept + gx*x + gy*y; }
    double slope() const { return std::hypot(gx, gy); }
    double angle_deg() const { return std::atan(slope()) * 180. / 3.14159265358979323846; }
    ModelDomain domain() const { return {xmin,xmax,ymin,ymax,top_max,slope()}; }

    static Ramp certify(const std::vector<Triangle>& triangles)
    {
        constexpr double tol = 2e-5; // mm; STL coordinates are float
        if (triangles.empty()) throw std::invalid_argument("Curved blend: empty mesh");
        Ramp r{};
        r.xmin = r.ymin = std::numeric_limits<double>::infinity();
        r.xmax = r.ymax = -std::numeric_limits<double>::infinity();
        bool have_roof = false;
        for (const auto& t : triangles) {
            for (const auto& p : t) {
                for (double c : p) if (!std::isfinite(c)) throw std::invalid_argument("Curved blend: non-finite mesh");
                r.xmin = std::min(r.xmin,p[0]); r.xmax = std::max(r.xmax,p[0]);
                r.ymin = std::min(r.ymin,p[1]); r.ymax = std::max(r.ymax,p[1]);
            }
            const Vertex n = normal(t);
            if (n[2] > tol*length(n)) {
                const double gx = -n[0]/n[2], gy = -n[1]/n[2];
                const double h = t[0][2]-gx*t[0][0]-gy*t[0][1];
                if (!have_roof) { r.gx=gx; r.gy=gy; r.intercept=h; have_roof=true; }
            }
        }
        if (!have_roof || r.xmax-r.xmin < .1 || r.ymax-r.ymin < .1)
            throw std::invalid_argument("Curved blend requires a rectangular ramp with a flat bed face");
        r.top_min = std::numeric_limits<double>::infinity(); r.top_max = 0.;
        for (double x : {r.xmin,r.xmax}) for (double y : {r.ymin,r.ymax}) {
            r.top_min=std::min(r.top_min,r.roof(x,y)); r.top_max=std::max(r.top_max,r.roof(x,y));
        }
        if (r.top_min <= .5 || r.angle_deg() > 10.+1e-5)
            throw std::invalid_argument("Curved blend requires a shallow ramp with a roof above 0.5 mm");
        std::array<double,6> areas{};
        for (const auto& t : triangles) {
            const Vertex n=normal(t); const double a=length(n)*.5;
            if (a <= 1e-12) throw std::invalid_argument("Curved blend: degenerate triangle");
            int face=-1;
            auto on = [&](auto predicate) { return predicate(t[0]) && predicate(t[1]) && predicate(t[2]); };
            if (on([&](const Vertex& p){return std::abs(p[2])<=tol;}) && n[2]<0.) face=0;
            else if (on([&](const Vertex& p){return std::abs(p[2]-r.roof(p[0],p[1]))<=tol;}) && n[2]>0.) face=1;
            else if (on([&](const Vertex& p){return std::abs(p[0]-r.xmin)<=tol;}) && n[0]<0.) face=2;
            else if (on([&](const Vertex& p){return std::abs(p[0]-r.xmax)<=tol;}) && n[0]>0.) face=3;
            else if (on([&](const Vertex& p){return std::abs(p[1]-r.ymin)<=tol;}) && n[1]<0.) face=4;
            else if (on([&](const Vertex& p){return std::abs(p[1]-r.ymax)<=tol;}) && n[1]>0.) face=5;
            if (face<0) throw std::invalid_argument("Curved blend supports only vertical rectangular sides, one planar roof and a flat bottom; remove cut planes and modifiers");
            areas[face]+=a;
        }
        const double dx=r.xmax-r.xmin, dy=r.ymax-r.ymin;
        const std::array<double,6> expected{{dx*dy,dx*dy*std::sqrt(1.+r.slope()*r.slope()),
            dy*r.roof(r.xmin,(r.ymin+r.ymax)*.5),dy*r.roof(r.xmax,(r.ymin+r.ymax)*.5),
            dx*r.roof((r.xmin+r.xmax)*.5,r.ymin),dx*r.roof((r.xmin+r.xmax)*.5,r.ymax)}};
        for (size_t i=0;i<6;++i)
            if (std::abs(areas[i]-expected[i]) > std::max(1e-4,expected[i]*2e-5))
                throw std::invalid_argument("Curved blend: mesh boundary does not cover exactly one rectangular ramp");
        return r; // caller additionally verifies a closed mesh
    }

    std::shared_ptr<const Field> field(double first_height, double reference=0.) const
    {
        const Ramp r=*this;
        Options o;
        o.enabled=true; o.flat_height_mm=first_height;
        o.reference_top_mm=reference>0. ? reference : (top_min+top_max)*.5;
        o.top_min_mm=top_min; o.top_max_mm=top_max; o.max_slope_deg=angle_deg()+1e-7;
        return std::make_shared<Field>(o,[r](double x,double y){
            // Numerical boundary tolerance only, not an extension onto unrelated geometry.
            if (x<r.xmin-1e-4 || x>r.xmax+1e-4 || y<r.ymin-1e-4 || y>r.ymax+1e-4)
                throw std::invalid_argument("Curved blend extrusion left the certified ramp footprint");
            return TopSample{r.roof(std::clamp(x,r.xmin,r.xmax),std::clamp(y,r.ymin,r.ymax)),r.gx,r.gy};
        });
    }

    // An integer count lands the final layer on the EXACT original roof. Choose
    // the largest step no greater than the process request that satisfies BOTH
    // minimum normal distance and maximum vertical spacing from printer config.
    std::vector<double> schedule(const Field& f,double first,double requested,double min_h,double max_h) const
    {
        if (!std::isfinite(requested) || requested<=0. || min_h<=0. || max_h<min_h || first<min_h || first>max_h)
            throw std::invalid_argument("Curved blend needs explicit valid printer minimum/maximum layer heights");
        const double remaining=f.reference_top_mm()-first;
        const double lo=min_h*f.jacobian_max()*std::sqrt(1.+f.slope_bound()*f.slope_bound());
        const double hi=std::min(requested,max_h*f.jacobian_min());
        if (hi<lo || remaining<=0.) throw std::invalid_argument("Curved blend cannot meet the configured layer-height range; use a taller or shorter ramp");
        const int count=int(std::ceil(remaining/hi));
        if (count<=0 || count>10000) throw std::invalid_argument("Curved blend layer count exceeds the experimental limit");
        const double step=remaining/count;
        f.require_spacing(step,min_h,max_h);
        std::vector<double> layers{0.,first}; layers.reserve(2*(count+1));
        for (int i=0;i<count;++i) { layers.push_back(first+i*step); layers.push_back(first+(i+1)*step); }
        return layers;
    }

    static int chord_segments(double xy_length,double roof_slope,double tolerance=.005)
    {
        if (!std::isfinite(xy_length) || xy_length<0. || !std::isfinite(roof_slope) || roof_slope<0. || tolerance<=0.)
            throw std::invalid_argument("Curved blend: invalid path subdivision");
        // Every layer is Lipschitz with constant <= roof_slope. The deviation
        // from its chord is <= slope * XY length / 2, including off-axis paths.
        const double n=std::ceil(xy_length*roof_slope/(2.*tolerance));
        if (n>200000.) throw std::invalid_argument("Curved blend path is too long");
        return std::max(1,int(n));
    }

    static double bead_area(double width,double normal_height)
    {
        // Match Flow::mm3_per_mm, including classic narrow gap fill. Upstream
        // permits width < height; the validity condition is positive area.
        const double area=normal_height*(width-normal_height*(1.-3.14159265358979323846/4.));
        if (!std::isfinite(width) || !std::isfinite(normal_height) ||
            !(width>0. && normal_height>0. && area>0.) || !std::isfinite(area))
            throw std::invalid_argument("Curved blend: bead dimensions produce nonpositive or invalid flow");
        return area;
    }

    static double max_bead_area(double width,double maximum_height)
    {
        if (!std::isfinite(width) || !std::isfinite(maximum_height) || width<=0. || maximum_height<=0.)
            throw std::invalid_argument("Curved blend: invalid flow bound dimensions");
        // The upstream area expression is a concave quadratic in height. A
        // narrow gap-fill bead can reach its maximum BEFORE the height bound.
        const double peak=width/(2.*(1.-3.14159265358979323846/4.));
        return bead_area(width,std::min(maximum_height,peak));
    }

private:
    static Vertex normal(const Triangle& t) {
        const Vertex a{{t[1][0]-t[0][0],t[1][1]-t[0][1],t[1][2]-t[0][2]}};
        const Vertex b{{t[2][0]-t[0][0],t[2][1]-t[0][1],t[2][2]-t[0][2]}};
        return {{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}};
    }
    static double length(const Vertex& n) { return std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); }
};
} // namespace Slic3r::CurvedTransition
