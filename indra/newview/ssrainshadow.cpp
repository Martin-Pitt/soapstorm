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
#include "ssglreadback.h"

#include "llagent.h"
#include "llfasttimer.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llsurface.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "pipeline.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cfloat>
#include <cstring>

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

// Drops every tile - full recapture on demand. A clear requested while a
// readback is in flight is deferred to the read's completion, when the shared
// target is once more untouched.
void SSRainShadowMap::clearCache()
{
    if (mReadbackPending) { mClearPending = true; return; }
    mTiles.clear();
    mDebugCloud.clear();
    mDebugGrid.clear();
    mDebugMapFrom = -1.0;
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

    // <SS:Nexii> The depth readback rides the shared SSGLReadback worker: the
    // synchronous glReadPixels that used to stall the frame loop here is now a
    // glGetTexImage on a dedicated GL thread. mValid stays false until the
    // texels land back on the main thread, so every consumer (surface grid,
    // resolveColumn, the debug mesh) keeps reading only complete tiles, and
    // capture() won't re-render the shared target while the read is in flight.
    tile.mValid = false;
    tile.mDepth.assign((size_t)res * res, 0.f);

    tile.mCapturedSerial = tile.mGeomSerial;

    mReadbackPending = true;
    mReadbackRegion = tile.mRegionHandle;
    const U64 region = tile.mRegionHandle;
    const U32 tres = res;

    SSGLReadback::Job job;
    job.mTexture = mTarget.getDepth();
    job.mTarget = GL_TEXTURE_2D;
    job.mWidth = tres;
    job.mHeight = tres;
    job.mFormat = GL_DEPTH_COMPONENT;
    job.mType = GL_FLOAT;
    job.mDone = [this, region, tres](const U8* data, size_t bytes)
    {
        mReadbackPending = false;
        mReadbackRegion = 0;
        if (mClearPending)
        {
            mClearPending = false;
            clearCache();
            return;
        }
        auto it = mTiles.find(region);
        if (it == mTiles.end()) return;
        Tile& tile = it->second;
        const size_t n = (size_t)tres * tres;
        if (bytes >= n * sizeof(F32) && tile.mDepth.size() >= n)
        {
            memcpy(tile.mDepth.data(), data, n * sizeof(F32));
            tile.mValid = true;
        }
    };
    if (!SSGLReadback::getInstance()->submit(job))
    {
        // Could not even stage the read - GL trouble. Leave the tile invalid
        // (and the target free) so the next capture retries it.
        mReadbackPending = false;
        mReadbackRegion = 0;
        return false;
    }

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
        // Never drop the tile an in-flight readback is copying into.
        if (oldest->second.mRegionHandle == mReadbackRegion) break;
        mTiles.erase(oldest);
    }
}

