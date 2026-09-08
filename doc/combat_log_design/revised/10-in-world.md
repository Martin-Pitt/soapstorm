# Entry 10 — In-World-First: the battlefield is the interface

## 1. Thesis

The officer already knows how to read a battlefield by standing in it and turning their head — that is the entire premise of alt-cam review. So the combat log should not be a spreadsheet with a 3D preview bolted on; it should be a **haunted replay of the terrain** where every fact the analysis modules produce is rendered as something you can walk up to, orbit, and click, and the two floaters exist only to hold transport controls and a breadcrumb trail. Ghost avatars replay the fight where it happened; trails, heat, and LOS fans are drawn *on* the world, not charted beside it; clicking a marker opens a billboard card at that exact 3D point, not a modal that steals your view. Digging means walking from one glowing thing to the next — a death card links to a killer's ghost, to a weapon halo, to a team-colour aura — and the floaters remember not just *where* you've been but *how you were looking at it*, so drilling in never strands you without a way back. The ladder of abstraction is a camera problem: rung 0 is standing at the spot at the moment; every rung above is a different way of *painting the same ground* — trails paint time onto space, heat paints density onto space, the front-ribbon(s) bend space itself. You never leave the world to go up the ladder; you only change what light falls on it.

## 2. Ladder

**Rung 0 — Ghost moment (concrete).** At the scrub cursor, every tracked avatar is a translucent ghost at its interpolated position/pose; damage lines flash for ~1 s around the cursor; a live LOS ray (if a death/damage sits at the cursor) draws as it resolved. A small hollow triangle over a ghost's head means "this avatar has at least one logged behaviour-evidence chip" — visible without opening anything.
*Step up:* press **Fan**, or a card's own footer chip, to reveal a trail — rung 0 → 1.

**Rung 1 — Trails (abstract over time).** Stroboscopic polylines per avatar over a chosen window (20 s default, up to whole-session), team-coloured, alpha old→new, tick-mark ghosts, death crosses, and the same evidence triangle carried onto the trail head so a scan of the whole session surfaces every avatar worth a look without picking a suspect first.
*Step down:* click any trail point → cursor jumps there, trail collapses to a rung-0 ghost, camera doesn't move.
*Step up:* select ≥2 avatars or "all" → **Heat** aggregates into rung 2.

