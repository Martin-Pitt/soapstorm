# Entry 05 — Time-First: the Combat Timeline (revised)

## Revision notes

Accepted and fixed: no rung-1 "where" view independent of team/front clustering (fatal); attacker position for turrets/deployables/mounts has no computation path (fatal); object targets have no home in the information architecture (fatal); phase segmentation bakes a single-front narrative into named UI without an honest fork for multi-front sessions (serious); Q8 has no per-avatar suspicion rollup, forcing the walkthrough to start from a weapon instead of a person (serious); overlay trail length is coupled 1:1 to timeline zoom with no cap, contradicting the engineering plan's `SSCombatLogTrailSeconds` (serious); the promised team-confidence slider is asserted in the ladder table but never built in the screens (serious); the through-wall reaction-time signal implies knowledge of the suspect's own view range, which the brief says is unknowable (serious); mid-fight arrivals, row-scrolling on jump-to-death, denominator wording drift, breadcrumb/history-stack ambiguity, and LOD specificity (all minor) are fixed below.

Rejected: the request for a dedicated side-by-side compare view for two pinned nouns. The brief already allows multiple floaters open at once (§"Constraints of the medium"), and pinning already holds one item in the docked strip — opening a second `Combat Inspector` instance and pinning the second candidate there gives side-by-side comparison for free. Building bespoke split-compare machinery on top of that duplicates a capability the medium already has, which cuts against the brief's own "prefer few, deep, composable views" instruction.

## 1. Thesis

A raid is a story that unfolds on one axis: time. Every other fact an officer wants — teams, deaths, weapons, suspicion — is really a question of *when* something changed. This design puts a zoomable, swimlaned timeline at the center of the tool instead of treating it as a transport bar bolted onto a 3D view. The timeline is not a scrubber for the overlay; it **is** the primary instrument, and the 3D overlay is one of its lenses. Rows are avatars (grouped into team bands), columns are time, density is combat intensity, and the tool segments the session into named phases the way a heart monitor segments a session into beats and arrhythmias. But time-first does not mean space-absent: sitting directly beside the time axis, driven by the same cursor and requiring no successful team or front detection to work, is a **Contact Map** — a raw density heatmap of where damage and deaths actually happened. Time is the spine an officer reads first; space is not one drill-down away behind a fragile two-cluster model, it is a second, always-available sibling view built from the one thing every event has: a place it happened. The officer's default motion is horizontal — skim the whole raid, spot its shape, zoom into a second and let the world materialize under the cursor — but "the shape of the raid" now includes both when it happened and where, honestly, even when the raid never sorted itself into two neat sides.

## 2. Ladder

| Rung | View | Step up (concrete → abstract) | Step down (abstract → concrete) |
|---|---|---|---|
| 0 — concrete | 3D overlay at the cursor's instant | Press "Trail" or widen the timeline zoom | Drag the timeline zoom to its narrowest stop (±0.5 s); overlay shows one instant |
| 1 — abstract over time | Whole-session swimlane view: every avatar's row from t=0 to t=end, trails not blips | Click any row/segment to jump the cursor there and re-narrow the zoom | Click a tick or drag-select an interval; overlay renders that instant/window |
| 1 — abstract over space | **Contact Map**: grid-binned density heatmap of DEATH and resolvable DAMAGE positions for the current zoom window, computed independent of team/phase clustering | Click a hot cell to see it break into per-avatar tracks | Click a cell to filter swimlanes/overlay to events in that cell — jumps toward rung 0 for that place |
| 2 — abstract over parameter | Phase-confidence hover, LOS-candidate strip (verdict across the travel-time window), **team-confidence slider** (built into the Teams tab, §4.1) | Raise a slider to see how fragile a verdict is | Click the single candidate/instant that matters; jumps to rung 0 for that candidate |
| 3 — multi-dimensional / small multiples | Phase cards (one tile per detected phase, or per simultaneous front when the session forks — see §6); per-team density sparklines stacked in the swimlane header | Click a phase card to select its time range everywhere | Phase card has a "Play this phase" button — narrows zoom to the phase and starts playback at rung 1→0 |
| 4 — coordinate transform | "Distance-to-front" strip: y-axis reinterpreted as distance between two opposing clusters; used only when the Contact Map confirms a single front | Toggle from raw density to front-distance in the same panel | Click a rise/plateau/drop on the unbent line; jumps to the timeline at that time, then to the overlay |

