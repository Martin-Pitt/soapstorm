# Ghillie — Alpha Sorting Review & Hardening Plan

Status: design plan (not yet implemented)
Applies to: feature-ghillie worktree
Companion: `ssghillie.{h,cpp}`, `ssghilliemesh.{h,cpp}`, `lldrawpoolalpha.cpp`, `llspatialpartition.cpp`, `llface.h`

---

## 1. Why this belongs with Ghillie

Alpha sorting and occlusion culling are linked:

- Alpha groups render **behind** the opaque sort in the same octree; their
  draw order is a *second* culling-like decision the viewer makes per frame.
- The Ghillie software Hi-Z buffer already describes what the **opaque**
  world blocks. Anything purely translucent **behind** that buffer is
  provably invisible — a free cull for the exact surface Ghillie rasterizes.
- Dense layered translucent surfaces (the common grid abuse: stacked alpha
  planes, particle spam, tree-of-overlapping-planes builds) are currently
  drawn **unconditionally**, full-screen, far→near, every frame. This is
  both a correctness (sort order) and a performance (fill + state churn)
  problem — and the perf half is the same class of problem Ghillie exists
  to solve for opaque geometry.

So: Ghillie's HZB is the natural tool to *derive* the alpha budget and the
alpha-behind-opaque skip, and Ghillie's worker pool is the natural place to
run a global alpha sort off the render thread.

---

## 2. Current machinery (verified against this tree)

1. **Per-group depth pick** — `LLSpatialPartition::calcDistance`
   (`llspatialpartition.cpp:660-701`):
   - `dist = length(eye)`, `eye.normalize3fast()`
   - if `!group->hasState(ALPHA_DIRTY)` and view-angle moved >0.64 rad →
     `group->mViewAngle = view_angle`, `group->setState(ALPHA_DIRTY)`,
     `gPipeline.markRebuild(group)`
   - `group->mDepth = (eye − 0.25 * at * objBounds) · at`  (front of AABB)
2. **Per-face key** — `LLFace::getKey() == mDistance` (`llface.h:202`);
   `LLDrawInfo::getKey()` and the face `operator<` order **farthest-first**
   (`llface.h:351`).
3. **Draw** — `LLDrawPoolAlpha::renderAlpha` (`lldrawpoolalpha.cpp:583`)
   walks per-group draw infos, farthest-first, via `pushBatch`; rigged
   alpha is a separate sub-pass; `prepare_alpha_shader` clamps minAlpha
   (`MINIMUM_ALPHA` vs `MINIMUM_IMPOSTOR_ALPHA`).
4. **Water plane** — `LLDrawPoolAlpha::sWaterPlane` used to cull alpha on
   the far side of opaque water (`renderPostDeferred`).

### What actually happens today

- Sorting is **per octree group, not global**. Two intersecting translucent
  planes in different groups render in *group order* → visible pop +
  state-change churn.
- `mDepth` is the **front corner of the AABB** along `at`, not the true far
  translucent surface. Large/overlapping planes can invert or tie.
- Group dirtiness is **angular-only** (0.64 rad). Pure translation with no
  rotation never re-sorts, even when a near translucent plane crosses a far
  one.
- `mDistance` is only refreshed when the group dirties → sort order is stale
  by up to 0.64 rad of view motion.
- Rigged + unrigged alpha never interleave globally → a rigged translucent
  attachment in front of an unrigged translucent wall renders wrong.
- No accounting of **layer count / overlap**; dense layered alpha is always
  drawn in full.

---

## 3. Goals

1. Correct global-ish alpha sort (no cross-group inversions for the common
   cases).
2. Re-sort when it *matters* (translation crossings, not just yaw).
3. Big perf win on the dense-layer abuse by:
   a. skipping alpha **behind the opaque Hi-Z** (Ghillie),
   b. **capping translucent layers** per screen-space slab after the first
      few (error-bounded: `(1-α)^k` contribution decays), with hysteresis,
   c. raising minAlpha for distant translucent.
4. Keep all of it **debug-visible** (extend the Ghillie debug view) and
   **fail-open** when the camera is in `FAST_REBUILD` / teleport.

---

## 4. Phase plan

### Phase 1 — Instrumentation (Ghillie debug extension)
- Per-region / per-group alpha statistics, surfaced on the overlay:
  - alpha drawinfo count
  - overlapping-plane metric (screen-space slab occupancy)
  - sort inversions vs a true back-surface sort reference
  - `renderAlpha` time
- Gate: `SSGhillieDebugAlpha` (default off). No rendering-behavior change.

