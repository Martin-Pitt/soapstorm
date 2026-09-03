/**
 * @file ssrocprobe.cpp
 * @brief ROC Phase 1.5 gate - measures the simulator's cached-object probe flood, see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssrocprobe.h"

#include "llagent.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// <SS:Nexii> See the header for what this measures and for the four ways the first cut of it was biased toward its own conclusion.
namespace
{
    // A region can hold 30,000 records. Samples are only read once, at exit, so this bounds a session that never leaves rather than trading away accuracy. Reaching it is reported rather than silently truncating the distribution.
    const size_t SSROCP_MAX_SAMPLES = 50000;

    struct RegionMeasure
    {
        std::string             mName;
        LLTimer                 mClock;             // real time, NOT LLFrameTimer: probes arrive from the network many times per frame and a frame-quantised clock collapses them onto one timestamp
        U32                     mCachedEntries{0};
        ESSROCProbeSource       mSource{SSROCP_SRC_SLC};
        U32                     mOutcomes[SSROCP_OUTCOME_COUNT]{0, 0, 0};
        U32                     mProbeEvents{0};    // every probe that arrived, including re-probes of the same object
        U32                     mFullUpdates{0};
        bool                    mReported{false};
        bool                    mSampleCapHit{false};
        std::unordered_set<U32> mProbedIds;         // distinct local ids seen: the coverage numerator
        std::vector<U32>        mProbeMs;           // ms since arrival, one per DISTINCT id, so index N really is the Nth entry covered
    };

    std::mutex sMutex;
    std::map<U64, RegionMeasure> sByHandle;

    bool measuring()
    {
        static LLCachedControl<bool> on(gSavedSettings, "SSROCProbeMeasure", false);
        return on;
    }

    U32 percentile(const std::vector<U32>& sorted, F32 p)
    {
        if (sorted.empty()) return 0;
        const size_t i = (size_t)(p * (F32)(sorted.size() - 1));
        return sorted[llmin(i, sorted.size() - 1)];
    }

    // Time by which a given fraction of the region's cached entries had been covered. Returns false for "that fraction was never reached", which is a genuinely different answer from "reached at 0 ms" - the first cut used 0 for both and a small cache reported every fraction as never reached.
    bool msToCoverage(const std::vector<U32>& sorted, U32 entries, F32 fraction, U32& out_ms)
    {
        out_ms = 0;
        if (!entries || sorted.empty()) return false;

        // Round up, so a 3-entry cache asks for 2 entries at 50% rather than 1, and never asks for 0.
        size_t need = (size_t)((fraction * (F32)entries) + 0.999f);
        if (need == 0) need = 1;
        if (need > sorted.size()) return false;

        out_ms = sorted[need - 1];
        return true;
    }
}

void ssROCProbeNoteRegionLoad(LLViewerRegion* regionp, U32 cached_entries, ESSROCProbeSource source)
{
    if (!regionp || !measuring()) return;

    std::lock_guard<std::mutex> lock(sMutex);
    RegionMeasure& m = sByHandle[regionp->getHandle()];
    // Re-entering a region the session already measured restarts the clock rather than appending, because the interesting quantity is per arrival - a second visit with a warm cache is a different experiment from the first.
    m.mName          = regionp->getName();
    m.mCachedEntries = cached_entries;
    m.mSource        = source;
    for (U32 i = 0; i < SSROCP_OUTCOME_COUNT; ++i) m.mOutcomes[i] = 0;
    m.mProbeEvents   = 0;
    m.mFullUpdates   = 0;
    m.mReported      = false;
    m.mSampleCapHit  = false;
    m.mProbedIds.clear();
    m.mProbeMs.clear();
    m.mClock.reset();

    LL_INFOS("SSROCProbe") << "measuring \"" << m.mName << "\": " << cached_entries << " entries at arrival, from "
                           << (source == SSROCP_SRC_ROC ? "the region object cache" : "the stock .slc") << LL_ENDL;
}

void ssROCProbeNoteProbe(LLViewerRegion* regionp, U32 local_id, ESSROCProbeOutcome outcome)
{
    if (!regionp || !measuring()) return;

    std::lock_guard<std::mutex> lock(sMutex);
    std::map<U64, RegionMeasure>::iterator it = sByHandle.find(regionp->getHandle());
    if (it == sByHandle.end()) return;   // probes for a region whose arrival was never seen: nothing to measure them against

    RegionMeasure& m = it->second;
    ++m.mProbeEvents;
    if (outcome < SSROCP_OUTCOME_COUNT) ++m.mOutcomes[outcome];

    // Coverage is about objects, not messages. A re-probe of an id already seen is a real event and is counted as one, but it covers nothing new, so it must not advance the coverage curve.
    if (!m.mProbedIds.insert(local_id).second) return;

    if (m.mProbeMs.size() < SSROCP_MAX_SAMPLES)
    {
        m.mProbeMs.push_back((U32)((F32)m.mClock.getElapsedTimeF32() * 1000.f));
    }
    else
    {
        m.mSampleCapHit = true;
    }
}

void ssROCProbeNoteFullUpdate(LLViewerRegion* regionp)
{
    if (!regionp || !measuring()) return;

    std::lock_guard<std::mutex> lock(sMutex);
    std::map<U64, RegionMeasure>::iterator it = sByHandle.find(regionp->getHandle());
    if (it == sByHandle.end()) return;
    ++it->second.mFullUpdates;
}

void ssROCProbeReport(LLViewerRegion* regionp)
{
    if (!regionp || !measuring()) return;

    std::lock_guard<std::mutex> lock(sMutex);
    std::map<U64, RegionMeasure>::iterator it = sByHandle.find(regionp->getHandle());
    if (it == sByHandle.end()) return;

    RegionMeasure& m = it->second;
    if (m.mReported) return;
    m.mReported = true;

    std::vector<U32> sorted = m.mProbeMs;
    std::sort(sorted.begin(), sorted.end());

    const U32 distinct = (U32)m.mProbedIds.size();
    const F32 secs = (F32)m.mClock.getElapsedTimeF32();
    const U32 coverage_pct = m.mCachedEntries ? (U32)(100.f * (F32)distinct / (F32)m.mCachedEntries) : 0;

    LL_INFOS("SSROCProbe") << "==== probe flood, \"" << m.mName << "\" after " << (S32)secs << "s ====" << LL_ENDL;
    LL_INFOS("SSROCProbe") << "  entries at arrival         " << m.mCachedEntries
                           << "  (from " << (m.mSource == SSROCP_SRC_ROC ? "the region object cache" : "the stock .slc") << ")" << LL_ENDL;
    LL_INFOS("SSROCProbe") << "  probes arrived             " << m.mProbeEvents
                           << "  (" << distinct << " distinct objects, " << (m.mProbeEvents - distinct) << " re-probes)" << LL_ENDL;
    LL_INFOS("SSROCProbe") << "    cache answered           " << m.mOutcomes[SSROCP_HIT] << LL_ENDL;
    LL_INFOS("SSROCProbe") << "    probed but copy stale    " << m.mOutcomes[SSROCP_CRC_MISS]
                           << "   (the simulator DID probe - do not read these as a gap in the flood)" << LL_ENDL;
    LL_INFOS("SSROCProbe") << "    never heard of it        " << m.mOutcomes[SSROCP_TOTAL_MISS] << LL_ENDL;
    LL_INFOS("SSROCProbe") << "  sent whole instead         " << m.mFullUpdates << LL_ENDL;

    // The coverage lines are the ones the gate turns on, so they say plainly what they are a fraction OF. On the ROC arm the denominator is ROC's accumulated record set rather than the .slc, which is a larger and differently-shaped population - a coverage number from that arm is not comparable with one from the control arm and must not be quoted as a simulator measurement.
    if (m.mSource == SSROCP_SRC_ROC)
    {
        LL_INFOS("SSROCProbe") << "  coverage                   " << coverage_pct
                               << "% of the ROC record set - NOT comparable with the SSROCEnabled=0 control arm" << LL_ENDL;
    }
    else
    {
        LL_INFOS("SSROCProbe") << "  coverage                   " << coverage_pct << "% of the .slc entries" << LL_ENDL;
    }

    if (!sorted.empty())
    {
        LL_INFOS("SSROCProbe") << "  first sighting of each object, ms since arrival:  first " << sorted.front()
                               << "  p50 " << percentile(sorted, 0.50f)
                               << "  p90 " << percentile(sorted, 0.90f)
                               << "  p99 " << percentile(sorted, 0.99f)
                               << "  last " << sorted.back() << LL_ENDL;

        U32 to50 = 0, to90 = 0;
        const bool got50 = msToCoverage(sorted, m.mCachedEntries, 0.50f, to50);
        const bool got90 = msToCoverage(sorted, m.mCachedEntries, 0.90f, to90);
        LL_INFOS("SSROCProbe") << "  time to cover half         " << (got50 ? std::to_string(to50) + " ms" : std::string("never reached")) << LL_ENDL;
        LL_INFOS("SSROCProbe") << "  time to cover 90%          " << (got90 ? std::to_string(to90) + " ms" : std::string("never reached")) << LL_ENDL;
    }

    if (m.mSampleCapHit)
    {
        LL_INFOS("SSROCProbe") << "  NOTE: hit the " << (U32)SSROCP_MAX_SAMPLES << " sample cap, so the timing lines describe the first objects only" << LL_ENDL;
    }

    // Named rather than quietly skipped: the phase spec also asks for an in-frustum versus behind-camera split. A probe arrives before decodeBoundingInfo has run for a fresh entry, so at the only moment this hook sees it there is no position to test, and testing later would measure where the object ended up rather than where it was when the probe landed. It needs its own hook further down the rez path and is not attempted here.
    LL_INFOS("SSROCProbe") << "  (in-frustum vs behind-camera split not measured - see the note in ssrocprobe.cpp)" << LL_ENDL;

    // The samples have been consumed, and a travelling session would otherwise carry every probe timestamp of every region it ever entered. The summary counters stay so a second call is still a no-op.
    m.mProbeMs.clear();
    m.mProbeMs.shrink_to_fit();
    m.mProbedIds.clear();
}

bool ssROCProbeCurrent(U32& out_entries, U32& out_distinct, U32& out_events, U32& out_full, F32& out_secs, bool& out_roc_denominator)
{
    out_entries = out_distinct = out_events = out_full = 0;
    out_secs = 0.f;
    out_roc_denominator = false;
    if (!measuring()) return false;

    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp) return false;

    std::lock_guard<std::mutex> lock(sMutex);
    std::map<U64, RegionMeasure>::iterator it = sByHandle.find(regionp->getHandle());
    if (it == sByHandle.end()) return false;

    out_entries         = it->second.mCachedEntries;
    out_distinct        = (U32)it->second.mProbedIds.size();
    out_events          = it->second.mProbeEvents;
    out_full            = it->second.mFullUpdates;
    out_secs            = (F32)it->second.mClock.getElapsedTimeF32();
    out_roc_denominator = (it->second.mSource == SSROCP_SRC_ROC);
    return true;
}
// </SS:Nexii>
