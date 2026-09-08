# Attack on Entry 02 — Hypertext-first — ANALYSIS + FEASIBILITY lens

## Summary verdict

The information architecture (addresses, page skeleton, backlinks, sweep facet, hedged prose)
is well-formed and mostly buildable in XUI. The analysis model is where the design breaks: one
derived quantity (**Restraint**) has no bounded computation path at the stated scale and it sits
underneath Team clustering, which is the load-bearing feature the whole "intra-group skirmish"
and "friendly fire" story depends on. Two more quantities (**reaction latency**, **pre-aim
fraction**) reuse the same unbounded pattern in miniature. The entry also states, as settled
fact in §3, a cross-viewer link-sharing capability that its own §8 admits may not work — that is
inference dressed as a shipped feature, which is exactly the failure mode the brief asks the
tool to avoid committing on other people's behalf.

## 1. Quantities with no (stated) computation path at the given scale

### Restraint(a,b) — fatal as specified
> "fraction of opportunities (LOS CLEAR, range <40 m, ≥2 s) with zero damage a→b"

This is defined over **every pair of avatars**, not just pairs that exchanged damage — that's
the whole point, restraint is supposed to catch pairs who *never* shot each other. The
engineering plan's LOS system is built for on-demand candidate casting anchored to a single
event (≤7 rays per death/hit, computed lazily when a page is opened). Restraint instead requires
knowing the CLEAR/BLOCKED verdict at (in principle) every tick for every pair over the whole 1–2 h
session: with 100 tracked agents at 2 Hz for 2 h that is ~14,400 samples/avatar, ~5,000 pairs,
and on the order of 10^7–10^8 `lineSegmentIntersectWorldGeometry` calls if computed naively and
un-downsampled — several orders of magnitude beyond anything else in the plan (which budgets
raycasts in single digits per event). Nothing in §6 or §8 proposes a sampling interval, a
proximity pre-filter, or a "only pairs that were ever within engagement range" cutoff. As written
this quantity cannot be computed incrementally or on demand; it would have to be computed
continuously for the full N² pair space, which the brief's performance constraint explicitly
rules out ("Views must be computed incrementally or on demand").

Because **Team clusters = label propagation on hostility − λ·restraint − μ·co-movement**, and
restraint is the term specifically justified as the fix for "friendly fire must not be misread as
different team" and for the intra-group-skirmish scenario, the entire team-detection story —
which the walkthroughs lean on heavily (Q2, the intra-group skirmish, the FFA fallback) —
inherits an unbounded, unimplementable dependency. This is not cosmetic; it is the feature the
owner asked for by name ("detecting distinct groups/teams although mind friendly fire").

Fix sketch the entry should have included but doesn't: restrict restraint to pairs that were ever
mutually within, say, 60 m for ≥2 s (a cheap proximity index, not a raycast), and only raycast at
a coarse fixed interval (e.g. once per 5–10 s of co-presence) rather than every tick. That reduces
the candidate set by orders of magnitude but the entry doesn't say so — it presents the formula as
if it were already tractable.

### Reaction latency — same shape, smaller blast radius
> "first CLEAR LOS onto target → first damage onto that target"

This requires scanning backward from the hit event through the attacker's track until the first
tick at which a raycast to the target resolves CLEAR. No search bound is given. If LOS had been
clear for minutes before the shot (attacker was simply not looking, or waiting), the search either
runs unbounded or silently caps at an unstated window — the entry doesn't say which, so the
number shown to the officer ("reaction p50 0.62 s") is not reproducible from the stated spec. This
is fixable (bound the search to, say, the preceding 10–30 s and mark as UNKNOWN past that), but as
written it's a gap, not a design decision.

### Pre-aim fraction — same shape, ambiguous scope
> "share of time yaw is within 8° of a target while LOS is BLOCKED and that target is later hit"

