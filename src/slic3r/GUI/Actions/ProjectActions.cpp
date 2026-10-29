// Automation actions: project.* (new, save, save as, info, open a recent project, import a model or
// ZIP archive from a URL). Owner: work package S7. See docs/HLSD/bridge-actions.md for how to write
// an action and docs/bridge-actions/presets.md for what these do.
//
// Replacing the project honours ifProjectDirty (ctx.project_dirty_preflight), so the person's
// unsaved project is never dropped without them: "ask" refuses with 1013, "save" saves it first,
// "prompt" leaves the slicer's own question to them. Saving over the open project's own file asks
// natively every time (decision D3); a first save, and Save As, use the native Save dialog.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "ProjectCommon.hpp"
#include "Register.hpp"

#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <wx/arrstr.h>
#include <wx/string.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"

#include "../GUI_App.hpp"
#include "../GUI_Utils.hpp"
#include "../MainFrame.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../Project.hpp"
#include "../ProjectAside.hpp"
#include "../StepMeshDialog.hpp"
#include "../WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json   = nlohmann::json;
namespace fs = boost::filesystem;

// ---- pure rules (ProjectCommon.hpp) -----------------------------------------------------------

const std::vector<std::string>& project_licenses()
{
    static const std::vector<std::string> licenses = {"", "CC0", "BY", "BY-SA", "BY-ND", "BY-NC", "BY-NC-SA", "BY-NC-ND"};
    return licenses;
}

const std::vector<AuxFolder>& aux_folders()
{
    // AuxiliaryPanel::on_import_file's folders and file filters.
    static const std::vector<AuxFolder> folders = {{"pictures", "Model Pictures", {"png", "jpg", "jpeg", "bmp"}},
                                                   {"bom", "Bill of Materials", {"xls", "xlsx", "pdf"}},
                                                   {"assembly", "Assembly Guide", {"pdf"}},
                                                   {"others", "Others", {"txt"}}};
    return folders;
}

const AuxFolder* find_aux_folder(const std::string& wire)
{
    for (const AuxFolder& f : aux_folders())
        if (f.wire == wire)
            return &f;
    return nullptr;
}

bool aux_file_name_ok(const std::string& name)
{
    if (name.empty() || name.size() > 255 || name == "." || name == "..")
        return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == '/' || c == '\\' || c == ':')
            return false;
    return true;
}

std::string aux_unique_name(const std::string& name, const std::vector<std::string>& taken)
{
    auto is_taken = [&taken](const std::string& n) { return std::find(taken.begin(), taken.end(), n) != taken.end(); };
    if (!is_taken(name))
        return name;
    const std::string::size_type dot  = name.rfind('.');
    const std::string            stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string            ext  = dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot);
    for (int i = 2;; ++i)
        if (const std::string candidate = stem + "_" + std::to_string(i) + ext; !is_taken(candidate))
            return candidate;
}

