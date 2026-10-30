// Automation actions: file.export (the File > Export entries and the object menu's export items)
// and file.reloadGcode. Owner: work package S7. See docs/HLSD/bridge-actions.md for how to write an
// action and docs/bridge-actions/presets.md for what these do.
//
// Every export writes where the person picks in the native Save dialog (that choice is the consent;
// the page never names a path). The menu entries open their own dialogs from inside Plater, so the
// runners pick the path first and then call the same writers the menu does: Plater::export_3mf,
// the mesh merge of Plater::combine_mesh_fff with store_stl / store_drc, and the preview's
// export_toolpaths_to_obj.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "PresetLogic.hpp"
#include "Register.hpp"

#include <set>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "../GLCanvas3D.hpp"
#include "../GUI_App.hpp"
#include "../NotificationManager.hpp"
#include "../Plater.hpp"
#include "../WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json   = nlohmann::json;
namespace fs = boost::filesystem;

namespace {

json object_schema(json properties, std::vector<std::string> required = {})
{
    json s = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = required;
    return s;
}

std::string utf8(const wxString& s) { return std::string(s.ToUTF8().data()); }

std::string wildcard_for(const std::string& ext)
{
    if (ext == ".stl") return "STL files (*.stl)|*.stl";
    if (ext == ".drc") return "Draco files (*.drc)|*.drc";
    if (ext == ".3mf") return "3MF files (*.3mf)|*.3mf";
    if (ext == ".obj") return "OBJ files (*.obj)|*.obj";
    return "All files (*.*)|*.*";
}

// The objects an export covers: the target's, else every object.
std::vector<int> export_objects(const InvokeContext& ctx, const Model& model)
{
    if (!ctx.targets.objects.empty())
        return ctx.targets.objects;
    std::vector<int> all;
    for (int i = 0; i < static_cast<int>(model.objects.size()); ++i)
        all.push_back(i);
    return all;
}

bool store_mesh(const std::string& path, TriangleMesh& mesh, bool drc)
{
    if (!drc)
        return Slic3r::store_stl(path.c_str(), &mesh, true);
    int bits = DRC_BITS_DEFAULT;
    if (AppConfig* cfg = wxGetApp().app_config; cfg != nullptr && !cfg->get("drc_bits").empty())
        bits = std::atoi(cfg->get("drc_bits").c_str());
    return Slic3r::store_drc(path.c_str(), &mesh, bits);
}

InvokeResult export_meshes(InvokeContext& ctx, const PresetLogic::ExportKind& kind, const std::string& chosen)
{
    Plater*                  plater  = wxGetApp().plater();
    Model&                   model   = plater->model();
    const std::vector<int>   objects = export_objects(ctx, model);
    const bool               drc     = std::string(kind.id).rfind("drc", 0) == 0;
    const std::string        ext     = drc ? ".drc" : ".stl";
    auto notify = [plater](const std::string& msg) { plater->get_notification_manager()->push_general_error_notification(msg); };
    json                     written = json::array();

    if (!kind.per_object) {
        TriangleMesh mesh;
        for (int idx : objects)
            mesh.merge(Plater::combine_mesh_fff(*model.objects[idx], -1, notify));
        if (!store_mesh(chosen, mesh, drc))
            return InvokeResult::error(WAB_IO, "could not write " + fs::path(chosen).filename().string());
        written.push_back(fs::path(chosen).filename().string());
    } else {
        // One file per object, into the folder of the file the person picked; names never clobber.
        const fs::path        dir = fs::path(chosen).parent_path();
        std::set<std::string> taken;
        boost::system::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            taken.insert(boost::algorithm::to_lower_copy(it->path().filename().string()));
        for (int idx : objects) {
            const ModelObject* mo   = model.objects[idx];
            TriangleMesh       mesh = Plater::combine_mesh_fff(*mo, -1, notify);
            mesh.translate(-mo->origin_translation.cast<float>());
            const std::string name = PresetLogic::per_object_file_name(mo->name, ext, taken);
            if (!store_mesh((dir / name).string(), mesh, drc))
                return InvokeResult::error(WAB_IO, "could not write " + name, {{"written", written}});
            written.push_back(name);
        }
    }
    return InvokeResult::ok({{"kind", kind.id}, {"files", written}, {"folder", fs::path(chosen).parent_path().filename().string()}});
}

InvokeResult run_export(InvokeContext& ctx)
{
    Plater*                         plater = wxGetApp().plater();
    const std::string               kind_id = ctx.params.value("kind", "");
    const PresetLogic::ExportKind*  kind    = PresetLogic::find_export_kind(kind_id);
    if (kind == nullptr)
        return InvokeResult::error(WAB_BAD_PARAMS, "unknown export kind", {{"path", "/params/kind"}});
    if (kind->mesh && plater->model().objects.empty())
        return InvokeResult::error(WAB_NOT_FOUND, "the plate has no objects to export");
    if (kind->needs_slice && (!plater->is_preview_loaded() || !plater->has_toolpaths_to_export()))
        return InvokeResult::error(WAB_NOT_FOUND, "there are no toolpaths to export; slice the plate and open the preview first");

    if (kind_id == "publish3mf") {
        // The Publish flow keeps the settings the person chose for this session; its own native
        // Save dialog picks the file.
        std::vector<std::string>                    keys;
        std::vector<Slic3r::PublishedMaterialEntry> materials;
        bool                                        legacy_process = false;
        plater->get_pending_published(keys, materials, legacy_process);
        if (plater->export_published_3mf(keys, materials) != wxID_YES)
            return InvokeResult::error(WAB_USER_CANCELLED, "the export was cancelled or failed");
        return InvokeResult::ok({{"kind", kind_id}, {"publishedKeys", keys.size()}});
    }

    std::string base = ctx.params.value("fileName", "");
    if (base.empty()) {
        base = utf8(plater->get_project_name());
        if (kind->per_object && !plater->model().objects.empty())
            base = plater->model().objects[export_objects(ctx, plater->model()).front()]->name;
    }
    const std::string ext = kind->per_object ? (std::string(kind->id).rfind("drc", 0) == 0 ? ".drc" : ".stl") : kind->extension;
    if (fs::path(base).extension().string() != ext)
        base += ext;
    std::string chosen;
    if (!native_save_path(kind->per_object ? "Export objects (one file each, into this folder)" : "Export", base, wildcard_for(ext), chosen))
        return InvokeResult::error(WAB_USER_CANCELLED, "the export was cancelled");

    if (kind->mesh)
        return export_meshes(ctx, *kind, chosen);
    if (kind_id == "3mf") {
        if (plater->export_3mf(fs::path(chosen), SaveStrategy::Silence) < 0)
            return InvokeResult::error(WAB_IO, "could not write " + fs::path(chosen).filename().string());
    } else if (kind_id == "toolpathsObj") {
        plater->get_preview_canvas3D()->export_toolpaths_to_obj(chosen.c_str());
        if (!fs::exists(fs::path(chosen)))
            return InvokeResult::error(WAB_IO, "could not write " + fs::path(chosen).filename().string());
    }
    return InvokeResult::ok({{"kind", kind_id}, {"files", {fs::path(chosen).filename().string()}}});
}

InvokeResult run_reload_gcode(InvokeContext&)
{
    Plater* plater = wxGetApp().plater();
    const std::string file = fs::path(utf8(plater->get_last_loaded_gcode())).filename().string();
    plater->reload_gcode_from_disk();
    return InvokeResult::ok({{"file", file}});
}

std::string enabled_with_plater(const InvokeContext&)
{
    return wxGetApp().plater() != nullptr ? std::string() : std::string("the slicer is starting");
}

} // namespace