If this is meant to run only over the actual attacker/eventual-victim pair for a bounded
pre-engagement window (which the walkthrough's "12 engagements" implies), it's tractable — one
pair, one window, a few dozen ticks. If it's meant to run for *any* target later hit by *any*
avatar (i.e. "was anyone dry-aiming through a wall at anyone before hitting them"), it degenerates
into the same all-pairs LOS problem as Restraint. The entry never states which, so a reviewer
cannot confirm it's buildable — it reads as a formula, not a computation plan. This is exactly the
kind of "no computation path" gap the brief warns about: the row in the table looks rigorous
(named formula, named uncertainty caveat) but the actual cost is undetermined because the domain
of the sum is unspecified.

## 2. Inference presented as fact

- **§3, cross-session link sharing**: "an officer can paste a link into group IM and another
  Soapstorm user in the same session clicks it and lands on the same page." This is stated as a
  working capability in the architecture section. §8 then admits addresses are session-local
  (`seq` is per-viewer) and that landing on the same page "only resolves if their session
  recorded the same events" — which, given per-viewer bridge polling, log-batch truncation, and
  independent drop/dedup (all described in the engineering plan), is not guaranteed even for two
  officers who were both present the whole raid. §3 oversells; §8 quietly walks it back. A design
  document should not assert a capability in its architecture section that its own risk section
  says may not hold — that's the "inference as fact" pattern applied to the tool's own
  functionality, not just to combat data.
- **Death page lede** (§4.1 wireframe): "Nyx Ashgrave was killed by Corr Vell (Blue, conf 0.81)
  with a bullet from Tanto SMG." The killer identity here is not itself hedged (only the team
  colour and weapon get confidence markers). But "who is the killer" is an attribution choice —
  killing blow = last DAMAGE in the window — and that heuristic can be wrong when two attackers'
  damage windows overlap (e.g. a second shooter's hit lands in the same 20 s window and happens to
  be the temporally-last event before death, but a third party's earlier hits did the real
  damage share). The design shows "share %" per contributor in the Facts box, which is good, but
  the prose sentence still states the killer as bare fact with no confidence chip, while
  formally the attribution set is a probabilistic grouping (stated explicitly as such in §6:
  "log-lag estimate per event"). This is a small but real instance of the exact failure mode the
  brief calls out.
- **Restraint and co-movement feeding team confidence**: the walkthrough numbers ("confidence
  0.58", "threshold 0.4 gives 11/11") are presented as if derived from a real, running
  computation. Given Restraint has no computable implementation as specified, these numbers are
  currently fiction dressed as worked example — acceptable for a design doc's illustrative
  walkthrough, but the entry doesn't flag that the underlying pipeline needs to exist first, which
  a less careful reader could mistake for "this is how it will behave."
- **LOS verdict confidence doesn't visibly account for track quality.** The overlay design
  correctly draws COARSE-location uncertainty as a translucent sphere (±2 m in z), but the
  analysis-model LOS Verdict computation (§6 row) never says candidate-position quality (COARSE
  vs BRIDGE vs VIEWER) feeds into the CLEAR/BLOCKED confidence. A ±2 m position error next to a
  wall is exactly the case that flips a verdict; the "Model" page's through-wall Wilson interval
  would then be silently computed over verdicts of mixed, unstated reliability — an inference
  (verdict) being reported with a confidence interval that doesn't include the actual dominant
  error source.

## 3. XUI / 60 fps / scale feasibility, screen by screen

All screens are, in principle, buildable from `LLTextBase` + `LLAccordionCtrl` + custom `LLView`
drawing, matching the brief's constraints. No screen requires a web view. Specific checks:

- **Reader (§4.1)**: read-only rich text with `parse_urls`, accordion sections, custom figure
  `LLView`. Feasible. The entry's own §8 flags the real risk correctly (many URL segments in
  `LLTextBase` are not free) and proposes a mitigation (lazy accordion build, LRU page cache) —
  this is the right level of self-awareness and should be kept.
- **Split/compare (§4.2)**: two panes, each an instance of the same reader. Feasible; "both in
  world" drawing two subjects' figures simultaneously is just two more polylines, cheap.
- **Session page (§4.3)**: 512 m top-down figure with a few hundred to low-thousand death dots,
  ~1,024 heat cells (32×32 at 16 m over 512 m), phase ribbon, sparkline. All primitive
  line/quad/text draws, computed once per session-recompute (not per frame) and cached until the
  next ingest tick. Feasible at 60 fps.
- **Model page (§4.4)**: "all shots by that model, faint, blocked ones red" — for a popular
  weapon (41 kills / 512 hits) this implies up to ~512 LOS verdicts computed on first page open if
  not already cached. At up to ~7 candidate rays each that's ~3,500 raycasts fired synchronously
  when the officer opens the page — plausibly a visible hitch on click (not per-frame, so it
  won't break 60 fps steady-state, but the entry doesn't say this should be computed
  incrementally in the background or spread across frames as verdicts are needed for display
  lazily rather than all at once). Minor perf risk, not fatal, but unaddressed.
- **Team page (§4.5)**: force-directed graph over ≤~40 nodes. Layout need not be recomputed every
  frame (cache positions, relax occasionally); drawing is circles+lines+text. Feasible.
- **Overlay (§5)**: consistent with the engineering plan's existing gating and draw-cost budget
  (trails, rings, rays, labels — all primitives already budgeted at <0.5 ms for 60 avatars × 20 s
  trails). The addition here is "union of active page and pinned pages," which could add
  additional geometry beyond the plan's budget if an officer pins several aggregate pages (e.g. a
  Model page with hundreds of shot lines) simultaneously with live trails. Not fatal — a few
  hundred extra lines is still cheap — but the entry should say pinned aggregate figures degrade
  gracefully (e.g. cap drawn lines, thin with distance) rather than assuming the plan's existing
  budget automatically covers it.
