#include "SliceServiceOptionsDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/NonplanarSliceClient.hpp"

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include <wx/busyinfo.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <cmath>
#include <thread>

using json   = nlohmann::json;
namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

const char* SECTION      = "slice_service";      // per printer: the options dialog's choices
const char* MODE_SECTION = "slice_service_mode"; // per printer: the selected mode's id

fs::path cache_dir() { return fs::path(data_dir()) / "cache" / "slice_service"; }
fs::path schema_path() { return cache_dir() / "schema.json"; }
fs::path info_path() { return cache_dir() / "info.json"; }

void write_cache(const fs::path& dest, const std::string& body)
{
    boost::system::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = fs::path(dest).replace_extension(".tmp");
    {
        fs::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        out.write(body.data(), std::streamsize(body.size()));
        if (!out.good())
            return;
    }
    fs::rename(tmp, dest, ec);
}

json read_cache(const fs::path& p)
{
    boost::system::error_code ec;
    if (!fs::is_regular_file(p, ec))
        return json();
    try {
        fs::ifstream in(p, std::ios::in | std::ios::binary);
        json         j = json::parse(in);
        if (j.is_object())
            return j;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "SliceService: cached " << p.filename().string() << " unreadable: " << e.what();
    }
    return json();
}

std::string selected_printer()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    return bundle != nullptr ? bundle->printers.get_selected_preset_name() : std::string();
}

json saved_for(const std::string& printer)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || printer.empty())
        return json::object();
    try {
        const std::string raw = cfg->get(SECTION, printer);
        if (!raw.empty()) {
            json j = json::parse(raw);
            if (j.is_object())
                return j;
        }
    } catch (const std::exception&) {
    }
    return json::object();
}

void save_for(const std::string& printer, const json& values)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || printer.empty())
        return;
    if (values.empty())
        cfg->erase(SECTION, printer);
    else
        cfg->set(SECTION, printer, values.dump());
}

// The name a condition or a default table uses for a fact or a field. `axes` is the deprecated
// spelling of the documented fact `machine_axes`, still accepted from older services.
std::string canonical_key(const std::string& key) { return key == "axes" ? std::string("machine_axes") : key; }

// A table key or a condition value against an actual value: equal JSON, or the same text when one
// side is a string ("5" in a JSON object key against the number 5).
bool values_equal(const json& want, const json& have)
{
    if (want == have)
        return true;
    auto text = [](const json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); };
    return (want.is_string() || have.is_string()) && text(want) == text(have);
}

// show_if: every listed condition must hold. A condition names another field (its current value)
// or a machine fact, and gives one value or an array of values any of which will do. A condition
// on something neither a field nor a fact never holds.
bool condition_holds(const json& cond, const json& values, const json& facts)
{
    if (!cond.is_object())
        return true;
    for (auto it = cond.begin(); it != cond.end(); ++it) {
        const std::string key  = canonical_key(it.key());
        const json*       have = facts.contains(key) ? &facts.at(key) : values.contains(key) ? &values.at(key) : nullptr;
        if (have == nullptr)
            return false;
        bool hit = false;
        if (it.value().is_array()) {
            for (const json& w : it.value())
                hit = hit || values_equal(w, *have);
        } else
            hit = values_equal(it.value(), *have);
        if (!hit)
            return false;
    }
    return true;
}

// A choice is offered on this machine unless its own show_if (machine facts only) says otherwise.
bool choice_offered(const json& choice, const json& facts)
{
    if (choice.contains("show_if"))
        return condition_holds(choice.at("show_if"), json::object(), facts);
    // Deprecated: {"axes": n} on the choice itself, the spelling before show_if.
    if (choice.contains("axes") && choice.at("axes").is_number_integer())
        return facts.contains("machine_axes") && values_equal(choice.at("axes"), facts.at("machine_axes"));
    return true;
}

