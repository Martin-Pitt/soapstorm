/**
 * @file ssrocghost.h
 * @brief Region Object Cache Phase 2: ghost injection and reconciliation - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_ROCGHOST_H
#define SS_ROCGHOST_H

#include "llsingleton.h"
#include "lluuid.h"
#include "llvocache.h"
#include "ssroccache.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

// Phase 2 of the Region Object Cache: paint a known region back from its own .roc file on arrival, instead of waiting for the simulator's probe flood to walk the interest list and mention each object in turn.
//
// A ghost is not a new kind of object. It is an ordinary LLVOCacheEntry made VALID early, so every stock path - the cache octree, the scene-contribution culling, createVisibleObjects, killInvisibleObjects and the re-rez on a camera turn - carries it without knowing anything about the ROC. What is ROC-specific lives on the record here, never on the LLViewerObject, because killInvisibleObjects reaps objects seconds after arrival and any state hung on the object alone would be lost with it.
//
// Two invariants govern everything below. The live stream ALWAYS wins: an entry the simulator has already validated, or an object already in the world, is never touched. And the ROC never speaks to the simulator, which is why injection goes through a purpose-built stock entry point rather than through probeCache, whose miss branches queue cache misses that the viewer later sends as a RequestMultipleObjects.

// A ghost's whole lifecycle. RECORDED and the promotion states live on the .roc record; these three are the visit-scoped half.
enum ESSROCGhostState : U8
{
    SSROC_GHOST_LIVE      = 0,  // injected, awaiting the simulator's word. Survives being reaped and re-rezzed by the stock invisibility culling - the TTL keeps ticking either way.
    SSROC_GHOST_CONFIRMED = 1,  // the simulator mentioned this object at this local id, so it is the sim's now and the ROC is done with it
    SSROC_GHOST_RETIRED   = 2,  // derezzed by TTL, by a local-id takeover or by the trash-all sweep. The .roc RECORD is untouched: silence is never evidence of deletion.
};

// Why a region visit did or did not paint ghosts. A CLOSED set, latched at every single exit from armRegion and reported once per region arrival, because the previous shape of this - one boolean printed as "mode B, no injection" - was assigned at only one of four return sites and read as "the simulator restarted" for three cases that were nothing of the kind. That one string was enough to make a working feature look permanently inert. Nothing here may ever be inferred from a default value: every path sets its own.
enum ESSROCArmOutcome : U8
{
    SSROC_ARM_PENDING           = 0,  // the tick has not attempted this region yet
    SSROC_ARM_NO_HANDSHAKE      = 1,  // the region was tracked but its handshake never reached us, so the local-id epoch was never knowable - what a mid-session enable looks like
    SSROC_ARM_LOAD_IN_FLIGHT    = 2,  // armed after the bounded wait expired with the disk read still outstanding; re-armed for free if the file lands later
    SSROC_ARM_NO_FILE           = 3,  // the disk read completed and there was no .roc at all - a first visit
    SSROC_ARM_FILE_EMPTY        = 4,  // a .roc exists but holds no records
    SSROC_ARM_REGION_ID_MISMATCH= 5,  // the file was written for a different region under the same handle
    SSROC_ARM_CACHEID_CHANGED   = 6,  // the true mode B: the simulator restarted, every stored local id belongs to somebody else now
    SSROC_ARM_PLAN_EMPTY        = 7,  // mode A held and the file had records, but none of them qualified on either tier
    SSROC_ARM_INJECTED          = 8,  // ghosts were planned and injection ran
    SSROC_ARM_COUNT             = 9,
};

const char* ssROCArmOutcomeName(U8 outcome);

class SSROCGhostMgr : public LLSingleton<SSROCGhostMgr>
{
    LLSINGLETON(SSROCGhostMgr);
    ~SSROCGhostMgr();

public:
    // Gated by SSROCEnabled and SSROCGhostsEnabled together, so ghosts can be turned off without giving up the ledger, the terrain cache or the promotion pipeline underneath them.
    static bool enabled();

    // The store's async load completion, forwarded by SSROCAuxMgr. Holds the file; the plan is not built until the handshake says whether the stored local ids still mean anything.
    void onRegionFileLoaded(U64 handle, SSROCFilePtr file);

    // LLViewerRegion::unpackRegionHandshake, forwarded by SSROCAuxMgr with the simulator's CacheID. A mismatch means the simulator restarted and every stored local id is now somebody else's, which is what decides mode A against mode B.
    void onRegionHandshake(LLViewerRegion* regionp, const LLUUID& cache_id);

    // LLWorld::removeRegion, forwarded by SSROCAuxMgr. Reports the visit's outcome and hands the created-but-unconfirmed entries to the save-time purge, which runs later inside the region destructor.
    void onRegionRemoved(LLViewerRegion* regionp);

    void shutdown();

    // One frame's work: budgeted injection followed by a budgeted TTL sweep. Called from LLWorld::updateRegions above the per-region idle updates, so anything injected this frame reaches createVisibleObjects in the same frame.
    void tick();

    // Reconciliation. Both of the ledger's existing recording hooks route through here, so the ROC has exactly one place where the simulator's word about an object arrives and no additional stock touch is needed for it.
    void noteSimUpdate(LLViewerRegion* regionp, U32 local_id, U32 crc, U32 flags, const U8* blob, S32 blob_len);

    // An unsolicited KillObject. Phase 2 issues no select probes at all, so every kill that reaches this is one the simulator volunteered.
    void noteKill(LLViewerRegion* regionp, U32 local_id, const LLUUID& full_id);

    // Called at every cache rez path, initial and re-rez alike, so a ghost reaped behind the camera and rebuilt on a camera turn is still suppressed.
    void applySuppression(LLViewerObject* objectp);

    // Erase the entries the ROC created from the region's object cache map before it is written to disk. Only entries the ROC itself invented and that the simulator never confirmed are dropped: an entry that came out of the .slc is the simulator's and is left exactly as it was found.
    U32 purgeInjectedEntries(U64 handle, LLVOCacheEntry::vocache_entry_map_t& cache_map);

    // A region whose ghosts were mass-refuted. Its object ledger is not written back at exit, because the evidence says the place we remember is gone.
    bool isRegionTrashed(U64 handle) const;

    struct Metrics
    {
        U32 mRegionsArmed    = 0;
        U32 mRegionsModeB    = 0;   // CacheID mismatch: the simulator restarted, so no injection at all in this phase
        U32 mValidated       = 0;   // .slc entries made visible early - the cheapest and safest half of the win
        U32 mCreated         = 0;   // entries the ROC invented because the .slc no longer held them
        U32 mSkipped         = 0;
        U32 mConfirmed       = 0;
        U32 mExpired         = 0;
        // <SS:Nexii> Ghosts whose watch ended without ROC touching the world, because the entry was the simulator's own. Counted apart from mExpired so "we stopped looking" is never mistaken for "we removed something".
        U32 mReleasedValidated = 0;
        U32 mIdReuse         = 0;
        U32 mKilled          = 0;
        U32 mTrashed         = 0;
        U32 mPurged          = 0;
        U32 mMissedSends     = 0;   // cache misses refused at the reserved-id floor - must stay zero, and is logged loudly if it ever does not

        // One counter per ESSROCArmOutcome, so the session summary answers "how often did this feature decline, and for which reason" without anyone having to grep a whole log for per-region lines.
        U32 mOutcomes[SSROC_ARM_COUNT] = { 0 };
    };
    const Metrics& metrics() const { return mMetrics; }
    std::string metricsString() const;

    // Counted rather than exposed as a mutable metrics reference, so the only thing outside this class that can move a ROC statistic is the one guard that has a reason to.
    void noteRefusedCacheMiss() { ++mMetrics.mMissedSends; }

private:
    struct Ghost
    {
        Ghost();

        LLUUID mFullID;
        U32    mLocalID;
        U32    mCRC;
        U32    mSimFlags;       // the update flags the ledger last saw from the simulator, restored on confirmation because a ghost is rezzed with flags of zero
        F64    mDeadline;       // elapsed-seconds moment an unconfirmed ghost derezzes
        U8     mState;
        bool   mCreatedEntry;   // the ROC invented this cache entry rather than validating one the .slc already held
    };

    struct RegionGhosts
    {
        RegionGhosts();

        U64          mHandle;
        LLUUID       mRegionID;
        SSROCFilePtr mFile;
        LLUUID       mCacheID;
        bool         mHaveHandshake;
        bool         mLoadDone;      // the async .roc read has landed, WITH OR WITHOUT a file. Without this the handshake can win the race on a large file or a busy worker pool and the region is written off as "no file" for the whole visit while its cache arrives a frame later.
        bool         mModeA;         // the simulator's CacheID matches the one stored beside the records, so the stored local ids are still the same objects
        bool         mArmed;         // the plan is built and injection may proceed
        bool         mPlanBuilt;
        bool         mTrashed;
        U8           mOutcome;       // ESSROCArmOutcome, latched at every exit from armRegion
        F64          mHandshakeAt;   // when the bounded wait for the disk read started
        F64          mArmedAt;
        F64          mFirstInject;

        std::vector<U32>             mPlan;      // indices into mFile->mRecords, most permanent and largest first
        size_t                       mNextPlan;
        std::vector<Ghost>           mGhosts;
        std::unordered_map<U32, U32> mByLocalID; // local id -> index into mGhosts

        // Mode-B shadow measurement. Nothing is painted; the plan that WOULD have been injected is held as a FullID set and struck off as the simulator delivers each object unaided. It is the only honest evidence for whether a mode-B injector is worth building, and it is free precisely in mode B: a CacheID change makes the stock code discard the whole .slc, so the simulator must send a full blob for every object in the region and every one of them carries its FullID at blob offset zero.
        std::unordered_set<LLUUID>   mShadow;
        U32 mShadowPlanned;
        U32 mShadowDelivered;
        U32 mShadowSightings;       // every sighting in this region this visit, as the denominator - without it "delivered 900" says nothing about coverage

        U32 mInjected;
        U32 mRefutations;
        U32 mTTLSweep;              // rolling cursor for the TTL pass, so a large ghost set is never walked whole in one frame
    };

    RegionGhosts* stateFor(U64 handle);
    void armRegion(LLViewerRegion* regionp, RegionGhosts& rg);
    void finishArm(RegionGhosts& rg, U8 outcome);
    void buildShadowPlan(RegionGhosts& rg, const SSROCInjectPolicy& policy);
    bool injectOne(LLViewerRegion* regionp, RegionGhosts& rg, const SSROCRecord& rec);
    void retireGhost(LLViewerRegion* regionp, RegionGhosts& rg, Ghost& ghost, const char* why);
    void confirmGhost(RegionGhosts& rg, Ghost& ghost, U32 flags);
    void trashRegion(LLViewerRegion* regionp, RegionGhosts& rg);
    void sweepTTL(LLViewerRegion* regionp, RegionGhosts& rg, S32& budget);
    void dropRegion(U64 handle);

    std::map<U64, RegionGhosts>  mRegions;

    // Every FullID currently standing as an unconfirmed ghost, across all regions. Suppression is asked about every single cache rez in the viewer, so on the ordinary path the answer has to cost an empty-set test and nothing more.
    std::unordered_set<LLUUID>   mLiveGhosts;

    // Local ids of entries the ROC created and the simulator never confirmed, kept per region so the purge can run later inside LLViewerRegion::saveObjectCache - which happens in the region destructor, well after LLWorld::removeRegion has already torn down the live state.
    std::map<U64, std::vector<U32> > mPendingPurge;

    std::vector<U64>             mTrashedRegions;
    Metrics                      mMetrics;
};

// The four stock entry points, as free functions so each stock file costs one include and one line. Injection deliberately does NOT appear here: it goes through LLViewerRegion::ssROCActivateCacheEntry, which is the only stock path that neither speaks to the simulator, nor moves a statistic, nor re-enters the ledger's recording hooks - so no re-entrancy guard is needed anywhere.
void ssROCGhostTick();
void ssROCGhostNoteKill(LLViewerRegion* regionp, U32 local_id, const LLUUID& full_id);

// The one hard fail-safe: true when a local id falls in the ROC's reserved range and must never be queued as a cache miss, because requestCacheMisses sends that queue to the simulator as a RequestMultipleObjects once a second. Nothing allocates in that range today, so this can only fire on a grid whose id policy has changed - which is exactly when a silent send would be hardest to notice. Turns "the ROC never causes a send" from an audit of every call site into one comparison. See doc/region_object_cache.md.
bool ssROCRefuseCacheMiss(U32 local_id);
void ssROCGhostSuppress(LLViewerObject* objectp);
void ssROCPurgeInjectedEntries(LLViewerRegion* regionp, LLVOCacheEntry::vocache_entry_map_t& cache_map);

#endif // SS_ROCGHOST_H
