/**
 * @file ssatmotrack.h
 * @brief Atmo Magic per-track weather: EEP splits the region into sky tracks
 *        by altitude, and weather is configured and run per track, so a rainy
 *        ground level can sit under a clear skybox band. Configuration comes
 *        from a notecard referenced in the parcel description, because the
 *        server side cannot be extended to carry it in the environment itself.
 *
 *        Editing follows the environment floaters: a loaded notecard is the
 *        baseline, local edits sit on top of it and are marked with an
 *        asterisk, and they can be reverted or saved back out to a notecard.
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

#ifndef SS_ATMOTRACK_H
#define SS_ATMOTRACK_H

// <SS:Nexii> Atmo Magic per-track weather

#include "llassettype.h"
#include "llextendedstatus.h"
#include "llquaternion.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "llviewerparcelmgr.h"
#include "v3math.h"

#include <array>
#include <string>

class LLInventoryItem;

// LLEnvironment::calculateSkyTrackForAltitude returns 1..4: track 1 is ground
// level, 2..4 are the sky bands delimited by the region's altitude settings.
// Track 0 is EEP's water track and carries no sky, so weather ignores it.
const S32 SS_TRACK_MIN   = 1;
const S32 SS_TRACK_MAX   = 4;
const S32 SS_TRACK_COUNT = 4;

// One track's weather. Nothing is defined by default: a track only produces
// weather once a config explicitly turns it on, so arriving anywhere without
// an Atmo notecard leaves the sky exactly as it is today.
struct SSAtmoTrackConfig
{
    bool mDefined = false;      // a config exists at all for this track
    bool mEnabled = false;      // ...and it asks for weather

    std::string mPreset;        // preset name; empty falls back to the active one
    F32 mPrecipitation = 0.5f;
    F32 mTurbulence    = 0.3f;

    // Wind direction as a rotation off north. A quaternion rather than a
    // compass angle so the direction can carry elevation too - updraughts and
    // downdraughts are just a tilted wind - and so it composes with the rest
    // of the viewer's orientation maths. Strength stays in mWindSpeed.
    LLQuaternion mWindRot;
    F32 mWindSpeed = 4.f;       // m/s

    // How the wind arrives rather than how hard it blows. The flowmap solves a
    // steady wind and layers these on top of it as waves carried along with the
    // air, so a surge crosses the region downwind at wind speed instead of the
    // whole build gusting in unison. Depth is scaled by mTurbulence, so a calm
    // track stays a steady draught however these are set.
    F32 mGustDepth  = 1.f;      // 0 steady, 1 near-lull to about double
    F32 mGustLength = 140.f;    // metres between fronts, along the wind
    F32 mGustVeer   = 14.f;     // degrees the wind swings as a front passes

    // Sky tracks need a floor to fall onto. By default that is the track's
    // own base altitude (its "ground zero"), but a config can pin it to a
    // platform's actual height when the band's base sits well below the build.
    bool mHasGround = false;
    F32  mGround    = 0.f;

    // Fraction of drops still drawn where nothing catches them. Sky tracks are
    // mostly empty air, so precipitation that finds no platform thins out
    // instead of pouring onto an imaginary plane.
    F32 mFallThrough = 1.f;

    bool runs() const { return mDefined && mEnabled; }

    // Unit vector the wind blows toward, north (+Y) rotated by mWindRot
    LLVector3 windDirection() const;

    // Compass helpers for the UI, degrees. Heading 0 is north, 90 is east;
    // elevation is positive upward.
    F32  heading() const;
    F32  elevation() const;
    void setHeadingElevation(F32 heading_deg, F32 elevation_deg);

    LLSD asLLSD() const;
    void fromLLSD(const LLSD& sd);

    bool operator==(const SSAtmoTrackConfig& rhs) const;
    bool operator!=(const SSAtmoTrackConfig& rhs) const { return !(*this == rhs); }
};

typedef std::array<SSAtmoTrackConfig, SS_TRACK_COUNT> ss_track_set_t;

// Loads and owns the per-track configuration.
//
// The parcel description is watched through LLParcelObserver rather than
// polled: descriptions almost never change, and the observer fires both when
// the agent crosses into a different parcel and when the current parcel's
// properties are re-sent after an edit.
class SSAtmoTrackManager : public LLSingleton<SSAtmoTrackManager>, public LLParcelObserver
{
    LLSINGLETON(SSAtmoTrackManager);
    ~SSAtmoTrackManager();

public:
    // Where the baseline came from
    enum ESource
    {
        SOURCE_DEFAULT = 0, // nothing loaded; system defaults
        SOURCE_PARCEL,      // a notecard referenced by the parcel description
        SOURCE_INVENTORY    // a notecard loaded or dropped by hand
    };

    // LLParcelObserver: agent parcel changed, or its properties were re-sent
    void changed() override;

    // Per-frame; only does deferred first-time load now that parcel discovery
    // is event driven.
    void idle();

    // Track the camera is in right now, clamped into 1..4
    S32 currentTrack() const;

    // The working config: the baseline plus whatever has been edited on top.
    // This is what the weather actually runs from.
    const SSAtmoTrackConfig& config(S32 track) const;
    const SSAtmoTrackConfig& active() const { return config(currentTrack()); }

    // Editable working copy. Call commit() after changing it.
    SSAtmoTrackConfig& editable(S32 track);
    void commit();

    // Edited away from the baseline; drives the asterisk in the floater
    bool isModified() const;
    bool isModified(S32 track) const;

    // Back to the loaded notecard, or to system defaults when none is loaded
    void revertToBaseline();
    void resetToDefaults();

    // Base and top altitude of a track band. The floor doubles as ground zero
    // for sky tracks: precipitation spawns above it and lands on it, rather
    // than falling all the way to the terrain thousands of metres below.
    F32 trackFloor(S32 track) const;
    F32 trackCeiling(S32 track) const;

    // Ground level precipitation still lands on terrain and water; only the
    // sky bands get a synthetic floor that drops can fall through.
    bool isSkyTrack(S32 track) const { return track > SS_TRACK_MIN; }

    ESource source() const { return mSource; }
    const std::string& statusText() const { return mStatus; }
    const std::string& configName() const { return mConfigName; }
    const LLUUID& configAsset() const { return mAssetID; }

    // Re-read the parcel description and refetch whatever it points at
    void reload();

    // Load a notecard the user picked or dropped onto the floater
    bool importFromInventory(const LLInventoryItem* item);

    // Serialise the working set and write it to a fresh inventory notecard
    void exportToNotecard(const std::string& name);

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd, ss_track_set_t& out) const;

private:
    void requestNotecard(const LLUUID& asset_id, ESource source, const std::string& name);
    void applyNotecardText(const std::string& text);
    void adoptBaseline(const ss_track_set_t& set, ESource source, const std::string& name);
    void loadWorking();
    void saveWorking();

    static void onNotecardLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                 void* user_data, S32 status, LLExtStat ext_status);

    // Parses "atmo:<uuid>" out of a parcel description, in any position and
    // case. Returns null when the description carries no reference.
    static LLUUID parseDescription(const std::string& desc);

    ss_track_set_t mBaseline;   // as loaded
    ss_track_set_t mWorking;    // baseline + local edits; what runs

    ESource     mSource = SOURCE_DEFAULT;
    std::string mStatus = "system defaults";
    std::string mConfigName;    // notecard name, for the floater title

    LLUUID      mAssetID;       // notecard currently applied
    LLUUID      mPendingID;     // fetch in flight
    ESource     mPendingSource = SOURCE_DEFAULT;
    std::string mPendingName;

    bool mLoaded = false;
};

// </SS:Nexii>

#endif // SS_ATMOTRACK_H
