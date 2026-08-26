/**
 * @file class3/deferred/waterHazeV.glsl
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

in vec3 position;

uniform vec2 screen_res;

out vec4 vary_fragcoord;

// forwards
void setAtmosAttenuation(vec3 c);
void setAdditiveColor(vec3 c);

uniform vec4 waterPlane;

uniform int above_water;

uniform mat4 modelview_projection_matrix;

#ifdef SS_ATMO
// <SS:Nexii> Mirror of waterV.glsl's far-sea frame placement and far-field squash, kept in EXACT arithmetic sync: this pass re-draws the water geometry with depth test on and no write, so any
// drawn-position difference from the surface pass makes the depth test cut the haze away - the missing-haze wedge over the squash band was exactly that. All four uniforms are bound by
// SSFarSea (bindSquash for the stock faces, render for the frame) and zeroed after, so without Atmo this whole block is inert. [interaction: waterV.glsl, SSFarSea] </SS:Nexii>
uniform vec3 ss_squash;
uniform vec4 ss_sea;
uniform vec4 ss_sea_hole;
uniform vec3 eyeVec;
#endif

void main()
{
    //transform vertex
    vec4 pos = vec4(position.xyz, 1.0);

    if (above_water > 0)
    {
#ifdef SS_ATMO
        if (ss_sea.y > 0.0)
        {
            vec2 sea_c = 0.5 * (ss_sea_hole.xy + ss_sea_hole.zw);
            vec2 sea_half = 0.5 * (ss_sea_hole.zw - ss_sea_hole.xy);
            vec2 sea_lat = sea_c + position.xy * sea_half;
            float sea_cheb = max(abs(position.x), abs(position.y)) * 0.5;
            float sea_t = clamp((sea_cheb - ss_sea.x) / max(1.0 - ss_sea.x, 1e-4), 0.0, 1.0);
            float sea_w = (exp(4.0 * sea_t) - 1.0) / (exp(4.0) - 1.0);
            vec2 sea_world = sea_lat;
            if (sea_w > 0.0)
            {
                vec2 sea_rd = sea_lat - eyeVec.xy;
                float sea_rl = length(sea_rd);
                vec2 sea_dir = sea_rl > 1e-3 ? sea_rd / sea_rl : vec2(0.0, 1.0);
                sea_world = mix(sea_lat, eyeVec.xy + sea_dir * ss_sea.y, sea_w);
                vec2 sea_rr = sea_world - eyeVec.xy;
                float sea_rrl = length(sea_rr);
                if (sea_rrl > ss_sea.y)
                {
                    sea_world = eyeVec.xy + sea_rr * (ss_sea.y / sea_rrl);
                }
            }
            float sea_rc = length(sea_world - eyeVec.xy);
            float sea_sink = 0.05;
            if (sea_cheb < 0.5)
            {
                float sea_sink_t = clamp((sea_rc - ss_squash.x) / 600.0, 0.0, 1.0);
                sea_sink = 0.05 + 2.95 * sea_sink_t * sea_sink_t;
            }
            float sea_droop = ss_sea.w > 0.0 ? sea_rc * sea_rc / (2.0 * ss_sea.w) : 0.0;
            pos.xyz = vec3(sea_world, ss_sea.z - sea_sink - sea_droop);
        }
        vec3 sq_rel = pos.xyz - eyeVec;
        float sq_d = length(sq_rel);
        if (sq_d > ss_squash.x && ss_squash.z > ss_squash.x)
        {
            float sq_drawn = ss_squash.x + (sq_d - ss_squash.x) * (ss_squash.y - ss_squash.x) / (ss_squash.z - ss_squash.x);
            sq_drawn = min(sq_drawn, ss_squash.y * 0.999);
            pos.xyz = eyeVec + sq_rel * (sq_drawn / sq_d);
        }
#endif
        pos = modelview_projection_matrix*pos;
    }

    gl_Position = pos;

    // appease OSX GLSL compiler/linker by touching all the varyings we said we would
    setAtmosAttenuation(vec3(1));
    setAdditiveColor(vec3(0));

    vary_fragcoord = pos;
}
