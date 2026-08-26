# Super-compressed textures: BC7 tier, cache layout, and texture-manager verdict

Status: ACCEPTED with owner amendments, 2026-08-26. Produced from a 13-agent design review (6 subsystem maps, 3 competing designs under opposed priors, adversarial critique of each, cross-design adjudication); ~110 file:line claims verified against the tree. Owner decisions folded in 2026-08-26: FULL-RES-ONLY BC7 with background promotion (supersedes encode-at-best-seen), lax watermark eviction, SSD-class hardware assumed, network promotion ON. A companion Region Object Cache design is in progress (separate section when settled). Remaining OWNER CALLs are marked.

## Verdicts (TLDR)

1. **BC7 tier**: build it as an append-only sidecar store (index + capped data segments) beside the existing J2C texture cache. J2C stays source of truth; BC7 is derived, versioned, and wipeable. FULL-RES-ONLY by owner mandate: every BC7 record is discard 0 with a full mip chain; a background promotion engine completes partial J2C via low-priority network fetches and encodes from full res. First user-visible build ~5-7 weeks.
2. **Region/agent data files (layout c)**: REJECT as physical layout, ADOPT as *manifests* over the shared UUID-keyed store. All three designs independently reached this. Manifests deliver 100% of the wanted UX (teleport warm-up, agent-appearance warm-up, evict-by-last-visited/last-seen) and get shared-asset semantics *right* where physical per-region files cannot. Details below — this is not a consolation prize, it is the correct implementation of the idea.
3. **Texture manager rewrite**: NO from-scratch rewrite now. The premise is off — there is no single texture manager; priorities live in LLViewerTextureList, decode scheduling in LLTextureFetch + the ImageDecode pool, cache traffic in LLTextureCache, per-texture glue in LLViewerFetchedTexture. Every BC7 goal lands at existing verified seams. Adopt two cheap hedges now (ITextureStore interface seam; explicit BC7 residency state ladder) that keep a later rewrite cheap if telemetry ever justifies it.
4. **Work queue granularity**: stage-parallel on the demand path (LLPointer handoff of the decoded raw to a dedicated background-QoS BC7 pool), fused full-sequence items only for idle backfill. The "avoid moving memory" concern dissolves: LLImageRaw is refcounted, so handoff moves 8 bytes, not 4 MB, and a 4-16 MB raw doesn't fit any core's cache anyway. Never fuse encode into the ImageDecode pool (latency inversion by construction).
5. **Unified all-asset cache (the full new cache system)**: deferred behind explicit triggers, not scheduled. The BC7 sidecar IS the first index+big-datafile cache, scoped to one tier; its container discipline is written so the unified store can adopt it later. OWNER CALL #6 below.

## What exists today (verified)

- TWO disk caches: legacy LLTextureCache (texture.entries fixed-record index + texture.cache 600-byte-prefix pool + textures/[0-f]/UUID.texture bodies + FastCache 16x16 thumbs) — J2C only, one dedicated worker thread, sole consumer LLTextureFetch. LLDiskCache/LLFileSystem for everything else (one loose file per asset, mtime-LRU purge thread doing a full recursive scan every 60s — 5s+ at 80K files, lldiskcache.cpp:522). This fork kept the old texture cache upstream deleted; "textures use LLDiskCache" is false here.
- Fetch is progressive: byte-range GETs sized by discard math; full resolution is the EXCEPTION (discard 0 needs ~full-screen coverage or boost). mFileSize=datasize+1 is the in-band partial sentinel (lltexturefetch.cpp:2078-2088). The fetch queue is FIFO — the priority set in llqueuedthread is commented out; priority docs at lltexturefetch.cpp:121-135 are stale.
- Uploads are raw RGBA on the main thread by default (RenderGLMultiThreadedTextures=0). Compression is effectively unused; format tables are DXT-only; no BPTC anywhere in-tree (glext.h has the enums). This is the single biggest VRAM lever available.
- Compressed upload contract: mip chain stored smallest-first, walked backward with per-mip glCompressedTexImage2D (llimagegl.cpp:779-801); compressed+mips without data_hasmips is fatal LL_ERRS (llimagegl.cpp:991); the compressed branch bypasses analyzeAlpha/updatePickMask (813/815).
- Derived-artifact precedents exist: FastCache 16x16 thumbs (the structural template for BC7 — derived-from-decode, sidecar, version-wiped) and decoded Vorbis→WAV .dsf files.
- VOCache is the per-region-file precedent: objects_X_Y.slc works per-region ONLY because its payload (local_id+CRC ObjectUpdate blobs) is meaningless outside its region. Asset bytes are the opposite: globally UUID-keyed and heavily shared.
- Other avatars: baked textures arrive with per-appearance-state UUIDs (churn on outfit change; disk-cached by UUID today, which pays off on unchanged re-sighting). Attachment mesh/texture UUIDs are the stable cross-sighting payload and arrive only via object streaming. AttachmentBlock carries inventory item IDs, not asset IDs.
- No server manifest of a region's assets exists (no cap, nothing in SimulatorFeatures) — but for revisited regions, loadObjectCache at handshake (llviewerregion.cpp:3427) materializes every previously-seen object's DP blob containing all texture/sculpt/mesh UUIDs, which the current code never mines. That is a free, locally-owned region manifest.

