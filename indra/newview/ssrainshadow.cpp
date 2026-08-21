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

#include <cfloat>

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
static LLTrace::BlockTimerStatHandle FTM_SS_SHADOW_GRID("Atmo Magic Surface Grid");

U32 SSRainShadowMap::resolution() const
{
    // The resolution a tile was actually captured at, which lags the setting
    // until the next capture
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid) return entry.second.mRes;
    }
    return 0;
}

void SSRainShadowMap::clearCache()
{
    mTiles.clear();
    mDebugMesh.clear();
}

// Driven from SSAtmoMagic's settle queue rather than straight off an object
// update. See the note on SSAtmoMagic::onObjectUpdate: an object has to have
// held still for a few seconds before a recapture is spent on it, so a
// projectile or a combat rez never triggers one.
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

    // This is the only place the region's shape is known to have changed. The
    // other reasons a tile gets recaptured - the camera climbing out of the
    // band, the wind swinging the fall direction - produce a new capture of the
    // same build, and anything derived from the map should not be thrown away
    // for those.
    ++tile.mGeomSerial;
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

    // How far upwind the near plane has to start. The plane is perpendicular
    // to the fall direction, so once the wind tilts it, it is no longer level:
    // sitting it on the eye clips away everything upwind of a diagonal cutting
    // through the region, and rain enters those buildings through an angled
    // slice with nothing recorded to shelter it. The furthest a point in the
    // footprint can lie upwind of the centre is the box half-diagonal, and
    // only its horizontal part is foreshortened by the tilt.
    const F32 lead = half * F_SQRT2 * tilt + 16.f;

    // Deep enough to cross the band and to come back out the far side, on top
    // of the lead-in
    const F32 range = lead + (band_top - band_bottom) * inv_z + half * F_SQRT2 * tilt + 40.f;

    // Backed off along the fall direction by the lead, so the whole region and
    // its band sit in front of the near plane whatever the wind is doing.
    // Everything downstream measures depth from this point, so moving it is
    // self-consistent: resolveColumn's u/v run along right/up, which are
    // perpendicular to dir and so unchanged by it.
    const LLVector3 eye = LLVector3(region_origin.mV[VX] + width * 0.5f,
                                    region_origin.mV[VY] + width * 0.5f,
                                    band_top) - dir * lead;

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

        // Rain falls on the world's own shape, not on whoever happens to be
        // standing in the capture band this frame. renderShadow() always
        // includes avatars - correctly, for the real sun shadow it exists
        // for - so without this an avatar walking through the capture area
        // presses a person-shaped dent into the drainage trace, one that
        // moves as they do and drives spurious sheltering and puddle shape
        // for as long as the tile stays cached. Attachments still get
        // through this: they render as ordinary RENDER_TYPE_VOLUME, sharing
        // a type with every rezzed prim in the world, so there is no
        // type-level way to exclude what someone is wearing without also
        // excluding the ground they are standing on. That half stays open.
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

    // Whatever the reason for this capture, the depth in hand now reflects the
    // region as of this revision
    tile.mCapturedSerial = tile.mGeomSerial;

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

        // Recorded before the capture clears it: a recapture forced by
        // geometry changing is the interesting one, and it is the only kind
        // that means anything derived from the map has to be retraced.
        const bool was_dirty = best->mDirty;

        LLTimer timer;
        captureTile(*best);

        mLastCaptureMS = (F32)(timer.getElapsedTimeF64() * 1000.0);
        ++mCaptureCount;
        if (was_dirty) ++mDirtyCaptures;
    }

    evict();
}

U32 SSRainShadowMap::dirtyTileCount() const
{
    U32 n = 0;
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid && entry.second.mDirty) ++n;
    }
    return n;
}

F64 SSRainShadowMap::lastCaptureAge() const
{
    if (mLastCapture <= 0.0) return 0.0;
    return SSAtmoMagic::getInstance()->sharedTime() - mLastCapture;
}

//-----------------------------------------------------------------------------
// Debug visualisation
//-----------------------------------------------------------------------------

