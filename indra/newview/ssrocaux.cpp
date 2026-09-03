/**
 * @file ssrocaux.cpp
 * @brief Region Object Cache auxiliary data: capture and instant re-apply of terrain, water and composition - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssrocaux.h"

#include "ssrocghost.h"
#include "ssrocledger.h"
#include "ssrocvocache.h"

#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvlcomposition.h"
#include "llworld.h"

#include <vector>

namespace
{
    // Sanity bounds for a cached heightmap. LLSurface::mMaxZ is a one-way llmax latch, so relief that is wildly out of range would permanently inflate the region's own bounds even after the real terrain arrives.
    const F32 SSROC_MIN_PLAUSIBLE_Z = -4096.f;
    const F32 SSROC_MAX_PLAUSIBLE_Z =  8192.f;

    bool ssIsFinite(F32 v) { return v == v && v >= SSROC_MIN_PLAUSIBLE_Z && v <= SSROC_MAX_PLAUSIBLE_Z; }
}

SSROCAuxMgr::SSROCAuxMgr()
{
}

SSROCAuxMgr::~SSROCAuxMgr()
{
}

void SSROCAuxMgr::shutdown()
{
    if (mMetrics.mRegionsLoaded || mMetrics.mCaptured || mMetrics.mRegionsMissing)
    {
        LL_INFOS("SSROC") << metricsString() << LL_ENDL;

        // Reported from here rather than from a manager of its own: backing the object cache has no per-frame state and no lifecycle beyond one region visit, so a whole singleton for two counters would be machinery for its own sake.
        LL_INFOS("SSROC") << ssROCVOCacheMetricsString() << LL_ENDL;
    }

    // The ledger and the ghost manager share this manager's lifecycle rather than owning hooks of their own in llappviewer, so they are retired here - after the last region removal has already run its promotion pass.
    if (SSROCGhostMgr::instanceExists())
    {
        SSROCGhostMgr::instance().shutdown();
    }
    if (SSROCLedger::instanceExists())
    {
        SSROCLedger::instance().shutdown();
    }

    mRegions.clear();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SSROCAuxMgr::onRegionAdded(LLViewerRegion* regionp)
{
    if (!regionp || !SSROCStore::enabled()) return;

    // The store is created lazily rather than only at startup, so enabling ROC at the login screen or in preferences works without a restart.
    SSROCStore::instance().ensureInitialized();

    const U64 handle = regionp->getHandle();
    if (mRegions.find(handle) != mRegions.end()) return;   // already tracking this region this session

    mRegions[handle] = RegionState();

    // Phase 2a. The ledger tracks the same region under the same handle and takes its record set from the file this load is about to produce.
    SSROCLedger::instance().onRegionAdded(regionp);

    SSROCStore::instance().loadRegionAsync(handle,
        [this](U64 loaded_handle, SSROCFilePtr file)
        {
            // Main thread. The region this was kicked for may already be gone, so nothing here dereferences a cached pointer - completeLoad re-resolves by handle.
            completeLoad(loaded_handle, file);
        });
}

void SSROCAuxMgr::completeLoad(U64 handle, SSROCFilePtr file)
{
    auto it = mRegions.find(handle);
    if (it == mRegions.end()) return;

    // Dropped rather than merged. The blocking fallback and the worker read can both deliver a copy of the same file, and handing the ledger two copies of one record set would replay every confirmation the merge path performs for records already sighted this visit.
    if (it->second.mLoadDone) return;

    it->second.mFile = file;
    it->second.mLoadDone = true;

    // Hand the record set to the ledger. Sightings can and do arrive before the disk read completes, so the ledger merges rather than adopts.
    if (SSROCLedger::instanceExists())
    {
        SSROCLedger::instance().onRegionFileLoaded(handle, file);
    }

    // And to the ghost manager, which holds it until the handshake says whether the stored local ids still name the same objects. Nothing is painted from here: injection is frame-budgeted and belongs to the tick.
    if (SSROCGhostMgr::enabled())
    {
        SSROCGhostMgr::instance().onRegionFileLoaded(handle, file);
    }

    if (file)
    {
        ++mMetrics.mRegionsLoaded;
        LL_INFOS("SSROC") << "Loaded region cache for " << handle
                          << " (" << file->mRecords.size() << " objects, heightmap "
                          << (file->mAux.mHeightmap.empty() ? "absent" : "present") << ")" << LL_ENDL;
    }
    else
    {
        ++mMetrics.mRegionsMissing;
        LL_DEBUGS("SSROC") << "No region cache on disk for " << handle << LL_ENDL;
    }

    applyPending(handle);
}

bool SSROCAuxMgr::ensureRegionLoaded(U64 handle)
{
    auto it = mRegions.find(handle);
    if (it == mRegions.end()) return false;
    if (it->second.mLoadDone) return it->second.mFile != NULL;

    // The worker read is still outstanding and cannot be waited on without blocking the main thread on a queue, so the file is read again here instead. Reading the same file twice is safe by construction: saves go through a temp file and a rename, so a reader never sees a partial write, and whichever copy arrives second is dropped by completeLoad.
    LLTimer timer;
    SSROCFilePtr file = SSROCStore::instance().loadRegionBlocking(handle);
    const F32 ms = timer.getElapsedTimeF32() * 1000.f;

    // Logged every time rather than only when slow, because this is the one piece of main-thread IO the region object cache performs and the cost of it is a number the owner has to be able to see rather than infer.
    LL_INFOS("SSROC") << "Region " << handle << ": the cache read had not landed by the handshake, so it was read inline in "
                      << ms << " ms (" << (file ? file->mRecords.size() : 0) << " records)" << LL_ENDL;

    completeLoad(handle, file);
    return file != NULL;
}

void SSROCAuxMgr::onRegionHandshake(LLViewerRegion* regionp, F32 water_height, const LLUUID& cache_id)
{
    if (!regionp || !SSROCStore::enabled()) return;

    auto it = mRegions.find(regionp->getHandle());
    if (it == mRegions.end())
    {
        // ROC was switched on between this region being added and its handshake arriving, so the region was never tracked and its cache id - the one thing that says whether stored local ids still mean anything - would be lost for the whole visit. Starting to track it here recovers everything except the head start on the disk read.
        LL_INFOS("SSROC") << "Region " << regionp->getHandle() << ": handshake arrived for an untracked region, so the cache was enabled after the region was added - starting to track it now" << LL_ENDL;
        onRegionAdded(regionp);

        it = mRegions.find(regionp->getHandle());
        if (it == mRegions.end()) return;
    }

    // The sim has now spoken, so its values are authoritative and are what gets saved. The cache id decides whether this is the same region content we saw last time.
    it->second.mHandshakeWater = water_height;
    it->second.mCacheID        = cache_id;
    it->second.mHaveHandshake  = true;

    // The ghost manager needs the same cache id for a different reason: it is the only thing that says whether the local ids stored beside the records still name the same objects, which is what separates a region we can repaint from one whose simulator has restarted since.
    if (SSROCGhostMgr::enabled())
    {
        SSROCGhostMgr::instance().onRegionHandshake(regionp, cache_id);
    }

    // And the ledger needs it for a third reason: the file stores one cache id for a whole record set, so after a visit whose id changed it carries the new id beside records still keyed to the old one. Marking those is what stops the visit AFTER a restart reading as mode A and painting remembered objects at ids that now belong to somebody else - a hole that is live today, with no mode B built at all.
    if (SSROCLedger::instanceExists())
    {
        SSROCLedger::instance().onRegionHandshake(regionp, cache_id);
    }

    // A cached apply that lost the race to the handshake is not a problem, but composition and terrain may still be pending if the disk read was slower than the handshake.
    applyPending(regionp->getHandle());
}

void SSROCAuxMgr::onWaterRebuilt(LLViewerRegion* regionp)
{
    if (!regionp || !SSROCStore::enabled()) return;

    auto it = mRegions.find(regionp->getHandle());
    if (it == mRegions.end()) return;

    // A rebuilt water object is parked at the default height, so whichever value we consider authoritative has to be pushed again. The handshake value wins when we have it; otherwise the cached one keeps the sea at the right level until the sim says otherwise.
    if (it->second.mHaveHandshake)
    {
        regionp->setWaterHeight(it->second.mHandshakeWater);
    }
    else if (it->second.mFile && it->second.mWaterApplied)
    {
        regionp->setWaterHeight(it->second.mFile->mAux.mWaterHeight);
    }
}

void SSROCAuxMgr::onRegionRemoved(LLViewerRegion* regionp)
{
    if (!regionp || !SSROCStore::enabled()) return;

    SSROCStore::instance().ensureInitialized();

    const U64 handle = regionp->getHandle();

    // The ghost manager is retired FIRST and unconditionally, above every early return below. It has to hand its created-but-unconfirmed cache entries to the save-time purge, and that purge runs inside the region destructor a few calls further on - a region whose disk load never landed still tears down through here and still owes that list.
    const bool ghosts_trashed = SSROCGhostMgr::instanceExists() && SSROCGhostMgr::instance().isRegionTrashed(handle);
    if (SSROCGhostMgr::instanceExists())
    {
        SSROCGhostMgr::instance().onRegionRemoved(regionp);
    }

    auto it = mRegions.find(handle);

    // A region we never tracked this session has nothing to carry forward, and writing a fresh file here would replace the region's accumulated cache with an aux-only stub. Reachable whenever ROC was enabled after the region was already added.
    if (it == mRegions.end()) return;

    // If the disk load has not landed yet there is nothing to carry forward EITHER, and saving here would rename a single visit's fragment over months of accumulated tenure and a complete heightmap. The load only completes synchronously when it fails, so any short visit - a teleport soon after arriving, a fast flight along a region chain, a login while the worker pool is busy - can reach this point with the file still in flight.
    if (!it->second.mLoadDone)
    {
        LL_INFOS("SSROC") << "Not saving region " << handle << ": its cache was still loading, so this visit has nothing to merge into" << LL_ENDL;
        mRegions.erase(it);
        return;
    }

    SSROCFilePtr file = std::make_shared<SSROCRegionFile>();
    if (it->second.mFile)
    {
        // Carry the existing file forward so the object ledger written by later phases is preserved; only the aux section is refreshed here.
        *file = *it->second.mFile;
    }

    file->mRegionHandle = handle;
    file->mRegionID     = regionp->getRegionID();
    file->mSavedAt      = (U64)time(NULL);
    ++file->mSessionsSeen;

    if (it->second.mHaveHandshake)
    {
        file->mLastCacheID = it->second.mCacheID;
    }

    // Only re-pack the relief when the simulator has replaced every patch we painted; otherwise keep what the file already holds.
    const bool keep_cached_heightmap = !file->mAux.mHeightmap.empty() && !terrainIsSimSourced(regionp, it->second);

    const bool aux_ok = captureAux(regionp, file->mAux, keep_cached_heightmap);
    if (aux_ok)
    {
        ++mMetrics.mCaptured;
        LL_INFOS("SSROC") << "Captured aux for region " << handle
                          << " (" << file->mAux.mHeightmapGridsPerEdge << "^2 heights, water " << file->mAux.mWaterHeight
                          << ", composition " << (file->mAux.mComposition.mValid ? "yes" : "no") << ")" << LL_ENDL;
    }

    // The object ledger runs its promotion pass and merges itself into this same file. It has to be able to force the save on its own: terrain capture refuses on a half-streamed region, and a visit that recorded thousands of objects must not be discarded because the relief was incomplete.
    bool ledger_ok = false;
    if (SSROCLedger::instanceExists())
    {
        ledger_ok = SSROCLedger::instance().onRegionRemoved(regionp, *file);
    }

    // A region whose ghosts were mass-refuted on arrival keeps its terrain and loses its objects, exactly as a sandbox does. Kills refute the build, not the relief - and the ledger still runs above, so its own per-region state is retired rather than leaked.
    if (ghosts_trashed)
    {
        file->mRecords.clear();
        file->mManifest.clear();
        ledger_ok = true;
    }

    if (aux_ok || ledger_ok)
    {
        SSROCStore::instance().saveRegionAsync(file);
    }

    mRegions.erase(it);
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

void SSROCAuxMgr::applyPending(U64 handle)
{
    auto it = mRegions.find(handle);
    if (it == mRegions.end() || !it->second.mLoadDone || !it->second.mFile) return;

    LLViewerRegion* regionp = LLWorld::instanceExists() ? LLWorld::getInstance()->getRegionFromHandle(handle) : NULL;
    if (!regionp) return;

    RegionState& state = it->second;
    const SSROCAux& aux = state.mFile->mAux;

    // The region id is checked rather than assumed: a handle can be reused by a different region on a different grid session, and painting one region's relief onto another would be both wrong and hard to diagnose.
    if (state.mFile->mRegionID.notNull() && regionp->getRegionID().notNull()
        && state.mFile->mRegionID != regionp->getRegionID())
    {
        return;
    }

    if (!state.mWaterApplied && applyWater(regionp, aux, state))
    {
        state.mWaterApplied = true;
        ++mMetrics.mWaterApplied;
    }

    if (!state.mCompApplied && applyComposition(regionp, aux))
    {
        state.mCompApplied = true;
        ++mMetrics.mCompApplied;
    }

    if (!state.mTerrainApplied && applyTerrain(regionp, aux, state))
    {
        state.mTerrainApplied = true;
        ++mMetrics.mTerrainApplied;
    }
}

bool SSROCAuxMgr::applyWater(LLViewerRegion* regionp, const SSROCAux& aux, RegionState& state)
{
    // Once the sim has told us the water height there is nothing to restore - the handshake value is already in place and is authoritative.
    if (state.mHaveHandshake) return false;
    if (!ssIsFinite(aux.mWaterHeight)) return false;

    regionp->setWaterHeight(aux.mWaterHeight);
    return true;
}

bool SSROCAuxMgr::applyComposition(LLViewerRegion* regionp, const SSROCAux& aux)
{
    if (!aux.mComposition.mValid) return false;

    LLVLComposition* compp = regionp->getComposition();
    if (!compp) return false;

    // Nothing to do once the handshake has already set the real values - getParamsReady is exactly the "the sim has spoken" flag.
    if (compp->getParamsReady()) return false;

    for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i)
    {
        if (aux.mComposition.mDetail[i].isNull()) return false;   // an incomplete set would leave the composition unable to generate materials at all
        if (!ssIsFinite(aux.mComposition.mStartHeight[i]) || !ssIsFinite(aux.mComposition.mHeightRange[i])) return false;
    }

    for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i)
    {
        compp->setDetailAssetID(i, aux.mComposition.mDetail[i]);
        compp->setStartHeight(i, aux.mComposition.mStartHeight[i]);
        compp->setHeightRange(i, aux.mComposition.mHeightRange[i]);
    }

    // This is the switch that lets the composition start fetching its textures, which is the actual latency being removed - the ground stops being grey before the handshake has even arrived.
    compp->setParamsReady();
    return true;
}

bool SSROCAuxMgr::applyTerrain(LLViewerRegion* regionp, const SSROCAux& aux, RegionState& state)
{
    if (aux.mHeightmap.empty()) return false;

    LLSurface& land = regionp->getLand();

    const S32 ppe  = land.getPatchesPerEdge();
    const S32 gpe  = land.getGridsPerEdge();          // one larger than the region grid: the extra row and column are the north and east buffer
    const S32 gppe = land.getGridsPerPatchEdge();
    const S32 W    = gpe - 1;

    if (ppe <= 0 || W <= 0 || gppe <= 0) return false;

    // A region that changed size or patch layout must fall back to the LayerData stream rather than paint mismatched relief.
    if (aux.mHeightmapGridsPerEdge != (U32)W) { ++mMetrics.mTerrainSkipped; return false; }
    if (aux.mHeightmapGridsPerPatchEdge != (U16)gppe) { ++mMetrics.mTerrainSkipped; return false; }
    if (!ssIsFinite(aux.mHeightmapMin) || !ssIsFinite(aux.mHeightmapMax) || aux.mHeightmapMax < aux.mHeightmapMin)
    {
        ++mMetrics.mTerrainSkipped;
        return false;
    }

    std::vector<F32> scratch((size_t)W * W);
    if (!aux.unpackHeightmap(scratch.data(), (U32)W, (U32)W)) { ++mMetrics.mTerrainSkipped; return false; }

    // PASS 1 - write Z and mark received for the whole set before any edge or normal work, because updateNormals consults getHasReceivedData() on neighbouring patches to decide whether to pull a corner value.
    std::vector<LLSurfacePatch*> painted;
    painted.reserve((size_t)ppe * ppe);

    for (S32 j = 0; j < ppe; ++j)
    {
        for (S32 i = 0; i < ppe; ++i)
        {
            LLSurfacePatch* p = land.getPatch(i, j);
            if (!p) continue;

            // A patch the sim has already delivered is never overwritten: the live stream always wins, and the sim does not resend a patch it has sent.
            if (p->getHasReceivedData()) continue;

            F32* dst = p->getDataZ();
            if (!dst) continue;

            const F32* src = scratch.data() + (size_t)(j * gppe) * W + (size_t)(i * gppe);
            for (S32 y = 0; y < gppe; ++y)
            {
                memcpy(dst + (size_t)y * gpe, src + (size_t)y * W, (size_t)gppe * sizeof(F32));
            }

            p->setHasReceivedData();
            painted.push_back(p);

            // Remember one value we wrote, so the capture at exit can tell our own restored relief from terrain the simulator actually delivered.
            RegionState::PaintWitness w;
            w.mX = i;
            w.mY = j;
            w.mZ = dst[0];
            state.mPaintWitnesses.push_back(w);
        }
    }

    if (painted.empty()) return false;

    // PASS 2 - the exact edge updates the LayerData decode performs per patch. The west/south-west/south half is what heals the seam in the other direction across a region boundary, where getNeighborPatch returns patches belonging to the adjacent surface.
    for (LLSurfacePatch* p : painted)
    {
        p->updateNorthEdge();
        p->updateEastEdge();

        if (LLSurfacePatch* west = p->getNeighborPatch(WEST))
        {
            west->updateEastEdge();
        }
        if (LLSurfacePatch* southwest = p->getNeighborPatch(SOUTHWEST))
        {
            southwest->updateEastEdge();
            southwest->updateNorthEdge();
        }
        if (LLSurfacePatch* south = p->getNeighborPatch(SOUTH))
        {
            south->updateNorthEdge();
        }
    }

    // PASS 3 - dirty, which invalidates normals on the patch and its eight neighbours and queues the geometry rebuild. Everything after this point is the ordinary frame loop: normals, vertical stats and the texture update all run from the dirty list, and it is updateVerticalStats that finally sets the surface's has-Z-data flag.
    for (LLSurfacePatch* p : painted)
    {
        p->dirtyZ();
    }

    LL_INFOS("SSROC") << "Painted " << painted.size() << " of " << (ppe * ppe)
                      << " terrain patches for region " << regionp->getHandle() << " from cache" << LL_ENDL;
    return true;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

bool SSROCAuxMgr::terrainIsSimSourced(LLViewerRegion* regionp, const RegionState& state) const
{
    // Nothing was painted this visit, so whatever is in the surface came from the simulator.
    if (state.mPaintWitnesses.empty()) return true;

    LLSurface& land = regionp->getLand();

    // Every patch we painted must have been overwritten since. If even one still holds the exact value we wrote, part of the surface is still our own restored copy and storing it would launder stale relief back into the cache as though the simulator had confirmed it.
    for (const RegionState::PaintWitness& w : state.mPaintWitnesses)
    {
        const LLSurfacePatch* p = land.getPatch(w.mX, w.mY);
        if (!p) return false;

        const F32* z = p->getDataZ();
        if (!z || z[0] == w.mZ) return false;
    }
    return true;
}

bool SSROCAuxMgr::captureAux(LLViewerRegion* regionp, SSROCAux& out, bool keep_cached_heightmap) const
{
    LLSurface& land = regionp->getLand();

    const S32 gpe  = land.getGridsPerEdge();
    const S32 gppe = land.getGridsPerPatchEdge();
    const S32 ppe  = land.getPatchesPerEdge();
    const S32 W    = gpe - 1;

    // Every rejection below is logged with its reason: a silent capture failure is indistinguishable from the feature not running at all, which is exactly the situation that wasted the first test session.
    if (W <= 0 || gppe <= 0 || ppe <= 0)
    {
        LL_INFOS("SSROC") << "No aux capture for region " << regionp->getHandle()
                          << ": implausible surface dimensions gpe=" << gpe << " gppe=" << gppe << " ppe=" << ppe << LL_ENDL;
        return false;
    }

    // Terrain is only worth storing once the surface actually has data - hasZData goes true when the first patch completes its vertical stats.
    if (!land.hasZData())
    {
        LL_INFOS("SSROC") << "No aux capture for region " << regionp->getHandle() << ": surface has no Z data yet" << LL_ENDL;
        return false;
    }

    // Only capture when every patch has real data, so a half-streamed region never overwrites a complete cached one with holes.
    S32 missing = 0;
    for (S32 j = 0; j < ppe; ++j)
    {
        for (S32 i = 0; i < ppe; ++i)
        {
            const LLSurfacePatch* p = land.getPatch(i, j);
            if (!p || !p->getHasReceivedData()) ++missing;
        }
    }
    if (missing > 0)
    {
        LL_INFOS("SSROC") << "No aux capture for region " << regionp->getHandle() << ": " << missing
                          << " of " << (ppe * ppe) << " terrain patches never arrived" << LL_ENDL;
        return false;
    }

    // Patch (0,0)'s Z pointer is the start of the surface's own grid array, so the whole region reads as one row-major block with a stride of gpe. The east and north buffer row and column are deliberately excluded: they belong to the neighbouring region.
    LLSurfacePatch* origin = land.getPatch(0, 0);
    if (!origin) return false;

    const F32* surface_z = origin->getDataZ();
    if (!surface_z) return false;

    if (keep_cached_heightmap)
    {
        // The relief currently in the surface is partly the copy we painted from this very file, so re-packing it would prove nothing and would overwrite a good cache with our own echo if the region has since been terraformed. Whatever heightmap the loaded file already carries stays as it is.
        LL_INFOS("SSROC") << "Keeping cached heightmap for region " << regionp->getHandle()
                          << ": the simulator did not resend terrain this visit" << LL_ENDL;
    }
    else
    {
        if (!out.packHeightmap(surface_z, (U32)W, (U32)gpe))
        {
            LL_INFOS("SSROC") << "No aux capture for region " << regionp->getHandle() << ": heightmap quantisation refused the surface data" << LL_ENDL;
            return false;
        }
        out.mHeightmapGridsPerPatchEdge = (U16)gppe;
    }

    out.mWaterHeight = land.getWaterHeight();

    LLVLComposition* compp = regionp->getComposition();
    if (compp && compp->getParamsReady())
    {
        bool complete = true;
        for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i)
        {
            if (compp->getDetailAssetID(i).isNull()) { complete = false; break; }
        }

        if (complete)
        {
            for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i)
            {
                out.mComposition.mDetail[i]      = compp->getDetailAssetID(i);
                out.mComposition.mStartHeight[i] = compp->getStartHeight(i);
                out.mComposition.mHeightRange[i] = compp->getHeightRange(i);
            }
            out.mComposition.mValid = true;
        }
    }

    return true;
}

std::string SSROCAuxMgr::metricsString() const
{
    return llformat("ROC aux: loaded %u missing %u | terrain %u (skipped %u) water %u comp %u | captured %u",
                    mMetrics.mRegionsLoaded, mMetrics.mRegionsMissing,
                    mMetrics.mTerrainApplied, mMetrics.mTerrainSkipped,
                    mMetrics.mWaterApplied, mMetrics.mCompApplied,
                    mMetrics.mCaptured);
}
