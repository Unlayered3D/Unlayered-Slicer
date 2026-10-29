// Automation actions: layer.getEvents / layer.setEvents -- the pause, filament change, custom
// G-code and template events the preview's layer slider puts at a layer.
// Owner: work package S5 (plates). See docs/HLSD/bridge-actions.md for how to write an action and
// docs/bridge-actions/plates.md for what these do.
//
// The events live in Model::plates_custom_gcodes[plate] (what IMSlider::GetTicksValues writes on
// every tick change). Like the slider's own edits they are not on the undo stack, so
// slicer.undoTurn does not take them back: layer.setEvents answers with the events it replaced
// ("previous"), and a mode:"replace" call with those puts them back.
//
// Decision D5: pauses, filament changes and the printer's own template are free; custom G-code
// text is shown to the person in a native confirmation listing every line, and goes in only if
// they accept.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "NativeConfirm.hpp"
#include "PlatesPreviewDescriptors.hpp"
#include "PlatesPreviewLogic.hpp"
#include "Register.hpp"

#include <cstdio>
#include <set>

#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "../GCodeViewer.hpp"
#include "../GLCanvas3D.hpp"
#include "../GUI_App.hpp"
#include "../IMSlider.hpp"
#include "../MainFrame.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

namespace PP = PlatesPreview;
using nlohmann::json;

namespace {

int target_plate(const InvokeContext& ctx)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return -1;
    PartPlateList& plates = plater->get_partplate_list();
    const int      idx    = ctx.targets.plate >= 0 ? ctx.targets.plate : plates.get_curr_plate_index();
    return (idx >= 0 && idx < plates.get_plate_count()) ? idx : -1;
}

// The plate's layers as the preview's layer slider numbers them (PP::preview_layers: one layer per
// run of G-code layer ids, z = its last extrusion's height), from the plate's own slice result, so
// "layer N" is the slider's layer N here, in preview.setView and in preview.getStats. Empty when
// the plate has no valid slice result. The slider's ticks store exactly these zs, and ToolOrdering
// applies an event to the layer whose print_z is nearest.
std::vector<double> plate_layer_zs(PartPlate* plate)
{
    if (plate == nullptr || !plate->is_slice_result_valid())
        return {};
    const GCodeProcessorResult* result = plate->get_slice_result();
    if (result == nullptr || result->moves.empty())
        return {};
    return PP::preview_layer_zs(PP::preview_layers(result->moves));
}

const char* mode_id(CustomGCode::Mode m)
{
    switch (m) {
    case CustomGCode::SingleExtruder: return "singleExtruder";
    case CustomGCode::MultiAsSingle: return "multiAsSingle";
    case CustomGCode::MultiExtruder: return "multiExtruder";
    default: return "undefined";
    }
}

CustomGCode::Info plate_events(const Model& model, int plate)
{
    auto it = model.plates_custom_gcodes.find(plate);
    return it == model.plates_custom_gcodes.end() ? CustomGCode::Info() : it->second;
}

json events_reply(int plate_idx, PartPlate* plate, const CustomGCode::Info& info, const std::vector<double>& zs)
{
    return {{"plate", plate_idx},
            {"sliced", !zs.empty()},
            {"layerCount", zs.size()},
            {"topZ", zs.empty() ? json(nullptr) : json(zs.back())},
            {"mode", mode_id(info.mode)},
            {"events", PP::layer_events_json(info, zs)},
            {"printSequence", PP::print_sequence_id(plate->get_real_print_seq())}};
}

std::string format_z(double z)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", z);
    return buf;
}

InvokeResult run_get_events(InvokeContext& ctx)
{
    const int idx = target_plate(ctx);
    if (idx < 0)
        return InvokeResult::error(WAB_NOT_FOUND, "no such plate", {{"missing", "plate"}});
    PartPlate* plate = ctx.plater()->get_partplate_list().get_plate(idx);
    return InvokeResult::ok(events_reply(idx, plate, plate_events(ctx.plater()->model(), idx), plate_layer_zs(plate)));
}

