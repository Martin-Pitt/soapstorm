# Entry 04 — The Cartographer (MAP-FIRST)

## 1. Thesis

Every officer question in the brief is secretly a spatial question: *where* did the fighting happen, *where* did the front sit, *where* did a shot come from, *where* is the choke point where friendly fire keeps happening. This entry makes the region map the hub of the tool and everything else — time, teams, equipment, individuals — a lens you apply to it. One floater, the **War Map**, owns the screen: a custom-drawn top-down canvas of the region with switchable layers (heat, flow, territory/fronts, sightlines, duels) and a time scrubber underneath it. The in-world 3D overlay is not a separate feature; it is the map's own rung 0, the same layers projected onto the ground at eye height once the officer walks over and alt-cams. Digging into a person, a death, a weapon, or a place opens a wiki-style Inspector page, but the map is always visible behind it and every page has a "show on map" anchor, because in this design nothing is understood until you know where it happened.

## 2. Ladder of abstraction

- **Rung 0 — Live ground, in-world.** The existing overlay: at the current cursor time, drawn on the terrain, only while stationary + alt-camming. Trails, hit lines, death markers, LOS rays, all in world space. *Step up*: the War Map has a "you are here" camera-frustum wedge; pointing at any marker in-world and clicking opens its Inspector page, which is also a map pin. *This is the grounding instance every abstract claim must cash out to.*
- **Rung 0.5 — Live ground, map.** Same instant, but top-down: a dot per avatar (arrow = facing), lines for damage in flight, ring for the last 2 s of deaths. This is the 2D twin of rung 0 — useful when the officer isn't in-world at all (reviewing later, or on a laptop away from the raid). *Step up*: drag the trail-length slider from 0. *Step down*: none needed, it's already the floor — but clicking a dot opens the avatar's Inspector page (a different kind of "down": down into biography, not into space).
- **Rung 1 — Trails (helicopter view).** All positions in the chosen window painted as fading ink on the map at once — literally Bret Victor's "every street of the city." Reveals routes, retreats, pincers. *Step up*: hold the trail slider at max (whole session) — trails become **Rung 2 heat**: a density field replaces individual lines, because 100k samples of ink converge into a texture anyway. *Step down*: click any point on a trail to jump the timeline cursor to that sample's timestamp and re-enter rung 0.5/0 there.
- **Rung 2 — Field abstractions (heat / flow / territory).** Three toggleable fields over the same grid: damage-density heat, movement-flow arrows, and the derived front line between team territories. Parameter to sweep: the time window (a draggable "as-of" range under the map) and, for teams, the **confidence threshold** slider — exactly the brief's example of Rung 2. *Step up*: switch to the Front Timeline (Rung 3), which stacks many of these windows side by side. *Step down*: click a hot cell → list of the events that built it → pick one → rung 0.
- **Rung 3 — Front Timeline (territory unbent through time).** A ribbon: x = session time, y = position along the dominant front's normal axis (a coordinate transform, "distance-to-front" made literal), colour = which side holds that stretch. This is the brief's higher rung: several dimensions (space, time, side, confidence) in one strip. *Step up*: small multiples — one ribbon per weapon or per team pairing, side by side, for cross-cutting comparison. *Step down*: click a point on the ribbon to set both the map's time window and its spatial crop to that segment.
- **Rung 4 — Small multiples & duel graph.** Per-weapon mini war-maps (kill locations only) tiled in a grid; a FFA-mode duel graph (nodes = combatants at their session-average position, edges = damage exchanged) laid directly over the terrain outline. *Step down* from any tile or edge: click to filter the main map to that weapon/pair and drop to Rung 1.

The transitions are the point: heat tells you *where*; clicking tells you *why*; the front ribbon tells you *when it changed*; going back to rung 0 at that moment tells you *what it looked like*. No page is a dead end — every page has a step up (aggregate me) and a step down (show me an instance).

## 3. Information architecture

