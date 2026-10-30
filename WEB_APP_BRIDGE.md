# Web-app bridge, protocol v1

Unlayered Slicer can host a web application, either as a top-level tab or as a side panel next to
every tab, and let that application use a defined set of slicer actions. This document is the
complete specification of that interface. How it fits with the slicer's other documented
interfaces (the slicing service and the account provider) is summarised in
[BOUNDARY.md](BOUNDARY.md).

Each slot (§7) is a browser control pointed at a URL the user configures in Preferences ▸
Advanced ▸ Web apps & services. It is the same mechanism the
**Device** tab already uses to embed a printer's own web interface — Duet Web Control, Fluidd,
Mainsail — none of which are part of this program.

**Any web application at any address may use this interface.** There is no vendor allow-list, no
registration, no key. If you can serve a page, you can write a panel. A working reference client
ships in [`resources/web/webapp-example/`](resources/web/webapp-example/) under the MIT licence;
start from it.

---

## 1. Relationship between the two programs

This matters enough to state plainly, because it constrains the design of everything below.

Unlayered Slicer is licensed under the **GNU Affero General Public License, version 3**. A web
application that talks to it across this interface is a **separate program**: it runs in its own
process (usually on its own machine), it is written in whatever language its author likes, it is
licensed however its author likes, and it exchanges nothing with the slicer but serialized JSON
over a documented protocol. Loading a page in a browser control has never made that page a
derivative of the browser, and nothing here changes that.

Two consequences follow, and the interface is shaped by them:

- **The bridge exposes capabilities, and — behind an explicit per-tab opt-in — a typed, filtered
  view of print settings.** You will not find a way to dump a `DynamicPrintConfig`, walk layer
  data, or read a preset's print-host credentials or custom G-code. Settings cross as the `.ini`
  serialisation of a *named* key, one you asked for, under three deny lists no configuration
  widens (§4.1). Those are the slicer's internal data structures; an interface that shovelled them
  to a client would be a much weaker claim to being two programs, quite apart from being a worse
  interface.
- **The bridge itself names no particular application.** The URL, the title and every
  capability gate are per-slot configuration that the user can change. Unlayered's builds seed
  default addresses for some slots, but only where no value is stored yet, and a blank address
  turns a slot off (§7).

If you host a web application that runs Unlayered Slicer **on your server** to produce output for
remote users, note that AGPL-3.0 §13 asks you to offer those users the Corresponding Source of the
slicer. `slicer.getInfo` returns `app.sourceUrl` precisely so your page can render that offer
without hard-coding it. That obligation covers the slicer, not your application.

---

## 2. Transport

| Direction | Mechanism |
|---|---|
| page → slicer | `window.wx.postMessage(jsonString)` |
| slicer → page | `window.__slicerBridge._recv(obj)`, falling back to `HandleStudio(obj)`, then `window.postMessage(obj, window.location.origin)` |

`window.wx` is installed by the browser control itself and is shared with other parts of the
slicer. **Always post a string**, never an object: WebView2 accepts both, but WKWebView and
WebKitGTK are string-oriented.

You do not normally touch either of these directly. On a trusted origin the slicer injects a small
shim, `window.__slicerBridge`, after each page load:

```js
window.__slicerBridge = Object.freeze({
  version: 1,
  token:   "…64 hex characters…",
  send:    function (method, params) { /* → Promise */ },
  on:      function (eventName, fn) {},
  off:     function (eventName, fn) {},
  _recv:   function (payload) {}          // internal
});
```

`send()` resolves with the response `data`, or rejects with `{code, message, method, data}` — the
response's `data` rides along on a rejection too, which is how a `1013` hands you the list of dirty
keys (§4.1). It rejects with `code: 'no-bridge'` immediately when not running inside the slicer,
and `code: 'timeout'` after 60 s.

A `slicerbridgeready` event fires on `window` once the shim is installed.

### Detecting the slicer

Feature-detect, in this order:

1. `window.__slicerBridge` exists → you are on a trusted origin inside the slicer.
2. `typeof window.wx?.postMessage === 'function'` → you are inside *a* slicer web view, but not
   necessarily on a trusted origin. A `slicer.getInfo` will tell you.
3. Neither → you are in an ordinary browser. Behave accordingly.

**Do not parse the user agent.** It currently contains a `BBL-Slicer/` token inherited from
upstream, it does not identify this fork, and it is rewritten with a macOS platform string on every
platform when the theme changes. It is not a reliable signal for anything.

---

## 3. Envelope

### Request (page → slicer)

```json
{
  "v": 1,
  "id": "wa-7",
  "method": "slicer.getInfo",
  "token": "…64 hex characters…",
  "params": {}
}
```

| Field | Required | Notes |
|---|---|---|
| `v` | no | Protocol version. Reserved; only `1` exists. |
| `id` | when you want a reply | Opaque; echoed back verbatim. Omit for fire-and-forget. |
| `method` | **yes** | Must begin with `slicer.`. Anything else is ignored, not rejected — the channel is shared. |
| `token` | **yes** | `window.__slicerBridge.token`. See §6. |
| `params` | no | Object. Defaults to `{}`. |

For compatibility with the older envelope used elsewhere in this codebase, `command` is accepted as
an alias for `method` and `data` for `params`. New code should not use them.

### Response (slicer → page)

```json
{
  "type": "response",
  "v": 1,
  "id": "wa-7",
  "method": "slicer.getInfo",
  "code": 0,
  "message": "success",
  "data": { }
}
```

A response is sent only when the request carried an `id`. `code` is `0` on success.

### Event (slicer → page)

```json
{ "type": "event", "v": 1, "id": "wa-7", "method": "slicer.projectOpened", "data": { "ok": true } }
```

Events carry the originating `id` when they report on a specific request. Subscribe with
`window.__slicerBridge.on("slicer.projectOpened", fn)`.

---

## 4. Methods

### `slicer.getInfo`

No parameters. Never fails on a trusted origin.

```json
{
  "api":      { "version": 1, "envelope": "slicer-rpc/1" },
  "app":      { "name": "Unlayered Slicer", "version": "0.0.3-dev", "coreVersion": "02.06.00.51",
                "sourceUrl": "https://github.com/Unlayered3D/Unlayered-Slicer", "license": "AGPL-3.0" },
  "platform": { "os": "windows", "locale": "en", "darkMode": true },
  "capabilities": ["slicer.getInfo", "slicer.openProjectFromUrl", "slicer.selectPreset", "slicer.openProject"],
  "limits":   { "maxDownloadBytes": 268435456,
                "fileTypes": ["3mf","stl","step","stp","obj","amf","svg","drc","gcode"] },
  "origin":   { "configured": "http://127.0.0.1:8765", "trusted": true, "loopback": true },
  "tab":      { "slot": "web_app", "title": "My Tools", "layout": "tab" },
  "settingsAccess": false,
  "printerAccess": false,
  "account":  { "signedIn": true, "provider": "https://accounts.example.com" },
  "printerAccessSetting": { "label": "Let the side panel use printers",
                            "path": ["Menu", "Preferences", "Developer", "Local web apps"] },
  "privacy":  { "status": "granted", "diagnosticsPermitted": true, "aiTraining": true, "program": true },
  "instanceId": "3f9c0a7d1b2e4c5f8a6d9e0b1c2d3e4f",
  "projectGeneration": 0,
  "features": ["openProjectFromUrl.printer", "openGcode.keepsProject", "getInfo.privacy",
               "settingsChanged.bridgeChanges", "getInfo.instanceId", "getInfo.projectGeneration",
               "projectChanged", "event.projectChanged", "getInfo.printerAccessSetting"]
}
```

`features` names behaviours of existing methods that `capabilities` (method names only) cannot
express; absent on older builds. `openProjectFromUrl.printer`: the `printer` parameter below is
honoured. `openGcode.keepsProject`: opening G-code sets the user's project aside instead of closing
it (see `slicer.openProjectFromUrl`).

**`capabilities` is computed at run time and is the only correct way to decide what you may call.**
It varies with the configured origin and the user's settings. Do not hard-code a method list.
With the settings opt-in on (§4.1) it grows by the settings-family methods; `slicer.capabilitiesChanged`
(§4.5) tells you when to re-read it.

`platform.os` is one of `windows`, `macos`, `linux`. `platform.locale` is the slicer's active
language code. `app.sourceUrl` is described in §1. `tab.slot` is `web_app` (the web-app tab),
`side_panel` (the side panel) or `universe_app` (the model-library tab), so one page can be served
in any of them; `tab.title` is what the slot is labelled. `tab.layout` is `"tab"` for a page
hosted as a notebook page and `"sidebar"` for the side panel, which the slicer shows as a narrow,
persistent panel beside every tab (a page that serves both arrangements lays itself out
accordingly). `settingsAccess` and `printerAccess` mirror whether the settings (§4.1) and printer
(§4.6) permissions are on for this slot.

`account` says whether the slicer is signed in to an account provider and which one:
`{ "signedIn": bool, "provider": "<provider origin, or empty when none is configured>" }`. It
carries no name, email or token, and nothing about what the account may use: a page that offers
paid features checks that on its own server ([ACCOUNT_PROVIDER_PROTOCOL.md](ACCOUNT_PROVIDER_PROTOCOL.md)).
A page on the provider's own origin can be signed in by the slicer (ACCOUNT_PROVIDER_PROTOCOL.md
§6); a page on any other origin never is.

`privacy` (feature `getInfo.privacy`, 0.2.0) is the person's usage-diagnostics choice, read live
on every call: `status` is `notAsked`, `declined`, `granted` or `needsNewChoice`; `diagnosticsPermitted`
is true only when the choice counts as consent where they live and their organisation has not
switched diagnostics off. `program` is true only when, on top of that, the person ticked the
"Slicing improvement program" box -- under notice 2026-10-v3 the only consent for AI use -- and
`aiTraining` equals `program` (builds before this reported the diagnostics choice there). Store a
person's training data only when it is true, and re-read it rather than caching it: Preferences
can change it at any time.

`instanceId` and `projectGeneration` (features `getInfo.instanceId` and `getInfo.projectGeneration`, 0.2.0) say which slicer window
the page is in and which project that window holds. Every window (File > New Window, or launching
the slicer again) is its own process, and `instanceId` is 32 random hex characters minted once per
process: a reload of the page, or the tab being shown again, reads the **same** id; a page in
another window, or after a restart, reads a different one. It is derived from nothing on the
machine and means nothing outside this window. `projectGeneration` starts at 0 and goes up by one
with every `slicer.projectChanged` (§4.5) this window has announced, so a page that was not loaded
when the project changed can still tell, by comparing it with the value it last saw. A page that
keeps per-window state (a conversation, say) keys it on `instanceId` and starts afresh when the id
differs from the one it stored; older builds have neither field.

### `slicer.openProjectFromUrl`

**The recommended way to hand a file to the slicer**, including when your server runs on the same
machine. It needs no filesystem configuration, and the same code path works once your application
moves to a server.

```json
{ "url": "http://127.0.0.1:8765/download/9f2c…", "filename": "bracket.3mf",
  "sha256": "e3b0c44298fc1c149afbf4c8996fb924…" }
```

| Parameter | Required | Notes |
|---|---|---|
| `url` | **yes** | `http` or `https`. The host must match the page's own host, unless it is listed in `web_app_download_hosts`. |
| `filename` | recommended | Reduced to a bare filename; path separators are stripped. Falls back to a name derived from the URL. The extension must be in `limits.fileTypes`. |
| `sha256` | optional but advised | Lowercase hex. On mismatch the file is deleted and never opened. |
| `printer` | optional, `.gcode` only | Installed printer preset (name or alias) the G-code was sliced for; unknown → `1008` before anything downloads. Selected **after** the user's project is set aside and **before** the file opens, so the preview gets the right bed without touching the user's project. Needs the `openProjectFromUrl.printer` feature; ignored for other file types. |

The file is downloaded to `<data dir>/cache/webapp/`, then opened: `.3mf` as a project, `.gcode` in
the **Preview tab in G-code-preview mode** (the sidebar is greyed and Slice/Print disabled until
File ▸ New Project — the same state as opening a G-code file from disk), everything else onto the
current plate. A STEP file is meshed with the Preferences' STEP import values (linear and angle
deflection, split compounds) without the meshing dialog, which a page could not answer
(`prefs.set` changes those values; `project.importModel` behaves the same).

Two things to know about the G-code case. Opening G-code **does not cost the user their project**
(feature `openGcode.keepsProject`): the open project is written to a stash 3MF and closed without a
prompt, and the preview shows a "set aside" notification. Clicking the **Prepare** tab or the
notification's link brings it back exactly as it was — same file, same title, unsaved changes still
unsaved; anything that would close the preview (new/open project, quitting) restores it first so the
ordinary save prompt covers it, and further G-code opened while previewing keeps it aside. Only if
the stash cannot be written does the old behaviour return — the "save changes?" dialog, where a user
who cancels still gets `ok: true` (see below). And the
slicer **ignores a G-code file whose name matches the one already shown**, so send a **unique
`filename` per result** (for example `part-3f6a2c1d.gcode`), including when the same model is sliced
again — otherwise the second result is silently dropped.

A handed-over G-code is not a dead end. In the preview the **Print** button uploads it to the
printer configured in Printer Settings, and **Export G-code file** saves it wherever the user likes
— the same two actions a locally sliced plate offers, operating on the file you sent. Slicing stays
disabled, since there is no model to slice.

Emits `slicer.openProjectFromUrl.progress` roughly once per received mebibyte:

```json
{ "received": 3145728, "total": 8388608 }
```

`total` is `0` when the server sends no `Content-Length`.

### `slicer.openProject`

Opens a file already on the user's disk. Present in `capabilities` **only** when the page origin is
loopback, or when the user has enabled *Allow the web application to open local files*.

```json
{ "path": "C:\\Users\\me\\AppData\\Roaming\\Unlayered Slicer\\cache\\webapp\\bracket.3mf" }
```

The path is canonicalised and then checked: it must be a regular file, of an accepted type, within
`limits.maxDownloadBytes`, not a UNC path, and underneath one of the roots in `web_app_path_roots`.
When that setting is empty the roots default to the slicer's `cache/webapp` folder, the system
temporary directory, and the user's configured download folder.

**Files elsewhere are rejected with `1003`, by design.** If your application writes files to its own
working directory, either serve them over HTTP and use `slicer.openProjectFromUrl`, or ask the user
to add that directory to `web_app_path_roots`. The first is almost always the better answer.

### `slicer.selectPreset`

Switches the application to an **already-installed** preset, by name or by the alias the dropdown
shows. It cannot create, modify or delete one. The only way a page can add a preset is
`slicer.installPrinterPreset` (§4.4): printer profiles only, only behind `<slot>_allow_install`, and
always behind a native confirmation the page cannot skip.

```json
{ "type": "printer",  "name": "Unlayered 5 Axis Printer 0.4 nozzle" }
{ "type": "print",    "name": "0.20mm Standard @Unlayered", "ifDirty": "ask" }
{ "type": "filament", "name": "Generic PETG", "slot": 1, "ifDirty": "discard" }
```

| Parameter | Required | Notes |
|---|---|---|
| `type` | no | `"printer"` (the default), `"print"` or `"filament"`. The printer form is protocol v1 and always available; the print and filament forms belong to the settings family (§4.1) and need its opt-in, else `1005`. |
| `name` | **yes** | An installed preset's name or alias; a `(modified)` suffix is ignored. Else `1008`. |
| `slot` | filament only | Sidebar slot index, default `0`. Out of range → `1002`. |
| `ifDirty` | no | What to do when a preset being replaced has unsaved edits: `"ask"` (default) answers `1013` and changes nothing; `"discard"` drops the edits and proceeds; `"save"` saves them into the person's own preset first (a system preset: `1013` with `needsName: true`); `"transfer"` carries them over to the new preset (print and filament switches only, not between filament types; else `1002`); `"prompt"` shows the slicer's own "transfer or discard?" dialog. A printer switch covers the process and filament edits too, a process switch the filament edits (the slicer asks about those dependents), so no dialog opens unless `"prompt"`. `1013` data: `{dirty, type, dirtyKeys, dirtyTypes: {type: keys}}`. The same contract as `ifDirty` on `slicer.invokeAction`'s envelope. While the tab has **no** settings opt-in this is always `"prompt"`, so a v1 client sees v1 behaviour. |
| `allowHidden` | no | Settings opt-in only (feature `selectPreset.allowHidden`). `true`: a **hidden** system preset (installed, but not ticked in the filament selection / the setup wizard, so absent from the dropdowns; see `listPresets` `includeHidden`) is enabled the way that tick enables it, then selected; its alias resolves too. Default `false`: a hidden filament or print preset is refused with `1008` and `data: {hidden: true, name}`, so the page can ask again. The reply carries `enabled: true` when it enabled one. |

```json
{ "type": "printer", "selected": "Unlayered 5 Axis Printer 0.4 nozzle", "changed": true }
{ "type": "filament", "slot": 1, "selected": "Generic PETG", "changed": true }
```

`selected` is the preset actually in force **after** the call, which is not always the one you
asked for: under `"prompt"` the user may decline the dialog, and a filament switch on a
single-filament printer goes through the filament tab. Read `selected` rather than assuming.
Selecting the preset that is already selected changes nothing and reports `changed: false`. On a
multi-filament printer a filament switch changes exactly the slot you named.

Why you would want the printer form: the bed drawn in the preview, and the coordinates the
toolpaths are judged against, come from the selected printer preset. Opening G-code does not change
that selection, so a page that slices for one printer while the application sits on another
produces a preview on the wrong bed. With the `openProjectFromUrl.printer` feature, pass the printer
to `slicer.openProjectFromUrl` instead: a `selectPreset` sent before the open switches the printer
of the user's project, which is then set aside already switched. Older builds: call this first.

### 4.1 Preset settings — behind a per-tab permission

The methods below -- the preset settings, the plate summary, and the plate edits
(`setCutPlanes`, `setOrientation`, `setObjectFilament`, `setScale`, `moveObject`, `arrange`,
`setCopies`, `fillBed`) -- and the print/filament forms of `selectPreset` appear in `capabilities`
only for a slot whose `<slot>_allow_settings` key is on (§7). It is off by default for every slot
in a customer build.
Without it every one of them answers `1005`, and `slicer.getInfo` reports `settingsAccess: false`.

Rules shared by all of them:

- `type` is `"printer"`, `"print"` or `"filament"` — the three sidebar presets.
- Values cross the bridge as **strings in the preset's own `.ini` form**: `"0.2"`, `"35%"`, `"1"`
  for a bool, `"1,0"` for a per-extruder bool vector, `"0x0,220x0,220x220,0x220"` for points.
  `setSettings` also accepts numbers, booleans and arrays and serialises them for you.
- Three deny lists apply everywhere and no setting widens them
  ([`src/slic3r/GUI/WebAppSettings.cpp`](src/slic3r/GUI/WebAppSettings.cpp)):
  - **secret** — `print_host`, `print_host_webui`, `printhost_apikey`, `printhost_cafile`,
    `printhost_user`, `printhost_password`, `printhost_port`, `printhost_ssl_ignore_revoke`,
    `printhost_authorization_type`, `bbl_use_printhost`, `host_type`: never read, never written;
  - **code** — the sixteen custom-G-code options (`machine_start_gcode`, `machine_end_gcode`,
    `layer_change_gcode`, `filament_start_gcode`, … `filament_change_extrusion_role_gcode`) and
    `post_process`: never written, read only when the call says `includeCode: true`;
  - **identity** — `inherits`, `printer_model`, `printer_variant`, `printer_technology`,
    `compatible_prints`, `compatible_printers`, `compatible_prints_condition`,
    `compatible_printers_condition`, `filament_vendor`, `filament_id` and every `*_settings_id`:
    readable, never written. Read-only and plugin-backed options are never written either.
- A key the slicer does not know is `1008`; a denied one is `1003`.
- Every call answers `1007` while a modal dialog is open (`"a dialog is open"`); the writers
  (`setSettings`, `selectPreset`, `slice`) also while the slicer is slicing.
- Writes are **unsaved edits**, exactly what typing the value into the sidebar would produce: the
  tab shows "(modified)", the user can undo or save, and the bridge writes nothing to disk.
- Labels, tooltips and categories in the schema are the slicer's untranslated source strings.

#### `slicer.listPresets`

```json
{ "type": "filament", "query": "PETG", "compatibleOnly": true, "limit": 50 }
```

`query` is a case-insensitive substring of the name, alias, vendor or (filament) material type;
`compatibleOnly` (default `true`) drops presets incompatible with the selected printer; `limit`
defaults to 500. Only visible presets are listed unless `includeHidden: true` (feature
`listPresets.includeHidden`): then the installed but hidden system presets are listed too, with
`isVisible: false`, and `hiddenTotal` counts them. That is every instantiable preset of every vendor
bundle the slicer has loaded -- OrcaFilamentLibrary and each installed vendor -- which is where a
vendor's own profile for a product ("Bambu TPU for AMS", "COEX TPE 60D") is found. A vendor bundle
that is not installed is not loaded; its filament presets would be incompatible with the selected
printer anyway, and `slicer.addPrinter` installs it.

```json
{ "type": "filament", "selected": "Generic PETG", "edited": { "name": "Generic PETG", "dirty": false },
  "total": 3,
  "presets": [ { "name": "Generic PETG", "alias": "Generic PETG", "isSystem": true, "isUser": false,
                 "isCompatible": true, "isVisible": true, "vendor": "Generic",
                 "inherits": "", "filamentType": "PETG" } ],
  "slots": ["Generic PLA", "Generic PETG"], "slotColours": ["#FFFFFF", "#00AE42"] }
```

`slots` and `slotColours` are present for `filament` only: the preset in each sidebar slot and
its project colour. `total` counts matches before `limit`. `vendor` is the filament's
`filament_vendor` (the brand) for filaments, else the vendor bundle's name; `filamentType` and
`filamentId` (shared by one filament's presets for each printer) are present for filaments.
Process and filament presets carry `compatiblePrinters` (empty: every printer, subject to the
condition). Every preset carries `isProjectEmbedded` (kept in the project, not the preset folder)
and `isFromBundle` (with `bundleId`).

#### Vendor catalog: `addFilamentSlot`, `saveFilamentPreset`, `listPrinterModels`, `addPrinter`

Settings family (opt-in, `1007` under a dialog, the writers also while slicing). What a person
does to set up a material or a printer they just bought: find the vendor's own preset (`listPresets`
with `includeHidden`, or `listPrinterModels`), add the filament slot or the printer, select, adjust
with `setSettings`, and keep the result as a named preset. Mutating calls may carry a `turn` like
the others (§4.7).

##### `slicer.addFilamentSlot`

```json
{ "preset": "Bambu TPU for AMS @BBL H2S", "allowHidden": true }
```

