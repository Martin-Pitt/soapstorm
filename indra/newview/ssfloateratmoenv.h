/**
 * @file ssfloateratmoenv.h
 * @brief Atmo Magic floater: phase 1 only - create/load/save/revert and a
 *        bare track list, proving the notecard round-trip before any of the
 *        per-tab editing UI (Atmosphere & Lighting / Clouds / Planetary /
 *        Weather) exists. See doc/atmo_magic_environment.md.
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

#ifndef SS_FLOATERATMOENV_H
#define SS_FLOATERATMOENV_H

// <SS:Nexii> Atmo Magic floater

#include "llfloater.h"
#include "ssatmoenvkeyframe.h"

#include <functional>
#include <vector>

class SSFloaterAtmoEnv : public LLFloater
{
public:
    SSFloaterAtmoEnv(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void onVisibilityChange(bool new_visibility) override;
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
    void onClickTrackRow(S32 index);
    void onCommitName();

    // Which track row is selected in the vertical list (see refreshTrackList) -
    // drives Remove Track's enable state and highlight, same role
    // LLScrollListCtrl::getFirstSelectedIndex() used to play when the track
    // list was a table. Index 0 (ground) is always selectable but never
    // removable.
    // Up to SS_ATMOENV_MAX_TRACKS (see ssatmoenvasset.h) fixed row widgets
    // (track_row_0..3) exist in the XML - like EEP's Edit Day Cycle floater,
    // which has a fixed Sky1-4/Water set of layer buttons rather than a
    // dynamically built list. This mirrors that rather than a scroll list/
    // table, per the vertical Environment-tab-style layout the design calls
    // for.
    S32 mSelectedTrackIndex = 0;

    // Ground track's Weather cube, one AE-style row per keyframable field:
    // label, slider, keyframe diamond, prev/next chevrons, keyframe count.
    // Not the real Weather tab yet (still only the ground track, still no
    // curve/HOLD controls) - see doc/atmo_magic_environment.md - but the
    // row mechanics (setValueAtHead's three-way rule, toggling a keyframe,
    // jumping between them) are the real thing, generalized past the
    // original single-field Moisture proof to every field the cube
    // actually keyframes. mField is re-derived fresh on every call rather
    // than cached, since a raw pointer into SSAtmoEnvAsset::mTracks would
    // dangle across an add/remove track that reallocates the vector.
    struct FloatRow
    {
        std::string mPrefix; // widget name prefix - "moisture" -> "moisture_slider", ...
        std::function<SSAtmoEnvKeyframed<F32>&()> mField;
        std::string mUnitSuffix;      // e.g. "\xC2\xB0" "C" for "\xC2\xB0" "C", "" for none
        bool mIntegerDisplay = false; // round the value_text display; the slider's own precision is unaffected
    };
    std::vector<FloatRow> mFloatRows;

    void refreshKeyframeProof();
    void onCommitPreviewTime();

    // Shared bodies every row's widget callbacks funnel into.
    void refreshFloatRow(const FloatRow& row, F64 time, F64 day_length);
    void commitFloatRow(const FloatRow& row);
    void toggleFloatRowKeyframe(const FloatRow& row);
    void jumpFloatRowPrev(const FloatRow& row);
    void jumpFloatRowNext(const FloatRow& row);

    // Seconds into the loaded asset's day-cycle loop; the timeline head
    // every keyframe operation above reads and writes against.
    F64 mPreviewTime = 0.0;

    F64 mLastPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_FLOATERATMOENV_H
