// Automation actions: preset.* (save, rename, delete, diff, revert, discard, visibility, create from
// a template, bundles in and out), printer.listPhysical. The vendor-catalog typed methods
// (slicer.addFilamentSlot, slicer.saveFilamentPreset, slicer.listPrinterModels, slicer.addPrinter)
// are described once, in ExistingMethods.cpp, which registers first.
// Owner: work package S7. See docs/HLSD/bridge-actions.md for how to write an action and
// docs/bridge-actions/presets.md for what these do.
//
// Every preset write goes through the same Tab / PresetCollection calls the Settings tab's buttons
// make, so the person's preset folder ends up exactly as the buttons would leave it. Nothing here
// opens the tab's own dialogs: names come as params, and the one question a destructive action asks
// is the native confirmation (ctx.confirm).

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "PresetLogic.hpp"
#include "Register.hpp"

#include <algorithm>
#include <map>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "miniz.h"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../CreatePresetsDialog.hpp"
#include "../MainFrame.hpp"
#include "../Plater.hpp"
#include "../Tab.hpp"
#include "../WebAppBridge.hpp"
#include "../WebAppPresetShare.hpp"
#include "../WebAppSettings.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json   = nlohmann::json;
namespace fs = boost::filesystem;

namespace {

const json TYPE_SCHEMA = {{"type", "string"},
                          {"enum", {"printer", "print", "filament"}},
                          {"description", "Which settings tab: printer, print (process) or filament."}};

json name_schema(const char* description)
{
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", 200}, {"description", description}};
}

json object_schema(json properties, std::vector<std::string> required = {})
{
    json s = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = required;
    return s;
}

PresetCollection* collection_for(PresetBundle& bundle, Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINTER: return &bundle.printers;
    case Preset::TYPE_PRINT: return &bundle.prints;
    case Preset::TYPE_FILAMENT: return &bundle.filaments;
    default: return nullptr;
    }
}

// The tab and collection params.type names.
struct TypeCtx
{
    Preset::Type      type       = Preset::TYPE_INVALID;
    const char*       name       = "";
    Tab*              tab        = nullptr;
    PresetBundle*     bundle     = nullptr;
    PresetCollection* collection = nullptr;
};

bool type_ctx(Preset::Type type, TypeCtx& out)
{
    out.type       = type;
    out.name       = WebAppSettings::preset_type_name(type);
    out.bundle     = wxGetApp().preset_bundle;
    out.tab        = wxGetApp().get_tab(type);
    out.collection = out.bundle != nullptr ? collection_for(*out.bundle, type) : nullptr;
    return out.bundle != nullptr && out.tab != nullptr && out.collection != nullptr && wxGetApp().plater() != nullptr;
}

bool resolve_type(const InvokeContext& ctx, TypeCtx& out, InvokeResult& error)
{
    Preset::Type type;
    if (!WebAppSettings::preset_type_from_string(ctx.params.value("type", ""), type)) {
        error = InvokeResult::error(WAB_BAD_PARAMS, "\"type\" must be \"printer\", \"print\" or \"filament\"", {{"path", "/params/type"}});
        return false;
    }
    if (!type_ctx(type, out)) {
        error = InvokeResult::error(WAB_INTERNAL, "the preset bundle is not ready");
        return false;
    }
    return true;
}

// An installed preset by name or display alias (a " (modified)" suffix is ignored); with
// `include_hidden`, a hidden one by alias too. Empty when none.
std::string resolve_name(const TypeCtx& t, const std::string& requested, bool include_hidden = false)
{
    const std::string name = Preset::remove_suffix_modified(requested);
    if (t.collection->find_preset(name, false) != nullptr)
        return name;
    const std::string by_alias = t.bundle->get_preset_name_by_alias(t.type, name);
    if (by_alias != name && t.collection->find_preset(by_alias, false) != nullptr)
        return by_alias;
    if (include_hidden)
        for (const Preset& p : t.collection->get_presets())
            if (!p.is_visible && p.alias == name)
                return p.name;
    return std::string();
}

InvokeResult not_found(const TypeCtx& t, const std::string& name)
{
    return InvokeResult::error(WAB_NOT_FOUND, std::string("no installed ") + t.name + " preset is named \"" + name + "\"",
                               {{"type", t.name}, {"name", name}});
}

InvokeResult check_new_name(const TypeCtx& t, const std::string& name, const char* path)
{
    if (const std::string why = PresetLogic::check_preset_name(name, PresetCollection::get_suffix_modified()); !why.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, why, {{"path", path}});
    return InvokeResult::ok();
}

// Puts the tab on `name`: a preset switch, so the person's unsaved edits get the ifDirty contract.
bool switch_tab_to(InvokeContext& ctx, const TypeCtx& t, const std::string& name, InvokeResult& refusal)
{
    if (t.collection->get_edited_preset().name == name)
        return true;
    // The switched preset's edits and its dependents' (a printer switch asks about print and
    // filament edits too): no UnsavedChangesDialog opens unless ifDirty is "prompt".
    if (!ctx.preset_switch_preflight(t.type, name, refusal))
        return false;
    t.tab->select_preset(name);
    if (t.collection->get_edited_preset().name != name) {
        // Under "prompt" the person answered the slicer's own question; otherwise nobody was
        // asked, and the preset could not be selected (incompatible, or hidden).
        refusal = ctx.if_dirty == "prompt"
                      ? InvokeResult::error(WAB_USER_CANCELLED, "the user kept the current preset",
                                            {{"selected", t.collection->get_edited_preset().name}})
                      : InvokeResult::error(WAB_INTERNAL,
                                            "\"" + name + "\" could not be selected (it does not suit the current printer, or it is hidden)",
                                            {{"selected", t.collection->get_edited_preset().name}, {"reason", "not_selectable"}});
        return false;
    }
    return true;
}

// A hidden system preset made visible the way the person would: the filament selection's tick
// (filament), the setup wizard's model + nozzle tick and the visibility pass load_selections runs
// (printer), or -- process presets carry no tick of their own -- for this session. False when the
// preset is not a system preset or a printer is not a vendor model variant.
bool set_system_visible(const TypeCtx& t, const std::string& name, bool visible, std::string& why)
{
    Preset* preset = t.collection->find_preset(name, false);
    if (preset == nullptr || !preset->is_system) {
        why = "only system presets are shown or hidden; your own presets are always listed";
        return false;
    }
    AppConfig* cfg = wxGetApp().app_config;
    if (t.type == Preset::TYPE_FILAMENT) {
        if (visible)
            cfg->set(AppConfig::SECTION_FILAMENTS, name, "true");
        else
            cfg->erase(AppConfig::SECTION_FILAMENTS, name);
        preset->set_visible_from_appconfig(*cfg);
    } else if (t.type == Preset::TYPE_PRINTER) {
        const std::string model   = preset->config.opt_string("printer_model");
        const std::string variant = preset->config.opt_string("printer_variant");
        if (preset->vendor == nullptr || model.empty() || variant.empty()) {
            why = "this printer preset is not a vendor model variant";
            return false;
        }
        cfg->set_variant(preset->vendor->id, model, variant, visible);
        t.bundle->load_installed_printers(*cfg);
        for (Preset& f : t.bundle->filaments)
            f.set_visible_from_appconfig(*cfg);
    } else {
        preset->is_visible = visible;
    }
    return true;
}

// The presets that name `name` as their parent ("inherits"): the one they hang off cannot go.
std::vector<std::string> children_of(const PresetCollection& c, const std::string& name)
{
    std::vector<std::string> out;
    for (const Preset& p : c.get_presets())
        if (p.inherits() == name)
            out.push_back(p.name);
    return out;
}

void refresh_after_preset_change(const TypeCtx& t)
{
    t.tab->update_tab_ui();
    if (Plater* plater = wxGetApp().plater(); plater != nullptr) {
        plater->sidebar().update_presets(t.type);
        if (t.type == Preset::TYPE_PRINTER)
            plater->sidebar().update_presets(Preset::TYPE_FILAMENT);
    }
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->diff_dialog.update_presets(t.type);
}

bool key_visible(const std::string& key) { return WebAppSettings::key_readable(key, false); }

json diff_json(const std::vector<PresetLogic::KeyDiff>& diffs, size_t limit, bool& truncated)
{
    json out  = json::array();
    truncated = diffs.size() > limit;
    for (size_t i = 0; i < diffs.size() && i < limit; ++i) {
        const PresetLogic::KeyDiff& d = diffs[i];
        json e = {{"key", d.key}, {"a", d.in_a ? json(d.a) : json(nullptr)}, {"b", d.in_b ? json(d.b) : json(nullptr)}};
        if (const ConfigOptionDef* def = print_config_def.get(d.key); def != nullptr) {
            e["label"]    = def->label;
            e["category"] = def->category;
        }
        out.push_back(std::move(e));
    }
    return out;
}

// The keys this turn's automated writes set in `preset` of `type` (TurnLog).
std::vector<std::string> turn_written_keys(const InvokeContext& ctx, const char* type, const std::string& preset)
{
    std::vector<std::string> keys;
    if (ctx.host == nullptr || ctx.turn.empty())
        return keys;
    if (const TurnLog::Turn* turn = ctx.host->turn_log.find(ctx.turn); turn != nullptr)
        for (const TurnLog::SettingWrite& w : turn->settings)
            if (w.type == type && w.preset == preset)
                keys.push_back(w.key);
    return keys;
}

// Loads `config` into the tab as slicer.setSettings does, records the writes of `keys` for
// slicer.undoTurn and reports them as settingsChanged {source:"bridge"}.
void load_into_tab(InvokeContext& ctx, const TypeCtx& t, const DynamicPrintConfig& config, const std::vector<std::string>& keys)
{
    std::map<std::string, std::string> before;
    for (const std::string& k : keys)
        if (const ConfigOption* o = t.tab->get_config()->option(k); o != nullptr)
            before[k] = o->serialize();
    {
        WebAppBridgeWriteScope scope;
        t.tab->load_config(config);
        // The field couplings a hand edit runs (filament_diameter, enable_prime_tower,
        // gcode_flavor ...), as slicer.setSettings does.
        WebAppSettings::apply_tab_couplings(*t.tab, config, keys);
    }
    const std::string preset  = t.collection->get_selected_preset_name();
    json              changes = json::array();
    for (const std::string& k : keys) {
        const ConfigOption* now = t.tab->get_config()->option(k);
        if (ctx.host != nullptr && !ctx.turn.empty())
            ctx.host->turn_log.note_setting(ctx.turn, {t.name, preset, k, before[k], now != nullptr ? now->serialize() : std::string()});
        std::string previous = before[k];
        if (previous.size() > 2000)
            previous.resize(2000);
        changes.push_back(t.tab->describe_setting_change(k, previous));
    }
    if (ctx.host != nullptr && !keys.empty())
        ctx.host->emit_event("slicer.settingsChanged",
                             {{"type", t.name}, {"keys", keys}, {"source", "bridge"}, {"preset", preset}, {"changes", changes}});
}

// ---------------------------------------------------------------------------------------------
// preset.save
// ---------------------------------------------------------------------------------------------

InvokeResult run_save(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    const std::string name = ctx.params.value("name", "");
    if (InvokeResult bad = check_new_name(t, name, "/params/name"); !bad.is_ok())
        return bad;

    const Preset* existing  = t.collection->find_preset(name, false);
    const bool    overwrite = ctx.params.value("overwrite", false);
    if (existing != nullptr && !existing->can_overwrite())
        return InvokeResult::error(WAB_FORBIDDEN_ORIGIN, "\"" + name + "\" is a system preset; choose another name",
                                   {{"exists", true}, {"isSystem", true}});
    if (existing != nullptr && !overwrite)
        return InvokeResult::error(WAB_BAD_PARAMS,
                                   std::string("a ") + t.name + " preset named \"" + name +
                                       "\" already exists; pass overwrite:true to replace it, or choose another name",
                                   {{"exists", true}, {"name", name}, {"path", "/params/name"}});
    if (existing == nullptr && t.bundle->get_preset_name_by_alias(t.type, name) != name)
        return InvokeResult::error(WAB_BAD_PARAMS, "the name is the display name of another preset; choose another name",
                                   {{"path", "/params/name"}});

    // Overwriting keeps the preset where it lives, as SavePresetDialog locks its radio: a preset
    // never moves between the preset folder and the project by being saved over.
    bool to_project = ctx.params.value("toProject", false);
    if (existing != nullptr) {
        const bool in_project = existing->is_project_embedded;
        if (ctx.params.contains("toProject") && to_project != in_project)
            return InvokeResult::error(WAB_BAD_PARAMS,
                                       "\"" + name + "\" lives in the " + (in_project ? "project" : "preset folder") +
                                           "; overwriting keeps it there. Leave toProject out, or save under another name",
                                       {{"path", "/params/toProject"}, {"isProjectEmbedded", in_project}});
        to_project = in_project;
    }
    // Save which preset? The tab's current one, or `from` (a preset switch under ifDirty first).
    if (ctx.params.contains("from")) {
        const std::string from = resolve_name(t, ctx.params["from"].get<std::string>());
        if (from.empty())
            return not_found(t, ctx.params["from"].get<std::string>());
        if (!switch_tab_to(ctx, t, from, err))
            return err;
    }
    const std::string              from  = t.collection->get_edited_preset().name;
    const std::vector<std::string> edits = t.collection->current_is_dirty() ? t.collection->current_dirty_options()
                                                                            : std::vector<std::string>();
    // Replacing ANOTHER saved preset loses what it held: the person says so, natively.
    if (existing != nullptr && existing->name != from) {
        ConfirmRequest req;
        req.title        = "Overwrite preset";
        req.message      = std::string("Replace the saved ") + t.name + " preset \"" + name + "\" with the settings of \"" + from + "\"?";
        req.accept_label = "Overwrite";
        if (!ctx.confirm(req))
            return InvokeResult::error(WAB_USER_CANCELLED, "the overwrite was declined");
    }
    const std::vector<std::string> slots_before = t.bundle->filament_presets;

    t.tab->save_preset(name, ctx.params.value("detach", false), to_project);

    if (t.collection->find_preset(name, false) == nullptr || t.collection->get_edited_preset().name != name)
        return InvokeResult::error(WAB_USER_CANCELLED, "the preset was not saved (the filament temperature check was declined)",
                                   {{"selected", t.collection->get_edited_preset().name}});
    // Save renames every filament slot that held the old preset (Sidebar::update_presets_from_to);
    // the filament tab edits slot 0's preset, so the other slots get theirs back.
    if (t.type == Preset::TYPE_FILAMENT && existing == nullptr) {
        Plater* plater = wxGetApp().plater();
        for (size_t i = 1; i < slots_before.size() && i < t.bundle->filament_presets.size(); ++i)
            if (slots_before[i] == from && t.bundle->filament_presets[i] == name)
                plater->select_filament_preset(i, from);
    }
    json result = {{"type", t.name},
                   {"name", name},
                   {"from", from},
                   {"overwritten", existing != nullptr},
                   {"savedEdits", edits},
                   {"selected", t.collection->get_selected_preset_name()}};
    if (t.type == Preset::TYPE_FILAMENT)
        result["slots"] = t.bundle->filament_presets;
    return InvokeResult::ok(result);
}

// ---------------------------------------------------------------------------------------------
// preset.rename
// ---------------------------------------------------------------------------------------------

