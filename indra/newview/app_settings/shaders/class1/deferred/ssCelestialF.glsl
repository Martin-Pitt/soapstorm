/**
 * @file ssCelestialF.glsl
 * @brief Atmo Magic: celestial discs - sphere phase shading, eclipse
 *        dimming, and emissive bodies. See ssCelestialV.glsl.
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

// <SS:Nexii> Atmo Magic celestial discs

out vec4 frag_data[4];

uniform sampler2D diffuseMap;

// Per-body state. Everything that varies from one disc to the next, and
// nothing that does not - the look constants are hardcoded below.
// Tint. Usually white: see the note in lldrawpoolwlsky.cpp on why the sky's
// own interpolated body colour is NOT what arrives here.
uniform vec4 ss_disc_color;
uniform vec3 ss_body_dir;        // unit, observer -> body
uniform vec3 ss_sun_dir;         // unit, body -> its star
uniform vec3 ss_quad_right;      // quad's +u axis, world space
uniform vec3 ss_quad_up;         // quad's +v axis, world space
uniform float ss_sunlight;       // 0 eclipsed, 1 in open sunlight
uniform float ss_emissive;       // 1 the body makes its own light
uniform float ss_phase_shaded;   // 1 shade it as a sphere

// The sky's own haze colour. These discs carry GBUFFER_FLAG_SKIP_ATMOS -
// the deferred atmospheric pass skips them entirely, the way the stock sun
// and moon do - so everything the atmosphere would have done to them has to
// happen here, from this one colour and the body's own elevation.
uniform vec3 ss_airlight;

in vec2 vary_texcoord0;

// How much brighter than its art an emissive disc is drawn. Enough to clear
// the haze glow around it and reach the bloom threshold: a disc that merely
// matched its own glow still reads as a hole in the sky, which is what a
// plain textured sun looked like against EEP's scattering.
const float SS_EMISSIVE_GAIN = 4.0;

// How lit the unlit side is left. Not zero - a new moon is not a hole in
// the sky, it is a disc lit by the light its own planet throws back at it.
const float SS_EARTHSHINE = 0.06;

// How much brighter a reflecting body is drawn than its own art.
//
// Modest, because lunar art is a PHOTOGRAPH of a correctly exposed full
// moon - mid-grey maria, near-white highlands - not a measurement of its
// 0.12 albedo. It already is the moon as an eye sees it, so it needs
// lifting only enough to sit clearly above a twilight sky rather than
// rebuilding from rock.
const float SS_LUNAR_GAIN = 1.5;

// How much a reflecting body's extinction is treated as hue rather than
// dimming, 0 fully physical and 1 hue-only.
//
// A dark-adapted eye does not see a horizon moon as three magnitudes down;
// it sees a big warm disc. Taking extinction literally - which is right for
// a daylight object, and is what the sun's own path deliberately avoids -
// left the moon dimmer than the sky it was sitting in front of. This keeps
// most of its brightness while still shifting it warm and letting the haze
// wash over it.
const float SS_LUNAR_ADAPT = 0.6;

// Terminator softness, in cosine either side of the boundary. A hard N.L cut
// on a disc a few dozen pixels across reads as a bite taken out of it; real
// ones are softened by the star's angular size anyway.
const float SS_TERMINATOR_SOFT = 0.15;

// Aerial perspective, the standard form:
//
//     seen = own * T + airlight * (1 - T)
//
// What the atmosphere takes out of a body's light it PUTS BACK as its own
// glow. That is the whole reason a rising moon is pale salmon and washed
// out rather than dark red: extinction has removed most of its own light,
// and what fills the disc instead is the dusk sky in front of it.
//
// Modelling those two halves separately - dim the disc, then add a little
// haze on top - is what made a horizon moon come out DARKER than the sky
// behind it, which is the one thing it never is.

// Per-channel extinction through one airmass. Blue scatters out hardest,
// which is why a low body is orange and a high one keeps its own colour.
const vec3 SS_EXTINCTION = vec3(0.06, 0.13, 0.28);

// How much air a body's light comes through, as a multiple of straight up.
// The real relation runs away at the horizon; this is the standard 1/sin
// with a floor, which tops out around 12 airmasses - enough to redden a
// setting sun hard without driving it to black.
float ss_airmass(float sin_alt)
{
    return 1.0 / max(sin_alt, 0.085);
}

void main()
{
    vec4 c = texture(diffuseMap, vary_texcoord0.xy);

    // The stock moon art carries transparent pixels at <0x55,0x55,0x55,0x00>;
    // dropping them rather than blending keeps the quad's corners from
    // hazing over whatever is behind them.
    if (c.a <= 2.0 / 255.0)
    {
        discard;
    }

    c.rgb *= ss_disc_color.rgb;

    // How much atmosphere this body is being seen through. Its own
    // elevation is the whole of it - a body overhead is one airmass, one on
    // the horizon is a dozen.
    float airmass = ss_airmass(max(ss_body_dir.z, 0.0));
    vec3 transmittance = exp(-SS_EXTINCTION * airmass);

    if (ss_emissive > 0.5)
    {
        // A star is the light: no terminator, no eclipse, and nothing the
        // atmosphere does to it either.
        //
        // Not because that is physically true - a setting sun really is
        // reddened and dimmed - but because it is not what anyone SEES. The
        // disc stays far past what an eye can hold right down to the
        // horizon, so it reads as flat bright white throughout, and every
        // attempt here to model the physics (extinction, limb darkening, a
        // softened rim) made it look worse: a muddy orange disc at twenty
        // degrees where the real thing is still blinding.
        //
        // What actually changes at sunset is the SKY - the haze, the
        // horizon gradient, the light on the clouds - and that is where the
        // work belongs.
        c.rgb *= SS_EMISSIVE_GAIN;
    }
    else
    {
        // Shaded as the sphere the disc stands in for. The billboard already
        // carries the normal implicitly: the disc IS the projection of a
        // sphere, so the UV gives it.
        if (ss_phase_shaded > 0.5)
        {
            vec2 p = vary_texcoord0.xy * 2.0 - 1.0;
            float r2 = min(dot(p, p), 1.0);

            // At the centre of the disc the surface faces the observer,
            // which is -ss_body_dir; the rest is the quad's own two axes.
            vec3 n = normalize(ss_quad_right * p.x
                             + ss_quad_up * p.y
                             + (-ss_body_dir) * sqrt(1.0 - r2));

            float lit = smoothstep(-SS_TERMINATOR_SOFT, SS_TERMINATOR_SOFT,
                                   dot(n, ss_sun_dir));
            c.rgb *= mix(SS_EARTHSHINE, 1.0, lit);
        }

        // ...and dimmed by whatever its planet's shadow leaves of the light
        // reaching it at all.
        c.rgb *= ss_sunlight;

        // Lifted out of its own albedo - see SS_LUNAR_GAIN. Before the
        // atmosphere gets at it, so a low moon still washes toward the haze
        // rather than staying stubbornly bright.
        c.rgb *= SS_LUNAR_GAIN;
    }

    // The atmosphere - but the two kinds of body see it differently, and
    // the difference is saturation rather than physics.
    //
    // A setting SUN is still far too bright to look at: its light is
    // reddened on the way in like everything else, but the disc is so far
    // past what an eye or a sensor can hold that the core clips to white
    // regardless, and only the dimmer limb shows the colour. Photographs of
    // sunsets show exactly that - a white-hot disc with an orange rim, in a
    // sky that carries all the colour.
    //
    // So an emissive body keeps its full brightness and takes only the HUE
    // of the transmittance (normalised on its strongest channel), and gets
    // no airlight added: nothing the air glows with competes with a star.
    // A reflecting body takes the transmittance as it is and has the air's
    // glow fill in what was removed, which is what makes a rising moon pale
    // salmon rather than dark red.
    // Reflecting bodies only. What the air takes out of a moon's light it
    // puts back as its own glow, which is what makes a rising one pale
    // salmon rather than dark red - see the note above on why a star is
    // left alone.
    if (ss_emissive <= 0.5)
    {
        // Eye adaptation first - see SS_LUNAR_ADAPT. Normalising toward the
        // strongest channel keeps the warm cast while giving back most of
        // the brightness a literal extinction would have taken.
        float peak = max(max(transmittance.r, transmittance.g), transmittance.b);
        vec3 t = mix(transmittance, transmittance / max(peak, 1.0e-4), SS_LUNAR_ADAPT);

        c.rgb = c.rgb * t + ss_airlight * (1.0 - t);
    }

    frag_data[0] = vec4(0);
    frag_data[1] = vec4(0);
    frag_data[2] = vec4(0, 0, 0, GBUFFER_FLAG_SKIP_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[3] = c;
#else
    frag_data[0] = c;
#endif
}

// </SS:Nexii>
