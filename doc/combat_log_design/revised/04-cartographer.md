# Entry 04 — The Cartographer (MAP-FIRST) — revised

## 1. Thesis

Every officer question in the brief is secretly a spatial question: *where* did the fighting happen, *where* did the front sit, *where* did a shot come from, *where* is the choke point where friendly fire keeps happening. This entry makes the region map the hub of the tool and everything else — time, teams, equipment, individuals — a lens applied to it. One floater, the **War Map**, owns the screen: a custom-drawn top-down canvas with switchable layers (heat, flow, territory, sightline, duels) and a time scrubber. The in-world 3D overlay is the map's own rung 0, the same layers projected onto the ground once the officer alt-cams over. Digging into a person, a death, a weapon, or a place opens a wiki-style Inspector page, but the map stays visible behind it, with a "show on map" anchor everywhere. The revision keeps that angle but fixes where it broke: "spatial" now has to mean something honest across the whole team-count spectrum the brief names — clean two-team fronts, drifting three-way neutral sims, no teams at all — not just the binary case, and the map itself has to remember where the officer has been, the way the Inspector already does for nouns.

## 2. Ladder of abstraction

- **Rung 0 — Live ground, in-world.** Unchanged: at the cursor time, drawn on terrain, only while stationary + alt-camming. *Step up*: the map's "you are here" wedge; clicking any in-world marker opens its Inspector page, which is also a map pin. This is the grounding instance every abstract claim cashes out to.
- **Rung 0.5 — Live ground, map.** The 2D twin: a dot per avatar, damage lines in flight, a death ring. *Step up*: drag the trail slider off zero. *Step down*: click a dot to open the Avatar page.
- **Rung 1 — Trails (helicopter view).** All positions in the window painted as fading ink at once. *Step up*: hold the slider at max — trails become **Rung 2 heat**. *Step down*: click a point on a trail to jump the cursor there and re-enter rung 0.5/0.
- **Rung 2 — Field abstractions (heat / flow / territory).** Generalized to any number of teams: occupancy is computed per team and every cell paints as whichever team locally dominates, with a separate contested measure (§6) instead of a single A-vs-B split — a 3-way neutral sim gets a real spatial fill, not a dead end. Sweep parameter: window and confidence threshold (the brief's own Rung-2 example). *Step up*: Front Timeline. *Step down*: click a hot cell → its event list → Rung 0.
- **Rung 3 — Front Timeline (territory unbent through time).** A ribbon: x = time, y = distance along the locally-dominant pair's front-normal axis, colour = which side holds that stretch. The axis is computed, not asserted (§6); for a bent front the ribbon auto-splits into per-segment small multiples instead of forcing one misleading global axis. *Step up*: per-weapon or per-segment ribbons side by side. *Step down*: click a point to set the map's window and crop, pushed onto the map's own view-history stack (§3) so the trip is reversible.
- **Rung 4 — Small multiples & duel graph.** One rung, two renderings of the same idea — per-weapon kill-location mini-maps and per-segment Front-Timeline ribbons are both tilings of the same abstraction, not two rungs sharing a label. The FFA duel graph (nodes at a damage-weighted position, edges = damage exchanged) sits here too, drawn over the terrain outline so FFA still reads as a map. *Step down*: click a tile or edge to filter the main map and drop to Rung 1.

Transitions are the point, and now so is retracing them: heat tells you *where*; clicking tells you *why*; the front ribbon tells you *when it changed*; Home (§3) tells you *how to get back to where you started*. No page is a dead end, and no map view is a one-way trip either.

## 3. Information architecture

**Hub:** War Map (main floater, `Combat > Combat Log`).

**Wiki pages** (Combat Inspector floater, breadcrumb history, Back/Forward + dropdown):

- **Place** — heat/flow/occupancy time series for a cell, events located there, backlinks to Front-segment pages. Staging clusters now split into two kinds (§6): **Staging** (gear-up behaviour confirmed) and **Arrival point** (co-located spawn-in, no confirmed push-out) — fixes a false-staging failure mode for mid-raid teleport-ins.
- **Avatar** — dossier: movement, mouselook time, kills/deaths, weapons, team + evidence-scaled confidence, trail thumbnail, plus a new **Anomalies** panel (§6): pre-contact-tracking and reaction-time outlier signals, shown as counts with sample size, never a verdict — a person-first entry point for Q8 that didn't exist before.
- **Death** — attribution, LOS verdict, damage build-up list, "show on map" / "show in-world" anchors (the latter always shows a bearing+distance HUD strip, §5, so it never silently does nothing).
- **Equipment** — classification + evidence, users, damage/kills, adjustment ratio, through-wall suspicion tally, kill-location mini-map.
- **Team** — roster with per-avatar confidence *and* its underlying evidence count, territory-% sparkline, friendly-fire list, manual override control.
- **Front segment** — a stretch of one Front-Timeline ribbon (now possibly one of several auto-split segments): which teams, when, confidence, linked deaths.
- **Duel** (FFA) — a pair of combatants: damage exchanged both ways, first contact, escalation to a kill.

**Navigation model:** breadcrumb trail at the Inspector's top; a Pin button docks up to 3 pages side by side for comparison; every page has a **Referenced by** backlinks footer. Two additions close the comprehension gap at the hub: the War Map keeps its **own** back/forward stack of view snapshots (mode, layer toggles, window, crop), pushed on every recentre-and-crop transition (Show on map, a Front-Timeline click, a hot-cell drill-down), plus a **Home** button returning to the whole-session default view — digging on the map is now as reversible as digging in the Inspector. A lightweight **breadcrumb dot trail** (last ~12 dig locations) also stays lit on the map as spatial memory, distinct from the 3-slot comparison Pins (pins hold pages open; breadcrumbs mark "already checked").

**Events (new tab, main floater):** a plain scroll list — time, kind, attacker, target, weapon, team — filterable by any column, matching the engineering plan's Phase 3.3 spec that the original draft silently dropped. Row select moves the cursor and opens the Inspector. This is the design's one deliberately non-spatial view: promotion review and violation investigation need to scan/filter events by attribute ("every hit by weapon X") when the officer doesn't yet know where to look on the map, and its attacker/target filter doubles as an avatar browser so a dedicated Avatars tab isn't needed.

## 4. Screens

### 4.1 War Map (main floater, default tab)

```
+-- Combat Log ------------------------------------------------[_][□][x]--+
| [Home][<][>]  [Now|Trails|Heat|Flow|Front|Sightline|Duels]  Δt=[--90s--]▭|
|  Layers: [x]Trails [x]Deaths [ ]FF-only [x]Territory [ ]LOS  conf:[==o-]|
+---------------------------------------------------------------------------+
|                                                                           |
|     .·:·.        N                                                      |
|   ░░▒▒▓▓██  ▲       ~~~~~~front~~~~~~                    ⚑ pin1  •••   |
|   ░░▒▒▓▓██ ↖  ↖    (band width = contested, dash = data coverage)       |
|   staging-A  ↖ ↖  ·A ·A      ·B ·B  →→   ⬦arrival-C                     |
|                  ·A   ×died14:31  ·B→  ↘                                 |
|                       ↘   ↘      ·B  staging-B                          |
|                          ~~~~~~~~~~                                      |
|   [you-are-here wedge, if in-world]                                     |
+---------------------------------------------------------------------------+
| status: 2h03m · 1,842 events · A 34% · B 29% · C 22% · contested 15%     |
+---------------------------------------------------------------------------+
| [<<][<][▶/❚❚][>][>>] speed:[1x] ────●───────────────── time 14:40:12    |
+---------------------------------------------------------------------------+
```
- `Home`/`<`/`>` are the map's own view-history controls (§3): Home resets mode/window/crop/layers to the session default; `<`/`>` step through prior recentre-and-crop actions.
- Mode row unchanged in mechanism; Heat/Flow/Territory now render an N-team dominant fill (§6), not a fixed two-team split — the wireframe above shows three factions plus a contested band.
- The `•••` mark near the pin is the breadcrumb dot trail (§3): faint numbered dots at the last several dig locations, always visible, independent of the 3-pin limit.
- Status line now has three defined formats keyed to how the session actually resolved (§6): two-team percentage-with-spread, top-N-teams-plus-contested, or (when clustering finds no stable teams) "no stable teams — N combatants, top exchange: X↔Y" pulled from the duel graph. It is never a bare number for a regime it wasn't defined for.
- Canvas, panning, clicking, box-select: unchanged from the original draft.

### 4.2 Front Timeline (tab next to the map)

```
+-- Front Timeline ---------------------------------------------------------+
| segment: [north approach ▾]  axis: auto (PCA, r=3.1)  facet:[per-wpn ▾]  |
|  pos                                                                     |
|  +80m |AAAAAAAAAAAAAAAAAAAAAAA░░░BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|   +40 |AAAAAAAAAAAAAAAAAA▓▓▓▓▓▓▓BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|    0  |AAAAAAAAAAAAA░░░░░░░░░░░BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|   -40 |AAAAAAAAAA┄┄┄┄┄┄┄┄┄┄┄┄┄┄BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|   -80 |AAAAAAAA┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|        14:00      14:20      14:40 ▲cursor    15:00       15:20         |
|  band width = contested   ┄ dashed = low data coverage (independent!)   |
+---------------------------------------------------------------------------+
```
- `segment` picker lists the auto-split segments when the front is bent (§6); each has its own locally-computed axis. `axis` shows the PCA explained-variance ratio so the officer can see *why* the tool trusts (or split) that axis, and can still override/merge/re-split manually.
- Two independent visual channels now, closing the old conflation: band **width** encodes contestedness (a tactical fact — genuinely fought-over ground); dash **pattern** encodes data coverage (a coverage fact — how many track samples back this stretch). A wide solid band is a real stalemate; a wide dashed band means "don't trust this line yet."
- Clicking a point pushes the map's view-history stack and crops/windows it there (Rung 3 → Rung 1 step-down, now reversible via the map's `<`).
- `facet` reaches Rung 4 small multiples, same as before.

