# Slicing service protocol, version 2

Unlayered Slicer can send the current plate to an **external slicing service** over HTTP and use
the G-code that comes back as that plate's slice result. This document specifies the whole
interface. A reference service is in
[`tools/reference-slice-service/`](tools/reference-slice-service/). It is MIT-licensed, uses only
the Python standard library, and implements every endpoint below.

Some slicing strategies are not layer-by-layer plane sweeps, and this program does not implement
them. A volumetric or five-axis engine, for example, makes toolpaths that tilt out of the XY plane.
Instead of embedding such an engine, the slicer can ask a service for the result, the same way it
asks a print host to accept a file.

**Any service at any address may implement this protocol.** The slicer has no vendor allow-list,
no registration and no key. The address is a setting, and the slicer's source names no particular
service. With no address set, the feature does not appear in the UI. The service decides who may
use it: a service may refuse any request (see [Errors and refusals](#refusals)), and the slicer
itself makes no entitlement check.

How this fits with the other documented interfaces, and what the slicer does and does not ship, is
summarised in [BOUNDARY.md](BOUNDARY.md).

---

## 1. Relationship between the two programs

Unlayered Slicer is licensed under the **GNU Affero General Public License, version 3**. A slicing
service it talks to over this interface is a **separate program**. It runs in its own process,
usually on its own machine, and can be written and licensed however its author likes. The only
things the two programs exchange are a 3MF file and JSON over this documented protocol. The slicer
has the same relationship with the print hosts it uploads to (Duet, OctoPrint and the rest), and
nothing here changes that.

If you run **this slicer** on a server to make output for remote users, AGPL-3.0 §13 asks you to
offer those users its Corresponding Source. This interface does not create that obligation, because
the slicer runs on the user's own machine and the service does the slicing.

---

## 2. Configuration

| Setting | Where | Meaning |
|---|---|---|
| Slicing service URL (`nonplanar_slice_url`) | Preferences ▸ Advanced ▸ Web apps & services | Service root, e.g. `https://slicing.example.com/api` or `http://localhost:8000`. Blank turns the feature off. |
| Account provider URL (`account_provider_url`) | same | Where the slicer signs in. See [ACCOUNT_PROVIDER_PROTOCOL.md](ACCOUNT_PROVIDER_PROTOCOL.md). Blank means no account. |
| Service token origins (`service_token_origins`) | same | A comma-separated list of extra service origins that may receive a provider token. See §3. |

`ORCA_NONPLANAR_SLICE_URL` overrides the URL for development. It is never written back.

**Transport.** `https` is accepted for any host. Plain `http` is accepted **only** for loopback
hosts (`127.0.0.0/8`, `::1`, `localhost`), the same rule the web-app tab uses. The slicer refuses an
`http://` address on the network before it connects, and there is no override. A trailing slash on
the root is ignored. Every path below is relative to the root.

---

## 3. Authentication

The slicer can attach a short-lived bearer token from the configured account provider
(`POST /api/slicer/slice-token`, see ACCOUNT_PROVIDER_PROTOCOL.md §5) to every request:
`Authorization: Bearer <token>`. It does so **only** when one of these is true:

- the service origin is **same-site** as the account provider: the same scheme and the same
  registrable domain (`https://slicing.example.com` and `https://example.com` are same-site), **or**
- the service origin is listed in `service_token_origins`.

The slicer approximates the registrable domain by the host's last two labels, or the last three
when the top-level domain has two letters and the second level is a generic one (`co`, `com`,
`net`, `org`, `gov`, `ac`, `edu`, `ne`, `or`, `go`, as in `example.co.uk`). An IP address must
match exactly. If your service is on a different site from your provider, list it in
`service_token_origins`. With `account_provider_url` blank there is no account and no token.

Otherwise the requests carry no `Authorization` header, and the service handles authentication
itself. A provider token is never sent to an origin the user has not tied to that provider.

The token's `audience` is the service origin. During a long job the slicer asks the provider for a
new token every 10 minutes, or a minute before the token's stated expiry if that is sooner (but no
more often than every 30 s). A service should accept any valid
token for its audience on every endpoint of one job.

A service that needs no authentication can ignore all of this.

---

## 4. Flow

```
GET  <root>/                     optional: name, modes, protocol version
GET  <root>/ui-schema            optional: the service's options form
POST <root>/prepare              optional: warm a cache for this geometry (no G-code)
POST <root>/slice                multipart: the plate + options          -> 202 {job_id}
GET  <root>/jobs/{id}            poll until status is succeeded or failed
POST <root>/jobs/{id}/cancel     optional: the user stopped the job
POST <root>/jobs/{id}/grant      optional: a one-use download path
GET  <root>/jobs/{id}/gcode      the result (or the path the grant named)
```

The slicer polls. There is no callback, so the service never needs inbound access to the user's
machine. The slicer polls every 1.5 s and gives up after one hour.

### `GET <root>/` (discovery, optional)

```json
{ "display_name": "Example Slicing", "protocol": 2,
  "modes": [ { "id": "planar", "label": "Planar (service)" },
             { "id": "fine", "label": "Fine walls", "options": { "wall_pattern": "concentric" } },
             { "id": "tilted", "label": "Tilted layers", "show_if": { "machine_axes": 5 } } ] }
```

| Field | Notes |
|---|---|
| `display_name` | The service's name. The slicer uses it for the options dialog title ("<name> options") and the Slice dropdown. The fallback order is `display_name`, then the ui-schema `title`, then "Slice with service". |
| `protocol` | `2` for this document. |
| `modes[]` | One Slice-dropdown entry per mode offered on this machine, labelled `label`. The selected mode's `id` is sent as the `mode` form field with `/slice` and `/prepare`. |
| `modes[].show_if` | Optional. **Machine facts only** (today `machine_axes`, see below). A mode whose condition fails is not offered. |
| `modes[].options` | Optional. `{dest: value}` field values the mode fixes. They are hidden in the options dialog and always sent in `options` with that mode, whatever the user saved. |

The default mode is the **first one offered**. The selected mode is remembered per printer preset;
if it is no longer offered, the first one is used. The slicer reads `GET /` when the Slice dropdown
opens, refreshes it in the background and caches it. If the service is unreachable, the cached
copy is used. Without discovery, or with no mode offered, the dropdown shows one entry with the
fallback name, and no `mode` is sent.

### `GET <root>/ui-schema` (optional)

This describes the options dialog ("<name> options…" in the Slice dropdown). The slicer knows only
the vocabulary below. The service supplies all names, labels, help texts and defaults.

```json
{ "title": "Example Slicing", "version": 1, "fields": [
  { "dest": "pattern", "label": "Wall pattern", "help": "…", "type": "enum",
    "choices": [ { "value": "concentric", "label": "Concentric" },
                 { "value": "tilted", "label": "Tilted", "show_if": { "machine_axes": 5 } } ],
    "default_by": { "machine_axes": { "3": "concentric", "5": "tilted" } } },
  { "dest": "max_tilt", "label": "Maximum tilt", "type": "number", "unit": "°",
    "min": 0, "max": 60, "step": 1, "default": 30,
    "show_if": { "machine_axes": 5, "pattern": ["tilted"] } },
  { "dest": "seam_hiding", "label": "Hide seams", "type": "bool", "default": true },
  { "dest": "spacing", "type": "number", "optional": true, "default": null }
] }
```

| Key | Meaning |
|---|---|
| `title` | A fallback display name when there is no `GET /`. |
| `fields[].dest` | The key the value is sent under in `options`. |
| `fields[].type` | `enum`, `bool` or `number`. The slicer skips a field of any other type, and the service default applies to it. |
| `label`, `help`, `unit` | Display text. `help` becomes the tooltip. |
| `choices[]` | For `enum`: `{value, label, show_if?}`. A choice whose `show_if` fails is not offered. |
| `min`, `max`, `step` | For `number`. The slicer refuses out-of-range input and never sends it. |
| `optional` | For `number`: a blank box sends nothing. |
| `default` | The service's default. |
| `default_by` | A default that depends on a machine fact. Today the only fact is `machine_axes`: `{"machine_axes": {"3": v, "5": v}}`. A missing key falls back to `default`. |
| `show_if` | `{ "<name>": value \| [values], … }`. Every listed condition must hold for the field to be shown and sent. A name is another field's `dest` (its current value in the form, or the value the selected mode fixes) or a machine fact (`machine_axes`). A condition on anything else never holds. |

**Deprecated spellings**, still accepted from older services: `default_by_axes: {"3": v, "5": v}`
(read as `default_by: {"machine_axes": …}`), `axes` as a `show_if` key (read as `machine_axes`), and
`axes: n` on a choice (read as `show_if: {"machine_axes": n}`). New services should not use them.

**`machine_axes`** is a fact the slicer computes from the selected printer preset. It is `5` when
the preset has non-planar printing on (`enable_nonplanar`) with a real rotary axis pair
(`axis_selection` is not `virtual`), and `3` otherwise. It is also sent as a form field, so the
service can apply the same rule on its side.

The dialog saves values **per printer preset**, and only the values that differ from the default,
so a later change of default reaches the user. When a slice is sent, `options` holds the saved
values whose field still exists, still applies (`show_if`) and still validates, plus every value
the selected mode fixes. The slicer caches
the schema and refreshes it in the background each time the dropdown opens, and fetches it again
before the dialog opens. If the service is unreachable, the cached copy is used.

### `POST <root>/slice`

`multipart/form-data`:

| Field | Always | Notes |
|---|---|---|
| `mesh` | yes | **The current plate**, as a binary `.3mf` (Bambu/Orca project format). See below. |
| `settings_from_mesh` | yes | `1`: the settings to slice with are the ones inside `mesh`. |
| `plate` | yes | The plate's number in the user's project, from 1. `mesh` holds only that plate. |
| `machine_axes` | yes | `3` or `5`, as above. |
| `mode` | when the service lists modes | The selected mode's `id` (`GET /`). |
| `options` | when any are set | A JSON object `{dest: value}`: the options dialog's values plus the mode's fixed `options`. |
| `printer`, `process`, `filament` | yes | The names of the selected presets, as **labels only**. |
| `settings` | no | A v1 field, a resolved-config JSON file. A v2 client does not send it. A service may still accept it for v1 clients. |

**What `mesh` holds.** The slicer writes a courier 3MF. It is never the user's project file.

- It holds the printable instances of the **current plate only**: every object and part on that
  plate, which print together as one job. Objects on other plates are not in the file.
- Every instance is moved by the plate's origin, so the file reads as a one-plate project with the
  plate at the origin. G-code that comes back in that frame is in the plate's own frame.
- `Metadata/model_settings.config` has one `<plate>` (`plater_id` 1) with this plate's settings
  (bed type, print sequence, …), the per-object and per-part overrides, and each part's filament.
- `Metadata/project_settings.config` holds the **resolved settings**: the selected presets with the
  user's unsaved edits, overlaid with the plate's settings, as for a local slice of that plate.
  These are values, not preset names. Print-host credentials are removed, as from any exported
  project.

The settings travel inside the file on purpose. A hosted service cannot see the user's preset
library, so preset names would not resolve there. With the values in the file, the two programs
only have to agree on what the setting keys mean. They never have to share a profile catalogue. The
three names are for display or a job record, and a service must not reject a request because a
name is unfamiliar.

Expected response, `202` (any 2xx is accepted):

```json
{ "job_id": "79cbadab8ab8462ab79880e045a86e31", "status": "queued", "warnings": [] }
```

Any status of 400 or above is an error. The slicer shows the body to the user, so write it as a
sentence, not a stack trace. See [Errors and refusals](#refusals) for the shapes it reads.

### `GET <root>/jobs/{id}`

```json
{ "status": "running", "progress": 0.42,
  "stages": [ { "id": "mesh", "label": "Preparing mesh", "state": "done" },
              { "id": "paths", "label": "Planning toolpaths", "state": "active" },
              { "id": "gcode", "label": "Writing G-code", "state": "pending" } ],
  "warnings": [] }
```

```json
{ "status": "succeeded",
  "result": { "filename": "part-79cbadab.gcode", "size": 8520017, "sha256": "0013…6168" },
  "warnings": ["Two overhangs exceed the tilt limit and were printed planar."] }
```

| Field | Notes |
|---|---|
| `status` | Exactly one of **`queued`**, **`running`**, **`succeeded`**, **`failed`**. Any other value is an error. |
| `progress` | Optional, `0..1`. The slicer maps it onto its progress bar and never moves the bar backwards. Without it, the bar creeps slowly. |
| `stages[]` | Optional. The `label` of the entry whose `state` is `active` is shown as "Slicing: <label>…". |
| `error` | On `failed`, the reason. The slicer shows it. |
| `result` | On `succeeded`. `filename` names the local copy. A name ending in `.gcode.3mf` is unpacked (below). The slicer checks `sha256` (lowercase or uppercase hex) against the download. `size` is informational. |
| `warnings` | Optional strings. The slicer shows them, and they do not stop the result being used. |

### `POST <root>/jobs/{id}/cancel` (optional)

The slicer sends this when the user stops the job after it was submitted. The body is `{}`. The
call is best effort: the slicer ignores the answer and does not wait for it, and a 404 is fine.

### `POST <root>/jobs/{id}/grant` (optional)

Before downloading, the slicer asks for a one-use download path. The body is `{}`.

```json
{ "url": "jobs/79cbadab8ab8462ab79880e045a86e31/gcode?grant=3c1e…" }
```

The slicer accepts `url` only if it is **relative to the root and starts with `jobs/{id}/`**, so a
grant cannot send the download anywhere else. Any answer other than 200 with such a `url` makes the
slicer fall back to the plain path. A service without grants can return 404.

### `GET <root>/jobs/{id}/gcode`

This returns the result bytes. Use `text/plain` for G-code and `application/octet-stream` (or
`application/vnd.ms-package.3dmanufacturing-3dmodel+xml`) for a `.gcode.3mf`. The slicer uses
`Content-Length` for download progress. `HEAD` on the same path should return the same headers
with no side effects. The slicer writes the bytes unchanged, so the quoted `sha256` still matches.
Responses are limited to 512 MiB.

**`.gcode.3mf` results.** If `result.filename` ends in `.gcode.3mf`, the slicer takes the G-code out
of the archive: `Metadata/plate_1.gcode`, or else the first `*.gcode` entry. The plate's slice
result is that G-code. When the user sends to a printer that takes a sliced 3MF, the slicer packs
its own 3MF around it.

### `POST <root>/prepare` (optional)

This uses the same multipart fields as `/slice`. The service may do its slow, geometry-only work
(meshing, decimation) and cache it, so that a later `/slice` of the same plate starts sooner. It
makes no G-code and returns no job.

```json
{ "status": "queued" }
```

`status` is informational: `queued`, `running` or `skipped`. While a service mode is selected and
the person is editing, the slicer sends a prepare after the plate's geometry has stayed the same
for about 3 seconds. Settings and option changes do not trigger one. No prepare is sent while a
slice is running.

- `404` or `405`: the service has no such endpoint, and the slicer stops sending prepares for the
  rest of the session.
- `401`, `402` or `403` (from the prepare itself or from the provider's token request): the slicer
  stops sending prepares for this account until the account changes. This is silent, because
  nobody pressed Slice.

---

<a id="refusals"></a>
## 5. Errors and refusals

Any endpoint may answer **`401`, `402` or `403`** to refuse a request, for example because the
user is not signed in, has no plan that includes the service, or has used up a quota. The
canonical body is:

```json
{ "error": { "code": "plan_required",
             "message": "Tilted slicing is part of the Studio plan.",
             "action_url": "https://slicing.example.com/plans",
             "action_label": "See plans" } }
```

| Field | Notes |
|---|---|
| `error.code` | A machine-readable string the service chooses. The slicer logs it. |
| `error.message` | Human text. The slicer shows it in its normal error notification. |
| `error.action_url` | Optional. The slicer adds a button that opens this URL in the system browser. Only `http` and `https` URLs are opened. A path (`/plans`) is resolved against the origin that answered. |
| `error.action_label` | Optional. The button label. The default is "Open". |

On a `401`, the slicer also offers "Sign in" when an account provider is configured.

The slicer also accepts these looser forms, so an existing service needs no changes:

| Body | Read as |
|---|---|
| `{"error": "Sign in to slice."}` | `error` (a string) is the message |
| `{"detail": "Sign in to slice."}` | `detail` (FastAPI's default) is the message. A non-string `detail` is shown as JSON. |
| `{"error": "…", "upgradeUrl": "/plans"}` | `upgradeUrl` is the `action_url` |
| anything else | the raw body, trimmed to 500 characters |

The same shapes apply to every other error status (400, 404, 422, 500 …). Only 401, 402 and 403
are treated as refusals and get the action button.

A slicer never decides by itself that a service is unavailable to someone. A service mode in the
dropdown is never greyed out, relabelled or hidden based on the user's account. The service's own
answer is the only gate.

---

## 6. What the slicer does with the result

- It sends the **whole current plate**, and only that plate. A service that merges the plate into
  one solid should say so in its own UI.
- It does **not** slice locally first. The local slice plays no part: no settings validation, no
  time estimate, no G-code.
- The result becomes the **plate's slice result** in the same project. It is not a separate file
  that gets opened. Preview shows it, and Print and Export G-code use it, like a local slice. Any
  edit that would invalidate a local slice (the plate's objects, any setting, the service options)
  also invalidates it. If the plate is edited while the service is slicing, the result is dropped.
  If the result arrives while another plate is current, the slicer keeps it until that plate is
  current again.
- **A service run starts only from the person's own action**: the Slice button, Ctrl+R, a click on
  the Preview tab, or picking a plate in Preview. It never starts from the automatic re-slice after
  a change, and never from an embedded web page. A page may preselect a mode, but it cannot start a
  run ([WEB_APP_BRIDGE.md](WEB_APP_BRIDGE.md), "No service run from a page").
- The preview draws the bed of the **selected printer preset**. Return G-code in the frame that
  printer expects. A service that centres on a rotary origin instead of a bed corner will look
  misplaced on a printer whose bed does not share that origin.

---

## 7. Changes from version 1

| | v1 | v2 |
|---|---|---|
| Plate | the project, split | a self-contained 3MF of the current plate + `plate` |
| Settings | `settings` JSON file | inside the 3MF (`settings_from_mesh=1`) |
| Options | none | `GET /ui-schema` + `options`, `mode`, `machine_axes` |
| Discovery | none | `GET /` (`display_name`, `modes` with `show_if` / `options`) |
| Progress | none | `progress`, `stages[]` |
| Cancel | local only | `POST jobs/{id}/cancel` |
| Download | `GET jobs/{id}/gcode` | optional `POST jobs/{id}/grant` first, `sha256` checked, `.gcode.3mf` accepted |
| Refusals | free text | 401/402/403 with `{error: {code, message, action_url, action_label}}` |
| Auth | none | provider token, same-site or allow-listed origins only |

A v1 service still works with a v2 slicer. It ignores the new form fields, has no ui-schema and no
discovery (so the dropdown has one entry), and reports no progress (so the bar creeps).

---

## 8. Implementation

| File | Role |
|---|---|
| `src/slic3r/Utils/NonplanarSliceClient.{hpp,cpp}` | the protocol: discovery, submit, prepare, poll, cancel, ui-schema, grant, download, address and transport rules, when a token is attached |
| `src/slic3r/Utils/ServiceRefusal.{hpp,cpp}` | the refusal format, its tolerant forms, and the same-site rule |
| `src/slic3r/GUI/Jobs/NonplanarSliceJob.{hpp,cpp}` | runs a job on a worker thread: token, progress, `.gcode.3mf` unpacking |
| `src/slic3r/GUI/SlicePrepare.{hpp,cpp}` | the background `/prepare` |
| `src/slic3r/GUI/SliceServiceOptionsDialog.{hpp,cpp}` | modes, the ui-schema vocabulary, the options dialog and the stored values |
| `Plater::export_slice_service_handoff()` | writes the courier 3MF |
| `Plater::accept_slice_service_result()` | makes the result the plate's slice result |
| `tools/reference-slice-service/` | MIT reference service and its tests |
