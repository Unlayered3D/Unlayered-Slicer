// Automation actions: view.* (camera, projection, overlays, zoom), ui.* (tabs, settings, gizmo,
// sidebar) and printer.fetchBedMesh.
// Owner: work package S8 (filament/UI). See docs/HLSD/bridge-actions.md for how to write an action
// and docs/bridge-actions/filament-ui.md for these.
//
// These change what is shown, not the project: risk Ui (no undo snapshot, nothing to confirm),
// except ui.closeGizmo (it throws away an open gizmo's uncommitted edits: native confirmation),
// view.previewMeshInfill (adds a preview volume: an undoable model edit) and printer.fetchBedMesh
// (reads from the printer: behind the printer permission).

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "FilamentUiRules.hpp"
#include "ProjectCommon.hpp"
#include "Register.hpp"

#include <cmath>

#include "libslic3r/BedMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "../BedMeshOverlay.hpp"
#include "../GLCanvas3D.hpp"
#include "../GUI_App.hpp"
#include "../MainFrame.hpp"
#include "../Notebook.hpp"
#include "../ParamsDialog.hpp"
#include "../ParamsPanel.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../PresetComboBoxes.hpp"
#include "../ProjectAside.hpp"
#include "../Tab.hpp"
#include "../Notebook.hpp"
#include "../WebAppPanel.hpp"
#include "../WebAppSlot.hpp"
#include "../WebAppBridge.hpp"
#include "../Gizmos/GLGizmosManager.hpp"
#include "../Gizmos/GLGizmoBase.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using nlohmann::json;

// ---- pure rules (FilamentUiRules.hpp) --------------------------------------------------------

json bed_mesh_json(const BedMesh& mesh, const std::string& host, bool grid)
{
    if (mesh.empty())
        return {{"loaded", false}};
    // Statistics come from BedMesh itself, so they skip unprobed (NaN) nodes as the legend does.
    auto round3 = [](double v) { return std::round(v * 1000.) / 1000.; };
    json out = {{"loaded", true},
                {"host", host},
                {"profile", mesh.profile_name},
                {"source", mesh.source},
                {"compensating", mesh.loaded},
                {"min", {round3(mesh.min.x()), round3(mesh.min.y())}},
                {"max", {round3(mesh.max.x()), round3(mesh.max.y())}},
                {"rows", mesh.probed_rows()},
                {"cols", mesh.probed_cols()},
                {"probedPoints", mesh.probed_count()},
                {"zMin", round3(mesh.z_min())},
                {"zMax", round3(mesh.z_max())},
                {"zRange", round3(mesh.z_range())},
                {"outOfFlat", mesh.out_of_flat()}};
    if (!mesh.note.empty())
        out["note"] = mesh.note;
    if (grid) {
        json rows = json::array();
        for (const std::vector<float>& row : mesh.probed) {
            json r = json::array();
            for (float z : row)
                r.push_back(BedMesh::is_probed(z) ? json(round3(z)) : json(nullptr));
            rows.push_back(std::move(r));
        }
        out["probed"] = std::move(rows);
    }
    return out;
}