### 4.3 Combat Inspector — Death page (unchanged; the entry's strongest artifact)

```
+-- Inspector --------------------------------------------------[_][□][x]--+
| ‹Back  Region Map ▸ Team:B ▸ Avatar:Renn ▸ Death @14:32:07     Pin ⚑    |
+---------------------------------------------------------------------------+
| DEATH — Renn (Team B, conf. 0.81, n=14 exchanges)  [Show on map][Show in-world]|
|   killed by: Kessa (Team A) — "Ashguard SMG mk3" · type: crushing        |
|   at (203,118,24) — near staging-B / choke "the ramp"                   |
+---------------------------------------------------------------------------+
| Damage build-up (window -8s..0s):                                       |
|  14:31:59  Kessa → Renn   6.2 dmg  (SMG bullet)      LOS: CLEAR         |
|  14:32:03  Kessa → Renn   9.8 dmg  (SMG bullet)      LOS: CLEAR         |
|  14:32:07  Kessa → Renn  14.0 dmg  KILL              LOS: BLOCKED(!)    |
|            └ candidates: t-0.0 BLOCKED · t-0.3 CLEAR · t-0.6 BLOCKED    |
|            caveat: travel-time window 1.5s, one clear candidate exists  |
+---------------------------------------------------------------------------+
| Adjustments: armour_v2.lsl  -22% (initial 18.0 → 14.0)                  |
| Related: [Kessa] [Ashguard SMG mk3] [Team A] [Team B] [Front seg: the ramp]|
+---------------------------------------------------------------------------+
| Referenced by: Front segment (choke "the ramp"), Equipment: SMG mk3 kill map|
+---------------------------------------------------------------------------+
```
- Header now carries the confidence's evidence count (`n=14 exchanges`) next to the team percentage — the same evidence-scaling fix applied everywhere a clustering confidence is shown (§6).
- "Show in-world" now always draws a small top-of-view compass strip (§5) regardless of where the officer's camera is, so it never silently does nothing.
- Same body-panel-swap skeleton serves Avatar (now with the Anomalies panel below Related), Team, Place, Equipment, Duel pages — still one `LLPanel` shell with a body sub-panel factory keyed by page type.

