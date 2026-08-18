/**
 * @file ssrainshadow.cpp
 * @brief Atmo Magic rain shadow maps: angled ortho depth capture and sampling.
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

// <SS:Nexii> Atmo Magic rain shadow maps

static const F64 CAPTURE_INTERVAL   = 0.25;  // at most one tile capture this often
static const F64 DIRTY_MIN_INTERVAL = 2.0;   // dirty tiles wait at least this long between recaptures
static const F32 BAND_ABOVE         = 80.f;  // capture band relative to the camera
static const F32 BAND_BELOW         = 160.f;
static const F32 BAND_MARGIN        = 30.f;  // recapture when the camera drifts this close to a band edge
static const F32 DIR_EPSILON        = 0.995f; // cos ~6 degrees; recapture when the fall direction swings further
static const F32 NEIGHBOR_REACH     = 64.f;  // tiles for adjacent regions this close to the camera
static const U32 MAX_TILES          = 4;
static const F32 DEPTH_MISS         = 0.9999f; // readback values at/above this hit nothing

static LLTrace::BlockTimerStatHandle FTM_SS_SHADOW("Atmo Magic Shadow Map");

void SSRainShadowMap::clearCache()
{
    mTiles.clear();
}

// static
void SSRainShadowMap::onObjectUpdate(LLViewerObject* objectp)
{
    if (!SSAtmoMagic::getInstance()->isEnabled()) return;
    if (!objectp || objectp->isDead() || objectp->isAvatar() || objectp->isAttachment()) return;

    const LLVector3 scale = objectp->getScale();
    const F32 dim = llmax(scale.mV[VX], scale.mV[VY], scale.mV[VZ]);
    if (dim < 0.5f) return; // too small to matter at map resolution

    getInstance()->markDirty(objectp->getRenderPosition(), scale.magVec() * 0.5f);
}

void SSRainShadowMap::markDirty(const LLVector3& pos_agent, F32 radius)
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end()) return; // no cached tile covering it, nothing to invalidate

    Tile& tile = it->second;
    if (!tile.mValid || tile.mDirty) return;

    // Only geometry inside the captured band can change the map
    const F32 z = pos_agent.mV[VZ];
    if (z + radius < tile.mBandBottom || z - radius > tile.mBandTop + 40.f) return;

    tile.mDirty = true;
}

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

bool SSRainShadowMap::needsCapture(const Tile& tile) const
{
    if (!tile.mValid) return true;
    const F64 now = SSAtmoMagic::getInstance()->sharedTime();

    static LLCachedControl<F32> max_age(gSavedSettings, "SSAtmoShadowMaxAge", 60.f);
    if (now - tile.mCaptureTime > (F64)llmax(5.f, (F32)max_age)) return true;

    if (tile.mDirty && now - tile.mCaptureTime > DIRTY_MIN_INTERVAL) return true;

    // Fall direction drifted away from what the map was rendered with
    if (tile.mDir * SSAtmoMagic::getInstance()->rainDirection() < DIR_EPSILON) return true;

    // Camera left the vertical band the tile was captured around
    const F32 cam_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    if (cam_z > tile.mBandTop - BAND_MARGIN || cam_z < tile.mBandBottom + BAND_MARGIN * 2.f) return true;

    static LLCachedControl<U32> res_setting(gSavedSettings, "SSAtmoShadowRes", 1024);
    if (tile.mRes != (U32)res_setting) return true;

    return false;
}

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

    // Basis perpendicular to the fall direction; dir is always down-ish so
    // the +Y reference never degenerates
    LLVector3 right = dir % LLVector3(0.f, 1.f, 0.f);
    right.normVec();
    LLVector3 up = right % dir;
    up.normVec();

    const F32 cam_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    const F32 band_top = cam_z + BAND_ABOVE;
    const F32 band_bottom = cam_z - BAND_BELOW;

    // Widen the footprint so tilted columns through the region still start
    // inside the map, and stretch the range to cross the whole band
    const F32 inv_z = 1.f / llmax(0.2f, fabsf(dir.mV[VZ]));
    const F32 tilt = sqrtf(llmax(0.f, 1.f - dir.mV[VZ] * dir.mV[VZ]));
    const F32 half = width * 0.5f + (band_top - band_bottom) * tilt * inv_z * 0.5f + 8.f;
    const F32 range = (band_top - band_bottom) * inv_z + 40.f;

    const LLVector3 eye(region_origin.mV[VX] + width * 0.5f,
                        region_origin.mV[VY] + width * 0.5f,
                        band_top);

    // Ortho depth render along the fall direction, reusing the sun shadow
    // machinery; save/restore the render matrices around it
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
        gPipeline.renderShadow(view, proj, shadow_cam, cull_result, true);
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

    return true;
}

void SSRainShadowMap::evict()
{
    // Drop tiles for regions that left the world, then the least recently
    // touched ones beyond the cache cap
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

    // The camera's region always deserves a tile; neighbors get one once the
    // camera is close enough to their border for drops to matter
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
        captureTile(*best);
    }

    evict();
}

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
                const F32 hit_d = tile->mNear + depth * range;
                const F32 pos_d = rel * tile->mDir;
                hit = pos_agent + dir * (hit_d - pos_d);
                from_map = true;

                if (hit_normal)
                {
                    // Surface normal from the depth gradients: forward
                    // differences to the +u/+v texels, slope-clamped so
                    // depth discontinuities at roof edges stay plausible
                    const U32 tx1 = llmin(tx + 1, tile->mRes - 1);
                    const U32 ty1 = llmin(ty + 1, tile->mRes - 1);
                    F32 d_u = tile->mDepth[(size_t)ty * tile->mRes + tx1];
                    F32 d_v = tile->mDepth[(size_t)ty1 * tile->mRes + tx];
                    if (d_u >= DEPTH_MISS) d_u = depth;
                    if (d_v >= DEPTH_MISS) d_v = depth;

                    const F32 texel = 2.f * tile->mHalfW / (F32)tile->mRes;
                    const F32 max_dd = texel * 4.f; // ~76 degrees max slope
                    const F32 dd_u = llclamp((d_u - depth) * range, -max_dd, max_dd);
                    const F32 dd_v = llclamp((d_v - depth) * range, -max_dd, max_dd);

                    LLVector3 t_u = tile->mRight * texel + tile->mDir * dd_u;
                    LLVector3 t_v = tile->mUp * texel + tile->mDir * dd_v;
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

    // Terrain/water floor: fallback when unmapped, clamp when the depth ray
    // slipped past ground level (terrain is in the capture, but a miss at
    // steep tilt or band edges is possible). Columns over the void beyond
    // region borders land on the void water surface, so rain carries on
    // past the sim edge instead of stopping there.
    const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(from_map ? hit : pos_agent);
    const F32 water = regionp ? regionp->getWaterHeight() : SSAtmoMagic::voidWaterHeight();
    const F32 floor_z = llmax(land, water);

    on_water = false;
    if (!from_map || hit.mV[VZ] < floor_z)
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
        on_water = water > land;

        if (hit_normal)
        {
            if (on_water)
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

// </SS:Nexii>
