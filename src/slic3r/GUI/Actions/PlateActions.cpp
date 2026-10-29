// Automation actions: plate.* actions (plate.clear is the objects package's), object.toPlate, object.autoOrient, slice.all.
// Owner: work package S5 (plates). See docs/HLSD/bridge-actions.md for how to write an action and
// docs/bridge-actions/plates.md for what these do.
//
// Every runner calls the same Plater / PartPlateList function the plate's context menu, the plate
// icons, the toolbar or the Plate Settings dialog calls, minus the dialog: what the dialog asks
// for arrives as params. Plater functions that take their own undo snapshot (delete_plate,
// select_plate, orient, reload_all_from_disk) are not wrapped in another.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "ActionTargets.hpp"
#include "ObjectListLogic.hpp"
#include "PlatesPreviewDescriptors.hpp"
#include "PlatesPreviewLogic.hpp"
#include "Register.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <set>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <wx/timer.h>

#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "../Camera.hpp"
#include "../GLCanvas3D.hpp"
#include "../GLToolbar.hpp"
#include "../GUI_App.hpp"
#include "../GUI_ObjectList.hpp"
#include "../Jobs/Job.hpp"
#include "../Jobs/Worker.hpp"
#include "../MainFrame.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

namespace PP = PlatesPreview;
using nlohmann::json;

namespace {

const json PLATE_TARGET = {{"kind", "plate"}, {"required", false}};

// The plate an action acts on: the target's plate, else the current one.
int target_plate(const InvokeContext& ctx)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return -1;
    PartPlateList& plates = plater->get_partplate_list();
    const int      idx    = ctx.targets.plate >= 0 ? ctx.targets.plate : plates.get_curr_plate_index();
    return (idx >= 0 && idx < plates.get_plate_count()) ? idx : -1;
}

InvokeResult no_plate() { return InvokeResult::error(WAB_NOT_FOUND, "no such plate", {{"missing", "plate"}}); }

InvokeResult busy_jobs() { return InvokeResult::error(WAB_BUSY, "an arrange, orient or fill-bed job is running"); }

bool jobs_idle()
{
    Plater* plater = wxGetApp().plater();
    return plater != nullptr && plater->get_ui_job_worker().is_idle();
}

std::string spiral_id(PartPlate* plate)
{
    if (!plate->has_spiral_mode_config())
        return "default";
    return plate->get_spiral_vase_mode() ? "on" : "off";
}

json other_layers_order_json(const std::vector<LayerPrintSequence>& seqs)
{
    json arr = json::array();
    for (const LayerPrintSequence& s : seqs)
        arr.push_back({{"fromLayer", s.first.first}, {"toLayer", s.first.second}, {"order", s.second}});
    return arr;
}

// Which (object, instance) pairs the plate holds.
std::vector<std::pair<int, int>> plate_instances(PartPlate* plate, const Model& model)
{
    std::vector<std::pair<int, int>> out;
    for (size_t o = 0; o < model.objects.size(); ++o)
        for (size_t i = 0; i < model.objects[o]->instances.size(); ++i)
            if (plate->contain_instance(int(o), int(i)))
                out.emplace_back(int(o), int(i));
    return out;
}

json plate_json(int index)
{
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    PartPlate*     plate  = plates.get_plate(index);
    const Model&   model  = plater->model();

    json          objects = json::array();
    std::set<int> seen;
    const auto    held = plate_instances(plate, model);
    for (const auto& oi : held)
        if (seen.insert(oi.first).second)
            objects.push_back(object_ref_json(model, oi.first));

    size_t events = 0;
    if (auto it = model.plates_custom_gcodes.find(index); it != model.plates_custom_gcodes.end())
        events = it->second.gcodes.size();

    json j = {{"index", index},
            {"name", plate->get_plate_name()},
            {"isCurrent", index == plates.get_curr_plate_index()},
            {"locked", plate->is_locked()},
            {"objects", objects},
            {"instanceCount", held.size()},
            {"hasPrintableInstances", plate->has_printable_instances()},
            {"sliceResultValid", plate->is_slice_result_valid()},
            {"bedType", PP::bed_type_id(plate->get_bed_type(false))},
            {"effectiveBedType", PP::bed_type_id(plate->get_bed_type(true))},
            {"printSequence", PP::print_sequence_id(plate->get_print_seq())},
            {"spiralVase", spiral_id(plate)},
            {"skirtStartAngle", plate->config()->has("skirt_start_angle") ? json(plate->config()->opt_float("skirt_start_angle")) : json(nullptr)},
            {"firstLayerFilamentOrder", plate->get_first_layer_print_sequence()},
            {"otherLayersFilamentOrder", other_layers_order_json(plate->get_other_layers_print_sequence())},
            {"layerEvents", events},
            {"filamentMapMode", PP::filament_map_mode_id(plate->get_filament_map_mode())}};
    // Multi-nozzle printers: the nozzle (1-based) each filament slot prints from on this plate.
    if (wxGetApp().preset_bundle->get_printer_extruder_count() > 1)
        j["filamentMaps"] = plate->get_real_filament_maps(wxGetApp().preset_bundle->project_config);
    return j;
}

