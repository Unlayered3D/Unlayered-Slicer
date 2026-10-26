<div align="center">

<picture>
  <img alt="Unlayered Slicer logo" src="resources/images/UnlayeredSlicer.png" width="15%" height="15%">
</picture>

**Unlayered Slicer**: the slicer for Unlayered 3D printers, with multiplanar printing built in.

</div>

Unlayered Slicer is Unlayered 3D's fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). It is built on OrcaSlicer 2.5.0-dev and keeps its feature set: calibration tools, wall and seam control, adaptive layer heights, tree supports, multi-material printing, and network printing through Klipper, PrusaLink, OctoPrint, Duet and others.

This README describes **Unlayered Slicer 0.1.7**.

- **Website:** <https://unlayered3d.com>
- **What's new:** [CHANGELOG.md](CHANGELOG.md) lists the changes in each release.
- **Source:** this repository. Each release is tagged; [`v0.1.7`](https://github.com/Unlayered3D/Unlayered-Slicer/tree/v0.1.7) is the source of the 0.1.7 installers.

# What it adds

## Multiplanar printing

Instead of slicing a whole model in flat layers parallel to the bed, you split it into segments with cut planes, and each segment is sliced in its own orientation. Overhangs and surfaces can then be printed in whichever direction suits them.

- **Cut Planes** (top toolbar, Prepare view): place planes on an object without splitting it. A coloured preview shows the segments they produce, and each part can be given its own build direction.
- **Skeleton Cut** (Advanced mode): computes a skeleton of the model and adds a cut plane for each branch.
- **Supports per segment**: support generation follows the segments rather than assuming one flat bed.
- **Hotend collision check**: after slicing, a sidebar warning shows where the hotend would hit already-printed material.
- **Help ▸ Multiplanar Tutorial** walks through the workflow.

These features are switched on per printer: **Printer settings ▸ Motion ability ▸ Non-planar printing** (Advanced mode). The printer type there decides what the G-code looks like:

- **A/B, B/C or A/C** for machines with two rotary axes. The segment orientations are written as rotary-axis moves. **A/C** (a C turntable on an A cradle) converts the exported G-code to the machine's own coordinates, using the A/C settings under Motion ability; **A/B** is an experimental first take on a bed tilted by three independent Z leadscrews ("tri-Z"): A and B are the bed's tilt about X and Y, which the firmware turns into leadscrew heights.
- **Virtual** for an ordinary three-axis printer. Each segment's layers are still tilted, but the nozzle stays upright and no rotary moves are written. Set **Max tilt angle** and **Max print height (from the bed)** to suit your printer. Both default to 0, which means *no limit*.

## Unlayered printers

- **Unlayered 5 Axis Printer** profile (0.4, 0.6 and 0.8 mm nozzles), with its own process presets. Its start G-code calls macros that exist only on an Unlayered printer.
- **Unlayered host type** for printers running RepRapFirmware. It sends and prints like the Duet host type, and it adds two buttons to the physical printer dialog:
  - **Firmware**: updates the mainboard, Wi-Fi module and display, and the printer's web interface when the release carries a matching one. It uses releases of [Unlayered3D/RepRapFirmware](https://github.com/Unlayered3D/RepRapFirmware) published on unlayered3d.com, or the bundle shipped with the installer.
  - **Config**: installs printer configuration files published on unlayered3d.com (with your Unlayered account), or from a zip. Files the printer measured for itself are never replaced, the replaced files are saved on this computer first, and a failed install is rolled back.

  When the selected printer is an Unlayered printer on your network, the slicer also offers newer firmware or configuration once per session: Update / Not now / Skip this version. Nothing is installed without your confirmation.

  Both work over the network or USB. They target the Duet 3 Mini 5+ in an Unlayered printer. See [USB_UPDATES.md](USB_UPDATES.md).

## Web application tab

One tab, **Unlayered Universe**, embeds a page from unlayered3d.com. It is on by default and fetches nothing until you open it. To remove it, set `universe_app_enabled` to `false` in `Unlayered Slicer.conf` while the app is closed.

A page talks to the slicer only through a documented bridge with per-tab permissions. The bridge is specified in [WEB_APP_BRIDGE.md](WEB_APP_BRIDGE.md), and there is an example page in [resources/web/webapp-example](resources/web/webapp-example).

## Removed from OrcaSlicer

- **Orca Cloud** sign-in and preset sync.
- **Online vendor profile updates.** Profiles come with the app.
- **Bambu Lab cloud features:** cloud sign-in, cloud printing and account binding. Bambu Lab printers are used over the local network (LAN mode, with an access code), which needs the Bambu Lab network plug-in. The plug-in is optional: the first-run setup offers it, and it can be installed later from **Preferences ▸ Online**.

## Importing from other slicers

**File ▸ Import ▸ Import Settings from Other Slicers** (also offered during setup) reads the printers, filaments and your own presets of OrcaSlicer, PrusaSlicer, Bambu Studio, Creality Print, SuperSlicer, ElegooSlicer, Anycubic Slicer Next, Snapmaker Orca, QIDI Studio, Flash Studio, PhrozenOrca and FLSUN Slicer from their folders on this computer. Nothing in those apps is changed, and API keys and passwords are never copied. The home page's **Recently opened** list also shows those apps' recent projects, with a filter per app. All of this stays on your computer.

## Stealth mode

**Stealth mode** is off by default and set in **Preferences**. It hides the remaining cloud account settings, and stops the account sign-in check and the printer configuration offer.

## Network access

These connections are not affected by stealth mode unless noted:

- **At launch:** the app checks for a new version at `https://unlayered3d.com/slicer/latest.json`. You can also check manually with **Help ▸ Check for Updates**. Builds whose version ends in `-dev` skip the automatic check.
- **Updates:** downloading an update needs an Unlayered account. **Help ▸ Sign in to Unlayered** opens unlayered3d.com in your browser; the slicer keeps the resulting sign-in token in the system credential store and sends it only to unlayered3d.com. Signed in, a new version downloads in the background and installs on **Help ▸ Restart to Update** or the next time the app closes. On macOS this works when the app is in *Applications* (or *Applications* in your home folder); a copy anywhere else opens the download page instead.
- **Usage diagnostics:** only if you choose to share them. First-run setup (or the first launch after updating from 0.1.5 or earlier) asks **Keep my data private** or **Share usage diagnostics**; nothing optional is sent before you answer. Shared diagnostics say which features are used, slicing results and errors, crashes, and basic system details (operating system, processor cores, memory, language), sent to `https://unlayered3d.com/api/slicer/telemetry` with a random id that is not linked to you or your account. Models, G-code, file and object names, and the names of your own presets are never sent. The site also derives an approximate country from the request (the IP address itself is not stored). The optional **Slicing improvement program**, a separate box under Share, adds early access to new features and lets that data be used to develop, train, test, evaluate, and improve AI, CAD, slicing, software, manufacturing, and hardware systems. Change either in **Preferences ▸ Online**, or set the `UNLAYERED_NO_TELEMETRY` environment variable to switch diagnostics off for every user of the computer. [USAGE_DIAGNOSTICS.md](USAGE_DIAGNOSTICS.md) lists every event and field that is sent, and how long it is kept; the bundled Slicer Privacy Policy covers the same data.
- **Crash reports:** if you do not share usage diagnostics, the launch after the app closed unexpectedly asks whether to send that one crash report, and sends it once only if you agree.
- **Universe tab:** loads its page from unlayered3d.com when opened.
- **Printer updates:** the Firmware and Config buttons, and the once-per-session update offer, read release information from `https://unlayered3d.com/printer-updates-api`. Configuration releases need your Unlayered account; with stealth mode on, only firmware is checked.

If you install the Bambu Lab network plug-in, it is downloaded from Bambu Lab's servers.

# Getting it

Installers are provided to Unlayered 3D customers. There are no public binary releases, so everyone else builds from source.

Unlayered's own builds are Windows x64 and macOS (universal: Apple silicon and Intel). The Linux build scripts are carried over from OrcaSlicer and use the renamed targets, but they are not regularly tested.

# How to build

The build follows OrcaSlicer's layout. The dependencies are built once from `deps/`, then the application is configured against them. The [OrcaSlicer build guide](https://www.orcaslicer.com/wiki/how_to_build) applies; the steps below are the short version.

## Windows

```bat
build_win.bat --install-vs ide
```

That sets up a new machine with CMake, Perl, Git and Visual Studio. Restart the shell afterwards, then build the dependencies and the slicer:

```bat
build_win.bat -ds
```

When the script finishes, it prints where the executable is. `build_win.bat -h` lists every option.

## Linux

```shell
./build_linux.sh -u      # install system dependencies (asks for sudo)
./build_linux.sh -dsi    # dependencies, slicer and AppImage
```

A Flatpak manifest is at `scripts/flatpak/com.unlayered3d.UnlayeredSlicer.yml`; build it with `build_flatpak.sh`.

## macOS

```shell
./build_release_macos.sh -a universal
```

`-h` lists the options. Use `-d` or `-s` to build only the dependencies or only the slicer.

## Bundled firmware

A Windows installer can carry a printer firmware bundle in `resources/firmware/`, as described in [its README](resources/firmware/README.md). It is optional. Without it, the firmware updater uses the GitHub releases alone.

# Klipper note

If you print through Klipper, add the following to your `printer.cfg`:

```gcode
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```

# Background

Unlayered Slicer is a fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) by SoftFever and the OrcaSlicer contributors. OrcaSlicer is itself based on [Bambu Studio](https://github.com/bambulab/BambuStudio), which originated from [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research and [SuperSlicer](https://github.com/supermerill/SuperSlicer), both forks of [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community. Thanks to all of them.

# License

Unlayered Slicer is licensed under the GNU Affero General Public License, version 3, the same license as OrcaSlicer, Bambu Studio and PrusaSlicer. See [LICENSE.txt](LICENSE.txt).

- The example page in `resources/web/webapp-example/` is MIT licensed, so that web applications can start from it freely.
- Firmware images bundled with an installer are separate programs under their own licenses. RepRapFirmware is GPLv3, and its source is at [Unlayered3D/RepRapFirmware](https://github.com/Unlayered3D/RepRapFirmware).

The Unlayered 3D name and logo are trademarks of Unlayered 3D and are not covered by the AGPL license.
