# Attack on Entry 11 (Comparative: the raid as a grid of differences) — lens: ANALYSIS + FEASIBILITY

## Verdict up front

The rendering layer is genuinely buildable: every screen and every overlay layer described (Grid of cells, Sweep dock, Board, τ-fan, front ribbon, lattice, stroboscope) reduces to lines/rects/text/rings drawn by custom `LLView`/pipeline code with screen-rect picking, exactly the primitives the medium offers. Nothing here needs a web view. That is a real strength and should survive.

The analysis layer is where the entry oversells. Several derived quantities are computed by silently assuming facts the brief says are unavailable or by chaining two inferences and reporting the result as if it were one measurement. Two of these (front field, spawn anchor) sit under headline officer-facing claims in the walkthroughs. A third class of quantity (reaction time, pre-aim-through-wall) has a computation path in principle but no performance budget, unlike the one quantity (τ-sweep) that the entry does budget carefully — which makes the omission look like it was noticed and dropped rather than missed.

## Computability check, quantity by quantity

**Fine, computable as described:**
- Death attribution (damage window grouped by owner+rezzer) — uses only fields DAMAGE/DEATH actually carry.
- LOS τ-sweep, flip fraction, coverage — correctly built from track samples + `lineSegmentIntersectWorldGeometry`, and it is the one place the entry budgets the raycast cost explicitly (~78 casts/event, capped/frame, lazy). This is the model instance of "compute a derived quantity honestly" in the entry.
- Team clustering across K×θ, allegiance stability, friendly-fire rate with its own spread bar — correctly built from a damage graph plus group-tag seeding; explicitly presented as a distribution over a parameter, not a fact. This is the strongest piece of the design.
- Adjustment waterfall (`damage/initial`, per-script delta) — directly from `modifications`.
- Wilson interval / caterpillar plot per weapon — standard, computable once kill+wall-verdict counts exist.
- Engagement segmentation by time+position (using *track* position, not the nonexistent DAMAGE position) — correctly sources position from the victim's track rather than from DAMAGE, which is the trap the brief sets ("DAMAGE has no positions"). The entry does not fall into it here.

**No real computation path, or inference presented as fact:**

1. **Front field `F(x)` "weighted by team confidence."** Team confidence is itself a function of θ (§6, team clustering is explicitly a family of clusterings, one per θ). The front-field formula never says which θ's confidence it uses. Sweeping σ is offered, but θ is not swept for the front field, so a specific, unstated team assignment is baked into the "unbent" front axis that the entire R4 rung and the front ribbon overlay are built on. As written this is not computable without an additional undocumented decision (pick θ=?, or re-derive F per θ and pick a mode) — a missing parameter, not a missing algorithm, but the walkthrough (Q1) reports "collapses 28 m in 90 s" as a fact with no hedge, when it rests on an inference-of-an-inference.

2. **Spawn anchor / `d_spawn` / "sawtooth Sparkpath" respawn cycles.** The computation is "cluster each team's post-respawn positions; post-respawn = first sample ≥3 s after a death." This assumes death → teleport-to-spawn is how the region works. The brief explicitly lists "the region's own game rules" as *not available*, and nothing in the data (DEATH has no "respawn" event, no HP field, no teleport marker) tells the tool whether the victim's avatar actually relocates on death, ragdolls in place, or the sim's game logic does something else entirely. The entry states this as if it were a safe derivation ("Respawn cycles show up as sawtooth Sparkpaths — the shape that answers 'how much time was spent walking back'") without ever naming the assumption or its failure mode (e.g. a sim with no forced respawn would produce spawn anchors that are just noise, silently misread as "someone camped spawn"). This needs a stated precondition and a degrade-to-hatched behavior symmetric with what the entry already does for the front field ("refuses to draw it rather than draw it wrong") — currently it does not extend that discipline here.