namespace {

json object_schema(json properties, std::vector<std::string> required = {})
{
    json s = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = required;
    return s;
}

json one_example(json params) { return json::array({{{"target", nullptr}, {"params", std::move(params)}}}); }

std::string utf8(const wxString& s) { return std::string(s.ToUTF8().data()); }

// The open project's own file, or "" when it was never saved.
std::string project_path(Plater& plater) { return utf8(plater.get_project_filename(".3mf")); }

std::string enabled_with_plater(const InvokeContext&)
{
    return wxGetApp().plater() != nullptr ? std::string() : std::string("the slicer is starting");
}

// Every settings tab's unsaved edits get the ifDirty contract before a project load replaces them
// (the load would otherwise put the slicer's own keep/discard question in front of the person).
bool presets_preflight(InvokeContext& ctx, InvokeResult& refusal)
{
    for (Preset::Type type : {Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT})
        if (!ctx.preset_dirty_preflight(type, refusal))
            return false;
    return true;
}

json project_info(Plater& plater)
{
    const std::string path = project_path(plater);
    json recent = json::array();
    if (AppConfig* cfg = wxGetApp().app_config; cfg != nullptr) {
        const std::vector<std::string> list = cfg->get_recent_projects();
        for (size_t i = 0; i < list.size(); ++i)
            recent.push_back({{"index", i}, {"name", fs::path(list[i]).filename().string()}, {"exists", fs::exists(fs::path(list[i]))}});
    }
    return {{"name", utf8(plater.get_project_name())},
            {"file", path.empty() ? json(nullptr) : json(fs::path(path).filename().string())},
            {"saved", !path.empty()},
            {"dirty", plater.is_project_dirty()},
            {"objects", plater.model().objects.size()},
            {"plates", plater.get_partplate_list().get_plate_count()},
            {"gcodePreview", plater.only_gcode_mode()},
            {"projectAside", has_project_aside(plater)},
            // The Project tab's Basic Info (saved in the 3MF).
            {"author", plater.model().design_info != nullptr ? plater.model().design_info->Designer : std::string()},
            {"modelName", plater.model().model_info != nullptr ? plater.model().model_info->model_name : std::string()},
            {"license", plater.model().model_info != nullptr ? plater.model().model_info->license : std::string()},
            {"description", plater.model().model_info != nullptr ? plater.model().model_info->description : std::string()},
            {"recent", recent}};
}

std::shared_ptr<ModelInfo> ensure_model_info(Model& model)
{
    if (model.model_info == nullptr)
        model.model_info = std::make_shared<ModelInfo>();
    return model.model_info;
}

// What the Project tab's edits leave behind: the project has unsaved changes (the 3MF carries the
// info and the files), and the tab, when it is showing, reloads.
void project_data_changed(Plater& plater)
{
    plater.set_plater_dirty(true);
    Slic3r::put_other_changes();
    if (MainFrame* frame = wxGetApp().mainframe; frame != nullptr && frame->m_project != nullptr && frame->m_project->IsShown())
        frame->m_project->update_model_data();
}

fs::path aux_dir(Plater& plater, const AuxFolder& folder) { return fs::path(plater.model().get_auxiliary_file_temp_path()) / folder.dir; }

std::vector<std::string> aux_file_names(const fs::path& dir)
{
    std::vector<std::string>  names;
    boost::system::error_code ec;
    if (!fs::is_directory(dir, ec))
        return names;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_regular_file(it->path(), ec))
            names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

std::string cover_file(Plater& plater) { return plater.model().model_info != nullptr ? plater.model().model_info->cover_file : std::string(); }

// {folder, name} of an existing auxiliary file, or an error.
bool resolve_aux_file(InvokeContext& ctx, const AuxFolder*& folder, fs::path& path, InvokeResult& out)
{
    folder                 = find_aux_folder(ctx.params.value("folder", std::string("pictures")));
    const std::string name = ctx.params.value("name", std::string());
    if (folder == nullptr) {
        out = InvokeResult::error(WAB_BAD_PARAMS, "unknown folder", {{"path", "/params/folder"}});
        return false;
    }
    if (!aux_file_name_ok(name)) {
        out = InvokeResult::error(WAB_BAD_PARAMS, "name is a file name from project.listFiles", {{"path", "/params/name"}});
        return false;
    }
    path = aux_dir(*ctx.plater(), *folder) / name;
    boost::system::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        out = InvokeResult::error(WAB_NOT_FOUND, "the project has no \"" + name + "\" in " + folder->wire + " (see project.listFiles)");
        return false;
    }
    return true;
}

// Writes the project to `path` and makes that its file: Save Project's own path, aimed with
// set_project_filename so no dialog opens. The old file name comes back if the write fails.
InvokeResult save_to(Plater& plater, const std::string& path)
{
    const wxString previous = plater.get_project_filename(".3mf");
    plater.set_project_filename(wxString::FromUTF8(path.c_str()));
    if (plater.save_project(false) != wxID_YES) {
        plater.set_project_filename(previous);
        return InvokeResult::error(WAB_IO, "the project could not be saved");
    }
    return InvokeResult::ok({{"file", fs::path(path).filename().string()}, {"path", path}, {"dirty", plater.is_project_dirty()}});
}

// ---------------------------------------------------------------------------------------------

