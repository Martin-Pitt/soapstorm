/**
 * @file ssfloaterfx.cpp
 * @brief See ssfloaterfx.h.
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

#include "ssfloaterfx.h"

#include "llcheckboxctrl.h"
#include "lluictrl.h"
#include "pipeline.h"

// Floater shell; every dial and switch on the tabs binds itself through control_name.
SSFloaterEffects::SSFloaterEffects(const LLSD& key) :
    LLFloater(key)
{
}

// <SS:Nexii> The one control that is not a setting. The cloud overlay is a render-debug mask so it shares a switch with Develop > Render Metadata > Cloud Field - toggling it either way shows up in both places - which is the same arrangement the Simulation floater's five overlays use, and the reason this floater has a class at all.
bool SSFloaterEffects::postBuild()
{
    getChild<LLCheckBoxCtrl>("cloud_overlay_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_CLOUD_FIELD);
            syncOverlayChecks();
        });

    syncOverlayChecks();
    return true;
}

// Fresh overlay state on open - the mask may have been changed from the menu since.
void SSFloaterEffects::onOpen(const LLSD& key)
{
    syncOverlayChecks();
}

void SSFloaterEffects::syncOverlayChecks()
{
    getChild<LLCheckBoxCtrl>("cloud_overlay_check")->set(
        gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_CLOUD_FIELD));
}
