/**
 * @file class1\deferred\cloudsF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */
/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[4];

/////////////////////////////////////////////////////////////////////////
// The fragment shader for the sky
/////////////////////////////////////////////////////////////////////////

in vec3 vary_CloudColorSun;
#ifdef SS_ATMO
in float vary_CloudGlow;   // <SS:Nexii> see cloudsV - the glow arrives separately, gated below by per-fragment thinness
#endif
in vec3 vary_CloudColorAmbient;
in float vary_CloudDensity;

uniform sampler2D cloud_noise_texture;
uniform sampler2D cloud_noise_texture_next;
uniform float blend_factor;
uniform vec3 cloud_pos_density1;
uniform vec3 cloud_pos_density2;
uniform float cloud_scale;
uniform float cloud_variance;

in vec2 vary_texcoord0;
in vec2 vary_texcoord1;
in vec2 vary_texcoord2;
in vec2 vary_texcoord3;
in float altitude_blend_factor;

vec4 cloudNoise(vec2 uv)
{
   vec4 a = texture(cloud_noise_texture, uv);
   vec4 b = texture(cloud_noise_texture_next, uv);
   vec4 cloud_noise_sample = mix(a, b, blend_factor);
   return cloud_noise_sample;
}

#ifdef SS_ATMO
// <SS:Nexii> The dome's authored LARGE-SCALE map, when one is set (lldrawpoolwlsky binds it and
// raises the gate): the broad octave and its self-shadow read it, while the fine octave keeps
// the cloud noise. The cloud noise's own blob scale is tuned for the fine octave, so a broad
// composition art-directed on it comes out samey - one map for every octave means the broad sky
// is the fine map stretched. Gate 0 leaves every octave on the cloud noise, exactly as before
// this existed.
uniform sampler2D ss_noise_large;
uniform float ss_noise_large_on;

// <SS:Nexii> The large map's crossfade partner and weight, live while the day cycle fades the
// broad octave between two authored maps. The pool pins the partner on the same map with weight
// 0 whenever no fade runs, so the inner mix below is a no-op then.
uniform sampler2D ss_noise_large_next;
uniform float ss_noise_large_blend;

vec4 cloudNoiseLarge(vec2 uv)
{
    vec4 large = texture(ss_noise_large, uv);
    if (ss_noise_large_blend > 0.0)
    {
        large = mix(large, texture(ss_noise_large_next, uv), ss_noise_large_blend);
    }
    return mix(cloudNoise(uv), large, ss_noise_large_on);
}
#else
// <SS:Nexii> The broad octave's calls in the opacity lines below are ungated so both variants
// share one body; in the stock build the large map does not exist, so the name resolves to the
// plain cloud noise and stock renders exactly what it always did.
vec4 cloudNoiseLarge(vec2 uv)
{
    return cloudNoise(uv);
}
#endif

#ifdef SS_ATMO
// <SS:Nexii> The deck mapping (doc/atmo_magic_cloud_parallax.md). Each dome band's UVs are
// derived HERE, per fragment, from the true view ray cloudsV hands down - not from the dome mesh's
// own texcoords plus a patch. Two things that buys: the parallax rate is per-band and exact in the
// anchored terms, and the sky curvature is the deck's own rather than whatever rate the dome mesh
// happens to distribute its vertices at. Per-fragment costs one normalize already paid in the
// vertex stage and a divide.
uniform vec2 region_offset;    // camera pos - region centre, metres, world X/Y
uniform vec2 ss_cloud_drift;   // metres the band has travelled on the wind, east and north
uniform float ss_cloud_alt_m;  // the BAND'S OWN height above the CAMERA, metres (world deck height minus camera)
uniform float ss_planet_orbit_m; // camera's distance from the planet's centre, metres - 0 falls back to the flat deck
uniform float ss_cloud_plane;  // 1: derive UVs from the view ray (active Atmo). 0: stock dome texcoords
uniform vec3 lightnorm;        // the self-shadow offset's direction - stock derived texcoord1 from it per-vertex
in vec3 vary_ray_dir;

// Metres of world per tile of the dome's noise. Pinned - deliberately free of both the band's
// height and cloud_scale, so one tile is one fixed piece of world: the convection merge can
// descend the cirrus band without breathing the pattern, and an imported day cycle's keyframed
// Scale dial cannot zoom the dome. Calibrated for the band's 6 km DEFAULT (the cirrus layer's
// default height): the visibly solid sky spans ~4 band-heights of reach before the horizon
// fade takes it (zenith to ~3 degrees of elevation), so a 32 km tile reads as a 3x3-to-4x4
// repeat across the dome - one tile ~5x the band's height, the broad composition the layer
// is tuned for. (The first pin's 8 km read as ~24 repeats marching into the horizon; the
// stock-anchored 2*alt*cloud_scale divisor it replaced breathed the pattern every time the
// band moved.)
const float SS_DOME_TILE_M = 32000.0;

