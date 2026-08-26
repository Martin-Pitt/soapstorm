/**
 * @file waterF.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

// class3/environment/waterF.glsl

#define WATER_MINIMAL 1

out vec4 frag_color;

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

vec3 scaleSoftClipFragLinear(vec3 l);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
vec4 applyWaterFogViewLinear(vec3 pos, vec4 color);
#ifdef SS_ATMO
vec4 applyWaterFogViewLinearNoClip(vec3 pos, vec4 color);
// <SS:Nexii> waterFogF.glsl's view-space water plane, redeclared here for the deep-column sample below - same name and type, so the linker merges them. </SS:Nexii>
uniform vec4 waterPlane;
#endif

void mirrorClip(vec3 pos);

// PBR interface
vec2 BRDF(float NoV, float roughness);

void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);

void pbrIbl(vec3 diffuseColor,
    vec3 specularColor,
    vec3 radiance, // radiance map sample
    vec3 irradiance, // irradiance map sample
    float ao,       // ambient occlusion factor
    float nv,       // normal dot view vector
    float perceptualRoughness,
    out vec3 diffuse,
    out vec3 specular);

void pbrPunctual(vec3 diffuseColor, vec3 specularColor,
                    float perceptualRoughness,
                    float metallic,
                    vec3 n, // normal
                    vec3 v, // surface point to camera
                    vec3 l, // surface point to light
                    out float nl,
                    out vec3 diff,
                    out vec3 spec);

vec3 pbrBaseLight(vec3 diffuseColor,
                  vec3 specularColor,
                  float metallic,
                  vec3 pos,
                  vec3 norm,
                  float perceptualRoughness,
                  vec3 light_dir,
                  vec3 sunlit,
                  float scol,
                  vec3 radiance,
                  vec3 irradiance,
                  vec3 colorEmissive,
                  float ao,
                  vec3 additive,
                  vec3 atten);

uniform sampler2D bumpMap;
uniform sampler2D bumpMap2;
uniform float     blend_factor;
#ifdef TRANSPARENT_WATER
uniform sampler2D screenTex;
uniform sampler2D depthMap;
#endif

uniform sampler2D exclusionTex;

uniform int classic_mode;
uniform vec3 lightDir;
uniform vec3 specular;

// <SS:Nexii> Angular radius of the body lighting the water, in radians.
uniform float ss_light_angular_radius;

// ...and the moon's own light, for when it is the only thing up.
//
// The punctual term below is scaled by sunlit, which is what the
// ATMOSPHERE delivers from the sun - so with the sun below the horizon it
// is zero and the moon laid down no glitter path at all, however bright the
// disc in the sky. This is the light that replaces it at night.
uniform vec3 ss_moonlit;
uniform float ss_sun_up;
#ifdef SS_ATMO
// <SS:Nexii> The same squash band waterV consumes (knee, cap, rim) - bound program-wide by SSFarSea and zeroed after the sea's draw, so it reads all-zero on vanilla water. Here it keys ss_far,
// the far-ocean blend in main. [interaction: SSFarSea, waterV.glsl] </SS:Nexii>
uniform vec3 ss_squash;
#endif

uniform float blurMultiplier;
uniform float refScale;
uniform float kd;
uniform vec3 normScale;
uniform float fresnelScale;
uniform float fresnelOffset;

//bigWave is (refCoord.w, view.w);
in vec4 refCoord;
in vec4 littleWave;
in vec4 view;
in vec3 vary_position;
in vec3 vary_normal;
in vec3 vary_tangent;
in vec3 vary_light_dir;

vec3 BlendNormal(vec3 bump1, vec3 bump2)
{
    vec3 n = mix(bump1, bump2, blend_factor);
    return n;
}

vec3 srgb_to_linear(vec3 col);
vec3 linear_to_srgb(vec3 col);

vec3 atmosLighting(vec3 light);
vec3 scaleSoftClip(vec3 light);
vec3 toneMapNoExposure(vec3 color);

vec3 vN, vT, vB;

vec3 transform_normal(vec3 vNt)
{
    return normalize(vNt.x * vT + vNt.y * vB + vNt.z * vN);
}

void sampleReflectionProbesWater(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, vec3 amblit_linear);

void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit_linear);

void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit);


vec3 getPositionWithNDC(vec3 ndc);

void generateWaveNormals(out vec3 wave1, out vec3 wave2, out vec3 wave3)
{
    // Generate all of our wave normals.
    // We layer these back and forth.

    vec2 bigwave = vec2(refCoord.w, view.w);

    vec3 wave1_a = texture(bumpMap, bigwave).xyz * 2.0 - 1.0;
    vec3 wave2_a = texture(bumpMap, littleWave.xy).xyz * 2.0 - 1.0;
    vec3 wave3_a = texture(bumpMap, littleWave.zw).xyz * 2.0 - 1.0;

    vec3 wave1_b = texture(bumpMap2, bigwave).xyz * 2.0 - 1.0;
    vec3 wave2_b = texture(bumpMap2, littleWave.xy).xyz * 2.0 - 1.0;
    vec3 wave3_b = texture(bumpMap2, littleWave.zw).xyz * 2.0 - 1.0;

    wave1 = BlendNormal(wave1_a, wave1_b);
    wave2 = BlendNormal(wave2_a, wave2_b);
    wave3 = BlendNormal(wave3_a, wave3_b);
}

void calculateFresnelFactors(out vec3 df3, out vec2 df2, vec3 viewVec, vec3 wave1, vec3 wave2, vec3 wave3, vec3 wavef)
{
    // We calculate the fresnel here.
    // We do this by getting the dot product for each sets of waves, and applying scale and offset.

    df3 = max(vec3(0), vec3(
        dot(viewVec, wave1),
        dot(viewVec, (wave2 + wave3) * 0.5),
        dot(viewVec, wave3)
    ) * fresnelScale + fresnelOffset);

    df3 *= df3;

    df2 = max(vec2(0), vec2(
        df3.x + df3.y + df3.z,
        dot(viewVec, wavef) * fresnelScale + fresnelOffset
    ));
}

void main()
{
    mirrorClip(vary_position);

    vN = vary_normal;
    vT = vary_tangent;
    vB = cross(vN, vT);

    vec3 pos = vary_position.xyz;
    float linear_depth = 1 / -pos.z;

    float dist = length(pos.xyz);

    //normalize view vector
    vec3 viewVec = normalize(pos.xyz);

#ifdef SS_ATMO
    // <SS:Nexii> Far-ocean blend factor: 0 in the near field, 1 by the squash knee, riding TRUE view distance (vary_position is pre-squash), so the stock planes and the far sea frame share one
    // smooth per-pixel ramp and the rect seam sits mid-blend where both sides already agree - the soft integration across stock water. Zero whenever the band is unbound, keeping stock vanilla.
    // [interaction: SSFarSea, waterV.glsl] </SS:Nexii>
    float ss_far = ss_squash.z > ss_squash.x ? smoothstep(ss_squash.x * 0.5, ss_squash.x, dist) : 0.0;
#endif

    // Setup our waves.

    vec3 wave1 = vec3(0, 0, 1);
    vec3 wave2 = vec3(0, 0, 1);
    vec3 wave3 = vec3(0, 0, 1);

    generateWaveNormals(wave1, wave2, wave3);

#ifdef SS_ATMO
    // <SS:Nexii> Deterministic far-field flattening: at squash-band distances the pixel footprint in wave UV space is radially thousands of texels, and anisotropic filtering (which picks its mip
    // from the SHORT axis) returns azimuth-coherent noise instead of the flat average - the radial streak forests. Mixing the sampled normals to flat by ss_far takes the sampler out of the far
    // field entirely; the slope variance removed here is what the ss_far roughness widening below stands in for. </SS:Nexii>
    wave1 = mix(wave1, vec3(0.0, 0.0, 1.0), ss_far);
    wave2 = mix(wave2, vec3(0.0, 0.0, 1.0), ss_far);
    wave3 = mix(wave3, vec3(0.0, 0.0, 1.0), ss_far);
#endif

    float dmod = sqrt(dist);
    vec2 distort = (refCoord.xy/refCoord.z) * 0.5 + 0.5;

    vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;

    vec3 df3 = vec3(0);
    vec2 df2 = vec2(0);

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVarsLinear(pos.xyz, wavef, vary_light_dir, sunlit, amblit, additive, atten);

    calculateFresnelFactors(df3, df2, normalize(view.xyz), wave1, wave2, wave3, wavef);

    vec3 waver = wavef*3;

    vec3 up = transform_normal(vec3(0,0,1));
    float vdu = -dot(viewVec, up)*2;

    vec3 wave_ibl = wavef * normScale;
    wave_ibl.z *= 2.0;
    wave_ibl = transform_normal(normalize(wave_ibl));

    vec3 norm = transform_normal(normalize(wavef));

    vdu = clamp(vdu, 0, 1);
    //wavef.z *= max(vdu*vdu*vdu, 0.1);

    wavef = normalize(wavef);

    //wavef = vec3(0, 0, 1);
    wavef = transform_normal(wavef);

    float dist2 = dist;
    dist = max(dist, 5.0);

    //figure out distortion vector (ripply)
    vec2 distort2 = distort + waver.xy * refScale / max(dmod, 1.0) * 2;

    distort2 = clamp(distort2, vec2(0), vec2(0.999));

    float shadow = 1.0f;

    float water_mask = texture(exclusionTex, distort).r;

#ifdef HAS_SUN_SHADOW
    shadow = sampleDirectionalShadow(pos.xyz, norm.xyz, distort);
#endif

    vec3 sunlit_linear = sunlit;
    float fade = 1;
#ifdef TRANSPARENT_WATER
    float depth = texture(depthMap, distort).r;

    vec3 refPos = getPositionWithNDC(vec3(distort*2.0-vec2(1.0), depth*2.0-1.0));

    // Calculate some distance fade in the water to better assist with refraction blending and reducing the refraction texture's "disconnect".
#ifdef SHORELINE_FADE
    fade = max(0,min(1, (pos.z - refPos.z) / 10));
#else
    fade = 1;
#endif
    fade *= water_mask;
    distort2 = mix(distort, distort2, min(1, fade * 10));
    depth = texture(depthMap, distort2).r;

    refPos = getPositionWithNDC(vec3(distort2 * 2.0 - vec2(1.0), depth * 2.0 - 1.0));

    if (pos.z < refPos.z - 0.05)
    {
        distort2 = distort;
    }

    vec4 fb = texture(screenTex, distort2);

#ifdef SS_ATMO
    // <SS:Nexii> What the refraction texture holds behind the squash band is mostly sky, so the far sea refracted red sunsets instead of showing ocean body colour. Blend to the fogged deep-water
    // column: sample 20m below the water plane THROUGH this fragment, NoClip and black input so only the accumulated fog colour returns. The clipped form on a mid-ray point was the white far sea -
    // a grazing ray stays above the plane for kilometres, and applyWaterFogViewLinear hands back the input colour untouched for any point above it. </SS:Nexii>
    fb.rgb = mix(fb.rgb, applyWaterFogViewLinearNoClip(pos.xyz - waterPlane.xyz * 20.0, vec4(0.0)).rgb, ss_far);
#endif

#else
    vec4 fb = applyWaterFogViewLinear(viewVec*2048.0, vec4(1.0));

    if (water_mask < 1)
        discard;
#endif

    float metallic = 1.0;
    float perceptualRoughness = blurMultiplier;

    // <SS:Nexii> Widen the specular lobe by the light's own angular size.
    //
    // pbrPunctual treats the light as a point, so on its own the glitter
    // path is as narrow as the water is smooth however large the body in
    // the sky is. This is the standard sphere-light approximation - fold
    // the source's angular radius into the roughness - so a big authored
    // moon lays down a broad soft path and a small one a tight bright
    // streak, matching the disc actually drawn up there.
    perceptualRoughness = clamp(perceptualRoughness + ss_light_angular_radius, 0.0, 1.0);
    // </SS:Nexii>

#ifdef SS_ATMO
    // <SS:Nexii> Distance-widened lobe: minification averages the wave normals toward flat, and the slope variance the mips discard comes back as roughness (the LEAN/Toksvig idea, fixed-budget
    // form), so the glitter path stays broad and alive out to the horizon instead of collapsing to a mirror stripe where the waves go sub-pixel. </SS:Nexii>
    perceptualRoughness = clamp(perceptualRoughness + ss_far * 0.2, 0.0, 1.0);
#endif

    float gloss      = 1 - perceptualRoughness;

    vec3  irradiance = vec3(0);
    vec3  radiance  = vec3(0);
    vec3 legacyenv = vec3(0);

    // TODO: Make this an option.
#ifdef WATER_MINIMAL
    sampleReflectionProbesWater(irradiance, radiance, distort2, pos.xyz, wave_ibl.xyz, gloss, amblit);
#elif WATER_MINIMAL_PLUS
    sampleReflectionProbes(irradiance, radiance, distort2, pos.xyz, wave_ibl.xyz, gloss, false, amblit);
#endif

    vec3 diffuseColor = vec3(0);
    vec3 specularColor = vec3(0);
    vec3 specular_linear = srgb_to_linear(specular);
    calcDiffuseSpecular(specular_linear, metallic, diffuseColor, specularColor);

    vec3 v = -normalize(pos.xyz);

    vec3 colorEmissive = vec3(0);
    float ao = 1.0;
    vec3 light_dir = transform_normal(lightDir);

    float NdotV = clamp(abs(dot(norm, v)), 0.001, 1.0);

    float nl = 0;
    vec3 diffPunc = vec3(0);
    vec3 specPunc = vec3(0);

    pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, normalize(wavef+up*max(dist, 32.0)/32.0*(1.0-vdu)), v, normalize(light_dir), nl, diffPunc, specPunc);

    // <SS:Nexii> Whichever body is actually up lights the water - see
    // ss_moonlit. With the sun down this used to fall to zero and take the
    // moon's reflection with it.
    vec3 ss_punctual_light = mix(srgb_to_linear(ss_moonlit), sunlit_linear, ss_sun_up);
    vec3 punctual = clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10)) * ss_punctual_light * shadow * atten;
    radiance *= df2.y;
    //radiance = toneMapNoExposure(radiance);
    vec3 color = vec3(0);
    color = mix(fb.rgb, radiance, min(1, df2.x)) + punctual.rgb;

    float water_haze_scale = 4;

    if (classic_mode > 0)
        water_haze_scale = 1;

    // This looks super janky, but we do this to restore water haze in the distance.
    // These values were finagled in to try and bring back some of the distant brightening on legacy water.  Also works reasonably well on PBR skies such as PBR midday.
    // color = mix(color, additive * water_haze_scale, (1 - atten));

    // We shorten the fade here at the shoreline so it doesn't appear too soft from a distance.
    fade *= 60;
    fade = min(1, fade);
    color = mix(fb.rgb, color, fade);

    float spec = min(max(max(punctual.r, punctual.g), punctual.b), 0);

    frag_color = min(vec4(1),max(vec4(color.rgb, spec * water_mask), vec4(0)));
}

