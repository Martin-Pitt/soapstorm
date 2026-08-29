/**
 * @file ssatmoenvskymodulator.h
 * @brief Atmo Magic: weather-to-sky modulation.
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

#ifndef SS_ATMOENVSKYMODULATOR_H
#define SS_ATMOENVSKYMODULATOR_H

#include "ssatmoenvasset.h"

#include "v2math.h"
#include "v3color.h"

struct SSAtmoEnvSkyWeatherInput
{
    F32 mMoisture = 0.f;
    F32 mConvection = 0.f;
    F32 mTemperatureC = 15.f;

    F32 mWindHeadingDeg = 0.f;
    F32 mWindSpeedMS = 0.f;

    F32 mMaxAltitudeM = 1605.f;
    F32 mCloudScale = 0.42f;

    // <SS:Nexii> The main deck's live coverage (SSVolCloud::lastCoverage). The dome's overcast band
    // tracks this instead of deriving its own opinion about how cloudy it is - one coverage, two
    // layers. One frame stale at worst; the dome altitude derivation already accepts that.
    F32 mDeckCoverage = 0.f;

    F32 mPrecipitationIntensity = 0.f;

    F32 mSecondsSinceRainStopped = -1.f;
    F32 mSunElevationSin = 0.f;
};

struct SSAtmoEnvSkyModulation
{
    F32 cloudCoverage(F32 base) const;

    LLVector2 cloudScrollRate(const LLVector2& base) const;

    F32 hazeDensity(F32 base) const;
    F32 distanceMultiplier(F32 base) const;
    F32 sceneGamma(F32 base) const;
    LLColor3 ambientColor(const LLColor3& base) const;
    LLColor3 blueDensity(const LLColor3& base) const;
    F32 skyIceLevel(F32 base) const;

    F32 skyMoistureLevel(F32 base) const;

    F32 waterFogModifier(F32 base) const;

    void setChurn(const LLVector2& along);

    F32 mCoverTarget = 0.f;
    F32 mCoverBlend = 0.f;
    F32 mWind = 0.f;
    LLVector2 mDriftVelocity;
    LLVector2 mScrollDelta;
    F32 mHaze = 0.f;
    F32 mPrecip = 0.f;
    F32 mDarkening = 0.f;
    F32 mCold = 0.f;
    F32 mRainbow = 0.f;
};

class SSAtmoEnvSkyWeatherModulator
{
public:
    static SSAtmoEnvSkyModulation compute(const SSAtmoEnvSkyWeatherInput& in,
                                          const SSAtmoEnvWeatherInfluence& influence);
};

#endif