namespace {

json schema(json properties, std::vector<std::string> required = {})
{
    json s = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = required;
    return s;
}

json example(json params) { return json::array({json{{"target", nullptr}, {"params", std::move(params)}}}); }

void repaint(Plater* plater)
{
    if (GLCanvas3D* canvas = plater->get_current_canvas3D()) {
        canvas->set_as_dirty();
        canvas->request_extra_frame();
        canvas->post_event(SimpleEvent(wxEVT_PAINT));
    }
}

std::string needs_plater_view()
{
    MainFrame* frame = wxGetApp().mainframe;
    if (frame == nullptr || wxGetApp().plater() == nullptr)
        return "the main window is not ready";
    if (!wxGetApp().plater()->IsShown())
        return "the Prepare or Preview tab is not shown";
    return {};
}

bool plater_or_error(InvokeContext& ctx, InvokeResult& out)
{
    if (ctx.plater() != nullptr && wxGetApp().mainframe != nullptr)
        return true;
    out = InvokeResult::error(WAB_INTERNAL, "the plater is not available");
    return false;
}

// ---- view.camera / projection / zoomTo / assemblyView ----------------------------------------

InvokeResult run_camera(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    const std::string dir = ctx.params.value("direction", std::string("default"));
    if (const std::string why = needs_plater_view(); !why.empty())
        return InvokeResult::error(WAB_ACTION_DISABLED, why, {{"reason", why}});
    if (dir == "default") {
        // View > Default View
        wxGetApp().mainframe->select_view("plate");
        if (GLCanvas3D* canvas = ctx.plater()->get_current_canvas3D())
            canvas->zoom_to_bed();
    } else if (dir == "topFront") {
        // Ctrl+7: the current plate from the top front, centred on it (GLCanvas3D::select_plate).
        if (GLCanvas3D* canvas = ctx.plater()->get_current_canvas3D())
            canvas->select_plate();
    } else {
        ctx.plater()->select_view(dir);
    }
    repaint(ctx.plater());
    return InvokeResult::ok({{"direction", dir}});
}

InvokeResult run_projection(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    AppConfig* cfg = wxGetApp().app_config;
    if (ctx.params.contains("mode"))
        cfg->set_bool("use_perspective_camera", ctx.params["mode"].get<std::string>() == "perspective");
    if (ctx.params.contains("auto"))
        cfg->set_bool("auto_perspective", ctx.params["auto"].get<bool>());
    wxGetApp().update_ui_from_settings();
    repaint(ctx.plater());
    return InvokeResult::ok({{"mode", cfg->get_bool("use_perspective_camera") ? "perspective" : "orthogonal"},
                             {"auto", cfg->get_bool("auto_perspective")}});
}

InvokeResult run_zoom(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    const std::string what   = ctx.params.value("what", std::string("bed"));
    GLCanvas3D*       canvas = what == "gcode" ? ctx.plater()->get_preview_canvas3D() : ctx.plater()->get_current_canvas3D();
    if (canvas == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "no 3D view");
    if (what == "bed")
        canvas->zoom_to_bed();
    else if (what == "plate")
        canvas->zoom_to_plate(ctx.params.value("plate", -1));
    else if (what == "objects")
        canvas->zoom_to_volumes();
    else if (what == "selection")
        canvas->zoom_to_selection();
    else
        canvas->zoom_to_gcode();
    repaint(ctx.plater());
    return InvokeResult::ok({{"what", what}});
}

InvokeResult run_assembly_view(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    const bool on = ctx.params.value("on", true);
    wxGetApp().mainframe->select_tab(wxString(TAB_ID_PREPARE));
    ctx.plater()->select_view_3D(on ? "Assemble" : "3D");
    return InvokeResult::ok({{"on", on}});
}

// ---- view.toggle ------------------------------------------------------------------------------

const std::vector<std::string>& toggle_names()
{
    static const std::vector<std::string> names = {"gcodeWindow", "navigator", "gridlines",  "bedMesh",
                                                   "cutChunkColors", "labels", "overhang", "outline"};
    return names;
}

bool toggle_state(const std::string& what, Plater* plater)
{
    GUI_App& app = wxGetApp();
    if (what == "gcodeWindow") return app.show_gcode_window();
    if (what == "navigator") return app.show_3d_navigator();
    if (what == "gridlines") return app.show_plate_gridlines();
    if (what == "bedMesh") return app.show_bed_mesh();
    if (what == "cutChunkColors") return app.show_cut_chunk_colors();
    if (what == "labels") return plater->are_view3D_labels_shown();
    if (what == "overhang") return plater->is_view3D_overhang_shown();
    if (what == "outline") return app.show_outline();
    return false;
}

InvokeResult run_toggle(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    Plater*           plater = ctx.plater();
    GUI_App&          app    = wxGetApp();
    const std::string what   = ctx.params.value("what", std::string());
    const bool        before = toggle_state(what, plater);
    const bool        on     = ctx.params.contains("on") ? ctx.params["on"].get<bool>() : !before;
    json              result = {{"what", what}, {"on", on}, {"changed", on != before}};
    if (on != before) {
        if (what == "gcodeWindow") app.toggle_show_gcode_window();
        else if (what == "navigator") app.toggle_show_3d_navigator();
        else if (what == "gridlines") app.toggle_show_plate_gridlines();
        else if (what == "cutChunkColors") {
            app.toggle_show_cut_chunk_colors();
            if (GLCanvas3D* v = plater->get_view3D_canvas3D())
                v->set_as_dirty();
        } else if (what == "labels") plater->show_view3D_labels(on);
        else if (what == "overhang") plater->show_view3D_overhang(on);
        else if (what == "outline") app.toggle_show_outline();
        else if (what == "bedMesh") {
            // View > Show Bed Mesh also reads the mesh from the printer when none is loaded; that
            // read is printer.fetchBedMesh's (printer permission), so only the overlay switches here.
            app.toggle_show_bed_mesh();
        }
    }
    if (what == "bedMesh")
        result["meshLoaded"] = plater->get_partplate_list().bed_mesh_overlay().has_mesh();
    repaint(plater);
    if (on == before)
        return InvokeResult::ok(result).with_effects({});
    return InvokeResult::ok(result);
}

// ---- view.previewMeshInfill -------------------------------------------------------------------

InvokeResult run_mesh_infill(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    if (ctx.model().objects.empty())
        return InvokeResult::error(WAB_NOT_FOUND, "the plate has no objects");
    const int obj = ctx.targets.objects.empty() ? std::max(0, ctx.plater()->get_selected_object_idx()) : ctx.targets.objects.front();
    if (obj < 0 || size_t(obj) >= ctx.model().objects.size())
        return InvokeResult::error(WAB_NOT_FOUND, "no such object");
    // Plater::preview_mesh_infill without its message boxes: what they would say is the error. A
    // dry run first, so an up-to-date preview takes no undo snapshot (refresh_infill_previews too).
    std::string error, warning;
    if (!ctx.plater()->generate_infill_preview(obj, /*dry_run=*/true, error, warning)) {
        if (!error.empty())
            return InvokeResult::error(WAB_BAD_PARAMS, error + "; change the object's infill settings first",
                                       {{"reason", "infill-preview-unavailable"}, {"object", object_ref_json(ctx.model(), obj)}});
        return InvokeResult::ok({{"object", object_ref_json(ctx.model(), obj)}, {"rebuilt", false}, {"upToDate", true}}).with_effects({});
    }
    ctx.snapshot();
    if (!ctx.plater()->generate_infill_preview(obj, /*dry_run=*/false, error, warning))
        return InvokeResult::error(WAB_BAD_PARAMS, error.empty() ? std::string("no infill preview was built") : error,
                                   {{"reason", "infill-preview-unavailable"}, {"object", object_ref_json(ctx.model(), obj)}});
    json result = {{"object", object_ref_json(ctx.model(), obj)}, {"rebuilt", true}};
    if (!warning.empty())
        result["warning"] = warning;
    return InvokeResult::ok(result);
}

// ---- ui.* --------------------------------------------------------------------------------------

Preset::Type settings_type(const std::string& t)
{
    return t == "print" ? Preset::TYPE_PRINT : t == "filament" ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINTER;
}

// PlaterPresetComboBox::switch_to_tab for print/printer: the settings live in the Prepare tab's
// parameter panel, or in the floating parameter dialog.
void show_settings_tab(Tab* tab)
{
    if (tab->GetParent() == wxGetApp().params_panel())
        wxGetApp().mainframe->select_tab(wxString(TAB_ID_PREPARE));
    else {
        wxGetApp().params_dialog()->Popup();
        tab->OnActivate();
    }
    tab->restore_last_select_item();
}

// Shows the filament settings of `slot` (the sidebar's edit button): the Filament tab switches to
// that slot's preset, which is a preset switch when the tab holds another one.
bool show_filament_slot(InvokeContext& ctx, int slot, InvokeResult& out)
{
    PresetBundle& b = *wxGetApp().preset_bundle;
    if (slot < 0 || size_t(slot) >= b.filament_presets.size() || b.is_mixed_filament(size_t(slot))) {
        out = InvokeResult::error(WAB_BAD_PARAMS, "slot must be a physical filament slot", {{"path", "/params/slot"}});
        return false;
    }
    auto& combos = ctx.plater()->sidebar().combos_filament();
    if (size_t(slot) >= combos.size()) {
        out = InvokeResult::error(WAB_INTERNAL, "the sidebar has no control for this slot");
        return false;
    }
    if (b.filaments.get_edited_preset().name != b.filament_presets[size_t(slot)] && b.filaments.current_is_dirty()) {
        if (!ctx.preset_dirty_preflight(Preset::TYPE_FILAMENT, out))
            return false;
    }
    if (!combos[size_t(slot)]->switch_to_tab()) {
        out = InvokeResult::error(WAB_USER_CANCELLED, "the filament settings were not shown");
        return false;
    }
    return true;
}

// The Universe tab exists only while its slot is enabled (a restart setting). With url, it opens that
// page of the Universe site: only URLs on the tab's own configured origin, never another site.
InvokeResult show_universe(InvokeContext& ctx)
{
    MainFrame* frame = wxGetApp().mainframe;
    if (frame->m_tabpanel == nullptr || frame->m_universe_view == nullptr ||
        frame->m_tabpanel->FindPageByName(TAB_ID_UNIVERSE) == wxNOT_FOUND)
        return InvokeResult::error(WAB_ACTION_DISABLED, "the Universe tab is turned off in this slicer",
                                   {{"reason", "universe-off"}, {"path", "/params/tab"}});
    if (ctx.params.contains("url")) {
        const wxString url  = wxString::FromUTF8(ctx.params["url"].get<std::string>());
        const wxString home = WebAppPanel::origin_of(WebAppPanel::configured_url(WebAppSlot::universe()));
        if (home.empty() || WebAppPanel::origin_of(url) != home || !WebAppPanel::is_secure_context(url))
            return InvokeResult::error(WAB_BAD_PARAMS, "url must be a page of the Universe site (" + home.ToStdString() + ")",
                                       {{"path", "/params/url"}});
        // After this reply: the page asking may be the Universe tab itself.
        WebAppPanel* panel = frame->m_universe_view;
        wxGetApp().CallAfter([panel, url]() {
            if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->m_universe_view == panel)
                panel->load_url(url);
        });
    }
    frame->select_tab(wxString(TAB_ID_UNIVERSE));
    json result = {{"tab", "universe"}};
    if (ctx.params.contains("url"))
        result["url"] = ctx.params["url"];
    return InvokeResult::ok(result);
}

