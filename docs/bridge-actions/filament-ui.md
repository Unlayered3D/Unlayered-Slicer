# Bridge actions: filaments, sidebar, calibration, view, preferences

Actions in `src/slic3r/GUI/Actions/{FilamentActions,CalibrationActions,ViewActions,PreferenceActions}.cpp`.
The pure rules behind them are in `Actions/FilamentUiRules.hpp` and are tested by
`tests/slic3rutils/test_actions_filament.cpp` (`[actions]`). The end-to-end case is
`tools/bridge_e2e/cases/filament_ui.yaml`.

They are called through `slicer.invokeAction {id, target?, params?, turn?, ifDirty?, ifProjectDirty?}`
(see `docs/HLSD/bridge-actions.md`). Slots are **0-based config indices**, as in `slicer.selectPreset`
and `slicer.addFilamentSlot`: physical filaments come first, then mixed (virtual) ones.
`slicer.setObjectFilament` keeps its 1-based `filament` numbering.

## Filaments and the printer box

| id | risk | what it does (the control it stands for) |
|---|---|---|
| `filament.delete {slot, replaceWith?}` | destructive, native confirm | Sidebar "-" / a mixed row's delete. Objects and painting on `slot` move to `replaceWith` (else each object's default). The last physical filament cannot go. |
| `filament.removeSlots {slots \| keep, replaceWith?}` (0.1.8) | destructive, ONE native confirm | Several slots at once: `slots` to remove, or `keep` (every other slot goes). One confirmation lists them ("Remove 8 filament slots from the project?" + one line per slot, 10 shown, then "+N more"); then the same removal as `filament.delete`, highest index first, so objects and painting fall back exactly as there. No undo snapshot (as `filament.delete`). Declined: 1006, nothing removed. 1002 (`data.path`) for an index out of range or listed twice, nothing to remove, every slot or every physical slot, or a `replaceWith` that is itself removed. Result `{removed:[{slot, preset, mixed}], replacedBy, objectsMoved, slots}` (indices from before the call). getInfo feature `filament.removeSlots`. |
| `filament.merge {from, to}` | destructive, native confirm | Filament menu > Merge with. Refused (1002) when `to` is a mixed filament made from `from`, where the sidebar would warn in a modal. |
| `filament.setColour {slot, colour}` | mutate | The colour picker. `colour` is `#RGB`/`#RRGGBB`. Mixed slots take their colour from their components (1002). Re-runs the automatic flushing volumes like the picker. |
| `filament.getFlushVolumes {}` | read | `{slots, nozzles:[{nozzle, multiplier, matrix}], modified}`; matrices are p x p over the physical slots, row = from, column = to, mm³. |
| `filament.setFlushVolumes {mode, matrix?, multiplier?, nozzle?}` | mutate | The Flushing volumes dialog. `auto` = its auto-calculation from the colours; `matrix` writes the p x p matrix (diagonal ignored; mixed slots' cells untouched). |
| `filament.setPurgeMode {mode}` | mutate | The Purge mode dialog: `default`, `saving` (multi-nozzle extruders), `fast` (printers with fast purge). Disabled on printers that have neither. |
| `filament.addMixed {components, ratios?, gradient?, gradientReverse?, gradientPerPart?}` | mutate | The mixed-filament "+" without its dialog: the same project-config writes as the dialog's OK (no custom gradient curve). |
| `filament.editMixed {slot, components?, ratios?, gradient?, gradientReverse?, gradientPerPart?}` | mutate | A mixed row's Edit without its dialog: the same writes as its OK at the slot's own index, so every object, part and painted area on the slot keeps it. What is not given stays; new `components` without `ratios` get equal shares; a gradient needs exactly two components; a custom gradient curve survives an edit that keeps the gradient as it was. `{changed:false}` when nothing differs. |
| `filament.decomposeColour {slot?, colour?, materialType?, mode?, dryRun?}` | mutate (native confirm when filaments are added) | Filament menu > Decompose Color. One of `slot` (a physical slot, left out of the mix) or `colour` is needed. `materialType` is the dialog's material selector (the project's types, or `"PLA Basic"`; default the slot's type); `mode` is `materialList` (needs two other filaments of that type), `cmyw` or `rybw` (PLA Basic only); default the first offered. Standard colours the project lacks are added as physical slots after a native confirmation, then the mixed filament is created. Returns the mix (`components` with `slot` or `new`, `ratios`, `matchedColour`), plus `slot`, `created`, `slots`. `dryRun` reports the mix only. 1002 for a material without modes (`data.offered`), a colour that already is a standard colour (`single-component`), or the filament limit. |
| `printer.setNozzle {diameterMm}` | mutate | The nozzle box: switches to this printer family's preset for that diameter (honours `ifDirty`). 1008 when no such variant is installed. |
| `bed.setType {type}` | mutate | The plate-type box: `coolPlate`, `engineeringPlate`, `highTempPlate`, `texturedPeiPlate`, `texturedCoolPlate`, `supertackPlate`; 1002 with `data.offered` when the printer does not offer it. Per-plate types are `plate.setSettings` (S5). |

`filament.setColour`, `editMixed`, `setFlushVolumes`, `setPurgeMode` and `bed.setType` change the project
configuration, which is not on the undo stack. They record the previous value in the turn log as a
`{type:"project"}` setting write, so `slicer.undoTurn` can put it back.

Adding a slot is `slicer.addFilamentSlot`; the filament list's "Set filaments to use" is
`slicer.listPresets {includeHidden}` + `slicer.selectPreset {allowHidden}`; the printer wizard is
`slicer.listPrinterModels` + `slicer.addPrinter`.

## Calibration: `calib.run`

The Calibration menu: each item's dialog fields become params, with the dialog's defaults and checks
(a refused value is 1002 with `data.path`). Then the same `Plater::calib_*` call builds the test as a
new project.

| kind | params (defaults) |
|---|---|
| `pressureAdvance` | `method` tower\|line\|pattern (tower), `extruderType` directDrive\|bowden, `start` 0, `end` 0.1 (pattern 0.08, bowden 1.0), `step` 0.002 (pattern 0.005, bowden 0.02/0.05), `printNumbers` (line), `accelerations`/`speeds` (pattern) |
| `temperature` | `start` 230 (the hot bottom), `end` 190, both snapped down to a multiple of 5 °C as the dialog's fields do; start >= end + 5; `nozzleBasedResize` |
| `maxFlowrate` | `start` 5, `end` 20, `step` 0.5 mm³/s |
| `flowRatio` | `flowMethod` yolo\|classic, `pass` 1\|2, `flowPattern` archimedeanChords\|monotonic |
| `retraction` | `start` 0, `end` 2, `step` 0.1 mm |
| `cornering` | `start`/`end` 1..15 (junction deviation printers: 0..0.25, max 0.3), `testModel` |
| `inputShapingFrequency` | `freqStartX/EndX/StartY/EndY` 15..110 Hz (RepRapFirmware: Y = X), `damping` 0.15, `shaperType`, `testModel` |
| `inputShapingDamping` | `freqX`/`freqY` 30 Hz, `start`/`end` 0..0.4, `shaperType`, `testModel` |
| `vfa` | `start` 40, `end` 200, `step` 10 mm/s, `nozzleBasedResize`, `autoAdjust` (true): the dialog's *Auto-adjust to max volumetric speed*. When the end speed exceeds what the filament's max volumetric speed allows the outer wall, it lowers the layer height to the largest standard one (from the printer's compatible process presets) that reaches it; if none does, it uses the smallest and lowers `end` to a step multiple. Where the dialog would ask "continue?", it goes ahead; `used.vfaAdjust {outcome: fits\|layerHeightLowered\|endLowered\|clamped, speedLimitMmS, layerHeight}` says what happened. |

The project is replaced (decision D13):
1. `ifProjectDirty` (`ask` -> 1013 `{projectDirty:true}`; `save`, `discard`, `prompt`) and, for each
   preset type with unsaved edits, `ifDirty` (`ask` -> 1013 with `dirtyKeys`).
   Each contract answers only its own question: `ifDirty: "prompt"` does not reopen the project
   question `ifProjectDirty` settled, nor the other way round. The printer is read again after
   these, so a discarded printer edit is not used.
2. A native confirmation names what is replaced, unless the plate is empty, or `ifProjectDirty:
   "prompt"` lets the slicer ask its own save question about a dirty project.
3. The new project is started before the test is built, so a cancelled prompt changes nothing (1006).
   The test's own `Plater::calib_*` then asks nothing again. If no test model was built, the
   reply is `1010` (the project is left empty), never a success with `objects: 0`.

The reply is `{project, objects, used:{...the values used}, note}`. Slice and print the result
with the usual methods. "Calibration Guide" (a web link) is not an action.

## View and UI

| id | risk | |
|---|---|---|
| `view.camera {direction}` | ui | View > Default/Top/Bottom/Front/Rear/Left/Right, plus `iso` and `topFront` (Ctrl+7: the current plate from the top front, centred on it). |
| `view.projection {mode?, auto?}` | ui | Perspective / Orthogonal / Auto Perspective. |
| `view.toggle {what, on?}` | ui | `gcodeWindow`, `navigator`, `gridlines`, `bedMesh`, `cutChunkColors`, `labels`, `overhang`, `outline`. Without `on` it flips. `bedMesh` only switches the overlay (`meshLoaded` says whether a mesh is there); reading one is `printer.fetchBedMesh`. |
| `view.zoomTo {what, plate?}` | ui | `bed`, `plate`, `objects`, `selection`, `gcode`. |
| `view.assemblyView {on}` | ui | The assembly view toolbar button. |
| `view.previewMeshInfill` (target object) | mutate | View > Preview Mesh Infill without its message boxes: `{object, rebuilt}` (plus `warning` when the tiling was truncated); an up-to-date preview is left alone (`rebuilt:false, upToDate:true`, no undo step). What the boxes would say is 1002 with `reason: "infill-preview-unavailable"`: an unsupported sparse infill pattern, no infill mesh, zero density, or no geometry. |
| `ui.showTab {tab, slot?, url?}` | ui | Main tabs (`home`, `prepare`, `preview`, `device`, `project`, `calibration`, `universe`) or the `print`/`filament` (of `slot`)/`printer` settings, as the sidebar's edit buttons do. A tab this slicer does not show answers 1008 `tab_unavailable` (Project and Calibration are removed for every printer; use `project.info`/`project.setInfo` and `calib.run`), one that would not come to the front 1016 `tab_not_selected`. `prepare` during a G-code preview brings back a set-aside project as `project.restoreAside` does, else answers 1016 `gcode_preview` (leave it with `project.new`). `universe` is 1016 (`universe-off`) when that tab is turned off; with `url` (a page on the Universe tab's own origin only) the tab opens it. |
| `ui.revealSetting {type, key, slot?}` | ui | Opens the settings page holding `key` and highlights it (the sidebar search's jump). For keys the caller may not write (print host, API keys), so the person can type them. |
| `ui.closeGizmo {}` | destructive, native confirm | Closes the open 3D tool; disabled (1016) when none is open. |
| `ui.collapseSidebar {collapsed}` / `ui.resetWindowLayout {}` | ui | |
| `printer.fetchBedMesh {show?}` | outward-printer, printer permission, long-running | View > Reload Bed Mesh from Printer: reads the mesh the printer host reports; nothing is sent to the printer. Finishes when the read does, with the mesh summary of `printer.bedMesh` (no grid; `loaded:false` when the printer has no mesh), or 1010 (`bed-mesh-unavailable`) with the host's error. |
| `printer.bedMesh {grid?}` | read | The mesh the overlay holds (from the last read): `{loaded, host, profile, source, compensating, min:[x,y], max:[x,y], rows, cols, probedPoints, zMin, zMax, zRange, outOfFlat, note?, probed?, showing}` in mm; `probed` rows run along Y from `min`, unprobed nodes are null. Talks to nothing. |

The assistant's own sidebar toggle ("Show ... Sidebar") is not an action: automation never hides itself.

## Preferences: `prefs.get` / `prefs.set` (decision D4)

`prefs.get {names?}` returns `{prefs: {name: {value, label, type, values|minimum/maximum, description}}}`.
`prefs.set {values: {name: value}}` writes and saves them at once and applies what the Preferences
dialog applies (colour scheme, overlays, preview palette, units/camera refresh). It returns
`{changed: {name: {previous, value}}}`. Every value is checked before any is written.

The allowlist is `units`, `defaultPage`, `loadBehaviour`, `colorScheme`, `colourVision`,
`opaqueOverlays`, `dimPreviousLayers`, `dimmedLayerBrightness` (0-99 %, as the dialog), `cameraStyle`,
`zoomToMouse`, `useFreeCamera`, `reverseMouseZoom`, `left/middle/rightMouseDrag`,
`autoArrangeAfterClone`, `autoFlush`, `autoSlice`, `autoSliceDelaySeconds`, the STEP import meshing
(`showStepImportOptions`, `stepLinearDeflection` 0.001-0.1 mm, `stepAngleDeflection` 0.01-1,
`stepSplitCompound`), `rememberPrinterConfig`, `autoBackup` and `autoBackupSeconds` (10-3600, applied
at once), `noWarnModifiedGcode`, `filamentGroupingPopup`, and `settingsMode` (`simple`, `advanced`,
`expert`: the settings tabs' mode switch, through `save_mode`; 1016 `developer-mode` while developer
mode is on, where `prefs.get` reports `effective: "develop"`). The params schema has one property per
allowlisted name and `additionalProperties:false`, so any other name is 1002 with `data.path` =
`/params/values/<name>`.

Automated imports (`project.importModel`, `slicer.openProjectFromUrl`) never show the STEP meshing
dialog, whatever `showStepImportOptions` says: they mesh with the three STEP values above.

`removeMixedTemperatureRestriction` is read by `prefs.get` (with `setWith`) but not written by
`prefs.set`. `prefs.allowMixedTemperatures {allow}` (mutate) changes it: allowing lifts a check that
refuses to slice plates mixing filaments with very different temperatures, so the person confirms in
a native dialog first (the Preferences dialog's own warning); forbidding needs no confirmation. The
plate is validated again afterwards.

These stay out of reach: language, single instance, file associations, login region, stealth mode,
diagnostics and the improvement program, the update channel, user/preset/preference sync, token
storage, the network plug-in, developer, debug and log level, local web apps, SSL, hosts, the downloads
folder, "clear my choice", the printer permission, and the theme editor (themes, theme files and
colours; light/dark is `colorScheme`). The remaining cosmetic rows (graphics, fonts, recent files,
Draco quality ...) are left out of the allowlist (`not-allowlisted` in the coverage manifest).

`app.checkUpdates {}` (outward-network, native confirm) runs Help > Check for Updates; the slicer
shows the person the outcome in its own windows.
