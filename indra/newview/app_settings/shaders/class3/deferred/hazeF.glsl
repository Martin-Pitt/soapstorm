/**
 * @file class3/deferred/hazeF.glsl
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
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

out vec4 frag_color;

// Inputs
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int  sun_up_factor;
in vec2 vary_fragcoord;

vec4 getNorm(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);

float getDepth(vec2 pos_screen);

vec3 linear_to_srgb(vec3 c);
vec3 srgb_to_linear(vec3 c);

uniform vec4 waterPlane;

#ifdef SS_ATMO
// <SS:Nexii> The Atmo far-field squash band (knee, cap, rim), bound by doAtmospherics only while the far sea is live and zeroed otherwise. [interaction: SSFarSea, waterV.glsl] </SS:Nexii>
uniform vec3 ss_squash;
#endif

uniform int cube_snapshot;

uniform float sky_hdr_scale;

void main()
{
    vec2  tc           = vary_fragcoord.xy;
    float depth        = getDepth(tc.xy);
    vec4  pos          = getPositionWithDepth(tc, depth);

#ifdef SS_ATMO
    // <SS:Nexii> Un-squash for aerial perspective: the depth buffer holds DRAWN positions, and the far-field squash compresses everything from the knee to the rim (kilometres to hundreds of km)
    // into a few hundred drawn metres - so haze all but stopped growing past the knee, the far sea never bathed in the horizon glow, and the growth-rate kink rasterised as a ring on the knee
    // circle. The pixel-is-squashed-water test rides pure RAY geometry - the view ray dips below the water plane and the drawn distance is past the knee - never the reconstructed position's
    // height: un-squashing amplifies depth-buffer quantization by the squash ratio (one ~1m LSB near the far plane becomes tens of metres), while the sea only sits ~2 eye-heights below the plane
    // out there, so any height threshold strobes per pixel - the black spike forest. Nothing but squashed water lives below the plane past the knee (the knee exceeds the max draw distance and
    // clouds sit above); a longer-than-knee draw distance would merely over-haze sub-eye-level scenery there. The analytic ray-plane distance floors the inverted one because it is depth-noise
    // FREE and near-exact in the mid-band where the ~240m quantization steps of the inversion terraced the haze; in the far band the droop makes it undershoot and the inversion takes over.
    // [interaction: SSFarSea] </SS:Nexii>
    if (ss_squash.y > ss_squash.x && ss_squash.z > ss_squash.x && waterPlane.w > 0.0)
    {
        float ss_dd = length(pos.xyz);
        if (ss_dd > ss_squash.x)
        {
            vec3 ss_rd = pos.xyz / ss_dd;
            float ss_es = -dot(ss_rd, waterPlane.xyz);
            if (ss_es > 0.0)
            {
                // The sea distance is fully ANALYTIC - never the inverted depth value, whose ~1m quantization amplifies to ~240m+ true-distance steps and rendered as camera-centred haze rings.
                // Ray direction is per-pixel exact, so this is noise-free. DIAGNOSTIC BASELINE: the sea is currently a dead-flat plane out to the rim (waterV.glsl), so the distance is simply the
                // ray-plane hit capped at the rim; the drooped-sphere solve that matches a planet-curved sea is in git history and must come back in lockstep with the droop.
                float ss_true = min(waterPlane.w / ss_es, ss_squash.z);
                pos.xyz = ss_rd * max(ss_true, ss_dd);
            }
        }
    }
#endif

    vec4  norm         = getNorm(tc);
    vec3  light_dir   = (sun_up_factor == 1) ? sun_dir : moon_dir;

    vec3  color = vec3(0);
    float bloom = 0.0;

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;

    calcAtmosphericVarsLinear(pos.xyz, norm.xyz, light_dir, sunlit, amblit, additive, atten);

    // mask off atmospherics below water (when camera is under water)
    bool do_atmospherics = false;

    if (dot(vec3(0), waterPlane.xyz) + waterPlane.w > 0.0 ||
        dot(pos.xyz, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        do_atmospherics = true;
    }

    vec3  irradiance = vec3(0);
    vec3  radiance  = vec3(0);

    if (depth >= 1.0)
    {
        //should only be true of sky, clouds, sun/moon, and stars
        discard;
    }

   float alpha = 0.0;

    if (do_atmospherics)
    {
        alpha = atten.r;
        color = srgb_to_linear(additive*2.0);
        color *= sky_hdr_scale;
    }
    else
    {
        color = vec3(0,0,0);
        alpha = 1.0;
    }

    frag_color = max(vec4(color.rgb, alpha), vec4(0)); //output linear since local lights will be added to this shader's results

}
