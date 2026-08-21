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
    if (ssWetDebugForce > 0.0)
    {
        wet = ssWetDebugForce;
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
        if (wet < 0.004)
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
        if (field.w < 0.0 || wet < 0.004)
        {
            frag_color = spec;
            return;
        }
    }

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
        spec.g = max(mix(spec.g, spec.g * ssWetRoughness, wet), ssWetRoughMin);
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

        spec.rgb = mix(spec.rgb, max(spec.rgb, vec3(target)), wet);
        spec.a = mix(spec.a, max(spec.a, ssWetGlossTarget), wet);
    }

    frag_color = spec;
}

// </SS:Nexii>