// default_by: {"<fact>": {"<fact value>": default, ...}}, first fact that has an entry wins; else
// `default`. The deprecated `default_by_axes: {"3": v, "5": v}` is read as
// default_by: {"machine_axes": {...}}.
json field_default(const json& field, const json& facts)
{
    json by = field.contains("default_by") && field.at("default_by").is_object() ? field.at("default_by") : json::object();
    if (!by.contains("machine_axes") && field.contains("default_by_axes") && field.at("default_by_axes").is_object())
        by["machine_axes"] = field.at("default_by_axes");
    for (auto it = by.begin(); it != by.end(); ++it) {
        const std::string fact = canonical_key(it.key());
        if (!facts.contains(fact) || !it.value().is_object())
            continue;
        const json& have = facts.at(fact);
        for (auto row = it.value().begin(); row != it.value().end(); ++row)
            if (values_equal(json(row.key()), have))
                return row.value();
    }
    return field.value("default", json());
}

bool field_applies(const json& field, const json& values, const json& facts)
{
    return !field.contains("show_if") || condition_holds(field.at("show_if"), values, facts);
}

// `value` is acceptable for `field` on this machine.
bool value_valid(const json& field, const json& value, const json& facts)
{
    const std::string type = field.value("type", std::string());
    if (type == "enum") {
        if (!value.is_string() || !field.contains("choices"))
            return false;
        for (const json& c : field.at("choices"))
            if (c.value("value", std::string()) == value.get<std::string>())
                return choice_offered(c, facts);
        return false;
    }
    if (type == "bool")
        return value.is_boolean();
    if (type == "number") {
        if (!value.is_number())
            return false;
        const double v = value.get<double>();
        if (field.contains("min") && field.at("min").is_number() && v < field.at("min").get<double>())
            return false;
        if (field.contains("max") && field.at("max").is_number() && v > field.at("max").get<double>())
            return false;
        return std::isfinite(v);
    }
    return false;
}

const json& fields_of(const json& schema)
{
    static const json none = json::array();
    return schema.is_object() && schema.contains("fields") && schema.at("fields").is_array() ? schema.at("fields") : none;
}

// Every field's value as a job would see it: what the selected mode fixes, else the saved value
// when still valid, else the field's default. Conditions are judged against these.
json effective_values(const json& schema, const json& saved, const json& facts, const json& pins)
{
    json out = json::object();
    for (const json& f : fields_of(schema)) {
        const std::string dest = f.value("dest", std::string());
        if (dest.empty())
            continue;
        if (pins.contains(dest))
            out[dest] = pins.at(dest);
        else if (saved.contains(dest) && value_valid(f, saved.at(dest), facts))
            out[dest] = saved.at(dest);
        else
            out[dest] = field_default(f, facts);
    }
    return out;
}

// The modes in GET / that are offered on this machine, as JSON entries ({id, label, options?}).
std::vector<json> offered_modes(const json& facts)
{
    std::vector<json> out;
    const json        info = SliceService::cached_info();
    if (!info.is_object() || !info.contains("modes") || !info.at("modes").is_array())
        return out;
    for (const json& m : info.at("modes")) {
        if (!m.is_object() || m.value("id", std::string()).empty())
            continue;
        if (m.contains("show_if") && !condition_holds(m.at("show_if"), json::object(), facts))
            continue;
        out.push_back(m);
    }
    return out;
}

// The option values the selected mode fixes: its `options` object, or none.
json mode_pins()
{
    const std::string id = SliceService::current_mode();
    for (const json& m : offered_modes(SliceService::machine_facts()))
        if (m.value("id", std::string()) == id && m.contains("options") && m.at("options").is_object())
            return m.at("options");
    return json::object();
}

wxString number_text(const json& v)
{
    if (!v.is_number())
        return {};
    const double d = v.get<double>();
    return d == std::floor(d) ? wxString::Format("%.0f", d) : wxString::Format("%g", d);
}

} // namespace

// ---------------------------------------------------------------------------------------------

