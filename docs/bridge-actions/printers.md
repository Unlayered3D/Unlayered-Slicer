# Printer actions (Device tab, Print / Send)

All behind the printer permission (`<slot>_allow_printer`, off by default). The methods and their params
are in WEB_APP_BRIDGE.md 4.6; this page says how they map onto the slicer and what is left out.

| Action | Typed method | GUI it follows |
|---|---|---|
| `printer.list` | `slicer.listPrinters` | Device tab printer list |
| `printer.status` | `slicer.printerStatus` | StatusPanel readouts, AMS panel, HMS / Assistant, error dialog buttons |
| `printer.control` | `slicer.printerControl` | StatusPanel controls, fan popup, AMS Load / Unload / Retry / RFID, AMS drying window, DeviceErrorDialog |
| `printer.print` | `slicer.print` | Print plate / Print all, SelectMachineDialog options, PrintHostSendDialog |
| `printer.send` | `slicer.sendToPrinter` | Send / Send all (SendToPrinterDialog) |
| `printer.select` | `slicer.selectPrinter` | MonitorPanel::on_select_printer |
| `printer.jobs`, `printer.cancelJob` | `slicer.printHostJobs`, `slicer.cancelPrintHostJob` | Print host upload queue (Ctrl+J) |
| `printer.exportGcode` | `slicer.exportGcode` | Export G-code / sliced file |
| `printer.fetchBedMesh` | catalog action | Reload Bed Mesh from Printer |

## How the rules are kept

- `WebAppPrinters.cpp` plans every control against the live printer object: the same checks as
  StatusPanel (homed axes, 170 C for the extruder, printing / paused / filament-change states, air-duct
  mode, the printer's temperature limits, capability flags) and the same commands. It plans again after the
  confirmation, so a printer that changed meanwhile is not sent a stale command.
- `Actions/PrinterRules.hpp` holds the pure rules, each naming the GUI code it mirrors; tested in
  `tests/slic3rutils/test_actions_printers.cpp`.
- Print-dialog options are applied after the dialog loads the printer's saved choices and are never saved
  as the person's defaults; host upload presets are not kept as the recent folder / group / storage.

## Not covered, and why

- Part skip: the object ids come from the printer's file system over the camera tunnel (PartSkipDialog's
  PrinterFileSystem download); there is no headless path yet (manifest todo).
- Printer storage files (list, download, delete, print from storage): same PrinterFileSystem dependency;
  todo.
- Camera live view, snapshot, recording: camera images are sensitive; needs an explicit product decision.
- AMS mapping in the print dialog: the person maps in the dialog; a validated `mapping` param is todo.
- AMS sync and printer nozzle / AMS sync (sidebar): not security matters, but both run through modal
  dialogs (SyncAmsInfoDialog, SyncNozzleAndAmsDialog) and need a dialog-free core first; todo.
- Deferred (printer settings and maintenance): firmware, multi-device, calibration dialogs, AMS settings on
  the printer, print / safety / parts options dialogs.
