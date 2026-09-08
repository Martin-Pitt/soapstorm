# Completeness critique — `doc/combat_log_ux.md`

Reviewed against `doc/combat_log_design/BRIEF.md` and the engineering plan
(`glistening-toasting-pebble.md`). The document is unusually disciplined — every finding below is a
genuine hole or an unflagged departure from the fixed plan, not a style note.

## Gaps

### 1. [Serious] Three of five R3 Field faces have no wireframe
The brief's deliverable format requires, for every screen, "an ASCII wireframe plus a description of
every region of it." §3.6 gives full wireframes for **Matrix** and **Braid** only; **Front**,
**Roster** and **Board** (and the R4 "Ground" toggle, partially excused by reuse of the Plan/Chart
wireframe) are prose-only. Roster in particular is load-bearing for Q8 (the most misuse-prone
surface in the tool) and its "drag the filter from 2× to 4× median" interaction (used verbatim in the
Q8 walkthrough) is never shown as a control anywhere.
**Fix:** add ASCII mockups for Front, Roster and Board at the same fidelity as Matrix/Braid — column
layout, the filter/threshold control referenced in the walkthrough, and the small-multiple cell grid
for Board — so `SSStageField`'s five painters are specified equally.

### 2. [Serious] `d_spawn` and "spawn-cohort affinity" have no computation path
The FFA walkthrough (§6, "FFA on a neutral sim") says: "The respawn test fails, so `d_spawn`, spawn
rings and the spawn-cohort affinity term all switch off with a `no-respawn` chip." None of `d_spawn`,
the "spawn-cohort affinity term," or the "respawn test" it depends on appear anywhere in §5 (Analysis
model) — the section the brief requires to state "how each [derived quantity] is computed from the
available data, and how its uncertainty is represented." As written, this is a quantity the UI and a
walkthrough depend on that nobody has a formula for.
**Fix:** add a §5 entry: spawn zones = DBSCAN clusters of teleport-heuristic landing points (§5.7);
`d_spawn(avatar, t)` = distance to nearest such zone; the affinity term is a decayed edge-weight boost
in §5.9's seeding for avatars co-located near the same zone in a shared time window; the "respawn
test" is the precondition (e.g. ≥N teleports clustering within R m) gating all three, mirroring the
group-tag admission tests already specified for §5.9.

### 3. [Serious] World-overlay navigation may not push the View chain — the one place digging is most likely to lose the thread
§2.5 states the load-bearing invariant: "Anything that moves the cursor or a parameter pushes the
previous View first, so a multi-hop violation dig ends with one click back to the framing it started
from." §4.6 (the pick model) specifies `Shift+click` descends a rung, `Ctrl+click` pins, and clicking
bare ground opens a Zone page — but never states that these overlay-initiated moves push the View
chain the same way floater-initiated ones do. Since the tool's entire selling point is that the
officer works by alt-camming (per the owner's vision) rather than by clicking through floaters, this
is exactly the interaction where "digging loses the thread" would first be noticed if the invariant
doesn't actually reach it.
**Fix:** add one sentence to §4.6: every overlay-initiated rung/subject change pushes the View chain
identically to a floater-initiated one; Peek-card hover (transient, no navigation) explicitly does
not.

### 4. [Serious — conflicts with the engineering plan] Two "already decided" analysis outputs are silently redefined
The brief says the plan's five analysis modules are fixed and "do not redesign." Two places quietly
change what the plan says those modules return, without being listed as deltas the way §7 correctly
flags the three Phase-1 changes (`t`-per-event, `CombatObjectInfo` position, raw-line arena):
- **Death attribution / killing blow.** Plan: "killing blow = last DAMAGE (also grafts type/damage
  onto the death...)" — a deterministic field. Doc §5.6: last event counts as the killing blow *only
  if* its margin exceeds combined timestamp uncertainty, otherwise the article reads "ambiguous
  between X and Y" and kill credit is split. This is a better design, but it changes the shape of the
  attribution output (single killer → killer-or-ambiguous-pair) that `sscombatanalysis.cpp` and the
  kill-feed grafting logic in the plan assume.
- **LOS verdict.** Plan: three-state `CLEAR / BLOCKED / UNKNOWN`. Doc §5.11: four-state, adding
  `MARGINAL` as "a first-class outcome." Same story — a good addition, an unflagged contract change.

