# Changelog

All notable changes to Unlayered Slicer are listed here, newest first.

## 0.1.8 — 2026-10-24

### Added

- **Update your Unlayered printer** in the setup wizard: after the printer picker, when an Unlayered
  printer is picked or already set up. Check for updates reads the printer and the published firmware and
  printer settings and says what is waiting; when you finish setup, the "Printer update available"
  notification installs them, firmware first, then the settings, each confirmed. Calibration, name and
  network settings are never replaced. The printer settings need the free Unlayered account.
- **Unlayered Agent.** Ask for what you want in plain words ("print this in PETG on my P1S", "make
  this part stronger", "fix the stringing", "cut the roof at its own angle") and the Agent makes the
  changes in your slicer for you: printer, filament and process settings, slots, orientation,
  scale, copies and arrangement, and cut planes. It looks settings up instead of guessing, every
  change it makes can be undone in one step, and it never exports, sends or prints anything without
  you confirming in the slicer. It opens in a panel beside the tabs (the button next to Print,
  View > Show Side Panel, or Ctrl+Shift+B) and needs an Unlayered account: sign in from the panel or
  the Help menu. Plans: unlayered3d.com/agent.
- **Side panel** beside the tabs, for the Agent or a web page of your choice, shown next to every
  tab. Like the web-app tab it talks to the slicer only through the documented bridge
  ([WEB_APP_BRIDGE.md](WEB_APP_BRIDGE.md)), within the permissions you give it.
- **Web apps & services** in Preferences > Advanced: the addresses of the web-app tab and the side
  panel. Blank turns each off.
- **Setting changes for pages:** `slicer.settingsChanged` says what changed (new, previous, saved and
  system value), so a page can follow along.
- **A/C trunnion printers:** *Printer type* A/C converts the exported G-code to machine A/C
  coordinates (pivot height, C offset, solution policy, transition clearance).

### Fixed

- Crash reports now include where in the slicer a crash happened. The location was dropped for
  being too long, so reports from crashes inside the slicer's own code arrived with only the error
  code.

## 0.1.7 — 2026-10-23

### Added

- **Import your settings from other slicers.** Unlayered Slicer finds OrcaSlicer, PrusaSlicer,
  Bambu Studio, Creality Print (6 and 7), SuperSlicer, ElegooSlicer, Anycubic Slicer Next,
  Snapmaker Orca, QIDI Studio, Flash Studio, PhrozenOrca and FLSUN Slicer on your computer and
  brings over the printers and filaments you picked from their built-in lists, and your own
  printer, filament and process presets, in one step. You choose which apps to import from. The
  setup wizard offers it as its own step before you pick printers (only when there is something to
  import), so what it found is already ticked on the next pages. Also from **File > Import > Import
  Settings from Other Slicers**, or a one-time question on the first launch after updating (Import /
  Not now / Don't ask again). On a fresh install the printer, process and filament you last used
  there are selected too; an existing selection is never changed. Your presets keep their values,
  and stay linked to the matching Unlayered Slicer profile where there is one; PrusaSlicer-style
  settings are translated to their names here. Nothing in the other apps is changed and nothing
  here is overwritten: a name that is already taken gets " (OrcaSlicer)" or similar added, and
  importing again only adds what is new. Printer network addresses come along; API keys, user
  names and passwords never do. The summary lists per app what came over, what was skipped and
  why, and which printers need their key entered again.
- **Recent projects from other slicers on the home page.** "Recently opened" also lists the projects
  your other slicers opened recently, each marked with the slicer it came from, with a filter per
  slicer. Removing one or "Clear all" hides them here; the other slicers' own lists are untouched.
- **Printer update available.** When the selected printer is an Unlayered printer on your network,
  the slicer checks once per session whether newer firmware or printer configuration has been
  published, and offers it in a notification: Update / Not now / Skip this version. Update installs
  the firmware first, then the configuration, each with its usual confirmation. Nothing is
  installed on its own.
- **Printer configuration updates from unlayered3d.com.** The printer's Config button checks
  unlayered3d.com for new Unlayered printer configuration releases. Configuration updates come with
  your Unlayered account: sign in from the Help menu (the "Access token" setting in Preferences is
  gone). Every file is checked against its published checksum before anything is written; the files
  a printer measured for itself (its calibration, name, network and setup) are never replaced; the
  files being replaced are saved to this computer first; and if an install fails partway, the
  previous files are put back.
- **The printer's web interface is updated with its firmware.** A firmware release that carries a
  matching web interface installs it after the firmware (the "Web interface" box in the Firmware
  dialog), so the printer's web page no longer warns about mismatched versions.
- **Unlayered Universe: share your printer profile and install others'.** Publish the printer you
  have set up to Universe, or install one someone else shared. Before anything is installed the
  slicer shows the profile and lists every custom G-code line that updates firmware, touches files
  or the network, changes safety limits or restarts the controller, and installs nothing unless you
  agree. Printer connections (network addresses, API keys, passwords) and local file paths are
  never shared, and are removed again on install. The profile is added as your own printer preset.
- **A crash report is your choice when you do not share usage diagnostics.** After the app closes
  unexpectedly, the next launch asks whether to send that one crash report and shows what it
  contains. It is sent once, with a random identifier that is not linked to your installation.
  Decline or close the question and nothing is sent. See USAGE_DIAGNOSTICS.md.

### Changed

- **Stealth Mode is off by default and no longer asked in setup.** With it on, the slicer also
  skipped the account sign-in check and printer update offers. It is switched off once on existing
  installs; turn it on again in Preferences and it stays on.
- **Signing in.** While the slicer is not signed in to your Unlayered account, the setup wizard has a
  "Sign in to Unlayered" step, and the first launch of a new version asks once. Both can be skipped.
  The Help menu has one item that reads Sign in or Sign out.
- **Setup wizard.** The multiplanar tutorial page says Next instead of Finish (the network plug-in
  question still follows), and the two choices on that last page are larger. The Bambu Lab network
  plug-in box is ticked when another slicer on this computer has Bambu Lab printers, with a line
  saying so; otherwise it stays unticked.
- **Choosing a theme switches to it**, also when the colour scheme is set to System.
- **Usage diagnostics** (only when you share them) now include how a printer configuration install
  ended (versions and outcome; never the printer's address, name or files) and how many presets were
  imported from each slicer (a number, never a name). See USAGE_DIAGNOSTICS.md.

### Fixed

- **Z contouring failed with several copies of an object on the plate.**
- **Universe and other web tabs could stay white** when the app started on one (macOS).
- **Startup and the home page could wait on macOS folder-permission prompts** while reading project
  thumbnails; thumbnails now load in the background.
- **The "Save as" name field in the printer's network settings was unreadable** with a dark
  operating system and a light slicer theme.
- **The multiplanar tutorial link did not open on macOS.**

## 0.1.6 — 2026-10-14

Changes since **0.1.5** (2026-10-13).

### Changed

- **No Orca Cloud.** The home page no longer shows an Orca Cloud or Bambu Lab account panel, and
  Preferences no longer has a login region or login side panel. Nothing in the slicer signs in to
  Orca Cloud.
- **The Bambu Lab network plug-in is off by default** on the first-run setup page; tick it there,
  in the installer, or later in Preferences if you print to Bambu Lab printers over the network.
- **Usage diagnostics are off until you choose to share them.** Setup now asks where you live, in
  one click (United States, Canada, Mexico, Central America & Caribbean, European Union / EEA,
  United Kingdom, Switzerland, Asia-Pacific, China, Rest of the world), then asks **Help us build a
  better slicer**: **Keep my data private** or **Share usage diagnostics**. Outside the United
  States neither is selected; in the United States, Share starts selected and can be changed before
  continuing. Your answer is saved before anything optional starts. Updating from 0.1.5 or earlier
  (where diagnostics were on by default) asks the same question once the app has started; nothing
  is collected or sent until you answer. If you had switched diagnostics off, that is kept.
- **Slicing improvement program**: an optional, separate box under Share, never ticked in advance.
  Members get early access to new features, and their shared data may be used to develop, train,
  test, evaluate, and improve AI, CAD, slicing, software, manufacturing, and hardware systems.
  Ordinary usage diagnostics are not used for that. Join or leave any time in Preferences > Online.
- Shared diagnostics now include the values of the settings you change (never custom G-code, names,
  paths, file references or credentials) and coarse measurements of each part (never its shape).
  After you export or send a print, a notification asks once how it came out (only while
  diagnostics are on). See [USAGE_DIAGNOSTICS.md](USAGE_DIAGNOSTICS.md).
- **The installation id is created only when you choose to share diagnostics**, not at first launch.
  It is kept if you later switch them off, so you can quote it in a privacy request, but no longer sent.
- **Privacy policy 2026-10-v3** is bundled, with the consent wording it sets
  out. Anyone who answered the earlier wording is asked again.
- **Preferences > Online > Privacy** shows your choice and where you live, switches diagnostics on or off at
  once (switching off deletes anything not yet sent), lets you join or leave the Slicing improvement
  program, lets you choose where you live again, and opens the full privacy policy.
- **Sharing a project to Unlayered Universe asks first, every time**, saying exactly what is
  uploaded, why, to whom and for how long, with a separate, unticked box for AI training. Usage
  diagnostics never upload models.
- Diagnostics carry only allowlisted fields; error messages hide paths with spaces in them and model
  file names; part measurements are rounded.
- **Crash reports on Windows say how the session ended**, read from the Windows event log at the
  next launch (the faulting module and offset, or that the app stopped responding), when
  diagnostics are shared.
- When a copy cannot update itself (for example, one unzipped or run from a build folder rather than
  installed), shared diagnostics now say why.
- **The update check notes, once, that an update installed and which privacy choice you made**
  (keep private or share), with no identifier, so we can count both without usage diagnostics.
  See [USAGE_DIAGNOSTICS.md](USAGE_DIAGNOSTICS.md).

### Fixed

- **macOS: "What's new" after an update was empty.** The changelog was missing from the macOS app.
- Unlayered's own macOS builds are counted as internal copies in usage diagnostics, as on Windows.
- Long sessions no longer build diagnostics batches too large for the site to accept: batches are
  split by size as well as by number of events.
- **An update downloaded before the app crashed is no longer lost.** It used to be downloaded
  again and only installed at the next clean exit. Now the next launch reuses the download (after
  checking it again) and offers **Install now**.

## 0.1.5 — 2026-10-13

Changes since **0.1.4** (2026-10-12).

### Fixed

- **macOS: Quit while the setup wizard was open did nothing.** Dock › Quit, logging out and Cmd-Q
  were ignored while the first-run wizard was up, so the only way out was Force Quit, and the
  wizard came back at the next launch. Quitting now closes the wizard (it returns next time, since
  nothing was saved) and quits normally. The wizard's red close button cancels setup.
- **macOS: a crash when quitting within the first half-minute** after launch.
- **macOS: Quit with another dialog open** (such as the Bambu Lab network plug-in prompt) is refused,
  as it always was on macOS, instead of leaving a half-closed app behind.
- **A crash measuring text in the 3D view** (an empty label).
- **Updates only offer a version that has an installer for your system.** A release published for
  one platform no longer prompts the others to update.

### Changed

- **Crash reports name the calling code**, not just where the crash happened, on Windows, and macOS
  and Linux now report crashes and hangs at all. See [USAGE_DIAGNOSTICS.md](USAGE_DIAGNOSTICS.md).
- Unlayered's own copies of the slicer (developer builds, copies run from a build folder, or with
  `UNLAYERED_INTERNAL` set) report diagnostics as `dev`, so they are not counted with customers'.

## 0.1.4 — 2026-10-12

Changes since **0.1.3** (2026-10-11).

### New

- **A/C printers** (a C turntable on an A cradle, such as a Voron trunnion build): the exported
  G-code is converted to the machine's own A/C coordinates. Each section's table pose is worked out
  from its build direction (A and C stay inside their limits, and C unwinds if it is endless), and
  between sections the nozzle lifts clear of the part and the turntable before the table turns.
  Set the A axis height, the C axis position and a few more values under **Printer settings ›
  Motion ability › Non-planar Printing** after choosing Printer type **A/C**. The slicer, preview
  and collision check keep working in the bed frame; only the exported file changes.
- **A/B printers: experimental.** A first, untested take on a bed carried by three independent Z
  leadscrews ("tri-Z"), tilted by driving them apart: A tilts it about X, B about Y, around a pivot
  in the bed plane at the leadscrews' centre (set its position with the A/C axis height and offset
  settings). The G-code carries A and B words, which the firmware has to turn into the three
  leadscrew heights. Leadscrew beds only tilt a few degrees, so tilt limits left at their defaults
  count as +-10 degrees; set the real limits from the machine. Check the G-code carefully before
  running it on a printer.
- **Updating shows progress.** "Restart to update" now shows the installer's progress window instead
  of nothing for the minute or two it takes, which looked as if the slicer were slow to start.

### Fixed

- **Brim type saved the wrong value** on every printer: "Outer brim only", "Inner brim only" and
  (in the object list) "No-brim" were stored as a different brim type. Projects saved with the wrong
  value need the brim type picked again.
- **Support direction:** support enforcers and blockers were shifted into the lean twice.
- **SVG infill** finds `<use>` targets and `<pattern>` cells reliably.
- **Belt printers:** eSUN PLA on the Printcepts BabyBelt Pro and IdeaFormer IR3 V2 has its own
  filament id.
- **Linux (Flatpak):** the Flatpak build failed because it did not include the changelog.
- **Tilted layers printed under the build plate.** An object sunk partly into the plate with its
  own layer rotation (Layer Rotation B/C) printed the sunk part below the plate, which drives the
  nozzle into the bed on a 3-axis printer. It is now cut off at the plate like any other object, and
  a virtual 5-axis slice whose toolpath would still go below the plate is refused with an error.
- **Preview lines turned grey** after hiding a line type (for example Outer wall) and moving the
  layer slider onto a layer with nothing left to show, common just above a cut plane.
- **Bambu Studio 2.x projects** no longer open a "generated by a newer version" warning about the
  vertical shell thickness setting.

- **Virtual 5-axis prints refused for going "above the total height"** when they did not. The check
  counted the printer's own start and end G-code (for example a Prusa end G-code that lifts and
  presents the print) and a 20 mm lift the slicer added at the end of every virtual print. It now
  checks only the print itself, and the extra lift is gone on virtual printers.
