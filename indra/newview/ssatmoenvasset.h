/**
 * @file ssatmoenvasset.h
 * @brief Atmo Magic: the unified environment asset. One document replaces
 *        separate sky/water/day-cycle assets and the v2 per-track weather
 *        notecard - see doc/atmo_magic_environment.md for the design this
 *        implements. Track/Water/Weather (phase 1), Planetary (phase 6) and
 *        the volumetric Cloud Field (phase 7) all have real typed schema
 *        now; only Atmosphere & Lighting is still an opaque LLSD blob,
 *        pending its own phase.
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

#include "llsd.h"
#include "lluuid.h"

#include <cfloat>
#include <string>
#include <vector>

#include "ssatmoenvkeyframe.h"

// Bumped whenever the on-disk shape changes in a way an older build cannot
// make sense of. fromLLSD() refuses anything newer than this build
// understands rather than guessing at a partial read.
// 2: keyframe times became a phase in [0,1) rather than absolute seconds,
//    so that a track's day length is purely a playback-speed control - see
//    SSAtmoEnvKeyframe::mTime. Version 1 assets are migrated on read rather
//    than rejected (SSAtmoEnvTrack::fromLLSD).
const S32 SS_ATMOENV_VERSION = 2;

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
    // overrides rather than a computed default. Not yet keyframable - these
    // stay plain scalars until the Weather tab's real UI (phase 5) settles
    // the exact field list; converting them then is the same mechanical
    // change already proven on the fields above.
    bool mGustAuto   = true;
    F32  mGustDepth  = 0.f;
    F32  mGustLength = 140.f;
    F32  mGustVeer   = 0.f;

    bool mLightningAuto      = true;
    F32  mLightningIntensity = 0.f;

    // Empty means "Auto": derive from the cube via the precipitation-type
    // formula. Otherwise one of Snow/Blizzard/FreezingRain/Sleet/SlushMix/
    // Rain/Hail, forced regardless of what the cube would otherwise pick.
    // Keyframed with the HOLD curve in mind: there is no sensible value
    // "between" Rain and Hail, so a keyframe here always steps rather than
    // blends - see ss_atmoenv_lerp<std::string>.
    SSAtmoEnvKeyframed<std::string> mPrecipitationOverride{std::string()};

    LLSD asLLSD() const;
    // from_version is the schema version the LLSD was written by, so
    // pre-phase keyframe times can be migrated - see SSAtmoEnvTrack::fromLLSD.
    bool fromLLSD(const LLSD& sd, F64 time_scale = 1.0);
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
    bool fromLLSD(const LLSD& sd, F64 time_scale = 1.0);
};

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

    // Index into the track's mBodies this one orbits - -1 for a root Sun,
    // which orbits nothing (it's "the centre of the universe" for this
    // system, per the design doc, since there's no mechanism to place one
    // anywhere else). A Planet's parent is a Sun; a Moon's parent is a
    // Planet - the resolver does not re-validate the hierarchy shape
    // beyond following mParentIndex, so a hand-edited notecard that breaks
    // the Sun->Planet->Moon rule degrades to "orbits whatever index it
    // says" rather than a hard failure.
    S32 mParentIndex = -1;

    // Physical size and mass. Diameter drives apparent angular size once a
    // distance is resolved; mass only matters for a bound pair's
    // barycenter (below).
    F32 mDiameterM = 1.0e7f; // metres; Earth-ish default
    F32 mMassRelative = 1.f; // arbitrary units, only ratios between a bound pair matter

    // The authored, fixed orbital position: distance from parent (or from
    // a bound pair's shared barycenter - see mBoundPartnerIndex), how far
    // around the orbit (0-360, "phase"/true-anomaly analogue), and how
    // tilted that orbit's plane is relative to the parent's own reference
    // plane. Eccentricity is deferred per the design doc.
    F32 mOrbitalRadius = 1.0e8f; // arbitrary distance units - see mSunPlanetScale/mPlanetMoonScale
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
    F32 mAxialTiltDeg = 0.f;
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

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// A track's volumetric storm-cloud tunables - the "Volumetric Field"
// sub-tab. Separate from the legacy Windlight cloud layer (now cirrus-only,
// see doc/atmo_magic_environment.md), which needs none of this. Actual
// per-frame coverage/density/churn are derived from these plus the weather
// cube's moisture/convection by SSAtmoEnvCloudFieldResolver (phase 7); this
// struct is only the artist's tunable baseline.
struct SSAtmoEnvCloudField
{
    // Metres. The band this track's storm clouds occupy at Convection 0 -
    // Stable phase is "flat, low to ground" per the design doc's convection
    // table; height climbs from here as convection rises.
    F32 mBaseHeightM = 800.f;
    F32 mBaseThicknessM = 300.f;

    // Multiplies the derived coverage fraction - an artistic override for
    // "this track's storms are always more/less widespread than the cube
    // alone would suggest", independent of moisture.
    F32 mCoverageScale = 1.f;

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

    // Atmosphere & Lighting, Clouds (+ Volumetric Field) - not yet real
    // fields, atmosphere for phase-6-adjacent reasons (it leans on whatever
    // rendering the planetary bodies end up needing) and the volumetric
    // cloud field's own tunables below being deliberately kept separate
    // from the legacy cirrus-layer settings this LLSD still round-trips.
    LLSD mAtmosphere = LLSD::emptyMap();
    SSAtmoEnvCloudField mCloudField;

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
    bool fromLLSD(const LLSD& sd, S32 from_version = SS_ATMOENV_VERSION);
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
