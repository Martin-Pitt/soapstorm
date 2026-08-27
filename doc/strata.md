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
