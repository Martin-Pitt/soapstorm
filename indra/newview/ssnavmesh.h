/**
 * @file ssnavmesh.h
 * @brief Atmo Magic: the census navmesh - Recast/Detour over the declared-shape census.
 *
 *        Replaces the homebrew 3D-tiled census raster (ssworldfieldtiles, removed
 *        2026-09-09) with recastnavigation: the census records are emitted as
 *        triangles per 32 m column, each column split into height bands wherever
 *        an air gap of SSNavMeshBandGap separates the geometry (a skybox never
 *        shares a build with the ground, and a band boundary never cuts a floor,
 *        which is what kept Detour's tile portals intact in the benchmark), and
 *        every band is one DetourTileCache layer. Rasterization runs on the
 *        General work queue; publishing, obstacles and queries stay on the main
 *        thread. Benchmark and design: doc/atmo_magic_navmesh.md section 10.
 *
 *        DYNAMIC records (movers) never enter a layer: they ride the tile cache
 *        as temporary box obstacles, re-synced on every census rebuild.
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

#ifndef SS_NAVMESH_H
#define SS_NAVMESH_H

#include "llsingleton.h"
#include "v3dmath.h"
#include "v3math.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class dtTileCache;
struct SSNavMeshImpl;

class SSNavMesh : public LLSingleton<SSNavMesh>
{
    LLSINGLETON(SSNavMesh);
    ~SSNavMesh();

public:
    static constexpr F32 TILE_M = 32.f;         // column edge in metres
    static constexpr S32 TILE_CELLS = 128;      // 0.25 m cells per column edge
    static constexpr F32 CELL = 0.25f;
    static constexpr S32 MAX_BANDS = 16;        // height bands per column (Detour layers)

    // Per-frame maintenance: follow census rebuilds, schedule changed bands,
    // launch worker builds under the frame budget, publish finished layers,
    // sync mover obstacles and pump the tile cache.
    void update();

    // Queries in agent coordinates. nearestPoint snaps to the navmesh within
    // reach; findPath fills a straight-path polyline, partial when the goal was
    // unreachable and the path ends at the closest point found.
    bool nearestPoint(const LLVector3& pos_agent, F32 reach, LLVector3& out_agent) const;
    bool findPath(const LLVector3& from_agent, const LLVector3& to_agent, std::vector<LLVector3>& out_points, bool& out_partial) const;
    bool onNavMesh(const LLVector3& pos_agent, F32 reach = 1.f) const;

    // The overlay: filled polygons and edges by band, tile links, mover obstacles, the census and the test path,
    // each behind its SSNavMeshShow* switch (the floater's View tab). force_navmesh draws the polygons regardless -
    // view 7 of SSWorldFieldDebugView.
    void renderDebug(bool force_navmesh = false);
    static bool overlayEnabled();

    // Drop every band and build again from the current census (the floater's Rebuild button).
    void rebuildAll();

    // The floater's Test path tab: endpoints in agent space, Detour's answer drawn in the overlay.
    void setTestStart(const LLVector3& pos_agent) { mTestStart = pos_agent; mHasTestStart = true; mTestValid = false; }
    void setTestEnd(const LLVector3& pos_agent) { mTestEnd = pos_agent; mHasTestEnd = true; mTestValid = false; }
    void clearTestPath() { mHasTestStart = mHasTestEnd = mTestValid = false; mTestPath.clear(); }
    bool hasTestStart() const { return mHasTestStart; }
    bool hasTestEnd() const { return mHasTestEnd; }
    bool runTestPath(std::string& out_status);

    // What the last schedule saw in the census.
    S32 lastScheduleSeen() const { return mLastSeen; }
    S32 lastScheduleDynamic() const { return mLastDynamic; }
    S32 lastSchedulePhantom() const { return mLastPhantom; }

    // Stats
    bool active() const { return mNavMesh != nullptr; }
    S32 columnCount() const { return (S32)mColumns.size(); }
    S32 bandCount() const { return (S32)mBands.size(); }
    S32 pendingCount() const { return (S32)mWorklist.size(); }
    S32 inFlightCount() const { return mInFlight; }
    S32 obstacleCount() const { return (S32)mObstacles.size(); }
    U32 buildCount() const { return mBuildCount; }
    F32 lastBuildMS() const { return mLastBuildMS; }
    U32 polyCount() const;
    size_t layerBytes() const { return mLayerBytes; }

private:
    // One band of one column: its geometry signature and the compressed tiles it published.
    struct Band
    {
        U64 mSig = 0;
        F32 mZMin = 0.f;
        F32 mZMax = 0.f;
        std::vector<U32> mRefs;             // dtCompressedTileRef per layer
        bool mAlive = false;                // touched by the latest schedule
        F64 mPublishedAt = -100.0;          // when its layers last landed, for the overlay's rebuild flash
    };

    // A band waiting for a worker build.
    struct Job
    {
        S32 mTx = 0, mTy = 0, mBand = 0;
        F32 mZMin = 0.f, mZMax = 0.f;
        U64 mSig = 0;
    };

    // What a worker hands back: compressed layers for one band.
    struct Result
    {
        Job mJob;
        U32 mGeneration = 0;
        std::vector<std::vector<U8> > mLayers;
        F32 mMS = 0.f;
        bool mOk = false;
    };

    bool ensureInit();
    void teardown();
    void schedule();
    void launch(const Job& job);
    void publish(const std::shared_ptr<Result>& result);
    void removeBand(U64 key, Band& band);
    void syncObstacles();
    void pumpObstacles();
    bool terrainZLocal(F32 x, F32 y, F32& z) const;
    LLVector3 toLocal(const LLVector3& pos_agent) const;
    LLVector3 fromLocal(const LLVector3& pos_local) const;
    static U64 bandKey(S32 tx, S32 ty, S32 band);
    static U64 columnKey(S32 tx, S32 ty);

    std::unique_ptr<SSNavMeshImpl> mImpl;
    dtNavMesh* mNavMesh = nullptr;
    dtTileCache* mTileCache = nullptr;
    dtNavMeshQuery* mQuery = nullptr;

    // <SS:Nexii> The build frame: agent coordinates shift by a region on every border crossing, so tiles are keyed in a frame pinned to the region origin at init and every agent-space position is re-based through toLocal; Detour then never sees a key change on a crossing. [interaction: census anchor]
    LLVector3d mOriginGlobal;
    U64 mCensusStamp = 0;
    U32 mGeneration = 0;                        // bumped on teardown so late worker results are dropped
    std::unordered_map<U64, Band> mBands;
    std::unordered_map<U64, S32> mColumns;      // column key -> band count
    std::vector<Job> mWorklist;
    // Mover obstacles: the live set plus the adds and removals still to be handed to the tile cache under the
    // per-frame request budget (dtTileCache queues at most 64 requests between its updates).
    struct Obstacle
    {
        U32 mRef = 0;                           // dtObstacleRef
        F32 mMin[3] = {0, 0, 0};                // Recast-space box
        F32 mMax[3] = {0, 0, 0};
    };
    std::vector<Obstacle> mObstacles;
    std::vector<Obstacle> mObstacleAdds;
    std::vector<U32> mObstacleRemovals;
    S32 mInFlight = 0;
    U32 mBuildCount = 0;
    F32 mLastBuildMS = 0.f;
    size_t mLayerBytes = 0;
    S32 mLastSeen = 0, mLastDynamic = 0, mLastPhantom = 0;

    // Test path state (agent space).
    LLVector3 mTestStart, mTestEnd;
    bool mHasTestStart = false, mHasTestEnd = false, mTestValid = false, mTestPartial = false;
    std::vector<LLVector3> mTestPath;
};

#endif
