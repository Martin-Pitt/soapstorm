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

// <SS:Nexii> The ice-crystal optics wear the sky's own SUNLIGHT colour - the stock engine
// uniform the dome already shades with (LLShaderMgr::SUNLIGHT_COLOR, bound for the same
// program), so a red sunrise's halos, arcs and mock suns are red too. No new uniform: the
// vertex stage of this very program reads it already. </SS:Nexii>
uniform vec3 sunlight_color;

// <SS:Nexii> The sun slot disc's half-angle as a direction-z sine (SSAtmoEnvApplier::
// sunSlotRadius) - the same value skyV holds its airmass with. The corona scales its every
// angle by this relative to the stock quad's 0.05, so it stays a rim around WHATEVER disc is
// drawn rather than the fixed-angle aureole tuned for the old 10x quad.
uniform float ss_sun_radius;

// <SS:Nexii> The physical rainbow's gate (SSAtmoRainbow, lldrawpoolwlsky.cpp): 1 lets
// ss_rainbow below take the stock strip's place, 0 keeps the stock single bow bit for bit.
uniform float ss_rainbow_gate;

// <SS:Nexii> The rainbow grades itself by its light, both already uploaded to this program:
// lightnorm is the active light's direction in the dome's own frame (+Y up, so lightnorm.y is
// its elevation sine) and sun_up_factor says whether that light is the sun. A sun within a few
// degrees of the horizon has the short wavelengths scattered out of its long light path - the
// monochrome red rainbow - and moonlight is too dim for the cones - the white moonbow.
uniform vec3 lightnorm;
uniform int  sun_up_factor;
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