namespace SliceService {

json cached_schema()
{
    json j = read_cache(schema_path());
    return j.is_object() && j.contains("fields") && j.at("fields").is_array() ? j : json();
}

json cached_info() { return read_cache(info_path()); }

void refresh_schema_async()
{
    if (!NonplanarSliceClient::is_configured())
        return;
    const std::string url = NonplanarSliceClient::configured_url();
    std::thread([url]() {
        NonplanarSliceClient client(url);
        std::string          body, error;
        if (client.fetch_schema(body, error))
            write_cache(schema_path(), body);
        else
            BOOST_LOG_TRIVIAL(info) << "SliceService: form not refreshed: " << error;
        body.clear();
        if (client.fetch_info(body, error))
            write_cache(info_path(), body);
        else
            BOOST_LOG_TRIVIAL(info) << "SliceService: description not refreshed: " << error;
    }).detach();
}

bool refresh_schema_now(std::string& error)
{
    if (!NonplanarSliceClient::is_configured()) {
        error = "no slicing service is configured";
        return false;
    }
    wxBusyCursor         busy;
    NonplanarSliceClient client(NonplanarSliceClient::configured_url());
    std::string          body, info_error;
    if (client.fetch_info(body, info_error))
        write_cache(info_path(), body);
    body.clear();
    if (!client.fetch_schema(body, error))
        return false;
    write_cache(schema_path(), body);
    return true;
}

wxString display_name()
{
    for (const json& j : {cached_info(), cached_schema()}) {
        for (const char* key : {"display_name", "title"}) {
            if (j.is_object() && j.contains(key) && j.at(key).is_string()) {
                const std::string t = j.at(key).get<std::string>();
                if (!t.empty())
                    return from_u8(t);
            }
        }
    }
    return _L("Slice with service");
}

int machine_axes()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return 3;
    const DynamicPrintConfig& cfg = bundle->printers.get_edited_preset().config;
    const auto*               np  = cfg.option<ConfigOptionBool>("enable_nonplanar");
    const auto*               sel = cfg.option<ConfigOptionEnum<AxisSelection>>("axis_selection");
    // Tilting needs non-planar on and a real rotary pair.
    const bool rotary = np != nullptr && np->value && (sel == nullptr || sel->value != AxisSelection::Virtual);
    return rotary ? 5 : 3;
}

json machine_facts() { return json{{"machine_axes", machine_axes()}}; }

std::vector<ModeChoice> mode_choices()
{
    std::vector<ModeChoice> out;
    for (const json& m : offered_modes(machine_facts())) {
        const std::string id = m.value("id", std::string());
        out.push_back({id, from_u8(m.value("label", id))});
    }
    return out;
}

std::string current_mode()
{
    const std::vector<ModeChoice> modes = mode_choices();
    if (modes.empty())
        return {};
    AppConfig*        cfg     = wxGetApp().app_config;
    const std::string printer = selected_printer();
    const std::string saved   = cfg != nullptr && !printer.empty() ? cfg->get(MODE_SECTION, printer) : std::string();
    for (const ModeChoice& m : modes)
        if (m.id == saved)
            return saved;
    return modes.front().id;
}

wxString mode_label(const std::string& id)
{
    for (const ModeChoice& c : mode_choices())
        if (c.id == id)
            return c.label;
    return {};
}

void set_mode(const std::string& id)
{
    AppConfig*        cfg     = wxGetApp().app_config;
    const std::string printer = selected_printer();
    if (cfg == nullptr || printer.empty())
        return;
    if (id.empty())
        cfg->erase(MODE_SECTION, printer);
    else
        cfg->set(MODE_SECTION, printer, id);
}

std::string request_options()
{
    const json schema = cached_schema();
    const json pins   = mode_pins();
    const json saved  = saved_for(selected_printer());
    const json facts  = machine_facts();
    const json values = effective_values(schema, saved, facts, pins);
    json       out    = json::object();
    for (const json& f : fields_of(schema)) {
        const std::string dest = f.value("dest", std::string());
        if (dest.empty() || pins.contains(dest) || !saved.contains(dest) || !field_applies(f, values, facts))
            continue;
        if (value_valid(f, saved.at(dest), facts))
            out[dest] = saved.at(dest);
    }
    // What the mode fixes goes along whatever the form says about it.
    for (auto it = pins.begin(); it != pins.end(); ++it)
        out[it.key()] = it.value();
    return out.empty() ? std::string() : out.dump();
}

