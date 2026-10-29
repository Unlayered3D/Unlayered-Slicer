#ifndef slic3r_GUI_Actions_FilamentUiRules_hpp_
#define slic3r_GUI_Actions_FilamentUiRules_hpp_

// The pure (GUI-free) rules behind the filament, calibration, view and preference actions
// (FilamentActions.cpp, CalibrationActions.cpp, ViewActions.cpp, PreferenceActions.cpp), kept apart
// so tests/slic3rutils/test_actions_filament.cpp can check them without a running application:
//   - calib.run: the parameters of each calibration kind, with the same checks and defaults as the
//     Calibration menu's dialogs (calib_dlg.cpp), turned into the Calib_Params Plater::calib_* takes;
//   - prefs.get / prefs.set: the Preferences allowlist (decision D4) and the conversion between the
//     wire values and what AppConfig stores;
//   - filament.*: colour parsing, the flushing-volume matrix layout, the nozzle diameter string.
// Header only for declarations; each definition lives in the action file that uses it.

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/calib.hpp"

namespace Slic3r {
struct BedMesh;
namespace GUI { namespace Actions {

// ---- calib.run ------------------------------------------------------------------------------

enum class CalibKind
{
    PressureAdvance,
    Temperature,
    MaxFlowrate,
    FlowRatio,
    Retraction,
    Cornering,
    InputShapingFrequency,
    InputShapingDamping,
    Vfa,
};

// Wire names, in CalibKind order: "pressureAdvance", "temperature", "maxFlowrate", "flowRatio",
// "retraction", "cornering", "inputShapingFrequency", "inputShapingDamping", "vfa".
const std::vector<std::string>& calib_kind_names();
bool                            calib_kind_from_name(const std::string& name, CalibKind& out);

// What the printer preset says that changes the dialogs' rules.
struct CalibMachine
{
    bool reprap_firmware    = false; // RRF: input shaping takes one frequency range (Y = X)
    bool junction_deviation = false; // Marlin with machine_max_junction_deviation > 0: cornering in mm (max 0.3)
    std::vector<std::string> shaper_types; // allowed input_shaping_type values; empty = any
};

struct CalibRequest
{
    CalibKind    kind = CalibKind::PressureAdvance;
    Calib_Params params;              // what Plater::calib_<kind> takes (mode set)
    // Flow ratio only (Plater::calib_flowrate takes these instead of Calib_Params).
    bool flow_linear    = true;  // the "YOLO" linear test (recommended) vs the classic two-pass test
    int  flow_pass      = 1;     // 1 or 2 (YOLO pass 2 = the "perfectionist" version)
    bool flow_monotonic = false; // top-surface pattern: monotonic instead of Archimedean chords
};

// The params schema of calib.run (restricted dialect): {kind, method, start, end, step, ...}.
nlohmann::json calib_params_schema();

// Fills `out` from validated calib.run params: per-kind defaults (the dialogs' initial values),
// then the dialogs' range checks. False with `path` ("/params/end") and `message` on a value the
// dialog would refuse. Pure.
bool calib_request_from_params(const nlohmann::json& params, const CalibMachine& machine, CalibRequest& out,
                               std::string& path, std::string& message);

// The calib.run temperature tower's range, as the Temperature dialog's fields leave it: rounded to
// whole degrees, then snapped down to a multiple of 5 (the tower's 5 degC blocks are labelled by it).
double calib_snap_temperature(double celsius);

// VFA's "Auto-adjust to max volumetric speed" (VFA_Test_Dlg::on_start). What the filament and
// printer allow the tower's outer wall.
struct VfaLimits
{
    double nozzle_diameter      = 0.4;
    double preset_layer_height  = 0.2; // the process preset's layer_height (used when not resizing)
    double max_volumetric_speed = 0.;  // filament_max_volumetric_speed; <= 0 = no limit
    double line_width           = 0.4; // outer wall line width (mm)
    // Layer heights of the process presets compatible with the printer, within its min/max layer
    // height: the only heights the adjustment picks from.
    std::vector<double> candidate_layer_heights;
};
struct VfaAdjustment
{
    enum class Outcome
    {
        Fits,               // the end speed is reachable as it is: nothing changes
        LayerHeightLowered, // a smaller standard layer height reaches the end speed
        EndLowered,         // even the smallest cannot: that layer height, and a lower end speed
        Clamped,            // auto-adjust off (or impossible): the top blocks print at the limit speed
    };
    Outcome outcome      = Outcome::Fits;
    double  layer_height = 0.;  // Calib_Params::vfa_layer_height (0 = auto, nozzle / 2)
    double  end          = 0.;  // the end speed to build with
    double  speed_limit  = 0.;  // the outer wall's limit at the default layer height (mm/s); 0 = none
};
// The dialog's rule. The layer height only changes when resizing to the nozzle (it is what decides
// the default height); where the dialog would ask "continue?", the adjustment is applied and the
// outcome says what happened. Pure.
VfaAdjustment vfa_auto_adjust(double start, double end, double step, bool nozzle_based_resize, bool auto_adjust,
                              const VfaLimits& limits);
const char*   vfa_outcome_name(VfaAdjustment::Outcome outcome);

// ---- prefs.get / prefs.set ------------------------------------------------------------------

struct PrefSpec
{
    std::string              name;       // wire name, lowerCamel ("colorScheme")
    std::string              config_key; // AppConfig key in [app] ("color_scheme")
    std::string              type;       // "boolean" | "enum" | "integer" | "number"
    std::vector<std::string> values;     // enum: wire values
    std::vector<std::string> stored;     // enum: what AppConfig stores for each wire value
    int                      minimum = 0, maximum = 0; // integer
    double                   min_number = 0., max_number = 0.; // number
    int                      decimals = 3;                     // number: digits stored after the point
    std::string              default_stored; // what an absent key means
    std::string              label;          // the Preferences row it is ("Colour scheme")
    std::string              description;
    // False for a preference that is not a row of the Preferences dialog (the settings mode
    // switch lives in the settings tabs): it names no Preferences.cpp coverage key.
    bool                     preferences_row = true;
    // Non-empty: prefs.get reads it, but prefs.set does not write it; this action does (a
    // preference that lifts a safety check is changed only behind the person's confirmation).
    std::string              set_by;
};

// The allowlist (decision D4): cosmetic and behaviour preferences an automated caller may read and
// change. Everything else in Preferences stays untouchable (language, single instance, file
// associations, login region, stealth mode, privacy, update channel, sync, tokens, plug-ins,
// developer/debug, local web apps, SSL, hosts, "clear my choice").
const std::vector<PrefSpec>& pref_allowlist();
const PrefSpec*              find_pref(const std::string& name);

// Wire value -> stored string. False with `message` when the value does not fit the spec.
bool pref_value_to_stored(const PrefSpec& spec, const nlohmann::json& value, std::string& stored, std::string& message);
// Stored string -> wire value (unknown stored values map to the default's wire value).
nlohmann::json pref_value_from_stored(const PrefSpec& spec, const std::string& stored);

// prefs.set params: {"values": {<name>: <typed value>, ...}} with one property per allowlisted
// preference (except those with set_by) and additionalProperties:false, so anything off the list
// fails validation by path.
nlohmann::json prefs_set_schema();
// prefs.get params: {"names": [<enum of allowlisted names>]} (absent = all).
nlohmann::json prefs_get_schema();

// ---- filament.* -----------------------------------------------------------------------------

// "#RGB", "#RRGGBB", "RRGGBB" (any case) -> "#RRGGBB" upper case. False otherwise.
bool normalize_hex_colour(const std::string& in, std::string& out);

// 0.4 -> "0.4", 0.25 -> "0.25", 1 -> "1.0": the nozzle combo's spelling (Plater.cpp get_diameter_string).
std::string nozzle_diameter_string(double diameter);

// flush_volumes_matrix holds `nozzles` blocks of full_n x full_n (row = from slot, column = to slot,
// config slot indices). Writes the p x p matrix over the physical slots `physical` (p = its size)
// into block `nozzle` of `raw`, leaving mixed slots' cells and other nozzles untouched. False with
// `message` when the shapes do not fit or a value is negative.
bool apply_flush_sub_matrix(std::vector<double>& raw, size_t nozzles, size_t nozzle, size_t full_n,
                            const std::vector<size_t>& physical, const std::vector<std::vector<double>>& sub,
                            std::string& message);
// The p x p physical sub-matrix of block `nozzle` (empty when raw is too short).
std::vector<std::vector<double>> flush_sub_matrix(const std::vector<double>& raw, size_t nozzles, size_t nozzle, size_t full_n,
                                                  const std::vector<size_t>& physical);

// ---- printer.bedMesh / printer.fetchBedMesh ---------------------------------------------------

// The mesh the bed mesh overlay holds, as printer.bedMesh reports it: {loaded:false} when there is
// none, else host, profile, source, note, compensating (BedMesh::loaded), min/max corners [x, y] in
// printer coordinates, rows/cols/probedPoints, zMin/zMax/zRange in mm and outOfFlat; with `grid`,
// also "probed" (rows along Y from min.y, columns along X from min.x; unprobed nodes null).
nlohmann::json bed_mesh_json(const BedMesh& mesh, const std::string& host, bool grid);

// filament.editMixed: the percentages the mixed filament dialog starts from for a stored
// filament_mixed_sublayer_ratios entry ("0.7000,0.3000" -> {70, 30}); an entry that does not hold
// `components` numbers becomes equal shares, the remainder on the first (Sidebar::edit_mixed_filament).
std::vector<int> mixed_ratio_percents(const std::string& stored, size_t components);

// filament.decomposeColour: the Decompose Color dialog's choices, without the dialog
// (ColorDecomposeDialog.cpp). physical_types are the project's physical filaments' types in order,
// as filament_type_for_color_decompose names them ("PLA", "PLA Basic", "PETG" ...); source is the
// 0-based physical filament being decomposed, or -1.
//   - "PLA" and "PLA Basic" count as the same material;
//   - the material selector offers each project type once, then "PLA Basic";
//   - it starts on the source's type, else on the first offered;
//   - the modes offered for a type: "materialList" with at least two other project filaments of
//     that type, "cmyw" and "rybw" for PLA Basic; listed in the dialog's order of preference.
bool                     decompose_type_matches(const std::string& a, const std::string& b);
std::vector<std::string> decompose_offered_types(const std::vector<std::string>& physical_types);
std::string              decompose_default_type(const std::vector<std::string>& physical_types, int source);
std::vector<std::string> decompose_offered_modes(const std::vector<std::string>& physical_types, int source, const std::string& type);

// filament.delete's replacement mapping, as Sidebar::delete_filament passes it on: the slot that
// takes over `slot`'s objects, re-indexed for after the delete (-1 = the default of each object).
int replacement_after_delete(int slot, int replace_with);

// filament.removeSlots: which slots go, in the order the runner removes them -- highest first, so
// every index still names the slot the caller meant when its turn comes (the slots above it are
// already gone, the ones below have not moved). `is_mixed` has one entry per slot (0-based config
// indices). `listed` are the slots to remove, or, with `listed_are_kept`, the slots to keep (every
// other one goes). `replace_with` is -1 or a slot that stays. On failure `order` is empty and
// `error` / `path` say why (1002): an index out of range, one listed twice, nothing to remove,
// every slot, every physical slot, or a replacement that is itself removed.
struct SlotRemovalPlan
{
    std::vector<int> order; // descending
    std::string      error;
    std::string      path;  // "/params/slots/2", "/params/keep", "/params/replaceWith"
};
SlotRemovalPlan plan_slot_removal(const std::vector<bool>& is_mixed, const std::vector<int>& listed, bool listed_are_kept,
                                  int replace_with);

// The replaceWith a single Sidebar::delete_filament call takes in the middle of a batch: the kept
// slot `replace_with` (an index from before the batch) re-indexed for the slots `removed_so_far`
// (also indices from before the batch) already being gone. -1 stays -1.
int replacement_during_batch(int replace_with, const std::vector<int>& removed_so_far);

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_FilamentUiRules_hpp_