static const F32 DEBUG_DIR_EPSILON = 0.9995f;

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
            const F32 lx = llmin((F32)i * step, width);
            const F32 ly = llmin((F32)j * step, width);

            // Drop a column from the top of the captured band. Seeding at the
            // heightmap instead put the sample on a different column wherever
            // the real ground is mesh, which is nearly everywhere, and then
            // drew the answer down at the dirt where a mesh floor hid it.
            const LLVector3 start(region_origin.mV[VX] + lx,
                                  region_origin.mV[VY] + ly,
                                  tile.mBandTop);

            // Ask the real function rather than re-deriving it here. Whatever
            // precipitation sees is what gets drawn, including its mistakes,
            // which is the entire point of a debug view.
            LLVector3 hit;
            bool on_water = false;
            resolveColumn(start, hit, on_water);

            // That column started over this patch, so under a tilted fall it
            // landed a long way downwind of it - the whole sheet slid off the
            // region, and the upwind edge was left with no samples at all, by
            // further the stronger the wind and the higher the camera. Step
            // the seed back upwind by the offset it just measured and ask
            // again, so what gets drawn is the column that lands here rather
            // than the one that leaves here.
            const LLVector3 corrected(2.f * start.mV[VX] - hit.mV[VX],
                                      2.f * start.mV[VY] - hit.mV[VY],
                                      tile.mBandTop);
            const bool mapped = resolveColumn(corrected, hit, on_water);

            const size_t idx = (size_t)j * n + i;

            // Sits on the surface the column actually found - a roof, a mesh
            // floor, terrain, the water - at that surface's own position, and
            // lifted a hair so it does not fight the geometry it describes.
            // Where a roof catches the column the sample climbs onto the roof
            // and sits upwind of the patch it was sheltering, which is the
            // offset the fall angle is asking for.
            mesh.mPos[idx].set(hit.mV[VX] - region_origin.mV[VX],
                               hit.mV[VY] - region_origin.mV[VY],
                               hit.mV[VZ] + 0.12f);

            // Not shelter any more: whether this came out of the depth
            // capture or out of the heightmap it falls back to. Real geometry
            // is what the drop lands on; a fallback is a guess, and seeing
            // where the guesses are is the whole point of looking.
            mesh.mShade[idx] = mapped ? 1.f : 0.f;
        }
    }

    mesh.mBuiltFrom  = tile.mCaptureTime;
    mesh.mBuiltDir   = atmo->rainDirection();
    mesh.mBuiltStep  = step;
    mesh.mBuiltFloor = sky ? sky_floor : 0.f;
    mesh.mBuiltSky   = sky;
}

