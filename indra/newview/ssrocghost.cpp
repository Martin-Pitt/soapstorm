/**
 * @file ssrocghost.cpp
 * @brief Region Object Cache Phase 2: ghost injection and reconciliation - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssrocghost.h"

#include "ssrocledger.h"

#include "llagent.h"
#include "lldatapacker.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvocache.h"
#include "llworld.h"

#include <algorithm>
#include <ctime>

namespace
{
    // How long after the first ghost of a visit a mass refutation still counts as evidence that the region we remember is gone. Later kills are ordinary churn - somebody taking a build apart an hour into a visit is not the same claim as arriving to find nothing there.
    const F64 SSROC_TRASH_WINDOW_SECS = 120.0;

    // Below this many refuted ghosts a percentage means nothing: on a region that injected forty objects, picking up eight of your own prims would otherwise trash the whole cache.
    const U32 SSROC_TRASH_MIN_REFUTATIONS = 10;

    // The blob prefix the injector needs to read - the same fixed 0..83 window the ledger parses.
    const S32 SSROC_BLOB_PREFIX_BYTES = 84;
}

SSROCGhostMgr::Ghost::Ghost()
:   mLocalID(0),
    mCRC(0),
    mSimFlags((U32)-1),
    mDeadline(0.0),
    mState(SSROC_GHOST_LIVE),
    mCreatedEntry(false)
{
}

SSROCGhostMgr::RegionGhosts::RegionGhosts()
:   mHandle(0),
    mHaveHandshake(false),
    mLoadDone(false),
    mModeA(false),
    mArmed(false),
    mPlanBuilt(false),
    mTrashed(false),
    mOutcome(SSROC_ARM_PENDING),
    mHandshakeAt(0.0),
    mArmedAt(0.0),
    mFirstInject(0.0),
    mNextPlan(0),
    mShadowPlanned(0),
    mShadowDelivered(0),
    mShadowSightings(0),
    mInjected(0),
    mRefutations(0),
    mTTLSweep(0)
{
}

const char* ssROCArmOutcomeName(U8 outcome)
{
    switch (outcome)
    {
    case SSROC_ARM_PENDING:            return "PENDING";
    case SSROC_ARM_NO_HANDSHAKE:       return "NO_HANDSHAKE";
    case SSROC_ARM_LOAD_IN_FLIGHT:     return "LOAD_IN_FLIGHT";
    case SSROC_ARM_NO_FILE:            return "NO_FILE";
    case SSROC_ARM_FILE_EMPTY:         return "FILE_EMPTY";
    case SSROC_ARM_REGION_ID_MISMATCH: return "REGION_ID_MISMATCH";
    case SSROC_ARM_CACHEID_CHANGED:    return "CACHEID_CHANGED";
    case SSROC_ARM_PLAN_EMPTY:         return "PLAN_EMPTY";
    case SSROC_ARM_INJECTED:           return "INJECTED";
    default:                           return "?";
    }
}

SSROCGhostMgr::SSROCGhostMgr()
{
}

SSROCGhostMgr::~SSROCGhostMgr()
{
}

// static
bool SSROCGhostMgr::enabled()
{
    static LLCachedControl<bool> ghosts(gSavedSettings, "SSROCGhostsEnabled", true);
    return SSROCStore::enabled() && (bool)ghosts;
}

void SSROCGhostMgr::shutdown()
{
    // Logged whenever a region was entered at all, not only when something was painted. A session in which every region declined is exactly the session whose summary is worth reading.
    U32 outcomes = 0;
    for (U8 i = 1; i < SSROC_ARM_COUNT; ++i) outcomes += mMetrics.mOutcomes[i];

    if (outcomes || mMetrics.mValidated || mMetrics.mCreated || mMetrics.mRegionsArmed)
    {
        LL_INFOS("SSROC") << metricsString() << LL_ENDL;
    }

    mRegions.clear();
    mLiveGhosts.clear();
    mPendingPurge.clear();
    mTrashedRegions.clear();
}

SSROCGhostMgr::RegionGhosts* SSROCGhostMgr::stateFor(U64 handle)
{
    auto it = mRegions.find(handle);
    return (it == mRegions.end()) ? NULL : &it->second;
}

void SSROCGhostMgr::dropRegion(U64 handle)
{
    auto it = mRegions.find(handle);
    if (it == mRegions.end()) return;

    for (const Ghost& ghost : it->second.mGhosts)
    {
        if (ghost.mState == SSROC_GHOST_LIVE) mLiveGhosts.erase(ghost.mFullID);
    }
    mRegions.erase(it);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SSROCGhostMgr::onRegionFileLoaded(U64 handle, SSROCFilePtr file)
{
    if (!enabled()) return;

    RegionGhosts& rg = mRegions[handle];
    rg.mHandle   = handle;
    rg.mFile     = file;
    rg.mLoadDone = true;

    // The bounded wait expired and the region was written off without its cache, but the file has arrived and nothing has been painted or refuted yet - so the visit is still recoverable. Clearing mPlanBuilt is the whole recovery: the tick re-arms on the next frame with the file in hand.
    if (rg.mPlanBuilt && rg.mOutcome == SSROC_ARM_LOAD_IN_FLIGHT && !rg.mArmed && !rg.mInjected && !rg.mTrashed)
    {
        LL_INFOS("SSROC") << "Region " << handle << ": cache arrived after the arm wait expired, re-arming ghosts" << LL_ENDL;
        rg.mPlanBuilt = false;
        rg.mOutcome   = SSROC_ARM_PENDING;
    }
}

void SSROCGhostMgr::onRegionHandshake(LLViewerRegion* regionp, const LLUUID& cache_id)
{
    if (!regionp || !enabled()) return;

    // The handshake and the disk read race in both directions, so the state is created by whichever arrives first and completed by the other. Arming itself is deferred to the tick, because building a plan for a fully populated region is thousands of records of work and belongs on a frame where the ghost budget owns it.
    RegionGhosts& rg = mRegions[regionp->getHandle()];
    rg.mHandle        = regionp->getHandle();
    rg.mRegionID      = regionp->getRegionID();
    rg.mCacheID       = cache_id;
    rg.mHaveHandshake = true;
    rg.mHandshakeAt   = (F64)LLTimer::getElapsedSeconds();
}

void SSROCGhostMgr::onRegionRemoved(LLViewerRegion* regionp)
{
    if (!regionp) return;

    const U64 handle = regionp->getHandle();

    RegionGhosts* rgp = stateFor(handle);
    if (rgp)
    {
        RegionGhosts& rg = *rgp;

        U32 live = 0, confirmed = 0, retired = 0;
        std::vector<U32>& purge = mPendingPurge[handle];

        for (const Ghost& ghost : rg.mGhosts)
        {
            switch (ghost.mState)
            {
            case SSROC_GHOST_CONFIRMED: ++confirmed; break;
            case SSROC_GHOST_RETIRED:   ++retired;   break;
            default:                    ++live;      break;
            }

            // Only entries the ROC invented and that the simulator never confirmed are withheld from the .slc. An entry that came out of the .slc in the first place is the simulator's own and is left exactly as it was found.
            if (ghost.mCreatedEntry && ghost.mState != SSROC_GHOST_CONFIRMED)
            {
                purge.push_back(ghost.mLocalID);
            }
        }

        if (purge.empty()) mPendingPurge.erase(handle);

        // A region that never even reached armRegion still owes an explanation, and the commonest reason it never reached it - ROC turned on after this region was already added, so its handshake was never latched - is exactly the one that used to produce no log line at all.
        U8 outcome = rg.mOutcome;
        if (outcome == SSROC_ARM_PENDING && !rg.mHaveHandshake) outcome = SSROC_ARM_NO_HANDSHAKE;
        ++mMetrics.mOutcomes[llmin<U8>(outcome, (U8)(SSROC_ARM_COUNT - 1))];

        // ONE line per region arrival, unconditionally, naming the path taken and why. This is deliberately not gated on anything: the previous shape of this log was gated on (mInjected || mPlanBuilt) and printed a mode-B suffix off a boolean that three of the four exit paths never assigned, which is how a feature that had in fact painted three thousand objects came to be read as never having run.
        LL_INFOS("SSROC") << "Ghosts for region " << handle << ": " << ssROCArmOutcomeName(outcome)
                          << (outcome == SSROC_ARM_PENDING ? " (not armed this visit - only the agent's own region is armed in this phase, and only once both its handshake and its cache read have landed)" : "")
                          << " | planned " << rg.mPlan.size()
                          << ", injected " << rg.mInjected
                          << " (confirmed " << confirmed << ", still unconfirmed " << live
                          << ", retired " << retired << ")"
                          << ", refutations " << rg.mRefutations
                          << (rg.mTrashed ? " | TRASHED" : "")
                          << LL_ENDL;

        // The mode-B evidence, reported separately because it measures something the ordinary line cannot: how much of what a mode-B injector WOULD have painted the simulator delivered on its own anyway, which is the number that decides whether building one is worth the risk.
        if (rg.mShadowPlanned)
        {
            LL_INFOS("SSROC") << "Region " << handle << " mode-B shadow: " << rg.mShadowPlanned
                              << " objects would have been painted, the simulator delivered "
                              << rg.mShadowDelivered << " of them unaided out of " << rg.mShadowSightings
                              << " sightings this visit" << LL_ENDL;
        }

        dropRegion(handle);
    }

    // Consumed by SSROCAuxMgr before this point, so the marker has done its job.
    mTrashedRegions.erase(std::remove(mTrashedRegions.begin(), mTrashedRegions.end(), handle), mTrashedRegions.end());
}

bool SSROCGhostMgr::isRegionTrashed(U64 handle) const
{
    return std::find(mTrashedRegions.begin(), mTrashedRegions.end(), handle) != mTrashedRegions.end();
}

// ---------------------------------------------------------------------------
// Arming
// ---------------------------------------------------------------------------

void SSROCGhostMgr::finishArm(RegionGhosts& rg, U8 outcome)
{
    rg.mOutcome = outcome;
}

// The plan that WOULD have been painted, held as FullIDs and never injected. Building it costs one pass and one sort over the same records mode A would have ranked, which measured at well under a millisecond on a 4132-record region, and the set is struck off as the simulator delivers each object on its own.
void SSROCGhostMgr::buildShadowPlan(RegionGhosts& rg, const SSROCInjectPolicy& policy)
{
    static LLCachedControl<bool> shadow(gSavedSettings, "SSROCGhostShadowMeasure", true);
    if (!(bool)shadow || !rg.mFile) return;

    std::vector<U32> plan;
    ssROCBuildInjectPlan(*rg.mFile, policy, plan);
    if (plan.empty()) return;

    rg.mShadow.reserve(plan.size());
    for (U32 index : plan)
    {
        if (index < rg.mFile->mRecords.size()) rg.mShadow.insert(rg.mFile->mRecords[index].mFullID);
    }
    rg.mShadowPlanned = (U32)rg.mShadow.size();
}

void SSROCGhostMgr::armRegion(LLViewerRegion* regionp, RegionGhosts& rg)
{
    rg.mPlanBuilt = true;
    rg.mArmedAt   = (F64)LLTimer::getElapsedSeconds();

    // Every one of the exits below latches its own reason. Nothing downstream may infer a reason from a default, because that is precisely the defect this replaces.
    if (!rg.mLoadDone)
    {
        LL_WARNS("SSROC") << "Region " << rg.mHandle << ": armed with the cache read still in flight after the wait - no ghosts unless it lands shortly" << LL_ENDL;
        finishArm(rg, SSROC_ARM_LOAD_IN_FLIGHT);
        return;
    }

    if (!rg.mFile)
    {
        LL_INFOS("SSROC") << "Region " << rg.mHandle << ": no cache file on disk, so this is a first visit and there is nothing to paint" << LL_ENDL;
        finishArm(rg, SSROC_ARM_NO_FILE);
        return;
    }

    if (rg.mFile->mRecords.empty())
    {
        LL_INFOS("SSROC") << "Region " << rg.mHandle << ": cache file holds no object records, so there is nothing to paint" << LL_ENDL;
        finishArm(rg, SSROC_ARM_FILE_EMPTY);
        return;
    }

    // Handles are reused across grids and sessions, so a file is only ever applied to the region it was written for.
    if (rg.mFile->mRegionID.notNull() && regionp->getRegionID().notNull()
        && rg.mFile->mRegionID != regionp->getRegionID())
    {
        LL_WARNS("SSROC") << "Region " << rg.mHandle << " cache belongs to a different region ("
                          << rg.mFile->mRegionID << " on disk, " << regionp->getRegionID() << " live) - no ghosts" << LL_ENDL;
        finishArm(rg, SSROC_ARM_REGION_ID_MISMATCH);
        return;
    }

    static LLCachedControl<bool> promoted_tier(gSavedSettings, "SSROCGhostPromotedTier", true);
    static LLCachedControl<bool> recent_tier(gSavedSettings, "SSROCGhostRecentTier", true);
    static LLCachedControl<U32>  recent_minutes(gSavedSettings, "SSROCRecentGhostMinutes", 180);
    static LLCachedControl<U32>  max_ghosts(gSavedSettings, "SSROCMaxGhostsPerRegion", 30000);

    SSROCInjectPolicy policy;
    policy.mPromotedTier = (bool)promoted_tier;
    policy.mRecentTier   = (bool)recent_tier;
    policy.mMaxGhosts    = (U32)max_ghosts;

    const U64 now     = (U64)time(NULL);
    const U64 window  = (U64)(U32)recent_minutes * 60;
    policy.mRecentCutoff = (window > 0 && now > window) ? (now - window) : 0;

    // MODE A ONLY in this phase. The simulator's CacheID is what says the local ids we stored still name the same objects; when it changed the simulator restarted, every id was reassigned, and injecting under a stored id would put a remembered object where an unrelated one now lives. Mode B needs FullID-only reconciliation and an unregistered id space, which is a later phase - so for now the visit is measured instead of painted.
    rg.mModeA = rg.mFile->mLastCacheID.notNull() && rg.mCacheID.notNull() && rg.mFile->mLastCacheID == rg.mCacheID;
    if (!rg.mModeA)
    {
        ++mMetrics.mRegionsModeB;

        // BOTH values, always. "The cache id changed" with neither id printed is untestable in the field, and a null on either side is a materially different situation from two different non-null ids - one means the previous visit never saw a handshake, the other means the simulator restarted.
        LL_INFOS("SSROC") << "Region " << rg.mHandle
                          << ": simulator cache id changed since the last visit, so every stored local id now belongs to a different object - no ghosts this time. Stored "
                          << rg.mFile->mLastCacheID << ", live " << rg.mCacheID << LL_ENDL;

        buildShadowPlan(rg, policy);
        finishArm(rg, SSROC_ARM_CACHEID_CHANGED);
        return;
    }

    LLTimer plan_timer;
    ssROCBuildInjectPlan(*rg.mFile, policy, rg.mPlan);
    rg.mNextPlan = 0;

    if (rg.mPlan.empty())
    {
        // The commonest silent nothing after a long absence: mode A holds and the file is full, but the promoted tier is empty and every record fell out of the recent window. Naming the window is what makes it actionable rather than mysterious.
        LL_INFOS("SSROC") << "Region " << rg.mHandle << ": mode A held over " << rg.mFile->mRecords.size()
                          << " records but none qualified - promoted " << rg.mFile->promotedCount()
                          << ", recent window " << (U32)recent_minutes << " minutes" << LL_ENDL;
        finishArm(rg, SSROC_ARM_PLAN_EMPTY);
        return;
    }

    rg.mArmed = true;
    ++mMetrics.mRegionsArmed;
    finishArm(rg, SSROC_ARM_INJECTED);

    LL_INFOS("SSROC") << "Region " << rg.mHandle << ": planned " << rg.mPlan.size()
                      << " ghosts out of " << rg.mFile->mRecords.size() << " records in "
                      << llformat("%.1f", plan_timer.getElapsedTimeF32() * 1000.f) << " ms" << LL_ENDL;
}

// ---------------------------------------------------------------------------
// Injection
// ---------------------------------------------------------------------------

bool SSROCGhostMgr::injectOne(LLViewerRegion* regionp, RegionGhosts& rg, const SSROCRecord& rec)
{
    const U32 planned_local = rec.mLastLocalID;
    if (!planned_local) return false;

    // Idempotent: an id this visit already resolved is never revisited, whatever it resolved to.
    if (rg.mByLocalID.find(planned_local) != rg.mByLocalID.end()) return false;

    // Live always wins. An object already in the world under this FullID is the simulator's, and it is not the ROC's business whether it got there by probe, by full update or by an earlier ghost.
    if (gObjectList.findObject(rec.mFullID))
    {
        ++mMetrics.mSkipped;
        return false;
    }

    static LLCachedControl<bool> suppress(gSavedSettings, "SSROCGhostSuppress", true);

    // A ghost is rezzed with no update flags at all when suppression is on, so it offers no touch, pay or sit affordance it may no longer have. The simulator's real flags are restored the moment it confirms.
    const U32 ghost_flags = (bool)suppress ? 0u : rec.mUpdateFlags;

    U32  local_id = planned_local;
    U32  crc      = rec.mCRC;
    bool created  = false;

    LLVOCacheEntry* entry = regionp->getCacheEntry(planned_local, false);

    // The ROC's own scratch copy of the blob, non-const because LLVOCacheEntry copies out of a packer rather than a buffer. Only built when the entry has to be invented, which is the minority case in mode A.
    std::vector<U8> scratch;

    if (entry)
    {
        // The commonest and by far the safest case: the .slc already holds this object but the entry is untrusted until a probe validates it, and the probe flood is interest-list driven so a behind-camera object waits for a camera turn. Making it visible here is the whole point of the phase, and the bytes being drawn are the simulator's own.
        if (entry->isValid() || entry->isState(LLVOCacheEntry::ACTIVE))
        {
            ++mMetrics.mSkipped;
            return false;
        }

        LLDataPackerBinaryBuffer* edp = entry->getDP();
        if (!edp || edp->getBufferSize() < SSROC_BLOB_PREFIX_BYTES)
        {
            ++mMetrics.mSkipped;
            return false;
        }

        // The entry under this id has to be the same object, or making it visible early would stand somebody else's furniture where ours used to be.
        LLUUID entry_id;
        LLViewerObject::unpackUUID(edp, entry_id, "ID");
        if (entry_id != rec.mFullID)
        {
            ++mMetrics.mSkipped;
            return false;
        }

        // The entry's OWN crc, never the record's. A record that disagrees with the .slc about this object is not one to force.
        crc = entry->getCRC();
    }
    else
    {
        // <SS:Nexii> OFF BY DEFAULT, and the asymmetry with the branch above is the whole reason. That branch can prove it has the right object: it unpacks the FullID the SIMULATOR wrote into the .slc and refuses on mismatch, so the worst case is a wasted skip. This branch has no such witness. Everything it checks - that the blob parses, that its FullID matches the record, that no other entry holds the id - is the ROC agreeing with itself, and a remembered id that the simulator has since reassigned passes every one of them. The only thing standing behind it is the CRC probe, and this project's own measurement found that non-discriminating.
        //
        // The cost of switching it off is measured rather than assumed: every session so far reports "created 0", so this path has never once fired. It is disabled until it can carry a witness of its own rather than because it is suspected - and painting somebody else's build where yours used to be is the one failure that would make the whole feature untrustworthy.
        static LLCachedControl<bool> create_entries(gSavedSettings, "SSROCGhostCreateEntries", false);
        if (!(bool)create_entries)
        {
            ++mMetrics.mSkipped;
            return false;
        }

        // The .slc no longer holds it - trimmed by the stale-entry pass, lost to the 128-slot LRU, or never written. Rebuild the entry from the ROC's own copy, which is byte-identical to what the .slc would have held.
        if (rec.mDP.size() < (size_t)SSROC_BLOB_PREFIX_BYTES) return false;
        if (rec.mDP.size() > (size_t)SSROC_MAX_DP_SIZE) return false;

        SSROCBlobFacts facts;
        if (!ssROCParseBlob(rec.mDP.data(), (S32)rec.mDP.size(), facts)) return false;

        // The blob's own ids are the truth about what this entry describes; a record whose stored local id has drifted from its blob is filed under the blob's.
        if (facts.mFullID != rec.mFullID || facts.mLocalID == 0) return false;
        if (facts.mLocalID != planned_local && rg.mByLocalID.find(facts.mLocalID) != rg.mByLocalID.end()) return false;
        if (facts.mLocalID != planned_local && regionp->getCacheEntry(facts.mLocalID, false)) return false;

        local_id = facts.mLocalID;
        crc      = facts.mCRC;
        scratch  = rec.mDP;
        created  = true;
    }

    // Activation can rez the object synchronously - decodeBoundingInfo goes straight to processObjectUpdateFromCache when cache culling is off, or when the entry already owns a drawable - so the ghost is registered as live BEFORE the call. Otherwise the very first rez, the one this whole phase exists to produce, would be the one rez that escapes suppression.
    mLiveGhosts.insert(rec.mFullID);

    LLDataPackerBinaryBuffer dp(scratch.data(), (S32)scratch.size());

    if (!regionp->ssROCActivateCacheEntry(local_id, crc, ghost_flags, created ? &dp : NULL))
    {
        mLiveGhosts.erase(rec.mFullID);
        ++mMetrics.mSkipped;
        return false;
    }

    Ghost ghost;
    ghost.mFullID       = rec.mFullID;
    ghost.mLocalID      = local_id;
    ghost.mCRC          = crc;
    ghost.mSimFlags     = rec.mUpdateFlags;
    ghost.mState        = SSROC_GHOST_LIVE;
    ghost.mCreatedEntry = created;

    static LLCachedControl<F32> ttl(gSavedSettings, "SSROCGhostTTL", 120.f);
    ghost.mDeadline = (F64)LLTimer::getElapsedSeconds() + (F64)llmax(1.f, (F32)ttl);

    const U32 index = (U32)rg.mGhosts.size();
    rg.mGhosts.push_back(ghost);
    rg.mByLocalID[local_id] = index;
    if (local_id != planned_local) rg.mByLocalID[planned_local] = index;

    ++rg.mInjected;
    if (created) ++mMetrics.mCreated; else ++mMetrics.mValidated;

    if (rg.mFirstInject == 0.0) rg.mFirstInject = (F64)LLTimer::getElapsedSeconds();

    return true;
}

// ---------------------------------------------------------------------------
// Retirement, confirmation and the TTL sweep
// ---------------------------------------------------------------------------

void SSROCGhostMgr::retireGhost(LLViewerRegion* regionp, RegionGhosts& rg, Ghost& ghost, const char* why)
{
    if (ghost.mState != SSROC_GHOST_LIVE) return;

    ghost.mState = SSROC_GHOST_RETIRED;
    mLiveGhosts.erase(ghost.mFullID);

    // <SS:Nexii> ROC ONLY DESTROYS WHAT ROC INVENTED. A ghost that merely VALIDATED an entry the simulator itself wrote into the .slc is the simulator's object, made visible early - the bytes are its own and its existence was never ROC's claim to make. Killing one of those on a timer removes a real object from a real region, which is precisely what was happening: a terrain surround made visible while the agent stood in its region, then derezzed once the agent crossed the border, alt-cammable to point blank and simply not there, returning the moment the region was re-entered.
    //
    // The timer was measuring nothing by then. Confirmation arrives through cacheFullUpdate and probeCache, and the simulator probes the region the agent is IN - so the instant the agent leaves, non-confirmation stops being evidence of anything and becomes a certainty. The comment this replaces had the premise exactly right, that silence is the interest list neglecting an object rather than the object being gone, and then killed it anyway.
    //
    // On the owner's own data every single ghost is validated and none are created, so this makes the destructive half of retirement unreachable in practice rather than merely rarer.
    if (ghost.mCreatedEntry)
    {
        LLViewerObject* obj = gObjectList.findObject(ghost.mFullID);
        if (obj && !obj->isDead() && obj->getRegion() == regionp)
        {
            gObjectList.killObject(obj);
        }

        // The invented cache entry has to go too, or createVisibleObjects rebuilds the object from it on the very next frame. This returns the region to exactly the state it was in before injection.
        if (regionp) regionp->killCacheEntry(ghost.mLocalID);
    }
    else
    {
        // Nothing to undo: the entry was the simulator's, it is still the simulator's, and it goes back to being an ordinary untrusted entry that a later probe validates normally. All that retires here is ROC's interest in watching it.
        ++mMetrics.mReleasedValidated;
    }

    LL_DEBUGS("SSROC") << "Retired ghost " << ghost.mFullID << " in region " << rg.mHandle << ": " << why << LL_ENDL;
}

void SSROCGhostMgr::confirmGhost(RegionGhosts& rg, Ghost& ghost, U32 flags)
{
    if (ghost.mState != SSROC_GHOST_LIVE) return;

    ghost.mState = SSROC_GHOST_CONFIRMED;
    mLiveGhosts.erase(ghost.mFullID);
    ++mMetrics.mConfirmed;

    // A confirming dupe only writes the simulator's flags to the CACHE ENTRY (llviewerregion.cpp:2985); the object standing in the world is never told. Without this, a ghost that was rezzed with blank flags keeps them for the rest of the visit and the object silently offers no touch or sit affordance it really has. The last flags the ledger saw stand in when the confirming message carried none, since they are still the simulator's word rather than ours.
    const U32 restore = (flags != (U32)-1) ? flags : ghost.mSimFlags;
    if (restore != (U32)-1)
    {
        LLViewerObject* obj = gObjectList.findObject(ghost.mFullID);
        if (obj && !obj->isDead()) obj->loadFlags(restore);
    }

    LL_DEBUGS("SSROC") << "Confirmed ghost " << ghost.mFullID << " in region " << rg.mHandle << LL_ENDL;
}

void SSROCGhostMgr::sweepTTL(LLViewerRegion* regionp, RegionGhosts& rg, S32& budget)
{
    const size_t n = rg.mGhosts.size();
    if (!n || budget <= 0) return;

    const F64 now = (F64)LLTimer::getElapsedSeconds();

    // A rolling cursor with a scan ceiling, not a full walk. Bounding only the number of RETIREMENTS would still traverse every ghost on every frame where none happened to expire, which is almost all of them - a fully populated region would pay thirty thousand comparisons a frame to find nothing. At this rate the whole set is swept in well under a second, which is finer granularity than a two-minute TTL needs.
    const size_t scan_cap = llmin(n, (size_t)1024);

    size_t checked = 0;
    while (checked < scan_cap && budget > 0)
    {
        if (rg.mTTLSweep >= n) rg.mTTLSweep = 0;

        Ghost& ghost = rg.mGhosts[rg.mTTLSweep];
        ++rg.mTTLSweep;
        ++checked;

        if (ghost.mState != SSROC_GHOST_LIVE) continue;
        if (ghost.mDeadline > now) continue;

        retireGhost(regionp, rg, ghost, "ttl");
        ++mMetrics.mExpired;
        --budget;
    }
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

void SSROCGhostMgr::noteSimUpdate(LLViewerRegion* regionp, U32 local_id, U32 crc, U32 flags, const U8* blob, S32 blob_len)
{
    if (!regionp || mRegions.empty()) return;

    RegionGhosts* rgp = stateFor(regionp->getHandle());
    if (!rgp) return;

    // Mode-B shadow accounting, above the ghost lookup because in mode B there are no ghosts to look up. It reads the FullID the simulator itself put at blob offset zero and strikes it off the plan that was never painted; it changes nothing, decides nothing and paints nothing.
    // Gated on the PLANNED count rather than on the remaining set, or the denominator would stop counting the moment the last planned object arrived and "delivered 900 of 900 sightings" would read as total coverage of a region that in fact sent four thousand.
    if (rgp->mShadowPlanned && blob && blob_len >= SSROC_BLOB_PREFIX_BYTES)
    {
        ++rgp->mShadowSightings;

        LLUUID shadow_id;
        LLDataPackerBinaryBuffer sdp(const_cast<U8*>(blob), blob_len);
        LLViewerObject::unpackUUID(&sdp, shadow_id, "ID");

        if (shadow_id.notNull() && rgp->mShadow.erase(shadow_id)) ++rgp->mShadowDelivered;
    }

    if (rgp->mByLocalID.empty()) return;

    auto it = rgp->mByLocalID.find(local_id);
    if (it == rgp->mByLocalID.end()) return;

    Ghost& ghost = rgp->mGhosts[it->second];
    if (ghost.mState != SSROC_GHOST_LIVE) return;

    // Which object is the simulator actually talking about? A full update carries the answer in its blob. A bare ObjectUpdateCached probe carries no identity at all, so what reaches this is the entry's own description - and at this local id that entry is the one the ROC injected, which is exactly the identity being confirmed.
    if (blob && blob_len >= SSROC_BLOB_PREFIX_BYTES)
    {
        LLUUID id;
        LLDataPackerBinaryBuffer dp(const_cast<U8*>(blob), blob_len);
        LLViewerObject::unpackUUID(&dp, id, "ID");

        if (id.notNull() && id != ghost.mFullID)
        {
            // The id was recycled onto a different object. That is neutral, not a refutation: KillObject carries no reason and a reused id says nothing about where the old object went. The stock code has already replaced the cache entry with the new object's, so only the stale visual is ours to remove - and the entry must be left alone.
            ghost.mState = SSROC_GHOST_RETIRED;
            mLiveGhosts.erase(ghost.mFullID);

            LLViewerObject* obj = gObjectList.findObject(ghost.mFullID);
            if (obj && !obj->isDead() && obj->getRegion() == regionp)
            {
                gObjectList.killObject(obj);
            }

            ++mMetrics.mIdReuse;
            return;
        }
    }

    // A changed CRC is still a confirmation - the object is there, it just is not quite what we remembered, and rewriting the record is the exit pass's job rather than this one's.
    if (crc != ghost.mCRC) ghost.mCRC = crc;

    confirmGhost(*rgp, ghost, flags);
}

void SSROCGhostMgr::noteKill(LLViewerRegion* regionp, U32 local_id, const LLUUID& full_id)
{
    if (!regionp || mRegions.empty()) return;

    RegionGhosts* rgp = stateFor(regionp->getHandle());
    if (!rgp || rgp->mByLocalID.empty()) return;

    auto it = rgp->mByLocalID.find(local_id);
    if (it == rgp->mByLocalID.end()) return;

    Ghost& ghost = rgp->mGhosts[it->second];
    if (ghost.mState != SSROC_GHOST_LIVE) return;
    if (full_id.notNull() && full_id != ghost.mFullID) return;

    // The stock kill handler is already doing the killing, both of the object and of the cache entry, so this only records that the ghost is gone.
    ghost.mState = SSROC_GHOST_RETIRED;
    mLiveGhosts.erase(ghost.mFullID);
    ++mMetrics.mKilled;

    // Phase 2 issues no select probes of any kind, so there is no such thing here as a kill we provoked ourselves - every one of these is a kill the simulator volunteered, which is the strong form of the evidence.
    ++rgp->mRefutations;

    if (rgp->mTrashed || rgp->mFirstInject == 0.0) return;

    const F64 now = (F64)LLTimer::getElapsedSeconds();
    if (now - rgp->mFirstInject > SSROC_TRASH_WINDOW_SECS) return;

    static LLCachedControl<U32> trash_pct(gSavedSettings, "SSROCTrashThresholdPct", 20);
    const U32 need = llmax(SSROC_TRASH_MIN_REFUTATIONS, (U32)(((U64)rgp->mInjected * (U64)(U32)trash_pct) / 100));

    if (rgp->mRefutations >= need) trashRegion(regionp, *rgp);
}

void SSROCGhostMgr::trashRegion(LLViewerRegion* regionp, RegionGhosts& rg)
{
    rg.mTrashed = true;
    rg.mArmed   = false;
    rg.mNextPlan = rg.mPlan.size();
    ++mMetrics.mTrashed;

    // Every ghost still standing is brought forward to the next TTL sweep rather than killed here, so a region that injected thousands of them is dismantled over a few frames instead of one long stall.
    const F64 now = (F64)LLTimer::getElapsedSeconds();
    for (Ghost& ghost : rg.mGhosts)
    {
        if (ghost.mState == SSROC_GHOST_LIVE) ghost.mDeadline = now;
    }

    mTrashedRegions.push_back(rg.mHandle);

    LL_WARNS("SSROC") << "Region " << (regionp ? regionp->getName() : std::string("?")) << " (" << rg.mHandle << "): "
                      << rg.mRefutations << " of " << rg.mInjected
                      << " ghosts were killed by the simulator within " << (S32)SSROC_TRASH_WINDOW_SECS
                      << "s of arrival - the remembered region is gone, discarding its object ledger" << LL_ENDL;
}

// ---------------------------------------------------------------------------
// Suppression
// ---------------------------------------------------------------------------

void SSROCGhostMgr::applySuppression(LLViewerObject* objectp)
{
    if (!objectp || mLiveGhosts.empty()) return;
    if (mLiveGhosts.find(objectp->getID()) == mLiveGhosts.end()) return;

    static LLCachedControl<bool> suppress(gSavedSettings, "SSROCGhostSuppress", true);
    if (!(bool)suppress) return;

    // A ghost is a guess about what is standing here, so it must not behave as though it were the object itself. Sound is the one that carries across a whole parcel and the one a user would notice from a demolished build; a spin is the other thing that reads as "this is alive" rather than "this is scenery".
    //
    // Hover text, particles and MOAP are unpacked and applied inside LLViewerObject::processUpdate itself and there is no public way back out of them, so those are not suppressed in this phase - see doc/region_object_cache.md.
    objectp->setAttachedSound(LLUUID::null, objectp->getID(), 0.f, LL_SOUND_FLAG_STOP);
    objectp->setAngularVelocity(LLVector3::zero);

    // Selectability is a deliberate default-ON, against the general "a ghost must not behave like the object" rule, and it is settled owner decision 1 in doc/region_object_cache.md: in mode A a ghost sits at the simulator's own local id, so edit or right-click select reaches the real object and auto-culls a ghost that is no longer there. Under a future mode B the id would be one the simulator never issued, a grab would be nonsense on the wire and touch and sit would silently fail - so the switch exists now, defaulted to today's behaviour, rather than being invented under pressure later.
    static LLCachedControl<bool> selectable(gSavedSettings, "SSROCGhostSelectable", true);
    if (!(bool)selectable) objectp->mbCanSelect = false;
}

// ---------------------------------------------------------------------------
// .slc isolation
// ---------------------------------------------------------------------------

U32 SSROCGhostMgr::purgeInjectedEntries(U64 handle, LLVOCacheEntry::vocache_entry_map_t& cache_map)
{
    auto it = mPendingPurge.find(handle);
    if (it == mPendingPurge.end()) return 0;

    U32 removed = 0;
    for (U32 local_id : it->second)
    {
        if (cache_map.erase(local_id)) ++removed;
    }
    mPendingPurge.erase(it);

    mMetrics.mPurged += removed;
    return removed;
}

// ---------------------------------------------------------------------------
// Per-frame work
// ---------------------------------------------------------------------------

void SSROCGhostMgr::tick()
{
    if (!enabled())
    {
        if (!mRegions.empty()) { mRegions.clear(); mLiveGhosts.clear(); }
        return;
    }
    if (mRegions.empty()) return;
    if (!LLWorld::instanceExists()) return;

    LLWorld* world = LLWorld::getInstance();

    // Regions the world has already forgotten. Reached on any teardown path that never routed through removeRegion.
    for (auto it = mRegions.begin(); it != mRegions.end(); )
    {
        if (world->getRegionFromHandle(it->first))
        {
            ++it;
        }
        else
        {
            for (const Ghost& ghost : it->second.mGhosts)
            {
                if (ghost.mState == SSROC_GHOST_LIVE) mLiveGhosts.erase(ghost.mFullID);
            }
            it = mRegions.erase(it);
        }
    }
    if (mRegions.empty()) return;

    static LLCachedControl<U32> per_frame(gSavedSettings, "SSROCInjectPerFrame", 200);
    static LLCachedControl<F32> ms_per_frame(gSavedSettings, "SSROCInjectMsPerFrame", 2.0f);

    // Two budgets, both enforced. The object count keeps a cheap frame from injecting the whole region at once during the teleport screen, where the stock creation throttle is deliberately cancelled and would not stop it; the millisecond ceiling is what actually protects the frame when individual injections turn out to be expensive.
    S32       budget    = (S32)llmax((U32)1, (U32)per_frame);
    const F32 time_cap  = llmax(0.1f, (F32)ms_per_frame) * 0.001f;
    LLTimer   frame_timer;

    // Ghosts belong to the agent's own region in this phase. A neighbour's interest list is distance-gated and there is nothing that can verify one, so neighbour ghosts would mass-expire at TTL having shown the user a region's worth of objects that may not be there.
    LLViewerRegion* agent_region = gAgent.getRegion();

    if (agent_region)
    {
        // Arming needs BOTH the handshake, which alone says whether the stored local ids still mean anything, and the disk read, which alone says whether there is anything to paint. The two race in both directions and the handshake usually wins on a large file or a busy worker pool, so waiting on the handshake alone wrote a loaded region off as "no file" for the whole visit. The wait is bounded rather than unbounded because a read that never completes must not leave the region permanently unexplained.
        static LLCachedControl<F32> arm_wait(gSavedSettings, "SSROCGhostArmWaitSecs", 8.0f);

        RegionGhosts* rgp = stateFor(agent_region->getHandle());
        if (rgp && rgp->mHaveHandshake && !rgp->mPlanBuilt)
        {
            const F64 waited = (F64)LLTimer::getElapsedSeconds() - rgp->mHandshakeAt;
            if (rgp->mLoadDone || waited >= (F64)llmax(0.f, (F32)arm_wait))
            {
                armRegion(agent_region, *rgp);
            }
        }

        if (rgp && rgp->mArmed && !rgp->mTrashed)
        {
            RegionGhosts& rg = *rgp;
            while (rg.mNextPlan < rg.mPlan.size() && budget > 0)
            {
                const U32 index = rg.mPlan[rg.mNextPlan++];
                if (index < rg.mFile->mRecords.size())
                {
                    if (injectOne(agent_region, rg, rg.mFile->mRecords[index])) --budget;
                }

                // Checked every sixteenth record rather than every one: the timer read is not free and the point is to bound the frame, not to be exact about where in it we stopped.
                if ((rg.mNextPlan & 0xF) == 0 && frame_timer.getElapsedTimeF32() > time_cap) break;
            }

            if (rg.mNextPlan >= rg.mPlan.size())
            {
                rg.mArmed = false;
                LL_INFOS("SSROC") << "Region " << rg.mHandle << ": ghost injection complete, "
                                  << rg.mInjected << " of " << rg.mPlan.size() << " planned objects painted" << LL_ENDL;
            }
        }
    }

    // The TTL sweep runs for every tracked region, not only the agent's: ghosts injected before the agent walked across a border still have to expire on schedule.
    S32 ttl_budget = (S32)llmax((U32)1, (U32)per_frame);
    for (auto& pair : mRegions)
    {
        if (ttl_budget <= 0 || frame_timer.getElapsedTimeF32() > time_cap * 2.f) break;

        LLViewerRegion* regionp = world->getRegionFromHandle(pair.first);
        if (!regionp) continue;

        sweepTTL(regionp, pair.second, ttl_budget);
    }
}

std::string SSROCGhostMgr::metricsString() const
{
    // The outcome histogram is the headline, not a footnote: the single most valuable question this feature can answer at the end of a session is "of the regions you entered, how many declined and for what reason", and it is the question the old summary was structurally unable to answer.
    std::string outcomes;
    for (U8 i = 0; i < SSROC_ARM_COUNT; ++i)
    {
        if (!mMetrics.mOutcomes[i]) continue;
        if (!outcomes.empty()) outcomes += " ";
        outcomes += llformat("%s=%u", ssROCArmOutcomeName(i), mMetrics.mOutcomes[i]);
    }
    if (outcomes.empty()) outcomes = "none";

    return llformat("ROC ghosts: regions armed %u (mode B %u) | outcomes: %s | validated %u created %u skipped %u | confirmed %u expired %u id-reuse %u killed %u | trashed %u purged %u | cache misses refused %u",
                    mMetrics.mRegionsArmed, mMetrics.mRegionsModeB, outcomes.c_str(),
                    mMetrics.mValidated, mMetrics.mCreated, mMetrics.mSkipped,
                    mMetrics.mConfirmed, mMetrics.mExpired, mMetrics.mIdReuse, mMetrics.mKilled,
                    mMetrics.mTrashed, mMetrics.mPurged, mMetrics.mMissedSends);
}

// ---------------------------------------------------------------------------
// Stock entry points
// ---------------------------------------------------------------------------

void ssROCGhostTick()
{
    if (!SSROCGhostMgr::instanceExists()) return;
    SSROCGhostMgr::instance().tick();
}

void ssROCGhostNoteKill(LLViewerRegion* regionp, U32 local_id, const LLUUID& full_id)
{
    if (!regionp || !SSROCGhostMgr::instanceExists()) return;
    SSROCGhostMgr::instance().noteKill(regionp, local_id, full_id);
}

bool ssROCRefuseCacheMiss(U32 local_id)
{
    if (local_id < SSROC_SYNTH_ID_FLOOR) return false;

    // Reached only if a grid starts allocating in the range this fork reserved, which is a fact worth one loud line rather than a silent drop. Warned once per session and counted thereafter, because the caller is on the object update path and a per-object warning would be its own denial of service.
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        LL_WARNS("SSROC") << "Refusing to queue a cache miss for local id " << local_id
                          << ": it falls in the range the region object cache reserves, so this grid's id allocation no longer matches what the reservation assumed" << LL_ENDL;
    }

    if (SSROCGhostMgr::instanceExists()) SSROCGhostMgr::instance().noteRefusedCacheMiss();
    return true;
}

void ssROCGhostSuppress(LLViewerObject* objectp)
{
    if (!objectp || !SSROCGhostMgr::instanceExists()) return;
    SSROCGhostMgr::instance().applySuppression(objectp);
}

void ssROCPurgeInjectedEntries(LLViewerRegion* regionp, LLVOCacheEntry::vocache_entry_map_t& cache_map)
{
    if (!regionp || cache_map.empty() || !SSROCGhostMgr::instanceExists()) return;

    const U32 removed = SSROCGhostMgr::instance().purgeInjectedEntries(regionp->getHandle(), cache_map);
    if (removed)
    {
        LL_INFOS("SSROC") << "Withheld " << removed << " unconfirmed cache entries from the object cache file for region "
                          << regionp->getHandle() << LL_ENDL;
    }
}