// Per-frame budget: at most one, most deserving, tile capture per interval.
void SSRainShadowMap::capture()
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!atmo->hasWeather())
    {
        // A read in flight still references mTarget's depth texture - let it
        // land first, then tear the target down next frame.
        if (mReadbackPending)
        {
            evict();
            return;
        }
        if (!mTiles.empty()) clearCache();
        if (mTarget.getWidth() > 0) mTarget.release();
        if (mDebugMapTex)
        {
            LLImageGL::deleteTextures(1, &mDebugMapTex);
            mDebugMapTex = 0;
            mDebugMapFrom = -1.0;
        }
        return;
    }

    // The shared capture target must not be rendered into again until the
    // outstanding readback has copied its contents off; capture() waits for it
    // rather than racing the worker.
    if (mReadbackPending)
    {
        evict();
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

// Debug view: the drawn footprint of one texel shrinks a touch so neighbours read as separate cells rather than a sealed sheet.
static const F32 DEBUG_QUAD_FILL   = 0.92f;
// Above this many world metres of hit drift across one texel the surface is near-parallel to the fall - the map is smearing a vertical face across the column, and every shelter answer taken from it is a guess.
static const F32 DEBUG_GRAZE_RATIO = 3.f;
// Height above the ground at which a hit is unambiguously sheltering geometry rather than the terrain the map was aimed at.
static const F32 DEBUG_SHELTER_TOP = 12.f;
// Lift along the fall direction, so a quad floats just clear of the surface it saw instead of z-fighting it. Along -dir rather than +Z because that is the axis the map looked down, so the quad is always in front of its own texel.
static const F32 DEBUG_LIFT        = 0.08f;
// How far from the camera the world views draw. Fixed rather than a dial: the cloud is a point per texel, so the honest range is whatever the frame can carry at full map resolution, and that does not vary by taste.
static const F32 DEBUG_RANGE       = 96.f;
// Sample spacing in texels. Below 1 there are no more texels to show, so the cloud interpolates between them the way resolveColumn does when it reads the map - a smoother surface, not more information.
static const F32 DEBUG_STRIDE_MIN  = 0.25f;
static const F32 DEBUG_STRIDE_MAX  = 32.f;
// Hard ceiling on baked points per tile - the stride is doubled until the cloud fits, so a fine stride on a large map degrades to a coarser one instead of eating a gigabyte.
static const size_t DEBUG_MAX_POINTS = 1500000;

// The stride actually used: the requested one, doubled until a map of this resolution bakes within the point ceiling. A sub-texel stride therefore only survives on maps coarse enough to have room for it, which is where it was wanted anyway.
static F32 fittedStride(U32 res, F32 stride)
{
    stride = llclamp(stride, DEBUG_STRIDE_MIN, DEBUG_STRIDE_MAX);
    while (stride < DEBUG_STRIDE_MAX)
    {
        const size_t steps = (size_t)llmax(1.f, floorf((F32)res / stride));
        if (steps * steps <= DEBUG_MAX_POINTS) break;
        stride *= 2.f;
    }
    return stride;
}

// Debug colour ramp: olive on the ground, through green and cyan to blue as a hit climbs clear of it, violet for the tall stuff. Magenta means the texel grazed a near-vertical face, so its shelter answer is not to be trusted.
static LLColor4U shadowTexelColor(F32 above_ground, bool graze, bool off_region, bool submerged)
{
    F32 r, g, b;

    const F32 t = llclamp(above_ground / DEBUG_SHELTER_TOP, 0.f, 1.f);
    if (above_ground < 0.35f)
    {
        r = 0.78f; g = 0.62f; b = 0.18f;
    }
    else if (t < 0.5f)
    {
        const F32 k = t * 2.f;
        r = lerp(0.45f, 0.10f, k); g = lerp(0.80f, 0.90f, k); b = lerp(0.25f, 0.80f, k);
    }
    else
    {
        const F32 k = (t - 0.5f) * 2.f;
        r = lerp(0.10f, 0.62f, k); g = lerp(0.90f, 0.30f, k); b = lerp(0.80f, 1.00f, k);
    }

    F32 a = (above_ground < 0.35f) ? 0.45f : lerp(0.55f, 0.85f, t);

    if (submerged)
    {
        r *= 0.35f; g = lerp(g, 0.65f, 0.5f); b = lerp(b, 0.85f, 0.6f); a *= 0.7f;
    }

    if (graze)
    {
        r = lerp(r, 1.f, 0.75f); g = lerp(g, 0.15f, 0.75f); b = lerp(b, 0.95f, 0.75f); a *= 0.8f;
    }

    // The capture frustum is wider than the region so a tilted fall still covers it - those texels landed on a neighbour or the void, and are dimmed rather than dropped so the overscan itself stays legible.
    if (off_region) a *= 0.3f;

    return LLColor4U((U8)(llclamp(r, 0.f, 1.f) * 255.f), (U8)(llclamp(g, 0.f, 1.f) * 255.f),
                     (U8)(llclamp(b, 0.f, 1.f) * 255.f), (U8)(llclamp(a, 0.f, 1.f) * 255.f));
}

// Bakes a tile's depth texels into world points. This is a straight unprojection - the same arithmetic buildSurfaceGrid uses - so what the cloud shows is exactly what every consumer of the map reads, with no second trace to disagree with it.
void SSRainShadowMap::buildDebugCloud(const Tile& tile, DebugCloud& cloud)
{
    cloud.mPos.clear();
    cloud.mColor.clear();

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp || tile.mDepth.empty() || tile.mRes == 0) return;

    static LLCachedControl<F32> stride_setting(gSavedSettings, "SSAtmoShadowDebugStride", 1.f);

    const U32 res = tile.mRes;
    const F32 stride = fittedStride(res, (F32)stride_setting);
    const S32 steps = (S32)llmax(1.f, floorf((F32)res / stride));

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();

    const LLVector3 origin = regionp->getOriginAgent();
    const LLVector3 eye = origin + tile.mEyeRegion;
    const F32 range = tile.mFar - tile.mNear;
    const F32 width = regionp->getWidth();
    const F32 water_z = regionp->getWaterHeight();
    const LLSurface& land = regionp->getLand();

    const F32 su = 2.f * tile.mHalfW / (F32)res;
    const F32 sv = 2.f * tile.mHalfH / (F32)res;
    const LLVector3 right_step = tile.mRight * (su * stride);
    const LLVector3 up_step    = tile.mUp * (sv * stride);
    const LLVector3 dir_range  = tile.mDir * range;
    const LLVector3 row_start  = eye
        + tile.mRight * (-tile.mHalfW + 0.5f * su)
        + tile.mUp * (-tile.mHalfH + 0.5f * sv)
        + tile.mDir * tile.mNear;

    // The graze test always steps whole texels - it is asking what the stored surface does between neighbouring samples of the map, which is not a question a sub-texel stride changes.
    const S32 istep = llmax(1, (S32)(stride + 0.5f));
    const F32 graze_limit = su * (F32)istep * DEBUG_GRAZE_RATIO;

    auto texel = [&](S32 ax, S32 ay) -> F32
    {
        ax = llclamp(ax, 0, (S32)res - 1);
        ay = llclamp(ay, 0, (S32)res - 1);
        return tile.mDepth[(size_t)ay * res + ax];
    };

    const size_t reserve = (size_t)steps * steps / 2;
    cloud.mPos.reserve(reserve);
    cloud.mColor.reserve(reserve);

    for (S32 sy = 0; sy < steps; ++sy)
    {
        const F32 fy = (F32)sy * stride;
        const S32 iy = llmin((S32)fy, (S32)res - 1);

        LLVector3 p = row_start + up_step * (F32)sy;

        for (S32 sx = 0; sx < steps; ++sx, p += right_step)
        {
            const F32 fx = (F32)sx * stride;
            const S32 ix = llmin((S32)fx, (S32)res - 1);

            F32 d;
            if (stride < 1.f)
            {
                // Asking for more samples than the map has texels, so read it the way resolveColumn does - bilinear between the four around this point. A miss in any corner leaves the whole sample a miss, so holes stay the shape they really are instead of being feathered away.
                const F32 gx = fx - 0.5f;
                const F32 gy = fy - 0.5f;
                const S32 x0 = (S32)floorf(gx);
                const S32 y0 = (S32)floorf(gy);

                const F32 d00 = texel(x0, y0),     d10 = texel(x0 + 1, y0);
                const F32 d01 = texel(x0, y0 + 1), d11 = texel(x0 + 1, y0 + 1);
                if (d00 >= DEPTH_MISS || d10 >= DEPTH_MISS || d01 >= DEPTH_MISS || d11 >= DEPTH_MISS) continue;

                const F32 wx = gx - (F32)x0;
                const F32 wy = gy - (F32)y0;
                d = lerp(lerp(d00, d10, wx), lerp(d01, d11, wx), wy);
            }
            else
            {
                d = texel(ix, iy);
                if (d >= DEPTH_MISS) continue;
            }

            const LLVector3 hit = p + dir_range * d;

            const F32 lx = hit.mV[VX] - origin.mV[VX];
            const F32 ly = hit.mV[VY] - origin.mV[VY];
            const bool off_region = (lx < 0.f || ly < 0.f || lx >= width || ly >= width);

            F32 ground;
            if (sky)
            {
                ground = sky_floor;
            }
            else if (off_region)
            {
                ground = water_z;
            }
            else
            {
                ground = llmax(land.resolveHeightRegion(lx, ly), water_z);
            }

            // Neighbouring texels along both map axes - a large swing means the column walked down a wall between one texel and the next.
            auto tap = [&](S32 ax, S32 ay) -> F32
            {
                const F32 v = texel(ax, ay);
                return (v >= DEPTH_MISS) ? d : v;
            };

            const F32 du = fabsf(tap(ix + istep, iy) - tap(ix - istep, iy)) * range * 0.5f;
            const F32 dv = fabsf(tap(ix, iy + istep) - tap(ix, iy - istep)) * range * 0.5f;
            const bool graze = llmax(du, dv) > graze_limit;

            cloud.mPos.push_back(hit - origin - tile.mDir * DEBUG_LIFT);
            cloud.mColor.push_back(shadowTexelColor(hit.mV[VZ] - ground, graze, off_region,
                                                    !sky && hit.mV[VZ] < water_z - 0.05f));
        }
    }

    cloud.mRight = tile.mRight;
    cloud.mUp = tile.mUp;
    cloud.mHalf = su * stride * 0.5f * DEBUG_QUAD_FILL;

    cloud.mBuiltFrom   = tile.mCaptureTime;
    cloud.mBuiltStride = stride;
    cloud.mBuiltRes    = res;
    cloud.mBuiltFloor  = sky ? sky_floor : 0.f;
    cloud.mBuiltSky    = sky;
}

