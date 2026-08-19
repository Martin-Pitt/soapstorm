/**
 * @file ssfloateratmo.h
 * @brief Atmo Magic floater: per-track weather authoring. Weather is defined
 *        per EEP sky track and normally comes from a notecard the parcel
 *        description points at. Editing follows the environment floaters: the
 *        loaded config is the baseline, edits on top are marked with an
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

#ifndef SS_FLOATERATMO_H
#define SS_FLOATERATMO_H

// <SS:Nexii> Atmo Magic floater

#include "llfloater.h"

class SSFloaterAtmoMagic : public LLFloater
{
public:
    SSFloaterAtmoMagic(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    // Dropping a notecard anywhere on the floater loads it as the config
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    void onClickEditPreset();
    void onClickReload();
    void onClickLoad();
    void onClickSave();
    void onClickRevert();
    void onClickDefaults();

    void refreshPresets();

    // Rebuild the track selector, including each band's altitude range
    void refreshTrackList();

    // Pull the selected track's config into the widgets
    void refreshTrackUI();

    // Title, status line and the modified asterisk
    void refreshStatus();

    // Push widget values back into the working config and persist
    void onCommitTrackField();
    void onCommitTrackSelect();

    S32  mTrack = 1;
    bool mFollowCamera = true;
    bool mUpdating = false;     // guards commit callbacks during refresh
    F64  mLastPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_FLOATERATMO_H
