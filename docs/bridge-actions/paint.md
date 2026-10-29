# Painting and geometry actions

Domains `paint`, `geometry` and the action `object.brimEars`. Source: `src/slic3r/GUI/Actions/PaintActions.cpp`, `GeometryActions.cpp`; the geometry itself is pure libslic3r in `src/libslic3r/FacetSelectors.{hpp,cpp}` (unit tests: `tests/libslic3r/test_facet_selectors.cpp` `[facets]`, `tests/slic3rutils/test_actions_paint.cpp` `[actions]`).

A web application cannot draw brush strokes, so painting is driven by **selectors**: predicates over the mesh's original facets. Everything is in the plate (world) frame, in millimetres: +X right, +Y rear, +Z up. Each vertex goes through `instance matrix * volume matrix`, and each facet normal through the inverse transpose of its linear part, so mirrored and non-uniformly scaled parts get correct normals.

## Actions

| id | risk | target | what it does |
|---|---|---|---|
| `paint.apply` | mutate | objects | paint `layer` with `state` on the facets `where` picks |
| `paint.clear` | mutate | objects | erase one layer, or all of them |
| `paint.remapFilaments` | mutate | objects | relabel colour-painted filaments (`pairs:[{from,to}]`, swaps allowed) |
| `paint.stats` | read | objects | painted facets and area per volume, layer and state |
| `geometry.query` | read | objects | `bbox`, `overhangs`, `sides`, `planarRegions`, `heightAt`, `features` |
| `geometry.measure` | read | two objects | closest surface distance and points, bounding-box gap, centroid distance |
| `object.brimEars` | mutate | object | ears at given plate points, at the first layer's sharp corners (`auto`), `remove` the ears at points, or `clear` |

Mutating actions take one undo snapshot, `"<label> (automated)"`, and only when something actually changes. Repeating the same paint is a no-op: no snapshot, `changed:false`, and `effects:[]`. A painter gizmo that is open blocks every mutate action (1007, `data.gizmo`), because the gizmo owns the paint state while it is open.

## paint.apply

```json
{ "id": "paint.apply",
  "target": {"objects": [{"id": 1234}]},
  "params": { "layer": "seam", "state": "enforce", "mode": "add",
              "where": {"all": [ {"normal": {"side": "rear", "withinDeg": 60}},
                                 {"not": {"normal": {"side": "top", "withinDeg": 30}}} ]} } }
```

- **`layer`**: `support`, `seam`, `fuzzy` or `color`.
- **`state`**:
  - `enforce` and `block` for support and seam;
  - `enforce` for fuzzy;
  - `filament` with `filament: n` (1-based) for color;
  - `erase` for any layer.
- **`mode`**:
  - `add` paints the matched facets.
  - `replace` first removes this state everywhere on the layer, then paints. Other states stay.
  - `clear` erases the matched facets, whatever their state.
- **`volumes`**: limits painting to these part ids. Modifiers and negative parts are never painted, the same as in the gizmos.
- **`instance`**: picks the instance whose placement defines the world frame. The default is 0.
- **`dryRun`**: reports `hits`, `hitAreaMm2` and the would-be per-state statistics (`wouldChange` per volume), without writing anything.
- **Result**:
  - `{layer, mode, dryRun, changed, undoable, hits, hitAreaMm2, objects:[{id,index,name, volumes:[{id,index,name,hits,hitAreaMm2,changed,states}]}], hints}`.
  - `states` maps `"enforce"` / `"block"` / `"fuzzy"` / `"filament 3"` to `{facets, areaMm2}`.
- **`hints`** say why the paint may not show in the print:
  - supports are off, or the support type is automatic;
  - fuzzy skin painting is disabled;
  - the filament slot does not exist;
  - the selector matched nothing.

v1 paints whole original facets. If a brush-subdivided facet ends up all one state, it is merged back.

## Selectors (`where`)

Exactly one key per selector object.