### 4.4 Teams & Territory tab

```
+-- Teams ------------------------------------------------------------------+
| conf. threshold: [======o===]  0.55        [Merge][Split][Reset]        |
| Team A (Ashguard-atk)  n=12  territory 34%  evid.med=9   FF: 2  [color■]|
| Team B (Ashguard-def)  n=9   territory 29%  evid.med=11  FF: 0  [color■]|
| Team C (Outer-Rim)     n=6   territory 22%  evid.med=3   FF: 0  [color■]|
|   low-evidence (hatched): Voss (0.51 margin, n=2 — shown uncertain)     |
+---------------------------------------------------------------------------+
|  [mini war-map: N-team fill, hatched = low-evidence avatars, per row]     |
+---------------------------------------------------------------------------+
```
- Confidence slider re-runs clustering live and greys out flips on the mini-map, as before — the concrete Rung-2 instance the brief names.
- New `evid.med` column and the low-evidence callout are the fix for the margin/sample-size gap: an avatar with a clean-looking margin but few recorded interactions renders hatched, not solid, the same way through-wall suspicion is never a bare percentage.
- Now scales cleanly past two teams (the wireframe shows three) instead of assuming exactly A/B.

### 4.5 Events (new tab, main floater)

```
+-- Events ------------------------------------------------------------------+
| filter: kind[DAMAGE▾] attacker[____] target[____] team[Any▾] type[Any▾]  |
|---------------------------------------------------------------------------|
| 14:31:59  DAMAGE  Kessa → Renn     6.2  SMG bullet   crushing            |
| 14:32:03  DAMAGE  Kessa → Renn     9.8  SMG bullet   crushing            |
| 14:32:07  DEATH   Kessa → Renn    14.0  SMG bullet   crushing  BLOCKED(!)|
+---------------------------------------------------------------------------+
```
- Plain `LLScrollListCtrl`, no new widget machinery. Row select moves the cursor and opens the Inspector — the attribute-filterable, non-spatial complement the plan already specified and the map alone cannot provide (promotion review, violation scans).

