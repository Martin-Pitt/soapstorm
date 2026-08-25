/**
 * @file ssfloaterpreset.h
 * @brief Atmo Magic preset editor: a tabbed editor for one weather preset -
 *        motion and density, per-tier appearance, impact behaviour, textures
 *        and its sound pack. Edits apply live to the running weather so a
 *        preset can be dialled in while watching it fall.
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

#ifndef SS_FLOATERPRESET_H
#define SS_FLOATERPRESET_H

// <SS:Nexii> Atmo Magic preset editor

#include "llfloater.h"
#include "ssprecippreset.h"

class SSFloaterPreset : public LLFloater
{
public:
    SSFloaterPreset(const LLSD& key);

    bool postBuild() override;

    static std::string stepWidgetName(SSStepSurface surface, SSStepAction action);
    void onOpen(const LLSD& key) override;

private:
    void loadPreset(const std::string& name);
    void refreshPresetList();
    void controlsToPreset();   // read every widget back into mEdited
    void presetToControls();   // push mEdited out to the widgets
    void applyLive();          // save and make the sim pick it up immediately

    void onSelectPreset();
    void onCommitAny();
    void onClickNew();
    void onClickBlank();
    void onClickRename();
    void onClickDelete();
    void onClickRevert();
    void onClickSave();
    void onClickDiscard();

    // Title carries the preset name and an asterisk while edits are staged but not written to disk
    void refreshTitle();

    SSPrecipPreset mEdited;
    bool mUpdating = false;    // guards presetToControls against commit echo
};

// </SS:Nexii>

#endif // SS_FLOATERPRESET_H
