/**
 * @file sswindflow.cpp
 * @brief Atmo Magic wind flowmap: per-region height capture, adaptive slicing,
 *        GPU pressure projection and sampling.
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

#include "sswindflow.h"

#include "ssatmomagic.h"
#include "ssatmotrack.h"
#include "ssatmoenvbridge.h"

#include "llagent.h"
#include "llenvironment.h"
#include "llfasttimer.h"
#include "llrender.h"
#include "llsurface.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llworld.h"
#include "pipeline.h"
#include "workqueue.h"

#include <algorithm>
#include <functional>
#include <set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// <SS:Nexii> Atmo Magic wind flowmap

static const F64 BUILD_MIN_INTERVAL   = 0.25;  // at most one region solved this often
static const F64 DIRTY_MIN_INTERVAL   = 3.0;   // a rezzing region resolves at most this often
static const F32 DEPTH_MISS           = 0.9999f;
static const F32 NO_SURFACE           = -1.0e6f;
static const F32 PROBE_MISS           =  1.0e6f;  // ray left the world without a hit
static const F32 WIND_EPSILON         = 0.75f; // m/s change that forces a resolve
static const F32 NEIGHBOR_REACH       = 96.f;  // neighbour tiles once the camera is this close to their border
static const U32 MAX_TILES            = 4;
static const S32 HISTOGRAM_BINS       = 64;

// A soffit has to cover at least this much ground before it earns a slab
// boundary of its own, and only so many of them can, or a region full of
// balconies would spend the entire slab budget on undersides. Expressed as an
// area rather than a column count so it means the same thing at any cell size.
static const F32 UNDERSIDE_MIN_AREA   = 24.f;   // square metres
static const S32 UNDERSIDE_MAX_BOUNDS = 4;

// How far below the overhead surface a probe hit has to land before it counts
// as something the overhead pass missed. Below this it is the same surface seen
// at an angle, and depth precision alone would fill the list with noise.
static const F32 HIDDEN_CLEARANCE     = 1.5f;

// Above this share of rays hitting nothing, a probe is treated as broken
// rather than as looking at open country. A real region viewed at a downward
// angle always has terrain under most of its rays.
static const F32 PROBE_MAX_MISS       = 0.98f;

// Width classes for the flow line view. Each costs a draw call, and the spread
// is deliberately wide: a jet is worth finding across a whole region, and a
// factor of two is not visible against a thicket of lines.
static const S32 SS_WIND_LINE_WIDTHS = 4;

// Capture debug views, in the order the floater lists them
static const S32 CAPTURE_VIEW_CANDIDATES = SS_WIND_PROBES + 2;

// Why a solid cell ended up the way it did
enum SSCarveFlag : U8
{
    CARVE_AIR = 0,          // never solid: above the captured surface
    CARVE_BLOCKED,          // a probe hit something in front of it, correctly solid
    CARVE_OPENED,           // a probe saw it, carved
    CARVE_NO_EVIDENCE       // every probe either missed or could not see this far
};

// The band is probed over this multiple of its nominal height before being
// settled, so a genuinely tall build is found rather than clipped off
static const F32 PROBE_SCALE          = 4.f;
static const F32 PROBE_PERCENTILE     = 0.995f;

static LLTrace::BlockTimerStatHandle FTM_SS_WINDFLOW("Atmo Magic Wind Flow");

//-----------------------------------------------------------------------------
// Capability
//-----------------------------------------------------------------------------

// static
bool SSWindFlowMap::isSupported()
{
    // Compute plus image load/store, i.e. GL 4.3. Bias down the compare the
    // way llgl.cpp does for its own capability flags.
    return gGLManager.mGLVersion >= 4.29f
        && glDispatchCompute != nullptr
        && glBindImageTexture != nullptr
        && glTexStorage3D != nullptr;
}

//-----------------------------------------------------------------------------
// Domain geometry
//-----------------------------------------------------------------------------

// A region's domain is the region itself plus a margin of overlap into each
// neighbour. That overlap is the whole answer to the seam problem: solving each
// region in isolation would leave wind stopping dead at a sim border, whereas a
// tile that can see 64m into next door produces very nearly the same flow near
// the border as its neighbour's tile does, because both are looking at the same
// buildings.
static void desiredGeometry(LLViewerRegion* regionp, S32& res, F32& extent, F32& margin)
{
    static LLCachedControl<F32> cell_setting(gSavedSettings, "SSAtmoWindFlowCell", 4.f);
    static LLCachedControl<F32> margin_setting(gSavedSettings, "SSAtmoWindFlowMargin", 64.f);
    static LLCachedControl<U32> res_cap(gSavedSettings, "SSAtmoWindFlowRes", 192);

    const F32 cell = llclamp((F32)cell_setting, 1.f, 32.f);
    margin = llclamp((F32)margin_setting, 0.f, 256.f);
    extent = regionp->getWidth() + margin * 2.f;

    // Cell size is the target, not a guarantee: a varregion wide enough to blow
    // past the cap gets coarser cells rather than a runaway solve
    res = llclamp((S32)llround(extent / cell), 32, (S32)llclamp((U32)res_cap, 32u, 512u));

    // Down to a multiple of sixteen, so the pressure pyramid halves exactly
    // four times and no level has to deal with a stray odd row. Rounded down
    // rather than up so this can never push back through the cap.
    res = llmax(32, (res / 16) * 16);
}

// How many levels the pyramid gets. Stops halving once a level is too small
// for the pass count to be doing anything a single sweep would not.
S32 SSWindFlowMap::levelCount(S32 res)
{
    S32 levels = 1;
    while (levels < SS_WIND_MAX_LEVELS
           && (res >> levels) >= SS_WIND_MIN_LEVEL_RES
           && ((res >> (levels - 1)) % 2) == 0)
    {
        ++levels;
    }
    return levels;
}

// Everything that changes what the solve produces, rolled into one value. The
// simulation floater edits these live, and comparing a hash is both cheaper and
// harder to forget to update than checking nine settings by hand.
static U32 tuningSignature()
{
    static LLCachedControl<F32> cell(gSavedSettings, "SSAtmoWindFlowCell", 4.f);
    static LLCachedControl<F32> margin(gSavedSettings, "SSAtmoWindFlowMargin", 64.f);
    static LLCachedControl<F32> height(gSavedSettings, "SSAtmoWindFlowHeight", 192.f);
    static LLCachedControl<F32> gradient(gSavedSettings, "SSAtmoWindFlowGradient", 0.25f);
    static LLCachedControl<F32> slice_min(gSavedSettings, "SSAtmoWindFlowSliceMin", 3.f);
    static LLCachedControl<U32> iterations(gSavedSettings, "SSAtmoWindFlowIterations", 128);
    static LLCachedControl<U32> slice_max(gSavedSettings, "SSAtmoWindFlowSliceMax", 6);
    static LLCachedControl<U32> shelter(gSavedSettings, "SSAtmoWindFlowShelterSteps", 2);
    static LLCachedControl<U32> res_cap(gSavedSettings, "SSAtmoWindFlowRes", 192);
    static LLCachedControl<F32> strength(gSavedSettings, "SSAtmoWindFlowStrength", 2.f);
    static LLCachedControl<F32> curve(gSavedSettings, "SSAtmoWindFlowSolidCurve", 1.f);
    static LLCachedControl<F32> shelter_amt(gSavedSettings, "SSAtmoWindFlowShelterAmount", 0.4f);
    static LLCachedControl<F32> max_gain(gSavedSettings, "SSAtmoWindFlowMaxGain", 4.f);
    static LLCachedControl<F32> probe_angle(gSavedSettings, "SSAtmoWindFlowProbeAngle", 30.f);
    static LLCachedControl<bool> use_probes(gSavedSettings, "SSAtmoWindFlowProbes", true);
    static LLCachedControl<U32> probe_res(gSavedSettings, "SSAtmoWindFlowProbeRes", 2);
    static LLCachedControl<U32> passage_gap(gSavedSettings, "SSAtmoWindFlowPassageGap", 4);

    U32 h = 2166136261u;
    auto mix = [&h](U32 v) { h = (h ^ v) * 16777619u; };

    mix((U32)llround((F32)cell * 100.f));
    mix((U32)llround((F32)margin * 100.f));
    mix((U32)llround((F32)height * 100.f));
    mix((U32)llround((F32)gradient * 1000.f));
    mix((U32)llround((F32)slice_min * 100.f));
    mix((U32)iterations);
    mix((U32)slice_max);
    mix((U32)shelter);
    mix((U32)res_cap);
    mix((U32)llround((F32)strength * 1000.f));
    mix((U32)llround((F32)curve * 1000.f));
    mix((U32)llround((F32)shelter_amt * 1000.f));
    mix((U32)llround((F32)max_gain * 1000.f));
    mix((U32)llround((F32)probe_angle * 100.f));
    mix(use_probes ? 1u : 0u);
    mix((U32)probe_res);
    mix((U32)passage_gap);

    return h;
}

//-----------------------------------------------------------------------------
// Resources
//-----------------------------------------------------------------------------

// glGetError reports one error from a queue the whole frame shares, so an
// error left behind by unrelated code reads back here as ours. Emptying the
// queue first is what makes the checks below mean anything: during login in
// particular there is plenty of other GL traffic to inherit a stale error from.
static void drainGLErrors()
{
    for (S32 guard = 0; guard < 32 && glGetError() != GL_NO_ERROR; ++guard) {}
}

static U32 createVolume(S32 res, S32 slices, GLenum format)
{
    U32 tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexStorage3D(GL_TEXTURE_3D, 1, format, res, res, slices);
    glBindTexture(GL_TEXTURE_3D, 0);
    return tex;
}

bool SSWindFlowMap::ensureResources(S32 res, S32 slices)
{
    if (mTexRes == res && mTexSlices == slices && mHeightTex != 0
        && (S32)mProbeTexRes >= mProbeRes) return true;

    releaseResources();

    drainGLErrors();

    // Named per allocation rather than once at the end: a failure here is
    // nearly always one specific texture being too large for the driver, and
    // the whole point of the warning is to say which one.
    const char* step = nullptr;
    auto failed = [&step](const char* what)
    {
        if (!step && glGetError() != GL_NO_ERROR) step = what;
    };

    glGenTextures(1, &mHeightTex);
    glBindTexture(GL_TEXTURE_2D, mHeightTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, res, res);
    glBindTexture(GL_TEXTURE_2D, 0);
    failed("the heightfield");

    glGenTextures(1, &mProbeTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, mProbeTex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const S32 probe_res = llmax(mProbeRes, res);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R32F,
                   probe_res, probe_res, SS_WIND_PROBES);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    failed("the probe array");

    // Each level is a quarter of the one above it, so the whole pyramid costs
    // about a third again on top of the full-resolution grid.
    mTexLevels = levelCount(res);
    for (S32 L = 0; L < mTexLevels; ++L)
    {
        const S32 r = levelRes(res, L);
        mSolidTex[L]       = createVolume(r, slices, GL_R8);
        mVelTex[L]         = createVolume(r, slices, GL_RGBA16F);
        mDivTex[L]         = createVolume(r, slices, GL_R32F);
        mPressureTex[L][0] = createVolume(r, slices, GL_R32F);
        mPressureTex[L][1] = createVolume(r, slices, GL_R32F);
        failed("the pressure pyramid");
    }

    if (step)
    {
        LL_WARNS("AtmoMagic") << "Wind flowmap could not allocate " << step
                              << " at " << res << "x" << res << "x" << slices
                              << " (probes " << probe_res << ")" << LL_ENDL;
        releaseResources();
        return false;
    }

    mTexRes = res;
    mTexSlices = slices;
    // Without this the size check at the top can never be satisfied and every
    // solve throws the whole set away and builds it again.
    mProbeTexRes = probe_res;
    return true;
}

void SSWindFlowMap::releaseResources()
{
    if (mHeightTex) glDeleteTextures(1, &mHeightTex);
    if (mProbeTex) glDeleteTextures(1, &mProbeTex);
    mHeightTex = mProbeTex = 0;

    for (S32 L = 0; L < SS_WIND_MAX_LEVELS; ++L)
    {
        U32 textures[5] = { mSolidTex[L], mVelTex[L], mDivTex[L],
                            mPressureTex[L][0], mPressureTex[L][1] };
        for (U32& t : textures)
        {
            if (t) glDeleteTextures(1, &t);
        }
        mSolidTex[L] = mVelTex[L] = mDivTex[L] = 0;
        mPressureTex[L][0] = mPressureTex[L][1] = 0;
    }

    mTexRes = mTexSlices = mTexLevels = 0;
    mProbeTexRes = 0;

    if (mCapture.getWidth() > 0) mCapture.release();
}

void SSWindFlowMap::clear()
{
    // Whatever is in flight is now for a world that has gone
    abandonBuild();

    if (mWorkerBusy)
    {
        // The worker holds a reference into mTiles and reads the scratch.
        // Erasing either out from under it is the one thing that is not safe,
        // so this finishes in the completion handler instead.
        mClearPending = true;
        return;
    }

    mTiles.clear();
    mTop.clear();
    mHidden.clear();
    for (S32 i = 0; i < SS_WIND_PROBES; ++i) mProbeDepth[i].clear();
    releaseResources();
}

void SSWindFlowMap::rebuildAll()
{
    // Zero the build time as well as dirtying: that is what the settle delay
    // for a rezzing region is measured from, and an explicit rebuild should
    // not have to wait it out.
    for (auto& entry : mTiles)
    {
        entry.second.mDirty = true;
        entry.second.mBuildTime = 0.0;
    }
    mLastBuild = 0.0;

    // Anything part-built was built against the settings that just changed
    abandonBuild();
}

bool SSWindFlowMap::ensureShaders()
{
    if (mShaderFailed) return false;
    if (mShadersReady) return true;

    mShadersReady = gSSWindInitProgram.isComplete()
                 && gSSWindDivProgram.isComplete()
                 && gSSWindJacobiProgram.isComplete()
                 && gSSWindProjectProgram.isComplete()
                 && gSSWindSeedProgram.isComplete()
                 && gSSWindRestrictProgram.isComplete()
                 && gSSWindProlongProgram.isComplete();

    if (!mShadersReady)
    {
        // The shader manager already logged why; stop trying so we do not
        // spam a failure every frame
        mShaderFailed = true;
    }
    return mShadersReady;
}

//-----------------------------------------------------------------------------
// Tile lookup
//-----------------------------------------------------------------------------

SSWindFlowMap::Tile* SSWindFlowMap::tileFor(LLViewerRegion* regionp, bool allow_create)
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

const SSWindFlowMap::Tile* SSWindFlowMap::tileAt(const LLVector3& pos_agent) const
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return nullptr;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end() || !it->second.mValid) return nullptr;
    return &it->second;
}

const SSWindFlowMap::Tile* SSWindFlowMap::cameraTile() const
{
    return tileAt(LLViewerCamera::getInstance()->getOrigin());
}

bool SSWindFlowMap::isValid() const
{
    return cameraTile() != nullptr;
}

// static
bool SSWindFlowMap::drivesWind()
{
    return SSAtmoMagic::instanceExists()
        && SSAtmoMagic::getInstance()->isEnabled()
        && SSWindFlowMap::instanceExists()
        && SSWindFlowMap::getInstance()->isValid();
}

//-----------------------------------------------------------------------------
// Staleness
//-----------------------------------------------------------------------------

// static
//
// Called from SSAtmoMagic::settleEdits, not from the object update itself: an
// edit has to have held still for a few seconds before it is allowed to cost a
// solve. See the note on SSAtmoMagic::onObjectUpdate.
void SSWindFlowMap::markDirty(const LLVector3& pos, F32 radius)
{
    SSWindFlowMap* self = getInstance();
    if (self->mTiles.empty()) return;
    if (radius < 0.5f) return;

    // Bounding-box overlap against each domain rather than a point-in-region
    // test, so a sim surround megaprim whose centre is in the next region over
    // still dirties the tile it actually covers
    for (auto& entry : self->mTiles)
    {
        Tile& tile = entry.second;
        if (!tile.mValid || tile.mDirty) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent() + tile.mOriginRegion;

        if (pos.mV[VX] + radius < origin.mV[VX] ||
            pos.mV[VY] + radius < origin.mV[VY] ||
            pos.mV[VX] - radius > origin.mV[VX] + tile.mExtent ||
            pos.mV[VY] - radius > origin.mV[VY] + tile.mExtent ||
            pos.mV[VZ] + radius < tile.mBandBottom ||
            pos.mV[VZ] - radius > tile.mBandTop)
        {
            continue;
        }

        tile.mDirty = true;
    }
}

bool SSWindFlowMap::needsSolve(const Tile& tile) const
{
    if (!tile.mValid) return true;
    if (tile.mDirty) return true;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;

    // The camera moved to a different sky track, so the band this was solved
    // for is not the one being flown in
    if (tile.mTrack != SSAtmoTrackManager::getInstance()->currentTrack()) return true;

    // Ambient wind changed enough to matter. The map is static with respect to
    // a fixed inflow; change the inflow and it has to be solved again.
    if ((SSAtmoMagic::getInstance()->wind() - tile.mBuiltWind).magVec() > WIND_EPSILON) return true;

    // Anything that changes what the solve produces was edited, most likely in
    // the simulation floater, so the answer on file is for different settings
    if (tile.mTuning != tuningSignature()) return true;

    S32 res; F32 extent, margin;
    desiredGeometry(regionp, res, extent, margin);
    if (tile.mRes != res || fabsf(tile.mExtent - extent) > 0.5f) return true;

    return false;
}

//-----------------------------------------------------------------------------
// Vertical band
//-----------------------------------------------------------------------------

void SSWindFlowMap::chooseBand(Tile& tile, LLViewerRegion* regionp)
{
    static LLCachedControl<F32> height_setting(gSavedSettings, "SSAtmoWindFlowHeight", 192.f);
    const F32 nominal = llclamp((F32)height_setting, 48.f, 1024.f);

    SSAtmoTrackManager* tracks = SSAtmoTrackManager::getInstance();
    const S32 track = tracks->currentTrack();

    // Base of the band. At ground level that is the lowest land in the region,
    // or the water surface where the seabed drops below it, so a deep trench
    // does not drag the band down away from the build. In a sky track it is the
    // track's own floor, which is that track's ground zero.
    F32 base;
    if (track <= SS_TRACK_MIN)
    {
        base = llmin(regionp->getLand().getMinZ(), regionp->getWaterHeight());
    }
    else
    {
        base = tracks->trackFloor(track);
    }

    tile.mTrack = track;
    tile.mBandBottom = base - 8.f;
    tile.mBandTop = base + nominal;     // provisional; the probe capture settles it

    // A slab must never straddle two tracks, because each carries its own
    // ambient wind, so the band stops at the track ceiling
    const F32 ceiling = tracks->trackCeiling(track);
    if (ceiling > tile.mBandBottom + 32.f)
    {
        tile.mBandTop = llmin(tile.mBandTop, ceiling);
    }
}

//-----------------------------------------------------------------------------
// Height capture
//-----------------------------------------------------------------------------

bool SSWindFlowMap::captureAlong(LLRenderTarget& target, S32 res, const Tile& tile,
                                 const LLVector3& dir, const LLVector3& eye,
                                 F32 half, F32 range, std::vector<F32>& out, glm::mat4& view_out)
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;

    // A basis perpendicular to the view direction. World up degenerates when
    // looking straight down, which the overhead pass does, so fall back to +Y
    // there; the probes are all tilted and never hit that case.
    const bool straight_down = fabsf(dir.mV[VZ]) > 0.999f;
    LLVector3 right = dir % (straight_down ? LLVector3(0.f, 1.f, 0.f) : LLVector3(0.f, 0.f, 1.f));
    right.normVec();
    LLVector3 up = right % dir;
    up.normVec();

    const glm::mat4 saved_view = get_current_modelview();
    const glm::mat4 saved_proj = get_current_projection();
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;

    const glm::mat4 view = glm::lookAt(
        glm::vec3(eye.mV[VX], eye.mV[VY], eye.mV[VZ]),
        glm::vec3(eye.mV[VX] + dir.mV[VX], eye.mV[VY] + dir.mV[VY], eye.mV[VZ] + dir.mV[VZ]),
        glm::vec3(up.mV[VX], up.mV[VY], up.mV[VZ]));
    const glm::mat4 proj = glm::ortho(-half, half, -half, half, 0.f, range);

    set_current_modelview(view);
    set_current_projection(proj);
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_SUN_SHADOW3;

    LLCamera cam = *LLViewerCamera::getInstance();
    cam.setOrigin(eye);
    cam.setFar(range);

    // Corner order matters: calcAgentFrustumPlanes builds each plane from three
    // of these in sequence, so a basis wound the other way inverts every plane
    // normal and the cull throws the whole world away.
    LLVector3 frust[8];
    frust[0] = eye - right * half - up * half;
    frust[1] = eye + right * half - up * half;
    frust[2] = eye + right * half + up * half;
    frust[3] = eye - right * half + up * half;
    for (U32 i = 0; i < 4; i++)
    {
        frust[i + 4] = frust[i] + dir * range;
    }
    cam.calcAgentFrustumPlanes(frust);
    cam.mFrustumCornerDist = 0.f;

    if (target.getWidth() != (U32)res)
    {
        target.release();
        if (!target.allocate(res, res, 0, true)) return false;
    }

    target.bindTarget();
    target.getViewport(gGLViewport);
    target.clear();

    {
        static LLCullResult cull_result;
        gPipeline.renderShadow(view, proj, cam, cull_result, true);
    }

    std::vector<F32> depth((size_t)res * res);
    glReadPixels(0, 0, res, res, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());

    target.flush();

    set_current_modelview(saved_view);
    set_current_projection(saved_proj);
    LLViewerCamera::sCurCameraID = saved_camera;

    // Ortho projection, so window depth is linear in view distance
    out.resize(depth.size());
    for (size_t i = 0; i < depth.size(); ++i)
    {
        out[i] = (depth[i] >= DEPTH_MISS) ? PROBE_MISS : depth[i] * range;
    }

    view_out = view;
    return true;
}

bool SSWindFlowMap::captureHeights(Tile& tile)
{
    // Probe pass. The nominal band height is only a guess at how tall the build
    // is, and a region with a 400m tower would have it sliced off at the
    // ceiling, so look down from well above and let the geometry set the real
    // ceiling.
    const F32 nominal = tile.mBandTop - tile.mBandBottom;
    const F32 probe_top = tile.mBandBottom + nominal * PROBE_SCALE;
    const F32 half = tile.mExtent * 0.5f;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;

    const LLVector3 region_origin = regionp->getOriginAgent();
    const LLVector3 centre(region_origin.mV[VX] + tile.mOriginRegion.mV[VX] + half,
                           region_origin.mV[VY] + tile.mOriginRegion.mV[VY] + half,
                           0.f);

    // Overhead pass: everything below the surface it finds is solid. That is
    // wrong under a skyway and right everywhere else, and being wrong the
    // conservative way is what lets the probes correct it afterwards without
    // any risk of opening a hole that is not there.
    {
        const F32 range = llmax(probe_top - tile.mBandBottom, 1.f);
        const LLVector3 eye(centre.mV[VX], centre.mV[VY], probe_top);

        std::vector<F32> depth;
        glm::mat4 unused;
        if (!captureAlong(mCapture, tile.mRes, tile, LLVector3(0.f, 0.f, -1.f),
                          eye, half, range, depth, unused))
        {
            return false;
        }

        mTop.resize(depth.size());
        for (size_t i = 0; i < depth.size(); ++i)
        {
            mTop[i] = (depth[i] >= PROBE_MISS * 0.5f) ? NO_SURFACE : (probe_top - depth[i]);
        }
    }

    // High percentile rather than the maximum: one lone platform on a pole
    // should not stretch the band, but a genuine tower should still fit inside
    // it. Empty columns carry no surface and are not part of the distribution.
    std::vector<F32> surfaces;
    surfaces.reserve(mTop.size());
    for (F32 h : mTop)
    {
        if (h > NO_SURFACE * 0.5f) surfaces.push_back(h);
    }

    F32 ceiling = tile.mBandTop;
    if (!surfaces.empty())
    {
        const size_t nth = (size_t)((F32)(surfaces.size() - 1) * PROBE_PERCENTILE);
        std::nth_element(surfaces.begin(), surfaces.begin() + nth, surfaces.end());
        const F32 tall = surfaces[nth];

        // Headroom above the tallest thing, so wind has somewhere undisturbed
        // to flow over the rooftops rather than scraping the ceiling
        ceiling = tall + llmax(16.f, (tall - tile.mBandBottom) * 0.25f);
    }

    tile.mBandTop = llclamp(ceiling, tile.mBandBottom + 48.f, probe_top);

    // Anything that reached above the settled ceiling is solid up to it
    for (F32& h : mTop)
    {
        if (h > NO_SURFACE * 0.5f) h = llmin(h, tile.mBandTop);
    }

    // The probes follow, one per frame, driven by the build state machine
    beginProbes(tile);
    return true;
}

void SSWindFlowMap::auditProbes(const Tile& tile) const
{
    if (mTop.empty()) return;

    // The tallest column in the region. Whatever else is true, a cell halfway
    // up a building is solid, and no probe should be able to see it.
    size_t best = 0;
    for (size_t i = 1; i < mTop.size(); ++i)
    {
        if (mTop[i] > mTop[best]) best = i;
    }
    if (mTop[best] <= NO_SURFACE * 0.5f) return;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return;

    const F32 cell = tile.mExtent / (F32)tile.mRes;
    const LLVector3 grid_origin = regionp->getOriginAgent() + tile.mOriginRegion;

    const S32 cx = (S32)(best % tile.mRes);
    const S32 cy = (S32)(best / tile.mRes);
    const F32 top = mTop[best];

    const LLVector3 world(grid_origin.mV[VX] + ((F32)cx + 0.5f) * cell,
                          grid_origin.mV[VY] + ((F32)cy + 0.5f) * cell,
                          0.5f * (top + tile.mBandBottom));

    LL_DEBUGS("AtmoMagic") << "Probe audit: tallest column (" << cx << "," << cy
                          << ") top " << llformat("%.1f", top)
                          << ", testing " << llformat("(%.1f, %.1f, %.1f)",
                                                      world.mV[VX], world.mV[VY], world.mV[VZ])
                          << ", probe half " << llformat("%.1f", mProbeHalf[0])
                          << ", grid origin " << llformat("(%.1f, %.1f)",
                                                          grid_origin.mV[VX], grid_origin.mV[VY])
                          << LL_ENDL;

    for (S32 i = 0; i < SS_WIND_PROBES; ++i)
    {
        if (mProbeDepth[i].empty()) continue;

        const glm::vec3 v = glm::vec3(mProbeView[i] * glm::vec4(world.mV[VX], world.mV[VY],
                                                                world.mV[VZ], 1.f));
        const F32 dist = -v.z;
        const F32 u = v.x / mProbeHalf[i] * 0.5f + 0.5f;
        const F32 w = v.y / mProbeHalf[i] * 0.5f + 0.5f;

        std::string verdict;
        F32 hit = -1.f;
        if (u < 0.f || u > 1.f || w < 0.f || w > 1.f)
        {
            verdict = "outside footprint, no information";
        }
        else
        {
            const S32 tx = llclamp((S32)(u * (F32)mProbeRes), 0, mProbeRes - 1);
            const S32 ty = llclamp((S32)(w * (F32)mProbeRes), 0, mProbeRes - 1);
            hit = mProbeDepth[i][(size_t)ty * mProbeRes + tx];
            verdict = (hit >= PROBE_MISS * 0.5f)
                    ? "ray hit nothing, no evidence either way"
                    : (dist < hit - 1.5f)
                        ? "SEES IT - would carve this solid cell open"
                        : "blocked, correct";
        }

        LL_DEBUGS("AtmoMagic") << "  probe " << i
                              << llformat(" dist %.1f", dist)
                              << llformat(" uv (%.3f, %.3f)", u, w)
                              << (hit >= 0.f ? llformat(" hit %.1f", hit) : std::string())
                              << " : " << verdict << LL_ENDL;
    }
}

// Set up for the probe passes. The captures themselves are one per frame from
// here on, so what they all share is settled once, up front.
void SSWindFlowMap::beginProbes(Tile& tile)
{
    static LLCachedControl<U32> probe_mult(gSavedSettings, "SSAtmoWindFlowProbeRes", 2);

    // The box is wider than the region and the ground inside it is
    // foreshortened, so matching the mask's resolution would leave a probe
    // texel covering several cells. Overshoot deliberately.
    mProbeRes = llclamp(tile.mRes * (S32)llclamp((U32)probe_mult, 1u, 4u), tile.mRes, 1536);

    mHidden.clear();
    for (S32 i = 0; i < SS_WIND_PROBES; ++i)
    {
        mProbeUsable[i] = false;
        mProbeMiss[i] = 1.f;
    }

    // What the capture debug view will draw until the next build replaces it,
    // and what the reconstruction resolves its hits against. Settled here
    // rather than after the last probe, because the reconstruction runs on a
    // worker and must not be reaching into LLWorld for the region.
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    mCaptureRegion = tile.mRegionHandle;
    mCaptureRes = tile.mRes;
    mCaptureCell = tile.mExtent / (F32)tile.mRes;
    mCaptureOrigin = (regionp ? regionp->getOriginAgent() : LLVector3::zero) + tile.mOriginRegion;
}

// One oblique probe: a full scene render and a depth readback, which is why
// only one of them happens per frame.
bool SSWindFlowMap::captureProbe(Tile& tile, S32 i)
{
    static LLCachedControl<F32> elevation(gSavedSettings, "SSAtmoWindFlowProbeAngle", 30.f);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;

    const F32 half_extent = tile.mExtent * 0.5f;
    const LLVector3 region_origin = regionp->getOriginAgent();

    const F32 band_lo = tile.mBandBottom;
    const F32 band_hi = tile.mBandTop;

    const F32 span = llmax(band_hi - band_lo, 1.f);
    const F32 elev = llclamp((F32)elevation, 10.f, 70.f) * DEG_TO_RAD;
    const F32 ce = cosf(elev);
    const F32 se = sinf(elev);

    {
        const F32 z_lo = band_lo;
        const F32 z_hi = band_hi;

        const F32 az = (F32)i * F_PI_BY_TWO;
        const LLVector3 dir(sinf(az) * ce, cosf(az) * ce, -se);

        const LLVector3 centre(region_origin.mV[VX] + tile.mOriginRegion.mV[VX] + half_extent,
                               region_origin.mV[VY] + tile.mOriginRegion.mV[VY] + half_extent,
                               0.5f * (z_lo + z_hi));

        // Footprint. Across the view the region is never wider than itself; up
        // the view it is the region foreshortened by the tilt plus the vertical
        // span standing up in it. The old form inflated by the cotangent, which
        // is the shear of the *depth* range rather than the height of the
        // image, and cost most of the probe resolution for nothing.
        const F32 diag = half_extent * F_SQRT2;
        const F32 half = llmax(diag, se * diag + ce * span * 0.5f) + 8.f;

        // Depth has to cross the whole box from outside it
        const F32 range = 2.f * (ce * diag + se * span * 0.5f) + 32.f;

        const LLVector3 eye = centre - dir * (range * 0.5f);

        mProbeHalf[i] = half;

        if (!captureAlong(mProbeCapture, mProbeRes, tile, dir, eye, half, range,
                          mProbeDepth[i], mProbeView[i]))
        {
            return false;
        }

        {
            LLVector3 fr = dir % LLVector3(0.f, 0.f, 1.f);
            fr.normVec();
            LLVector3 fu = fr % dir;
            fu.normVec();
            mProbeFrame[i].mEye = eye;
            mProbeFrame[i].mDir = dir;
            mProbeFrame[i].mRight = fr;
            mProbeFrame[i].mUp = fu;
        }

        // A probe that hit nothing anywhere is far more likely to have failed
        // than to be looking at an empty region, and the two are impossible to
        // tell apart from the carve's point of view: a miss means "no evidence"
        // per ray, but a probe that is nothing but misses contributes nothing
        // and may be masking a broken capture. Drop it and say so.
        size_t misses = 0;
        for (F32 d : mProbeDepth[i])
        {
            if (d >= PROBE_MISS * 0.5f) ++misses;
        }

        const F32 miss_frac = mProbeDepth[i].empty()
                            ? 1.f : (F32)misses / (F32)mProbeDepth[i].size();
        mProbeMiss[i] = miss_frac;

        if (miss_frac > PROBE_MAX_MISS)
        {
            LL_WARNS("AtmoMagic") << "Wind flow probe " << i << " saw nothing ("
                                  << llformat("%.1f%%", miss_frac * 100.f)
                                  << " miss); dropping it" << LL_ENDL;
            return true;
        }

        mProbeUsable[i] = true;
    }

    return true;
}

// CPU, worker: reconstruct where every probe ray landed. A hit well below the
// overhead surface for that column is by definition something the overhead
// pass could not see - either the underside of an overhang or the ground
// beneath it - and both are altitudes the slicer needs.
//
// Four probes at up to 1536 texels each is nine million rays to walk, which is
// the largest single CPU cost in a build and the reason this is off the main
// thread rather than inline in the capture.
void SSWindFlowMap::reconstructHidden(Tile& tile)
{
    mHidden.clear();

    const F32 cell = tile.mExtent / (F32)tile.mRes;
    const LLVector3 grid_origin = mCaptureOrigin;

    for (S32 i = 0; i < SS_WIND_PROBES; ++i)
    {
        if (!mProbeUsable[i]) continue;
        if ((S32)mProbeDepth[i].size() < mProbeRes * mProbeRes) continue;

        const LLVector3& eye = mProbeFrame[i].mEye;
        const LLVector3& dir = mProbeFrame[i].mDir;
        const LLVector3& fr = mProbeFrame[i].mRight;
        const LLVector3& fu = mProbeFrame[i].mUp;
        const F32 half = mProbeHalf[i];

        for (S32 y = 0; y < mProbeRes; ++y)
        {
            for (S32 x = 0; x < mProbeRes; ++x)
            {
                const F32 d = mProbeDepth[i][(size_t)y * mProbeRes + x];
                if (d >= PROBE_MISS * 0.5f) continue;

                const F32 u = ((F32)x + 0.5f) / (F32)mProbeRes * 2.f - 1.f;
                const F32 v = ((F32)y + 0.5f) / (F32)mProbeRes * 2.f - 1.f;
                const LLVector3 hit = eye + fr * (u * half) + fu * (v * half) + dir * d;

                const S32 cx = (S32)((hit.mV[VX] - grid_origin.mV[VX]) / cell);
                const S32 cy = (S32)((hit.mV[VY] - grid_origin.mV[VY]) / cell);
                if (cx < 0 || cy < 0 || cx >= tile.mRes || cy >= tile.mRes) continue;

                const F32 top = mTop[(size_t)cy * tile.mRes + cx];
                if (top <= NO_SURFACE * 0.5f) continue;
                if (hit.mV[VZ] > top - HIDDEN_CLEARANCE) continue;

                mHidden.push_back(hit.mV[VZ]);
            }
        }
    }

    const F32 samples = (F32)mProbeRes * (F32)mProbeRes * (F32)SS_WIND_PROBES;
    tile.mCarved = (samples > 0.f) ? (F32)mHidden.size() / samples : 0.f;
}

//-----------------------------------------------------------------------------
// Adaptive slicing
//-----------------------------------------------------------------------------

// The carve happens inside the init pass, where it cannot report itself. This
// repeats it on the CPU and records why each solid cell ended up as it did, so
// a passage that fails to open can be told apart from one that was never a
// candidate. Only run while the view that draws it is selected: it is a pass
// over the whole volume against every probe.
void SSWindFlowMap::buildCarveFlags(const Tile& tile)
{
    static LLCachedControl<U32> capture_view(gSavedSettings, "SSAtmoWindFlowDebugCapture", 0);
    if ((S32)capture_view != CAPTURE_VIEW_CANDIDATES)
    {
        mCarveFlags.clear();
        return;
    }

    const S32 res = tile.mRes;
    const S32 slices = tile.mSlices;
    const F32 cell = tile.mExtent / (F32)res;
    const F32 bias = 1.5f * cell;

    mCarveFlags.assign((size_t)res * res * slices, CARVE_AIR);

    for (S32 k = 0; k < slices; ++k)
    {
        const F32 lo = tile.mSliceZ[k];
        const F32 hi = tile.mSliceZ[k + 1];

        for (S32 y = 0; y < res; ++y)
        {
            for (S32 x = 0; x < res; ++x)
            {
                const F32 top = mTop[(size_t)y * res + x];
                if (top <= NO_SURFACE * 0.5f || top <= lo) continue;   // never solid

                const F32 wx = mCaptureOrigin.mV[VX] + ((F32)x + 0.5f) * cell;
                const F32 wy = mCaptureOrigin.mV[VY] + ((F32)y + 0.5f) * cell;
                const F32 z = 0.5f * (lo + llmin(hi, top));

                bool seen = false;
                bool blocked = false;

                for (S32 i = 0; i < SS_WIND_PROBES && !seen; ++i)
                {
                    if (!mProbeUsable[i] || mProbeDepth[i].empty()) continue;

                    const glm::vec3 v = glm::vec3(mProbeView[i] * glm::vec4(wx, wy, z, 1.f));
                    const F32 dist = -v.z;
                    if (dist < 0.f) continue;

                    const F32 u = v.x / mProbeHalf[i] * 0.5f + 0.5f;
                    const F32 w = v.y / mProbeHalf[i] * 0.5f + 0.5f;
                    if (u < 0.f || u > 1.f || w < 0.f || w > 1.f) continue;

                    const S32 tx = llclamp((S32)(u * (F32)mProbeRes), 0, mProbeRes - 1);
                    const S32 ty = llclamp((S32)(w * (F32)mProbeRes), 0, mProbeRes - 1);
                    const F32 hit = mProbeDepth[i][(size_t)ty * mProbeRes + tx];

                    if (hit >= PROBE_MISS * 0.5f) continue;    // no evidence either way

                    if (dist < hit - bias) seen = true;
                    else blocked = true;
                }

                const size_t idx = index(tile, x, y, k);
                mCarveFlags[idx] = seen ? CARVE_OPENED
                                        : (blocked ? CARVE_BLOCKED : CARVE_NO_EVIDENCE);
            }
        }
    }
}

void SSWindFlowMap::placeSlices(Tile& tile)
{
    static LLCachedControl<F32> min_sep(gSavedSettings, "SSAtmoWindFlowSliceMin", 3.f);
    static LLCachedControl<U32> max_slices(gSavedSettings, "SSAtmoWindFlowSliceMax", 6);

    const F32 sep = llmax(1.f, (F32)min_sep);
    const S32 cap = llclamp((S32)max_slices, SS_WIND_MIN_SLICES, SS_WIND_MAX_SLICES);

    F32 lo = tile.mBandTop;
    F32 hi = tile.mBandBottom;
    std::vector<S32> histogram(HISTOGRAM_BINS, 0);

    // Undersides count as geometry here just as much as rooftops do. The mask
    // already knows a bridge deck is solid between its underside and its top,
    // and that the span below it is open - but a slab boundary has to land in
    // that opening for the solve to have anywhere to put the air. Placing
    // slabs from rooftops alone leaves the clearance under a deck sitting
    // inside one slab, where it averages into a partly solid haze and the wind
    // neither flows under it nor is properly stopped by it.
    auto eachSurface = [&](const std::function<void(F32)>& fn)
    {
        for (F32 h : mTop)
        {
            if (h > NO_SURFACE * 0.5f) fn(h);
        }
        for (F32 h : mHidden)
        {
            fn(h);
        }
    };

    eachSurface([&](F32 h)
    {
        const F32 z = llclamp(h, tile.mBandBottom, tile.mBandTop);
        lo = llmin(lo, z);
        hi = llmax(hi, z);
    });

    if (hi <= lo)
    {
        // Nothing captured: a flat pair of slabs over the band is still usable
        lo = tile.mBandBottom;
        hi = tile.mBandTop;
    }

    // Give the top slab some air above the tallest thing, so wind has somewhere
    // undisturbed to flow over the rooftops
    hi = llmin(hi + llmax(8.f, (hi - lo) * 0.25f), tile.mBandTop);

    const F32 span = llmax(hi - lo, 1.f);
    S32 count = (S32)llround(span / sep);
    count = llclamp(count, SS_WIND_MIN_SLICES, cap);

    // Histogram of surface heights, so boundaries land where the geometry
    // actually changes rather than spreading evenly through empty air
    eachSurface([&](F32 h)
    {
        const F32 t = (llclamp(h, lo, hi) - lo) / span;
        histogram[llclamp((S32)(t * (HISTOGRAM_BINS - 1)), 0, HISTOGRAM_BINS - 1)]++;
    });

    S32 total = 0;
    for (S32 n : histogram) total += n;

    std::vector<F32> bounds;
    bounds.push_back(lo);

    if (total > 0)
    {
        // Quantile placement: walk the cumulative distribution and drop a
        // boundary every 1/count of the mass
        S32 running = 0;
        S32 next = 1;
        for (S32 b = 0; b < HISTOGRAM_BINS && next < count; ++b)
        {
            running += histogram[b];
            while (next < count && running >= (total * next) / count)
            {
                bounds.push_back(lo + span * ((F32)(b + 1) / (F32)HISTOGRAM_BINS));
                ++next;
            }
        }
    }

    while ((S32)bounds.size() < count)
    {
        bounds.push_back(lo + span * ((F32)bounds.size() / (F32)count));
    }

    std::sort(bounds.begin(), bounds.end());

    // -----------------------------------------------------------------------
    // Forced boundaries.
    //
    // Quantile placement is mass-weighted, which is right for deciding where
    // the region's bulk sits and useless for anything small. One bridge over
    // one street is a few dozen columns out of a hundred thousand: it can
    // never win a quantile boundary, so the opening underneath it lands inside
    // a slab that also contains the deck, comes out half solid, and the wind
    // neither goes under nor around. These get placed first and the quantiles
    // fill in around them.
    // -----------------------------------------------------------------------
    std::vector<F32> forced;

    // A slab may never straddle two EEP tracks: each carries its own ambient
    // wind. This one is a correctness constraint, so it goes in first.
    SSAtmoTrackManager* tracks = SSAtmoTrackManager::getInstance();
    for (S32 track = SS_TRACK_MIN; track <= SS_TRACK_MAX; ++track)
    {
        forced.push_back(tracks->trackFloor(track));
    }

    // Then the altitudes the probes found under cover. A boundary sitting at a
    // soffit, and another at the floor below it, is what gives the air in
    // between a slab of its own to travel along.
    {
        const F32 cell = tile.mExtent / (F32)llmax(1, tile.mRes);
        const S32 min_columns = llmax(4, (S32)(UNDERSIDE_MIN_AREA / llmax(0.01f, cell * cell)));

        std::vector<S32> under_count(HISTOGRAM_BINS, 0);
        std::vector<F32> under_sum(HISTOGRAM_BINS, 0.f);

        for (F32 h : mHidden)
        {
            const F32 t = (llclamp(h, lo, hi) - lo) / span;
            const S32 b = llclamp((S32)(t * (HISTOGRAM_BINS - 1)), 0, HISTOGRAM_BINS - 1);
            under_count[b]++;
            under_sum[b] += h;
        }

        // Peaks only, and only ones with enough columns behind them to be a
        // structure rather than a stray hit on the edge of a prim.
        std::vector<std::pair<S32, F32>> peaks;
        for (S32 b = 0; b < HISTOGRAM_BINS; ++b)
        {
            const S32 n = under_count[b];
            if (n < min_columns) continue;
            if (b > 0 && under_count[b - 1] > n) continue;
            if (b + 1 < HISTOGRAM_BINS && under_count[b + 1] > n) continue;
            peaks.push_back({ n, under_sum[b] / (F32)n });
        }

        // Strongest first, so if the slab budget runs out it is the biggest
        // spans that keep their boundary
        std::sort(peaks.begin(), peaks.end(),
                  [](const std::pair<S32, F32>& a, const std::pair<S32, F32>& b)
                  { return a.first > b.first; });

        for (size_t i = 0; i < peaks.size() && i < (size_t)UNDERSIDE_MAX_BOUNDS; ++i)
        {
            forced.push_back(peaks[i].second);
        }
    }

    // -----------------------------------------------------------------------
    // Assemble, forced first. The old walk took boundaries in altitude order
    // and stopped at the cap, which spent the whole budget near the ground and
    // left the rooftops unresolved whenever the band was tall.
    // -----------------------------------------------------------------------
    std::vector<F32> final_bounds;
    final_bounds.push_back(lo);
    final_bounds.push_back(hi);

    auto tryAdd = [&](F32 z)
    {
        if (z <= lo || z >= hi) return;
        if ((S32)final_bounds.size() >= cap + 1) return;
        for (F32 b : final_bounds)
        {
            if (fabsf(b - z) < sep) return;
        }
        final_bounds.push_back(z);
    };

    for (F32 z : forced) tryAdd(z);
    for (F32 z : bounds) tryAdd(z);

    std::sort(final_bounds.begin(), final_bounds.end());

    // A band with almost no geometry in it can come out of that with fewer
    // slabs than the solve needs; halve the widest one until it has enough.
    while ((S32)final_bounds.size() - 1 < SS_WIND_MIN_SLICES)
    {
        size_t widest = 0;
        for (size_t i = 1; i + 1 <= final_bounds.size() - 1; ++i)
        {
            if (final_bounds[i + 1] - final_bounds[i]
                > final_bounds[widest + 1] - final_bounds[widest])
            {
                widest = i;
            }
        }
        final_bounds.insert(final_bounds.begin() + widest + 1,
                            0.5f * (final_bounds[widest] + final_bounds[widest + 1]));
    }

    tile.mSlices = llclamp((S32)final_bounds.size() - 1, SS_WIND_MIN_SLICES, cap);
    for (S32 i = 0; i <= tile.mSlices; ++i)
    {
        tile.mSliceZ[i] = final_bounds[llmin((size_t)i, final_bounds.size() - 1)];
    }

    // Low end of the captured surfaces is the ground reference for the wind
    // gradient. The mean would be dragged upward by rooftops, which is exactly
    // the height the gradient is supposed to be measuring from.
    tile.mGroundRef = lo;

    static LLCachedControl<F32> gradient(gSavedSettings, "SSAtmoWindFlowGradient", 0.25f);
    const F32 alpha = llclamp((F32)gradient, 0.f, 0.6f);

    // Ambient wind per slab, from the track that slab sits in. Tried as a v3
    // altitude-band track first - resolveActiveTrack() returns false when
    // v3 has no asset loaded, the same "is v3 actually driving anything"
    // question SSAtmoMagic::refreshParams() asks before picking cfg there -
    // so this stays in lockstep with whichever source is actually live
    // rather than always reading v2's SSAtmoTrackManager regardless. Read from
    // the wrong one and the solve keeps baking in whatever the legacy
    // config's wind speed was (0, most likely, or whatever it was last
    // left at) no matter what the v3 floater's Wind Speed slider says.
    for (S32 k = 0; k < tile.mSlices; ++k)
    {
        const F32 centre = 0.5f * (tile.mSliceZ[k] + tile.mSliceZ[k + 1]);

        SSAtmoTrackConfig v3_cfg;
        bool v3_is_ground = false;
        const bool v3_active = SSAtmoEnvBridge::resolveActiveTrack(
            centre, centre, false, v3_cfg, v3_is_ground);

        if (v3_active)
        {
            tile.mAmbient[k] = v3_cfg.runs() ? v3_cfg.windDirection() * v3_cfg.mWindSpeed
                                              : SSAtmoMagic::getInstance()->wind();
        }
        else
        {
            const S32 track = llclamp(
                LLEnvironment::instance().calculateSkyTrackForAltitude((F64)centre),
                SS_TRACK_MIN, SS_TRACK_MAX);

            const SSAtmoTrackConfig& cfg = tracks->config(track);
            tile.mAmbient[k] = cfg.runs() ? cfg.windDirection() * cfg.mWindSpeed
                                          : SSAtmoMagic::getInstance()->wind();
        }

        // Atmospheric boundary layer. Ground drag slows the air near the
        // surface and releases it with height, which is why a rooftop is
        // windier than the street below it and why getting above the roofline
        // should feel exposed. Power law against a 10m reference, the standard
        // engineering form.
        if (alpha > 0.f)
        {
            const F32 h = llmax(centre - tile.mGroundRef, 0.5f);
            tile.mAmbient[k] *= llclamp(powf(h / 10.f, alpha), 0.35f, 3.f);
        }
    }
}

//-----------------------------------------------------------------------------
// Passage bridging
//-----------------------------------------------------------------------------

// The probes are an evidence-only test, so they open the mouth of an underpass
// and leave the middle of it solid: no ray reaches in that far. The middle is
// nonetheless open, and the evidence for it is sitting on either side.
//
// Opening a solid cell that has carved cells on both sides along an axis is
// that inference and nothing more. Requiring both sides is what makes it safe
// against the case that kills every looser rule - the ring of overhang carved
// around a building, which has open air outside it and solid building inside,
// and so never presents evidence on both sides of anything.
// GL half, main thread: pull the mask the init pass produced. Split out from
// the bridge itself so the pass over the volume can run on a worker.
void SSWindFlowMap::readMaskForBridge(const Tile& tile)
{
    mMaskRaw.clear();
    mMaskBridged.clear();
    mMaskChanged = false;

    static LLCachedControl<U32> gap(gSavedSettings, "SSAtmoWindFlowPassageGap", 4);
    if (llclamp((S32)gap, 0, 16) == 0) return;

    const size_t allocated = (size_t)mTexRes * mTexRes * mTexSlices;
    mMaskRaw.assign(allocated, 0);
    glBindTexture(GL_TEXTURE_3D, mSolidTex[0]);
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RED, GL_UNSIGNED_BYTE, mMaskRaw.data());
    glBindTexture(GL_TEXTURE_3D, 0);
}

// GL half, main thread: put back whatever the bridge opened
void SSWindFlowMap::uploadBridgedMask(const Tile& tile)
{
    if (!mMaskChanged || mMaskBridged.empty()) return;

    glBindTexture(GL_TEXTURE_3D, mSolidTex[0]);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, tile.mRes, tile.mRes, tile.mSlices,
                    GL_RED, GL_UNSIGNED_BYTE, mMaskBridged.data());
    glBindTexture(GL_TEXTURE_3D, 0);
}

void SSWindFlowMap::bridgePassages(const Tile& tile)
{
    static LLCachedControl<U32> gap(gSavedSettings, "SSAtmoWindFlowPassageGap", 4);
    const S32 reach = llclamp((S32)gap, 0, 16);
    if (reach == 0 || mMaskRaw.empty()) return;

    const S32 res = tile.mRes;
    const S32 slices = tile.mSlices;
    const size_t active = (size_t)res * res * slices;

    const std::vector<U8>& solid = mMaskRaw;

    // A cell counts as carved only where the heightmap called it solid and the
    // carve opened it. Air above the rooftops is open too and is no evidence of
    // a passage, so it must not seed one.
    std::vector<U8> carved(active, 0);
    for (S32 k = 0; k < slices; ++k)
    {
        const F32 lo = tile.mSliceZ[k];
        for (S32 y = 0; y < res; ++y)
        {
            for (S32 x = 0; x < res; ++x)
            {
                const F32 top = mTop[(size_t)y * res + x];
                if (top <= NO_SURFACE * 0.5f || top <= lo) continue;   // never solid

                const size_t idx = index(tile, x, y, k);
                if (solid[idx] < 128) carved[idx] = 1;
            }
        }
    }

    auto carvedAt = [&](S32 x, S32 y, S32 k) -> bool
    {
        if (x < 0 || y < 0 || k < 0 || x >= res || y >= res || k >= slices) return false;
        return carved[index(tile, x, y, k)] != 0;
    };

    mMaskBridged.assign(solid.begin(), solid.begin() + active);
    std::vector<U8>& bridged = mMaskBridged;
    size_t opened = 0;

    static const S32 AXES[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

    for (S32 k = 0; k < slices; ++k)
    {
        for (S32 y = 0; y < res; ++y)
        {
            for (S32 x = 0; x < res; ++x)
            {
                const size_t idx = index(tile, x, y, k);
                if (solid[idx] < 128) continue;     // already open

                for (const S32* a : AXES)
                {
                    bool before = false, after = false;
                    for (S32 d = 1; d <= reach && !before; ++d)
                    {
                        before = carvedAt(x - a[0] * d, y - a[1] * d, k - a[2] * d);
                    }
                    if (!before) continue;

                    for (S32 d = 1; d <= reach && !after; ++d)
                    {
                        after = carvedAt(x + a[0] * d, y + a[1] * d, k + a[2] * d);
                    }
                    if (!after) continue;

                    bridged[idx] = 0;
                    ++opened;
                    break;
                }
            }
        }
    }

    if (opened == 0)
    {
        mMaskBridged.clear();
        return;
    }

    // The upload is GL, so it waits for solveRun back on the main thread
    mMaskChanged = true;

    LL_DEBUGS("AtmoMagic") << "Wind flow bridged " << opened
                          << llformat(" cells (%.2f%% of the volume)",
                                      (F32)opened / (F32)llmax<size_t>(active, 1) * 100.f)
                          << " between carved ones" << LL_ENDL;
}

//-----------------------------------------------------------------------------
// Solve
//-----------------------------------------------------------------------------

// A uniform the running program does not have takes -1 here, and every
// glUniform call against -1 is quietly ignored. That is how a whole set of
// tuning knobs came to do nothing at all while looking wired up, so say so
// once per name instead of letting it pass.
static S32 uniformLoc(const LLGLSLShader& shader, const char* name)
{
    const S32 loc = glGetUniformLocation(shader.mProgramObject, name);
    if (loc < 0)
    {
        static std::set<std::string> reported;
        if (reported.insert(shader.mName + "." + name).second)
        {
            LL_WARNS("AtmoMagic") << "wind flow shader " << shader.mName
                                  << " has no uniform " << name
                                  << "; its value is being dropped. A stale "
                                     "cached program binary does this."
                                  << LL_ENDL;
        }
    }
    return loc;
}

// Uniforms are set in groups rather than all at once because the passes do not
// all declare the same ones, and a driver drops any uniform a shader does not
// read. Handing a pass a value it has no use for would trip the missing-uniform
// warning on every solve and drown out the case that warning is there to catch.
//
// The cost of that arrangement is that adding a use to a shader means adding
// the matching call here, and nothing enforces it: an unset uniform is zero,
// not absent, so the missing-uniform warning stays quiet. That is how the carve
// came to compute every cell's position from a zero cell size and test the same
// corner of the domain for the whole region.

static void setGrid(LLGLSLShader& shader, S32 res, S32 slices)
{
    glUniform1i(uniformLoc(shader, "uRes"), res);
    glUniform1i(uniformLoc(shader, "uSlices"), slices);
}

static void setExtent(LLGLSLShader& shader, F32 extent)
{
    glUniform1f(uniformLoc(shader, "uExtent"), extent);
}

static void setSliceZ(LLGLSLShader& shader, const F32* slice_z, S32 slices)
{
    glUniform1fv(uniformLoc(shader, "uSliceZ"), slices + 1, slice_z);
}

static void setAmbient(LLGLSLShader& shader, const LLVector3* ambient, S32 slices)
{
    F32 amb[SS_WIND_MAX_SLICES * 3] = { 0.f };
    for (S32 k = 0; k < slices; ++k)
    {
        amb[k * 3 + 0] = ambient[k].mV[VX];
        amb[k * 3 + 1] = ambient[k].mV[VY];
        amb[k * 3 + 2] = ambient[k].mV[VZ];
    }
    glUniform3fv(uniformLoc(shader, "uAmbient"), slices, amb);
}

bool SSWindFlowMap::solveInit(const Tile& tile)
{
    LL_PROFILE_GPU_ZONE("atmo wind flow init");

    const S32 res = tile.mRes;
    const S32 slices = tile.mSlices;

    // Upload the captured height field and the oblique probe depths
    glBindTexture(GL_TEXTURE_2D, mHeightTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, res, res, GL_RED, GL_FLOAT, mTop.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindTexture(GL_TEXTURE_2D_ARRAY, mProbeTex);
    for (S32 i = 0; i < SS_WIND_PROBES; ++i)
    {
        if ((S32)mProbeDepth[i].size() < mProbeRes * mProbeRes) continue;
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, mProbeRes, mProbeRes, 1,
                        GL_RED, GL_FLOAT, mProbeDepth[i].data());
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    auto groupsFor = [](S32 r) { return (GLuint)((r + 7) / 8); };

    // --- build the finest solid mask and seed its velocity field ---
    static LLCachedControl<F32> solid_curve(gSavedSettings, "SSAtmoWindFlowSolidCurve", 1.f);

    gSSWindInitProgram.bind();
    setGrid(gSSWindInitProgram, res, slices);
    setExtent(gSSWindInitProgram, tile.mExtent);    // the carve needs cellSize()
    setSliceZ(gSSWindInitProgram, tile.mSliceZ, slices);
    setAmbient(gSSWindInitProgram, tile.mAmbient, slices);
    glUniform1f(uniformLoc(gSSWindInitProgram, "uSolidCurve"),
                llclamp((F32)solid_curve, 0.1f, 4.f));

    // Everything the probes need to be looked up in. The view matrices are
    // agent space as of the capture that produced them, which is why this is
    // only ever read inside the build that captured it.
    {
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
        const LLVector3 grid_origin = (regionp ? regionp->getOriginAgent() : LLVector3::zero)
                                    + tile.mOriginRegion;

        // Probes are numbered contiguously in the shader, so pack the usable
        // ones down to the front rather than leaving holes it would have to
        // test around. Filled before the upload, obviously - but this went out
        // the other way round once, and unset matrices are not a failure the
        // shader can notice: it happily projects through them and carves the
        // mask open on the strength of whatever was on the stack.
        static LLCachedControl<bool> use_probes(gSavedSettings, "SSAtmoWindFlowProbes", true);

        glm::mat4 views[SS_WIND_PROBES];
        for (S32 i = 0; i < SS_WIND_PROBES; ++i) views[i] = glm::mat4(1.f);

        S32 usable = 0;
        if (use_probes)
        {
            for (S32 i = 0; i < SS_WIND_PROBES; ++i)
            {
                if (mProbeUsable[i]) views[usable++] = mProbeView[i];
            }
        }

        glUniform2f(uniformLoc(gSSWindInitProgram, "uOrigin"),
                    grid_origin.mV[VX], grid_origin.mV[VY]);
        glUniformMatrix4fv(uniformLoc(gSSWindInitProgram, "uProbeView"),
                           SS_WIND_PROBES, GL_FALSE, glm::value_ptr(views[0]));
        glUniform1i(uniformLoc(gSSWindInitProgram, "uProbeCount"), usable);
        glUniform1i(uniformLoc(gSSWindInitProgram, "uProbeRes"), mProbeRes);
        F32 halves[SS_WIND_PROBES] = { 0.f };
        for (S32 i = 0, n = 0; i < SS_WIND_PROBES; ++i)
        {
            if (use_probes && mProbeUsable[i]) halves[n++] = mProbeHalf[i];
        }
        glUniform1fv(uniformLoc(gSSWindInitProgram, "uProbeHalf"), SS_WIND_PROBES, halves);
        glUniform1f(uniformLoc(gSSWindInitProgram, "uProbeBias"),
                    1.5f * tile.mExtent / (F32)llmax(1, res));
        glBindImageTexture(3, mProbeTex, 0, GL_TRUE, 0, GL_READ_ONLY, GL_R32F);
    }

    glBindImageTexture(0, mHeightTex,   0, GL_FALSE, 0, GL_READ_ONLY,  GL_R32F);
    glBindImageTexture(1, mSolidTex[0], 0, GL_TRUE,  0, GL_WRITE_ONLY, GL_R8);
    glBindImageTexture(2, mVelTex[0],   0, GL_TRUE,  0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(groupsFor(res), groupsFor(res), (GLuint)slices);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // --- infer the parts of a passage no probe could reach ---
    // Read the mask out here and stop. Joining the passages up is a pass over
    // the whole volume against three axes, which is the single largest CPU
    // cost in a build, so it runs on a worker and solveRun picks up from what
    // it produced. It edits the mask behind the init pass, so the velocity
    // seeded from the old mask is stale wherever a cell was opened - which is
    // why solveRun re-seeds rather than leaving dead air in a passage that now
    // exists.
    readMaskForBridge(tile);
    return true;
}

bool SSWindFlowMap::solveRun(const Tile& tile)
{
    LL_PROFILE_GPU_ZONE("atmo wind flow solve");

    // This pass reports its result through glGetError at the end, so it has to
    // start from an empty queue or it inherits somebody else's failure.
    drainGLErrors();

    // Passes per level. Jacobi settles a feature of wavelength L cells in
    // something like L^2 passes, so this alone decides how large a structure
    // the pressure field can see: at a metre per cell a hundred or so passes
    // resolves the few metres around a corner and nothing wider. The pyramid
    // is what covers the rest - each level below sees the same lane at half
    // the wavelength, for a quarter of the cost - so this is a detail budget
    // now rather than a reach budget.
    static LLCachedControl<U32> iterations(gSavedSettings, "SSAtmoWindFlowIterations", 128);
    const S32 iters = llclamp((S32)iterations, 4, 512);

    const S32 res = tile.mRes;
    const S32 slices = tile.mSlices;
    const S32 levels = llmin(mTexLevels, levelCount(res));

    auto groupsFor = [](S32 r) { return (GLuint)((r + 7) / 8); };

    uploadBridgedMask(tile);

    gSSWindSeedProgram.bind();
    setGrid(gSSWindSeedProgram, res, slices);
    setAmbient(gSSWindSeedProgram, tile.mAmbient, slices);
    glBindImageTexture(1, mSolidTex[0], 0, GL_TRUE, 0, GL_READ_ONLY,  GL_R8);
    glBindImageTexture(2, mVelTex[0],   0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(groupsFor(res), groupsFor(res), (GLuint)slices);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // --- carry the mask down the pyramid ---
    if (levels > 1)
    {
        gSSWindRestrictProgram.bind();
        for (S32 L = 1; L < levels; ++L)
        {
            const S32 r = levelRes(res, L);
            setGrid(gSSWindRestrictProgram, r, slices);
            glBindImageTexture(6, mSolidTex[L - 1], 0, GL_TRUE, 0, GL_READ_ONLY,  GL_R8);
            glBindImageTexture(7, mSolidTex[L],     0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R8);
            glDispatchCompute(groupsFor(r), groupsFor(r), (GLuint)slices);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }
    }

    // --- solve for pressure, coarsest level first ---
    S32 src = 0;
    for (S32 L = levels - 1; L >= 0; --L)
    {
        const S32 r = levelRes(res, L);
        const GLuint groups = groupsFor(r);

        // The finest level already has its velocity from the seed above; the
        // coarser ones only ever had a mask handed down to them.
        if (L > 0)
        {
            gSSWindSeedProgram.bind();
            setGrid(gSSWindSeedProgram, r, slices);
            setAmbient(gSSWindSeedProgram, tile.mAmbient, slices);
            glBindImageTexture(1, mSolidTex[L], 0, GL_TRUE, 0, GL_READ_ONLY,  GL_R8);
            glBindImageTexture(2, mVelTex[L],   0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glDispatchCompute(groups, groups, (GLuint)slices);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }

        gSSWindDivProgram.bind();
        setGrid(gSSWindDivProgram, r, slices);
        setExtent(gSSWindDivProgram, tile.mExtent);
        setSliceZ(gSSWindDivProgram, tile.mSliceZ, slices);
        setAmbient(gSSWindDivProgram, tile.mAmbient, slices);
        glBindImageTexture(2, mVelTex[L], 0, GL_TRUE, 0, GL_READ_ONLY,  GL_RGBA16F);
        glBindImageTexture(3, mDivTex[L], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R32F);
        glDispatchCompute(groups, groups, (GLuint)slices);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

        // Starting guess. The coarsest level has nothing to inherit and starts
        // from still air; every level above starts from the answer below it,
        // which is the whole point of the pyramid.
        if (L == levels - 1)
        {
            std::vector<F32> zeros((size_t)r * r * slices, 0.f);
            glBindTexture(GL_TEXTURE_3D, mPressureTex[L][0]);
            glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, r, r, slices, GL_RED, GL_FLOAT, zeros.data());
            glBindTexture(GL_TEXTURE_3D, 0);
        }
        else
        {
            gSSWindProlongProgram.bind();
            setGrid(gSSWindProlongProgram, r, slices);
            glBindImageTexture(6, mPressureTex[L + 1][src], 0, GL_TRUE, 0, GL_READ_ONLY,  GL_R32F);
            glBindImageTexture(7, mPressureTex[L][0],       0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R32F);
            glDispatchCompute(groups, groups, (GLuint)slices);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }

        gSSWindJacobiProgram.bind();
        setGrid(gSSWindJacobiProgram, r, slices);
        setExtent(gSSWindJacobiProgram, tile.mExtent);
        setSliceZ(gSSWindJacobiProgram, tile.mSliceZ, slices);
        glBindImageTexture(1, mSolidTex[L], 0, GL_TRUE, 0, GL_READ_ONLY, GL_R8);
        glBindImageTexture(3, mDivTex[L],   0, GL_TRUE, 0, GL_READ_ONLY, GL_R32F);

        src = 0;
        for (S32 i = 0; i < iters; ++i)
        {
            glBindImageTexture(4, mPressureTex[L][src],     0, GL_TRUE, 0, GL_READ_ONLY,  GL_R32F);
            glBindImageTexture(5, mPressureTex[L][1 - src], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R32F);
            glDispatchCompute(groups, groups, (GLuint)slices);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            src = 1 - src;
        }
    }

    // --- subtract the gradient, measure exposure ---
    static LLCachedControl<U32> shelter(gSavedSettings, "SSAtmoWindFlowShelterSteps", 2);
    static LLCachedControl<F32> shelter_amt(gSavedSettings, "SSAtmoWindFlowShelterAmount", 0.4f);
    static LLCachedControl<F32> strength(gSavedSettings, "SSAtmoWindFlowStrength", 2.f);
    static LLCachedControl<F32> max_gain(gSavedSettings, "SSAtmoWindFlowMaxGain", 4.f);

    gSSWindProjectProgram.bind();
    setGrid(gSSWindProjectProgram, res, slices);
    setExtent(gSSWindProjectProgram, tile.mExtent);
    setSliceZ(gSSWindProjectProgram, tile.mSliceZ, slices);
    setAmbient(gSSWindProjectProgram, tile.mAmbient, slices);
    glUniform1i(uniformLoc(gSSWindProjectProgram, "uShelterSteps"), llclamp((S32)shelter, 0, 32));
    glUniform1f(uniformLoc(gSSWindProjectProgram, "uShelterAmount"), llclamp((F32)shelter_amt, 0.f, 1.f));
    glUniform1f(uniformLoc(gSSWindProjectProgram, "uStrength"), llclamp((F32)strength, 0.f, 8.f));
    glUniform1f(uniformLoc(gSSWindProjectProgram, "uMaxGain"), llclamp((F32)max_gain, 0.f, 32.f));
    glBindImageTexture(1, mSolidTex[0],          0, GL_TRUE, 0, GL_READ_ONLY,  GL_R8);
    glBindImageTexture(2, mVelTex[0],            0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
    glBindImageTexture(4, mPressureTex[0][src],  0, GL_TRUE, 0, GL_READ_ONLY,  GL_R32F);
    glDispatchCompute(groupsFor(res), groupsFor(res), (GLuint)slices);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);

    gSSWindProjectProgram.unbind();

    return glGetError() == GL_NO_ERROR;
}

// GL half, main thread. The audio mix and precipitation advection both sample
// on the CPU, so the solved volume has to come back once per build.
//
// glGetTexImage returns the entire level, not the part we dispatched over. The
// volumes are allocated at the maximum slab count and only the first mSlices
// are solved, so the staging buffers have to cover all of them.
void SSWindFlowMap::readback(Tile& tile)
{
    const size_t allocated = (size_t)mTexRes * mTexRes * mTexSlices;

    mVolumeRaw.resize(allocated * 4);
    glBindTexture(GL_TEXTURE_3D, mVelTex[0]);
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_FLOAT, mVolumeRaw.data());
    glBindTexture(GL_TEXTURE_3D, 0);

    // And the mask, so "is this point inside a building" is answerable. A lee
    // and a wall both have almost no velocity in them, and telling them apart
    // from the velocity alone is not possible.
    mSolidRaw.assign(allocated, 0);
    glBindTexture(GL_TEXTURE_3D, mSolidTex[0]);
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RED, GL_UNSIGNED_BYTE, mSolidRaw.data());
    glBindTexture(GL_TEXTURE_3D, 0);
}

// CPU half, worker: unpack the staged volume into the tile. A quarter of a
// million LLVector4 stores at a typical resolution, and nothing about it needs
// the GL context.
void SSWindFlowMap::unpackVolume(Tile& tile)
{
    const size_t active = (size_t)tile.mRes * tile.mRes * tile.mSlices;
    if (mVolumeRaw.size() < active * 4 || mSolidRaw.size() < active) return;

    tile.mFlow.resize(active);
    for (size_t i = 0; i < active; ++i)
    {
        tile.mFlow[i].set(mVolumeRaw[i * 4], mVolumeRaw[i * 4 + 1],
                          mVolumeRaw[i * 4 + 2], mVolumeRaw[i * 4 + 3]);
    }

    tile.mSolid.assign(mSolidRaw.begin(), mSolidRaw.begin() + active);
}

//-----------------------------------------------------------------------------
// Build
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Staged build
//-----------------------------------------------------------------------------
//
// A build used to run start to finish inside one update(). That is five full
// scene renders, three GPU readbacks and several passes over a res*res*slices
// grid, all charged to the same frame - tens of milliseconds, every time a
// region resolves. Regions resolve whenever anything near the camera rezzes,
// so in a busy place it was a hitch every few seconds.
//
// The same work now runs one stage per update(), and the stages that touch no
// GL run on the general work queue. Only ever one build is in flight, so the
// scratch buffers stay single owner and need no locking: while a worker stage
// holds them the main thread does not touch them, and abandoning a build waits
// for the worker to report back before anything is freed.

SSWindFlowMap::Tile* SSWindFlowMap::buildTile()
{
    if (mBuildRegion == 0) return nullptr;
    auto it = mTiles.find(mBuildRegion);
    return (it == mTiles.end()) ? nullptr : &it->second;
}

void SSWindFlowMap::releaseScratch()
{
    // The staging tile is scratch too: it carries a full mFlow and mSolid, and
    // an abandoned build has no reason to keep either
    mBuild = Tile();

    mMaskRaw.clear();
    mMaskRaw.shrink_to_fit();
    mMaskBridged.clear();
    mMaskBridged.shrink_to_fit();
    mVolumeRaw.clear();
    mVolumeRaw.shrink_to_fit();
    mSolidRaw.clear();
    mSolidRaw.shrink_to_fit();
    mMaskChanged = false;
}

void SSWindFlowMap::abandonBuild()
{
    // The generation is what a worker completion checks itself against, so
    // bumping it here is what makes a stale one land harmlessly
    ++mBuildGeneration;
    mStage = EStage::IDLE;
    mBuildRegion = 0;
    mBuildProbe = 0;

    // A worker still holding the scratch frees it when it reports back
    if (!mWorkerBusy) releaseScratch();
}

void SSWindFlowMap::postWorker(std::function<void()> work, EStage next)
{
    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    LL::WorkQueue::ptr_t main = LL::WorkQueue::getInstance("mainloop");

    if (!general || !main)
    {
        // No pool to hand it to: run it here rather than stalling the build
        work();
        mStage = next;
        return;
    }

    mWorkerBusy = true;
    const U32 generation = mBuildGeneration;

    main->postTo(
        general,
        [work]()            // worker
        {
            work();
            return true;
        },
        [this, generation, next](bool)   // back on the main thread
        {
            mWorkerBusy = false;

            // A clear() that arrived mid-stage had to leave the map alone;
            // now that the worker has let go, it can finish
            if (mClearPending)
            {
                mClearPending = false;
                clear();
                return;
            }

            // rebuildAll(), or a region going away, while this was out
            if (generation != mBuildGeneration)
            {
                releaseScratch();
                return;
            }
            mStage = next;
        });
}

bool SSWindFlowMap::beginBuild(Tile& tile)
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;

    S32 res; F32 extent, margin;
    desiredGeometry(regionp, res, extent, margin);

    // Fresh staging tile. The live one is left exactly as it is - still valid,
    // still self-consistent, still being sampled - until COMMIT swaps the
    // finished result in.
    mBuild = Tile();
    mBuild.mRegionHandle = tile.mRegionHandle;
    mBuild.mRes = res;
    mBuild.mExtent = extent;
    mBuild.mMargin = margin;
    mBuild.mOriginRegion.set(-margin, -margin, 0.f);

    chooseBand(mBuild, regionp);

    // Anything that dirties the region from here on is a change this build has
    // not seen, so clear it now and let it re-arm. Clearing it at COMMIT
    // instead would swallow every edit made while the solve was running.
    tile.mDirty = false;

    mBuildRegion = tile.mRegionHandle;
    mBuildProbe = 0;
    mBuildStart = LLTimer::getElapsedSeconds();
    mStage = EStage::CAPTURE_TOP;
    return true;
}

bool SSWindFlowMap::stageCaptureTop(Tile& tile)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WINDFLOW);
    return captureHeights(tile);
}

bool SSWindFlowMap::stageCaptureProbe(Tile& tile, S32 which)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WINDFLOW);
    return captureProbe(tile, which);
}

// Worker. Everything between the last capture and the first dispatch that does
// not need the context: nine million probe rays turned back into altitudes,
// the slab placement that reads them, and the solid fill measurement.
void SSWindFlowMap::stageReduce(Tile& tile)
{
    reconstructHidden(tile);
    placeSlices(tile);

    // Hold on to what the capture saw, and to how much of the volume it filled.
    // A solve with an empty mask looks exactly like a solve with no buildings
    // in the region, and there is otherwise no way to tell the two apart from
    // the outside.
    tile.mSurfaceTop = mTop;
    tile.mSolidFill = 0.f;
    {
        // Before carving: everything under the overhead surface. What the
        // probes then open back up is reported separately as mCarved, so the
        // two numbers together say both how much geometry the capture found
        // and how much of it turned out to be roofed rather than solid.
        const size_t cells = (size_t)tile.mRes * tile.mRes;
        F64 sum = 0.0;
        for (size_t i = 0; i < cells && i < mTop.size(); ++i)
        {
            const F32 top = mTop[i];
            for (S32 k = 0; k < tile.mSlices; ++k)
            {
                const F32 lo = tile.mSliceZ[k];
                const F32 hi = tile.mSliceZ[k + 1];
                const F32 overlap = llmax(0.f, llmin(hi, top) - lo);
                sum += llclamp(overlap / llmax(hi - lo, 0.01f), 0.f, 1.f);
            }
        }
        const F64 total = (F64)cells * (F64)llmax(tile.mSlices, 1);
        if (total > 0.0) tile.mSolidFill = (F32)(sum / total);
    }
}

bool SSWindFlowMap::stageSolveInit(Tile& tile)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WINDFLOW);

    if (!ensureResources(tile.mRes, SS_WIND_MAX_SLICES)) return false;
    return solveInit(tile);
}

// Worker: joining passages up is a pass over the whole volume against three
// axes, the largest CPU cost left in a build
void SSWindFlowMap::stageBridge(Tile& tile)
{
    bridgePassages(tile);
}

bool SSWindFlowMap::stageSolveRun(Tile& tile)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WINDFLOW);
    return solveRun(tile);
}

bool SSWindFlowMap::stageReadback(Tile& tile)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WINDFLOW);
    readback(tile);
    return true;
}

// Worker: unpack the staged volume, and repeat the carve for the debug view if
// that view is the one selected
void SSWindFlowMap::stageConvert(Tile& tile)
{
    unpackVolume(tile);
    buildCarveFlags(tile);
}

void SSWindFlowMap::stageCommit(Tile& live)
{
    mBuild.mBuiltWind = SSAtmoMagic::getInstance()->wind();
    mBuild.mTuning = tuningSignature();
    mBuild.mBuildTime = SSAtmoMagic::getInstance()->sharedTime();
    mBuild.mValid = true;

    // Anything that dirtied the region while the solve was running describes a
    // build this result never saw, so it carries over and asks for another.
    mBuild.mDirty = live.mDirty;
    mBuild.mLastTouched = live.mLastTouched;

    // The one point at which the live tile changes shape. Everything about it
    // - resolution, slab count, slab altitudes, the field itself - moves in a
    // single assignment, so a sampler either gets the whole of the old solve or
    // the whole of the new one and never a mix of the two.
    live = std::move(mBuild);
    mBuild = Tile();

    Tile& tile = live;

    ++mBuildCount;
    mSolveMS = (F32)((LLTimer::getElapsedSeconds() - mBuildStart) * 1000.0);

    auditProbes(tile);

    LL_INFOS("AtmoMagic") << "Wind flow solved region " << tile.mRegionHandle
                          << ": " << tile.mRes << " cells, " << tile.mSlices << " slabs, "
                          << llformat("%.0f%% solid", tile.mSolidFill * 100.f)
                          << llformat(", %.2f%% of probe rays landed below the overhead surface", tile.mCarved * 100.f)
                          << ", probe miss "
                          << llformat("%.0f/%.0f/%.0f/%.0f%%",
                                      mProbeMiss[0] * 100.f, mProbeMiss[1] * 100.f,
                                      mProbeMiss[2] * 100.f, mProbeMiss[3] * 100.f)
                          << ", " << llformat("%.0fms wall", mSolveMS) << LL_ENDL;
}

// One stage per call. Returns true while a build is in flight, so update()
// knows not to look for another tile to start.
bool SSWindFlowMap::advanceBuild()
{
    if (mStage == EStage::IDLE) return false;

    // A worker stage is out; nothing to do here until it reports back
    if (mWorkerBusy) return true;

    // The live tile is only needed to commit into; every stage before that
    // works on the staging tile
    Tile* livep = buildTile();
    if (!livep)
    {
        // The region went away underneath the build
        abandonBuild();
        return false;
    }

    Tile& tile = mBuild;

    auto fail = [&](const char* why)
    {
        // The live tile keeps whatever it had. A failed build produces no
        // result rather than invalidating a solve that was working.
        LL_WARNS("AtmoMagic") << "Wind flow build abandoned at " << why << LL_ENDL;
        abandonBuild();
        return false;
    };

    switch (mStage)
    {
        case EStage::CAPTURE_TOP:
            if (!stageCaptureTop(tile)) return fail("the overhead capture");
            mBuildProbe = 0;
            mStage = EStage::CAPTURE_PROBE;
            return true;

        case EStage::CAPTURE_PROBE:
            if (!stageCaptureProbe(tile, mBuildProbe)) return fail("an oblique probe");
            if (++mBuildProbe < SS_WIND_PROBES) return true;
            postWorker([this, &tile]() { stageReduce(tile); }, EStage::SOLVE_INIT);
            mStage = EStage::REDUCE;
            return true;

        case EStage::REDUCE:
            // Held by the worker; the completion moves us on
            return true;

        case EStage::SOLVE_INIT:
            if (!stageSolveInit(tile))
            {
                // A failed solve is a broken shader, not a broken region, so
                // stop trying rather than spinning on it
                mShaderFailed = true;
                return fail("the mask init pass");
            }
            postWorker([this, &tile]() { stageBridge(tile); }, EStage::SOLVE_RUN);
            mStage = EStage::BRIDGE;
            return true;

        case EStage::BRIDGE:
            return true;

        case EStage::SOLVE_RUN:
            if (!stageSolveRun(tile))
            {
                mShaderFailed = true;
                return fail("the pressure solve");
            }
            mStage = EStage::READBACK;
            return true;

        case EStage::READBACK:
            if (!stageReadback(tile)) return fail("the volume readback");
            postWorker([this, &tile]() { stageConvert(tile); }, EStage::COMMIT);
            mStage = EStage::CONVERT;
            return true;

        case EStage::CONVERT:
            return true;

        case EStage::COMMIT:
            // Everything downstream indexes mFlow by mRes and mSlices, so the
            // three have to agree before this is allowed to become the tile
            // people sample. A short readback publishes as "valid but empty",
            // which reads as dead calm rather than as the failure it is.
            if (mBuild.mFlow.size() != (size_t)mBuild.mRes * mBuild.mRes * mBuild.mSlices)
            {
                return fail("the volume unpack: the field came back the wrong size");
            }
            stageCommit(*livep);
            mStage = EStage::IDLE;
            mBuildRegion = 0;
            releaseScratch();
            return false;

        default:
            abandonBuild();
            return false;
    }
}
void SSWindFlowMap::evict()
{
    // A worker stage holds a reference into the map. Inserting into a std::map
    // leaves existing references alone, but erasing from it does not, so no
    // tile goes anywhere until the build in flight has let go.
    if (mWorkerBusy) return;

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

void SSWindFlowMap::update()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoWindFlow", true);

    if (!enabled || !isSupported() || !SSAtmoMagic::getInstance()->isEnabled())
    {
        if (!mTiles.empty()) clear();
        return;
    }

    if (!ensureShaders()) return;

    // A build in flight gets one stage per frame and nothing else happens.
    // Evicting mid-build would pull the tile out from under it.
    if (advanceBuild()) return;

    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    if (now - mLastBuild < BUILD_MIN_INTERVAL) return;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    // A dirty tile waits before resolving. A region rezzing in fires object
    // updates continuously for tens of seconds, and re-solving on every one of
    // them would be both pointless and expensive; this lets it settle and
    // catches up once things stop moving.
    auto ready = [&](const Tile& tile)
    {
        if (!needsSolve(tile)) return false;
        if (tile.mDirty && tile.mValid && now - tile.mBuildTime < DIRTY_MIN_INTERVAL) return false;
        return true;
    };

    // The camera's region always deserves a tile; neighbours get one once the
    // camera is close enough to their border to be hearing their wind
    LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);
    Tile* best = nullptr;

    if (cam_region)
    {
        Tile* tile = tileFor(cam_region, true);
        tile->mLastTouched = now;
        if (ready(*tile)) best = tile;
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
            if (ready(*tile))
            {
                best = tile;
                break;
            }
        }
    }

    if (best)
    {
        mLastBuild = now;
        // Only sets the build going; the stages run one per frame from here
        beginBuild(*best);
    }

    evict();
}

F64 SSWindFlowMap::age() const
{
    const Tile* tile = cameraTile();
    if (!tile) return 0.0;
    return SSAtmoMagic::getInstance()->sharedTime() - tile->mBuildTime;
}

S32 SSWindFlowMap::sliceCount() const
{
    const Tile* tile = cameraTile();
    return tile ? tile->mSlices : 0;
}

S32 SSWindFlowMap::resolution() const
{
    const Tile* tile = cameraTile();
    return tile ? tile->mRes : 0;
}

F32 SSWindFlowMap::extent() const
{
    const Tile* tile = cameraTile();
    return tile ? tile->mExtent : 0.f;
}

F32 SSWindFlowMap::cellSize() const
{
    const Tile* tile = cameraTile();
    return tile ? tile->mExtent / (F32)llmax(1, tile->mRes) : 0.f;
}

F32 SSWindFlowMap::sliceAltitude(S32 i) const
{
    const Tile* tile = cameraTile();
    if (!tile) return 0.f;
    return tile->mSliceZ[llclamp(i, 0, tile->mSlices)];
}

//-----------------------------------------------------------------------------
// Sampling
//-----------------------------------------------------------------------------

// static
void SSWindFlowMap::sliceAt(const Tile& tile, F32 z, S32& k, F32& frac)
{
    // Blend between slab centres so advection does not step at slab edges
    for (S32 i = 0; i < tile.mSlices; ++i)
    {
        const F32 centre = 0.5f * (tile.mSliceZ[i] + tile.mSliceZ[i + 1]);
        if (z < centre)
        {
            if (i == 0) { k = 0; frac = 0.f; return; }
            const F32 prev = 0.5f * (tile.mSliceZ[i - 1] + tile.mSliceZ[i]);
            k = i - 1;
            frac = llclamp((z - prev) / llmax(centre - prev, 0.01f), 0.f, 1.f);
            return;
        }
    }
    k = tile.mSlices - 1;
    frac = 0.f;
}

bool SSWindFlowMap::surfaceAt(const LLVector3& pos_agent, F32& top) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid || tile->mSurfaceTop.empty()) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const LLVector3 origin = regionp->getOriginAgent() + tile->mOriginRegion;
    const F32 cell = tile->mExtent / (F32)tile->mRes;
    const S32 x = (S32)((pos_agent.mV[VX] - origin.mV[VX]) / cell);
    const S32 y = (S32)((pos_agent.mV[VY] - origin.mV[VY]) / cell);
    if (x < 0 || y < 0 || x >= tile->mRes || y >= tile->mRes) return false;

    top = tile->mSurfaceTop[(size_t)y * tile->mRes + x];

    // Nothing stood in this column at all
    return top > NO_SURFACE * 0.5f;
}

F32 SSWindFlowMap::carvedFraction() const
{
    const Tile* tile = cameraTile();
    return tile ? tile->mCarved : 0.f;
}

F32 SSWindFlowMap::solidFill() const
{
    const Tile* tile = cameraTile();
    return tile ? tile->mSolidFill : 0.f;
}

//-----------------------------------------------------------------------------
// Travelling gusts
//-----------------------------------------------------------------------------

// The solve is deliberately static: it says how a build bends a steady wind,
// and it is far too expensive to re-run at anything like a frame rate. On its
// own though it blows at one unvarying strength forever, everywhere at once,
// and that is the one thing a storm never does.
//
// So the rhythm is layered on at sample time, as frozen turbulence: a noise
// field fixed in the moving air rather than in the world. Standing still, the
// pattern is carried past at the wind's own speed and the wind rises and falls
// as it goes by; watched from above, a surge enters the region on the windward
// side and walks across it, reaching the far edge as much later as the air
// takes to get there. Nothing about it is a clock - it is one field, and where
// you are in it is where the air has carried it to.
//
// The field is anisotropic, long across the wind and short along it, because a
// gust front arrives as a line rather than a blob, and it veers as well as
// surges: a gust that only changed speed reads as the whole world pulsing.
void SSWindFlowMap::gustAt(const LLVector3& pos_agent, F64 time, F32& scale, F32& veer) const
{
    // Depth, spacing and veer are the track's own weather, alongside its
    // turbulence and wind speed; only how fast a front travels relative to the
    // air is a viewer-side tuning knob.
    static LLCachedControl<F32> travel_setting(gSavedSettings, "SSAtmoWindGustTravel", 1.f);

    scale = 1.f;
    veer = 0.f;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    // Already scaled by the track's turbulence: at zero the track is asking for
    // a steady draught and there is nothing to modulate
    const F32 depth = atmo->gustDepth();
    if (depth <= 0.001f) return;

    const LLVector3 wind = atmo->windXY();
    const F32 speed = wind.magVec();
    if (speed < 0.05f) return;

    const LLVector3 dir = wind / speed;

    // Grid coordinates, so a front crosses a region border without a seam.
    // Wrapped far outside anything visible across, on a region multiple so the
    // wrap lands on a border rather than through the middle of a build.
    const LLVector3d global = gAgent.getPosGlobalFromAgent(pos_agent);
    const F32 gx = (F32)fmod(global.mdV[VX], 8192.0);
    const F32 gy = (F32)fmod(global.mdV[VY], 8192.0);

    // Distance along and across the wind, the along axis carried backwards by
    // however far the air has travelled. That subtraction is the whole trick:
    // it is what sweeps the pattern downwind at exactly wind speed.
    const F64 drift = atmo->windDrift() * (F64)llclamp((F32)travel_setting, 0.f, 4.f);
    const F64 along = (F64)(gx * dir.mV[VX] + gy * dir.mV[VY]) - drift;
    const F32 across = -gx * dir.mV[VY] + gy * dir.mV[VX];

    const F32 wavelength = atmo->gustLength();
    const U32 seed = atmo->seed();

    // Frozen turbulence is only ever an approximation: a real eddy changes as
    // it travels, so a pattern that never did would repeat itself out of a
    // steady wind. A slow crosswind slide on the shared clock decorrelates it
    // over a couple of minutes without disturbing the downwind travel.
    const F32 evolve = (F32)fmod(time, 4096.0) * 0.006f;

    // The surge itself, and a shorter channel riding on it so a wave has some
    // texture rather than passing as one clean swell
    const F32 su = (F32)(along / (F64)wavelength);
    const F32 sv = across / (wavelength * 2.4f) + evolve;
    const F32 wave = SSAtmoNoise::fbm2(su, sv, seed ^ 0x57055EE1u, 2);

    const F32 ru = (F32)(along / (F64)(wavelength * 0.27f));
    const F32 rv = across / (wavelength * 0.65f) + evolve * 3.f;
    const F32 ripple = SSAtmoNoise::value2(ru, rv, seed ^ 0x9F17E2B3u);

    // Leaned toward the peaks: a windy day spends longer in the lulls between
    // gusts than it does inside them, and a symmetric field reads as breathing
    F32 g = 0.8f * wave + 0.3f * ripple;
    g = (g > 0.f) ? g * 1.35f : g * 0.8f;

    scale = llclamp(1.f + depth * g, 0.1f, 3.f);

    // Backing and veering a few degrees either side of the mean as the front
    // goes through. Slower and broader than the speed channel, so the air
    // swings once per gust instead of jittering.
    const F32 swing = SSAtmoNoise::value2(su * 0.7f, sv * 0.7f, seed ^ 0x2C0FFE55u);
    veer = swing * depth * atmo->gustVeer();
}

F32 SSWindFlowMap::gust(const LLVector3& pos_agent) const
{
    F32 scale, veer;
    gustAt(pos_agent, SSAtmoMagic::getInstance()->sharedTime(), scale, veer);
    return scale;
}

// Turn a horizontal wind by an angle, leaving any vertical component alone:
// air going over a roof still goes over it, gust or no gust
static LLVector3 veerWind(const LLVector3& v, F32 angle)
{
    if (fabsf(angle) < 0.001f) return v;
    const F32 c = cosf(angle), s = sinf(angle);
    return LLVector3(v.mV[VX] * c - v.mV[VY] * s,
                     v.mV[VX] * s + v.mV[VY] * c,
                     v.mV[VZ]);
}

LLVector3 SSWindFlowMap::sample(const LLVector3& pos_agent) const
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    // The gust rides on whatever the wind at this point turns out to be, so
    // the ambient fallback surges along with the solved field rather than the
    // two disagreeing either side of a tile border
    F32 gust_scale, gust_veer;
    gustAt(pos_agent, atmo->sharedTime(), gust_scale, gust_veer);
    auto gusted = [&](const LLVector3& v, F32 shelter)
    {
        // A courtyard the wind barely reaches does not get the gust either: it
        // feels the surges as a distant swell rather than as the front itself
        const F32 felt = 1.f + (gust_scale - 1.f) * shelter;
        return veerWind(v * felt, gust_veer * shelter);
    };

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return gusted(atmo->wind(), 1.f);

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end() || !it->second.mValid || it->second.mFlow.empty())
    {
        return gusted(atmo->wind(), 1.f);
    }

    const Tile& tile = it->second;
    const LLVector3 origin = regionp->getOriginAgent() + tile.mOriginRegion;
    const F32 cell = tile.mExtent / (F32)tile.mRes;

    const F32 fx = (pos_agent.mV[VX] - origin.mV[VX]) / cell - 0.5f;
    const F32 fy = (pos_agent.mV[VY] - origin.mV[VY]) / cell - 0.5f;

    if (fx < 0.f || fy < 0.f || fx >= (F32)(tile.mRes - 1) || fy >= (F32)(tile.mRes - 1))
    {
        return gusted(atmo->wind(), 1.f);   // outside the domain, undisturbed
    }

    const S32 x0 = (S32)fx, y0 = (S32)fy;
    const F32 tx = fx - (F32)x0, ty = fy - (F32)y0;

    S32 k; F32 tz;
    sliceAt(tile, pos_agent.mV[VZ], k, tz);
    const S32 k1 = llmin(k + 1, tile.mSlices - 1);

    // Velocity in xyz and exposure in w interpolated together: the gust wants
    // the exposure at the same point, and it is already sitting in the cell
    auto bilinear = [&](S32 slab)
    {
        const LLVector4& a = tile.mFlow[index(tile, x0,     y0,     slab)];
        const LLVector4& b = tile.mFlow[index(tile, x0 + 1, y0,     slab)];
        const LLVector4& c = tile.mFlow[index(tile, x0,     y0 + 1, slab)];
        const LLVector4& d = tile.mFlow[index(tile, x0 + 1, y0 + 1, slab)];

        const LLVector4 top = a * (1.f - tx) + b * tx;
        const LLVector4 bot = c * (1.f - tx) + d * tx;
        return top * (1.f - ty) + bot * ty;
    };

    const LLVector4 cell_v = bilinear(k) * (1.f - tz) + bilinear(k1) * tz;

    return gusted(LLVector3(cell_v.mV[0], cell_v.mV[1], cell_v.mV[2]),
                  llclamp(cell_v.mV[3], 0.f, 1.f));
}

F32 SSWindFlowMap::exposure(const LLVector3& pos_agent) const
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return 1.f;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end() || !it->second.mValid || it->second.mFlow.empty()) return 1.f;

    const Tile& tile = it->second;
    const LLVector3 origin = regionp->getOriginAgent() + tile.mOriginRegion;
    const F32 cell = tile.mExtent / (F32)tile.mRes;

    const S32 x = (S32)((pos_agent.mV[VX] - origin.mV[VX]) / cell);
    const S32 y = (S32)((pos_agent.mV[VY] - origin.mV[VY]) / cell);
    if (x < 0 || y < 0 || x >= tile.mRes || y >= tile.mRes) return 1.f;

    S32 k; F32 tz;
    sliceAt(tile, pos_agent.mV[VZ], k, tz);
    const S32 k1 = llmin(k + 1, tile.mSlices - 1);

    const F32 a = tile.mFlow[index(tile, x, y, k)].mV[3];
    const F32 b = tile.mFlow[index(tile, x, y, k1)].mV[3];
    return a * (1.f - tz) + b * tz;
}

//-----------------------------------------------------------------------------
// Debug visualisation
//-----------------------------------------------------------------------------

// Flow direction as hue, so the eye can read a whole slab at a glance:
// opposing directions come out as opposing colours.
static LLColor4 flowColor(const LLVector3& v, F32 exposure, F32 alpha)
{
    const F32 len = v.magVec();
    if (len < 0.01f) return LLColor4(0.2f, 0.2f, 0.25f, alpha * 0.35f);

    const F32 angle = atan2f(v.mV[VY], v.mV[VX]);
    const F32 h = (angle + F_PI) / F_TWO_PI * 6.f;
    const S32 sector = llclamp((S32)h, 0, 5);
    const F32 f = h - (F32)sector;

    F32 r = 0.f, g = 0.f, b = 0.f;
    switch (sector)
    {
        case 0: r = 1.f;     g = f;       b = 0.f;     break;
        case 1: r = 1.f - f; g = 1.f;     b = 0.f;     break;
        case 2: r = 0.f;     g = 1.f;     b = f;       break;
        case 3: r = 0.f;     g = 1.f - f; b = 1.f;     break;
        case 4: r = f;       g = 0.f;     b = 1.f;     break;
        default: r = 1.f;    g = 0.f;     b = 1.f - f; break;
    }

    // Exposure drives brightness: sheltered cells go dark, gaps blow out
    const F32 v_scale = llclamp(0.25f + exposure * 0.75f, 0.f, 1.5f);
    return LLColor4(r * v_scale, g * v_scale, b * v_scale, alpha);
}

// Draw what a capture pass actually saw, as the world points it saw them at.
// A depth map shown flat says whether it has holes; shown in place it says
// which part of the region fell into them, which is the question that matters.
void SSWindFlowMap::renderDebugCapture(S32 which)
{
    if (mCaptureRes <= 0) return;

    const S32 res = mCaptureRes;
    const F32 cell = mCaptureCell;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    static LLCachedControl<F32> range_setting(gSavedSettings, "SSAtmoWindFlowDebugRange", 24.f);
    const F32 full = llclamp((F32)range_setting, 16.f, 4096.f);

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // A cross rather than a point: points do not scale with distance and a
    // dense capture turns into a solid wall of them.
    auto mark = [&](const LLVector3& p, const LLColor4& c, F32 size)
    {
        gGL.color4fv(c.mV);
        gGL.vertex3f(p.mV[VX] - size, p.mV[VY], p.mV[VZ]);
        gGL.vertex3f(p.mV[VX] + size, p.mV[VY], p.mV[VZ]);
        gGL.vertex3f(p.mV[VX], p.mV[VY] - size, p.mV[VZ]);
        gGL.vertex3f(p.mV[VX], p.mV[VY] + size, p.mV[VZ]);
    };

    gGL.begin(LLRender::LINES);

    if (which == 0)
    {
        // Overhead pass. Green where it found a surface; a red mark down at the
        // band floor where the column came back empty, so holes are visible
        // rather than merely absent.
        for (S32 y = 0; y < res; ++y)
        {
            for (S32 x = 0; x < res; ++x)
            {
                const F32 cx = mCaptureOrigin.mV[VX] + ((F32)x + 0.5f) * cell;
                const F32 cy = mCaptureOrigin.mV[VY] + ((F32)y + 0.5f) * cell;

                const F32 away = llmax(fabsf(cx - cam.mV[VX]), fabsf(cy - cam.mV[VY]));
                const S32 step = (away < full) ? 1 : (away < full * 2.f) ? 2 : 4;
                if ((x % step) || (y % step)) continue;

                const F32 h = mTop[(size_t)y * res + x];
                if (h > NO_SURFACE * 0.5f)
                {
                    mark(LLVector3(cx, cy, h), LLColor4(0.3f, 1.f, 0.4f, 0.8f), cell * 0.4f);
                }
            }
        }
    }
    else if (which == CAPTURE_VIEW_CANDIDATES - 1)
    {
        if (mCarveFlags.empty())
        {
            gGL.end();
            return;
        }

        // Blocked cells are left dim: they are the majority and they are the
        // ones behaving. What matters is the difference between a cell a probe
        // opened and one no probe could speak for, because only the second
        // kind is a passage the capture is failing to find.
        const Tile* tile = nullptr;
        auto it = mTiles.find(mCaptureRegion);
        if (it != mTiles.end()) tile = &it->second;
        if (!tile)
        {
            gGL.end();
            return;
        }

        for (S32 k = 0; k < tile->mSlices; ++k)
        {
            const F32 lo = tile->mSliceZ[k];
            const F32 hi = tile->mSliceZ[k + 1];

            for (S32 y = 0; y < res; ++y)
            {
                for (S32 x = 0; x < res; ++x)
                {
                    const size_t idx = index(*tile, x, y, k);
                    if (idx >= mCarveFlags.size()) continue;

                    const U8 flag = mCarveFlags[idx];
                    if (flag == CARVE_AIR) continue;

                    // Drawn where the cell was tested, which is the middle of
                    // the part of the slab that is actually under the surface -
                    // not the middle of the slab. A roof caught low in a thick
                    // top slab is otherwise drawn tens of metres above itself,
                    // and reads as a phantom layer floating over the rooftops.
                    const F32 top = mTop[(size_t)y * res + x];
                    const F32 z = 0.5f * (lo + llmin(hi, top));

                    const F32 cx = mCaptureOrigin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const F32 cy = mCaptureOrigin.mV[VY] + ((F32)y + 0.5f) * cell;

                    const F32 away = llmax(fabsf(cx - cam.mV[VX]), fabsf(cy - cam.mV[VY]));
                    if (away > full * 4.f) continue;
                    const S32 step = (away < full) ? 1 : (away < full * 2.f) ? 2 : 4;
                    if ((x % step) || (y % step)) continue;

                    LLColor4 c;
                    switch (flag)
                    {
                        case CARVE_OPENED:      c.set(0.3f, 1.f, 0.4f, 0.9f);   break;
                        case CARVE_NO_EVIDENCE: c.set(1.f, 0.75f, 0.1f, 0.9f);  break;
                        default:                c.set(0.5f, 0.15f, 0.15f, 0.25f); break;
                    }

                    mark(LLVector3(cx, cy, z), c, cell * 0.4f);
                }
            }
        }
    }
    else
    {
        const S32 i = llclamp(which - 1, 0, SS_WIND_PROBES - 1);
        if (mProbeDepth[i].empty())
        {
            gGL.end();
            return;
        }

        const ProbeFrame& f = mProbeFrame[i];

        // Each probe gets its own hue so several can be told apart when the
        // view is cycled, and so a direction that is missing whole swathes of
        // the region stands out against the ones that are not.
        static const LLColor4 hues[SS_WIND_PROBES] = {
            LLColor4(1.f, 0.5f, 0.2f, 0.8f),
            LLColor4(0.3f, 0.8f, 1.f, 0.8f),
            LLColor4(1.f, 0.9f, 0.3f, 0.8f),
            LLColor4(0.9f, 0.4f, 1.f, 0.8f)
        };

        for (S32 y = 0; y < mProbeRes; ++y)
        {
            for (S32 x = 0; x < mProbeRes; ++x)
            {
                const F32 d = mProbeDepth[i][(size_t)y * mProbeRes + x];
                if (d >= PROBE_MISS * 0.5f) continue;   // nothing to draw for a miss

                const F32 u = ((F32)x + 0.5f) / (F32)mProbeRes * 2.f - 1.f;
                const F32 v = ((F32)y + 0.5f) / (F32)mProbeRes * 2.f - 1.f;
                const LLVector3 hit = f.mEye + f.mRight * (u * mProbeHalf[i])
                                             + f.mUp * (v * mProbeHalf[i]) + f.mDir * d;

                const F32 away = llmax(fabsf(hit.mV[VX] - cam.mV[VX]),
                                       fabsf(hit.mV[VY] - cam.mV[VY]));
                if (away > full * 4.f) continue;

                const S32 step = (away < full) ? 1 : (away < full * 2.f) ? 2 : 4;
                if ((x % step) || (y % step)) continue;

                mark(hit, hues[i], cell * 0.4f);
            }
        }
    }

    gGL.end();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

F32 SSWindFlowMap::slabAlpha(const Tile& tile, S32 k, F32 cam_z) const
{
    static LLCachedControl<F32> fade(gSavedSettings, "SSAtmoWindFlowDebugFade", 0.5f);
    const F32 falloff = llclamp((F32)fade, 0.05f, 1.f);

    // Measured in slabs rather than metres, so the slab you are standing in is
    // always the solid one however unevenly the adaptive slicing placed them.
    S32 k_cam = 0;
    F32 frac = 0.f;
    sliceAt(tile, cam_z, k_cam, frac);

    // Compounding per slab rather than ramping across the band. A linear ramp
    // spreads the difference evenly and leaves every slab looking much like its
    // neighbours, which is unreadable once a dozen of them are drawn at once;
    // halving each step apart separates them at a glance and buries the distant
    // ones quickly, which is the whole point of drawing them at all.
    const F32 alpha = powf(falloff, (F32)llabs(k - k_cam));

    // Never quite nothing: a slab that has faded out entirely cannot be told
    // from one the solve never produced
    return llmax(0.02f, alpha);
}

// One arrow per cell says what the field is at a point. A flow line says where
// the air actually goes, which is the question being asked of an alley or a
// gate. Lines do not branch - the field has one value per point - but seeded
// densely they read as branching, because neighbouring lines diverge around an
// obstacle and converge again through a gap.
// Speed as colour temperature, measured against the ambient wind rather than in
// m/s so it reads the same in a breeze and a gale. Cold is sheltered, neutral
// is undisturbed, hot is a jet. Direction is left to the line's own path and
// its downstream brightening, which a single arrow cannot lean on and a
// continuous line can - that is what frees the hue to carry something else.
static LLColor4 speedColor(F32 ratio, F32 alpha)
{
    F32 r, g, b;

    if (ratio < 1.f)
    {
        // Sheltered: deep blue up to a pale neutral at ambient
        const F32 t = llclamp(ratio, 0.f, 1.f);
        r = lerp(0.10f, 0.85f, t);
        g = lerp(0.30f, 0.92f, t);
        b = lerp(0.85f, 1.00f, t);
    }
    else
    {
        // Accelerated: neutral through amber into red. Two ambients is already
        // a strong jet, so the ramp is spent by then rather than crawling on.
        const F32 t = llclamp((ratio - 1.f), 0.f, 1.f);
        r = 1.f;
        g = lerp(0.92f, 0.25f, t);
        b = lerp(1.00f, 0.10f, t);
    }

    return LLColor4(r, g, b, alpha);
}

void SSWindFlowMap::renderDebugStreamlines()
{
    static LLCachedControl<F32> range_setting(gSavedSettings, "SSAtmoWindFlowDebugRange", 24.f);
    static LLCachedControl<U32> seed_setting(gSavedSettings, "SSAtmoWindFlowDebugSeed", 6);
    static LLCachedControl<U32> length_setting(gSavedSettings, "SSAtmoWindFlowDebugLength", 96);
    static LLCachedControl<F32> aspect_setting(gSavedSettings, "SSAtmoWindFlowDebugAspect", 3.f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 full = llclamp((F32)range_setting, 16.f, 4096.f);
    const S32 seed_step = llclamp((S32)seed_setting, 2, 32);
    const S32 max_steps = llclamp((S32)length_setting, 8, 512);
    const F32 aspect = llclamp((F32)aspect_setting, 1.f, 12.f);

    static LLCachedControl<U32> tint_setting(gSavedSettings, "SSAtmoWindFlowDebugLineTint", 1);
    const bool tint_by_speed = (tint_setting != 0);

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Width carries speed as well as colour, which is what makes a jet findable
    // when the whole volume is on screen at once. Line width is global state and
    // changing it mid-stream forces a flush, so segments are collected by width
    // and drawn in one pass each rather than in the order they were traced.
    struct Vert { LLVector3 mPos; LLColor4 mColor; };
    std::vector<Vert> buckets[SS_WIND_LINE_WIDTHS];

    for (const auto& entry : mTiles)
    {
        const Tile& tile = entry.second;
        if (!tile.mValid || tile.mFlow.empty()) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent() + tile.mOriginRegion;
        const F32 cell = tile.mExtent / (F32)tile.mRes;
        const F32 step_len = cell * 0.75f;

        // Whether a point is inside geometry. Stopping on low speed alone walks
        // lines straight into buildings: the interpolation across a wall face
        // blends open air with solid and leaves just enough velocity to keep
        // stepping, and a partly filled slab has a real velocity throughout.
        auto solidAt = [&](const LLVector3& q) -> F32
        {
            if (tile.mSolid.empty()) return 0.f;

            const S32 x = (S32)((q.mV[VX] - origin.mV[VX]) / cell);
            const S32 y = (S32)((q.mV[VY] - origin.mV[VY]) / cell);
            if (x < 0 || y < 0 || x >= tile.mRes || y >= tile.mRes) return 0.f;

            S32 kk = 0;
            F32 frac = 0.f;
            sliceAt(tile, q.mV[VZ], kk, frac);

            const size_t idx = index(tile, x, y, llclamp(kk, 0, tile.mSlices - 1));
            return (idx < tile.mSolid.size()) ? (F32)tile.mSolid[idx] / 255.f : 0.f;
        };

        for (S32 k = 0; k < tile.mSlices; ++k)
        {
            const F32 z = 0.5f * (tile.mSliceZ[k] + tile.mSliceZ[k + 1]);
            const F32 ref = llmax(0.1f, tile.mAmbient[k].magVec());
            const F32 slab_alpha = slabAlpha(tile, k, cam.mV[VZ]);
            if (slab_alpha <= 0.02f) continue;

            // Seed upwind, in a footprint stretched along this slab's wind.
            //
            // A line is only interesting once it has been somewhere: seeded
            // beside the camera it is a short stub that has met nothing, and
            // most of the budget goes on lines coming in from the sides that
            // never reach anything you are looking at. Starting them well
            // upwind means they arrive already shaped by whatever they have
            // passed through, which is the thing worth seeing.
            LLVector3 along_dir(tile.mAmbient[k].mV[VX], tile.mAmbient[k].mV[VY], 0.f);
            if (along_dir.magVecSquared() < 0.0001f)
            {
                along_dir.set(0.f, 1.f, 0.f);   // dead calm: any axis will do
            }
            along_dir.normVec();
            const LLVector3 across_dir(-along_dir.mV[VY], along_dir.mV[VX], 0.f);

            const F32 upwind_reach = full * aspect;
            const F32 downwind_reach = full * 0.5f;
            const F32 side_reach = full;

            for (S32 sy = 0; sy < tile.mRes; sy += seed_step)
            {
                const F32 cy = origin.mV[VY] + ((F32)sy + 0.5f) * cell;

                for (S32 sx = 0; sx < tile.mRes; sx += seed_step)
                {
                    const F32 cx = origin.mV[VX] + ((F32)sx + 0.5f) * cell;

                    const LLVector3 offset(cx - cam.mV[VX], cy - cam.mV[VY], 0.f);
                    const F32 along = offset * along_dir;
                    const F32 across = offset * across_dir;

                    if (fabsf(across) > side_reach) continue;
                    if (along > downwind_reach || along < -upwind_reach) continue;

                    LLVector3 p(cx, cy, z);
                    if (solidAt(p) > 0.5f) continue;    // seeded inside geometry

                    for (S32 n = 0; n < max_steps; ++n)
                    {
                        const LLVector3 v = sample(p);
                        const F32 speed = v.magVec();
                        if (speed < 0.05f) break;       // dead air

                        // Midpoint step. Following the field straight from the
                        // start of a step cuts every corner it is meant to be
                        // showing, and the lines drift through wall corners.
                        const LLVector3 dir = v * (1.f / speed);
                        const LLVector3 mid = p + dir * (step_len * 0.5f);
                        LLVector3 mv = sample(mid);
                        const F32 mspeed = mv.magVec();
                        const LLVector3 mdir = (mspeed > 0.01f) ? mv * (1.f / mspeed) : dir;

                        const LLVector3 next = p + mdir * step_len;
                        if (solidAt(next) > 0.5f) break;    // ran into geometry

                        // Brightening downstream, so which way the air is
                        // travelling is legible without drawing a head on it
                        const F32 t0 = (F32)n / (F32)max_steps;
                        const F32 t1 = (F32)(n + 1) / (F32)max_steps;

                        const F32 ratio = speed / ref;
                        const F32 a0 = slab_alpha * (0.15f + 0.85f * t0);
                        const F32 a1 = slab_alpha * (0.15f + 0.85f * t1);

                        const LLColor4 c0 = tint_by_speed ? speedColor(ratio, a0)
                                                          : flowColor(v, ratio, a0);
                        const LLColor4 c1 = tint_by_speed ? speedColor(ratio, a1)
                                                          : flowColor(v, ratio, a1);

                        // Thin where the air is slack, thick through a jet.
                        // Banded around ambient rather than proportional, so the
                        // interesting half of the range is not spent below 1.
                        const S32 w = (ratio < 0.6f) ? 0
                                    : (ratio < 1.05f) ? 1
                                    : (ratio < 1.5f) ? 2 : 3;
                        buckets[w].push_back({ p, c0 });
                        buckets[w].push_back({ next, c1 });

                        p = next;

                        // Leaving the domain: the field outside is the plain
                        // ambient wind and tracing it says nothing
                        if (p.mV[VX] < origin.mV[VX] || p.mV[VY] < origin.mV[VY]
                            || p.mV[VX] > origin.mV[VX] + tile.mExtent
                            || p.mV[VY] > origin.mV[VY] + tile.mExtent
                            || p.mV[VZ] < tile.mSliceZ[0]
                            || p.mV[VZ] > tile.mSliceZ[tile.mSlices])
                        {
                            break;
                        }
                    }
                }
            }
        }
    }

    // Clamped to the driver maximum by setLineWidth, which in a core profile
    // can be as low as 1 - in which case the tint carries speed on its own.
    static const F32 WIDTHS[SS_WIND_LINE_WIDTHS] = { 1.f, 3.f, 6.f, 10.f };

    for (S32 w = 0; w < SS_WIND_LINE_WIDTHS; ++w)
    {
        if (buckets[w].empty()) continue;

        gGL.setLineWidth(WIDTHS[w]);
        gGL.begin(LLRender::LINES);
        for (const Vert& vert : buckets[w])
        {
            gGL.color4fv(vert.mColor.mV);
            gGL.vertex3fv(vert.mPos.mV);
        }
        gGL.end();
    }

    gGL.setLineWidth(1.f);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

void SSWindFlowMap::renderDebug()
{
    // Capture views replace the flow field rather than overlaying it: the
    // arrows are dense enough that anything drawn among them is unreadable.
    static LLCachedControl<U32> capture_view(gSavedSettings, "SSAtmoWindFlowDebugCapture", 0);
    if (capture_view > 0)
    {
        renderDebugCapture(llclamp((S32)capture_view - 1, 0, CAPTURE_VIEW_CANDIDATES - 1));
        return;
    }

    if (mTiles.empty()) return;

    static LLCachedControl<U32> style(gSavedSettings, "SSAtmoWindFlowDebugStyle", 0);
    if (style == 1)
    {
        renderDebugStreamlines();
        return;
    }

    // One arrow per solved cell. Nothing here is interpolated or scaled up: an
    // arrow sits at a cell centre and reads exactly one texel, so what is drawn
    // is the field the solve produced.
    static LLCachedControl<F32> range_setting(gSavedSettings, "SSAtmoWindFlowDebugRange", 24.f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 full = llclamp((F32)range_setting, 16.f, 4096.f);

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);     // test, but do not write
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    for (const auto& entry : mTiles)
    {
        const Tile& tile = entry.second;
        if (!tile.mValid || tile.mFlow.empty()) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent() + tile.mOriginRegion;
        const F32 cell = tile.mExtent / (F32)tile.mRes;

        gGL.begin(LLRender::LINES);

        for (S32 k = 0; k < tile.mSlices; ++k)
        {
            const F32 z = 0.5f * (tile.mSliceZ[k] + tile.mSliceZ[k + 1]);

            // Reference speed from *this slab's* ambient wind, not the tile's
            // strongest. The gradient makes an upper slab several times windier
            // than the street, so measuring street air against the rooftop wind
            // puts every reading near the bottom of the scale and no local
            // acceleration can ever show.
            const F32 ref = llmax(0.1f, tile.mAmbient[k].magVec());
            const F32 slab_alpha = slabAlpha(tile, k, cam.mV[VZ]);
            if (slab_alpha < 0.02f) continue;

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 cy = origin.mV[VY] + ((F32)y + 0.5f) * cell;

                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 cx = origin.mV[VX] + ((F32)x + 0.5f) * cell;

                    // Texel density where you are standing, decimating outward
                    // so a whole neighbourhood of tiles stays affordable
                    const F32 away = llmax(fabsf(cx - cam.mV[VX]), fabsf(cy - cam.mV[VY]));
                    const S32 step = (away < full) ? 1 : (away < full * 2.f) ? 2 : 4;
                    if ((x % step) || (y % step)) continue;

                    const LLVector4& f = tile.mFlow[index(tile, x, y, k)];
                    LLVector3 v(f.mV[0], f.mV[1], f.mV[2]);

                    const F32 speed = v.magVec();
                    if (speed < 0.02f) continue;    // solid cell, or dead air
                    v *= 1.f / speed;

                    // Length carries speed against the ambient reference, so a
                    // venturi through an alley overruns its cell and a lee
                    // pocket shrinks to a stub
                    const F32 len = cell * (F32)step * 0.9f
                                  * llclamp(speed / ref, 0.08f, 1.6f);

                    const LLVector3 centre(cx, cy, z);
                    const LLVector3 tail = centre - v * (len * 0.5f);
                    const LLVector3 head = centre + v * (len * 0.5f);

                    // Dark tail to bright head: direction is legible from the
                    // gradient alone, which is what lets the barbs go away and
                    // the density go up by a factor of sixteen
                    LLColor4 tip = flowColor(v * speed, f.mV[3], 0.9f * slab_alpha);
                    gGL.color4f(tip.mV[0] * 0.1f, tip.mV[1] * 0.1f, tip.mV[2] * 0.1f,
                                0.35f * slab_alpha);
                    gGL.vertex3fv(tail.mV);
                    gGL.color4fv(tip.mV);
                    gGL.vertex3fv(head.mV);
                }
            }
        }

        // Domain footprint: which region the tile belongs to, and how far its
        // margin reaches into the neighbours it overlaps
        const F32 x0 = origin.mV[VX];
        const F32 y0 = origin.mV[VY];
        const F32 x1 = x0 + tile.mExtent;
        const F32 y1 = y0 + tile.mExtent;
        const F32 z0 = tile.mSliceZ[0];
        const F32 z1 = tile.mSliceZ[tile.mSlices];

        gGL.color4f(0.5f, 0.55f, 0.7f, 0.5f);
        const F32 bx[4] = { x0, x1, x1, x0 };
        const F32 by[4] = { y0, y0, y1, y1 };
        for (S32 i = 0; i < 4; ++i)
        {
            const S32 j = (i + 1) % 4;
            gGL.vertex3f(bx[i], by[i], z0); gGL.vertex3f(bx[j], by[j], z0);
            gGL.vertex3f(bx[i], by[i], z1); gGL.vertex3f(bx[j], by[j], z1);
            gGL.vertex3f(bx[i], by[i], z0); gGL.vertex3f(bx[i], by[i], z1);
        }

        gGL.end();
    }

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

// </SS:Nexii>
