/**
 * @file ssatmoenvbridge.h
 * @brief Atmo Magic -> v2 renderer bridge. SSAtmoMagic::refreshParams()
 *        is the existing, working rain/wind/particle driver - it is being
 *        kept, not replaced (see doc/atmo_magic_environment.md's
 *        untangle plan). Rather than duplicate its easing, wind-drift, and
 *        rain-direction math for a second, v3-native renderer, this
 *        translates a v3 environment's resolved weather into the exact
 *        shape SSAtmoTrackConfig already is, so that math runs unmodified
 *        either way.
 *
 *        This whole file is scaffolding: once v2 is actually untangled and
 *        SSAtmoTrackConfig is gone, whatever SSAtmoMagic reads from lives
 *        natively in v3's own shape and this translation layer is deleted,
 *        not carried forward. It existing at all, in its own file, is what
 *        makes that deletion a one-file removal later rather than an
 *        archaeology exercise through a renderer that's been fed two
 *        formats for a while.
 *
 *        Spliced into SSAtmoMagic::refreshParams() once the rest of v3's
 *        data/logic layer had actually been exercised against a running
 *        client (create, load, revert, parcel discovery, caching) rather
 *        than only compiled - see refreshParams()'s own comment for
 *        exactly where the v2/v3 fork happens.
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

#ifndef SS_ATMOENVLEGACYBRIDGE_H
#define SS_ATMOENVLEGACYBRIDGE_H

// <SS:Nexii> Atmo Magic -> v2 renderer bridge (temporary, see file header)

#include "llpreprocessor.h"

#include <string>

struct SSAtmoTrackConfig;

class SSAtmoEnvBridge
{
public:
    // False (out_cfg untouched) if there is no v3 asset loaded at all - the
    // caller should fall through to the existing SSAtmoTrackManager path
    // unchanged in that case. True populates out_cfg as if it were a v2
    // notecard's track config for whichever v3 track is currently active at
    // world_z, including the transition-buffer fade toward zero precipitation
    // approaching a track boundary (the same "ease toward nothing rather
    // than blend two incompatible types" idiom SSAtmoMagic already uses for
    // a preset swap - see SSAtmoEnvTrackResolver's neighbour weight).
    //
    // The precipitation *type* is carried as a preset name in out_cfg.mPreset
    // (e.g. "rain" -> "Rain"), resolved the same way a v2 notecard's own
    // preset name is: SSPrecipPresetManager::find() with the existing "empty or
    // unrecognised name falls back to whatever's active" behaviour. A fresh
    // install's preset library not yet having a preset for every v3 type is
    // an asset/content gap, not a code one - it degrades to that fallback,
    // it does not fail.
    //
    // out_is_ground_track is whether the resolved track is v3's own index
    // 0 - the caller needs this to decide mSkyTrack, since v2's own track
    // number (whatever SSAtmoTrackManager::currentTrack() happens to report)
    // means nothing once v3 is what's actually active.
    static bool resolveActiveTrack(F32 world_z, F32 prev_world_z, bool teleported,
                                   SSAtmoTrackConfig& out_cfg, bool& out_is_ground_track);

private:
    // "rain" -> "Rain", etc. Empty in or empty out both mean "no override",
    // consistent with SSAtmoTrackConfig::mPreset's own empty-means-active
    // convention.
    static std::string presetNameForType(const std::string& v3_type);
};

// </SS:Nexii>

#endif // SS_ATMOENVLEGACYBRIDGE_H
