# Strata: unified cache policy, and the container it does not need

Settled 2026-08-27 by a design review that ran three competing architectures against the tree and against
the owner's disk. This supersedes the "Strata container/journal spec" referenced as deferred in
`doc/super_compressed_textures.md`. Read this before proposing anything named Strata.

## SCOPE CORRECTION, 2026-08-27 - read before the rest of this file

The review below answered "should the BC7 store and the ROC store be merged into one container?" and
correctly said no. **That is not what Strata was for.** Strata was always aimed at J2C and general asset
storage - the tens of thousands of individual files - and the review scoped exactly those out, putting the
J2C header trio in DO-NOT-BUILD and treating LLDiskCache as a reported number.

The owner's actual goal: **stop 85,400 individual asset files existing.** Per-tier containers are
explicitly acceptable; BC7 and J2C living in separate sets of large volumes is fine. The measured problem
is LLDiskCache (50,544 files) and the J2C texture cache (34,817 files), not the two stores that already
behave.

Everything below remains correct FOR THE QUESTION IT ANSWERED - the engine assessment, the ROC verdict,
the no-journal and no-allocator findings and the segment-size reasoning all still hold, and the engine
assessment is the reason the little-files work is tractable at all. But do not cite "a unified container
adds nothing" as a reason not to fix LLDiskCache: that conclusion was reached about merging two tiers that
were already few-big-files, and says nothing about the tiers that are not.

## The verdict in one line

**Grow `ssbc7store.*` into the container; unify POLICY, not storage; leave ROC alone; build no allocator
and no journal.**

## Why the container was the wrong half of the problem

A unified container adds almost nothing here, and this was checked rather than assumed:

- **File handles: no gain.** Neither store holds any. The BC7 store closes every handle deliberately,
  because an open handle defeats the directory-rename purge in `lltexturecache.cpp`. ROC holds none either.
- **Syscalls: no gain.** One open/seek/read per blob either way.
- **Cross-tier dedupe: nothing to dedupe.** A BC7 texture blob and a ROC region record share no bytes.
- **Crash recovery: actively worse.** Each tier's current scheme is trivially correct — ROC gets whole-file
  atomicity from temp-plus-rename, BC7 from write-blob-then-index plus a read-side UUID check. A shared
  container replaces two simple schemes with one needing a journal.

Unified *policy* is where the value is: one budget that is genuinely total, one definition of hot, one
metrics surface, one serialisation primitive.

## The BC7 store is already ~80% of the engine

It has volumes with a runtime cap held separate from the format ceiling, a fixed-stride per-record
checksummed index recovered by one sequential scan, tombstones with later-supersedes-earlier semantics, a
derived free list with a dying-id interlock, byte accounting on *allocated* rather than live bytes, three
phase startup recovery, self-describing blobs with per-level CRCs so the index can be rebuilt from data
alone, and a generation epoch re-checked under the lock that performs the unlink — the piece hand-rolled
stores most often get wrong, already correct here.

What it lacks to be general: a type/namespace dimension, sub-object addressing, an intra-volume allocator,
support for mutation, a 64-bit seek (`ssBC7WriteAt` refuses past 0x7FFFFFFF), online compaction, and
cross-tier arbitration. That is a list of features to add to one engine, not a reason to start a second.

Building beside it would mean a second generation counter, a second torn-tail scanner, a second orphan
sweep and a second read-side identity check — and getting all four right twice.

## ROC stays exactly as it is

Not a close call; all three candidates reached it independently.

- **The record IS the file.** Whole-file read into one buffer, whole-file write via temp-plus-rename. No
  seek, no partial read, no per-record addressing anywhere. A container's entire value is addressable
  sub-objects, and ROC would use none of it.
- **Every field mutates on every visit** — last confirmed, dwell, confirm count, miss streak, day bitmap,
  score. The BC7 engine's correctness rests on the opposite: records immutable per (uuid, encoder_version),
  which is what makes plain-UUID dedupe sound. A mutable tenant means either update-in-place, for which
  there is no allocator, or a ~2.9 MB free-then-allocate on every region exit — the worst possible workload
  for an extent allocator, manufacturing precisely the fragmentation this store provably does not have.
- **The key is the wrong shape**: a grid-scoped U64 region handle, not a global asset UUID.

ROC joins the unified BUDGET. It does not join the container.

## Do not build

- **The journal.** A WAL earns its keep when an update must be atomic across writes a reader cannot
  independently validate. Neither holds: every blob carries its own UUID and per-level CRCs, checked at
  read time with the UUID checked first and unconditionally, and there is no multi-volume transaction by
  construction. A journal is a second file that can disagree with the index — exactly the cross-file torn
  state this design deliberately has none of.
- **The extent allocator.** Measured twice, the second time at double the sample: 30 segments, 3,596 live
  records, 323 tombstones, and **zero gap bytes in every single segment**. 100% dense. There is no free
  space inside any live volume to allocate into, and that is structural, not luck.
- **Hole punching.** 64 KB granularity on NTFS, a different API with different semantics elsewhere, and
  sparse files break the stat-versus-derived drift check the orphan sweep prints today.
- **A second thread pool.** `LL::ThreadPool` registers by name in an instance tracker, so a second one is
  more shutdown ordering to get wrong, for work that runs once a minute.
- **An overlay line claiming to be "the cache".** The governed tiers are ~8,800 MB of an 11,171 MB cache
  directory. Label it "governed tiers" and stay honest.

## Segment size: settled, and it is not a file-size knob

`SSBC7_SEGMENT_BUCKETS` is the **reclaim precision** knob, because reclaim is one unlink. Measured: at the
old 85 MiB cap a segment held 51-108 live records; at the 341 MiB cap it holds 261-374, so one kill already
drops around 300 textures. Bigger volumes mean proportionally more thrown away per reclaim.

At `CacheSize` 16384 with a 50% share the store lands on ~24 volumes of 341 MiB on its own, with no further
work. Anyone re-raising "why not one enormous file" should read this paragraph and the allocator entry above.

## Measured state, 2026-08-27