// The landing grid the surface field and the drainage trace resample the capture into - same figure, so this view and theirs cannot disagree.
static const S32 DEBUG_GRID_RES = 128;
// A landing this far above the terrain under it is sheltered by something rather than sitting on the ground.
static const F32 DEBUG_SHELTER_MIN = 0.5f;

// Bakes the resampled landing grid and its colours. What this view says that the texel cloud cannot: which cells the capture never reached at all, because those fall back to the bare heightmap and every consumer takes that fallback without knowing.
void SSRainShadowMap::buildDebugGrid(const Tile& tile, DebugGrid& grid)
{
    grid.mColor.clear();

    if (!buildSurfaceGrid(tile.mRegionHandle, DEBUG_GRID_RES, grid.mGrid))
    {
        grid.mGrid.mN = 0;
        return;
    }

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp)
    {
        grid.mGrid.mN = 0;
        return;
    }

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();
    const F32 water_z = regionp->getWaterHeight();
    const LLSurface& land = regionp->getLand();

    const S32 n = grid.mGrid.mN;
    grid.mColor.resize((size_t)n * n);

    for (S32 gy = 0; gy < n; ++gy)
    {
        for (S32 gx = 0; gx < n; ++gx)
        {
            const size_t idx = (size_t)gy * n + gx;
            const U8 flags = grid.mGrid.mFlags[idx];
            const F32 z = grid.mGrid.mZ[idx];

            F32 r, g, b, a;

            if (flags & SURF_FALLBACK)
            {
                // The loud one on purpose: the capture told this cell nothing, so its landing came off the heightmap and no roof over it is being felt at all.
                r = 1.f; g = 0.25f; b = 0.1f; a = 0.6f;
            }
            else
            {
                const F32 ground = sky ? sky_floor
                                       : llmax(land.resolveHeightRegion(grid.mGrid.axis(gx), grid.mGrid.axis(gy)), water_z);
                const F32 shelter = z - ground;

                if (shelter < DEBUG_SHELTER_MIN)
                {
                    r = 0.32f; g = 0.36f; b = 0.42f; a = 0.28f;
                }
                else
                {
                    const F32 t = llclamp(shelter / DEBUG_SHELTER_TOP, 0.f, 1.f);
                    if (t < 0.5f)
                    {
                        const F32 k = t * 2.f;
                        r = lerp(0.35f, 0.10f, k); g = lerp(0.90f, 0.85f, k); b = lerp(0.30f, 0.95f, k);
                    }
                    else
                    {
                        const F32 k = (t - 0.5f) * 2.f;
                        r = lerp(0.10f, 0.70f, k); g = lerp(0.85f, 0.25f, k); b = lerp(0.95f, 1.00f, k);
                    }
                    a = lerp(0.5f, 0.9f, t);
                }

                if (flags & SURF_WATER)
                {
                    r *= 0.4f; g = lerp(g, 0.75f, 0.4f); b = lerp(b, 0.95f, 0.5f);
                }
            }

            grid.mColor[idx] = LLColor4U((U8)(llclamp(r, 0.f, 1.f) * 255.f), (U8)(llclamp(g, 0.f, 1.f) * 255.f),
                                         (U8)(llclamp(b, 0.f, 1.f) * 255.f), (U8)(llclamp(a, 0.f, 1.f) * 255.f));
        }
    }

    grid.mBuiltFrom  = tile.mCaptureTime;
    grid.mBuiltFloor = sky ? sky_floor : 0.f;
    grid.mBuiltSky   = sky;
}

