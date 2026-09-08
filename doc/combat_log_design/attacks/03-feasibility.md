# Attack on Entry 03 — Officer Workflows (ANALYSIS + FEASIBILITY lens)

Reviewing against: the brief's stated data facts (DAMAGE has no positions, objects may be unknown,
team membership is inferred, LOS has travel-time false positives), the XUI/overlay medium
constraints, and the engineering plan's five fixed analysis modules.

## Fatal: quantities with no stated computation path

1. **"Awareness while occluded" (behavioural anomaly, §6)** — defined as "target changed aim/movement
   toward an attacker with no LOS in the preceding window." The brief's data facts give body position,
   velocity, **yaw**, and boolean `AGENT_MOUSELOOK`/sitting/flying flags for every avatar via the
   bridge tick, plus finer viewer object updates for in-range avatars. None of these carry a pitch or
   camera-look vector for *other* avatars — only your own client knows your own camera direction, and
   that isn't broadcast. "Changed aim toward an attacker" cannot be computed from yaw alone (yaw is
   body heading, not aim), and there is no data source anywhere in the brief for another avatar's aim
   vector. This flag has no computation path as written; at best it can be downgraded to "body turned
   to face" from yaw, which is a materially weaker and more error-prone signal than "aim."

2. **"Reaction-time percentile ... relative to the whole session's distribution" (§6)** — defined as
   time from "becoming visible-and-targetable" to first return fire. "Visible" can be approximated via
   the LOS raycast machinery (attacker-eye to target-chest, run for arbitrary avatar pairs, not just the
   officer's own viewpoint — this reuse is plausible). But "targetable" additionally implies weapon
   range and aim-cone, and range/cone data for a given weapon is nowhere in the brief's data facts
   (equipment records give name/creator/owner/attach point/classification evidence, not a
   range or falloff). The entry asserts this metric can be built and turned into a session-wide
   percentile without ever stating where "targetable" comes from. As written this is an invented input.

3. **Named "Fronts" ("west choke 22:10-22:45", "courtyard 23:00-23:20") in the Overview screen
   (§4) and reused unhedged in the Q1 walkthrough** — the brief's data facts contain no region place
   names, parcel names, or landmark data of any kind; only combat log events, avatar tracks, and world
   raycasts against geometry are listed as available. A spatial-density cluster of damage/death events
   over a time window is plausible to compute (damage line midpoints binned in space and time), but a
   *human-readable place name* for that cluster ("west choke") has no source. The wireframe presents
   this as tool output rather than an officer-supplied label on an auto-detected cluster. This is the
   clearest case in the entry of inference (or outright invention) presented as fact: every other
   uncertain quantity in the doc gets a confidence chip; this one doesn't even get flagged as
   officer-authored.

4. **"Combat-zone time 61%" in the Case Binder scorecard (§4, Promotion Review)** — no formula is
   given anywhere in the doc (unlike every other scorecard number, which maps to a named computation
   in §6). What "zone" means (near any damage event? near the session's spatial centroid? near a
   detected front — which itself has no computation path, see #3) is left undefined.

## Serious: presents an unproven algorithm as a settled mechanism

5. **Team-confidence "single posterior, re-threshold instead of re-cluster" claim (§2 rung 2, §6, and
   the Team Ledger screen)** — the entry states the confidence slider "re-thresholds the same posterior
   live, it does not recompute clustering, so sweeping the slider is a Rung-2 view, not a re-run," and
   shows the Ledger going from 3 clusters to 2 as the slider moves. A single scalar posterior per
   avatar can only ever support a **binary** split (in/out of a fixed reference cluster) by
   thresholding; it cannot reproduce a different *count* of clusters (2 vs 3 vs 6) purely by moving a
   threshold on a single number per avatar. Yet the FFA walkthrough (§7) explicitly requires exactly
   that: "clusters count '6 (was 4, was 9)' as the officer scrubs." Either (a) the real representation
   is a full pairwise affinity graph or a per-avatar distribution over K clusters, which is a much
   bigger data structure and a genuinely different (and non-trivial-to-implement-live) algorithm than
   "one posterior," or (b) each threshold change actually re-runs clustering, contradicting the
   entry's own claim that it's cheap and non-recomputing. As written the mechanism is asserted rather
   than shown to be buildable, and the two illustrative examples in the doc (Team Ledger, 2↔3 clusters)
   and the FFA walkthrough (4↔6↔9 clusters) are mutually inconsistent about what the underlying
   representation even is.

6. **FFA per-scrub-window re-clustering (§7, FFA scenario)** — "clustering churns every few minutes as
   damage graphs shift" implies clustering is being recomputed against a moving time window as the
   officer scrubs the timeline, not once over the whole session. The engineering plan's team-clustering
   module (label propagation over a damage-weighted graph, "manual override... persists for the
   session") reads as a single whole-session pass, not a windowed, scrub-driven recompute. Re-running a
   graph clustering algorithm at interactive (scrub) rates is a materially different performance and
   implementation ask than the plan covers, and the entry doesn't address whether it's incremental,
   cached per time-bucket, or genuinely re-run per frame — a real gap given the brief's explicit
   "views must be computed incrementally or on demand" performance constraint.

## Minor

7. **Ctrl-click as the "pin to case" gesture on in-world markers (§5)** — the engineering plan's own
   click hook explicitly consumes only "plain left-click (no ALT/CTRL, so alt-cam is untouched)"
   because SL camera navigation uses ALT (orbit) and CTRL+ALT (pan) as modifiers during exactly the
   alt-cam mode this tool targets. A non-drag Ctrl-click may not collide in practice, but the entry
   asserts it works without checking against the plan's stated reason for avoiding those modifiers.
   Needs verification against actual camera bindings, not assumed.

8. **UNKNOWN equipment not represented in any Dossier/Ledger mockup** — the brief is explicit that
   objects are "sometimes unknown." Every screen wireframe shows fully-named weapons (Ashguard Mk.II
   Carbine, Ghost Slug); none show how a Weapon Dossier, Roster row, or Team Ledger cell renders an
   unresolved `source`/`rezzer` key. Not fatal (the plan's classification module already carries an
   UNKNOWN kind with evidence), but the entry's own screens never exercise it, so the "honest
   uncertainty" bar isn't demonstrated for the case the brief calls out by name.

9. **Generic single-class Dossier for four noun kinds** — the entry itself flags this as an open risk
   (§8, "Generic Dossier risk"), which is the correct call; no further attack needed beyond noting the
   self-identified risk is real and likely under-resourced relative to how much screen real estate the
   four kinds' differing stat tables would actually need.

## Buildability check: screens and overlay (the parts that hold up)

All screens in §4 (Combat Log hub, Team Ledger, Dossier, Inspector, Case Binder, Export Summary) are
composable from immediate-mode XUI primitives already used elsewhere in the codebase pattern
(scroll lists, tab containers, sliders, text editors, custom-drawn `LLView` grids/ribbons) — nothing
requires a web view, and nothing requires client-side layout beyond what floaters already do. Per-item
costs are all bounded by session scale (≤~100 avatars, a handful of open Cases, a few dozen pinned
cards per Case), well inside "compute on demand" even without caching. The LOS-sweep slider in the
Inspector (§4) is cheap: one incident, a handful of raycasts across a short travel window, recomputed
only while that one floater's slider is being dragged — this is a legitimate Rung-2 view. The overlay's
tack-glyph pin indicator is a trivial additional draw call per already-drawn marker (a small icon plus
one colour lookup) and doesn't add new picking complexity beyond the plan's existing screen-rect list.
Breadcrumb/backlink navigation is cheap because it's computed lazily, only for the handful of nouns an
officer actually opens, exactly as the entry claims.

## What must be kept

- The **Case Binder / Evidence Card / Pin** mechanic as the workflow spine: it is the one part of the
  entry that most directly answers "what does an officer produce," is cheap to implement (a card is a
  snapshot of an already-computed fact plus a permalink), and gives the tool a natural stopping point
  other entries may lack.
- **Snapshotting a pinned fact instead of a live reference** (§8, "Threshold-sensitive team scenes") —
  freezing the Ledger/threshold state into a card at pin time is the correct fix for the exact
  inconsistency raised in finding #5; it doesn't solve the clustering-representation problem but it
  does stop an exported Case from silently changing meaning.
- **Never emitting a manufactured "cheating: yes/no" verdict** (§6, behavioural anomaly flags) and the
  **officer-only Verdict picker** in Violation Investigation cases — this is the strongest, most literal
  compliance with the brief's "never a hard fact" and "evidence not verdicts" requirements anywhere in
  the entry, and should survive into any merged design regardless of what happens to the anomaly
  metrics themselves.
- The **generic Inspector / Dossier breadcrumb + related + backlinks chrome** as the shared navigation
  skeleton (independent of whether Dossier stays a single parameterized class or splits by noun kind).
- **Disputed-if-flips-across-threshold-range** as the honesty mechanism for friendly-fire flags (§6) —
  a genuinely good pattern (show instability itself as the uncertainty signal) that should be kept even
  if the underlying clustering representation (finding #5) needs to change.
