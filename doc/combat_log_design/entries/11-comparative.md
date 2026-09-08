# Entry 11 — Comparative: the raid as a grid of differences

## 1. Thesis

An officer never really asks "what happened?" — they ask "compared to what?". Was this push worse than the last one? Does this weapon kill through walls more than the other twelve? Is Vex's reaction time weird, or is everyone that fast? Would the verdict still be BLOCKED if the bullet had flown 0.4 s instead of 0.1 s? So this design has no "detail view" and no "summary view". It has **one view, the Grid**, whose atom is a small picture of a subset, and whose grammar is three choices: **what splits into cells** (facet), **what is drawn inside a cell** (cell view), and **what varies inside the drawing** (sweep). Every noun in the tool is a query into that grammar; every link is a query; every pin is a query. Uncertainty is not an error bar bolted onto a number — it *is* a sweep: a claim that survives its whole parameter range is solid, a claim that flips halfway through is not, and the officer sees the flip. And because raw x/y coordinates make comparison impossible (every fight is somewhere else), the tool carries three **unbent** coordinate systems — distance-to-front, distance-to-spawn, time-before-death — that put unlike moments onto the same axes so they can finally be laid side by side.

## 2. The ladder

**R0 — Concrete.** One event, one moment. In-world overlay at the cursor time; the Event page in the inspector.
*Up:* "See in context" turns the event into a 1×1 Grid of its engagement. *Down:* it is the ground; the world is right there.

**R1 — Over time.** Trails, engagement strips, the whole session as one picture in a Map cell.
*Up:* set a row facet — the one picture becomes many. *Down:* click anywhere on a trail or strip; cursor jumps to that instant, R0 redraws.

**R2 — Over parameters.** The Sweep dock. LOS verdict across the travel-time window; team assignment across the confidence threshold; damage across each adjustment script; attribution across the death window.
*Up:* "Sweep across cells" applies the same sweep to every cell in the Grid at once (rung 3). *Down:* click one step of the sweep and the world draws exactly that hypothesis — that candidate shooter position, that clustering.

**R3 — Small multiples.** The Grid proper: facet × facet, dozens of cells sharing one scale.
*Up:* Δ mode replaces each cell with its difference from a baseline. *Down:* click a cell; that cell's subset becomes the whole Grid (drill), or shift-click to open it as a 1×1.

**R4 — Unbent and differenced.** Cells drawn in front-distance, spawn-distance or death-aligned coordinates, in Δ-vs-baseline colour.
*Up:* nothing — this is the roof, and the tool says so. *Down:* every unbent chart is clickable and every axis it uses is also **drawn in the world** (front isoline on the ground, spawn distance rings), so a point on the abstraction has a visible twin in the region.

The transitions are the product. The Grid header, the breadcrumb chips, and the world overlay all share one cursor and one hover: hovering a cell highlights its avatars in world, hovering a world marker outlines the cells it belongs to.

## 3. Information architecture

**One query struct rules everything.** `Query = {filter, rowFacet, colFacet, cellView, sweep, baseline, timeWindow}`. The Grid renders a Query. A breadcrumb chip is a Query. A pin is a Query. Compare slots A/B/C hold Queries. A "link" on a page is a Query with a label. This is why the tool can be deep with only four custom LLViews.

**Nouns (pages in the inspector).** Each has a header, an at-a-glance sweep, related-events list, links, and backlinks ("appears in these cells"):

- **Session** — the whole raid; links to Engagements, Teams, Phases.
- **Engagement** — auto-segmented fight (see §6); links Deaths, participants, place, phase.
- **Death** — victim, killer, weapon, contributing damage, LOS fan, τ sweep.
- **Damage** — one hit; adjustment waterfall; siblings from the same burst.
- **Avatar** — the whole session of one person; state ribbon, unbent path, peer bands.
- **Pair (dyad)** — A vs B: every exchange, range-over-time, who won trades. The noun most tools forget, and the one that reveals teams.
- **Equipment** — weapon/HUD/deployable/vehicle/mount; users, damage profile, wall record, evidence for its classification.
- **Team** — cluster; membership over θ, friendly fire, equipment mix, front history.
- **Place** — a 16 m region cell or an officer-named landmark; deaths in it, who held it.
- **Minute / Phase** — a time slice; phases auto-labelled push / hold / collapse / lull.
- **Verdict** — CLEAR/BLOCKED/UNKNOWN as a page: all deaths with that verdict, ranked by flip fraction.