// Shared setup for the two world-space views.
static void beginWorldDebug()
{
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

// View 0: the capture's own texels, each drawn as the footprint it covers.
void SSRainShadowMap::drawTexelCloud()
{
    beginWorldDebug();

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 reach_sq = DEBUG_RANGE * DEBUG_RANGE;
    const F32 fade_from = DEBUG_RANGE * 0.6f;
    const F32 fade_from_sq = fade_from * fade_from;

    for (const auto& entry : mDebugCloud)
    {
        const DebugCloud& cloud = entry.second;
        if (cloud.mPos.empty()) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp) continue;

        const LLVector3 base = regionp->getOriginAgent();
        // Each quad is the texel's own footprint, laid in the map plane rather than flat - so a face the fall grazes shows up edge-on and stretched, exactly as the map stores it.
        const LLVector3 ru = cloud.mRight * cloud.mHalf;
        const LLVector3 uu = cloud.mUp * cloud.mHalf;

        gGL.begin(LLRender::TRIANGLES);
        for (size_t i = 0; i < cloud.mPos.size(); ++i)
        {
            const LLVector3 c = base + cloud.mPos[i];

            const F32 dist_sq = (c - cam).magVecSquared();
            if (dist_sq > reach_sq) continue;

            LLColor4U col = cloud.mColor[i];
            if (dist_sq > fade_from_sq)
            {
                const F32 k = 1.f - (sqrtf(dist_sq) - fade_from) / llmax(0.01f, DEBUG_RANGE - fade_from);
                col.mV[3] = (U8)((F32)col.mV[3] * llclamp(k, 0.f, 1.f));
                if (col.mV[3] == 0) continue;
            }

            gGL.color4ubv(col.mV);

            const LLVector3 a = c - ru - uu;
            const LLVector3 b = c + ru - uu;
            const LLVector3 e = c + ru + uu;
            const LLVector3 f = c - ru + uu;

            gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(e.mV);
            gGL.vertex3fv(a.mV); gGL.vertex3fv(e.mV); gGL.vertex3fv(f.mV);
        }
        gGL.end();
    }
}

