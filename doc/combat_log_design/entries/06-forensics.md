# Entry 06 — Forensics-First

## 1. Thesis

Every other feature in this tool is infrastructure for answering one question well: *why did this specific person die, right there, right then?* The **Death Report** is the hero screen — a single case file that reconstructs one death from first principles (who, what, from where, could they see it, was the log lying, was the damage adjusted) with a bounded micro-replay the officer can step frame by frame. Session overview, team detection, and equipment stats are not separate destinations; they are **indexes into Death Reports** — ways to find the case worth opening, and rollups computed by aggregating many solved cases. The Death Wall (a wall of case cards, not a log of rows) is the front door. Everything the officer digs into eventually resolves to "show me the death(s) that prove this," and every death, once opened, offers to widen back out to the pattern it belongs to. Uncertainty is treated as evidence quality, not noise to hide: every case file states its own confidence and shows exactly which inputs (track density, LOS window coverage, log lag) it rests on, because a promotion or a violation call made on a shaky reconstruction is worse than no reconstruction at all.

## 2. Ladder

- **Rung 0 — Concrete: the Micro-Replay.** A single death's window, `[t_death − travel − lag_pad, t_death + 2s]`, played frame-by-frame (0.1 s steps) in both a 2D top-down mini-panel (always available) and the full 3D world overlay (when stationary + alt-cam). *Step up:* click "Widen to session" to jump the main timeline to this moment and open the Death Wall filtered to this victim/killer/weapon. *Step down (from anywhere):* every abstract view's marks are clickable and open the Micro-Replay for the underlying death.
- **Rung 1 — Abstract over time: Death Wall + Ghost Trails.** The Death Wall shows every death of the session as one small-multiple card; Ghost Trails (Timeline tab, and an in-world toggle) draws every avatar's full-session path as one static picture. *Step up:* select a region of the wall/trails (a team, a time band, a place) to get counts and rates (rung 2). *Step down:* click any one card or any one trail segment to open its Death Report / Micro-Replay at that instant.
- **Rung 2 — Abstract over parameters.** Three sliders reveal behavior across a variable instead of a single value: the **LOS filmstrip** (verdict across the whole travel-time window, not just at `t`), the **confidence threshold slider** on the Team Ledger (re-clusters live as you drag), and the **adjustment toggle** on a Death Report (damage with/without each armor script). *Step up:* pin several parameter settings side by side (e.g. three thresholds) as small multiples to see which one is stable. *Step down:* drag the slider to the value that produced a surprising case and jump straight to it.
- **Rung 3 — Several dimensions, unbent space.** The Session Overview's **Front-Distance Lane** re-plots every avatar's track as (time, signed distance from the inferred front line) instead of (x, y) — flanks and backstabs become visible as a line crossing zero from the "wrong" side. Small multiples of Death Wall cards encode team (color), weapon class (glyph), and confidence (border) simultaneously. *Step up:* this is closest to "how did the raid go" — the officer stays here to brief. *Step down:* any point on the lane is a death or a moment; click it to drop straight into the Micro-Replay.

## 3. Information architecture

Nouns, each a page rendered inside the **Case File** floater (one detail pane, many pages, browser-style history):

