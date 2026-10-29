# Web App Bridge — High Level Design

## Why it exists

Unlayered Slicer hosts web applications either as top-level tabs or as a persistent **side panel**
beside those tabs. Each is a browser control pointed at a URL the user configures, the same way the
Device tab embeds a printer's own web interface. A side panel keeps a page and the plate it works on
visible together. A hosted page needs a few things only the slicer can do: open the file it
produced, put the application on the right printer, and, for a page the user has trusted with
settings, read and change the values in the Print / Filament / Printer tabs and start a slice.

The bridge is the whole of what such a page can ask for: a small JSON-RPC surface over the web
view's `window.wx.postMessage` channel, specified in `WEB_APP_BRIDGE.md` at the repository root
and exercised by the MIT-licensed reference client in `resources/web/webapp-example/`. Everything
about it is shaped by one fact: the page is a **separate program**, possibly on a remote server,
possibly not written by us. The slicer is AGPL-3.0; the page is whatever its author likes. The
interface therefore exposes capabilities, never internals, and every widening of it is argued in
the header comment of `src/slic3r/GUI/WebAppBridge.hpp`.

## The slots

There is one panel class, `WebAppPanel`, and one bridge class, `WebAppBridge`, instantiated once
per **slot**. A slot (`WebAppSlot`, a struct of `const char*`) is the row of AppConfig keys,
environment overrides and seed file a panel is configured from. Three exist:

| Slot | Hosted as | Keys | Theme toggle |
|---|---|---|---|
| `generic()` (`web_app`) | the web-app tab | `web_app_*` | reloads the page, like every other web view |
| `side_panel()` (`side_panel`) | the **side panel** beside the notebook (collapsed by default, Ctrl+Shift+B) | `side_panel_*` | re-themes in place through `slicer.themeChanged`, because the page may hold state the user would lose |
| `universe()` (`universe_app`) | the model-library tab | `universe_app_*` | reloads; browsing state is cheap to rebuild |

Every slot's address is user configuration: Preferences ▸ Advanced ▸ Web apps & services has the
side panel's and the web-app tab's (plus the slicing service, the account provider and the service
token origins), the model-library tab's is a `.conf` key, and a blank address turns a slot off. Each permission is off by default. A build may carry default values for a slot (its
`default_url`, `default_title` and permission fields); `ensure_seeded` writes a default **only when
the key is absent**, so a user's value, including a blank one, survives every update. The seed files
in `resources/web/webapp/*.json` are a development overlay where they exist; they are not shipped.

`MainFrame::web_app_slot_view` is the one place that maps a slot to its panel and notebook page
(null for the side panel, which is not a page), so a fourth slot is an entry there rather than
another branch in each caller. The side panel is a child of the frame laid out in a row with the
notebook — `[notebook | sash | side panel]` — and `WebAppPanel::hosted_as_sidebar()` is how the
bridge knows to report `tab.layout: "sidebar"`. The side panel is never handed the plate by the
slicer; its page pulls it with `slicer.requestHandoff` when it needs it. Every reply carries a
`token` naming the plate and settings it describes; a page that passes the token of the plate it
last uploaded as `ifChangedSince` gets `{unchanged: true}` and no export when nothing changed, and
`probe: true` only answers `{unchanged, token}` (never exports). A plain request exports as before.

The model-library tab embeds a model-sharing site so that opening a print puts it on the plate
instead of in the Downloads folder. The page asks its own API for a signed URL and passes that to
`slicer.openProjectFromUrl`; the slicer fetches the file itself. Two constraints shape that. The
slicer downloads with its own HTTP client, which carries none of the web view's cookies, so a
cookie-authenticated download route would answer 401 — the URL handed over has to be one that
stands on its own. And `openProjectFromUrl` allows the page's own host automatically but nothing
else, so a site that redirects downloads to a separate storage host must name it in
`universe_app_download_hosts` or every open is refused with `1003`.

The publish direction is the mirror of that. `slicer.exportProject` writes the open project into
the slicer's cache and returns an opaque handle; the page pulls the bytes with
`slicer.readExportChunk` and drops the assembled file into the ordinary upload form. It sits
behind its own `<slot>_allow_export` gate rather than reusing `allow_settings`, because consenting
to a page reading print settings is not the same as consenting to the model leaving the machine.

Nothing in the slicer's source names a particular application: the URL, the title and every gate
are configuration, and the seed files are data. Unlayered's own default addresses are values in that
configuration, treated exactly like an address the user typed. The bridge reads *its own* slot's keys
(`owner.slot().key_allow_local`, `key_path_roots`, `key_download_hosts`, `key_allow_settings`) so
each tab is governed independently; a page in one tab cannot learn or exploit another's
settings. `slicer.getInfo` tells a page which slot it is in (`tab.slot`) so one application can be
served to any of them.

