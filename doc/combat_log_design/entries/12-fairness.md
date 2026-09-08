# Entry 12 — Behaviour-Audit-First: Combat Log as a Fair-Play Instrument

## 1. Thesis

Every existing kill-feed or damage-meter answers "who won." This entry answers a harder and more dangerous question: "was it won fairly?" — and the central design problem is that this question is trivially abused. A tool that outputs a "cheat score" turns an officer into a witch-hunter and turns every laggy sim into a purge. So the organising idea is: **the tool never renders a verdict; it renders a ledger of falsifiable signals, each shown next to its own innocent explanation and its own population baseline, so that suspicion is always a comparison the officer makes themselves, in view of the counter-evidence.** Nothing is "suspicious" in isolation — a single through-wall hit is bullet-travel-time noise; a *pattern* of them, shown against how often *everyone else* in the same fight produced that pattern, is a question worth asking. The whole floater set exists to make that comparison fast, honest, and drillable down to the one raw event that either confirms or kills the pattern.

## 2. Ladder

**Rung 0 — concrete instant.** The live/scrubbed 3D overlay at the cursor time: the selected avatar's view cone, one LOS ray candidate to their target, the hit dot, and an "awareness glyph" (eye = could see, ear = could hear only, wall = blocked). *Step up*: click "Show pattern" on the glyph → jumps to the Engagement Card for that avatar (rung 1). *Step down*: from any higher rung, clicking a single tick, row, or matrix cell always re-anchors the overlay's cursor to that instant and opens this view.

**Rung 1 — abstract over time.** The *Engagement Trail*: every shot a chosen avatar landed all session, drawn simultaneously as thin lines from shooter-track to target-track, colour-coded by awareness class (green CLEAR, amber SOUND-ONLY, red BLOCKED, grey UNKNOWN) instead of one moving dot. A wall with a fan of red lines through it is visible as a shape, not a statistic. *Step up*: the same avatar's row in the Fairness Matrix (rung 3) or the LOS travel-time sweep (rung 2). *Step down*: click any single line to drop to its rung-0 instant.

**Rung 2 — abstract over parameter.** Two independent sweeps, both literal sliders: (a) the *LOS travel-time window* — sweep candidate shooter time `t − k·Δ` for one hit and watch the verdict flip CLEAR↔BLOCKED as k changes, with the flip point marked; (b) the *baseline threshold* — sweep "how many × the session median counts as notable" and watch the Watchlist reorder live. Both make the arbitrariness of any single cutoff visible instead of hidden in code. *Step up*: pin the flip point or threshold as an annotation on the avatar's page. *Step down*: any candidate or threshold position can be clicked to render its rung-0/1 consequence immediately.

**Rung 3 — multi-dimensional small multiples.** The *Fairness Matrix*: avatars × signal types, one sparkline/heat cell per pair (mouselook %, awareness-class mix, speed-flag count, equipment-suspicion count). Scan the whole roster in one screen. *Step up*: none above this — it is the ceiling, deliberately, because the brief warns against "verdicts"; there is no rung that collapses the matrix into a single score. *Step down*: click any cell → rung 1 Engagement Trail for that avatar/signal.

## 3. Information architecture

Nouns, each a page with backlinks, reachable from a shared breadcrumb bar (`Watchlist › Avatar:Kaela › Event:14:32:07 Death › Equipment:Ashguard Repeater`):

- **Avatar** — identity, team (with confidence), the Awareness Ledger, kills/deaths, equipment used, all signal instances, backlinks from every event/equipment/team page that mentions them.
- **Event** (DAMAGE/DEATH) — as already planned in the Inspector, plus a **Fairness sub-tab**: awareness class at the moment of the hit, LOS candidate table, reaction-latency figure if part of an engagement.
- **Equipment** — facts + classification (existing plan) plus the **Fairness Ledger**: through-wall rate with confidence interval, damage-adjustment ratio, list of avatars who used it.
- **Team** — roster, confidence, friendly-fire log (existing plan) plus aggregated fairness signals per team (does one side show more blocked-hit clustering — could mean a real chokepoint fight, not cheating).
- **Signal** — a *documentation* page per signal type (Reaction Latency, Awareness Mix, Speed Flag, Through-Wall Rate, Wireframe-Suspect Heuristic): plain-language definition, exact formula, known false-positive causes, and a live list of every instance across the session. This is the page that keeps the tool honest — every number the officer sees links to the page that explains how it can be wrong.
- **Place** (a front/chokepoint, inferred from death density) — where signals cluster geographically, so "everyone gets flagged for blocked-hits at the north wall" reads as a map problem, not a roster problem.
- **Session Baseline** — the reference distributions (median reaction time, median awareness mix, etc.) that every per-avatar number is compared against; itself inspectable and re-computable if the officer excludes outliers.

