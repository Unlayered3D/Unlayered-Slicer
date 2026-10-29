# Presets, project and file actions

The `preset.*`, `project.*` and `file.*` actions of the action catalog (see
`docs/HLSD/bridge-actions.md` for the catalog, `slicer.invokeAction` and the risk rules). Source:
`src/slic3r/GUI/Actions/PresetActions.cpp`, `ProjectActions.cpp`, `FileActions.cpp`; the pure
logic they share (config diff, revert plan, name rules, export kinds) is
`src/slic3r/GUI/Actions/PresetLogic.hpp`, tested in `tests/slic3rutils/test_actions_presets.cpp`.

Every preset write goes through the same `Tab` / `PresetCollection` calls the Settings tab's own
buttons make, so the person's preset folder ends up exactly as the buttons would leave it. No
action opens the tab's dialogs: a name the dialog would ask for is a parameter, and the only window
an action may raise is the native confirmation or the native Save dialog, which the page cannot
press, pre-answer or see into.

## Presets

All take `type`: `printer`, `print` (process) or `filament`. A preset is named by its name or its
display alias.

### Unsaved edits: `ifDirty`

Any action that switches a tab's preset honours `ifDirty`. On `slicer.invokeAction` it goes on the
envelope (`{id, params, ifDirty}`); the typed methods (`slicer.selectPreset`, `slicer.setSettings`,
`slicer.saveFilamentPreset`, `slicer.addPrinter`) take it inside their params. Both use the same
contract (`ActionRuntime.hpp`, `preset_switch_preflight`):