The rule enforced throughout: nothing is a dead end, and nothing at rung 3–4 is load-bearing for a question rung 1 can already answer honestly. Every tick, row, cell, card, and slider handle is clickable and either narrows the time/space window (down the ladder) or opens a coarser summary (up the ladder). The cursor is the single shared coordinate between all rungs — moving it anywhere moves it everywhere, in both the time swimlanes and the Contact Map.

## 3. Information architecture

**Nouns** (each a page in the Inspector, each with a breadcrumb trail and a "Referenced by" backlinks block):

- **Moment** — a point or narrow range in time. Default landing page when you click empty timeline. Shows: active phase(s), avatars present, events in range, camera jump button.
- **Place** — a grid cell or officer-drawn region on the Contact Map. Shows: event count/density, participants, dominant weapon, links to every Event inside it. Independent of team or phase detection succeeding — this is the honest spatial answer the fatal review found missing.
- **Event** (DAMAGE / DEATH) — attacker, target (avatar or **object**), weapon, LOS verdict with its position-confidence tier (see §6), adjustment chain, related events.
- **Avatar** — session dossier: kills/deaths, weapons used, mouselook %, time seated, team history (with reassignment timestamps), a mini swimlane row rendered full-width, and a **suspicion rollup** (per-avatar BLOCKED-kill ratio with denominator, reaction-time distribution vs. cohort — see §6).
- **Object** — a vehicle, turret, or deployable that was itself damaged or destroyed. Kind (inferred), owner/rezzer chain, last known position and its confidence tier, events where it is the target, avatars who rode/rezzed/fired it. New noun, added to close the fatal gap: object targets previously had no page anywhere in the architecture.
- **Team** — roster, confidence, friendly-fire count, shared front-distance history (when a single front holds), weapon mix.
- **Equipment** — classification + evidence, users, damage/kills, adjustment ratio, through-wall suspicion score with sample size, last-known-position confidence tier for non-avatar attackers (turrets/deployables).
- **Phase** — auto-detected segment: kind, time range, confidence, participants, casualties, dominant weapon, "why this boundary" explanation, and — when the Contact Map found more than one simultaneous cluster during the window — a fork into per-cluster sub-phases instead of one blended label.
- **Verdict** — a single LOS or suspicion judgment; shows every candidate considered, its individual outcome, and which position-confidence tier produced it.

**Navigation model**: the Inspector floater keeps a **history stack** (back/forward arrows) and a **breadcrumb bar** that is, by construction, just the current path through that same stack — pressing Back pops the last breadcrumb segment too, so the two never disagree. The breadcrumb is deliberately a linear trail of "how I got here," not a hierarchy; when a noun has several parents (an event referenced by more than one page) the breadcrumb still shows only the click path that led here, and the **backlinks** block at the bottom of the page is the tool for walking the graph's other edges ("Referenced by: 3 deaths, 1 friendly-fire flag, Phase: Collapse"). Any page can be **pinned** to a docked side-strip so it survives navigation — the officer building a promotion case pins the candidate's Avatar page and keeps digging elsewhere without losing it; opening a second `Combat Inspector` floater and pinning a second candidate there gives side-by-side comparison without any new UI.

The **Combat Timeline** floater is not a page in this graph; it is the map the graph is drawn on, and the Contact Map is its spatial twin — both read from and write to the same cursor. Clicking anything on either opens/updates the Inspector; clicking anything in the Inspector moves the timeline cursor and, if the item has a location, nudges the overlay camera and highlights the matching Contact Map cell.