**Navigation.** One shared history stack for Grid + inspector; `Alt+←/→`; a breadcrumb chip bar that is editable (click a chip to truncate, drag a chip into slot A/B). Pinning copies a cell *and its Query* to the Board. Backlinks are computed by re-running the current facets and testing membership — cheap, because facet membership is already indexed.

## 4. Screens

### 4.1 Main floater — `ss_combat_log` (resizable, ~980×640)

```
+= COMBAT LOG ==================================================== [_][#][X] =+
| REC *  bridge OK  ev 41,203  trk 2.1M  agents 37  rtt 180ms  drop 0  mem 71M|
+-----------------------------------------------------------------------------+
| [|<][<<][ > ][>>][>|]  spd 1x v |  14:32:07.4  | [LIVE] [world follows]      |
| 13:00     13:30     14:00     14:30     15:00        deaths | dmg rate       |
| ..:_:_.:###.:_..::_:####:..._.:.:####|##:.._..:.._.:_...::..:.._....         |
|   [E1]     [E2]        [E3]      [E4]^      [E5]        ^cursor              |
+---------------+-------------------------------------------------------------+
| COMPARE       | ROWS [Engagement v]  COLS [Team v]  CELL [Map v]             |
| A [#] Ashgd-1 | scale [locked v]  D vs [none v]  sort [deaths v]  n>= [3]    |
| B [O] Ashgd-2 +-------------------------------------------------------------+
| C [ ] --      |         |  ASHGUARD-1   |  ASHGUARD-2   |  UNALIGNED        |
| [swap] [A-B]  | E2      | +-----------+ | +-----------+ | +-----------+     |
|               | 13:41   | |  \  X     | | |   X X     | | |     .     |     |
| FILTER        | 6 deaths| | \  \X .   | | | \   .     | | |///////////|     |
| [x] deaths    | 3m12s   | |    X      | | |  X        | | |//  n=1  //|     |
| [x] damage    |         | +-----------+ | +-----------+ | +-----------+     |
| [ ] custom    |         | 4 dth [--o-] | 2 dth [-o--]  | 0 dth   thin       |
| type  [all v] | E3      | +-----------+ | +-----------+ | +-----------+     |
| weapon[all v] | 14:02   | | XX  \     | | | .  \X     | | |           |     |
| team  [all v] | 11 dths | |  \ X.     | | |   X       | | |  (empty)  |     |
| conf  >=[0.6] | 5m40s   | +-----------+ | +-----------+ | +-----------+     |
|               |         | 7 dth [---o] | 4 dth [-o--] |  --                 |
| PINNED (3)    +-------------------------------------------------------------+
| > D#47 tau-fan| SWEEP [LOS travel tau v]   0.0 s ----o------ 1.5 s  [step >] |
| > Vex peers   | tau  .0  .1  .2  .3  .4  .5  .6  .7  .8  .9 1.0 1.2 1.5     |
| > AR15 wall   | D#47 ##  ##  ..  ..  //  //  //  //  ..  ##  ##  ##  ##     |
| [Board...]    | legend: ## clear   .. blocked   // no track   clear 6/13    |
+---------------+-------------------------------------------------------------+
```

