#!/usr/bin/env python3
"""Multiplanar Calibration Totem (MCT-1) -- a parametric test part for multiplanar / tilted-layer slicing.

One print shows, side by side on one plate:

  * TILT LADDERS   four rows of ramps, one per side (+X, -X, +Y, -Y), each rising away from the centre at
                   graded angles. Which rungs use tilted layers depends on the planner, the available
                   planes and the configured machine limits. A planar rung is not a measurement of
                   the printer's collision limit.
  * OVERHANG SHELVES  eight shelves on the central tower, undersides at graded angles from horizontal, to
                   see where a small tilt removes support and where it cannot.
  * DOME CAP       a shallow spherical cap on the tower: curved-surface finish, planar vs tilted.
  * FLAT REFERENCE the base plate top and the corner cube: planar printing should stay planar here.
  * ARCH           a tube arch in one corner: bridge / overhang at its crown, curved spine for skeleton planes.
  * FORK           a trunk with two leaning branches in another corner: branched geometry.
  * SEAM WITNESS   a ribbed cylinder: seam position on both sides of a planar-to-tilted transition.

Orientation: the FORK sits in the +X+Y corner; the ARCH in -X+Y; the SEAM WITNESS in -X-Y; the CUBE in +X-Y.
Rungs are ordered from shallowest to steepest going counter-clockwise around the tower (seen from above).

Every rung, shelf and module is generated from the parameters below, so the same part can be re-made for a
printer with a different tilt range (e.g. --tilts 1,2,3,4,5,6,8,10 for a tight 3-axis printer, or
--tilts 5,10,15,20,30,40,50,60 for a 5-axis machine). Each module is also written as its own STL so a
subset can be printed.

    python3 make_totem.py --out out_dir [--scale 1.0] [--tilts ...] [--overhangs ...] [--preview]

Requires: numpy, trimesh, manifold3d (booleans), scipy; matplotlib for --preview.
"""

import argparse
import json
import math
import os

import numpy as np
import trimesh
from trimesh.creation import box, cylinder, icosphere

DEFAULT_TILTS = [2, 3, 4, 5, 7, 10, 15, 25]          # deg, ramp top from horizontal; 8 rungs per side
DEFAULT_OVERHANGS = [15, 20, 25, 30, 35, 40, 45, 55]  # deg, shelf underside from horizontal; 8 shelves
MODULE_NAMES = {"base", "tower_dome", "shelves", "tilt_ladders", "cube", "arch", "fork", "seam_witness"}


def wedge(x0, x1, y0, y1, z0, h_in, slope_deg):
    """Block over [x0,x1]x[y0,y1] from z0, top rising along +X from z0+h_in at x0 at slope_deg."""
    t = math.tan(math.radians(slope_deg))
    h_out = h_in + (x1 - x0) * t
    v = np.array([
        [x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0],
        [x0, y0, z0 + h_in], [x1, y0, z0 + h_out], [x1, y1, z0 + h_out], [x0, y1, z0 + h_in],
    ])
    f = np.array([
        [0, 2, 1], [0, 3, 2],          # bottom
        [4, 5, 6], [4, 6, 7],          # top
        [0, 1, 5], [0, 5, 4],          # -Y
        [1, 2, 6], [1, 6, 5],          # +X
        [2, 3, 7], [2, 7, 6],          # +Y
        [3, 0, 4], [3, 4, 7],          # -X
    ])
    m = trimesh.Trimesh(v, f, process=True)
    m.fix_normals()
    return m


