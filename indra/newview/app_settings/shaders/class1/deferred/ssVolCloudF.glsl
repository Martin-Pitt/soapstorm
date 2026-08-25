/**
 * @file ssVolCloudF.glsl
 * @brief Atmo Magic volumetric cloud field - camera-facing puffs.
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

// <SS:Nexii> Atmo Magic volumetric cloud field

out vec4 frag_color;

uniform sampler2D diffuseMap;

in vec2 vary_texcoord0;
in vec4 vary_color;

// A COPY of the scene depth - see mDepthCopy.
//
// Named depthMap because that is one of LLShaderMgr's RESERVED uniform
// names, and only reserved names can be bound as textures. bindTexture takes
// an index into the shader's reserved-uniform table, while looking a custom
// name up returns a raw GL location - so binding "sceneDepth", as this was
// called, indexed that table with a number that meant nothing and left the
// sampler pointed at whatever was on texture unit 0. Which is the cloud
// noise map, read as depth.
// The sky dome's cloud map, as a second and third octave - see mDomeTexRef.
// Reserved name, because only reserved names can be bound as textures.
uniform sampler2D cloud_noise_texture;

uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform vec2 ss_clip;           // near and far plane, for linearising depth
uniform float ss_soft_m;        // metres of fade; 0 disables it

uniform vec3 ss_light_dir;  // toward whatever is lighting the sky

// Strikes lighting the deck from inside, as point lights: xyz agent-space
// position, w brightness. Up to four, because that is as many as can be
// glowing at once before they stop being distinguishable anyway.
//
// Point lights rather than a per-puff colour the CPU worked out, because a
// discharge inside a cloud is a LOCAL source and the puffs are spheres: the
// face turned toward it lights and the far face does not. A flat brightness
// added per puff cannot express that - and worse, it then gets shaped by
// the sphere term belonging to the SUN, so a strike under a night deck was
// dimmed by SS_FORM_DARK on exactly the underside it should have lit.
#define SS_MAX_STRIKES 4
uniform vec4 ss_strike[SS_MAX_STRIKES];
uniform int ss_strike_count;

// What colour the discharge lights the deck. The sheath colour rather than
// the core's: what reaches a puff has been through cloud, and the core is
// the part that does not get out.
uniform vec3 ss_strike_color;

// How far a discharge reaches through the deck, in metres. Not an inverse
// square: cloud scatters, so the light spreads much further and much more
// softly than it would through clear air.
#define SS_STRIKE_REACH 700.0
uniform vec3 ss_sun_color;  // its colour
uniform vec3 ss_haze;       // the sky's own scattered light
uniform vec3 ss_cam_pos;

uniform float ss_base_z;        // world height of the layer's underside
uniform float ss_layer_thick;   // and how deep it is
uniform float ss_anvil;         // 0 rounded tops, 1 flat against the inversion
uniform float ss_tex_mix;       // authored bias toward the detail map
uniform float ss_puff_density;  // ceiling on one puff's opacity
uniform float ss_detail_scale;  // multiplies the fine octaves' size
uniform float ss_drift_rate;    // multiplies how fast they boil
uniform vec2 ss_wind;       // unit, the direction the air is travelling

uniform vec2 ss_drift;      // metres the air has travelled, east and north
uniform float ss_time;      // seconds, for the boil
uniform float ss_churn;     // 0 still air, 1 violently convective

in vec3 vary_world;

// How much of the puff is solid core before the noise starts eating into
// it, as a fraction of its radius.
const float SS_PUFF_CORE = 0.15;

// Where the rim starts closing, as a fraction of the radius. Inside the
// window's own falloff, so the noise still ragged-edges the puff well
// before this takes over.
const float SS_PUFF_RIM = 0.75;

// How hard the noise swings the density either side of the window.
//
// Raised, because the window is a circle and the noise is not: the more of
// the silhouette the noise decides, the less the field looks like a pile of
// spheres. At low contrast the round window wins everywhere and every puff
// reads as the ball it is.
const float SS_PUFF_CONTRAST = 3.2;

// Over how many metres the underside of the layer is cut flat.
//
// A cumulus deck has a flat bottom - the condensation level is a height, the
// same height everywhere, and cloud simply does not exist below it. Rounded
// puffs cannot produce that on their own; left alone they hang their lower
// halves below the base and the deck reads as a heap of balls from
// underneath, which is the giveaway.
//
// Cutting in WORLD space rather than per puff is what makes it a deck: every
// puff is sliced by the same plane at the same height, so the cut lines up
// across all of them into one flat surface.
//
// Fading over a long stretch rather than a short one. The plane is what
// makes the base flat; the LENGTH of the fade is what stops it looking
// stamped. Over a few tens of metres the deck gains a clean edge and reads
// as sheet metal - it is the slow ramp that gives the underside the depth of
// something you could fly up into. There is a limit: push it past the layer
// thickness and the cut stops being a base at all, just a general dimming of
// everything low in the field.
const float SS_BASE_SOFT_M = 120.0;

// And the same for the top, once the weather is anvilling.
//
// A cumulonimbus stops dead at the tropopause: it has run out of air less
// dense than itself, so there is nothing to rise into and the tower spreads
// sideways instead. That ceiling is as flat as the base is, and for the
// mirror-image reason - both are a HEIGHT the air cannot cross, so the cut
// belongs in world space where every puff meets it at the same altitude.
//
// Sharper than the base fade: a cloud base is softened by wisps hanging
// under it, while an anvil top is sheared off by the winds up there.
const float SS_TOP_SOFT_M = 70.0;

// How far height through the layer swings the texture mix, and how far a
// slow wander across the field does.
//
// Height, because a cloud is not the same stuff top to bottom: the base is
// flat and dense where it has just condensed, the top ragged where it is
// coming apart. Position, because a sky is not uniform either - one part of
// it can be doing something different from the rest, and a mix that varies
// only with height would band the whole field into horizontal stripes.
const float SS_MIX_HEIGHT = 0.55;
const float SS_MIX_WANDER = 0.45;

// How much faster the top of the layer boils than the base.
//
// Convection is a vertical motion, and it is not evenly distributed: the
// base of a cumulus sits at the condensation level and stays put, while the
// top is where the rising air actually arrives and piles up. So the same
// churn buys far more movement up there. Driving the whole layer at one
// rate makes it slide as a slab, which reads as a texture animating rather
// than as air moving.
const float SS_BOIL_TOP = 3.0;

// How far out of step the top of the layer runs from the base, in radians.
// Fixed, so the layers never drift further apart than this however long the
// viewer has been open - see the note where it is used.
const float SS_BOIL_LEAD = 2.0;

// How much the noise shades the puff internally. Small: the colour is the
// CPU's per-puff sun/ambient mix, and multiplying that by a mid-grey noise
// map - as this used to - simply halves the brightness of the whole field.
const float SS_PUFF_SHADE = 0.35;

// How dark the side of a puff facing away from the light is left.
//
// Not very, because cloud is not opaque - light that enters one side comes
// out of the other, which is why a cloud has soft shading rather than a
// terminator. But not one flat value either, which is what a per-quad
// colour gives and why the field read as grey card after grey card with no
// form to any of it.
const float SS_FORM_DARK = 0.55;

// How much the thin parts glow.
//
// The bright fringe on a cloud is the sun coming THROUGH it where it is
// thin enough to pass - so the rim lights up while the body stays dull, and
// that fringe is most of what gives a cloud its silhouette. Keyed to low
// density, so it lands exactly on the ragged edges the noise cuts.
const float SS_RIM = 0.8;

// Over how many metres a puff washes out into the sky's own haze.
//
// Without this the field ignores the atmosphere entirely: the dome behind it
// is hazed with distance while the puffs stay crisp at any range, so a puff
// crossing the dome cuts a hard shape out of it instead of blending into it.
const float SS_HAZE_M = 2600.0;

// How far the noise is stretched along the wind when the air is perfectly
// still, easing back to round as convection rises.
//
// Stable air does not make lumps, it makes LAYERS. With nothing lifting it,
// cloud spreads out along the shear instead of piling up, and stratus comes
// out drawn into long streaks running downwind - which is why a calm
// overcast reads as a sheet and a convective sky reads as heaps. Sampling
// the noise round at every convection made the calm end look like weak
// cumulus rather than like stratus.
// Halved from where it started. At 4x the noise ran so far downwind that
// the sky came out as rails rather than as layers - and it was compounding
// with two other elongations nothing was accounting for: the quads are
// already drawn 1.7 wide by 0.62 tall (PUFF_WIDE), and the stretch is
// applied on all three planes, so no orientation broke the direction up.
// Stacked, a 4x stretch on the map became far more than 4x on screen.
const float SS_STREAK = 2.0;

// The finer octaves, as fractions of the base tile, and how much of the
// density each contributes.
//
// One octave can only ever describe lumps of one size. The base map gives
// the body of a cloud; these give it the curdled surface that a body of
// vapour has, and - because they SCROLL rather than sitting still - the
// sense that it is turning over rather than posing.
// Roughly doubled from where they started. At a third and a tenth of the
// base tile the octaves were resolving detail finer than a puff can carry -
// so the surface came out as grain rather than as structure, and the Detail
// Scale dial had to be wound up before it looked like cloud at all. A
// default that needs correcting is the wrong default.
const float SS_OCT2_SCALE = 0.70;
const float SS_OCT3_SCALE = 0.28;
const float SS_OCT2_W = 0.30;
const float SS_OCT3_W = 0.15;

// How far the detail travels along the flow in one cycle, in METRES.
//
// Metres, not tiles of its own octave, and that is not a detail. Expressed
// in tiles the world distance came out as tiles x octave size - so Detail
// Scale, which exists to change how BIG the detail is, was also changing how
// far it moved, and a dial meant for one thing quietly drove two. Turning it
// up swept the flow across the sky; turning it down left it shimmering in
// place.
//
// Fixed in metres, the motion is the same however finely the map is
// sampled, and Detail Scale only does what it says.
const float SS_FLOW_M = 90.0;

// Ceiling on that travel once converted to tiles. Past about half a tile the
// two cross-faded copies are far enough apart to read as two textures rather
// than one moving - so a very fine octave, where 90m is many tiles, is held
// back to where the cross-fade still holds together.
const float SS_FLOW_MAX_TILES = 0.5;

// How much lift is added to the flow before it is normalised. At zero the
// equator of a puff flows dead sideways and the underside barely moves; a
// little of this tilts the whole field upward, which is the direction a
// convective cloud is actually going.
const float SS_FLOW_RISE = 0.45;

// How many laps of the boil cycle the detail makes per second at full
// convection, and the share of that it keeps in dead calm. Never quite
// nothing: even still air is not static.
const float SS_OCT_LAPS = 0.06;
const float SS_OCT_DRIFT_FLOOR = 0.25;

// How far the lookup is displaced by a coarse read of the map itself, in
// metres - domain warping.
//
// A tiling map sampled on a straight grid repeats visibly, and at 880m a
// tile the field is seven repeats wide in each direction: the same scrap of
// cloud over and over, in rows lined up with the world axes because that is
// what the planes are aligned to. No amount of octaves hides it, because
// every octave repeats on the same grid.
//
// Warping bends the grid before it is sampled. The lookup position is
// pushed around by a much coarser read of the same map, so the repeats stop
// falling on straight lines and stop landing at even spacings - the tile is
// still there, but there is no longer a pattern to notice. One extra sample
// per axis buys it.

// How far each plane is skewed along the axis it DROPS, per metre of that
// axis.
//
// A triplanar lookup on xy knows nothing about z, so every height over the
// same ground samples the same texel - which stacks into vertical columns
// through the layer, most obvious looking straight up. Same for the other
// two planes and their own missing axes. Sliding each plane's coordinates
// by the axis it cannot see decorrelates them: two points differing only in
// height now land in different parts of the map.
// Metres of the dropped axis per tile of skew. Larger is gentler.
const float SS_SKEW_M = 1400.0;

// Metres of world per tile of the noise map. This sets the size of the
// lumps the field breaks into - not the size of a puff, which is the
// field's own business.
//
// Four times what it started at, i.e. the map applied at a quarter scale.
// At 220m the noise was resolving detail finer than the puffs carrying it,
// so every puff showed a busy scrap of texture and the field read as fine
// grain rather than as bodies of cloud. Stretching it puts the structure
// back at the scale of the cloud rather than the scale of the map.
const float SS_NOISE_M = 880.0;

// Eye-space distance from a depth-buffer reading. The projection is the
// ordinary one, so this is just its inverse.
float ss_eye_z(float d)
{
    float ndc = d * 2.0 - 1.0;
    return (2.0 * ss_clip.x * ss_clip.y)
         / (ss_clip.y + ss_clip.x - ndc * (ss_clip.y - ss_clip.x));
}

float ss_density(vec2 uv)
{
    return dot(texture(diffuseMap, uv).rgb, vec3(0.3333));
}

float ss_detail(vec2 uv)
{
    return dot(texture(cloud_noise_texture, uv).rgb, vec3(0.3333));
}

// One detail sample, ADVECTED - the flow-map trick.
//
// Two copies of the same lookup half a cycle apart, each dragged along the
// flow by how far through its own cycle it is, cross-faded on a triangle so
// whichever copy is showing is always the one nearest the start of its
// travel. Neither copy is ever seen resetting, because at the moment one
// would snap back it has already faded to nothing.
//
// This is what sliding an offset could never do. Translation moves the whole
// pattern rigidly - the structure goes past, which reads as wind. What
// convection actually does is grow structure at one end of the motion and
// destroy it at the other, and the cross-fade is exactly that: detail wells
// up, travels, and dissolves.
float ss_flow(vec2 uv, vec2 flow, float ph0, float ph1, float w)
{
    return mix(ss_detail(uv - flow * ph0), ss_detail(uv - flow * ph1), w);
}

// One plane's density, with the two maps already blended - see the note at
// the lookup.
float ss_mixed(vec2 uv, float m)
{
    return mix(ss_density(uv), ss_detail(uv), m);
}

void main()
{
    // A soft radial window. The art has no edge of its own - it is seamless
    // noise, opaque corner to corner, with no alpha channel - so without a
    // window every puff draws as its quad, hard borders and all. That was
    // the wall of rectangles.
    vec2 p = vary_texcoord0.xy * 2.0 - 1.0;
    float r = length(p);
    float shape = 1.0 - smoothstep(SS_PUFF_CORE, 1.0, r);

    // ...and a hard stop at the rim, which the window above cannot provide
    // on its own.
    //
    // The window is ADDED to the noise below, so where it falls to zero the
    // noise alone can still carry a fragment - and it does, right out to the
    // corners of the quad. That is why the puffs were reading as rounded
    // rectangles rather than as cloud: the shape was suggesting an edge
    // while the noise kept drawing past it. This multiplies, so nothing
    // survives the boundary whatever the noise says.
    float rim = 1.0 - smoothstep(SS_PUFF_RIM, 1.0, r);

    // The noise sampled in the AIR's frame, not the quad's.
    //
    // This is the difference between a field of clouds and a field of
    // stickers. The map is world-space noise for a whole body of cloud; a
    // puff is one lump inside that body, so what belongs on a puff is the
    // part of the field it happens to occupy. Sampled per quad - the whole
    // tile on every one, as it was - every puff carries an identical copy
    // of the same picture, and no amount of jittering their positions hides
    // that. Sampled by position, neighbouring puffs continue each other and
    // the lumps that emerge belong to the field rather than to any quad.
    //
    // In the air's frame rather than the world's, so a cloud keeps its shape
    // as the deck drifts instead of dissolving and reforming while it
    // travels. The puffs are placed on cells in that same frame.
    vec3 air = vary_world - vec3(ss_drift, 0.0);

    // Sampled on all three planes, weighted by the quad's own facing.
    //
    // Two planes was not enough, and failed in a way worth recording: a
    // billboard turned side-on to one of them has almost no variation left
    // in that plane's first coordinate across the whole quad, so the lookup
    // collapses to a single line of the map stretched down the puff. That is
    // where the vertical streaking came from - not an alpha artefact, a
    // texture being read along one axis.
    //
    // The quad's frame, built from the camera rather than from screen-space
    // derivatives.
    //
    // Derivatives were the obvious way to get it - the quad is flat, so its
    // tangents are constant and their cross product is exact - and they are
    // a trap. A puff covering less than a 2x2 pixel quad, or one caught
    // edge-on, has derivatives that collapse to nothing; cross() of those is
    // a zero vector and normalize() of THAT is NaN. A NaN colour draws
    // black, and which puffs are small enough to hit it changes as the
    // camera moves, so they blink in and out. That is the scatter of little
    // black tiles - nothing to do with buffers or blending.
    //
    // These quads face the camera (or lie flat, near the zenith), so the
    // direction to the eye is the normal to within a few degrees in every
    // case that matters, and it can never degenerate. The distance falls out
    // of the same operation for the haze below.
    vec3 to_eye = ss_cam_pos - vary_world;
    float eye_dist = length(to_eye);
    vec3 nrm = (eye_dist > 1.0e-4) ? to_eye / eye_dist : vec3(0.0, 0.0, 1.0);

    vec3 tri = abs(nrm);

    // The sphere the quad stands in for, reconstructed once and used twice -
    // for the light below, and for which way the detail flows.
    //
    // Axes spanning the quad, taken from the world rather than the screen.
    // Any pair perpendicular to the normal will do: rotating the frame
    // within the quad's own plane turns the fake sphere about the view axis,
    // which a wrapped light term cannot tell apart.
    vec3 ref = (abs(nrm.z) < 0.95) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tan_u = normalize(cross(ref, nrm));
    vec3 tan_v = cross(nrm, tan_u);
    vec3 sphere_n = normalize(tan_u * p.x + tan_v * p.y + nrm * sqrt(max(1.0 - r * r, 0.0)));
    tri /= max(tri.x + tri.y + tri.z, 1.0e-4);

    // Stretched along the wind, by however little convection there is.
    float streak = mix(SS_STREAK, 1.0, clamp(ss_churn, 0.0, 1.0));

    // The horizontal lookup goes into the wind's own frame first - along and
    // across - so the stretch follows the weather rather than the world
    // axes. Dividing the ALONG coordinate by more metres is what makes the
    // noise change slowly in that direction, and slowly is what a streak is.
    vec2 across = vec2(-ss_wind.y, ss_wind.x);
    vec2 wind_uv = vec2(dot(air.xy, ss_wind) / streak, dot(air.xy, across));

    // No domain warp here any more.
    //
    // It was displacing the lookup by a coarse read of the map to break up
    // the tiling, which it did - and took the cloud with it. Warping bends
    // the sample grid, and the same bend that hides a repeat also drags real
    // structure sideways; at the scale the base map is now sampled at, there
    // was more structure being dragged than repeat being hidden, and the
    // field came out smeared. The tiling it was fighting is better dealt
    // with by sampling nearer the size of a puff, so there is something at
    // puff scale to look at instead.
    //
    // Kept in metres, not yet divided down: every octave needs all three of
    // these at its own scale, so the division happens at the point of use.
    vec2 pl_yz = vec2(air.y / streak, air.z);
    vec2 pl_xz = vec2(air.x / streak, air.z);
    vec2 pl_xy = wind_uv;

    // ...and these are added AFTER that division, in tiles, so they mean the
    // same thing to every octave. Each plane is skewed by the axis it drops
    // (see SS_SKEW_M), which is what stops the three of them agreeing about
    // where the tile boundaries fall.
    vec2 off_yz = vec2(air.x / SS_SKEW_M, 0.0);
    vec2 off_xz = vec2(-air.y / SS_SKEW_M, 0.0);
    vec2 off_xy = vec2(air.z / SS_SKEW_M, air.z / SS_SKEW_M * -0.7);

    // How far this fragment leans toward the other map, and how high it sits
    // in the layer. Both are wanted before the base octave is taken - the
    // mix decides what that octave is sampled FROM, and the height drives
    // the boil rate below.
    //
    // Three things decide the mix: what the author asked for, where in the
    // layer the fragment sits, and a slow wander over the field so the
    // change is regional rather than a stripe.
    //
    // The wander alone is horizontal, and that one is safe: it is sampled
    // three times coarser than the base octave, so it barely changes across
    // a single puff. What it must not do is change FAST on a plane with no
    // variation to give - which is why everything below is triplanar.
    float layer_h = clamp((vary_world.z - ss_base_z) / ss_layer_thick, 0.0, 1.0);
    float wander = ss_detail(air.xy / (SS_NOISE_M * 3.0));
    float tex_mix = clamp(ss_tex_mix
                        + (layer_h - 0.5) * SS_MIX_HEIGHT
                        + (wander - 0.5) * SS_MIX_WANDER, 0.0, 1.0);

    // The body of the cloud: triplanar, mixed per plane, and STATIC in the
    // air's frame.
    //
    // Static because the shape of a body of vapour is not what boils - the
    // surface of it is. This used to cross-fade between two offsets of the
    // whole field to fake that, which cost a second full set of samples to
    // animate the one part that should hold still while the deck drifts.
    // The scrolling octaves below do the churning now, and do it better.
    float noise = tri.x * ss_mixed(pl_yz / SS_NOISE_M + off_yz, tex_mix)
                + tri.y * ss_mixed(pl_xz / SS_NOISE_M + off_xz, tex_mix)
                + tri.z * ss_mixed(pl_xy / SS_NOISE_M + off_xy, tex_mix);


    // Two finer octaves from the DOME's own cloud map, sliding across each
    // other - see SS_OCT_LAPS and SS_FLOW_M.
    //
    // Triplanar, like the base. They were taken on the horizontal plane
    // alone, on the reasoning that surface detail is too fine to need
    // placing carefully - which was wrong in a way that showed. A horizontal
    // coordinate barely changes as you move UP a puff, so on any quad facing
    // the camera the octaves held nearly still down the whole height of it
    // and smeared into vertical streaks. Puffs overhead looked right for the
    // same reason: there the horizontal plane is the correct one.
    //
    // Height changes how FAR an octave wanders, not how fast it goes round.
    //
    // It used to scale the rate, and that reintroduced the same failure the
    // orbit was meant to end - one level down. Rate multiplied by height
    // means the PHASE differs across a puff by an amount proportional to
    // elapsed time, so however bounded each orbit is, the gap between the
    // bottom of a puff and the top keeps widening. Given a few minutes it
    // sweeps whole turns over a puff's height, nearby heights land on
    // completely different offsets, and because layer_h runs vertically the
    // tearing comes out as horizontal bands. Winding the rate up only got
    // there sooner.
    //
    // Scaling the amplitude keeps the intent - the top of a convective layer
    // moves more than its base - with nothing that grows. The rate is now
    // uniform across the fragment, so there is no gradient to accumulate,
    // and a fixed phase lead with height keeps the layers out of step
    // without ever drifting further apart.
    float boil = mix(1.0, SS_BOIL_TOP, layer_h);
    float turn = ss_time * SS_OCT_LAPS * ss_drift_rate
               * (SS_OCT_DRIFT_FLOOR + clamp(ss_churn, 0.0, 1.0)) * 6.2831853;
    float lead = layer_h * SS_BOIL_LEAD;

    float oct2_m = SS_NOISE_M * SS_OCT2_SCALE * ss_detail_scale;

    // Where in its cycle the flow is, and its half-cycle partner.
    float cycles = turn / 6.2831853 + lead * 0.15;
    float ph0 = fract(cycles);
    float ph1 = fract(cycles + 0.5);
    float w = abs(1.0 - 2.0 * ph0);

    // Which way the detail travels: OUT of the puff, and up.
    //
    // Along the sphere normal, the same one the light uses. A convective
    // parcel does not slide, it grows - it pushes outward from where it is
    // rising and carries its texture with it, which is why the surface of a
    // cumulus appears to boil out of itself. Following the normal puts that
    // motion where the shape already implies it: detail leaves the middle of
    // a puff, travels out across the curve, and dissolves at the rim.
    //
    // A clock could never produce that. An offset on a circle - which is
    // what this was - moves every fragment of every puff the same way at the
    // same instant, so the field slides as one and reads as wind however the
    // path is dressed up. The direction has to come from the geometry, and
    // the geometry is right here.
    //
    // Never downward, though. Air in a convective cloud goes up and spreads;
    // the underside is where it is fed from, not where it flows to. So the
    // vertical component is clipped at zero and given a lift on top of that
    // - the lower half of a puff flows sideways rather than draining out of
    // the bottom of it.
    vec3 flow_w = normalize(vec3(sphere_n.xy,
                                 max(sphere_n.z, 0.0) + SS_FLOW_RISE));

    // The same direction seen in each plane's own two axes. Whichever plane
    // the triplanar weights favour, the detail is travelling the same way
    // through the world.
    // Scaled by boil, so the top of the layer travels further per cycle than
    // the base does. Safe to vary per fragment here in a way it never was on
    // the rate: this multiplies a DISTANCE that resets every cycle, so it
    // cannot accumulate into the growing shear that scaling the rate caused.
    float reach = min(SS_FLOW_M * boil / oct2_m, SS_FLOW_MAX_TILES);
    vec2 flow_yz = vec2(flow_w.y / streak, flow_w.z) * reach;
    vec2 flow_xz = vec2(flow_w.x / streak, flow_w.z) * reach;
    vec2 flow_xy = vec2(dot(flow_w.xy, ss_wind) / streak,
                        dot(flow_w.xy, across)) * reach;

    float oct2 = tri.x * ss_flow(pl_yz / oct2_m + off_yz, flow_yz, ph0, ph1, w)
               + tri.y * ss_flow(pl_xz / oct2_m + off_xz, flow_xz, ph0, ph1, w)
               + tri.z * ss_flow(pl_xy / oct2_m + off_xy, flow_xy, ph0, ph1, w);

    // Folded in around the midpoint rather than averaged, so the fine
    // octaves push the density either way instead of dragging everything
    // toward mid-grey and flattening the base octave out.
    // One detail octave now, not two. Advecting it costs a second sample per
    // plane, and a single octave that genuinely rises and dissolves says
    // more than two that slide.
    noise += (oct2 - 0.5) * (SS_OCT2_W + SS_OCT3_W);
    noise = clamp(noise, 0.0, 1.0);

    // Window and noise combined by ADDING, the same way the dome layer
    // biases its own noise with coverage (cloudsF.glsl). Multiplying would
    // give a circle with texture painted on it; adding lets the noise decide
    // where the edge falls - solid through the core where the window
    // dominates, ragged and broken toward the rim where the noise does.
    float density = clamp((noise - 0.5) * SS_PUFF_CONTRAST + shape, 0.0, 1.0) * rim;

    // ...and cut flat underneath - see SS_BASE_SOFT_M. Softened over a few
    // tens of metres rather than a hard edge, because a real cloud base is
    // ragged at the scale of the wisps hanging off it, just not at the scale
    // of the deck.
    density *= smoothstep(ss_base_z, ss_base_z + SS_BASE_SOFT_M, vary_world.z);

    // ...and flat on top too, once there is an anvil to flatten - see
    // SS_TOP_SOFT_M. Faded in by ss_anvil so an ordinary convective sky
    // keeps its rounded tops and only a driven one gets the table.
    float top_z = ss_base_z + ss_layer_thick;
    float lid = 1.0 - smoothstep(top_z - SS_TOP_SOFT_M, top_z, vary_world.z);
    density *= mix(1.0, lid, ss_anvil);

    float a = density * vary_color.a * ss_puff_density;

    // Fade out where the puff meets solid geometry.
    //
    // The depth test only ever gives the all-or-nothing answer: a fragment
    // is in front of the surface or it is gone, and the boundary between
    // those two is the quad's own outline drawn across whatever it ran into.
    // That is the hard intersection - the one thing that says "card" no
    // matter how good the shape is.
    //
    // What is wanted is the DISTANCE to that surface, so the puff thins as
    // it closes on it and gathers as haze against it instead of ending on an
    // edge. Same idea as ambient occlusion reading proximity to geometry,
    // spent on alpha rather than on shadow.
    if (ss_soft_m > 0.0)
    {
        float scene_z = ss_eye_z(texture(depthMap, gl_FragCoord.xy / screen_res).r);
        float frag_z = ss_eye_z(gl_FragCoord.z);
        a *= clamp((scene_z - frag_z) / ss_soft_m, 0.0, 1.0);
    }

    if (a <= 2.0 / 255.0)
    {
        discard;
    }

    // Shaded as the sphere the quad stands in for, the same way the
    // celestial discs are: the billboard carries its normal implicitly,
    // because the disc IS the projection of a sphere.
    //
    // Axes spanning the quad, taken from the world rather than the screen -
    // see the note on derivatives above. Any pair perpendicular to the
    // normal will do: rotating the frame within the quad's own plane turns
    // the fake sphere about the view axis, which a wrapped light term cannot
    // tell apart.
    // Wrapped rather than clamped - see SS_FORM_DARK. Light goes through
    // cloud, so there is no dark side, only a dimmer one.
    float wrap = 0.5 + 0.5 * dot(sphere_n, ss_light_dir);

    vec3 body = vary_color.rgb
              * mix(SS_FORM_DARK, 1.0, wrap)
              * mix(1.0 - SS_PUFF_SHADE, 1.0, noise);

    // The bright fringe where the puff is thin enough for light to come
    // through it - see SS_RIM.
    float thin = 1.0 - density;
    body += ss_sun_color * (SS_RIM * wrap * thin * thin * thin);

    // Lightning inside the deck. Each strike is a point source, so it gets
    // its own wrapped sphere term against ITS direction - which is the whole
    // difference between a puff that brightens and a puff that is lit from
    // somewhere. Wrapped rather than clamped for the same reason the sun is:
    // light goes through cloud, so the far side dims, it does not go black.
    for (int i = 0; i < ss_strike_count; ++i)
    {
        vec3 to_strike = ss_strike[i].xyz - vary_world;
        float dist = length(to_strike);
        if (dist < 0.001) continue;

        float reach = SS_STRIKE_REACH * SS_STRIKE_REACH;
        float atten = reach / (reach + dist * dist * 4.0);

        float lit = 0.5 + 0.5 * dot(sphere_n, to_strike / dist);
        lit = mix(SS_FORM_DARK, 1.0, lit);

        // A thin edge of puff with a discharge behind it glows through,
        // exactly as it does with the sun - and this is what a bolt seen
        // THROUGH cloud actually looks like from below.
        float through = 1.0 + SS_RIM * thin * thin;

        body += ss_strike_color * (ss_strike[i].w * atten * lit * through);
    }

    // ...and then the atmosphere, over distance, exactly as it treats
    // everything else in the world. This is what lets a far puff sit IN the
    // sky rather than in front of it.
    float haze = 1.0 - exp(-eye_dist / SS_HAZE_M);
    vec3 shaded = mix(body, ss_haze, haze);

    // Bounded exactly the way the dome layer bounds itself (cloudsF.glsl).
    //
    // Everything feeding this is in EEP's HDR units - sunlight and ambient
    // both run well past 1, and the rim term adds a whole sun colour on top
    // - so the shading came out far brighter than anything else in the
    // frame. Nothing writes to a glow buffer here, but the bloom pass takes
    // its bright-pass off the finished screen, and unclamped cloud sails
    // straight over that threshold. Hence the halo around every puff.
    //
    // Clamping to 1 and doubling is not a taste decision: it is the range
    // the dome layer already occupies, so the two kinds of cloud end up on
    // the same scale as well as out of the bloom.
    shaded = clamp(shaded, vec3(0.0), vec3(1.0)) * 2.0;

    frag_color = vec4(shaded, a);
}

// </SS:Nexii>