InvokeResult run_new(InvokeContext& ctx)
{
    Plater*      plater = wxGetApp().plater();
    InvokeResult refusal;
    if (!ctx.project_dirty_preflight(refusal))
        return refusal;
    // "prompt": the slicer's own save-changes question decides; otherwise the preflight already did.
    const bool     prompt = ctx.if_project_dirty == "prompt";
    const wxString name   = wxString::FromUTF8(ctx.params.value("name", "").c_str());
    if (plater->new_project(!prompt, false, name) == wxID_CANCEL)
        return InvokeResult::error(WAB_USER_CANCELLED, "the user kept the current project");
    return InvokeResult::ok(project_info(*plater));
}

InvokeResult run_save(InvokeContext& ctx)
{
    Plater*           plater = wxGetApp().plater();
    const std::string path   = project_path(*plater);
    if (path.empty()) {
        // Never saved: the native Save dialog is where the person names it.
        std::string chosen;
        if (!native_save_path("Save project", utf8(plater->get_project_name()) + ".3mf", "3MF files (*.3mf)|*.3mf", chosen))
            return InvokeResult::error(WAB_USER_CANCELLED, "the save was cancelled");
        return save_to(*plater, chosen);
    }
    ConfirmRequest req;
    req.title        = "Save project";
    req.message      = "Save the project over \"" + fs::path(path).filename().string() + "\"?";
    req.accept_label = "Save";
    req.destructive  = false;
    if (!ctx.confirm(req))
        return InvokeResult::error(WAB_USER_CANCELLED, "the save was declined");
    return save_to(*plater, path);
}

InvokeResult run_save_as(InvokeContext& ctx)
{
    Plater*     plater = wxGetApp().plater();
    std::string name   = ctx.params.value("fileName", utf8(plater->get_project_name()));
    if (!boost::algorithm::iends_with(name, ".3mf"))
        name += ".3mf";
    std::string chosen;
    if (!native_save_path("Save project as", name, "3MF files (*.3mf)|*.3mf", chosen))
        return InvokeResult::error(WAB_USER_CANCELLED, "the save was cancelled");
    return save_to(*plater, chosen);
}

InvokeResult run_info(InvokeContext&) { return InvokeResult::ok(project_info(*wxGetApp().plater())); }

InvokeResult run_open_recent(InvokeContext& ctx)
{
    Plater*                        plater = wxGetApp().plater();
    const std::vector<std::string> list   = wxGetApp().app_config->get_recent_projects();
    const int                      index  = ctx.params.value("index", -1);
    if (index < 0 || static_cast<size_t>(index) >= list.size())
        return InvokeResult::error(WAB_BAD_PARAMS, "index out of range (" + std::to_string(list.size()) + " recent projects)",
                                   {{"path", "/params/index"}});
    const std::string path = list[index];
    if (!fs::exists(fs::path(path)))
        return InvokeResult::error(WAB_NOT_FOUND, "\"" + fs::path(path).filename().string() + "\" is no longer available");
    if (!wxGetApp().can_load_project())
        return InvokeResult::error(WAB_BUSY, "a project cannot be opened right now");
    InvokeResult refusal;
    if (!ctx.project_dirty_preflight(refusal) || !presets_preflight(ctx, refusal))
        return refusal;
    if (ctx.if_project_dirty != "prompt") {
        // Saved or discarded by the preflight: the load must not ask again.
        plater->up_to_date(true, false);
        plater->up_to_date(true, true);
    }
    plater->load_project(wxString::FromUTF8(path.c_str()));
    json info      = project_info(*plater);
    info["opened"] = fs::path(path).filename().string();
    return InvokeResult::ok(info);
}