json plates_json()
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    json           arr    = json::array();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        arr.push_back(plate_json(i));
    return {{"currentPlate", plates.get_curr_plate_index()}, {"plateCount", plates.get_plate_count()}, {"plates", arr}};
}

// After moving instances directly: the plate list re-files them and the object list follows
// (what GLCanvas3D::do_move does after a gizmo move).
void refile_objects(const std::set<int>& objs)
{
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    for (int o : objs) {
        plater->changed_object(o);
        for (size_t i = 0; i < plater->model().objects[size_t(o)]->instances.size(); ++i)
            plates.notify_instance_update(o, int(i));
    }
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
        list->update_plate_values_for_items();
}

// The Plate Settings dialog offers a per-plate bed type only for the built-in vendor's printers
// (PlateSettingsDialog greys the choice out otherwise, even where the sidebar has a global bed-type
// choice). Going back to "default" is always allowed, as the plate tab's reset does. "" = allowed,
// else why not.
std::string bed_type_unavailable(BedType type)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return "no printer is selected";
    if (type == btDefault)
        return "";
    if (!bundle->is_bbl_vendor())
        return "this printer's profile offers no per-plate bed type; every plate follows the global bed type";
    if (const VendorProfile::PrinterModel* pm = wxGetApp().plater()->get_curr_printer_model(); pm != nullptr) {
        const ConfigOptionDef* def = print_config_def.get("curr_bed_type");
        if (def != nullptr && size_t(type) >= 1 && size_t(type) <= def->enum_labels.size()) {
            const std::string& label = def->enum_labels[size_t(type) - 1];
            if (std::find(pm->not_support_bed_types.begin(), pm->not_support_bed_types.end(), label) != pm->not_support_bed_types.end())
                return "this printer does not support the " + label;
        }
    }
    return "";
}

std::string has_objects_enabled(const InvokeContext&)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->model().objects.empty())
        return "the plate is empty";
    return "";
}

// ---------------------------------------------------------------------------------------------
// Runners
// ---------------------------------------------------------------------------------------------

InvokeResult run_list(InvokeContext&) { return InvokeResult::ok(plates_json()); }

InvokeResult run_add(InvokeContext& ctx)
{
    Plater*        plater = ctx.plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (!plater->can_add_plate())
        return InvokeResult::error(WAB_ACTION_DISABLED, "no more plates can be added", {{"reason", "plate_limit"}});
    const std::string name = ctx.params.value("name", std::string());
    std::string       why;
    if (!PP::valid_plate_name(name, why))
        return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params/name"}});

    // The toolbar's Add Plate (Plater::priv::on_action_add_plate), with the snapshot ours.
    ctx.snapshot("Add plate");
    plates.create_plate();
    const int index = plates.get_plate_count() - 1;
    if (!name.empty())
        plates.get_plate(index)->set_plate_name(name);
    if (ctx.params.value("select", true))
        plates.select_plate(index);
    plater->update();
    plater->get_camera().requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;
    json out     = plate_json(index);
    out["plate"] = index;
    return InvokeResult::ok(out);
}

InvokeResult run_delete(InvokeContext& ctx)
{
    Plater*        plater = ctx.plater();
    PartPlateList& plates = plater->get_partplate_list();
    const int      index  = target_plate(ctx);
    if (index < 0)
        return no_plate();
    if (plates.get_plate_count() <= 1)
        return InvokeResult::error(WAB_ACTION_DISABLED, "the last plate cannot be deleted", {{"reason", "last_plate"}});
    // Objects on it move off the plates, as when the person deletes it (undoable).
    const size_t moved = plate_instances(plates.get_plate(index), plater->model()).size();
    if (plater->delete_plate(index) != 0)
        return InvokeResult::error(WAB_INTERNAL, "the plate could not be deleted");
    json out                 = plates_json();
    out["deleted"]           = index;
    out["instancesOffPlate"] = moved;
    return InvokeResult::ok(out);
}

InvokeResult run_duplicate(InvokeContext& ctx)
{
    Plater*   plater = ctx.plater();
    const int index  = target_plate(ctx);
    if (index < 0)
        return no_plate();
    if (!plater->can_add_plate())
        return InvokeResult::error(WAB_ACTION_DISABLED, "no more plates can be added", {{"reason", "plate_limit"}});
    // Edit > Duplicate Current Plate takes no snapshot; this one does, so it can be undone.
    ctx.snapshot("Duplicate plate");
    const int created = plater->duplicate_plate(index);
    if (created < 0)
        return InvokeResult::error(WAB_INTERNAL, "the plate could not be duplicated");
    json out          = plate_json(created);
    out["plate"]      = created;
    out["duplicated"] = index;
    return InvokeResult::ok(out);
}