// View 1: the resampled landing grid, flat cells over the ground they stand for.
void SSRainShadowMap::drawShelterGrid()
{
    beginWorldDebug();

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 reach_sq = DEBUG_RANGE * DEBUG_RANGE;

    for (const auto& entry : mDebugGrid)
    {
        const DebugGrid& grid = entry.second;
        const S32 n = grid.mGrid.mN;
        if (n < 2 || grid.mColor.empty()) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const F32 half = grid.mGrid.mCell * 0.45f;

        gGL.begin(LLRender::TRIANGLES);
        for (S32 gy = 0; gy < n; ++gy)
        {
            for (S32 gx = 0; gx < n; ++gx)
            {
                const size_t idx = (size_t)gy * n + gx;

                const LLVector3 c(origin.mV[VX] + grid.mGrid.axis(gx),
                                  origin.mV[VY] + grid.mGrid.axis(gy),
                                  grid.mGrid.mZ[idx] + 0.06f);
                if ((c - cam).magVecSquared() > reach_sq) continue;

                gGL.color4ubv(grid.mColor[idx].mV);

                gGL.vertex3f(c.mV[VX] - half, c.mV[VY] - half, c.mV[VZ]);
                gGL.vertex3f(c.mV[VX] + half, c.mV[VY] - half, c.mV[VZ]);
                gGL.vertex3f(c.mV[VX] + half, c.mV[VY] + half, c.mV[VZ]);

                gGL.vertex3f(c.mV[VX] - half, c.mV[VY] - half, c.mV[VZ]);
                gGL.vertex3f(c.mV[VX] + half, c.mV[VY] + half, c.mV[VZ]);
                gGL.vertex3f(c.mV[VX] - half, c.mV[VY] + half, c.mV[VZ]);
            }
        }
        gGL.end();
    }
}