InvokeResult run_show_tab(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    const std::string tab = ctx.params.value("tab", std::string("prepare"));
    static const std::map<std::string, const char*> pages = {
        {"home", TAB_ID_HOME},       {"prepare", TAB_ID_PREPARE}, {"preview", TAB_ID_PREVIEW},
        {"device", TAB_ID_MONITOR},  {"project", TAB_ID_PROJECT}, {"calibration", TAB_ID_CALIBRATION},
    };
    if (auto it = pages.find(tab); it != pages.end()) {
        MainFrame* frame = wxGetApp().mainframe;
        Plater*    p     = ctx.plater();
        if (tab == "prepare" && (p->only_gcode_mode() || p->using_exported_file())) {
            // A G-code preview: clicking Prepare brings back the project it set aside, or asks to
            // close the preview (MainFrame::preview_only_hint). The first is done here too; the
            // second is project.new, which honours ifProjectDirty.
            if (has_project_aside(*p))
                return restore_project_aside_action(ctx);
            return InvokeResult::error(WAB_ACTION_DISABLED,
                                       "a G-code file is open in preview-only mode; leaving it closes the file: use project.new",
                                       {{"reason", "gcode_preview"}});
        }
        // Some tabs exist only for some printers or not at all in this slicer (Project and
        // Calibration are removed for every printer): report it rather than claim success.
        if (frame->m_tabpanel == nullptr || frame->m_tabpanel->FindPageByName(it->second) == wxNOT_FOUND)
            return InvokeResult::error(WAB_NOT_FOUND,
                                       "the " + tab + " tab is not shown in this slicer" +
                                           (tab == "project"     ? std::string(" (project.info and project.setInfo cover it)") :
                                            tab == "calibration" ? std::string(" (calib.run builds the calibration tests)") :
                                                                   std::string()),
                                       {{"reason", "tab_unavailable"}, {"tab", tab}});
        frame->select_tab(wxString(it->second));
        if (frame->m_tabpanel->GetSelectedPageName() != wxString(it->second))
            return InvokeResult::error(WAB_ACTION_DISABLED, "the " + tab + " tab could not be shown right now",
                                       {{"reason", "tab_not_selected"}, {"tab", tab}});
        return InvokeResult::ok({{"tab", tab}});
    }
    if (tab == "universe")
        return show_universe(ctx);
    if (tab == "filament") {
        if (!show_filament_slot(ctx, ctx.params.value("slot", 0), out))
            return out;
        return InvokeResult::ok({{"tab", tab}, {"slot", ctx.params.value("slot", 0)}});
    }
    Tab* t = wxGetApp().get_tab(settings_type(tab));
    if (t == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "no " + tab + " settings tab");
    show_settings_tab(t);
    return InvokeResult::ok({{"tab", tab}});
}

