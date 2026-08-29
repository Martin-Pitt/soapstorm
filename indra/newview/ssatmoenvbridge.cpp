/**
 * @file ssatmoenvbridge.cpp
 * @brief See ssatmoenvbridge.h.
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

#include "ssatmoenvbridge.h"

#include "ssatmotrack.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvtrackstate.h"
#include "ssatmoenvweatherstate.h"

// Maps a v3 precipitation type to the v2 preset name the legacy renderer keys on.
std::string SSAtmoEnvBridge::presetNameForType(const std::string& v3_type)
{
    if (v3_type.empty())          return std::string();
    if (v3_type == "rain")        return "Rain";
    if (v3_type == "snow")        return "Snow";
    if (v3_type == "hail")        return "Hail";
    if (v3_type == "blizzard")    return "Blizzard";
    if (v3_type == "sleet")       return "Sleet";
    if (v3_type == "freezing_rain") return "Freezing Rain";
    if (v3_type == "slush_mix")   return "Wintry Mix";

    // <SS:Nexii> Anything else is a type the environment carries under its own authored name - the
    // derivation vocabulary above is only the shipped set. Handing the name straight through lets
    // SSPrecipPresetManager::find() decide: it resolves an environment type staged by
    // ssAtmoEnvStagePrecipTypes(), and a name that resolves to nothing falls back to the active
    // preset exactly as an empty string used to. See doc/atmo_magic_env_ui.md.
    return v3_type;
}

// Translates the active v3 track's resolved weather into a v2 SSAtmoTrackConfig so the existing renderer runs unmodified.
bool SSAtmoEnvBridge::resolveActiveTrack(F32 world_z, F32 prev_world_z, bool teleported,
                                               SSAtmoTrackConfig& out_cfg, bool& out_is_ground_track)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return false;

    const SSAtmoEnvAsset& asset = mgr->asset();

    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(asset, world_z, prev_world_z, teleported);
    const SSAtmoEnvTrack& track = asset.mTracks[blend.mPrimaryTrack];
    out_is_ground_track = (blend.mPrimaryTrack == 0);

    const F64 phase = mgr->hasPreviewPhaseOverride() ? mgr->previewPhaseOverride()
                                                     : track.currentDayCyclePhase();
    const SSAtmoEnvWeatherState state = SSAtmoEnvWeatherResolver::resolve(track.mWeather, phase);

    out_cfg = SSAtmoTrackConfig();
    out_cfg.mDefined = true;
    out_cfg.mEnabled = true;

    out_cfg.mPreset = presetNameForType(state.mPrecipitationType);

    const F32 neighbor_fade = (blend.mNeighborTrack >= 0) ? (1.f - blend.mNeighborWeight) : 1.f;

    out_cfg.mPrecipitation = llclamp(state.mPrecipitationIntensity, 0.f, 1.f) * neighbor_fade;
    out_cfg.mTurbulence = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);

    out_cfg.mTemperatureC = llclamp(track.mWeather.mTemperatureC.valueAt(phase), -60.f, 60.f);

    out_cfg.mLightningColor = state.mLightningColor;
    out_cfg.mLightningCoreWhite = state.mLightningCoreWhite;

    out_cfg.mLightning = state.mLightningEnabled;
    out_cfg.mLightningCharge = state.mLightningCharge;
    out_cfg.mLightningSparks = state.mLightningSparks;
    out_cfg.mLightningIntervalMin = state.mLightningIntervalMinSeconds;
    out_cfg.mLightningIntervalMax = state.mLightningIntervalMaxSeconds;
    out_cfg.mLightningIntensity = state.mLightningIntensity;

    out_cfg.mGustDepth = llclamp(state.mGustDepth, 0.f, 3.f);
    out_cfg.mGustLength = llclamp(state.mGustLength, 8.f, 2000.f);
    out_cfg.mGustVeer = llclamp(state.mGustVeer, 0.f, 90.f);

    out_cfg.setHeadingElevation(state.mWindHeading, 0.f);
    out_cfg.mWindSpeed = llmax(0.f, state.mWindSpeed);

    out_cfg.mHasGround = true;
    out_cfg.mGround = track.mFloorZ;

    out_cfg.mFallThrough = 1.f;

    return true;
}