## 5. In-world overlay

Draws the same fields as the map, projected onto the ground, gated exactly as decided (stationary + alt-cam, 5 s grace):
- **Rung 0/0.5 parity, trails, sightline fan:** unchanged — same team colours, same click model as map/list.
- **Territory/front on the ground:** a dashed ribbon-on-terrain now carrying the Front Timeline's two-channel split: dash-gap width = data coverage, ribbon translucency/thickness = contestedness, so the ground doesn't lose the distinction the 2D ribbon makes.
- **Ground-grid LOD (fix):** heat/territory quads re-tile independently of the map's fixed ~4 m canvas grid — within ~30 m of the camera an ~1.5 m leaf is used, coarsening back to 4 m beyond, blended at the transition. A separate LOD parameter, not a reuse of the map's grid.
- **Damage-line honesty parity (fix):** in-world DAMAGE lines, built from the same target-track approximation as the map's heat, now draw at reduced alpha with a light cross-hatch, matching the map's desaturation instead of looking fully trusted.
- **"Show in-world" bearing strip (fix):** clicking it always draws a small always-on-top 2D compass ribbon (independent of the overlay's gating, since it's a flat HUD strip) with a bearing arrow and distance ("↗ 340 m — the ramp"). If the officer is already stationary + alt-cam and in range, the ground marker also lights up; if not, the compass explains why nothing else is showing.
- **Uncertainty encoding:** unchanged — low team confidence hatched, missing/coarse track thin+dashed+no head, LOS caveat text always attached to a verdict.
- **Clickable:** unchanged — one click-target model, three entry surfaces (map, list, world).

## 6. Analysis model

Shared adaptive grid (quadtree, leaf ≈4 m for the map, coarsened further out), recomputed incrementally on append; heavy fields cached on demand.

