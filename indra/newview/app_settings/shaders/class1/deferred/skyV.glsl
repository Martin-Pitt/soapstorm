/**
 * @file WLSkyV.glsl
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

// SKY ////////////////////////////////////////////////////////////////////////
// The vertex shader for creating the atmospheric sky
///////////////////////////////////////////////////////////////////////////////

// Output parameters
out vec3 vary_HazeColor;
out float vary_LightNormPosDot;

#ifdef SS_ATMO
out float vary_ss_below_horizon;
#endif

#ifdef HAS_HDRI
out vec4 vary_position;
out vec3 vary_rel_pos;
#endif

// Inputs
uniform vec3 camPosLocal;

uniform vec3  lightnorm;
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform int   sun_up_factor;
uniform vec3  ambient_color;
uniform vec3  blue_horizon;
uniform vec3  blue_density;
uniform float haze_horizon;
uniform float haze_density;

#ifdef SS_ATMO
// <SS:Nexii> Below-horizon ray treatment: 1 mirrors the ray (Atmo look), 0 leaves
// the stock -32000 collapse. Zero unless an ACTIVE Atmo environment is driving the
// sky (lldrawpoolwlsky.cpp), so an enabled-but-idle viewer's EEP sky stays stock.
uniform float ss_horizon_mirror;

// <SS:Nexii> How much of the sun's disc has cleared the horizon (SSAtmoEnvApplier::sunRiseFraction):
// 0 fully set - or no active Atmo environment, which is exactly stock - up to 1 fully risen,
// ramping across the disc's own angular span. The dome's light and its sun glow are ramped on
// this below, because stock switches BOTH the moment the disc's centre crosses zero, which reads
// as the whole sunrise horizon snapping on at once instead of growing with the disc.
uniform float ss_sun_rise;

// <SS:Nexii> The sun's TRUE direction while any part of the disc is in sight
// (SSAtmoEnvApplier::sunSlotDirection). The shared lightnorm direction switches from the sun to
// the moon the moment the disc's CENTRE sets (LLSettingsSky::getLightDirection) - stock never
// saw it because stock zeroes the glow and culls the disc below the horizon, but the ramps keep
// both alive through the rise band, and the takeover rule is sun > moon until the sun is
// completely out of sight: the glow and the extinction below keep aiming at the SUN while
// ss_sun_rise is positive, and the moon takes the lightnorm back only once it is not.
uniform vec3 ss_sun_dir;

// <SS:Nexii> The disc's half-angle as a direction-z sine (SSAtmoEnvApplier::sunSlotRadius) - the
// same value the risen fraction is measured against. The extinction below holds the sun's airmass
// at the JUST-CLEARED path (off_axis 1/radius at the horizon line) while any part of the disc is
// above the horizon, because elevation 0 is not the horizon-sitting airmass - it is the infinite
// one, and 1/max(1e-6, rel_pos.y + 0) collapses every horizon ray to unlit. The floor releases
// continuously as the centre climbs past it and is exact at full rise: the risen fraction hits 1
// precisely when the centre clears the radius. Zero while no Atmo environment drives the sky.
uniform float ss_sun_radius;

// <SS:Nexii> The stock ray lift. The atmosphere ray below is computed 50 m above the geometry it
// belongs to (the + vec3(0, 50, 0) in rel_pos), a legacy fudge that once rode along with the stock
// sun disc's own legacy 50 m drop (sunDiscV.glsl) - glow hotspot and drawn disc wrong together,
// which passed for agreement. The Atmo discs draw at the TRUE direction (ssCelestialV.glsl carries
// no offset of any kind), so while they own the sky the lift drops to 0 and the glow hotspot lands
// on the disc: 50 m of lift at the ~5000 m dome is 0.57 degrees, about one sun-diameter of visible
// droop. 1 is exactly stock - 50 * 1.0 is the same ray bit for bit - which is what an
// enabled-but-idle viewer keeps.
uniform float ss_ray_lift;
#endif

uniform float cloud_shadow;
uniform float density_multiplier;
uniform float distance_multiplier;
uniform float max_y;

uniform vec3  glow;
uniform float sun_moon_glow_factor;

uniform int cube_snapshot;

// NOTE: Keep these in sync!
//       indra\newview\app_settings\shaders\class1\deferred\skyV.glsl
//       indra\newview\app_settings\shaders\class1\deferred\cloudsV.glsl
//       indra\newview\lllegacyatmospherics.cpp
void main()
{
    // World / view / projection
    vec4 pos = modelview_projection_matrix * vec4(position.xyz, 1.0);

    gl_Position = pos;

#ifdef SS_ATMO
    // <SS:Nexii> Which half of the dome this vertex belongs to, for the horizon clip
    // (SSAtmoEnvAtmosphere::mHorizonClip - see skyF.glsl and lldrawpoolwlsky.cpp).
    vary_ss_below_horizon = position.y - camPosLocal.y;
#endif

    // Get relative position
#ifdef SS_ATMO
    // <SS:Nexii> The lift rides ss_ray_lift (see the uniform note above): 1 keeps the
    // stock ray bit for bit, 0 aims the glow - and the rainbow dot below - at the
    // true direction the Atmo discs draw at.
    vec3 rel_pos = position.xyz - camPosLocal.xyz + vec3(0, 50.0 * ss_ray_lift, 0);
#else
    vec3 rel_pos = position.xyz - camPosLocal.xyz + vec3(0, 50, 0);
#endif

#ifdef HAS_HDRI
    vary_rel_pos = rel_pos;
    vary_position = pos;
#endif

    // <SS:Nexii> Below the horizon, mirror the ray instead of collapsing it.
    //
    // Stock stretches a below-horizon ray to 32000 units, and separately the
    // sunlight term below clamps its elevation with max(0., rel_pos_norm.y).
    // Together those make the lower dome black: full extinction over an
    // enormous path, lit by ambient alone.
    //
    // That is invisible as long as land or water covers the lower dome, and
    // it does not. A flat sea of radius R seen from height h ends atan(h/R)
    // below eye level, so there is always a wedge between the water's edge
    // and the true horizon - about two degrees at twenty metres up - with
    // nothing in it but lower dome. Hence the black band under a sunset,
    // widening as you climb, papered over only when cloud coverage is high
    // enough to fill it.
    //
    // Mirroring shades a ray a degree below the horizon exactly like one a
    // degree above, so the haze simply carries on down. Continuous at the
    // horizon, and truer than black: what is actually down there is the
    // same air, seen along the same sort of path.
    //
    // Runtime-gated by ss_horizon_mirror (see the uniform note above): with
    // the mirror off, the stock behaviour below is restored exactly - the
    // -32000 branch stays compiled because the SS_ATMO variant serves both
    // the idle (stock collapse) and active (mirror) cases per frame, and
    // under the mirror it is simply unreachable: the abs keeps y >= 0.
    // </SS:Nexii>
#ifdef SS_ATMO
    if (ss_horizon_mirror > 0.)
    {
        rel_pos.y = abs(rel_pos.y);
    }
#endif
    if (rel_pos.y > 0.)
    {
        rel_pos *= (max_y / rel_pos.y);
    }
    if (rel_pos.y < 0.)
    {
        rel_pos *= (-32000. / rel_pos.y);
    }

    // Normalized
    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    // Grab this value and pass to frag shader for rainbows
    float rel_pos_lightnorm_dot = dot(rel_pos_norm, lightnorm.xyz);
    vary_LightNormPosDot = rel_pos_lightnorm_dot;

    // Initialize temp variables
    vec3 sunlight = (sun_up_factor == 1) ? sunlight_color : moonlight_color * 0.7; //magic 0.7 to match legacy color

#ifdef SS_ATMO
    // <SS:Nexii> The disc sheds light as it rises, not the instant its centre clears the horizon:
    // the dome's sunlight walks from the night value to the day value across the disc's own rise
    // (ss_sun_rise is the risen share of the disc), so the haze it lights up grows in with it.
    // Zero leaves the stock switch untouched - night, idle environments, and the fully-set case.
    if (ss_sun_rise > 0.0)
    {
        sunlight = mix(moonlight_color * 0.7, sunlight_color, ss_sun_rise);
    }
#endif

    // Sunlight attenuation effect (hue and brightness) due to atmosphere
    // this is used later for sunlight modulation at various altitudes
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

#ifdef SS_ATMO
    // <SS:Nexii> While any part of the disc is above the horizon, the sun's term in the light
    // path floors at the DISC'S OWN half-angle (ss_sun_radius). Stock feeds the raw (clamped)
    // lightnorm elevation in here, so the moment the disc's CENTRE dips under, every ray near
    // the horizon loses its light path - the max(1e-6, ...) below collapses them to unlit - and
    // the whole sunset band is cut out from under a disc that is still half up. Holding the
    // elevation at the radius is the JUST-CLEARED airmass (1/radius at the horizon line, the
    // same path a sun that has only just cleared gets), so the band stays lit for the whole
    // rise and the sunrise horizon exists from the first sliver; held at 0 the horizon rays get
    // the infinite-airmass clamp and die exactly as they do in stock. The floor releases
    // continuously once the centre clears the radius - the risen fraction hits 1 exactly there -
    // and the gate is stock with it off.
    float sun_elev = (ss_sun_rise > 0.0) ? max(ss_sun_dir.z, ss_sun_radius) : lightnorm.y;
#else
    float sun_elev = lightnorm.y;
#endif

    // Calculate relative weights
    vec3 combined_haze = max(abs(blue_density) + vec3(abs(haze_density)), vec3(1e-6));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = haze_density / combined_haze;

    // Compute sunlight from rel_pos & lightnorm (for long rays like sky)
    float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + sun_elev);
    sunlight *= exp(-light_atten * off_axis);

    // Distance
    float density_dist = rel_pos_len * density_multiplier;

    // Transparency (-> combined_haze)
    // ATI Bugfix -- can't store combined_haze*density_dist in a variable because the ati
    // compiler gets confused.
    combined_haze = exp(-combined_haze * density_dist);

    // Compute haze glow
    // <SS:Nexii> The glow tracks the disc (ss_sun_dir), not the lightnorm: lightnorm hands the
    // direction to the moon at centre-set, which would swing the whole sunset band across the
    // sky to the moon's azimuth while the disc is still half up. See the ss_sun_dir note above.
    // Frame note: rel_pos and lightnorm live in the ogl frame lightnorm is uploaded in
    // (LLEnvironment::toLightNorm permutes world x,y,z to y,z,x), while ss_sun_dir arrives in
    // world axes - the same raw vector the celestial discs phase against - so the swizzle below
    // puts both directions in one frame. ss_sun_dir.z keeps meaning the true elevation.
#ifdef SS_ATMO
    vec3 glow_dir = (ss_sun_rise > 0.0) ? ss_sun_dir.yzx : lightnorm.xyz;
#else
    vec3 glow_dir = lightnorm.xyz;
#endif
    float haze_glow = 1.0 - dot(rel_pos_norm, glow_dir);
    // haze_glow is 0 at the sun and increases away from sun
    haze_glow = max(haze_glow, .001);
    // Set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
    haze_glow *= glow.x;
    // Higher glow.x gives dimmer glow (because next step is 1 / "angle")
    haze_glow = pow(haze_glow, glow.z);
    // glow.z should be negative, so we're doing a sort of (1 / "angle") function

    // Add "minimum anti-solar illumination"
    // For sun, add to glow.  For moon, remove glow entirely. SL-13768
#ifdef SS_ATMO
    // <SS:Nexii> The glow is the light the disc sheds, so during the rise band it is built from
    // the RAW angular term and scaled by the risen share - the hotspot exists from the first
    // sliver above the horizon. Stock's factor line cannot be allowed to touch it there: below
    // centre-rise the factor belongs to the moon (< 1.0), whose branch zeroes the term entirely
    // (SL-13768 - right for the moon, which must not glow), and ramping on the zeroed term grew
    // a FLAT 0.25 wash with no hotspot at all until the factor snapped to 1.0 at centre-rise -
    // the sunrise horizon simply was not there while the disc poked over. At full rise this is
    // exactly the stock sun line (ss_sun_rise 1 * (raw + 0.25)); with the gate off, stock.
    if (ss_sun_rise > 0.0)
    {
        haze_glow = ss_sun_rise * (haze_glow + 0.25);
    }
    else
#endif
    {
        haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (sun_moon_glow_factor * (haze_glow + 0.25));
    }

    // Haze color above cloud
    vec3 color = (blue_horizon * blue_weight * (sunlight + ambient_color)
               + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient_color));

    // Final atmosphere additive
    color *= (1. - combined_haze);

    // Increase ambient when there are more clouds
    vec3 ambient = ambient_color + max(vec3(0), (1. - ambient_color)) * cloud_shadow * 0.5;

    // Dim sunlight by cloud shadow percentage
    sunlight *= max(0.0, (1. - cloud_shadow));

    // Haze color below cloud
    vec3 add_below_cloud = (blue_horizon * blue_weight * (sunlight + ambient)
                         + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient));

    // Attenuate cloud color by atmosphere
    combined_haze = sqrt(combined_haze);  // less atmos opacity (more transparency) below clouds

    // At horizon, blend high altitude sky color towards the darker color below the clouds
    color += (add_below_cloud - color) * (1. - sqrt(combined_haze));

    // Haze color above cloud
    vary_HazeColor = color;
}