| tier | files | size |
|---|---|---|
| J2C texture cache | 34,817 | ~2,700 MB |
| LLDiskCache (`cache/`) | 50,544 | 3,898 MB |
| **BC7** | **31** | **4,180 MB** |
| ROC | 47 | 135 MB |
| object cache | 93 | 105 MB |

BC7 holds more bytes than any other tier, in 31 files against 85,400. It is the only tier not producing the
little-files problem.

**Total authorised spend is 34,816 MB against a 16,384 MB slider**, because `FSDiskCacheSize` (16384) and
`SSROCDiskBudgetMB` (2048) both sit entirely outside `CacheSize`. Measured in use: ~11,171 MB and climbing —
the BC7 tier alone went 1,966 → 4,179 MB in a single session.

## Staged plan

1. **Shared serialisation + tier accounting, REPORTING ONLY.** One `ssserial.h` for the Writer/Reader/CRC
   that `ssbc7store.cpp` already admits it copied from `ssroccache.cpp`; a minimal `ISSCacheTier`
   (allocated, budget, name, read-only, metrics line — and nothing else); four thin adapters; one overlay
   block. Nothing on disk changes. ~1 session, low risk.
2. **The budget arbiter — `CacheSize` becomes genuinely total.** Folds in `FSDiskCacheSize`,
   `SSROCDiskBudgetMB` and the now-dead `SSBC7CacheSize` override. The J2C share becomes movable
   mid-session by writing `sCacheMaxTexturesSize`, which both purge paths re-read live — so no worker
   protocol and no touching the documented mutex ordering. Keep the absolute-override escape hatch.
   **Ship shares visible-but-not-enforced for one session first: turning the total on WILL delete data that
   currently survives.** ~1 session, medium risk, and the risk is user-visible rather than technical.
3. **Reclaim arbitration** — one tick, one watermark, named verdicts modelled on the existing eviction enum
   so a refusal is counted rather than silent.

Stages beyond 3 are gated on the BC7 read path, which is now landing as its own workstream.

## Owner decisions outstanding

1. **`FSDiskCacheSize` is 16384 MB, entirely outside the total**, holding 3,898 MB across 50,544 files right
   now. It is the single largest unaccounted number in the tree. Folding it in will start evicting assets
   that currently survive indefinitely. Confirm before stage 2.
2. **Is 16384 the ceiling you actually want enforced?** Say the number before stage 2 makes it real.
3. **Bucket count is taste, not correctness.** 24 volumes of 341 MiB, which is free, or 8 of 1 GiB behind a
   debug setting at roughly triple the textures dropped per kill. Recommendation: stay at 24 until the
   hot-records-dropped metric has a real number to argue with.
4. **Stage ordering for the mesh/asset tier versus the J2C bodies.** Mesh first touches no texture-fetch
   state machine; J2C first is the tier you actually look at but has the higher blast radius.

## Corrections to the review's own output

The review asserted the owner had set `SSBC7CacheSize` to 8192 and that it was being silently ignored.
**Wrong on the premise**: `SSBC7CacheSize` is at its shipped default of 2048 and is not overridden. The
substantive half stands — the setting is dead whenever `SSSqueezeCachePercent` is non-zero, which it is by
default — but nobody set 8192.

It also reported `readBlobPrefix` as declared-but-undefined with no callers. True when it ran; the read
path workstream has since implemented it and wired the residency ladder.

---

# Strata stage 1 as built, 2026-08-27: the LLDiskCache asset volume store

The plan above is unchanged and still the right order for the BUDGET work. What follows is the little-files
work the SCOPE CORRECTION asked for, landed against LLDiskCache only: `indra/llfilesystem/ssstrata.{h,cpp}`,
`ssstrataio.cpp`, `ssstratapack.cpp`, plus `indra/llcommon/ssserial.h`. The J2C texture cache is untouched.

## The measured distribution, which decided the shape

`V:/Firestorm/Cache/cache`, full recursive scan, 2026-08-27:

| | |
|---|---|
| files | **57,947** (the 50,544 in the table above has grown) |
| bytes | 4,704,503,197 (4,487 MB) |
| min / median / max | 20 B / 17,687 B / **6,439,778 B** |
| p10 / p25 / p75 / p90 | 1,476 / 4,594 / 68,030 / 201,719 |
| p95 / p99 | 372,873 / 950,459 |
| files <= 4 KiB | 13,376 (23.1%), holding 0.5% of the bytes |
| files <= 64 KiB | 42,950 (74.1%), holding 14.4% |
| files <= 1 MiB | 57,475 (99.2%), holding 84.2% |
| files > 1 MiB | **472** (0.8%), holding 15.8% |
| NTFS 4 KiB cluster slack | **119 MB wasted** on per-file rounding alone |

**There is no large-file population.** The largest object in the entire cache is 6.14 MiB, which is smaller
than one volume. A size threshold that left "the big ones" loose would leave 472 files behind and save nothing,
so the threshold shipped at 8 MiB is a GUARD, not a policy: today it excludes zero files, and it exists only to
bound the packer's per-object buffer and to guarantee an object always fits in a volume with room to spare. The
data says pack everything, and it does.

## The architectural decision: not (a), not (b), but a third thing

The problem posed was that `LLDiskCache` lives in `indra/llfilesystem`, below the `indra/newview` home of the
BC7 store. The answer taken is **(c): the reusable engine is not the volume manager, it is the record framing,
and only that goes down.**

`indra/llcommon/ssserial.h` is header-only and holds the explicit little-endian Writer/Reader, the CRC32
wrapper, and the fixed-stride record checksum convention (`sealRecord` / `checkRecord`) that `ssbc7store.cpp`
already admits it copied out of `ssroccache.cpp`. llcommon sits below both llfilesystem and newview, so no new
link edge exists in either direction. The asset store is then a SECOND store in llfilesystem, modelled closely
on the BC7 one: same three-file split, same two-lock discipline, same derived-frontier recovery, same
generation re-checked under the lock that performs the unlink, same named-verdict enums.

Why not (a), lifting the volume manager down:

1. **The workloads are genuinely different shapes, and the difference is the one the BC7 store cannot absorb.**
   The review above already lists "support for mutation" first among what the BC7 engine lacks to be general.
   That is not hypothetical here: `llmeshrepository.cpp` opens a cached mesh with `LLFileSystem::READ_WRITE`,
   pre-allocates it to full size, and then patches LOD bytes into it at an offset as each level arrives - four
   separate call sites, minutes or days apart. `llviewerassetstorage.cpp` and `lltransfertargetvfile.cpp` build
   objects through runs of `APPEND`. Generalising the BC7 engine to serve that means either update-in-place -
   for which there is no allocator, and the allocator was rejected on measurement - or copy-forward on every
   patch. Either way the BC7 store loses the property its correctness rests on: records immutable per
   (uuid, encoder_version), which is what makes plain-UUID dedupe sound there.
2. **It is unpaid risk on a component under active measurement.** The BC7 tier is serving 1.8 GB of live VRAM
   savings and is mid-testing. Refactoring it buys nothing this increment needs.
3. **Half the BC7 engine is texture-specific and cannot go down anyway** - per-mip CRCs, pick masks, discard
   levels, prefix reads. What is left after removing those is roughly the record framing, which is exactly what
   was lifted.

`ssbc7store.*` is **not touched by this change at all**, so its no-viewer-globals property and its offline test
are unaffected. Folding it onto `ssserial.h` is stage 1 of the plan above and remains a separate, mechanical
pass. The cost of deferring it is worth stating plainly: for now the two stores each carry their own copy of the
framing, which is the "second torn-tail scanner" objection the review makes above. It is accepted deliberately,
because the alternative was editing a working store mid-test.

## The shape: a write-back staging tier, not a direct container

Writes are left EXACTLY where they are - one loose file per uuid, written on the fetch thread with the same
syscalls and the same failure modes as today. A background packer running on the existing
`LLPurgeDiskCacheThread` folds objects into volumes once they have stopped changing. Reads consult the
in-memory index first and fall through to the loose path when it misses.

This is what makes the mutation problem disappear rather than needing to be solved: mutation happens on loose
files, and packed objects are immutable. It is also what satisfies "no main-thread stall, and fetch threads must
not block on cache IO" - no fetch thread ever waits on container IO, because the container is only ever written
by the once-a-minute maintenance thread that already existed.

Costs of this shape, stated rather than buried:

- A steady-state staging area of loose files exists and never reaches zero. It is bounded by the pack age
  (300 s by default) times the write rate.
- An object written, packed, then written again pays one extra copy. The unstable set doubles the settle time
  for each such object, up to eight times the base, so a mesh being filled in LOD by LOD backs itself off rather
  than thrashing. `mUnpacked` and `mUnpackedBytes` measure exactly this; if unpacked bytes ever approach packed
  bytes, either the pack age is too short or a type-aware rule is needed.

## The crash rule, in one line

**The index record is committed LAST when packing and REVOKED FIRST when unpacking.** Every crash window then
fails toward a cache miss and never toward two disagreeing copies of one uuid.

- Packing: write the blob, flush, append the index record, flush, and only THEN unlink the loose file. A crash
  before the index flush leaves the loose file authoritative and some unreferenced bytes past the volume
  frontier, which the next rollover overwrites. A crash after it leaves a byte-identical duplicate, which the
  next pass recognises and unlinks (`mSkipAlreadyPacked`).
- Unpacking: read the old bytes, revoke the record, THEN write the loose file. A crash in between loses the
  object and costs one re-fetch.

There is therefore **no startup reconciliation scan** and no window in which a stale volume copy can shadow a
newer loose file. The one place stale bytes can be served is a tombstone write that fails; it is counted as
`mTombstoneFailed` and logged with a reason, and the cost is that the next session serves an older but valid
copy of the same uuid until it is superseded.

A concurrent write against an object mid-pack is handled by an ownership token (`mPacking`), erased by every
write gate under the same lock that publishes the record. A pass whose token was revoked drops the record
(`mSkipRaced`) and never unlinks the loose file. The unlink loop re-checks the index a second time, because a
write gate may have unpacked the object between the commit and the unlink.

## Nothing is wiped

The existing 57,947 files are **absorbed, not deleted.** They are the loose staging tier by construction, so the
packer folds them into volumes over the first few minutes of runtime and unlinks them as it goes. There is no
re-fetch storm and no user-visible data loss. First-run absorption uses a catch-up rate of 512 MB per tick while
more than 4,096 loose files remain - roughly nine minutes for the measured 4,487 MB - dropping to 64 MB per tick
in steady state.

The volumes themselves are versioned and wiped, never migrated: `strata.idx` stamps `SSSTRATA_FORMAT_VERSION`
and the viewer's `DiskCacheVersion`, and a mismatch in either orphans every volume, which the startup sweep then
unlinks. That is the established precedent in this codebase for derived data the network is authoritative for,
and it is the same rule `ssroccache` and `ssbc7store` already use.

## Resulting file count

The volume cap is derived from the budget, `clamp(budget / 64, 16 MiB, 256 MiB)`, for the reason recorded beside
`SSBC7_SEGMENT_BUCKETS`: reclaim is one unlink, so the number of volumes IS the precision of one kill. At the
shipped `FSDiskCacheSize` of 16384 MB that is 256 MiB volumes.

| | files |
|---|---|
| before | 57,947 |
| volumes holding the measured 4,487 MB | 18 |
| index | 1 |
| loose staging, steady state | low hundreds |
| **after** | **a few hundred, from 57,947** |

One kill drops 256 MiB, about 3,300 assets at the measured mean of 78 KB. That is the same trade the BC7 tier
already accepts at 341 MiB, and `mObjectsDropped` and `mHotObjectsDropped` exist so the trade can be argued with
using a number.

## Budget: one total across both tiers

`LLDiskCache::purge` now drives three things off ONE directory walk - the pack pass, the accounting and the
drain - because the walk is the expensive part on a cache this size and doing it twice a minute would cost more
than the container saves.