**Fix:** add both to §7's build-order delta list next to the three Phase-1 deltas, with the exact
enum/struct changes needed (`CombatEvent`/analysis-result shapes) so the engineering side isn't
surprised mid-Phase-5.

### 5. [Minor] Occlusion point cloud has no retention rule
§4.5's occlusion point cloud ("every BLOCKED raycast's hit point... accreted into a session spatial
index") is the only store in the whole design with no stated cap or eviction, unlike events, tracks
and the R−1 raw arena, all of which get explicit retention sweeps or hard caps (§5.21, plan §"Data
model"). Over a 2 h session with an officer actively sweeping LOS across many deaths, this can grow
without bound.
**Fix:** cap it — reuse the retention-minutes sweep, or a fixed point budget with LRU-by-cell
eviction — and state the number, matching the discipline applied everywhere else.

### 6. [Minor] "Structure index S" is used but never defined
S drives the DEATHMATCH threshold, the Braid's collapse into a hostility heat strip, and the Front
face's refusal to draw — a load-bearing derived quantity — but §5.9 only says it is tested "against a
null model from 20 size-preserving random partitions," never naming the metric itself (e.g. graph
modularity of the current partition). An implementer has no formula to build from.
**Fix:** name the exact statistic (state it is graph modularity, or whatever it actually is) in §5.9.

### 7. [Minor] Pick-model refinements aren't logged as a plan delta
§4.6's mouse-up/4 px/350 ms debounce and the pick-stack disambiguation list extend the plan's Phase
4.4 ("consume plain left-click... select + open inspector") but, like items in finding 4, are not
listed in §7's delta list.
**Fix:** fold into the same delta list as finding 4 for a single place the engineering side checks.

### 8. [Minor] Zone-page fallback undefined pending owner answer
Open Question #8 correctly asks the owner whether Zones get officer-given names. But §2.3/§4.1
already depend on Zone pages existing today (clicking bare ground → "that Zone's page"), and the doc
never states what that page looks like if the owner says no names. A reader implementing before the
owner answers has nothing to build.
**Fix:** state the fallback inline — e.g. `ss:zone/12,-4` coordinate-keyed page, unnamed — so the
open question only affects *labelling*, not whether the page exists.

## Strengths to preserve

- The two-history model (Noun chain vs View chain) plus per-`(rung, subject)` anchors is a genuinely
  complete answer to "digging that loses the thread" — it is the strongest single idea in the
  document and should not be simplified away in implementation.
- Honest uncertainty is enforced structurally, not just stated: the haze rule, the two kinds of
  number (interval-with-n vs ordinal band), MARGINAL as a first-class LOS outcome, and "a view that
  silently drops glyphs is a lying view" for LOD. This directly answers the brief's "uncertainty
  presented as fact" risk.
- The seed-admission tests + mandatory tag-free second solve + STRAINED/`(!)` flags correctly solve
  the intra-group-skirmish scenario the brief calls out by name, and the structure-index/DEATHMATCH
  degrade path correctly and gracefully handles the FFA/neutral-sim case without inventing team
  labels that don't exist.
- The Chart/World parity rule, backed by a single shared `SSCombatDrawList`, is the correct fix for
  "overlay unusable while alt-camming isn't possible" (walking, briefing) — enforced as a shared data
  structure rather than a convention that will drift.
- §5.21's cost/caching contract (O(1) accumulators on append, bounded raycast budget with LRU memo,
  debounced front extraction, article LRU with epoch invalidation) is a concrete, specific answer to
  the 2 h/100k-event/millions-of-samples performance constraint, not a hand-wave.
- Walkthroughs cover all nine officer questions plus both required scenarios (intra-group skirmish,
  FFA) plus a bonus mid-fight-arrival case, exceeding the brief's minimum (Q1, Q3, Q8 + two
  scenarios).
- Delivery-method classification (hitscan vs projectile vs area, §5.5) correctly gates the
  travel-time window per-event rather than globally, honoring the SLMC domain facts about mixed
  weapon delivery methods coexisting in one raid.
- Object/LBA handling (§2.1, §3.3) is honest: destroyed turrets get "unknown," never an inferred
  death, matching the brief's explicit LBA-invisibility constraint.
