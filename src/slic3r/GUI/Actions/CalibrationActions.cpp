// Automation actions: calib.run (the Calibration menu: every test model the menu builds).
// Owner: work package S8 (filament/UI). See docs/HLSD/bridge-actions.md for how to write an action
// and docs/bridge-actions/filament-ui.md for this one.
//
// Each Calibration menu item opens a dialog (calib_dlg.cpp) whose OK button calls a
// Plater::calib_<kind>(Calib_Params). calib.run takes the dialog's fields as params, applies the same
// defaults and checks, and calls the same function. Every one of them replaces the project (a new
// project with the test model, presets adjusted as unsaved edits), so the ifProjectDirty and
// ifDirty contracts run first (decision D13).

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "FilamentUiRules.hpp"
#include "Register.hpp"

#include <algorithm>
#include <cmath>

#include "libslic3r/Flow.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "../GUI_App.hpp"
#include "../Plater.hpp"
#include "../Tab.hpp"
#include "../WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using nlohmann::json;

// ---- pure rules (FilamentUiRules.hpp) --------------------------------------------------------

const std::vector<std::string>& calib_kind_names()
{
    static const std::vector<std::string> names = {"pressureAdvance", "temperature", "maxFlowrate", "flowRatio", "retraction",
                                                   "cornering", "inputShapingFrequency", "inputShapingDamping", "vfa"};
    return names;
}

bool calib_kind_from_name(const std::string& name, CalibKind& out)
{
    const auto& names = calib_kind_names();
    auto        it    = std::find(names.begin(), names.end(), name);
    if (it == names.end())
        return false;
    out = CalibKind(it - names.begin());
    return true;
}

json calib_params_schema()
{
    auto num = [](double lo, double hi, const char* desc) {
        return json{{"type", "number"}, {"minimum", lo}, {"maximum", hi}, {"description", desc}};
    };
    json props = {
        {"kind", {{"type", "string"}, {"enum", calib_kind_names()}, {"description", "Which calibration test to build."}}},
        {"method", {{"type", "string"}, {"enum", {"tower", "line", "pattern"}}, {"default", "tower"},
                    {"description", "pressureAdvance: tower, line or pattern test."}}},
        {"extruderType", {{"type", "string"}, {"enum", {"directDrive", "bowden"}}, {"default", "directDrive"},
                          {"description", "pressureAdvance: picks the default range (bowden goes to 1.0)."}}},
        {"start", num(0, 1000, "Start of the tested range (pressure advance, degC, mm3/s, mm/s, mm, damping or cornering).")},
        {"end", num(0, 1000, "End of the tested range.")},
        {"step", num(0, 100, "Step between test blocks (pressureAdvance, maxFlowrate, vfa, retraction).")},
        {"printNumbers", {{"type", "boolean"}, {"description", "pressureAdvance line: print the value beside each line."}}},
        {"accelerations", {{"type", "array"}, {"minItems", 1}, {"maxItems", 16},
                           {"items", {{"type", "number"}, {"minimum", 1}, {"maximum", 100000}}},
                           {"description", "pressureAdvance pattern: accelerations to test (mm/s2)."}}},
        {"speeds", {{"type", "array"}, {"minItems", 1}, {"maxItems", 16},
                    {"items", {{"type", "number"}, {"minimum", 1}, {"maximum", 2000}}},
                    {"description", "pressureAdvance pattern: speeds to test (mm/s)."}}},
        {"pass", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2}, {"default", 1},
                  {"description", "flowRatio: pass 1, or pass 2 (the finer test / the perfectionist version)."}}},
        {"flowMethod", {{"type", "string"}, {"enum", {"yolo", "classic"}}, {"default", "yolo"},
                        {"description", "flowRatio: the linear (YOLO) test or the classic two-pass test."}}},
        {"flowPattern", {{"type", "string"}, {"enum", {"archimedeanChords", "monotonic"}}, {"default", "archimedeanChords"},
                         {"description", "flowRatio: top surface pattern."}}},
        {"freqStartX", num(0, 500, "inputShapingFrequency: X start frequency (Hz).")},
        {"freqEndX", num(0, 500, "inputShapingFrequency: X end frequency (Hz).")},
        {"freqStartY", num(0, 500, "inputShapingFrequency: Y start frequency (Hz); RepRapFirmware uses X.")},
        {"freqEndY", num(0, 500, "inputShapingFrequency: Y end frequency (Hz).")},
        {"freqX", num(0, 500, "inputShapingDamping: X frequency (Hz).")},
        {"freqY", num(0, 500, "inputShapingDamping: Y frequency (Hz).")},
        {"damping", num(0, 0.999, "inputShapingFrequency: damping factor used while sweeping the frequency.")},
        {"shaperType", {{"type", "string"}, {"minLength", 1}, {"maxLength", 32},
                        {"description", "Input shaping: shaper type the printer's firmware offers (default: the first)."}}},
        {"testModel", {{"type", "integer"}, {"minimum", 0}, {"maximum", 2},
                       {"description", "inputShaping*: 0 ringing tower, 1 fast tower. cornering: the dialog's model choice."}}},
        {"nozzleBasedResize", {{"type", "boolean"}, {"default", true},
                               {"description", "temperature, vfa: scale the model to the nozzle diameter."}}},
        {"autoAdjust", {{"type", "boolean"}, {"default", true},
                        {"description", "vfa: when the end speed exceeds the filament's max volumetric speed, lower the layer height to a "
                                        "standard one that reaches it (resizing only), else also lower the end speed. The result's "
                                        "used.vfaAdjust says what happened."}}},
    };
    return {{"type", "object"}, {"additionalProperties", false}, {"required", json::array({"kind"})},{"properties", props}};
}