// The fine layers' multiplier. Stock's 16 made the fine tile a fraction of the broad one -
// dozens of copies of the same clump across the sky, marching in rows under the perspective
// compression. At 2x the fine tile is half the broad one: two close octaves a single factor
// apart, so their repetitions never align into a visible grid. Stock's texcoord path keeps
// its 16.
const float SS_FINE_LAYER = 2.0;

// One band's base UVs, and how much of the band survives. Intersect the view ray with the band's
// deck, anchor at the region centre, subtract the wind travel, and divide by a PINNED
// metres-per-uv (see THE TILE IS PINNED below). vary_ray_dir rides the dome mesh's Y-up local space (renderDome's 120 degree
// permute: local y is world UP, local x is world Y, local z is world X), so the horizontal
// components reach deck_m as (ray.z, ray.x) - east, north, matching region_offset's (world X,
// world Y) order.
//
// THE TILE IS PINNED - deliberately free of both the band's height and cloud_scale. One tile is
// one fixed piece of world: the height above the camera survives into the pattern (vertical
// parallax, on the flat fallback too), and altitude changes slide instead of zoom - the
// convection merge descending onto the deck does not breathe the pattern, and an imported day
// cycle's keyframed Scale dial stays out of the Atmo render entirely. The calibration is the
// DEFAULT band height's, not the live band's - see SS_DOME_TILE_M above. (An earlier cut
// anchored the tile at 2*alt*cloud_scale - stock EEP's zenith calibration - which matched
// stock at any altitude but breathed the pattern as the band moved and cancelled the vertical
// parallax out of the static pattern entirely.)
//
// THE DECK CURVES. With ss_planet_orbit_m set, the ray meets a SPHERE centred on the planet at
// radius orbit + deck height - the deck is a finite disc that terminates at its own curved horizon
// (the tangent elevation sqrt(2*alt/orbit), about 1.4 degrees for a 1500 m deck under a 5000 km
// home planet) instead of stretching flat into the world's horizon line. The camera's own height
// rides the orbit uniform, so the shell stays at its world altitude while you fly: rising toward
// it brings its rim up and over. Above the shell the near intersection switches to the minus root
// and only down-rays hit - the deck seen from above. Orbit 0 keeps the flat-deck fallback.
//
// The flat fallback's denominator is SOFTENED, not clamped: (1+F)*alt / (|up| + F) is smooth in
// the ray everywhere, exact at the zenith, and caps the deck distance at ~10 band-altitudes in
// the horizon fold. The old hard max(up, 0.02) clamp did two kinds of damage the horizon fade
// never hid: below ~1.2 degrees it froze the UVs into an azimuth-only field, which smears the
// band into vertical stripes toward the horizon, and exactly on the clamp line the screen-space
// derivative jumps, which collapses the mip selection into a grid of tile boundaries in the
// distance. The sphere needs no fold: it is smooth to its own edge and bounded beyond it.
//
// The world-anchored terms - camera travel and wind drift - run DAMPED: the shipped vertex nudge
// moved at one eighth of the plane-honest rate (its /16 compensation over the stock 2*cs-radian
// zenith tile, hand-tuned in the live viewer), and the undamped plane rate read as the deck
// swimming. The ray's own hit keeps the honest geometry; only the terms that MOVE are damped, so
// the motion matches the version the eye tuned. Sign conventions are the old vertex patches':
// world north runs down the texture's v, and the wind travel negates the same way.
//
// NO DOMAIN WARP. An earlier cut ran three nested warp levels at incommensurate frequencies and
// rotated frames - an attempt at aperiodic tiling against the pinned tile, where the same
// clumps can genuinely march across the sky in rows toward the horizon. It read as smear and
// buckle, not as aperiodicity: a displacement field sampled from the same map it displaces is
// itself periodic, so the warped grid was still a grid, just bent. The mapping is a straight,
// honest lookup now - the repetition is softened by the fine octave's distance fade and by an
// authored large map (ss_noise_large) art-directing the broad octave when one is set.
vec2 ss_plane_base(float alt, out float plane_fade, out float detail_fade)
{
    const float SS_DECK_FOLD     = 0.1;
    const float SS_PARALLAX_DAMP = 0.125;
    const float SS_THROUGH_LO_M  = 40.0;
    const float SS_THROUGH_HI_M  = 300.0;
    // Where the fine layers give up. Perspective compresses the deck toward its horizon, and the
    // fine detail's angular size collapses with it. The fade is a rim-zone cleanup: the last
    // stretch before the melt, where the compression spikes, lets the broad layer carry the
    // sheet alone.
    const float SS_DETAIL_LO_M   = 100000.0;
    const float SS_DETAIL_HI_M   = 250000.0;

    // The band holds a signed height over the camera. Under it, up-rays hit; over it, down-rays
    // do - the deck seen from above. Rays heading away from the plane see none of it, and the band
    // dissolves across its own altitude (the mapping degenerates as the camera meets the plane,
    // and the deck's own volume takes over exactly there).
    float side = (alt >= 0.0) ? 1.0 : -1.0;
    float ah = abs(alt);
    plane_fade = step(0.0, vary_ray_dir.y * side)
               * smoothstep(SS_THROUGH_LO_M, SS_THROUGH_HI_M, ah);

    float reach;
    if (ss_planet_orbit_m > 0.0)
    {
        float a = ss_planet_orbit_m;
        if (alt >= 0.0)
        {
            float u = max(vary_ray_dir.y, 0.0);
            float disc = a * a * u * u + 2.0 * a * alt + alt * alt;
            reach = -a * u + sqrt(max(disc, 0.0));
        }
        else
        {
            float disc = a * a * vary_ray_dir.y * vary_ray_dir.y + 2.0 * a * alt + alt * alt;
            reach = -a * vary_ray_dir.y - sqrt(max(disc, 0.0));
        }
    }
    else
    {
        reach = (1.0 + SS_DECK_FOLD) * ah / (max(vary_ray_dir.y * side, 0.0) + SS_DECK_FOLD);
    }

    detail_fade = 1.0 - smoothstep(SS_DETAIL_LO_M, SS_DETAIL_HI_M, reach);

    vec2 deck_m  = vec2(vary_ray_dir.z, vary_ray_dir.x) * reach;
    vec2 world_m = SS_PARALLAX_DAMP * (region_offset - ss_cloud_drift);

    return vec2(deck_m.x + world_m.x, -deck_m.y - world_m.y) / SS_DOME_TILE_M;
}

