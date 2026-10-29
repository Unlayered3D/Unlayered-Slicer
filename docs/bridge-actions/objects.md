# Bridge actions: model tree, selection and object structure

Work package S2. Sources: `src/slic3r/GUI/Actions/ModelTree.{hpp,cpp}`, `SelectionActions.cpp`,
`ObjectActions.cpp`. Tests: `tests/slic3rutils/test_actions_objects.cpp` (`[actions][objects]`),
`tools/bridge_e2e/cases/objects.yaml`. Coverage manifest: `resources/actions/surface/object.json`.
Design: `docs/HLSD/bridge-actions.md`.

Every action here is invoked with `slicer.invokeAction {id, target, params, turn}` (or, for
`model.getTree`, the typed method `slicer.getModelTree`). All are behind `<slot>_allow_settings`;
none needs another gate, none asks for confirmation: every model edit is **one undo step**
named `"<label> (automated)"`, and `slicer.undoTurn` reverts it.

## How a runner works

1. The runtime validates `params`, resolves `target` and **selects** it in the 3D scene and the
   object list.
2. The runner checks the conditions under which the menu command would pop a dialog and
   answers them from params, or refuses with the reason (1002 with `data.path`/`data.needs`,
   1016 with `data.reason`).
3. It takes the snapshot (`ctx.snapshot`) and calls the same Plater / ObjectList / Selection
   function the menu item calls, with that function's own snapshots suppressed.

## Targets

`{"objects": [{"id": 123} | {"index": 0} | {"name": "Benchy"}]}`; ids are
`ModelObject::id().id` from the model tree and stay valid when other objects are deleted, so
prefer them. A required single-object target may be omitted when the plate holds exactly one
object.

Object actions whose menu item also works on a part or a single instance declare
`target.accepts` (see `docs/HLSD/bridge-actions.md`) and take `{"volumes": [...]}` /
`{"instances": [{"object": ref, "index": i} | {"id"}]}` as well: `object.mirror`, `object.center`,
`object.drop` (parts and instances), `object.setPrintable`, `object.removeInstance` (instances),
`object.order`, `edit.copy` (parts), `selection.set` (both).

## slicer.getModelTree / model.getTree (read)

`{plate?: int}` (-1 / absent = every plate) returns:

```json
{ "objects": [ {
    "id": 412, "index": 0, "name": "Benchy", "plate": 0, "printable": true,
    "filament": 0,                    // 1-based slot the object sets; 0 = inherits (slot 1)
    "overrides": ["wall_loops"],      // per-object setting keys
    "isCut": false,
    "bboxMm": {"min": [..], "max": [..], "size": [..]},          // instance 0, world frame
    "cutPlanes": {"enabled": true, "active": true, "userOrdered": false, "frame": "world",
                  "planes": [{"point", "normal", "enabled", "buildNormal"?, "buildAnglesDeg"?: {"b", "c"}}]},
    "brimEars": [{"x", "y", "radiusMm"}],                        // plate frame (instance 0)
    "layerRanges": [{"zMin": 0, "zMax": 5, "overrides": ["layer_height"]}],
    "instances": [{"id", "index", "offset": [x,y,z], "rotationDeg": [x,y,z], "scale": [x,y,z],
                   "mirrored", "printable", "autoDrop", "plate", "bboxMm"}],
    "volumes": [{"id", "index", "name", "type": "part|negative|modifier|blocker|enforcer|infillMesh",
                 "filament", "overrides", "paint": {"support","seam","fuzzy","color"},
                 "isText", "isSvg", "bboxMm",
                 "text"?: {"text", "font", "heightMm", "depthMm", "charGap", "lineGap", "boldness", "skew",
                           "alignH", "alignV", "fontIndex", "useSurface", "fontStyle"?, "fontWeight"?},
                 "svg"?: {"sizeMm": [w, h], "depthMm", "mirrored", "useSurface"}}] } ],
  "objectCount": 1, "truncated": false, "treeVersion": "9f3c...",
  "currentPlate": 0, "plateCount": 1 }
```

`cutPlanes` is in the plate (world) frame -- the frame `slicer.setCutPlanes` takes by default and
replies in, and `slicer.getPlate` reports -- so planes read here can be edited and written back.
`buildNormal` (and its machine angles `buildAnglesDeg`) appear only on a plane whose build
direction was overridden; a replace through `setCutPlanes` keeps the override of a plane written
back unchanged. `brimEars` and the `text` / `svg` blocks appear only when present.

`treeVersion` (FNV-1a over names, ids, transforms, configs, mesh stats and paint content)
changes with any of them and returns to the same value when an undo restores the same state.
Past 500 objects the list is cut (`truncated: true`).

## selection.set (ui)

`{mode: "target"|"plate"|"all"|"none", plate?}`: select exactly the target objects, parts or
instances (as a click, an Alt-click on a part or a row selection does); all objects on a plate
(Plate menu > Select All); all objects on all plates (Edit > Select All); nothing (Edit > Deselect
All). Returns the resulting selection as refs.

## Object actions

