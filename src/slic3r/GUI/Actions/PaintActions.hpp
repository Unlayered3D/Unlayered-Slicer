#ifndef slic3r_GUI_Actions_PaintActions_hpp_
#define slic3r_GUI_Actions_PaintActions_hpp_

// paint.* and geometry.* cores, separated from the GUI so they are unit-testable on a bare Model:
// the runners in PaintActions.cpp / GeometryActions.cpp add the snapshot, the object list refresh
// and the background-process kick around them. Painting design: docs/bridge-actions/paint.md.

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ActionCatalog.hpp"

namespace Slic3r {
class DynamicPrintConfig;
class Model;
namespace GUI { namespace Actions {

// The restricted-dialect schema of a `where` selector (two levels of all/any/not around the leaf
// selectors of libslic3r/FacetSelectors.hpp).
nlohmann::json selector_schema();

// Params schemas of the actions (also used by the catalog descriptors).
nlohmann::json paint_apply_params_schema();
nlohmann::json paint_clear_params_schema();
nlohmann::json paint_stats_params_schema();

// What the GUI side needs to know to report hints; all optional.
struct PaintHintContext
{
    const DynamicPrintConfig* print_config    = nullptr; // the edited print preset
    int                       filament_count  = 0;       // 0 = unknown
};

// paint.apply on `objects` (Model::objects indices). `before_write` runs exactly once, before the
// first change to the model, and only when something will change (the runner takes its undo
// snapshot there). `changed` receives the indices of objects whose annotations changed.
InvokeResult paint_apply_model(Model& model, const std::vector<int>& objects, const nlohmann::json& params,
                               const std::function<void()>& before_write, std::vector<int>& changed,
                               const PaintHintContext& hints = {});
// paint.clear: erase a whole paint layer (or every layer).
InvokeResult paint_clear_model(Model& model, const std::vector<int>& objects, const nlohmann::json& params,
                               const std::function<void()>& before_write, std::vector<int>& changed);
// paint.remapFilaments: relabel colour-painted filaments ({pairs:[{from,to}]}), as the colour
// painting gizmo's Remap does.
nlohmann::json paint_remap_params_schema();
InvokeResult   paint_remap_model(Model& model, const std::vector<int>& objects, const nlohmann::json& params,
                                 const std::function<void()>& before_write, std::vector<int>& changed);
// paint.stats: per object / volume, per layer and state: painted facets and area.
InvokeResult paint_stats_model(const Model& model, const std::vector<int>& objects, const nlohmann::json& params);

// geometry.query / geometry.measure (GeometryActions.cpp).
nlohmann::json geometry_query_params_schema();
nlohmann::json geometry_measure_params_schema();
InvokeResult   geometry_query_model(const Model& model, const std::vector<int>& objects, const nlohmann::json& params);
InvokeResult   geometry_measure_model(const Model& model, const std::vector<int>& objects, const nlohmann::json& params);

// object.brimEars on one object: {mode: "points"|"clear", points:[{x,y}], radiusMm, replace}.
nlohmann::json brim_ears_params_schema();
InvokeResult   brim_ears_model(Model& model, int object, const nlohmann::json& params, float default_radius,
                               const std::function<void()>& before_write, bool& changed);

}} // namespace GUI::Actions
} // namespace Slic3r

#endif // slic3r_GUI_Actions_PaintActions_hpp_