The sidebar's "+" beside the filament list: a new physical slot with the next colour, inserted
after the last physical one (mixed-colour slots stay at the tail), then `preset` (optional; a name
or alias, hidden ones only with `allowHidden`) selected into it. A bad `preset` is refused before
anything is added. Refused with `1002` and `data.reason`: `"fixedExtruders"` on a printer with
one filament per extruder (the "+" does not exist there: the count is the extruder count, a printer
setting) and `"maximum"` at 64 filaments; `1007` while a sliced file is open for preview. Not
undoable (the slicer's own "+" is not); the sidebar's "-" removes it.

```json
{ "slot": 3, "selected": "Bambu TPU for AMS @BBL H2S", "colour": "#26A69A", "enabled": true,
  "slots": ["Bambu PLA Basic @BBL H2S", "Generic PETG", "Generic PLA", "Bambu TPU for AMS @BBL H2S"] }
```

`slot` is 0-based, like `selectPreset`'s.

##### `slicer.saveFilamentPreset`

```json
{ "slot": 3, "name": "Bambu TPU 72D (custom)", "overwrite": false, "ifDirty": "ask" }
```

The Filament tab's Save with the name typed in: the slot's preset, with its unsaved edits, is
written as a **user preset** (`Tab::save_preset` -> `PresetCollection::save_current_preset` ->
`update_compatible`) and takes that slot's place. This is the one settings call that writes a file:
the user preset JSON in the person's own preset folder, exactly what Save writes; nothing else. A
new preset made from a system one is limited to the selected printer (`compatible_printers`), as
Save does. `slot` (optional, 0-based) puts the Filament tab on that slot's preset first, a preset
switch under `ifDirty`; without it the tab's edited preset is saved. Save renames every slot that
held the old preset; this call puts the others back, so only `slot` changes.

`name` follows the Save dialog's rules (the same check as `preset.save`): not empty, at most 200
characters, none of `<>[]:/\|?*"`, no leading or trailing space, no `(modified)`, not a reserved
name, not another preset's alias. An existing **system** preset of that name is `1003`; an existing
user preset is `1002` with `data.exists: true` unless `overwrite: true`, and replacing a *different*
saved preset asks the person natively ("Overwrite preset"; declined: `1006`), as `preset.save` does. The tab's first-layer temperature check
may still ask the person; declined, the call answers `1006`. Before saving, every per-variant list
of the edited preset is made one value per extruder variant (see `setSettings`); keys that had to
be resized are listed in `data.variantResized` (absent when none).

```json
{ "name": "Bambu TPU 72D (custom)", "from": "Bambu TPU for AMS @BBL H2S", "overwritten": false,
  "savedEdits": ["nozzle_temperature", "filament_max_volumetric_speed"], "slot": 3,
  "slots": ["...", "Bambu TPU 72D (custom)"], "compatiblePrinters": ["Bambu Lab H2S 0.4 nozzle"] }
```

##### `slicer.listPrinterModels`

```json
{ "query": "bambu h2s", "includeNotInstalled": true, "limit": 50 }
```

Every FFF printer model the slicer ships in `resources/profiles/<vendor>.json`, installed or not
(plus any model an installed, online-updated bundle knows): what the setup wizard's printer page
offers. `query` words must all occur in the vendor id, vendor name, model name or family (letters
and digits compared, so "Bambu Lab" matches "Bambulab"); exact model names sort first.
`includeNotInstalled: false` keeps to vendors already installed. `limit` defaults to 50 (max 500).
Read-only.

```json
{ "total": 1,
  "models": [ { "vendor": "BBL", "vendorName": "Bambulab", "model": "Bambu Lab H2S", "family": "BBL-3DP",
                "nozzles": ["0.2", "0.4", "0.6", "0.8"], "defaultNozzle": "0.4", "installed": true,
                "enabled": false, "enabledNozzles": [], "presets": { "0.4": "Bambu Lab H2S 0.4 nozzle" },
                "selected": false, "shipped": true } ],
  "current": { "printer": "Unlayered 5 Axis Printer 0.4 nozzle", "model": "Unlayered 5 Axis Printer", "nozzle": "0.4" } }
```

`presets` names the system printer preset per nozzle when the vendor is installed.

##### `slicer.addPrinter`

```json
{ "vendor": "BBL", "model": "Bambu Lab H2S", "nozzle": "0.4", "select": true, "ifDirty": "ask" }
```

The setup wizard's printer pick for one model and nozzle. `model` (required) is matched exactly
(letters and digits) within `vendor` (optional; id or name), else as words; two or more matches
are `1002` with `data.candidates`, none `1008`. `nozzle` (number or string) defaults to the
standard 0.4 when the model has it, else its first; an unknown one is `1002` with `data.nozzles`.

- Vendor already installed: the nozzle is ticked in the app config's vendor section, the printer
  and its default filaments become visible (the same passes `load_selections` runs), and with
  `select` (default `true`) it is selected like a pick from the printer dropdown, under
  `ifDirty` (printer, process and filament edits, as any printer switch).
- Vendor not installed: `PresetBundle::apply_vendor_config`, which the wizard calls, installs the
  vendor's profiles from the resources, merges the model into what is enabled and reloads every
  preset; then `load_current_presets` as after the wizard. Because every preset reloads, unsaved
  edits in ANY tab are under `ifDirty` first (`1013` lists them per type; `"discard"` and
  `"save"` act on every tab; `"transfer"` is refused; `"prompt"` shows the wizard's keep/save
  dialog).

```json
{ "type": "printer", "vendor": "BBL", "model": "Bambu Lab H2S", "nozzle": "0.4",
  "preset": "Bambu Lab H2S 0.4 nozzle", "installedVendor": false, "enabled": true, "wasEnabled": false,
  "selected": "Bambu Lab H2S 0.4 nozzle", "changed": true }
```

A printer the slicer does not ship is not this call: `slicer.installPrinterPreset` (4.4) installs a
shared profile from a URL, behind its own opt-in and a native confirmation.

These calls write two things besides the usual unsaved edits, both through the paths the person's
own clicks take: the preset-visibility sections of the app config (`filaments`, and the vendor
model/nozzle ticks) and, for `saveFilamentPreset`, one user preset file. No other application
setting is read or written.

#### `slicer.getSettingsSchema`

```json
{ "type": "print", "query": "infill", "includeCode": false, "limit": 200 }
{ "type": "print", "keys": ["layer_height", "sparse_infill_density"] }
{ "type": "printer", "category": "Extruder" }
```

Give `keys`, or filter the tab's own keys by `category` (exact, case-insensitive) and/or `query`
(words separated by spaces or commas; an option matches when ANY word is a substring of its key,
label or tooltip, so `"support brim skirt"` returns all three families). `limit` defaults to 200
(`truncated: true` when it bit).
Each option comes from the slicer's option definitions:

```json
{ "type": "print", "total": 14, "truncated": false,
  "options": [ { "key": "sparse_infill_density", "type": "percent", "vector": false, "nullable": false,
                 "label": "Sparse infill density", "fullLabel": "", "category": "Strength",
                 "tooltip": "Density of internal sparse infill, 100% turns all sparse infill into solid infill and internal solid infill are 100% density",
                 "sidetext": "%", "min": 0, "max": 100, "mode": "simple",
                 "enumValues": [], "enumLabels": [], "readonly": false, "writable": true, "value": "15%" } ] }
```

On a multi-extruder printer, options that hold one value per extruder variant carry
`perVariant: true` (`valuesPerVariant: 2` for the machine limits' normal/silent pairs), and the
reply lists `variants: [{extruder, index, variant}]` -- which value index each extruder (0-based, as
the tab's extruder switch counts) edits. `getSettings` / `setSettings` with `extruder` use it.

`type` is one of `float`, `floats`, `int`, `ints`, `string`, `strings`, `percent`, `percents`,
`float_or_percent`, `floats_or_percents`, `point`, `points`, `point3`, `bool`, `bools`, `enum`,
`enums`, `points_groups`, `ints_groups`. `min`/`max` are `null` when unbounded. `writable` is the
deny-list verdict; `value` is the current serialised value, `null` when this tab's preset does not
hold the key.

#### `slicer.getSettings`

```json
{ "type": "print", "keys": ["layer_height", "wall_loops"] }
{ "type": "filament", "slot": 1 }
```

Without `keys`, every readable key of the preset. `slot` (filament) reads the preset in that
sidebar slot; when it is not the one the Filament tab is editing you get its *saved* values, since
only the edited preset carries unsaved edits (`preset.source` names which preset you got).
`extruder` (0-based) reads per-variant options for that extruder's variant only (one value, or
two comma-separated for the machine limits) and adds `variant: {extruder, index}`; `1002` when the
preset has no variant for it.

```json
{ "type": "print",
  "preset": { "selected": "0.20mm Standard @Unlayered", "edited": "0.20mm Standard @Unlayered",
              "dirty": true, "dirtyKeys": ["wall_loops"], "source": "0.20mm Standard @Unlayered" },
  "values": { "layer_height": "0.2", "wall_loops": "4" } }
```

#### `slicer.setSettings`

```json
{ "type": "print", "values": { "sparse_infill_density": "35%", "wall_loops": 4, "enable_support": true } }
{ "type": "filament", "slot": 1, "values": { "nozzle_temperature": [235] }, "ifDirty": "ask" }
```

`values` holds 1 to 64 entries. Each is parsed by the slicer's own deserialiser (so it is exactly
as strict as a `.ini` file) and range-checked against the option's `min`/`max`; a value that fails
is reported in `rejected` and **nothing** of that key is applied. Keys are independent: one bad
key does not stop the others. `extruder` (0-based) writes per-variant options at that extruder's
variant only (one value, or one or two for the machine limits), leaving the other variants; other
keys are written whole. The field couplings a hand edit triggers (`filament_diameter`,
`enable_prime_tower`, `gcode_flavor` ...) run after the write. `slot` (filament) first puts the Filament tab on that slot's preset,
which is a preset switch and therefore under `ifDirty` (`1013` by default when the tab has unsaved
edits; `1006` if the user declines under `"prompt"`).

```json
{ "applied": ["sparse_infill_density", "wall_loops"],
  "rejected": [ { "key": "print_host", "code": 1003, "message": "print-host address, credential or upload switch: never exposed over the bridge" },
                { "key": "layer_height", "code": 1002, "message": "value out of range [0.001, inf]" } ],
  "preset": { "type": "print", "selected": "0.20mm Standard @Unlayered", "dirty": true } }
```

Filament options that hold **one value per extruder variant** (`filament_options_with_variant`:
`nozzle_temperature`, `filament_flow_ratio`, `filament_max_volumetric_speed`, the retraction and
ramming keys, ...) must match the preset's `filament_extruder_variant` list (two entries on a Bambu
H2S/H2D: `"Direct Drive Standard"`, `"Direct Drive High Flow"`). A single value on a multi-variant
preset is applied to every variant and the key is listed in `data.variantResized`; any other length
is `rejected` with `1002`, as is a change to the length of `filament_extruder_variant` itself. A list
of the wrong length would shift that option for every later filament in the project.

When no key at all was applied the call fails with `1002` and the same `rejected` list in `data`.
A successful write is followed by `slicer.settingsChanged` with `source: "bridge"` (§4.5).

#### `slicer.getPlate`

No parameters.

```json
{ "printerTechnology": "FFF", "projectName": "bracket", "projectDirty": true, "isEmpty": false,
  "currentPlate": 0, "plateCount": 2, "bedMm": { "width": 220, "depth": 220, "height": 250 },
  "plates": [ { "index": 0, "isCurrent": true, "name": "", "hasPrintableInstances": true, "sliceResultValid": false } ],
  "objects": [ { "index": 0, "name": "bracket.stl", "plate": 0, "printable": true, "instances": 1,
                 "bboxMm": { "min": [90.1, 90.1, 0], "max": [129.9, 129.9, 12], "size": [39.8, 39.8, 12] } } ],
  "objectsTruncated": false }
```

`objects` is capped at 100 (`objectsTruncated`). `plate: -1` means the object sits on no plate.

**What would be printed.** Every instance is tested fresh on each call the way slicing tests it:
against the real bed shape (round, convex, custom) of the plate it sits on, the printable height,
the bed exclude areas, and the object's / instance's printable flags. The same verdict decides the
`nothing_printable` refusals of `slicer.slice`, `slicer.exportGcode` and `slicer.print`. Fields:

| Where | Field | Meaning |
|---|---|---|
| top | `hasPrintableInstances` | the current plate has at least one printable instance |
| top | `instanceCount`, `printableCount` | all instances / printable ones, all plates |
| top | `outsideCount` | instances refused for their placement (outside the bed, in an exclude area, or on no plate) |
| top | `offPlateCount` | instances on no plate (including any entirely below the bed surface) |
| top | `nothingPrintable` | only when the current plate has nothing printable: `{ reason, message }`, `reason` as `data.cause` below |
| `plates[i]` | `hasPrintableInstances` | `printableCount > 0` |
| `plates[i]` | `printableCount`, `outsideCount`, `excludeAreaCount`, `notPrintableCount` | per plate; `outsideCount` includes `excludeAreaCount` |
| `plates[i]` | `plateListHasPrintable` | the plate list's own cached flag (rectangular plate bounds, refreshed on moves and bed changes); diagnostics only |
| `objects[j]` | `instanceStates` | first 200 instances: `{ index, plate, placement, volumeState, outside, printable }` (`instanceStatesTruncated`) |
| `objects[j]` | `outsideCount`, `anyPrintable` | over all its instances |

`placement` is `printable`, `offPlate`, `outsideBed` (partly or fully outside the bed shape or above
the printable height), `excludeArea` (inside the bed shape but overlapping an exclude area) or
`notPrintable` (object or instance set not printable). `volumeState` is the bed-shape test alone:
`inside`, `partlyOutside` or `fullyOutside`. `outside` is true for every placement but `printable`
and `notPrintable`.

**Refusal when nothing is printable** (`slicer.slice`, `slicer.exportGcode`, `slicer.print`): `1008`,
message `"the plate has no printable object: <why>"` (`<why>` in the UI language), `data: { reason:
"nothing_printable", cause }` with `cause` one of `empty_plate`, `outside_bed`, `exclude_area`,
`not_printable`, or `nothing_printable` when several apply.
An object with multiplanar cut planes also carries `cutPlanes` -- the body `setCutPlanes` returns
(below), world frame.

#### `slicer.setCutPlanes`

Declarative multiplanar (5-axis) cut planes on **one object** -- what the Cut Planes gizmo
authors. They decide how the object is sliced exactly as a print setting does, so they belong to
this family and its gate. The object is never split; the planes are a non-destructive list on
the object that the 3MF round-trips.

```json
{ "object": 0, "mode": "replace", "frame": "world",
  "planes": [ { "point": [110.0, 110.0, 31.5], "normal": [0, 0.17, 0.985], "enabled": true },
              { "point": [110.0, 110.0, 60.0], "normal": [0, 0, 1], "buildB": -30, "buildC": 90 } ],
  "enabled": true, "userOrdered": false }
```