// project.importModel: a download, then the Import menu's load (geometry only; a ZIP archive is
// unpacked and each model inside imported, as Import ZIP Archive does).
InvokeResult run_import_model(InvokeContext& ctx)
{
    if (ctx.host == nullptr || !ctx.host->download)
        return InvokeResult::error(WAB_CAPABILITY_DISABLED, "this tab cannot download files");
    const std::vector<std::string> exts = {"3mf", "stl", "step", "stp", "obj", "amf", "svg", "drc", "zip"};
    std::shared_ptr<Deferred>      deferred = ctx.defer();
    std::weak_ptr<bool>            alive    = ctx.host->alive;
    BridgeHost*                    host     = ctx.host;
    const std::string              turn     = ctx.turn;
    ctx.host->download(
        ctx.params.value("url", ""), exts,
        [deferred, alive, host, turn](const std::string& local) {
            if (alive.expired())
                return;
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr) {
                deferred->finish(InvokeResult::error(WAB_INTERNAL, "no plater"));
                return;
            }
            const size_t              before_count = plater->model().objects.size();
            const std::vector<size_t> before       = undo_stack_timestamps();
            bool                      ok           = false;
            // Nobody can answer the STEP meshing dialog: mesh with the Preferences values instead.
            NoStepImportDialogs no_step_dialogs;
            if (boost::algorithm::iends_with(local, ".zip")) {
                wxArrayString files;
                files.Add(wxString::FromUTF8(local.c_str()));
                ok = plater->load_files(files);
            } else {
                ok = !plater->load_files(std::vector<fs::path>{fs::path(local)}, LoadStrategy::LoadModel).empty();
            }
            // The load ran after the request's own scope closed: its snapshots belong to the turn too.
            if (!turn.empty())
                host->turn_log.note_snapshots(turn, before, undo_stack_timestamps());
            const size_t added = plater->model().objects.size() - std::min(before_count, plater->model().objects.size());
            if (!ok || added == 0) {
                deferred->finish(InvokeResult::error(WAB_IO, "nothing was imported from \"" + fs::path(local).filename().string() + "\""));
                return;
            }
            if (wxGetApp().mainframe != nullptr)
                wxGetApp().mainframe->select_tab(static_cast<wxPanel*>(plater));
            deferred->finish(InvokeResult::ok({{"file", fs::path(local).filename().string()}, {"objectsAdded", added}}));
        },
        [deferred](int code, const std::string& message) { deferred->finish(InvokeResult::error(code, message)); });
    return InvokeResult::started();
}

InvokeResult run_restore_aside(InvokeContext& ctx) { return restore_project_aside_action(ctx); }

// project.setInfo: the Project tab's Basic Info fields (DesignerPanel), each optional.
InvokeResult run_set_info(InvokeContext& ctx)
{
    Plater* plater = ctx.plater();
    Model&  model  = plater->model();
    if (ctx.params.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "give at least one of author, modelName, license, description");
    if (ctx.params.contains("author"))
        model.SetDesigner(ctx.params["author"].get<std::string>(), ""); // as DesignerPanel: clears the designer's user id
    if (ctx.params.contains("modelName"))
        ensure_model_info(model)->model_name = ctx.params["modelName"].get<std::string>();
    if (ctx.params.contains("license"))
        ensure_model_info(model)->license = ctx.params["license"].get<std::string>();
    if (ctx.params.contains("description"))
        ensure_model_info(model)->description = ctx.params["description"].get<std::string>();
    project_data_changed(*plater);
    return InvokeResult::ok(project_info(*plater));
}

InvokeResult run_list_files(InvokeContext& ctx)
{
    Plater*           plater = ctx.plater();
    const std::string cover  = cover_file(*plater);
    json              out    = json::object();
    for (const AuxFolder& folder : aux_folders()) {
        json                      files = json::array();
        boost::system::error_code ec;
        for (const std::string& name : aux_file_names(aux_dir(*plater, folder))) {
            const auto size = fs::file_size(aux_dir(*plater, folder) / name, ec);
            files.push_back({{"name", name}, {"sizeBytes", ec ? 0 : size}, {"cover", folder.wire == "pictures" && name == cover}});
        }
        out[folder.wire] = files;
    }
    return InvokeResult::ok({{"folders", out}, {"cover", cover.empty() ? json(nullptr) : json(cover)}});
}

