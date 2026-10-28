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
    bool has_breakdown=false;
    double roof_gain_percent=0.,underside_gain_percent=0.,time_delta_seconds=0.,time_delta_percent=0.;
    double support_saved_mm3=0.,model_added_mm3=0.,travel_seconds=0.;
    size_t travel_routes=0,local_routes=0;
    std::string rejection_reason;
    std::string encode() const {
        std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(10);
        s<<"v2 "<<id<<' '<<kind<<' '<<int(selected)<<' '<<x<<' '<<y<<' '<<z<<' '<<radius<<' '<<angle<<' '
         <<gain<<' '<<motion_seconds<<' '<<support_mm3<<' '<<int(has_breakdown)<<' '
         <<roof_gain_percent<<' '<<underside_gain_percent<<' '<<time_delta_seconds<<' '<<time_delta_percent<<' '
         <<support_saved_mm3<<' '<<model_added_mm3<<' '<<travel_seconds<<' '<<travel_routes<<' '<<local_routes<<' '
         <<std::quoted(reason.substr(0,160))<<' '<<std::quoted(rejection_reason.substr(0,160));return s.str();
    }
    static bool decode(const std::string& text,RegionDecision& out) {
        if(text.size()>1024 || std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32;})) return false;
        std::istringstream s(text);s.imbue(std::locale::classic());std::string version;int selected=0,breakdown=0;
        RegionDecision r;
        if(!(s>>version>>r.id>>r.kind>>selected>>r.x>>r.y>>r.z>>r.radius>>r.angle>>r.gain>>r.motion_seconds>>r.support_mm3) ||
           (version!="v1" && version!="v2") || r.id<0 || r.id>65535 || r.kind<0 || r.kind>2 || (selected!=0 && selected!=1)) return false;
        if(version=="v2") {
            if(!(s>>breakdown>>r.roof_gain_percent>>r.underside_gain_percent>>r.time_delta_seconds>>r.time_delta_percent>>
                 r.support_saved_mm3>>r.model_added_mm3>>r.travel_seconds>>r.travel_routes>>r.local_routes>>
                 std::quoted(r.reason)>>std::quoted(r.rejection_reason)) || (breakdown!=0 && breakdown!=1)) return false;
            s>>std::ws;if(!s.eof()) return false;
            if(r.travel_routes>100000000 || r.local_routes>r.travel_routes || r.travel_seconds<0. ||
               r.reason.size()>160 || r.rejection_reason.size()>160) return false;
            r.has_breakdown=breakdown!=0;
        } else {
            std::getline(s,r.reason);if(!r.reason.empty() && r.reason[0]==' ') r.reason.erase(0,1);
            if(r.reason.size()>512) return false;
        }
        for(double v:{r.x,r.y,r.z,r.radius,r.angle,r.gain,r.motion_seconds,r.support_mm3})
            if(!std::isfinite(v) || std::abs(v)>1e9) return false;
        for(double v:{r.roof_gain_percent,r.underside_gain_percent,r.time_delta_seconds,r.time_delta_percent,
                      r.support_saved_mm3,r.model_added_mm3,r.travel_seconds})
            if(!std::isfinite(v) || std::abs(v)>1e9) return false;
        if(r.radius<0. || r.radius>10000. || r.angle<0. || r.angle>=90. || r.motion_seconds<0. || r.support_mm3<0.) return false;
        r.selected=selected!=0;out=std::move(r);return true;
    }
};
} // namespace Slic3r::CurvedTransition
