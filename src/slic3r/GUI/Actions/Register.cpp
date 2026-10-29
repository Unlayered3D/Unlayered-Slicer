#include "Register.hpp"

#include "ActionCatalog.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

void register_all(Catalog& c)
{
    // Fixed order: the catalog version and listActions order depend on it.
    register_existing_methods(c);
    register_edit_actions(c);
    register_selection_actions(c);
    register_object_actions(c);
    register_part_actions(c);
    register_mesh_actions(c);
    register_text_actions(c);
    register_object_settings_actions(c);
    register_layer_height_actions(c);
    register_plate_actions(c);
    register_layer_event_actions(c);
    register_preview_actions(c);
    register_paint_actions(c);
    register_geometry_actions(c);
    register_preset_actions(c);
    register_project_actions(c);
    register_file_actions(c);
    register_filament_actions(c);
    register_calibration_actions(c);
    register_view_actions(c);
    register_preference_actions(c);
    register_slice_actions(c);
}

}}} // namespace Slic3r::GUI::Actions