InvokeResult run_rename(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    const std::string requested = ctx.params.value("name", "");
    const std::string old_name  = resolve_name(t, requested);
    if (old_name.empty())
        return not_found(t, requested);
    const std::string new_name = ctx.params.value("newName", "");
    if (InvokeResult bad = check_new_name(t, new_name, "/params/newName"); !bad.is_ok())
        return bad;
    if (new_name == old_name)
        return InvokeResult::ok({{"type", t.name}, {"name", old_name}, {"renamed", false}}).with_effects({});
    const Preset* preset = t.collection->find_preset(old_name, false);
    if (preset == nullptr || !preset->is_user())
        return InvokeResult::error(WAB_FORBIDDEN_ORIGIN, "only your own presets can be renamed; \"" + old_name + "\" is not one",
                                   {{"isSystem", preset != nullptr && preset->is_system}});
    if (t.collection->find_preset(new_name, false) != nullptr || t.bundle->get_preset_name_by_alias(t.type, new_name) != new_name)
        return InvokeResult::error(WAB_BAD_PARAMS, std::string("a ") + t.name + " preset named \"" + new_name + "\" already exists",
                                   {{"exists", true}, {"path", "/params/newName"}});
    if (const std::vector<std::string> kids = children_of(*t.collection, old_name); !kids.empty())
        return InvokeResult::error(WAB_FORBIDDEN_ORIGIN, "other presets are based on \"" + old_name + "\"; it cannot be renamed",
                                   {{"inheritedBy", kids}});
    // Renaming drops the old preset, and with it its cloud copy (sync deletes it, and the new name is a
    // new preset there). Cloud sync is the person's own: automation leaves synced presets alone.
    if (!preset->setting_id.empty())
        return InvokeResult::error(WAB_FORBIDDEN_ORIGIN,
                                   "\"" + old_name + "\" is synced to the person's cloud account; automation does not touch cloud sync, so the "
                                   "person renames it in the Settings tab",
                                   {{"cloudSynced", json::array({old_name})}});
    if (!switch_tab_to(ctx, t, old_name, err))
        return err;
    if (t.collection->current_is_dirty())
        return InvokeResult::error(WAB_NEEDS_CONFIRMATION, "\"" + old_name + "\" has unsaved edits; save or discard them before renaming",
                                   {{"dirty", true}, {"type", t.name}, {"dirtyKeys", t.collection->current_dirty_options()}});

    // Save As under the new name (selects it; the sidebar's slots follow), then drop the old one.
    t.tab->save_preset(new_name, false, false);
    if (t.collection->get_edited_preset().name != new_name)
        return InvokeResult::error(WAB_USER_CANCELLED, "the preset was not saved under the new name");
    // Physical printers that used the old printer preset use the renamed one.
    if (t.type == Preset::TYPE_PRINTER)
        for (PhysicalPrinter& ph : t.bundle->physical_printers)
            if (ph.get_preset_names().count(old_name) > 0) {
                ph.add_preset(new_name);
                ph.delete_preset(old_name);
                ph.update_preset_names_in_config();
                ph.save(nullptr);
            }
    // Filament and process presets limited to the old printer name are limited to the new one:
    // otherwise renaming a printer would orphan every preset made for it. Saved in place, as
    // their files already are (a diff against their parent when they have one).
    json repointed = json::array();
    if (t.type == Preset::TYPE_PRINTER)
        for (PresetCollection* coll : {&t.bundle->filaments, &t.bundle->prints}) {
            std::vector<std::string> names;
            for (const Preset& p : coll->get_presets())
                if (!p.is_default && p.can_overwrite())
                    if (const auto* cp = p.config.option<ConfigOptionStrings>("compatible_printers");
                        cp != nullptr && std::find(cp->values.begin(), cp->values.end(), old_name) != cp->values.end())
                        names.push_back(p.name);
            for (const std::string& n : names) {
                Preset* p = coll->find_preset(n, false);
                if (p == nullptr)
                    continue;
                auto repoint = [&](DynamicPrintConfig& cfg) {
                    if (auto* cp = cfg.option<ConfigOptionStrings>("compatible_printers"); cp != nullptr)
                        std::replace(cp->values.begin(), cp->values.end(), old_name, new_name);
                };
                repoint(p->config);
                if (coll->get_edited_preset().name == n)
                    repoint(coll->get_edited_preset().config);
                if (const Preset* parent = coll->get_preset_parent(*p); parent != nullptr && parent != p) {
                    DynamicPrintConfig parent_config = parent->config;
                    p->save(&parent_config);
                } else {
                    p->save(nullptr);
                }
                repointed.push_back({{"type", coll == &t.bundle->prints ? "print" : "filament"}, {"name", n}});
            }
        }
    t.collection->delete_preset(old_name);
    // Erasing from the collection shifts its indices: select the renamed preset again by name.
    t.collection->select_preset_by_name(new_name, true);
    t.bundle->update_compatible(PresetSelectCompatibleType::Never);
    refresh_after_preset_change(t);
    json result = {{"type", t.name}, {"from", old_name}, {"name", new_name}, {"renamed", true}};
    if (!repointed.empty())
        result["compatibleUpdated"] = repointed;
    return InvokeResult::ok(result);
}

// ---------------------------------------------------------------------------------------------
// preset.delete
// ---------------------------------------------------------------------------------------------

InvokeResult run_delete(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    const std::string requested = ctx.params.value("name", "");
    const std::string name      = resolve_name(t, requested);
    if (name.empty())
        return not_found(t, requested);
    const Preset* preset = t.collection->find_preset(name, false);
    if (preset == nullptr || preset->is_default || preset->is_system || preset->is_from_bundle())
        return InvokeResult::error(WAB_FORBIDDEN_ORIGIN, "system, bundle and default presets cannot be deleted",
                                   {{"isSystem", preset != nullptr && preset->is_system}});
    if (const std::vector<std::string> kids = children_of(*t.collection, name); !kids.empty())
        return InvokeResult::error(WAB_FORBIDDEN_ORIGIN, "presets inherited by other presets cannot be deleted", {{"inheritedBy", kids}});
    // Deleting a custom root printer (one made with Create printer, not a vendor's) also deletes
    // every filament and process preset of the person's that suits it: Tab::select_preset's
    // delete_third_printer branch, which the GUI announces in its DeleteConfirmDialog. The same
    // lists here, named in the confirmation, and only with cascade:true.
    const bool selected       = t.collection->get_edited_preset().name == name;
    const bool custom_printer = t.type == Preset::TYPE_PRINTER && t.collection->get_preset_base(*preset) == preset &&
                                !preset->is_system && !preset->is_from_bundle();
    // `use_flags`: on the selected printer the flags are what the GUI reads; for another printer
    // they are recomputed once it is selected, which is what is predicted here. Cloud-synced
    // presets the cascade would take are collected in `synced_out`.
    auto cascade_of = [&t, &name, custom_printer](bool use_flags, std::vector<std::string>* synced_out) {
        json out = json::object();
        if (!custom_printer)
            return out;
        const Preset* printer_preset = t.collection->find_preset(name, false);
        if (printer_preset == nullptr)
            return out;
        const PresetWithVendorProfile printer(*printer_preset, printer_preset->vendor);
        for (const auto& [key, presets] : {std::pair<const char*, PresetCollection*>{"filament", &t.bundle->filaments},
                                           std::pair<const char*, PresetCollection*>{"print", &t.bundle->prints}}) {
            json names = json::array();
            for (const Preset& p : presets->get_presets()) {
                if (p.is_default || !p.can_overwrite())
                    continue; // the GUI's list includes these, but delete_preset refuses them
                const bool compatible = use_flags ? p.is_compatible :
                                                    is_compatible_with_printer(PresetWithVendorProfile(p, p.vendor), printer);
                if (!compatible)
                    continue;
                names.push_back(p.name);
                if (synced_out != nullptr && !p.setting_id.empty())
                    synced_out->push_back(p.name);
            }
            if (!names.empty())
                out[key] = names;
        }
        return out;
    };
    std::vector<std::string> synced;
    if (!preset->setting_id.empty())
        synced.push_back(name);
    const json cascade = cascade_of(selected, &synced);
    // Deleting a synced preset deletes its cloud copy too (Tab::select_preset queues it, and the
    // .info left behind is picked up at the next start). Cloud sync is the person's own.
    if (!synced.empty())
        return InvokeResult::error(WAB_FORBIDDEN_ORIGIN,
                                   "this would delete presets synced to the person's cloud account; automation does not touch cloud sync, "
                                   "so the person deletes them in the Settings tab",
                                   {{"cloudSynced", synced}, {"cascade", cascade}});
    if (!cascade.empty() && !ctx.params.value("cascade", false))
        return InvokeResult::error(WAB_BAD_PARAMS,
                                   "\"" + name + "\" is a printer you created: deleting it also deletes the filament and process presets "
                                   "listed in data.cascade. Retry with cascade:true to delete them all, or keep the printer",
                                   {{"path", "/params/cascade"}, {"cascade", cascade}, {"reason", "cascade"}});

    // Getting the tab onto the preset is a switch (ifDirty "ask" refuses before anything is asked).
    // Refusals first, before the question is asked; "transfer" has nowhere to go (the preset
    // switched to is the one deleted).
    if (!selected && (ctx.if_dirty == "ask" || ctx.if_dirty == "transfer")) {
        json        data;
        std::string message;
        if (const int code = presets_replace_preflight(preset_switch_scope(t.type), ctx.if_dirty, data, message); code != WAB_OK)
            return InvokeResult::error(code, message, data);
    }
    ConfirmRequest req;
    req.title        = "Delete preset";
    req.message      = std::string("Delete the ") + t.name + " preset \"" + name + "\"? Its file is removed; this cannot be undone.";
    req.accept_label = "Delete";
    if (selected && t.collection->current_is_dirty())
        for (const std::string& k : t.collection->current_dirty_options())
            req.details.push_back("Unsaved change: " + k);
    if (t.type == Preset::TYPE_PRINTER)
        for (const std::string& ph : t.bundle->physical_printers.get_printers_with_preset(name))
            req.details.push_back("Also removed from physical printer: " + ph);
    for (const auto& [key, names] : cascade.items())
        for (const json& n : names)
            req.details.push_back(std::string("Also deleted, ") + (key == "print" ? "process" : "filament") + " preset: " +
                                  n.get<std::string>());
    if (!cascade.empty())
        req.message = std::string("Delete the printer preset \"") + name + "\" and the " +
                      std::to_string(cascade.value("filament", json::array()).size()) + " filament and " +
                      std::to_string(cascade.value("print", json::array()).size()) +
                      " process presets made for it? Their files are removed; this cannot be undone.";
    if (!ctx.confirm(req))
        return InvokeResult::error(WAB_USER_CANCELLED, "the deletion was declined", {{"name", name}});
    if (!switch_tab_to(ctx, t, name, err))
        return err;
    // With the printer now the tab's, the cascade is exactly what Tab::select_preset will take. Anything
    // beyond what the person was shown, or a synced preset, and nothing is deleted.
    if (custom_printer && !selected) {
        std::vector<std::string> synced_now;
        const json               now   = cascade_of(true, &synced_now);
        bool                     extra = !synced_now.empty();
        for (const auto& [key, names] : now.items()) {
            const json shown = cascade.value(key, json::array());
            for (const json& n : names)
                extra = extra || std::find(shown.begin(), shown.end(), n) == shown.end();
        }
        if (extra)
            return InvokeResult::error(WAB_NEEDS_CONFIRMATION,
                                       "deleting this printer would also delete presets the confirmation did not list; nothing was deleted",
                                       {{"cascade", now}, {"cloudSynced", synced_now}});
    }

    if (t.type == Preset::TYPE_PRINTER && !t.bundle->physical_printers.empty())
        t.bundle->physical_printers.delete_preset_from_printers(name);
    // What Tab::delete_preset does once its own question is answered: the select deletes the
    // file, moves the selection on and refreshes the dependent presets and the sidebar.
    t.tab->select_preset("", true);
    if (t.collection->find_preset(name, false) != nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the preset is still installed");
    json result = {{"type", t.name}, {"deleted", name}, {"selected", t.collection->get_selected_preset_name()}};
    if (!cascade.empty())
        result["cascade"] = cascade; // deleted right after this reply, as the GUI does (Tab::select_preset's CallAfter)
    return InvokeResult::ok(result);
}

// ---------------------------------------------------------------------------------------------
// preset.diff
// ---------------------------------------------------------------------------------------------

InvokeResult run_diff(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    const Preset& edited = t.collection->get_edited_preset();

    // Side a: a named preset as saved, or (default) the tab's preset WITH its unsaved edits.
    const Preset* a      = &edited;
    bool          a_live = true;
    if (ctx.params.contains("a")) {
        const std::string n = resolve_name(t, ctx.params["a"].get<std::string>(), true);
        if (n.empty())
            return not_found(t, ctx.params["a"].get<std::string>());
        if (n != edited.name) {
            a      = t.collection->find_preset(n, false);
            a_live = false;
        }
    }
    // Side b: a named preset, else `against`: saved (default) or parent (the preset it is based on).
    const Preset* b       = nullptr;
    std::string   against = ctx.params.value("against", "saved");
    if (ctx.params.contains("b")) {
        const std::string n = resolve_name(t, ctx.params["b"].get<std::string>(), true);
        if (n.empty())
            return not_found(t, ctx.params["b"].get<std::string>());
        b       = t.collection->find_preset(n, false);
        against = "named";
    } else if (against == "parent") {
        b = t.collection->get_preset_parent(a_live ? t.collection->get_selected_preset() : *a);
        if (b == nullptr)
            return InvokeResult::ok({{"type", t.name},
                                     {"a", {{"name", a->name}, {"unsavedEdits", a_live && t.collection->current_is_dirty()}}},
                                     {"b", nullptr},
                                     {"against", against},
                                     {"count", 0},
                                     {"differences", json::array()},
                                     {"note", "\"" + a->name + "\" is not based on another preset"}});
    } else if (!a_live || against == "edited") {
        // A named preset has no unsaved edits to list: it is compared with the tab's preset as
        // edited (unsaved edits included), the other side the person is looking at.
        b = &edited;
        if (b == a) // a is the tab's preset as edited: "edited" means its saved state
            b = &t.collection->get_selected_preset();
        else
            against = "edited";
    } else {
        b = &t.collection->get_selected_preset();
    }

    std::vector<std::string> only;
    if (ctx.params.contains("keys"))
        only = ctx.params["keys"].get<std::vector<std::string>>();
    const auto   diffs     = PresetLogic::diff_configs(a->config, b->config, key_visible, only.empty() ? nullptr : &only);
    bool         truncated = false;
    const size_t limit     = static_cast<size_t>(ctx.params.value("limit", 200));
    json result = {{"type", t.name},
                   {"a", {{"name", a->name}, {"unsavedEdits", a_live && t.collection->current_is_dirty()}}},
                   {"b", {{"name", b->name}, {"isSystem", b->is_system}}},
                   {"against", against},
                   {"count", diffs.size()},
                   {"differences", diff_json(diffs, limit, truncated)},
                   {"truncated", truncated}};
    if (a_live)
        result["dirtyKeys"] = t.collection->current_dirty_options();
    return InvokeResult::ok(result);
}

// ---------------------------------------------------------------------------------------------
// preset.revertKeys / preset.discardEdits
// ---------------------------------------------------------------------------------------------

// What the tabs' per-option undo arrows can put back: every writable key, plus custom G-code and
// the compatible_* lists, which the bridge never writes with a value of its own but a revert only
// restores from the saved or parent preset. Print hosts and credentials never.
bool revertable(const std::string& key, std::string& why)
{
    if (WebAppSettings::key_writable(key, why))
        return true;
    if (WebAppSettings::key_is_secret(key))
        return false;
    if (WebAppSettings::key_is_code(key) || boost::algorithm::starts_with(key, "compatible_")) {
        why.clear();
        return true;
    }
    return false;
}

InvokeResult run_revert_keys(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    const std::string to        = ctx.params.value("to", "saved");
    const Preset*     reference = to == "parent" ? t.collection->get_selected_preset_parent() : &t.collection->get_selected_preset();
    if (reference == nullptr)
        return InvokeResult::error(WAB_NOT_FOUND, "\"" + t.collection->get_selected_preset_name() + "\" is not based on another preset");
    const auto                    keys = ctx.params["keys"].get<std::vector<std::string>>();
    const PresetLogic::RevertPlan plan = PresetLogic::plan_revert(*t.tab->get_config(), reference->config, keys, revertable);
    json skipped = json::array();
    for (const auto& [k, why] : plan.skipped)
        skipped.push_back({{"key", k}, {"message", why}});
    if (!plan.apply.empty()) {
        DynamicPrintConfig             copy    = *t.tab->get_config();
        const std::vector<std::string> written = PresetLogic::apply_revert(copy, reference->config, plan.apply);
        load_into_tab(ctx, t, copy, written);
    }
    InvokeResult r = InvokeResult::ok({{"type", t.name},
                                       {"to", to},
                                       {"reference", reference->name},
                                       {"reverted", plan.apply},
                                       {"unchanged", plan.unchanged},
                                       {"skipped", skipped},
                                       {"dirty", t.collection->current_is_dirty()}});
    if (plan.apply.empty())
        r.with_effects({});
    return r;
}

InvokeResult run_discard_edits(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    if (!t.collection->current_is_dirty()) {
        InvokeResult r = InvokeResult::ok({{"type", t.name}, {"discarded", json::array()}});
        return r.with_effects({});
    }
    const std::vector<std::string> dirty  = t.collection->current_dirty_options();
    const std::string              preset = t.collection->get_selected_preset_name();
    // Edits this turn made itself are the automation's to drop; any other edit is the person's.
    const bool own = PresetLogic::dirty_keys_all_written(dirty, turn_written_keys(ctx, t.name, preset));
    if (!own) {
        ConfirmRequest req;
        req.title        = "Discard changes";
        req.message      = std::string("Discard the unsaved changes to the ") + t.name + " preset \"" + preset + "\"?";
        req.details      = dirty;
        req.accept_label = "Discard";
        if (!ctx.confirm(req))
            return InvokeResult::error(WAB_USER_CANCELLED, "discarding was declined", {{"dirtyKeys", dirty}});
    }
    // The tab's undo button (Tab::on_roll_back_value): every option back to its saved value
    // through the option groups, then update(), so what depends on them -- the printer tab's
    // extruder count and its pages -- follows.
    {
        WebAppBridgeWriteScope scope;
        t.tab->on_roll_back_value(false);
    }
    if (t.collection->current_is_dirty()) { // an option no page shows: drop it the plain way
        t.collection->discard_current_changes();
        t.tab->update_dirty();
        t.tab->reload_config();
    }
    if (Plater* plater = wxGetApp().plater(); plater != nullptr)
        plater->sidebar().update_presets(t.type);
    return InvokeResult::ok({{"type", t.name}, {"preset", preset}, {"discarded", dirty}, {"confirmed", !own}});
}

// ---------------------------------------------------------------------------------------------
// preset.setVisible
// ---------------------------------------------------------------------------------------------

InvokeResult run_set_visible(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    const std::string requested = ctx.params.value("name", "");
    const std::string name      = resolve_name(t, requested, true);
    Preset*           preset    = name.empty() ? nullptr : t.collection->find_preset(name, false);
    if (preset == nullptr)
        return not_found(t, requested);
    const bool visible = ctx.params.value("visible", true);
    if (!preset->is_system)
        return InvokeResult::error(WAB_BAD_PARAMS, "only system presets are shown or hidden; your own presets are always listed");
    if (!visible && t.collection->get_selected_preset_name() == name)
        return InvokeResult::error(WAB_BAD_PARAMS, "the selected preset cannot be hidden; select another one first");
    if (preset->is_visible == visible) {
        InvokeResult r = InvokeResult::ok({{"type", t.name}, {"name", name}, {"visible", visible}, {"changed", false}});
        return r.with_effects({});
    }

    if (std::string why; !set_system_visible(t, name, visible, why))
        return InvokeResult::error(WAB_BAD_PARAMS, why);
    preset         = t.collection->find_preset(name, false);
    const bool now = preset != nullptr && preset->is_visible;
    refresh_after_preset_change(t);
    return InvokeResult::ok({{"type", t.name}, {"name", name}, {"visible", now}, {"changed", now == visible}});
}

// ---------------------------------------------------------------------------------------------
// preset.createFromTemplate
// ---------------------------------------------------------------------------------------------

InvokeResult run_create_from_template(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    const std::string name = ctx.params.value("name", "");
    if (InvokeResult bad = check_new_name(t, name, "/params/name"); !bad.is_ok())
        return bad;
    if (t.collection->find_preset(name, false) != nullptr || t.bundle->get_preset_name_by_alias(t.type, name) != name)
        return InvokeResult::error(WAB_BAD_PARAMS, std::string("a ") + t.name + " preset named \"" + name + "\" already exists",
                                   {{"exists", true}, {"path", "/params/name"}});
    const std::string base_req = ctx.params.value("base", "");
    const std::string base     = resolve_name(t, base_req, true);
    if (base.empty())
        return not_found(t, base_req);
    const int slot = ctx.params.value("slot", 0);
    if (t.type == Preset::TYPE_FILAMENT && (slot < 0 || static_cast<size_t>(slot) >= t.bundle->filament_presets.size()))
        return InvokeResult::error(WAB_BAD_PARAMS, "slot out of range (" + std::to_string(t.bundle->filament_presets.size()) + " slots)",
                                   {{"path", "/params/slot"}});

    // The values first, against a copy of the template, so a bad one refuses before anything moves.
    const Preset*            tmpl = t.collection->find_preset(base, false);
    DynamicPrintConfig       copy = tmpl->config;
    std::vector<std::string> applied;
    json                     rejected = json::array();
    const json               values   = ctx.params.contains("values") ? ctx.params["values"] : json::object();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const std::string&     key = it.key();
        const ConfigOptionDef* def = print_config_def.get(key);
        std::string            why;
        if (def == nullptr || copy.option(key) == nullptr) {
            rejected.push_back({{"key", key}, {"code", WAB_NOT_FOUND}, {"message", std::string("not a ") + t.name + " setting"}});
            continue;
        }
        if (!WebAppSettings::key_writable(key, why)) {
            rejected.push_back({{"key", key}, {"code", WAB_FORBIDDEN_ORIGIN}, {"message", why}});
            continue;
        }
        std::string text;
        if (!PresetLogic::json_to_ini(it.value(), *def, text)) {
            rejected.push_back({{"key", key}, {"code", WAB_BAD_PARAMS}, {"message", "value must be a string, number, bool or an array of those"}});
            continue;
        }
        try {
            ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
            copy.set_deserialize(key, text, context);
            applied.push_back(key);
        } catch (const std::exception& e) {
            copy.set_key_value(key, tmpl->config.option(key)->clone());
            rejected.push_back({{"key", key}, {"code", WAB_BAD_PARAMS}, {"message", e.what()}});
        }
    }
    if (!rejected.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "some values were refused; nothing was created", {{"rejected", rejected}});

    // A hidden system template is shown first, as ticking it would (printer, filament) or for
    // this session (process), so the tab can select it.
    if (!tmpl->is_visible && tmpl->is_system) {
        if (std::string why; !set_system_visible(t, base, true, why))
            return InvokeResult::error(WAB_BAD_PARAMS, "the hidden template cannot be shown: " + why, {{"path", "/params/base"}});
        refresh_after_preset_change(t);
    }
    const std::vector<std::string> slots_before = t.bundle->filament_presets;
    if (!switch_tab_to(ctx, t, base, err))
        return err;
    if (!applied.empty()) {
        DynamicPrintConfig live = *t.tab->get_config();
        live.apply_only(copy, applied, true);
        WebAppBridgeWriteScope scope;
        t.tab->load_config(live);
        WebAppSettings::apply_tab_couplings(*t.tab, live, applied);
    }
    t.tab->save_preset(name, false, false);
    if (t.collection->get_edited_preset().name != name)
        return InvokeResult::error(WAB_USER_CANCELLED, "the preset was not saved (the filament temperature check was declined)");

    json result = {{"type", t.name}, {"name", name}, {"base", base}, {"applied", applied}, {"selected", t.collection->get_selected_preset_name()}};
    if (t.type == Preset::TYPE_FILAMENT) {
        // Only the asked-for slot takes the new filament; every other slot keeps what it had.
        Plater* plater = wxGetApp().plater();
        for (size_t i = 0; i < slots_before.size() && i < t.bundle->filament_presets.size(); ++i) {
            const std::string want = static_cast<int>(i) == slot ? name : slots_before[i];
            if (t.bundle->filament_presets[i] != want)
                plater->select_filament_preset(i, want);
        }
        result["slot"]  = slot;
        result["slots"] = t.bundle->filament_presets;
    }
    return InvokeResult::ok(result);
}

