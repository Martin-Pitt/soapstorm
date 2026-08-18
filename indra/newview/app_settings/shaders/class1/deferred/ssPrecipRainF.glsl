/**
 * @file ssPrecipRainF.glsl
 * @brief Atmo Magic rain particle fragment shader (SS:Nexii): water-like
 *        droplets with screen refraction, probe environment reflection and a
 *        sun/moon specular glint over a fake cylindrical normal.
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseMap;   // splatted drops, alpha = coverage
uniform sampler2D sceneMap;     // last frame's lit scene (SSR buffer)
uniform vec2 screen_res;
// eye-space dominant light direction; must match the vec3 declaration in
// windlight/atmosphericsFuncs.glsl exactly or the link fails
uniform vec3 lightnorm;
uniform float ss_refract_strength;

in vec3 vary_position;
in vec4 vertex_color;
in vec2 vary_texcoord0;

vec3 srgb_to_linear(vec3 cs);
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten);
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);
void mirrorClip(vec3 pos);

void main()
{
    mirrorClip(vary_position);

    float splat = texture(diffuseMap, vary_texcoord0.xy).a;
    float final_alpha = splat * vertex_color.a;
    if (final_alpha < 0.004)
    {
        discard;
    }

    vec3 pos = vary_position;
    vec3 view = normalize(pos);

    // Fake cylindrical water normal: bulges toward the viewer, bending
    // sideways across the sprite; enough surface variation for specular,
    // reflection and refraction to read as water
    vec2 c = vary_texcoord0.xy * 2.0 - 1.0;
    vec3 norm = normalize(vec3(c.x * 0.6, c.y * 0.15, 1.0));

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVars(pos.xyz, vec3(0), 1.0, sunlit, amblit, additive, atten);
    vec3 amblit_linear = srgb_to_linear(amblit);

    // Environment reflection through the probe system; the class2 fallback
    // approximates from sky ambient when probes are disabled
    vec3 irradiance = amblit_linear;
    vec3 glossenv = vec3(0);
    vec3 legacyenv = vec3(0);
    vec2 frag_tc = gl_FragCoord.xy / screen_res;
    sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, frag_tc, pos.xyz, norm, 0.9, 1.0, true, amblit_linear);

    // Refraction: pull last frame's scene sideways through the droplet;
    // without an SSR buffer fall back to ambient transmission
    vec3 transmitted = irradiance;
    if (ss_refract_strength > 0.0)
    {
        vec2 refract_tc = clamp(frag_tc + norm.xy * ss_refract_strength, vec2(0.001), vec2(0.999));
        transmitted = texture(sceneMap, refract_tc).rgb;
    }

    // Water fresnel: transmit head-on, reflect the environment at the edges;
    // biased reflective so the water look reads at streak scale
    float ndv = clamp(dot(norm, -view), 0.0, 1.0);
    float fres = 0.06 + 0.94 * pow(1.0 - ndv, 2.5);

    // Sun/moon glint plus forward-scatter sparkle when looking lightward
    vec3 light_dir = lightnorm.xyz;
    float spec = pow(clamp(dot(reflect(view, norm), light_dir), 0.0, 1.0), 96.0);
    float scatter = pow(clamp(dot(-view, -light_dir), 0.0, 1.0), 8.0) * 0.25;

    vec4 color;
    color.rgb = mix(transmitted, glossenv, fres);
    color.rgb += srgb_to_linear(sunlit) * (spec * 1.5 + scatter) * splat;
    color.rgb *= vertex_color.rgb;
    color.a = final_alpha;

    color.rgb = applySkyAndWaterFog(pos, additive, atten, color).rgb;

    frag_color = max(color, vec4(0));
}