namespace {

double num_or(const json& p, const char* key, double fallback)
{
    auto it = p.find(key);
    return (it != p.end() && it->is_number()) ? it->get<double>() : fallback;
}

bool fail(std::string& path, std::string& message, const char* key, const std::string& why)
{
    path    = std::string("/params/") + key;
    message = why;
    return false;
}

} // namespace

double calib_snap_temperature(double celsius) { return std::floor(std::round(celsius) / 5.0) * 5.0; }

const char* vfa_outcome_name(VfaAdjustment::Outcome outcome)
{
    switch (outcome) {
    case VfaAdjustment::Outcome::Fits: return "fits";
    case VfaAdjustment::Outcome::LayerHeightLowered: return "layerHeightLowered";
    case VfaAdjustment::Outcome::EndLowered: return "endLowered";
    case VfaAdjustment::Outcome::Clamped: return "clamped";
    }
    return "fits";
}

VfaAdjustment vfa_auto_adjust(double start, double end, double step, bool nozzle_based_resize, bool auto_adjust, const VfaLimits& l)
{
    VfaAdjustment out;
    out.end = end;
    if (l.max_volumetric_speed <= 0. || l.nozzle_diameter <= 0.)
        return out;
    const double line_width = l.line_width > 0. ? l.line_width : l.nozzle_diameter;
    // The outer wall's speed limit at a layer height (VFA_Test_Dlg::on_start speed_limit_for_lh).
    auto speed_limit = [&](double lh) {
        double mm3_per_mm = 0.;
        try {
            mm3_per_mm = Flow(float(line_width), float(lh), float(l.nozzle_diameter)).mm3_per_mm();
        } catch (const std::exception&) {
        }
        return mm3_per_mm > 0. ? l.max_volumetric_speed / mm3_per_mm : 1e9;
    };
    const double default_lh = nozzle_based_resize ? l.nozzle_diameter / 2.0 : (l.preset_layer_height > 0. ? l.preset_layer_height : l.nozzle_diameter / 2.0);
    out.speed_limit         = speed_limit(default_lh);
    if (end <= out.speed_limit)
        return out;
    if (!auto_adjust || !nozzle_based_resize) {
        out.outcome = VfaAdjustment::Outcome::Clamped;
        return out;
    }
    std::vector<double> candidates = l.candidate_layer_heights;
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }),
                     candidates.end());
    // The largest standard height at or below the default that still reaches the end speed.
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (*it > default_lh + 1e-6)
            continue;
        if (speed_limit(*it) >= end) {
            out.outcome      = VfaAdjustment::Outcome::LayerHeightLowered;
            out.layer_height = *it;
            return out;
        }
    }
    if (candidates.empty()) {
        out.outcome = VfaAdjustment::Outcome::Clamped;
        return out;
    }
    // Even the smallest height cannot: use it, and lower the end to a step multiple it reaches.
    const double min_lh  = candidates.front();
    double       new_end = step > 0. ? std::floor(speed_limit(min_lh) / step) * step : speed_limit(min_lh);
    if (new_end < start + step)
        new_end = start + step;
    out.outcome      = VfaAdjustment::Outcome::EndLowered;
    out.layer_height = min_lh;
    out.end          = new_end;
    return out;
}

