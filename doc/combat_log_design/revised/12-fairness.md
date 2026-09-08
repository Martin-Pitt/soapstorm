# Entry 12 (revised) — Behaviour-Audit-First: Combat Log as a Fair-Play Instrument

## 1. Thesis

Every existing kill-feed or damage-meter answers "who won." This entry answers a harder question: "was it won fairly?" — a question that is trivially abused. A tool that outputs a "cheat score" turns an officer into a witch-hunter and turns every laggy sim into a purge. The organising idea is: **the tool never renders a verdict; it renders a ledger of falsifiable signals, each shown next to its own innocent explanation and its own population baseline, so suspicion is always a comparison the officer makes themselves, in view of the counter-evidence.** Nothing is suspicious in isolation — one through-wall hit is bullet-travel-time noise; a *pattern* of them, shown against how often everyone else in the same fight produced that pattern, is worth asking about. This revision adds the piece the first draft skipped: "everyone else in the same fight" is not just a population, it is a place and a time — a signal only reads honestly once the officer can see the shape of the raid it happened inside (a real front, or an outlier). The floater set makes both comparisons — against the roster, and against the raid's own shape — fast, honest, and drillable down to the one raw event that confirms or kills the pattern.

## 2. Ladder

**Rung 0 — concrete instant.** The live/scrubbed 3D overlay at the cursor time: the selected avatar's view cone, one LOS ray candidate, the hit dot, and an awareness glyph (eye = CLEAR, ear = SOUND-ONLY, broken-wall = BLOCKED). The two measured glyphs (eye, wall) draw solid; the modelled one (ear, an unverified audible-range guess) draws hollow/dashed — ground truth and guess must not look equally certain. *Step up*: click "Show pattern" → Engagement Trail (rung 1), avatar-filtered. *Step down*: from any higher rung, clicking a tick, row, or matrix cell re-anchors the overlay's cursor to that instant.

**Rung 1 — abstract over time, whole raid by default.** This is the brief's own "helicopter" rung — every avatar's trail, the whole session, as one picture — and it is a first-class view here, not borrowed from elsewhere: the **Raid Shape** panel plots every avatar's decimated track as a trail (no moving dot), death density as a heatmap over time and place jointly, and auto-detected phases (push/lull/rally, changepoints in the density curve the base plan's timeline already tracks) as labelled bands. Clicking "Filter to Kaela" narrows the same rendering to one avatar's trail — what the first draft called "Engagement Trail." These are one rung, not two: a whole-raid instance and an avatar-scoped instance of the same abstraction, matching the brief's own Rung 1 definition instead of skipping past it. *Step up*: pin a phase or an avatar-filtered trail into the Fairness Matrix (rung 3) or Flagged-Signal Map (rung 4). *Step down*: click any trail segment, phase band, or density cell to drop to its rung-0 instant.

**Rung 2 — abstract over parameter.** Two literal sliders: (a) the *LOS window* sweeps candidate shooter time `t − k·Δ` and watches the verdict flip, now widened by a second, independently-shaded margin for the log's own up-to-1s timestamp lag (see §6) so the flip point and the "this could be wrong anyway" band are both visible at once; (b) the *baseline threshold* sweeps "how many × the session median counts as notable" and refilters (not reorders by a sum — see §3/§4) the Watchlist live. *Step up*: pin the flip point or threshold as an annotation on the avatar's page. *Step down*: any candidate or threshold position clicks through to its rung-0/1 consequence.

**Rung 3 — multi-dimensional small multiples.** The *Fairness Matrix*: avatars × signal types, one sparkline/heat cell per pair. Scan the whole roster in one screen. *Step down*: click any cell → rung 1, avatar-filtered trail, that signal highlighted. *Step up*: select any set of flagged cells → rung 4.

