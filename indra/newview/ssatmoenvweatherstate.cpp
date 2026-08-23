/**
 * @file ssatmoenvweatherstate.cpp
 * @brief Atmo Magic weather cube derivation - see the design doc's
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

#include "ssatmoenvweatherstate.h"

// <SS:Nexii> Atmo Magic: weather cube -> derived state

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

    bool isDrizzleCapable(const std::string& type)
    {
        // Drizzle is a droplet-size phenomenon specific to liquid
        // precipitation - snow's lightest form is just light snow, not
        // "snow drizzle". Freezing drizzle is real (METAR: FZDZ, distinct
        // from FZRA/freezing rain) even though it renders identically to
        // freezing rain here per the agreed simplification - same visual
        // type, the difference is only in droplet size and where the
        // banding sits.
        return type == "rain" || type == "freezing_rain";
    }

    // WMO/METAR okta scale: 0 SKC (clear), 1-2 FEW, 3-4 SCT (scattered),
    // 5-7 BKN (broken), 8 OVC (overcast). Derived from moisture as a
    // coverage proxy - see the header note on this being provisional until
    // it is the same number the volumetric cloud field (phase 7) computes.
    S32 oktaFromMoisture(F32 moisture)
    {
        if (moisture <= CLEAR_MOISTURE_THRESHOLD) return 0;
        return llclamp((S32)llround(moisture * 8.0), 1, 8);
    }

    std::string skyTextForOkta(S32 okta)
    {
        if (okta <= 0) return "Clear sky";
        if (okta <= 2) return "Mostly clear";
        if (okta <= 4) return "Partly cloudy";
        if (okta <= 6) return "Cloudy";
        return "Overcast";
    }

    std::string capitalized(std::string s)
    {
        if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
        return s;
    }
}

// static
SSAtmoEnvWeatherState::EConvectionPhase SSAtmoEnvWeatherResolver::convectionPhase(F32 convection)
{
    if (convection <= 0.25f) return SSAtmoEnvWeatherState::STABLE;
    if (convection <= 0.55f) return SSAtmoEnvWeatherState::BREEZY;
    if (convection <= 0.75f) return SSAtmoEnvWeatherState::TURBULENT;
    return SSAtmoEnvWeatherState::SEVERE;
}

// static
std::string SSAtmoEnvWeatherResolver::derivePrecipitationType(F32 convection, F32 temperature_c)
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
SSAtmoEnvPrecipIntensity SSAtmoEnvWeatherResolver::classifyIntensity(F32 moisture, const std::string& type)
{
    if (moisture <= CLEAR_MOISTURE_THRESHOLD) return SSAtmoEnvPrecipIntensity::NONE;

    if (isDrizzleCapable(type))
    {
        // A finer grade than the original two-band (Light/Heavy) split,
        // which read as an abrupt jump rather than a gradient - drizzle
        // itself gets three sub-bands too, per the note that drizzle is
        // graded the same way rain is, not just "the step before rain".
        if (moisture <= 0.06f) return SSAtmoEnvPrecipIntensity::DRIZZLE_LIGHT;
        if (moisture <= 0.10f) return SSAtmoEnvPrecipIntensity::DRIZZLE;
        if (moisture <= 0.15f) return SSAtmoEnvPrecipIntensity::DRIZZLE_HEAVY;
        if (moisture <= 0.35f) return SSAtmoEnvPrecipIntensity::LIGHT;
        if (moisture <= 0.65f) return SSAtmoEnvPrecipIntensity::MODERATE;
        if (moisture <= 0.85f) return SSAtmoEnvPrecipIntensity::HEAVY;
        return SSAtmoEnvPrecipIntensity::TORRENTIAL;
    }

    // Snow/hail/sleet/slush_mix/blizzard: no drizzle-equivalent stage -
    // "drizzle" specifically describes a liquid droplet size, and light
    // snow already covers the same "barely falling" end of the range.
    if (moisture <= 0.35f) return SSAtmoEnvPrecipIntensity::LIGHT;
    if (moisture <= 0.65f) return SSAtmoEnvPrecipIntensity::MODERATE;
    if (moisture <= 0.85f) return SSAtmoEnvPrecipIntensity::HEAVY;
    return SSAtmoEnvPrecipIntensity::TORRENTIAL;
}

// static
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
            default:            return "heavy sleet"; // HEAVY and TORRENTIAL read the same - sleet doesn't graduate further
        }
    }
    else if (type == "slush_mix")
    {
        return (band == I::LIGHT) ? "light wintry mix" : "wintry mix";
    }
    else if (type == "blizzard")
    {
        return "blizzard conditions"; // always - see generateForecastText, this is a type call, not an intensity one
    }

    return type; // unrecognised type - degrade to the bare type name rather than an empty string
}

// static
std::string SSAtmoEnvWeatherResolver::generateForecastText(const SSAtmoEnvWeatherState& state, F32 moisture)
{
    std::string precip;
    if (moisture <= CLEAR_MOISTURE_THRESHOLD)
    {
        precip = skyTextForOkta(state.mOktaCloudCover);
    }
    else if (state.mConvectionPhase == SSAtmoEnvWeatherState::SEVERE)
    {
        // Thunder gets first billing at this phase regardless of type -
        // it's the defining feature of a supercell-grade sky, snow or rain.
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

// static
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

    // Type and intensity are deliberately separate axes: moisture decides
    // whether anything is falling at all, convection/temperature decide
    // what it is once something is, and moisture again decides how much
    // (the graded intensity band) once there's a type to grade.
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

        // Coarse-graded (per band, not continuous with moisture) - see the
        // header note: a future particle system regenerates its drop
        // texture only when this category changes, not every frame
        // moisture drifts a fraction.
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
        state.mLightningIntensity = weather.mLightningIntensity;
        // An explicit override still respects the Stable/Breezy "never"
        // rule - overriding the *intensity* is not the same as overriding
        // whether lightning is possible at all in a flat overcast.
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

// </SS:Nexii>