| Parameter | Required | Notes |
|---|---|---|
| `object` | when the plate holds more than one object | index from `getPlate`, or an exact object name. |
| `planes` | yes | up to 64 `{ point, normal, enabled?, buildNormal? \| buildB?/buildC? }`; `point` and `normal` are `[x, y, z]` in mm. The side the normal points to is printed along that normal, unless the plane's build direction is overridden: `buildNormal` `[x, y, z]` (in `frame`; `null` resets it to the plane normal) or `buildB` (-180..0) / `buildC` (-180..180), the machine angles the Cut Planes tool's B/C fields take. With `replace`, a plane that sets neither keeps the override of the old plane it rewrites unchanged (same plane within 0.01 mm / 0.05°), so planes read from `getModelTree` or `getPlate` can be written back without losing what the person set. An empty list with `replace` is the full reset deleting the object list's "Cut planes" item does: no planes, `enabled` off, the user order dropped (an explicit `enabled` still applies). `enabled: false` on a plane keeps it but ignores it when slicing. |
| `mode` | no | `"replace"` (default) swaps the object's planes for these; `"append"` adds them. |
| `frame` | no | `"world"` (default): plate coordinates, converted to object-local with the instance's transform the way the gizmo does. `"object"`: already object-local. |
| `enabled` | no | the object's cut-plane printing on/off. Absent: switched on whenever planes exist (authoring planes implies wanting the feature), as the gizmo does. |
| `userOrdered` | no | `true`: the list order decides the print order -- the first plane peels last, so it prints last (the tool's "Automatic order" unticked plus its reorder arrows). `false`: automatic order. Absent: unchanged. |

On a **virtual 5-axis printer** every plane normal and build direction is held at the printer's
tilt limit as the Cut Planes tool holds it while authoring (`virtual_max_tilt_deg` for an
unmeasured chunk, which is the hotend term); each change is listed in `tiltClamped`
(`[{plane, what: "normal"|"buildNormal", tiltDeg, limitDeg}]`). `Print::validate` stays the
backstop for a chunk whose own reach is stricter. Other printers have no limit.

While any tool (gizmo) is open the method answers `1007` with `data.gizmo`, like every catalog
action; close it with the catalog's `ui.closeGizmo`. The same holds for `setOrientation`,
`setObjectFilament`, `setScale`, `moveObject`, `arrange`, `setCopies` and `fillBed`.

An undo snapshot is taken, so Ctrl+Z takes it back like a gizmo edit; the current slice is
invalidated and the object list's cut-planes badge updates. Errors: `1002` for a malformed plane,
`1008` for an unknown object or an empty plate. The reply:

```json
{ "object": 0, "name": "benchy.stl", "planeCount": 1, "printSequenceByGroup": false,
  "cutPlanes": { "enabled": true, "active": true, "userOrdered": false, "frame": "world",
                 "planes": [ { "point": [...], "normal": [...], "enabled": true },
                             { "point": [...], "normal": [...], "enabled": true,
                               "buildNormal": [0.433, -0.75, 0.5], "buildAnglesDeg": { "b": -60, "c": 60 } } ] },
  "buildNormalsKept": 1,
  "hint": "cut-plane printing needs the print setting print_sequence = \"by group\"; set it with slicer.setSettings" }
```

`buildNormal` / `buildAnglesDeg` (the override as machine angles) appear only on planes whose
build direction is overridden. `buildNormalsKept` counts overrides a replace carried over;
`buildNormalsDropped` (with a `note`) counts overrides whose plane was moved or removed.
`hint` is present only while the object has active planes and the print sequence is not
"by group" -- the page is expected to set `print_sequence` through
`setSettings`, which is the user's own unsaved edit like any other.

#### `slicer.setOrientation`

Rotates **one object** on the plate and drops it back onto the bed -- what the Rotate gizmo or
Lay on Face does. Same gate as `setCutPlanes`.

```json
{ "object": 0, "face": "+x" }
{ "object": 0, "rotation": [90, 0, 0] }
```

| Parameter | Required | Notes |
|---|---|---|
| `object` | when the plate holds more than one object | index from `getPlate`, or an exact object name. |
| `face` | one of `face` / `rotation` | which side of the part **as it sits now** goes down onto the bed: `"+x"` puts the side facing +X down, `"+z"` flips it upside down, `"-z"` changes nothing. |
| `rotation` | one of `face` / `rotation` | `[x, y, z]` degrees about the plate's axes, applied X then Y then Z on top of the current pose. |

Every instance of the object turns about its own bounding-box centre, so it stays where it is on
the plate, and is then dropped onto the bed. An undo snapshot is taken (Ctrl+Z) and the current
slice is invalidated. Errors: `1002` for neither or both of `face`/`rotation` or a bad value,
`1008` for an unknown object or an empty plate. The reply carries the new size and the instance's
rotation in degrees:

```json
{ "object": 0, "name": "casing.stl", "size": [34.0, 104.0, 66.0], "rotation": [0, 90, 0] }
```

#### `slicer.setObjectFilament`

Assigns **one object** to a filament slot -- the object list's filament column. The object takes
the slot and its parts drop their own assignment, so the whole object prints and shows in that
slot's colour. Same gate as `setCutPlanes`.

```json
{ "object": 0, "filament": 2 }
```

| Parameter | Required | Notes |
|---|---|---|
| `object` | when the plate holds more than one object | index from `getPlate`, or an exact object name. |
| `filament` | yes | the slot number **as the sidebar shows it, from 1** (unlike the 0-based `slot` of `selectPreset` / `setSettings`). |

Undoable (Ctrl+Z); the current slice is invalidated. Errors: `1002` for a slot out of range,
`1008` for an unknown object or an empty plate. Reply:
`{ "object": 0, "name": "casing.stl", "filament": 2, "preset": "Bambu PLA Matte @BBL X1C" }`.

#### Native plate edits: `setScale`, `moveObject`, `arrange`, `setCopies`, `fillBed`

What the sidebar's Scale and Move gizmos, Arrange, the +/- instance buttons and Fill Bed do, so a
page never needs a model round trip to resize, place or multiply parts. Rules shared by the five:

- `object` is an index from `getPlate` or an exact object name, required when the plate holds more
  than one object (`1002`); an unknown object or an empty plate is `1008`.
- `1007` while a dialog or a tool (gizmo, `data.gizmo`) is open, a slice is running, or an arrange /
  fill-bed job is running (only one at a time, whoever started it).
- Every edit is one undo step (Ctrl+Z). `setScale` (factor/sizeMm) and `moveObject` take a
  snapshot named "Scale (automated)" / "Move (automated)"; fit, copies, arrange and fill bed run the
  slicer's own command, which takes its own snapshot ("Scale To Fit", "Increase/Decrease
  Instances", "Arrange").
- The current slice is invalidated, the object list and plate membership follow, and parts stay
  on the bed. Scaling is **uniform** only.
- `bboxMm` is `{ "min": [x,y,z], "max": [x,y,z] }` in plate mm. `insidePlate` / `fitsBed` are
  computed geometrically against the plate's print volume (bed area and printable height), so they
  are right in the reply itself. `false` is a successful answer for the page to report, not an
  error.

##### `slicer.setScale`

```json
{ "object": 0, "factor": 1.5 }
{ "object": 0, "sizeMm": { "axis": "z", "mm": 120 } }
{ "object": 0, "fit": true }
```

Exactly one of: `factor` (> 0, multiplies the CURRENT size: 2 = twice as big), `sizeMm` (`axis`
`"x"`, `"y"`, `"z"` or `"max"` = the longest side of the current bounding box becomes `mm`), or
`fit: true` (the largest uniform scale that fits the print volume -- the sidebar's Scale to fit).
The resulting factor must lie in 0.001..1000. Each instance keeps its XY centre and is dropped to
the bed. Reply:

```json
{ "object": 0, "name": "casing.stl", "scalePercent": [150, 150, 150], "sizeMm": [99, 156, 51],
  "bboxMm": { "min": [...], "max": [...] }, "fitsBed": true }
```

`scalePercent` is instance 0's scale relative to the original mesh.

##### `slicer.moveObject`

```json
{ "object": 0, "to": [128, 128] }
{ "object": 0, "instance": 2, "by": [10, 0] }
{ "object": 0, "center": true }
```

Optional `instance` (default 0), plus exactly one of `to: [x, y]` (that instance's bounding-box
centre goes there, plate mm), `by: [dx, dy]`, or `center: true` (centre of the plate it is on, or
of the current plate when it is off every bed). Z never changes. Reply:
`{ "object": 0, "name": "casing.stl", "instance": 0, "bboxMm": {...}, "insidePlate": true }`.

##### `slicer.arrange`

```json
{ }
{ "plate": 1, "spacingMm": 5, "allowRotation": true }
{ "allowRotation": false, "alignToYAxis": true, "allowMultiMaterials": false }
{ "all": true }
```

The Arrange command on the current plate (or `plate`, which becomes the current plate), or on every
plate with `all: true`. `spacingMm` (0..100), `allowRotation` and the rest of the Arrange options
popup apply to this run only; the user's own Arrange settings are restored afterwards.
`allowMultiMaterials` (objects of different filaments may share a plate), `avoidCalibrationRegion`
(keep clear of the extrusion calibration area; only printers that scan the first layer, else `1002`)
and `alignToYAxis` (needs rotation off, as in the popup; with rotation on it is `1002`). Arrange is a slicer job: **the reply comes when the job
has finished** (the request stays open). If the job is still running after 55 s the bridge answers
`1012` "did not finish in time" -- inside the shim's fixed 60 s call timeout, so the page always
hears an outcome; the job itself carries on and its result shows in the next `getPlate`. Reply:

```json
{ "plates": [0], "objects": [ { "index": 0, "name": "casing.stl", "plate": 0, "instances": 1,
  "bboxMm": {...} } ], "unplaced": [] }
```

`objects[].plate` and `bboxMm` are instance 0's; `unplaced` lists objects with any instance not
wholly inside a plate after arranging.

##### `slicer.setCopies`

```json
{ "object": 0, "count": 6, "arrange": true }
```

`count` is the TOTAL number of instances wanted (1..200); the +/- instance buttons add or remove
the difference. `1007` when the slicer refuses (a cut object, a non-printable instance, a gizmo
open). With `arrange: true` the current plate is arranged afterwards and the reply waits for it,
as `arrange` does. Reply: `{ "object": 0, "name": "casing.stl", "instances": 6, "insidePlate": true }`.

##### `slicer.fillBed`

```json
{ "object": 0 }
```

Selects the object and runs Fill Bed (as from its right-click menu); the object stays selected.
A job: the reply comes when it has finished. Reply:
`{ "object": 0, "name": "casing.stl", "instances": 14, "added": 13 }`.

#### `slicer.slice`

```json
{ "plate": 0 }
```

`plate` (optional) selects that plate first. Refused with `1014` (`data: { reason:
"service_mode_selected", selected }`) while a slicing-service mode is selected in the Slice
dropdown -- before anything changes, the plate switch included: see "No service run from a page"
below. Refused with `1008` when the plate has nothing printable (`data.reason: "nothing_printable"`,
`data.cause`; see `slicer.getPlate`) or a G-code file is open in preview mode, `1007` while a slice is running. Then it does what the Slice button does — which may decline for reasons the slicer shows as a notification —
and answers immediately:

```json
{ "accepted": true, "plate": 0, "sliceResultValid": false }
```

`accepted: false` means no slice is under way (the plate was already sliced and unchanged, or the
slicer refused). The outcome arrives as `slicer.sliceStarted` then `slicer.sliceFinished` (§4.5);
wait for those, not for the response.

#### `slicer.getSliceModes`

No params. The Slice dropdown's slicing modes and the selected one:

```json
{ "modes": [ { "id": "local", "label": "Slice plate", "kind": "local" },
             { "id": "local_all", "label": "Slice all", "kind": "local" },
             { "id": "<service mode>", "label": "<service label>", "kind": "service" } ],
  "selected": "local" }
```

`local` and `local_all` are fixed. The `service` entries exist only when a slicing service is
configured. Each is one mode the service lists in its discovery document (`GET <service>/`,
`modes[]`, [SLICE_SERVICE_PROTOCOL.md](SLICE_SERVICE_PROTOCOL.md)) and offers on this machine:
`id` is the mode's own id and `label` the dropdown's. A service without discovery gives a single
entry with `id: "service"`. Service ids
come from the service at runtime, so the page should read them here, never hard-code them. The
list changes with the printer and when the form is refetched. `selected` is one of the ids.

With a slicing service configured the reply also has `service`:

```json
{ "prepare": "deferred",
  "plates": [ { "plate": 0, "serviceResult": false, "serviceResultCurrent": false, "resultWaiting": false,
                "jobRunning": true, "progress": 42, "stage": "Slicing: infill...", "waitingForSlice": false } ] }
```

`prepare` is `off` (a local mode), `active`, or `deferred` (a page preselected the mode; the
background prepare waits for the person). Per plate: `serviceResult` -- its valid result is the
service's (`serviceResultCurrent`: made with the mode and options it would be sent now);
`resultWaiting` -- a result is back and is taken when the plate is shown; `jobRunning` with the
job's last `progress` and `stage`; `waitingForSlice` -- a service mode is selected and the plate needs
a service run that only the person can start: tell them to press Slice.

#### `slicer.setSliceMode`

```json
{ "id": "local" }
```

Selects that dropdown entry as the person picking it does: the button label and, for a service
mode, the remembered mode for this printer. It starts **no** slice, opens no dialog and sends
nothing to the service: the background prepare upload a person's own pick starts (`SlicePrepare`,
`POST <service>/prepare`) stays deferred until the person presses Slice, opens Preview or picks the
mode themselves (`getSliceModes` reports `service.prepare: "deferred"` meanwhile). Reply:
`{ "selected": "<id>", "modes": [...] }` (as `getSliceModes`). `1002` with
`data.reason: "unknown_mode"` for an id not in `getSliceModes`. The slicer does not check whether
the account may use a service mode; the service answers that when the person slices.

#### No service run from a page

A slicing-service run sends the person's plate to another machine. A page may preselect a service
mode, but it can never start a run, directly or indirectly:

- `slicer.slice` answers `1014` (`data.reason: "service_mode_selected"`) while a service mode is
  selected. This is not an account check: it holds for every account and every service.
- Every bridge call is handled inside a `Plater::ServiceSliceBlock`. While one exists, no service
  run starts, whatever the handler calls.
- `Plater::reslice()` starts a service run only on a pending request from the person,
  `Plater::note_user_slice_request()`. That is made only by the person's own controls: the Slice
  button, Ctrl+R, a click on the Preview tab button (`wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED`, which only
  a tab-button click posts), a plate picked in Preview (`select_plate(..., need_slice=true)`, which
  the bridge never passes), and the slice-plate event (posted only by the person's controls). The
  request is consumed by the next `reslice()` and expires after 2 s.
- Preselecting a service mode with `setSliceMode` starts no background prepare upload either; it
  waits for the person's own request (`MainFrame::activate_deferred_slice_prepare`).
- The automatic re-slice after a change, a service result arriving, an arrange or plate switch, and
  a settings change never start one, whoever made the change. The plate waits, unsliced and with
  nothing sliced locally either, until the person presses Slice or opens Preview.

#### `slicer.getSliceResult`

```json
{ "plate": 0 }
```

`1008` unless that plate (default: the current one) holds a valid slice result; its `data` is
`{ plate, service }` with the plate's slicing-service state (as in `getSliceModes`), so
`service.waitingForSlice` says the person has to press Slice and `service.jobRunning` that a service
slice is on its way. A valid result adds `source` (`"service"` or `"local"`) and the same `service`.

```json
{ "plate": 0, "timeSeconds": 2711.4, "prepareTimeSeconds": 61.2, "filamentMm": 4123.7, "filamentG": 12.3,
  "cost": 0.31, "filamentChanges": 0, "travelDistanceMm": 9876.5, "toolpathOutside": false, "gcodeCheckError": 0 }
```

`gcodeCheckError` is a bit mask: `1` a multi-extruder printer's per-extruder area, `2` its
per-extruder height, `4` paths outside the plate, `8` paths above the printable height, `16` the
wrapping-detection area, `2048` printed weight over the printer's limit.

#### `slicer.getSliceIssues`

Why a plate will not slice, or what its last slice warned about, as the notifications show it. It
answers for any plate (default: the current one), sliced or not, and never starts anything.
Settings family. Since 0.2.0; check `capabilities` before calling it.

```json
{ "plate": 0 }
```

```json
{ "plate": 0, "isCurrent": true, "slicing": false, "hasPrintableInstances": true, "sliceResultValid": false,
  "lastSlice": { "plate": 0, "ok": false, "cancelled": false, "error": "No layers were detected. …", "secondsAgo": 41 },
  "validation": {
    "error": { "message": "Layer height cannot exceed nozzle diameter.", "key": "layer_height",
               "type": null, "object": { "index": 0, "name": "bracket.stl" } },
    "warnings": [ { "message": "…", "key": "outer_wall_speed", "type": null, "object": null } ] },
  "warnings": [ { "level": "warning", "message": "…floating regions…", "id": 0, "step": "support_material",
                  "object": { "index": 0, "name": "bracket.stl" } } ],
  "gcode": { "toolpathOutside": false, "checkError": 4, "checkFlags": ["plate_area"],
             "conflict": { "objects": ["a.stl", "b.stl"], "wipeTower": false, "heightMm": 12.4, "layer": 61, "message": "" },
             "warnings": [ { "level": 1, "kind": "not_generate_timelapse", "code": "10014001", "params": [] } ] } }
```

- `lastSlice` is the most recent `slicer.sliceFinished` payload without `stats`, whichever plate it
  was, plus `secondsAgo`; `null` until a slice has finished in this session. The slicer keeps it even
  while no page is loaded, so a page opened after a failed slice can still read the error.
- `validation` is what the Slice button checks before it starts (`Print::validate`): `error` blocks
  slicing, `warnings` do not. `key` is the setting the slicer names as the cause, when it names one;
  `type` is one of `filament_not_match_bed_type`, `filaments_different_temp`,
  `object_collision_in_seq_print`, `object_collision_in_layer_print`, `layer_height_exceeds_limit`, or
  `null`. `null` as a whole while slicing and for a plate other than the current one.
- `warnings` are the current slicing warnings of the plate's print (the notification texts), with the
  pipeline `step` they came from and the object, numbered as `slicer.getPlate` numbers objects (`null`
  for a whole-print step). Empty while slicing.
- `gcode` is present for a plate with a valid slice result: `checkError` and its `checkFlags` (the
  mask above), `conflict` when two objects' paths (or an object and the wipe tower) collide, including
  the hotend collision check's `message`, and the G-code processor's printer warnings.
- Messages are in the slicer's language and capped at 600 characters; `key`, `type`, `step`,
  `checkFlags` and `kind` are the stable fields to match on. At most 50 entries per list.

### Completion is two-stage

`code: 0` from either open method means **accepted, not finished**. The slicer may put a modal in
front of the user ("discard the current project?") or spend a while downloading. The outcome
arrives as an event carrying the same `id`:

```json
{ "type": "event", "id": "wa-7", "method": "slicer.projectOpened",
  "data": { "ok": true, "path": "…" } }

{ "type": "event", "id": "wa-7", "method": "slicer.projectOpened",
  "data": { "ok": false, "error": "checksum mismatch: expected …, got …" } }
```

A G-code open adds `projectAside` (`true` when the user's project was stashed rather than closed;
`false` when there was nothing on the plate, or the stash failed and the old dialog ran) and, when
`printer` was sent, `printer` — the preset actually selected afterwards.

Keep your acknowledgement timeout short and wait on the event for the real result.

**What `ok: true` does and does not promise.** It means the file was fetched,
checksum-verified if you supplied one, and handed to the slicer's own open path. That
path then runs asynchronously and may put a "discard the current project?" modal in
front of the user. The slicer offers no completion callback there, so **a user who
cancels that modal still produces `ok: true`.** Everything that can go wrong before
that point — forbidden host, unsupported type, missing file, failed download,
checksum mismatch, busy slicer — is reported accurately as `ok: false` with a reason.

Design your UI around that: "handed to the slicer" is a safe thing to say on
`ok: true`; "the model is now on the plate" is not. This is a known limitation of v1
rather than a guarantee to be tightened later without notice.

---

### 4.2 Publishing the open project — behind a separate per-tab opt-in

Three methods let a page take what is on the plate, so it can be shared without the user saving a
3MF by hand and finding it again in a file picker. They appear in `capabilities` only when the
tab's `<slot>_allow_export` is on, and answer `1005` while it is off.

This is a **separate permission from `allow_settings`**, because it is a different thing to agree
to: the user's model leaving the machine, rather than the slicer's own settings being read. A tab
may hold either without the other.

The page never names a path. The slicer chooses where the export goes, writes it into its own
cache directory and returns an opaque `handle`; every later call is in terms of that handle. This
is why the family is not the `slicer.readFile` the bridge refuses to grow — it can only hand back
a file the slicer just produced from state the user already has open.

#### `slicer.exportProject`

Params: `{plate?}` — one plate by index, or the whole project when omitted.

Returns `{handle, filename, sizeBytes, chunkBytes}`. `filename` is derived from the project name.
Errors: `1005` not permitted, `1008` the plate is empty, `1007` a slice is running or too many
exports are still open (three at once per tab), `1002` the export is over the size limit, `1012`
the slicer could not export.

The export runs with the slicer's silent save strategy, so it never raises a dialog of its own.

#### `slicer.readExportChunk`

Params: `{handle, offset?, length?}`. Returns `{handle, offset, length, nextOffset, eof, base64}`.

Reads are capped at `chunkBytes` (4 MB) per call regardless of `length`. The payload is plain
base64, not a data URL. Chunked rather than one response because the bytes cross the process
boundary inside a JSON message: a 100 MB project in one reply would be a 133 MB string and a
visible stall. Loop until `eof`, using `nextOffset` — a chunk that does not advance is a fault,
not a reason to retry.

#### `slicer.exportThumbnail`

Params: `{plate?}` — defaults to the current plate. Returns `{plate, mime, width, height,
sizeBytes, base64}`: the picture the slicer already renders of that plate, as a PNG.

It exists because a sharing site will not take an upload without a photo, and asking someone to
go and find a screenshot of a model the slicer is already drawing is absurd. Small enough to come
back in one response, unlike the project itself.

Errors: `1008` the plate is empty, **or no picture has been rendered yet**. Rendering needs the 3D
canvas, which is not drawn while a web-application tab is in front, so on a cold start there may
be nothing to hand over until the Prepare tab has been shown once. Treat a missing picture as
something the user can supply instead, never as a reason to abandon the upload.

#### `slicer.discardExport`

Params: `{handle}`. Deletes the export. Idempotent, so a page cleaning up after a failure does not
have to work out whether the export survived. The slicer also discards every live export when the
tab closes, so a page that never calls this leaks nothing permanent.

### 4.3 Plate hand-off -- the plate reaches the page

Two user actions hand the current plate to a tab's page: **"Slice with <tab title>"** in the
slice menu (generic web-app tab; `action: "slice"`), and **opening** the generic tab when the
plate changed since that tab last got it (`action: "show"`; a fingerprint of objects, transforms,
plate and presets decides). The slicer exports the plate as a self-contained `.3mf` (settings
embedded, like a saved project), parks it as the tab's pending hand-off and fires `slicer.handoff`
with the same body. Because the click is the consent, `readExportChunk` and `discardExport` accept
**that handle** even while `<slot>_allow_export` is off -- nothing else opens up.

The side panel is handed nothing automatically: it is on screen most of the time, and exporting
the plate on every change would be wasteful. Its page **pulls** the plate instead, with
`slicer.requestHandoff` below, right before a message goes out and only when the plate changed
since it last did.

#### `slicer.requestHandoff`

In `capabilities` for a slot with the settings **or** the export permission on (a page trusted to
rewrite every print setting of the plate may read the plate it is editing); `1005` otherwise.
Params: `{ plate? }` (default: the current plate). Exports the plate exactly as a hand-off does and
answers with the hand-off body directly (`action: "request"`) -- nothing is parked and no
`slicer.handoff` event is fired, so the page never sees the same plate twice. `1008` when the
plate is empty. Read the bytes with `readExportChunk` (allowed for this handle on the same terms
as a user hand-off) and finish with `discardExport`.

#### `slicer.takeHandoff`

Always in `capabilities`. Returns and **clears** the pending hand-off -- once, so a reload does
not import the same plate twice:

```json
{ "pending": true, "action": "slice", "handle": "3f2a...", "filename": "bracket.3mf",
  "sizeBytes": 812345, "chunkBytes": 4194304, "plate": 0,
  "printer": "My Voron 0.4 nozzle", "process": "0.20mm Standard @MyVoron", "filament": "Generic PLA @System" }
```

or `{ "pending": false }`. Call it once the page is ready (a never-shown tab is loaded by the
hand-off itself, after the event has gone) and again on every `slicer.handoff` event; read the
bytes with `readExportChunk` and finish with `discardExport`. A newer hand-off supersedes an
uncollected one.

### 4.4 Printer profiles

Two methods let a site share a printer profile: one user publishes their printer,
another installs it. They deal in **printer** presets only — never print or filament presets,
bundles or physical printers.

**What never travels.** A shared profile carries no print host: not the address, API key, user,
password, CA file, port, authorisation type, host type or upload switch (the `WAB_SECRET_KEYS` list
in `WebAppSettings.cpp`), and not `bed_custom_texture` / `bed_custom_model`, which are absolute
paths on the author's disk. They are removed on export **and removed again on install, whatever the
file says**, so a profile from the web can never point the installer's uploads at someone else's
machine.

#### `slicer.exportPrinterPreset`

Part of the export family (§4.2): in `capabilities` only while `<slot>_allow_export` is on, else
`1005`. Params: `{}` (unknown keys ignored).

Writes the printer preset in force — the **edited** one, as the sidebar shows it, unsaved edits
included — as a complete Orca user-preset JSON: every printer option, flattened so it does not need
its parent, plus `name`, `from: "User"`, `inherits` (the parent's name, kept as is; `""` for a
custom printer), `version` and `printer_settings_id` (= `name`). Read it with `readExportChunk` and
finish with `discardExport`, exactly like a project export; it counts towards the three live
exports.

```json
{ "handle": "3f2a…", "filename": "My Voron.json", "sizeBytes": 18734, "chunkBytes": 4194304,
  "name": "My Voron", "dirty": true, "inherits": "Voron 2.4 350 0.4 nozzle" }
```

`dirty: true` means the file holds edits the user has not saved to the preset. Errors: `1005` not
permitted; `1008` the selected printer is a system or default preset, with the message *"Save this
printer as your own preset first: system printer presets can't be shared."* — a vendor's preset is
not the user's to publish, a saved copy of it is; `1007` a dialog is open, a slice is running, or
three exports are still open; `1012` nothing was written.

#### `slicer.installPrinterPreset`

Its own permission, `<slot>_allow_install` (§7), off by default; absent from `capabilities` and answered with `1005` while off. Even with it on, **nothing is
installed until the user agrees in a native dialog that the page can neither skip, pre-answer nor
restyle.** The permission only lets a page ask.

```json
{ "url": "https://profiles.example.com/…/9f2c…", "filename": "My Voron.json",
  "sha256": "e3b0c44298fc1c149afbf4c8996fb924…" }
```

| Parameter | Required | Notes |
|---|---|---|
| `url` | **yes** | Exactly the rules of `openProjectFromUrl`: `http`/`https`, the page's own host unless listed in `<slot>_download_hosts` (else `1003`). |
| `filename` | recommended | Only the extension is used, and it must be `.json` (else `1009`). Falls back to a name derived from the URL. |
| `sha256` | optional but advised | Lowercase hex. On mismatch nothing is installed. |

Refused at once with `1002`, `1003`, `1005`, `1009`, or `1007` (a dialog is open, a slice is
running, another download or install is in flight). Otherwise the answer is `{ "accepted": true }`
straight away and the outcome arrives later as `slicer.presetInstalled`, carrying the request's
`id` (two-stage, like `openProjectFromUrl`):

```json
{ "ok": true,  "name": "My Voron", "selected": "My Voron" }
{ "ok": false, "cancelled": true }
{ "ok": false, "error": "not a printer profile", "code": 1009 }
```

`cancelled: true` is the user saying no in the dialog — not an error, and not worth an error
message. On other failures `error` says why and `code` is a §5 code (`1011` larger than 4 MiB,
`1008`/`1010` download failed, `1009` not a printer profile, `1002` unusable or system-reserved
name, `1007` the slicer became busy, `1012` the import failed).

What happens between the two:

1. **Download**, at most 4 MiB.
2. **Parse**, as a preset file from disk is parsed, legacy migrations included. It needs
   `printer_settings_id`, `name` and a `version` the slicer can parse, else `"not a printer profile"`.
3. **Sanitise** — see above.
4. **Parent.** An `inherits` preset installed here is kept. One that belongs to a vendor this slicer
   ships but has not installed is offered in the dialog as a tick box (off by default) to install
   that vendor too. Otherwise — or if the box stays unticked — `inherits` is dropped and the profile
   installs as a standalone printer, which is complete because exports are flattened. A name a
   system printer preset already uses is refused: `"a system printer preset already has this name"`.
5. **Confirm.** One modal dialog, *Install printer profile from <tab title>*: the name; whether it
   replaces the user's own preset of that name; printer type, bed size, nozzle diameter(s), G-code
   flavor; for a non-planar printer the hotend envelope and gantry height; the parent; and a
   **custom G-code review** listing every line of every `*_gcode` option (comments stripped) that
   uses one of these M-codes, with why — M997 firmware update; M30 deletes a file; M28/M29 write a
   file; M559/M560 upload/write a file; M500 saves settings permanently; M502 resets settings to
   defaults; M552/M553/M554/M587/M588/M589 change network settings; M564 axis-limit enforcement;
   M302 cold extrusion; M143 a heater's temperature limit; M906/M913 motor current; M208 axis travel
   limits; M669 kinematics; M584 drive mapping; M999 restarts the controller — and the macros the
   G-code calls with `M98 P"…"`, which must exist on the user's printer.
6. **Install.** Imported as a user preset (replacing the user's preset of the same name, as the
   dialog said), presets refreshed, then selected the way `slicer.selectPreset` selects a printer —
   so if the current printer has unsaved edits the slicer asks the user first. `selected` is the
   printer in force afterwards and differs from `name` when the user kept their current printer. If
   the replaced preset was the one in force, its unsaved edits are dropped (the dialog says so).

---

## 4.5 Events

Besides the request-correlated events above, the slicer pushes these unsolicited ones (no `id`),
plus `slicer.presetInstalled`, which is listed here too although it answers a request and carries
its `id`. Subscribe with `window.__slicerBridge.on(name, fn)`. The settings-family events —
`presetChanged`, `settingsChanged`, `plateChanged`, `sliceStarted`, `sliceFinished` — reach a tab
only when its settings opt-in is on (§4.1); the rest are not settings-gated.

| Event | Data | When |
|---|---|---|
| `slicer.presetChanged` | `{ type, selected, dirty, slot? }` | a sidebar preset was switched, by the user or by the bridge. `slot` is present for a multi-filament slot change. |
| `slicer.settingsChanged` | `{ type, keys: [...], source: "user" \| "bridge", preset, changes: [{ key, label, category, tooltip, value, previous, saved, system }] }` (`preset` and `changes` on bridge writes too since 0.2.0, feature `settingsChanged.bridgeChanges`; `previous` there is the exact value before the write) | a value changed in a settings tab. **`source: "bridge"` is your own write coming back — never read it as user intent.** `changes` says what changed -- what the person did, or for `source: "bridge"` what your own write did: `value` is the new value, `previous` the value just before this edit, `saved` the value in the saved preset and `system` the built-in preset's (each the `.ini` text, at most 2000 characters, or `null`). |
| `slicer.plateChanged` | `{ currentPlate, plateCount, objectCount }` | objects or plates changed. Coalesced: one event per burst of changes. |
| `slicer.sliceStarted` | `{ plate }` | a slice began. |
| `slicer.sliceFinished` | `{ plate, ok, cancelled, error?, stats? }` | a slice ended. `stats` is the `getSliceResult` body when the plate ended with a valid result. |
| `slicer.handoff` | the `takeHandoff` body | the user handed the plate to this tab (4.3). Not settings-gated. Collect it with `slicer.takeHandoff`. |
| `slicer.presetInstalled` | `{ ok, cancelled?, name?, selected?, error?, code? }` | the outcome of a `slicer.installPrinterPreset` (§4.4), with that request's `id`: installed (`name`, and `selected` = the printer in force now), declined in the dialog (`cancelled: true`), or failed (`error`, `code`). |
| `slicer.themeChanged` | `{ darkMode }` | the user toggled light/dark. Sent only to the side panel, which keeps its page across the toggle instead of reloading; the tabs reload. |
| `slicer.capabilitiesChanged` | `{ capabilities }` | a Preferences toggle changed what you may call. Re-read it. |
| `slicer.projectChanged` | `{ reason: "new" \| "open", source: "user" \| "bridge", generation, slot? }` | the window switched to another project (features `projectChanged` and `event.projectChanged`, 0.2.0), once, after it succeeded (a cancelled or failed one sends nothing). See below. Not settings-gated. |
| `slicer.openProjectFromUrl.progress` | `{ received, total }` | download progress of an `openProjectFromUrl` (§4), with its `id`. |
| `slicer.projectOpened` | `{ ok, path?, error?, code?, projectAside?, printer? }` | the outcome of `openProject` / `openProjectFromUrl`, with its `id` ("Completion is two-stage"). |
| `slicer.exportFinished` | `{ ok, cancelled?, path?, error?, reason?, cause?, message? }` | the outcome of `slicer.exportGcode` (§4.6), with its `id`. |
| `slicer.printSent` | `{ printer, ok, cancelled?, error? }` | the outcome of `slicer.print` (§4.6), with its `id`. |
| `slicer.printerControlDone` | `{ printer, action, ok, cancelled?, command?, error? }` | the outcome of `slicer.printerControl` (§4.6), with its `id`. |
| `slicer.actionFinished` | `{ jobId, id, ok, result \| error, effects, undo, turn }` | a long-running `slicer.invokeAction` finished (§4.7). Settings-gated. |
| `slicer.selectionChanged` | `{ objects, volumes, instances, plate }` | the selection on the plate changed (§4.7). Settings-gated. |
| `slicer.actionsChanged` | `{ catalogVersion }` | the action catalog changed; re-read `listActions` (§4.7). Settings-gated. |
| `slicer.sendFinished` | `{ printer, plate, ok, cancelled? }` | the outcome of `slicer.sendToPrinter` (§4.6), with its `id`. |
| `slicer.printHostJobCancelled` | `{ id, ok, cancelled?, error? }` | the outcome of `slicer.cancelPrintHostJob` (§4.6), with its `id`. |

**`slicer.projectChanged`** is sent when the plate is replaced by a different project:

- `reason: "new"`: File > New Project (or its shortcut / toolbar button), and the new project a
  calibration starts.
- `reason: "open"`: File > Open Project, a recent project, a `.3mf` dropped on the window or
  opened from the file manager and opened as a project, a project downloaded from a model site.
- `source: "bridge"` when a page asked for that project with `slicer.openProject` /
  `slicer.openProjectFromUrl`; `slot` is then the asking tab's (`side_panel`, `web_app`, ...), so a page can
  tell its own open from the person's. Also `source: "bridge"` (without `slot`) when any other bridge
  request caused it (the action catalog's project actions, calibration). Otherwise `source: "user"`
  and no `slot`.
- `generation` is the new `projectGeneration` (§4 `slicer.getInfo`).

It is **not** sent for: importing a model (STL, STEP, ... or a `.3mf` imported as geometry only)
into the current project; opening G-code or a sliced `.gcode.3mf`, which are previews of output,
including the project set aside for such a preview and brought back; restoring the project after a
crash or after the window is rebuilt for a theme or language change. A new window does not send it
either: it has a new `instanceId` instead.

## 4.6 Printers: export, print and control (0.2.0), behind a per-tab permission

A page may export G-code, send a plate to a printer and control printers. Three rules hold for every
method here:

1. **Opt-in per slot.** `<slot>_allow_printer` (§7), off by default. Off means every method below answers `1005` and none of them is in `capabilities`.
2. **The person confirms in a native slicer window, never in the page.**
   - Exporting uses the slicer's own Save dialog.
   - Printing uses the slicer's own Print / Send dialog (Bambu: *Send print job*; print hosts: *Send to
     print host*).
   - Control uses a confirmation that shows the printer, the action and the exact command. The page
     cannot press any of these.
   - **Exceptions: `pause` and `stop` act at once.** They are the safety actions, and a dialog would only
     slow them down.
3. **The page never sees a printer's address, access code or API key.**
   - Printers are named by an opaque `id`, valid for this bridge session.
   - The printers are the ones the slicer already prints to: Bambu printers it knows (LAN or cloud),
     and the selected printer preset's print host (Duet/RRF, OctoPrint, Moonraker, ...).

### `slicer.listPrinters`
`{}` → `{ printers: [{ id, name, kind: "bambu" | "host", model, hostType?, online, state, selected,
controllable, statusReadable }] }`.
- `state` is `idle` / `printing` / `paused` / `finished` / `failed` / `offline` / `unknown`.
- `hostType` is set for `kind: "host"`: `duet`, `octoprint`, `moonraker`, ...
- `controllable` says whether `printerControl` works on this printer:
  - Bambu: yes.
  - Duet/RRF: yes.
  - Klipper: yes when the slicer talks to it through the device connection; no for plain OctoPrint or
    Moonraker print hosts. Those can only print.

### `slicer.printerStatus`
`{ printer }` → what the slicer knows about that printer. Fields a printer does not report are absent.
- Every printer: `state`, `progress`, `remainingMinutes`, `layer`, `totalLayers`, `job`, `temperatures: { bed,
  bedTarget, nozzle: [..], nozzleTarget: [..], chamber, chamberTarget? }`.
- Bambu printers and device connections add what the Device tab shows:
  - `speedLevel` (1 silent - 4 ludicrous), `stage` (the printer's stage text), `firmware`, `storage`
    (`none` / `normal` / `abnormal` / `readonly`).
  - `limits: { bedMax, nozzleMin, nozzleMax, chamberMin?, chamberMax? }`: the printer's own caps, which
    `printerControl` enforces.
  - `extruders: [{ id, nozzleType, nozzleDiameter, hasFilament, loaded?: { ams, slot } }]`, `currentExtruder`.
  - `fans: [{ id, key, percent, control: "manual" | "auto" | "off" }]`, and `airduct: { mode, modes, coolingFilter,
    supportsCoolingFilter }` on printers with air-duct modes.
  - `light: { on, offWarns }` (`offWarns`: turning it off breaks AI monitoring of the running print).
  - `ams: { units: [{ id, type, extruder, humidityLevel?, humidityPercent?, temperature?, drying?: { remote, status,
    leftMinutes, cannotDryReasons, settings? }, trays: [{ slot, exists, type, color, colors?, remain, filamentId,
    settingId, bambuSpool, nozzleTempMin?, nozzleTempMax? }] }], externalSpools: [{ ams: 255 | 254, slot: 0, ... }] }`.
  - `filamentChange: { inProgress, canRetry, canAbort }`.
  - `hms: [{ code, level, text }]` and, while the printer reports one, `error: { code, text, actions }`; `actions`
    are the names `printerControl errorAction` takes.
  - `supports: { partSkip, airduct, chamberTemp, remoteDrying, printAllPlates, filamentTrackSwitch }`.
- RepRapFirmware hosts read progress, layers, remaining time and temperatures from the object model.
- `1008` for an unknown `printer`; `1009` when `statusReadable` is false.

**Two-stage, like `slicer.slice`.** A native dialog can stay open for minutes, longer than a call may
wait. So `exportGcode`, `print` and `printerControl` answer `{ accepted: true }` at once, and the outcome
arrives later as an event carrying the same request `id`.

`slicer.getInfo` reports `printerAccess` (true when this tab's printer permission is on), next to
`settingsAccess`, so a page can say "printer control is off, enable it in Preferences" instead of
meeting `1005`.

`printerAccessSetting` (feature `getInfo.printerAccessSetting`) says where that switch is, so the
directions a page gives match the build in front of the person: `label` is the switch's text and
`path` the menus, Preferences tab and section leading to it, both translated into the slicer's
language exactly as the slicer shows them. `path` is platform-specific: on Windows and Linux it
starts at the top bar's drop-down (`["Menu", "Preferences", "Developer", "Local web apps"]`); on
macOS at the application menu (`["Unlayered Slicer", "Settings…", "Developer", "Local web apps"]`,
or `"Preferences"` before macOS 13). It is `null` when this build has no such switch for this tab;
then say printer control is unavailable here rather than pointing at a setting. Build directions
from these fields; never hard-code them.

### `slicer.exportGcode`
`{ plate?: "current" | "all", format?: "gcode" | "3mf" }` → opens the slicer's own Save dialog, with
the slicer's usual file name and folder:
- `format: "gcode"` (default): the current plate's G-code (*Export G-code file*).
- `format: "3mf"`: a sliced `.gcode.3mf` of the current plate (*Export plate sliced file*), or with
  `plate: "all"` of every plate (*Export all sliced file*; every printable plate must be sliced,
  else `1002` with `data.unsliced`). `plate: "all"` exists only as a 3MF (`1002` otherwise).
- Answers `{ accepted: true }`. The outcome is the event `slicer.exportFinished`:
  `{ ok, format, plate, cancelled?, path? }`; an export the slicer refused because nothing on the
  plate(s) is printable reports `{ ok: false, reason: "nothing_printable", cause, message }` instead
  of `cancelled`.
- `1008` (`data.reason: "nothing_printable"`, `data.cause`) when the plate, or with `plate: "all"`
  every plate, has nothing printable (see `slicer.getPlate`).
- `1002` when there is no valid slice on the current plate (slice first), or for any other value
  of `plate` / `format` (`data.path`).
- The file is written by the slicer's normal export, post-processing scripts included.

### `slicer.print`
`{ printer, plate?: "current" | "all", options?, upload? }` → opens the slicer's own print dialog for that
printer, with everything that dialog offers (AMS mapping, the printer list, ...).
- `1008` (`data.reason: "nothing_printable"`, `data.cause`) when the plate, or with `plate: "all"` every plate,
  has nothing printable (see `slicer.getPlate`).
- `plate: "all"` is the Print all button: every plate must have a slice ready to print, a Bambu printer must
  support printing all plates, and of the print hosts only SimplyPrint takes it (`1009` otherwise).
- `options` (Bambu) presets the dialog: `timelapse`, `bedLeveling`, `flowCalibration`, `nozzleOffsetCalibration`,
  `sharedPaProfile` (`"on"` / `"off"` / `"auto"`), `jobName`, `timelapseStorage` (`"internal"` / `"external"`),
  `extChangeAssist` (bool). Each applies only where the dialog shows that option and offers that value for this
  printer (timelapse stays off where it cannot run). They hold for this print and are not saved as the
  person's defaults.
- `upload` (print hosts): `{ action?: "upload" | "print", path?, group?, storage? }`. `action` leaves only that
  button in the *Send to print host* dialog (`"print"` needs a host that can start a print); `path` is the
  folder on the host; `group` (Repetier) and `storage` (PrusaLink) apply when the dialog lists them. Not kept as
  the recent folder / group / storage. Flashforge's own dialog takes none of these.
- Answers `{ accepted: true }`. Nothing is sent unless the person presses the dialog's Print / Upload button.
- The outcome is the event `slicer.printSent`: `{ printer, plate, ok, cancelled? }`.
  - `cancelled: true`: the person closed the dialog.
  - `ok: true`: the job reached the printer (Bambu) or the host's upload queue accepted it (print hosts; the
    upload's own result is in `slicer.printHostJobs`).
- `1002` with no slice ready to print (or with all plates shown in the preview for `"current"`); `1007` while
  another print or send from this tab is in flight.

### `slicer.pressPrint`
`{}` → presses the Print button exactly as the person does, with whatever its dropdown is set to (Print plate,
Print all, Send, Export G-code, ...). The button's own window opens for the printer preset the person has
selected (print, send or save dialog) and nothing leaves until they confirm there, so unlike `slicer.print`
this needs only the settings permission and names no printer. Reply `{ accepted: true, button: "<label>" }`
once the window opens; event `slicer.printDialogClosed` `{ button, sent }` when it closes (`sent` true = a print
or upload job started). `1016` with `reason: "not_sliced"` while the button is disabled (slice first); `1007` while
a dialog is open, the slicer is slicing, or a print dialog from this tab is already open.

### `slicer.printerControl`
`{ printer, action, ...params }` → `{ accepted: true }`.
- The outcome is the event `slicer.printerControlDone`: `{ printer, action, ok, cancelled?, command?, error? }`.
- `pause` and `stop` send their event straight away.
- Every other action sends it after the person answers the confirmation, which shows the action, the
  Device tab's own warning where it has one (fan change while printing, light off with AI monitoring,
  heating mode, ...) and what will be sent. The printer is checked again after the confirmation.
- Refused before anything is shown where the Device tab refuses: `1016` with `data.reason` for a printer state
  (`offline`, `notHomed`, `nozzleCold`, `coolingMode`, `fanNotControllable`, `notPaused`, ...), `1007` while
  printing or changing filament, `1002` for values outside the printer's limits (`printerStatus.limits`),
  `1009` for what this printer does not have.

| `action` | params | Bambu | Duet/RRF |
|---|---|---|---|
| `pause` | - | pause (no confirm) | `M25` (no confirm) |
| `stop` | - | stop the print (no confirm) | `M25` then `M0` (no confirm) |
| `resume` | - | resume a paused print | `M24` |
| `setBedTemp` | `celsius` | bed target, up to the printer's bed limit | `M140 S` |
| `setNozzleTemp` | `celsius` (0 = off), `nozzle?` (0 right / only, 1 left) | nozzle target; the left nozzle by id | `G10 P<n> S` |
| `setChamberTemp` | `celsius` | chamber target, where the printer allows it; refused in cooling mode while printing | `M141 S` |
| `setFan` | `fan` (`part` / `aux` / `chamber`) or `fanId` (from `printerStatus.fans`), `percent` | in 10% steps; air-duct fan ids and the new protocol on H2-series printers; only fans the current mode leaves to the person | `M106 P0 S` (part fan) |
| `setAirductMode` | `mode` (`cooling` / `heating` / `exhaust` / `fullCooling`), `coolingFilter?` | fan mode; not changed while printing | - |
| `setSpeed` | `level` (1 silent - 4 ludicrous) | speed level, while printing | `M220 S50/100/124/166` |
| `setLight` | `on` | chamber light | - |
| `home` | - | home all; not while printing | `G28` |
| `jog` | `axis` (X/Y/Z/E), `mm`, `speed?` (RRF) | refused while printing, for an axis that is not homed, and for E below 170 C; Z at the Device tab's speed; negative Z raises the bed as the "up" button does | `G91` / `G1` / `G90` |
| `selectExtruder` | `extruder` (0 right, 1 left) | dual-nozzle printers; not while printing (paused is fine) or changing filament | - |
| `loadFilament` | `ams`, `slot` (255 / 254 = external spool), `extruder?` (Filament Track Switch only) | AMS load at the trays' midpoint temperatures | - |
| `unloadFilament` | `ams?`, `slot?` (default: what the current extruder holds) | AMS unload | - |
| `retryFilamentChange` / `abortFilamentChange` | - | Retry / Stop of a filament change or extruder switch (`printerStatus.filamentChange`) | - |
| `refreshRfid` | `ams`, `slot` | read the spool's tag; not while that filament is in the tool head | - |
| `dryAms` | `ams`, `celsius`, `hours` (1-24), `filament?` (preset name or filament id), `rotate?` | AMS 2 Pro (45-65 C) / AMS HT (45-85 C) drying, with the drying window's limits | - |
| `stopDryingAms` | `ams` | stop drying | - |
| `errorAction` | `code`, `action` (one of `printerStatus.error.actions`) | the printer error's button | - |
| `gcode` | `lines` (string, up to 20 lines) | raw G-code | raw G-code |

- `command` in the event is what was sent, as text. **The confirmation lists every line of a `gcode` action.**
- A printer that is not `controllable` answers `1009`.

### `slicer.sendToPrinter`
`{ printer, plate?: "current" | "all" }` → the *Send* dialog (Bambu printers and device connections), which saves
the sliced file to the printer's storage without printing. The person picks the storage and presses Send.
Answers `{ accepted: true }`; the outcome is `slicer.sendFinished { printer, plate, ok, cancelled? }`. Same
`1002` / `1007` rules as `slicer.print`.

### `slicer.selectPrinter`
`{ printer }` → `{ printer, selected: true, changed }`. Selects the Bambu printer the Device tab, AMS sync and
the print dialog follow; a LAN printer connects. Print hosts follow the printer preset instead (`1009`).

### `slicer.printHostJobs` / `slicer.cancelPrintHostJob`
- `slicer.printHostJobs {}` → `{ jobs: [{ id, state: "queued" | "uploading" | "error" | "cancelling" |
  "cancelled" | "completed", progress, file, error? }] }`: this session's uploads to print hosts (no host
  addresses).
- `slicer.cancelPrintHostJob { id }` → `{ accepted: true }`, then, after the person confirms,
  `slicer.printHostJobCancelled { id, ok, cancelled?, error? }`. `1016` for an upload that already ended.

## 4.7 The action catalog (0.2.0), behind the settings permission

Everything a person can do in the slicer that a page may ask for is described by one catalog. It
is **self-describing**: `slicer.listActions` with `detail: "full"` returns every action with its
parameter schema, effects and examples, so a page needs no other document. The design is in
[docs/HLSD/bridge-actions.md](docs/HLSD/bridge-actions.md) and the per-domain notes in
[docs/bridge-actions/](docs/bridge-actions/); `tools/gen_actions_reference.py` writes the full
reference from the catalog. Five methods, all behind `<slot>_allow_settings` and absent from
`capabilities` while it is off:

| Method | Params | Answer |
|---|---|---|
| `slicer.listActions` | `{domain?, query?, ids?, detail?: "summary"\|"full", includeDisabled?: true}` | `{catalogVersion, total, actions: [{id, domain, label, summary, risk, confirm, gate, longRunning, bridgeMethod, enabled, disabledReason, ...full: target, params, effects, timeoutMs, ui, examples, since}]}` |
| `slicer.invokeAction` | `{id, target?, params?, turn?, wait?: true, ifDirty?, ifProjectDirty?}` | `{id, status: "done", result, effects, undo: {snapshots, label}}`, or `{id, status: "running", jobId}` followed by `slicer.actionFinished` |
| `slicer.getSelection` | `{}` | `{objects, volumes, instances, plate}` with `{id, index, name}` refs |
| `slicer.getModelTree` | `{plate?}` (`-1`, the default, = every plate) | every object with its stable `id`, index, name, plate, printable, filament, per-object override keys, cut planes, layer ranges, instances (position, rotation, scale) and volumes (type, filament, overrides, painted layers, bbox), plus `treeVersion`, which changes whenever any of it does |
| `slicer.select` | a target `{objects?, volumes?, instances?, plate?}`; empty deselects | the new selection |
| `slicer.undoTurn` | `{turn}` | `{turn, undoneSnapshots, restoredSettings, skipped}`; `1013 {laterEdits: true}` when the person edited since |

- `params` are validated against the action's schema (a restricted JSON-Schema dialect); a failure
  is `1002` with `data.path` (and `data.needs` for a missing required property).
- An action needing a permission the tab lacks (`gate`: export, printer, install) is `1005` and is
  left out of `listActions`. A typed method's entry (`bridgeMethod`) is listed only while that
  method is in `capabilities`; invoke it directly.
- A disabled action answers `1016` with `data.reason`; a dialog, an open tool (gizmo, `data.gizmo`),
  slicing or a running job answers `1007`.
- Destructive and outward actions wait on a native confirmation the page cannot press (`1006` when
  declined). Model edits are undoable and never confirmed: each takes one undo snapshot named
  "<label> (automated)".
- `turn`: a grouping the page defines. Send the same opaque id (a string, at most 100 characters)
  in `params.turn` on every call that belongs together, typed methods included: for example
  everything one request from the person causes. The bridge records the undo snapshots and settings
  writes made under each turn, and `slicer.undoTurn {turn}` takes all of them back at once, as long
  as the person has not edited since (`1013 {laterEdits: true}` otherwise). Calls without `turn` are
  not grouped.
- Events (settings-gated): `slicer.actionFinished {jobId, id, ok, result|error, effects, undo, turn}`,
  `slicer.selectionChanged {objects, volumes, instances, plate}`, `slicer.actionsChanged {catalogVersion}`.
- `getInfo` adds `features: "actions.v1"` and `actions: {version, count}` (null without the
  permission).
- Removing filament slots: `filament.delete {slot, replaceWith?}` removes one, behind one native
  confirmation. To remove several, send ONE `filament.removeSlots {slots | keep, replaceWith?}` (0.1.8;
  `getInfo` feature `filament.removeSlots`, settings-gated like `filament.delete`): `slots` are the
  0-based slots to remove, or `keep` the ones to keep (every other slot goes). The person sees ONE
  confirmation listing them ("Remove 8 filament slots from the project?", a line per slot, 10 shown
  then "+N more"); on Remove the slots go highest index first through the same removal as
  `filament.delete`, so objects and painting fall back the same way. Declined: `1006`, nothing
  removed. `1002` with `data.path` for an index out of range or listed twice, nothing to remove,
  every slot or every physical slot, or a `replaceWith` that is itself removed. Pages that find no
  such feature fall back to one `filament.delete` per slot (one confirmation each). See
  [docs/bridge-actions/filament-ui.md](docs/bridge-actions/filament-ui.md).
- Preferences go through an allowlist (`prefs.get` / `prefs.set`, including the settings mode);
  the mixed-temperature restriction has its own confirmed action, `prefs.allowMixedTemperatures`.
  See [docs/bridge-actions/filament-ui.md](docs/bridge-actions/filament-ui.md).

---

## 4.8 Dialogs a page's request opened (0.2.0), behind the settings permission

Some calls end in one of the slicer's own modal dialogs: `addPrinter` / `selectPreset` with
`ifDirty:"prompt"` show *unsaved changes: Transfer / Discard / Save*, and an action can show a message
box. While a modal is open the frame is disabled and every call that needs it answers 1007; its message
now names the dialog: `a dialog is open: "Add printer: unsaved changes" (Transfer, Discard, Save); read it
with slicer.getDialogs and answer it with slicer.answerDialog`. Feature flag: `dialogs.v1`.

### `slicer.getDialogs` `{}`
`{"dialogs": [{id, title, openedBy: "bridge"|"user", texts: [...], buttons: [{label, enabled}],
checkboxes?, fields?, answerable, reason?}]}`, outermost first; the last is the one in front.

### `slicer.answerDialog` `{dialog?, button}`
Presses the button with that label (case and a trailing `...`/`:` ignored) or, with `"close"`, closes
the dialog as its title bar's X does (cancel). `dialog` defaults to the one in front, and only the one in
front can be answered (1007 otherwise). The response (`{dialog, pressed}`) comes first; the press runs
right after it, so a dialog the press opens (Save -> name the preset) is reported as `dialogOpened` and
can be answered in turn. The original call's own response arrives when its dialog closes.

Refused with 1005 -- the person answers these, the page only reads them:
- a dialog the **person** opened (`openedBy: "user"`);
- the person's **own confirmation**: every native confirmation, the Save file dialog, the print and send
  dialogs, the printer-profile install review. They are the consent the export, printer and install
  families rest on, so no page can press them, whoever asked for them;
- any dialog kind outside the allow-list: unsaved changes, save preset, and the slicer's message boxes.
  Login, plugin, preference and print-host dialogs are not on it.

Events: `slicer.dialogOpened {dialog}` and `slicer.dialogClosed {id, result, by: "bridge"|"user"}`.
Every press is logged. (By design: a page must be able to resolve the dialogs its own
calls raise, without a person at the slicer.)

## 5. Error codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `1000` | Malformed request — unparseable JSON, or no method |
| `1001` | Unknown method |
| `1002` | Bad parameters |
| `1003` | Forbidden — page origin, download host, or file path not permitted |
| `1004` | Missing or stale session token |
| `1005` | Capability disabled for this origin or configuration |
| `1006` | Cancelled by the user |
| `1007` | Busy — the slicer is slicing, or another download is in flight |
| `1008` | Not found |
| `1009` | Unsupported file type |
| `1010` | I/O or network failure |
| `1011` | Too large |
| `1012` | Internal error |
| `1013` | Needs confirmation — the call would discard unsaved preset edits. `data` is `{ dirty: true, type, dirtyKeys, dirtyTypes }` (`needsName: true` when `"save"` met a system preset); retry with `ifDirty: "discard"`, `"save"`, `"transfer"` or `"prompt"` (§4.1) |
| `1014` | Service mode selected: `slicer.slice` while a slicing-service mode is selected. Only the person starts a service run ("No service run from a page"). Not an entitlement check. `data` is `{ reason: "service_mode_selected", selected }` |
| `1015` | **Retired, reserved.** Earlier builds used it for an account check; the slicer makes no entitlement checks and never returns it. A page that offers paid features shows its own upgrade UI. |
| `1016` | Action disabled — the catalog action exists but cannot run right now (nothing selected, nothing to undo ...). `data.reason` says why (§4.7) |

Plus two produced by the client shim rather than the slicer: `'no-bridge'` and `'timeout'`.

---

## 6. Security model

Five independent layers. Understand them before reporting that something "doesn't work" — most of
the time it is one of these, working.

**Origin allow-list.** Bridge messages are honoured only when the loaded page's origin exactly
equals the configured one, compared as normalised `scheme://host[:port]` strings. Not a prefix
match, not a substring match. On any other origin the shim is not injected at all and every request
returns `1003`.

**Per-document session token.** A fresh 256-bit token is minted on each successful load of a
trusted origin and handed to the page in `window.__slicerBridge.token`. Every request must carry
it. It is invalidated by any navigation or reload, so it cannot be replayed, and a cross-origin
frame that can reach `window.wx` cannot read the main frame's JavaScript to steal it. Requests
without it return `1004`.

**Transport.** `https` is accepted anywhere. Plain `http` is accepted **only** for loopback hosts
(`127.0.0.0/8`, `::1`, `localhost`), which browsers treat as trustworthy origins. An `http://`
address on the network is refused before any connection is attempted, and there is deliberately no
override. Certificate errors are fatal — there is no click-through.

**Navigation containment.** The tab may navigate only within the configured origin, plus any
https origin listed in `web_app_extra_origins`. Everything else is vetoed and handed to the system
browser, as are all pop-ups. This is what stops the trusted origin drifting out from under the
checks above.

**Navigable is not trusted.** `web_app_extra_origins` exists so an application can send the user to
a vendor page, a documentation site or an identity provider without ejecting them from the window.
It widens *navigation only*. The bridge shim is injected solely when the origin allow-list above
matches exactly, so a page reached this way has no `window.__slicerBridge`, no session token, and
every `slicer.*` call it attempts returns `1003`. Keep the two lists straight when reading the
code: `is_navigable()` consults both, `origin_is_allowed()` never does.

Because the tab has no address bar, a bar appears across the top whenever the user is on one of
these origins. It names the site, offers a way back to the application, and offers to reopen the
current page in the user's real browser. Do not remove it: without it the user cannot tell whose
site they are on — which matters most on a page asking for a password or a card number — and has
no way back.

**Capability gating.** `slicer.openProject` requires a loopback origin or an explicit user opt-in,
and is absent from `capabilities` when unavailable. Downloads are same-host by default. The
preset-settings family (§4.1) requires its own per-slot opt-in, is absent from `capabilities`
without it, and is filtered by deny lists that no configuration widens. Each tab is governed by
its own settings (`web_app_*`, `side_panel_*`, `universe_app_*`): a page in one slot cannot see or
use another's.

### Deliberately absent

There is no way to read or write arbitrary files, run a process, open an arbitrary path in the
shell, read or modify the slicer's own configuration (AppConfig — print hosts, download folders,
the tab URLs themselves), reach anything to do with accounts, tokens or the vendor network plug-in,
write custom G-code or `post_process` scripts into a preset, or read a print host's address or
credentials. This is not an oversight and these are not on a roadmap. The list, and the reasoning
for each item — including why preset *settings* are exposed and exactly how narrowly — is kept in
the header comment of [`src/slic3r/GUI/WebAppBridge.hpp`](src/slic3r/GUI/WebAppBridge.hpp).

Installing a printer profile (§4.4) is the one way a page can add a preset, and the one way custom
G-code it supplies can reach the slicer — so it is not the page's decision. The user sees the
profile, and every custom-G-code line that touches firmware, files, the network or safety limits,
in a native dialog, and nothing happens unless they agree; print-host settings and local paths are
stripped from the profile on export and again on install, so a shared profile can never redirect
the installer's print host.

---

## 7. Configuration

### Slots

A **slot** is one place a page can be hosted. Each slot has its own address and its own
permissions, and a page in one slot cannot see or use another slot's settings. There are three:

| Slot (`tab.slot`) | Hosted as | Keys |
|---|---|---|
| `web_app` | a top-level **web-app tab** | `web_app_*` |
| `side_panel` | the **side panel**: a narrow panel beside the notebook, visible in every tab, toggled from the tab bar, the View menu or Ctrl+Shift+B, and resizable by its sash (`tab.layout: "sidebar"`) | `side_panel_*` |
| `universe_app` | a top-level **model-library tab** that opens and publishes models | `universe_app_*` |

**The web-app tab and the side panel are general-purpose.** The user points each at any address in
**Preferences ▸ Advanced ▸ Web apps & services**, which also holds the slicing service URL, the
account provider URL and the service token origins. A blank address turns the slot off. The page
warns that pages at these addresses can read the plate and change settings within their
permissions, and to enter only addresses you trust. A newly set address for a slot that had none
appears after a restart; a changed one reloads at once. The model-library tab has the same keys;
its address is set in the `.conf`.

### Keys

Each slot has the keys below, with its own prefix (`web_app_`, `side_panel_`, `universe_app_`):

| Key | Default | Meaning |
|---|---|---|
| `<slot>_enabled` | `true` | Shows the slot when it also has an address. |
| `<slot>_url` | empty, or a seeded default | The address to load. Blank means the slot is not shown. |
| `<slot>_title` | seeded | The label of the tab or panel. |
| `<slot>_allow_local_files` | `false` | Permit `slicer.openProject` from a non-loopback origin. |
| `<slot>_path_roots` | empty | `;`-separated roots that `slicer.openProject` may read from. Empty means the conservative default in §4. |
| `<slot>_download_hosts` | empty | `;`-separated extra hosts that `slicer.openProjectFromUrl` and `installPrinterPreset` may fetch from. |
| `<slot>_extra_origins` | empty | `;`-separated extra **https** origins the slot may navigate to (§6). |
| `<slot>_allow_settings` | `false` | Open the settings family (§4.1), the action catalog (§4.7) and `requestHandoff` to this slot. |
| `<slot>_allow_export` | `false` | Open the export family (§4.2) and `exportPrinterPreset` (§4.4). |
| `<slot>_allow_install` | `false` | Let the page offer printer profiles with `slicer.installPrinterPreset` (§4.4). Each install still waits for the user to confirm in a native dialog. |
| `<slot>_allow_printer` | `false` | Open the printer family (§4.6). |
| `side_panel_sidebar_open` | `false` | Whether the side panel is expanded. |
| `side_panel_sidebar_width` | `0` | Its width in DIP. `0` means the default of 380. |

**Defaults.** A build may seed a default address, title and permissions for a slot. Seeding writes
a key **only when it is absent**. A value the user set, including a blank address, is never
overwritten by a later version. A customer build seeds no address for the web-app tab or the side
panel. It seeds the model-library tab with Unlayered's model library (with the export and install
permissions); that is an ordinary value the user can change or clear, and the slicer treats the
page there exactly like any other page.

