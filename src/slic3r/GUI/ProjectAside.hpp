#ifndef slic3r_GUI_ProjectAside_hpp_
#define slic3r_GUI_ProjectAside_hpp_

// Unlayered: G-code handed over by a web app (its "Open in the slicer") opens in preview-only
// mode, and stock Plater::load_gcode() gets there by closing the project -- the "unsaved changes,
// save?" dialog, or the plate simply gone. Instead the open project is SET ASIDE: written to a stash
// 3MF under data_dir()/handoff_stash together with its path, name and dirty state, and RESTORED on
// the way back -- the Prepare tab, the preview notification's link, or anything that would close
// the preview (new/open project, quitting), which restores it first so the ordinary save prompt
// covers it. G-code opened while one is aside (a re-slice, File > Import G-code, reload) keeps it
// aside. Implemented in Plater.cpp; no Plater.hpp change, so it costs no wide rebuild.

#include <string>

namespace Slic3r {
namespace GUI {

class Plater;

// Stash the open project and close it without a prompt. Does nothing (and returns true) when there
// is nothing on the plate to lose, or when a G-code preview is already showing -- any project
// already aside stays aside. Unsaved preset edits travel in the stash and are discarded here, so the
// printer switch and the G-code open that follow raise no "modified presets" dialog. False: the
// stash could not be written and the project is untouched; open the old way (it will ask).
bool set_project_aside(Plater& plater);

// A project is aside AND a G-code preview is what replaced it: the state the restore paths act on.
bool has_project_aside(Plater& plater);

// Reload the stashed project as it was: same file (Save overwrites the original), same name, dirty
// again if it was dirty, Prepare tab. False if there was nothing to restore or it failed to load;
// the stash file is then kept on disk and its path logged.
bool restore_project_aside(Plater& plater);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ProjectAside_hpp_
