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
#include "llsettingssky.h"
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

    // The override values serialise even while Auto is on - authored keyframes should survive a save made with Auto re-enabled mid-experiment, not silently drop out of the notecard the way they did
    // when these were plain scalars only written on !auto.
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

bool SSAtmoEnvWeather::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    if (sd.has("moisture"))      mMoisture.fromLLSD(sd["moisture"], 0.f);
    if (sd.has("convection"))    mConvection.fromLLSD(sd["convection"], 0.f);
    if (sd.has("temperature_c")) mTemperatureC.fromLLSD(sd["temperature_c"], 15.f);
    if (sd.has("wind_heading"))  mWindHeading.fromLLSD(sd["wind_heading"], 0.f);
    if (sd.has("wind_speed"))    mWindSpeed.fromLLSD(sd["wind_speed"], 0.f);

    // The keyframe container reads a bare scalar as a plain value, so a notecard written back when these were plain F32s parses through the same call - see SSAtmoEnvKeyframed::fromLLSD's
    // fallthrough.
    mGustAuto = sd.has("gust_auto") ? sd["gust_auto"].asBoolean() : true;
    if (sd.has("gust_depth"))  mGustDepth.fromLLSD(sd["gust_depth"], 0.f);
    if (sd.has("gust_length")) mGustLength.fromLLSD(sd["gust_length"], 140.f);
    if (sd.has("gust_veer"))   mGustVeer.fromLLSD(sd["gust_veer"], 0.f);

    // Absent in anything written before these existed, and the defaults are what those environments were already doing: lightning on, neither embellishment running.
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

bool SSAtmoEnvWater::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    // Every field keeps whatever its constructor default was when the notecard doesn't mention it - a hand-written notecard that only sets height and fog colour is valid, and everything else stays
    // at the sensible default rather than becoming zero.
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

//-----------------------------------------------------------------------------
// SSAtmoEnvCelestialBody
//-----------------------------------------------------------------------------

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

bool SSAtmoEnvCelestialBody::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    mKind = (EKind)llclamp(sd.has("kind") ? sd["kind"].asInteger() : (S32)PLANET, (S32)SUN, (S32)MOON);
    mName = sd.has("name") ? sd["name"].asString() : std::string("Body");
    // Absent on anything written before auto-naming existed - false (still auto-named) is the right reading for those, no migration needed.
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
    // Absent in a notecard written while tilt was doing latitude's job, and that is what it meant then - so an old document keeps the sky it had.
    mLatitudeDeg = llclamp(sd.has("latitude_deg") ? (F32)sd["latitude_deg"].asReal()
                                                 : mAxialTiltDeg, -90.f, 90.f);

    // Absent in a notecard written before these were authorable, where the behaviour was derived from kind - so that is what absent means here, and such a document loads looking exactly as it did.
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

    // Exactly one home, ever - clearing everywhere else first rather than trusting the caller to have already done so.
    for (SSAtmoEnvCelestialBody& body : mBodies)
    {
        body.mIsHome = false;
    }
    mBodies[index].mIsHome = true;
    // A body flagged home cannot also be a light emitter - you don't light yourself.
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

