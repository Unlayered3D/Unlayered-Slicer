# Experimental native curved layer blend

## Joint planning and decision overlay

Automatic selection now compares roof-only, underside-only, up to two joint
roof/underside slope allocations, and a genuine native planar reference. Both
stages of a joint candidate operate on the same solid: `z=U(x,y,R(x,y,w))`.
Roof proposals are made in the underside field's logical domain. Combined
slope and Jacobian bounds reserve headroom for both stages before slicing.
The composition's inverse Hessian uses the chain rule over expanded boxes;
conforming mesh tolerance, spacing, printer envelope and travel checks remain.
This is a bounded joint search, not a global optimum over arbitrary fields.
Explicit authored global plane stacks retain the user's target instead of
silently losing to the automatic cost policy. Their geometry and path safety
checks still apply.

Each feasible trial generates native contours, infill and supports. Generated
bridge/support bead conflicts prune roof patches and regenerate the solid.
Mapped support foundations and affine bridge slabs are checked for joint
candidates too. An unsafe candidate is rejected. A selected candidate is
regenerated and independently checked again before export; a failed final
reconstruction stops output. User support/process/printer settings are retained.

The balanced policy compares against that object's generated planar control:
normalized roof angular-alignment improvement + normalized underside angular
deficit improvement + 0.5 times the relative generated support-volume saving,
minus 0.15 times the relative motion-cost increase and 0.10 times the relative
model-volume increase. A score must exceed 0.0001 to beat planar. Surface
integrals retain original physical facet areas and evaluate actual local field
normals, including flat roofs and opposing features. These are geometric finish
and overhang estimates, not measured Ra, strength or support removability.

Material cost samples the generated paths with the same physical bead-area
correction as the writer. Motion cost includes generated path lengths, configured
role/Z speeds, path connections and the retained raised nonplanar travel. At most
16 samples per native segment bound evaluation work. The traversal used during
planning is not the final G-code ordering; acceleration, retractions, firmware,
warmup and plate-wide inter-object travel are not simulated. Costs are labelled
estimates and must not be presented as measured print time or certified savings.
Ramp/global candidates also compete against a native planar control.

**Preview > Layer mode > Region overlay** shows labelled XY planning footprints.
**Include rejected regions** adds dashed outlines. Hover a label, or select a
row under **Region decisions**, to read its reason, candidate surface gain,
generated support volume, motion estimate and net score. Selecting a row isolates
that region. Footprints are displayed at transition heights; they are not cut
planes, exact surface masks or collision meshes. The bounded versioned
`layer_region` G-code headers retain this overlay when a file is imported.

The current development changes require exact-source native/package/GUI
validation before replacing a previously validated desktop build.

The engine slices the original mesh through one continuous coordinate field,
generates contours and infill in that domain, and emits physical XYZ paths through
the inverse field. This is native slicing and Preview integration. It is not a
physically validated printer profile or a guarantee that every plane can be blended.

## User flow

**Automatic layer mode (experimental)** runs inside Slice plate.
It takes precedence over **Manual curved layers (experimental)**. The manual
control is disabled and labeled inactive while automatic mode is on, including
when an older preset has both saved switches on. Switching automatic mode off
restores access to the saved manual choice; the UI does not rewrite either value.
Existing enabled planes
are tested as a continuous stack. Otherwise the selector tries a bounded global
roof direction and local surface-region proposals, including two successive
directions. Up to eight proposals run through the actual field, conforming mesh
mapping, layer schedule and configured printer-limit checks. The original model
and its saved plane list are preserved; inferred planes are internal to the slice.

The roof direction and regional planner scores are mesh-based finish heuristics.
The regional scores describe piecewise-planar estimates and only rank proposals;
they do not predict curved print time, support savings or surface quality. The
selector does not yet optimize strength or independently directed branches.
Symmetric curved surfaces, unsupported undersides, supports and sparse infill use
independent compact roof fields instead of requiring a single global direction.
Roof-region downward-face contacts have protected planar zones. Native bridge/support bead
boxes are inspected after path generation; intersecting local patches are removed
and the original solid is re-sliced, up to 64 bounded retries. The joint search
also compares the bounded underside strategy and planar control even when useful
roof regions exist. No process setting or printer limit is
changed. Explicit B/C rotations or directed parts conflict with
automatic selection and require the user to choose manual mode instead.