- **Death** — the hero page. Links to: victim Avatar, each candidate-attacker Avatar, the killing Equipment, the rezzer chain, the Team(s) involved, the LOS Verdict, any Adjustment scripts, and the enclosing Moment.
- **Avatar** — dossier: kills/deaths as victim/killer backlinks, weapons used, mouselook/vehicle time, team history, flagged-behavior notes.
- **Equipment** — dossier: every Death/Damage backlink it appears in, owner history, classification evidence, through-wall tally, adjustment ratio it causes (if it's armor).
- **Team** — ledger: membership with confidence, friendly-fire backlinks, aggregate stats, the confidence-threshold slider.
- **Moment** — a time-window page (e.g. "14:31:50–14:32:10"): every event, every avatar position, in that slice; mostly reached by scrubbing, occasionally pinned directly.
- **LOS Verdict** — the per-candidate ray table for one Death, promoted to its own page because it's dug into independently ("does this weapon see through walls in general?" pulls up many LOS Verdict pages at once).
- **Adjustment** — one armor/damage-modifier script: which deaths/damage it touched, its net effect.

**Navigation model:** the Case File floater has a breadcrumb strip (`Death 14:32:07 › Nyx Ashworth › SS Pulse Rifle mk2 › Ashguard-B`) and back/forward arrows, like a browser. A **pin rail** along the floater's left edge holds up to 6 pinned pages as small tabs, so an officer building a violation report can keep "Death #1", "Death #2", and the suspect's Avatar dossier open simultaneously without losing the digging thread. Every page has a **Backlinks** footer ("Referenced by: 3 deaths, 1 friendly-fire flag") so digging is bidirectional, not just forward.

## 4. Screens

### 4.1 Combat Log (main floater) — Death Wall tab, default view

```
+-[ Combat Log ]---------------------------------------------------------+
| [Live] [<<][Play][>>] speed:1x   14:32:07 / 01:58:40  [====|--------] |
| Filter: Team[All v] Weapon[All v] Confidence[>=Low v] [x]Flagged only |
+--------------------------------------------------------------------+
| DEATH WALL  (each card = one death, click = open Case File)         |
|  ___________  ___________  ___________  ___________                |
| | 14:02:11  || 14:05:44  || 14:07:02  || 14:09:55  |                |
| |  V:Kade   ||  V:Mara   ||  V:Rill   ||  V:Kade   |                |
| |  A:Finn?  ||  A:2 cand ||  A:Orr    ||  A:UNK    |                |
| |  [rifle]  ||  [shotgn] ||  [turret] ||  [??]     |                |
| |  LOS:CLR  ||  LOS:BLK! ||  LOS:CLR  ||  LOS:???  |                |
| |__conf:Hi__||__conf:Med_||__conf:Hi__||__conf:Low_|                |
|  ___________  ___________  ___________  ___________                |
| | 14:11:30  || 14:14:02  || 14:15:19  || ...       |                |
|  (border color = confidence, fill = team, corner glyph = weapon class)|
+--------------------------------------------------------------------+
| [Timeline] [Death Wall] [Avatars] [Equipment] [Teams] [Docket(3)]    |
+--------------------------------------------------------------------+
```
Top bar: transport + session scrub (shared by every tab). Card grid: a custom `LLView` drawing fixed-size cards (rung-1 small multiples); a red "!" badge marks a BLOCKED-but-fatal LOS verdict or a flagged card. Bottom row switches tabs; **Docket** is a working queue of cards an officer flagged mid-review ("come back to this one").

### 4.2 Combat Log — Timeline / Overview tab (rung 1 → rung 3)

```
+-[ Combat Log : Overview ]-----------------------------------------------+
| view: (o) Map lanes   ( ) Front-distance (unbent)                       |
|                                                                          |
|  dist   ^                                                               |
|  front  |     Ashguard-A ----.__      x <- death (Ashguard-A victim)    |
|  +40m   |....................  \_____/\_____                           |
|    0m===|============front-line===================  <- inferred front |
|  -40m   |  Ashguard-B __/‾‾\__/‾‾\________  x <- death (B victim)       |
|         +--------------------------------------------------> time      |
|         14:00        14:15        14:30        14:45         15:00     |
|  density band (damage events/min): ▁▂▅▇▆▃▂▁▁▃▆▇▅▂▁                     |
+--------------------------------------------------------------------+
```
Map-lanes mode is a plain top-down still of all trails (rung 1, "helicopter view"). Front-distance mode is the rung-3 unbending transform: y = signed distance from the session's inferred front centroid (median position of simultaneous opposed damage events, recomputed per 2-minute bucket). A line crossing from negative to positive mid-fight without a matching push is a flag for "someone went around" — click the crossing point to open that Moment.

### 4.3 Case File — Death Report page (the hero screen)

```
+-[ Case File ]------------------------------------------[pin][x]------+
| < Ashguard-B  <  SS Pulse Rifle mk2  <  Death 14:32:07                |
+------------------------------------------------------------------+
| DEATH: Nyx Ashworth  @ 14:32:07.4        confidence: MEDIUM      |
| killed by: Finn Drask (72%) | 2nd candidate: Orr Vex (18%) |UNK 10%|
| weapon: SS Pulse Rifle mk2 (handheld) -> bolt-9f2c (projectile)   |
+------------------------------------------------------------------+
| MICRO-REPLAY  [<<][<][ || ][>][>>]  t = -0.6s  window[-1.5s..+1.0s]|
|  .--------------------------------------.   [Show in world] (needs|
|  | N            F----->x  (bolt path)   |    stationary+altcam;   |
|  |     wall█████                        |    currently: walking)  |
|  |         (top-down, region-space)     |                         |
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
| QUALITY: track=BRIDGE(2Hz) for Finn, VIEWER(8Hz) for Nyx; log lag |
|  0.2s (fd-confirmed); LOS window covered 5/6 samples. -> MEDIUM   |
+------------------------------------------------------------------+
| [Track Finn] [Track Nyx] [Open weapon] [Flag] [Add to briefing]  |
+------------------------------------------------------------------+
```
Header: breadcrumb + confidence badge. Attribution line: candidate attackers ranked by share of lethal-window damage, shown as *probabilities that sum near 100%*, not a single name — this is the honesty mechanism for ambiguous kills (two people shooting the same target in the same second). Micro-replay: bounded scrub, independent of the session-wide transport bar; a top-down 2D panel always renders (custom `LLView`, primitives only); the "Show in world" button is greyed with a live reason string when overlay gating fails, so the officer knows *why* they can't get the 3D view instead of it silently not appearing. LOS filmstrip: one column per sampled candidate time inside the travel window, each cell a small colored swatch (CLEAR/BLOCKED/UNKNOWN) — this is rung 2 made literal on the hero screen, because "could they see them" is never a single boolean. Adjustments and Quality panels make the two biggest sources of doubt (armor scripts, track/log fidelity) first-class instead of buried.

### 4.4 Case File — Avatar Dossier page

```
+-[ Case File ]------------------------------------------[pin][x]------+
| < Team Ashguard-B  <  Nyx Ashworth                                    |
+------------------------------------------------------------------+
| Nyx Ashworth   team: Ashguard-B (conf 81%)   K:4  D:7  dmg 1120/2004|
| mouselook: 22% of session   seated(vehicle): 6 min   flagged: no   |
+------------------------------------------------------------------+
| ACTIVITY STRIP (whole session, scrub-linked)                       |
| |‾‾‾‾\__/‾‾\_____/‾‾‾‾‾‾‾\___|  <- speed          x  x    x   x    |
| moving  still  mlook  vehicle  (color bands)      deaths (click)   |
+------------------------------------------------------------------+
| WEAPONS USED         | DEATHS (backlinks)  | KILLS (backlinks)    |
| SS Pulse Rifle x812  | 14:32:07 (this)     | 13:58:02 -> Orr Vex  |
| combat knife x2      | 14:41:19            | 14:20:44 -> Finn D.  |
+------------------------------------------------------------------+
```
Activity strip is a decimated render of the avatar's flags/velocity track for the whole session (rung 1, one avatar's helicopter view); death/kill ticks are clickable straight into Death Reports.

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
|  14:51:40 Rill Vance    LOS: 4/4 BLOCKED             [open]        |
|  caveat: travel-time + lag can fake this; treat as leads, not proof|
+------------------------------------------------------------------+
```
The through-wall claim (officer question 4) lives here as a *rate with a link to every underlying case*, not a verdict — the officer is expected to open the suspicion list and read the actual LOS filmstrips before believing it.

### 4.6 Case File — Team Ledger page (rung 2)

```
+-[ Case File ]------------------------------------------[pin][x]------+
| Teams (inferred)              confidence threshold: [----o----] 60%  |
+------------------------------------------------------------------+
| Ashguard-A (11)      Ashguard-B (9)      Unclustered (3)            |
|  Finn D.  92%          Nyx A.  81%         Vex K. 48% (splits A/B)  |
|  Tamsin R. 88%         Rill V. 77%                                  |
+------------------------------------------------------------------+
| FRIENDLY FIRE (this session): 3 events, 1 death                    |
|  14:22 Tamsin -> Finn  (18 dmg, same team 92%/88%)     [open]       |
+------------------------------------------------------------------+
| graph: nodes=avatars, edge width=damage exchanged, color=cluster   |
| dragging the slider recolors nodes live; drop below 40% to see the |
| intra-group-skirmish case (both sides share one active group tag)  |
+------------------------------------------------------------------+
```
The slider is the parameter rung made interactive: dragging it re-runs the clustering threshold live and repaints the node graph, so the officer *feels* where the algorithm is confident versus guessing, per Bret Victor's "draw a slider, watch it respond."

## 5. In-world overlay

- **Rung 0 (Micro-Replay in world):** when a Death Report is open and the officer is stationary + alt-camming, its window draws in full 3D: victim marker (hollow ring, fills in as t approaches death), each candidate attacker as a labeled arrow-head colored by attribution probability (opacity = probability), the projectile path as a bright line that pulses once at impact, LOS rays as thin lines colored per the filmstrip (green CLEAR, red BLOCKED, grey dashed UNKNOWN) fanned from every sampled candidate position to the victim.
- **Rung 1 (Ghost Trails toggle):** every avatar's full-session trail as a faint colored polyline (team color, alpha ramps by recency if a time cursor is set, flat if "whole session" is chosen); death markers persist as small crosses along the trail regardless of cursor position, always clickable.
- **Rung 2 (LOS fan / confidence recolor):** the same LOS fan as rung 0 but swept across the whole travel window simultaneously (all candidate rays visible at once, not just the one at cursor time) — this is the in-world version of the filmstrip. Team confidence recoloring: when the Team Ledger's slider is being dragged, all in-world avatar markers recolor live at whatever alpha equals their membership confidence, so low-confidence avatars visibly desaturate toward grey.
- **Clicking:** any marker (avatar, death cross, attacker arrow, LOS ray endpoint) opens/updates the Case File to that noun, exactly like clicking its 2D counterpart — the world and the floater are one navigation surface with two renderers.
- **Uncertainty encoding:** UNKNOWN track gaps draw dashed and thin with a "?" glyph at the gap; COARSE-quality samples draw as a wide translucent band instead of a crisp line (position could be anywhere in the band); LOS BLOCKED draws red *only* when at least one dense (VIEWER-quality) sample was tested — if all candidates were coarse, the ray draws grey regardless of the raycast result, because the underlying position wasn't trustworthy enough to raycast from in the first place.

## 6. Analysis model

- **Attribution shares** — for a Death at `t`, gather DAMAGE to the same target in `[t−W, t]` (`W` = `SSCombatLogDeathWindowSeconds`), group by `(owner, rezzer)`, weight each group by (damage contributed, recency, whether it's the killing blow); normalize to probabilities. A single dominant group → HIGH confidence single attacker; multiple comparable groups → split probabilities shown as ranked list, never collapsed to one name.
- **LOS filmstrip** — candidate times `t_k = t − k·Δ` for `k=0..N` over `SSCombatLogTravelSeconds` (Δ≈0.1s); candidate position = attacker's `sampleAt(owner, t_k)` (fallback to `source_pos` on the DEATH record when `k=0`); ray from candidate eye height to victim's `sampleAt(target, t)` chest height via `lineSegmentIntersectWorldGeometry`. Cell verdict: CLEAR/BLOCKED/UNKNOWN (no sample within tolerance). Death-level verdict is a *summary sentence*, not a single letter: "clear for the last 0.5 s of the window" / "blocked at every sampled position; only 2/6 positions had reliable tracks."
- **Reconstruction confidence** — composite of: attacker-share concentration (one clear winner vs. a split), track quality of both parties at the relevant instants (VIEWER > BRIDGE > COARSE > none), LOS sample coverage (samples-with-track / N), and log lag (small when `fd`-confirmed, larger/flagged when only the delayed combat-log copy exists). Reported as HIGH/MEDIUM/LOW plus the contributing factors listed, never a bare score.
- **Adjustment ratio** — `damage / initial` per DAMAGE event with `modifications`; rolled up per script (`task_id`) as mean reduction % and per avatar/team as "average incoming damage reduction," always paired with a sample count so "3 hits" doesn't read like "300 hits."
- **Through-wall suspicion rate** — per equipment, `BLOCKED-at-killing-blow deaths / total kills`, always rendered with the caveat text and a link to every contributing Death Report; never presented context-free.
- **Front-line estimate** — per 2-minute bucket, median position of avatar-pairs exchanging damage that session-clustering marks as opposing teams; used only for the unbent lane view, explicitly labeled "inferred, smoothed" since it can wobble with few simultaneous fights.
- **Reaction/awareness flag (Q8)** — for a victim's first DAMAGE in an engagement, compare their yaw-rate and velocity-change in the following 0.5–1.5 s against the session's own population distribution for "just took damage"; also check whether their facing began tracking the attacker's true bearing *before* any LOS candidate was CLEAR. Both are shown as "outside the Nth percentile" / "faced attacker before any clear LOS sample," explicitly labeled evidence, not a cheat verdict, and always linked to the underlying Death/Moment.
- **Team confidence** — label-propagation clustering (as in the shared plan) exposed with a threshold slider; per-avatar confidence = cluster-assignment margin, redrawn live as the slider moves.

## 7. Walkthroughs

**Q1 — how did the raid go?** Open Combat Log → Timeline tab → Front-distance lane. See three pushes (line crossing zero and back) between 14:10 and 14:50, and a density spike at 14:32. Switch to Death Wall, sort by time, scan the cluster of cards around 14:32 — mixed team colors, one red "!" card. Click it.

**Q3 — why did X die at 14:32?** Lands directly on the Death Report from Q1 (or search Avatar "Nyx Ashworth" → Deaths backlink → 14:32:07). Read the attribution line (Finn 72%, Orr 18%, UNK 10%), scrub the micro-replay to −0.6 s to watch the killing bolt travel, open the LOS filmstrip (blocked until −0.5 s, then clear for three samples), check Adjustments (Orr's hit was reduced by Ashguard Plate v3, Finn's wasn't — Nyx had no armor tag), read Quality (MEDIUM, because Finn's track was only 2 Hz bridge-quality at the critical instant). Answer: mostly Finn, from a position with a brief LOS window, medium confidence because of Finn's coarser track.

**Q8 — suspicious behavior?** From an Equipment Dossier's suspicion list (or a Docket flag left by a duty officer), open a Death Report showing LOS "4/4 BLOCKED." Check Quality first — if track quality was VIEWER-grade for all four candidates, the case is strong; if it says "2/4 UNKNOWN," it's a lead, not evidence. Open the victim's Avatar Dossier, check the Reaction flag on their activity strip — if it shows "faced attacker before any clear LOS sample" at that same timestamp, note it, but the report language stays "evidence of a wallhack-consistent hit; population-outlier reaction latency" — never "confirmed cheater." Add both Death Reports to the Docket with a note; the officer, not the tool, decides.

**Intra-group skirmish (two Ashguard teams).** Active group tag is Ashguard for everyone, so the Team Ledger seeds nothing from group data; drag the confidence slider down from 60% toward 30% and watch the node graph split into two dense clusters connected by thin cross-cluster edges (the friendly-fire-shaped exchanges are actually cross-team damage the group tag mislabels as FF). The FRIENDLY FIRE panel is misleading until the slider is dropped — the design surfaces this by always showing the current threshold next to every FF claim ("flagged as friendly fire at 60% confidence; team split at 35% resolves this as opposing sides").

**FFA (Bad Space).** Team Ledger shows many small, low-confidence clusters or a warning "clustering unstable: too few repeated pairings." The tool falls back to per-avatar view: Death Wall filtered to "Unclustered," each card's attribution line becomes the primary signal since team color-coding is unreliable. Officer works avatar-by-avatar via Avatar Dossiers rather than trusting team aggregates.

## 8. Risks and open questions

- **Attribution splits can feel unsatisfying** to an officer wanting a name for a citation; resist collapsing ties into a false single answer under pressure to look decisive.
- **Reaction/awareness flags are the most misuse-prone feature** — wording must stay evidence-first everywhere or it reads as an accusation engine.
- **Front-line estimate is fragile in FFA/low-density fights**; needs a visible "unstable" state, not a wobbly authoritative-looking line.
- **Micro-replay/world overlay parity** (same colors, same LOS fan in both renderers) is ongoing work, not a one-time build.
- **Docket workflow** (flag → review → briefing export) is sketched, not fully specified; needs its own pass once the Death Report is validated against real sessions.
