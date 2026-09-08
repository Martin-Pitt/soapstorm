# Atmo Magic: the WorldField physics-source competition

> **Status (2026-09-08):** a 14-proposal design competition (13 designers, 3 adversarial red
> teams, 3 judges) answered the question *"should `SSWorldField` use the physics shapes of
> objects — shape type and phantom status read respectively — as a geometry source, and should
> Jolt be the engine?"* **Verdict:** physics shapes enter as **instrumentation, not truth**.
> The approved build is the **probe oracle** (P12+P13 fusion, no Jolt): a build-scoped
> declared-shape census rasterized into a read-only second span store, diffed against the
> render capture per consumer. **Jolt is deferred, not rejected** — it reopens as a major
> upgrade under the gate in §8. Consumers read nothing new until the oracle's numbers say a
> specific consumer is better served. This doc records the evidence; `doc/atmo_magic_architecture.md`
> §8 carries the rejection row; `doc/atmo_magic_worldfield.md` stays the implementation-status
> doc for the shipped field.
>
> **Revision (2026-09-08, §10):** the original brief froze the column-span representation and
> so under-asked the question. The user's actual target was the *representation*: per-column
> spans quantize indoor geometry (0.25 m horizontal cells, slab-threshold gap merging, 4 m
> bands, Z-bisection approximating bodies the render source never yields exactly) — that is
> the indoor fidelity ceiling the soundscapes, windflow and shockwave propagation hit. The
> corrected build target promotes the **exact declared-geometry query layer** (P6+P13+P14
> lineage, hand-rolled, no Jolt) for the fidelity-hungry consumers, and demotes the probe
> oracle to the governance instrument. Column spans remain only where 0.25 m is the right
> fidelity (rain/wet, drainage, coverage).

## 1. The question and the process

§8 of the architecture doc rejected "WorldField Design F (physics-sweep capture)" on record:
the capture must see what a drop, an ear and the wind see — including non-physical megaprims,
alpha with depth writes, and terrain — which is a render property. The competition tested
whether a refined physics-source idea (all objects, shape type + phantom status respectively,
phantom as a provenance layer rather than an exclusion) dodges that rejection, and what should
actually be built.

Process: 12 designers each produced one proposal from a distinct school (full replacement,
hybrid correction, per-object footprints, channel split, GPU hull raster, local query engine,
closed-form analytic, server-assisted coarse, SDF bake, LOD tiers, transient dynamic bodies,
shadow oracle); a 13th round added the declared-shape mirror with Jolt; the user's mid-run
corrections and a platform-infrastructure proposal (P14) were folded in. Three red teams
attacked on coverage/correctness, cost/threading/memory, and architecture/conventions. Three
judges scored each field on their own lens (performance economics, consumer correctness,
chief-architect process) and re-scored P13 after correction.

## 2. The field and the verdicts

Judges: J1 = performance economics, J2 = consumer correctness, J3 = architecture/process.