InvokeResult run_reveal_setting(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    const std::string type = ctx.params.value("type", std::string("print"));
    const std::string key  = ctx.params.value("key", std::string());
    if (print_config_def.get(key) == nullptr)
        return InvokeResult::error(WAB_NOT_FOUND, "no setting is named \"" + key + "\"", {{"path", "/params/key"}});
    Tab*          tab = wxGetApp().get_tab(settings_type(type));
    PresetBundle& b   = *wxGetApp().preset_bundle;
    const DynamicPrintConfig& cfg = type == "print" ? b.prints.get_edited_preset().config :
                                    type == "filament" ? b.filaments.get_edited_preset().config :
                                                         b.printers.get_edited_preset().config;
    if (tab == nullptr || !cfg.has(key))
        return InvokeResult::error(WAB_NOT_FOUND, "the " + type + " settings have no \"" + key + "\"", {{"path", "/params/key"}});
    if (type == "filament" && ctx.params.contains("slot") && !show_filament_slot(ctx, ctx.params["slot"].get<int>(), out))
        return out;
    // The sidebar search's jump: switches to the settings, opens the page holding the option and
    // highlights it.
    ctx.plater()->sidebar().jump_to_option(key, settings_type(type), std::wstring());
    return InvokeResult::ok({{"type", type}, {"key", key}});
}