## Trust, in layers

Requests are honoured only when the loaded document's origin **exactly equals** the configured
one; the origin is checked again before every dispatch. A fresh 256-bit **session token** is minted
on each load of a trusted origin, injected into the page as `window.__slicerBridge.token`, and
required on every request — a navigation or reload invalidates it, and a cross-origin frame that
can reach `window.wx` cannot read it. `https` is accepted anywhere, `http` only on loopback; the
tab may navigate only within the configured origin plus listed extra https origins, and those are
**navigable, not trusted** — no token is injected there.

Above that sit **capabilities**: the method list `slicer.getInfo` returns is computed at call time
from the origin and the user's settings, and it is the only correct way for a page to decide what
it may call. A method that is gated off answers `1005` whether or not the page believed it was
available.

## The gate on print settings

Reading and changing preset values is the one place the bridge reaches into the user's own data,
so it is behind an explicit, per-slot, default-off opt-in, the `<slot>_allow_settings` key. With
every one off the
slicer behaves byte-for-byte as it did before the settings family existed: the capability list is
the original three (four with local files), the events of §"Events" are never sent, and the
printer form of `slicer.selectPreset` keeps its v1 behaviour including its dialog. Toggling the
preference fires `slicer.capabilitiesChanged` so an open page re-reads what it may do.

Behind the gate are `listPresets`, the vendor-catalog methods, `getSettingsSchema`,
`getSettings`, `setSettings`, `getPlate`, the plate edits, `slice`, `getSliceResult`,
`getSliceIssues` (why a plate will not slice: validation, slicing warnings, G-code checks),
`getSliceModes` / `setSliceMode`, `requestHandoff`, the action catalog and the print/filament forms
of `selectPreset`. Each begins with the same **preflight**: the gate; "is a modal dialog open"
(`MainFrame::IsEnabled()` is false under one — a heuristic that keeps a bridge call from nesting a
second modal, not a security check); for writers, "is the slicer slicing"; and "does the tab for
this preset type exist".

## What is never exposed: the deny lists

`src/slic3r/GUI/WebAppSettings.cpp` holds three lists that no configuration widens:

- **Secret** — `print_host`, `print_host_webui`, `printhost_apikey`, `printhost_cafile`,
  `printhost_user`, `printhost_password`, `printhost_port` (the seven
  `PresetBundle::full_config_secure()` erases before a config leaves the process) plus
  `printhost_ssl_ignore_revoke`, `printhost_authorization_type`, `bbl_use_printhost`, `host_type`.
  Never read, never written.
- **Code** — the sixteen custom-G-code options of `PrintConfig.cpp` and `post_process`: text the
  slicer emits into a print or runs as a command. Never written; read only when a call says
  `includeCode: true`.
- **Identity** — `inherits`, `printer_model`, `printer_variant`, `printer_technology`, the four
  `compatible_*` keys, `filament_vendor`, `filament_id`, and every `*_settings_id`: what a preset
  *is* rather than what it does. Readable, never written — changing one would turn the edited
  preset into a different preset, which is what `selectPreset` is for.

Two predicates from the option definition complete the write filter: `readonly` options and
plugin-backed options (`ConfigOptionDef::is_plugin_backed()`) are never written either. An
unknown key is `1008`; a denied key is `1003`, named in `rejected[]` with the reason.

Values cross the bridge as the **`.ini` serialisation of a named key** — `"0.2"`, `"35%"`, `"1"`
— never as a `DynamicPrintConfig` dump. A page asks for keys and gets strings; the schema method
tells it the type, range, enum values and labels to make sense of them. This is both the smaller
interface and the one that keeps the two programs two programs.

## The write path

`setSettings` never touches a preset on disk and never bypasses the tab. It copies the tab's
edited config, deserialises each accepted value into the copy with
`ConfigBase::set_deserialize` under the `Disable` substitution rule (so a value the slicer cannot
parse is an error, not a silent default; `handle_legacy` renames still apply), range-checks
numerics with `ConfigOptionDef::is_value_valid`, restores any key that failed, and hands the copy
to **`Tab::load_config()`** — the entry the cut-plane gizmo already uses. That runs
`update_dirty → reload_config → update`, so the result is indistinguishable from the user typing
the value: the tab shows "(modified)", the sidebar's frequently-changed fields follow, the plater
and background process receive the new config, and the user can undo or save as usual.

`Tab::load_config` does not run the couplings `Tab::on_value_change` maintains for GUI field
handlers, so the bridge calls it afterwards for exactly the keys that have one —
`pellet_flow_coefficient` / `filament_diameter` (each rewrites the other; `boost::any<double>`),
the prime-tower / single-extruder-multi-material / purge trio (`boost::any<bool>`) and
`gcode_flavor` (empty `boost::any`) — with the `boost::any` type each handler casts, because a
wrong type there throws.

