/**
 * @file ssstrata.h
 * @brief Strata asset volume store - the little-files fix for LLDiskCache, see doc/strata.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_STRATA_H
#define SS_STRATA_H

#include "llassettype.h"
#include "lluuid.h"

#include <atomic>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// WHAT THIS IS. LLDiskCache stores every general asset the viewer fetches as one file per uuid, which on the machine this was measured on is 57,947 files holding 4,487 MB. Strata folds those into a handful of append-only volume files with one checksummed, fixed-stride index, so the same bytes cost tens of files rather than tens of thousands.
//
// TWO TENANTS, ONE ENGINE, SEPARATE VOLUMES. The J2C texture body cache has exactly the same disease - 15,090 files holding 1,255 MB in `texturecache/[0-f]` on the same machine - and, unlike LLDiskCache, its bodies are written whole and never mutated, so it is if anything an easier tenant than the one this store was built for. It gets its own instance with its own directory, its own index and its own budget rather than sharing one: the two tiers are wiped by different code paths, are governed by different sliders, and share a uuid namespace they must not be allowed to collide in. What they DO share is the engine - one torn-tail scanner, one orphan sweep, one generation interlock, one set of named verdicts - which is the whole reason a second store was not written. See doc/strata.md.
//
// THE SHAPE IS A WRITE-BACK STAGING TIER, NOT A DIRECT CONTAINER, and that is the central design decision. LLDiskCache is not a store of immutable blobs the way the BC7 sidecar is: llmeshrepository.cpp opens the SAME cache object with LLFileSystem::READ_WRITE and patches LOD bytes into it at an offset as each level arrives, minutes or days after the header was written, and llviewerassetstorage.cpp builds an object through a run of APPEND opens. An append-only volume cannot serve that, and adding an intra-volume allocator to make it serve that is exactly what doc/strata.md rejected on measurement. So writes are left EXACTLY where they are - one loose file per uuid, on the fetch thread, with the same syscalls and the same failure modes as today - and a background packer folds objects into volumes once they have stopped changing. Reads consult the in-memory index first and fall through to the loose path when it misses.
//
// THE CRASH RULE, ONE LINE: the index record is committed LAST when packing and REVOKED FIRST when unpacking, so every crash window fails toward a cache miss and never toward two disagreeing copies of one uuid. Packing writes the blob, flushes, appends the index record, flushes, and only then unlinks the loose file - a crash before the index flush leaves the loose file authoritative and some unreferenced bytes past the volume frontier, and a crash after it leaves a byte-identical duplicate that the next pass unlinks. Unpacking tombstones the record before the loose file is written, so a crash in between loses the object and costs one re-fetch. There is no startup reconciliation scan and no window in which a stale volume copy can shadow a newer loose file.
//
// NOTHING IS EVER TRUNCATED, exactly as in the BC7 store: the logical end of a volume is derived from the index, and the next append overwrites whatever partial blob a kill left behind.

// The format ceiling. deserializeRecord rejects any record whose blob ends past this and the index scan stops at the first rejected record, so lowering it would silently discard the tail of an index written by an older build. The cap the packer actually rolls over at is a runtime value derived from the budget - the two must stay different numbers, which is the lesson SSBC7_SEGMENT_CAP_MIN records next door.
constexpr U64 SSSTRATA_VOLUME_CAP        = 512ull * 1024 * 1024;
constexpr U64 SSSTRATA_VOLUME_CAP_MIN    = 16ull * 1024 * 1024;
constexpr U64 SSSTRATA_VOLUME_CAP_MAX    = 256ull * 1024 * 1024;   // the runtime ceiling; reclaim is one unlink, so this is the precision of one kill and not a file-size knob
constexpr U32 SSSTRATA_VOLUME_BUCKETS    = 64;      // how many volumes the budget is divided into; at the shipped 16384 MB budget that lands on 256 MiB volumes, and at the measured 4,487 MB of content on eighteen files instead of 57,947
constexpr U32 SSSTRATA_MAX_VOLUMES       = 4096;    // a bound is what stops a corrupt volume id from being followed anywhere
constexpr U32 SSSTRATA_ALIGN             = 16;      // every blob starts on a sixteen byte boundary so a future mapped reader never has to care

constexpr U32 SSSTRATA_IDX_MAGIC         = 0x41525453;  // 'STRA' little-endian
constexpr U32 SSSTRATA_VOL_MAGIC         = 0x4C4F5653;  // 'SVOL'
constexpr U32 SSSTRATA_BLOB_MAGIC        = 0x424C4253;  // 'SBLB'
constexpr U32 SSSTRATA_FORMAT_VERSION    = 1;           // wipe-never-migrate, exactly as the region cache and the BC7 sidecar do
constexpr U32 SSSTRATA_HEADER_SIZE       = 64;
constexpr U32 SSSTRATA_RECORD_SIZE       = 64;
constexpr U32 SSSTRATA_BLOB_HEADER_SIZE  = 64;
constexpr U32 SSSTRATA_VOL_HEADER_SIZE   = 64;          // a multiple of SSSTRATA_ALIGN, so the first blob lands aligned without a pad

// A guard rather than a policy. The measured distribution has no large-file population at all - the largest object in the whole cache is 6.14 MiB and 99.2% are under 1 MiB - so there is nothing here that a container fails to earn its keep on, and the threshold exists only to bound the packer's per-object buffer and to guarantee an object always fits inside a volume with room to spare. An object over this stays a loose file forever and is counted as such. Each tenant carries its own value in its Config; this is only the default the asset tier ships with.
constexpr U64 SSSTRATA_MAX_OBJECT_BYTES  = 8ull * 1024 * 1024;

// The FORMAT ceiling for one object, held separate from the per-tenant policy above for the same reason SSSTRATA_VOLUME_CAP is held separate from the runtime cap: deserializeRecord validates against this, so lowering it would silently discard the tail of an index written by a build that allowed more. It was raised from 8 to 16 MiB when the texture tier arrived, because the largest measured J2C body on the owner's disk is 10,160,524 bytes and a ceiling that excluded it would have left the biggest files - the ones a container helps least with but a user notices most - loose forever. Raising a ceiling can only ever accept records an older build already accepted, so no existing index is invalidated by it.
constexpr U64 SSSTRATA_MAX_OBJECT_CEILING = 16ull * 1024 * 1024;

// The J2C texture tenant's own object limit. Sized off the measured maximum with headroom rather than set to the ceiling, so the packer's per-object buffer stays bounded by a number this file can point at a measurement for.
constexpr U64 SSSTRATA_TEX_MAX_OBJECT_BYTES = 12ull * 1024 * 1024;

constexpr U32 SSSTRATA_PACK_AGE_SECS     = 300;     // how long an object must have sat unmodified before the packer will touch it, which is the only thing that keeps a mesh being filled in LOD by LOD out of the volumes while it is still moving
constexpr U32 SSSTRATA_UNSTABLE_SHIFT_MAX = 3;      // an object unpacked by a write gets its settle time doubled, up to eight times the base, so a repeatedly patched mesh backs itself off rather than being packed and unpacked once per LOD
constexpr U32 SSSTRATA_UNSTABLE_CAP      = 65536;   // bound on the unstable set; past this the adaptive settle time degrades to the base value rather than growing memory without limit
constexpr U64 SSSTRATA_PACK_BYTES_TICK   = 64ull * 1024 * 1024;    // steady-state work per minute, which is about 1 MB/s of background IO
constexpr U64 SSSTRATA_PACK_BYTES_CATCHUP = 512ull * 1024 * 1024;  // used while the loose tier is still the whole cache, so a first run absorbs the measured 4,487 MB in roughly nine minutes instead of seventy
constexpr U32 SSSTRATA_PACK_FILES_TICK   = 4096;    // a second bound, because a tick of tiny objects is bounded by syscalls rather than by bytes
constexpr U32 SSSTRATA_CATCHUP_FILES     = 4096;    // above this many loose files the tier is judged to be catching up rather than in steady state

// How many entries one LLTextureCache maintenance tick copies out of mTexturesSizeMap. It MUST be larger than SSSTRATA_CATCHUP_FILES or the texture tier can never present enough candidates in one pass to be recognised as catching up, and a cold cache would absorb at the 64 MB steady-state rate - twenty minutes for the measured 1,255 MB instead of four. It is also the bound on how long mHeaderMutex is held, which is why the paths and the stats are built outside the lock rather than inside it.
constexpr U32 SSSTRATA_TEX_SCAN_TICK     = 8192;

constexpr U32 SSSTRATA_RECLAIM_HOT_SECS    = 300;   // a hard recency floor independent of mLastUseDay's one-day resolution
constexpr U32 SSSTRATA_RECLAIM_HOT_PERCENT = 25;    // a volume with more than this fraction of hot bytes is excluded unless nothing else is available
constexpr U32 SSSTRATA_RECLAIM_COOLDOWN_SECS = 60;    // a moving watermark must not be able to cause back-to-back kills

constexpr const char* SSSTRATA_DIR_NAME  = "strata";      // a child of the tier's own cache directory, so an older viewer sharing the same cache never sees it and a directory-level wipe takes it along. BOTH tenants use this same leaf name on purpose: it is what killVolumeFile checks the last path component against before it unlinks anything, and one name means one guard rather than two that can drift apart
constexpr const char* SSSTRATA_IDX_NAME  = "strata.idx";

// WHICH CACHE a store instance is serving. Volumes, index, budget and wipe lifetime are per tenant and nothing crosses between them - the only thing shared is the code. Assets is index 0 so that every existing call site, which named no tenant at all, keeps meaning exactly what it meant.
enum ESSStrataTenant
{
    SSSTRATA_TENANT_ASSETS = 0,     // LLDiskCache: meshes, sounds, animations, everything LLFileSystem stores
    SSSTRATA_TENANT_TEXTURES,       // LLTextureCache J2C bodies: the `texturecache/[0-f]/<uuid>.texture` files
    SSSTRATA_TENANT_COUNT
};

const char* ssStrataTenantName(ESSStrataTenant tenant);

enum ESSStrataFlags : U16
{
    SSSTRATA_FLAG_TOMBSTONE = 1 << 0,   // this uuid is deleted; a later record always supersedes an earlier one
};

// Why a pack pass ended the way it did. A bare "return false" makes "packing is switched off" and "packing ran and found nothing ready" produce the same silence, which is the exact failure this project has already paid for once, so every exit carries a name that is counted and logged.
enum ESSStrataPackVerdict
{
    SSSTRATA_PACK_RAN = 0,              // at least one object moved into a volume
    SSSTRATA_PACK_NOTHING_READY,        // candidates existed but every one of them declined; the per-reason counters say which
    SSSTRATA_PACK_NO_CANDIDATES,        // the loose tier is empty, which is the goal state
    SSSTRATA_PACK_DISABLED,
    SSSTRATA_PACK_READ_ONLY,            // a second instance never packs, exactly as it never evicts
    SSSTRATA_PACK_NOT_READY,            // the store never came up, or shutdown has begun
    SSSTRATA_PACK_STALE,                // initStore or purgeAll moved underneath the pass
    SSSTRATA_PACK_ALREADY_RUNNING,
    SSSTRATA_PACK_OVER_BUDGET,          // the volumes already fill the slider, so reclaim has to run before anything else is admitted
    SSSTRATA_PACK_NO_VOLUME,            // out of volume ids for this session
    SSSTRATA_PACK_VOLUME_WRITE_FAILED,  // nothing was unlinked, so the loose files are still authoritative
    SSSTRATA_PACK_INDEX_WRITE_FAILED,   // likewise; the blobs written this pass are unreferenced bytes the next pass overwrites
    SSSTRATA_PACK_COUNT
};

// Why a reclaim pass ended the way it did. Modelled on ESSBC7EvictVerdict for the same reason.
enum ESSStrataReclaimVerdict
{
    SSSTRATA_RECLAIM_RAN = 0,
    SSSTRATA_RECLAIM_NOT_NEEDED,        // the caller's watermark is satisfied without touching a volume
    SSSTRATA_RECLAIM_DISABLED,
    SSSTRATA_RECLAIM_READ_ONLY,
    SSSTRATA_RECLAIM_NOT_READY,
    SSSTRATA_RECLAIM_STALE,
    SSSTRATA_RECLAIM_ALREADY_RUNNING,
    SSSTRATA_RECLAIM_COOLDOWN,
    SSSTRATA_RECLAIM_NO_CANDIDATE,      // nothing but the write head exists, so there is no sealed volume to reclaim
    SSSTRATA_RECLAIM_TOO_YOUNG,         // the coldest volume is still newer than the loose files the caller would otherwise drop, so the caller should drop those instead
    SSSTRATA_RECLAIM_ALL_HOT,           // every sealed volume is over the hot bar; the tier sits above the watermark and says so rather than dropping what the user is looking at
    SSSTRATA_RECLAIM_INDEX_WRITE_FAILED,// nothing on disk changed
    SSSTRATA_RECLAIM_UNLINK_FAILED,     // the records are dead and the file is an orphan for the startup sweep - NEVER rolled back, because un-killing records is where this wedges
    SSSTRATA_RECLAIM_COUNT
};

// Why a write gate refused to hand an object back to the loose tier, or why it did not have to. Named for the same reason: "the object was not packed" and "the object was packed and we could not free it" must never look alike from a log.
enum ESSStrataUnpack
{
    SSSTRATA_UNPACK_NOT_PACKED = 0,     // the overwhelmingly common answer, and it costs one hash lookup
    SSSTRATA_UNPACK_DONE,               // the record is revoked and, if the caller asked for them, the old bytes are back in the loose file
    SSSTRATA_UNPACK_DROPPED,            // the record is revoked and the old bytes were NOT restored, because the caller is about to truncate anyway
    SSSTRATA_UNPACK_DISABLED,
    SSSTRATA_UNPACK_READ_ONLY_FORGOT,   // a second instance revoked its in-memory record only; the disk is untouched, which is correct because the first instance's copy is byte-identical
    SSSTRATA_UNPACK_READ_FAILED,        // the volume would not give the bytes back, so the record is revoked and the object is a cache miss
    SSSTRATA_UNPACK_WRITE_FAILED,       // likewise, and for the same reason the record is still revoked
    SSSTRATA_UNPACK_COUNT
};

const char* ssStrataPackVerdictName(ESSStrataPackVerdict verdict);
const char* ssStrataReclaimVerdictName(ESSStrataReclaimVerdict verdict);
const char* ssStrataUnpackName(ESSStrataUnpack verdict);

// One 64 byte index record. Fixed width so the index is a flat array and record i lives at SSSTRATA_HEADER_SIZE + i * SSSTRATA_RECORD_SIZE.
struct SSStrataRecord
{
    SSStrataRecord();

    LLUUID  mUUID;
    U64     mOffset;        // of the BLOB HEADER within the volume, always a multiple of SSSTRATA_ALIGN
    U32     mSize;          // payload bytes, excluding the blob header and the alignment pad
    U32     mDataCRC;       // over the payload only
    U16     mVolume;
    U16     mLastUseDay;    // days since the epoch - a drop-first priority signal, never an expiry
    U16     mFlags;         // ESSStrataFlags
    U8      mAssetType;     // LLAssetType::EType as written, or 0xFF when the packer never saw a write for this uuid this session - Firestorm's metaDataToFilepath drops the type from the filename, so an object packed from a previous session's file genuinely has no type to record
    U8      mReserved;
    U32     mRecordCRC;

    // IN MEMORY ONLY, never serialised, and the reason is the same one written down for SSBC7Record: mLastUseDay is a U16 of days, so on a cache filled in one session every record shares a single value and a day-only sort is arbitrary order. Losing these to a crash costs ordering quality and nothing else, which is the whole latitude a drop-priority signal has that a TTL would not.
    U32     mTouchSecond;

    bool isTombstone() const { return (mFlags & SSSTRATA_FLAG_TOMBSTONE) != 0; }
    U64 blobEnd() const { return mOffset + (U64)SSSTRATA_BLOB_HEADER_SIZE + (U64)mSize; }
};

// The 64 byte header in front of every payload. Self-describing on purpose: a reclaimed volume id can be handed out again, so a stale offset lands on a blob that is perfectly well formed and the ONLY thing distinguishing it from the blob the caller asked for is its uuid - which is why the uuid is checked first and unconditionally on every read, including partial ones. It is also what would let the index be rebuilt from the volumes alone.
struct SSStrataBlobHeader
{
    SSStrataBlobHeader();

    LLUUID  mUUID;
    U32     mVersion;
    U32     mSize;
    U32     mDataCRC;
    U16     mFlags;
    U8      mAssetType;
    U8      mReserved;
};

// ---------------------------------------------------------------------------
// Pure format helpers - no filesystem, no globals, so the offline harness links these alone
// ---------------------------------------------------------------------------

void ssStrataSerializeRecord(const SSStrataRecord& rec, U8* out);
bool ssStrataDeserializeRecord(const U8* in, SSStrataRecord& out);

void ssStrataBuildIndexHeader(U32 disk_cache_version, std::vector<U8>& out);
bool ssStrataParseIndexHeader(const U8* data, U32 disk_cache_version);
void ssStrataBuildVolumeHeader(U32 volume, std::vector<U8>& out);
bool ssStrataParseVolumeHeader(const U8* data, U32 expect_volume);

// Builds blob header plus payload into one buffer, and fills in the record fields the format owns. Offset, volume and the record checksum are the IO half's business and are left alone here.
bool ssStrataBuildBlob(const LLUUID& id, const U8* payload, U32 size, U8 asset_type, std::vector<U8>& out_blob, SSStrataRecord& out_record);
bool ssStrataParseBlobHeader(const U8* data, size_t size, SSStrataBlobHeader& out);

// Structural check for a blob whose payload may be only partly present, which is what a positional read of a mesh LOD is. The expected uuid is a REQUIRED parameter for the reason given above, and making it a parameter is what stops a future fast path from skipping the check by accident.
bool ssStrataVerifyBlobHeader(const U8* data, size_t size, const LLUUID& expect_uuid, U32 expect_size);

// Full check including the payload checksum. Only possible when the whole payload is in hand, which is every pack round-trip and every read that covers the object end to end.
bool ssStrataVerifyBlob(const U8* data, size_t size, const LLUUID& expect_uuid);

// Days since the unix epoch, and wall seconds. Both are wall clock on purpose: a clock that jumps costs eviction ordering and nothing else.
U16 ssStrataToday();
U32 ssStrataNowSeconds();

// ---------------------------------------------------------------------------
// The on-disk store
// ---------------------------------------------------------------------------

// One loose file as the disk cache purge already sees it. Passed in rather than re-walked because purge scans the whole cache directory once a minute anyway, and a second walk of the same tens of thousands of entries is the one cost this design must not add.
struct SSStrataLooseFile
{
    SSStrataLooseFile() : mTime(0), mSize(0), mPacked(false), mPinned(false) {}
    SSStrataLooseFile(std::time_t t, U64 sz, const std::string& p, const LLUUID& id, bool pinned) : mTime(t), mSize(sz), mPath(p), mUUID(id), mPacked(false), mPinned(pinned) {}

    std::time_t mTime;
    U64         mSize;
    std::string mPath;
    LLUUID      mUUID;      // parsed by the caller, because the filename convention belongs to LLDiskCache::metaDataToFilepath and duplicating it here is how the two would eventually disagree
    bool        mPacked;    // set by packPass when the object moved into a volume and the loose file is gone, so the caller stops counting or deleting it
    bool        mPinned;    // one of Firestorm's static assets, which prepopulateCacheWithStatic re-copies on every startup and the purge refuses to delete. Packing one would have the copy recreate the loose file every session and the packer unlink it again every session, so it is simply left alone
};

// One asset class's share of a tier, for the overlay. Emitted rather than accumulated live because a reclaim drops a whole volume's worth of records at once, and running totals that have to be unwound by an eviction are how a counter ends up disagreeing with the thing it counts.
struct SSStrataTypeStat
{
    SSStrataTypeStat() : mType(0xFF), mCount(0), mBytes(0) {}
    U8  mType;      // LLAssetType::EType, or 0xFF for records whose type this tier never saw - see SSStrataRecord::mAssetType
    U32 mCount;
    U64 mBytes;
};

// TWO LOCKS, NEVER NESTED THE OTHER WAY, copied deliberately from SSBC7Store because the reasoning is identical: mMapMutex guards the in-memory index only and is held for microseconds, mStoreMutex guards every byte of file IO. A multi-megabyte pack batch must never share a mutex with the lookup path or the fetch threads stall behind a disk write. Reads take mMapMutex to resolve a record and then do their IO holding NOTHING - correctness against a volume reclaimed underneath them comes from the uuid in the blob header, not from a lock.
class SSStrataStore
{
public:
    SSStrataStore();
    ~SSStrataStore();

    // Everything the tier needs from the viewer's settings, pushed down rather than pulled up. llfilesystem sits below newview and has no gSavedSettings, and passing a struct is what keeps this store free of the viewer globals that would stop the offline harness linking it.
    struct Config
    {
        bool mEnabled{true};
        bool mReadOnly{false};      // a second instance shares the cache directory and is a read-only snapshot, exactly the posture LLTextureCache, LLVOCache and the BC7 sidecar take
        bool mVerbose{false};
        U32  mPackAgeSecs{SSSTRATA_PACK_AGE_SECS};
        U64  mMaxObjectBytes{SSSTRATA_MAX_OBJECT_BYTES};
        U32  mDiskCacheVersion{0};  // the cache-format stamp of whichever tier owns this instance - DiskCacheVersion for the asset tier, a digest of the texture header version and the J2C encoder string for the texture tier. It is written into the index header and a mismatch orphans every volume, which is the established precedent here for derived data the network is authoritative for
        const char* mTag{"assets"}; // what this tenant calls itself in a log line, because two tiers writing "Strata packed 400 objects" with no way to tell which is a diagnosis lost
        U8   mDefaultAssetType{0xFF};   // the type every object in this tenant has, when the tenant has only one. The J2C body tier is entirely AT_TEXTURE, so recording that per uuid would be up to 1.5 MB of resident map holding one repeated constant; setting it here instead makes noteAssetType a no-op there and the record still carries a real type. 0xFF means "the tenant is mixed, ask per object", which is what LLDiskCache is
    };

    // ONE INSTANCE PER TENANT, created on first use and never destroyed. Deliberately not LLSingleton any
    // more, and the reason is the one already written down for live(): LLSingleton::instanceExists takes a
    // mutex on every call, and a fetch thread reaching instance() during shutdown could resurrect a singleton
    // LLSingleton had already deleted. A store that is never destroyed cannot be resurrected, and teardown is
    // explicit through shutdownStore() rather than implicit through a destructor ordering nobody controls.
    static SSStrataStore& tier(ESSStrataTenant tenant);

    // Compatibility spellings for the asset tier, which is what every call site that names no tenant means.
    static SSStrataStore& instance() { return tier(SSSTRATA_TENANT_ASSETS); }
    static bool instanceExists() { return true; }

    void configure(const Config& cfg);
    bool enabled() const { return mConfig.mEnabled; }
    const Config& config() const { return mConfig; }
    ESSStrataTenant tenant() const { return mTenant; }

    // THE HOT-PATH ACCESSOR, and every gate in LLFileSystem and LLTextureCache goes through this rather than
    // through tier(). Null means "no store", which is exactly what an unconfigured, disabled or torn-down tier
    // should look like: the caller then behaves precisely as the viewer did before Strata existed, at the cost
    // of one relaxed atomic load. The default argument is what lets every pre-tenant call site keep compiling
    // and keep meaning the asset tier.
    static SSStrataStore* live(ESSStrataTenant tenant = SSSTRATA_TENANT_ASSETS) { return sLive[tenant].load(std::memory_order_acquire); }

    // `cache_dir` is LLDiskCache's own directory, passed in rather than recomputed so the two can never disagree about where the volumes live. `disk_cache_version` is the viewer's DiskCacheVersion and is stamped into the index header: a bump orphans every volume and the sweep unlinks them, which is the established precedent in this codebase for derived data that the network is authoritative for.
    void initStore(const std::string& cache_dir, U64 budget_bytes);
    void beginShutdown();
    void shutdownStore();

    bool isInitialized() const { return mInitialized; }
    bool isReadOnly() const { return mReadOnly; }

    // ---- the read gates, any thread ----

    // NON-CONST because a hit is the reference signal, and this is the only place in the tree that learns a packed object is still wanted. It costs one store into a record under the map lock the call already takes - no new lock, no IO, no queued work - and it is what stops the reclaim scorer from being plain pack-order FIFO.
    bool hasObject(const LLUUID& id);
    bool objectSize(const LLUUID& id, S32& out_size);

    // Positional read straight out of the volume: one open, one seek, one read, one close, which is exactly what the loose path costs today. A read that covers the whole payload verifies the payload checksum as well as the uuid; a partial read - which is what a mesh LOD fetch is - can only verify the uuid and the declared size, and that is the same compromise ssBC7VerifyBlobPrefix makes for a coarse discard serve.
    bool readObject(const LLUUID& id, S32 offset, U8* dst, S32 bytes, S32& out_read);

    // ---- the write gates, any thread ----

    // Called before LLFileSystem writes a loose file. If the object is packed the record is revoked FIRST and, when `restore_bytes` is set, the old payload is written back out to `loose_path` so that an APPEND or a positional READ_WRITE sees the object it expects. A WRITE-mode open is about to truncate, so it passes false and the bytes are simply dropped.
    ESSStrataUnpack prepareForWrite(const LLUUID& id, LLAssetType::EType type, const std::string& loose_path, bool restore_bytes);

    // Called from the remove and rename paths. Revokes the record; the caller still unlinks or renames the loose file as it always did.
    bool forget(const LLUUID& id);

    // Remembers the asset type of a uuid written this session, purely so a packed record can carry one. Firestorm's metaDataToFilepath writes every object as `_0.asset` regardless of type, so an object inherited from a previous session has no type to recover and records 0xFF.
    void noteAssetType(const LLUUID& id, LLAssetType::EType type);

    // ---- maintenance, LLPurgeDiskCacheThread only ----

    // Folds ready loose files into volumes. `files` is the purge scan's own output and entries this pass consumed are marked mPacked, so the caller neither counts them toward the loose total nor tries to delete them.
    ESSStrataPackVerdict packPass(std::vector<SSStrataLooseFile>& files);

    // Kills the coldest sealed volume, but only if its coldest content is at least as old as `max_day`. The caller passes the day of the oldest loose file it would otherwise delete, so the two tiers are drained oldest-first without either of them having to know how the other stores a timestamp; 0xFFFF means there is no loose competition left.
    //
    // `out_dropped`, when supplied, receives every uuid that died with the volume. LLDiskCache does not want it - a dropped asset is simply a cache miss there, because the loose file IS the record. LLTextureCache does: its entry table separately claims a body size for every one of these uuids, and an entry left claiming a body that no longer exists serves 600 bytes and calls it a texture. Handing the list back is what lets the caller drop those entries in the same pass rather than waiting for the one-in-256 startup validation to notice.
    ESSStrataReclaimVerdict reclaimColdest(U16 max_day, bool respect_cooldown, U64& out_freed, std::vector<LLUUID>* out_dropped = nullptr);

    // Hooked into every cache-clear path. Strata is a complete second copy of everything the user has fetched, so it has to die with the rest of the cache or "clear cache" is a lie.
    void purgeAll();

    U64 allocatedBytes() const;     // what the tier actually costs on disk, which is what a budget is enforced against - the sum of live object sizes is not, because it drops the instant a record is tombstoned while the disk does not
    U64 budgetBytes() const { return mBudgetBytes.load(); }
    void setBudgetBytes(U64 bytes);
    U64 volumeCap() const { return mVolumeCap; }
    U32 generation() const { return mGeneration.load(); }
    U32 objectCount() const { return mLiveRecords.load(); }
    // Walks the index once and returns the live records grouped by asset class, largest share first. Deliberately NOT maintained as a running total: see SSStrataTypeStat. The caller is expected to cache the result rather than ask per frame, because this is O(records) under mMapMutex and the fetch threads resolve every read through that lock.
    void typeBreakdown(std::vector<SSStrataTypeStat>& out) const;
    U32 volumeCount() const;
    std::string storeDir() const { return mStoreDir; }
    std::string indexPath() const;
    std::string volumePath(U32 volume) const;

    struct Metrics
    {
        Metrics()
        {
            for (U32 i = 0; i < SSSTRATA_PACK_COUNT; ++i)    mPackVerdicts[i] = 0;
            for (U32 i = 0; i < SSSTRATA_RECLAIM_COUNT; ++i) mReclaimVerdicts[i] = 0;
            for (U32 i = 0; i < SSSTRATA_UNPACK_COUNT; ++i)  mUnpackVerdicts[i] = 0;
        }

        std::atomic<U32> mRecordsLoaded{0};
        std::atomic<U32> mRecordsRejected{0};   // index entries dropped at startup because their checksum or their geometry did not survive the last session
        std::atomic<U32> mPacked{0};
        std::atomic<U64> mPackedBytes{0};
        std::atomic<U32> mPackPasses{0};
        std::atomic<U32> mUnpacked{0};
        std::atomic<U64> mUnpackedBytes{0};
        std::atomic<U32> mForgotten{0};
        std::atomic<U32> mTombstoneFailed{0};   // the in-memory record went but the tombstone did not reach the disk, so the next session will serve the older packed copy until it is overwritten - counted rather than swallowed, because it is the one place this design can serve stale bytes
        std::atomic<U32> mReadsServed{0};
        std::atomic<U64> mReadBytes{0};
        std::atomic<U32> mReadsFailedOpen{0};
        std::atomic<U32> mReadsFailedShort{0};
        std::atomic<U32> mReadsFailedIdentity{0};   // the blob was well formed and belonged to a different uuid, which is precisely the stale-offset case the header uuid exists to catch
        std::atomic<U32> mReadsFailedCRC{0};
        // <SS:Nexii> Published by the packer's caller because only it has just scanned the directory. This is the number a user sees in Explorer and the one that made the feature look broken during a cold-cache burst, so it belongs somewhere the viewer can show it rather than only in a log line.
        std::atomic<U32> mLooseFiles{0};
        std::atomic<U64> mLooseBytes{0};

        // <SS:Nexii> The maintenance pass had no stopwatch, which hid how expensive it is on a large cache: the pass returns early whenever the cache is under its high watermark - the common case - so the timing that already existed was never reached.
        //
        // An earlier version of this comment claimed a long lap makes maintenance run "back to back". That was wrong and is corrected here rather than deleted, because the wrong model is the intuitive one: LLPurgeDiskCacheThread::run sleeps its full 60 seconds at the TOP of every iteration, unconditionally, BEFORE calling purge. So a 90 second lap still leaves a full 60 second idle gap - the sleep is a fixed gap between passes, not a period the work has to fit inside. A slow lap means the disk is busy for a larger fraction of each 150 seconds, which is worth knowing and is what mSlowLaps counts; it does not mean the disk never idles.
        std::atomic<U32> mLastScanFiles{0};     // files the asset tier's directory walk visited, which is the cost that scales with a fragmented cache
        std::atomic<U32> mLastScanMs{0};        // that walk alone: stat-heavy, and the part that grows as the cache accumulates small files
        std::atomic<U32> mLastLapMs{0};         // the WHOLE iteration, both tenants - the asset purge and the texture tier's pack and reclaim, which runs after it on the same thread and was previously not timed at all even though it is the larger tier
        std::atomic<U32> mLaps{0};
        std::atomic<U32> mSlowLaps{0};

        // <SS:Nexii> mReadsServed and mReadBytes are cumulative, and a cumulative byte total cannot be compared against a disk-usage reading, which is a rate. These are the same two counters differenced across one sweep cycle by LLPurgeDiskCacheThread. The divisor is the whole cycle - the sweep plus the sleep before it - not the sweep alone, because the reads being counted happen mostly while the sweep is NOT running.
        std::atomic<U32> mReadsPerSec{0};
        std::atomic<U32> mReadKBPerSec{0};          // laps that ran longer than the 60s gap that follows them, so the sweep occupies more of the cycle than it rests

        std::atomic<U32> mVolumesKilled{0};
        std::atomic<U64> mBytesReclaimed{0};
        std::atomic<U32> mObjectsDropped{0};
        std::atomic<U32> mHotObjectsDropped{0};
        std::atomic<U32> mOrphansSwept{0};
        std::atomic<U32> mUnlinkFailures{0};

        // Per-candidate refusals inside a pack pass. Without these a pass that examined forty thousand files and packed none of them reports the same NOTHING_READY as a pass that examined three, and the difference is the whole diagnosis.
        std::atomic<U32> mSkipTooBig{0};
        std::atomic<U32> mSkipTooYoung{0};
        std::atomic<U32> mSkipUnstable{0};
        std::atomic<U32> mSkipEmpty{0};
        std::atomic<U32> mSkipUnreadable{0};
        std::atomic<U32> mSkipBadName{0};
        std::atomic<U32> mSkipAlreadyPacked{0};     // a duplicate left behind by a crash between the index flush and the unlink; the file is simply removed
        std::atomic<U32> mSkipBudget{0};
        std::atomic<U32> mSkipPinned{0};
        std::atomic<U32> mSkipChanged{0};           // the file moved between the purge scan and the read, so the bytes this pass has are not the bytes on disk
        std::atomic<U32> mSkipRaced{0};             // a write gate took the object back while it was being packed; the blob is written but unreferenced and the loose file stays authoritative

        std::atomic<U32> mPackVerdicts[SSSTRATA_PACK_COUNT];
        std::atomic<U32> mReclaimVerdicts[SSSTRATA_RECLAIM_COUNT];
        std::atomic<U32> mUnpackVerdicts[SSSTRATA_UNPACK_COUNT];
    };
    Metrics& metrics() { return mMetrics; }
    std::string metricsString() const;
    std::string statusString() const;

private:
    // Per tenant now rather than process-wide. It was a static while there was one store, and making it a member is most of what "second tenant" cost: the two tiers genuinely differ on every field in it - a different budget, a different version stamp, a different object ceiling and a different name in the log.
    Config              mConfig;
    ESSStrataTenant     mTenant{SSSTRATA_TENANT_ASSETS};

    static std::atomic<SSStrataStore*> sLive[SSSTRATA_TENANT_COUNT];   // non-null only between the end of initStore and the start of shutdownStore, one slot per tenant

    bool loadIndex();
    bool writeIndexHeader();
    bool appendIndexRecords(const std::vector<SSStrataRecord>& recs);    // one open, one write, one flush for the whole batch; a per-record open would turn a 64 byte durability point into thousands of them
    bool ensureVolume(U32 volume);
    bool allocateVolume(U32& out_volume);   // hands out the lowest reclaimed id before growing the head, because SSSTRATA_MAX_VOLUMES is consumed at the rate the tier is WRITTEN rather than the rate it grows
    bool appendBlob(const std::vector<U8>& blob, U16& out_volume, U64& out_offset);
    bool readPayload(const SSStrataRecord& rec, S32 offset, U8* dst, S32 bytes, S32& out_read);
    bool readWholePayload(const SSStrataRecord& rec, std::vector<U8>& out);
    bool killVolumeFile(U32 volume, U32 expect_generation);   // the ONLY delete of a volume in the whole tier, and it re-checks the generation under mStoreMutex before unlinking
    void sweepOrphanVolumes();
    U32  settleSecondsFor(const LLUUID& id) const;
    void deriveVolumeCap();
    ESSStrataPackVerdict packPassLocked(std::vector<SSStrataLooseFile>& files);
    ESSStrataReclaimVerdict reclaimColdestLocked(U16 max_day, bool respect_cooldown, U64& out_freed, std::vector<LLUUID>* out_dropped);
    void notePackVerdict(ESSStrataPackVerdict v);
    void noteReclaimVerdict(ESSStrataReclaimVerdict v);
    ESSStrataUnpack noteUnpack(ESSStrataUnpack v);

    // Atomic because the fetch threads read these on every gate while the main thread and the purge thread are the only writers. They are flags, not a state machine - no reader ever needs two of them to agree with each other.
    std::atomic<bool>   mInitialized{false};
    std::atomic<bool>   mShuttingDown{false};
    std::atomic<bool>   mReadOnly{false};
    std::atomic<U64>    mBudgetBytes{0};
    std::string         mStoreDir;
    U32                 mDiskCacheVersion{0};

    // Guarded by mStoreMutex unless marked otherwise. None of it is persisted: the frontiers, the free list and the byte totals are all derived from one index scan plus one directory listing, so there is no second file to fall out of step with strata.idx and therefore no torn cross-file state to reason about.
    U64                 mVolumeCap{SSSTRATA_VOLUME_CAP_MAX};
    U32                 mCurrentVolume{0};
    U64                 mVolumeEnd{SSSTRATA_VOL_HEADER_SIZE};
    U32                 mVolumeHigh{0};
    U32                 mDyingVolume{SSSTRATA_MAX_VOLUMES};   // so the allocator cannot hand out an id a kill is still working through
    std::vector<U64>    mVolFrontier;
    std::vector<U32>    mFreeVolumes;
    std::atomic<U64>    mAllocBytes{0};
    std::atomic<U32>    mRecordCursor{0};
    std::atomic<U32>    mLiveRecords{0};
    std::atomic<U32>    mGeneration{0};     // bumped by initStore and purgeAll; re-checked at every step of a pass, because purgeAll deletes the directory and can interleave with one
    std::atomic<bool>   mPackRunning{false};
    std::atomic<bool>   mReclaimRunning{false};
    std::atomic<U32>    mLastReclaimSecond{0};

    mutable std::mutex  mMapMutex;
    std::unordered_map<LLUUID, SSStrataRecord> mIndex;
    std::unordered_map<LLUUID, U8> mUnstable;                   // how many times a uuid has been unpacked by a write this session, which is what backs a repeatedly patched mesh off the packer
    std::unordered_map<LLUUID, U8> mPendingTypes;               // uuid to LLAssetType for objects written this session, so a packed record can carry a type the filename does not preserve
    std::unordered_set<LLUUID>     mPacking;                    // the packer's ownership token for a uuid it is mid-way through moving. Every write gate erases the uuid from here under the same lock, so a writer that arrives while an object is being packed silently revokes the pass's claim to it - which is the only thing standing between a concurrent write and a freshly written loose file being unlinked underneath it

    mutable std::mutex  mStoreMutex;
    Metrics             mMetrics;
};

#endif // SS_STRATA_H
