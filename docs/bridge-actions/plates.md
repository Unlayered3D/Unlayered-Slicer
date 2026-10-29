# Plates, layer events, preview and capture actions

Work package S5. Design: `docs/HLSD/bridge-actions.md`. Code: `src/slic3r/GUI/Actions/PlateActions.cpp`,
`LayerEventActions.cpp`, `PreviewActions.cpp`; descriptors (pure data) in `PlatesPreviewDescriptors.hpp`,
GUI-free helpers in `PlatesPreviewLogic.hpp`. Tests: `tests/slic3rutils/test_actions_plates.cpp` (`[actions]`),
E2E: `tools/bridge_e2e/cases/plates.yaml`. Coverage manifests: `resources/actions/surface/plate.json` and `preview.json` (the preview's ImGui
controls, which `tools/check_action_coverage.py` lists in `IMGUI_PREVIEW_CONTROLS`).

All of these run through `slicer.invokeAction` behind `<slot>_allow_settings`. Plates are 0-based
(`target: {"plate": 1}`); without a target the current plate is meant. Layers are 1-based.

**Layer numbers** mean the same layer in every action: the G-code preview's layer slider numbering.
The layers are built from the plate's slice result exactly as the preview builds them
(`PlatesPreview::preview_layers`): a new layer wherever the G-code's layer id changes, so ids the
file skips leave no empty layer, and a layer's z is the height of its last (non-custom) extrusion.
`layer.getEvents` / `layer.setEvents`, `preview.setView` (`layerRange`, `layerCount`),
`preview.getStats` (`layers`, `layerCount`, `slowestLayer`, `fastestLayer`) and `preview.getMove`
(`layer`) all use it. Raft and support-only layers are counted (they are on the slider). On a belt
printer the numbering is the same, but the preview shows z in its display frame while the other
actions report the G-code's own z.

## Plates

| Action | Risk | What it does (UI equivalent) |
|---|---|---|
| `plate.list` | read | Every plate: name, current, locked, objects (refs), instance count, slice state, `bedType` (own) and `effectiveBedType`, `printSequence`, `spiralVase` (`default`/`on`/`off`), `skirtStartAngle` (null = global), first/other-layer filament order, number of layer events. |
| `plate.add {name?, select=true}` | mutate | Toolbar *Add plate*. |
| `plate.delete` | mutate | Plate menu *Delete Plate*. Objects on it move off the plates (as by hand); undoable. The last plate cannot go (1016 `last_plate`). |
| `plate.duplicate` | mutate | Edit > *Duplicate Current Plate* (any plate). Takes an undo snapshot, which the menu item does not. |
| `plate.select` | ui | Click a plate. With Preview shown, the preview reloads to the plate's toolpaths (or its shells when it is not sliced) as clicking its thumbnail in Preview does, but it never slices (`previewReloaded`, `previewShowsToolpaths`). |
| `plate.move {toIndex}` | mutate | The plate's *Move plate to the front* icon, for any place (`toIndex: 0` = front). Every plate in between shifts by one, so the answer is the new `plates` list with each plate's `previousIndex`. Undoable. Only one plate: 1016. |
| `plate.setWipeTower {x, y}` | mutate | Dragging the prime tower: its position in mm from the plate's front-left corner (per-plate project setting `wipe_tower_x/y`). A position where the tower would not fit is moved onto the plate as the scene does (`clamped: true`, `requested`). Also accepted while the prime tower is off (`primeTowerEnabled: false`, `hint`); it applies once `enable_prime_tower` is on. Undoable. |
| `plate.rename {name}` | mutate | Plate name edit. |
| `plate.lock {locked}` | mutate | Plate lock icon. |
| `plate.setSettings {bedType?, printSequence?, spiralVase?, skirtStartAngle? \| resetSkirtStartAngle?, firstLayerFilamentOrder?, otherLayersFilamentOrder?, name?, locked?, filamentMapMode?, filamentMaps?}` | mutate | The *Plate Settings* dialog and the plate's settings tab, minus the dialog. `default` = same as global. `printSequence`: `default`, `byLayer`, `byObject`, `byGroup` (objects print group by group by their `print_group`). `bedType` other than `default` is refused (1016 `bed_type_unavailable`) where the dialog greys the choice out (printers outside the built-in vendor) or for a type the printer model does not support. `skirtStartAngle` (-180..180 degrees) is the plate tab's skirt start point; `resetSkirtStartAngle: true` returns to the global value. A custom filament order (non-empty) is refused (1016 `mixed_filaments`) while the filament list holds a mixed filament, where the dialog says it would not take effect. `spiralVase: "on"` also writes the per-object overrides the dialog asks about, listed in `objectOverridesSet` (undoable). `filamentMapMode` (`default`, `autoForFlush`, `autoForMatch`, `manual`) with `filamentMaps` (manual: the 1-based nozzle of each filament slot) is the *Filament Grouping* dialog and the legend's *Set to Optimal* (= `autoForFlush`), multi-nozzle printers only (1016 `single_nozzle`); it invalidates the slice, and `slice.run` re-slices. The per-nozzle manual mode is not offered. Answers `applied` (keys that changed). |
| `plate.selectAll {allPlates=false}` | ui | Plate menu *Select All* / *Select All Plates*. |
| `plate.reloadAll` | mutate | Plate menu *Reload All*. Refused (1016 `sources_missing`, `volumes`) when a source file is missing or is an `.obj`: by hand the slicer would open a file or colour dialog. |
| `plate.autoOrient` | mutate, long | Plate menu *Auto Rotate*; answers when the job is done with each object's rotation. |
| `object.autoOrient` | mutate, long | Toolbar *Auto orient* on the given objects. |
| `object.toPlate {plate \| newPlate, arrange=false}` | mutate | Move objects or single instances (`target.instances`) to a plate. Instances keep their position relative to their plate; one on no plate lands centred. Refused onto a locked plate. `arrange` arranges the destination afterwards (long). |
| `slice.all` | mutate, long | *Slice all*: local slicing of every printable plate, answering when all are done (`plates[{plate, sliceResultValid}]`, `complete`). Never a slicing-service (cloud) run (decision D7): refused with `1014` (`data.reason: "service_mode_selected"`) while a slicing-service mode is selected. |

