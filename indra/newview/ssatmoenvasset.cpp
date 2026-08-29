/**
 * @file ssatmoenvasset.cpp
 * @brief See ssatmoenvasset.h.
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
#include "llsettingssky.h"
#include "llviewerregion.h"

#include <algorithm>
#include <cmath>
#include <ctime>

// The weather cube out to its notecard document.
LLSD SSAtmoEnvWeather::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["moisture"]      = mMoisture.asLLSD();
    sd["convection"]    = mConvection.asLLSD();
    sd["temperature_c"] = mTemperatureC.asLLSD();
    sd["wind_heading"]  = mWindHeading.asLLSD();
    sd["wind_speed"]    = mWindSpeed.asLLSD();

    sd["gust_auto"]   = mGustAuto;
    sd["gust_depth"]  = mGustDepth.asLLSD();
    sd["gust_length"] = mGustLength.asLLSD();
    sd["gust_veer"]   = mGustVeer.asLLSD();

    sd["lightning_enabled"]   = mLightningEnabled;
    sd["lightning_charge"]    = mLightningCharge;
    sd["lightning_sparks"]    = mLightningSparks;
    sd["lightning_auto"]      = mLightningAuto;
    sd["lightning_intensity"] = mLightningIntensity.asLLSD();
    sd["lightning_color"] = mLightningColor.asLLSD();
    sd["lightning_core_white"] = mLightningCoreWhite.asLLSD();

    sd["precipitation_override"] = mPrecipitationOverride.asLLSD();

    return sd;
}

// The weather cube back from a document, tolerating missing fields.
bool SSAtmoEnvWeather::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    if (sd.has("moisture"))      mMoisture.fromLLSD(sd["moisture"], 0.f);
    if (sd.has("convection"))    mConvection.fromLLSD(sd["convection"], 0.f);
    if (sd.has("temperature_c")) mTemperatureC.fromLLSD(sd["temperature_c"], 15.f);
    if (sd.has("wind_heading"))  mWindHeading.fromLLSD(sd["wind_heading"], 0.f);
    if (sd.has("wind_speed"))    mWindSpeed.fromLLSD(sd["wind_speed"], 0.f);

    mGustAuto = sd.has("gust_auto") ? sd["gust_auto"].asBoolean() : true;
    if (sd.has("gust_depth"))  mGustDepth.fromLLSD(sd["gust_depth"], 0.f);
    if (sd.has("gust_length")) mGustLength.fromLLSD(sd["gust_length"], 140.f);
    if (sd.has("gust_veer"))   mGustVeer.fromLLSD(sd["gust_veer"], 0.f);

    mLightningEnabled = sd.has("lightning_enabled") ? sd["lightning_enabled"].asBoolean() : true;
    mLightningCharge  = sd.has("lightning_charge")  ? sd["lightning_charge"].asBoolean()  : false;
    mLightningSparks  = sd.has("lightning_sparks")  ? sd["lightning_sparks"].asBoolean()  : false;

    mLightningAuto = sd.has("lightning_auto") ? sd["lightning_auto"].asBoolean() : true;
    if (sd.has("lightning_intensity")) mLightningIntensity.fromLLSD(sd["lightning_intensity"], 0.f);
    if (sd.has("lightning_color")) mLightningColor.fromLLSD(sd["lightning_color"], LLColor3(0.62f, 0.55f, 1.f));
    if (sd.has("lightning_core_white")) mLightningCoreWhite.fromLLSD(sd["lightning_core_white"], 0.85f);

    if (sd.has("precipitation_override")) mPrecipitationOverride.fromLLSD(sd["precipitation_override"], std::string());

    return true;
}

// The water block out to its notecard document.
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

// The water block back from a document, tolerating missing fields.
bool SSAtmoEnvWater::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    const SSAtmoEnvWater def;

    mEnabled = sd.has("enabled") ? sd["enabled"].asBoolean() : false;

    if (sd.has("height")) mHeight.fromLLSD(sd["height"], 0.f);

    if (sd.has("fog_color"))           mFogColor.fromLLSD(sd["fog_color"], def.mFogColor.valueAt(0.0));
    if (sd.has("fog_density"))         mFogDensity.fromLLSD(sd["fog_density"], def.mFogDensity.valueAt(0.0));
    if (sd.has("underwater_modifier")) mUnderwaterModifier.fromLLSD(sd["underwater_modifier"], def.mUnderwaterModifier.valueAt(0.0));

    if (sd.has("fresnel_scale"))  mFresnelScale.fromLLSD(sd["fresnel_scale"], def.mFresnelScale.valueAt(0.0));
    if (sd.has("fresnel_offset")) mFresnelOffset.fromLLSD(sd["fresnel_offset"], def.mFresnelOffset.valueAt(0.0));

    if (sd.has("normal_map"))       mNormalMap.fromLLSD(sd["normal_map"], LLUUID::null);
    if (sd.has("large_wave_speed")) mLargeWaveSpeed.fromLLSD(sd["large_wave_speed"], def.mLargeWaveSpeed.valueAt(0.0));
    if (sd.has("small_wave_speed")) mSmallWaveSpeed.fromLLSD(sd["small_wave_speed"], def.mSmallWaveSpeed.valueAt(0.0));

    if (sd.has("normal_scale_x")) mNormalScaleX.fromLLSD(sd["normal_scale_x"], def.mNormalScaleX.valueAt(0.0));
    if (sd.has("normal_scale_y")) mNormalScaleY.fromLLSD(sd["normal_scale_y"], def.mNormalScaleY.valueAt(0.0));
    if (sd.has("normal_scale_z")) mNormalScaleZ.fromLLSD(sd["normal_scale_z"], def.mNormalScaleZ.valueAt(0.0));

    if (sd.has("refraction_scale_above")) mRefractionScaleAbove.fromLLSD(sd["refraction_scale_above"], def.mRefractionScaleAbove.valueAt(0.0));
    if (sd.has("refraction_scale_below")) mRefractionScaleBelow.fromLLSD(sd["refraction_scale_below"], def.mRefractionScaleBelow.valueAt(0.0));
    if (sd.has("blur_multiplier"))        mBlurMultiplier.fromLLSD(sd["blur_multiplier"], def.mBlurMultiplier.valueAt(0.0));
    return true;
}

// One celestial body out to its notecard document.
LLSD SSAtmoEnvCelestialBody::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["kind"] = (S32)mKind;
    sd["name"] = mName;
    sd["name_custom"] = mNameCustom;
    sd["parent_index"] = mParentIndex;

    sd["diameter_m"] = (LLSD::Real)mDiameterM;
    sd["mass_relative"] = (LLSD::Real)mMassRelative;

    sd["orbital_radius"] = (LLSD::Real)mOrbitalRadius;
    sd["orbital_inclination_deg"] = (LLSD::Real)mOrbitalInclinationDeg;
    sd["orbital_phase_deg"] = (LLSD::Real)mOrbitalPhaseDeg;
    sd["orbital_period_seconds"] = mOrbitalPeriodSeconds;
    sd["rotation_period_seconds"] = mRotationPeriodSeconds;

    sd["axial_tilt_deg"] = (LLSD::Real)mAxialTiltDeg;
    sd["latitude_deg"] = (LLSD::Real)mLatitudeDeg;
    sd["emissive"] = mEmissive;
    sd["phase_shaded"] = mPhaseShaded;
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

// One celestial body back from a document, tolerating missing fields.
bool SSAtmoEnvCelestialBody::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    mKind = (EKind)llclamp(sd.has("kind") ? sd["kind"].asInteger() : (S32)PLANET, (S32)SUN, (S32)MOON);
    mName = sd.has("name") ? sd["name"].asString() : std::string("Body");
    mNameCustom = sd.has("name_custom") ? sd["name_custom"].asBoolean() : false;
    mParentIndex = sd.has("parent_index") ? sd["parent_index"].asInteger() : -1;

    if (sd.has("diameter_m")) mDiameterM = llmax(0.f, (F32)sd["diameter_m"].asReal());
    if (sd.has("mass_relative")) mMassRelative = llmax(0.f, (F32)sd["mass_relative"].asReal());

    if (sd.has("orbital_radius")) mOrbitalRadius = llmax(0.f, (F32)sd["orbital_radius"].asReal());
    if (sd.has("orbital_inclination_deg")) mOrbitalInclinationDeg = (F32)sd["orbital_inclination_deg"].asReal();
    if (sd.has("orbital_phase_deg")) mOrbitalPhaseDeg = (F32)sd["orbital_phase_deg"].asReal();
    mOrbitalPeriodSeconds = sd.has("orbital_period_seconds") ? sd["orbital_period_seconds"].asReal() : 0.0;
    mRotationPeriodSeconds = sd.has("rotation_period_seconds") ? sd["rotation_period_seconds"].asReal() : 0.0;

    if (sd.has("axial_tilt_deg")) mAxialTiltDeg = (F32)sd["axial_tilt_deg"].asReal();
    mLatitudeDeg = llclamp(sd.has("latitude_deg") ? (F32)sd["latitude_deg"].asReal()
                                                 : mAxialTiltDeg, -90.f, 90.f);

    mEmissive = sd.has("emissive") ? sd["emissive"].asBoolean() : (mKind == SUN);
    mPhaseShaded = sd.has("phase_shaded") ? sd["phase_shaded"].asBoolean() : (mKind != SUN);
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

// The body the observer stands on, or -1.
S32 SSAtmoEnvPlanetary::homeBodyIndex() const
{
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        if (mBodies[i].mIsHome) return (S32)i;
    }
    return -1;
}

// Moves the observer to another body.
bool SSAtmoEnvPlanetary::setHomeBody(S32 index)
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;

    for (SSAtmoEnvCelestialBody& body : mBodies)
    {
        body.mIsHome = false;
    }
    mBodies[index].mIsHome = true;
    mBodies[index].mIsLightEmitter = false;
    return true;
}

// Which bodies are marked as light emitters, in list order.
std::vector<S32> SSAtmoEnvPlanetary::lightEmitterIndices() const
{
    std::vector<S32> out;
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        if (mBodies[i].mIsLightEmitter) out.push_back((S32)i);
    }
    return out;
}

// Whether another emitter slot is available for this body.
bool SSAtmoEnvPlanetary::canSetLightEmitter(S32 index) const
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;
    if (mBodies[index].mIsHome) return false;
    if (mBodies[index].mIsLightEmitter) return true;

    return lightEmitterIndices().size() < 2;
}

// Adds a body of a kind with sensible defaults, parented and placed so it appears somewhere reasonable.
S32 SSAtmoEnvPlanetary::addBody(SSAtmoEnvCelestialBody::EKind kind, S32 preferred_parent_index)
{
    S32 sun_count = 0;
    for (const SSAtmoEnvCelestialBody& b : mBodies)
    {
        if (b.mKind == SSAtmoEnvCelestialBody::SUN) ++sun_count;
    }
    if (kind == SSAtmoEnvCelestialBody::SUN && sun_count >= SS_ATMOENV_MAX_SUNS)
    {
        return -1;
    }

    SSAtmoEnvCelestialBody body;
    body.mKind = kind;

    body.mEmissive = (kind == SSAtmoEnvCelestialBody::SUN);
    body.mPhaseShaded = !body.mEmissive;

    S32 same_kind = 0;
    for (const SSAtmoEnvCelestialBody& b : mBodies)
    {
        if (b.mKind == kind) ++same_kind;
    }
    const char* kind_name = (kind == SSAtmoEnvCelestialBody::SUN)    ? "Sun"
                          : (kind == SSAtmoEnvCelestialBody::PLANET) ? "Planet"
                                                                     : "Moon";
    body.mName = llformat("%s %d", kind_name, same_kind + 1);

    body.mParentIndex = -1;
    if (kind == SSAtmoEnvCelestialBody::PLANET)
    {
        for (size_t i = 0; i < mBodies.size(); ++i)
        {
            if (mBodies[i].mKind == SSAtmoEnvCelestialBody::SUN)
            {
                body.mParentIndex = (S32)i;
                break;
            }
        }
    }
    else if (kind == SSAtmoEnvCelestialBody::MOON)
    {
        if (preferred_parent_index >= 0 && preferred_parent_index < (S32)mBodies.size()
            && mBodies[preferred_parent_index].mKind == SSAtmoEnvCelestialBody::PLANET)
        {
            body.mParentIndex = preferred_parent_index;
        }
        else
        {
            for (size_t i = 0; i < mBodies.size(); ++i)
            {
                if (mBodies[i].mKind == SSAtmoEnvCelestialBody::PLANET)
                {
                    body.mParentIndex = (S32)i;
                    break;
                }
            }
        }
    }

    switch (kind)
    {
        case SSAtmoEnvCelestialBody::SUN:
            body.mDiameterM = 1.392e9f;
            body.mMassRelative = 1.f;
            body.mOrbitalRadius = 0.f;
            break;
        case SSAtmoEnvCelestialBody::PLANET:
        {
            body.mDiameterM = 1.2742e7f;
            body.mMassRelative = 1.f;
            F32 outermost = 0.f;
            for (const SSAtmoEnvCelestialBody& b : mBodies)
            {
                if (b.mKind == SSAtmoEnvCelestialBody::PLANET)
                {
                    outermost = llmax(outermost, b.mOrbitalRadius);
                }
            }
            body.mOrbitalRadius = outermost + 1.496e11f;
            break;
        }
        case SSAtmoEnvCelestialBody::MOON:
        {
            body.mDiameterM = 3.475e6f;
            body.mMassRelative = 0.0123f;
            F32 outermost = 0.f;
            for (const SSAtmoEnvCelestialBody& b : mBodies)
            {
                if (b.mKind == SSAtmoEnvCelestialBody::MOON
                    && body.mParentIndex >= 0 && b.mParentIndex == body.mParentIndex)
                {
                    outermost = llmax(outermost, b.mOrbitalRadius);
                }
            }
            body.mOrbitalRadius = outermost + 3.844e8f;
            break;
        }
    }

    mBodies.push_back(body);
    const S32 index = (S32)mBodies.size() - 1;

    if (kind == SSAtmoEnvCelestialBody::SUN)
    {
        normalizeSunTopology();
    }

    if (kind == SSAtmoEnvCelestialBody::SUN && sun_count == 1
        && canSetLightEmitter(index))
    {
        mBodies[index].mIsLightEmitter = true;
    }
    else if (kind == SSAtmoEnvCelestialBody::MOON && sun_count < 2
             && body.mParentIndex >= 0 && body.mParentIndex == homeBodyIndex()
             && canSetLightEmitter(index))
    {
        mBodies[index].mIsLightEmitter = true;
    }

    autoNameBodies();
    return index;
}

// Removes a body and repairs every index that pointed at or past it.
bool SSAtmoEnvPlanetary::removeBody(S32 index)
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;

    std::vector<bool> doomed(mBodies.size(), false);
    doomed[index] = true;
    if (mBodies[index].mKind == SSAtmoEnvCelestialBody::PLANET)
    {
        for (size_t i = 0; i < mBodies.size(); ++i)
        {
            if (mBodies[i].mKind == SSAtmoEnvCelestialBody::MOON
                && mBodies[i].mParentIndex == index)
            {
                doomed[i] = true;
            }
        }
    }

    std::vector<S32> remap(mBodies.size());
    S32 next = 0;
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        remap[i] = doomed[i] ? -1 : next++;
    }

    std::vector<SSAtmoEnvCelestialBody> kept;
    kept.reserve((size_t)next);
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        if (doomed[i]) continue;
        SSAtmoEnvCelestialBody body = mBodies[i];
        body.mParentIndex = (body.mParentIndex >= 0) ? remap[body.mParentIndex] : -1;
        body.mBoundPartnerIndex = (body.mBoundPartnerIndex >= 0) ? remap[body.mBoundPartnerIndex] : -1;
        kept.push_back(body);
    }
    mBodies.swap(kept);

    normalizeSunTopology();
    autoNameBodies();

    return true;
}

// The body this one actually orbits, resolving defaults.
S32 SSAtmoEnvPlanetary::effectiveParent(S32 index) const
{
    if (index < 0 || index >= (S32)mBodies.size()) return -1;
    const SSAtmoEnvCelestialBody& body = mBodies[(size_t)index];

    if (body.mKind == SSAtmoEnvCelestialBody::PLANET)
    {
        for (size_t i = 0; i < mBodies.size(); ++i)
        {
            if (mBodies[i].mKind == SSAtmoEnvCelestialBody::SUN) return (S32)i;
        }
        return -1;
    }

    const S32 parent = body.mParentIndex;
    if (parent < 0 || parent >= (S32)mBodies.size() || parent == index) return -1;
    return parent;
}

// Repairs sun parent/partner links into a consistent topology after edits.
void SSAtmoEnvPlanetary::normalizeSunTopology()
{
    std::vector<S32> suns;
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        if (mBodies[i].mKind == SSAtmoEnvCelestialBody::SUN) suns.push_back((S32)i);
    }
    if (suns.empty()) return;

    for (const S32 s : suns)
    {
        clearBoundPartner(s);
    }

    mBodies[suns[0]].mParentIndex = -1;
    if (suns.size() >= 2)
    {
        mBodies[suns[1]].mParentIndex = -1;
        setBoundPartner(suns[0], suns[1]);
        if (mBodies[suns[1]].mOrbitalRadius <= 0.f)
        {
            mBodies[suns[1]].mOrbitalRadius = 3.74e10f;
        }
    }
    if (suns.size() >= 3)
    {
        mBodies[suns[2]].mParentIndex = suns[0];
        if (mBodies[suns[2]].mOrbitalRadius <= 0.f)
        {
            mBodies[suns[2]].mOrbitalRadius = 1.496e11f;
        }
    }
    if (suns.size() >= 4)
    {
        mBodies[suns[3]].mParentIndex = suns[0];
        setBoundPartner(suns[2], suns[3]);
        if (mBodies[suns[3]].mOrbitalRadius <= 0.f)
        {
            mBodies[suns[3]].mOrbitalRadius = 1.496e11f;
        }
    }
}

namespace
{
    // Roman numerals for auto body names.
    std::string ss_roman_numeral(S32 n)
    {
        std::string out;
        const struct { S32 mValue; const char* mGlyph; } steps[] = {
            { 10, "X" }, { 9, "IX" }, { 5, "V" }, { 4, "IV" }, { 1, "I" },
        };
        for (const auto& step : steps)
        {
            while (n >= step.mValue)
            {
                out += step.mGlyph;
                n -= step.mValue;
            }
        }
        return out;
    }
}

// Names unnamed bodies from their kind and position in the system.
void SSAtmoEnvPlanetary::autoNameBodies()
{
    const S32 n = (S32)mBodies.size();

    std::vector<S32> suns;
    std::vector<S32> planets;
    for (S32 i = 0; i < n; ++i)
    {
        if (mBodies[i].mKind == SSAtmoEnvCelestialBody::SUN)         suns.push_back(i);
        else if (mBodies[i].mKind == SSAtmoEnvCelestialBody::PLANET) planets.push_back(i);
    }

    if (!suns.empty() && !mBodies[suns[0]].mNameCustom)
    {
        mBodies[suns[0]].mName = "Sol";
    }
    const std::string stem = suns.empty() ? std::string("Sol") : mBodies[suns[0]].mName;

    for (size_t s = 1; s < suns.size(); ++s)
    {
        if (!mBodies[suns[s]].mNameCustom)
        {
            mBodies[suns[s]].mName = llformat("%s %c", stem.c_str(), (char)('A' + (S32)s));
        }
    }

    std::stable_sort(planets.begin(), planets.end(),
        [this](S32 a, S32 b) { return mBodies[a].mOrbitalRadius < mBodies[b].mOrbitalRadius; });
    for (size_t r = 0; r < planets.size(); ++r)
    {
        if (!mBodies[planets[r]].mNameCustom)
        {
            mBodies[planets[r]].mName = llformat("%s %s", stem.c_str(),
                                                 ss_roman_numeral((S32)r + 1).c_str());
        }
    }

    for (const S32 planet : planets)
    {
        std::vector<S32> moons;
        for (S32 i = 0; i < n; ++i)
        {
            if (mBodies[i].mKind == SSAtmoEnvCelestialBody::MOON
                && mBodies[i].mParentIndex == planet)
            {
                moons.push_back(i);
            }
        }
        std::stable_sort(moons.begin(), moons.end(),
            [this](S32 a, S32 b) { return mBodies[a].mOrbitalRadius < mBodies[b].mOrbitalRadius; });
        for (size_t m = 0; m < moons.size(); ++m)
        {
            if (!mBodies[moons[m]].mNameCustom)
            {
                mBodies[moons[m]].mName = llformat("%s.%d", mBodies[planet].mName.c_str(),
                                                   (S32)m + 1);
            }
        }
    }
}

// Binds two suns into a pair.
bool SSAtmoEnvPlanetary::setBoundPartner(S32 a, S32 b)
{
    if (a == b) return false;
    if (a < 0 || a >= (S32)mBodies.size()) return false;
    if (b < 0 || b >= (S32)mBodies.size()) return false;

    if (mBodies[a].mParentIndex != mBodies[b].mParentIndex) return false;

    clearBoundPartner(a);
    clearBoundPartner(b);

    mBodies[a].mBoundPartnerIndex = b;
    mBodies[b].mBoundPartnerIndex = a;
    return true;
}

// Dissolves a sun pair.
bool SSAtmoEnvPlanetary::clearBoundPartner(S32 index)
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;

    const S32 partner = mBodies[index].mBoundPartnerIndex;
    mBodies[index].mBoundPartnerIndex = -1;

    if (partner >= 0 && partner < (S32)mBodies.size()
        && mBodies[partner].mBoundPartnerIndex == index)
    {
        mBodies[partner].mBoundPartnerIndex = -1;
    }
    return true;
}

// The planetary system out to its notecard document.
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

// The planetary system back from a document, tolerating missing fields.
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

// Defaults to auto derivation.
SSAtmoEnvCloudField::SSAtmoEnvCloudField()
    : mBaseTexture(LLUUID(SSAtmoEnvCloudDome::CLOUD_TEXTURE_CUMULONIMBUS))
    , mDetailTexture(LLUUID(SSAtmoEnvCloudDome::CLOUD_TEXTURE_ALTOCUMULUS))
{
}

// The volumetric cloud field block out to its notecard document.
LLSD SSAtmoEnvCloudField::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["auto"] = mAuto;
    sd["base_height_m"] = mBaseHeightM.asLLSD();
    sd["base_thickness_m"] = mBaseThicknessM.asLLSD();
    sd["coverage_scale"] = mCoverageScale.asLLSD();
    sd["base_texture"] = mBaseTexture.asLLSD();
    sd["detail_texture"] = mDetailTexture.asLLSD();
    sd["texture_mix"] = mTextureMix.asLLSD();
    sd["puff_density"] = mPuffDensity.asLLSD();
    sd["detail_scale"] = mDetailScale.asLLSD();
    sd["drift_rate"] = mDriftRate.asLLSD();
    sd["storm_darkening"] = mStormDarkening.asLLSD();
    return sd;
}

// The volumetric cloud field block back from a document, tolerating missing fields.
bool SSAtmoEnvCloudField::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;
    mAuto = sd.has("auto") ? sd["auto"].asBoolean() : true;
    if (sd.has("base_height_m")) mBaseHeightM.fromLLSD(sd["base_height_m"], 800.f);
    if (sd.has("base_thickness_m")) mBaseThicknessM.fromLLSD(sd["base_thickness_m"], 300.f);
    if (sd.has("coverage_scale")) mCoverageScale.fromLLSD(sd["coverage_scale"], 1.f);
    const SSAtmoEnvCloudField def;
    if (sd.has("base_texture")) mBaseTexture.fromLLSD(sd["base_texture"], def.mBaseTexture.valueAt(0.0));
    if (sd.has("detail_texture")) mDetailTexture.fromLLSD(sd["detail_texture"], def.mDetailTexture.valueAt(0.0));
    if (sd.has("texture_mix")) mTextureMix.fromLLSD(sd["texture_mix"], 0.4f);
    if (sd.has("puff_density")) mPuffDensity.fromLLSD(sd["puff_density"], 0.8f);
    if (sd.has("detail_scale")) mDetailScale.fromLLSD(sd["detail_scale"], 3.f);
    if (sd.has("drift_rate")) mDriftRate.fromLLSD(sd["drift_rate"], 1.f);
    if (sd.has("storm_darkening")) mStormDarkening.fromLLSD(sd["storm_darkening"], 0.85f);
    return true;
}

namespace
{
    template <typename T>
    // Adds or overwrites a keyframe at a phase.
    void stampKeyframe(SSAtmoEnvKeyframed<T>& field, F64 phase, const T& value)
    {
        if (!field.hasKeyframeAt(phase))
        {
            field.toggleKeyframeAtHead(phase);
        }
        field.setValueAtHead(phase, value);
    }

    const F32 SEED_COLLAPSE_EPSILON = 1.0e-6f;
}

// Seeds the dome block from a fetched EEP sky.
void SSAtmoEnvCloudDome::fromSettingsSky(const LLSettingsSky& sky)
{
    mColor    = SSAtmoEnvKeyframed<LLColor3>(sky.getCloudColor());
    mCoverage = SSAtmoEnvKeyframed<F32>(sky.getCloudShadow());
    mScale    = SSAtmoEnvKeyframed<F32>(sky.getCloudScale());
    mVariance = SSAtmoEnvKeyframed<F32>(sky.getCloudVariance());

    mScrollRate = SSAtmoEnvKeyframed<LLVector2>(sky.getCloudScrollRate());

    const LLColor3 density = sky.getCloudPosDensity1();
    mDensityX = SSAtmoEnvKeyframed<F32>(density.mV[0]);
    mDensityY = SSAtmoEnvKeyframed<F32>(density.mV[1]);
    mDensityD = SSAtmoEnvKeyframed<F32>(density.mV[2]);

    const LLColor3 detail = sky.getCloudPosDensity2();
    mDetailX = SSAtmoEnvKeyframed<F32>(detail.mV[0]);
    mDetailY = SSAtmoEnvKeyframed<F32>(detail.mV[1]);
    mDetailD = SSAtmoEnvKeyframed<F32>(detail.mV[2]);

    const LLUUID noise = sky.getCloudNoiseTextureId();
    mNoiseTexture = SSAtmoEnvKeyframed<LLUUID>(
        noise == LLSettingsSky::GetDefaultCloudNoiseTextureId() ? LLUUID::null : noise);
}

// Stamps a fetched sky's dome values as keyframes at a phase.
void SSAtmoEnvCloudDome::addKeyframesFromSky(const LLSettingsSky& sky, F64 phase)
{
    stampKeyframe(mColor,    phase, sky.getCloudColor());
    stampKeyframe(mCoverage, phase, sky.getCloudShadow());
    stampKeyframe(mScale,    phase, sky.getCloudScale());
    stampKeyframe(mVariance, phase, sky.getCloudVariance());

    stampKeyframe(mScrollRate, phase, sky.getCloudScrollRate());

    const LLColor3 density = sky.getCloudPosDensity1();
    stampKeyframe(mDensityX, phase, density.mV[0]);
    stampKeyframe(mDensityY, phase, density.mV[1]);
    stampKeyframe(mDensityD, phase, density.mV[2]);

    const LLColor3 detail = sky.getCloudPosDensity2();
    stampKeyframe(mDetailX, phase, detail.mV[0]);
    stampKeyframe(mDetailY, phase, detail.mV[1]);
    stampKeyframe(mDetailD, phase, detail.mV[2]);

    const LLUUID noise = sky.getCloudNoiseTextureId();
    stampKeyframe(mNoiseTexture, phase,
        noise == LLSettingsSky::GetDefaultCloudNoiseTextureId() ? LLUUID::null : noise);
}

// Fields all keyframes agree on collapse back to plain values.
void SSAtmoEnvCloudDome::collapseConstantKeyframes()
{
    mHeightM.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mColor.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mCoverage.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mScale.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mVariance.collapseIfConstant(SEED_COLLAPSE_EPSILON);

    mScrollRate.collapseIfConstant(SEED_COLLAPSE_EPSILON);

    mDensityX.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mDensityY.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mDensityD.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mDetailX.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mDetailY.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mDetailD.collapseIfConstant(SEED_COLLAPSE_EPSILON);

    mNoiseTexture.collapseIfConstant(SEED_COLLAPSE_EPSILON);
}

const char* const SSAtmoEnvCloudDome::CLOUD_TEXTURE_LAYERED =
    "dc6e4164-e279-fda8-fe8e-b6d154156c1b";
const char* const SSAtmoEnvCloudDome::CLOUD_TEXTURE_CUMULONIMBUS =
    "a5053062-3b50-290e-1a44-5d6dc6a5fabf";
const char* const SSAtmoEnvCloudDome::CLOUD_TEXTURE_ALTOCUMULUS =
    "8438e081-06e7-bd49-1eea-aba44332750c";

const char* const SSAtmoEnvCloudDome::BODY_TEXTURE_SUN =
    "b78495ac-042f-fe13-b593-9c32e98fd99f";
const char* const SSAtmoEnvCloudDome::BODY_TEXTURE_MOON =
    "db13b827-7e6a-7ace-bed4-4419ee00984d";

// The legacy cloud dome block out to its notecard document.
LLSD SSAtmoEnvCloudDome::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["auto"]     = mAuto;
    sd["height"]   = mHeightM.asLLSD();
    sd["color"]    = mColor.asLLSD();
    sd["coverage"] = mCoverage.asLLSD();
    sd["scale"]    = mScale.asLLSD();
    sd["variance"] = mVariance.asLLSD();

    sd["scroll_rate"] = mScrollRate.asLLSD();

    sd["density_x"] = mDensityX.asLLSD();
    sd["density_y"] = mDensityY.asLLSD();
    sd["density_d"] = mDensityD.asLLSD();
    sd["detail_x"]  = mDetailX.asLLSD();
    sd["detail_y"]  = mDetailY.asLLSD();
    sd["detail_d"]  = mDetailD.asLLSD();

    sd["noise_texture"] = mNoiseTexture.asLLSD();
    return sd;
}

// The legacy cloud dome block back from a document, tolerating missing fields.
bool SSAtmoEnvCloudDome::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    const SSAtmoEnvCloudDome def;

    // <SS:Nexii> A document written before the dome had a height of its own gets the authored default, not auto: 6000m is the altitude that derivation returned in clear air anyway, so a sky with
    // no volumetric field renders identically and only one with a built-up deck notices the difference - and that one can tick Auto back on.
    mAuto = sd.has("auto") ? sd["auto"].asBoolean() : def.mAuto;
    if (sd.has("height")) mHeightM.fromLLSD(sd["height"], def.mHeightM.valueAt(0.0));

    if (sd.has("color"))    mColor.fromLLSD(sd["color"], def.mColor.valueAt(0.0));
    if (sd.has("coverage")) mCoverage.fromLLSD(sd["coverage"], def.mCoverage.valueAt(0.0));
    if (sd.has("scale"))    mScale.fromLLSD(sd["scale"], def.mScale.valueAt(0.0));
    if (sd.has("variance")) mVariance.fromLLSD(sd["variance"], def.mVariance.valueAt(0.0));

    if (sd.has("scroll_rate")) mScrollRate.fromLLSD(sd["scroll_rate"], def.mScrollRate.valueAt(0.0));

    if (sd.has("density_x")) mDensityX.fromLLSD(sd["density_x"], def.mDensityX.valueAt(0.0));
    if (sd.has("density_y")) mDensityY.fromLLSD(sd["density_y"], def.mDensityY.valueAt(0.0));
    if (sd.has("density_d")) mDensityD.fromLLSD(sd["density_d"], def.mDensityD.valueAt(0.0));
    if (sd.has("detail_x"))  mDetailX.fromLLSD(sd["detail_x"], def.mDetailX.valueAt(0.0));
    if (sd.has("detail_y"))  mDetailY.fromLLSD(sd["detail_y"], def.mDetailY.valueAt(0.0));
    if (sd.has("detail_d"))  mDetailD.fromLLSD(sd["detail_d"], def.mDetailD.valueAt(0.0));

    if (sd.has("noise_texture")) mNoiseTexture.fromLLSD(sd["noise_texture"], LLUUID::null);
    return true;
}

// Seeds the atmosphere block from a fetched EEP sky.
void SSAtmoEnvAtmosphere::fromSettingsSky(const LLSettingsSky& sky)
{
    mAmbientColor  = SSAtmoEnvKeyframed<LLColor3>(sky.getAmbientColor());
    mBlueHorizon   = SSAtmoEnvKeyframed<LLColor3>(sky.getBlueHorizon());
    mBlueDensity   = SSAtmoEnvKeyframed<LLColor3>(sky.getBlueDensity());
    mSunlightColor = SSAtmoEnvKeyframed<LLColor3>(sky.getSunlightColor());

    mHazeHorizon        = SSAtmoEnvKeyframed<F32>(sky.getHazeHorizon());
    mHazeDensity        = SSAtmoEnvKeyframed<F32>(sky.getHazeDensity());
    mSkyMoistureLevel   = SSAtmoEnvKeyframed<F32>(sky.getSkyMoistureLevel());
    mSkyDropletRadius   = SSAtmoEnvKeyframed<F32>(sky.getSkyDropletRadius());
    mSkyIceLevel        = SSAtmoEnvKeyframed<F32>(sky.getSkyIceLevel());
    mDensityMultiplier  = SSAtmoEnvKeyframed<F32>(sky.getDensityMultiplier());
    mDistanceMultiplier = SSAtmoEnvKeyframed<F32>(sky.getDistanceMultiplier());
    mMaxAltitude        = SSAtmoEnvKeyframed<F32>(sky.getMaxY());
    mReflectionProbeAmbiance = SSAtmoEnvKeyframed<F32>(sky.getReflectionProbeAmbiance());
    mSceneGamma         = SSAtmoEnvKeyframed<F32>(sky.getGamma());

    mStarBrightness = SSAtmoEnvKeyframed<F32>(sky.getStarBrightness());
    mMoonBrightness = SSAtmoEnvKeyframed<F32>(sky.getMoonBrightness());

    const LLColor3 glow = sky.getGlow();
    mGlowSize  = SSAtmoEnvKeyframed<F32>(2.f - glow.mV[0] / 20.f);
    mGlowFocus = SSAtmoEnvKeyframed<F32>(glow.mV[2] / -5.f);

}

// Stamps a fetched sky's atmosphere values as keyframes at a phase.
void SSAtmoEnvAtmosphere::addKeyframesFromSky(const LLSettingsSky& sky, F64 phase)
{
    stampKeyframe(mAmbientColor,  phase, sky.getAmbientColor());
    stampKeyframe(mBlueHorizon,   phase, sky.getBlueHorizon());
    stampKeyframe(mBlueDensity,   phase, sky.getBlueDensity());
    stampKeyframe(mSunlightColor, phase, sky.getSunlightColor());

    stampKeyframe(mHazeHorizon,        phase, sky.getHazeHorizon());
    stampKeyframe(mHazeDensity,        phase, sky.getHazeDensity());
    stampKeyframe(mSkyMoistureLevel,   phase, sky.getSkyMoistureLevel());
    stampKeyframe(mSkyDropletRadius,   phase, sky.getSkyDropletRadius());
    stampKeyframe(mSkyIceLevel,        phase, sky.getSkyIceLevel());
    stampKeyframe(mDensityMultiplier,  phase, sky.getDensityMultiplier());
    stampKeyframe(mDistanceMultiplier, phase, sky.getDistanceMultiplier());
    stampKeyframe(mMaxAltitude,        phase, sky.getMaxY());
    stampKeyframe(mReflectionProbeAmbiance, phase, sky.getReflectionProbeAmbiance());
    stampKeyframe(mSceneGamma,         phase, sky.getGamma());

    stampKeyframe(mStarBrightness, phase, sky.getStarBrightness());
    stampKeyframe(mMoonBrightness, phase, sky.getMoonBrightness());

    const LLColor3 glow = sky.getGlow();
    stampKeyframe(mGlowSize,  phase, 2.f - glow.mV[0] / 20.f);
    stampKeyframe(mGlowFocus, phase, glow.mV[2] / -5.f);
}

// Fields all keyframes agree on collapse back to plain values.
void SSAtmoEnvAtmosphere::collapseConstantKeyframes()
{
    mAmbientColor.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mBlueHorizon.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mBlueDensity.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mSunlightColor.collapseIfConstant(SEED_COLLAPSE_EPSILON);

    mHazeHorizon.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mHazeDensity.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mSkyMoistureLevel.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mSkyDropletRadius.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mSkyIceLevel.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mDensityMultiplier.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mDistanceMultiplier.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mMaxAltitude.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mReflectionProbeAmbiance.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mSceneGamma.collapseIfConstant(SEED_COLLAPSE_EPSILON);

    mStarBrightness.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mGlowFocus.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mGlowSize.collapseIfConstant(SEED_COLLAPSE_EPSILON);
    mMoonBrightness.collapseIfConstant(SEED_COLLAPSE_EPSILON);
}

// The atmosphere and lighting block out to its notecard document.
LLSD SSAtmoEnvAtmosphere::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["ambient"]        = mAmbientColor.asLLSD();
    sd["blue_horizon"]   = mBlueHorizon.asLLSD();
    sd["blue_density"]   = mBlueDensity.asLLSD();
    sd["sunlight_color"] = mSunlightColor.asLLSD();

    sd["haze_horizon"]        = mHazeHorizon.asLLSD();
    sd["haze_density"]        = mHazeDensity.asLLSD();
    sd["moisture_level"]      = mSkyMoistureLevel.asLLSD();
    sd["droplet_radius"]      = mSkyDropletRadius.asLLSD();
    sd["ice_level"]           = mSkyIceLevel.asLLSD();
    sd["density_multiplier"]  = mDensityMultiplier.asLLSD();
    sd["distance_multiplier"] = mDistanceMultiplier.asLLSD();
    sd["max_altitude"]        = mMaxAltitude.asLLSD();
    sd["reflection_probe_ambiance"] = mReflectionProbeAmbiance.asLLSD();
    sd["scene_gamma"]         = mSceneGamma.asLLSD();

    sd["star_brightness"] = mStarBrightness.asLLSD();
    sd["glow_focus"]      = mGlowFocus.asLLSD();
    sd["glow_size"]       = mGlowSize.asLLSD();
    sd["moon_brightness"] = mMoonBrightness.asLLSD();

    sd["horizon_clip"] = mHorizonClip;
    return sd;
}

// The atmosphere and lighting block back from a document, tolerating missing fields.
bool SSAtmoEnvAtmosphere::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    const SSAtmoEnvAtmosphere def;

    if (sd.has("ambient"))        mAmbientColor.fromLLSD(sd["ambient"], def.mAmbientColor.valueAt(0.0));
    if (sd.has("blue_horizon"))   mBlueHorizon.fromLLSD(sd["blue_horizon"], def.mBlueHorizon.valueAt(0.0));
    if (sd.has("blue_density"))   mBlueDensity.fromLLSD(sd["blue_density"], def.mBlueDensity.valueAt(0.0));
    if (sd.has("sunlight_color")) mSunlightColor.fromLLSD(sd["sunlight_color"], def.mSunlightColor.valueAt(0.0));

    if (sd.has("haze_horizon"))        mHazeHorizon.fromLLSD(sd["haze_horizon"], def.mHazeHorizon.valueAt(0.0));
    if (sd.has("haze_density"))        mHazeDensity.fromLLSD(sd["haze_density"], def.mHazeDensity.valueAt(0.0));
    if (sd.has("moisture_level"))      mSkyMoistureLevel.fromLLSD(sd["moisture_level"], def.mSkyMoistureLevel.valueAt(0.0));
    if (sd.has("droplet_radius"))      mSkyDropletRadius.fromLLSD(sd["droplet_radius"], def.mSkyDropletRadius.valueAt(0.0));
    if (sd.has("ice_level"))           mSkyIceLevel.fromLLSD(sd["ice_level"], def.mSkyIceLevel.valueAt(0.0));
    if (sd.has("density_multiplier"))  mDensityMultiplier.fromLLSD(sd["density_multiplier"], def.mDensityMultiplier.valueAt(0.0));
    if (sd.has("distance_multiplier")) mDistanceMultiplier.fromLLSD(sd["distance_multiplier"], def.mDistanceMultiplier.valueAt(0.0));
    if (sd.has("max_altitude"))        mMaxAltitude.fromLLSD(sd["max_altitude"], def.mMaxAltitude.valueAt(0.0));
    if (sd.has("reflection_probe_ambiance")) mReflectionProbeAmbiance.fromLLSD(sd["reflection_probe_ambiance"], def.mReflectionProbeAmbiance.valueAt(0.0));
    if (sd.has("scene_gamma"))         mSceneGamma.fromLLSD(sd["scene_gamma"], def.mSceneGamma.valueAt(0.0));

    if (sd.has("star_brightness")) mStarBrightness.fromLLSD(sd["star_brightness"], def.mStarBrightness.valueAt(0.0));
    if (sd.has("glow_focus"))      mGlowFocus.fromLLSD(sd["glow_focus"], def.mGlowFocus.valueAt(0.0));
    if (sd.has("glow_size"))       mGlowSize.fromLLSD(sd["glow_size"], def.mGlowSize.valueAt(0.0));
    if (sd.has("moon_brightness")) mMoonBrightness.fromLLSD(sd["moon_brightness"], def.mMoonBrightness.valueAt(0.0));

    mHorizonClip = sd.has("horizon_clip") ? sd["horizon_clip"].asBoolean() : def.mHorizonClip;
    return true;
}

// The weather influence settings out to its notecard document.
LLSD SSAtmoEnvWeatherInfluence::asLLSD() const
{
    LLSD sd;
    sd["enabled"] = mEnabled;

    sd["cloud_cover_enabled"]     = mCloudCoverEnabled;
    sd["cloud_cover_strength"]    = (LLSD::Real)mCloudCoverStrength;
    sd["wind_scroll_enabled"]     = mWindScrollEnabled;
    sd["wind_scroll_strength"]    = (LLSD::Real)mWindScrollStrength;
    sd["haze_enabled"]            = mHazeEnabled;
    sd["haze_strength"]           = (LLSD::Real)mHazeStrength;
    sd["storm_darkening_enabled"] = mStormDarkeningEnabled;
    sd["storm_darkening_strength"]= (LLSD::Real)mStormDarkeningStrength;
    sd["cold_sky_enabled"]        = mColdSkyEnabled;
    sd["cold_sky_strength"]       = (LLSD::Real)mColdSkyStrength;
    sd["rainbow_enabled"]         = mRainbowEnabled;
    sd["rainbow_strength"]        = (LLSD::Real)mRainbowStrength;
    return sd;
}

// The weather influence settings back from a document, tolerating missing fields.
bool SSAtmoEnvWeatherInfluence::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    auto flag = [&sd](const char* key, bool& out)
    {
        if (sd.has(key)) out = sd[key].asBoolean();
    };
    auto strength = [&sd](const char* key, F32& out)
    {
        if (sd.has(key)) out = llclamp((F32)sd[key].asReal(), 0.f, 1.f);
    };

    flag("enabled", mEnabled);
    flag("cloud_cover_enabled", mCloudCoverEnabled);
    strength("cloud_cover_strength", mCloudCoverStrength);
    flag("wind_scroll_enabled", mWindScrollEnabled);
    strength("wind_scroll_strength", mWindScrollStrength);
    flag("haze_enabled", mHazeEnabled);
    strength("haze_strength", mHazeStrength);
    flag("storm_darkening_enabled", mStormDarkeningEnabled);
    strength("storm_darkening_strength", mStormDarkeningStrength);
    flag("cold_sky_enabled", mColdSkyEnabled);
    strength("cold_sky_strength", mColdSkyStrength);
    flag("rainbow_enabled", mRainbowEnabled);
    strength("rainbow_strength", mRainbowStrength);
    return true;
}

// One track out to its notecard document.
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
    sd["cloud_dome"]  = mCloudDome.asLLSD();
    sd["atmosphere"] = mAtmosphere.asLLSD();
    sd["weather_influence"] = mWeatherInfluence.asLLSD();
    return sd;
}

// One track back from a document, tolerating missing fields.
bool SSAtmoEnvTrack::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    if (sd.has("name")) mName = sd["name"].asString();
    if (sd.has("floor_z")) mFloorZ = (F32)sd["floor_z"].asReal();
    if (sd.has("transition_buffer")) mTransitionBuffer = llmax(0.f, (F32)sd["transition_buffer"].asReal());

    mDayLengthSeconds = sd.has("day_length_seconds")
        ? sd["day_length_seconds"].asReal() : (4.0 * 60.0 * 60.0);
    mDayOffsetSeconds = sd.has("day_offset_seconds") ? sd["day_offset_seconds"].asReal() : 0.0;

    if (sd.has("water"))       mWater.fromLLSD(sd["water"]);
    if (sd.has("weather"))     mWeather.fromLLSD(sd["weather"]);
    if (sd.has("planetary"))   mPlanetary.fromLLSD(sd["planetary"]);
    if (sd.has("cloud_field")) mCloudField.fromLLSD(sd["cloud_field"]);
    if (sd.has("cloud_dome"))  mCloudDome.fromLLSD(sd["cloud_dome"]);
    if (sd.has("atmosphere"))  mAtmosphere.fromLLSD(sd["atmosphere"]);
    if (sd.has("weather_influence")) mWeatherInfluence.fromLLSD(sd["weather_influence"]);

    return true;
}

// Where in the day cycle this track is right now, from shared wall-clock time.
F64 SSAtmoEnvTrack::currentDayCyclePhase() const
{
    if (mDayLengthSeconds <= 0.0) return 0.0;

    const F64 utc_now = (F64)time(nullptr);
    F64 t = fmod(utc_now - mDayOffsetSeconds, mDayLengthSeconds);
    if (t < 0.0) t += mDayLengthSeconds;
    return t / mDayLengthSeconds;
}

// The fresh-creation default: one ground track, Earth-like planetary system, sensible weather.
SSAtmoEnvAsset SSAtmoEnvAsset::makeDefault()
{
    SSAtmoEnvAsset asset;
    asset.mName = "New Atmo Environment";

    SSAtmoEnvTrack ground;
    ground.mName = "Ground";
    ground.mFloorZ = 0.f;
    ground.mDayLengthSeconds = 4.0 * 60.0 * 60.0;
    ground.mDayOffsetSeconds = 0.0;
    ground.mWeather = SSAtmoEnvWeather();

    ground.mWater.mEnabled = true;
    {
        LLViewerRegion* region = gAgent.getRegion();
        ground.mWater.mHeight = SSAtmoEnvKeyframed<F32>(region ? region->getWaterHeight() : 20.f);
    }

    SSAtmoEnvCelestialBody sun;
    sun.mKind = SSAtmoEnvCelestialBody::SUN;
    sun.mParentIndex = -1;
    sun.mDiameterM = 1.392e9f;
    sun.mMassRelative = 1.f;
    sun.mIsLightEmitter = true;
    sun.mEmissive = true;
    sun.mPhaseShaded = false;
    sun.mCustomTexture = LLUUID(SSAtmoEnvCloudDome::BODY_TEXTURE_SUN);
    ground.mPlanetary.mBodies.push_back(sun);

    SSAtmoEnvCelestialBody home;
    home.mKind = SSAtmoEnvCelestialBody::PLANET;
    home.mParentIndex = 0;
    home.mDiameterM = 1.2742e7f;
    home.mMassRelative = 1.f;
    home.mOrbitalRadius = 1.496e11f;
    home.mAxialTiltDeg = 23.44f;
    home.mLatitudeDeg = 50.f;
    home.mOrbitalInclinationDeg = 7.155f;
    home.mOrbitalPhaseDeg = 0.f;
    home.mIsHome = true;
    ground.mPlanetary.mBodies.push_back(home);

    SSAtmoEnvCelestialBody moon;
    moon.mKind = SSAtmoEnvCelestialBody::MOON;
    moon.mParentIndex = 1;
    moon.mDiameterM = 3.4748e6f;
    moon.mMassRelative = 1.f;
    moon.mOrbitalRadius = 3.844e8f;
    moon.mOrbitalInclinationDeg = 5.145f;
    moon.mOrbitalPhaseDeg = 30.f;
    moon.mAxialTiltDeg = 6.68f;
    moon.mIsLightEmitter = true;
    moon.mCustomTexture = LLUUID(SSAtmoEnvCloudDome::BODY_TEXTURE_MOON);
    ground.mPlanetary.mBodies.push_back(moon);

    ground.mPlanetary.autoNameBodies();

    asset.mTracks.push_back(ground);
    return asset;
}

// Adds an altitude track above the existing ones.
bool SSAtmoEnvAsset::addTrack()
{
    if ((S32)mTracks.size() >= SS_ATMOENV_MAX_TRACKS) return false;

    SSAtmoEnvTrack track;
    F32 highest = 0.f;
    for (const SSAtmoEnvTrack& t : mTracks) highest = llmax(highest, t.mFloorZ);

    const F32 SPACING = SS_ATMOENV_MIN_TRACK_FLOOR;
    track.mFloorZ = llclamp(highest + SPACING,
                            SS_ATMOENV_MIN_TRACK_FLOOR,
                            SS_ATMOENV_REGION_CEILING - SPACING);

    track.mName = nextDefaultTrackName();

    mTracks.push_back(track);
    sortTracksByAltitude();
    return true;
}

// Removes a track; the ground track stays.
bool SSAtmoEnvAsset::removeTrack(S32 index)
{
    if (index <= 0 || index >= (S32)mTracks.size()) return false;
    mTracks.erase(mTracks.begin() + index);
    sortTracksByAltitude();
    return true;
}

// First free default track name.
std::string SSAtmoEnvAsset::nextDefaultTrackName() const
{
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

// Re-sorts tracks by floor altitude, returning where the followed one landed.
S32 SSAtmoEnvAsset::sortTracksByAltitude(S32 follow_index)
{
    if (mTracks.size() < 2) return follow_index;

    const SSAtmoEnvTrack* follow = (follow_index >= 0 && follow_index < (S32)mTracks.size())
        ? &mTracks[follow_index] : nullptr;
    const std::string follow_name = follow ? follow->mName : std::string();
    const bool follow_is_ground = (follow_index == 0);

    std::stable_sort(mTracks.begin() + 1, mTracks.end(),
        [](const SSAtmoEnvTrack& a, const SSAtmoEnvTrack& b) { return a.mFloorZ < b.mFloorZ; });

    if (follow_index < 0 || follow_is_ground) return follow_index;

    for (S32 i = 1; i < (S32)mTracks.size(); ++i)
    {
        if (mTracks[i].mName == follow_name) return i;
    }
    return follow_index;
}

// A track's ceiling: the next floor up, or open ended.
F32 SSAtmoEnvAsset::trackCeilingZ(S32 index) const
{
    if (index < 0 || index >= (S32)mTracks.size()) return SS_ATMOENV_REGION_CEILING;

    const F32 own_floor = mTracks[index].mFloorZ;

    F32 ceiling = SS_ATMOENV_REGION_CEILING;
    for (S32 i = 0; i < (S32)mTracks.size(); ++i)
    {
        if (i == index) continue;
        const F32 other = mTracks[i].mFloorZ;
        if (other > own_floor && other < ceiling) ceiling = other;
    }
    return ceiling;
}

// The water height of whichever track owns a visible water plane, if any.
bool SSAtmoEnvAsset::visibleWaterHeight(F32& out_height) const
{
    bool found = false;
    F32 lowest = FLT_MAX;
    for (const SSAtmoEnvTrack& track : mTracks)
    {
        if (!track.mWater.mEnabled) continue;

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

// The whole document.
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

// Parses and validates a whole document; on failure says why.
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
        track.fromLLSD(tracks_sd[i]);
        parsed.mTracks.push_back(track);
    }

    if (parsed.mTracks.empty())
    {
        out_error = "no tracks survived parsing";
        *this = makeDefault();
        return false;
    }

    parsed.sortTracksByAltitude();

    *this = parsed;
    return true;
}
