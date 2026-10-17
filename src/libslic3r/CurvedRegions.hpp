#pragma once
#include "CurvedRamp.hpp"
#include <vector>
#include <limits>

namespace Slic3r::CurvedTransition {

// Disjoint compact C2 fields. Each roof patch has its own direction; the rest
// of the model (including protected underside/contact regions) stays planar.
// z=w+S(w)*g.(x-c)*phi(|x-c|/r), phi=(1-r)^4(1+4r).
// Blending in logical height keeps |grad z| <= |g|, including transitions.
// The physical-height formulation divided that slope by a varying Jacobian
// and needed an additional angular reserve to prevent intermediate overshoot.
struct RegionalClearance {
    // Signed corners of the configured four-face hotend pyramid, per mm of Z.
    // Empty means the caller has already supplied an all-direction slope cap.
    std::vector<Gradient> corners;
    double target_limit(double gx,double gy,double allowed) const {
        const double g=std::hypot(gx,gy);
        if(!(std::isfinite(g) && std::isfinite(allowed) && allowed>0.))
            throw std::invalid_argument("Invalid regional clearance direction");
        if(g==0.) return 0.;
        double result=allowed;
        for(const auto& a:corners) {
            if(!std::isfinite(a.dx) || !std::isfinite(a.dy))
                throw std::invalid_argument("Invalid regional clearance corner");
            const double dot=(a.dx*gx+a.dy*gy)/g;
            // grad(F) = phi*g + r*phi'*(g.e)*e. For every unit e,
            // -(g.e)*(a.e) <= (|a||g|-a.g)/2. Also phi<=1 and
            // -r*phi'=20*r^2*(1-r)^3 <= 432/625, for ALL 0<=r<=1.
            // This bounds the complete disk, including opposite/diagonal
            // taper directions, rather than checking only its centre slope.
            const double projection=std::max(0.,dot)+(432./625.)*
                std::max(0.,(std::hypot(a.dx,a.dy)-dot)*.5);
            if(projection>0.) result=std::min(result,1./projection);
        }
        return result;
    }
};

class Regions final : public Field {
public:
    struct Patch { double x,y,radius,lo,hi,gx,gy; };
    Regions(ModelDomain domain,std::vector<Patch> patches,double allowed,const RegionalClearance& clearance={})
        : Field(Options{},{}),m_domain(domain),m_patches(std::move(patches))
    {
        if(m_patches.empty() || !std::isfinite(allowed) || allowed<=0.)
            throw std::invalid_argument("No feasible regional curved surface");
        for(size_t i=0;i<m_patches.size();++i) {
            const auto& p=m_patches[i];
            for(double v:{p.x,p.y,p.radius,p.lo,p.hi,p.gx,p.gy})
                if(!std::isfinite(v)) throw std::invalid_argument("Nonfinite curved region");
            if(p.radius<=0. || p.lo<0. || p.hi<=p.lo)
                throw std::invalid_argument("Invalid curved region bounds");
            for(size_t j=0;j<i;++j) if(std::hypot(p.x-m_patches[j].x,p.y-m_patches[j].y)<p.radius+m_patches[j].radius)
                throw std::invalid_argument("Curved region footprints must not overlap");
            const double g=std::hypot(p.gx,p.gy),depth=p.hi-p.lo;
            if(g>clearance.target_limit(p.gx,p.gy,allowed)+1e-10)
                throw std::invalid_argument("Regional slope exceeds a configured toolhead face or angle limit");
            // max r*phi(r)<0.17. The eigenvalues of grad(u*phi) are
            // phi in [0,1] and phi+r*phi' in [-7/16,1]. Thus |grad L|<=|g|.
            const double a=.17*g*p.radius,d=1.875*a/depth;
            m_amplitude=std::max(m_amplitude,a);
            if(d>.25) throw std::invalid_argument("Regional blend exceeds monotonicity allowance");
            m_jmin=std::min(m_jmin,1./(1.+d));m_jmax=std::max(m_jmax,1./(1.-d));
            m_g=std::max(m_g,g);
            // Implicit inverse w+S(w)F(x,y)-z=0. The full inverse gradient
            // has norm <= sqrt(1+g^2)/(1-d). Bound all mixed and ZZ terms.
            const double inverse_gradient=std::hypot(1.,g)/(1.-d);
            m_hessian=std::max(m_hessian,(22.*g/p.radius+3.75*g/depth*inverse_gradient+
                (10.*std::sqrt(3.)/3.)*a/(depth*depth)*inverse_gradient*inverse_gradient)/(1.-d));
        }
        if(m_jmin<.75 || m_g>allowed+1e-10)
            throw std::invalid_argument("Regional blend exceeds monotonicity or slope clearance");
        m_reference=domain.top_max+m_amplitude;
    }
    bool enabled() const override {return true;}
    double reference_top_mm() const override {return m_reference;}
    void set_reference_top(double z) {m_reference=z;}
    double slope_bound() const override {return m_g;}
    double jacobian_min() const override {return m_jmin;}
    double jacobian_max() const override {return m_jmax;}
    double max_displacement_mm() const override {return m_amplitude;}
    double inverse_hessian_bound() const override {return m_hessian;}
    double inverse_hessian_bound_box(double xmin,double xmax,double ymin,double ymax,double zmin,double zmax) const override {
        return identity_on_box(xmin,xmax,ymin,ymax,zmin,zmax)?0.:m_hessian;
    }
    bool identity_on_box(double xmin,double xmax,double ymin,double ymax,double zmin,double zmax) const override {
        (void)zmin;
        for(const auto& p:m_patches) {
            const double dx=std::max({xmin-p.x,0.,p.x-xmax});
            const double dy=std::max({ymin-p.y,0.,p.y-ymax});
            if(zmax>p.lo && dx*dx+dy*dy<p.radius*p.radius) return false;
        }
        return true;
    }
    const std::vector<Patch>& patches() const {return m_patches;}
    LayerMode layer_mode(double x,double y,double physical_z) const override {
        for(const auto& p:m_patches) if(physical_z>p.lo && std::hypot(x-p.x,y-p.y)<p.radius)
            return slice_z(x,y,physical_z)<p.hi?LayerMode::Transition:LayerMode::Curved;
        return LayerMode::Planar;
    }
    double slice_z(double x,double y,double z) const override {
        check(x,y,z);
        if(identity_on_box(x,x,y,y,z,z)) return z;
        double lo=z-m_amplitude,hi=z+m_amplitude;
        // A safeguarded inverse: most roofs are in the terminal slab and
        // changing-height samples converge in a few Newton steps. The bracket
        // is retained, so a rejected step cannot jump across layers.
        double w=z;
        for(int i=0;i<12;++i) {
            const auto q=displacement(x,y,w);
            const double residual=w+q.value-z;
            if(std::abs(residual)<1e-12) return w;
            if(residual<0.) lo=w;else hi=w;
            const double candidate=w-residual/(1.+q.dz);
            w=candidate>lo && candidate<hi?candidate:(lo+hi)*.5;
        }
        for(int i=0;i<48;++i) {const double mid=(lo+hi)*.5;if(world_z(x,y,mid)<z) lo=mid;else hi=mid;}
        return (lo+hi)*.5;
    }
    double world_z(double x,double y,double w) const override {
        check(x,y,w);
        return w+displacement(x,y,w).value;
    }
    Gradient gradient(double x,double y,double w) const override {
        const auto q=displacement(x,y,w);
        return {q.dx,q.dy};
    }
    // Gradient of a physical roof after mapping into the logical slice frame.
    // Used only to rank candidate usefulness; field bounds are independent.
    Gradient logical_roof_gradient(double x,double y,double z,double gx,double gy) const {
        const auto q=displacement(x,y,slice_z(x,y,z));
        return {(gx-q.dx)/(1.+q.dz),(gy-q.dy)/(1.+q.dz)};
    }
private:
    struct Sample {double value=0.,dx=0.,dy=0.,dz=0.;};
    static void check(double x,double y,double z) {
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))
            throw std::invalid_argument("Nonfinite regional field coordinate");
    }
    Sample displacement(double x,double y,double z) const {
        check(x,y,z);
        for(const auto& p:m_patches) {
            const double ux=x-p.x,uy=y-p.y,d=std::hypot(ux,uy),r=d/p.radius;
            if(r>=1. || z<=p.lo) continue;
            const double a=1.-r,phi=a*a*a*a*(1.+4.*r),L=p.gx*ux+p.gy*uy;
            double s=1.,ds=0.;
            if(z<p.hi) {const double t=(z-p.lo)/(p.hi-p.lo);s=t*t*t*(10.+t*(-15.+6.*t));ds=30.*t*t*(1.-t)*(1.-t)/(p.hi-p.lo);}
            const double radial=d>1e-12?-20.*r*a*a*a/(p.radius*d):0.;
            return {s*L*phi,s*(p.gx*phi+L*radial*ux),s*(p.gy*phi+L*radial*uy),ds*L*phi};
        }
        return {};
    }
    ModelDomain m_domain;
    std::vector<Patch> m_patches;
    double m_reference=0.,m_amplitude=0.,m_jmin=1.,m_jmax=1.,m_g=0.,m_hessian=0.;
};

