/**
 * @file ssLightningF.glsl
 * @brief Atmo Magic lightning - channel ribbons, plasma, sparks, aura and fire discs.
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

// <SS:Nexii> Atmo Magic lightning

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_texcoord0;
in vec2 vary_texcoord1;
in vec4 vary_color;
in vec3 vary_aux;
in vec4 vary_ctl;

// An electric-line texture tiled along the ribbon (u across the width, v down the length). ss_use_tex 0 falls back to the procedural core below, so the bolt draws with no asset configured.
uniform sampler2D diffuseMap;
uniform float ss_use_tex;

// A COPY of the scene depth (SSVolCloud takes it, the lightning pass shares it) under the reserved name the binder needs - see the depthMap note in ssVolCloudF.glsl. ss_soft_on gates every read
// so the flash pass, which runs before any copy exists, never samples an unbound unit. ss_clip is the projection's near and far (the CONSTANT far plane, not the draw distance).
uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform vec2 ss_clip;
uniform float ss_soft_on;

// The bloom dial, the shared clock (wrapped on the CPU so the float keeps sub-millisecond steps), the live bolt's beading and the plasma's warp amplitude.
uniform float ss_glow;
uniform float ss_time;
uniform float ss_bead;
uniform float ss_warp;

// <SS:Nexii> The plasma's colour walk over its life, from the recorded frames: the column in the air goes white to white-cyan to grey (never green - the green stays in the foot), the amber foot goes
// white-yellow through yellow-green to a yellow knot that outshines the wisps, and the hot flare centre is over-exposed orange-white. doc/atmo_magic_lightning_strike.md
const vec3 RAMP_AIR0 = vec3(1.00, 1.00, 1.00);
const vec3 RAMP_AIR1 = vec3(0.85, 0.98, 1.00);
const vec3 RAMP_AIR2 = vec3(0.78, 0.82, 0.86);
const vec3 RAMP_AIR3 = vec3(0.60, 0.60, 0.58);
const vec3 RAMP_GND0 = vec3(1.00, 0.85, 0.50);
const vec3 RAMP_GND1 = vec3(0.85, 1.00, 0.55);
const vec3 RAMP_GND2 = vec3(0.98, 0.95, 0.45);
const vec3 RAMP_GND3 = vec3(0.70, 0.45, 0.20);
const vec3 KNOT_COLOR = vec3(1.00, 0.95, 0.45);
const vec3 HOT_COLOR = vec3(1.00, 0.93, 0.68);

// Eye-space distance from a depth-buffer reading. The projection is the ordinary one, so this is just its inverse.
float ss_eye_z(float d)
{
    float ndc = d * 2.0 - 1.0;
    return (2.0 * ss_clip.x * ss_clip.y)
         / (ss_clip.y + ss_clip.x - ndc * (ss_clip.y - ss_clip.x));
}

// Integer hash noise for the plasma: deterministic in ribbon space, so every client and every frame agrees on where the wisps are.
uint ss_uh(uvec2 p)
{
    p *= uvec2(1597334677u, 3812015801u);
    uint h = (p.x ^ p.y) * 1597334677u;
    h ^= h >> 16;
    return h;
}

float ss_h21(vec2 p)
{
    return float(ss_uh(uvec2(ivec2(floor(p)) + 0x7fff))) * (1.0 / 4294967296.0);
}

float ss_vnoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(ss_h21(i), ss_h21(i + vec2(1.0, 0.0)), f.x),
               mix(ss_h21(i + vec2(0.0, 1.0)), ss_h21(i + vec2(1.0, 1.0)), f.x), f.y);
}

vec3 ss_ramp4(vec3 c0, vec3 c1, vec3 c2, vec3 c3, float u)
{
    if (u < 0.35) return mix(c0, c1, u / 0.35);
    if (u < 0.65) return mix(c1, c2, (u - 0.35) / 0.30);
    return mix(c2, c3, clamp((u - 0.65) / 0.25, 0.0, 1.0));
}

void main()
{
    // The fragment mode rides tangent.w: 0 live core ribbon, 1 sheath ribbon, 2 aura / flare / fire disc (fraction = the flare share), 3 plain ribbon (sparks), 4 sky flash disc, 5 flat fill (wash, markers), 6 occlusion box.
    int mode = int(floor(vary_ctl.w));
    float bright = vary_ctl.x;
    vec3 col = vary_color.rgb;
    float a8 = vary_color.a;

    if (mode == 6)
    {
        // The occlusion query's box: every fragment must count, so nothing here may discard or read depth.
        frag_color = vec4(0.0);
        return;
    }

    if (mode == 5)
    {
        // A flat fill: the fullscreen amber wash (a8 0 - a veil is never a bloom seed) and the debug markers.
        frag_color = vec4(col * bright, a8);
        return;
    }

    if (mode == 4)
    {
        // The sky flash: a soft disc of light where the discharge is, what the air and cloud around a channel actually do. Cubed rather than linear so it reads as a glow with a centre
        // instead of a painted circle with an edge.
        vec2 d = vary_texcoord0 * 2.0 - 1.0;
        float r = clamp(1.0 - length(d), 0.0, 1.0);
        float soft = r * r * r;
        frag_color = vec4(col * bright * soft, ss_glow * a8 * soft);
        return;
    }

    if (mode == 2)
    {
        // Aura, flare and fire discs. A soft skirt hollowed toward the STRIKE POINT (texcoord1, in this disc's own uv units, scaled by the hollow radius in aux.z) rather than toward each disc's
        // own centre, so the five discs' sum is dim at the attachment instead of brightest there; the flare's share (the fraction of ctl.w) fills it back in with a power-law spike at the contact.
        // Three fades against the world: the per-vertex height above the surface (aux.y) dissolves the bottom edge instead of the depth test's hard chord; the anchor compare (aux.x, the true
        // strike point's view-axis depth) fades pixels where the scene sits well in front of the point - lenient, because at a grazing view the road under the disc is nearer than the point
        // without hiding it; and the soft-particle compare against the disc's own drawn depth softens whatever it still passes through.
        vec2 d = vary_texcoord0 * 2.0 - 1.0;
        float rr = length(d);
        if (rr >= 1.0) discard;
        float r = 1.0 - rr;

        float skirt = pow(r, 1.5);
        float hollow = vary_ctl.y;
        if (hollow > 0.0 && vary_aux.z > 0.0)
        {
            float rh = length(d - vary_texcoord1) / vary_aux.z;
            skirt *= 1.0 - hollow * (1.0 - smoothstep(0.0, 1.0, rh));
        }

        float q = fract(vary_ctl.w) * 2.0;
        float spike = (q > 0.0) ? 1.6 * q * pow(r, 6.0) : 0.0;

        float bf = smoothstep(-0.10, 0.45, vary_aux.y);

        float occ = 1.0;
        if (ss_soft_on > 0.5 && vary_ctl.z > 0.0)
        {
            float depth = texture(depthMap, gl_FragCoord.xy / screen_res).r;
            if (depth < 1.0)
            {
                float scene_z = ss_eye_z(depth);
                float anchor = vary_aux.x;
                occ = smoothstep(anchor * 0.45, anchor * 0.7, scene_z);
                float frag_z = ss_eye_z(gl_FragCoord.z);
                occ *= clamp((scene_z - frag_z) / vary_ctl.z, 0.0, 1.0);
            }
        }

        float I = (skirt + spike) * bright * bf * occ;
        float sp = clamp(spike, 0.0, 1.0);
        vec3 rgb = mix(col, HOT_COLOR, sp) * I;
        if (max(max(rgb.r, rgb.g), rgb.b) < 2.0 / 255.0) discard;
        frag_color = vec4(min(rgb, vec3(3.0)), ss_glow * (a8 + 0.35 * sp) * min(I, 1.0));
        return;
    }

    // Ribbons. Across the strip: 0 at one edge, 1 at the other, core in the middle.
    float across = abs(vary_texcoord0.x * 2.0 - 1.0);
    float x = across;
    float along = vary_texcoord0.y;
    float u = vary_aux.z;
    float w = vary_aux.y;
    float s = vary_aux.x * 97.0;
    float pl = vary_ctl.y;
    float bead_mul = vary_texcoord1.x;

    float mask;
    if (mode == 3 || mode == 1 || (u <= 0.0 && ss_bead <= 0.0))
    {
        // The cheap path, first: sparks, the sheath always, and the intact core with beading off - one power, no hashes. The fourth power makes the middle read as a discharge rather than
        // a painted stripe (the falloff from a line source really is this steep); the sheath and the amber foot lie softer.
        float e = (mode == 1) ? 3.0 : ((mode == 3) ? 4.0 : mix(4.0, 2.5, w));
        mask = pow(1.0 - x, e);
        if (ss_use_tex > 0.5 && mode == 0)
        {
            // The texture's own alpha is the channel shape; its red carries filament detail the author drew. Multiplied by the width falloff so a rectangular texture still ends softly at the
            // ribbon's edge.
            vec4 t = texture(diffuseMap, vary_texcoord0);
            mask = t.a * max(t.r, 0.35) * (1.0 - x * x);
        }
    }
    else
    {
        // The live core with beading, and the plasma the popped column turns into: the strip's own profile lumps along its length (a re-lit column beads where the old wisps had pinched),
        // then with age the filament is domain-warped into curling wisps, eroded by a rising threshold into stretches and knots, and grained at 30Hz. Sub-pixel ribbons (fwidth) skip the warp and grain.
        float aa = clamp(1.0 - fwidth(along) * 3.0, 0.0, 1.0);
        float b = ss_bead * bead_mul;
        float bead = ss_vnoise(vec2(along * 0.8 + s, 0.0));
        float wmul = mix(1.0 - b * 0.5 + b * bead, 0.35 + 1.4 * bead, u);
        x /= max(wmul, 0.05);

        float keep = 1.0;
        float spark = 0.0;
        if (u > 0.0 && pl * aa > 0.0)
        {
            float t = ss_time * 3.0 + u * 4.0;
            float n1 = ss_vnoise(vec2(x * 1.5, along * 0.35) + s + vec2(0.0, -t));
            float n2 = ss_vnoise(vec2(x * 4.0, along * 0.9) + s * 1.7 + vec2(t * 0.7, n1 * 3.0));
            x += ((n1 - 0.5) * 2.2 + (n2 - 0.5) * 0.9) * u * ss_warp * pl * aa;
            keep = smoothstep(u * 1.1 - 0.15, u * 1.1 + 0.15, n2 * 0.6 + n1 * 0.4);
            float g = ss_h21(vec2(x * 40.0, along * 40.0) + floor(ss_time * 30.0) * 0.37 + s);
            spark = smoothstep(0.55, 1.0, g) * u * pl * aa;
        }
        x = abs(x);
        float body = pow(max(0.0, 1.0 - x), mix(mix(4.0, 2.5, w), 1.5, u));
        if (ss_use_tex > 0.5)
        {
            vec4 t = texture(diffuseMap, vec2(clamp(0.5 + 0.5 * x, 0.0, 1.0), vary_texcoord0.y));
            body = t.a * max(t.r, 0.35) * (1.0 - x * x);
        }
        // The edge guard on the ORIGINAL across keeps a warped wisp from ending in the quad's straight cut.
        body *= smoothstep(1.0, 0.8, across);
        mask = body * ((u > 0.0) ? keep : 1.0) * (1.0 - 0.7 * u + 1.6 * spark) * (0.6 + 0.4 * mix(1.0, bead, b));

        if (u > 0.0)
        {
            vec3 air = ss_ramp4(RAMP_AIR0, RAMP_AIR1, RAMP_AIR2, RAMP_AIR3, u);
            vec3 gnd = ss_ramp4(RAMP_GND0, RAMP_GND1, RAMP_GND2, RAMP_GND3, u);
            col = mix(col, mix(air, gnd, w), smoothstep(0.0, 0.25, u));
            float k = pow(keep, 6.0) * smoothstep(0.3, 0.8, w) * u;
            col = mix(col, KNOT_COLOR, 0.5 * k);
            bright *= 1.0 + 3.0 * k;
        }
    }

    if (mask < 2.0 / 255.0) discard;

    // Drawn additively, so colour IS brightness and the alpha is the bloom seed (screen alpha is the glow mask in this pass): rgb may run above white for the amber foot and the knot, clamped
    // so the tonemapper is not blown out; the seed is the per-element bloom fraction times the profile, never inflated by the HDR brightness.
    frag_color = vec4(min(col * bright * mask, vec3(3.0)), ss_glow * a8 * mask * min(bright, 1.0));
}
