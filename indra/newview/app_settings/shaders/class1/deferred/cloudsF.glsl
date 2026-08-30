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
// raises the gate): the broad composition - the warp fields, the base octave and its self-shadow
// - reads it, while the fine octave keeps the cloud noise. At an 8 km tile the cloud noise's blob
// scale is too small to art-direct the broad composition, and one map for every octave means the
// broad sky is the fine map stretched. Gate 0 leaves every octave on the cloud noise, exactly as
// before this existed.
uniform sampler2D ss_noise_large;
uniform float ss_noise_large_on;

vec4 cloudNoiseLarge(vec2 uv)
{
    return mix(cloudNoise(uv), texture(ss_noise_large, uv), ss_noise_large_on);
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

// Metres of world per tile of the large map. See ss_plane_base - pinned against both the band's
// height and cloud_scale on purpose. Deliberately enormous: at 8 km a tile spans more than the
// visible plane at deck heights, so the broad composition never repeats across the sky, and the
// fine layer (at 2x, 4 km) carries the masses. The old 4 km tile put visible clumps at a few
// hundred metres and the same clumps marched across the whole sky in rows.
const float SS_DOME_TILE_M = 8000.0;

// The fine layers' multiplier. Stock's 16 made the fine tile a few hundred metres of world -
// dozens of copies of the same clump across the sky, marching in rows under the perspective
// compression. At 2x the fine tile is half the broad one: two close octaves, each covering more
// sky than the eye can span. Alignment is not a concern at this ratio - the fine layers read a
// rotated frame (irrational angle, see main), so the two grids can never row up regardless.
// Stock's texcoord path keeps its 16.
const float SS_FINE_LAYER = 2.0;

// One band's base UVs, and how much of the band survives. Intersect the view ray with the band's
// deck, anchor at the region centre, subtract the wind travel, and divide by a PINNED
// metres-per-uv. vary_ray_dir rides the dome mesh's Y-up local space (renderDome's 120 degree
// permute: local y is world UP, local x is world Y, local z is world X), so the horizontal
// components reach deck_m as (ray.z, ray.x) - east, north, matching region_offset's (world X,
// world Y) order.
//
// THE TILE IS PINNED - deliberately free of both the band's height and cloud_scale. The old
// 2*alt*cloud_scale divisor cancelled alt out of the flat mapping's own static pattern
// (reach divided by metres_per_uv left no h), which is why the pattern never moved vertically;
// it also zoomed the texture every time the band's altitude moved (the merge descending onto the
// deck), and let an imported day cycle's keyframed scale dial breathe the whole dome - zoom and
// parallax rate swinging together, cycle after cycle. Pinned, one tile is one fixed piece of
// world: the height above the camera survives into the pattern (vertical parallax, on the flat
// fallback too), altitude changes slide instead of zoom, and the scale dial stays out of the Atmo
// render entirely. The value is the old calibration's at the stock ceiling and default scale
// (2 x 1605 x 0.42), so the default look is preserved.
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
// The lookup is then made APERIODIC by nested domain warping - the GPU-practical equivalent of
// aperiodic tiling (Penrose and kin need per-tile art with matching rules; what a coordinate
// transform can do instead is make the composite lookup quasiperiodic, so no two patches of sky
// ever sample the same composite point). The seamless noise is a tiling texture, and the horizon
// compression marches its repeats into converging rows: every elevation where the ray crosses
// another tile multiple lands on a copy of the same lump. A single warp cannot break that - a
// displacement field sampled from the same tiling map is itself periodic, so the warped grid is
// still a grid, just bent. Three NESTED levels at incommensurate frequencies, each sampled in a
// differently-rotated frame, do break it: the pattern repeats only where all three warp fields
// AND the base map agree, which is a period no viewer will ever cross. The fine layers inherit
// the warped coordinates and add their own rotated frame, so the fine grid can neither align with
// the broad one nor repeat in step across the sky.
vec2 ss_rotate(vec2 v, float a)
{
    float c = cos(a);
    float s = sin(a);
    return vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

vec2 ss_plane_base(float alt, out float plane_fade, out float detail_fade)
{
    const float SS_DECK_FOLD     = 0.1;
    const float SS_PARALLAX_DAMP = 0.125;
    const float SS_THROUGH_LO_M  = 40.0;
    const float SS_THROUGH_HI_M  = 300.0;
    // Where the fine layers give up. Perspective compresses the deck toward its horizon, and the
    // fine detail's angular size collapses with it. With a 4 km fine tile that point sits far out
    // - sub-degree tiles only arrive near the deck's own rim - so the fade is a rim-zone cleanup
    // rather than a mid-sky tool: the last stretch before the melt, where the compression spikes,
    // lets the broad layer carry the sheet alone.
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

    vec2 deck_m  = vec2(vary_ray_dir.z, vary_ray_dir.x) * reach;
    vec2 world_m = SS_PARALLAX_DAMP * (region_offset - ss_cloud_drift);

    vec2 p = vec2(deck_m.x + world_m.x, -deck_m.y - world_m.y) / SS_DOME_TILE_M;

    // Warp level 1: the broad bend. Period ~11 tiles (44 km), amplitude over half a tile - the
    // whole deck's layout is laid out by it. Frame rotated by the golden angle so its grid and
    // the base map's share no axis.
    vec2 q1 = ss_rotate(p, 2.399963);
    p += (vec2(cloudNoiseLarge(q1 * 0.09 + vec2(0.37, 0.11)).x,
               cloudNoiseLarge(q1 * 0.09 + vec2(0.71, 0.53)).x) - 0.5) * 0.55;

    // Warp level 2: the row breaker. Period ~2.7 tiles - comparable to the ground spacing of the
    // horizon-compressed tile rows - so neighbouring rows read from visibly different parts of
    // the map. Another frame, another incommensurate frequency.
    vec2 q2 = ss_rotate(p, 1.13);
    p += (vec2(cloudNoiseLarge(q2 * 0.37 + vec2(0.11, 0.67)).x,
               cloudNoiseLarge(q2 * 0.37 + vec2(0.53, 0.29)).x) - 0.5) * 0.35;

    // Warp level 3: the fine jitter. Period ~1.2 tiles, small amplitude - it only has to knock
    // the last recognisable repeats off.
    vec2 w3 = vec2(cloudNoise(p * 0.83 + vec2(0.29, 0.83)).x,
                   cloudNoise(p * 0.83 + vec2(0.67, 0.41)).x) - 0.5;
    detail_fade = 1.0 - smoothstep(SS_DETAIL_LO_M, SS_DETAIL_HI_M, reach);
    return p + w3 * 0.12;
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
        // The fine layers read a ROTATED frame: an irrational-angle rotation between the broad
        // grid and the fine one means the two can never align their rows, anywhere in the sky -
        // the noise is isotropic, so the rotation itself paints nothing.
        uv3 = ss_rotate(uv1, -1.618034) * SS_FINE_LAYER;
        uv4 = ss_rotate(uv2, -1.618034) * SS_FINE_LAYER;
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
