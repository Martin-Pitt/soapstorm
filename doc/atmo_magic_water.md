# Atmo Magic: the SSWater plane family

Atmo Magic renders its own water plane geometry - `SSWater`, `SSEdgeWater` and `SSFarWater` in
`indra/newview/sswater.h/cpp` - instead of bending the stock `LLVOWater`/`LLVOVoidWater` objects to
its will. The stock planes stay pristine: whatever we later do to Atmo water (tides from the
keyframed height, per-tile waterlines, horizon-reaching geometry) lands in the SS classes and the
stock viewer water is untouched and instantly recoverable by switching Atmo off.

## The family

| Class | Base | Covers | Status |
|---|---|---|---|
| `SSWater` | `LLVOWater` | one plane per connected region, at that region's own water height | live |
| `SSEdgeWater` | `LLVOVoidWater` | 256x256m void tiles ringing the regions out toward the projection far plane | live |
| `SSFarWater` | `LLVOVoidWater` | the band from the tile ring to the true horizon | stub |

All three reuse the stock water machinery wholesale - `LLVOWater::updateGeometry`, the existing
`PARTITION_WATER`/`PARTITION_VOIDWATER` spatial partitions, `POOL_WATER`, the water shaders, water
haze, refraction and the exclusion mask - **and the stock pcodes** (`LL_VO_WATER` /
`LL_VO_VOID_WATER`), so every pcode-keyed check in the viewer (the static-drawable assert, octree
debug colours, the non-interactive guard, blocked-neighbour culling) treats both families
identically with nothing registered anywhere. The only discriminator is the `mIsAtmoWater` flag on
`LLVOWater`, set by the SS constructors. Because the pcodes are shared, the pcode factory cannot
build the subclasses: `SSWaterWorld` news them directly and hands them to
`gObjectList.adoptViewerObject`, a small helper that does the same bookkeeping
`createObjectViewer` would have.

## The swap

`SSWaterWorld` (singleton, `sswater.cpp`) ticks once per frame from `llviewerdisplay.cpp`, right
after `SSAtmoEnvApplier::apply()` so it reads the same frame's active state. When the applier is
active (Atmo enabled, asset loaded, tracks present) it builds the SS family and the stock planes
stop drawing; when inactive the SS family is destroyed and stock water returns. Exactly one family
renders on any given frame.

The suppression is at the two draw call sites, not at object lifecycle: stock water objects keep
existing (the region water object doubles as the region's water height store - `LLSurface` reads
its position - so it must never be killed). `LLDrawPoolWater::pushWaterPlanes` and the
`pushFaceGeometry` override (the water haze re-push) both gate each face through
`SSWaterWorld::drawsThisFrame`, which compares the face's `mIsAtmoWater` flag against the frame's
owner.
Both families share one `mDrawFace` list in the single water pool, so these two sites are the
complete set of render entry points.

## What drives the water look

Nothing new. The applier already installs its own `LLSettingsWater` into `ENV_LOCAL` and writes
every keyframed `SSAtmoEnvWater` param (fog colour/density, fresnel, normal map, wave speeds,
refraction scales, blur) into it per frame; `LLDrawPoolWater` reads `getCurrentWater()`. Since
SSWater renders through that same pool, the whole keyframed param set applies to SSWater and
SSEdgeWater together - one track, one look, sim water and void water in lockstep.

The **water plane checkbox** (`water_enabled_check`, `SSAtmoEnvWater::mEnabled`) also needs nothing
new: the applier's `setWaterRendering(false)` flips `RENDER_TYPE_WATER` off, which hides water and
void water alike - an empty void, water removed from the sim and the surrounding void together.
When neighbouring sims exist we currently assume they share this water; per-neighbour environments
and cross-sim track sharing are future work.

## SSEdgeWater tiling

Stock void water is nine stretched slabs (hole fillers plus an 8-piece skirt). SSEdgeWater is
instead a flat grid of 256x256m tiles - region-sized units - so each tile can later carry its own
waterline, per-neighbour height, or shoreline treatment without re-splitting geometry.

- Tiles are laid on the 256m global grid, anchored to the agent region's origin, and trimmed to a
  circle of radius `reach = max(MAX_FAR_CLIP * 0.7 - region_width / 2, 512)` around the region
  centre. The 0.7 (~1/sqrt(2)) cap is the same rationale as the stock skirt fix in
  `LLWorld::updateWaterObjects`: any triangle whose far corner crosses `MAX_FAR_CLIP`, the constant
  projection far plane, gets sliced by the projection and rasterises as a black band along the
  horizon. Water is deliberately drawn past the draw distance (the water partitions set
  `mInfiniteFarClip`), so the projection far plane - not the draw distance - is what "up to the far
  clip" means here.
- A cell occupied by any connected region is skipped: the region gets a full-size `SSWater` plane
  at its own water height instead, and holes between regions get tiles naturally.
- Tiles use the agent region's water height (the stock assumption) and the stock void slab
  convention: position Z at `256 + water_height` with Z scale 512, which puts the rendered quad at
  water height while the bounding slab reaches up for visibility culling.
- Rebuilds are keyed on a per-frame signature (applier active, agent region handle and water
  height, region set hash, water height sum, transparent-water setting) plus a dead-object sweep;
  a rebuild kills and recreates the whole set, which is cheap at ~100 small objects and only fires
  on region changes, water height changes or the Atmo toggle.

## Deferred / known limits

- **Keyframed water height (tide) is authored but not yet applied to SS geometry**, matching the
  applier's long-standing deferral (`doc/archive/atmo_magic_environment.md`). With SS-owned planes the old
  objection - plane height is region state, fighting the sim - falls away for the *rendered* plane,
  but underwater detection, fog flips and the water clip plane all key off
  `LLEnvironment::getWaterHeight()` (region height), so moving only the visible plane desyncs them.
  Applying tide means teaching those consumers about the Atmo height first.
- **SSFarWater is a stub.** It exists so the class and manager slot are in place; the
  actual work - water all the way to the horizon, past `MAX_FAR_CLIP`, presumably eye-anchored
  geometry with a bespoke projection or shader trick (see the removed far-sea experiment in git
  history for constraints) - is unstarted.
- On very large var regions the camera can sit far from the region centre the tile circle is
  anchored to, thinning coverage in the camera's direction; the `reach` subtraction of half the
  region width keeps the slicing guarantee but the ring is not camera-centred. Camera-anchored
  tiling (with hysteresis to avoid rebuild churn) is the fix if it ever shows.
- Neighbour regions with their own Atmo environments, and tracks shared across sims, are future
  work; today every tile and every `SSWater` plane wears the agent's track.