3. **Reaction time / pre-aim-through-wall.** Both require sampling LOS (attacker→victim, yaw-within-cone, blocked/clear) at every track sample across a 5–10 s pre-event window, for every damage/death event a Rows=Avatar Grid needs (per the Q8 walkthrough, 37 avatars). That is potentially thousands of raycasts per avatar, with no cap, no lazy/on-demand phrasing, and no mention in the raycast-cost risk paragraph, which only budgets the death τ-sweep and the whole-weapon tally. Given the entry's own admission that a *single* weapon-wide tally is "tens of thousands" of casts, an unbudgeted per-sample scan for two more behaviour metrics across the full roster is a real 60fps risk that the entry doesn't surface — it looks dropped, not solved.

4. **Equipment identity used for aggregate claims ("M4 kills 34 (wall 4)", the wall-record caterpillar).** The brief says object facts, and hence weapon identity/classification, can be unknown or only partially resolved, and object *names* are user-editable/spoofable — nothing in the data model attaches trust to a `name` string. The entry's caterpillar plot and "Compared with" rollups group by weapon apparently by name/type with no visible path for (a) what happens when the same displayed name is worn by unrelated objects, (b) discounting a weapon's `p_wall` by the confidence of its own classification. This is exactly the officer question ("which equipment kills through walls") the tool is supposed to answer carefully, so a silent full-trust assumption on name-based grouping is a real gap, not a cosmetic one.

## Screens needing a web view

None. Grid cells, Sweep dock rows, Board tiles, and the inspector's tables/waterfalls/ribbons are all achievable as scroll lists, standard XUI controls, and custom-drawn `LLView` panels using lines/rects/text — consistent with the brief's stated constraint set. This is worth keeping as a validated claim.

## Buildability at 60 fps / 100k events / millions of samples

- The Grid's own performance story (index once per Query change, cache cell geometry, invalidate only on cursor/filter change, cap at 60 visible cells) is a coherent incremental-computation plan and matches the brief's requirement. Not fatal.
- But the stated cell cap (60) is smaller than what the entry's own default Query can produce: Rows=Engagement (an uncapped, session-length-dependent count — a busy 2 h raid could segment into well more than 10) × Cols=Team (≤5) can exceed 60 easily; the entry never says what happens past the cap (silently drop rows? force re-facet?). Minor but real.
- The claim "why the tool can be deep with only four custom LLViews" (§3) is contradicted by the entry's own screens, which require at least Map, Sparkpath, Waterfall, Matrix, Band/Ribbon, Fan, and caterpillar/Wilson-interval rendering — seven-plus distinct custom drawing routines, not four. This doesn't break feasibility (all seven are still just primitives), but it understates the actual engineering surface, and a plan that under-costs its own component count is a rigor problem worth flagging before someone estimates the work from this document.
- The front-ribbon isoline (marching-squares-style extraction over a continuous Gaussian-sum field, redrawn with faded history every 10 s) is more expensive per redraw than the entry's other overlay layers and is not covered by the entry's performance paragraph (which only speaks to Grid caching), though it is gated behind stationary+alt-cam so live 60fps combat pressure is not a concern — mainly a scrub-drag risk.

## What must be kept

- The τ-sweep + flip-fraction treatment of LOS verdicts (§6, §7 Q3) — this is the correct, brief-mandated answer to "travel-time false positives," and it's the entry's best-executed idea.
- The θ-swept team clustering with allegiance-stability and a spread-bar friendly-fire rate, plus per-avatar officer override with a lock glyph — directly satisfies "present allegiance as inference with confidence, never fact" and the intra-group-skirmish scenario.
- The uniform uncertainty-encoding legend (dotted/desaturated/hatched/dashed, applied identically in 2D and 3D) — this is the kind of single, consistent honesty mechanism the brief's "everything is uncertain" constraint calls for.
- The "Compared with" column and peer-band presentation for behaviour metrics, with a printed confound list and no printed verdict about a person — matches Q8's explicit instruction to show evidence and uncertainty, not accusations.
- The finding, engineering-feasibility-wise, that no part of this design requires anything beyond XUI widgets and custom LLView drawing — that constraint is satisfied throughout.
- The entry's own honesty about front-field fragility, clustering over-fit, and raycast cost in §8 — these are correctly identified risks; they should be extended to the two gaps this review adds (spawn-anchor precondition, reaction-time raycast budget) rather than treated as separately solved problems.