// Clip a contact facet to a conservative XY patch box. A remote high vertex
// must not reserve its height over the whole triangle's bounding rectangle.
inline double regional_contact_height(const Ramp::Triangle& triangle,
    double xmin,double xmax,double ymin,double ymax)
{
    std::vector<Ramp::Vertex> polygon(triangle.begin(),triangle.end());
    for(const auto& edge:std::array<std::array<double,3>,4>{{
            {{0.,xmin,1.}},{{0.,xmax,-1.}},{{1.,ymin,1.}},{{1.,ymax,-1.}}}}) {
        if(polygon.empty()) return -std::numeric_limits<double>::infinity();
        const size_t axis=size_t(edge[0]);const double bound=edge[1],sign=edge[2];
        std::vector<Ramp::Vertex> clipped;
        auto a=polygon.back();double da=sign*(a[axis]-bound);
        for(const auto& b:polygon) {
            const double db=sign*(b[axis]-bound);
            if((da>=0.)!=(db>=0.)) {
                const double t=da/(da-db);Ramp::Vertex point{};
                for(size_t k=0;k<3;++k) point[k]=a[k]+t*(b[k]-a[k]);
                clipped.push_back(point);
            }
            if(db>=0.) clipped.push_back(b);
            a=b;da=db;
        }
        polygon=std::move(clipped);
    }
    double height=-std::numeric_limits<double>::infinity();
    for(const auto& p:polygon) height=std::max(height,p[2]);
    return height;
}

