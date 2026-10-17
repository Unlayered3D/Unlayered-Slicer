# Curved transition geometry prototype

This directory contains the standalone geometry experiment and tests. The probe produces geometry CSV/JSON, never print G-code. The separate native engine integration is described in [ENGINE.md](ENGINE.md); its release status must be checked against the actual build and test results.

The current auto-plane backend uses one affine frame per section. A curved transition must replace that assumption, rather than append extra independent cut-plane sections or change Z only after exporting G-code. In particular, the Centauri's post-processing path does not update Preview after script changes. A post-processing shortcut would therefore make the reviewed Preview differ from the print output.

## The mapping

For a roof that is a single-valued function `T(x,y)`, choose a flat starting height `z0` and a reference roof height `R`. The flat slicing coordinate is:

```
t = clamp((z - z0) / (T(x,y) - z0), 0, 1)
S(t) = 10 t^3 - 15 t^4 + 6 t^5
w = z - (T(x,y) - R) S(t)
```

Below `z0`, the mapping is exactly identity. The roof maps to `w = R`; the inverse therefore preserves its physical shape. The first and second derivatives of `S` vanish at both boundaries. Because transition depth depends on XY, intermediate constant-w surfaces are actually curved even when the final roof is a flat slope.

This construction is restricted to roof height fields, not arbitrary undercuts, branching solids or disconnected geometry. Mesh extraction, domain verification and geometric integration remain required.

## Analytical checks

Let `q = (T-R)/(T-z0)`. Require `q <= 1/3` everywhere. With `max S' = 15/8`, the forward vertical derivative `J = 1 - q S'` stays strictly positive. Layers cannot cross at a fixed XY point. The induced slope multiplier is:

```
A = (S - q t S') / (1 - q S')
```

For `q <= 1/3`, `0 <= A <= 1`. For positive q this follows from:

```
3 S - t S' = 3 t^4 (5 - 4t) >= 0
1 - S - (S'/3)(1-t) = (1-t)^4 (1+4t) >= 0
```

For nonpositive q, each inequality follows directly from `0 <= S,t <= 1`. Thus a verified Lipschitz bound on the roof also bounds all intermediate layer slopes. These mathematical statements require true domain-wide roof bounds; sample checks alone do not prove those bounds for an arbitrary mesh or callback.

Global bounds on J give vertical layer gaps between `step/Jmax` and `step/Jmin`. The lower gap divided by `sqrt(1 + max_slope^2)` also bounds distance between the layer graphs over their domain, with domain boundaries requiring separate handling. It does not model extrusion bead shape or guarantee bonding.

The probe uses **0.08 to 0.32 mm as illustrative acceptance limits**, not as a physically calibrated Centauri process. Its flat-domain layer step is different from the locally varying physical layer spacing. A 0.20 mm step can be rejected even when the maximum slope is acceptable.

## Running the geometry experiment

This directory has a standalone CMake project requiring only a C++17 compiler:

```
cmake -S tools/curved_transition -B <separate-output-directory>
cmake --build <separate-output-directory> --config Release
ctest --test-dir <separate-output-directory> -C Release --output-on-failure
```

Alternatively `build_probe.cmd <separate-output-directory>` uses this host's existing MSVC installation. The executable requires `--self-test` or explicit `--enable OUTPUT_PREFIX`. The latter writes two diagnostic cases: the original 40 mm / 4 degree ramp and a 20 mm / 5 degree ramp. The additional Catch2 cases are registered in the main libslic3r test target.

## Integration requirements

These are the requirements identified by the original geometry experiment. See [ENGINE.md](ENGINE.md) for the native mesh/plane transition integration and its current planning limits.

1. Extract and verify a height-field domain from actual model geometry; reject unsupported topology. Apply instance/volume transforms correctly and establish domain-wide bounds. The first milestone should support one bed-mounted prism with a shallow roof and no supports, modifiers or cut-plane stack.
2. Inverse-map the actual solid before slicing. Refine mesh facets where the nonlinear map curves them and bound the resulting surface error. Slicing an ordinary planar solid and changing only output Z is insufficient.
3. Map all perimeter and infill paths back with bounded chord error, physical layer-height and flow compensation, consistent seams, and model/bed boundary treatment. Recompute bridge, cooling and contact classification in the physical geometry.
4. Plan approach, departure, retraction and travel paths in physical coordinates. Check the whole translated toolhead against the material already printed, other objects, machine bounds and allowed motion. A slope bound alone is insufficient for this check.
5. Produce Preview from those same final physical paths. Preserve model dimensions, forbid crossing/interpenetration and verify the foundation of every first/transition extrusion. Test disabled-feature equivalence, failure behavior and project/preset round trips.
6. Deliver a separate experimental build with the user's Centauri presets carried over. The user chooses the feature, slices and starts the print. Geometry tests, successful builds and observed absence of contact are distinct evidence; none alone establishes a safe general daily-driver profile.

No stock printer, firmware, clearance, profile or physical-test claim is inferred from this prototype.