bool calib_request_from_params(const json& p, const CalibMachine& m, CalibRequest& out, std::string& path, std::string& message)
{
    out = CalibRequest{};
    if (!p.contains("kind") || !p["kind"].is_string() || !calib_kind_from_name(p["kind"].get<std::string>(), out.kind))
        return fail(path, message, "kind", "kind must be one of the calibration kinds");
    Calib_Params& c = out.params;

    switch (out.kind) {
    case CalibKind::PressureAdvance: {
        const std::string method = p.value("method", std::string("tower"));
        const bool        bowden = p.value("extruderType", std::string("directDrive")) == "bowden";
        c.mode                   = method == "line" ? CalibMode::Calib_PA_Line : method == "pattern" ? CalibMode::Calib_PA_Pattern : CalibMode::Calib_PA_Tower;
        // PA_Calibration_Dlg::reset_params
        double end = method == "pattern" ? 0.08 : 0.1, step = method == "pattern" ? 0.005 : 0.002;
        if (bowden) {
            end  = 1.0;
            step = method == "pattern" ? 0.05 : 0.02;
        }
        c.start         = num_or(p, "start", 0.0);
        c.end           = num_or(p, "end", end);
        c.step          = num_or(p, "step", step);
        c.print_numbers = method == "tower" ? false : method == "pattern" ? true : p.value("printNumbers", true);
        if (c.start < 0)
            return fail(path, message, "start", "start pressure advance must be >= 0");
        if (c.step < 10 * EPSILON)
            return fail(path, message, "step", "pressure advance step must be >= 0.001");
        if (c.end < c.start + c.step)
            return fail(path, message, "end", "end must be at least start + step");
        if (method == "pattern") {
            if (p.contains("accelerations"))
                c.accelerations = p["accelerations"].get<std::vector<double>>();
            if (p.contains("speeds"))
                c.speeds = p["speeds"].get<std::vector<double>>();
            if (!c.accelerations.empty() && !c.speeds.empty() &&
                *std::min_element(c.accelerations.begin(), c.accelerations.end()) <= *std::max_element(c.speeds.begin(), c.speeds.end()))
                return fail(path, message, "accelerations", "acceleration values must be greater than speed values (swapped inputs?)");
        } else if (p.contains("accelerations") || p.contains("speeds")) {
            return fail(path, message, p.contains("accelerations") ? "accelerations" : "speeds", "accelerations and speeds apply to the pattern method only");
        }
        break;
    }
    case CalibKind::Temperature: {
        c.mode                = CalibMode::Calib_Temp_Tower;
        // The dialog's fields snap to multiples of 5 degC (the tower's blocks are 5 degC apart).
        c.start               = calib_snap_temperature(num_or(p, "start", 230));
        c.end                 = calib_snap_temperature(num_or(p, "end", 190));
        c.nozzle_based_resize = p.value("nozzleBasedResize", true);
        // Temp_Calibration_Dlg::on_start: the tower goes from the hot start down to the end.
        if (c.start > 500)
            return fail(path, message, "start", "start temperature must be <= 500");
        if (c.end < 155)
            return fail(path, message, "end", "end temperature must be >= 155");
        if (c.end > c.start - 5)
            return fail(path, message, "end", "start temperature must be at least end + 5 (the tower cools going up)");
        break;
    }
    case CalibKind::MaxFlowrate: {
        c.mode  = CalibMode::Calib_Vol_speed_Tower;
        c.start = num_or(p, "start", 5);
        c.end   = num_or(p, "end", 20);
        c.step  = num_or(p, "step", 0.5);
        if (c.start <= 0)
            return fail(path, message, "start", "start volumetric speed must be > 0");
        if (c.step <= 0)
            return fail(path, message, "step", "step must be > 0");
        if (c.end < c.start + c.step)
            return fail(path, message, "end", "end must be at least start + step");
        break;
    }
    case CalibKind::Vfa: {
        c.mode                = CalibMode::Calib_VFA_Tower;
        c.start               = num_or(p, "start", 40);
        c.end                 = num_or(p, "end", 200);
        c.step                = num_or(p, "step", 10);
        c.nozzle_based_resize = p.value("nozzleBasedResize", true);
        c.vfa_layer_height    = 0.0; // auto (nozzle / 2)
        if (c.start <= 10)
            return fail(path, message, "start", "start speed must be > 10 mm/s");
        if (c.step <= 0)
            return fail(path, message, "step", "step must be > 0");
        if (c.end < c.start + c.step)
            return fail(path, message, "end", "end must be at least start + step");
        break;
    }
    case CalibKind::Retraction: {
        c.mode  = CalibMode::Calib_Retraction_tower;
        c.start = num_or(p, "start", 0);
        c.end   = num_or(p, "end", 2);
        c.step  = num_or(p, "step", 0.1);
        if (c.start < 0)
            return fail(path, message, "start", "start retraction length must be >= 0");
        if (c.step <= 0)
            return fail(path, message, "step", "step must be > 0");
        if (c.end < c.start + c.step)
            return fail(path, message, "end", "end must be at least start + step");
        break;
    }
    case CalibKind::Cornering: {
        c.mode             = CalibMode::Calib_Cornering;
        const double max_v = m.junction_deviation ? 0.3 : 100.0;
        c.start            = num_or(p, "start", m.junction_deviation ? 0.0 : 1.0);
        c.end              = num_or(p, "end", m.junction_deviation ? 0.25 : 15.0);
        c.test_model       = p.value("testModel", 0);
        if (c.start < 0)
            return fail(path, message, "start", "start must be >= 0");
        if (c.end > max_v)
            return fail(path, message, "end", std::string("end must be <= ") + (m.junction_deviation ? "0.3 (junction deviation, mm)" : "100"));
        if (c.start >= c.end)
            return fail(path, message, "end", "end must be greater than start");
        break;
    }
    case CalibKind::InputShapingFrequency:
    case CalibKind::InputShapingDamping: {
        const bool freq = out.kind == CalibKind::InputShapingFrequency;
        c.mode          = freq ? CalibMode::Calib_Input_shaping_freq : CalibMode::Calib_Input_shaping_damp;
        c.test_model    = p.value("testModel", 0);
        if (c.test_model > 1)
            return fail(path, message, "testModel", "testModel is 0 (ringing tower) or 1 (fast tower)");
        if (freq) {
            c.freqStartX = num_or(p, "freqStartX", 15);
            c.freqEndX   = num_or(p, "freqEndX", 110);
            c.freqStartY = m.reprap_firmware ? c.freqStartX : num_or(p, "freqStartY", 15);
            c.freqEndY   = m.reprap_firmware ? c.freqEndX : num_or(p, "freqEndY", 110);
            c.start      = num_or(p, "damping", 0.15);
            if (c.freqStartX >= c.freqEndX)
                return fail(path, message, "freqEndX", "the X end frequency must be above the start");
            if (c.freqStartY >= c.freqEndY)
                return fail(path, message, "freqEndY", "the Y end frequency must be above the start");
            if (c.start < 0 || c.start >= 1)
                return fail(path, message, "damping", "damping must be in [0, 1)");
        } else {
            c.freqStartX = num_or(p, "freqX", 30);
            c.freqStartY = m.reprap_firmware ? c.freqStartX : num_or(p, "freqY", 30);
            c.start      = num_or(p, "start", 0.0);
            c.end        = num_or(p, "end", 0.4);
            if (c.start < 0 || c.end > 1 || c.start >= c.end)
                return fail(path, message, "end", "the damping range must satisfy 0 <= start < end <= 1");
        }
        if (!m.shaper_types.empty()) {
            c.shaper_type = p.value("shaperType", m.shaper_types.front());
            if (std::find(m.shaper_types.begin(), m.shaper_types.end(), c.shaper_type) == m.shaper_types.end())
                return fail(path, message, "shaperType", "this printer's firmware offers no shaper type \"" + c.shaper_type + "\"");
        } else {
            c.shaper_type = p.value("shaperType", std::string());
        }
        break;
    }
    case CalibKind::FlowRatio: {
        c.mode             = CalibMode::Calib_Flow_Rate;
        out.flow_linear    = p.value("flowMethod", std::string("yolo")) == "yolo";
        out.flow_pass      = p.value("pass", 1);
        out.flow_monotonic = p.value("flowPattern", std::string("archimedeanChords")) == "monotonic";
        if (out.flow_pass != 1 && out.flow_pass != 2)
            return fail(path, message, "pass", "pass is 1 or 2");
        break;
    }
    }
    return true;
}

