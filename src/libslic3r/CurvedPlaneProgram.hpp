#pragma once

#include "CurvedTransition.hpp"
#include <vector>
#include <limits>

namespace Slic3r::CurvedTransition {

// A plane selects the target layer direction at its placed centre height. The
// ORIGINAL solid is sliced through this field; it is never clipped into chunks.
// This is shape independent: closed meshes may have curved outlines and holes.
class PlaneProgram : public Field {
public:
    struct Target { double height, gx, gy; };
    struct Blend { double lo, hi, from_x, from_y, delta_x, delta_y; };

    PlaneProgram(ModelDomain domain, double first, double requested,
                 std::vector<Target> targets, double allowed_slope=std::tan(10.*3.14159265358979323846/180.))
        : Field(Options{},{}), m_domain(domain), m_first(first)
    {
        if (!std::isfinite(domain.xmin) || !std::isfinite(domain.xmax) || !std::isfinite(domain.ymin) ||
            !std::isfinite(domain.ymax) || !std::isfinite(domain.top_max) || !std::isfinite(first) || !std::isfinite(requested) ||
            !(domain.xmax>domain.xmin && domain.ymax>domain.ymin && domain.top_max>first &&
              first>0. && requested>0.) || targets.empty())
            throw std::invalid_argument("Curved blend needs valid model bounds and at least one enabled plane");
        m_cx=(domain.xmin+domain.xmax)*.5; m_cy=(domain.ymin+domain.ymax)*.5;
        std::sort(targets.begin(),targets.end(),[](const Target& a,const Target& b){return a.height<b.height;});
        for(const auto& t:targets) m_target_slope=std::max(m_target_slope,std::hypot(t.gx,t.gy));
        for(const auto& t:targets) m_amplitude=std::max(m_amplitude,
            std::abs(t.gx)*(domain.xmax-domain.xmin)*.5+std::abs(t.gy)*(domain.ymax-domain.ymin)*.5);
        if (!std::isfinite(allowed_slope) || allowed_slope<=m_target_slope)
            throw std::invalid_argument("Curved blend needs clearance margin above its target directions for intermediate curved slopes");
        const double derivative_budget=std::min(.25,.95*(1.-m_target_slope/allowed_slope));
        double previous_end=first, gx=0., gy=0.;
        for (const Target& t:targets) {
            if (!std::isfinite(t.height) || !std::isfinite(t.gx) || !std::isfinite(t.gy) ||
                t.height<=first || t.height>=domain.top_max)
                throw std::invalid_argument("Curved blend plane centre must lie above the foundation and below the model top");
            const double dx=t.gx-gx,dy=t.gy-gy;
            if (std::hypot(dx,dy)<1e-10) continue;
            const double projection=std::abs(dx)*((domain.xmax-domain.xmin)*.5+1e-4)+
                                    std::abs(dy)*((domain.ymax-domain.ymin)*.5+1e-4);
            // Blend as a function of PHYSICAL height z. Isosurfaces of
            // w=z-g(z).(x-cx,y-cy) are genuinely curved in XY. max S'=15/8
            // bounds dw/dz over the entire model rectangle and all heights.
            const double width=std::max(3.*requested,1.875*projection/derivative_budget);
            const double lo=std::max(previous_end,t.height-width*.5),hi=lo+width;
            if (hi>domain.top_max+1e-8 || lo>t.height+1e-8)
                throw std::invalid_argument("Not enough height between plane transitions for a monotone curved blend; move or remove the crowded plane");
            m_blends.push_back({lo,hi,gx,gy,dx,dy});
            const double derivative=1.875*projection/width;
            m_world_jmin=std::min(m_world_jmin,1.-derivative);
            m_world_jmax=std::max(m_world_jmax,1.+derivative);
            m_grad_derivative=std::max(m_grad_derivative,1.875*std::hypot(dx,dy)/width);
            m_second_projection=std::max(m_second_projection,(10.*std::sqrt(3.)/3.)*projection/(width*width));
            gx=t.gx; gy=t.gy; previous_end=hi;
        }
        if (m_blends.empty()) throw std::invalid_argument("Curved blend selected planes do not change the layer direction");
        m_slope=m_target_slope/m_world_jmin;
        m_terminal_x=gx; m_terminal_y=gy;
        m_reference=domain.top_max + std::abs(gx)*(domain.xmax-domain.xmin)*.5+
                                       std::abs(gy)*(domain.ymax-domain.ymin)*.5;
    }