GLGizmosManager* gizmos(Plater* plater)
{
    GLCanvas3D* canvas = plater ? plater->get_view3D_canvas3D() : nullptr;
    return canvas ? &canvas->get_gizmos_manager() : nullptr;
}

InvokeResult run_close_gizmo(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    GLGizmosManager* mgr = gizmos(ctx.plater());
    if (mgr == nullptr || mgr->get_current_type() == GLGizmosManager::Undefined)
        return InvokeResult::ok({{"closed", nullptr}}).with_effects({});
    GLGizmoBase*      g    = mgr->get_current();
    const std::string name = g ? g->get_name(false) : std::string("tool");
    ConfirmRequest    req;
    req.title        = "Close " + name;
    req.message      = "Close the " + name + " tool? Changes made in it that were not applied yet are discarded.";
    req.accept_label = "Close";
    if (!ctx.confirm(req))
        return InvokeResult::error(WAB_USER_CANCELLED, "the person kept the tool open");
    mgr->reset_all_states();
    repaint(ctx.plater());
    return InvokeResult::ok({{"closed", name}});
}

InvokeResult run_collapse_sidebar(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    const bool collapse = ctx.params.value("collapsed", true);
    const bool before   = ctx.plater()->is_sidebar_collapsed();
    if (before != collapse)
        ctx.plater()->collapse_sidebar(collapse);
    return InvokeResult::ok({{"collapsed", ctx.plater()->is_sidebar_collapsed()}, {"changed", before != collapse}});
}

InvokeResult run_reset_layout(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    ctx.plater()->reset_window_layout();
    return InvokeResult::ok();
}

// ---- printer.fetchBedMesh ---------------------------------------------------------------------