`plate.clear` (plate menu *Delete All*) and `plate.arrange` belong to the objects package and the
existing `slicer.arrange` (which also takes the Arrange options popup's `allowMultiMaterials`,
`avoidCalibrationRegion` and `alignToYAxis`); the manifest maps those menu items to them. The icons
on each plate on the bed are scanned as `Plater.cpp::bedicon::hover:<n>` (0 select, 1 delete,
2 orient, 3 arrange, 4 lock, 5 plate settings, 6 name, 7 move to front; the dual-nozzle filament
grouping icon is excluded as deferred-printer).

Example, "put the second copy on a new plate with textured PEI":

```json
{"id": "object.toPlate", "target": {"instances": [{"object": {"index": 0}, "index": 1}]}, "params": {"newPlate": true}}
{"id": "plate.setSettings", "target": {"plate": 1}, "params": {"bedType": "texturedPEI"}}
```

Bed type ids: `default`, `coolPlate`, `engineeringPlate`, `highTempPlate`, `texturedPEI`, `texturedCoolPlate`,
`supertack` (the config names such as `"Textured PEI Plate"` are accepted by the runner too).

## Layer events

The preview layer slider's per-layer events, stored in the project per plate.

- `layer.getEvents` (read): `{plate, sliced, layerCount, topZ, mode, printSequence, events:[{type, z, layer, filament?, color?, gcode?, message?}]}`.
- `layer.setEvents {items?, mode: add|replace, remove?: [z]}` (mutate). Item types:
  - `pause` (`message` optional): the printer profile's pause G-code at that layer. A warning says so when that G-code is empty.
  - `toolChange {filament, color?}`: *Change Filament* to a slot; needs two or more slots and a plate printing from one filament (as the slider). Refused in spiral vase (1016 `spiral_vase`): the slider offers no filament change there and drops such ticks on the next preview load.
  - `custom {gcode}`: free G-code text (at most 64 lines / 4096 bytes, no control characters). **The person confirms in a native dialog that lists every line** (decision D5); declined = 1006, nothing changes.
  - `template`: the printer's template custom G-code; refused when the profile has none.
  - Position: exactly one of `z` (mm) or `layer` (1-based, the slider's numbering). Once the plate is sliced, `z` snaps to the nearest layer; `layer` needs a slice. Unsliced, `z` is kept and applies to the nearest layer when sliced.
  - `mode: add` keeps the other events (one event per layer: the new one replaces one at the same z); `replace` makes the items all the events (`items: []` clears).
- Not in by-object print sequence (1016 `print_sequence_by_object`), as on the slider.
- Colour change (M600) items are not offered: this slicer writes nothing for them.
- **Undo**: like the slider's own edits these are not on the undo stack, so `slicer.undoTurn` does not take them back. Every reply carries `previous`; `layer.setEvents {items: previous, mode: "replace"}` restores.
- The change invalidates the plate's slice and refreshes the Slice/Print buttons (as a tick change does). The processed G-code reports pauses and custom codes in `preview.getStats.events`.

## Preview

- `preview.getStats {layers=false, maxLayers=2000, timeMode=normal, allPlates=false}` (read): the plate's slice summary (`timeSeconds`, `prepareTimeSeconds`, filament, cost...) plus, as the preview legend shows them:
  - `totalTimeS`, `features{<role>: {timeS, percent, filamentMm, volumeMm3, pathMm}}`, `travel{timeS, distanceMm, moves}`;
  - `options{retract, unretract: {count, timeS, percent, filamentMm}, wipe, seams: {count, timeS, percent, distanceMm}}` (seams also `gapMm`, `scarfMm`);
  - `filamentChangeTime{loadS, unloadS, toolChangeS, totalS, changes, extruderChanges}` (the *Filament changes* row);
  - `otherTimeS`, `events{pauses, toolChanges, customGcodes, colorChanges}`;
  - `layerCount`, `slowestLayer`, `fastestLayer`, optional `layers[{layer, z, timeS}]` (slider numbering, no empty layers);
  - `ranges{layerHeight, lineWidth, speed, actualSpeed, flow, actualFlow, fanSpeed, temperature, pressureAdvance, acceleration, jerk, layerTime: {min, max, unit}}`: the colour scales, over the extrusion moves (travel and wipe hidden, as by default), not rounded into the legend's bins;
  - `filaments[{filament, model, support, flushed, primeTower, total: {mm3, m, g}, cost, totalMm3, modelMm3, supportMm3, primeTowerMm3, flushMm3}]`: the legend's filament table per slot (m and g from the slice's own filament diameter and density, cost from its cost per kg).
  - `timeMode: "stealth"` gives the stealth-mode times (1016 `no_stealth_times` when the G-code has none).
  - `allPlates: true`: the *All Plates Stats* totals `{plates[{plate, timeS, cost}], timeS, cost, filaments[...]}` over every plate with objects. Refused (1016 `not_sliced`, `unsliced: [plates]`) while any of them is unsliced, as the window is.
  - Unsliced plate: 1016 `not_sliced`.
  Feature ids: `innerWall`, `outerWall`, `overhangWall`, `sparseInfill`, `internalSolidInfill`, `topSurface`, `bottomSurface`, `ironing`, `bridge`, `internalBridge`, `gapFill`, `skirt`, `brim`, `support`, `supportInterface`, `supportTransition`, `primeTower`, `custom`.