## BC7 sidecar architecture

LOCATION: `<cache>/texturecache/bc7cache/` — inside the texture cache dir so old viewers sharing the cache never see it, and wipes can cascade (but see purge-cascade fix below; the rename trick alone is NOT sufficient).

FILES: `bc7.idx` (append-only 64B records after a 64B header carrying format_version + encoder_version + j2c_source_version + address size) + `bc7_NNN.dat` append-only segments capped 512 MB. Record: uuid, segment/offset/size, full WxH, base discard, mip_count, format (BC7_UNORM now; BC7_SRGB/BC6H/BC5 reserved), flags (alpha_is_mask, fully_opaque, tombstone, has_pickmask, source_was_partial), **source-components byte** (see fix 2), U32 pickmask_bytes (U16 overflows at 2048²), last_use_day, data crc32, record crc32. Later record for same UUID supersedes; tombstone deletes. In-memory unordered_map built by one sequential read at startup.

BLOB: header + optional 1-bit pick mask + mip chain in the exact data_hasmips layout (smallest mip first), chain covering base_discard..5 so every upload always carries full mips (the LL_ERRS at llimagegl.cpp:991 makes miplessness fatal). One read → one buffer → glCompressedTexImage2D loop, zero reshuffle. Bonus: a prefix read yields a usable low-res chain (progressive property; eventually replaces FastCache).

CRASH SAFETY: append-only everywhere; blob before index record; per-record CRC; recovery = truncate index at last valid record, truncate segments to logical end computed from the index. Disk full → session read-only. Corruption → wipe bc7cache only (derived data; re-encode is the only cost). Versioning: wipe-on-mismatch, per the only precedent in the tree (llappviewer.cpp:5228-5246); encoder_version bump wipes only this tier.

TWO-LOCK DISCIPLINE (critique fix): map lock for lookups; store lock for file I/O; **no file I/O ever under the map lock** (multi-MB appends sharing the lookup mutex would stall the main thread). last_use touches from the main-thread lookup path are queued to the pool, never written inline.

MULTI-INSTANCE: reuse the exec-marker second-instance gate (llappviewer.cpp:4776-4811); instance 2 is a read-only snapshot, no encode pool, no eviction; readers open segments with FILE_SHARE_DELETE so the writer's eviction can proceed.