Regional proposals clip each nearby downward facet to the patch's padded XY box
before reserving its highest contact height. A remote high tip no longer excludes
an otherwise usable roof. Bounded interior seeds supplement facet centroids;
sparse proposals also reserve the highest roof in their whole footprint so the
field cannot reach below the configured top shell. These remain candidate
heuristics: full generated bead-box checks decide which patches survive.

Regional layers use `z = w + S(w) F(x,y)`, with compact C2 roof fields.
At fixed logical layer height `w`, the slope is `S(w) grad F`: it cannot
overshoot the target slope during a transition. This removes the old fixed
70% angular reserve. The inverse Jacobian and inverse Hessian still bound
spacing and mesh error; a safeguarded inverse retains a monotone bracket.

The regional angle cap retains the configured maximum and gantry reach.
Every candidate additionally bounds the entire disk, including diagonal and
opposite taper slopes, against all four signed corners of the configured
hotend envelope. For `phi=(1-r)^4(1+4r)`, the proof uses
`-r phi' <= 432/625` and the angular bound
`-(a.e)(g.e) <= (|a||g|-a.g)/2`. A centre-direction check alone is insufficient.
Thus a target such as 5 degrees can be reached where this envelope and the
available transition height permit it, even if a scalar worst-azimuth limit
would be smaller. Tighter envelopes still reduce the target or reject it.
This is a configured-envelope certificate, not a new physical measurement;
manual global plane blends and underside fields retain their existing caps.

The decision and reason are displayed as a slicing notification and recorded in
the `layer_strategy` G-code header. A later extrusion or motion safety refusal
still stops slicing: automatic mode does not conceal such errors or export a
partially failed curved job. It is off in existing presets by default. An optional
automatic trial process is provided alongside the explicit curved trial.

For manual mode, turn automatic mode off and use
**Advanced → Process → Others → Multiplanar → Manual curved layers (experimental)**.
Enable nonplanar printing, choose a Virtual printer and keep belt printing off.
Keep process layer rotations B/C at zero. Add or accept planes whose centres are
ordered by height and whose surfaces do not intersect within the model bounds.
Press Slice plate yourself and inspect Preview before exporting. The original
object is sliced as one object; planes guide layer direction without creating
separately sliced sections.

A plane specifies its target build direction and the centre height of the
transition, not a sharp material cut. The engine chooses the transition depth
from object size, direction change, layer spacing and clearance margin. Crowded
planes, steep changes and insufficient transition height are refused with a reason.
Suggest planes still estimates piecewise-planar results; its cost estimates and
section warnings do not predict curved-blend output.

The mesh may have a curved roof, curved outline, through holes and other contours.
Each object requires one closed mesh volume. Multiple different models and copies
can share a plate and have independent fields, including planar objects. Modifiers,
painting, variable heights and intersecting authored branch planes remain outside
this integration. Automatic mode preserves planar bridges and support/contact
regions in roof mode and uses native support generation. The underside strategy
regenerates normal supports in its logical slicing domain and maps them with
the object. It does not make curved unsupported bridges printable or remove
support solely because a roof can curve.
With no planes, a certified rectangular ramp remains a calibration mode blending
from its flat foundation into its exact roof.

The full MCT-1 totem is a broad geometry benchmark. Its shelves and arch retain
planar bridge/support planning while eligible roof regions can use curved layers.
Check the actual slicing decision and Preview: finite patch budgets, competing
contact zones and process/clearance checks can leave features or the whole object
planar. Manual global plane blending still has no general support planner.
An STL contains no authored cut planes. **Suggest planes** proposes saved
piecewise-planar cuts, whereas automatic curved mode's candidates remain internal
to the slice. Check the slicing decision before using a completed totem print as
evidence that any curved layers were printed.

