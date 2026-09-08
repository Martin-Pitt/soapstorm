# Entry 07 — Equipment-first Combat Log (Revised)

## 1. Thesis

Every combat session is, underneath the avatars, a population of *things that hurt people*: rifles, bullets, HUDs, grenades, turrets, cars, gun mounts. This design keeps equipment as the primary noun for *cause and identity* — "what did this weapon do," "what carried this damage," "what does this team carry" are answered by digging into an **Equipment Codex**, not a roster. But equipment-first only works once a thing exists to click on; "where and when did people fight" has no equipment attached to it yet, so this revision adds one equipment-agnostic backdrop, the **Engagement Field** — a weapon-blind density picture of every hit and death in the session — as the first thing the officer sees, before any digging begins. Equipment still answers "what" and "why"; the Engagement Field answers "where" and "when" without requiring a guess about which weapon mattered first. Deaths, avatars, teams and suspicion are still reached by following equipment links out and back — a death's dossier is a chain of equipment (source → rezzer → owner), an avatar's page is a loadout sheet, a team's page is a shared armory — but every page, and the Field itself, is now honest about which of its numbers are measured and which are reconstructed, which the first version was not.

## 2. Ladder of abstraction

- **Rung 0 — concrete, whole raid at the cursor instant.** Default overlay, nothing selected: every avatar marker plus a transient spark at every DAMAGE/DEATH event within ±0.5 s of the cursor, coloured by damage type, regardless of weapon — the brief's literal "live 3D overlay at the cursor time," not a single shot. Clicking a spark narrows to that one event and opens its **Event Dossier** (§3); the focused single-shot view — one bright line, muzzle glyph, hit spark — is now a drill-down *from* rung 0. Step up: the "Hot Zones" toggle expands the current instant into the whole-session density field.
- **Rung 1 — abstract over time, two scales.** (a) **Engagement Field**: every DAMAGE/DEATH event all session, weapon-agnostic, plotted as accumulated points shaded by local density — the literal helicopter/all-streets-at-once picture, reached without selecting a weapon. (b) The **Fire Fan** in an Equipment Dossier: one item's shots only, fanning from wielder to target positions, opacity by count. Step down from either: click a bright spot or line → cursor jumps to that event's time, rung 0 re-appears focused. Step up: from rung 0, "show full history" expands to (a) if nothing is selected, or (b) if an item is selected.
- **Rung 2 — abstract over a parameter.** Three instances: (a) **LOS candidate sweep** — drag a slider over the travel window, watch the candidate position and CLEAR/BLOCKED verdict update. (b) **Adjustment-ratio sweep** in an armour dossier — a slider over incoming damage types replays the reduction table. (c) **NEW — allegiance-over-time sweep**: on a Loadout Sheet, a slider over session time steps through that avatar's team-assignment confidence interval by interval (§6), so "was Doe still Ashguard at minute 90" is a sweep, not a frozen number. Any point on any sweep pins a rung-0 marker.
- **Rung 3 — small multiples.** Equipment Leaderboard: one row per item, sparkline, kill/damage bars, suspicion/adjustment chips, sortable. Step down: click a row → Dossier. Step up: select several rows → Loadout Comparison.
- **Rung 4 — coordinate transform.** Engagement Envelope: unbend 3D space into hit-distance, histogram every hit against it per weapon family. Step down: click a bucket → filters the Fire Fan, then rung 0.

## 3. Information architecture

Nouns, each a page with backlinks and a breadcrumb trail. The breadcrumb now carries the session time active at each visit, not just noun identity — `Session@14:32 > Death "Rowan" > "SA-90 Mk2"@14:32 > Wielder: Kestrel@09:10` — so digging several nouns deep never loses track of which moment motivated the dig; the history dropdown shows the same timestamps.

