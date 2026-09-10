/**
 * @file ssnavmesh.cpp
 * @brief See ssnavmesh.h.
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

#include "ssnavmesh.h"

#include "ssworldfieldshapes.h"

#include "llagent.h"
#include "llfasttimer.h"
#include "llgl.h"
#include "llrender.h"
#include "llsurface.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "workqueue.h"

#include "Recast.h"
#include "DetourAlloc.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"

#ifdef LL_USESYSTEMLIBS
#include <zlib.h>
#else
#include "zlib-ng/zlib.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

static LLTrace::BlockTimerStatHandle FTM_SS_NAVMESH_UPDATE("SS NavMesh Update");
static LLTrace::BlockTimerStatHandle FTM_SS_NAVMESH_PUBLISH("SS NavMesh Publish");

// Build-side constants: the Recast config the benchmark ran with (doc/atmo_magic_navmesh.md 10). The agent
// parameters are settings; the rest is fixed until a second agent class exists.
static constexpr F32 SS_NAV_MAX_EDGE_M = 12.f;
static constexpr F32 SS_NAV_MAX_SIMPLIFICATION_ERROR = 1.3f;
static constexpr S32 SS_NAV_MIN_REGION_CELLS = 8;
static constexpr S32 SS_NAV_MERGE_REGION_CELLS = 20;
static constexpr S32 SS_NAV_MAX_LAYERS_PER_BAND = 8;
static constexpr S32 SS_NAV_TERRAIN_NODES = 37;     // 1 m grid over the bordered column: 32 m + 2 x 2 m margin, plus one
static constexpr F32 SS_NAV_TERRAIN_MARGIN_M = 2.f;
static constexpr S32 SS_NAV_TERRAIN_SCHEDULE_STEP = 4;  // metres between scheduling samples of the land
static constexpr S32 SS_NAV_MAX_TILES = 4096;
static constexpr S32 SS_NAV_MAX_OBSTACLES = 512;
static constexpr S32 SS_NAV_OBSTACLE_REQUESTS_PER_FRAME = 48;   // under dtTileCache's 64-request queue, drained once per update
static constexpr S32 SS_NAV_QUERY_NODES = 4096;
static constexpr S32 SS_NAV_MAX_PATH = 512;

// ---------------------------------------------------------------------------- Recast-side helpers

namespace
{
    // zlib-backed DetourTileCache compressor: the tile cache owns compressed layers and inflates on rebuild.
    struct SSNavCompressor : public dtTileCacheCompressor
    {
        int maxCompressedSize(const int bufferSize) override { return (int)compressBound((uLong)bufferSize); }
        dtStatus compress(const unsigned char* buffer, const int bufferSize, unsigned char* compressed, const int maxCompressedSize, int* compressedSize) override
        {
            uLongf out = (uLongf)maxCompressedSize;
            if (compress2(compressed, &out, buffer, (uLong)bufferSize, Z_BEST_SPEED) != Z_OK) return DT_FAILURE;
            *compressedSize = (int)out;
            return DT_SUCCESS;
        }
        dtStatus decompress(const unsigned char* compressed, const int compressedSize, unsigned char* buffer, const int maxBufferSize, int* bufferSize) override
        {
            uLongf out = (uLongf)maxBufferSize;
            if (uncompress(buffer, &out, compressed, (uLong)compressedSize) != Z_OK) return DT_FAILURE;
            *bufferSize = (int)out;
            return DT_SUCCESS;
        }
    };

    // Every polygon walkable, one area: agent classes and door portals are later plumbing.
    struct SSNavMeshProcess : public dtTileCacheMeshProcess
    {
        void process(dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags) override
        {
            for (int i = 0; i < params->polyCount; ++i) { polyAreas[i] = 0; polyFlags[i] = 1; }
        }
    };

    // Triangle soup in Recast space (x, up, z) = (local x, local z, -local y): a proper rotation, so outward
    // winding survives and floors keep their up-facing normals.
    struct SSNavSoup
    {
        std::vector<float> mVerts;
        void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c)
        {
            const LLVector3* p[3] = {&a, &b, &c};
            for (S32 i = 0; i < 3; ++i) { mVerts.push_back(p[i]->mV[VX]); mVerts.push_back(p[i]->mV[VZ]); mVerts.push_back(-p[i]->mV[VY]); }
        }
        void quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d) { tri(a, b, c); tri(a, c, d); }
        S32 triCount() const { return (S32)(mVerts.size() / 9); }
    };

    // The main-thread snapshot a worker build consumes: geometry, terrain heights, config.
    struct SSNavBuildInput
    {
        SSNavSoup mSoup;
        F32 mMin[3] = {0, 0, 0};            // local-space column bounds (x, y, band zmin)
        F32 mMax[3] = {0, 0, 0};
        F32 mAgentHeight = 2.f, mAgentRadius = 0.5f, mAgentClimb = 0.75f, mAgentSlope = 45.f;
        S32 mTx = 0, mTyDetour = 0, mBand = 0;
    };

    void emitBox(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavSoup& out)
    {
        LLVector3 c[8];
        S32 i = 0;
        for (S32 z = -1; z <= 1; z += 2) for (S32 y = -1; y <= 1; y += 2) for (S32 x = -1; x <= 1; x += 2)
        {
            c[i++] = r.mCenter + off + r.mAxes[0] * (r.mHalf.mV[VX] * (F32)x) + r.mAxes[1] * (r.mHalf.mV[VY] * (F32)y) + r.mAxes[2] * (r.mHalf.mV[VZ] * (F32)z);
        }
        out.quad(c[0], c[2], c[3], c[1]);
        out.quad(c[4], c[5], c[7], c[6]);
        out.quad(c[0], c[1], c[5], c[4]);
        out.quad(c[2], c[6], c[7], c[3]);
        out.quad(c[0], c[4], c[6], c[2]);
        out.quad(c[1], c[3], c[7], c[5]);
    }

    void emitCylinder(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavSoup& out)
    {
        const S32 n = llclamp((S32)(8.f + r.mRadius * 8.f), 8, 32);
        const F32 rad = r.mRadius / cosf(F_PI / (F32)n);      // circumscribed: conservative
        LLVector3 r0[32], r1[32];
        const LLVector3 c0 = r.mCenter + off - r.mAxes[2] * r.mHalfHeight;
        const LLVector3 c1 = r.mCenter + off + r.mAxes[2] * r.mHalfHeight;
        for (S32 k = 0; k < n; ++k)
        {
            const F32 a = F_TWO_PI * (F32)k / (F32)n;
            const LLVector3 o = r.mAxisU * (cosf(a) * rad) + r.mAxisV * (sinf(a) * rad);
            r0[k] = c0 + o; r1[k] = c1 + o;
        }
        for (S32 k = 0; k < n; ++k)
        {
            const S32 j = (k + 1) % n;
            out.quad(r0[k], r0[j], r1[j], r1[k]);
            out.tri(c0, r0[j], r0[k]);
            out.tri(c1, r1[k], r1[j]);
        }
    }

    void emitEllipsoid(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavSoup& out)
    {
        const F32 rmax = llmax(r.mRadii.mV[VX], llmax(r.mRadii.mV[VY], r.mRadii.mV[VZ]));
        const S32 nl = llclamp((S32)(8.f + rmax * 6.f), 8, 24), ns = llmax(4, nl / 2);
        auto pt = [&](S32 st, S32 sl)
        {
            const F32 phi = F_PI * (F32)st / (F32)ns, th = F_TWO_PI * (F32)sl / (F32)nl;
            return r.mCenter + off + r.mAxes[0] * (sinf(phi) * cosf(th) * r.mRadii.mV[VX])
                                   + r.mAxes[1] * (sinf(phi) * sinf(th) * r.mRadii.mV[VY])
                                   + r.mAxes[2] * (cosf(phi) * r.mRadii.mV[VZ]);
        };
        for (S32 st = 0; st < ns; ++st) for (S32 sl = 0; sl < nl; ++sl)
        {
            out.quad(pt(st, sl), pt(st + 1, sl), pt(st + 1, sl + 1), pt(st, sl + 1));
        }
    }

    void emitRecord(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavSoup& out)
    {
        switch (r.mClass)
        {
            case SSWorldFieldShapes::Record::CLASS_BOX: emitBox(r, off, out); break;
            case SSWorldFieldShapes::Record::CLASS_CYLINDER: emitCylinder(r, off, out); break;
            case SSWorldFieldShapes::Record::CLASS_SPHERE: emitEllipsoid(r, off, out); break;
            case SSWorldFieldShapes::Record::CLASS_TRI:
            {
                const std::vector<LLVector3>& tri = r.tris();
                const size_t nt = tri.size() / 3;
                for (size_t k = 0; k < nt; ++k) out.tri(tri[k * 3] + off, tri[k * 3 + 1] + off, tri[k * 3 + 2] + off);
                break;
            }
        }
    }

    // Terrain over the bordered column as two triangles per 1 m cell; heights were sampled on the main thread.
    void emitTerrain(const F32* heights, F32 x0, F32 y0, SSNavSoup& out)
    {
        const S32 n = SS_NAV_TERRAIN_NODES;
        for (S32 gy = 0; gy + 1 < n; ++gy) for (S32 gx = 0; gx + 1 < n; ++gx)
        {
            const F32 h00 = heights[gy * n + gx], h10 = heights[gy * n + gx + 1], h11 = heights[(gy + 1) * n + gx + 1], h01 = heights[(gy + 1) * n + gx];
            if (h00 < -900.f || h10 < -900.f || h11 < -900.f || h01 < -900.f) continue;     // void between regions
            const F32 x = x0 + (F32)gx, y = y0 + (F32)gy;
            out.quad(LLVector3(x, y, h00), LLVector3(x + 1.f, y, h10), LLVector3(x + 1.f, y + 1.f, h11), LLVector3(x, y + 1.f, h01));
        }
    }

    U64 fnv(U64 h, U64 v) { h ^= v; h *= 1099511628211ull; return h; }
    U64 fnvF(U64 h, F32 f) { return fnv(h, (U64)(S64)llround(f * 100.f)); }

    // The worker: one band through Recast to compressed tile cache layers.
    void buildBand(const SSNavBuildInput& in, SSNavCompressor& comp, std::vector<std::vector<U8> >& out_layers)
    {
        rcContext ctx(false);
        rcConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.cs = SSNavMesh::CELL;
        cfg.ch = SSNavMesh::CELL;
        cfg.walkableSlopeAngle = in.mAgentSlope;
        cfg.walkableHeight = (int)ceilf(in.mAgentHeight / cfg.ch);
        cfg.walkableClimb = (int)floorf(in.mAgentClimb / cfg.ch);
        cfg.walkableRadius = (int)ceilf(in.mAgentRadius / cfg.cs);
        cfg.maxEdgeLen = (int)(SS_NAV_MAX_EDGE_M / cfg.cs);
        cfg.maxSimplificationError = SS_NAV_MAX_SIMPLIFICATION_ERROR;
        cfg.minRegionArea = SS_NAV_MIN_REGION_CELLS * SS_NAV_MIN_REGION_CELLS;
        cfg.mergeRegionArea = SS_NAV_MERGE_REGION_CELLS * SS_NAV_MERGE_REGION_CELLS;
        cfg.maxVertsPerPoly = 6;
        cfg.tileSize = SSNavMesh::TILE_CELLS;
        cfg.borderSize = cfg.walkableRadius + 3;
        cfg.width = cfg.tileSize + cfg.borderSize * 2;
        cfg.height = cfg.tileSize + cfg.borderSize * 2;
        const F32 border = (F32)cfg.borderSize * cfg.cs;
        cfg.bmin[0] = in.mMin[0] - border; cfg.bmax[0] = in.mMax[0] + border;
        cfg.bmin[1] = in.mMin[2];          cfg.bmax[1] = in.mMax[2];
        cfg.bmin[2] = -in.mMax[1] - border; cfg.bmax[2] = -in.mMin[1] + border;

        const S32 ntris = in.mSoup.triCount();
        if (ntris == 0) return;
        std::vector<int> idx((size_t)ntris * 3);
        for (S32 i = 0; i < ntris * 3; ++i) idx[i] = i;
        std::vector<unsigned char> areas((size_t)ntris, 0);

        rcHeightfield* hf = rcAllocHeightfield();
        if (!hf || !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) { rcFreeHeightField(hf); return; }
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, in.mSoup.mVerts.data(), ntris * 3, idx.data(), ntris, areas.data());
        rcRasterizeTriangles(&ctx, in.mSoup.mVerts.data(), ntris * 3, idx.data(), areas.data(), ntris, *hf, cfg.walkableClimb);
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

        rcCompactHeightfield* chf = rcAllocCompactHeightfield();
        const bool compact_ok = chf && rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf);
        rcFreeHeightField(hf);
        if (!compact_ok) { rcFreeCompactHeightfield(chf); return; }
        rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);

        rcHeightfieldLayerSet* lset = rcAllocHeightfieldLayerSet();
        if (lset && rcBuildHeightfieldLayers(&ctx, *chf, cfg.borderSize, cfg.walkableHeight, *lset))
        {
            for (int i = 0; i < lset->nlayers && i < SS_NAV_MAX_LAYERS_PER_BAND; ++i)
            {
                const rcHeightfieldLayer* layer = &lset->layers[i];
                dtTileCacheLayerHeader header;
                header.magic = DT_TILECACHE_MAGIC;
                header.version = DT_TILECACHE_VERSION;
                header.tx = in.mTx;
                header.ty = in.mTyDetour;
                header.tlayer = in.mBand * SS_NAV_MAX_LAYERS_PER_BAND + i;
                dtVcopy(header.bmin, layer->bmin);
                dtVcopy(header.bmax, layer->bmax);
                header.width = (unsigned char)layer->width;
                header.height = (unsigned char)layer->height;
                header.minx = (unsigned char)layer->minx;
                header.maxx = (unsigned char)layer->maxx;
                header.miny = (unsigned char)layer->miny;
                header.maxy = (unsigned char)layer->maxy;
                header.hmin = (unsigned short)layer->hmin;
                header.hmax = (unsigned short)layer->hmax;
                unsigned char* data = nullptr;
                int size = 0;
                if (dtStatusSucceed(dtBuildTileCacheLayer(&comp, &header, layer->heights, layer->areas, layer->cons, &data, &size)) && data)
                {
                    out_layers.emplace_back(data, data + size);
                    dtFree(data);
                }
            }
        }
        rcFreeHeightfieldLayerSet(lset);
        rcFreeCompactHeightfield(chf);
    }
}

struct SSNavMeshImpl
{
    dtTileCacheAlloc mAlloc;
    SSNavCompressor mCompressor;
    SSNavMeshProcess mProcess;
};

// ---------------------------------------------------------------------------- lifecycle

SSNavMesh::SSNavMesh() : mImpl(new SSNavMeshImpl())
{
}

SSNavMesh::~SSNavMesh()
{
    teardown();
}

// Tile keys pack column and band; columns carry the band count for pruning.
U64 SSNavMesh::bandKey(S32 tx, S32 ty, S32 band)
{
    return ((U64)(U32)(tx + (1 << 20)) << 42) | ((U64)(U32)(ty + (1 << 20)) << 21) | (U64)(U32)band;
}

U64 SSNavMesh::columnKey(S32 tx, S32 ty)
{
    return bandKey(tx, ty, 0x1FFFFF);
}

// Agent space to the pinned build frame: the difference between the agent's current region origin and the one
// captured at init, so a border crossing shifts positions, never tile keys.
LLVector3 SSNavMesh::toLocal(const LLVector3& pos_agent) const
{
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp) return pos_agent;
    const LLVector3d d = regionp->getOriginGlobal() - mOriginGlobal;
    return pos_agent + LLVector3((F32)d.mdV[VX], (F32)d.mdV[VY], (F32)d.mdV[VZ]);
}

LLVector3 SSNavMesh::fromLocal(const LLVector3& pos_local) const
{
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp) return pos_local;
    const LLVector3d d = regionp->getOriginGlobal() - mOriginGlobal;
    return pos_local - LLVector3((F32)d.mdV[VX], (F32)d.mdV[VY], (F32)d.mdV[VZ]);
}

// Land height at a local-frame xy; false in the void between regions.
bool SSNavMesh::terrainZLocal(F32 x, F32 y, F32& z) const
{
    const LLVector3 pos_agent = fromLocal(LLVector3(x, y, 0.f));
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;
    LLVector3 region_pos = regionp->getPosRegionFromAgent(pos_agent);
    region_pos.mV[VX] = llclamp(region_pos.mV[VX], 0.f, 255.9f);
    region_pos.mV[VY] = llclamp(region_pos.mV[VY], 0.f, 255.9f);
    z = regionp->getLand().resolveHeightRegion(region_pos.mV[VX], region_pos.mV[VY]);
    return true;
}

// Allocate the navmesh, the tile cache and the query once the agent has a region to pin the frame to.
bool SSNavMesh::ensureInit()
{
    if (mNavMesh) return true;
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp) return false;
    mOriginGlobal = regionp->getOriginGlobal();

    static LLCachedControl<F32> agent_height(gSavedSettings, "SSNavMeshAgentHeight", 2.f);
    static LLCachedControl<F32> agent_radius(gSavedSettings, "SSNavMeshAgentRadius", 0.5f);
    static LLCachedControl<F32> agent_climb(gSavedSettings, "SSNavMeshAgentClimb", 0.75f);

    dtTileCacheParams tcp;
    memset(&tcp, 0, sizeof(tcp));
    tcp.cs = CELL;
    tcp.ch = CELL;
    tcp.width = TILE_CELLS;
    tcp.height = TILE_CELLS;
    tcp.walkableHeight = agent_height;
    tcp.walkableRadius = agent_radius;
    tcp.walkableClimb = agent_climb;
    tcp.maxSimplificationError = SS_NAV_MAX_SIMPLIFICATION_ERROR;
    tcp.maxTiles = SS_NAV_MAX_TILES;
    tcp.maxObstacles = SS_NAV_MAX_OBSTACLES;
    mTileCache = dtAllocTileCache();
    if (!mTileCache || dtStatusFailed(mTileCache->init(&tcp, &mImpl->mAlloc, &mImpl->mCompressor, &mImpl->mProcess)))
    {
        LL_WARNS("SSNavMesh") << "tile cache init failed" << LL_ENDL;
        teardown();
        return false;
    }

    dtNavMeshParams np;
    memset(&np, 0, sizeof(np));
    np.tileWidth = TILE_M;
    np.tileHeight = TILE_M;
    S32 tile_bits = 0;
    while ((1 << tile_bits) < SS_NAV_MAX_TILES) ++tile_bits;
    np.maxTiles = SS_NAV_MAX_TILES;
    np.maxPolys = 1 << (22 - tile_bits);
    mNavMesh = dtAllocNavMesh();
    if (!mNavMesh || dtStatusFailed(mNavMesh->init(&np)))
    {
        LL_WARNS("SSNavMesh") << "navmesh init failed" << LL_ENDL;
        teardown();
        return false;
    }
    mQuery = dtAllocNavMeshQuery();
    if (!mQuery || dtStatusFailed(mQuery->init(mNavMesh, SS_NAV_QUERY_NODES)))
    {
        LL_WARNS("SSNavMesh") << "navmesh query init failed" << LL_ENDL;
        teardown();
        return false;
    }
    mCensusStamp = 0;
    return true;
}

// Drop everything; in-flight worker results are refused by the generation bump.
void SSNavMesh::teardown()
{
    ++mGeneration;
    if (mQuery) { dtFreeNavMeshQuery(mQuery); mQuery = nullptr; }
    if (mTileCache) { dtFreeTileCache(mTileCache); mTileCache = nullptr; }
    if (mNavMesh) { dtFreeNavMesh(mNavMesh); mNavMesh = nullptr; }
    mBands.clear();
    mColumns.clear();
    mWorklist.clear();
    mObstacles.clear();
    mObstacleAdds.clear();
    mObstacleRemovals.clear();
    mLayerBytes = 0;
    mCensusStamp = 0;
}

// ---------------------------------------------------------------------------- per frame

void SSNavMesh::update()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSNavMesh", false);
    static LLCachedControl<bool> census_enabled(gSavedSettings, "SSWorldFieldShapes", false);
    static LLCachedControl<U32> builds_per_frame(gSavedSettings, "SSNavMeshBuildsPerFrame", 2);
    static LLCachedControl<U32> max_in_flight(gSavedSettings, "SSNavMeshMaxInFlight", 2);
    if (!enabled || !census_enabled)
    {
        if (mNavMesh) teardown();
        return;
    }
    LL_RECORD_BLOCK_TIME(FTM_SS_NAVMESH_UPDATE);
    if (!ensureInit()) return;

    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();
    if (shapes->censusCurrent())
    {
        const U64 stamp = shapes->censusStamp();
        if (stamp != mCensusStamp)
        {
            mCensusStamp = stamp;
            schedule();
            syncObstacles();
        }
    }

    U32 launched = 0;
    while (launched < (U32)builds_per_frame && mInFlight < (S32)max_in_flight && !mWorklist.empty())
    {
        const Job job = mWorklist.back();
        mWorklist.pop_back();
        launch(job);
        ++launched;
    }

    // <SS:Nexii> One update per frame builds at most one obstacle-affected tile from its cached layer (0.26 ms in the benchmark), so movers cost a bounded slice whatever their count. [interaction: DYNAMIC]
    pumpObstacles();
    bool up_to_date = false;
    mTileCache->update(0.f, mNavMesh, &up_to_date);
}

// Bands off the census: every store-bound record nominates the columns its AABB meets with its z interval; the
// intervals of a column merge into bands across gaps smaller than SSNavMeshBandGap; a band whose geometry signature
// changed since it was published becomes a job. Columns and bands that vanished are evicted.
void SSNavMesh::schedule()
{
    static LLCachedControl<F32> range(gSavedSettings, "SSWorldFieldShapesRange", 192.f);
    static LLCachedControl<F32> band_gap(gSavedSettings, "SSNavMeshBandGap", 8.f);
    const F32 env = llclamp((F32)range, 32.f, 1024.f);
    const F32 gap = llmax((F32)band_gap, 2.f);
    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();
    const LLVector3 anchor = toLocal(shapes->censusAnchor());
    const LLVector3 bmin = shapes->censusAnchor() - LLVector3(env, env, env);
    const LLVector3 bmax = shapes->censusAnchor() + LLVector3(env, env, env);
    const LLVector3 off = toLocal(LLVector3::zero);      // agent -> local offset

    struct Interval { F32 mLo, mHi; U64 mSig; };
    std::map<U64, std::vector<Interval> > columns;
    const F32 inv = 1.f / TILE_M;
    const S32 cx0 = (S32)floorf((anchor.mV[VX] - env) * inv), cx1 = (S32)floorf((anchor.mV[VX] + env) * inv);
    const S32 cy0 = (S32)floorf((anchor.mV[VY] - env) * inv), cy1 = (S32)floorf((anchor.mV[VY] + env) * inv);

    shapes->forEachRecord(bmin, bmax, [&](const SSWorldFieldShapes::Record& rec)
    {
        if (rec.mDynamic || rec.mLayer == SSWorldFieldShapes::LAYER_DECLARED_PHANTOM) return;
        const LLVector3 lo = rec.mBMin + off, hi = rec.mBMax + off;
        const S32 x0 = llmax(cx0, (S32)floorf(lo.mV[VX] * inv)), x1 = llmin(cx1, (S32)floorf(hi.mV[VX] * inv));
        const S32 y0 = llmax(cy0, (S32)floorf(lo.mV[VY] * inv)), y1 = llmin(cy1, (S32)floorf(hi.mV[VY] * inv));
        if (x1 < x0 || y1 < y0) return;
        if ((S64)(x1 - x0 + 1) * (y1 - y0 + 1) > 256) return;    // a kilometre-scale surround is not navigable structure
        U64 sig = 1469598103934665603ull;
        for (U32 c = 0; c < 3; ++c) { sig = fnvF(sig, lo.mV[c]); sig = fnvF(sig, hi.mV[c]); }
        sig = fnv(sig, (U64)rec.mClass | ((U64)rec.mProv << 8) | ((U64)rec.tris().size() << 16));
        for (S32 y = y0; y <= y1; ++y) for (S32 x = x0; x <= x1; ++x)
        {
            columns[columnKey(x, y)].push_back(Interval{lo.mV[VZ], hi.mV[VZ], sig});
        }
    });

    // Terrain: every column in the envelope carries its land interval, hashed from a coarse sample (the build
    // samples the full metre grid; scheduling only needs to notice change, and 268k land lookups per census did not).
    for (S32 y = cy0; y <= cy1; ++y) for (S32 x = cx0; x <= cx1; ++x)
    {
        F32 lo = FLT_MAX, hi = -FLT_MAX;
        U64 sig = 14695981039346656037ull;
        bool any = false;
        // The coarse grid shifts by a metre per schedule, so every land node is sampled within four censuses and a
        // small edit between samples cannot stay invisible.
        const S32 phase = (S32)(mScheduleCount % (U32)SS_NAV_TERRAIN_SCHEDULE_STEP);
        for (S32 gy = phase; gy < SS_NAV_TERRAIN_NODES; gy += SS_NAV_TERRAIN_SCHEDULE_STEP) for (S32 gx = phase; gx < SS_NAV_TERRAIN_NODES; gx += SS_NAV_TERRAIN_SCHEDULE_STEP)
        {
            F32 z;
            if (!terrainZLocal((F32)x * TILE_M - SS_NAV_TERRAIN_MARGIN_M + (F32)gx, (F32)y * TILE_M - SS_NAV_TERRAIN_MARGIN_M + (F32)gy, z)) continue;
            any = true;
            lo = llmin(lo, z); hi = llmax(hi, z);
            sig = fnvF(sig, z);
        }
        if (any) columns[columnKey(x, y)].push_back(Interval{lo, hi, sig});
    }

    ++mScheduleCount;
    for (auto& kv : mBands) kv.second.mAlive = false;
    mWorklist.clear();
    std::unordered_map<U64, S32> new_columns;
    for (auto& kv : columns)
    {
        std::vector<Interval>& iv = kv.second;
        std::sort(iv.begin(), iv.end(), [](const Interval& a, const Interval& b) { return a.mLo < b.mLo; });
        const S32 tx = (S32)((kv.first >> 42) & 0x1FFFFF) - (1 << 20);
        const S32 ty = (S32)((kv.first >> 21) & 0x1FFFFF) - (1 << 20);
        std::vector<Interval> bands;
        for (const Interval& i : iv)
        {
            if (!bands.empty() && i.mLo - bands.back().mHi < gap)
            {
                bands.back().mHi = llmax(bands.back().mHi, i.mHi);
                bands.back().mSig = fnv(bands.back().mSig, i.mSig);
            }
            else if ((S32)bands.size() < MAX_BANDS)
            {
                bands.push_back(Interval{i.mLo, i.mHi, fnv(1469598103934665603ull, i.mSig)});
            }
            else
            {
                bands.back().mHi = llmax(bands.back().mHi, i.mHi);     // over the band budget: fold into the top band
                bands.back().mSig = fnv(bands.back().mSig, i.mSig);
            }
        }
        new_columns[kv.first] = (S32)bands.size();
        for (size_t b = 0; b < bands.size(); ++b)
        {
            const U64 key = bandKey(tx, ty, (S32)b);
            Band& band = mBands[key];
            band.mAlive = true;
            const U64 sig = fnvF(fnvF(bands[b].mSig, bands[b].mLo), bands[b].mHi);
            if (band.mSig == sig && !band.mRefs.empty()) continue;
            Job job;
            job.mTx = tx; job.mTy = ty; job.mBand = (S32)b;
            job.mZMin = bands[b].mLo - 1.f; job.mZMax = bands[b].mHi + 1.f;
            job.mSig = sig;
            mWorklist.push_back(job);
        }
    }
    mColumns.swap(new_columns);

    for (auto it = mBands.begin(); it != mBands.end();)
    {
        if (it->second.mAlive) { ++it; continue; }
        removeBand(it->first, it->second);
        it = mBands.erase(it);
    }

    // Ground-up, nearest-first: the bands the agent stands in publish first.
    std::sort(mWorklist.begin(), mWorklist.end(), [&](const Job& a, const Job& b)
    {
        const F32 da = fabsf((F32)a.mTx * TILE_M + TILE_M * 0.5f - anchor.mV[VX]) + fabsf((F32)a.mTy * TILE_M + TILE_M * 0.5f - anchor.mV[VY]) + fabsf(a.mZMin - anchor.mV[VZ]);
        const F32 db = fabsf((F32)b.mTx * TILE_M + TILE_M * 0.5f - anchor.mV[VX]) + fabsf((F32)b.mTy * TILE_M + TILE_M * 0.5f - anchor.mV[VY]) + fabsf(b.mZMin - anchor.mV[VZ]);
        return da > db;     // back() pops first
    });
}

// Evict a band's published layers from both the tile cache and the navmesh.
void SSNavMesh::removeBand(U64 key, Band& band)
{
    (void)key;
    if (!mTileCache || !mNavMesh) { band.mRefs.clear(); return; }
    for (U32 ref : band.mRefs)
    {
        const dtCompressedTile* tile = mTileCache->getTileByRef(ref);
        if (tile && tile->header)
        {
            mNavMesh->removeTile(mNavMesh->getTileRefAt(tile->header->tx, tile->header->ty, tile->header->tlayer), nullptr, nullptr);
            mLayerBytes -= (size_t)tile->dataSize;
        }
        mTileCache->removeTile(ref, nullptr, nullptr);
    }
    band.mRefs.clear();
}

// Snapshot the band's geometry on the main thread, hand the Recast pipeline to the General queue, publish on return.
void SSNavMesh::launch(const Job& job)
{
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue) return;

    static LLCachedControl<F32> agent_height(gSavedSettings, "SSNavMeshAgentHeight", 2.f);
    static LLCachedControl<F32> agent_radius(gSavedSettings, "SSNavMeshAgentRadius", 0.5f);
    static LLCachedControl<F32> agent_climb(gSavedSettings, "SSNavMeshAgentClimb", 0.75f);
    static LLCachedControl<F32> agent_slope(gSavedSettings, "SSNavMeshAgentSlope", 45.f);

    std::shared_ptr<SSNavBuildInput> in = std::make_shared<SSNavBuildInput>();
    in->mTx = job.mTx;
    in->mTyDetour = -job.mTy - 1;               // Detour tile y runs along Recast z = -local y
    in->mBand = job.mBand;
    in->mAgentHeight = agent_height; in->mAgentRadius = agent_radius; in->mAgentClimb = agent_climb; in->mAgentSlope = agent_slope;
    in->mMin[0] = (F32)job.mTx * TILE_M; in->mMin[1] = (F32)job.mTy * TILE_M; in->mMin[2] = job.mZMin;
    in->mMax[0] = in->mMin[0] + TILE_M;  in->mMax[1] = in->mMin[1] + TILE_M;  in->mMax[2] = job.mZMax;

    const F32 border = (ceilf(agent_radius / CELL) + 3.f) * CELL;
    const LLVector3 off = toLocal(LLVector3::zero);
    const LLVector3 gmin_agent = LLVector3(in->mMin[0] - border, in->mMin[1] - border, in->mMin[2]) - off;
    const LLVector3 gmax_agent = LLVector3(in->mMax[0] + border, in->mMax[1] + border, in->mMax[2]) - off;
    SSWorldFieldShapes::getInstance()->forEachRecord(gmin_agent, gmax_agent, [&](const SSWorldFieldShapes::Record& rec)
    {
        if (rec.mDynamic || rec.mLayer == SSWorldFieldShapes::LAYER_DECLARED_PHANTOM) return;
        emitRecord(rec, off, in->mSoup);
    });

    // Terrain heights for the bordered column, void marked below -900 so the worker skips those cells.
    std::vector<F32> heights((size_t)SS_NAV_TERRAIN_NODES * SS_NAV_TERRAIN_NODES, -1000.f);
    const F32 tx0 = in->mMin[0] - SS_NAV_TERRAIN_MARGIN_M, ty0 = in->mMin[1] - SS_NAV_TERRAIN_MARGIN_M;
    bool terrain_in_band = false;
    for (S32 gy = 0; gy < SS_NAV_TERRAIN_NODES; ++gy) for (S32 gx = 0; gx < SS_NAV_TERRAIN_NODES; ++gx)
    {
        F32 z;
        if (!terrainZLocal(tx0 + (F32)gx, ty0 + (F32)gy, z)) continue;
        heights[gy * SS_NAV_TERRAIN_NODES + gx] = z;
        if (z >= in->mMin[2] - 1.f && z <= in->mMax[2] + 1.f) terrain_in_band = true;
    }
    if (terrain_in_band) emitTerrain(heights.data(), tx0, ty0, in->mSoup);

    const U32 generation = mGeneration;
    SSNavCompressor* comp = &mImpl->mCompressor;
    ++mInFlight;
    const bool posted = main_queue->postTo(
        general_queue,
        [in, comp, job, generation]() -> std::shared_ptr<Result>
        {
            std::shared_ptr<Result> r = std::make_shared<Result>();
            r->mJob = job;
            r->mGeneration = generation;
            LLTimer t;
            buildBand(*in, *comp, r->mLayers);
            r->mMS = t.getElapsedTimeF32() * 1000.f;
            r->mOk = true;
            return r;
        },
        [this](std::shared_ptr<Result> r)
        {
            publish(r);
        });
    if (!posted) --mInFlight;
}

// Main thread: swap the band's layers into the tile cache and rebuild its navmesh tiles from them.
void SSNavMesh::publish(const std::shared_ptr<Result>& result)
{
    --mInFlight;
    if (!result || result->mGeneration != mGeneration || !mTileCache || !mNavMesh) return;
    LL_RECORD_BLOCK_TIME(FTM_SS_NAVMESH_PUBLISH);
    const Job& job = result->mJob;
    auto it = mBands.find(bandKey(job.mTx, job.mTy, job.mBand));
    if (it == mBands.end()) return;                     // evicted while building
    Band& band = it->second;
    removeBand(it->first, band);
    band.mSig = job.mSig;
    band.mZMin = job.mZMin;
    band.mZMax = job.mZMax;
    for (const std::vector<U8>& layer : result->mLayers)
    {
        unsigned char* data = (unsigned char*)dtAlloc((size_t)layer.size(), DT_ALLOC_PERM);
        if (!data) continue;
        memcpy(data, layer.data(), layer.size());
        dtCompressedTileRef ref = 0;
        if (dtStatusFailed(mTileCache->addTile(data, (int)layer.size(), DT_COMPRESSEDTILE_FREE_DATA, &ref)))
        {
            dtFree(data);
            continue;
        }
        band.mRefs.push_back(ref);
        mLayerBytes += layer.size();
    }
    mTileCache->buildNavMeshTilesAt(job.mTx, -job.mTy - 1, mNavMesh);
    mLastBuildMS = result->mMS;
    ++mBuildCount;
}

// Movers: every DYNAMIC record in the envelope wants a box obstacle. The desired set is diffed against the live one
// per census (a mover whose box has not changed keeps its obstacle), and the adds and removals drain through
// pumpObstacles under the tile cache's request budget rather than all at once.
void SSNavMesh::syncObstacles()
{
    if (!mTileCache) return;
    static LLCachedControl<F32> range(gSavedSettings, "SSWorldFieldShapesRange", 192.f);
    const F32 env = llclamp((F32)range, 32.f, 1024.f);
    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();
    const LLVector3 bmin = shapes->censusAnchor() - LLVector3(env, env, env);
    const LLVector3 bmax = shapes->censusAnchor() + LLVector3(env, env, env);
    const LLVector3 off = toLocal(LLVector3::zero);

    std::vector<Obstacle> desired;
    shapes->forEachRecord(bmin, bmax, [&](const SSWorldFieldShapes::Record& rec)
    {
        if (!rec.mDynamic || rec.mLayer == SSWorldFieldShapes::LAYER_DECLARED_PHANTOM) return;
        if ((S32)desired.size() >= SS_NAV_MAX_OBSTACLES) return;
        const LLVector3 lo = rec.mBMin + off, hi = rec.mBMax + off;
        Obstacle o;
        o.mMin[0] = lo.mV[VX]; o.mMin[1] = lo.mV[VZ]; o.mMin[2] = -hi.mV[VY];
        o.mMax[0] = hi.mV[VX]; o.mMax[1] = hi.mV[VZ]; o.mMax[2] = -lo.mV[VY];
        desired.push_back(o);
    });

    // Match on the quantized box: unchanged movers keep their obstacle, everything else churns.
    auto same = [](const Obstacle& a, const Obstacle& b)
    {
        for (S32 c = 0; c < 3; ++c)
        {
            if (fabsf(a.mMin[c] - b.mMin[c]) > 0.05f || fabsf(a.mMax[c] - b.mMax[c]) > 0.05f) return false;
        }
        return true;
    };
    std::vector<bool> claimed(desired.size(), false);
    std::vector<Obstacle> kept;
    for (const Obstacle& live : mObstacles)
    {
        bool matched = false;
        for (size_t i = 0; i < desired.size(); ++i)
        {
            if (claimed[i] || !same(live, desired[i])) continue;
            claimed[i] = true;
            matched = true;
            break;
        }
        if (matched) kept.push_back(live);
        else mObstacleRemovals.push_back(live.mRef);
    }
    // Adds still queued from an earlier census are superseded by this census's view.
    mObstacleAdds.clear();
    for (size_t i = 0; i < desired.size(); ++i)
    {
        if (!claimed[i]) mObstacleAdds.push_back(desired[i]);
    }
    mObstacles.swap(kept);
}

// Hand queued obstacle removals and adds to the tile cache, at most the request budget per frame; a refused
// request (queue full) simply waits for the next frame.
void SSNavMesh::pumpObstacles()
{
    if (!mTileCache) return;
    S32 budget = SS_NAV_OBSTACLE_REQUESTS_PER_FRAME;
    while (budget > 0 && !mObstacleRemovals.empty())
    {
        if (dtStatusFailed(mTileCache->removeObstacle(mObstacleRemovals.back()))) return;
        mObstacleRemovals.pop_back();
        --budget;
    }
    while (budget > 0 && !mObstacleAdds.empty())
    {
        Obstacle o = mObstacleAdds.back();
        dtObstacleRef ref = 0;
        if (dtStatusFailed(mTileCache->addBoxObstacle(o.mMin, o.mMax, &ref))) return;
        mObstacleAdds.pop_back();
        o.mRef = ref;
        mObstacles.push_back(o);
        --budget;
    }
}

// ---------------------------------------------------------------------------- queries

bool SSNavMesh::nearestPoint(const LLVector3& pos_agent, F32 reach, LLVector3& out_agent) const
{
    if (!mQuery) return false;
    const LLVector3 l = toLocal(pos_agent);
    const float centre[3] = {l.mV[VX], l.mV[VZ], -l.mV[VY]};
    const float ext[3] = {reach, reach * 2.f, reach};
    dtQueryFilter filter;
    dtPolyRef ref = 0;
    float pt[3];
    if (dtStatusFailed(mQuery->findNearestPoly(centre, ext, &filter, &ref, pt)) || !ref) return false;
    out_agent = fromLocal(LLVector3(pt[0], -pt[2], pt[1]));
    return true;
}

bool SSNavMesh::onNavMesh(const LLVector3& pos_agent, F32 reach) const
{
    LLVector3 p;
    return nearestPoint(pos_agent, reach, p);
}

bool SSNavMesh::findPath(const LLVector3& from_agent, const LLVector3& to_agent, std::vector<LLVector3>& out_points, bool& out_partial) const
{
    out_points.clear();
    out_partial = false;
    if (!mQuery) return false;
    const LLVector3 a = toLocal(from_agent), b = toLocal(to_agent);
    const float sp[3] = {a.mV[VX], a.mV[VZ], -a.mV[VY]};
    const float ep[3] = {b.mV[VX], b.mV[VZ], -b.mV[VY]};
    const float ext[3] = {2.f, 4.f, 2.f};
    dtQueryFilter filter;
    dtPolyRef sref = 0, eref = 0;
    float snear[3], enear[3];
    if (dtStatusFailed(mQuery->findNearestPoly(sp, ext, &filter, &sref, snear)) || !sref) return false;
    if (dtStatusFailed(mQuery->findNearestPoly(ep, ext, &filter, &eref, enear)) || !eref) return false;
    dtPolyRef polys[SS_NAV_MAX_PATH];
    int npolys = 0;
    const dtStatus st = mQuery->findPath(sref, eref, snear, enear, &filter, polys, &npolys, SS_NAV_MAX_PATH);
    if (dtStatusFailed(st) || npolys == 0) return false;
    out_partial = (st & DT_PARTIAL_RESULT) != 0;
    float target[3] = {enear[0], enear[1], enear[2]};
    if (out_partial) mQuery->closestPointOnPoly(polys[npolys - 1], enear, target, nullptr);
    float straight[SS_NAV_MAX_PATH * 3];
    unsigned char flags[SS_NAV_MAX_PATH];
    dtPolyRef straight_refs[SS_NAV_MAX_PATH];
    int nstraight = 0;
    mQuery->findStraightPath(snear, target, polys, npolys, straight, flags, straight_refs, &nstraight, SS_NAV_MAX_PATH);
    for (int i = 0; i < nstraight; ++i)
    {
        out_points.push_back(fromLocal(LLVector3(straight[i * 3], -straight[i * 3 + 2], straight[i * 3 + 1])));
    }
    return nstraight > 0;
}

U32 SSNavMesh::polyCount() const
{
    if (!mNavMesh) return 0;
    U32 n = 0;
    const dtNavMesh* nm = mNavMesh;
    for (int i = 0; i < nm->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = nm->getTile(i);
        if (tile && tile->header) n += (U32)tile->header->polyCount;
    }
    return n;
}

// ---------------------------------------------------------------------------- overlay

void SSNavMesh::renderDebug()
{
    if (!mNavMesh) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const dtNavMesh* nm = mNavMesh;

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::LINES);
    for (int i = 0; i < nm->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = nm->getTile(i);
        if (!tile || !tile->header) continue;
        const LLVector3 tmin = fromLocal(LLVector3(tile->header->bmin[0], -tile->header->bmax[2], tile->header->bmin[1]));
        const LLVector3 tmax = fromLocal(LLVector3(tile->header->bmax[0], -tile->header->bmin[2], tile->header->bmax[1]));
        if (((tmin + tmax) * 0.5f - cam).magVec() > 256.f) continue;
        // Hue by layer so stacked bands read separately, the same convention as the world field's band view.
        const F32 hue = (F32)(tile->header->layer % 8) / 8.f;
        gGL.color4f(0.3f + 0.7f * hue, 0.9f - 0.6f * hue, 0.4f, 0.6f);
        for (int p = 0; p < tile->header->polyCount; ++p)
        {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            for (int v = 0; v < (int)poly.vertCount; ++v)
            {
                const float* a = &tile->verts[poly.verts[v] * 3];
                const float* b = &tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3];
                const LLVector3 pa = fromLocal(LLVector3(a[0], -a[2], a[1] + 0.05f));
                const LLVector3 pb = fromLocal(LLVector3(b[0], -b[2], b[1] + 0.05f));
                gGL.vertex3fv(pa.mV);
                gGL.vertex3fv(pb.mV);
            }
        }
    }
    gGL.end();
}