json saved_options() { return saved_for(selected_printer()); }

void save_options(const json& values) { save_for(selected_printer(), values); }

json mode_options() { return mode_pins(); }

json describe_options(const json& schema)
{
    if (!schema.is_object() || !schema.contains("fields") || !schema.at("fields").is_array())
        return json();
    const json saved  = saved_for(selected_printer());
    const json facts  = machine_facts();
    const json pins   = mode_pins();
    const json values = effective_values(schema, saved, facts, pins);
    json       out    = json::array();
    for (const json& f : fields_of(schema)) {
        const std::string dest = f.value("dest", std::string());
        const std::string type = f.value("type", std::string());
        if (dest.empty() || (type != "enum" && type != "bool" && type != "number"))
            continue; // the dialog does not draw it either
        if (pins.contains(dest))
            continue; // the selected mode decides this one: the dialog does not draw it
        json field = {{"dest", dest}, {"type", type}, {"label", f.value("label", dest)}};
        for (const char* key : {"help", "unit", "min", "max", "optional"})
            if (f.contains(key))
                field[key] = f.at(key);
        if (type == "enum") {
            json choices = json::array();
            for (const json& c : f.value("choices", json::array()))
                if (choice_offered(c, facts))
                    choices.push_back({{"value", c.value("value", std::string())}, {"label", c.value("label", c.value("value", std::string()))}});
            field["choices"] = choices;
        }
        const json def   = field_default(f, facts);
        field["default"] = def;
        field["value"]   = saved.contains(dest) && value_valid(f, saved.at(dest), facts) ? saved.at(dest) : def;
        field["applies"] = field_applies(f, values, facts);
        out.push_back(field);
    }
    return out;
}

bool options_to_store(const json& schema, const json& saved, const json& changes, const json& facts, const json& pins, json& out,
                      std::string& error_dest,
                      std::string& error)
{
    out = json::object();
    if (!schema.is_object() || !schema.contains("fields") || !schema.at("fields").is_array()) {
        error = "the service's options form is not available";
        return false;
    }
    auto find_field = [&schema](const std::string& dest) -> const json* {
        for (const json& f : fields_of(schema))
            if (f.value("dest", std::string()) == dest)
                return &f;
        return nullptr;
    };
    // The values the dialog would hold: remembered and valid, then the changes on top.
    json merged = json::object();
    for (const json& f : fields_of(schema)) {
        const std::string dest = f.value("dest", std::string());
        if (!dest.empty() && saved.is_object() && saved.contains(dest) && value_valid(f, saved.at(dest), facts))
            merged[dest] = saved.at(dest);
    }
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const json*       f    = find_field(it.key());
        const std::string type = f != nullptr ? f->value("type", std::string()) : std::string();
        if (f == nullptr || (type != "enum" && type != "bool" && type != "number")) {
            error_dest = it.key();
            error      = "the service's form has no option \"" + it.key() + "\"";
            return false;
        }
        if (pins.contains(it.key())) {
            error_dest = it.key();
            error      = "this option is fixed by the selected slicing mode";
            return false;
        }
        if (it.value().is_null()) {
            merged.erase(it.key());
            continue;
        }
        if (!value_valid(*f, it.value(), facts)) {
            error_dest = it.key();
            error      = type == "enum"   ? "not one of the choices offered on this printer" :
                         type == "number" ? "not a number in the field's range" :
                                            "not a boolean";
            return false;
        }
        merged[it.key()] = it.value();
    }
    const json values = effective_values(schema, merged, facts, pins);
    for (auto it = changes.begin(); it != changes.end(); ++it)
        if (!it.value().is_null() && !field_applies(*find_field(it.key()), values, facts)) {
            error_dest = it.key();
            error      = "this option does not apply to the selected mode or printer";
            return false;
        }
    // collect(): every applying, drawn field's value, stored only when it differs from the default.
    for (const json& f : fields_of(schema)) {
        const std::string dest = f.value("dest", std::string());
        const std::string type = f.value("type", std::string());
        if (dest.empty() || pins.contains(dest) || (type != "enum" && type != "bool" && type != "number") ||
            !field_applies(f, values, facts))
            continue;
        const json def = field_default(f, facts);
        if (merged.contains(dest) && merged.at(dest) != def)
            out[dest] = merged.at(dest);
    }
    return true;
}

} // namespace SliceService