def shelf(depth, width, z_root, overhang_deg, top_thick, root_inset=1.0):
    """Shelf sticking out along +X from x=0: underside rises at overhang_deg from horizontal going out,
    top is flat. Width centred on y=0."""
    t = math.tan(math.radians(overhang_deg))
    z_tip_under = z_root + depth * t
    z_inside_under = z_root - root_inset * t
    z_top = z_tip_under + top_thick
    v = np.array([
        [-root_inset, -width / 2, z_inside_under], [depth, -width / 2, z_tip_under], [depth, width / 2, z_tip_under], [-root_inset, width / 2, z_inside_under],
        [-root_inset, -width / 2, z_top], [depth, -width / 2, z_top], [depth, width / 2, z_top], [-root_inset, width / 2, z_top],
    ])
    f = np.array([[0, 2, 1], [0, 3, 2], [4, 5, 6], [4, 6, 7], [0, 1, 5], [0, 5, 4],
                  [1, 2, 6], [1, 6, 5], [2, 3, 7], [2, 7, 6], [3, 0, 4], [3, 4, 7]])
    m = trimesh.Trimesh(v, f, process=True)
    m.fix_normals()
    return m


def rot_z(m, deg):
    m = m.copy()
    m.apply_transform(trimesh.transformations.rotation_matrix(math.radians(deg), [0, 0, 1]))
    return m


def move(m, x, y, z):
    m = m.copy()
    m.apply_translation([x, y, z])
    return m


def tube_along(points, r, sections=24):
    """Solid tube along a polyline, as a union of cylinders and spheres (robust for booleans)."""
    parts = []
    for a, b in zip(points[:-1], points[1:]):
        a, b = np.asarray(a, float), np.asarray(b, float)
        seg = cylinder(radius=r, segment=np.array([a, b]), sections=sections)
        parts.append(seg)
    for p in points:
        parts.append(move(icosphere(subdivisions=2, radius=r), *p))
    return trimesh.boolean.union(parts, engine="manifold")