void SSRainShadowMap::renderDebug()
{
    if (mTiles.empty())
    {
        mDebugMesh.clear();
        return;
    }

    // Cast the map back onto the world rather than drawing the texture in mid
    // air. Each sample drops a column through the region and draws the surface
    // it lands on, so the sheet follows the geometry precipitation is actually
    // using - over roofs, over mesh floors, down onto terrain and water - and
    // offsets downwind exactly as far as the fall angle says it should. Cool
    // and faint is a surface the capture saw; warm and solid is a column with
    // no depth behind it, guessing from the heightmap.
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const LLVector3 dir = atmo->rainDirection();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();

    static LLCachedControl<F32> step_setting(gSavedSettings, "SSAtmoShadowDebugStep", 2.f);
    const F32 step = llclamp((F32)step_setting, 0.5f, 16.f);

    // Snapshot the handles: rebuilding a mesh calls resolveColumn, which walks
    // the tile map, and iterating it at the same time is asking for trouble
    std::vector<U64> handles;
    handles.reserve(mTiles.size());
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid && !entry.second.mDepth.empty())
        {
            handles.push_back(entry.first);
        }
    }

    // Drop meshes for tiles that have gone
    for (auto it = mDebugMesh.begin(); it != mDebugMesh.end(); )
    {
        it = (mTiles.count(it->first) == 0) ? mDebugMesh.erase(it) : std::next(it);
    }

    for (U64 handle : handles)
    {
        const Tile& tile = mTiles[handle];
        ShadowMesh& mesh = mDebugMesh[handle];

        // Rebuild only when what it was baked against has actually changed.
        // Rebaking every frame would mean a quarter of a million column
        // resolves a second for no new information.
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
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);     // test against the world, do not write
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

            // Cool near-black where the column found real geometry, warm and
            // heavier where it fell back to the heightmap. Interpolating
            // across the grid gives the boundary a soft falloff for free.
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
                // A roof edge puts two corners of this cell metres apart in
                // height. Bridging them would hang a curtain down the side of
                // every building; leaving the gap draws the roof and the
                // ground as the separate surfaces they are.
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

    // Fall direction at the camera, so the offset between a building and its
    // shadow reads as an angle rather than as a mystery
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

    // Scatter, not gather. Every texel is projected to where it actually landed
    // in the world and dropped into the region cell it fell in, keeping the
    // highest hit per cell - the first thing a falling drop would meet.
    //
    // Gathering instead (one output cell reads a block of texels) would be
    // marginally cheaper but wrong: the tile is an oblique projection taken
    // around the camera, so a block of texels is not a column of world space,
    // and the mapping between the two shifts every time the tile is recaptured.
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

    // Resolve what each cell ended up being. Water is the one surface the depth
    // pass does not draw, so a hit under the waterline is the seabed and the
    // cell belongs to the water above it.
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

            // Nothing captured in this cell. In a sky band that is open air and
            // stays empty; at ground level the heightmap and the water plane
            // are the only answer there is, and it is only reached where the
            // capture missed, which is rare enough to afford the lookup.
            if (sky)
            {
                out.mZ[idx] = sky_floor;
                out.mFlags[idx] = 0;    // no real surface: nothing to drain
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

    // Sample the map for the column through a point, exactly as resolveColumn
    // does, and hand back where that column meets the surface
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

    // Step outward a texel at a time and stop where the surface does. The last
    // step that stayed level with the start is the lip.
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

                // How far a neighbouring texel may differ before it is treated
                // as a different surface rather than a slope of this one:
                // one texel across, four along, which is the same ~76 degree
                // ceiling the normal below is clamped to.
                const F32 max_step = (texel_u * 4.f) / llmax(range, 0.01f);

                // Depth at a texel, in the surface this sample landed on:
                // misses and anything across a discontinuity - the lip of a
                // roof, the side of a prim - are pulled back to the texel that
                // was actually hit, so an edge stays an edge instead of
                // smearing a ramp out into the air beside it.
                auto tap = [&](S32 ix, S32 iy) -> F32
                {
                    ix = llclamp(ix, 0, (S32)tile->mRes - 1);
                    iy = llclamp(iy, 0, (S32)tile->mRes - 1);
                    const F32 d = tile->mDepth[(size_t)iy * tile->mRes + ix];
                    if (d >= DEPTH_MISS) return depth;
                    return llclamp(d, depth - max_step, depth + max_step);
                };

                // Interpolate the depth across the four texels around the
                // sample rather than taking the one it fell in. A nearest
                // lookup reports the height of the texel centre, so on a slope
                // the impact is placed up to a texel's worth of rise off the
                // real surface - half the time below it - and at a quarter of
                // a metre per texel that is enough to bury a ripple in the
                // ground it is supposed to be lying on. This is the same
                // reconstruction the gradients below assume.
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
                    // Surface normal from the depth gradients. Centred
                    // differences rather than forward ones: a forward
                    // difference is the slope half a texel downhill of the
                    // sample, which tilts every normal the same way and leans
                    // the ripple into the surface on one side.
                    const F32 max_dd = texel_u * 4.f; // ~76 degrees max slope
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

    // Floor of the active track. At ground level that is terrain and water as
    // before; in a sky band it is the track's own ground zero, because the
    // terrain thousands of metres below is not what precipitation up there
    // should be landing on.
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
        // The capture already contains whatever the ground is actually built
        // from - mesh, prims and the terrain alike - so a hit is the answer.
        // The heightmap underneath it is a relic that says nothing about where
        // a build's floor is, and clamping a real hit up to it can only ever
        // move a drop onto a surface that is not there. The one surface the
        // depth pass does not draw is water, so a hit under the waterline is
        // the seabed and the drop belongs on the water above it.
        floor_z = regionp ? regionp->getWaterHeight() : SSAtmoMagic::voidWaterHeight();
        floor_is_water = true;
    }
    else
    {
        // No depth for this column at all: outside the captured footprint, or
        // a region with no tile yet. The heightmap is a poor stand-in for
        // ground that is usually built rather than sculpted, but without the
        // map it is the only answer there is. Columns over the void beyond
        // region borders land on the void water surface, so rain carries on
        // past the sim edge instead of stopping there.
        const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(pos_agent);
        const F32 water = regionp ? regionp->getWaterHeight() : SSAtmoMagic::voidWaterHeight();
        floor_is_water = water > land;
        floor_z = llmax(land, water);
    }

    on_water = false;

    // Either there was no hit to use, or the hit is below the surface the
    // drop should have landed on: the water at ground level, and nothing at
    // all in a sky band, where a real hit below the imaginary floor is a
    // platform hanging under the band base and stays authoritative.
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

    // false here means the column found no real surface. Callers in a sky
    // track use that to fade the drop out instead of landing it on nothing.
    return from_map;
}

// </SS:Nexii>