// View 2: the capture as captured, on screen. Nothing is unprojected or resampled here, so this is the one view that can be read against SSAtmoShadowRes: holes are black, and if detail is missing at this size it was never in the map to begin with.
void SSRainShadowMap::drawDepthMap()
{
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);

    const Tile* tile = nullptr;
    U64 handle = 0;
    if (cam_region)
    {
        auto it = mTiles.find(cam_region->getHandle());
        if (it != mTiles.end() && it->second.mValid && !it->second.mDepth.empty())
        {
            tile = &it->second;
            handle = it->first;
        }
    }
    if (!tile)
    {
        for (const auto& entry : mTiles)
        {
            if (entry.second.mValid && !entry.second.mDepth.empty())
            {
                tile = &entry.second;
                handle = entry.first;
                break;
            }
        }
    }
    if (!tile) return;

    const U32 res = tile->mRes;

    if (mDebugMapFrom != tile->mCaptureTime || mDebugMapRegion != handle || mDebugMapRes != res)
    {
        // Stretched between the nearest and furthest thing the capture actually saw - the raw range is a thin slice of the far plane, and unstretched it reads as one flat grey.
        F32 lo = 1.f, hi = 0.f;
        for (F32 d : tile->mDepth)
        {
            if (d >= DEPTH_MISS) continue;
            lo = llmin(lo, d);
            hi = llmax(hi, d);
        }
        const F32 span = llmax(hi - lo, 0.0001f);

        std::vector<U8> rgba((size_t)res * res * 4);
        for (size_t i = 0; i < (size_t)res * res; ++i)
        {
            const F32 d = tile->mDepth[i];
            U8* px = &rgba[i * 4];
            if (d >= DEPTH_MISS)
            {
                // A hole: the fall passed clean through and hit nothing at all.
                px[0] = 70; px[1] = 12; px[2] = 16; px[3] = 255;
            }
            else
            {
                // Near is bright, so roofs and canopy stand out white over dark ground.
                const U8 v = (U8)(llclamp(1.f - (d - lo) / span, 0.f, 1.f) * 255.f);
                px[0] = v; px[1] = v; px[2] = v; px[3] = 255;
            }
        }

        if (mDebugMapTex == 0) LLImageGL::generateTextures(1, &mDebugMapTex);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mDebugMapTex);
        LLImageGL::setManualImage(GL_TEXTURE_2D, 0, GL_RGBA8, (S32)res, (S32)res, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data(), false);
        // Point filtering, and no mips - a smoothed debug map would hide exactly the single-texel holes it is here to show.
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);

        mDebugMapFrom = tile->mCaptureTime;
        mDebugMapRegion = handle;
        mDebugMapRes = res;
    }

    if (mDebugMapTex == 0) return;

    const LLRect world = gViewerWindow->getWorldViewRectScaled();
    const F32 vw = (F32)world.getWidth();
    const F32 vh = (F32)world.getHeight();
    const F32 size = llmin(384.f, llmin(vw, vh) * 0.4f);
    const F32 margin = 16.f;
    const F32 x0 = vw - size - margin;
    const F32 y0 = margin;
    const F32 x1 = x0 + size;
    const F32 y1 = y0 + size;

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.ortho(0.f, vw, 0.f, vh, -1.f, 1.f);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadIdentity();

    {
        LLGLDisable depth_test(GL_DEPTH_TEST);
        gGL.setSceneBlendType(LLRender::BT_ALPHA);

        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mDebugMapTex);
        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        gGL.begin(LLRender::TRIANGLES);
        // Flipped in v: the capture's first row is the bottom of the map plane, and ortho here puts y=0 at the bottom of the screen, so an unflipped draw would show it upside down against the world.
        gGL.texCoord2f(0.f, 1.f); gGL.vertex3f(x0, y0, 0.f);
        gGL.texCoord2f(1.f, 1.f); gGL.vertex3f(x1, y0, 0.f);
        gGL.texCoord2f(1.f, 0.f); gGL.vertex3f(x1, y1, 0.f);
        gGL.texCoord2f(0.f, 1.f); gGL.vertex3f(x0, y0, 0.f);
        gGL.texCoord2f(1.f, 0.f); gGL.vertex3f(x1, y1, 0.f);
        gGL.texCoord2f(0.f, 0.f); gGL.vertex3f(x0, y1, 0.f);
        gGL.end();

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4f(0.35f, 0.75f, 1.f, 0.9f);
        gGL.begin(LLRender::LINES);
        gGL.vertex3f(x0, y0, 0.f); gGL.vertex3f(x1, y0, 0.f);
        gGL.vertex3f(x1, y0, 0.f); gGL.vertex3f(x1, y1, 0.f);
        gGL.vertex3f(x1, y1, 0.f); gGL.vertex3f(x0, y1, 0.f);
        gGL.vertex3f(x0, y1, 0.f); gGL.vertex3f(x0, y0, 0.f);
        gGL.end();
    }

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGLLastMatrix = nullptr;
}

