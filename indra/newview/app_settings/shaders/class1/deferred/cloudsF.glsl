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

// One band's base UVs: intersect the view ray with the band's deck, anchor at the region centre,
// subtract the wind travel, and divide by the band's metres-per-UV. vary_ray_dir rides the dome
// mesh's Y-up local space (renderDome's 120 degree permute: local y is world UP, local x is world
// Y, local z is world X), so the horizontal components reach deck_m as (ray.z, ray.x) - east,
// north, matching region_offset's (world X, world Y) order.
//
// THE DECK CURVES. With ss_planet_orbit_m set, the ray meets a SPHERE centred on the planet at
// radius orbit + deck height - t = -orbit*dy + sqrt(orbit^2*dy^2 + 2*orbit*alt + alt^2) - so the
// deck is a finite disc that terminates at its own curved horizon (the tangent elevation
// sqrt(2*alt/orbit), about 1.4 degrees for a 1500 m deck under a 5000 km home planet) instead of
// stretching flat into the world's horizon line, and climbs overhead into view when the camera
// rises past it. Orbit 0 keeps the flat-deck fallback for an environment without a home body.
//
// metres_per_uv anchors the cloud_scale dial to stock: stock's dome texcoords tile every
// 2*cloud_scale radians of arc at the zenith, and a tile of 2*alt*cloud_scale metres subtends
// exactly that from a camera alt metres under the deck - so overhead clouds match stock EEP at
// the same slider setting. Away from the zenith this mapping tiles denser than stock's
// direction-linear projection on purpose: a real deck compresses toward its horizon where stock
// stretches (its tiles blow up), which is the whole point of the design.
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
vec2 ss_plane_base(float alt)
{
    const float SS_DECK_FOLD     = 0.1;
    const float SS_PARALLAX_DAMP = 0.125;
    float dy = vary_ray_dir.y;
    float reach;
    if (ss_planet_orbit_m > 0.0 && alt > 0.0)
    {
        float a = ss_planet_orbit_m;
        float disc = a * a * dy * dy + 2.0 * a * alt + alt * alt;
        reach = -a * dy + sqrt(max(disc, 0.0));
    }
    else
    {
        reach = (1.0 + SS_DECK_FOLD) * alt / (abs(dy) + SS_DECK_FOLD);
    }
    vec2 deck_m  = vec2(vary_ray_dir.z, vary_ray_dir.x) * reach;
    vec2 world_m = SS_PARALLAX_DAMP * (region_offset - ss_cloud_drift);
    float metres_per_uv = 2.0 * alt * cloud_scale;
    return vec2(deck_m.x + world_m.x, -deck_m.y - world_m.y) / metres_per_uv;
}

// The curved deck's own horizon fade. The deck exists ABOVE the tangent elevation
// sqrt(2*alt/orbit) - below it the ray passes under the shell's rim and there is no deck at all -
// and the last stretch before the rim compresses endlessly, so the band dissolves across the
// approach: alpha zero at the rim, full a fraction of the rim's elevation above it. The edge
// reads as a curved cloud horizon melting into the atmosphere rather than a smeared seam, and a
// low storm deck and a high cirrus shell each fade across about a third of their own rim height.
float ss_deck_edge_fade(float alt)
{
    if (ss_planet_orbit_m <= 0.0 || alt <= 0.0) return 1.0;
    float a = ss_planet_orbit_m;
    float edge_dy = sqrt(max(2.0 * a * alt + alt * alt, 0.0)) / a;
    return smoothstep(edge_dy, edge_dy * 1.35, vary_ray_dir.y);
}
#endif

vec4 cloudNoise(vec2 uv)
{
   vec4 a = texture(cloud_noise_texture, uv);
   vec4 b = texture(cloud_noise_texture_next, uv);
   vec4 cloud_noise_sample = mix(a, b, blend_factor);
   return cloud_noise_sample;
}

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
#ifdef SS_ATMO
    if (ss_cloud_plane > 0.0)
    {
        uv1 = ss_plane_base(ss_cloud_alt_m);
        uv2 = uv1 + vec2(lightnorm.x, lightnorm.z) * 0.0125;
        uv3 = uv1 * 16.0;
        uv4 = uv2 * 16.0;
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
    vec2 disturbance2 = vec2(cloudNoise((uv1 + uv3) / 4.0f).x, cloudNoise((uv4 + uv2) / 8.0f).x) * cloud_variance * (1.0f - cloud_scale * 0.25f);

    // Offset texture coords
    uv1 += cloud_pos_density1.xy + (disturbance * 0.2);    //large texture, visible density
    uv2 += cloud_pos_density1.xy;   //large texture, self shadow
    uv3 += cloud_pos_density2.xy;   //small texture, visible density
    uv4 += cloud_pos_density2.xy;   //small texture, self shadow

    float density_variance = min(1.0, (disturbance.x* 2.0 + disturbance.y* 2.0 + disturbance2.x + disturbance2.y) * 4.0);

    cloudDensity *= 1.0 - (density_variance * density_variance);

    // Compute alpha1, the main cloud opacity

    float alpha1 = (cloudNoise(uv1).x - 0.5) + (cloudNoise(uv3).x - 0.5) * cloud_pos_density2.z;
    alpha1 = min(max(alpha1 + cloudDensity, 0.) * 10 * cloud_pos_density1.z, 1.);

    // And smooth
    alpha1 = 1. - alpha1 * alpha1;
    alpha1 = 1. - alpha1 * alpha1;

    alpha1 *= altitude_blend_factor * deck_edge_fade;
    alpha1 = clamp(alpha1, 0.0, 1.0);

    // Compute alpha2, for self shadowing effect
    // (1 - alpha2) will later be used as percentage of incoming sunlight
    float alpha2 = (cloudNoise(uv2).x - 0.5);
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

