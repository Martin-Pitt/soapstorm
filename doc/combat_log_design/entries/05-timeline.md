# Entry 05 — Time-First: the Combat Timeline

## 1. Thesis

A raid is a story that unfolds on one axis: time. Every other fact an officer wants — teams, deaths, weapons, suspicion — is really a question of *when* something changed. This design puts a zoomable, swimlaned timeline at the center of the tool instead of treating it as a transport bar bolted onto a 3D view. The timeline is not a scrubber for the overlay; it **is** the primary instrument, and the 3D overlay is one of its lenses. Rows are avatars (grouped into team bands), columns are time, density is combat intensity, and the tool automatically segments the session into named phases (staging, push, front, collapse, respawn wave) the way a heart monitor segments a session into beats and arrhythmias. The officer's default motion is horizontal: skim the whole raid as one picture, spot the shape of it, then zoom into a second and let the world materialize under the cursor. Space is never absent — it is always one drill-down away — but time is the spine everything else hangs off, because time is the one axis that is honest, always available, and shared by every avatar, team, and weapon in the log.

## 2. Ladder

| Rung | View | Step up (concrete → abstract) | Step down (abstract → concrete) |
|---|---|---|---|
| 0 — concrete | 3D overlay at the cursor's instant | Press "Trail" or widen the timeline zoom | Drag the timeline zoom to its narrowest stop (±0.5 s); overlay shows one instant |
| 1 — abstract over time | Whole-session swimlane view: every avatar's row from t=0 to t=end, trails not blips | Click any row/segment to jump the cursor there and re-narrow the zoom | Click a tick or drag-select an interval; overlay renders that instant/window |
| 2 — abstract over parameter | Phase-confidence slider (loosen/tighten thresholds), LOS-candidate strip (verdict across the travel-time window), team-confidence slider (redraw swimlane grouping at looser/tighter clustering) | Raise a slider to see how fragile a verdict is | Click the single candidate/instant that matters; jumps to rung 0 for that candidate |
| 3 — multi-dimensional / small multiples | Phase cards (one tile per detected phase: duration, casualties, top weapon, dominant front distance); per-team density sparklines stacked in the swimlane header | Click a phase card to select its time range everywhere | Phase card has a "Play this phase" button — narrows zoom to the phase and starts playback at rung 1→0 |
| 4 — coordinate transform | "Distance-to-front" strip: y-axis reinterpreted as distance between opposing clusters instead of world position; unbends the raid into a single line that rises (push), plateaus (front), and drops (collapse) | Toggle from raw density to front-distance in the same panel | Click a rise/plateau/drop on the unbent line; jumps to the timeline at that time, then to the overlay |

The rule enforced throughout: nothing is a dead end. Every tick, row, card, and slider handle is clickable and either narrows the time window (down the ladder) or opens a coarser summary (up the ladder). The cursor is the single shared coordinate between all rungs — moving it anywhere moves it everywhere.

## 3. Information architecture

**Nouns** (each a page in the Inspector, each with a breadcrumb trail and a "Referenced by" backlinks block):

- **Moment** — a point or narrow range in time. Default landing page when you click empty timeline. Shows: active phase, avatars present, events in range, camera jump button.
- **Event** (DAMAGE / DEATH) — attacker, target, weapon, LOS verdict, adjustment chain, related events (the death this damage led to, or the damage that led to this death).
- **Avatar** — session dossier: kills/deaths, weapons used, mouselook %, time seated, team history (with reassignment timestamps), a mini swimlane row rendered full-width.
- **Team** — roster, confidence, friendly-fire count, shared front-distance history, weapon mix.
- **Equipment** — classification + evidence, users, damage/kills, adjustment ratio, through-wall suspicion score with sample size.
- **Phase** — auto-detected segment: kind, time range, confidence, participants, casualties, dominant weapon, "why this boundary" explanation (which signal crossed which threshold).
- **Place** — a spawn point or front location inferred from clustering; only reachable by drilling from a Phase or the front-distance transform, since this entry keeps space secondary.
- **Verdict** — a single LOS or suspicion judgment; shows every candidate considered and its individual outcome.

