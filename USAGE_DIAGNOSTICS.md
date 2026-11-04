# Usage diagnostics

Unlayered Slicer can send optional usage diagnostics to Unlayered 3D, **only if you choose to share
them**. They tell us which features are used, which printers and materials people slice for, and
what fails, so we can fix the right problems. If you also join the optional **Slicing improvement
program**, your shared data may be used to develop, train, test, evaluate, and improve AI, CAD,
slicing, software, manufacturing, and hardware systems; ordinary usage diagnostics are not used for
that. This page lists **everything** that is sent. The
code that collects it is [`src/slic3r/Utils/UsageTelemetry.cpp`](src/slic3r/Utils/UsageTelemetry.cpp),
what an event may contain is enforced by
[`src/slic3r/Utils/DiagnosticsAllowlist.cpp`](src/slic3r/Utils/DiagnosticsAllowlist.cpp), and the call
sites are listed at the end. The Slicer Privacy Policy
([`resources/web/privacy/slicer-privacy-policy.html`](resources/web/privacy/slicer-privacy-policy.html),
opened from setup and Preferences) covers the same data. How the choice works in the code is in
[PRIVACY.md](PRIVACY.md).

## Your choice

Since 0.1.6 diagnostics are **off until you choose to share them**:

- **Setup** asks where you live, one click: United States, Canada, Mexico, Central America & Caribbean, European Union / EEA, United Kingdom, Switzerland, Asia-Pacific, China, or Rest of the world. The next page, **Help us build a better slicer**, offers
  **Keep my data private** or **Share usage diagnostics**. Outside the United States neither is
  selected; in the United States, Share starts selected (and is marked Recommended) and can be
  changed before continuing. Choosing Share shows a separate **Slicing improvement program** box,
  never ticked in advance anywhere. Continue is available once you pick an answer. Your answer is
  saved before anything optional starts, and nothing is sent while setup is open.
- **After updating from 0.1.5 or earlier** (whose diagnostics were on by default, which is not a
  choice) the same pages open once the app has started. Until you answer, nothing is collected or
  sent. Closing them answers nothing; they are offered again on the next launch, three times at
  most. If you had switched diagnostics off before, that stays your answer and you are not asked.
- **Preferences ▸ Online ▸ Privacy**: the **Send usage diagnostics** switch (applies at once;
  switching off deletes everything queued on the computer and not yet sent), the **Slicing
  improvement program** switch (only while diagnostics are on; joining or leaving also deletes what
  was queued under the other answer), your current choice
  and country, **Change** to choose your country again, and **Read Full Privacy Policy**.
- **For a whole computer**, set the environment variable `UNLAYERED_NO_TELEMETRY` to any value.
  Nothing is collected or sent while it is set, whatever anyone chose, and you are not asked. This
  is for organizations that deploy the slicer centrally.

Diagnostics are separate from stealth mode. Stealth mode does not switch them off.

Uploading a model is never part of diagnostics. Anything that uploads a model (today only the
Universe tab, when you share a project there) asks first, every time, for that one file, saying what
is sent, why, to whom and for how long, with a separate box for AI training.

## What is never sent

- Models, meshes, thumbnails or any geometry
- G-code
- File names, folder names or paths
- Object, part, plate or project names
- The names you give your own presets: they are reported as the built-in preset they are based
  on, or as `custom`
- Custom G-code, post-processing scripts, notes, printer hosts and credentials, file paths and file
  references, vendor names, and any setting value that spans several lines. Since 0.1.6 the *values*
  of other changed settings are sent (see `slice` below); these never are.
- The shape of a part: only coarse measurements of it (size to 5 mm, volume and surface area to two
  significant figures, overhang and bed-contact shares to 5 %)
- Your Unlayered account, email address or name
- Printer addresses, access codes, serial numbers or other printer identifiers

A random install id is sent with every batch (below). It is not your name or account, but it does
link the batches of one installation together, so diagnostics are **pseudonymous, not anonymous**.

## Identifiers

Every batch carries:

| Field | What it is |
|---|---|
| `install` | A random id (a UUID) created on your computer the first time you choose to share diagnostics, and stored in the app's settings (`usage_diagnostics_id`). It is kept if you later switch diagnostics off, so you can still quote it in a privacy request, but it is no longer sent. It is not derived from your hardware or account, and is not linked to either. It is never sent unless you chose to share diagnostics. |
| `session` | A random id created each time the app starts. |
| `program` | `true` if you are in the Slicing improvement program, otherwise `false`. Only batches marked `true` may be used to develop and train AI and the other systems named above. |
| `app.version` | The Unlayered Slicer version, for example `0.1.3`. |
| `app.channel` | `release`, `beta` (an alpha, beta or release-candidate version) or `dev`. Unlayered's own copies always say `dev`: a development build, a copy run from a build folder rather than installed, or one started with the `UNLAYERED_INTERNAL` environment variable set. The site counts only `release` towards its usage figures. |
| `app.os` | `windows`, `mac` or `linux`. |

## Events

Each event has a name, a timestamp and the properties below. Nothing else is attached.

