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
#include "sssurfacefield.h"
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

static const F64 STATUS_POLL_INTERVAL = 0.5;

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
        [](LLUICtrl*, const LLSD&) { LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_WIND_FLOW); });
    getChild<LLCheckBoxCtrl>("shadow_overlay_check")->setCommitCallback(
        [](LLUICtrl*, const LLSD&) { LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_RAIN_SHADOW); });
    getChild<LLCheckBoxCtrl>("settle_overlay_check")->setCommitCallback(
        [](LLUICtrl*, const LLSD&) { LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_GEOM_SETTLE); });

    refreshStatus();
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
            refreshStatus();
        }));
}

// Fresh status on open.
void SSFloaterSimulation::onOpen(const LLSD& key)
{
    refreshStatus();
}

// Polls status twice a second - solves finish in the background.
void SSFloaterSimulation::draw()
{
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mLastPoll > STATUS_POLL_INTERVAL)
    {
        mLastPoll = now;
        refreshStatus();
    }
    LLFloater::draw();
}

// Explicit rain-shadow recapture.
void SSFloaterSimulation::onClickRecaptureShadow()
{
    SSRainShadowMap::getInstance()->clearCache();
    refreshStatus();
}

// Explicit flowmap re-solve.
void SSFloaterSimulation::onClickRebuildFlow()
{
    SSWindFlowMap::getInstance()->rebuildAll();
    refreshStatus();
}

// Rewrites every status line and overlay checkbox from the live maps.
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

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLTextBox* flow_status = getChild<LLTextBox>("flow_status");
    SSWindFlowMap* flow = SSWindFlowMap::getInstance();

    if (!SSWindFlowMap::isSupported())
    {
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
    getChild<LLCheckBoxCtrl>("settle_overlay_check")->set(
        gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_GEOM_SETTLE));

    SSSurfaceField* surface = SSSurfaceField::getInstance();
    getChild<LLTextBox>("runoff_status")->setText(
        surface->fieldCount() == 0
            ? std::string("no surface dressed yet")
            : llformat("%d region%s dressed. Peak wet %.2f, snow %.0f mm, puddle %.0f mm, in %.1f ms",
                       surface->fieldCount(), surface->fieldCount() == 1 ? "" : "s",
                       surface->peakWet(), surface->peakSnow() * 1000.f,
                       surface->peakPuddle() * 1000.f, surface->lastTickMS()));

    LLTextBox* capture_status = getChild<LLTextBox>("flow_capture_status");
    static LLCachedControl<U32> capture_view(gSavedSettings, "SSAtmoWindFlowDebugCapture", 0);

    capture_status->setVisible(capture_view != 0);

    if (capture_view == 0)
    {
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
