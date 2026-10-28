# Centauri Carbon experimental nonplanar presets

These opt-in profiles describe a stock-looking CC1 assembly with an upright nozzle and ordinary XYZ motion. They do not enable nozzle rotation. The ordinary Centauri machine and process parameters are unchanged. Select the profiles containing **Experimental nonplanar** explicitly; stock filament compatibility lists include the new machine names without changing any material calibration.

| Nozzle | Printer | Process |
| --- | --- | --- |
| 0.4 mm | Elegoo Centauri Carbon 0.4 - Experimental nonplanar 5deg | 0.20mm @Elegoo CC 0.4 - Experimental nonplanar |
| 0.2 mm | Elegoo Centauri Carbon 0.2 - Experimental nonplanar 5deg | 0.10mm @Elegoo CC 0.2 - Experimental nonplanar |

Both inherit the corresponding stock nozzle profile and stock process. Neither includes a newly calibrated filament profile. The 0.2 mm exterior envelope and installed tip position have **not** been confirmed against the inspected 0.4 mm assembly. Do not infer interchangeability from bore diameter. Automatic planning and curved transition engine changes are separate contributions; these settings alone do not create curved layers or remove supports.

## Source and uncertainty

The measurements were made on 10 October 2026 from a live imported OpenCentauri assembly in Fusion. The inspected XY assembly included 650 BRep bodies, including 252 toolhead bodies; all visibility states were included. The refined extraction contained 1,471,211 triangles, of which 808,425 belonged to the toolhead. The nozzle tip is the origin, +X is right, +Y back and +Z up, as determined from assembly features. Physical axis verification remains pending.

