# Entry 11 — Comparative: the raid as a grid of differences

## 1. Thesis

An officer never really asks "what happened?" — they ask "compared to what?". Was this push worse than the last one? Does this weapon kill through walls more than the other twelve? Is Vex's reaction time weird, or is everyone that fast? Would the verdict still be BLOCKED if the bullet had flown 0.4 s instead of 0.1 s? So this design has no summary/detail split: there is **one grammar, and every screen is a query in it**. The grammar has three choices — **what splits into cells** (facet), **what is drawn inside a cell** (cell view), **what varies inside the drawing** (sweep) — and the Grid is simply its widest expression. The eleven noun pages are not a different kind of screen; each is that same query narrowed to one subject, and every link, pin, breadcrumb and compare slot is literally a `Query` value. Uncertainty is not an error bar bolted onto a number — it *is* a sweep: a claim that survives its whole parameter range is solid, a claim that flips halfway through is not, and the officer sees the flip. And because raw x/y coordinates make comparison impossible (every fight is somewhere else), the tool carries three **unbent** coordinate systems — distance-to-front, distance-to-spawn, time-before-death — that put unlike moments onto the same axes so they can finally be laid side by side. Each unbent axis is allowed to refuse to exist when the data does not support it.

## 2. The ladder

**R0 — Concrete.** One event, one moment. In-world overlay at the cursor time; the Event page in the inspector.
*Up:* "See in context" turns the event into a 1×1 Grid of its engagement. *Down:* it is the ground; the world is right there.

**R1 — Over time.** Trails, engagement strips, the whole session as one picture in a Map cell.
*Up:* set a row facet — the one picture becomes many. *Down:* click anywhere on a trail or strip; cursor jumps to that instant, R0 redraws.

**R2 — Over parameters.** The Sweep dock. LOS verdict across the travel-time window; team assignment across the confidence threshold θ **and across time windows**; damage across each adjustment script; attribution across the death window.
*Up:* "Sweep across cells" applies the same sweep to every cell at once (rung 3). *Down:* click one step and the world draws exactly that hypothesis — that candidate shooter position, that clustering.

**R3 — Small multiples.** The Grid proper: facet × facet, dozens of cells sharing one scale.
*Up:* Δ mode replaces each cell with its difference from a baseline. *Down:* click a cell; that subset becomes the whole Grid (drill), or shift-click to open it as a 1×1.

**R4 — Unbent and differenced.** Cells drawn in front-distance, spawn-distance or death-aligned coordinates, in Δ-vs-baseline colour.
*Up:* nothing — this is the roof, and the tool says so. *Down:* every unbent chart is clickable and every axis it uses is also **drawn in the world** (front isoline on the ground, spawn rings), so a point on the abstraction has a visible twin in the region.

The transitions are the product. Grid header, breadcrumb chips and world overlay share one cursor and one hover: hovering a cell highlights its avatars in world, hovering a world marker outlines the cells it belongs to.

## 3. Information architecture

**One query struct rules everything.** `Query = {filter, rowFacet, colFacet, cellView, sweep, baseline, timeWindow}`. The Grid renders a Query. A breadcrumb chip is a Query. A pin is a Query. Compare slots A/B/C hold Queries. A "link" on a page is a Query with a label. A noun page is a Query whose filter is a single subject. Custom drawing surface, stated honestly: four `LLView` subclasses (`SSTimelineView`, `SSGridView`, `SSSweepView`, `SSCellView`) plus **seven paint routines** inside `SSCellView` (Map, Sparkpath, Waterfall, Matrix, Band/Ribbon, Fan, Caterpillar) — roughly 2.5k lines of primitive drawing, not four widgets' worth.

