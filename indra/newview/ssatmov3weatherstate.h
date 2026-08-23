/**
 * @file ssatmov3weatherstate.h
 * @brief Atmo Magic v3: derives the actual precipitation type/intensity,
 *        gust and lightning behaviour, and convection phase from a track's
 *        authored moisture/convection/temperature cube. Phase 5 of
 *        doc/atmo_magic_v3_environment.md's Weather tab - this is the pure
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

#ifndef SS_ATMOV3WEATHERSTATE_H
#define SS_ATMOV3WEATHERSTATE_H

// <SS:Nexii> Atmo Magic v3: weather cube -> derived state

#include "ssatmov3asset.h"

#include <string>

// One evaluation of a track's weather cube at a point in time - everything
// downstream (particles, clouds, audio) reads this rather than the raw
// cube, so the moisture/convection/temperature -> "what actually happens"
// formulas live in exactly one place.
struct SSAtmoV3WeatherState
{
    // Empty means clear - no precipitation at all. Otherwise one of
    // "drizzle"/"rain"/"hail"/"snow"/"blizzard"/"freezing_rain"/"sleet"/
    // "slush_mix", per the mid-fall precipitation-type formula.
    std::string mPrecipitationType;
    F32 mPrecipitationIntensity = 0.f; // 0..1, from moisture directly

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
    // call. Never persisted: it isn't part of SSAtmoV3Weather's own schema,
    // only ever lives in a resolved-for-right-now SSAtmoV3WeatherState.
    std::string mForecastText;
};

class SSAtmoV3WeatherResolver
{
public:
    // Evaluates the cube (via SSAtmoV3Keyframed::valueAt(time)) and derives
    // everything above from it. Explicit per-field overrides
    // (mPrecipitationOverride, mGustAuto/mLightningAuto false) win over the
    // derivation for their own field, exactly as authored.
    static SSAtmoV3WeatherState resolve(const SSAtmoV3Weather& weather, F64 time);

    // Broken out because both the resolver and the floater's forecast-text
    // preview want it without evaluating the rest of the cube.
    static SSAtmoV3WeatherState::EConvectionPhase convectionPhase(F32 convection);

    // The mid-fall precipitation-type formula (temperature/convection only
    // - moisture decides *whether* anything falls, not what kind). Public
    // so a forced override string can still be validated against the same
    // vocabulary this produces.
    static std::string derivePrecipitationType(F32 convection, F32 temperature_c);

private:
    static std::string generateForecastText(const SSAtmoV3WeatherState& state, F32 moisture);
};

// </SS:Nexii>

#endif // SS_ATMOV3WEATHERSTATE_H
