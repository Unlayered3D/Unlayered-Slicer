// Automation actions: filament.* actions, printer.setNozzle, bed.setType.
// Owner: work package S8 (filament/UI). See docs/HLSD/bridge-actions.md for how to write an action
// and docs/bridge-actions/filament-ui.md for these.
//
// Each runner does what the sidebar control does, minus its dialog: the dialog's answer is a param.
// Slots are 0-based config indices, as in slicer.selectPreset / slicer.addFilamentSlot (physical
// slots first, mixed slots after them).

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "FilamentUiRules.hpp"
#include "Register.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <sstream>

#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "../ColorDecomposeSupport.hpp"
#include "../GLCanvas3D.hpp"
#include "../GUI_App.hpp"
#include "../MixedFilamentDialog.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../PresetComboBoxes.hpp"
#include "../Tab.hpp"
#include "../WebAppBridge.hpp"
#include "../WipeTowerDialog.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using nlohmann::json;

// ---- pure rules (FilamentUiRules.hpp) --------------------------------------------------------

bool normalize_hex_colour(const std::string& in, std::string& out)
{
    std::string s = in;
    if (!s.empty() && s.front() == '#')
        s.erase(0, 1);
    if (s.size() != 3 && s.size() != 6)
        return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    if (s.size() == 3)
        s = std::string{s[0], s[0], s[1], s[1], s[2], s[2]};
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    out = "#" + s;
    return true;
}

std::string nozzle_diameter_string(double diameter)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << diameter;
    std::string s = stream.str();
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.')
            s += '0';
    }
    return s;
}

bool apply_flush_sub_matrix(std::vector<double>& raw, size_t nozzles, size_t nozzle, size_t full_n,
                            const std::vector<size_t>& physical, const std::vector<std::vector<double>>& sub,
                            std::string& message)
{
    if (nozzles == 0 || nozzle >= nozzles) {
        message = "nozzle " + std::to_string(nozzle) + " out of range (" + std::to_string(nozzles) + " nozzles)";
        return false;
    }
    const size_t p = physical.size();
    if (sub.size() != p) {
        message = "matrix must have " + std::to_string(p) + " rows (one per physical filament), got " + std::to_string(sub.size());
        return false;
    }
    for (size_t r = 0; r < p; ++r) {
        if (sub[r].size() != p) {
            message = "matrix row " + std::to_string(r) + " must have " + std::to_string(p) + " values";
            return false;
        }
        for (size_t c = 0; c < p; ++c)
            if (!std::isfinite(sub[r][c]) || sub[r][c] < 0.) {
                message = "matrix values must be finite and >= 0 (row " + std::to_string(r) + ", column " + std::to_string(c) + ")";
                return false;
            }
    }
    for (size_t i : physical)
        if (i >= full_n) {
            message = "physical slot index out of range";
            return false;
        }
    const size_t block = full_n * full_n;
    if (raw.size() < nozzles * block)
        raw.resize(nozzles * block, 0.);
    const size_t offset = nozzle * block;
    for (size_t r = 0; r < p; ++r)
        for (size_t c = 0; c < p; ++c)
            raw[offset + physical[r] * full_n + physical[c]] = (r == c) ? 0. : sub[r][c];
    return true;
}

std::vector<std::vector<double>> flush_sub_matrix(const std::vector<double>& raw, size_t nozzles, size_t nozzle, size_t full_n,
                                                  const std::vector<size_t>& physical)
{
    const size_t block = full_n * full_n;
    if (nozzle >= nozzles || raw.size() < (nozzle + 1) * block)
        return {};
    const size_t                     p = physical.size();
    std::vector<std::vector<double>> out(p, std::vector<double>(p, 0.));
    for (size_t r = 0; r < p; ++r)
        for (size_t c = 0; c < p; ++c)
            if (physical[r] < full_n && physical[c] < full_n)
                out[r][c] = raw[nozzle * block + physical[r] * full_n + physical[c]];
    return out;
}

std::vector<int> mixed_ratio_percents(const std::string& stored, size_t components)
{
    std::vector<int> out;
    std::string      token;
    std::istringstream in(stored);
    while (std::getline(in, token, ',')) {
        std::istringstream num(token);
        num.imbue(std::locale::classic());
        double v = 0.;
        if (!(num >> v) || !std::isfinite(v))
            break;
        out.push_back(int(v * 100. + 0.5));
    }
    if (components == 0)
        return {};
    if (out.size() != components) {
        const int n = int(components);
        out.assign(components, 100 / n);
        out[0] += 100 - (100 / n) * n;
    }
    return out;
}

static const char* const DECOMPOSE_PLA_BASIC = "PLA Basic"; // kDecomposePlaBasicType, without the GUI header

bool decompose_type_matches(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
        return false;
    return a == b || a == b + " Basic" || b == a + " Basic";
}

std::vector<std::string> decompose_offered_types(const std::vector<std::string>& physical_types)
{
    std::vector<std::string> out;
    for (const std::string& t : physical_types) {
        const std::string type = t.empty() ? std::string("PLA") : t;
        if (std::find(out.begin(), out.end(), type) == out.end())
            out.push_back(type);
    }
    if (std::find(out.begin(), out.end(), DECOMPOSE_PLA_BASIC) == out.end())
        out.push_back(DECOMPOSE_PLA_BASIC);
    return out;
}

std::string decompose_default_type(const std::vector<std::string>& physical_types, int source)
{
    const std::vector<std::string> offered = decompose_offered_types(physical_types);
    std::string preferred = source >= 0 && size_t(source) < physical_types.size() ? physical_types[size_t(source)] :
                            physical_types.empty()                                ? std::string() :
                                                                                    physical_types.front();
    if (std::find(offered.begin(), offered.end(), preferred) == offered.end())
        preferred = offered.front();
    return preferred;
}

std::vector<std::string> decompose_offered_modes(const std::vector<std::string>& physical_types, int source, const std::string& type)
{
    int same_type = 0;
    for (size_t i = 0; i < physical_types.size(); ++i)
        if (int(i) != source && decompose_type_matches(physical_types[i], type))
            ++same_type;
    std::vector<std::string> modes;
    if (same_type >= 2)
        modes.push_back("materialList");
    if (type == DECOMPOSE_PLA_BASIC) {
        modes.push_back("cmyw");
        modes.push_back("rybw");
    }
    return modes;
}

int replacement_after_delete(int slot, int replace_with)
{
    if (replace_with < 0 || replace_with == slot)
        return -1;
    return replace_with > slot ? replace_with - 1 : replace_with;
}

SlotRemovalPlan plan_slot_removal(const std::vector<bool>& is_mixed, const std::vector<int>& listed, bool listed_are_kept,
                                  int replace_with)
{
    SlotRemovalPlan plan;
    const int       total = int(is_mixed.size());
    const char*     field = listed_are_kept ? "/params/keep" : "/params/slots";
    auto            fail  = [&plan](std::string why, std::string path) {
        plan.order.clear();
        plan.error = std::move(why);
        plan.path  = std::move(path);
        return plan;
    };

    std::vector<bool> named(size_t(total), false);
    for (size_t i = 0; i < listed.size(); ++i) {
        const int s = listed[i];
        if (s < 0 || s >= total)
            return fail("slot " + std::to_string(s) + " is out of range (" + std::to_string(total) + " slots, counted from 0)",
                        std::string(field) + "/" + std::to_string(i));
        if (named[size_t(s)])
            return fail("slot " + std::to_string(s) + " is listed twice", std::string(field) + "/" + std::to_string(i));
        named[size_t(s)] = true;
    }

    std::vector<bool> removed(size_t(total), false);
    for (int s = 0; s < total; ++s)
        removed[size_t(s)] = listed_are_kept ? !named[size_t(s)] : named[size_t(s)];

    int removed_count = 0, physical_left = 0;
    for (int s = 0; s < total; ++s) {
        removed_count += removed[size_t(s)] ? 1 : 0;
        physical_left += (!removed[size_t(s)] && !is_mixed[size_t(s)]) ? 1 : 0;
    }
    if (removed_count == 0)
        return fail(listed_are_kept ? "keep names every slot: nothing to remove" : "no slot to remove", field);
    if (removed_count == total)
        return fail("every slot would be removed; at least one filament must stay", field);
    if (physical_left == 0)
        return fail("every physical filament would be removed; at least one must stay", field);
    if (replace_with >= 0) {
        if (replace_with >= total)
            return fail("replaceWith is out of range (" + std::to_string(total) + " slots)", "/params/replaceWith");
        if (removed[size_t(replace_with)])
            return fail("replaceWith must name a slot that stays", "/params/replaceWith");
    }

    for (int s = total - 1; s >= 0; --s)
        if (removed[size_t(s)])
            plan.order.push_back(s);
    return plan;
}