// View 3: the frame each capture was taken in - the ortho box, the band it spans, and what state the tile is in. The view for "why did this region never get a usable map", which none of the others can answer because they only draw captures that worked.
void SSRainShadowMap::drawCaptureVolume()
{
    beginWorldDebug();

    const F64 now = SSAtmoMagic::getInstance()->sharedTime();

    for (const auto& entry : mTiles)
    {
        const Tile& tile = entry.second;
        if (tile.mRes == 0) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const LLVector3 eye = origin + tile.mEyeRegion;
        const F32 width = regionp->getWidth();

        LLColor4 col;
        if (!tile.mValid)            col.set(1.f, 0.25f, 0.85f, 0.85f);   // readback still in flight, or it never landed
        else if (tile.mDirty)        col.set(1.f, 0.72f, 0.15f, 0.85f);   // geometry moved under it; waiting on a recapture
        else if (needsCapture(tile)) col.set(0.9f, 0.9f, 0.35f, 0.7f);    // stale for some other reason - age, drift, the camera leaving the band
        else                         col.set(0.3f, 0.9f, 1.f, 0.7f);      // current

        const LLVector3 r = tile.mRight * tile.mHalfW;
        const LLVector3 u = tile.mUp * tile.mHalfH;
        const LLVector3 near_c = eye + tile.mDir * tile.mNear;
        const LLVector3 far_c  = eye + tile.mDir * tile.mFar;

        LLVector3 corner[8];
        corner[0] = near_c - r - u; corner[1] = near_c + r - u;
        corner[2] = near_c + r + u; corner[3] = near_c - r + u;
        corner[4] = far_c  - r - u; corner[5] = far_c  + r - u;
        corner[6] = far_c  + r + u; corner[7] = far_c  - r + u;

        gGL.begin(LLRender::LINES);
        gGL.color4fv(col.mV);
        for (S32 i = 0; i < 4; ++i)
        {
            const S32 j = (i + 1) % 4;
            gGL.vertex3fv(corner[i].mV);     gGL.vertex3fv(corner[j].mV);
            gGL.vertex3fv(corner[i + 4].mV); gGL.vertex3fv(corner[j + 4].mV);
            gGL.vertex3fv(corner[i].mV);     gGL.vertex3fv(corner[i + 4].mV);
        }

        // The eye, as a cross in the map plane.
        const LLVector3 tick_r = tile.mRight * 4.f;
        const LLVector3 tick_u = tile.mUp * 4.f;
        gGL.vertex3fv((eye - tick_r).mV); gGL.vertex3fv((eye + tick_r).mV);
        gGL.vertex3fv((eye - tick_u).mV); gGL.vertex3fv((eye + tick_u).mV);

        // The fall direction out of the eye, so the box can be read against the direction that shaped it.
        gGL.vertex3fv(eye.mV);
        gGL.vertex3fv((eye + tile.mDir * 24.f).mV);

        // The band the capture spans, as the region footprint at its top and bottom - what markDirty tests geometry against.
        for (S32 k = 0; k < 2; ++k)
        {
            const F32 z = k ? tile.mBandTop : tile.mBandBottom;
            gGL.color4f(col.mV[0], col.mV[1], col.mV[2], k ? 0.55f : 0.3f);

            const LLVector3 p0(origin.mV[VX],         origin.mV[VY],         z);
            const LLVector3 p1(origin.mV[VX] + width, origin.mV[VY],         z);
            const LLVector3 p2(origin.mV[VX] + width, origin.mV[VY] + width, z);
            const LLVector3 p3(origin.mV[VX],         origin.mV[VY] + width, z);

            gGL.vertex3fv(p0.mV); gGL.vertex3fv(p1.mV);
            gGL.vertex3fv(p1.mV); gGL.vertex3fv(p2.mV);
            gGL.vertex3fv(p2.mV); gGL.vertex3fv(p3.mV);
            gGL.vertex3fv(p3.mV); gGL.vertex3fv(p0.mV);
        }

        // Age since capture, as a bar rising up the eye - a tile that never refreshes grows a visibly long one.
        if (tile.mValid)
        {
            const F32 age = llclamp((F32)(now - tile.mCaptureTime), 0.f, 60.f);
            gGL.color4f(1.f, 1.f, 1.f, 0.5f);
            gGL.vertex3fv(eye.mV);
            gGL.vertex3f(eye.mV[VX], eye.mV[VY], eye.mV[VZ] + age);
        }
        gGL.end();
    }
}