| selector | params | picks |
|---|---|---|
| `overhang` | `maxAngleDeg` 0-90 | downward facets within that angle of straight down (the gizmo's rule). "Overhangs steeper than 50 degrees from vertical" is `maxAngleDeg: 40` |
| `normal` | `side` (rear/front/left/right/top/bottom) or `direction:[x,y,z]`; `withinDeg` (45) | facets facing that way |
| `zRange` | `[zMin, zMax]` | facets whose centroid height is in range |
| `box` | `min`, `max`, `test` | facets inside an axis-aligned box |
| `sphere` | `center`, `radius`, `test` | facets inside a sphere |
| `halfspace` | `point`, `normal`, `test` | facets on the side the normal points to |
| `facets` | `volume` (id) or `volumeIndex`, `indices` | explicit facets, e.g. from `geometry.query planarRegions` |
| `planarRegion` | `seedSide` or `seedPoint`, `toleranceDeg` (5) | a flat face, flood-filled over shared edges from a seed facet. `seedSide` uses the outermost facet facing that side; `seedPoint` uses the facet closest to the point |
| `smoothRegion` | `seedSide` or `seedPoint`, `maxNeighbourAngleDeg` (30) | the painting tools' **Smart fill**: flood-filled over shared edges while each facet's normal is within the angle of the neighbour it is reached from (not of the seed), so a cylinder wall or a fillet fills as one region where `planarRegion` stops |
| `all` | `[selectors]` | the intersection. `[]` = every facet |
| `any` | `[selectors]` | the union |
| `not` | selector | the complement |

`test` (spatial selectors) is `centroid` (the default), `all` corners or `any` corner.

The catalog schema allows two levels of nesting: `all` / `any` around leaves, and `not` around a leaf. The C++ parser accepts deeper nesting, up to 16 levels.

Errors name the failing node, e.g. `data.path = "/params/where/all/0/normal/side"`.

**Examples:**

- Seam on the back: `{"normal":{"side":"rear","withinDeg":60}}`, `layer:"seam"`, `state:"enforce"`.
- Support blockers below 30 mm: `{"zRange":[0,30]}`, `layer:"support"`, `state:"block"`.
- Support only under steep overhangs: `{"overhang":{"maxAngleDeg":40}}`, `state:"enforce"`, `mode:"replace"`.
- Paint the top face filament 2: `{"planarRegion":{"seedSide":"top"}}`, `layer:"color"`, `state:"filament"`, `filament:2`.

## geometry.query

`params.query` picks the query:

- **`bbox`**: the object's box and each part's box (`min`, `max`, `size`, facet counts).
- **`overhangs`** `{maxAngleDeg=45, limit=10}`: total facets and area, `zMin` / `zMax`, and the largest connected regions.
- **`sides`** `{withinDeg=45}`: per named side, the area facing it and `extreme`, the outermost coordinate along that axis (max y for rear, min y for front, ...).
- **`planarRegions`** `{toleranceDeg=5, minAreaMm2=1, limit=20}`: flat faces, largest first, with `normal`, `side` (when within 10 degrees of one), `centroid`, box and `areaMm2`. Regions of 64 facets or fewer also carry `indices`, which can be used directly as a `facets` selector.
- **`features`** `{limit=20}`: the Measure tool's features (libslic3r `Measure::Measuring`, what it highlights under the cursor): `circles [{center, diameterMm, axis}]` -- hole and boss rims, largest first -- and straight `edges [{from, to, lengthMm}]`, longest first, each reported once, plus `planeCount`. Distances and angles between them (the tool's perpendicular / direct / XYZ readouts) follow from these points and axes.
- **`heightAt`** `{x, y}`: every surface the vertical line through (x, y) crosses, as `{z, facing: up|down, volume}`, plus `top`.

`includeModifiers: true` includes modifiers and negative parts. `volumes` limits the query to those part ids.

## geometry.measure

The target is exactly two objects. The result is:

- `distanceMm`: the closest surface distance, from vertex-to-surface checks run both ways. It does not detect one body inside the other.
- `closest {a, b}`: the two closest points.
- `bboxGapMm` per axis: negative values mean overlap.
- `centroidA`, `centroidB`, `centroidDistanceMm`.

## object.brimEars

- **`mode: points`** with `points:[{x,y}]`: adds ears at those plate positions, mapped into object coordinates as the gizmo does.
- **`mode: auto`** `{maxAngleDeg=125, detectionRadiusMm=1}`: the tool's Auto-generate. The first layer is sliced 0.1 mm above the bed (parts minus negative volumes); its outlines, simplified at `detectionRadiusMm`, get an ear at every convex corner sharper than `maxAngleDeg`, and holes at every corner that is sharp seen from the material (libslic3r/BrimEarsAuto, shared with the tool).
- **`mode: remove`** `{points, toleranceMm?}`: the tool's Remove selected: removes the ears whose centre is within `toleranceMm` (default: the ear's own radius) of a point.
- **`mode: clear`**: removes every ear.

`model.getTree` lists an object's ears as `brimEars [{x, y, radiusMm}]` (plate frame).
- **`radiusMm`**: defaults to 8 first-layer line widths, the gizmo's default.
- **`replace: true`**: drops the existing ears first.

Ears are printed only with `brim_type = painted`. A hint says so.

## Not automated

- Freehand brush strokes: selectors replace them (`smoothRegion` is the smart fill).
- **Bucket fill** (MMU, the same-colour connected region, with edge detection): not yet. It needs a selector over the current paint state (`painted {layer, state}`), which the facet selectors cannot see: they evaluate original facets, while the state lives in the brush-subdivided TriangleSelector tree.
- **Height range** cursor: `zRange` selects whole original facets (by centroid, or all / any corners); it does not split facets at the bounds as the tool's cursor does, so on a coarse mesh the painted band follows facet edges.
- **Gap fill** (Gap area + Apply): not yet. The island merge is computed on the gizmo's render patches (`TriangleSelectorPatch::update_triangles_per_patch` / `update_selector_triangles` in the GUI) and has to move to libslic3r first.
- The gizmos' "reset direction" (view-only).
- The painter debug buttons.
- Interactive Measure and Assembly picking: `geometry.measure` plus `object.setTransform` replace them.

The coverage manifest is `resources/actions/surface/paint.json`.