**Rung 2 — Aggregate field (abstract over parameters).** Two instruments:
- **Battle heat**: a ground-projected field on a coarse grid (4 m cells, matching the tracks' own coarse-location quantisation), coloured by damage-event density. DAMAGE carries no positions, so each cell is stamped by looking up attacker/target `sampleAt(t)`; a cell built mostly from COARSE/stale samples, or from an event whose attacker had no track in range at all, renders blurred/desaturated, and that event is also added to a visible "unattributed" tally — heat never looks more certain than the trails it's built from, and nothing is silently dropped. Recomputed incrementally as the window slides, not per frame.
- **LOS candidate fan**: every candidate shooter position across the travel window drawn as green/red/grey (CLEAR/BLOCKED/UNKNOWN) — the whole window at once, never one collapsed verdict.
A **confidence slider**, defaulted to the clustering module's own "confident" cutoff (never to zero), re-tints ghosts live by threshold; each card still prints the raw confidence number regardless of slider position.
*Step down:* click a LOS ray → rung 0 at that candidate's instant. Click a hot patch → cursor jumps to its density peak.
*Step up:* toggle **Front(s)** for rung 3.

**Rung 3 — Unbent space + small multiples (top rung).** For every pair of clusters with a substantial mutual-damage edge (reusing the plan's own K-cluster output — no second clustering pass), a translucent ribbon is fitted along that pair's front; a two-team raid gets one ribbon, a genuine three-way fight gets a triangle of three, each striped by its flanking teams. Every avatar projects onto its nearest active front as a dot: height = signed distance-to-front, colour = team, size = recent damage dealt. Chop into 10-minute buckets for a small-multiples row.
*Step down:* click a dot → that avatar/time, rung 0.
*Step up:* none — the way back down is always a click, never a menu.

## 3. Information architecture

Everything is a **card**. Cards live in-world as billboards anchored to a 3D point and are mirrored as text rows in the Inspector floater, so every link-chip and action a card offers has a same-effect fallback that doesn't depend on aiming a camera. Chip hit-rects hold a fixed minimum screen-space size regardless of world distance, the same floor nameplate text already uses.

- **Moment** (implicit) — the scrub cursor, the "you are here."
- **Avatar dossier** — name, inferred team + confidence, K/D/damage, weapons, mouselook time; links → Team, each Death/Kill, each Equipment, Behaviour evidence. A freshly-arrived avatar with no damage-graph edges yet shows "no team data yet" in flat grey rather than a guessed colour; a logged-off avatar's card is tagged "offline since HH:MM" and its trail freezes then fades to nothing over a fixed window instead of continuing to draw.
- **Event card** (Damage/Death) — time, attacker→target, weapon, damage, LOS verdict, adjustment note; links → both Avatars, Equipment, related events (a Death inlines its damage chain as backlinks), Place.
- **Equipment card** — name/creator/kind + classification evidence checklist, users, damage/kills, adjustment-ratio distribution, through-wall tally; links → user Avatars, sample Events, Adjustments.
- **Team card** — roster with per-avatar confidence, friendly-fire tally, aggregate damage graph; links → member Avatars, friendly-fire Events, active Front segment(s).
- **Place** — auto-named from heat peaks, or hand-pinned; links → Events, Teams present, Front segment.
- **Adjustment** — one armour/damage-script instance: ratio over time, events touched; links → Equipment, wearer.

**Navigation model.** A breadcrumb strip runs along the top of the thin floater (`Overview ▸ Avatar:Kestrel ▸ Death@14:32 ▸ Weapon:AK-Prime`). The first chip, **Overview**, is pinned and restores the full-session Heat+Trails view exactly, one click, no matter how deep you've dug. Every other chip is a *view snapshot*, not just a noun: which layers were on, trail window, slider position, and front toggle at the moment you navigated away. Clicking a chip restores both the card and that state, so narrowing into a hot patch never strands the officer without a recorded way back; later history is kept, not popped. **Pinning** (a card action) keeps a billboard floating permanently, so the officer can pin two avatars' dossiers on opposite sides of the sim and alt-cam pan between them — gating only cares that the *avatar* is stationary, not where the camera focus point sits, so this never drops the overlay. "Pan" means a handful of alt-cam focus-recentre clicks, not one continuous drag; if a pinned card ever sits beyond comfortable camera reach, its Inspector text mirror is live regardless of distance, so comparison never actually depends on the camera getting there. **Backlinks** live in every card's footer, always visible.

## 4. Screens

### A. Floater: Combat Log (thin controller, ~360×230, resizable)

```
+-- COMBAT LOG -------------------------------------[_][X]--+
| [Overview] Avatar:Kestrel > Death@14:32                   |  <- breadcrumb, home-
+-------------------------------------------------------------+     pinned chip first
| [Live] [|<] [Play] [>|]  speed[ 1x v]  step[-1s][+1s]      |  <- transport
+-------------------------------------------------------------+
| ===[deaths: | | |  ]=====@==========================  14:32|  <- timeline: death
| ===[heat band: (light)(mid)(dark)(mid)(light)]============ |     ticks, density
+-------------------------------------------------------------+     band, scrub-drag
| Layers:  [x]Trails 20s  [x]Heat  [ ]LOS fan  [ ]Front(s)   |
| Confidence >======o====<  0.0    [0.55 default]     1.0    |  <- team-conf slider
| Xray:( )on (o)off   Trail len:[20s v]                       |
+-------------------------------------------------------------+
| Avatars: [ ] show only avatars with evidence chips           |  <- Q8 browse filter
+-------------------------------------------------------------+
| Pinned: [Kestrel] [Weapon:AK-Prime] [+]                     |
+-------------------------------------------------------------+
| Recording: ON  •  events 41,203  •  agents 34  •  unattr. 12|  <- status line
+---------------------------------------------------------------+
```
Every widget is a stock XUI control (buttons, checkboxes, slider, combo box) except the timeline strip, one custom `LLView` drawing lines/quads/text (same technique as the storm-debug overlays already in the codebase). The evidence checkbox filters the existing Avatars tab to those with ≥1 chip — a filter, not a ranking; nothing is summed into a score.

### B. Floater: Combat Inspector (thin companion, ~300×260)

```
+-- INSPECTOR --------------------------------------[_][X]--+
| DEATH  14:32:07                                            |
| Kestrel  <-  Owlblade   weapon: AK-Prime (bullet)           |
| damage 62 (killing blow)  type: impact                     |
| LOS: BLOCKED (3/3 candidates) - see fan in world            |
+-------------------------------------------------------------+
| Damage chain (last 6s):                                     |
|  14:32:02  Owlblade -> Kestrel   18 dmg   impact              |
|  14:32:05  Owlblade -> Kestrel   22 dmg   impact              |
|  14:32:07  Owlblade -> Kestrel   62 dmg   impact  (killing)   |
+-------------------------------------------------------------+
| [Focus in world]  [Track Owlblade]  [Pin]  [Assign team]     |  <- plain-click
+-------------------------------------------------------------+     card actions
```
This floater is the *reading fallback*: everything, including every action, mirrors the selected in-world card. `Focus in world` recentres alt-cam's pivot without switching camera mode, since that would break the gating rule.

### C. On-world dossier card (billboard, screen-facing)

```
        .-------------------------------.
        |  KESTREL              [team:  |
        |  Ashguard  62% conf.]  /\      |   <- evidence triangle if chips exist
        |  K 4  D 1   dmg out 210       |
        |  dmg in 340   mouselook 12%   |
        |  [bar] adj. ratio 0.71        |   <- mini bar (quads), not a real chart
        |  weapons: AK-Prime, Frag HUD  |
        |  ------------------------------ |
        |  > Team  > Deaths(1) > Kills(4)|   <- link-chips
        |  [Pin] [Track] [Compare] [Assign team]| <- action chips
        '-------------------------------'
              v (leader line to the ghost's feet)
```
Drawn from the same primitives as any overlay marker: a filled low-alpha quad, `hud_render_utf8text` lines, bar-quads, and screen-rect picking extended to each chip's sub-rect at a fixed minimum hit size. A plain click on a chip fires its action; a plain click elsewhere on the card opens the dossier. There is no held-click or modifier-click menu — this stays inside the plan's existing click hook, which consumes only a plain left-click on a combat marker's rect; ALT/CTRL, camera drag, and right-click all fall through untouched, so nothing here competes with SL's object pie menu or alt-cam's drag-to-orbit. Overlapping cards offset vertically (same trick nameplates use); at most 8 unpinned cards render at once (proximity + recency), pinned cards exempt.

### D. Front Overview (pop-out panel, 2D fallback)

```
+-- FRONT OVERVIEW (10-min buckets) --------------------------+
| [00-10]  [10-20]  [20-30]  [30-40]  [40-50]  [50-60]        |
|  (ribbon)  (ribbon) (ribbon) (ribbon) (ribbon) (ribbon)      |
|  one mini-ribbon per active team-pair per bucket;             |
|  x=front position, y=density, colour=pair, dot=damage        |
+---------------------------------------------------------------+
```
Exists because an in-world ribbon is hard to read from an arbitrary alt-cam angle. A three-way fight draws three overlapping mini-ribbons per bucket rather than forcing one line through three clusters. Clicking a mini-ribbon jumps the timeline and re-anchors the matching in-world ribbon(s).

## 5. In-world overlay

| Rung | Draws | Colour / encoding | Click target | Uncertainty cue |
|---|---|---|---|---|
| 0 | ghost (capsule + yaw arrow + label + evidence triangle), damage flashes, single LOS ray | team colour on ghost; damage-type colour on line; LOS ray green/red/grey | ghost → dossier; line → event card; ray → LOS detail; triangle → Behaviour evidence | dashed/faded ghost when track quality is COARSE or >2 s stale; no track sample at all → static "unknown position" marker at last-seen, tagged, never silently frozen mid-motion |
| 1 | trails, tick ghosts, death crosses, evidence triangles | team colour, alpha old→new | trail point → jump cursor; cross → death card; triangle → Behaviour evidence | gap >5 s draws a faint dotted connector, never solid; a logged-off avatar's trail fades to nothing over a fixed window instead of continuing |
| 2 | heat decals (coarse grid), LOS fan, confidence-tinted ghosts | heat: sequential ramp, never red-heavy; fan: green/red/grey | hot patch → density peak; fan ray → candidate instant | heat cells built mostly from COARSE/unattributed samples render blurred/desaturated, and unattributed events are tallied separately; fan shows every candidate including UNKNOWN (grey, dashed); label states "n candidates, false positives possible from travel time/lag" |
| 3 | front ribbon(s), projected dots | ribbon grey, striped by flanking teams when 3+ clusters active; dots team-coloured, size = damage | dot → avatar/time; ribbon segment → Place card | width balloons where that pair's fit residual is high — width *is* the uncertainty encoding; a pair with too little mutual damage to fit draws no ribbon at all rather than a meaningless one |

Clicking never requires leaving alt-cam or third-person: the pick hook fires on plain left-click only while gating is "on" (stationary + alt-cam + 5 s grace); ALT/CTRL and camera drag are untouched, and no held-click or right-click behaviour is introduced anywhere. Depth-tested lines draw twice (full-alpha depth-tested, 30% depth-off) so a marker behind a wall is visibly "there but occluded" — the honest rendering of a BLOCKED verdict: you can *see* the wall it's blocked by.

## 6. Analysis model

- **Team + confidence.** Label-propagation over the damage graph (edge weight = damage dealt), *seeded* by active group id — the seed only initialises starting labels; iteration reassigns by actual edge weight, so two squads sharing a group id split apart because their mutual damage is negative evidence that outweighs the shared starting label after a few rounds. Confidence = normalized margin between best and second-best cluster score. The slider re-thresholds render colour only, defaulted to the module's own confident cutoff, and never hides the printed confidence number. An avatar with no edges yet stays ungrouped/grey.
- **Front line(s) / distance-to-front.** For every cluster pair with mutual damage above a floor, fit a separating line (1D PCA perpendicular to the pair's centroid vector) over DEATH/DAMAGE positions in a trailing window (default 2 min), recency-weighted — reusing the plan's existing K-cluster output, not a second clustering pass. Distance-to-front = signed perpendicular distance from the nearest active front. Residual (mean squared scatter) drives that ribbon's width alone, so a three-way fight with one clean front and one messy one shows exactly that instead of one number standing in for the whole battle.
- **LOS verdict + candidate set.** Per the engineering plan: candidates across the travel window, each raycast, verdict CLEAR/BLOCKED/UNKNOWN. Through-wall suspicion per equipment = (# kills all-BLOCKED) / (# kills ≥1 tested), always shown with its denominator ("3/4, small sample — treat as a lead").
- **Damage adjustment ratio.** `damage / initial` per event, aggregated per avatar/team/equipment, attributed to the `modifications[].script`; shown as the card's mini bar and an Adjustment-card distribution strip.
- **Equipment classification confidence.** A checklist (attach point, source==rezzer, rezzer chain, movement, seated-and-firing) shown verbatim; thin evidence reads "UNKNOWN (evidence: attached, rezzer unseen)" rather than a guessed category.
- **Battle heat.** Built on a coarse 4 m grid from the same `sampleAt()` reconstruction the damage-line draw already needs, since DAMAGE has no positions of its own. An event whose attacker has zero track coverage at time t is excluded from the field and tallied instead in a visible "unattributed" counter — never smeared onto a nearby cell as if its position were known. Cell blur/desaturation is the fraction of contributing weight from COARSE/stale samples. Recomputed incrementally as the window slides.
- **Behaviour evidence (never a verdict).** Three independent chips: reaction-time-before-hit (shot time vs the avatar's movement/aim change, net of a latency budget, floored by the 2 Hz bridge tick and reported as a window — e.g. "400–600 ms," never a false-precision single figure, with an explicit note that the number is a product of the track's interpolation model as much as the avatar's behaviour), speed-vs-recent-norm, and turn-toward-unseen-attacker. Each states its own uncertainty and links to the instant that produced it; never summed into a suspicion score, per the brief. An avatar with any chip carries the evidence triangle at rungs 0–1 and appears under the Avatars-tab "has evidence" filter, so the officer can browse the whole raid for outliers before picking a suspect.
- **Track quality.** Every sample carries source (VIEWER/COARSE/BRIDGE) and staleness. A freshly-arrived avatar with too few samples for a trail yet draws solid at its one known point without a trail; a departed avatar's trail freezes at its last sample and fades over a fixed window (default 30 s) instead of implying continued presence.

## 7. Walkthroughs

**Q1 — how did the raid go?** Officer opens the floater in Live mode, hits `|<` to session start, enables Heat + whole-session trails — the state the **Overview** chip now remembers. The terrain lights up with 2–3 bright patches, solid where samples are good and blurred where the field leans on COARSE data. Clicking the brightest patch jumps to its density peak; trails collapse to that window; toggling Front(s) shows one stable ribbon there, and the Front Overview shows it wobble early before locking in mid-raid. Clicking **Overview** at any point snaps straight back with no re-configuring.

**Q3 — why did X die at 14:32?** A death cross near the heat patch opens Kestrel's Death card, mirrored in the Inspector. The damage chain (three impact hits from Owlblade) is listed; clicking the killing-blow line enables the LOS fan — three candidates all render red, depth-tested rendering showing the intervening wall. Clicking one candidate drops to rung 0: Owlblade's ghost is visibly behind the wall, confirming BLOCKED with the standard false-positive caveat.

**Q8 — suspicious behaviour?** Before picking a suspect, the officer ticks "show only avatars with evidence chips"; four names appear, each carrying the evidence triangle — a browse, not a ranking. Opening one dossier's Behaviour evidence: "reaction window 400–600 ms before first shot lands (n=1, floored by 2 Hz track resolution — could be lag or interpolation, not measured sub-tick)" and "turned toward attacker 0.4 s before attacker was visible to any tracked geometry (n=2)." Clicking the second's `Focus in world` link jumps to rung 0: the yaw arrow is visibly pre-aimed at a spot behind a hill. No verdict — only evidence, each with its own sample size and caveat.

**Intra-group skirmish.** Two Ashguard squads fighting each other: the Team card shows one declared group, but the damage graph splits into two dense sub-clusters once propagation runs past the shared seed — mutual damage outweighs it. The UI labels them "Ashguard-A / Ashguard-B (inferred — same declared group)," grey-striped until confidence clears the slider's default cutoff. Friendly-fire counting checks the inferred sub-team, not the group tag. The officer locks the split via the **Assign team** chip on any card, treated as ground truth thereafter.

**FFA scenario.** In a genuine neutral deathmatch, clustering confidence stays low across the board and every pairwise front-fit residual is high — no ribbon renders anywhere. The system falls back to duel-pair statistics: each dossier lists "no stable team — 6 distinct opponents this session." (Contrast: a sim with three coherent, persistent factions instead produces three confident clusters and a triangle of three well-fitted ribbons, per §2/§6 — the design tells the two cases apart rather than flattening a real three-way war into "noisy FFA.")

## 8. Risks and open questions

- **Billboard clutter at scale.** The 8-card cap (proximity + recency, pinned exempt) helps, but many pinned cards with stacked leader lines can still occlude geometry; a screen-density fade for pinned cards remains open.
- **Picking through geometry.** Depth-tested picking (needed for the "see it's occluded" LOS cue) risks selecting things the officer can't actually see; needs a precedence rule (nearest depth-tested hit wins unless Xray is on).
- **Front-line fit is a model, not a fact.** Per-pair width communicates "no coherent front for this pair," but officers may still misread ribbon height as a hard number; needs an explicit "estimated, residual = N m" caption per ribbon.
- **Heat-field cost at full-session scale.** The incremental accumulate/decay scheme is designed, not load-tested against a 2 h/~100k-event session at "whole session" window; may need a coarser grid or a sample cap at that setting.
- **Colour budget.** Team colour, damage-type colour, front-pair striping, and LOS colour compete for the same channel, especially with 3+ team stripes; needs a colourblind-safe audit, with shape redundancy (arrows, dash patterns, the evidence triangle) carrying information wherever colour fails.

**Rejected keep.** The comprehension review's keep-list asked to preserve the click-and-hold radial/pie menu "even though the trigger condition needs fixing." Dropped instead: no trigger fix removes the collision between a held click and alt-cam's drag-to-orbit, or the conflict with the plan's Phase 4.4 click hook (plain left-click only) and SL's own right-click object pie menu. Its actions now live as plain-click chips on the card and its Inspector mirror — same zero-learning-cost reuse, no new input-precedence engineering.
