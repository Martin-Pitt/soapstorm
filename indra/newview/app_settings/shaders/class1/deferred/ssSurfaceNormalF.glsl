/**
 * @file class1/deferred/ssSurfaceNormalF.glsl
 * @brief Atmo Magic wet surfaces: normal flattening. A water film smooths out
 *        the micro-relief a rough surface scatters its highlight over, and
 *        the visible result of that is a flatter normal, not only a tighter
 *        specular lobe on the original bumpy one. This is the second half of
 *        the wet look; ssSurfaceWetF.glsl is the first.
 *
 * A companion pass to ssSurfaceWetF.glsl rather than folded into it: that
 * shader is already proven end to end, and every early return in it would
 * have needed a matching normal output added by hand to extend it in place.
 * A second, independent pass over the same field costs one more full-screen
 * triangle and touches none of that proven code.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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
 * $/LicenseInfo$
 */

// <SS:Nexii> Atmo Magic wet surfaces - normal flattening

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform mat4 ssFieldInvView;

uniform float ssWetStrength;
uniform float ssWetDebugForce;
uniform float ssWetSkipExposure;

// How far a fully wet surface's normal leans toward world up, 0 leaves the
// normal alone and 1 goes all the way flat. Deliberately its own dial rather
// than reusing wet directly - a puddle wants this at 1, a merely damp wall
// wants barely any of it, and that is a judgement about the look, not
// something the wetness value itself should decide.
uniform float ssWetNormalFlatten;

// Cosines of the two angles from vertical-up that bound the taper: at or
// below ssWetFlattenCosFull the surface is close enough to flat that water
// on it pools the way it does on a roof or the ground, and gets the full
// flatten amount; at or above ssWetFlattenCosZero it is close enough to a
// wall that water on it runs as a thin sheet following the wall's own plane
// rather than pooling flat, and gets none. Uploaded as cosines rather than
// angles so the shader never has to take an inverse cosine to use them - the
// surface's own normal dotted with up is already a cosine.
uniform float ssWetFlattenCosFull;
uniform float ssWetFlattenCosZero;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);
vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);
vec4 ssFieldAt(vec3 p_agent, vec3 n_agent);
vec4 ssFieldFetch(vec2 xy_agent);

void main()
{
    vec2 tc = vary_fragcoord.xy;

    // The env intensity and gbuffer flag live in the same texel as the
    // encoded normal but are not part of what decodeNormal reconstructs -
    // exactly the trap ssSurfaceWetF.glsl's own flag read fell into earlier.
    // Both have to come from the raw fetch and go back out unchanged; only
    // the normal itself is ever supposed to move.
    vec4 raw = getNormRaw(tc);
    float flag = raw.w;
    float env = raw.z;

    // Sky, stars, the sun disc, HDRI - none of them are surfaces with a
    // normal to flatten
    if (GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_HDRI) ||
        GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        frag_color = raw;
        return;
    }

    float depth = getDepth(tc);
    vec4 pos_view = getPositionWithDepth(tc, depth);
    vec3 p = (ssFieldInvView * vec4(pos_view.xyz, 1.0)).xyz;

    vec3 n_view = decodeNormal(raw).xyz;
    vec3 n_world = normalize(mat3(ssFieldInvView) * n_view);

    float wet;
    if (ssWetDebugForce > 0.0)
    {
        wet = ssWetDebugForce;
    }
    else if (ssWetSkipExposure > 0.0)
    {
        vec4 field = ssFieldFetch(p.xy);
        if (field.x < -1.0e5)
        {
            frag_color = raw;
            return;
        }
        wet = field.y * ssWetStrength;
        if (wet < 0.004)
        {
            frag_color = raw;
            return;
        }
    }
    else
    {
        vec4 field = ssFieldAt(p, n_world);
        wet = field.x * field.w * ssWetStrength;
        if (field.w < 0.0 || wet < 0.004)
        {
            frag_color = raw;
            return;
        }
    }

    // How much this surface's own tilt lets water pool flat on it at all.
    // Roofs and the ground read close to 1; a wall reads close to 0, because
    // water clinging to a wall runs down as a sheet that still follows the
    // wall's own plane rather than levelling out the way standing water
    // does. Without this a vertical surface would flatten exactly as much as
    // a horizontal one for the same wetness, which is what turned every wet
    // wall into a puddle standing on its side.
    //
    // The gbuffer's normal is the final SHADING normal - whatever a bump or
    // normal map perturbed it to - not the flat geometric surface underneath.
    // "Is this a wall or a roof" is a question about the geometry, not the
    // brickwork on it, and answering it from the pixel normal would have a
    // heavily bump-mapped vertical wall flattening in some pixels and not
    // others depending on which way each individual bump happened to tilt.
    // The gradient of view-space position across the screen is the actual
    // surface the geometry describes, independent of any normal map, and
    // costs nothing beyond two derivatives already sitting in hardware.
    vec3 n_geo_view = cross(dFdx(pos_view.xyz), dFdy(pos_view.xyz));
    if (dot(n_geo_view, -pos_view.xyz) < 0.0) n_geo_view = -n_geo_view;
    vec3 n_geo_world = normalize(mat3(ssFieldInvView) * n_geo_view);

    float up_align = dot(n_geo_world, vec3(0.0, 0.0, 1.0));
    float slope_factor = smoothstep(ssWetFlattenCosZero, ssWetFlattenCosFull, up_align);

    // Blended toward world up rather than toward some notion of the
    // surface's own unweathered flat direction, so a sloped wet roof still
    // tilts its highlight the way a real film of water lying or running on
    // it would - the water's surface answers to gravity, not to whatever the
    // material underneath happens to be shaped like.
    float flatten = clamp(wet * ssWetNormalFlatten * slope_factor, 0.0, 1.0);
    vec3 flat_world = normalize(mix(n_world, vec3(0.0, 0.0, 1.0), flatten));

    // Back to view space the same way the exposure march's normal input got
    // to world space in the first place, undone: ssFieldInvView's rotational
    // part is orthonormal, so its transpose is its inverse and there is no
    // need for a second matrix upload just to run the transform backward.
    vec3 flat_view = normalize(transpose(mat3(ssFieldInvView)) * flat_world);

    frag_color = encodeNormal(flat_view, env, flag);
}

// </SS:Nexii>