| Event | When | Properties |
|---|---|---|
| `app_start` | Once per launch, about 30 seconds in | Operating system version (for example `10.0.26100`); `x64` or `arm64`; the app's display language; number of processor cores; installed memory in GB (rounded); whether this is the first launch of a new installation; whether stealth mode and dark mode are on; whether the Bambu Lab network plug-in is off, loaded or failed to load, and its version when loaded. |
| `preset_selected` | When a different printer, process or filament preset is selected | Which kind (printer, process or filament); the built-in preset's name, or the built-in preset a custom one is based on, or `custom`; whether it is a built-in preset. |
| `slice` | When slicing finishes | Result (`ok`, `error` or `cancelled`); how long it took; the number of plates, objects and copies; the printer, process and filament presets, named as in `preset_selected`; the printer's vendor; the material types (for example `PLA`); the **names** of up to 60 settings that differ from the built-in presets (for example `wall_loops`, never their values); which of a fixed list of features are on (non-planar printing, virtual 5-axis, Z anti-aliasing, wave overhangs, adaptive layer height, supports, spiral vase, prime tower, arc fitting, make-overhang-printable, brim ears, independent support layer height, cut planes, variable layer height, height-range modifiers, modifier parts, painted supports, seams, colours and fuzzy skin) and on how many copies; the chosen value of a fixed list of options with built-in choices (support type, wall generator, sparse infill pattern, slicing mode, print sequence, ironing type, fuzzy skin, seam position, timelapse type). If slicing failed: the type of error, and its message cut to the first line and 160 characters, with object, part and project names, quoted text, file paths (including folder names with spaces) and model file names removed. For errors in custom G-code templates only the type is sent, because the message would quote your own G-code. If slicing itself crashed (a hardware error such as an access violation), the error's name and where it happened, as a program file name and offset. Since 0.1.6, also: the **values** of the changed settings (up to 60, as `key=value`, per-object overrides as `o<n>.key=value`; never custom G-code, scripts, notes, names, paths, hosts or credentials, nor any value spanning several lines, and each item cut to 80 characters); for each object (up to 20) its size (to 5 mm), volume and surface area (to two significant figures), the share of its surface that overhangs by more than 45 degrees and the share resting on the bed (to 5 %), and its number of copies; a random `slice_id`; and for a successful slice the estimated print time and the model and support volume. |
| `export` | When G-code is exported or sent | What kind: `gcode` (and whether to removable media), `gcode_3mf` (and whether all plates), or `send` (and the kind of printer connection, for example `octoprint`; never its address). |
| `print_rating` | Only if you answer the "How did your last print come out?" notification, shown once after an export or send when the print should be done | The `slice_id` of the slice it rates; `good` or `problem`; for a problem, which of a fixed list you ticked (warping, bed adhesion, stringing, layer shift, supports, under- or over-extrusion, surface, failed part-way, something else); whether the G-code was exported or sent. |
| `update_offered` | When the update check finds a newer version | The new version number; whether you checked by hand; whether you are signed in; what happened next: downloaded in the background, reused the download from a session that ended before installing it, shown the update dialog and what you chose there (update, sign in, download page or close), skipped, or no in-app update possible (no installer for your platform, or a copy that cannot replace itself). When this copy cannot update itself, why: an unzipped or development copy, an all-users installation, or one registered to a different folder. |
| `update_download` | When an update download finishes | The version; whether it succeeded; how many seconds it took; if it failed, the kind of failure (for example `network`, `checksum`, `not_signed_in`) and the HTTP status. Never the error text. |
| `update_applied` | At the first launch after an update was downloaded | The version it was updating from and to; whether this launch is now the new version; how handing over to the installer went when the app closed (started, could not start, or another copy was still open). |
| `printer_config_update` | When the Config button (or the automatic printer update offer) finishes installing configuration onto a printer | The configuration version installed and the one it replaced (for example `2026.10.21` and `2026.10.12`, or `unknown`); the outcome (`success`, `up_to_date`, `failed`, `cancelled`); how many files were written; whether a failed install was put back; the printer's firmware version. Never the printer's address, name, board id, the file names or any error text. |
| `account_signin` | When you sign in to Unlayered from the app | Whether it worked, was cancelled or failed, and whether an update or the Help menu asked for it. Never your email address or account. |
| `crash` | At the next launch after the app closed unexpectedly | The version that crashed, how long it had been running (to the minute), and what it was doing (`startup`, `running`, `slicing` or `closing`). What ended it, when the app could tell: on Windows the error code (for example `0xC0000005`); on macOS and Linux the signal (for example `SIGSEGV`); `terminate` for an unhandled program error; `hang` when the app had stopped responding (which is what a force-quit of a frozen app looks like). Where it happened, on Windows and macOS, as a program file name and offsets into it (for example `UnlayeredSlicer.dll+0x1a2b3c` or `UnlayeredSlicer.arm64+0x2f1a3c>+0x2e1b00`, never a folder or path). A session that ended because the computer logged off or shut down, or on macOS an ordinary Quit, is not reported. Since 0.1.10 also a random `report_id`, and the end of that session's log is sent alongside (see Crash logs below). |
| `session_end` | When the app closes | How long it was open, and counts of what was used during the session: tabs opened (`tab:<name>`), tools opened (`gizmo:<name>`, for example `gizmo:cut_planes`) and files opened by type (`import:stl`, `import:3mf`, ... or `import:other`), and since 0.1.7 how many presets were imported from another slicer (one counter per slicer, for example `import:presets_prusaslicer`, `import:presets_orcaslicer`, `import:presets_bambustudio` or `import:presets_crealityprint`: a number only, never a preset name). |

