/**
 * @file ssbc7encodequeue.cpp
 * @brief Squeeze BC7 encode pool - the demand-path hook that turns a freshly decoded full resolution texture into a stored sidecar, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7encodequeue.h"

#include "llappviewer.h"
#include "llevents.h"
#include "lltexturecache.h"
#include "llimage.h"
#include "llimagegl.h"
#include "llsd.h"
#include "llwin32headers.h"
#include "llviewercontrol.h"
#include "llviewertexturelist.h"   // <SS:Nexii> Squeeze eviction - the maintenance tick walks the resident set to mark what is still referenced
#include "ssbc7encoder.h"
#include "ssbc7store.h"
#include "threadpool.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace
{
    // Bounded on purpose. The pool takes work from the texture fetch thread, and WorkQueue::post BLOCKS when the queue is full - a fetch thread parked on a BC7 queue would stall every texture in the world, so this module only ever uses tryPost and treats a refusal as a want-list entry.
    const size_t SSBC7_QUEUE_CAPACITY = 128;

    // Each queued item pins a decoded LLImageRaw alive. A 2048 square RGBA raw is 16 MB, so an unbounded queue during a teleport arrival would hold hundreds of megabytes hostage; this is the real backpressure, the item count is only a coarse first cut.
    const S64 SSBC7_MAX_PINNED_BYTES = 256ll * 1024 * 1024;

    // How often the running verdict tally is written to the log. Frequent enough that a short session still produces one line, rare enough that a busy region does not drown the log.
    const U32 SSBC7_LOG_EVERY = 512;

    const char* s_verdict_names[SSBC7_VERDICT_COUNT] =
    {
        "enqueued", "feature off", "store not ready", "excluded type", "not full resolution",
        "unsupported geometry", "already stored", "backpressure", "post refused"
    };

    // Set once on the main thread and read from the fetch thread. These are plain values rather than gSavedSettings lookups because the fetch thread must never touch the control group, and rather than LLCachedControl because its first construction would then happen on whichever thread got there first.
    struct Policy
    {
        std::atomic<bool> mEnabled{false};
        std::atomic<bool> mBackgroundEncode{true};
        std::atomic<bool> mGpuSupported{false};
        std::atomic<U32>  mMinSize{64};
    };

    class SSBC7EncodePool : public LL::ThreadPool
    {
    public:
        // auto_shutdown is false so that nothing closes this pool behind our back. The stock listener closes the queue, and a WorkQueue DRAINS before it closes - which at quit means executing every queued megapixel encode while the user waits. Shutdown here has to raise the abandon flag first, so it owns the whole sequence.
        SSBC7EncodePool(size_t threads)
        :   LL::ThreadPool("BC7Encode", threads, SSBC7_QUEUE_CAPACITY, false)
        {}

        void run() override
        {
#if LL_WINDOWS
            // Background QoS deprioritises CPU, disk and memory pressure together, which is exactly the shape of this work. Note the existing in-tree call at llappviewerwin32.cpp:1404 passes a handle to a DIFFERENT thread, and THREAD_MODE_BACKGROUND_BEGIN is only ever valid for the calling thread - so that one does nothing and this is the first place the mode is actually applied.
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
            LL::ThreadPool::run();
#if LL_WINDOWS
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
#endif
        }
    };

    struct EncodeState
    {
        Policy                          mPolicy;
        // Published only once start() has returned, and never taken down again while the session lasts. A pool is created at most once because LL::ThreadPool registers itself by name in an instance tracker, so closing one and building another under the same name would collide - which means disabling Squeeze mid-session leaves idle workers behind rather than reclaiming them, and that is the cheaper of the two wrong answers.
        std::atomic<SSBC7EncodePool*>   mPool{nullptr};
        SSBC7Store*                     mStore{nullptr};   // resolved on the main thread at init, because a worker must never be the first toucher of an LLSingleton

        std::atomic<bool>               mShuttingDown{false};
        std::atomic<bool>               mAbandon{false};
        std::atomic<bool>               mClosed{false};
        std::atomic<S32>                mPending{0};
        std::atomic<S64>                mPinnedBytes{0};
        std::atomic<U32>                mConsidered{0};
        std::atomic<U32>                mAbandoned{0};
        std::atomic<U32>                mEncodeFailed{0};
        std::atomic<U32>                mVerdicts[SSBC7_VERDICT_COUNT];

        std::mutex                      mSetMutex;
        std::unordered_set<LLUUID>      mInFlight;
        std::unordered_set<LLUUID>      mWantList;

        EncodeState()
        {
            for (U32 i = 0; i < SSBC7_VERDICT_COUNT; ++i) mVerdicts[i] = 0;
        }
    };

    // Created once on the main thread, then never destroyed. A fetch thread that loads this pointer while the viewer is quitting has to find a closed pool, not freed memory, and the object is a few hundred bytes.
    std::atomic<EncodeState*> s_state{nullptr};

    EncodeState* state() { return s_state.load(std::memory_order_acquire); }

    // The whole want list is capped: one walk through a busy mall must not be able to queue an unbounded set of uuids that nothing will ever get around to.
    const size_t SSBC7_WANT_LIST_CAP = 8192;

    void noteWanted(EncodeState* st, const LLUUID& id)
    {
        std::lock_guard<std::mutex> lock(st->mSetMutex);
        if (st->mWantList.size() < SSBC7_WANT_LIST_CAP) st->mWantList.insert(id);
    }

    ESSBC7EncodeVerdict record(EncodeState* st, ESSBC7EncodeVerdict verdict)
    {
        ++st->mVerdicts[verdict];

        const U32 n = ++st->mConsidered;
        if ((n % SSBC7_LOG_EVERY) == 0)
        {
            LL_INFOS("Squeeze") << ssBC7EncodeMetricsString() << LL_ENDL;
        }
        return verdict;
    }

    // Retires the worker-side accounting. Everything here runs on the WORKER and never in a completion callback on the main thread: at quit the main loop has stopped pumping, so a counter decremented there would never come back down and the pinned-bytes budget would stay permanently exhausted. This project has already shipped that bug once in the region cache.
    struct EncodeGuard
    {
        EncodeGuard(EncodeState* st, const LLUUID& id, S64 bytes) : mState(st), mUUID(id), mBytes(bytes) {}
        ~EncodeGuard()
        {
            --mState->mPending;
            mState->mPinnedBytes -= mBytes;
            std::lock_guard<std::mutex> lock(mState->mSetMutex);
            mState->mInFlight.erase(mUUID);
        }
        EncodeState* mState;
        LLUUID       mUUID;
        S64          mBytes;
    };

    // Alpha shape, decided from the source pixels while the shared lock is still held. Cheap next to the encode itself, and doing it now is what keeps a later pass from having to re-read the whole texture just to learn whether it is opaque.
    void classifyAlpha(const U8* data, U32 width, U32 height, U32 components, U16& out_flags)
    {
        if (components != 2 && components != 4)
        {
            out_flags |= SSBC7_FLAG_FULLY_OPAQUE;
            return;
        }

        const U32 stride = components;
        const size_t texels = (size_t)width * height;
        bool any_transparent = false;
        bool any_midtone = false;

        for (size_t i = 0; i < texels; ++i)
        {
            const U8 a = data[i * stride + (components - 1)];
            if (a != 255)
            {
                any_transparent = true;
                // The renderer treats a binary cutout differently from smooth alpha, and the boundary it cares about is whether anything lands between the two extremes.
                if (a != 0) { any_midtone = true; break; }
            }
        }

        if (!any_transparent)   out_flags |= SSBC7_FLAG_FULLY_OPAQUE;
        else if (!any_midtone)  out_flags |= SSBC7_FLAG_ALPHA_IS_MASK;
    }

    bool isPowerOfTwo(U32 v) { return v != 0 && (v & (v - 1)) == 0; }
}

const char* ssBC7VerdictName(ESSBC7EncodeVerdict verdict)
{
    if ((U32)verdict >= (U32)SSBC7_VERDICT_COUNT) return "unknown";
    return s_verdict_names[verdict];
}

void ssBC7EncodeRefreshPolicy()
{
    EncodeState* st = state();
    if (!st) return;

    st->mPolicy.mEnabled          = gSavedSettings.getBOOL("SSSqueezeEnabled");
    st->mPolicy.mBackgroundEncode = gSavedSettings.getBOOL("SSSqueezeBackgroundEncode");
    st->mPolicy.mMinSize          = gSavedSettings.getU32("SSSqueezeMinTextureSize");

    // Snapshotted here rather than read on the fetch thread: canUseSqueeze() reads gGLManager, and there is no reason to have a worker thread reaching into GL state at all. A card with no BPTC would produce records nothing could ever upload.
    st->mPolicy.mGpuSupported     = LLImageGL::canUseSqueeze();

    LL_INFOS("Squeeze") << "BC7 encode policy: enabled " << (st->mPolicy.mEnabled.load() ? 1 : 0)
                        << ", background encode " << (st->mPolicy.mBackgroundEncode.load() ? 1 : 0)
                        << ", gpu " << (st->mPolicy.mGpuSupported.load() ? "supports BC7" : "has no BC7, nothing will be encoded")
                        << ", minimum size " << st->mPolicy.mMinSize.load() << LL_ENDL;

    // Everything below brings the tier up on demand. Squeeze ships off, so the very first time anyone tries it is by ticking the box at the login screen or in preferences - if that needed a restart to take effect, the honest description of the feature would be that it does not work. This is the same lesson the region cache already learned.
    if (!st->mPolicy.mEnabled || !st->mPolicy.mBackgroundEncode || !st->mPolicy.mGpuSupported) return;
    if (st->mShuttingDown) return;

    if (st->mStore && !st->mStore->isInitialized() && LLAppViewer::getTextureCache())
    {
        st->mStore->initStore(LLAppViewer::getTextureCache()->getTexturesDirName(), ssBC7EncoderVersion());
    }

    if (!st->mPool.load())
    {
        size_t threads = (size_t)gSavedSettings.getU32("SSSqueezeEncodeThreads");
        if (threads == 0)
        {
            // Deliberately narrow until throughput is measured on real hardware, which is the P0 exit criterion this width depends on. Background QoS is what keeps the pool out of the way, not a small width - but a wide pool of idle workers also burns wakeups under the sleepy_robin scheduler.
            const unsigned hw = std::thread::hardware_concurrency();
            threads = (size_t)llclamp((S32)(hw / 4), 1, 4);
        }

        SSBC7EncodePool* pool = new SSBC7EncodePool(threads);
        pool->start();
        st->mPool.store(pool);   // published only after start(), so a fetch thread that ever sees a non-null pool sees one with workers already servicing it

        LL_INFOS("Squeeze") << "BC7 encode pool started with " << pool->getWidth()
                            << " workers, queue capacity " << SSBC7_QUEUE_CAPACITY << LL_ENDL;
    }
}

void ssBC7EncodeInit()
{
    if (state()) return;

    EncodeState* st = new EncodeState();
    st->mStore = SSBC7Store::getInstance();
    s_state.store(st, std::memory_order_release);

    // Our own shutdown listener, so the pool is always joined even if the explicit hook in LLAppViewer::cleanup is ever moved or lost. It has to be ours rather than the ThreadPool's built-in one because the abandon flag must be raised BEFORE the queue is closed, and the built-in listener only knows how to close.
    LLEventPumps::instance().obtain("LLApp").listen(
        "SSBC7Encode",
        [](const LLSD& stat)
        {
            if (std::string(stat["status"]) != "running")
            {
                ssBC7EncodeBeginShutdown();
                ssBC7EncodeShutdown();
            }
            return false;
        });

    // No threads are created here. Squeeze ships off, and a pool of workers polling for work nobody will ever post is a cost every user who never touches the feature would otherwise pay.
    ssBC7EncodeRefreshPolicy();
}

void ssBC7EncodeBeginShutdown()
{
    EncodeState* st = state();
    if (!st) return;

    st->mShuttingDown = true;
    st->mAbandon = true;

    // The store is shut at the same instant rather than later on, so a worker already past its abandon check finds the door closed instead of appending into a store that is about to be torn down. Keeping the two in one call is what stops them from drifting apart the next time the shutdown sequence is reordered.
    if (st->mStore) st->mStore->beginShutdown();
}

void ssBC7EncodeShutdown()
{
    EncodeState* st = state();
    if (!st) return;

    st->mShuttingDown = true;
    st->mAbandon = true;

    if (st->mClosed.exchange(true)) return;

    if (SSBC7EncodePool* pool = st->mPool.load())
    {
        // close() drains the queue and then joins. Every remaining item sees the abandon flag and returns immediately, so this costs one in-progress encode rather than the whole backlog.
        pool->close();
    }

    LL_INFOS("Squeeze") << "BC7 encode pool stopped. " << ssBC7EncodeMetricsString() << LL_ENDL;
}

ESSBC7EncodeVerdict ssBC7EncodeConsider(const LLUUID& id,
                                        FTType ftype,
                                        const LLPointer<LLImageRaw>& raw,
                                        S32 decoded_discard,
                                        bool have_all_data,
                                        bool needs_aux,
                                        bool in_local_cache)
{
    EncodeState* st = state();
    if (!st) return SSBC7_DECLINE_NOT_READY;

    if (!st->mPolicy.mEnabled || !st->mPolicy.mBackgroundEncode || !st->mPolicy.mGpuSupported)
    {
        return record(st, SSBC7_DECLINE_OFF);
    }

    SSBC7EncodePool* pool = st->mPool.load();
    if (!pool || st->mShuttingDown || !st->mStore || !st->mStore->isInitialized() || st->mStore->isReadOnly())
    {
        return record(st, SSBC7_DECLINE_NOT_READY);
    }

    // Excluded kinds. Bakes churn a fresh uuid on every outfit change, map tiles and local files are not world content, and anything wanting the aux channel is a bump or sculpt source whose consumer needs the raw rather than a compressed upload. See doc/super_compressed_textures.md for the full list and the reasoning behind each.
    if (ftype != FTT_DEFAULT || needs_aux || in_local_cache)
    {
        return record(st, SSBC7_DECLINE_TYPE);
    }

    // FULL RESOLUTION ONLY. Encoding an intermediate discard spends the same CPU for a record that the next fetch supersedes, so anything short of the complete asset at discard zero becomes a want-list entry for the promotion engine instead.
    if (decoded_discard != 0 || !have_all_data || raw.isNull())
    {
        // A texture that failed to decode at all, or was blocked before it ever decoded, arrives here with no raw and a negative discard. It is not a partial asset waiting to be completed, so it stays off the want list - the promotion engine would only spend network on something that has already refused to exist.
        if (decoded_discard >= 0 && raw.notNull() && !id.isNull()) noteWanted(st, id);
        return record(st, SSBC7_DECLINE_PARTIAL);
    }

    const U32 width      = raw->getWidth();
    const U32 height     = raw->getHeight();
    const U32 components = (U32)raw->getComponents();
    const U32 min_size   = st->mPolicy.mMinSize;

    // The size gate is on AREA, not on either edge, because area is what the saving is proportional to: a 1024x256 banner holds exactly as many texels as a 512x512, saves exactly as much video memory, and an edge test would throw it away while keeping the square. The setting is still expressed as a square edge length, since "512" and "1024" are how texture sizes are actually spoken about, so the threshold it means is that square's texel count.
    //
    // The separate four texel floor is not about worth, it is about shape: BC7 works in 4x4 blocks, so an edge below four is padded out and the texels paid for are mostly padding. Power of two sources admit 1 and 2, so the case is reachable even though SL content never really produces it.
    const U64 texels     = (U64)width * (U64)height;
    const U64 min_texels = (U64)min_size * (U64)min_size;

    // Non power of two sources cannot be uploaded through the mip path at all.
    if (!isPowerOfTwo(width) || !isPowerOfTwo(height)
        || texels < min_texels
        || width < 4 || height < 4
        || components < 1 || components > 4)
    {
        return record(st, SSBC7_DECLINE_GEOMETRY);
    }

    if (st->mStore->hasRecord(id))
    {
        return record(st, SSBC7_DECLINE_ALREADY);
    }

    const S64 bytes = (S64)raw->getDataSize();
    if (bytes <= 0)
    {
        return record(st, SSBC7_DECLINE_GEOMETRY);
    }

    if (st->mPinnedBytes.load() + bytes > SSBC7_MAX_PINNED_BYTES)
    {
        noteWanted(st, id);
        return record(st, SSBC7_DECLINE_BACKPRESSURE);
    }

    // The in-flight set is what makes plain-uuid dedupe hold across threads: the store only learns about a record once the append completes, so without this two workers can encode the same texture at the same time and the second one's work is thrown away. The verdict is recorded outside the lock, because record() reaches for the same mutex to size the want list.
    bool claimed = false;
    {
        std::lock_guard<std::mutex> lock(st->mSetMutex);
        claimed = st->mInFlight.insert(id).second;
        if (claimed) st->mWantList.erase(id);
    }
    if (!claimed)
    {
        return record(st, SSBC7_DECLINE_ALREADY);
    }

    ++st->mPending;
    st->mPinnedBytes += bytes;

    LLPointer<LLImageRaw> pinned = raw;

    const bool posted = pool->getQueue().tryPost(
        [st, id, pinned, bytes]()
        {
            // Retires the pending count, the pinned bytes and the in-flight entry on THIS thread, whatever happens below - including the abandon path, where nothing else runs at all.
            EncodeGuard guard(st, id, bytes);

            if (st->mAbandon.load()) { ++st->mAbandoned; return; }

            // One scratch per worker thread, reused for every texture that thread ever encodes, so a busy pool does no allocation per image and none whatsoever per block.
            static thread_local SSBC7EncodeScratch scratch;

            std::vector<U8> payload;
            SSBC7EncodeResult result;
            U16 flags = 0;
            U32 width = 0;
            U32 height = 0;
            U32 components = 0;

            {
                // The raw is treated as strictly immutable here. The shared lock is what makes that a promise rather than a hope, since the decode pool and the cache writer both reach for the same object.
                LLImageDataSharedLock lock(pinned.get());

                const U8* src = pinned->getData();
                width      = pinned->getWidth();
                height     = pinned->getHeight();
                components = (U32)pinned->getComponents();

                if (!src || pinned->isBufferInvalid())
                {
                    ++st->mEncodeFailed;
                    LL_WARNS("Squeeze") << "BC7 encode of " << id << " skipped: the decoded image was released before the worker reached it" << LL_ENDL;
                    return;
                }

                classifyAlpha(src, width, height, components, flags);

                if (!ssBC7EncodeMipChain(src, width, height, components, scratch, payload, result))
                {
                    ++st->mEncodeFailed;
                    LL_WARNS("Squeeze") << "BC7 encode of " << id << " failed at " << width << "x" << height
                                        << " with " << components << " components, the texture keeps using the ordinary J2C path" << LL_ENDL;
                    return;
                }
            }

            if (st->mAbandon.load()) { ++st->mAbandoned; return; }

            SSBC7Encoded enc;
            enc.mUUID          = id;
            enc.mWidth         = (U16)result.mWidth;
            enc.mHeight        = (U16)result.mHeight;
            enc.mMipCount      = result.mMipCount;
            enc.mSrcComponents = result.mSrcComponents;   // the ORIGINAL component count, which is what later keeps an opaque texture out of the alpha pool
            enc.mFlags         = flags;
            enc.mPayload       = std::move(payload);

            // The append is done here rather than posted back to the main thread for the same reason the accounting is: a completion callback does not run once the main loop stops, and a store write is disk work that has no business on the frame thread anyway.
            if (!st->mStore->append(enc, ssBC7EncoderVersion()))
            {
                LL_DEBUGS("Squeeze") << "BC7 store declined " << id << ", see the store metrics for whether that was a duplicate or a write failure" << LL_ENDL;
            }
        });

    if (!posted)
    {
        // tryPost returns false when the queue is full OR closed, and neither one runs the work item. Without unwinding here the pending count and the pinned-bytes budget leak permanently and the pool refuses everything for the rest of the session.
        --st->mPending;
        st->mPinnedBytes -= bytes;
        {
            std::lock_guard<std::mutex> lock(st->mSetMutex);
            st->mInFlight.erase(id);
        }
        noteWanted(st, id);
        return record(st, SSBC7_DECLINE_POST_FAILED);
    }

    return record(st, SSBC7_ENQUEUED);
}

// <SS:Nexii> Squeeze eviction - the whole main-thread cost of eviction, once a minute: one walk of the resident texture set and at most one work item posted.
//
// It lives here rather than beside the eviction policy for one reason - this is the module that owns the pool, and the trigger inside appendBlob cannot post its own work because it runs under the store lock that the work will itself want. That trigger sets a flag; this reads it, outside every lock, on a thread that is allowed to block for a microsecond.
void ssBC7EncodeMaintenanceTick()
{
    EncodeState* st = state();
    if (!st || st->mShuttingDown || !st->mPolicy.mEnabled) return;

    SSBC7Store* store = st->mStore;
    if (!store || !store->isInitialized() || store->isReadOnly()) return;

    static U32 s_last_second = 0;
    const U32 now = ssBC7NowSeconds();
    if (s_last_second && now >= s_last_second && (now - s_last_second) < SSBC7_EVICT_TICK_SECONDS) return;
    s_last_second = now;

    // THE OTHER HALF OF THE HEAT SIGNAL. hasRecord() only fires when a texture is decoded, so anything that decodes once at login and then stays resident forever - avatar skins, the UI atlas, system assets - would be stamped once and then rank as the coldest thing in the store, which is exactly the set a user would be angriest to lose. Reused across ticks so the walk costs no allocation.
    static std::vector<LLUUID> s_referenced;
    s_referenced.clear();
    for (const LLPointer<LLViewerFetchedTexture>& imagep : gTextureList)
    {
        if (imagep.isNull()) continue;
        const LLUUID& id = imagep->getID();
        if (id.notNull()) s_referenced.push_back(id);
    }

    std::vector<LLUUID> rewant;
    store->touchReferenced(s_referenced, rewant);

    // A texture that is on screen RIGHT NOW and whose record a kill dropped is the only part of a kill worth undoing, and the want list is the existing machinery for saying so. Every other dropped uuid is left alone on purpose: it re-encodes for free the next time it is decoded, on a decode the viewer was going to do anyway.
    for (const LLUUID& id : rewant) noteWanted(st, id);

    if (!store->evictionWanted() && store->allocatedBytes() <= store->budgetBytes()) return;
    if (store->evictionRunning()) return;

    SSBC7EncodePool* pool = st->mPool.load();
    if (!pool) return;

    // tryPost, never post: a refused item is not an error, it is a busy encode queue, and the next tick asks again. No second thread pool - LL::ThreadPool registers by name in an instance tracker, so a second one is more shutdown ordering to get wrong for work that runs once a minute.
    const bool posted = pool->getQueue().tryPost(
        [st, store]()
        {
            if (st->mAbandon.load()) return;
            store->evictPass(true);
        });

    if (!posted)
    {
        LL_DEBUGS("Squeeze") << "BC7 eviction pass not posted: the encode queue is full, retrying at the next tick" << LL_ENDL;
    }
}
// </SS:Nexii>

void ssBC7EncodeWantList(std::vector<LLUUID>& out)
{
    out.clear();
    EncodeState* st = state();
    if (!st) return;

    std::lock_guard<std::mutex> lock(st->mSetMutex);
    out.reserve(st->mWantList.size());
    for (const LLUUID& id : st->mWantList) out.push_back(id);
}

size_t ssBC7EncodeWantListSize()
{
    EncodeState* st = state();
    if (!st) return 0;

    std::lock_guard<std::mutex> lock(st->mSetMutex);
    return st->mWantList.size();
}

std::string ssBC7EncodeMetricsString()
{
    EncodeState* st = state();
    if (!st) return "BC7 encode pool not started";

    std::string counts;
    for (U32 i = 0; i < SSBC7_VERDICT_COUNT; ++i)
    {
        const U32 n = st->mVerdicts[i].load();
        if (!n) continue;
        if (!counts.empty()) counts += ", ";
        counts += llformat("%s %u", ssBC7VerdictName((ESSBC7EncodeVerdict)i), n);
    }
    if (counts.empty()) counts = "nothing considered yet";

    return llformat("BC7 encode: %s | in flight %d, pinned %lld MB, abandoned %u, encode failures %u, want list %u",
                    counts.c_str(),
                    (S32)st->mPending.load(),
                    (long long)(st->mPinnedBytes.load() / (1024 * 1024)),
                    st->mAbandoned.load(),
                    st->mEncodeFailed.load(),
                    (U32)ssBC7EncodeWantListSize());
}
