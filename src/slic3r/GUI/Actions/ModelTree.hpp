#ifndef slic3r_GUI_Actions_ModelTree_hpp_
#define slic3r_GUI_Actions_ModelTree_hpp_

// slicer.getModelTree {plate?}: every object with its stable id, index, name, plate, printable,
// filament, per-object overrides (keys), cut planes, layer ranges, instances and volumes (type,
// filament, overrides, which paint layers hold paint, bbox), plus treeVersion. Owned by the
// objects work package (S2); registered as a typed method by register_selection_actions.

#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r {
class Model;
class ModelObject;
enum class ModelVolumeType : int;
namespace GUI {
class PartPlateList;
namespace Actions {

// The tree for `plate` (-1 = every plate). `plates` may be null (headless: plate fields omitted).
nlohmann::json build_model_tree(const Model& model, const PartPlateList* plates, int plate);

// Changes whenever anything build_model_tree reports changes. Cheap enough to send with every
// plateChanged event.
std::string tree_version(const Model& model);

// An object's cut planes in the WORLD (plate) frame, the frame slicer.setCutPlanes takes by default,
// so a read-modify-write round-trips: {enabled, active, userOrdered, frame: "world", planes: [{point,
// normal, enabled, buildNormal?, buildAnglesDeg?}]}. buildNormal / buildAnglesDeg {b, c} are present
// only where the person overrode the plane's build direction. Shared by model.getTree,
// slicer.getPlate and slicer.setCutPlanes' reply.
nlohmann::json cut_planes_json(const ModelObject& object);

// The wire name of a volume type: part | negative | modifier | blocker | enforcer | infillMesh.
const char* volume_type_name(ModelVolumeType type);

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_ModelTree_hpp_
