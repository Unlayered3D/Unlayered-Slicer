#ifndef slic3r_GUI_Actions_PrinterRules_hpp_
#define slic3r_GUI_Actions_PrinterRules_hpp_

// The pure (GUI-free) rules behind the printer methods (WebAppPrinters.cpp, WebAppBridge.cpp
// section 4.6): when the Device tab refuses a control, what it sends, and how the page's values map
// onto the printer's. Each rule names the GUI code it mirrors, so the two stay in step. Header
// only; tests/slic3rutils/test_actions_printers.cpp checks them without a running application.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Slic3r { namespace GUI { namespace Actions { namespace PrinterRules {

// StatusPanel::on_axis_ctrl_e_*: the extruder moves only when the current nozzle is this hot.
constexpr int E_JOG_MIN_NOZZLE_C = 170; // TEMP_THRESHOLD_ALLOW_E_CTRL
// StatusPanel::on_axis_ctrl_z_*: Z always moves at this feed rate; XY at 3000.
constexpr int Z_JOG_SPEED  = 900;
constexpr int XY_JOG_SPEED = 3000;
constexpr int E_JOG_SPEED  = 900;
// Fallbacks when the printer does not report a range (StatusPanel's initial SetMaxTemp values).
constexpr int DEFAULT_NOZZLE_MAX_C = 300;

// The midpoint of a tray's nozzle temperature range, as StatusPanel::on_ams_load_curr computes it
// for old_temp / new_temp; -1 (the printer's default) when either end is missing.
inline int tray_mid_temp(const std::string& min_c, const std::string& max_c)
{
    if (min_c.empty() || max_c.empty())
        return -1;
    return (std::atoi(min_c.c_str()) + std::atoi(max_c.c_str())) / 2;
}

// FanControlNew::command_control_fan: the fan moves in tenths. The old protocol takes 0-255
// (floor(step * 25.5)), the new one (is_enable_np) percent (step * 10).
inline int fan_step(double percent) { return std::clamp(int(std::lround(percent / 10.0)), 0, 10); }
inline int fan_value_old_protocol(int step) { return int(std::floor(step * 25.5)); }
inline int fan_value_new_protocol(int step) { return step * 10; }

// StatusPanel::update_ams_control_state, the reasons Load / Unload are disabled. Facts the bridge
// reads from the MachineObject; the empty string means allowed.
struct FilamentFacts
{
    bool printing          = false; // is_in_printing()
    bool can_resume        = false; // paused
    bool virtual_slot      = false; // the external spool (ams 254 / 255)
    bool changing_filament = false; // busy loading (NP) or ams_status_main == FILAMENT_CHANGE
    bool track_switch_installed = false;
    bool track_switch_ready     = false;
    bool slot_loaded       = false; // an extruder already holds exactly this slot
    bool slot_known        = true;  // the AMS and tray exist
    bool slot_has_filament = true;  // tray is_exists
};

inline std::string filament_refusal(const FilamentFacts& f, bool load)
{
    if (f.printing && !f.can_resume)
        return "the printer is busy with another print job";
    if (f.can_resume && !f.virtual_slot)
        return "while a print is paused, filament can be loaded and unloaded only from the external spool";
    if (f.changing_filament)
        return "the extruder is busy changing filament; wait for it to finish";
    if (f.track_switch_installed && f.virtual_slot)
        return "the external spool cannot be loaded or unloaded while the Filament Track Switch is installed";
    if (f.track_switch_installed && !f.track_switch_ready)
        return "the Filament Track Switch has not been set up; set it up on the printer first";
    if (!load)
        return {};
    if (f.slot_loaded)
        return "that slot is already loaded";
    if (!f.virtual_slot && !f.slot_known)
        return "no such AMS slot; read slicer.printerStatus for the AMS units and their trays";
    if (!f.virtual_slot && !f.slot_has_filament)
        return "the selected slot is empty";
    return {};
}

// StatusPanel::on_set_chamber_temp: refused in cooling mode while printing on printers with an
// air duct; a target at or above the switch temperature moves a printer that is not already in
// heating mode into heating mode, which the person confirms.
inline std::string chamber_refusal(bool printing, bool supports_airduct, bool cooling_mode)
{
    return printing && supports_airduct && cooling_mode ? "the chamber temperature cannot be changed in cooling mode while printing"
                                                        : std::string();
}
inline bool chamber_switches_to_heating(bool heating_mode, int celsius, int switch_heat_c)
{
    return !heating_mode && celsius >= switch_heat_c;
}