- The setting is now called **Max print height (from the bed)**, with a clearer explanation: it is
  a height measured from the bed, usually close to the printer's build height, not a clearance. The
  error names the setting and where to find it.

### Changed

- **Crash reports say where and when.** When the slicer crashes, the next launch's diagnostics now
  include what it was doing (starting, slicing, closing) and, on Windows, the error and the program
  file it happened in. Closing because Windows logged off or shut down no longer counts as a crash.
  See [USAGE_DIAGNOSTICS.md](USAGE_DIAGNOSTICS.md).
- **Diagnostics now cover the in-app updater**: what the update check found, whether the download
  worked (and the kind of failure if not), and whether the update installed. Account sign-ins are
  counted as worked, cancelled or failed, never with your email. See
  [USAGE_DIAGNOSTICS.md](USAGE_DIAGNOSTICS.md).
- While diagnostics are on, the update check also sends the diagnostics install id, so the site can
  count different installs instead of launches. The site keeps only a daily count, never the id;
  with diagnostics off, no id is sent.

## 0.1.3 — 2026-10-11

Changes since **0.1.2** (2026-10-10).

### New

- **Anonymous usage diagnostics.** Unlayered Slicer now tells us which features are used, how
  slicing went (including errors), when it crashes, and basic system details (operating system,
  processor cores, memory, language), with a random id that is not linked to you or your account.
  Models, G-code, file and object names, and the names of your own presets are never sent. It is on
  by default: turn it off in **Preferences > Online > Send anonymous usage diagnostics**, or set the
  `UNLAYERED_NO_TELEMETRY` environment variable to switch it off for the whole computer. A notice
  says so the first time it runs.
