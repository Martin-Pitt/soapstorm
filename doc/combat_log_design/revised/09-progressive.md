# Entry 09 — Minimal Progressive Disclosure (revised)

## 1. Thesis

The tool should look, at rest, like almost nothing: a scrub bar and one sentence of prose. Everything an officer can ask about a raid — teams, deaths, equipment, suspicion — is a *noun* reachable by drilling down, never a panel sitting open by default eating screen and frame budget. There are exactly two windows because the engineering plan fixes two (`ss_combat_log`, `ss_combat_inspector`), but only one ever shows content: the main floater is pure transport chrome (play/pause/scrub, a one-line auto-generated headline, an Inspect button), and the Inspector is a single content pane that behaves like a Wikipedia tab — one page visible at a time, a breadcrumb above it, a backlink strip below it, a pin rail for the handful of nouns you're actively comparing. The in-world overlay is the primary display surface, not decoration: at rung 0 and rung 1 it *is* the answer to "what happened and where," and the 2D UI only earns its keep for facts geometry can't carry (exact numbers, a probability, a list). The discipline is unchanged: if it can be drawn on the world, draw it there; if it must be text, put it one level deep in the Inspector, never in a panel that's always open. What this revision fixes is the edges of that idea: the single stack no longer silently erases a branch you dug into, three thin index pages give a cold-start way into equipment, suspicion, and any specific death, and team clustering plus the LOS/uncertainty rules are made computable and consistent rather than merely asserted.

## 2. Ladder

**Rung 0 — concrete, at the cursor.** The live/scrubbed 3D overlay: head markers, mouselook glyph, an in-flight damage line, a LOS ray if something is selected. *Step up*: press **Trails** (or wait — a short default trail is always drawn). *Step down*: nothing below rung 0; scrubbing is the only motion within it.

**Rung 1 — abstract over time.** Press **Range**: every avatar's entire-session track as one static picture (colour = inferred team, or a per-avatar palette when team confidence is too low to mean anything — §5), plus a death-density heatmap and spawn marker. *Step up*: drag-select a dense knot of trails → snaps to the nearest precomputed **Zone** if the selection overlaps one; otherwise opens a throwaway **Selection** page (same histogram widget, no persistent identity, dropped from history on leave — §3 explains why Zone and Selection are deliberately two different nouns). Picking a peak in either histogram steps back down: cursor moves there, overlay returns to rung 0. *Step down*: click any trail line → cursor jumps to the nearest sample, overlay drops to rung 0, Inspector opens that avatar.

**Rung 2 — abstract over a parameter.** Three sweeps, triggered from the Inspector, drawn back onto the overlay: (a) LOS candidates across the travel window, colour-coded CLEAR/BLOCKED; (b) team-assignment confidence swept across a threshold slider, redrawing cluster membership per time bucket live; (c) damage before/after each adjustment script, as a stacked bar. *Step up*: aggregate a sweep across every event sharing an equipment or avatar → a **Claim** page. *Step down*: click a candidate/bar → jump to that instant at rung 0 with the ray or hit drawn.

