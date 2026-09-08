# Entry 06 — Forensics-First (revised)

## 1. Thesis

Every other feature in this tool is infrastructure for answering one question well: *why did this specific person die, right there, right then?* The **Death Report** is the hero screen — a single case file that reconstructs one death from first principles (who, what, from where, could they see it, was the log lying, was the damage adjusted) with a bounded micro-replay the officer can step frame by frame. Session overview, team detection, and equipment stats are not separate destinations; they are **indexes into Death Reports** — ways to find the case worth opening, and rollups computed by aggregating many solved cases. Everything the officer digs into resolves to "show me the death(s) that prove this," and every death, once opened, offers to widen back out to the pattern it belongs to. Uncertainty is evidence quality, not decoration: every derived number states what it rests on and never claims more precision than its inputs support — including, this revision insists, the number that names a killer, and the assumption that a team is one fixed thing for the whole session.

## 2. Ladder

- **Rung 0 — Concrete: the Micro-Replay.** A single death's window, `[t_death − travel − lag_pad, t_death + 2s]`, played frame-by-frame (0.1 s steps) in a 2D top-down mini-panel (always available) and the full 3D overlay (when stationary + alt-cam). *Up:* "Widen to session" opens the Death Wall filtered to this victim/killer/weapon. *Down:* every abstract view's marks are clickable into the Micro-Replay for the underlying death.
- **Rung 1 — Abstract over time: Overview + Death Wall + Ghost Trails.** The Overview draws every avatar's full-session path as one still picture ("helicopter view"); the Death Wall shows every death as a small-multiple card. *Up:* select a region (team, time band, place) for counts and rates. *Down:* click any trail segment or card into its Death Report at that instant.
- **Rung 2 — Abstract over parameters.** Three sliders: the **LOS filmstrip** (verdict across the travel-time window), the **team confidence threshold** (global — re-clusters live as you drag), and the **adjustment toggle** on a Death Report (damage with/without each armor script). *Up:* pin several parameter settings side by side as small multiples. *Down:* drag to the value that produced a surprising case and jump to it.
- **Rung 3 — Several dimensions, unbent space and time.** The **Front-Distance Lane** re-plots tracks as (time, signed distance from the inferred front) — flanks read as a line crossing zero from the wrong side. The **Team History strip** (§4.4) re-plots team membership itself against time instead of one session-wide fact, so a defector is a color change, not a footnote. *Up:* this is where the officer briefs from. *Down:* any point is a death or moment; click it into the Micro-Replay.

Rungs 1–3 deliberately look different — a trail map, a cluster graph, an axis-transformed chart — and that's fine: Victor's own worked examples (a numeric readout, a drawn path, a parameter-swept family of curves) differ in representation too. What matters, and what the previous draft under-delivered, is that a discovery at one rung *updates* the others instead of leaving them stale. §4.1 and §4.6 fix that by making the team-confidence threshold global state every rung reads.

## 3. Information architecture

Nouns, each a page in the **Case File** floater (one detail pane, many pages, browser-style history):

- **Death** — hero page. Links to: victim Avatar, each candidate-attacker Avatar, the killing Equipment, the rezzer chain, the Team Epoch(s) involved, the LOS Verdict, any Adjustment, the enclosing Moment.
- **Avatar** — dossier: kills/deaths as backlinks, weapons used, mouselook/vehicle time, a **team-history strip** (not a single label), flagged-behavior notes.
- **Equipment** — dossier: every Death/Damage backlink, owner history, classification evidence, through-wall tally, adjustment ratio caused.
- **Team Epoch** — one team's membership *within one time bucket* (default 2 minutes, matching the front-line buckets). A Team page is a stack of its epochs; an avatar can belong to different epochs of different teams across a session instead of being averaged into one.
- **Moment** — a time-window page; mostly reached by scrubbing, occasionally pinned directly.
- **LOS Verdict** — the per-candidate ray table for one Death, its own page because "does this weapon see through walls in general?" pulls up many at once.
- **Adjustment** — one armor/modifier script: which deaths/damage it touched, its net effect.