## 4. Screens

### 4.1 Main floater — `Combat Timeline` (`ss_combat_log`)

```
+---------------------------------------------------------------------------------------+
| Combat Timeline                                                            [_][口][x]  |
+---------------------------------------------------------------------------------------+
| ⦿ Live  ▶ Play  ⏸  Speed[0.25x 1x 2x 8x]  ⏮1s ⏭1s   Zoom ──●───────── 2h..0.5s        |
| Find avatar: [__________] (jumps cursor, scrolls+expands the row, opens Inspector)     |
+---------------------------------------------------------------------------------------+
| PHASES  [Staging▓|Push▓▓|Front(A)███/Front(B)███ forked - 2 clusters|Collapse▓|Resp░]  |
|         (hover a band -> tooltip: kind, fit-confidence 0.82, duration; forked bands    |
|          show cluster membership instead of one blended confidence number)             |
+-----------------------------------------------------------------+---------------------+
| OVERVIEW  (whole session, fixed 1 col/window, never rescrolls)  |  [Phases][Teams]     |
| ▁▂▃▅█▇▅▃▂▁▁▂▄▇█▇▅▃▁▁▂▃▅▇█▇▄▂▁▁▁▂▃▄▅▆▇█▇▆▅▄▃▂▁▁                  |  [Equip.][Avatars]   |
| ·participants: 4 5 7 7 9 9 8 6 5 4 4 5 6 7 9 11 11 9 6 4 3       |  [Where][Objects]    |
|                                   ▽ viewport(zoomed window)      |                       |
+-----------------------------------------------------------------+  ------------------   |
| WHERE (Contact Map, current zoom window; grid-binned density,   |  Phase: Front (A)     |
|  DEATH + resolvable DAMAGE positions; click a cell to filter)   |  09:41-11:02          |
|  ░░▒▓█▓▒░░···░░▒▓▓█▓▒░···········                                |  fit-conf 0.78        |
|  ···········░▒▓█▓▒░············ 2 hot cells => 2 clusters live  |  casualties: 9        |
+-----------------------------------------------------------------+  top weapon: SMG-9mm  |
| SWIMLANES  (scrolls vertically; header rows collapse when zoomed out past N rows)       |
|  Team: Ashguard-A  conf 0.9   ▁▂▃▅▇█▇▅▃▂▁ (team density)         |  [Play][Pin][Open]    |
|   Nexii   ▤▤▤▤▤▤────●───┼────╳────────────●──●───┼──────▤▤▤▤▤▤   |                       |
|   Orion   ────────────────┼───●──●──────────────┼───────────    |                       |
|   ...                                                            |                       |
|  Team: Ashguard-B  conf 0.62 (mixed w/ intra-group)  ▁▂▄▇▇▄▂▁    |                       |
|   Bret    ────┼──╳────●──────────────────────────┼──────        |                       |
|  Unaffiliated / low-confidence  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ |                       |
|  Objects  ▽ (collapsed)   Turret-3 ──●──╳ (destroyed 14:29)      |                       |
|                                          ▲ cursor 14:32:07       |                       |
+-------------------------------------------------------------------------------------+---+
| Selected: DEATH — Nexii @14:32:07 · killer Orion · weapon "Ashguard-9mm" · LOS CLEAR    |
+---------------------------------------------------------------------------------------+
```