// ---------------------------------------------------------------------------------------------
// preset.exportBundle
// ---------------------------------------------------------------------------------------------

// A user preset's file, as its JSON, with every key of WebAppPresetShare::stripped_keys() removed:
// a printer preset holds the print host and its credentials when the person set them up
// (PhysicalPrinterDialog saves them into the preset), and they never go into a bundle. The GUI's
// ExportConfigsDialog strips the same keys (earse_preset_fields_for_safe).
bool sanitised_preset_file(const std::string& path, std::string& text, std::vector<std::string>& stripped)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    json j = json::parse(buffer.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return false;
    stripped = PresetLogic::strip_preset_json(j, WebAppPresetShare::stripped_keys());
    text     = j.dump(4);
    return true;
}

// A project preset has no file: its config written out the way Save writes a user preset, then
// read back as JSON.
bool project_preset_text(const Preset& p, std::string& text, std::vector<std::string>& stripped)
{
    boost::system::error_code ec;
    const fs::path            dir = fs::path(data_dir()) / "automation" / "export";
    fs::create_directories(dir, ec);
    const fs::path tmp = dir / ("project-preset-" + std::to_string(std::hash<std::string>{}(p.name)) + ".json");
    try {
        // The importer refuses a file whose version it cannot parse.
        p.config.save_to_json(tmp.string(), get_preset_bare_name(p.name), "User",
                              p.version.valid() ? p.version.to_string() : std::string(SLIC3R_VERSION));
    } catch (const std::exception&) {
        fs::remove(tmp, ec);
        return false;
    }
    const bool ok = sanitised_preset_file(tmp.string(), text, stripped);
    fs::remove(tmp, ec);
    return ok;
}

InvokeResult run_export_bundle(InvokeContext& ctx)
{
    // zip: the presets' files in a plain archive (Import Configs reads it back as user presets).
    // bundle: an .orca_bundle -- the Preset Bundle dialog's export, with bundle_structure.json and
    // the metadata the bundle list shows; it imports as one bundle.
    const bool bundle_format = ctx.params.value("format", "zip") == "bundle";
    struct Entry
    {
        std::string in_zip;
        std::string text;
    };
    std::vector<Entry> entries;
    json               listed   = json::array();
    json               stripped = json::array();
    json               by_type  = {{"printer", json::array()}, {"filament", json::array()}, {"print", json::array()}};
    for (const json& item : ctx.params["presets"]) {
        Preset::Type type;
        TypeCtx      t;
        if (!WebAppSettings::preset_type_from_string(item.value("type", ""), type) || !type_ctx(type, t))
            return InvokeResult::error(WAB_BAD_PARAMS, "each preset needs a type", {{"path", "/params/presets"}});
        const std::string name = resolve_name(t, item.value("name", ""));
        const Preset*     p    = name.empty() ? nullptr : t.collection->find_preset(name, false);
        if (p == nullptr)
            return not_found(t, item.value("name", ""));
        const bool has_file = p->is_user() && !p->file.empty() && fs::exists(fs::path(p->file));
        if (!has_file && !p->is_project_embedded)
            return InvokeResult::error(WAB_BAD_PARAMS, "\"" + name + "\" is not one of your saved or project presets; only those can be exported",
                                       {{"name", name}});
        Entry                    e;
        std::vector<std::string> removed;
        if (!(has_file ? sanitised_preset_file(p->file, e.text, removed) : project_preset_text(*p, e.text, removed)))
            return InvokeResult::error(WAB_IO, "\"" + name + "\" could not be read", {{"name", name}});
        const std::string file = has_file ? fs::path(p->file).filename().string() : get_preset_bare_name(p->name) + ".json";
        // The bundle dialog's folders are printer/, filament/ and process/.
        e.in_zip = (bundle_format && t.type == Preset::TYPE_PRINT ? std::string("process") : std::string(t.name)) + "/" + file;
        by_type[t.name].push_back(e.in_zip);
        entries.push_back(std::move(e));
        listed.push_back({{"type", t.name}, {"name", name}});
        if (!removed.empty())
            stripped.push_back({{"type", t.name}, {"name", name}, {"keys", removed}});
    }
    const char* ext          = bundle_format ? ".orca_bundle" : ".zip";
    std::string default_name = ctx.params.value("fileName", bundle_format ? "presets.orca_bundle" : "presets.zip");
    if (fs::path(default_name).extension() != ext)
        default_name += ext;
    if (bundle_format) {
        // ExportPresetBundleDialog::OnExportData's structure; "name" makes the importer keep it as a
        // bundle under that name (read_export_archive_info).
        const json meta = ctx.params.contains("metadata") ? ctx.params["metadata"] : json::object();
        json       structure = {{"bundle_type", "printer config bundle"},
                                {"printer_preset_name", "export"},
                                {"name", meta.value("name", fs::path(default_name).stem().string())},
                                {"version", meta.value("version", std::string())},
                                {"author", meta.value("author", std::string())},
                                {"description", meta.value("description", std::string())},
                                {"printer_config", by_type["printer"]},
                                {"filament_config", by_type["filament"]},
                                {"process_config", by_type["print"]}};
        entries.push_back({BUNDLE_STRUCTURE_JSON_NAME, structure.dump(4)});
    }
    // The native Save dialog is the person's consent: they pick where the file goes.
    std::string path;
    if (!native_save_path("Export presets", default_name,
                          bundle_format ? "Preset bundle (*.orca_bundle)|*.orca_bundle" : "Preset bundle (*.zip)|*.zip", path))
        return InvokeResult::error(WAB_USER_CANCELLED, "the save was cancelled");

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_writer_init_file(&zip, encode_path(path.c_str()).c_str(), 0))
        return InvokeResult::error(WAB_IO, "could not create " + path);
    bool ok = true;
    for (const Entry& e : entries)
        ok = ok && mz_zip_writer_add_mem(&zip, e.in_zip.c_str(), e.text.data(), e.text.size(), MZ_DEFAULT_COMPRESSION);
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    if (!ok) {
        boost::system::error_code ec;
        fs::remove(fs::path(path), ec);
        return InvokeResult::error(WAB_IO, "could not write the bundle");
    }
    // Names of the keys only, never their values.
    return InvokeResult::ok({{"path", path},
                             {"file", fs::path(path).filename().string()},
                             {"format", bundle_format ? "bundle" : "zip"},
                             {"presets", listed},
                             {"stripped", stripped}});
}

// ---------------------------------------------------------------------------------------------
// preset.importBundle / preset.importConfig
// ---------------------------------------------------------------------------------------------

// What a downloaded preset file may not bring in, whatever it says: everything
// WebAppPresetShare::stripped_keys() lists (print hosts, credentials, upload switches, paths on
// the author's disk) and post-processing scripts, which the slicer would run after every slice.
// The same rule slicer.installPrinterPreset applies; the Import Configs menu applies none.
const std::vector<std::string>& import_stripped_keys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k = WebAppPresetShare::stripped_keys();
        k.emplace_back("post_process");
        return k;
    }();
    return keys;
}

