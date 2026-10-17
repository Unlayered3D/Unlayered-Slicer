#pragma once
#include "PrintConfig.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace Slic3r::CurvedTransition {

// One active tip for the whole job. The common envelope describes the ENTIRE
// moving head about that tip, including the other tip in its parked position.
// These are recorded bounds, not measurements performed by the software.
struct FixedNozzleLimits {
    size_t nozzle=0;
    double min_layer=0., max_layer=0., max_z=0., z_speed=0.;

    static FixedNozzleLimits from_config(const PrintConfig& c, size_t active)
    {
        const size_t count=c.nozzle_diameter.size();
        if(count==0 || active>=count) throw std::invalid_argument("No valid fixed active nozzle.");
        if(count>1) {
            if(count!=2 || c.nonplanar_fixed_nozzle.value!=int(active+1))
                throw std::invalid_argument("Curved printing requires one explicitly selected fixed nozzle on a two-nozzle carriage.");
            if(c.nonplanar_clearance_nozzle.value!=int(active+1) ||
               !c.nonplanar_fixed_clearance_recorded.value ||
               c.nonplanar_fixed_clearance_source.value.find_first_not_of(" \t\r\n")==std::string::npos)
                throw std::invalid_argument("Record the whole toolhead clearance for this active nozzle, including the parked nozzle. Changing the active nozzle requires reviewing the envelope.");
            if(c.extruder_offset.size()!=count || !c.extruder_offset.values[active].allFinite() ||
               c.extruder_printable_area.size()!=count || c.extruder_printable_area.values[active].size()<3 ||
               std::any_of(c.extruder_printable_area.values[active].begin(),c.extruder_printable_area.values[active].end(),
                   [](const Vec2d& p){return !p.allFinite();}) ||
               c.extruder_printable_height.size()!=count || c.min_layer_height.size()!=count || c.max_layer_height.size()!=count)
                throw std::invalid_argument("Fixed-nozzle printing needs the active tip offset, printable area/height and layer limits.");
            const auto& route=c.physical_extruder_map.values;
            if(route.size()!=2 || !((route[0]==0 && route[1]==1) || (route[0]==1 && route[1]==0)))
                throw std::invalid_argument("The stock physical-nozzle routing must be a complete two-nozzle permutation.");
            const auto envelope=HotendEnvelope::from_config(c);
            if(!std::isfinite(envelope.height) || envelope.height<=0. ||
               !std::isfinite(c.nonplanar_gantry_height.value) || c.nonplanar_gantry_height.value<=0.)
                throw std::invalid_argument("Fixed-nozzle printing requires positive whole-toolhead and gantry bounds.");
            for(double angle:envelope.angle)
                if(!std::isfinite(angle) || angle<=0. || angle>=90.)
                    throw std::invalid_argument("Record all four conservative whole-toolhead side angles.");
        }
        FixedNozzleLimits out;out.nozzle=active;
        out.min_layer=c.min_layer_height.get_at(active);out.max_layer=c.max_layer_height.get_at(active);
        out.max_z=std::min(c.nonplanar_virtual_max_z.value,c.printable_height.value);
        if(count>1) {
            const double height=c.extruder_printable_height.values[active];
            if(!std::isfinite(height) || height<=0.) throw std::invalid_argument("Active-nozzle printable height is unknown.");
            out.max_z=std::min(out.max_z,height);
        }
        if(!std::isfinite(out.min_layer) || out.min_layer<=0. || !std::isfinite(out.max_layer) || out.max_layer<out.min_layer ||
           !std::isfinite(out.max_z) || out.max_z<=0.)
            throw std::invalid_argument("Curved printing requires finite positive active-nozzle layer and Z limits.");
        // The writer uses normal-mode entries of stride-2 machine-limit arrays.
        // Use the slowest normal-mode entry; silent-mode zeros are placeholders.
        out.z_speed=INFINITY;
        const size_t stride=printer_options_with_variant_2.count("machine_max_speed_z")?2:1;
        for(size_t i=0;i<c.machine_max_speed_z.size();i+=stride) {
            const double speed=c.machine_max_speed_z.values[i];
            if(!std::isfinite(speed) || speed<=0.) throw std::invalid_argument("Curved printing requires a positive machine Z speed.");
            out.z_speed=std::min(out.z_speed,speed);
        }
        if(!std::isfinite(out.z_speed)) throw std::invalid_argument("Machine Z speed is unknown.");
        return out;
    }
};

// Validate AFTER template expansion. Stock Bambu layer code controls cooling
// and progress; it must never move axes, change the active tool or the frame.
inline bool layer_code_safe(const std::string& code)
{
    std::istringstream lines(code);std::string line;
    while(std::getline(lines,line)) {
        line=line.substr(0,line.find(';'));
        size_t i=0;while(i<line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if(i==line.size()) continue;
        const size_t first=i++;
        while(i<line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
        const std::string command=line.substr(first,i-first);
        const std::string allowed=command=="M73"?"PLRC":command=="M991"?"SP":command=="M106"?"PS":
            command=="M142"?"PRSTUVOWL":command=="G92"?"E":"";
        if(allowed.empty()) return false;
        bool e_reset=false;
        while(i<line.size()) {
            while(i<line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if(i==line.size()) break;
            const char axis=line[i++];if(allowed.find(axis)==std::string::npos) return false;
            char* end=nullptr;const char* start=line.c_str()+i;
            const double value=std::strtod(start,&end);
            if(end==start || !std::isfinite(value)) return false;
            i=size_t(end-line.c_str());
            if(command=="G92") {if(axis!='E' || value!=0. || e_reset) return false;e_reset=true;}
        }
        if(command=="G92" && !e_reset) return false;
    }
    return true;
}
}
