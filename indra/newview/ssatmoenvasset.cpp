/**
 * @file ssatmoenvasset.cpp
 * @brief Atmo Magic unified environment asset: LLSD round-trip.
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

#include "llviewerprecompiledheaders.h"

#include "ssatmoenvasset.h"

#include "llagent.h"
#include "llviewerregion.h"

#include <algorithm>
#include <cmath>
#include <ctime>

// <SS:Nexii> Atmo Magic: unified environment asset

//-----------------------------------------------------------------------------
// SSAtmoEnvWeather
//-----------------------------------------------------------------------------

LLSD SSAtmoEnvWeather::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["moisture"]      = mMoisture.asLLSD();
    sd["convection"]    = mConvection.asLLSD();
    sd["temperature_c"] = mTemperatureC.asLLSD();
    sd["wind_heading"]  = mWindHeading.asLLSD();
    sd["wind_speed"]    = mWindSpeed.asLLSD();

    sd["gust_auto"] = mGustAuto;
    if (!mGustAuto)
    {
        sd["gust_depth"]  = (LLSD::Real)mGustDepth;
        sd["gust_length"] = (LLSD::Real)mGustLength;
        sd["gust_veer"]   = (LLSD::Real)mGustVeer;
    }

    sd["lightning_auto"] = mLightningAuto;
    if (!mLightningAuto)
    {
        sd["lightning_intensity"] = (LLSD::Real)mLightningIntensity;
    }

    sd["precipitation_override"] = mPrecipitationOverride.asLLSD();

    return sd;
}

bool SSAtmoEnvWeather::fromLLSD(const LLSD& sd, F64 time_scale)
{
    if (!sd.isMap()) return false;

    if (sd.has("moisture"))      mMoisture.fromLLSD(sd["moisture"], 0.f, time_scale);
    if (sd.has("convection"))    mConvection.fromLLSD(sd["convection"], 0.f, time_scale);
    if (sd.has("temperature_c")) mTemperatureC.fromLLSD(sd["temperature_c"], 15.f, time_scale);
    if (sd.has("wind_heading"))  mWindHeading.fromLLSD(sd["wind_heading"], 0.f, time_scale);
    if (sd.has("wind_speed"))    mWindSpeed.fromLLSD(sd["wind_speed"], 0.f, time_scale);

    mGustAuto = sd.has("gust_auto") ? sd["gust_auto"].asBoolean() : true;
    if (sd.has("gust_depth"))  mGustDepth  = (F32)sd["gust_depth"].asReal();
    if (sd.has("gust_length")) mGustLength = (F32)sd["gust_length"].asReal();
    if (sd.has("gust_veer"))   mGustVeer   = (F32)sd["gust_veer"].asReal();

    mLightningAuto = sd.has("lightning_auto") ? sd["lightning_auto"].asBoolean() : true;
    if (sd.has("lightning_intensity")) mLightningIntensity = (F32)sd["lightning_intensity"].asReal();

    if (sd.has("precipitation_override")) mPrecipitationOverride.fromLLSD(sd["precipitation_override"], std::string(), time_scale);

    return true;
}

//-----------------------------------------------------------------------------
// SSAtmoEnvWater
//-----------------------------------------------------------------------------

LLSD SSAtmoEnvWater::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["enabled"] = mEnabled;
    sd["height"]  = mHeight.asLLSD();

    sd["fog_color"]           = mFogColor.asLLSD();
    sd["fog_density"]         = mFogDensity.asLLSD();
    sd["underwater_modifier"] = mUnderwaterModifier.asLLSD();

    sd["fresnel_scale"]  = mFresnelScale.asLLSD();
    sd["fresnel_offset"] = mFresnelOffset.asLLSD();

    sd["normal_map"]       = mNormalMap.asLLSD();
    sd["large_wave_speed"] = mLargeWaveSpeed.asLLSD();
    sd["small_wave_speed"] = mSmallWaveSpeed.asLLSD();

    sd["normal_scale_x"] = mNormalScaleX.asLLSD();
    sd["normal_scale_y"] = mNormalScaleY.asLLSD();
    sd["normal_scale_z"] = mNormalScaleZ.asLLSD();

    sd["refraction_scale_above"] = mRefractionScaleAbove.asLLSD();
    sd["refraction_scale_below"] = mRefractionScaleBelow.asLLSD();
    sd["blur_multiplier"]        = mBlurMultiplier.asLLSD();
    return sd;
}

bool SSAtmoEnvWater::fromLLSD(const LLSD& sd, F64 time_scale)
{
    if (!sd.isMap()) return false;

    // Every field keeps whatever its constructor default was when the
    // notecard doesn't mention it - a hand-written notecard that only sets
    // height and fog colour is valid, and everything else stays at the
    // sensible default rather than becoming zero.
    const SSAtmoEnvWater def;

    mEnabled = sd.has("enabled") ? sd["enabled"].asBoolean() : false;

    if (sd.has("height")) mHeight.fromLLSD(sd["height"], 0.f, time_scale);

    if (sd.has("fog_color"))           mFogColor.fromLLSD(sd["fog_color"], def.mFogColor.valueAt(0.0), time_scale);
    if (sd.has("fog_density"))         mFogDensity.fromLLSD(sd["fog_density"], def.mFogDensity.valueAt(0.0), time_scale);
    if (sd.has("underwater_modifier")) mUnderwaterModifier.fromLLSD(sd["underwater_modifier"], def.mUnderwaterModifier.valueAt(0.0), time_scale);

    if (sd.has("fresnel_scale"))  mFresnelScale.fromLLSD(sd["fresnel_scale"], def.mFresnelScale.valueAt(0.0), time_scale);
    if (sd.has("fresnel_offset")) mFresnelOffset.fromLLSD(sd["fresnel_offset"], def.mFresnelOffset.valueAt(0.0), time_scale);

    if (sd.has("normal_map"))       mNormalMap.fromLLSD(sd["normal_map"], LLUUID::null, time_scale);
    if (sd.has("large_wave_speed")) mLargeWaveSpeed.fromLLSD(sd["large_wave_speed"], def.mLargeWaveSpeed.valueAt(0.0), time_scale);
    if (sd.has("small_wave_speed")) mSmallWaveSpeed.fromLLSD(sd["small_wave_speed"], def.mSmallWaveSpeed.valueAt(0.0), time_scale);

    if (sd.has("normal_scale_x")) mNormalScaleX.fromLLSD(sd["normal_scale_x"], def.mNormalScaleX.valueAt(0.0), time_scale);
    if (sd.has("normal_scale_y")) mNormalScaleY.fromLLSD(sd["normal_scale_y"], def.mNormalScaleY.valueAt(0.0), time_scale);
    if (sd.has("normal_scale_z")) mNormalScaleZ.fromLLSD(sd["normal_scale_z"], def.mNormalScaleZ.valueAt(0.0), time_scale);

    if (sd.has("refraction_scale_above")) mRefractionScaleAbove.fromLLSD(sd["refraction_scale_above"], def.mRefractionScaleAbove.valueAt(0.0), time_scale);
    if (sd.has("refraction_scale_below")) mRefractionScaleBelow.fromLLSD(sd["refraction_scale_below"], def.mRefractionScaleBelow.valueAt(0.0), time_scale);
    if (sd.has("blur_multiplier"))        mBlurMultiplier.fromLLSD(sd["blur_multiplier"], def.mBlurMultiplier.valueAt(0.0), time_scale);
    return true;
}

//-----------------------------------------------------------------------------
// SSAtmoEnvCelestialBody
//-----------------------------------------------------------------------------

LLSD SSAtmoEnvCelestialBody::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["kind"] = (S32)mKind;
    sd["name"] = mName;
    sd["parent_index"] = mParentIndex;

    sd["diameter_m"] = (LLSD::Real)mDiameterM;
    sd["mass_relative"] = (LLSD::Real)mMassRelative;

    sd["orbital_radius"] = (LLSD::Real)mOrbitalRadius;
    sd["orbital_inclination_deg"] = (LLSD::Real)mOrbitalInclinationDeg;
    sd["orbital_phase_deg"] = (LLSD::Real)mOrbitalPhaseDeg;
    sd["orbital_period_seconds"] = mOrbitalPeriodSeconds;
    sd["rotation_period_seconds"] = mRotationPeriodSeconds;

    sd["axial_tilt_deg"] = (LLSD::Real)mAxialTiltDeg;
    sd["spin_period_seconds"] = mSpinPeriodSeconds;

    sd["is_home"] = mIsHome;
    sd["is_light_emitter"] = mIsLightEmitter;
    sd["bound_partner_index"] = mBoundPartnerIndex;

    if (mCustomTexture.notNull()) sd["custom_texture"] = mCustomTexture;

    sd["has_ring"] = mHasRing;
    if (mHasRing)
    {
        sd["ring_inner_radius"] = (LLSD::Real)mRingInnerRadius;
        sd["ring_outer_radius"] = (LLSD::Real)mRingOuterRadius;
        if (mRingTexture.notNull()) sd["ring_texture"] = mRingTexture;
    }

    return sd;
}

bool SSAtmoEnvCelestialBody::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    mKind = (EKind)llclamp(sd.has("kind") ? sd["kind"].asInteger() : (S32)PLANET, (S32)SUN, (S32)MOON);
    mName = sd.has("name") ? sd["name"].asString() : std::string("Body");
    mParentIndex = sd.has("parent_index") ? sd["parent_index"].asInteger() : -1;

    if (sd.has("diameter_m")) mDiameterM = llmax(0.f, (F32)sd["diameter_m"].asReal());
    if (sd.has("mass_relative")) mMassRelative = llmax(0.f, (F32)sd["mass_relative"].asReal());

    if (sd.has("orbital_radius")) mOrbitalRadius = llmax(0.f, (F32)sd["orbital_radius"].asReal());
    if (sd.has("orbital_inclination_deg")) mOrbitalInclinationDeg = (F32)sd["orbital_inclination_deg"].asReal();
    if (sd.has("orbital_phase_deg")) mOrbitalPhaseDeg = (F32)sd["orbital_phase_deg"].asReal();
    mOrbitalPeriodSeconds = sd.has("orbital_period_seconds") ? sd["orbital_period_seconds"].asReal() : 0.0;
    mRotationPeriodSeconds = sd.has("rotation_period_seconds") ? sd["rotation_period_seconds"].asReal() : 0.0;

    if (sd.has("axial_tilt_deg")) mAxialTiltDeg = (F32)sd["axial_tilt_deg"].asReal();
    mSpinPeriodSeconds = sd.has("spin_period_seconds") ? sd["spin_period_seconds"].asReal() : 0.0;

    mIsHome = sd.has("is_home") ? sd["is_home"].asBoolean() : false;
    mIsLightEmitter = sd.has("is_light_emitter") ? sd["is_light_emitter"].asBoolean() : false;
    mBoundPartnerIndex = sd.has("bound_partner_index") ? sd["bound_partner_index"].asInteger() : -1;

    mCustomTexture = sd.has("custom_texture") ? sd["custom_texture"].asUUID() : LLUUID::null;

    mHasRing = sd.has("has_ring") ? sd["has_ring"].asBoolean() : false;
    if (sd.has("ring_inner_radius")) mRingInnerRadius = (F32)sd["ring_inner_radius"].asReal();
    if (sd.has("ring_outer_radius")) mRingOuterRadius = (F32)sd["ring_outer_radius"].asReal();
    mRingTexture = sd.has("ring_texture") ? sd["ring_texture"].asUUID() : LLUUID::null;

    return true;
}

//-----------------------------------------------------------------------------
// SSAtmoEnvPlanetary
//-----------------------------------------------------------------------------

S32 SSAtmoEnvPlanetary::homeBodyIndex() const
{
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        if (mBodies[i].mIsHome) return (S32)i;
    }
    return -1;
}

bool SSAtmoEnvPlanetary::setHomeBody(S32 index)
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;

    // Exactly one home, ever - clearing everywhere else first rather than
    // trusting the caller to have already done so.
    for (SSAtmoEnvCelestialBody& body : mBodies)
    {
        body.mIsHome = false;
    }
    mBodies[index].mIsHome = true;
    // A body flagged home cannot also be a light emitter - you don't light
    // yourself.
    mBodies[index].mIsLightEmitter = false;
    return true;
}

std::vector<S32> SSAtmoEnvPlanetary::lightEmitterIndices() const
{
    std::vector<S32> out;
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        if (mBodies[i].mIsLightEmitter) out.push_back((S32)i);
    }
    return out;
}

bool SSAtmoEnvPlanetary::canSetLightEmitter(S32 index) const
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;
    if (mBodies[index].mIsHome) return false;
    if (mBodies[index].mIsLightEmitter) return true; // already on - toggling off is always fine

    return lightEmitterIndices().size() < 2;
}

LLSD SSAtmoEnvPlanetary::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["sun_planet_scale"] = (LLSD::Real)mSunPlanetScale;
    sd["planet_moon_scale"] = (LLSD::Real)mPlanetMoonScale;

    LLSD bodies = LLSD::emptyArray();
    for (const SSAtmoEnvCelestialBody& body : mBodies)
    {
        bodies.append(body.asLLSD());
    }
    sd["bodies"] = bodies;
    return sd;
}

bool SSAtmoEnvPlanetary::fromLLSD(const LLSD& sd)
{
    mBodies.clear();
    if (!sd.isMap()) return false;

    mSunPlanetScale = sd.has("sun_planet_scale") ? llmax(0.001f, (F32)sd["sun_planet_scale"].asReal()) : 1.f;
    mPlanetMoonScale = sd.has("planet_moon_scale") ? llmax(0.001f, (F32)sd["planet_moon_scale"].asReal()) : 1.f;

    if (sd.has("bodies") && sd["bodies"].isArray())
    {
        for (const LLSD& entry : llsd::inArray(sd["bodies"]))
        {
            SSAtmoEnvCelestialBody body;
            body.fromLLSD(entry);
            mBodies.push_back(body);
        }
    }

    // Enforce the invariants on read too, not just through the setters -
    // a hand-edited notecard that names two homes or three emitters should
    // degrade to "first one wins", not carry the contradiction forward.
    bool have_home = false;
    S32 emitters = 0;
    for (SSAtmoEnvCelestialBody& body : mBodies)
    {
        if (body.mIsHome)
        {
            if (have_home) body.mIsHome = false;
            else have_home = true;
        }
        if (body.mIsLightEmitter)
        {
            if (body.mIsHome || emitters >= 2) body.mIsLightEmitter = false;
            else ++emitters;
        }
    }

    return true;
}

//-----------------------------------------------------------------------------
// SSAtmoEnvCloudField
//-----------------------------------------------------------------------------

LLSD SSAtmoEnvCloudField::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["base_height_m"] = (LLSD::Real)mBaseHeightM;
    sd["base_thickness_m"] = (LLSD::Real)mBaseThicknessM;
    sd["coverage_scale"] = (LLSD::Real)mCoverageScale;
    return sd;
}

bool SSAtmoEnvCloudField::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;
    if (sd.has("base_height_m")) mBaseHeightM = (F32)sd["base_height_m"].asReal();
    if (sd.has("base_thickness_m")) mBaseThicknessM = llmax(0.f, (F32)sd["base_thickness_m"].asReal());
    if (sd.has("coverage_scale")) mCoverageScale = llmax(0.f, (F32)sd["coverage_scale"].asReal());
    return true;
}

//-----------------------------------------------------------------------------
// SSAtmoEnvTrack
//-----------------------------------------------------------------------------

LLSD SSAtmoEnvTrack::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["name"]       = mName;
    sd["floor_z"]    = (LLSD::Real)mFloorZ;
    sd["transition_buffer"] = (LLSD::Real)mTransitionBuffer;

    sd["day_length_seconds"] = (LLSD::Real)mDayLengthSeconds;
    sd["day_offset_seconds"] = (LLSD::Real)mDayOffsetSeconds;

    sd["water"]      = mWater.asLLSD();
    sd["weather"]    = mWeather.asLLSD();
    sd["planetary"]  = mPlanetary.asLLSD();
    sd["cloud_field"] = mCloudField.asLLSD();

    // Opaque until its own phase - stored as-is, even empty, so a reader
    // that only knows this phase's schema never has to special-case a
    // missing key once real content starts appearing here.
    sd["atmosphere"] = mAtmosphere;
    return sd;
}

bool SSAtmoEnvTrack::fromLLSD(const LLSD& sd, S32 from_version)
{
    if (!sd.isMap()) return false;

    if (sd.has("name")) mName = sd["name"].asString();
    if (sd.has("floor_z")) mFloorZ = (F32)sd["floor_z"].asReal();
    // ceiling_z was a stored field in the very first schema revision and is
    // now derived (see SSAtmoEnvAsset::trackCeilingZ) - an older notecard
    // still carrying one is simply ignored rather than rejected.
    if (sd.has("transition_buffer")) mTransitionBuffer = llmax(0.f, (F32)sd["transition_buffer"].asReal());

    mDayLengthSeconds = sd.has("day_length_seconds")
        ? sd["day_length_seconds"].asReal() : (4.0 * 60.0 * 60.0);
    mDayOffsetSeconds = sd.has("day_offset_seconds") ? sd["day_offset_seconds"].asReal() : 0.0;

    // Schema 1 stored keyframe times as absolute seconds into this track's
    // own day. Dividing by that day length turns them into the phase this
    // build expects, which is why the migration has to happen here: this is
    // the only scope that knows both the version and the day length. Note
    // mDayLengthSeconds is read above, so it is already the value the old
    // times were authored against.
    const F64 time_scale = (from_version < 2 && mDayLengthSeconds > 0.0)
        ? (1.0 / mDayLengthSeconds) : 1.0;

    if (sd.has("water"))       mWater.fromLLSD(sd["water"], time_scale);
    if (sd.has("weather"))     mWeather.fromLLSD(sd["weather"], time_scale);
    if (sd.has("planetary"))   mPlanetary.fromLLSD(sd["planetary"]);
    if (sd.has("cloud_field")) mCloudField.fromLLSD(sd["cloud_field"]);

    mAtmosphere = sd.has("atmosphere") ? sd["atmosphere"] : LLSD::emptyMap();

    return true;
}

F64 SSAtmoEnvTrack::currentDayCyclePhase() const
{
    if (mDayLengthSeconds <= 0.0) return 0.0;

    // Plain Unix epoch seconds: a universal wall clock every viewer already
    // agrees on without needing to ask anything else for the time, which is
    // the whole point - two people who load the same notecard minutes apart
    // should land on the same phase.
    const F64 utc_now = (F64)time(nullptr);
    F64 t = fmod(utc_now - mDayOffsetSeconds, mDayLengthSeconds);
    if (t < 0.0) t += mDayLengthSeconds;
    return t / mDayLengthSeconds;
}

//-----------------------------------------------------------------------------
// SSAtmoEnvAsset
//-----------------------------------------------------------------------------

// static
SSAtmoEnvAsset SSAtmoEnvAsset::makeDefault()
{
    SSAtmoEnvAsset asset;
    asset.mName = "New Atmo Environment";

    SSAtmoEnvTrack ground;
    ground.mName = "Ground";
    ground.mFloorZ = 0.f;
    ground.mDayLengthSeconds = 4.0 * 60.0 * 60.0;
    ground.mDayOffsetSeconds = 0.0;
    // Calm, clear default: moisture/convection both 0 is "bone dry, clear
    // skies" per the design doc's Weather tab.
    ground.mWeather = SSAtmoEnvWeather();

    // Water on by default at the ground track, at whatever height the
    // current region's water actually sits - same fallback SSAtmoMagic's
    // own voidWaterHeight() uses (20m, SL's default) when there's no live
    // region to ask. A freshly created ground track should look like the
    // ordinary sea-level world it's standing in until an author changes
    // it, not come up with an invisible/zero-height water plane no one
    // asked for.
    ground.mWater.mEnabled = true;
    {
        LLViewerRegion* region = gAgent.getRegion();
        ground.mWater.mHeight = SSAtmoEnvKeyframed<F32>(region ? region->getWaterHeight() : 20.f);
    }

    // Default new asset per the design doc: an Earth-sized planet as home,
    // orbiting a 1-solar-mass sun. Neither has an authored orbital radius
    // that means anything yet (the sun is the hierarchy root; the planet's
    // radius/phase are placeholders an author is expected to actually set),
    // but the sun is flagged as the one light emitter so a freshly created
    // environment already has *something* lighting it rather than a black
    // sky by default.
    SSAtmoEnvCelestialBody sun;
    sun.mKind = SSAtmoEnvCelestialBody::SUN;
    sun.mName = "Sun";
    sun.mParentIndex = -1;
    sun.mDiameterM = 1.392e9f;   // Sol, for scale
    sun.mMassRelative = 1.f;     // 1 solar mass
    sun.mIsLightEmitter = true;
    ground.mPlanetary.mBodies.push_back(sun);

    SSAtmoEnvCelestialBody home;
    home.mKind = SSAtmoEnvCelestialBody::PLANET;
    home.mName = "Home";
    home.mParentIndex = 0; // the sun above
    home.mDiameterM = 1.2742e7f; // Earth, for scale
    home.mOrbitalRadius = 1.496e11f; // 1 AU, for scale
    home.mIsHome = true;
    ground.mPlanetary.mBodies.push_back(home);

    asset.mTracks.push_back(ground);
    return asset;
}

bool SSAtmoEnvAsset::addTrack()
{
    if ((S32)mTracks.size() >= SS_ATMOENV_MAX_TRACKS) return false;

    SSAtmoEnvTrack track;
    // A new track starts above whatever is currently highest, far enough up
    // that its marker on the floater's rail can't collide with the one below
    // it (the rail enforces a minimum separation of its own and would
    // otherwise refuse to place the marker), and clamped so it can never
    // land on or above the region ceiling where it would have no band at
    // all. The author can drag it anywhere afterwards.
    F32 highest = 0.f;
    for (const SSAtmoEnvTrack& t : mTracks) highest = llmax(highest, t.mFloorZ);

    const F32 SPACING = SS_ATMOENV_MIN_TRACK_FLOOR; // == the rail's enforced minimum
    track.mFloorZ = llclamp(highest + SPACING,
                            SS_ATMOENV_MIN_TRACK_FLOOR,
                            SS_ATMOENV_REGION_CEILING - SPACING);

    track.mName = nextDefaultTrackName();

    mTracks.push_back(track);
    sortTracksByAltitude();
    return true;
}

bool SSAtmoEnvAsset::removeTrack(S32 index)
{
    // Index 0 is the mandatory ground track - never removable.
    if (index <= 0 || index >= (S32)mTracks.size()) return false;
    mTracks.erase(mTracks.begin() + index);
    // Names are the author's, so nothing is rewritten here - the survivors
    // keep whatever they were called. Only the ordering is maintained.
    sortTracksByAltitude();
    return true;
}

std::string SSAtmoEnvAsset::nextDefaultTrackName() const
{
    // Lowest "Track N" not already taken, so adding a track after deleting
    // one doesn't hand out a name another track is still using.
    for (S32 n = 1; n <= SS_ATMOENV_MAX_TRACKS; ++n)
    {
        const std::string candidate = llformat("Track %d", n);

        bool taken = false;
        for (const SSAtmoEnvTrack& t : mTracks)
        {
            if (t.mName == candidate) { taken = true; break; }
        }
        if (!taken) return candidate;
    }
    return "Track";
}

S32 SSAtmoEnvAsset::sortTracksByAltitude(S32 follow_index)
{
    if (mTracks.size() < 2) return follow_index;

    // Ground stays at index 0 - it is the catch-all band rather than a peer
    // with a floor worth sorting on, and callers (and the notecard format)
    // treat index 0 as ground unconditionally.
    const SSAtmoEnvTrack* follow = (follow_index >= 0 && follow_index < (S32)mTracks.size())
        ? &mTracks[follow_index] : nullptr;
    const std::string follow_name = follow ? follow->mName : std::string();
    const bool follow_is_ground = (follow_index == 0);

    std::stable_sort(mTracks.begin() + 1, mTracks.end(),
        [](const SSAtmoEnvTrack& a, const SSAtmoEnvTrack& b) { return a.mFloorZ < b.mFloorZ; });

    if (follow_index < 0 || follow_is_ground) return follow_index;

    // Track the caller's selection across the reorder by name. Names are
    // authored and can in principle collide (nothing stops two tracks both
    // being called "Sky"), so this is a best effort - a collision just
    // leaves the selection on the first match, which is still a real track.
    for (S32 i = 1; i < (S32)mTracks.size(); ++i)
    {
        if (mTracks[i].mName == follow_name) return i;
    }
    return follow_index;
}

F32 SSAtmoEnvAsset::trackCeilingZ(S32 index) const
{
    if (index < 0 || index >= (S32)mTracks.size()) return SS_ATMOENV_REGION_CEILING;

    const F32 own_floor = mTracks[index].mFloorZ;

    F32 ceiling = SS_ATMOENV_REGION_CEILING;
    for (S32 i = 0; i < (S32)mTracks.size(); ++i)
    {
        if (i == index) continue;
        const F32 other = mTracks[i].mFloorZ;
        // Strictly above: two tracks sharing a floor exactly is degenerate
        // either way, and treating one as the other's ceiling would give it
        // a zero-height band rather than just letting the lower-indexed one
        // win in trackContaining().
        if (other > own_floor && other < ceiling) ceiling = other;
    }
    return ceiling;
}

bool SSAtmoEnvAsset::visibleWaterHeight(F32& out_height) const
{
    bool found = false;
    F32 lowest = FLT_MAX;
    for (const SSAtmoEnvTrack& track : mTracks)
    {
        if (!track.mWater.mEnabled) continue;

        // Water height is a tide now, so "lowest" is a question about a
        // particular instant. Each track's own day cycle decides what that
        // instant is for it - the same evaluation a live (non-previewing)
        // read of any other field on that track would do.
        const F32 height = track.mWater.mHeight.valueAt(track.currentDayCyclePhase());

        if (!found || height < lowest)
        {
            lowest = height;
            found = true;
        }
    }
    if (found) out_height = lowest;
    return found;
}

LLSD SSAtmoEnvAsset::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["version"] = SS_ATMOENV_VERSION;
    sd["name"] = mName;

    LLSD tracks = LLSD::emptyArray();
    for (const SSAtmoEnvTrack& track : mTracks)
    {
        tracks.append(track.asLLSD());
    }
    sd["tracks"] = tracks;

    return sd;
}

bool SSAtmoEnvAsset::fromLLSD(const LLSD& sd, std::string& out_error)
{
    if (!sd.isMap())
    {
        out_error = "not an LLSD map";
        *this = makeDefault();
        return false;
    }

    const S32 version = sd.has("version") ? sd["version"].asInteger() : 0;
    if (version <= 0)
    {
        out_error = "missing or invalid version";
        *this = makeDefault();
        return false;
    }
    if (version > SS_ATMOENV_VERSION)
    {
        out_error = llformat("asset version %d is newer than this viewer understands (%d)",
                              version, SS_ATMOENV_VERSION);
        *this = makeDefault();
        return false;
    }

    if (!sd.has("tracks") || !sd["tracks"].isArray() || sd["tracks"].size() < 1)
    {
        out_error = "no tracks defined";
        *this = makeDefault();
        return false;
    }

    SSAtmoEnvAsset parsed;
    parsed.mName = sd.has("name") ? sd["name"].asString() : std::string("Untitled");

    const LLSD& tracks_sd = sd["tracks"];
    const S32 count = llclamp((S32)tracks_sd.size(), SS_ATMOENV_MIN_TRACKS, SS_ATMOENV_MAX_TRACKS);
    for (S32 i = 0; i < count; ++i)
    {
        SSAtmoEnvTrack track;
        track.fromLLSD(tracks_sd[i], version);
        parsed.mTracks.push_back(track);
    }

    if (parsed.mTracks.empty())
    {
        out_error = "no tracks survived parsing";
        *this = makeDefault();
        return false;
    }

    // Names come from the notecard as authored - see nextDefaultTrackName()
    // for why nothing is rewritten here. Ordering is normalised though, so
    // a hand-edited notecard listing its tracks out of altitude order still
    // reads bottom-to-top in the floater.
    parsed.sortTracksByAltitude();

    *this = parsed;
    return true;
}

// </SS:Nexii>
