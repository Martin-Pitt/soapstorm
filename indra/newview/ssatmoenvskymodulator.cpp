/**
 * @file ssatmoenvskymodulator.cpp
 * @brief See ssatmoenvskymodulator.h.
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

#include "ssatmoenvskymodulator.h"

#include <cmath>

namespace
{

    const F32 SCROLL_FULL_WIND_MS = 25.f;

    const F32 CHURN_FULL_ADD = 0.5f;

    const F32 HAZE_FULL_ADD = 1.5f;

    const F32 DISTANCE_FULL_CUT = 0.5f;

    const F32 WATERFOG_FULL_BOOST = 0.5f;

    const F32 GAMMA_FULL_CUT    = 0.5f;
    const F32 AMBIENT_FULL_CUT  = 0.6f;

    const F32 DARKENING_ONSET = 0.55f;

    const F32 ICE_FULL_ADD        = 0.5f;
    const F32 BLUE_FULL_BOOST     = 0.20f;
    const F32 RED_FULL_CUT        = 0.10f;
    const F32 COLD_FULL_BELOW_C   = 15.f;
    const F32 COLD_CLEAR_MOISTURE = 0.5f;

    const F32 RAINBOW_WINDOW_S      = 240.f;
    const F32 RAINBOW_MIN_MOISTURE  = 0.3f;
    const F32 RAINBOW_FULL_MOISTURE = 0.75f;

    const F32 RAINBOW_SUN_FULL_DEG = 42.f;
    const F32 RAINBOW_SUN_GONE_DEG = 60.f;

    // Linear ramp of value across [lo, hi] to 0..1.
    F32 ss_ramp(F32 value, F32 lo, F32 hi)
    {
        if (hi <= lo) return (value >= hi) ? 1.f : 0.f;
        return llclamp((value - lo) / (hi - lo), 0.f, 1.f);
    }

    // A drive gated by its influence toggle and scaled by its strength.
    F32 ss_effect(F32 drive, bool enabled, F32 strength)
    {
        if (!enabled) return 0.f;
        return llclamp(drive, 0.f, 1.f) * llclamp(strength, 0.f, 1.f);
    }
}

// Turns the weather cube plus the author's influence settings into this frame's set of sky transforms.
SSAtmoEnvSkyModulation SSAtmoEnvSkyWeatherModulator::compute(const SSAtmoEnvSkyWeatherInput& in,
                                                             const SSAtmoEnvWeatherInfluence& influence)
{
    SSAtmoEnvSkyModulation mod;
    if (!influence.mEnabled) return mod;

    // <SS:Nexii> The dome's overcast band tracks the main deck's live coverage - the same number
    // the puffs render with, so band and deck can never disagree about how overcast it is. The
    // okta count this replaces was the forecast's wording stepped into the render: every eighth of
    // moisture popped the band 12.5% in one frame. Okta stays where wording belongs, in the
    // forecast text; authored coverage below stays a floor (the cirrus duty), so the target only
    // ever lifts.
    mod.mCoverTarget = llclamp(in.mDeckCoverage, 0.f, 1.f);
    mod.mCoverBlend  = ss_effect(1.f, influence.mCloudCoverEnabled, influence.mCloudCoverStrength);

    {
        const F32 blend = ss_effect(1.f, influence.mWindScrollEnabled,
                                    influence.mWindScrollStrength);

        mod.mWind = ss_effect(ss_ramp(in.mWindSpeedMS, 0.f, SCROLL_FULL_WIND_MS),
                              influence.mWindScrollEnabled, influence.mWindScrollStrength);

        const F32 heading_rad = in.mWindHeadingDeg * DEG_TO_RAD;
        mod.mDriftVelocity = LLVector2(sinf(heading_rad), cosf(heading_rad))
            * (in.mWindSpeedMS * blend);
    }

    mod.mHaze   = ss_effect(in.mMoisture, influence.mHazeEnabled, influence.mHazeStrength);
    mod.mPrecip = ss_effect(in.mPrecipitationIntensity, influence.mHazeEnabled, influence.mHazeStrength);

    mod.mDarkening = ss_effect(ss_ramp(in.mConvection, DARKENING_ONSET, 1.f),
                               influence.mStormDarkeningEnabled, influence.mStormDarkeningStrength);

    {
        const F32 heading_rad = in.mWindHeadingDeg * DEG_TO_RAD;
        LLVector2 along(sinf(heading_rad), cosf(heading_rad));
        if (in.mWindSpeedMS < 0.05f) along = LLVector2(1.f, 0.f);
        mod.setChurn(along);
    }

    {
        const F32 cold  = ss_ramp(-in.mTemperatureC, 0.f, COLD_FULL_BELOW_C);
        const F32 clear = 1.f - ss_ramp(in.mMoisture, 0.f, COLD_CLEAR_MOISTURE);
        mod.mCold = ss_effect(cold * clear, influence.mColdSkyEnabled, influence.mColdSkyStrength);
    }

    if (in.mSecondsSinceRainStopped >= 0.f && in.mSunElevationSin > 0.f)
    {
        const F32 decay = 1.f - ss_ramp(in.mSecondsSinceRainStopped, 0.f, RAINBOW_WINDOW_S);
        const F32 wet   = ss_ramp(in.mMoisture, RAINBOW_MIN_MOISTURE * 0.5f, RAINBOW_MIN_MOISTURE);
        const F32 elevation_deg = RAD_TO_DEG * asinf(llclamp(in.mSunElevationSin, -1.f, 1.f));
        const F32 low_sun = 1.f - ss_ramp(elevation_deg, RAINBOW_SUN_FULL_DEG, RAINBOW_SUN_GONE_DEG);

        mod.mRainbow = ss_effect(decay * wet * low_sun,
                                 influence.mRainbowEnabled, influence.mRainbowStrength);
    }

    return mod;
}

// Blends authored coverage toward the deck's live coverage, lift only: weather can pile cloud on,
// but a sky authored overcast stays overcast in fair weather. The old storm-cover push is gone with
// the okta drive - a band that overcasts past the deck it tracks is exactly the disagreement the
// tracking exists to prevent; a storm's weight comes from the deck's own gloom, thickness and the
// gamma/ambient cuts below.
F32 SSAtmoEnvSkyModulation::cloudCoverage(F32 base) const
{
    return base + (llmax(base, mCoverTarget) - base) * mCoverBlend;
}

// Authored scroll plus the churn delta.
LLVector2 SSAtmoEnvSkyModulation::cloudScrollRate(const LLVector2& base) const
{
    return base + mScrollDelta;
}

// Storm churn: extra scroll along the wind, scaled by darkening.
void SSAtmoEnvSkyModulation::setChurn(const LLVector2& along)
{
    mScrollDelta = along * (mDarkening * CHURN_FULL_ADD);
}

// Moisture adds haze.
F32 SSAtmoEnvSkyModulation::hazeDensity(F32 base) const
{
    return base + mHaze * HAZE_FULL_ADD;
}

// Haze pulls the fog distance in.
F32 SSAtmoEnvSkyModulation::distanceMultiplier(F32 base) const
{
    return base * (1.f - mHaze * DISTANCE_FULL_CUT);
}

// Storm darkening flattens gamma.
F32 SSAtmoEnvSkyModulation::sceneGamma(F32 base) const
{
    return base * (1.f - mDarkening * GAMMA_FULL_CUT);
}

// Storm darkening cuts ambient.
LLColor3 SSAtmoEnvSkyModulation::ambientColor(const LLColor3& base) const
{
    return base * (1.f - mDarkening * AMBIENT_FULL_CUT);
}

// Cold clear sky shifts blue density up and red down.
LLColor3 SSAtmoEnvSkyModulation::blueDensity(const LLColor3& base) const
{
    if (mCold <= 0.f) return base;

    LLColor3 out = base;
    out.mV[0] *= (1.f - mCold * RED_FULL_CUT);
    out.mV[2] *= (1.f + mCold * BLUE_FULL_BOOST);
    return out;
}

// Cold adds sky ice.
F32 SSAtmoEnvSkyModulation::skyIceLevel(F32 base) const
{
    return llclamp(base + mCold * ICE_FULL_ADD, 0.f, 1.f);
}

// A live rainbow raises sky moisture toward its full value.
F32 SSAtmoEnvSkyModulation::skyMoistureLevel(F32 base) const
{
    return llmax(base, base + (RAINBOW_FULL_MOISTURE - base) * mRainbow);
}

// Precipitation thickens water fog.
F32 SSAtmoEnvSkyModulation::waterFogModifier(F32 base) const
{
    return base * (1.f + mPrecip * WATERFOG_FULL_BOOST);
}