- `preview.setView` (ui) does what the preview's legend and sliders do. Everything is checked first; a refusal changes nothing (a switch to Preview made to load the toolpaths is switched back).
  - `show: prepare|preview`: switches the plater's panel without slicing (a stale plate shows its shells). It does not move the main window's tab header; `ui.showTab {tab: "preview"}` goes through the notebook like a click on the tab, which re-slices a stale plate.
  - `viewType`: the legend's colouring combo, combo label included: `summary`, `featureType`, `filament`, `speed`, `actualSpeed`, `layerHeight`, `lineWidth`, `flow`, `actualFlow`, `layerTime`, `layerTimeLog`, `fanSpeed`, `temperature`, `pressureAdvance`, `acceleration`, `jerk`, `section`. `section` only when the print has more than one section (1016 `view_type_unavailable` with `offered`). Loading new G-code can change the colouring: an unavailable one falls back to `featureType`, and a change between one and several filaments picks `filament` / `featureType`.
  - `layerRange: [first, last]` or `zRange: [from, to]` (nearest layers; out-of-range values are clamped, with a note). `oneLayer: true|false` is the slider's one-layer button: true shows the top of the range (`[n, n]`) or the current top layer; false shows every layer again.
  - `move: {index} | {to: "start"|"end"} | {step}`: the moves slider (the top shown layer's moves; `index` 1-based, `step` stops at the layer's ends). The reply adds the `vertex` (as `preview.getMove`).
  - `features: {<feature id>: bool}`, `showAllFeatures`: the feature-type rows. A later `viewType: "featureType"` shows them all again (as the combo does).
  - `options: {travel, wipe, retract, unretract, seams, filamentChanges: bool}`: the option rows (travel is also the Travel row of the speed, acceleration and jerk views). Kept across colourings.
  - `sections: [n...]` (1-based) with the section colouring: those sections shown, the rest hidden. Any other colouring shows every section again.
  - `timeMode: normal|stealth`: *Show stealth mode* / *Show normal mode*.
  - `beltRaw: bool`: a belt printer's *Show raw machine G-code* (1016 `not_belt_print` otherwise). The preview reloads afterwards, so the layer range and move may reset.
  - Everything but `show`, `viewType` and `showAllFeatures` needs the plate's toolpaths loaded (1016 `preview_not_loaded`).
  - Answers the view state: `tab`, `previewLoaded`, `viewType`, `viewTypes` (offered), `layerCount`, `layerRange`, `zRange`, `oneLayer`, `move{index, count}`, `options`, `features`, `sections[{section, visible}]`, `timeMode`, `stealthTimes`, `beltRaw` (belt G-code only), `notes`.