- **Equipment item / Equipment family**: unchanged — rifle template, bullet family, HUD, deployable, vehicle, mount, collapsing ephemeral instances into one family.
- **Event** (`Event Dossier`, replaces "Death Dossier"): now covers *any* DAMAGE or DEATH event, lethal or not. A killing-blow event additionally shows the damage ladder and is labelled `kind: KILL`; a non-lethal hit shows just its own row (attacker, weapon, damage, LOS verdict) labelled `kind: HIT` — directly answering the brief's worked example that a bare damage event should open a page. Links to every equipment item and avatar involved; backlinked from their "implicated in" lists.
- **Avatar** (`Loadout Sheet`): gear list, then kills/deaths/damage, then behaviour, then the new **allegiance timeline** (§6) instead of one team number.
- **Team** (`Armory Board`): roster × equipment-kind matrix, damage share, friendly-fire ledger keyed by which allegiance interval was active at each hit's time.
- **Engagement Field**: deliberately not a page — a toggleable overlay layer plus a "Hot Zones" panel on the Overview tab. The original dropped "Place/moment" to stay minimal; the gap was real, but the fix is this layer, not a digging destination (see rejected critiques).
- **Unknown object**: unchanged — a degenerate Equipment Dossier with an explicit "why unknown" line.

Navigation: unchanged — one shared Inspector floater re-skins per noun; pin detaches a read-only card into Loadout Comparison.

## 4. Screens

### 4.1 Main floater — Combat Log (Overview tab)

```
+-[ Combat Log ]---------------------------------------------------------+
| [Live] [|<] [<] [>] [>|]  speed:[1x v]  step:[-1s][+1s]   REC * 02:14:07|
| Timeline -----------------------------------------------------------   |
| |..||.||||...|##|.|.||.|##|....||.|.|.||##|.||...|.....|##||..|.|..|   |
|                                   ^cursor 00:41:12          deaths=## |
+--------------------------------------------------------------------+---+
| [Overview] [Equipment] [Teams] [Avatars] [Events]                       |
+--------------------------------------------------------------------+---+
| HOT ZONES (weapon-agnostic, whole session)   [x] show in world         |
|  density field: two clusters — doorway (61% of events) and courtyard   |
|  (28%); "reconstructed positions, see caveat" note always shown        |
| SESSION AT A GLANCE                        TEAM DAMAGE SHARE           |
| deaths: 37   damage events: 4,812          Ashguard  [######----] 61%  |
| unique equipment seen: 58 (12 unknown)     Wraith    [####------] 39%  |
|                                             friendly fire: 3 events    |
| EQUIPMENT LEADERBOARD (top 8 by damage)   sort:[Damage v] filter:[All v]|
| item                 kills dmg    sus.  adj.   activity                |
| SA-90 Mk2 (rifle)      9   4120   12%   x0.94  ▂▃▅▇▆▃▂▁▂▃▅▇▃▁         |
| ...                                                                     |
+--------------------------------------------------------------------------+
```
New: the Hot Zones panel sits above the leaderboard and is the first thing the officer reads for "where did this happen" — a plain-text summary of the density field's clusters (grid-binned, §6) plus the toggle that turns the in-world layer on. It answers Q1's geography without any equipment selected; the leaderboard stays the entry point for "what mattered."

### 4.2 Main floater — Equipment tab (the Codex)

Unchanged layout. One correction: expand/collapse of instance rows under a family row is **not** an existing viewer pattern — `LLScrollListCtrl` is flat. It is buildable, but as bespoke logic: expand/collapse rebuilds the visible row set (splices instance rows in/out of the bound `std::vector`, re-labels the divider) rather than relying on a native tree widget. Called out so it isn't underestimated at implementation time.

### 4.3 Inspector floater — Equipment Dossier mode