The drain is the only genuinely new policy. Loose files are individually deletable and a volume is not, so the
two are drained oldest-first against each other: the coldest sealed volume is offered its own bytes-weighted
mean use day against the mtime of the oldest loose file still standing, and whichever is older goes. The store
declines by name - `TOO_YOUNG`, `ALL_HOT`, `COOLDOWN`, `NO_CANDIDATE` - so a purge that could not free anything
says which of those it was.

Volume heat is a BYTES-WEIGHTED MEAN of the last-use day, not the newest member. Scoring by the newest member
sounds conservative and is in fact broken: in steady state the loose tier is a handful of freshly written files
and the volumes are everything else, so no volume would ever score older than the oldest loose file and the
drain would grind the small warm staging area to nothing while never touching four gigabytes of cold data. The
hot-byte veto - 25% of bytes touched in the last 300 seconds - is what protects what the user is looking at, and
it does that far more precisely than a maximum could.

`allocatedBytes()` is the sum of the volume frontiers plus the index, which is what is actually on disk. It is
deliberately not the sum of live object sizes, because that drops the instant a record is tombstoned while the
disk does not move until the unlink. The packer also refuses to grow past the budget on its own
(`SSSTRATA_PACK_OVER_BUDGET`), so the tier cannot outrun the slider between watermark checks the way the BC7
store once did.

## Every decline is named and counted

Three verdict enums - `ESSStrataPackVerdict`, `ESSStrataReclaimVerdict`, `ESSStrataUnpack` - all tallied, plus
per-candidate refusal counters: `mSkipTooBig`, `mSkipTooYoung`, `mSkipUnstable`, `mSkipEmpty`,
`mSkipUnreadable`, `mSkipBadName`, `mSkipAlreadyPacked`, `mSkipBudget`, `mSkipChanged`, `mSkipRaced` and
`mSkipPinned`. Read failures are separated into `mReadsFailedOpen`, `mReadsFailedShort`,
`mReadsFailedIdentity` and `mReadsFailedCRC`, because "the blob was well formed and belonged to a different
uuid" is a different bug from "the bytes did not verify". `metricsString()` prints all of it at shutdown and at
the end of every purge, and the counters include the SUCCESS paths so that a working feature cannot look inert.

## Other decisions worth reviewing

- **Firestorm's static assets are never packed.** `prepopulateCacheWithStatic` decides what to copy by asking
  whether the loose file exists, so a packed static asset would be re-copied every startup and unlinked every
  session. The purge scan marks them pinned and the packer skips them (`mSkipPinned`).
- **The `LLFileSystem` gates go through `SSStrataStore::live()`**, a relaxed atomic load, rather than
  `instance()`. `LLSingleton::instanceExists()` takes a mutex on every call and these are the hottest functions
  in llfilesystem; more importantly, a fetch thread reaching `instance()` during shutdown would resurrect a
  singleton `LLSingleton` had already deleted. Null means "behave exactly as the viewer did before Strata".
- **A pre-existing infinite loop was fixed in `purge`.** The stock Firestorm code did `continue` without
  advancing the directory iterator when `file_size` or `last_write_time` failed, so a single unreadable file
  would spin the purge thread forever.
- **A partial read cannot verify the payload checksum.** A read covering the whole object checks the uuid, the
  declared size AND the payload CRC; a positional read - which is what a mesh LOD fetch is - can only check the
  uuid and the declared size. That is the same compromise `ssBC7VerifyBlobPrefix` makes for a coarse discard
  serve, and it is why the blob header carries a uuid at all.
- **`DiskCacheVersion` was NOT bumped by this change.** Existing loose files are absorbed, so there is nothing
  to invalidate. Bump it only if the loose-file layout itself ever changes.

---

# Strata stage 1b as built, 2026-08-27: the J2C texture body tier

The second half of the little-files problem, landed against `LLTextureCache` as a SECOND TENANT of the store
built in the previous section rather than as a second store. Files touched:
`indra/newview/lltexturecache.{h,cpp}`, `indra/llfilesystem/ssstrata.{h,cpp}`, `ssstrataio.cpp`,
`ssstratapack.cpp`, `lldiskcache.{h,cpp}`, `indra/newview/llappviewer.cpp`, `settings.xml`. No new files, so
no CMake change; both lists were checked for an existing entry first.

## The measured distribution, which decided the shape

`V:/Firestorm/Cache/texturecache/[0-f]`, full recursive scan, 2026-08-27. `bc7cache` excluded - it is 7 files
holding 2,698 MB and is not the problem.

| | |
|---|---|
| files | **15,090** |
| bytes | 1,316,059,804 (**1,255 MB**) |
| min / median / max | 5 B / 12,288 B / **10,160,524 B** |
| p10 / p25 / p75 / p90 | 3,609 / 4,096 / 45,056 / 176,128 |
| p95 / p99 | 392,423 / 1,524,000 |
| mean | 87,214 B |
| files <= 4 KiB | 6,362 (42.2%), holding **1.7%** of the bytes |
| files <= 16 KiB | 10,322 (68.4%), holding 5.1% |
| files <= 64 KiB | 12,312 (81.6%), holding 11.4% |
| files <= 512 KiB | 14,871 (98.5%), holding 61.1% |
| files > 8 MiB | 13 |
| NTFS 4 KiB cluster slack | 8.6 MB |

Container files alongside them: `texture.cache` 23,452,200 B, `texture.entries` 1,094,480 B (39,087 slots,
of which 15,090 have a body), `FastCache.cache` 40,650,480 B.

The shape is the mirror image of the asset cache: **the files are tiny and the bytes are not in them.** 42%
of the files hold 1.7% of the bytes. Anything that only absorbs small files deletes most of the file count and
almost none of the megabytes, and anything that leaves large files loose leaves 13 of them behind.

## The cheap route was measured and it loses. Here are the numbers

`TEXTURE_CACHE_ENTRY_SIZE` is `FIRST_PACKET_SIZE`, 600 bytes. `texture.cache` is a **FIXED-STRIDE** file:
record `i` lives at `i * TEXTURE_CACHE_ENTRY_SIZE`, and `39,087 * 600 = 23,452,200` is exactly its size on
disk. Raising the threshold to absorb files of size X therefore costs `entry_count * X`, **whether or not any
given entry is that big**, and the bodies are what is left after the first 600 bytes are taken away.

