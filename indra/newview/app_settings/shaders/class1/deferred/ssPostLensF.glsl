/**
 * @file class1/deferred/ssPostLensF.glsl
 * @brief Atmo Magic: lens drops - a rain-on-glass post pass after depth of
 *        field and before anti-aliasing (the drops are ON the lens, so they
 *        are sharp and they blur what is behind them).
 *
 *        The Heartfelt construction (BigWings, shadertoy ltffzl), written
 *        from scratch in our idiom: a static-drop layer stuck to the glass,
 *        two moving-drop layers with trails sliding down it, and a
 *        condensation layer the trails wipe clear. No textures beyond the
 *        scene itself - every drop is procedural, hashed per grid cell.
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

// <SS:Nexii> Atmo Magic lens drops. Design: doc/atmo_magic_surface_weather.md section 12. Presentation only - no state
// depends on this pass, so its hashing and its stylised drop shapes are tuned by eye, not pinned by a twin test.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;
uniform vec2 screen_res;

// How wet the lens is, 0..1 (SSScreenFX::Lens::mWet) - scales the static drops' presence and the rim highlight.
uniform float ssLensWet;

// Condensation, 0..1, already multiplied by the SSAtmoLensCondensation dial - the fog layer's strength before the trails cut it clear.
uniform float ssLensFog;

// Wall-clock seconds, drives the moving drops' fall and wobble.
uniform float ssLensTime;

// Unit screen direction the drops slide in (gravity plus a share of the wind), from SSScreenFX::slideDir.
uniform vec2 ssLensSlide;

// screen_res.x / screen_res.y, so the drops stay round rather than stretching with the window.
uniform float ssLensAspect;

// SSAtmoLensDropsStrength as a size/intensity dial, 0..2.
uniform float ssLensScale;

// <SS:Nexii> Grid layout shared by the layers below: the static layer is dense and fine, the moving layers coarser and
// taller than wide (2:1 - a falling drop reads as a teardrop, not a dot) so their trails have somewhere to run.
const float SS_LENS_STATIC_COLS = 40.0;
const float SS_LENS_STATIC_ROWS = 60.0;
const float SS_LENS_MOVING_COLS = 14.0;
const float SS_LENS_MOVING_ROWS = 7.0;

// A float hash of a vec2 - presentation only, never state, so fract(sin(dot(...))) is fine here.
float ssLensHash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// The 2x2 rotation that carries the slide direction onto local -y: solved once so every layer below can just treat
// +y as "up the glass" and -y as "the way a drop falls", whatever the camera's roll or the wind's push. dir is unit
// length (slideDir's contract), so this is a pure rotation with no scale.
mat2 ssLensRot2(vec2 dir)
{
    return mat2(-dir.y, -dir.x, dir.x, -dir.y);
}

// <SS:Nexii> LAYER 1 - static drops: stuck to the glass, laid out on a fine 40x60 grid. Each cell hashes its own
// presence, offset within the cell and radius; presence is gated by how wet the lens is, so a dry lens shows none
// and a soaked one shows most of the grid. The normal is a stylised spherical-cap slope (zero at the drop's centre
// and at its rim, peaking between) - not a physically exact cap, but a cheap shape that reads as one.
vec3 ssLensStaticDrops(vec2 uv, float t)
{
    vec2 gridUV = uv * vec2(SS_LENS_STATIC_COLS, SS_LENS_STATIC_ROWS);
    vec2 cell = floor(gridUV);
    vec2 f = fract(gridUV);

    float presenceHash = ssLensHash(cell + vec2(7.0, 11.0));
    float presence = step(1.0 - clamp(ssLensWet, 0.0, 1.0), presenceHash);

    vec2 jitter = vec2(ssLensHash(cell + vec2(3.1, 0.0)), ssLensHash(cell + vec2(0.0, 9.7))) - vec2(0.5, 0.5);
    float size = 0.22 + 0.30 * ssLensHash(cell + vec2(1.3, 4.6));
    vec2 center = vec2(0.5, 0.5) + jitter * 0.3;

    vec2 delta = (f - center) / size;
    float r2 = dot(delta, delta);
    float edge = smoothstep(1.0, 0.8, r2);
    float coverage = presence * edge;

    vec2 normal = delta * (1.0 - r2) * coverage;
    return vec3(normal, coverage);
}

// <SS:Nexii> LAYERS 2-3 - moving drops with trails: a coarser 14x7 grid of 2:1-tall cells, scaled by layerScale so
// the two calling layers do not read as the same repeating pattern. Each cell's drop falls (local -y) at its own
// hashed speed and phase, with a small sine wobble across it; the trail is a narrow column of coverage left ABOVE
// the drop's current position (higher local y, where it has already fallen from), fading with distance travelled -
// this is the streak that clears the condensation behind a running drop.
vec3 ssLensMovingDrops(vec2 uv, float t, float layerScale)
{
    vec2 gridUV = uv * vec2(SS_LENS_MOVING_COLS, SS_LENS_MOVING_ROWS) * layerScale;
    vec2 cell = floor(gridUV);
    vec2 f = fract(gridUV);

    float speed = 0.4 + 0.6 * ssLensHash(cell + vec2(2.0, 6.0));
    float phase = ssLensHash(cell + vec2(5.0, 8.0));
    float fall = fract(t * speed * 0.15 + phase);
    float wobble = sin(t * 3.0 + phase * 6.28318530718) * 0.08;

    float dropY = 1.0 - fall;
    vec2 center = vec2(0.5 + wobble, dropY);
    float size = 0.16 + 0.10 * ssLensHash(cell + vec2(8.0, 1.0));

    vec2 delta = (f - center) / size;
    float r2 = dot(delta, delta);
    float dropCoverage = smoothstep(1.0, 0.75, r2);
    vec2 normal = delta * (1.0 - r2) * dropCoverage;

    float trailLocalX = (f.x - center.x) / size;
    float above = max(f.y - dropY, 0.0);
    float trailCoverage = exp(-trailLocalX * trailLocalX * 4.0) * exp(-above * 6.0) * step(dropY, f.y);
    normal += vec2(-trailLocalX, 0.0) * trailCoverage * 0.3;

    float coverage = max(dropCoverage, trailCoverage);
    return vec3(normal, coverage);
}

void main()
{
    vec2 uv = vary_fragcoord.xy;

    // <SS:Nexii> Aspect-corrected, slide-aligned local space: every layer below sees a frame where "down" is
    // always the direction the drops actually slide, so the grid stays round and the fall direction is right
    // whatever the camera's roll or the wind's push.
    vec2 aspectUV = vec2((uv.x - 0.5) * ssLensAspect, uv.y - 0.5);
    mat2 rot = ssLensRot2(ssLensSlide);
    vec2 localUV = rot * aspectUV + vec2(0.5, 0.5);

    vec3 layerStatic = ssLensStaticDrops(localUV, ssLensTime);
    vec3 layerMoveA = ssLensMovingDrops(localUV, ssLensTime, 1.0);
    vec3 layerMoveB = ssLensMovingDrops(localUV + vec2(0.37, 0.61), ssLensTime + 5.0, 1.85);

    vec2 dropNormal = layerStatic.xy + layerMoveA.xy + layerMoveB.xy;
    float dropCoverage = clamp(max(layerStatic.z, max(layerMoveA.z, layerMoveB.z)), 0.0, 1.0);

    // <SS:Nexii> Condensation the trails clear: only the moving layers' drop/trail coverage counts here, so a
    // static drop sitting on the glass does not itself wipe the fog, but a drop running down the window does.
    float fogMask = max(layerMoveA.z, layerMoveB.z);

    // <SS:Nexii> Refraction: the scene re-sampled through the combined drop normal, sharp - the drops are windows through the fog, not part of it.
    vec2 offset = dropNormal * 0.03 * ssLensScale;
    vec2 sampleUV = clamp(uv + offset, vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec3 refracted = texture(diffuseRect, sampleUV).rgb;

    // <SS:Nexii> Condensation on the glass itself: a soft 5-tap cross blur toward a grey-white tint, present where the fog demand is high and the trails have not just cleared it.
    vec2 texel = 1.0 / screen_res;
    vec3 blurred = texture(diffuseRect, uv).rgb
                 + texture(diffuseRect, uv + vec2(texel.x * 2.0, 0.0)).rgb
                 + texture(diffuseRect, uv - vec2(texel.x * 2.0, 0.0)).rgb
                 + texture(diffuseRect, uv + vec2(0.0, texel.y * 2.0)).rgb
                 + texture(diffuseRect, uv - vec2(0.0, texel.y * 2.0)).rgb;
    blurred *= 0.2;
    vec3 fogged = blurred * 0.85 + vec3(0.15, 0.15, 0.15);
    float fogAmount = clamp(ssLensFog, 0.0, 1.0) * (1.0 - clamp(fogMask, 0.0, 1.0));

    vec3 sceneWithFog = mix(refracted, fogged, fogAmount);
    vec3 rgb = mix(sceneWithFog, refracted, dropCoverage);

    // <SS:Nexii> Rim highlight: a glint against the sky, strongest where the combined normal is steep (a drop's rim, not its flat crown), gated by actual drop coverage rather than the always-true step() this replaces, scaled by how wet the lens is.
    float rim = 0.25 * clamp(ssLensWet, 0.0, 1.0) * pow(clamp(dot(dropNormal, dropNormal), 0.0, 1.0), 4.0) * dropCoverage;
    rgb += vec3(rim, rim, rim);

    frag_color = vec4(rgb, 1.0);
}