// The custom-G-code keys a preset may still carry in: shown to the person, line by line where a
// line does something risky, before the import.
const std::vector<std::string>& import_code_keys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const std::string& key : WebAppSettings::WAB_CODE_KEYS)
            if (key != "post_process")
                k.push_back(key);
        return k;
    }();
    return keys;
}

// One preset file of a download, made safe. `flagged` collects its risky G-code lines.
struct ImportScan
{
    std::vector<PresetLogic::ImportReview>       presets;
    std::vector<WebAppPresetShare::FlaggedLine>  flagged;
};

bool sanitise_import_text(const std::string& in, std::string& out, ImportScan& scan)
{
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || PresetLogic::preset_json_type(j).empty())
        return false;
    PresetLogic::ImportReview review = PresetLogic::review_import_json(j, import_stripped_keys(), import_code_keys());
    for (const std::string& k : review.code_keys)
        WebAppPresetShare::scan_gcode_text(review.name + ": " + k, PresetLogic::preset_json_text(j, k), scan.flagged);
    scan.presets.push_back(std::move(review));
    out = j.dump(4);
    return true;
}

// Writes the sanitised copy of `local` (a .json, or a bundle archive whose preset JSONs are each
// sanitised; bundle_structure.json is kept, anything else dropped) to `out`, with the same
// extension, so PresetBundle::import_presets treats it as it would the original.
bool sanitise_import_file(const std::string& local, const std::string& out, ImportScan& scan, std::string& error)
{
    const std::string ext = boost::algorithm::to_lower_copy(fs::path(local).extension().string());
    if (ext == ".json") {
        boost::nowide::ifstream in(local, std::ios::binary);
        if (!in) {
            error = "the file could not be read";
            return false;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string text;
        if (!sanitise_import_text(buffer.str(), text, scan)) {
            error = "the file is not a printer, process or filament preset";
            return false;
        }
        boost::nowide::ofstream o(out, std::ios::binary);
        o << text;
        return o.good();
    }

    // An archive. Limits against a hostile one: entries and their unpacked size.
    static const size_t MAX_ENTRIES = 2000, MAX_ENTRY_BYTES = size_t(16) << 20, MAX_TOTAL_BYTES = size_t(256) << 20;
    mz_zip_archive reader;
    mz_zip_zero_struct(&reader);
    if (!mz_zip_reader_init_file(&reader, encode_path(local.c_str()).c_str(), 0)) {
        error = "the file is not a preset bundle (not a zip archive)";
        return false;
    }
    mz_zip_archive writer;
    mz_zip_zero_struct(&writer);
    if (!mz_zip_writer_init_file(&writer, encode_path(out.c_str()).c_str(), 0)) {
        mz_zip_reader_end(&reader);
        error = "could not prepare the import";
        return false;
    }
    bool         ok    = true;
    size_t       total = 0;
    const size_t count = mz_zip_reader_get_num_files(&reader);
    if (count > MAX_ENTRIES) {
        ok    = false;
        error = "the bundle has too many files";
    }
    for (size_t i = 0; ok && i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&reader, mz_uint(i), &st) || st.m_is_directory)
            continue;
        const std::string entry = st.m_filename;
        const std::string base  = fs::path(entry).filename().string();
        if (!boost::algorithm::iends_with(base, ".json"))
            continue;
        if (st.m_uncomp_size > MAX_ENTRY_BYTES || (total += size_t(st.m_uncomp_size)) > MAX_TOTAL_BYTES) {
            ok    = false;
            error = "the bundle is too large";
            break;
        }
        size_t size = 0;
        void*  data = mz_zip_reader_extract_to_heap(&reader, mz_uint(i), &size, 0);
        if (data == nullptr)
            continue;
        std::string text(static_cast<const char*>(data), size);
        mz_free(data);
        std::string clean;
        if (base == BUNDLE_STRUCTURE_JSON_NAME)
            clean = text;
        else if (!sanitise_import_text(text, clean, scan))
            continue; // not a preset: import_presets would skip it too
        ok = mz_zip_writer_add_mem(&writer, entry.c_str(), clean.data(), clean.size(), MZ_DEFAULT_COMPRESSION);
        if (!ok)
            error = "could not prepare the import";
    }
    ok = mz_zip_writer_finalize_archive(&writer) && ok;
    mz_zip_writer_end(&writer);
    mz_zip_reader_end(&reader);
    if (ok && scan.presets.empty()) {
        ok    = false;
        error = "the bundle holds no printer, process or filament preset";
    }
    return ok;
}

// What MainFrame's install_missing_import_parents finds: for each vendor this build ships, the
// parents of the imported presets it has (and the printer models to enable for them).
struct ParentVendorNeed
{
    bool                                             installed = false;
    std::vector<std::string>                         parents;
    std::vector<std::pair<std::string, std::string>> models; // (printer_model, printer_variant)
};
using ParentVendorNeeds = std::map<std::string, ParentVendorNeed>;

ParentVendorNeeds parent_vendor_needs(PresetBundle& bundle, const std::string& file)
{
    ParentVendorNeeds needs;
    for (const auto& [subdir, parent] : PresetBundle::collect_import_parents({file})) {
        PresetCollection& coll = subdir == PRESET_PRINTER_NAME ? static_cast<PresetCollection&>(bundle.printers) :
                                 subdir == PRESET_PRINT_NAME   ? bundle.prints :
                                                                 bundle.filaments;
        if (coll.find_preset2(parent) != nullptr)
            continue; // already here
        std::string       model, variant;
        const std::string vendor = PresetBundle::find_resource_vendor_for_preset(subdir, parent, &model, &variant);
        if (vendor.empty())
            continue; // not shipped: the import reports it as a missing parent
        const bool installed = bundle.vendors.find(vendor) != bundle.vendors.end();
        if (installed && model.empty())
            continue; // the vendor is here and simply has no such preset
        ParentVendorNeed& need = needs[vendor];
        need.installed         = installed;
        if (std::find(need.parents.begin(), need.parents.end(), parent) == need.parents.end())
            need.parents.push_back(parent);
        if (!model.empty())
            need.models.emplace_back(model, variant);
    }
    return needs;
}

// The install half of install_missing_import_parents: vendors from the resources, their models
// ticked as the setup wizard would, every preset reloaded.
bool install_parent_vendors(PresetBundle& bundle, const ParentVendorNeeds& needs, std::string& error)
{
    AppConfig&               cfg = *wxGetApp().app_config;
    std::vector<std::string> to_install;
    for (const auto& [vendor, need] : needs)
        if (!need.installed)
            to_install.push_back(vendor);
    if (!to_install.empty() && !Slic3r::install_vendor_bundles_from_resources(to_install)) {
        error = "the printer profiles could not be installed from the application's resources";
        return false;
    }
    for (const auto& [vendor, need] : needs)
        for (const auto& [model, variant] : need.models)
            if (!cfg.get_variant(vendor, model, variant))
                cfg.set_variant(vendor, model, variant, true);
    cfg.save();
    bundle.load_presets(cfg, ForwardCompatibilitySubstitutionRule::EnableSilent);
    return true;
}

InvokeResult import_local_file(InvokeContext& ctx, const std::string& local, const std::string& url)
{
    // A sanitised copy is what gets imported; the download itself is never read by the importer.
    // Same file name (a bundle is named after its archive), in a folder of its own.
    boost::system::error_code ec;
    const fs::path            clean_dir = fs::path(local).parent_path() / "sanitised";
    fs::create_directories(clean_dir, ec);
    const fs::path clean = clean_dir / fs::path(local).filename();
    ImportScan                scan;
    std::string               error;
    if (!sanitise_import_file(local, clean.string(), scan, error)) {
        fs::remove_all(clean_dir, ec);
        return InvokeResult::error(WAB_UNSUPPORTED_TYPE, error + "; nothing was imported", {{"file", fs::path(local).filename().string()}});
    }

    ConfirmRequest req;
    req.title   = "Import presets";
    req.message = "Import " + std::to_string(scan.presets.size()) + " preset(s) from \"" + fs::path(local).filename().string() +
                  "\" into your preset folder?";
    req.details = {"From: " + url};
    json review = json::array();
    for (const PresetLogic::ImportReview& r : scan.presets) {
        req.details.push_back(r.type + " preset: " + r.name);
        if (!r.removed.empty())
            req.details.push_back("  Removed (never imported): " + boost::algorithm::join(r.removed, ", "));
        if (!r.code_keys.empty())
            req.details.push_back("  Carries custom G-code: " + boost::algorithm::join(r.code_keys, ", "));
        review.push_back({{"name", r.name}, {"type", r.type}, {"removed", r.removed}, {"customGcode", r.code_keys}});
    }
    if (!scan.flagged.empty()) {
        req.details.push_back("Look at these G-code lines before you import:");
        for (const std::string& line : WebAppPresetShare::format_flagged(scan.flagged))
            req.details.push_back("  " + line);
    }
    // Parents the presets need that a vendor this build ships has (MainFrame's
    // install_missing_import_parents, without its question: installVendors says yes).
    PresetBundle&           bundle  = *wxGetApp().preset_bundle;
    const ParentVendorNeeds needs   = parent_vendor_needs(bundle, clean.string());
    const bool              install = ctx.params.value("installVendors", false) && !needs.empty();
    json                    vendors = json::array();
    for (const auto& [vendor, need] : needs)
        vendors.push_back({{"vendor", vendor}, {"installed", need.installed}, {"parents", need.parents}});
    const std::vector<Preset::Type> all_types = {Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT};
    if (install) {
        // Installing a vendor reloads every preset: unsaved edits are refused or handled first.
        if (ctx.if_dirty == "ask" || ctx.if_dirty == "transfer") {
            json        data;
            std::string message;
            if (const int code = presets_replace_preflight(all_types, ctx.if_dirty, data, message); code != WAB_OK) {
                fs::remove_all(clean_dir, ec);
                return InvokeResult::error(code, "installing printer profiles reloads every preset: " + message, data);
            }
        }
        for (const auto& [vendor, need] : needs)
            req.details.push_back((need.installed ? "Also enables, from the installed " : "Also installs the ") + vendor +
                                  " printer profiles, needed by: " + boost::algorithm::join(need.parents, ", "));
    }
    req.accept_label = "Import";
    req.destructive  = false;
    if (!ctx.confirm(req)) {
        fs::remove_all(clean_dir, ec);
        return InvokeResult::error(WAB_USER_CANCELLED, "the import was declined");
    }
    if (install) {
        if (ctx.if_dirty == "prompt") {
            if (wxGetApp().has_current_preset_changes() &&
                !wxGetApp().check_and_save_current_preset_changes(_L("Install printer profiles"),
                                                                  _L("Installing printer profiles reloads every preset."), false)) {
                fs::remove_all(clean_dir, ec);
                return InvokeResult::error(WAB_USER_CANCELLED, "the user kept the unsaved changes; nothing was imported");
            }
        } else {
            json        data;
            std::string message;
            if (const int code = presets_replace_preflight(all_types, ctx.if_dirty, data, message); code != WAB_OK) {
                fs::remove_all(clean_dir, ec);
                return InvokeResult::error(code, message, data);
            }
        }
        if (std::string error; !install_parent_vendors(bundle, needs, error)) {
            fs::remove_all(clean_dir, ec);
            return InvokeResult::error(WAB_IO, error + "; nothing was imported");
        }
    }

    const bool                 overwrite = ctx.params.value("overwrite", false);
    std::vector<std::string>   files     = {clean.string()};
    PresetBundle::ImportReport report;
    // ConfigsOverwriteConfirmDialog's answers by index: no, yes, no to all, yes to all.
    bundle.import_presets(
        files, [overwrite](const std::string&) { return overwrite ? 3 : 2; }, ForwardCompatibilitySubstitutionRule::Enable,
        *wxGetApp().app_config, &report);
    fs::remove_all(clean_dir, ec);
    wxGetApp().load_current_presets();
    bundle.update_compatible(PresetSelectCompatibleType::Always);
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->update_side_preset_ui();
    json missing = json::array();
    for (const auto& [n, parent] : report.missing_parents)
        missing.push_back({{"name", n}, {"needs", parent}});
    json result = {{"imported", report.imported}, {"missingParents", missing}, {"review", review}};
    if (install)
        result["installedVendors"] = vendors;
    else if (!needs.empty())
        result["installableVendors"] = vendors; // retry with installVendors:true to install them first
    if (report.imported.empty())
        result["hint"] = "nothing was imported: a preset needs its printer (or parent preset) installed first, and existing names "
                         "are kept unless overwrite:true";
    return InvokeResult::ok(result);
}

Runner import_runner(bool bundle_only)
{
    return [bundle_only](InvokeContext& ctx) -> InvokeResult {
        if (ctx.host == nullptr || !ctx.host->download)
            return InvokeResult::error(WAB_CAPABILITY_DISABLED, "this tab cannot download files");
        std::vector<std::string> exts = {"zip", "orca_bundle", "orca_printer", "orca_filament"};
        if (!bundle_only)
            exts.push_back("json");
        const std::string     url      = ctx.params.value("url", "");
        std::shared_ptr<Deferred> deferred = ctx.defer();
        std::weak_ptr<bool>   alive    = ctx.host->alive;
        InvokeContext         later    = ctx; // the completion runs after this call returns
        ctx.host->download(
            url, exts,
            [deferred, alive, later, url](const std::string& local) mutable {
                if (alive.expired())
                    return;
                deferred->finish(import_local_file(later, local, url));
            },
            [deferred](int code, const std::string& message) { deferred->finish(InvokeResult::error(code, message)); });
        return InvokeResult::started();
    };
}

// ---------------------------------------------------------------------------------------------
// printer.listPhysical
// ---------------------------------------------------------------------------------------------

InvokeResult run_list_physical(InvokeContext&)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the preset bundle is not ready");
    // Names and the presets they use only: the host, its credentials and certificates stay put.
    json list = json::array();
    for (const PhysicalPrinter& p : bundle->physical_printers) {
        const std::set<std::string>& presets = p.get_preset_names();
        list.push_back({{"name", p.name}, {"presets", std::vector<std::string>(presets.begin(), presets.end())}});
    }
    return InvokeResult::ok({{"printers", list}});
}

// ---------------------------------------------------------------------------------------------
// preset.setCompatibility
// ---------------------------------------------------------------------------------------------

// The tab's "Profile dependencies" group (Tab::compatible_widget_create): the "All" checkbox and
// the "Set..." list of printers -- and, for a filament, of processes. An unsaved edit of the tab's
// preset, as the widget makes; it decides where the preset is listed once saved.
InvokeResult run_set_compatibility(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    if (t.type == Preset::TYPE_PRINTER)
        return InvokeResult::error(WAB_BAD_PARAMS, "printer presets have no compatibility lists; set them on process and filament presets",
                                   {{"path", "/params/type"}});
    if (!ctx.params.contains("printers") && !ctx.params.contains("prints"))
        return InvokeResult::error(WAB_BAD_PARAMS, "give printers, prints or both", {{"path", "/params"}, {"needs", "printers"}});
    if (ctx.params.contains("prints") && t.type != Preset::TYPE_FILAMENT)
        return InvokeResult::error(WAB_BAD_PARAMS, "only filament presets list the processes they suit", {{"path", "/params/prints"}});
    if (ctx.params.contains("name")) {
        const std::string name = resolve_name(t, ctx.params["name"].get<std::string>());
        if (name.empty())
            return not_found(t, ctx.params["name"].get<std::string>());
        if (!switch_tab_to(ctx, t, name, err))
            return err;
    }
    DynamicPrintConfig       copy = *t.tab->get_config();
    std::vector<std::string> keys;
    json                     unknown = json::array();
    auto list = [&](const char* param, Preset::Type of, const char* key) {
        if (!ctx.params.contains(param))
            return;
        TypeCtx o;
        type_ctx(of, o);
        std::vector<std::string> names;
        for (const json& n : ctx.params[param]) {
            const std::string r = resolve_name(o, n.get<std::string>(), true);
            if (r.empty())
                unknown.push_back(n);
            else if (std::find(names.begin(), names.end(), r) == names.end())
                names.push_back(r);
        }
        copy.option<ConfigOptionStrings>(key, true)->values = names; // empty: all (the "All" checkbox)
        keys.push_back(key);
    };
    list("printers", Preset::TYPE_PRINTER, "compatible_printers");
    list("prints", Preset::TYPE_PRINT, "compatible_prints");
    if (!unknown.empty())
        return InvokeResult::error(WAB_NOT_FOUND, "these are not installed presets: nothing was changed", {{"unknown", unknown}});
    load_into_tab(ctx, t, copy, keys);
    t.bundle->update_compatible(PresetSelectCompatibleType::Never);
    refresh_after_preset_change(t);
    json result = {{"type", t.name}, {"preset", t.collection->get_edited_preset().name}, {"dirty", t.collection->current_is_dirty()}};
    for (const std::string& k : keys)
        result[k == "compatible_printers" ? "printers" : "prints"] = t.tab->get_config()->option<ConfigOptionStrings>(k)->values;
    return InvokeResult::ok(result);
}