- **macOS: the slicer updates itself**, as it already did on Windows. Signed in with your Unlayered
  account, a new version downloads in the background and replaces the app when you choose *Restart
  to update* or close it. This works when the app is in *Applications* (or *Applications* in your
  home folder); a copy anywhere else still opens the download page.

### Changed

- **Easier to get started with multiplanar printing.**
  - The Cut Planes tool is greyed out until non-planar printing is turned on for your printer. Its
    tooltip now says so. Clicking it offers to turn non-planar printing on, with Printer type set
    to Virtual (for an ordinary 3-axis printer). Unlayered 5-axis printers already have it on.
  - *Enable non-planar printing* and *Printer type* (Printer settings > Motion ability) now show in
    Simple mode. Before, you had to switch to Advanced mode to find them.
  - The multiplanar tutorial (Help > Multiplanar Tutorial) has been rewritten. It explains what a
    cut plane is for, lists the settings you need *before* the quick start, shows the toolbar
    icon, gives the correct setting locations, and adds a settings reference.
  - The help links on the non-planar and layer-rotation settings open that tutorial. Before, they
    went to web pages that don't exist.

### Fixed

- **macOS: "Unlayered Slicer quit unexpectedly" on every quit** while the Bambu Lab network plug-in
  was installed. The plug-in is now unloaded before the app exits.
