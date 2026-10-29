# Slicing actions

Code: `src/slic3r/GUI/Actions/SliceActions.cpp` (catalog actions), the typed methods `slicer.slice`,
`slicer.getSliceResult`, `slicer.getSliceModes` and `slicer.setSliceMode` in `WebAppBridge.cpp`
(described in `ExistingMethods.cpp` and `WEB_APP_BRIDGE.md`), and `slice.all` in `PlateActions.cpp`.
Coverage manifest: `resources/actions/surface/core.json`.

All of these run through `slicer.invokeAction` behind `<slot>_allow_settings`. Decision D7 applies
throughout: automation may preselect a slicing mode, change the slicing service's options and stop
a run, but it never starts a service run. `slice.run` and `slice.all` answer `1014`
(`data.reason: "service_mode_selected"`) while a slicing-service mode is selected; the person presses Slice.

| Action | Risk | What it does (UI equivalent) |
|---|---|---|
| `slice.cancel` | ui | The slicing notification's *Cancel*: stops a local slice and the rest of a *Slice all*, or cancels a service job (the service is told to stop). Answers `{cancelled, local, service}`; `cancelled: false` when nothing was running. Runs while the slicer is busy slicing. |
| `slice.getOptions {}` | read | The Slice dropdown's *Slicing service options...* form for the selected printer, from the cached form only (never a blocking fetch): `{service, printer, axes, build, serviceModeSelected, fields[{dest, type, label, help?, unit?, min?, max?, optional?, choices?[{value, label}], default, value, applies}], stored}`. No entitlement check (whether the service takes a job is its decision); `1008` with `reason` `no_service` or `no_form` (the form is fetched in the background; try again). |
| `slice.setOptions {values?[{dest, choice\|number\|on\|toDefault}], reset?}` | mutate | The dialog's OK (and *Reset*): stores what the dialog would, per printer (`slice_service` in the app config): the applying fields that differ from their defaults, plus `build`. `reset` erases the remembered options first. A field that does not apply under the resulting build, an unknown field or a value outside the field's choices or range is `1002` with `data.path`. Relabels the Slice button for the mode; a mode changed this way defers the background prepare again (D7). Answers as `getOptions` plus `changed`. A service result made with other options is out of date afterwards; only the person's Slice makes a new one. |