// project.addFile: the Project tab's Add File, from a URL instead of the file dialog.
InvokeResult run_add_file(InvokeContext& ctx)
{
    if (ctx.host == nullptr || !ctx.host->download)
        return InvokeResult::error(WAB_CAPABILITY_DISABLED, "this tab cannot download files");
    const AuxFolder* folder = find_aux_folder(ctx.params.value("folder", std::string()));
    if (folder == nullptr)
        return InvokeResult::error(WAB_BAD_PARAMS, "unknown folder", {{"path", "/params/folder"}});
    const std::string wanted = ctx.params.value("name", std::string());
    if (!wanted.empty() && !aux_file_name_ok(wanted))
        return InvokeResult::error(WAB_BAD_PARAMS, "name must be a plain file name", {{"path", "/params/name"}});
    std::shared_ptr<Deferred> deferred = ctx.defer();
    std::weak_ptr<bool>       alive    = ctx.host->alive;
    const AuxFolder           f        = *folder;
    ctx.host->download(
        ctx.params.value("url", ""), f.extensions,
        [deferred, alive, f, wanted](const std::string& local) {
            if (alive.expired())
                return;
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr) {
                deferred->finish(InvokeResult::error(WAB_INTERNAL, "no plater"));
                return;
            }
            std::string name = wanted.empty() ? fs::path(local).filename().string() : wanted;
            // The downloaded type decides; a given name keeps it.
            const std::string ext = fs::path(local).extension().string();
            if (!boost::algorithm::iends_with(name, ext))
                name += ext;
            const fs::path            dir = aux_dir(*plater, f);
            boost::system::error_code ec;
            fs::create_directories(dir, ec);
            name = aux_unique_name(name, aux_file_names(dir));
            fs::copy_file(fs::path(local), dir / name, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                deferred->finish(InvokeResult::error(WAB_IO, "the file could not be added to the project: " + ec.message()));
                return;
            }
            project_data_changed(*plater);
            deferred->finish(InvokeResult::ok({{"folder", f.wire}, {"name", name}}));
        },
        [deferred](int code, const std::string& message) { deferred->finish(InvokeResult::error(code, message)); });
    return InvokeResult::started();
}

// project.setCover: a picture's "Set as cover" (AuFile::on_set_cover): the 3MF's cover and plate
// thumbnails come from it.
InvokeResult run_set_cover(InvokeContext& ctx)
{
    InvokeResult     out;
    const AuxFolder* folder = nullptr;
    fs::path         path;
    if (!resolve_aux_file(ctx, folder, path, out))
        return out;
    if (folder->wire != "pictures")
        return InvokeResult::error(WAB_BAD_PARAMS, "only a picture can be the cover", {{"path", "/params/folder"}});
    Plater*  plater = ctx.plater();
    fs::path thumbs = path.parent_path().parent_path() / ".thumbnails";
    boost::system::error_code ec;
    fs::create_directories(thumbs, ec);
    struct Size { wxSize size; const char* file; };
    for (const Size& s : {Size{_3MF_COVER_SIZE, "thumbnail_3mf.png"}, Size{PRINTER_THUMBNAIL_SMALL_SIZE, "thumbnail_small.png"},
                          Size{PRINTER_THUMBNAIL_MIDDLE_SIZE, "thumbnail_middle.png"}}) {
        wxImage image;
        if (!generate_image(path.string(), image, s.size) || !image.SaveFile(wxString::FromUTF8((thumbs / s.file).string().c_str()), wxBITMAP_TYPE_PNG))
            return InvokeResult::error(WAB_IO, "\"" + path.filename().string() + "\" could not be made into a cover picture");
    }
    ensure_model_info(plater->model())->cover_file = path.filename().string();
    project_data_changed(*plater);
    return InvokeResult::ok({{"cover", path.filename().string()}});
}

// project.removeFile: a file's delete (AuFile::on_set_delete), after a native confirmation.
InvokeResult run_remove_file(InvokeContext& ctx)
{
    InvokeResult     out;
    const AuxFolder* folder = nullptr;
    fs::path         path;
    if (!resolve_aux_file(ctx, folder, path, out))
        return out;
    ConfirmRequest req;
    req.title        = "Delete file";
    req.message      = "Delete \"" + path.filename().string() + "\" from the project's " + folder->dir + "? It is not kept anywhere else.";
    req.accept_label = "Delete";
    if (!ctx.confirm(req))
        return InvokeResult::error(WAB_USER_CANCELLED, "the person kept the file");
    Plater*                   plater = ctx.plater();
    boost::system::error_code ec;
    if (!fs::remove(path, ec) || ec)
        return InvokeResult::error(WAB_IO, "the file could not be deleted");
    const bool was_cover = folder->wire == "pictures" && cover_file(*plater) == path.filename().string();
    if (was_cover) {
        const fs::path thumbs = path.parent_path().parent_path() / ".thumbnails";
        for (const char* f : {"thumbnail_3mf.png", "thumbnail_small.png", "thumbnail_middle.png"})
            fs::remove(thumbs / f, ec);
        plater->model().model_info->cover_file.clear();
    }
    project_data_changed(*plater);
    return InvokeResult::ok({{"folder", folder->wire}, {"removed", path.filename().string()}, {"wasCover", was_cover}});
}

