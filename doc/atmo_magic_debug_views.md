# Atmo Magic: info views — Cities: Skylines-style debug visualization

Status: **design** (2026-09-05). Companion to `atmo_magic_wind_profile.md`, `atmo_magic_storm_dynamics.md`, `atmo_magic_far_clouds.md` — every system phase in those docs ships with its view here. Existing machinery this builds on: `SSFloaterAtmoDebug` (the tabbed debug floater; overlay checkboxes bound to pipeline render-debug masks via `bindOverlayToggle`), the per-system `renderDebug()` methods (post-deferred immediate-mode gGL, squash-corrected, distance-thinned), and `SSStatsView` (translucent read-only console; "everything it shows is read from counters the features already maintain, so having it open changes nothing").

## 1. What "Skylines-style" means here

The current overlays are engineering probes: combinable checkboxes, raw markers, meaning lives in tooltips. An **info view** is a curated presentation: pick ONE mode, the world dims, one color-coded data layer pops, a legend says what the colors mean, and a chart panel shows the quantities a 3D overlay can't. Principles:

1. **Exclusive mode picker** — a new `SSAtmoInfoView` setting (0 = off, one mode at a time) on a new "Views" tab of the debug floater. The existing low-level checkboxes stay untouched underneath for engineering work; info views are allowed to *drive* them (activating a mode flips the relevant masks, deactivating restores).
2. **World dimming** — a fullscreen translucent dark quad drawn at the top of the UI stage (post-tonemap: no glow/alpha hazard, no stock shader touched, zero cost when off). Strength dial `SSAtmoInfoViewDim` (~0.55 default).
3. **One shared legend widget** — `SSAtmoLegendView` (an `LLView` docked bottom-left, SSStatsView idiom): mode title, a gradient bar with min/max labels in real units, and the mode's icon key. Every view pushes its legend rows through one interface; no view invents its own panel.
4. **One color language**, used by every view (ramps, not rainbow soup):
   - moisture / water / wet: blue ramp
   - convection / energy / heat: orange→red
   - rotation / vorticity: purple (sign by hue shift: cyclonic violet, anticyclonic magenta)
   - wind speed: dark→bright teal ramp; wind *direction* is always shown by geometry (arrows), never by color
   - presence / coverage / density: grey→white
   - reachable / OK / outside: green; sealed / blocked / interior: red (matches the existing air-labels view)
   - lifecycle / age: cool→warm across the stage bar
