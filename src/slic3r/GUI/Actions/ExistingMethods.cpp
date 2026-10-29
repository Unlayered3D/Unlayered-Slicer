// Descriptors of the typed slicer.* methods WebAppBridge.cpp (and the branches that add to it)
// serves itself. They carry a bridge_method and no runner: slicer.invokeAction refuses them
// ("call the method directly"), and slicer.listActions shows each only while the method is in the
// tab's capabilities -- so a method from a branch that is not merged, or one behind a permission
// that is off, is never advertised. Params are documentation here (the methods validate their own);
// they stay inside the dialect, so free-form maps are described in prose.
// Owner: S1 (actions core).

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "Register.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

namespace {

struct Existing
{
    const char*              id;
    const char*              method;
    const char*              label;
    const char*              summary;
    Risk                     risk;
    Gate                     gate;
    std::vector<std::string> effects;
    const char*              properties; // JSON object of property schemas
    std::vector<std::string> ui;
    const char*              since;
    // Non-null: how the method is confirmed instead of (or as) the catalog's native confirmation.
    const char* skip_rule;
    // Non-null: JSON array of the required property names.
    const char* required = nullptr;
};

const char* const OWN_CONFIRM = "the method shows its own native confirmation";

} // namespace

void register_existing_methods(Catalog& c)
{
    const std::vector<Existing> methods = {
        // --- project and files
        {"project.openUrl", "slicer.openProjectFromUrl", "Open from URL",
         "Download a 3MF/STL/STEP/OBJ/AMF/G-code from an allowed host and open or import it.", Risk::Mutate, Gate::Settings,
         {"geometry", "structure", "plates", "project"},
         R"({"url":{"type":"string","minLength":8,"maxLength":4096},"filename":{"type":"string","maxLength":255},
             "sha256":{"type":"string","minLength":64,"maxLength":64},"printer":{"type":"string","maxLength":200}})",
         {"MainFrame.cpp::menu::Import 3MF/STL/STEP/SVG/OBJ/AMF", "GLCanvas3D.cpp::toolbar::add"}, "0.1.0", nullptr},
        {"project.openLocal", "slicer.openProject", "Open local file", "Open a local project or model file (loopback pages only).",
         Risk::Destructive, Gate::Settings, {"geometry", "structure", "plates", "project"},
         R"({"path":{"type":"string","minLength":1,"maxLength":4096}})", {"MainFrame.cpp::menu::Open Project"}, "0.1.0", OWN_CONFIRM},
        // --- presets and settings
        {"preset.select", "slicer.selectPreset", "Select preset",
         "Select an installed printer, process or filament preset (by name or alias); allowHidden enables a hidden system preset "
         "first, as ticking it in the filament or printer selection would.",
         Risk::Mutate, Gate::Settings, {"presets", "settings"},
         R"({"type":{"type":"string","enum":["printer","print","filament"]},"name":{"type":"string","minLength":1,"maxLength":200},
             "slot":{"type":"integer","minimum":0,"maximum":63},"allowHidden":{"type":"boolean"},
             "ifDirty":{"type":"string","enum":["ask","discard","prompt","save","transfer"]}})",
         {"Plater.cpp::sidebar::sidebar"}, "0.1.0", nullptr, R"(["name"])"},
        {"preset.list", "slicer.listPresets", "List presets", "List installed presets of one type (optionally compatible only, filtered), with filamentId, compatiblePrinters, "
         "isProjectEmbedded and isFromBundle for each.",
         Risk::Read, Gate::Settings, {},
         R"({"type":{"type":"string","enum":["printer","print","filament"]},"query":{"type":"string","maxLength":200},
             "compatibleOnly":{"type":"boolean"},"includeHidden":{"type":"boolean"},"limit":{"type":"integer","minimum":1,"maximum":500}})",
         {}, "0.1.5", nullptr},
        {"settings.schema", "slicer.getSettingsSchema", "Settings schema",
         "Describe settings: label, type, range, enum values, category, current value; perVariant settings and the "
         "variants list (which value index each extruder edits) on multi-extruder printers.", Risk::Read, Gate::Settings, {},
         R"({"type":{"type":"string","enum":["printer","print","filament"]},"keys":{"type":"array","maxItems":200,"items":{"type":"string"}},
             "category":{"type":"string","maxLength":100},"query":{"type":"string","maxLength":200},"includeCode":{"type":"boolean"},
             "limit":{"type":"integer","minimum":1,"maximum":2000}})",
         {"Tab.cpp::tabbutton::m_btn_search"}, "0.1.5", nullptr},
        {"settings.get", "slicer.getSettings", "Read settings", "Read setting values of the edited print, filament (per slot) or printer preset.",
         Risk::Read, Gate::Settings, {},
         R"({"type":{"type":"string","enum":["printer","print","filament"]},"keys":{"type":"array","maxItems":500,"items":{"type":"string"}},
             "slot":{"type":"integer","minimum":0,"maximum":63},"includeCode":{"type":"boolean"},
             "extruder":{"type":"integer","minimum":0,"maximum":15,"description":"Per-variant settings: that extruder's value only."}})",
         {}, "0.1.5", nullptr},
        {"settings.set", "slicer.setSettings", "Change settings",
         "Change print, filament or printer settings as unsaved edits. Also takes values: {key: value} (1 to 64 entries).", Risk::Mutate,
         Gate::Settings, {"settings"},
         R"({"type":{"type":"string","enum":["printer","print","filament"]},"slot":{"type":"integer","minimum":0,"maximum":63},
             "extruder":{"type":"integer","minimum":0,"maximum":15,"description":"Per-variant settings: write that extruder's value only."},
             "ifDirty":{"type":"string","enum":["ask","discard","prompt","save","transfer"]}})",
         {}, "0.1.5", nullptr},
        // --- the plate (typed object edits)
        {"plate.get", "slicer.getPlate", "Read the plate", "Plates, objects (index, name, bbox, instances) and project state.", Risk::Read,
         Gate::Settings, {}, "{}", {}, "0.1.5", nullptr},
        {"object.setCutPlanes", "slicer.setCutPlanes", "Set cut planes",
         "Declarative multiplanar cut planes on one object, with each plane's build direction and the print order (undoable). "
         "On a virtual 5-axis printer planes and build directions are held at its tilt limit, as the Cut Planes tool does.",
         Risk::Mutate, Gate::Settings, {"geometry", "settings"},
         R"({"object":{"type":"string","description":"index or exact name"},
             "enabled":{"type":"boolean","description":"Default: on when planes are given. planes [] with mode replace clears everything, enabled included."},
             "userOrdered":{"type":"boolean","description":"true: the list order decides the print order (the first plane prints last), as unticking Automatic order and reordering in the tool. false: automatic order."},
             "frame":{"type":"string","enum":["world","object"]},"mode":{"type":"string","enum":["replace","append"]},
             "planes":{"type":"array","maxItems":64,"items":{"type":"object","additionalProperties":false,"properties":{
               "point":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
               "normal":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
               "enabled":{"type":"boolean","description":"false keeps the plane but ignores it when slicing."},
               "buildNormal":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"},
                 "description":"Build direction of the plane's far side, in frame. null resets it to the plane normal. Absent: a replace keeps the override of an unchanged plane."},
               "buildB":{"type":"number","minimum":-180,"maximum":0,"description":"Build direction as machine angle B (tilt about Y), degrees; with buildC."},
               "buildC":{"type":"number","minimum":-180,"maximum":180,"description":"Machine angle C (about Z), degrees."}}}}})",
         {"GLGizmosManager.cpp::gizmo::GLGizmoCutPlanes"}, "0.1.5", nullptr},
        {"object.setOrientation", "slicer.setOrientation", "Orient object",
         "Put a face down or rotate an object by degrees, then drop it on the bed.", Risk::Mutate, Gate::Settings, {"geometry"},
         R"({"object":{"type":"string","description":"index or exact name"},
             "face":{"type":"string","enum":["+x","-x","+y","-y","+z","-z"],"description":"The side (as it sits now) that goes down onto the bed: +x = the side facing +X; +z turns it upside down; -z changes nothing."},
             "rotation":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}}})",
         {"GLGizmosManager.cpp::gizmo::GLGizmoRotate3D"}, "0.1.5", nullptr},
        {"object.setFilament", "slicer.setObjectFilament", "Set object filament", "Assign an object to a filament slot.", Risk::Mutate,
         Gate::Settings, {"settings", "structure"},
         R"({"object":{"type":"string","description":"index or exact name"},"filament":{"type":"integer","minimum":1,"maximum":64}})", {},
         "0.1.5", nullptr},
        {"object.setScale", "slicer.setScale", "Scale object", "Scale an object uniformly by a factor, to a size, or to fit the build volume.",
         Risk::Mutate, Gate::Settings, {"geometry"},
         R"({"object":{"type":"string","description":"index or exact name"},"factor":{"type":"number","minimum":0.001,"maximum":1000},
             "sizeMm":{"type":"object","additionalProperties":false,"required":["axis","mm"],"description":"Uniform scale so this extent becomes mm.",
               "properties":{"axis":{"type":"string","enum":["x","y","z","max"]},"mm":{"type":"number","minimum":0.001,"maximum":100000}}},
             "fit":{"type":"boolean"}})",
         {"GLGizmosManager.cpp::gizmo::GLGizmoScale3D", "GUI_Factories.cpp::menu::Scale to build volume"}, "0.1.5", nullptr},
        {"object.move", "slicer.moveObject", "Move object", "Move an object (or one instance) to a position or by an offset.", Risk::Mutate,
         Gate::Settings, {"geometry", "plates"},
         R"({"object":{"type":"string","description":"index or exact name"},"instance":{"type":"integer","minimum":0},
             "to":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number"},"description":"[x, y] of the bounding-box centre, mm; Z never changes."},
             "by":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number"},"description":"[dx, dy] mm."},"center":{"type":"boolean"}})",
         {"GLGizmosManager.cpp::gizmo::GLGizmoMove3D"}, "0.1.5", nullptr},
        {"plate.arrange", "slicer.arrange", "Arrange",
         "Arrange the objects on the current plate, one plate or all plates, with this run's spacing, rotation, mixed-material, "
         "calibration-area and Y-alignment options.", Risk::Mutate, Gate::Settings,
         {"geometry", "plates"},
         R"({"all":{"type":"boolean"},"plate":{"type":"integer","minimum":0},"allowRotation":{"type":"boolean"},
             "spacingMm":{"type":"number","minimum":0,"maximum":100},
             "allowMultiMaterials":{"type":"boolean","description":"Objects of different filaments may share a plate (default on)."},
             "avoidCalibrationRegion":{"type":"boolean","description":"Keep clear of the extrusion calibration area (first-layer-scanning printers only)."},
             "alignToYAxis":{"type":"boolean","description":"Align objects to the Y axis; needs allowRotation false."}})",
         {"GLCanvas3D.cpp::toolbar::arrange", "GUI_Factories.cpp::menu::Arrange", "Plater.cpp::bedicon::hover:3"}, "0.1.5", nullptr},
        {"object.setCopies", "slicer.setCopies", "Set number of instances", "Set how many instances an object has.", Risk::Mutate,
         Gate::Settings, {"structure", "plates"},
         R"({"object":{"type":"string","description":"index or exact name"},"count":{"type":"integer","minimum":1,"maximum":500},
             "arrange":{"type":"boolean"}})",
         {"GLCanvas3D.cpp::toolbar::more", "GLCanvas3D.cpp::toolbar::fewer", "GUI_Factories.cpp::menu::Set number of instances"}, "0.1.5",
         nullptr},
        {"object.fillBed", "slicer.fillBed", "Fill bed with instances", "Fill the plate with instances of an object.", Risk::Mutate,
         Gate::Settings, {"structure", "plates"}, R"({"object":{"type":"string","description":"index or exact name"}})",
         {"GUI_Factories.cpp::menu::Fill bed with instances"}, "0.1.5", nullptr},
        // --- slicing
        {"slice.run", "slicer.slice", "Slice plate", "Slice a plate (the current one by default) and wait for the result.", Risk::Mutate,
         Gate::Settings, {"plates"}, R"({"plate":{"type":"integer","minimum":0}})", {"MainFrame.cpp::sidebutton::Slice plate"}, "0.1.5",
         nullptr},
        {"slice.result", "slicer.getSliceResult", "Slice result", "Print time, filament use and warnings of a sliced plate, and whether it is a slicing-service result or waits for the person's Slice.", Risk::Read,
         Gate::Settings, {}, R"({"plate":{"type":"integer","minimum":0}})", {}, "0.1.5", nullptr},
        {"slice.issues", "slicer.getSliceIssues", "Slice issues", "Why a plate will not slice, or what the slicer warned about.", Risk::Read,
         Gate::Settings, {}, R"({"plate":{"type":"integer","minimum":0}})", {}, "0.2.0", nullptr},
        {"slice.modes", "slicer.getSliceModes", "Slicing modes", "The slicing modes this slicer offers, which one is selected, and each plate's slicing-service job/result state.", Risk::Read,
         Gate::Settings, {}, "{}", {"MainFrame.cpp::sidebutton::member:m_slice_option_btn"}, "0.2.0", nullptr},
        {"slice.setMode", "slicer.setSliceMode", "Select slicing mode",
         "Preselect a slicing mode for the next slice. Never starts a run and sends nothing to the service: a slicing-service mode "
         "slices, and prepares in the background, only once the person presses Slice.",
         Risk::Ui, Gate::Settings, {"settings"}, R"({"id":{"type":"string","minLength":1,"maxLength":100}})",
         {"MainFrame.cpp::sidebutton::expr:label"}, "0.2.0", nullptr},
        // --- export family
        {"export.project", "slicer.exportProject", "Export project", "Export the plate as a 3MF the page can read back by handle.",
         Risk::OutwardFile, Gate::Export, {"file"}, R"({"plate":{"type":"integer","minimum":-1}})", {}, "0.1.2",
         "the export permission is the consent; the file stays in the slicer's cache"},
        {"export.readChunk", "slicer.readExportChunk", "Read export chunk", "Read part of an export by handle.", Risk::Read, Gate::Export, {},
         R"({"handle":{"type":"string","maxLength":100},"offset":{"type":"integer","minimum":0},"length":{"type":"integer","minimum":1}})",
         {}, "0.1.2", nullptr},
        {"export.discard", "slicer.discardExport", "Discard export", "Delete an export by handle.", Risk::Ui, Gate::Export, {},
         R"({"handle":{"type":"string","maxLength":100}})", {}, "0.1.2", nullptr},
        {"export.thumbnail", "slicer.exportThumbnail", "Plate picture", "A PNG picture of a plate.", Risk::Read, Gate::Export, {},
         R"({"plate":{"type":"integer","minimum":-1}})", {}, "0.1.2", nullptr},
        {"export.printerPreset", "slicer.exportPrinterPreset", "Share printer profile",
         "Export the edited printer preset (no secrets) by handle.", Risk::OutwardFile, Gate::Export, {"file"}, "{}", {}, "0.1.3",
         "the export permission is the consent; secrets are stripped"},
        {"preset.installPrinter", "slicer.installPrinterPreset", "Install printer profile",
         "Download a shared printer profile and install it after the person confirms in the slicer.", Risk::OutwardNetwork, Gate::Install,
         {"presets"},
         R"({"url":{"type":"string","minLength":8,"maxLength":4096},"filename":{"type":"string","maxLength":255},
             "sha256":{"type":"string","minLength":64,"maxLength":64},"printer":{"type":"string","maxLength":200}})",
         {}, "0.1.3", OWN_CONFIRM},
        {"handoff.request", "slicer.requestHandoff", "Hand over the plate", "Export a plate (the current one by default) as a 3MF this page reads back by handle: a snapshot of what is on the plate.",
         Risk::OutwardFile, Gate::Settings, {"file"}, R"({"plate":{"type":"integer","minimum":-1}})", {}, "0.1.4",
         "the plate stays in the slicer's cache, readable only by handle"},
        // --- printers
        {"printer.list", "slicer.listPrinters", "List printers", "Printers this slicer prints to (opaque ids).", Risk::Read, Gate::Printer, {},
         "{}", {}, "0.1.5", nullptr},
        {"printer.status", "slicer.printerStatus", "Printer status",
         "What the Device tab shows for one printer: job, progress, temperatures and limits, fans, light, extruders, AMS units and "
         "trays, storage, firmware, HMS items and the current error with the actions it offers.",
         Risk::Read, Gate::Printer, {}, R"({"printer":{"type":"string","maxLength":200}})", {}, "0.1.5", nullptr},
        {"printer.exportGcode", "slicer.exportGcode", "Export G-code",
         "Export the sliced plate's G-code, or (format 3mf) the sliced .gcode.3mf of the plate or of all plates, through the native "
         "Save dialog; slicer.exportFinished reports the outcome.",
         Risk::OutwardFile, Gate::Printer, {"file"},
         R"({"plate":{"type":"string","enum":["current","all"]},"format":{"type":"string","enum":["gcode","3mf"]}})",
         {"MainFrame.cpp::sidebutton::Export G-code file", "MainFrame.cpp::sidebutton::Export plate sliced file",
          "MainFrame.cpp::sidebutton::Export all sliced file", "MainFrame.cpp::menu::Export G-code",
          "MainFrame.cpp::menu::Export plate sliced file", "MainFrame.cpp::menu::Export all plate sliced file"},
         "0.1.5", OWN_CONFIRM},
        {"printer.print", "slicer.print", "Print",
         "Open the print dialog for the sliced plate (or all plates) on a printer, optionally preset; the person presses Print.",
         Risk::OutwardPrinter, Gate::Printer, {"printer"},
         R"({"printer":{"type":"string","maxLength":200},"plate":{"type":"string","enum":["current","all"]},
             "options":{"type":"object","additionalProperties":false,"properties":{
               "timelapse":{"type":"string","enum":["on","off","auto"]},"bedLeveling":{"type":"string","enum":["on","off","auto"]},
               "flowCalibration":{"type":"string","enum":["on","off","auto"]},
               "nozzleOffsetCalibration":{"type":"string","enum":["on","off","auto"]},
               "sharedPaProfile":{"type":"string","enum":["on","off","auto"]},"jobName":{"type":"string","minLength":1,"maxLength":99},
               "timelapseStorage":{"type":"string","enum":["internal","external"]},"extChangeAssist":{"type":"boolean"}}},
             "upload":{"type":"object","additionalProperties":false,"properties":{
               "action":{"type":"string","enum":["upload","print"]},"path":{"type":"string","maxLength":200},
               "group":{"type":"string","maxLength":200},"storage":{"type":"string","maxLength":200}}}})",
         {"MainFrame.cpp::sidebutton::Print plate", "MainFrame.cpp::sidebutton::Print", "MainFrame.cpp::sidebutton::Print all"}, "0.1.5",
         OWN_CONFIRM},
        {"printer.pressPrint", "slicer.pressPrint", "Press Print",
         "Press the Print button as it is set (Print plate, Print all, Send, Export G-code ...): the slicer's own window opens "
         "for the selected printer preset and nothing leaves until the person confirms there. Needs a sliced plate.",
         Risk::OutwardPrinter, Gate::Settings, {"printer"}, "{}", {}, "0.2.0", OWN_CONFIRM},
        {"printer.send", "slicer.sendToPrinter", "Send to printer storage",
         "Open the Send dialog that saves the sliced plate (or all plates) to a Bambu printer's storage without printing.",
         Risk::OutwardPrinter, Gate::Printer, {"printer"},
         R"({"printer":{"type":"string","maxLength":200},"plate":{"type":"string","enum":["current","all"]}})",
         {"MainFrame.cpp::sidebutton::Send", "MainFrame.cpp::sidebutton::Send all"}, "0.2.0", OWN_CONFIRM},
        {"printer.select", "slicer.selectPrinter", "Select printer",
         "Select the Bambu printer the Device tab, AMS sync and the Print dialog follow (a LAN printer connects).", Risk::Ui,
         Gate::Printer, {"printer"}, R"({"printer":{"type":"string","maxLength":200}})", {"Monitor.cpp::device::on_select_printer"},
         "0.2.0", nullptr},
        {"printer.jobs", "slicer.printHostJobs", "Upload queue", "Uploads to print hosts this session: progress, state and errors.",
         Risk::Read, Gate::Printer, {}, "{}", {}, "0.2.0", nullptr},
        {"printer.cancelJob", "slicer.cancelPrintHostJob", "Cancel upload", "Cancel a print-host upload that has not finished.",
         Risk::OutwardPrinter, Gate::Printer, {"printer"}, R"({"id":{"type":"integer","minimum":0}})", {}, "0.2.0", OWN_CONFIRM},
        {"printer.control", "slicer.printerControl", "Printer control",
         "Pause, resume, stop, temperatures, fans and air-duct mode, light, speed, home and jog, extruder, filament load / unload / "
         "change retry, RFID, AMS drying, answering a printer error, raw G-code (confirmed natively except pause and stop).",
         Risk::OutwardPrinter, Gate::Printer, {"printer"},
         R"({"printer":{"type":"string","maxLength":200},
             "action":{"type":"string","enum":["pause","stop","resume","setBedTemp","setNozzleTemp","setChamberTemp","setFan",
               "setAirductMode","setSpeed","setLight","home","jog","selectExtruder","loadFilament","unloadFilament",
               "retryFilamentChange","abortFilamentChange","refreshRfid","dryAms","stopDryingAms","errorAction","gcode"]},
             "celsius":{"type":"number","minimum":0},"nozzle":{"type":"integer","minimum":0,"maximum":15},
             "fan":{"type":"string","enum":["part","aux","chamber"]},"fanId":{"type":"integer","minimum":0},
             "percent":{"type":"number","minimum":0,"maximum":100},"mode":{"type":"string","maxLength":20},
             "coolingFilter":{"type":"boolean"},"level":{"type":"integer","minimum":1,"maximum":4},"on":{"type":"boolean"},
             "axis":{"type":"string","enum":["X","Y","Z","E","x","y","z","e"]},"mm":{"type":"number","minimum":-100,"maximum":100},
             "speed":{"type":"number","minimum":1},"extruder":{"type":"integer","minimum":0,"maximum":1},
             "ams":{"type":"integer","minimum":0,"maximum":255},"slot":{"type":"integer","minimum":0,"maximum":15},
             "filament":{"type":"string","maxLength":200},"hours":{"type":"integer","minimum":1,"maximum":24},"rotate":{"type":"boolean"},
             "code":{"type":"string","maxLength":20},"lines":{"type":"string","maxLength":4000}})",
         {"StatusPanel.cpp::device::on_subtask_pause_resume", "StatusPanel.cpp::device::on_subtask_abort",
          "StatusPanel.cpp::device::on_nozzle_fan_switch",
          "StatusPanel.cpp::device::on_switch_speed", "StatusPanel.cpp::device::on_lamp_switch",
          "StatusPanel.cpp::device::on_axis_ctrl_xy", "StatusPanel.cpp::device::on_axis_ctrl_z_up_10",
          "StatusPanel.cpp::device::on_axis_ctrl_z_up_1", "StatusPanel.cpp::device::on_axis_ctrl_z_down_1",
          "StatusPanel.cpp::device::on_axis_ctrl_z_down_10", "StatusPanel.cpp::device::on_axis_ctrl_e_up_10",
          "StatusPanel.cpp::device::on_axis_ctrl_e_down_10", "StatusPanel.cpp::device::on_nozzle_selected",
          "StatusPanel.cpp::device::on_ams_load", "StatusPanel.cpp::device::on_ams_unload", "StatusPanel.cpp::device::on_ams_retry",
          "StatusPanel.cpp::device::on_ams_refresh_rfid", "StatusPanel.cpp::device::on_set_bed_temp",
          "StatusPanel.cpp::device::on_set_nozzle_temp", "StatusPanel.cpp::device::on_set_chamber_temp"},
         "0.1.5", OWN_CONFIRM},
        // --- filament / printer catalog methods (listed only once merged and in capabilities)
        {"filament.addSlot", "slicer.addFilamentSlot", "Add filament slot",
         "Add a filament slot (the sidebar's +), optionally with a preset; allowHidden enables a hidden system preset first.",
         Risk::Mutate, Gate::Settings, {"presets", "settings"},
         R"({"preset":{"type":"string","minLength":1,"maxLength":200},"allowHidden":{"type":"boolean"}})",
         {"Plater.cpp::sidebar::add_btn"}, "0.2.0", nullptr},
        {"filament.savePreset", "slicer.saveFilamentPreset", "Save filament preset",
         "Save a filament slot's preset, with its unsaved edits, as a user preset under a name; only that slot changes. "
         "Overwriting another saved user preset asks the person natively.",
         Risk::Mutate, Gate::Settings, {"presets"},
         R"({"name":{"type":"string","minLength":1,"maxLength":200},"slot":{"type":"integer","minimum":0,"maximum":63},
             "overwrite":{"type":"boolean"},"ifDirty":{"type":"string","enum":["ask","discard","prompt","save","transfer"]}})",
         {}, "0.2.0", nullptr, R"(["name"])"},
        {"printer.listModels", "slicer.listPrinterModels", "List printer models",
         "Every printer model the slicer ships, installed or not, with its nozzle variants.", Risk::Read, Gate::Settings, {},
         R"({"query":{"type":"string","maxLength":200},"includeNotInstalled":{"type":"boolean"},"limit":{"type":"integer","minimum":1,"maximum":500}})",
         {"MainFrame.cpp::menu::Setup Wizard"}, "0.2.0", nullptr},
        {"printer.add", "slicer.addPrinter", "Add printer",
         "Add a shipped printer model and nozzle (the setup wizard's pick), installing its vendor's profiles when needed, and "
         "select it unless select:false.",
         Risk::Mutate, Gate::Settings, {"presets", "settings"},
         R"({"vendor":{"type":"string","maxLength":200},"model":{"type":"string","minLength":1,"maxLength":200},
             "nozzle":{"type":"string","maxLength":20,"description":"\"0.4\", \"0.6 mm\"; a number is accepted too."},
             "select":{"type":"boolean"},"ifDirty":{"type":"string","enum":["ask","discard","prompt","save","transfer"]}})",
         {}, "0.2.0", nullptr, R"(["model"])"},
    };

    for (const Existing& m : methods) {
        ActionDescriptor d;
        d.id            = m.id;
        d.label         = m.label;
        d.summary       = m.summary;
        d.risk          = m.risk;
        d.gate          = m.gate;
        d.effects       = m.effects;
        d.ui            = m.ui;
        d.bridge_method = m.method;
        d.since         = m.since;
        d.params        = {{"type", "object"},
                           {"additionalProperties", false},
                           {"properties", nlohmann::json::parse(m.properties)},
                           {"description", "Typed method: these params are documented here and validated by the method itself."}};
        if (m.required != nullptr)
            d.params["required"] = nlohmann::json::parse(m.required);
        if (m.skip_rule != nullptr) {
            d.confirm_skip_rule = m.skip_rule;
            d.confirm           = m.skip_rule == OWN_CONFIRM ? Confirm::Native : Confirm::None;
        }
        c.add(std::move(d));
    }

    // The catalog's own typed methods (WebAppBridgeActions), listed for completeness.
    const nlohmann::json no_params = {{"type", "object"}, {"additionalProperties", false}, {"properties", nlohmann::json::object()}};

    ActionDescriptor get_sel;
    get_sel.id            = "bridge.getSelection";
    get_sel.label         = "Read the selection";
    get_sel.summary       = "What the person has selected: objects, parts or instances, with ids.";
    get_sel.risk          = Risk::Read;
    get_sel.bridge_method = "slicer.getSelection";
    get_sel.params        = no_params;
    c.add(get_sel);

    ActionDescriptor select;
    select.id                    = "bridge.select";
    select.label                 = "Select";
    select.summary               = "Select objects, parts or instances (a target); an empty target deselects all.";
    select.risk                  = Risk::Ui;
    select.effects               = {"selection"};
    select.bridge_method         = "slicer.select";
    select.params                = no_params;
    select.params["description"] = "Takes a target (objects, volumes, instances, plate) as its params.";
    select.ui                    = {"MainFrame.cpp::menu::Select All", "MainFrame.cpp::menu::Deselect All"};
    c.add(select);

    ActionDescriptor undo_turn;
    undo_turn.id            = "bridge.undoTurn";
    undo_turn.label         = "Undo a turn";
    undo_turn.summary       = "Undo everything one turn changed (its undo snapshots and settings writes), if nothing changed since.";
    undo_turn.risk          = Risk::Mutate;
    undo_turn.effects       = {"geometry", "structure", "plates", "settings", "paint"};
    undo_turn.bridge_method = "slicer.undoTurn";
    undo_turn.params        = {{"type", "object"},
                               {"additionalProperties", false},
                               {"required", nlohmann::json::array({"turn"})},
                               {"properties", {{"turn", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}}}};
    c.add(undo_turn);
}

}}} // namespace Slic3r::GUI::Actions