    bool enabled() const override { return true; }
    double reference_top_mm() const override { return m_reference; }
    void set_reference_top(double height) {
        if (!std::isfinite(height) || height<=m_first) throw std::invalid_argument("Invalid mapped model top");
        m_reference=height;
    }
    double slope_bound() const override { return m_slope; }
    double max_displacement_mm() const override {return m_amplitude;}
    bool identity_on_box(double,double,double,double,double,double zmax) const override {
        return zmax<=m_blends.front().lo;
    }
    bool affine_on_box(double,double,double,double,double zmin,double zmax) const override {
        // The box is in logical coordinates. |physical_z-logical_z| is
        // bounded over the entire field, including the bead's bottom face.
        return zmax<=m_blends.front().lo || zmin-m_amplitude>=m_blends.back().hi;
    }
    double jacobian_min() const override { return m_world_jmin; }
    double jacobian_max() const override { return m_world_jmax; }
    double inverse_hessian_bound() const override {
        // Direct Hessian of physical-to-logical w; mixed XY/Z terms plus ZZ.
        return m_second_projection+2.*m_grad_derivative;
    }
    double inverse_hessian_bound_box(double,double,double,double,double zmin,double zmax) const override {
        // map_mesh supplies PHYSICAL bounds. Outside all direction-changing
        // slabs the map is affine, so its exact Hessian is zero.
        for(const auto& b:m_blends) if(zmax>b.lo && zmin<b.hi) return inverse_hessian_bound();
        return 0.;
    }
    const std::vector<Blend>& blends() const { return m_blends; }
    LayerMode layer_mode(double,double,double physical_z) const override {
        for(const auto& b:m_blends)
            if(physical_z>b.lo && physical_z<b.hi) return LayerMode::Transition;
        const auto g=direction(physical_z);
        return std::hypot(g.dx,g.dy)>1e-12?LayerMode::Curved:LayerMode::Planar;
    }

    double world_z(double x,double y,double w) const override {
        check(x,y,w);
        if (w<=m_first) return w;
        const double margin=m_target_slope*std::hypot(m_domain.xmax-m_domain.xmin,m_domain.ymax-m_domain.ymin);
        double lo=m_first,hi=std::max(w+margin,m_domain.top_max+margin);
        for (int i=0;i<60;++i) {
            const double mid=(lo+hi)*.5;
            if (slice_z(x,y,mid)<w) lo=mid; else hi=mid;
        }
        return (lo+hi)*.5;
    }
    double slice_z(double x,double y,double z) const override {
        check(x,y,z);
        if (z<=m_first) return z;
        const auto g=direction(z);
        return z-g.dx*(x-m_cx)-g.dy*(y-m_cy);
    }
    Gradient gradient(double x,double y,double w) const override {
        check(x,y,w);
        if (w<=m_first) return {0.,0.};
        const double z=world_z(x,y,w);
        const auto g=direction(z),d=direction_derivative(z);
        const double jacobian=1.-d.dx*(x-m_cx)-d.dy*(y-m_cy);
        return {g.dx/jacobian,g.dy/jacobian};
    }
    Gradient direction(double z) const {
        if (z<=m_first) return {0.,0.};
        for (const auto& b:m_blends) {
            if (z<b.lo) return {b.from_x,b.from_y};
            if (z<=b.hi) {
                const double t=(z-b.lo)/(b.hi-b.lo);
                const double s=t*t*t*(10.+t*(-15.+6.*t));
                return {b.from_x+s*b.delta_x,b.from_y+s*b.delta_y};
            }
        }
        return {m_terminal_x,m_terminal_y};
    }

private:
    Gradient direction_derivative(double z) const {
        for(const auto& b:m_blends) if(z>b.lo && z<b.hi) {
            const double t=(z-b.lo)/(b.hi-b.lo);
            const double derivative=30.*t*t*(1.-t)*(1.-t)/(b.hi-b.lo);
            return {derivative*b.delta_x,derivative*b.delta_y};
        }
        return {0.,0.};
    }
    void check(double x,double y,double z) const {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            x<m_domain.xmin-1e-4 || x>m_domain.xmax+1e-4 ||
            y<m_domain.ymin-1e-4 || y>m_domain.ymax+1e-4)
            throw std::invalid_argument("Curved blend coordinate left the model bounds");
    }
    ModelDomain m_domain;
    double m_first,m_cx=0.,m_cy=0.,m_reference=0.,m_slope=0.,m_target_slope=0.;
    double m_terminal_x=0.,m_terminal_y=0.,m_amplitude=0.;
    double m_world_jmin=1.,m_world_jmax=1.,m_grad_derivative=0.,m_second_projection=0.;
    std::vector<Blend> m_blends;
};
} // namespace Slic3r::CurvedTransition