def build(p):
    s = p.scale
    plate = p.plate * s
    base_h = 2.0 * s
    tower = 20.0 * s
    tower_h = 22.0 * s
    z_base = base_h
    lap = 0.2 * s  # every module sinks this far into what it stands on, so the union is watertight

    modules = {}

    # Base plate: the flat planar reference.
    modules["base"] = move(box([plate, plate, base_h]), 0, 0, base_h / 2)

    # Central tower with dome cap.
    t = move(box([tower, tower, tower_h + lap]), 0, 0, z_base - lap + (tower_h + lap) / 2)
    cap_h = 4.0 * s
    R = ((tower / 2) ** 2 + cap_h ** 2) / (2 * cap_h)            # sphere through the face midpoints
    sphere = move(icosphere(subdivisions=5, radius=R), 0, 0, z_base + tower_h + cap_h - R)
    cap_box = move(box([tower, tower, cap_h + 2 * lap]), 0, 0, z_base + tower_h + cap_h / 2)
    cap = trimesh.boolean.intersection([sphere, cap_box], engine="manifold")
    modules["tower_dome"] = trimesh.boolean.union([t, cap], engine="manifold")

    # Overhang shelves: two per tower face, stacked, angles in order around the tower.
    shelf_depth, shelf_w, shelf_top = 7.0 * s, 8.0 * s, 1.6 * s
    levels = [z_base + 4.0 * s, z_base + 12.5 * s]
    shelves = []
    for i, ang in enumerate(p.overhangs[:8]):
        face, level = i // 2, i % 2
        offset = (-1 if level == 0 else 1) * 4.5 * s if True else 0
        sh = shelf(shelf_depth, shelf_w, levels[level], ang, shelf_top, root_inset=s)
        sh = move(sh, tower / 2, offset, 0)
        shelves.append(rot_z(sh, 90 * face))
    modules["shelves"] = trimesh.boolean.union(shelves, engine="manifold")

    # Tilt ladders: one row per side, ramps rising AWAY from the tower.
    r0, r1 = 22.0 * s, 36.0 * s                     # radial span of a rung
    span = 2 * 21.0 * s                             # tangential span of a row
    n = len(p.tilts)
    pitch = span / n
    rung_w = pitch * 0.78
    ladders = []
    for side in range(4):
        for j, ang in enumerate(p.tilts):
            y_c = -span / 2 + pitch * (j + 0.5)
            w = wedge(r0, r1, y_c - rung_w / 2, y_c + rung_w / 2, z_base - lap, 1.5 * s + lap, ang)
            ladders.append(rot_z(w, 90 * side))
    modules["tilt_ladders"] = trimesh.boolean.union(ladders, engine="manifold")

    c = 30.0 * s  # corner module centre offset

    # Corner +X+Y: fork (trunk + two branches leaning 35 deg outward along the diagonal's normal).
    trunk_top = z_base + 10.0 * s
    br = 2.0 * s
    d = np.array([1, -1, 0]) / math.sqrt(2)          # branches lean across the diagonal
    lean = math.radians(35)
    L = 9.0 * s
    tip_a = np.array([c, c, trunk_top]) + L * (math.sin(lean) * d + math.cos(lean) * np.array([0, 0, 1]))
    tip_b = np.array([c, c, trunk_top]) + L * (-math.sin(lean) * d + math.cos(lean) * np.array([0, 0, 1]))
    fork = [tube_along([[c, c, z_base - lap], [c, c, trunk_top]], 2.5 * s),
            tube_along([[c, c, trunk_top], tip_a.tolist()], br),
            tube_along([[c, c, trunk_top], tip_b.tolist()], br)]
    fork_m = trimesh.boolean.union(fork, engine="manifold")
    modules["fork"] = trimesh.boolean.intersection(
        [fork_m, move(box([30 * s, 30 * s, 60 * s]), c, c, z_base + 30 * s - lap)], engine="manifold")

    # Corner -X+Y: arch (semicircular tube) spanning the diagonal.
    arch_r, tube_r = 6.5 * s, 2.0 * s
    e = np.array([1, 1, 0]) / math.sqrt(2)
    centre = np.array([-c, c, z_base - lap])
    pts = []
    for k in range(0, 25):
        a = math.pi * k / 24
        pts.append((centre + arch_r * math.cos(a) * e + np.array([0, 0, arch_r * math.sin(a) + tube_r])).tolist())
    arch = tube_along(pts, tube_r, sections=20)
    modules["arch"] = trimesh.boolean.intersection(
        [arch, move(box([40 * s, 40 * s, 60 * s]), -c, c, z_base + 30 * s - lap)], engine="manifold")

    # Corner -X-Y: seam witness, a cylinder with 8 shallow vertical ribs.
    cyl = move(cylinder(radius=5.0 * s, height=14.0 * s + lap, sections=64), -c, -c, z_base - lap + (14.0 * s + lap) / 2)
    ribs = []
    for k in range(8):
        rib = move(box([1.2 * s, 1.0 * s, 14.0 * s]), 5.0 * s, 0, z_base + 7.0 * s)
        ribs.append(move(rot_z(rib, 45 * k), -c, -c, 0))
    modules["seam_witness"] = trimesh.boolean.union([cyl] + ribs, engine="manifold")

    # Corner +X-Y: 10 mm reference cube.
    modules["cube"] = move(box([10 * s, 10 * s, 10 * s + lap]), c, -c, z_base - lap + (10 * s + lap) / 2)

    if p.only:
        keep = {x.strip() for x in p.only.split(",") if x.strip()}
        # Shelves root into the tower, not the base. Preserve that foundation in a subset.
        if "shelves" in keep:
            keep.add("tower_dome")
        modules = {k: v for k, v in modules.items() if k in keep or k == "base"}
    totem = trimesh.boolean.union(list(modules.values()), engine="manifold")
    return totem, modules