- **Heat / flow:** unchanged from the original draft (`H(cell,t,Δt)` gaussian-weighted damage density from victim-track position, desaturated for the DAMAGE-only approximation; `F(cell,t,Δt)` recency-weighted mean velocity).
- **N-team occupancy and contested measure (generalized):** `O(team,cell,t,Δt) = Σ presence_time(avatar,cell)·teamConfidence(avatar,team)` for every team, not just two. Per cell, `team* = argmax_team O(team,cell)`, and `margin(cell) = (O(team*,cell) − O(second,cell)) / (O(team*,cell)+O(second,cell)+ε)`. Fill colour is `team*`'s colour; the front contour is the level-set where `margin(cell)` crosses a fixed contested threshold between the two *locally* dominant teams — for exactly two teams this is identical to the original zero-crossing of `O(A)−O(B)`, a strict generalization, not a rewrite. A second, independent scalar `coverage(cell,t,Δt)` (track-sample density vs. expected count) drives the dash/solid channel, kept separate from `margin` so contested ground and data gaps never share one glyph. Territory-% is `margin`-weighted cell-time per team, feeding the three-mode status line (§4.1).
- **Front-normal axis (fixed):** over the contested contour's vertices, PCA weighted by local `|∇margin|` gives a candidate axis and explained-variance ratio `r=λ1/λ2`. `r≥2`: use it directly. `r<2` (bent/L-shaped/branching): Douglas-Peucker-simplify the contour to ≤4 segments, split at corners (turn angle >45°), compute a local axis per segment; the Front Timeline renders one ribbon per segment instead of one misleading global axis. Manual override/merge/re-split stays available per segment.
- **Team clustering / evidence-scaled confidence (fixed):** unchanged label-propagation seeded by active-group, but `teamConfidence(avatar,team) = margin(avatar) × evidenceFactor(avatar)`, where `evidenceFactor(avatar) = min(1, totalEdgeWeight(avatar)/EVIDENCE_FLOOR)` (`EVIDENCE_FLOOR` ≈ 5 recorded damage exchanges). A low-interaction avatar now renders hatched regardless of how clean its margin looks, with margin and edge-weight count shown next to the percentage — never bare. Manual overrides stay "officer-asserted," immune to silent re-clustering.
- **Staging vs. arrival point (fixed):** DBSCAN over each avatar's first track sample (ε tunable per-region via a slider, default ≈6 m, min-pts≈3). Promoted to **Staging** only if ≥50% of members show a subsequent low-velocity dwell ≥20 s *and* at least one member's first outbound damage originates within 40 m within 5 minutes. Clusters failing this — several people teleporting into a shared landing point mid-raid — are shown but labelled **Arrival point**, no gear-up stat, no role in the front narrative. Gear-up duration itself shows "n/a — no outbound damage recorded" for avatars who never deal damage.
- **Sightline, bounded (fixed):** the map-wide Sightline mode casts only from *active vantages* — avatars who fired in the current window, typically single digits to low dozens even in a large raid — each reusing the existing single-vantage fan (cached by vantage+cursor-time), aggregated as "covered by ≥1 clear active shooter." Budget: active-shooters(≲30) × ring-samples(≈24) rays per scrub tick, same order of magnitude as the already-specified per-click fan, not a new thousands-of-cells cost.
- **Per-avatar suspicion signals (new — closes the Q8 gap):** the Avatar page's Anomalies panel adds **pre-contact tracking** (count of instances where facing points within a narrow cone at a target for ≥1 s before any DAMAGE/DEATH links them, while LOS for that vantage/window is BLOCKED) and **reaction-time outlier** (time from a target entering the avatar's LOS-clear cone to first outbound damage, flagged only below the session's 5th percentile for that weapon class). Both render as counts/times with sample size and link to the concrete instances behind them — never a verdict, like the Equipment page's through-wall rate.
- **Duel graph node placement (fixed):** nodes sit at a damage-weighted centroid — median track position restricted to windows within ±3 s of any DAMAGE event the avatar is party to, not the plain session-median — pulling the node toward where the fighting actually happened. A short two-tick "spread" glyph connects the node to the top two damage-density clusters it contributes to, so a combatant who fought in two disjoint spots reads as a compromise point with a visible caveat, not a silently misplaced dot.

## 7. Walkthroughs

**Q1 — how did the raid go?** Open the War Map; the status line reads one of three honest forms — here, two teams: "A 58% (±6, n=61)." Set Territory, Δt = whole session, scrub: the front sweeps from the two staging blobs, stalls at "the ramp," then jumps. Click the biggest heat cell at the jump → Place page → death cluster → Rung 0 to confirm. Four levels down, the officer hits **Home**: the map snaps back to the whole-session Territory view it started from, with the stops still lit as breadcrumb dots — the thread the original draft lost is now recoverable in one click.

**Neutral sim, drifting factions (the middle case).** Three loose groups, allegiance still settling. Territory mode paints an N-team dominant fill (A/B/C plus a contested remainder) instead of forcing a two-sided front; the status line reads "A 34% · B 29% · C 22% · contested 15%." As two factions start trading more damage with each other than with the third, the Teams-tab slider shows their margins tightening and the map's fill redraws — the spatial rung the brief's "spectrum, not binary" language asked for.

