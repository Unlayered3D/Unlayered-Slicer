# Centauri Carbon: Fusion check of nonplanar height

Date: 12 October 2026. Evidence: live Fusion MCP inspection of the saved CC assembly, with calculations performed on the returned geometry. **Result: vertical geometric bounds measured; maximum safe nonplanar object height remains unestablished.**

## What was inspected

The active document was CC, version 1, file identifier `urn:adsk.wipprod:dm.lineage:9tJ8boa0TrC7UnYaomqjzw`. Description: `Imported from ad60f0e0-0c34-4948-a98b-752709e89f59.f3z`. It contains 2,160 occurrences, 10,675 body records across the full assembly, 650 XY-assembly bodies, and 252 toolhead bodies. The imported file is not independently tied to a particular OpenCentauri repository commit. The document was saved and unmodified before the inspection; every measurement script used the MCP read-only context. No joint was driven, geometry changed, or save issued.

## Nozzle and build-surface references

The nozzle datum is the centroid of its lowest horizontal terminal annular face. The top build surface is the upper large horizontal face of `CC_z_azis v12:1+Bed_assm v33:1+Bed:1+buildplate2:1`.

| Quantity | Value | Evidence and qualification |
|---|---:|---|
| Nozzle-tip world X | -160.120000 mm | Live BRep terminal-face centroid |
| Nozzle-tip world Y | 161.350000 mm | Live BRep terminal-face centroid |
| Nozzle-tip world Z | 289.025000 mm | Live BRep terminal-face centroid |
| Build-surface world Z | 119.724623 mm | Live BRep horizontal-face centroid |
| Current nozzle-to-bed vertical separation | 169.300377 mm | Coordinate difference; native face-distance value agrees within 0.000001 mm |

These numerical values come from CAD. The owner subsequently reported that the installed 0.4 mm toolhead dimensions and Z travel match CAD; no separate numerical physical endpoints or measurement precision were recorded. The decimal places preserve the calculation; they do not imply matching physical accuracy. The native distance routine returned witness points whose association with the bounded nozzle face was inconsistent. Only its numerical parallel-face separation was used as a cross-check; its returned witness points are not evidence of finite-face clearance.

The current 169.300 mm separation is the assembly's displayed pose. It is not a maximum height or a tested printer Z position.

## What the bed joint actually permits in CAD

`CC_z_azis v12 / Slider 11` has enabled limits of **-175.000 to +90.000 mm**, a span of **265.000 mm**. Its current slide value is -1.000 mm. Its slide direction is effectively `(0, 0, -1)`: increasing the joint value lowers the bed. The parent rotation is effectively the identity. Projecting the joint-origin separation onto the slide axis gives -131.000 mm, matching its -130.000 mm geometric offset plus the -1.000 mm current slide value. This confirms the coordinate mapping without changing the joint.

Let `s` be that CAD slider value in millimeters, and `H` the vertical nozzle-to-build-surface separation in millimeters. In this assembly:

`H = 169.300376989 + (s - (-1.000)) = 170.300376989 + s`

The raw joint limits therefore correspond to:

- At `s = -175`: **H = -4.699623 mm**. The build surface would be above the nozzle tip.
- At `s = +90`: **H = 260.300377 mm**.
- Nozzle-to-bed zero separation would occur at **s = -170.300377 mm**.

**260.300 mm is a joint-based upper bound, not a certified mechanical or firmware travel limit.** The negative endpoint shows that these assembly limits themselves permit a nozzle/bed overlap. No endpoint collision analysis or full moving-bed/frame sweep was completed, and no endpoint was driven in the live design.

The inspected stock `Elegoo Centauri Carbon 0.4 nozzle` preset separately specifies **256 mm printable height**. Printable height, physical machine travel, CAD joint displacement, and firmware Z zero are different quantities; this inspection does not establish their physical calibration.

## Nearby material and the gantry

The following live body bounds reproduce the earlier relative clearances:

| Obstruction | Lowest height above nozzle datum | Interpretation |
|---|---:|---|
| Low carriage/PCB fastener | 15.122920 mm | Local rigid obstruction |
| Carriage plate underside | 16.749990 mm | Local rigid obstruction |
| Lower X rod underside | 34.250000 mm | Gantry obstruction |
| Existing conservative gantry parameter | 14 mm | A conservative modeled proxy, requiring physical comparison |

These distances concern material beside or above the current nozzle position. **They do not impose a 14 mm or 34 mm maximum total object height.** An isolated shallow top on a taller pedestal can retain the same local head clearance, because its lower body extends downward toward the lowered bed. A tall adjacent wall or another previously printed object can still obstruct a lower nozzle position.

