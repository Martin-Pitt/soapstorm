/**
 * @file ssatmoenvasset.h
 * @brief Atmo Magic: the unified environment asset. One document replaces
 *        separate sky/water/day-cycle assets and the v2 per-track weather
 *        notecard - see doc/atmo_magic_environment.md for the design this
 *        implements. Track/Water/Weather (phase 1), Planetary (phase 6),
 *        the volumetric Cloud Field (phase 7), the legacy-layer Cloud
 *        Dome and Atmosphere & Lighting all have real typed schema now.
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

#ifndef SS_ATMOENVASSET_H
#define SS_ATMOENVASSET_H

// <SS:Nexii> Atmo Magic: unified environment asset

#include <cmath>

#include "llsd.h"
#include "lluuid.h"

#include <cfloat>
#include <string>
#include <vector>

#include "ssatmoenvkeyframe.h"

class LLSettingsSky;

// Bumped whenever the on-disk shape changes in a way an older build cannot
// make sense of. fromLLSD() refuses anything newer than this build
// understands rather than guessing at a partial read. There is no
// migration machinery for older versions and none is planned until there
// are actually notecards in the wild to migrate - pre-release schema
// changes just change the format.
const S32 SS_ATMOENV_VERSION = 1;

// Mandatory ground track (index 0) plus up to seven optional tracks. This
// started out matching EEP's four altitude tracks, but there was never a
// reason to inherit that limit: an Atmo Magic track's floor is freely
// authored rather than tied to the region's altitude settings, so the real
// constraint is how many markers the floater's rail can show without them
// colliding - see the overlap_threshold on track_altitude_slider.
const S32 SS_ATMOENV_MIN_TRACKS = 1;
const S32 SS_ATMOENV_MAX_TRACKS = 8;

// Everything above the topmost track belongs to that track. 4096m is the
// hard ceiling a region actually builds to, so a track floor can never be
// authored above it and there is nothing meaningful past it to describe.
const F32 SS_ATMOENV_REGION_CEILING = 4096.f;

// The lowest floor an optional track may have. Ground owns everything below
// it, so a track sitting at (or near) 0 would be claiming ground's own band
// - and on the floater's rail its marker would collide with the ground
// marker. Matches the minimum separation the rail enforces between any two
// markers, so the spacing rule reads the same everywhere.
const F32 SS_ATMOENV_MIN_TRACK_FLOOR = 256.f;

// Highest water height the floater will let you author, matching the limit
// SL puts on a region's own water (see panel_region_terrain.xml's
// water_height_spin). Purely a UI bound: fromLLSD deliberately does not
// enforce it, so a hand-written or generated notecard can put a track's
// water anywhere up to the region ceiling - a skybox with its own sea at
// 2000m is a legitimate thing to describe, just not something worth giving
// up the useful resolution of a 0-100m slider for.
const F32 SS_ATMOENV_WATER_CEILING = 100.f;

// How many positions the editor's preview scrubber can actually stop on,
// over one full cycle. The slider runs 0..100 with an increment of
// 100 / this, and anything that places a keyframe without a human dragging
// that slider quantises to the same grid.
//
// That matters more than it sounds: the head only reads as sitting ON a
// keyframe when it lands within PHASE_EPSILON of it, so a keyframe placed at
// a phase the slider cannot reach is one an author can scrub past but never
// onto - its diamond never fills, and the row it belongs to never offers to
// edit it. Seeding measures phases from a world's own sun and those land
// anywhere; this is what puts them somewhere reachable.
//
// 100 makes the scrubber a percentage of the cycle, which is what a phase
// actually is - the day length only turns it into a time afterwards, and a
// scrubber whose stops were durations would move whenever the track's day
// length changed. One stop per percent is 2.4 minutes of a four-hour day.
const S32 SS_ATMOENV_PREVIEW_STEPS = 100;

// Snap a phase to the nearest scrubber stop, wrapped into [0, 1).
inline F64 ss_atmoenv_snap_phase(F64 phase)
{
    const F64 steps = (F64)SS_ATMOENV_PREVIEW_STEPS;
    F64 snapped = std::floor(phase * steps + 0.5) / steps;
    snapped = std::fmod(snapped, 1.0);
    if (snapped < 0.0) snapped += 1.0;
    return snapped;
}

// One track's moisture/convection/temperature cube plus wind. This is the
// authored input side only - the precipitation-type formula, ground-state
// table, and convection-threshold table from the design doc's Weather tab
// are phase 5 (they read this struct, they do not live in it).
struct SSAtmoEnvWeather
{
    // The core cube, plus wind heading/speed - keyframable, per the design
    // doc's "every parameterized slider ... has a keyframe icon" rule. See
    // ssatmoenvkeyframe.h: no keyframes on a field is just its plain value,
    // exactly as if this were still a bare F32.
    //
    // Range clamping (0..1 for moisture/convection, -30..40 for
    // temperature) used to happen in fromLLSD() when these were bare
    // floats; the generic keyframe container has no notion of a field's
    // valid range, so that clamp is deferred to whatever reads these values
    // (phase 5) rather than half-implemented here. A hand-edited notecard
    // can put an out-of-range value in for now.
    SSAtmoEnvKeyframed<F32> mMoisture{0.f};      // 0..1
    SSAtmoEnvKeyframed<F32> mConvection{0.f};    // 0..1
    SSAtmoEnvKeyframed<F32> mTemperatureC{15.f}; // -30..40

    SSAtmoEnvKeyframed<F32> mWindHeading{0.f};   // degrees, 0 = north, 90 = east
    SSAtmoEnvKeyframed<F32> mWindSpeed{0.f};     // m/s

    // Auto derives from the cube; false means mGust* below are authored
    // overrides rather than a computed default. The Auto flags themselves
    // stay plain bools - whether a field is hand-authored at all is a
    // structural choice, like a water plane existing, not an animatable
    // one - but the override values are keyframable like everything else
    // on this tab. The container reads a bare scalar as a plain value, so
    // notecards written while these were plain F32s load unchanged with no
    // version bump.
    bool mGustAuto = true;
    SSAtmoEnvKeyframed<F32> mGustDepth{0.f};    // 0..3
    SSAtmoEnvKeyframed<F32> mGustLength{140.f}; // metres between fronts
    SSAtmoEnvKeyframed<F32> mGustVeer{0.f};     // degrees

    bool mLightningAuto = true;
    SSAtmoEnvKeyframed<F32> mLightningIntensity{0.f}; // 0..1

    // Empty means "Auto": derive from the cube via the precipitation-type
    // formula. Otherwise one of Snow/Blizzard/FreezingRain/Sleet/SlushMix/
    // Rain/Hail, forced regardless of what the cube would otherwise pick.
    // Keyframed with the HOLD curve in mind: there is no sensible value
    // "between" Rain and Hail, so a keyframe here always steps rather than
    // blends - see ss_atmoenv_lerp<std::string>.
    SSAtmoEnvKeyframed<std::string> mPrecipitationOverride{std::string()};

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// A track's optional water plane. Height is independent of the track's own
// altitude band; only the lowest *enabled* track's plane is ever the one
// globally visible one, regardless of which track the avatar is in.
//
// Every field below except mEnabled is keyframable, unlike EEP's water
// settings which are a fixed snapshot per day-cycle frame: a tide that
// rises over the day, fog that thickens toward dusk, and waves that pick
// up with the weather are all things this is meant to be able to express
// on its own. The parameter set deliberately mirrors EEP's own Water panel
// (panel_settings_water.xml) one-for-one so anything authored there has a
// direct equivalent here.
struct SSAtmoEnvWater
{
    // Not keyframable: a water plane blinking in and out mid-cycle isn't a
    // tide, it's a bug. Whether a track *has* water is a structural choice,
    // not an animated one.
    bool mEnabled = false;

    SSAtmoEnvKeyframed<F32> mHeight{0.f};   // metres - the tide

    // Fog: colour, how fast it thickens with depth, and how much stronger
    // it reads once the camera is actually under the surface.
    SSAtmoEnvKeyframed<LLColor3> mFogColor{LLColor3(0.f, 0.24f, 0.34f)};
    SSAtmoEnvKeyframed<F32> mFogDensity{16.f};        // 0.001..100, exponent
    SSAtmoEnvKeyframed<F32> mUnderwaterModifier{0.25f}; // 0..20

    // Fresnel: how much the surface mirrors vs. sees through, by angle.
    SSAtmoEnvKeyframed<F32> mFresnelScale{0.4f};   // 0..1
    SSAtmoEnvKeyframed<F32> mFresnelOffset{0.5f};  // 0..1

    // Surface normal map, and the two scrolling wave layers sampled from
    // it. The map itself holds across a keyframe rather than blending -
    // see ss_atmoenv_lerp<LLUUID>.
    SSAtmoEnvKeyframed<LLUUID> mNormalMap{LLUUID::null};
    SSAtmoEnvKeyframed<LLVector2> mLargeWaveSpeed{LLVector2(0.f, -0.2f)}; // -20..20 each
    SSAtmoEnvKeyframed<LLVector2> mSmallWaveSpeed{LLVector2(0.f, -0.3f)};

    // Reflection wavelet scale, per axis - EEP exposes these as three
    // independent sliders rather than one vector control, and they are
    // authored independently often enough that keeping them as three
    // separately keyframable fields matches how they're actually used.
    SSAtmoEnvKeyframed<F32> mNormalScaleX{2.f};  // 0..10
    SSAtmoEnvKeyframed<F32> mNormalScaleY{2.f};
    SSAtmoEnvKeyframed<F32> mNormalScaleZ{2.f};

    // Refraction and blur.
    SSAtmoEnvKeyframed<F32> mRefractionScaleAbove{0.03f}; // 0..3
    SSAtmoEnvKeyframed<F32> mRefractionScaleBelow{0.2f};  // 0..3
    SSAtmoEnvKeyframed<F32> mBlurMultiplier{0.04f};       // 0..0.5

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// Suns per system are capped: the canonical topology normalizeSunTopology()
// enforces (a pair, then an outer pair orbiting the inner pair's barycenter)
// only has slots for two pairs, and past four the diagram stops reading as
// anything an author could reason about anyway.
const S32 SS_ATMOENV_MAX_SUNS = 4;

// One celestial body. Sun -> Planet -> Moon, exactly three levels, any
// number of bodies per level - see the design doc's Planetary tab. Position
// is always fixed/authored (radius + inclination + phase), never simulated;
// orbital/rotation period are stored for a future "actually animate this"
// toggle, unused by anything today.
struct SSAtmoEnvCelestialBody
{
    enum EKind { SUN = 0, PLANET = 1, MOON = 2 };

    EKind mKind = PLANET;
    std::string mName = "Body";

    // False while the name is autoNameBodies()'s to manage ("Sol II",
    // "Sol I.2"); flipped true the moment the user commits a name of their
    // own, after which auto-naming never touches this body again. No
    // version bump for the new key - there are no notecards in the wild,
    // and an absent key just reads false (auto-named), which is right for
    // anything older.
    bool mNameCustom = false;

    // Index into the track's mBodies this one orbits - -1 for a root Sun,
    // which orbits nothing (it's "the centre of the universe" for this
    // system, per the design doc, since there's no mechanism to place one
    // anywhere else). A Planet's parent is a Sun; a Moon's parent is a
    // Planet - the resolver does not re-validate the hierarchy shape
    // beyond following mParentIndex, so a hand-edited notecard that breaks
    // the Sun->Planet->Moon rule degrades to "orbits whatever index it
    // says" rather than a hard failure.
    // Ignored entirely for PLANET bodies - see
    // SSAtmoEnvPlanetary::effectiveParent(); a planet belongs to the sun
    // system as a whole, not to any authored parent.
    S32 mParentIndex = -1;

    // Physical size and mass. Diameter drives apparent angular size once a
    // distance is resolved; mass only matters for a bound pair's
    // barycenter (below).
    //
    // Mass is stored in per-LEVEL units so it reads naturally in the
    // floater: solar masses for a sun, Earth masses for a planet or moon
    // (Luna is 0.0123). Mixing units across levels is sound because only
    // same-level ratios ever feed a barycenter - a bound pair is always
    // two siblings of the same kind's scale.
    F32 mDiameterM = 1.0e7f; // metres; Earth-ish default
    F32 mMassRelative = 1.f; // per-level units - see above

    // The authored, fixed orbital position: distance from parent (or from
    // a bound pair's shared barycenter - see mBoundPartnerIndex), how far
    // around the orbit (0-360, "phase"/true-anomaly analogue), and how
    // tilted that orbit's plane is relative to the parent's own reference
    // plane. Eccentricity is deferred per the design doc.
    F32 mOrbitalRadius = 1.0e8f; // metres - see mSunPlanetScale/mPlanetMoonScale; the floater displays AU/km per kind
    F32 mOrbitalInclinationDeg = 0.f;
    F32 mOrbitalPhaseDeg = 0.f;

    // Stored, not yet consumed by anything - see the file comment. Both in
    // seconds once populated; days/years is an authoring-UI concern, not a
    // schema one.
    F64 mOrbitalPeriodSeconds = 0.0;
    F64 mRotationPeriodSeconds = 0.0;

    // Tilts this body's own spin axis relative to its orbital plane -
    // distinct from mOrbitalInclinationDeg, which tilts the *orbit*, not
    // the body. Only visibly affects anything once bodies render as
    // spheres rather than quads (not yet); the home body's tilt is also
    // what drives the computed primary sun's seasonal arc regardless of
    // rendering mode - see SSAtmoEnvPlanetaryResolver.
    // Obliquity: how far the body's own spin axis leans out of its orbital
    // plane. On the HOME body this is what gives it seasons - it tips the
    // sun's daily circle north and south of the celestial equator through
    // the year - and it is NOT where the observer stands. That is
    // mLatitudeDeg below.
    F32 mAxialTiltDeg = 0.f;

    // Where on the home body the observer is standing, in degrees north of
    // its equator. Meaningless on any other body.
    //
    // This is what sets the sky's whole geometry: the celestial pole sits
    // exactly this far above the northern horizon, so at 0 the sun climbs
    // through the zenith and every star rises vertically, at 90 nothing
    // rises or sets at all, and in between the sky turns at an angle. It
    // used to be conflated with mAxialTiltDeg, which made a world's seasons
    // and its latitude the same dial and neither of them right.
    //
    // 50 degrees by default - a temperate northern latitude, where the sun
    // reaches about 40 degrees at an equinox noon and the seasons are
    // pronounced without the year collapsing into polar day and night.
    F32 mLatitudeDeg = 50.f;

    // Drawn at full brightness with no terminator: the body is a light
    // source rather than something lit by one. On by default for suns and
    // off for everything else (see addBody), but authored rather than
    // derived - a glowing artificial moon, a lava world or a magical
    // second sun that is technically a planet are all things a world might
    // want, and deriving this from kind would make them unauthorable.
    //
    // Takes precedence over mPhaseShaded: something emitting its own light
    // has no dark side to draw.
    bool mEmissive = false;

    // Shade the disc as the sphere it stands in for - N.L against the
    // direction of this body's star, giving it a phase. On for anything not
    // emissive by default.
    //
    // Toggleable because it is a look as much as a simulation: a stylised
    // world may want its moons drawn as flat discs the way the stock sky
    // does, and a body whose art already has a terminator painted into it
    // would otherwise get a second one.
    bool mPhaseShaded = true;

    // (There is deliberately no per-body brightness dial. How bright a body
    // looks is worked out from where it is - its phase, and whether its
    // star is shining on it at all - and an authored multiplier on top of
    // that was just a way to disagree with the geometry.)
    F64 mSpinPeriodSeconds = 0.0; // 0 = tidally locked (no visible self-rotation)

    // Exactly one body across the whole track's mBodies should have this
    // set - enforced by SSAtmoEnvPlanetary::setHomeBody(), not by this
    // struct alone. The home body supplies mAxialTiltDeg to the primary
    // sun's computed arc and is never itself rendered (you don't see
    // yourself in the sky) or a light emitter.
    bool mIsHome = false;

    // At most two bodies across the whole track should have this set - see
    // SSAtmoEnvPlanetary::canSetLightEmitter(). A body flagged home can
    // never also be a light emitter.
    bool mIsLightEmitter = false;

    // Hierarchical binary pair: index of the sibling this body shares a
    // computed barycenter with, or -1 for an ordinary single-parent orbit.
    // Symmetric - if A's partner is B, B's partner must be A - and only
    // ever between two bodies with the same mParentIndex. True N-body
    // (3+ mutually orbiting) is out of scope; a third body orbits the
    // *pair's* combined point by giving it mParentIndex pointing at either
    // paired body - see SSAtmoEnvPlanetaryResolver for how that's resolved.
    S32 mBoundPartnerIndex = -1;

    // Rendering: quad/billboard only for v1. An equirectangular texture on
    // a sphere, and a real 3D ring, are both deferred until that mode
    // exists - see the design doc's "Deferred / explicitly out of scope".
    LLUUID mCustomTexture;

    bool mHasRing = false;
    F32 mRingInnerRadius = 1.5f; // multiples of mDiameterM/2
    F32 mRingOuterRadius = 2.2f;
    LLUUID mRingTexture;

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// A track's planetary system: every celestial body plus the two artistic
// distance-scale dials that compress authored orbital *distances* (not any
// body's own physical size) for effect - a nearby-looking ring system, a
// moon that dominates the sky.
struct SSAtmoEnvPlanetary
{
    std::vector<SSAtmoEnvCelestialBody> mBodies;

    // Compresses Sun<->Planet and Planet<->Moon orbital radii respectively,
    // multiplicatively (1.0 = authored distance, unmodified). Kept as two
    // separate dials rather than one because a system can want its planets
    // spread out realistically while its moons loom close, or vice versa.
    F32 mSunPlanetScale = 1.f;
    F32 mPlanetMoonScale = 1.f;

    // Index of the one body flagged home, or -1 if mBodies is empty. Moving
    // "home" to a different body is just calling this again - it clears
    // the flag everywhere else first, so exactly one is ever set.
    S32 homeBodyIndex() const;
    bool setHomeBody(S32 index);

    // Indices of every body flagged as a light emitter (0, 1, or 2 of
    // them - never more, enforced by canSetLightEmitter()).
    std::vector<S32> lightEmitterIndices() const;
    // False once two bodies are already flagged, or if index is the home
    // body - matches the floater's own "disable the checkbox" rule rather
    // than silently picking a top-2 at some other layer.
    bool canSetLightEmitter(S32 index) const;

    // Appends a body of the given kind with per-kind physical defaults
    // (Sol/Earth/Luna scale, matching makeDefault()'s own sun and planet);
    // a planet or moon lands one step (1 AU / one Luna distance) outside
    // its outermost sibling rather than at a fixed radius.
    // Parenting is automatic, never the user's: suns get the canonical
    // topology normalizeSunTopology() enforces; planets always parent to
    // the first sun (whose barycenter rule makes that "orbits the inner
    // pair" in a multi-sun system); a moon parents to
    // preferred_parent_index when that names a planet, else the first
    // planet, else starts orphaned. Returns the new body's index, or -1
    // for a refused add (a fifth sun - see SS_ATMOENV_MAX_SUNS; other
    // kinds are uncapped, there is no rail or renderer slot limit for
    // them to collide with).
    //
    // Creation-time lighting defaults, never re-asserted afterwards: a
    // second sun, or a moon of the home planet while there is no second
    // sun, claims the free second light-emitter slot if one exists - a
    // fresh binary gets both stars lighting the scene, and a lone-sun
    // world that gains a moon gets the moon as its night light, matching
    // the renderer's sun+moon light slots. Never steals: two existing
    // emitters (or the home flag) mean the new body just isn't one, per
    // canSetLightEmitter().
    S32 addBody(SSAtmoEnvCelestialBody::EKind kind, S32 preferred_parent_index = -1);

    // Removes the body and fixes up every survivor's mParentIndex and
    // mBoundPartnerIndex: indices above the removed set shift down, and
    // anything that pointed into it becomes -1. Removing a PLANET takes
    // its moons with it - an orphaned moon used to be re-parentable in
    // the floater, but with parenting now fully automatic there is no UI
    // path back, so a stranded moon would just be clutter that can only
    // be deleted anyway. Removing a SUN does NOT cascade: planets belong
    // to the system, not to any one star - their lineage is derived by
    // effectiveParent() and their position anchors at the sun group's
    // barycenter, so no re-anchoring is even needed (a fully sunless
    // system is legal). If the removed body was home, home is
    // simply gone (-1 is a legal state - see homeBodyIndex()) rather than
    // guessed at. Sun topology and auto names re-normalise afterwards, so
    // removing half a sun pair can never leave the survivor an isolated
    // star.
    bool removeBody(S32 index);

    // The parent a body hangs under for HIERARCHY purposes (scene graph,
    // canvas grouping, moon ordinals). For a PLANET this is never the
    // stored index: planets intrinsically belong to the sun system - the
    // first sun stands in as their display parent, or -1 in a sunless
    // system - and whatever mParentIndex happens to hold is ignored, which
    // removes a whole class of stale-parent bugs (first sun removed, sun
    // added to a sunless system) by never storing the answer at all. Note
    // this is display lineage only: a planet's resolved POSITION anchors
    // at the sun group's collective barycenter (the origin), not at the
    // first sun - see SSAtmoEnvPlanetaryResolver::resolveWorldPositions.
    // Suns and moons return their stored parent, degraded to -1 when out
    // of range or self-referencing - the same forgiveness the resolver
    // has always shown a hand-edited notecard.
    S32 effectiveParent(S32 index) const;

    // Enforces the one canonical sun topology, by star count: one sun is
    // the root; two are a bound root pair; a third orbits that pair's
    // barycenter (by parenting to the first sun - the resolver substitutes
    // the pair's barycenter); a fourth pairs with the third, giving two
    // pairs, the outer orbiting the inner's barycenter. Called after every
    // sun add and every removeBody() so an isolated star - no orbit and no
    // pair - is impossible to reach. A sun placed into an orbiting role
    // with no authored separation yet (radius 0, the root default) gets a
    // sensible one. "One pair orbiting a giant sun" needs no special case:
    // it is just pair separations/masses that put the barycenter inside
    // the primary.
    void normalizeSunTopology();

    // SpaceEngine-style automatic names for every body whose mNameCustom
    // is false: first sun "Sol" (its name, custom or not, is the stem for
    // everything below), further suns "Sol B"/"Sol C"/"Sol D" in structure
    // order, planets "Sol I"/"Sol II"/... by orbital radius ascending
    // across all planets, moons "<planet name>.1"/".2" by radius around
    // their planet - a custom-named planet's moons still follow it
    // ("Tatooine.1"). Called after every add/remove and every
    // orbital-radius commit, since radius ordering is what the ordinals
    // encode.
    void autoNameBodies();

    // Flags a and b as a hierarchical bound pair - see mBoundPartnerIndex.
    // Only valid between two distinct bodies with the same mParentIndex;
    // any existing partnership either one is in is dissolved first, so the
    // symmetry invariant can never be left dangling on a third body.
    bool setBoundPartner(S32 a, S32 b);
    // Symmetric clear: the partner's own back-reference goes too.
    bool clearBoundPartner(S32 index);

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// A track's volumetric storm-cloud tunables - the "Volumetric Field"
// sub-tab. Separate from the legacy Windlight cloud layer (now cirrus-only
// and authored on the "Sky Dome" sub-tab - see SSAtmoEnvCloudDome), which
// needs none of this. Actual
// per-frame coverage/density/churn are derived from these plus the weather
// cube's moisture/convection by SSAtmoEnvCloudFieldResolver (phase 7); this
// struct is only the artist's tunable baseline.
struct SSAtmoEnvCloudField
{
    // Auto derives the baseline below from the weather cube's moisture and
    // convection - same split as the gust/lightning Auto flags: the toggle
    // is a plain structural bool, the values it parks stay keyframable
    // authored overrides. See SSAtmoEnvCloudFieldResolver::deriveAutoBaseline
    // for what Auto actually computes.
    bool mAuto = true;

    // Metres. The band this track's storm clouds occupy at Convection 0 -
    // Stable phase is "flat, low to ground" per the design doc's convection
    // table; height climbs from here as convection rises.
    //
    // Keyframable like the weather cube's fields - the container reads a
    // bare scalar as a plain value, so notecards written while these were
    // plain F32s load unchanged with no version bump. The >= 0 clamps
    // fromLLSD() used to apply moved to SSAtmoEnvCloudFieldResolver for
    // the same reason the cube's moved out: the generic keyframe container
    // has no notion of a field's valid range.
    SSAtmoEnvKeyframed<F32> mBaseHeightM{800.f};
    SSAtmoEnvKeyframed<F32> mBaseThicknessM{300.f};

    // Multiplies the derived coverage fraction - an artistic override for
    // "this track's storms are always more/less widespread than the cube
    // alone would suggest", independent of moisture.
    SSAtmoEnvKeyframed<F32> mCoverageScale{1.f};

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// A track's Sky Dome: the classic Windlight/EEP cloud-layer parameters -
// the legacy scrolling-noise plane, kept and demoted to cirrus duty above
// the volumetric field (see doc/atmo_magic_environment.md). Every field is
// keyframable. The parameter set deliberately mirrors EEP's own Clouds
// panel (panel_settings_sky_clouds.xml) one-for-one, so anything authored
// there has a direct equivalent here; defaults are LLSettingsSky's own.
// Cloud colour lives here rather than with the Atmosphere colours: it
// tints this layer and nothing else, so it moves with the layer it paints.
struct SSAtmoEnvCloudDome
{
    SSAtmoEnvKeyframed<LLColor3> mColor{LLColor3(0.4099f, 0.4099f, 0.4099f)};

    // EEP's "Cloud Coverage" slider actually drives cloud_shadow (see
    // llpaneleditsky.cpp's onCloudCoverageChanged -> setCloudShadow); the
    // UI-facing name is kept because it is what an author knows the dial
    // as, and the shadow name only surfaces at the applier's setter call.
    SSAtmoEnvKeyframed<F32> mCoverage{0.2699f}; // 0..1
    SSAtmoEnvKeyframed<F32> mScale{0.4199f};    // 0.01..3
    SSAtmoEnvKeyframed<F32> mVariance{0.f};     // 0..1

    SSAtmoEnvKeyframed<LLVector2> mScrollRate{LLVector2(0.2f, 0.01f)}; // -30..30 each

    // The main and detail noise samplings. EEP packs each triple into an
    // LLColor3 for its setter, but authors edit them as three independent
    // sliders - so, like the water wavelet scales, they are stored as
    // three separately keyframable scalars and only fold into the packed
    // form at the applier's setter call.
    SSAtmoEnvKeyframed<F32> mDensityX{1.f};    // 0..1
    SSAtmoEnvKeyframed<F32> mDensityY{0.526f}; // 0..1
    SSAtmoEnvKeyframed<F32> mDensityD{1.f};    // 0..3
    SSAtmoEnvKeyframed<F32> mDetailX{1.f};     // 0..1
    SSAtmoEnvKeyframed<F32> mDetailY{0.526f};  // 0..1
    SSAtmoEnvKeyframed<F32> mDetailD{1.f};     // 0..1

    // Null means "the stock cloud noise" - same guard idiom as the water
    // normal map; the applier substitutes GetDefaultCloudNoiseTextureId(),
    // so a keyframe stepping back to null restores the default rather than
    // silently keeping the last custom map.
    SSAtmoEnvKeyframed<LLUUID> mNoiseTexture{LLUUID::null};

    // Cloud art worth reaching for, named so an author can find them in the
    // texture picker rather than having to know a UUID. Layered Clouds is
    // what a new environment starts on (see makeSeededDefault); the other
    // two are here because a sky wanting weight or structure wants a
    // different map, not a different coverage number.
    static const char* const CLOUD_TEXTURE_LAYERED;      // cirrus-like decks
    static const char* const CLOUD_TEXTURE_CUMULONIMBUS; // storm towers
    static const char* const CLOUD_TEXTURE_ALTOCUMULUS;  // broken mid-level

    // Disc art for the bodies themselves. Named here beside the cloud maps
    // for the same reason: an author should be able to find them in a
    // picker rather than having to know a UUID.
    static const char* const BODY_TEXTURE_SUN;
    static const char* const BODY_TEXTURE_MOON;

    // Repopulates every field above as a plain (un-keyframed) value read
    // from a live EEP sky - the same creation-time seeding step
    // SSAtmoEnvAtmosphere::fromSettingsSky performs for its own fields,
    // called alongside it (see SSAtmoEnvManager::createDefaultNotecard).
    // A noise texture equal to the stock one reads back as null so the
    // "null = default" convention holds from the document's first save.
    void fromSettingsSky(const LLSettingsSky& sky);

    // The keyframed sibling of fromSettingsSky: stamps every field it
    // covers as a keyframe at `phase` instead of replacing each container
    // wholesale, so several skies stamped at several phases build a day
    // cycle out of EEP presets. Same field set, same null-for-stock noise
    // convention - see SSAtmoEnvAtmosphere::addKeyframesFromSky for the
    // shared contract.
    void addKeyframesFromSky(const LLSettingsSky& sky, F64 phase);

    // Post-seeding cleanup - see SSAtmoEnvAtmosphere::collapseConstantKeyframes.
    void collapseConstantKeyframes();

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// A track's Atmosphere & Lighting: the classic haze/gamma/glow set, every
// field keyframable. The parameter set deliberately mirrors EEP's own
// Atmosphere and Sun & Moon panels (panel_settings_sky_atmos.xml /
// panel_settings_sky_sunmoon.xml) so anything authored there has a direct
// equivalent here; defaults are LLSettingsSky's own. Sun/moon POSITION is
// deliberately absent: azimuth/elevation are computed from the home body's
// axial tilt and rotation by the Planetary tab's resolver, replacing EEP's
// keyframed sun arc entirely - see the design doc's Planetary section.
struct SSAtmoEnvAtmosphere
{
    // Colours. Ambient is the flat fill light, Blue Horizon/Density shape
    // the sky's gradient and Sunlight tints the sun's direct light - all
    // exactly EEP's meanings, so an EEP sky can be transcribed swatch for
    // swatch. Cloud colour is deliberately not here: it tints the legacy
    // cirrus layer and belongs to SSAtmoEnvCloudDome with the rest of that
    // layer's parameters.
    SSAtmoEnvKeyframed<LLColor3> mAmbientColor{LLColor3(0.25f, 0.25f, 0.25f)};
    SSAtmoEnvKeyframed<LLColor3> mBlueHorizon{LLColor3(0.4954f, 0.4954f, 0.6399f)};
    SSAtmoEnvKeyframed<LLColor3> mBlueDensity{LLColor3(0.2447f, 0.4487f, 0.7599f)};
    SSAtmoEnvKeyframed<LLColor3> mSunlightColor{LLColor3(0.7342f, 0.7815f, 0.8999f)};

    // Haze and atmospheric falloff, in EEP's own panel order. Ranges are
    // the UI's (the floater's sliders match EEP's panel bounds); like
    // every other keyframed field nothing here re-clamps on read - see
    // the weather cube's comment.
    SSAtmoEnvKeyframed<F32> mHazeHorizon{0.19f};         // 0..5
    SSAtmoEnvKeyframed<F32> mHazeDensity{0.7f};          // 0..5

    // The PBR-era optics dials, in the positions EEP's panel gives them.
    // SKY moisture drives the rainbow/halo optics around the sun - an
    // optical dial with nothing to do with the Weather cube's moisture,
    // which drives precipitation; the field name keeps EEP's own Sky
    // prefix so the two can never be confused in code either. Droplet
    // radius is in micrometres.
    SSAtmoEnvKeyframed<F32> mSkyMoistureLevel{0.f};      // 0..1
    SSAtmoEnvKeyframed<F32> mSkyDropletRadius{800.f};    // um, 5..1000
    SSAtmoEnvKeyframed<F32> mSkyIceLevel{0.f};           // 0..1

    SSAtmoEnvKeyframed<F32> mDensityMultiplier{0.0001f}; // 0.0001..2
    SSAtmoEnvKeyframed<F32> mDistanceMultiplier{0.8f};   // 0.05..1000
    SSAtmoEnvKeyframed<F32> mMaxAltitude{1605.f};        // metres, 0..10000

    // Non-zero switches EEP's lighting model to HDR (its own panel then
    // relabels Brightness to "HDR Scale"). Stored and applied plainly:
    // EEP's commit handler calls setReflectionProbeAmbiance with no
    // gating, and the RenderSkyAutoAdjustLegacy-aware read its refresh
    // does is a display convenience, not part of the authored value.
    SSAtmoEnvKeyframed<F32> mReflectionProbeAmbiance{0.f}; // 0..10
    SSAtmoEnvKeyframed<F32> mSceneGamma{1.f};            // 0..20

    // Lighting: the stars, and the glow disc around a light emitter. Glow
    // is stored in the same UI-space scale EEP's own sliders use rather
    // than the renderer's packed glow colour (size 5.0 == UI 1.75, focus
    // -0.48 == UI 0.096 via llpaneleditsky's SLIDER_SCALE_GLOW_R/B) - a
    // notecard should read like the panel that authored it, and the
    // packing is a renderer concern for the phase that consumes this.
    SSAtmoEnvKeyframed<F32> mStarBrightness{250.f};      // 0..500
    SSAtmoEnvKeyframed<F32> mGlowFocus{0.096f};          // -2..2
    SSAtmoEnvKeyframed<F32> mGlowSize{1.75f};            // 0..1.99

    // The moon DISC's own luminance (EEP's Brightness slider under Moon) -
    // an appearance dial like the sunlight colour, so it lives here rather
    // than on the Planetary tab: which body occupies the moon slot is
    // planetary structure, how brightly its disc renders is lighting.
    SSAtmoEnvKeyframed<F32> mMoonBrightness{0.5f};       // 0..1

    // Repopulates every field above as a plain (un-keyframed) value read
    // from a live EEP sky - the "transcribed swatch for swatch" promise in
    // the colours comment, done in code. Used at creation time to seed a
    // fresh environment from EEP's stock non-legacy Midday sky (see
    // SSAtmoEnvManager::createDefaultNotecard); the constructor defaults
    // above stay as the documented fallback when that fetch fails. Sun and
    // moon rotations (and every other positional field the sky carries)
    // are deliberately not read: position is the Planetary tab's resolver's
    // to compute - see the struct comment.
    void fromSettingsSky(const LLSettingsSky& sky);

    // The keyframed sibling of fromSettingsSky: every field it covers gets
    // a keyframe at `phase` holding the sky's value, leaving whatever is
    // already authored at other phases alone. A field with no keyframes yet
    // is promoted (its first keyframe is the stamp), because writing the
    // plain value instead - what setValueAtHead alone would do - could not
    // build a cycle out of repeated stamps. Used both by creation-time
    // seeding from the four stock skies (SSAtmoEnvManager) and by dropping
    // an EEP sky onto the floater (SSFloaterAtmoEnv::handleDragAndDrop).
    void addKeyframesFromSky(const LLSettingsSky& sky, F64 phase);

    // Creation-seeding cleanup: collapseIfConstant() on every field
    // addKeyframesFromSky covers, so a field the stamped skies all agree
    // on goes back to being a plain value instead of carrying one
    // redundant keyframe per sky into every notecard.
    void collapseConstantKeyframes();

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// How much the weather cube is allowed to push the authored sky around.
//
// MODULATION, NEVER MUTATION: nothing here edits an authored value. The
// applier evaluates the keyframes exactly as written, then this config says
// how far the derived weather state may bend the result on its way to the
// renderer - so turning a mapping off returns precisely the authored sky,
// and an author scrubbing the timeline always sees the values they typed.
//
// Deliberately NOT keyframable, unlike almost everything else in this
// schema. These are a statement about how a world behaves ("storms darken
// my sky by this much"), not about what it looks like at 3pm; keyframing
// them would mean authoring the same storm twice, once in the weather cube
// and once in its own influence. The per-mapping strengths are the tuning
// surface instead - see SSFloaterAtmoWeatherInfluence.
//
// Each mapping is an independent enable + strength pair rather than one
// global dial, because they answer to different tastes: an author who wants
// storms to genuinely darken the world may still want the cloud layer to
// hold exactly the scroll they authored.
struct SSAtmoEnvWeatherInfluence
{
    // Master switch. Off means the applier skips the modulator entirely -
    // not "all strengths at zero", which would still cost the derivation.
    // On by default: a weather system whose weather does nothing to the sky
    // is the surprising configuration, and every individual mapping can
    // still be turned off underneath this.
    bool mEnabled = true;

    // Strengths are 0..1 scalings of each mapping's own full-effect range,
    // NOT raw parameter deltas - the ranges themselves live in the
    // modulator, so a strength means the same thing ("how much of it")
    // everywhere and no author has to know that haze density happens to top
    // out at 4.0.
    bool mCloudCoverEnabled = true;
    F32  mCloudCoverStrength = 1.f;    // okta cover -> cirrus dome coverage

    bool mWindScrollEnabled = true;
    F32  mWindScrollStrength = 1.f;    // wind heading/speed -> dome scroll

    bool mHazeEnabled = true;
    F32  mHazeStrength = 1.f;          // moisture -> haze density / distance / water fog

    bool mStormDarkeningEnabled = true;
    F32  mStormDarkeningStrength = 1.f; // convection -> variance up, gamma/ambient down

    bool mColdSkyEnabled = true;
    F32  mColdSkyStrength = 1.f;       // sub-freezing clear air -> ice level, bluer density

    bool mRainbowEnabled = true;
    F32  mRainbowStrength = 1.f;       // rain just stopped, sun up -> moisture-lit sky

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// One track: a complete, isolated environment - its own weather, its own
// (eventual) sky, clouds and planetary system. Nothing blends across
// tracks; a build's own floor/ceiling say when it is active, nothing more.
struct SSAtmoEnvTrack
{
    std::string mName = "Ground";

    // Where this track starts. There is deliberately no matching ceiling:
    // exactly like EEP's own sky-track altitudes, a track simply runs up
    // to wherever the next track begins, or to the top of the region if
    // nothing is above it - see SSAtmoEnvAsset::trackCeilingZ(). Storing a
    // ceiling too would let an author create gaps and overlaps that have
    // no sensible answer for "which track is the avatar in".
    //
    // The ground track (index 0) is pinned at 0 and is simply whatever
    // isn't claimed by an optional track above it.
    F32 mFloorZ = 0.f;

    // Soft cross-fade zone, in metres, for physically crossing into or out
    // of this track. Teleport / sit-teleport / region-position-change, and
    // any crossing where water is involved, bypass this and cut instantly.
    F32 mTransitionBuffer = 15.f;

    // Seconds. Per-track, not per-asset - each isolated track can be a
    // wholly different planet with its own spin, per the design doc's Q2:
    // a floating sci-fi skybox should be able to run a 3-hour day even
    // while the ground track below it runs a standard 4-hour one. Defaults
    // to stock SL's four-hour day; phase 6/8 is what defaults a *new*
    // track's value from the region's current EEP day-length instead.
    F64 mDayLengthSeconds = 4.0 * 60.0 * 60.0;
    F64 mDayOffsetSeconds = 0.0;

    SSAtmoEnvWater    mWater;
    SSAtmoEnvWeather  mWeather;
    SSAtmoEnvPlanetary mPlanetary;

    // Per track, like the weather cube it reads: a storm at 2000m has no
    // business dimming the sky at ground level, and a skybox track with no
    // weather of its own should not inherit the ground's.
    SSAtmoEnvWeatherInfluence mWeatherInfluence;

    // Two cloud layers, two homes: the volumetric field's tunables and the
    // legacy cirrus dome's parameters are deliberately separate structs,
    // mirrored by the Clouds tab's two sub-tabs.
    SSAtmoEnvAtmosphere mAtmosphere;
    SSAtmoEnvCloudField mCloudField;
    SSAtmoEnvCloudDome  mCloudDome;

    // How far through this track's own day cycle it is right now, as a
    // fraction in [0, 1) - the unit every keyframe is stored in, so this
    // feeds SSAtmoEnvKeyframed::valueAt() directly. Wraps real-world UTC
    // wall-clock time against mDayLengthSeconds/mDayOffsetSeconds, per the
    // design doc: two viewers with the same notecard loaded should see the
    // same phase, not one keyed to whenever each of them happened to load
    // it. The floater's own preview scrubber is a separate, explicit
    // override of "what point in the cycle to look at" and does not call
    // this - this is only for a live (non-editing) evaluation.
    F64 currentDayCyclePhase() const;

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// The whole environment: one document, no separate sky/water/day-cycle
// assets. See doc/atmo_magic_environment.md for the full design.
struct SSAtmoEnvAsset
{
    std::string mName = "New Atmo Environment";

    // Index 0 is always the mandatory ground track; size() is 1..4.
    std::vector<SSAtmoEnvTrack> mTracks;

    // One ground track, calm weather, nothing keyframed - the "hello world"
    // both the inventory New Atmo Magic action and the floater's one-time
    // create button write out. See doc: "Creation has two entry points,
    // same underlying action, no floater-side default."
    static SSAtmoEnvAsset makeDefault();

    // false if already at SS_ATMOENV_MAX_TRACKS
    bool addTrack();
    // false for index 0 (the mandatory ground track is never removable) or
    // an out-of-range index
    bool removeTrack(S32 index);

    // Where the track at `index` stops: the lowest floor above its own, or
    // SS_ATMOENV_REGION_CEILING if nothing sits above it. Derived rather
    // than stored (see SSAtmoEnvTrack::mFloorZ), and computed by scanning
    // rather than by assuming mTracks is sorted by altitude - dragging a
    // marker on the floater's rail past another one is allowed and must
    // not need a reordering pass to stay correct.
    F32 trackCeilingZ(S32 index) const;

    // Sorts mTracks by floor altitude, lowest first, so array order always
    // matches what the floater's rail shows bottom-to-top (and what a saved
    // notecard reads like). Ground is pinned at index 0 regardless - it is
    // 0m by definition and is the catch-all band, not a peer of the others.
    //
    // Nothing functional depends on this: trackContaining() and
    // trackCeilingZ() both scan rather than assuming an order, precisely so
    // dragging a marker past another can never silently break band
    // resolution. It is kept sorted anyway so the ordering can't become a
    // source of confusion later. Returns the new index of the track that
    // was at `follow_index`, so a caller holding a selection can keep
    // pointing at the same track across the sort.
    S32 sortTracksByAltitude(S32 follow_index = -1);

    // The default name a newly added track gets: the lowest "Track N" not
    // already in use, so adding after a delete doesn't collide with a name
    // still on another track. Names are authored, never rewritten behind
    // the user's back - a track keeps its identity when moved, which is the
    // whole point of a track being a self-contained biome rather than a
    // numbered slot (see doc/atmo_magic_environment.md).
    std::string nextDefaultTrackName() const;

    // The one globally visible water plane: whichever *enabled* track sits
    // lowest, regardless of which track the avatar currently occupies - a
    // high skybox can still see the sea far below it. Returns false if no
    // track has water enabled at all.
    bool visibleWaterHeight(F32& out_height) const;

    LLSD asLLSD() const;

    // On failure, out_error explains what did not parse and *this is left
    // as makeDefault() rather than partially populated.
    bool fromLLSD(const LLSD& sd, std::string& out_error);
};

// </SS:Nexii>

#endif // SS_ATMOENVASSET_H