**Hub:** War Map (always open when the tool is enabled; lives in the main floater, `Combat > Combat Log`).

**Wiki pages** (rendered in the Combat Inspector floater, one page at a time, with breadcrumb history — Back/Forward buttons plus a dropdown history list, like a browser):

- **Place** (a map cell or a named Staging/Spawn Area) — heat/flow/team-occupancy time series for that cell, list of events located there, "who died here", backlinks to any Front Timeline segment that touched it.
- **Avatar** — dossier: movement summary, mouselook time, kills/deaths, weapons used, team + confidence, a mini trail-map thumbnail. Links to every Death, Equipment, and Team page it touches.
- **Death** — attribution (who/what/from where), LOS verdict, damage build-up list, "show on map" and "show in world" anchors.
- **Equipment** — classification + evidence, users, damage/kills, adjustment ratio, through-wall suspicion tally, its own kill-location mini-map.
- **Team** — roster with per-avatar confidence, territory-% over time (a sparkline pulled from the Front Timeline), friendly-fire list, colour swatch, manual override control.
- **Front segment** — a stretch of the Rung 3 ribbon: which teams, when, how confidently, links to the deaths that happened astride it.
- **Duel** (FFA mode) — a pair of combatants: damage exchanged both ways, first contact time/place, whether it escalated to a kill.

**Navigation model:** breadcrumb trail (`Region Map ▸ Team: Ashguard ▸ Avatar: Kessa ▸ Death @14:32`) at the top of the Inspector; a **Pin** button that docks a page into a fixed side-strip (up to 3 pinned) so comparisons survive further digging; every page has a **Referenced by** footer (backlinks) — e.g. an Equipment page lists every Death and every Front-segment where it was the decisive weapon. The map itself keeps a light-up "breadcrumb trail" of pins as small numbered flags so you can always see, spatially, where your last few dig-ins happened.

## 4. Screens

### 4.1 War Map (main floater, default tab)

```
+-- Combat Log ------------------------------------------------[_][□][x]--+
| [Now|Trails|Heat|Flow|Front|Sightline|Duels]  window: [--Δt=90s--]▭     |
|  Layers: [x]Trails [x]Deaths [ ]FF-only [x]Territory [ ]LOS  conf:[==o-]|
+---------------------------------------------------------------------------+
|                                                                           |
|     .·:·.        N                                                      |
|   ░░▒▒▓▓██  ▲       ~~~~~~front~~~~~~                    ⚑ pin1        |
|   ░░▒▒▓▓██ ↖  ↖    (dashed = low-confidence stretch)                    |
|   staging-A  ↖ ↖  ·A ·A      ·B ·B  →→                                  |
|                  ·A   ×died14:31  ·B→  ↘                                 |
|                       ↘   ↘      ·B  staging-B                          |
|                          ~~~~~~~~~~                                      |
|   [you-are-here wedge, if in-world]                                     |
+---------------------------------------------------------------------------+
| status: 2h03m session · 1,842 events · 61 tracked · A holds 58% (14:40) |
+---------------------------------------------------------------------------+
| [<<][<][▶/❚❚][>][>>] speed:[1x] ────●───────────────── time 14:40:12    |
+---------------------------------------------------------------------------+
```
- Mode row: switches which field layer paints the canvas (heat/flow/territory/sightline/duels); Trails/Now always available as an underlay.
- Layer checkboxes + confidence slider (the Rung-2 sweep control).
- Canvas: an `LLView` subclass drawing the region top-down from stored terrain heightfield outline, grid cells shaded by the active field, avatar glyphs (▲ facing arrow), death crosses, staging-area blobs (auto-labelled A/B once teams settle), pinned flags. Click a glyph/cross/cell → opens Inspector on that noun. Right-drag pans, wheel zooms, click-drag on empty ground with no modifier box-selects a region (feeds "Place" aggregate for that box).
- Status line: session totals plus the current territory read-out — the one-line answer to "how's it going" the officer glances at without asking anything.
- Transport bar: identical contract to the plan's timeline strip (Live/Play/Pause/speed/step/scrub); the scrub thumb also carries small tick marks for deaths (red) and territory flips (grey), so scanning the bar alone previews the shape of the raid.

