# Atmo Magic: phase 8 — making the storm show visible

Status: **design + findings** (2026-09-06, from the first viewer build of phases 6c–7). Depends on `atmo_magic_storm_dynamics.md`, `atmo_magic_far_clouds.md`, `atmo_magic_wind_profile.md`, `atmo_magic_debug_views.md`. Harness: `V:\Scratch\atmo\PLAN.md` §17.

## 1. What the first build showed, and why

The scheduler, coupling and debug views work as designed; almost nothing of the *show* reaches the eye. Each observation traced to a cause:

| Observation | Cause | Fix |
|---|---|---|
| Forced storm under the editor's preview "loops a few seconds back and forth", never matures | The preview map's reference instant was the 2 s memo bucket start, re-latched every bucket; the cue's wall time jumped 2 s every 2 s, so the storm's age stood still and its position sawtoothed | 7d: reference latched once when the preview turns on or changes phase (`SSStormCells::mPreviewRefTimeS`) |
| The funnel vanishes on every cue/offset tweak | Kind and life window were hashed from the cell id; a new offset is a new cell, usually one whose hashed window is closed at the cue | 7d: a forced tornado/waterspout/anticyclonic pin **guarantees** its funnel — slot 0 is mesocyclonic with a fixed window (age 0.30–0.75, touchdown 0.39–0.64, cue at 0.5) |
| Spontaneous heroes far away, no funnel even when close | Hero staging is right (spawn 1.5–2 km upwind, pass 0.5–1 km from the anchor) but a spontaneous funnel needs supercell + meso ≥ 0.55 + tornado-eligible + a hashed window; most heroes carry only a gustnado. The V2 legend's "why not" rows name the failing gate | Accept for spontaneous storms (that is what "Allow" means); the authored path is the reliable show. 8a adds a "severe day" roll that *authors* a cue |
| Blurry rectangles under the deck | Rain shafts: 286 m-wide isolated pillars at 0.55 alpha with a 45% solid core, one per qualifying cell | 7d: curtains overlap into a sheet (width 0.62 cells), core 10%, streak erosion 0.65, alpha 0.35→0.03 |
| Squall lines never seen | 6% of severe epochs = one per ~8 h of severe weather; and a line is 7 ordinary cells fighting for 4 coupling slots, so it never reads as a wall | 7d: odds 0.18. 8a: authored squall kind + a line-band coupling primitive |
| Puffs do not rotate with the storm | Rotation only reaches the shader's striation read; puff positions are lattice-hashed and never displaced by the meso | 8b: swirl frame term |
| No anvils | Anvil is a shaping weight on the existing column (flatten + tower window); nothing spreads downwind at anvil altitude | 8b: anvil plume tier + mammatus |
| Mid-field clouds patchy, rows visible; wants 20 km horizon | Thinning keeps one puff per cell; 260 m lattice shows as rows in the squash; nothing beyond 9.8 km | 8c: horizon deck |
| Non-forced cells move in real time but warp on preview change | Expected: preview re-decides which epochs are severe; ages stay wall-clock | Documented (§2) |
| Generator does not time a severe storm's arrival | The bias raises peaks; it never authors *when/where* a storm passes | 8a: the roll writes a cue |

## 2. How the preview slider relates to storms (answer)

Two clocks exist. The **weather cube** is read at a *phase*; the **scheduler** hashes storm births on the *wall clock* and ages them in wall time. With the preview off, phase = the track's day cycle at the wall clock, and a storm born at 14:03 is what it is. With the preview on, the phase map is re-based: the sky is at the previewed phase *at the instant you moved the slider*, and time then flows normally from there. Consequences:

- **Spontaneous storms** (hash-born) keep their wall-clock ages. Scrubbing changes *which* epochs are severe (so cells appear/disappear) but cannot fast-forward a given storm to maturity. You wait, or you author.
- **Authored (forced) storms** are placed relative to the cue *phase*, so scrubbing the preview to the cue phase shows the forced storm mature at that moment (age 0.5), with its funnel down (7d). Scrub earlier and you see it building; later, roping out. This is the intended authoring loop.

## 3. Phase 8a — authored severe weather that actually arrives

1. **Squall as an override kind.** `mStormOverride` gains `"squall"` (kind 5). `SSSquall::forcedLine(seed, override, anchor, windAnvil)` composes a line event whose leading edge crosses `anchor + offset` at the cue (origin = crossing − motion × lead), with the ordinary member template; the scheduler's line hook returns it for the epoch containing the cue − lead (replacing that epoch's hashed decision), so it is deterministic and previewable like a forced cell.
2. **Line-band coupling.** A new uniform in `ssstormcouplecore.h`: `LineBand { origin, dir, halfLen, bandM, strength, shelfM }`. `towerWindow`/`anvilWeight` take `max()` with a segment-distance field so the deck raises a **continuous wall** along the line (not 7 discs), with a lowered **shelf** (gust-front) band `shelfM` ahead of it along the motion. Replicated in `ssVolCloudF.glsl` (twin test), read by the bake through the existing quantised sample. Costs one segment-distance per cell.
3. **The severe-day roll authors a cue.** `randomize(severeDay)` also writes a forced override: kind tornado (or squall, 30%), cue phase = the biased convection peak, offset = 1.5 km *upwind* at the anvil wind of that phase so the flyby crosses the region at the cue. The roll summary says so. This is what "timing the arrival" means; the bias alone never could.
4. V2 draws the line band and shelf; V5 marks the authored cue.

