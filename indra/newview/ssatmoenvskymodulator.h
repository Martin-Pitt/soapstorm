/**
 * @file ssatmoenvskymodulator.h
 * @brief Atmo Magic: the weather cube's influence on the rendered sky.
 *        A pure function - derived weather in, a set of value transforms
 *        out - sitting between the applier's keyframe evaluation and its
 *        setters. See doc/atmo_magic_environment.md.
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

// <SS:Nexii> Atmo Magic: weather -> sky modulation

#include "ssatmoenvasset.h"

#include "v2math.h"
#include "v3color.h"

// Everything the modulation depends on, gathered by the caller. Explicit
// rather than "here is the track, work it out": the derivation stays a
// pure function of stated inputs, which is what makes it testable and what
// keeps every formula in one file.
struct SSAtmoEnvSkyWeatherInput
{
    // Raw cube values at the current phase. The derived
    // SSAtmoEnvWeatherState carries bands and categories, which are the
    // right thing for particles and forecast text and the wrong thing
    // here - a sky that steps between four convection phases reads as a
    // bug, so the continuous values drive the ramps.
    F32 mMoisture = 0.f;        // 0..1
    F32 mConvection = 0.f;      // 0..1
    F32 mTemperatureC = 15.f;

    F32 mWindHeadingDeg = 0.f;  // 0 = north, 90 = east
    F32 mWindSpeedMS = 0.f;

    // The cloud layer's own geometry, needed to turn a wind speed in metres
    // per second into a scroll rate: how far away the layer is and how far
    // its texture is stretched both decide how fast a given wind LOOKS.
    // Authored values at the current phase - see cloudScrollRate().
    F32 mMaxAltitudeM = 1605.f;
    F32 mCloudScale = 0.42f;

    // From the resolved state, where the resolver has already done work
    // worth not repeating.
    S32 mOktaCloudCover = 0;             // 0..8
    F32 mPrecipitationIntensity = 0.f;   // 0..1

    // Rainbow gating, the one piece of the model that is not a function of
    // the cube alone (see SSAtmoEnvSkyModulation::skyMoistureLevel).
    // Negative means "not applicable": it is raining now, or no rain has
    // stopped since this environment was loaded.
    F32 mSecondsSinceRainStopped = -1.f;
    F32 mSunElevationSin = 0.f;          // sin(elevation) of the sun right now
};

// The computed transform. Default-constructed it is the IDENTITY - every
// method hands back exactly what it was given - so the applier can call
// these unconditionally instead of scattering "is weather influence on"
// branches through every setter.
//
// Methods rather than a bag of factors because the mapping from "how
// stormy is it" to "what does haze density become" is the interesting part
// and belongs next to the constants it uses, not at each call site.
struct SSAtmoEnvSkyModulation
{
    // --- Clouds / Sky Dome -------------------------------------------
    // Authored coverage is a FLOOR, never a ceiling: weather can pile
    // cloud on, but a sky authored overcast stays overcast in fair
    // weather. An author who wants clear skies to actually clear the
    // dome can keyframe the coverage down - the point of modulation is
    // that the authored value keeps its meaning.
    F32 cloudCoverage(F32 base) const;

    // EEP's cloud scroll rate is CHURN, not travel. The accumulated scroll is
    // added to cloud_pos_density1 and to nothing else (llsettingsvo.cpp), so
    // it slides the large cloud texture against the small one that stays put
    // - the pattern boils and reforms in place rather than the sky moving.
    // Convection drives it for exactly that reason; wind does not, because
    // wind does not churn a cloud, it carries it.
    //
    // What the author set is the still-air baseline, and this adds to it.
    LLVector2 cloudScrollRate(const LLVector2& base) const;

    F32 cloudVariance(F32 base) const;

    // --- Atmosphere ---------------------------------------------------
    F32 hazeDensity(F32 base) const;
    F32 distanceMultiplier(F32 base) const;
    F32 sceneGamma(F32 base) const;
    LLColor3 ambientColor(const LLColor3& base) const;
    LLColor3 blueDensity(const LLColor3& base) const;
    F32 skyIceLevel(F32 base) const;

    // EEP's own rainbow/halo driver. Spikes when rain has just stopped
    // with the sun still up - the only mapping needing state beyond the
    // current instant, which is why mSecondsSinceRainStopped is an input
    // rather than something this file remembers.
    F32 skyMoistureLevel(F32 base) const;

    // --- Water --------------------------------------------------------
    F32 waterFogModifier(F32 base) const;

    // Fills mScrollDelta from the storm factor, along the given axis. Called
    // by the modulator once mDarkening is known.
    void setChurn(const LLVector2& along);

    // Effect amounts, each already multiplied by its mapping's strength
    // and zero when that mapping is off. Public because the editor shows
    // them: an author tuning "storm darkening" wants to see what it is
    // doing right now, the same way the Auto weather fields show their
    // computed values.
    F32 mCoverTarget = 0.f;    // 0..1, the coverage weather is asking for
    F32 mCoverBlend = 0.f;     // 0..1, how far toward it to go
    F32 mWind = 0.f;           // 0..1, wind's share of the drift, for the readout
    LLVector2 mDriftVelocity;  // metres/second the whole cloud layer travels
    LLVector2 mScrollDelta;    // added to authored scroll - churn, not travel
    F32 mHaze = 0.f;           // 0..1, moisture's haze contribution
    F32 mPrecip = 0.f;         // 0..1, precipitation's water-fog contribution
    F32 mDarkening = 0.f;      // 0..1, convection's storm darkening
    F32 mCold = 0.f;           // 0..1, sub-freezing clear-air factor
    F32 mRainbow = 0.f;        // 0..1, post-rain rainbow window
};

class SSAtmoEnvSkyWeatherModulator
{
public:
    // The whole model. Pure: same inputs, same output, no reads of global
    // state, no clock - the applier owns the clock and hands the result in
    // via mSecondsSinceRainStopped.
    //
    // With influence.mEnabled false this returns the identity, so "off"
    // costs nothing and means exactly the authored sky.
    static SSAtmoEnvSkyModulation compute(const SSAtmoEnvSkyWeatherInput& in,
                                          const SSAtmoEnvWeatherInfluence& influence);
};

// </SS:Nexii>

#endif // SS_ATMOENVSKYMODULATOR_H