// The curved deck's own horizon fade. The deck exists ABOVE the tangent elevation
// sqrt(2*alt/orbit) - below it the ray passes under the shell's rim and there is no deck at all -
// and the last stretch before the rim compresses endlessly, so the band dissolves across the
// approach: alpha zero at the rim, full a fraction of the rim's elevation above it. The edge
// reads as a curved cloud horizon melting into the atmosphere rather than a smeared seam, and a
// low storm deck and a high cirrus shell each fade across about a third of their own rim height.
float ss_deck_edge_fade(float alt)
{
    if (ss_planet_orbit_m <= 0.0 || alt < 0.0) return 1.0;
    float a = ss_planet_orbit_m;
    float edge_dy = sqrt(max(2.0 * a * alt + alt * alt, 0.0)) / a;
    if (edge_dy <= 0.0) return 1.0;
    return smoothstep(edge_dy, edge_dy * 1.6, vary_ray_dir.y);
}
#endif

void main()
{
    // Set variables
    vec3 cloudColorSun = vary_CloudColorSun;
    vec3 cloudColorAmbient = vary_CloudColorAmbient;
    float cloudDensity = vary_CloudDensity;

    // The four texcoords: base, base plus the self-shadow offset, and both at 16x for the fine
    // layers. Stock derives them per-vertex from the dome mesh's own mapping; the Atmo plane path
    // derives the base per-fragment from the view ray (see ss_plane_base) and rebuilds the other
    // three from it with stock's own offsets, so the two paths agree about WHAT each coordinate is
    // and differ only about where it comes from.
    vec2 uv1;
    vec2 uv2;
    vec2 uv3;
    vec2 uv4;
    float deck_edge_fade = 1.0;
    float plane_fade = 1.0;
    float detail_fade = 1.0;
#ifdef SS_ATMO
    if (ss_cloud_plane > 0.0)
    {
        uv1 = ss_plane_base(ss_cloud_alt_m, plane_fade, detail_fade);
        uv2 = uv1 + vec2(lightnorm.x, lightnorm.z) * 0.0125;
        uv3 = uv1 * SS_FINE_LAYER;
        uv4 = uv2 * SS_FINE_LAYER;
        deck_edge_fade = ss_deck_edge_fade(ss_cloud_alt_m);
    }
    else
    {
#endif
    uv1 = vary_texcoord0.xy;
    uv2 = vary_texcoord1.xy;
    uv3 = vary_texcoord2.xy;
    uv4 = vary_texcoord3.xy;
#ifdef SS_ATMO
    }
#endif

    if (cloud_scale < 0.001)
    {
        discard;
    }

    vec2 disturbance  = vec2(cloudNoise(uv1 / 8.0f).x, cloudNoise((uv3 + uv1) / 16.0f).x) * cloud_variance * (1.0f - cloud_scale * 0.25f);
    // <SS:Nexii> The fine-sourced disturbance rides the same distance fade as the fine layer
    // itself - past it the far deck's variance comes from the broad octaves alone.
    vec2 disturbance2 = vec2(cloudNoise((uv1 + uv3) / 4.0f).x, cloudNoise((uv4 + uv2) / 8.0f).x) * cloud_variance * (1.0f - cloud_scale * 0.25f) * detail_fade;

    // Offset texture coords
    uv1 += cloud_pos_density1.xy + (disturbance * 0.2);    //large texture, visible density
    uv2 += cloud_pos_density1.xy;   //large texture, self shadow
    uv3 += cloud_pos_density2.xy;   //small texture, visible density
    uv4 += cloud_pos_density2.xy;   //small texture, self shadow

    float density_variance = min(1.0, (disturbance.x* 2.0 + disturbance.y* 2.0 + disturbance2.x + disturbance2.y) * 4.0);

    cloudDensity *= 1.0 - (density_variance * density_variance);

    // Compute alpha1, the main cloud opacity
    // <SS:Nexii> The fine octave's weight rides detail_fade: past the fade's range the fine
    // tiling compresses into sub-degree rows that read as striping, so its voice in the opacity
    // fades with its angular size and the broad layer carries the far deck alone. The term is
    // zero-mean, so the fade changes the far field's TEXTURE, not its coverage.
    float alpha1 = (cloudNoiseLarge(uv1).x - 0.5) + (cloudNoise(uv3).x - 0.5) * cloud_pos_density2.z * detail_fade;
    alpha1 = min(max(alpha1 + cloudDensity, 0.) * 10 * cloud_pos_density1.z, 1.);

    // And smooth
    alpha1 = 1. - alpha1 * alpha1;
    alpha1 = 1. - alpha1 * alpha1;

    alpha1 *= altitude_blend_factor * deck_edge_fade * plane_fade;
    alpha1 = clamp(alpha1, 0.0, 1.0);

    // Compute alpha2, for self shadowing effect
    // (1 - alpha2) will later be used as percentage of incoming sunlight
    float alpha2 = (cloudNoiseLarge(uv2).x - 0.5);
    alpha2 = min(max(alpha2 + cloudDensity, 0.) * 2.5 * cloud_pos_density1.z, 1.);

    // And smooth
    alpha2 = 1. - alpha2;
    alpha2 = 1. - alpha2 * alpha2;

    // Combine
    vec3 color;
#ifdef SS_ATMO
    // <SS:Nexii> The glow reaches a fragment only through its THINNESS: the forward-scatter fire belongs to the airlight behind the cloud, so a dense core stays a dark silhouette right up to the
    // disc's edge (it gets only the anti-solar base the stock far-field carries) while the ragged fringes transmit the full glow and catch fire - which is what every backlit-cloud photograph
    // shows and the baked-in glow never could. Far from the sun haze_glow sits near its 0.25 floor, below the base cap, so open-sky cloud shading is unchanged.
    float glow_thin = (1.0 - alpha1) * (1.0 - alpha1);
    float glow_gate = mix(min(vary_CloudGlow, 0.35), vary_CloudGlow, glow_thin);
    color = (cloudColorSun*(1.-alpha2)*glow_gate + cloudColorAmbient);
#else
    color = (cloudColorSun*(1.-alpha2) + cloudColorAmbient);
#endif
    color.rgb = clamp(color.rgb, vec3(0), vec3(1));
    color.rgb *= 2.0;

    /// Gamma correct for WL (soft clip effect).

    frag_data[1] = vec4(0.0,0.0,0.0,0.0);
    frag_data[2] = vec4(0,0,0,GBUFFER_FLAG_SKIP_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(color.rgb, alpha1);
#else
    frag_data[0] = vec4(color.rgb, alpha1);
#endif
}
