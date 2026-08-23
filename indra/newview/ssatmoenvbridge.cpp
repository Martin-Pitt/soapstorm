/**
 * @file ssatmoenvbridge.cpp
 * @brief Atmo Magic -> v2 renderer bridge implementation. See the header
 *        for why this file exists and why it's deliberately not called yet.
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

#include "ssatmotrack.h" // for SSAtmoTrackConfig
#include "ssatmoenvmanager.h"
#include "ssatmoenvtrackstate.h"
#include "ssatmoenvweatherstate.h"

// <SS:Nexii> Atmo Magic -> v2 renderer bridge (temporary, see header)

// static
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
    // Unrecognised - SSAtmoTrackConfig::mPreset's own empty-means-active
    // convention handles this the same as a v2 notecard naming a preset
    // that isn't installed.
    return std::string();
}

// static
bool SSAtmoEnvBridge::resolveActiveTrack(F32 world_z, F32 prev_world_z, bool teleported,
                                               SSAtmoTrackConfig& out_cfg, bool& out_is_ground_track)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return false;

    const SSAtmoEnvAsset& asset = mgr->asset();

    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(asset, world_z, prev_world_z, teleported);
    const SSAtmoEnvTrack& track = asset.mTracks[blend.mPrimaryTrack];
    out_is_ground_track = (blend.mPrimaryTrack == 0);

    // The Atmo Magic floater's preview time slider overrides the real
    // wall clock while it's open and visible, the same way EEP's Edit Day
    // Cycle floater overrides LLEnvironment's live time - see
    // SSAtmoEnvManager::setPreviewPhaseOverride().
    const F64 phase = mgr->hasPreviewPhaseOverride() ? mgr->previewPhaseOverride()
                                                     : track.currentDayCyclePhase();
    const SSAtmoEnvWeatherState state = SSAtmoEnvWeatherResolver::resolve(track.mWeather, phase);

    out_cfg = SSAtmoTrackConfig();
    out_cfg.mDefined = true;
    out_cfg.mEnabled = true;

    out_cfg.mPreset = presetNameForType(state.mPrecipitationType);

    // Ease toward nothing approaching a track boundary rather than blend
    // two tracks' weather together - the same "a preset can't be
    // interpolated" idiom SSAtmoMagic already applies to a preset swap,
    // applied here to a track swap instead. An instant cut (teleport,
    // sit-teleport, region-position-change, or a water crossing) skips
    // this entirely: mNeighborTrack stays -1 whenever mInstantCut is set,
    // so the multiply below is a no-op in that case.
    const F32 neighbor_fade = (blend.mNeighborTrack >= 0) ? (1.f - blend.mNeighborWeight) : 1.f;

    out_cfg.mPrecipitation = llclamp(state.mPrecipitationIntensity, 0.f, 1.f) * neighbor_fade;
    out_cfg.mTurbulence = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);

    out_cfg.mGustDepth = llclamp(state.mGustDepth, 0.f, 3.f);
    out_cfg.mGustLength = llclamp(state.mGustLength, 8.f, 2000.f);
    out_cfg.mGustVeer = llclamp(state.mGustVeer, 0.f, 90.f);

    // v3 dropped wind elevation entirely to keep authoring simple - see the
    // design doc's Weather tab - so elevation is always flat here.
    out_cfg.setHeadingElevation(state.mWindHeading, 0.f);
    out_cfg.mWindSpeed = llmax(0.f, state.mWindSpeed);

    // v3 tracks always have an authored floor (there is no separate
    // "pin to a platform" flag the way v2's sky tracks need one) - so this
    // is unconditionally the resolved track's own floor.
    out_cfg.mHasGround = true;
    out_cfg.mGround = track.mFloorZ;

    // Not yet an authored v3 field - see doc/atmo_magic_environment.md's
    // Track schema. Defaulting to fully drawn rather than 0 so open-air v3
    // tracks are not silently invisible until this gets its own control.
    out_cfg.mFallThrough = 1.f;

    return true;
}

// </SS:Nexii>
