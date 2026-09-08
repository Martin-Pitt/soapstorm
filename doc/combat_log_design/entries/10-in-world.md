# Entry 10 — In-World-First: the battlefield is the interface

## 1. Thesis

The officer already knows how to read a battlefield by standing in it and turning their head — that is the entire premise of alt-cam review. So the combat log should not be a spreadsheet with a 3D preview bolted on; it should be a **haunted replay of the terrain** where every fact the analysis modules produce is rendered as something you can walk up to, orbit, and click, and the two floaters exist only to hold the transport controls and a breadcrumb trail back to where you've been. Ghost avatars replay the fight on the ground where it happened; trails, heat, and line-of-sight fans are drawn *on* the world, not charted beside it; clicking a marker opens a small billboard card floating at that exact 3D point, not a modal dialog that steals your view. Digging (Wikipedia-style) means walking from one glowing thing to the next — a death card links to a killer's ghost, which links to a weapon halo hovering over their attachment point, which links to a team-colour aura — and the floaters just remember the path. The ladder of abstraction is a camera problem: rung 0 is standing at the spot at the moment; every rung above is a different way of *painting the same ground* — trails paint time onto space, heat paints density onto space, the front-ribbon bends space itself. You never leave the world to go up the ladder; you only change what light falls on it.

## 2. Ladder

**Rung 0 — Ghost moment (concrete).** At the scrub cursor, every tracked avatar is drawn as a translucent ghost at its interpolated position/pose; damage lines flash for ~1 s around the cursor; a live LOS ray (if a death/damage is at the cursor) is cast and drawn as it actually resolved. This is literally standing there.
*Step up:* press **Fan** (floater button or radial action) to reveal that avatar's/area's trail — rung 0 → 1.

**Rung 1 — Trails (abstract over time).** Stroboscopic polylines per avatar over a chosen window (20 s default, up to the whole session), team-coloured, alpha ramping old→new, with tick-mark ghosts every N seconds and persistent death crosses. This is the "helicopter over the city" view, but the helicopter is you, alt-cammed above the sim.
*Step down:* click anywhere on a trail → cursor jumps to that instant, the trail collapses back to a single rung-0 ghost at that point, camera does **not** move (you stay in control).
*Step up:* select ≥2 avatars or "all" → **Heat** aggregates into rung 2.

**Rung 2 — Aggregate field (abstract over parameters).** Two instruments, same rung:
- **Battle heat**: a ground-projected translucent field (quad decals on the terrain) coloured by damage-event density in the current window — where the fighting actually was, independent of who.
- **LOS candidate fan**: for a selected death/damage, every candidate shooter position across the bullet-travel window is drawn as a translucent ray, green (CLEAR), red (BLOCKED), grey (UNKNOWN) — the *whole travel-time window* rendered at once, not one verdict.
A **confidence slider** (floater) re-tints every ghost's team colour live by the clustering-confidence threshold, so sliding it shows exactly which avatars' allegiance is fragile.
*Step down:* click one LOS ray → jumps to rung 0 at that candidate's instant, shows the single raycast that produced it. Click a hot patch of ground → cursor jumps to the peak-density instant in that area.
*Step up:* toggle **Front ribbon** for rung 3.

**Rung 3 — Unbent space + small multiples (the top rung).** A translucent ribbon is fitted along the inferred front line (see §6) and floats just above the terrain; every avatar is projected onto it as a dot whose height above the ribbon's centre line is signed distance-to-front (advancing vs retreating), coloured by team, sized by recent damage dealt. Chop the session into 10-minute buckets and you get a row of small ribbons side by side (small multiples) — the whole raid's ebb and flow, unbent, in one glance.
*Step down:* click a dot on any ribbon segment → jumps to that avatar at that time, rung 0.
*Step up:* none — this is the top; the way back down is always a click, never a menu.

## 3. Information architecture

