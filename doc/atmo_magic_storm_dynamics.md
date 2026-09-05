# Atmo Magic: storm dynamics — thunderstorm lifecycle, supercells, tornadoes

Status: **design** (synthesized 2026-09-05 from an entity-first and a field-first competing design + adversarial review; entity-first leads, field-first's lattice/decomposition grafted in). Depends on `atmo_magic_wind_profile.md` (`windAt(z)`, bounded shear offsets, drift frame rules). Feeds `atmo_magic_far_clouds.md` (distant rain shafts read per-cell precip intensity). Debug: `atmo_magic_debug_views.md` V2 (storm cells / hero trajectory / "why not" readout) and V7 (sync console) ship with the scheduler.

## 1. Premise

There is **no positional storm entity in the codebase**. "Tracks" are altitude bands (floor Z only); moisture/convection are single scalars per track per frame; the only spatial variation is 2D procedural noise (`areaFactorAt`, the deck's noise map). The existing storm consolidation ramp (`ssvolcloud.cpp:507-510` — moisture×convection widens the tower window into 1–3km connected cells) is the closest thing to a thunderstorm and is the anchor this design builds on. Everything below adds a **deterministic storm-cell layer** on top of the deck, never a parallel cloud system.

## 2. Storm cells: entities, recomputed from scratch

POD entities in new `ssatmostormcell.h/.cpp`, with **no persisted mutable sim state** — every frame, active cells are re-derived from `(SS_ATMO_SEED, wall clock, position)`, the same idiom as `areaFactorAt`/`gustEnvelopeAt`/`stormApproach`. A client joining mid-storm recomputes the identical cell from its birth phase.

```cpp
struct SSStormCell {
    U64  mId;            // hash(latticeX, latticeY, epoch)
    F64  mBirthTime;     // wall-clock bucket of spawn
    F32  mLifetimeS;     // hashed 20-70 min, ONE-SHOT (no cycling)
    LLVector2 mOriginXY; // WORLD frame at birth — never the drifted air frame
    LLVector2 mMotionXY; // m/s, closed-form from windAt(anvilZ) + deviation
    F32  mPotential;     // P: favorability at birth
    F32  mRotation;      // Ω: signed swirl strength (rare hash flips sign → anticyclonic)
    F32  mShear;         // SH: captured from wind profile's shearStrength at birth
    bool mSupercell, mSquallMember;
    U64  mParentLineId;
};
```

**Motion is closed-form**: `centre(t) = mOriginXY + mMotionXY * age`. This is the only determinism-safe motion model — an integrator (accumulated drift frame) makes storm positions a function of session history and framerate; two avatars would see the same tornado at different coordinates. Hard rule: **storm entities live strictly in the world frame**; consumers convert to whatever frame they need at the read site.

**Spawn schedule** — two placement modes:

- **Background cells** (non-tornadic): a ~3km hashed lattice × time epochs. One birth candidate per `(latticeCell, epoch)`, hashed from the shared seed; spawn iff `convection(birthPhase) * moisture(birthPhase) * SH_noise(cell)` clears a threshold. Active set = candidates whose `[birth, birth+lifetime]` contains now, **ranked by hashed id and culled by radius only — never "nearest camera N"** (camera-dependent selection of world state was the entity design's one fatal flaw; two clients 6km apart would render different skies). The 20km/40min lattice of the original draft is ~10× too sparse (one candidate per 40min across the whole visible disc); 3km/epoch gives usable storm density and the threshold does the thinning. Lattice period must be incommensurate with the deck noise map's 2048m repeat so storms don't inherit its horizon-visible period. Background cells provide sky texture (towers, anvils, precip geography) but **never carry the tornado**.
- **The hero cell** (decided 2026-09-05, answering density Q1): tornado/supercell formation is a **staged flyby, one at a time**, not free-range emergence — a tornado spawning 9km away is invisible and wasted. On a qualifying epoch (checkboxes on + weather gates clear + the epoch's hash fires), exactly one tornado-eligible supercell is *composed* rather than lattice-rolled: origin = weather-domain anchor + `D_spawn` (~1.5–2km) **up the storm-motion vector** (the deviated one, not raw wind — supercell motion is mean wind rotated 20–30°), lateral offset hashed so the closest approach to the anchor passes at **500m–1km**, side (left/right pass) hashed per epoch. The cell then rides its ordinary closed-form motion downwind past the region and dies out beyond it. Everything derives from `(seed, epoch, wind-at-birth, anchor)` — still a pure function; the "direction" is baked into the birth parameters, not steered live. **Anchor = the weather domain's region centre** (the region whose environment asset is driving — world state shared by every client under that sky), never the camera; crossing into a different weather domain re-derives the anchor, which is fine because the whole sky changes there anyway. Concurrency cap: one hero cell; satellite tornadoes and multi-vortex structure live on it, so "multiple funnels" is still possible without a second cell.

**Lifecycle** (`age01 = age/lifetime`, one-shot, bell curves scaled by birth params):

| age01 | stage | effects |
|---|---|---|
| 0.00–0.15 | towering cumulus | local tower boost ramps in; **landspout window** (high potential, low rotation) |
| 0.15–0.30 | maturing Cb | anvil begins, updraft peaks |
| 0.30–0.55 | mature | overshooting top pulses; mesocyclone/tornado window; precip displaced downwind |
| 0.55–0.80 | anvil spread | anvil pushed downwind at `windAt(anvilZ)` via the wind profile's `O(z)` transform; **mammatus** |
| 0.80–1.00 | decay | collapse, spin-down, rope-out if a vortex is live |

Any time-discretized effect (overshoot pulses, vortex sub-lifecycles) uses **wall-clock buckets** (`floor(now/Δ)`, the `sslightning.cpp:428` idiom) with a smoothstep crossfade between adjacent buckets — never a frame counter (framerate-dependent), never a raw step (pops).

## 3. Coupling into the deck — the replication discipline

The review's key insight: **phase 1 must not touch the cell gate or `presence`.** The gate is replicated three ways (CPU builder, `ss_cell_occupied` in the fragment shader, ground-shadow bake), and the bake's cache key contains nothing storm-shaped — touch presence and either the shadow shows a storm-free sky forever or the 65k-texel bake re-runs per frame.

Default path (phase 1): cells modulate **`height_shape` / `cell_anvil` only** — a storm-potential peak pushes existing gated columns toward "connected tower, early anvil" (locally blending the baked `mNoiseTowerLo/Hi` toward the storm window), adds no puffs, moves no gates. Cost: `MAX` a handful of smoothstep/dist ops per cell in the loop that already runs. Consequences to handle:

- `ss_tower_ramp` is currently one deck-wide uniform — it becomes a per-fragment *local* window (storm params as ~6 floats × N cells of uniforms; GLSL re-evaluates the same radial falloff). Two-way lockstep (builder + shader), deliberately not three.
- **Shadow bake key** gains a quantized storm term — storm centre quantized to ≥260m of travel so the bake re-runs on meaningful movement only.
- **Precip displacement rides `tower`, not `presence`**: the rain-free base / downwind precip shift is a read-side transform in `precipNoiseAt` (shift the sample point relative to the meso centre), sharing the one function builder and precip already agree on. Frame care: `noiseFieldAt` is air-frame, storm centres are world-frame — the conversion happens inside, with the drift passed in, or the rain-free base lands sheared away from the visual base.
- Overlap rule: coverage/anvil combine via `max()`; rotation/motion never blend — a column is owned by the cell with the largest influence there (argmax), no averaged spin.
- **Storm motion vs cloud drift** (decided 2026-09-05, Q2: physically right wins, within artistic limits): puffs are welded to air-frame cells, so a storm moving at storm velocity ≠ cloud drift would otherwise be a shape-wave sweeping through a stationary puff pattern. The supercell therefore gets a **storm-local frame shift**, the same discipline as the wind profile's `O(z)`: inside the cell's influence radius, the field's sample coordinate is offset by the closed-form relative displacement `(v_storm − v_drift)·age`, blended by `infl` — a coherent local frame, so the noise columns, fragment carve, shadow bake and precip gate all shift *together* (replicated as a formula of the same per-cell uniforms, never a per-puff CPU displacement that would strand puffs off their columns). Deterministic (pure function of age), bounded in practice by lifetime + influence fade; artistic clamp on `|v_storm − v_drift|` (a few m/s) keeps the slide readable instead of tearing the deck. Background cells stay pinned to deck drift — only the hero cell pays for the warp. Ships with the supercell phase, not deferred: the guaranteed 500m–1km flyby makes the shape-wave artifact impossible to miss.
- `stormy` texture pick (`CUMULONIMBUS` vs `ALTOCUMULUS`) is deck-wide; a lone supercell in a fair sky wears the wrong art. Per-cell texture-mix bias, or accepted v1 limitation.
- Storms affect only `weatherDeck()` — never the under deck.
- Local gloom (wall-cloud darkening): per-puff scalar on the **spare `b` vertex channel** (verified free), consumed as an extra gloom term in the fragment stage — plus a companion uniform for the base-veil sheet path, or the darkening stops dead in a hard seam at the veil directly under the wall cloud. The `b` channel is hereby reserved for storm-local gloom (the far-clouds doc's shaft flag uses a sign trick instead — see that doc).

**Supercell** (`mSupercell`, gated by the "Allow Supercells" checkbox + `Ω·SH` threshold): motion deviates 20–30° right of the mean wind (sign flips for the rare anticyclonic hash), persistent rotation drives base striations/lowering via the local-gloom channel + a rotational detail-scroll bias, rain-free base + downwind precip via the `precipNoiseAt` shift.

## 4. Vortices: one renderer, a taxonomy of gates

Vortex objects are children of cells (one deterministic hash per parent decides existence, kind, timing), plus two parentless kinds. All share one renderer.

| kind | gate |
|---|---|
| mesocyclonic | `Ω` high, mature stage, "Allow Tornadoes" |
| landspout | TCU stage (0.00–0.15!), high potential, low `Ω` — forms under the growing cumulus before it is a storm |
| waterspout | landspout/weak-meso rule ∧ `centre(t)` over water (region water height vs terrain) |
| satellite | second hash slot on a strong mesocyclonic parent, orbiting offset |
| anticyclonic | rare sign-flip hash (~2–5% of qualifying cells) |
| QLCS | squall-line members only, at inter-cell junctions on the leading edge, short-lived |
| gustnado | outflow ring (~1.3× core radius, opposite motion vector) during mature/decay; funnel-less, dust only |
| dust devil | parentless mini-scheduler (small lattice, short epochs) gated on high temperature, high sun, near-calm wind, ~zero coverage |

Fire whirls: out of scope (not a meteorological vortex; would belong to a fire system).

**Rendering**: a stack of 10–16 Z-billboarded "collar" quads from wall-cloud height to contact, radius per collar from a taper curve — one taper constant spans **wedge** (wider than tall) to **drill-bit** (thin). Condensation fraction animates funnel-aloft → touchdown → rope-out. **Multi-vortex is a shader term, not entities**: `radius(θ,t) = base·(1 + amp·sin(N·θ + ω·t))`, N∈[2,6] hashed — suction vortices appear/disappear and orbit without sub-frame entity state. Debris/dust skirt = ordinary `LLViewerPartSim` particles seeded from the vortex id (per-client sparkle nondeterminism = existing precedent for rain). Constraints from review:

- Vortex quads never enter `mPuffs` or the puff budget; own cap (~3 vortices × 16 quads).
- **Squash**: a funnel is one tall vertical span inside the squash band — subdivide before `squashScale()` (the squash is non-linear along a segment), same discipline as `renderDebug()`.
- **Glow/alpha (constraint 4)**: the funnel draws in the sky forward pass with real alpha; never post-deferred additive — a dark funnel that writes additive alpha *blooms*.
- Ground contact uses the same ground reference as the wind profile (track floor / low-res terrain average — True Ground is gone).

## 5. Squall line

A **spawn template**, not a new code path: a rare lattice event emits a line origin + heading (perpendicular to its motion), spacing 2–3km, along which ordinary `SSStormCell`s share one birth/motion (`mParentLineId`), each with small lifecycle jitter. Supercells suppressed along the line except one hash-picked embedded segment (real QLCS behaviour); QLCS vortices spawn at junctions on the leading edge. At 12km field scale a "line hundreds of km long" is exactly this: only the local advancing segment materializes.

## 6. Formation control — three layers

**Staging principle (2026-09-05):** a severe event that plays out offscreen might as well never have happened. Spontaneous supercell/tornado spectacle is always staged into the weather domain's near field (the hero flyby, §2); background cells may live anywhere in the field but carry sky texture only, never the show.

1. **Spontaneous**: the lattice + weather gate, with per-track checkboxes **"Allow Supercells"** / **"Allow Tornadoes"** (Weather Influence enable+strength rows; strength scales the thresholds).
2. **Encouraged**: weathergen "roll a day" gains a *severe day* bias — reshapes authored convection/moisture/shear curves at authoring time (still offline `llrand`; output curves are what syncs).
3. **Forced**: keyframe overrides on the weather cube, `mPrecipitationOverride` idiom — `mStormOverride` (kind string), `mStormOverridePhase`, `mStormOverrideOffsetM` (track-floor-relative XY). The nearest lattice cell to the offset gets its birth/params pinned to guaranteed-active constants — still a pure function of (phase, offset), so a forced tornado hits its cue on every client.

## 7. Cost

Schedule resolution: dozens of hashes over the lattice window per frame (<0.01ms). Field modulation: a few ops × active cells × the cell loop that already runs. Vortices: ≤48 quads. Uniform traffic: ~6 floats per active cell. The heavy risks are the two cache keys (shadow bake, and far-clouds' thinning) — both addressed by quantization, above.

## 8. Phasing

1. Scheduler + debug-overlay circles (verify cross-client determinism with two viewers side by side), zero visual coupling.
2. Tower/anvil modulation + per-fragment tower window + shadow-key quantization + precip displacement. Biggest visual win.
3. Supercell: motion deviation, meso gloom/lowering, overshoot pulses, mammatus (CPU puff-shape Worley modifier first; fragment-carve version only if too soft — that one needs full triple replication, defer).
4. Vortex renderer + taxonomy (mesocyclonic, landspout, waterspout, dust devil, gustnado first), multi-vortex shader term.
5. Squall line, forced overrides, weathergen bias.

## 9. Open questions

1. ~~Target density~~ **Answered 2026-09-05**: one hero cell with a staged upwind-spawn/downwind-flyby trajectory (spawn ≤2km out, closest approach 500m–1km — see §2); background lattice cells stay non-tornadic.
2. ~~Storm slide vs pinned drift~~ **Answered 2026-09-05**: physically right, within artistic limits — the hero cell gets the storm-local frame shift (§3), clamped for readability; background cells stay pinned.
3. Is the existing `mCloudDriftM` cross-client divergence accepted debt? (This design assumes yes and keeps storms world-frame; if it gets fixed per the wind-profile doc, nothing here changes.)
4. Multi-vortex visual fidelity: is the shader angular term enough, or do the largest tornadoes deserve 2–3 real child funnels?