**Nouns (pages in the inspector).** Each has a header, an at-a-glance sweep, related-events list, links, and backlinks ("appears in these cells"): **Session**, **Engagement** (auto-segmented, §6), **Death** (LOS fan, τ sweep), **Damage** (adjustment waterfall, burst siblings), **Avatar** (state ribbon, unbent path, peer bands, allegiance history), **Pair** (A vs B: every exchange, range over time, who won trades — the noun most tools forget and the one that reveals teams), **Equipment**, **Team**, **Place**, **Minute/Phase**, **Verdict** (all deaths with a verdict, ranked by flip fraction).

**Navigation.** One shared history stack for Grid + inspector; `Alt+←/→` restores the exact Query *and* cursor time. The breadcrumb chip bar is editable: click a chip to truncate to it, drag a chip into slot A/B. Pinning copies a cell *and its Query* to the Board. Backlinks re-run the current facets and test membership — cheap, because facet membership is already indexed.

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
| COMPARE       | ROWS [Engagement v]  COLS [Team v]  CELL [Map v]  12 of 19   |
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
|               |  ...    | 7 dth [---o] | 4 dth [-o--] |  --                 |
| PINNED (3)    | +7 rows | +-- overflow: 7 engagements, 22 deaths ------+     |
| > D#47 tau-fan+-------------------------------------------------------------+
| > Vex peers   | SWEEP [LOS travel tau v]   0.0 s ----o------ 1.5 s  [step >] |
| > AR15 wall   | tau  .0  .1  .2  .3  .4  .5  .6  .7  .8  .9 1.0 1.2 1.5     |
| [Board...]    | D#47 ##  ##  ..  ..  //  //  //  //  ..  ##  ##  ##  ##     |
|               | flips 0.54  coverage 9/13  [exists clear]  (headline chip)  |
+---------------+-------------------------------------------------------------+
```

**Regions.** *Status line*: recording health and honest counters (dropped ticks, poll RTT) — the officer must be able to distrust the data. *Transport + timeline*: `SSTimelineView`; deaths above the axis, damage rate below, engagement brackets `[E1]`; drag scrubs, shift-drag sets the window the whole Grid respects. *Compare rail*: three Query slots; `A−B` switches to Δ mode. *Filters*: XUI combos and checkboxes plus a confidence floor every inference must clear. *Grid controls*: facets, cell view, scale lock (shared by default — unlocked small multiples lie), baseline, sort, minimum n, and a **"12 of 19" chip**: the Grid caps at 60 cells, so rows past the cap collapse into one aggregated **overflow row** (never silently dropped) that re-facets on click. *Grid*: `SSGridView` of `SSCellView`s, headers are links; under each cell a headline number plus a **spread bar** `[--o-]` locating it in its sweep range — no number is shown naked. Hatch `///` = insufficient data, clock glyph = not yet computed. *Sweep dock*: `SSSweepView`; clicking a step pushes that hypothesis to the world.

The plan's Events/Avatars/Equipment/Teams tabs survive as four **preset buttons** setting `rowFacet` + `cellView`. Nothing is lost; a tab is a saved Query.

### 4.2 Inspector — `ss_combat_inspector` (the page)