Replacing the edited preset (`selectPreset` print/filament, `setSettings` on another slot's
filament) is the one operation that can destroy the user's unsaved work, so it is under the
**`ifDirty` contract**: the default `"ask"` answers `1013` with `{dirty, type, dirtyKeys}` and
changes nothing, `"discard"` drops the edits and proceeds, `"prompt"` leaves the slicer's own
"transfer or discard?" dialog to `Tab::select_preset`. A page cannot see the user's edits before it
asks, so the safe default is to be told.

Filament slots have no public selection API of their own in the sidebar; the sequence the sidebar
combo runs (`Plater::priv::on_select_preset`) is extracted into
`Plater::priv::apply_filament_selection`, which the combo and the public
`Plater::select_filament_preset(idx, name)` share. The bridge calls the latter with no combo, so the
`editing_filament` latch `Sidebar::update_presets` relies on never comes into play, and a
multi-filament printer changes exactly the slot asked for.

## Events

The slicer pushes unsolicited events through `WebAppPanel::notify_all`, which fans out to every
live panel's `WebAppBridge::notify`. `notify` is a no-op until the page holds a session token and
drops the settings-family events unless the tab's gate is on. The hooks are where the slicer
already knows the fact, not where the bridge guesses it:

| Event | Hook |
|---|---|
| `presetChanged` | end of `Tab::on_presets_changed`; `apply_filament_selection` when the filament tab did not run (multi-filament slot change) |
| `settingsChanged {source:"user"}` | `Tab::on_value_change`, suppressed while a bridge write is in flight (`WebAppBridgeWriteScope`) so a `setSettings` is reported once, as `source:"bridge"` |
| `plateChanged` | `Plater::priv::object_list_changed`, coalesced onto the next event-loop turn with a pending flag, because it fires several times per user action |
| `sliceStarted` / `sliceFinished` | `Plater::priv::on_slicing_began` / `on_process_completed` after `m_is_slicing` clears, with the slice summary when the result is valid |
| `themeChanged` | the side panel's `EVT_WEBVIEW_RECREATED` handler, instead of a reload |
| `capabilitiesChanged` | Preferences, after a bridge-relevant toggle |
| `projectChanged` | end of `Plater::new_project` / `Plater::load_project` (not for a G-code or `.gcode.3mf` preview), via `WebAppPanel::project_changed`, which also bumps the `projectGeneration` getInfo reports. The slicer's internal uses of the two -- the project set aside for a preview and restored, `load_gcode`, the crash/rebuild restore, leaving a sliced-file preview -- run under a `QuietProjectChange`. `source:"bridge"` when the path matches the one `open_resolved_file` noted with `Plater::note_bridge_project_open` |

An application built on these must not read its own `source:"bridge"` echo as user intent; the
field exists so an automated loop cannot argue with itself.

## Relationship with a hosted application

A hosted application is a separate program. Its server never runs inside the slicer and the slicer
never starts it: a slot is a URL, and `resources/web/webapp/unavailable.html` is what the user sees
when nothing answers. A typical integration maps its own operations onto `slicer.*` bridge methods
in the page, waits for the response (and for `sliceStarted` / `sliceFinished` when it asked for a
slice), and reports the result to its server. It offers an operation only when the page saw the
method in `capabilities`, so a user who has not opted in sees an application that cannot touch
their settings, and a plain browser with no bridge sees the application's own fallback. The bridge
knows none of this; it answers the same documented methods for any page in any slot, and
`resources/web/webapp-example/` (MIT) exercises every one of them.

Entitlement is not the bridge's business either. The slicer reports `account: {signedIn, provider}`
and nothing about what an account may use; a page that sells features checks that on its own
server and shows its own upgrade UI.

## Constraints that shape it

- **Gates off means unchanged.** With every `*_allow_settings` false the slicer's observable
  behaviour is that of the release before the settings family existed.
- **No AppConfig, no files, no processes, no accounts, no tokens, no vendor network plug-in, no
  G-code writes, no print-host keys** — the standing list in `WebAppBridge.hpp`, and the reason the
  settings family is a fixed set of typed methods rather than a generic get/set.
- **GUI thread.** Every handler runs synchronously inside the script-message event; the only
  worker thread is the download, which touches the bridge state through a weak pointer and hops
  back via `CallAfter`. Events are built on the GUI thread and delivered the same way.
- **Additive within protocol v1.** Methods, fields and events are added; nothing specified changes
  meaning. A page checks `capabilities`, not `app.version`.