**Navigation model**: the Inspector floater keeps a **history stack** (back/forward arrows, like a browser) plus a **breadcrumb bar** built from the click path (Session ▸ Phase: Front ▸ Death: Nexii @14:32:07 ▸ Equipment: Ashguard-9mm). Any page can be **pinned** (a small pin icon) so it survives navigation in a docked side-strip — the officer building a promotion case pins the candidate's Avatar page and keeps digging elsewhere without losing it. **Backlinks** appear at the bottom of every page ("Referenced by: 3 deaths, 1 friendly-fire flag, Phase: Collapse") so the officer can walk the graph in either direction — this is the wikipedia-style requirement made literal.

The **Combat Timeline** floater is not a page in this graph; it is the map the graph is drawn on. Clicking anything on it opens/updates the Inspector; clicking anything in the Inspector moves the timeline cursor and, if the item has a location, nudges the overlay camera.

## 4. Screens

### 4.1 Main floater — `Combat Timeline` (`ss_combat_log`)

```
+---------------------------------------------------------------------------------------+
| Combat Timeline                                                            [_][口][x]  |
+---------------------------------------------------------------------------------------+
| ⦿ Live  ▶ Play  ⏸  Speed[0.25x 1x 2x 8x]  ⏮1s ⏭1s   Zoom ──●───────── 2h..0.5s        |
+---------------------------------------------------------------------------------------+
| PHASES  [Staging ▓▓▓|Push ▓▓▓▓▓|Front ███████████████|Collapse ▓▓▓|Respawn ░░░|Front..]|
|         (hover a band -> tooltip: kind, confidence 0.82, duration 4m12s)               |
+-----------------------------------------------------------------+---------------------+
| OVERVIEW  (whole session, fixed 1 col/window, never rescrolls)  |  [Phases][Teams]     |
| ▁▂▃▅█▇▅▃▂▁▁▂▄▇█▇▅▃▁▁▂▃▅▇█▇▄▂▁▁▁▂▃▄▅▆▇█▇▆▅▄▃▂▁▁                  |  [Equip.][Avatars]   |
|                                   ▽ viewport(zoomed window)      |                       |
+-----------------------------------------------------------------+  ------------------   |
| SWIMLANES  (scrolls vertically; header rows collapse when zoomed out past N rows)       |
|  Team: Ashguard-A  conf 0.9   ▁▂▃▅▇█▇▅▃▂▁ (team density)         |  Phase: Front         |
|   Nexii   ▤▤▤▤▤▤────●───┼────╳────────────●──●───┼──────▤▤▤▤▤▤   |  09:41–13:53          |
|   Orion   ────────────────┼───●──●──────────────┼───────────    |  conf 0.71            |
|   ...                                                            |  casualties: 9        |
|  Team: Ashguard-B  conf 0.62 (mixed w/ intra-group)  ▁▂▄▇▇▄▂▁    |  top weapon: SMG-9mm  |
|   Bret    ────┼──╳────●──────────────────────────┼──────        |  [Play][Pin][Open]    |
|  Unaffiliated / low-confidence  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ |                       |
|                                          ▲ cursor 14:32:07       |                       |
+-------------------------------------------------------------------------------------+---+
| Selected: DEATH — Nexii @14:32:07 · killer Orion · weapon "Ashguard-9mm" · LOS CLEAR    |
+---------------------------------------------------------------------------------------+
```

Regions:
- **Transport bar**: Live toggle, play/pause, speed, single-step, and the **Zoom** slider — the one control that moves the whole floater along the ladder rungs 0↔1. Zoom range is logarithmic: whole-session at one end, 0.5 s at the other.
- **Phase ribbon**: a single custom-drawn strip, one colored band per detected phase, width proportional to duration in the *current* zoom, label + hatching density encoding confidence (solid = confident, hatched/lighter = weak). Clicking a band sets the zoom window to that phase (rung 3→1 step down) and opens its Phase page.
- **Overview strip**: always shows the *entire* session as a density sparkline (rung 1 permanently visible, no matter how zoomed the swimlanes below are), with a draggable viewport rectangle — this is the "never get stuck in the zoomed weeds" safety rail Bret Victor calls for.
- **Swimlanes**: the workhorse. One row per avatar, grouped under team header rows. Team header row is itself a mini density sparkline (small multiple). Row background hatching = state (▤ mouselook, ░ seated/vehicle, blank = normal, dashed = track gap/unknown). Row ticks: `●` damage tick (dealt=filled, taken=hollow), `╳` death, `★` kill credit, small shield glyph = damage adjustment applied. Tick color = damage type; tick outline color = LOS verdict (green/red/grey ring) so through-wall suspicion is visible without opening anything. When zoomed past a density threshold, individual avatar rows collapse into the team's aggregate row (small-multiples rung, automatic, reversible by widening a row).
- **Side panel** (tab container, standard XUI `tab_container`): Phases (list + selected phase card, shown above), Teams (roster + confidence + reassignment log), Equipment (sortable list), Avatars (sortable list) — each row click sets the Inspector page.
- **Status line**: mirrors the current Inspector selection headline so the officer never loses context while looking at the timeline.