```
+= DEATH #47 ===================================================== [_][#][X] =+
| < back  Session > Ashguard-1 > E3 > Death #47            [pin] [A][B] [3D]  |
+-----------------------------------------------------------------------------+
| Vex Nightfall  killed by  Ruen Calder   14:32:07.4   dist 41.2 m            |
| weapon: "M4 Carbine" (handheld, class conf 0.82, creator RC-3f2, 1 of 2     |
|          objects sharing this name -> [split rows])   type 102  dmg 63      |
+-----------------------------------------------------------------------------+
| LOS across bullet travel                     aim point                       |
|            head  ## ## .. .. // // // // .. ## ## ## ##   clear 7/13         |
|            chest ## .. .. .. // // // // .. .. ## ## ##   clear 5/13         |
|            feet  .. .. .. .. // // // // .. .. .. ## ##   clear 3/13         |
|            tau   .0 .1 .2 .3 .4 .5 .6 .7 .8 .9 1.0 1.2 1.5                  |
| flip fraction 0.54   coverage 9/13   [a clear shot exists]                   |
| reading: the verdict is unstable across travel time - not wall-kill evidence |
| caveat: log lag <=1 s, sim batching; a clear cell is possible, not proven.   |
+-----------------------------------------------------------------------------+
| Damage that led here (window 20 s v)      | Compared with                    |
| 14:31:52 Ruen  M4     18 -> 14  (armour)  | this pair    11 exchanges        |
| 14:32:01 Ruen  M4     22 -> 22            | Vex's deaths  9 (this: median)   |
| 14:32:07 Ruen  M4     63 -> 63  KILL      | M4 kills     34 (wall 4 of 31    |
| 14:32:03 Sella grenade 9 -> 4  (armour)   |               classified)        |
|                                           | E3 deaths    11                  |
+-----------------------------------------------------------------------------+
| links: Vex | Ruen | Pair Vex-Ruen | M4 Carbine | E3 | Place H7 | Ashguard-1  |
| backlinks: cells [E3 x Ashgd-1], [M4 x wall], [minute 14:32], board pin #1   |
+-----------------------------------------------------------------------------+
```

Every page has the same skeleton: header + breadcrumb + pin/compare/3D, a sweep panel, a body table, an always-populated **Compared with** column (peer context is not optional), links and backlinks. Avatar pages swap the sweep panel for the state ribbon and peer bands, Equipment pages for the wall-record caterpillar, Team pages for the allegiance field below.

### 4.3 Team page — the allegiance field (θ × time)

```
+= TEAMS ========================================================= [_][#][X] =+
| window [phase v]  half-life [6 min]  theta [0.45 o----]  K best 2  [locks 2] |
|            13:10   13:40   14:10   14:40   15:10      stability  n  note     |
| Ruen       AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA      .94    412          |
| Sella      AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA      .91    377          |
| Corvin  [L]BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB      1.0     88  locked  |
| Tamsin     BBBBBBBBBBBBB>AAAAAAAAAAAAAAAAAAAAAAAAAA      .86    203  DRIFT   |
| Marek      ///////AAAAAAAAAA///////AAAAAAA//////////     .38    141  noise   |
| Juno                       AAAAAAAAAAAAAAAAAAAAAAAA      .77     46  short   |
+-----------------------------------------------------------------------------+
| Tamsin changed side at 14:41 (+/- 1 window); stability .84 before, .88 after |
| Marek: unstable within windows at every theta -> not classified              |
| friendly fire (theta 0.45): stable members 3% [-o--]  unclassified 40% [ooo] |
+-----------------------------------------------------------------------------+
```

Rows = avatars, x = **time windows**, colour = modal cluster, saturation ∝ θ-stability inside that window. A `>` chevron marks a genuine side change; hatching marks θ-instability; a `short` badge marks small-sample windows; `[L]` is an officer lock.

### 4.4 Board — `ss_combat_board` (debrief deck)

```
+= BOARD (debrief) ================================================ [_][#][X] =+
| [add current cell] [reorder] [copy as text] [clear]      6 pins              |
| 1 +-----------+  2 +-----------+  3 +-----------+                            |
|   | front v t |    | tau fan   |    | wall bars |                            |
|   |  \___/~~  |    | ## .. ##  |    | ###### M4 |                            |
|   +-----------+    +-----------+    +----- 4 --+                             |
|   E3 front lost    D#47 flips at   caterpillar: only M4                     |
|   24-31 m / 90 s   tau 0.2-0.8     interval excludes baseline               |
+-----------------------------------------------------------------------------+
```

Pins keep their Query, so clicking one restores the exact Grid state and cursor. "Copy as text" writes a plain-text after-action summary — including any open flags and their dispositions — to the clipboard (no files in v1).

## 5. In-world overlay

Drawn only under the plan's gating (stationary + alt-cam + 5 s grace).