| Value | The unsaved edits |
|---|---|
| `ask` (default) | Refused with 1013 `{dirty, type, dirtyKeys, dirtyTypes}`; nothing changes. |
| `discard` | Dropped. |
| `save` | Saved into the preset being edited when it is the person's own (user or project preset). On a system preset: 1013 `{needsName:true}`; save it under a name with `preset.save` first. |
| `transfer` | Carried over to the preset being switched to (the dialog's Transfer). Not for printer switches (the slicer does not offer it) and not between two filament types: 1002. |
| `prompt` | The slicer's own unsaved-changes dialog asks the person. |

A switch covers the dependent presets too, as `Tab::select_preset` does: a printer switch looks at
the printer, process and filament edits; a process switch at the process and filament edits. With
any value but `prompt`, no unsaved-changes dialog opens. Every dirty preset is checked before any is
touched. Without the settings opt-in a page's printer `selectPreset` keeps `prompt`.

### Actions

| Action | Risk | What it does |
|---|---|---|
| `preset.save {type, name, from?, overwrite?, detach?, toProject?}` | mutate | The tab's Save with the name given. Saves the tab's preset with its unsaved edits (or `from`, after a preset switch under `ifDirty`) and selects the new preset. A system name is refused (1003); an existing user name needs `overwrite:true`, and replacing a different saved preset asks natively. An overwrite stays where the existing preset lives (project or preset folder), as the Save dialog locks its radio; a contradicting `toProject` is refused. A filament saved for a multi-slot printer changes slot 0 only; for another slot's filament use `filament.savePreset` (`slicer.saveFilamentPreset`), which uses the same name rules and the same overwrite question. The filament tab's own temperature-pair check may still ask the person. |
| `preset.rename {type, name, newName}` | mutate | Only the person's own presets with no unsaved edits and no presets based on them. Save As under the new name, then the old file goes; the selection, filament slots and physical printers follow, and a renamed printer stays the printer of the filament and process presets limited to it (`compatibleUpdated`). No cloud copy is touched. |
| `preset.delete {type, name, cascade?}` | destructive | Native confirmation naming the preset, any unsaved edits on it and the physical printers that lose it; then what `Tab::delete_preset` does after its own question. A printer the person created (a custom root) also deletes every filament and process preset of theirs that suits it: that needs `cascade:true` (else 1002 with `data.cascade`), and the confirmation lists each one. System, bundle and default presets, and presets other presets inherit from, are refused. |
| `preset.diff {type, a?, b?, against?, keys?, limit?}` | read | Differing keys with both values, label and category. Default: the tab's preset with its unsaved edits against its saved state (`dirtyKeys` too). `against:"parent"` compares with the preset it is based on; `against:"edited"` with the tab's preset as edited (what a named `a` is compared with by default); `b` with another preset. One type per call. Secret keys never appear. |
| `preset.revertKeys {type, keys, to?}` | mutate | The tab's undo arrows: the listed keys back to the saved value or (`to:"parent"`) the parent's. `"key#i"` reverts one extruder or variant value. Custom G-code and the compatibility lists can be reverted (the value comes from the saved or parent preset, never from the caller); print hosts and credentials never. The field couplings a hand edit runs (`filament_diameter`, `enable_prime_tower`, `gcode_flavor` ...) run too. Reported as `settingsChanged {source:"bridge"}` and recorded for `slicer.undoTurn`. Keys that are unknown, not revertable or already equal are listed, not errors. |
| `preset.discardEdits {type}` | destructive | The tab's undo button (`Tab::on_roll_back_value`), so the printer tab's extruder count and pages follow. Asks natively unless every dirty key was written by the same turn (`turn` on the invoke). |
| `preset.setVisible {type, name, visible}` | mutate | The setup wizard's ticks: a system filament or printer variant is shown in or hidden from the lists (persisted in the app config). A process preset has no tick of its own: its visibility lasts for this session only. The selected preset cannot be hidden. |
| `preset.setCompatibility {type, name?, printers?, prints?}` | mutate | The Profile dependencies lists of a process or filament preset: the printers (and, for a filament, processes) it suits; `[]` means all. An unsaved edit of the tab's preset (`name` switches to it first, under `ifDirty`), recorded for `undoTurn`. |
| `preset.transfer {type, from, to, keys}` | mutate | The compare dialog's Transfer: the tab switches to `to` (under `ifDirty`) and takes `keys` (`key#i` allowed) from `from` as saved, as unsaved edits. |
| `preset.createFromTemplate {type, base, name, values?, slot?}` | mutate | A new user preset from an installed or hidden system template (hidden printer and process templates are shown first) with `values` applied (same conversion, deny lists and range checks as `slicer.setSettings`, and the same field couplings; one bad value refuses the lot), saved and selected. A filament goes into `slot` only. A new printer model or vendor: `preset.createPrinter`. |
| `preset.createFilament {vendor, type, serial, base, printers[], overwrite?}` | mutate | The Create Filament dialog: a user filament `"<vendor> <type> <serial>"` for each printer, each a copy of the base filament's preset for that printer (same filament id or display name, system first; else `base`), limited to that printer, all sharing one new filament id. A custom vendor may not be `Bambu`, `Generic` or a number. Existing copies need `overwrite:true`. Adding a printer to an existing filament is the same call with the new printer. |
| `preset.createPrinter {printer, vendor?, model?, nozzle, bedSizeMm?, bedOriginMm?, maxHeightMm?, prints[], filaments[], scaleToNozzle?, overwrite?}` | mutate | The Create Printer dialog: from the template `printer`, either a new `vendor` + `model` (with its bed) or another nozzle of the template's model, named `"<model> <nozzle> nozzle"`; copies of `prints` and `filaments` limited to it (their `compatible_printers` is set here). The new printer is selected, as the dialog leaves it, so unsaved edits go through `ifDirty` (`prompt` is refused: nothing can ask on the way). A custom bed texture or model: `bed.setShape` afterwards. |
| `preset.exportBundle {presets[], fileName?, format?, metadata?}` | outwardFile, export gate | The person's own (user or project) presets into a file saved where they choose in the native Save dialog. `format:"zip"` (default): `<type>/<file>.json`, read back by Import Configs. `format:"bundle"`: an `.orca_bundle` with `bundle_structure.json` and `metadata {name, author, version, description}`, which imports as one named bundle. Print-host addresses, credentials and local bed-file paths (`WebAppPresetShare::stripped_keys()`) are removed from every preset; `stripped` lists which keys were removed, never their values. |
| `preset.importBundle {url, overwrite?, installVendors?}` / `preset.importConfig {url, overwrite?, installVendors?}` | outwardNetwork, install gate, long-running | Downloads on `slicer.openProjectFromUrl`'s terms (same host or the tab's download hosts). A sanitised copy is what gets imported: every preset loses print hosts, credentials, upload switches, local paths and `post_process` scripts (the rule `slicer.installPrinterPreset` applies). The native confirmation lists each preset, what was removed from it, which custom G-code it carries and the risky G-code lines. Existing user presets are kept unless `overwrite:true`. Presets based on a vendor this build ships but has not installed are reported (`installableVendors`); with `installVendors:true` the confirmation names them and they are installed first (that reloads every preset, so `ifDirty` applies). Reports `imported`, `missingParents` and `review`. Bundle: `.zip .orca_bundle .orca_printer .orca_filament`; config adds `.json`. |
| `preset.listBundles {}` | read | The preset bundles the slicer holds: id, name, type (`local`, `subscribed`, `default`), author, version, description and their presets. No folder paths. |
| `preset.deleteBundle {id}` | destructive | An imported (local) bundle's folder and every preset loaded from it, after a native confirmation listing them. Refused while one of its presets is selected. |
| `printer.listPhysical {}` | read | Physical printers (print hosts): names and the printer presets they use. Never the host, key or certificate. |