// ---------------------------------------------------------------------------------------------
// preset.transfer
// ---------------------------------------------------------------------------------------------

// The compare dialog's "Transfer values from left to right" (MainFrame::bind_diff_dialog ->
// Tab::transfer_options): the tab switches to `to` (under ifDirty) and takes `keys` from `from`
// as unsaved edits, recorded for undoTurn.
InvokeResult run_transfer(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    const std::string from = resolve_name(t, ctx.params.value("from", ""), true);
    if (from.empty())
        return not_found(t, ctx.params.value("from", ""));
    const std::string to = resolve_name(t, ctx.params.value("to", ""));
    if (to.empty())
        return not_found(t, ctx.params.value("to", ""));
    if (from == to)
        return InvokeResult::error(WAB_BAD_PARAMS, "from and to are the same preset", {{"path", "/params/to"}});
    const DynamicPrintConfig source = t.collection->find_preset(from, false)->config; // as saved, as the dialog compares
    if (!switch_tab_to(ctx, t, to, err))
        return err;
    const PresetLogic::RevertPlan plan =
        PresetLogic::plan_revert(*t.tab->get_config(), source, ctx.params["keys"].get<std::vector<std::string>>(), revertable);
    json skipped = json::array();
    for (const auto& [k, why] : plan.skipped)
        skipped.push_back({{"key", k}, {"message", why}});
    std::vector<std::string> written;
    if (!plan.apply.empty()) {
        DynamicPrintConfig copy = *t.tab->get_config();
        written                 = PresetLogic::apply_revert(copy, source, plan.apply);
        load_into_tab(ctx, t, copy, written);
    }
    InvokeResult r = InvokeResult::ok({{"type", t.name},
                                       {"from", from},
                                       {"to", to},
                                       {"transferred", plan.apply},
                                       {"unchanged", plan.unchanged},
                                       {"skipped", skipped},
                                       {"dirty", t.collection->current_is_dirty()}});
    return r;
}

// ---------------------------------------------------------------------------------------------
// settings.copyExtruder
// ---------------------------------------------------------------------------------------------

// The tabs' "copy to the other extruder" button (Tab::sync_excluder): per-variant values of one
// extruder written over another's, as unsaved edits. Without keys, the edited ones -- what the
// button copies -- with keys, those.
InvokeResult run_copy_extruder(InvokeContext& ctx)
{
    TypeCtx      t;
    InvokeResult err;
    if (!resolve_type(ctx, t, err))
        return err;
    const int count = WebAppSettings::printer_extruder_count();
    const int from  = ctx.params.value("fromExtruder", 0);
    const int to    = ctx.params.value("toExtruder", 1);
    if (count < 2)
        return InvokeResult::error(WAB_BAD_PARAMS, "the printer has one extruder; there is nothing to copy to", {{"extruders", count}});
    if (from == to || from >= count || to >= count)
        return InvokeResult::error(WAB_BAD_PARAMS, "fromExtruder and toExtruder must be two different extruders below " + std::to_string(count),
                                   {{"path", "/params/toExtruder"}, {"extruders", count}});
    const auto* types = t.bundle->printers.get_edited_preset().config.option<ConfigOptionEnumsGeneric>("extruder_type");
    if (types != nullptr && types->get_at(from) != types->get_at(to))
        return InvokeResult::error(WAB_BAD_PARAMS, "the two extruders are of different types (direct drive and Bowden); their values do not carry over");
    const DynamicPrintConfig& cfg      = *t.tab->get_config();
    const int                 from_idx = WebAppSettings::variant_index_for(*t.tab, cfg, from);
    const int                 to_idx   = WebAppSettings::variant_index_for(*t.tab, cfg, to);
    if (from_idx < 0 || to_idx < 0 || from_idx == to_idx)
        return InvokeResult::error(WAB_BAD_PARAMS, std::string("this ") + t.name + " preset has no separate values for those extruders");

    std::vector<std::string> keys;
    json                     skipped = json::array();
    if (ctx.params.contains("keys")) {
        for (const json& k : ctx.params["keys"]) {
            const std::string key = k.get<std::string>();
            std::string       why;
            if (WebAppSettings::variant_stride(t.type, key) == 0 || cfg.option(key) == nullptr)
                skipped.push_back({{"key", key}, {"message", "not a per-extruder setting of this preset"}});
            else if (!WebAppSettings::key_writable(key, why))
                skipped.push_back({{"key", key}, {"message", why}});
            else
                keys.push_back(key);
        }
    } else {
        // The edited per-variant values of `from` ("key#i" in the deep dirty list).
        for (const std::string& d : t.collection->current_dirty_options(true)) {
            std::string base;
            size_t      index = 0;
            if (!PresetLogic::split_indexed_key(d, base, index))
                continue;
            const int stride = WebAppSettings::variant_stride(t.type, base);
            std::string why;
            if (stride > 0 && int(index) / stride == from_idx && WebAppSettings::key_writable(base, why) &&
                std::find(keys.begin(), keys.end(), base) == keys.end())
                keys.push_back(base);
        }
    }
    DynamicPrintConfig       copy = cfg;
    std::vector<std::string> copied;
    for (const std::string& key : keys) {
        auto*       dst    = dynamic_cast<ConfigOptionVectorBase*>(copy.option(key));
        const auto* src    = dynamic_cast<const ConfigOptionVectorBase*>(cfg.option(key));
        const int   stride = WebAppSettings::variant_stride(t.type, key);
        if (dst == nullptr || src == nullptr || src->size() < size_t((std::max(from_idx, to_idx) + 1) * stride))
            continue;
        std::unique_ptr<ConfigOption> before(dst->clone());
        for (int j = 0; j < stride; ++j)
            dst->set_at(src, size_t(to_idx * stride + j), size_t(from_idx * stride + j));
        if (!(*before == *dst))
            copied.push_back(key);
    }
    if (!copied.empty())
        load_into_tab(ctx, t, copy, copied);
    InvokeResult r = InvokeResult::ok({{"type", t.name},
                                       {"fromExtruder", from},
                                       {"toExtruder", to},
                                       {"copied", copied},
                                       {"skipped", skipped},
                                       {"note", copied.empty() ? json("no modifications need to be copied") : json(nullptr)}});
    if (copied.empty())
        r.with_effects({});
    return r;
}

// ---------------------------------------------------------------------------------------------
// printer.setExtruderCount
// ---------------------------------------------------------------------------------------------

// The printer tab's "Extruders" field (Multimaterial page): TabPrinter::extruders_count_changed,
// which resizes the per-extruder settings, the filament slots and the plates' data.
InvokeResult run_set_extruder_count(InvokeContext& ctx)
{
    TypeCtx t;
    if (!type_ctx(Preset::TYPE_PRINTER, t))
        return InvokeResult::error(WAB_INTERNAL, "the preset bundle is not ready");
    auto* tab = dynamic_cast<TabPrinter*>(t.tab);
    if (tab == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "no printer settings tab");
    if (t.bundle->is_bbl_vendor())
        return InvokeResult::error(WAB_BAD_PARAMS, "this printer's extruders are fixed by the machine; choose another printer preset instead",
                                   {{"reason", "fixed_extruders"}});
    const size_t count  = size_t(ctx.params.value("count", 1));
    const size_t before = tab->m_extruders_count;
    if (count > 1 && t.tab->get_config()->opt_bool("single_extruder_multi_material"))
        return InvokeResult::error(WAB_BAD_PARAMS,
                                   "single-extruder multi-material is on, which means one extruder; turn it off first "
                                   "(settings.set single_extruder_multi_material false)",
                                   {{"reason", "semm"}});
    if (count == before)
        return InvokeResult::ok({{"before", before}, {"count", before}, {"changed", false}}).with_effects({});
    {
        WebAppBridgeWriteScope scope;
        tab->extruders_count_changed(count);
        tab->init_options_list();
        tab->update_dirty();
    }
    if (Plater* plater = wxGetApp().plater(); plater != nullptr)
        plater->sidebar().update_presets(Preset::TYPE_FILAMENT);
    return InvokeResult::ok({{"before", before},
                             {"count", tab->m_extruders_count},
                             {"changed", tab->m_extruders_count != before},
                             {"dirty", t.collection->current_is_dirty()}});
}

// ---------------------------------------------------------------------------------------------
// Downloads a setting points at: bed.setShape (texture, model), settings.setFileFromUrl
// ---------------------------------------------------------------------------------------------

// Where a downloaded file is kept for a setting to point at -- the slicer's own data folder, since
// the download's temporary folder goes away: <data_dir>/automation/<folder>/<file name>.
bool keep_download(const std::string& local, const std::string& folder, std::string& kept, std::string& error)
{
    boost::system::error_code ec;
    const fs::path            dir = fs::path(data_dir()) / "automation" / folder;
    fs::create_directories(dir, ec);
    const fs::path to = dir / fs::path(local).filename();
    fs::copy_file(fs::path(local), to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "could not keep the downloaded file: " + ec.message();
        return false;
    }
    kept = to.string();
    return true;
}

struct Fetch
{
    std::string              key; // the setting that will point at the file
    std::string              url;
    std::vector<std::string> exts;
    std::string              folder;
};

// Downloads `todo` one after another (a tab downloads one file at a time), keeping each; then
// `done(0, paths by key)` or `done(code, message)` once. GUI thread.
void fetch_all(BridgeHost* host, std::shared_ptr<std::vector<Fetch>> todo, size_t i, std::shared_ptr<std::map<std::string, std::string>> got,
               std::function<void(int, const std::string&)> done)
{
    if (i >= todo->size()) {
        done(WAB_OK, std::string());
        return;
    }
    const Fetch         f     = (*todo)[i];
    std::weak_ptr<bool> alive = host->alive;
    host->download(
        f.url, f.exts,
        [host, todo, i, got, done, f, alive](const std::string& local) {
            if (alive.expired())
                return;
            std::string kept, error;
            if (!keep_download(local, f.folder, kept, error)) {
                done(WAB_IO, error);
                return;
            }
            (*got)[f.key] = kept;
            fetch_all(host, todo, i + 1, got, done);
        },
        [done](int code, const std::string& message) { done(code, message); });
}

// Writes `values` (keys of `keys`) into the tab of `type` as unsaved edits, recorded for undoTurn.
InvokeResult write_values(InvokeContext& ctx, Preset::Type type, const DynamicPrintConfig& values, const std::vector<std::string>& keys)
{
    TypeCtx t;
    if (!type_ctx(type, t))
        return InvokeResult::error(WAB_INTERNAL, "the preset bundle is not ready");
    if (wxGetApp().mainframe == nullptr || !wxGetApp().mainframe->IsEnabled())
        return InvokeResult::error(WAB_BUSY, "a dialog is open; nothing was changed");
    DynamicPrintConfig copy = *t.tab->get_config();
    copy.apply_only(values, keys, true);
    load_into_tab(ctx, t, copy, keys);
    json written = json::object();
    for (const std::string& k : keys)
        written[k] = t.tab->get_config()->opt_serialize(k);
    return InvokeResult::ok({{"type", t.name}, {"preset", t.collection->get_edited_preset().name}, {"values", written},
                             {"dirty", t.collection->current_is_dirty()}});
}

// Runs the downloads, then writes `values` plus each kept file's path under its key.
InvokeResult write_after_downloads(InvokeContext& ctx, Preset::Type type, DynamicPrintConfig values, std::vector<std::string> keys,
                                   std::vector<Fetch> fetches)
{
    if (fetches.empty())
        return write_values(ctx, type, values, keys);
    if (ctx.host == nullptr || !ctx.host->download)
        return InvokeResult::error(WAB_CAPABILITY_DISABLED, "this tab cannot download files");
    std::shared_ptr<Deferred> deferred = ctx.defer();
    auto                      later    = std::make_shared<InvokeContext>(ctx); // the completion runs after this call returns
    auto                      got      = std::make_shared<std::map<std::string, std::string>>();
    auto                      todo     = std::make_shared<std::vector<Fetch>>(std::move(fetches));
    fetch_all(ctx.host, todo, 0, got, [deferred, later, got, type, values, keys](int code, const std::string& message) mutable {
        if (code != WAB_OK) {
            deferred->finish(InvokeResult::error(code, message + "; nothing was changed"));
            return;
        }
        for (const auto& [key, path] : *got) {
            values.set_key_value(key, new ConfigOptionString(path));
            if (std::find(keys.begin(), keys.end(), key) == keys.end())
                keys.push_back(key);
        }
        deferred->finish(write_values(*later, type, values, keys));
    });
    return InvokeResult::started();
}

// ---------------------------------------------------------------------------------------------
// bed.setShape
// ---------------------------------------------------------------------------------------------

