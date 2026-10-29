#ifndef slic3r_GUI_Actions_NativeConfirm_hpp_
#define slic3r_GUI_Actions_NativeConfirm_hpp_

// The one native confirmation an automated Destructive or Outward action waits on, and the
// native Save dialog Outward-file actions use. Both are modal slicer windows the embedded page
// cannot press, pre-answer or see into: the page only learns the outcome (1006 when declined).
//
// Dev edition only: the environment variables UNLAYERED_E2E_AUTOCONFIRM=accept|cancel and
// UNLAYERED_E2E_SAVE_DIR=<dir> answer them without a window so the end-to-end tests
// (tools/bridge_e2e) can run unattended. Every automatic answer is logged. Neither variable is
// read by a customer build.

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Actions {

struct ConfirmRequest
{
    std::string              title;        // window title ("Delete preset")
    std::string              message;      // what will happen, naming exactly what is lost
    std::vector<std::string> details;      // listed lines: keys, G-code lines, file names (capped at 40 shown)
    std::string              accept_label; // the accepting button ("Delete"); default "Continue"
    bool                     destructive = true;
};

// GUI thread; modal. True = the person accepted.
bool native_confirm(const ConfirmRequest& request);

// Native Save dialog. `wildcard` is a wx wildcard ("STL files (*.stl)|*.stl"). False = cancelled.
bool native_save_path(const std::string& title, const std::string& default_name, const std::string& wildcard,
                      std::string& out_path);

enum class AutoConfirm { Off, Accept, Cancel };
// Always Off and "" in a customer build.
AutoConfirm e2e_autoconfirm();
std::string e2e_save_dir();

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_NativeConfirm_hpp_
