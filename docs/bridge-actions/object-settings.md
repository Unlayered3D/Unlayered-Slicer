# Object settings, height ranges and variable layer height (automation actions)

Owner: work package S4. Design: `docs/HLSD/bridge-actions.md` (catalog, targets, undo, turns).
Source: `src/slic3r/GUI/Actions/ObjectSettingsActions.cpp`, `LayerHeightActions.cpp`, the pure rules in
`ObjectSettingsRules.hpp` (unit tests: `tests/slic3rutils/test_actions_objectsettings.cpp`, tag
`[objectsettings]`). E2E: `tools/bridge_e2e/cases/object_settings.yaml`.

These are the object list's per-object settings, called through `slicer.invokeAction`:

| Action | Risk | Target | What it is in the GUI |
|---|---|---|---|
| `object.getSettings` | read | one object | Settings shown under an object, its "Layers" node, Copy Process Settings |
| `object.setSettings` | mutate | one object | Add Settings, Edit Process Settings, Edit in Parameter Table, Paste Process Settings, Flush Options; with `layerRange`: a height range's settings |
| `part.getSettings` | read | one volume | Settings shown under a part or modifier |
| `part.setSettings` | mutate | one volume | Add Settings on a part or modifier |
| `layer.addRange` | mutate | one object | Height Range Modifier, "add range" |
| `layer.removeRange` | mutate | one object | Delete a range (or all) |
| `layer.editRange` | mutate | one object | Editing a range's bounds |
| `object.layerHeightProfile` | mutate | objects | Variable layer height: Adaptive / Smooth / Reset |
| `object.setBuildDirection` | mutate | one object | Cut Planes tool: "Use plane as build direction" / "Reset direction" |
| `part.setBuildDirection` | mutate | one volume | The same with one part selected |

Every mutate action takes one undo snapshot ("<label> (automated)"), so `edit.undo` and
`slicer.undoTurn` restore it. None asks for confirmation. All of them are disabled (1016,
`data.reason`) unless the printer is FFF. Effects: `settings`. The overrides show in
`slicer.getModelTree` (`overrides`, `layerRanges`).

## Which keys can be overridden where

The rule is `ObjectSettings::check_key(key, level, filamentCount, write)`. It mirrors the object list,
`SettingsFactory::get_options` / `get_bundle` and `is_improper_category`, and adds the bridge's deny
lists:

| Level | Keys offered |
|---|---|
| object | every `PrintRegionConfig` + `PrintObjectConfig` option (walls, infill, speeds, supports, brim, seam, layer height, flush options, ...) |
| part / modifier | `PrintRegionConfig` options (walls, infill, top/bottom, ironing, speeds), plus `layer_rotation_b` / `layer_rotation_c` on a solid part (a directed part: see "Build direction"). Object-wide options are refused with the hint "set it on the object" |
| height range | `PrintRegionConfig` options, plus `layer_height` and `extruder` (0 = the object's filament), which every range always has |

Each rejected key appears in `rejected[]` as `{key, code, message}`. The other keys are still applied.
The codes are:

- **1008**: unknown option.
- **1003**: deny-listed, the same lists as `slicer.setSettings` (`WebAppSettings::key_writable`). That covers secrets / print host, custom G-code and post-processing, preset identity, read-only and plugin-backed options.
- **1002**: not settable at this level. That is a printer or filament option, an object-wide option on a part or range, an option with no category (the object list never offers it), an `Extruders` / `Wipe options` option while only one filament is loaded, a value that doesn't parse, a value out of range, or a range layer height outside the printer's `min_layer_height`/`max_layer_height` (a max of 0 means 75 % of the nozzle).
- **`extruder` on an object or part** is refused. The filament column owns it (`part.setFilament`, or the existing object filament method).

If nothing at all was applied or removed, the call fails with 1002 "no value was applied" and
`data.rejected`.

## Values

Values are strings in the option's text form, as in a project file. The schema dialect has no
free-form maps or type unions, so `values` is a list of `{key, value}`:

```json
{"id": "object.setSettings", "target": {"object": {"id": 1234}},
 "params": {"values": [{"key": "wall_loops", "value": "4"},
                       {"key": "sparse_infill_density", "value": "30%"},
                       {"key": "flush_into_infill", "value": "1"}],
            "remove": ["top_shell_layers"]}}
```

- `remove` lists overrides to drop (the value is inherited again). `clear: true` drops every override first, but keeps `extruder`, and a range's `layer_height`.
- The result is `{level, object, volume?, layerRange?, applied, removed, rejected, overrides, undoable}`.

## Reading

`object.getSettings {keys?, includeAllowed?, allowedQuery?, allowedCategory?, layerRange?}` returns:

- `overrides` as `{key: text}`;
- `effective[key] = {value, source: object|part|layerRange|preset}` for `keys` (up to 64);
- for an object, also:
  - `layerRanges[{zMin, zMax, overrides}]`;
  - `layerHeightProfile {variable, points, minLayerHeight, maxLayerHeight}`;
  - `partsWithOverrides`.

`includeAllowed` lists the options settable at that level. A filtered list of 60 or fewer comes with
labels and categories. A longer one is bare keys, capped at 200 with `truncated`. Narrow it with
`allowedQuery: "infill"` or `allowedCategory: "Support"`.

## Height ranges

```json
{"id": "layer.addRange", "target": {"object": {"index": 0}},
 "params": {"zMin": 30, "zMax": 40, "values": [{"key": "sparse_infill_density", "value": "50%"}]}}
```

- Bounds are mm from the object's bottom. A range must start below the object's top and must not overlap another range; touching is fine.
- `layerHeight` defaults to the object's layer height.
- Change a range's settings later with `object.setSettings` + `layerRange: {zMin, zMax}`. Bounds match within 1e-4 mm.
- An unknown range answers 1008 with the existing `layerRanges`.
- `layer.removeRange` takes either `{zMin, zMax}` or `{all: true}`, exactly one (`x-exactlyOne`).
- `layer.editRange {zMin, zMax, newZMin, newZMax}` moves a range and keeps its settings.

"More infill only in the top 10 mm" means reading the object's height from `getModelTree` (bbox),
then calling `layer.addRange {zMin: h-10, zMax: h, values:[sparse_infill_density]}`.

## Variable layer height

`object.layerHeightProfile {mode: adaptive|smooth|reset, quality?, radius?, keepMin?}` runs the
libslic3r functions the variable layer height tool uses: `layer_height_profile_adaptive` and
`smooth_height_profile`. It uses the same `PrintObject::slicing_parameters` and writes
`ModelObject::layer_height_profile`.

- `quality` runs from 0 (speed) to 1 (quality), default 0.5.
- `radius` runs from 1 to 10 layers, default 5.
- A height range inside the profile region still wins, as in the GUI.
- `reset` clears the profile.
- The tool's cached profile is dropped through the background-process scheduling, so an open layer-editing view shows the new one.

Brush strokes (add/remove detail at the cursor) are not exposed. Height ranges express the same intent
reproducibly.

## Build direction

`object.setBuildDirection` / `part.setBuildDirection` are the Cut Planes tool's "Use plane as build
direction" and "Reset direction", for the object or for one solid part. Exactly one of:

- `normal` `[x, y, z]`: the world direction that becomes "up" when slicing, e.g. a face's outward
  normal from `geometry.query planarRegions`;
- `bDeg` (-180..0) / `cDeg` (-180..180): the machine angles directly;
- `fromPlane`: the index of one of the object's cut planes, whose normal (arrow side) is used, as
  the tool uses its editing plane;
- `reset: true`: back to the default (the object builds along +Z; a part builds with its object).

The direction is stored as `layer_rotation_b/c` in the object's or part's config, the same values
the process settings show under Others > Multiplanar, under one undo step named like the tool's.
On a virtual 5-axis printer it is held at the printer's tilt limit for the object's footprint (the
tool's `object_tilt_limit_deg`); the reply reports `tiltClamped {tiltDeg, limitDeg, b, c}`. A
directed **part** is sliced as its own section after the rest of the object, which needs
`print_sequence = "by group"`: for the first directed part of an object the reply carries a `hint`
instead of the tool's "Change print sequence?" dialog. An object direction while the object's cut
planes are enabled comes with the tool's `note` that it is not used then.

`object.setSettings` / `part.setSettings` with `layer_rotation_b/c` apply the same clamp and hint.

## Not here

- Copying and pasting settings between objects is a `getSettings` followed by a `setSettings`.
- The object or part filament belongs to S3 (`part.setFilament`) and to the existing object filament method.
- Plate settings belong to S5 (`plate.setSettings`).