**Migration.** Builds before 0.2.0 stored the side panel's settings under a different key prefix.
On the first start, each old value is copied to the matching `side_panel_*` key if that key is
absent, and the old keys are then removed.

The **model-library tab** needs `universe_app_download_hosts` when its site answers a download with a
redirect to a short-lived signed URL on a **separate storage host**. `slicer.openProjectFromUrl`
allows only the page's own host plus the hosts listed there, so without it every "open in the
slicer" is refused with `1003`. A site that serves its own files needs nothing.

Development overrides are read from the environment and never written back to the configuration:
`ORCA_WEB_APP_URL`, `ORCA_WEB_APP_TITLE`, `ORCA_SIDE_PANEL_URL`, `ORCA_SIDE_PANEL_TITLE`,
`ORCA_UNIVERSE_APP_URL`, `ORCA_UNIVERSE_APP_TITLE` and `ORCA_WEB_APP_DEVTOOLS=1`. A `*_URL` override shows that slot.

Developer tools are available in the tab when Preferences ▸ Debug ▸ developer mode is on, or
`ORCA_WEB_APP_DEVTOOLS=1` is set: right-click ▸ Inspect.

---

## 8. Worked example

```js
async function handOff(downloadUrl, name, sha256) {
  const bridge = window.__slicerBridge;
  if (!bridge) return false;                       // ordinary browser: leave the link alone

  const info = await bridge.send("slicer.getInfo");
  if (!info.capabilities.includes("slicer.openProjectFromUrl")) return false;

  const done = new Promise(resolve => {
    const onOpened = data => { bridge.off("slicer.projectOpened", onOpened); resolve(data); };
    bridge.on("slicer.projectOpened", onOpened);
  });

  bridge.on("slicer.openProjectFromUrl.progress",
            p => setStatus(p.total ? `${Math.round(100 * p.received / p.total)}%` : "downloading…"));

  try {
    await bridge.send("slicer.openProjectFromUrl", { url: downloadUrl, filename: name, sha256 });
  } catch (e) {
    setStatus(`Could not hand over: ${e.message}`);  // the download link stays usable
    return false;
  }

  const result = await done;
  setStatus(result.ok ? "Opened in the slicer" : `Not opened: ${result.error}`);
  return result.ok;
}
```

