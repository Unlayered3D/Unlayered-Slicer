#include "WebAppPrinters.hpp"

#include "WebAppBridge.hpp" // WebAppBridgeError
#include "Actions/PrinterRules.hpp"

#include "GUI_App.hpp"
#include "HMS.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevBed.h"
#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevConfigUtil.h"
#include "DeviceCore/DevCtrl.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevFan.h"
#include "DeviceCore/DevFilaSwitch.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevHMS.h"
#include "DeviceCore/DevLamp.h"
#include "DeviceCore/DevStorage.h"
#include "DeviceCore/DevUtilBackend.h"

#include "slic3r/Utils/DuetRrfClient.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace Slic3r { namespace GUI { namespace WebAppPrinters {

using nlohmann::json;
namespace Rules = Actions::PrinterRules;

ControlPlan ControlPlan::refuse(int code, std::string message, std::string reason)
{
    ControlPlan p;
    p.code    = code;
    p.message = std::move(message);
    p.reason  = std::move(reason);
    return p;
}

namespace {

const int MAX_GCODE_LINES = 20;
// RepRapFirmware hosts report no limits to the slicer; these are the caps there.
const int RRF_MAX_NOZZLE_C  = 350;
const int RRF_MAX_BED_C     = 120;
const int RRF_MAX_CHAMBER_C = 65;

std::string u8(const wxString& s) { return std::string(s.ToUTF8().data()); }

bool get_number(const json& p, const char* key, double& out)
{
    auto it = p.find(key);
    if (it == p.end() || !it->is_number())
        return false;
    out = it->get<double>();
    return true;
}

bool get_int(const json& p, const char* key, int& out)
{
    double d = 0;
    if (!get_number(p, key, d) || std::floor(d) != d)
        return false;
    out = int(d);
    return true;
}

std::string get_string(const json& p, const char* key)
{
    auto it = p.find(key);
    return it != p.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string state_of(MachineObject* obj)
{
    if (obj == nullptr || !obj->is_online())
        return "offline";
    const std::string& s = obj->print_status;
    if (s == "RUNNING" || s == "PREPARE" || s == "SLICING")
        return "printing";
    if (s == "PAUSE")
        return "paused";
    if (s == "FINISH")
        return "finished";
    if (s == "FAILED")
        return "failed";
    if (s == "IDLE" || s.empty())
        return "idle";
    return "unknown";
}

const char* ams_type_name(DevAmsType t)
{
    switch (t) {
    case DevAmsType::AMS: return "ams";
    case DevAmsType::AMS_LITE:
    case DevAmsType::AMS_LITE_MIXED: return "amsLite";
    case DevAmsType::N3F: return "ams2Pro";
    case DevAmsType::N3S: return "amsHt";
    case DevAmsType::EXT_SPOOL: return "external";
    }
    return "unknown";
}

const char* dry_status_name(DevAms::DryStatus s)
{
    switch (s) {
    case DevAms::DryStatus::Off: return "off";
    case DevAms::DryStatus::Checking: return "checking";
    case DevAms::DryStatus::Drying: return "drying";
    case DevAms::DryStatus::Cooling: return "cooling";
    case DevAms::DryStatus::Stopping: return "stopping";
    case DevAms::DryStatus::Error: return "error";
    case DevAms::DryStatus::CannotStopHeatOutofControl: return "heatOutOfControl";
    case DevAms::DryStatus::PrdTesting: return "testing";
    }
    return "unknown";
}

const char* cannot_dry_name(DevAms::CannotDryReason r)
{
    switch (r) {
    case DevAms::CannotDryReason::TaskOccupied: return "taskOccupied";
    case DevAms::CannotDryReason::InsufficientPower: return "insufficientPower";
    case DevAms::CannotDryReason::AmsBusy: return "amsBusy";
    case DevAms::CannotDryReason::ConsumableAtAmsOutlet: return "filamentAtAmsOutlet";
    case DevAms::CannotDryReason::InitiatingAmsDrying: return "initiatingDrying";
    case DevAms::CannotDryReason::NotSupportedIn2dMode: return "notSupportedIn2dMode";
    case DevAms::CannotDryReason::DryingInProgress: return "dryingInProgress";
    case DevAms::CannotDryReason::Upgrading: return "upgrading";
    case DevAms::CannotDryReason::InsufficientPowerNeedPluginPower: return "needsPowerAdapter";
    case DevAms::CannotDryReason::FilamentAtAmsOutletManualUnload: return "filamentAtAmsOutletUnloadByHand";
    }
    return "unknown";
}

// The fan keys the page uses, by AIR_FUN id (FanControlPopupNew::get_fan_func_name).
const char* fan_key(int id)
{
    switch (id) {
    case AIR_FUN::FAN_HEAT_BREAK_0_IDX: return "hotend";
    case AIR_FUN::FAN_COOLING_0_AIRDOOR: return "part";
    case AIR_FUN::FAN_REMOTE_COOLING_0_IDX: return "aux";
    case AIR_FUN::FAN_CHAMBER_0_IDX: return "chamber";
    case AIR_FUN::FAN_HEAT_BREAK_1_IDX: return "hotend2";
    case AIR_FUN::FAN_MC_BOARD_0_IDX: return "board";
    case AIR_FUN::FAN_INNNER_LOOP_FAN_0_IDX: return "heat";
    case AIR_FUN::FAN_REMOTE_COOLING_1_IDX: return "aux2";
    default: return "fan";
    }
}

const char* airduct_mode_name(int mode)
{
    switch (mode) {
    case AIR_DUCT::AIR_DUCT_COOLING_FILT: return "cooling";
    case AIR_DUCT::AIR_DUCT_HEATING_INTERNAL_FILT: return "heating";
    case AIR_DUCT::AIR_DUCT_EXHAUST: return "exhaust";
    case AIR_DUCT::AIR_DUCT_FULL_COOLING: return "fullCooling";
    case AIR_DUCT::AIR_DUCT_INIT: return "init";
    default: return "none";
    }
}

int airduct_mode_id(const std::string& name)
{
    for (int m : {int(AIR_DUCT::AIR_DUCT_COOLING_FILT), int(AIR_DUCT::AIR_DUCT_HEATING_INTERNAL_FILT), int(AIR_DUCT::AIR_DUCT_EXHAUST),
                  int(AIR_DUCT::AIR_DUCT_FULL_COOLING)})
        if (name == airduct_mode_name(m))
            return m;
    return AIR_DUCT::AIR_DUCT_NONE;
}

// "manual" (the person sets it), "auto" (the printer does) or "off", in the current air-duct mode
// (FanControlNew::update_mode).
std::string fan_control_of(const AirDuctData& data, int part_id)
{
    if (data.curren_mode < 0)
        return "manual";
    auto it = data.modes.find(data.curren_mode);
    if (it == data.modes.end())
        return "manual";
    const AirMode& mode = it->second;
    if (std::find(mode.off.begin(), mode.off.end(), part_id) != mode.off.end())
        return "off";
    if (std::find(mode.ctrl.begin(), mode.ctrl.end(), part_id) == mode.ctrl.end())
        return "auto";
    return "manual";
}

// The fans the Device tab's fan popup lists. Without air-duct data, the old part / aux / chamber
// fans (DevFan::converse_to_duct), read without changing the printer object.
json fans_json(MachineObject* obj)
{
    json    fans = json::array();
    DevFan* fan  = obj->GetFan();
    if (fan == nullptr)
        return fans;
    const AirDuctData data = fan->GetAirDuctData();
    if (!data.modes.empty()) {
        for (const AirParts& part : data.parts)
            fans.push_back({{"id", part.id}, {"key", fan_key(part.id)}, {"percent", part.state}, {"control", fan_control_of(data, part.id)}});
        return fans;
    }
    auto pct = [](int raw) { return int(std::lround(raw / 25.5)) * 10; };
    fans.push_back({{"id", int(AIR_FUN::FAN_COOLING_0_AIRDOOR)}, {"key", "part"}, {"percent", pct(fan->GetCoolingFanSpeed())}, {"control", "manual"}});
    if (fan->GetSupportAuxFanData()) {
        fans.push_back({{"id", int(AIR_FUN::FAN_REMOTE_COOLING_0_IDX)}, {"key", "aux"}, {"percent", pct(fan->GetBigFan1Speed())}, {"control", "manual"}});
        fans.push_back({{"id", int(AIR_FUN::FAN_CHAMBER_0_IDX)}, {"key", "chamber"}, {"percent", pct(fan->GetBigFan2Speed())}, {"control", "manual"}});
    }
    return fans;
}

int to_int(const std::string& s, int fallback = -1)
{
    if (s.empty())
        return fallback;
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

json tray_json(const DevAmsTray& t)
{
    json j = {{"slot", to_int(t.id, 0)},
              {"exists", t.is_exists},
              {"type", t.get_display_filament_type()},
              {"color", t.color.empty() ? std::string() : "#" + t.color},
              {"remain", t.remain},
              {"filamentId", t.setting_id},
              {"settingId", t.filament_setting_id},
              {"bambuSpool", t.is_bbl}};
    if (t.cols.size() > 1) {
        json cols = json::array();
        for (const std::string& c : t.cols)
            cols.push_back("#" + c);
        j["colors"] = cols;
    }
    if (!t.nozzle_temp_min.empty() && !t.nozzle_temp_max.empty()) {
        j["nozzleTempMin"] = to_int(t.nozzle_temp_min);
        j["nozzleTempMax"] = to_int(t.nozzle_temp_max);
    }
    return j;
}

json ams_json(MachineObject* obj)
{
    json units = json::array();
    if (std::shared_ptr<DevFilaSystem> fila = obj->GetFilaSystem()) {
        for (auto& [id, ams] : fila->GetAmsList()) {
            if (ams == nullptr)
                continue;
            json unit = {{"id", to_int(id, 0)}, {"type", ams_type_name(ams->GetAmsType())}, {"extruder", ams->GetExtruderId()}};
            if (ams->SupportHumidity()) {
                unit["humidityLevel"] = ams->GetHumidityLevel();
                if (ams->GetHumidityPercent() >= 0)
                    unit["humidityPercent"] = ams->GetHumidityPercent();
            }
            if (ams->GetCurrentTemperature() != INVALID_AMS_TEMPERATURE)
                unit["temperature"] = ams->GetCurrentTemperature();
            if (ams->SupportDrying()) {
                json dry = {{"remote", ams->IsSupportRemoteDry(obj)},
                            {"status", ams->GetDryStatus() ? dry_status_name(*ams->GetDryStatus()) : "off"},
                            {"leftMinutes", ams->GetLeftDryTime()}};
                json reasons = json::array();
                if (auto r = ams->GetCannotDryReason())
                    for (DevAms::CannotDryReason reason : *r)
                        reasons.push_back(cannot_dry_name(reason));
                dry["cannotDryReasons"] = reasons;
                if (auto s = ams->GetDrySettings())
                    dry["settings"] = {{"filamentType", s->dry_filament}, {"celsius", s->dry_temp}, {"hours", s->dry_hour}};
                unit["drying"] = dry;
            }
            json trays = json::array();
            for (auto& [tray_id, tray] : ams->GetTrays())
                if (tray != nullptr)
                    trays.push_back(tray_json(*tray));
            unit["trays"] = trays;
            units.push_back(unit);
        }
    }
    json external = json::array();
    for (size_t i = 0; i < obj->vt_slot.size() && i < 2; ++i) {
        json t   = tray_json(obj->vt_slot[i]);
        t["ams"] = i == 0 ? VIRTUAL_TRAY_MAIN_ID : VIRTUAL_TRAY_DEPUTY_ID;
        t["slot"] = 0;
        external.push_back(t);
    }
    return {{"units", units}, {"externalSpools", external}};
}

// The buttons the Device tab offers for the current print error (DeviceErrorDialog::show_error_code).
std::vector<int> error_buttons(MachineObject* obj, int print_error)
{
    static const std::set<std::string> s_retry_codes = {"0701-8004", "0701-8005", "0701-8007", "0701-8012",
                                                         "0702-8012", "0703-8012", "07FF-8012", "07FF-8013"};
    std::vector<int> buttons;
    if (s_retry_codes.count(MachineObject::get_error_code_str(print_error)) != 0)
        return {10002 /*DBL_CHECK_RETRY*/, 10004 /*DBL_CHECK_OK*/};
    if (HMSQuery* hms = wxGetApp().get_hms_query())
        hms->query_print_image_action(obj, print_error, buttons);
    return buttons;
}

json error_json(MachineObject* obj)
{
    json e = {{"code", MachineObject::get_error_code_str(obj->print_error)}};
    if (HMSQuery* hms = wxGetApp().get_hms_query())
        e["text"] = u8(hms->query_print_error_msg(obj, obj->print_error));
    json actions = json::array();
    for (int b : error_buttons(obj, obj->print_error))
        if (const char* name = Rules::error_action_name(b); name != nullptr && std::find(actions.begin(), actions.end(), name) == actions.end())
            actions.push_back(name);
    e["actions"] = actions;
    return e;
}

const char* hms_level_name(HMSMessageLevel l)
{
    switch (l) {
    case HMS_FATAL: return "fatal";
    case HMS_SERIOUS: return "serious";
    case HMS_COMMON: return "common";
    case HMS_INFO: return "info";
    default: return "unknown";
    }
}

const char* storage_name(DevStorage::SdcardState s)
{
    switch (s) {
    case DevStorage::NO_SDCARD: return "none";
    case DevStorage::HAS_SDCARD_NORMAL: return "normal";
    case DevStorage::HAS_SDCARD_ABNORMAL: return "abnormal";
    case DevStorage::HAS_SDCARD_READONLY: return "readonly";
    default: return "unknown";
    }
}

// The cap the Device tab's temperature inputs use (StatusPanel::update_temp_ctrl).
int bed_max_c(MachineObject* obj)
{
    int limit = obj->get_bed_temperature_limit();
    if (obj->bed_temp_range.size() > 1)
        limit = obj->bed_temp_range[1];
    return limit;
}
int nozzle_min_c(MachineObject* obj) { return obj->nozzle_temp_range.size() >= 2 ? obj->nozzle_temp_range[0] : 0; }
int nozzle_max_c(MachineObject* obj) { return obj->nozzle_temp_range.size() >= 2 ? obj->nozzle_temp_range[1] : Rules::DEFAULT_NOZZLE_MAX_C; }

// The AMS is changing filament (StatusPanel::update_filament_loading_panel).
bool is_loading_filament(MachineObject* obj)
{
    if (obj->is_enable_np && obj->ams_status_main != AMS_STATUS_MAIN_COLD_PULL)
        return obj->GetExtderSystem()->IsBusyLoading();
    return obj->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE;
}

// StatusPanel::update_ams_control_state's "switch now".
bool is_switching_filament(MachineObject* obj)
{
    return obj->is_enable_np ? obj->GetExtderSystem()->IsBusyLoading() : obj->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE;
}

bool ctrl_area_locked(MachineObject* obj) { return obj->is_in_printing() && !obj->can_resume(); }

std::string itos(int v) { return std::to_string(v); }

ControlPlan plan_with(std::string summary, std::string command, std::function<int(MachineObject*)> run)
{
    ControlPlan p;
    p.summary = std::move(summary);
    p.command = std::move(command);
    p.run     = std::move(run);
    return p;
}

// Which filament the dry action means: a filament preset name or alias, or a filament id.
std::optional<FilamentBaseInfo> drying_filament(const std::string& wanted)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || wanted.empty())
        return std::nullopt;
    std::string filament_id = wanted;
    if (const Preset* preset = bundle->filaments.find_preset(wanted, false); preset != nullptr && !preset->filament_id.empty())
        filament_id = preset->filament_id;
    return bundle->get_filament_by_filament_id(filament_id);
}

std::string pla_filament_id()
{
    NetworkAgent* agent = wxGetApp().getAgent();
    return agent ? agent->to_orca_filament_id("GFA00") : std::string("GFA00");
}

// AMSDryCtrWin::start_sending_drying_command and the checks that enable its Start button
// (update_normal_description, update_filament_guide_info, update_state, update_normal_state).
ControlPlan plan_dry(MachineObject* obj, const json& p)
{
    int ams_id = -1, celsius = 0, hours = 0;
    if (!get_int(p, "ams", ams_id) || !get_int(p, "celsius", celsius) || !get_int(p, "hours", hours))
        return ControlPlan::refuse(WAB_BAD_PARAMS, "dryAms needs ams, celsius and hours (integers)");
    std::shared_ptr<DevFilaSystem> fila = obj->GetFilaSystem();
    DevAms*                        ams  = fila ? fila->GetAmsById(std::to_string(ams_id)) : nullptr;
    if (ams == nullptr)
        return ControlPlan::refuse(WAB_NOT_FOUND, "no AMS " + itos(ams_id) + " on this printer; read slicer.printerStatus");
    if (!ams->IsSupportRemoteDry(obj))
        return ControlPlan::refuse(WAB_UNSUPPORTED_TYPE, "this AMS cannot be dried from the slicer");
    if (auto reasons = ams->GetCannotDryReason(); reasons && !reasons->empty() &&
                                                  !(reasons->size() == 1 && (*reasons)[0] == DevAms::CannotDryReason::DryingInProgress)) {
        std::vector<std::string> names;
        for (auto r : *reasons)
            names.push_back(cannot_dry_name(r));
        return ControlPlan::refuse(WAB_ACTION_DISABLED, "the printer cannot dry this AMS now (" + boost::algorithm::join(names, ", ") + ")",
                                   "cannotDry");
    }
    if (auto s = ams->GetDryStatus(); s && *s != DevAms::DryStatus::Off && *s != DevAms::DryStatus::Cooling)
        return ControlPlan::refuse(WAB_ACTION_DISABLED, "this AMS is already drying; stop it first (stopDryingAms)", "drying");
    const DevAmsType type = ams->GetAmsType();
    if (std::string why = Rules::dry_refusal(Rules::dry_limits(type == DevAmsType::N3F, type == DevAmsType::N3S), celsius, hours); !why.empty())
        return ControlPlan::refuse(WAB_BAD_PARAMS, why);

    // The filament the drying is for: the one named, else the Device tab's default (the loaded
    // filament with the lowest recommended drying temperature, unknown ones counted as PLA).
    const bool printing = obj->is_in_printing() && obj->GetExtderSystem()->GetCurrentAmsId() == ams->GetAmsId();
    const auto fallback = DevUtilBackend::GetFilamentDryingPreset(pla_filament_id());
    auto       preset_of = [&](const DevAmsTray& t) {
        auto pr = t.get_display_filament_type().empty() ? std::nullopt : t.get_ams_drying_preset();
        return pr ? pr : fallback;
    };
    float       recommended = std::numeric_limits<float>::max();
    std::string default_id  = fallback ? fallback->filament_id : pla_filament_id();
    bool        has_filament = false;
    for (auto& [id, tray] : ams->GetTrays()) {
        if (tray == nullptr)
            continue;
        has_filament |= tray->is_exists;
        if (tray->is_exists) {
            // update_filament_guide_info: Start stays off while a loaded spool would soften.
            if (auto pr = preset_of(*tray); pr && int(pr->filament_dev_ams_drying_heat_distortion_temperature) < celsius)
                return ControlPlan::refuse(WAB_BAD_PARAMS,
                                           "slot " + tray->id + " holds a filament that softens below " + itos(celsius) +
                                               " C; take it out of the AMS or dry at a lower temperature");
        }
        if (!tray->is_tray_info_ready())
            continue;
        auto pr = preset_of(*tray);
        if (!pr)
            continue;
        const float t = printing ? std::min({pr->filament_dev_ams_drying_temperature_on_print[type], pr->filament_dev_drying_softening_temperature,
                                             pr->filament_dev_ams_drying_heat_distortion_temperature}) :
                                   pr->filament_dev_ams_drying_temperature_on_idle[type];
        if (t < recommended) {
            recommended = t;
            default_id  = pr->filament_id;
        }
    }
    const std::string wanted = get_string(p, "filament");
    auto              info   = drying_filament(wanted.empty() ? default_id : wanted);
    if (!info)
        return ControlPlan::refuse(WAB_NOT_FOUND, "no filament preset or filament id \"" + (wanted.empty() ? default_id : wanted) + "\"");
    if (printing && recommended != std::numeric_limits<float>::max() && celsius > recommended)
        return ControlPlan::refuse(WAB_BAD_PARAMS, "this AMS is printing: the drying temperature may not exceed the recommended " +
                                                       itos(int(recommended)) + " C");
    const auto preset = DevUtilBackend::GetFilamentDryingPreset(info->filament_id);
    if (preset && has_filament && celsius > int(preset->filament_dev_ams_drying_heat_distortion_temperature))
        return ControlPlan::refuse(WAB_BAD_PARAMS, "the temperature may not exceed the filament's heat distortion temperature (" +
                                                       itos(int(preset->filament_dev_ams_drying_heat_distortion_temperature)) + " C)");
    const int  cooling = preset ? int(preset->filament_dev_drying_softening_temperature) : 50;
    const bool rotate  = p.value("rotate", false);
    const std::string type_name = info->filament_type;
    ControlPlan plan = plan_with("Dry AMS " + itos(ams_id) + " for " + type_name + " at " + itos(celsius) + " C for " + itos(hours) + " h" +
                                     (rotate ? ", rotating the spools" : ""),
                                 "dry ams " + itos(ams_id) + " " + type_name + " " + itos(celsius) + "C " + itos(hours) + "h",
                                 [ams_id, type_name, celsius, hours, rotate, cooling](MachineObject* o) {
                                     auto f = o->GetFilaSystem();
                                     return f ? f->CtrlAmsStartDryingHour(ams_id, type_name, celsius, hours, rotate, cooling, false) : -1;
                                 });
    if (preset && preset->ams_limitations.count(type) == 0)
        plan.warning = "This filament may not be completely dried.";
    return plan;
}

} // namespace

