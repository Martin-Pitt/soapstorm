# Fast Timer Coverage Expansion (Proposal)

Companion to the offline capture in `ssfasttimerexport.cpp`. Goal: make the always-on
Fast Timers view (and the JSONL export) account for the whole frame, so a lag spike
can be attributed without Tracy.

## 1. Why the current set misses most of the frame

The stock conversion of the legacy `LLFastTimer` set replaced many `LL_RECORD_BLOCK_TIME`
sites with `LL_PROFILE_ZONE_*` (Tracy) calls. In non-Tracy builds those macros are **no-ops**
(`llprofiler.h`), so the Fast Timers console silently lost coverage even though the
`FTM_*` handles still exist. The frame skeleton around them is fine; what is missing is
everything between the surviving milestones.

Still active (verified `LL_RECORD_BLOCK_TIME`): frame/idle skeleton, object list update,
process objects, cleanup, HUD effects, region update, LOD update, cull/state sort/render
(pipeline + pools + shadows — the best covered area), media, chat console, inventory bulk
fetch, all `FTM_SS_*` Atmo/Storm timers.

Zombies — handle declared, site commented out:

| Handle | Site | Notes |
| --- | --- | --- |
| `FTM_IDLE_CB` | `LLAppViewer::idle()` callbacks block | commented |
| `FTM_AGENT_NETWORK` | mainloop agent-update block | declared, unused |
| `FTM_IDLE_NETWORK` | `LLAppViewer::idleNetwork()` decode loop | commented; decode is a known spike with many avatars |
| `FTM_MESSAGE_ACKS` / `FTM_RETRANSMIT` / `FTM_CHECK_REGION_CIRCUIT` / `FTM_DYNAMIC_THROTTLE` | inside `idleNetwork()` | declared at top of function, unused |
| `FTM_WORLD_UPDATE` | `gPipeline.updateMove()` in `idle()` | commented |
| `FTM_AUDIO_UPDATE` | audio block in `idle()` | commented |
| `FTM_RENDER_UI` / `_2D` / `_3D` | `llviewerdisplay.cpp` renderUI | commented — UI render time is invisible right now |
| `FTM_UPDATE_SKY` | `llviewerdisplay.cpp` | commented |
| `FTM_MESH_FETCH` | `llmeshrepository.cpp` completion callbacks (4 sites) | commented |

## 2. Placement rules

- **New handles are `FTM_SS_*`** (`static LLTrace::BlockTimerStatHandle` next to the use
  site). `FTM_*` without the prefix is reserved for restoring stock sites.
- Prefer **restoring a zombie over creating a handle** — the identifier, tree placement,
  and existing muscle memory already exist.
- Parenting is automatic via last-caller bootstrap; place the record **inside the intended
  parent's scope** (e.g. avatar sub-timers inside the `FTM_OBJECTLIST_UPDATE` scope).
- Granularity floor: only time blocks that are ≥ ~50 µs typical, or rare-but-heavy events
  (region cross, appearance change, map open). No per-face / per-joint / per-item timers;
  that is Tracy's job when it is available.
- Thread-side timers (texture decode, mesh repo) are fine: `pullFromChildren()` merges
  their sums into the frame recording, so the export captures them. They will not nest in
  the tree, which the export's meta `tree` map makes obvious to offline tooling.

## 3. Phase 0 — restore zombies (no new identifiers)

One-line uncomments, in call order per frame:

1. `FTM_IDLE_CB` — idle callbacks.
2. `FTM_AGENT_NETWORK` — agent update / autopilot block.
3. `FTM_IDLE_NETWORK`, `FTM_MESSAGE_ACKS`, `FTM_RETRANSMIT`, `FTM_CHECK_REGION_CIRCUIT`,
   `FTM_DYNAMIC_THROTTLE` — split `idleNetwork()` into decode / acks / retransmit /
   asset timeouts / throttle / circuit check. Packet decode is the #1 suspect for
   "everyone logged in, frame hitches".
4. `FTM_WORLD_UPDATE` — `gPipeline.updateMove()`.
5. `FTM_AUDIO_UPDATE` — audio commit.
6. `FTM_RENDER_UI`, `FTM_RENDER_UI_2D`, `FTM_RENDER_UI_3D`, `FTM_UPDATE_SKY` — display.cpp.
7. `FTM_MESH_FETCH` × 4 — mesh header/LOD/skin/decomposition completion callbacks.

## 4. Phase 1 — the big three gaps (new `FTM_SS_*`)

### Avatars (children of `FTM_OBJECTLIST_UPDATE`)

