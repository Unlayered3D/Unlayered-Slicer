#pragma once

#include "CurvedRamp.hpp"
#include <functional>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Slic3r::CurvedTransition {

// This compares surface normals with hypothetical, uniform layer directions.
// It does not generate supports, evaluate bridge spans or certify a deposition
// field. In particular, a lower score must never disable support generation.
struct UndersideAnalysis {
    struct Candidate {
        double gx=0.,gy=0.; // layer plane z=gx*x+gy*y, NOT a rotated nozzle
        double angular_deficit_mm2=0.;
        double below_threshold_mm2=0.;
        double minimum_angle_deg=90.;
        double improved_mm2=0.,worsened_mm2=0.,worst_regression_deg=0.;
    };
    size_t facets=0,candidates_checked=0;
    double projected_area_mm2=0.,minimum_angle_deg=90.,maximum_angle_deg=0.,threshold_deg=30.;
    bool explicit_threshold=true;
    Candidate planar,best;
    bool has_candidate=false;
};

inline UndersideAnalysis analyze_undersides(const std::vector<Ramp::Triangle>& mesh,
    double allowed_slope,double threshold_deg,const std::function<void()>& cancel={})
{
    constexpr double pi=3.14159265358979323846,bed_tolerance=2e-5,angle_tolerance=1e-4;
    if(!std::isfinite(allowed_slope) || allowed_slope<0. ||
       !std::isfinite(threshold_deg) || threshold_deg<0. || threshold_deg>90.)
        throw std::invalid_argument("Invalid underside analysis limits");
    UndersideAnalysis report;
    report.explicit_threshold=threshold_deg>0.;
    // Zero selects upstream's layer/bead-overlap support rule, not a fixed
    // angle. Retain a clearly labelled comparison reference in that case.
    report.threshold_deg=report.explicit_threshold?threshold_deg:30.;
    struct Face {double nx,ny,nz,projected_area,surface_area,angle;};
    std::vector<Face> faces;
    size_t inspected=0;
    const auto checkpoint=[&] {if(cancel && (++inspected%4096)==0) cancel();};
    if(cancel) cancel();
    for(const auto& t:mesh) {
        checkpoint();
        for(const auto& p:t) for(double v:p)
            if(!std::isfinite(v)) throw std::invalid_argument("Nonfinite underside mesh vertex");
        const auto& a=t[0];const auto& b=t[1];const auto& c=t[2];
        const double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
        const double ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
        const double nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        const double magnitude=std::hypot(std::hypot(nx,ny),nz);
        if(!std::isfinite(magnitude) || magnitude<=1e-12)
            throw std::invalid_argument("Invalid or degenerate underside mesh facet");
        // Clip faces that straddle the bed instead of treating their entire
        // area as elevated. Horizontal bed contact is excluded completely.
        std::vector<Ramp::Vertex> polygon;
        for(size_t i=0;i<3;++i) {
            const auto& p=t[i];const auto& q=t[(i+1)%3];
            const bool pin=p[2]>bed_tolerance,qin=q[2]>bed_tolerance;
            if(pin) polygon.push_back(p);
            if(pin!=qin) {
                const double u=(bed_tolerance-p[2])/(q[2]-p[2]);
                polygon.push_back({p[0]+u*(q[0]-p[0]),p[1]+u*(q[1]-p[1]),bed_tolerance});
            }
        }
        double surface_area=0.;
        for(size_t i=1;i+1<polygon.size();++i) {
            const auto& p=polygon[0];const auto& q=polygon[i];const auto& r=polygon[i+1];
            const double x=(q[1]-p[1])*(r[2]-p[2])-(q[2]-p[2])*(r[1]-p[1]);
            const double y=(q[2]-p[2])*(r[0]-p[0])-(q[0]-p[0])*(r[2]-p[2]);
            const double z=(q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0]);
            surface_area+=std::hypot(std::hypot(x,y),z)*.5;
        }
        if(surface_area<=1e-12) continue;
        const double angle=nz<0.?std::acos(std::clamp(-nz/magnitude,0.,1.))*180./pi:90.;
        const double projected_area=surface_area*std::abs(nz)/magnitude;
        faces.push_back({nx/magnitude,ny/magnitude,nz/magnitude,projected_area,surface_area,angle});
        if(nz<-1e-8*magnitude) {
            ++report.facets;report.projected_area_mm2+=projected_area;
            report.minimum_angle_deg=std::min(report.minimum_angle_deg,angle);
            report.maximum_angle_deg=std::max(report.maximum_angle_deg,angle);
        }
    }
    // Actual surface area stays fixed between candidates. Include initially
    // vertical/upward faces so a proposed tilt cannot hide a newly created
    // overhang on another side of the model. World-XY area alone would assign
    // a vertical wall zero cost even when its layer-relative overhang worsens.
    const auto evaluate=[&](double gx,double gy) {
        UndersideAnalysis::Candidate c;c.gx=gx;c.gy=gy;
        const double length=std::hypot(1.,std::hypot(gx,gy));
        for(const auto& f:faces) {
            checkpoint();
            const double dot=(-f.nx*gx-f.ny*gy+f.nz)/length;
            const double angle=dot<0.?std::acos(std::clamp(-dot,0.,1.))*180./pi:90.;
            c.minimum_angle_deg=std::min(c.minimum_angle_deg,angle);
            // Float STL normals at a nominal threshold can differ by a few
            // millionths of a degree. Avoid counting whole facets on that noise.
            if(angle<report.threshold_deg-angle_tolerance) {
                c.angular_deficit_mm2+=f.surface_area*(report.threshold_deg-angle)/report.threshold_deg;
                c.below_threshold_mm2+=f.projected_area;
            }
            // Only rank changes to faces near the support threshold. Tilting a
            // previously harmless steep underside is not itself a benefit.
            if(std::min(angle,f.angle)<report.threshold_deg-angle_tolerance) {
                if(angle>f.angle+1e-7) c.improved_mm2+=f.surface_area;
                if(angle<f.angle-1e-7) c.worsened_mm2+=f.surface_area;
                c.worst_regression_deg=std::max(c.worst_regression_deg,f.angle-angle);
            }
        }
        return c;
    };
    report.planar=report.best=evaluate(0.,0.);
    if(report.facets==0 || allowed_slope<=0. || report.planar.angular_deficit_mm2<=1e-12) return report;
    const auto consider=[&](double gx,double gy) {
        if(cancel) cancel();
        const auto c=evaluate(gx,gy);++report.candidates_checked;
        const double eps=1e-10*std::max(1.,report.planar.angular_deficit_mm2);
        if(c.angular_deficit_mm2<report.best.angular_deficit_mm2-eps ||
           (std::abs(c.angular_deficit_mm2-report.best.angular_deficit_mm2)<=eps &&
            std::hypot(gx,gy)<std::hypot(report.best.gx,report.best.gy)-1e-12)) report.best=c;
    };
    // Bounded azimuth/radius search includes diagonal directions. Also test
    // exact dominant facet directions so a feature is not lost between bins.
    for(int radius=1;radius<=3;++radius) for(int azimuth=0;azimuth<32;++azimuth) {
        const double a=2.*pi*azimuth/32.,r=allowed_slope*radius/3.;
        consider(r*std::cos(a),r*std::sin(a));
    }
    std::stable_sort(faces.begin(),faces.end(),[](const Face& a,const Face& b){return a.surface_area>b.surface_area;});
    std::vector<std::array<double,2>> directions;
    for(const auto& f:faces) {
        checkpoint();
        if(f.angle>=report.threshold_deg-angle_tolerance) continue;
        const double xy=std::hypot(f.nx,f.ny);
        if(xy<=1e-10) continue; // a flat underside has no preferred azimuth
        const std::array<double,2> d{{-f.nx/xy,-f.ny/xy}};
        if(std::any_of(directions.begin(),directions.end(),[&](const auto& p){return std::hypot(d[0]-p[0],d[1]-p[1])<1e-6;})) continue;
        directions.push_back(d);consider(allowed_slope*d[0],allowed_slope*d[1]);
        if(directions.size()==32) break;
    }
    report.has_candidate=report.best.angular_deficit_mm2<report.planar.angular_deficit_mm2-1e-10 &&
        std::hypot(report.best.gx,report.best.gy)>1e-10;
    return report;
}