**Rung 4 — several dimensions plus raid shape.** The *Flagged-Signal Map*: takes whatever the Matrix or Watchlist currently has flagged and plots those instances back onto the Raid Shape canvas — where and when, relative to the raid's own fronts, did this roster's fairness questions cluster. A flag pattern that lines up with a front band reads as geography; a pattern with no front nearby reads as a real question. This is the rung the first draft was missing, and it answers "was that cluster a real fight or an anomaly" instead of asserting it in prose. *Step down*: any plotted instance → rung 0. There is no rung above this in v1 — not because a composite score must never exist above it (that reasoning was a non-sequitur: the brief's "never get stuck in the clouds" is an interaction rule, not a cap on abstraction count, and conflating the two is why Q1 went unanswered before), but because no further combination of the available fields (position-free DAMAGE, sparse DEATH positions, inferred equipment/teams) earns a fifth dimension worth a whole new view yet. If one turns up, it slots in above rung 4 the way rung 4 slots in above the Matrix.

## 3. Information architecture

Nouns, each a page with backlinks, reachable from a shared breadcrumb bar (`Raid Shape › Front:North-Wall(14:08–14:22) › Avatar:Kaela Ashguard › Event:14:32:07 Death › Equipment:Ashguard Repeater`):

- **Avatar** — identity, team (with confidence), the Awareness Ledger (now `CLEAR 58% (46–69%, n=17) · SOUND 12% · BLOCKED 24% (13–39%, n=17) · UNKNOWN 6%` — interval-and-n first, like the Equipment Ledger, so a person's number is never trusted more readily than a weapon's), kills/deaths, equipment used, all signal instances tagged `thin` below n=8, backlinks from every event/equipment/team/front page that mentions them.
- **Event** (DAMAGE/DEATH) — the planned Inspector, plus a **Fairness sub-tab**: one row per DAMAGE event inside the death-attribution window (not just the killing blow — three contributing hits get three independent awareness-class + LOS-sweep rows), each with its own reaction-latency figure or an explicit "no cue available" state (§6, ambush case).
- **Equipment** — facts + classification, plus the Fairness Ledger: through-wall rate with Wilson interval, damage-adjustment ratio, users.
- **Team** — roster, confidence, friendly-fire log, plus fairness signals aggregated *per time-window* (§6) so a mid-session split doesn't blend pre/post behaviour into one number.
- **Signal** — a documentation page per signal type: definition, formula, known false-positive causes, live instance list; unchanged, still the strongest honesty mechanism in the entry. The minimap annotation (§5) and the SOUND-ONLY audible-cue model now get an entry here too, hedged like the wireframe heuristic already was.
- **Place** — a front/chokepoint, now with a **density-over-time sparkline**, not a static geography fact: a wall that was a chokepoint only 14:08–14:22 shows that as a shape, so "the north wall" can't stand in for "always suspicious here." Every counter-evidence claim cites the geography, the active time-window, and its own sample size (deaths only — usually an order of magnitude fewer than the DAMAGE signal it's explaining).
- **Raid Shape** (new) — the whole-session, all-avatar picture: trails, joint time+place death density, auto-detected phase bands. Every phase links to its Place entries and active avatars/teams.
- **Session Baseline** — reference distributions, computed per time-window (re-segmented at clustering-confidence changepoints, e.g. a team split), each window inspectable, with a permanent, visible outlier-exclusion audit trail.

Navigation: persistent breadcrumb, back/forward, a pinboard (right-click "Pin" keeps a row/cell in a sidebar across navigation, for building a case file), and "Referenced by (n)" backlinks on every page. The Auditor floater and the Combat Log floater are separate, non-modal, dockable floaters that stay open together — narrowing a trail's time window in the Auditor while alt-cammed does not require leaving the overlay; both are visible at once.

## 4. Screens

### 4.1 Fair Play Auditor (`ss_fairplay_auditor`, opens from Combat Log floater's "Audit" button)

