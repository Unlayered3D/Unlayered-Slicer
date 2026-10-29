// Automation actions: slice.* that are not typed bridge methods (slice.run, slice.result,
// slice.modes and slice.setMode are, ExistingMethods.cpp; slice.all is PlateActions.cpp).
// See docs/HLSD/bridge-actions.md for how to write an action and docs/bridge-actions/slicing.md
// for what these do.
//
// Decision D7 shapes all of them: automation may preselect a slicing mode and change the slicing
// service's options, and it may stop a run, but it never starts a slicing-service (cloud) run.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "Register.hpp"

#include "libslic3r/PresetBundle.hpp"

#include "../GUI_App.hpp"
#include "../MainFrame.hpp"
#include "../Plater.hpp"
#include "../SliceServiceOptionsDialog.hpp"
#include "../WebAppBridge.hpp"
#include "slic3r/Utils/NonplanarSliceClient.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;

namespace {

json object_schema(json properties, std::vector<std::string> required = {})
{
    json s = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty())
        s["required"] = required;
    return s;
}

json one_example(json params) { return json::array({{{"target", nullptr}, {"params", std::move(params)}}}); }

std::string enabled_with_plater(const InvokeContext&)
{
    return wxGetApp().plater() != nullptr ? std::string() : std::string("the slicer is starting");
}

// ---------------------------------------------------------------------------------------------

// slice.cancel: the slicing notification's Cancel (and, for a service job, the progress bar's).
InvokeResult run_cancel(InvokeContext& ctx)
{
    bool local = false, service = false;
    ctx.plater()->cancel_slicing(local, service);
    return InvokeResult::ok({{"cancelled", local || service}, {"local", local}, {"service", service}});
}

// slice.getOptions / slice.setOptions: the Slice dropdown's "Slicing service options..." dialog,
// minus the dialog. Only the cached form is read (never SliceService::refresh_schema_now, which
// blocks on the network). No entitlement check: whether the service takes a job is its decision.
bool options_preflight(InvokeResult& out, json& schema)
{
    if (!NonplanarSliceClient::is_configured()) {
        out = InvokeResult::error(WAB_NOT_FOUND, "no slicing service is configured in this slicer", {{"reason", "no_service"}});
        return false;
    }
    schema = SliceService::cached_schema();
    if (!schema.is_object()) {
        // Fetched in the background, as opening the Slice dropdown does; nothing is sliced.
        SliceService::refresh_schema_async();
        out = InvokeResult::error(WAB_NOT_FOUND, "the service's options form has not been fetched yet; try again in a few seconds",
                                  {{"reason", "no_form"}});
        return false;
    }
    return true;
}

json options_state(const json& schema)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    MainFrame*    frame  = wxGetApp().mainframe;
    return {{"service", std::string(SliceService::display_name().ToUTF8().data())},
            {"printer", bundle != nullptr ? bundle->printers.get_selected_preset_name() : std::string()},
            {"axes", SliceService::machine_axes()},
            {"facts", SliceService::machine_facts()},
            {"mode", SliceService::current_mode()},
            {"serviceModeSelected", frame != nullptr && frame->slice_mode_is_service()},
            {"fields", SliceService::describe_options(schema)},
            {"stored", SliceService::saved_options()}};
}

InvokeResult run_get_options(InvokeContext&)
{
    InvokeResult out;
    json         schema;
    if (!options_preflight(out, schema))
        return out;
    return InvokeResult::ok(options_state(schema));
}

