#ifndef slic3r_GUI_Actions_PlatesPreviewDescriptors_hpp_
#define slic3r_GUI_Actions_PlatesPreviewDescriptors_hpp_

// The descriptors of the plate, layer-event and preview actions (work package S5), as pure data.
// PlateActions.cpp / LayerEventActions.cpp / PreviewActions.cpp pair each with its runner;
// tests/slic3rutils/test_actions_plates.cpp registers them into a fresh Catalog (which checks ids,
// schemas, effects and that every example validates) without a running application.

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ActionCatalog.hpp"
#include "PlatesPreviewLogic.hpp"

namespace Slic3r { namespace GUI { namespace Actions { namespace PlatesPreview {

// Preview colourings, in libvgcode::EViewType order (PreviewActions.cpp static_asserts the count).
inline const std::vector<std::string>& view_type_ids()
{
    static const std::vector<std::string> ids = {"summary",     "featureType", "filament",       "speed",        "actualSpeed",
                                                 "layerHeight", "lineWidth",   "flow",           "actualFlow",   "layerTime",
                                                 "layerTimeLog", "fanSpeed",   "temperature",    "pressureAdvance", "acceleration",
                                                 "jerk",        "tool",        "section"};
    return ids;
}

// Capture cameras (PreviewActions.cpp maps them to Camera::ViewAngleType and View-menu directions).
inline const std::vector<std::string>& camera_ids()
{
    static const std::vector<std::string> ids = {"iso", "topFront", "top", "bottom", "front", "rear", "left", "right"};
    return ids;
}

namespace detail {

inline nlohmann::json obj(nlohmann::json properties, nlohmann::json required = nlohmann::json::array())
{
    nlohmann::json s = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = std::move(required);
    return s;
}

inline nlohmann::json example(nlohmann::json target, nlohmann::json params)
{
    nlohmann::json e = nlohmann::json::object();
    e["target"]      = std::move(target);
    e["params"]      = std::move(params);
    return nlohmann::json::array({e});
}

inline ActionDescriptor make(const char* id, const char* label, Risk risk, const char* summary)
{
    ActionDescriptor d;
    d.id      = id;
    d.label   = label;
    d.risk    = risk;
    d.summary = summary;
    d.params  = obj(nlohmann::json::object());
    return d;
}

inline nlohmann::json plate_target(bool required = false) { return {{"kind", "plate"}, {"required", required}}; }
inline nlohmann::json plate_ref(int i) { return {{"plate", i}}; }

} // namespace detail

inline std::vector<ActionDescriptor> plate_descriptors()
{
    using nlohmann::json;
    using namespace detail;
    const json name_prop = {{"type", "string"}, {"maxLength", 120}, {"description", "Plate name (empty clears it)."}};
    std::vector<ActionDescriptor> out;

    {
        ActionDescriptor d = make("plate.list", "Plates", Risk::Read,
                                  "List the plates with their name, lock, objects, slice state, bed type, print sequence, spiral vase "
                                  "and filament order.");
        d.examples = example(nullptr, json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.add", "Add plate", Risk::Mutate, "Add an empty plate (optionally named) and make it the current plate.");
        d.params   = obj({{"name", name_prop},
                          {"select", {{"type", "boolean"}, {"default", true}, {"description", "Make the new plate current."}}}});
        d.effects  = {"plates"};
        d.ui       = {"GLCanvas3D.cpp::toolbar::addplate"};
        d.examples = example(nullptr, {{"name", "Second copy"}});
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.delete", "Delete Plate", Risk::Mutate,
                                  "Delete a plate (default: the current one); objects on it move off the plates. Undoable.");
        d.target   = plate_target();
        d.effects  = {"plates", "structure"};
        d.ui       = {"GUI_Factories.cpp::menu::Delete Plate", "Plater.cpp::bedicon::hover:1"};
        d.examples                = example(plate_ref(1), json::object());
        d.examples[0]["setup"]    = json::array({{{"id", "plate.add"}, {"params", json::object()}}});
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.duplicate", "Duplicate Current Plate", Risk::Mutate,
                                  "Copy a plate (default: the current one) and everything on it to a new plate.");
        d.target   = plate_target();
        d.effects  = {"plates", "structure"};
        d.ui       = {"MainFrame.cpp::menu::Duplicate Current Plate"};
        d.examples = example(plate_ref(0), json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.select", "Select plate", Risk::Ui,
                                  "Make a plate the current plate (what Slice, Arrange and the preview act on). With Preview shown, "
                                  "the preview reloads to the plate's toolpaths; never starts a slice.");
        d.target   = plate_target(true);
        d.effects  = {"plates", "view"};
        d.ui       = {"Plater.cpp::bedicon::hover:0", "GLCanvas3D.cpp::imgui::Sliced plate thumbnail"};
        d.examples = example(plate_ref(0), json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.rename", "Edit Plate Name", Risk::Mutate, "Rename a plate (default: the current one).");
        d.target   = plate_target();
        d.params   = obj({{"name", name_prop}}, json::array({"name"}));
        d.effects  = {"plates"};
        d.ui       = {"GUI_Factories.cpp::menu::expr:name@append_menu_item_plate_name", "Plater.cpp::bedicon::hover:6"};
        d.examples = example(plate_ref(0), {{"name", "PETG parts"}});
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.lock", "Lock plate", Risk::Mutate,
                                  "Lock or unlock a plate; arrange and auto-orient leave a locked plate alone.");
        d.target   = plate_target();
        d.params   = obj({{"locked", {{"type", "boolean"}, {"default", true}}}});
        d.effects  = {"plates"};
        d.ui       = {"GUI_Factories.cpp::menu::expr:lock_text@append_menu_item_locked", "Plater.cpp::bedicon::hover:4"};
        d.examples = example(plate_ref(0), {{"locked", true}});
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.setSettings", "Plate Settings", Risk::Mutate,
                                  "Change a plate's own settings: bed type, print sequence, spiral vase, skirt start angle, first/"
                                  "other-layer filament order, name, lock, and on multi-nozzle printers the filament grouping. "
                                  "\"default\" means the same as the global setting.");
        d.target         = plate_target();
        const json order = {{"type", "array"},
                            {"maxItems", 64},
                            {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                            {"description", "1-based filament slots in printing order; [] = automatic."}};
        d.params = obj({{"bedType",
                         {{"type", "string"},
                          {"enum", bed_type_ids()},
                          {"description", "Per-plate bed type (printers with several bed types only); default = the global one."}}},
                        {"printSequence",
                         {{"type", "string"},
                          {"enum", json::array({"default", "byLayer", "byObject", "byGroup"})},
                          {"description", "byGroup prints objects group by group by their print_group setting."}}},
                        {"skirtStartAngle",
                         {{"type", "number"},
                          {"minimum", -180},
                          {"maximum", 180},
                          {"description", "Degrees from the object centre to where the skirt starts (0 = +X, counter-clockwise positive)."}}},
                        {"resetSkirtStartAngle", {{"type", "boolean"}, {"description", "true: back to the global skirt start angle."}}},
                        {"spiralVase",
                         {{"type", "string"},
                          {"enum", json::array({"default", "on", "off"})},
                          {"description", "on also sets the per-object overrides spiral vase needs (reported back)."}}},
                        {"firstLayerFilamentOrder", order},
                        {"otherLayersFilamentOrder",
                         {{"type", "array"},
                          {"maxItems", 32},
                          {"items", obj({{"fromLayer", {{"type", "integer"}, {"minimum", 1}}},
                                         {"toLayer", {{"type", "integer"}, {"minimum", 1}}},
                                         {"order", order}},
                                        json::array({"fromLayer", "toLayer", "order"}))}}},
                        {"name", name_prop},
                        {"locked", {{"type", "boolean"}}},
                        {"filamentMapMode",
                         {{"type", "string"},
                          {"enum", json::array({"default", "autoForFlush", "autoForMatch", "manual"})},
                          {"description", "Multi-nozzle printers: how filaments are grouped onto the nozzles (Filament Grouping). "
                                          "autoForFlush = Set to Optimal; manual needs filamentMaps."}}},
                        {"filamentMaps",
                         {{"type", "array"},
                          {"minItems", 1},
                          {"maxItems", 64},
                          {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2}}},
                          {"description", "manual: the nozzle (1 = left, 2 = right) of each filament slot, in slot order."}}}});
        d.effects  = {"plates", "settings"};
        d.ui       = {"Plater.cpp::bedicon::hover:5", "GCodeViewer.cpp::imgui::Regroup filament", "GCodeViewer.cpp::imgui::Set to Optimal"};
        d.examples = example(plate_ref(0), {{"printSequence", "byObject"}});
        json skirt = {{"target", plate_ref(0)}, {"params", {{"skirtStartAngle", 90}}}};
        d.examples.push_back(skirt);
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.move", "Move plate", Risk::Mutate,
                                  "Move a plate (default: the current one) to another place in the plate order, e.g. 0 to put it first "
                                  "(the plate's Move-to-front icon); the plates in between shift by one. Returns the new order.");
        d.target   = plate_target();
        d.params   = obj({{"toIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"description", "Where the plate goes (0-based)."}}}},
                         json::array({"toIndex"}));
        d.effects  = {"plates"};
        d.ui       = {"Plater.cpp::bedicon::hover:7"};
        d.examples                = example(plate_ref(1), {{"toIndex", 0}});
        d.examples[0]["setup"]    = json::array({{{"id", "plate.add"}, {"params", json::object()}}});
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.setWipeTower", "Prime tower position", Risk::Mutate,
                                  "Place a plate's prime (wipe) tower: x, y in mm from the plate's front-left corner, as dragging it does. "
                                  "A position the tower would not fit at is moved onto the plate (reported). Kept for when the prime tower is on.");
        d.target   = plate_target();
        d.params   = obj({{"x", {{"type", "number"}, {"minimum", -1000}, {"maximum", 5000}, {"description", "mm from the plate's left edge."}}},
                          {"y", {{"type", "number"}, {"minimum", -1000}, {"maximum", 5000}, {"description", "mm from the plate's front edge."}}}},
                         json::array({"x", "y"}));
        d.effects  = {"plates", "settings"};
        d.examples = example(plate_ref(0), {{"x", 20}, {"y", 180}});
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.selectAll", "Select All", Risk::Ui,
                                  "Select every object on a plate (default: the current one), or on all plates.");
        d.target   = plate_target();
        d.params   = obj({{"allPlates", {{"type", "boolean"}, {"default", false}}}});
        d.effects  = {"selection"};
        d.ui       = {"GUI_Factories.cpp::menu::Select All", "GUI_Factories.cpp::menu::Select All Plates"};
        d.examples = example(plate_ref(0), json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.reloadAll", "Reload All", Risk::Mutate,
                                  "Reload every object from its source file on disk. Refused when a source is missing (the person "
                                  "would be asked to find it).");
        d.effects  = {"geometry", "structure"};
        d.ui       = {"GUI_Factories.cpp::menu::Reload All"};
        d.examples = example(nullptr, json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("plate.autoOrient", "Auto Rotate", Risk::Mutate,
                                  "Auto-orient every object on a plate (default: the current one) for printing.");
        d.target       = plate_target();
        d.effects      = {"geometry"};
        d.long_running = true;
        d.ui           = {"GUI_Factories.cpp::menu::Auto Rotate", "Plater.cpp::bedicon::hover:2"};
        d.examples     = example(plate_ref(0), json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("object.autoOrient", "Auto orient", Risk::Mutate, "Auto-orient the given objects for printing (the toolbar's Auto orient on a selection).");
        d.target       = {{"kind", "objects"}, {"required", true}};
        d.effects      = {"geometry"};
        d.long_running = true;
        d.ui           = {"GLCanvas3D.cpp::toolbar::orient"};
        d.examples     = example({{"objects", json::array({{{"index", 0}}})}}, json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("object.toPlate", "Move to plate", Risk::Mutate,
                                  "Move objects or single instances (copies) to another plate, or to a new plate; optionally arrange it.");
        d.target = {{"kind", "instances"}, {"required", true}};
        d.params = obj({{"plate", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"description", "Destination plate (0-based)."}}},
                        {"newPlate", {{"type", "boolean"}, {"description", "Create a plate at the end and move there."}}},
                        {"arrange", {{"type", "boolean"}, {"default", false}, {"description", "Arrange the destination plate afterwards."}}}});
        d.params["x-exactlyOne"] = json::array({json::array({"plate"}), json::array({"newPlate"})});
        d.effects                = {"geometry", "plates"};
        json inst                = {{"object", {{"index", 0}}}, {"index", 0}};
        d.examples               = example({{"instances", json::array({inst})}}, {{"newPlate", true}});
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("slice.all", "Slice all", Risk::Mutate,
                                  "Slice every plate locally, one after the other, and answer when all are done (per-plate "
                                  "slicer.sliceFinished events too). Never a service run: refused with 1014 while a slicing-service mode is selected.");
        d.effects      = {"plates"};
        d.long_running = true;
        d.ui       = {"MainFrame.cpp::sidebutton::Slice all"};
        d.examples = example(nullptr, json::object());
        out.push_back(d);
    }
    return out;
}

inline std::vector<ActionDescriptor> layer_event_descriptors()
{
    using nlohmann::json;
    using namespace detail;
    std::vector<ActionDescriptor> out;
    {
        ActionDescriptor d = make("layer.getEvents", "Layer events", Risk::Read,
                                  "List a plate's per-layer events (pause, filament change, custom G-code, template) with their z and layer.");
        d.target   = plate_target();
        d.ui       = {"IMSlider.cpp::imgui::Tick tooltip"};
        d.examples = example(plate_ref(0), json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("layer.setEvents", "Layer events", Risk::Mutate,
                                  "Add, replace or remove a plate's per-layer events: pause, filament change (toolChange), custom G-code "
                                  "(the person confirms the lines), or the printer's template. Positions are z in mm or a 1-based layer "
                                  "(the preview slider's numbering). No filament change in spiral vase.");
        d.target  = plate_target();
        d.ui      = {"IMSlider.cpp::imgui::Add Pause", "IMSlider.cpp::imgui::Add Custom G-code", "IMSlider.cpp::imgui::Add Custom Template",
                     "IMSlider.cpp::imgui::Change Filament", "IMSlider.cpp::imgui::Delete tick", "IMSlider.cpp::imgui::Edit Custom G-code"};
        json item = obj({{"type", {{"type", "string"}, {"enum", json::array({"pause", "toolChange", "custom", "template"})}}},
                         {"z", {{"type", "number"}, {"minimum", 0}, {"maximum", 10000},
                                {"description", "Height in mm (snapped to the nearest layer once sliced)."}}},
                         {"layer", {{"type", "integer"}, {"minimum", 1}, {"description", "1-based layer number (needs a slice)."}}},
                         {"filament", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64},
                                       {"description", "toolChange: the filament slot to switch to."}}},
                         {"color", {{"type", "string"}, {"minLength", 7}, {"maxLength", 7}, {"description", "toolChange: #RRGGBB for the preview."}}},
                         {"gcode", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}, {"description", "custom: the G-code text."}}},
                         {"message", {{"type", "string"}, {"maxLength", 120}, {"description", "pause: a note kept with the event."}}}},
                        json::array({"type"}));
        item["x-exactlyOne"] = json::array({json::array({"z"}), json::array({"layer"})});
        d.params = obj({{"items", {{"type", "array"}, {"maxItems", 200}, {"items", item}}},
                        {"mode", {{"type", "string"},
                                  {"enum", json::array({"add", "replace"})},
                                  {"default", "add"},
                                  {"description", "add: keep the other events too (one per layer); replace: these become all the events."}}},
                        {"remove", {{"type", "array"},
                                    {"maxItems", 200},
                                    {"items", {{"type", "number"}, {"minimum", 0}}},
                                    {"description", "z values (mm) of events to delete."}}}});
        d.effects  = {"plates"};
        json pause = {{"type", "pause"}, {"z", 15.0}};
        d.examples = example(plate_ref(0), {{"items", json::array({pause})}});
        out.push_back(d);
    }
    return out;
}

