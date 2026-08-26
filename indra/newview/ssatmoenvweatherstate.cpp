/**
 * @file ssatmoenvweatherstate.cpp
 * @brief See ssatmoenvweatherstate.h.
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

#include "ssatmoenvweatherstate.h"

namespace
{
    const F32 CLEAR_MOISTURE_THRESHOLD = 0.02f;

    // Lerp.
    F32 ss_flerp(F32 a, F32 b, F32 t) { return a + (b - a) * t; }

    // Only liquid rain types get the drizzle bands.
    bool isDrizzleCapable(const std::string& type)
    {
        return type == "rain" || type == "freezing_rain";
    }

    // Cloud cover in oktas straight from moisture; below the clear threshold reads 0.
    S32 oktaFromMoisture(F32 moisture)
    {
        if (moisture <= CLEAR_MOISTURE_THRESHOLD) return 0;
        return llclamp((S32)llround(moisture * 8.0), 1, 8);
    }

    // Human sky wording per okta band.
    std::string skyTextForOkta(S32 okta)
    {
        if (okta <= 0) return "Clear sky";
        if (okta <= 2) return "Mostly clear";
        if (okta <= 4) return "Partly cloudy";
        if (okta <= 6) return "Cloudy";
        return "Overcast";
    }

    // First-letter capitalisation for forecast wording.
    std::string capitalized(std::string s)
    {
        if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
        return s;
    }
}

// Buckets convection into the four named phases the UI and the lightning defaults key on.
SSAtmoEnvWeatherState::EConvectionPhase SSAtmoEnvWeatherResolver::convectionPhase(F32 convection)
{
    if (convection <= 0.25f) return SSAtmoEnvWeatherState::STABLE;
    if (convection <= 0.55f) return SSAtmoEnvWeatherState::BREEZY;
    if (convection <= 0.75f) return SSAtmoEnvWeatherState::TURBULENT;
    return SSAtmoEnvWeatherState::SEVERE;
}

// Temperature picks the family, convection the severity: snow/blizzard below freezing through hail at extreme convection.
std::string SSAtmoEnvWeatherResolver::derivePrecipitationType(F32 convection, F32 temperature_c)
{
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
    return (convection > 0.95f) ? "hail" : "rain";
}

// Moisture into named intensity bands, with the extra drizzle bands only for liquid types.
SSAtmoEnvPrecipIntensity SSAtmoEnvWeatherResolver::classifyIntensity(F32 moisture, const std::string& type)
{
    if (moisture <= CLEAR_MOISTURE_THRESHOLD) return SSAtmoEnvPrecipIntensity::NONE;

    if (isDrizzleCapable(type))
    {
        if (moisture <= 0.06f) return SSAtmoEnvPrecipIntensity::DRIZZLE_LIGHT;
        if (moisture <= 0.10f) return SSAtmoEnvPrecipIntensity::DRIZZLE;
        if (moisture <= 0.15f) return SSAtmoEnvPrecipIntensity::DRIZZLE_HEAVY;
        if (moisture <= 0.35f) return SSAtmoEnvPrecipIntensity::LIGHT;
        if (moisture <= 0.65f) return SSAtmoEnvPrecipIntensity::MODERATE;
        if (moisture <= 0.85f) return SSAtmoEnvPrecipIntensity::HEAVY;
        return SSAtmoEnvPrecipIntensity::TORRENTIAL;
    }

    if (moisture <= 0.35f) return SSAtmoEnvPrecipIntensity::LIGHT;
    if (moisture <= 0.65f) return SSAtmoEnvPrecipIntensity::MODERATE;
    if (moisture <= 0.85f) return SSAtmoEnvPrecipIntensity::HEAVY;
    return SSAtmoEnvPrecipIntensity::TORRENTIAL;
}

// Forecast wording for a type at an intensity band.
std::string SSAtmoEnvWeatherResolver::intensityLabel(const std::string& type, SSAtmoEnvPrecipIntensity band)
{
    using I = SSAtmoEnvPrecipIntensity;
    if (band == I::NONE) return std::string();

    if (type == "rain")
    {
        switch (band)
        {
            case I::DRIZZLE_LIGHT: return "light drizzle";
            case I::DRIZZLE:       return "drizzle";
            case I::DRIZZLE_HEAVY: return "heavy drizzle";
            case I::LIGHT:         return "light rain";
            case I::MODERATE:      return "rain";
            case I::HEAVY:         return "heavy rain";
            case I::TORRENTIAL:    return "torrential rain";
            default: break;
        }
    }
    else if (type == "freezing_rain")
    {
        switch (band)
        {
            case I::DRIZZLE_LIGHT: return "light freezing drizzle";
            case I::DRIZZLE:       return "freezing drizzle";
            case I::DRIZZLE_HEAVY: return "heavy freezing drizzle";
            case I::LIGHT:         return "light freezing rain";
            case I::MODERATE:      return "freezing rain";
            case I::HEAVY:         return "heavy freezing rain";
            case I::TORRENTIAL:    return "severe freezing rain";
            default: break;
        }
    }
    else if (type == "snow")
    {
        switch (band)
        {
            case I::LIGHT:      return "light snow";
            case I::MODERATE:   return "snow";
            case I::HEAVY:      return "heavy snow";
            case I::TORRENTIAL: return "intense snowfall";
            default: return "snow";
        }
    }
    else if (type == "hail")
    {
        switch (band)
        {
            case I::LIGHT:      return "light hail";
            case I::MODERATE:   return "hail";
            case I::HEAVY:      return "heavy hail";
            case I::TORRENTIAL: return "severe hailstorm";
            default: return "hail";
        }
    }
    else if (type == "sleet")
    {
        switch (band)
        {
            case I::LIGHT:      return "light sleet";
            case I::MODERATE:   return "sleet";
            default:            return "heavy sleet";
        }
    }
    else if (type == "slush_mix")
    {
        return (band == I::LIGHT) ? "light wintry mix" : "wintry mix";
    }
    else if (type == "blizzard")
    {
        return "blizzard conditions";
    }

    return type;
}

// One human sentence for the HUD: precipitation (or sky cover) plus wind strength.
std::string SSAtmoEnvWeatherResolver::generateForecastText(const SSAtmoEnvWeatherState& state, F32 moisture)
{
    std::string precip;
    if (moisture <= CLEAR_MOISTURE_THRESHOLD)
    {
        precip = skyTextForOkta(state.mOktaCloudCover);
    }
    else if (state.mConvectionPhase == SSAtmoEnvWeatherState::SEVERE)
    {
        const bool snowy = (state.mPrecipitationType == "snow" || state.mPrecipitationType == "blizzard");
        precip = snowy ? "Thundersnow" : "Thundery showers";
    }
    else
    {
        precip = capitalized(intensityLabel(state.mPrecipitationType, state.mIntensityBand));
    }

    std::string wind;
    if (state.mWindSpeed < 1.f)       wind = "still air";
    else if (state.mWindSpeed < 3.f)  wind = "light winds";
    else if (state.mWindSpeed < 7.f)  wind = "a gentle breeze";
    else if (state.mWindSpeed < 12.f) wind = "brisk winds";
    else if (state.mWindSpeed < 20.f) wind = "strong winds";
    else                              wind = "gale-force winds";

    return precip + " and " + wind;
}

// The whole weather cube for one phase: type, intensity, gusts, lightning behaviour and forecast text from moisture/convection/temperature.
SSAtmoEnvWeatherState SSAtmoEnvWeatherResolver::resolve(const SSAtmoEnvWeather& weather, F64 phase)
{
    SSAtmoEnvWeatherState state;

    const F32 moisture    = weather.mMoisture.valueAt(phase);
    const F32 convection  = weather.mConvection.valueAt(phase);
    const F32 temperature = weather.mTemperatureC.valueAt(phase);

    state.mConvectionPhase = convectionPhase(convection);
    state.mWindHeading = weather.mWindHeading.valueAt(phase);
    state.mWindSpeed   = weather.mWindSpeed.valueAt(phase);

    state.mOktaCloudCover = oktaFromMoisture(moisture);

    if (moisture <= CLEAR_MOISTURE_THRESHOLD)
    {
        state.mPrecipitationType = std::string();
        state.mPrecipitationIntensity = 0.f;
        state.mIntensityBand = SSAtmoEnvPrecipIntensity::NONE;
        state.mDropletSizeScale = 0.f;
        state.mImpactScale = 0.f;
    }
    else
    {
        const std::string override_type = weather.mPrecipitationOverride.valueAt(phase);
        state.mPrecipitationType = !override_type.empty()
            ? override_type
            : derivePrecipitationType(convection, temperature);
        state.mPrecipitationIntensity = moisture;
        state.mIntensityBand = classifyIntensity(moisture, state.mPrecipitationType);

        switch (state.mIntensityBand)
        {
            using I = SSAtmoEnvPrecipIntensity;
            case I::DRIZZLE_LIGHT: state.mDropletSizeScale = 0.05f; state.mImpactScale = 0.00f; break;
            case I::DRIZZLE:       state.mDropletSizeScale = 0.12f; state.mImpactScale = 0.00f; break;
            case I::DRIZZLE_HEAVY: state.mDropletSizeScale = 0.20f; state.mImpactScale = 0.05f; break;
            case I::LIGHT:         state.mDropletSizeScale = 0.35f; state.mImpactScale = 0.25f; break;
            case I::MODERATE:      state.mDropletSizeScale = 0.55f; state.mImpactScale = 0.50f; break;
            case I::HEAVY:         state.mDropletSizeScale = 0.75f; state.mImpactScale = 0.80f; break;
            case I::TORRENTIAL:    state.mDropletSizeScale = 1.00f; state.mImpactScale = 1.00f; break;
            default:                state.mDropletSizeScale = 0.f;   state.mImpactScale = 0.f;   break;
        }
    }

    if (weather.mGustAuto)
    {
        state.mGustDepth  = convection;
        state.mGustLength = ss_flerp(220.f, 80.f, convection);
        state.mGustVeer   = ss_flerp(4.f, 35.f, convection);
    }
    else
    {
        state.mGustDepth  = weather.mGustDepth.valueAt(phase);
        state.mGustLength = weather.mGustLength.valueAt(phase);
        state.mGustVeer   = weather.mGustVeer.valueAt(phase);
    }

    state.mLightningColor = weather.mLightningColor.valueAt(phase);
    state.mLightningCoreWhite = llclamp(weather.mLightningCoreWhite.valueAt(phase), 0.f, 1.f);

    state.mLightningEnabled = weather.mLightningEnabled;
    state.mLightningCharge  = weather.mLightningCharge;
    state.mLightningSparks  = weather.mLightningSparks;

    if (weather.mLightningAuto)
    {
        switch (state.mConvectionPhase)
        {
            case SSAtmoEnvWeatherState::TURBULENT:
                state.mLightningIntervalMinSeconds = 30.f;
                state.mLightningIntervalMaxSeconds = 60.f;
                state.mLightningIntensity = convection;
                break;
            case SSAtmoEnvWeatherState::SEVERE:
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
        state.mLightningIntensity = weather.mLightningIntensity.valueAt(phase);
        if (state.mConvectionPhase == SSAtmoEnvWeatherState::TURBULENT)
        {
            state.mLightningIntervalMinSeconds = 30.f;
            state.mLightningIntervalMaxSeconds = 60.f;
        }
        else if (state.mConvectionPhase == SSAtmoEnvWeatherState::SEVERE)
        {
            state.mLightningIntervalMinSeconds = 2.f;
            state.mLightningIntervalMaxSeconds = 5.f;
        }
    }

    state.mForecastText = generateForecastText(state, moisture);
    return state;
}