| new stride | absorbs | files removed | files left | `texture.cache` becomes | padding bought |
|---|---|---|---|---|---|
| 600 (today) | - | - | 15,090 | 23 MB | - |
| 4,696 | <= 4 KiB | 6,362 | 8,728 | **184 MB** | +160 MB for 22 MB of data |
| 16,984 | <= 16 KiB | 10,322 | 4,768 | **663 MB** | +640 MB for 64 MB of data |
| 66,136 | <= 64 KiB | 12,312 | 2,778 | **2,585 MB** | +2,562 MB for 143 MB of data |
| 10,161,124 | everything | 15,090 | 0 | **379 GB** | absurd |

Three further findings, any one of which is on its own disqualifying:

1. **It shrinks the cache.** `initCache` computes `sCacheMaxEntries = 0.36 * max_size / (ENTRY_SIZE +
   TEXTURE_FAST_CACHE_ENTRY_SIZE)`. At a 4 GB texture budget that is 88,470 entries at a 16 KiB stride and
   **23,196 at 64 KiB - fewer than the 39,087 entries in use right now.** The knob that absorbs the most files
   also caps the cache below its current occupancy.
2. **It is load-bearing in the fetch state machine.** `lltexturefetch.cpp` initialises `mDesiredSize` to
   `TEXTURE_CACHE_ENTRY_SIZE` and floors it there in three more places, so the constant is simultaneously
   "the header record size" and "the smallest amount of a texture the viewer ever asks the network for". The
   stock comment says as much: *"there is no good to define 1024 for TEXTURE_CACHE_ENTRY_SIZE while
   FIRST_PACKET_SIZE is 600 on sim side."* Changing it changes network request sizing.
3. **It wipes anyway.** The stride is baked into `texture.cache`, so changing it needs a
   `sHeaderCacheVersion` bump, which calls `purgeAllTextures` and orphans every body file. The cheap route
   costs a full cache wipe *and* hundreds of megabytes of zero padding *and* a smaller cache.

**Verdict: (a) is rejected on measurement.** It is not cheap, it does not capture most of the win, and it
reaches into the texture fetch state machine, which is the one thing this pass was told to leave alone.

## What was built: (b), the existing staging tier as a second tenant

`SSStrataStore` stopped being a singleton and became one instance per `ESSStrataTenant`, created on first use
and never destroyed. The asset tier is tenant 0, so `live()`, `instance()` and `instanceExists()` keep their
old spellings and old meanings and `ssstatsview.cpp` was not touched. Dropping `LLSingleton` is a small safety
gain rather than a cost: the header already explains that `live()` exists because a fetch thread reaching
`instance()` during shutdown could resurrect a singleton `LLSingleton` had already deleted, and a store that is
never destroyed cannot be resurrected.

What became per-tenant: the whole `Config` (it was a process-wide static), the published `sLive` pointer, and
the log tag - every `LL_INFOS("Strata")` line now reads `Strata[assets]` or `Strata[textures]`, because two
tiers writing *"packed 400 objects"* into one log with no way to tell which is a diagnosis lost.

What is deliberately NOT shared: volumes, index, budget, version stamp and wipe lifetime. `initStore` refuses
outright if another live tenant already serves the same directory, because the two indexes have the same name
and the two volume sets the same numbering, so a mis-set path would have each tier sweep the other's volumes
as orphans. That refusal is tested.

Three engine changes, all additive:

- `SSSTRATA_MAX_OBJECT_CEILING` (16 MiB) split out of `SSSTRATA_MAX_OBJECT_BYTES` (8 MiB). The format ceiling
  had to rise because the largest measured J2C body is 10,160,524 bytes - the asset cache's largest object was
  6.14 MiB, so 8 MiB had never been tested against anything. Raising a ceiling can only accept records an older
  build already accepted, so no existing index is invalidated. The asset tenant's own policy value is
  unchanged at 8 MiB; the texture tenant uses 12 MiB.
- `reclaimColdest` takes an optional `out_dropped`. `LLDiskCache` passes nothing and is unchanged.
- `Config::mDefaultAssetType`. Every object in the texture tenant is `AT_TEXTURE`, so `noteAssetType` is a
  no-op there and the packer reads the constant - which is up to 1.5 MB of resident map not spent holding one
  repeated value.

## Why the texture tier is an EASIER tenant than the asset one

The previous section's central problem does not exist here, and it is worth saying why rather than leaving it
to be rediscovered. `llmeshrepository.cpp` opens a cached mesh `READ_WRITE` and patches LOD bytes in at an
offset; that is what forced the write-back staging design. **A J2C body is never mutated.**
`LLTextureCacheRemoteWorker::doWrite` writes the body whole, at offset 0, every time - a better discard level
rewrites the object rather than extending it - and `doRead` reads it whole or as a prefix from offset 0. So
`prepareForWrite` is called with `restore_bytes = false`: the record is revoked and the old payload is simply
dropped, because the caller is about to overwrite it a syscall later.

The staging shape is kept anyway, for two reasons that are about risk rather than about mutation. It keeps
every body write on exactly the syscall and thread it uses today, so no fetch thread can end up waiting on
container IO; and a bug in the packer degrades to a cache miss while the loose files keep working, which a
direct container would not.

The `mUnstable` machinery does earn its keep here after all, incidentally: a texture being refined discard by
discard is unpacked once per refinement, and the settle-time doubling backs the packer off it exactly as it
does for a mesh.

## The seam: six call sites, four helpers, one file

Body files are touched in exactly six places in `lltexturecache.cpp` and nowhere else in the viewer. All six
go through four helpers so that no null check is spread through the fetch state machine:

| call site | was | now |
|---|---|---|
| `doRead`, BODY stage | `LLAPRFile::size` | `bodySize(id, path, pool)` |
| `doRead`, BODY stage | `LLAPRFile::readEx` | `readBody(id, path, offset, dst, n)` |
| `doWrite`, BODY stage | `LLAPRFile::writeEx` | `writeBody(id, path, src, n)` |
| `removeCachedTexture` | `LLAPRFile::remove` | `forgetBody(id)` + the unlink |
| `removeEntry` | `LLAPRFile::remove` | `forgetBody(id)` + the unlink |
| `purgeTextures`, the 1-in-256 validation | `LLAPRFile::size` | `bodySize(id, path, pool)` |

`readBody` falls straight through to the loose read when Strata declines rather than probing for the file
first. `readObject` returns false both for "this uuid was never packed" - one hash lookup, nothing logged -
and for a genuine read failure, which has already revoked the record and named the reason; the two resolve the
same way, and telling them apart would have cost a `stat` on **every** body read to make one log line nicer.

`pool` is a required parameter rather than a default because which APR pool is safe depends on the calling
thread, and the stock code already gets that right at each site: the header-mutex holders use
`mHeaderAPRFilePoolp`, the workers use their own. Hiding that choice inside the helper is how the two would
eventually be mixed up.

**The documented mutex ordering is untouched.** Nothing in Strata ever takes `mHeaderMutex` or
`mHeaderIDMapMutex`; its two mutexes are strict leaves below them, so no new nesting exists and no cycle is
possible. `mHeaderMutex` is taken by the maintenance tick in exactly two bounded windows and by nothing else.

## Where the packer runs

`LLPurgeDiskCacheThread` - the once-a-minute maintenance thread that already hosts the asset packer, which
`doc/strata.md` above rules out duplicating. It cannot name `LLTextureCache` (llfilesystem sits below newview),
so it gained one registered `std::function` hook, set in `initThreads` next to the thread's construction and
long before its `start()`, which is what makes the registration race-free. The texture tick runs AFTER the
asset purge in the same wake-up rather than on a timer of its own: two tiers doing heavy directory work at the
same instant on the same disk is the one arrangement that makes background maintenance something the user can
feel.

Candidates come from `mTexturesSizeMap`, **not from a directory walk.** The entry table already knows every
uuid that has a body, so the tier needs no second recursive scan of a 15,000 file directory - which is the cost
`doc/strata.md` says the container must not add. A tick copies at most `SSSTRATA_TEX_SCAN_TICK` (8,192)
`(uuid, size)` pairs under `mHeaderMutex` and builds every path and stats every file **outside** it, then
resumes from a rotating cursor next tick. 8,192 rather than 4,096 on purpose: it has to exceed
`SSSTRATA_CATCHUP_FILES` or a cold cache can never present enough candidates in one pass to be recognised as
catching up, and absorption would run at the 64 MB/tick steady-state rate instead of 512 MB.

`mLooseFiles` and `mLooseBytes` are accumulated across the ticks of one lap and published only when the lap
closes. Publishing a tick's slice directly would report "8,000 loose files" forever on a cache that has three,
and a permanently wrong metric is worse than one that updates every few minutes.

## Reclaim, and the one genuinely new interaction

`LLTextureCache`'s own LRU purge keeps the sum of LIVE body sizes under the slider and is unchanged - packed
bodies still carry their `mBodySize` in the entry table, so its accounting never learns that anything moved.
But a packed body that is evicted frees nothing on disk until the volume holding it is unlinked, so **dead
bytes are the only thing that can push the volumes past the budget while the live total sits comfortably under
it.** The maintenance tick therefore reclaims against `allocatedBytes()`, not against the live total.

Killing a volume drops records the entry table still claims a body size for, and an entry claiming a body that
no longer exists serves 600 bytes and reports them as a whole image - a corrupt texture rather than a cache
miss. That is why `reclaimColdest` hands the dying uuid list back and `strataDropEntries` retires those
entries in the same pass. It queues them through `mUpdatedEntryMap` and calls `writeUpdatedEntries` once, so a
reclaim that drops three thousand entries costs one open of `texture.entries` rather than three thousand.

`max_day` is passed as `0xFFFF` because, unlike `LLDiskCache`, there is no loose-versus-packed drain race to
arbitrate: the loose tier here is transient staging the packer empties within one settle time.

One consequence worth knowing before reading a log: `mTouchSecond` is in-memory only, so **everything just
packed is inside the 300 second hot window** and a tier that has only ever packed and never been reloaded
reports `ALL_HOT` rather than freeing anything. That is by design and the harness asserts it.

## What happens to the 15,090 existing files

**They are absorbed, not deleted.** They are the loose staging tier by construction, so the packer folds them
into volumes over the first few minutes of runtime and unlinks them as it goes. There is no re-fetch storm and
no user-visible data loss. Catch-up runs at 512 MB per tick while more than 4,096 candidates remain, and the
per-tick file cap is 4,096, so the binding constraint is files rather than bytes: **roughly four minutes for
the measured 1,255 MB.**

`DiskCacheVersion` is NOT bumped, `sHeaderCacheVersion` is NOT bumped, and nothing is wiped. The volumes
themselves are versioned and wiped rather than migrated: the texture tenant stamps a CRC32 of
`sHeaderCacheVersion`, `sHeaderCacheAddressSize` and `LLImageJ2C::getEngineInfo()` into its index header, so
any change that already makes `readHeaderCache` purge the entry table also orphans the volumes, belt and
braces. Volume format changes are handled the same way `ssroccache` and `ssbc7store` handle them.

One gap was found and closed while writing this: `LLAppViewer::initCache` calls `purgeCache()` on the texture
cache BEFORE `initCache()` has told the store where it lives, so `purgeAll` would decline. On Windows the
whole `texturecache` directory is renamed away and the volumes go with it, but elsewhere the loops only call
`deleteFilesInDir` on the root, which does not descend. `purgeAllTextures` now deletes the `strata` child by
path when the store is not yet initialised.

## Resulting file count

Volume cap is `clamp(budget / 64, 16 MiB, 256 MiB)`, the same derivation and the same reclaim-precision
reasoning as the asset tier. At the shipped split the J2C tier gets whatever `CacheSize` leaves after the BC7
share, which on the owner's current settings is several gigabytes, so the cap lands on 256 MiB.