// Draws the captured depth maps, in whichever of the four views SSAtmoShadowDebugView picks. Only the active view's cache is baked, and the others are dropped, so switching views does not leave the one you are not looking at costing memory.
void SSRainShadowMap::renderDebug()
{
    if (mTiles.empty())
    {
        mDebugCloud.clear();
        mDebugGrid.clear();
        return;
    }

    static LLCachedControl<U32> view_setting(gSavedSettings, "SSAtmoShadowDebugView", 0);
    static LLCachedControl<F32> stride_setting(gSavedSettings, "SSAtmoShadowDebugStride", 1.f);

    const U32 view = ((U32)view_setting < DEBUG_VIEW_COUNT) ? (U32)view_setting : (U32)DEBUG_CLOUD;
    const F32 requested_stride = llclamp((F32)stride_setting, DEBUG_STRIDE_MIN, DEBUG_STRIDE_MAX);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();

    if (view != DEBUG_CLOUD) mDebugCloud.clear();
    if (view != DEBUG_SHELTER) mDebugGrid.clear();

    for (auto it = mDebugCloud.begin(); it != mDebugCloud.end(); )
    {
        it = (mTiles.count(it->first) == 0) ? mDebugCloud.erase(it) : std::next(it);
    }
    for (auto it = mDebugGrid.begin(); it != mDebugGrid.end(); )
    {
        it = (mTiles.count(it->first) == 0) ? mDebugGrid.erase(it) : std::next(it);
    }

    // Both bakes are copies of a capture, so they only go stale when the capture is replaced or when what they are measured against moves - a shifting fall direction needs no rebake, because the texels already fell the way they fell.
    if (view == DEBUG_CLOUD || view == DEBUG_SHELTER)
    {
        for (const auto& entry : mTiles)
        {
            const Tile& tile = entry.second;
            if (!tile.mValid || tile.mDepth.empty()) continue;

            if (view == DEBUG_CLOUD)
            {
                DebugCloud& cloud = mDebugCloud[entry.first];

                const F32 fitted = fittedStride(tile.mRes, requested_stride);

                const bool stale = cloud.mBuiltFrom != tile.mCaptureTime
                                || cloud.mBuiltStride != fitted
                                || cloud.mBuiltRes != tile.mRes
                                || cloud.mBuiltSky != sky
                                || (sky && fabsf(cloud.mBuiltFloor - sky_floor) > 0.5f);

                if (stale) buildDebugCloud(tile, cloud);
            }
            else
            {
                DebugGrid& grid = mDebugGrid[entry.first];

                const bool stale = grid.mBuiltFrom != tile.mCaptureTime
                                || grid.mBuiltSky != sky
                                || (sky && fabsf(grid.mBuiltFloor - sky_floor) > 0.5f);

                if (stale) buildDebugGrid(tile, grid);
            }
        }
    }

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);

    switch (view)
    {
        case DEBUG_SHELTER: drawShelterGrid();   break;
        case DEBUG_MAP:     drawDepthMap();      break;
        case DEBUG_VOLUME:  drawCaptureVolume(); break;
        default:            drawTexelCloud();    break;
    }

    // The fall direction, drawn from the camera's own column down to where that column lands, so every world view can be read against the direction that produced it. Skipped for the on-screen map, which has no world to draw it in.
    if (view != DEBUG_MAP)
    {
        const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
        const LLVector3 dir = atmo->rainDirection();

        LLVector3 landing;
        bool on_water = false;
        const bool mapped = resolveColumn(cam, landing, on_water);

        gGL.begin(LLRender::LINES);
        gGL.color4f(mapped ? 0.4f : 1.f, mapped ? 0.85f : 0.55f, mapped ? 1.f : 0.15f, 0.9f);
        gGL.vertex3fv((landing - dir * 24.f).mV);
        gGL.vertex3fv(landing.mV);
        gGL.end();
    }

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
    out.mAbove.assign((size_t)n * n, 0.f);

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

                    // Height over the ground reference under this cell - the
                    // debug cloud's colour ramp, kept as data. The reference is
                    // the terrain-or-water the fallback path would have used;
                    // in a skybox it is the track floor, so a platform's decks
                    // read as the structure they are rather than as terrain
                    // four thousand metres below them.
                    F32 ground;
                    if (sky)
                    {
                        ground = sky_floor;
                    }
                    else
                    {
                        const LLVector3 centre(origin.mV[VX] + out.axis(gx),
                                               origin.mV[VY] + out.axis(gy),
                                               water_z);
                        ground = llmax(worldp->resolveLandHeightAgent(centre), water_z);
                    }
                    out.mAbove[idx] = llmax(z - ground, 0.f);
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
