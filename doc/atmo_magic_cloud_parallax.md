# Windlight cloud parallax

Stock windlight clouds glide along with the camera 1:1. The cloud dome is
retranslated onto the camera's true position every frame, but the cloud UV
(`texcoord0` in `cloudsV.glsl`) is baked per-vertex from the dome mesh's own
local *direction* only (see `llvowlsky.cpp`) — it carries no information about
where the camera actually is. Same screen direction always samples the same
UV, no matter where you stand, so the pattern never appears to move against
the world; it just follows you.

## Design considered and rejected

The "correct" fix looked like reusing the ray/plane projection the vertex
shader already computes for lighting (`rel_pos`, scaled so `rel_pos.y ==
max_y`) and adding the camera's true position to it before deriving UV from
that world-space point. That gives real distance falloff — near-zenith
clouds would track close to 1:1 with the camera, horizon clouds barely move —
because the ray to the horizon travels much further to reach the same
`max_y` layer.

In practice this swapped the whole UV basis from a bounded, hand-tuned
cosine-ish mapping to an unbounded perspective one, and broke the sky
everywhere, not just near the camera: the magic constants downstream (`*16`
tiling, `0.0125` self-shadow offset, etc.) were calibrated against the old
bounded mapping. It also degenerated to sampling one texel across the entire
dome when the effect was dialed to zero, instead of reproducing the stock
look.

## What shipped instead

A uniform additive nudge on top of the untouched stock UV, `#ifdef SS_ATMO`
gated so a stock environment compiles the pristine texcoord path:

```glsl
float metres_per_uv = 16.0 * ss_cloud_alt_m * cloud_scale;
vary_texcoord0.xy += vec2(region_offset.x, -region_offset.y) / metres_per_uv;
vary_texcoord0.xy += vec2(-ss_cloud_drift.x, ss_cloud_drift.y) / metres_per_uv;
```

`ss_cloud_alt_m` is the band's height above the camera, and there is ONE band on the dome mesh:

- **The dome band** — the cirrus layer. Its height is the Sky Dome's ANIMATABLE
  height param (`SSAtmoEnvCloudDome::mHeightM`, keyframed like every other value
  on that tab, default 6000m) relative to the owning track's floor — the same
  convention both decks' base heights use, so an imported day cycle's height
  keyframes play through it and a sky build's track carries it whole
  (`SSAtmoEnvApplier::cloudDomeAltitudeMetres`). Moisture never moves it: an
  earlier derivation merged the band down onto the deck's mid-altitude as the
  deck's coverage built, which let three hundredths of moisture drag a 6 km
  cirrus deck down onto a 1 km storm. The cirrus belongs at the cirrus level;
  the ONLY thing that brings it down is convection — as the deck anvils (the
  same ramp that flattens the deck's own tops, 0.6..0.9) the band descends onto
  the deck's lid, ending ~300 m over the deck's max height: a towering anvil
  reaches UP and hits the cirrus, never the other way round. Its density is the
  live sky's cloud shadow — the tracked blend of the authored coverage lifted
  toward the deck's — which also dims the world, so band, deck and world light
  overcast in lockstep.

(A two-band design — a separate cirrus veil at the authored height, over an overcast band
tracking the deck — shipped and was removed. Two passes over one noise texture with per-band
parallax rates ghost apart the moment the camera moves: the same pattern twice, shifted, a second
ghost layer. And a band's altitude cancels out of its own static pattern entirely — the plane
reach scales with altitude and so does the metres-per-UV — so the veil's height dial had nothing
to show for it.)

The height is a parallax RATE and a curvature radius, not a dome-mesh position - the band is
drawn on a fixed dome radius either way (`renderDome(..., 0.3325f)`); the number says how far
away the deck behaves and how strongly the planet curves it.

The `/16.0` compensates for `vary_texcoord2`/`vary_texcoord3`, built as
`vary_texcoord0 * 16` a few lines down — the fine detail/self-shadow layer
that actually dominates perceived cloud shape and motion. Without it, any
shift added to the base UV shows up 16x stronger than intended: before this
compensation existed, clouds visibly swam within a few steps, and only read
as real cloud drift once hand-tuned down to a `SSAtmoCloudParallax` debug
value of ~0.005-0.04 (tested live in the viewer, not guessed). With the
`/16` folded in and confirmed against the same live viewer, that reduces to
a flat `1.0` - no separate scale constant needed, so the debug setting was
dropped once it stopped doing anything.

- `region_offset` (`lldrawpoolwlsky.cpp`) is the camera's true region
  position minus the region's centre, in metres. Centring avoids a bias from
  always measuring off the SW corner; it does not affect the parallax rate.
- **The tile is pinned** (`SS_DOME_TILE_M`, 8000 m). The divisor
  used to be `2 * height * cloud_scale`, which cancelled the height out of
  the flat mapping's own static pattern — reach scaled with height and so did
  metres-per-UV, so the pattern never moved vertically at all — and it let
  the texture zoom every time the band's altitude moved and let an imported
  day cycle's keyframed Cloud Scale dial breathe the whole dome, zoom and
  parallax rate swinging together cycle after cycle. Pinned, one tile is one
  fixed piece of world: the height above the camera survives into the pattern
  (vertical parallax on both the flat and curved paths), altitude changes
  slide instead of zoom, and cloud_scale stays out of the Atmo render
  entirely — imported cycles animating it can no longer reach the dome.
  Deliberately enormous: at 8 km a tile spans more than the visible plane at
  deck heights, so neither octave's tiling can read — the repetition problem
  the warp machinery once chased is gone at the root.
- **The fine layers run at 2x, not stock's 16x** (`SS_FINE_LAYER`): stock's 16
  put the fine tile at a few hundred metres of world - dozens of copies of the
  same clump across the sky, marching in rows under the perspective
  compression. At 2x the fine tile is half the broad one (4 km): two close
  octaves, each covering more sky than the eye can span. Stock's texcoord path
  keeps its 16.
- **The broad octave can read its own map** (`SSAtmoEnvCloudDome::
  mLargeNoiseTexture`, the Sky Dome tab's "Large Noise" picker): the warps,
  base octave and self-shadow read the authored large-scale map when one is
  set, the fine octave keeps the cloud noise. At an 8 km tile the cloud
  noise's blob scale is too small to art-direct the broad composition. Null
  keeps every octave on the cloud noise; keyframable like every dome param.
- The horizontal camera travel and wind drift run DAMPED (one eighth) to
  match the rate of the vertex nudge this replaced — the eye tuned to that
  rate, and the undamped plane rate read as the deck swimming. The vertical
  term is honest: rising toward the deck's height expands the pattern
  radially from the zenith at the true rate, and the band dissolves across
  its own altitude (40..300 m) because the mapping degenerates as the camera
  meets the plane — the deck's own volume takes over exactly there. Rays
  heading away from the plane see none of it, so a camera above the band sees
  it from above and clear sky overhead.
- It is computed per FRAGMENT now, from the true view ray (cloudsV hands the
  camera-relative ray down in `vary_ray_dir`; cloudsF intersects it with the
  band's deck and maps the intersection point). The first version shifted the
  dome mesh's own texcoords by a uniform amount per VERTEX, which was correct
  in rate but wrong in two ways: the shift was interpolated linearly across
  the dome mesh's triangles while the true mapping is nonlinear in exactly
  the place that matters (the horizon), and the base mapping was the dome's
  curvature rather than the deck's - a real deck compresses into the horizon
  at tan(elevation); the dome mesh compresses at whatever rate its vertices
  are laid out. Intersecting the ray with the deck per fragment makes the
  anchored parallax exact and the curvature the deck's own.
- **No domain warp.** An earlier cut ran three nested warp levels at incommensurate
  frequencies and rotated frames - an attempt at aperiodic tiling against the old,
  small tile, where the same clumps genuinely marched across the sky in rows. It
  read as smear and buckle, not as aperiodicity: a displacement field sampled from
  the same map it displaces is itself periodic, so the warped grid was still a
  grid, just bent. The tile scale-up (next bullet) removed the repetition at its
  root - the visible plane spans barely one tile of each octave - and the mapping
  is a straight, honest lookup now.
- **The fine octaves fade with distance** (terrain-LOD logic, `SS_DETAIL_LO_M`
  / `SS_DETAIL_HI_M`, 8–20 km of ray): past ~8 km the fine tiles sit under a
  degree wide and perspective compresses their tiling into tight horizontal
  stripes - an anisotropic squeeze no warp can decorrelate, because it is the
  mapping itself, not the repetition. The fine octave's voice in the opacity
  and in the storm variance fades over the same range (the term is zero-mean,
  so the far field's coverage is untouched - only its texture simplifies), and
  the broad warped layer carries the far deck alone: structure without
  resolvable detail, which is what a real distant sheet is.
- The deck CURVES: cloudsF intersects the ray with a sphere centred on the
  planet at radius `orbit + deck height` (`ss_planet_orbit_m` is the camera's
  distance from the home body's centre, home radius plus camera height), so
  the deck is a finite disc that terminates at its own curved horizon - the
  tangent elevation sqrt(2*height/orbit), about 1.4 degrees for a 1500 m deck
  under a 5000 km home planet - instead of stretching flat into the world's
  horizon line, and it sweeps down under a camera that climbs past it. The
  band's alpha fades across the last stretch of the approach to the edge
  (ss_deck_edge_fade), so the rim reads as a curved cloud horizon dissolving
  into the atmosphere. A track with no home body falls back to an Earth-sized
  default radius rather than to flat (see the applier) - "no planet authored"
  should not read as "flat cartoon sky", and the curved rim is the visible
  tell of the curvature. No-orbit flat fallback remains as the shader's
  orbit-0 guard, whose grazing reach softens as (1+F)*height/(|up| + F) -
  never the hard max(up, 0.02) clamp an earlier cut shipped: that froze the
  UVs into an azimuth-only stripe field below ~1.2 degrees and kinked the mip
  selection into a grid of tile boundaries at the clamp line.
- metres_per_uv anchors the cloud_scale dial to stock: stock's dome texcoords
  tile every 2*cloud_scale radians of arc at the zenith, and a tile of
  2*height*cloud_scale metres subtends exactly that from a camera one deck
  height below - so overhead clouds match stock EEP at the same slider
  setting. Away from the zenith the deck tiles denser than stock's
  direction-linear projection on purpose: a real deck compresses toward its
  horizon where stock stretches.
- The world-anchored terms - camera travel and wind drift - run DAMPED to
  one eighth of the plane-honest rate (SS_PARALLAX_DAMP): the shipped vertex
  nudge moved at that rate (its /16 compensation over the stock zenith tile),
  hand-tuned in the live viewer, and the undamped rate read as the deck
  swimming. The ray's own hit keeps the honest geometry; only the terms that
  move are damped.

## The altitude parameter

The old `max_y`-driven version needed a `smoothstep(1000, 1600, max_y)`
"realism" fade because max altitude doubles as the artist's pull-the-dome-
close dial and the parallax got STRONGER exactly on the most stylised skies.
With the layer altitude decoupled from max_y that patch is gone: a stylised
sky simply is not driving `ss_cloud_alt_m` low unless its weather says so.

WMO cloud altitude bands (mid-latitudes) for reference:

- Low (stratus/cumulus/stratocumulus): surface–2,000 m — most common by raw
  global cloud-cover fraction
- Mid (altocumulus/altostratus): 2,000–7,000 m — the "scattered clouds across
  open sky" look distant/atmospheric skies are often going for
- High (cirrus/cirrostratus): 5,000–13,000 m (higher in the tropics)

`max_y` is the sky editor's "Maximum Altitude" slider (Atmosphere tab,
`panel_settings_sky_atmos.xml`, bound straight to `getMaxY()`/`setMaxY()`),
range 0–10,000 m, stock default 1605 m — so it's the same value an author is
already setting by hand, not an internal-only quantity.

A `smoothstep(1000.0, 1600.0, max_y)` multiplier fades the whole effect to
zero below ~1000 m (clearly a close decorative dome) and to full strength by
~1600 m — low clouds are the most common band, so the ramp is centred there
rather than pushed up toward mid-altitude. Stock default (1605 m) sits right
at the top of the ramp, at (or effectively at) full strength, so an
untouched preset moves normally rather than at some partial-strength
in-between. The `1/max_y` falloff above the band still does the
physically-correct thing on its own — a tall, thin cirrus-height dome
(5,000 m+) still barely parallaxes, same as the real thing would.

## Sign convention

Traced from `llvowlsky.cpp`'s vertex bake (`texcoord0 = ((-z0+1)/2,
(-x0+1)/2)`, where `(x0,y0,z0)` is the dome vertex's local direction) through
the vertex shader's own `vec2(-texcoord0.x, texcoord0.y)` flip, then through
the dome's `gGL.rotatef(120°, (1,1,1))` into world space (same permutation
`LLEnvironment::toLightNorm()` uses for `lightnorm`: local `x = world Y`,
local `z = world X`): `vary_texcoord0.x` runs with `+world X`,
`vary_texcoord0.y` runs with `-world Y`. `region_offset` is passed raw
(world X, world Y); the shader applies this sign convention itself.