| | files |
|---|---|
| before | **15,090** loose bodies |
| volumes holding the measured 1,255 MB | 5 |
| index | 1 |
| loose staging, steady state | low tens |
| `texture.cache` / `texture.entries` / `FastCache.cache` | 3, unchanged |
| **after** | **under 20, from 15,090** |

Across both tiers the two caches go from roughly 73,000 files to a few dozen.

One kill drops 256 MiB, about 3,000 texture bodies at the measured mean of 87 KB. `mObjectsDropped` and
`mHotObjectsDropped` exist so that trade can be argued with using a number.

## Settings

One new setting, `SSStrataTextures` (Boolean, default on). Off leaves any existing volumes in place and still
readable and simply stops new bodies being folded in - the same posture as `SSStrataEnabled`.
`SSStrataPackAgeSeconds` is shared by both tenants; the texture tenant's object limit is a constant sized off
the measurement rather than a setting.

## Metrics

The texture tier's counters are reached with `SSStrataStore::live(SSSTRATA_TENANT_TEXTURES)`, which returns
null when the tier is off or torn down, and `metrics()` / `metricsString()` / `statusString()` on it are the
same surface the asset tier already exposes to `ssstatsview.cpp`. That file was not touched; wiring a second
overlay block is the owner's to do.

## Decisions the owner should review

1. **Reclaim can drop entries the LRU purge would have kept.** Killing a volume is coarse by design, and it
   retires whichever entries were in it regardless of their LRU position. It only fires when the volumes
   outgrow the slider on dead bytes, so it should be rare, but `mObjectsDropped` versus `mHotObjectsDropped`
   is the number to watch.
2. **The J2C budget is now enforced twice against two different totals** - the LRU purge against live body
   bytes, the tier against allocated bytes - and they can disagree by the dead-byte amount. That is deliberate
   and is what bounds the tier, but it means the on-disk total can sit slightly above the slider by the size
   of the loose staging area.
3. **Every eviction now writes a 64 byte tombstone.** `purgeTextures` at startup can retire thousands of
   entries, each costing one `fopen`/`fwrite`/`fflush`/`fclose` of `strata.idx` on top of the unlink it already
   did. `fflush` is a CRT flush and not an `fsync`, so this is tens of microseconds each rather than
   milliseconds - but it is new work on a path that already runs at startup, and it is worth a look at the
   first startup timing after the tier has been packing for a while.
4. **`SSSTRATA_MAX_OBJECT_CEILING` moved from 8 to 16 MiB**, which is a format-level change to a store that is
   already live for assets. It is a widening, so it cannot reject anything that used to be accepted, and the
   asset tenant's policy value did not move - but it is the one edit in this pass that touches a shipped
   format.

---

# Strata stage 2 as built, 2026-08-28: the budget arbiter

`CacheSize` is now divided in ONE place instead of by three tiers each deciding for themselves. **Nothing is
enforced yet.** `SSStrataBudgetEnforce` ships OFF and every tier keeps, to the byte, the number it has today;
all this build does is compute what it WOULD hand out and say so in the log. That was a deliberate instruction
and it is the reason the pass is safe to run for a session before anyone decides a ceiling.

Files: new `indra/newview/ssstratabudget.{h,cpp}` and `ssstratabudgetwire.cpp`; edits to `llappviewer.cpp`,
`llviewercontrol.cpp`, `lltexturecache.{h,cpp}`, `ssbc7storeio.cpp`, `ssbc7store.cpp`,
`indra/llfilesystem/lldiskcache.h`, `settings.xml`, `indra/newview/CMakeLists.txt`. **ROC is not touched.**

## The measured problem, restated as an arithmetic identity

| setting | value | inside `CacheSize`? |
|---|---|---|
| `CacheSize` | 16384 MB | it IS the total |
| `FSDiskCacheSize` | 16384 MB | **no** |
| `SSROCDiskBudgetMB` | 2048 MB | **no** |

The governed tiers alone authorise **32768 MB against a 16384 MB slider** - 8192 MB of J2C remainder, 8192 MB
of BC7 share and the whole of `FSDiskCacheSize` on top - with 2048 MB of region cache outside even that. The
harness asserts that number rather than describing it, so a future edit that quietly halves the overspend
cannot land without saying so.

## The split, in two halves, for one reason

`ssstratabudget.cpp` is the arbiter and reaches for **no setting, no singleton and no filesystem**: numbers in,
plan out. `ssstratabudgetwire.cpp` is everything that has to know about the viewer. The split exists because an
arithmetic mistake in the first file deletes gigabytes of somebody's cache, and this is the strongest argument
the codebase has for a file that can be driven in an offline harness. It is the same split
`ssbc7store.cpp` / `ssbc7storeio.cpp` already uses.

## The three rules, in order, and the order is the design

1. **The J2C tier is reserved a quarter of the total FIRST and is never scaled.** It is the re-encode source
   for everything else: a BC7 blob whose J2C body was evicted comes back from the network rather than from
   disk. This is the floor that already existed in `initCache`, moved rather than invented.
2. **Absolute overrides are honoured at face value out of what is left.** A share of zero selects an absolute
   number - the escape hatch `SSBC7CacheSize` already models - and an override that gets quietly scaled is not
   an override.
3. **Percent shares divide whatever the overrides left**, scaled proportionally when they oversubscribe rather
   than served first-come, so three tiers asking 50% each get a third each rather than two full and one empty.

Only when the absolutes ALONE do not fit does rule 2 give way, and that outcome is named `OVERRIDE_SCALED`
rather than folded into the general scaling, because it is the one case where the arbiter overrode something
the user asked for explicitly.

Seven verdicts, all named and printed: `AS_REQUESTED`, `FLOORED`, `PERCENT_SCALED`, `OVERRIDE_SCALED`,
`SQUEEZED_OUT`, `UNGOVERNED`, `TIER_DISABLED`. A share that became zero always says which of them it was.

## The shares at `CacheSize` 16384, with the shipped defaults