XUI feasibility: swimlanes, phase ribbon, and overview strip are one custom `LLView` subclass (`SSCombatTimelineView`) drawing rects/lines/text per frame from cached per-row draw lists, rebuilt only when the zoom window or filter changes (not every frame) — same immediate-mode-with-a-cache pattern as the existing storm debug overlays. The side panel is a stock `tab_container` + `scroll_list`s, no custom drawing needed.

### 4.2 Inspector floater — `Combat Inspector` (`ss_combat_inspector`)

```
+--------------------------------------------------------------------+
| Combat Inspector                                        [_][口][x] |
+--------------------------------------------------------------------+
| Session ▸ Phase: Front ▸ Death: Nexii @14:32:07 ▸ Equipment: 9mm   |  <- breadcrumb
+--------------------------------------------------------------------+
| DEATH — Nexii   14:32:07.4   [Show in world] [Track victim] [Pin]  |
|  killer: Orion (Ashguard-A)      weapon: "Ashguard-9mm" (SMG)      |
|  killing blow: 34 dmg, type ANTI_ARMOR   adjusted from 51 (-33%)   |
+--------------------------------------------------------------------+
| RELATED DAMAGE (window: 8s before death)                           |
|  14:32:01.1  Orion → Nexii   12 dmg  [LOS: CLEAR]                  |
|  14:32:04.6  Orion → Nexii   19 dmg  [LOS: UNKNOWN - track gap]    |
|  14:32:07.4  Orion → Nexii   34 dmg  [LOS: CLEAR]  <- killing blow |
+--------------------------------------------------------------------+
| LOS CANDIDATES (killing blow, travel window 1.5s, step 0.25s)      |
|  t-1.50 (12,44,21) BLOCKED   t-1.25 (13,44,21) BLOCKED             |
|  t-1.00 (14,45,21) CLEAR     t-0.75 (15,45,20) CLEAR   <- used     |
|  t-0.50 (16,46,20) CLEAR     t-0.25 (17,46,20) CLEAR               |
|  Verdict: CLEAR (4/6 candidates clear)   caveat: lag/travel-time   |
+--------------------------------------------------------------------+
| ADJUSTMENTS   armor_v3.lsl: -33%  (task 8f2c…)                     |
+--------------------------------------------------------------------+
| Referenced by: Phase "Front", Team Ashguard-A friendly-fire: none  |
+--------------------------------------------------------------------+
```

Every noun type renders through the same shell (breadcrumb, header actions, body sections, backlinks); only the body sections differ per type (Avatar page swaps "Related damage" for a full session timeline row plus stat block; Equipment page swaps LOS candidates for a suspicion histogram across all its kills). `[Show in world]` snaps the timeline cursor to the event and, if gating conditions are met, the officer sees the overlay light up at that instant.

## 5. In-world overlay

The overlay is rung 0 and always reads *from the timeline cursor*, live or scrubbed — "the world follows the cursor" is the literal contract, not a metaphor.