**Regions.** *Status line*: recording health, honest counters (dropped ticks, poll RTT) — the officer must be able to distrust the data. *Transport + timeline*: an `SSTimelineView`; the density band is deaths above the axis, damage rate below; engagement segments are named brackets `[E1]`; drag scrubs, shift-drag sets the time window that the whole Grid respects. *Compare rail*: three slots holding Queries; `A−B` switches the Grid into Δ mode. *Filters*: standard XUI combos/checkboxes, plus a confidence floor that every inference in every cell must clear. *Grid controls*: row facet, column facet, cell view, scale lock (shared vs per-cell — locked by default, because unlocked small multiples lie), baseline, sort, minimum n. *Grid*: `SSGridView` of `SSCellView`s with row/column headers that are themselves links. Under each cell: the headline number and a **spread bar** `[--o-]` showing where that number sits within its sweep range — no number is ever shown naked. Hatched `///` = insufficient data. *Sweep dock*: `SSSweepView`; a parameter axis and one row per selected item; clicking a step pushes that hypothesis to the world.

The plan's Events/Avatars/Equipment/Teams tabs survive as the four **preset buttons** that set `rowFacet` and `cellView` (Events→Death/Strip, Avatars→Avatar/Band, Equipment→Equipment/Bars, Teams→Team/Ribbon). Nothing is lost; a tab is just a saved Query.

### 4.2 Inspector — `ss_combat_inspector` (the page)

```
+= DEATH #47 ===================================================== [_][#][X] =+
| < back  Session > Ashguard-1 > E3 > Death #47            [pin] [A][B] [3D]  |
+-----------------------------------------------------------------------------+
| Vex Nightfall  killed by  Ruen Calder   14:32:07.4   dist 41.2 m            |
| weapon: "M4 Carbine" (handheld, conf 0.82)   type 102 explosive  dmg 63     |
+-----------------------------------------------------------------------------+
| LOS across bullet travel                     aim point                       |
|            head  ## ## .. .. // // // // .. ## ## ## ##   clear 7/13         |
|            chest ## .. .. .. // // // // .. .. ## ## ##   clear 5/13         |
|            feet  .. .. .. .. // // // // .. .. .. ## ##   clear 3/13         |
|            tau   .0 .1 .2 .3 .4 .5 .6 .7 .8 .9 1.0 1.2 1.5                  |
| headline: CLEAR shot exists (flip fraction 0.54, track gap 0.4-0.7 s)        |
| caveat: log lag <=1 s, sim batching; a clear cell is possible, not proven.   |
+-----------------------------------------------------------------------------+
| Damage that led here (window 20 s v)      | Compared with                    |
| 14:31:52 Ruen  M4     18 -> 14  (armour)  | this pair    11 exchanges        |
| 14:32:01 Ruen  M4     22 -> 22            | Vex's deaths  9 (this: median)   |
| 14:32:07 Ruen  M4     63 -> 63  KILL      | M4 kills     34 (wall 4)         |
| 14:32:03 Sella grenade 9 -> 4  (armour)   | E3 deaths    11                  |
+-----------------------------------------------------------------------------+
| links: Vex | Ruen | Pair Vex-Ruen | M4 Carbine | E3 | Place H7 | Ashguard-1  |
| backlinks: cells [E3 x Ashgd-1], [M4 x wall], [minute 14:32], board pin #1   |
+-----------------------------------------------------------------------------+
```

Every page has the same skeleton: header + breadcrumb + pin/compare/3D buttons, a sweep panel, a body table, a **Compared with** column that is always populated (the peer context is not optional), and links + backlinks. Avatar pages swap the sweep panel for the state ribbon and peer bands; Equipment pages for the wall-record caterpillar; Team pages for the θ ribbon.

### 4.3 Board — `ss_combat_board` (debrief deck)

```
+= BOARD (debrief) ================================================ [_][#][X] =+
| [add current cell] [reorder] [copy as text] [clear]      6 pins              |
| 1 +-----------+  2 +-----------+  3 +-----------+                            |
|   | front v t |    | tau fan   |    | wall bars |                            |
|   |  \___/‾‾  |    | ## .. ##  |    | ###### M4 |                            |
|   +-----------+    +-----------+    +----- 4 --+                             |
|   E3 front lost    D#47 flips at   caterpillar: only M4                     |
|   28 m in 90 s     tau 0.2-0.8     interval excludes 0                      |
+-----------------------------------------------------------------------------+
```

