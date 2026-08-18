/**
 * @file ssPrecipLitF.glsl
 * @brief Atmo Magic lit particle fragment shader (SS:Nexii): non-emissive
 *        precipitation (snow, ripples) shaded by probe ambient and the sun
 *        with directional shadow sampling, like other lit alpha objects.
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

uniform sampler2D diffuseMap;   // splatted particles, alpha = coverage
uniform vec2 screen_res;

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

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

void main()
{
    mirrorClip(vary_position);

    vec4 tex = texture(diffuseMap, vary_texcoord0.xy);
    float final_alpha = tex.a * vertex_color.a;
    if (final_alpha < 0.004)
    {
        discard;
    }

    vec3 pos = vary_position;
    // Billboards face the camera; light them like the legacy particle
    // path does, with a viewer-facing normal
    vec3 norm = -normalize(pos);

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVars(pos.xyz, vec3(0), 1.0, sunlit, amblit, additive, atten);
    vec3 amblit_linear = srgb_to_linear(amblit);

    vec2 frag_tc = gl_FragCoord.xy / screen_res;

    float shadow = 1.0;
#ifdef HAS_SUN_SHADOW
    shadow = sampleDirectionalShadow(pos.xyz, norm, frag_tc);
#endif

    // Probe irradiance as local ambient (sky fallback when probes are off)
    vec3 irradiance = amblit_linear;
    vec3 glossenv = vec3(0);
    vec3 legacyenv = vec3(0);
    sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, frag_tc, pos.xyz, norm, 0.0, 0.0, true, amblit_linear);

    // Flakes scatter light near-isotropically: ambient plus wrapped,
    // shadowed sun instead of a hard lambert term
    vec3 lit = irradiance + srgb_to_linear(sunlit) * shadow * 0.6;

    vec4 color;
    color.rgb = srgb_to_linear(tex.rgb * vertex_color.rgb) * lit;
    color.a = final_alpha;

    color.rgb = applySkyAndWaterFog(pos, additive, atten, color).rgb;

    frag_color = max(color, vec4(0));
}
