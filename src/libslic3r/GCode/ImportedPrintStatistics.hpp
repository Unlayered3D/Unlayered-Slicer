#ifndef slic3r_ImportedPrintStatistics_hpp_
#define slic3r_ImportedPrintStatistics_hpp_

#include "libslic3r/Print.hpp"

#include <cmath>
#include <stdexcept>

namespace Slic3r {

// A loaded G-code file has no sliced Print to supply these totals. Reconstruct
// them from the same processed volumes/materials used by the Preview role rows.
// Keep internal units in mm, mm^3 and grams; the viewer converts for display.
inline PrintStatistics imported_print_statistics(const GCodeProcessorResult& result)
{
    PrintStatistics stats;
    for (const auto& [id, volume] : result.print_statistics.total_volumes_per_extruder) {
        if (id >= result.filament_diameters.size() || id >= result.filament_densities.size() ||
            id >= result.filament_costs.size())
            throw std::invalid_argument("Imported G-code filament statistics have incomplete material data");
        const double diameter = result.filament_diameters[id];
        const double density = result.filament_densities[id];
        const double cost_per_kg = result.filament_costs[id];
        if (!std::isfinite(volume) || volume < 0.0 || !std::isfinite(diameter) || diameter <= 0.0 ||
            !std::isfinite(density) || density < 0.0 || !std::isfinite(cost_per_kg) || cost_per_kg < 0.0)
            throw std::invalid_argument("Imported G-code filament statistics contain invalid material data");

        const double area = PI * sqr(0.5 * diameter);
        const double weight = volume * density * 0.001;
        stats.total_extruded_volume += volume;
        stats.total_used_filament += volume / area;
        stats.total_weight += weight;
        stats.total_cost += weight * cost_per_kg * 0.001;
    }
    stats.filament_stats = result.print_statistics.model_volumes_per_extruder;
    return stats;
}

} // namespace Slic3r

#endif