// The printer tab's bed shape dialog (BedShapeDialog -> printable_area, bed_custom_texture,
// bed_custom_model): a rectangle with its origin, a circle, or a polygon; the texture and model
// come from URLs (the dialog's file pickers) and are kept in the slicer's data folder.
InvokeResult run_bed_set_shape(InvokeContext& ctx)
{
    const json&        p     = ctx.params;
    // No shape given: whichever of sizeMm / diameterMm / points is there (none: the shape stays).
    const std::string shape = p.contains("shape")      ? p["shape"].get<std::string>() :
                              p.contains("sizeMm")     ? "rectangle" :
                              p.contains("diameterMm") ? "circle" :
                              p.contains("points")     ? "custom" :
                                                         "";
    std::vector<Vec2d> points;
    if (shape == "rectangle") {
        if (!p.contains("sizeMm"))
            return InvokeResult::error(WAB_BAD_PARAMS, "a rectangle needs sizeMm [x, y]", {{"path", "/params/sizeMm"}, {"needs", "sizeMm"}});
        const double x = p["sizeMm"][0].get<double>(), y = p["sizeMm"][1].get<double>();
        const double dx = p.contains("originMm") ? p["originMm"][0].get<double>() : 0.;
        const double dy = p.contains("originMm") ? p["originMm"][1].get<double>() : 0.;
        // BedShapePanel::update_shape: the corners, shifted so the origin sits at (dx, dy) from the front left.
        points = {Vec2d(-dx, -dy), Vec2d(x - dx, -dy), Vec2d(x - dx, y - dy), Vec2d(-dx, y - dy)};
    } else if (shape == "circle") {
        if (!p.contains("diameterMm"))
            return InvokeResult::error(WAB_BAD_PARAMS, "a circle needs diameterMm", {{"path", "/params/diameterMm"}, {"needs", "diameterMm"}});
        // 72 edges, as the dialog draws it (BuildVolume detects a circular bed by them).
        const double r = p["diameterMm"].get<double>() / 2.;
        for (int i = 1; i <= 72; ++i)
            points.emplace_back(r * std::cos(i * 2. * PI / 72.), r * std::sin(i * 2. * PI / 72.));
    } else if (shape == "custom") {
        if (!p.contains("points"))
            return InvokeResult::error(WAB_BAD_PARAMS, "a custom bed needs points [[x, y], ...]", {{"path", "/params/points"}, {"needs", "points"}});
        for (const json& pt : p["points"])
            points.emplace_back(pt[0].get<double>(), pt[1].get<double>());
    }

    DynamicPrintConfig       values;
    std::vector<std::string> keys;
    if (!points.empty()) {
        values.set_key_value("printable_area", new ConfigOptionPoints(points));
        keys.push_back("printable_area");
    }
    if (p.contains("maxHeightMm")) {
        values.set_key_value("printable_height", new ConfigOptionFloat(p["maxHeightMm"].get<double>()));
        keys.push_back("printable_height");
    }
    for (const auto& [param, key] : {std::pair<const char*, const char*>{"clearTexture", "bed_custom_texture"},
                                     std::pair<const char*, const char*>{"clearModel", "bed_custom_model"}})
        if (p.value(param, false)) {
            values.set_key_value(key, new ConfigOptionString(""));
            keys.push_back(key);
        }
    std::vector<Fetch> fetches;
    if (p.contains("textureUrl"))
        fetches.push_back({"bed_custom_texture", p["textureUrl"].get<std::string>(), {"png", "svg"}, "bed"});
    if (p.contains("modelUrl"))
        fetches.push_back({"bed_custom_model", p["modelUrl"].get<std::string>(), {"stl", "svg"}, "bed"});
    if (keys.empty() && fetches.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "nothing to change: give a shape, maxHeightMm, a texture or a model", {{"path", "/params"}});
    return write_after_downloads(ctx, Preset::TYPE_PRINTER, std::move(values), std::move(keys), std::move(fetches));
}

// ---------------------------------------------------------------------------------------------
// settings.setFileFromUrl
// ---------------------------------------------------------------------------------------------

// The print tab's "Browse for ..." buttons next to the file-path settings: the file comes from a
// URL instead of an OS dialog, is kept in the slicer's data folder, and the setting points at it.
InvokeResult run_set_file_from_url(InvokeContext& ctx)
{
    const std::string key = ctx.params.value("key", "");
    std::vector<std::string> exts;
    if (key == "sparse_infill_svg_path")
        exts = {"svg"};
    else if (key == "infill_mesh_path")
        exts = {"stl", "obj", "3mf"};
    return write_after_downloads(ctx, Preset::TYPE_PRINT, DynamicPrintConfig(), {},
                                 {{key, ctx.params.value("url", ""), exts, "print"}});
}

// ---------------------------------------------------------------------------------------------
// preset.listBundles / preset.deleteBundle
// ---------------------------------------------------------------------------------------------

const char* bundle_type_name(BundleType t) { return t == Subscribed ? "subscribed" : t == Local ? "local" : "default"; }

json bundle_preset_names(const std::vector<std::string>& names)
{
    json out = json::array();
    for (const std::string& n : names)
        out.push_back(fs::path(n).filename().string());
    return out;
}

// The Preset Bundle dialog's list (PresetBundleDialog::ListBundles), without folder paths.
InvokeResult run_list_bundles(InvokeContext&)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the preset bundle is not ready");
    json list = json::array();
    bundle->bundles.ReadLock();
    for (const auto& [id, m] : bundle->bundles.m_bundles)
        list.push_back({{"id", m.id},
                        {"name", m.name},
                        {"type", bundle_type_name(m.bundle_type)},
                        {"author", m.author},
                        {"version", m.version},
                        {"description", m.description},
                        {"printers", bundle_preset_names(m.printer_presets)},
                        {"filaments", bundle_preset_names(m.filament_presets)},
                        {"processes", bundle_preset_names(m.print_presets)}});
    bundle->bundles.ReadUnlock();
    std::sort(list.begin(), list.end(), [](const json& a, const json& b) { return a["name"].get<std::string>() < b["name"].get<std::string>(); });
    return InvokeResult::ok({{"bundles", list}});
}

// The dialog's Delete (PresetBundleDialog::DeleteBundleById): the bundle's folder and every
// preset loaded from it. Imported (local) bundles only, and none of whose presets is in use.
InvokeResult run_delete_bundle(InvokeContext& ctx)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the preset bundle is not ready");
    const std::string id = ctx.params.value("id", "");
    BundleMetadata    meta;
    bool              found = false;
    bundle->bundles.ReadLock();
    if (auto it = bundle->bundles.m_bundles.find(id); it != bundle->bundles.m_bundles.end()) {
        meta  = it->second;
        found = true;
    }
    bundle->bundles.ReadUnlock();
    if (!found)
        return InvokeResult::error(WAB_NOT_FOUND, "no preset bundle has id \"" + id + "\"; preset.listBundles lists them", {{"id", id}});
    if (meta.bundle_type != Local)
        return InvokeResult::error(WAB_FORBIDDEN_ORIGIN, std::string("only imported (local) bundles can be deleted; this one is ") +
                                                             bundle_type_name(meta.bundle_type),
                                   {{"type", bundle_type_name(meta.bundle_type)}});

    std::vector<std::pair<std::string, std::string>> presets; // (type, name)
    json                                             in_use = json::array();
    for (const auto& [type_name, coll] : {std::pair<const char*, PresetCollection*>{"printer", &bundle->printers},
                                          std::pair<const char*, PresetCollection*>{"print", &bundle->prints},
                                          std::pair<const char*, PresetCollection*>{"filament", &bundle->filaments}})
        for (const Preset& p : coll->get_presets())
            if (p.bundle_id == id) {
                presets.emplace_back(type_name, p.name);
                const bool used = coll->get_edited_preset().name == p.name ||
                                  (coll == &bundle->filaments &&
                                   std::find(bundle->filament_presets.begin(), bundle->filament_presets.end(), p.name) != bundle->filament_presets.end());
                if (used)
                    in_use.push_back({{"type", type_name}, {"name", p.name}});
            }
    // The dialog deletes them from under the selection; here the person's selection moves first.
    if (!in_use.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "presets of this bundle are selected; select other presets first (data.inUse)",
                                   {{"inUse", in_use}, {"reason", "in_use"}});

    ConfirmRequest req;
    req.title   = "Delete preset bundle";
    req.message = "Delete the preset bundle \"" + meta.name + "\" from its folder, with the " + std::to_string(presets.size()) +
                  " presets loaded from it? This cannot be undone.";
    for (const auto& [type_name, name] : presets)
        req.details.push_back(std::string(type_name) + " preset: " + name);
    req.accept_label = "Delete";
    if (!ctx.confirm(req))
        return InvokeResult::error(WAB_USER_CANCELLED, "the deletion was declined", {{"id", id}});

    bundle->bundles.WriteLock();
    bundle->bundles.m_bundles.erase(id);
    bundle->bundles.WriteUnlock();
    for (PresetCollection* coll : std::initializer_list<PresetCollection*>{&bundle->prints, &bundle->filaments, &bundle->printers}) {
        std::vector<std::string> names;
        for (const Preset& p : coll->get_presets())
            if (p.bundle_id == id)
                names.push_back(p.name);
        for (const std::string& n : names)
            coll->delete_preset(n);
    }
    boost::system::error_code ec;
    if (const fs::path dir = fs::path(meta.path).parent_path(); !meta.path.empty() && fs::exists(dir, ec))
        fs::remove_all(dir, ec);
    bundle->update_compatible(PresetSelectCompatibleType::Always);
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->update_side_preset_ui();
    json deleted = json::array();
    for (const auto& [type_name, name] : presets)
        deleted.push_back({{"type", type_name}, {"name", name}});
    return InvokeResult::ok({{"id", id}, {"name", meta.name}, {"deleted", deleted}});
}

// ---------------------------------------------------------------------------------------------
// preset.createFilament / preset.createPrinter (CreatePresetsDialog without its dialogs)
// ---------------------------------------------------------------------------------------------

// The dialogs' remove_special_key + trim.
std::string clean_dialog_text(std::string text)
{
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '\n' || c == '\t' || c == '\r' || c == '\v' || c == '@' || c == ';'; }),
               text.end());
    boost::algorithm::trim(text);
    return text;
}

std::string first_string(const Preset& p, const char* key)
{
    const auto* v = p.config.option<ConfigOptionStrings>(key);
    return v != nullptr && !v->values.empty() ? v->values.front() : std::string();
}

// CreateFilamentPresetDialog's Create: one user filament "<vendor> <type> <serial>", cloned for
// each printer from the base filament's preset for that printer, all sharing a new filament_id,
// each limited to its printer.
InvokeResult run_create_filament(InvokeContext& ctx)
{
    TypeCtx t;
    if (!type_ctx(Preset::TYPE_FILAMENT, t))
        return InvokeResult::error(WAB_INTERNAL, "the preset bundle is not ready");
    TypeCtx printers;
    type_ctx(Preset::TYPE_PRINTER, printers);

    const std::string vendor = clean_dialog_text(ctx.params.value("vendor", ""));
    const std::string serial = clean_dialog_text(ctx.params.value("serial", ""));
    const std::string type   = clean_dialog_text(ctx.params.value("type", ""));
    if (vendor.empty() || serial.empty() || type.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "vendor, type and serial must hold more than spaces and the characters @ ; tab newline",
                                   {{"path", "/params/vendor"}});
    // The vendors of installed filaments are the dialog's list; anything else is a custom vendor,
    // held to the dialog's custom-vendor rules.
    std::set<std::string> known_vendors, known_types;
    for (const Preset& p : t.collection->get_presets())
        if (p.is_system) {
            known_vendors.insert(first_string(p, "filament_vendor"));
            known_types.insert(first_string(p, "filament_type"));
        }
    if (known_vendors.count(vendor) == 0) {
        if (vendor == "Bambu" || vendor == "Generic")
            return InvokeResult::error(WAB_BAD_PARAMS, "\"Bambu\" or \"Generic\" cannot be the vendor of a custom filament", {{"path", "/params/vendor"}});
        if (std::all_of(vendor.begin(), vendor.end(), [](unsigned char c) { return std::isdigit(c); }))
            return InvokeResult::error(WAB_BAD_PARAMS, "the vendor cannot be a number", {{"path", "/params/vendor"}});
    }
    if (const ConfigOptionDef* def = print_config_def.get("filament_type"); known_types.count(type) == 0 && def != nullptr &&
                                                                          std::find(def->enum_values.begin(), def->enum_values.end(), type) == def->enum_values.end())
        return InvokeResult::error(WAB_BAD_PARAMS, "\"" + type + "\" is not a filament type the slicer knows",
                                   {{"path", "/params/type"}, {"types", std::vector<std::string>(known_types.begin(), known_types.end())}});
    const std::string name = vendor + " " + (type == "PLA-AERO" ? "PLA Aero" : type) + " " + serial;
    if (InvokeResult bad = check_new_name(t, name, "/params/serial"); !bad.is_ok())
        return bad;

    const std::string base_name = resolve_name(t, ctx.params.value("base", ""), true);
    if (base_name.empty())
        return not_found(t, ctx.params.value("base", ""));
    const Preset* base = t.collection->find_preset(base_name, false);

    // For each printer, the base filament's own preset for it when there is one (same
    // filament_id, or same display name), system first; else the base preset itself.
    std::vector<std::pair<std::string, const Preset*>> plan; // (printer, template)
    json                                               exists = json::array();
    const bool                                         overwrite = ctx.params.value("overwrite", false);
    for (const json& pj : ctx.params["printers"]) {
        const std::string printer = resolve_name(printers, pj.get<std::string>(), true);
        if (printer.empty())
            return not_found(printers, pj.get<std::string>());
        if (std::any_of(plan.begin(), plan.end(), [&](const auto& e) { return e.first == printer; }))
            continue;
        const Preset&                 pp = *printers.collection->find_preset(printer, false);
        const PresetWithVendorProfile ppv(pp, pp.vendor);
        const Preset*                 tmpl = nullptr;
        for (const Preset& f : t.collection->get_presets()) {
            const bool same = (!base->filament_id.empty() && base->filament_id != "null" && f.filament_id == base->filament_id) ||
                              (!base->alias.empty() && f.alias == base->alias);
            if (!same || f.is_default || !is_compatible_with_printer(PresetWithVendorProfile(f, f.vendor), ppv))
                continue;
            if (tmpl == nullptr || (f.is_system && !tmpl->is_system))
                tmpl = &f;
        }
        plan.emplace_back(printer, tmpl != nullptr ? tmpl : base);
        const std::string full = name + " @" + printer;
        if (const Preset* e = t.collection->find_preset(full, false); e != nullptr) {
            if (!e->can_overwrite())
                return InvokeResult::error(WAB_FORBIDDEN_ORIGIN, "\"" + full + "\" is a system preset; choose another serial", {{"path", "/params/serial"}});
            if (!overwrite)
                exists.push_back(full);
        }
    }
    if (!exists.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "these presets exist already; pass overwrite:true to replace them, or choose another serial",
                                   {{"exists", exists}, {"path", "/params/serial"}});

    const bool        alias_taken = t.collection->is_alias_exist(name);
    const std::string filament_id = create_filament_id(name);
    json              created     = json::array();
    json              failed      = json::array();
    for (const auto& [printer, tmpl] : plan) {
        DynamicConfig dyn;
        dyn.set_key_value("filament_vendor", new ConfigOptionStrings({vendor}));
        dyn.set_key_value("compatible_printers", new ConfigOptionStrings({printer}));
        dyn.set_key_value("filament_type", new ConfigOptionStrings({type}));
        std::vector<std::string> failures;
        if (t.collection->clone_presets_for_filament(tmpl, failures, name, filament_id, dyn, printer, overwrite))
            created.push_back({{"name", name + " @" + printer}, {"printer", printer}, {"from", tmpl->name}});
        else
            failed.push_back({{"printer", printer}, {"presets", failures}});
    }
    t.bundle->update_compatible(PresetSelectCompatibleType::Always);
    refresh_after_preset_change(t);
    json result = {{"name", name}, {"filamentId", filament_id}, {"created", created}};
    if (!failed.empty())
        result["failed"] = failed;
    if (alias_taken)
        result["note"] = "another filament already shows this name; the new presets are listed with their full names";
    if (created.empty())
        return InvokeResult::error(WAB_IO, "no preset was created", result);
    return InvokeResult::ok(result);
}

// CreatePrinterPresetDialog::generate_process_presets_data: a process scaled to the nozzle.
void scale_process_to_nozzle(DynamicPrintConfig& config, double nozzle)
{
    auto set_float = [&config](const char* key, double v) {
        if (auto* o = config.option<ConfigOptionFloat>(key); o != nullptr)
            o->value = v;
    };
    auto set_int = [&config](const char* key, int v) {
        if (auto* o = config.option<ConfigOptionInt>(key); o != nullptr)
            o->value = v;
    };
    set_float("layer_height", nozzle / 2);
    set_float("initial_layer_print_height", nozzle / 2);
    for (const char* k : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width", "top_surface_line_width",
                          "sparse_infill_line_width", "internal_solid_infill_line_width", "support_line_width"})
        set_float(k, nozzle);
    set_int("wall_loops", std::max(2, int(std::ceil(2 * 0.4 / nozzle))));
    set_int("top_shell_layers", std::max(5, int(std::ceil(5 * 0.4 / nozzle))));
    set_int("bottom_shell_layers", std::max(3, int(std::ceil(3 * 0.4 / nozzle))));
}

