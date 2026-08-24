/**
 * @file class1/deferred/ssSurfaceWetF.glsl
 * @brief Atmo Magic wet surfaces. A screen space pass over the gbuffer that
 *        tightens the specular lobe of anything the weather has been falling
 *        on, and touches nothing else.
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

// <SS:Nexii> Atmo Magic wet surfaces

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D specularRect;

// Agent space from view space. The field is anchored to the world; everything
// the gbuffer hands back is relative to the eye.
uniform mat4 ssFieldInvView;

// Master scale on the whole effect, so a user who wants none of it pays for
// none of it and one who wants it subtle can have that instead
uniform float ssWetStrength;

// What a soaked surface's roughness is multiplied by, and the floor it may
// never go below. The floor is not cosmetic: a perfect mirror in the gbuffer
// is what the screen space reflections fall apart on.
uniform float ssWetRoughness;
uniform float ssWetRoughMin;

// Legacy materials carry glossiness rather than roughness, and a great deal of
// older content sits at zero - multiplying that stays at zero forever, so the
// wet value converges on a target instead of scaling what is already there.
uniform float ssWetGlossTarget;

// The specular colour a water film puts on a legacy surface that had none.
// Written in the same sRGB-ish encoding the legacy gbuffer uses, so a quarter
// here lands near the 0.04 a dielectric reflects once soften linearises it.
uniform float ssWetSpecular;

// A lower target for legacy surfaces that carried no baked specular data at
// all - terrain always writes exactly zero here, by construction, and a
// great deal of plain, matte content does too. A surface with nothing shiny
// about it to begin with is unlikely to turn glossy under rain the way an
// already-finished one does, and on terrain specifically the normal is
// coherent over a large area with none of the micro-detail that breaks a
// tight highlight into many small glints instead of one sliding blob - so it
// wants noticeably less specular energy than ordinary content, not just a
// slightly lower amount of the same thing.
uniform float ssWetSpecularMatte;

// Standing water is not the same thing as a damp surface, and reusing the
// wetness dials for it would have made the two impossible to tune apart: a
// puddle is a pool of actual water sitting on top of the material rather
// than a film soaked into it, so it wants to read as close to a mirror as
// this pass can make it, independent of how shiny the material underneath
// would ever get from being merely rained on. Driven off the drainage's own
// standing-depth channel rather than a second wetness figure, because a
// puddle is a place water collects and stays, which is exactly what that
// channel already tracks.
uniform float ssWetPuddleDepthFull;   // standing depth, metres, that reads as a full puddle
uniform float ssWetPuddleRoughness;   // PBR roughness multiplier at full puddle
uniform float ssWetPuddleRoughMin;    // PBR roughness floor at full puddle
uniform float ssWetPuddleSpecular;    // legacy specular colour target at full puddle
uniform float ssWetPuddleGloss;       // legacy gloss target at full puddle

// Diagnostic. Above zero this is used as the wetness for every fragment the
// pass reaches, ignoring the field, the exposure and the shelter march
// entirely. It answers one question and only one: does anything this pass
// writes reach the screen. Everything else here is downstream of that.
uniform float ssWetDebugForce;

// Diagnostic. Above zero, samples the REAL field texture for wetness at this
// fragment's own position, but skips the exposure march entirely (treats
// every fragment as fully exposed). Isolates the field lookup and coordinate
// math from the shelter/overhang logic - the one piece of this shader that
// has never been independently verified, everything else having been proven
// tonight one layer at a time.
uniform float ssWetSkipExposure;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);
vec4 ssFieldAt(vec3 p_agent, vec3 n_agent);
vec4 ssFieldFetch(vec2 xy_agent);

//-----------------------------------------------------------------------------
// Avatars
//
// The field cannot answer for them - it is a 2D thing describing the top of
// each column, and a person is something standing IN a column - so they carry
// their own wetness, tested here as a capsule per avatar. See ssavatarwet.h.
//-----------------------------------------------------------------------------

#define SS_AVATAR_MAX 8

// How far above the stored surface a fragment has to stand before it counts
// as something other than the surface itself. The field's own heights are
// cell averages, so a real floor can sit a little either side of one.
const float SS_AVATAR_LIFT = 0.15;

uniform int ssAvatarCount;
uniform vec4 ssAvatarPos[SS_AVATAR_MAX];    // xyz foot position, w radius
uniform vec4 ssAvatarShape[SS_AVATAR_MAX];  // x height, y soak

// How wet a point at height fraction t up a body is, given how soaked that
// body is overall.
//
// Rain arrives from above, so the head and shoulders go first. The feet are
// close behind them but for the opposite reason - they are being splashed
// from the ground rather than rained on - and the middle of the body is last,
// which is why a light shower reads as damp hair and wet boots on an
// otherwise dry person while a downpour eventually soaks all of them.
//
// Each height has a soak threshold it starts wetting at; the two curves below
// are "distance from the head" and "distance from the feet", and a band takes
// whichever of the two reaches it first.
float ssAvatarWetAt(float t, float soak)
{
    // Every threshold has to be reachable, which these did not used to be.
    //
    // Soak runs 0 to 1, and the old curves peaked at 1.15 - so the middle of
    // a body needed more soak than exists and could never wet at all, however
    // long someone stood in a downpour. Only the two ends came within range,
    // and the band between them switched on all at once at whatever soak
    // finally crossed it, which is the hard edge with no gradient.
    //
    // Now the far end of both curves is 0.72: the last part of a body to wet
    // does so around three quarters soaked, and is fully wet before soak
    // reaches 1.
    float from_head = mix(0.72, 0.02, smoothstep(0.25, 1.0, t));
    float from_feet = mix(0.12, 0.72, smoothstep(0.0, 0.40, t));
    float threshold = min(from_head, from_feet);

    // The band wets over a range rather than switching on at its threshold,
    // so the boundary between wet and dry is a gradient up the body instead
    // of a waterline.
    return smoothstep(threshold, threshold + 0.30, soak);
}

// Wetness from whichever avatar capsule contains this fragment, 0 if none.
float ssAvatarWet(vec3 p_agent)
{
    float best = 0.0;

    for (int i = 0; i < SS_AVATAR_MAX; ++i)
    {
        if (i >= ssAvatarCount) break;

        vec3 foot = ssAvatarPos[i].xyz;
        float radius = ssAvatarPos[i].w;
        float height = ssAvatarShape[i].x;
        float soak = ssAvatarShape[i].y;

        // Generous slack under the soles and over the head.
        //
        // mBodySize is the shape's own measurement of a body, and what gets
        // drawn is not only that: hair, hats, heels, a hovering avatar and
        // anything rigged past the skeleton all sit outside it. With only
        // 15cm underneath and 25cm above, the capsule ended somewhere around
        // the shoulders and started above the shoes - so the feet and the
        // head fell outside it entirely and a band across the legs was the
        // only part that could wet. Hence knees, and nothing else.
        float rel = p_agent.z - foot.z;
        if (rel < -0.40 || rel > height * 1.15 + 0.50) continue;

        vec2 off = p_agent.xy - foot.xy;
        if (dot(off, off) > radius * radius) continue;

        float t = clamp(rel / max(height, 0.1), 0.0, 1.0);

        // Softened at the capsule's rim so the edge of the test is not a
        // visible cylinder cut across a shoulder.
        float edge = 1.0 - smoothstep(radius * 0.75, radius, length(off));
        best = max(best, ssAvatarWetAt(t, soak) * edge);
    }

    return best;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 spec = texture(specularRect, tc);

    float depth = getDepth(tc);

    // decodeNormal() reconstructs xyz from the octahedral encoding but never
    // assigns w - the flag channel comes along for the ride in the same
    // texture but is not part of what that function decodes, and reading it
    // off its result is uninitialised GLSL output. The flag has to come from
    // the raw fetch; only the normal itself goes through decodeNormal.
    vec4 raw = getNormRaw(tc);
    float flag = raw.w;
    vec4 norm = decodeNormal(raw);

    // Sky, stars, the sun disc, HDRI - none of them are surfaces and none of
    // them have a specular response to spoil
    if (GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_HDRI) ||
        GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        frag_color = spec;
        return;
    }

    vec4 pos_view = getPositionWithDepth(tc, depth);
    vec3 p = (ssFieldInvView * vec4(pos_view.xyz, 1.0)).xyz;
    vec3 n = normalize(mat3(ssFieldInvView) * norm.xyz);

    float wet;
    float puddle;

    // Avatars first - but only where the field genuinely has nothing to say.
    //
    // The capsule is a screen-space pass's only way of asking "is this
    // fragment a person": there is no per-object identity in a G-buffer, so
    // a world-space cylinder stands in for one. It cannot tell a shin from
    // the floor between two feet, and on its own it claimed both - handing
    // the ground the avatar's soak and zeroing its puddle. That is the dry
    // island that followed people around.
    //
    // The field already draws the line the capsule cannot: ssFieldAt rejects
    // anything standing ABOVE the stored surface height, which is exactly
    // what an avatar's body is and exactly what the floor is not. Pairing the
    // two makes them complementary rather than overlapping - the capsule
    // answers only for fragments the field has declined, and the ground under
    // someone stays as wet as the ground beside them.
    vec4 field_here = ssFieldFetch(p.xy);
    bool field_knows = field_here.x > -1.0e5;
    bool on_surface = field_knows && (p.z <= field_here.x + SS_AVATAR_LIFT);

    float avatar_wet = on_surface ? 0.0 : ssAvatarWet(p);
    if (avatar_wet > 0.004)
    {
        // No puddle term: water stands on a floor, not on a person. What is
        // on them is a film, which is exactly what the wet path below does.
        wet = avatar_wet * ssWetStrength;
        puddle = 0.0;
    }
    else if (ssWetDebugForce > 0.0)
    {
        wet = ssWetDebugForce;
        puddle = ssWetDebugForce;
    }
    else if (ssWetSkipExposure > 0.0)
    {
        vec4 raw = ssFieldFetch(p.xy);
        if (raw.x < -1.0e5)
        {
            // Outside the stitched window entirely
            frag_color = spec;
            return;
        }
        wet = raw.y * ssWetStrength;
        puddle = clamp(raw.w / ssWetPuddleDepthFull, 0.0, 1.0);
        if (wet < 0.004 && puddle < 0.004)
        {
            frag_color = spec;
            return;
        }
    }
    else
    {
        vec4 field = ssFieldAt(p, n);

        // Outside the window, or nothing has fallen here yet. Either way the
        // surface is left exactly as the material author wrote it.
        wet = field.x * field.w * ssWetStrength;

        // Standing depth is not scaled by exposure the way wet is: a puddle
        // under an eave was filled by water that ran there, not by rain
        // falling on that exact spot, so whether the sky above it is open
        // right now says nothing about whether it is still full.
        puddle = clamp(field.z / ssWetPuddleDepthFull, 0.0, 1.0);
        if (field.w < 0.0 || (wet < 0.004 && puddle < 0.004))
        {
            frag_color = spec;
            return;
        }
    }

    // What actually drives the blend toward the wet/puddle look below - a
    // spot can be a full puddle while the general wetness pass call for this
    // frame is low (just after the rain stopped, say), and it should still
    // shine like standing water rather than fade with the film around it.
    float wetBlend = max(wet, puddle);

    if (GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_PBR))
    {
        // Occlusion, roughness, metal. Only the middle one moves.
        //
        // Water fills the micro-relief a rough surface scatters its highlight
        // over, so the same specular energy comes back in a tighter lobe: a
        // brighter, sharper highlight and a sharper reflection, with nothing
        // added to the light budget. That is the whole reason this is the one
        // channel worth touching - it reads as wet without overwriting a
        // single thing the creator authored.
        float rough_mul = mix(ssWetRoughness, ssWetPuddleRoughness, puddle);
        float rough_min = mix(ssWetRoughMin, ssWetPuddleRoughMin, puddle);
        spec.g = max(mix(spec.g, spec.g * rough_mul, wetBlend), rough_min);
    }
    else
    {
        // The legacy gbuffer packs specular colour in rgb and a Blinn-Phong
        // exponent in alpha, and gloss runs the opposite way to roughness -
        // the same tightening is an increase here, not a decrease.
        //
        // The colour has to move as well, and that is not optional. Every
        // legacy path that spends the exponent multiplies by the colour first
        // - the sun highlight does, and so does the gloss environment - and a
        // plain prim carries black there. Raising the exponent alone opens the
        // gate and then multiplies by nothing, which is a great deal of work
        // for no pixels.
        //
        // A water film is a weak neutral dielectric laid over whatever was
        // underneath, so the wet end of this is a floor rather than a cap: it
        // puts a specular on a surface that had none and never takes one away
        // from a surface whose author gave it one. But wet has to be the
        // blend factor toward that floor, not baked into the floor itself -
        // max(spec.rgb, ssWetSpecular*wet) makes the floor rise WITH wet,
        // which means whether the transition looks smooth or like a switch
        // depends on how bright the surface already was relative to that
        // rising floor, different for every piece of content. Blending
        // toward a fixed target the way the gloss line already does makes
        // the ramp wet-driven and predictable regardless of what the
        // surface started at.
        // Terrain, and plain content like it, carries no baked specular at
        // all to begin with - checked once, before wet moves anything, so
        // the choice of target does not itself depend on what wet already
        // did to spec this frame.
        bool matte = max(max(spec.r, spec.g), max(spec.b, spec.a)) < 0.02;
        float target = matte ? ssWetSpecularMatte : ssWetSpecular;
        target = mix(target, ssWetPuddleSpecular, puddle);
        float gloss_target = mix(ssWetGlossTarget, ssWetPuddleGloss, puddle);

        spec.rgb = mix(spec.rgb, max(spec.rgb, vec3(target)), wetBlend);
        spec.a = mix(spec.a, max(spec.a, gloss_target), wetBlend);
    }

    frag_color = spec;
}

// </SS:Nexii>
