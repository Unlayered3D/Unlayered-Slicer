#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace Slic3r::CurvedTransition {

// Conservative deposited-material upper height cells. Their XY boxes include
// the entire bead/chord width; every object, support and adhesion path shares
// the map. The upright envelope is the configured four-sided pyramid below a
// globally blocking gantry plane, never a nozzle-tip-only line test.
class TravelClearance {
public:
    using Point=std::array<double,3>;
    bool empty() const {return m_cells.empty();}
    void invalidate() {m_complete=false;}
    void configure(double height,const std::array<double,4>& angles,double gantry) {
        m_cells.clear();m_top=-INFINITY;m_complete=true;m_enabled=false;
        if(!std::isfinite(height) || height<=0. || !std::isfinite(gantry) || gantry<=0.) return;
        for(size_t i=0;i<4;++i) {
            if(!std::isfinite(angles[i]) || angles[i]<=0. || angles[i]>=90.) return;
            m_tan[i]=std::tan(angles[i]*3.14159265358979323846/180.);
        }
        m_ceiling=std::min(height,gantry);m_enabled=true;
    }
    void add(const Point& a,const Point& b,double half_width,double bead_rise) {
        if(!m_enabled || !m_complete) return;
        if(!finite(a) || !finite(b) || !std::isfinite(half_width) || half_width<0. ||
           !std::isfinite(bead_rise) || bead_rise<0.) {invalidate();return;}
        // G-code coordinates round to microns. Include that output error in
        // addition to the bead edge; cells round outwards, never inwards.
        const double radius=half_width+.002;
        const double x0=std::min(a[0],b[0])-radius,x1=std::max(a[0],b[0])+radius;
        const double y0=std::min(a[1],b[1])-radius,y1=std::max(a[1],b[1])+radius;
        if(std::max({std::abs(x0),std::abs(x1),std::abs(y0),std::abs(y1)})>100000.) {invalidate();return;}
        const int ix0=int(std::floor(x0/cell)),ix1=int(std::floor(x1/cell));
        const int iy0=int(std::floor(y0/cell)),iy1=int(std::floor(y1/cell));
        if(double(ix1-ix0+1)*(iy1-iy0+1)>200000.) {invalidate();return;}
        const double top=std::max(a[2],b[2])+bead_rise+.002;
        m_top=std::max(m_top,top);
        for(int x=ix0;x<=ix1;++x) for(int y=iy0;y<=iy1;++y) {
            auto [it,inserted]=m_cells.emplace(std::make_pair(x,y),top);
            if(!inserted) it->second=std::max(it->second,top);
            if(m_cells.size()>200000) {invalidate();return;}
        }
    }
    std::optional<double> height(const Point& start,const std::vector<Point>& route,double clearance) const {
        if(!m_enabled || !m_complete || m_cells.empty() || route.empty() || !finite(start) ||
           !std::isfinite(clearance) || clearance<=0. || route.size()>2000000/m_cells.size()) return {};
        // Above the modelled cone or gantry, assume the carriage fills ALL XY.
        // Thus no distant tall object disappears just because it misses the cone.
        double bound=m_top-m_ceiling+clearance;
        Point previous=start;
        for(const auto& p:route) {
            if(!finite(p)) return {};
            const double x0=std::min(previous[0],p[0]),x1=std::max(previous[0],p[0]);
            const double y0=std::min(previous[1],p[1]),y1=std::max(previous[1],p[1]);
            for(const auto& [xy,z]:m_cells) {
                // Min of the pyramid's lower surface over this entire swept
                // route rectangle and obstacle cell. The box is an overbound
                // even for a diagonal, so off-axis corners cannot be missed.
                const double dx0=xy.first*cell-x1-.5,dx1=(xy.first+1)*cell-x0+.5;
                const double dy0=xy.second*cell-y1-.5,dy1=(xy.second+1)*cell-y0+.5;
                const double sx=dx0>0.?dx0*m_tan[0]:dx1<0.?-dx1*m_tan[1]:0.;
                const double sy=dy0>0.?dy0*m_tan[2]:dy1<0.?-dy1*m_tan[3]:0.;
                bound=std::max(bound,z-std::max(sx,sy)+clearance);
            }
            previous=p;
        }
        return bound;
    }
private:
    static bool finite(const Point& p) {return std::all_of(p.begin(),p.end(),[](double x){return std::isfinite(x);});}
    static constexpr double cell=2.;
    std::map<std::pair<int,int>,double> m_cells;
    std::array<double,4> m_tan{};
    double m_top=-INFINITY,m_ceiling=0.;
    bool m_complete=true,m_enabled=false;
};
} // namespace Slic3r::CurvedTransition
