#pragma once
#include "CurvedRegions.hpp"
#include "CurvedUnderside.hpp"
#include "CurvedPlanningReport.hpp"
#include <iomanip>
#include <locale>
#include <memory>
#include <sstream>
#include <string>

namespace Slic3r::CurvedTransition {

// Compose maps, not independent XYZ offsets: z=U(x,y,R(x,y,w)). Native
// contours AND supports are generated from this same monotonically mapped solid.
// Roof proposals live in U's logical space. All-direction slope headroom is
// reserved before making R, so neither coincident slopes nor diagonal taper
// directions can consume the configured clearance twice.
class JointField final : public Field {
public:
    JointField(std::shared_ptr<const UndersideProgram> underside,
               std::shared_ptr<const Regions> roof,double allowed)
        : Field(Options{},{}),m_under(std::move(underside)),m_roof(std::move(roof)) {
        if(!m_under || !m_roof || !std::isfinite(allowed) || allowed<=0.)
            throw std::invalid_argument("Invalid joint field");
        m_slope=m_under->slope_bound()+m_roof->slope_bound()/m_under->jacobian_min();
        if(m_slope>allowed+1e-10) throw std::invalid_argument("Joint field exceeds combined clearance budget");
        m_reference=m_under->reference_top_mm()+m_roof->max_displacement_mm();
    }
    bool enabled() const override {return true;}
    bool maps_supports() const override {return true;}
    double reference_top_mm() const override {return m_reference;}
    void set_reference_top(double z) {if(!std::isfinite(z)) throw std::invalid_argument("Invalid joint top");m_reference=z;}
    const std::shared_ptr<const UndersideProgram>& underside() const {return m_under;}
    const std::shared_ptr<const Regions>& roof_regions() const {return m_roof;}
    double slope_bound() const override {return m_slope;}
    double jacobian_min() const override {return m_under->jacobian_min()*m_roof->jacobian_min();}
    double jacobian_max() const override {return m_under->jacobian_max()*m_roof->jacobian_max();}
    double max_displacement_mm() const override {return m_under->max_displacement_mm()+m_roof->max_displacement_mm();}
    double world_z(double x,double y,double w) const override {return m_under->world_z(x,y,m_roof->world_z(x,y,w));}
    double slice_z(double x,double y,double z) const override {return m_roof->slice_z(x,y,m_under->slice_z(x,y,z));}
    Gradient gradient(double x,double y,double w) const override {
        const double u=m_roof->world_z(x,y,w);
        const auto a=m_under->gradient(x,y,u),b=m_roof->gradient(x,y,w);
        const double j=m_under->forward_vertical_scale(x,y,u);
        return {a.dx+j*b.dx,a.dy+j*b.dy};
    }
    LayerMode layer_mode(double x,double y,double z) const override {
        const auto a=m_under->layer_mode(x,y,z),b=m_roof->layer_mode(x,y,m_under->slice_z(x,y,z));
        if(a==LayerMode::Transition || b==LayerMode::Transition) return LayerMode::Transition;
        return a==LayerMode::Curved || b==LayerMode::Curved?LayerMode::Curved:LayerMode::Planar;
    }
    bool identity_on_box(double x0,double x1,double y0,double y1,double z0,double z1) const override {
        return m_under->identity_on_box(x0,x1,y0,y1,z0,z1) && m_roof->identity_on_box(x0,x1,y0,y1,z0,z1);
    }
    bool affine_on_box(double x0,double x1,double y0,double y1,double w0,double w1) const override {
        // affine_on_box takes LOGICAL bead bounds. An identity roof preserves
        // the underside's affine bridge slab; a compact active roof is curved.
        const double a=m_roof->max_displacement_mm();
        return m_roof->identity_on_box(x0,x1,y0,y1,w0-a,w1+a) &&
            m_under->affine_on_box(x0,x1,y0,y1,w0-a,w1+a);
    }
    double inverse_hessian_bound() const override {
        const double j=m_under->jacobian_max(),s=m_under->slope_bound();
        return m_roof->inverse_hessian_bound()*(2.+j*j*(1.+s*s))+
            m_roof->jacobian_max()*m_under->inverse_hessian_bound();
    }
    double world_hessian_bound_box(double x0,double x1,double y0,double y1,double z0,double z1) const override {
        return implicit_world_hessian_bound_box(x0,x1,y0,y1,z0,z1);
    }
    double inverse_hessian_bound_box(double x0,double x1,double y0,double y1,double z0,double z1) const override {
        // w=R^-1(x,y,U^-1(x,y,z)); ||D U^-1|| <= its Frobenius
        // bound. Chain rule: H_R ||DU||^2 + J_R H_U. Physical boxes
        // supplied by conforming refinement are expanded before querying R.
        const double a=m_under->max_displacement_mm(),j=m_under->jacobian_max(),s=m_under->slope_bound();
        return m_roof->inverse_hessian_bound_box(x0,x1,y0,y1,z0-a,z1+a)*(2.+j*j*(1.+s*s))+
            m_roof->jacobian_max()*m_under->inverse_hessian_bound_box(x0,x1,y0,y1,z0,z1);
    }
private:
    std::shared_ptr<const UndersideProgram> m_under;
    std::shared_ptr<const Regions> m_roof;
    double m_reference=0.,m_slope=0.;
};

struct SurfaceBenefit {
    double roof_error=0.,underside_deficit_mm2=0.,roof_area_mm2=0.;
};

// Original surface areas/normals are invariant between candidates. Evaluate
// the actual field at quadrature points, not its requested centre direction.
// Roof error is an area-weighted angular alignment estimate, NOT measured Ra.
inline SurfaceBenefit surface_benefit(const std::vector<Ramp::Triangle>& mesh,const Field* field,
    double threshold,const std::function<void()>& cancel={}) {
    SurfaceBenefit result;size_t count=0;
    threshold=threshold>0.?threshold:30.;
    constexpr double pi=3.14159265358979323846;
    for(const auto& t:mesh) {
        if(cancel && (++count%256)==0) cancel();
        const auto& a=t[0];const auto& b=t[1];const auto& c=t[2];
        double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
        double ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
        double nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        const double length=std::hypot(std::hypot(nx,ny),nz);
        if(!(std::isfinite(length) && length>1e-12)) throw std::invalid_argument("Invalid benefit facet");
        const double area=length*.5;nx/=length;ny/=length;nz/=length;
        const bool roof=nz>=std::cos(pi/6.);
        if(roof) result.roof_area_mm2+=area;
        constexpr int n=4;
        const auto sample=[&](double u,double v) {
            const double x=a[0]+u*(b[0]-a[0])+v*(c[0]-a[0]);
            const double y=a[1]+u*(b[1]-a[1])+v*(c[1]-a[1]);
            const double z=a[2]+u*(b[2]-a[2])+v*(c[2]-a[2]);
            if(z<=2e-5) return;
            const auto g=field?field->gradient(x,y,field->slice_z(x,y,z)):Gradient{0.,0.};
            const double dot=std::clamp((-nx*g.dx-ny*g.dy+nz)/std::hypot(1.,std::hypot(g.dx,g.dy)),-1.,1.);
            if(roof) result.roof_error+=area*(1.-dot*dot)/(n*n);
            const double angle=dot<0.?std::acos(-dot)*180./pi:90.;
            if(angle<threshold-1e-4) result.underside_deficit_mm2+=area*(threshold-angle)/threshold/(n*n);
        };
        for(int i=0;i<n;++i) for(int j=0;j<n-i;++j) {
            sample((i+1./3.)/n,(j+1./3.)/n);
            if(i+j<n-1) sample((i+2./3.)/n,(j+2./3.)/n);
        }
    }
    return result;
}

struct GeneratedCost {
    double model_mm3=0.,support_mm3=0.,path_mm=0.,motion_seconds=0.;
    size_t paths=0;
    double travel_seconds=0.;
    size_t travel_routes=0,local_routes=0;
};
struct PlanningBreakdown {
    double roof=0.,under=0.,support=0.,time=0.,material=0.;
    double time_seconds=0.,support_mm3=0.,material_mm3=0.;
    double score() const {return roof+under+.5*support-.15*time-.10*material;}
};
inline PlanningBreakdown planning_breakdown(const SurfaceBenefit& baseline,const SurfaceBenefit& surface,
                                           const GeneratedCost& planar,const GeneratedCost& candidate) {
    PlanningBreakdown b;
    b.roof=(baseline.roof_error-surface.roof_error)/std::max(.01,baseline.roof_error);
    b.under=(baseline.underside_deficit_mm2-surface.underside_deficit_mm2)/std::max(1.,baseline.underside_deficit_mm2);
    b.support_mm3=planar.support_mm3-candidate.support_mm3;
    b.time_seconds=candidate.motion_seconds-planar.motion_seconds;
    b.material_mm3=candidate.model_mm3-planar.model_mm3;
    b.support=b.support_mm3/std::max(1.,planar.support_mm3);
    b.time=b.time_seconds/std::max(1.,planar.motion_seconds);
    b.material=b.material_mm3/std::max(1.,planar.model_mm3);
    return b;
}
inline double planning_score(const SurfaceBenefit& baseline,const SurfaceBenefit& surface,
                             const GeneratedCost& planar,const GeneratedCost& candidate) {
    // Explicit balanced policy, normalized against this object's generated
    // planar control. Negative deltas are charged; no absolute area threshold
    // favors a large part. Time is a kinematic estimate, not firmware runtime.
    return planning_breakdown(baseline,surface,planar,candidate).score();
}

struct PlanningTrial {
    std::shared_ptr<Field> field;
    ModelDomain domain{};
    SurfaceBenefit surface;
    GeneratedCost cost;
    std::string name,reason;
    std::vector<RegionDecision> rejected;
    bool assessed=false,valid=true;
    double score=-INFINITY;
};
struct PlanningState {
    std::vector<PlanningTrial> trials;
    std::vector<Ramp::Triangle> mesh;
    size_t current=0,chosen=0;
    bool final=false;
    double allowed=0.,threshold=30.;
    std::vector<RegionDecision> decisions;
};

inline std::shared_ptr<const Regions> roof_part(const std::shared_ptr<const Field>& field) {
    if(auto joint=std::dynamic_pointer_cast<const JointField>(field)) return joint->roof_regions();
    return std::dynamic_pointer_cast<const Regions>(field);
}

inline std::shared_ptr<Field> without_roof_centres(const std::shared_ptr<const Field>& field,
    const ModelDomain& domain,const std::vector<std::array<double,2>>& rejected,double allowed) {
    const auto roof=roof_part(field);if(!roof) return {};
    auto patches=roof->patches();
    patches.erase(std::remove_if(patches.begin(),patches.end(),[&](const Regions::Patch& p){
        return std::any_of(rejected.begin(),rejected.end(),[&](const auto& xy){return xy[0]==p.x && xy[1]==p.y;});
    }),patches.end());
    const auto joint=std::dynamic_pointer_cast<const JointField>(field);
    if(patches.empty()) return joint?std::const_pointer_cast<UndersideProgram>(joint->underside()):nullptr;
    auto next=std::make_shared<Regions>(domain,std::move(patches),roof->slope_bound());
    // Subsets retain the original corner proof; allowed here is the SAME
    // combined scalar cap used when this joint candidate was constructed.
    return joint?std::static_pointer_cast<Field>(std::make_shared<JointField>(joint->underside(),next,allowed)):next;
}
} // namespace Slic3r::CurvedTransition