int replacement_during_batch(int replace_with, const std::vector<int>& removed_so_far)
{
    if (replace_with < 0)
        return -1;
    int shift = 0;
    for (int r : removed_so_far)
        if (r < replace_with)
            ++shift;
    return replace_with - shift;
}

namespace {

// ---- helpers ----------------------------------------------------------------------------------

json schema(json properties, std::vector<std::string> required = {})
{
    json s = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = required;
    return s;
}

json slot_schema(const std::string& description)
{
    return {{"type", "integer"}, {"minimum", 0}, {"maximum", 63}, {"description", description}};
}

json example(json params) { return json::array({json{{"target", nullptr}, {"params", std::move(params)}}}); }

PresetBundle& bundle() { return *wxGetApp().preset_bundle; }

size_t slot_count() { return wxGetApp().preset_bundle ? wxGetApp().preset_bundle->filament_presets.size() : 0; }

std::string slot_label(size_t slot)
{
    const auto& presets = bundle().filament_presets;
    std::string s       = "slot " + std::to_string(slot + 1);
    if (slot < presets.size())
        s += " (" + presets[slot] + ")";
    return s;
}

// The sidebar replaces a .gcode.3mf preview with a new project (and says so in a modal) before any
// filament edit; an automated edit refuses instead.
bool refuse_in_gcode_preview(InvokeContext& ctx, InvokeResult& out)
{
    Plater* plater = ctx.plater();
    if (plater == nullptr || wxGetApp().preset_bundle == nullptr) {
        out = InvokeResult::error(WAB_INTERNAL, "the plater is not available");
        return true;
    }
    if (plater->using_exported_file()) {
        out = InvokeResult::error(WAB_BUSY, "a sliced file is open for preview; open or create a project first",
                                  {{"reason", "gcode-preview"}});
        return true;
    }
    return false;
}

// Objects that print (in whole or in part) with `slot` (0-based), for the confirmation text.
std::vector<std::string> users_of_slot(const Model& model, size_t slot)
{
    std::vector<std::string> names;
    const int                one_based = int(slot) + 1;
    for (const ModelObject* obj : model.objects) {
        bool uses = obj->config.has("extruder") && obj->config.extruder() == one_based;
        for (const ModelVolume* vol : obj->volumes)
            if (vol->config.has("extruder") && vol->config.extruder() == one_based)
                uses = true;
        if (uses)
            names.push_back(obj->name);
    }
    return names;
}

std::string project_value(const std::string& key)
{
    const DynamicPrintConfig& cfg = bundle().project_config;
    return cfg.has(key) ? cfg.opt_serialize(key) : std::string();
}

// Project-config writes are not on the undo stack; the turn log keeps them so undoTurn can put
// them back (type "project").
void note_project_write(InvokeContext& ctx, const std::string& key, const std::string& previous)
{
    if (ctx.host == nullptr || ctx.turn.empty())
        return;
    TurnLog::SettingWrite w;
    w.type     = "project";
    w.key      = key;
    w.previous = previous;
    w.written  = project_value(key);
    if (w.written != w.previous)
        ctx.host->turn_log.note_setting(ctx.turn, std::move(w));
}

json slots_json()
{
    PresetBundle& b       = bundle();
    const auto*   colours = b.project_config.option<ConfigOptionStrings>("filament_colour");
    json          arr     = json::array();
    for (size_t i = 0; i < b.filament_presets.size(); ++i)
        arr.push_back({{"slot", i},
                       {"preset", b.filament_presets[i]},
                       {"colour", colours && i < colours->values.size() ? colours->values[i] : std::string()},
                       {"mixed", b.is_mixed_filament(i)}});
    return arr;
}

std::string enabled_needs_slots(size_t minimum)
{
    if (wxGetApp().preset_bundle == nullptr)
        return "presets are not loaded";
    if (bundle().num_physical_filaments() < minimum)
        return "the project has fewer than " + std::to_string(minimum) + " filaments";
    return {};
}

// ---- filament.delete / merge ------------------------------------------------------------------

InvokeResult run_delete(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    const int    slot    = ctx.params.value("slot", -1);
    const int    replace = ctx.params.value("replaceWith", -1);
    const size_t total   = slot_count();
    if (slot < 0 || size_t(slot) >= total)
        return InvokeResult::error(WAB_BAD_PARAMS, "slot out of range (" + std::to_string(total) + " slots)", {{"path", "/params/slot"}});
    if (replace >= 0 && (size_t(replace) >= total || replace == slot))
        return InvokeResult::error(WAB_BAD_PARAMS, "replaceWith must name another existing slot", {{"path", "/params/replaceWith"}});
    const bool mixed = bundle().is_mixed_filament(size_t(slot));
    if (!mixed && bundle().num_physical_filaments() <= 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "the last physical filament cannot be removed", {{"path", "/params/slot"}});

    // Sidebar::delete_filament re-selects slot 1's preset in the Filament tab when the removed slot
    // is the one being edited (or the first): a preset switch, so the ifDirty contract applies.
    if (!mixed && (slot == 0 || bundle().is_the_only_edited_filament(unsigned(slot)))) {
        InvokeResult refusal;
        if (!ctx.preset_dirty_preflight(Preset::TYPE_FILAMENT, refusal))
            return refusal;
    }

    const std::string              removed = bundle().filament_presets[size_t(slot)];
    const std::vector<std::string> users   = users_of_slot(ctx.model(), size_t(slot));
    ConfirmRequest                 req;
    req.title   = "Remove filament";
    req.message = "Remove filament " + slot_label(size_t(slot)) + " from the project?";
    if (!users.empty())
        req.message += " " + std::to_string(users.size()) + " object(s) print with it; they will print with " +
                       (replace >= 0 ? slot_label(size_t(replace)) : std::string("their default filament")) +
                       ", and painting in this colour moves the same way.";
    req.details      = users;
    req.accept_label = "Remove";
    if (!ctx.confirm(req))
        return InvokeResult::error(WAB_USER_CANCELLED, "the person declined to remove the filament");

    Sidebar& sidebar = ctx.plater()->sidebar();
    sidebar.delete_filament(size_t(slot), replace);
    sidebar.update_filaments_counter();
    return InvokeResult::ok({{"removed", {{"slot", slot}, {"preset", removed}, {"mixed", mixed}}},
                             {"replacedBy", replace},
                             {"objectsMoved", users.size()},
                             {"slots", slots_json()}});
}

// ---- filament.removeSlots ---------------------------------------------------------------------
// Several slots at once behind ONE native confirmation (a person asked to keep only slot 1 should
// press Remove once, not once per slot). Each removal is the same Sidebar::delete_filament call
// filament.delete makes, highest index first, so objects, volumes and painting fall back exactly
// as they do there. Like filament.delete it takes no undo snapshot (slot removal is not on the
// slicer's undo stack).

constexpr size_t REMOVE_SLOTS_LISTED = 10; // slot lines shown in the confirmation before "+N more"

InvokeResult run_remove_slots(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    const bool has_slots = ctx.params.contains("slots");
    const bool has_keep  = ctx.params.contains("keep");
    if (has_slots == has_keep)
        return InvokeResult::error(WAB_BAD_PARAMS, "give exactly one of slots (the slots to remove) or keep (the slots to keep)",
                                   {{"path", "/params"}, {"needs", "slots"}});
    std::vector<int> listed;
    for (const json& v : ctx.params[has_keep ? "keep" : "slots"])
        listed.push_back(v.get<int>());
    const int replace = ctx.params.value("replaceWith", -1);

    PresetBundle&     b     = bundle();
    const size_t      total = slot_count();
    std::vector<bool> is_mixed(total, false);
    for (size_t i = 0; i < total; ++i)
        is_mixed[i] = b.is_mixed_filament(i);
    const SlotRemovalPlan plan = plan_slot_removal(is_mixed, listed, has_keep, replace);
    if (plan.order.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, plan.error, {{"path", plan.path}});

    // Sidebar::delete_filament re-selects slot 1's preset in the Filament tab when a removed slot is
    // the one being edited (or the first): a preset switch, so the ifDirty contract applies -- once.
    for (int s : plan.order)
        if (!is_mixed[size_t(s)] && (s == 0 || b.is_the_only_edited_filament(unsigned(s)))) {
            InvokeResult refusal;
            if (!ctx.preset_dirty_preflight(Preset::TYPE_FILAMENT, refusal))
                return refusal;
            break;
        }

    // What goes, lowest first as the person reads it, and the objects that print with any of it.
    std::vector<int> ascending(plan.order.rbegin(), plan.order.rend());
    json             removed = json::array();
    std::vector<std::string> lines, users;
    for (int s : ascending) {
        removed.push_back({{"slot", s}, {"preset", b.filament_presets[size_t(s)]}, {"mixed", bool(is_mixed[size_t(s)])}});
        std::string line = slot_label(size_t(s));
        line[0]          = 'S'; // "Slot 2 (...)" as a line of its own
        lines.push_back(std::move(line));
        for (std::string& name : users_of_slot(ctx.model(), size_t(s)))
            if (std::find(users.begin(), users.end(), name) == users.end())
                users.push_back(std::move(name));
    }

    ConfirmRequest req;
    req.title   = "Remove filaments";
    req.message = ascending.size() == 1 ? "Remove 1 filament slot from the project?"
                                        : "Remove " + std::to_string(ascending.size()) + " filament slots from the project?";
    if (!users.empty())
        req.message += " " + std::to_string(users.size()) + " object(s) print with them; they will print with " +
                       (replace >= 0 ? slot_label(size_t(replace)) : std::string("their default filament")) +
                       ", and painting in these colours moves the same way.";
    const size_t shown = std::min(lines.size(), REMOVE_SLOTS_LISTED);
    req.details.assign(lines.begin(), lines.begin() + shown);
    if (lines.size() > shown)
        req.details.push_back("+" + std::to_string(lines.size() - shown) + " more");
    req.accept_label = "Remove";
    if (!ctx.confirm(req))
        return InvokeResult::error(WAB_USER_CANCELLED, "the person declined to remove the filaments");

    Sidebar&         sidebar = ctx.plater()->sidebar();
    std::vector<int> gone;
    for (int s : plan.order) {
        sidebar.delete_filament(size_t(s), replacement_during_batch(replace, gone));
        gone.push_back(s);
    }
    sidebar.update_filaments_counter();
    return InvokeResult::ok({{"removed", removed},
                             {"replacedBy", replace},
                             {"objectsMoved", users.size()},
                             {"slots", slots_json()}});
}

InvokeResult run_merge(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    const int    from  = ctx.params.value("from", -1);
    const int    to    = ctx.params.value("to", -1);
    const size_t total = slot_count();
    if (from < 0 || size_t(from) >= total)
        return InvokeResult::error(WAB_BAD_PARAMS, "from is out of range (" + std::to_string(total) + " slots)", {{"path", "/params/from"}});
    if (to < 0 || size_t(to) >= total || to == from)
        return InvokeResult::error(WAB_BAD_PARAMS, "to must name another existing slot", {{"path", "/params/to"}});
    PresetBundle& b          = bundle();
    const bool    from_mixed = b.is_mixed_filament(size_t(from));
    if (!from_mixed && b.num_physical_filaments() <= 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "the last physical filament cannot be merged away", {{"path", "/params/from"}});
    // The sidebar warns (modal) before merging a filament into a mixed one built from it; refuse instead.
    if (!from_mixed && b.is_mixed_filament(size_t(to))) {
        if (const auto* comps = b.project_config.option<ConfigOptionStrings>("filament_mixed_components");
            comps && size_t(to) < comps->values.size()) {
            for (unsigned int c : Slic3r::parse_mixed_components(comps->values[size_t(to)]))
                if (c == unsigned(from) + 1)
                    return InvokeResult::error(WAB_BAD_PARAMS, "the target mixed filament is made from this filament; merging would break it",
                                               {{"path", "/params/to"}});
        }
    }
    if (!from_mixed && (from == 0 || b.is_the_only_edited_filament(unsigned(from)))) {
        InvokeResult refusal;
        if (!ctx.preset_dirty_preflight(Preset::TYPE_FILAMENT, refusal))
            return refusal;
    }

    const std::string              removed = b.filament_presets[size_t(from)];
    const std::vector<std::string> users   = users_of_slot(ctx.model(), size_t(from));
    ConfirmRequest                 req;
    req.title        = "Merge filaments";
    req.message      = "Merge filament " + slot_label(size_t(from)) + " into " + slot_label(size_t(to)) + "? Filament " +
                  slot_label(size_t(from)) + " is removed from the project and everything printed with it prints with " +
                  slot_label(size_t(to)) + ".";
    req.details      = users;
    req.accept_label = "Merge";
    if (!ctx.confirm(req))
        return InvokeResult::error(WAB_USER_CANCELLED, "the person declined the merge");

    Sidebar& sidebar = ctx.plater()->sidebar();
    sidebar.change_filament(size_t(from), size_t(to));
    sidebar.update_filaments_counter();
    return InvokeResult::ok({{"removed", {{"slot", from}, {"preset", removed}}},
                             {"into", replacement_after_delete(from, to)},
                             {"objectsMoved", users.size()},
                             {"slots", slots_json()}});
}

// ---- filament.setColour ---------------------------------------------------------------------

InvokeResult run_set_colour(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    const int slot = ctx.params.value("slot", -1);
    if (slot < 0 || size_t(slot) >= slot_count())
        return InvokeResult::error(WAB_BAD_PARAMS, "slot out of range (" + std::to_string(slot_count()) + " slots)", {{"path", "/params/slot"}});
    if (bundle().is_mixed_filament(size_t(slot)))
        return InvokeResult::error(WAB_BAD_PARAMS, "a mixed filament's colour comes from its components", {{"path", "/params/slot"}});
    std::string hex;
    if (!normalize_hex_colour(ctx.params.value("colour", std::string()), hex))
        return InvokeResult::error(WAB_BAD_PARAMS, "colour must be a hex colour like \"#FF8800\"", {{"path", "/params/colour"}});

    Sidebar& sidebar = ctx.plater()->sidebar();
    auto&    combos  = sidebar.combos_filament();
    if (size_t(slot) >= combos.size() || combos[size_t(slot)] == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the sidebar has no control for this slot");

    const std::string previous = project_value("filament_colour");
    combos[size_t(slot)]->sync_colour_config({hex}, false);
    // As the colour picker does: auto flushing volumes, mixed swatches.
    wxCommandEvent evt(EVT_FILAMENT_COLOR_CHANGED);
    evt.SetInt(slot);
    wxPostEvent(ctx.plater(), evt);
    note_project_write(ctx, "filament_colour", previous);
    return InvokeResult::ok({{"slot", slot}, {"colour", hex}});
}

// ---- flushing volumes / purge mode -----------------------------------------------------------

json flush_state()
{
    PresetBundle&             b       = bundle();
    const DynamicPrintConfig& cfg     = b.project_config;
    const auto*               raw     = cfg.option<ConfigOptionFloats>("flush_volumes_matrix");
    const auto*               mult    = cfg.option<ConfigOptionFloats>("flush_multiplier");
    const auto*               colours = cfg.option<ConfigOptionStrings>("filament_colour");
    const size_t              full_n  = colours ? colours->values.size() : 0;
    const auto                phys    = b.physical_filament_config_indices();
    const size_t              nozzles = std::max<size_t>(1, mult ? mult->values.size() : 1);
    json                      blocks  = json::array();
    for (size_t n = 0; n < nozzles; ++n) {
        const auto matrix = raw ? flush_sub_matrix(raw->values, nozzles, n, full_n, phys) : std::vector<std::vector<double>>{};
        blocks.push_back({{"nozzle", n}, {"multiplier", mult && n < mult->values.size() ? mult->values[n] : 1.}, {"matrix", matrix}});
    }
    return {{"slots", phys}, {"nozzles", blocks}, {"modified", is_flush_config_modified()}};
}

void after_flush_change(Plater* plater)
{
    plater->sidebar().set_flushing_volume_warning(is_flush_config_modified());
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
    plater->update_project_dirty_from_presets();
    if (GLCanvas3D* canvas = plater->get_view3D_canvas3D())
        canvas->reload_scene(true);
    plater->update();
    plater->schedule_background_process();
}

InvokeResult run_get_flush(InvokeContext& ctx)
{
    if (ctx.plater() == nullptr || wxGetApp().preset_bundle == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the plater is not available");
    return InvokeResult::ok(flush_state());
}

InvokeResult run_set_flush(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    PresetBundle&       b      = bundle();
    DynamicPrintConfig& cfg    = b.project_config;
    auto*               raw    = cfg.option<ConfigOptionFloats>("flush_volumes_matrix");
    auto*               mult   = cfg.option<ConfigOptionFloats>("flush_multiplier");
    const auto*         colors = cfg.option<ConfigOptionStrings>("filament_colour");
    if (raw == nullptr || mult == nullptr || colors == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the project has no flushing volumes");
    const std::string mode      = ctx.params.value("mode", std::string("auto"));
    const size_t      nozzles   = std::max<size_t>(1, mult->values.size());
    const int         nozzle_in = ctx.params.value("nozzle", 0);
    if (nozzle_in < 0 || size_t(nozzle_in) >= nozzles)
        return InvokeResult::error(WAB_BAD_PARAMS, "nozzle out of range (" + std::to_string(nozzles) + " nozzles)", {{"path", "/params/nozzle"}});
    const size_t nozzle = size_t(nozzle_in);

    const std::string prev_matrix = project_value("flush_volumes_matrix");
    const std::string prev_mult   = project_value("flush_multiplier");

    auto set_multiplier = [&]() {
        if (!ctx.params.contains("multiplier"))
            return;
        if (mult->values.size() < nozzles)
            mult->values.resize(nozzles, 1.);
        mult->values[nozzle] = ctx.params["multiplier"].get<double>();
    };

    if (mode == "auto") {
        // The flushing dialog's auto-calculation: colour-based defaults for every pair.
        set_multiplier();
        ctx.plater()->sidebar().auto_calc_flushing_volumes(-1, ctx.params.contains("nozzle") ? int(nozzle) : -1);
    } else {
        if (!ctx.params.contains("matrix"))
            return InvokeResult::error(WAB_BAD_PARAMS, "mode \"matrix\" needs matrix", {{"path", "/params/matrix"}, {"needs", "matrix"}});
        const auto          sub    = ctx.params["matrix"].get<std::vector<std::vector<double>>>();
        std::vector<double> values = raw->values;
        std::string         message;
        if (!apply_flush_sub_matrix(values, nozzles, nozzle, colors->values.size(), b.physical_filament_config_indices(), sub, message))
            return InvokeResult::error(WAB_BAD_PARAMS, message, {{"path", "/params/matrix"}});
        raw->values = std::move(values);
        set_multiplier();
    }
    after_flush_change(ctx.plater());
    note_project_write(ctx, "flush_volumes_matrix", prev_matrix);
    note_project_write(ctx, "flush_multiplier", prev_mult);
    return InvokeResult::ok(flush_state());
}

struct PurgeSupport
{
    bool multiple_nozzle = false;
    bool fast            = false;
};

// Tab.cpp (TabPrinter) shows the sidebar's Purge mode button on the same conditions.
PurgeSupport purge_support()
{
    PurgeSupport s;
    if (wxGetApp().preset_bundle == nullptr)
        return s;
    const DynamicPrintConfig& printer = bundle().printers.get_edited_preset().config;
    if (const auto* counts = printer.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count"))
        s.multiple_nozzle = std::any_of(counts->values.begin(), counts->values.end(),
                                        [](int v) { return v > 1 && v != ConfigOptionIntsNullable::nil_value(); });
    if (const auto* fast = printer.option<ConfigOptionBool>("support_fast_purge_mode"))
        s.fast = fast->value;
    return s;
}

InvokeResult run_set_purge_mode(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    const std::string  mode  = ctx.params.value("mode", std::string("default"));
    const PurgeSupport s     = purge_support();
    PrimeVolumeMode    value = pvmDefault;
    if (mode == "saving") {
        if (!s.multiple_nozzle)
            return InvokeResult::error(WAB_BAD_PARAMS, "this printer has no multi-nozzle extruder, so it has no prime-saving mode",
                                       {{"path", "/params/mode"}});
        value = pvmSaving;
    } else if (mode == "fast") {
        if (!s.fast)
            return InvokeResult::error(WAB_BAD_PARAMS, "this printer does not support fast purge", {{"path", "/params/mode"}});
        value = pvmFast;
    }
    const std::string previous = project_value("prime_volume_mode");
    bundle().project_config.set_key_value("prime_volume_mode", new ConfigOptionEnum<PrimeVolumeMode>(value));
    ctx.plater()->update();
    note_project_write(ctx, "prime_volume_mode", previous);
    return InvokeResult::ok({{"mode", mode}});
}

// ---- mixed filaments -------------------------------------------------------------------------

std::string ratio_string(const std::vector<int>& ratios)
{
    int sum = 0;
    for (int r : ratios)
        sum += r;
    if (sum <= 0)
        sum = 100;
    CNumericLocalesSetter locale;
    std::string           s;
    for (size_t i = 0; i < ratios.size(); ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", float(ratios[i]) / float(sum));
        if (i > 0)
            s += ",";
        s += buf;
    }
    return s;
}

template<class Opt, class V> void put_at(DynamicPrintConfig& cfg, const char* key, size_t idx, const V& value, const V& fill)
{
    auto* opt = cfg.option<Opt>(key);
    if (opt == nullptr)
        return;
    while (opt->values.size() <= idx)
        opt->values.push_back(fill);
    opt->values[idx] = value;
}

// The mixed-filament section's "+" without its dialog: the same project-config writes as
// Plater.cpp create_mixed_filament_from_result (without a custom gradient curve).
InvokeResult run_add_mixed(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    PresetBundle& b        = bundle();
    Sidebar&      sidebar  = ctx.plater()->sidebar();
    const size_t  physical = sidebar.combos_filament().size();
    if (physical < 2)
        return InvokeResult::error(WAB_BAD_PARAMS, "mixing needs at least two physical filaments");
    if (b.filament_presets.size() >= size_t(EnforcerBlockerType::ExtruderMax))
        return InvokeResult::error(WAB_BAD_PARAMS, "the project has the maximum number of filaments");

    std::vector<unsigned int> components;
    for (const json& c : ctx.params.at("components")) {
        const int slot = c.get<int>();
        if (slot < 0 || size_t(slot) >= physical)
            return InvokeResult::error(WAB_BAD_PARAMS, "components must be physical slots (0.." + std::to_string(physical - 1) + ")",
                                       {{"path", "/params/components"}});
        if (std::find(components.begin(), components.end(), unsigned(slot) + 1) != components.end())
            return InvokeResult::error(WAB_BAD_PARAMS, "components must differ", {{"path", "/params/components"}});
        components.push_back(unsigned(slot) + 1);
    }
    std::vector<int> ratios;
    if (ctx.params.contains("ratios")) {
        for (const json& r : ctx.params["ratios"])
            ratios.push_back(r.get<int>());
        if (ratios.size() != components.size())
            return InvokeResult::error(WAB_BAD_PARAMS, "ratios must have one entry per component", {{"path", "/params/ratios"}});
    } else {
        const int n = int(components.size());
        ratios.assign(size_t(n), 100 / n);
        ratios[0] += 100 - (100 / n) * n;
    }
    const bool gradient = ctx.params.value("gradient", false);
    const bool reverse  = ctx.params.value("gradientReverse", false);
    const bool per_part = ctx.params.value("gradientPerPart", false);
    if (gradient && components.size() != 2)
        return InvokeResult::error(WAB_BAD_PARAMS, "a gradient mixes exactly two filaments", {{"path", "/params/gradient"}});

    std::vector<std::string> colours, names, types;
    sidebar.collect_physical_filament_info(colours, names, types);
    std::vector<std::string> hex;
    for (unsigned int id : components)
        hex.push_back(id >= 1 && id <= colours.size() ? colours[id - 1] : std::string("#808080"));
    const std::string mixed_colour = Slic3r::blend_color_multi(hex, ratios);

    DynamicPrintConfig& cfg     = b.project_config;
    const size_t        new_idx = b.filament_presets.size();
    b.set_num_filaments(unsigned(new_idx + 1), mixed_colour);
    std::string comp_str;
    for (size_t i = 0; i < components.size(); ++i)
        comp_str += (i ? "," : "") + std::to_string(components[i]);

    const unsigned char yes = 1, no = 0;
    put_at<ConfigOptionStrings>(cfg, "filament_multi_colour", new_idx, mixed_colour, std::string());
    put_at<ConfigOptionBools>(cfg, "filament_is_mixed", new_idx, yes, no);
    put_at<ConfigOptionStrings>(cfg, "filament_mixed_components", new_idx, comp_str, std::string());
    put_at<ConfigOptionStrings>(cfg, "filament_mixed_sublayer_ratios", new_idx, ratio_string(ratios), std::string());
    if (!cfg.option("filament_mixed_gradient"))
        cfg.set_key_value("filament_mixed_gradient", new ConfigOptionBools({false}));
    if (!cfg.option("filament_mixed_gradient_range"))
        cfg.set_key_value("filament_mixed_gradient_range", new ConfigOptionStrings({""}));
    if (!cfg.option("filament_mixed_gradient_curve"))
        cfg.set_key_value("filament_mixed_gradient_curve", new ConfigOptionStrings({""}));
    if (!cfg.option("filament_mixed_gradient_per_part"))
        cfg.set_key_value("filament_mixed_gradient_per_part", new ConfigOptionBools({false}));
    put_at<ConfigOptionBools>(cfg, "filament_mixed_gradient", new_idx, gradient ? yes : no, no);
    put_at<ConfigOptionStrings>(cfg, "filament_mixed_gradient_range", new_idx,
                                std::string(gradient ? (reverse ? "0.1000,0.9000" : "0.9000,0.1000") : ""), std::string());
    put_at<ConfigOptionStrings>(cfg, "filament_mixed_gradient_curve", new_idx, std::string(), std::string());
    put_at<ConfigOptionBools>(cfg, "filament_mixed_gradient_per_part", new_idx, (gradient && per_part) ? yes : no, no);

    auto& presets = b.filament_presets;
    if (components[0] >= 1 && components[0] <= physical && presets.size() > new_idx)
        presets[new_idx] = presets[components[0] - 1];

    const size_t count = b.filament_presets.size();
    ctx.plater()->get_partplate_list().on_filament_added(int(count));
    ctx.plater()->on_filament_count_change(count);
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
    b.export_selections(*wxGetApp().app_config);
    sidebar.update_mixed_filament_list();
    sidebar.update_filaments_counter();
    ctx.plater()->update_project_dirty_from_presets();
    ctx.plater()->schedule_background_process();
    return InvokeResult::ok({{"slot", new_idx}, {"colour", mixed_colour}, {"slots", slots_json()}});
}

// The mixed row's Edit (Sidebar::edit_mixed_filament) without its dialog: the same project-config
// writes at the slot's own index, so every object, part and painted area printing with it keeps
// doing so. What is not given stays as it is.
InvokeResult run_edit_mixed(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    PresetBundle& b    = bundle();
    const int     slot = ctx.params.value("slot", -1);
    if (slot < 0 || size_t(slot) >= b.filament_presets.size() || !b.is_mixed_filament(size_t(slot)))
        return InvokeResult::error(WAB_BAD_PARAMS, "slot must be a mixed filament slot", {{"path", "/params/slot"}});
    Sidebar&     sidebar  = ctx.plater()->sidebar();
    const size_t physical = sidebar.combos_filament().size();
    if (physical < 2)
        return InvokeResult::error(WAB_BAD_PARAMS, "mixing needs at least two physical filaments");

    DynamicPrintConfig& cfg = b.project_config;
    auto stored = [&](const char* key) -> std::string {
        const auto* opt = cfg.option<ConfigOptionStrings>(key);
        return opt != nullptr && size_t(slot) < opt->values.size() ? opt->values[size_t(slot)] : std::string();
    };
    auto stored_bool = [&](const char* key) {
        const auto* opt = cfg.option<ConfigOptionBools>(key);
        return opt != nullptr && size_t(slot) < opt->values.size() && opt->values[size_t(slot)] != 0;
    };

    // Current state, as the dialog would open with it.
    std::vector<unsigned int> components = Slic3r::parse_mixed_components(stored("filament_mixed_components"));
    std::vector<int>          ratios     = mixed_ratio_percents(stored("filament_mixed_sublayer_ratios"), components.size());
    if (components.size() < 2) {
        components = {1, 2};
        ratios     = {50, 50};
    }
    const bool  was_gradient = stored_bool("filament_mixed_gradient");
    const bool  was_per_part = stored_bool("filament_mixed_gradient_per_part");
    std::string range        = stored("filament_mixed_gradient_range");
    bool        was_reverse  = false;
    {
        CNumericLocalesSetter locale;
        float v0 = 0.f, v1 = 0.f;
        if (std::sscanf(range.c_str(), "%f,%f", &v0, &v1) == 2)
            was_reverse = v0 < v1;
    }

    // The edit.
    if (ctx.params.contains("components")) {
        components.clear();
        for (const json& c : ctx.params["components"]) {
            const int s = c.get<int>();
            if (s < 0 || size_t(s) >= physical)
                return InvokeResult::error(WAB_BAD_PARAMS, "components must be physical slots (0.." + std::to_string(physical - 1) + ")",
                                           {{"path", "/params/components"}});
            if (std::find(components.begin(), components.end(), unsigned(s) + 1) != components.end())
                return InvokeResult::error(WAB_BAD_PARAMS, "components must differ", {{"path", "/params/components"}});
            components.push_back(unsigned(s) + 1);
        }
        if (!ctx.params.contains("ratios")) {
            const int n = int(components.size());
            ratios.assign(size_t(n), 100 / n);
            ratios[0] += 100 - (100 / n) * n;
        }
    }
    if (ctx.params.contains("ratios")) {
        ratios.clear();
        for (const json& r : ctx.params["ratios"])
            ratios.push_back(r.get<int>());
    }
    if (ratios.size() != components.size())
        return InvokeResult::error(WAB_BAD_PARAMS, "ratios must have one entry per component", {{"path", "/params/ratios"}});
    for (unsigned int id : components)
        if (id < 1 || id > physical)
            return InvokeResult::error(WAB_BAD_PARAMS, "the mixed filament names a filament slot that no longer exists; give components",
                                       {{"path", "/params/components"}, {"needs", "components"}});

    bool gradient = ctx.params.value("gradient", was_gradient);
    if (gradient && components.size() != 2) {
        if (ctx.params.value("gradient", false))
            return InvokeResult::error(WAB_BAD_PARAMS, "a gradient mixes exactly two filaments", {{"path", "/params/gradient"}});
        gradient = false; // the dialog offers the gradient for two components only
    }
    const bool reverse  = ctx.params.value("gradientReverse", was_reverse);
    const bool per_part = gradient && ctx.params.value("gradientPerPart", was_per_part);
    // A custom curve (drawn in the dialog) survives an edit that keeps the gradient as it was.
    std::string curve = stored("filament_mixed_gradient_curve");
    if (!gradient || reverse != was_reverse || !was_gradient || components.size() != 2)
        curve.clear();

    std::vector<std::string> colours, names, types;
    sidebar.collect_physical_filament_info(colours, names, types);
    std::vector<std::string> hex;
    for (unsigned int id : components)
        hex.push_back(id >= 1 && id <= colours.size() ? colours[id - 1] : std::string("#808080"));
    const std::string mixed_colour = Slic3r::blend_color_multi(hex, ratios);

    static const char* const keys[] = {"filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_gradient",
                                       "filament_mixed_gradient_range", "filament_mixed_gradient_curve",
                                       "filament_mixed_gradient_per_part", "filament_colour", "filament_multi_colour"};
    std::vector<std::string> previous;
    for (const char* k : keys)
        previous.push_back(project_value(k));

    std::string comp_str;
    for (size_t i = 0; i < components.size(); ++i)
        comp_str += (i ? "," : "") + std::to_string(components[i]);
    const unsigned char yes = 1, no = 0;
    const size_t        idx = size_t(slot);
    if (!cfg.option("filament_mixed_gradient"))
        cfg.set_key_value("filament_mixed_gradient", new ConfigOptionBools({false}));
    if (!cfg.option("filament_mixed_gradient_range"))
        cfg.set_key_value("filament_mixed_gradient_range", new ConfigOptionStrings({""}));
    if (!cfg.option("filament_mixed_gradient_curve"))
        cfg.set_key_value("filament_mixed_gradient_curve", new ConfigOptionStrings({""}));
    if (!cfg.option("filament_mixed_gradient_per_part"))
        cfg.set_key_value("filament_mixed_gradient_per_part", new ConfigOptionBools({false}));
    put_at<ConfigOptionStrings>(cfg, "filament_mixed_components", idx, comp_str, std::string());
    put_at<ConfigOptionStrings>(cfg, "filament_mixed_sublayer_ratios", idx, ratio_string(ratios), std::string());
    put_at<ConfigOptionBools>(cfg, "filament_mixed_gradient", idx, gradient ? yes : no, no);
    put_at<ConfigOptionStrings>(cfg, "filament_mixed_gradient_range", idx,
                                std::string(gradient ? (reverse ? "0.1000,0.9000" : "0.9000,0.1000") : ""), std::string());
    put_at<ConfigOptionStrings>(cfg, "filament_mixed_gradient_curve", idx, curve, std::string());
    put_at<ConfigOptionBools>(cfg, "filament_mixed_gradient_per_part", idx, per_part ? yes : no, no);
    // As the dialog's OK: the swatch takes the blend, only where the slot already has an entry.
    if (auto* opt = cfg.option<ConfigOptionStrings>("filament_colour"); opt && idx < opt->values.size())
        opt->values[idx] = mixed_colour;
    if (auto* opt = cfg.option<ConfigOptionStrings>("filament_multi_colour"); opt && idx < opt->values.size())
        opt->values[idx] = mixed_colour;

    bool changed = false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        changed = changed || project_value(keys[i]) != previous[i];
        note_project_write(ctx, keys[i], previous[i]);
    }
    if (!changed)
        return InvokeResult::ok({{"slot", slot}, {"colour", mixed_colour}, {"changed", false}, {"slots", slots_json()}}).with_effects({});

    // The edited slot keeps its index, so only the lists that show it need refreshing.
    sidebar.update_mixed_filament_list();
    sidebar.update_dynamic_filament_list();
    ctx.plater()->update_project_dirty_from_presets();
    ctx.plater()->schedule_background_process();
    json comps = json::array();
    for (unsigned int id : components)
        comps.push_back(int(id) - 1);
    return InvokeResult::ok({{"slot", slot},
                             {"components", comps},
                             {"ratios", ratios},
                             {"gradient", gradient},
                             {"colour", mixed_colour},
                             {"changed", true},
                             {"slots", slots_json()}});
}

// ---- filament.decomposeColour ------------------------------------------------------------------

DecomposeMode decompose_mode_from_wire(const std::string& m)
{
    return m == "cmyw" ? DecomposeMode::CMYW : m == "rybw" ? DecomposeMode::RYBW : DecomposeMode::MaterialList;
}

std::string hex_of(const wxColour& c) { return decompose_normalize_color_hex(c.GetAsString(wxC2S_HTML_SYNTAX).ToStdString()); }

// Filament menu > Decompose Color without its dialog: the dialog's recommendation for the chosen
// material and mode (compute_decompose_result), then what its OK does (Sidebar::
// decompose_filament_color): missing standard filaments are added as physical slots, behind the
// native confirmation instead of the "Tip" message box, and the mixed filament is created.
InvokeResult run_decompose_colour(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    PresetBundle& b       = bundle();
    Sidebar&      sidebar = ctx.plater()->sidebar();
    if (sidebar.combos_filament().size() < 2)
        return InvokeResult::error(WAB_ACTION_DISABLED, "decomposing a colour needs at least two physical filaments",
                                   {{"reason", "too-few-filaments"}});
    if (!ctx.params.contains("slot") && !ctx.params.contains("colour"))
        return InvokeResult::error(WAB_BAD_PARAMS, "give slot (whose colour to decompose) or colour", {{"needs", "slot"}});

    std::vector<std::string> colours, names, types;
    std::vector<size_t>      physical_cfg;
    sidebar.collect_physical_filament_info(colours, names, types, &physical_cfg);
    // The dialog tells PLA Basic (CMYW/RYBW cards) from other PLA, which the sidebar's types do not.
    std::vector<std::string> decompose_types;
    for (size_t ci : physical_cfg)
        decompose_types.push_back(
            filament_type_for_color_decompose(ci < b.filament_presets.size() ? b.filaments.find_preset(b.filament_presets[ci]) : nullptr));

    size_t source_cfg      = size_t(-1);
    int    source_physical = -1;
    std::string target_hex;
    if (ctx.params.contains("slot")) {
        const int slot = ctx.params["slot"].get<int>();
        auto      it   = std::find(physical_cfg.begin(), physical_cfg.end(), size_t(std::max(slot, 0)));
        if (slot < 0 || it == physical_cfg.end())
            return InvokeResult::error(WAB_BAD_PARAMS, "slot must be a physical filament slot", {{"path", "/params/slot"}});
        source_cfg      = size_t(slot);
        source_physical = int(it - physical_cfg.begin());
        const auto* filament_colours = b.project_config.option<ConfigOptionStrings>("filament_colour");
        if (filament_colours == nullptr || source_cfg >= filament_colours->values.size())
            return InvokeResult::error(WAB_INTERNAL, "the slot has no colour");
        target_hex = filament_colours->values[source_cfg];
    }
    if (ctx.params.contains("colour") && !normalize_hex_colour(ctx.params["colour"].get<std::string>(), target_hex))
        return InvokeResult::error(WAB_BAD_PARAMS, "colour must be a hex colour like \"#FF8800\"", {{"path", "/params/colour"}});
    const wxColour target(wxString::FromUTF8(target_hex.c_str()));
    if (!target.IsOk())
        return InvokeResult::error(WAB_BAD_PARAMS, "the colour to decompose is not a colour", {{"path", "/params/colour"}});

    // The dialog's material selector and mode cards.
    const std::vector<std::string> offered_types = decompose_offered_types(decompose_types);
    const std::string type = ctx.params.contains("materialType") ? ctx.params["materialType"].get<std::string>() :
                                                                   decompose_default_type(decompose_types, source_physical);
    if (std::find(offered_types.begin(), offered_types.end(), type) == offered_types.end())
        return InvokeResult::error(WAB_BAD_PARAMS, "materialType must be one the project offers", {{"path", "/params/materialType"}, {"offered", offered_types}});
    const std::vector<std::string> modes = decompose_offered_modes(decompose_types, source_physical, type);
    if (modes.empty())
        return InvokeResult::error(WAB_BAD_PARAMS,
                                   "no decomposition for " + type + ": the material list needs two other " + type +
                                       " filaments, and CMYW/RYBW need materialType \"PLA Basic\"",
                                   {{"path", "/params/materialType"}, {"offered", offered_types}});
    const std::string mode = ctx.params.value("mode", modes.front());
    if (std::find(modes.begin(), modes.end(), mode) == modes.end())
        return InvokeResult::error(WAB_BAD_PARAMS, "mode \"" + mode + "\" is not offered for " + type, {{"path", "/params/mode"}, {"offered", modes}});

    const ColorDecomposeResult recipe = compute_decompose_result(decompose_mode_from_wire(mode), target, source_physical, colours, names,
                                                                 decompose_types, type);
    if (recipe.components.size() < 2)
        return InvokeResult::error(WAB_BAD_PARAMS, "the colour already is a standard " + type + " colour; use that filament instead of a mix",
                                   {{"reason", "single-component"}, {"matchedColour", hex_of(recipe.matched_color)}});

    MixedFilamentResult                    mixed;
    std::vector<DecomposeMissingComponent> missing;
    if (!prepare_decompose_mixed_result(recipe, source_cfg, source_physical < 0 ? size_t(-1) : size_t(source_physical), colours,
                                        decompose_types, physical_cfg, mixed, missing))
        return InvokeResult::error(WAB_INTERNAL, "the recommended mix could not be turned into a mixed filament");
    // The dialog blocks OK when the new filaments and the mixed one would pass the limit.
    const size_t limit = size_t(EnforcerBlockerType::ExtruderMax);
    if (b.filament_presets.size() + missing.size() + 1 > limit)
        return InvokeResult::error(WAB_BAD_PARAMS, "the project would pass " + std::to_string(limit) + " filaments; remove unused ones first",
                                   {{"reason", "filament-limit"}});

    json plan = json::array();
    for (size_t i = 0; i < recipe.components.size(); ++i) {
        const DecomposeComponent& c = recipe.components[i];
        json item = {{"colour", hex_of(c.colour)}, {"ratio", c.ratio}};
        if (c.base_color != DecomposeBaseColor::None)
            item["base"] = decompose_base_color_en(c.base_color);
        auto miss = std::find_if(missing.begin(), missing.end(), [i](const DecomposeMissingComponent& m) { return m.component_idx == i; });
        if (miss != missing.end())
            item["new"] = std::string(miss->display_name.ToUTF8().data());
        else if (i < mixed.components.size() && mixed.components[i] >= 1)
            item["slot"] = int(mixed.components[i]) - 1;
        plan.push_back(std::move(item));
    }
    json result = {{"mode", mode}, {"materialType", type}, {"target", hex_of(target)}, {"matchedColour", hex_of(recipe.matched_color)},
                   {"components", plan}};
    if (ctx.params.value("dryRun", false))
        return InvokeResult::ok(result).with_effects({});

    if (!missing.empty()) {
        ConfirmRequest req;
        req.title   = "Decompose Color";
        req.message = "The mix needs " + std::to_string(missing.size()) + " filament(s) the project does not have. Add them as new filament "
                      "slots, then create the mixed filament?";
        for (const DecomposeMissingComponent& m : missing)
            req.details.push_back(m.display_name.ToUTF8().data());
        req.accept_label = "Add filaments";
        req.destructive  = false;
        if (!ctx.confirm(req))
            return InvokeResult::error(WAB_USER_CANCELLED, "the person did not add the missing filaments");
    }

    json created = json::array();
    for (const DecomposeMissingComponent& m : missing) {
        const size_t before = sidebar.combos_filament().size();
        sidebar.add_custom_filament(wxColour(wxString::FromUTF8(m.official_component.color_hex.c_str())), m.preset_name, true);
        if (sidebar.combos_filament().size() <= before)
            return InvokeResult::error(WAB_INTERNAL, "a missing filament could not be added", {{"created", created}, {"slots", slots_json()}});
        set_created_standard_component_metadata(before, m.official_component);
        if (m.component_idx < mixed.components.size())
            mixed.components[m.component_idx] = unsigned(before + 1);
        created.push_back({{"slot", before}, {"preset", b.filament_presets[before]}, {"colour", m.official_component.color_hex}});
    }
    if (!missing.empty())
        sidebar.collect_physical_filament_info(colours, names, types, &physical_cfg);

    const size_t new_idx = b.filament_presets.size();
    if (!sidebar.add_mixed_filament_from_result(mixed, colours))
        return InvokeResult::error(WAB_INTERNAL, "the mixed filament could not be added", {{"created", created}, {"slots", slots_json()}});
    sidebar.update_filaments_counter();

    json comps = json::array();
    for (unsigned int id : mixed.components)
        comps.push_back(int(id) - 1);
    result["slot"]       = new_idx;
    result["components"] = comps;
    result["ratios"]     = mixed.ratios;
    result["created"]    = created;
    result["slots"]      = slots_json();
    if (const auto* fc = b.project_config.option<ConfigOptionStrings>("filament_colour"); fc && new_idx < fc->values.size())
        result["colour"] = fc->values[new_idx];
    return InvokeResult::ok(result);
}

// ---- printer.setNozzle / bed.setType ---------------------------------------------------------

InvokeResult run_set_nozzle(InvokeContext& ctx)
{
    if (ctx.plater() == nullptr || wxGetApp().preset_bundle == nullptr)
        return InvokeResult::error(WAB_INTERNAL, "the plater is not available");
    const std::string wanted  = nozzle_diameter_string(ctx.params.value("diameterMm", 0.4));
    Preset&           printer = bundle().printers.get_edited_preset();
    if (const auto* dia = printer.config.option<ConfigOptionFloats>("nozzle_diameter"); dia && !dia->values.empty()) {
        if (nozzle_diameter_string(dia->values[0]) == wanted)
            return InvokeResult::ok({{"diameterMm", wanted}, {"printer", printer.name}, {"changed", false}}).with_effects({});
    }
    Preset* target = bundle().get_similar_printer_preset({}, wanted);
    if (target == nullptr)
        return InvokeResult::error(WAB_NOT_FOUND, "this printer has no " + wanted + " mm nozzle variant installed",
                                   {{"path", "/params/diameterMm"}});
    // A printer switch: the printer's edits and the process and filament ones it asks about.
    InvokeResult refusal;
    if (!ctx.preset_switch_preflight(Preset::TYPE_PRINTER, target->name, refusal))
        return refusal;
    target = bundle().get_similar_printer_preset({}, wanted); // a "discard"/"save" may have moved the collection
    if (target == nullptr)
        return InvokeResult::error(WAB_NOT_FOUND, "this printer has no " + wanted + " mm nozzle variant installed");
    target->is_visible     = true; // as the sidebar's nozzle box does
    const std::string name = target->name;
    if (!wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(name))
        return InvokeResult::error(WAB_USER_CANCELLED, "the printer preset was not switched");
    return InvokeResult::ok({{"diameterMm", wanted}, {"printer", bundle().printers.get_selected_preset_name()}, {"changed", true}});
}

struct BedName
{
    const char* wire;
    BedType     type;
};
const BedName BED_NAMES[] = {
    {"coolPlate", btPC},         {"engineeringPlate", btEP},   {"highTempPlate", btPEI},
    {"texturedPeiPlate", btPTE}, {"texturedCoolPlate", btPCT}, {"supertackPlate", btSuperTack},
};

const char* bed_wire_name(BedType t)
{
    for (const BedName& b : BED_NAMES)
        if (b.type == t)
            return b.wire;
    return "default";
}

InvokeResult run_set_bed_type(InvokeContext& ctx)
{
    InvokeResult out;
    if (refuse_in_gcode_preview(ctx, out))
        return out;
    const std::string wire = ctx.params.value("type", std::string());
    BedType           type = btCount;
    for (const BedName& b : BED_NAMES)
        if (wire == b.wire)
            type = b.type;
    Sidebar&                    sidebar = ctx.plater()->sidebar();
    const std::vector<BedType>& offered = sidebar.get_cur_combox_bed_types();
    if (type == btCount || std::find(offered.begin(), offered.end(), type) == offered.end()) {
        json names = json::array();
        for (BedType t : offered)
            names.push_back(bed_wire_name(t));
        return InvokeResult::error(WAB_BAD_PARAMS, "this printer does not offer that plate type", {{"path", "/params/type"}, {"offered", names}});
    }
    if (bundle().project_config.opt_enum<BedType>("curr_bed_type") == type)
        return InvokeResult::ok({{"type", wire}, {"changed", false}}).with_effects({});
    const std::string previous = project_value("curr_bed_type");
    sidebar.set_bed_type_accord_combox(type); // SelectAndNotify: the combo's own handler applies it
    note_project_write(ctx, "curr_bed_type", previous);
    return InvokeResult::ok({{"type", bed_wire_name(bundle().project_config.opt_enum<BedType>("curr_bed_type"))}, {"changed", true}});
}

} // namespace

void register_filament_actions(Catalog& c)
{
    {
        ActionDescriptor d;
        d.id       = "filament.delete";
        d.label    = "Remove filament";
        d.summary  = "Remove a filament slot from the project; objects printing with it move to replaceWith (or their default). "
                     "The person confirms in a slicer dialog.";
        d.risk     = Risk::Destructive;
        d.params   = schema({{"slot", slot_schema("0-based slot to remove (physical or mixed).")},
                             {"replaceWith", slot_schema("0-based slot that takes over its objects and painting; absent = each object's default.")}},
                            {"slot"});
        d.effects  = {"presets", "settings", "structure", "paint", "plates"};
        d.ui       = {"Plater.cpp::sidebar::del_btn", "Plater.cpp::sidebar::m_btn_mixed_del", "Plater.cpp::sidebar::menu_btn"};
        d.examples = example({{"slot", 1}, {"replaceWith", 0}});
        c.add(std::move(d), run_delete, [](const InvokeContext&) { return enabled_needs_slots(2); });
    }
    {
        ActionDescriptor d;
        d.id      = "filament.removeSlots";
        d.label   = "Remove filaments";
        d.summary = "Remove several filament slots at once (slots, or keep = every other slot goes); the person confirms ONE "
                    "slicer dialog for all of them. Use instead of repeating filament.delete.";
        d.risk    = Risk::Destructive;
        json slot_list = {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", slot_schema("0-based slot.")}};
        json slots     = slot_list;
        slots["description"] = "0-based slots to remove (physical or mixed), each once.";
        json keep            = slot_list;
        keep["description"]  = "0-based slots to keep; every other slot is removed.";
        d.params = schema({{"slots", slots},
                           {"keep", keep},
                           {"replaceWith", slot_schema("0-based slot (one that stays) that takes over their objects and painting; "
                                                       "absent = each object's default.")}});
        d.params["x-exactlyOne"] = json::array({json::array({"slots"}), json::array({"keep"})});
        d.effects  = {"presets", "settings", "structure", "paint", "plates"};
        d.examples = example({{"keep", json::array({0})}});
        d.since    = "0.1.8";
        c.add(std::move(d), run_remove_slots, [](const InvokeContext&) { return enabled_needs_slots(2); });
    }
    {
        ActionDescriptor d;
        d.id       = "filament.merge";
        d.label    = "Merge with";
        d.summary  = "Merge one filament slot into another: the first is removed and everything printed with it prints with the "
                     "second. The person confirms in a slicer dialog.";
        d.risk     = Risk::Destructive;
        d.params   = schema({{"from", slot_schema("0-based slot that goes away.")}, {"to", slot_schema("0-based slot that takes over.")}},
                            {"from", "to"});
        d.effects  = {"presets", "settings", "structure", "paint", "plates"};
        d.ui       = {"GUI_Factories.cpp::submenu::Merge with", "GUI_Factories.cpp::menu::expr:item_name@create_filament_action_menu"};
        d.examples = example({{"from", 1}, {"to", 0}});
        c.add(std::move(d), run_merge, [](const InvokeContext&) { return enabled_needs_slots(2); });
    }
    {
        ActionDescriptor d;
        d.id       = "filament.setColour";
        d.label    = "Change filament colour";
        d.summary  = "Set the display colour of a physical filament slot (what the plate, the preview and flushing volumes use).";
        d.risk     = Risk::Mutate;
        d.params   = schema({{"slot", slot_schema("0-based physical slot.")},
                             {"colour", {{"type", "string"}, {"minLength", 3}, {"maxLength", 7}, {"description", "Hex colour, e.g. \"#FF8800\"."}}}},
                            {"slot", "colour"});
        d.effects  = {"settings", "view"};
        d.ui       = {"PresetComboBoxes.cpp::menu::Change extruder color"};
        d.examples = example({{"slot", 0}, {"colour", "#FF8800"}});
        c.add(std::move(d), run_set_colour);
    }
    {
        ActionDescriptor d;
        d.id       = "filament.getFlushVolumes";
        d.label    = "Flushing volumes";
        d.summary  = "Read the flushing-volume matrix (mm3 purged going from the row slot to the column slot) and the multiplier, per nozzle.";
        d.risk     = Risk::Read;
        d.effects  = {};
        d.ui       = {"Plater.cpp::sidebar::m_flushing_volume_btn"};
        d.examples = example(json::object());
        c.add(std::move(d), run_get_flush);
    }
    {
        ActionDescriptor d;
        d.id      = "filament.setFlushVolumes";
        d.label   = "Flushing volumes";
        d.summary = "Set the flushing volumes: \"auto\" recalculates them from the colours (the dialog's auto-calculation); "
                    "\"matrix\" writes a p x p matrix over the physical slots (row = from, column = to, mm3).";
        d.risk    = Risk::Mutate;
        d.params  = schema(
            {{"mode", {{"type", "string"}, {"enum", {"auto", "matrix"}}, {"default", "auto"}}},
             {"matrix",
              {{"type", "array"},
               {"minItems", 2},
               {"maxItems", 64},
               {"items", {{"type", "array"}, {"minItems", 2}, {"maxItems", 64}, {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 9999}}}}},
               {"description", "Rows and columns in physical slot order; the diagonal is ignored."}}},
             {"multiplier", {{"type", "number"}, {"minimum", 0}, {"maximum", 3}, {"description", "Flushing multiplier of that nozzle."}}},
             {"nozzle", {{"type", "integer"}, {"minimum", 0}, {"maximum", 7}, {"default", 0}, {"description", "Extruder whose matrix is set."}}}},
            {"mode"});
        d.effects  = {"settings"};
        d.ui       = {"Plater.cpp::sidebar::m_flushing_volume_btn"};
        d.examples = example({{"mode", "auto"}});
        c.add(std::move(d), run_set_flush, [](const InvokeContext&) { return enabled_needs_slots(2); });
    }
    {
        ActionDescriptor d;
        d.id       = "filament.setPurgeMode";
        d.label    = "Purge mode";
        d.summary  = "Choose the purge mode on printers that offer one: default, saving (multi-nozzle extruders) or fast.";
        d.risk     = Risk::Mutate;
        d.params   = schema({{"mode", {{"type", "string"}, {"enum", {"default", "saving", "fast"}}}}}, {"mode"});
        d.effects  = {"settings"};
        d.ui       = {"Plater.cpp::sidebar::m_purge_mode_btn"};
        d.examples = example({{"mode", "default"}});
        c.add(std::move(d), run_set_purge_mode, [](const InvokeContext&) {
            const PurgeSupport s = purge_support();
            return (s.multiple_nozzle || s.fast) ? std::string() : std::string("this printer has no purge modes");
        });
    }
    {
        ActionDescriptor d;
        d.id      = "filament.addMixed";
        d.label   = "Add mixed filament";
        d.summary = "Add a mixed (virtual) filament that alternates two or more physical filaments by layer share, optionally as a "
                    "two-filament gradient over the height.";
        d.risk    = Risk::Mutate;
        d.params  = schema(
            {{"components", {{"type", "array"}, {"minItems", 2}, {"maxItems", 8}, {"items", slot_schema("0-based physical slot.")}}},
             {"ratios",
              {{"type", "array"},
               {"minItems", 2},
               {"maxItems", 8},
               {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
               {"description", "Relative share of each component in percent; absent = equal shares."}}},
             {"gradient", {{"type", "boolean"}, {"default", false}, {"description", "Blend from the first to the second component over the height."}}},
             {"gradientReverse", {{"type", "boolean"}, {"default", false}, {"description", "Blend from the second to the first instead."}}},
             {"gradientPerPart", {{"type", "boolean"}, {"default", false}, {"description", "Run the gradient over each part's own height."}}}},
            {"components"});
        d.effects  = {"presets", "settings"};
        d.ui       = {"Plater.cpp::sidebar::m_btn_mixed_add", "Plater.cpp::sidebar::icon_add"};
        d.examples = example({{"components", {0, 1}}, {"ratios", {50, 50}}});
        c.add(std::move(d), run_add_mixed, [](const InvokeContext&) { return enabled_needs_slots(2); });
    }
    {
        ActionDescriptor d;
        d.id      = "filament.editMixed";
        d.label   = "Edit mixed filament";
        d.summary = "Change a mixed filament in place (components, layer shares, gradient): it keeps its slot, so everything printing "
                    "with it still does. Anything not given stays as it is.";
        d.risk    = Risk::Mutate;
        d.params  = schema(
            {{"slot", slot_schema("0-based slot of the mixed filament.")},
             {"components", {{"type", "array"}, {"minItems", 2}, {"maxItems", 8}, {"items", slot_schema("0-based physical slot.")}}},
             {"ratios",
              {{"type", "array"},
               {"minItems", 2},
               {"maxItems", 8},
               {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
               {"description", "Relative share of each component in percent; absent with new components = equal shares."}}},
             {"gradient", {{"type", "boolean"}, {"description", "Blend from the first to the second component over the height (two components only)."}}},
             {"gradientReverse", {{"type", "boolean"}, {"description", "Blend from the second to the first instead."}}},
             {"gradientPerPart", {{"type", "boolean"}, {"description", "Run the gradient over each part's own height."}}}},
            {"slot"});
        d.effects  = {"settings"};
        d.ui       = {"Plater.cpp::sidebar::menu_btn"};
        d.examples = json::array({json{{"target", nullptr},
                                       {"params", {{"slot", 2}, {"ratios", {70, 30}}}},
                                       {"setup", json::array({json{{"id", "filament.addMixed"}, {"params", {{"components", {0, 1}}}}}})}}});
        c.add(std::move(d), run_edit_mixed, [](const InvokeContext&) {
            if (wxGetApp().preset_bundle == nullptr)
                return std::string("presets are not loaded");
            return bundle().num_mixed_filaments() > 0 ? std::string() : std::string("the project has no mixed filament");
        });
    }
    {
        ActionDescriptor d;
        d.id      = "filament.decomposeColour";
        d.label   = "Decompose Color";
        d.summary = "Turn a colour (a filament slot's, or one given) into a mixed filament of printable filaments, as Decompose Color "
                    "does: from the project's filaments (materialList) or standard CMYW/RYBW PLA Basic colours. Standard colours the "
                    "project lacks are added as new slots after the person agrees in a slicer dialog. dryRun only reports the mix.";
        d.risk    = Risk::Mutate;
        d.params  = schema(
            {{"slot", slot_schema("0-based physical slot whose colour to decompose (it is left out of the mix).")},
             {"colour", {{"type", "string"}, {"minLength", 3}, {"maxLength", 7}, {"description", "Hex colour to match instead of the slot's."}}},
             {"materialType",
              {{"type", "string"}, {"minLength", 1}, {"maxLength", 64},
               {"description", "The dialog's material: a type of the project's filaments, or \"PLA Basic\"; absent = the slot's type."}}},
             {"mode", {{"type", "string"}, {"enum", {"materialList", "cmyw", "rybw"}},
                       {"description", "Which recommendation; absent = the first the dialog offers for the material."}}},
             {"dryRun", {{"type", "boolean"}, {"default", false}, {"description", "Report the recommended mix without changing anything."}}}});
        d.effects  = {"presets", "settings"};
        d.ui       = {"GUI_Factories.cpp::menu::Decompose Color"};
        d.examples = example({{"slot", 0}, {"materialType", "PLA Basic"}, {"mode", "cmyw"}, {"dryRun", true}});
        c.add(std::move(d), run_decompose_colour, [](const InvokeContext&) {
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr || wxGetApp().preset_bundle == nullptr)
                return std::string("presets are not loaded");
            return plater->sidebar().combos_filament().size() >= 2 ? std::string() : std::string("the project has fewer than 2 filaments");
        });
    }
    {
        ActionDescriptor d;
        d.id       = "printer.setNozzle";
        d.label    = "Nozzle";
        d.summary  = "Switch to this printer's preset for another nozzle diameter, as the sidebar's nozzle box does (honours ifDirty).";
        d.risk     = Risk::Mutate;
        d.params   = schema({{"diameterMm", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 2.0}, {"description", "Nozzle diameter in mm (0.2, 0.4, 0.6 ...)."}}}},
                            {"diameterMm"});
        d.effects  = {"presets", "settings"};
        d.ui       = {"Plater.cpp::sidebar::combo_nozzle_dia", "Plater.cpp::sidebar::single_extruder->combo_diameter",
                      "Plater.cpp::sidebar::left_extruder->combo_diameter", "Plater.cpp::sidebar::right_extruder->combo_diameter"};
        d.examples = example({{"diameterMm", 0.4}});
        c.add(std::move(d), run_set_nozzle);
    }
    {
        ActionDescriptor d;
        d.id      = "bed.setType";
        d.label   = "Plate type";
        d.summary = "Set the project's build plate type (the sidebar's plate box); plates given their own type keep it.";
        d.risk    = Risk::Mutate;
        d.params  = schema({{"type",
                            {{"type", "string"},
                             {"enum", {"coolPlate", "engineeringPlate", "highTempPlate", "texturedPeiPlate", "texturedCoolPlate", "supertackPlate"}}}}},
                          {"type"});
        d.effects  = {"settings", "view"};
        d.ui       = {"Plater.cpp::sidebar::combo_printer_bed"};
        d.examples = example({{"type", "texturedPeiPlate"}});
        c.add(std::move(d), run_set_bed_type);
    }
}

}}} // namespace Slic3r::GUI::Actions
