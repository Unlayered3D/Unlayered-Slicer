#ifndef slic3r_GUI_Actions_Register_hpp_
#define slic3r_GUI_Actions_Register_hpp_

// Every domain's registration function, called in a fixed order by register_all() (Register.cpp).
// Explicit on purpose: libslic3r_gui is a static library, so a self-registering translation unit
// nobody references would be dropped by the linker and its actions would silently vanish.
//
// A register function only builds descriptors and stores runner lambdas (Actions::add); it must
// not touch the GUI, so the catalog builds in unit tests.

namespace Slic3r { namespace GUI { namespace Actions {

class Catalog;

// Fills `c` with every action, in the fixed order below. Idempotent for the process-wide catalog
// (catalog() calls it once).
void register_all(Catalog& c);

// Each adds its domain's actions to `c`.
void register_existing_methods(Catalog& c);     // ExistingMethods.cpp      (S1) typed slicer.* methods
void register_edit_actions(Catalog& c);         // EditActions.cpp          (S1) edit.undo / edit.redo, bridge method descriptors of S1
void register_selection_actions(Catalog& c);    // SelectionActions.cpp     (S2) selection.*, getModelTree
void register_object_actions(Catalog& c);       // ObjectActions.cpp        (S2) object.*, edit.cut/copy/paste
void register_part_actions(Catalog& c);         // PartActions.cpp          (S3) part.*
void register_mesh_actions(Catalog& c);         // MeshActions.cpp          (S3) boolean / simplify / fix / cut
void register_text_actions(Catalog& c);         // TextActions.cpp          (S3) text.* / svg.*
void register_object_settings_actions(Catalog& c); // ObjectSettingsActions.cpp (S4) per-object/volume/range settings
void register_layer_height_actions(Catalog& c); // LayerHeightActions.cpp   (S4) variable layer height, layer ranges
void register_plate_actions(Catalog& c);        // PlateActions.cpp         (S5) plate.*
void register_layer_event_actions(Catalog& c);  // LayerEventActions.cpp    (S5) layer.setEvents
void register_preview_actions(Catalog& c);      // PreviewActions.cpp       (S5) preview.*, view.capture
void register_paint_actions(Catalog& c);        // PaintActions.cpp         (S6) paint.*
void register_geometry_actions(Catalog& c);     // GeometryActions.cpp      (S6) geometry.*
void register_preset_actions(Catalog& c);       // PresetActions.cpp        (S7) preset.*
void register_project_actions(Catalog& c);      // ProjectActions.cpp       (S7) project.*
void register_file_actions(Catalog& c);         // FileActions.cpp          (S7) file.*
void register_filament_actions(Catalog& c);     // FilamentActions.cpp      (S8) filament.*, printer.setNozzle, bed.setType
void register_calibration_actions(Catalog& c);  // CalibrationActions.cpp   (S8) calib.run
void register_view_actions(Catalog& c);         // ViewActions.cpp          (S8) view.*, ui.*
void register_preference_actions(Catalog& c);   // PreferenceActions.cpp    (S8) prefs.*, app.checkUpdates
void register_slice_actions(Catalog& c);        // SliceActions.cpp         slice.cancel, slice.getOptions/setOptions, slice.exportSliced

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_Register_hpp_