| Handle | Site |
| --- | --- |
| `FTM_SS_AVATAR_IDLE` | `LLVOAvatar::idleUpdate` (llvoavatar.cpp:3087); entered from LLVOAvatarSelf (llvoavatarself.cpp:1019) and control avatars (llcontrolavatar.cpp:371) |
| `FTM_SS_AVATAR_TEXTURES` | `LLVOAvatar::updateTextures` (llvoavatar.cpp:6871) |
| `FTM_SS_AVATAR_APPEARANCE` | `idleUpdateAppearanceAnimation` (llvoavatar.cpp:3576) — appearance baking spikes |
| `FTM_SS_AVATAR_KEYFRAMES` | `LLMotionController::updateMotion` (indra/llcharacter/llmotioncontroller.cpp) — BVH playback; cite-site for animation hitches |
| `FTM_SS_AVATAR_MESH_DATA` | `LLVOAvatar::updateMeshData` (llvoavatar.cpp:2822) — classic-avatar vertex path |
| `FTM_SS_GLTF_UPDATE` | `GLTFSceneManager::instance().update()` (called inside the object list update) — PBR/GLTF asset updates |

### Textures / assets (children of `FTM_FRAME` via idle)

| Handle | Site |
| --- | --- |
| `FTM_SS_TEXTURE_UPDATE` | `LLViewerTextureList::updateImages` (llviewertexturelist.cpp:851) — per-frame priority/fetch update, scales with texture count |
| `FTM_SS_TEXTURE_CACHE` | `LLTextureCache::update` (lltexturecache.cpp:826) — main-thread completions; interacts with Strata/Squeeze |
| `FTM_SS_TEXTURE_UPLOAD` | `LLImageGL::createGLTexture` (indra/llrender/llimagegl.cpp:1626) and `setSubImage` (:1252) — GL upload spikes on big mip chains |
| `FTM_SS_MESH_UPDATE` | `LLMeshRepository::update` (llmeshrepository.cpp:4401) — per-frame mesh/skin/decomposition completions; crowds and teleport-in |
| `FTM_SS_VOLUME_BUILD` | `LLVolumeGeometryManager::rebuildMesh` (llvovolume.cpp:6506) — LOD/detail regeneration on the main thread |

### Events (children as noted)

| Handle | Site | Trigger |
| --- | --- | --- |
| `FTM_SS_REGION_CACHE` | `LLVOCache` entry read path (llvocache.cpp) | region cross / teleport-in |
| `FTM_SS_WORLD_MAP` | `LLWorldMap::updateRegions` (llworldmap.cpp:669), `LLWorldMapView::draw` (llworldmapview.cpp:378) | map open / panning |
| `FTM_SS_INPUT` | `processMiscNativeEvents` / `gatherInput` (LLAppViewer::mainLoop) | OS message storms, IME, focus churn |
| `FTM_SS_SWAP` | `swapBuffers` (llviewerdisplay.cpp:230/522/1819) | GPU/vsync wait made visible as its own block |

### SS self-instrumentation follow-up

The Squeeze ticks in `idle()` (`ssBC7EncodeMaintenanceTick`, `ssBC7PromoteTick`,
`ssBC7AdaptiveTick`, `ssBudgetTick`) are un-timed; wrap each in `FTM_SS_BC7_MAINTAIN`,
`FTM_SS_BC7_PROMOTE`, `FTM_SS_BC7_ADAPTIVE`, `FTM_SS_BUDGET` so their claims ("clock
comparison until the tick elapses") are verifiable in the export.

## 5. Offline analysis loop

1. Enable `SSFastTimerExport` (debug settings), repro, insert marks from
   Advanced > Insert Fast Timer Mark at spikes, disable (or quit) to close the stream.
2. Hand over `logs/ssfasttimers_*.jsonl` (whole session) or
   `logs/ssfasttimers_recent_*.jsonl` (last `SSFastTimerExportBufferFrames` frames).
3. Offline: take the last `meta` line's `tree` for the hierarchy, then per frame the
   `[name, total, self, calls]` array supports: spike frames (`ms` > threshold),
   per-timer share of frame, self-vs-total attribution, call-count outliers, and
   mark-aligned windowing.

## 6. Cost budget

Each live timer is two rdtsc reads plus a few stores; 50 additional Phase 0/1 sites cost
well under 0.1 ms/frame. `processTimes()` (tree bootstrap) stays gated to the Fast Timers
console and the export's rare meta re-emit, as stock intends. Everything above is
main-thread measurement only; GPU time is only visible through `FTM_SS_SWAP` blocking
behavior (or Tracy when present).

Per project rules: no build here — changes land for the user to compile and soak.