Pins keep their Query, so clicking one restores the exact Grid state. "Copy as text" writes a plain-text after-action summary to the clipboard (no files in v1).

## 5. In-world overlay

Drawn only under the plan's gating (stationary + alt-cam + 5 s grace).

**R0 (cursor):** feet ring + yaw arrow per avatar in team colour; eye glyph when MOUSELOOK; damage lines from attacker track to victim track, hue by damage type, brightness by damage, 1 s pulse; deaths as a cross + ring at `target_pos` with a killer line from `source_pos`.

**R1 (window):** trails as polylines, alpha ramped old→new, width bucketed by sample quality (coarse = thin, dashed across gaps >5 s). **Stroboscope**: for a selected avatar, faded rings every 2 s with tiny time labels — all moments at once.

**R2 (parameter, in world):** the **τ-fan** — one ray per candidate shooter position over the travel window, from the attacker's swept path to the victim, green for clear, red for blocked, grey for no track; each blocked ray gets a small `x` at its geometry hit point and the blocking surface gets a 1 m normal disc (the **wall witness**). This is the sweep from the dock, drawn in space. Selecting a step in the dock brightens exactly that ray.

**R3 (small multiples, in world):** the **lattice** — an 8 m ground grid over the fight area whose cells are filled by a chosen quantity (deaths, damage taken, exposure seconds), quantised into 5 steps of alpha. In Δ mode it goes diverging (A above, B below, neutral transparent). **Ghost pairs**: slot A drawn solid, slot B drawn hollow/dotted with the same geometry — two teams, two phases, or the same team in two engagements, overlaid.

**R4 (unbent axes, in world):** the **front ribbon** — the F=0 isoline of the front field drawn as a ground polyline at the cursor time plus faded past isolines every 10 s (a contour stroboscope of the front moving). **Spawn rings** — labelled concentric rings at 25/50/100 m around each team's spawn anchor. These are literally the axes of the unbent charts, drawn on the terrain, so a point on a chart has an obvious home in the world.

**Uncertainty encodings (one legend, everywhere).** Dotted outline = inferred below the confidence floor. Desaturation ∝ confidence. Hatching (2D) / stipple (3D) = missing or too-thin data. Dashed segments = interpolated across track gaps. A marker whose position came from a coarse location gets a ±2 m vertical bar instead of a crisp ring. Nothing that is unknown is ever drawn as if known.

**Clicking.** Rings, crosses, damage lines, fan rays, lattice cells and isoline segments all push screen rects during render and open their page on plain left-click (ALT/CTRL untouched, so alt-cam still works). Lattice cell → Place page. Fan ray → the τ step. Isoline segment → the Phase page.

## 6. Analysis model

