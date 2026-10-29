# Reference slicing service (MIT)

A complete implementation of [SLICE_SERVICE_PROTOCOL.md](../../SLICE_SERVICE_PROTOCOL.md)
(protocol version 2) in one file of standard-library Python. It is here to show that any program
can stand behind the slicer's **Slice with service** entry. Copy it, or use it to test a slicer or
your own service. It is MIT-licensed, unlike the rest of this repository (see [LICENSE](LICENSE)).

It does not really slice. Its "result" is a short, valid planar G-code outline traced around the
plate's bounding box at the plate's own layer height and temperatures. It reads these from the
uploaded 3MF (`3D/*.model` and `Metadata/project_settings.config`) or from an STL. That is enough
to drive the whole round trip in a real slicer and see the result in Preview.

## Run it

```
python server.py                     # http://127.0.0.1:8000
```

Then, in the slicer, open **Preferences ▸ Advanced ▸ Web apps & services** and set **Slicing
service URL** to `http://127.0.0.1:8000`. Plain `http` is accepted for loopback addresses only. The
Slice dropdown gains the service's modes (two, or three on a five-axis printer). **Reference Slicing options…** opens the form from
`/ui-schema`.

| Flag | Effect |
|---|---|
| `--port N`, `--host H` | Where to listen (default `127.0.0.1:8000`). |
| `--stage-seconds S` | How long each fake stage takes (default 0.5), so progress is visible. |
| `--refuse` | Every `/slice` and `/prepare` is refused with **402** in the canonical refusal format (`{"error": {code, message, action_url, action_label}}`). |
| `--refuse-status 401\|402\|403`, `--refuse-style canonical\|string\|detail` | The refusal's status and body shape. `string` and `detail` are the looser shapes clients also accept. |
| `--token T` | Require `Authorization: Bearer T`. Without it every endpoint except `GET /` answers 401. |
| `--gcode-3mf` | Return results as `.gcode.3mf` (the slicer takes `Metadata/plate_1.gcode` out). |
| `--require-grant` | Refuse plain downloads; only a one-use path from `POST jobs/{id}/grant` works. |

## What each endpoint does here

| Endpoint | Behaviour |
|---|---|
| `GET /` | `display_name`, `protocol: 2` and three `modes`: `outline` (the default, first), `outline_double` with fixed `options: {loops: 2}`, and `outline_alternate`, offered only on five-axis machines (`show_if: {machine_axes: 5}`) with `options: {direction: "alternate"}`. |
| `GET /ui-schema` | Six fields. Includes a `show_if` on another field (the gap only with two or more loops), a choice with `show_if: {machine_axes: 5}`, and a `default_by: {machine_axes: …}`. |
| `POST /prepare` | Hashes the plate: `queued` the first time, `skipped` after that. |
| `POST /slice` | Reads the plate and starts a job with three fake stages. Honours `mode` (default: the first mode), checks the mode's `show_if` against `machine_axes`, and applies the mode's fixed `options` over the form's. Bad input gets `400`/`422` in the error format. |
| `GET /jobs/{id}` | `status`, `progress`, `stages[]` with one `active`, then `result {filename, size, sha256}` and `warnings`. |
| `POST /jobs/{id}/cancel` | Stops the job, which ends as `failed` with "Cancelled by the user.". |
| `POST /jobs/{id}/grant` | `{url: "jobs/{id}/gcode?grant=…"}`, valid for one download. |
| `GET`/`HEAD /jobs/{id}/gcode` | The result bytes, with `Content-Length`. |

`read_refusal()` in `server.py` is a reference for client authors. It reads an error body exactly
as the protocol's section 5 describes (canonical object, string `error`, FastAPI `detail`,
`upgradeUrl`, raw text) and opens only `http(s)` action URLs.

## Tests

```
python -m unittest -v
```

`test_server.py` starts the service on an ephemeral loopback port and exercises every endpoint over
HTTP, sending the same multipart fields the slicer sends. It also checks every refusal shape the
protocol accepts, including a FastAPI-style `{"detail": …}` body.
