/**
 * @file ssatmov3asset.cpp
 * @brief Atmo Magic v3 unified environment asset: LLSD round-trip.
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

#include "ssatmov3asset.h"

#include "llagent.h"
#include "llviewerregion.h"

#include <cmath>
#include <ctime>

// <SS:Nexii> Atmo Magic v3: unified environment asset

//-----------------------------------------------------------------------------
// SSAtmoV3Weather
//-----------------------------------------------------------------------------

LLSD SSAtmoV3Weather::asLLSD() const
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

bool SSAtmoV3Weather::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    if (sd.has("moisture"))      mMoisture.fromLLSD(sd["moisture"], 0.f);
    if (sd.has("convection"))    mConvection.fromLLSD(sd["convection"], 0.f);
    if (sd.has("temperature_c")) mTemperatureC.fromLLSD(sd["temperature_c"], 15.f);
    if (sd.has("wind_heading"))  mWindHeading.fromLLSD(sd["wind_heading"], 0.f);
    if (sd.has("wind_speed"))    mWindSpeed.fromLLSD(sd["wind_speed"], 0.f);

    mGustAuto = sd.has("gust_auto") ? sd["gust_auto"].asBoolean() : true;
    if (sd.has("gust_depth"))  mGustDepth  = (F32)sd["gust_depth"].asReal();
    if (sd.has("gust_length")) mGustLength = (F32)sd["gust_length"].asReal();
    if (sd.has("gust_veer"))   mGustVeer   = (F32)sd["gust_veer"].asReal();

    mLightningAuto = sd.has("lightning_auto") ? sd["lightning_auto"].asBoolean() : true;
    if (sd.has("lightning_intensity")) mLightningIntensity = (F32)sd["lightning_intensity"].asReal();

    if (sd.has("precipitation_override")) mPrecipitationOverride.fromLLSD(sd["precipitation_override"], std::string());

    return true;
}

//-----------------------------------------------------------------------------
// SSAtmoV3Water
//-----------------------------------------------------------------------------

LLSD SSAtmoV3Water::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["enabled"] = mEnabled;
    sd["height"]  = (LLSD::Real)mHeight;
    return sd;
}

bool SSAtmoV3Water::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;
    mEnabled = sd.has("enabled") ? sd["enabled"].asBoolean() : false;
    if (sd.has("height")) mHeight = (F32)sd["height"].asReal();
    return true;
}

//-----------------------------------------------------------------------------
// SSAtmoV3CelestialBody
//-----------------------------------------------------------------------------

LLSD SSAtmoV3CelestialBody::asLLSD() const
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

bool SSAtmoV3CelestialBody::fromLLSD(const LLSD& sd)
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
// SSAtmoV3Planetary
//-----------------------------------------------------------------------------

S32 SSAtmoV3Planetary::homeBodyIndex() const
{
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        if (mBodies[i].mIsHome) return (S32)i;
    }
    return -1;
}

bool SSAtmoV3Planetary::setHomeBody(S32 index)
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;

    // Exactly one home, ever - clearing everywhere else first rather than
    // trusting the caller to have already done so.
    for (SSAtmoV3CelestialBody& body : mBodies)
    {
        body.mIsHome = false;
    }
    mBodies[index].mIsHome = true;
    // A body flagged home cannot also be a light emitter - you don't light
    // yourself.
    mBodies[index].mIsLightEmitter = false;
    return true;
}

std::vector<S32> SSAtmoV3Planetary::lightEmitterIndices() const
{
    std::vector<S32> out;
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        if (mBodies[i].mIsLightEmitter) out.push_back((S32)i);
    }
    return out;
}

bool SSAtmoV3Planetary::canSetLightEmitter(S32 index) const
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;
    if (mBodies[index].mIsHome) return false;
    if (mBodies[index].mIsLightEmitter) return true; // already on - toggling off is always fine

    return lightEmitterIndices().size() < 2;
}

LLSD SSAtmoV3Planetary::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["sun_planet_scale"] = (LLSD::Real)mSunPlanetScale;
    sd["planet_moon_scale"] = (LLSD::Real)mPlanetMoonScale;

    LLSD bodies = LLSD::emptyArray();
    for (const SSAtmoV3CelestialBody& body : mBodies)
    {
        bodies.append(body.asLLSD());
    }
    sd["bodies"] = bodies;
    return sd;
}

bool SSAtmoV3Planetary::fromLLSD(const LLSD& sd)
{
    mBodies.clear();
    if (!sd.isMap()) return false;

    mSunPlanetScale = sd.has("sun_planet_scale") ? llmax(0.001f, (F32)sd["sun_planet_scale"].asReal()) : 1.f;
    mPlanetMoonScale = sd.has("planet_moon_scale") ? llmax(0.001f, (F32)sd["planet_moon_scale"].asReal()) : 1.f;

    if (sd.has("bodies") && sd["bodies"].isArray())
    {
        for (const LLSD& entry : llsd::inArray(sd["bodies"]))
        {
            SSAtmoV3CelestialBody body;
            body.fromLLSD(entry);
            mBodies.push_back(body);
        }
    }

    // Enforce the invariants on read too, not just through the setters -
    // a hand-edited notecard that names two homes or three emitters should
    // degrade to "first one wins", not carry the contradiction forward.
    bool have_home = false;
    S32 emitters = 0;
    for (SSAtmoV3CelestialBody& body : mBodies)
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
// SSAtmoV3CloudField
//-----------------------------------------------------------------------------

LLSD SSAtmoV3CloudField::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["base_height_m"] = (LLSD::Real)mBaseHeightM;
    sd["base_thickness_m"] = (LLSD::Real)mBaseThicknessM;
    sd["coverage_scale"] = (LLSD::Real)mCoverageScale;
    return sd;
}

bool SSAtmoV3CloudField::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;
    if (sd.has("base_height_m")) mBaseHeightM = (F32)sd["base_height_m"].asReal();
    if (sd.has("base_thickness_m")) mBaseThicknessM = llmax(0.f, (F32)sd["base_thickness_m"].asReal());
    if (sd.has("coverage_scale")) mCoverageScale = llmax(0.f, (F32)sd["coverage_scale"].asReal());
    return true;
}

//-----------------------------------------------------------------------------
// SSAtmoV3Track
//-----------------------------------------------------------------------------

LLSD SSAtmoV3Track::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["name"]       = mName;
    sd["floor_z"]    = (LLSD::Real)mFloorZ;
    // FLT_MAX does not round-trip through LLSD::Real cleanly on every
    // platform; the ground track (the only one that uses the open-ended
    // default) simply omits ceiling_z rather than writing a sentinel.
    if (mCeilingZ < FLT_MAX)
    {
        sd["ceiling_z"] = (LLSD::Real)mCeilingZ;
    }
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

bool SSAtmoV3Track::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    if (sd.has("name")) mName = sd["name"].asString();
    if (sd.has("floor_z")) mFloorZ = (F32)sd["floor_z"].asReal();
    mCeilingZ = sd.has("ceiling_z") ? (F32)sd["ceiling_z"].asReal() : FLT_MAX;
    if (sd.has("transition_buffer")) mTransitionBuffer = llmax(0.f, (F32)sd["transition_buffer"].asReal());

    mDayLengthSeconds = sd.has("day_length_seconds")
        ? sd["day_length_seconds"].asReal() : (4.0 * 60.0 * 60.0);
    mDayOffsetSeconds = sd.has("day_offset_seconds") ? sd["day_offset_seconds"].asReal() : 0.0;

    if (sd.has("water"))       mWater.fromLLSD(sd["water"]);
    if (sd.has("weather"))     mWeather.fromLLSD(sd["weather"]);
    if (sd.has("planetary"))   mPlanetary.fromLLSD(sd["planetary"]);
    if (sd.has("cloud_field")) mCloudField.fromLLSD(sd["cloud_field"]);

    mAtmosphere = sd.has("atmosphere") ? sd["atmosphere"] : LLSD::emptyMap();

    return true;
}

F64 SSAtmoV3Track::currentDayCycleTime() const
{
    if (mDayLengthSeconds <= 0.0) return 0.0;

    // Plain Unix epoch seconds: a universal wall clock every viewer already
    // agrees on without needing to ask anything else for the time, which is
    // the whole point - two people who load the same notecard minutes apart
    // should land on the same phase.
    const F64 utc_now = (F64)time(nullptr);
    F64 t = fmod(utc_now - mDayOffsetSeconds, mDayLengthSeconds);
    if (t < 0.0) t += mDayLengthSeconds;
    return t;
}

//-----------------------------------------------------------------------------
// SSAtmoV3Asset
//-----------------------------------------------------------------------------

// static
SSAtmoV3Asset SSAtmoV3Asset::makeDefault()
{
    SSAtmoV3Asset asset;
    asset.mName = "New Atmo Environment";

    SSAtmoV3Track ground;
    ground.mName = "Ground";
    ground.mFloorZ = 0.f;
    ground.mCeilingZ = FLT_MAX;
    ground.mDayLengthSeconds = 4.0 * 60.0 * 60.0;
    ground.mDayOffsetSeconds = 0.0;
    // Calm, clear default: moisture/convection both 0 is "bone dry, clear
    // skies" per the design doc's Weather tab.
    ground.mWeather = SSAtmoV3Weather();

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
        ground.mWater.mHeight = region ? region->getWaterHeight() : 20.f;
    }

    // Default new asset per the design doc: an Earth-sized planet as home,
    // orbiting a 1-solar-mass sun. Neither has an authored orbital radius
    // that means anything yet (the sun is the hierarchy root; the planet's
    // radius/phase are placeholders an author is expected to actually set),
    // but the sun is flagged as the one light emitter so a freshly created
    // environment already has *something* lighting it rather than a black
    // sky by default.
    SSAtmoV3CelestialBody sun;
    sun.mKind = SSAtmoV3CelestialBody::SUN;
    sun.mName = "Sun";
    sun.mParentIndex = -1;
    sun.mDiameterM = 1.392e9f;   // Sol, for scale
    sun.mMassRelative = 1.f;     // 1 solar mass
    sun.mIsLightEmitter = true;
    ground.mPlanetary.mBodies.push_back(sun);

    SSAtmoV3CelestialBody home;
    home.mKind = SSAtmoV3CelestialBody::PLANET;
    home.mName = "Home";
    home.mParentIndex = 0; // the sun above
    home.mDiameterM = 1.2742e7f; // Earth, for scale
    home.mOrbitalRadius = 1.496e11f; // 1 AU, for scale
    home.mIsHome = true;
    ground.mPlanetary.mBodies.push_back(home);

    asset.mTracks.push_back(ground);
    return asset;
}

bool SSAtmoV3Asset::addTrack()
{
    if ((S32)mTracks.size() >= SS_ATMOV3_MAX_TRACKS) return false;

    SSAtmoV3Track track;
    track.mName = llformat("Track %d", (S32)mTracks.size() + 1);
    // New optional tracks default to starting where the previous one's
    // ceiling was, so a freshly-added track doesn't silently overlap or gap
    // against what's already there; the author can still move it anywhere.
    const F32 prev_ceiling = mTracks.empty() ? 0.f : mTracks.back().mCeilingZ;
    track.mFloorZ = (prev_ceiling < FLT_MAX) ? prev_ceiling : 0.f;
    track.mCeilingZ = track.mFloorZ + 1000.f;

    mTracks.push_back(track);
    return true;
}

bool SSAtmoV3Asset::removeTrack(S32 index)
{
    // Index 0 is the mandatory ground track - never removable.
    if (index <= 0 || index >= (S32)mTracks.size()) return false;
    mTracks.erase(mTracks.begin() + index);
    return true;
}

bool SSAtmoV3Asset::visibleWaterHeight(F32& out_height) const
{
    bool found = false;
    F32 lowest = FLT_MAX;
    for (const SSAtmoV3Track& track : mTracks)
    {
        if (!track.mWater.mEnabled) continue;
        if (!found || track.mWater.mHeight < lowest)
        {
            lowest = track.mWater.mHeight;
            found = true;
        }
    }
    if (found) out_height = lowest;
    return found;
}

LLSD SSAtmoV3Asset::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["version"] = SS_ATMOV3_VERSION;
    sd["name"] = mName;

    LLSD tracks = LLSD::emptyArray();
    for (const SSAtmoV3Track& track : mTracks)
    {
        tracks.append(track.asLLSD());
    }
    sd["tracks"] = tracks;

    return sd;
}

bool SSAtmoV3Asset::fromLLSD(const LLSD& sd, std::string& out_error)
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
    if (version > SS_ATMOV3_VERSION)
    {
        out_error = llformat("asset version %d is newer than this viewer understands (%d)",
                              version, SS_ATMOV3_VERSION);
        *this = makeDefault();
        return false;
    }

    if (!sd.has("tracks") || !sd["tracks"].isArray() || sd["tracks"].size() < 1)
    {
        out_error = "no tracks defined";
        *this = makeDefault();
        return false;
    }

    SSAtmoV3Asset parsed;
    parsed.mName = sd.has("name") ? sd["name"].asString() : std::string("Untitled");

    const LLSD& tracks_sd = sd["tracks"];
    const S32 count = llclamp((S32)tracks_sd.size(), SS_ATMOV3_MIN_TRACKS, SS_ATMOV3_MAX_TRACKS);
    for (S32 i = 0; i < count; ++i)
    {
        SSAtmoV3Track track;
        track.fromLLSD(tracks_sd[i]);
        parsed.mTracks.push_back(track);
    }

    if (parsed.mTracks.empty())
    {
        out_error = "no tracks survived parsing";
        *this = makeDefault();
        return false;
    }

    *this = parsed;
    return true;
}

// </SS:Nexii>
