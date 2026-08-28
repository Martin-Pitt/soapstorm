/**
 * @file ssrocaux.h
 * @brief Region Object Cache auxiliary data: capture and instant re-apply of terrain, water and composition - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_ROCAUX_H
#define SS_ROCAUX_H

#include "llsingleton.h"
#include "lluuid.h"
#include "ssroccache.h"

#include <map>
#include <vector>

class LLViewerRegion;

// Phase 1 of the Region Object Cache: the parts of a region that are not objects. On re-entering a known region this paints back the terrain relief, the water level and the terrain textures immediately, instead of waiting for the LayerData stream and the region handshake. Everything applied here is overwritten harmlessly by the real data when it arrives, because the natural paths write the same state through the same setters.
//
// EEP region environment is deliberately NOT part of this - see the out-of-scope note in doc/region_object_cache.md.
class SSROCAuxMgr : public LLSingleton<SSROCAuxMgr>
{
    LLSINGLETON(SSROCAuxMgr);
    ~SSROCAuxMgr();

public:
    // LLWorld::addRegion, once the region is findable by handle. Kicks the async load; the apply always happens later on a main-thread completion, never inline here.
    void onRegionAdded(LLViewerRegion* regionp);

    // LLWorld::removeRegion, before anything is torn down. Captures what this visit learned and queues the save.
    void onRegionRemoved(LLViewerRegion* regionp);

    // LLViewerRegion::unpackRegionHandshake, while the sim's own water height and cache id are still in scope and after the composition block has run. These values are authoritative and are what gets stored.
    void onRegionHandshake(LLViewerRegion* regionp, F32 water_height, const LLUUID& cache_id);

    // LLViewerRegion::rebuildWater. A freshly rebuilt water object is parked at the default height, so a cached or handshake value has to be pushed again or the sea sits at the wrong level.
    void onWaterRebuilt(LLViewerRegion* regionp);

    // Make the region's .roc resident NOW, reading it on this thread if the async load has not landed. Exists for exactly one caller: LLViewerRegion::loadObjectCache runs inside unpackRegionHandshake and the map it fills decides bit 1 of the RegionHandshakeReply, the bit that tells the simulator whether to send cache probes at all - so an answer given from a half-arrived cache would change what the simulator sends. Returns true when a record set is available, whichever way it arrived.
    //
    // It is not a new stall. It stands where the stock synchronous .slc read stands and replaces it, and on the common path where the async read already landed it does no IO at all.
    bool ensureRegionLoaded(U64 handle);

    void shutdown();

    struct Metrics
    {
        U32 mRegionsLoaded  = 0;
        U32 mRegionsMissing = 0;
        U32 mTerrainApplied = 0;
        U32 mTerrainSkipped = 0;
        U32 mWaterApplied   = 0;
        U32 mCompApplied    = 0;
        U32 mCaptured       = 0;
    };
    const Metrics& metrics() const { return mMetrics; }
    std::string metricsString() const;

private:
    // Per-region state for the current session. Keyed by region handle rather than by pointer, because the async load completes later and the region it was kicked for may already be gone.
    struct RegionState
    {
        SSROCFilePtr mFile;                 // what was loaded from disk, null when the region had no cache
        LLUUID       mCacheID;
        F32          mHandshakeWater = 0.f;
        bool         mHaveHandshake  = false;
        bool         mTerrainApplied = false;
        bool         mWaterApplied   = false;
        bool         mCompApplied    = false;
        bool         mLoadDone       = false;

        // One witness value per patch we painted from cache. LLSurfacePatch carries no provenance - our own paint sets the same "received" flag the simulator's LayerData sets - so without these the capture at exit cannot tell its own restored relief from freshly streamed terrain, and a brief visit would re-save stale heights over a region that has since been terraformed.
        struct PaintWitness { S32 mX; S32 mY; F32 mZ; };
        std::vector<PaintWitness> mPaintWitnesses;
    };

    // The one completion path, shared by the async load and the blocking fallback so the two can never diverge. Idempotent: the second arrival for a handle is dropped, which is what makes the fallback safe to run while the worker read is still outstanding.
    void completeLoad(U64 handle, SSROCFilePtr file);
    void applyPending(U64 handle);
    bool applyTerrain(LLViewerRegion* regionp, const SSROCAux& aux, RegionState& state);
    bool applyWater(LLViewerRegion* regionp, const SSROCAux& aux, RegionState& state);
    bool applyComposition(LLViewerRegion* regionp, const SSROCAux& aux);
    bool captureAux(LLViewerRegion* regionp, SSROCAux& out, bool keep_cached_heightmap) const;

    // True only when every patch this visit painted from cache was subsequently overwritten by the simulator, which is the one case where the live surface is entirely sim-sourced and safe to store.
    bool terrainIsSimSourced(LLViewerRegion* regionp, const RegionState& state) const;

    std::map<U64, RegionState> mRegions;
    Metrics                    mMetrics;
};

#endif // SS_ROCAUX_H