std::string bambu_state(MachineObject* obj) { return state_of(obj); }

json bambu_status(MachineObject* obj)
{
    json temps = json::object();
    if (DevBed* bed = obj->GetBed()) {
        temps["bed"]       = bed->GetBedTemp();
        temps["bedTarget"] = bed->GetBedTempTarget();
    }
    json extruders = json::array();
    if (DevExtderSystem* ext = obj->GetExtderSystem()) {
        json now = json::array(), target = json::array();
        for (int i = 0; i < ext->GetTotalExtderCount(); ++i) {
            now.push_back(ext->GetNozzleTempCurrent(i));
            target.push_back(ext->GetNozzleTempTarget(i));
            json e = {{"id", i}, {"nozzleDiameter", ext->GetNozzleDiameter(i)}, {"nozzleType", "unknown"}, {"hasFilament", ext->HasFilamentInExt(i)}};
            auto it = NozzleTypeEumnToStr.find(ext->GetNozzleType(i));
            if (it != NozzleTypeEumnToStr.end())
                e["nozzleType"] = it->second;
            if (auto x = ext->GetExtderById(i); x && x->HasFilamentInExt())
                e["loaded"] = {{"ams", to_int(x->GetSlotNow().ams_id)}, {"slot", to_int(x->GetSlotNow().slot_id)}};
            extruders.push_back(e);
        }
        temps["nozzle"]       = now;
        temps["nozzleTarget"] = target;
    }
    temps["chamber"] = obj->chamber_temp;
    if (obj->GetConfig()->SupportChamberEdit())
        temps["chamberTarget"] = obj->chamber_temp_target;

    json limits = {{"bedMax", bed_max_c(obj)}, {"nozzleMin", nozzle_min_c(obj)}, {"nozzleMax", nozzle_max_c(obj)}};
    if (obj->GetConfig()->SupportChamberEdit()) {
        limits["chamberMin"] = obj->GetConfig()->GetChamberTempEditMin();
        limits["chamberMax"] = obj->GetConfig()->GetChamberTempEditMax();
    }

    json data = {{"state", state_of(obj)},
                 {"progress", obj->mc_print_percent},
                 {"remainingMinutes", obj->mc_left_time / 60},
                 {"layer", obj->curr_layer},
                 {"totalLayers", obj->total_layers},
                 {"job", obj->subtask_name},
                 {"stage", u8(obj->get_curr_stage())},
                 {"temperatures", temps},
                 {"limits", limits},
                 {"extruders", extruders},
                 {"currentExtruder", obj->GetExtderSystem() ? obj->GetExtderSystem()->GetCurrentExtderId() : 0},
                 {"fans", fans_json(obj)},
                 {"storage", obj->GetStorage() ? storage_name(obj->GetStorage()->get_sdcard_state()) : "unknown"},
                 {"firmware", obj->get_ota_version()},
                 {"ams", ams_json(obj)}};
    if (const int lvl = int(obj->GetPrintingSpeedLevel()); lvl >= 1 && lvl <= 4)
        data["speedLevel"] = lvl;
    if (DevLamp* lamp = obj->GetLamp(); lamp != nullptr && obj->is_fdm_type())
        data["light"] = {{"on", lamp->IsChamberLightOn()}, {"offWarns", lamp->HasLampCloseRecheck()}};
    if (DevFan* fan = obj->GetFan(); fan != nullptr && fan->GetSupportAirduct()) {
        const AirDuctData d     = fan->GetAirDuctData();
        json              modes = json::array();
        for (auto& [id, m] : d.modes)
            modes.push_back(airduct_mode_name(id));
        data["airduct"] = {{"mode", airduct_mode_name(d.curren_mode)}, {"modes", modes}, {"coolingFilter", d.IsCoolingFilerOn()},
                           {"supportsCoolingFilter", d.IsSupportCoolingFilter()}};
    }
    data["filamentChange"] = {{"inProgress", is_loading_filament(obj)},
                              {"canRetry", is_loading_filament(obj) || obj->GetExtderSystem()->CanRetrySwitching()},
                              {"canAbort", (is_loading_filament(obj) && (obj->is_support_fila_change_abort ||
                                                                         DevPrinterConfigUtil::support_ams_fila_change_abort(obj->printer_type))) ||
                                               obj->GetExtderSystem()->CanQuitSwitching()}};
    json hms = json::array();
    if (DevHMS* h = obj->GetHMS()) {
        HMSQuery* query = wxGetApp().get_hms_query();
        for (const DevHMSItem& item : h->GetHMSItems()) {
            const std::string code = item.get_long_error_code();
            hms.push_back({{"code", code}, {"level", hms_level_name(item.get_level())}, {"text", query ? u8(query->query_hms_msg(obj, code)) : ""}});
        }
    }
    data["hms"] = hms;
    if (obj->print_error != 0)
        data["error"] = error_json(obj);
    data["supports"] = {{"partSkip", obj->is_support_partskip},
                        {"airduct", obj->GetFan() && obj->GetFan()->GetSupportAirduct()},
                        {"chamberTemp", obj->GetConfig()->SupportChamberEdit()},
                        {"remoteDrying", obj->is_support_remote_dry},
                        {"printAllPlates", obj->GetConfig()->SupportPrintAllPlates()},
                        {"filamentTrackSwitch", obj->GetFilaSwitch() && obj->GetFilaSwitch()->IsInstalled()}};
    return data;
}