InvokeResult run_set_options(InvokeContext& ctx)
{
    InvokeResult out;
    json         schema;
    if (!options_preflight(out, schema))
        return out;
    const bool reset   = ctx.params.value("reset", false);
    json       changes = json::object();
    for (const json& v : ctx.params.value("values", json::array())) {
        const std::string dest = v.value("dest", std::string());
        if (changes.contains(dest))
            return InvokeResult::error(WAB_BAD_PARAMS, "\"" + dest + "\" is given twice", {{"path", "/params/values"}});
        changes[dest] = v.contains("choice") ? v["choice"] : v.contains("number") ? v["number"] : v.contains("on") ? v["on"] : json();
    }
    if (!reset && changes.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "give values to change, or reset:true", {{"path", "/params/values"}});
    const std::string before_mode = SliceService::current_mode();
    const json        before       = SliceService::saved_options();
    json              stored;
    std::string       bad, why;
    // Reset is the dialog's Reset then OK: back to the service's defaults (the remembered choices
    // are erased), with `values` applied on top when given.
    if (!SliceService::options_to_store(schema, reset ? json::object() : before, changes, SliceService::machine_facts(), SliceService::mode_options(), stored, bad, why))
        return InvokeResult::error(WAB_BAD_PARAMS, bad.empty() ? why : "\"" + bad + "\": " + why,
                                   {{"path", bad.empty() ? std::string("/params/values") : "/params/values/" + bad}});
    SliceService::save_options(reset && changes.empty() ? json::object() : stored);
    const bool mode_changed = SliceService::current_mode() != before_mode;
    if (MainFrame* frame = wxGetApp().mainframe; frame != nullptr)
        frame->slice_service_options_changed(/*by_person=*/false, mode_changed);
    json result       = options_state(schema);
    result["changed"] = SliceService::saved_options() != before;
    result["note"]    = "a service result of the current plate made with other options is now out of date; only the person's Slice makes a new one";
    return InvokeResult::ok(result);
}

} // namespace

void register_slice_actions(Catalog& c)
{
    {
        ActionDescriptor d;
        d.id      = "slice.cancel";
        d.label   = "Cancel slicing";
        d.summary = "Stop the slice in progress, as the slicing notification's Cancel does: a local slice (and the rest of a Slice all), "
                    "or a slicing-service (cloud) job, which the service is told to stop. Starts nothing; cancelled:false when nothing was running.";
        // Only stops work; changes no model, setting or file, so it runs while slicing is busy.
        d.risk     = Risk::Ui;
        d.params   = object_schema(json::object());
        d.effects  = {"plates"};
        d.examples = one_example(json::object());
        c.add(std::move(d), run_cancel, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id      = "slice.getOptions";
        d.label   = "Slicing service options";
        d.summary = "The slicing service's options for the selected printer, as the Slice dropdown's Slicing service options dialog shows "
                    "them: each field with its type, choices, range, default, current value and whether it applies.";
        d.risk     = Risk::Read;
        d.params   = object_schema(json::object());
        d.ui       = {"MainFrame.cpp::sidebutton::Slicing service options"};
        d.examples = one_example(json::object());
        c.add(std::move(d), run_get_options, enabled_with_plater);
    }
    {
        ActionDescriptor d;
        d.id      = "slice.setOptions";
        d.label   = "Slicing service options";
        d.summary = "Change the slicing service's options for the selected printer (fields by dest from slice.getOptions), or reset:true "
                    "for the service's defaults, as the Slicing service options dialog's OK and Reset do. Slices nothing and sends nothing; the "
                    "person's next Slice uses them.";
        d.risk     = Risk::Mutate;
        json value = object_schema({{"dest", {{"type", "string"}, {"minLength", 1}, {"maxLength", 100}, {"description", "The field's dest."}}},
                                    {"choice", {{"type", "string"}, {"maxLength", 200}, {"description", "An enum field's choice value."}}},
                                    {"number", {{"type", "number"}, {"description", "A number field's value."}}},
                                    {"on", {{"type", "boolean"}, {"description", "A bool field's value."}}},
                                    {"toDefault", {{"type", "boolean"}, {"description", "Back to the service's default (blank for an optional number)."}}}},
                                   {"dest"});
        value["x-exactlyOne"] = json::array({json::array({"choice"}), json::array({"number"}), json::array({"on"}), json::array({"toDefault"})});
        d.params   = object_schema(
            {{"values", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", value}}},
             {"reset", {{"type", "boolean"}, {"default", false}, {"description", "Erase the remembered options first (the dialog's Reset)."}}}});
        d.effects  = {"settings"};
        d.ui       = {"MainFrame.cpp::sidebutton::Slicing service options"};
        d.examples = one_example({{"reset", true}});
        c.add(std::move(d), run_set_options, enabled_with_plater);
    }
}

}}} // namespace Slic3r::GUI::Actions
