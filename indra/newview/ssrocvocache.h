/**
 * @file ssrocvocache.h
 * @brief Region Object Cache as the backing store for the stock protocol object cache - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_ROCVOCACHE_H
#define SS_ROCVOCACHE_H

#include "llvocache.h"
#include "ssroccache.h"

class LLViewerRegion;

// The ROC becomes the BACKING STORE beneath LLVOCache rather than a replacement for it. LLVOCacheEntry, the entry map and every line of the object-update protocol above them are untouched; only the two ends change, so that a region's entries are built from ROC records instead of being read from a second copy of the same objects in an .slc file.
//
// The reason the object cache can be absorbed at all is that the ROC already stores strictly more than the .slc does: the byte-identical data-packer blob, the CRC, the local id, the FullID that the .slc has no field for, and the update flags that its disk format does not carry either - plus tenure, dwell, confirmation counts and the local-id epoch bits. Two caches of the same objects, one of which is a superset of the other, is a duplication with no purpose.
//
// WHAT THE SIMULATOR SEES DOES NOT CHANGE, and that is the requirement everything here is shaped around. LLViewerRegion::probeCache and LLViewerRegion::cacheFullUpdate are not modified and are not called from here. Their entire input, besides the message itself, is the region's entry map - so the simulator-facing behaviour is a pure function of what that map contains, and the job of this file is to put the same entries in it that the .slc would have. A seeded entry is created INVALID, exactly as one read from the .slc is, so it is inert until the simulator's own probe validates it: seeding fills the protocol cache, it does not draw anything, and it is not ghost injection.
//
// Two directions of error, and they are not symmetrical. An entry we fail to supply becomes a cache miss the viewer requests - a wasted round trip, never a wrong picture. An entry supplied at a local id that no longer names the same object would answer a probe with a false CRC match, and the viewer would then never request the object that is actually there. Every refusal below leans towards the first.
//
// When any of this declines, the STOCK .slc path runs for that region exactly as it does today. That is what makes the integration stageable and what makes SSROCBackObjectCache a real toggle rather than a promise.

// Why one record was not turned into a cache entry, as opposed to why it was never offered - the plan-level refusals live in ESSROCSeedRefusal. Split because these two happen at different layers: the plan is a pure decision over records and is tested offline, while these need the live entry map and the blob in front of them.
enum ESSROCSinkRefusal : U8
{
    SSROC_SINK_OK              = 0,
    SSROC_SINK_ID_OCCUPIED     = 1,  // the live stream already put something at this local id this visit, and the live stream always wins
    SSROC_SINK_BLOB_SHORT      = 2,  // the blob is too short to carry the fixed prefix, so nothing about it can be checked
    SSROC_SINK_BLOB_MISMATCH   = 3,  // the blob's own local id or CRC disagrees with the record's - an .slc entry can never disagree with its own body, so neither may this one
    SSROC_SINK_COUNT           = 4,
};

const char* ssROCSinkRefusalName(U8 refusal);

// True when the ROC is to be used in place of the .slc. Off by default: the fallback is the setting, and turning it off restores today's behaviour for the next region entry with no other change and no data loss.
bool ssROCBacksObjectCache();

// Fill a region's entry map from the ROC. Returns true only when the map was actually filled, in which case the caller must NOT read the .slc; on false the stock path runs untouched and the reason has already been logged by name.
//
// Called from LLViewerRegion::loadObjectCache, which runs inside unpackRegionHandshake ABOVE the RegionHandshakeReply. That placement is load-bearing: bit 1 of that reply tells the simulator whether the viewer's cache is empty, and therefore whether to send cache probes at all. Answering it from a cache that has not finished arriving would change what the simulator sends, so the read is completed here rather than deferred.
bool ssROCLoadObjectCache(LLViewerRegion* regionp, LLVOCacheEntry::vocache_entry_map_t& cache_map);

// True when this region's map came from the ROC this visit, so LLViewerRegion::saveObjectCache knows not to write the objects out a second time.
bool ssROCObjectCacheIsBacked(U64 handle);

// Called from LLViewerRegion::saveObjectCache once the region is finished with. Reports the visit's probe hit rate against how the map was filled - the number that says whether backing the cache made the simulator answer more requests or fewer - and releases the per-region state.
void ssROCNoteObjectCacheSaved(LLViewerRegion* regionp);

std::string ssROCVOCacheMetricsString();

#endif // SS_ROCVOCACHE_H