bool rrf_status(DuetRrfClient& client, json& out, std::string& error)
{
    namespace pt = boost::property_tree;
    std::string status;
    if (!client.read_status(status, error))
        return false;
    out["state"] = status;
    auto array = [](const pt::ptree& t, const std::string& path) {
        std::vector<pt::ptree> v;
        if (auto c = t.get_child_optional(path))
            for (auto& kv : *c)
                v.push_back(kv.second);
        return v;
    };
    pt::ptree job;
    std::string ignored;
    if (client.object_model("job", "v", job, ignored)) {
        const double size = job.get<double>("file.size", 0), pos = job.get<double>("filePosition", 0);
        if (size > 0)
            out["progress"] = int(std::lround(std::clamp(pos / size, 0.0, 1.0) * 100));
        if (auto layer = job.get_optional<int>("layer"))
            out["layer"] = *layer;
        if (auto layers = job.get_optional<int>("file.numLayers"))
            out["totalLayers"] = *layers;
        if (auto left = job.get_optional<double>("timesLeft.file"))
            out["remainingMinutes"] = int(*left / 60);
        if (auto name = job.get_optional<std::string>("file.fileName"); name && !name->empty())
            out["job"] = *name;
    }
    pt::ptree heat, tools;
    if (client.object_model("heat", "v", heat, ignored)) {
        const std::vector<pt::ptree> heaters = array(heat, "heaters");
        auto heater = [&](int i, const char* key) -> json {
            if (i < 0 || i >= int(heaters.size()))
                return nullptr;
            auto v = heaters[size_t(i)].get_optional<double>(key);
            return v ? json(*v) : json(nullptr);
        };
        json temps = json::object();
        const std::vector<pt::ptree> beds = array(heat, "bedHeaters"), chambers = array(heat, "chamberHeaters");
        if (!beds.empty()) {
            const int b        = beds.front().get_value<int>(-1);
            temps["bed"]       = heater(b, "current");
            temps["bedTarget"] = heater(b, "active");
        }
        if (!chambers.empty())
            temps["chamber"] = heater(chambers.front().get_value<int>(-1), "current");
        if (client.object_model("tools", "v", tools, ignored)) {
            json now = json::array(), target = json::array();
            for (const pt::ptree& tool : array(tools, "")) {
                const std::vector<pt::ptree> th = array(tool, "heaters");
                const int                    h  = th.empty() ? -1 : th.front().get_value<int>(-1);
                now.push_back(heater(h, "current"));
                target.push_back(heater(h, "active"));
            }
            temps["nozzle"]       = now;
            temps["nozzleTarget"] = target;
        }
        out["temperatures"] = temps;
    }
    return true;
}