InvokeResult run_select(InvokeContext& ctx)
{
    const int index = target_plate(ctx);
    if (index < 0)
        return no_plate();
    Plater* plater = ctx.plater();
    if (plater->select_plate(index) != 0)
        return InvokeResult::error(WAB_INTERNAL, "the plate could not be selected");
    json out = plate_json(index);
    // With Preview shown, select_plate(i) leaves the previous plate's toolpaths on screen (its
    // "next plate of Slice all" branch). Clicking the plate's thumbnail in Preview reloads the
    // preview instead (select_plate(i, need_slice=true)); this does the same minus the reslice that
    // path starts for a stale plate -- it never calls select_sliced_plate, which can start a
    // slicing-service run (D7). A stale plate shows its shells until sliced.
    if (plater->is_preview_shown()) {
        PartPlate*                  plate  = plater->get_partplate_list().get_plate(index);
        const GCodeProcessorResult* result = plate->get_slice_result();
        const bool                  sliced = plate->is_slice_result_valid() && result != nullptr && !result->moves.empty();
        if (!sliced)
            plater->reset_gcode_toolpaths();
        plater->reload_print();
        if (sliced && wxGetApp().mainframe != nullptr)
            wxGetApp().mainframe->update_slice_print_status(MainFrame::eEventPlateUpdate, false, true);
        out["previewReloaded"] = true;
        out["previewShowsToolpaths"] = sliced;
    }
    return InvokeResult::ok(out);
}

InvokeResult run_rename(InvokeContext& ctx)
{
    const int index = target_plate(ctx);
    if (index < 0)
        return no_plate();
    const std::string name = ctx.params.value("name", std::string());
    std::string       why;
    if (!PP::valid_plate_name(name, why))
        return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params/name"}});
    PartPlate* plate = ctx.plater()->get_partplate_list().get_plate(index);
    if (plate->get_plate_name() == name)
        return InvokeResult::ok(plate_json(index)).with_effects({});
    ctx.snapshot("Rename plate");
    plate->set_plate_name(name);
    ctx.plater()->set_plater_dirty(true);
    ctx.plater()->get_current_canvas3D()->set_as_dirty();
    return InvokeResult::ok(plate_json(index));
}

InvokeResult run_lock(InvokeContext& ctx)
{
    const int index = target_plate(ctx);
    if (index < 0)
        return no_plate();
    PartPlateList& plates = ctx.plater()->get_partplate_list();
    const bool     locked = ctx.params.value("locked", true);
    if (plates.is_locked(index) == locked)
        return InvokeResult::ok(plate_json(index)).with_effects({});
    ctx.snapshot(locked ? "Lock plate" : "Unlock plate");
    plates.lock_plate(index, locked);
    ctx.plater()->get_current_canvas3D()->set_as_dirty();
    return InvokeResult::ok(plate_json(index));
}

