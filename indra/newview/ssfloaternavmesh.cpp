/**
 * @file ssfloaternavmesh.cpp
 * @brief See ssfloaternavmesh.h.
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

#include "ssfloaternavmesh.h"

#include "ssnavmesh.h"
#include "ssworldfieldshapes.h"

#include "llagent.h"
#include "llbutton.h"
#include "llframetimer.h"
#include "lltextbox.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"

SSFloaterNavMesh::SSFloaterNavMesh(const LLSD& key) :
    LLFloater(key)
{
}

// Wires the buttons; the View tab's switches bind straight to their settings in the XML.
bool SSFloaterNavMesh::postBuild()
{
    mNavStatus = getChild<LLTextBox>("navmesh_status");
    mCensusStatus = getChild<LLTextBox>("census_status");
    mPathStatus = getChild<LLTextBox>("path_status");

    getChild<LLButton>("rebuild_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->rebuildAll(); });
    getChild<LLButton>("start_here_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->setTestStart(gAgent.getPositionAgent()); onFindPath(); });
    getChild<LLButton>("start_camera_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->setTestStart(LLViewerCamera::getInstance()->getOrigin()); onFindPath(); });
    getChild<LLButton>("end_here_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->setTestEnd(gAgent.getPositionAgent()); onFindPath(); });
    getChild<LLButton>("end_camera_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->setTestEnd(LLViewerCamera::getInstance()->getOrigin()); onFindPath(); });
    getChild<LLButton>("find_path_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onFindPath(); });
    getChild<LLButton>("clear_path_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            SSNavMesh::getInstance()->clearTestPath();
            mPathStatus->setText(getString("path_choose"));
        });
    mPathStatus->setText(getString("path_choose"));
    return true;
}

// Status texts follow the live state; a quarter-second cadence is plenty for a console.
void SSFloaterNavMesh::draw()
{
    const F32 now = (F32)LLFrameTimer::getTotalSeconds();
    if (now - mLastRefresh > 0.25f)
    {
        mLastRefresh = now;
        refresh();
    }
    LLFloater::draw();
}

void SSFloaterNavMesh::refresh()
{
    static LLCachedControl<bool> nav_enabled(gSavedSettings, "SSNavMesh", false);
    static LLCachedControl<bool> census_enabled(gSavedSettings, "SSWorldFieldShapes", false);
    SSNavMesh* nav = SSNavMesh::getInstance();
    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();

    if (!nav_enabled || !census_enabled)
    {
        mNavStatus->setText(getString("status_off"));
    }
    else if (!nav->active())
    {
        mNavStatus->setText(getString("status_no_census"));
    }
    else
    {
        mNavStatus->setText(llformat("%d columns, %d bands (%d to build, %d in flight)\n%u polygons, %.1f MB of layers\n%u builds, last %.1f ms\n%d mover obstacles",
                                     nav->columnCount(), nav->bandCount(), nav->pendingCount(), nav->inFlightCount(),
                                     nav->polyCount(), nav->layerBytes() / 1048576.0,
                                     nav->buildCount(), nav->lastBuildMS(), nav->obstacleCount()));
    }

    if (!census_enabled)
    {
        mCensusStatus->setText("Census is off (SSWorldFieldShapes).");
    }
    else
    {
        mCensusStatus->setText(llformat("%d records, %d triangles, %d parts cached\n%s, last build %.1f ms\nlast schedule saw %d records: %d dynamic, %d phantom",
                                        shapes->recordCount(), shapes->triangleCount(), shapes->cachedPartCount(),
                                        shapes->building() ? "rebuilding" : (shapes->censusCurrent() ? "current" : "stale"),
                                        shapes->lastBuildMS(), nav->lastScheduleSeen(), nav->lastScheduleDynamic(), nav->lastSchedulePhantom()));
    }
}

void SSFloaterNavMesh::onFindPath()
{
    SSNavMesh* nav = SSNavMesh::getInstance();
    if (!nav->hasTestStart() || !nav->hasTestEnd())
    {
        mPathStatus->setText(getString("path_choose"));
        return;
    }
    if (!nav->active())
    {
        mPathStatus->setText(getString("path_no_navmesh"));
        return;
    }
    std::string status;
    if (!nav->runTestPath(status))
    {
        mPathStatus->setText(getString("path_none"));
        return;
    }
    mPathStatus->setText(status);
}
