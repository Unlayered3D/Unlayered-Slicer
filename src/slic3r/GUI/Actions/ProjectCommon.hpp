#ifndef slic3r_GUI_Actions_ProjectCommon_hpp_
#define slic3r_GUI_Actions_ProjectCommon_hpp_

// Shared between ProjectActions.cpp and ViewActions.cpp: bringing back the project a G-code preview
// set aside (ProjectAside.hpp), which project.restoreAside does and ui.showTab {tab:"prepare"} does
// during such a preview, as a click on the Prepare tab does (MainFrame::preview_only_hint).

#include "ActionCatalog.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Actions {

struct InvokeContext;

// ---- pure rules (tests/slic3rutils/test_actions_slice_project.cpp) ---------------------------

// The licences the Project tab's Basic Info offers (DesignerPanel's license_list, Auxiliary.cpp),
// "" = none.
const std::vector<std::string>& project_licenses();

// The project's auxiliary folders (Project tab: Pictures, BOM, Assembly Guide, Others): the wire
// name, the folder inside the project's auxiliary directory, and the file types the tab's Add
// File accepts there (lowercase, no dot).
struct AuxFolder
{
    std::string              wire;
    std::string              dir;
    std::vector<std::string> extensions;
};
const std::vector<AuxFolder>& aux_folders();
const AuxFolder*              find_aux_folder(const std::string& wire);
// A plain file name: no folder part, not "." or "..", 1..255 bytes, no control characters.
bool aux_file_name_ok(const std::string& name);
// `name`, or "<stem>_2<ext>", "<stem>_3<ext>" ... : the first not in `taken`.
std::string aux_unique_name(const std::string& name, const std::vector<std::string>& taken);

// Restores the set-aside project (honours ifDirty for unsaved preset edits first) and finishes
// later: returns InvokeResult::started(), or an error now (1008 when nothing is set aside).
InvokeResult restore_project_aside_action(InvokeContext& ctx);

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_ProjectCommon_hpp_