InvokeResult run_set_settings(InvokeContext& ctx)
{
    Plater*        plater = ctx.plater();
    PartPlateList& plates = plater->get_partplate_list();
    const int      index  = target_plate(ctx);
    if (index < 0)
        return no_plate();
    PartPlate*  plate = plates.get_plate(index);
    const json& p     = ctx.params;
    std::string why;

    // Validate everything before changing anything.
    std::optional<BedType> bed;
    if (p.contains("bedType")) {
        bed = PP::bed_type_from_id(p["bedType"].get<std::string>());
        if (!bed)
            return InvokeResult::error(WAB_BAD_PARAMS, "unknown bed type", {{"path", "/params/bedType"}});
        if (std::string no = bed_type_unavailable(*bed); !no.empty())
            return InvokeResult::error(WAB_ACTION_DISABLED, no, {{"reason", "bed_type_unavailable"}, {"path", "/params/bedType"}});
    }
    std::optional<PrintSequence> seq;
    if (p.contains("printSequence")) {
        seq = PP::print_sequence_from_id(p["printSequence"].get<std::string>());
        if (!seq)
            return InvokeResult::error(WAB_BAD_PARAMS, "unknown print sequence", {{"path", "/params/printSequence"}});
    }
    const int                       filaments = int(wxGetApp().preset_bundle->filament_presets.size());
    std::optional<std::vector<int>> first_order;
    if (p.contains("firstLayerFilamentOrder")) {
        first_order = p["firstLayerFilamentOrder"].get<std::vector<int>>();
        if (!PP::valid_filament_order(*first_order, filaments, why))
            return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params/firstLayerFilamentOrder"}});
    }
    std::optional<std::vector<LayerPrintSequence>> other_order;
    if (p.contains("otherLayersFilamentOrder")) {
        other_order.emplace();
        for (size_t i = 0; i < p["otherLayersFilamentOrder"].size(); ++i) {
            const json&       r     = p["otherLayersFilamentOrder"][i];
            const std::string path  = "/params/otherLayersFilamentOrder/" + std::to_string(i);
            const int         from  = r["fromLayer"].get<int>();
            const int         to    = r["toLayer"].get<int>();
            std::vector<int>  order = r["order"].get<std::vector<int>>();
            if (to < from)
                return InvokeResult::error(WAB_BAD_PARAMS, "toLayer is below fromLayer", {{"path", path}});
            if (!PP::valid_filament_order(order, filaments, why))
                return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", path + "/order"}});
            other_order->push_back({{from, to}, order});
        }
    }
    std::optional<std::string> name;
    if (p.contains("name")) {
        name = p["name"].get<std::string>();
        if (!PP::valid_plate_name(*name, why))
            return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params/name"}});
    }
    const std::string spiral = p.value("spiralVase", std::string());
    if (p.contains("skirtStartAngle") && p.value("resetSkirtStartAngle", false))
        return InvokeResult::error(WAB_BAD_PARAMS, "give skirtStartAngle or resetSkirtStartAngle, not both", {{"path", "/params/resetSkirtStartAngle"}});
    std::optional<double> skirt;
    if (p.contains("skirtStartAngle")) {
        skirt = p["skirtStartAngle"].get<double>();
        if (!PP::valid_skirt_start_angle(*skirt))
            return InvokeResult::error(WAB_BAD_PARAMS, "skirtStartAngle is -180 to 180 degrees", {{"path", "/params/skirtStartAngle"}});
    }
    // A mixed-colour slot changes filament from layer to layer, so a custom filament order cannot be
    // honoured (the dialog and the plate tab grey the choice out); "automatic" ([]) stays allowed.
    const bool custom_order = (first_order && !first_order->empty()) || (other_order && !other_order->empty());
    if (custom_order) {
        const auto* is_mixed = wxGetApp().preset_bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
        if (is_mixed != nullptr && Slic3r::has_any_mixed_filament(is_mixed->values))
            return InvokeResult::error(WAB_ACTION_DISABLED,
                                       "the filament list contains mixed filaments, so a custom filament order would not take effect; "
                                       "leave the order automatic ([])",
                                       {{"reason", "mixed_filaments"},
                                        {"path", first_order && !first_order->empty() ? "/params/firstLayerFilamentOrder" : "/params/otherLayersFilamentOrder"}});
    }
    // Filament grouping (multi-nozzle printers): the Filament Grouping dialog's OK, minus the dialog
    // (Plater::open_filament_map_setting_dialog).
    std::optional<FilamentMapMode>  map_mode;
    std::optional<std::vector<int>> filament_maps;
    if (p.contains("filamentMapMode") || p.contains("filamentMaps")) {
        const int nozzles = wxGetApp().preset_bundle->get_printer_extruder_count();
        if (nozzles < 2)
            return InvokeResult::error(WAB_ACTION_DISABLED, "filament grouping applies to printers with more than one nozzle",
                                       {{"reason", "single_nozzle"}, {"path", p.contains("filamentMapMode") ? "/params/filamentMapMode" : "/params/filamentMaps"}});
        if (p.contains("filamentMapMode")) {
            map_mode = PP::filament_map_mode_from_id(p["filamentMapMode"].get<std::string>());
            if (!map_mode)
                return InvokeResult::error(WAB_BAD_PARAMS, "unknown filament map mode", {{"path", "/params/filamentMapMode"}});
        }
        if (p.contains("filamentMaps")) {
            if (map_mode != fmmManual)
                return InvokeResult::error(WAB_BAD_PARAMS, "filamentMaps goes with filamentMapMode \"manual\"", {{"path", "/params/filamentMaps"}});
            filament_maps = p["filamentMaps"].get<std::vector<int>>();
            if (!PP::valid_filament_maps(*filament_maps, filaments, nozzles, why))
                return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", "/params/filamentMaps"}});
        } else if (map_mode == fmmManual) {
            // Manual without a map keeps the plate's current grouping, as the dialog opens on it.
            filament_maps = plate->get_real_filament_maps(wxGetApp().preset_bundle->project_config);
            filament_maps->resize(size_t(filaments), 1);
        }
    }

    // The Plate Settings dialog's OK (Plater::open_platesettings_dialog), minus the dialog.
    ctx.snapshot("Plate settings");
    json applied = json::array();
    if (bed && plate->get_bed_type(false) != *bed) {
        plate->set_bed_type(*bed);
        applied.push_back("bedType");
    }
    if (seq && plate->get_print_seq() != *seq) {
        plate->set_print_seq(*seq);
        applied.push_back("printSequence");
    }
    // The plate tab's skirt start angle (TabPrintPlate::on_value_change / reset_model_config).
    if (skirt && !(plate->config()->has("skirt_start_angle") && plate->config()->opt_float("skirt_start_angle") == *skirt)) {
        plate->config()->set_key_value("skirt_start_angle", new ConfigOptionFloat(*skirt));
        applied.push_back("skirtStartAngle");
    }
    if (p.value("resetSkirtStartAngle", false) && plate->config()->has("skirt_start_angle")) {
        plate->reset_skirt_start_angle();
        applied.push_back("skirtStartAngle");
    }
    if (first_order && plate->get_first_layer_print_sequence() != *first_order) {
        plate->set_first_layer_print_sequence(*first_order);
        applied.push_back("firstLayerFilamentOrder");
    }
    if (other_order && plate->get_other_layers_print_sequence() != *other_order) {
        plate->set_other_layers_print_sequence(*other_order);
        applied.push_back("otherLayersFilamentOrder");
    }
    json vase_overrides = json::array();
    if (!spiral.empty() && spiral != spiral_id(plate)) {
        if (spiral == "default")
            plate->set_spiral_vase_mode(false, true);
        else if (spiral == "off")
            plate->set_spiral_vase_mode(false, false);
        else {
            // The dialog's "Enable" asks to confirm the per-object overrides spiral vase needs.
            // They are part of this (undoable) edit and are reported back instead.
            plate->config()->set_key_value("spiral_mode", new ConfigOptionBool(true));
            plate->set_vase_mode_related_object_config();
            vase_overrides = {"wall_loops",       "top_shell_layers", "sparse_infill_density", "enable_support",
                              "enforce_support_layers", "detect_thin_wall", "timelapse_type",   "overhang_reverse"};
        }
        plate->update_slice_result_valid_state(false);
        applied.push_back("spiralVase");
    }
    if (name && plate->get_plate_name() != *name) {
        plate->set_plate_name(*name);
        applied.push_back("name");
    }
    if (map_mode) {
        const std::vector<int> old_maps = plate->get_real_filament_maps(wxGetApp().preset_bundle->project_config);
        const FilamentMapMode  old_mode = plate->get_filament_map_mode();
        if (old_mode != *map_mode)
            plate->set_filament_map_mode(*map_mode);
        if (*map_mode == fmmManual && filament_maps)
            plate->set_filament_maps(*filament_maps);
        if (old_mode != *map_mode || (filament_maps && old_maps != *filament_maps)) {
            plate->update_slice_result_valid_state(false);
            applied.push_back("filamentMapMode");
        }
    }
    if (p.contains("locked") && plates.is_locked(index) != p["locked"].get<bool>()) {
        plates.lock_plate(index, p["locked"].get<bool>());
        applied.push_back("locked");
    }

    if (!applied.empty()) {
        plater->update_project_dirty_from_presets();
        plater->set_plater_dirty(true);
        plater->config_change_notification(*plate->config(), std::string("print_sequence"));
        plater->update();
        if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
            list->update_selections();
        plater->schedule_background_process();
    }
    json out       = plate_json(index);
    out["applied"] = applied;
    if (!vase_overrides.empty())
        out["objectOverridesSet"] = vase_overrides;
    if (applied.empty())
        return InvokeResult::ok(out).with_effects({});
    return InvokeResult::ok(out);
}

