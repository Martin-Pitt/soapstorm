# Atmo Magic snow: code architecture

The implementation plan for `doc/atmo_magic_snow.md` - files, classes, signatures, data flow and
wiring, in build order. Everything is additive to the existing Atmo Magic singletons; no new
threads, no new GPU solves, and every pass gates to zero when idle. Section numbers in brackets
refer to the design doc.

## Principles carried from the design

- **The surface field stays the one ground truth.** Depth, lift and creep live in
  `SSSurfaceField`; particles, shaders and sounds read it and never write it except through one
  deposit entry point.
- **One lift authority.** "Is snow lifting here and how hard" is computed in exactly one place
  (`SSAtmoMagic::liftAt`) and consumed by drift spawning, runoff feed, the near-camera ring and
  stats. No component re-derives the threshold band.
- **The wind flowmap stays a static, deterministic solve.** Blowing snow consumes it; nothing
  writes to it. Its one new output is a CPU-windowed copy of the ground slab for shaders.
- **Reuse over new tiers.** Drift is a separate particle pool with riser-shaped emission, not a
  `SSPrecipTier`; granular runoff is a re-feed of `shedRegion`, not a second shed path.

## Module map

| File | Change | Responsibilities added |
|---|---|---|
| `sswindflow.h/cpp` | modify | `sampleGround()`, the ground-window GPU texture + binding |
| `sssurfacefield.h/cpp` | modify | lift/creep/deposit in `tick()`, `depositAt()`, `renderSnowPass()`, extended `Sample` |
| `ssatmomagic.h/cpp` | modify | `liftAt()`, `squallFactor()`, granular deposit routing in `processImpacts()` |
| `ssprecippreset.h/cpp` | modify | granular authoring fields, `isGranular()`, the Sand built-in |
| `ssprecipitation.h/cpp` | modify | `mDrift` pool, `updateDrift()`, `emitDrift()`, granular stream/drip look, near-camera ring |
| `sspreciprenderer.h/cpp` | modify | one extra source loop over the drift pool in `render()`'s batching |
| `sswhiteout.h/cpp` | **new** | the whiteout pass singleton: intensity state, half-res density + composite |
| `ssavatarwet.h/cpp` | modify | caking channel on the existing capsules |
| `shaders/class1/deferred/ssSurfaceSnowF.glsl` | **new** | snow surface: coverage, albedo lift, creep scroll, POM, sparkle |
| `shaders/class1/deferred/ssWhiteoutF.glsl` | **new** | density pass (half-res), reused by a tiny composite entry |
| `shaders/.../ssPrecipRainF.glsl` (and `ssPrecipLitF.glsl`) | modify | the dithered granular branch |
| `llviewershadermgr.cpp` | modify | `gSSSurfaceSnowProgram`, `gSSWhiteoutProgram` registration (wet-pass pattern) |
| `pipeline.cpp` | modify | snow pass call in the wet block; whiteout call after `doAtmospherics()` |
| `settings.xml` | modify | the `SSAtmoSnow*` / `SSAtmoWhiteout*` keys |
| `sssoundscape.cpp` | modify (phase 5) | snow surfaces at the `STEP_TERRAIN_*` resolution site (line ~1350) |
| `llviewermenu.cpp`, `ssfloatersim.cpp`, `ssatmomagic.cpp` (`drawInfo`) | modify | debug entries + stats lines |

## Data model

### `SSSurfaceField::Field` (per region)

Existing: `mZ`, `mWet`, `mSnow`, `mPuddle`, `mStore` (shed accumulator), `mAccum` (spawn
accumulator). Added:

- `std::vector<F32> mLift` - the per-cell lift figure (smoothstep of local ground wind against
  the threshold band, temperature-gated). Recomputed each tick; read by `sample()` and by drift
  spawning. Not uploaded to the field window - it is CPU-consumed only; the shaders that want a
  lift-like figure (whiteout corridor term) read the wind flowmap's ground window directly.
- Creep needs no storage: the advection exchange is computed cell-by-cell inside `tick()` into a
  scratch row (double-buffer one row, not the whole grid).