- **Linux: embedded web tabs (such as Universe) could open blank.**
- **Building from source on macOS and Linux** works again, including the universal (Intel and Apple
  silicon) macOS build.

## 0.1.2 — 2026-10-10

Changes since **0.1.1** (2026-09-21).

### ⚠️ Safety fixes — please update

- **The nozzle could drive down into a finished multiplanar print.** On a print with cut planes
  (multiplanar or virtual 5-axis, printed *By group*), the `{max_layer_z}` value given to the
  printer's start/end G-code was the height of the *first* chunk, not the top of the part. Bambu
  Lab profiles end with `G1 Z{max_layer_z + 0.5} ; lower z a little`, so at the end of such a print
  the nozzle moved straight down into the part — seen on a Bambu Lab H2S, where it went about 30 mm
  into a finished Benchy. `{max_layer_z}` now always reports the true top of the printed part.
  Ordinary flat prints are unaffected. **If you print multiplanar or virtual 5-axis jobs on a Bambu
  Lab printer, do not use 0.1.1 for them.**
- **Virtual 5-axis: steep tilts are refused when the hotend would hit the part.** The tilt limit
  derived from your hotend's shape used to apply only when a gantry height was entered; it now
  applies whenever the hotend envelope is on. With the default envelope, tilts beyond roughly
  35–45° are refused with a message naming the hotend side that limits them. *Settings that used to
  slice may now be refused; that is intended.*