InvokeResult run_select_all(InvokeContext& ctx)
{
    Plater* plater = ctx.plater();
    if (ctx.params.value("allPlates", false))
        plater->select_all();
    else {
        const int index = target_plate(ctx);
        if (index < 0)
            return no_plate();
        if (index != plater->get_partplate_list().get_curr_plate_index())
            plater->select_plate(index);
        plater->select_curr_plate_all();
    }
    const ResolvedTargets sel  = current_selection();
    json                  objs = json::array();
    for (int o : sel.objects)
        objs.push_back(object_ref_json(plater->model(), o));
    return InvokeResult::ok({{"selected", objs}});
}

// Volumes whose source file cannot be found would make Reload All open a file dialog, and .obj
// sources a colour dialog; refuse instead, naming them.
std::vector<std::string> reload_blockers(const Model& model)
{
    namespace fs = boost::filesystem;
    std::vector<std::string> out;
    for (const ModelObject* o : model.objects)
        for (const ModelVolume* v : o->volumes) {
            const std::string& src = v->source.input_file;
            if (src.empty())
                continue;
            if (boost::iends_with(src, ".obj")) {
                out.push_back(v->name + " (.obj source)");
                continue;
            }
            boost::system::error_code ec;
            bool                      found = fs::exists(src, ec);
            if (!found && !o->input_file.empty()) {
                const fs::path alt = fs::path(o->input_file).remove_filename() / fs::path(src).filename();
                found              = fs::exists(alt, ec);
            }
            if (!found)
                out.push_back(v->name + " (" + fs::path(src).filename().string() + " not found)");
        }
    return out;
}

InvokeResult run_reload_all(InvokeContext& ctx)
{
    Plater* plater = ctx.plater();
    if (plater->model().objects.empty())
        return InvokeResult::ok({{"reloaded", false}}).with_effects({});
    if (const auto blockers = reload_blockers(plater->model()); !blockers.empty())
        return InvokeResult::error(WAB_ACTION_DISABLED, "some source files cannot be reloaded without asking the person",
                                   {{"reason", "sources_missing"}, {"volumes", blockers}});
    plater->set_prepare_state(Job::PREPARE_STATE_MENU);
    plater->reload_all_from_disk(); // its own "Reload all" snapshot
    return InvokeResult::ok({{"reloaded", true}});
}