Everything is a **card** (a destination). Cards live in-world as billboards anchored to a 3D point, and are mirrored as text in the thin Inspector floater for when the officer wants to read without aiming a camera. Every card has: header, 3–5 stat lines, a footer row of link-chips (click = navigate), and a **Referenced by** strip (backlinks).

- **Moment** (implicit, not a card) — the scrub cursor; the "you are here" of the whole tool.
- **Avatar dossier** — name, inferred team + confidence, kills/deaths/damage, weapons used, time in mouselook, links: → Team, → each Death/Kill event, → each Equipment used, → Behaviour evidence.
- **Event card** (Damage or Death) — time, attacker→target, weapon, damage, LOS verdict, adjustment note; links: → Avatar (attacker), → Avatar (target), → Equipment, → related events (a Death's card inlines its damage chain as backlinks), → Place.
- **Equipment card** — name/creator/kind + classification evidence checklist, users, damage/kills dealt, adjustment-ratio distribution, through-wall suspicion tally; links: → each user Avatar, → sample Events, → Adjustment cards that touched it.
- **Team card** — roster with per-avatar confidence, friendly-fire tally, aggregate damage graph; links: → member Avatars, → friendly-fire Events.
- **Place** — an auto-named location (from heat-field peaks, e.g. "the courtyard choke") the officer can also hand-pin anywhere; links: → Events there, → Teams present, → Front-ribbon segment.
- **Adjustment** — one armour/damage-modifying script instance (task_id): ratio over time, which events it touched; links: → Equipment, → Avatar wearing it.

**Navigation model.** A breadcrumb strip of small chips runs along the top of the thin floater (`Session ▸ Avatar:Kestrel ▸ Death@14:32 ▸ Weapon:AK-Prime`); clicking a chip jumps back to that card without losing later history (classic browser back-stack, not a stack pop). **Pinning** (radial action) keeps a card's billboard floating in-world permanently instead of despawning when you look away, so the officer can pin two avatars' dossiers on opposite sides of the sim and physically fly between them to compare — a spatial substitute for "open in a new tab." **Backlinks** are always visible as the card's own footer, so "what points at this weapon" never requires a separate query.

## 4. Screens

### A. Floater: Combat Log (thin controller, ~360×230, resizable)

```
+-- COMBAT LOG -------------------------------------[_][X]--+
| Session ▸ Avatar:Kestrel ▸ Death@14:32                    |  <- breadcrumb (click = back)
+-------------------------------------------------------------+
| [Live] [|<] [Play] [>|]  speed[ 1x v]  step[-1s][+1s]      |  <- transport
+-------------------------------------------------------------+
| ===[deaths: | | |  ]=====@==========================  14:32|  <- timeline: death
| ===[heat band: ░▒▓█▓▒░░░▒▓]===================          |     ticks, damage
+-------------------------------------------------------------+     density band,
| Layers:  [x]Trails 20s  [x]Heat  [ ]LOS fan  [ ]Front ribbon|     drag to scrub
| Confidence >====o========<  0.0            1.0             |  <- team-conf slider
| Xray:( )on (o)off   Trail len:[20s v]                       |
+-------------------------------------------------------------+
| Pinned: [Kestrel] [Weapon:AK-Prime] [+]                     |  <- pinned cards
+-------------------------------------------------------------+
| Recording: ON  •  events 41,203  •  agents 34  •  RTT 90ms  |  <- status line
+---------------------------------------------------------------+
```
Every widget here is a stock XUI control (buttons, checkboxes, a slider, a combo box) except the timeline strip, which is one custom `LLView` drawing lines/quads/text (same technique as the storm-debug overlays already in the codebase) — well within XUI feasibility.

### B. Floater: Combat Inspector (thin companion, docks beside A, ~300×260)

```
+-- INSPECTOR --------------------------------------[_][X]--+
| DEATH  14:32:07                                            |
| Kestrel  <—  Owlblade   weapon: AK-Prime (bullet)           |
| damage 62 (killing blow)  type: impact                     |
| LOS: BLOCKED (3/3 candidates) — see fan in world            |
+-------------------------------------------------------------+
| Damage chain (last 6s):                                     |
|  14:32:02  Owlblade → Kestrel   18 dmg   impact              |
|  14:32:05  Owlblade → Kestrel   22 dmg   impact              |
|  14:32:07  Owlblade → Kestrel   62 dmg   impact  (killing)   |
+-------------------------------------------------------------+
| [Focus in world]  [Track Owlblade]  [Pin this card]          |
+-------------------------------------------------------------+
```
This floater is the *reading fallback* — everything it shows is a text mirror of the currently-selected in-world card. `Focus in world` recentres alt-cam's pivot on the marker without moving the camera mode (still requires the officer's own orbit); it never force-switches camera modes, since that would break the gating rule.

