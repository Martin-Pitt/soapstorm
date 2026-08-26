/**
 * @file sswindflow.h
 * @brief Atmo Magic: per-region wind flowmap.
 *
 *        A top-down and a bottom-up ortho depth capture give the solid span of
 *        every column. Those are sliced into a handful of adaptive horizontal
 *        slabs, and a divergence-free field is solved over the result on the
 *        GPU, so air accelerates through alleys, piles up against windward
 *        faces, lifts over rooftops and goes calm in courtyards.
 *
 *        Anchored to the region and solved once, the way the rain shadow maps
 *        are: a static flowmap of the build, not a running simulation. It
 *        rebuilds when the build changes underneath it, when the ambient wind
 *        changes, or when the camera moves to a different sky track.
 *
 *        Consumed by the ambient audio mix and by precipitation advection.
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

#ifndef SS_WINDFLOW_H
#define SS_WINDFLOW_H

#include "llrendertarget.h"
#include "llsingleton.h"
#include "v3math.h"
#include "v4math.h"

#include <glm/mat4x4.hpp>

#include <functional>
#include <map>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

const S32 SS_WIND_MAX_SLICES = 16;
const S32 SS_WIND_MIN_SLICES = 2;

const S32 SS_WIND_MAX_LEVELS = 5;
const S32 SS_WIND_MIN_LEVEL_RES = 24;

const S32 SS_WIND_PROBES = 4;

class SSWindFlowMap : public LLSingleton<SSWindFlowMap>
{
    LLSINGLETON_EMPTY_CTOR(SSWindFlowMap);

public:
    static bool isSupported();

    void update();

    static void markDirty(const LLVector3& pos_agent, F32 radius);

    void clear();

    void rebuildAll();

    bool isValid() const;

    static bool drivesWind();

    LLVector3 sample(const LLVector3& pos_agent) const;

    F32 exposure(const LLVector3& pos_agent) const;

    void gustAt(const LLVector3& pos_agent, F64 time, F32& scale, F32& veer) const;
    F32 gust(const LLVector3& pos_agent) const;

    void renderDebug();

    U64 capturedRegion() const { return mCaptureRegion; }

    S32 sliceCount() const;
    S32 resolution() const;
    F32 extent() const;
    F32 cellSize() const;
    F32 sliceAltitude(S32 i) const;
    F32 lastSolveMS() const { return mSolveMS; }

    bool surfaceAt(const LLVector3& pos_agent, F32& top) const;

    S32 forEachColumn(const LLVector3& center_agent, F32 radius_m,
                      const std::function<void(const LLVector3& pos_agent, F32 top)>& fn) const;
    F32 carvedFraction() const;
    F32 solidFill() const;
    F64 age() const;
    U32 buildCount() const { return mBuildCount; }
    S32 tileCount() const { return (S32)mTiles.size(); }

private:
    struct Tile
    {
        U64 mRegionHandle = 0;

        S32 mRes = 0;
        S32 mSlices = 0;
        F32 mExtent = 0.f;
        F32 mMargin = 0.f;
        LLVector3 mOriginRegion;

        F32 mSliceZ[SS_WIND_MAX_SLICES + 1] = { 0.f };
        LLVector3 mAmbient[SS_WIND_MAX_SLICES];

        F32 mGroundRef = 0.f;
        F32 mBandTop = 0.f;
        F32 mBandBottom = 0.f;
        S32 mTrack = 0;

        std::vector<LLVector4> mFlow;

        std::vector<U8> mSolid;

        std::vector<F32> mSurfaceTop;

        F32 mCarved = 0.f;
        F32 mSolidFill = 0.f;

        LLVector3 mBuiltWind;
        U32 mTuning = 0;
        F64 mBuildTime = 0.0;
        bool mDirty = false;
        bool mValid = false;
        F64 mLastTouched = 0.0;
    };

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    const Tile* tileAt(const LLVector3& pos_agent) const;
    const Tile* cameraTile() const;

    bool needsSolve(const Tile& tile) const;
    void evict();

    enum class EStage
    {
        IDLE,
        CAPTURE_TOP,
        CAPTURE_PROBE,
        REDUCE,
        SOLVE_INIT,
        BRIDGE,
        SOLVE_RUN,
        READBACK,
        CONVERT,
        COMMIT
    };

    bool advanceBuild();
    bool beginBuild(Tile& tile);

    void abandonBuild();
    void releaseScratch();

    void postWorker(std::function<void()> work, EStage next);

    Tile* buildTile();

    bool stageCaptureTop(Tile& tile);
    bool stageCaptureProbe(Tile& tile, S32 which);
    void stageReduce(Tile& tile);
    bool stageSolveInit(Tile& tile);
    void stageBridge(Tile& tile);
    bool stageSolveRun(Tile& tile);
    bool stageReadback(Tile& tile);
    void stageConvert(Tile& tile);
    void stageCommit(Tile& tile);

    bool ensureResources(S32 res, S32 slices);
    void releaseResources();
    bool ensureShaders();

    void chooseBand(Tile& tile, LLViewerRegion* regionp);

    bool captureHeights(Tile& tile);

    bool captureAlong(LLRenderTarget& target, S32 res, const Tile& tile,
                      const LLVector3& dir, const LLVector3& eye,
                      F32 half, F32 range, std::vector<F32>& out, glm::mat4& view_out);

    void beginProbes(Tile& tile);
    bool captureProbe(Tile& tile, S32 which);
    void reconstructHidden(Tile& tile);

    void auditProbes(const Tile& tile) const;

    void renderDebugCapture(S32 which);

    void renderDebugStreamlines();

    F32 slabAlpha(const Tile& tile, S32 k, F32 cam_z) const;

    void buildCarveFlags(const Tile& tile);

    void readMaskForBridge(const Tile& tile);
    void bridgePassages(const Tile& tile);
    void uploadBridgedMask(const Tile& tile);

    void placeSlices(Tile& tile);

    bool solveInit(const Tile& tile);
    bool solveRun(const Tile& tile);

    static S32 levelRes(S32 res, S32 level) { return res >> level; }
    static S32 levelCount(S32 res);

    void readback(Tile& tile);
    void unpackVolume(Tile& tile);

    static size_t index(const Tile& tile, S32 x, S32 y, S32 k)
    {
        return ((size_t)k * tile.mRes + y) * tile.mRes + x;
    }

    static void sliceAt(const Tile& tile, F32 z, S32& k, F32& frac);

    bool mShadersReady = false;
    bool mShaderFailed = false;

    std::map<U64, Tile> mTiles;

    std::vector<F32> mTop;

    std::vector<F32> mProbeDepth[SS_WIND_PROBES];
    glm::mat4        mProbeView[SS_WIND_PROBES];
    F32              mProbeHalf[SS_WIND_PROBES] = { 0.f };

    S32              mProbeRes = 0;

    struct ProbeFrame
    {
        LLVector3 mEye;
        LLVector3 mDir;
        LLVector3 mRight;
        LLVector3 mUp;
    };
    ProbeFrame       mProbeFrame[SS_WIND_PROBES];

    U64              mCaptureRegion = 0;
    S32              mCaptureRes = 0;
    F32              mCaptureCell = 0.f;
    LLVector3        mCaptureOrigin;
    bool             mProbeUsable[SS_WIND_PROBES] = { false };
    F32              mProbeMiss[SS_WIND_PROBES] = { 1.f };

    std::vector<F32> mHidden;

    U32 mHeightTex = 0;
    U32 mProbeTex = 0;
    U32 mSolidTex[SS_WIND_MAX_LEVELS] = { 0 };
    U32 mVelTex[SS_WIND_MAX_LEVELS] = { 0 };
    U32 mDivTex[SS_WIND_MAX_LEVELS] = { 0 };
    U32 mPressureTex[SS_WIND_MAX_LEVELS][2] = {};
    S32 mTexRes = 0;
    S32 mTexSlices = 0;
    S32 mProbeTexRes = 0;
    S32 mTexLevels = 0;

    LLRenderTarget mCapture;
    LLRenderTarget mProbeCapture;

    std::vector<U8> mCarveFlags;

    F64 mLastBuild = 0.0;
    F32 mSolveMS = 0.f;
    U32 mBuildCount = 0;

    EStage mStage = EStage::IDLE;

    Tile mBuild;

    U64 mBuildRegion = 0;
    S32 mBuildProbe = 0;
    F64 mBuildStart = 0.0;
    bool mWorkerBusy = false;
    bool mClearPending = false;

    U32 mBuildGeneration = 0;

    std::vector<U8> mMaskRaw;
    std::vector<U8> mMaskBridged;
    bool mMaskChanged = false;

    std::vector<F32> mVolumeRaw;
    std::vector<U8> mSolidRaw;
};

#endif
