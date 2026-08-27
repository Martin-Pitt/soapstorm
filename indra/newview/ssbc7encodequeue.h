/**
 * @file ssbc7encodequeue.h
 * @brief Squeeze BC7 encode pool - the demand-path hook that turns a freshly decoded full resolution texture into a stored sidecar, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_BC7ENCODEQUEUE_H
#define SS_BC7ENCODEQUEUE_H

#include "llpointer.h"
#include "lluuid.h"
#include "llviewertexture.h"    // FTType

#include <string>
#include <vector>

class LLImageRaw;

// Everything that decides whether a decoded texture becomes a BC7 record, and the pool that does the work.
//
// This is deliberately a set of free functions over one file-static state object rather than an LLSingleton: the consider entry point is called from the texture fetch thread, and LLSingleton's first-touch construction is a main-thread affair. The state is created once on the main thread and then never destroyed, so a fetch thread reading it during shutdown finds a closed pool rather than freed memory.

// Why a given texture was or was not enqueued. Every one of these is counted and periodically logged, because a bare rejection makes "the feature is off" and "the feature ran and declined everything" look identical from a log file - which is precisely the failure this project has already paid for once.
enum ESSBC7EncodeVerdict
{
    SSBC7_ENQUEUED = 0,
    SSBC7_DECLINE_OFF,              // SSSqueezeEnabled or SSSqueezeBackgroundEncode is off, or the GPU has no BPTC so there would be nothing to serve the record to
    SSBC7_DECLINE_NOT_READY,        // the store never came up, is a read-only second instance, or shutdown has begun
    SSBC7_DECLINE_TYPE,             // an excluded texture kind - see doc/super_compressed_textures.md for why each one is excluded
    SSBC7_DECLINE_PARTIAL,          // not full resolution, which is the whole point of full-res-only; the uuid goes on the want list for the promotion engine instead
    SSBC7_DECLINE_GEOMETRY,         // non power of two, below the minimum size, or a component count BC7 cannot represent
    SSBC7_DECLINE_ALREADY,          // already stored, or already being encoded by another worker
    SSBC7_DECLINE_BACKPRESSURE,     // the queue or the pinned-bytes budget is full; lossless, since the J2C is on disk and the promotion engine will come back for it
    SSBC7_DECLINE_POST_FAILED,      // the pool refused the work item
    SSBC7_VERDICT_COUNT
};

const char* ssBC7VerdictName(ESSBC7EncodeVerdict verdict);

// Main thread. Brings up the pool and caches the policy that the fetch thread reads; safe to call when the feature is off, in which case it does nothing at all.
void ssBC7EncodeInit();

// Main thread. Re-reads the SSSqueeze settings into the cached policy so the gate can be flipped without a restart. The fetch thread only ever reads these cached values, never gSavedSettings.
void ssBC7EncodeRefreshPolicy();

// Main thread. Stops accepting work and tells any worker already inside an encode to give up at its next check. Encodes are ABANDONED rather than drained at quit: the work queue drains itself before it closes, so without an abandon flag a full queue of megapixel encodes would be executed in full while the user waits for the window to disappear.
void ssBC7EncodeBeginShutdown();

// Main thread. Closes and joins the pool. Must run BEFORE SSBC7Store::shutdownStore(), because every in-flight append belongs to a worker that this call is what waits for. Idempotent.
void ssBC7EncodeShutdown();

// Any thread, in practice the texture fetch thread from the DONE state. Applies every gate and either posts the encode or records why it did not.
//
// MUST NOT BLOCK. The caller holds LLTextureFetchWorker::mWorkMutex for the whole of doWork, so anything that parked here would park every texture in the world behind it: the post is a tryPost, the store lookup takes only the store's map lock, and no file IO happens on this thread at all. A verdict other than SSBC7_ENQUEUED always means the texture carries on down the ordinary uncompressed path exactly as it does today.
ESSBC7EncodeVerdict ssBC7EncodeConsider(const LLUUID& id,
                                        FTType ftype,
                                        const LLPointer<LLImageRaw>& raw,
                                        S32 decoded_discard,
                                        bool have_all_data,
                                        bool needs_aux,
                                        bool in_local_cache);

// <SS:Nexii> Squeeze eviction - main thread, every frame, and almost always a clock comparison and nothing else. It owns the two jobs that have to happen on the frame thread and nowhere else: marking every texture the running viewer still references, which is the only way a permanently resident texture ever looks warm, and posting the eviction pass onto THIS pool rather than a second one - a WorkQueue post made from inside the store's own lock is how a deadlock gets written, so appendBlob only ever raises a flag and this is what reads it.
void ssBC7EncodeMaintenanceTick();
// </SS:Nexii>

// Snapshot of the want list, which is where every uuid that was declined for a reason a later pass could fix ends up. Phase 4's promotion engine drains this; for now it exists so the population is measurable rather than guessed at.
void ssBC7EncodeWantList(std::vector<LLUUID>& out);
size_t ssBC7EncodeWantListSize();

std::string ssBC7EncodeMetricsString();

#endif // SS_BC7ENCODEQUEUE_H