```
+-[ Inspector ]  Session@09:10 > "SA-90 Mk2" ---------------------[pin][x]+
| SA-90 Mk2                                    kind: WEAPON  conf: 92%    |
| creator: Kestrel Vex   owner-of-record: (varies, 3 wielders)            |
| classification evidence (computed once, at family level):               |
|   [x] attach point 5 (right hand)                            +30       |
|   [x] source==rezzer on 61/61 hit events (self-fired)        +30       |
|   [~] bridge CombatObjectInfo confirm (2 of 3 instances)      +17/25   |
|   [x] behavioural: worn in mouselook by all 3 wielders        +15       |
|   total: 92%   (HUD-range override: n/a — attach point not 31-38)      |
+---------------------------------------------------------------------+---+
| DAMAGE PROFILE                        | THROUGH-WALL SUSPICION          |
|  ... unchanged from original ...      |  ... unchanged from original ... |
+---------------------------------------------------------------------+---+
```
Two fixes from the original: attach point moved from 34 (the plan treats 31–38 categorically as HUD) to 5, removing the self-contradiction; the bridge-confirm line now shows **partial credit** (2 of 3 confirmed = 2/3 of 25, i.e. 17), which is what makes the four lines actually sum to 92%. Confidence is computed **once, from family-aggregated evidence**, never averaged from per-instance scores — removing the aggregation ambiguity the feasibility review flagged. If an item *is* attach-point 31–38 but behaviour contradicts HUD, an explicit override line appears — `[!] overridden by behavioural evidence — attach-point line contributes 0, −10 penalty` — priced and visible, never silent.

### 4.4 Inspector floater — Event Dossier mode

```
+-[ Inspector ]  Session@14:32 > Event — Rowan (KILL) -----------[pin][x]+
| victim: Rowan     kind: KILL     killing blow: SA-90 Mk2 (Kestrel Vex)  |
| pos: DEATH ground-truth (128,64,22)                                     |
| allegiance @14:32 — Rowan: Wraith (79%, interval 00:00-end)             |
|              Kestrel: Ashguard (88%, interval 00:00-end)                |
+---------------------------------------------------------------------+---+
| DAMAGE LADDER (window 14:31:52 - 14:32:07)  [only shown for KILL kind]  |
|  ... unchanged ...                                                      |
+---------------------------------------------------------------------+---+
| LINE OF SIGHT                          candidate sweep: [<]===o===[>]  |
|  ... unchanged ...                                                      |
+---------------------------------------------------------------------+---+
| [Show in world]   [Track Kestrel Vex]   [Track Rowan]   [Open SA-90 >] |
+---------------------------------------------------------------------+---+
```
A non-lethal HIT opens the same floater with `kind: HIT`, no damage ladder, and `pos:` labelled `reconstructed from wielder track (±lag/quality caveat)` instead of `DEATH ground-truth` — the direct fix for the fatal finding that no dossier existed for a bare damage event. The allegiance line reads from the avatar's interval timeline (§6), not a session-wide number, and states which interval is active at this event's time.

### 4.5 Main floater — Teams tab (Armory Board)

```
+-[ Teams ]----------------------------------------------------------------+
| clustering: label-propagation on damage graph   confidence: overall 71%  |
| [ ] show equipment-signature hint (weak, off by default)                 |
+----------------------------------------------------------------------+---+
|              Ashguard (14, conf 82%)   |   Wraith (11, conf 79%)         |
| weapons      SA-90 Mk2 x3, SMG x2      |   Longbow x2, Frag Trap x1      |
| ...                                                                       |
| allegiance drift (2): Doe [Ashg 00:00-00:41 (61%) | Wraith 00:41-end     |
|   (74%)]  [override v]   ...                                             |
+----------------------------------------------------------------------+---+
```
New row: avatars whose damage-graph edges shift cluster mid-session list their interval breakdown, each independently overridable — the fix for the team-over-time gap. Confidence is now the damage-margin formula in §6, not an unexplained scalar.

### 4.6 Loadout Comparison floater

Unchanged; column cap of 3 is a legibility default, not a technical limit — XUI floaters can scroll horizontally past it, noted here rather than implied.

## 5. In-world overlay