## On your computer

Diagnostics keep a few small files in the `usage` folder of the app's data folder:

- `session-<id>.json` while the app is running: its process id, version, start time, what it is
  doing (`startup`, `running`, `slicing`, `closing`), the time it last sent diagnostics, and whether
  you had chosen to share diagnostics at the time. It is deleted when the app closes normally. One
  left behind means that session crashed, which is reported as a `crash` event, and only if
  diagnostics were shared during that session. Since 0.1.10 it also names that session's log file.
- `pending.json`: batches that could not be sent (offline, or the site was down), at most 20. They
  are retried at the next launch, and deleted when diagnostics are switched off.

## Crash logs (0.1.10)

Every crash report that is sent (a `crash` event of a session that shared diagnostics, or one you
agreed to send in the crash report question) also sends up to 256 KB of the end of that session's
log to `https://unlayered3d.com/api/slicer/crash-log`, once, with the crash's `report_id`. Before it
leaves your computer, your home folder, login and computer name, e-mail addresses, passwords, keys
and tokens, network addresses and the names of model, project, G-code and image files are removed
(`src/slic3r/Utils/CrashLogScrub.hpp` lists the rules). Printer and preset names can remain. The
site scrubs it again, keeps it up to 90 days, and only Unlayered staff can read it. When you decline
the crash report, the log is not read.

## Sending and storage

Batches of at most 100 events go to `https://unlayered3d.com/api/slicer/telemetry` over HTTPS, every
few minutes and when the app closes. No sign-in is used, and nothing in a request identifies you.

**The update check carries the install id too.** While you share diagnostics, the app's check for a new version
(`https://unlayered3d.com/slicer/latest.json`) sends the same `install` id in an `X-Unlayered-Install` header, so
the site can count how many different installs check for updates rather than how many times. The site never
stores the id itself, only a per-day hash sketch that can say how many, not which. Without that choice (or with
`UNLAYERED_NO_TELEMETRY` set) the update check sends no id.

**Two one-time notes on the update check, whatever you choose.** The first check after an update installs adds
the version you updated from (`&updated=0.1.5`), and the first check after you make or change your privacy
choice adds that choice (`&choice=private` or `&choice=shared`). These are not usage diagnostics: they add no
id, no account and nothing about how you use the app (while you share diagnostics the check carries your install id
anyway, as above, and the site does not store the two together), and the site only adds one to a count per day,
platform and version. They are how we know that updates install and how many people keep their data private.
With `UNLAYERED_NO_TELEMETRY` set, neither is sent.

On the site, the request's IP address is used only to limit how often one address can send and to
look up an approximate country. Neither the IP address nor any request header is stored. The
country is kept only as a daily count.

The site keeps daily aggregate counts (installs, sessions, events, versions, operating systems,
printers, features, countries) for 400 days. It also keeps the most recent batches as received, at
most 2,000 a day, for 30 days, to check that the data is sound. Only Unlayered 3D site
administrators can see either.

## Where it is in the code

- [`src/slic3r/Utils/UsageTelemetry.cpp`](src/slic3r/Utils/UsageTelemetry.cpp): batching, sending,
  crash detection, preset naming and error-message redaction
- [`src/slic3r/Utils/PrivacyConsent.cpp`](src/slic3r/Utils/PrivacyConsent.cpp): the saved choice, the regional rules, whether diagnostics may run
- [`src/slic3r/Utils/DiagnosticsAllowlist.cpp`](src/slic3r/Utils/DiagnosticsAllowlist.cpp): the events and properties that may be sent
- [`resources/web/guide/11/`](resources/web/guide/11/), [`13/`](resources/web/guide/13/) and
  [`src/slic3r/GUI/PrivacySetupDialog.cpp`](src/slic3r/GUI/PrivacySetupDialog.cpp): the region and privacy pages
- [`src/slic3r/GUI/GUI_App.cpp`](src/slic3r/GUI/GUI_App.cpp): `app_start`, the privacy question after an update
- [`src/slic3r/GUI/Plater.cpp`](src/slic3r/GUI/Plater.cpp): `slice`, `export`, file types opened
- [`src/slic3r/GUI/Tab.cpp`](src/slic3r/GUI/Tab.cpp): `preset_selected`
- [`src/slic3r/GUI/MainFrame.cpp`](src/slic3r/GUI/MainFrame.cpp): tabs opened
- [`src/slic3r/GUI/Gizmos/GLGizmosManager.cpp`](src/slic3r/GUI/Gizmos/GLGizmosManager.cpp): tools opened
- [`src/slic3r/GUI/Preferences.cpp`](src/slic3r/GUI/Preferences.cpp): the switch, current choice, country and policy link