InvokeResult run_fetch_bed_mesh(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    // Reads the mesh the printer's host reports (Moonraker, OctoPrint or Duet) in the background, as
    // View > Reload Bed Mesh from Printer does (same notifications); the action finishes with the
    // outcome once the overlay has the mesh or the read failed.
    if (ctx.params.value("show", true) && !wxGetApp().show_bed_mesh())
        wxGetApp().toggle_show_bed_mesh();
    std::shared_ptr<Deferred> deferred = ctx.defer();
    std::weak_ptr<bool>       alive;
    if (ctx.host != nullptr)
        alive = ctx.host->alive;
    const bool watch = ctx.host != nullptr;
    ctx.plater()->fetch_bed_mesh([deferred, alive, watch](bool ok, const std::string& error) {
        if (watch && alive.expired())
            return;
        if (!ok) {
            deferred->finish(InvokeResult::error(WAB_IO, error + "; check the printer's host settings and that it is on",
                                                 {{"reason", "bed-mesh-unavailable"}}));
            return;
        }
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            deferred->finish(InvokeResult::error(WAB_INTERNAL, "the plater is not available"));
            return;
        }
        const BedMeshOverlay& overlay = plater->get_partplate_list().bed_mesh_overlay();
        json result = bed_mesh_json(overlay.mesh(), overlay.host(), false);
        result["showing"] = wxGetApp().show_bed_mesh();
        deferred->finish(InvokeResult::ok(result));
    });
    return InvokeResult::started();
}

// ---- printer.bedMesh --------------------------------------------------------------------------

InvokeResult run_bed_mesh(InvokeContext& ctx)
{
    InvokeResult out;
    if (!plater_or_error(ctx, out))
        return out;
    // What the overlay holds: the last mesh read from the printer. Reading it here talks to nothing.
    const BedMeshOverlay& overlay = ctx.plater()->get_partplate_list().bed_mesh_overlay();
    json result = bed_mesh_json(overlay.mesh(), overlay.host(), ctx.params.value("grid", true));
    result["showing"] = wxGetApp().show_bed_mesh();
    return InvokeResult::ok(result);
}

} // namespace