A conservative bounding-box screen found **zero overlaps** between a central 30 x 20 mm column at the current nozzle XY position and the remaining fixed hardware outside the toolhead and moving bed. The checked column ran from world Z=33.025 mm to 292.025 mm, corresponding to 256 mm below the nozzle and 3 mm above it. Non-overlapping body boxes exclude solid intersections for that static placement. This does not test the toolhead itself, its XY motion, the moving bed, flexible cables, or a deposited-history toolpath. The 3 mm extension is simply the extent of this illustrative column, not an approved nonplanar slope or clearance.

## Height budgets for an actual job

The controlling condition is that the highest real nozzle position anywhere in the complete job, with its required uncertainty allowance, stays inside physically verified travel. A part height cannot be substituted directly for that highest position.

The table evaluates an illustrative 2 mm printing lift, plus the inspected stock end-parking formula. It assumes the template's `max_layer_z` equals the actual highest deposited Z. These are arithmetic examples, not sliced jobs or approved printing heights.

| Highest deposited Z, mm | With a 2 mm lift, mm | Stock end parking, mm | Higher of those two moves, mm |
|---:|---:|---:|---:|
| 10 | 12 | 100 | 100 |
| 25 | 27 | 100 | 100 |
| 50 | 52 | 100 | 100 |
| 100 | 102 | 100 | 102 |
| 200 | 202 | 250 | 250 |
| 250 | 252 | 256.5 | 256.5 |
| 256 | 258 | 256.5 | 258 |

The stock end template parks at Z100 for `max_layer_z <= 100`; otherwise it parks at `min(max_layer_z + 50, printable_height + 0.5)`. It also includes a `max_layer_z + 0.5` move. Firmware macros and the actual exported result remain to be inspected. A 256.5 mm scripted command is not proved physically permissible by a 256 mm printable-height declaration.

For the restricted example of a verified 256 mm usable nozzle range and only a 2 mm lift, **254 mm would be the arithmetic ceiling before any further allowance or other motion**. Subtracting an illustrative 0.5 mm additional Z reserve would give 253.5 mm for that partial budget. Neither value is a recommended setting; the Z uncertainty is unmeasured, and the stock end routine may require more headroom. The existing 0.5 mm side-clearance target must not be mistaken for a measured Z-position uncertainty.

A 100 mm deposited height requires at least Z102 for a 2 mm lift before adding allowance. It fits the raw CAD joint range, but a 100 mm checked-nozzle ceiling would reject that lift. The upstream code prepared for the pull requests excludes custom startup/end scripts from its print-height checker, so those scripts need separate verification.

## Attempted native clearance check

A native full-toolhead body-to-transient-plane measurement was attempted in the previous front-right limiting direction. Fusion returned `RuntimeError: 3 : measurement failed` on the first required measurement. No native body-to-plane clearance, interference result, or 4/5-degree native acceptance is claimed. The earlier mesh-based angle report remains separate evidence; it is not promoted to a new native result by this inspection.

## What would establish a usable nonplanar height

1. Use the owner-reported agreement of the installed 0.4 mm toolhead dimensions and Z travel with CAD as physical evidence. Record numerical endpoints and measurement precision before extending the tested height range, and include the changing cable/tube envelope and operating motion.
2. Check every generated deposition and travel segment, along with start/end scripts and firmware-controlled movements, against the complete toolhead and everything printed so far. Include the moving bed and all relevant XY poses.
3. Use the verified travel range and those actual movements to calculate the height budget for the particular model.
4. Physically commission progressively taller versions of the same simple geometry, first under observation and then at the intended speed. Repeat before extending the validated scope.

The proposed 10, 25, 50, and 100 mm stages are test stages, not established safe heights. The upstream port's simple automatic-ramp path currently has a 10 mm experimental part restriction; the authored-plane-program path uses different height guards. A taller trial must use a supported path and pass its software bounds before physical commissioning. A successful test establishes evidence for its geometry, nozzle, material, speed, bed region, and height; it does not establish a universal safe height for arbitrary models.

**Decision:** The Fusion check supports a CAD-based vertical budget and confirms the local obstruction heights. It does not support raising the limit on the basis of a universal safe nonplanar object height.

## Evidence files

- `live_height_inventory.json`: complete current body bounds, planar-face references, joint limits, and document identity.
- `joint_mapping.json`: joint geometry, orientation, and offset readback.
- `native_bed_distance.json`: numerical face separation and explicit witness-point qualification.
- `native_clearance_response.json`: the failed body-to-plane measurement.
- `height_results.json`: calculated bounds and partial height-budget examples.
- `initial_screenshot_response.png`: current Fusion assembly view.
- `open_documents_response.json` and `final_open_documents_response.json`: saved/unmodified document state before and after.

## Preset integration

The experimental profiles retain their 100 mm software nozzle-Z ceiling and 14 mm relative gantry proxy. No 254 mm, 253.5 mm or 260.3 mm safe object-height setting is introduced. The first simple-ramp trial stays below the engine's 10 mm scope. Current upstream checking excludes custom startup/end scripts; a complete job budget must review them separately. The owner's physical report is not evidence of 0.2 mm nozzle interchangeability or a measured dynamic gap.