**Navigation:** breadcrumb + back/forward, like a browser, plus a flat **Recently Visited** list (last 20 pages) — the cheap fix for the risk reviewers named: a linear back/forward stack drops a branch when you dig Death→Avatar→Weapon, back up, and dig a different Death (the first Weapon page falls out of "forward"). A full branching-history tree solves the same problem but is more machinery than a short digging session needs; the flat list recovers anything touched without new navigation UI. A **pin rail** (up to 6 tabs) holds pages open in parallel; each pin owns its own breadcrumb/back-forward stack (stated explicitly, closing an ambiguity reviewers flagged), so pinning "Death #1" before diverging keeps that thread intact. Every page has a **Backlinks** footer.

## 4. Screens

### 4.1 Combat Log (main floater) — Overview tab, default view

```
+-[ Combat Log ]---------------------------------------------------------+
| [Live] [<<][Play][>>] speed:1x   14:32:07 / 01:58:40  [====|--------] |
| Team split confidence: [------o---] 55%   (drives every tab's colors) |
+--------------------------------------------------------------------+
| view: (o) Map lanes   ( ) Front-distance (unbent)                       |
|  dist   ^                                                               |
|  front  |     Ashguard-A ----.__      x <- death (Ashguard-A victim)    |
|  +40m   |....................  \_____/\_____                           |
|    0m===|============front-line=================== conf: MED (§6)     |
|  -40m   |  Ashguard-B __/‾‾\__/‾‾\________  x <- death (B victim)       |
|         +--------------------------------------------------> time      |
|         14:00        14:15        14:30        14:45         15:00     |
|  density (dmg/min): ▁▂▅▇▆▃▂▁▁▃▆▇▅▂▁     team-shift markers: ▲ 14:38    |
+--------------------------------------------------------------------+
| [Overview] [Death Wall] [Avatars] [Equipment] [Teams] [Docket(3)]    |
+--------------------------------------------------------------------+
```
The floater now opens on **Overview**, not Death Wall — the old default made "how did the raid go" require two mode-switches before it answered anything; this puts the orientation view first and the case index one tab over. The **team split confidence slider moves to the main floater's top bar**, shared by every tab: dragging it recolors Death Wall card fills, the front-line, the in-world markers, and the Team pages together, because they all read one global threshold instead of separate snapshots. A ▲ marker on the density strip flags a **team-shift**: a bucket-to-bucket change in an avatar's dominant cluster (§4.6).

### 4.2 Combat Log — Death Wall tab (rung 1 case index)

```
+--------------------------------------------------------------------+
| DEATH WALL  (each card = one death, click = open Case File)         |
|  ___________  ___________  ___________  ___________                |
| | 14:02:11  || 14:05:44  || 14:07:02  || 14:09:55  |                |
| |  V:Kade   ||  V:Mara   ||  V:Rill   ||  V:Kade   |                |
| |  A:Finn?  ||  A:2 cand ||  A:Orr    ||  A:UNK    |                |
| |  [rifle]  ||  [shotgn] ||  [turret] ||  [??]     |                |
| |  LOS:CLR  ||  LOS:BLK! ||  LOS:CLR  ||  LOS:???  |                |
| |__conf:Hi__||__conf:Med_||__conf:Hi__||__conf:Low_|                |
+--------------------------------------------------------------------+
```
Card fill = team at the global confidence threshold *and time bucket of the death*, not a session-wide label — the propagation fix: move the slider and every card recolors against its own moment, never a value baked in once at clustering time. Red "!" marks a BLOCKED-but-fatal LOS verdict or a flagged card.

### 4.3 Case File — Death Report page (the hero screen)