S32 SSAtmoEnvPlanetary::addBody(SSAtmoEnvCelestialBody::EKind kind, S32 preferred_parent_index)
{
    S32 sun_count = 0;
    for (const SSAtmoEnvCelestialBody& b : mBodies)
    {
        if (b.mKind == SSAtmoEnvCelestialBody::SUN) ++sun_count;
    }
    // The canonical topology has no slot for a fifth star - the floater's Add Sun button disables at the cap, this is the backstop behind it.
    if (kind == SSAtmoEnvCelestialBody::SUN && sun_count >= SS_ATMOENV_MAX_SUNS)
    {
        return -1;
    }

    SSAtmoEnvCelestialBody body;
    body.mKind = kind;

    // Shading defaults follow the kind at creation, and are authored from there - a sun lights itself, everything else is lit by one. See SSAtmoEnvCelestialBody::mEmissive.
    body.mEmissive = (kind == SSAtmoEnvCelestialBody::SUN);
    body.mPhaseShaded = !body.mEmissive;

    // "Moon 3": only ever seen on a body auto-naming skips (a moon whose parenting fell through to orphaned) - everything else gets its real name from autoNameBodies() below before this call
    // returns.
    S32 same_kind = 0;
    for (const SSAtmoEnvCelestialBody& b : mBodies)
    {
        if (b.mKind == kind) ++same_kind;
    }
    const char* kind_name = (kind == SSAtmoEnvCelestialBody::SUN)    ? "Sun"
                          : (kind == SSAtmoEnvCelestialBody::PLANET) ? "Planet"
                                                                     : "Moon";
    body.mName = llformat("%s %d", kind_name, same_kind + 1);

    // Parenting is automatic - there is no parent dropdown any more. Suns are normalizeSunTopology()'s business below. A planet always parents to the first sun: for one sun that is literally it, for
    // two the resolver's barycenter rule anchors the planet at the pair's barycenter, and for the 4-sun case "barycenter of ALL suns" would be more honest but the inner pair's barycenter is an
    // acceptable approximation until someone actually needs better. A moon parents to the caller's preferred planet (the floater passes the selection, or the selection's own planet) when that names
    // one, else the first planet as a last resort - the floater disables Add Moon when no planet exists at all.
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

    // Per-kind physical defaults on the same Sol/Earth/Luna scale makeDefault() uses, so a freshly added body reads as the thing its kind says rather than as the struct's one-size PLANET default.
    // Mass is in per-level units (solar/Earth masses - see the header), so each default reads as 1.0-ish rather than some cross-level ratio. A sun's orbital radius starts 0 - roots sit at the
    // anchor, and normalizeSunTopology() fills in a pair separation where one applies. A planet or moon arrives one step OUTSIDE its outermost sibling rather than at a fixed radius - a fixed default
    // would stack every new body onto whichever sibling happens to sit there. The steps are the familiar yardsticks (1 AU, one Luna distance), so the first of each still reads as Earth's orbit and
    // the Moon's.
    switch (kind)
    {
        case SSAtmoEnvCelestialBody::SUN:
            body.mDiameterM = 1.392e9f;   // Sol
            body.mMassRelative = 1.f;     // solar masses
            body.mOrbitalRadius = 0.f;
            break;
        case SSAtmoEnvCelestialBody::PLANET:
        {
            body.mDiameterM = 1.2742e7f;  // Earth
            body.mMassRelative = 1.f;     // Earth masses
            // All planets orbit the one sun anchor, so every planet is a sibling of every other.
            F32 outermost = 0.f;
            for (const SSAtmoEnvCelestialBody& b : mBodies)
            {
                if (b.mKind == SSAtmoEnvCelestialBody::PLANET)
                {
                    outermost = llmax(outermost, b.mOrbitalRadius);
                }
            }
            body.mOrbitalRadius = outermost + 1.496e11f; // + 1 AU
            break;
        }
        case SSAtmoEnvCelestialBody::MOON:
        {
            body.mDiameterM = 3.475e6f;   // Luna
            body.mMassRelative = 0.0123f; // Earth masses - Luna's actual share
            // Siblings are only the moons of this moon's own planet.
            F32 outermost = 0.f;
            for (const SSAtmoEnvCelestialBody& b : mBodies)
            {
                if (b.mKind == SSAtmoEnvCelestialBody::MOON
                    && body.mParentIndex >= 0 && b.mParentIndex == body.mParentIndex)
                {
                    outermost = llmax(outermost, b.mOrbitalRadius);
                }
            }
            body.mOrbitalRadius = outermost + 3.844e8f; // + one Luna distance
            break;
        }
    }

    mBodies.push_back(body);
    const S32 index = (S32)mBodies.size() - 1;

    if (kind == SSAtmoEnvCelestialBody::SUN)
    {
        normalizeSunTopology();
    }

    // Creation-time lighting defaults - see the header. A fresh binary should light the scene with both stars, and a lone-sun world gaining a moon of the home planet gets that moon as its night
    // light - matching the renderer's sun+moon light slots. Defaults only: the user can uncheck either afterwards and nothing re-asserts them. canSetLightEmitter() is the never-steal guard (cap of
    // two, home excluded).
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

bool SSAtmoEnvPlanetary::removeBody(S32 index)
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;

    // Removing a planet takes its moons with it - see the header for why cascade replaced orphaning once re-parenting left the UI. The doomed set is gathered up front so one old->new index map can
    // fix every survivor in a single pass, whatever got removed.
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

    // Old index -> new index, -1 for the removed. Survivor pointers into the doomed set become -1 (a bound partner reverting to -1 is just "not paired" - no special case); pointers above removals
    // shift by however many doomed bodies sat below them. Trace, removing the Planet from [Sun(parent -1), Planet(parent 0), Moon(parent 1)]: Planet and Moon are both doomed, the Sun survives
    // untouched, and the map is [0, -1, -1].
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

    // The fixup above can leave a surviving pair member an isolated star (its partner just went to -1) - re-normalising restores the one canonical shape for however many suns remain. Planets need
    // nothing: their lineage is derived (effectiveParent) and their position anchors at the sun group's barycenter, so there is no stored planet parent to go stale. Names follow: the ordinals encode
    // ordering, and a removal changes it.
    normalizeSunTopology();
    autoNameBodies();

    return true;
}

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
        return -1; // sunless system - planets are their own roots
    }

    const S32 parent = body.mParentIndex;
    if (parent < 0 || parent >= (S32)mBodies.size() || parent == index) return -1;
    return parent;
}

