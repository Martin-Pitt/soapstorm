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
// <SS:Nexii> The plane mapping (doc/atmo_magic_cloud_parallax.md). Each dome band's UVs are
// derived HERE, per fragment, from the true view ray cloudsV hands down - not from the dome mesh's
// own texcoords plus a patch. Two things that buys: the parallax is exact (a metre of camera travel
// moves the ray's intersection with the band's plane exactly one metre, instead of being
// approximated per dome-mesh vertex and linearly interpolated across triangles whose size the
// mapping never asked for), and the sky curvature is the band's own - a real flat deck compresses
// into the horizon at tan(elevation), and so does this, rather than at whatever rate the dome mesh
// happens to distribute its vertices. Per-fragment costs one normalize already paid in the vertex
// stage and one divide.
uniform vec2 region_offset;    // camera pos - region centre, metres, world X/Y
uniform vec2 ss_cloud_drift;   // metres the band has travelled on the wind, east and north
uniform float ss_cloud_alt_m;  // the BAND'S OWN altitude, metres - what a metre of camera travel is worth in uv
uniform float ss_cloud_plane;  // 1: derive UVs from the view ray (active Atmo). 0: stock dome texcoords
uniform vec3 lightnorm;        // the self-shadow offset's direction - stock derived texcoord1 from it per-vertex
in vec3 vary_ray_dir;

// One band's base UVs: intersect the view ray with a horizontal plane at the band's own altitude
// above the camera, anchor at the region centre, subtract the wind travel, and divide by the
// band's metres-per-UV. Sign conventions are the old vertex patches': world north runs down the
// texture's v, and the wind travel negates the same way. The grazing clamp holds the intersection
// finite below ~1.2 degrees of elevation, where the horizon fade has already taken the band's
// alpha to nothing.
vec2 ss_plane_base(float alt)
{
    float dz = max(vary_ray_dir.z, 0.02);
    vec2 plane_xy = region_offset + vary_ray_dir.xy * (alt / dz);
    float metres_per_uv = 16.0 * alt * cloud_scale;
    return vec2(plane_xy.x - ss_cloud_drift.x, -plane_xy.y + ss_cloud_drift.y) / metres_per_uv;
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
#ifdef SS_ATMO
    if (ss_cloud_plane > 0.0)
    {
        uv1 = ss_plane_base(ss_cloud_alt_m);
        uv2 = uv1 + vec2(lightnorm.x, lightnorm.z) * 0.0125;
        uv3 = uv1 * 16.0;
        uv4 = uv2 * 16.0;
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

    alpha1 *= altitude_blend_factor;
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