ControlPlan plan_bambu_control(MachineObject* obj, const std::string& action, const json& p)
{
    if (obj == nullptr || !obj->is_online())
        return ControlPlan::refuse(WAB_ACTION_DISABLED, "the printer is offline; the Device tab cannot reach it either", "offline");
    DevExtderSystem* ext = obj->GetExtderSystem();

    if (action == "pause") {
        if (!obj->can_pause())
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "there is no running print to pause", "notPrinting");
        ControlPlan plan = plan_with("Pause the print", "pause", [](MachineObject* o) { return o->command_task_pause(); });
        plan.immediate   = true;
        return plan;
    }
    if (action == "stop") {
        if (!obj->can_abort())
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "there is no print to stop", "notPrinting");
        ControlPlan plan = plan_with("Stop the print", "stop", [](MachineObject* o) { return o->command_task_abort(); });
        plan.immediate   = true;
        return plan;
    }
    if (action == "resume") {
        if (!obj->can_resume())
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "the print is not paused", "notPaused");
        return plan_with("Resume the print", "resume", [](MachineObject* o) { return o->command_task_resume(); });
    }
    if (action == "setBedTemp" || action == "setNozzleTemp" || action == "setChamberTemp") {
        double c = 0;
        if (!get_number(p, "celsius", c) || c < 0)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "celsius must be a number of 0 or more");
        const int t = int(c);
        if (action == "setBedTemp") {
            const int max = bed_max_c(obj);
            if (t > max)
                return ControlPlan::refuse(WAB_BAD_PARAMS, "this printer's bed goes up to " + itos(max) + " C");
            return plan_with("Set the bed to " + itos(t) + " C", "bed " + itos(t), [t](MachineObject* o) { return o->command_set_bed(t); });
        }
        if (action == "setNozzleTemp") {
            int nozzle = 0;
            if (p.contains("nozzle") && !get_int(p, "nozzle", nozzle))
                return ControlPlan::refuse(WAB_BAD_PARAMS, "nozzle must be 0 (right / only) or 1 (left)");
            const int count = ext ? ext->GetTotalExtderCount() : 1;
            if (nozzle < 0 || nozzle >= count)
                return ControlPlan::refuse(WAB_BAD_PARAMS, "this printer has " + itos(count) + " nozzle(s); nozzle must be 0" + (count > 1 ? " or 1" : ""));
            const int min = nozzle_min_c(obj), max = nozzle_max_c(obj);
            if (t != 0 && (t < min || t > max))
                return ControlPlan::refuse(WAB_BAD_PARAMS, "a nozzle target must be 0 (off) or " + itos(min) + " to " + itos(max) + " C on this printer");
            // StatusPanel::on_set_nozzle_temp: a single-nozzle printer takes the old command, a
            // dual-nozzle one names the nozzle.
            if (count <= 1)
                return plan_with("Set the nozzle to " + itos(t) + " C", "nozzle " + itos(t), [t](MachineObject* o) { return o->command_set_nozzle(t); });
            return plan_with(std::string("Set the ") + (nozzle == MAIN_EXTRUDER_ID ? "right" : "left") + " nozzle to " + itos(t) + " C",
                             "nozzle " + itos(nozzle) + " " + itos(t), [nozzle, t](MachineObject* o) { return o->command_set_nozzle_new(nozzle, t); });
        }
        DevConfig* cfg = obj->GetConfig();
        if (!cfg->SupportChamberEdit())
            return ControlPlan::refuse(WAB_UNSUPPORTED_TYPE, "this printer's chamber temperature cannot be set");
        if (t != 0 && (t < cfg->GetChamberTempEditMin() || t > cfg->GetChamberTempEditMax()))
            return ControlPlan::refuse(WAB_BAD_PARAMS, "a chamber target must be 0 or " + itos(cfg->GetChamberTempEditMin()) + " to " +
                                                           itos(cfg->GetChamberTempEditMax()) + " C on this printer");
        DevFan* fan = obj->GetFan();
        if (std::string why = Rules::chamber_refusal(obj->is_in_printing(), fan && fan->GetSupportAirduct(), fan && fan->is_at_cooling_mode());
            !why.empty())
            return ControlPlan::refuse(WAB_ACTION_DISABLED, why, "coolingMode");
        ControlPlan plan = plan_with("Set the chamber to " + itos(t) + " C", "chamber " + itos(t), [t](MachineObject* o) { return o->command_set_chamber(t); });
        if (fan && Rules::chamber_switches_to_heating(fan->is_at_heating_mode(), t, cfg->GetChamberTempSwitchHeat()))
            plan.warning = "At " + itos(cfg->GetChamberTempSwitchHeat()) + " C or more the printer switches to heating mode.";
        return plan;
    }
    if (action == "setFan") {
        DevFan* fan = obj->GetFan();
        double  percent = 0;
        if (fan == nullptr || !get_number(p, "percent", percent) || percent < 0 || percent > 100)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "percent must be 0 to 100");
        int fan_id = -1;
        if (p.contains("fanId")) {
            if (!get_int(p, "fanId", fan_id))
                return ControlPlan::refuse(WAB_BAD_PARAMS, "fanId must be an id from slicer.printerStatus fans");
        } else {
            const std::string name = get_string(p, "fan").empty() ? std::string("part") : get_string(p, "fan");
            fan_id = name == "part" ? int(AIR_FUN::FAN_COOLING_0_AIRDOOR) : name == "aux" ? int(AIR_FUN::FAN_REMOTE_COOLING_0_IDX) :
                     name == "chamber" ? int(AIR_FUN::FAN_CHAMBER_0_IDX) : -1;
            if (fan_id < 0)
                return ControlPlan::refuse(WAB_BAD_PARAMS, "fan must be part, aux or chamber (or give fanId)");
        }
        // The fans the fan popup shows, and whether this one is the person's to set now.
        bool known = false;
        std::string control = "manual";
        for (const json& f : fans_json(obj))
            if (f["id"] == fan_id) {
                known   = true;
                control = f["control"].get<std::string>();
            }
        if (!known)
            return ControlPlan::refuse(WAB_NOT_FOUND, "this printer has no such fan; read slicer.printerStatus fans");
        if (control != "manual")
            return ControlPlan::refuse(WAB_ACTION_DISABLED, std::string("this fan is ") + (control == "off" ? "off" : "automatic") +
                                                                " in the current air-duct mode", "fanNotControllable");
        const int  step = Rules::fan_step(percent);
        const bool np   = obj->is_enable_np;
        const int  val  = np ? Rules::fan_value_new_protocol(step) : Rules::fan_value_old_protocol(step);
        ControlPlan plan = plan_with(std::string("Set the ") + fan_key(fan_id) + " fan to " + itos(step * 10) + "%",
                                     std::string(np ? "set_fan " : "M106 P") + itos(fan_id) + (np ? " " : " S") + itos(val),
                                     [fan_id, val, np](MachineObject* o) {
                                         if (o->GetFan() == nullptr)
                                             return -1;
                                         return np ? o->GetFan()->command_control_fan_new(fan_id, val) : o->GetFan()->command_control_fan(fan_id, val);
                                     });
        if (obj->is_in_printing())
            plan.warning = "Changing fan speed during printing may affect print quality.";
        return plan;
    }
    if (action == "setAirductMode") {
        DevFan* fan = obj->GetFan();
        if (fan == nullptr || !fan->GetSupportAirduct() || fan->GetAirDuctData().modes.empty())
            return ControlPlan::refuse(WAB_UNSUPPORTED_TYPE, "this printer has no air-duct modes");
        const AirDuctData d    = fan->GetAirDuctData();
        const std::string name = get_string(p, "mode");
        const int         mode = name.empty() ? d.curren_mode : airduct_mode_id(name);
        if (d.modes.count(mode) == 0)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "mode must be one of the printer's modes in slicer.printerStatus airduct.modes");
        if (mode != d.curren_mode && obj->is_in_printing())
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "the fan mode cannot be changed during printing", "printing");
        int submode = -1;
        if (p.contains("coolingFilter")) {
            if (mode != AIR_DUCT::AIR_DUCT_COOLING_FILT || !d.IsSupportCoolingFilter())
                return ControlPlan::refuse(WAB_BAD_PARAMS, "coolingFilter applies only to cooling mode on printers that have the filter");
            submode = p.value("coolingFilter", false) ? 1 : 0;
        } else if (mode == d.curren_mode) {
            return ControlPlan::refuse(WAB_BAD_PARAMS, std::string("the printer is already in ") + airduct_mode_name(mode) + " mode");
        }
        ControlPlan plan = plan_with(std::string("Switch the fans to ") + airduct_mode_name(mode) + " mode" +
                                         (submode < 0 ? "" : submode == 1 ? " with the filter on" : " with the filter off"),
                                     "set_airduct " + itos(mode) + " " + itos(submode), [mode, submode](MachineObject* o) {
                                         return o->GetFan() ? o->GetFan()->command_control_air_duct(mode, submode, [](const json&) {}) : -1;
                                     });
        if (submode == 1 && obj->is_in_printing())
            plan.warning = "Enabling filtration during printing may reduce cooling and affect print quality.";
        return plan;
    }
    if (action == "setSpeed") {
        int level = 0;
        if (!get_int(p, "level", level) || level < 1 || level > 4)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "level must be 1 (silent) to 4 (ludicrous)");
        if (!obj->is_in_printing())
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "the speed level only takes effect during printing", "notPrinting");
        return plan_with("Set the print speed level to " + itos(level), "speed " + itos(level),
                         [level](MachineObject* o) { return o->command_set_printing_speed(DevPrintingSpeedLevel(level)); });
    }
    if (action == "setLight") {
        if (!p.contains("on") || !p["on"].is_boolean())
            return ControlPlan::refuse(WAB_BAD_PARAMS, "on must be true or false");
        if (obj->GetLamp() == nullptr || !obj->is_fdm_type())
            return ControlPlan::refuse(WAB_UNSUPPORTED_TYPE, "this printer's light cannot be switched from the slicer");
        const bool  on   = p["on"].get<bool>();
        ControlPlan plan = plan_with(std::string("Turn the chamber light ") + (on ? "on" : "off"), on ? "light on" : "light off", [on](MachineObject* o) {
            if (o->GetLamp() == nullptr)
                return -1;
            o->GetLamp()->CtrlSetChamberLight(on ? DevLamp::LIGHT_EFFECT_ON : DevLamp::LIGHT_EFFECT_OFF);
            return 0;
        });
        // StatusPanel::on_lamp_switch's recheck, folded into the one confirmation.
        if (!on && obj->GetLamp()->HasLampCloseRecheck())
            plan.warning = "Turning off the light during the task will cause AI monitoring, such as spaghetti detection, to fail.";
        return plan;
    }
    if (action == "home") {
        if (ctrl_area_locked(obj))
            return ControlPlan::refuse(WAB_BUSY, "the printer is printing; axes move only while it is idle or paused", "printing");
        return plan_with("Home all axes", "home", [](MachineObject* o) { return o->command_go_home(); });
    }
    if (action == "jog") {
        const std::string axis = boost::to_upper_copy(get_string(p, "axis"));
        double            mm   = 0;
        if (axis != "X" && axis != "Y" && axis != "Z" && axis != "E")
            return ControlPlan::refuse(WAB_BAD_PARAMS, "axis must be X, Y, Z or E");
        if (!get_number(p, "mm", mm) || std::abs(mm) > 100 || mm == 0)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "mm must be a non-zero number from -100 to 100");
        if (ctrl_area_locked(obj))
            return ControlPlan::refuse(WAB_BUSY, "the printer is printing; axes move only while it is idle or paused", "printing");
        char m[48];
        snprintf(m, sizeof(m), "%.3f", mm);
        if (axis == "E") {
            std::optional<DevExtder> cur = ext ? ext->GetCurrentExtder() : std::nullopt;
            if (!cur || cur->GetCurrentTemp() < Rules::E_JOG_MIN_NOZZLE_C)
                return ControlPlan::refuse(WAB_ACTION_DISABLED,
                                           "heat the nozzle to " + itos(Rules::E_JOG_MIN_NOZZLE_C) + " C or more before moving the extruder (setNozzleTemp)",
                                           "nozzleCold");
            const int id = cur->GetExtId();
            if (obj->is_enable_np)
                return plan_with("Move the extruder by " + std::string(m) + " mm", "extruder " + itos(id) + " " + m,
                                 [id, mm](MachineObject* o) { return o->command_extruder_control(id, mm); });
            return plan_with("Move the extruder by " + std::string(m) + " mm", "E " + std::string(m),
                             [mm](MachineObject* o) { return o->command_axis_control("E", 1.0, mm, Rules::E_JOG_SPEED); });
        }
        // StatusPanel::on_axis_ctrl_xy / check_axis_z_at_home: an axis that lost its home is
        // re-homed first, never jogged blind.
        if (!obj->is_axis_at_home(axis))
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "the " + axis + " axis is not homed; home first (action home)", "notHomed");
        const int speed = axis == "Z" ? Rules::Z_JOG_SPEED : Rules::XY_JOG_SPEED;
        return plan_with("Move " + axis + " by " + m + " mm", axis + " " + m + " F" + itos(speed),
                         [axis, mm, speed](MachineObject* o) { return o->command_axis_control(axis, 1.0, mm, speed); });
    }
    if (action == "selectExtruder") {
        int id = -1;
        if (ext == nullptr || ext->GetTotalExtderCount() < 2)
            return ControlPlan::refuse(WAB_UNSUPPORTED_TYPE, "this printer has one extruder");
        if (!get_int(p, "extruder", id) || (id != MAIN_EXTRUDER_ID && id != DEPUTY_EXTRUDER_ID))
            return ControlPlan::refuse(WAB_BAD_PARAMS, "extruder must be 0 (right) or 1 (left)");
        if (std::string why = Rules::select_extruder_refusal(obj->is_in_printing(), obj->is_in_printing_pause(),
                                                             obj->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE);
            !why.empty())
            return ControlPlan::refuse(WAB_BUSY, why, "busy");
        return plan_with(std::string("Switch to the ") + (id == MAIN_EXTRUDER_ID ? "right" : "left") + " extruder", "select_extruder " + itos(id),
                         [id](MachineObject* o) { return o->GetCtrl() ? o->GetCtrl()->command_select_extruder(id) : -1; });
    }
    if (action == "loadFilament" || action == "unloadFilament") {
        const bool load = action == "loadFilament";
        int        ams_id = -1, slot = 0;
        if (load && (!get_int(p, "ams", ams_id) || ams_id < 0 || ams_id > 255))
            return ControlPlan::refuse(WAB_BAD_PARAMS, "ams must be an AMS id from slicer.printerStatus, or 255 / 254 for the external spool");
        if (!load && p.contains("ams") && !get_int(p, "ams", ams_id))
            return ControlPlan::refuse(WAB_BAD_PARAMS, "ams must be an integer");
        if (p.contains("slot") && (!get_int(p, "slot", slot) || slot < 0 || slot > 15))
            return ControlPlan::refuse(WAB_BAD_PARAMS, "slot must be the tray number in that AMS (0-3)");
        if (!load && ams_id < 0) {
            // Unload what the current extruder holds.
            std::optional<DevExtder> cur = ext ? ext->GetCurrentExtder() : std::nullopt;
            if (!cur || !cur->HasFilamentInExt())
                return ControlPlan::refuse(WAB_ACTION_DISABLED, "no filament is loaded", "notLoaded");
            ams_id = to_int(cur->GetSlotNow().ams_id);
            slot   = to_int(cur->GetSlotNow().slot_id, 0);
        }
        const bool        virtual_slot = devPrinterUtil::IsVirtualSlot(ams_id);
        const std::string ams_s = std::to_string(ams_id), slot_s = virtual_slot ? std::string("0") : std::to_string(slot);
        Rules::FilamentFacts f;
        f.printing               = obj->is_in_printing();
        f.can_resume             = obj->can_resume();
        f.virtual_slot           = virtual_slot;
        f.changing_filament      = is_switching_filament(obj);
        f.track_switch_installed = obj->GetFilaSwitch() && obj->GetFilaSwitch()->IsInstalled();
        f.track_switch_ready     = obj->GetFilaSwitch() && obj->GetFilaSwitch()->IsReady();
        int holder = -1; // the extruder that holds exactly this slot
        if (ext)
            for (const DevExtder& e : ext->GetExtruders())
                if (e.GetSlotNow().ams_id == ams_s && e.GetSlotNow().slot_id == slot_s)
                    holder = e.GetExtId();
        f.slot_loaded = holder >= 0 && !f.track_switch_installed;
        DevAmsTray* tray = virtual_slot ? nullptr : obj->get_ams_tray(ams_s, slot_s);
        f.slot_known        = tray != nullptr;
        f.slot_has_filament = tray != nullptr && tray->is_exists;
        if (std::string why = Rules::filament_refusal(f, load); !why.empty())
            return ControlPlan::refuse(f.printing || f.changing_filament ? WAB_BUSY : WAB_ACTION_DISABLED, why, "filamentState");
        if (!load) {
            // StatusPanel::on_ams_unload: on NP printers only the extruder holding that slot unloads.
            if (obj->is_enable_np && holder < 0)
                return ControlPlan::refuse(WAB_ACTION_DISABLED, "no extruder holds that slot", "notLoaded");
            return plan_with("Unload the filament of " + std::string(virtual_slot ? "the external spool" : "AMS " + ams_s), "unload " + ams_s,
                             [ams_s](MachineObject* o) { return o->command_ams_change_filament(false, ams_s, "255"); });
        }
        // StatusPanel::on_ams_load_curr. A Filament Track Switch routes the load to the extruder
        // the person picks (FeedDirectionDialog); the page names it.
        std::optional<int> extruder_id;
        if (f.track_switch_installed) {
            int e = -1;
            if (!get_int(p, "extruder", e) || (e != MAIN_EXTRUDER_ID && e != DEPUTY_EXTRUDER_ID))
                return ControlPlan::refuse(WAB_BAD_PARAMS, "this printer's Filament Track Switch needs extruder: 0 (right) or 1 (left)");
            if (holder == e)
                return ControlPlan::refuse(WAB_ACTION_DISABLED, "that extruder already holds this filament", "alreadyLoaded");
            extruder_id = e;
        } else if (p.contains("extruder")) {
            return ControlPlan::refuse(WAB_BAD_PARAMS, "extruder applies only to printers with a Filament Track Switch");
        }
        std::string where = virtual_slot ? "the external spool" : "AMS " + ams_s + ", slot " + slot_s;
        if (virtual_slot) {
            const size_t vt = ams_id == VIRTUAL_TRAY_DEPUTY_ID ? 1 : 0;
            if (vt >= obj->vt_slot.size())
                return ControlPlan::refuse(WAB_NOT_FOUND, "this printer has no such external spool");
            const int  t     = Rules::tray_mid_temp(obj->vt_slot[vt].nozzle_temp_min, obj->vt_slot[vt].nozzle_temp_max);
            const bool np    = obj->is_enable_np || obj->is_enable_ams_np;
            const std::string target = np ? ams_s : std::string("254");
            return plan_with("Load filament from " + where, "load " + target + " 0", [target, t, extruder_id](MachineObject* o) {
                return o->command_ams_change_filament(true, target, "0", t, t, extruder_id);
            });
        }
        DevAmsTray* curr = obj->get_curr_tray();
        const int   old_t = curr ? Rules::tray_mid_temp(curr->nozzle_temp_min, curr->nozzle_temp_max) : -1;
        const int   new_t = curr ? Rules::tray_mid_temp(tray->nozzle_temp_min, tray->nozzle_temp_max) : -1;
        return plan_with("Load filament from " + where, "load " + ams_s + " " + slot_s, [ams_s, slot_s, old_t, new_t, extruder_id](MachineObject* o) {
            return o->command_ams_change_filament(true, ams_s, slot_s, old_t, new_t, extruder_id);
        });
    }
    if (action == "retryFilamentChange") {
        if (!is_loading_filament(obj) && !(ext && ext->CanRetrySwitching()))
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "no filament change or extruder switch is waiting for a retry", "nothingToRetry");
        return plan_with("Retry the filament change", "ams resume", [](MachineObject* o) { return o->command_ams_control("resume"); });
    }
    if (action == "abortFilamentChange") {
        const bool can = (is_loading_filament(obj) &&
                          (obj->is_support_fila_change_abort || DevPrinterConfigUtil::support_ams_fila_change_abort(obj->printer_type))) ||
                         (ext && ext->CanQuitSwitching());
        if (!can)
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "no filament change or extruder switch can be stopped now", "nothingToAbort");
        return plan_with("Stop the filament change", "ams abort", [](MachineObject* o) { return o->command_ams_control("abort"); });
    }
    if (action == "refreshRfid") {
        int ams_id = -1, slot = -1;
        if (!get_int(p, "ams", ams_id) || !get_int(p, "slot", slot))
            return ControlPlan::refuse(WAB_BAD_PARAMS, "refreshRfid needs ams and slot");
        if (devPrinterUtil::IsVirtualSlot(ams_id))
            return ControlPlan::refuse(WAB_UNSUPPORTED_TYPE, "the external spool has no RFID reader");
        DevAms* ams = obj->GetFilaSystem() ? obj->GetFilaSystem()->GetAmsById(std::to_string(ams_id)) : nullptr;
        if (ams == nullptr || ams->GetTray(std::to_string(slot)) == nullptr)
            return ControlPlan::refuse(WAB_NOT_FOUND, "no such AMS slot; read slicer.printerStatus");
        // StatusPanel::on_ams_refresh_rfid
        const bool np = obj->is_enable_np || obj->is_enable_ams_np;
        bool       loaded = false;
        if (np)
            loaded = ams->GetExtruderId() < ext->GetTotalExtderSize() && ext->HasFilamentInExt(ams->GetExtruderId());
        else
            loaded = obj->is_filament_at_extruder();
        if (loaded)
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "the filament is loaded in the tool head; unload it first, then read the tag",
                                       "filamentLoaded");
        return plan_with("Read the RFID tag of AMS " + itos(ams_id) + ", slot " + itos(slot), "rfid " + itos(ams_id) + " " + itos(slot),
                         [ams_id, slot](MachineObject* o) { return o->command_ams_refresh_rfid(ams_id, slot); });
    }
    if (action == "dryAms")
        return plan_dry(obj, p);
    if (action == "stopDryingAms") {
        int ams_id = -1;
        if (!get_int(p, "ams", ams_id))
            return ControlPlan::refuse(WAB_BAD_PARAMS, "stopDryingAms needs ams");
        DevAms* ams = obj->GetFilaSystem() ? obj->GetFilaSystem()->GetAmsById(std::to_string(ams_id)) : nullptr;
        if (ams == nullptr || !ams->IsSupportRemoteDry(obj))
            return ControlPlan::refuse(WAB_NOT_FOUND, "no AMS " + itos(ams_id) + " that dries from the slicer");
        auto s = ams->GetDryStatus();
        if (!s || *s == DevAms::DryStatus::Off || *s == DevAms::DryStatus::Cooling)
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "this AMS is not drying", "notDrying");
        return plan_with("Stop drying AMS " + itos(ams_id), "stop drying " + itos(ams_id), [ams_id](MachineObject* o) {
            return o->GetFilaSystem() ? o->GetFilaSystem()->CtrlAmsStopDrying(ams_id) : -1;
        });
    }
    if (action == "errorAction") {
        const int err = obj->print_error;
        if (err == 0)
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "the printer reports no error", "noError");
        const std::string code = get_string(p, "code"), name = get_string(p, "action");
        if (code != MachineObject::get_error_code_str(err))
            return ControlPlan::refuse(WAB_ACTION_DISABLED, "the printer's error is now " + MachineObject::get_error_code_str(err) +
                                                                "; read slicer.printerStatus again", "errorChanged");
        int button = -1;
        for (int b : error_buttons(obj, err))
            if (const char* n = Rules::error_action_name(b); n != nullptr && name == n)
                button = b;
        if (button < 0)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "\"" + name + "\" is not one of this error's actions (slicer.printerStatus error.actions)");
        const std::string err_s = std::to_string(err);
        // DeviceErrorDialog::on_button_click
        std::function<int(MachineObject*)> run = [button, err, err_s](MachineObject* o) -> int {
            switch (button) {
            case 2: case 3: case 4: case 12: case 28: return o->command_hms_resume(err_s, o->job_id_);
            case 5: return o->command_hms_stop(err_s, o->job_id_);
            case 7: case 10001: return o->command_ams_control("done");
            case 8: case 9: case 34: case 10002: return o->command_ams_control("resume");
            case 11: return o->command_clean_print_error(o->subtask_id_, err);
            case 10004: {
                const int rc = o->command_clean_print_error(o->subtask_id_, err);
                o->command_clean_print_error_uiop(err);
                return rc;
            }
            case 23: return o->command_hms_idle_ignore(err_s, 0);
            case 25: case 27: return o->command_hms_ignore(err_s, o->job_id_);
            case 29: return o->command_stop_buzzer();
            case 35: return o->command_ams_drying_stop();
            case 24: return o->command_refresh_nozzle();
            case 51: return o->command_ams_control("abort");
            case 54: return o->command_purification_disable();
            default: return -1;
            }
        };
        return plan_with("Answer printer error " + code + ": " + name, "error " + code + " " + name, run);
    }
    if (action == "gcode") {
        std::vector<std::string> lines, kept;
        const std::string        text = get_string(p, "lines");
        boost::split(lines, text, boost::is_any_of("\n"));
        for (std::string& l : lines) {
            boost::trim(l);
            if (!l.empty())
                kept.push_back(l);
        }
        if (kept.empty() || int(kept.size()) > MAX_GCODE_LINES)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "lines must hold 1 to " + itos(MAX_GCODE_LINES) + " G-code lines");
        const std::string joined = boost::algorithm::join(kept, "\n");
        return plan_with("Send " + itos(int(kept.size())) + " line(s) of G-code", joined,
                         [joined](MachineObject* o) { return o->publish_gcode(joined + "\n"); });
    }
    return ControlPlan::refuse(WAB_BAD_PARAMS, "unknown action \"" + action + "\"");
}

