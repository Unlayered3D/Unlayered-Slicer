#pragma once
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

namespace Slic3r::CurvedTransition {
// Bounded display metadata shared by live and imported Preview. It never
// controls motion, paths, permissions or physical clearance checks.
struct RegionDecision {
    int id=0,kind=0; // 0 roof disk, 1 underside domain, 2 planar fallback
    bool selected=false;
    double x=0.,y=0.,z=0.,radius=0.,angle=0.;
    double gain=0.,motion_seconds=0.,support_mm3=0.;
    std::string reason;
    std::string encode() const {
        std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(10);
        s<<"v1 "<<id<<' '<<kind<<' '<<int(selected)<<' '<<x<<' '<<y<<' '<<z<<' '<<radius<<' '<<angle<<' '
         <<gain<<' '<<motion_seconds<<' '<<support_mm3<<' '<<reason.substr(0,512);return s.str();
    }
    static bool decode(const std::string& text,RegionDecision& out) {
        if(text.size()>1024 || std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32;})) return false;
        std::istringstream s(text);s.imbue(std::locale::classic());std::string version;int selected=0;
        RegionDecision r;
        if(!(s>>version>>r.id>>r.kind>>selected>>r.x>>r.y>>r.z>>r.radius>>r.angle>>r.gain>>r.motion_seconds>>r.support_mm3) ||
           version!="v1" || r.id<0 || r.id>65535 || r.kind<0 || r.kind>2 || (selected!=0 && selected!=1)) return false;
        for(double v:{r.x,r.y,r.z,r.radius,r.angle,r.gain,r.motion_seconds,r.support_mm3})
            if(!std::isfinite(v) || std::abs(v)>1e9) return false;
        if(r.radius<0. || r.radius>10000. || r.angle<0. || r.angle>=90. || r.motion_seconds<0. || r.support_mm3<0.) return false;
        std::getline(s,r.reason);if(!r.reason.empty() && r.reason[0]==' ') r.reason.erase(0,1);
        if(r.reason.size()>512) return false;
        r.selected=selected!=0;out=std::move(r);return true;
    }
};
} // namespace Slic3r::CurvedTransition
