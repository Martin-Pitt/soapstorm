/**
 * @file class1/deferred/skyF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, Linden Research, Inc.
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

// Inputs
in vec3 vary_HazeColor;
in float vary_LightNormPosDot;

#ifdef SS_ATMO
in float vary_ss_below_horizon;
uniform float ss_horizon_clip;
#endif

#ifdef HAS_HDRI
in vec4 vary_position;
in vec3 vary_rel_pos;
uniform float sky_hdr_scale;
uniform float hdri_split_screen;
uniform mat3 env_mat;
uniform sampler2D environmentMap;
#endif

uniform sampler2D rainbow_map;
uniform sampler2D halo_map;

uniform float moisture_level;
uniform float droplet_radius;
uniform float ice_level;

#ifdef SS_ATMO
in vec3 vary_ss_view_dir;

// <SS:Nexii> Weather-driven optics (ss_optics below), bound from the sky pool's active Atmo
// applier. ss_optic_light is the light direction in the dome's own frame (the same permuted
// frame lightnorm lives in, with +Y up) and the amplitudes are the weather's corona / crystal
// drives. ss_optic_gate is 0 unless an ACTIVE Atmo environment is pushing at least one of them,
// so an idle viewer keeps stock halo_map exactly.
uniform vec3  ss_optic_light;
uniform float ss_optic_gate;
uniform float ss_optic_active;
uniform float ss_optic_corona;
uniform float ss_optic_halo22;
uniform float ss_optic_halo46;
uniform float ss_optic_align;
#endif

out vec4 frag_data[4];

vec3 srgb_to_linear(vec3 c);
vec3 linear_to_srgb(vec3 c);

#define PI 3.14159265

/////////////////////////////////////////////////////////////////////////
// The fragment shader for the sky
/////////////////////////////////////////////////////////////////////////


vec3 rainbow(float d)
{
    // 'Interesting' values of d are -0.75 .. -0.825, i.e. when view vec nearly opposite of sun vec
    // Rainbox tex is mapped with REPEAT, so -.75 as tex coord is same as 0.25.  -0.825 -> 0.175. etc.
    // SL-13629
    // Unfortunately the texture is inverted, so we need to invert the y coord, but keep the 'interesting'
    // part within the same 0.175..0.250 range, i.e. d = (1 - d) - 1.575
    d         = clamp(-0.575 - d, 0.0, 1.0);

    // With the colors in the lower 1/4 of the texture, inverting the coords leaves most of it inaccessible.
    // So, we can stretch the texcoord above the colors (ie > 0.25) to fill the entire remaining coordinate
    // space. This improves gradation, reduces banding within the rainbow interior. (1-0.25) / (0.425/0.25) = 4.2857
    float interior_coord = max(0.0, d - 0.25) * 4.2857;
    d = clamp(d, 0.0, 0.25) + interior_coord;

    float rad = (droplet_radius - 5.0f) / 1024.0f;
    return pow(texture(rainbow_map, vec2(rad+0.5, d)).rgb, vec3(1.8)) * moisture_level;
}

vec3 halo22(float d)
{
    d       = clamp(d, 0.1, 1.0);
    float v = sqrt(clamp(1 - (d * d), 0, 1));
    return texture(halo_map, vec2(0, v)).rgb * ice_level;
}

#ifdef SS_ATMO
// Halo fringe colour: soft white, warmed toward the ring's inner edge and cooled on the outer -
// the classic red-inward / blue-outward halo tint, kept faint so halos read as clean light.
vec3 ss_optic_color(float rho, float ring, float width)
{
    float off = (rho - ring) / max(width, 1e-4);
    vec3 c = vec3(0.42, 0.45, 0.48);
    c += vec3(0.30, 0.10, -0.02) * (1.0 - smoothstep(-1.0, 0.2, off));
    c += vec3(-0.06, 0.02, 0.26) * smoothstep(-0.2, 1.0, off);
    return c;
}

// Weather-driven split optics. The view ray and light direction share the sky dome's frame (+Y up,
// light permuted like lightnorm), so each phenomenon lands at its true angular position: the corona
// hugs the light, the 22 deg and 46 deg halos are rings at those radii, sundogs sit where the
// 22 deg small circle crosses the light's altitude plane, the circumzenithal arc arches over the
// zenith at the light's zenith distance, and the supralateral arc crowns the 46 deg ring while the
// light rides low. The amplitudes and the elevation gates come from the weather uniforms - a given
// sky may simply not be asking for a phenomenon.
vec3 ss_optics(vec3 view)
{
    if (ss_optic_corona <= 0.001 && ss_optic_halo22 <= 0.001
        && ss_optic_halo46 <= 0.001 && ss_optic_align <= 0.001)
    {
        return vec3(0.0);
    }

    vec3 light = normalize(ss_optic_light);
    const vec3 up = vec3(0.0, 1.0, 0.0);

    float rho  = degrees(acos(clamp(dot(view, light), -1.0, 1.0)));
    float elev = degrees(asin(clamp(light.y, -1.0, 1.0)));

    // The light's own vertical: 0 points up the vertical circle of the light, 90 the horizontal.
    vec3 vertical = normalize(up - light * dot(up, light));
    if (dot(up, light) > 0.998)      // light overhead: no usable vertical circle
    {
        vertical = vec3(0.0, 0.0, 1.0);
    }
    float psid = degrees(acos(clamp(dot(view, vertical), -1.0, 1.0)));

    vec3 col = vec3(0.0);

    // Corona: a thin bluish-white aureole hugging the light plus two faint diffraction rings
    // beyond it (water drops). Kept small and dim - the aureole is half gone by ~0.4 deg and
    // dead by ~1 deg, so a corona reads as a rim around the disc, never a glow that doubles it.
    if (ss_optic_corona > 0.001)
    {
        const float aureole = exp(-pow(rho * 2.4, 2.0));
        const float ringA   = exp(-pow((rho - 2.4) / 1.0, 2.0));
        const float ringB   = exp(-pow((rho - 4.6) / 1.5, 2.0));

        vec3 ccol = vec3(0.42, 0.44, 0.48) * aureole
                  + vec3(0.12, 0.07, 0.04) * ringA
                  + vec3(0.04, 0.07, 0.12) * ringB;
        col += ccol * ss_optic_corona * 0.5;
    }

    // 22 deg halo: the everywhere veil of small platelets. Real halos are faint - a soft ring a
    // few times dimmer than the sun's own glare, so the scales below are kept low; the sundog is
    // the one member of the family that reads as a bright spot.
    if (ss_optic_halo22 > 0.001)
    {
        float w = 1.9;
        col += ss_optic_halo22 * exp(-pow((rho - 22.0) / w, 2.0))
             * ss_optic_color(rho, 22.0, w) * 0.18;
    }

    // 46 deg halo family (large plates/columns) - wider and fainter still.
    if (ss_optic_halo46 > 0.001)
    {
        float w = 2.3;
        col += ss_optic_halo46 * exp(-pow((rho - 46.0) / w, 2.0))
             * ss_optic_color(rho, 46.0, w) * 0.12;
    }

    // Aligned-plate phenomena: need the plates to settle, not just plenty of crystals.
    if (ss_optic_align > 0.001)
    {
        float alt_ok = smoothstep(2.0, 10.0, elev) * (1.0 - smoothstep(42.0, 58.0, elev));
        if (alt_ok > 0.001)              // sundogs: the 22 deg circle at the light's own altitude
        {
            float dog = exp(-pow((rho - 22.0) / 1.7, 2.0))
                      * exp(-pow((psid - 90.0) / 9.0, 2.0));
            col += ss_optic_align * alt_ok * dog
                 * ss_optic_color(rho, 22.0, 1.7) * 0.55;
        }

        float arc_ok = smoothstep(3.0, 9.0, elev) * (1.0 - smoothstep(30.0, 34.0, elev));
        if (arc_ok > 0.001)              // circumzenithal arc: arched over the zenith from the light's meridian
        {
            vec3 h_light = normalize(light - up * dot(light, up) + vec3(1e-5));
            vec3 h_view  = normalize(view  - up * dot(view, up)  + vec3(1e-5));
            float az = degrees(acos(clamp(dot(h_light, h_view), -1.0, 1.0)));
            float zd = degrees(acos(clamp(dot(view, up), -1.0, 1.0)));
            float cza = exp(-pow((zd - (90.0 - elev)) / 1.6, 2.0))
                      * exp(-pow(az / 20.0, 2.0));
            col += ss_optic_align * arc_ok * cza
                 * ss_optic_color(zd, 90.0 - elev, 1.6) * 0.4;
        }
    }

    // Supralateral arc: the 46 deg ring's tangent arc crowning the light, seen while the light is low.
    if (ss_optic_halo46 > 0.001)
    {
        float low = 1.0 - smoothstep(34.0, 52.0, elev);
        if (low > 0.001)
        {
            float w = 2.3;
            float lateral = exp(-pow((rho - 46.0) / w, 2.0))
                          * exp(-pow(psid / 34.0, 2.0)) * low;
            col += ss_optic_halo46 * lateral * ss_optic_color(rho, 46.0, w) * 0.16;
        }
    }

    return col * ss_optic_gate;
}
#endif

void main()
{
#ifdef SS_ATMO
    // <SS:Nexii> Horizon clip's depth write (SSAtmoEnvAtmosphere::mHorizonClip). The uniform is the
    // on/off gate; the depth itself is the shader const - one step nearer than the clouds (0.99998)
    // and the discs (0.99999), so both fail LEQUAL behind it below the horizon. Above it - and when
    // the gate is off - this writes 1.0, bit-identical to the cleared buffer; and the depth mask is
    // off unless the pool asks for the clip (drawDome), so the off path stores nothing at all.
    gl_FragDepth = (ss_horizon_clip > 0.0 && vary_ss_below_horizon < 0.0) ? LL_SHADER_CONST_HORIZON_DEPTH : 1.0;
#endif

    vec3 color;
#ifdef HAS_HDRI
    vec3 frag_coord = vary_position.xyz/vary_position.w;
    if (-frag_coord.x > ((1.0-hdri_split_screen)*2.0-1.0))
    {
        vec3 pos = normalize(vary_rel_pos);
        pos = env_mat * pos;
        vec2 texCoord = vec2(atan(pos.z, pos.x) + PI, acos(pos.y)) / vec2(2.0 * PI, PI);
        color = textureLod(environmentMap, texCoord.xy, 0).rgb * sky_hdr_scale;
        color = min(color, vec3(8192*8192*16)); // stupidly large value arrived at by binary search -- avoids framebuffer corruption from some HDRIs

        frag_data[2] = vec4(0.0,0.0,0.0,GBUFFER_FLAG_HAS_HDRI);
    }
    else
#endif
    {
        // Potential Fill-rate optimization.  Add cloud calculation
        // back in and output alpha of 0 (so that alpha culling kills
        // the fragment) if the sky wouldn't show up because the clouds
        // are fully opaque.

        color = vary_HazeColor;

        float  rel_pos_lightnorm = vary_LightNormPosDot;
        float optic_d = rel_pos_lightnorm;
#ifdef SS_ATMO
        // <SS:Nexii> The horizon clip cuts the lower dome at eye level - and the sun with it, since
        // under the clip's grace band the disc keeps drawing until it is ENTIRELY below the horizon.
        // No optical effect may trace an arc below that cut: without this the rainbow and the halo
        // strip carry on down off the anti-solar point and the weather optics continue under the
        // horizon, painting the half the clip exists to deny. Each fragment reads its own side of
        // the cut via vary_ss_below_horizon; with the clip off nothing is removed, so the phenomena
        // keep their natural continuation across the line as before. </SS:Nexii>
        const bool ss_clipped_below = (ss_horizon_clip > 0.0) && (vary_ss_below_horizon < 0.0);
        if (!ss_clipped_below)
        {
            color.rgb += rainbow(optic_d);
        }
        if (ss_optic_active > 0.001)
        {
            // <SS:Nexii> Weather-driven optics take over while an active Atmo environment drives the
            // sky (ss_optic_gate): the corona, the 22/46 deg halos, sundogs and the aligned-plate arcs
            // each render at their true angular positions instead of one merged texture strip. When
            // the halo step is switched off the strip is skipped too - an active Atmo sky never draws
            // the old corona-plus-wide-ring halo_map. An IDLE Atmo viewer (boosted master switch, no
            // environment) keeps the stock strip below, so it stays bit-for-bit. </SS:Nexii>
            if (ss_optic_gate > 0.001 && !ss_clipped_below)
            {
                color.rgb += ss_optics(normalize(vary_ss_view_dir));
            }
        }
        else if (!ss_clipped_below)
        {
            color.rgb += halo22(optic_d);
        }
#else
        color.rgb += rainbow(optic_d);
        color.rgb += halo22(optic_d);
#endif
        color.rgb *= 2.;
        color.rgb = clamp(color.rgb, vec3(0), vec3(5));

        frag_data[2] = vec4(0.0,0.0,0.0,GBUFFER_FLAG_SKIP_ATMOS);
    }

    frag_data[1] = vec4(0);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(color.rgb, 1.0);
#else
    frag_data[0] = vec4(color.rgb, 1.0);
#endif
}