namespace {

CalibMachine machine_of_current_printer()
{
    CalibMachine m;
    if (wxGetApp().preset_bundle == nullptr)
        return m;
    const DynamicPrintConfig& printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    if (const auto* flavor = printer.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")) {
        m.reprap_firmware = flavor->value == gcfRepRapFirmware;
        if (flavor->value == gcfMarlinFirmware)
            if (const auto* jd = printer.option<ConfigOptionFloats>("machine_max_junction_deviation"); jd && !jd->values.empty())
                m.junction_deviation = jd->values[0] > 0;
        // calib_dlg.cpp get_shaper_type_values(): the types the firmware offers, by config name.
        const ConfigOptionDef* def = printer.def()->get("input_shaping_type");
        for (InputShaperType type : input_shaper_types_for_flavor(flavor->value)) {
            if (type == InputShaperType::Disable)
                continue;
            const size_t idx = static_cast<size_t>(type);
            if (def && idx < def->enum_values.size())
                m.shaper_types.push_back(def->enum_values[idx]);
        }
        if (m.shaper_types.empty() && def && !def->enum_values.empty())
            m.shaper_types.push_back(def->enum_values.front());
    }
    return m;
}

// What VFA_Test_Dlg::on_start reads from the edited presets for its auto-adjust.
VfaLimits vfa_limits_of_current_presets(int extruder_id)
{
    VfaLimits     l;
    PresetBundle& b               = *wxGetApp().preset_bundle;
    const auto&   printer_config  = b.printers.get_edited_preset().config;
    const auto&   print_config    = b.prints.get_edited_preset().config;
    const auto&   filament_config = b.filaments.get_edited_preset().config;
    const size_t  idx             = size_t(std::max(extruder_id, 0));
    auto          get_at          = [idx](const ConfigOptionFloats* opt, double fallback) {
        return opt == nullptr || opt->values.empty() ? fallback : opt->values[std::min(idx, opt->values.size() - 1)];
    };
    l.nozzle_diameter     = get_at(printer_config.option<ConfigOptionFloats>("nozzle_diameter"), vfa_base_nozzle_diameter);
    l.preset_layer_height = l.nozzle_diameter / 2.0;
    if (const auto* lh = print_config.option<ConfigOptionFloat>("layer_height"); lh != nullptr && lh->value > 0.)
        l.preset_layer_height = lh->value;
    l.max_volumetric_speed = get_at(filament_config.option<ConfigOptionFloats>("filament_max_volumetric_speed"), 0.);
    l.line_width           = print_config.get_abs_value("outer_wall_line_width", l.nozzle_diameter);
    if (l.line_width <= 0.)
        l.line_width = print_config.get_abs_value("line_width", l.nozzle_diameter);
    if (l.line_width <= 0.)
        l.line_width = l.nozzle_diameter;
    const double min_lh = get_at(printer_config.option<ConfigOptionFloats>("min_layer_height"), 0.);
    const double max_lh = get_at(printer_config.option<ConfigOptionFloats>("max_layer_height"), 0.);
    for (const Preset& preset : b.prints.get_presets()) {
        if (!preset.is_compatible || preset.is_default)
            continue;
        const auto* lh = preset.config.option<ConfigOptionFloat>("layer_height");
        if (lh == nullptr || lh->value <= 0. || (min_lh > 0. && lh->value < min_lh - 1e-6) || (max_lh > 0. && lh->value > max_lh + 1e-6))
            continue;
        l.candidate_layer_heights.push_back(lh->value);
    }
    return l;
}

const char* PRESET_TYPES_NOTE = "the calibration replaces the project and adjusts the print, filament and printer presets";

InvokeResult run_calib(InvokeContext& ctx)
{
    Plater* plater = ctx.plater();
    if (plater == nullptr || wxGetApp().preset_bundle == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the plater is not available");

    CalibRequest req;
    std::string  path, message;
    // Checked first, so a bad value is reported before anything is asked or saved; checked again
    // below against the printer as the preflights leave it (a discarded edit may change it).
    if (!calib_request_from_params(ctx.params, machine_of_current_printer(), req, path, message))
        return InvokeResult::error(WAB_BAD_PARAMS, message, {{"path", path}});

    // The project is replaced: ifProjectDirty first (ask -> 1013 projectDirty, save, discard, prompt).
    InvokeResult refusal;
    if (!ctx.project_dirty_preflight(refusal))
        return refusal;
    // A new project keeps or drops unsaved preset edits through the slicer's dialog; ifDirty
    // answers it instead (ask -> 1013 with the dirty keys, discard, prompt).
    PresetBundle& b = *wxGetApp().preset_bundle;
    for (Preset::Type type : {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER}) {
        PresetCollection* presets = type == Preset::TYPE_PRINT ? static_cast<PresetCollection*>(&b.prints) :
                                    type == Preset::TYPE_FILAMENT ? static_cast<PresetCollection*>(&b.filaments) :
                                                                    static_cast<PresetCollection*>(&b.printers);
        if (presets->current_is_dirty() && !ctx.preset_dirty_preflight(type, refusal))
            return refusal;
    }

    if (!calib_request_from_params(ctx.params, machine_of_current_printer(), req, path, message))
        return InvokeResult::error(WAB_BAD_PARAMS, message, {{"path", path}});

    // VFA's "Auto-adjust to max volumetric speed", with the presets as they now are.
    VfaAdjustment vfa;
    if (req.kind == CalibKind::Vfa) {
        vfa = vfa_auto_adjust(req.params.start, req.params.end, req.params.step, req.params.nozzle_based_resize,
                              ctx.params.value("autoAdjust", true), vfa_limits_of_current_presets(req.params.extruder_id));
        req.params.vfa_layer_height = vfa.layer_height;
        req.params.end              = vfa.end;
    }

    // Each question is answered by its own contract: ifProjectDirty for the project, ifDirty for
    // the preset edits. "prompt" in one never reopens the question the other already settled.
    const bool prompt_project = ctx.if_project_dirty == "prompt" && plater->is_project_dirty();
    const bool prompt_presets = ctx.if_dirty == "prompt";

    // The person sees what is replaced (D2), unless the plate is empty (nothing to lose) or the
    // slicer's own "save the project?" question is about to be asked.
    if (!ctx.model().objects.empty() && !prompt_project) {
        ConfirmRequest confirm;
        confirm.title        = "Calibration";
        confirm.message      = "Replace the current project with a " + calib_kind_names()[size_t(req.kind)] +
                          " calibration test? The plate is cleared and the print, filament and printer settings are adjusted for the test "
                          "as unsaved changes.";
        confirm.accept_label = "Build test";
        if (!ctx.confirm(confirm))
            return InvokeResult::error(WAB_USER_CANCELLED, "the person kept the current project");
    }

    // Start the new project here, so a cancelled prompt stops before anything is built (the
    // Plater::calib_* functions carry on after a cancelled new_project). Only the questions left
    // to "prompt" are asked; the preflights above settled the others.
    if (!prompt_project) {
        // Saved or discarded by the preflight (or clean): the project question must not come up.
        plater->up_to_date(true, false);
        plater->up_to_date(true, true);
    }
    if (plater->new_project(!(prompt_project || prompt_presets), true) == wxID_CANCEL)
        return InvokeResult::error(WAB_USER_CANCELLED, "the person kept the current project");

    // Every Plater::calib_* starts a new project of its own: the questions are settled, so it
    // must not ask them a second time (a cancel there leaves an empty project behind).
    plater->skip_next_new_project_confirm();
    const Calib_Params& c = req.params;
    switch (req.kind) {
    case CalibKind::PressureAdvance: plater->calib_pa(c); break;
    case CalibKind::Temperature: plater->calib_temp(c); break;
    case CalibKind::MaxFlowrate: plater->calib_max_vol_speed(c); break;
    case CalibKind::FlowRatio: plater->calib_flowrate(req.flow_linear, req.flow_pass, req.flow_monotonic ? ipMonotonic : ipArchimedeanChords); break;
    case CalibKind::Retraction: plater->calib_retraction(c); break;
    case CalibKind::Cornering: plater->Calib_Cornering(c); break;
    case CalibKind::InputShapingFrequency: plater->calib_input_shaping_freq(c); break;
    case CalibKind::InputShapingDamping: plater->calib_input_shaping_damp(c); break;
    case CalibKind::Vfa: plater->calib_VFA(c); break;
    }
    plater->skip_next_new_project_confirm(false);
    if (ctx.model().objects.empty())
        return InvokeResult::error(WAB_IO, "the calibration test was not built (its model could not be loaded); the project is now empty",
                                   {{"kind", calib_kind_names()[size_t(req.kind)]}});

    json used = {{"kind", calib_kind_names()[size_t(req.kind)]}};
    if (req.kind == CalibKind::FlowRatio) {
        used["flowMethod"]  = req.flow_linear ? "yolo" : "classic";
        used["pass"]        = req.flow_pass;
        used["flowPattern"] = req.flow_monotonic ? "monotonic" : "archimedeanChords";
    } else if (req.kind == CalibKind::InputShapingFrequency) {
        used.update({{"freqStartX", c.freqStartX}, {"freqEndX", c.freqEndX}, {"freqStartY", c.freqStartY}, {"freqEndY", c.freqEndY},
                     {"damping", c.start}, {"shaperType", c.shaper_type}, {"testModel", c.test_model}});
    } else if (req.kind == CalibKind::InputShapingDamping) {
        used.update({{"freqX", c.freqStartX}, {"freqY", c.freqStartY}, {"start", c.start}, {"end", c.end},
                     {"shaperType", c.shaper_type}, {"testModel", c.test_model}});
    } else {
        used.update({{"start", c.start}, {"end", c.end}});
        if (req.kind != CalibKind::Temperature && req.kind != CalibKind::Cornering)
            used["step"] = c.step;
        if (req.kind == CalibKind::Vfa) {
            json adjust = {{"outcome", vfa_outcome_name(vfa.outcome)}};
            if (vfa.speed_limit > 0.)
                adjust["speedLimitMmS"] = vfa.speed_limit;
            if (vfa.layer_height > 0.)
                adjust["layerHeight"] = vfa.layer_height;
            used["vfaAdjust"] = adjust;
        }
    }
    return InvokeResult::ok({{"project", plater->get_project_name().ToUTF8().data()},
                             {"objects", ctx.model().objects.size()},
                             {"used", used},
                             {"note", PRESET_TYPES_NOTE}});
}

} // namespace

void register_calibration_actions(Catalog& c)
{
    ActionDescriptor d;
    d.id      = "calib.run";
    d.label   = "Calibration";
    d.summary = "Build a calibration test as a new project, as the Calibration menu does: pressure advance, temperature, max flowrate, "
                "flow ratio, retraction, cornering, input shaping frequency/damping or VFA. Replaces the project (honours ifProjectDirty "
                "and ifDirty); then slice and print it. Temperatures snap down to multiples of 5 degC, as the dialog's fields do.";
    d.risk    = Risk::Destructive;
    d.confirm = Confirm::Native;
    d.confirm_skip_rule =
        "no dialog when the plate is empty (nothing is replaced), or when ifProjectDirty \"prompt\" lets the slicer ask "
        "its own save question about a dirty project; unsaved projects and preset edits additionally need ifProjectDirty / "
        "ifDirty (decision D13)";
    d.params  = calib_params_schema();
    d.effects = {"project", "structure", "geometry", "settings", "presets", "plates"};
    d.ui      = {"MainFrame.cpp::menu::Pressure advance", "MainFrame.cpp::menu::Temperature", "MainFrame.cpp::menu::Max flowrate",
                 "MainFrame.cpp::menu::Flow ratio", "MainFrame.cpp::menu::Retraction", "MainFrame.cpp::menu::Cornering",
                 "MainFrame.cpp::menu::Input Shaping Frequency", "MainFrame.cpp::menu::Input Shaping Damping/zeta factor",
                 "MainFrame.cpp::menu::VFA"};
    d.examples = json::array({json{{"target", nullptr}, {"params", {{"kind", "pressureAdvance"}, {"method", "tower"}, {"start", 0}, {"end", 0.1}, {"step", 0.005}}}},
                              json{{"target", nullptr}, {"params", {{"kind", "temperature"}, {"start", 230}, {"end", 190}}}},
                              json{{"target", nullptr}, {"params", {{"kind", "flowRatio"}, {"flowMethod", "yolo"}, {"pass", 1}}}}});
    c.add(std::move(d), run_calib);
}

}}} // namespace Slic3r::GUI::Actions
