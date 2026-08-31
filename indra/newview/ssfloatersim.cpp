/**
 * @file ssfloatersim.cpp
 * @brief See ssfloatersim.h.
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

#include "ssfloatersim.h"

#include "llfloaterreg.h"

#include "ssrainshadow.h"
#include "sswindflow.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcontrol.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "pipeline.h"

// Floater shell; all content is wired in postBuild.
SSFloaterSimulation::SSFloaterSimulation(const LLSD& key) :
    LLFloater(key)
{
}

// Wires rebuild buttons, overlay checkboxes, and setting watchers that invalidate the right map.
bool SSFloaterSimulation::postBuild()
{
    getChild<LLButton>("shadow_rebuild_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRecaptureShadow(); });
    getChild<LLButton>("flow_rebuild_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRebuildFlow(); });

    watch("SSAtmoShadowRes", EInvalidate::SHADOW);
    watch("SSAtmoShadowMaxAge", EInvalidate::SHADOW);

    const char* flow_controls[] = {
        "SSAtmoWindFlow", "SSAtmoWindFlowCell", "SSAtmoWindFlowRes",
        "SSAtmoWindFlowMargin", "SSAtmoWindFlowHeight",
        "SSAtmoWindFlowIterations", "SSAtmoWindFlowSliceMax",
        "SSAtmoWindFlowSliceMin", "SSAtmoWindFlowShelterSteps",
        "SSAtmoWindFlowGradient"
    };
    for (const char* name : flow_controls)
    {
        watch(name, EInvalidate::FLOW);
    }

    getChild<LLCheckBoxCtrl>("flow_overlay_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_WIND_FLOW);
            syncOverlayChecks();
        });
    getChild<LLCheckBoxCtrl>("shadow_overlay_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_RAIN_SHADOW);
            syncOverlayChecks();
        });
    getChild<LLCheckBoxCtrl>("settle_overlay_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_GEOM_SETTLE);
            syncOverlayChecks();
        });
    getChild<LLCheckBoxCtrl>("runoff_overlay_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_ROOF_RUNOFF);
            syncOverlayChecks();
        });

    syncOverlayChecks();
    return true;
}

// Invalidates the owning map (shadow cache or flow solve) whenever one of its settings changes.
void SSFloaterSimulation::watch(const std::string& control, EInvalidate what)
{
    LLControlVariable* var = gSavedSettings.getControl(control);
    if (!var)
    {
        LL_WARNS("AtmoMagic") << "Simulation floater has no setting named "
                              << control << LL_ENDL;
        return;
    }

    mConnections.emplace_back(var->getSignal()->connect(
        [this, what](LLControlVariable*, const LLSD&, const LLSD&)
        {
            if (what == EInvalidate::SHADOW)
            {
                SSRainShadowMap::getInstance()->clearCache();
            }
            else
            {
                SSWindFlowMap::getInstance()->rebuildAll();
            }
        }));
}

// Fresh overlay state on open.
void SSFloaterSimulation::onOpen(const LLSD& key)
{
    syncOverlayChecks();
}

// Explicit rain-shadow recapture.
void SSFloaterSimulation::onClickRecaptureShadow()
{
    SSRainShadowMap::getInstance()->clearCache();
}

// Explicit flowmap re-solve.
void SSFloaterSimulation::onClickRebuildFlow()
{
    SSWindFlowMap::getInstance()->rebuildAll();
}

// Mirrors the debug-view masks into the overlay checkboxes. The live status of
// every map lives in the Atmo Magic info overlay; this floater only carries the
// toggles and the dials, so it just reflects what the debug masks say.
void SSFloaterSimulation::syncOverlayChecks()
{
    getChild<LLCheckBoxCtrl>("flow_overlay_check")->set(
        gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_WIND_FLOW));
    getChild<LLCheckBoxCtrl>("shadow_overlay_check")->set(
        gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_RAIN_SHADOW));
    getChild<LLCheckBoxCtrl>("settle_overlay_check")->set(
        gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_GEOM_SETTLE));
    getChild<LLCheckBoxCtrl>("runoff_overlay_check")->set(
        gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_ROOF_RUNOFF));
}