// ---------------------------------------------------------------------------------------------

SliceServiceOptionsDialog::SliceServiceOptionsDialog(wxWindow* parent, const json& schema)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY,
                wxString::Format(_L("%s options"), SliceService::display_name()), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_schema(schema)
    , m_printer(selected_printer())
    , m_facts(SliceService::machine_facts())
    , m_pins(mode_pins())
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto* v_sizer = new wxBoxSizer(wxVERTICAL);

    auto* intro = new wxStaticText(this, wxID_ANY,
                                   wxString::Format(_L("For printer \"%s\". Everything else comes from the Process, "
                                                       "Filament and Printer settings."),
                                                    from_u8(m_printer)));
    intro->Wrap(FromDIP(380));
    v_sizer->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(16));
    grid->AddGrowableCol(1);

    for (const json& f : fields_of(m_schema)) {
        const std::string dest = f.value("dest", std::string());
        const std::string type = f.value("type", std::string());
        if (dest.empty() || (type != "enum" && type != "bool" && type != "number"))
            continue;   // a newer field kind this build cannot draw: the service default applies
        if (m_pins.contains(dest))
            continue;   // the selected mode decides this one

        Row row;
        row.field = f;
        wxString label = from_u8(f.value("label", dest));
        if (type == "number" && f.contains("unit") && f.at("unit").is_string())
            label += " (" + from_u8(f.at("unit").get<std::string>()) + ")";
        row.label = new wxStaticText(this, wxID_ANY, label + ":");
        const wxString help = from_u8(f.value("help", std::string()));

        if (type == "enum") {
            auto* combo = new ComboBox(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(200), -1), 0, nullptr,
                                       wxCB_READONLY);
            for (const json& c : f.value("choices", json::array())) {
                if (!choice_offered(c, m_facts))
                    continue;
                row.values.push_back(c.value("value", std::string()));
                combo->Append(from_u8(c.value("label", row.values.back())));
            }
            // Any field may be what another one's show_if looks at.
            combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) { update_visibility(); e.Skip(); });
            row.control = combo;
        } else if (type == "bool") {
            auto* check = new ::CheckBox(this);
            check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { e.Skip(); CallAfter([this]() { update_visibility(); }); });
            row.control = check;
        } else {
            row.control = new TextInput(this, "", "", "", wxDefaultPosition, wxSize(FromDIP(120), -1));
        }
        if (!help.empty()) {
            row.label->SetToolTip(help);
            row.control->SetToolTip(help);
        }
        grid->Add(row.label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(row.control, 0, wxALIGN_CENTER_VERTICAL);
        m_rows.push_back(std::move(row));
    }
    v_sizer->Add(grid, 1, wxEXPAND | wxALL, FromDIP(12));

    auto* dlg_btns = new DialogButtons(this, {"Reset", "OK", "Cancel"}, "", 1 /*left_aligned*/);
    dlg_btns->GetFIRST()->SetToolTip(_L("Back to the service's defaults"));
    dlg_btns->GetFIRST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load(json::object()); });
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const json values = collect();
        if (values.is_null())
            return;   // collect() already said which field is wrong
        save_for(m_printer, values);
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(v_sizer);
    load(saved_for(m_printer));
    wxGetApp().UpdateDlgDarkUI(this);
}

