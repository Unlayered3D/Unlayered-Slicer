# Bridge actions: parts, mesh operations, cut, text and SVG

Work package S3. Sources: `src/slic3r/GUI/Actions/PartActions.cpp`, `MeshActions.cpp`,
`TextActions.cpp`, with helpers in `PartsCommon.hpp` (pure) and `PartsCommonGui.hpp`.
Tests: `tests/slic3rutils/test_actions_parts.cpp` (`[actions][parts]`),
`tools/bridge_e2e/cases/parts.yaml` (fixture `tools/bridge_e2e/fixtures/logo.svg`).
Coverage manifest: `resources/actions/surface/part.json`. Design: `docs/HLSD/bridge-actions.md`.

Every action is invoked with `slicer.invokeAction {id, target, params, turn}`. All are behind
`<slot>_allow_settings`, none needs another gate and none asks for confirmation: each one is a
model edit with **one undo step** named `"<label> (automated)"`, and `slicer.undoTurn` reverts it.
`text.listFonts` only reads.

## How these runners differ from the menus

The menus behind these actions show dialogs (the "Add modifier" tips box, error boxes, the
subdivision warning, the cut tool's "fix non-manifold edges?" question) or work through
interactive tools (Cut, Mesh boolean, Simplify, Text, SVG, Skeleton). A runner never opens any of
them:

- It answers from params what the dialog or tool would ask, or refuses with the reason (1002
  with `data.path`, or 1016 with `data.reason` when the target cannot take the action).
- It does what the tool's apply button does, with the tool's choices as params, and never opens
  the tool. The volume a runner creates carries the same data the tool writes, so text and SVG
  parts stay editable in the Text and SVG tools afterwards.
- **Long-running actions** (`part.simplify`, `part.smooth`, `part.fix`,
  `object.skeletonCutPlanes`, and everything that downloads: `part.add` with a URL, `part.replace`,
  `svg.add`) take their undo point when they start. They then work on a copy of the mesh, on a
  worker thread or after the download, and apply the result on the GUI thread. The bridge
  answers `done` when that happens within its wait; otherwise it answers `running` and
  `slicer.actionFinished` follows. Undoing the turn restores the state from before the start.
- **Downloads** follow `slicer.openProjectFromUrl`'s rules through `BridgeHost::download`: http(s),
  the page's own host or the slot's download hosts, one download at a time per tab, and the size
  limit. Without a download hook, these actions answer 1002.

## Frames and units

All lengths are in mm. Positions are on the **plate**: X/Y on the bed, Z up from it. This is the
frame `getPlate` / `getModelTree` report bounding boxes in. A volume's position is its origin,
which for a new primitive is its center. Rotations are in degrees about the plate X, Y and Z axes,
applied in that order. The instance's own rotation and scale are undone for a new part, so the
size you ask for is its size on the plate.

## Targets

- Object actions take `{"object": {"id"|"index"|"name"}}`.
- Part actions take `{"volume": {"id": 123}}` or `{"volume": {"object": <ref>, "index": n | "name": "..."}}`.
  For `volumes` actions, `{"volumes": [...]}` takes several parts.
- A single-part object implies its part.
- Volume ids come from `getModelTree` (`objects[].volumes[].id`). They survive edits to other
  parts, so prefer them.

## part.*

| id | target | params | notes |
|---|---|---|---|
| `part.add` | object | `type` (part, negative, modifier (default), blocker, enforcer, infillMesh), `shape` (cube (default), cylinder, sphere, cone, disc, torus, url), `url`, `sizeMm` ([d] = largest side, or [x,y,z]), `positionMm`, `relativeTo` (plate (default) or object), `rotationDeg`, `name` | No position: the menu's spot, the object's left-front corner on the bed. `relativeTo: "object"` makes positionMm an offset from the object's bounding-box center, so `[0,0,h/2]` is the top. A part inherits the object's filament. |
| `part.setType` | volumes | `type` | Refuses to change an object's last solid part. Text and SVG parts can only be part, negative or modifier. |
| `part.setFilament` | volumes | `filament` (0 = the object's, 1..n) | Parts and modifiers only. |
| `part.rename`, `part.delete` | volume / volumes | `name` / `breakCutLink?` | Object-list part rows; registered with the object actions, see `objects.md`. The part menu's Center, Drop, Mirror and Order are `object.center` / `object.drop` / `object.mirror` / `object.order` with `target.volumes`. |
| `part.setTransform` | volume | `positionMm`, `rotationDeg`, `scale` [x,y,z], `relative`, `frame` (world, object) | Values are absolute, on the plate; anything left out keeps its value. With `relative` they are amounts: a move, a turn (about X, then Y, then Z) and a scale about the part's centre, along the plate's axes (`frame: world`, default) or the part's own (`frame: object`), as the gizmos' coordinate combo; `frame: object` needs `relative`. A solid part is dropped back onto the bed when the object has auto-drop. |
| `part.splitToParts` | volume | (none) | One part per disconnected piece, inside the same object. |
| `part.mergeToSinglePart` | object | (none) | The "Mesh boolean" menu item: every part merged into one, negative parts subtracted, modifiers dropped. The object is replaced and its id changes. |
| `part.replace` | volume | `url` | "Replace 3D file": the file (one part) takes the old part's place, type, settings and paint. |
| `part.boolean` | volumes (exactly 2, same object) | `op` (union, difference, intersection), `keepTool` | The Mesh boolean tool. The first part is replaced by the result; the second is the tool, kept by default as the tool keeps it unless "Delete input" is ticked (`keepTool: false` removes it; a union always consumes it). Paint is kept. |
| `part.simplify` | volumes | exactly one of `ratio` (0.01..1, the fraction of triangles to **keep**), `triangles`, `detail` (extraHigh, high, medium, low, extraLow) or `maxError` | Quadric edge collapse, as the Simplify tool does. The tool's "Decimate ratio" is the percent **removed**, so its 30 % is `ratio: 0.7`. `detail` is the tool's Detail level: the quadric error allowed (1e-3, 1e-2, 0.1, 0.5, 1), with no target count; `maxError` is the same for any value. Parts already at or below the target are left alone (`changed: false`). |
| `part.smooth` | volumes | `allowLarge` | Subdivision: four triangles from each one. Past a million faces it answers 1011 unless `allowLarge`. A part with mesh errors reports one: run `part.fix` first. |
| `part.fix` | volumes | (none) | CGAL repair of parts with open edges; closed parts are left alone. Unlike the menu's repair, a part is not split into its pieces. |

The result is `{volume: {id, index, name, type, bboxMm, facets}}`, or `{volumes: [...]}` for
several parts. Mesh jobs add `changed`, `facetsBefore` and `facetsAfter`, or `error`, per part.

## object.*

| id | target | params | notes |
|---|---|---|---|
| `object.addShape` | none | `shape`, `sizeMm`, `positionMm` (x, y), `name` | "Add Primitive". New objects go to the nearest empty spot unless a position is given. |
| `object.addHandyModel` | none | `name` (3dBenchy, orcaCube, orcaSlicedCombo, orcaBadge, orcaToleranceTest, caliCat, autodeskFdmTest, voronCube, stanfordBunny, orcaStringHell), `arrange` | "Add Handy models". Arranges when the menu would; arranging is a plate job, and the action finishes when it ends. |
| `object.cut` | object | `z` or `plane: {point, normal}`; `spinDeg`; `mode` (planar, dovetail) and `groove`; `keep` (both, upper, lower); `keepAsParts`; `placeOnCut` (both pieces), `placeOnCutUpper`, `placeOnCutLower`, `flipUpper`, `flipLower`; `assign`; `instance`; `connectors` | The Cut tool's "Perform cut". The upper piece is on the normal's side. `spinDeg` turns the plane about its normal, which orients a dovetail groove and square, hexagon or triangle connectors; the result gives the plane's resulting `planeXAxis`. Per piece, place-on-cut and flip exclude each other, need the piece kept and do not apply with `keepAsParts`, as in the tool (1002 otherwise). **Dovetail**: `groove: {depthMm, depthTolMm (0.1), widthMm (4 x depth), widthTolMm (0.1), flapAngleDeg (30..120, 60), angleDeg (0..15, 0), count (1..100), gapMm (10)}`; the depth defaults to max(1, (x+y+z)/60) of the object's size; refused with connectors, `keepAsParts` or `assign`, as the tool disables them, and when the flaps do not fit the width. **Connectors** (`[{positionMm?, type: plug, dowel or snap, style: prism or frustum, shape: circle, square, hexagon or triangle, diameterMm, depthMm, diameterToleranceMm, depthTolerance, rotationDeg (0..180), bulgePct (5..50, 15), spacePct (10..50, 30)}]`; bulge at most space): both pieces are kept as separate objects, and the cut info is kept so the pieces stay matched. A connector's position defaults to where the plane crosses the object's center. **Cut by contour**: `assign: [{pointMm, side: upper or lower}]` -- the object is cut and split into its connected pieces as the tool does, each piece goes with the side it lies on, and each entry moves the piece nearest `pointMm` to `side` (the tool's right-click); the result lists what was `assigned`; refused when every piece ends up on one side. Connectors the Cut tool left on the object after it closed are dropped inside the same undo step (`pendingConnectorsDiscarded`). Refused (1002) when the plane misses the object. When some pieces have open edges, the result says so (`openEdges`, `hint`) instead of offering a repair. |
| `object.invalidateCutInfo` | object | (none) | Forget the cut relation of a piece and its siblings. Disabled (1016) for an object that is not from a cut. |
| `object.skeletonCutPlanes` | object | `method` (mcf or voxel), `replace`, `cutAt`, `preview`, and the Skeleton tool's settings: `contractionRate` (0.01..1), `voxelResolution` (32..256), `resampleEdgeLenMm`, `traceBores` (off, threshold, always), `bedRoot` (auto, middle, tips), `collapseToSpine`, `correctConvergence`, `smoothIterations` (0..50), `smoothAlpha` (0..1), `minEdgeFactor` (0.0001..0.02), `boreAspectMin` (0.2..5), `faceBudget` (0..40000), `pruneLeavesMm`, `tangentBlend` (0..1), `rootLeanDeg` (0..90), `rootRampMm` | The Skeleton tool's "Cut along pipe": one declarative cut plane per centreline edge (`slicer.setCutPlanes` edits them). `cutAt` adds only the plane of that edge in the cut order (Step + "Cut here"); `preview` reports every plane (world frame, with its tilt) and changes nothing. `faceBudget` decimates a dense mesh first, which is what makes a long build fit the timeout (it can change an organic part's skeleton). There is a limit of 64 planes. The undo point is taken when the planes are written. Cut planes print only with `print_sequence = "by group"`; the result has a `hint` when that is not set. |
| `object.cancelSkeleton` | none | `jobId` | The Skeleton tool's Cancel during a build: stops this tab's running `object.skeletonCutPlanes` (or the one `jobId` names) when its current stage ends; that build then answers 1006 and changes nothing. 1008 when none is running. |

## text.* and svg.*

| id | target | params | notes |
|---|---|---|---|
| `text.add` | object | `text`, `heightMm` (10), `depthMm` (1.5), `type` (part, raised (default); negative, engraved; modifier), `font`, `bold`, `italic`, `charGap`, `lineGap`, `boldness`, `skew`, `alignH`, `alignV`, `fontIndex`, `side` (front (default), back, left, right, top), `offsetMm` [along, up], `rotationDeg` | A ray from outside the chosen side finds the surface; the text lies flat on it there, upright. The result reports `surfaceHit`, `positionMm` and `normal`. The text is flat, not wrapped around a curved surface. `bold` / `italic` pick the face's bold or italic variant and, as the Text tool does, fall back to synthetic boldness (20) or skew (0.2) when the face has none. `charGap` / `lineGap` (font units), `boldness` (font units), `skew` (ratio), `alignH` / `alignV` are the tool's Advanced settings (0 = none); `fontIndex` picks a face of a .ttc collection. |
| `text.edit` | volume (a text) | `text`, `heightMm`, `depthMm`, `font`, `bold`, `italic`, `charGap`, `lineGap`, `boldness`, `skew`, `alignH`, `alignV`, `fontIndex` | Keeps its place, and keeps every setting not given -- a new font keeps the size, gaps, alignment and synthetic boldness / skew. `model.getTree` reports a text part's current settings under `text`. A text that follows the surface (made in the Text tool with "use surface") is refused: its mesh is cut from the object by the Text tool's own job, which needs the tool open. |
| `text.listFonts` | none | `query`, `limit` | Installed face names, plus the default. |
| `svg.add` | object | `url`, `sizeMm` (30, largest side), `depthMm`, `type`, `side`, `offsetMm`, `rotationDeg` | Filled and stroked paths, as the SVG tool reads them. The SVG is kept in the project. |
| `svg.edit` | volume (an SVG) | `sizeMm` or `widthMm` / `heightMm`, `depthMm`, `mirror` (x, y), `url`, `bake` | `sizeMm` scales the drawing uniformly (largest side); `widthMm` / `heightMm` scale the part along the drawing's own X / Y as the SVG tool's W / H fields do; `mirror` flips it along its own X or Y. `url` is the tool's "Change file": another .svg, downloaded on the tab's rules, at the same scale, keeping the part's place (long-running). `bake: true` alone is the tool's Bake: the mesh stays as a plain part and the SVG source is dropped. `model.getTree` reports `svg {sizeMm, depthMm, mirrored, useSurface}`. A surface-following SVG can only be baked. |

## Not automated here

- Gizmo navigation buttons (Cancel, Exit) are covered by `ui.closeGizmo`.
- "Face the camera" depends on the camera and is covered by `part.setTransform`.
- The Skeleton tool's single-plane steps are covered by `slicer.setCutPlanes`.
- "Replace all with 3D files" needs a folder (excluded, `os-dialog-replaced`).
- The tongue-and-groove cut mode and per-piece selection in the Cut tool are not exposed. A cut
  keeps every piece the plane makes.