**Rung 3 — small multiples and unbent space.** The Team page shows one sparkline per member plus a new team-membership-over-time strip (§3), side by side. The Zone page can unbend: pick two teams and it re-plots every position as signed distance to a straight-line approximation of the front (perpendicular bisector of the teams' bucketed centroids, recomputed per bucket, labelled as an approximation — §6). *Step up* from here is interpretive text only (the headline); there is no rung 4 by design.

## 3. Information architecture

**Nouns, one class of object, one page shape each:**
- **Session** (root) — headline stats, team summary, zone summary, top claims, plus three new browse links: *All events*, *All equipment*, *All suspicion evidence* (collapsed by default).
- **Zone** — a precomputed, persistently-named density cluster of ground; activity histogram; unbend-to-front control.
- **Selection** — an ad hoc rung-1 marquee that didn't land on a named Zone; same histogram shape, no identity, no backlinks, evicted from history on leave. A different noun from Zone, not a second construction path for the same one.
- **Minute/Interval** — a time bucket; what happened, who was where, links to events inside it.
- **Team** — members with confidence *per time bucket* (§6), threshold slider, friendly-fire ledger, equipment rollup.
- **Avatar** — role stats, weapons used, mouselook %, team-history sparkline, collapsed event list.
- **Event** (Death/Damage) — attribution chain, LOS verdict with its caveat always visible, adjustments, friendly-fire check.
- **Equipment** — classification + evidence (or "unidentified object" treatment — §6), users, damage/adjustment stats.
- **Claim** — a generalized assertion with evidence list, confidence interval, permanent caveat.
- **Event / Equipment / Suspicion Index** — three sortable, filterable browse lists, one reused widget with three column sets, closing the cold-start gaps the original entry left in Q3/Q4/Q7/Q8.

**Linking.** Every page ends in a *Related* strip of backlink chips. Clicking a chip **pushes** the target onto the Inspector's single stack; the breadcrumb renders that stack, e.g. `Session ▸ Team A ▸ Kira ▸ Death @14:32:07`. Clicking a breadcrumb segment pops back to it.

**Branching no longer silently loses work.** Popping back to a segment already visited and continuing on discards nothing new, as before. What changes: popping to a *mid-stack* segment and then pushing somewhere **different** from before now auto-pins the discarded forward branch, labelled by its leaf noun (e.g. `↳SMG-X Claim`), instead of dropping it. One sentence still teaches the whole model: "the breadcrumb is linear, but branching off it leaves a pin behind." This is the direct fix for comparing two sub-investigations (two deaths of one avatar, a promotion review vs. a violation review): each digression is one click to resume, not a re-walk. Pin cap raised 5→8 to absorb the extra churn; restoring a pin re-enters at its leaf page, not its whole prior stack — a named trade-off, not a claim of full history (§8).

**Time-bounded overrides.** A manual team override now asks "from when?" (now / from start / a specific time), stored as `(avatar, team, effective_from)`. Windowed clustering (§6) treats it as a hard seed only from that point on, so "she defected at minute 40" is representable instead of forcing one session-wide verdict.

## 4. Screens

### Main floater — `ss_combat_log` (fixed, ~340×120, not resizable)

```
┌ Combat Log ─────────────────────────────────── [x]┐
│ ● LIVE   ⏮  ⏸  ⏭   1x▾        [Trails] [Range] [?]│
│ ───●═════════▓▓▓░────────────────────────────────  │
│ 14:32:07 — "3 deaths near NW Wall in the last 10s.  │
│  Team split looks stable (62% conf)."               │
│                                        [Inspect ▸]  │
└──────────────────────────────────────────────────┘
```
Unchanged: transport row, custom-drawn scrub strip (death ticks, damage-density band, drag-to-scrub), templated headline, Inspect button. Still no browse lists here — those live one hop into the Inspector.

### Inspector — `ss_combat_inspector` (resizable, default ~420×320)

```
┌ ★Kira ★West-Wall  ↳SMG-X Claim   [pin: ★] ──── [x]┐
│ Session ▸ Team Ashguard-A ▸ Kira Nightshade         │
├────────────────────────────────────────────────────┤
│  < content pane, one page type at a time >          │
├────────────────────────────────────────────────────┤
│ Related: Team Ashguard-A · West Wall · SMG-X        │
└──────────────────────────────────────────────────┘
```
Header = pin rail (manual + auto-branch) + toggle; sub-header = breadcrumb; footer = backlinks.

**Session page** — as before, plus:
```
All events ▸   All equipment ▸   All suspicion evidence ▸
```

**Event Index** (new)
```
Filter: [avatar ▾] [type ▾] [14:20 – 14:45     ]
 14:32:07  Death   Kira Nightshade  ← Marrow, SMG-X   ▸
 14:32:04  Damage  Kira Nightshade  ← Marrow, SMG-X   ▸
[show more ▾]
```
Sortable/filterable list over the event store; row click pushes the Event page. Cold-start path for Q3: filter to "around 14:32" without already having scrubbed there.

**Equipment Index** (new)
```
Sort: [Kills ▾]
 SMG-X                 Kills 9   Dmg 3,120  Claim: through-wall (med)
 Unidentified obj ◇a3f Kills 1   Dmg  210   Claim: none
```
Closes Q4: audit a rumoured item that never crossed the auto-surface claim threshold.

**Suspicion Index** (new)
```
Sort: [Reaction time ▾]
 Wren (uncertain)  Reaction 0.11s (fast)  Occluded-fire 0
 Talon Reyes       Reaction 0.34s        Occluded-fire 2
```
Sortable by any signal; closes the Q8 gap (rule out a pattern without a named suspect).

**Avatar page**
```
Team: Ashguard-A (81% conf, this bucket)  [override ▾ from: now/start/t=]
Team history  ▓▓▓▓▓░░░▓▓▓▓▓▓▓▓  (hover → bucket + confidence)
Kills 4  Deaths 1  Dmg dealt 1,240  taken 640
Weapons: SMG-X (912 dmg) · Frag-Trap (deployed)
Mouselook 38% · Seated 4 min
Activity  ▂▃▅▇▆▃▁▂▃▅▇   (click bar → scrub + jump)
Suspicion: reaction 0.34s · occluded-fire 2 [evidence ▾]
Events [show all 62 ▾]
```
The team-history strip (one cell per bucket, coloured by that bucket's cluster) is the fix for allegiance drift: a defection at minute 40 is a visible colour change, not buried in one static percentage.

**Death/Event page**
```
Kira Nightshade died 14:32:07 (Impact)
Killing blow: Marrow Voss, SMG-X, 46 dmg
Contributing (6s window): ...
LOS: CLEAR (4/4 candidates)
  — travel time/lag can produce false BLOCKED (rarely false CLEAR)  [see world ▸]
Adjustments: none
Friendly fire: no (conf 81%/74%, this instant)
        [ ▶ Show in world ]  [ ⏱ Jump cursor ]
```
The caveat line is now permanent here, not only on the Claim page — this is the exact screen Q3 exercises.

**Team page**
```
Confidence threshold  ░░░●░░░░░░  62%  (drag → live redraw, this bucket)
Members at threshold (11): Kira 92% ▸  Talon 88% ▸  Wren (uncertain) 54% ▸
Friendly fire (this team): 3 events [list ▾]
Equipment used by this team [see all 6 ▾]
 SMG-X  Dmg 3,120   Frag-Trap  Dmg 640
```
The equipment rollup (Equipment Index filtered by team membership at time of use) directly answers Q7.

**Claim page** — unchanged (Wilson interval, permanent caveat, expandable evidence).

All lists, old and new, start collapsed — a summary row plus a disclosure arrow.

## 5. In-world overlay

- **Rung 0**: head ring + yaw arrow + name label (culled >256 m), coloured by team or per-avatar palette; eye glyph for mouselook; pulsing in-flight damage line; persistent cross+ring at recent deaths; white outline on selection.
- **Rung 1**: polylines per avatar, alpha ramps by recency in Trails mode, flat in Range mode. **Colour fallback, specified**: Range mode uses team colour only while the session-wide top-two cluster margin exceeds 15%; below that (FFA), it switches to the per-avatar name-tag palette already used elsewhere in the viewer, so trails stay legible without asserting a split the data doesn't support. Heatmap disc in both cases.
- **Rung 2**: LOS candidate dots (green/red), ray to victim, hollow+`?` for UNKNOWN. **Unidentified-equipment glyph** (hollow diamond, replacing the weapon-class icon) marks any line/marker whose source or rezzer never resolved a name — same discipline already used for LOS UNKNOWN. Threshold drag recolours rung-0/1 markers live, per the viewed bucket.
- **Uncertainty, always**: confidence → alpha; degraded position → dashed trail; UNKNOWN/unidentified → hollow+glyph, never a colour guess.
- **Picking, with disambiguation**: any marker is a screen-rect pick; plain left-click opens the Inspector, pushing the noun onto the stack; empty space does nothing. When pickable rects overlap (busy Range-mode zone), the first click selects the nearest-to-camera candidate and shows "1 of 4 — click again"; repeated clicks at the same point within one second cycle through the overlap in depth order.

## 6. Analysis model

- **Death attribution**: unchanged — same-`target` DAMAGE events in `(t−window, t]`, grouped by `(owner, rezzer)`; killing blow = last event (recovers server-authoritative `DEATH.type`, not an inference).
- **Team clustering, given a real algorithm.** Graph: nodes = avatars; edge weight = **magnitude** of damage exchanged in the current bucket (unsigned — who hurt whom doesn't matter, only how much, matching the fixed engineering plan's own edge definition). Objective: partition into K clusters minimizing total *intra*-cluster edge weight — the split with the least mutual damage inside any one team. Seed from active group id where present, else a 2-way split from the single highest-weight edge; iterate, each avatar greedily moving to whichever cluster minimizes its own intra-cluster contribution, to convergence or a pass cap (Kernighan-Lin-style local search). Confidence = normalized margin between the chosen cluster's intra-weight and the next-best. This has a defined answer for the intra-group skirmish: two sub-teams with heavy mutual damage and little damage to anyone else sit on one dominant edge, and minimizing intra-cluster damage means separating them — the objective is aligned with the scenario, unlike plain label propagation on signed weights (the original entry's bug).
- **Windowed reclustering, not one static graph.** Recomputed per coarse bucket (default 5 min, decayed carry-over from the previous bucket to avoid flapping on sparse buckets), producing a per-avatar `(bucket, cluster, confidence)` sequence. Friendly-fire confidence on a damage event reads the minimum of both avatars' confidence *for that event's bucket*; the team-history strips (§4) render this sequence directly. This makes "time-indexed confidence" an actual computation, not an assertion.
- **LOS verdict**: unchanged mechanism (candidate positions across the travel window, ray-cast per candidate, CLEAR/BLOCKED/UNKNOWN). Its caveat now renders on every page showing a verdict, including the Death page, not only the Claim page.
- **Through-wall claim per equipment**: unchanged (Wilson interval, permanent Claim-page caveat). An item whose source/rezzer never resolved shows as "Unidentified object (key …abcd)" with a hollow-diamond icon; any Claim built on it inherits an added line — "source identity unresolved — treat class/ownership inferences as provisional."
- **Damage adjustment ratio**: unchanged, `Σfinal / Σinitial` with script names from `modifications`.
- **Suspicion score (Q8)**, budgeted: three signals, never combined. Reaction time and yaw-rate smoothness come straight from track samples, no raycasts. Fire-while-occluded reuses the same LOS raycast module as death verdicts, but a cheap track-only pre-filter (attacker's yaw sweeping toward the target shortly before an accurate hit) runs first, and only pairs that pass it get raycasts, sampled at the bridge's 2 Hz over that short window — not continuously across the session. "Human baseline" and "normal yaw-rate range" are named, editable constants (`SSCombatLogTunables`), shown next to the number ("vs. 250ms baseline"). Fire-while-occluded carries a stronger caveat than the generic one: it inherits the LOS module's own false-positive rate, so the UI states "treat one flagged instance as a reason to look, not a conclusion."
- **Zone**: grid-based density clustering of event+track positions, centroid labelled by compass offset + distance from the first-detected staging cluster ("spawn"); this is the *only* construction path for a Zone, distinct from the rung-1 marquee, which produces a Selection when it doesn't land on one (§3).
- **Front, unbent by approximation, stated as such.** In place of a KDE zero-contour (no closed form, would need a grid + distance transform per bucket — not budgeted), the "front" is the perpendicular bisector of the two chosen teams' bucketed centroids, cached per bucket; distance-to-front is signed distance to that line. Coarser than a true boundary — flattens a curved or pincer-shaped front — and the Zone page labels it "straight-line approximation."
- **Mid-fight arrivals**: track/team history begins at an avatar's first-sighted sample; clustering has no edges before that (absent, not a low-confidence guess), and pages never backfill pre-arrival state.
- **Headline sentence**: unchanged — deterministic template, most salient fact in the trailing window, confidence phrase sourced from the real number.

## 7. Walkthroughs

**Q1 — how did the raid go?** Unchanged path: headline → **Range** → drag-select the hot patch (now correctly snapping to the "NW Wall" Zone, or opening a scratch Selection if it doesn't line up) → click the tallest peak → rung 0. In an FFA session, Range mode renders the per-avatar fallback palette instead of implying a split Session's own "unassigned is largest" note already disclaims — overlay and pages now agree.

**Q3 — why did X die at 14:32?** Two honest paths instead of one overclaimed one: click X's death cross directly if the cursor is already near 14:32; otherwise open the **Event Index**, filter by time or avatar, click the row. Either way lands on the Death page, which now shows the LOS caveat permanently. "Show in world" is the step back down for visual confirmation.

**Q8 — anyone behaving suspiciously?** Session's "Top claims" surfaces anything already past a threshold; for due diligence without a named suspect, the officer opens the **Suspicion Index**, sorts by reaction time or occluded-fire count, and drills into whoever stands out. Every number links to concrete events at rung 0, and occluded-fire now carries its stronger, LOS-inherited caveat.

**Intra-group skirmish.** Two Ashguard sub-teams, group tag useless. The redefined clustering separates them because minimizing intra-cluster damage is exactly the split that pulls two heavily-mutually-damaging sub-teams apart; the Team page shows two clusters both labelled "Ashguard" with a rename affordance and visibly lower confidence. The officer drags the threshold, watches boundary avatars reshuffle per-bucket live, and hand-confirms via the now time-bounded reassign control (so a fix doesn't overwrite earlier, correctly-inferred history) before trusting the friendly-fire ledger.

**FFA scenario.** No stable clusters form; most avatars sit near 50/50 for most buckets. The Team page shows "unassigned" as the largest group, Range mode falls back to per-avatar colour, and the officer works avatar-by-avatar via the indexes rather than team-by-team — the tool degrades gracefully everywhere, including the overlay, rather than only on the Team page.

## 8. Risks and open questions

- **Branch-preserving pins restore a leaf page, not a full prior stack** — a bounded trade-off kept for teachability, not a claim of full DAG navigation.
- **Windowed reclustering costs more than one static graph**: a recurring background pass every 5 minutes that must be scheduled off the render path, not assumed free.
- **Front-unbending is an approximation, not a boundary**: a straight bisector misrepresents a curved or pincer-shaped front; disclosed on the page, but a real loss of fidelity versus the (unbuildable) KDE version.
- **Auto-generated headline sentences risk sounding more certain than they are**; the template must always carry a hedge phrase sourced from the real confidence number.
- **Zone naming from compass-offset-from-spawn is weak** on sims with no staging area; degrades to numbered clusters or Selection pages — honest, less readable.
- **Rung-1 full-session trail rendering at 100k events** needs a decimated, precomputed polyline, cached and recomputed only on demand.
- **Click-to-drill on small in-world markers at distance** needs a pick radius independent of world distance and must not fire on drag; the new cycle-on-repeat-click adds one more learned interaction, a small cost against wrong-target picks in busy zones.
- **Collapsing everything to one visible page** still means no side-by-side stat blocks without pins — the direct cost of this angle, now partially offset by branch-preserving pins but not eliminated.

## Rejected critiques

- *Feasibility (minor): "display names and object facts resolve asynchronously... no placeholder-key treatment shown."* Rejected: async name resolution is an existing, viewer-wide mechanism (`LLAvatarNameCache::get(id, slot)`, per the fixed engineering plan) with its own placeholder-then-update convention already; this entry doesn't need to re-specify a primitive decided elsewhere.
- *Comprehension (serious, Q3 sub-point): the implicit suggestion that rung-1 Range mode should render every historical death as an individually pickable in-world marker.* Rejected as the wrong fix: at session scale (up to ~100k events, potentially hundreds of deaths) that reintroduces the overlay-clutter and pick-radius cost the entry's own risk section already flags for trails. The Event Index solves the same cold-start problem as a filterable list instead, cheaper and consistent with "if it must be text, put it in the Inspector."