- **Rung 0 (cursor instant, zoom < ~5 s)**: exact positions at t. Avatar = ring at feet + yaw arrow + name label; mouselook = small eye glyph above the head; seated = rides the vehicle marker. Damage in flight = a line from attacker to target colored by damage type, brightening then fading over 1 s; a hit dot on the target. Deaths = persistent cross+ring, killer line drawn from `source_pos`, label "victim ← killer (weapon)".
- **Rung 1 (zoom widened, trail mode)**: same markers but every avatar also carries a **trail** — a polyline of its positions across the current zoom window, alpha-ramped old→new, color = team, width bucketed by track quality (coarse = thin/dashed). This is the direct visual analogue of the swimlane row: the swimlane is the trail unrolled onto a time axis, the overlay trail is the swimlane folded back into space. Widening the timeline zoom widens the trail window identically — one slider drives both.
- **Rung 2 (parameter sweep)**: selecting an event's LOS candidates highlights each candidate shooter position as a small numbered marker (green ring = clear, red ring = blocked, grey = untested/gap), all shown simultaneously so the officer sees the whole travel-time window at once rather than stepping through it.
- **Uncertainty encoding**: track gaps draw dashed/lower-alpha trail segments and a "?" glyph at the gap; unknown-team avatars draw in neutral grey with a dotted ring instead of a solid team color; LOS UNKNOWN draws a grey dashed line instead of solid green/red; equipment of unknown classification draws with a question-mark icon instead of a weapon glyph. Nothing that is uncertain is drawn with the same visual confidence as something verified — this is enforced by using outline style (solid vs. dashed vs. dotted) as a second, uncertainty-only channel, never overloaded onto color (color is reserved for team/damage-type).
- **Clicking**: any marker or trail segment is a screen-rect hit target; clicking it moves the timeline cursor to that instant and opens the matching Inspector page — the overlay is a second input surface for the same graph, not a separate view.
- **Gating**: unchanged from the engineering plan (stationary + alt-cam, 5 s grace after leaving mouselook/OTS). While gated off, the timeline and Inspector remain fully usable — this design does not require the overlay to answer any officer question on its own, only to make the answer visceral once the officer stops to look.

## 6. Analysis model