// Fill every control from `saved`, the field's default where nothing valid is saved.
void SliceServiceOptionsDialog::load(const json& saved)
{
    for (Row& row : m_rows) {
        const std::string dest  = row.field.value("dest", std::string());
        const std::string type  = row.field.value("type", std::string());
        const json        value = saved.contains(dest) && value_valid(row.field, saved.at(dest), m_facts)
                                      ? saved.at(dest)
                                      : field_default(row.field, m_facts);
        if (type == "enum") {
            auto* combo = static_cast<ComboBox*>(row.control);
            int   idx   = 0;
            for (size_t i = 0; i < row.values.size(); ++i)
                if (value.is_string() && row.values[i] == value.get<std::string>())
                    idx = int(i);
            if (!row.values.empty())
                combo->SetSelection(idx);
        } else if (type == "bool") {
            static_cast<::CheckBox*>(row.control)->SetValue(value.is_boolean() && value.get<bool>());
        } else {
            static_cast<TextInput*>(row.control)->GetTextCtrl()->SetValue(number_text(value));
        }
    }
    update_visibility();
}

json SliceServiceOptionsDialog::control_values() const
{
    // Fields not drawn (pinned, or of a kind this build cannot draw) keep their pin or default.
    json values = effective_values(m_schema, json::object(), m_facts, m_pins);
    for (const Row& row : m_rows) {
        const std::string dest = row.field.value("dest", std::string());
        const std::string type = row.field.value("type", std::string());
        if (type == "enum") {
            const int idx = static_cast<ComboBox*>(row.control)->GetSelection();
            if (idx >= 0 && idx < int(row.values.size()))
                values[dest] = row.values[idx];
        } else if (type == "bool") {
            values[dest] = static_cast<::CheckBox*>(row.control)->GetValue();
        } else {
            double         d    = 0.;
            const wxString text = static_cast<const TextInput*>(row.control)->GetTextCtrl()->GetValue().Trim().Trim(false);
            if (!text.empty() && (text.ToCDouble(&d) || text.ToDouble(&d)))
                values[dest] = d;
        }
    }
    return values;
}

void SliceServiceOptionsDialog::update_visibility()
{
    const json values = control_values();
    for (Row& row : m_rows) {
        const bool show = field_applies(row.field, values, m_facts);
        row.label->Show(show);
        row.control->Show(show);
    }
    Layout();
    Fit();
}

// Every visible field's value, or null after telling the user which number is wrong. Values equal
// to the service's default are not stored, so a later change of default reaches the user.
json SliceServiceOptionsDialog::collect() const
{
    const json values = control_values();
    json       out    = json::object();
    for (const Row& row : m_rows) {
        if (!field_applies(row.field, values, m_facts))
            continue;
        const std::string dest = row.field.value("dest", std::string());
        const std::string type = row.field.value("type", std::string());
        json              value;
        if (type == "enum") {
            const int idx = static_cast<ComboBox*>(row.control)->GetSelection();
            if (idx >= 0 && idx < int(row.values.size()))
                value = row.values[idx];
        } else if (type == "bool") {
            value = static_cast<::CheckBox*>(row.control)->GetValue();
        } else {
            const wxString text = static_cast<const TextInput*>(row.control)->GetTextCtrl()->GetValue().Trim().Trim(false);
            if (text.empty()) {
                if (row.field.value("optional", false))
                    continue;   // blank = not sent
                value = field_default(row.field, m_facts);
            } else {
                double d = 0.;
                if (!text.ToCDouble(&d) && !text.ToDouble(&d)) {
                    show_error(const_cast<SliceServiceOptionsDialog*>(this),
                               wxString::Format(_L("\"%s\" is not a number."), row.label->GetLabel()));
                    return json();
                }
                value = d;
            }
            if (!value.is_null() && !value_valid(row.field, value, m_facts)) {
                show_error(const_cast<SliceServiceOptionsDialog*>(this),
                           wxString::Format(_L("\"%s\" is out of range."), row.label->GetLabel()));
                return json();
            }
        }
        if (!value.is_null() && value != field_default(row.field, m_facts))
            out[dest] = value;
    }
    return out;
}

}} // namespace Slic3r::GUI