```
+-[ FAIR PLAY AUDITOR ]-------------------------------------------------+
| Raid Shape > Front:North-Wall(14:08-14:22) > Kaela Ashguard      [<][>]|
+-------------------------------------------------------------------------+
| [Raid Shape] [Watchlist] [Fairness Matrix] [Equip. Ledger] [Baseline]   |
+-------------------------------------------------------------------------+
| Watchlist   Filter: signals >= [==|--] 2.5x median   Sort: [Name v]    |
| ---------------------------------------------------------------------- |
| Avatar        ThruWall  Speed  AwareMix(n)          Notes              |
| Kaela Ashg.    3(15%,n=17,thin) ok  58/12/24/6(n=17) 3 blocked, N wall  |
| Rook Bellamy   0                !1   80/10/8/2(n=12) 1 speed flag(crs.)|
| Vex Solari     0                ok   70/20/10/0(n=6, thin)  -          |
| ...                                                                    |
+-------------------------------------------------------------------------+
| [ Open Avatar Page ]  [ Pin ]  [ Show Trail In-World ]                 |
+-------------------------------------------------------------------------+
| Status: 42 avatars tracked - baselines: 3 windows (split @14:40) -     |
|         excludes 2 (audit >) - counts cached, filter re-scans 0 events |
+-------------------------------------------------------------------------+
```
Two structural changes from the first draft, both closing the same hole: there is no summed "Signals" column, and the slider is a **filter**, not a rank. Each signal type keeps its own raw count column; the slider hides rows where *every* count sits below threshold×median for its own signal type, and rows that remain are ordered by name (or by one clicked column's own count — never a sum of columns), so nothing here reads as a leaderboard. `AwareMix` always carries its `n`; below n=8 a row is marked `thin` and excluded from ranking-by-column (still viewable, just not sorted to the top by a handful of samples). Counts are precomputed per avatar per signal type as the log arrives, so the slider only re-filters a small cached table — never rescans events.

Raid Shape tab (rung 1, whole-raid; custom `LLView` canvas, x = session time, y = a place-cluster axis, cell/band = density):
```
+-------------------------------------------------------------------------+
| [Push 14:08-14:22]  [Lull]  [Rally 14:33-14:41]     Filter: [ Kaela v ] |
| N-Wall  ############--------############------------                  |
| Courtyd ----########------------------################----------------|
| Spawn   ##----------------------------------------------------########|
+-------------------------------------------------------------------------+
| Deaths: 14  Damage events: 3,410 (shown as density only, no positions) |
+-------------------------------------------------------------------------+
```
Rows are Place clusters (from death-position clustering); density bands are the same time-windowed history that lives on each Place page. The `Filter` dropdown re-renders the same canvas restricted to one avatar's trail plus their own flagged instances — the "avatar-filtered Rung 1" from §2, same widget, one control.

Fairness Matrix tab (rung 3) and Equipment Ledger tab (Wilson-CI-first: `8% (2-22%, n=38)`, interval before point estimate) are unchanged from the first draft.

Flagged-Signal Map is reached via a "Project onto Raid Shape" button on the Matrix or Watchlist, not a persistent tab — it reuses the Raid Shape canvas with flagged instances plotted as colour-coded dots per signal type.

Baseline tab: per-time-window reference histograms, current avatar's value marked, a visible audit list of excluded outliers (who, when, why) that cannot be cleared without a logged reason.

### 4.2 Avatar Page

```
+-[ INSPECTOR: Kaela Ashguard ]------------------------------------------+
| Team: Ashguard-A (72% conf, override) | K/D 6/2 | Mouselook 61%        |
+-------------------------------------------------------------------------+
| Awareness Ledger (n=17): CLEAR 58%(46-69%) SOUND 12% BLOCKED 24%(13-39%)|
|   UNKNOWN 6%                                    [see all 24 BLOCKED >] |
+-------------------------------------------------------------------------+
| Signal: Through-wall cluster, N wall, window 14:08-14:22 (3 instances) |
|   14:32:07 death of Rook — BLOCKED k=0..3, flip at k=4 (0.8s back)     |
|   14:34:51 hit on Vex   — BLOCKED all candidates                       |
|   14:41:02 hit on Vex   — BLOCKED all candidates                       |
|   Counter-evidence: N wall was an active front 14:08-14:22 (Front >),  |
|     death sample n=6 there — sparse, treat as suggestive not proof     |
+-------------------------------------------------------------------------+
| Equipment used: Ashguard Repeater (34 hits), Grenade HUD (4 hits)      |
| Movement: max 6.1 m/s (BRIDGE sample, plausible) - seated 4m (mount)   |
+-------------------------------------------------------------------------+
| [ Track In-World ] [ Pin ] [ Add officer note ] [ View on Raid Shape ] |
+-------------------------------------------------------------------------+
```
The counter-evidence line now states its own sample size instead of gesturing at "a known chokepoint" as settled fact — six deaths is real signal, but explicitly weaker than the 3-instance pattern it's explaining, and the page says so rather than implying parity.

### 4.3 Engagement Card (Fairness tab inside the event Inspector)

```
+-[ EVENT 14:32:07  Rook Bellamy died ]-----------------------------------+
| Killed by: Kaela Ashguard, Ashguard Repeater, 41 dmg (explosive)        |
| Contributing hits in window: Kaela (killing blow), Zin Marsh (12 dmg,   |
|   14:31:58) — each swept independently below                           |
+-------------------------------------------------------------------------+
| [Damage chain] [Equipment] [Fairness: Kaela] [Fairness: Zin]            |
+-------------------------------------------------------------------------+
| Reaction latency: 0.4s after target audible (Rook's own shot, 0.4s     |
|                    prior) — within population median (0.3-0.9s)       |
| LOS sweep, travel window 1.6s, 9 candidates (k=0..8, step 0.2s):        |
|   k=0 BLOCKED  k=1 BLOCKED  k=2 BLOCKED  k=3 BLOCKED                    |
|   k=4 CLEAR    k=5 CLEAR    k=6 CLEAR    k=7 CLEAR   k=8 CLEAR          |
|   -> verdict: CLEAR (flip at k=4, 0.8s before hit)                      |
|   +/- log timestamp uncertainty (event time may be off by up to 1s):    |
|      re-run at t-1s -> flip shifts to k=1..2 (still CLEAR); at t+1s ->  |
|      flip shifts to k=6..7 (still CLEAR) -- verdict is ROBUST to lag    |
| [ Move overlay cursor here ] [ Sweep candidates in-world ]              |
+-------------------------------------------------------------------------+
```
Two fixes here: the sweep now shows what happens if the event's own timestamp — independently lagged by up to 1s per the brief — were off in either direction, and states whether the verdict survives that (a "robust" CLEAR is a stronger claim than one that flips under a 1s nudge, and both are shown). And a death with more than one contributing DAMAGE event gets one Fairness sub-tab per contributor, each independently swept, instead of only the killing blow.

## 5. In-world overlay

Gated identically to the plan (stationary + alt-cam only). Rung-0/1 additions:

- **Awareness glyph**: eye (green, CLEAR) and broken-wall (red, BLOCKED) draw solid, geometry-coded for colour-blind readability; ear (amber, SOUND-ONLY) draws hollow/dashed — a measured verdict and a modelled one must not look equally certain.
- **View cone**: translucent wedge from the shooter's eye at hit time, dual-pass (depth-tested + low-alpha depth-off) so occlusion by walls is directly visible.
- **LOS candidate fan**: all `k=0..N` candidate rays for the selected hit, red/green, flip point pulsing; clicking a ray jumps the cursor to that candidate's time. Needs thin-line-segment picking (distance-to-projected-segment, pixel tolerance) — a small, explicit addition to the plan's rect-per-marker pick list, not something that falls out of it for free.
- **Raid Shape trail mode** (rung 1, whole-raid): a distinct, exclusive overlay mode — not layered under per-avatar detail layers — drawing every avatar's already-decimated track as a simplified polyline (one vertex per direction change or 5s, whichever is coarser) so cost stays bounded regardless of roster size; death markers sized by local density. Toggling in replaces, rather than adds to, the plan's live per-avatar trail rendering, keeping frame cost inside the plan's budget.
- **Engagement trail** (rung 1, avatar-filtered): a selected avatar's landed-hit lines for a chosen time window — the Auditor's Raid Shape time-window control narrows it live while the overlay stays open beside it; the floaters are simultaneous, not sequential, so narrowing scope is not "leaving" the concrete view.
- **Minimap-range annotation**: a faint ring at the assumed sim-wide radar radius, labelled "minimap-range consistent (unverified — client radar settings are not knowable)." Never phrased as exoneration ("minimap-plausible" is dropped); one more falsifiable-but-hedged data point, listed on the Signal page beside the wireframe heuristic with the same confounder treatment.
- Clicking any glyph, cone, or ray opens the Engagement Card at that instant; clicking empty space clears selection but leaves the layer toggled on.
- **Two pinned avatars simultaneously**: each renders in its own hue and its cone/fan get a small perpendicular screen-space offset when they'd otherwise overlap, so two pinned comparisons don't merge into one shape.

## 6. Analysis model

- **Awareness class**: unchanged core (CLEAR if any candidate in the travel window is unobstructed; SOUND-ONLY if all candidates BLOCKED but an audible cue exists within ~20m/2s; BLOCKED if none; UNKNOWN if no track samples). New: every verdict is re-checked at `t-1s` and `t+1s` (the log's own stated timestamp lag, independent of the travel-time window) and labelled ROBUST if it doesn't flip across that range, else shown with both possible verdicts. This is the fix for the biggest fact-from-assumption gap in the first draft: the sweep was anchored to an event timestamp the brief says can itself be wrong by a margin comparable to the window being swept.
- **Reaction latency**: unchanged three cues (LOS flip, audible shot, view-cone entry), each reported with its cue type. The view-cone backscan is now bounded to the start of the current engagement (first DAMAGE between this shooter/target pair) or 30s, whichever is nearer, not session start — a small local scan, not a session-wide one; noted for click-to-response snappiness, not because an unbounded on-demand scan would itself have violated the brief (on-demand is explicitly permitted). **Ambush case**: if the target never fired and never entered the shooter's tracked view cone before the hit, none of the three cues resolve — the Fairness sub-tab reads "no reaction cue available (target gave no audible/visual signal before the hit)" explicitly, the correct display for a clean ambush, not a gap.
- **Speed flag, through-wall rate, damage adjustment ratio**: unchanged — BRIDGE/VIEWER-quality samples only with class-appropriate caps for speed; Wilson interval for through-wall; `Σdamage/Σinitial` for adjustment.
- **Population baseline**: now computed per rolling time-window, re-segmented whenever team-clustering confidence shifts materially for a meaningful share of the roster — a mid-session split produces two windows rather than one blended number; the Baseline tab and status line show which window is active and when it last re-segmented. Only avatars with ≥N qualifying instances *in that window* count toward it; others get a `thin` badge and are excluded from ranking, addressing mid-fight arrivals directly.
- **Wireframe-suspect heuristic**: unchanged three-way AND-gate (above threshold, no Place/geography explanation, recurs across targets/times); never labelled "wallhacking."
- **Minimap-range consistency**: reclassified from an exonerating signal to a hedged, symmetric one — same computation, but its documentation page states plainly it cannot verify another viewer's settings and must never singularly clear a suspicion, matching the wireframe heuristic's treatment.
- **Multiple contributing attackers**: every DAMAGE event inside a death's attribution window gets its own awareness class, LOS sweep, and reaction latency (or "no cue") independently — a death is not collapsed to its killing blow.
- **Front/phase detection (Raid Shape)**: joint time+place clustering of DEATH positions, with changepoints on the existing timeline density band marking phase boundaries (push/lull/rally); Place pages inherit a density-over-time sparkline from the same computation, caveated as death-only and therefore sparse relative to the DAMAGE volume it contextualises — every Place-based counter-evidence claim states its own n.
- **Caching**: per-avatar per-signal-type counts are maintained incrementally as events ingest; the Watchlist filter and Matrix cells read this cache, never rescan the log — live slider dragging re-filters a few dozen cached rows, not a query.
- **Team confidence/friendly-fire**: reuse the plan's clustering module unchanged; team-conditioned, time-windowed baselines are this entry's addition on top of it.

## 7. Walkthroughs

**Q1 — how did the raid go?** Open the Auditor, Raid Shape tab (default, whole-raid, no avatar filter): three density bands across three place-clusters show a push at N-Wall 14:08–14:22, a lull, and a rally at the Courtyard 14:33–14:41 — this entry's own rung-1 view, not borrowed capability. "Project onto Raid Shape" from the (empty, at rest) Watchlist confirms nothing pre-flagged clusters unusually against those fronts — a clean pass, itself useful for a briefing. Click the N-Wall band to drop to its Place page; click any point to drop to a rung-0 instant and alt-cam there.

**Q3 — why did X die at 14:32?** Death tick → Inspector → Damage chain → Fairness tab shows two contributors (Kaela's killing blow, Zin's earlier hit), each independently swept; Kaela's sweep is CLEAR, flip at k=4, ROBUST (survives the ±1s timestamp check) — "Sweep candidates in-world" shows her stepping into the sightline before firing. **Ambush variant**: if Rook had never fired and never entered the view cone, the Fairness tab reads "no reaction cue available" rather than a fabricated number — stated explicitly rather than left as an unhandled case.

**Q8 — did anyone behave suspiciously?** Watchlist, drag the threshold filter 2×→4×: rows with every count below threshold grey out; only Kaela's ThruWall count and one Rook speed-flag survive at 4×, each shown as its own raw count, not a summed score; Vex stays `thin` throughout regardless of the slider. Open Kaela's Avatar Page: 3 BLOCKED instances, N-Wall an active front 14:08–14:22 (n=6 deaths there, flagged sparse), and "Project onto Raid Shape" shows her instances landing inside that front window alongside two other avatars' elevated BLOCKED rates in the same place/time. Officer's conclusion, written as a note, not a verdict: "front geography explains it; sample is thin either way."

**Intra-group skirmish** (two Ashguard teams): clustering falls back to who-shoots-whom, ~80% confidence, manual override available; baselines computed per inferred cluster, not per group tag, in the current time window — if the split happens mid-session, the Baseline tab shows two windows (pre-split undivided, post-split per-cluster) rather than one blended figure.

**FFA sim**: clustering confidence stays low/flat; Teams tab states "no stable teams, treat as individuals"; baselines fall back to whole-population, stated explicitly rather than silently producing a meaningless team baseline.

## 8. Risks and open questions

- Removing the composite "Signals" sort closes the biggest self-inflicted risk from the first draft, but a filter that greys out rows can still read as ranking by "how many columns survived" — worth a usability pass to confirm officers read it as a filter, not a softened leaderboard.
- The timestamp-lag robustness check adds a second axis of uncertainty next to the travel-time sweep; risk of officer fatigue ("robust" vs "fragile" vs raw BLOCKED/CLEAR) — needs copy testing, not just correctness.
- LOS raycasts still ignore other avatars (brief-stated limitation): a target hidden behind a third avatar reads false CLEAR, one seen past a thin avatar-shaped gap can read false BLOCKED — stays on the Signal documentation page.
- Front/phase detection on death-only positions is the sparsest signal here and is labelled as such wherever used as counter-evidence, but a 1–2 death "front" is still a weak claim in the same UI chrome as a 20-death one; consider a minimum-n gate before a Place counts as an active front at all, not just an n label.
- The baseline audit trail (kept from the first draft) is necessary but not sufficient — a chain of individually-defensible exclusions can still hollow out a baseline over a long session; needs a running "excluded so far" count always visible, not just a drill-in list.
- Raid Shape's whole-roster trail mode is new relative to the fixed plan and needs its own perf pass — decimated polylines bound per-avatar cost, but 100 avatars simultaneously is more geometry than the plan's 60-avatar/20s trail budget assumed.

## Rejected critiques

- *"The unbounded view-cone backscan contradicts the brief's mandatory incremental/on-demand computation requirement."* Rejected as stated: the brief permits on-demand computation explicitly ("computed incrementally **or** on demand"), and a backscan triggered once per clicked death is exactly that, not a per-frame cost. The bound was added anyway in §6 for responsiveness, but the claim that the original design violated the brief's constraint is incorrect.
- *"Entry 12's whole-session Engagement Trail breaking the 'step between rungs without losing place' promise by requiring a trip to the 2D Auditor."* Rejected on its own terms: XUI floaters are non-modal and dockable, so the Auditor's time-window control and the live 3D overlay are simultaneously visible during alt-cam, not sequential — narrowing scope in one is not "leaving" the other. (The design was still improved in §5 to make this explicit, since it was previously only implied.)