The [OpenCentauri CAD repository](https://github.com/OpenCentauri/cad) was observed at `c63a80d3026bf582296c6372a716c6e80fd5bbae`. The live imported assembly could not be proven to match that revision. It is a measured CAD representation, not a manufacturer-certified stock collision model. No third-party CAD is redistributed here. The numerical record is [tools/centauri_nonplanar/measurements.json](../../../tools/centauri_nonplanar/measurements.json); the [original public measurement package](https://github.com/Anteriousis/centauri-carbon-unlayered-presets) contains the fuller report and section figures.

Low cowling meshes used a requested 0.002 mm surface deviation and 1 mm maximum edge; other surfaces used 0.020 mm deviation and 2 mm edge. Approximately ±0.05° describes numerical angular uncertainty, **not physical uncertainty**. On 12 October the owner reported that the installed 0.4 mm toolhead dimensions and Z travel match CAD. This is owner-reported physical agreement; numerical physical endpoints, measurement precision, hot deformation, flexible-wire sweeps and dynamic running gap were not recorded. The 0.2 mm exterior geometry remains unconfirmed.

## Collision geometry

Angles describe the uphill direction of a printed surface. For a surface point (x,y,h), its directional horizontal reach is d. The nominal plane collision angle is atan2(h,d). A desired normal gap c requires `h cos(theta) - d sin(theta) >= c`. The complete off-axis toolhead mesh was evaluated, not just a central cross-section.

| Uphill direction | Limiting geometry | Contact point x,y,h (mm) | d (mm) | Nominal angle | With 0.5 mm normal gap |
| --- | --- | --- | ---: | ---: | ---: |
| +X / right | Front magnetic cowling / part-cooling duct | 24.031, 0.640, 3.770 | 24.031 | 8.917° | 7.739° |
| -X / left | Front magnetic cowling / part-cooling duct | -17.356, 9.640, 3.697 | 17.356 | 12.024° | 10.410° |
| +Y / back | Rear cowling lower outer corner | 32.177, 44.401, 13.060 | 44.401 | 16.391° | 15.772° |
| -Y / front | Front magnetic cowling / part-cooling duct | -15.820, -34.037, 4.197 | 34.037 | 7.029° | 6.194° |

The 5° full-mesh all-azimuth plane screen gave a worst nominal **non-contact** gap of 0.583811 mm. This leaves only about 0.084 mm above a 0.5 mm target before unknown physical error. It is not a measured running gap. The original conservative starting recommendation was 4°; 5° was the owner's requested trial. The simplified four-face envelope has a worst-direction cap near 4.565151°, so some 5° diagonal proposals should be rejected. Do not enlarge the envelope angles to force them through.

The intended nozzle contact region was excluded from those macro clearance angles. Its finite flat tip and interaction with newly deposited material require separate checks. In particular, these settings cannot provide a universal 0.5 mm air gap under the extrusion-contact region.

## Preset values

| Setting | Value | Source and limits |
| --- | ---: | --- |
| Enable non-planar printing | ON | Explicit opt-in trial profile |
| Printer type | Virtual | Fixed upright nozzle, three-axis kinematics |
| Belt printing | OFF | Explicit |
| Hotend envelope radius | 125 mm | Spherical cutoff, not horizontal width: full CAD head radius 123.254 mm; coordinate-expanded radius 123.911 mm. Flexible motion sweep unknown. Historical config key is `nonplanar_hotend_cone_height`. |
| Side angle +X | 7° | Rounded down from CAD gap calculation |
| Side angle -X | 10° | Rounded down from CAD gap calculation |
| Side angle +Y | 15° | Rounded down from CAD gap calculation |
| Side angle -Y | 6° | Rounded down from CAD gap calculation |
| Gantry height | 14 mm | Conservative proxy below a carriage/PCB fastener at 15.123 mm; carriage plate 16.750 mm, lower X rod underside 34.250 mm. Verify the installed hardware. |
| Maximum tilt | 5° | Trial ceiling, further restricted by directional/diagonal limits; nozzle does not rotate |
| Maximum total Z | 100 mm | Software nozzle-Z ceiling; current upstream print-height checking excludes custom startup/end scripts. **Not a verified 100 mm nonplanar object height.** |
| Approach / departure | 2 mm / 2 mm | Trial allowances requiring inspection of every emitted path |
| Print sequence | By group | Existing multiplanar workflow; deposition order must be checked |

Commissioning models in the original trial were below 10 mm. A historical 10 mm total Z cap rejected stock end parking, which is why the exported preset uses 100 mm. Maximum feasible nonplanar object height remains undetermined: it depends on verified machine travel, staging/lifts, adjacent printed geometry, gantry clearance and cable/tube sweeps. Stock nominal build height is 256 mm; this alone is insufficient to establish a nonplanar height setting.

The [12 October live Fusion height check](../../../tools/centauri_nonplanar/HEIGHT_CHECK_2026-10-12.md) measured a 169.300377 mm tip-to-bed gap at the displayed pose and mapped the enabled bed-slider limits to a raw upper separation of 260.300377 mm. Those joint limits also permit overlap at the other endpoint. The owner reports both the installed 0.4 mm toolhead dimensions and Z travel match CAD. These observations support the geometric budget but do not make a universal safe object height: a 100 mm deposited top plus a 2 mm lift requires at least Z102, and stock parking/macros must be reviewed separately. The 14 mm gantry parameter measures relative head clearance, not maximum object height. No numerical clearance or height limit was raised.

## Physical trial record

The owner reported no nozzle, duct or shroud contact noticed in one fast-moving trial, while noting that spare clearance was difficult to judge. This is not a measurement or clearance certificate. The exact printed project, firmware and installed hardware were not confirmed. Reported defects included nonplanar supports starting in midair, excessive support adhesion, a transition gap, stringing and seams changing position between planar and inclined sections. The broken coupon was **planar**; the owner explicitly corrected the apparent block deformation to a seam mismatch, **not warping**. Photo 7 was planar and photo 6 inclined. Neither usable support savings nor overall finish improvement was established. The 0.2 mm preset remains physically untested.

Before another trial, verify the installed nozzle datum and protrusion, sock fit, the full duct/cowling rim including off-axis corners, leads, magnets and fasteners; confirm the coordinate directions and travel limits, and inspect the changing cable/chain/tube envelope. Check continuous extrusion, every travel and approach/departure against all material printed earlier, including supports and nearby objects. Resolve any collision or height rejection from the actual geometry/path. Test material stringing separately and evaluate a simple, bonded shallow ramp before another support comparison.

These records support an experimental CAD proposal and one physical observation with defects. They do not establish that the Centauri Carbon is safe for nonplanar printing.
