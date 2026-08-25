/**
 * @file ssatmoenvskymodulator.cpp
 * @brief Atmo Magic: weather -> sky modulation. See the header, and
 *        doc/atmo_magic_environment.md for the design and the table of
 *        which weather input reaches which EEP parameter.
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

// <SS:Nexii> Atmo Magic: weather -> sky modulation

namespace
{
    //-------------------------------------------------------------------
    // Full-effect constants. Each is what its mapping does at strength 1.0 with its input at the top of its range - so a strength is always "how much of this", never a raw parameter value, and an
    // author never has to know that EEP's haze density happens to run to 4.0. Retuning the model means editing these numbers and nothing else.
    //-------------------------------------------------------------------

    // Wind speed that reads as "as windy as this dial goes" - for the editor's own readout only. The scroll itself is derived from the wind in metres per second rather than from this ramp, so a gale
    // past it still scrolls faster than a breeze under it.
    const F32 SCROLL_FULL_WIND_MS = 25.f;

    // How hard a storm churns the cloud layer, in scroll-rate units on top of whatever the author set. EEP's stock scroll is 0.2, so half again is a sky visibly boiling without the texture tearing
    // across itself.
    const F32 CHURN_FULL_ADD = 0.5f;

    // Moisture's haze contribution. EEP haze density runs 0..4, and +1.5 over a clear-air baseline is the difference between "crisp" and "muggy" without erasing whatever the author set.
    const F32 HAZE_FULL_ADD = 1.5f;

    // ...and how far it closes the view in. A multiplier, not a subtraction, so it scales sanely whatever distance multiplier the author chose.
    const F32 DISTANCE_FULL_CUT = 0.5f;

    // Rain thickening the underwater fog. Also a multiplier, same reason.
    const F32 WATERFOG_FULL_BOOST = 0.5f;

    // Convection's storm darkening. Gamma and ambient are multipliers - a storm dims what is there rather than subtracting a fixed amount from a sky that might already be dark.
    const F32 GAMMA_FULL_CUT    = 0.5f;
    const F32 AMBIENT_FULL_CUT  = 0.6f;

    // How heavy a storm makes the cloud deck. A storm sky is a LOT of cloud, so convection lifts coverage the way okta cover does - toward this, never down from whatever the author set.
    const F32 STORM_COVER_FULL = 0.9f;

    // ...and how much churn it adds on top. Small, and deliberately so: EEP's cloud variance does not mean "more turbulent cloud", it displaces the noise lookup and then does cloudDensity *= 1 -
    // density_variance^2 (see cloudsF.glsl), so raising it ERODES the deck. Driving it hard for "boiling cumulus" is what made turning convection to maximum visibly pull the clouds back instead of
    // piling them up - the exact opposite of a storm sky. It buys texture at this size and nothing but holes above it.
    const F32 VARIANCE_FULL_ADD = 0.15f;

    // Convection below this is weather, not a storm: the ramp starts at the STABLE/BREEZY boundary's upper end (see SSAtmoEnvWeatherResolver::convectionPhase) so that ordinary breezy days do not dim
    // the world at all.
    const F32 DARKENING_ONSET = 0.55f;

    // Cold clear air. Ice level is 0..1 and adds; the blue-density tweak is the "crisper" half - a little more blue, a little less red.
    const F32 ICE_FULL_ADD        = 0.5f;
    const F32 BLUE_FULL_BOOST     = 0.20f;
    const F32 RED_FULL_CUT        = 0.10f;
    const F32 COLD_FULL_BELOW_C   = 15.f;  // degrees below freezing for full effect
    const F32 COLD_CLEAR_MOISTURE = 0.5f;  // moisture at which "clear air" is fully gone

    // Post-rain rainbows. The window is generous because it is a decay, not a duration: the effect is strongest the moment the rain stops and is barely there by the end.
    const F32 RAINBOW_WINDOW_S      = 240.f;
    const F32 RAINBOW_MIN_MOISTURE  = 0.3f;  // air has to still be wet enough to bow
    const F32 RAINBOW_FULL_MOISTURE = 0.75f; // sky moisture level at full effect

    // A rainbow needs the sun low: the bow's own radius means an observer above ~42 degrees of sun elevation is looking at a bow entirely below the horizon. Full effect below that, gone by 60 - a
    // soft edge rather than a cliff, since this is a look, not an ephemeris.
    const F32 RAINBOW_SUN_FULL_DEG = 42.f;
    const F32 RAINBOW_SUN_GONE_DEG = 60.f;

    // 0 below `lo`, 1 above `hi`, linear between. The one shaping function this file needs; every ramp above is stated in terms of it so they all behave alike at their edges.
    F32 ss_ramp(F32 value, F32 lo, F32 hi)
    {
        if (hi <= lo) return (value >= hi) ? 1.f : 0.f;
        return llclamp((value - lo) / (hi - lo), 0.f, 1.f);
    }

    // A mapping's effect amount: its own 0..1 drive, zeroed when the mapping is off and scaled by its strength. Every mapping goes through this, so "off" and "strength 0" are the same thing
    // everywhere and neither can be half-implemented in one place.
    F32 ss_effect(F32 drive, bool enabled, F32 strength)
    {
        if (!enabled) return 0.f;
        return llclamp(drive, 0.f, 1.f) * llclamp(strength, 0.f, 1.f);
    }
}

//-----------------------------------------------------------------------------
// The model
//-----------------------------------------------------------------------------

// static
SSAtmoEnvSkyModulation SSAtmoEnvSkyWeatherModulator::compute(const SSAtmoEnvSkyWeatherInput& in,
                                                             const SSAtmoEnvWeatherInfluence& influence)
{
    SSAtmoEnvSkyModulation mod;
    if (!influence.mEnabled) return mod; // identity - the authored sky, exactly

    // --- Okta cover -> cirrus dome coverage --------------------------- The resolver already grades 0-8 okta out of moisture for the forecast text; reusing it here is what makes the cirrus layer
    // agree with the words the floater is showing and with the volumetric storm layer, instead of being a third opinion about how cloudy it is.
    mod.mCoverTarget = llclamp((F32)in.mOktaCloudCover / 8.f, 0.f, 1.f);
    mod.mCoverBlend  = ss_effect(1.f, influence.mCloudCoverEnabled, influence.mCloudCoverStrength);

    // --- Wind -> cloud drift ------------------------------------------ Actual travel, published as a velocity in metres per second. The applier integrates it and the cloud vertex shader shifts the
    // whole layer by the result, in the same UV-per-metre terms the region parallax already uses - so the deck moves over the world the way a real one does, and every texture layer in it moves
    // together. Heading is a compass bearing (0 = north, 90 = east) naming where the wind blows TOWARD, matching the gust fronts the precipitation bridge already uses.
    {
        const F32 blend = ss_effect(1.f, influence.mWindScrollEnabled,
                                    influence.mWindScrollStrength);

        // Kept as a saturating ramp for the editor's readout only: the drift itself is linear in the actual wind, so a gale still outruns a breeze rather than both pinning at the top of a dial.
        mod.mWind = ss_effect(ss_ramp(in.mWindSpeedMS, 0.f, SCROLL_FULL_WIND_MS),
                              influence.mWindScrollEnabled, influence.mWindScrollStrength);

        const F32 heading_rad = in.mWindHeadingDeg * DEG_TO_RAD;
        mod.mDriftVelocity = LLVector2(sinf(heading_rad), cosf(heading_rad))
            * (in.mWindSpeedMS * blend);
    }

    // --- Moisture -> haze, and precipitation -> water fog -------------
    mod.mHaze   = ss_effect(in.mMoisture, influence.mHazeEnabled, influence.mHazeStrength);
    mod.mPrecip = ss_effect(in.mPrecipitationIntensity, influence.mHazeEnabled, influence.mHazeStrength);

    // --- Convection -> storm darkening --------------------------------
    mod.mDarkening = ss_effect(ss_ramp(in.mConvection, DARKENING_ONSET, 1.f),
                               influence.mStormDarkeningEnabled, influence.mStormDarkeningStrength);

    // Convection also churns the deck. Streamed along the wind rather than in a fixed direction: convective cells develop downwind, and an authored sky with no scroll of its own would otherwise
    // churn along an arbitrary axis picked here.
    {
        const F32 heading_rad = in.mWindHeadingDeg * DEG_TO_RAD;
        LLVector2 along(sinf(heading_rad), cosf(heading_rad));
        if (in.mWindSpeedMS < 0.05f) along = LLVector2(1.f, 0.f); // dead calm: any axis will do
        mod.setChurn(along);
    }

    // --- Cold clear air -> ice halos, crisper blue --------------------
    // Both halves have to hold: freezing fog is not a halo sky, so the
    // sub-freezing ramp is multiplied by how clear the air is.
    {
        const F32 cold  = ss_ramp(-in.mTemperatureC, 0.f, COLD_FULL_BELOW_C);
        const F32 clear = 1.f - ss_ramp(in.mMoisture, 0.f, COLD_CLEAR_MOISTURE);
        mod.mCold = ss_effect(cold * clear, influence.mColdSkyEnabled, influence.mColdSkyStrength);
    }

    // --- Post-rain rainbow -------------------------------------------- Three gates, all of which a real bow needs: rain has actually stopped (and recently), the air is still wet, and the sun is up
    // but low. Any one failing means no bow rather than a weak one - a bow with the sun overhead is not a faint bow, it is no bow.
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

//-----------------------------------------------------------------------------
// Applying it - each of these is "base value in, what the renderer gets out", and each is the identity on a default-constructed modulation.
//-----------------------------------------------------------------------------

F32 SSAtmoEnvSkyModulation::cloudCoverage(F32 base) const
{
    // Lift only: llmax against the authored value before blending, so the strength dial fades between "authored" and "authored, plus whatever weather piles on" - never between authored and less than
    // authored.
    F32 out = base + (llmax(base, mCoverTarget) - base) * mCoverBlend;

    // Storms pile cloud on as well as darkening it, and this is where that half of the mapping lives. It belongs to the storm dial rather than the cover one - "storms darken the sky" plainly means a
    // heavier deck, and an author who turns the forecast-cover mapping off has not asked for their thunderstorms to happen under a clear sky.
    out += (llmax(out, STORM_COVER_FULL) - out) * mDarkening;
    return out;
}

LLVector2 SSAtmoEnvSkyModulation::cloudScrollRate(const LLVector2& base) const
{
    return base + mScrollDelta;
}

// Convection's churn, worked out where the storm factor is - see the header on why churn is what a scroll rate actually is.
void SSAtmoEnvSkyModulation::setChurn(const LLVector2& along)
{
    mScrollDelta = along * (mDarkening * CHURN_FULL_ADD);
}

F32 SSAtmoEnvSkyModulation::cloudVariance(F32 base) const
{
    return llclamp(base + mDarkening * VARIANCE_FULL_ADD, 0.f, 1.f);
}

F32 SSAtmoEnvSkyModulation::hazeDensity(F32 base) const
{
    return base + mHaze * HAZE_FULL_ADD;
}

F32 SSAtmoEnvSkyModulation::distanceMultiplier(F32 base) const
{
    return base * (1.f - mHaze * DISTANCE_FULL_CUT);
}

F32 SSAtmoEnvSkyModulation::sceneGamma(F32 base) const
{
    return base * (1.f - mDarkening * GAMMA_FULL_CUT);
}

LLColor3 SSAtmoEnvSkyModulation::ambientColor(const LLColor3& base) const
{
    return base * (1.f - mDarkening * AMBIENT_FULL_CUT);
}

LLColor3 SSAtmoEnvSkyModulation::blueDensity(const LLColor3& base) const
{
    if (mCold <= 0.f) return base;

    LLColor3 out = base;
    out.mV[0] *= (1.f - mCold * RED_FULL_CUT);
    out.mV[2] *= (1.f + mCold * BLUE_FULL_BOOST);
    return out;
}

F32 SSAtmoEnvSkyModulation::skyIceLevel(F32 base) const
{
    return llclamp(base + mCold * ICE_FULL_ADD, 0.f, 1.f);
}

F32 SSAtmoEnvSkyModulation::skyMoistureLevel(F32 base) const
{
    // Lift, like coverage: an author who has set a high moisture level for a permanently bowed sky keeps it, and the rainbow window only ever adds to that.
    return llmax(base, base + (RAINBOW_FULL_MOISTURE - base) * mRainbow);
}

F32 SSAtmoEnvSkyModulation::waterFogModifier(F32 base) const
{
    return base * (1.f + mPrecip * WATERFOG_FULL_BOOST);
}

// </SS:Nexii>