### Phase 2 — Sort correctness (quality, low risk)
1. **Global back-surface sort** at cull time:
   - Gather the region's alpha draw infos (snapshot PODs, no viewer
     structures on the worker), sort by true back-surface depth:
     `key = (eye − center)·at + radius·|at·axis|`  (back of the AABB, not
     the front; per-face `mDistance` recomputed for visible alpha faces).
   - Fail open to today's per-group order when the threaded result isn't
     ready (same double-buffer pattern as Ghillie's verdicts).
2. **Translation dirtying**: mark a group `ALPHA_DIRTY` when
   `|Δ(center−eye)| / |center−eye| > few %` — not only view-angle — so
   near-plane crossings re-sort.
3. **Rigged+unrigged interleave**: one merged alpha bucket for the global
   sort; rigged kept as a sub-bucket by *merged* key so skin palettes stay
   coherent across the boundary (reorder boundary only when budget allows).
4. **Layer-aware cutoff** per screen-space slab (coarse 64×64 grid like the
   HZB): draw far→near; once a slab has seen `N` translucent layers, stop
   (see 5.2 for the error bound).

### Phase 3 — Dense-layer mitigation (the abusable case)
1. **Alpha-behind-opaque skip via Ghillie HZB (biggest win):**
   - Build/keep the software Hi-Z from the **opaque** surface as today.
   - For each alpha draw's screen-space slab, if the slab's *near* surface
     is behind the HZB depth → entirely hidden → skip. (Opaque occludes
     translucent by construction.)
   - Cautions:
     - never apply when the alpha crosses the camera near plane,
     - keep the depth test *itself* enabled so the GPU still resolves
       partially-covered edges (skip only the *provably behind* case),
     - the skip must not race the HZB build: reuse the double-buffer /
       generation lane from the main HZB job.
2. **Layer cap with error budget:** only the first ~2-4 *nearest*
   translucent layers matter per pixel; each additional layer contributes
   `(1-α)*prior`. Implement per screen-space bin with hysteresis (emit only
   after stable for K frames) to avoid popping. Expose `SSGhillieAlphaMaxLayers`.
3. **minAlpha by distance/slab:** raise the effective minAlpha for distant
   translucent using HZB depth + distance; clamp so cheap sparse layers
   discard rather than blend.

### Phase 4 — Perf at the draw side
- Run the global alpha sort on the **Ghillie worker pool** (same POD
  snapshot pattern, off render thread).
- Batch alpha draws by (shader, texture, blend) within the global order to
  cut `pushBatch` state churn — the real fps killer in dense scenes.
- Skip the global sort during `CAMERA_FAST_REBUILD` frames (moving camera);
  fall back to per-group.

---

## 5. Adversarial review of this plan

### 5.1 Correctness traps
- **Global sort on a moving camera** can thrash (teleport/respawn yanks the
  key). Mitigate: gate on camera mode, fall back to per-group during
  `FAST_REBUILD`.
- **`mDepth` is a proxy**, and a *back*-surface key is still a proxy for a
  plane crossing + looking through it. Document that the sort is
  "correct enough for far-vs-near", not a true per-fragment sort.
- **Alpha-behind-opaque must not apply** when the alpha plane straddles the
  near plane or when the HZB belongs to a different camera state (Ghillie
  already fails open on camera-jump; reuse that lane).
- **Layer-cap popping**: dropping the 3rd layer while looking at a gradient
  stack will pop on slight camera motion. Hysteresis + screen-slab
  stability (not per-object) is mandatory.
- **Rigged sub-bucketing** must not break per-avatar skin palette batching;
  only reorder across the boundary when it cannot break a batch.

### 5.2 Error budget for the layer cap
- For alpha stack with opacity `α1..αk` (nearest first order of
  accumulates): contribution of layer k ≈
  `αk * Π_{i<k}(1−αi)`. For typical `α≈0.3-0.5`, layer 3 contributes
  ≤ `0.5 * 0.5 * 0.5 = 12%` and layer 4 ≤ 6%. So cap 2-4 layers when
  translucent α is the typical grid range; expose the setting and measure
  in Phase 1.
- **Danger**: if a *near* layer is nearly opaque (`α≈1`), capping the far
  layers is wrong (they're hidden — fine) but capping the *near* layer is
  fatal. Only cap from the **far side**.

### 5.3 Thread/state hazards
- The sorted draw list must be **snapshot copied** (PODs), never mutated
  while `renderAlpha` is mid-iteration. Reuse the publish/double-buffer
  pattern; abort (fail open) if the generation changed.
- HZB reuse: the alpha skip reads the **opaque** HZB; keep the opaque HZB
  separate from any future translucent operations. Never sample a
  half-built mip.
- `renderAlpha` may run on a different *frame* than the cull — gate the
  skip on the same `mJobInFlight`/generation that the verdicts use.

### 5.4 Scope discipline
- Phase 1 + 2.1/2.2 are safe, pure quality, and independently shippable.
- Phase 3.1 (alpha-behind-opaque) is the perf knob worth doing first given
  the layer-abuse complaint — it reuses the HZB and is fail-open.
- Phase 3.2 (layer cap) needs Phase 1's heatmap first or the visual bugs
  are unseeable.
- Phase 4 (threaded sort + state batching) only after 1-3 prove value.

---

## 6. Suggested sequence

1. Phase 1 instrumentation (debug alpha stats + heatmap).
2. Phase 2.1 global back-surface sort + 2.2 translation dirtying
   (off the Ghillie pool in Phase 4).
3. Phase 3.1 alpha-behind-opaque via the Ghillie HZB (default off until
   measured).
4. Phase 3.2 layer cap + 3.3 minAlpha (gated by the debug heatmap).
5. Phase 4 threaded sort + batching, budget-tuned per camera mode.

Everything fails open, every new setting defaults to the current behavior,
and the entire alpha pipeline becomes as observable as the occluder view —
so dense-layer abuse can be reviewed on the same screen where Ghillie
already shows what it culls.