Regions (changes from the original marked *new*):
- **Transport bar**: unchanged, plus *new* a **Find avatar** field beneath it — typing and selecting a name moves the cursor to that avatar's next/nearest event, scrolls the swimlanes to reveal their row (expanding it out of a team-aggregate collapse if needed), and opens their Avatar page. This closes the "row isn't visible" gap from the Q3 walkthrough.
- **Phase ribbon**: as before, plus *new* forking behavior — see §6. A forked band's tooltip shows per-cluster participants/casualties instead of one merged number, and its rendered confidence is a **fit** confidence only (how well this window matches its own rule thresholds); the fork itself, not a hidden second number, is what communicates "the single-front model didn't apply here."
- **Overview strip**: unchanged (rung-1 safety rail), plus *new* a thin **participant-count** line under the density sparkline so a density rise driven by new arrivals is visually distinguishable from a density rise driven by escalation among the same people — the two look identical in raw `I(t)` alone (a real gap the reviews caught).
- ***New* Where strip (Contact Map)**: a second small custom-drawn panel, same width as Overview, showing grid-binned event density for the *current zoom window* (not a fixed whole-session view, since "where" for the last 30 seconds and "where" for the whole raid are different questions). Cells brighten with density; clicking a cell filters the swimlanes and Inspector list to events inside it and highlights it on the in-world minimap. This is the direct answer to Q1's "where" that does not require team or front clustering to have converged — it works identically for a classic raid, an FFA deathmatch, or three unrelated skirmishes.
- **Swimlanes**: as before (ticks, hatching, team header rows, collapse-on-zoom), plus *new* a collapsed **Objects** band beneath Unaffiliated, one row per object that was itself a DAMAGE/DEATH target (turrets, vehicles, deployables), using the same tick vocabulary. Collapsed by default since most sessions have few object-target events; expands on click or when an object event is selected.
- **Side panel**: tab container gains *new* **Where** (a larger, interactive version of the Contact Map strip, with an officer-drawn lasso to select an irregular region rather than a single grid cell) and *new* **Objects** (sortable list of objects with target events) tabs, alongside the original Phases, Teams, Equipment, Avatars. The **Teams** tab gains *new* a **Clustering looseness** slider (rung 2, previously promised but not built) that redraws swimlane grouping at looser/tighter confidence thresholds in place, live.
- **Status line**: unchanged.

