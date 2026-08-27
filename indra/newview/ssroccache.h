/**
 * @file ssroccache.h
 * @brief Region Object Cache (ROC) container and store - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_ROCCACHE_H
#define SS_ROCCACHE_H

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The ROC is a FullID-keyed sidecar store layered over an untouched LLVOCache/.slc - it never reads or writes the protocol cache, and its own format is versioned per file so it can iterate without inflicting a global object-cache wipe on users. Rationale and the full design live in doc/region_object_cache.md.

constexpr U32 SSROC_MAGIC             = 0x434f5253; // 'SROC' as stored little-endian
// <SS:Nexii> 6 -> 7 for a SEMANTIC change rather than a layout one, which is a reason to bump this that the line below does not cover and is worth stating so nobody "optimises" the bump away.
//
// Version 6 files predate the epoch flags. A record in one carries neither ID_CURRENT nor ID_STALE, and absent-both reads as "not stale", so every such record is injectable. That is wrong for a specific and reachable file: one saved by a build with no epoch logic, on a visit that saw a restart, which therefore stamped the NEW CacheID into the header while its records still held ids from before it. On the next visit the CacheID compares equal, mode A passes, nothing is marked stale, and remembered ids from a dead epoch get painted - the exact case the epoch flags exist to refuse.
//
// Discarding is right rather than merely easy here. There is no in-file evidence that separates a safe version 6 file from that one, so a migration would have to guess, and ROC data is derived: the cost is that ghosts stay quiet for one visit per region while the ledger records them again.
constexpr U32 SSROC_FORMAT_VERSION    = 7;         // bump on ANY layout change - files from an older version are rejected outright and reclaimed by the budget sweep, never migrated
constexpr U32 SSROC_DAY_BITMAP_BYTES  = 16;         // 128 days of presence bits, one bit per calendar day, bit 0 = the day of mLastConfirmed
constexpr U32 SSROC_MAX_DP_SIZE       = 10000;      // matches MAX_ENTRY_BODY_SIZE in llvocache.cpp - blobs larger than this cannot be cached by the stock path either
// Sized for a fully upgraded region: 30000 objects at a few hundred bytes of blob each lands around 20 MB, and a region full of unusually large blobs has to fit too or saveRegionAsync silently refuses the whole file and the region never caches at all. The global budget in SSROCDiskBudgetMB is what actually bounds disk use; these two only bound one region.
constexpr U32 SSROC_SOFT_REGION_BYTES = 24 * 1024 * 1024;
constexpr U32 SSROC_HARD_REGION_BYTES = 96 * 1024 * 1024;

// Section identifiers for the file's section table. Unknown sections are skipped on read so a newer viewer's extra sections never break an older one within the same format version.
enum ESSROCSection : U32
{
    SSROC_SECTION_OBJECTS  = 1,
    SSROC_SECTION_AUX      = 2,
    SSROC_SECTION_MANIFEST = 3,
    SSROC_SECTION_GLTF     = 4,
};

// Per-record state bits. Everything here is ROC's own bookkeeping - none of it is protocol state and none of it is ever written back into the .slc.
enum ESSROCRecordFlags : U32
{
    SSROC_REC_AUTORETURN_PROOF   = 1 << 0,  // established immune to parcel auto-return (owner or group match, or auto-return disabled on the parcel)
    SSROC_REC_AUTORETURN_UNKNOWN = 1 << 1,  // immunity could not be established this session - promotion is penalised, not blocked
    SSROC_REC_BRIDGE_VERIFIED    = 1 << 2,  // an answered bridge sweep confirmed existence at least once
    SSROC_REC_REZTIME_VERIFIED   = 1 << 3,  // rez age passed the promotion threshold via an answered bridge sweep
    SSROC_REC_GROUP_SET          = 1 << 4,  // object is set to a group (only knowable from ObjectProperties or the bridge)
    SSROC_REC_DIRTY              = 1 << 5,  // mutated since it was written - re-snapshot the blob at save
    SSROC_REC_PROMOTED           = 1 << 6,  // passed every promotion gate and may be rezzed as a ghost on entry
    SSROC_REC_OWNER_PUBLIC_WORKS = 1 << 7,  // owner resolved to a Mole (Linden Department of Public Works) - permanent landscape furniture by construction
    SSROC_REC_DISQUALIFIED       = 1 << 8,  // tripped a hard disqualifier (physics, character, avatar PCode, attachment state, oversized blob) - never cleared, an object that was physical once is not trusted to have stopped
    SSROC_REC_IS_CHILD           = 1 << 9,  // the blob carried a non-zero ParentID, so its Pos is parent-relative and it may never be classified or positioned on its own

    // Local-id epoch. The simulator's CacheID names one local-id assignment epoch, and it is stored ONCE per file beside every record - so after a visit whose CacheID changed, the file carries the NEW CacheID next to records whose stored ids are still from the OLD one, because only the records the simulator actually mentioned that visit were re-keyed. Without these two bits the visit after that reads as mode A and would file a remembered object under a dead id, which is the one thing the whole design forbids. Neither bit set means a file written before this existed: legacy, treated as usable, so the format version does not move.
    SSROC_REC_ID_CURRENT         = 1 << 10, // the stored mLastLocalID came from the simulator's own stream under the CacheID this file was last saved with
    SSROC_REC_ID_STALE           = 1 << 11, // the CacheID changed while this record went unmentioned, so its stored local id names whatever the simulator has since put there - never inject it
};

// The reserved private local-id range. Nothing in the protocol reserves it; it is chosen from measurement - 556,931 real local ids across 43 SL regions, sampled from both the .slc and .roc stores on this machine, peak 0x399791FA, and not one value at or above 0x40000000. The floor therefore sits over three billion ids above the highest counter any region here has ever reached. Today nothing allocates in it; the constant exists so "the ROC never causes a send" is a one-instruction test that a stock guard can enforce rather than an invariant that has to be audited by reading call sites. See doc/region_object_cache.md.
constexpr U32 SSROC_SYNTH_ID_FLOOR = 0xFF000001u;

// One cached object. The DP blob is byte-identical to the ObjectUpdateCompressed payload the .slc stores, so every existing static unpacker and the whole OUT_FULL_CACHED rez path can consume it unmodified.
struct SSROCRecord
{
    SSROCRecord();

    LLUUID      mFullID;
    LLUUID      mParentFullID;      // null when this record is a linkset root
    LLUUID      mOwnerID;           // read out of the DP blob at parse time - free, offline, no probe required
    U32         mLastLocalID;
    U32         mCRC;
    U32         mUpdateFlags;       // captured live at message time - NOT present in the .slc disk format
    U32         mRecordFlags;       // ESSROCRecordFlags
    F32         mScore;
    U64         mFirstSeen;         // unix seconds
    U64         mLastConfirmed;     // unix seconds
    U64         mRezTimeEpoch;      // 0 when unknown - the viewer protocol has no rez time, this only ever comes from the bridge
    U16         mSessionsSeen;
    U16         mCRCChangeCount;
    U16         mChildCount;        // captured from the live object at save - root blobs carry no child enumeration, so linkset completeness is undecidable without it
    U16         mEntryCount;        // distinct region entries during which this object was confirmed
    U32         mDwellSecs;         // total seconds the agent was present in the region with this object confirmed, across all sessions
    U16         mConfirmCount;      // confirmations across all sessions - the ROC-side equivalent of the .slc hit and dupe counters, which die with the .slc and are unreachable from here anyway
    U16         mMoveCount;         // times the object was re-sighted outside its own neighbourhood
    U16         mMissStreak;        // consecutive settled visits with no confirmation - what silence does, and its complete effect
    U8          mBlockedBy;         // ESSROCBlockedBy, see ssrocledger.h - zero when promoted
    U8          mDayBitmap[SSROC_DAY_BITMAP_BYTES];
    LLVector3   mPos;
    LLVector3   mScale;
    std::vector<U8> mDP;

    // Rootness is decided by the blob's ParentID, not by whether the parent's FullID was resolvable. A child whose parent was never mentioned this visit still has a parent-relative position, and treating it as a root would file it at the region origin.
    bool isRoot() const { return (mRecordFlags & SSROC_REC_IS_CHILD) == 0; }
    bool isPromoted() const { return (mRecordFlags & SSROC_REC_PROMOTED) != 0; }

    // Day-bitmap helpers. Distinct calendar days are the CONSERVATIVE currency, used when nothing has established that the object is safe from parcel auto-return - there, only the passage of real days is evidence.
    void markSeenOn(U64 unix_secs);
    U32  distinctDaysSeen() const;
    U32  tenureDays() const;

    // Dwell accounting. A long stay is genuinely more evidence than a glance, so every visit_secs of confirmed presence counts as another visit on top of the entry itself - an evening in your home region is worth several drive-bys past a stranger's build.
    void addDwell(U32 secs);
    void noteEntry();
    U32  visitCount(U32 visit_secs) const;
};

// Sandbox policy. Auto-return is a PER-PARCEL setting, so immunity is always decided per parcel - but two cases are decided up front.
//
// A region carrying REGION_FLAGS_SANDBOX is authoritative (only Linden Lab can set it) and its object content is transient by definition, so its objects are never recorded at all. Region flags arrive in unpackRegionHandshake (llviewerregion.cpp:3276) and can change mid-session via the RegionInfo message (llviewermessage.cpp:5380), so this is re-checked rather than latched.
//
// A parcel whose NAME contains "sandbox" is a heuristic, not a fact - anyone can name a parcel anything - so it does not skip recording. It only denies the structural immunity credit, dropping those objects onto the conservative distinct-days path where they must survive real days before being cached. A false positive therefore costs patience, never correctness.
bool ssROCParcelNameDeniesImmunity(const std::string& parcel_name);

// Linden Department of Public Works. Objects owned by a Mole are the most permanent content on the grid - roads, bridges, terraforming, public builds - placed deliberately by the estate owner's own department, so they earn both the auto-return immunity credit and a large score bonus.
//
// Takes a LEGACY name ("Abnor Mole"), not a username: LLAvatarName::getUserName() returns the dot-separated form ("Abnor.Mole"), so anything that splits a username on whitespace never sees a last name at all.
//
// Deliberately narrower than FSCommon::isLinden, which also matches Scout, Tester and ProductEngine accounts - those are contractor and QA identities with no connection to permanent public works.
//
// Moving public works (trains, vehicles) need no special case here: they are physical, and physical objects are rejected by a hard disqualifier that is evaluated BEFORE any score, so a bonus can never resurrect one.
bool ssROCOwnerIsPublicWorks(const std::string& legacy_name);

// Injection order for a region's cached objects. Painting a whole region takes many frames, so what arrives in the first of them decides whether the place reads as itself straight away or as a scatter of small props with the buildings missing.
//
// Two criteria, blended rather than tiered so neither can starve the other: how permanent the object looks, and how visually large it is. A tiered sort would put several thousand tiny promoted objects ahead of a large one that only qualifies on recency, which is the wrong picture for exactly the region-hopping case the recent tier exists to serve.
//
// Size is the object's own bounding radius plus a bonus for anchoring a large linkset, because a building's root prim is frequently small while the linkset hanging off it is not - and the root prim's scale is all a cached blob carries.
//
// Returns roughly 0..1; higher is painted first. `recent_cutoff` is the unix second before which a record counts only as recent rather than promoted; pass 0 to score purely on promotion.
F32 ssROCInjectPriority(const SSROCRecord& rec, U64 recent_cutoff);

// Which of a region's cached records may be painted back on arrival. The two tiers answer different questions and are gated independently: PROMOTED asks "is this permanent enough to remember for weeks", RECENT asks "was this here a moment ago", and the second is the commoner and stronger case when hopping between a handful of regions in one sitting.
struct SSROCInjectPolicy
{
    SSROCInjectPolicy();

    bool mPromotedTier;     // draw records that cleared the immunity, persistence and score gates
    bool mRecentTier;       // draw records confirmed at or after mRecentCutoff, whatever their long-term standing
    U64  mRecentCutoff;     // unix seconds; 0 means the recent tier contributes nothing regardless of mRecentTier
    U32  mMaxGhosts;        // hard ceiling on the plan, applied after ranking so the cut falls on the least valuable objects
};

constexpr S32 SSROC_TERRAIN_ASSETS = 4;   // mirrors LLTerrainMaterials::ASSET_COUNT and LLVLComposition::CORNER_COUNT

// Terrain composition parameters: the four detail assets plus the per-corner blend heights. Kept as a plain struct so it can be packed and unit tested without dragging the composition class in.
struct SSROCComposition
{
    SSROCComposition();

    bool    mValid;
    LLUUID  mDetail[SSROC_TERRAIN_ASSETS];
    F32     mStartHeight[SSROC_TERRAIN_ASSETS];
    F32     mHeightRange[SSROC_TERRAIN_ASSETS];
};

// Region-scoped aux data - terrain heightmap, water height and composition params. EEP region environment is deliberately NOT cached (owner decision 2026-08-27): it is the one aux item whose apply path fights the viewer's own sequencing (the region-change hook fires before the destination handshake exists to validate against) and it risks a visible double sky transition, for a win the rest of the cache does not depend on.
struct SSROCAux
{
    SSROCAux() : mWaterHeight(0.f), mHeightmapGridsPerEdge(0), mHeightmapGridsPerPatchEdge(0), mHeightmapMin(0.f), mHeightmapMax(0.f) {}

    F32                 mWaterHeight;
    U32                 mHeightmapGridsPerEdge;   // 0 when no heightmap is stored - var-regions and OpenSim large patches differ, so dimensions travel with the data
    U16                 mHeightmapGridsPerPatchEdge; // patch size at capture time - a region that changed patch layout must fall back to the LayerData stream rather than paint mismatched relief
    F32                 mHeightmapMin;            // quantisation range, stored so a restore is exact to within one 65535th of the region's own relief
    F32                 mHeightmapMax;
    std::vector<U16>    mHeightmap;               // row-major, mHeightmapGridsPerEdge squared
    SSROCComposition    mComposition;

    bool empty() const { return mHeightmap.empty() && !mComposition.mValid; }

    // Quantise a region's decoded surface Z into the store. `stride` is the source row stride in floats, which is not always equal to grids_per_edge. Returns false on degenerate input rather than storing something a restore would misinterpret.
    bool packHeightmap(const F32* src, U32 grids_per_edge, U32 stride);

    // Restore into a caller-owned float array. Fails if the cached dimensions do not match what the live region expects - a var-region that changed size must fall back to the LayerData stream rather than write mismatched relief.
    bool unpackHeightmap(F32* dst, U32 grids_per_edge, U32 stride) const;
};

// One region's whole cache file, in memory. Loads are parsed off-thread and handed over as a shared_ptr; saves take a main-thread snapshot and serialise off-thread.
struct SSROCRegionFile
{
    SSROCRegionFile();

    U64                         mRegionHandle;
    LLUUID                      mRegionID;
    LLUUID                      mLastCacheID;     // the sim's CacheID at the time of writing - a mismatch on the next visit means the sim restarted and local IDs are worthless
    U64                         mSavedAt;         // unix seconds
    U32                         mSessionsSeen;
    U32                         mFlags;
    std::vector<SSROCRecord>    mRecords;
    SSROCAux                    mAux;
    std::vector<LLUUID>         mManifest;        // deduped asset UUIDs mined from the records, exported to the BC7 tier's region manifest

    size_t promotedCount() const;
    size_t approxBytes() const;
};

// Build the arrival paint order for a loaded region file. Returns indices into `file.mRecords`, most permanent and largest first, because painting a whole region takes many frames and what lands in the first of them decides whether the place reads as itself immediately or as a scatter of props with the buildings missing.
//
// Records with no blob, no last-known local id, a hard disqualifier or a STALE local-id epoch are never planned - none of the first three can be rezzed back, and the fourth would be rezzed at an id the simulator has since reissued to somebody else.
//
// Linkset children whose root did not survive the plan are dropped rather than injected as orphans: a child's cached position is parent-relative, so on its own it is a fragment with nowhere to stand.
void ssROCBuildInjectPlan(const SSROCRegionFile& file, const SSROCInjectPolicy& policy, std::vector<U32>& out);

using SSROCFilePtr      = std::shared_ptr<SSROCRegionFile>;
using SSROCLoadCallback = std::function<void(U64 handle, SSROCFilePtr file)>;

// The store owns disk layout, async IO and the disk budget. It deliberately knows nothing about ghosts, scoring or reconciliation - those live above it, so the container can be tested and soaked on its own.
class SSROCStore : public LLSingleton<SSROCStore>
{
    LLSINGLETON(SSROCStore);
    ~SSROCStore();

public:
    // SSROCEnabled gates every entry point. While it is off the store does nothing at all: no directory is created, no file is read or written, and no worker is posted.
    static bool enabled();

    void initStore();

    // Initialise on demand. initStore() runs once at startup and does nothing while the feature is off, so without this the store stays dead for the whole session when the user enables ROC at the login screen or in preferences - which is exactly how anyone would try it for the first time.
    void ensureInitialized();
    // Call BEFORE the world is torn down at quit. From here on saves are written inline on the calling thread rather than posted, because the region removals that produce the final captures happen while the worker pool is already winding down - posting them means they are never executed and the last visit to every region is silently lost.
    void beginShutdown();

    void shutdownStore();   // drains anything still in flight from before beginShutdown
    bool isInitialized() const { return mInitialized; }

    // Async load. The callback always fires on the main thread, exactly once, with a null file when the region has no cache or the file was unreadable.
    void loadRegionAsync(U64 handle, SSROCLoadCallback cb);

    // Async save of a completed snapshot. The caller must not touch the file afterwards.
    void saveRegionAsync(SSROCFilePtr file);

    void deleteRegion(U64 handle);
    void purgeAll();        // hooked into every cache-clear path - a region cache is a record of where you have been and must die with the rest of the cache

    std::string storeDir() const { return mStoreDir; }
    std::string regionFilePath(U64 handle) const;

    U64  diskBytesUsed() const { return mDiskBytesUsed; }
    void refreshDiskUsage();
    void enforceDiskBudget();   // lax watermark: nothing is evicted until the budget is approached, then whole least-recently-visited region files are dropped

    struct Metrics
    {
        std::atomic<U32> mLoadsOk{0};
        std::atomic<U32> mLoadsMissing{0};
        std::atomic<U32> mLoadsCorrupt{0};
        std::atomic<U32> mSavesOk{0};
        std::atomic<U32> mSavesFailed{0};
        std::atomic<U32> mFilesEvicted{0};
        std::atomic<U64> mBytesRead{0};
        std::atomic<U64> mBytesWritten{0};
    };
    Metrics& metrics() { return mMetrics; }
    std::string metricsString() const;

    // Serialisation is exposed for unit testing and for the debug commands - both are pure functions over a buffer with no store state involved.
    static bool serialize(const SSROCRegionFile& file, std::vector<U8>& out);
    static bool deserialize(const U8* data, size_t size, SSROCRegionFile& out);

    // Internal, public only so the worker-side scope guard can reach it: retires one in-flight IO op and wakes a shutdown that is waiting to drain.
    void endOp();

private:
    std::string gridKey() const;
    bool        writeFileBlocking(const std::string& path, const std::vector<U8>& bytes);
    static bool readFileBlocking(const std::string& path, std::vector<U8>& bytes);

    bool                    mInitialized;
    bool                    mShuttingDown;
    bool                    mReadOnly;      // a second viewer instance shares this cache directory and must never write to it, mirroring how LLVOCache refuses in read-only mode
    std::string             mStoreDir;
    std::atomic<U64>        mDiskBytesUsed{0};
    std::atomic<S32>        mPendingOps{0};
    mutable std::mutex      mDrainMutex;
    std::condition_variable mDrainCV;
    Metrics                 mMetrics;
};

#endif // SS_ROCCACHE_H