// StatusPanel::on_nozzle_selected: no extruder switch while printing (paused is fine) or while
// the AMS is changing filament.
inline std::string select_extruder_refusal(bool printing, bool paused, bool changing_filament)
{
    return (printing && !paused) || changing_filament ? "the printer is busy with another print job" : std::string();
}

// AMSDryCtrWin::update_normal_description: each drying AMS type has its own temperature range, and
// the time is 1 to 24 hours. type: DevAmsType (N3F = AMS 2 Pro, N3S = AMS HT).
struct DryLimits
{
    int min_c = 0, max_c = 0;
    const char* name = "";
};
inline DryLimits dry_limits(bool is_n3f, bool is_n3s)
{
    if (is_n3f)
        return {45, 65, "AMS 2 Pro"};
    if (is_n3s)
        return {45, 85, "AMS HT"};
    return {};
}
inline std::string dry_refusal(const DryLimits& lim, int celsius, int hours)
{
    if (lim.max_c == 0)
        return "this AMS cannot dry filament";
    if (celsius < lim.min_c || celsius > lim.max_c)
        return std::string(lim.name) + " dries at " + std::to_string(lim.min_c) + " to " + std::to_string(lim.max_c) + " C";
    if (hours < 1 || hours > 24)
        return "hours must be 1 to 24";
    return {};
}

// SelectMachineDialog::on_rename_enter: the job name. Runs of spaces collapse to one; then the
// name must be non-empty, under 100 characters, not start or end with a space, and hold none of
// <>[]:/\|?*" nor the "(modified)" suffix. `out` receives the normalised name.
inline std::string job_name_refusal(const std::string& name, std::string& out)
{
    out.clear();
    bool prev_space = false;
    for (char c : name) {
        if (c == ' ') {
            if (!prev_space)
                out += c;
            prev_space = true;
        } else {
            out += c;
            prev_space = false;
        }
    }
    if (out.find_first_of("<>[]:/\\|?*\"") != std::string::npos)
        return "the job name may not contain any of <>[]:/\\|?*\"";
    if (out.find("(modified)") != std::string::npos)
        return "the job name may not contain \"(modified)\"";
    if (out.empty())
        return "the job name may not be empty";
    if (out.front() == ' ' || out.back() == ' ')
        return "the job name may not start or end with a space";
    if (out.size() >= 100)
        return "the job name must be shorter than 100 characters";
    return {};
}

// DeviceErrorDialog::ActionButton: the buttons the printer offers for an error, by the stable name
// the page uses. Buttons that only navigate the slicer's own windows (Assistant, live view, nozzle
// rack) or carry the printer's own request (Proceed, Don't remind next time) have no name here.
struct ErrorAction
{
    int         button;
    const char* name;
};
inline const ErrorAction* error_actions()
{
    static const ErrorAction table[] = {
        {2, "resumePrinting"},       {3, "resumePrintingDefects"}, {4, "resumePrintingProblemSolved"}, {5, "stopPrinting"},
        {7, "filamentExtruded"},     {8, "retryFilamentExtruded"}, {9, "continue"},                   {11, "ok"},
        {12, "filamentLoadResume"},  {23, "noReminderNextTime"},   {24, "refreshNozzle"},              {25, "ignoreNoReminderNextTime"},
        {27, "ignoreResume"},        {28, "problemSolvedResume"},  {29, "turnOffFireAlarm"},           {34, "retryProblemSolved"},
        {35, "stopDrying"},          {51, "abort"},                {54, "disablePurification"},        {10001, "done"},
        {10002, "retry"},            {10004, "ok"},                {0, nullptr},
    };
    return table;
}
inline const char* error_action_name(int button)
{
    for (const ErrorAction* a = error_actions(); a->name != nullptr; ++a)
        if (a->button == button)
            return a->name;
    return nullptr;
}

// The RepRapFirmware M220 factor of each Bambu speed level (silent, standard, sport, ludicrous).
inline int rrf_speed_percent(int level)
{
    static const int pct[] = {0, 50, 100, 124, 166};
    return level >= 1 && level <= 4 ? pct[level] : 100;
}

}}}} // namespace Slic3r::GUI::Actions::PrinterRules

#endif // slic3r_GUI_Actions_PrinterRules_hpp_