- `preview.getMove {gcodeLines=false, before=5, after=5}` (read): the tool-position window. `move{vertex, type, position, layer, tool, feature, widthMm, heightMm, lengthMm, speedMmS, actualSpeedMmS, flowMm3S, accelerationMmS2, jerkMmS, fanPercent, temperatureC, pressureAdvance, moveTimeS, elapsedTimeS, layerTimeS, section, gcodeLine}`, the `slider{index, count}`, `viewType`, `timeMode`, `plate`. At a seam the window shows the last real move instead (`atSeam: true`). `gcodeLines: true` adds `gcodeLines[{line, text, current}]` (at most 50 before and 50 after, each cut at 200 bytes), the G-code window's text. **The G-code text is gated exactly like G-code export (`slicer.exportGcode`): the tab's printer permission** (1005 `printer_permission_off` without it). Needs the plate's toolpaths loaded (1016 `preview_not_loaded`).

## Capture

`view.capture {canvas: prepare|preview, camera?, projection: ortho|perspective, width, height (64-1024), showBed, transparent}` (read) answers `{mime: "image/png", width, height, sizeBytes, base64, canvas, plate, camera}`; at most 1024 px a side and 1.5 MB (halved until it fits).

- `prepare` (default): the plate rendered off-screen with the plate-thumbnail renderer from `camera` (`iso` default, `topFront`, `top`, `bottom`, `front`, `rear`, `left`, `right`). Works while a web tab covers the canvas; the user's camera does not move.
- `preview`: the Preview canvas as on screen (toolpaths, legend, sliders), read back after one frame. The Preview canvas must really be on screen, not just selected under the Home page or a web tab (1016 `preview_not_shown`; `ui.showTab {tab: "preview"}` or `preview.setView {show: "preview"}` first), and only the current plate can be captured. `camera` here moves the person's preview camera like View > *direction*. `projection`, `showBed` and `transparent` are refused here (1002): the picture is the canvas as drawn.

## Open items

- The pause-in-G-code E2E check reads the processed toolpaths (`preview.getStats.events.pauses`), because `slicer.exportGcode` always opens the native Save dialog; a file check can follow once the outward-file export (`file.export`) has the E2E save-dir hook.
- The legend's *Actual speed profile* plot (a debug readout) is not offered.