5. **Charts are first-class** — a reusable `SSAtmoGraphView` (LLView subclass drawing polylines/filled bands with gGL 2D, monospace labels) powering the panel side of the views below. The attached-image style — altitude on Y, quantity on X, annotated markers — is exactly its first client.
6. **Read-only discipline** — views read counters/fields the systems already maintain (SSStatsView's rule). A view must never tick, build, or reorder anything; if data isn't resident, the view says "not built" rather than causing a build.

## 2. View catalog

### V1 — Wind Profile (the flagship; ships with wind-profile phase 2)

- **Chart panel**: altitude (Y, 0 → cirrus) vs wind speed (X) — the classic boundary-layer curve from the reference image, sampled from `windAt(z)`. Annotated horizontal rails at the load-bearing altitudes: 10m reference, boundary-layer top (1500m), deck base, deck lid/anvil, cirrus band's *current* altitude (it moves with the anvil ramp — watching the rail slide down onto the lid during a storm is the whole point). Live numbers at each rail (speed + heading). A second inset: the **hodograph** — wind vector tips per altitude connected into the veer curve, the supercell ingredient made visible.
- **Comparison mode** (the image's three-panel idea): overlay the curve for the flowmap's region-roughness alpha vs the weather cube's authored/derived shear exponent vs the `SSAtmoWindFlowGradient` fallback — shows exactly which source is live and what each would do.
- **In-world**: a **wind mast** at the camera column (optionally a second at a picked point): a vertical stack of arrows from ground to cirrus, each rotated/scaled by `windAt(z)`, colored by the speed ramp — shear reads as the stack twisting with height. Near the ground the mast hands off to the existing flowmap arrows (which stay their own engineering view).
- Data anchors: `windAt(z)`, `groundZero()`/track floor, `cirrusAltitudeMetres()`, flowmap `windAlpha()`.

### V2 — Storm Cells (ships with storm phases 1–3, replacing the "debug circles")

- **In-world**: lattice cells tinted by storm potential (energy ramp, threshold line visible as the tint's floor); active cells drawn as influence rings colored by lifecycle stage; the **hero cell's full trajectory ribbon** — birth point, path with age tick marks, closest-approach marker at the anchor, death point — so a staged flyby can be verified before the tornado ever renders; meso rotation as orbiting arrows (purple, direction = spin sign); vortex icons with taxonomy labels (mesocyclonic / landspout / waterspout / …) and condensation-fraction bars.
- **Chart panel**: active-cell timeline — one row per cell, a progress bar across the lifecycle stages with the "now" cursor; hero row pinned on top.
- **"Why not" readout** (the Skylines problem-icon equivalent): when checkboxes are on but nothing spawns, list the failing gates with live values — `convection 0.31 < 0.45`, `Allow Tornadoes off`, `epoch hash missed (next window ~14min)`. Formation debugging without reading code.
- Data anchors: the scheduler's resolved cell set, birth params P/Ω/SH, gate expression terms.

### V3 — Cloud Deck (extends `SSAtmoCloudDebugView` 1–3 as sub-modes)

Existing cell-gate/tower map, column outlines and ground-shadow projection restyled under the palette + legend, plus new sub-modes:
- **LOD tiers**: the A/B/C tier rings drawn on the deck plane, macro-cell gate lattice in tier B, impostor card outlines in tier C, and the thinning `keep_frac(dist)` as a grey ramp — makes "where did my puffs go" answerable at a glance.
- **Drift & shear frames**: a mast of three arrows (deck base drift velocity, `O(anvilZ)`, `O(cirrusZ)`) plus the accumulated drift values and wrap counters as text — the view that verifies the whole frame contract, and the storm-local frame shift inside the hero cell's ring.
- **Budget**: live puff counts per tier vs `SSAtmoCloudPuffBudget`, colored bar in the legend.

### V4 — Precip & Virga (ships with far-clouds shaft phase)

In-world: shaft-qualifying cells outlined (presence×intensity as the tint), fall-tilt trajectories from deck base to landing (the wind-tilted entry offset drawn as the actual slanted line), the particle-rain handoff ring (`TIER_SHEETS` r2) on the ground. Chart: presence/tower sampled along a camera-forward transect.

### V5 — Weather Cube (any time; pure chart)

The day-cycle curves — moisture, convection, temperature, wind speed/heading, shear strength/veer — plotted over one cycle with the "now" cursor, plus the *derived* figures on a second lane (storm consolidation, gloom, anvil ramp, lightning gates). This is the "why is the sky doing that" view: every scalar the systems gate on, in one place, against time. Data anchors: `SSAtmoEnvWeatherResolver::resolve` at sampled phases (already pure — sampling it for the chart is side-effect-free by construction).

### V6 — World Field (existing, restyled)

Band surfaces / air labels / drainage under the shared legend + palette. (True Ground view removed 2026-09-05.)

### V7 — Sync console (SSStatsView sibling, not an info view — stackable)

Monospace readout of every shared-state input: seed, wall clock + current bucket, day phase, drift accumulators + wrap counts, active storm cell ids/hashes/ages, hero trajectory params. Purpose: two clients screenshot this side by side and any determinism divergence is immediately localizable. Cheap insurance for constraint 1, worth building **first** alongside the storm scheduler.

## 3. Implementation notes

- **Framework cost**: dim quad + legend + graph widget + mode plumbing is one small self-contained chunk (`ssatmoinfoview.h/.cpp` + XUI "Views" tab). Everything else is incremental restyling inside `renderDebug()` methods that already exist.
- In-world layers keep the established `renderDebug()` disciplines: cells not puffs, squash-corrected, distance-thinned, drawn post-deferred.
- Charts draw in UI space via the graph widget; no render-target or shader work anywhere in this doc.
- XUI: new tab in `floater_ss_atmo_debug.xml`; the mode picker is a combo bound to `SSAtmoInfoView`; per-mode option rows appear under it (the fields-tab combo idiom). Rect anchoring per the stretch rules (explicit left+right).
- Settings: `SSAtmoInfoView` (U32), `SSAtmoInfoViewDim` (F32), per-mode sub-view U32s reusing `SSAtmoCloudDebugView`/`SSWorldFieldDebugView` where they exist.

## 4. Phasing

1. Framework (dim/legend/graph/mode picker) + **V1 Wind Profile** — lands with wind-profile phase 2, which it exists to verify.
2. **V7 Sync console** + **V2 Storm Cells** — with the storm scheduler (V2 *is* the phase-1 deliverable's grown-up form).
3. **V3 Cloud Deck** sub-modes — drift/shear frames with the `O(z)` work; LOD tiers with the far-clouds tiers.
4. **V4 Precip & Virga** — with the shaft phase.
5. **V5 Weather Cube**, **V6 restyle** — any time; V5 is high value-to-effort and can be pulled forward.
