#ifndef slic3r_GUI_Actions_PlatesPreviewLogic_hpp_
#define slic3r_GUI_Actions_PlatesPreviewLogic_hpp_

// Pure helpers behind the plate, layer-event, preview and capture actions (PlateActions.cpp,
// LayerEventActions.cpp, PreviewActions.cpp). Nothing here touches wxWidgets, the GUI app or
// OpenGL, so tests/slic3rutils/test_actions_plates.cpp exercises it directly.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

namespace Slic3r { namespace GUI { namespace Actions { namespace PlatesPreview {

// ---------------------------------------------------------------------------------------------
// Plate settings vocabulary (wire ids are camelCase and stable; the config's own enum strings are
// accepted too, so "Textured PEI Plate" works as well as "texturedPEI").
// ---------------------------------------------------------------------------------------------

struct BedTypeName { BedType type; const char* id; const char* config; };
inline const std::vector<BedTypeName>& bed_type_names()
{
    static const std::vector<BedTypeName> names = {
        {btDefault, "default", "Default Plate"},
        {btPC, "coolPlate", "Cool Plate"},
        {btEP, "engineeringPlate", "Engineering Plate"},
        {btPEI, "highTempPlate", "High Temp Plate"},
        {btPTE, "texturedPEI", "Textured PEI Plate"},
        {btPCT, "texturedCoolPlate", "Textured Cool Plate"},
        {btSuperTack, "supertack", "Supertack Plate"},
    };
    return names;
}

inline std::vector<std::string> bed_type_ids()
{
    std::vector<std::string> out;
    for (const BedTypeName& n : bed_type_names())
        out.emplace_back(n.id);
    return out;
}

inline std::optional<BedType> bed_type_from_id(const std::string& id)
{
    for (const BedTypeName& n : bed_type_names())
        if (id == n.id || id == n.config)
            return n.type;
    return std::nullopt;
}

inline std::string bed_type_id(BedType type)
{
    for (const BedTypeName& n : bed_type_names())
        if (n.type == type)
            return n.id;
    return "default";
}

// "default" = same as the global print sequence (PrintSequence::ByDefault). "byGroup" prints the
// objects group by group (each object's print_group number; cut-plane objects need it), the Plate
// Settings dialog's third entry and the plate tab's "By group".
inline std::optional<PrintSequence> print_sequence_from_id(const std::string& id)
{
    if (id == "default") return PrintSequence::ByDefault;
    if (id == "byLayer") return PrintSequence::ByLayer;
    if (id == "byObject") return PrintSequence::ByObject;
    if (id == "byGroup") return PrintSequence::ByGroup;
    return std::nullopt;
}

inline std::string print_sequence_id(PrintSequence seq)
{
    switch (seq) {
    case PrintSequence::ByLayer: return "byLayer";
    case PrintSequence::ByObject: return "byObject";
    case PrintSequence::ByGroup: return "byGroup";
    default: return "default";
    }
}

// Filament grouping on multi-nozzle printers (the Filament Grouping dialog / legend panel).
// "default" = same as the global mode. The fully manual per-nozzle mode (fmmNozzleManual) is not
// offered: it needs the physical-nozzle map the dialog builds from the machine's nozzle rack.
inline std::optional<FilamentMapMode> filament_map_mode_from_id(const std::string& id)
{
    if (id == "default") return fmmDefault;
    if (id == "autoForFlush") return fmmAutoForFlush;
    if (id == "autoForMatch") return fmmAutoForMatch;
    if (id == "manual") return fmmManual;
    return std::nullopt;
}

inline std::string filament_map_mode_id(FilamentMapMode mode)
{
    switch (mode) {
    case fmmAutoForFlush: return "autoForFlush";
    case fmmAutoForMatch: return "autoForMatch";
    case fmmManual: return "manual";
    case fmmNozzleManual: return "nozzleManual";
    default: return "default";
    }
}

// A manual map: one 1-based nozzle per filament slot, every slot listed.
inline bool valid_filament_maps(const std::vector<int>& maps, int filament_count, int nozzle_count, std::string& why)
{
    if (int(maps.size()) != filament_count) {
        why = "filamentMaps lists " + std::to_string(maps.size()) + " slots; the project has " + std::to_string(filament_count);
        return false;
    }
    for (int n : maps)
        if (n < 1 || n > nozzle_count) {
            why = "nozzle " + std::to_string(n) + " is out of range (1-" + std::to_string(nozzle_count) + ")";
            return false;
        }
    return true;
}

// Plate names: what the plate-name dialog accepts, minus control characters.
inline bool valid_plate_name(const std::string& name, std::string& why)
{
    if (name.size() > 120) {
        why = "name is longer than 120 bytes";
        return false;
    }
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f) {
            why = "name contains a control character";
            return false;
        }
    return true;
}

// The skirt start angle a plate may carry (PrintConfig "skirt_start_angle": -180..180 degrees).
inline bool valid_skirt_start_angle(double deg) { return std::isfinite(deg) && deg >= -180. && deg <= 180.; }

// Filament order lists (1-based filament numbers): each listed once, all in range.
inline bool valid_filament_order(const std::vector<int>& order, int filament_count, std::string& why)
{
    std::vector<bool> seen(size_t(std::max(filament_count, 0)) + 1, false);
    for (int f : order) {
        if (f < 1 || f > filament_count) {
            why = "filament " + std::to_string(f) + " is out of range (1-" + std::to_string(filament_count) + ")";
            return false;
        }
        if (seen[size_t(f)]) {
            why = "filament " + std::to_string(f) + " is listed twice";
            return false;
        }
        seen[size_t(f)] = true;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Layer events (the preview layer slider's pause / filament change / custom G-code ticks)
// ---------------------------------------------------------------------------------------------

struct LayerEvent
{
    CustomGCode::Type   type = CustomGCode::PausePrint;
    std::optional<double> z;      // mm, print_z of the layer the event precedes
    std::optional<int>    layer;  // 1-based layer number (needs a slice result)
    int                 filament = 0; // 1-based; toolChange
    std::string         color;    // "#RRGGBB"; toolChange (defaults to the filament's colour)
    std::string         gcode;    // custom
    std::string         message;  // pause (informative)
};

inline const char* layer_event_type_id(CustomGCode::Type t)
{
    switch (t) {
    case CustomGCode::PausePrint: return "pause";
    case CustomGCode::ToolChange: return "toolChange";
    case CustomGCode::Custom: return "custom";
    case CustomGCode::Template: return "template";
    case CustomGCode::ColorChange: return "colorChange";
    default: return "unknown";
    }
}

inline bool valid_hex_color(const std::string& c)
{
    if (c.size() != 7 || c[0] != '#')
        return false;
    for (size_t i = 1; i < 7; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(c[i])))
            return false;
    return true;
}

// Splits custom G-code into the lines the confirmation lists. Refuses what the slider's text box
// could never have produced: NUL/control bytes other than tab, CR, LF; more than 4096 bytes or 64
// lines. Empty text is refused (the slider refuses it too).
inline bool custom_gcode_lines(const std::string& text, std::vector<std::string>& lines, std::string& why)
{
    lines.clear();
    if (text.empty()) {
        why = "gcode is empty";
        return false;
    }
    if (text.size() > 4096) {
        why = "gcode is longer than 4096 bytes";
        return false;
    }
    std::string cur;
    for (unsigned char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        if (c == '\r')
            continue;
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            why = "gcode contains a control character";
            return false;
        }
        cur.push_back(char(c));
    }
    if (!cur.empty())
        lines.push_back(cur);
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    if (lines.empty()) {
        why = "gcode is empty";
        return false;
    }
    if (lines.size() > 64) {
        why = "gcode has more than 64 lines";
        return false;
    }
    return true;
}

// Parses one item of layer.setEvents (already validated against the schema). Semantic checks
// only: which fields go with which type.
inline bool parse_layer_event(const nlohmann::json& j, LayerEvent& out, std::string& why)
{
    const std::string type = j.value("type", std::string());
    if (type == "pause")
        out.type = CustomGCode::PausePrint;
    else if (type == "toolChange")
        out.type = CustomGCode::ToolChange;
    else if (type == "custom")
        out.type = CustomGCode::Custom;
    else if (type == "template")
        out.type = CustomGCode::Template;
    else {
        why = "unknown event type '" + type + "' (pause, toolChange, custom, template)";
        return false;
    }
    const bool has_z     = j.contains("z") && j["z"].is_number();
    const bool has_layer = j.contains("layer") && j["layer"].is_number_integer();
    if (has_z == has_layer) {
        why = "give exactly one of z (mm) or layer (1-based)";
        return false;
    }
    if (has_z) {
        const double z = j["z"].get<double>();
        if (!(z > 0.) || z > 10000.) {
            why = "z must be above 0 mm";
            return false;
        }
        out.z = z;
    } else {
        const int layer = j["layer"].get<int>();
        if (layer < 1) {
            why = "layer is 1-based";
            return false;
        }
        out.layer = layer;
    }
    out.filament = j.contains("filament") && j["filament"].is_number_integer() ? j["filament"].get<int>() : 0;
    out.color    = j.contains("color") && j["color"].is_string() ? j["color"].get<std::string>() : std::string();
    out.gcode    = j.contains("gcode") && j["gcode"].is_string() ? j["gcode"].get<std::string>() : std::string();
    out.message  = j.contains("message") && j["message"].is_string() ? j["message"].get<std::string>() : std::string();
    if (out.type == CustomGCode::ToolChange && out.filament < 1) {
        why = "toolChange needs filament (1-based slot)";
        return false;
    }
    if (out.type != CustomGCode::ToolChange && out.filament != 0) {
        why = "filament goes only with toolChange";
        return false;
    }
    if (!out.color.empty() && (out.type != CustomGCode::ToolChange || !valid_hex_color(out.color))) {
        why = "color is a #RRGGBB colour and goes only with toolChange";
        return false;
    }
    if (out.type == CustomGCode::Custom) {
        std::vector<std::string> lines;
        if (!custom_gcode_lines(out.gcode, lines, why))
            return false;
    } else if (!out.gcode.empty()) {
        why = "gcode goes only with custom";
        return false;
    }
    if (!out.message.empty() && out.type != CustomGCode::PausePrint) {
        why = "message goes only with pause";
        return false;
    }
    return true;
}

// The layer whose z is nearest `z`. Returns -1 when there are none. Preview layer zs are ascending
// for a planar print; a tilted (multiplanar) print can step back down, and then every layer is
// compared (the first of equally near ones wins).
inline int nearest_layer_index(const std::vector<double>& layer_zs, double z)
{
    if (layer_zs.empty())
        return -1;
    if (!std::is_sorted(layer_zs.begin(), layer_zs.end())) {
        size_t best = 0;
        for (size_t i = 1; i < layer_zs.size(); ++i)
            if (std::abs(layer_zs[i] - z) < std::abs(layer_zs[best] - z))
                best = i;
        return int(best);
    }
    auto it = std::lower_bound(layer_zs.begin(), layer_zs.end(), z);
    if (it == layer_zs.end())
        return int(layer_zs.size()) - 1;
    if (it == layer_zs.begin())
        return 0;
    const auto prev = std::prev(it);
    return (z - *prev) <= (*it - z) ? int(prev - layer_zs.begin()) : int(it - layer_zs.begin());
}

// Resolves an event's position to the print_z stored in CustomGCode::Item. With layers known, z
// snaps to the nearest layer (as a slider tick always sits on one) and a layer number becomes that
// layer's print_z. Without a slice result, z is kept as given -- ToolOrdering applies an item to the
// layer whose print_z is nearest -- and a layer number cannot be resolved.
inline bool resolve_event_z(const LayerEvent& ev, const std::vector<double>& layer_zs, double& print_z, int& layer1, std::string& why)
{
    layer1 = 0;
    if (ev.layer) {
        if (layer_zs.empty()) {
            why = "layer numbers need a slice result for this plate; slice first or give z in mm";
            return false;
        }
        if (*ev.layer > int(layer_zs.size())) {
            why = "layer " + std::to_string(*ev.layer) + " is above the top layer (" + std::to_string(layer_zs.size()) + ")";
            return false;
        }
        layer1  = *ev.layer;
        print_z = layer_zs[size_t(*ev.layer - 1)];
        return true;
    }
    if (layer_zs.empty()) {
        print_z = *ev.z;
        return true;
    }
    const double top = *std::max_element(layer_zs.begin(), layer_zs.end());
    if (*ev.z > top + 1e-6) {
        why = "z " + std::to_string(*ev.z) + " mm is above the top layer (" + std::to_string(top) + " mm)";
        return false;
    }
    const int idx = nearest_layer_index(layer_zs, *ev.z);
    layer1        = idx + 1;
    print_z       = layer_zs[size_t(idx)];
    return true;
}

// The slider's mode for the plate (Preview::update_layers_slider_mode + IMSlider::
// SetModeAndOnlyExtruder): several filaments with the plate printing from at least one -> the
// "multi as single" mode, else single extruder.
inline CustomGCode::Mode slider_mode(size_t filament_count, const std::vector<int>& plate_extruders)
{
    if (filament_count > 1 && !plate_extruders.empty())
        return CustomGCode::MultiAsSingle;
    return CustomGCode::SingleExtruder;
}

inline bool same_z(double a, double b) { return std::abs(a - b) < 1e-4; }

// mode "replace": the plate's events become `items`. mode "add": each item replaces any event at
// the same print_z (one tick per layer, as on the slider) and the rest stay. The result is sorted
// by print_z, which ToolOrdering::assign_custom_gcodes relies on.
inline CustomGCode::Info merge_layer_events(const CustomGCode::Info& existing, const std::vector<CustomGCode::Item>& items,
                                            bool replace, CustomGCode::Mode mode)
{
    CustomGCode::Info out;
    out.mode = mode;
    if (!replace)
        out.gcodes = existing.gcodes;
    for (const CustomGCode::Item& item : items) {
        out.gcodes.erase(std::remove_if(out.gcodes.begin(), out.gcodes.end(),
                                        [&item](const CustomGCode::Item& e) { return same_z(e.print_z, item.print_z); }),
                         out.gcodes.end());
        out.gcodes.push_back(item);
    }
    std::stable_sort(out.gcodes.begin(), out.gcodes.end());
    if (out.gcodes.empty() && !replace)
        out.mode = existing.mode == CustomGCode::Undef ? mode : existing.mode;
    return out;
}

// Removes the events at the given print_zs (each matched within 1e-4 mm); returns how many went.
inline size_t remove_layer_events(CustomGCode::Info& info, const std::vector<double>& zs)
{
    const size_t before = info.gcodes.size();
    info.gcodes.erase(std::remove_if(info.gcodes.begin(), info.gcodes.end(),
                                     [&zs](const CustomGCode::Item& e) {
                                         return std::any_of(zs.begin(), zs.end(), [&e](double z) { return same_z(z, e.print_z); });
                                     }),
                      info.gcodes.end());
    return before - info.gcodes.size();
}

inline nlohmann::json layer_event_json(const CustomGCode::Item& item, const std::vector<double>& layer_zs)
{
    nlohmann::json j = {{"type", layer_event_type_id(item.type)}, {"z", item.print_z}};
    const int idx = nearest_layer_index(layer_zs, item.print_z);
    if (idx >= 0 && same_z(layer_zs[size_t(idx)], item.print_z))
        j["layer"] = idx + 1;
    else
        j["layer"] = nullptr;
    if (item.type == CustomGCode::ToolChange || item.type == CustomGCode::ColorChange) {
        j["filament"] = item.extruder;
        j["color"]    = item.color;
    }
    if (item.type == CustomGCode::Custom)
        j["gcode"] = item.extra;
    if (item.type == CustomGCode::PausePrint && !item.extra.empty())
        j["message"] = item.extra;
    return j;
}

inline nlohmann::json layer_events_json(const CustomGCode::Info& info, const std::vector<double>& layer_zs)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const CustomGCode::Item& item : info.gcodes)
        arr.push_back(layer_event_json(item, layer_zs));
    return arr;
}

// ---------------------------------------------------------------------------------------------
// Preview statistics from a slice result
// ---------------------------------------------------------------------------------------------

inline const char* extrusion_role_id(ExtrusionRole role)
{
    switch (role) {
    case erPerimeter: return "innerWall";
    case erExternalPerimeter: return "outerWall";
    case erOverhangPerimeter: return "overhangWall";
    case erInternalInfill: return "sparseInfill";
    case erSolidInfill: return "internalSolidInfill";
    case erTopSolidInfill: return "topSurface";
    case erBottomSurface: return "bottomSurface";
    case erIroning: return "ironing";
    case erBridgeInfill: return "bridge";
    case erInternalBridgeInfill: return "internalBridge";
    case erGapFill: return "gapFill";
    case erSkirt: return "skirt";
    case erBrim: return "brim";
    case erSupportMaterial: return "support";
    case erSupportMaterialInterface: return "supportInterface";
    case erSupportTransition: return "supportTransition";
    case erWipeTower: return "primeTower";
    case erCustom: return "custom";
    case erMixed: return "mixed";
    default: return "none";
    }
}

// The feature ids above, in a fixed order, and back (the preview.setView "features" keys).
inline const std::vector<std::string>& extrusion_role_ids()
{
    static const std::vector<std::string> ids = {"innerWall",     "outerWall",      "overhangWall", "sparseInfill", "internalSolidInfill",
                                                 "topSurface",    "bottomSurface",  "ironing",      "bridge",       "internalBridge",
                                                 "gapFill",       "skirt",          "brim",         "support",      "supportInterface",
                                                 "supportTransition", "primeTower", "custom"};
    return ids;
}

inline std::optional<ExtrusionRole> extrusion_role_from_id(const std::string& id)
{
    for (int r = int(erNone) + 1; r < int(erMixed); ++r)
        if (id == extrusion_role_id(ExtrusionRole(r)))
            return ExtrusionRole(r);
    return std::nullopt;
}

// The legend's option rows (preview.setView "options" keys), in libvgcode::EOptionType terms:
// Travels, Wipes, Retractions, Unretractions, Seams, ToolChanges. PreviewActions.cpp maps them.
inline const std::vector<std::string>& preview_option_ids()
{
    static const std::vector<std::string> ids = {"travel", "wipe", "retract", "unretract", "seams", "filamentChanges"};
    return ids;
}

// ---------------------------------------------------------------------------------------------
// Layer numbering. ONE source for every action: the G-code preview's layers, i.e. what the
// preview's layer slider numbers. libvgcode builds them from the slice result's moves (skipping
// moves[0], see libvgcode::convert): a new layer starts wherever MoveVertex::layer_id changes from
// the previous move, so ids the file skips leave no empty layer behind (ViewerImpl::load,
// Layers::update). A layer's z is the height of its last non-custom extrusion (0 when it has
// none), its time the sum of its moves' times. Layer N (1-based) is item N-1.
// ---------------------------------------------------------------------------------------------

struct PreviewLayer
{
    double z = 0.;
    std::array<double, 2> time_s{0., 0.}; // normal, stealth
    size_t first_move = 0, last_move = 0; // indices into moves
};

inline std::vector<PreviewLayer> preview_layers(const std::vector<GCodeProcessorResult::MoveVertex>& moves)
{
    std::vector<PreviewLayer> out;
    unsigned int last_id = 0;
    for (size_t i = 1; i < moves.size(); ++i) {
        const GCodeProcessorResult::MoveVertex& m = moves[i];
        if (out.empty() || m.layer_id != last_id) {
            last_id = m.layer_id;
            PreviewLayer l;
            l.first_move = i;
            out.push_back(l);
        }
        PreviewLayer& l = out.back();
        l.last_move     = i;
        for (size_t k = 0; k < 2 && k < m.time.size(); ++k)
            l.time_s[k] += double(m.time[k]);
        if (m.type == Slic3r::EMoveType::Extrude && m.extrusion_role != erCustom)
            l.z = double(m.position.z());
    }
    return out;
}

inline std::vector<double> preview_layer_zs(const std::vector<PreviewLayer>& layers)
{
    std::vector<double> zs;
    zs.reserve(layers.size());
    for (const PreviewLayer& l : layers)
        zs.push_back(l.z);
    return zs;
}

// ---------------------------------------------------------------------------------------------
// Move summary (the legend's feature, option and layer figures)
// ---------------------------------------------------------------------------------------------

struct MoveSummary
{
    struct Role { double time_s = 0.; double extruded_mm = 0.; double volume_mm3 = 0.; double length_mm = 0.; };
    // The legend's option rows: count, time, and length (filament mm for retract/unretract, path mm
    // for wipe and seam), as GCodeViewer::load_as_gcode sums them (internal-only moves skipped).
    struct Option { size_t count = 0; double time_s = 0.; double length_mm = 0.; };
    std::map<std::string, Role> roles;
    Option retract, unretract, wipe, seam;
    double travel_time_s = 0.;
    double travel_mm     = 0.;
    size_t travel_moves  = 0;
    double other_time_s  = 0.; // retracts, tool changes, pauses...
    double total_time_s  = 0.;
    size_t pauses = 0, tool_changes = 0, custom_gcodes = 0, color_changes = 0;
    std::vector<std::pair<double, double>> layers; // (z, seconds) per preview layer
};

// Sums a slice result's moves. MoveVertex::time[mode] is each move's own duration; time_mode 0 =
// normal, 1 = stealth.
inline MoveSummary summarize_moves(const std::vector<GCodeProcessorResult::MoveVertex>& moves, size_t time_mode = 0)
{
    MoveSummary s;
    const GCodeProcessorResult::MoveVertex* prev = nullptr;
    for (const GCodeProcessorResult::MoveVertex& m : moves) {
        const double t = time_mode < m.time.size() ? double(m.time[time_mode]) : 0.;
        s.total_time_s += t;
        const double len = prev != nullptr ? double((m.position - prev->position).norm()) : 0.;
        auto option = [&m, t](MoveSummary::Option& o, double length) {
            if (m.internal_only)
                return;
            ++o.count;
            o.time_s += t;
            o.length_mm += length;
        };
        switch (m.type) {
        case Slic3r::EMoveType::Extrude: {
            MoveSummary::Role& r = s.roles[extrusion_role_id(m.extrusion_role)];
            r.time_s += t;
            r.extruded_mm += double(m.delta_extruder);
            r.length_mm += len;
            r.volume_mm3 += double(m.mm3_per_mm) * len;
            break;
        }
        case Slic3r::EMoveType::Travel:
            s.travel_time_s += t;
            s.travel_mm += len;
            ++s.travel_moves;
            break;
        case Slic3r::EMoveType::Retract: option(s.retract, std::fabs(double(m.delta_extruder))); s.other_time_s += t; break;
        case Slic3r::EMoveType::Unretract: option(s.unretract, std::fabs(double(m.delta_extruder))); s.other_time_s += t; break;
        case Slic3r::EMoveType::Wipe: option(s.wipe, double(m.travel_dist)); s.other_time_s += t; break;
        case Slic3r::EMoveType::Seam: option(s.seam, double(m.travel_dist)); s.other_time_s += t; break;
        case Slic3r::EMoveType::Pause_Print: ++s.pauses; s.other_time_s += t; break;
        case Slic3r::EMoveType::Tool_change: ++s.tool_changes; s.other_time_s += t; break;
        case Slic3r::EMoveType::Custom_GCode: ++s.custom_gcodes; s.other_time_s += t; break;
        case Slic3r::EMoveType::Color_change: ++s.color_changes; s.other_time_s += t; break;
        default: s.other_time_s += t; break;
        }
        prev = &m;
    }
    for (const PreviewLayer& l : preview_layers(moves))
        s.layers.emplace_back(l.z, time_mode < 2 ? l.time_s[time_mode] : 0.);
    return s;
}

inline nlohmann::json move_summary_json(const MoveSummary& s, bool with_layers, size_t max_layers = 2000)
{
    nlohmann::json roles = nlohmann::json::object();
    for (const auto& [id, r] : s.roles)
        roles[id] = {{"timeS", r.time_s},
                     {"percent", s.total_time_s > 0. ? 100. * r.time_s / s.total_time_s : 0.},
                     {"filamentMm", r.extruded_mm},
                     {"volumeMm3", r.volume_mm3},
                     {"pathMm", r.length_mm}};
    auto option = [&s](const MoveSummary::Option& o, const char* length_key) {
        return nlohmann::json{{"count", o.count},
                              {"timeS", o.time_s},
                              {"percent", s.total_time_s > 0. ? 100. * o.time_s / s.total_time_s : 0.},
                              {length_key, o.length_mm}};
    };
    nlohmann::json j = {{"totalTimeS", s.total_time_s},
                        {"features", roles},
                        {"travel", {{"timeS", s.travel_time_s}, {"distanceMm", s.travel_mm}, {"moves", s.travel_moves}}},
                        {"options", {{"retract", option(s.retract, "filamentMm")},
                                     {"unretract", option(s.unretract, "filamentMm")},
                                     {"wipe", option(s.wipe, "distanceMm")},
                                     {"seams", option(s.seam, "distanceMm")}}},
                        {"otherTimeS", s.other_time_s},
                        {"events", {{"pauses", s.pauses},
                                    {"toolChanges", s.tool_changes},
                                    {"customGcodes", s.custom_gcodes},
                                    {"colorChanges", s.color_changes}}},
                        {"layerCount", s.layers.size()}};
    if (with_layers) {
        nlohmann::json layers = nlohmann::json::array();
        for (size_t i = 0; i < s.layers.size() && i < max_layers; ++i)
            layers.push_back({{"layer", i + 1}, {"z", s.layers[i].first}, {"timeS", s.layers[i].second}});
        j["layers"]          = layers;
        j["layersTruncated"] = s.layers.size() > max_layers;
    }
    if (!s.layers.empty()) {
        size_t slowest = 0, fastest = 0;
        for (size_t i = 0; i < s.layers.size(); ++i) {
            if (s.layers[i].second > s.layers[slowest].second) slowest = i;
            if (s.layers[i].second < s.layers[fastest].second) fastest = i;
        }
        j["slowestLayer"] = {{"layer", slowest + 1}, {"z", s.layers[slowest].first}, {"timeS", s.layers[slowest].second}};
        j["fastestLayer"] = {{"layer", fastest + 1}, {"z", s.layers[fastest].first}, {"timeS", s.layers[fastest].second}};
    }
    return j;
}

// The colour scales of the range colourings, as libvgcode's update_color_ranges spans them with
// the default visibility (travel and wipe hidden): min/max over the extrusion moves, layer time
// over the preview layers. Values are not rounded into the legend's bins.
inline nlohmann::json color_ranges_json(const std::vector<GCodeProcessorResult::MoveVertex>& moves, size_t time_mode = 0)
{
    struct R { double lo = 0., hi = 0.; bool any = false; void add(double v) { if (!any) { lo = hi = v; any = true; } else { lo = std::min(lo, v); hi = std::max(hi, v); } } };
    R height, width, speed, actual_speed, flow, actual_flow, fan, temp, pa, accel, jerk, layer_time;
    for (size_t i = 1; i < moves.size(); ++i) {
        const GCodeProcessorResult::MoveVertex& m = moves[i];
        if (m.type != Slic3r::EMoveType::Extrude)
            continue;
        height.add(m.height);
        width.add(m.width);
        flow.add(m.volumetric_rate());
        actual_flow.add(m.actual_volumetric_rate());
        fan.add(m.fan_speed);
        temp.add(m.temperature);
        if (m.pressure_advance >= 0.f)
            pa.add(m.pressure_advance);
        speed.add(m.feedrate);
        actual_speed.add(m.actual_feedrate);
        accel.add(m.acceleration);
        jerk.add(m.jerk);
    }
    for (const PreviewLayer& l : preview_layers(moves))
        layer_time.add(time_mode < 2 ? l.time_s[time_mode] : 0.);
    nlohmann::json j = nlohmann::json::object();
    auto put = [&j](const char* id, const R& r, const char* unit) {
        if (r.any)
            j[id] = {{"min", r.lo}, {"max", r.hi}, {"unit", unit}};
    };
    put("layerHeight", height, "mm");
    put("lineWidth", width, "mm");
    put("speed", speed, "mm/s");
    put("actualSpeed", actual_speed, "mm/s");
    put("flow", flow, "mm3/s");
    put("actualFlow", actual_flow, "mm3/s");
    put("fanSpeed", fan, "%");
    put("temperature", temp, "C");
    put("pressureAdvance", pa, "");
    put("acceleration", accel, "mm/s2");
    put("jerk", jerk, "mm/s");
    put("layerTime", layer_time, "s");
    return j;
}

// Filament from a volume, as GCodeProcessor's post-process writes it: length from the diameter,
// weight from the density (g/cm3), cost from the cost per kg.
struct FilamentUse { double m = 0., g = 0., cost = 0.; };
inline FilamentUse filament_use(double volume_mm3, double diameter_mm, double density, double cost_per_kg)
{
    FilamentUse u;
    if (diameter_mm > 0.)
        u.m = volume_mm3 / (M_PI * 0.25 * diameter_mm * diameter_mm) * 0.001;
    u.g    = volume_mm3 * 0.001 * density;
    u.cost = u.g * cost_per_kg * 0.001;
    return u;
}

inline double at_or(const std::vector<float>& v, size_t i, double fallback) { return i < v.size() ? double(v[i]) : fallback; }

// One slot's line of the legend's filament table (and the All Plates Stats window): model,
// support, flushed, prime tower and total, each in mm3, m and g, plus the cost of the total.
// `slot0` is 0-based.
inline nlohmann::json filament_slot_json(const PrintEstimatedStatistics& st, size_t slot0, const std::vector<float>& diameters,
                                         const std::vector<float>& densities, const std::vector<float>& costs)
{
    const double dia = at_or(diameters, slot0, 1.75), rho = at_or(densities, slot0, 1.24), cost = at_or(costs, slot0, 0.);
    auto vol = [slot0](const std::map<size_t, double>& m) { auto it = m.find(slot0); return it == m.end() ? 0. : it->second; };
    nlohmann::json f = {{"filament", slot0 + 1}};
    auto put = [&](const char* key, double v) {
        const FilamentUse u = filament_use(v, dia, rho, cost);
        f[key] = {{"mm3", v}, {"m", u.m}, {"g", u.g}};
    };
    put("model", vol(st.model_volumes_per_extruder));
    put("support", vol(st.support_volumes_per_extruder));
    put("flushed", vol(st.flush_per_filament));
    put("primeTower", vol(st.wipe_tower_volumes_per_extruder));
    const double total = vol(st.total_volumes_per_extruder);
    put("total", total);
    f["cost"] = filament_use(total, dia, rho, cost).cost;
    // The flat mm3 fields earlier replies carried, kept for callers that read them.
    f["totalMm3"] = total;
    f["modelMm3"] = vol(st.model_volumes_per_extruder);
    f["supportMm3"] = vol(st.support_volumes_per_extruder);
    f["primeTowerMm3"] = vol(st.wipe_tower_volumes_per_extruder);
    f["flushMm3"] = vol(st.flush_per_filament);
    return f;
}

// The legend's "Filament changes" row time: load + unload + tool-change time (GCodeViewer
// render_legend: tool-change delays are not attributed to ToolChange moves).
inline nlohmann::json filament_change_time_json(const PrintEstimatedStatistics& st)
{
    return {{"loadS", st.total_filament_load_time},
            {"unloadS", st.total_filament_unload_time},
            {"toolChangeS", st.total_tool_change_time},
            {"totalS", double(st.total_filament_load_time) + st.total_filament_unload_time + st.total_tool_change_time},
            {"changes", st.total_filament_changes},
            {"extruderChanges", st.total_extruder_changes}};
}

// The All Plates Stats window: per-slot volumes summed over the plates, then converted with the
// first plate's diameters and densities (as render_all_plates_stats does); time and cost summed.
struct PlateFigures
{
    int plate = 0;
    const PrintEstimatedStatistics* stats = nullptr;
    double time_s = 0.;
    double cost   = 0.;
};
inline nlohmann::json all_plates_json(const std::vector<PlateFigures>& plates, const std::vector<float>& diameters,
                                      const std::vector<float>& densities, const std::vector<float>& costs)
{
    PrintEstimatedStatistics sum;
    double time_s = 0., cost = 0.;
    nlohmann::json per_plate = nlohmann::json::array();
    std::set<size_t> slots;
    for (const PlateFigures& p : plates) {
        if (p.stats == nullptr)
            continue;
        auto add = [&slots](std::map<size_t, double>& dst, const std::map<size_t, double>& src) {
            for (const auto& [k, v] : src) {
                dst[k] += v;
                slots.insert(k);
            }
        };
        add(sum.model_volumes_per_extruder, p.stats->model_volumes_per_extruder);
        add(sum.support_volumes_per_extruder, p.stats->support_volumes_per_extruder);
        add(sum.flush_per_filament, p.stats->flush_per_filament);
        add(sum.wipe_tower_volumes_per_extruder, p.stats->wipe_tower_volumes_per_extruder);
        add(sum.total_volumes_per_extruder, p.stats->total_volumes_per_extruder);
        time_s += p.time_s;
        cost += p.cost;
        per_plate.push_back({{"plate", p.plate}, {"timeS", p.time_s}, {"cost", p.cost}});
    }
    nlohmann::json filaments = nlohmann::json::array();
    for (size_t slot : slots)
        filaments.push_back(filament_slot_json(sum, slot, diameters, densities, costs));
    return {{"plates", per_plate}, {"timeS", time_s}, {"cost", cost}, {"filaments", filaments}};
}

// ---------------------------------------------------------------------------------------------
// G-code lines around a move (preview.getMove), as the G-code window reads them: line ids are
// 1-based, line id lies in [lines_ends[id-2], lines_ends[id-1]) of GCodeProcessorResult::filename.
// ---------------------------------------------------------------------------------------------

constexpr size_t GCODE_LINE_MAX_BYTES = 200;

// [first, last] 1-based line ids around `line_id`, clamped to the file; {0, 0} when there is none.
inline std::array<size_t, 2> gcode_line_span(size_t line_count, size_t line_id, size_t before, size_t after)
{
    if (line_count == 0 || line_id == 0 || line_id > line_count)
        return {0, 0};
    const size_t first = line_id > before ? line_id - before : 1;
    const size_t last  = std::min(line_count, line_id + after);
    return {first, last};
}

// The byte range of a line: {offset, length}.
inline std::array<size_t, 2> gcode_line_bytes(const std::vector<size_t>& lines_ends, size_t line_id)
{
    if (line_id == 0 || line_id > lines_ends.size())
        return {0, 0};
    const size_t start = line_id == 1 ? 0 : lines_ends[line_id - 2];
    const size_t end   = lines_ends[line_id - 1];
    return {start, end > start ? end - start : 0};
}

// A line as shown: the line break dropped, cut at max_bytes on a UTF-8 character boundary.
inline std::string clip_gcode_line(std::string line, size_t max_bytes = GCODE_LINE_MAX_BYTES)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    if (line.size() > max_bytes) {
        size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
            --cut;
        line = line.substr(0, cut) + "...";
    }
    return line;
}

// ---------------------------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------------------------

constexpr unsigned CAPTURE_MAX_SIDE  = 1024;
constexpr unsigned CAPTURE_MIN_SIDE  = 64;
constexpr size_t   CAPTURE_MAX_BYTES = 1536u * 1024u; // 1.5 MB of PNG

// Fits (w, h) inside max_side keeping the aspect ratio, never below min_side on the short side.
inline std::array<unsigned, 2> fit_capture_size(unsigned w, unsigned h, unsigned max_side = CAPTURE_MAX_SIDE)
{
    if (w == 0 || h == 0)
        return {0, 0};
    const double k = std::min(1., double(max_side) / double(std::max(w, h)));
    return {std::max(1u, unsigned(std::lround(w * k))), std::max(1u, unsigned(std::lround(h * k)))};
}

// Box-filter downscale of RGBA8 rows (any row order is kept as is).
inline std::vector<unsigned char> downscale_rgba(const std::vector<unsigned char>& src, unsigned sw, unsigned sh, unsigned dw, unsigned dh)
{
    std::vector<unsigned char> dst(size_t(dw) * dh * 4, 0);
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0 || src.size() < size_t(sw) * sh * 4)
        return dst;
    for (unsigned y = 0; y < dh; ++y) {
        const unsigned y0 = unsigned(uint64_t(y) * sh / dh), y1 = std::max(y0 + 1, unsigned(uint64_t(y + 1) * sh / dh));
        for (unsigned x = 0; x < dw; ++x) {
            const unsigned x0 = unsigned(uint64_t(x) * sw / dw), x1 = std::max(x0 + 1, unsigned(uint64_t(x + 1) * sw / dw));
            uint64_t acc[4] = {0, 0, 0, 0};
            for (unsigned yy = y0; yy < y1 && yy < sh; ++yy)
                for (unsigned xx = x0; xx < x1 && xx < sw; ++xx)
                    for (int c = 0; c < 4; ++c)
                        acc[c] += src[(size_t(yy) * sw + xx) * 4 + c];
            const uint64_t n = uint64_t(std::min(y1, sh) - y0) * (std::min(x1, sw) - x0);
            for (int c = 0; c < 4; ++c)
                dst[(size_t(y) * dw + x) * 4 + c] = (unsigned char) (n ? acc[c] / n : 0);
        }
    }
    return dst;
}

}}}} // namespace Slic3r::GUI::Actions::PlatesPreview

#endif