// project.forgetRecent: Home's remove-from-recent (one entry) and clear-all.
InvokeResult run_forget_recent(InvokeContext& ctx)
{
    const std::vector<std::string> list = wxGetApp().app_config->get_recent_projects();
    if (ctx.params.contains("index")) {
        const int index = ctx.params["index"].get<int>();
        if (index < 0 || size_t(index) >= list.size())
            return InvokeResult::error(WAB_BAD_PARAMS, "index out of range (" + std::to_string(list.size()) + " recent projects)",
                                       {{"path", "/params/index"}});
        wxGetApp().request_remove_project(list[size_t(index)]);
    } else {
        wxGetApp().request_remove_project("");
    }
    return InvokeResult::ok(project_info(*ctx.plater()));
}

} // namespace

// The "Back to the project" link of the set-aside notification (and a click on Prepare): reload
// the stashed project as it was. GUI_App::can_load_project and the preset preflight first, so the
// load asks nothing; then on the next idle turn, as the tab click does (CallAfter).
InvokeResult restore_project_aside_action(InvokeContext& ctx)
{
    Plater* plater = ctx.plater();
    if (!has_project_aside(*plater))
        return InvokeResult::error(WAB_NOT_FOUND, "no project is set aside: no G-code preview replaced one", {{"reason", "nothing_aside"}});
    if (!wxGetApp().can_load_project())
        return InvokeResult::error(WAB_BUSY, "a project cannot be loaded right now");
    InvokeResult refusal;
    if (!presets_preflight(ctx, refusal))
        return refusal;
    std::shared_ptr<Deferred> deferred = ctx.defer();
    std::weak_ptr<bool>       alive    = ctx.host != nullptr ? std::weak_ptr<bool>(ctx.host->alive) : std::weak_ptr<bool>();
    wxGetApp().CallAfter([deferred, alive]() {
        Plater* p = wxGetApp().plater();
        if (p == nullptr) {
            deferred->finish(InvokeResult::error(WAB_INTERNAL, "no plater"));
            return;
        }
        if (!has_project_aside(*p)) { // brought back meanwhile (the person clicked Prepare)
            json info         = project_info(*p);
            info["restored"]  = true;
            deferred->finish(InvokeResult::ok(info));
            return;
        }
        const bool ok = restore_project_aside(*p);
        if (!ok) {
            deferred->finish(has_project_aside(*p)
                                 ? InvokeResult::error(WAB_USER_CANCELLED, "the person kept the G-code preview (the preset question was cancelled)")
                                 : InvokeResult::error(WAB_IO, "the project could not be brought back; its stash file is kept"));
            return;
        }
        json info        = project_info(*p);
        info["restored"] = true;
        deferred->finish(InvokeResult::ok(info));
    });
    return InvokeResult::started();
}