// CreatePrinterPresetDialog's Create: a printer from a template printer -- a new vendor and model
// (with its bed), or another nozzle of the template's model -- and copies of the chosen process
// and filament presets limited to it (compatible_printers set here, never by the caller).
InvokeResult run_create_printer(InvokeContext& ctx)
{
    TypeCtx      t, prints, filaments;
    InvokeResult err;
    if (!type_ctx(Preset::TYPE_PRINTER, t) || !type_ctx(Preset::TYPE_PRINT, prints) || !type_ctx(Preset::TYPE_FILAMENT, filaments))
        return InvokeResult::error(WAB_INTERNAL, "the preset bundle is not ready");
    const json& p        = ctx.params;
    const bool  custom   = p.contains("vendor") || p.contains("model");
    const std::string tmpl_name = resolve_name(t, p.value("printer", ""), true);
    if (tmpl_name.empty())
        return not_found(t, p.value("printer", ""));
    Preset printer = *t.collection->find_preset(tmpl_name, false); // a copy: the dialog's m_printer_preset

    std::string model_name;
    if (custom) {
        const std::string vendor = clean_dialog_text(p.value("vendor", "")), model = clean_dialog_text(p.value("model", ""));
        if (vendor.empty() || model.empty())
            return InvokeResult::error(WAB_BAD_PARAMS, "a new printer needs both vendor and model", {{"path", "/params/model"}});
        model_name = vendor + " " + model;
    } else {
        model_name = printer.config.opt_string("printer_model");
        if (model_name.empty())
            return InvokeResult::error(WAB_BAD_PARAMS, "the template printer has no model; give vendor and model for a new printer",
                                       {{"path", "/params/model"}});
    }
    std::string nozzle = p.value("nozzle", "0.4");
    std::replace(nozzle.begin(), nozzle.end(), ',', '.');
    double nozzle_mm = 0.;
    try {
        nozzle_mm = std::stod(nozzle);
    } catch (...) {
    }
    if (!(nozzle_mm > 0.05 && nozzle_mm < 3.))
        return InvokeResult::error(WAB_BAD_PARAMS, "the nozzle diameter is not valid", {{"path", "/params/nozzle"}});
    const std::string name = model_name + " " + nozzle + " nozzle";
    if (InvokeResult bad = check_new_name(t, name, "/params/model"); !bad.is_ok())
        return bad;
    const bool overwrite = p.value("overwrite", false);
    if (const Preset* e = t.collection->find_preset(name, false); e != nullptr) {
        if (e->is_system)
            return InvokeResult::error(WAB_FORBIDDEN_ORIGIN, "\"" + name + "\" is a printer the slicer ships; select it instead (printer.add)",
                                       {{"exists", true}, {"isSystem", true}});
        if (!overwrite)
            return InvokeResult::error(WAB_BAD_PARAMS, "a printer preset named \"" + name + "\" exists; pass overwrite:true to replace it",
                                       {{"exists", true}, {"name", name}});
    }

    // The templates for the new printer's process and filament presets.
    auto pick = [&](TypeCtx& tc, const char* param, std::vector<Preset>& out, json& exists) -> InvokeResult {
        for (const json& n : p[param]) {
            const std::string r = resolve_name(tc, n.get<std::string>(), true);
            if (r.empty())
                return not_found(tc, n.get<std::string>());
            out.push_back(*tc.collection->find_preset(r, false));
            std::string prefix = r.substr(0, r.find(" @"));
            std::replace(prefix.begin(), prefix.end(), '/', '-');
            if (tc.collection->find_preset(prefix + " @" + name, false) != nullptr && !overwrite)
                exists.push_back(prefix + " @" + name);
        }
        return InvokeResult::ok();
    };
    std::vector<Preset> print_tmpls, filament_tmpls;
    json                exists = json::array();
    if (InvokeResult r = pick(prints, "prints", print_tmpls, exists); !r.is_ok())
        return r;
    if (InvokeResult r = pick(filaments, "filaments", filament_tmpls, exists); !r.is_ok())
        return r;
    if (!exists.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "presets for this printer exist already; pass overwrite:true to recreate them",
                                   {{"exists", exists}});
    if (p.value("scaleToNozzle", false))
        for (Preset& pr : print_tmpls)
            scale_process_to_nozzle(pr.config, nozzle_mm);

    // The printer: save_printable_area_config (a new printer's bed), then model, variant, nozzle.
    DynamicPrintConfig& cfg = printer.config;
    if (custom) {
        if (p.contains("bedSizeMm")) {
            const double x = p["bedSizeMm"][0].get<double>(), y = p["bedSizeMm"][1].get<double>();
            const double dx = p.contains("bedOriginMm") ? p["bedOriginMm"][0].get<double>() : 0.;
            const double dy = p.contains("bedOriginMm") ? p["bedOriginMm"][1].get<double>() : 0.;
            if (dx >= x || dy >= y)
                return InvokeResult::error(WAB_BAD_PARAMS, "the bed origin must lie inside the bed", {{"path", "/params/bedOriginMm"}});
            cfg.set_key_value("printable_area", new ConfigOptionPoints({Vec2d(-dx, -dy), Vec2d(x - dx, -dy), Vec2d(x - dx, y - dy), Vec2d(-dx, y - dy)}));
        }
        if (p.contains("maxHeightMm"))
            cfg.set("printable_height", p["maxHeightMm"].get<double>());
        // The template's custom bed files are the template's, not this printer's.
        cfg.set("bed_custom_model", std::string());
        cfg.set("bed_custom_texture", std::string());
    }
    cfg.option<ConfigOptionString>("printer_model", true)->value   = model_name;
    cfg.option<ConfigOptionString>("printer_variant", true)->value = nozzle;
    if (auto* nd = cfg.option<ConfigOptionFloats>("nozzle_diameter"); nd != nullptr)
        std::fill(nd->values.begin(), nd->values.end(), nozzle_mm);

    // Saving the printer selects it, outside the tab (as the dialog does): any unsaved edits are
    // under ifDirty first, and nobody can be asked on the way.
    if (ctx.if_dirty == "prompt")
        for (Preset::Type type : preset_switch_scope(Preset::TYPE_PRINTER))
            if (TypeCtx d; type_ctx(type, d) && d.collection->current_is_dirty())
                return InvokeResult::error(WAB_NEEDS_CONFIRMATION,
                                           std::string("creating a printer selects it without a question, and the ") + d.name +
                                               " preset has unsaved changes; use ifDirty \"save\" or \"discard\"",
                                           {{"dirty", true}, {"type", d.name}, {"dirtyKeys", d.collection->current_dirty_options()}});
    if (!ctx.preset_switch_preflight(Preset::TYPE_PRINTER, name, err))
        return err;

    // Filaments, processes, then the printer, in the dialog's order.
    auto clone = [&](TypeCtx& tc, const std::vector<Preset>& tmpls, json& made) -> std::vector<std::string> {
        std::vector<const Preset*> ptrs;
        for (const Preset& pr : tmpls)
            ptrs.push_back(&pr);
        std::vector<std::string> failures;
        if (!ptrs.empty() && tc.collection->clone_presets_for_printer(ptrs, failures, name, create_filament_id, overwrite))
            for (const Preset& pr : tmpls) {
                std::string prefix = pr.name.substr(0, pr.name.find(" @"));
                std::replace(prefix.begin(), prefix.end(), '/', '-');
                made.push_back(prefix + " @" + name);
            }
        return failures;
    };
    json made_filaments = json::array(), made_prints = json::array();
    const auto f_fail = clone(filaments, filament_tmpls, made_filaments);
    const auto p_fail = clone(prints, print_tmpls, made_prints);
    t.collection->save_current_preset(name, true /*detach*/, false, &printer);
    t.bundle->update_compatible(PresetSelectCompatibleType::Always);
    if (t.collection->find_preset(name, false) == nullptr)
        return InvokeResult::error(WAB_IO, "the printer preset could not be saved");
    // What Sidebar::create_printer_preset does once the dialog closes.
    wxGetApp().load_current_presets();
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->update_side_preset_ui();

    json result = {{"name", name},
                   {"model", model_name},
                   {"nozzle", nozzle},
                   {"filaments", made_filaments},
                   {"prints", made_prints},
                   {"selected", t.collection->get_selected_preset_name()}};
    if (!f_fail.empty() || !p_fail.empty())
        result["failed"] = {{"filaments", f_fail}, {"prints", p_fail}};
    return InvokeResult::ok(result);
}

std::string enabled_when_ready(const InvokeContext&)
{
    return wxGetApp().preset_bundle != nullptr && wxGetApp().plater() != nullptr ? std::string() : std::string("the slicer is starting");
}

ActionDescriptor make(const char* id, const char* label, const char* summary, Risk risk, json params, std::vector<std::string> effects,
                      std::vector<std::string> ui, json examples)
{
    ActionDescriptor d;
    d.id       = id;
    d.label    = label;
    d.summary  = summary;
    d.risk     = risk;
    d.params   = std::move(params);
    d.effects  = std::move(effects);
    d.ui       = std::move(ui);
    d.examples = std::move(examples);
    return d;
}

json one_example(json params) { return json::array({{{"target", nullptr}, {"params", std::move(params)}}}); }

} // namespace

