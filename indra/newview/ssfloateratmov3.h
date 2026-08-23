/**
 * @file ssfloateratmov3.h
 * @brief Atmo Magic v3 floater: phase 1 only - create/load/save/revert and a
 *        bare track list, proving the notecard round-trip before any of the
 *        per-tab editing UI (Atmosphere & Lighting / Clouds / Planetary /
 *        Weather) exists. See doc/atmo_magic_v3_environment.md.
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

#ifndef SS_FLOATERATMOV3_H
#define SS_FLOATERATMOV3_H

// <SS:Nexii> Atmo Magic v3 floater

#include "llfloater.h"

class SSFloaterAtmoV3 : public LLFloater
{
public:
    SSFloaterAtmoV3(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    // Dropping a notecard anywhere on the floater loads it
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    // hasAsset() false: only the create button is meaningful, per the
    // no-implicit-default rule. True: the normal editing chrome.
    void refreshVisibility();

    void refreshTrackList();
    void refreshStatus();

    void onClickCreateNew();
    void onClickLoad();
    void onClickSave();
    void onClickRevert();
    void onClickAddTrack();
    void onClickRemoveTrack();
    void onCommitName();

    // Phase 3 proof of the keyframe engine, wired to the ground track's
    // Moisture field only - not the real Weather tab (phase 5), just enough
    // to exercise ssatmov3keyframe.h's editing rule end to end: no
    // keyframe is a plain value, editing on a keyframe edits it, editing
    // elsewhere inserts one, and the chevrons jump the preview head between
    // whatever keyframes exist.
    void refreshKeyframeProof();
    void onCommitPreviewTime();
    void onCommitMoisture();
    void onClickMoistureKeyframeToggle();
    void onClickMoisturePrev();
    void onClickMoistureNext();

    // Seconds into the loaded asset's day-cycle loop; the timeline head
    // every keyframe operation above reads and writes against.
    F64 mPreviewTime = 0.0;

    F64 mLastPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_FLOATERATMOV3_H
