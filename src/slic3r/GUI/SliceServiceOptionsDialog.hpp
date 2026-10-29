#ifndef slic3r_GUI_SliceServiceOptionsDialog_hpp_
#define slic3r_GUI_SliceServiceOptionsDialog_hpp_

// The options of an external slicing service (SLICE_SERVICE_PROTOCOL.md), as the Slice dropdown's
// "Slicing service options..." dialog, and the service's modes as the dropdown's entries. The
// service describes both itself: its modes and name in GET <root>/ and its form in GET
// <root>/ui-schema. This program knows only the vocabulary -- enum, bool and number fields,
// `show_if` conditions on other fields and on documented machine facts, and `default_by` defaults
// keyed on those facts -- never the service's option names or what they do. Choices are remembered
// per printer preset in the app config's "slice_service" section and sent with each job as the
// `options` form field; the selected mode goes as the `mode` field.
//
// Machine facts the slicer computes from the printer preset (documented in the protocol):
//   machine_axes   5 for a printer that tilts the nozzle (non-planar on, a real rotary axis
//                  pair), else 3. Also sent with each job as the `machine_axes` form field.

#include "GUI_Utils.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

class wxStaticText;
class wxWindow;

namespace Slic3r { namespace GUI {

namespace SliceService {

// The service's form as last fetched (data_dir()/cache/slice_service/schema.json); null when none.
nlohmann::json cached_schema();
// What the service said about itself (GET /, cache/slice_service/info.json); null when none.
nlohmann::json cached_info();
// Fetch the form and the description again in the background and cache them; silent on failure.
void           refresh_schema_async();
// Fetch them now, on the calling (GUI) thread, with a busy cursor. False with `error` when the form
// could not be fetched (the description is optional).
bool           refresh_schema_now(std::string& error);

// The service's name for itself: GET / `display_name`, else the form's `title`, else a generic one.
wxString       display_name();

// The documented machine facts for the selected printer, as JSON ({"machine_axes": 3|5}).
nlohmann::json machine_facts();
int            machine_axes();

// The service's modes (GET / `modes`) offered on the selected printer, in the service's order, for
// the Slice dropdown's one-entry-per-mode list. Empty when the service names none.
struct ModeChoice
{
    std::string id;
    wxString    label;
};
std::vector<ModeChoice> mode_choices();
// The mode a job on the selected printer would use (remembered choice, else the first offered).
std::string current_mode();
// The mode's label, or "" when the service does not name it.
wxString    mode_label(const std::string& id);
// Remember `id` as the selected printer's mode.
void        set_mode(const std::string& id);

// The `options` JSON for a job on the selected printer: the remembered choices that still apply
// under the cached form (visible for this printer, valid values), plus the values the selected
// mode fixes (a mode's `options`). "" when nothing is set.
std::string    request_options();

// For automation (slice.getOptions / slice.setOptions): the same form and storage the options
// dialog uses, with no dialog and no network. The selected printer preset's remembered choices
// (the app config's "slice_service" section, as the dialog saves them; {} when none).
nlohmann::json saved_options();
// The form's fields for this printer: each with its value as the dialog would show it (remembered
// and valid, else the default), its default, whether it applies (show_if) under the effective
// values and machine facts, and for enums only the choices offered on this printer. Fields the
// selected mode fixes are left out, as the dialog leaves them out. Null without a cached form.
nlohmann::json describe_options(const nlohmann::json& schema);
// What the dialog's OK would store after the person changed `changes` ({dest: value}; null = back
// to the default, or blank for an optional number) starting from `saved` ({} after Reset):
// the applying fields that differ from their defaults. False with the offending `error_dest` and
// `error` for an unknown field, a field the selected mode fixes, an invalid value, or a field that
// does not apply under the resulting values. `facts` is machine_facts(), `pins` the values the
// selected mode fixes (mode_options()). Pure on its inputs.
bool options_to_store(const nlohmann::json& schema, const nlohmann::json& saved, const nlohmann::json& changes, const nlohmann::json& facts,
                      const nlohmann::json& pins,
                      nlohmann::json& out, std::string& error_dest, std::string& error);
// The option values the selected mode fixes (its `options` in GET /); {} when none.
nlohmann::json mode_options();
// Stores `values` for the selected printer as the dialog's OK does; empty erases them (Reset).
void save_options(const nlohmann::json& values);

} // namespace SliceService

class SliceServiceOptionsDialog : public DPIDialog
{
public:
    SliceServiceOptionsDialog(wxWindow* parent, const nlohmann::json& schema);

private:
    struct Row
    {
        nlohmann::json field;
        wxStaticText*  label   = nullptr;
        wxWindow*      control = nullptr;
        std::vector<std::string> values;   // enum: the value behind each combo entry
    };

    nlohmann::json   m_schema;
    std::string      m_printer;
    nlohmann::json   m_facts;
    nlohmann::json   m_pins;   // fields the selected mode fixes: not shown, never stored
    std::vector<Row> m_rows;

    void           load(const nlohmann::json& saved);
    nlohmann::json collect() const;
    // Every field's value as the controls show it (pins and defaults for fields not drawn).
    nlohmann::json control_values() const;
    void           update_visibility();

    void on_dpi_changed(const wxRect& suggested_rect) override {}
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SliceServiceOptionsDialog_hpp_