InvokeResult orient_and_wait(InvokeContext& ctx, int prepare_state, json extra)
{
    Plater*   plater = ctx.plater();
    const int saved  = plater->get_prepare_state();
    plater->set_prepare_state(prepare_state);
    plater->orient(); // "Orient" snapshot, then OrientJob
    return ctx.after_plate_jobs([saved, extra](InvokeContext& c, bool timed_out) {
        c.plater()->set_prepare_state(saved);
        if (timed_out)
            return InvokeResult::error(WAB_INTERNAL, "auto-orient did not finish in time");
        json         out   = extra;
        json         objs  = json::array();
        const Model& model = c.plater()->model();
        for (size_t o = 0; o < model.objects.size(); ++o) {
            if (model.objects[o]->instances.empty())
                continue;
            const Vec3d r = model.objects[o]->instances.front()->get_rotation();
            objs.push_back({{"object", object_ref_json(model, int(o))},
                            {"rotationDeg", {Geometry::rad2deg(r.x()), Geometry::rad2deg(r.y()), Geometry::rad2deg(r.z())}}});
        }
        out["objects"] = objs;
        return InvokeResult::ok(out);
    });
}

InvokeResult run_plate_auto_orient(InvokeContext& ctx)
{
    if (!jobs_idle())
        return busy_jobs();
    const int index = target_plate(ctx);
    if (index < 0)
        return no_plate();
    Plater* plater = ctx.plater();
    if (plate_instances(plater->get_partplate_list().get_plate(index), plater->model()).empty())
        return InvokeResult::error(WAB_NOT_FOUND, "the plate is empty");
    if (index != plater->get_partplate_list().get_curr_plate_index())
        plater->select_plate(index);
    return orient_and_wait(ctx, Job::PREPARE_STATE_MENU, {{"plate", index}});
}

InvokeResult run_object_auto_orient(InvokeContext& ctx)
{
    if (!jobs_idle())
        return busy_jobs();
    if (ctx.targets.objects.empty())
        return InvokeResult::error(WAB_NOT_FOUND, "no object named", {{"missing", "objects"}});
    // The runtime selected the objects; orient acts on the selection.
    return orient_and_wait(ctx, Job::PREPARE_STATE_DEFAULT, json::object());
}

InvokeResult run_to_plate(InvokeContext& ctx)
{
    Plater*        plater = ctx.plater();
    PartPlateList& plates = plater->get_partplate_list();
    Model&         model  = plater->model();

    std::vector<std::pair<int, int>> moving = ctx.targets.instances;
    for (int o : ctx.targets.objects)
        for (size_t i = 0; i < model.objects[size_t(o)]->instances.size(); ++i)
            moving.emplace_back(o, int(i));
    std::sort(moving.begin(), moving.end());
    moving.erase(std::unique(moving.begin(), moving.end()), moving.end());
    if (moving.empty())
        return InvokeResult::error(WAB_NOT_FOUND, "no object or instance named", {{"missing", "instances"}});

    const bool new_plate = ctx.params.value("newPlate", false);
    int        dest      = ctx.params.value("plate", -1);
    if (!new_plate && (dest < 0 || dest >= plates.get_plate_count()))
        return InvokeResult::error(WAB_BAD_PARAMS, "plate out of range (" + std::to_string(plates.get_plate_count()) + " plates)",
                                   {{"path", "/params/plate"}});
    if (new_plate && !plater->can_add_plate())
        return InvokeResult::error(WAB_ACTION_DISABLED, "no more plates can be added", {{"reason", "plate_limit"}});
    if (!new_plate && plates.is_locked(dest))
        return InvokeResult::error(WAB_ACTION_DISABLED, "plate " + std::to_string(dest) + " is locked", {{"reason", "plate_locked"}});

    ctx.snapshot("Move to plate");
    if (new_plate) {
        plates.create_plate();
        dest = plates.get_plate_count() - 1;
        plater->update();
    }
    PartPlate*  target        = plates.get_plate(dest);
    const Vec3d target_origin = target->get_origin();
    const Vec3d target_center = target->get_bounding_box(false).center();

    // Instances keep their place relative to their plate, so several moved together keep their
    // layout; one that is on no plate lands centred on the destination.
    std::set<int> touched;
    json          moved = json::array();
    for (const auto& oi : moving) {
        const int      o    = oi.first;
        const int      i    = oi.second;
        ModelObject*   mo   = model.objects[size_t(o)];
        ModelInstance* inst = mo->instances[size_t(i)];
        const int      from = plates.find_instance_belongs(o, i);
        if (from == dest)
            continue;
        Vec3d delta = Vec3d::Zero();
        if (from >= 0)
            delta = target_origin - plates.get_plate(from)->get_origin();
        else {
            const Vec3d c = mo->instance_bounding_box(size_t(i)).center();
            delta         = Vec3d(target_center.x() - c.x(), target_center.y() - c.y(), 0.);
        }
        delta.z() = 0.;
        inst->set_offset(inst->get_offset() + delta);
        mo->invalidate_bounding_box();
        touched.insert(o);
        moved.push_back({{"object", object_ref_json(model, o)}, {"instance", i}, {"fromPlate", from}});
    }
    refile_objects(touched);

    json out            = plate_json(dest);
    out["plate"]        = dest;
    out["moved"]        = moved;
    out["createdPlate"] = new_plate;
    if (!ctx.params.value("arrange", false) || !jobs_idle())
        return InvokeResult::ok(out);

    // Then arrange the destination plate, as its Arrange icon does.
    plater->select_plate(dest);
    const int saved = plater->get_prepare_state();
    plater->set_prepare_state(Job::PREPARE_STATE_MENU);
    plater->arrange();
    return ctx.after_plate_jobs([saved, out, dest](InvokeContext& c, bool timed_out) {
        c.plater()->set_prepare_state(saved);
        if (timed_out)
            return InvokeResult::error(WAB_INTERNAL, "arrange did not finish in time");
        json fresh            = plate_json(dest);
        fresh["plate"]        = dest;
        fresh["moved"]        = out["moved"];
        fresh["createdPlate"] = out["createdPlate"];
        fresh["arranged"]     = true;
        return InvokeResult::ok(fresh);
    });
}