## 4. Phase 8b — storm anatomy

1. **Swirl.** A bounded rotation displacement about each coupled cell's centre: `R(p) = rotate(p − c, θ(t) · w(|p − c|/r))`, `θ = ω · age`, `w` = smooth bump (0 at centre, peak mid-radius, 0 at the rim). Displacement is bounded by `2 r w_max` and is **closed-form in wall time**, so it obeys the frame rules: producer applies it after the hero shift; observers (fragment, bake texel loop, precipNoiseAt) invert it (lesson 12); the shear ring problem (lesson 10) is avoided because `w` vanishes at the rim. Twin + round-trip tests before call sites. Supercells only (`mGate.mSupercell`), sign from rotation.
2. **Anvil plume tier.** For each coupled cell with `anvil > 0`, an extra puff pass at the equilibrium level: puffs placed along the anvil-level wind vector from the tower top, spread with distance (a fan of half-angle ~20°), altitude flat at `top_z`, radius growing and alpha fading with downwind distance up to `ANVIL_REACH_M = 6 km × anvil`. Placement through `placeWorld` with the frame terms; count budgeted like Tier B bodies (a few dozen per cell). Read-only for the bake (anvil casts no ground shadow of its own).
3. **Mammatus.** Under the anvil plume, a Worley-cellular lobe field: each lobe a small downward puff at `top_z − lobe_depth`, positions from a 2-D Worley on the anvil base (the wind-profile doc already names Worley for this); appears only for `lifecycle.mMammatus > 0`.
4. **Tower updraft loop.** The user's trick: in the tower column, puffs carry a periodic vertical offset `z += A · frac(age/T + hash)` with alpha fading in over the bottom 15% and out over the top 15% of the loop. Closed-form, bounded (`A ≤ tower height`), deterministic per puff hash; observers do not need to invert it because it only moves puffs *within* the column the gate already owns (state that explicitly and pin it: the offset never crosses a cell boundary).

## 5. Phase 8c — the horizon deck (replaces Tier C impostors)

The user's framing is right: beyond the puff field the sky needs a **modelled layer**, not sprites. Design:

- A **far annulus mesh** at deck altitude from `HORIZON_INNER_M` (≈ 8 km, inside the puff edge fade) to `HORIZON_OUTER_M` (≈ 200 km, past the geometric horizon at 3 km altitude), tessellated in rings so the squash fold and the Earth-curvature drop (`h = d²/2R`) bend it smoothly.
- Shaded in the fragment stage from the **same de-tiled presence field** (`ss_noise_mapDetiled`) at the annulus's air-frame position, gated by the same coverage, so the puff field and the horizon layer agree where cloud exists; thickness faked by a normal from the presence gradient and the deck's own shade/form formula (`SSDeckShade`), so the lighting matches the puffs. **Storm bearing bias**: far coupled cells beyond the puff field (the scheduler already resolves them) darken and thicken the layer at their bearing/distance — the deferred step 6 of the far-clouds plan.
- Blends: inner edge crossfades against Tier B bodies over the existing 8000/9800 rails; outer edge dissolves into the dome band's horizon melt. Depth: drawn in the sky pass behind everything at the squash cap.
- **Mid-field patchiness** is a separate fix inside Tier A/B: the row pattern is the 260 m lattice under the squash; cure is a per-cell rotational jitter of sub-puff placement (hashed, deterministic) plus a coverage-conserving raise of Tier B body radius (`BODY_RADIUS_FRAC` 0.425 → ~0.55 with alpha scaled down), measured by the visual rung before/after.
- Cost: one annulus draw (≈ 2k triangles), one presence read per fragment — cheaper than the puffs it replaces beyond 10 km.

## 6. Phase 8d — debug UI (from the same report)

1. **World dim.** The dim is a 2-D quad drawn *after* the in-world overlay (which renders in `LLPipeline::renderDebug`), so it dims the overlay lines too. Fix: draw the info view's in-world layer from the UI stage, after the dim quad, with the 3-D projection pushed (the HUD-render idiom), depth test off — the world dims, the overlay stays bright, the Skylines look. (An RLV-sphere-style post effect would dim the overlay just the same, since `renderDebug` runs before post-processing.)
2. **Chart beside the legend.** The `ss_atmo_graph_view` widget moves out of the floater's Views tab onto the debug HUD, docked to the right of the legend in the bottom-left; the floater keeps the mode combo and the dim slider.
3. **Tab reorganisation.** Each "Draw the …" checkbox moves into the pane that owns its dropdown: wind flowmap → Wind Flow; rain shadow map and roof runoff → Rain; surface/world/cloud field → Fields; "Show info overlay" → Views. Overlays keeps the markers and the settle marker and is renamed **Markers**. XML-only (controls bind by `control_name`), stretch anchoring with explicit left+right.

## 7. Order

7d (done, this build) → 8d (UI, small) → 8a (authored squall + cue: makes the show reachable) → 8b (anatomy) → 8c (horizon deck, after a frame-time measurement at 10 km).