void register_project_actions(Catalog& c)
{
    {
        ActionDescriptor d;
        d.id      = "project.new";
        d.label   = "New Project";
        d.summary = "Close the current project and start an empty one. Honours ifProjectDirty: an unsaved project is never dropped "
                    "without ifProjectDirty save, discard or prompt.";
        d.risk    = Risk::Destructive;
        d.confirm_skip_rule = "the ifProjectDirty protocol: an unsaved project is only replaced on save, discard or the slicer's own prompt";
        d.params  = object_schema({{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 120}, {"description", "Name of the new project."}}}});
        d.effects = {"project", "structure", "plates"};
        d.ui      = {"MainFrame.cpp::menu::New Project"};
        d.examples = one_example(json::object());
        c.add(std::move(d), run_new, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id       = "project.save";
        d.label    = "Save Project";
        d.summary  = "Save the project to its own file after a native confirmation, or, never saved, where the person picks in the native "
                     "Save dialog.";
        d.risk     = Risk::OutwardFile;
        d.confirm  = Confirm::Native;
        d.params   = object_schema(json::object());
        d.effects  = {"project", "file"};
        d.ui       = {"MainFrame.cpp::menu::Save Project"};
        d.examples = one_example(json::object());
        c.add(std::move(d), run_save, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id                = "project.saveAs";
        d.label             = "Save Project as";
        d.summary           = "Save the project to a new file the person picks in the native Save dialog; it becomes the project's file.";
        d.risk              = Risk::OutwardFile;
        d.confirm_skip_rule = "the native Save dialog is the consent: the person picks the file";
        d.params   = object_schema({{"fileName", {{"type", "string"}, {"minLength", 1}, {"maxLength", 120}, {"description", "Suggested file name."}}}});
        d.effects  = {"project", "file"};
        d.ui       = {"MainFrame.cpp::menu::Save Project as"};
        d.examples = one_example({{"fileName", "automated-test.3mf"}});
        c.add(std::move(d), run_save_as, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id       = "project.info";
        d.label    = "Project info";
        d.summary  = "The open project's name, file name, unsaved state, object and plate counts, and the recent projects by name.";
        d.risk     = Risk::Read;
        d.params   = object_schema(json::object());
        d.ui       = {"MainFrame.cpp::submenu::Recent files"};
        d.examples = one_example(json::object());
        c.add(std::move(d), run_info, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id                = "project.openRecent";
        d.label             = "Open recent project";
        d.summary           = "Open one of the recent projects by its index in project.info. Honours ifProjectDirty and ifDirty.";
        d.risk              = Risk::Destructive;
        d.confirm_skip_rule = "the ifProjectDirty and ifDirty protocols: unsaved work is only replaced on save, discard or the slicer's own prompt";
        d.params   = object_schema({{"index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 100}}}}, {"index"});
        d.effects  = {"project", "structure", "plates", "presets"};
        d.ui       = {"MainFrame.cpp::menu::Open Project"};
        d.examples = one_example({{"index", 0}});
        c.add(std::move(d), run_open_recent, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id      = "project.importModel";
        d.label   = "Import model";
        d.summary = "Download a model (3MF/STL/STEP/OBJ/AMF/SVG/DRC) or a ZIP archive of models and add it to the plate, as Import does. "
                    "Geometry only; to open a project file with its settings use slicer.openProjectFromUrl.";
        d.risk    = Risk::Mutate;
        d.params  = object_schema({{"url", {{"type", "string"}, {"minLength", 8}, {"maxLength", 2048},
                                            {"description", "http(s) URL on the page's host or the tab's download hosts."}}}},
                                  {"url"});
        d.effects      = {"structure", "geometry", "plates"};
        d.long_running = true;
        d.ui       = {"MainFrame.cpp::menu::Import 3MF/STL/STEP/SVG/OBJ/AMF", "MainFrame.cpp::menu::Import ZIP Archive", "GLCanvas3D.cpp::toolbar::add"};
        d.examples = one_example({{"url", "http://127.0.0.1:8799/fixtures/s7_cube.stl"}});
        c.add(std::move(d), run_import_model, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id      = "project.restoreAside";
        d.label   = "Back to the project";
        d.summary = "Close the G-code preview that replaced the project (a G-code handed over by a web app) and bring the project back as "
                    "it was: same file, name and unsaved state. As the preview notification's link or a click on Prepare. 1008 when no "
                    "project is set aside. Honours ifDirty for unsaved preset edits.";
        d.risk     = Risk::Mutate;
        d.params   = object_schema(json::object());
        d.effects  = {"project", "structure", "plates", "presets"};
        d.long_running = true;
        d.examples = one_example(json::object());
        c.add(std::move(d), run_restore_aside, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id      = "project.setInfo";
        d.label   = "Project info";
        d.summary = "Set the project's Basic Info (saved in the 3MF): author, model name, license, description. Each is optional; the "
                    "project becomes unsaved. project.info reads them.";
        d.risk    = Risk::Mutate;
        d.params  = object_schema({{"author", {{"type", "string"}, {"maxLength", 200}}},
                                   {"modelName", {{"type", "string"}, {"maxLength", 200}}},
                                   {"license", {{"type", "string"}, {"enum", project_licenses()}, {"description", "\"\" = none."}}},
                                   {"description", {{"type", "string"}, {"maxLength", 10000}}}});
        d.effects  = {"project"};
        d.examples = one_example({{"modelName", "Bracket"}, {"license", "BY-SA"}});
        c.add(std::move(d), run_set_info, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id       = "project.listFiles";
        d.label    = "Project files";
        d.summary  = "The project's auxiliary files by folder (pictures, bom, assembly, others), with sizes and which picture is the cover.";
        d.risk     = Risk::Read;
        d.params   = object_schema(json::object());
        d.examples = one_example(json::object());
        c.add(std::move(d), run_list_files, enabled_with_plater);
    }
    json folder_enum = json::array();
    for (const AuxFolder& f : aux_folders())
        folder_enum.push_back(f.wire);
    const json name_schema = {{"type", "string"}, {"minLength", 1}, {"maxLength", 255}, {"description", "A file name from project.listFiles."}};
    {
        ActionDescriptor d;
        d.id      = "project.addFile";
        d.label   = "Add file";
        d.summary = "Download a file into one of the project's auxiliary folders, as the Project tab's Add File does: pictures "
                    "(png/jpg/jpeg/bmp), bom (xls/xlsx/pdf), assembly (pdf), others (txt). A taken name gets a _2, _3 ... suffix.";
        d.risk    = Risk::Mutate;
        d.params  = object_schema({{"url", {{"type", "string"}, {"minLength", 8}, {"maxLength", 2048},
                                            {"description", "http(s) URL on the page's host or the tab's download hosts."}}},
                                   {"folder", {{"type", "string"}, {"enum", folder_enum}}},
                                   {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 255}, {"description", "File name to store it under."}}}},
                                  {"url", "folder"});
        d.effects      = {"project"};
        d.long_running = true;
        d.examples     = one_example({{"url", "http://127.0.0.1:8799/fixtures/s7_cover.png"}, {"folder", "pictures"}});
        c.add(std::move(d), run_add_file, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id       = "project.setCover";
        d.label    = "Set as cover";
        d.summary  = "Make one of the project's pictures its cover (the 3MF's cover and thumbnails), as Set as cover does.";
        d.risk     = Risk::Mutate;
        d.params   = object_schema({{"folder", {{"type", "string"}, {"enum", json::array({"pictures"})}, {"default", "pictures"}}}, {"name", name_schema}},
                                   {"name"});
        d.effects  = {"project"};
        d.examples = one_example({{"name", "s7_cover.png"}});
        c.add(std::move(d), run_set_cover, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id       = "project.removeFile";
        d.label    = "Delete file";
        d.summary  = "Delete one of the project's auxiliary files after a native confirmation (not undoable); a cover picture stops being "
                     "the cover.";
        d.risk     = Risk::Destructive;
        d.confirm  = Confirm::Native;
        d.params   = object_schema({{"folder", {{"type", "string"}, {"enum", folder_enum}}}, {"name", name_schema}}, {"folder", "name"});
        d.effects  = {"project", "file"};
        d.examples = one_example({{"folder", "pictures"}, {"name", "s7_cover.png"}});
        c.add(std::move(d), run_remove_file, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id      = "project.forgetRecent";
        d.label   = "Remove from recent";
        d.summary = "Remove one project from the recent list by its index in project.info, or (no index) clear the list, as Home does. "
                    "No file is deleted.";
        d.risk    = Risk::Mutate;
        d.params  = object_schema({{"index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 100},
                                              {"description", "Without it the whole list is cleared."}}}});
        d.effects  = {"project"};
        d.examples = one_example({{"index", 0}});
        c.add(std::move(d), run_forget_recent, enabled_with_plater);
    }
}

}}} // namespace Slic3r::GUI::Actions