**Engagement segmentation.** Single-link clustering of damage+death events: same cluster if within 20 s and 25 m of any member (position from the victim's track). Cells with <3 events become "scattered". Uncertainty: events with no track for either party are attached by time only and marked provisional.

**Team clustering across θ (rung 2 by construction).** Graph nodes = avatars; edge weight = damage exchanged (symmetrised, log-scaled to stop one grenade dominating); seeds = shared active group. Rather than one clustering, run label propagation for `K = 2..5` and confidence threshold `θ ∈ {0.0 … 1.0, step 0.05}`, matching cluster ids across runs by maximum overlap. Output the **allegiance ribbon**: rows = avatars, x = θ, colour = cluster. **Allegiance stability** = fraction of θ at which an avatar keeps its modal cluster. Officers override per avatar; overrides pin that row and are drawn with a lock glyph. **Friendly fire** = intra-cluster damage at the current θ, reported as a rate `ff = intra / total` per avatar and team, with its own spread bar across θ — because in an intra-group skirmish the *whole question* is whether that damage is friendly fire or evidence of a second team, and the sweep shows exactly where the answer changes.

**Front field and d_front.** At time t, `F(x) = Σ_A k(|x−p_i|) − Σ_B k(|x−p_j|)` with a Gaussian kernel, σ = 12 m, over living tracked avatars, weighted by team confidence. `d_front(a,t) ≈ F(p_a)/|∇F(p_a)|`, signed positive toward the avatar's own side. Undefined (drawn as a gap) when |∇F| is tiny or fewer than 3 avatars per side are tracked. Sweeping σ ∈ [6,24] m is offered; a front that only exists at one σ is not a front.

**Spawn anchor and d_spawn.** Cluster each team's post-respawn positions (first sample ≥3 s after a death); anchor = densest cluster centroid; confidence = fraction of respawns within 15 m. `d_spawn` is path distance from the anchor. Respawn cycles show up as sawtooth Sparkpaths — the shape that answers "how much time was spent walking back".

**Death-aligned time.** `τ_d = t − t_death`, so all deaths stack on one axis: the last 30 s before every death, overlaid, is the single most comparative picture the tool draws.

**LOS sweep and flip fraction.** For a death/damage at t, sample candidates `t − τ` for τ over `[0, SSCombatLogTravelSeconds]` in 0.1 s steps × 3 aim points (head/chest/feet) × 2 eye heights → up to ~78 raycasts. Headline verdict CLEAR if any cell is clear; **flip fraction** = fraction of sampled cells disagreeing with the headline; **coverage** = fraction with track data. A verdict with flip fraction 0.05 is worth acting on; 0.5 is not, and the officer sees which. Budget: raycasts are computed lazily per selected event, capped at ~200/frame with a progress chip; whole-weapon tallies run in the background over the visible rows only.

**Through-wall record per weapon.** `p_wall` = proportion of that weapon's kills with **zero** clear candidates, shown as a **caterpillar plot**: weapons sorted by `p_wall`, each with a Wilson 95% interval and `n` printed. A weapon is only interesting when its interval excludes the session baseline — comparison, not accusation.

**Adjustments.** Per damage event `r = damage/initial`; per modification script, the delta it contributed. Cell view Waterfall shows initial → each script → final. Facet by team or avatar to answer "who is running armour, and how much does it buy them": mean `1 − r` weighted by `initial`, with the distribution shown, not just the mean.

**Behaviour metrics, always as peer bands.** *Reaction time*: from the first instant in the preceding 10 s at which LOS attacker→victim was clear **and** the victim was within 60° of the attacker's yaw, to the first damage. *Pre-aim through wall*: seconds an attacker's yaw stayed within 15° of a victim while LOS was blocked, immediately before firing. *Mouselook share*, *speed profile*, *seat time*: straight from bridge flags. Each is shown as the population strip with the subject's tick on it, plus percentile and n, plus a printed confound list (track sampling, sound cues, prior knowledge, lag). The tool never prints a verdict about a person.

**Δ vs baseline.** Baselines: none, session mean, peer (same team), self-earlier-half, slot B. Cells in Δ mode use a diverging ramp with a zero-anchored shared scale.

**Performance.** Facet membership is indexed once per Query change; cell geometry is cached and invalidated only by cursor/filter changes; the Grid caps at 60 visible cells; sweeps compute newest-first.

## 7. Walkthroughs

**Q1 — How did the raid go?** Open the floater; the default Query is Rows=Engagement, Cols=Team, Cell=Map, scale locked. Five map cells per team, one row per engagement, shared extent: at a glance, E2 and E4 happened at the same choke, E3 sprawled. Switch Cell to Sparkpath and the y-axis becomes `d_front`: Ashguard-1's line climbs through E2 (pushing), collapses 28 m in 90 s during E3 (pinned), recovers in E5. Switch Cols to Phase to see the same rows sliced push/hold/collapse/lull. Set Δ vs = session mean: the two cells that light up red are the answer to "where did it go wrong". Pin both.

**Q3 — Why did X die at 14:32?** Click the death tick on the timeline (or the cross in world). The Death page opens with the contributing-damage table already grouped by attacker+weapon, and the τ×aim-point sweep grid at the top: clear at 7 of 13 τ steps for a head aim, 3 of 13 for feet. Press `3D`: the world draws the τ-fan from Ruen's swept path, red rays with `x` marks on the wall between them, the blocking surface discs visible. Drag the sweep dock to τ = 0.5 and the world shows the single hypothesis: Ruen behind the container, no line. Verdict: a clear shot exists, flip fraction 0.54, so this is *not* evidence of a wall kill. Click "Vex's deaths" in the Compared with column → a Grid of all nine of Vex's deaths in death-aligned time; the M4 kill is unremarkable.

**Q8 — Did anyone behave suspiciously?** Rows=Avatar, Cell=Band, metric=pre-aim-through-wall. Thirty-seven strips, one shared scale, sorted descending. Two avatars sit outside the population box. Click the top one: the Avatar page shows n = 6 engagements, percentile 97, and the confound list. Set Cell=Fan and sweep τ across their six engagements at once — four of the six flip to CLEAR somewhere in the window, two never do. Pin those two. The tool's output is "two engagements where no clear line exists at any tested time, from an avatar in the 97th percentile for pre-aim, n = 6" — evidence with its uncertainty, handed to a human.

**Intra-group skirmish.** Both teams wear Ashguard tags, so group seeds are useless. Open Teams: the allegiance ribbon over θ shows a clean two-way split that holds from θ = 0.15 up to θ = 0.7, with three avatars whose rows flicker — the medics who healed across the line and the one person who arrived late. Set the confidence floor to 0.5 and the flickering rows go dotted everywhere in the tool. Friendly-fire rate for the stable members is 3% with a narrow spread bar across θ; for the flickering three it is 40% with a spread bar covering half the range — which is the tool saying "these three are not classified, don't call this friendly fire". Officer overrides two of them from the Team page; the ribbon locks those rows and every cell recomputes.

**FFA on a neutral sim.** Team clustering reports K = 5 with low stability and the front field refuses to resolve (fewer than 3 confident avatars per side) — the Sparkpath cell shows a "front undefined" hatch rather than a fake line. The tool switches the unbent axis to **nearest-enemy distance** and the default Query to Rows=Pair, Cell=Matrix: the who-shot-whom matrix over 5-minute columns shows transient alliances as blocks that appear and dissolve. Deaths are still fully attributable, equipment stats and wall records are unaffected (they never needed teams), and the honest statement "no stable teams detected" sits in the Teams tab instead of a made-up two-colour map.

## 8. Risks and open questions

- **Raycast cost.** The full τ × aim × eye sweep is ~78 casts per event; a session-wide weapon tally could be tens of thousands. Mitigation is lazy, visible-rows-first, capped per frame, cached per event — but a "compute all wall verdicts" button needs a progress bar and a cancel.
- **Small multiples lie without shared scales.** Scale lock is default-on, but locked scales hide detail in small cells. The spread bar helps; a per-cell "scale broken" glyph is needed when a cell clips.
- **The front field is a model, not a fact.** It is meaningless in FFA and fragile with <6 tracked avatars. The design refuses to draw it rather than draw it wrong; officers may still over-trust it when it *is* drawn.
- **Clustering over-fits.** Label propagation on damage graphs will happily split a team that had one bad friendly-fire minute. The θ ribbon exposes this, but the default θ choice is a value judgement that needs playtesting.
- **Colour budget.** Past four teams, hues run out; shape-coded markers (ring/square/triangle/hollow) carry identity beyond that, at a cost in legibility at distance.
- **Text density.** The Grid fits ~12 characters per cell label at 100% UI scale; at 150% labels must drop to numbers only. Needs a real test at the officer's scale.
- **Open:** is the Pair the right default noun for FFA? The matrix is dense at 37 agents and needs an exchange-count threshold nobody has calibrated. And should the τ sweep be precomputed live per death? It would make the tool instant, at the cost of raycasts during combat, exactly when frames matter.