- **The hotend is now modelled as a four-sided pyramid** (separate angles for the +X, −X, +Y and −Y
  sides) instead of a single cone, and the virtual 5-axis collision check uses the head's real,
  upright orientation. Collision checks also follow G2/G3 arcs.
- **Bambu Lab P1S:** printing no longer clears the printer's SD-card state.

### Fixed

- **Supports rotated 90° away from the part** when multiplanar printing was on and *Layer rotation
  B* or *C* was changed. Supports now print under the part and stand on the plate.
- **macOS: "Slice Plate" crashed** on colour-painted objects whose project had fewer filament
  colours than filaments.
- **macOS and Linux builds** compile again.
- **Bambu Lab network plug-in could not be installed** ("network plug-in not found" with no way to
  install it). It can be installed again — from the installer, the first-run setup, or
  Preferences › Online — and is loaded correctly from long installation paths.
- Cut-plane pieces that sit off the build plate no longer get mixed into the shared brim.
- Auto drop (placing objects on the bed) is on by default again.
- Projects with missing or partial settings, and meshes with invalid vertices, no longer crash the
  loader or the command line.
- Painted facets (supports, seams, colours) land in the right place on rotated and cut objects.
- Copies of rotated objects each get their own orientation.
- 5-axis G-code from other software previews correctly.
- "Bind with Pin Code" explains why it cannot continue instead of silently doing nothing.