Classic walls are required. Combined infill, scarf seams, ironing, fuzzy skin,
Z anti-aliasing, wave overhangs, wipe and circular arcs are off. Ordinary retraction
is available. Draft shields and adaptive pressure advance are refused. XY and
elephant-foot compensation and Z offset are zero; shrinkage compensation is 100%.
Layer-change code may contain the checked cooling/progress commands or G92 E0. External slicing/G-code scripts,
timelapse motion and height-change code are outside this integration. Start/end
code remain in the physical machine frame.

## Fixed-nozzle dual-carriage mode

The dual-carriage integration uses the existing BBL filament/nozzle protocol,
with one used filament manually mapped to one nozzle throughout deposition.
The physical printer profile remains two-nozzle. Independent carriages, nozzle
clusters, toolchangers, automatic reassignment and in-print tool/material changes
are excluded. This mode is intended for fixed-nozzle X2D commissioning; it does
not establish its physical clearances or support simultaneous printing.

Printer settings provide a fixed active nozzle number (1 or 2), the active nozzle
used as the clearance origin, a whole-toolhead-recorded declaration and a source.
The existing common envelope/angles, gantry and Virtual Z bounds then describe
the whole head in that one state, including the parked nozzle. Both nozzle
identities must match. Changing the active nozzle requires reviewing these common
bounds and recording their new origin. No X2D geometry values are supplied.

The engine checks the active printable footprint/height, layer range, finite
offset and normal-mode Z speed. The writer pins the planned nozzle and refuses
tool commands during curved deposition before altering its state. Startup still
uses the existing stock physical-tool mapping and BBL protocol. Fixed mode
resolves X2D's initial hotend placeholder explicitly and checks the expanded
startup's last ordinary T command selects the planned filament/nozzle with H;
automatic H-1 selection is refused. This is a command check, not observation of
the firmware's physical response. Raw start/end
and filament scripts, firmware compensation and mechanical startup actions need
separate review; the field does not certify those actions.

Expanded layer scripts may contain cooling/progress commands M106, M142, M73,
M991 and G92 E0. Axis motion, coordinate resets, tool changes and other commands
are refused before emission. This preserves stock Bambu layer cooling/progress
without permitting raw XYZ movement through the curved layer path. Imported
Preview maps physical-nozzle offsets to filament slots once and retains the
already zero-based identity of a job using only nozzle 2.

Project reload keeps saved planes at their world positions when a single-volume
object's local origin is normalized. The G-code height header uses the physical
deposition bound in curved mode; its logical layer coordinate is not a height
above the bed. End-code height placeholders continue to use actual deposited Z.

## Field and geometry bounds

For selected planes the physical-to-logical mapping is
`w = z - gx(z)*(x-cx) - gy(z)*(y-cy)`. A quintic smoothstep blends each successive
target gradient over a physical height interval. Intermediate constant-w layers
are curved in XY, not merely tilted flat planes. The foundation is identity-mapped.
After a blend, layers follow its target direction until the next blend. The last
target need not match an arbitrary curved roof: the original mesh still determines
where material exists.

The maximum smoothstep derivative is 15/8. Bounds on its projected displacement
over the entire model rectangle establish a strictly positive Jacobian and an
upper bound on every intermediate slope. The engine reserves clearance margin
for that bound. The general plane program conservatively uses the worst configured
azimuth; each target is also checked in its own direction. The certified ramp uses
its known directional bound. Sparse facet sampling is not a collision certificate.

The physical mesh is copied and conformingly refined using a global Hessian bound
before changing coordinates. Shared edge midpoints preserve connectivity and hole
topology. The bound limits physical boundary interpolation error to 0.01 mm before
float rounding and normal slicer simplification. Resource limits fail explicitly
rather than silently coarsening the mesh. The ramp boundary maps exactly. The
user's model and Prepare geometry are unchanged.