**Q3 — why did X die at 14:32?** Click X's death cross (map, list, or in-world). The Death page's build-up list is unchanged and remains the strongest artifact: three hits, the killing blow `BLOCKED(!)` with one CLEAR candidate spelled out inline, never a bare verdict. "Show in-world" now always draws the bearing/distance compass strip first; if the officer is near "the ramp" already the sightline fan also lights up on the ground, and if not, the compass tells them which way to walk instead of showing nothing.

**Q8 — suspicious behaviour, starting from a person.** Open the reported avatar's dossier directly — no weapon required. The Anomalies panel shows "pre-contact tracking: 3 of 40 encounters (small sample)" and a reaction-time outlier flagged against the SMG-class distribution; each entry links to the concrete Death/Damage instance with its own LOS caveat — the same honest evidence the Equipment-first path already offered, reachable from a name. The officer can still pivot to the weapon from there; the two entry points now meet in the middle.

**Intra-group skirmish.** Unchanged and still correct: active-group tag is useless (both sides are "Ashguard"), the Teams tab seeds purely from who-shoots-whom, the map shows two staging blobs hatched A/B under one group colour, and the officer drags the confidence slider until the clusters stop bleeding — now also showing the evidence-count column, so a thin split reads as tentative, not solid. Friendly-fire semantics unchanged.

**FFA.** Territory/front layers switch off only when clustering genuinely finds **no stable teams at all** — not merely "more than two" (that's the N-team fill above) — and the officer uses the Duel graph. Nodes sit at damage-weighted centroids with a spread glyph for anyone who fought in two spots, so the graph tells the truth about where exchanges happened instead of parking someone at a staging area they barely fought near. Edges remain clickable to individual damage events, drawn over the terrain outline — space stays the axis even where teams don't exist.

## 8. Risks and open questions

- **Front/territory math remains the riskiest derived quantity**: PCA-plus-corner-split is a heuristic, not a guarantee, for genuinely chaotic fronts; the confidence-band rendering must stay non-skippable by default or officers will over-trust a clean-looking line.
- **DAMAGE-location approximation** (victim's track, not shooter's) is a real gap for heat accuracy near cover; the desaturation/cross-hatch caveat must be visually obvious in both the map and world, not a tooltip nobody reads.
- **Grid/quadtree recompute cost** at 100k events / millions of samples still needs profiling with dirty-cell tracking on append; the new ground-level LOD re-tiling adds a second cost surface to profile alongside it.
- **Small-multiples panels (Rung 4)** remain the most XUI-novel piece, now used for one more thing (auto-split front segments) — worth prototyping early.
- **Staging/arrival-point split reduces but doesn't eliminate mislabeling**: a real early rally point with unusually fast gear-up could still miss the 20 s dwell threshold and land as "Arrival point"; the Place page needs a manual promote/demote between the two labels.
- **Per-avatar suspicion signals will produce false positives** for cautious players or lucky reads; both ship with sample size by design, but the framing must keep emphasizing "evidence, not verdict" — a person-keyed panel invites snap judgment more readily than a weapon-keyed one.
- **Sightline's bounded active-vantage definition** undercounts shooters who fired just outside the current window; worth a small look-back allowance if testing shows gaps at window edges.

## Rejected critiques

- *Comprehension, Walkthrough 1 — the FFA duel graph as "the pile of views failure mode."* Rejected as stated: duel-graph nodes plot at real (now damage-weighted) positions on the same terrain canvas as every other rung, so it's a spatial lens, not a bolted-on graph view. Accepted underneath: *when* it fires is narrowed from "3+ teams" to "clustering finds no stable teams at all," since N-team occupancy now covers the multi-faction case the duel graph used to handle alone.
- *Comprehension, minor — "Rung 4 used for two different constructs" is numbering confusion.* Rejected: per-weapon kill-location mini-maps and per-segment Front-Timeline ribbons are both small-multiples tilings of one abstraction — the rung doing its job, not overload. Prose is tightened so both are introduced under one Rung-4 umbrella.
- *Feasibility, minor #7 — DBSCAN constants need a full sensitivity study before v1.* Rejected as a v1 requirement: the Staging/Arrival-point split (§6) already neutralizes the failure mode (mid-raid TP-landing clusters) the reviewer was worried about; a per-region-tunable slider on the existing constants closes the remaining tuning concern cheaply, and a full study is future work, not a blocker.