### 4.2 Front Timeline (tab next to the map)

```
+-- Front Timeline ---------------------------------------------------------+
| axis: [front-normal ▾]   facet: [none ▾ | per-weapon | per-team-pair]    |
|  pos                                                                     |
|  +80m |AAAAAAAAAAAAAAAAAAAAAAA░░░BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|   +40 |AAAAAAAAAAAAAAAAAA░░░░░░░BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|    0  |AAAAAAAAAAAAA░░░░░░░░░░░BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|   -40 |AAAAAAAAAA░░░░░░░░░░░░░░BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|   -80 |AAAAAAAA░░░░░░░░░░░░░░░░BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB        |
|        14:00      14:20      14:40 ▲cursor    15:00       15:20         |
|  ░ = contested / low-confidence band     ✕ = death astride the line     |
+---------------------------------------------------------------------------+
```
- x = time, y = distance along the front's normal (auto-computed axis; officer can override which axis with the picker if the fight is L-shaped).
- Colour bands = which team holds that stretch of ground; the grey `░` band width directly visualises confidence (wide grey = a genuinely contested no-man's-land, not a UI glitch).
- Clicking any point sets the War Map's time window centred there and crops it to that stretch of the front — the Rung 3 → Rung 2/1 step-down named in the ladder.
- `facet` dropdown re-renders the ribbon as small multiples (one thin ribbon per weapon, stacked) for cross-cutting comparison — Rung 4 reached from here too.

### 4.3 Combat Inspector (wiki page, shown here as a Death page)

```
+-- Inspector --------------------------------------------------[_][□][x]--+
| ‹Back  Region Map ▸ Team:B ▸ Avatar:Renn ▸ Death @14:32:07     Pin ⚑    |
+---------------------------------------------------------------------------+
| DEATH — Renn (Team B, conf. 0.81)              [Show on map][Show in-world]|
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
| Related: [Kessa] [Ashguard SMG mk3] [Team A] [Team B] [Front seg 14:20-14:40]|
+---------------------------------------------------------------------------+
| Referenced by: Front segment (choke "the ramp"), Equipment: SMG mk3 kill map|
+---------------------------------------------------------------------------+
```
- Same skeleton (header/breadcrumb, body, related-links row, backlinks footer) serves Avatar, Team, Place, Equipment and Duel pages — only the body panel swaps, which keeps this a single custom `LLPanel` with a body sub-panel factory keyed by page type, well within XUI's tab/scroll-list toolkit.
- "Show on map" recentres the War Map and drops a flag; "Show in-world" moves the timeline cursor and, if the officer is already stationary+alt-cam, highlights the marker on the ground.

### 4.4 Teams & Territory tab

```
+-- Teams ------------------------------------------------------------------+
| conf. threshold: [======o===]  0.55        [Merge][Split][Reset]        |
| Team A (Ashguard-atk)  n=12  territory 42%  FF: 2 incidents  [color■]   |
| Team B (Ashguard-def)  n=9   territory 58%  FF: 0 incidents  [color■]   |
|   uncertain (below threshold): Voss (0.51 → A), Ilyra (0.49 → B)        |
+---------------------------------------------------------------------------+
|  [mini war-map: territory fill, greyed = uncertain avatars, per row]     |
+---------------------------------------------------------------------------+
```
- The confidence slider re-runs the clustering cut live, and greys out avatars that flip sides below/above threshold on the mini-map — the concrete embodiment of Rung 2's "team assignment across confidence thresholds."
- `Merge`/`Split`/manual per-avatar override buttons let the officer correct clustering (e.g., mark that the two Ashguard tags are actually one intra-group skirmish split), persisted for the session.

## 5. In-world overlay

The overlay draws the *same four fields* as the map, projected onto the ground, gated exactly as already decided (stationary + alt-cam, 5 s grace after leaving mouselook/OTS):
- **Rung 0/0.5 parity:** avatar rings + facing arrows + name labels, hit lines, death crosses — identical semantics to the map dots, same team colours, so the officer never has to relearn an encoding switching between map and world.
- **Rung 1 (trails):** polylines on the ground, alpha-ramped old→new, exactly mirroring the map's ink.
- **Rung 2 (heat/territory):** heat becomes translucent ground quads (colour ramp, low alpha so terrain stays legible); the front line becomes a coloured ribbon-on-terrain (a thick dashed line following the contour, dash gaps sized by local confidence — the same "grey band width" idea, expressed as gap width instead of colour since it must read against real ground clutter).
- **Sightline layer:** clicking a death or a map point while in-world casts the candidate-shooter fan live: green ray = CLEAR, red = BLOCKED, dotted grey = UNKNOWN (no track sample), each ray labelled with its candidate timestamp offset; a translucent red disc marks the blocking surface hit point.
- **Uncertainty encoding, consistent across both spaces:** low team confidence = hatched/dashed rather than solid; missing/COARSE track quality = thinner, dashed line, no head arrow; LOS false-positive caveat always rendered as a persistent on-screen note ("candidates: 1 clear / 2 blocked — travel-time ambiguity") rather than a bare verdict, in-world and on the Death page alike.
- **Clickable:** every marker pushes a screen-rect during `render()` exactly per the plan's picking hook; clicking opens the Inspector page for that noun, same as clicking its map or list twin — one click target model, three entry surfaces (map, list, world).

## 6. Analysis model

All fields share one **adaptive grid** (quadtree, leaf ≈4 m, coarsened for zoomed-out map draws), recomputed incrementally as events/samples append; heavy fields (LOS fan) are computed on demand and cached by (vantage, cursor-time) key.

- **Heat** `H(cell,t,Δt) = Σ_e damage(e)·gaussian(‖pos(e) − cell‖, σ)` for events in `[t−Δt, t]`. DAMAGE has no position; `pos(e)` is the *target's* track sample at `e.t` (approximate — flagged on hover as "location = victim's position, not verified shot origin"). DEATH events carry true `target_pos`/`source_pos` and get full weight; DAMAGE-only heat draws desaturated to mark the approximation.
- **Flow** `F(cell,t,Δt)` = recency-weighted mean velocity of track samples in the cell/window, drawn as an arrow sized by mean speed — reveals pushes vs retreats without individual trails.
- **Team occupancy** `O(team,cell,t,Δt) = Σ presence_time(avatar,cell) · teamConfidence(avatar,team)`. The **front** is the zero-crossing contour of `O(A) − O(B)` on the grid (marching-squares over cell corners); contour confidence = normalised `|∇(O_A−O_B)|` — steep gradient draws solid, shallow/sparse draws dashed. **Territory-%** = grid-cell-time where a team's occupancy exceeds the other's, over contested cell-time in the window; feeds the status line and Team-page sparkline.
- **Team clustering / confidence**: the shared plan's label-propagation over the who-shoots-whom graph, seeded by active-group where available, plus a per-avatar **margin** (assigned-cluster edge weight minus best alternative, normalised) as the confidence value driving the threshold slider and the map's hatch/grey rendering. Manual overrides pin a team regardless of margin, recorded as "officer-asserted" and never silently overwritten by re-clustering.
- **Staging/spawn areas**: DBSCAN-style clustering (ε≈6 m, min-pts≈3) over two point sets — each avatar's first track sample of the session, and each avatar's first sample after a gap >8 s following its own DEATH (a respawn). A cluster with ≥3 members becomes a named Place page ("Staging-A"); "gear-up duration" = median low-velocity dwell time before that avatar's first outbound damage.
- **Sightline fan**: for a vantage and radius R, sample ring points at coarse resolution; each ray reuses the shared LOS module (candidate shooter positions across the travel-time window) and is cached; aggregated to cells for the map layer, kept per-ray in-world. Through-wall suspicion per weapon = count of BLOCKED-only kills, shown as a rate with sample size, never a bare percentage.
- **Duel graph (FFA)**: symmetric matrix `D(i,j)` = damage exchanged either direction; an edge draws when `max(D(i,j),D(j,i))` exceeds a noise floor; nodes sit at each avatar's session-median location so the graph stays a map, not a force-layout — FFA still reads through the "space is the axis" thesis instead of a generic network view.

## 7. Walkthroughs

**Q1 — how did the raid go?** Open the War Map; the status line already reads "A holds 58% (14:40)." Set mode to Territory, Δt = whole session, and scrub the transport bar slowly: the front line sweeps in from the two staging blobs, stalls near the choke labelled "the ramp," then jumps as one side collapses — visible as a fast colour-swap on the canvas and a step in the Front Timeline ribbon. Switch to Trails at max window for the "helicopter" overview of where anyone ever went. Click the biggest heat cell near the jump; its Place page lists the death cluster that caused the collapse — one click down to Rung 0 there confirms it visually.

**Q3 — why did X die at 14:32?** Click X's death cross on the map (or scrub to 14:32 and click in-world). The Inspector's Death page shows the build-up list (§4.3): three hits, cumulative into the kill, each with its own LOS verdict; the last hit shows BLOCKED with one CLEAR candidate in the travel window, rendered with the caveat text, not a bare verdict. "Show in-world" drops the officer at Rung 0 with the sightline fan already drawn from the killer's candidate positions.

**Q8 — suspicious behaviour?** Open Equipment for the weapon in question; its through-wall suspicion rate (e.g. "4 of 11 kills, BLOCKED-only, small sample") is shown with sample size. Its kill-location mini-map (Rung 4) clusters those kills near one specific wall corner — click the cluster to drop into the Place page, then into individual Death pages, each carrying its own honest LOS caveat. Nothing here asserts cheating; the tool surfaces a concentrated, unusual pattern and lets the officer judge it against the evidence.

**Intra-group skirmish.** Active-group tag is useless (both sides are "Ashguard"), so the Teams tab seeds purely from who-shoots-whom; the War Map shows two staging blobs both labelled Ashguard-colour but hatched with an A/B pattern, and the confidence slider is dragged up until the two clusters stop bleeding into each other. Friendly-fire toggle stays off by default here since same-tag damage is expected to be split, not flagged, once the split is confident; incidents where an avatar hits its *own* resolved sub-team still show on the Team page as true friendly fire.

**FFA.** Territory/front layers are switched off (no stable two-sided front exists); the officer uses Duels mode instead — the graph immediately shows which two or three avatars are actually trading damage in a corner while everyone else is scattered, each edge clickable down to individual damage events, keeping the map (not a force-directed abstraction) as the spatial anchor even when "teams" don't really exist.

## 8. Risks and open questions

- **Front/territory math is the riskiest derived quantity here** — it can look authoritative while resting on sparse, noisy occupancy data early in a fight or in a scattered FFA; the confidence-band rendering must never be skippable/hideable by default, or officers will over-trust a clean-looking line.
- **DAMAGE-location approximation** (victim's track position, not shooter's) is a real gap for heat accuracy near cover; needs the desaturation/hover caveat to be genuinely noticeable, not a tooltip nobody reads.
- **Grid/quadtree recompute cost** at 100k events / millions of samples needs profiling; the plan's incremental philosophy should extend to the grid (dirty-cell tracking on event append) rather than recomputing fields per frame.
- **Small-multiples panels (Rung 4)** are the most XUI-novel piece (tiled custom-drawn mini-maps); worth prototyping early since layout/scroll behaviour in immediate-mode XUI for a variable-count tile grid is unproven in this codebase.
- **Staging-area auto-detection** may mislabel a mid-fight rally point as a "spawn" in freeform sessions with no real staging phase; needs a manual rename/merge affordance on the Place page.