### C. On-world dossier card (billboard, drawn in 3D, screen-facing)

```
        .-------------------------------.
        |  KESTREL              [team:  |
        |  Ashguard  62% conf.]         |
        |  K 4  D 1   dmg out 210       |
        |  dmg in 340   mouselook 12%   |
        |  ████████░░  adj. ratio 0.71  |   <- mini bar (quads), not a real chart
        |  weapons: AK-Prime, Frag HUD  |
        |  ------------------------------ |
        |  ↳ Team  ↳ Deaths(1) ↳ Kills(4)|   <- link-chips (click-through)
        '-------------------------------'
              ⌄ (leader line to the ghost's feet)
```
Drawn with the same primitive set as any overlay marker: a filled quad background (low alpha), `hud_render_utf8text` lines, tiny bar-quads for the two numeric bars, and screen-rect picking already planned for markers extended to each link-chip's sub-rect. Cards billboard to the camera but anchor to a world point; when two cards would overlap on screen, the later one offsets vertically (cheap "leader line" stacking, same trick minimap and nameplates already use).

### D. Radial context menu (click-and-hold on any marker, or a modifier-click)

```
                Track
                 |
      Pin ---- MARKER ---- Compare
                 |
              Open dossier
      (LOS fan, Show equipment, Assign team... on outer ring
       for event/avatar-specific markers)
```
Plain click on a marker = default action (open dossier card at that point). Click-and-hold or right-click = radial menu with 4–8 context actions (Pin, Track, Compare, Open dossier, Show LOS fan, Assign team override, Show equipment, Back). This reuses SL's existing pie-menu interaction language, so it costs the officer nothing to learn, and it keeps the primary click cheap (a tap opens the obvious thing).

### E. Front-ribbon small multiples (floater "Overview" pop-out panel, optional 2D fallback)

```
+-- FRONT OVERVIEW (10-min buckets) --------------------------+
| [00-10]  [10-20]  [20-30]  [30-40]  [40-50]  [50-60]        |
|  ~~/\~~   ~~\/\~   ~/~~~~   ~~~\/~   ~~~~/~   ~~\~~~/       |
|  (each mini-ribbon: x=front position, y=avatar density,     |
|   colour=team, dot size=damage dealt in that bucket)         |
+---------------------------------------------------------------+
```
This is the only place raw small-multiples charting happens in 2D; it exists because a floating in-world ribbon is hard to read from an arbitrary alt-cam angle, and the brief wants both a "step up" that shows everything and honest XUI feasibility. Clicking a mini-ribbon jumps the main timeline to that bucket and re-anchors the in-world ribbon there.

## 5. In-world overlay

