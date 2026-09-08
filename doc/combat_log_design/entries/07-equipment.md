# Entry 07 — Equipment-first Combat Log

## 1. Thesis

Every combat session is, underneath the avatars, a population of *things that hurt people*: rifles, bullets, HUDs, grenades, turrets, cars, gun mounts. This design makes equipment the primary noun. Avatars, deaths, teams and suspicion are all views you reach by asking "what did this weapon do" or "what carried this damage" — not the other way around. An **Equipment Codex** is the flagship screen: every item the log has ever seen, grouped by kind, each with a damage/kill profile, a classification-confidence readout, an adjustment interaction table, and a through-wall suspicion score that always shows its sample size and caveats. Deaths and avatars still get their own pages, but their pages are built by *following equipment links out and back*: a death's dossier is a chain of equipment (source → rezzer → owner), an avatar's page is a loadout sheet, a team's page is a shared armory. The officer answers "who killed whom" by first answering "what killed whom," which is the honest order for a game where the *thing* is more reliably observed than the *intent* behind it.

## 2. Ladder of abstraction

- **Rung 0 — concrete, one shot.** In-world overlay at the scrub cursor: a single glowing line from the exact firing position to the exact hit position, weapon glyph at the muzzle, hit spark at the target, a small floating tag naming the item. Step up: click the tag → opens that item's Equipment Dossier. Step down from anywhere: any equipment/avatar/event page has a "Show in world" button that jumps the cursor to a representative instant and re-centers the overlay there.
- **Rung 1 — abstract over time, one item's whole session.** The Equipment Dossier's **Fire Fan**: every shot that item ever took, drawn simultaneously as faint lines fanning from its firing positions to its hit positions, opacity by recency-independent count (so it reads as a static "photograph" of the weapon's whole campaign, not a replay). Step down: click any single line in the fan → cursor jumps to that shot's time, rung 0 re-appears with just that shot lit. Step up from rung 0: the "show full history" toggle in the HUD-style corner readout expands the current shot into its fan.
- **Rung 2 — abstract over a parameter.** Two instances: (a) the **LOS candidate sweep** — for one kill, drag a slider over the bullet-travel window and watch the candidate shooter position and its CLEAR/BLOCKED verdict update; this is literally Victor's "vary the parameter" applied to travel-time uncertainty. (b) The **adjustment-ratio sweep** in an armor item's dossier — a slider over incoming damage types replays the same reduction table filtered by type, showing whether an armor script is type-selective. Step down: any point on the sweep can be pinned, which drops a rung-0 marker for that exact instant in the overlay.
- **Rung 3 — small multiples, cross-item.** The **Equipment Leaderboard** (Overview tab): one compact row per item — sparkline of activity, kill/damage bars, suspicion chip, adjustment chip — all items visible at once, sortable. Step down: click a row → its Dossier (rung 1). Step up: select several rows → they pin into the **Loadout Comparison** floater, a coordinate-free multiples-of-multiples view.
- **Rung 4 — coordinate transform.** The **Engagement Envelope**: for a weapon, "unbend" 3D space into a single axis, hit distance (computed from attacker/target track samples at event time), and histogram every hit against it. A rifle with a tight 20–40 m band and a "sniper" with a flat histogram from 5–150 m are visually distinct without ever looking at a map. Step down: click a histogram bucket → filters the Fire Fan (rung 1) to just those shots, then any shot drills to rung 0.

## 3. Information architecture

Nouns, each a page with backlinks and a breadcrumb trail (rendered as a thin clickable strip at the top of the Inspector: `Session > Death @14:32 > "SA-90 Mk2" > Wielder: Kestrel`). A history list (last 20 visited nouns, in a dropdown next to the breadcrumb) lets the officer jump back without re-clicking.

- **Equipment item** (`Equipment Dossier`): one physical/logical thing — a specific rifle template, a bullet family, a HUD, a deployable, a vehicle, a mount. Links to: its wielder(s)/owner(s), every death/damage event it's implicated in, its rezzer (parent item) and its rezzed children (a HUD links down to the bullets it spawned via a weapon), the team(s) that used it, its through-wall evidence list.
- **Equipment family**: a template-level grouping (see §6) collapsing ephemeral projectile UUIDs and re-rezzed deployables into one entry — this *is* what the Codex lists; the raw per-UUID instance is one level down, reached by expanding a family row.
- **Death**: `Death Dossier` — victim, time, the damage ladder leading up to it, the killing-blow equipment, LOS verdict. Links to every equipment item and avatar involved; backlinked from those items' "implicated in" lists.
- **Avatar** (`Loadout Sheet`): not a stats page first — a *gear list* first (every item they wielded/wore/rode this session, by kind), then kills/deaths/damage as secondary tables, then behaviour (mouselook %, movement, seat time). Links to every equipment item in its loadout and to its team.
- **Team** (`Armory Board`): roster × equipment-kind matrix, damage share, friendly-fire ledger. Links to every avatar and every item used by the team.
- **Place / moment**: not a dedicated page in this entry (kept minimal per "few, deep views") — reached only as a cursor position via "Show in world," never a browsable noun of its own.
- **Unknown object**: a degenerate Equipment Dossier — same page, evidence panel mostly empty, an explicit "why unknown" line (no viewer sighting, bridge query timed out, de-rezzed before facts arrived).

Navigation model: single shared Inspector floater re-skins its body to whichever noun is selected (avoids floater sprawl); a **pin** icon on the Dossier detaches the current view into a free-floating read-only card (feeds the Loadout Comparison floater when ≥2 are pinned); breadcrumbs and a "back" arrow live in the Inspector's fixed header regardless of which noun is showing.

## 4. Screens

### 4.1 Main floater — Combat Log (Overview tab, default landing tab)

```
+-[ Combat Log ]---------------------------------------------------------+
| [Live] [|<] [<] [>] [>|]  speed:[1x v]  step:[-1s][+1s]   REC * 02:14:07|
| Timeline -----------------------------------------------------------   |
| |..||.||||...|##|.|.||.|##|....||.|.|.||##|.||...|.....|##||..|.|..|   |
|                                   ^cursor 00:41:12          deaths=## |
+--------------------------------------------------------------------+---+
| [Overview] [Equipment] [Teams] [Avatars] [Events]                       |
+--------------------------------------------------------------------+---+
| SESSION AT A GLANCE                        TEAM DAMAGE SHARE           |
| deaths: 37   damage events: 4,812          Ashguard  [######----] 61%  |
| unique equipment seen: 58 (12 unknown)     Wraith    [####------] 39%  |
|                                             friendly fire: 3 events    |
| EQUIPMENT LEADERBOARD (top 8 by damage)   sort:[Damage v] filter:[All v]|
| item                 kills dmg    sus.  adj.   activity                |
| SA-90 Mk2 (rifle)      9   4120   12%   x0.94  ▂▃▅▇▆▃▂▁▂▃▅▇▃▁         |
| Frag Trap (deployable) 4   1880    -    x1.00  ▁▁▅▁▁▁▇▁▁▁▁▅▁▁▁         |
| Wraith AC (vehicle)    2    990    -    x1.00  ▁▁▁▃▇▇▇▅▃▁▁▁▁▁▁         |
| "Unknown-a91f"         1    640   88%⚠   x1.00  ▁▁▁▁▁▁▁▁▁▇▁▁▁▁▁         |
| ...                                                                    |
+--------------------------------------------------------------------------+
```
Top region is the decided transport bar + timeline strip (tick marks = events, `#` = death). Tab strip below it. The Overview body is two panels: a small stacked team-damage bar (filled quads, one colour per team, proportional widths) plus a friendly-fire counter that links to the Teams tab; and the Equipment Leaderboard, a custom `LLScrollListCtrl`-like list where the "activity" column is a hand-drawn sparkline (polyline over 15 time buckets) and "sus." is a coloured percentage chip (grey = insufficient sample, amber/red = flagged) with a `⚠` glyph once past a confidence threshold. Row click opens the Inspector as an Equipment Dossier.

### 4.2 Main floater — Equipment tab (the Codex)

```
+-[ Equipment ]-----------------------------------------------------------+
| group by: [Kind v]   filter: [ ] Weapons [ ] Projectiles [ ] HUDs        |
|           [ ] Deployables [ ] Vehicles [ ] Mounts [x] Unknown            |
| search: [___________]                     N items shown: 58             |
+---------------------------------------------------------------------+---+
| v WEAPON (7)                                                             |
|    SA-90 Mk2            9 kills   61 hits   4120 dmg   sus 12% (n=17)   |
|    Ashguard SMG         3 kills   40 hits   1510 dmg   sus  -  (n=2)    |
| v PROJECTILE (14)                                                        |
|    SA-90 Mk2 · bullet   (rolled into weapon family above)               |
| v DEPLOYABLE (6)                                                         |
|    Frag Trap            4 kills   splash r≈4m   owner: rezzed by HUD    |
| v HUD (9)                                                                |
|    "Ashguard Armor v3"  role: DEFENSIVE  avg reduction 6% (n=310 hits)  |
| v VEHICLE (2) / MOUNT (3)                                                |
| v UNKNOWN (12)  -----------------------------------------------------   |
|    obj-a91f3c           1 kill  640 dmg  sus 88%⚠  evidence: none (derezzed <0.3s) |
+---------------------------------------------------------------------+---+
```
A collapsible-tree scroll list (native `LLScrollListCtrl` supports flat rows; the kind headers are non-selectable divider rows, a pattern already used elsewhere in the viewer). Every row is a *family*; an expand arrow (not drawn above for space) reveals raw instances. Unknown-kind rows always show an "evidence: ..." reason instead of blank space — never a silent gap.

### 4.3 Inspector floater — Equipment Dossier mode

```
+-[ Inspector ]  Session > "SA-90 Mk2" -------------------------- [pin][x]+
| SA-90 Mk2                                    kind: WEAPON  conf: 92%    |
| creator: Kestrel Vex   owner-of-record: (varies, 3 wielders)            |
| classification evidence:                                                |
|   [x] attach point 34 (HUD point, handheld override) seen on 3 avatars  |
|   [x] source==rezzer on 61/61 hit events (self-fired, not a projectile) |
|   [ ] bridge CombatObjectInfo confirm (2 of 3 instances timed out)      |
+---------------------------------------------------------------------+---+
| DAMAGE PROFILE                        | THROUGH-WALL SUSPICION          |
|  final dmg total   4120               |  verdicts: 12 BLOCKED / 5 CLEAR |
|  initial dmg total 4380 (adj x0.94)   |  suspicion 12%  (n=17, LOW-CONF)|
|  hits 61  kills 9  avg dmg/hit 67.5   |  caveat: travel window 1.2s,    |
|  dmg types: [Impact 70%][Explosive30%]|   3 of 12 candidates had gaps   |
|  engagement envelope (hit distance):  |  [see kill-by-kill breakdown >] |
|  0m  ▁▂▅▇▇▅▃▁▁▁▁  120m  (tight band)  |                                  |
+---------------------------------------------------------------------+---+
| USERS (3)                    | IMPLICATED DEATHS (9)  | RELATED         |
| Kestrel Vex  6 kills          | 14:32 Rowan  [open]    | rezzes: bullet |
| Talia Ren    2 kills          | 14:41 Doe    [open]    |  family (14)   |
| Unknown alt  1 kill  (owner   | ...                    | seen on team:  |
|   key resolved, name pending) |                        |  Ashguard      |
+---------------------------------------------------------------------+---+
```
Four stacked panels under a fixed header (breadcrumb + pin/close). Left/right split panels drawn as two child `LLPanel`s; the engagement-envelope and activity sparklines are custom-drawn polylines/bars, same primitive style as the storm overlay helpers. Every uncertain number carries its `n=` and a plain-language caveat line — never a bare percentage.

### 4.4 Inspector floater — Death Dossier mode

```
+-[ Inspector ]  Session > Death @14:32:07 — Rowan ---------------[pin][x]+
| victim: Rowan            killing blow: SA-90 Mk2 (Kestrel Vex)          |
| pos: (128,64,22)  team(Rowan)=Wraith(88%)  team(Kestrel)=Ashguard(95%)  |
+---------------------------------------------------------------------+---+
| DAMAGE LADDER (window 14:31:52 - 14:32:07)                              |
|  14:31:53  Ashguard SMG    · Talia Ren     dmg 40  (initial 42, adj)    |
|  14:31:59  SA-90 Mk2       · Kestrel Vex   dmg 55                        |
|  14:32:07  SA-90 Mk2       · Kestrel Vex   dmg 70  <- killing blow      |
+---------------------------------------------------------------------+---+
| LINE OF SIGHT                          candidate sweep: [<]===o===[>]  |
|  candidate t-0.0s  pos A   BLOCKED (wall)                                |
|  candidate t-0.4s  pos B   CLEAR                                        |
|  candidate t-0.8s  pos C   UNKNOWN (no track sample)                    |
|  verdict: CLEAR (>=1 clear candidate)   caveat: bullet travel ≈0.6-1.2s |
+---------------------------------------------------------------------+---+
| [Show in world]   [Track Kestrel Vex]   [Track Rowan]   [Open SA-90 >] |
+---------------------------------------------------------------------+---+
```
The LOS row is the rung-2 sweep control: dragging the slider re-renders the candidate marker in-world live. "Open SA-90 >" is the equipment-first exit link that makes this dossier a stop on the way to, not the end of, the digging path.

### 4.5 Main floater — Teams tab (Armory Board)

```
+-[ Teams ]----------------------------------------------------------------+
| clustering: label-propagation on damage graph   confidence: overall 71%  |
| [ ] show equipment-signature hint (weak, off by default)                 |
+----------------------------------------------------------------------+---+
|              Ashguard (14, conf 82%)   |   Wraith (11, conf 79%)         |
| weapons      SA-90 Mk2 x3, SMG x2      |   Longbow x2, Frag Trap x1      |
| vehicles     Wraith AC (captured, +2)  |   —                             |
| armor HUDs   "Ashguard Armor v3" x9    |   none detected                  |
| friendly fire  1 event (Frag Trap splash on own trooper, [open])         |
| unresolved avatars (3): Doe(53%->Ashguard), ...  [override v]            |
+----------------------------------------------------------------------+---+
```
Per-team columns list equipment by kind, not just avatar rosters — the "what did each team bring" answer (officer Q7) is native to this screen rather than derived from a stats table.

### 4.6 Loadout Comparison floater (new, pinned-instance view)

```
+-[ Compare ]------------------------------------------------------[x]+
| SA-90 Mk2 (Ashguard)         | Longbow (Wraith)                     |
| dmg/hit 67.5   sus 12%       | dmg/hit 58.0   sus 41% (n=9)          |
| range: tight 20-40m          | range: flat 10-150m                   |
| adj vs armor: x0.94          | adj vs armor: x1.00 (no counter)      |
+-----------------------------+----------------------------------------+
```
Populated only by pinning ≥2 Dossiers; a read-only side-by-side, columns capped at 3 to stay legible in XUI's fixed-panel layout.

## 5. In-world overlay

- **Rung 0**: one bright line (attacker→target, colour = damage type, width by damage magnitude) plus a muzzle glyph at the firing position (small triangle oriented by track yaw) and a hit spark at the target. Depth-tested draw at full alpha *and* a depth-off draw at 30% alpha layered underneath, so a shot that visibly threads a wall the officer is looking through reads immediately as "the bright core is behind geometry."
- **Rung 1 (Fire Fan)**: all of one item's shots at once, thin low-alpha lines, colour by damage type, no time fade — a static density picture. Toggled per selected equipment from the Dossier's "show in world" button; overlapping fans from multiple selected items are allowed but capped at 3 concurrent to avoid clutter.
- **Rung 2 (LOS sweep)**: each travel-time candidate drawn as a ring at its position with a short line to the target; ring colour CLEAR=green, BLOCKED=red, UNKNOWN=grey dashed; the slider-selected candidate is solid and larger, the rest are faint. This is the clearest place uncertainty is *drawn, not stated*: a death with 4 red rings and 1 green ring visually reads as "borderline," not as a false certainty.
- **Unknown equipment** markers: a dashed outline glyph (question-mark tag) instead of the normal weapon glyph, so an unresolved shooter never silently looks identical to a known one.
- Clicking any marker (muzzle glyph, hit spark, ring, vehicle hull outline) opens the Inspector on that noun, screen-rect hit-tested per the decided picking model. Plain left-click only, so alt-cam drag is untouched.

## 6. Analysis model (derived quantities)

- **Equipment family key**: `hash(name, creator, kind-guess)` collapsing same-named/created ephemeral instances (bullets, re-rezzed traps) into one family; the Codex lists families, instance list is one click down. Confidence of merge is shown when names collide across creators (kept separate, flagged "same name, different creator — not merged").
- **Classification confidence**: weighted checklist (attach point known +30, source==rezzer relation resolved +30, bridge/viewer facts arrived +25, behavioural corroboration e.g. seated+moved for vehicle +15), summed and clipped to 0–100%; each contributing check is listed, never a black-box number.
- **Damage profile**: `sum(final)`, `sum(initial)`, `adj ratio = sum(final)/sum(initial)`, hits, kills, `avg dmg/hit`, damage-type histogram — straight aggregation over DAMAGE events keyed by `source`/`rezzer` family.
- **Defensive role & shield profile**: an equipment item is tagged role=DEFENSIVE when its id appears as a `modifications[].task_id` on events where `target == (that item's wearer)`; its dossier then shows *reduction by incoming damage type/weapon* instead of an outgoing profile.
- **Engagement envelope**: for each DAMAGE/DEATH, `distance = |sampleAt(owner_or_source_track, t) - sampleAt(target_track, t)|`; histogrammed per family. Bins with only COARSE-quality samples on either side are shown hatched, signalling lower positional confidence.
- **Through-wall suspicion**: per family, `suspicion = BLOCKED / (BLOCKED + CLEAR)` over its implicated deaths' LOS verdicts (UNKNOWN excluded from the ratio but counted in `n` and surfaced as "N unresolved"); below `n=5` the chip renders grey "insufficient data" regardless of ratio, never a misleadingly precise percentage from one event.
- **Rate of fire / activity sparkline**: event count per 15 time-buckets across the session, min-max normalized per row (comparable shapes, not comparable absolute heights across rows — noted in a legend line).
- **Team armory aggregation**: for each resolved team (from the decided damage-graph clustering), group its avatars' loadout sheets by equipment kind; this is a pure join, no new inference, but it is equipment-first because it's the *first* thing the Teams tab shows, ahead of the roster.
- **Equipment-signature team hint (secondary, off by default)**: cosine similarity of two avatars' equipment-family sets as a very low weight nudge into the same clustering pass. Explicitly suppressed when the group-id seed already agrees (intra-group skirmishes: everyone owns the same faction gear, so this signal is useless there and the UI hides the checkbox's effect rather than pretending it helps).

