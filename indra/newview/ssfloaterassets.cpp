/**
 * @file ssfloaterassets.cpp
 * @brief Atmo Magic global assets. See the header.
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

#include "ssfloaterassets.h"

#include "ssfloatersoundlist.h"

#include "ssprecippreset.h"

#include "llviewercontrol.h"

// <SS:Nexii> Atmo Magic global assets

namespace
{
    const char* THUNDER_CRACK_SETTING = "SSAtmoThunderCrack";
    const char* THUNDER_RUMBLE_SETTING = "SSAtmoThunderRumble";
    const char* WIND_LIGHT_SETTING = "SSAtmoLoopWindLight";
    const char* WIND_STRONG_SETTING = "SSAtmoLoopWindStrong";
}

SSFloaterAssets::SSFloaterAssets(const LLSD& key)
:   LLFloater(key)
{
}

bool SSFloaterAssets::postBuild()
{
    getChild<LLUICtrl>("loop_wind_light")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitWind(); });
    getChild<LLUICtrl>("loop_wind_strong")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitWind(); });

    getChild<SSSoundListCtrl>("thunder_crack")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitThunder(); });
    getChild<SSSoundListCtrl>("thunder_rumble")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitThunder(); });

    // The dry-ground footstep grid - see SSFootstepSounds::surfaceIsGlobal.
    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        if (!SSFootstepSounds::surfaceIsGlobal((SSStepSurface)sf)) continue;

        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            const std::string name = stepWidgetName((SSStepSurface)sf, (SSStepAction)ac);
            if (LLUICtrl* ctrl = findChild<LLUICtrl>(name))
            {
                ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitSteps(); });
            }
        }
    }

    return true;
}

// The widget name for a footstep slot, built from the same keys the settings
// and the preset both use.
std::string SSFloaterAssets::stepWidgetName(SSStepSurface surface, SSStepAction action)
{
    return std::string("step_") + SSFootstepSounds::surfaceKey(surface)
         + "_" + SSFootstepSounds::actionKey(action);
}

void SSFloaterAssets::onCommitThunder()
{
    gSavedSettings.setString(THUNDER_CRACK_SETTING,
        ss_asset_list_str(getChild<SSSoundListCtrl>("thunder_crack")->getList()));
    gSavedSettings.setString(THUNDER_RUMBLE_SETTING,
        ss_asset_list_str(getChild<SSSoundListCtrl>("thunder_rumble")->getList()));
}

void SSFloaterAssets::onCommitSteps()
{
    getChild<SSSoundListCtrl>("thunder_crack")->setList(
        ss_asset_list_parse(gSavedSettings.getString(THUNDER_CRACK_SETTING)));
    getChild<SSSoundListCtrl>("thunder_rumble")->setList(
        ss_asset_list_parse(gSavedSettings.getString(THUNDER_RUMBLE_SETTING)));

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        const SSStepSurface surface = (SSStepSurface)sf;
        if (!SSFootstepSounds::surfaceIsGlobal(surface)) continue;

        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            const SSStepAction action = (SSStepAction)ac;
            if (SSSoundListCtrl* ctrl =
                    findChild<SSSoundListCtrl>(stepWidgetName(surface, action)))
            {
                gSavedSettings.setString(
                    SSFootstepSounds::globalSettingName(surface, action),
                    ss_asset_list_str(ctrl->getList()));
            }
        }
    }
}

void SSFloaterAssets::onOpen(const LLSD& key)
{
    // Read on every open rather than once at build. These are global
    // settings, so something else may have changed them since - and a stale
    // list here would be written straight back over the new value the moment
    // anything in this window was touched.
    getChild<SSSoundListCtrl>("loop_wind_light")->setList(
        ss_asset_list_parse(gSavedSettings.getString(WIND_LIGHT_SETTING)));
    getChild<SSSoundListCtrl>("loop_wind_strong")->setList(
        ss_asset_list_parse(gSavedSettings.getString(WIND_STRONG_SETTING)));

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        const SSStepSurface surface = (SSStepSurface)sf;
        if (!SSFootstepSounds::surfaceIsGlobal(surface)) continue;

        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            const SSStepAction action = (SSStepAction)ac;
            if (SSSoundListCtrl* ctrl =
                    findChild<SSSoundListCtrl>(stepWidgetName(surface, action)))
            {
                ctrl->setList(ss_asset_list_parse(gSavedSettings.getString(
                    SSFootstepSounds::globalSettingName(surface, action))));
                ctrl->setSlotLabel(std::string(SSFootstepSounds::surfaceName(surface))
                                   + " - " + SSFootstepSounds::actionName(action));
            }
        }
    }
}

void SSFloaterAssets::onCommitWind()
{
    gSavedSettings.setString(WIND_LIGHT_SETTING,
        ss_asset_list_str(getChild<SSSoundListCtrl>("loop_wind_light")->getList()));
    gSavedSettings.setString(WIND_STRONG_SETTING,
        ss_asset_list_str(getChild<SSSoundListCtrl>("loop_wind_strong")->getList()));
}

// </SS:Nexii>
