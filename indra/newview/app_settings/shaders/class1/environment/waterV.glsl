/**
 * @file class1\environment\waterV.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

uniform mat4 modelview_matrix;
uniform mat3 normal_matrix;
uniform mat4 modelview_projection_matrix;

in vec3 position;


void calcAtmospherics(vec3 inPositionEye);

uniform vec2 waveDir1;
uniform vec2 waveDir2;
uniform float time;
uniform vec3 eyeVec;
uniform float waterHeight;
uniform vec3 lightDir;

#ifdef SS_ATMO
// <SS:Nexii> The Atmo far-field squash band (knee, cap, virtual radius), shared verbatim with the cloud field and lightning so water, cloud and bolt agree about drawn depth. </SS:Nexii>
uniform vec3 ss_squash;
// <SS:Nexii> The far sea's frame: ss_sea = (blend start as a chebyshev fraction of the frame, rim radius, sea height, planet radius); ss_sea_hole = the stock-water union rect (min xy, max xy)
// the frame lattice hangs from, which also world-anchors the waves - no snapped origin needed. ss_sea is all zeros for every draw except the sea's own - see the placement block in main.
// [interaction: SSFarSea] </SS:Nexii>
uniform vec4 ss_sea;
uniform vec4 ss_sea_hole;
#endif

out vec4 refCoord;
out vec4 littleWave;
out vec4 view;
out vec3 vary_position;
out vec3 vary_light_dir;
out vec3 vary_tangent;
out vec3 vary_normal;
out vec2 vary_fragcoord;

float wave(vec2 v, float t, float f, vec2 d, float s)
{
   return (dot(d, v)*f + t*s)*f;
}

void main()
{
    //transform vertex
    vec4 pos = vec4(position.xyz, 1.0);

#ifdef SS_ATMO
    // <SS:Nexii> The far sea arrives as an immutable canonical FRAME lattice in units of the stock-water rect's half extent: |xy| = 1 is the rect edge, the frame reaches out to chebyshev 4 (the
    // 128/32 cell ratio in SSFarSea::build), and the interior is absent except a one-cell apron tucked inside the rect that the depth sink hides under stock water - seam pinholes and the <=0.5m
    // origin-rounding mismatch land on sunken sea instead of void. Built once, never rebuilt - everything varying is a uniform. [interaction: SSFarSea] </SS:Nexii>
    vec3 ss_true_pos = position.xyz;
    if (ss_sea.y > 0.0)
    {
        // Affine placement from the rect: the frame's inner edge lands exactly on the stock footprint (per-axis half extents, so a neighbour-stretched rect stays connected at the cost of a few
        // percent of cell squareness), and cells continue outward in the same world steps - undistorted UVs, waves world-anchored by the rect itself.
        vec2 sea_c = 0.5 * (ss_sea_hole.xy + ss_sea_hole.zw);
        vec2 sea_half = 0.5 * (ss_sea_hole.zw - ss_sea_hole.xy);
        vec2 sea_lat = sea_c + position.xy * sea_half;
        // Blend to the horizon: chebyshev fraction of the frame (0.5 is the rect edge - the 64/32 cell ratio in SSFarSea::build - and 1 the outer square edge), smoothstepped from the knee-derived
        // start in ss_sea.x to 100% at the edge, each vertex pulled along its own camera ray toward the rim circle. MESH-anchored, not camera-distance-anchored, so the rect and apron are ALWAYS
        // identity - a camera-distance ramp let far-side seam vertices pick up a few percent of a 100km pull and tear kilometres off the rect edge. w = 1 exactly at the outer edge, so the square's
        // rim IS the round horizon; the camera stays inside the identity zone whenever stock water is on screen, which keeps the ray directions sweeping the circle monotonically.
        float sea_cheb = max(abs(position.x), abs(position.y)) * 0.5;
        float sea_w = smoothstep(ss_sea.x, 1.0, sea_cheb);
        vec2 sea_world = sea_lat;
        if (sea_w > 0.0)
        {
            vec2 sea_rd = sea_lat - eyeVec.xy;
            float sea_rl = length(sea_rd);
            vec2 sea_dir = sea_rl > 1e-3 ? sea_rd / sea_rl : vec2(0.0, 1.0);
            sea_world = mix(sea_lat, eyeVec.xy + sea_dir * ss_sea.y, sea_w);
            // A fat rect can put mid-blend lattice positions past the rim itself; folded back onto the rim circle they collapse into slivers ON the horizon, instead of mapping past the squash
            // cap and rasterising as a far-clipped wall around it - which is what the first-cut 4x frame did.
            vec2 sea_rr = sea_world - eyeVec.xy;
            float sea_rrl = length(sea_rr);
            if (sea_rrl > ss_sea.y)
            {
                sea_world = eyeVec.xy + sea_rr * (ss_sea.y / sea_rrl);
            }
        }

        float sea_rc = length(sea_world - eyeVec.xy);
        // Sunk below the authored level: 5cm near, so stock region water wins depth ties outright, ramping to 3m past the knee where the squash compresses drawn-depth separation hundreds-fold
        // and centimetres would land inside depth-buffer precision.
        float sea_sink_t = clamp((sea_rc - ss_squash.x) / 600.0, 0.0, 1.0);
        float sea_sink = 0.05 + 2.95 * sea_sink_t * sea_sink_t;
        // Planet droop d^2/2R: at the tangent distance the sea has dropped by exactly the eye height, so the visible horizon is a sphere's silhouette. w = 0 means a flat world.
        float sea_droop = ss_sea.w > 0.0 ? sea_rc * sea_rc / (2.0 * ss_sea.w) : 0.0;
        ss_true_pos = vec3(sea_world, ss_sea.z - sea_sink - sea_droop);
        pos.xyz = ss_true_pos;
    }
#endif

    mat4 modelViewProj = modelview_projection_matrix;

    vary_position = (modelview_matrix * pos).xyz;
    vary_light_dir = normal_matrix * lightDir;
    vary_normal = normal_matrix * vec3(0, 0, 1);
    vary_tangent = normal_matrix * vec3(1, 0, 0);

    vec4 oPosition;

    //get view vector
    vec3 oEyeVec;
    oEyeVec.xyz = pos.xyz-eyeVec;

    float d = length(oEyeVec.xy);
    float ld = min(d, 2560.0);

    pos.xy = eyeVec.xy + oEyeVec.xy/d*ld;
    view.xyz = oEyeVec;

    d = clamp(ld/1536.0-0.5, 0.0, 1.0);
    d *= d;

#ifdef SS_ATMO
    // <SS:Nexii> ss_true_pos equals position for everything except the far sea disc, whose true placement was computed above. </SS:Nexii>
    oPosition = vec4(ss_true_pos, 1.0);
#else
    oPosition = vec4(position, 1.0);
#endif
//  oPosition.z = mix(oPosition.z, max(eyeVec.z*0.75, 0.0), d); // SL-11589 remove "U" shaped horizon

#ifdef SS_ATMO
    // <SS:Nexii> Far-field squash, per vertex: beyond the knee the vertex pulls radially toward the camera along its own ray, so the plane reaches kilometres past the far plane with the image
    // unchanged and no far-plane slicing - the black-horizon artifact the old void-water stretch cap existed to dodge. eyeVec is the camera in this same space. </SS:Nexii>
    vec3 sq_rel = oPosition.xyz - eyeVec;
    float sq_d = length(sq_rel);
    if (sq_d > ss_squash.x && ss_squash.z > ss_squash.x)
    {
        float sq_drawn = ss_squash.x + (sq_d - ss_squash.x) * (ss_squash.y - ss_squash.x) / (ss_squash.z - ss_squash.x);
        sq_drawn = min(sq_drawn, ss_squash.y * 0.999);
        oPosition.xyz = eyeVec + sq_rel * (sq_drawn / sq_d);
    }
#endif

    oPosition = modelViewProj * oPosition;

    refCoord.xyz = oPosition.xyz + vec3(0,0,0.2);

    //get wave position parameter (create sweeping horizontal waves)
    vec3 v = pos.xyz;
    v.x += (cos(v.x*0.08/*+time*0.01*/)+sin(v.y*0.02))*6.0;

    //push position for further horizon effect.
    pos.xyz = oEyeVec.xyz*(waterHeight/oEyeVec.z);
    pos.w = 1.0;
    pos = modelview_matrix*pos;

    calcAtmospherics(pos.xyz);

    //pass wave parameters to pixel shader
    vec2 bigWave =  (v.xy) * vec2(0.04,0.04)  + waveDir1 * time * 0.055;
    //get two normal map (detail map) texture coordinates
    littleWave.xy = (v.xy) * vec2(0.45, 0.9)   + waveDir2 * time * 0.13;
    littleWave.zw = (v.xy) * vec2(0.1, 0.2) + waveDir1 * time * 0.1;
    view.w = bigWave.y;
    refCoord.w = bigWave.x;

    gl_Position = oPosition;
}
