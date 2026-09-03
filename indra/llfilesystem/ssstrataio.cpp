/**
 * @file ssstrataio.cpp
 * @brief Strata asset volume store, IO half - directories, handles, recovery and the read and write gates, see doc/strata.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ssstrata.h"
#include "ssserial.h"

#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

// Split from ssstrata.cpp deliberately: everything that decides what the bytes MEAN lives there and depends on nothing, so it can be tested offline; everything that decides where they GO lives here. The pack and reclaim POLICY - which loose file is ready and which volume dies - lives next door again in ssstratapack.cpp, so that there is exactly one place a path is built and exactly one place a volume is deleted.

namespace
{
    // Every offset in this store is bounded by SSSTRATA_VOLUME_CAP for volumes and by record count times 64 for the index, so a 32 bit fseek is always enough; the guard is here so that stops being an assumption if either bound ever moves.
    bool ssStrataWriteAt(LLFILE* f, U64 offset, const void* data, size_t bytes)
    {
        if (offset > 0x7FFFFFFFull) return false;
        if (fseek(f, (long)offset, SEEK_SET) != 0) return false;
        return fwrite(data, 1, bytes, f) == bytes;
    }

    LLFILE* ssStrataOpenRW(const std::string& path)
    {
        LLFILE* f = LLFile::fopen(path, "r+b");
        if (!f) f = LLFile::fopen(path, "w+b");
        return f;
    }

}

std::atomic<SSStrataStore*> SSStrataStore::sLive[SSSTRATA_TENANT_COUNT] = {};

// LEAKED ON PURPOSE, one per tenant. A store that is never destroyed cannot be resurrected by a fetch thread
// arriving during shutdown, which is the hazard live() exists to sidestep, and it cannot be torn down out of
// order against LLDiskCache or LLTextureCache either. Teardown is shutdownStore(), which unpublishes the
// instance and prints the session's metrics; what the operating system reclaims afterwards is not this file's
// business. The pointers are function-local statics so construction is thread safe without a lock of our own.
SSStrataStore& SSStrataStore::tier(ESSStrataTenant tenant)
{
    static SSStrataStore* stores[SSSTRATA_TENANT_COUNT] = {};
    static std::once_flag once;
    std::call_once(once, []()
    {
        for (U32 i = 0; i < SSSTRATA_TENANT_COUNT; ++i)
        {
            stores[i] = new SSStrataStore();
            stores[i]->mTenant = (ESSStrataTenant)i;
            stores[i]->mConfig.mTag = ssStrataTenantName((ESSStrataTenant)i);
        }
    });
    if (tenant >= SSSTRATA_TENANT_COUNT) tenant = SSSTRATA_TENANT_ASSETS;
    return *stores[tenant];
}

void SSStrataStore::configure(const Config& cfg)
{
    mConfig = cfg;
    // The tenant's own name wins over whatever the caller left in the struct, because a mislabelled log line is worse than an unlabelled one.
    mConfig.mTag = ssStrataTenantName(mTenant);
}

SSStrataStore::SSStrataStore()
{
    // 32 KB of frontiers, allocated once, so the budget can be enforced against what the volumes actually occupy rather than against the sum of live object sizes.
    mVolFrontier.assign((size_t)SSSTRATA_MAX_VOLUMES, (U64)0);
}

SSStrataStore::~SSStrataStore()
{
    sLive[mTenant].store(nullptr, std::memory_order_release);
}

std::string SSStrataStore::indexPath() const
{
    return gDirUtilp->add(mStoreDir, SSSTRATA_IDX_NAME);
}

std::string SSStrataStore::volumePath(U32 volume) const
{
    return gDirUtilp->add(mStoreDir, llformat("strata_%03u.dat", volume));
}

U64 SSStrataStore::allocatedBytes() const
{
    // The sum of the volume frontiers plus the index file, which is what the user paid for on disk. A total built on live object sizes would let the tier sit permanently over the slider, because a tombstone drops the object's bytes from that total instantly while the disk does not move until the unlink.
    return mAllocBytes.load() + (U64)SSSTRATA_HEADER_SIZE + (U64)mRecordCursor.load() * SSSTRATA_RECORD_SIZE;
}

U32 SSStrataStore::volumeCount() const
{
    std::lock_guard<std::mutex> lock(mStoreMutex);
    U32 n = 0;
    for (U32 v = 0; v < SSSTRATA_MAX_VOLUMES; ++v)
    {
        if (mVolFrontier[v] != 0) ++n;
    }
    return n;
}

void SSStrataStore::setBudgetBytes(U64 bytes)
{
    mBudgetBytes = bytes;
}

// The rollover threshold is derived from the budget rather than fixed, for the reason written down beside SSBC7_SEGMENT_BUCKETS: reclaim is one unlink, so the number of volumes IS the precision of one kill. At the shipped 16384 MB disk cache budget this lands on the 256 MiB runtime ceiling and sixty-four buckets; at a 512 MB budget it lands on the 16 MiB floor, which is what keeps a small cache from being a single file that has to be thrown away whole.
void SSStrataStore::deriveVolumeCap()
{
    const U64 budget = mBudgetBytes.load();
    U64 cap = budget / SSSTRATA_VOLUME_BUCKETS;
    if (cap < SSSTRATA_VOLUME_CAP_MIN) cap = SSSTRATA_VOLUME_CAP_MIN;
    if (cap > SSSTRATA_VOLUME_CAP_MAX) cap = SSSTRATA_VOLUME_CAP_MAX;
    cap = (cap + (SSSTRATA_ALIGN - 1)) & ~(U64)(SSSTRATA_ALIGN - 1);
    if (cap > SSSTRATA_VOLUME_CAP) cap = SSSTRATA_VOLUME_CAP;   // never past the FORMAT ceiling, which is what deserializeRecord validates against
    mVolumeCap = cap;
}

void SSStrataStore::initStore(const std::string& cache_dir, U64 budget_bytes)
{
    if (mInitialized) return;
    if (cache_dir.empty())
    {
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] not started: its cache directory is not set" << LL_ENDL;
        return;
    }

    // TWO TENANTS MUST NEVER SHARE A DIRECTORY. Their indexes have the same name and their volumes the same numbering, so a mis-set cache path that pointed both at one place would have each store treat the other's volumes as orphans and unlink them. The check costs one string compare at startup and turns a silent mutual wipe into a refusal that says so.
    const std::string want_dir = gDirUtilp->add(cache_dir, SSSTRATA_DIR_NAME);
    for (U32 i = 0; i < SSSTRATA_TENANT_COUNT; ++i)
    {
        if ((ESSStrataTenant)i == mTenant) continue;
        const SSStrataStore* other = sLive[i].load(std::memory_order_acquire);
        if (other && other->mStoreDir == want_dir)
        {
            LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] not started: " << want_dir << " is already being served by the " << ssStrataTenantName((ESSStrataTenant)i) << " tier" << LL_ENDL;
            return;
        }
    }

    // initStore can legitimately run more than once - a mid-session cache clear puts it right back through here - so anything a pass might still be holding from a previous incarnation is invalidated rather than being allowed to write into the store this call is building.
    ++mGeneration;

    mStoreDir = want_dir;
    mDiskCacheVersion = mConfig.mDiskCacheVersion;
    mBudgetBytes = budget_bytes;
    mReadOnly = mConfig.mReadOnly;
    mShuttingDown = false;
    deriveVolumeCap();

    // The enabled() test sits HERE rather than at the top, after the directory is known. A tier the user has
    // switched off still has to be findable: its volumes are left in place and stay readable if it is switched
    // back on, but clearCache has to be able to delete them, and it cannot delete what the store cannot name.
    if (!enabled())
    {
        LL_INFOS("Strata") << "Strata[" << mConfig.mTag << "] volumes are switched off; this cache keeps every object as its own file. Any existing volumes in "
                           << mStoreDir << " are left alone and are not read from" << LL_ENDL;
        return;
    }

    if (!mReadOnly) LLFile::mkdir(mStoreDir);

    if (!loadIndex())
    {
        // A header that does not parse means either a torn first write or a DiskCacheVersion the volumes were not written for. Both say the same thing: this is derived data, the network is authoritative, and starting over costs re-fetches rather than correctness. The loose files in the parent directory are NOT touched - they are the tier that survives this.
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] index at " << indexPath() << " did not parse for disk cache version " << mDiskCacheVersion << "; the volumes are orphaned and will be swept" << LL_ENDL;
        purgeAll();
    }

    sweepOrphanVolumes();

    mInitialized = true;
    sLive[mTenant].store(this, std::memory_order_release);   // published LAST, so no gate can see a store that is still recovering

    LL_INFOS("Strata") << "Strata[" << mConfig.mTag << "] up: " << mLiveRecords.load() << " objects in " << volumeCount() << " volumes, "
                       << (allocatedBytes() / (1024 * 1024)) << " MB allocated against a " << (mBudgetBytes.load() / (1024 * 1024))
                       << " MB budget, volume cap " << (mVolumeCap / (1024 * 1024)) << " MB"
                       << (mReadOnly ? ", READ-ONLY second instance" : "") << LL_ENDL;
}

void SSStrataStore::beginShutdown()
{
    mShuttingDown = true;
}

void SSStrataStore::shutdownStore()
{
    // Unpublished FIRST, so a gate that is already past its own null check finishes against a store that is
    // still fully formed and every gate arriving after this one behaves as though the tier were switched off.
    sLive[mTenant].store(nullptr, std::memory_order_release);
    mShuttingDown = true;
    if (mInitialized)
    {
        LL_INFOS("Strata") << metricsString() << LL_ENDL;
    }
    mInitialized = false;
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

bool SSStrataStore::loadIndex()
{
    const std::string path = indexPath();

    // Every session-scoped counter is reset here rather than assumed clean, because initStore can legitimately run more than once and a record count carried over would place the next append past a slot nobody ever wrote - leaving a hole of zeros that fails its checksum and truncates the whole index on the next startup.
    mCurrentVolume = 0;
    mVolumeEnd = SSSTRATA_VOL_HEADER_SIZE;
    mRecordCursor = 0;
    mLiveRecords = 0;
    mAllocBytes = 0;
    mVolumeHigh = 0;
    mDyingVolume = SSSTRATA_MAX_VOLUMES;
    mVolFrontier.assign((size_t)SSSTRATA_MAX_VOLUMES, (U64)0);
    mFreeVolumes.clear();
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        mIndex.clear();
        mUnstable.clear();
        mPendingTypes.clear();
    }

    LLFILE* f = LLFile::fopen(path, "rb");
    if (!f)
    {
        // No index at all is the normal first-run case, not a failure. Return true so a fresh header is written rather than the directory being wiped for no reason.
        if (!mReadOnly) writeIndexHeader();
        return true;
    }

    U8 header[SSSTRATA_HEADER_SIZE];
    const bool read_header = fread(header, 1, SSSTRATA_HEADER_SIZE, f) == SSSTRATA_HEADER_SIZE;
    if (!read_header || !ssStrataParseIndexHeader(header, mDiskCacheVersion))
    {
        LLFile::close(f);
        return false;
    }

    // One sequential pass. The first record that fails its own checksum ends the index: everything after a torn append is unreachable by construction, and the next append overwrites it.
    std::unordered_map<LLUUID, SSStrataRecord> index;
    U32 count = 0;
    U8 buf[SSSTRATA_RECORD_SIZE];

    while (fread(buf, 1, SSSTRATA_RECORD_SIZE, f) == SSSTRATA_RECORD_SIZE)
    {
        SSStrataRecord rec;
        if (!ssStrataDeserializeRecord(buf, rec))
        {
            ++mMetrics.mRecordsRejected;
            break;
        }

        // A later record for the same uuid always supersedes an earlier one, which is what makes the file append-only rather than rewritten.
        if (rec.isTombstone()) index.erase(rec.mUUID);
        else                   index[rec.mUUID] = rec;

        ++count;
    }
    LLFile::close(f);

    // SECOND PASS, OVER THE SURVIVORS ONLY. Deriving the frontiers inside the loop above would accumulate the ends of superseded and tombstoned records too, so after a kill the next startup would park the write head at the dead volume's old logical end and the first append would re-grow the file that had just been deleted, as a hole. Taken over the final map instead, a fully dead volume derives a frontier of nothing at all, which is what makes id reuse work without an allocator.
    std::vector<U64> volume_end((size_t)SSSTRATA_MAX_VOLUMES, (U64)0);

    for (const auto& entry : index)
    {
        const SSStrataRecord& rec = entry.second;
        const U64 end = rec.blobEnd();
        if (end > volume_end[rec.mVolume]) volume_end[rec.mVolume] = end;
    }

    U64 alloc = 0;
    for (U32 v = 0; v < SSSTRATA_MAX_VOLUMES; ++v)
    {
        if (volume_end[v] == 0) continue;
        mVolFrontier[v] = llmax(volume_end[v], (U64)SSSTRATA_VOL_HEADER_SIZE);
        alloc += mVolFrontier[v];
        mCurrentVolume = v;   // the write head is the highest volume still holding a LIVE record
        mVolumeHigh = v;
    }
    mVolumeEnd = mVolFrontier[mCurrentVolume] ? mVolFrontier[mCurrentVolume] : (U64)SSSTRATA_VOL_HEADER_SIZE;
    mAllocBytes = alloc;

    // The free list is derived, never persisted: every id below the head that no live record points into is reusable, and the orphan sweep is what makes sure any file still sitting on one of them is gone.
    for (U32 v = 0; v < mVolumeHigh; ++v)
    {
        if (mVolFrontier[v] == 0) mFreeVolumes.push_back(v);
    }

    mRecordCursor = count;
    mLiveRecords = (U32)index.size();
    mMetrics.mRecordsLoaded = (U32)index.size();

    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        mIndex.swap(index);
    }
    return true;
}

bool SSStrataStore::writeIndexHeader()
{
    if (mReadOnly) return false;

    std::vector<U8> header;
    ssStrataBuildIndexHeader(mDiskCacheVersion, header);

    LLFILE* f = ssStrataOpenRW(indexPath());
    if (!f)
    {
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] could not create its index at " << indexPath() << " (errno " << errno << "); the tier stays empty this session" << LL_ENDL;
        return false;
    }
    const bool ok = ssStrataWriteAt(f, 0, header.data(), header.size()) && fflush(f) == 0;
    LLFile::close(f);
    return ok;
}

// Deletes any volume file no live record points into. Runs at startup only, where it is also the cleanup for a crash between a kill's index write and its unlink.
void SSStrataStore::sweepOrphanVolumes()
{
    if (mReadOnly || mStoreDir.empty()) return;

    std::vector<U32> orphans;
    U64 on_disk = 0;
    U64 derived = 0;

    {
        LLDirIterator iter(mStoreDir, "strata_*.dat");
        std::string name;
        while (iter.next(name))
        {
            // ROUND-TRIP THE NAME. Parse the id out, rebuild the canonical filename from it and require an exact string match, so anything unexpected is skipped rather than guessed at.
            if (name.compare(0, 7, "strata_") != 0) continue;

            const size_t dot = name.find('.', 7);
            if (dot == std::string::npos || name.compare(dot, std::string::npos, ".dat") != 0) continue;

            const std::string digits = name.substr(7, dot - 7);
            if (digits.empty() || digits.size() > 5) continue;
            if (digits.find_first_not_of("0123456789") != std::string::npos) continue;

            const U32 id = (U32)strtoul(digits.c_str(), NULL, 10);
            if (id >= SSSTRATA_MAX_VOLUMES) continue;
            if (llformat("strata_%03u.dat", id) != name) continue;

            const std::string path = gDirUtilp->add(mStoreDir, name);
            if (LLFile::isdir(path)) continue;

            llstat st;
            const bool statted = LLFile::stat(path, &st, ENOENT) == 0;

            if (mVolFrontier[id] == 0 && id != mCurrentVolume)
            {
                orphans.push_back(id);
                continue;
            }

            if (statted) on_disk += (U64)st.st_size;
            derived += mVolFrontier[id];
        }
    }

    for (U32 id : orphans)
    {
        if (killVolumeFile(id, mGeneration.load()))
        {
            ++mMetrics.mOrphansSwept;
            LL_INFOS("Strata") << "Strata[" << mConfig.mTag << "] swept orphan volume " << id << ", which no live record pointed into" << LL_ENDL;
        }
        else
        {
            ++mMetrics.mUnlinkFailures;
            LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] could not delete orphan volume " << id << " (errno " << errno << "), leaving it for the next startup" << LL_ENDL;
        }
    }

    // The accounting is DERIVED, so it is worth saying out loud when it disagrees with the disk rather than quietly trusting it. Drift is expected and small - torn tails past the last valid index record are exactly this - but a large number here means the frontier arithmetic is wrong.
    if (on_disk != derived)
    {
        LL_INFOS("Strata") << "Strata[" << mConfig.mTag << "] accounting drift: volumes hold " << (on_disk / 1024) << " KB on disk against "
                           << (derived / 1024) << " KB derived from the index; the difference is torn tails past the last valid record" << LL_ENDL;
    }
}

// ---------------------------------------------------------------------------
// Volumes
// ---------------------------------------------------------------------------

bool SSStrataStore::ensureVolume(U32 volume)
{
    // Caller holds mStoreMutex.
    const std::string path = volumePath(volume);
    const bool exists = gDirUtilp->fileExists(path);

    // A reused id whose file survived a failed unlink is NOT empty. The frontier is the store's own opinion of how much of this volume is real, so anything at or below the header offset is treated as fresh and gets its header rewritten; stale blobs past the new write point remain, and are harmless only because every read checks the blob's uuid against the one it asked for.
    if (exists && mVolFrontier[volume] > (U64)SSSTRATA_VOL_HEADER_SIZE) return true;

    // The directory can vanish under a running session - a mid-session cache clear deletes it - and the next pack pass has to find a home rather than silently failing every append for the rest of the session.
    LLFile::mkdir(mStoreDir);

    std::vector<U8> header;
    ssStrataBuildVolumeHeader(volume, header);

    LLFILE* f = ssStrataOpenRW(path);
    if (!f) return false;
    const bool ok = ssStrataWriteAt(f, 0, header.data(), header.size());
    LLFile::close(f);

    // A volume that exists costs its header even before the first blob lands, and the budget is enforced against what exists.
    if (ok && mVolFrontier[volume] < (U64)SSSTRATA_VOL_HEADER_SIZE)
    {
        mAllocBytes += (U64)SSSTRATA_VOL_HEADER_SIZE - mVolFrontier[volume];
        mVolFrontier[volume] = SSSTRATA_VOL_HEADER_SIZE;
    }
    return ok;
}

// Caller holds mStoreMutex. Hands out the lowest reclaimed id before growing the head, because SSSTRATA_MAX_VOLUMES is consumed at the rate the tier is WRITTEN rather than the rate it grows.
bool SSStrataStore::allocateVolume(U32& out_volume)
{
    while (!mFreeVolumes.empty())
    {
        const U32 id = mFreeVolumes.front();
        mFreeVolumes.erase(mFreeVolumes.begin());
        if (id == mDyingVolume) continue;   // a kill is still working through this one, so handing it out now would let a fresh blob land in a file about to be unlinked
        if (mVolFrontier[id] != 0) continue;
        out_volume = id;
        return true;
    }

    if (mVolumeHigh + 1 >= SSSTRATA_MAX_VOLUMES) return false;
    out_volume = ++mVolumeHigh;
    return true;
}

bool SSStrataStore::appendBlob(const std::vector<U8>& blob, U16& out_volume, U64& out_offset)
{
    // Caller holds mStoreMutex. Every blob starts on a SSSTRATA_ALIGN boundary so a future mapped or direct-IO reader never has to care about alignment.
    U64 offset = (mVolumeEnd + (SSSTRATA_ALIGN - 1)) & ~(U64)(SSSTRATA_ALIGN - 1);

    // The runtime cap, not the format ceiling. See the note beside SSSTRATA_VOLUME_CAP_MIN for why the two must stay different numbers.
    if (offset + blob.size() > mVolumeCap)
    {
        U32 next = 0;
        if (!allocateVolume(next))
        {
            LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] is out of volume ids, stopping packing for this session" << LL_ENDL;
            return false;
        }
        mCurrentVolume = next;
        mVolumeEnd = SSSTRATA_VOL_HEADER_SIZE;
        offset = SSSTRATA_VOL_HEADER_SIZE;
    }

    if (!ensureVolume(mCurrentVolume)) return false;

    LLFILE* f = ssStrataOpenRW(volumePath(mCurrentVolume));
    if (!f) return false;

    // Handles are opened and closed per pass rather than held: an open handle defeats the directory-level wipe that clearCache relies on, and one open plus one close per pack batch is nothing next to the batch itself.
    const bool ok = ssStrataWriteAt(f, offset, blob.data(), blob.size());
    LLFile::close(f);
    if (!ok) return false;

    mVolumeEnd = offset + blob.size();
    if (mVolumeEnd > mVolFrontier[mCurrentVolume])
    {
        mAllocBytes += mVolumeEnd - mVolFrontier[mCurrentVolume];
        mVolFrontier[mCurrentVolume] = mVolumeEnd;
    }
    out_volume = (U16)mCurrentVolume;
    out_offset = offset;
    return true;
}

// One open, one write, one flush for the whole batch. A per-record open would turn a 64 byte durability point into thousands of them, and one pack pass is thousands of records.
bool SSStrataStore::appendIndexRecords(const std::vector<SSStrataRecord>& recs)
{
    // Caller holds mStoreMutex.
    if (recs.empty()) return true;
    if (mReadOnly) return false;

    std::vector<U8> buf;
    buf.resize(recs.size() * SSSTRATA_RECORD_SIZE);
    for (size_t i = 0; i < recs.size(); ++i)
    {
        ssStrataSerializeRecord(recs[i], buf.data() + i * SSSTRATA_RECORD_SIZE);
    }

    const U64 offset = (U64)SSSTRATA_HEADER_SIZE + (U64)mRecordCursor.load() * SSSTRATA_RECORD_SIZE;

    LLFILE* f = ssStrataOpenRW(indexPath());
    if (!f) return false;
    const bool ok = ssStrataWriteAt(f, offset, buf.data(), buf.size()) && fflush(f) == 0;
    LLFile::close(f);
    if (!ok) return false;

    mRecordCursor += (U32)recs.size();
    return true;
}

// The ONLY delete of a volume in the whole tier. The generation is re-checked HERE, under the lock, and the unlink happens while it is still held: checking it only afterwards leaves a window in which a pass descheduled just before the unlink wakes up after a purgeAll has recreated the directory and unlinks a file the fresh index is already using.
bool SSStrataStore::killVolumeFile(U32 volume, U32 expect_generation)
{
    std::lock_guard<std::mutex> lock(mStoreMutex);

    if (mGeneration.load() != expect_generation)
    {
        LL_INFOS("Strata") << "Strata[" << mConfig.mTag << "] abandoned the unlink of volume " << volume << ": the store was purged and recreated underneath it" << LL_ENDL;
        return false;
    }

    if (mReadOnly) return false;
    if (mStoreDir.empty()) return false;
    if (volume >= SSSTRATA_MAX_VOLUMES) return false;

    // The last component of the store directory must be the store's own name. This is the guard that stands between a mis-set cache path and deleting somebody's documents.
    const std::string& delim = gDirUtilp->getDirDelimiter();
    const size_t cut = mStoreDir.find_last_of(delim);
    const std::string leaf = (cut == std::string::npos) ? mStoreDir : mStoreDir.substr(cut + 1);
    if (leaf != std::string(SSSTRATA_DIR_NAME))
    {
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] refused to delete inside " << mStoreDir << ": that is not a " << SSSTRATA_DIR_NAME << " directory" << LL_ENDL;
        return false;
    }

    const std::string path = volumePath(volume);
    if (LLFile::isdir(path)) return false;   // a directory wearing a volume's name is not ours to remove
    if (!gDirUtilp->fileExists(path)) return true;   // already gone counts as reclaimed

    return LLFile::remove(path, ENOENT) == 0;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

bool SSStrataStore::readPayload(const SSStrataRecord& rec, S32 offset, U8* dst, S32 bytes, S32& out_read)
{
    out_read = 0;
    if (offset < 0 || bytes <= 0 || !dst) return false;
    if ((U64)offset >= (U64)rec.mSize) return false;   // a read starting at or past the end is eof, which the caller reports as a short read exactly as the loose path does

    const U32 avail = rec.mSize - (U32)offset;
    const U32 want  = (U32)llmin((U64)avail, (U64)(U32)bytes);

    LLFILE* f = LLFile::fopen(volumePath(rec.mVolume), "rb");
    if (!f)
    {
        ++mMetrics.mReadsFailedOpen;
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] could not open volume " << rec.mVolume << " for " << rec.mUUID << " (errno " << errno << "); the record is dropped so the asset is re-fetched" << LL_ENDL;
        return false;
    }

    // The header read is the price of the identity check, and it is the only thing this path costs over the loose file it replaced: one open, two reads, one close against one open, one seek, one read.
    U8 hdr[SSSTRATA_BLOB_HEADER_SIZE];
    bool ok = fseek(f, (long)rec.mOffset, SEEK_SET) == 0 && fread(hdr, 1, SSSTRATA_BLOB_HEADER_SIZE, f) == SSSTRATA_BLOB_HEADER_SIZE;
    if (!ok)
    {
        LLFile::close(f);
        ++mMetrics.mReadsFailedShort;
        return false;
    }

    if (!ssStrataVerifyBlobHeader(hdr, SSSTRATA_BLOB_HEADER_SIZE, rec.mUUID, rec.mSize))
    {
        LLFile::close(f);
        ++mMetrics.mReadsFailedIdentity;
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] found the wrong blob at volume " << rec.mVolume << " offset " << rec.mOffset << " for " << rec.mUUID << "; the record is dropped" << LL_ENDL;
        return false;
    }

    // The header read above left the stream positioned exactly at the payload when offset is 0, which is every whole-object read - a texture body, an asset. Seeking there again is not just a wasted syscall: it discards the stdio buffer the header read just filled, so a small object that was already resident in that buffer is fetched a second time. Only a positional read (offset > 0, which is a mesh LOD) genuinely needs the seek.
    const U64 payload_at = rec.mOffset + (U64)SSSTRATA_BLOB_HEADER_SIZE + (U64)offset;
    ok = (offset == 0 || fseek(f, (long)payload_at, SEEK_SET) == 0) && fread(dst, 1, want, f) == want;
    LLFile::close(f);
    if (!ok)
    {
        ++mMetrics.mReadsFailedShort;
        return false;
    }

    // A read that covers the object end to end can check the payload checksum as well as the identity. A positional read - which is what a mesh LOD fetch is - can only check the identity and the declared size, and that is the same compromise ssBC7VerifyBlobPrefix makes for a coarse discard serve.
    if (offset == 0 && want == rec.mSize && ssserial::crc32Of(dst, want) != rec.mDataCRC)
    {
        ++mMetrics.mReadsFailedCRC;
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] payload checksum failed for " << rec.mUUID << " in volume " << rec.mVolume << "; the record is dropped" << LL_ENDL;
        return false;
    }

    out_read = (S32)want;
    ++mMetrics.mReadsServed;
    mMetrics.mReadBytes += want;
    return true;
}

bool SSStrataStore::readWholePayload(const SSStrataRecord& rec, std::vector<U8>& out)
{
    out.resize(rec.mSize);
    S32 got = 0;
    if (!readPayload(rec, 0, out.data(), (S32)rec.mSize, got)) return false;
    return got == (S32)rec.mSize;
}

bool SSStrataStore::hasObject(const LLUUID& id)
{
    if (!mInitialized) return false;

    std::lock_guard<std::mutex> lock(mMapMutex);
    auto it = mIndex.find(id);
    if (it == mIndex.end()) return false;

    // A hit is the reference signal, and this is the only place in the tree that learns a packed object is still wanted. One store into a record under a lock the call already takes.
    it->second.mLastUseDay  = ssStrataToday();
    it->second.mTouchSecond = ssStrataNowSeconds();
    return true;
}

bool SSStrataStore::objectSize(const LLUUID& id, S32& out_size)
{
    out_size = 0;
    if (!mInitialized) return false;

    std::lock_guard<std::mutex> lock(mMapMutex);
    auto it = mIndex.find(id);
    if (it == mIndex.end()) return false;

    it->second.mLastUseDay  = ssStrataToday();
    it->second.mTouchSecond = ssStrataNowSeconds();
    out_size = (S32)it->second.mSize;
    return true;
}

bool SSStrataStore::readObject(const LLUUID& id, S32 offset, U8* dst, S32 bytes, S32& out_read)
{
    out_read = 0;
    if (!mInitialized) return false;

    SSStrataRecord rec;
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        auto it = mIndex.find(id);
        if (it == mIndex.end()) return false;
        it->second.mLastUseDay  = ssStrataToday();
        it->second.mTouchSecond = ssStrataNowSeconds();
        rec = it->second;
    }

    // END OF OBJECT IS NOT A FAILURE, and separating the two is load-bearing: a caller that seeks to the end and reads gets a short read from a loose file, and if that answer were routed through the failure path below it would tombstone a perfectly good record every time anything probed for eof. True with nothing read is what the loose path's zero-byte fread means, and it also stops the caller falling through to a loose file that is not there.
    if (bytes <= 0 || (U64)offset >= (U64)rec.mSize)
    {
        out_read = 0;
        return true;
    }

    // The IO happens holding NOTHING. Correctness against a volume reclaimed underneath this read comes from the uuid in the blob header, not from a lock - which is the whole reason the header carries one.
    if (readPayload(rec, offset, dst, bytes, out_read)) return true;

    // Any hard read failure drops the record, tombstone and all. Leaving it would make getExists keep promising an asset that can never be read, which is strictly worse than a cache miss: the fetch path handles a miss and has no handling at all for an existence claim that then fails.
    forget(id);
    return false;
}

// ---------------------------------------------------------------------------
// Write gates
// ---------------------------------------------------------------------------

void SSStrataStore::typeBreakdown(std::vector<SSStrataTypeStat>& out) const
{
    out.clear();

    // 256 slots rather than AT_COUNT, because the type byte comes off disk and a corrupted or future record must land in a bucket instead of past the end of one.
    U32 counts[256] = { 0 };
    U64 bytes[256]  = { 0 };

    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (const auto& pair : mIndex)
        {
            const U8 t = pair.second.mAssetType;
            ++counts[t];
            bytes[t] += (U64)pair.second.mSize;
        }
    }

    for (U32 t = 0; t < 256; ++t)
    {
        if (!counts[t]) continue;
        SSStrataTypeStat st;
        st.mType  = (U8)t;
        st.mCount = counts[t];
        st.mBytes = bytes[t];
        out.push_back(st);
    }

    std::sort(out.begin(), out.end(), [](const SSStrataTypeStat& a, const SSStrataTypeStat& b) { return a.mBytes > b.mBytes; });
}

ESSStrataUnpack SSStrataStore::noteUnpack(ESSStrataUnpack v)
{
    ++mMetrics.mUnpackVerdicts[v];
    return v;
}

void SSStrataStore::noteAssetType(const LLUUID& id, LLAssetType::EType type)
{
    if (!mInitialized) return;
    // A single-type tenant already knows the answer, so remembering it once per uuid would be a map of one repeated constant. The packer reads mConfig.mDefaultAssetType instead.
    if (mConfig.mDefaultAssetType != 0xFF) return;
    if (type < 0 || type > 0xFE) return;

    std::lock_guard<std::mutex> lock(mMapMutex);
    if (mPendingTypes.size() < SSSTRATA_UNSTABLE_CAP) mPendingTypes[id] = (U8)type;
}

ESSStrataUnpack SSStrataStore::prepareForWrite(const LLUUID& id, LLAssetType::EType type, const std::string& loose_path, bool restore_bytes)
{
    if (!mInitialized) return SSSTRATA_UNPACK_NOT_PACKED;   // not counted: with the tier off, "this object is not in a volume" is simply true rather than a refusal

    noteAssetType(id, type);

    SSStrataRecord rec;
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        // Revoking the packer's token comes FIRST and happens whether or not the object turns out to be packed, because the case that matters is exactly the one where it is not yet in the index: the packer has read the loose file and is about to publish it, and this is the only signal that will make it drop the record instead.
        mPacking.erase(id);
        auto it = mIndex.find(id);
        if (it == mIndex.end()) return noteUnpack(SSSTRATA_UNPACK_NOT_PACKED);
        rec = it->second;
    }

    // A second instance never writes to the shared store. Dropping the record from ITS OWN map is both necessary and sufficient: the loose file it is about to write becomes authoritative for this process, and the first instance's packed copy is byte-identical for the same uuid so leaving it is harmless.
    if (mReadOnly)
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        if (mIndex.erase(id)) --mLiveRecords;
        return noteUnpack(SSSTRATA_UNPACK_READ_ONLY_FORGOT);
    }

    std::vector<U8> payload;
    if (restore_bytes && !readWholePayload(rec, payload))
    {
        forget(id);
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] could not give " << id << " back to the loose tier; the object is dropped and will be re-fetched" << LL_ENDL;
        return noteUnpack(SSSTRATA_UNPACK_READ_FAILED);
    }

    // REVOKE FIRST. Every crash window in this direction has to fail toward a cache miss, and the only ordering that guarantees that is one where the index stops claiming the object before anything else exists to disagree with it.
    forget(id);
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        U8& strikes = mUnstable[id];
        if (strikes < 0xFF) ++strikes;
        if (mUnstable.size() > SSSTRATA_UNSTABLE_CAP) mUnstable.clear();   // the settle time degrades to the base value rather than growing memory without limit
    }

    ++mMetrics.mUnpacked;
    mMetrics.mUnpackedBytes += rec.mSize;

    if (!restore_bytes) return noteUnpack(SSSTRATA_UNPACK_DROPPED);

    LLFILE* f = LLFile::fopen(loose_path, "wb");
    if (!f)
    {
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] could not recreate the loose file " << loose_path << " for " << id << " (errno " << errno << "); the object is dropped" << LL_ENDL;
        return noteUnpack(SSSTRATA_UNPACK_WRITE_FAILED);
    }
    const bool ok = fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    LLFile::close(f);
    if (!ok)
    {
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] wrote a short loose file for " << id << "; the object is dropped" << LL_ENDL;
        return noteUnpack(SSSTRATA_UNPACK_WRITE_FAILED);
    }

    return noteUnpack(SSSTRATA_UNPACK_DONE);
}

bool SSStrataStore::forget(const LLUUID& id)
{
    if (!mInitialized) return false;

    bool was_live = false;
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        mPacking.erase(id);
        if (mIndex.erase(id))
        {
            was_live = true;
            --mLiveRecords;
        }
    }
    if (!was_live) return false;

    ++mMetrics.mForgotten;
    if (mReadOnly) return true;

    SSStrataRecord tomb;
    tomb.mUUID = id;
    tomb.mFlags = SSSTRATA_FLAG_TOMBSTONE;
    tomb.mLastUseDay = ssStrataToday();

    std::vector<SSStrataRecord> one(1, tomb);
    std::lock_guard<std::mutex> lock(mStoreMutex);
    if (!appendIndexRecords(one))
    {
        // Named and counted rather than swallowed, because it is the one place this design can serve stale bytes: the next session's index still points at the packed copy, which is a valid older snapshot of the same uuid, so the cost is a re-fetch of whatever the writer was about to add.
        ++mMetrics.mTombstoneFailed;
        LL_WARNS("Strata") << "Strata[" << mConfig.mTag << "] could not write the tombstone for " << id << " (errno " << errno << "); the next session will serve the older packed copy until it is superseded" << LL_ENDL;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Wipe
// ---------------------------------------------------------------------------

void SSStrataStore::purgeAll()
{
    if (mStoreDir.empty()) return;
    if (mReadOnly)
    {
        LL_INFOS("Strata") << "Strata[" << mConfig.mTag << "] declined to wipe: this is a read-only second instance" << LL_ENDL;
        return;
    }

    // Bumped BEFORE the lock so any pass already holding a cursor into the old store abandons it at its next generation check rather than writing into the store this call is about to rebuild.
    ++mGeneration;

    // Enumerated from the DIRECTORY rather than from the frontier table. The frontiers are derived from an
    // index this call may never have loaded - a session running with the tier switched off has none at all -
    // and "clear cache" that left four gigabytes of volumes behind would be a lie.
    std::vector<U32> to_kill;
    {
        LLDirIterator iter(mStoreDir, "strata_*.dat");
        std::string name;
        while (iter.next(name))
        {
            const size_t dot = name.find('.', 7);
            if (name.compare(0, 7, "strata_") != 0 || dot == std::string::npos) continue;
            const std::string digits = name.substr(7, dot - 7);
            if (digits.empty() || digits.size() > 5) continue;
            if (digits.find_first_not_of("0123456789") != std::string::npos) continue;
            const U32 id = (U32)strtoul(digits.c_str(), NULL, 10);
            if (id >= SSSTRATA_MAX_VOLUMES) continue;
            if (llformat("strata_%03u.dat", id) != name) continue;
            to_kill.push_back(id);
        }
    }

    const U32 gen = mGeneration.load();
    for (U32 v : to_kill)
    {
        if (!killVolumeFile(v, gen)) ++mMetrics.mUnlinkFailures;
    }

    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        LLFile::remove(indexPath(), ENOENT);
        mVolFrontier.assign((size_t)SSSTRATA_MAX_VOLUMES, (U64)0);
        mFreeVolumes.clear();
        mCurrentVolume = 0;
        mVolumeHigh = 0;
        mDyingVolume = SSSTRATA_MAX_VOLUMES;
        mVolumeEnd = SSSTRATA_VOL_HEADER_SIZE;
        mAllocBytes = 0;
        mRecordCursor = 0;
        mLiveRecords = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        mIndex.clear();
        mUnstable.clear();
        mPendingTypes.clear();
    }

    if (enabled()) writeIndexHeader();
    LL_INFOS("Strata") << "Strata[" << mConfig.mTag << "] wiped " << to_kill.size() << " volumes and its index" << LL_ENDL;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

std::string SSStrataStore::statusString() const
{
    std::ostringstream s;
    if (!mInitialized)
    {
        s << "Strata[" << mConfig.mTag << "] off";
        return s.str();
    }
    s << mConfig.mTag << ": " << mLiveRecords.load() << " objects in " << volumeCount() << " volumes, "
      << (allocatedBytes() / (1024 * 1024)) << " MB";
    if (mReadOnly) s << " (read-only)";
    return s.str();
}

std::string SSStrataStore::metricsString() const
{
    std::ostringstream s;
    s << "Strata[" << mConfig.mTag << "]: " << mLiveRecords.load() << " objects, " << volumeCount() << " volumes, "
      << (allocatedBytes() / (1024 * 1024)) << " MB of " << (mBudgetBytes.load() / (1024 * 1024)) << " MB"
      << " | loaded " << mMetrics.mRecordsLoaded.load() << " rejected " << mMetrics.mRecordsRejected.load()
      << " | packed " << mMetrics.mPacked.load() << " (" << (mMetrics.mPackedBytes.load() / (1024 * 1024)) << " MB) in " << mMetrics.mPackPasses.load() << " passes"
      << " | unpacked " << mMetrics.mUnpacked.load() << " (" << (mMetrics.mUnpackedBytes.load() / (1024 * 1024)) << " MB)"
      << " forgotten " << mMetrics.mForgotten.load() << " tombstone-failed " << mMetrics.mTombstoneFailed.load()
      << " | reads " << mMetrics.mReadsServed.load() << " (" << (mMetrics.mReadBytes.load() / (1024 * 1024)) << " MB)"
      << " open-fail " << mMetrics.mReadsFailedOpen.load() << " short " << mMetrics.mReadsFailedShort.load()
      << " identity " << mMetrics.mReadsFailedIdentity.load() << " crc " << mMetrics.mReadsFailedCRC.load()
      << " | killed " << mMetrics.mVolumesKilled.load() << " volumes (" << (mMetrics.mBytesReclaimed.load() / (1024 * 1024)) << " MB, "
      << mMetrics.mObjectsDropped.load() << " objects, " << mMetrics.mHotObjectsDropped.load() << " of them warm)"
      << " swept " << mMetrics.mOrphansSwept.load() << " unlink-fail " << mMetrics.mUnlinkFailures.load();

    // Every decline is named and counted. A pass that examined forty thousand files and packed none of them must not report the same silence as a pass that examined three, which is the exact failure this project has already paid for once.
    s << " | skip too-big " << mMetrics.mSkipTooBig.load()
      << " too-young " << mMetrics.mSkipTooYoung.load()
      << " unstable " << mMetrics.mSkipUnstable.load()
      << " empty " << mMetrics.mSkipEmpty.load()
      << " unreadable " << mMetrics.mSkipUnreadable.load()
      << " changed " << mMetrics.mSkipChanged.load()
      << " raced " << mMetrics.mSkipRaced.load()
      << " pinned " << mMetrics.mSkipPinned.load()
      << " bad-name " << mMetrics.mSkipBadName.load()
      << " already-packed " << mMetrics.mSkipAlreadyPacked.load()
      << " budget " << mMetrics.mSkipBudget.load();

    s << " | pack verdicts";
    for (U32 i = 0; i < SSSTRATA_PACK_COUNT; ++i)
    {
        const U32 n = mMetrics.mPackVerdicts[i].load();
        if (n) s << " " << ssStrataPackVerdictName((ESSStrataPackVerdict)i) << "=" << n;
    }
    s << " | reclaim verdicts";
    for (U32 i = 0; i < SSSTRATA_RECLAIM_COUNT; ++i)
    {
        const U32 n = mMetrics.mReclaimVerdicts[i].load();
        if (n) s << " " << ssStrataReclaimVerdictName((ESSStrataReclaimVerdict)i) << "=" << n;
    }
    s << " | unpack verdicts";
    for (U32 i = 0; i < SSSTRATA_UNPACK_COUNT; ++i)
    {
        const U32 n = mMetrics.mUnpackVerdicts[i].load();
        if (n) s << " " << ssStrataUnpackName((ESSStrataUnpack)i) << "=" << n;
    }
    return s.str();
}
