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

#include "llfasttimer.h"
#include "llrender.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cfloat>
#include <cmath>

static const F32 NEIGHBOR_REACH   = 64.f;
static const F32 DEPTH_MISS       = 0.9999f;
static const F32 BOUNDARY_EPSILON = 0.05f;
static const F32 NO_SURFACE       = -FLT_MAX;

static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD("Atmo Magic World Field");
static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD_GRID("Atmo Magic World Field Grid");

// Channel interest refcounts. File statics so an Interest handle's deleter
// stays valid for the life of the process regardless of singleton teardown
// order - a destroyed handle must always be able to release its count.
static std::map<std::pair<U64, S32>, S32> sInterests;

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

// The wet field's source switch - while it is on, SURFACE_TOP counts as
// claimed for the camera region and its neighbours, the same reach the rain
// shadow capture serves today.
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

    // Bands the edit's own altitude could touch. A removed roof drops its
    // whole column, so a rect re-peel sweeps from the band floor up to just
    // past the edit rather than only the bands the edit's box overlaps.
    const S32 band = llclamp((S32)((pos_agent.mV[VZ] + r) / tile.mBandHeight), 0, SSWorldField::MAX_BANDS - 1);
    tile.mBandTarget = llmax(tile.mBandTarget, band + 1);
}

void SSWorldField::clear()
{
    mTiles.clear();
    mBuild.mActive = false;
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

    static LLCachedControl<F32> cell_setting(gSavedSettings, "SSWorldFieldCell", 2.f);
    const F32 cell = llclamp((F32)cell_setting, 0.5f, 8.f);
    const F32 width = regionp->getWidth();
    const S32 res = llclamp((S32)llround(width / cell), 32, 256);

    Tile& tile = mTiles[regionp->getHandle()];
    tile.mRegionHandle = regionp->getHandle();
    tile.mRes = res;
    tile.mCell = width / (F32)res;
    tile.mBandHeight = bandHeight();
    tile.mBandTop.assign((size_t)MAX_BANDS * res * res, NO_SURFACE);
    tile.mBandFlags.assign((size_t)MAX_BANDS * res * res, 0);
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

    static LLCachedControl<F32> cell_setting(gSavedSettings, "SSWorldFieldCell", 2.f);
    const F32 cell = llclamp((F32)cell_setting, 0.5f, 8.f);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;
    const S32 res = llclamp((S32)llround(regionp->getWidth() / cell), 32, 256);
    if (res != tile.mRes) return true;

    if (fabsf(tile.mBandHeight - bandHeight()) > 0.01f) return true;

    return false;
}

// Most deserving tile first: the camera's region, then any region within
// neighbour reach that either has a claimed channel or is serving the
// demanded surface top. Nothing builds at all while nothing demands anything.
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

    // Begin a build. A dirty tile re-peels only its dirty rectangle's
    // frustum, but still sweeps every band from the floor up to the highest
    // band the edits could touch - a removed roof drops its whole column, so
    // band scoping below the edit is not safe.
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

        // Rect capture resources: a square frustum covering the rect's wider
        // axis, with the short side's extra texels spilling outside the rect
        // and being skipped at splice time.
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
// cache cap.
void SSWorldField::evict()
{
    for (auto it = mTiles.begin(); it != mTiles.end();)
    {
        if (!LLWorld::getInstance()->getRegionFromHandle(it->first))
        {
            it = mTiles.erase(it);
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
        mTiles.erase(oldest);
    }
}

// One band step: capture, splice, then either move to the next band, stop
// early on empty sky, or commit.
bool SSWorldField::advanceBuild()
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(mBuild.mRegionHandle);
    Tile* tile = regionp ? tileFor(regionp, false) : nullptr;
    if (!tile)
    {
        mBuild.mActive = false;
        return false;
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

    applyBand(*tile);
    ++mBuild.mBand;

    // Full builds stop early once the sky has been genuinely empty for a few
    // consecutive bands; rect builds run to their target so the spliced
    // columns stay consistent with the neighbours around them.
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

        mBuild.mDepth.resize((size_t)res * res);
        glReadPixels(0, 0, res, res, GL_DEPTH_COMPONENT, GL_FLOAT, mBuild.mDepth.data());

        mTarget.flush();
    }

    set_current_modelview(saved_view);
    set_current_projection(saved_proj);
    LLViewerCamera::sCurCameraID = saved_camera;

    return ok;
}

// Splices the captured band into the tile: per column, the highest surface
// inside the band, projected out of the depth readback. Full builds write
// every column; rect builds write only the dirty rectangle's columns.
void SSWorldField::applyBand(Tile& tile)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD_GRID);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp || mBuild.mDepth.empty()) return;

    const S32 band = mBuild.mBand;
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
}

// Resolves the landing-surface grid for a region - SSRainShadowMap's exact
// contract, sourced from the band stack instead of a private capture. The
// first thing a falling drop meets is the highest surface in the column, so
// the bands are scanned top-down and the first hit wins.
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