Regional mode uses `z=w+S(w)*g.(x-c)*phi(|x-c|/r)` with
`phi(t)=(1-t)^4*(1+4t)` inside disjoint disks and zero outside. Both the spatial
taper and temporal blend are C2. The spatial gradient operator has eigenvalues
`phi` in [0,1] and `phi+t*phi'` in [-7/16,1]; displacement is bounded by
`0.17*|g|*r`. Temporal derivative and Hessian bounds establish monotonicity, slope,
normal spacing, conforming mesh error and emitted path subdivision. Local Hessian
boxes use only intersected patches and avoid refining identity facets unnecessarily.
For each patch, `w >= z-0.17*|g|*r`; when the whole physical box is above
`hi+0.17*|g|*r`, the blend has saturated and the inverse is `w=z-F(x,y)`.
Only the spatial Hessian bound `22*|g|/r` is then needed. A thin transition in
another patch cannot force its mixed/vertical bound over the entire model.
Shared edge splits are still propagated to neighbours; the mesh tolerance and
resource ceilings are unchanged. The selector compares full, three-quarter, half and
quarter amplitudes of the locally permitted gradient, each at several patch radii.
The configured maximum is a ceiling; a lower angle may fit a thinner shell or
improve a larger footprint. At each seed the feasible angle/radius pair with the
largest sampled gain wins. The selector uses equal-area
barycentric quadrature over affected upward facets, including adjacent flat roofs.
It requires at least a 2% sampled reduction in projected squared roof slope in the
logical slice frame. This rejects compact patches that merely move stair-stepping
to their taper. The proposal count is bounded at 256 and the accepted patch count
at 64; cancellation is checked during selection. The score is a roof-alignment
heuristic, not a surface finish or support-saving measurement. A patch tapers to horizontal at its edges;
it need not flatten an entire curved roof.

An integer schedule respects explicit printer minimum normal spacing and maximum
vertical layer height. Requested nominal layer step is an upper bound; the engine
may choose a smaller logical step. Paths are subdivided using a Lipschitz bound
of at most 0.005 mm vertical chord error before coordinate rounding. Physical bead
area and segment length replace the planar geometric flow factor while retaining
material/role ratios. Narrow classic gap fill uses the same positive-area
width/height convention as upstream Flow. Its speed bound accounts for the area
maximum inside the permitted height interval, not just at its upper endpoint.
Nonpositive or nonfinite mapped flow is refused. This bead model is not a bonding
guarantee.

## Motion and Preview

Automatic selection now checks the undersides as well as the roofs. It excludes
bed-contact faces, clips triangles crossing the bed, and measures elevated
downward surfaces against the configured support angle. A zero/automatic angle
uses a labelled 30-degree analysis reference; it does not replace upstream's
bead-overlap rule. At most 128 uniform layer-direction candidates are compared,
including diagonal and dominant facet directions, within the conservative
all-direction configured slope bound. Area-weighted angular-deficit scores use
unchanged actual surface area, and opposing-surface regressions are reported.
Initially vertical and upward faces participate in candidate scoring so a tilt
cannot hide a new overhang elsewhere. Reported downward footprint areas remain
world-XY projections. A 0.0001-degree threshold band excludes float-normal noise;
it is an analysis convention, not a printer allowance or a support-generation rule.

The saved automatic decision and Preview Selection details include this analysis
even when automatic selection falls back to planar. The score is a geometric
ranking heuristic, not support material saved, measured printability, or a bridge
span or collision check.

If no roof patch fits, automatic mode can
apply a whole-object underside field. It requires a candidate that does not worsen
another threshold-relevant surface. The field blends above a flat first layer and
finishes below all elevated downward faces, with bead, contact-gap and displacement
slabs reserved. A bounded search reduces its terminal angle when a short part
cannot finish the transition at the configured maximum. The report identifies
the actual applied angle; an analyzed maximum is not the deposited angle.

