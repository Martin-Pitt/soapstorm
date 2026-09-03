/**
 * @file ssrocvocache.cpp
 * @brief Region Object Cache as the backing store for the stock protocol object cache - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssrocvocache.h"

#include "ssrocaux.h"
#include "ssrocprobe.h"   // <SS:Nexii/> ROC Phase 1.5 probe-flood measurement
#include "ssrocledger.h"

#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"

#include <map>
#include <sstream>

namespace
{
    // Per-region state for the current visit. Handle-keyed rather than held on LLViewerRegion, so absorbing the object cache costs the stock class no new member and no new header line. Entered by ssROCLoadObjectCache and released by ssROCNoteObjectCacheSaved, which the region destructor always reaches.
    struct BackedRegion
    {
        U8  mOutcome   = SSROC_BACK_DISABLED;
        U32 mSeeded    = 0;
        F32 mSeedMs    = 0.f;
    };

    std::map<U64, BackedRegion> gBacked;

    struct Metrics
    {
        U32 mRegionsBacked = 0;
        U32 mRegionsStock  = 0;
        U32 mEntriesSeeded = 0;
        U64 mProbeHits     = 0;   // across ROC-backed regions only - the stock ones are the control group
        U64 mProbeMisses   = 0;
        U32 mOutcomes[SSROC_BACK_COUNT] = { 0 };
        U32 mSinkRefusals[SSROC_SINK_COUNT] = { 0 };
        U32 mSeedRefusals[SSROC_SEED_COUNT] = { 0 };
    };

    Metrics gMetrics;
}

const char* ssROCSinkRefusalName(U8 refusal)
{
    switch (refusal)
    {
        case SSROC_SINK_OK:            return "ok";
        case SSROC_SINK_ID_OCCUPIED:   return "id already live";
        case SSROC_SINK_BLOB_SHORT:    return "blob too short";
        case SSROC_SINK_BLOB_MISMATCH: return "blob disagrees with record";
        default:                       return "unknown";
    }
}

bool ssROCBacksObjectCache()
{
    if (!SSROCStore::enabled()) return false;

    // ObjectCacheEnabled is the user's own switch for caching objects at all, and backing the cache from a different file must not quietly defeat it. Latched on first use rather than read live, because LLVOCache latches it in its constructor (llvocache.cpp:1190) - reading it live here would let a mid-session flip turn one of the two on while the other stayed off, which is the sort of divergence that is only ever discovered from a confusing bug report.
    static const bool stock_cache_enabled = gSavedSettings.getBOOL("ObjectCacheEnabled");
    if (!stock_cache_enabled) return false;

    static LLCachedControl<bool> backing(gSavedSettings, "SSROCBackObjectCache", false);
    return (bool)backing;
}

bool ssROCObjectCacheIsBacked(U64 handle)
{
    auto it = gBacked.find(handle);
    return it != gBacked.end() && it->second.mOutcome == SSROC_BACK_SEEDED;
}

bool ssROCLoadObjectCache(LLViewerRegion* regionp, LLVOCacheEntry::vocache_entry_map_t& cache_map)
{
    if (!regionp || !ssROCBacksObjectCache()) return false;
    if (!SSROCLedger::instanceExists() || !SSROCAuxMgr::instanceExists()) return false;

    const U64 handle = regionp->getHandle();

    BackedRegion& br = gBacked[handle];
    br = BackedRegion();

    LLTimer timer;

    // The .roc has to be resident before the map can be answered for, because the answer sets bit 1 of the RegionHandshakeReply and therefore decides whether the simulator sends cache probes at all. On the common path the worker read has already landed and this does nothing; when it has not, it reads the file here - standing exactly where the stock synchronous .slc read stands, and replacing it rather than adding to it.
    SSROCAuxMgr::instance().ensureRegionLoaded(handle);

    static LLCachedControl<U32> max_seed(gSavedSettings, "SSROCBackMaxSeed", 0);

    U32 sink_refusals[SSROC_SINK_COUNT] = { 0 };

    SSROCSeedStats stats;
    U8  outcome = SSROC_BACK_DISABLED;

    const U32 seeded = SSROCLedger::instance().seedObjectCache(handle, (U32)max_seed,
        [&cache_map, &sink_refusals](const SSROCRecord& rec) -> bool
        {
            // The live stream always wins. An id already in the map got there from the simulator's own words this visit, and nothing remembered may stand over it.
            if (cache_map.find(rec.mLastLocalID) != cache_map.end())
            {
                ++sink_refusals[SSROC_SINK_ID_OCCUPIED];
                return false;
            }

            // The ledger's own parser rather than a second copy of the blob offsets, so there is exactly one place in the tree that knows where a cached object's local id and CRC live.
            SSROCBlobFacts facts;
            if (!ssROCParseBlob(rec.mDP.data(), (S32)rec.mDP.size(), facts))
            {
                ++sink_refusals[SSROC_SINK_BLOB_SHORT];
                return false;
            }

            // An .slc entry's header and its body come out of one message and can never disagree about which object they describe. A ROC record's header is re-keyed on every sighting while its blob is only replaced when the object is described in full, so the two are checked against each other here rather than assumed - and a disagreement means the entry is skipped and the object requested, which is the harmless direction.
            if (facts.mLocalID != rec.mLastLocalID || facts.mCRC != rec.mCRC || facts.mFullID != rec.mFullID)
            {
                ++sink_refusals[SSROC_SINK_BLOB_MISMATCH];
                return false;
            }

            // Non-const because LLVOCacheEntry copies out of a packer rather than out of a buffer; the packer never writes through it.
            std::vector<U8>& blob = const_cast<std::vector<U8>&>(rec.mDP);
            LLDataPackerBinaryBuffer dp(blob.data(), (S32)blob.size());

            LLPointer<LLVOCacheEntry> entry = new LLVOCacheEntry(rec.mLastLocalID, rec.mCRC, dp);

            // THE ONE LINE THAT MAKES THIS EQUIVALENT TO A .slc READ. The three-argument constructor is the one the message path uses and it marks the entry VALID; the file constructor marks it INVALID. probeCache treats a valid entry as already probed and returns without calling decodeBoundingInfo, so an entry seeded as valid would answer every probe and never enter the cache octree - the objects would be remembered and then never rezzed.
            entry->setValid(false);

            cache_map[rec.mLastLocalID] = entry;
            return true;
        },
        stats, outcome);

    br.mOutcome = outcome;
    br.mSeeded  = seeded;
    br.mSeedMs  = timer.getElapsedTimeF32() * 1000.f;

    if (outcome < SSROC_BACK_COUNT) ++gMetrics.mOutcomes[outcome];
    for (U32 i = 0; i < SSROC_SEED_COUNT; ++i) gMetrics.mSeedRefusals[i] += stats.mCounts[i];
    for (U32 i = 0; i < SSROC_SINK_COUNT; ++i) gMetrics.mSinkRefusals[i] += sink_refusals[i];

    if (outcome != SSROC_BACK_SEEDED || seeded == 0)
    {
        // Declined, by name and with the reason attached, and the stock .slc path runs for this region exactly as it does today. A decline is never silent here: "the cache was not used" and "the cache was empty" look identical from the outside and this project has already paid for that once on the ghost path.
        ++gMetrics.mRegionsStock;
        br.mOutcome = (outcome == SSROC_BACK_SEEDED) ? SSROC_BACK_PLAN_EMPTY : outcome;

        LL_INFOS("SSROC") << "Region " << handle << ": object cache NOT backed by the region cache ("
                          << ssROCBackOutcomeName(br.mOutcome) << "), falling back to the stock object cache file"
                          << " | records offered: " << stats.describe() << LL_ENDL;
        return false;
    }

    ++gMetrics.mRegionsBacked;
    gMetrics.mEntriesSeeded += seeded;

    std::ostringstream sinks;
    bool any_sink = false;
    for (U32 i = 1; i < SSROC_SINK_COUNT; ++i)
    {
        if (!sink_refusals[i]) continue;
        if (any_sink) sinks << ", ";
        any_sink = true;
        sinks << ssROCSinkRefusalName((U8)i) << " " << sink_refusals[i];
    }

    LL_INFOS("SSROC") << "Region " << handle << ": object cache backed by the region cache, " << seeded
                      << " entries seeded in " << br.mSeedMs << " ms"
                      << " | records offered: " << stats.describe()
                      << (any_sink ? " | entries refused: " : " | entries refused: none") << (any_sink ? sinks.str() : std::string())
                      << LL_ENDL;

    return true;
}

void ssROCNoteObjectCacheSaved(LLViewerRegion* regionp)
{
    if (!regionp) return;

    // <SS:Nexii/> ROC Phase 1.5: saveObjectCache runs in the region destructor, so this is the last moment the measurement can be reported before the region it describes is gone. Above the gBacked lookup below, which returns early on every region the cache never backed - including all of them when ROC is off, which is exactly when this needs to report.
    ssROCProbeReport(regionp);

    const U64 handle = regionp->getHandle();

    auto it = gBacked.find(handle);
    if (it == gBacked.end()) return;

    const bool backed = it->second.mOutcome == SSROC_BACK_SEEDED;

    // The number that decides whether absorbing the object cache was worth doing. Probe hits are objects the simulator did not have to send; misses are the requests it did have to answer. Reported per region beside how the map was filled, so a regression shows up as a hit rate that fell on backed regions rather than as a vague report of slow rezzing.
    const U64 hits   = regionp->getRegionCacheHitCount();
    const U64 misses = regionp->getRegionCacheMissCount();
    const U64 total  = hits + misses;

    if (backed)
    {
        gMetrics.mProbeHits   += hits;
        gMetrics.mProbeMisses += misses;
    }

    LL_INFOS("SSROC") << "Region " << handle << " object cache: " << (backed ? "region-cache backed" : "stock")
                      << " (" << ssROCBackOutcomeName(it->second.mOutcome) << ")"
                      << ", seeded " << it->second.mSeeded
                      << ", probe hits " << hits << " of " << total
                      << " (" << (total ? (F32)hits * 100.f / (F32)total : 0.f) << "%)" << LL_ENDL;

    gBacked.erase(it);
}

std::string ssROCVOCacheMetricsString()
{
    std::ostringstream os;
    const U64 total = gMetrics.mProbeHits + gMetrics.mProbeMisses;

    os << "vocache: backed " << gMetrics.mRegionsBacked << " regions, stock " << gMetrics.mRegionsStock
       << ", seeded " << gMetrics.mEntriesSeeded << " entries"
       << ", probe hit rate on backed regions " << (total ? (F32)gMetrics.mProbeHits * 100.f / (F32)total : 0.f) << "%";

    for (U32 i = 0; i < SSROC_BACK_COUNT; ++i)
    {
        if (gMetrics.mOutcomes[i]) os << " | " << ssROCBackOutcomeName((U8)i) << " " << gMetrics.mOutcomes[i];
    }
    for (U32 i = 1; i < SSROC_SEED_COUNT; ++i)
    {
        if (gMetrics.mSeedRefusals[i]) os << " | record " << ssROCSeedRefusalName((U8)i) << " " << gMetrics.mSeedRefusals[i];
    }
    for (U32 i = 1; i < SSROC_SINK_COUNT; ++i)
    {
        if (gMetrics.mSinkRefusals[i]) os << " | entry " << ssROCSinkRefusalName((U8)i) << " " << gMetrics.mSinkRefusals[i];
    }

    return os.str();
}