def manifest(p, totem, modules):
    s = p.scale
    sides = ["+X", "+Y", "-X", "-Y"]
    rungs = []
    for side_i, side in enumerate(sides):
        for j, a in enumerate(p.tilts):
            rungs.append({"side": side, "rung": j + 1, "top_slope_deg": a,
                          "layers_climb_toward": side,
                          "note": "build normal leans toward the tower; layers climb outward"})
    shelves = [{"face": sides[i // 2], "level": ["low", "high"][i % 2], "underside_deg_from_horizontal": a}
               for i, a in enumerate(p.overhangs[:8])]
    corners = {"+X+Y": "fork", "-X+Y": "arch", "-X-Y": "seam_witness", "+X-Y": "cube"}
    return {
        "name": "Multiplanar Calibration Totem (MCT-1)",
        "scale": s,
        "bounds_mm": np.round(totem.bounds, 3).tolist(),
        "volume_mm3": round(float(totem.volume), 1),
        "watertight": bool(totem.is_watertight),
        "connected_bodies": int(totem.body_count),
        "modules": list(modules),
        "tilt_rungs": rungs if "tilt_ladders" in modules else [],
        "overhang_shelves": shelves if "shelves" in modules else [],
        "corners": {side: module for side, module in corners.items() if module in modules},
    }


def preview(totem, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    fig = plt.figure(figsize=(10, 5), dpi=130)
    light = np.array([0.4, -0.5, 0.75]); light /= np.linalg.norm(light)
    shade = np.clip(totem.face_normals @ light, 0.15, 1.0)
    colors = np.stack([0.85 * shade, 0.55 * shade, 0.25 * shade, np.ones_like(shade)], axis=1)
    for i, (elev, azim) in enumerate([(32, -55), (60, 35)]):
        ax = fig.add_subplot(1, 2, i + 1, projection="3d")
        ax.add_collection3d(Poly3DCollection(totem.triangles, facecolors=colors, edgecolors="none"))
        b = totem.bounds
        mid = b.mean(axis=0); r = (b[1] - b[0]).max() / 2
        ax.set_xlim(mid[0] - r, mid[0] + r); ax.set_ylim(mid[1] - r, mid[1] + r); ax.set_zlim(0, 2 * r * 0.6)
        ax.set_box_aspect((1, 1, 0.6))
        ax.view_init(elev=elev, azim=azim)
        ax.set_axis_off()
    fig.suptitle("Multiplanar Calibration Totem (MCT-1)")
    fig.tight_layout()
    fig.savefig(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="mct1_out")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--plate", type=float, default=76.0, help="base plate size in mm before scaling")
    ap.add_argument("--tilts", default=",".join(map(str, DEFAULT_TILTS)))
    ap.add_argument("--overhangs", default=",".join(map(str, DEFAULT_OVERHANGS)))
    ap.add_argument("--only", default="", help="comma list of modules to keep (base always kept)")
    ap.add_argument("--preview", action="store_true")
    p = ap.parse_args()
    p.tilts = [float(x) for x in p.tilts.split(",") if x]
    p.overhangs = [float(x) for x in p.overhangs.split(",") if x]

    if not math.isfinite(p.scale) or p.scale <= 0 or not math.isfinite(p.plate) or p.plate <= 0:
        ap.error("--scale and --plate must be finite positive numbers")
    if not p.tilts or any(not math.isfinite(x) or not 0 <= x < 90 for x in p.tilts):
        ap.error("--tilts must contain at least one finite angle from 0 (inclusive) to 90 (exclusive)")
    if not p.overhangs or len(p.overhangs) > 8 or any(not math.isfinite(x) or not 0 <= x < 90 for x in p.overhangs):
        ap.error("--overhangs must contain 1 to 8 finite angles from 0 (inclusive) to 90 (exclusive)")
    unknown = {x.strip() for x in p.only.split(",") if x.strip()} - MODULE_NAMES
    if unknown:
        ap.error("unknown --only module(s): " + ", ".join(sorted(unknown)))

    os.makedirs(p.out, exist_ok=True)
    totem, modules = build(p)
    totem.export(os.path.join(p.out, "MCT-1_totem.stl"))
    for name, m in modules.items():
        m.export(os.path.join(p.out, f"MCT-1_module_{name}.stl"))
    info = manifest(p, totem, modules)
    with open(os.path.join(p.out, "MCT-1_manifest.json"), "w") as f:
        json.dump(info, f, indent=1)
    if p.preview:
        preview(totem, os.path.join(p.out, "MCT-1_preview.png"))
    print(json.dumps({k: info[k] for k in ("bounds_mm", "volume_mm3", "watertight")}))


if __name__ == "__main__":
    main()