void register_preset_actions(Catalog& c)
{
    c.add(make("preset.save", "Save preset",
               "Save the settings tab's preset, with its unsaved edits, as a user preset under a name and select it. Overwriting another "
               "saved user preset asks the person natively.",
               Risk::Mutate,
               object_schema({{"type", TYPE_SCHEMA},
                              {"name", name_schema("The new preset's name.")},
                              {"from", name_schema("Save this installed preset instead of the tab's current one (a preset switch, under ifDirty).")},
                              {"overwrite", {{"type", "boolean"}, {"default", false}, {"description", "Replace an existing user preset of that name."}}},
                              {"detach", {{"type", "boolean"}, {"default", false}, {"description", "Save without a parent preset (Detach)."}}},
                              {"toProject", {{"type", "boolean"}, {"description", "Keep the preset in the project file, not the preset folder. Default false; "
                                                                             "an overwrite stays where the existing preset lives."}}}},
                             {"type", "name"}),
               {"presets", "settings"}, {"Tab.cpp::tabbutton::m_btn_save_preset", "Tab.cpp::control::detach from parent", "SavePresetDialog.cpp::control::preset name", "SavePresetDialog.cpp::control::user preset / project", "SavePresetDialog.cpp::control::detach from parent"},
               one_example({{"type", "print"}, {"name", "Automated test process"}})),
          run_save, enabled_when_ready);

    c.add(make("preset.rename", "Rename preset", "Rename one of the person's own presets; the selection and filament slots follow, and a renamed printer stays the "
               "printer of the filament and process presets limited to it. Presets synced to a cloud account are refused (the person "
               "renames those).",
               Risk::Mutate,
               object_schema({{"type", TYPE_SCHEMA}, {"name", name_schema("The preset to rename.")}, {"newName", name_schema("Its new name.")}},
                             {"type", "name", "newName"}),
               {"presets"}, {}, one_example({{"type", "print"}, {"name", "Automated test process"}, {"newName", "Automated test process 2"}})),
          run_rename, enabled_when_ready);

    c.add(make("preset.delete", "Delete preset",
               "Delete one of the person's own presets and its file, after a native confirmation. A printer the person created takes "
               "its filament and process presets with it: that needs cascade:true, and the confirmation lists them. Presets synced to a "
               "cloud account are refused.",
               Risk::Destructive,
               object_schema({{"type", TYPE_SCHEMA},
                              {"name", name_schema("The preset to delete.")},
                              {"cascade", {{"type", "boolean"}, {"default", false},
                                           {"description", "A created printer: also delete the filament and process presets made for it."}}}},
                             {"type", "name"}),
               {"presets", "settings"}, {"Tab.cpp::tabbutton::m_btn_delete_preset"},
               one_example({{"type", "print"}, {"name", "Automated test process 2"}})),
          run_delete, enabled_when_ready);

    c.add(make("preset.diff", "Compare presets",
               "List the settings that differ between the tab's preset (with its unsaved edits) and its saved state or the preset it is "
               "based on, or between two named presets.",
               Risk::Read,
               object_schema({{"type", TYPE_SCHEMA},
                              {"a", name_schema("First side, an installed preset as saved. Default: the tab's preset with unsaved edits.")},
                              {"b", name_schema("Second side, an installed preset. Overrides against.")},
                              {"against", {{"type", "string"}, {"enum", {"saved", "parent", "edited"}}, {"default", "saved"},
                                           {"description", "Without b: saved = the preset as saved (lists unsaved edits); parent = the preset "
                                                           "it is based on; edited = the tab's preset with its unsaved edits (what a named a "
                                                           "is compared with when against is saved)."}}},
                              {"keys", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 200}, {"description", "Compare only these keys."}}},
                              {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"default", 200}}}},
                             {"type"}),
               {}, {"ParamsPanel.cpp::control::compare presets", "UnsavedChangesDialog.cpp::control::compare: show all presets"}, one_example({{"type", "print"}, {"against", "parent"}})),
          run_diff, enabled_when_ready);

    c.add(make("preset.revertKeys", "Revert settings",
               "Put settings of the tab's preset back to their saved values, or to the preset it is based on (the tab's undo arrows). "
               "\"key#i\" reverts one extruder or variant value; custom G-code and compatibility lists can be reverted too. An unsaved "
               "edit, undone with undoTurn.",
               Risk::Mutate,
               object_schema({{"type", TYPE_SCHEMA},
                              {"keys", {{"type", "array"}, {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 120}}}, {"minItems", 1}, {"maxItems", 200},
                                        {"description", "Setting keys; \"key#i\" for one value of a per-extruder or per-variant setting."}}},
                              {"to", {{"type", "string"}, {"enum", {"saved", "parent"}}, {"default", "saved"}}}},
                             {"type", "keys"}),
               {"settings"}, {"Tab.cpp::tabbutton::m_undo_btn"}, one_example({{"type", "print"}, {"keys", {"wall_loops"}}, {"to", "parent"}})),
          run_revert_keys, enabled_when_ready);

    {
        ActionDescriptor d = make("preset.discardEdits", "Discard changes",
                                  "Drop every unsaved edit of the tab's preset. Asks natively unless every edit was made by this same turn.",
                                  Risk::Destructive, object_schema({{"type", TYPE_SCHEMA}}, {"type"}), {"settings"}, {},
                                  one_example({{"type", "print"}}));
        d.confirm = Confirm::Native;
        c.add(std::move(d), run_discard_edits, enabled_when_ready);
    }

    c.add(make("preset.setVisible", "Show or hide preset",
               "Show or hide an installed system preset in the preset lists (the setup wizard's printer and filament ticks). Process "
               "presets have no tick of their own: their visibility lasts for this session only.", Risk::Mutate,
               object_schema({{"type", TYPE_SCHEMA},
                              {"name", name_schema("The system preset, by name or display name.")},
                              {"visible", {{"type", "boolean"}}}},
                             {"type", "name", "visible"}),
               {"presets"}, {"PresetComboBoxes.cpp::menu::Add/Remove presets"},
               one_example({{"type", "filament"}, {"name", "Generic PETG"}, {"visible", true}})),
          run_set_visible, enabled_when_ready);

    c.add(make("preset.createFromTemplate", "Create preset",
               "Create a user preset from an installed or hidden system template with some values changed, save it under a name and select "
               "it; a filament goes into one slot only.",
               Risk::Mutate,
               object_schema({{"type", TYPE_SCHEMA},
                              {"base", name_schema("The template preset, by name or display name; hidden system presets allowed.")},
                              {"name", name_schema("The new preset's name.")},
                              {"values", {{"type", "object"}, {"additionalProperties", false}, {"properties", json::object()},
                                          {"description", "Setting key -> value, as slicer.setSettings takes them. Keys are checked by the slicer."}}},
                              {"slot", {{"type", "integer"}, {"minimum", 0}, {"maximum", 63}, {"default", 0}, {"description", "Filament: the slot that takes the new preset."}}}},
                             {"type", "base", "name"}),
               {"presets", "settings"}, {}, one_example({{"type", "print"}, {"base", "0.20mm Standard @BBL X1C"}, {"name", "Automated test template"}})),
          run_create_from_template, enabled_when_ready);

    {
        ActionDescriptor d = make("preset.exportBundle", "Export presets",
                                  "Write some of the person's own presets into a .zip, or an .orca_bundle with a name, author, version and description, "
                                  "saved where they choose in the native Save dialog. Print-host addresses, credentials and local bed-model paths "
                                  "are left out.",
                                  Risk::OutwardFile,
                                  object_schema({{"presets", {{"type", "array"}, {"minItems", 1}, {"maxItems", 200},
                                                              {"items", object_schema({{"type", TYPE_SCHEMA}, {"name", name_schema("A user or project preset.")}}, {"type", "name"})}}},
                                                 {"fileName", {{"type", "string"}, {"minLength", 1}, {"maxLength", 120}, {"description", "Suggested file name."}}},
                                                 {"format", {{"type", "string"}, {"enum", {"zip", "bundle"}}, {"default", "zip"},
                                                             {"description", "zip: plain archive of preset files; bundle: an .orca_bundle that imports as one named bundle."}}},
                                                 {"metadata", object_schema({{"name", {{"type", "string"}, {"maxLength", 120}}},
                                                                             {"author", {{"type", "string"}, {"maxLength", 120}}},
                                                                             {"version", {{"type", "string"}, {"maxLength", 40}}},
                                                                             {"description", {{"type", "string"}, {"maxLength", 1000}}}})}},
                                                {"presets"}),
                                  {"file"}, {"MainFrame.cpp::menu::Export Preset Bundle", "PresetBundleDialog.cpp::control::export bundle", "ExportPresetBundleDialog.cpp::control::export bundle", "CreatePresetsDialog.cpp::control::export configs"},
                                  one_example({{"presets", json::array({{{"type", "print"}, {"name", "Automated test process"}}})}}));
        d.gate              = Gate::Export;
        d.confirm_skip_rule = "the native Save dialog is the consent: the person picks the file";
        c.add(std::move(d), run_export_bundle, enabled_when_ready);
    }

    for (const bool bundle_only : {true, false}) {
        ActionDescriptor d = make(bundle_only ? "preset.importBundle" : "preset.importConfig", bundle_only ? "Import preset bundle" : "Import configs",
                                  bundle_only ? "Download a preset bundle (.zip, .orca_bundle, .orca_printer, .orca_filament) and import its "
                                                "presets after a native confirmation. Print hosts, credentials and post-processing scripts are removed; custom G-code is "
                                                "listed in the confirmation."
                                              : "Download a preset file (.json, or any bundle form) and import it after a native confirmation. Print hosts, "
                                                "credentials and post-processing scripts are removed; custom G-code is listed in the confirmation.",
                                  Risk::OutwardNetwork,
                                  object_schema({{"url", {{"type", "string"}, {"minLength", 8}, {"maxLength", 2048},
                                                          {"description", "http(s) URL on the page's host or the tab's download hosts."}}},
                                                 {"overwrite", {{"type", "boolean"}, {"default", false}, {"description", "Replace user presets of the same name."}}},
                                                 {"installVendors", {{"type", "boolean"}, {"default", false},
                                                                     {"description", "Install the shipped printer profiles the presets are based on when they "
                                                                                     "are missing (listed in the confirmation; reloads every preset, so ifDirty applies)."}}}},
                                                {"url"}),
                                  {"presets"},
                                  bundle_only ? std::vector<std::string>{} : std::vector<std::string>{"MainFrame.cpp::menu::Import Configs"},
                                  one_example({{"url", "http://127.0.0.1:8799/fixtures/s7_presets.zip"}}));
        d.gate         = Gate::Install;
        d.long_running = true;
        c.add(std::move(d), import_runner(bundle_only), enabled_when_ready);
    }

    c.add(make("preset.setCompatibility", "Set compatible printers",
               "Set which printers (and, for a filament, which processes) the tab's process or filament preset suits: the Profile "
               "dependencies lists. An empty list means all. An unsaved edit; name switches the tab to that preset first (under ifDirty).",
               Risk::Mutate,
               object_schema({{"type", {{"type", "string"}, {"enum", {"print", "filament"}}}},
                              {"name", name_schema("The preset to edit (default: the tab's).")},
                              {"printers", {{"type", "array"}, {"maxItems", 500}, {"items", name_schema("A printer preset.")},
                                            {"description", "Printer presets it suits; [] = all printers."}}},
                              {"prints", {{"type", "array"}, {"maxItems", 500}, {"items", name_schema("A process preset.")},
                                          {"description", "Filament only: process presets it suits; [] = all."}}}},
                             {"type"}),
               {"settings", "presets"}, {"Tab.cpp::control::compatible printers/prints"},
               one_example({{"type", "print"}, {"printers", json::array({"Bambu Lab X1 Carbon 0.4 nozzle"})}})),
          run_set_compatibility, enabled_when_ready);

    c.add(make("preset.transfer", "Transfer settings",
               "Copy settings from one preset into another as unsaved edits (the compare dialog's Transfer): the tab switches to `to` "
               "(under ifDirty) and takes `keys` from `from` as saved. \"key#i\" moves one extruder value.",
               Risk::Mutate,
               object_schema({{"type", TYPE_SCHEMA},
                              {"from", name_schema("The preset the values come from (left side).")},
                              {"to", name_schema("The preset that takes them (right side).")},
                              {"keys", {{"type", "array"}, {"minItems", 1}, {"maxItems", 200},
                                        {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 120}}}}}},
                             {"type", "from", "to", "keys"}),
               {"settings", "presets"}, {"UnsavedChangesDialog.cpp::control::compare: transfer", "UnsavedChangesDialog.cpp::control::compare: transfer values"},
               one_example({{"type", "print"}, {"from", "0.20mm Standard @BBL X1C"}, {"to", "0.16mm Optimal @BBL X1C"}, {"keys", json::array({"wall_loops"})}})),
          run_transfer, enabled_when_ready);

    c.add(make("settings.copyExtruder", "Copy to other extruder",
               "Copy one extruder's per-extruder values over another's in the tab's preset, as unsaved edits (the tab's copy button). "
               "Without keys, the edited ones.",
               Risk::Mutate,
               object_schema({{"type", TYPE_SCHEMA},
                              {"fromExtruder", {{"type", "integer"}, {"minimum", 0}, {"maximum", 15}, {"default", 0}}},
                              {"toExtruder", {{"type", "integer"}, {"minimum", 0}, {"maximum", 15}, {"default", 1}}},
                              {"keys", {{"type", "array"}, {"maxItems", 200}, {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 120}}},
                                        {"description", "Per-extruder settings to copy (getSettingsSchema perVariant)."}}}},
                             {"type"}),
               {"settings"}, {"Tab.cpp::control::copy to other extruder"}, one_example({{"type", "print"}, {"fromExtruder", 0}, {"toExtruder", 1}})),
          run_copy_extruder, enabled_when_ready);

    c.add(make("printer.setExtruderCount", "Set extruder count",
               "Set how many extruders the printer preset has (the printer tab's Extruders field), resizing per-extruder settings and "
               "filament slots. An unsaved edit. Not for printers whose extruders are fixed by the machine.",
               Risk::Mutate, object_schema({{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}}}, {"count"}),
               {"settings", "presets"}, {"Tab.cpp::control::extruders count"}, one_example({{"count", 2}})),
          run_set_extruder_count, enabled_when_ready);

    {
        const json pair = {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}, {"minimum", -10000}, {"maximum", 10000}}}};
        const json url  = {{"type", "string"}, {"minLength", 8}, {"maxLength", 2048}, {"description", "http(s) URL on the page's host or the tab's download hosts."}};
        json       size = pair;
        size["items"]["minimum"] = 1;
        ActionDescriptor d = make(
            "bed.setShape", "Set bed shape",
            "Set the printer preset's bed (the printer tab's bed shape dialog): a rectangle (size and origin), a circle (diameter) or "
            "a polygon, its height, and a custom bed texture or model downloaded from a URL. Unsaved edits of the printer preset.",
            Risk::Mutate,
            object_schema({{"shape", {{"type", "string"}, {"enum", {"rectangle", "circle", "custom"}},
                                      {"description", "Default: from sizeMm (rectangle), diameterMm (circle) or points (custom)."}}},
                           {"sizeMm", size},
                           {"originMm", pair},
                           {"diameterMm", {{"type", "number"}, {"minimum", 1}, {"maximum", 10000}}},
                           {"points", {{"type", "array"}, {"minItems", 3}, {"maxItems", 1000}, {"items", pair}}},
                           {"maxHeightMm", {{"type", "number"}, {"minimum", 0}, {"maximum", 10000}}},
                           {"textureUrl", url},
                           {"modelUrl", url},
                           {"clearTexture", {{"type", "boolean"}}},
                           {"clearModel", {{"type", "boolean"}}}}),
            {"settings"}, {"Tab.cpp::control::bed shape", "BedShapeDialog.cpp::control::shape (rectangle/circle/custom)", "BedShapeDialog.cpp::control::load texture", "BedShapeDialog.cpp::control::load model"}, one_example({{"shape", "rectangle"}, {"sizeMm", {256, 256}}, {"originMm", {0, 0}}}));
        d.long_running = true;
        c.add(std::move(d), run_bed_set_shape, enabled_when_ready);
    }

    {
        ActionDescriptor d = make("settings.setFileFromUrl", "Set file setting from URL",
                                  "Point a print setting that names a file (infill SVG pattern, infill mesh) at a file downloaded from a URL "
                                  "(the print tab's Browse buttons). An unsaved edit of the process preset.",
                                  Risk::Mutate,
                                  object_schema({{"key", {{"type", "string"}, {"enum", {"sparse_infill_svg_path", "infill_mesh_path"}}}},
                                                 {"url", {{"type", "string"}, {"minLength", 8}, {"maxLength", 2048},
                                                          {"description", "http(s) URL on the page's host or the tab's download hosts."}}}},
                                                {"key", "url"}),
                                  {"settings"}, {"Tab.cpp::control::browse sparse_infill_svg_path", "Tab.cpp::control::browse infill_mesh_path"}, one_example({{"key", "sparse_infill_svg_path"}, {"url", "http://127.0.0.1:8799/fixtures/pattern.svg"}}));
        d.long_running = true;
        c.add(std::move(d), run_set_file_from_url, enabled_when_ready);
    }

    c.add(make("preset.listBundles", "List preset bundles",
               "The preset bundles the slicer holds (imported, default): id, name, type, author, version and the presets in each.",
               Risk::Read, object_schema(json::object()), {}, {"PresetBundleDialog.cpp::control::list bundles"}, one_example(json::object())),
          run_list_bundles, enabled_when_ready);

    c.add(make("preset.deleteBundle", "Delete preset bundle",
               "Delete an imported preset bundle and every preset loaded from it, after a native confirmation. None of its presets "
               "may be selected.",
               Risk::Destructive, object_schema({{"id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 300}}}}, {"id"}),
               {"presets"}, {"PresetBundleDialog.cpp::control::delete bundle"}, one_example({{"id", "an-imported-bundle-id"}})),
          run_delete_bundle, enabled_when_ready);

    c.add(make("preset.createFilament", "Create filament",
               "Create a user filament \"<vendor> <type> <serial>\" for one or more printers (the Create Filament dialog): each copy "
               "starts from the base filament's preset for that printer, is limited to it and shares one new filament id.",
               Risk::Mutate,
               object_schema({{"vendor", {{"type", "string"}, {"minLength", 1}, {"maxLength", 60}}},
                              {"type", {{"type", "string"}, {"minLength", 1}, {"maxLength", 40}, {"description", "Filament type, e.g. PLA, PETG."}}},
                              {"serial", {{"type", "string"}, {"minLength", 1}, {"maxLength", 60}, {"description", "The product line, e.g. \"Matte\"."}}},
                              {"base", name_schema("The filament it starts from, by name or display name; hidden system presets allowed.")},
                              {"printers", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", name_schema("A printer preset.")}}},
                              {"overwrite", {{"type", "boolean"}, {"default", false}}}},
                             {"vendor", "type", "serial", "base", "printers"}),
               {"presets"}, {"CreatePresetsDialog.cpp::control::create filament", "CreatePresetsDialog.cpp::control::edit filament: add for printer"},
               one_example({{"vendor", "Automated"}, {"type", "PLA"}, {"serial", "Test"}, {"base", "Generic PLA"},
                            {"printers", json::array({"Bambu Lab X1 Carbon 0.4 nozzle"})}})),
          run_create_filament, enabled_when_ready);

    {
        const json pair = {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 10000}}}};
        c.add(make("preset.createPrinter", "Create printer",
                   "Create a printer preset from a template printer (the Create Printer dialog): a new vendor and model with its bed, or "
                   "another nozzle of the template's model; with copies of the chosen process and filament presets limited to it. "
                   "It is selected, as the dialog does, so unsaved edits are under ifDirty (save or discard).",
                   Risk::Mutate,
                   object_schema({{"printer", name_schema("The template printer preset; hidden system presets allowed.")},
                                  {"vendor", {{"type", "string"}, {"minLength", 1}, {"maxLength", 60}, {"description", "A new printer: its vendor."}}},
                                  {"model", {{"type", "string"}, {"minLength", 1}, {"maxLength", 80}, {"description", "A new printer: its model."}}},
                                  {"nozzle", {{"type", "string"}, {"minLength", 1}, {"maxLength", 8}, {"default", "0.4"}, {"description", "Nozzle diameter, mm."}}},
                                  {"bedSizeMm", pair},
                                  {"bedOriginMm", pair},
                                  {"maxHeightMm", {{"type", "number"}, {"minimum", 1}, {"maximum", 10000}}},
                                  {"prints", {{"type", "array"}, {"minItems", 1}, {"maxItems", 100}, {"items", name_schema("A process preset to copy.")}}},
                                  {"filaments", {{"type", "array"}, {"minItems", 1}, {"maxItems", 200}, {"items", name_schema("A filament preset to copy.")}}},
                                  {"scaleToNozzle", {{"type", "boolean"}, {"default", false},
                                                     {"description", "Set the copied processes' layer heights, line widths and shells for the nozzle."}}},
                                  {"overwrite", {{"type", "boolean"}, {"default", false}}}},
                                 {"printer", "prints", "filaments"}),
                   {"presets", "settings"}, {"CreatePresetsDialog.cpp::control::create printer"},
                   one_example({{"printer", "Bambu Lab X1 Carbon 0.4 nozzle"}, {"vendor", "Automated"}, {"model", "Test Printer"},
                                {"nozzle", "0.4"}, {"bedSizeMm", {220, 220}}, {"maxHeightMm", 250},
                                {"prints", json::array({"0.20mm Standard @BBL X1C"})}, {"filaments", json::array({"Generic PLA @BBL X1C"})}})),
              run_create_printer, enabled_when_ready);
    }

    c.add(make("printer.listPhysical", "List physical printers",
               "The physical printers (print hosts) set up in the slicer: names and the printer presets they use. No addresses or credentials.",
               Risk::Read, object_schema(json::object()), {}, {}, one_example(json::object())),
          run_list_physical, enabled_when_ready);
}

}}} // namespace Slic3r::GUI::Actions
