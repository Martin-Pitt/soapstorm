/**
 * @file ssstratapack.cpp
 * @brief Strata asset volume store, policy half - which loose file is ready to pack and which volume dies, see doc/strata.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ssstrata.h"
#include "ssserial.h"

#include "llapp.h"
#include "lldir.h"
#include "llfile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// Everything here runs on LLPurgeDiskCacheThread and nowhere else. It is the only thread in the tier that reads or writes more than one object at a time, and it is deliberately not the main thread and not a fetch thread: the whole point of leaving writes on the loose path is that no thread the user can feel ever waits on container IO.

void SSStrataStore::notePackVerdict(ESSStrataPackVerdict v)
{
    ++mMetrics.mPackVerdicts[v];
}

void SSStrataStore::noteReclaimVerdict(ESSStrataReclaimVerdict v)
{
    ++mMetrics.mReclaimVerdicts[v];
}

// How long an object must sit unmodified before the packer will take it. The base is a plain settle time; the doubling is what a repeatedly patched object earns for itself. llmeshrepository.cpp fills a mesh in LOD by LOD over the life of the object, so a mesh that has already been dragged back out of a volume once is exactly the object that should not be packed again on the same timer.
U32 SSStrataStore::settleSecondsFor(const LLUUID& id) const
{
    U32 shift = 0;
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        auto it = mUnstable.find(id);
        if (it != mUnstable.end()) shift = llmin((U32)it->second, SSSTRATA_UNSTABLE_SHIFT_MAX);
    }
    // Clamped rather than shifted blindly: the base comes from a user setting, and a doubling of an absurd value would wrap the U32 and turn the longest settle time into the shortest one.
    const U32 base = llmin(mConfig.mPackAgeSecs, (U32)(0xFFFFFFFFu >> SSSTRATA_UNSTABLE_SHIFT_MAX));
    return base << shift;
}

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

ESSStrataPackVerdict SSStrataStore::packPass(std::vector<SSStrataLooseFile>& files)
{
    if (!enabled())                     { notePackVerdict(SSSTRATA_PACK_DISABLED);  return SSSTRATA_PACK_DISABLED; }
    if (!mInitialized || mShuttingDown) { notePackVerdict(SSSTRATA_PACK_NOT_READY); return SSSTRATA_PACK_NOT_READY; }
    if (mReadOnly)                      { notePackVerdict(SSSTRATA_PACK_READ_ONLY); return SSSTRATA_PACK_READ_ONLY; }
    if (files.empty())                  { notePackVerdict(SSSTRATA_PACK_NO_CANDIDATES); return SSSTRATA_PACK_NO_CANDIDATES; }

    // The pass owns the mutual exclusion itself, with a single acquire and a single release, rather than threading a clear through a dozen early returns - which is exactly where a "declined" that never unlocks would hide.
    bool expected = false;
    if (!mPackRunning.compare_exchange_strong(expected, true))
    {
        notePackVerdict(SSSTRATA_PACK_ALREADY_RUNNING);
        return SSSTRATA_PACK_ALREADY_RUNNING;
    }

    const ESSStrataPackVerdict v = packPassLocked(files);
    mPackRunning = false;
    notePackVerdict(v);
    return v;
}

ESSStrataPackVerdict SSStrataStore::packPassLocked(std::vector<SSStrataLooseFile>& files)
{
    ++mMetrics.mPackPasses;

    const U32 gen    = mGeneration.load();
    const U64 budget = mBudgetBytes.load();

    // Catch-up exists for exactly one situation, and it is the one the user is in on the very first run: the entire cache is still loose. Steady state is a trickle, so the constant that governs it is chosen for "invisible background IO" rather than for throughput.
    const bool catchup = files.size() > SSSTRATA_CATCHUP_FILES;
    const U64 byte_budget = catchup ? SSSTRATA_PACK_BYTES_CATCHUP : SSSTRATA_PACK_BYTES_TICK;

    struct Pending
    {
        size_t          mIndex;
        SSStrataRecord  mRecord;
    };
    std::vector<Pending> pending;
    std::vector<U8> payload;

    U64 bytes_done = 0;
    U32 files_done = 0;
    U32 examined = 0;
    ESSStrataPackVerdict early = SSSTRATA_PACK_RAN;

    for (size_t i = 0; i < files.size(); ++i)
    {
        if (!LLApp::isRunning() || mShuttingDown) break;
        if (mGeneration.load() != gen) { early = SSSTRATA_PACK_STALE; break; }
        if (bytes_done >= byte_budget || files_done >= SSSTRATA_PACK_FILES_TICK) break;

        SSStrataLooseFile& lf = files[i];
        if (lf.mPacked) continue;
        ++examined;

        if (lf.mPinned)                 { ++mMetrics.mSkipPinned;  continue; }
        if (lf.mUUID.isNull())          { ++mMetrics.mSkipBadName; continue; }
        if (lf.mSize == 0)              { ++mMetrics.mSkipEmpty;   continue; }
        if (lf.mSize > mConfig.mMaxObjectBytes) { ++mMetrics.mSkipTooBig; continue; }

        // A duplicate is what a crash between the index flush and the unlink leaves behind. Both copies are byte-identical by construction, so the loose one is simply removed rather than packed a second time.
        {
            std::lock_guard<std::mutex> lock(mMapMutex);
            if (mIndex.find(lf.mUUID) != mIndex.end())
            {
                if (LLFile::remove(lf.mPath, ENOENT) == 0)
                {
                    lf.mPacked = true;
                    ++mMetrics.mSkipAlreadyPacked;
                }
                continue;
            }
        }

        const U32 settle = settleSecondsFor(lf.mUUID);
        const U32 now = ssStrataNowSeconds();
        if (lf.mTime <= 0 || (U64)now < (U64)lf.mTime + (U64)settle)
        {
            if (settle > mConfig.mPackAgeSecs) ++mMetrics.mSkipUnstable;
            else                           ++mMetrics.mSkipTooYoung;
            continue;
        }

        // Refuse to grow past the slider even if the purge that normally enforces it is asleep. A tier that can outrun its own budget between watermark checks is how the BC7 store reached 2.2 GB on top of a 16 GB texture cache without anything being wrong by its own reckoning.
        if (budget && allocatedBytes() + (U64)SSSTRATA_BLOB_HEADER_SIZE + lf.mSize + SSSTRATA_ALIGN > budget)
        {
            ++mMetrics.mSkipBudget;
            early = SSSTRATA_PACK_OVER_BUDGET;
            break;
        }

        // Re-stat rather than trusting the scan. The purge walk that produced this list can be tens of seconds old on a large cache, and an object that a fetch thread touched in between must not be captured half-written.
        llstat st;
        if (LLFile::stat(lf.mPath, &st, ENOENT) != 0) { ++mMetrics.mSkipUnreadable; continue; }
        if ((U64)st.st_size != lf.mSize || (std::time_t)st.st_mtime != lf.mTime) { ++mMetrics.mSkipChanged; continue; }

        // The ownership token. From here until the commit, any write gate that touches this uuid erases it from mPacking under the same lock, which silently revokes this pass's claim and is the only thing standing between a concurrent write and a freshly written loose file being unlinked underneath it.
        {
            std::lock_guard<std::mutex> lock(mMapMutex);
            mPacking.insert(lf.mUUID);
        }

        payload.resize((size_t)lf.mSize);
        bool read_ok = false;
        if (LLFILE* f = LLFile::fopen(lf.mPath, "rb"))
        {
            read_ok = fread(payload.data(), 1, (size_t)lf.mSize, f) == (size_t)lf.mSize;
            LLFile::close(f);
        }
        if (!read_ok)
        {
            std::lock_guard<std::mutex> lock(mMapMutex);
            mPacking.erase(lf.mUUID);
            ++mMetrics.mSkipUnreadable;
            continue;
        }

        // The tenant's own type first, because a tenant that holds exactly one kind of object never populates mPendingTypes at all.
        U8 asset_type = mConfig.mDefaultAssetType;
        if (asset_type == 0xFF)
        {
            std::lock_guard<std::mutex> lock(mMapMutex);
            auto it = mPendingTypes.find(lf.mUUID);
            if (it != mPendingTypes.end()) asset_type = it->second;
        }

        std::vector<U8> blob;
        SSStrataRecord rec;
        if (!ssStrataBuildBlob(lf.mUUID, payload.data(), (U32)lf.mSize, asset_type, blob, rec))
        {
            std::lock_guard<std::mutex> lock(mMapMutex);
            mPacking.erase(lf.mUUID);
            ++mMetrics.mSkipUnreadable;
            continue;
        }

        // One object per lock acquisition, on purpose. Holding mStoreMutex across a whole 512 MB catch-up batch would park any fetch thread that happened to remove or rename a cached asset behind it, and that is precisely the stall this design exists to avoid. The cost is one open and one close per object, which is the same order as the read of the loose file it replaces.
        bool appended = false;
        {
            std::lock_guard<std::mutex> lock(mStoreMutex);
            if (mGeneration.load() == gen)
            {
                U16 vol = 0;
                U64 off = 0;
                if (appendBlob(blob, vol, off))
                {
                    rec.mVolume = vol;
                    rec.mOffset = off;
                    appended = true;
                }
            }
        }
        if (!appended)
        {
            std::lock_guard<std::mutex> lock(mMapMutex);
            mPacking.erase(lf.mUUID);
            early = SSSTRATA_PACK_VOLUME_WRITE_FAILED;
            break;
        }

        Pending p;
        p.mIndex  = i;
        p.mRecord = rec;
        pending.push_back(p);

        bytes_done += lf.mSize;
        ++files_done;
    }

    if (pending.empty())
    {
        if (early != SSSTRATA_PACK_RAN) return early;
        return examined ? SSSTRATA_PACK_NOTHING_READY : SSSTRATA_PACK_NO_CANDIDATES;
    }

    // COMMIT, and the order is the crash rule. The in-memory index is published first, under one lock, so the check for a revoked token and the publication cannot be separated by a writer; the volume already holds every blob and has been flushed, so a reader that arrives now gets correct bytes even though the on-disk record is still a moment away.
    std::vector<SSStrataRecord> to_write;
    std::vector<size_t> to_unlink;
    to_write.reserve(pending.size());
    to_unlink.reserve(pending.size());
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (const Pending& p : pending)
        {
            const LLUUID& id = p.mRecord.mUUID;
            if (mPacking.erase(id) == 0)
            {
                ++mMetrics.mSkipRaced;   // a write gate took it back; the blob stays in the volume as unreferenced bytes the next rollover overwrites
                continue;
            }
            mIndex[id] = p.mRecord;
            ++mLiveRecords;
            to_write.push_back(p.mRecord);
            to_unlink.push_back(p.mIndex);
        }
    }

    if (to_write.empty()) return SSSTRATA_PACK_NOTHING_READY;

    bool index_ok = false;
    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        index_ok = (mGeneration.load() == gen) && appendIndexRecords(to_write);
    }

    if (!index_ok)
    {
        // Nothing is unlinked and the in-memory publication is rolled back, so the loose files stay authoritative and the blobs already written become unreferenced bytes past a frontier that was never advanced in the index.
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (const SSStrataRecord& rec : to_write)
        {
            if (mIndex.erase(rec.mUUID)) --mLiveRecords;
        }
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] could not commit " << to_write.size() << " index records (errno " << errno << "); the loose files were left alone" << LL_ENDL;
        return SSSTRATA_PACK_INDEX_WRITE_FAILED;
    }

    // Only now is the loose copy redundant. The re-check is not paranoia: a write gate may have unpacked one of these between the commit and here, which rewrote the loose file, and unlinking it then would destroy the newer copy.
    U32 unlinked = 0;
    U64 unlinked_bytes = 0;
    for (size_t idx : to_unlink)
    {
        SSStrataLooseFile& lf = files[idx];
        {
            std::lock_guard<std::mutex> lock(mMapMutex);
            if (mIndex.find(lf.mUUID) == mIndex.end()) continue;
        }
        if (LLFile::remove(lf.mPath, ENOENT) != 0)
        {
            ++mMetrics.mUnlinkFailures;
            LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] packed " << lf.mUUID << " but could not remove " << lf.mPath << " (errno " << errno << "); the duplicate is removed on a later pass" << LL_ENDL;
            continue;
        }
        lf.mPacked = true;
        ++unlinked;
        unlinked_bytes += lf.mSize;
    }

    mMetrics.mPacked += unlinked;
    mMetrics.mPackedBytes += unlinked_bytes;

    LL_INFOS("Strata") << "Strata[" << mConfig.mTag << "] packed " << unlinked << " objects (" << (unlinked_bytes / 1024) << " KB) into "
                       << volumeCount() << " volumes" << (catchup ? " [catching up]" : "")
                       << "; " << mLiveRecords.load() << " objects now live" << LL_ENDL;

    return SSSTRATA_PACK_RAN;
}

// ---------------------------------------------------------------------------
// Reclaim
// ---------------------------------------------------------------------------

ESSStrataReclaimVerdict SSStrataStore::reclaimColdest(U16 max_day, bool respect_cooldown, U64& out_freed, std::vector<LLUUID>* out_dropped)
{
    out_freed = 0;
    if (out_dropped) out_dropped->clear();

    if (!enabled())                     { noteReclaimVerdict(SSSTRATA_RECLAIM_DISABLED);  return SSSTRATA_RECLAIM_DISABLED; }
    if (!mInitialized || mShuttingDown) { noteReclaimVerdict(SSSTRATA_RECLAIM_NOT_READY); return SSSTRATA_RECLAIM_NOT_READY; }
    if (mReadOnly)                      { noteReclaimVerdict(SSSTRATA_RECLAIM_READ_ONLY); return SSSTRATA_RECLAIM_READ_ONLY; }

    bool expected = false;
    if (!mReclaimRunning.compare_exchange_strong(expected, true))
    {
        noteReclaimVerdict(SSSTRATA_RECLAIM_ALREADY_RUNNING);
        return SSSTRATA_RECLAIM_ALREADY_RUNNING;
    }

    const ESSStrataReclaimVerdict v = reclaimColdestLocked(max_day, respect_cooldown, out_freed, out_dropped);
    mReclaimRunning = false;
    noteReclaimVerdict(v);
    return v;
}

ESSStrataReclaimVerdict SSStrataStore::reclaimColdestLocked(U16 max_day, bool respect_cooldown, U64& out_freed, std::vector<LLUUID>* out_dropped)
{
    const U32 gen = mGeneration.load();
    const U32 now = ssStrataNowSeconds();

    // The cooldown guards the START of a run, not the steps inside one. A caller draining to a low-water mark has already computed how many bytes it needs and stops when it has them, so applying the cooldown per kill would break that contract rather than protect anything.
    if (respect_cooldown)
    {
        const U32 last = mLastReclaimSecond.load();
        if (last && now >= last && (now - last) < SSSTRATA_RECLAIM_COOLDOWN_SECS) return SSSTRATA_RECLAIM_COOLDOWN;
    }

    struct VolStat
    {
        U64 mBytes{0};
        U64 mHotBytes{0};
        U64 mDaySum{0};     // last-use day weighted by bytes, so one recently touched object cannot make a whole volume of cold ones look warm
        U32 mObjects{0};
        U32 mHotObjects{0};
    };
    std::unordered_map<U16, VolStat> stats;

    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (const auto& entry : mIndex)
        {
            const SSStrataRecord& rec = entry.second;
            VolStat& vs = stats[rec.mVolume];
            vs.mBytes += rec.mSize;
            ++vs.mObjects;
            vs.mDaySum += (U64)rec.mLastUseDay * (U64)rec.mSize;

            // A hard recency floor that does not depend on mLastUseDay's one-day resolution: anything referenced this recently is never counted as cold, which is what stops a cache filled in a single session from being scored in arbitrary order.
            if (rec.mTouchSecond && now >= rec.mTouchSecond && (now - rec.mTouchSecond) < SSSTRATA_RECLAIM_HOT_SECS)
            {
                vs.mHotBytes += rec.mSize;
                ++vs.mHotObjects;
            }
        }
    }

    U16 victim = 0;
    bool have_victim = false;
    bool saw_candidate = false;
    bool saw_hot = false;
    U16 best_day = 0xFFFF;

    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        for (const auto& entry : stats)
        {
            const U16 vol = entry.first;
            if ((U32)vol == mCurrentVolume) continue;   // the write head is where the next object goes, so it is never a candidate
            if ((U32)vol == mDyingVolume) continue;
            saw_candidate = true;

            const VolStat& vs = entry.second;
            if (vs.mBytes && (vs.mHotBytes * 100 / vs.mBytes) > SSSTRATA_RECLAIM_HOT_PERCENT)
            {
                saw_hot = true;
                continue;   // the tier sits above its watermark and says so rather than dropping what the user is looking at right now
            }

            // A BYTES-WEIGHTED MEAN rather than the newest member, and the difference matters: scoring a volume by its hottest object would mean that in steady state - where the loose tier is a handful of freshly written files and the volumes are everything else - no volume ever scores older than the oldest loose file, so the caller would drain its small warm staging area and never touch the four gigabytes of genuinely cold data. The hot-byte veto below is what protects the objects the user is looking at right now, and it does it far more precisely than a maximum ever could.
            const U16 mean_day = (U16)(vs.mBytes ? (vs.mDaySum / vs.mBytes) : 0);
            if (!have_victim || mean_day < best_day)
            {
                best_day = mean_day;
                victim = vol;
                have_victim = true;
            }
        }
    }

    if (!have_victim) return saw_candidate ? (saw_hot ? SSSTRATA_RECLAIM_ALL_HOT : SSSTRATA_RECLAIM_NO_CANDIDATE) : SSSTRATA_RECLAIM_NO_CANDIDATE;

    // The caller passes the day of the oldest loose file it would otherwise delete, so the two tiers drain oldest-first without either of them having to know how the other stores a timestamp.
    if (best_day > max_day) return SSSTRATA_RECLAIM_TOO_YOUNG;

    // Collect what dies from the INDEX rather than from mVolMembers, which is a scoring aid and is allowed to hold uuids that have since been superseded or forgotten.
    std::vector<SSStrataRecord> tombstones;
    std::vector<LLUUID> dying;
    U32 hot_dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (const auto& entry : mIndex)
        {
            if (entry.second.mVolume != victim) continue;
            dying.push_back(entry.first);

            SSStrataRecord tomb;
            tomb.mUUID = entry.first;
            tomb.mFlags = SSSTRATA_FLAG_TOMBSTONE;
            tomb.mLastUseDay = ssStrataToday();
            tombstones.push_back(tomb);

            if (entry.second.mTouchSecond && now >= entry.second.mTouchSecond && (now - entry.second.mTouchSecond) < SSSTRATA_RECLAIM_HOT_SECS) ++hot_dropped;
        }
    }

    U64 frontier = 0;
    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        if (mGeneration.load() != gen) return SSSTRATA_RECLAIM_STALE;
        mDyingVolume = victim;   // so the allocator cannot hand this id out while the kill is still working through it
        frontier = mVolFrontier[victim];

        if (!appendIndexRecords(tombstones))
        {
            mDyingVolume = SSSTRATA_MAX_VOLUMES;
            LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] could not write " << tombstones.size() << " tombstones for volume " << victim << " (errno " << errno << "); nothing on disk changed" << LL_ENDL;
            return SSSTRATA_RECLAIM_INDEX_WRITE_FAILED;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (const LLUUID& id : dying)
        {
            if (mIndex.erase(id)) --mLiveRecords;
        }
    }

    // NEVER rolled back. The records are already dead on disk, and un-killing them is where this design would wedge; a file that would not go is an orphan the startup sweep removes.
    const bool unlinked = killVolumeFile(victim, gen);

    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        if (mAllocBytes.load() >= frontier) mAllocBytes -= frontier;
        else                                mAllocBytes = 0;
        mVolFrontier[victim] = 0;
        mDyingVolume = SSSTRATA_MAX_VOLUMES;
        if (std::find(mFreeVolumes.begin(), mFreeVolumes.end(), victim) == mFreeVolumes.end())
        {
            mFreeVolumes.push_back(victim);
            std::sort(mFreeVolumes.begin(), mFreeVolumes.end());
        }
    }

    // Handed back only here, past the tombstone write, so a caller that drops its own entries on the strength of this list can never do so for a volume whose records survived.
    if (out_dropped) out_dropped->assign(dying.begin(), dying.end());

    mLastReclaimSecond = now;
    ++mMetrics.mVolumesKilled;
    mMetrics.mBytesReclaimed += frontier;
    mMetrics.mObjectsDropped += (U32)dying.size();
    mMetrics.mHotObjectsDropped += hot_dropped;
    out_freed = frontier;

    if (!unlinked)
    {
        ++mMetrics.mUnlinkFailures;
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] killed the records of volume " << victim << " but could not unlink the file (errno " << errno << "); it is an orphan for the next startup sweep" << LL_ENDL;
        return SSSTRATA_RECLAIM_UNLINK_FAILED;
    }

    LL_INFOS("Strata") << "Strata[" << mConfig.mTag << "] reclaimed volume " << victim << ": " << (frontier / (1024 * 1024)) << " MB, "
                       << dying.size() << " objects (" << hot_dropped << " of them still warm), mean use day " << best_day << LL_ENDL;
    return SSSTRATA_RECLAIM_RAN;
}