## 7. Walkthroughs

**Q1 — how did the raid go?** Overview tab: timeline shows two dense bands separated by a quiet gap (regroup); Equipment Leaderboard's activity sparklines show the same two-hump shape for the top weapons, confirming it's a session-wide lull, not one squad's; team-damage bar shows 61/39 split. Scrub into band one, watch the overlay at rung 1 (turn on Fire Fans for the top two weapons) — the fans cluster around a doorway, telling the officer where "the fighting happened" without ever opening a map tool.

**Q3 — why did Rowan die at 14:32?** Click the death tick on the timeline (or the marker in-world) → Death Dossier opens: damage ladder shows two hits, killing blow from SA-90 Mk2/Kestrel Vex; LOS sweep shows 1 CLEAR candidate among 4, verdict CLEAR with the travel-time caveat visible; officer opens "SA-90 Mk2 >" and lands on its Dossier, sees suspicion is 12% (n=17, not flagged), decides this was a clean kill.

**Q8 — suspicious behaviour?** Codex sorted by suspicion: "obj-a91f3c" (Unknown) sits at 88% with `n=1` → chip renders grey "insufficient data," correctly refusing to accuse on one sample. Next row, a named rifle at 41% with `n=9` is amber. Officer opens it, reads the kill-by-kill LOS breakdown (each of the 9 deaths individually), opens the wielder's Loadout Sheet from the Users panel, sees 97% mouselook time and sub-150ms first-shot latency across three separate re-engagements — all shown as raw evidence rows with timestamps, no "cheater" verdict anywhere in the UI.