// The bed's "Move plate to the front" icon, for any place (Plater::move_plate_to_index): plate
// indices shift, so the new order comes back with each plate's previous index.
InvokeResult run_move(InvokeContext& ctx)
{
    Plater*        plater = ctx.plater();
    PartPlateList& plates = plater->get_partplate_list();
    const int      index  = target_plate(ctx);
    if (index < 0)
        return no_plate();
    const int count = plates.get_plate_count();
    const int to    = ctx.params.value("toIndex", 0);
    if (to >= count)
        return InvokeResult::error(WAB_BAD_PARAMS, "toIndex out of range (" + std::to_string(count) + " plates)", {{"path", "/params/toIndex"}});
    if (to == index) {
        json out     = plates_json();
        out["plate"] = index;
        return InvokeResult::ok(out).with_effects({});
    }
    ctx.snapshot("Move plate");
    if (plater->move_plate_to_index(index, to) != 0)
        return InvokeResult::error(WAB_INTERNAL, "the plate could not be moved");
    json out                 = plates_json();
    const std::vector<int> o = objlist::plate_order_after_move(count, index, to);
    for (size_t i = 0; i < out["plates"].size() && i < o.size(); ++i)
        out["plates"][i]["previousIndex"] = o[i];
    out["plate"] = to;
    out["from"]  = index;
    return InvokeResult::ok(out);
}

// Dragging the prime tower (GLCanvas3D::do_move -> project_config wipe_tower_x/y[plate], plate-
// relative mm). The scene clamps a tower that would not fit (PartPlate::estimate_wipe_tower_polygon);
// the same clamp is applied here and reported.
InvokeResult run_set_wipe_tower(InvokeContext& ctx)
{
    Plater*   plater = ctx.plater();
    const int index  = target_plate(ctx);
    if (index < 0)
        return no_plate();
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "no printer is selected");
    const double x = ctx.params.value("x", 0.), y = ctx.params.value("y", 0.);
    DynamicPrintConfig& proj = bundle->project_config;
    auto*               xo   = proj.option<ConfigOptionFloats>("wipe_tower_x", true);
    auto*               yo   = proj.option<ConfigOptionFloats>("wipe_tower_y", true);
    if (xo == nullptr || yo == nullptr || xo->values.empty() || yo->values.empty())
        return InvokeResult::error(WAB_INTERNAL, "the project has no prime tower position");
    const json previous = {{"x", xo->get_at(size_t(index))}, {"y", yo->get_at(size_t(index))}};

    ctx.snapshot("Move prime tower");
    ConfigOptionFloat nx(x), ny(y);
    xo->set_at(&nx, size_t(index), 0);
    yo->set_at(&ny, size_t(index), 0);
    // Where the scene would put a tower dropped there (inside the plate, clear of its edges).
    const DynamicPrintConfig full    = bundle->full_config();
    Vec3d                    pos     = Vec3d(x, y, 0.), size = Vec3d::Zero();
    bool                     clamped = false;
    if (full.has("wipe_tower_x") && full.has("wipe_tower_y")) {
        plater->get_partplate_list().get_plate(index)->estimate_wipe_tower_polygon(full, index, pos, size);
        if (std::abs(pos.x() - x) > 1e-3 || std::abs(pos.y() - y) > 1e-3) {
            ConfigOptionFloat cx(pos.x()), cy(pos.y());
            xo->set_at(&cx, size_t(index), 0);
            yo->set_at(&cy, size_t(index), 0);
            clamped = true;
        }
    }
    plater->set_plater_dirty(true);
    plater->update(); // reloads the scene's tower and re-arms slicing
    const bool enabled = full.has("enable_prime_tower") && full.opt_bool("enable_prime_tower");
    json out = {{"plate", index},
                {"x", xo->get_at(size_t(index))},
                {"y", yo->get_at(size_t(index))},
                {"requested", {{"x", x}, {"y", y}}},
                {"clamped", clamped},
                {"previous", previous},
                {"primeTowerEnabled", enabled}};
    if (!enabled)
        out["hint"] = "the prime tower is off (enable_prime_tower); the position applies once it is turned on with slicer.setSettings";
    return InvokeResult::ok(out);
}