### Settings tabs

| Action | Risk | What it does |
|---|---|---|
| `settings.copyExtruder {type, fromExtruder?, toExtruder?, keys?}` | mutate | The tabs' copy-to-other-extruder button: per-extruder values of one extruder written over another's, as unsaved edits; without `keys`, the edited ones. Refused between extruders of different types. |
| `printer.setExtruderCount {count}` | mutate | The printer tab's Extruders field (`TabPrinter::extruders_count_changed`): per-extruder settings and filament slots resize. Refused on machines whose extruders are fixed (BBL) and while single-extruder multi-material is on. |
| `bed.setShape {shape?, sizeMm?, originMm?, diameterMm?, points?, maxHeightMm?, textureUrl?, modelUrl?, clearTexture?, clearModel?}` | mutate, long-running with URLs | The printer tab's bed shape dialog: a rectangle (size, origin), a circle (diameter, 72 edges) or a polygon; the bed height; a texture (PNG/SVG) or model (STL/SVG) downloaded and kept in `<data_dir>/automation/bed`. Unsaved printer-preset edits. |
| `settings.setFileFromUrl {key, url}` | mutate, long-running | The print tab's Browse buttons: `sparse_infill_svg_path` (SVG) or `infill_mesh_path` (STL/OBJ/3MF) pointed at a downloaded file kept in `<data_dir>/automation/print`. |