**Intra-group skirmish** (two Ashguard teams): group-id seed collapses to one cluster, so clustering falls back entirely to the damage graph; the equipment-signature hint is auto-suppressed (both sides carry "Ashguard Armor v3"). Friendly-fire ledger on the Teams tab is what actually reveals the split forming: damage edges start clustering into two dense subgraphs before either side's group tag would say anything, and a Frag Trap's splash catching a nearby ally shows up as the one flagged friendly-fire row rather than being silently folded into the wrong team's kill count.

**FFA scenario** (Bad Space): clustering confidence stays low network-wide (many small, weakly-connected components); the Teams tab shows this honestly as "no coherent teams — treat as deathmatch" instead of forcing two boxes, and the overlay falls back to per-avatar hashed colours instead of team colours. The Equipment Leaderboard and Codex work identically either way, since they never depended on team resolution — which is the payoff of putting equipment, not teams, at the center.

## 8. Risks and open questions

- **Family-key false merges/splits**: same-named weapons from different scripters, or a rezzer script that randomizes bullet names for lag, could over- or under-merge. Needs an officer-visible "split family" / "merge family" override, mirroring the team override pattern.
- **Suspicion score legibility**: percentages invite over-trust even with `n=` shown; may need a stronger visual floor (e.g., refuse to render a percentage at all below `n=5`, show a bare tally instead).
- **Armor-HUD role detection** depends on `modifications[].task_id` reliably matching a resolvable object; if that id never resolves to viewer/bridge facts, the "defensive role" tag can't be assigned and the item sits misclassified as generic HUD.
- **Fire Fan performance** at high shot counts (a spray weapon with hundreds of hits) needs a drawn-line cap with random subsampling, disclosed in the corner readout ("showing 300 of 1,240 shots").
- **Unknown-object volume**: fast-derezzing bullets may dominate the Unknown bucket early in a session before bridge facts land; the Codex should auto-promote items out of Unknown as facts arrive rather than requiring a manual refresh.
