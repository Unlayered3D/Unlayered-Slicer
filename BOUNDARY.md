# Boundary: what the slicer talks to, and how

Unlayered Slicer is an AGPL-3.0 program. It works with other software only over three documented
network interfaces. Each interface is product-neutral and specified in this repository, and each
has a reference implementation under the MIT licence. Anyone can put their own program on the
other side of any of them. This page is a summary for engineers and reviewers.

```
                         Unlayered Slicer (AGPL-3.0, this repository)
   +-------------------------------------------------------------------------------+
   |  web-app tab / side panel /     Slice dropdown:          Help > Sign in        |
   |  model-library tab              "slice with service"                           |
   |  (browser control + bridge)     (HTTP client)            (HTTP client)         |
   +---------------+-------------------------+------------------------+-------------+
                   |                         |                        |
     web-app bridge: JSON messages   slicing-service HTTP:     account-provider HTTP:
     in the page's browser control   3MF out, G-code back      sign-in, short-lived tokens
     WEB_APP_BRIDGE.md               SLICE_SERVICE_PROTOCOL.md ACCOUNT_PROVIDER_PROTOCOL.md
                   |                         |                        |
   +---------------v----------+  +-----------v-------------+  +-------v-----------------+
   | any web application at   |  | any slicing service at  |  | any account provider at |
   | a URL the user sets      |  | a URL the user sets     |  | a URL the user sets     |
   | ref: resources/web/      |  | ref: tools/reference-   |  | (recipe in the protocol |
   |   webapp-example/ (MIT)  |  |   slice-service/ (MIT)  |  |  document, section 8)   |
   +--------------------------+  +-------------------------+  +-------------------------+
```

## The three interfaces

| Interface | What crosses it | Specification | Reference implementation |
|---|---|---|---|
| **Web-app bridge** | JSON messages between a web page shown in the slicer and the slicer: typed methods with per-slot permissions, events. Never internal data structures, files, processes or credentials. | [WEB_APP_BRIDGE.md](WEB_APP_BRIDGE.md) | [resources/web/webapp-example/](resources/web/webapp-example/) (MIT): calls every method, shows every event |
| **Slicing service** | HTTP: the current plate as a 3MF with its resolved settings goes out, and G-code (or a `.gcode.3mf`) comes back. | [SLICE_SERVICE_PROTOCOL.md](SLICE_SERVICE_PROTOCOL.md) | [tools/reference-slice-service/](tools/reference-slice-service/) (MIT, Python standard library, with tests) |
| **Account provider** | HTTP: browser-based sign-in, a device token, and short-lived tokens for services on the provider's own site. Authentication only. | [ACCOUNT_PROVIDER_PROTOCOL.md](ACCOUNT_PROVIDER_PROTOCOL.md) | the minimal recipe in that document |

All addresses are user settings in **Preferences ▸ Advanced ▸ Web apps & services**. A blank address
turns the feature off. A build may seed a default address only where no value is stored, and it
never overwrites a value the user set. The slicer treats a page or service at a default address
exactly like one at any other address.

## What the binary contains

- **No proprietary code is linked into, loaded by, or shipped in the slicer binary** to talk to
  these programs. The slicer side of every interface is in this repository under AGPL-3.0, and the
  programs on the far side run in their own processes, usually on other machines.
- **No entitlement logic.** The slicer does not decide who may use a service or a page. A service
  or page that restricts a feature refuses the request on its own server, in the documented refusal
  format, and the slicer shows its message.
- **No private product names.** The source and the public docs name no particular web application
  or slicing service. `tools/check_boundary.py` enforces this, and `tools/export_public.py` runs it
  on every public source export, so an export with a hit fails.
- **Tokens stay with their owner.** A provider token is sent only to a slicing service on the
  provider's own site or on an origin the user listed. A page never receives a token, and only a
  page on the provider's own origin is signed in.

Optional components that are not ours keep their own terms and are documented separately: the
Bambu Lab network plug-in (off by default, downloaded only when the user turns it on) and the
printer firmware images an installer may carry (see [README.md](README.md)).

## Corresponding source (AGPL-3.0 §13)

The complete corresponding source of each release is this public repository,
<https://github.com/Unlayered3D/Unlayered-Slicer>, with one tag per release that matches the
installer built from it. The slicer offers the same address to every hosted page as
`slicer.getInfo` → `app.sourceUrl`, so a page whose server runs the slicer for remote users can show
the offer without hard-coding it.

## Licences and trademarks

- The slicer: GNU AGPL-3.0 ([LICENSE.txt](LICENSE.txt)), as OrcaSlicer, Bambu Studio and PrusaSlicer.
- The reference implementations (`resources/web/webapp-example/`, `tools/reference-slice-service/`):
  MIT, so that they can be copied into a program under any licence.
- The protocol documents describe interfaces. Implementing an interface needs no permission from us.
- The Unlayered 3D name and logo are trademarks of Unlayered 3D. Neither the AGPL nor the MIT
  licence grants any right to them.
