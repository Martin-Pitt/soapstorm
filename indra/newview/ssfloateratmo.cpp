/**
 * @file ssfloateratmo.cpp
 * @brief Atmo Magic floater implementation.
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

#include "ssfloateratmo.h"
#include "ssrainshadow.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llviewercontrol.h"
#include "ssprecippreset.h"

// <SS:Nexii> Atmo Magic floater

SSFloaterAtmoMagic::SSFloaterAtmoMagic(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterAtmoMagic::postBuild()
{
    // Sliders/checkboxes/combo bind straight to their debug settings via
    // control_name; only the cache tools and sub-floaters need code
    getChild<LLButton>("recapture_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRecapture(); });
    getChild<LLButton>("fx_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_fx"); });
    getChild<LLButton>("assets_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_assets"); });
    getChild<LLButton>("audio_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_audio"); });
    getChild<LLButton>("edit_preset_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("ss_atmo_preset"); });

    refreshPresets();
    return true;
}

void SSFloaterAtmoMagic::onOpen(const LLSD& key)
{
    // Presets can be added or removed from the editor while this is open
    refreshPresets();
}

void SSFloaterAtmoMagic::refreshPresets()
{
    LLComboBox* combo = getChild<LLComboBox>("preset_combo");
    const std::string current = gSavedSettings.getString("SSAtmoPreset");

    combo->removeall();
    const auto& presets = SSPrecipPresetMgr::instance().presets();
    for (const SSPrecipPreset& preset : presets)
    {
        combo->add(preset.mName, LLSD(preset.mName));
    }
    // The saved name can go stale if a preset was deleted
    if (!combo->selectByValue(LLSD(current)) && !presets.empty())
    {
        combo->selectByValue(LLSD(presets.front().mName));
    }
}

void SSFloaterAtmoMagic::onClickRecapture()
{
    SSRainShadowMap::getInstance()->clearCache();
}

// </SS:Nexii>