| id | params | menu | notes |
|---|---|---|---|
| `object.rename` | `name` | Rename | illegal file-name characters refused; a single-part object's part is renamed too |
| `object.delete` | `breakCutLink?` | Delete, Edit > Delete Selected | pieces of a cut object need `breakCutLink: true` (the menu asks in a dialog) |
| `object.clone` | `count 1-100`, `arrange?` | Clone, Edit > Clone Selected | independent objects; replaces the slicer's internal copy buffer like the dialog; `arrange` makes it long-running |
| `object.addInstance` | `count` | Add instance, toolbar + | linked copies |
| `object.removeInstance` | `count`, or target `instances` | Remove instance, toolbar -; Del on instance rows | with an object: its last `count`; with `target.instances`: exactly those (highest index first). An object always keeps one (use `object.delete`). Returns `removed`, `objects[].instances` |
| `object.instancesToObjects` | `instances?` | Set as An Individual Object | all instances when omitted |
| `object.center` / `object.drop` | - | Center / Drop (object and part menus); the list's sinking column | objects, one part or single instances; returns `objects` / `volumes` / `instances` with where they are now |
| `object.setAutoDrop` | `enabled` | Auto Drop | set, not toggled |
| `object.setPrintable` | `printable` | Printable, the list's printable column, 'p' / 'V' | set, not toggled; objects (every instance) or single `target.instances` |
| `object.setVisible` | `visible` | Hide / Show (assembly view) | display only (risk ui), not undoable, no model change |
| `object.mirror` | `axis x|y|z` | Mirror > Along X/Y/Z (object and part menus) | objects, one part or single instances; an object with several instances is refused whole (name its instances), as the menu greys it out |
| `object.order` | exactly one of `move top|up|down|bottom`, `toIndex`, `before {id|index|name}` | Order (object and part menus), dragging a row by its grip | one object among the objects of its plate (print order by object): `toIndex` is 0-based on that plate, `before` another object on the same plate; returns `plateOrder`. Or one part (`target.volumes`) among its object's parts, by `move` or `toIndex` (the part index to take): parts stay within their type group, index 0 is kept for a solid part, blockers/enforcers stay put (refused with `data.allowed`) |
| `object.splitToObjects` | `keepZ?` | Split > To Objects, toolbar | refused for a one-piece object (the menu shows a warning); `keepZ` answers "disable Auto-Drop to preserve Z?" (default: keep when the object was not auto-dropping) |
| `object.merge` | `mode multipart|single` | Assemble | multipart: >=2 objects into one object; single: one object's parts into one part |
| `object.convertUnits` | `conversion fromInch|toInch|fromMeter|toMeter` | Convert from Inches/Meters, Restore | refused when already converted / nothing to restore |
| `object.reloadFromDisk` | - | Reload from disk | refused (1008 `data.missing`) when a source file is gone (the menu opens a file dialog) and for OBJ sources (colour dialog) |
| `object.copySettings` | - | Copy Process Settings | ui; per-object overrides into the list's clipboard |
| `object.pasteSettings` | - | Paste Process Settings | 1016 until something was copied |
| `object.setTransform` | `offset?`, `rotationDeg?`, `scale?`, `relative?`, `frame?`, `instance?` | Move / Rotate / Scale gizmos | at least one of the three; absolute rotation = Euler XYZ, relative = about plate X, Y, Z around the object's centre; non-uniform scale allowed; an absolute offset on a multi-instance object needs `instance`; auto-drop objects land back on the bed. `frame` (relative only) is the gizmos' coordinate combo: `world` = offset, rotation and scale all along the plate's axes (a non-uniform scale of a rotated object along a plate axis), `object` = all along the object's own axes (a move along its own X); absent keeps offset and rotation on the plate's axes and scale on the object's own |
| `object.layOnFace` | exactly one of `normal` / `largestFace: true` | Flatten gizmo | `normal` = world direction the face points now; largest = convex-hull face of largest area; the same object face goes down on every instance |
| `object.deleteConnectors` | - | Del on the "Cut connectors" item > Delete all connectors | a piece of a cut object: every connector of every piece goes, the cut link stays; pieces left without a solid part are deleted (`removedObjects`) |
| `plate.clear` | `scope plate|all`, target `plate?` | Plate > Delete All, Edit > Delete All | undoable; selects the named plate first |
| `edit.cut` / `edit.copy` / `edit.paste` | - | Edit > Cut / Copy / Paste | copy is ui-only and reports `copied` (objects) and `copiedParts`; paste takes an optional object target, into which copied parts are pasted, and reports `added` (objects) and `addedParts`; cut/paste are one undo step each |

## Part actions (object list)

| id | params | GUI | notes |
|---|---|---|---|
| `part.rename` | `name` | in-place rename of a part row; Parameter Table name cell | one part (`target.volume`); same name rules as `object.rename`. Text / SVG parts take a name from their text or file again on the next `text.edit` / `svg.edit` |
| `part.delete` | `breakCutLink?` | Del on a part / modifier / negative / enforcer row, or with a part selected in the scene; part menu Delete | refused (1016) for an object's last solid part (the list shows an error); a solid part or negative volume of a cut object needs `breakCutLink: true` (the list asks in a dialog). Deletes the highest index first; a last remaining part's settings move onto the object. Returns `deleted`, `objects[].parts` |

Results name objects by `{id, index, name}` refs; actions that create objects return them as
`added`/`objects`.

## Not here

- Parts (add, type, split to parts, boolean, simplify, cut, text): S3 `part.*`.
- Per-object settings values, layer ranges, variable layer height: S4.
- Fill bed / number of copies / scale to fit: the typed methods `slicer.fillBed`,
  `slicer.setCopies`, `slicer.setScale` (manifest `coveredBy`).
- Duplicate plate, auto-orient: S5 (`plate.duplicate`, `object.autoOrient`).