| tier | today | would get | measured on disk | would drop |
|---|---|---|---|---|
| J2C textures | 8192 MB | 4096 MB (`FLOORED`) | ~1255 MB | 0 |
| BC7 sidecar | 8192 MB | 8192 MB (`AS_REQUESTED`) | 4180 MB | 0 |
| asset cache | 16384 MB | 4096 MB (`AS_REQUESTED`) | ~4487 MB | **~391 MB** |
| region cache | 2048 MB | 2048 MB (`UNGOVERNED`) | 135 MB | 0, not governed |

`SSStrataAssetCachePercent` defaults to 25 and `SSSqueezeCachePercent` is already 50, so the three governed
shares land on exactly 50 / 25 / 25 and spend the slider precisely. **That leaves J2C exactly on its floor**,
which is a coincidence of the defaults rather than a design, and the harness pins it so that a future default
raising either share is caught by a failing test rather than by a user's cache emptying.

## Why nothing moves until the switch is flipped, stated mechanically

`ssBudgetEffectiveBytes` returns the GRANTED number when enforcing and the LEGACY number when not, and legacy
is today's formula transcribed rather than recomputed. That matters more than it sounds, because the two
existing formulas genuinely disagree with each other:

- `SSBC7Store::budgetBytes` multiplies the **raw, unclamped** setting and then divides.
- `llappviewer.cpp` divides the **clamped** total and then multiplies, and treats a percent of zero as
  contributing nothing to the split rather than selecting the absolute.

At `CacheSize` 16384 with a 50% share those two land 42 bytes apart, and a percent of zero today gives the J2C
tier the whole total *while* the sidecar takes `SSBC7CacheSize` on top - a genuine overspend that the granted
column fixes and the legacy column faithfully reproduces. The harness asserts the 42 bytes explicitly.

With enforcement off, `LLTextureCache::sCacheMaxTexturesSize` **is not written at all** - not with a recomputed
value, not with the same value. The only way to promise no byte moves is to leave the variable alone.

## The lever, and why there is still no worker protocol

`sCacheMaxTexturesSize` became a `std::atomic<S64>` and gained `LLTextureCache::ssSetTexturesBudget`. Both
purge paths and `strataMaintenance` already re-read it on every pass, so writing it moves the tier at the next
pass with nothing notified and **nothing added to the documented mutex ordering**. The entry table's cost comes
off inside the setter, exactly as `initCache` takes it off `max_size`, so the arbiter deals in whole tier
shares. Every access is relaxed on purpose: a reader that sees the old number purges to the old ceiling once
more, and a mutex around an eviction target would be a new lock in the middle of the documented ordering for no
correctness gained.

`LLDiskCache` needed no new seam - Firestorm already moves `mMaxSizeBytes` mid-session through
`setMaxSizeBytes` - and the BC7 store is deliberately never pushed at, because it reads the arbiter on its own
tick and a second copy of a number is a second thing to disagree with the first.

## What is deliberately NOT here

- **ROC is reported and never governed.** `SSBudgetROCTier` reads two numbers and writes nothing. Owner
  decision 5 splits the workstreams here, and `ssroccache.cpp` / `ssrocledger.cpp` were live under another pass
  while this one landed.
- **No overlay edit.** `ssBudgetOverlayLine()` and `ssBudgetTierAdapter()` are exposed; `ssstatsview.cpp` is
  untouched. The line says "governed tiers", not "the cache", for the reason this document already gives.
- **No reclaim arbitration.** That is stage 3.

## `ISSCacheTier`, exactly as specified and no more

`allocatedBytes` / `budgetBytes` / `tierName` / `isReadOnly` / `metricsLine`, four thin adapters, no lifecycle,
no eviction, no registration. Anything wanting a sixth member wants a different object. `LLDiskCache` gained
two const accessors (`getStoredCacheSize`, `getMaxSizeBytes`) so the asset adapter costs nothing: the stored
size is the number the once-a-minute purge already publishes from the directory walk it had to do anyway.

## Stage 1's serialisation lift, done

`ssbc7store.cpp`'s anonymous `Writer` / `Reader` / `crc32Of` - which its own comment admitted were a verbatim
copy out of `ssroccache.cpp` - are gone, replaced by three `using` declarations over `indra/llcommon/ssserial.h`.
One hunk at the top of the file, no link edge in either direction, and proved rather than asserted: the store's
221-check offline test was built twice, differing in that one file alone, and the two runs produce byte-for-byte
identical output. The volume manager and everything texture-specific stayed where they were, for the reasons
`ssserial.h` states at length.

## Verification

- Syntax-only parse (`/Zs`) against the real viewer headers, zero errors: `ssstratabudget.cpp`,
  `ssstratabudgetwire.cpp`, `llappviewer.cpp`, `llviewercontrol.cpp`, `lltexturecache.cpp`,
  `ssbc7storeio.cpp`, `ssbc7store.cpp`, and the `llfilesystem` files `lldiskcache.cpp`, `ssstrata.cpp`,
  `ssstrataio.cpp`, `ssstratapack.cpp`, `llfilesystem.cpp`.
- Offline harness: the existing 165 Strata checks still pass, plus **130 new arbiter checks** including a
  49,005-configuration sweep of the invariants (never overspend the slider, never breach the J2C floor, never
  escape the clamp, never move the region cache) and a second sweep asserting that with enforcement off not one
  configuration differs from today by a single byte.
- The BC7 store's 221 checks, identical before and after the serialisation lift, with
  `SSBC7Store::budgetBytes` routed through the real arbiter in both builds.

## The one decision left, and it is the owner's

**Is 16384 the ceiling you actually want enforced?** On the measured occupancy, turning
`SSStrataBudgetEnforce` on today costs roughly **391 MB out of the asset cache and nothing else**, because BC7
and the J2C bodies both sit under their shares already. That is much smaller than it looks only because the
asset tier has not yet grown into the 16384 MB it is currently authorised: at its authorised size the same
switch would drop about **12 GB**. The cost of waiting is that the tier keeps growing towards a number nobody
chose.
