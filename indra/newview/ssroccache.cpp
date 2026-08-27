/**
 * @file ssroccache.cpp
 * @brief Region Object Cache (ROC) container and store - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssroccache.h"

#include "llapp.h"
#include "llappviewer.h"
#include "llcrc.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "workqueue.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>

namespace
{
    const char* SSROC_DIR_NAME = "roc";
    const U32   SSROC_SECS_PER_DAY = 86400;
    const U32   SSROC_DRAIN_TIMEOUT_MS = 5000;

    // A tiny append-only writer and a bounds-checked reader. Everything the ROC stores is fixed-width or length-prefixed, so a corrupt file can never make the reader walk off the end - it just fails and the file is deleted, which is safe because every byte in it is derived data.
    class Writer
    {
    public:
        explicit Writer(std::vector<U8>& out) : mOut(out) {}

        void bytes(const void* src, size_t n) { const U8* p = (const U8*)src; mOut.insert(mOut.end(), p, p + n); }
        template <typename T> void pod(const T& v) { bytes(&v, sizeof(T)); }
        void uuid(const LLUUID& id) { bytes(id.mData, UUID_BYTES); }
        void vec3(const LLVector3& v) { pod(v.mV[0]); pod(v.mV[1]); pod(v.mV[2]); }
        void blob(const std::vector<U8>& b) { U32 n = (U32)b.size(); pod(n); if (n) bytes(b.data(), n); }
        size_t size() const { return mOut.size(); }

    private:
        std::vector<U8>& mOut;
    };

    class Reader
    {
    public:
        Reader(const U8* data, size_t size) : mData(data), mSize(size), mPos(0), mOk(true) {}

        bool ok() const { return mOk; }
        size_t tell() const { return mPos; }
        void seek(size_t pos) { if (pos > mSize) { mOk = false; return; } mPos = pos; }

        bool bytes(void* dst, size_t n)
        {
            if (!mOk || mPos + n > mSize) { mOk = false; return false; }
            memcpy(dst, mData + mPos, n);
            mPos += n;
            return true;
        }
        template <typename T> T pod() { T v = T(); bytes(&v, sizeof(T)); return v; }
        LLUUID uuid() { LLUUID id; bytes(id.mData, UUID_BYTES); return id; }
        LLVector3 vec3() { F32 x = pod<F32>(), y = pod<F32>(), z = pod<F32>(); return LLVector3(x, y, z); }

        bool blob(std::vector<U8>& out, U32 max_size)
        {
            U32 n = pod<U32>();
            if (!mOk || n > max_size || mPos + n > mSize) { mOk = false; return false; }
            out.resize(n);
            if (n) bytes(out.data(), n);
            return mOk;
        }

    private:
        const U8*   mData;
        size_t      mSize;
        size_t      mPos;
        bool        mOk;
    };

    U32 crc32Of(const U8* data, size_t size)
    {
        LLCRC crc;
        if (size) crc.update(data, size);
        return crc.getCRC();
    }

    // Shift the day bitmap towards older bits. Bit i means "seen i days before mLastConfirmed", so advancing the clock by d days moves every recorded day d places up and drops anything older than the window.
    void shiftBitmapUp(U8* bits, U32 nbytes, U32 shift)
    {
        if (shift == 0) return;
        if (shift >= nbytes * 8) { memset(bits, 0, nbytes); return; }

        const U32 byte_shift = shift / 8;
        const U32 bit_shift  = shift % 8;

        for (S32 i = (S32)nbytes - 1; i >= 0; --i)
        {
            U32 src = 0;
            const S32 lo_idx = i - (S32)byte_shift;
            const S32 hi_idx = lo_idx - 1;
            if (lo_idx >= 0) src |= (U32)bits[lo_idx] << bit_shift;
            if (bit_shift && hi_idx >= 0) src |= (U32)bits[hi_idx] >> (8 - bit_shift);
            bits[i] = (U8)(src & 0xFF);
        }
    }

    U32 popcount8(U8 v)
    {
        U32 n = 0;
        while (v) { n += (v & 1); v >>= 1; }
        return n;
    }

    // In-flight accounting is retired on the WORKER, never in the main-thread completion callback: at shutdown the main loop has stopped pumping its queue, so a counter decremented there would never reach zero and every quit would eat the full drain timeout.
    struct OpGuard
    {
        explicit OpGuard(SSROCStore* store) : mStore(store) {}
        ~OpGuard() { if (mStore) mStore->endOp(); }
        SSROCStore* mStore;
    };
}

// ---------------------------------------------------------------------------
// SSROCRecord
// ---------------------------------------------------------------------------

SSROCRecord::SSROCRecord()
:   mLastLocalID(0),
    mCRC(0),
    mUpdateFlags(0),
    mRecordFlags(0),
    mScore(0.f),
    mFirstSeen(0),
    mLastConfirmed(0),
    mRezTimeEpoch(0),
    mSessionsSeen(0),
    mCRCChangeCount(0),
    mChildCount(0),
    mEntryCount(0),
    mDwellSecs(0),
    mConfirmCount(0),
    mMoveCount(0),
    mMissStreak(0),
    mBlockedBy(0),
    mPos(),
    mScale()
{
    memset(mDayBitmap, 0, sizeof(mDayBitmap));
}

void SSROCRecord::markSeenOn(U64 unix_secs)
{
    if (unix_secs == 0) return;

    const U64 day = unix_secs / SSROC_SECS_PER_DAY;

    if (mFirstSeen == 0)
    {
        mFirstSeen = unix_secs;
    }

    if (mLastConfirmed == 0)
    {
        mDayBitmap[0] |= 1;
        mLastConfirmed = unix_secs;
        return;
    }

    const U64 last_day = mLastConfirmed / SSROC_SECS_PER_DAY;

    // A clock that went backwards must never rewrite history - the record simply keeps its existing ledger and the sighting is recorded on the day already at bit 0.
    if (day > last_day)
    {
        shiftBitmapUp(mDayBitmap, SSROC_DAY_BITMAP_BYTES, (U32)llmin<U64>(day - last_day, SSROC_DAY_BITMAP_BYTES * 8));
    }

    mDayBitmap[0] |= 1;

    if (unix_secs > mLastConfirmed)
    {
        mLastConfirmed = unix_secs;
    }
}

U32 SSROCRecord::distinctDaysSeen() const
{
    U32 n = 0;
    for (U32 i = 0; i < SSROC_DAY_BITMAP_BYTES; ++i) n += popcount8(mDayBitmap[i]);
    return n;
}

U32 SSROCRecord::tenureDays() const
{
    // Calendar days, not elapsed seconds. Seconds arithmetic makes tenure depend on the time of day the user happened to log in - an object first seen at 23:00 and re-confirmed at 01:00 two days later would score zero - and it would disagree with the day bitmap, which is already calendar-based.
    if (mFirstSeen == 0 || mLastConfirmed <= mFirstSeen) return 0;
    return (U32)((mLastConfirmed / SSROC_SECS_PER_DAY) - (mFirstSeen / SSROC_SECS_PER_DAY));
}

void SSROCRecord::noteEntry()
{
    if (mEntryCount < 65535) ++mEntryCount;
}

void SSROCRecord::addDwell(U32 secs)
{
    // Saturate rather than wrap: a record that has accumulated 136 years of dwell is already past every threshold that matters, and wrapping would silently reset its standing.
    const U32 room = 0xFFFFFFFFu - mDwellSecs;
    mDwellSecs += llmin(secs, room);
}

U32 SSROCRecord::visitCount(U32 visit_secs) const
{
    // Saturate rather than wrap: addDwell deliberately pins mDwellSecs at its maximum, and a wrap here would turn a maximally-tenured record into a brand new one.
    if (visit_secs == 0) return mEntryCount;
    const U32 from_dwell = mDwellSecs / visit_secs;
    if (from_dwell > 0xFFFFFFFFu - (U32)mEntryCount) return 0xFFFFFFFFu;
    return (U32)mEntryCount + from_dwell;
}

bool ssROCParcelNameDeniesImmunity(const std::string& parcel_name)
{
    // Plain case-insensitive substring rather than a word match, so "LR Sandbox 3", "Sandbox Island" and "sandbox-2" are all caught. Matching too eagerly only sends an object down the slower path, so the bias is deliberately towards catching more.
    static const char* TOKEN = "sandbox";
    const size_t token_len = strlen(TOKEN);
    if (parcel_name.size() < token_len) return false;

    for (size_t i = 0; i + token_len <= parcel_name.size(); ++i)
    {
        size_t j = 0;
        while (j < token_len && (char)tolower((unsigned char)parcel_name[i + j]) == TOKEN[j]) ++j;
        if (j == token_len) return true;
    }
    return false;
}

bool ssROCOwnerIsPublicWorks(const std::string& legacy_name)
{
    // Match the final whitespace-separated token exactly, so "Mole" is a surname and not a substring of someone's chosen first name.
    const size_t end = legacy_name.find_last_not_of(" \t");
    if (end == std::string::npos) return false;

    const size_t start = legacy_name.find_last_of(" \t", end);

    // A first name is required. LLAvatarName::getLegacyName() falls back to the DISPLAY name when legacy names are unavailable, so a single bare token could be any resident's chosen display name rather than an account surname.
    if (start == std::string::npos) return false;
    if (legacy_name.find_first_not_of(" \t") >= start) return false;

    const std::string last_name = legacy_name.substr(start + 1, end - start);

    if (last_name.size() != 4) return false;
    return (tolower((unsigned char)last_name[0]) == 'm'
         && tolower((unsigned char)last_name[1]) == 'o'
         && tolower((unsigned char)last_name[2]) == 'l'
         && tolower((unsigned char)last_name[3]) == 'e');
}

F32 ssROCInjectPriority(const SSROCRecord& rec, U64 recent_cutoff)
{
    // PERMANENCE, 0..1. A promoted record has cleared the immunity, persistence and score gates, so it outranks one that qualifies only because it was seen recently - but not by so much that a large recent object loses to a tiny promoted one.
    F32 permanence = llclamp(rec.mScore, 0.f, 1.f);
    if (rec.isPromoted())
    {
        permanence = 0.6f + 0.4f * permanence;
    }
    else if (recent_cutoff != 0 && rec.mLastConfirmed >= recent_cutoff)
    {
        permanence = 0.3f + 0.3f * permanence;
    }
    else
    {
        permanence *= 0.3f;
    }

    // SIZE, 0..1. Logarithmic, because the span from a potted plant to a tower is three orders of magnitude and a linear term would make everything below the largest handful indistinguishable.
    const F32 radius = 0.5f * rec.mScale.length();
    F32 size_term = logf(1.f + llmax(0.f, radius));

    // A large linkset is a building even when its root prim is small, and the root's scale is the only geometry a cached blob carries.
    size_term += 0.5f * logf(1.f + (F32)rec.mChildCount);

    // Normalised against a reference that a genuinely large build reaches: a 32m radius root, or a root anchoring a few hundred children.
    const F32 SSROC_SIZE_REFERENCE = 3.5f;   // logf(1+32) is about 3.5
    const F32 size01 = llclamp(size_term / SSROC_SIZE_REFERENCE, 0.f, 1.f);

    return 0.6f * permanence + 0.4f * size01;
}

SSROCInjectPolicy::SSROCInjectPolicy()
:   mPromotedTier(true),
    mRecentTier(true),
    mRecentCutoff(0),
    mMaxGhosts(0)
{
}

void ssROCBuildInjectPlan(const SSROCRegionFile& file, const SSROCInjectPolicy& policy, std::vector<U32>& out)
{
    out.clear();

    const size_t n = file.mRecords.size();
    if (!n) return;
    if (!policy.mPromotedTier && !(policy.mRecentTier && policy.mRecentCutoff)) return;

    // The cutoff handed to the priority function is the one the RECENT tier is actually being judged on. When that tier is off it must be zero, or a record would be ranked as recent evidence while being ineligible to be drawn for that reason at all.
    const U64 rank_cutoff = policy.mRecentTier ? policy.mRecentCutoff : 0;

    // The priority is computed ONCE per candidate rather than inside the comparator. It costs two logarithms, and a comparator that recomputes it evaluates roughly 2n*log2(n) times - a hundred thousand logarithms for a fully populated region, on the arrival frame.
    struct Candidate
    {
        U32 mIndex;
        F32 mPriority;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(n);

    for (size_t i = 0; i < n; ++i)
    {
        const SSROCRecord& rec = file.mRecords[i];

        // Nothing here can be re-created without a blob and a local id to key it under, and a record that ever tripped a hard disqualifier is never drawn on any tier - physical objects, avatars and attachments move on their own and an oversized blob could not be rezzed even by the stock path.
        if (rec.mFullID.isNull()) continue;
        if (rec.mDP.empty()) continue;
        if (rec.mLastLocalID == 0) continue;
        if (rec.mRecordFlags & SSROC_REC_DISQUALIFIED) continue;

        // A stale epoch is not a weaker candidate, it is a different object: the CacheID moved on while this record went unmentioned, so its stored id now names whatever the simulator reissued it to. Only a fresh sighting can rescue it, and until then it is not drawable at any priority.
        if (rec.mRecordFlags & SSROC_REC_ID_STALE) continue;

        const bool promoted = policy.mPromotedTier && rec.isPromoted();
        const bool recent   = policy.mRecentTier && policy.mRecentCutoff != 0 && rec.mLastConfirmed >= policy.mRecentCutoff;
        if (!promoted && !recent) continue;

        Candidate c;
        c.mIndex    = (U32)i;
        c.mPriority = ssROCInjectPriority(rec, rank_cutoff);
        candidates.push_back(c);
    }

    if (candidates.empty()) return;

    // Descending priority: most permanent and largest first. Stable on the record index so a given file always paints in the same order, which is what makes a field report of "the big things came back but the small ones did not" reproducible.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.mPriority > b.mPriority; });

    if (policy.mMaxGhosts > 0 && candidates.size() > (size_t)policy.mMaxGhosts)
    {
        candidates.resize((size_t)policy.mMaxGhosts);
    }

    // Drop children left without a root. A child's cached position is relative to its parent (llvocache.cpp:701-706), so an orphan is a fragment with nowhere to stand - the stock orphan map would simply park it until a parent that is never coming turns up.
    std::vector<LLUUID> kept_ids;
    kept_ids.reserve(candidates.size());
    for (const Candidate& c : candidates) kept_ids.push_back(file.mRecords[c.mIndex].mFullID);
    std::sort(kept_ids.begin(), kept_ids.end());

    out.reserve(candidates.size());
    for (const Candidate& c : candidates)
    {
        const U32 index = c.mIndex;
        const SSROCRecord& rec = file.mRecords[index];
        if (!rec.isRoot())
        {
            if (rec.mParentFullID.isNull()) continue;
            if (!std::binary_search(kept_ids.begin(), kept_ids.end(), rec.mParentFullID)) continue;
        }
        out.push_back(index);
    }
}

// ---------------------------------------------------------------------------
// SSROCComposition / SSROCAux
// ---------------------------------------------------------------------------

SSROCComposition::SSROCComposition()
:   mValid(false)
{
    for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i)
    {
        mStartHeight[i] = 0.f;
        mHeightRange[i] = 0.f;
    }
}

bool SSROCAux::packHeightmap(const F32* src, U32 grids_per_edge, U32 stride)
{
    if (!src || grids_per_edge == 0 || stride < grids_per_edge) return false;
    if ((U64)grids_per_edge * grids_per_edge > (U64)SSROC_HARD_REGION_BYTES / sizeof(U16)) return false;

    F32 lo = src[0];
    F32 hi = src[0];
    for (U32 row = 0; row < grids_per_edge; ++row)
    {
        const F32* r = src + (size_t)row * stride;
        for (U32 col = 0; col < grids_per_edge; ++col)
        {
            const F32 z = r[col];
            if (!std::isfinite(z)) return false;   // NaN or infinity means the surface is not usable - store nothing rather than poison a restore
            if (z < lo) lo = z;
            if (z > hi) hi = z;
        }
    }

    mHeightmapMin = lo;
    mHeightmapMax = hi;
    mHeightmapGridsPerEdge = grids_per_edge;
    mHeightmap.resize((size_t)grids_per_edge * grids_per_edge);

    const F32 range = hi - lo;
    const F32 scale = (range > 0.f) ? (65535.f / range) : 0.f;   // a perfectly flat region quantises to all-zero and restores to exactly lo

    for (U32 row = 0; row < grids_per_edge; ++row)
    {
        const F32* r = src + (size_t)row * stride;
        U16* d = mHeightmap.data() + (size_t)row * grids_per_edge;
        for (U32 col = 0; col < grids_per_edge; ++col)
        {
            const F32 q = (r[col] - lo) * scale;
            d[col] = (U16)llclamp((S32)(q + 0.5f), 0, 65535);
        }
    }
    return true;
}

bool SSROCAux::unpackHeightmap(F32* dst, U32 grids_per_edge, U32 stride) const
{
    if (!dst || grids_per_edge == 0 || stride < grids_per_edge) return false;
    if (mHeightmapGridsPerEdge != grids_per_edge) return false;   // dimension change: fall back to the live LayerData stream
    if (mHeightmap.size() != (size_t)grids_per_edge * grids_per_edge) return false;
    // Infinity passes a NaN-only test and would make the scale infinite, writing NaN into the caller's array for every zero sample.
    if (!std::isfinite(mHeightmapMin) || !std::isfinite(mHeightmapMax)) return false;
    if (mHeightmapMax < mHeightmapMin) return false;

    const F32 range = mHeightmapMax - mHeightmapMin;
    const F32 scale = range / 65535.f;

    for (U32 row = 0; row < grids_per_edge; ++row)
    {
        const U16* s = mHeightmap.data() + (size_t)row * grids_per_edge;
        F32* d = dst + (size_t)row * stride;
        for (U32 col = 0; col < grids_per_edge; ++col)
        {
            d[col] = mHeightmapMin + (F32)s[col] * scale;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// SSROCRegionFile
// ---------------------------------------------------------------------------

SSROCRegionFile::SSROCRegionFile()
:   mRegionHandle(0),
    mSavedAt(0),
    mSessionsSeen(0),
    mFlags(0)
{
}

size_t SSROCRegionFile::promotedCount() const
{
    return (size_t)std::count_if(mRecords.begin(), mRecords.end(),
                                 [](const SSROCRecord& r) { return r.isPromoted(); });
}

size_t SSROCRegionFile::approxBytes() const
{
    size_t n = 128;
    for (const SSROCRecord& r : mRecords) n += 128 + r.mDP.size();
    n += mAux.mHeightmap.size() * sizeof(U16);
    n += mManifest.size() * UUID_BYTES;
    return n;
}

// ---------------------------------------------------------------------------
// SSROCStore
// ---------------------------------------------------------------------------

SSROCStore::SSROCStore()
:   mInitialized(false),
    mShuttingDown(false),
    mReadOnly(false)
{
}

SSROCStore::~SSROCStore()
{
}

bool SSROCStore::enabled()
{
    static LLCachedControl<bool> roc_enabled(gSavedSettings, "SSROCEnabled", false);
    return roc_enabled;
}

std::string SSROCStore::gridKey() const
{
    // Region handles collide across grids, so every path is grid-scoped. Anything that is not a filesystem-safe character is folded to an underscore rather than dropped, so two grids can never map to the same directory.
    std::string grid = LLGridManager::getInstance()->getGridId();
    if (grid.empty()) grid = "unknown";

    for (char& c : grid)
    {
        if (!isalnum((unsigned char)c) && c != '-' && c != '.') c = '_';
    }
    return grid;
}

void SSROCStore::initStore()
{
    if (mInitialized || !enabled()) return;

    const std::string root = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, SSROC_DIR_NAME);
    LLFile::mkdir(root);

    mStoreDir = gDirUtilp->add(root, gridKey());
    LLFile::mkdir(mStoreDir);

    // A second instance shares the cache directory, so it reads but never writes - the stock object cache takes exactly this posture.
    mReadOnly = LLAppViewer::instance() && LLAppViewer::instance()->isSecondInstance();

    mInitialized = true;
    mShuttingDown = false;

    refreshDiskUsage();

    LL_INFOS("SSROC") << "Region object cache store at " << mStoreDir
                      << " holding " << (mDiskBytesUsed / (1024 * 1024)) << " MB" << LL_ENDL;
}

void SSROCStore::beginShutdown()
{
    mShuttingDown = true;
}

void SSROCStore::ensureInitialized()
{
    if (mInitialized || mShuttingDown || !enabled()) return;
    initStore();

    // The budget sweep used to run from the startup hook; with initialisation now deferred until after login this is its only trigger.
    enforceDiskBudget();
}

void SSROCStore::shutdownStore()
{
    if (!mInitialized) return;

    mShuttingDown = true;

    // Saves are posted to a worker that outlives nothing in particular, so quitting has to wait for them here rather than hope: the pool and LLVOCache are torn down immediately after this returns.
    std::unique_lock<std::mutex> lock(mDrainMutex);
    if (!mDrainCV.wait_for(lock, std::chrono::milliseconds(SSROC_DRAIN_TIMEOUT_MS),
                           [this]() { return mPendingOps.load() <= 0; }))
    {
        LL_WARNS("SSROC") << "Timed out draining " << mPendingOps.load()
                          << " pending region cache saves at shutdown" << LL_ENDL;
    }

    mInitialized = false;
}

std::string SSROCStore::regionFilePath(U64 handle) const
{
    return gDirUtilp->add(mStoreDir, llformat("%016llx.roc", (unsigned long long)handle));
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

bool SSROCStore::serialize(const SSROCRegionFile& file, std::vector<U8>& out)
{
    out.clear();

    std::vector<U8> objects;
    {
        Writer w(objects);
        w.pod<U32>((U32)file.mRecords.size());
        for (const SSROCRecord& r : file.mRecords)
        {
            if (r.mDP.size() > SSROC_MAX_DP_SIZE) return false;

            w.uuid(r.mFullID);
            w.uuid(r.mParentFullID);
            w.uuid(r.mOwnerID);
            w.pod(r.mLastLocalID);
            w.pod(r.mCRC);
            w.pod(r.mUpdateFlags);
            w.pod(r.mRecordFlags);
            w.pod(r.mScore);
            w.pod(r.mFirstSeen);
            w.pod(r.mLastConfirmed);
            w.pod(r.mRezTimeEpoch);
            w.pod(r.mSessionsSeen);
            w.pod(r.mCRCChangeCount);
            w.pod(r.mChildCount);
            w.pod(r.mEntryCount);
            w.pod(r.mDwellSecs);
            w.pod(r.mConfirmCount);
            w.pod(r.mMoveCount);
            w.pod(r.mMissStreak);
            w.pod(r.mBlockedBy);
            w.bytes(r.mDayBitmap, SSROC_DAY_BITMAP_BYTES);
            w.vec3(r.mPos);
            w.vec3(r.mScale);
            w.blob(r.mDP);
        }
    }

    std::vector<U8> aux;
    {
        Writer w(aux);
        w.pod(file.mAux.mWaterHeight);
        w.pod(file.mAux.mHeightmapGridsPerEdge);
        w.pod(file.mAux.mHeightmapGridsPerPatchEdge);
        w.pod(file.mAux.mHeightmapMin);
        w.pod(file.mAux.mHeightmapMax);

        const U32 samples = (U32)file.mAux.mHeightmap.size();
        w.pod(samples);
        if (samples) w.bytes(file.mAux.mHeightmap.data(), (size_t)samples * sizeof(U16));

        w.pod<U8>(file.mAux.mComposition.mValid ? 1 : 0);
        for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i) w.uuid(file.mAux.mComposition.mDetail[i]);
        for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i) w.pod(file.mAux.mComposition.mStartHeight[i]);
        for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i) w.pod(file.mAux.mComposition.mHeightRange[i]);
    }

    std::vector<U8> manifest;
    {
        Writer w(manifest);
        w.pod<U32>((U32)file.mManifest.size());
        for (const LLUUID& id : file.mManifest) w.uuid(id);
    }

    struct SectionOut { U32 mType; const std::vector<U8>* mData; };
    const SectionOut sections[] = {
        { SSROC_SECTION_OBJECTS,  &objects  },
        { SSROC_SECTION_AUX,      &aux      },
        { SSROC_SECTION_MANIFEST, &manifest },
    };
    const U32 section_count = (U32)(sizeof(sections) / sizeof(sections[0]));

    // The header and its section table are fixed width, so payload offsets are known before a single payload byte is emitted.
    const size_t header_size = 4 + 4 + 8 + UUID_BYTES + UUID_BYTES + 8 + 4 + 4 + 4
                             + (size_t)section_count * (4 + 8 + 8 + 4)
                             + 4;

    std::vector<U8> header;
    header.reserve(header_size);
    {
        Writer w(header);
        w.pod<U32>(SSROC_MAGIC);
        w.pod<U32>(SSROC_FORMAT_VERSION);
        w.pod<U64>(file.mRegionHandle);
        w.uuid(file.mRegionID);
        w.uuid(file.mLastCacheID);
        w.pod<U64>(file.mSavedAt);
        w.pod<U32>(file.mSessionsSeen);
        w.pod<U32>(file.mFlags);
        w.pod<U32>(section_count);

        U64 offset = (U64)header_size;
        for (U32 i = 0; i < section_count; ++i)
        {
            const std::vector<U8>& data = *sections[i].mData;
            w.pod<U32>(sections[i].mType);
            w.pod<U64>(offset);
            w.pod<U64>((U64)data.size());
            w.pod<U32>(crc32Of(data.data(), data.size()));
            offset += data.size();
        }
        w.pod<U32>(crc32Of(header.data(), header.size()));
    }

    if (header.size() != header_size)
    {
        LL_WARNS("SSROC") << "Header size mismatch: computed " << header_size
                          << " wrote " << header.size() << LL_ENDL;
        return false;
    }

    out.reserve(header.size() + objects.size() + aux.size() + manifest.size());
    out.insert(out.end(), header.begin(), header.end());
    for (U32 i = 0; i < section_count; ++i)
    {
        const std::vector<U8>& data = *sections[i].mData;
        out.insert(out.end(), data.begin(), data.end());
    }
    return true;
}

bool SSROCStore::deserialize(const U8* data, size_t size, SSROCRegionFile& out)
{
    Reader r(data, size);

    if (r.pod<U32>() != SSROC_MAGIC) return false;
    if (r.pod<U32>() != SSROC_FORMAT_VERSION) return false;   // wipe-never-migrate: a format bump simply orphans old files, which the budget sweep then reclaims

    out.mRegionHandle = r.pod<U64>();
    out.mRegionID     = r.uuid();
    out.mLastCacheID  = r.uuid();
    out.mSavedAt      = r.pod<U64>();
    out.mSessionsSeen = r.pod<U32>();
    out.mFlags        = r.pod<U32>();

    const U32 section_count = r.pod<U32>();
    if (!r.ok() || section_count > 32) return false;

    struct SectionIn { U32 mType; U64 mOffset; U64 mSize; U32 mCRC; };
    std::vector<SectionIn> sections(section_count);
    for (U32 i = 0; i < section_count; ++i)
    {
        sections[i].mType   = r.pod<U32>();
        sections[i].mOffset = r.pod<U64>();
        sections[i].mSize   = r.pod<U64>();
        sections[i].mCRC    = r.pod<U32>();
    }

    const size_t header_end = r.tell();
    const U32 stored_header_crc = r.pod<U32>();
    if (!r.ok()) return false;
    if (crc32Of(data, header_end) != stored_header_crc) return false;

    for (const SectionIn& s : sections)
    {
        if (s.mOffset > size || s.mSize > size || s.mOffset + s.mSize > size) return false;
        if (crc32Of(data + s.mOffset, (size_t)s.mSize) != s.mCRC) return false;

        Reader sr(data + s.mOffset, (size_t)s.mSize);

        switch (s.mType)
        {
        case SSROC_SECTION_OBJECTS:
        {
            const U32 count = sr.pod<U32>();
            if (!sr.ok()) return false;
            out.mRecords.clear();
            out.mRecords.reserve(llmin<U32>(count, 65536u));
            for (U32 i = 0; i < count; ++i)
            {
                SSROCRecord rec;
                rec.mFullID         = sr.uuid();
                rec.mParentFullID   = sr.uuid();
                rec.mOwnerID        = sr.uuid();
                rec.mLastLocalID    = sr.pod<U32>();
                rec.mCRC            = sr.pod<U32>();
                rec.mUpdateFlags    = sr.pod<U32>();
                rec.mRecordFlags    = sr.pod<U32>();
                rec.mScore          = sr.pod<F32>();
                rec.mFirstSeen      = sr.pod<U64>();
                rec.mLastConfirmed  = sr.pod<U64>();
                rec.mRezTimeEpoch   = sr.pod<U64>();
                rec.mSessionsSeen   = sr.pod<U16>();
                rec.mCRCChangeCount = sr.pod<U16>();
                rec.mChildCount     = sr.pod<U16>();
                rec.mEntryCount     = sr.pod<U16>();
                rec.mDwellSecs      = sr.pod<U32>();
                rec.mConfirmCount   = sr.pod<U16>();
                rec.mMoveCount      = sr.pod<U16>();
                rec.mMissStreak     = sr.pod<U16>();
                rec.mBlockedBy      = sr.pod<U8>();
                sr.bytes(rec.mDayBitmap, SSROC_DAY_BITMAP_BYTES);
                rec.mPos            = sr.vec3();
                rec.mScale          = sr.vec3();
                if (!sr.blob(rec.mDP, SSROC_MAX_DP_SIZE)) return false;
                if (!sr.ok()) return false;
                out.mRecords.push_back(std::move(rec));
            }
            break;
        }
        case SSROC_SECTION_AUX:
        {
            out.mAux.mWaterHeight           = sr.pod<F32>();
            out.mAux.mHeightmapGridsPerEdge = sr.pod<U32>();
            out.mAux.mHeightmapGridsPerPatchEdge = sr.pod<U16>();
            out.mAux.mHeightmapMin          = sr.pod<F32>();
            out.mAux.mHeightmapMax          = sr.pod<F32>();

            const U32 samples = sr.pod<U32>();
            if (!sr.ok() || samples > SSROC_HARD_REGION_BYTES / sizeof(U16)) return false;
            // The sample count must agree with the stored dimensions, or a restore would read past the end of a row.
            if (samples != (U64)out.mAux.mHeightmapGridsPerEdge * out.mAux.mHeightmapGridsPerEdge) return false;
            // Both dimension fields come from the same untrusted file, so they can agree with each other and still be a lie - check against the bytes actually remaining in the section before allocating.
            if ((U64)samples * sizeof(U16) > (U64)s.mSize - sr.tell()) return false;
            out.mAux.mHeightmap.resize(samples);
            if (samples && !sr.bytes(out.mAux.mHeightmap.data(), (size_t)samples * sizeof(U16))) return false;

            out.mAux.mComposition.mValid = (sr.pod<U8>() != 0);
            for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i) out.mAux.mComposition.mDetail[i]      = sr.uuid();
            for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i) out.mAux.mComposition.mStartHeight[i] = sr.pod<F32>();
            for (S32 i = 0; i < SSROC_TERRAIN_ASSETS; ++i) out.mAux.mComposition.mHeightRange[i] = sr.pod<F32>();
            if (!sr.ok()) return false;
            break;
        }
        case SSROC_SECTION_MANIFEST:
        {
            const U32 count = sr.pod<U32>();
            if (!sr.ok() || count > 1000000) return false;
            out.mManifest.clear();
            // Validate the declared count against the bytes actually present before reserving, so a crafted 4-byte section cannot cost a million iterations and a large allocation.
            if ((U64)count * UUID_BYTES + sizeof(U32) > s.mSize) return false;
            out.mManifest.reserve(count);
            for (U32 i = 0; i < count; ++i)
            {
                out.mManifest.push_back(sr.uuid());
                if (!sr.ok()) return false;
            }
            break;
        }
        default:
            // Unknown section written by a newer build within the same format version - skipping it is always safe because every section is self-describing and independently checksummed.
            break;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Blocking file IO - only ever called from a worker
// ---------------------------------------------------------------------------

bool SSROCStore::readFileBlocking(const std::string& path, std::vector<U8>& bytes)
{
    llifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    in.seekg(0, std::ios::end);
    const std::streamoff len = in.tellg();
    in.seekg(0, std::ios::beg);

    if (len <= 0 || (U64)len > (U64)SSROC_HARD_REGION_BYTES * 2) return false;

    bytes.resize((size_t)len);
    in.read((char*)bytes.data(), len);
    return in.good() || in.eof();
}

bool SSROCStore::writeFileBlocking(const std::string& path, const std::vector<U8>& bytes)
{
    // Write to a sibling temp file and rename over the target, so a kill mid-write leaves the previous good file intact rather than a half-written one. The scratch name is unique per writer and per call: two viewers sharing a cache, or a save racing a re-save of the same region, would otherwise truncate each other's scratch file and rename the blend into place.
    static std::atomic<U32> s_tmp_seq(0);
    const std::string tmp = llformat("%s.%d.%u.tmp", path.c_str(), (int)LLApp::getPid(), (U32)(s_tmp_seq++));

    {
        llofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        if (!out.good()) return false;
    }

    // No unlink first: rename replaces the destination on both platforms (MoveFileExW with MOVEFILE_REPLACE_EXISTING on Windows), and removing it opens a window in which the previous good file is simply gone.
    if (LLFile::rename(tmp, path, ENOENT) != 0)
    {
        LLFile::remove(tmp, ENOENT);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Async entry points
// ---------------------------------------------------------------------------

void SSROCStore::loadRegionAsync(U64 handle, SSROCLoadCallback cb)
{
    if (!mInitialized || mShuttingDown || !enabled() || !cb)
    {
        if (cb) cb(handle, SSROCFilePtr());
        return;
    }

    LL::WorkQueue::ptr_t main_queue    = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue)
    {
        cb(handle, SSROCFilePtr());
        return;
    }

    const std::string path = regionFilePath(handle);
    ++mPendingOps; // loads touch store state on a worker, so shutdown has to wait for them exactly as it does for saves

    const bool posted_load = main_queue->postTo(
        general_queue,
        [this, handle, path]() -> SSROCFilePtr
        {
            OpGuard guard(this);

            std::vector<U8> bytes;
            if (!readFileBlocking(path, bytes))
            {
                ++mMetrics.mLoadsMissing;
                return SSROCFilePtr();
            }

            SSROCFilePtr file = std::make_shared<SSROCRegionFile>();
            if (!deserialize(bytes.data(), bytes.size(), *file))
            {
                // Corruption costs nothing but a rebuild: the file is deleted outright rather than salvaged, and only this region is affected.
                ++mMetrics.mLoadsCorrupt;
                LLFile::remove(path, ENOENT);
                return SSROCFilePtr();
            }

            if (file->mRegionHandle != handle)
            {
                ++mMetrics.mLoadsCorrupt;
                LLFile::remove(path, ENOENT);
                return SSROCFilePtr();
            }

            mMetrics.mBytesRead += bytes.size();
            ++mMetrics.mLoadsOk;
            return file;
        },
        [handle, cb](SSROCFilePtr file)
        {
            cb(handle, file);
        });

    // postTo returns false rather than throwing when the target queue is closed, in which case the worker never runs: the counter would leak forever and the caller would wait for a callback that can never arrive. The header promises the callback fires exactly once, so honour it here.
    if (!posted_load)
    {
        --mPendingOps;
        cb(handle, SSROCFilePtr());
    }
}

void SSROCStore::saveRegionAsync(SSROCFilePtr file)
{
    // Saves are still accepted while shutting down - the whole point of the drain is to let a region that is being destroyed at quit finish writing. Loads are not, since nothing would consume them.
    if (!mInitialized || !enabled() || !file) return;
    if (mReadOnly) return;

    if (file->approxBytes() > SSROC_HARD_REGION_BYTES)
    {
        LL_WARNS("SSROC") << "Refusing to save oversized region cache for handle "
                          << file->mRegionHandle << LL_ENDL;
        return;
    }

    const std::string path = regionFilePath(file->mRegionHandle);

    LL::WorkQueue::ptr_t main_queue    = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");

    // Inline write during shutdown, or whenever the worker queues are gone. Posting here would look like success and silently drop the write, which is exactly what happened to the last three regions of the first real test session.
    if (mShuttingDown || !main_queue || !general_queue || general_queue->isClosed())
    {
        std::vector<U8> bytes;
        if (serialize(*file, bytes) && writeFileBlocking(path, bytes))
        {
            mMetrics.mBytesWritten += bytes.size();
            mDiskBytesUsed += bytes.size();
            ++mMetrics.mSavesOk;
        }
        else
        {
            ++mMetrics.mSavesFailed;
            LL_WARNS("SSROC") << "Failed writing region cache " << path << " inline" << LL_ENDL;
        }
        return;
    }

    ++mPendingOps;

    const bool posted_save = main_queue->postTo(
        general_queue,
        [this, file, path]() -> bool
        {
            OpGuard guard(this);

            std::vector<U8> bytes;
            bool ok = serialize(*file, bytes) && writeFileBlocking(path, bytes);
            if (ok)
            {
                mMetrics.mBytesWritten += bytes.size();
                mDiskBytesUsed += bytes.size();
                ++mMetrics.mSavesOk;
            }
            else
            {
                ++mMetrics.mSavesFailed;
                LL_WARNS("SSROC") << "Failed writing region cache " << path << LL_ENDL;
            }
            return ok;
        },
        [](bool)
        {
        });

    // A rejected post means the write never happens; without this the pending count never returns to zero and every subsequent quit burns the full drain timeout.
    if (!posted_save)
    {
        --mPendingOps;
        ++mMetrics.mSavesFailed;
        LL_WARNS("SSROC") << "Region cache save for " << file->mRegionHandle << " could not be queued" << LL_ENDL;
    }
}

void SSROCStore::deleteRegion(U64 handle)
{
    if (!mInitialized || mReadOnly) return;
    LLFile::remove(regionFilePath(handle), ENOENT);
}

void SSROCStore::purgeAll()
{
    // A second instance must not delete the primary's cache out from under it, exactly as LLVOCache::removeCache refuses in read-only mode.
    if (mReadOnly) return;

    // Called from every cache-clear path. A region cache records where the user has been, so leaving it behind after "clear cache" would be a privacy lie - see doc/region_object_cache.md.
    const std::string root = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, SSROC_DIR_NAME);
    if (gDirUtilp->fileExists(root))
    {
        gDirUtilp->deleteDirAndContents(root);
    }
    mDiskBytesUsed = 0;

    // A mid-session clear must leave the store usable rather than silently disabled for the rest of the session, so the directory is recreated immediately if the store was already running.
    if (mInitialized)
    {
        LLFile::mkdir(root);
        LLFile::mkdir(mStoreDir);
    }

    LL_INFOS("SSROC") << "Purged region object cache" << LL_ENDL;
}

// ---------------------------------------------------------------------------
// Disk budget
// ---------------------------------------------------------------------------

void SSROCStore::refreshDiskUsage()
{
    if (mStoreDir.empty()) return;

    U64 total = 0;
    std::string name;
    LLDirIterator iter(mStoreDir, "*.roc");
    while (iter.next(name))
    {
        llstat st;
        if (LLFile::stat(gDirUtilp->add(mStoreDir, name), &st) == 0 && st.st_size > 0)
        {
            total += (U64)st.st_size;
        }
    }
    mDiskBytesUsed = total;
}

void SSROCStore::enforceDiskBudget()
{
    if (!mInitialized) return;

    static LLCachedControl<U32> budget_mb(gSavedSettings, "SSROCDiskBudgetMB", 2048);
    const U64 budget = (U64)budget_mb * 1024 * 1024;
    if (budget == 0) return;

    refreshDiskUsage();
    if (mDiskBytesUsed <= budget) return;

    // Lax watermark, matching the BC7 tier's policy: nothing is touched until the budget is actually approached, and then whole region files are dropped least-recently-visited first. Last-visit time is a drop-priority signal, never an expiry.
    struct Victim { std::string mPath; U64 mSize; time_t mTime; };
    std::vector<Victim> files;

    std::string name;
    LLDirIterator iter(mStoreDir, "*.roc");
    while (iter.next(name))
    {
        const std::string path = gDirUtilp->add(mStoreDir, name);
        llstat st;
        if (LLFile::stat(path, &st) == 0)
        {
            files.push_back({ path, (U64)llmax<S64>(0, (S64)st.st_size), st.st_mtime });
        }
    }

    std::sort(files.begin(), files.end(),
              [](const Victim& a, const Victim& b) { return a.mTime < b.mTime; });

    U64 used = mDiskBytesUsed;
    for (const Victim& v : files)
    {
        if (used <= budget) break;
        if (LLFile::remove(v.mPath, ENOENT) == 0)
        {
            used -= llmin(used, v.mSize);
            ++mMetrics.mFilesEvicted;
        }
    }
    mDiskBytesUsed = used;

    LL_INFOS("SSROC") << "Region cache trimmed to " << (used / (1024 * 1024)) << " MB" << LL_ENDL;
}

void SSROCStore::endOp()
{
    if (--mPendingOps <= 0)
    {
        std::lock_guard<std::mutex> lock(mDrainMutex);
        mDrainCV.notify_all();
    }
}

std::string SSROCStore::metricsString() const
{
    return llformat("ROC loads %u/%u miss %u corrupt %u | saves %u fail %u | evicted %u | disk %llu MB",
                    mMetrics.mLoadsOk.load(),
                    mMetrics.mLoadsOk.load() + mMetrics.mLoadsMissing.load(),
                    mMetrics.mLoadsMissing.load(),
                    mMetrics.mLoadsCorrupt.load(),
                    mMetrics.mSavesOk.load(),
                    mMetrics.mSavesFailed.load(),
                    mMetrics.mFilesEvicted.load(),
                    (unsigned long long)(mDiskBytesUsed.load() / (1024 * 1024)));
}
