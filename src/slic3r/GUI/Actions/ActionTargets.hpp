#ifndef slic3r_GUI_Actions_ActionTargets_hpp_
#define slic3r_GUI_Actions_ActionTargets_hpp_

// What an action acts on. The page names model entities in `target`; the runtime resolves them to
// indices into Model::objects / ModelObject::volumes / ModelObject::instances, then SELECTS them,
// so a runner can call the same Plater / ObjectList function the menu item calls (which acts on
// the selection) and inherit its snapshots, list refresh and plate refiling.
//
// Wire form (every list may hold one or more refs; ids are preferred because they survive edits
// to other objects -- they are ModelObject/ModelVolume/ModelInstance::id().id, the "id" of
// slicer.getModelTree):
//
//   "target": {
//     "objects":   [ {"id": 1234} | {"index": 0} | {"name": "Benchy"} ],
//     "volumes":   [ {"id": 1240} | {"object": <object ref>, "index": 1} | {"object": <ref>, "name": "Modifier"} ],
//     "instances": [ {"id": 1236} | {"object": <object ref>, "index": 0} ],
//     "plate":     0,                  // 0-based plate index
//     "selection": true                // what the user has selected right now ("this part")
//   }
//
// Singular sugar: "object": <ref>, "volume": <ref>, "instance": <ref>. With no target and a
// required object(s) kind, a plate holding exactly one object implies it (as the typed methods do).

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
class Model;
class ModelObject;
class ModelVolume;
class ModelInstance;
} // namespace Slic3r

namespace Slic3r { namespace GUI { namespace Actions {

struct ResolvedTargets
{
    std::vector<int>                 objects;   // Model::objects indices, ascending, unique
    std::vector<std::pair<int, int>> volumes;   // (object index, volume index)
    std::vector<std::pair<int, int>> instances; // (object index, instance index)
    int                              plate = -1; // -1: not named (the current plate where one is needed)

    bool empty() const { return objects.empty() && volumes.empty() && instances.empty() && plate < 0; }
};

// Does the descriptor's target spec take this kind of entity? Its own kind, or one listed in the
// optional "accepts" (an object action that also acts on parts or single instances, e.g.
// {"kind":"objects","required":true,"accepts":["volumes","instances"]}). Pure.
bool target_accepts(const nlohmann::json& spec, const std::string& kind);

// Pure (Model only). `spec` is the descriptor's target {"kind","required","accepts"?}; `target` is what the
// page sent (null/absent allowed). `selection` is the GUI selection, used for "selection": true
// (nullptr: refused). `plate_count` bounds "plate". Returns WAB_OK or 1002 (bad shape, data.path)
// / 1008 (names nothing; data.missing). Objects named via volumes/instances are NOT added to
// `objects`; kinds that need objects accept only objects (or the implied one), unless the spec's
// "accepts" lists volumes / instances, which then satisfy a required target too.
int resolve_targets(const Model& model, int plate_count, const nlohmann::json& spec, const nlohmann::json& target,
                    const ResolvedTargets* selection, ResolvedTargets& out, std::string& message, nlohmann::json& data);

// Index of the object/volume/instance with that ObjectID, or -1.
int                 object_index_by_id(const Model& model, size_t id);
std::pair<int, int> volume_index_by_id(const Model& model, size_t id);
std::pair<int, int> instance_index_by_id(const Model& model, size_t id);

// {"id", "index", "name"}: how results refer back to an object.
nlohmann::json object_ref_json(const Model& model, int obj_idx);
nlohmann::json volume_ref_json(const Model& model, int obj_idx, int vol_idx);

// GUI side. The current selection in model terms (objects fully selected, else volumes, else
// instances) and its inverse: select exactly `t` in the 3D scene and the object list. Both no-ops
// without a Plater.
ResolvedTargets current_selection();
void            select_targets(const ResolvedTargets& t);

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_ActionTargets_hpp_