Window textures unchanged in phase 1 (RGBA32F `Z, wet, snow, puddle`; flow window
`slopeX, slopeY, slopeNorm*wet, spare`). Phase 5 (compaction) takes the flow window's spare
channel and, if it needs a second, adds one more field texture - the layout decision is deferred
with the phase, and `bindFlowForShader` is the only code that would change.

### `SSPrecipSim` particle pools

Existing: `mParticles` (tiered falling), `mRipples` (impacts/drips), `mStreams` (persistent
cascades). Added: `mDrift` - ground-emitted blowing snow and the near-camera ring, one vector,
its own count and cap, rendered through the same material batching (see renderer below). Drift
particles are ordinary `SSPrecipParticle`s; the integrate loop does not see them (they carry
their own simple motion: horizontal flow plus a decaying loft, integrated in `updateDrift`), so
tier counts, tier targets and tier culling radii are untouched.

### The wind flowmap ground window (`SSWindFlowMap`)

The flow tiles are CPU-resident post-readback. A camera-centred window over the bottom slab,
rebuilt on the same cadence as `SSSurfaceField::updateWindow()`:

- per cell: `(flow_x, flow_y, speed, exposure)` into an RGBA32F texture,
- plus the origin/cell/res uniforms `ssWindOrigin` (mirroring `ssFieldOrigin`'s layout so the two
  windows fetch identically),
- one allocation, `glTexSubImage2D` per update, evicted with the tiles.

This window is the shader-side "where is the wind faster than ambient" answer for both the
whiteout corridor term and the snow surface pass's creep scroll.

## Key APIs

```cpp
// sswindflow.h - the cheap field read and the shader window
LLVector3 sampleGround(const LLVector3& pos_agent) const;   // bottom slab bilinear, NO gust
bool bindGroundWindow(LLGLSLShader& shader, S32 channel);    // + ssWindOrigin uniform

// ssatmomagic.h - the single lift authority
F32  liftAt(const LLVector3& pos_agent) const;   // 0-1: band x sampleGround x gust x temp x preset rate
F32  squallFactor() const;                        // 0-1 derived in refreshParams()
bool granularWeather() const;                     // preset().isGranular()

// sssurfacefield.h - the one write path, and the snow pass
void depositAt(const LLVector3& pos_agent, F32 depth);  // repose-capped; overflow discarded
void renderSnowPass();                                   // called inside renderWetPass()'s block
Sample sample(...) const;                                // Sample gains F32 mLift

// ssprecipitation.h - the drift pool
void updateDrift(F32 dt);                                // field walk + camera ring + integrate
void emitDrift(const LLVector3& ground, const LLVector3& flow, F32 lift, SSRandStream& rng);
const std::vector<SSPrecipParticle>& drift() const;
S32  driftCount() const;

// sswhiteout.h - new singleton
class SSWhiteout : public LLSingleton<SSWhiteout>
{
public:
    void idle(F32 dt);     // intensity state: lift activity + squall factor, window liveness
    void render();         // half-res density pass, then full-res composite onto the target
    void clear();
    void releaseGL();
    F32  intensity() const;
private:
    LLRenderTarget mDensity;      // half-res
    bool mDensityValid = false;
};

// ssprecippreset.h - authoring
F32 mSnowLiftRate   = 0.f;   // 0 = this type never blows
F32 mSnowDepositRate = 0.f;
F32 mSnowCreepRate  = 0.f;
F32 mSnowDriftAge   = 2.5f;
bool isGranular() const      // FLAKE and SOLID are granular; LIQUID and RISER are not
    { return mArchetype == SSPrecipArchetype::FLAKE || mArchetype == SSPrecipArchetype::SOLID; }
```

`SSAtmoMagic::liftAt` is deliberately the only place the band settings
(`SSAtmoSnowLiftLo/Hi`), the gust envelope, the temperature gate and the preset's rate meet. It
calls `sampleGround()` once - never `sample()`: the fbm gust evaluation is per-caller, and every
caller of `liftAt` applies `gustEnvelopeAt(sharedTime())` as a scalar multiplier itself.

## Frame flow

### CPU (idle, main thread, existing budgets)

1. `SSAtmoMagic::idle` - params, squall derivation (existing `refreshParams` grows the squall
   factor).