InvokeResult run_slice_all(InvokeContext& ctx)
{
    Plater*        plater = ctx.plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (plater->only_gcode_mode())
        return InvokeResult::error(WAB_NOT_FOUND, "a G-code file is open; there is no model to slice");
    int printable = 0;
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (plates.get_plate(i)->has_printable_instances())
            ++printable;
    if (printable == 0)
        return InvokeResult::error(WAB_NOT_FOUND, "no plate has a printable object");
    // A slicing-service (cloud) mode is selected: as slicer.slice, refuse before anything changes.
    // The person presses Slice themselves, or a local mode is picked first (decision D7).
    if (MainFrame* frame = wxGetApp().mainframe; frame != nullptr && frame->slice_mode_is_service())
        return InvokeResult::error(WAB_SERVICE_MODE_SELECTED,
                                   "a slicing-service (cloud) mode is selected; the person must press Slice themselves, or pick a local "
                                   "mode first with slice.setMode {id:\"local_all\"}",
                                   {{"reason", "service_mode_selected"}, {"selected", frame->selected_slice_mode()}});
    // The Slice all button. It slices locally, plate after plate, and never starts a
    // slicing-service (cloud) run: those start only from the person's own Slice press.
    SimpleEvent evt(EVT_GLTOOLBAR_SLICE_ALL);
    plater->GetEventHandler()->ProcessEvent(evt);

    // Finish once slicing has stopped for good: every printable plate has a valid result, or
    // nothing has been slicing for a few seconds (a plate failed; slicer.getSliceIssues says why).
    // Each plate also reports slicer.sliceFinished as usual.
    std::shared_ptr<Deferred> d       = ctx.defer();
    auto*                     timer   = new wxTimer();
    const auto                started = std::chrono::steady_clock::now();
    auto                      idle    = std::make_shared<int>(0);
    timer->Bind(wxEVT_TIMER, [timer, d, idle, started](wxTimerEvent&) {
        Plater* p = wxGetApp().plater();
        if (p == nullptr || d->finished()) {
            timer->Stop();
            wxGetApp().CallAfter([timer]() { delete timer; });
            return;
        }
        PartPlateList& pl        = p->get_partplate_list();
        int            printable = 0, valid = 0;
        json           plates    = json::array();
        for (int i = 0; i < pl.get_plate_count(); ++i) {
            PartPlate* plate = pl.get_plate(i);
            if (!plate->has_printable_instances())
                continue;
            ++printable;
            const bool ok = plate->is_slice_result_valid();
            valid += ok ? 1 : 0;
            plates.push_back({{"plate", i}, {"sliceResultValid", ok}});
        }
        *idle = p->is_background_process_slicing() ? 0 : *idle + 1;
        const bool all_done = valid == printable && *idle >= 3;
        const bool stalled  = *idle >= 20;
        const bool late     = std::chrono::steady_clock::now() - started > std::chrono::minutes(15);
        if (!all_done && !stalled && !late)
            return;
        timer->Stop();
        wxGetApp().CallAfter([timer]() { delete timer; });
        d->finish(InvokeResult::ok({{"plates", plates}, {"sliced", valid}, {"printable", printable}, {"complete", valid == printable}}));
    });
    timer->Start(250);
    return InvokeResult::started();
}

// ---------------------------------------------------------------------------------------------
// Descriptors: PlatesPreviewDescriptors.hpp
// ---------------------------------------------------------------------------------------------

std::string can_add_plate_enabled(const InvokeContext&)
{
    Plater* p = wxGetApp().plater();
    return (p != nullptr && !p->can_add_plate()) ? "no more plates can be added" : "";
}

std::string can_delete_plate_enabled(const InvokeContext&)
{
    Plater* p = wxGetApp().plater();
    return (p != nullptr && p->get_partplate_list().get_plate_count() <= 1) ? "the last plate cannot be deleted" : "";
}

std::string several_plates_enabled(const InvokeContext&)
{
    Plater* p = wxGetApp().plater();
    return (p != nullptr && p->get_partplate_list().get_plate_count() <= 1) ? "there is only one plate" : "";
}

} // namespace

void register_plate_actions(Catalog& c)
{
    PP::add_bound(c, PP::plate_descriptors(),
                  {{"plate.list", {run_list, {}}},
                   {"plate.add", {run_add, can_add_plate_enabled}},
                   {"plate.delete", {run_delete, can_delete_plate_enabled}},
                   {"plate.duplicate", {run_duplicate, can_add_plate_enabled}},
                   {"plate.select", {run_select, {}}},
                   {"plate.rename", {run_rename, {}}},
                   {"plate.lock", {run_lock, {}}},
                   {"plate.setSettings", {run_set_settings, {}}},
                   {"plate.move", {run_move, several_plates_enabled}},
                   {"plate.setWipeTower", {run_set_wipe_tower, {}}},
                   {"plate.selectAll", {run_select_all, has_objects_enabled}},
                   {"plate.reloadAll", {run_reload_all, has_objects_enabled}},
                   {"plate.autoOrient", {run_plate_auto_orient, has_objects_enabled}},
                   {"object.autoOrient", {run_object_auto_orient, has_objects_enabled}},
                   {"object.toPlate", {run_to_plate, has_objects_enabled}},
                   {"slice.all", {run_slice_all, has_objects_enabled}}});
}

}}} // namespace Slic3r::GUI::Actions
