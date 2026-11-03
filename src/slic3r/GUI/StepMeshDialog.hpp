#ifndef _STEP_MESH_DIALOG_H_
#define _STEP_MESH_DIALOG_H_

#include <thread>
#include <wx/event.h>
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "Widgets/Button.hpp"
#include "GeometryImportDialog.hpp"
class Button;
class RadioGroup;

class StepMeshDialog : public Slic3r::GUI::DPIDialog
{
public:
    StepMeshDialog(wxWindow* parent, Slic3r::Step& file, double linear_init, double angle_init,
                   Slic3r::GUI::GeometryImportOptions* import_options = nullptr);
    ~StepMeshDialog() override;
    void on_dpi_changed(const wxRect& suggested_rect) override;
    inline double get_linear_deflection() {
        double value;
        if (m_linear_last.ToDouble(&value)) {
            return value;
        }else {
            return m_last_linear;
        }
    }
    inline double get_angle_deflection() {
        double value;
        if (m_angle_last.ToDouble(&value)) {
            return value;
        } else {
            return m_last_angle;
        }
    }
    inline bool get_split_compound_value() {
        return m_split_compound_checkbox.GetValue();
    }
private:
    // A non-null context defers all preference writes until the whole import is ready.
    Slic3r::GUI::GeometryImportOptions* m_import_options = nullptr;
    Slic3r::GUI::ImportCheckBox m_keep_names_checkbox;
    Slic3r::GUI::ImportCheckBox m_sort_names_checkbox;
    // Item 0 loads as an assembly, item 1 as a single object.
    RadioGroup* m_load_mode = nullptr;
    Slic3r::Step& m_file;
    Slic3r::GUI::ImportCheckBox m_checkbox;
    Slic3r::GUI::ImportCheckBox m_split_compound_checkbox;
    Slic3r::GUI::ImportCheckBox m_save_default_checkbox;
    wxString m_linear_last;
    wxString m_angle_last;
    wxStaticText* mesh_face_number_text;
    double m_last_linear = 0.003;
    double m_last_angle = 0.5;
    unsigned int m_mesh_number = 0;
    boost::thread* m_task {nullptr};
    bool validate_number_range(const wxString& value, double min, double max);
    void update_mesh_number_text();
    void on_task_done(wxCommandEvent& event);
    void stop_task();
};

namespace Slic3r { namespace GUI {

// Automated loads (the web bridge's model imports) cannot answer the STEP meshing dialog or the
// non-UTF-8 name warning. While one of these is alive, STEP imports mesh with the Preferences values,
// as they do when "Show options when importing STEP file" is off, and the warning is only logged.
// The geometry import dialogs of Plater::load_files() (AMF/3MF options, name cleanup, the assemblies
// notice) are skipped the same way, as for a LoadStrategy::Silence load.
class NoStepImportDialogs
{
public:
    NoStepImportDialogs() { ++s_depth; }
    ~NoStepImportDialogs() { --s_depth; }
    NoStepImportDialogs(const NoStepImportDialogs&)            = delete;
    NoStepImportDialogs& operator=(const NoStepImportDialogs&) = delete;
    static bool active() { return s_depth > 0; }

private:
    static int s_depth;
};

// "Show options when importing STEP file" is on and no NoStepImportDialogs is alive.
bool step_mesh_dialog_wanted();

}} // namespace Slic3r::GUI

#endif  // _STEP_MESH_DIALOG_H_