EVICTION (Phase 3): FSBC7CacheSize budget (BC7+mips ≈ 2-4x the J2C bytes — measure; OWNER CALL #3). Log-structured: copy-forward live blobs (recent last_use OR manifest-pinned) from the oldest segment, then delete the segment whole. Bytes/sec I/O throttle (HDD users). Index compaction when superseded+tombstoned >50%.

## Encode pipeline

HOOK: LLTextureFetchWorker DONE state (lltexturefetch.cpp:2128-2146), NOT WRITE_TO_CACHE — DONE also covers cache-hit decodes, and the loop-back-to-INIT condition sits right there, enabling skip-if-looping so intermediate discards streaming in are never encoded. The work item carries LLPointer<LLImageRaw> (refcounted; encoder treats it as strictly immutable and takes LLImageDataSharedLock), UUID, decoded discard, dims, components, mHaveAllData.

FULL-RES-ONLY (owner mandate, supersedes the review's encode-at-best-seen): encoding partial-res sources is wasted work and space, because the full-res asset will be visible at some point — so BC7 records exist ONLY at discard 0. The demand-path hook encodes immediately only in the free case (mHaveAllData && decoded at discard 0); every other decoded texture just lands its UUID on the promotion want-list. THE PROMOTION ENGINE (now core, not optional) drains that list on the BC7 pool in strict preference order: (a) J2C already COMPLETE in cache but lacking BC7 → fused [read → decode → mips → encode → store], zero network; (b) no complete-J2C work remaining → issue low-priority continuation fetches (offset=have, the fetch pipeline's native mode) to COMPLETE partial J2C for want-listed textures, then encode on arrival. Network completion is throttled (bandwidth setting, idle-only, paused during teleport/login) and targeted — only textures referenced by a manifest or with recent last_use, so one walk through a busy mall does not queue thousands of full-res fetches for textures never seen again.

SIMPLIFICATIONS this buys: records are effectively immutable per (UUID, encoder_version) — no supersede churn, no re-encode debounce, and dedupe is by plain UUID again. SERVING: every BC7 hit satisfies ANY desired discard. The smallest-mips-first blob layout means mips d..5 are a contiguous PREFIX of the blob — serving desired discard d is a prefix read + upload starting at that mip, so VRAM cost still tracks demand even though storage is always full-res. VRAM-pressure down-rez = re-upload a shorter prefix (RAM-resident tail mips make this free below 128px).

MIPS: generated at encode time (box filter, LLImageBase::generateMip precedent) — required, since glGenerateMipmap is illegal on compressed. Parity of sRGB-space box filtering + alpha at small mips vs today's GPU mips needs a visual check on cutout content (open question).

FORMAT: GL_COMPRESSED_RGBA_BPTC_UNORM (0x8E8C), NOT the SRGB variant — zero GL_SRGB internal formats are used for world textures; shaders do srgb_to_linear, so UNORM preserves sampling semantics exactly. gGLManager.mHasBPTC runtime check (core GL 4.2), silent RGBA8 fallback.

ENCODER: vendor bc7enc_rdo (MIT, plain C++/SSE4.1, no ISPC toolchain), wrapped behind an interface so ISPCTextureCompressor/bc7e can swap in later via encoder_version bump. Throughput claims in review ranged 5-20 vs 100+ Mpix/s/core — **P0 exit criterion: measure on target hardware before freezing any pool/backpressure constant.**

EXCLUSIONS (enqueue + read side): sculpt (geometry-not-color, FIRE-35428), media, UI/local/explicit-format, mNeedsAux, FTT_SERVER_BAKE in v1 (bake UUIDs churn per outfit; OWNER CALL #2), icon/thumbnail variants — keyed by BOOST_ICON/BOOST_THUMBNAIL boost level, not just ETexListType (the destructive rescale keys off boost). Raw-consuming textures (needsToSaveRawImage etc.) always take the J2C path. NPOT sources skipped.

## The four critical fixes (designed in from day one — each was a verified would-have-shipped bug)

1. **Explicit-format lifecycle (guaranteed crash otherwise)**: setExplicitFormat(BPTC) leaves mHasExplicitFormat + BPTC in mFormatPrimary; the reset guard at llimagegl.cpp:1575-1582 does not cover BPTC. The first time a BC7 preview is upgraded by a progressive J2C fetch, setImage computes is_compressed from the stale format and hits the LL_ERRS at llimagegl.cpp:991. Every BC7→uncompressed re-create must clear/re-derive formats as a first-class state transition (also fires on readback→refetch and raw-consumer paths).
2. **Source components injection (alpha-pool misclassification otherwise)**: BPTC is intrinsically 4-channel; pipeline.cpp:1935 / llface.cpp:346 classify alpha-blended faces by getComponents()==4. Without a stored source-components byte injected at upload, every opaque RGB texture served from BC7 lands in the alpha pool — sorting cost, draw-order artifacts, large perf regression.
3. **VRAM accounting (the success metric would be fiction otherwise)**: alloc_tex_image is called only inside setManualImage (llimagegl.cpp:1491); the compressed branch (796-801) never accounts. BC7 textures would be invisible to getTextureBytesAllocated — the A/B would over-report and the discard-bias controller would mis-steer. Add explicit alloc/free_tex_image to the compressed path.
4. **Purge cascade (privacy lie otherwise)**: the rename-wipe covers only purgeAllTextures(true); purgeAllTextures(false) (mid-session clear) and clearCorruptedCache never touch bc7cache — "clear cache" would leave a complete BC7 copy of every texture plus manifests recording where you went and whom you saw. Hook ALL purge sites; close store handles before wipe (open handles also break the Windows dir-rename trick for the J2C cache).

Plus smaller specified fixes: (UUID,discard)-keyed encode dedupe with supersede-pending (plain UUID dedupe silently drops discard improvements); raw-needing callback acquired after BC7 residency forces the J2C path (callback starvation otherwise); pick mask computed at encode at PARITY resolution (base level, not a 128px cap — coarsening it changes picking behavior on vegetation/fences); blob-read failure → tombstone + J2C fallthrough; BC7 residents exempt from mDownScaleQueue (scaleDown's FBO path is illegal on BPTC); VRAM-pressure down-rez via RAM-resident tail mips (sub-128px mips are tiny) with blob re-read fallback — never a disk-read storm at the worst moment; flush encode queue on teleport (queued raws pin up to 256 MB).

## Work queue

Pool: LL::ThreadPool("BC7Encode") — the name auto-gains the ThreadPoolSizes override. Workers self-set THREAD_MODE_BACKGROUND_BEGIN inside an overridden run() — note the in-tree "precedent" at llappviewerwin32.cpp:1404 is itself broken (wrong-thread handle; the mode is valid only for the current thread), so this is the first working instance. Background QoS deprioritizes CPU, I/O, and memory pressure — priority via QoS, not width rationing. Conservative default width until throughput is measured (sleepy_robin idle workers Sleep(1)-poll; a wide idle pool burns wakeups).

Backpressure: bounded queue + tryPost only (WorkQueue::post blocks when full — a blocked fetch thread stalls every texture); ~256 MB cap on pinned raw bytes; overflow → drop to a want-list (lossless — J2C is on disk; Phase 4 backfill re-materializes). Pause during teleport/login decode bursts.

Backfill (Phase 4): fused [read complete J2C → decode → mips → encode → store] as ONE item wholly on the BC7 pool — the decode there has no latency consumer, and this keeps it out of the latency-critical ImageDecode pool entirely.

## Q1: why manifests beat physical region/agent data files

The dedup math, honestly: asset identity is a global UUID and cross-region/cross-agent reuse is the dominant cache economics. A handful of mesh bodies/heads/hair dominate what agents wear — 100 club-goers in 5 dominant bodies means per-agent files store the same multi-MB body textures ~20x. Full-perm kits and popular textures recur across most regions — a texture seen in K regions costs K× disk and K× write amplification per revisit. This compounds with BC7 already being 2-4x the J2C bytes. Once a global index exists (conceded), physical (c) has degenerated into (a)/(b) plus grouping metadata — paying duplication for nothing the metadata doesn't deliver. And mmap-a-region-file warming is a fiction after your first visit anywhere else: a revisited region is mostly already-cached-from-elsewhere UUIDs, never contiguous in a region file anyway.

Manifests over the shared store (Phase 5): tiny files `manifests/r_<grid>_<x>_<y>.uml` / `a_<agentuuid>.uml` (grid-scoped names — SL/OpenSim coordinates collide), header + UUID list + last_seen, rewritten whole, LRU-capped (~128 regions / ~512 agents, generalizing the VOCache mTime LRU).

- TELEPORT WARM-UP: location TPs know the destination handle client-side at request time (llagent.cpp:5134-5150) — load the manifest, resolve against the RAM index, sort hits by (segment, offset), issue sequential prefix reads during the TP's network round-trip; landmark/lure TPs warm at RegionHandshake/loadObjectCache, where the already-loaded VOCache DP blobs are mined for texture/mesh UUIDs (the parser exists as partial-parse precedent, llviewerregion.cpp:2819). Warm-up lives in the store layer because gTeleportDisplay wipes all in-flight fetches (llviewertexturelist.cpp:841-855).
- AGENT WARM-UP: at processAvatarAppearance (llvoavatar.cpp:11114), load a_<uuid> and prefix-warm attachment assets (the stable cross-sighting payload; bakes churn per outfit). AttachmentBlock item-ID set = outfit fingerprint to skip warming unchanged re-sightings.
- SEMANTIC EVICTION: a record is evictable iff no manifest with last_seen < N days references it; deleting a manifest evicts a region/agent in one file operation. This gets shared assets RIGHT: an asset referenced by two regions stays until both age out — deleting region X's physical data file would either delete assets region Y still needs, or reveal there was never dedup.
- COVERAGE CAVEAT: TextureEntry mining misses GLTF/PBR material textures (base color/normal/ORM/emissive live in material assets + the GLTF extras cache) — exactly the content where VRAM pressure is worst; the miner must include the extras path.
- PRIVACY: manifests are an on-disk record of where you went and whom you saw wearing what. They MUST die on cache clear (falls out of fix 4). Posture beyond that is OWNER CALL #1.

## Q2: texture manager — deconstruction and verdict

What it actually is: LLViewerTextureList (vsize scan → log4 desired discard, round-robin update budget, discard-bias VRAM machine), LLTextureFetch (16-state worker machine + all cache traffic; transport encodes years of scar tissue — Varnish 1-byte range overlap, +1 sentinel, 404→UDP rollback, bake-URL retention), LLViewerFetchedTexture (per-texture glue, saved-raw entanglement, loaded-callback contract: multi-fire with improving discards, ALWAYS fires final on cleanup — bakes/bump/sculpt/UI depend on this), LLViewerTextureManager (stateless facade). External surface: ~160-234 call sites across 54-71 files, but wide-and-shallow (~25 methods + statics) — internals ARE replaceable behind the headers later.

Real defects (performance debt, not blockers): FIFO-not-priority fetch queue, poll-based completion (latency scales with texture count), coarse LRU, forceToSaveRawImage→refetch entanglement, mFullyLoaded never correct, TEX_LIST_SCALE dual-fetch race. None fights a sidecar; BC7 hits bypass most of the fetch pipeline, shrinking the population those defects apply to.

VERDICT: no rewrite now — 30-60+ dev-days of pure risk with zero VRAM benefit. Adopt now: (1) ITextureStore-style narrow interface in front of the new store so a later internal swap needs no consumer churn; (2) explicit BC7 residency ladder on LLViewerFetchedTexture (NONE → HIT_KNOWN → READING → RESIDENT(d), plus the RESIDENT→raw-needed and RESIDENT→uncompressed-upgrade edges — verification showed those two edges are where the crash and callback-starvation bugs live). REWRITE TRIGGERS (criteria, not schedule): (a) BC7 becomes the primary render path and J2C becomes cold storage, or (b) Phase 2-3 telemetry proves FIFO/poll latency dominates rez on warm caches. If fired: staged rewrite behind the preserved facade, keep the transport, legacy path selectable one release.

## Phases (hybrid plan)

- P0 (4-5d): GL groundwork — BPTC enums in the four llimagegl switches; explicit alloc/free_tex_image in the compressed branch; mHasBPTC + RGBA8 fallback; vendor bc7enc_rdo; debug command uploading one encoded texture via data_hasmips; decide RenderCompressTextures policy (BC7 supersedes driver compression where eligible; A/B controls for the setting — users already on it see ~1x VRAM delta, the pitch is quality/determinism/cached mips). EXIT: measured encode Mpix/s on target hardware.
- P1 (8-10d): write-only sidecar store — index+segments with CRC/truncate recovery; corrected record layout; two-lock discipline; (UUID,discard) dedupe; DONE-state enqueue + exclusions; purge cascade at ALL purge sites with close-before-wipe; second-instance read-only; kill-process/disk-full/two-instance tests.
- P2 (12-15d): read path, the VRAM win — index probe beside loadFromFastCache; async-only blob reads; explicit-format lifecycle fix; source-components injection; encode-time pickmask/alpha at parity resolution; raw-callback→J2C forcing; NPOT guard; tombstone+fallthrough on read failure; downscale exemption; telemetry. FIRST USER-VISIBLE BUILD (~5-7 weeks in).
- P3 (5-7d): bounded steady state — FSBC7CacheSize; copy-forward eviction + compaction with I/O throttle; time-based re-encode debounce; VRAM-pressure down-rez via RAM tail mips; multi-day soak.
- P4 (3-5d, optional): fused idle backfill over complete J2C entries + want-list; paused during teleport/login.
- P5 (7-10d): manifests + warm-up + semantic eviction pins (grid-scoped names, VOCache DP + cacheFullUpdate + processAvatarAppearance + TP-request hooks, GLTF extras coverage, deleted on cache clear).

CORE (P0-P3): 29-37 dev-days ≈ 6-8 calendar weeks solo. With P4+P5: ~40-52 days. DEFERRED behind triggers: unified all-asset store (keep the Strata container/journal spec as reference: per-tier volumes ≤1 GiB, journal+checkpoint, extent allocator, segment_mask for mesh) and the orchestration rewrite (keep the staged-rewrite-behind-facade plan as template).

## Owner calls

1. Agent-manifest privacy posture: deletion-on-clear only (mandatory regardless) vs obfuscation vs opt-in.
2. Server bakes in the BC7 tier: exclude v1 (recommended) vs include flagged evict-first (crowded-venue re-sighting speed vs churn).
3. Disk footprint: own FSBC7CacheSize slider (default 2-4 GB) vs folding into CacheSize; prefs UI honesty about ~2-4x growth.
4. Cold-start posture: accept multi-session warm-up vs an off-by-default network-promote setting (CDN bandwidth for full-res backfill).
5. RenderCompressTextures: retire when BC7 ships vs keep both paths.
6. North star: is the unified all-asset container the destination (stay format-compatible; adopt container discipline where free, zero speculative generality) or is a permanent BC7 sidecar acceptable?
7. P5 timing: ship manifests right after P3, or hold for P2-P3 hit-rate telemetry.

## Open questions (measure before freezing constants)

1. bc7enc_rdo Mpix/s on target hardware at chosen quality (designs disagreed 5-10x; gates every pool/backpressure constant and the ISPC swap decision).
2. Real BC7:J2C byte ratio over an actual user cache (claims 2-4x to 3-9x; gates budget default).
3. Warm-cache hit profile: fraction of revisit lookups with stored_discard <= desired_discard — the tier's whole value hypothesis; P2 telemetry.
4. Preview→upgrade UX: prototype the BC7-preview → J2C-delta → re-encode loop for perceived pop/blur walking toward buildings (most user-visible behavior in the system).
5. BPTC support rate + Intel/old-driver quirks (sizes the RGBA8 fallback; blocklist?).
6. Manifest coverage on PBR regions via GLTF extras mining.
7. HDD posture: cold blob-read latency, compaction interference, index rebuild on spinning disks.
8. sleepy_robin idle-wakeup cost at chosen pool width.
9. Mip/alpha parity: sRGB-space box filter + alpha_is_mask at small mips vs GPU mips on cutout content (vegetation, fences).
