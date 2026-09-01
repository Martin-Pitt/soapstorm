/**
 * @file ssghillie.h
 * @brief Ghillie: multi-threaded software Hi-Z object occlusion culling.
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

#ifndef SS_GHILLIE_H
#define SS_GHILLIE_H

#include "llsingleton.h"
#include "llquaternion.h"
#include "lltimer.h"
#include "lluuid.h"
#include "m4math.h"
#include "v3math.h"
#include "v4math.h"

#include "ssghilliemesh.h"  // SS::GhillieMeshBox / GhillieMeshDecomp

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class LLViewerCamera;
class LLViewerOctreeGroup;
class LLCullResult;
class LLDrawable;
class LLVector4a;
class LLSurface;

// <SS:Nexii> Ghillie: offloaded object occlusion culling
// Replaces the stock per-node GL occlusion query verdicts for the main camera
// with a software hierarchical-Z test built and consumed on worker threads.
// Stock behavior is untouched while SSGhillieEnabled is off.
//
// v2 additions (from the design review and combat analysis):
//   - occluders are exact parametric prims + AABB fallbacks + terrain/water
//     + mesh decomposition boxes, not just pristine boxes;
//   - a static-landscape tier (how long an object has not moved; never-moved
//     fast path; teleport/death resets) determines what may use stable
//     occluders and tile-persistence;
//   - an activity + proximity gate defers "peek around me" occluders while
//     the camera is actively moving (walk/run/mouselook/vehicle), so FPS
//     doesn't tank exactly when the user turns a corner;
//   - parallax-ranked live occluders and static-tile HZB reuse under camera
//     stickiness keep per-frame raster work bounded;
//   - a teleport-aware fast full-rebuild path keeps death/respawn flat.
class SSGhillie : public LLSingleton<SSGhillie>
{
    LLSINGLETON(SSGhillie);

public:
    void cleanupSingleton() override;

    // Consume published worker verdicts before octree traversal.
    // [interaction: LLPipeline] called from llviewerdisplay.cpp ahead of updateCull
    void preCull(const LLViewerCamera& camera);

    // Snapshot occluders/occludees and post the worker job DAG.
    // [interaction: LLPipeline] called from LLPipeline::postSort while sCull is populated
    void gatherAndPost(const LLViewerCamera& camera, LLCullResult& cull);

    // Traversal consult from LLOctreeCull::earlyFail (main thread).
    // Returns true when the node's subtree can be pruned as occluded; also
    // records the node so it is re-tested every frame.
    bool consultNode(LLViewerOctreeGroup* group);

    // Stats overlay + on-world occluder/occludee debug render.
    // drawDebug() is the top-left text overlay, called from llviewerwindow.cpp;
    // drawDebugWorld() draws world-space wireframe boxes and is called from
    // LLPipeline::renderGeom (world camera bound) via renderDebug().
    void drawDebug();
    void drawDebugWorld();

    // True while Ghillie owns the main camera's occlusion verdicts this frame.
    // Stock query paths consult this to stand down; read only on the main thread.
    static bool sActive;

    // True when the current camera pass is the main world pass under Ghillie.
    // Shadow, reflection and HUD passes keep the stock GL query machine running.
    static bool activeForCurrentPass();

private:
    static constexpr U32 NO_GENERATION = 0;

    // --- camera mode / activity state (main thread only) ---
    enum CameraActivity
    {
        CAMERA_STILL,
        CAMERA_RELAXED,     // low movement, third person look around
        CAMERA_WALK_RUN,
        CAMERA_MLOOK,       // mouselook / first person peek tension
        CAMERA_VEHICLE,     // sustained high angular or linear velocity
        CAMERA_FAST_REBUILD // teleport / death respawn / region change
    };

    struct CameraState
    {
        CameraActivity mActivity = CAMERA_STILL;
        U32 mModeFrame = 0;         // frame the mode last changed
        U32 mLastFrame = 0;
        LLVector3 mLastPos;
        LLVector3 mLastAt;
        bool mDeferNearPeek = false;
    };

    // --- occluder PODs shared with workers (no viewer structures) ---
    struct Occluder
    {
        LLVector3 mPos;         // agent space center
        LLQuaternion mRot;      // world rotation (consistent with agent space)
        LLVector3 mHalfScale;   // half extents (always positive)
    };

    // One expanded occluder emitted into the job, already in agent space.
    struct OccluderSolid
    {
        U8 mKind;               // OC_SEG_BOX, OC_STATIC_BOX, OC_STATIC_SLAB
        LLVector3 mPos;         // agent space center
        LLQuaternion mRot;      // world rotation (consistent with agent space)
        LLVector3 mHalf;        // agent space half extents (always positive)
        U8 mProfile;            // one of the LL_PCODE_PROFILE_* values
    };

    struct Occludee
    {
        LLViewerOctreeGroup* mGroup;
        LLVector3 mCenter;      // agent space center
        LLVector3 mHalfSize;    // agent space half extents
    };

    struct Verdict
    {
        LLViewerOctreeGroup* mGroup = nullptr;
        bool mOccluded = false;
    };

    struct Job
    {
        U32 mGeneration = NO_GENERATION;
        S32 mTileCount = 1;
        U32 mHzbWidth = 0;
        U32 mHzbHeight = 0;
        LLMatrix4 mModelview;   // agent -> eye (row vector convention)
        LLMatrix4 mProjection;  // eye -> clip
        LLVector3 mEye;
        LLVector3 mAtAxis;

        std::vector<OccluderSolid> mOccluders;
        std::vector<Occludee> mOccludees;

        // worker-owned terrain/water state (snapshotted at gather time)
        std::vector<F32> mTerrainHeights;   // region-local height grid
        S32 mTerrainGrid = 0;               // grid points per edge
        F32 mTerrainMetersPerGrid = 0.f;
        LLVector3 mTerrainOrigin;           // region origin, agent space
        F32 mWaterZ = 0.f;
        bool mWaterEnabled = false;

        // worker-owned raster state
        std::vector<U32> mHzb;                  // base level, width*height
        std::vector<std::vector<U32>> mMips;    // max-reduced levels
        std::atomic<S32> mTileCountdown{ 0 };
        std::atomic<S32> mTestCountdown{ 0 };
        std::atomic<bool> mAbandon{ false };    // watchdog: skip work but still drain
        LLTimer mTimer;
        std::atomic<F32> mHiZMS{ 0.f };
    };

    struct NodeState
    {
        U32 mLastSeenFrame = 0;
        U32 mVerdictFrame = 0;  // frame the current streak was computed from
        U32 mStreak = 0;        // consecutive occluded verdicts
    };

    struct Result
    {
        U32 mGeneration = NO_GENERATION;
        LLVector3 mEye;
        LLVector3 mAtAxis;
        std::vector<Verdict> mVerdicts;
        U64 mOccluderCount = 0;
        U64 mOccludedCount = 0;
        F32 mHiZMS = 0.f;
        F32 mTotalMS = 0.f;
        U32 mGhillieBoxes = 0;  // occluders drawn this frame (debug overlay)
    };

    // --- static-landscape classification (main thread only) ---
    struct ObjectRecord
    {
        LLUUID mObjectID;
        LLVector3 mLastPos;
        LLVector3 mLastScale;
        U32 mLastMoveFrame = 0;
        U32 mFirstSeenFrame = 0;
        F32 mStillSeconds = 0.f;
        bool mNeverMoved = true;
        bool mTeleported = false;
    };

    struct MeshCacheEntry
    {
        LLUUID mMeshID;                     // the mesh asset's own ID
        LLVector3 mScale;
        LLVector3 mPosClass;                // coarse cell to share transforms across instances
        SS::GhillieMeshDecomp mDecomp;
        bool mReady = false;
    };

    bool enabledSetting() const;
    void clearNodeStates();
    void updateCameraMode(const LLViewerCamera& camera);
    void updateStaticRecords(LLCullResult& cull);
    void resolveMeshOccluders(LLCullResult& cull, std::vector<OccluderSolid>& out);
    void postJob();
    void runTileJob(S32 tile);      // worker phase 1: rasterize occluders
    void buildMips();               // worker phase 2 (single thread)
    void runTestChunk(S32 chunk);   // worker phase 3
    bool testOccludee(const Occludee& occludee);
    void publish();                 // worker phase 4
    void drawBoxLines(const LLVector3& pos, const LLQuaternion& rot, const LLVector3& half, bool thick);

    std::unique_ptr<LL::ThreadPool> mThreadPool;
    std::unique_ptr<LL::ThreadPool> mMeshPool;  // low-priority worker pool (mesh decomp)

    // --- main thread only ---
    std::vector<LLViewerOctreeGroup*> mSeenNodes;                       // visited nodes this frame
    std::unordered_map<LLViewerOctreeGroup*, NodeState> mNodeStates;    // keyed by live pointer, never dereferenced
    std::vector<Occluder> mCandScratch;                                 // occluder candidate scratch
    std::vector<F32> mCandDistScratch;
    std::vector<SS::GhillieMeshBox> mBoxScratch;                        // decomposed-wall scratch
    std::vector<OccluderSolid> mDebugDeferred;                          // near-peek-deferred occluders (debug world view)
    std::unordered_map<LLUUID, ObjectRecord> mObjectRecords;
    std::unordered_map<LLUUID, MeshCacheEntry> mMeshCache;
    std::mutex mMeshCacheMutex;             // worker writes mDecomp/mReady under this
    U32 mLastPruneFrame = 0;
    U32 mLastTeleportFrame = 0;

    CameraState mCameraState;

    // --- cross-thread handoff ---
    Job mJob;               // main writes at post time, workers read until publish
    Result mResults[2];     // workers fill one while main reads the other
    std::atomic<U32> mBackBufferIdx{ 0 };
    std::atomic<U32> mPublishedIdx{ 0 };
    std::atomic<U32> mPublishedGen{ NO_GENERATION };
    U32 mConsumedGen = NO_GENERATION;
    U32 mNextGeneration = NO_GENERATION;
    std::atomic<bool> mJobInFlight{ false };
    U32 mLastJobFrame = 0;

    // --- stats (main thread) ---
    U32 mSkipFrames = 0;
    U64 mTotalNodesOccluded = 0;
};
// </SS:Nexii>

#endif // SS_GHILLIE_H