void SSAtmoEnvPlanetary::normalizeSunTopology()
{
    // Suns in mBodies order - "structure order", the order they were added, which is also the order auto-naming letters them in.
    std::vector<S32> suns;
    for (size_t i = 0; i < mBodies.size(); ++i)
    {
        if (mBodies[i].mKind == SSAtmoEnvCelestialBody::SUN) suns.push_back((S32)i);
    }
    if (suns.empty()) return;

    // Dissolve every sun's existing pairing first (symmetrically, so a partner reference from some other body can't dangle), then assign the one canonical shape per star count. "One pair orbiting a
    // giant sun" needs no case of its own here - it is expressible as pair separations/masses that put the barycenter inside the primary. Trace, adding four suns one at a time (indices = add order):
    // 1 sun:  0(parent -1, partner -1) 2 suns: 0(-1, 1)  1(-1, 0)            - a bound root pair 3 suns: 0(-1, 1)  1(-1, 0)  2(0, -1)  - 2 orbits the pair's barycenter via the resolver's
    // partner-substitution rule 4 suns: 0(-1, 1)  1(-1, 0)  2(0, 3)  3(0, 2) - the outer pair orbits the inner pair's barycenter Then removing sun 1: survivors shift to 0,1,2 and re-normalise to
    // 0(-1, 1)  1(-1, 0)  2(0, -1) - the old third sun is promoted into the root pair rather than leaving the old first sun an isolated star.
    for (const S32 s : suns)
    {
        clearBoundPartner(s);
    }

    mBodies[suns[0]].mParentIndex = -1;
    if (suns.size() >= 2)
    {
        mBodies[suns[1]].mParentIndex = -1;
        setBoundPartner(suns[0], suns[1]);
        // A sun promoted into an orbiting role with no authored separation yet (radius 0, the root default) gets one; an authored radius is the user's and survives re-normalisation.
        if (mBodies[suns[1]].mOrbitalRadius <= 0.f)
        {
            mBodies[suns[1]].mOrbitalRadius = 3.74e10f; // ~0.25 AU - a close binary
        }
    }
    if (suns.size() >= 3)
    {
        mBodies[suns[2]].mParentIndex = suns[0];
        if (mBodies[suns[2]].mOrbitalRadius <= 0.f)
        {
            mBodies[suns[2]].mOrbitalRadius = 1.496e11f; // ~1 AU - the wider outer orbit
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
    // Up to XXXIX, far past the body counts a system diagram stays legible at - the subtractive pairs keep it exact rather than "IIII".
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

    // The first sun's name is the stem everything below derives from - renamed first so a fresh system's planets read "Sol I" rather than deriving from a stale name. A custom-named first sun stems
    // its whole system ("Alpha B", "Alpha II"); no sun at all falls back to the plain "Sol" stem rather than leaving planets unnamed.
    if (!suns.empty() && !mBodies[suns[0]].mNameCustom)
    {
        mBodies[suns[0]].mName = "Sol";
    }
    const std::string stem = suns.empty() ? std::string("Sol") : mBodies[suns[0]].mName;

    // Further suns letter B/C/D in structure order - the same order normalizeSunTopology() builds pairs in, so the letters read inner-pair-then-outer-pair by construction.
    for (size_t s = 1; s < suns.size(); ++s)
    {
        if (!mBodies[suns[s]].mNameCustom)
        {
            mBodies[suns[s]].mName = llformat("%s %c", stem.c_str(), (char)('A' + (S32)s));
        }
    }

    // Planets take roman ordinals by orbital radius ascending across ALL planets - they all orbit the sun anchor, so one distance ordering is the ordering. Stable, so equal radii keep add order.
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

    // Moons: "<planet name>.N" by radius around their planet, using the planet's name as it now stands - an auto "Sol I" or a custom "Tatooine" both carry their moons ("Tatooine.1"). A moon whose
    // parent isn't a planet (orphaned by a removal) keeps its current name; the list already marks it "(orphan)".
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

bool SSAtmoEnvPlanetary::setBoundPartner(S32 a, S32 b)
{
    if (a == b) return false;
    if (a < 0 || a >= (S32)mBodies.size()) return false;
    if (b < 0 || b >= (S32)mBodies.size()) return false;

    // Only ever between same-parent siblings - see mBoundPartnerIndex. Two roots (both -1) count as siblings, which is exactly the binary-star case the design doc names.
    if (mBodies[a].mParentIndex != mBodies[b].mParentIndex) return false;

    // Dissolve whatever either one was in first, so a partner being reassigned can never leave a third body still pointing at it - symmetry is the invariant the resolver relies on.
    clearBoundPartner(a);
    clearBoundPartner(b);

    mBodies[a].mBoundPartnerIndex = b;
    mBodies[b].mBoundPartnerIndex = a;
    return true;
}

bool SSAtmoEnvPlanetary::clearBoundPartner(S32 index)
{
    if (index < 0 || index >= (S32)mBodies.size()) return false;

    const S32 partner = mBodies[index].mBoundPartnerIndex;
    mBodies[index].mBoundPartnerIndex = -1;

    // The partner's back-reference only clears if it actually points here - a hand-edited notecard can break symmetry, and stomping some other pairing it names would compound the damage rather than
    // contain it.
    if (partner >= 0 && partner < (S32)mBodies.size()
        && mBodies[partner].mBoundPartnerIndex == index)
    {
        mBodies[partner].mBoundPartnerIndex = -1;
    }
    return true;
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

    // Enforce the invariants on read too, not just through the setters - a hand-edited notecard that names two homes or three emitters should degrade to "first one wins", not carry the contradiction
    // forward.
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

// Defined here rather than in the header: the texture ids belong to SSAtmoEnvCloudDome, which is declared after this struct. Cumulonimbus for the body, altocumulus over it. Chosen by looking rather
// than by reasoning about what each map is called. The pairing works because of what the two layers now DO: the body carries the broad shape of a cloud mass, which the storm map has plenty of, while
// the detail supplies the moving structure across it, which the broken mid-level map breaks up far better than the layered one did. Layered is a deck seen from below - it belongs on the dome, where
// it still is by default, and reads as too even here.
SSAtmoEnvCloudField::SSAtmoEnvCloudField()
    : mBaseTexture(LLUUID(SSAtmoEnvCloudDome::CLOUD_TEXTURE_CUMULONIMBUS))
    , mDetailTexture(LLUUID(SSAtmoEnvCloudDome::CLOUD_TEXTURE_ALTOCUMULUS))
{
}

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

bool SSAtmoEnvCloudField::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;
    // The keyframe container reads a bare scalar as a plain value, so a notecard written back when these were plain F32s parses through the same call - see SSAtmoEnvKeyframed::fromLLSD's
    // fallthrough. The old >= 0 clamps live in SSAtmoEnvCloudFieldResolver now - see the header.
    mAuto = sd.has("auto") ? sd["auto"].asBoolean() : true;
    if (sd.has("base_height_m")) mBaseHeightM.fromLLSD(sd["base_height_m"], 800.f);
    if (sd.has("base_thickness_m")) mBaseThicknessM.fromLLSD(sd["base_thickness_m"], 300.f);
    if (sd.has("coverage_scale")) mCoverageScale.fromLLSD(sd["coverage_scale"], 1.f);
    // Falling back to the constructor's choice, not to null - a malformed entry should land on the default rather than on "pick one for me".
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

//-----------------------------------------------------------------------------
// Sky seeding helpers, shared by SSAtmoEnvCloudDome and SSAtmoEnvAtmosphere
//-----------------------------------------------------------------------------

namespace
{
    // addKeyframesFromSky's per-field stamp. setValueAtHead alone would write the PLAIN value while the field has no keyframes yet (that is the widget editing rule it implements), and a plain value
    // overwritten per stamp can never accumulate into a cycle - so promote the field first: toggleKeyframeAtHead turns the value in force at `phase` into a real keyframe, which setValueAtHead then
    // finds and edits.
    template <typename T>
    void stampKeyframe(SSAtmoEnvKeyframed<T>& field, F64 phase, const T& value)
    {
        if (!field.hasKeyframeAt(phase))
        {
            field.toggleKeyframeAtHead(phase);
        }
        field.setValueAtHead(phase, value);
    }

    // Tolerance collapseConstantKeyframes() hands collapseIfConstant(). Skies that agree on a field hand over byte-identical F32s (settings assets store them verbatim), so this only has to absorb
    // float noise - and it must NOT be coarse enough to merge genuinely different small values, since density multiplier legitimately lives around 1e-4. Ids and strings compare exactly regardless -
    // see ss_atmoenv_near_equal.
    const F32 SEED_COLLAPSE_EPSILON = 1.0e-6f;
}

//-----------------------------------------------------------------------------
// SSAtmoEnvCloudDome
//-----------------------------------------------------------------------------

void SSAtmoEnvCloudDome::fromSettingsSky(const LLSettingsSky& sky)
{
    // Whole-container assignment on purpose, for the same reason the atmosphere's own seeding does it - see SSAtmoEnvAtmosphere::fromSettingsSky below.
    mColor    = SSAtmoEnvKeyframed<LLColor3>(sky.getCloudColor());
    mCoverage = SSAtmoEnvKeyframed<F32>(sky.getCloudShadow());
    mScale    = SSAtmoEnvKeyframed<F32>(sky.getCloudScale());
    mVariance = SSAtmoEnvKeyframed<F32>(sky.getCloudVariance());

    mScrollRate = SSAtmoEnvKeyframed<LLVector2>(sky.getCloudScrollRate());

    // The sky's packed density/detail colours unfold into the three scalars EEP's own panel edits - see the header on why the schema stores them unpacked.
    const LLColor3 density = sky.getCloudPosDensity1();
    mDensityX = SSAtmoEnvKeyframed<F32>(density.mV[0]);
    mDensityY = SSAtmoEnvKeyframed<F32>(density.mV[1]);
    mDensityD = SSAtmoEnvKeyframed<F32>(density.mV[2]);

    const LLColor3 detail = sky.getCloudPosDensity2();
    mDetailX = SSAtmoEnvKeyframed<F32>(detail.mV[0]);
    mDetailY = SSAtmoEnvKeyframed<F32>(detail.mV[1]);
    mDetailD = SSAtmoEnvKeyframed<F32>(detail.mV[2]);

    // The stock noise texture reads back as null: the schema's "null = default" convention should hold from the document's first save rather than pinning today's default id into the notecard.
    const LLUUID noise = sky.getCloudNoiseTextureId();
    mNoiseTexture = SSAtmoEnvKeyframed<LLUUID>(
        noise == LLSettingsSky::GetDefaultCloudNoiseTextureId() ? LLUUID::null : noise);
}

void SSAtmoEnvCloudDome::addKeyframesFromSky(const LLSettingsSky& sky, F64 phase)
{
    // Field set and conversions match fromSettingsSky above exactly - the packed density/detail unfold, the null-for-stock noise map - only the write differs: a keyframe stamped at `phase`, leaving
    // whatever is authored at other phases alone.
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

void SSAtmoEnvCloudDome::collapseConstantKeyframes()
{
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

LLSD SSAtmoEnvCloudDome::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
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

bool SSAtmoEnvCloudDome::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    // Absent keys keep their constructor defaults, same as the water and atmosphere structs - a hand-written notecard that only sets coverage gets LLSettingsSky's stock cirrus layer for everything
    // else.
    const SSAtmoEnvCloudDome def;

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

//-----------------------------------------------------------------------------
// SSAtmoEnvAtmosphere
//-----------------------------------------------------------------------------

void SSAtmoEnvAtmosphere::fromSettingsSky(const LLSettingsSky& sky)
{
    // Whole-container assignment on purpose: each field becomes a plain value with no keyframes, which is the only state a freshly created asset can be in anyway - this is a seeding step, not a
    // merge.
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
    // The RAW authored ambiance, not the RenderSkyAutoAdjustLegacy-aware read EEP's own refresh uses - that flag is a per-viewer display convenience, and a transcription should carry what the sky
    // says.
    mReflectionProbeAmbiance = SSAtmoEnvKeyframed<F32>(sky.getReflectionProbeAmbiance());
    mSceneGamma         = SSAtmoEnvKeyframed<F32>(sky.getGamma());

    mStarBrightness = SSAtmoEnvKeyframed<F32>(sky.getStarBrightness());
    mMoonBrightness = SSAtmoEnvKeyframed<F32>(sky.getMoonBrightness());

    // Packed glow -> the UI-space scale this struct stores (see the header:
    // a notecard should read like the panel that authored it). Exact
    // inverse of SSAtmoEnvApplier::applySky's forward conversion, which is
    // itself llpaneleditsky.cpp's SLIDER_SCALE_GLOW_R (20) and
    // SLIDER_SCALE_GLOW_B (-5): size = 2 - r/20, focus = b/-5. Round trip
    // on the stock packed glow (r 5.0, b -0.48): size 1.75, focus 0.096;
    // forward again gives (2 - 1.75) * 20 = 5.0 and 0.096 * -5 = -0.48.
    // The r channel round-trips bit-exactly (5/20 = 0.25 is a binary
    // fraction); b round-trips to float precision (0.48 and 0.096 are
    // not). The packed g channel is a renderer constant the applier
    // carries on its own (see SSAtmoEnvApplier::mGlowG) - nothing of it
    // belongs in the schema.
    const LLColor3 glow = sky.getGlow();
    mGlowSize  = SSAtmoEnvKeyframed<F32>(2.f - glow.mV[0] / 20.f);
    mGlowFocus = SSAtmoEnvKeyframed<F32>(glow.mV[2] / -5.f);

    // Nothing else is read on purpose. Sun/moon rotations, disc textures and scales are the Planetary tab's to compute from bodies (see the struct comment), and every cloud-layer field the sky
    // carries - colour included - is SSAtmoEnvCloudDome::fromSettingsSky's to read, seeded alongside this call.
}

void SSAtmoEnvAtmosphere::addKeyframesFromSky(const LLSettingsSky& sky, F64 phase)
{
    // Field set and conversions match fromSettingsSky above exactly - see there for the raw-ambiance read, the glow packing maths and why nothing positional is read. Only the write differs: a
    // keyframe stamped at `phase`, leaving whatever is authored elsewhere alone.
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

LLSD SSAtmoEnvAtmosphere::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["ambient"]        = mAmbientColor.asLLSD();
    sd["blue_horizon"]   = mBlueHorizon.asLLSD();
    sd["blue_density"]   = mBlueDensity.asLLSD();
    sd["sunlight_color"] = mSunlightColor.asLLSD();

    sd["haze_horizon"]        = mHazeHorizon.asLLSD();
    sd["haze_density"]        = mHazeDensity.asLLSD();
    // Same keys LLSettingsSky itself uses for these four, so a notecard and a settings asset read alike where they overlap.
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
    return sd;
}

bool SSAtmoEnvAtmosphere::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    // This key used to round-trip as an opaque LLSD blob "pending its own phase", and nothing ever wrote structured content into it - every saved notecard carries it as an empty map. So there is no
    // migration to do and no version bump to take: reading the known keys and ignoring anything else covers every asset that exists. Absent keys keep their constructor defaults, same as the water
    // struct - a hand-written notecard that only sets a haze value gets LLSettingsSky's stock sky for everything else, not black.
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
    return true;
}

//-----------------------------------------------------------------------------
// SSAtmoEnvTrack
//-----------------------------------------------------------------------------

LLSD SSAtmoEnvWeatherInfluence::asLLSD() const
{
    LLSD sd;
    sd["enabled"] = mEnabled;

    // Written flat rather than as a map per mapping: six pairs is not enough structure to be worth the nesting, and flat keys keep a hand-edited notecard readable.
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

bool SSAtmoEnvWeatherInfluence::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    // Every field keeps its constructed default when absent, so a notecard written before this struct existed loads as "influence on, everything at full strength" - which is what a fresh environment
    // gets too, so an older document and a new one behave alike rather than diverging on the strength of their own weather.
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

bool SSAtmoEnvTrack::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return false;

    if (sd.has("name")) mName = sd["name"].asString();
    if (sd.has("floor_z")) mFloorZ = (F32)sd["floor_z"].asReal();
    // ceiling_z was a stored field in the very first schema revision and is now derived (see SSAtmoEnvAsset::trackCeilingZ) - an older notecard still carrying one is simply ignored rather than
    // rejected.
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

F64 SSAtmoEnvTrack::currentDayCyclePhase() const
{
    if (mDayLengthSeconds <= 0.0) return 0.0;

    // Plain Unix epoch seconds: a universal wall clock every viewer already agrees on without needing to ask anything else for the time, which is the whole point - two people who load the same
    // notecard minutes apart should land on the same phase.
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
    // Calm, clear default: moisture/convection both 0 is "bone dry, clear skies" per the design doc's Weather tab.
    ground.mWeather = SSAtmoEnvWeather();

    // Water on by default at the ground track, at whatever height the current region's water actually sits - same fallback SSAtmoMagic's own voidWaterHeight() uses (20m, SL's default) when there's
    // no live region to ask. A freshly created ground track should look like the ordinary sea-level world it's standing in until an author changes it, not come up with an invisible/zero-height water
    // plane no one asked for.
    ground.mWater.mEnabled = true;
    {
        LLViewerRegion* region = gAgent.getRegion();
        ground.mWater.mHeight = SSAtmoEnvKeyframed<F32>(region ? region->getWaterHeight() : 20.f);
    }

    // Default new asset per the design doc: an Earth-sized planet as home, orbiting a 1-solar-mass sun. The sun is flagged as the one light emitter so a freshly created environment already has
    // *something* lighting it rather than a black sky by default. The planet's radius is exactly 1 AU so the floater's AU display reads a clean 1.00, and masses are the per-level units' own 1.0
    // (solar/Earth masses - see the header). Both names are autoNameBodies()'s ("Sol", "Sol I"), with mNameCustom left false so they keep tracking any restructuring.
    SSAtmoEnvCelestialBody sun;
    sun.mKind = SSAtmoEnvCelestialBody::SUN;
    sun.mParentIndex = -1;
    sun.mDiameterM = 1.392e9f;   // Sol, for scale
    sun.mMassRelative = 1.f;     // 1 solar mass
    sun.mIsLightEmitter = true;
    sun.mEmissive = true;
    sun.mPhaseShaded = false;
    // Authored disc art rather than EEP's built-in sun, which is a null texture id meaning "let the renderer draw its own" - a real texture is what lets the same body be drawn as a billboard when it
    // is not the one in the sun slot.
    sun.mCustomTexture = LLUUID(SSAtmoEnvCloudDome::BODY_TEXTURE_SUN);
    ground.mPlanetary.mBodies.push_back(sun);

    // Earth, transcribed rather than approximated - the default world is the one every author starts from, so it may as well be the one they already know the sky of. Real values also make the
    // derived machinery honest: 23.44 degrees of tilt is what puts the celestial pole where a temperate observer expects it, and the rise/set markers on the editor's scrubber then read as a real day
    // rather than an equinox at the equator.
    SSAtmoEnvCelestialBody home;
    home.mKind = SSAtmoEnvCelestialBody::PLANET;
    home.mParentIndex = 0; // the sun above
    home.mDiameterM = 1.2742e7f; // Earth, for scale
    home.mMassRelative = 1.f;    // 1 Earth mass
    home.mOrbitalRadius = 1.496e11f; // 1 AU - reads exactly 1.00 AU in the floater
    home.mAxialTiltDeg = 23.44f; // Earth's obliquity - the seasons
    home.mLatitudeDeg = 50.f;    // ...and where the observer stands on it
    // Earth's orbit IS the ecliptic, so its inclination is zero by definition in the usual frame - the number worth carrying is its 7.155 degrees against the Sun's own equator, which is the plane
    // bodies here orbit in. (At phase 0 an inclination has no effect at all - the orbit crosses the reference plane there - so this is carried for correctness rather than for anything it does
    // today.)
    home.mOrbitalInclinationDeg = 7.155f;
    // Phase 0, which in THIS model means equinox, not "the start of the year". A home body's orbital phase does not behave like a heliocentric longitude here: the reference plane contains the
    // rotation axis at zero tilt (see celestialAxis), so phase is really the angle between the sun and the celestial equator - a season dial. Phase 0 puts the sun ON the equator, rising due east and
    // culminating at 90 - tilt = 66.6 degrees, which is exactly an equinox at a temperate latitude and the most useful place to start authoring from. Real Earth's J2000 mean longitude of 100.46
    // degrees would put the sun 19 degrees from the celestial pole instead, which in this convention is polar night: a default world where the sun never rises at all.
    home.mOrbitalPhaseDeg = 0.f;
    home.mIsHome = true;
    ground.mPlanetary.mBodies.push_back(home);

    // ...and its moon, likewise real, and likewise a light emitter: EEP's moon slot goes to the smaller of the two emitters (see resolveLightRoles), so Luna lights the night while Sol keeps the sun
    // slot, and a fresh environment has both lights an author expects to find rather than a sun over an empty night sky.
    SSAtmoEnvCelestialBody moon;
    moon.mKind = SSAtmoEnvCelestialBody::MOON;
    moon.mParentIndex = 1; // the home planet above
    moon.mDiameterM = 3.4748e6f;     // Luna
    moon.mMassRelative = 1.f;        // 1 lunar mass - moons are measured in Lunas here
    moon.mOrbitalRadius = 3.844e8f;  // reads 1.00 Luna distance in the floater
    moon.mOrbitalInclinationDeg = 5.145f;
    // Thirty degrees off the planet's own orbital phase. Matching it exactly puts the moon on the far side of the planet from the sun - a full moon, which sounds ideal and is in fact a permanent
    // total lunar eclipse: dead on the anti-sun line, inside the planet's shadow, every hour of every day. Real Luna escapes that because its orbit is inclined and the nodes rarely line up; this
    // model is static, so it has to be placed off the line deliberately. Thirty degrees is a waxing gibbous - (1 + cos 30) / 2, so about 93% lit, still reading as "a full moon" - while putting it
    // around 190,000 km off the shadow axis, which is thirty times the planet's own radius and nowhere near the umbra.
    moon.mOrbitalPhaseDeg = 30.f;
    moon.mAxialTiltDeg = 6.68f;
    moon.mIsLightEmitter = true;
    moon.mCustomTexture = LLUUID(SSAtmoEnvCloudDome::BODY_TEXTURE_MOON);
    ground.mPlanetary.mBodies.push_back(moon);

    ground.mPlanetary.autoNameBodies();

    asset.mTracks.push_back(ground);
    return asset;
}

bool SSAtmoEnvAsset::addTrack()
{
    if ((S32)mTracks.size() >= SS_ATMOENV_MAX_TRACKS) return false;

    SSAtmoEnvTrack track;
    // A new track starts above whatever is currently highest, far enough up that its marker on the floater's rail can't collide with the one below it (the rail enforces a minimum separation of its
    // own and would otherwise refuse to place the marker), and clamped so it can never land on or above the region ceiling where it would have no band at all. The author can drag it anywhere
    // afterwards.
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
    // Names are the author's, so nothing is rewritten here - the survivors keep whatever they were called. Only the ordering is maintained.
    sortTracksByAltitude();
    return true;
}

std::string SSAtmoEnvAsset::nextDefaultTrackName() const
{
    // Lowest "Track N" not already taken, so adding a track after deleting one doesn't hand out a name another track is still using.
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

    // Ground stays at index 0 - it is the catch-all band rather than a peer with a floor worth sorting on, and callers (and the notecard format) treat index 0 as ground unconditionally.
    const SSAtmoEnvTrack* follow = (follow_index >= 0 && follow_index < (S32)mTracks.size())
        ? &mTracks[follow_index] : nullptr;
    const std::string follow_name = follow ? follow->mName : std::string();
    const bool follow_is_ground = (follow_index == 0);

    std::stable_sort(mTracks.begin() + 1, mTracks.end(),
        [](const SSAtmoEnvTrack& a, const SSAtmoEnvTrack& b) { return a.mFloorZ < b.mFloorZ; });

    if (follow_index < 0 || follow_is_ground) return follow_index;

    // Track the caller's selection across the reorder by name. Names are authored and can in principle collide (nothing stops two tracks both being called "Sky"), so this is a best effort - a
    // collision just leaves the selection on the first match, which is still a real track.
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
        // Strictly above: two tracks sharing a floor exactly is degenerate either way, and treating one as the other's ceiling would give it a zero-height band rather than just letting the
        // lower-indexed one win in trackContaining().
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

        // Water height is a tide now, so "lowest" is a question about a particular instant. Each track's own day cycle decides what that instant is for it - the same evaluation a live
        // (non-previewing) read of any other field on that track would do.
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
        track.fromLLSD(tracks_sd[i]);
        parsed.mTracks.push_back(track);
    }

    if (parsed.mTracks.empty())
    {
        out_error = "no tracks survived parsing";
        *this = makeDefault();
        return false;
    }

    // Names come from the notecard as authored - see nextDefaultTrackName() for why nothing is rewritten here. Ordering is normalised though, so a hand-edited notecard listing its tracks out of
    // altitude order still reads bottom-to-top in the floater.
    parsed.sortTracksByAltitude();

    *this = parsed;
    return true;
}

// </SS:Nexii>
