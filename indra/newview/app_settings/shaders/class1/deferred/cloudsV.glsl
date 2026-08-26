/**
 * @file WLCloudsV.glsl
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

uniform mat4 modelview_projection_matrix;

in vec3 position;
in vec2 texcoord0;

//////////////////////////////////////////////////////////////////////////
// The vertex shader for creating the atmospheric sky
///////////////////////////////////////////////////////////////////////////////

// Output parameters
out vec3 vary_CloudColorSun;
#ifdef SS_ATMO
out float vary_CloudGlow;   // <SS:Nexii> the sky's forward-scatter glow, handed to the fragment stage separately so thinness can gate it
#endif
out vec3 vary_CloudColorAmbient;
out float vary_CloudDensity;

out vec2 vary_texcoord0;
out vec2 vary_texcoord1;
out vec2 vary_texcoord2;
out vec2 vary_texcoord3;
out float altitude_blend_factor;

// Inputs
uniform vec3 camPosLocal;

uniform vec3 lightnorm;
uniform vec3 sunlight_color;
uniform vec3 moonlight_color;
uniform int sun_up_factor;
uniform vec3 ambient_color;
uniform vec3 blue_horizon;
uniform vec3 blue_density;
uniform float haze_horizon;
uniform float haze_density;

uniform float cloud_shadow;
uniform float density_multiplier;
uniform float max_y;

uniform vec3 glow;
uniform float sun_moon_glow_factor;

uniform vec3 cloud_color;

uniform float cloud_scale;

#ifdef SS_ATMO
// <SS:Nexii> Region-relative cloud parallax and wind travel (doc/atmo_magic_cloud_parallax.md) - Atmo-only, so a stock environment compiles the pristine texcoord path.
uniform vec2 ss_cloud_drift;  // metres the layer has travelled on the wind, east and north
uniform vec2 region_offset;   // camera pos - region centre, metres, world X/Y
uniform float ss_cloud_alt_m; // the LAYER'S OWN altitude, metres - what a metre of camera travel is worth in uv
#endif
// </SS:Nexii>

// NOTE: Keep these in sync!
//       indra\newview\app_settings\shaders\class1\deferred\skyV.glsl
//       indra\newview\app_settings\shaders\class1\deferred\cloudsV.glsl
//       indra\newview\app-settings\shaders\class2\windlight\cloudsV.glsl
//       indra\newview\lllegacyatmospherics.cpp
//       indra\newview\llsettingsvo.cpp
void main()
{
    // World / view / projection
    // <SS:Nexii> The cloud layer's own depth slot.
    //
    // LLGLSPipelineSkyBox's LLGLSquashToFarClip would otherwise put this on
    // 0.99999 along with the haze dome and everything else in the sky, which
    // leaves no room to order the sky's layers against each other - and the
    // celestial discs need to be ordered against this one, since a disc is
    // added to the sky rather than composited over it and so can only be
    // hidden by depth.
    //
    // Its layer parameter cannot express this: it steps in units of 0.0001
    // (0.99999 - 0.0001 * layer, see setProjectionMatrix), and what is
    // wanted here is one step of 0.00001.
    //
    // Set in the same FORM the disc shader uses - w times a constant, in a
    // vertex shader - and that matters as much as the value. Reaching the
    // same number by two different routes (a multiply here, a rewritten
    // projection row there) leaves the two disagreeing in the last bits, so
    // the depth test flips per pixel and the layers speckle through each
    // other. Same expression, same result, no fight.
    vec4 cloud_pos = modelview_projection_matrix * vec4(position.xyz, 1.0);
    cloud_pos.z = cloud_pos.w * 0.99998;
    gl_Position = cloud_pos;
    // </SS:Nexii>

    // Texture coords
    // SL-13084 EEP added support for custom cloud textures -- flip them horizontally to match the preview of Clouds > Cloud Scroll
    vary_texcoord0 = vec2(-texcoord0.x, texcoord0.y);  // See: LLSettingsVOSky::applySpecial

    vary_texcoord0.xy -= 0.5;
    vary_texcoord0.xy /= cloud_scale;
    vary_texcoord0.xy += 0.5;

#ifdef SS_ATMO
    // <SS:Nexii> Region-relative cloud parallax, scaled by the LAYER'S OWN altitude rather than the max-altitude proxy it first shipped with - max_y is an atmosphere ceiling, not a cloud height,
    // and borrowing it coupled the parallax to a dial authored for haze. ss_cloud_alt_m is the dome's authored height (Clouds > Sky Dome), or, on that tab's Auto setting, the altitude the
    // volumetric deck implies - cirrus-high in dry still air, merging down onto the deck as its coverage builds, so dome band and deck agree about where the cloud IS as they merge at the rim.
    float metres_per_uv = 16.0 * ss_cloud_alt_m * cloud_scale;
    vary_texcoord0.xy += vec2(region_offset.x, -region_offset.y) / metres_per_uv;

    // ...and the layer's own travel on the wind, in the same terms - the sky actually moving over the world, which the cloud scroll rate is NOT (that slides the large texture against the small
    // one and the pattern boils in place). Before the other three texcoords are derived, so the deck moves as one thing. Signs are the parallax term's, negated: clouds moving one way looks like
    // the camera moving the other.
    vary_texcoord0.xy += vec2(-ss_cloud_drift.x, ss_cloud_drift.y) / metres_per_uv;
    // </SS:Nexii>
#endif

    vary_texcoord1 = vary_texcoord0;
    vary_texcoord1.x += lightnorm.x * 0.0125;
    vary_texcoord1.y += lightnorm.z * 0.0125;

    vary_texcoord2 = vary_texcoord0 * 16.;
    vary_texcoord3 = vary_texcoord1 * 16.;

    // Get relative position
    vec3 rel_pos = position.xyz - camPosLocal.xyz + vec3(0, 50, 0);

#ifdef SS_ATMO
    // <SS:Nexii> The horizon fade decoupled from max altitude: dividing by max_y let the ATMOSPHERE ceiling thin every low-sky cloud - at an authored 1000m ceiling a cloud at the horizon sat at
    // half alpha before anything else touched it, which is exactly the "sun disc through solid clouds" leak, and no amount of disc-side machinery could out-engineer an alpha the author never
    // chose. A short fixed ramp keeps the horizon soft; the below-horizon droop cut below is untouched.
    // (Eased back a touch from the first cut of (y+100)/200, which held clouds fully solid to ~1 degree and read as too hard a wall at the waterline.)
    altitude_blend_factor = clamp((rel_pos.y + 90.0) / 300.0, 0.0, 1.0);

    // ...except INTO the sun: the eased fade reads beautifully against sky but lets the disc burn through the same half-faded clouds, and the two aesthetics only collide inside the disc's
    // angular neighbourhood - so exactly there, and nowhere else, horizon clouds keep their body. The ramp spans roughly the width of a large authored sun disc.
    float sun_prox = smoothstep(0.965, 0.992, dot(normalize(rel_pos), lightnorm.xyz));
    altitude_blend_factor = max(altitude_blend_factor, sun_prox);
#else
    altitude_blend_factor = clamp((rel_pos.y + 512.0) / max_y, 0.0, 1.0);
#endif

    // Set altitude
    if (rel_pos.y > 0)
    {
        rel_pos *= (max_y / rel_pos.y);
    }
    if (rel_pos.y < 0)
    {
        altitude_blend_factor = 0; // SL-11589 Fix clouds drooping below horizon
        rel_pos *= (-32000. / rel_pos.y);
    }

    // Can normalize then
    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    // Initialize temp variables
    vec3 sunlight = sunlight_color;
    vec3 light_atten;

    // Sunlight attenuation effect (hue and brightness) due to atmosphere
    // this is used later for sunlight modulation at various altitudes
    light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

    // Calculate relative weights
    vec3 combined_haze = abs(blue_density) + vec3(abs(haze_density));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = haze_density / combined_haze;

    // Compute sunlight from rel_pos & lightnorm (for long rays like sky)
    float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + lightnorm.y);
    sunlight *= exp(-light_atten * off_axis);

    // Distance
    float density_dist = rel_pos_len * density_multiplier;

    // Transparency (-> combined_haze)
    // ATI Bugfix -- can't store combined_haze*density_dist in a variable because the ati
    // compiler gets confused.
    combined_haze = exp(-combined_haze * density_dist);

    // Compute haze glow
    float haze_glow = 1.0 - dot(rel_pos_norm, lightnorm.xyz);
    // haze_glow is 0 at the sun and increases away from sun
    haze_glow = max(haze_glow, .001);
        // Set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
    haze_glow *= glow.x;
        // Higher glow.x gives dimmer glow (because next step is 1 / "angle")
    haze_glow = pow(haze_glow, glow.z);
        // glow.z should be negative, so we're doing a sort of (1 / "angle") function

    haze_glow *= sun_moon_glow_factor;

    // Add "minimum anti-solar illumination"
    // For sun, add to glow.  For moon, remove glow entirely. SL-13768
    haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (haze_glow + 0.25);

    // Increase ambient when there are more clouds
    vec3 tmpAmbient = ambient_color;
    tmpAmbient += (1. - tmpAmbient) * cloud_shadow * 0.5;

    // Dim sunlight by cloud shadow percentage
    sunlight *= (1. - cloud_shadow);

    // Haze color below cloud
    vec3 additiveColorBelowCloud =
        (blue_horizon * blue_weight * (sunlight + tmpAmbient) + (haze_horizon * haze_weight) * (sunlight * haze_glow + tmpAmbient));

    // CLOUDS
    sunlight = sunlight_color;
    off_axis = 1.0 / max(1e-6, lightnorm.y * 2.);
    sunlight *= exp(-light_atten * off_axis);

    // Cloud color out
#ifdef SS_ATMO
    // <SS:Nexii> The glow SPLIT OUT of the cloud body colour instead of multiplied into it: haze_glow is the sky's forward-scatter airlight, which lives BEHIND a cloud - baked into
    // vary_CloudColorSun it recoloured every cloud in a wide cone around the sun toward the glow, when a backlit cloud is a dark silhouette whose thin fringes alone transmit the fire. The
    // fragment shader gates it by per-fragment thinness; the body keeps glow-free sunlight. </SS:Nexii>
    vary_CloudColorSun = sunlight * cloud_color;
    vary_CloudGlow     = haze_glow;
#else
    vary_CloudColorSun     = (sunlight * haze_glow) * cloud_color;
#endif
    vary_CloudColorAmbient = tmpAmbient * cloud_color;

    // Attenuate cloud color by atmosphere
#ifdef SS_ATMO
    // <SS:Nexii> Full-strength optical depth, no sqrt: the stock halving left horizon clouds crisp, which never showed while the max_y alpha fade was thinning them into the sky anyway - with
    // that fade decoupled, the honest colour convergence has to carry the melt alone. This is the density/distance dials doing on the dome band exactly what they do on the volumetric deck: cloud
    // extinguishes and takes on the airlight over the same slab path the sky itself is hazed by, so at the horizon the band dissolves into the atmosphere instead of silhouetting against it.
#else
    combined_haze = sqrt(combined_haze);  // less atmos opacity (more transparency) below clouds
#endif
    vary_CloudColorSun *= combined_haze;
    vary_CloudColorAmbient *= combined_haze;
    vec3 oHazeColorBelowCloud = additiveColorBelowCloud * (1. - combined_haze);

    // Make a nice cloud density based on the cloud_shadow value that was passed in.
    vary_CloudDensity = 2. * (cloud_shadow - 0.25);

    // Combine these to minimize register use
    vary_CloudColorAmbient += oHazeColorBelowCloud;

    // needs this to compile on mac
    //vary_AtmosAttenuation = vec3(0.0,0.0,0.0);

    // END CLOUDS
}