- **"Gallery" facet**: named as a footer facet on multiple pages but the data has no captured
  imagery (no screenshot/thumbnail pipeline anywhere in the plan). If this means a grid of
  text/colour-swatch cards, it's fine and buildable; if it implies actual visual thumbnails of
  avatars/equipment, there is no data source for that. The entry doesn't clarify, so a reader
  cannot verify buildability of this specific facet.

No screen in this entry requires HTML or a web view. The design correctly stays inside XUI's
immediate-mode + custom-LLView model throughout.

## 4. Is the overlay drawable with lines/rings/text/screen-rect picking?

Yes, for everything actually specified: trails, alpha ramps, width buckets, yaw cones, eye/crouch
glyphs, death crosses, candidate-ray fans with CLEAR/BLOCKED colour and dotted wall-continuation,
heat cells as quads, front polylines with arrowheads, spawn rings, halo-highlighting of
mentioned-elsewhere markers. All of it maps onto primitives already exercised by the engineering
plan's Phase 4 (lines, rings, filled quads, `hud_render_utf8text`). Screen-rect picking per marker
via `LLViewerCamera::projectPosAgentToScreen` is the same mechanism the plan already specifies.
The one item worth a caveat: "halo" highlighting of every marker the open page's text mentions
requires the overlay renderer to know, per frame, the current page's full reference set — cheap if
that set is small (a death page mentions a handful of avatars/objects) but the entry doesn't cap
it for aggregate pages (a Team page's figure could reference 18+ avatars, 500+ events) — still
almost certainly fine at these counts, but unbounded in principle if an officer pins a query
page with thousands of matches ("wall-kills" query, "anomalies" query). Should state a cap.

## 5. What must be kept

- The address grammar plus reuse of the existing `secondlife:///app/...` / `LLUrlEntryBase`
  mechanism for clickable links inside `LLTextBase` — real, already-proven viewer capability,
  elegant fit, and it satisfies the brief's "digging" requirement without inventing new UI
  machinery.
- One page skeleton, one reader, reused as the plan's inspector floater and as "sub floaters" via
  a second reader instance — directly answers "there may be need for different sub floaters"
  without multiplying bespoke screens.
- The hedge-word style rule tied to the confidence chip and the mandatory `why` page for every
  inference — this is the correct, disciplined answer to "present allegiance as inference with
  confidence, never a hard fact," and it's enforced structurally (chip = link), not just by
  convention.
- The sweep facet as one mechanism reused for every tunable claim (LOS travel window, team
  threshold, adjustment scripts) — a genuinely unifying idea that matches the brief's "Rung 2"
  requirement without a bespoke UI per parameter.
- The death reconstruction figure correctly uses only what DEATH actually provides
  (`source_pos`/`target_pos` at travel-window candidates) and never invents positions for plain
  DAMAGE events — it is careful about the brief's core data constraint even where other parts of
  the entry (Restraint) are not.
- The "n facts known of 6" honesty pattern for partially-unknown equipment, and UNKNOWN-with-
  evidence classification — matches the brief's "objects may be unknown" requirement precisely.
- Section 8's self-audit (text-widget cost, address stability, backlink memory, auto-prose
  overconfidence, clustering churn, override conflicts) is the right list of risks and should
  survive into any merged design — it is more honest than the body text it's attached to, and the
  fix is to make the body match the caveats, not to drop the caveats.

## 6. What must change before this entry is buildable as specified

1. Give Restraint(a,b) an explicit, bounded computation path (proximity pre-filter + coarse
   fixed-interval sampling, not per-tick all-pairs raycasting) or drop it from the team-clustering
   formula and rely on hostility + co-movement + explicit officer overrides, with restraint
   demoted to an on-demand, per-pair "check restraint for these two" tool rather than an
   always-on clustering input.
2. Bound reaction-latency and pre-aim backward searches to a stated window, and state explicitly
   whether pre-aim is computed only over pairs with an actual later hit (tractable) or over all
   pairs (not tractable).
3. Reconcile §3's cross-viewer link-sharing claim with §8's admission that it may not work —
   either state it as an open problem in §3 too, or scope the claim to "same viewer session /
   same officer" until content-hashed addresses exist.
4. Add a confidence/quality term to LOS verdicts that reflects candidate track-sample quality
   (COARSE vs BRIDGE vs VIEWER), so through-wall rates don't quietly average over
   position-uncertain and position-precise verdicts alike.
5. State a cap or lazy/background computation strategy for Model-page aggregate LOS verdicts
   (hundreds of raycasts on first open) and for overlay halo-highlighting on aggregate/query
   pages with large reference sets.
