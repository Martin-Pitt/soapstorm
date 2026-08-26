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

`ss_cloud_alt_m` is the LAYER'S OWN altitude - it replaced the `max_y` proxy
this first shipped with (max altitude is an atmosphere ceiling authored for
haze, not a cloud height, and the old `cloud_realism` smoothstep existed only
to patch over that mismatch for low art-directed domes).

It is now an authored dome parameter: `SSAtmoEnvCloudDome::mHeightM`, keyframed
like every other value on the Clouds > Sky Dome tab, defaulting to 6000m. The
altitude is a parallax RATE, not a position - the band is drawn on a fixed dome
radius either way (`renderDome(..., 0.3325f)`), and the number only says how far
away the layer behaves.

Ticking the tab's Auto box hands the number back to the derivation this shipped
with, kept in `SSAtmoEnvApplier::autoCloudDomeAltitudeMetres()`: cirrus-high
(6 km) while the volumetric field is empty, merging quickly down onto the deck's
mid-altitude as the field's coverage builds (smoothstep over coverage
0.05..0.30) - so exactly when the dome band and the deck merge visually at the
rim, they also agree about where the cloud IS and parallax at the same rate.
That is what an authored height gives up, and why the Auto box exists rather
than the derivation simply being deleted.

Resolved in one place, `SSAtmoEnvApplier::cloudDomeAltitudeMetres()`, per call
rather than cached with the rest of the sky walk - the auto branch reads the
volumetric field's live coverage, which moves between applies. No Atmo
environment driving the sky means nobody authored a height, so the derivation
stands in. The disc occlusion in ssCelestialF.glsl mirrors the same gate and
altitude; keep the three in sync.

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
- The rate is normalised by the layer altitude (`ss_cloud_alt_m`) — not by
  region size. Region width (256 m stock, more on a
  varregion) has nothing to do with how high the clouds sit; typical EEP
  `max_y` defaults to ~1605 m and presets are generally 1000–2000 m+. Dividing
  by region width instead made a short walk shift the pattern as if the
  clouds were only as high as one region is wide. Dividing by the altitude
  means a short walk under a high cirrus layer barely shifts it while a low
  storm deck slides properly, same as real clouds.
- The UV shift reaches `1/16` once you've walked a distance equal to the
  layer altitude
  from the region centre (the `/16` is the `vary_texcoord2`/`vary_texcoord3`
  compensation above, not a separate dial).
- The parallax is horizontal only — gaining or losing camera altitude does
  not add to it; the layer altitude is "N metres above the camera",
  re-centred every frame, rather than a fixed world height.
- It is a uniform shift, not distance-projected, so it does not fall off
  between zenith and horizon the way true motion parallax would (see the
  rejected design above). The whole cloud pattern slides together at one
  rate as you walk.

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
