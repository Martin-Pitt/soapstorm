/**
 * @file ssatmov3weatherstate.cpp
 * @brief Atmo Magic v3 weather cube derivation - see the design doc's
 *        Weather tab for the source of every threshold and formula here.
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

#include "ssatmov3weatherstate.h"

// <SS:Nexii> Atmo Magic v3: weather cube -> derived state

namespace
{
    // Moisture below this reads as "bone dry, clear skies" per the design
    // doc, rather than a technically-nonzero drizzle nobody would notice.
    const F32 CLEAR_MOISTURE_THRESHOLD = 0.02f;

    // Named to avoid colliding with the overload set of LL's own global
    // lerp() (LLColor4/LLVector3/... variants pulled in transitively via
    // llviewerprecompiledheaders.h), which a plain "lerp(F32,F32,F32)" here
    // was ambiguous against.
    F32 ss_flerp(F32 a, F32 b, F32 t) { return a + (b - a) * t; }
}

// static
SSAtmoV3WeatherState::EConvectionPhase SSAtmoV3WeatherResolver::convectionPhase(F32 convection)
{
    if (convection <= 0.25f) return SSAtmoV3WeatherState::STABLE;
    if (convection <= 0.55f) return SSAtmoV3WeatherState::BREEZY;
    if (convection <= 0.75f) return SSAtmoV3WeatherState::TURBULENT;
    return SSAtmoV3WeatherState::SEVERE;
}

// static
std::string SSAtmoV3WeatherResolver::derivePrecipitationType(F32 convection, F32 temperature_c)
{
    // Deliberately a pure function of the instantaneous cube, not a state
    // machine - see the design doc's note on why the "Or:" formula was
    // chosen over the alternative: a transitionary type (freezing rain
    // decaying into sleet, say) would need memory of what it used to be,
    // and every frame re-deriving from scratch avoids that entirely.
    if (temperature_c < -1.f)
    {
        return (convection > 0.7f) ? "blizzard" : "snow";
    }
    if (temperature_c <= 0.f)
    {
        return (convection > 0.5f) ? "freezing_rain" : "sleet";
    }
    if (temperature_c <= 1.5f)
    {
        return "slush_mix";
    }
    return (convection > 0.8f) ? "hail" : "rain";
}

// static
std::string SSAtmoV3WeatherResolver::generateForecastText(const SSAtmoV3WeatherState& state, F32 moisture)
{
    std::string precip;
    if (moisture <= CLEAR_MOISTURE_THRESHOLD)
    {
        precip = "Clear sky";
    }
    else if (state.mConvectionPhase == SSAtmoV3WeatherState::SEVERE)
    {
        // Thunder gets first billing at this phase regardless of type -
        // it's the defining feature of a supercell-grade sky, snow or rain.
        if (state.mPrecipitationType == "snow" || state.mPrecipitationType == "blizzard")
        {
            precip = "Thundersnow";
        }
        else
        {
            precip = "Thundery showers";
        }
    }
    else if (state.mPrecipitationType == "blizzard")       precip = "Blizzard conditions";
    else if (state.mPrecipitationType == "snow")           precip = (moisture > 0.6f) ? "Heavy snowfall" : "Light snow";
    else if (state.mPrecipitationType == "freezing_rain")  precip = "Freezing rain";
    else if (state.mPrecipitationType == "sleet")          precip = "Sleet";
    else if (state.mPrecipitationType == "slush_mix")      precip = "Wintry mix";
    else if (state.mPrecipitationType == "hail")           precip = "Hailstorm";
    else if (state.mPrecipitationType == "rain")           precip = (moisture > 0.6f) ? "Heavy rain" : "Light rain";
    else                                                   precip = "Overcast";

    std::string wind;
    if (state.mWindSpeed < 1.f)       wind = "still air";
    else if (state.mWindSpeed < 3.f)  wind = "light winds";
    else if (state.mWindSpeed < 7.f)  wind = "a gentle breeze";
    else if (state.mWindSpeed < 12.f) wind = "brisk winds";
    else                              wind = "gale-force winds";

    return precip + " and " + wind;
}

// static
SSAtmoV3WeatherState SSAtmoV3WeatherResolver::resolve(const SSAtmoV3Weather& weather, F64 time)
{
    SSAtmoV3WeatherState state;

    const F32 moisture    = weather.mMoisture.valueAt(time);
    const F32 convection  = weather.mConvection.valueAt(time);
    const F32 temperature = weather.mTemperatureC.valueAt(time);

    state.mConvectionPhase = convectionPhase(convection);
    state.mWindHeading = weather.mWindHeading.valueAt(time);
    state.mWindSpeed   = weather.mWindSpeed.valueAt(time);

    // Type and intensity are deliberately separate axes: moisture decides
    // whether anything is falling at all, convection/temperature decide
    // what it is once something is.
    if (moisture <= CLEAR_MOISTURE_THRESHOLD)
    {
        state.mPrecipitationType = std::string();
        state.mPrecipitationIntensity = 0.f;
    }
    else
    {
        const std::string override_type = weather.mPrecipitationOverride.valueAt(time);
        state.mPrecipitationType = !override_type.empty()
            ? override_type
            : derivePrecipitationType(convection, temperature);
        state.mPrecipitationIntensity = moisture;
    }

    // Gust: auto-derived from convection unless the track overrides it.
    // Longer, gentler gaps at low convection; tight, wide-swinging bursts
    // as it climbs toward supercell - the "boiling/churning" feel the
    // design doc describes.
    if (weather.mGustAuto)
    {
        state.mGustDepth  = convection;
        state.mGustLength = ss_flerp(220.f, 80.f, convection);
        state.mGustVeer   = ss_flerp(4.f, 35.f, convection);
    }
    else
    {
        state.mGustDepth  = weather.mGustDepth;
        state.mGustLength = weather.mGustLength;
        state.mGustVeer   = weather.mGustVeer;
    }

    // Lightning: Stable/Breezy never strike regardless of override, since
    // there is nothing to override toward - the design doc is explicit that
    // lightning probability is strictly 0% below the Turbulent phase.
    if (weather.mLightningAuto)
    {
        switch (state.mConvectionPhase)
        {
            case SSAtmoV3WeatherState::TURBULENT:
                state.mLightningIntervalMinSeconds = 30.f;
                state.mLightningIntervalMaxSeconds = 60.f;
                state.mLightningIntensity = convection;
                break;
            case SSAtmoV3WeatherState::SEVERE:
                state.mLightningIntervalMinSeconds = 2.f;
                state.mLightningIntervalMaxSeconds = 5.f;
                state.mLightningIntensity = convection;
                break;
            default:
                state.mLightningIntervalMinSeconds = 0.f;
                state.mLightningIntervalMaxSeconds = 0.f;
                state.mLightningIntensity = 0.f;
                break;
        }
    }
    else
    {
        state.mLightningIntensity = weather.mLightningIntensity;
        // An explicit override still respects the Stable/Breezy "never"
        // rule - overriding the *intensity* is not the same as overriding
        // whether lightning is possible at all in a flat overcast.
        if (state.mConvectionPhase == SSAtmoV3WeatherState::TURBULENT)
        {
            state.mLightningIntervalMinSeconds = 30.f;
            state.mLightningIntervalMaxSeconds = 60.f;
        }
        else if (state.mConvectionPhase == SSAtmoV3WeatherState::SEVERE)
        {
            state.mLightningIntervalMinSeconds = 2.f;
            state.mLightningIntervalMaxSeconds = 5.f;
        }
    }

    state.mForecastText = generateForecastText(state, moisture);
    return state;
}

// </SS:Nexii>