ControlPlan plan_rrf_control(const std::string& action, const json& p)
{
    auto lines = [](std::string summary, std::vector<std::string> rrf) {
        ControlPlan plan;
        plan.summary = std::move(summary);
        plan.rrf     = std::move(rrf);
        plan.command = boost::algorithm::join(plan.rrf, "\n");
        return plan;
    };
    if (action == "pause" || action == "stop") {
        ControlPlan plan = action == "pause" ? lines("Pause the print", {"M25"}) : lines("Stop the print", {"M25", "M0"});
        plan.immediate   = true;
        return plan;
    }
    if (action == "resume")
        return lines("Resume the print", {"M24"});
    if (action == "setBedTemp" || action == "setNozzleTemp" || action == "setChamberTemp") {
        const int limit = action == "setBedTemp" ? RRF_MAX_BED_C : action == "setChamberTemp" ? RRF_MAX_CHAMBER_C : RRF_MAX_NOZZLE_C;
        double    c     = 0;
        if (!get_number(p, "celsius", c) || c < 0 || c > limit)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "celsius must be a number from 0 to " + itos(limit));
        const int t = int(c);
        if (action == "setBedTemp")
            return lines("Set the bed to " + itos(t) + " C", {"M140 S" + itos(t)});
        if (action == "setChamberTemp")
            return lines("Set the chamber to " + itos(t) + " C", {"M141 S" + itos(t)});
        int nozzle = 0;
        if (p.contains("nozzle") && (!get_int(p, "nozzle", nozzle) || nozzle < 0 || nozzle > 15))
            return ControlPlan::refuse(WAB_BAD_PARAMS, "nozzle must be a tool number");
        return lines("Set nozzle " + itos(nozzle) + " to " + itos(t) + " C", {"G10 P" + itos(nozzle) + " S" + itos(t)});
    }
    if (action == "setFan") {
        double percent = 0;
        const std::string fan = get_string(p, "fan").empty() ? std::string("part") : get_string(p, "fan");
        if (!get_number(p, "percent", percent) || percent < 0 || percent > 100)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "percent must be 0 to 100");
        if (fan != "part" || p.contains("fanId"))
            return ControlPlan::refuse(WAB_UNSUPPORTED_TYPE, "only the part fan (P0) can be set on this printer");
        char s[16];
        snprintf(s, sizeof(s), "%.2f", percent / 100.0);
        return lines("Set the part fan to " + itos(int(percent)) + "%", {std::string("M106 P0 S") + s});
    }
    if (action == "setSpeed") {
        int level = 0;
        if (!get_int(p, "level", level) || level < 1 || level > 4)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "level must be 1 (silent) to 4 (ludicrous)");
        return lines("Set the print speed level to " + itos(level), {"M220 S" + itos(Rules::rrf_speed_percent(level))});
    }
    if (action == "home")
        return lines("Home all axes", {"G28"});
    if (action == "jog") {
        const std::string axis = boost::to_upper_copy(get_string(p, "axis"));
        double            mm = 0, speed = 0;
        if (axis != "X" && axis != "Y" && axis != "Z" && axis != "E")
            return ControlPlan::refuse(WAB_BAD_PARAMS, "axis must be X, Y, Z or E (rotary axes cannot be jogged from here)");
        if (!get_number(p, "mm", mm) || std::abs(mm) > 100 || mm == 0)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "mm must be a non-zero number from -100 to 100");
        if (!get_number(p, "speed", speed) || speed <= 0)
            speed = axis == "E" ? 300 : axis == "Z" ? Rules::Z_JOG_SPEED : Rules::XY_JOG_SPEED;
        char s[64];
        snprintf(s, sizeof(s), "G1 %s%.3f F%d", axis.c_str(), mm, int(speed));
        char m[48];
        snprintf(m, sizeof(m), "%.3f mm", mm);
        return lines("Move " + axis + " by " + m, {"G91", s, "G90"});
    }
    if (action == "gcode") {
        std::vector<std::string> raw, kept;
        boost::split(raw, get_string(p, "lines"), boost::is_any_of("\n"));
        for (std::string& l : raw) {
            boost::trim(l);
            if (!l.empty())
                kept.push_back(l);
        }
        if (kept.empty() || int(kept.size()) > MAX_GCODE_LINES)
            return ControlPlan::refuse(WAB_BAD_PARAMS, "lines must hold 1 to " + itos(MAX_GCODE_LINES) + " G-code lines");
        return lines("Send " + itos(int(kept.size())) + " line(s) of G-code", kept);
    }
    static const std::set<std::string> bambu_only = {"setLight", "setAirductMode", "selectExtruder", "loadFilament", "unloadFilament",
                                                     "retryFilamentChange", "abortFilamentChange", "refreshRfid", "dryAms",
                                                     "stopDryingAms", "errorAction"};
    if (bambu_only.count(action) != 0)
        return ControlPlan::refuse(WAB_UNSUPPORTED_TYPE, action + " is supported on Bambu printers only");
    return ControlPlan::refuse(WAB_BAD_PARAMS, "unknown action \"" + action + "\"");
}

}}} // namespace Slic3r::GUI::WebAppPrinters
