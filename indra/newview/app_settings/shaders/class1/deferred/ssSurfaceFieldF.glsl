/**
 * @file class1/deferred/ssSurfaceFieldF.glsl
 * @brief Atmo Magic surface field lookup: what the weather has left on the
 *        surface at a point, and how much of it ever reached that point.
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

// <SS:Nexii> Atmo Magic surface field

uniform sampler2D ssFieldMap;

// xy: agent-space corner of texel zero. z: metres one cell spans.
// w: resolution, so the two together give the span.
uniform vec4 ssFieldOrigin;

// The direction precipitation is falling, agent space, normalised and pointing
// downward. The same vector the shadow map was captured along.
uniform vec3 ssFieldFall;

// How far the fall direction is spread by turbulence, as a tangent. Rain is
// not a collimated beam - it arrives over a cone of directions - and this is
// what turns the hard edge of an overhang into a penumbra. It is the gust
// figure the wind system is already tracking, not a number invented here.
uniform float ssFieldSpread;

// How much a surface facing away from the weather is spared it. At 0 a wall
// wets the same whichever way it points; at 1 only what the rain can actually
// see gets wet.
uniform float ssFieldFacing;

#define SS_FIELD_STEPS 8

// The raw field. Height in x - or a very large negative where the stitch found
// no surface at all - then wetness, snow depth and standing depth.
vec4 ssFieldFetch(vec2 xy_agent)
{
    float span = ssFieldOrigin.z * ssFieldOrigin.w;
    vec2 uv = (xy_agent - ssFieldOrigin.xy) / span;

    // Outside the window there is nothing to say. Clamping would smear the
    // border texel across the whole world instead.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0))))
    {
        return vec4(-1.0e6, 0.0, 0.0, 0.0);
    }
    return texture(ssFieldMap, uv);
}

// Cheap per-fragment hash, stable in world space so the pattern it drives
// does not swim as the camera moves - unlike a screen-space hash, which
// would.
float ssFieldHash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// One ray back up the fall direction. Returns 1 where something is in the way.
float ssFieldRay(vec3 origin, vec3 dir, float cell)
{
    float step_len = cell * 1.25;
    for (int s = 1; s <= SS_FIELD_STEPS; ++s)
    {
        vec3 q = origin - dir * (float(s) * step_len);
        vec4 f = ssFieldFetch(q.xy);

        // The bias keeps a surface from sheltering itself out of its own
        // sampling noise, at the cost of letting the weather a few
        // centimetres under a lip
        if (f.x > q.z + cell * 0.35) return 1.0;
    }
    return 0.0;
}

// What the weather has left here, weighted by how much of it arrives.
//   x  wetness, 0 to 1
//   y  settled snow depth, metres
//   z  standing water depth, metres
//   w  exposure, 0 fully sheltered to 1 open sky; negative where the window
//      holds no answer at all and the caller should leave the surface alone
vec4 ssFieldAt(vec3 p_agent, vec3 n_agent)
{
    float cell = ssFieldOrigin.z;
    vec4 here = ssFieldFetch(p_agent.xy);
    if (here.x < -1.0e5) return vec4(0.0, 0.0, 0.0, -1.0);

    float exposure;
    if (p_agent.z >= here.x - cell * 0.5)
    {
        // The top surface of its own column, so nothing can be sheltering it.
        // Most of what is on screen at any moment is ground or roof, which
        // makes this the path that decides what the whole effect costs.
        exposure = 1.0;
    }
    else
    {
        // Something is stacked above this point. Walk back up the way the
        // weather came and find out whether it is actually in the way.
        //
        // The march starts a cell out along the surface normal, which is what
        // lets a wall be treated separately from the roof it shares a cell
        // with. Without it the top metre of every wall in the world reads as
        // sheltered by the eave directly above it.
        vec3 origin = p_agent + n_agent * (cell * 0.75);

        // Five rays across a small cone around the fall direction rather than
        // two points on a line either side of it. Each ray is still a binary
        // hit test, and five of them can only ever answer in sixths - on
        // their own that reads as six clean, concentric stripes tracing the
        // sheltering geometry's own silhouette, which is a worse look than
        // the hard edge it replaced. Rotating the ray fan by a per-fragment
        // random angle is what actually sells the softness: it does not add
        // any more information at a single point, but it means neighbouring
        // fragments right at the same true exposure level land on different
        // sides of their own six-level answer, so the coherent stripe breaks
        // up into noise - the same reason PCF shadow filters and SSAO jitter
        // their kernels rather than using every fragment's sample points
        // unrotated.
        vec3 tangent = normalize(cross(ssFieldFall, vec3(0.0, 0.0, 1.0)) + vec3(1.0e-4, 0.0, 0.0));
        vec3 bitangent = cross(ssFieldFall, tangent);

        float ang = ssFieldHash(p_agent.xy) * 6.28318530718;
        float ca = cos(ang), sa = sin(ang);
        vec3 t = tangent * ca + bitangent * sa;
        vec3 b = bitangent * ca - tangent * sa;

        float blocked = ssFieldRay(origin, ssFieldFall, cell);
        blocked += ssFieldRay(origin, normalize(ssFieldFall + t * ssFieldSpread), cell);
        blocked += ssFieldRay(origin, normalize(ssFieldFall - t * ssFieldSpread), cell);
        blocked += ssFieldRay(origin, normalize(ssFieldFall + b * ssFieldSpread), cell);
        blocked += ssFieldRay(origin, normalize(ssFieldFall - b * ssFieldSpread), cell);
        exposure = 1.0 - blocked * 0.2;
    }

    // Driven weather wets the face it can see and leaves the lee of it alone.
    // Straight down this does nothing, which is correct: in still air a
    // vertical wall stays dry whichever way it points.
    float facing = mix(1.0, clamp(dot(n_agent, -ssFieldFall), 0.0, 1.0), ssFieldFacing);
    exposure *= facing;

    return vec4(here.y, here.z, here.w, exposure);
}

// </SS:Nexii>