| # | Proposal (school) | J1/J2/J3 | Fate and harvest |
|---|---|---|---|
| P1 | **PhysicsCast** — full replacement; workers rasterize shapes; delete the GPU pipeline | 2/1/1 | **Killed.** Covert F revival + deletes the render fallback; a 1024 m megaprim is 1,048,576 `spanInsert` calls. |
| P2 | **PhysBase** — physics base layer; render demoted to a per-tile correction pass | 6/4/4 | Redesign. Right instinct, 14-row merge table owed. **Harvested:** dirty-rect render correction scoped to disagreement columns (reuses `mDirtyX0..Y1`). |
| P3 | **PhysicsStore** — per-object span footprints merged live; no global capture | 1/2/1 | **Killed.** 50 footprint swaps/s (jittering physical stack) starves the flood/acoustic serial; arrival-order-dependent answers (the banned LOD-cache class); megaprim footprints ~6 MB. |
| P4 | **Two Truths** — render capture untouched; physics builds SOLID_VOLUME_3D/WALKABLE with a provenance bit | 7.5/6/4 | **Target architecture** for any future physics channel. Fix on record: the air flood reads render spans only. Requires per-channel serial (`mGeomSerial[2]`). |
| P5 | **Hull-Raster** — same staged pipeline; `capturePass` draws instanced hull VBOs instead of the scene | 7/3/5 | Conditional keep. Keeps snapshot discipline; canopy hulls and GL_GREATER undersides need care; batched uploads only. Held in reserve. |
| P6 | **SSSolidScene** — BVH query engine; per-column lazy sweeps; tile = query cache | 2.5/5/3 | Redesign. Highest correctness potential, but lazy columns break the whole-tile snapshot discipline (flood, 2D DDA, drainage all depend on it). |
| P7 | **SpanForge** — zero-fetch closed-form analytic prim intersection; OBB degrade for meshes | 3/2/5 | **Killed as truth** — closed forms die on hollow/path-cut/twist (torus gazebo = solid block). **Harvested:** zero-fetch determinism as the discipline for any degraded path. |
| P8 | **Coarse-First** — paper AABB layer valid frame one; render authority; refine-skip oracle | 5/7/5 | **Top-3.** Physics' sanctioned role: work-skipper, never truth. Drop the invented region-wide `ObjectPhysicsProperties` protocol; never skip refinement under an AABB span's edges (that is where bridges' air gaps live). |
| P9 | **SDF Bake** — narrow-band per-tile SDF; spans = zero crossings; wall profiles = SDF taps | 1.5/4/2 | **Killed.** Own arithmetic fails: 512×512×256 cells × F16 = **134 MB/tile** (claimed 15–30); 0.5 m cells vanish 5 cm pickets. **Harvested:** SDF taps as probe-local wall distances. |
| P10 | **LOD-tiered** — FAR/MID/NEAR tiers; whole-sim far coverage; MAX_TILES split | 3.5/4/3 | **Killed as designed.** Camera-history-dependent tiering (banned pattern); 200 MB uncapped far tier; tier seams change what a Tile is. **Harvested:** memory cap as a first-class setting; the LRU is half-built (`evict()`/`mLastTouched`). |
| P11 | **Transient** — avatars/movers as decaying dynamic spans, per-channel opt-in | 3/3/2 | **Killed as shipped.** 4 serial bumps/s/tile; TTL decay is history-dependent output. **Harvested:** read-time per-channel opt-in (crowd acoustics) *if* decay becomes read-time, never history. |
| P12 | **Shadow Oracle** — parallel diff probe tile; bucket taxonomy; never served | 4/4/**9** | **The process winner.** The only proposal manufacturing the missing §8 evidence. Becomes the spine of the approved build. |
| P13 | **Declared-Shape Mirror** — ALL objects' declared shapes; phantom status as layer/provenance; Jolt | 3.5/5/3 as drafted; **~7 de-specced** | The user's LOD correction (§5) killed the top adversarial attack. De-specced (bucket grid, keyed shapes, dirty-not-serial, read-only store, resumable rasters) ties P2. **Harvested:** phantom-as-provenance layers, the all-objects census. |
| P14 | **SSQuery** — Jolt as a shared parallel ray/shape-cast service (platform infra) | *not tried* | Verified inventory: soundscape fallback = 7 rays/50 ms; `traceSolid` DDA ≈ 256 columns/64 m ray; tier-B = 128-ray bakes (store-backed); lightning strike search walks the octree (the recorded crawl rejection). Rain/snow/wind are captures, not rays. Shelved as an option paper (§8). |

## 3. Recurring failure classes (what any physics source must answer for)

1. **The visible-but-unphysical world.** Phantom megaprim walls and decks, alpha-with-depth-write
   foliage and fences, `PHYSICS_SHAPE_NONE`-but-visible decor, flexi, attachments, unfetched
   meshes. A physics-*base* answers the lived world empty; rain shadow, shelter and burial are
   render properties first. Render truth stays load-bearing.
2. **Proxy falsification.** Convex hulls seal concave gaps (arches, doorways, boat hulls,
   canopies); decomposition hulls ≠ render mesh; unfetched mesh = absent or bbox-solid; closed
   forms die on hollow/cut/twist. The proxy errs **over-solid**, which is exactly the failure
   `traceSolid`, flood labels and walkability cannot absorb.
3. **Two-source span arithmetic.** The ≤6-span store with thinnest-gap collapse has no implicit
   merge policy for a second source; every hybrid owed a consumer×layer merge table and none
   wrote one. Cross-region megaprims, water and avatar policy are members of this class:
   ownership unassigned.

## 4. Cost truths (the economics any design must respect)

1. **Serial is the currency.** One `mGeomSerial` per tile gates the flood, acoustic bake and
   every staleness check. Above ~0.5 commits/s/tile the derivations never catch up and
   consumers silently fall back to raycasts while paying full cost. Any design that writes
   per-move or per-fetch (P3, P6, P11-as-shipped) loses by construction.
2. **Column cost is quadratic in footprint.** 1,048,576 columns per region. Per-column-per-object
   schemes are O(objects × area); megaprims are the adversary. Sparseness must be per-span,
   never per-column-per-object; per-body rasterization must be resumable and budgeted.
3. **Validity is network-bound.** Mesh physics shapes arrive via `LLMeshRepository` fetch;
   `getPhysicsShapeType()` on an unknown fires an `ObjectPhysicsProperties` request
   (llviewerobject.cpp:7568; ssobjectfacts.h:113 refuses it for sweeps). Gate every census on
   `getPhysicsShapeUnknown()`, meter deliberately, batch.

## 5. Corrections entered on record

- **Physics shapes are LOD-independent.** Initial review assumed client prim tessellation was
  render-LOD and camera-dependent. Wrong: `get_physics_detail()` (llspatialpartition.cpp:2187)
  computes detail from volume params + scale alone (default 1, +1 over 5 m, +1 over 25 m —
  size-adaptive, so megaprims get *more* segments, not fewer); BOX/SPHERE/CYLINDER specs ref
  fixed-detail volumes (llviewerwindow.cpp:5141/5159); mesh physics is a separate asset from the
  four render LODs (`gMeshRepo.getDecomposition`). Confirmed empirically by enabling Render
  Metadata > Physics Shapes and inspecting at distance. The declared-shape source is stable,
  at-distance, and shares its coverage envelope (the object list) with the render capture —
  a shared bound, not a regression.
- **Phantom status is a provenance signal, not an exclusion filter.** A phantom object with
  `PHYSICS_SHAPE_PRIM` still declares good geometry; the layer it lands in decides which
  consumers may read it (a drop IS stopped by a phantom deck; walkability is not; acoustics
  mostly is). Because the render capture already *sees* phantom geometry, this insight lands
  as consumer filtering of existing render spans in phase 3 — and as probe attribution classes
  in phase 1.

## 6. The synthesis (phased, evidence-gated)

1. **Phase 1 — the probe oracle (approved to build).** P12+P13 fusion, no Jolt. Details in §7.
2. **Phase 2 — physics as work-skipper (gated on phase 1 evidence).** P8's skip-oracle: the
   probe store proves empty bands/rects cheaply so render passes are skipped. Never under AABB
   span edges. Still answers no queries.
3. **Phase 3 — the second channel (gated on phase 1 evidence).** P4's shape: provenance bit in
   the shared store, `mGeomSerial[2]` per channel, the 14-row consumer×layer table committed in
   this doc tree *before* any consumer reads a physics span; the flood keeps reading render
   spans. **WALKABLE** is the first channel where physics truth is definitionally correct
   (avatar collision *is* physics truth), built from the same census.
4. **Jolt — deferred as a major upgrade** (§8 below): reopens when a real consumer profile needs
   narrowphase (parallel raycasts / shape casts) that neither the span store nor the bucket grid
   can serve.

## 7. Build target: the probe oracle (P12+P13 fusion, no Jolt)

**Mental model: one producer, two indexes, zero consumer change.** The declared-shape census is
the producer. It feeds (1) the shipped render-capture store — untouched, the only store
consumers read — and (2) a read-only **probe span store** with the identical schema, serving
only the diff, the debug overlay and the harness.

### 7.1 Producer: a build-scoped census, not a persistent mirror

Without Jolt there is no query engine to keep fed, so P13's persistent body mirror (and with it
every dangling-body/teardown/lifecycle hazard the red teams flagged) is deliberately not built.
Instead, each probe build **snapshots** the region's declared shapes on the main thread at build
start, and workers rasterize from that immutable snapshot:

- Per object, one census record: world transform + scale, shape class, layer, provenance, plus
  either an analytic record (uncut prim: type + `LLVolumeParams`) or a tessellated/hull source
  (cut/hollow/twist prims: `LLVolume` ref'd at `get_physics_detail(params, scale)` from the
  shared `sVolumeManager` cache; mesh: `LLMeshRepository::getDecomposition` hulls, `PROV_UNFETCHED`
  shared bbox until arrival).
- Shape class resolution is **gated on `getPhysicsShapeUnknown()`** — unknown lands in
  `PROV_UNFETCHED`/its own bucket; no census ever fires an `ObjectPhysicsProperties` request.
- Layer from `flagPhantom()`: `L_DECLARED` / `L_DECLARED_PHANTOM` / `L_FEEBLE` (bbox/none/unfetched).
- Spatial index for the build: a **uniform 64 m bucket grid** over the snapshot (~150 LOC;
  replaces Jolt's broadphase; a megaprim is one candidate, never per-column-per-object work).
- Linksets rasterize as one compound record (parts individually classified).

### 7.2 The probe span store

A parallel `ProbeTile` per claimed region, **same schema as `Tile`'s store**: per 0.25 m column,
up to `SS_WF_MAX_SPANS` = 6 `[bottom, top]` solid spans (col-major), plus a per-span provenance
byte. Same cell setting (`SSWorldFieldCell`), same band ceiling semantics. No band scratch, no
flood, no acoustic bake, no drainage — spans and provenance only. It exists **only** where a
render tile is already claimed (same `Interest` refcounts) **and** `SSWorldFieldProbe` (default
off) is on; otherwise nothing is allocated and nothing runs.

### 7.3 Rasterization by shape class (per body, resumable, budgeted)

- Analytic records: closed-form per-column z-intervals over the body's 2D footprint (OBB/sphere/
  cylinder support functions clipped by column rects).
- Tessellated/hull records: per-triangle → 2D footprint cells → per-cell z-interval, unioned per
  column. Triangle source is the physics-detail volume / decomposition hull — **never the render
  LOD**.
- Per-body **resumable raster tokens** budgeted per frame on the General worker queue: a 1024 m
  wall takes many frames without ever starving the flood/acoustics jobs.

### 7.4 Merge policy and deterministic tie-breaks

`spanInsert` (the existing span algebra) materializes the probe store, with provenance-major
precedence and a **deterministic tie-break written here, not discovered in QA**:

1. Provenance order: `PROV_EXACT` > `PROV_HULL` > `PROV_TESSELLATED` > `PROV_BBOX` > `PROV_UNFETCHED`.
2. Within equal provenance: lower span bottom wins; then the lower body id — same snapshot, same
   answer, every client.
3. `L_FEEBLE` spans never evict declared spans on overflow; they merge into enclosing spans
   (the sealed-doorway rule). `L_FEEBLE` is excluded from the enclosure/coverage metric columns.

### 7.5 Commit, serials, edit choreography

Whole-tile commit on the main thread; the probe tile carries its own `mProbeSerial`. The render
tile's `mGeomSerial` is untouched. `markDirty` (ssatmomagic.cpp:777 → SSWorldField::markDirty,
ssworldfield.cpp:148) fans out to both stores: the render path re-peels its dirty rect as
shipped; the probe re-rasterizes only census bodies intersecting the rect, debounced by the same
`DIRTY_MIN_INTERVAL`. **Mesh-arrival shape swaps mark dirty; they never bump a serial per body**
(20k arrivals on region load must not re-run the flood). A diff only runs on serial-matched
pairs — arrival-order variance surfaces as `STALE_SERIAL` skips, never as wrong answers.

### 7.6 The diff: buckets, attribution, per-consumer metrics

A worker job runs when both stores are current and classifies per column:

- **AGREE** — spans match within tolerance (one cell / slab threshold).
- **RENDER_ONLY** — render solid, probe air. Attribution re-queries the census for the
  disagreeing columns: `L_DECLARED_PHANTOM` mass (a deck render saw and physics declared but
  layered apart) vs no-body-at-all (alpha cards, NONE-shape decor). That split is the empirical
  answer to the phantom question.
- **PROBE_ONLY** — probe solid, render air: hull overhangs, off-draw-distance declared objects,
  culling misses.
- **HULL_SLOP** — both solid, edges differ > one cell.
- **SPAN_OVERFLOW**, **SHAPE_UNKNOWN**, **STALE_SERIAL** — the honesty buckets.

Per-tile histograms are logged with a stable tag for the **harness** (per §9, verifiers live
outside the viewer tree) to count and threshold. Per-consumer decision metrics:

| Consumer | Metric |
|---|---|
| SurfaceGrid / surfaceTop | topmost-surface mismatch % |
| enclosure / coverage / air labels | solid-fraction delta + gap-structure disagreement % |
| traceSolid | per-ray solid-metre delta on sampled segments |
| drainage | D8 divergence + pool flips on the materialised grids |
| acoustics | wall-distance delta + portal-count diff on the baked lattice |

### 7.7 Consumer contract

All 14 read surfaces (`buildSurfaceGrid`, `surfaceTop`, `coverageAt/Detail`, `enclosureAt`,
`airLabelAt/airDepthAt`, `traceSolid`, `probesAt`, `propagationQuery`, `acousticAt`,
`buildDrainage`, and the flood's own inputs) are **byte-identical with the gate on or off**.
That is the merge-proofing invariant: enabling the oracle can never move a shipped answer.
Phase 2/3 promotions change that only through the §6 gates, with the P4 merge table written
into this doc tree first.

### 7.8 Settings, files, integration anchors

- **Setting:** `SSWorldFieldProbe` (Boolean, default 0) — real `settings.xml` entry in the
  `SSWorldField*` family (beside `SSWorldFieldDebugView`, settings.xml:27126). One gate; no
  lazy `LLCachedControl` inventions.
- **Files:** `indra/newview/ssworldfieldprobe.h/.cpp`, registered in `indra/newview/CMakeLists.txt`
  beside the `ssworldfield` entries (:268/:1240).
- **Hooks:** `llviewerdisplay.cpp` after `SSWorldField::getInstance()->update()` (:1000), behind
  `<SS:Nexii>` tags; `SSWorldField::markDirty` forwards to the probe (ss file, one line); the
  debug overlay is a new `SSWorldFieldDebugView` value (view range clamp at ssworldfield.cpp:3468),
  tinting probe spans by provenance and disagreement class.
- **Nothing else is touched.** No render-path edits, no consumer edits, no pipeline changes.

### 7.9 Costs

Second span store per claimed tile (spans + provenance only — a fraction of what `Tile` holds,
since the probe carries no flood/acoustic/scratch arrays); tens of KB of bucket grid per build;
worker-queue time for raster + diff, budgeted and resumable; zero GL, zero readbacks, zero new
network traffic. The shipped render path's frame cost is unchanged.

## 8. Jolt deferral (the option paper, P14)

Jolt (MIT, C++17, STL-only, VS2022+/GCC12+/Clang16+) is **deferred, not rejected**. The probe
oracle never steps and needs only AABB-overlap candidates plus rasterization — a ~150 LOC bucket
grid serves it, and vendoring 150k LOC (with `/fp` overrides scoped to its TUs against the
viewer's contraction ban, 00-Common.cmake:156, and a `use_prebuilt`-style governance route,
since `HAVOK_TPV` is force-disabled on Linux so no Havok substitute exists) buys nothing phase 1
can use.

**Reopen gate:** a real, profiled consumer needs narrowphase that neither the span store nor the
bucket grid can serve — the current candidates from P14's verified inventory are the lightning
strike-point search (today an octree walk; the recorded crawl rejection) and capsule shape casts
for WALKABLE. First migrated consumer would be the lightning strike search; the dependency
lands only when that profile delta clears the integration's review cost. Parallel raycast
batching for already-captured systems (rain, snow, wind) is **not** a reopen reason: they read
grids, and should.

## 9. Rejections forwarded to architecture §8

- **Physics-shape capture as consumer truth** (P1 replacement, P3 per-object footprints,
  P7 analytic-as-truth, P9 SDF bake, P10 tiering, P11 as-shipped): see the table above for the
  per-proposal whys; the class-level whys are §3 and §4. Physics enters as instrumentation; any
  revival must cite the probe oracle's per-consumer numbers.
- **Persistent per-object body mirrors without a query engine** (P13 as drafted): lifecycle
  surface with no phase-1 payoff; the build-scoped census replaces it.
- **A region-wide `ObjectPhysicsProperties` protocol** (P8's protocol half): unsupported regions
  silently degrade to permanent-coarse; the census never asks.

## 10. Addendum (2026-09-08): the representation correction — exact declared geometry for the fidelity-hungry consumers

**What the brief got wrong.** Every designer was told "the span store, flood, acoustics and all
consumer APIs stay unchanged." That froze the column-span representation and turned the
competition into a contest over capture *sources*. The user's actual target was the
*representation*, and the competition's own evidence supports the correction: P6 scored the
highest correctness potential of the field precisely because its BVH answered segments against
real geometry, and the brief still made it materialize spans.

**Why column spans cannot serve indoors, whatever the source.** Per-column `[bottom, top]`
intervals at 0.25 m cells quantize wall position and thickness to the cell grid; a 0.1 m wall
inflates to a full cell in every column it touches; a diagonal wall smears. Apertures are
counted in cells (`sqrt(aperture spans)` porch budgets), probe wall distances are cell-quantized,
air gaps below the slab threshold merge into solid, and 4 m bands mean bodies are only ever
*approximated* — the Z-bisection is a search for geometry the render source never hands over
exactly. Every acoustic figure (occlusion solid-metres, wall profiles, portal loss, RT60/room
volume, thunder travel, weapon-fire coverage) and the wind solve's interior masks inherit that
quantization. No capture source fixes this while the answer is stored as spans.

**The corrected build: two representations, by consumer need.**

1. **The exact declared-geometry query layer** — the fidelity-hungry consumers: soundscape
   occlusion and probe wall profiles, the probe graph's portals, thunder and weapon-fire
   shockwave paths, windflow apertures and interior masks. Built on the declared-shape census
   (§7.1 — gating, layers, provenance all as specified there), queried **exactly**:
   - Segment-vs-shape intersection: closed-form vs analytic prim records (OBB/sphere/cylinder/
     prism support math); ray–triangle (Möller–Trumbore) vs hull and tessellated records at
     `get_physics_detail()` — never the render LOD.
   - Raycasts replace the quantized wall lattice: the 8-direction probe profile becomes 8 exact
     cast distances.
   - Apertures and diffraction edges are **measured off the geometry** (real doorway widths,
     window areas, eave edges) instead of span-count budgets.
   - Layer semantics fit sound naturally: `L_DECLARED` and `L_DECLARED_PHANTOM` walls block
     sound; `PHYSICS_SHAPE_NONE` reads through, optionally supplemented by render-derived
     coarse occluders (policy decided by the probe oracle's numbers).
   - Broadphase: the 64 m bucket grid walked 3D-DDA along each segment; queries batched on the
     worker queue. Hand-rolled — no Jolt (§8 deferral stands; the reopen gate is unchanged).
   - This is the P6+P13+P14 lineage: P6's exact-query insight, P13's declared-shape census and
     layer semantics, P14's service shape — minus everything that broke (lazy stores, live
     merges, vendoring).
2. **The column-span store stays** — for the bulk consumers where 0.25 m *is* the right fidelity
   and alpha canopies matter: rain/wet SurfaceGrid, drainage, coverage/burial, air-gap flood
   labels. Render-sourced, untouched. This is Design F's domain, correctly kept.

**What this retires for the migrated consumers:** `traceSolid`'s 2D DDA, the acoustic wall
lattice's cell quantization, span-derived apertures, and any dependence on Z-bisection ever
resolving indoor bodies — bodies arrive exact, with no bisection, no slab merging and no
horizontal quantization.

**Cost reality.** Per-query cost, not store cost: the soundscape spends ~7 live rays per 50 ms
cycle plus bake-time batches; thunder and weapon fire are event-driven; every query batch is
worker-parallel and deterministic per census snapshot. The per-segment cost of exact math
against bucketed candidates is bounded by the candidate count along the segment — the megaprim
adversary is handled by the same per-body budgeting as §7.3.

**Sequencing (decided 2026-09-08).** Build order: the **exact query layer first** — the census
plus exact segment/ray queries, wired into the soundscape behind `SSWorldFieldShapes` (default
off) — because it attacks the indoor fidelity pain directly. The probe oracle (§7) follows on
the same census as the governance instrument that decides, with per-consumer numbers, which
truth feeds which consumer (the phantom/alpha attribution and the NONE-shape fallback policy
for sound). WALKABLE (§6 phase 3) becomes capsule casts against the same census.