**R0 (cursor):** feet ring + yaw arrow per avatar in team colour, eye glyph when MOUSELOOK, damage lines attacker-track → victim-track (hue by damage type, brightness by damage, 1 s pulse), deaths as cross + ring at `target_pos` with a killer line from `source_pos`.

**R1 (window):** trails as polylines, alpha old→new, width bucketed by sample quality (coarse thin, dashed across gaps >5 s). **Stroboscope**: faded rings every 2 s with time labels — all moments at once.

**R2 (parameter):** the **τ-fan** — one ray per candidate shooter position over the travel window, green clear / red blocked / grey no-track; each blocked ray gets an `x` at its geometry hit and the blocking surface a 1 m normal disc (the **wall witness**). A dock step brightens exactly its ray.

**R3 (small multiples):** the **lattice** — an 8 m ground grid filled by a chosen quantity, 5 alpha steps, diverging in Δ mode. **Ghost pairs**: slot A solid, slot B hollow/dotted, same geometry.

**R4 (unbent axes):** the **front ribbon** — the F=0 isoline as a ground polyline at the cursor, plus faded past isolines every 10 s. **Spawn rings** at 25/50/100 m, *only when the respawn test in §6 passes*; otherwise no rings and the Sparkpath's spawn axis is hatched.

**Density and LOD (default-on).** The overlay draws, by default: the current selection and its related nouns, all Board pins, and avatars within 96 m of the camera focus. Trails beyond that set are opt-in. Per-layer caps: ≤80 trails, ≤200 damage lines, ≤400 lattice cells, ≤128 fan rays; labels culled past 256 m and thinned to ≥18 px separation; ring segments fall with distance. A corner chip reads `drawing 62 of 214 markers (LOD)`; clicking widens one step. The officer is told what is hidden, never silently shown a subset.

**Uncertainty encodings (one legend, everywhere).** Dotted outline = inferred below the confidence floor. Desaturation ∝ confidence. Hatching (2D) / stipple (3D) = missing or too-thin data. Clock glyph = not computed yet. Dashed segments = interpolated across gaps. A marker from a coarse location gets a ±2 m vertical bar instead of a crisp ring. Nothing unknown is drawn as if known.

**Clicking, and how it avoids fighting the camera.** Markers push screen rects during render. Selection resolves on mouse-**up**, and only if pointer travel since mouse-down stayed under 4 px and the press was under 350 ms; any drag cancels the pick, and any ALT/CTRL/SHIFT-modified press is never consumed, so camera control (ALT+drag orbit) and the pick tool keep their gestures. Picks use a 12 px radius, nearest first; ties raise a small pick-stack list at the cursor ("3 here: death #47 / Ruen trail / lattice G4") instead of guessing. Lattice cell → Place page. Fan ray → the τ step. Isoline segment → Phase page.

## 6. Analysis model