- **Combat intensity `I(t)`**: kernel-smoothed event rate, `I(t) = Σ w(kind) · K(t - t_i, σ)`, with `w(DEATH)=5, w(DAMAGE)=1`, Gaussian `σ` scaled to zoom (3 s narrow, up to 60 s whole-session). Drives the overview sparkline, team header sparklines, and phase segmentation. `σ` is disclosed in a tooltip since it shapes what "a spike" means.
- **Team centroid `C_team(t)`** / **dispersion `D_team(t)`**: mean position and mean pairwise distance of team members with a track sample within ±2 s of t (interpolated per the tracks store). Avatars below 0.5 team-assignment confidence are excluded from the centroid and folded into a wider "uncertain member" dispersion band instead.
- **Front distance `F(t)`**: for two dominant clusters, `|C_A(t) - C_B(t)|`; for FFA/>2 clusters, the minimum inter-cluster centroid distance, or, absent stable clusters, the median nearest-opposing-avatar distance across all present avatars. This is the quantity behind the distance-to-front transform (rung 4).
- **Phase segmentation**: a hysteresis state machine over `(I(t), trend of D_team(t), trend of F(t), distance of activity centroid from the inferred spawn point)`. STAGING = I(t) low, dispersion low, centroid near the session's first two minutes, before any sustained rise. PUSH = I(t) rising, centroid moving away from spawn. FRONT = I(t) sustained above a mid threshold and F(t) stable within an engagement band (default 10–40 m) for ≥20 s. COLLAPSE = death-rate spike co-occurring with a dispersion jump or a team's retreat velocity toward spawn. RESPAWN WAVE = a cluster of track "reappearances" (a segment starting with no preceding sample within 5 s, near a spawn candidate) from recently-dead avatars. Boundaries fade over ±half the kernel width rather than cutting sharply. Each phase carries a **confidence score** (normalized margin between the winning rule's signal and its threshold); low-confidence phases render hatched, and the Phase page states which signal was closest to ambiguous.
- **Team assignment and its confidence**: as in the engineering plan (shared-group seed + label propagation minimizing intra-cluster damage), plus a **reassignment log** per avatar rendered as a thin marker at the transition point in the swimlane header, since teams drift mid-session; swimlane grouping uses the assignment live during most of the visible window and flags windows that straddle a reassignment.
- **Friendly fire**: DAMAGE/DEATH where `owner`'s team-at-`t` equals `target`'s team-at-`t` (reassignment-aware, not a single session-wide label) — this is what separates true friendly fire from an intra-group skirmish between different sides.
- **LOS verdict**: as in the engineering plan — CLEAR if any travel-window candidate is clear, BLOCKED if all are, UNKNOWN if no track sample exists. Equipment-level **through-wall suspicion** = BLOCKED / all verdicts for that weapon, always shown with its denominator so 1-of-1 reads differently from 8-of-40.
- **Damage adjustment ratio**: `damage / initial` per event; aggregated per avatar/team/equipment as a distribution (median + range), not one number, since armor scripts can behave differently per damage type.

## 7. Walkthroughs

**Q1 — How did the raid go overall?** Open the Combat Timeline. The Overview sparkline and Phase ribbon are visible with zero clicks: five bands — Staging, Push, Front, Collapse, Respawn, then another Front band (the raid re-engaged). Hovering each band gives duration, casualties, confidence. Clicking the largest Front band narrows the zoom to it; the swimlanes show two team blocks converging, and the Front-distance transform (Phases tab) shows the classic rise-plateau-drop shape. The officer has "where, when, between whom" inside three clicks, from rungs 1 and 3, no individual events needed.

**Q3 — Why did X die at 14:32?** Scrub to 14:32 and click the `╳` on Nexii's row. The Inspector opens on the Death page (§4.2): killer, weapon, killing-blow damage and its adjustment, the related damage in the preceding 8 s, and the LOS verdict with all six travel-window candidates shown together (CLEAR, 4/6). `[Show in world]` moves the cursor there; stationary and alt-cammed, the overlay lights the exact geometry — killer line, candidate markers, the wall or its absence.

**Q8 — Suspicious behavior?** From an Equipment page ("Ashguard-9mm") the officer sees a through-wall suspicion score, say 3/40 BLOCKED, linked to each. Opening one shows the same candidate strip as above; if all three share an oddly short reaction time (derived: `time-to-first-hit after target track re-enters view range`), the Equipment page surfaces that as a second, independent signal alongside LOS, both captioned with caveats (lag, travel time, sim batching) and presented as evidence, not a verdict.

**Intra-group skirmish**: two Ashguard teams. Active-group coloring is useless, so the Teams tab shows two clusters labeled "Ashguard (cluster 1)" / "(cluster 2)" from who-shot-whom, not the tag. Swimlanes group by these clusters; friendly-fire stays silent for cross-cluster damage (different sides) and flags only same-cluster damage. The officer can rename the clusters ("Defenders"/"Attackers") from the Teams page; the rename propagates everywhere via the shared model.

**FFA scenario**: a neutral-sim deathmatch. Team clustering may never stabilize past singleton "teams" of confidence ~0. Swimlanes fall back to an "Unaffiliated / low-confidence" block (greyed, un-grouped, sorted by activity) instead of forcing bands that don't exist. Phase detection still works — intensity and dispersion don't need team identity — so the officer still gets a shape-of-the-fight read with zero named teams; front-distance degrades to "average distance between anyone actively fighting anyone."

## 8. Risks and open questions

- **Phase thresholds are heuristic and raid-shape-dependent.** A slow siege with no clear push, or a raid with three simultaneous fronts, may not fit five clean bands. Mitigation: expose the threshold/kernel-width sliders (rung 2) so an officer can retune per-session rather than trusting a single global default; always show confidence so a wrong segmentation reads as "weak," not "wrong."
- **Swimlane scaling with headcount.** 40+ avatars at whole-session zoom risks a wall of thin rows. The team-aggregate collapse helps, but the threshold for auto-collapsing needs tuning against a real 1–2 h synthetic session, not guessed.
- **Front-distance transform assumes ≤2 dominant clusters** cleanly; with 3+ simultaneous fronts (plausible in a large sim) the single scalar `F(t)` will blur distinct fights together. A per-cluster-pair small-multiples version is the likely fix but adds screen real estate this design otherwise tries to avoid spending.
- **Reassignment-aware friendly-fire lookup** requires the team model to expose "assignment as of time t," not just "current assignment" — a small but real addition to the engineering plan's team-clustering data shape that should be confirmed before implementation.
- **Respawn-wave detection via track discontinuity** may false-positive on ordinary lag/track gaps (coarse-location dropouts) that look like a "reappearance." Needs a minimum-gap-duration and near-spawn-radius tuned against real data, and should always be presented as inferred, with the raw track gap visible on click-through.