### New

**Getting started**
- **New first-run setup page — "Privacy and printer connections".** One page asks whether to
  enable Stealth Mode (off by default) and whether to install the Bambu Lab network plug-in (on by
  default), which is required for wireless communication with Bambu Lab printers.
- **The slicer updates itself.** Sign in once with your Unlayered account and new versions download
  in the background; choose *Restart to update* (or just close the app) to install. Help menu:
  *Restart to Update*, *Sign in*, *Sign out*. An interrupted download picks up where it stopped.
  (Updating *from* 0.1.1 still means installing 0.1.2 by hand once; from 0.1.2 on it is automatic.)
- **"What's new" after an update.** The first time a new version starts, it shows the changes since
  the version you had before, taken from this changelog.
- New installations start in the Dark colour scheme.

**Appearance and accessibility**
- **Colour themes:** named themes you can create, edit live, save, rename, import and export, with
  bundled *High contrast* and *Neon* presets. Every part of the window follows the theme — toolbars,
  3D view background, build plate, grid, sliders, notifications and overlays.
- **Colour-vision previews** to check a theme for colour blindness, and a contrast audit.
- **Font choice and font scale**, including OpenDyslexic and Comic Sans, applied without restarting;
  dialogs and controls scale with the font.
- **Full keyboard navigation:** a visible focus ring, Tab through the main tabs, menus, settings
  pages, sidebar, drop-downs and 3D-view toolbars in on-screen order; Space/Enter press buttons.
- Preferences is resizable, remembers its size, and applies theme changes live.

**Printing**
- **Wave overhangs** are no longer experimental. *(Ported from Dennis Klappe's
  [OrcaSlicer-WaveOverhangs](https://github.com/dennisklappe/OrcaSlicer-WaveOverhangs).)*
- **Mesh infill:** Catfill, Puppyfill, custom SVG patterns (previewed on the plate) and infill-mesh
  volumes, with orientation control and a single-wall option.
- **Belt printers:** slicing along a tilted gantry, support resting on the belt, brims, purge tower,
  several objects interleaved along the belt, and cut planes on a belt. New profiles: **Printcepts
  BabyBelt Pro** and **IdeaFormer IR3 V2**.
- **Contact-aware first layer:** first-layer speed, flow and fan now follow each line's height above
  what it rests on, rather than its layer number. On belt printers, every strip lying on the belt
  gets first-layer treatment; a cut-plane piece resting on plastic no longer gets bed-style first
  layers.
- **Support direction:** lean support along a chosen world direction; cut-plane supports carry the
  load of every piece above them and stop flush under the material they hold up.
- **Bed mesh overlay:** see your printer's probed bed mesh on the plate (RepRapFirmware and
  OctoPrint), with an out-of-flat warning.

**Unlayered printers**
- The printer configuration updater refuses to install files onto firmware too old to run them, so
  an update can no longer leave a printer that stops part-way through its startup file.

### Changed

- **Bambu Lab cloud features have been removed.** Cloud sign-in, cloud printing, the privacy-policy
  dialog and pin-code account binding are gone. Bambu Lab printers are used over the local network
  (LAN mode, with an access code); the network plug-in is still needed for that.

### Known issues

- **Windows may warn about the installer** ("Windows protected your PC", or an antivirus alert).
  The installer is not code-signed yet. Choose *More info › Run anyway*. If a download says
  *"installer integrity check has failed"*, the file was incomplete — download it again.
- The paint-on supports preview does not yet follow the rotation of a rotated or cut object; the
  sliced supports are correct.