The original solid is mapped and sliced again; native normal supports are generated
from those same slices. XY is unchanged and each vertical column is monotone.
The mathematical domain includes a bounded support margin; generated beads outside
it fail rather than being extrapolated. Support layer steps are restricted by the
physical-spacing bounds; contact merging uses the active nozzle's minimum normal
spacing in the field. After path generation, support components need lower
deposition within their height and configured bottom gap. Bridges must occupy a
fully affine slab, preserving straight strands; their ends need deposited anchors
(overhang perimeters need deposited overlap). Normal bridge-strand area is retained
while extrusion uses physical path length. Missing foundations, unanchored bridges,
changing-direction air bridges and excessive support spacing stop slicing before
export. These are geometric checks, not sag, bonding or physical-print qualification.
Support settings and printer clearances are retained. Enabling/disabling supports
or changing the support angle invalidates the automatic slice.

Supports do not have to be enabled to attempt an underside field. With supports
off, the engine neither creates supports nor borrows the inactive support gap
for bridge anchors or transition room. Actual generated bridges, deposited
anchors and changing-direction slabs are still checked before export. With
supports enabled, mapped underside generation currently requires Normal (auto).
Tree/manual supports and configurations without a usable foundation transition
retain the roof strategy or planar fallback. A candidate remains explicitly
unapplied when no eligible field was built. This is not a universal support-free
planner; curved air bridges and independently steered underside branches remain
outside this integration.

For fixed dual-carriage jobs, expanded startup selection tracks both BBL T/H
and M1020 S/H commands. Parking/unloading or an unsupported selection invalidates
an earlier selection; out-of-range IDs are rejected before numeric conversion.
The final explicit selection must match the planned used filament and nozzle.
This checks emitted commands, not the printer's actual firmware state.

Expanded filament-start, layer and extrusion-role scripts in a curved job may
only control the permitted cooling/progress commands or emit G92 E0. Raw axis,
frame/tool commands and silent changes to position/extrusion writer-state output
variables stop export before the writer accepts those changes. Stock start/end
and calibration macros still need independent physical/firmware review.

The writer emits physical XYZ, local bead-height and layer-normal tags. Preview
processes those same paths and orients extrusion cross-sections. Virtual mode
keeps the actual nozzle upright and emits no rotary B/C motions.

Preview's **Layer mode** view colours native planar paths blue, the active
direction-transition intervals amber, and the established nonplanar field purple.
These labels come from the field at each emitted chord's midpoint; colours are
descriptive, not an independent collision check. The position inspector shows
the local layer inclination, while the physical Virtual nozzle remains upright.
The legend's **Selection details** expands the saved automatic decision reasons.
G-code imported from older builds cannot reliably distinguish a completed curved
field from its transition; tilted paths without a native mode label stay grey
and unclassified. This metadata does not alter coordinates, extrusion or speeds.

The represented asymmetric hotend pyramid and gantry bounds apply to the whole
layer field. The legacy checker for independent cut-plane episodes does not
partition a continuous curved pass correctly and is bypassed only for this bounded
mode. This does not claim a general CAD mesh collision checker or cover missing
hardware. Within one object, earlier material lies beneath earlier field
isosurfaces. Regional roof mode keeps support and bridge beads entirely in
identity zones; underside mode additionally permits straight affine bridges and
regenerated supports after its bounded foundation transition.
Different objects additionally receive whole-footprint checks including generated
supports and every instance. The generated first-layer hull, including brim and
skirt bead margins, is checked against the common and active-nozzle printable areas.
By-layer printing (or one shared nonzero print group)
uses each field's displacement bound plus the larger actual layer allowance to bound relative
height. Sequential groups use full part heights and also include generated brim
and skirt paths in the nozzle approach footprint against neighbouring models.
Insufficient head/gantry spacing
is refused with a message; the engine does not silently raise clearance limits.
All members of a curved plate must use compatible frame/process settings. Protected
planar paths retain ordinary overhang speed and fan splitting; curved paths use the
physical subdivision and flow branch.

For general roofs, contour midpoint sampling can deposit up to a fraction of a
layer above the CAD top. The travel envelope reserves a whole maximum physical
layer plus 0.02 mm for mapping/path/rounding error; output outside that envelope
is refused. That inflated height is also checked against machine and Virtual Z
limits. This is a toolpath bound, not a measured allowance for material swelling.

