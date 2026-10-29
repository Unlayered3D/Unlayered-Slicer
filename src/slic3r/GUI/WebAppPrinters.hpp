#ifndef slic3r_GUI_WebAppPrinters_hpp_
#define slic3r_GUI_WebAppPrinters_hpp_

// What slicer.printerStatus reports and what slicer.printerControl does on a printer
// (WEB_APP_BRIDGE.md 4.6), kept out of WebAppBridge.cpp: the Bambu side reads the MachineObject the
// Device tab shows and sends the same commands its controls send, refusing in the same states;
// the RepRapFirmware side builds the G-code lines a Duet / Unlayered host gets. The bridge owns
// the permission, the printer ids, the native confirmation and the event; this file owns the
// rules (pure ones in Actions/PrinterRules.hpp).

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
class MachineObject;
class DuetRrfClient;
namespace GUI { namespace WebAppPrinters {

// idle / printing / paused / finished / failed / offline / unknown.
std::string bambu_state(MachineObject* obj);

// Everything the Device tab shows about a Bambu (or printer-agent) printer, without addresses,
// access codes or serial numbers.
nlohmann::json bambu_status(MachineObject* obj);

// State, progress and temperatures from a connected RepRapFirmware client's object model. Fills
// `error` and returns false when the printer does not answer.
bool rrf_status(DuetRrfClient& client, nlohmann::json& out, std::string& error);

// One printerControl request, checked and described before anything is sent.
struct ControlPlan
{
    int         code = 0;  // WebAppBridgeError: 0 = go ahead
    std::string message;   // why not, and what to do instead
    std::string reason;    // a short slug for data.reason on a refusal
    std::string summary;   // the action, for the confirmation ("Set nozzle 1 to 220 C")
    std::string command;   // what will be sent, as text: the event's `command`
    std::string warning;   // an extra paragraph the confirmation shows (the Device tab's own warning)
    bool        immediate = false; // pause / stop: no confirmation
    // Bambu: sends it (re-planned against the live printer after the confirmation). 0 = sent.
    std::function<int(MachineObject*)> run;
    // RepRapFirmware: the lines, in order.
    std::vector<std::string> rrf;

    static ControlPlan refuse(int code, std::string message, std::string reason = {});
};

// The Bambu actions: pause, stop, resume, setBedTemp, setNozzleTemp, setChamberTemp, setFan,
// setAirductMode, setSpeed, setLight, home, jog, selectExtruder, loadFilament, unloadFilament,
// retryFilamentChange, abortFilamentChange, refreshRfid, dryAms, stopDryingAms, errorAction, gcode.
ControlPlan plan_bambu_control(MachineObject* obj, const std::string& action, const nlohmann::json& params);

// The RepRapFirmware actions: pause, stop, resume, setBedTemp, setNozzleTemp, setChamberTemp,
// setFan (part fan, P0), setSpeed, home, jog, gcode. Anything else answers 1009.
ControlPlan plan_rrf_control(const std::string& action, const nlohmann::json& params);

}}} // namespace Slic3r::GUI::WebAppPrinters

#endif // slic3r_GUI_WebAppPrinters_hpp_