inline std::string underside_analysis_summary(const UndersideAnalysis& r,bool field_applied=false)
{
    std::ostringstream s;s.imbue(std::locale::classic());s<<std::fixed<<std::setprecision(1);
    s<<"Underside check: "<<r.facets<<" elevated downward facets, "<<r.projected_area_mm2<<" mm2 projected area";
    if(!r.facets) return s.str()+"; no elevated underside found.";
    s<<"; angles "<<r.minimum_angle_deg<<"-"<<r.maximum_angle_deg<<" deg above horizontal; "
     <<r.planar.below_threshold_mm2<<" mm2 below "<<r.threshold_deg<<" deg "
        <<(r.explicit_threshold?"configured angle":"analysis reference (automatic support uses bead overlap)")<<".";
    if(r.has_candidate) {
        const double slope=std::hypot(r.best.gx,r.best.gy);
        const double ux=std::abs(r.best.gx/slope)<1e-4?0.:r.best.gx/slope;
        const double uy=std::abs(r.best.gy/slope)<1e-4?0.:r.best.gy/slope;
        s<<" Best uniform-layer candidate: "<<std::atan(slope)*180./3.14159265358979323846
         <<" deg, uphill XY ("<<ux<<", "<<uy<<"); shallowest underside angle to its layer plane "
         <<r.planar.minimum_angle_deg<<" -> "<<r.best.minimum_angle_deg<<" deg; "
         <<r.best.worsened_mm2<<" mm2 of relevant surface area worsens.";
        if(field_applied) s<<" The applied field can use a smaller angle to finish its foundation transition; see the selection report.";
        else s<<" Candidate not applied: underside-changing layers require support/bridge generation and anchoring checks.";
    } else s<<" No lower angular-deficit candidate within the configured angle bound.";
    return s.str()+" Geometry estimate only; not support savings or a clearance/printability certificate. "+
        (field_applied?"Support settings retained; paths regenerated with the object.":"Supports unchanged.");
}
} // namespace Slic3r::CurvedTransition
