/**
 * @file ssfloatersim.cpp
 * @brief Atmo Magic simulation settings floater.
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

#include "ssrainshadow.h"
#include "sswindflow.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "lltextbox.h"
#include "lluictrl.h"
#include "llcontrol.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "llviewercontrol.h"
#include "pipeline.h"

// <SS:Nexii> Atmo Magic simulation settings

static const F64 STATUS_POLL_INTERVAL = 0.5;

SSFloaterSimulation::SSFloaterSimulation(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterSimulation::postBuild()
{
    getChild<LLButton>("shadow_rebuild_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRecaptureShadow(); });
    getChild<LLButton>("flow_rebuild_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRebuildFlow(); });

    // Watch the settings, not the widgets. Each value is reachable from a
    // slider, a spinner, a reset button and the debug console, and every one of
    // them has to invalidate the map it tunes.
    watch("SSAtmoShadowRes", true);
    watch("SSAtmoShadowMaxAge", true);

    const char* flow_controls[] = {
        "SSAtmoWindFlow", "SSAtmoWindFlowCell", "SSAtmoWindFlowRes",
        "SSAtmoWindFlowMargin", "SSAtmoWindFlowHeight",
        "SSAtmoWindFlowIterations", "SSAtmoWindFlowSliceMax",
        "SSAtmoWindFlowSliceMin", "SSAtmoWindFlowShelterSteps",
        "SSAtmoWindFlowGradient"
    };
    for (const char* name : flow_controls)
    {
        watch(name, false);
    }

    // The overlays live in the render debug mask rather than in a setting, so
    // these drive it directly and read it back in refreshStatus. Toggling from
    // Develop > Render Metadata therefore shows up here too, rather than the
    // two disagreeing about what is on screen.
    getChild<LLCheckBoxCtrl>("flow_overlay_check")->setCommitCallback(
        [](LLUICtrl*, const LLSD&) { LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_WIND_FLOW); });
    getChild<LLCheckBoxCtrl>("shadow_overlay_check")->setCommitCallback(
        [](LLUICtrl*, const LLSD&) { LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_RAIN_SHADOW); });

    refreshStatus();
    return true;
}

void SSFloaterSimulation::watch(const std::string& control, bool shadow)
{
    LLControlVariable* var = gSavedSettings.getControl(control);
    if (!var)
    {
        LL_WARNS("AtmoMagic") << "Simulation floater has no setting named "
                              << control << LL_ENDL;
        return;
    }

    // Scoped, so the connections go when the floater does
    mConnections.emplace_back(var->getSignal()->connect(
        [this, shadow](LLControlVariable*, const LLSD&, const LLSD&)
        {
            if (shadow)
            {
                // Resolution is read at capture time and the refresh interval
                // only governs the next one, so neither takes effect until the
                // cache is dropped
                SSRainShadowMap::getInstance()->clearCache();
            }
            else
            {
                // The flowmap would notice this on its own through its settings
                // hash, but only after waiting out the rezzing-region settle
                // delay. Asking explicitly makes dragging a slider feel like it
                // did something.
                SSWindFlowMap::getInstance()->rebuildAll();
            }
            refreshStatus();
        }));
}

void SSFloaterSimulation::onOpen(const LLSD& key)
{
    refreshStatus();
}

void SSFloaterSimulation::draw()
{
    // Neither map has a change signal, and the status only matters while this
    // is on screen, so poll rather than plumb one through
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mLastPoll > STATUS_POLL_INTERVAL)
    {
        mLastPoll = now;
        refreshStatus();
    }
    LLFloater::draw();
}

void SSFloaterSimulation::onClickRecaptureShadow()
{
    SSRainShadowMap::getInstance()->clearCache();
    refreshStatus();
}

void SSFloaterSimulation::onClickRebuildFlow()
{
    SSWindFlowMap::getInstance()->rebuildAll();
    refreshStatus();
}

void SSFloaterSimulation::refreshStatus()
{
    SSRainShadowMap* shadow = SSRainShadowMap::getInstance();
    const U32 shadow_res = shadow->resolution();

    getChild<LLTextBox>("shadow_status")->setText(
        shadow->tileCount() == 0
            ? std::string("no regions mapped yet")
            : llformat("%d region%s mapped at %d texels",
                       shadow->tileCount(), shadow->tileCount() == 1 ? "" : "s",
                       (S32)shadow_res));

    // The per-column readouts for both maps are in the Atmo Magic info
    // overlay, not here: they describe the spot you are standing on, and
    // reading them means looking at that spot rather than at a settings window.
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLTextBox* flow_status = getChild<LLTextBox>("flow_status");
    SSWindFlowMap* flow = SSWindFlowMap::getInstance();

    if (!SSWindFlowMap::isSupported())
    {
        // Compute shaders are GL 4.3. Say so rather than showing an idle map
        // and leaving the tuning looking broken.
        flow_status->setText(std::string("unavailable: needs OpenGL 4.3"));
    }
    else if (flow->tileCount() == 0)
    {
        flow_status->setText(std::string("no regions solved yet"));
    }
    else
    {
        flow_status->setText(llformat(
            "%d region%s solved, %.1fm cells, %d slabs, %.0fms, solved %.0fs ago",
            flow->tileCount(), flow->tileCount() == 1 ? "" : "s",
            flow->cellSize(), flow->sliceCount(),
            flow->lastSolveMS(), (F32)flow->age()));
    }

    getChild<LLCheckBoxCtrl>("flow_overlay_check")->set(
        gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_WIND_FLOW));
    getChild<LLCheckBoxCtrl>("shadow_overlay_check")->set(
        gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_RAIN_SHADOW));

    LLTextBox* capture_status = getChild<LLTextBox>("flow_capture_status");
    static LLCachedControl<U32> capture_view(gSavedSettings, "SSAtmoWindFlowDebugCapture", 0);

    if (capture_view == 0)
    {
        capture_status->setText(std::string("off, drawing the solved flow field"));
    }
    else if (flow->capturedRegion() == 0)
    {
        capture_status->setText(std::string("nothing captured yet"));
    }
    else
    {
        LLViewerRegion* captured = LLWorld::getInstance()->getRegionFromHandle(flow->capturedRegion());
        LLViewerRegion* here = LLWorld::getInstance()->getRegionFromPosAgent(cam);

        const std::string name = captured ? captured->getName() : std::string("a region that has gone");
        capture_status->setText(llformat("showing %s%s",
            name.c_str(),
            (captured && captured == here) ? "" : ", which is not the region you are in"));
    }
}

// </SS:Nexii>