// The colourings preview.setView offers: the libvgcode list minus Tool, which the legend's combo
// never lists (Section is listed only when the print has more than one section).
inline std::vector<std::string> offered_view_type_ids()
{
    std::vector<std::string> ids;
    for (const std::string& id : view_type_ids())
        if (id != "tool")
            ids.push_back(id);
    return ids;
}

inline std::vector<ActionDescriptor> preview_descriptors()
{
    using nlohmann::json;
    using namespace detail;
    std::vector<ActionDescriptor> out;
    const json time_mode = {{"type", "string"},
                            {"enum", json::array({"normal", "stealth"})},
                            {"description", "Time estimate: normal, or stealth (silent) mode when the printer profile has one."}};
    {
        ActionDescriptor d = make("preview.getStats", "Slice statistics", Risk::Read,
                                  "A sliced plate's figures, as the preview legend shows them: total time, filament per slot (mm3, m, g, "
                                  "cost), per feature time/filament/share, travel, retract/unretract/wipe/seam figures, filament-change "
                                  "time, colour-scale ranges, per-layer times on request; or the All Plates Stats totals.");
        d.target = plate_target();
        d.params = obj({{"layers", {{"type", "boolean"}, {"default", false}, {"description", "Include per-layer z and time."}}},
                        {"maxLayers", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"default", 2000}}},
                        {"timeMode", time_mode},
                        {"allPlates",
                         {{"type", "boolean"},
                          {"default", false},
                          {"description", "Totals over every plate with objects (the All Plates Stats window); refused while any is unsliced."}}}});
        d.ui       = {"IMSlider.cpp::imgui::Layer time tooltip", "GCodeViewer.cpp::imgui::Colour range scale",
                      "GCodeViewer.cpp::imgui::Filament table", "GCodeViewer.cpp::imgui::Show stealth mode",
                      "GCodeViewer.cpp::imgui::Statistics of All Plates", "GLCanvas3D.cpp::imgui::All plates stats"};
        d.examples = example(plate_ref(0), json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("preview.setView", "Preview view", Risk::Ui,
                                  "Switch between Prepare and Preview and set what the preview shows, as its legend and sliders do: "
                                  "colouring, layers (or one layer), the move on the moves slider, which features, travel/wipe/"
                                  "retract/seam markers and sections are visible, normal/stealth times, a belt printer's raw G-code. "
                                  "All-or-nothing: a bad value changes nothing.");
        json feature_props = json::object();
        for (const std::string& id : extrusion_role_ids())
            feature_props[id] = {{"type", "boolean"}};
        json option_props = json::object();
        for (const std::string& id : preview_option_ids())
            option_props[id] = {{"type", "boolean"}};
        json move = obj({{"index", {{"type", "integer"}, {"minimum", 1}, {"description", "1-based position on the moves slider."}}},
                         {"to", {{"type", "string"}, {"enum", json::array({"start", "end"})}, {"description", "Home / End."}}},
                         {"step", {{"type", "integer"}, {"minimum", -10000}, {"maximum", 10000},
                                   {"description", "Moves forward (+) or back (-), stopping at the layer's ends."}}}});
        move["x-exactlyOne"]  = json::array({json::array({"index"}), json::array({"to"}), json::array({"step"})});
        move["description"]   = "The moves slider (the top shown layer's moves): exactly one of index, to, step.";
        json features         = obj(feature_props);
        features["description"] = "Feature types to show (true) or hide (false), by the preview.getStats feature ids; others unchanged.";
        json options          = obj(option_props);
        options["description"] = "Legend option rows to show or hide; others unchanged.";
        d.params = obj({{"show", {{"type", "string"}, {"enum", json::array({"prepare", "preview"})}}},
                        {"viewType", {{"type", "string"}, {"enum", offered_view_type_ids()},
                                      {"description", "section only when the print has more than one section."}}},
                        {"layerRange", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "integer"}, {"minimum", 1}}},
                                        {"description", "[first, last] 1-based layers to show."}}},
                        {"zRange", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}, {"minimum", 0}}},
                                    {"description", "[from, to] heights in mm (nearest layers)."}}},
                        {"oneLayer", {{"type", "boolean"},
                                      {"description", "The slider's one-layer mode. true shows the top of layerRange/zRange (both ends equal) or "
                                                      "the current top layer; false shows every layer again."}}},
                        {"move", move},
                        {"features", features},
                        {"showAllFeatures", {{"type", "boolean"}, {"description", "Make every feature type visible again."}}},
                        {"options", options},
                        {"sections", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256},
                                      {"items", {{"type", "integer"}, {"minimum", 1}}},
                                      {"description", "Section view: the 1-based sections to show; the others are hidden."}}},
                        {"timeMode", time_mode},
                        {"beltRaw", {{"type", "boolean"},
                                     {"description", "Belt printer G-code: true shows the raw machine-frame G-code, false the upright view."}}}});
        d.effects  = {"view"};
        d.ui       = {"IMSlider.cpp::imgui::Layer slider", "IMSlider.cpp::imgui::Jump to layer", "IMSlider.cpp::imgui::One layer mode",
                      "IMSlider.cpp::imgui::Moves slider", "GCodeViewer.cpp::imgui::Color scheme combo",
                      "GCodeViewer.cpp::imgui::Feature type rows", "GCodeViewer.cpp::imgui::Option rows",
                      "GCodeViewer.cpp::imgui::Section rows", "GCodeViewer.cpp::imgui::Show stealth mode",
                      "GCodeViewer.cpp::imgui::Show raw machine G-code"};
        d.examples = example(nullptr, {{"show", "preview"}, {"viewType", "featureType"}});
        json hide  = {{"options", {{"travel", true}}}, {"features", {{"support", false}}}, {"layerRange", json::array({5, 5})},
                      {"oneLayer", true}, {"move", {{"to", "end"}}}};
        d.examples.push_back({{"target", nullptr}, {"params", hide}});
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("preview.getMove", "Tool position", Risk::Read,
                                  "The move at the moves slider's position in the shown preview, as the tool-position window lists it "
                                  "(position, type, feature, width, height, speed, flow, fan, temperature, acceleration, jerk, pressure "
                                  "advance, layer, time); optionally the G-code lines around it, as the G-code window shows them.");
        d.params   = obj({{"gcodeLines", {{"type", "boolean"}, {"default", false},
                                          {"description", "Include the G-code text around the move (needs the printer permission, as G-code export does)."}}},
                          {"before", {{"type", "integer"}, {"minimum", 0}, {"maximum", 50}, {"default", 5}}},
                          {"after", {{"type", "integer"}, {"minimum", 0}, {"maximum", 50}, {"default", 5}}}});
        d.ui       = {"GCodeViewer.cpp::imgui::Tool position window", "GCodeViewer.cpp::imgui::G-code window"};
        d.examples = example(nullptr, json::object());
        out.push_back(d);
    }
    {
        ActionDescriptor d = make("view.capture", "Capture view", Risk::Read,
                                  "A PNG picture of a plate: the Prepare view rendered from a chosen camera, or what the Preview canvas "
                                  "shows (toolpaths; the Preview tab must be on screen, and projection/showBed/transparent apply to "
                                  "prepare only). At most 1024 px and 1.5 MB.");
        d.target   = plate_target();
        d.params   = obj({{"canvas", {{"type", "string"}, {"enum", json::array({"prepare", "preview"})}, {"default", "prepare"}}},
                          {"camera", {{"type", "string"}, {"enum", camera_ids()},
                                      {"description", "prepare: default iso. preview: moves the preview camera like View > <direction>."}}},
                          {"projection", {{"type", "string"}, {"enum", json::array({"ortho", "perspective"})},
                                          {"description", "prepare only (default ortho)."}}},
                          {"width", {{"type", "integer"}, {"minimum", 64}, {"maximum", 1024}, {"default", 512}}},
                          {"height", {{"type", "integer"}, {"minimum", 64}, {"maximum", 1024}, {"default", 512}}},
                          {"showBed", {{"type", "boolean"}, {"description", "prepare only (default true)."}}},
                          {"transparent", {{"type", "boolean"}, {"description", "prepare only (default false)."}}}});
        d.examples = example(plate_ref(0), {{"camera", "iso"}, {"width", 512}, {"height", 512}});
        out.push_back(d);
    }
    return out;
}

// Adds each descriptor with its runner (and enabled predicate); a descriptor without one is a
// programming error and throws, so it shows in the first test run.
using Binding = std::pair<Runner, EnabledFn>;
inline void add_bound(Catalog& c, std::vector<ActionDescriptor> descriptors, const std::map<std::string, Binding>& bindings)
{
    for (ActionDescriptor& d : descriptors) {
        auto it = bindings.find(d.id);
        if (it == bindings.end() || !it->second.first)
            throw std::logic_error("action '" + d.id + "' has no runner");
        c.add(std::move(d), it->second.first, it->second.second);
    }
}

}}}} // namespace Slic3r::GUI::Actions::PlatesPreview

#endif