#ifdef SS_ATMO
// <SS:Nexii> The physical rainbow, painted from the same strip texture the stock lookup reads.
// Stock sweeps one texcoord across the whole strip and adds whatever is there at one strength -
// the bow band and the bright interior stretch together - which is why the interior arrives as
// the blinding wash below the arc. The three phenomena the strip (and physics) carry are read
// separately here, each at its own strength:
//
//   * the PRIMARY bow, the colour band at 34.4..41.4 deg off the antisolar point (red outer,
//     violet inner), at ~30% - a real bow is a faint thing against the storm light behind it;
//   * the INTERIOR brightening - light re-scattered inside the bow - on the stock stretch above
//     the band, at ~8%: real photos show it, never as a wash;
//   * the SECONDARY bow at ~49..55 deg, the same colour band read REVERSED (red inner, violet
//     outer) at ~a third of the primary. Every rainbow is a double: the second bow rides two
//     internal reflections instead of one and arrives faint. The gap between the bows is
//     Alexander's band and renders as exactly nothing - darker than either bow in every photo.
//
// The light's own state then grades the result: a sun within a few degrees of the horizon has
// blue and green scattered away before the drops ever see it (the monochrome red rainbow), and
// a moonbow is too dim for the cones - the full spectrum is present but the eye reads white,
// at a fraction of the strength. The gate (ss_rainbow_gate) leaves the stock strip above
// untouched when the debug setting is down.
vec3 ss_rainbow(float d)
{
    if (moisture_level <= 0.0)
    {
        return vec3(0.0);
    }

    // Stock's remap, unclamped: t sweeps 0.175..0.25 across the band (red outer, violet
    // inner), 0.25..0.425 across the interior stretch, and 0..0.081 across the secondary's
    // angular window.
    float t   = -0.575 - d;
    float rad = (droplet_radius - 5.0f) / 1024.0f;

    // Primary bow: the band clamp, faded at the red edge where the strip hands over to black.
    // The (1 - w) share keeps it off the interior's half of the stretch.
    float w    = smoothstep(0.25, 0.258, t);
    float m_in = smoothstep(0.172, 0.178, t);
    vec3 bow = pow(texture(rainbow_map, vec2(rad + 0.5, clamp(t, 0.175, 0.25))).rgb, vec3(1.8));

    // Interior: the stock stretched lookup, dead until the violet edge has passed.
    vec3 interior = pow(texture(rainbow_map, vec2(rad + 0.5, 0.25 + max(t - 0.25, 0.0) * 4.2857)).rgb, vec3(1.8));

    // Secondary bow: the colour band reversed - s 1 is the red inner edge (t 0.081, 49 deg off
    // the antisolar point), s 0 the violet outer one (t 0.0, ~55 deg) - windowed so neither
    // edge smears into Alexander's band.
    float s     = clamp(t / 0.081, 0.0, 1.0);
    float m_sec = smoothstep(0.0, 0.005, t) * (1.0 - smoothstep(0.076, 0.081, t));
    vec3 secondary = pow(texture(rainbow_map, vec2(rad + 0.5, 0.25 - s * 0.075)).rgb, vec3(1.8));

    vec3 col = bow * (m_in * (1.0 - w)) * 0.30
             + interior * w * 0.08
             + secondary * m_sec * 0.09;

    col *= moisture_level;

    // The light's grade. lightnorm.y is the active light's elevation sine either way;
    // sun_up_factor says whether that light is the sun.
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    if (sun_up_factor == 0)
    {
        // Moonbow: needs a near-full moon to see at all, and the cones stand down - the
        // spectrum is present but reads white, at a quarter strength.
        col = mix(vec3(luma), col, 0.2) * 0.25;
    }
    else
    {
        // Red rainbow: the lower the sun, the longer the light path and the more of the short
        // wavelengths is scattered away before the drops ever see it. Full monochrome inside
        // two degrees of the horizon, gone by seven.
        float mono = 1.0 - smoothstep(1.0, 7.0, degrees(asin(clamp(lightnorm.y, -1.0, 1.0))));
        col = mix(col, vec3(luma) * vec3(0.85, 0.22, 0.07), mono);
    }
    return col;
}
#endif

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

    // <SS:Nexii> The horizon-clip state as a mask: 1 everywhere the dome may draw - clip off,
    // or this fragment above the horizon - and 0 under the clip. Every phenomenon except the
    // sundogs multiplies by (1.0 - ss_below), so halos and arcs never paint below the horizon.
    // The dogs alone reach a little UNDER it (their own block re-carves the margin), because
    // the top of the light is still visible when its centre is just below the horizon. </SS:Nexii>
    float ss_below = (ss_horizon_clip > 0.001 && vary_ss_below_horizon < 0.0) ? 1.0 : 0.0;

    // <SS:Nexii> The 22 deg ring falls away as the light sinks toward the horizon: by ~14 deg of
    // light elevation the ring has faded, leaving only the mock-sun sundogs flanking the light -
    // the look of a low winter sun. </SS:Nexii>
    float ring_elev = smoothstep(14.0, 20.0, elev);

    vec3 col = vec3(0.0);

    // Corona: a thin bluish-white aureole hugging the light plus two faint diffraction rings
    // beyond it (water drops). Kept small and dim - the aureole is half gone by ~0.4 deg and
    // dead by ~1 deg, so a corona reads as a rim around the disc, never a glow that doubles it.
    // Every angle below runs on rho scaled by the drawn disc relative to the stock quad
    // (ss_disc), which is what kept it that rim when the discs shrank 10x off the old quad:
    // the ring radii ride in the disc's own half-angle the way they originally rode in the
    // stock quad's. The crystal halos further down stay at TRUE angles - droplet and ice
    // optics, not disc-relative ones.
    if (ss_optic_corona > 0.001)
    {
        // No drawn disc (active environment, no emitter) keeps the fixed-angle corona rather
        // than letting the 1e-5 floor blow the scale up into a full-sky wash.
        float ss_disc       = (ss_sun_radius > 1e-6) ? ss_sun_radius / 0.05 : 1.0;
        float cor_rho       = rho * ss_disc;
        const float aureole = exp(-pow(cor_rho * 2.4, 2.0));
        const float ringA   = exp(-pow((cor_rho - 2.4) / 1.0, 2.0));
        const float ringB   = exp(-pow((cor_rho - 4.6) / 1.5, 2.0));

        vec3 ccol = vec3(0.42, 0.44, 0.48) * aureole
                  + vec3(0.12, 0.07, 0.04) * ringA
                  + vec3(0.04, 0.07, 0.12) * ringB;
        col += ccol * ss_optic_corona * 0.5 * (1.0 - ss_below);
    }

    // 22 deg halo: the everywhere veil of small platelets. Real halos are faint - a soft ring a
    // few times dimmer than the sun's own glare, so the scales below are kept low; the sundog is
    // the one member of the family that reads as a bright spot.
    if (ss_optic_halo22 > 0.001)
    {
        float w = 1.9;
        // <SS:Nexii> The veil rides a deliberately stingy cold drive (SSAtmoEnvSkyWeatherModulator's
        // ice block), so its shader scale stays well below stock's 0.18 - the drive is what keeps
        // the halo an event, not the multiplier. The ring wears the light's own sunlight colour
        // with just a trace of the red-inward/blue-outward fringe, fades out as the light sinks
        // below ~20 deg (ring_elev), and never paints below the horizon. </SS:Nexii>
        col += ss_optic_halo22 * exp(-pow((rho - 22.0) / w, 2.0))
             * (sunlight_color.rgb * 0.8 + ss_optic_color(rho, 22.0, w) * 0.2)
             * 0.048 * (1.0 - ss_below) * ring_elev;
    }

    // 46 deg halo family (large plates/columns) - wider and fainter still.
    if (ss_optic_halo46 > 0.001)
    {
        float w = 2.3;
        col += ss_optic_halo46 * exp(-pow((rho - 46.0) / w, 2.0))
             * (sunlight_color.rgb * 0.8 + ss_optic_color(rho, 46.0, w) * 0.2)
             * 0.048 * (1.0 - ss_below);
    }

    // Aligned-plate phenomena: need the plates to settle, not just plenty of crystals.
    if (ss_optic_align > 0.001)
    {
        // <SS:Nexii> Sundogs - the mock suns. The old spot was a bare brightening of the ring,
        // which read as "a slightly stronger halo on the sides". A real sundog is a SECOND SUN:
        // a bright disc with the sun's own glow character, seated on the 22 deg parhelic circle
        // at the light's altitude, strongest when the light rides LOW. The dog wears the light's
        // OWN sunlight colour - it is a sun, not a white spot - and its disc is compact (a small
        // tweak below the sun's own angular scale, so it reads as a defined mock sun rather than
        // a soft blur). The soft inner-circle mask slices it on the halo's inner edge, so each
        // dog reads as a half-sun cut by the ring; and because the top of the light is still
        // visible when its centre is just below the horizon, the dogs' elevation gate reaches a
        // little UNDER it - but only in the dome's reachable band: main() gates ss_optics on
        // the clip, so the under-horizon part only ever counts where the clip is off. </SS:Nexii>
        {
            float dog_elev = smoothstep(-1.0, 1.0, elev)
                           * (1.0 - smoothstep(45.0, 58.0, elev));
            if (dog_elev > 0.001)
            {
                float core = exp(-pow((rho - 22.0) / 0.9, 2.0))
                           * exp(-pow((psid - 90.0) / 3.2, 2.0));
                float glow = exp(-pow((rho - 23.0) / 2.8, 2.0))
                           * exp(-pow((psid - 90.0) / 6.0, 2.0));
                float inner = smoothstep(18.5, 21.0, rho);
                float dog = (core + 0.4 * glow) * inner * dog_elev * 0.6;
                vec3 dog_col = mix(sunlight_color.rgb,
                                   ss_optic_color(rho, 22.0, 3.0), 0.15);
                col += ss_optic_align * dog_col * dog;
            }
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
                 * (sunlight_color.rgb * 0.8 + ss_optic_color(zd, 90.0 - elev, 1.6) * 0.2)
                 * 0.4 * (1.0 - ss_below);
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
            col += ss_optic_halo46 * lateral
             * (sunlight_color.rgb * 0.8 + ss_optic_color(rho, 46.0, w) * 0.2)
             * 0.16 * (1.0 - ss_below);
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
            // <SS:Nexii> The physical rainbow (ss_rainbow above) takes the stock strip's place
            // while the SSAtmoRainbow gate is up; gate down keeps the stock single bow bit for
            // bit. </SS:Nexii>
            color.rgb += (ss_rainbow_gate > 0.0) ? ss_rainbow(optic_d) : rainbow(optic_d);
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