Two habits worth copying from it: render your own download link **first** and treat the hand-off as
an enhancement on top, and never let a bridge failure block the user from their file.

---

## 9. Versioning

`api.version` is `1`. Within version 1, methods and fields may be **added**; nothing already
specified will change meaning or be removed. Check `capabilities` rather than `app.version`.

An incompatible change would ship as `api.version: 2`, with version 1 still answered for a
transition period.

---

## 10. Implementation

| File | Role |
|---|---|
| [`src/slic3r/GUI/WebAppSlot.hpp`](src/slic3r/GUI/WebAppSlot.hpp) | Every slot: its keys, environment overrides, seeded defaults and seed file |
| [`src/slic3r/GUI/WebAppBridgeActions.cpp`](src/slic3r/GUI/WebAppBridgeActions.cpp), [`src/slic3r/GUI/Actions/`](src/slic3r/GUI/Actions/) | The action catalog (§4.7): `listActions`, `invokeAction`, selection, turns and `undoTurn` |
| [`src/slic3r/GUI/WebAppPanel.cpp`](src/slic3r/GUI/WebAppPanel.cpp) | The tab: configuration, navigation, origin tracking, fallback page, event fan-out (`notify_all`) |
| [`src/slic3r/GUI/WebAppBridge.cpp`](src/slic3r/GUI/WebAppBridge.cpp) | The protocol: envelope, token, dispatch, every method, `notify` |
| [`src/slic3r/GUI/WebAppSettings.cpp`](src/slic3r/GUI/WebAppSettings.cpp) | The settings policy: deny lists, readable/writable predicates, schema and slice-summary JSON |
| [`src/slic3r/GUI/WebAppPresetShare.cpp`](src/slic3r/GUI/WebAppPresetShare.cpp) | Printer-profile sharing (§4.4): what is stripped, the risky M-code table and scanner, the install dialog, export and import |
| [`docs/HLSD/web-app-bridge.md`](docs/HLSD/web-app-bridge.md) | High-level design: slots, gate, deny lists, write path, event hooks |
| [`resources/web/webapp/unavailable.html`](resources/web/webapp/unavailable.html) | Shown when the application cannot be reached |
| [`resources/web/webapp/*.json`](resources/web/webapp/) | Development overlay on the slot defaults; not shipped in the installer |
| [`resources/web/webapp-example/`](resources/web/webapp-example/) | MIT-licensed reference client; exercises every method and event |
| [`SLICE_SERVICE_PROTOCOL.md`](SLICE_SERVICE_PROTOCOL.md), [`ACCOUNT_PROVIDER_PROTOCOL.md`](ACCOUNT_PROVIDER_PROTOCOL.md) | The two other documented interfaces |