`slicer.getSettingsSchema` marks per-extruder settings `perVariant` (`valuesPerVariant: 2` for the
machine limits' normal/silent pairs) and lists `variants` (`{extruder, index, variant}`) on
multi-extruder printers; `slicer.getSettings` and `slicer.setSettings` take `extruder` (0-based) to
read or write that extruder's value only, as the tabs' extruder switch picks it. `slicer.listPresets`
gives each preset `filamentId`, `compatiblePrinters`, `isProjectEmbedded` and `isFromBundle`
(`bundleId`).

The vendor-catalog typed methods are described in the catalog with `bridgeMethod` (no runner), once,
in `ExistingMethods.cpp`: `filament.addSlot` → `slicer.addFilamentSlot`, `filament.savePreset` →
`slicer.saveFilamentPreset`, `printer.listModels` → `slicer.listPrinterModels`, `printer.add` →
`slicer.addPrinter`.

## Project

| Action | Risk | What it does |
|---|---|---|
| `project.info {}` | read | Name, file name, saved/dirty state, object and plate counts, G-code preview mode, and the recent projects as `{index, name, exists}` (names only). |
| `project.new {name?}` | destructive | Starts an empty project. Honours `ifProjectDirty` on the invoke: `ask` (default) refuses with 1013 `{projectDirty:true}` when there are unsaved changes; `save` saves first (only when the project has a file); `discard` drops them; `prompt` shows the slicer's own question. |
| `project.save {}` | outwardFile | Saving over the project's own file asks natively every time. A never-saved project goes to the native Save dialog. |
| `project.saveAs {fileName?}` | outwardFile | The native Save dialog; the new file becomes the project's file. |
| `project.openRecent {index}` | destructive | Opens recent project `index` (from `project.info`). `ifProjectDirty` for the project, `ifDirty` for every settings tab's unsaved edits. |
| `project.importModel {url}` | mutate, long-running | Downloads a 3MF/STL/STEP/OBJ/AMF/SVG/DRC or a ZIP of models and adds it to the plate as Import does (geometry only). The load's undo snapshot belongs to the invoke's turn. To open a 3MF as a project, use `slicer.openProjectFromUrl`. |
| `project.restoreAside {}` | mutate, long-running | The set-aside notification's *Back to the project* (and a click on Prepare): closes the G-code preview a web app's hand-back opened and reloads the project it replaced, with its file, name and unsaved state. `ifDirty` for unsaved preset edits first. 1008 `nothing_aside` when no project is set aside; 1006 when the person cancels the slicer's preset question. Answers `project.info` plus `restored: true`. `ui.showTab {tab:"prepare"}` during such a preview does the same. |
| `project.setInfo {author?, modelName?, license?, description?}` | mutate | The Project tab's *Basic Info* (saved in the 3MF). `license` is one of the tab's list (`""`, `CC0`, `BY`, `BY-SA`, `BY-ND`, `BY-NC`, `BY-NC-SA`, `BY-NC-ND`). Marks the project unsaved; answers `project.info`, which now also has `author`, `modelName`, `license`, `description` and `projectAside`. The tab itself is not shown in this slicer; the fields still travel in the 3MF. |
| `project.listFiles {}` | read | The project's auxiliary files: `folders.{pictures, bom, assembly, others}[{name, sizeBytes, cover}]`, `cover`. |
| `project.addFile {url, folder, name?}` | mutate, long-running | The tab's *Add File*, from a URL (download rules as `project.importModel`): pictures png/jpg/jpeg/bmp, bom xls/xlsx/pdf, assembly pdf, others txt. A taken name gets `_2`, `_3` ... Marks the project unsaved. |
| `project.setCover {name}` | mutate | *Set as cover* on a picture: writes the cover thumbnails and the 3MF's cover file. |
| `project.removeFile {folder, name}` | destructive, native confirm | Deletes one auxiliary file (not undoable); a cover stops being the cover. |
| `project.forgetRecent {index?}` | mutate | Home's remove-from-recent for `index` (from `project.info`), or clear the whole list without `index`. No file is deleted. |

## Files

| Action | Risk | What it does |
|---|---|---|
| `file.export {kind, fileName?}` + optional `target.objects` | outwardFile, export gate | Native Save dialog, then the menu's writer. `stl`/`drc`: the objects merged into one mesh (negative parts subtracted, as Export does); `stls`/`drcs`: one file per object into the folder of the picked file, never overwriting; `3mf`: the generic 3MF; `toolpathsObj`: the sliced preview's toolpaths (1008 before a slice); `publish3mf`: the Publish flow with this session's published settings, through its own Save dialog. |
| `file.reloadGcode {}` | ui | Reloads the G-code shown in the G-code preview; disabled outside it. |

## Not automated

The Settings tab's search box is covered by `slicer.getSettingsSchema`; the "Edit preset" entries
by `ui.showTab`. The settings mode switch (simple / advanced / expert) is left to the preferences
actions (todo in `preset.json`). Custom G-code editing, physical printers and print hosts, and the
bundle dialog's cloud and open-folder entries stay excluded; the project attachment list (Auxiliary) is
excluded (OS file dialogs, no effect on the print); the model mall upload/download entries are
cloud-account flows and excluded. See `resources/actions/surface/preset.json` and `project.json`.

## End-to-end cases

`tools/bridge_e2e/cases/presets.yaml`, with fixtures `tools/bridge_e2e/fixtures/s7_presets.zip`
(one process preset based on `0.20mm Standard @BBL X1C`) and `s7_cube.stl`. Native questions are
answered by `UNLAYERED_E2E_AUTOCONFIRM` and Save dialogs by `UNLAYERED_E2E_SAVE_DIR` (dev edition
only).
