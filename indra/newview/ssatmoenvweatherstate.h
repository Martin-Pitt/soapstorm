/**
 * @file ssatmoenvweatherstate.h
 * @brief Atmo Magic: derives the actual precipitation type/intensity,
 *        gust and lightning behaviour, and convection phase from a track's
 *        authored moisture/convection/temperature cube. Phase 5 of
 *        doc/atmo_magic_environment.md's Weather tab - this is the pure
 *        formula side only. Wiring the existing rain/particle renderer
 *        (SSAtmoMagic/SSPrecipSim et al.) to actually read from this is a
 *        separate, later step: that renderer is being kept, not replaced,
 *        so it deserves its own careful pass rather than a guess bundled in
 *        here.
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

#ifndef SS_ATMOENVWEATHERSTATE_H
#define SS_ATMOENVWEATHERSTATE_H

// <SS:Nexii> Atmo Magic: weather cube -> derived state

#include "ssatmoenvasset.h"

#include <string>

// Precipitation intensity band, from moisture, within whichever type
// derivePrecipitationType() picked. METAR-inspired (its "-"/none/"+"
// intensity prefixes), extended to a finer grade since "just two bands"
// (the original Light/Heavy split) was reading as an abrupt jump rather
// than a gradient - see doc/atmo_magic_environment.md and BBC-style
// forecast wording for the reference point. Drizzle is its own graded
// sub-range below Light, not a separate type - it and rain differ by
// droplet size (see mDropletSizeScale) more than by anything else here,
// matching the "same visual type, different droplet size" simplification
// already agreed for freezing drizzle vs. freezing rain.
enum class SSAtmoEnvPrecipIntensity
{
    NONE = 0,
    DRIZZLE_LIGHT,
    DRIZZLE,
    DRIZZLE_HEAVY,
    LIGHT,
    MODERATE,
    HEAVY,
    TORRENTIAL
};

// One evaluation of a track's weather cube at a point in time - everything
// downstream (particles, clouds, audio) reads this rather than the raw
// cube, so the moisture/convection/temperature -> "what actually happens"
// formulas live in exactly one place.
struct SSAtmoEnvWeatherState
{
    // Empty means clear - no precipitation at all. Otherwise one of
    // "rain"/"hail"/"snow"/"blizzard"/"freezing_rain"/"sleet"/"slush_mix",
    // per the mid-fall precipitation-type formula. mIntensityBand is the
    // separate, moisture-driven axis within whichever of these this picked -
    // "rain" + HEAVY is what used to just be "rain" on its own.
    std::string mPrecipitationType;
    F32 mPrecipitationIntensity = 0.f; // 0..1, from moisture directly - the continuous form mIntensityBand is graded from
    SSAtmoEnvPrecipIntensity mIntensityBand = SSAtmoEnvPrecipIntensity::NONE;

    // 0..1, "how big are the drops/flakes right now" - meant for a future
    // particle system's texture selection. Deliberately coarse-graded
    // (quantized to a handful of steps, not continuous with moisture) per
    // the original design note: regenerate the actual drop texture only
    // when this category changes and it's been a while, not every frame
    // moisture drifts a little; individual drops can still resize freely
    // within a category.
    F32 mDropletSizeScale = 0.f;

    // 0..1, how visible a splash/ripple should be on impact - drizzle-scale
    // drops barely disturb a puddle, so this is not just mDropletSizeScale
    // under another name: it is weighted toward the heavy end, near-zero
    // through the whole drizzle range, per your own note that ripple
    // impacts aren't really noticeable until the drops are.
    F32 mImpactScale = 0.f;

    // Okta cloud cover, 0-8 - the actual WMO/METAR scale (0 SKC, 1-2 FEW,
    // 3-4 SCT, 5-7 BKN, 8 OVC), derived from moisture as a coverage proxy.
    // Feeds the forecast text's "overcast"/"scattered clouds"/etc wording;
    // not wired to the volumetric cloud field's own coverage (phase 7)
    // yet, though that is the same number in spirit and the two should
    // eventually just be one calculation.
    S32 mOktaCloudCover = 0;

    enum EConvectionPhase
    {
        STABLE = 0,     // 0.0-0.25: flat overcast, no lightning
        BREEZY = 1,     // 0.25-0.55: rolling shapes, angled fall, no lightning
        TURBULENT = 2,  // 0.55-0.75: boiling cumulus, lightning every 30-60s
        SEVERE = 3      // 0.75-1.0: pitch-black churn, lightning every 2-5s
    };
    EConvectionPhase mConvectionPhase = STABLE;

    F32 mWindHeading = 0.f;
    F32 mWindSpeed   = 0.f;

    F32 mGustDepth  = 0.f;
    F32 mGustLength = 140.f;
    F32 mGustVeer   = 0.f;

    // Both zero means no lightning at all (Stable/Breezy). Otherwise the
    // random-interval range a caller should schedule strikes within.
    F32 mLightningIntervalMinSeconds = 0.f;
    F32 mLightningIntervalMaxSeconds = 0.f;
    F32 mLightningIntensity = 0.f;

    // Purely a display convenience - a compact natural-language reading of
    // what the moisture/convection/temperature sliders currently add up to
    // (e.g. "Thundery showers and a gentle breeze"), recomputed fresh every
    // call. Never persisted: it isn't part of SSAtmoEnvWeather's own schema,
    // only ever lives in a resolved-for-right-now SSAtmoEnvWeatherState.
    std::string mForecastText;
};

class SSAtmoEnvWeatherResolver
{
public:
    // Evaluates the cube (via SSAtmoEnvKeyframed::valueAt(time, day_length_seconds))
    // and derives everything above from it. Explicit per-field overrides
    // (mPrecipitationOverride, mGustAuto/mLightningAuto false) win over the
    // derivation for their own field, exactly as authored. day_length_seconds
    // is the owning track's own SSAtmoEnvTrack::mDayLengthSeconds - passed
    // through rather than read from the track directly so this stays a
    // pure function of its own two inputs.
    static SSAtmoEnvWeatherState resolve(const SSAtmoEnvWeather& weather, F64 time, F64 day_length_seconds);

    // Broken out because both the resolver and the floater's forecast-text
    // preview want it without evaluating the rest of the cube.
    static SSAtmoEnvWeatherState::EConvectionPhase convectionPhase(F32 convection);

    // The mid-fall precipitation-type formula (temperature/convection only
    // - moisture decides *whether* anything falls, not what kind). Public
    // so a forced override string can still be validated against the same
    // vocabulary this produces.
    static std::string derivePrecipitationType(F32 convection, F32 temperature_c);

    // moisture -> intensity band, gated by whether `type` is one of the
    // drizzle-capable types ("rain", "freezing_rain") - snow/hail/sleet/
    // slush_mix skip straight to LIGHT, since "drizzle" is specifically a
    // liquid-droplet-size phenomenon, not a light dusting of anything else.
    static SSAtmoEnvPrecipIntensity classifyIntensity(F32 moisture, const std::string& type);

    // "-"/""/"+" METAR-style, English label ("Drizzle", "Light Rain",
    // "Heavy Snow", ...) for a (type, band) pair - the one place that
    // decides how a band reads out loud, so the forecast text and any
    // future UI label agree with each other by construction.
    static std::string intensityLabel(const std::string& type, SSAtmoEnvPrecipIntensity band);

private:
    static std::string generateForecastText(const SSAtmoEnvWeatherState& state, F32 moisture);
};

// </SS:Nexii>

#endif // SS_ATMOENVWEATHERSTATE_H