Navigation model: a persistent breadcrumb bar in both floaters, back/forward like a browser, a pinboard (right-click "Pin" on any row/cell keeps it in a sidebar list across navigation — for building a promotion or violation case file across many pages), and backlinks rendered as a small "Referenced by (n)" list at the bottom of every page.

## 4. Screens

### 4.1 Fair Play Auditor (new floater, `ss_fairplay_auditor`, opens from Combat Log floater's "Audit" button or Combat menu)

```
+-[ FAIR PLAY AUDITOR ]-------------------------------------------------+
| Watchlist > Kaela Ashguard > Event 14:32:07 Death                [<][>]|
+-------------------------------------------------------------------------+
| [Watchlist] [Fairness Matrix] [Equipment Ledger] [Baseline]            |
+-------------------------------------------------------------------------+
| Watchlist  (sorted by signal count, not "score")     Threshold: [==|--]|
| ---------------------------------------------------  2.5x median      |
| Avatar        Signals  Mouselook% AwareMix        Notes               |
| Kaela Ashg.    4       61%        58/12/24/6      3 blocked hits(N wall)|
| Rook Bellamy   2       12%        80/10/8/2       1 speed flag(coarse) |
| Vex Solari     1        4%        70/20/10/0      -                   |
| ...                                                                    |
+-------------------------------------------------------------------------+
| [ Open Avatar Page ]  [ Pin ]  [ Show Trail In-World ]                 |
+-------------------------------------------------------------------------+
| Status: 42 avatars tracked · baseline recomputed 14:40 · excludes 2    |
+-------------------------------------------------------------------------+
```
"Signals" is a raw count of instances that crossed the current threshold slider, never a weighted composite score — the column header literally says how many, and the slider up top makes the cutoff visibly adjustable so nobody mistakes it for a fixed judgement. `AwareMix` is `CLEAR/SOUND/BLOCKED/UNKNOWN` as percentages of that avatar's landed hits.

Fairness Matrix tab (rung 3, custom `LLView` grid drawn from rects+text, one row per avatar, one column per signal, cell = small sparkline or 2x2 heat swatch):
```
+-------------------------------------------------------------------------+
| Avatar        Mouselook  AwareMix     Speed    ThruWall   FF-given      |
| Kaela Ashg.   [####----] [▓▓░░]       [ok]     [▓▓▓░]     0            |
| Rook Bellamy  [##------] [▓░░░]       [!1]     [░░░░]     1 (Ashguard) |
+-------------------------------------------------------------------------+
```
Clicking any swatch opens that avatar's Engagement Trail filtered to that signal.

Equipment Ledger tab:
```
+-------------------------------------------------------------------------+
| Equipment          Hits  ThruWall%(CI)     AdjRatio   Users             |
| Ashguard Repeater   38   8% (2–22%, n=38)   0.94       Kaela, Rook      |
| Unknown HUD-31       6   33%(10–70%, n=6)   0.55       Vex              |
+-------------------------------------------------------------------------+
```
The confidence interval column is load-bearing: it is how "Unknown HUD-31 killed through walls a third of the time" gets read correctly as "six hits, could easily be one bad sim tick."

Baseline tab shows the session's reference histograms (reaction latency, awareness mix, speed) with the current avatar's value marked, and a "recompute excluding selected outliers" button — so officers can see whether one lag-spiking avatar is warping everyone else's baseline.

### 4.2 Avatar Page (opened from any avatar link, hosted as a mode of the existing Combat Inspector)

