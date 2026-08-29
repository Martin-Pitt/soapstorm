# Atmo Magic: blowing snow, drift and whiteout

Snow already settles: `SSPrecipPreset` carries `mSnowRate`/`mSnowMelt`/`mSnowDepth`/`mSnowRepose`,
`SSSurfaceField` integrates it per region cell (`sssurfacefield.cpp`, `tick()`, slope-gated by
`lieHere()`), and the field window ships the settled depth to the shaders. What does not exist yet
is everything the wind does to that snow once it is down - and everything the wind-driven snow does
back. This doc is the design for that layer: pickup, drift, ground blizzard, squall whiteout, and
the snow surface treatment, all built on the machinery Atmo Magic already runs.

The physics ladder, and how it maps to what follows:

| Condition | Threshold (typical) | Effect |
|---|---|---|
| Calm | < 3 m/s at the surface | snow only accumulates |
| Saltation onset | 3-8 m/s, ramping | settled snow lifts off exposed ground and streams downwind |
| Ground blizzard | sustained strong wind over bare deep snow | continuous mass lift everywhere exposed; visibility collapses near the ground |
| Snow squall | heavy falling snow + gusty + cold | falling snow plus drift plus whiteout, locally near-zero visibility |

The whole design hangs off one reading: **the wind flowmap already knows where the wind is faster
than ambient** (`SSWindFlowMap::sample`, alley jets, rooftop lift, lee calm - capped by
`SSAtmoWindFlowMaxGain`, default 4x). Every threshold below is evaluated against that local field,
not the sim's uniform breeze, so drift starts in the alleys before it starts in the open, and a
narrow gap whites out before the plaza does.

## What exists and what this adds

| Subsystem | State | Role here |
|---|---|---|
| `SSWindFlowMap` (`sswindflow.h/cpp`) | live | the wind field itself: `sample()` returns the gust-modulated flow at a point, `exposure()` how sheltered it is, `surfaceAt()`/`forEachColumn()` walk solid ground |
| `SSRainShadowMap` | live | top-down surface capture; `resolveColumn()` is where every particle finds its floor |
| `SSSurfaceField` | live | per-region wet/snow/puddle fields; **snow is stored but not yet shaded** - no surface pass reads the snow channel today |
| `SSPrecipSim`/`SSPrecipRenderer` | live | deterministic particle sim with tier bands, cell-hash spawning, flowmap advection (`windAt`, `ssprecipitation.cpp:84`) |
| RISER archetype ("Mana Embers") | live | particles spawned **from the ground**, rising - the exact emission shape blowing snow needs |
| `SSAvatarWet` | live | per-avatar capsule soak, folded into the wet pass - the template for snow caking |
| Weather state (`SSAtmoEnvWeatherState`) | live | wind speed, gusts, temperature, intensity bands - the knobs whiteout and squall derive from |

The one genuinely new render pass is whiteout. Everything else extends existing passes and sims.

## 1. Pickup: erosion from the surface field

`SSSurfaceField::tick()` gains a wind branch, active when the preset is a snow preset
(`mSnowRate > 0`) and the preset opts in with a lift rate (below). Per cell, at the field's own
tick:

```cpp
// the wind the cell actually feels, ground level, gusts included
const LLVector3 flow = SSWindFlowMap::getInstance()->sample(cell_pos);
const F32 v = flow.magVec();                 // sample() already applies gustAt scale + veer
const F32 lift = smoothstep(lift_lo, lift_hi, v);   // 0 below the band, 1 above
```

Two things make this the right gate:

- **`sample()` already includes the gust envelope** (`gustAt` is applied inside `sample()`,
  weighted by the cell's `exposure`), so gust-driven pickup - the pulsing quality of real blowing
  snow, lines of it jumping off a roof edge as a gust arrives - is free. No separate gust pass.
- **Corridors jet, plazas do not.** With the default `SSAtmoWindFlowMaxGain` of 4, a gap between
  buildings runs up to 4x ambient, so with an ambient 3 m/s breeze only the alleys saltate; in a
  storm with 8 m/s ambient, everything does. The threshold is evaluated locally, which is what
  makes the *same* preset produce drift on the exposed shore and calm behind the wall - and makes
  the user-facing band land where the literature puts it: onset around 3-4 m/s, full transport by
  8.

Defaults ship as settings, because the threshold is a physical constant, not an art dial:
`SSAtmoSnowLiftLo` (3.5 m/s), `SSAtmoSnowLiftHi` (8 m/s). The opt-in and the rate are authored per
preset: `mSnowLiftRate` (0 = the type never blows; "Blizzard" gets a strong rate, "Snow" a light
one). A temperature gate rides on top - above ~1.5 C the preset's own `mSnowMelt` already fights
accumulation, and wet snow does not blow, so lift scales down toward zero as temperature climbs
past 0.

Erosion removes depth: `mSnow[i] -= min(mSnow[i], lift_rate * lift * dt)`, and the removed mass
becomes the drift tier's spawn budget (below). The `lieHere()` repose clamp still governs how much
snow a cell can hold, so an eroded slope cannot take more than it has room for when it re-deposits.

## 2. Drift: snow advected by the flowmap, emitted from the ground

The user-facing ask is "snow blowing across the wind flow map from the ground, like the mana
embers, but snow" - and the RISER archetype is already that shape: `risesFromGround()` spawns each
particle at `resolveColumn()`'s hit, gives it an upward `mFallSpeed`, and hands it the wind sampled
at the ground (`emitParticle`, `ssprecipitation.cpp:676`). Mana Embers rise; blowing snow *streams*
- same emission point, different velocity profile and a spawn gate the embers do not have.

**Recommended form: a drift flavour of the RISER archetype**, not a new tier. A new `SSPrecipTier`
would grow `TIER_SPEC`, every preset's tier array, the renderer's bucket matrix and the preset
serialisation for one behaviour that the riser path already covers; the differences we actually
need are parameterisable:

- **Spawn gate** (`spawnTierCell`, riser branch): a cell emits only if the surface field reports
  snow at that cell (`SSSurfaceField::sample()` - one grid lookup, already region-anchored) AND
  the local lift figure from section 1 is above zero. The spawn weight carries that lift figure,
  so density is proportional to how hard the wind is working the snow there, not to area.
- **Velocity**: horizontal dominant. `mFallSpeed` becomes the loft (a gentle climb, a few tenths
  of a m/s, decaying over the particle's life so flakes arc up, stream, and settle back); the
  horizontal velocity is `windAt(hit_pos)` - which is the flowmap, gust included - times
  `mWindResponse`. A blizzard preset with `mWindResponse` 2.5+ funnels through the same alley jets
  the banner prims lean in.
- **Look**: saltation is near-surface. The riser's short life (2-3 s) and the `VIS_BAND` floor gate
  already keep the tier hugging the ground; `PART_GUSTY` (preset `mSway >= 1.5`, which Blizzard
  already sets) adds the vertical tumble. `applyEmberFlavor`-style per-particle flavouring is
  where a snow variant can mix shard/streak sprites for the streaky, motion-blurred read AC
  Shadows gets by stretching particles along velocity: `KIND_STREAK` with size scaled by the
  particle's own speed is the cheap version, and the streak path already exists.
- **Determinism**: unchanged. Spawns come from the shared-clock cell hash; the flowmap underneath
  is itself a deterministic solve, so every viewer lifts snow from the same corners of the same
  alleys.

What this buys visually, for free, because the emission points at the ground: snow visibly
*originates* at the surface and moves with the flow around obstacles - the lee of a wall stays
quiet, a rooftop edge trails streamers, a courtyard stays clear while the street outside its gap
streams. That is the ground-blizzard read, and it is a mass effect: when the ambient wind holds
above the threshold and the field is deep, every exposed cell qualifies, the tier caps (the riser
budget shares) saturate, and the air near the ground fills. No new "blizzard" code path - it is the
steady state of the same gate.

Redeposit closes the loop: in `tick()`, cells with a low lift figure but a sheltered flow exposure
(the flowmap's own `exposure()` channel - the calm the lee already has) gain back depth at a
deposit rate, capped by `lieHere()` room as today. Snowfall + wind therefore scours the open
ground and banks drifts against windward-obstacle lees and inside courtyards, and the field - the
same texture the shaders and footstep queries already read - is the whole record of it. This is the
tie-in the accumulation system makes cheap: drift is just erosion and redeposition on the existing
wet/puddle field, with the flowmap as the transport term.

## 3. Whiteout

A local, screen-space fog pass - `SSWhiteoutF`, in the same deferred-post family as the wet pass -
rather than a global haze change, because the whole point of wind-driven visibility loss is that it
is *structured*: the corridor whites out, the courtyard next to it does not.

**Insertion point**: beside `doAtmospherics()` in the pool pass (pipeline.cpp:4453 block), after
the global atmospherics so it composites on top of them, before the weather render block
(pipeline.cpp:4461) so falling flakes and drift particles stay in front of their own fog - the
same reason rain draws after clouds. The known trade is the one already documented for weather
there: a window between the camera and the whiteout shows the whiteout behind the glass.

**Density model** - three factors, all already computed somewhere:

1. **Drift veil** (wind-driven): per pixel, reconstruct the world position from depth, and march
   the eye ray through the drift band - the slab from the surface up `SSAtmoSnowBand` (1-3 m).
   Surface height per cell comes from the surface field window the shaders already bind
   (`ssFieldFetch`); flow speed comes from a new small window over the flowmap's bottom slab
   (same camera-centred window pattern `SSSurfaceField::updateWindow()` uses). Local speed above
   the ambient (`atmo->wind().magVec()`) by more than the `SSAtmoWhiteoutCorridor` ratio is the
   corridor term - the narrow-alley jet the user called out - and it is exactly what makes the
   gap between two buildings light up white while the plaza beside it stays clear.
2. **Squall veil** (fall-driven): background density everywhere the pixel's column is open sky,
   proportional to falling intensity (`atmo->precipitation()` when the preset is a snow type).
   Heavy falling snow alone, with no wind, still costs you the far hillside; this is the term that
   pays for it. Open-sky test reuses the exposure march `ssSurfaceFieldF.glsl` already runs
   (the `w` channel of `ssFieldAt`) - under an eave, no squall veil.
3. **Ground-blizzard coupling**: the drift veil's intensity is the same lift figure section 1
   computes, integrated over the column - sustained strong wind over deep snow saturates it, which
   is the ground blizzard as a visibility state rather than a separate system.

Colour is the environment's own fog colour at that depth (the same one atmospherics uses), so the
whiteout tints with the weather rather than being a hardcoded white; as density saturates, scene
colour lerps to fog colour exactly - a true whiteout. Depth falloff is exp-style over
`SSAtmoWhiteoutRange` so the near field stays readable while everything past a few tens of metres
goes.

Interiors are safe by construction: both factors need the column to be exposed, and the
exposure march already answers "is anything above this fragment" - the same test that keeps rain
off the inside of porches keeps whiteout off the inside of rooms.

Sky pixels (no depth) take the squall veil only, faded by distance toward the horizon - the storm
deck and atmospherics carry most of that look already, so the sky term stays modest.

## 4. The snow surface shader

The accumulation field is done; the *shading* is the missing half. The wet pass pattern -
fullscreen pre-lighting pass reading the field window, scratch target, commit into the gbuffer
attachments (`sssurfacefield.cpp:919`, hooked at pipeline.cpp:9761 ahead of all lighting) - is the
correct vehicle, and the new snow pass slots into the same block:

- **`gSSSurfaceSnowProgram`** (new, alongside `gSSSurfaceWetProgram`): reads `ssFieldAt`'s snow
  channel (already returned, already gated to the column top), and for snow-covered cells:
  - **Albedo lift** toward the snow tint with depth, keeping the underlying albedo's shade
    relationships - "caked snow texture over geometry", applied in lighting space so every light,
    probe and projector sees one consistent gbuffer, exactly the reasoning the wet pass documents.
  - **Roughness/gloss** dials: fresh snow is rough and bright; the existing
    `spec_dim`/cloud-transmittance logic the wet pass uses for rain carries over so specular
    behaves under the same deck.
  - **Normal treatment**: the field's slope/edge data (already in `mWindowFlowData`) plus
    procedural flake noise leans the shading normal - fresh snow fuzzes the surface, packed snow
    (below) keeps some of the ground's relief.
  - **Glints**: the RDR2/Crysis sparkle is a per-fragment hash on the sun-facing half vector -
    a cheap screen-stable hash (the `ssFieldHash` pattern) gated on depth and view alignment,
    folding into the specular term. Off-axis it vanishes, which is what sparkle *is*.
- **Depth gating**: the same `on_top`/exposure logic `ssSurfaceFieldF.glsl` already resolves -
  walls do not wear their cell's snow (only rain-wet is a wall-earned channel), roofs do.

**The 3D read - parallax occlusion mapping.** First instinct confirmed, with one honest caveat.
The snow depth per cell is a real heightfield; inside the snow pass, POM against a procedural
relief (fbm, offset by cell hash for stability) scaled by that depth gives the buried-kerb,
soft-intersection read on every surface the field knows about, at zero geometry cost - the same
stability argument as every other world-anchored trick here (hashes in agent space, camera
independent). The caveat: this is a deferred post pass, so POM **cannot move the silhouette** - the
displacement is shading only, and a 30 cm kerb poking through snow has a real geometric silhouette
no shading will bend. That is fine: the relief is sub-cell (ridges, drift ripples, a footprint
wall), the cell height is what buries things in the *look* of them, and the alternative -
displacing the terrain mesh or generating drift meshes - is rejected here for exactly the reason
the water family stays on stock geometry: the build is arbitrary viewer-side content, and shader-
side displacement over a CPU heightfield is the only representation that works on everything.

What this does *not* attempt: AC Shadows' geometric edge deformation (pushed-out snow ridges as
actual displaced triangles). That needs the surface geometry to exist to displace - terrain
tessellation or per-object decals with edge meshing - and it is the largest cost-for-value item on
the whole list. The POM edge treatment (steepen relief near the snow boundary, darken the compacted
core, lighten the pushed ridge) fakes the read at a tiny fraction of the cost. Deferred, and
written down so it is a decision, not an omission.

**Compaction** (the AC Shadows "compacted areas adhere to movement" note) slots into the field the
same way the avatar capsules slot into the wet pass: avatars compress the cell they stand in
(depth to `compacted depth`, edges of the path keep a slightly deeper lip). One extra channel's
worth of bookkeeping in `Field`, one input to the POM height, one darkening term in the albedo
mix. Follow-on, not launch scope.

## 5. Avatars: caking

`SSAvatarWet` is the template: per-avatar capsule, exposure-driven accumulation, folded into the
surface pass so avatars and the ground agree. The snow version rides the same capsule: while the
preset is a snow type and the temperature is at or below freezing, the capsule gains *caking* on
upward-facing exposure (bias by the rain-shadow exposure the avatar stands in), and sheds - by the
preset's melt figure indoors, or by rubbing off faster than it settles outdoors. The wet pass
already has the "avatar here" containment logic (`ssSurfaceWetF.glsl`); the snow pass reuses it so
ground snow never paints up a body, and the body's own caking is what shows. This is the RDR2
behaviour verbatim: time outdoors in snowy regions cake, indoor time sheds.

## 6. Authoring surface

**Preset additions** (`SSPrecipPreset`, serialised like the rest):

| Field | Meaning | Snow | Blizzard |
|---|---|---|---|
| `mSnowLiftRate` | erosion rate, 0 = the type never blows | low | high |
| `mSnowDepositRate` | lee redeposition rate | low | moderate |
| `mSnowDriftAge` | drift particle life cap, seconds | 2.5 | 3.5 |

Thresholds stay global settings (`SSAtmoSnowLiftLo/Hi`) - they are physics, not art direction, and
one band keeps every snow type consistent about when the wind starts winning.

**Weather state**: the squall is a *derived* label, not a new input - `SSAtmoEnvWeatherResolver`
classifies type and intensity already; a temperature-at-or-below-freezing moisture band at
`TURBULENT`/`SEVERE` convection surfaces as "Snow Squall" in the forecast text and (via the
existing intensity band) drives the whiteout pass's background term and the drift tier's budget
multiplier. Nothing new for an environment author to learn; the existing knobs (moisture,
convection, wind, temperature) compose into it.

**Footsteps** get `STEP_*_SNOW` surfaces the way wet/puddle did, keyed off the field's snow
channel at the foot - the enum, the global-setting plumbing and the surface-name tables all
already enumerate the pattern. Follow-on with the sounds, not launch scope.

## 7. Settings

New keys, in the house style (`SSAtmoSnow*`):

- `SSAtmoSnowLiftLo` / `SSAtmoSnowLiftHi` - the 3-8 m/s band, defaults 3.5 / 8.
- `SSAtmoSnowLiftResponse` - how fast drift particles take up the field (analogous to
  `SSAtmoWindFlowParticleResponse`).
- `SSAtmoSnowDriftBudget` - share of `SSAtmoParticleBudget` the drift tier may take.
- `SSAtmoSnowSurfaceStrength`, `SSAtmoSnowDepthFull`, `SSAtmoSnowSparkle` - the surface pass
  dials, mirroring the `SSAtmoWet*` family.
- `SSAtmoWhiteoutStrength`, `SSAtmoWhiteoutBand`, `SSAtmoWhiteoutRange`,
  `SSAtmoWhiteoutCorridor` - the fog pass, with the corridor ratio and the band height.
- `SSAtmoSnowDebug` - Render Metadata styles, matching the wind flow debug family: 0 off, 1 the
  erosion/deposit field (red scouring, blue banking), 2 the lift rate per cell, 3 the whiteout
  density.

Everything above degrades gracefully when `SSAtmoWindFlow` is off (or the GL 4.3 requirement
fails): `sample()` falls back to the ambient vector, so the threshold simply becomes an ambient
one - drift starts uniformly rather than in the corridors, and whiteout loses its spatial
structure but keeps its squall term.

## 8. Phasing

1. **Erosion + drift particles** - the field branch, the spawn gate, the RISER parameterisation.
   Purely additive; the visible change is snow moving with the wind from the ground up. Smallest
   change, biggest single effect.
2. **Redeposit** - the lee/stagnation term in `tick()`, so scouring and banking emerge from the
   same wind that moves the particles.
3. **Snow surface pass** - albedo/normal/specular/sparkle over the existing field window; the
   moment snowfall starts looking like snow on the ground rather than a debug quad.
4. **Whiteout pass** - local fog with the corridor term; the squall derivation feeding it.
5. **Avatar caking, footsteps, compaction** - the tactile follow-ons, each riding a pattern that
   already exists.

## Deferred / known limits

- **POM in a deferred pass shades, never silhouettes.** Geometric burial (a kerb genuinely
  swallowed, AC Shadows' pushed ridges) needs surface geometry to bite into; see the rejection
  above. The field's depth, the repose gate and the POM relief read correctly on slopes, drift
  banks and footprint edges regardless.
- **The drift band is a slab, not a volume.** The whiteout march integrates a constant-density
  band above the stored surface height; it does not see a snow plume curling over a roof the way
  a real volumetric solve would. The flowmap's slab structure could carry a per-slab drift density
  later if the top-down band ever reads wrong; until then the band is 2-3 m and the lie of the
  land carries it.
- **Redeposition is per-region and terrain-anchored**, like everything in `SSSurfaceField`: a
  drift banks against the *field's* stored surface, cell-resolution. A fence finer than the cell
  catches snow coarsely. The field cell size is the accuracy budget, as it is for puddles today.
- **Drift responds to flowmap rebuilds instantly.** A building appearing mid-storm moves the jet,
  and the snow that was banking behind the old shadow line is already somewhere else - the field
  re-erodes and re-deposits on its own tick budget, but the first minutes after a big edit can
  reshuffle visibly. The edit-settle machinery (`SSAtmoMagic::settleEdits`) already gates the
  captures; the field's `REBUILD_DZ` reset is the analogous damper here.
- **Cross-region drift stops at region borders** like everything else region-anchored; the
  flowmap's own margin overlap keeps the *wind* continuous, and particles crossing a border
  simply continue on the next region's field, but the accumulated bank at a border is not shared
  state.
- **Sky tracks** work the same way rain does today - `resolveColumn()` finds platform tops, snow
  settles on decks, and drift will scour them if the wind says so; there is no "indoor snow"
  concept beyond exposure, which is the correct one.