Travel leaves vertically, traverses at a bounded height, and descends vertically.
The fallback height uses the greatest physical deposition Z emitted
so far, plus a bound on tilted bead-edge rise, 0.001 mm for output rounding,
and the configured clearance
(at least 2 mm). The same bound survives changes between objects, instances,
planar support paths and curved fields; future unprinted layers do not raise it.
Unknown positions after custom code retain the full final-plate height fallback.
For a single-nozzle Virtual profile with a declared four-sided envelope and
gantry height, a conservative height map tracks the entire width and tilted
edge rise of every emitted object, support and adhesion chord. A route checks
the swept XY rectangles of all its waypoints against every occupied height
cell, with an additional 0.5 mm lateral envelope margin. The gantry and the top
of the envelope remain globally blocking, even for distant tall objects.
Only a complete map can lower a traverse; unknown motion, untracked native
extrusion, invalid geometry or excessive query work retains the global fallback.
The route keeps the same configured vertical allowance of at least 2 mm,
upward output rounding, and vertical departure/approach. Multiple physical
nozzles retain the global route because this check does not model a parked tip.
Custom machine-start routines must finish with a prepared bed and may not leave
unmodeled obstructions; firmware macros and material swelling are not inferred.
An XY detour retains all planned waypoints at one raised height, with one
departure and one final approach. Exact repeated destinations emit no motion.
These changes do not reduce the configured clearance or raise motion limits.
Completed affine tilted slabs emit each straight extrusion as one exact segment;
mesh mapping likewise avoids refining affine physical slabs. Changing-direction
and regional curved paths use interval Hessian bounds to merge fine chords only
when the full curve remains within the existing 0.005 mm chord tolerance.
The original fine-grid volume integral is retained, and layer-mode boundaries,
normal variation and bead-height variation further limit merging. Generic
callbacks without a curvature bound retain the original fine subdivision.
Sortable internal and solid infill additionally receive a bounded relocation
pass over the existing greedy chain, scored by physical XYZ travel time.
This preserves each path's direction and volume; perimeters, top surfaces,
bridges, support order, seams and explicit no-sort collections retain their order.
Candidate cost uses the configured Z travel speed capped by machine limits.
This reduces
redundant rounded moves without flattening a curved path or changing its bead
volume, and does not promise the same elapsed print time as planar output.
The writer refuses a computed height above the common machine/Virtual Z ceiling
or the declared active tip's lower ceiling, including output precision padding.
Every actual travel requests ordinary retraction, including
short nominal XY gaps. XY and Z speed are separate; Z velocity and volumetric flow
respect configured limits. This may be slow and does not establish string-free
material tuning. Model height plus travel must remain below machine and Virtual Z
limits. The no-plane ramp's 10 mm cap is a calibration scope restriction. No number
here establishes a measured generally safe nonplanar print height.

The option defaults off. Serialization, preset lists and slice invalidation are
wired into the normal app flow. Disabling it restores ordinary slicing. Windows
packaging retains stock preset JSON files and the cached preset catalog.

## Verification and remaining work

Standalone tests cover field inversion, curvature, spacing, successive direction
changes and infeasible-plane rejection. Native fff_print tests cover actual mesh
slicing, original-model preservation, curved roofs, a through hole, conforming
refinement, output, Preview, flow, travel and disabling/reslicing. Record actual
results and exact commit before distributing a build.

Intersecting authored branch fields, general curved bridge deposition, seam
optimization across changing topology and full measured toolhead meshes need
further integration. Roof-mode supports stay planar; tree paths crossing a local
field remove that patch during automatic planning. Underside-mode Normal (auto)
supports are regenerated and mapped under the independent foundation/anchor
checks above. Rafts remain excluded. The
selector does not promise that every model has a useful curved region, nor that
its limited local adjustment improves the printed finish.
Software tests and physical prints are separate evidence.

On mixed plates, physical skirts and brims reset to the identity mapping before
each instance visit. A previous object's bounded curved field must not map the
next object's adhesion coordinates. This reset retains the same plate-wide
raised-travel bound; it does not bypass machine, bed, or neighbour checks.