InvokeResult run_set_events(InvokeContext& ctx)
{
    Plater* plater = ctx.plater();
    Model&  model  = plater->model();
    const int idx  = target_plate(ctx);
    if (idx < 0)
        return InvokeResult::error(WAB_NOT_FOUND, "no such plate", {{"missing", "plate"}});
    PartPlate* plate = plater->get_partplate_list().get_plate(idx);

    // The slider offers no events in a by-object print (ToolOrdering only applies them by layer).
    if (plate->get_real_print_seq() == PrintSequence::ByObject)
        return InvokeResult::error(WAB_ACTION_DISABLED, "layer events need a by-layer print sequence on this plate",
                                   {{"reason", "print_sequence_by_object"}});

    const std::vector<double> zs        = plate_layer_zs(plate);
    PresetBundle*             bundle    = wxGetApp().preset_bundle;
    const size_t              filaments = bundle->filament_presets.size();
    const DynamicPrintConfig  full      = bundle->full_config();
    const std::vector<int>    plate_ex  = plate->get_extruders_without_support();
    const bool                replace   = ctx.params.value("mode", std::string("add")) == "replace";
    // What the slider reads (full_config's spiral_mode); a plate's own spiral vase setting overrides it.
    const bool spiral_vase = plate->has_spiral_mode_config() ? plate->get_spiral_vase_mode()
                                                             : (full.has("spiral_mode") && full.opt_bool("spiral_mode"));

    std::vector<CustomGCode::Item> items;
    std::vector<std::string>       confirm_lines;
    std::vector<double>            custom_zs;
    const json&                    in = ctx.params.contains("items") ? ctx.params["items"] : json::array();
    for (size_t n = 0; n < in.size(); ++n) {
        const std::string path = "/params/items/" + std::to_string(n);
        PP::LayerEvent    ev;
        std::string       why;
        if (!PP::parse_layer_event(in[n], ev, why))
            return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", path}});
        double z     = 0.;
        int    layer = 0;
        if (!PP::resolve_event_z(ev, zs, z, layer, why))
            return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", path}});

        CustomGCode::Item item;
        item.print_z  = z;
        item.type     = ev.type;
        item.extruder = 1;
        if (ev.type == CustomGCode::ToolChange) {
            if (filaments < 2)
                return InvokeResult::error(WAB_ACTION_DISABLED, "a filament change needs at least two filament slots",
                                           {{"reason", "single_filament"}, {"path", path}});
            // The slider offers no Change Filament in spiral vase (IMSlider::SetModeAndOnlyExtruder), and
            // drops any filament-change tick on the next preview load (SetTicksValues).
            if (spiral_vase)
                return InvokeResult::error(WAB_ACTION_DISABLED, "spiral vase prints cannot change filament mid-print; turn spiral vase off first",
                                           {{"reason", "spiral_vase"}, {"path", path}});
            if (size_t(ev.filament) > filaments)
                return InvokeResult::error(WAB_BAD_PARAMS,
                                           "filament " + std::to_string(ev.filament) + " is out of range (1-" + std::to_string(filaments) + ")",
                                           {{"path", path + "/filament"}});
            // The slider offers filament changes only while the plate prints from one filament.
            std::set<int> used(plate_ex.begin(), plate_ex.end());
            if (used.size() > 1)
                return InvokeResult::error(WAB_ACTION_DISABLED, "the plate already prints with several filaments",
                                           {{"reason", "plate_multi_filament"}, {"path", path}});
            item.extruder = ev.filament;
            item.color    = ev.color;
            if (item.color.empty()) {
                const auto* colours = full.option<ConfigOptionStrings>("filament_colour");
                if (colours != nullptr && size_t(ev.filament) <= colours->values.size())
                    item.color = colours->values[size_t(ev.filament - 1)];
            }
        } else if (!plate_ex.empty()) {
            item.extruder = plate_ex.front();
        }
        if (ev.type == CustomGCode::Template) {
            const auto* tmpl = full.option<ConfigOptionString>("template_custom_gcode");
            if (tmpl == nullptr || tmpl->value.empty())
                return InvokeResult::error(WAB_ACTION_DISABLED, "the printer profile has no template custom G-code",
                                           {{"reason", "no_template"}, {"path", path}});
        }
        if (ev.type == CustomGCode::Custom) {
            std::vector<std::string> lines;
            PP::custom_gcode_lines(ev.gcode, lines, why);
            confirm_lines.push_back("At Z " + format_z(z) + " mm:");
            for (const std::string& l : lines)
                confirm_lines.push_back("    " + l);
            item.extra = ev.gcode;
            custom_zs.push_back(z);
        }
        if (ev.type == CustomGCode::PausePrint)
            item.extra = ev.message;
        items.push_back(item);
    }
    std::vector<double> remove;
    if (ctx.params.contains("remove"))
        for (const json& z : ctx.params["remove"])
            remove.push_back(z.get<double>());

    // Custom G-code goes to the printer as typed: the person reads it and agrees first (D5).
    if (!confirm_lines.empty()) {
        ConfirmRequest req;
        req.title        = "Insert custom G-code";
        req.message      = "A web application asks to insert custom G-code into this plate's print, at the start of " +
                      std::string(custom_zs.size() == 1 ? "one layer" : std::to_string(custom_zs.size()) + " layers") +
                      ". It runs on the printer exactly as written:";
        req.details      = confirm_lines;
        req.accept_label = "Insert";
        req.destructive  = false;
        if (!ctx.confirm(req))
            return InvokeResult::error(WAB_USER_CANCELLED, "the person declined the custom G-code");
    }

    const CustomGCode::Info before = plate_events(model, idx);
    CustomGCode::Info       after  = PP::merge_layer_events(before, items, replace, PP::slider_mode(filaments, plate_ex));
    const size_t            removed = PP::remove_layer_events(after, remove);
    const bool              changed = after.gcodes != before.gcodes;

    if (changed) {
        model.plates_custom_gcodes[idx] = after;
        // As a tick change on the slider (Plater's EVT_CUSTOMEVT_TICKSCHANGED): the plate's G-code
        // is stale now, and the project is modified.
        plate->update_slice_result_valid_state(false);
        plater->set_plater_dirty(true);
        if (idx == plater->get_partplate_list().get_curr_plate_index() && plater->is_preview_loaded()) {
            if (GLCanvas3D* preview = plater->get_preview_canvas3D(); preview != nullptr) {
                preview->get_gcode_viewer().get_layers_slider()->SetTicksValues(after);
                preview->set_as_dirty();
            }
            plater->reload_print();
        }
        // As the slider's tick handler (Plater, EVT_CUSTOMEVT_TICKSCHANGED): refresh the Slice and
        // Print buttons for the now stale plate.
        if (wxGetApp().mainframe != nullptr)
            wxGetApp().mainframe->update_slice_print_status(MainFrame::SlicePrintEventType::eEventSliceUpdate, true, false);
        plater->schedule_background_process();
    }

    json out        = events_reply(idx, plate, after, zs);
    out["previous"] = PP::layer_events_json(before, zs);
    out["removed"]  = removed;
    out["note"]     = "Layer events are not on the undo stack; to undo, call layer.setEvents with mode \"replace\" and the "
                      "\"previous\" events.";
    if (zs.empty() && !items.empty())
        out["warning"] = "the plate is not sliced, so z was kept as given; each event applies at the layer nearest its z";
    for (const CustomGCode::Item& it : items)
        if (it.type == CustomGCode::PausePrint) {
            const auto* pause = full.option<ConfigOptionString>("machine_pause_gcode");
            if (pause == nullptr || pause->value.empty())
                out["warning"] = "the printer profile's pause G-code is empty, so a pause writes only a marker into the G-code";
            break;
        }
    if (!changed)
        return InvokeResult::ok(out).with_effects({});
    return InvokeResult::ok(out);
}

} // namespace

void register_layer_event_actions(Catalog& c)
{
    PP::add_bound(c, PP::layer_event_descriptors(), {{"layer.getEvents", {run_get_events, {}}}, {"layer.setEvents", {run_set_events, {}}}});
}

}}} // namespace Slic3r::GUI::Actions