| Rung | Draws | Colour / encoding | Click target | Uncertainty cue |
|---|---|---|---|---|
| 0 | ghost avatar (capsule outline + yaw arrow + name label), damage flash lines, single LOS ray | team colour on ghost; damage-type colour on line (reused hit-marker palette); LOS ray green/red/grey | ghost → dossier; damage line → event card; LOS ray → LOS detail | dashed ghost + reduced alpha when track quality is COARSE or the sample is >2 s stale ("last known") |
| 1 | trail polylines, tick ghosts, death crosses | team colour, alpha old→new | any trail point → jump cursor; death cross → death card | gap in a trail (>5 s no sample) draws as a faint dotted connector, never a solid line — never implies knowledge we don't have |
| 2 | heat decals, LOS candidate fan, confidence-tinted ghosts | heat: perceptual sequential ramp (dark→bright, never red-heavy so it doesn't collide with "blocked"); fan: green/red/grey per candidate | hot patch → jump to density peak; fan ray → jump to candidate instant | fan explicitly shows *all* candidates including UNKNOWN ones (grey, dashed) rather than collapsing to one verdict; label always states "n candidates, false positives possible from travel time/lag" |
| 3 | front ribbon, projected dots | ribbon itself neutral translucent grey; dots team-coloured, size = damage | dot → jump to avatar/time; ribbon segment → Place card | ribbon width balloons where the front-fit residual is high (i.e., the line is a bad fit — FFA scatter) — width *is* the uncertainty encoding here |

Clicking never requires leaving alt-cam or third-person: the pick hook fires on plain left-click only when the overlay is gating "on" (stationary + alt-cam + 5 s grace), exactly as engineered; ALT/CTRL-modified clicks and camera drag are untouched so alt-cam navigation is never interrupted. Depth-tested lines drawn twice (full alpha depth-tested, 30% depth-off) so a marker behind a wall is visibly "there but occluded" — this is also the honest rendering of a BLOCKED verdict: you can *see* the wall it's blocked by.

## 6. Analysis model

- **Team + confidence.** Label-propagation over the damage graph (edge weight = damage dealt), seeded by active group id (per engineering plan). Confidence = normalized margin between an avatar's best and second-best cluster score. The slider in §4A re-thresholds which avatars render "confidently coloured" vs grey-striped live, with no recompute needed — it is a display threshold, not a re-cluster.
- **Front line / distance-to-front.** Within a trailing window (default 2 min), take all DEATH/DAMAGE positions, weight by recency, and fit a separating line via 1D PCA on the perpendicular axis to the vector connecting the two largest team centroids. Distance-to-front for an avatar at time t = signed perpendicular distance from that line, sign by team. Residual (mean squared perpendicular scatter of events around the fitted line) drives ribbon width — a bad fit (FFA, no coherent front) visibly widens/flattens the ribbon rather than lying with a confident thin line.
- **LOS verdict + candidate set.** Per the engineering plan: candidate shooter positions across the travel window, each raycast, verdict CLEAR/BLOCKED/UNKNOWN. Derived **through-wall suspicion** per equipment = (# kills with all-candidates-BLOCKED) / (# kills with ≥1 candidate tested), always shown with the denominator ("3/4, small sample — treat as a lead") so a thin sample never reads as proof.
- **Damage adjustment ratio.** `damage / initial` per event, aggregated (min/median/max) per avatar, team, and equipment, attributed to the specific `modifications[].script`; rendered as the mini bar-quad on dossier cards and a distribution strip on Adjustment cards.
- **Equipment classification confidence.** Not a single label: a checklist of the evidence the plan already tracks (attach point, source==rezzer, rezzer chain, movement, seated-and-firing) shown verbatim on the Equipment card; when evidence is thin the header reads "UNKNOWN (evidence: attached, rezzer unseen)" rather than guessing a category.
- **Behaviour evidence (never a verdict).** Three independent chips on the Avatar dossier: reaction-time-before-hit (candidate shot time vs when the avatar's movement/aim changed, net of a stated latency budget), speed-vs-recent-norm, and turn-toward-unseen-attacker. Each chip states its own uncertainty and links to the in-world instant that produced it; the UI never sums them into a single suspicion score, matching the brief's explicit instruction for question 8.
- **Track quality.** Every sample already carries source (VIEWER/COARSE/BRIDGE) and staleness; ghost alpha and dash pattern are a direct function of this, so "we don't actually know where they were" is always visible, not silently interpolated away.

## 7. Walkthroughs

**Q1 — how did the raid go?** Officer opens the floater in Live mode after the raid, hits `|<` to go to session start, enables Heat + sets trail length to "whole session." The terrain lights up with 2–3 bright patches (the courtyard, the north gate, a brief flare by the spawn). They click the brightest patch — cursor jumps to its density peak (14:31–14:36); trails collapse to that window; they toggle Front ribbon and see it thin and stable there (a real front), then check the small-multiples Overview and see it wobble in the first 10 minutes (skirmishing) before locking in mid-raid.

**Q3 — why did X die at 14:32?** From the heat patch above, a death cross is visible; left-click opens Kestrel's Death card in-world and mirrors it in the Inspector. The damage chain (three impact hits from Owlblade) is listed; clicking the killing-blow line enables the LOS fan — three candidate shooter positions across the travel window all render red, and depth-tested rendering shows the intervening wall. Clicking one candidate ray drops to rung 0 at that instant: Owlblade's ghost is visibly behind the wall, confirming BLOCKED with the standard false-positive caveat text pinned in the card.

**Q8 — suspicious behaviour?** Officer opens the suspected avatar's dossier, scrolls to Behaviour evidence: "reaction time 180 ms before first shot lands (n=1, could be lag)" and "turned toward attacker 0.4 s before attacker was visible to any tracked geometry (n=2)." Each chip has a `Focus in world` link; clicking the second jumps to rung 0 at that instant and the avatar's yaw arrow is visibly pre-aimed at a spot behind a hill. No verdict is rendered — only the two pieces of evidence, each carrying its own sample size and caveat.

**Intra-group skirmish.** Two Ashguard squads fighting each other: the Team card for "Ashguard" shows one declared group but the damage graph splits into two dense sub-clusters; the UI auto-labels them "Ashguard-A / Ashguard-B (inferred — same declared group)" with grey-striped ghosts until confidence clears a threshold. Friendly-fire counting checks the *inferred* sub-team, not the group tag, so genuine cross-squad kills aren't miscounted as friendly fire; the officer can lock the split via the radial "Assign team" action per avatar, which is then treated as ground truth for the rest of the session.

**FFA scenario.** In a neutral deathmatch sim, clustering confidence stays low across the board (many roughly-equal edge weights) and the front-fit residual is high — the ribbon renders wide and flat, correctly saying "no coherent front here." The system falls back to duel-pair statistics: each avatar's dossier lists "no stable team — 6 distinct opponents this session" with per-opponent damage totals, rather than forcing a two-team narrative onto genuinely freeform combat.

## 8. Risks and open questions

- **Billboard clutter at scale.** A busy 60-avatar fight with pinned cards and stacked leader lines could occlude the very geometry the officer is trying to read; needs an auto-declutter (fade cards below a screen-size threshold, cap simultaneous unpinned cards).
- **Picking through geometry.** Screen-rect picking on markers behind walls (needed for the "see it's occluded" LOS cue) risks accidentally selecting things the officer can't actually see when they meant to click the wall itself; needs a precedence rule (nearest depth-tested hit wins unless Xray is on).
- **Radial menu vs. existing pie menu.** Right-click already opens SL's object pie menu; the combat radial must trigger only when the overlay is gating "on" and the click hits a combat marker's rect first, or it will fight the base viewer's context menu.
- **Front-line fit is a model, not a fact.** A wide/flat ribbon communicates "no coherent front," but officers may still misread ribbon *height* as a hard number rather than an estimate; needs an explicit "estimated, residual = N m" caption, not just implicit width.
- **Colour budget.** Team colour, damage-type colour, and LOS verdict colour all compete for the same channel; a colourblind-safe palette audit is needed before this ships, and glyph/shape redundancy (arrows for direction, dash pattern for staleness) has to carry real information wherever colour alone would fail.