2. `SSWindFlowMap::update` - unchanged solve; ground window refresh appended.
3. `SSSurfaceField::idle` -> `tick()` per region, in the existing cursor order, now four stages
   per cell: settle/melt (unchanged) -> lift (`mLift[i]`) -> creep (row-buffered downwind
   exchange, CFL-capped, spilling into `mStore[ui]` at edge cells) -> deposit from calm (the lee
   term, hysteresis gap below the lift threshold). `shedEdges()` then runs unchanged - for a
   granular preset its inflow is what creep spilled into `mStore`, for liquid the existing rain
   feed.
4. `SSPrecipSim::update` - falling tiers (unchanged) then `updateDrift(dt)`:
   - field walk: `SSSurfaceField` hands the sim lift cells around the camera (`forEachLiftCell`,
     a small iterator over `mLift` above a floor, camera-radius bounded), spawn weighted by lift
     x depth, deterministic from the shared-clock cell hash;
   - near-camera ring: a capped ring spawn when `liftAt(camera)` or `squallFactor()` is above
     zero;
   - integrate the pool: flow advection plus decaying loft, ground clamp via
     `SSRainShadowMap::resolveColumn` on a slice, fade by `mSnowDriftAge`.
5. `SSAtmoMagic::processImpacts` - granular drips land: instead of ripples, `depositAt(land,
   clump_depth)`; the `from_runoff` flag already marks these.
6. `SSAvatarWet::idle` - soak (existing) plus caking gain/decay.
7. `SSWhiteout::idle` - intensity state only (cheap; no per-frame CPU field reads).

### GPU (render order)

1. Deferred gbuffer as today.
2. Wet pass block (pipeline.cpp:9761): `renderWetPass()` then `renderSnowPass()` - the snow pass
   reuses the same scratch targets sequentially and commits into the gbuffer attachments
   (albedo + specular via the existing commit shader; its attachment mask becomes a parameter).
   Ahead of all lighting, same reasoning as wet.
3. Lighting, SSAO, etc. - untouched.
4. Pool pass, `doAtmospherics()` site (pipeline.cpp:4453): immediately after
   `done_atmospherics`, `SSWhiteout::render()` - half-res density (depth, field window, wind
   ground window, band march with heightfield occlusion taps), then a composite lerp to the fog
   colour on the scene target. Before the weather block, so flakes and cascades stay in front of
   their own fog.
5. Weather block: `SSPrecipRenderer::render()` batches `mParticles`, `mRipples`, `mStreams` and
   now `mDrift` through the same material buckets; granular streams/drips take the dithered
   branch (below).

## Shader inventory

| Shader | Status | Content |
|---|---|---|
| `ssSurfaceSnowF.glsl` | new | includes `ssSurfaceFieldF.glsl`; coverage from the snow channel; albedo lift; roughness/gloss; creep scroll (wind ground window + time); POM (step count/range uniforms, taper by distance); sparkle hash; avatar containment via the capsule bind |
| `ssWhiteoutF.glsl` | new | density: band march (surface Z from the field window, occlusion taps), corridor term (wind ground window vs ambient), squall term (exposure march from `ssFieldAt`), depth falloff; composite entry lerps scene colour to fog colour |
| `ssPrecipRainF.glsl` / `ssPrecipLitF.glsl` | modify | granular branch: static Bayer/IGN screen hash vs alpha -> discard, opaque write with depth; distance crossfade back to blend; granular tint/scroll look |
| `ssSurfaceFieldF.glsl` | unchanged | `ssFieldAt` already returns everything the snow pass needs |
| `ssSurfaceNormalF.glsl` | unchanged | water waves stay water's; snow scroll lives in the snow pass |

New programs in `llviewershadermgr.cpp`, registered exactly like `gSSSurfaceWetProgram`
(deferred feature level, `bindDeferredShader`, texture channels via `mActiveTextureChannels`):
`gSSSurfaceSnowProgram`, `gSSWhiteoutProgram` (density) and `gSSWhiteoutCompositeProgram` (or a
second entry in the same program behind a uniform, if the two fits one file cleanly - decide at
implementation; two entries is the safer default).

## Settings (new keys, house style)