```
+-[ Case File ]------------------------------------------[pin][x]------+
| < Ashguard-B  <  SS Pulse Rifle mk2  <  Death 14:32:07                |
+------------------------------------------------------------------+
| DEATH: Nyx Ashworth  @ 14:32:07.4        confidence: MEDIUM      |
| damage in lethal window: Finn 53 (dominant) | Orr 38 (contested)|
|   | unattributed 6 (UNK)     [see raw ledger below]              |
| weapon: SS Pulse Rifle mk2 (handheld) -> bolt-9f2c (projectile)   |
+------------------------------------------------------------------+
| MICRO-REPLAY  [<<][<][ || ][>][>>]  t = -0.6s  window[-1.5s..+1.0s]|
|  .--------------------------------------.   [Show in world] (needs|
|  | N     F----->x   . . (blocked-ray     |    stationary+altcam;  |
|  |   . .  . .        hit points, this    |    currently: walking) |
|  |         area, all-time)               |                        |
|  '--------------------------------------'                         |
+------------------------------------------------------------------+
| DAMAGE LEDGER (this death's window)      | LOS FILMSTRIP (rung 2) |
| t-1.9s dmg 12 (init 20) Finn  [rifle]    | -1.5s [BLK][BLK][UNK]  |
| t-0.6s dmg 41 (init 41) Finn  [rifle] *  | -1.0s [BLK][CLR]       |
| t-0.1s dmg 38 (init 60) Orr   [turret]   | -0.5s [CLR][CLR][CLR]  |
|  * = killing blow   totals: Finn 53 dmg, Orr 38 dmg               |
+------------------------------------------------------------------+
| ADJUSTMENTS: "Ashguard Plate v3" script reduced Orr's hit 60->38  |
|   (-37%). Finn's hits unmodified (no armor tag detected on Nyx). |
+------------------------------------------------------------------+
| QUALITY: track=BRIDGE(2Hz) for Finn, VIEWER(8Hz) for Nyx;         |
|  log lag: ~1s combat-log estimate (not fd-confirmed — Nyx is not |
|  the reviewing officer's avatar); LOS window 5/6 covered -> MEDIUM|
+------------------------------------------------------------------+
| [Track Finn] [Track Nyx] [Open weapon] [Flag] [Add to briefing]  |
+------------------------------------------------------------------+
```
**Attribution fix (both reviews):** no more manufactured percentages — raw damage totals, ranked, tagged **dominant** (>2x runner-up) / **contested** (within 2x) / **long-shot** (<15%), auditable against the numbers beside them instead of an opaque 72/18/10 split from an unstated formula. The weighting (§6) still exists internally to pick the killing-blow highlight, never shown as a probability.