void register_file_actions(Catalog& c)
{
    {
        json kinds = json::array();
        for (const PresetLogic::ExportKind& k : PresetLogic::export_kinds())
            kinds.push_back(k.id);
        ActionDescriptor d;
        d.id      = "file.export";
        d.label   = "Export";
        d.summary = "Export to a file the person picks in the native Save dialog: stl/drc (objects merged into one mesh), stls/drcs (one "
                    "file per object), 3mf (generic 3MF), publish3mf, toolpathsObj (the sliced preview's toolpaths). Targets limit a mesh "
                    "export to those objects.";
        d.risk              = Risk::OutwardFile;
        d.gate              = Gate::Export;
        d.confirm_skip_rule = "the native Save dialog is the consent: the person picks the file";
        d.target            = {{"kind", "objects"}, {"required", false}};
        d.params            = object_schema({{"kind", {{"type", "string"}, {"enum", kinds}}},
                                             {"fileName", {{"type", "string"}, {"minLength", 1}, {"maxLength", 120},
                                                           {"description", "Suggested file name (per-object kinds: the first file's)."}}}},
                                            {"kind"});
        d.effects  = {"file"};
        d.ui       = {"MainFrame.cpp::menu::Export all objects as one STL", "MainFrame.cpp::menu::Export all objects as STLs",
                      "MainFrame.cpp::menu::Export all objects as one DRC", "MainFrame.cpp::menu::Export all objects as DRCs",
                      "MainFrame.cpp::menu::Export Generic 3MF",          "MainFrame.cpp::menu::Publish 3MF",
                      "MainFrame.cpp::menu::Export toolpaths as OBJ",
                      "GUI_Factories.cpp::menu::Export as one STL",       "GUI_Factories.cpp::menu::Export as STLs",
                      "GUI_Factories.cpp::menu::Export as one DRC",       "GUI_Factories.cpp::menu::Export as DRCs",
                      "MainFrame.cpp::submenu::Export"};
        d.examples = json::array({{{"target", nullptr}, {"params", {{"kind", "stl"}, {"fileName", "automated-test.stl"}}}}});
        c.add(std::move(d), run_export, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id       = "file.reloadGcode";
        d.label    = "Reload G-code";
        d.summary  = "Reload the G-code file shown in the G-code preview from disk.";
        d.risk     = Risk::Ui;
        d.params   = object_schema(json::object());
        d.effects  = {"view"};
        d.ui       = {"MainFrame.cpp::menu::Reload from Disk"};
        d.examples = json::array({{{"target", nullptr}, {"params", json::object()}}});
        c.add(std::move(d), run_reload_gcode, [](const InvokeContext&) -> std::string {
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr)
                return "the slicer is starting";
            if (!plater->only_gcode_mode() || plater->get_last_loaded_gcode().empty())
                return "no G-code file is open in the preview";
            return std::string();
        });
    }
}

}}} // namespace Slic3r::GUI::Actions