**Engagement segmentation (trough-first, spatially split, capped).** (1) Damage rate `r(t)` in 5 s bins, smoothed over 15 s; cut at local minima where `r` falls below 20 % of the surrounding 90 s peak for ≥20 s. (2) Inside each block, single-link split on **position only** (25 m radius, position from the victim's track — DAMAGE has none) to separate simultaneous fights. (3) Caps: 6 min duration, 80 m diameter; overflow forces a recursive split at the deepest interior minimum of `r`. (4) **Fallback:** if a continuous grinder still yields one block, the row facet auto-switches to **Slice** (3-min bins) with a hatched header reading "sliced — no segmentation found". Q1 works unchanged, because the row facet is just a Query field. (5) Events with no track for either party attach by time only and are marked provisional.

**Team clustering across θ *and* across time.** Nodes = avatars; edge weight = damage exchanged, symmetrised, log-scaled, with an exponential **half-life of 6 min**, and normalised per avatar by their tracked-and-armed seconds in the window — so a 25-minute late arrival is judged on rate, not volume. Windows are the auto Phases (or a fixed 2-min grid). Label propagation runs for `K = 2..5` × `θ ∈ {0…1, step 0.05}` **per window**; cluster ids are matched across θ and across adjacent windows by maximum overlap. Outputs: **allegiance stability** (fraction of θ keeping the modal cluster inside a window) and, crossing windows, two diagnostics a single whole-session clustering cannot tell apart — **drift**: modal cluster changes between adjacent windows while stability stays high on both sides → chevron plus a printed change time ("changed side at 14:41 ± 1 window"); **noise**: stability low inside the window → hatch, no chevron. Sample size is its own badge (`short`), never conflated with ambiguity, so an officer reading a low-confidence late arrival knows it is a window-length artefact. Officer overrides per avatar (whole session or one interval) draw a lock glyph. **Friendly fire** = intra-cluster damage at the current θ and window, reported as `ff = intra/total` with its own spread bar across θ — in an intra-group skirmish the whole question is whether that damage is friendly fire or a second team, and the sweep shows where the answer changes.

**Front field and d_front.** `F(x) = Σ_A k(|x−p_i|) − Σ_B k(|x−p_j|)`, Gaussian kernel, over living tracked avatars weighted by team confidence **at θ\***, where θ\* = the threshold maximising mean allegiance stability in the current window (printed in the axis label). Because F is an inference over an inference, the axis is swept: 3 θ (θ\*, θ\*±0.15) × 3 σ (6/12/24 m) = 9 fields; `d_front = F(p)/|∇F(p)|` is the median with a band, and any cell whose **sign** disagrees across the sweep is hatched instead of drawn. Undefined when |∇F| is tiny, fewer than 3 confident avatars per side exist, or the window has no stable clustering. Fields cache on a 4 m lattice per cursor time with 100 ms debounce during scrub-drag; past isolines are cached polylines.

**Spawn anchor and d_spawn — tested, not assumed.** The brief gives no respawn event and no game rules, so the tool *tests* for a respawn pattern before offering the axis. Per death with bridge samples: the victim's next sample within 4 s lying >15 m away at an implied speed >20 m/s is relocation evidence; a continuous path is a walk; no bridge data is unknown. Over ≥8 decided deaths, if the team's relocation fraction φ < 0.5 the tool prints "no respawn pattern detected", disables `d_spawn`, hatches its charts and omits the spawn rings — the same refusal it applies to the front field. When φ passes, anchor = densest centroid of relocation destinations, confidence = fraction within 15 m ("spawn anchor 0.78, 23 of 29"), and sawtooth Sparkpaths answer "how much time was spent walking back".

**Death-aligned time.** `τ_d = t − t_death`; the last 30 s before every death, overlaid, is the most comparative picture the tool draws.

**LOS sweep and flip fraction.** Sample `t − τ` over `[0, SSCombatLogTravelSeconds]` in 0.1 s steps × 3 aim points × 2 eye heights → ≤78 raycasts. The headline is **flip fraction** (fraction of sampled cells disagreeing with the majority) and **coverage**; the existential result appears only as a small `[a clear shot exists]` chip, because that test is uninformative when true and decisive when false — "no clear line at any τ" is the actual wall-kill claim.

**Behaviour metrics, with a real cast budget.** *Reaction time*: from the first instant in the preceding 10 s where LOS attacker→victim was clear **and** the victim was within 60° of attacker yaw, to first damage. *Pre-aim through wall*: seconds of yaw within 15° of a victim while LOS was blocked, immediately before firing. Both are computed coarse-to-fine, not per track sample: 0.5 s stride over the pre-window (≤20 casts) then binary refinement around the first transition (≤6), so ≤26 casts per event. All casts share one **pool of 200/frame** (priority: selected event > visible Grid rows > background tallies) with a progress chip and cancel; verdicts memoise in an LRU keyed by attacker/victim positions quantised to 1 m — avatars stand still, so hit rate is high — invalidated when track backfill moves a sample. Pending cells show the clock glyph. *Mouselook share*, *speed profile*, *seat time* come straight from bridge flags. Every metric is a population strip with the subject's tick, percentile, n, and a printed, explicitly non-exhaustive confound list. The tool never prints a verdict about a person; the officer records one, see below.

**Equipment identity (names are labels, not keys).** Object names are user-editable and spoofable, so the aggregate key is `(creator, root name, attach-point class, damage-type signature)`, never the display name alone. Two creators sharing "M4 Carbine" produce two rows plus a collision chip; the officer can merge or split keys, with a lock glyph. Classification confidence comes from evidence count (attach point known, rezzer chain known, seen visually vs bridge-only). **Through-wall record**: `p_wall` = proportion of kills with zero clear candidates, over kills whose classification confidence clears the floor, as a **caterpillar plot** with a Wilson 95 % interval and both `n_used/n_total` printed; excluded kills go into a permanent **UNATTRIBUTED** row with its own interval, never hidden and never merged into a named weapon. A weapon is interesting only when its interval excludes the session baseline — comparison, not accusation.

**Adjustments.** Per event `r = damage/initial`; per script, its delta. Waterfall shows initial → each script → final; facet by team or avatar for "who runs armour and what does it buy", showing the distribution, not just the mean. **Δ vs baseline:** none / session mean / peer / self-earlier-half / slot B, diverging ramp, zero-anchored shared scale.

**Flags and disposition.** Any cell, verdict or metric outlier can be flagged. A flag carries a disposition — `open` / `checked — plausible cause` / `escalated` — plus a free-text note, officer initials and a timestamp, session-local and included in "Copy as text". This closes the promotion/violation loop without the tool ever asserting intent.

**Performance.** Facet membership indexed once per Query change; cell geometry cached, invalidated by cursor/filter change; 60 visible cells with an overflow row; sweeps compute newest-first.

## 7. Walkthroughs

**Q1 — How did the raid go?** Default Query: Rows=Engagement, Cols=Team, Cell=Map, scale locked; the chip says "12 of 19", the overflow row holds the rest. At a glance E2 and E4 happened at the same choke, E3 sprawled. Switch Cell to Sparkpath: the y-axis becomes `d_front` at θ\*=0.45, and Ashguard-1 climbs through E2, then loses ground in E3 — the label reads "front collapses 24–31 m in 90 s (θ×σ sweep, sign stable)", not a bare number. Set Δ vs = session mean; two cells go red. **Drill and back:** click the red E3 × Ashguard-1 cell (breadcrumb `Session > E3 > Ashguard-1`), then click the worst minute inside it (`… > 14:07`), then a death cross in world (`… > Death #52`) — three levels down. `Alt+←` three times walks the breadcrumb back chip by chip, restoring both Query and cursor time, landing exactly on the original five-row Grid. Pin the two red cells on the way past. *If the raid was one continuous grinder*, segmentation reports no troughs and the row facet arrives as **Slice**, headers hatched and labelled — the same walkthrough, honestly relabelled.

**Q3 — Why did X die at 14:32?** Click the death tick on the timeline (or the cross in world — press and release without moving, or the pick is cancelled as a camera gesture). The Death page opens with contributing damage grouped by attacker+weapon and the τ×aim sweep at the top: clear at 7 of 13 τ steps for head, 3 of 13 for feet. Press `3D`: the world draws the τ-fan from Ruen's swept path, red rays with `x` marks and the blocking surface discs visible. Drag the dock to τ = 0.5 and the world shows the single hypothesis: Ruen behind the container, no line. Reading: flip fraction 0.54, coverage 9/13 — unstable, so **not** wall-kill evidence, whatever the `[a clear shot exists]` chip says. Click "Vex's deaths" in Compared with → all nine of Vex's deaths in death-aligned time; this one is unremarkable.

**Q8 — Did anyone behave suspiciously?** Rows=Avatar, Cell=Band, metric = pre-aim-through-wall. Thirty-seven strips, shared scale, sorted descending; cells resolve progressively (clock glyphs, cancel button live) because the cast pool is doing ≤26 casts per event newest-first. Two avatars sit outside the population box. Click the top one: n = 6 engagements, percentile 97, confound list printed. Set Cell=Fan and sweep τ across all six at once — four flip to CLEAR somewhere, two never do. Output: "two engagements with no clear line at any tested τ, from an avatar at the 97th percentile for pre-aim, n = 6" — evidence with its uncertainty. The officer flags it, sets disposition `checked — plausible cause: heavy foliage, see note`, and the flag travels into the debrief text with their initials.

**Intra-group skirmish.** Both teams wear Ashguard tags, so group seeds are useless. The Team page's allegiance field shows a clean two-way split holding from θ = 0.15 to 0.7 across all windows — except three rows. Two medics who healed across the line are **hatched**: unstable at every θ inside every window, so unclassified, and their friendly-fire rate reads 40 % with a spread bar covering half the range — the tool saying "don't call this friendly fire". Tamsin is different: solid B until 14:41, solid A after, stability .84 then .88, marked with a chevron and the printed change time. That is a real defection, not noise, and the old θ-only ribbon could not have distinguished them. Juno carries a `short` badge (arrived at 14:20) — low n, not low clarity. The officer locks the two medics to Ashguard-1 from the Team page; the ribbon locks those rows and every cell recomputes.

**FFA on a neutral sim.** Clustering reports K = 5 with low stability in every window, so θ\* is meaningless and the front field refuses to resolve — the Sparkpath shows "front undefined" hatch rather than a fake line. The respawn test also fails (φ = 0.2), so the spawn axis and spawn rings vanish rather than inventing an anchor. Segmentation finds no troughs and rows arrive as 3-min Slices, labelled as such. The unbent axis switches to **nearest-enemy distance**, and the default Query becomes Rows=Pair, Cell=Matrix: the who-shot-whom matrix over 5-minute columns shows transient alliances as blocks that appear and dissolve. Deaths remain fully attributable, equipment stats and wall records are unaffected (they never needed teams), and "no stable teams detected" sits in the Teams tab instead of a made-up two-colour map.

## 8. Risks and open questions

- **Raycast cost.** ≤78 casts per death sweep, ≤26 per behaviour-metric event, one shared 200/frame pool with priority, LRU memoisation on 1 m-quantised positions. A "compute all wall verdicts" run still needs its progress bar and cancel, and cache invalidation on track backfill is a correctness hazard worth testing.
- **Segmentation can still fail.** Trough detection has a threshold (20 % of local peak) that a grinding raid may never cross; the Slice fallback keeps the UI working but the officer loses the "engagement" as a meaningful noun. Threshold needs playtesting.
- **Drift window length is a value judgement.** A 6-min half-life smears a fast defection; a 1-min one manufactures chevrons from noise. Both are exposed, so both can be gamed by an officer looking for a result.
- **The front field is a model, not a fact.** Now doubly so: it depends on θ\*, which depends on clustering. The θ×σ sweep and the sign test are the guard; officers may still over-trust it when it *is* drawn.
- **Spawn semantics remain inferred.** The relocation test detects a discontinuity, not a game rule; a sim that teleports for other reasons, or one with heavy track gaps, fools it in both directions.
- **Equipment identity can be attacked.** Keying on creator + name + attach class + damage signature beats naming alone, but a spoofer with their own creator account defeats it; the UNATTRIBUTED row is the honest sink.
- **Overlay density.** LOD defaults cap what is drawn and the chip says what is hidden, but a wide alt-cam over a 37-agent raid is still the worst case for legibility; widen-one-step needs testing at distance.
- **Colour budget** past four teams (shape-coded markers help, at a legibility cost) and **text density** (≈12 chars per cell label at 100 % UI scale; numbers only at 150 %).
- **Open:** is Pair the right default noun for FFA — the matrix is dense at 37 agents and needs an exchange threshold nobody has calibrated. And should the τ sweep precompute live per death? Instant answers, at the cost of raycasts during combat, exactly when frames matter.