- **Rung 0 (whole raid)**: transient sparks at every event within ±0.5 s of cursor, coloured by damage type, small and low-alpha so they read as a field, not clutter; clicking one opens its Event Dossier.
- **Rung 0 (focused single shot, after a click)**: bright line + muzzle glyph + hit spark, but the language changes: this is **not** "the exact firing position." DEATH events use real `source_pos`/`target_pos`, drawn solid, full alpha. Every other event — the overwhelming majority of the log — has no ground-truth muzzle or hit position; the line is reconstructed from `sampleAt(owner_or_source_track, t)`, degraded by lag, decimation and interpolation gaps, and drawn **dashed at 60% alpha** with a "reconstructed" tag on hover. Applied everywhere a position appears, not just here.
- **Rung 1a (Engagement Field)**: session-wide point cloud, grid-binned (§6) into shaded cells, neutral grey/white (not per-team/weapon colour, so it never pre-judges "what" before "where"). Because a flat top-down reading is what this rung needs and the tool has no map view, each accumulated point also drops a faint ground-projected dot beneath it on the terrain — a permanent "shadow" readable from any oblique alt-cam angle without fighting foreshortened 3D lines. This is an in-world decal, not a 2D UI.
- **Rung 1b (Fire Fan)**: unchanged, but target-side hit sparks get the same solid/dashed treatment — a Fire Fan built from DEATH events (rare) is solid, the majority built from DAMAGE is dashed, never rendered identically.
- **Rung 2 (LOS sweep)** and **unknown equipment glyph**: unchanged.
- Clicking: unchanged picking model; any spark, line, or ring now always opens an Event or Equipment Dossier — never a re-centre with nothing to open, closing the original's dead-end on non-lethal markers.

## 6. Analysis model