| Key | Type/default | Notes |
|---|---|---|
| `SSAtmoSnowQuality` | S32, 2 | 0 off - 4 ultra; drives the section 9 table |
| `SSAtmoSnowLiftLo` / `SSAtmoSnowLiftHi` | F32, 3.5 / 8.0 | the m/s band |
| `SSAtmoSnowDriftBudget` | F32, 0.15 | share of `SSAtmoParticleBudget` |
| `SSAtmoSnowCreep` | F32, 1.0 | creep advection rate multiplier |
| `SSAtmoSnowCascadeDither` | F32, 0.8 | 0 blend - 1 full stipple |
| `SSAtmoSnowSurfaceStrength` / `SSAtmoSnowDepthFull` / `SSAtmoSnowSparkle` | F32 | surface pass dials |
| `SSAtmoSnowPomSteps` / `SSAtmoSnowPomRange` | S32/F32, 16 / 32 | POM ceiling and near range |
| `SSAtmoWhiteoutStrength` / `Band` / `Range` / `Corridor` / `Scale` | F32 | band metres, range metres, corridor ratio, 0.5/1.0 res |
| `SSAtmoLodDrift` | F32, 1.0 | beside `SSAtmoLodDrops` |
| `SSAtmoSnowDebug` | S32, 0 | Render Metadata styles (field overlay, lift, whiteout density) |

Reused unchanged: `SSAtmoRunoff`, `SSAtmoRunoffScale`, `SSAtmoRunoffRadius`,
`SSAtmoParticleBudget`, `SSAtmoDensity`, the `SSAtmoWindFlow*` family.

## Phase -> touch list

| Phase | Files |
|---|---|
| 1. Erosion + drift | `sssurfacefield`, `sswindflow` (`sampleGround`), `ssatmomagic` (`liftAt`), `ssprecipitation` (`updateDrift`/`emitDrift`/`mDrift`), `sspreciprenderer` (one loop), `ssprecippreset` (fields), `settings.xml` |
| 2. Redeposit | `sssurfacefield` (`tick` deposit term, hysteresis) |
| 3. Snow surface | `ssSurfaceSnowF.glsl`, `llviewershadermgr`, `sssurfacefield` (`renderSnowPass`), `pipeline.cpp` (one call) |
| 4. Whiteout + squall | `sswhiteout.*` (new), `ssWhiteoutF.glsl`, `sswindflow` (ground window), `llviewershadermgr`, `pipeline.cpp` (one call), `ssatmomagic` (`squallFactor`), `ssatmoenvweatherstate` (squall derivation + forecast text) |
| 5. Caking, footsteps, compaction | `ssavatarwet`, `sssoundscape.cpp:~1350` (+ `SSStepSurface` enums in `ssprecippreset.h`), `sssurfacefield` (compaction channel) |
| 6. Granular runoff | `sssurfacefield` (creep + shed re-feed), `ssprecipitation` (granular stream/drip look), `ssPrecipRainF.glsl`/`ssPrecipLitF.glsl` (dither branch), `ssatmomagic` (`processImpacts` deposit routing), Sand built-in preset |

## Guards and known sharp edges

- **`mLift` is per-tick state, not per-frame.** Consumers (drift spawn, near ring) read the last
  tick's figure; the tick interval (sub-second) is far below the visibility threshold for a
  quantity that ramps over seconds.
- **`depositAt` overflow is discarded, not re-routed.** Re-routing would need edge-cell store
  access from the impact path; the repose cap makes the loss invisible (a clump landing on a full
  pile barely changes it). Revisit only if eave piles ever visibly under-fill.
- **Drift never touches tier machinery.** `mTierCount`/`mTierTarget`/`tierBands` stay the falling
  tiers' alone; the drift pool has its own cap (`SSAtmoSnowDriftBudget`) and its own cull radius,
  so a blizzard cannot starve falling snow of budget or vice versa.
- **The ground window follows the flowmap's own validity.** While a tile rebuilds, the window
  keeps the last committed cells (they are CPU-resident until replaced) - no flicker, and the
  whiteout pass gates on window age like the surface pass gates on `mWindowValid`.
- **The dither branch is keyed on the particle's material flag, not the preset**, so a granular
  preset's ripples (if any) and the near-camera ring can choose per-particle.
- **`sampleGround()` returns the solved field, gusts excluded** - every consumer applies the
  scalar gust envelope itself, once per call site, never per cell. This is the rule that keeps
  the erosion tick and the spawn walk linear.