// Roof selection is a heuristic. Accepted fields have analytic bounds above;
// bridge/support toolpaths are checked against their full bead boxes at export.
inline std::vector<Regions::Patch> select_regions(const std::vector<Ramp::Triangle>& mesh,
    double first,double step,double allowed,double solid_depth,bool sparse,const std::function<void()>& cancel={},
    const RegionalClearance& clearance={})
{
    struct Face {double x,y,z,gx,gy,area,xmin,xmax,ymin,ymax,zmax;const Ramp::Triangle* triangle;};
    struct Box {double xmin,xmax,ymin,ymax,zmax;const Ramp::Triangle* triangle;};
    std::vector<Face> roofs,surfaces;std::vector<Box> contacts;
    size_t inspected=0;
    for(const auto& t:mesh) {
        if(cancel && (++inspected%4096)==0) cancel();
        const auto& a=t[0];const auto& b=t[1];const auto& c=t[2];
        const double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
        const double ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
        const double nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        if(!std::isfinite(nx+ny+nz) || std::hypot(std::hypot(nx,ny),nz)<=1e-12)
            throw std::invalid_argument("Invalid regional mesh facet");
        const double zmax=std::max({a[2],b[2],c[2]});
        if(nz<0. && zmax>2e-5) contacts.push_back({std::min({a[0],b[0],c[0]})-1.,std::max({a[0],b[0],c[0]})+1.,
            std::min({a[1],b[1],c[1]})-1.,std::max({a[1],b[1],c[1]})+1.,zmax,&t});
        if(nz<=0.) continue;
        const double gx=-nx/nz,gy=-ny/nz,g=std::hypot(gx,gy);
        if(g>std::tan(30.*3.14159265358979323846/180.)) continue;
        const Face face{(a[0]+b[0]+c[0])/3.,(a[1]+b[1]+c[1])/3.,(a[2]+b[2]+c[2])/3.,gx,gy,nz*.5,
            std::min({a[0],b[0],c[0]}),std::max({a[0],b[0],c[0]}),std::min({a[1],b[1],c[1]}),std::max({a[1],b[1],c[1]}),zmax,&t};
        surfaces.push_back(face);
        if(g>=std::tan(.5*3.14159265358979323846/180.)) roofs.push_back(face);
    }
    std::stable_sort(roofs.begin(),roofs.end(),[](const Face& a,const Face& b){return a.area>b.area;});
    std::vector<Regions::Patch> patches;
    // A compact field can make a plane worse by moving its stair-steps to the
    // taper. Include adjacent flat roofs in the cost and require a positive
    // sampled reduction. This is a ranking heuristic, not a finish certificate.
    const auto benefit=[&](const Regions::Patch& p) {
        const Regions trial({p.x-p.radius,p.x+p.radius,p.y-p.radius,p.y+p.radius,p.hi,0.},{p},allowed,clearance);
        double before=0.,after=0.;
        constexpr int divisions=16,samples=divisions*divisions;
        for(const auto& f:surfaces) {
            if(cancel && (++inspected%4096)==0) cancel();
            if(f.zmax<=p.lo || f.xmax<=p.x-p.radius || f.xmin>=p.x+p.radius ||
               f.ymax<=p.y-p.radius || f.ymin>=p.y+p.radius) continue;
            const auto& t=*f.triangle;
            const auto sample=[&](double u,double v) {
                const double x=t[0][0]+u*(t[1][0]-t[0][0])+v*(t[2][0]-t[0][0]);
                const double y=t[0][1]+u*(t[1][1]-t[0][1])+v*(t[2][1]-t[0][1]);
                const double z=t[0][2]+u*(t[1][2]-t[0][2])+v*(t[2][2]-t[0][2]);
                if(z<=p.lo || std::hypot(x-p.x,y-p.y)>=p.radius) return;
                const auto q=trial.logical_roof_gradient(x,y,z,f.gx,f.gy);
                before+=f.area*(f.gx*f.gx+f.gy*f.gy)/samples;
                after+=f.area*(q.dx*q.dx+q.dy*q.dy)/samples;
            };
            // Both triangles of each barycentric grid cell have equal area.
            for(int i=0;i<divisions;++i) for(int j=0;j<divisions-i;++j) {
                sample((i+1./3.)/divisions,(j+1./3.)/divisions);
                if(i+j<divisions-1) sample((i+2./3.)/divisions,(j+2./3.)/divisions);
            }
        }
        return before>1e-12 && after<before*.98?before-after:0.;
    };
    size_t proposals=0;
    for(const auto& source:roofs) {
      if(proposals>=256 || patches.size()>=64) break;
      // A long triangle's centroid may sit over its thinnest/unsupported area.
      // Check bounded interior seeds nearer its vertices and edges as well.
      for(const auto& weights:std::array<std::array<double,3>,7>{{
              {{1./3.,1./3.,1./3.}},{{.8,.1,.1}},{{.1,.8,.1}},{{.1,.1,.8}},
              {{.45,.45,.1}},{{.45,.1,.45}},{{.1,.45,.45}}}}) {
        Face f=source;f.x=f.y=f.z=0.;
        for(size_t k=0;k<3;++k) {f.x+=weights[k]*(*f.triangle)[k][0];
            f.y+=weights[k]*(*f.triangle)[k][1];f.z+=weights[k]*(*f.triangle)[k][2];}
        if(cancel) cancel();
        const double g=std::hypot(f.gx,f.gy),scale=std::min(1.,clearance.target_limit(f.gx,f.gy,allowed)/g);
        const double gx=f.gx*scale,gy=f.gy*scale;
        if(std::hypot(gx,gy)<std::tan(.5*3.14159265358979323846/180.) || scale<.10) continue;
        bool covered=false;
        for(const auto& p:patches) if(std::hypot(f.x-p.x,f.y-p.y)<p.radius+1.) {covered=true;break;}
        if(covered) continue;
        if(++proposals>256) break;
        const double base=std::clamp(std::sqrt(f.area)*.7,1.,12.);
        Regions::Patch best{};double best_gain=0.;
        for(double factor:{.75,1.,1.5,2.,4.,8.,16.}) {
            const double radius=std::clamp(base*factor,1.,24.);
            bool overlap=false;
            for(const auto& p:patches) if(std::hypot(f.x-p.x,f.y-p.y)<radius+p.radius) {overlap=true;break;}
            if(overlap) continue;
            const double amplitude=.17*std::hypot(gx,gy)*radius;
            const double depth=std::max(sparse?step:3.*step,1.875*amplitude/.20);
            double lo=first;
            if(sparse) for(const auto& roof:surfaces) {
                if(cancel && (++inspected%4096)==0) cancel();
                if(f.x+radius<roof.xmin || f.x-radius>roof.xmax ||
                   f.y+radius<roof.ymin || f.y-radius>roof.ymax) continue;
                const double local=regional_contact_height(*roof.triangle,
                    f.x-radius,f.x+radius,f.y-radius,f.y+radius);
                lo=std::max(lo,local-solid_depth+step);
            }
            for(const auto& b:contacts) if(f.x+radius>b.xmin && f.x-radius<b.xmax && f.y+radius>b.ymin && f.y-radius<b.ymax) {
                const double local=regional_contact_height(*b.triangle,
                    f.x-radius-1.,f.x+radius+1.,f.y-radius-1.,f.y+radius+1.);
                lo=std::max(lo,local+3.*step);
            }
            if(lo+depth+step*.5>f.z) continue;
            const Regions::Patch p{f.x,f.y,radius,lo,lo+depth,gx,gy};
            const double gain=benefit(p);
            if(gain>best_gain) {best=p;best_gain=gain;}
        }
        if(best_gain<=0.) continue;
        patches.push_back(best);
        if(patches.size()==64) break;
      }
    }
    return patches;
}
} // namespace Slic3r::CurvedTransition