```
+-[ INSPECTOR: Kaela Ashguard ]------------------------------------------+
| Team: Ashguard-A (72% conf, override) | K/D 6/2 | Mouselook 61%        |
+-------------------------------------------------------------------------+
| Awareness Ledger:  CLEAR 58% | SOUND 12% | BLOCKED 24% | UNKNOWN 6%    |
|   [see all 24 BLOCKED instances >]                                    |
+-------------------------------------------------------------------------+
| Signal: Through-wall hit cluster, N wall (3 instances)      [Signal >] |
|   14:32:07 death of Rook — BLOCKED at k=0..4, flip at k=5 (0.8s back) |
|   14:34:51 hit on Vex   — BLOCKED all candidates              |
|   14:41:02 hit on Vex   — BLOCKED all candidates              |
|   Counter-evidence: sim has one known dense chokepoint here (Place >) |
+-------------------------------------------------------------------------+
| Equipment used: Ashguard Repeater (34 hits), Grenade HUD (4 hits)      |
| Movement: max 6.1 m/s (BRIDGE sample, plausible) · seated 4m (mount)  |
+-------------------------------------------------------------------------+
| [ Track In-World ] [ Pin ] [ Add officer note ]                        |
+-------------------------------------------------------------------------+
```
The counter-evidence line is not optional decoration — every clustered signal on this page is required to show whether the Place page independently explains it (a known chokepoint raises everyone's blocked rate, which is exculpatory, not incriminating).

### 4.3 Engagement Card (event-level fairness detail, a tab inside the standard event Inspector)

```
+-[ EVENT 14:32:07  Rook Bellamy died ]-----------------------------------+
| Killed by: Kaela Ashguard, Ashguard Repeater, 41 dmg (explosive)       |
+-------------------------------------------------------------------------+
| [Damage chain] [Equipment] [Fairness]                                  |
+-------------------------------------------------------------------------+
| Reaction latency: 0.4s after target audible (SOUND cue, Rook fired    |
|                    0.4s prior) — within population median (0.3–0.9s)  |
| LOS sweep (travel window 1.5s, 8 candidates):                         |
|   k=0 BLOCKED  k=1 BLOCKED  k=2 BLOCKED  k=3 BLOCKED  k=4 BLOCKED      |
|   k=5 CLEAR    k=6 CLEAR    k=7 CLEAR                                  |
|   -> verdict: CLEAR (flip at k=5, 0.75s before hit) — plausible: Kaela |
|      likely fired while stepping past the doorway                     |
| [ Move overlay cursor here ] [ Sweep candidates in-world ]             |
+-------------------------------------------------------------------------+
```

## 5. In-world overlay

Rung-0 additions on top of the plan's trail/death markers, gated identically (stationary + alt-cam only):

- **Awareness glyph**, drawn at the shooter's head at the moment of a selected hit: a small eye (green, CLEAR), ear (amber, SOUND-ONLY), or broken-wall icon (red, BLOCKED) — geometry, not colour alone, so it reads for colour-blind officers.
- **View cone**: a thin translucent wedge from the shooter's eye along their yaw at hit time, width fixed (fair-use HUD-style FOV), depth-tested off at low alpha so it reads through geometry — this is what lets an officer see "they were not even facing the target" versus "dead on."
- **LOS candidate fan**: instead of one ray, all `k=0..N` candidate rays drawn at once for the selected hit (rung 2 in-world), red for blocked, green for clear, with the flip point pulsing — clicking a ray jumps the cursor to that candidate's exact time.
- **Engagement trail** (rung 1): all of a selected avatar's landed-hit lines for the whole session, coloured by awareness class as above, alpha constant (this is a "the whole picture" view, not a recency fade) — toggled per-avatar from the Auditor, not always-on, to avoid clutter.
- **Minimap-fair shading**: a very faint ring at true minimap radar range around the shooter at hit time (fixed sim-wide radius) — a hit landing on a target whose *coarse* position was inside this ring is annotated "minimap-plausible" in the tooltip, explicitly defusing the single most common false accusation.
- Clicking any glyph, cone, or ray opens the Engagement Card at that instant; clicking empty space clears selection but leaves the overlay layer toggled on.

## 6. Analysis model

- **Awareness class** (per landed hit): CLEAR if any LOS candidate in the travel window is unobstructed; SOUND-ONLY if all candidates BLOCKED but target was within a fixed audible range (~20 m, SL default chat/attack proximity) and fired within the preceding 2 s (their own shot is a plausible audible cue, taken from the target's own DAMAGE-as-source events); BLOCKED if all candidates blocked and no audible cue; UNKNOWN if the shooter track has no samples in the window. Computed from `lineSegmentIntersectWorldGeometry` per the existing LOS module, reused, not re-derived.
- **Reaction latency**: `t(first damage from shooter to target) − t(target first became knowable to shooter)`, where "became knowable" is the earliest of: LOS candidate flips to CLEAR, target fires (audible), or target enters shooter's last known view cone (yaw ± FOV from track). Always reported with the *cue type* that produced it, since a sound-cue reaction and a LOS reaction mean different things.
- **Speed flag**: per-avatar max speed from BRIDGE or VIEWER quality samples only (COARSE, 1 Hz/4 m-quantised, is excluded — a single COARSE jump is measurement error, not motion) compared against a class-appropriate cap table (walking/running/flying/vehicle, from `AGENT_*` flags at that sample); flagged only if sustained over ≥2 consecutive good samples.
- **Through-wall rate per equipment**: `BLOCKED hits / total hits`, reported with a Wilson score interval (not a raw percentage) so small-n weapons don't look damning.
- **Damage adjustment ratio**: `Σdamage / Σinitial` per avatar/team/equipment from `modifications`, used for both Q5 (armour effect) and Q7 (equipment fairness — an adjustment script that makes a weapon deal far more than `initial` is itself a fairness signal, shown on the Equipment page).
- **Population baseline**: for each signal, the session-wide median and IQR computed only over avatars with ≥N qualifying instances (avoids one avatar with 1 sample skewing "the norm"); every per-avatar figure is shown as "vs session median" not against a hardcoded constant, because sim geometry and lag vary raid to raid.
- **Wireframe-suspect heuristic** (deliberately the weakest, most hedged signal): flagged only when an avatar's BLOCKED-hit share is materially above the *current* threshold slider **and** the Place page shows no chokepoint/geometry explanation **and** the pattern recurs against multiple different targets/times (rules out one lucky lag spike). Never labelled "wallhacking" anywhere in the UI — the signal's own documentation page states outright that it cannot prove client-side rendering settings and lists every known confounder (dense terrain, third-avatar occlusion not modelled by the raycast, voice/text spotting, log lag).
- **Team confidence** and **friendly-fire flags** reuse the plan's clustering module unchanged; this entry only adds that team-conditioned baselines exist (e.g. "blocked-hit rate compared within your own team's engagements," useful in intra-group skirmishes where the global baseline mixes two sides of one group).

## 7. Walkthroughs

**Q1 — how did the raid go?** Open Combat Log floater, Live/whole-session view; overlay in helicopter rung-1 mode shows all trails as one picture, death ticks on the timeline show two clusters (a push at 14:10–14:20, a rally at 14:35). Switch to Fairness Matrix for a first honesty pass — nothing lights up unusually, so the review is "clean," which is itself useful in a post-raid briefing.

**Q3 — why did X die at 14:32?** Click the death tick → Inspector → Damage chain shows Kaela's Repeater hits leading up to it → Fairness tab shows CLEAR verdict (flip at k=5) with the doorway explanation drawn in-world by clicking "Sweep candidates in-world" — officer confirms visually that Kaela stepped into a sightline just before firing. Answered concretely, no accusation needed.

**Q8 — did anyone behave suspiciously?** Officer opens the Auditor Watchlist, drags the threshold slider down to see who appears at 2×, then back up to 4× median to see who survives — only Kaela's north-wall cluster persists. Open her Avatar Page: 3 BLOCKED instances, but the Place page shows that wall is a known chokepoint and two other avatars unrelated to her show the same elevated BLOCKED rate there. Officer's conclusion (never the tool's): "geometry explains it, not equipment" — logged as an officer note on her page, not a system verdict.

**Intra-group skirmish** (two Ashguard teams): Team panel shows active-group tag useless (both sides tagged Ashguard); clustering falls back to who-shoots-whom, produces two clusters at ~80% confidence with a manual override control. Fairness baselines are computed *per inferred cluster*, not per group tag, so friendly-fire-shaped patterns (both sides technically "Ashguard") don't wrongly zero out the awareness-mix baseline — an officer reviewing "Team A vs Team B" gets independent, correctly-scoped medians.

**FFA sim** (Bad Space-style deathmatch): clustering confidence stays low/flat (everyone roughly equidistant in damage graph) — Teams tab honestly shows "no stable teams, treat as individuals," and every Auditor baseline computation automatically falls back to whole-population (not per-team) since no team split is trustworthy; the UI states this fallback explicitly rather than silently producing a meaningless team baseline.

## 8. Risks and open questions

- The single greatest risk is officers reading "Signals: 4" as a score anyway despite the UI's framing; mitigate with copy discipline (never "suspicion score," always instance counts) and by never sorting anything by a composite number.
- LOS raycasts ignore other avatars (stated in the brief), so a target hidden behind a third avatar reads as false CLEAR and a target correctly seen past a thin avatar-shaped gap can read as false BLOCKED — this must stay visible on the Signal documentation page, not buried in a tooltip.
- Reaction-latency's "audible cue" model is a guess at SL's actual sound falloff and is not verified against real client behaviour; needs an explicit confidence caveat until measured.
- The Wilson-interval equipment ledger still invites a "gotcha" reading if an officer only glances at the point estimate; consider forcing the interval to render before the percentage number, not after.
- Baseline recomputation with outlier exclusion is powerful but can be used to make an outcome disappear ("recompute without Kaela") — needs an audit trail of what was excluded, shown permanently on the Baseline tab, so nobody quietly launders a target out of the reference population.