- **Equipment family key** and **damage profile**: unchanged.
- **Classification confidence**: weighted checklist — attach point known +30 (0 plus a −10 penalty if attach point ∈[31,38] but behaviour contradicts HUD), source==rezzer resolved +30, bridge/viewer facts +25 *scaled by the confirmed fraction of instances checked* (2 of 3 confirmed → 25×2/3), behavioural corroboration +15. Computed **once, directly over family-aggregated evidence** — never averaged from separately-scored instances — so there is one formula, one number, no aggregation ambiguity.
- **Splash estimate** (fixes the original's bare `r≈4m`): cluster DAMAGE events sharing a source/rezzer family within a 0.2 s window that hit ≥2 distinct targets ("a volley"); `radius_est = max pairwise distance between those targets' sampleAt(target_track, t) positions / 2`. Shown as `r≈4m (est., n=6 volleys)`; below `n=3` volleys, "insufficient data," never a bare number next to hard counts.
- **Defensive role & shield profile**: unchanged.
- **Position-source conflict rule** (new): when both an owner/wielder track and a separately-tracked `source` have samples near `t`, prefer `source` — the actual moving object — falling back to owner/rezzer only when `source` has none in the interpolation window. Divergence over 5 m tags the marker "conflicting tracks, showing source" instead of silently picking one; the alternative is available on hover.
- **Engagement envelope**: unchanged, built only from events with at least COARSE-quality samples on both sides.
- **Engagement Field grid**: bin event positions into a fixed 4 m ground grid (matching coarse-location quantisation, no false precision); cell intensity = event count in `[cursor-window, cursor]` live, or whole-session for Hot Zones; updated incrementally, append-only per cell.
- **Through-wall suspicion**: unchanged formula and `n=5` grey floor.
- **Allegiance timeline** (replaces the single per-avatar/team scalar): the damage graph clusters independently per rolling, overlapping 5-minute window; an avatar's timeline is the resulting (interval, cluster, confidence) sequence, merged when adjacent intervals agree. Confidence per interval = *margin* = `(damage-with-winning-cluster − damage-with-runner-up-cluster) / total damage involving this avatar in the interval`, clipped 0–100%; under 3 interactions renders "insufficient data" grey. Team confidence = damage-weighted mean of active members' interval confidences. Friendly-fire keys off the interval active at the event's own timestamp, so a defector's earlier and later friendly-fire classify correctly instead of collapsing into one number.
- **Reaction latency** (new, replaces the invented Q8 figure): `t_visible` = earliest sample where a LOS ray from shooter S clears to target T, T is within a ±60° cone of S's track yaw, and S had no prior CLEAR sample against T in the preceding 15 s (a fresh re-engagement). `latency = t_first_hit(S→T, after t_visible) − t_visible`, requiring BRIDGE-quality samples for both; otherwise "insufficient track quality," never a number. Shown with its own `n=` and the FOV-cone/LOS caveats stated.
- **Rate of fire / activity sparkline**: unchanged, explicitly append-only per bucket on ingest.
- **Team armory aggregation**: unchanged, a pure join over the currently-active allegiance interval per avatar.
- **Equipment-signature team hint**: unchanged, still suppressed when the group-id seed already agrees.
- **Draw-call budget** (design-level, not the engineering plan's job): Fire Fan and Engagement Field primitives capped at a stated design constant (e.g. 2,000 per frame across all active layers, with random subsampling and a "showing N of M" disclosure). The exact number is an implementation tuning parameter for the engineering plan; this entry's job is only to require that a stated, disclosed cap exists.

## 7. Walkthroughs

**Q1 — how did the raid go?** Hot Zones panel reads "two clusters — doorway (61%), courtyard (28%)" before any equipment is touched — the weapon-agnostic answer to "where," fixing the original's dependency on guessing which weapon mattered. Toggled into the world, the density field's ground-projected shadow picks out the doorway even from an oblique alt-cam angle. *Then* the officer cross-references the Equipment Leaderboard's two-hump sparklines and the team-damage bar to learn *who* and *with what* fought there — equipment is now the second question, not the only door into the first.

**Q3 — why did Rowan die at 14:32?** Unchanged: click the death tick → Event Dossier (kind: KILL) → damage ladder → LOS sweep (1 CLEAR of 4) → "Open SA-90 >" → suspicion 12% (n=17), clean kill. The allegiance line now shows which interval was active for both Rowan and Kestrel at 14:32, in case either had switched sides earlier.

**Q8 — suspicious behaviour?** Codex sorted by suspicion: `obj-a91f3c` at 88% (n=1) renders grey, correctly refusing to accuse on one sample. A named rifle at 41% (n=9) is amber; the officer opens the kill-by-kill LOS breakdown, then the Loadout Sheet's reaction-latency panel: "3 qualifying re-engagements, median latency 140 ms (n=3), LOS+60° cone method, requires bridge-quality tracks" — a defined, bounded, honestly-caveated number replacing the original's invented "sub-150ms first-shot latency," with no verdict, only evidence rows and timestamps.

**Intra-group skirmish** (two Ashguard teams): unchanged mechanism — damage-graph fallback, equipment-signature hint auto-suppressed — but the friendly-fire ledger now checks the interval active at each hit's timestamp, so a trooper captured mid-skirmish has earlier and later friendly-fire correctly split rather than folded into one label.

**FFA scenario** (Bad Space): unchanged — clustering confidence stays low network-wide, Teams tab honestly shows "no coherent teams." The Engagement Field, Leaderboard and Codex work identically regardless, since none depend on team resolution — the payoff of keeping equipment, and now geography, ahead of team status.

## 8. Risks and open questions

- **Engagement Field perf at 100k events**: grid-binning must be incremental (§6) and the in-world decal count capped by the Fire Fan's draw-call budget — never a full re-scan per frame.
- **Reaction-latency false positives/negatives**: the 60° FOV cone is a coarse visibility proxy; it may flag a lucky snap-shot as "fast reaction" or miss one whose target approached from behind. Always shown as evidence with method and caveat, never a verdict.
- **Classification override abuse**: the HUD-attach-point override (§4.3, §6) is priced with a visible penalty, not free, but a careless officer could still misuse it; logging who applied it and when is worth adding later.
- **Family-key false merges/splits**: unchanged — needs an officer-visible split/merge override.
- **Splash-estimate sample scarcity**: weapons whose volleys rarely hit multiple targets at once will sit at "insufficient data" for splash radius even if a splash effect exists — honest, but may frustrate an officer with other reasons to suspect AoE.
- **Unknown-object volume**: unchanged — the Codex should auto-promote items out of Unknown as facts arrive.

## Rejected critiques

- *"The dropped 'Place/moment' noun compounds the Q1 gap, reinstate it as a page."* Rejected as stated: the gap is real and is fixed above, but as a toggleable overlay layer plus a summary panel (Engagement Field / Hot Zones), not a new noun page — a dedicated "Place" destination would reintroduce the page sprawl this design otherwise avoids for no capability the layer doesn't already provide.
- *"Fire-fan/Engagement Field draw-call caps should be tied to a concrete per-frame budget appropriate to the session scale."* Accepted only in part: a stated, disclosed cap is now required (§6), but the exact numeric budget is the engineering plan's tuning responsibility, not this UX/analysis entry's.
