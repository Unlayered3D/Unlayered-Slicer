# Multiplanar Calibration Totem (MCT-1)

A parametric test part for multiplanar (tilted flat layer) printing. One 76 × 76 × 28 mm print shows, side
by side, how a printer and slicer handle tilt, overhangs, curved tops, transitions and branched shapes. It
can be used to compare slicing strategies on the same geometry. The current automatic planner
supports fixed-nozzle 3-axis Virtual printers. It deliberately keeps rotary and belt configurations
planar. A rung retained as planar can reflect the planner's benefit threshold, plane budget or
interference with other features; it does **not** establish a physical clearance limit.

Generate it (and every module as its own STL, plus a manifest of every angle):

```
pip install -r tools/multiplanar_test_totem/requirements.txt
python3 make_totem.py --out mct1 --preview
```

## Features and what to look for

| Feature | Where | What it tests | What to look at |
|---|---|---|---|
| Tilt ladders | four rows, one per side | planner decisions and surface finish at known slopes | check which rungs actually use tilted layers in preview, then compare their finish |
| Overhang shelves | 8 shelves on the tower, 15–55° | support threshold vs available tilt | which shelves needed support planar, which lost it when tilted, sag on the underside |
| Dome cap | top of the tower | curved-surface finish | terrace bands, planar vs tilted |
| Flat reference | base top, corner cube (+X−Y) | "keep planar" decisions; dimensional check | should stay planar; cube 10.00 mm |
| Arch | corner −X+Y | crown bridge/overhang, curved spine | sag at the crown; skeleton-cut behaviour |
| Fork | corner +X+Y | branched geometry | branch surfaces and joints |
| Seam witness | corner −X−Y | seam position across a transition | whether the seam jumps between planar and tilted sections |

Default rung angles (top slope from horizontal): **2, 3, 4, 5, 7, 10, 15, 25°** on every side. The ramps rise
away from the tower, so layers parallel to the +X ramp climb toward +X and their upward normal leans
toward −X. Surface rise and build-normal azimuth are opposite directions. Clearance also depends on
the nearby tower, other modules, supports and travel paths; the four ladders alone cannot measure
the toolhead envelope.

Default shelf undersides (from horizontal): **15, 20, 25, 30, 35, 40, 45, 55°**, two per tower face, in
order +X (low, high), +Y, −X, −Y.

## Re-making it for another printer

Every angle is a parameter. Pick rungs that bracket the expected limit:

| Printer | Suggested `--tilts` |
|---|---|
| fixed-nozzle 3-axis with a small limit (e.g. 5°) | `1,2,3,4,5,6,8,10` |
| 3-axis with a slim, high-clearance hotend | `2,4,6,8,10,12,15,20` |
| 5-axis / rotary | `5,10,15,20,30,40,50,60` |
| belt printer | default, printed with the ladders along and across the belt |

The rotary and belt rows describe geometry variants, not automatic-planner support. On a printer
with a 5° configured limit, steeper rungs may still be printed with ordinary planar layers. Do not
raise the machine's tilt limit to match the model. The default planner proposes at most two new
global cut planes, so it cannot independently align every rung of this combined model.

`--scale` scales the whole part, `--only tilt_ladders` keeps the low ramps and base for an initial surface
test, and `--overhangs` changes the shelf angles. The base is always kept; selecting `shelves` also keeps
the tower they attach to. The manifest reports only the features included in the exported subset.

## Recording results

`MCT-1_manifest.json` lists every rung and shelf with its side and angle. For each, note: tilted or planar
(from the slicer preview), visible steps (yes/no), support needed (yes/no), defects. The rung where a
side switches from tilted to planar records a **planner decision for this model and profile**.
Separate tests and physical measurements are needed to establish clearance. A completed print
validates only its particular geometry, settings and paths, not all prints at that slope.

For a Centauri Carbon first test, retain the provisional 5° machine limit and use
`--tilts 1,2,3,4,5,6,8,10`. Slice the part yourself, inspect the cut transitions, support foundations
and all travel paths, and retain supports wherever needed. This STL contains geometry only; it
does not contain cut planes or a printer profile.

The part is original geometry made for this purpose; it is not derived from any existing benchmark model.
