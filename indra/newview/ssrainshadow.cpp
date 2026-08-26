/**
 * @file ssrainshadow.cpp
 * @brief See ssrainshadow.h.
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

#include "ssrainshadow.h"
#include "ssatmomagic.h"

#include "llagent.h"
#include "llfasttimer.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cfloat>

static const F64 CAPTURE_INTERVAL   = 0.25;
static const F64 DIRTY_MIN_INTERVAL = 2.0;
static const F32 BAND_ABOVE         = 80.f;
static const F32 BAND_BELOW         = 160.f;
static const F32 BAND_MARGIN        = 30.f;
static const F32 DIR_EPSILON        = 0.995f;
static const F32 NEIGHBOR_REACH     = 64.f;
static const U32 MAX_TILES          = 4;
static const F32 DEPTH_MISS         = 0.9999f;

static LLTrace::BlockTimerStatHandle FTM_SS_SHADOW("Atmo Magic Shadow Map");
static LLTrace::BlockTimerStatHandle FTM_SS_SHADOW_GRID("Atmo Magic Surface Grid");

// The resolution tiles were actually captured at - lags the setting until the next capture.
U32 SSRainShadowMap::resolution() const
{
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid) return entry.second.mRes;
    }
    return 0;
}

// Drops every tile - full recapture on demand.
void SSRainShadowMap::clearCache()
{
    mTiles.clear();
    mDebugMesh.clear();
}

// Settled geometry changed inside a tile's captured band - the settle queue's entry point.
void SSRainShadowMap::markDirty(const LLVector3& pos_agent, F32 radius)
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end()) return;

    Tile& tile = it->second;
    if (!tile.mValid || tile.mDirty) return;

    const F32 z = pos_agent.mV[VZ];
    if (z + radius < tile.mBandBottom || z - radius > tile.mBandTop + 40.f) return;

    tile.mDirty = true;

    ++tile.mGeomSerial;
}

// The tile covering a region, optionally created.
SSRainShadowMap::Tile* SSRainShadowMap::tileFor(LLViewerRegion* regionp, bool allow_create)
{
    if (!regionp) return nullptr;
    const U64 handle = regionp->getHandle();
    auto it = mTiles.find(handle);
    if (it != mTiles.end()) return &it->second;
    if (!allow_create) return nullptr;

    Tile& tile = mTiles[handle];
    tile.mRegionHandle = handle;
    return &tile;
}

// Whether a tile's capture is stale: dirty, fall-direction drift, or the camera leaving its band.
bool SSRainShadowMap::needsCapture(const Tile& tile) const
{
    if (!tile.mValid) return true;
    const F64 now = SSAtmoMagic::getInstance()->sharedTime();

    static LLCachedControl<F32> max_age(gSavedSettings, "SSAtmoShadowMaxAge", 60.f);
    if (now - tile.mCaptureTime > (F64)llmax(5.f, (F32)max_age)) return true;

    if (tile.mDirty && now - tile.mCaptureTime > DIRTY_MIN_INTERVAL) return true;

    if (tile.mDir * SSAtmoMagic::getInstance()->rainDirection() < DIR_EPSILON) return true;

    const F32 cam_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    if (cam_z > tile.mBandTop - BAND_MARGIN || cam_z < tile.mBandBottom + BAND_MARGIN * 2.f) return true;

    static LLCachedControl<U32> res_setting(gSavedSettings, "SSAtmoShadowRes", 1024);
    if (tile.mRes != (U32)res_setting) return true;

    return false;
}

// Ortho depth render of the region along the fall direction, avatars masked out, reusing the sun-shadow machinery.
bool SSRainShadowMap::captureTile(Tile& tile)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_SHADOW);
    LL_PROFILE_GPU_ZONE("atmo shadow map");

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;

    static LLCachedControl<U32> res_setting(gSavedSettings, "SSAtmoShadowRes", 1024);
    const U32 res = llclamp((U32)res_setting, 128u, 2048u);

    if (mTarget.getWidth() != res)
    {
        mTarget.release();
        if (!mTarget.allocate(res, res, 0, true))
        {
            return false;
        }
    }

    const LLVector3 region_origin = regionp->getOriginAgent();
    const F32 width = regionp->getWidth();
    const LLVector3 dir = SSAtmoMagic::getInstance()->rainDirection();

    LLVector3 right = dir % LLVector3(0.f, 1.f, 0.f);
    right.normVec();
    LLVector3 up = right % dir;
    up.normVec();

    const F32 cam_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    const F32 band_top = cam_z + BAND_ABOVE;
    const F32 band_bottom = cam_z - BAND_BELOW;

    const F32 inv_z = 1.f / llmax(0.2f, fabsf(dir.mV[VZ]));
    const F32 tilt = sqrtf(llmax(0.f, 1.f - dir.mV[VZ] * dir.mV[VZ]));
    const F32 half = width * 0.5f + (band_top - band_bottom) * tilt * inv_z * 0.5f + 8.f;

    const F32 lead = half * F_SQRT2 * tilt + 16.f;

    const F32 range = lead + (band_top - band_bottom) * inv_z + half * F_SQRT2 * tilt + 40.f;

    const LLVector3 eye = LLVector3(region_origin.mV[VX] + width * 0.5f,
                                    region_origin.mV[VY] + width * 0.5f,
                                    band_top) - dir * lead;

    const glm::mat4 saved_view = get_current_modelview();
    const glm::mat4 saved_proj = get_current_projection();
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;

    const glm::mat4 view = glm::lookAt(glm::vec3(eye.mV[VX], eye.mV[VY], eye.mV[VZ]),
                                       glm::vec3(eye.mV[VX] + dir.mV[VX], eye.mV[VY] + dir.mV[VY], eye.mV[VZ] + dir.mV[VZ]),
                                       glm::vec3(up.mV[VX], up.mV[VY], up.mV[VZ]));
    const glm::mat4 proj = glm::ortho(-half, half, -half, half, 0.f, range);

    set_current_modelview(view);
    set_current_projection(proj);
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_SUN_SHADOW3;

    LLCamera shadow_cam = *LLViewerCamera::getInstance();
    shadow_cam.setOrigin(eye);
    shadow_cam.setFar(range);

    LLVector3 frust[8];
    frust[0] = eye - right * half - up * half;
    frust[1] = eye + right * half - up * half;
    frust[2] = eye + right * half + up * half;
    frust[3] = eye - right * half + up * half;
    for (U32 i = 0; i < 4; i++)
    {
        frust[i + 4] = frust[i] + dir * range;
    }
    shadow_cam.calcAgentFrustumPlanes(frust);
    shadow_cam.mFrustumCornerDist = 0.f;

    mTarget.bindTarget();
    mTarget.getViewport(gGLViewport);
    mTarget.clear();

    {
        static LLCullResult cull_result;

        gPipeline.pushRenderTypeMask();
        gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_AVATAR,
                                      LLPipeline::RENDER_TYPE_CONTROL_AV,
                                      LLPipeline::END_RENDER_TYPES);
        gPipeline.renderShadow(view, proj, shadow_cam, cull_result, true);
        gPipeline.popRenderTypeMask();
    }

    tile.mRes = res;
    tile.mDepth.resize((size_t)res * res);
    glReadPixels(0, 0, res, res, GL_DEPTH_COMPONENT, GL_FLOAT, tile.mDepth.data());

    mTarget.flush();

    set_current_modelview(saved_view);
    set_current_projection(saved_proj);
    LLViewerCamera::sCurCameraID = saved_camera;

    tile.mEyeRegion = eye - region_origin;
    tile.mDir = dir;
    tile.mRight = right;
    tile.mUp = up;
    tile.mHalfW = half;
    tile.mHalfH = half;
    tile.mNear = 0.f;
    tile.mFar = range;
    tile.mBandTop = band_top;
    tile.mBandBottom = band_bottom;
    tile.mCaptureTime = SSAtmoMagic::getInstance()->sharedTime();
    tile.mDirty = false;
    tile.mValid = true;

    tile.mCapturedSerial = tile.mGeomSerial;

    return true;
}

// Drops tiles for departed regions, then the least recently used beyond the cache cap.
void SSRainShadowMap::evict()
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
    while (mTiles.size() > MAX_TILES)
    {
        auto oldest = mTiles.begin();
        for (auto it = mTiles.begin(); it != mTiles.end(); ++it)
        {
            if (it->second.mLastTouched < oldest->second.mLastTouched) oldest = it;
        }
        mTiles.erase(oldest);
    }
}

// Per-frame budget: at most one, most deserving, tile capture per interval.
void SSRainShadowMap::capture()
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!atmo->hasWeather())
    {
        if (!mTiles.empty()) clearCache();
        if (mTarget.getWidth() > 0) mTarget.release();
        return;
    }

    const F64 now = atmo->sharedTime();
    if (now - mLastCapture < CAPTURE_INTERVAL) return;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);
    Tile* best = nullptr;
    if (cam_region)
    {
        Tile* tile = tileFor(cam_region, true);
        tile->mLastTouched = now;
        if (needsCapture(*tile)) best = tile;
    }

    if (!best)
    {
        for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
        {
            if (regionp == cam_region) continue;
            const LLVector3 origin = regionp->getOriginAgent();
            const F32 width = regionp->getWidth();
            const F32 dx = llmax(origin.mV[VX] - cam.mV[VX], cam.mV[VX] - (origin.mV[VX] + width), 0.f);
            const F32 dy = llmax(origin.mV[VY] - cam.mV[VY], cam.mV[VY] - (origin.mV[VY] + width), 0.f);
            if (dx * dx + dy * dy > NEIGHBOR_REACH * NEIGHBOR_REACH) continue;

            Tile* tile = tileFor(regionp, true);
            tile->mLastTouched = now;
            if (needsCapture(*tile))
            {
                best = tile;
                break;
            }
        }
    }

    if (best)
    {
        mLastCapture = now;

        const bool was_dirty = best->mDirty;

        LLTimer timer;
        captureTile(*best);

        mLastCaptureMS = (F32)(timer.getElapsedTimeF64() * 1000.0);
        ++mCaptureCount;
        if (was_dirty) ++mDirtyCaptures;
    }

    evict();
}

// How many tiles await recapture, for status UI.
U32 SSRainShadowMap::dirtyTileCount() const
{
    U32 n = 0;
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid && entry.second.mDirty) ++n;
    }
    return n;
}

// Seconds since anything was captured, for status UI.
F64 SSRainShadowMap::lastCaptureAge() const
{
    if (mLastCapture <= 0.0) return 0.0;
    return SSAtmoMagic::getInstance()->sharedTime() - mLastCapture;
}

static const F32 DEBUG_DIR_EPSILON = 0.9995f;

// Bakes the debug mesh: one column per sample, the vertex sitting on whatever it lands on, seeds stepped upwind so tilted falls stay on-region.
void SSRainShadowMap::buildShadowMesh(const Tile& tile, ShadowMesh& mesh)
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp)
    {
        mesh.mN = 0;
        return;
    }

    static LLCachedControl<F32> step_setting(gSavedSettings, "SSAtmoShadowDebugStep", 2.f);
    const F32 step = llclamp((F32)step_setting, 0.5f, 16.f);

    const F32 width = regionp->getWidth();

    const S32 n = llclamp((S32)(width / step) + 1, 2, 1025);
    const F32 grid_step = width / (F32)(n - 1);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();
    const LLVector3 region_origin = regionp->getOriginAgent();

    mesh.mN = n;
    mesh.mPos.resize((size_t)n * n);
    mesh.mShade.resize((size_t)n * n);

    for (S32 j = 0; j < n; ++j)
    {
        for (S32 i = 0; i < n; ++i)
        {
            const F32 lx = llmin((F32)i * grid_step, width);
            const F32 ly = llmin((F32)j * grid_step, width);

            const LLVector3 start(region_origin.mV[VX] + lx,
                                  region_origin.mV[VY] + ly,
                                  tile.mBandTop);

            LLVector3 hit;
            bool on_water = false;
            resolveColumn(start, hit, on_water);

            const LLVector3 corrected(2.f * start.mV[VX] - hit.mV[VX],
                                      2.f * start.mV[VY] - hit.mV[VY],
                                      tile.mBandTop);
            const bool mapped = resolveColumn(corrected, hit, on_water);

            const size_t idx = (size_t)j * n + i;

            mesh.mPos[idx].set(hit.mV[VX] - region_origin.mV[VX],
                               hit.mV[VY] - region_origin.mV[VY],
                               hit.mV[VZ] + 0.12f);

            mesh.mShade[idx] = mapped ? 1.f : 0.f;
        }
    }

    mesh.mBuiltFrom  = tile.mCaptureTime;
    mesh.mBuiltDir   = atmo->rainDirection();
    mesh.mBuiltStep  = step;
    mesh.mBuiltFloor = sky ? sky_floor : 0.f;
    mesh.mBuiltSky   = sky;
}

// Casts the map back onto the world - cool where depth was captured, warm where a column fell back to the heightmap.
void SSRainShadowMap::renderDebug()
{
    if (mTiles.empty())
    {
        mDebugMesh.clear();
        return;
    }

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const LLVector3 dir = atmo->rainDirection();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();

    static LLCachedControl<F32> step_setting(gSavedSettings, "SSAtmoShadowDebugStep", 2.f);
    const F32 step = llclamp((F32)step_setting, 0.5f, 16.f);

    std::vector<U64> handles;
    handles.reserve(mTiles.size());
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid && !entry.second.mDepth.empty())
        {
            handles.push_back(entry.first);
        }
    }

    for (auto it = mDebugMesh.begin(); it != mDebugMesh.end(); )
    {
        it = (mTiles.count(it->first) == 0) ? mDebugMesh.erase(it) : std::next(it);
    }

    for (U64 handle : handles)
    {
        const Tile& tile = mTiles[handle];
        ShadowMesh& mesh = mDebugMesh[handle];

        const bool stale = mesh.mN == 0
                        || mesh.mBuiltFrom != tile.mCaptureTime
                        || fabsf(mesh.mBuiltStep - step) > 0.01f
                        || mesh.mBuiltSky != sky
                        || (sky && fabsf(mesh.mBuiltFloor - sky_floor) > 0.5f)
                        || mesh.mBuiltDir * dir < DEBUG_DIR_EPSILON;

        if (stale)
        {
            buildShadowMesh(tile, mesh);
        }
    }

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    for (U64 handle : handles)
    {
        const ShadowMesh& mesh = mDebugMesh[handle];
        if (mesh.mN < 2) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(handle);
        if (!regionp) continue;

        const LLVector3 base = regionp->getOriginAgent();
        const S32 n = mesh.mN;

        auto vert = [&](S32 i, S32 j)
        {
            const size_t idx = (size_t)j * n + i;
            const F32 s = mesh.mShade[idx];

            gGL.color4f(lerp(0.85f, 0.04f, s),
                        lerp(0.40f, 0.07f, s),
                        lerp(0.10f, 0.16f, s),
                        lerp(0.60f, 0.30f, s));

            const LLVector3& p = mesh.mPos[idx];
            gGL.vertex3f(base.mV[VX] + p.mV[VX], base.mV[VY] + p.mV[VY], p.mV[VZ]);
        };

        gGL.begin(LLRender::TRIANGLES);
        for (S32 j = 0; j + 1 < n; ++j)
        {
            for (S32 i = 0; i + 1 < n; ++i)
            {
                const F32 z00 = mesh.mPos[(size_t)j * n + i].mV[VZ];
                const F32 z10 = mesh.mPos[(size_t)j * n + i + 1].mV[VZ];
                const F32 z01 = mesh.mPos[(size_t)(j + 1) * n + i].mV[VZ];
                const F32 z11 = mesh.mPos[(size_t)(j + 1) * n + i + 1].mV[VZ];
                const F32 spread = llmax(llmax(z00, z10), llmax(z01, z11))
                                 - llmin(llmin(z00, z10), llmin(z01, z11));
                if (spread > llmax(4.f, step * 4.f)) continue;

                vert(i, j);     vert(i + 1, j);     vert(i + 1, j + 1);
                vert(i, j);     vert(i + 1, j + 1); vert(i, j + 1);
            }
        }
        gGL.end();
    }

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 land = sky ? sky_floor : LLWorld::getInstance()->resolveLandHeightAgent(cam);
    const LLVector3 marker(cam.mV[VX], cam.mV[VY], land + 0.2f);

    gGL.begin(LLRender::LINES);
    gGL.color4f(0.4f, 0.7f, 1.f, 0.7f);
    gGL.vertex3fv(marker.mV);
    gGL.vertex3fv((marker - dir * 20.f).mV);
    gGL.end();

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

// Handles and geometry serials of usable tiles, for consumers deciding whether to retrace.
void SSRainShadowMap::validTiles(std::vector<std::pair<U64, U32> >& out) const
{
    out.clear();
    out.reserve(mTiles.size());
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid && !entry.second.mDepth.empty())
        {
            out.emplace_back(entry.first, entry.second.mCapturedSerial);
        }
    }
}

// Resolves a full n x n landing-surface grid for a region - the drainage trace's input.
bool SSRainShadowMap::buildSurfaceGrid(U64 region_handle, S32 n, SurfaceGrid& out)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_SHADOW_GRID);

    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid || it->second.mDepth.empty()) return false;

    const Tile& tile = it->second;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return false;

    n = llclamp(n, 16, 512);

    const LLVector3 origin = regionp->getOriginAgent();
    const F32 width = regionp->getWidth();

    out.mRegionHandle = region_handle;
    out.mN = n;
    out.mCell = width / (F32)n;
    out.mGeomSerial = tile.mCapturedSerial;
    out.mZ.assign((size_t)n * n, -FLT_MAX);
    out.mFlags.assign((size_t)n * n, 0);

    const LLVector3 eye = origin + tile.mEyeRegion;
    const F32 range = tile.mFar - tile.mNear;
    const F32 water_z = regionp->getWaterHeight();

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();

    const U32 res = tile.mRes;
    const F32 su = 2.f * tile.mHalfW / (F32)res;
    const F32 sv = 2.f * tile.mHalfH / (F32)res;
    const LLVector3 right_step = tile.mRight * su;
    const LLVector3 up_step = tile.mUp * sv;
    const LLVector3 dir_range = tile.mDir * range;
    const LLVector3 row_start = eye
        + tile.mRight * (-tile.mHalfW + 0.5f * su)
        + tile.mUp * (-tile.mHalfH + 0.5f * sv)
        + tile.mDir * tile.mNear;

    const F32 inv_cell = 1.f / out.mCell;
    F32* zbuf = out.mZ.data();

    for (U32 ty = 0; ty < res; ++ty)
    {
        const F32* row = &tile.mDepth[(size_t)ty * res];
        LLVector3 p = row_start + up_step * (F32)ty;

        for (U32 tx = 0; tx < res; ++tx, p += right_step)
        {
            const F32 d = row[tx];
            if (d >= DEPTH_MISS) continue;

            const F32 hx = p.mV[VX] + dir_range.mV[VX] * d;
            const F32 hy = p.mV[VY] + dir_range.mV[VY] * d;
            const F32 hz = p.mV[VZ] + dir_range.mV[VZ] * d;

            const F32 lx = hx - origin.mV[VX];
            const F32 ly = hy - origin.mV[VY];
            if (lx < 0.f || ly < 0.f) continue;

            const S32 gx = (S32)(lx * inv_cell);
            const S32 gy = (S32)(ly * inv_cell);
            if (gx >= n || gy >= n) continue;

            F32& slot = zbuf[(size_t)gy * n + gx];
            if (hz > slot) slot = hz;
        }
    }

    LLWorld* worldp = LLWorld::getInstance();
    for (S32 gy = 0; gy < n; ++gy)
    {
        for (S32 gx = 0; gx < n; ++gx)
        {
            const size_t idx = (size_t)gy * n + gx;
            F32 z = out.mZ[idx];

            if (z > -FLT_MAX)
            {
                if (!sky && z < water_z)
                {
                    out.mZ[idx] = water_z;
                    out.mFlags[idx] = SURF_MAPPED | SURF_WATER;
                }
                else
                {
                    out.mFlags[idx] = SURF_MAPPED;
                }
                continue;
            }

            if (sky)
            {
                out.mZ[idx] = sky_floor;
                out.mFlags[idx] = 0;
                continue;
            }

            const LLVector3 centre(origin.mV[VX] + out.axis(gx),
                                   origin.mV[VY] + out.axis(gy),
                                   water_z);
            const F32 land = worldp->resolveLandHeightAgent(centre);
            out.mZ[idx] = llmax(land, water_z);
            out.mFlags[idx] = SURF_FALLBACK | ((water_z > land) ? SURF_WATER : 0);
        }
    }

    return true;
}

// Bisects between two samples to localise a shelter edge precisely.
bool SSRainShadowMap::refineEdge(U64 region_handle, const LLVector3& from_agent,
                                 const LLVector3& out_dir, F32 max_dist, F32 tolerance,
                                 LLVector3& refined_agent) const
{
    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid || it->second.mDepth.empty()) return false;

    const Tile& tile = it->second;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return false;

    const LLVector3 eye = regionp->getOriginAgent() + tile.mEyeRegion;
    const F32 range = tile.mFar - tile.mNear;
    const F32 texel = 2.f * tile.mHalfW / (F32)tile.mRes;

    auto sample = [&](const LLVector3& probe, LLVector3& hit) -> bool
    {
        const LLVector3 rel = probe - eye;
        const F32 u = (rel * tile.mRight) / (2.f * tile.mHalfW) + 0.5f;
        const F32 v = (rel * tile.mUp) / (2.f * tile.mHalfH) + 0.5f;
        if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f) return false;

        const U32 tx = llmin((U32)(u * tile.mRes), tile.mRes - 1);
        const U32 ty = llmin((U32)(v * tile.mRes), tile.mRes - 1);
        const F32 depth = tile.mDepth[(size_t)ty * tile.mRes + tx];
        if (depth >= DEPTH_MISS) return false;

        hit = probe + tile.mDir * ((tile.mNear + depth * range) - (rel * tile.mDir));
        return true;
    };

    refined_agent = from_agent;

    const S32 steps = llclamp((S32)(max_dist / llmax(0.01f, texel)), 1, 64);
    for (S32 i = 1; i <= steps; ++i)
    {
        LLVector3 hit;
        if (!sample(from_agent + out_dir * (texel * (F32)i), hit)) break;
        if (from_agent.mV[VZ] - hit.mV[VZ] > tolerance) break;
        refined_agent = hit;
    }

    return true;
}

// THE landing lookup: where a falling column ends up (depth capture first, heightmap fallback), what it hit, and whether it is sheltered.
bool SSRainShadowMap::resolveColumn(const LLVector3& pos_agent, LLVector3& hit_pos_agent, bool& on_water,
                                    LLVector3* hit_normal)
{
    const LLVector3 dir = SSAtmoMagic::getInstance()->rainDirection();
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);

    bool from_map = false;
    LLVector3 hit;
    if (hit_normal)
    {
        hit_normal->set(0.f, 0.f, 1.f);
    }

    Tile* tile = regionp ? tileFor(regionp, false) : nullptr;
    if (tile && tile->mValid)
    {
        const LLVector3 eye = regionp->getOriginAgent() + tile->mEyeRegion;
        const LLVector3 rel = pos_agent - eye;
        const F32 u = (rel * tile->mRight) / (2.f * tile->mHalfW) + 0.5f;
        const F32 v = (rel * tile->mUp) / (2.f * tile->mHalfH) + 0.5f;
        if (u >= 0.f && u < 1.f && v >= 0.f && v < 1.f)
        {
            const U32 tx = llmin((U32)(u * tile->mRes), tile->mRes - 1);
            const U32 ty = llmin((U32)(v * tile->mRes), tile->mRes - 1);
            const F32 depth = tile->mDepth[(size_t)ty * tile->mRes + tx];
            if (depth < DEPTH_MISS)
            {
                const F32 range = tile->mFar - tile->mNear;
                const F32 texel_u = 2.f * tile->mHalfW / (F32)tile->mRes;
                const F32 texel_v = 2.f * tile->mHalfH / (F32)tile->mRes;

                const F32 max_step = (texel_u * 4.f) / llmax(range, 0.01f);

                auto tap = [&](S32 ix, S32 iy) -> F32
                {
                    ix = llclamp(ix, 0, (S32)tile->mRes - 1);
                    iy = llclamp(iy, 0, (S32)tile->mRes - 1);
                    const F32 d = tile->mDepth[(size_t)iy * tile->mRes + ix];
                    if (d >= DEPTH_MISS) return depth;
                    return llclamp(d, depth - max_step, depth + max_step);
                };

                const F32 fx = u * (F32)tile->mRes - 0.5f;
                const F32 fy = v * (F32)tile->mRes - 0.5f;
                const S32 x0 = (S32)floorf(fx);
                const S32 y0 = (S32)floorf(fy);
                const F32 wx = fx - (F32)x0;
                const F32 wy = fy - (F32)y0;

                const F32 d00 = tap(x0,     y0);
                const F32 d10 = tap(x0 + 1, y0);
                const F32 d01 = tap(x0,     y0 + 1);
                const F32 d11 = tap(x0 + 1, y0 + 1);
                const F32 d_lerped = lerp(lerp(d00, d10, wx), lerp(d01, d11, wx), wy);

                const F32 hit_d = tile->mNear + d_lerped * range;
                const F32 pos_d = rel * tile->mDir;
                hit = pos_agent + dir * (hit_d - pos_d);
                from_map = true;

                if (hit_normal)
                {
                    const F32 max_dd = texel_u * 4.f;
                    const F32 dd_u = llclamp((tap((S32)tx + 1, (S32)ty) - tap((S32)tx - 1, (S32)ty)) * range * 0.5f,
                                             -max_dd, max_dd);
                    const F32 dd_v = llclamp((tap((S32)tx, (S32)ty + 1) - tap((S32)tx, (S32)ty - 1)) * range * 0.5f,
                                             -max_dd, max_dd);

                    LLVector3 t_u = tile->mRight * texel_u + tile->mDir * dd_u;
                    LLVector3 t_v = tile->mUp * texel_v + tile->mDir * dd_v;
                    LLVector3 normal = t_u % t_v;
                    normal.normVec();
                    if (normal * tile->mDir > 0.f)
                    {
                        normal = -normal;
                    }
                    *hit_normal = normal;
                }
            }
        }
    }

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool sky = atmo->isSkyTrack();

    F32 floor_z;
    bool floor_is_water = false;
    if (sky)
    {
        floor_z = atmo->groundZero();
    }
    else if (from_map)
    {
        floor_z = regionp ? regionp->getWaterHeight() : SSAtmoMagic::voidWaterHeight();
        floor_is_water = true;
    }
    else
    {
        const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(pos_agent);
        const F32 water = regionp ? regionp->getWaterHeight() : SSAtmoMagic::voidWaterHeight();
        floor_is_water = water > land;
        floor_z = llmax(land, water);
    }

    on_water = false;

    if (!from_map || (!sky && hit.mV[VZ] < floor_z))
    {
        const F32 dz = dir.mV[VZ];
        if (fabsf(dz) < 0.01f)
        {
            hit = pos_agent;
            hit.mV[VZ] = floor_z;
        }
        else
        {
            hit = pos_agent + dir * ((floor_z - pos_agent.mV[VZ]) / dz);
        }
        on_water = floor_is_water;

        if (hit_normal)
        {
            if (sky || on_water)
            {
                hit_normal->set(0.f, 0.f, 1.f);
            }
            else
            {
                *hit_normal = LLWorld::getInstance()->resolveLandNormalGlobal(gAgent.getPosGlobalFromAgent(hit));
            }
        }
    }

    hit_pos_agent = hit;

    return from_map;
}
