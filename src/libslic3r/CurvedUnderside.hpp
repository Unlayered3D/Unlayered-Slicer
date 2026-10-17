#pragma once

#include "CurvedPlaneProgram.hpp"
#include "UndersideAnalysis.hpp"
#include <memory>

namespace Slic3r::CurvedTransition {

// XY remains unchanged and every vertical column is mapped monotonically.
// Native support generation therefore runs on the SAME logical solid as the
// object. Its paths are mapped with the object, not reused from a planar job.
// Bridges must lie wholly outside the direction-changing interval: an affine
// map preserves a straight bridge; a curved air path would not.
class UndersideProgram final : public PlaneProgram {
public:
    UndersideProgram(ModelDomain domain,double first,double requested,Target target,double allowed)
        : PlaneProgram(domain,first,requested,{target},allowed),m_domain(domain),m_target(target) {}
    bool maps_supports() const override { return true; }
    const ModelDomain& domain() const { return m_domain; }
    double angle_deg() const {return std::atan(std::hypot(m_target.gx,m_target.gy))*180./3.14159265358979323846;}
private:
    ModelDomain m_domain;
    Target m_target;
};

inline std::shared_ptr<UndersideProgram> select_underside_program(
    const std::vector<Ramp::Triangle>& mesh,const UndersideAnalysis& analysis,
    double first,double requested,double max_layer,double allowed,double padding=10.,double contact_gap=0.,
    const std::function<void()>& cancel={})
{
    if(!analysis.has_candidate || analysis.best.worsened_mm2>1e-7) return {};
    if(!std::isfinite(allowed) || allowed<=0.) return {};
    if(!std::isfinite(padding) || padding<0. || !std::isfinite(max_layer) || max_layer<=0. ||
       !std::isfinite(contact_gap) || contact_gap<0.)
        throw std::invalid_argument("Invalid underside support-domain bounds");
    ModelDomain domain{INFINITY,-INFINITY,INFINITY,-INFINITY,-INFINITY,0.};
    double underside=INFINITY;
    for(const auto& t:mesh) {
        if(cancel) cancel();
        for(const auto& p:t) {
            domain.xmin=std::min(domain.xmin,p[0]);domain.xmax=std::max(domain.xmax,p[0]);
            domain.ymin=std::min(domain.ymin,p[1]);domain.ymax=std::max(domain.ymax,p[1]);
            domain.top_max=std::max(domain.top_max,p[2]);
        }
        const double nz=(t[1][0]-t[0][0])*(t[2][1]-t[0][1])-(t[1][1]-t[0][1])*(t[2][0]-t[0][0]);
        if(nz< -1e-10 && std::max({t[0][2],t[1][2],t[2][2]})>2e-5)
            underside=std::min({underside,t[0][2],t[1][2],t[2][2]});
    }
    // This extends the field's mathematical domain, not the permitted printer
    // footprint or a toolhead allowance. Generated beads outside it fail checks.
    domain.xmin-=padding;domain.xmax+=padding;domain.ymin-=padding;domain.ymax+=padding;
    for(int scale=31;scale>=1;--scale) {
        if(cancel) cancel();
        const double gx=analysis.best.gx*scale/32.,gy=analysis.best.gy*scale/32.;
        const double slope=std::hypot(gx,gy);
        if(std::atan(slope)*180./3.14159265358979323846<.25) continue; // usefulness heuristic
        const double budget=std::min(.25,.95*(1.-slope/allowed));
        const double projection=std::abs(gx)*((domain.xmax-domain.xmin)*.5+1e-4)+
                                std::abs(gy)*((domain.ymax-domain.ymin)*.5+1e-4);
        const double depth=std::max(3.*requested,1.875*projection/budget);
        // Leave whole bead/contact slabs outside the changing direction. The
        // displacement terms also cover the affine-box logical/physical bound.
        if(first+depth+2.*projection+2.*max_layer+contact_gap+.05>underside) continue;
        // A full-angle ranking does not certify its scaled direction: at a
        // broad angle a normal can pass through its worst alignment. Check
        // every elevated facet again at the angle actually being applied.
        bool regression=false;size_t inspected=0;
        for(const auto& t:mesh) {
            if(cancel && (++inspected%4096)==0) cancel();
            if(std::max({t[0][2],t[1][2],t[2][2]})<=2e-5) continue;
            const double nx=(t[1][1]-t[0][1])*(t[2][2]-t[0][2])-(t[1][2]-t[0][2])*(t[2][1]-t[0][1]);
            const double ny=(t[1][2]-t[0][2])*(t[2][0]-t[0][0])-(t[1][0]-t[0][0])*(t[2][2]-t[0][2]);
            const double nz=(t[1][0]-t[0][0])*(t[2][1]-t[0][1])-(t[1][1]-t[0][1])*(t[2][0]-t[0][0]);
            const double length=std::hypot(std::hypot(nx,ny),nz);
            const double dot=(-nx*gx-ny*gy+nz)/(length*std::hypot(1.,slope));
            const double before=nz<0.?std::acos(std::clamp(-nz/length,0.,1.))*180./3.14159265358979323846:90.;
            const double after=dot<0.?std::acos(std::clamp(-dot,0.,1.))*180./3.14159265358979323846:90.;
            if(std::min(before,after)<analysis.threshold_deg-1e-4 && after<before-1e-7) {regression=true;break;}
        }
        if(regression) continue;
        try {
            return std::make_shared<UndersideProgram>(domain,first,requested,
                PlaneProgram::Target{first+depth*.5,gx,gy},allowed);
        } catch(const std::invalid_argument&) {}
    }
    return {};
}
} // namespace Slic3r::CurvedTransition