**Micro-replay fix (feasibility fatal #3):** no invented wall silhouette — no wall-geometry store exists anywhere in the plan, only a boolean raycast per LOS test. The panel plots the **occlusion point cloud** instead: every BLOCKED raycast near this death leaves its already-computed hit point as a dim dot. Chokepoints accrete a recognizable scatter for free; untested ground stays honestly sparse.

**fd-confirmed fix (feasibility serious #4):** Quality states plainly that `final_damage`-confirmed lag exists only for the reviewing officer's own deaths; for everyone else, including this example, it's the generic ≤1 s estimate, never sub-second-precise.

### 4.4 Case File — Avatar Dossier page

```
+-[ Case File ]------------------------------------------[pin][x]------+
| < Nyx Ashworth                                                        |
+------------------------------------------------------------------+
| K:4  D:7  dmg 1120/2004   mouselook: 22%   seated(vehicle): 6 min |
| flagged: no        reaction check: no anomaly in 6/7 deaths,      |
|                     insufficient track density in 1/7 (14:41:19)  |
+------------------------------------------------------------------+
| TEAM HISTORY (bucketed, follows the global confidence slider)      |
| 14:00-14:20 Ashguard-B (81%) | 14:20-14:32 Ashguard-B (77%) |      |
| 14:32-14:50 Ashguard-B (62%) | 14:50-15:00 UNCLEAR (splits A/B)   |
+------------------------------------------------------------------+
| ACTIVITY STRIP (whole session, scrub-linked)                       |
| |‾‾‾‾\__/‾‾\_____/‾‾‾‾‾‾‾\___|  <- speed          x  x    x   x    |
+------------------------------------------------------------------+
| WEAPONS USED         | DEATHS (backlinks)  | KILLS (backlinks)    |
| SS Pulse Rifle x812  | 14:32:07 (this)     | 13:58:02 -> Orr Vex  |
+------------------------------------------------------------------+
```
**Team drift fix (comprehension serious):** the static "team: X (conf %)" line is gone, replaced by the sequence of Team Epochs this avatar belonged to — a mid-raid defector reads as changing labels, not a blended or discarded fact, satisfying the brief's "team membership can change during a session" requirement by reusing the front-line's existing 2-minute bucketing.

**Reaction-flag fix (feasibility serious #5):** the header distinguishes "checked, nothing anomalous" from "insufficient track density to assess," so a coarse-tracked avatar never silently reads as cleared.

### 4.5 Case File — Equipment Dossier page

```
+-[ Case File ]------------------------------------------[pin][x]------+
| < SS Pulse Rifle mk2  (handheld, evidence: high)                      |
+------------------------------------------------------------------+
| carried by: Finn Drask, Orr Vex, Tamsin Reyes (3)                  |
| kills:14  hits:203  avg dmg:19.6  through-wall suspicion: 2/14 (14%)|
+------------------------------------------------------------------+
| SUSPICION LIST (deaths where LOS was BLOCKED at the killing blow)  |
|  14:32:07 Nyx Ashworth  LOS: 5/6 BLOCKED, 1 UNKNOWN  [open]        |
|  caveat: travel-time + lag can fake this; treat as leads, not proof|
|  LOS candidate positions sourced from: owner track (handheld).     |
|  Deployables/mounts/vehicles use the firing object's own track —   |
|  see §6.                                                            |
+------------------------------------------------------------------+
```
The position-sourcing note exists because it is *not* uniform across equipment kinds — see §6.

### 4.6 Case File — Team Ledger page (rung 2 detail behind the global slider)

```
+-[ Case File ]------------------------------------------[pin][x]------+
| Teams — bucket @ cursor: 14:30-14:32   [Whole-session view]          |
| confidence threshold: [------o---] 55%  (same control as top bar)   |
+------------------------------------------------------------------+
| Ashguard-A (11)      Ashguard-B (9)      Unclustered (3)            |
|  Finn D.  92%          Nyx A.  81%         Vex K. 48% (splits A/B)  |
+------------------------------------------------------------------+
| FRIENDLY FIRE (this bucket): 1 event                                |
|  14:31 Tamsin -> Finn  (18 dmg, same team @ this bucket: 92%/88%)   |
|  note: bucket 14:36-14:38 disagrees (Tamsin reads UNCLEAR there) —  |
|  this FF flag is bucket-local, not a session-wide fact.             |
+------------------------------------------------------------------+
| graph: nodes=avatars (positions fixed once, from full-session       |
| pairing data — the slider recolors nodes, it never re-lays-out the  |
| graph, so dragging it doesn't jitter); edge width=damage exchanged  |
| pin [40%] [55%] [70%] side by side to compare thresholds at once    |
+------------------------------------------------------------------+
```
**Propagation + drift fix, combined:** the threshold slider is the same control shown in the top bar — one live value, read everywhere. The page defaults to **the bucket at the current scrub cursor**, not a session-wide blend, with a toggle to "Whole-session view." Friendly-fire flags are per-bucket facts with per-bucket confidence, and a flag that doesn't hold in the neighboring bucket says so. The node graph is stated to be layout-stable across drags (only color changes), closing the jitter risk reviewers flagged. "Pin thresholds side by side" is the rung-2 "step up" the Ladder promises but the original entry never gave a home — it lives here.

## 5. In-world overlay

- **Rung 0 (Micro-Replay in world):** victim marker (hollow ring, fills as t approaches death), each candidate attacker as a labeled arrow-head, the projectile path pulsing once at impact, LOS rays colored per the filmstrip (green CLEAR, red BLOCKED, grey dashed UNKNOWN) fanned from every sampled candidate position. Candidate positions follow §6's equipment-aware rule, not always the owner avatar.
- **Rung 1 (Ghost Trails), decluttered by default:** the previous draft drew every avatar's full-session trail plus persistent death crosses at once — for 20-30 avatars that's unreadable soup, and desaturating low-confidence avatars toward grey made a crowded FFA worse. This revision defaults to **Focus mode**: trails only for the avatar(s)/team currently selected, plus anyone within one team-hop of a pinned Death. A "Show all trails" toggle covers the deliberate helicopter-view moment, with distance-based alpha falloff (trails beyond ~60 m fade toward a floor alpha) so pulling back doesn't cost legibility up close. Death crosses always persist and stay clickable.
- **Rung 2 (LOS fan / confidence recolor):** the LOS fan swept across the whole travel window at once. Confidence recoloring follows the same global slider as every 2D view, so one drag recolors in-world markers, the Death Wall, and the front-line together.
- **Clicking:** any marker opens/updates the Case File to that noun — the world and the floater are one navigation surface with two renderers.
- **Uncertainty encoding (kept — both reviews name this the entry's strongest element):** UNKNOWN track gaps draw dashed and thin with a "?" glyph; COARSE-quality samples draw as a wide translucent band; LOS BLOCKED draws red *only* when at least one dense (VIEWER-quality) sample was tested — otherwise grey regardless of the raycast result, because the underlying position wasn't trustworthy enough to raycast from.

## 6. Analysis model

- **Attribution shares (revised):** for a Death at `t`, gather DAMAGE to the target in `[t−W, t]` (`W = SSCombatLogDeathWindowSeconds`), group by `(owner, rezzer)`, report the **raw per-group damage total and hit count**, ranked, with a killing-blow marker. A composite score (damage share / recency / killing-blow bonus, weighted 0.6/0.25/0.15 — stated here, not hidden) sorts ties and classifies each group dominant (>2x runner-up)/contested (within 2x)/long-shot (<15%); never shown as a percentage, since the weighting is a heuristic, not a calibrated model.
- **LOS candidate positions (revised, feasibility fatal #2):** position source branches by the equipment classification the tool already computes. Handheld: attacker's own `sampleAt(owner, t_k)`. Deployable (turret/trap): the deployable object's own track (`sampleAt(source, t_k)` — the equipment registry extends its existing bridge-query path to carry position samples for these, stationary/slow enough not to need avatar-track fidelity). Mount/vehicle-mounted weapon: the mount/vehicle's own track, not the seated owner's nominal position (SL offsets a seated avatar into the seat; that isn't where the shot originates). No track source → UNKNOWN, never silently dropped — closing the gap where turrets and traps, the paradigm through-wall case, previously had no computation path.
- **Occlusion point cloud (new, replaces the invented wall schematic):** every LOS raycast resolving BLOCKED records its hit point (already returned by `lineSegmentIntersectWorldGeometry`) into a session-scoped spatial index; the micro-replay panel queries nearby points as a sparse scatter — a real, incomplete trace of tested geometry, never a solid wall the tool has no data for.
- **Reconstruction confidence, with numeric bands:** composite of attacker-share concentration, track quality, LOS coverage, and log lag. HIGH = single dominant attacker AND ≥80% LOS coverage AND both tracks VIEWER/BRIDGE; LOW = any of (long-shot-only attribution, <40% LOS coverage, a COARSE/absent track); MEDIUM = between. Reported as band plus contributing factors, never a bare score.
- **Adjustment ratio:** `damage / initial` per DAMAGE event with `modifications`, rolled up per script and per avatar/team as mean reduction % paired with a sample count.
- **Through-wall suspicion rate:** per equipment, `BLOCKED-at-killing-blow / total kills`, with the caveat text, a link to every case, and a note of which position-sourcing rule applied — a turret's BLOCKED verdict rests on better position data than a mount's.
- **Front-line estimate, with composite confidence (revised, feasibility serious #6):** per 2-minute bucket, median position of avatar-pairs whose *same-bucket* clustering marks opposing (clustering is now itself bucketed, so this no longer mixes a session-wide label with a windowed position estimate). Confidence = LOW if bucket team-cluster confidence is low OR fewer than 3 opposing pairs; else MEDIUM/HIGH by pair count and track quality — shown inline, so a wobbly line reads as "team assignment was shaky," not "fighting was diffuse."
- **Team clustering, bucketed (revised, comprehension serious):** label-propagation clustering runs independently per 2-minute bucket, using only that bucket's damage-exchange pairs. An avatar's Team History is the sequence of per-bucket assignments at the current global threshold. Friendly-fire flags are per-bucket facts, noting disagreement with adjacent buckets instead of one session-wide verdict. The confidence slider is single global state (top bar, Team Ledger, Death Wall, overlay all read it); dragging it recomputes membership for the relevant buckets live — cheap at raid scale (tens to ~100 avatars/bucket).
- **Reaction/awareness flag (Q8), with an explicit no-data state (revised, feasibility serious #5):** compare a victim's post-hit yaw-rate/velocity-change against the session's population distribution, and whether facing tracked the attacker's true bearing before any LOS candidate was CLEAR. No heading samples in the window (coarse-only tracking) → **"insufficient track density to assess,"** distinct from "checked, no anomaly," so a tracking lapse is never misread as exculpatory.

## 7. Walkthroughs

**Q1 — how did the raid go?** Combat Log opens on **Overview** (no tab switch needed). The lane shows three pushes between 14:10 and 14:50 and a density spike at 14:32; the front-line carries an inline MED/LOW confidence band, so a flat stretch around 14:47 reads as "team assignment was uncertain there," not "nothing happened." The officer nudges the **top-bar confidence slider** from 55% to 65% and watches the front-line firm up mid-raid while the shaky 14:47 stretch stays unclear — a genuine rung-2 stop, not a bypassed one. Switch to Death Wall (recolored live at 65%), scan the 14:32 cluster, click the red "!" card.

**Q3 — why did X die at 14:32?** Lands on the Death Report. Ranked damage totals (Finn 53, dominant; Orr 38, contested; 6 unattributed), scrub to −0.6 s, LOS filmstrip (blocked until −0.5 s, then clear for three samples), Adjustments (Orr's hit reduced by Ashguard Plate v3, Finn's wasn't), Quality (MEDIUM — Finn's track was 2 Hz bridge-quality, log lag the generic ≤1 s estimate since Nyx isn't the reviewing officer). Answer: mostly Finn, brief LOS window, medium confidence.

**Q8 — suspicious behavior?** An Equipment Dossier's suspicion list opens a Death Report showing LOS "4/4 BLOCKED," all from the shooter's own handheld track (strong sourcing, not a turret/mount case), with Quality VIEWER-grade for all four candidates — a strong case, not just a lead. The victim's Avatar Dossier reaction check reads "outside 95th percentile reaction latency," not "insufficient track density," so it's a real signal. Language stays "evidence of a wallhack-consistent hit; population-outlier reaction latency," never "confirmed cheater." Added to the Docket; the officer decides.

**Intra-group skirmish (two Ashguard teams).** The active group tag seeds nothing. Dragging the confidence slider from 55% to 30% splits the node graph into two dense clusters; because clustering is bucketed, the Team History strip shows exactly which buckets the split holds up in. FF flags update live and each states its bucket and threshold ("flagged at 55% in bucket 14:20-14:22; splits as opposing at 30%").

**FFA (Bad Space), including a defector.** Team Ledger shows many small, low-confidence clusters. One avatar's Team History reads Ashguard-A for 40 minutes, then UNCLEAR, then a stable low-confidence cluster with a different group for the last 20 — a defection visible as a color change, not lost in an average. Death Wall filtered to "Unclustered" becomes primary; the officer works avatar-by-avatar via Dossiers rather than trusting team aggregates.

## 8. Risks and open questions

- **Ranked-share attribution can still feel unsatisfying** to an officer wanting a name for a citation; resist collapsing "dominant/contested" back into a false single percentage.
- **Reaction/awareness flags remain the most misuse-prone feature**; the explicit "insufficient track density" state removes one failure mode, but wording discipline still has to hold everywhere.
- **Bucketed clustering adds a tunable parameter** (bucket width, reusing the front-line's 2-minute default) — too narrow and short skirmishes never accumulate enough pairs to cluster; too wide and fast defections smear across a boundary.
- **Occlusion point cloud is only as good as nearby LOS testing**; a death in an untested area shows a near-empty panel, which is correct but needs a visual distinction from "confirmed open ground."
- **Docket workflow** (flag → review → briefing export) stays intentionally unspecified, as before — a real gap, but forcing an export design into an already-full Death Report now would crowd out the mechanism this entry is about; it deserves its own pass once the Death Report is validated.
