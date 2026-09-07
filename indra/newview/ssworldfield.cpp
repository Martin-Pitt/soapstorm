/**
 * @file ssworldfield.cpp
 * @brief See ssworldfield.h.
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

#include "ssworldfield.h"
#include "ssatmomagic.h"
#include "ssglreadback.h"

#include "llfasttimer.h"
#include "llrender.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "workqueue.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <queue>

static const F32 NEIGHBOR_REACH   = 64.f;
static const F32 DEPTH_MISS       = 0.9999f;
static const F32 BOUNDARY_EPSILON = 0.05f;
static const F32 NO_SURFACE       = -FLT_MAX;

// <SS:Nexii> The enclosure spectrum's saturation constant: metres of
// air-graph distance back to open sky at which the ramp reads half enclosed.
// Small structures - eaves, canopies, a doorway's threshold - sit a metre or
// two from the outdoors and stay near 0; rooms and caves whose openings are
// tens of metres of graph away climb toward 1.
static const F32 SS_WF_ENCLOSURE_TAU_M = 4.f;

// <SS:Nexii> How far the acoustic lattice's wall walks run before an
// direction is called open. Above the soundscape's own side-ray length, so
// a lattice "open" can never read as a wall hit in its consumers.
static const F32 SS_WF_ACOUSTIC_REACH_M = 64.f;

static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD("Atmo Magic World Field");
static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD_GRID("Atmo Magic World Field Grid");

// Channel interest refcounts. File statics so an Interest handle's deleter
// stays valid for the process's life regardless of singleton teardown
// order - a destroyed handle must always release its count.
static std::map<std::pair<U64, S32>, S32> sInterests;

// <SS:Nexii> The debug overlay's materialised drainage view, cached per region and rebuilt only when the tile's geometry serial moves - the same drop-on-serial-change discipline the real channels will follow, so the overlay never pays a per-frame priority flood just to draw. Declared with the other file statics because evict() drops it alongside the tiles it belongs to.
struct SS_WF_DrainDebug
{
    SSRainShadowMap::SurfaceGrid mGrid;
    SSWorldField::Drainage mDrain;
    S32 mN = 0;
    U32 mSerial = 0;
};
static std::map<U64, SS_WF_DrainDebug> sDrainDebug;

static S32 ss_wf_interest_count(U64 region_handle, S32 channel)
{
    auto it = sInterests.find(std::make_pair(region_handle, channel));
    return (it != sInterests.end()) ? it->second : 0;
}

static bool ss_wf_region_claimed(U64 region_handle)
{
    for (S32 ch = 0; ch <= (S32)SSWorldField::EChannel::ACOUSTIC; ++ch)
    {
        if (ss_wf_interest_count(region_handle, ch) > 0) return true;
    }
    return false;
}

SSWorldField::Interest SSWorldField::claim(U64 region_handle, EChannel channel)
{
    const std::pair<U64, S32> key(region_handle, (S32)channel);
    ++sInterests[key];

    return Interest(std::shared_ptr<void>((void*)1, [key](void*)
    {
        auto it = sInterests.find(key);
        if (it != sInterests.end() && --(it->second) <= 0)
        {
            sInterests.erase(it);
        }
    }));
}

// The wet field's source switch - while on, SURFACE_TOP counts as claimed
// for the camera region and its neighbours, the same reach the rain shadow
// capture serves today.
bool SSWorldField::surfaceTopDemanded() const
{
    static LLCachedControl<bool> demanded(gSavedSettings, "SSWorldFieldSurfaceTop", false);
    return demanded;
}

// One edit fan-out. Settled prim edits land here; the tile's dirty rectangle
// grows to cover the edit, and the re-peel is scissored to it.
void SSWorldField::markDirty(const LLVector3& pos_agent, F32 radius)
{
    SSWorldField* self = getInstance();
    if (!self) return;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return;

    auto it = self->mTiles.find(regionp->getHandle());
    if (it == self->mTiles.end() || !it->second.mValid) return;

    Tile& tile = it->second;
    tile.mLastTouched = self->mNow;

    const F32 x = pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX];
    const F32 y = pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY];
    const F32 r = llmax(radius, tile.mCell);

    const S32 x0 = llclamp((S32)floorf((x - r) / tile.mCell), 0, tile.mRes - 1);
    const S32 y0 = llclamp((S32)floorf((y - r) / tile.mCell), 0, tile.mRes - 1);
    const S32 x1 = llclamp((S32)ceilf((x + r) / tile.mCell), x0 + 1, tile.mRes);
    const S32 y1 = llclamp((S32)ceilf((y + r) / tile.mCell), y0 + 1, tile.mRes);

    if (tile.mDirty)
    {
        tile.mDirtyX0 = llmin(tile.mDirtyX0, x0);
        tile.mDirtyY0 = llmin(tile.mDirtyY0, y0);
        tile.mDirtyX1 = llmax(tile.mDirtyX1, x1);
        tile.mDirtyY1 = llmax(tile.mDirtyY1, y1);
    }
    else
    {
        tile.mDirtyX0 = x0;
        tile.mDirtyY0 = y0;
        tile.mDirtyX1 = x1;
        tile.mDirtyY1 = y1;
        tile.mDirty = true;
    }

// Bands the edit's own altitude could touch. A removed roof drops its whole
    // column, so a rect re-peel sweeps from the band floor up to just past the
    // edit, not only the bands the edit's box overlaps.
    const S32 band = llclamp((S32)((pos_agent.mV[VZ] + r) / tile.mBandHeight), 0, SSWorldField::MAX_BANDS - 1);
    tile.mBandTarget = llmax(tile.mBandTarget, band + 1);
}

void SSWorldField::clear()
{
    // A read in flight still references mTarget's depth texture - let it land
    // first, then tear the target down from the read's completion.
    if (mReadbackPending) { mClearPending = true; return; }
    mTiles.clear();
    mBuild.mActive = false;
    ++mFloodGeneration;     // a flood in flight lands into nothing
    mTarget.release();
}

bool SSWorldField::tileValid(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    return (it != mTiles.end() && it->second.mValid);
}

U32 SSWorldField::geometrySerial(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    return (it != mTiles.end()) ? it->second.mGeomSerial : 0;
}

F32 SSWorldField::bandHeight() const
{
    static LLCachedControl<F32> band(gSavedSettings, "SSWorldFieldBand", 16.f);
    return llclamp((F32)band, 4.f, 64.f);
}

S32 SSWorldField::bandCount() const
{
    static LLCachedControl<F32> ceiling(gSavedSettings, "SSWorldFieldCeiling", 256.f);
    const S32 count = (S32)ceilf(llmax((F32)ceiling, bandHeight()) / bandHeight());
    return llclamp(count, 1, MAX_BANDS);
}

S32 SSWorldField::resolution() const
{
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid) return entry.second.mRes;
    }
    return 0;
}

F64 SSWorldField::tileAge(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return -1.0;
    return mNow - tile->mCaptureTime;
}

S32 SSWorldField::effectiveBands(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    return tile ? tile->mBandCount : 0;
}

const SSWorldField::Tile* SSWorldField::tileAt(const LLVector3& pos_agent) const
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return nullptr;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end()) return nullptr;
    return &it->second;
}

SSWorldField::Tile* SSWorldField::tileFor(LLViewerRegion* regionp, bool allow_create)
{
    auto it = mTiles.find(regionp->getHandle());
    if (it != mTiles.end()) return &it->second;
    if (!allow_create) return nullptr;

    static LLCachedControl<F32> cell_setting(gSavedSettings, "SSWorldFieldCell", 0.25f);
    const F32 cell = llclamp((F32)cell_setting, 0.25f, 8.f);
    const F32 width = regionp->getWidth();
    const S32 res = llclamp((S32)llround(width / cell), 32, 1024);

    Tile& tile = mTiles[regionp->getHandle()];
    tile.mRegionHandle = regionp->getHandle();
    tile.mRes = res;
    tile.mCell = width / (F32)res;
    tile.mBandHeight = bandHeight();
    tile.mAllocBands = 0;
    return &tile;
}

// Whether a tile is worth (re)building: never built, edited, stale, or
// captured under a cell/band setting that has since changed.
bool SSWorldField::needsBuild(const Tile& tile) const
{
    if (!tile.mValid) return true;

    static LLCachedControl<F32> max_age(gSavedSettings, "SSWorldFieldMaxAge", 120.f);
    if (mNow - tile.mCaptureTime > (F64)llmax(5.f, (F32)max_age)) return true;

    if (tile.mDirty && mNow - tile.mCaptureTime > DIRTY_MIN_INTERVAL) return true;

    static LLCachedControl<F32> cell_setting(gSavedSettings, "SSWorldFieldCell", 0.25f);
    const F32 cell = llclamp((F32)cell_setting, 0.25f, 8.f);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;
    const S32 res = llclamp((S32)llround(regionp->getWidth() / cell), 32, 1024);
    if (res != tile.mRes) return true;

    if (fabsf(tile.mBandHeight - bandHeight()) > 0.01f) return true;

    return false;
}

// Most deserving tile first: the camera's region, then any region within
// neighbour reach that has a claimed channel or serves the demanded surface
// top. Nothing builds while nothing demands anything.
SSWorldField::Tile* SSWorldField::pickBuildTarget()
{
    if (!surfaceTopDemanded() && sInterests.empty()) return nullptr;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);
    if (cam_region)
    {
        Tile* tile = tileFor(cam_region, true);
        tile->mLastTouched = mNow;
        if (needsBuild(*tile)) return tile;
    }

    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || regionp == cam_region) continue;
        if (!ss_wf_region_claimed(regionp->getHandle()) && !surfaceTopDemanded()) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const F32 width = regionp->getWidth();
        const F32 dx = llmax(origin.mV[VX] - cam.mV[VX], cam.mV[VX] - (origin.mV[VX] + width), 0.f);
        const F32 dy = llmax(origin.mV[VY] - cam.mV[VY], cam.mV[VY] - (origin.mV[VY] + width), 0.f);
        if (dx * dx + dy * dy > NEIGHBOR_REACH * NEIGHBOR_REACH) continue;

        Tile* tile = tileFor(regionp, true);
        tile->mLastTouched = mNow;
        if (needsBuild(*tile)) return tile;
    }

    return nullptr;
}

// Per-frame drive: evict departed regions, keep stepping the live build one
// band at a time, and begin a new build when a tile deserves one.
void SSWorldField::update()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD);

    mNow = SSAtmoMagic::getInstance()->sharedTime();

    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldField", true);
    if (!enabled || !SSAtmoMagic::getInstance()->hasWeather())
    {
        if (!mTiles.empty() || mBuild.mActive) clear();
        return;
    }

    evict();

// Catch-up for the connectivity labels: a tile that committed while a flood
    // was in flight was skipped rather than queued; this is where it gets its
    // turn - one tile per call. Oldest-first is unnecessary at this scale
    // (four tiles).
    if (!mFloodBusy)
    {
        for (auto& entry : mTiles)
        {
            Tile& tile = entry.second;
            if (tile.mValid && tile.mAirSerial != tile.mGeomSerial && !tile.mDirty)
            {
                scheduleFlood(tile);
                break;
            }
        }
    }

    if (mBuild.mActive)
    {
        if (mNow - mLastBandAt < BAND_MIN_INTERVAL) return;
        mLastBandAt = mNow;

        LLTimer band_timer;
        advanceBuild();
        mLastCaptureMS = band_timer.getElapsedTimeF32() * 1000.f;
        return;
    }

    Tile* target = pickBuildTarget();
    if (!target) return;

// Begin a build. A dirty tile re-peels only its dirty rectangle's frustum,
    // but still sweeps every band from the floor to the highest band the edits
    // could touch - a removed roof drops its whole column, so band scoping below
    // the edit is not safe.
    mBuild.mActive = true;
    mBuild.mRegionHandle = target->mRegionHandle;
    mBuild.mBand = 0;
    mBuild.mEmptyRun = 0;
    mBuild.mChanged = false;
    mBuild.mRectOnly = target->mDirty;

    if (mBuild.mRectOnly)
    {
        mBuild.mRectX0 = target->mDirtyX0;
        mBuild.mRectY0 = target->mDirtyY0;
        mBuild.mRectX1 = target->mDirtyX1;
        mBuild.mRectY1 = target->mDirtyY1;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(target->mRegionHandle);
        const LLVector3 origin = regionp ? regionp->getOriginAgent() : LLVector3::zero;

        mBuild.mRectCentre.setVec(origin.mV[VX] + 0.5f * (F32)(mBuild.mRectX0 + mBuild.mRectX1) * target->mCell,
                                  origin.mV[VY] + 0.5f * (F32)(mBuild.mRectY0 + mBuild.mRectY1) * target->mCell,
                                  0.f);

// Rect capture resources: a square frustum covering the rect's wider axis,
        // with the short side's extra texels spilling outside the rect and skipped
        // at splice time.
        const S32 rw = mBuild.mRectX1 - mBuild.mRectX0;
        const S32 rh = mBuild.mRectY1 - mBuild.mRectY0;
        mBuild.mRectRes = llclamp(llmax(rw, rh), 4, target->mRes);
        mBuild.mRectHalf = 0.5f * (F32)mBuild.mRectRes * target->mCell;
    }
    else
    {
        target->mBandTarget = bandCount();
    }

    target->mBandTarget = llmax(llmax(target->mBandTarget, target->mBandCount), 1);
    mBuild.mBand = 0;
    mBuild.mEmptyRun = 0;
    mBuild.mChanged = false;
    mLastBandAt = mNow;
}

// Drops tiles for departed regions, then the least recently used beyond the
// cache cap. Erasing a tile also moves the flood generation - a walk in
// flight for the departed tile must not land on a fresh tile the same region
// handle re-created (restarted at geometry serial 1, so the serial gate alone
// would not stop it) - and drops its cached debug views.
void SSWorldField::evict()
{
    bool erased = false;
    for (auto it = mTiles.begin(); it != mTiles.end();)
    {
        if (!LLWorld::getInstance()->getRegionFromHandle(it->first))
        {
            sDrainDebug.erase(it->first);
            it = mTiles.erase(it);
            erased = true;
        }
        else
        {
            ++it;
        }
    }
    while ((S32)mTiles.size() > MAX_TILES)
    {
        auto oldest = mTiles.begin();
        for (auto it = mTiles.begin(); it != mTiles.end(); ++it)
        {
            if (it->second.mLastTouched < oldest->second.mLastTouched) oldest = it;
        }
        sDrainDebug.erase(oldest->first);
        mTiles.erase(oldest);
        erased = true;
    }
    if (erased) ++mFloodGeneration;
}

// One band step: capture, splice, then advance, stop early on empty sky, or
// commit. The depth readback is async (SSGLReadback): capture renders and
// submits, applyBand runs a step after the texels land, and the build waits a
// step while one is in flight.
bool SSWorldField::advanceBuild()
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(mBuild.mRegionHandle);
    Tile* tile = regionp ? tileFor(regionp, false) : nullptr;
    if (!tile)
    {
        mBuild.mActive = false;
        return false;
    }

    // A band readback is still in flight: neither splice it (the texels are
    // not here yet) nor render the shared capture target into again.
    if (mReadbackPending) return true;

    // The previous band's capture rendered and its readback landed; splice it
    // in and advance the build state that depends on it.
    if (mBuild.mJustCaptured)
    {
        mBuild.mJustCaptured = false;
        applyBand(*tile);
        ++mBuild.mBand;

// Full builds stop early once the sky has been genuinely empty for a few
        // consecutive bands; rect builds run to their target so the spliced columns
        // stay consistent with their neighbours.
        if (!mBuild.mRectOnly && mBuild.mEmptyRun >= EMPTY_BANDS_TO_STOP)
        {
            commitBuild(*tile);
            return false;
        }

        if (mBuild.mBand >= tile->mBandTarget)
        {
            commitBuild(*tile);
            return false;
        }

        return true;
    }

    if (mBuild.mBand >= tile->mBandTarget)
    {
        commitBuild(*tile);
        return false;
    }

    if (!captureBand(*tile))
    {
        // GL trouble - abandon rather than spin. The tile keeps its previous
        // contents and stays dirty, so the next update tries again.
        mBuild.mActive = false;
        return false;
    }

    mBuild.mJustCaptured = true;
    return true;
}

// One band capture: an ortho straight-down depth render whose frustum starts
// at the band's top, so everything above the band is behind the near plane
// and the readback is the highest surface *inside the band*.
bool SSWorldField::captureBand(Tile& tile)
{
    LL_PROFILE_GPU_ZONE("atmo world field band");

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;

    const F32 band_top = bandTopZ(mBuild.mBand, tile.mBandHeight);
    const F32 range = tile.mBandHeight + 2.f;

    S32 res;
    F32 half, centre_x, centre_y;
    if (mBuild.mRectOnly)
    {
        res = mBuild.mRectRes;
        half = mBuild.mRectHalf;
        centre_x = mBuild.mRectCentre.mV[VX];
        centre_y = mBuild.mRectCentre.mV[VY];
    }
    else
    {
        res = tile.mRes;
        half = regionp->getWidth() * 0.5f + 8.f;
        centre_x = regionp->getOriginAgent().mV[VX] + regionp->getWidth() * 0.5f;
        centre_y = regionp->getOriginAgent().mV[VY] + regionp->getWidth() * 0.5f;
    }

    const LLVector3 eye(centre_x, centre_y, band_top);

    const glm::mat4 saved_view = get_current_modelview();
    const glm::mat4 saved_proj = get_current_projection();
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;

    const glm::mat4 view = glm::lookAt(
        glm::vec3(eye.mV[VX], eye.mV[VY], eye.mV[VZ]),
        glm::vec3(eye.mV[VX], eye.mV[VY], eye.mV[VZ] - 1.f),
        glm::vec3(0.f, 1.f, 0.f));
    const glm::mat4 proj = glm::ortho(-half, half, -half, half, 0.f, range);

    set_current_modelview(view);
    set_current_projection(proj);
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_SUN_SHADOW3;

    LLCamera cam = *LLViewerCamera::getInstance();
    cam.setOrigin(eye);
    cam.setFar(range);

    LLVector3 frust[8];
    frust[0] = eye + LLVector3(-half, -half, 0.f);
    frust[1] = eye + LLVector3(half, -half, 0.f);
    frust[2] = eye + LLVector3(half, half, 0.f);
    frust[3] = eye + LLVector3(-half, half, 0.f);
    for (U32 i = 0; i < 4; i++)
    {
        frust[i + 4] = frust[i] + LLVector3(0.f, 0.f, -range);
    }
    cam.calcAgentFrustumPlanes(frust);
    cam.mFrustumCornerDist = 0.f;

    bool ok = true;
    if (mTarget.getWidth() != (U32)res)
    {
        mTarget.release();
        ok = mTarget.allocate(res, res, 0, true);
    }
    else
    {
        ok = true;
    }

    if (ok)
    {
        mTarget.bindTarget();
        mTarget.getViewport(gGLViewport);
        mTarget.clear();

        {
            static LLCullResult cull_result;

            gPipeline.pushRenderTypeMask();
            gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_AVATAR,
                                          LLPipeline::RENDER_TYPE_CONTROL_AV,
                                          LLPipeline::END_RENDER_TYPES);
            gPipeline.renderShadow(view, proj, cam, cull_result, true);
            gPipeline.popRenderTypeMask();
        }

        mTarget.flush();

        // <SS:Nexii> The band's depth lands via the shared SSGLReadback worker: the synchronous glReadPixels that used to block becomes a glGetTexImage on a dedicated GL thread, and applyBand() runs the step after the texels come back (see mJustCaptured). The worker writes only its own buffer; mDone copies into mBuild.mDepth on the main thread, so the Build never sees a partial read.
        mBuild.mDepth.assign((size_t)res * res, 0.f);
        mReadbackPending = true;
        const U32 tres = (U32)res;

        SSGLReadback::Job job;
        job.mTexture = mTarget.getDepth();
        job.mTarget = GL_TEXTURE_2D;
        job.mWidth = tres;
        job.mHeight = tres;
        job.mFormat = GL_DEPTH_COMPONENT;
        job.mType = GL_FLOAT;
        job.mDone = [this, tres](const U8* data, size_t bytes)
        {
            mReadbackPending = false;
            if (mClearPending)
            {
                mClearPending = false;
                clear();
                return;
            }
            const size_t n = (size_t)tres * tres;
            if (bytes >= n * sizeof(F32) && mBuild.mDepth.size() >= n)
            {
                memcpy(mBuild.mDepth.data(), data, n * sizeof(F32));
            }
        };
        if (!SSGLReadback::getInstance()->submit(job))
        {
            // Could not even stage the read - GL trouble. Abandon rather than
            // leave mReadbackPending stuck and the build spinning.
            mReadbackPending = false;
            mBuild.mJustCaptured = false;
            ok = false;
        }
    }

    set_current_modelview(saved_view);
    set_current_projection(saved_proj);
    LLViewerCamera::sCurCameraID = saved_camera;

    return ok;
}

// Grow the tile's flat band arrays one doubling at a time; the new region is
// open sky until a capture splices over it. Everything that indexes the arrays
// does so below mBandCount, which only grows after the band it names was
// ensured here.
void SSWorldField::ensureBands(Tile& tile, S32 bands)
{
    if (bands <= tile.mAllocBands) return;

    const S32 next = llclamp(llmax(bands, tile.mAllocBands * 2), 1, MAX_BANDS);
    const size_t layer = (size_t)tile.mRes * (size_t)tile.mRes;
    const size_t old_cells = (size_t)tile.mAllocBands * layer;
    const size_t new_cells = (size_t)next * layer;

    tile.mBandTop.resize(new_cells);
    std::fill(tile.mBandTop.begin() + old_cells, tile.mBandTop.end(), NO_SURFACE);
    tile.mBandFlags.resize(new_cells);
    std::fill(tile.mBandFlags.begin() + old_cells, tile.mBandFlags.end(), 0);
    tile.mAllocBands = next;
}

// Splices the captured band into the tile: per column, the highest surface
// inside the band, projected out of the depth readback. Full builds write
// every column; rect builds only the dirty rectangle's columns.
void SSWorldField::applyBand(Tile& tile)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD_GRID);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp || mBuild.mDepth.empty()) return;

    const S32 band = mBuild.mBand;
    ensureBands(tile, band + 1);

    const F32 band_top = bandTopZ(band, tile.mBandHeight);
    const F32 range = tile.mBandHeight + 2.f;
    const F32 hi = band_top - BOUNDARY_EPSILON;

    const S32 x0 = mBuild.mRectOnly ? mBuild.mRectX0 : 0;
    const S32 y0 = mBuild.mRectOnly ? mBuild.mRectY0 : 0;
    const S32 x1 = mBuild.mRectOnly ? mBuild.mRectX1 : tile.mRes;
    const S32 y1 = mBuild.mRectOnly ? mBuild.mRectY1 : tile.mRes;
    const S32 cap_res = mBuild.mRectOnly ? mBuild.mRectRes : tile.mRes;
    const F32 half = mBuild.mRectOnly ? mBuild.mRectHalf : regionp->getWidth() * 0.5f + 8.f;
    const F32 centre_x = mBuild.mRectOnly ? mBuild.mRectCentre.mV[VX]
                                          : regionp->getOriginAgent().mV[VX] + regionp->getWidth() * 0.5f;
    const F32 centre_y = mBuild.mRectOnly ? mBuild.mRectCentre.mV[VY]
                                          : regionp->getOriginAgent().mV[VY] + regionp->getWidth() * 0.5f;

    const F32 texel = (2.f * half) / (F32)cap_res;
    const F32 frust_min_x = centre_x - half;
    const F32 frust_min_y = centre_y - half;

    const F32 water_z = regionp->getWaterHeight();
    const bool sky = SSAtmoMagic::getInstance()->isSkyTrack();
    const F32 sky_floor = SSAtmoMagic::getInstance()->groundZero();

    const S32 stride = tile.mRes;
    F32* top_z = &tile.mBandTop[(size_t)band * (size_t)tile.mRes * (size_t)tile.mRes];
    U8* top_flags = &tile.mBandFlags[(size_t)band * tile.mRes * tile.mRes];

    U32 hits = 0;

    for (S32 cy = y0; cy < y1; ++cy)
    {
        for (S32 cx = x0; cx < x1; ++cx)
        {
            const F32 wx = regionp->getOriginAgent().mV[VX] + ((F32)cx + 0.5f) * tile.mCell;
            const F32 wy = regionp->getOriginAgent().mV[VY] + ((F32)cy + 0.5f) * tile.mCell;

            const size_t idx = (size_t)cy * stride + cx;

            F32 z = NO_SURFACE;
            U8 flags = 0;

            const F32 u = (wx - frust_min_x) / (2.f * half);
            const F32 v = (wy - frust_min_y) / (2.f * half);
            if (u >= 0.f && u < 1.f && v >= 0.f && v < 1.f)
            {
                const S32 tx = llmin((S32)(u * (F32)cap_res), cap_res - 1);
                const S32 ty = llmin((S32)(v * (F32)cap_res), cap_res - 1);
                const F32 d = mBuild.mDepth[(size_t)ty * cap_res + tx];
                if (d < DEPTH_MISS)
                {
                    z = band_top - d * range;
                    if (z > hi) z = hi;
                    flags = SSRainShadowMap::SURF_MAPPED;
                    ++hits;
                }
            }

            if (z > NO_SURFACE + 1.f)
            {
                // Water is the one surface the depth pass does not draw, so a
                // hit under the waterline is the seabed and the cell belongs
                // to the water above it - the rain shadow rule, at every band.
                if (!sky && z < water_z)
                {
                    z = water_z;
                    flags = SSRainShadowMap::SURF_MAPPED | SSRainShadowMap::SURF_WATER;
                }
            }
            else if (band == 0)
            {
                // The ground band falls back to the heightmap, exactly as the
                // rain shadow capture does for what it missed. Higher bands
                // leave unmapped cells open instead.
                if (sky)
                {
                    z = sky_floor;
                    flags = 0;
                }
                else
                {
                    const LLVector3 probe(wx, wy, water_z);
                    const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(probe);
                    z = llmax(land, water_z);
                    flags = SSRainShadowMap::SURF_FALLBACK
                            | ((water_z > land) ? SSRainShadowMap::SURF_WATER : 0);
                }
            }
            else
            {
                z = NO_SURFACE;
                flags = 0;
            }

            // Splice, and notice when the column actually changed so a
            // no-op edit does not bump the geometry serial.
            if (fabsf(top_z[idx] - z) > 0.01f || top_flags[idx] != flags)
            {
                mBuild.mChanged = true;
            }
            top_z[idx] = z;
            top_flags[idx] = flags;
        }
    }

    // Bands with content extend the tile's live band stack; a run of
    // genuinely empty bands ends a full build early.
    if (hits > 0)
    {
        if (band + 1 > tile.mBandCount) tile.mBandCount = band + 1;
        mBuild.mEmptyRun = 0;
    }
    else
    {
        ++mBuild.mEmptyRun;
    }
}

void SSWorldField::commitBuild(Tile& tile)
{
    if (mBuild.mChanged)
    {
        if (tile.mGeomSerial == 0xFFFFFFFFu)
        {
            tile.mGeomSerial = 1;
        }
        else
        {
            ++tile.mGeomSerial;
        }
    }

    if (!mBuild.mRectOnly)
    {
        ++mCaptureCount;
        tile.mValid = true;
    }
    else
    {
        ++mDirtyCaptures;
    }

    // Rect cleared, target reset, timestamps refreshed. The dirty rect is a
    // one-shot: the re-peel splices exactly what was marked.
    tile.mDirtyX0 = tile.mDirtyY0 = 0;
    tile.mDirtyX1 = tile.mDirtyY1 = 0;
    tile.mDirty = false;
    tile.mBandTarget = 0;
    tile.mCaptureTime = mNow;
    tile.mLastTouched = mNow;
    mBuild.mActive = false;

// The connectivity labels follow every commit, not only the ones that changed
    // something: they also serve their first fill, and a commit that changed
    // nothing left mAirSerial already matching, so scheduleFlood's serial check
    // makes the walk a no-op store.
    if (tile.mAirSerial != tile.mGeomSerial)
    {
        scheduleFlood(tile);
    }
}

// Resolves the landing-surface grid for a region - SSRainShadowMap's exact
// contract, sourced from the band stack, not a private capture. The first
// thing a falling drop meets is the column's highest surface, so bands are
// scanned top-down and the first hit wins.
bool SSWorldField::buildSurfaceGrid(U64 region_handle, S32 n, SSRainShadowMap::SurfaceGrid& out)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD_GRID);

    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid) return false;

    const Tile& tile = it->second;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return false;

    n = llclamp(n, 16, 512);
    const F32 width = regionp->getWidth();

    out.mRegionHandle = region_handle;
    out.mN = n;
    out.mCell = width / (F32)n;
    out.mGeomSerial = tile.mGeomSerial;
    out.mZ.assign((size_t)n * n, -FLT_MAX);
    out.mFlags.assign((size_t)n * n, 0);
    out.mAbove.assign((size_t)n * n, 0.f);

    const F32 water_z = regionp->getWaterHeight();
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();

    for (S32 gy = 0; gy < n; ++gy)
    {
        for (S32 gx = 0; gx < n; ++gx)
        {
            const S32 cx = llclamp((S32)(((F32)gx + 0.5f) * (F32)tile.mRes / (F32)n), 0, tile.mRes - 1);
            const S32 cy = llclamp((S32)(((F32)gy + 0.5f) * (F32)tile.mRes / (F32)n), 0, tile.mRes - 1);

            const size_t col = (size_t)cy * tile.mRes + cx;
            const size_t oidx = (size_t)gy * n + gx;

            F32 z = -FLT_MAX;
            U8 flags = 0;

            for (S32 b = tile.mBandCount - 1; b >= 0; --b)
            {
                const size_t bi = (size_t)b * (size_t)tile.mRes * (size_t)tile.mRes + col;
                if (tile.mBandTop[bi] > -FLT_MAX * 0.5f)
                {
                    z = tile.mBandTop[bi];
                    flags = tile.mBandFlags[bi];
                    break;
                }
            }

            if (z > -FLT_MAX * 0.5f)
            {
                if (!sky && z < water_z)
                {
                    out.mZ[oidx] = water_z;
                    out.mFlags[oidx] = SSRainShadowMap::SURF_MAPPED | SSRainShadowMap::SURF_WATER;
                }
                else
                {
                    out.mZ[oidx] = z;
                    out.mFlags[oidx] = flags | SSRainShadowMap::SURF_MAPPED;

// Same figure the rain shadow builder derives: metres over the
                    // terrain-or-water reference (the sky track floor in a skybox),
                    // so both sources hand consumers identical ground-vs-structure
                    // data and the SSWorldFieldSurfaceTop switch stays behaviour-
                    // neutral.
                    F32 ground;
                    if (sky)
                    {
                        ground = sky_floor;
                    }
                    else
                    {
                        const LLVector3 centre(regionp->getOriginAgent().mV[VX] + out.axis(gx),
                                               regionp->getOriginAgent().mV[VY] + out.axis(gy),
                                               water_z);
                        ground = llmax(LLWorld::getInstance()->resolveLandHeightAgent(centre), water_z);
                    }
                    out.mAbove[oidx] = llmax(z - ground, 0.f);
                }
            }
            else if (sky)
            {
                out.mZ[oidx] = sky_floor;
                out.mFlags[oidx] = 0;
            }
            else
            {
                const LLVector3 centre(regionp->getOriginAgent().mV[VX] + out.axis(gx),
                                       regionp->getOriginAgent().mV[VY] + out.axis(gy),
                                       water_z);
                const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(centre);
                out.mZ[oidx] = llmax(land, water_z);
                out.mFlags[oidx] = SSRainShadowMap::SURF_FALLBACK | ((water_z > land) ? SSRainShadowMap::SURF_WATER : 0);
            }
        }
    }

    return true;
}

void SSWorldField::validTiles(std::vector<std::pair<U64, U32> >& out) const
{
    out.clear();
    out.reserve(mTiles.size());
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid)
        {
            out.emplace_back(entry.first, entry.second.mGeomSerial);
        }
    }
}

bool SSWorldField::surfaceTop(const LLVector3& pos_agent, F32& z, U8& flags) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);

    const size_t col = (size_t)cy * tile->mRes + cx;
    for (S32 b = tile->mBandCount - 1; b >= 0; --b)
    {
        const size_t bi = (size_t)b * (size_t)tile->mRes * (size_t)tile->mRes + col;
        if (tile->mBandTop[bi] > -FLT_MAX * 0.5f)
        {
            z = tile->mBandTop[bi];
            flags = tile->mBandFlags[bi];
            return true;
        }
    }
    return false;
}

bool SSWorldField::coverageAt(const LLVector3& pos_agent, bool& outdoor, F32& buried_depth) const
{
    outdoor = true;
    buried_depth = 0.f;

    F32 top = 0.f;
    U8 flags = 0;
    if (!surfaceTop(pos_agent, top, flags)) return false;

    // Standing on the top surface counts as outdoors; anything below it is
    // under the column's sky-open top by however much.
    if (top < pos_agent.mV[VZ] - 0.01f)
    {
        return true;
    }

    outdoor = false;
    buried_depth = llmax(0.f, top - pos_agent.mV[VZ]);
    return true;
}

bool SSWorldField::coverageDetail(const LLVector3& pos_agent, bool& covered,
                                  F32& ceiling_z, F32& column_top_z) const
{
    covered = false;
    ceiling_z = 0.f;
    column_top_z = 0.f;

    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)cy * tile->mRes + cx;

    // The half metre of grace keeps the surface being stood on from reading
    // as its own ceiling - a capture texel of the floor under the camera can
    // land a hair above the camera's own feet.
    const F32 over = pos_agent.mV[VZ] + 0.5f;

    bool any = false;
    for (S32 b = 0; b < tile->mBandCount; ++b)
    {
        const size_t bi = (size_t)b * (size_t)tile->mRes * (size_t)tile->mRes + col;
        const F32 z = tile->mBandTop[bi];
        if (z <= -FLT_MAX * 0.5f) continue;

        any = true;
        column_top_z = llmax(column_top_z, z);
        if (z > over && (!covered || z < ceiling_z))
        {
            covered = true;
            ceiling_z = z;
        }
    }

    return any;
}

// <SS:Nexii> Air connectivity lookup: the band the point stands in, read from the labels the flood stored, or AIR_UNKNOWN when nothing is current - after an edit, before the first flood, or off-tile.
U8 SSWorldField::airLabelAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return AIR_UNKNOWN;
    if (tile->mAirLabel.empty() || tile->mAirSerial != tile->mGeomSerial) return AIR_UNKNOWN;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return AIR_UNKNOWN;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const S32 band = llclamp((S32)(pos_agent.mV[VZ] / tile->mBandHeight), 0, tile->mBandCount - 1);

    const size_t bi = ((size_t)band * tile->mRes + cy) * tile->mRes + cx;
    return (bi < tile->mAirLabel.size()) ? tile->mAirLabel[bi] : (U8)AIR_UNKNOWN;
}

// Occlusion depth behind airLabelAt: the flood's distance walk per band-cell,
// gated by the same serial check so a stale walk is never served.
U32 SSWorldField::airDepthAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return AIR_DEPTH_UNREACHED;
    if (tile->mAirDepth.empty() || tile->mAirSerial != tile->mGeomSerial) return AIR_DEPTH_UNREACHED;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return AIR_DEPTH_UNREACHED;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const S32 band = llclamp((S32)(pos_agent.mV[VZ] / tile->mBandHeight), 0, tile->mBandCount - 1);

    const size_t bi = ((size_t)band * tile->mRes + cy) * tile->mRes + cx;
    return (bi < tile->mAirDepth.size()) ? (U32)tile->mAirDepth[bi] : AIR_DEPTH_UNREACHED;
}

// <SS:Nexii> The enclosure spectrum at a point. The raw band-cell label
// answers only where the point's own band-cell is air - the mezzanine case -
// so a point sitting inside a SOLID cell is re-resolved against the column's
// stored surfaces: above the band's surface is open sub-band air, and it
// inherits the nearest air label above it in the column (the air mass it
// physically opens into); below the surface is the implied solid body, where
// the store has no answer. Everything else rides the labels' own serial gate.
F32 SSWorldField::enclosureAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return -1.f;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return -1.f;

    return enclosureInRegion(regionp, *tile, pos_agent);
}

// The bulk form for callers that walk one region's cells (the surface field's
// window stitch): same answer, region and tile resolved once instead of per
// point.
F32 SSWorldField::enclosureAtRegion(U64 region_handle, const LLVector3& pos_agent) const
{
    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid) return -1.f;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return -1.f;

    return enclosureInRegion(regionp, it->second, pos_agent);
}

F32 SSWorldField::enclosureInRegion(const LLViewerRegion* regionp, const Tile& tile,
                                    const LLVector3& pos_agent) const
{
    if (tile.mAirLabel.empty() || tile.mAirSerial != tile.mGeomSerial) return -1.f;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile.mCell), 0, tile.mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile.mCell), 0, tile.mRes - 1);
    const S32 band = llclamp((S32)(pos_agent.mV[VZ] / tile.mBandHeight), 0, tile.mBandCount - 1);
    const size_t cells = (size_t)tile.mBandCount * tile.mRes * tile.mRes;
    if (cells > tile.mAirLabel.size()) return -1.f;

    auto labelAt = [&](S32 b) -> U8
    {
        const size_t bi = ((size_t)b * tile.mRes + cy) * tile.mRes + cx;
        return (bi < tile.mAirLabel.size()) ? tile.mAirLabel[bi] : (U8)AIR_UNKNOWN;
    };
    auto topAt = [&](S32 b) -> F32
    {
        const size_t bi = ((size_t)b * tile.mRes + cy) * tile.mRes + cx;
        return (bi < tile.mBandTop.size()) ? tile.mBandTop[bi] : NO_SURFACE;
    };

    U8 label = labelAt(band);
    S32 air_band = band;    // the band whose depth figure answers for the point
    if (label == AIR_SOLID)
    {
        // Sub-band resolution: the band's own surface decides whether the
        // point is in the open air above it or inside the implied solid.
        if (pos_agent.mV[VZ] <= topAt(band) + 0.01f) return -1.f;

        label = AIR_UNKNOWN;
        for (S32 b = band + 1; b < tile.mBandCount; ++b)
        {
            const U8 above = labelAt(b);
            if (above == AIR_SOLID) continue;
            label = above;
            air_band = b;
            break;
        }
        // No air band anywhere above: the sliver of sub-band air is sealed
        // under a full stack of structure - maximally enclosed, not open.
        if (label == AIR_UNKNOWN || label == AIR_SOLID) return 1.f;
    }

    switch (label)
    {
        case AIR_OUTDOORS: return 0.f;
        case AIR_INTERIOR: return 1.f;
        case AIR_SHELTERED:
        {
            // The depth figure of the resolved air band - airDepthAt would
            // re-derive the point's own (solid) band and answer unreached.
            const size_t di = ((size_t)air_band * tile.mRes + cy) * tile.mRes + cx;
            const U16 d = (di < tile.mAirDepth.size()) ? tile.mAirDepth[di] : (U16)AIR_DEPTH_UNREACHED;
            if (d == AIR_DEPTH_UNREACHED) return 1.f;
            const F32 metres = (F32)d * tile.mCell;
            return metres / (metres + SS_WF_ENCLOSURE_TAU_M);
        }
        default: return -1.f;
    }
}

// <SS:Nexii> The precomputed wall profile at a point: the four cardinal
// distances the lattice holds for the point's band, in the side-probe
// contract (metres, saturated at the reach cap).
bool SSWorldField::acousticAt(const LLVector3& pos_agent, F32 wall[4]) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return false;

    const Tile::Acoustic& ac = tile->mAcoustic;
    if (ac.mLatRes < 1 || ac.mSerial != tile->mGeomSerial) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const S32 lx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / ac.mLatCell), 0, ac.mLatRes - 1);
    const S32 ly = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / ac.mLatCell), 0, ac.mLatRes - 1);
    const S32 band = llclamp((S32)(pos_agent.mV[VZ] / tile->mBandHeight), 0, tile->mBandCount - 1);

    const size_t base = ((size_t)band * ac.mLatRes * ac.mLatRes + (size_t)ly * ac.mLatRes + (size_t)lx) * 4u;
    if (base + 3 >= ac.mWall.size()) return false;

    for (S32 i = 0; i < 4; ++i) wall[i] = ac.mWall[base + i];
    return true;
}

// Share of a tile's band-cells carrying a current label - 1.0 once the first
// flood has landed and nothing has edited since. The overlay reads this to
// tell a settled field from one still catching up.
F32 SSWorldField::airCoverage(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid) return 0.f;

    const Tile& tile = it->second;
    if (tile.mAirLabel.empty() || tile.mAirSerial != tile.mGeomSerial) return 0.f;
    if (tile.mBandCount < 1 || tile.mRes < 1) return 0.f;

    const size_t cells = (size_t)tile.mBandCount * (size_t)tile.mRes * (size_t)tile.mRes;
    if (tile.mAirLabel.size() < cells) return 0.f;

    size_t labelled = 0;
    for (size_t i = 0; i < cells; ++i)
    {
        const U8 l = tile.mAirLabel[i];
        if (l != AIR_UNKNOWN) ++labelled;
    }
    return (F32)labelled / (F32)cells;
}

// The DRAINAGE_NETWORK core over one landing surface. Barnes' priority flood
// is the O(n log n) way to fill every depression to its spill elevation:
// drains (the grid border, water, unmapped sky) seed the heap at their own
// height, each cell pops once at the lowest spill reaching it, and a cell
// whose spill stands meaningfully above its own surface is standing water.
// Flow directions then run down the FILLED surface, so a pool's water heads
// for its outlet instead of into its own floor.
bool SSWorldField::buildDrainage(const SSRainShadowMap::SurfaceGrid& grid, Drainage& out) const
{
    out.mSpill.clear();
    out.mPool.clear();
    out.mD8.clear();

    const S32 n = grid.mN;
    if (n < 3 || grid.mZ.size() < (size_t)n * n) return false;

    const size_t count = (size_t)n * n;
    out.mSpill.assign(count, -FLT_MAX);
    out.mPool.assign(count, 0);
    out.mD8.assign(count, 4);

    // The hydrological domain: cells with any surface flag, water excluded -
    // water, unmapped sky and the grid border are drains the fill opens out at.
    // The height guard keeps a NODATA cell that somehow carried flags from
    // seeding a -FLT_MAX spill that would poison every fill elevation it
    // reached.
    auto land = [&](size_t i)
    {
        const U8 f = grid.mFlags[i];
        return (f & SSRainShadowMap::SURF_WATER) == 0 && f != 0
            && grid.mZ[i] > -FLT_MAX * 0.5f;
    };

    static const S32 DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    static const S32 DY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

    struct Node
    {
        F32 mSpill;
        S32 mIndex;
    };
    struct NodeHeapOrder
    {
        bool operator()(const Node& a, const Node& b) const
        {
            // Min-heap on spill; index tie-break keeps the walk deterministic.
            if (a.mSpill != b.mSpill) return a.mSpill > b.mSpill;
            return a.mIndex > b.mIndex;
        }
    };
    std::priority_queue<Node, std::vector<Node>, NodeHeapOrder> heap;

    std::vector<U8> visited(count, 0);

    auto seed = [&](S32 i)
    {
        if (visited[i] || !land((size_t)i)) return;
        visited[i] = 1;
        out.mSpill[i] = grid.mZ[i];
        heap.push({ out.mSpill[i], i });
    };

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const S32 i = y * n + x;
            if (!land((size_t)i)) continue;

            if (x == 0 || y == 0 || x == n - 1 || y == n - 1)
            {
                seed(i);
                continue;
            }

            for (S32 d = 0; d < 8; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
                if (!land((size_t)ny * n + nx))
                {
                    seed(i);
                    break;
                }
            }
        }
    }

    while (!heap.empty())
    {
        const S32 c = heap.top().mIndex;
        heap.pop();

        const F32 spill_c = out.mSpill[(size_t)c];
        const S32 cx = c % n;
        const S32 cy = c / n;

        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

            const S32 ni = ny * n + nx;
            if (visited[ni] || !land((size_t)ni)) continue;

            visited[ni] = 1;
            out.mSpill[ni] = llmax(spill_c, grid.mZ[(size_t)ni]);
            heap.push({ out.mSpill[ni], ni });
        }
    }

// Pool membership: the fill is standing water where it rises clear of the
    // surface - the depression's depth, not a sampled dip. Five centimetres sits
    // below the puddle thresholds that consume the mask, so this only gates
    // genuine standing water, never a capture texel's jitter.
    static const F32 POOL_FILL_EPS = 0.05f;

    // Flow: D8 down the filled surface, 3x3-indexed ((dy+1)*3 + (dx+1)), 4 when
    // nothing is lower - a pool floor, a sink, or a drain cell.
    for (S32 c = 0; c < (S32)count; ++c)
    {
        const size_t i = (size_t)c;
        if (!land(i)) continue;

        if (out.mSpill[i] - grid.mZ[i] > POOL_FILL_EPS)
        {
            out.mPool[i] = 1;
        }

        const S32 cx = c % n;
        const S32 cy = c / n;
        const F32 here = out.mSpill[i];

        S32 best_dir = 4;
        F32 best_z = here;
        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

            const size_t ni = (size_t)ny * n + nx;
            if (!land(ni)) continue;

            // Dropping the FILLED elevation from the outlet chain windows out
            // the water-plane cases a raw-surface D8 gets wrong: a pool run
            // into its own floor.
            const F32 nz = out.mSpill[ni];
            if (nz < best_z - 0.01f)
            {
                best_z = nz;
                best_dir = (DY[d] + 1) * 3 + (DX[d] + 1);
            }
        }
        out.mD8[i] = (U8)best_dir;
    }

    return true;
}

// The flood itself, run on the general worker queue against a snapshot. A
// band-cell is solid where the capture found a surface in that band; air
// otherwise. Every air cell in the top band, and every air cell on the
// horizontal border, starts OUTSIDE; the flood walks 6-connected through air.
// The touching classification then splits the reachable air: a cell whose
// column carries structure above it is covered, everything else is OUTDOORS.
// Covered air earns SHELTERED or INTERIOR by an aperture budget handed across
// each outdoors-sheltered touching: the opening's porch (the outdoors cells
// touching it, clustered 6-connected so one opening is one aperture) seeds
// sqrt(aperture cells) - the opening's linear width, not its area - and every
// cell step inward spends one, widest-path propagated so the best opening
// wins. A cave mouth therefore stays sheltered tens of cells deep while a
// window's budget dies a cell or two past the glass; air every budget missed
// is INTERIOR, exactly what the wind solve wants to skip and the soundscape
// wants to know it is standing in. Evidence-only in the same spirit as the
// probe carve: a passage narrower than a cell stays uncounted rather than
// invented. The second walk measures occlusion: every outdoors or sheltered
// cell's graph distance in cells to the nearest OUTDOORS cell, the "how
// enclosed is this air" figure the acoustic channel's travel times and sparse
// air solve both read. Interior cells never reach the outdoors through air,
// so they keep AIR_DEPTH_UNREACHED - sealed is maximally enclosed by
// construction. Transient working set at the densest legal tile (1024^2 x 24
// bands) is a few hundred MB on the worker; typical builds run a few bands
// and a fraction of that.
static void ss_wf_flood(S32 res, S32 bands, const std::vector<F32>& band_top,
                        std::vector<U8>& label, std::vector<U16>& depth)
{
    const size_t layer = (size_t)res * res;
    const size_t cells = layer * (size_t)bands;
    label.assign(cells, SSWorldField::AIR_INTERIOR);
    depth.assign(cells, (U16)SSWorldField::AIR_DEPTH_UNREACHED);

    std::vector<S32> queue;
    queue.reserve(cells / 8);

    auto isAir = [&](size_t i) { return band_top[i] <= -FLT_MAX * 0.5f; };

    for (size_t i = 0; i < cells; ++i)
    {
        if (!isAir(i))
        {
            label[i] = SSWorldField::AIR_SOLID;
            continue;
        }

        const S32 b = (S32)(i / layer);
        const S32 y = (S32)((i % layer) / res);
        const S32 x = (S32)(i % res);
        if (b == bands - 1 || x == 0 || y == 0 || x == res - 1 || y == res - 1)
        {
            label[i] = SSWorldField::AIR_OUTDOORS;
            queue.push_back((S32)i);
        }
    }

    static const S32 DX[6] = { 1, -1, 0, 0, 0, 0 };
    static const S32 DY[6] = { 0, 0, 1, -1, 0, 0 };
    static const S32 DB[6] = { 0, 0, 0, 0, 1, -1 };

    for (size_t head = 0; head < queue.size(); ++head)
    {
        const S32 i = queue[head];
        const S32 b = (S32)((size_t)i / layer);
        const S32 y = (S32)(((size_t)i % layer) / res);
        const S32 x = (S32)((size_t)i % res);

        for (S32 d = 0; d < 6; ++d)
        {
            const S32 nx = x + DX[d], ny = y + DY[d], nb = b + DB[d];
            if (nx < 0 || ny < 0 || nb < 0 || nx >= res || ny >= res || nb >= bands) continue;

            const size_t j = ((size_t)nb * res + ny) * (size_t)res + nx;
            if (label[j] != SSWorldField::AIR_INTERIOR) continue;

            label[j] = SSWorldField::AIR_OUTDOORS;
            queue.push_back((S32)j);
        }
    }

    // ---- the touching classification: outdoors / sheltered / indoors ----
    auto xOf = [&](size_t i) { return (S32)(i % res); };
    auto yOf = [&](size_t i) { return (S32)((i % layer) / res); };
    auto bOf = [&](size_t i) { return (S32)(i / layer); };
    auto at = [&](S32 x, S32 y, S32 b) { return ((size_t)b * res + y) * (size_t)res + x; };

    // Topmost solid band per column: an air cell above it has nothing over it
    // and the elements land straight on it - outdoors, whatever the flood
    // walked through to reach it.
    std::vector<S32> top_solid(layer, -1);
    for (S32 b = 0; b < bands; ++b)
    {
        const size_t base = (size_t)b * layer;
        for (S32 c = 0; c < layer; ++c)
        {
            if (!isAir(base + c)) top_solid[c] = b;
        }
    }

    // Covered air: outside-connected but with structure standing over it.
    std::vector<U8> covered(cells, 0);
    for (size_t i = 0; i < cells; ++i)
    {
        if (label[i] != SSWorldField::AIR_OUTDOORS) continue;
        if (bOf(i) < top_solid[i % layer]) covered[i] = 1;
    }

    // Porch: the outdoors cells touching covered air, clustered 6-connected
    // so one opening is one aperture. Two windows in one wall stay separate
    // clusters (the porch cells are wall-face neighbours only within the
    // opening), while a door and its adjacent window merge into the one
    // opening they physically are. Stored as the cluster's budget -
    // sqrt(cells), the opening's linear width - so a metre-scale window hands
    // the same shelter in cells whatever the column density is.
    std::vector<U16> porch(cells, 0);
    std::vector<S32> cluster;
    for (size_t i = 0; i < cells; ++i)
    {
        if (label[i] != SSWorldField::AIR_OUTDOORS || covered[i] || porch[i]) continue;

        bool touches = false;
        const S32 ix = xOf(i), iy = yOf(i), ib = bOf(i);
        for (S32 d = 0; d < 6 && !touches; ++d)
        {
            const S32 nx = ix + DX[d], ny = iy + DY[d], nb = ib + DB[d];
            if (nx < 0 || ny < 0 || nb < 0 || nx >= res || ny >= res || nb >= bands) continue;
            touches = covered[at(nx, ny, nb)] != 0;
        }
        if (!touches) continue;

        cluster.clear();
        cluster.push_back((S32)i);
        porch[i] = 1;   // visited mark; the real budget lands after the walk
        for (size_t head = 0; head < cluster.size(); ++head)
        {
            const S32 j = cluster[head];
            const S32 jx = xOf(j), jy = yOf(j), jb = bOf(j);

            for (S32 d = 0; d < 6; ++d)
            {
                const S32 nx = jx + DX[d], ny = jy + DY[d], nb = jb + DB[d];
                if (nx < 0 || ny < 0 || nb < 0 || nx >= res || ny >= res || nb >= bands) continue;

                const size_t k = at(nx, ny, nb);
                if (label[k] != SSWorldField::AIR_OUTDOORS || covered[k] || porch[k]) continue;

                bool k_touches = false;
                for (S32 e = 0; e < 6 && !k_touches; ++e)
                {
                    const S32 mx = nx + DX[e], my = ny + DY[e], mb = nb + DB[e];
                    if (mx < 0 || my < 0 || mb < 0 || mx >= res || my >= res || mb >= bands) continue;
                    k_touches = covered[at(mx, my, mb)] != 0;
                }
                if (!k_touches) continue;

                porch[k] = 1;
                cluster.push_back((S32)k);
            }
        }

        // sqrt(aperture) fits a U16 by construction: a cluster cannot hold
        // more cells than the tile (25.2M at the densest legal setting), whose
        // root is ~5020.
        const U16 budget = (U16)(sqrtf((F32)cluster.size()) + 0.5f);
        for (const S32 j : cluster)
        {
            porch[j] = budget;
        }
    }

    // Budget propagation over covered air, widest path first: each cell's
    // remaining budget is the best (seed budget - steps) over every inward
    // path, so the strongest opening decides how deep the shelter reaches.
    // First pop is final (later entries only ever carry smaller budgets);
    // spent cells fall back to INTERIOR.
    std::vector<U16> reach(cells, 0);
    std::priority_queue<std::pair<U16, S32> > heap;
    for (size_t i = 0; i < cells; ++i)
    {
        if (!covered[i]) continue;

        U16 best = 0;
        const S32 ix = xOf(i), iy = yOf(i), ib = bOf(i);
        for (S32 d = 0; d < 6; ++d)
        {
            const S32 nx = ix + DX[d], ny = iy + DY[d], nb = ib + DB[d];
            if (nx < 0 || ny < 0 || nb < 0 || nx >= res || ny >= res || nb >= bands) continue;

            const size_t j = at(nx, ny, nb);
            if (label[j] != SSWorldField::AIR_OUTDOORS || covered[j]) continue;
            best = llmax(best, porch[j]);
        }
        if (best > 0)
        {
            reach[i] = best;
            heap.emplace(best, (S32)i);
        }
    }

    while (!heap.empty())
    {
        const U16 b = heap.top().first;
        const S32 i = heap.top().second;
        heap.pop();
        if (b != reach[(size_t)i]) continue;    // a stronger seed already passed
        if (b <= 1) continue;                   // nothing left to hand inward

        const S32 ix = xOf(i), iy = yOf(i), ib = bOf(i);
        for (S32 d = 0; d < 6; ++d)
        {
            const S32 nx = ix + DX[d], ny = iy + DY[d], nb = ib + DB[d];
            if (nx < 0 || ny < 0 || nb < 0 || nx >= res || ny >= res || nb >= bands) continue;

            const size_t j = at(nx, ny, nb);
            if (!covered[j] || reach[j] >= b - 1) continue;

            reach[j] = b - 1;
            heap.emplace((U16)(b - 1), (S32)j);
        }
    }

    for (size_t i = 0; i < cells; ++i)
    {
        if (!covered[i]) continue;
        label[i] = reach[i] > 0 ? (U8)SSWorldField::AIR_SHELTERED
                                : (U8)SSWorldField::AIR_INTERIOR;
    }

    // Occlusion depth, one BFS from the whole outdoors set over outdoors and
    // sheltered air. Interior air is unreachable and stays marked.
    // Distances saturate at AIR_DEPTH_UNREACHED - 1: the sentinel must stay
    // exclusive to "unvisited", or a cell whose true distance hit 0xFFFF would
    // read as never visited and the walk would loop on it forever.
    {
        std::vector<S32> depth_q;
        depth_q.reserve(queue.size());
        for (size_t i = 0; i < cells; ++i)
        {
            if (label[i] == SSWorldField::AIR_OUTDOORS)
            {
                depth[i] = 0;
                depth_q.push_back((S32)i);
            }
        }

        for (size_t head = 0; head < depth_q.size(); ++head)
        {
            const S32 i = depth_q[head];
            const S32 b = (S32)((size_t)i / layer);
            const S32 y = (S32)(((size_t)i % layer) / res);
            const S32 x = (S32)((size_t)i % res);

            for (S32 d = 0; d < 6; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d], nb = b + DB[d];
                if (nx < 0 || ny < 0 || nb < 0 || nx >= res || ny >= res || nb >= bands) continue;

                const size_t j = ((size_t)nb * res + ny) * (size_t)res + nx;
                if (label[j] != SSWorldField::AIR_OUTDOORS && label[j] != SSWorldField::AIR_SHELTERED) continue;
                if (depth[j] != SSWorldField::AIR_DEPTH_UNREACHED) continue;

                depth[j] = (U16)llmin((U32)depth[i] + 1u,
                                      SSWorldField::AIR_DEPTH_UNREACHED - 1u);
                depth_q.push_back((S32)j);
            }
        }
    }
}

// <SS:Nexii> The acoustic lattice: one precomputed wall distance per
// cardinal, per band, per coarse lattice cell - the room-size and occlusion
// questions the soundscape otherwise re-raycasts every probe cycle. Walks
// the labels the flood just made: from each probe's band-cell, step
// horizontally through air until a solid band-cell stops it, the air count
// times the capture cell size being the wall distance in metres. A direction
// that leaves the tile or runs out of reach reads as open at the reach cap,
// matching the side raycast's own "nothing within 50m" convention (the cap
// sits above it, so a lattice open can never count as a wall hit). Runs on
// the flood's worker job; anchor and staleness ride the flood's own gates.
static void ss_wf_acoustic(S32 res, S32 bands, S32 lat_res, F32 cell_m,
                           const std::vector<U8>& label, std::vector<F32>& wall)
{
    wall.clear();
    if (lat_res < 1 || lat_res > res || cell_m <= 0.f) return;

    const size_t lat_layer = (size_t)lat_res * lat_res;
    wall.assign(lat_layer * (size_t)bands * 4u, SS_WF_ACOUSTIC_REACH_M);

    static const S32 DX[4] = { 1, -1, 0, 0 };
    static const S32 DY[4] = { 0, 0, 1, -1 };

    const S32 reach_cells = llclamp((S32)(SS_WF_ACOUSTIC_REACH_M / cell_m), 1, res);

    for (S32 b = 0; b < bands; ++b)
    {
        for (S32 ly = 0; ly < lat_res; ++ly)
        {
            for (S32 lx = 0; lx < lat_res; ++lx)
            {
                // The lattice cell's centre, back onto the capture grid.
                const S32 cx = llclamp((S32)(((F32)lx + 0.5f) * (F32)res / (F32)lat_res), 0, res - 1);
                const S32 cy = llclamp((S32)(((F32)ly + 0.5f) * (F32)res / (F32)lat_res), 0, res - 1);

                F32* out = &wall[((size_t)b * lat_layer + (size_t)ly * lat_res + lx) * 4u];

                for (S32 d = 0; d < 4; ++d)
                {
                    S32 x = cx, y = cy, steps = 0;
                    while (steps < reach_cells)
                    {
                        x += DX[d]; y += DY[d];
                        if (x < 0 || y < 0 || x >= res || y >= res)
                        {
                            steps = reach_cells;    // off-tile: open, not a nearby wall
                            break;
                        }
                        if (label[((size_t)b * res + y) * (size_t)res + x] == SSWorldField::AIR_SOLID) break;
                        ++steps;
                    }
                    out[d] = llmin((F32)steps * cell_m, SS_WF_ACOUSTIC_REACH_M);
                }
            }
        }
    }
}

void SSWorldField::scheduleFlood(Tile& tile)
{
    if (mFloodBusy) return;                     // this commit's successor will reschedule
    if (tile.mBandCount < 1 || tile.mRes < 1) return;

    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    LL::WorkQueue::ptr_t main = LL::WorkQueue::getInstance("mainloop");
    if (!general || !main) return;              // no worker: labels stay AIR_UNKNOWN, consumers cope

    mFloodBusy = true;
    const U32 generation = mFloodGeneration;
    const U64 region = tile.mRegionHandle;
    const U32 serial = tile.mGeomSerial;
    const S32 res = tile.mRes;
    const S32 bands = tile.mBandCount;

    // Snapshot: the walk must never read the live band stack, which the next
    // build splices on the main thread while the worker is mid-flood.
    if (tile.mBandTop.size() < (size_t)bands * res * res) return;   // lazy-alloc invariant
    auto snapshot = std::make_shared<std::vector<F32> >(
        tile.mBandTop.begin(),
        tile.mBandTop.begin() + (size_t)bands * res * res);
    auto labels = std::make_shared<std::vector<U8> >();
    auto depths = std::make_shared<std::vector<U16> >();
    auto walls = std::make_shared<std::vector<F32> >();

    // The acoustic lattice runs at a coarse multiple of the capture grid -
    // near 8m per probe, never finer than the capture itself.
    const S32 lat_res = llmin(llclamp((S32)llround((F32)res * tile.mCell / 8.f), 8, 64), res);
    const F32 cell_m = tile.mCell;

    main->postTo(
        general,
        [res, bands, lat_res, cell_m, snapshot, labels, depths, walls]()
        {
            ss_wf_flood(res, bands, *snapshot, *labels, *depths);
            ss_wf_acoustic(res, bands, lat_res, cell_m, *labels, *walls);
            return true;
        },
        [this, generation, region, serial, lat_res, cell_m, labels, depths, walls](bool)
        {
            mFloodBusy = false;
            if (generation != mFloodGeneration) return;

            auto it = mTiles.find(region);
            if (it == mTiles.end() || !it->second.mValid) return;
            if (it->second.mGeomSerial != serial) return;   // edited mid-walk; the next commit refloods

            it->second.mAirLabel = std::move(*labels);
            it->second.mAirDepth = std::move(*depths);
            it->second.mAirSerial = serial;

            it->second.mAcoustic.mLatRes = lat_res;
            it->second.mAcoustic.mLatCell = (lat_res > 0) ? (F32)it->second.mRes * it->second.mCell / (F32)lat_res : 0.f;
            it->second.mAcoustic.mWall = std::move(*walls);
            it->second.mAcoustic.mSerial = serial;
        });
}

// A band's surface hue for the overlay: the store's vertical resolution
// reads as colour - blue at the floor through green to ember red at the
// ceiling.
static LLColor4 ss_wf_band_hue(F32 t, F32 alpha)
{
    t = llclamp(t, 0.f, 1.f);
    const F32 c0[3] = { 0.25f, 0.5f, 1.f };
    const F32 c1[3] = { 0.3f, 1.f, 0.4f };
    const F32 c2[3] = { 1.f, 0.45f, 0.2f };
    const F32* lo = (t < 0.5f) ? c0 : c1;
    const F32* hi = (t < 0.5f) ? c1 : c2;
    const F32 u = (t < 0.5f) ? t * 2.f : (t - 0.5f) * 2.f;
    return LLColor4(lerp(lo[0], hi[0], u), lerp(lo[1], hi[1], u),
                    lerp(lo[2], hi[2], u), alpha);
}

// The world field's own overlay: what the capture resolved, what the air flood
// decided with its occlusion depth, and what the drainage pass reads - view
// picked by SSWorldFieldDebugView, distance-thinned like the wind flowmap's.
void SSWorldField::renderDebug()
{
    static LLCachedControl<U32> view(gSavedSettings, "SSWorldFieldDebugView", 1);
    const S32 which = llclamp((S32)view, 1, 5);
    if (mTiles.empty()) return;

    static LLCachedControl<F32> range_setting(gSavedSettings, "SSAtmoWindFlowDebugRange", 24.f);
    const F32 full = llclamp((F32)range_setting, 16.f, 4096.f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    auto mark = [&](const LLVector3& p, const LLColor4& c, F32 size)
    {
        gGL.color4fv(c.mV);
        gGL.vertex3f(p.mV[VX] - size, p.mV[VY], p.mV[VZ]);
        gGL.vertex3f(p.mV[VX] + size, p.mV[VY], p.mV[VZ]);
        gGL.vertex3f(p.mV[VX], p.mV[VY] - size, p.mV[VZ]);
        gGL.vertex3f(p.mV[VX], p.mV[VY] + size, p.mV[VZ]);
    };

    auto strideFor = [&](F32 wx, F32 wy) -> S32
    {
        const F32 away = llmax(fabsf(wx - cam.mV[VX]), fabsf(wy - cam.mV[VY]));
        return (away < full) ? 1 : (away < full * 2.f) ? 2 : 4;
    };

    gGL.begin(LLRender::LINES);

    for (const auto& entry : mTiles)
    {
        const Tile& tile = entry.second;
        if (!tile.mValid) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const F32 cell = tile.mCell;
        const F32 h = tile.mBandHeight;

        // The tile's stack footprint, so a region nobody has captured yet reads
        // as an empty box rather than as nothing at all.
        {
            const F32 x1 = origin.mV[VX] + (F32)tile.mRes * cell;
            const F32 y1 = origin.mV[VY] + (F32)tile.mRes * cell;
            const F32 z0 = 0.f;
            const F32 z1 = (F32)llmax(tile.mBandCount, 1) * h;
            gGL.color4f(0.5f, 0.55f, 0.7f, 0.35f);
            const F32 bx[4] = { origin.mV[VX], x1, x1, origin.mV[VX] };
            const F32 by[4] = { origin.mV[VY], origin.mV[VY], y1, y1 };
            for (S32 i = 0; i < 4; ++i)
            {
                const S32 j = (i + 1) % 4;
                gGL.vertex3f(bx[i], by[i], z0); gGL.vertex3f(bx[j], by[j], z0);
                gGL.vertex3f(bx[i], by[i], z1); gGL.vertex3f(bx[j], by[j], z1);
                gGL.vertex3f(bx[i], by[i], z0); gGL.vertex3f(bx[i], by[i], z1);
            }
        }

        if (which == 1)
        {
// Every band the capture gave a surface, standing at the altitude the store
            // holds it at, hue by band so stacked storeys read separately instead of
            // fusing into one roof.
            const S32 b_last = llmax(tile.mBandCount - 1, 1);
            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    for (S32 b = 0; b < tile.mBandCount; ++b)
                    {
                        const size_t bi = ((size_t)b * tile.mRes + y) * (size_t)tile.mRes + x;
                        const F32 z = tile.mBandTop[bi];
                        if (z <= -FLT_MAX * 0.5f) continue;

                        mark(LLVector3(wx, wy, z),
                             ss_wf_band_hue((F32)b / (F32)b_last, 0.85f), cell * 0.4f);
                    }
                }
            }
        }
        else if (which == 2)
        {
            // The touching classification: outdoors air green, sheltered air
            // amber fading with occlusion depth (how far the opening's reach
            // still carries), interior air red. Solid cells draw nothing -
            // the surfaces view shows those. Current means the labels cover
            // every live band: a no-op re-peel can extend mBandCount without
            // moving the serial, and the flood refills those bands later.
            const bool current = !tile.mAirLabel.empty() && tile.mAirSerial == tile.mGeomSerial
                                 && tile.mAirLabel.size() >= (size_t)tile.mBandCount * tile.mRes * tile.mRes;
            if (!current) continue;

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    for (S32 b = 0; b < tile.mBandCount; ++b)
                    {
                        const size_t bi = ((size_t)b * tile.mRes + y) * (size_t)tile.mRes + x;
                        const U8 lab = tile.mAirLabel[bi];
                        if (lab == AIR_SOLID || lab == AIR_UNKNOWN) continue;

                        const F32 z = ((F32)b + 0.5f) * h;
                        if (lab == AIR_OUTDOORS)
                        {
                            mark(LLVector3(wx, wy, z), LLColor4(0.3f, 1.f, 0.4f, 0.85f), cell * 0.4f);
                        }
                        else if (lab == AIR_SHELTERED)
                        {
                            const U16 d = tile.mAirDepth[bi];
                            const F32 a = llmax(0.9f / (1.f + (F32)d * 0.25f), 0.08f);
                            mark(LLVector3(wx, wy, z), LLColor4(1.f, 0.8f, 0.2f, a), cell * 0.4f);
                        }
                        else
                        {
                            mark(LLVector3(wx, wy, z), LLColor4(1.f, 0.25f, 0.25f, 0.85f), cell * 0.4f);
                        }
                    }
                }
            }
        }
        else if (which == 3)
        {
            // Drainage topology at the tile's own resolution: standing water
            // blue, and an arrow per cell down the filled surface's D8 - the
            // outlet chain a pool's water will actually follow.
            auto& cached = sDrainDebug[tile.mRegionHandle];
            if (cached.mSerial != tile.mGeomSerial || cached.mN != tile.mRes)
            {
                cached.mGrid = SSRainShadowMap::SurfaceGrid();
                cached.mDrain = Drainage();
                cached.mN = tile.mRes;
                cached.mSerial = tile.mGeomSerial;
                if (!buildSurfaceGrid(tile.mRegionHandle, tile.mRes, cached.mGrid)
                    || !buildDrainage(cached.mGrid, cached.mDrain))
                {
                    cached.mSerial = 0;
                    continue;
                }
            }
            const SSRainShadowMap::SurfaceGrid& grid = cached.mGrid;
            const Drainage& drain = cached.mDrain;

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t i = (size_t)y * tile.mRes + x;
                    const U8 f = grid.mFlags[i];
                    if (f == 0 || (f & SSRainShadowMap::SURF_WATER)) continue;

                    const F32 z = grid.mZ[i];
                    if (drain.mPool[i])
                    {
                        mark(LLVector3(wx, wy, z), LLColor4(0.2f, 0.5f, 1.f, 0.9f), cell * 0.45f);
                    }
                    else if (drain.mD8[i] != 4)
                    {
                        const S32 di = drain.mD8[i];
                        const F32 len = cell * 0.7f;
                        const F32 dx = (F32)((di % 3) - 1) * len;
                        const F32 dy = (F32)((di / 3) - 1) * len;
                        gGL.color4f(0.7f, 0.85f, 1.f, 0.55f);
                        gGL.vertex3f(wx, wy, z + 0.4f);
                        gGL.vertex3f(wx + dx, wy + dy, z + 0.4f);
                    }
                }
            }
        }
        else if (which == 5)
        {
            // The column spans themselves: every solid span drawn as two flat
            // rects - its floor at the band's bottom, its ceiling at the
            // surface the capture stored - coloured by the air state standing
            // on it, with a dim line joining ceiling to floor through the
            // span's solid body. A column's stack reads as a ladder of
            // state-coloured plates on one spine; the air gaps between spans
            // stay empty, which is exactly where the flood walks.
            const bool current = !tile.mAirLabel.empty() && tile.mAirSerial == tile.mGeomSerial
                                 && tile.mAirLabel.size() >= (size_t)tile.mBandCount * tile.mRes * tile.mRes;

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const F32 s = cell * 0.4f;
                    auto span_rect = [&](F32 z)
                    {
                        gGL.vertex3f(wx - s, wy - s, z); gGL.vertex3f(wx + s, wy - s, z);
                        gGL.vertex3f(wx + s, wy - s, z); gGL.vertex3f(wx + s, wy + s, z);
                        gGL.vertex3f(wx + s, wy + s, z); gGL.vertex3f(wx - s, wy + s, z);
                        gGL.vertex3f(wx - s, wy + s, z); gGL.vertex3f(wx - s, wy - s, z);
                    };

                    for (S32 b = 0; b < tile.mBandCount; ++b)
                    {
                        const size_t bi = ((size_t)b * tile.mRes + y) * (size_t)tile.mRes + x;
                        const F32 z = tile.mBandTop[bi];
                        if (z <= -FLT_MAX * 0.5f) continue;   // an open band

                        const F32 z0 = (F32)b * h;

                        // The state of the air the span holds up: the nearest
                        // air band-cell at or above it in the column.
                        U8 st = AIR_UNKNOWN;
                        if (current)
                        {
                            for (S32 bb = b + 1; bb < tile.mBandCount; ++bb)
                            {
                                const size_t ai = ((size_t)bb * tile.mRes + y) * (size_t)tile.mRes + x;
                                if (tile.mAirLabel[ai] != AIR_SOLID)
                                {
                                    st = tile.mAirLabel[ai];
                                    break;
                                }
                            }
                        }

                        switch (st)
                        {
                            case AIR_OUTDOORS:  gGL.color4f(0.3f, 1.f, 0.4f, 0.8f); break;
                            case AIR_SHELTERED: gGL.color4f(1.f, 0.8f, 0.2f, 0.8f); break;
                            case AIR_INTERIOR:  gGL.color4f(1.f, 0.25f, 0.25f, 0.85f); break;
                            default:            gGL.color4f(0.55f, 0.6f, 0.7f, 0.5f); break;
                        }

                        span_rect(z);
                        span_rect(z0);

                        // The spine: ceiling to floor, through the solid.
                        gGL.color4f(0.6f, 0.65f, 0.75f, 0.35f);
                        gGL.vertex3f(wx, wy, z);
                        gGL.vertex3f(wx, wy, z0);
                    }
                }
            }
        }
    }

    gGL.end();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    // Drop debug views for regions the field no longer holds.
    for (auto it = sDrainDebug.begin(); it != sDrainDebug.end();)
    {
        it = mTiles.count(it->first) ? std::next(it) : sDrainDebug.erase(it);
    }
}

