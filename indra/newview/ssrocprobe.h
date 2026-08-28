/**
 * @file ssrocprobe.h
 * @brief ROC Phase 1.5 gate - measures the simulator's cached-object probe flood, see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_ROCPROBE_H
#define SS_ROCPROBE_H

#include "lluuid.h"

class LLViewerRegion;

// <SS:Nexii> ROC Phase 1.5, the gate Phase 2 was built past. The whole object-side value claim rests on the simulator's ObjectUpdateCached probe flood being slow, incomplete, or ordered badly enough that painting remembered objects beats waiting for it - and that claim comes from a 2010-era comment. LL's interest-list work may have narrowed it since; IL_MODE_360 exists in this tree. This measures the flood directly so the claim can be settled with numbers rather than inherited.
//
// EVERY DESIGN CHOICE BELOW EXISTS TO STOP THE MEASUREMENT CONFIRMING ITS OWN HYPOTHESIS. The first cut of this
// file got all four of these wrong. They did NOT all bias the same way - (1) undercounts probes and makes the
// flood look worse than it is, which favours ROC; (2) overcounts them and makes it look better, which favours
// dropping ROC. They do not cancel, because neither error is proportional to the other and their relative size
// depends on how long the agent stands still. A measurement whose bias direction cannot be stated is not a
// weaker measurement than one that is merely skewed - it is not a measurement:
//
//   1. It hooked ssROCNoteCacheProbe, which sits inside probeCache's `if (entry)` and `if (crc == entry crc)`
//      branches. Probes that MISSED the viewer's cache were never counted, so a warm-but-stale region reported
//      a complete, prompt flood as 0% coverage. The outcome argument exists so all three branches are counted.
//   2. It counted probe EVENTS as though each were a distinct entry. The simulator re-probes objects that leave
//      and re-enter the interest list, so coverage ran past 100% and "time to cover 90%" fired early. Coverage
//      is now counted on distinct local ids; the raw event count is kept beside it, not instead of it.
//   3. It timed with LLFrameTimer, which advances once per frame, so every probe decoded in one frame shared a
//      timestamp. LLTimer is used instead - the quantity being measured is network arrival, not frame arrival.
//   4. It took its coverage denominator from whatever filled the entry map, which on the ROC path is ROC's own
//      injected record set rather than the .slc. That made the control and treatment arms of the A/B measure
//      different denominators. The source is recorded and the report says which arm it came from.
//
// Independent of SSROCEnabled by design: it measures the SIMULATOR, so it has to run with the cache switched
// off - that arm is the control. Its own setting is SSROCProbeMeasure.

// Which of probeCache's three outcomes a probe took. The distinction is the measurement: a CRC miss means the
// simulator DID probe promptly and the viewer's copy was stale, which is a completely different verdict from
// the simulator never probing at all - and the first cut could not tell them apart.
enum ESSROCProbeOutcome
{
    SSROCP_HIT = 0,     // entry present, CRC matched: the cache answered
    SSROCP_CRC_MISS,    // entry present, CRC differed: probed promptly, viewer's copy stale
    SSROCP_TOTAL_MISS,  // no entry at all: the viewer had never heard of this object
    SSROCP_OUTCOME_COUNT
};

// Where the entry map the coverage denominator is taken from actually came from.
enum ESSROCProbeSource
{
    SSROCP_SRC_SLC = 0,     // the stock .slc read - the control arm, and the only one whose coverage % is a clean simulator measurement
    SSROCP_SRC_ROC          // ROC's injected records - a different and generally larger denominator, so coverage is reported as cache-relative rather than as a simulator number
};

// Region entry. cached_entries is the size of the entry map just loaded, which is the coverage denominator.
void ssROCProbeNoteRegionLoad(LLViewerRegion* regionp, U32 cached_entries, ESSROCProbeSource source);

// One ObjectUpdateCached probe arrived for this region, whatever the viewer's cache made of it.
void ssROCProbeNoteProbe(LLViewerRegion* regionp, U32 local_id, ESSROCProbeOutcome outcome);

// One full object update arrived through cacheFullUpdate - the simulator sending the whole object rather than
// probing for it. Counted as context for the probe numbers, not as part of them.
void ssROCProbeNoteFullUpdate(LLViewerRegion* regionp);

// Writes one region's measurement to the log and releases its samples. Called at region exit; safe to call more than once.
void ssROCProbeReport(LLViewerRegion* regionp);

// For the overlay: the region the agent is in. out_distinct is the coverage numerator, out_events the raw arrival count; they differ and both matter.
bool ssROCProbeCurrent(U32& out_entries, U32& out_distinct, U32& out_events, U32& out_full, F32& out_secs, bool& out_roc_denominator);
// </SS:Nexii>

#endif // SS_ROCPROBE_H