void register_view_actions(Catalog& c)
{
    {
        ActionDescriptor d;
        d.id       = "view.camera";
        d.label    = "Camera view";
        d.summary  = "Point the 3D view's camera: default (the whole plate), iso, topFront (the current plate from the top front, "
                     "as Ctrl+7), top, bottom, front, rear, left or right.";
        d.risk     = Risk::Ui;
        d.params   = schema({{"direction", {{"type", "string"}, {"enum", {"default", "iso", "topFront", "top", "bottom", "front", "rear", "left", "right"}}}}},
                            {"direction"});
        d.effects  = {"view"};
        d.ui       = {"MainFrame.cpp::menu::Default View", "MainFrame.cpp::menu::Top", "MainFrame.cpp::menu::Bottom", "MainFrame.cpp::menu::Front",
                      "MainFrame.cpp::menu::Rear", "MainFrame.cpp::menu::Left", "MainFrame.cpp::menu::Right"};
        d.examples = example({{"direction", "front"}});
        c.add(std::move(d), run_camera, [](const InvokeContext&) { return needs_plater_view(); });
    }
    {
        ActionDescriptor d;
        d.id       = "view.projection";
        d.label    = "Camera projection";
        d.summary  = "Use a perspective or orthogonal camera, and switch automatic perspective (orthographic for top/side views) on or off.";
        d.risk     = Risk::Ui;
        d.params   = schema({{"mode", {{"type", "string"}, {"enum", {"perspective", "orthogonal"}}}},
                             {"auto", {{"type", "boolean"}, {"description", "Auto Perspective."}}}});
        d.effects  = {"view"};
        d.ui       = {"MainFrame.cpp::menu::Use Perspective View", "MainFrame.cpp::menu::Use Orthogonal View", "MainFrame.cpp::menu::Auto Perspective"};
        d.examples = example({{"mode", "orthogonal"}});
        c.add(std::move(d), run_projection);
    }
    {
        ActionDescriptor d;
        d.id      = "view.toggle";
        d.label   = "Show in 3D view";
        d.summary = "Show or hide a 3D view overlay: G-code window, 3D navigator, gridlines, bed mesh height map, cut chunk colours, "
                    "object labels, overhang highlight or selected outline. Without on, it flips.";
        d.risk    = Risk::Ui;
        d.params  = schema({{"what", {{"type", "string"}, {"enum", toggle_names()}}}, {"on", {{"type", "boolean"}}}}, {"what"});
        d.effects = {"view"};
        d.ui      = {"MainFrame.cpp::menu::Show G-code Window", "MainFrame.cpp::menu::Show 3D Navigator", "MainFrame.cpp::menu::Show Gridlines",
                     "MainFrame.cpp::menu::Show Bed Mesh", "MainFrame.cpp::menu::Color Cut Chunks", "MainFrame.cpp::menu::Show Labels",
                     "GUI_Factories.cpp::menu::Show Labels", "MainFrame.cpp::menu::Show Overhang", "MainFrame.cpp::menu::Show Selected Outline (beta)",
                     "Preferences.cpp::pref::Color cut chunks"};
        d.examples = example({{"what", "gridlines"}, {"on", false}});
        c.add(std::move(d), run_toggle);
    }
    {
        ActionDescriptor d;
        d.id       = "view.zoomTo";
        d.label    = "Zoom to";
        d.summary  = "Fit the 3D view to the bed, a plate, all objects, the selection or the sliced toolpaths.";
        d.risk     = Risk::Ui;
        d.params   = schema({{"what", {{"type", "string"}, {"enum", {"bed", "plate", "objects", "selection", "gcode"}}}},
                             {"plate", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"description", "0-based plate for what=plate; absent = current."}}}},
                            {"what"});
        d.effects  = {"view"};
        d.examples = example({{"what", "objects"}});
        c.add(std::move(d), run_zoom, [](const InvokeContext&) { return needs_plater_view(); });
    }
    {
        ActionDescriptor d;
        d.id       = "view.assemblyView";
        d.label    = "Assembly view";
        d.summary  = "Switch the Prepare tab between the assembly view (parts exploded around the object) and the normal 3D view.";
        d.risk     = Risk::Ui;
        d.params   = schema({{"on", {{"type", "boolean"}, {"default", true}}}});
        d.effects  = {"view"};
        d.ui       = {"GLCanvas3D.cpp::toolbar::assembly_view"};
        d.examples = example({{"on", false}});
        c.add(std::move(d), run_assembly_view);
    }
    {
        ActionDescriptor d;
        d.id       = "view.previewMeshInfill";
        d.label    = "Preview Mesh Infill";
        d.summary  = "Generate the mesh-backed infill of an object as a non-printing preview volume that follows it (run again after "
                     "changes to refresh; an up-to-date preview is left as is). Refused when the object's infill cannot be previewed.";
        d.risk     = Risk::Mutate;
        d.target   = {{"kind", "object"}, {"required", true}};
        d.effects  = {"structure"};
        d.ui       = {"MainFrame.cpp::menu::Preview Mesh Infill"};
        d.examples = json::array({json{{"target", {{"objects", json::array({json{{"index", 0}}})}}}, {"params", json::object()}}});
        c.add(std::move(d), run_mesh_infill);
    }
    {
        ActionDescriptor d;
        d.id      = "ui.showTab";
        d.label   = "Show tab";
        d.summary = "Show a main tab (home, prepare, preview, device, project, calibration, universe) or the print, filament (of a slot) "
                    "or printer settings. The universe tab can open a page of its own site.";
        d.risk    = Risk::Ui;
        d.params  = schema({{"tab", {{"type", "string"},
                                     {"enum", {"home", "prepare", "preview", "device", "project", "calibration", "universe", "print", "filament",
                                               "printer"}}}},
                            {"slot", {{"type", "integer"}, {"minimum", 0}, {"maximum", 63}, {"description", "tab=filament: 0-based slot (default 0)."}}},
                            {"url", {{"type", "string"}, {"minLength", 8}, {"maxLength", 2048},
                                     {"description", "tab=universe: a page on the Universe site's own origin to open there."}}}},
                           {"tab"});
        d.effects = {"view"};
        d.ui      = {"Plater.cpp::sidebar::btn_edit_printer", "Plater.cpp::sidebar::edit_btn", "GUI_Factories.cpp::menu::Edit"};
        d.examples = example({{"tab", "print"}});
        c.add(std::move(d), run_show_tab);
    }
    {
        ActionDescriptor d;
        d.id       = "ui.revealSetting";
        d.label    = "Show setting";
        d.summary  = "Open the settings page holding a setting and highlight it, so the person can see or type it (e.g. print host keys).";
        d.risk     = Risk::Ui;
        d.params   = schema({{"type", {{"type", "string"}, {"enum", {"print", "filament", "printer"}}}},
                             {"key", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}, {"description", "Config key, e.g. \"wall_loops\"."}}},
                             {"slot", {{"type", "integer"}, {"minimum", 0}, {"maximum", 63}, {"description", "type=filament: whose settings."}}}},
                            {"type", "key"});
        d.effects  = {"view"};
        d.examples = example({{"type", "print"}, {"key", "wall_loops"}});
        c.add(std::move(d), run_reveal_setting);
    }
    {
        ActionDescriptor d;
        d.id       = "ui.closeGizmo";
        d.label    = "Close tool";
        d.summary  = "Close the open 3D-view tool (gizmo) so other edits can run; its unapplied changes are lost. The person confirms in a slicer dialog.";
        d.risk     = Risk::Destructive;
        // The one write that must run while a gizmo is open: it is how automation gets out of one.
        d.allow_with_gizmo = true;
        d.effects  = {"view"};
        d.examples = example(json::object());
        // It exists to run while a gizmo is open: without this the gizmo guard refuses it then.
        d.allow_with_gizmo = true;
        c.add(std::move(d), run_close_gizmo, [](const InvokeContext&) {
            GLGizmosManager* mgr = gizmos(wxGetApp().plater());
            return (mgr != nullptr && mgr->get_current_type() != GLGizmosManager::Undefined) ? std::string() : std::string("no tool is open");
        });
    }
    {
        ActionDescriptor d;
        d.id       = "ui.collapseSidebar";
        d.label    = "Collapse sidebar";
        d.summary  = "Collapse or expand the Prepare tab's settings sidebar.";
        d.risk     = Risk::Ui;
        d.params   = schema({{"collapsed", {{"type", "boolean"}}}}, {"collapsed"});
        d.effects  = {"view"};
        d.examples = example({{"collapsed", false}});
        c.add(std::move(d), run_collapse_sidebar);
    }
    {
        ActionDescriptor d;
        d.id       = "ui.resetWindowLayout";
        d.label    = "Reset Window Layout";
        d.summary  = "Put the window's panels back to the default layout.";
        d.risk     = Risk::Ui;
        d.effects  = {"view"};
        d.ui       = {"MainFrame.cpp::menu::Reset Window Layout"};
        d.examples = example(json::object());
        c.add(std::move(d), run_reset_layout, [](const InvokeContext&) {
            Plater* p = wxGetApp().plater();
            return (p != nullptr && p->is_sidebar_enabled()) ? std::string() : std::string("the sidebar is not shown");
        });
    }
    {
        ActionDescriptor d;
        d.id      = "printer.fetchBedMesh";
        d.label   = "Reload Bed Mesh from Printer";
        d.summary = "Read the bed mesh the printer has loaded (Moonraker, OctoPrint or Duet host) and show it as a height map on the "
                    "plate. Finishes when the mesh arrived (its summary; loaded false when the printer has none) or the read failed.";
        d.risk    = Risk::OutwardPrinter;
        d.long_running = true;
        d.gate    = Gate::Printer;
        d.confirm_skip_rule =
            "only reads the mesh the printer already has loaded from the printer host of the selected preset; nothing is sent that "
            "moves or changes the printer, and the printer permission (off by default) gates it";
        d.params   = schema({{"show", {{"type", "boolean"}, {"default", true}, {"description", "Also switch the bed mesh overlay on."}}}});
        d.effects  = {"view", "printer"};
        d.ui       = {"MainFrame.cpp::menu::Reload Bed Mesh from Printer"};
        d.examples = example({{"show", true}});
        c.add(std::move(d), run_fetch_bed_mesh);
    }
    {
        ActionDescriptor d;
        d.id       = "printer.bedMesh";
        d.label    = "Bed mesh";
        d.summary  = "The bed mesh last read from the printer (printer.fetchBedMesh or View > Show Bed Mesh): host, profile, grid corners, "
                     "probe grid and flatness in mm. Talks to nothing; loaded is false when no mesh was read.";
        d.risk     = Risk::Read;
        d.params   = schema({{"grid", {{"type", "boolean"}, {"default", true}, {"description", "Include the probed heights."}}}});
        d.effects  = {};
        d.examples = example({{"grid", false}});
        c.add(std::move(d), run_bed_mesh);
    }
}

}}} // namespace Slic3r::GUI::Actions