XUI feasibility: the Contact Map is a second cached-draw-list custom `LLView`, identical pattern to the swimlane view — a 2D grid of rects colored by a bucketed density count, rebuilt only when the zoom window, filter, or event set changes. Rebuild cost is bounded by the same technique used for the time-axis views: **column/cell-bucketed min/max/density binning** — one accumulator per output cell (roughly 32×32 for the floater's map size), a single incrementally-maintained pass over events in the current window, never a per-frame full recompute. This makes the earlier "LOD is asserted, not specified" gap concrete for both the time and space overviews.

### 4.2 Inspector floater — `Combat Inspector` (`ss_combat_inspector`)

```
+--------------------------------------------------------------------+
| Combat Inspector                                        [_][口][x] |
+--------------------------------------------------------------------+
| Session > Phase: Front(A) > Death: Nexii @14:32:07 > Equipment: 9mm |  <- breadcrumb (linear; Back pops one segment)
+--------------------------------------------------------------------+
| DEATH — Nexii   14:32:07.4   [Show in world] [Track victim] [Pin]  |
|  killer: Orion (Ashguard-A)      weapon: "Ashguard-9mm" (SMG)      |
|  killing blow: 34 dmg, type ANTI_ARMOR   adjusted from 51 (-33%)   |
+--------------------------------------------------------------------+
| RELATED DAMAGE (window: 8s before death)                           |
|  14:32:01.1  Orion -> Nexii   12 dmg  [LOS: CLEAR, tier: track]    |
|  14:32:04.6  Orion -> Nexii   19 dmg  [LOS: UNKNOWN - track gap]   |
|  14:32:07.4  Orion -> Nexii   34 dmg  [LOS: CLEAR, tier: track]    |
+--------------------------------------------------------------------+
| LOS CANDIDATES (killing blow, travel window 1.5s, step 0.25s)      |
|  attacker position tier: TRACK (avatar has a live sample)          |
|  t-1.50 (12,44,21) BLOCKED   t-1.25 (13,44,21) BLOCKED             |
|  t-1.00 (14,45,21) CLEAR     t-0.75 (15,45,20) CLEAR   <- used     |
|  t-0.50 (16,46,20) CLEAR     t-0.25 (17,46,20) CLEAR               |
|  Verdict: CLEAR (4/6 candidates clear)   caveat: lag/travel-time   |
+--------------------------------------------------------------------+
| ADJUSTMENTS   armor_v3.lsl: -33%  (task 8f2c...)                   |
+--------------------------------------------------------------------+
| Referenced by: Phase "Front(A)", Team Ashguard-A friendly-fire: none|
+--------------------------------------------------------------------+
```

Every noun type renders through the same shell; body sections differ per type. *New*: the Avatar page's body now includes a **Suspicion** section (per-avatar BLOCKED-kill ratio with its own denominator, reaction-time distribution vs. cohort, both always shown as raw counts/distributions, never a verdict) instead of suspicion living only on the Equipment page. *New*: an Object page (turret/vehicle/deployable as target) shows the same shell with a body section for its own damage/death history and its position-confidence tier instead of a stat block. *New*: every LOS candidate list and every damage-in-flight line now states its **attacker position tier** (`TRACK` / `LAST-SEEN` / `APPROX-VIA-OWNER`, see §6) so the officer can see how much to trust a given verdict before reading it. `[Show in world]` snaps the timeline cursor to the event and, if gating conditions are met, the officer sees the overlay light up at that instant.

## 5. In-world overlay

The overlay is rung 0 and always reads *from the timeline cursor*, live or scrubbed — "the world follows the cursor" is the literal contract, not a metaphor.

- **Rung 0 (cursor instant, zoom < ~5 s)**: exact positions at t. Avatar = ring at feet + yaw arrow + name label; mouselook = small eye glyph above the head; seated = rides the vehicle marker. Damage in flight = a line from the attacker's resolved position (see §6 tiers) to target, colored by damage type; a **hollow diamond** marker (instead of the normal solid ring) at the attacker end whenever the position is `LAST-SEEN` or `APPROX-VIA-OWNER` rather than a live track, so an approximate turret/deployable position is never visually confused with a precise one. Deaths = persistent cross+ring, killer line drawn from the same tiered position, label "victim <- killer (weapon)". Object deaths (turret/vehicle destroyed) use a square marker instead of a ring to distinguish them from avatar deaths at a glance.
- **Rung 1 (zoom widened, trail mode)**: every avatar carries a trail — a polyline of positions across the trail window, alpha-ramped old->new, color = team, width bucketed by track quality. *Fixed*: the trail window is **`min(current zoom window, SSCombatLogTrailSeconds)`**, not the raw zoom value — widening the timeline toward whole-session no longer asks the overlay to draw hours of trail. When the cap is active, a small caption reads "trail: 20s (capped; timeline window is 6m)" so the officer knows the overlay is showing a narrower slice than the swimlanes. This directly fixes the earlier 1:1 coupling that could request a multi-hour trail while alt-cammed.
- **Rung 1s (space)**: the Contact Map's selected cell/region, if any, renders as a translucent ground-plane highlight in-world — the spatial equivalent of the time-axis trail, and it works without any team/phase clustering.
- **Rung 2 (parameter sweep)**: selecting an event's LOS candidates highlights each candidate shooter position as a small numbered marker (green ring = clear, red ring = blocked, grey = untested/gap), all shown simultaneously.
- **Uncertainty encoding**: unchanged core rule — outline style (solid/dashed/dotted) is the uncertainty channel, color stays reserved for team/damage-type. Track gaps draw dashed/lower-alpha trail segments; unknown-team avatars draw neutral grey with a dotted ring; LOS UNKNOWN draws a grey dashed line; equipment of unknown classification draws a question-mark icon; *new* attacker positions below `TRACK` tier draw with the hollow-diamond marker described above, which is the same "don't let uncertain data look as confident as verified data" discipline applied to the newly-added position-tier concept.
- **Clicking**: any marker, trail segment, or Contact Map cell is a screen-rect/ground-quad hit target; clicking it moves the timeline cursor and opens the matching Inspector page.
- **Gating**: unchanged from the engineering plan (stationary + alt-cam, 5 s grace after leaving mouselook/OTS). While gated off, the timeline, Contact Map, and Inspector remain fully usable.

## 6. Analysis model

- **Combat intensity `I(t)`**: kernel-smoothed event rate, `I(t) = Σ w(kind) · K(t - t_i, σ)`, `w(DEATH)=5, w(DAMAGE)=1`, Gaussian `σ` scaled to zoom. *New*: `N(t)`, the count of distinct avatars with an event or track sample in the window, is computed alongside `I(t)` and plotted as the participant-count line under the Overview sparkline, so an arrivals-driven density rise (both `I` and `N` rise together) reads differently from an escalation among the same people (`I` rises, `N` flat).
- **Contact Map density `M(cell, t-window)`**: *new*. For the current zoom window, bin every DEATH position (always available) and every DAMAGE position resolvable per the attacker/target tiers below into a fixed grid (roughly 32x32 cells over the sim), incrementally accumulated as the window changes. No team, phase, or front detection is a precondition — this is the fatal-review fix: "where did the fighting happen" now has an answer built directly from raw positions, for FFA and multi-front sessions equally.
- **Spatial cluster count `K_space(t-window)`**: *new*. A cheap flood-fill over `M` (adjacent cells above a density threshold merge into one cluster) gives the number of simultaneous, spatially distinct fights active in the window. This is the signal phase segmentation was missing: it distinguishes "one front, some noise" from "this isn't one front at all."
- **Attacker position tiers** (*new*, closes the fatal gap on turret/deployable/mount attackers, which the fixed per-avatar-only track store cannot resolve on its own): for any event, resolve the attacker's position at `t` as the first of: **(1) TRACK** — `source` or `owner` is an avatar with a track sample within the window (handheld weapons, mounts fired by the rider); **(2) LAST-SEEN** — the object (`source`/`rezzer`) is a turret/deployable/vehicle whose equipment-registry entry recorded a position the last time the viewer or bridge actually saw it (the existing fact-gathering path in the engineering plan already captures object facts on sighting; recording the position alongside those facts is a small, additive analysis, not a redesign of the fixed track store), decayed in confidence with staleness; **(3) APPROX-VIA-OWNER** — no object sighting exists at all, so the rezzing avatar's own track position stands in, visually and textually flagged as approximate. Every LOS candidate, damage-in-flight line, and suspicion tally states which tier produced it; through-wall suspicion tallies exclude tier-3 positions by default (an officer can opt in, clearly labeled) so a guessed position never silently inflates or launders a weapon's suspicion score. This does not give objects a full continuous track — that would require redesigning the fixed per-avatar track store, which is out of scope — but it gives every attacker *some* honestly-tiered position instead of an unstated assumption that only avatars shoot.
- **Object targets** (*new*, closes the fatal gap on destroyed vehicles/turrets/deployables): any event with `targetIsAgent` false gets an **Object** page (§3) and a row in the collapsed Objects swimlane band, using the same tick vocabulary as avatar rows. Object identity/kind reuses the existing equipment classification.
- **Team centroid `C_team(t)`** / **dispersion `D_team(t)`**: unchanged from the original — mean position/pairwise distance of team members with a track sample within +/-2 s of t, excluding low-confidence members into a wider "uncertain member" band.
- **Front distance `F(t)`**: unchanged formula, but *scoped*: it is only computed and only shown (rung 4) when `K_space` reports exactly one active cluster for the window. When `K_space > 1`, the distance-to-front transform does not render a blurred average; the Phases tab instead shows one small-multiples front-distance strip per cluster (capped at 4 before falling back to "N simultaneous fronts — see Where map").
- **Phase segmentation**: same hysteresis state machine (STAGING/PUSH/FRONT/COLLAPSE/RESPAWN) over `(I(t), trend of D_team(t), trend of F(t), distance from inferred spawn)`, but *revised*: the state machine now runs **per spatial cluster** from `K_space`, not once globally. When `K_space(t-window) == 1`, this is identical to the original design — one ribbon, one confidence number (fit-confidence: normalized margin between the winning rule and its threshold). When `K_space(t-window) > 1`, the ribbon **forks** into one sub-band per cluster instead of averaging them into one misleading label — this is the direct fix for the reviewed failure mode where a three-front session would render as a single "weak-confidence Front" that reads as "roughly right" when the honest statement is "this session doesn't have one front." Each fork carries its own participants/casualties/dominant weapon and its own fit-confidence; there is no separate "is this the right model" number because the fork itself is the model-appropriateness signal — a single unforked band already means the single-front model held.
- **Team assignment and its confidence**: unchanged (shared-group seed + label propagation), plus the *new* Clustering-looseness slider (§4.1 Teams tab) that re-runs the propagation at a looser/tighter threshold on demand and redraws swimlane grouping live — the rung-2 control the original ladder table promised but the original screens never built.
- **Avatar arrival handling** (*new*): a swimlane row appears the moment an avatar has its first track sample or event, marked with a small arrival caret at that point rather than implying presence from t=0. Team assignment is attempted only once the avatar has at least one damage event linking them to a cluster; until then they render in the Unaffiliated band, consistent with the existing FFA fallback rather than a new mechanism.
- **Friendly fire**: unchanged — DAMAGE/DEATH where `owner`'s team-at-`t` equals `target`'s team-at-`t`, reassignment-aware. **Dependency flag, elevated per review**: this requires the team model to expose "assignment as of time t," which the fixed engineering plan's team-clustering shape does not yet promise; this is called out in §8 as a blocking dependency to confirm before implementation, not a background risk.
- **LOS verdict**: unchanged mechanics (CLEAR if any travel-window candidate is clear, BLOCKED if all are, UNKNOWN if no sample exists), now computed from the tiered attacker position above and always displaying its tier alongside the verdict. Through-wall suspicion = BLOCKED / all damage-and-death verdicts attributed to that weapon (tier-3 positions excluded by default), always shown with its denominator.
- **Reaction-time signal**: *revised and demoted*. Defined precisely as "time between our own LOS model marking a target newly visible from the shooter's candidate position, and the shooter's first subsequent hit" — this measures the tool's visibility model, not the suspect's actual screen, draw distance, or settings, which the brief states are never knowable. It is shown as a **corroborating flag**, always beside (never merged into, never averaged with) the LOS BLOCKED ratio, captioned explicitly: "measures our own tracking model's view of when the target became reachable, not the suspect's client — treat as a prompt for follow-up, not evidence."
- **Damage adjustment ratio**: unchanged — `damage / initial` per event, aggregated as a distribution (median + range), not one number.

## 7. Walkthroughs

**Q1 — How did the raid go overall?** Open the Combat Timeline. Overview, participant-count line, and Phase ribbon are visible with zero clicks. For the classic-raid case, five bands appear (Staging/Push/Front/Collapse/Respawn); clicking the Front band narrows the zoom, the swimlanes show two team blocks converging, and the front-distance transform shows the rise-plateau-drop shape, because the Contact Map confirmed exactly one active cluster. For a session where fighting is genuinely scattered, the ribbon instead shows a forked "Front(A) / Front(B)" pair from the moment two clusters go active, and the always-present Where strip shows two hot regions on the map regardless of whether team clustering ever converges — so "where did the fighting happen" has an answer from rung 1 even before any team or front model succeeds. The officer has "where, when, between whom" inside a few clicks in either case, and the top-level view never overstates a single-front story it can't back up.

**Q3 — Why did X die at 14:32?** Type "Nexii" into Find avatar, or scrub to 14:32 and click the `x` on her row (now guaranteed reachable — Find avatar expands and scrolls to it if it was collapsed or off-screen). The Inspector opens on the Death page: killer, weapon, killing-blow damage and its adjustment, related damage in the preceding 8 s each tagged with its LOS verdict and position tier, and the LOS verdict with all six travel-window candidates shown together (CLEAR, 4/6, tier TRACK). `[Show in world]` moves the cursor there; stationary and alt-cammed, the overlay lights the exact geometry.

**Q8 — Suspicious behavior?** Open Nexii's — or any avatar's — Avatar page directly (person-first, not weapon-first): the Suspicion section shows her BLOCKED-kill ratio (say 3/12, her own denominator, not diluted by other users of a shared weapon) and her reaction-time distribution against the cohort median, both flagged only as outliers, both captioned with the "our tracking model, not the suspect's client" caveat. If the officer instead started from an Equipment page ("Ashguard-9mm," 3/40 BLOCKED of all damage events attributed to that weapon, not just kills), opening any flagged kill's LOS candidates cross-references straight back to the responsible avatar's own Suspicion section — so a shared weapon's diluted aggregate can no longer hide a single bad actor, and the officer can dig from either direction and land on the same evidence.

**Intra-group skirmish**: two Ashguard teams. Active-group coloring is useless, so the Teams tab shows two clusters labeled "Ashguard (cluster 1)" / "(cluster 2)" from who-shot-whom. Swimlanes group by these clusters; friendly-fire stays silent for cross-cluster damage and flags only same-cluster damage. The officer renames the clusters ("Defenders"/"Attackers") from the Teams page; the rename propagates everywhere.

**FFA scenario**: a neutral-sim deathmatch. Team clustering stays at confidence ~0; swimlanes fall back to an "Unaffiliated / low-confidence" block. Phase detection still runs off intensity/dispersion, and — because the Contact Map's `K_space` typically reports several simultaneous small clusters in a true FFA — the phase ribbon forks accordingly instead of forcing one blended "Front" band; the Where strip shows several hot spots directly. The officer gets a shape-of-the-fight read with zero named teams and an honest, non-averaged spatial picture, which is exactly the case the earlier design could not answer.

## 8. Risks and open questions

- **Reassignment-aware friendly-fire lookup is a confirmed blocking dependency, not a background risk.** It requires the team model to expose "assignment as of time t," which the fixed engineering plan's team-clustering data shape does not currently promise. This must be confirmed with the engineering plan before implementation, ahead of any UI work that depends on it.
- **Attacker position tiers for objects are an honest approximation, not parity with avatar tracking.** `LAST-SEEN`/`APPROX-VIA-OWNER` positions can be stale or simply wrong for a turret that was moved after last sighting; the tier label and the hollow-diamond marker are the mitigation, not a fix — a full continuous object track store would require redesigning the fixed per-avatar-only track store, which is out of scope for this entry.
- **Spatial cluster count `K_space` is itself a threshold-tuned heuristic** (flood-fill over a density grid) and can over- or under-merge nearby-but-distinct skirmishes, or briefly flicker a fork on and off near the threshold at cell/frame boundaries. Mitigation: hysteresis on the fork/merge decision (require N consecutive windows before flipping), same discipline already used for phase-boundary hysteresis.
- **Swimlane scaling with headcount.** 40+ avatars at whole-session zoom risks a wall of thin rows; the team-aggregate collapse and the new Objects band both need their auto-collapse thresholds tuned against a real 1-2 h synthetic session, not guessed.
- **Contact Map grid resolution is a tradeoff.** Too coarse blurs two nearby but distinct skirmishes into one hot cell; too fine is noisy and expensive to flood-fill every window change. Needs tuning against synthetic multi-front data before shipping the fork mechanism that depends on it.
- **Respawn-wave detection via track discontinuity** may false-positive on ordinary lag/track gaps that look like a "reappearance." Needs a minimum-gap-duration and near-spawn-radius tuned against real data, and should always be presented as inferred, with the raw track gap visible on click-through.
