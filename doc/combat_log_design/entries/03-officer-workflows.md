# Entry 03 — Officer Workflows (workflow-first)

## 1. Thesis

An officer never opens the Combat Log tool to "look at data" — they open it because someone asked a question: *is this person ready to lead a squad, did this person break a rule, what do I tell the group tonight.* Every other design question — how deep the ladder goes, how uncertainty is drawn, what gets exported — should fall out of answering those three jobs well, not the other way around. This entry makes the **Case** the organizing unit of the tool: a Case Binder is a persistent, named container (Promotion Review / Violation Investigation / Post-Raid Briefing) that an officer opens *before* digging, and every view in the tool — overview, roster, team ledger, dossier, inspector, in-world overlay — has a one-click **Pin** that drops what the officer is looking at into the active Case as an **Evidence Card**: a small, self-contained, timestamped, confidence-tagged fact with a link back to where it came from. Analysis (attribution, teams, LOS, stats) is the same five modules the engineering plan already fixes; what workflow-first adds is that nothing the officer finds is ever "just seen" — it is either passed over or captured, and a Case is done when its pinned evidence, read in order, *is* the report. Export is not a bolt-on feature; it's the terminal state of a Case, so the tool is designed backward from "what text needs to exist in a notecard or group chat" to "what evidence produces that text."

## 2. Ladder

The ladder is the same physics as the brief's Bret Victor rungs, but every rung is entered *from a job* and every step down is *pinnable*.

- **Rung 0 — concrete instance.** A single death, a single damage line, a single LOS candidate, the in-world overlay at the scrubbed cursor time. Step down happens by clicking anything at any higher rung (a roster row, a heatmap cell, a trail segment) — it always opens the **Inspector** on that instance. Step up: the Inspector's "Show in world" / "Zoom out" always returns to the rung the officer came from, and a breadcrumb lets them retrace.
- **Rung 1 — abstract over time.** Avatar trails instead of a moving dot, the Overview heatmap (whole session as one picture), an avatar's Dossier timeline strip (mouselook bands, damage spikes, deaths, as one horizontal ribbon). Step up from Rung 0: any Inspector has "Show avatar timeline" / "Show all deaths this session." Step down: click a spike or a tick on the ribbon → Inspector on that instant.
- **Rung 2 — abstract over parameters.** LOS verdict swept across the travel-time window (a slider that recolors the candidate row live instead of only showing the final verdict); team-confidence threshold slider that redraws the Team Ledger's cluster boundaries as it moves; a before/after toggle on damage that replays `initial` vs `damage` across a whole fight. Step up: from a single LOS verdict, "Sweep window" opens the parameter view. Step down: dragging the slider to a specific value and clicking "Pin this reading" freezes it back to a concrete claim with that parameter value recorded.
- **Rung 3 — small multiples / multi-dimensional.** The Team Ledger (avatar × stat grid, one row per person, colored by team and by friendly-fire flag), the Equipment grid (weapon × {users, kills, adjustment ratio, wall-kill rate}), the Overview's front-line-over-time strip. Step up: Case Binder scorecards summarize a whole grid into 6–8 numbers. Step down: click any cell → the Dossier for that row/column.
- **Rung 4 — the Case itself.** A curated, ordered stack of Evidence Cards plus officer notes — the narrative rung, the only one a human wrote by choice rather than the tool computing. This is the rung workflow-first adds beyond the brief's ladder: it is what makes "digging" *end* in something exportable instead of ending when the officer gets tired of clicking. Step down: every card in a Case links straight back to its Rung 0–3 origin. Step up: "Export Summary" flattens the Case into text.

Per job, the natural entry rung differs: Promotion Review starts at Rung 3 (the candidate's Dossier — a stat grid of one), Violation Investigation starts at Rung 0 (the reported incident) and climbs to find pattern, Post-Raid Briefing starts at Rung 1 (the Overview heatmap) and samples down opportunistically. The tool doesn't force a direction; it just makes the Case Binder catch whatever rung the officer stopped at.

## 3. Information architecture

**Nouns (destinations, each a page with links and backlinks):** Session, Region/Place, Minute (a scrub point), Avatar, Team, Weapon/Equipment, Death, Damage cluster, LOS verdict, Adjustment script, Case, Evidence Card.

**Panels (views onto those nouns):**

- **Combat Log** (main floater, singleton, docked): transport + timeline + tabs *Overview / Roster / Teams / Equipment / Cases*. The hub; everything else is opened from here or from a click in-world.
- **Dossier** (one floater class, parameterized by noun kind — Avatar, Team, Weapon, or Place — this is deliberately *one* view type, not four, so "digging" always looks the same regardless of what you dug into). Shows identity, a Rung-1 timeline ribbon, a stat table, a "Related" link row, and a "Backlinks" row.
- **Inspector** (the plan's inspector floater): Rung-0 detail for a Death, Damage cluster, or LOS verdict. Shows related events, LOS sweep, adjustments, and buttons to jump to any Dossier it references.
- **Case Binder** (new; the workflow spine): one instance per open case, three templates (Promotion Review, Violation Investigation, Post-Raid Briefing) sharing one chrome — header/breadcrumb, subject/window picker, a template-specific summary block, the pinned Evidence Card stack, a notes editor, Export.
- **Evidence Card**: not a floater — a small embeddable widget (kind icon, one-line summary, confidence chip, view/pin/dispute buttons) that appears inside Case Binder, and as a hover/click preview anywhere a pinnable fact is shown.
- **Export Summary** sheet: modal over a Case Binder; renders the Case as plaintext, previews it, offers Copy / Save-as-notecard.

**Navigation model.** Every Dossier/Inspector keeps a **breadcrumb bar** (`Cases > Kestrel Voss > Death 14:32:07`) built from an explicit navigation stack, with back/forward — this is the wikipedia-digging mechanic made concrete. Every Dossier has a **Related** row (outbound links: an avatar links to its team, its top weapon, its deaths) and a **Backlinks** row (inbound: "referenced by 2 cases, 1 dispute, 6 deaths as killer") computed by scanning the event/case store for references to this id — cheap, since the officer only opens a handful of dossiers per session. **Pinning** is the one mechanic that crosses panel boundaries: any card, row, or in-world marker exposes a "Pin to case ▾" control listing open Cases (or "New Case…"); pinning snapshots the current fact (with its confidence and a permalink) into that Case's evidence stack. Cases persist for the session (and can be exported before the session data itself would be evicted) and are listed under the *Cases* tab of the main floater with status (draft/exported) and template icon.

## 4. Screens

**Combat Log (main floater)**
```
+-- Combat Log ----------------------------------------------------------+
| [Live] [<<][Play/Pause][>>] speed:[1.0x v] step:[-1s][+1s]   14:32:07  |
+--------------------------------------------------------------------------+
| |####|---deaths---|====damage density====|--------------------|  [+][-] |
|      ^cursor                                          session end        |
+--------------------------------------------------------------------------+
| [Overview] [Roster] [Teams] [Equipment] [Cases]                          |
+--------------------------------------------------------------------------+
| Overview tab:                                                            |
|  region heat trail (small multiple, 4 time-slices)   [Open Briefing >]  |
|  Fronts: west choke 22:10-22:45, courtyard 23:00-23:20                  |
|  Top event: 41 dmg kill @ 14:32:07 (Owen Vance -> Mara Lindqvist)       |
+--------------------------------------------------------------------------+
| rec: ON  bridge: OK  events 8,412  agents 37  poll RTT 210ms  drop 0    |
+--------------------------------------------------------------------------+
```
*Roster tab*: scroll list, columns `Name | Team | K | D | Dmg out | Dmg in | Mouselook% | [Dossier][Pin]`. *Teams tab* is the Team Ledger (below). *Equipment tab* mirrors Roster for weapons. *Cases tab*: list of Case Binders (`[icon] Kestrel Voss — Promotion Review — draft [Open][Export]`, `[New Case ▾]`).

**Team Ledger** (Teams tab body, a custom-drawn grid, confidence slider up top drives both the grid and the in-world overlay):
```
Confidence threshold: [----o-------] 0.55      clusters: 2 (was 3 at 0.70)
Name           Team        Conf   K   D  DmgOut DmgIn  FF-out FF-in
Owen Vance     Ashguard-A  0.86  14   6   8220  3110     0      1
Doran Reyes    Ashguard-A  0.61   9   4   4010  3900     1      0
Mara Lindqvist Ashguard-B  0.90   7   9   3300  6100     0      2
[row click -> Avatar Dossier]   [override team ▾]   [Pin ledger snapshot]
```

**Dossier** (one class, shown here as Avatar):
```
+-- Dossier: Avatar — "Kestrel Voss" --------------------------------[x]-+
| Cases > Kestrel Voss  |  history: [Overview] < >                       |
+--------------------------------------------------------------------------+
| Team: Ashguard-A (conf 0.86) [override]     Active group: Ashguard      |
+--------------------------------------------------------------------------+
| |--mouselook bands--|--dmg-out spikes--|--deaths x2--|                 |
+--------------------------------------------------------------------------+
| Kills 14  Deaths 6  DmgOut 8220  DmgIn 3110  Seated 4m12s  Speed max..  |
| Weapons: Ashguard Mk.II Carbine 62%, Frag Grenade 5%, ...               |
+--------------------------------------------------------------------------+
| Related: [Team Ashguard-A][Weapon Mk.II Carbine][6 deaths][1 FF?]       |
| Backlinks: referenced by 2 cases, 1 dispute                             |
+--------------------------------------------------------------------------+
| [Pin to active case]                              [Show in world]       |
+--------------------------------------------------------------------------+
```

**Inspector** (Death instance):
```
+-- Inspector: Death — Mara Lindqvist @ 14:32:07 ---------------------[x]-+
| Cases > Kestrel Voss > Death 14:32:07                                   |
+--------------------------------------------------------------------------+
| Victim: Mara Lindqvist (Ashguard-B)   Killer: Owen Vance (Ashguard-A)   |
| Weapon: Ashguard Mk.II Carbine (bullet)   Killing blow: 41 dmg          |
+--------------------------------------------------------------------------+
| Related damage (window -4.0s..0s)                                       |
|  -3.8s  12 dmg  bullet   LOS CLEAR                                      |
|  -1.1s  18 dmg  bullet   LOS BLOCKED (conf low, 1/5 candidates)         |
|  -0.0s  41 dmg  bullet   LOS BLOCKED (conf med, 2/5 candidates)         |
+--------------------------------------------------------------------------+
| LOS sweep @ 0.0s, window 1.5s, step 0.25s:  [scrub  ----o----------]    |
|  t-1.50 CLEAR  t-1.25 CLEAR  t-1.00 BLOCKED  t-0.75 BLOCKED  ...  t-0   |
+--------------------------------------------------------------------------+
| Adjustments: none                                                        |
+--------------------------------------------------------------------------+
| [Show in world]  [Track killer]  [Pin to case ▾]                        |
+--------------------------------------------------------------------------+
```

**Case Binder** (Promotion Review template):
```
+-- Case: Promotion Review — "Kestrel Voss" ---------------------------[x]-+
| < back  Cases > Kestrel Voss                             [Export ▾]      |
+------------------------------------------------------------------------+
| Subject: Kestrel Voss [change]   Window: [session v] 22:00-23:40        |
+------------------------------------------------------------------------+
| Scorecard (derived — not a verdict, check evidence before deciding)    |
|  Kills 14  Deaths 6  Dmg out 8220  Dmg in 3110  Combat-zone time 61%   |
|  Mouselook 74%   Friendly fire: 0 confirmed, 1 disputed (see #4)        |
+------------------------------------------------------------------------+
| Pinned evidence (4)                               [+ Pin from Roster]  |
|  1. Dossier: Kestrel Voss                                      [view] |
|  2. Death review 14:32:07 — 3 kills in the exchange, LOS clear [view] |
|  3. Weapon use: Ashguard Mk.II Carbine, 62% of kills            [view]|
|  4. Possible FF on Doran Reyes 21:04, conf 0.31        [dispute][view]|
+------------------------------------------------------------------------+
| Notes: [ "Steady under pressure, holds the west choke..."          ]  |
+------------------------------------------------------------------------+
|                                     [Save Draft]   [Export Summary >] |
+------------------------------------------------------------------------+
```
Violation Investigation swaps the Scorecard for `Reported by / Alleged / Window` fields and adds a **Verdict** picker (`Unsubstantiated / Inconclusive / Substantiated`) that only the officer sets — the tool never fills it in. Post-Raid Briefing swaps Subject for "Whole session" and replaces the Scorecard with a **Digest draft**: an editable text area pre-filled from Overview/Team Ledger/Equipment stats, with an "Insert stat…" button that opens a small picker onto those tables.

**Export Summary** (sheet over a Case Binder):
```
+-- Export Summary — "Post-Raid Briefing: 2026-09-08" ------------------[x]+
| Format: (o) Notecard   ( ) Chat-paste (auto-splits at 1024B)             |
+--------------------------------------------------------------------------+
| RAID DIGEST — Bad Space region — 22:00-23:52 (1h52m)                    |
| Fronts: west choke (22:10-22:45), courtyard collapse (23:00)            |
| Teams: Ashguard-A (14) vs Ashguard-B (11) — intra-group, conf 0.79      |
| Friendly fire: 2 confirmed, 1 disputed                                  |
| Top damage: Owen Vance 8220 ...                                         |
| Suspect equipment: "Ghost Slug" — 4/6 kills LOS BLOCKED (conf med)      |
+--------------------------------------------------------------------------+
|                                 [Copy to clipboard]  [Save as Notecard] |
+--------------------------------------------------------------------------+
```

## 5. In-world overlay

The overlay draws the same layers the engineering plan already specifies (trails, damage lines, death markers, LOS candidates), and workflow-first adds exactly one cross-cutting encoding: a small **tack glyph** rendered next to any marker (death cross, damage line midpoint, avatar head ring) that is already pinned into the *currently open* Case, in that Case's accent color. This turns alt-cam replay into a literal walk through the evidence the officer has gathered so far — gaps in the tack pattern are gaps in the case. Team colour still carries allegiance (desaturated/hatched fill when confidence is below the Team Ledger's threshold, per the base plan); LOS lines are green/red for clear/blocked with width encoding candidate-count confidence (thin = 1 of N candidates, thick = unanimous); unknown-position segments are dashed, never solid, so absence of data is never visually confused with "nothing happened." Clicking any marker opens the Inspector on it (as in the base plan); a modifier-free click on a *tacked* marker instead reopens the Case Binder scrolled to that card, so the officer can jump from "this is where it happened" straight back to "this is what I wrote about it." Ctrl-click on any marker offers "Pin to case ▾" directly, so evidence gathering never requires leaving alt-cam.

## 6. Analysis model

Derived quantities layered on the plan's five modules, all uncertainty-tagged (low/med/high confidence, or a 0–1 posterior where a model produces one):

- **Team assignment + confidence**: label-propagation posterior per avatar (plan's clustering); the Team Ledger's threshold slider re-thresholds the same posterior live, it does not recompute clustering, so sweeping the slider is a Rung-2 view, not a re-run.
- **Friendly-fire index**: `damage dealt to same-team targets / total damage dealt`, per avatar and per team; flagged per-event when attacker/victim posterior teams match above 0.5 even if under different clusters at other thresholds (shown as "disputed" if the flag flips across the plausible threshold range — this range check *is* the honesty mechanism, not a single number).
- **Death attribution weight**: proportional credit across the death window's damage contributors (`contributor's cumulative damage / total damage in window`), used for kill-assist lines in Dossiers and Case scorecards instead of a binary "who got the kill."
- **Damage adjustment ratio**: `damage / initial` per event, aggregated per avatar/team/script from `modifications`; shown as a before/after pair, never collapsed to one number, because the officer needs to see both the raw hit and what the armour script did to it.
- **Wall-kill suspicion score** per weapon: `BLOCKED verdicts / total verdicts` where BLOCKED requires *all* travel-window candidates blocked; the score carries its own confidence = fraction of that weapon's events with enough track samples to have a verdict at all (an under-sampled weapon gets a low-confidence score, displayed as such, never suppressed).
- **Behavioural anomaly flags** (question 8): reaction-time percentile (time from becoming visible-and-targetable to first return fire, relative to the whole session's distribution), speed z-score against the avatar's own baseline, and "awareness while occluded" (target changed aim/movement toward an attacker with no LOS in the preceding window) — every one of these is surfaced only as an **evidence card with the raw numbers and the counter-explanations** (lag, alt tabs, minimap use is fair per the owner's own framing), never as a "cheating: yes/no" flag. This is the single place the tool is most disciplined about not manufacturing a verdict.
- **Case completeness** (workflow-first addition): for Violation Investigation, a checklist derived from which evidence *kinds* are pinned (incident instance, LOS sweep, prior-history dossier, counter-evidence) vs. a fixed template list — shown as `3/5 checks` in the Case header so an officer can tell they're about to write a verdict on a thin file.

## 7. Walkthroughs

**Q1 — how did the raid go, overall (Post-Raid Briefing job).** Officer opens Combat Log → Overview tab, reads the heat trail and front list, clicks "Open Briefing" → new Case Binder (Post-Raid Briefing template, subject = whole session). The Digest draft is pre-filled from Overview/Team Ledger/Equipment. Officer scrubs the timeline for the two front collapses, pins the Overview heat-trail snapshot and the Team Ledger grid as evidence, edits the narrative, exports to a group notecard.

**Q3 — why did X die at 14:32 (Violation Investigation job, opened as a dispute).** Someone reports a "no-LOS kill" complaint. Officer starts a Violation Investigation case, subject = the death. From Roster, opens Mara Lindqvist's Dossier, clicks the death tick on her timeline ribbon → Inspector opens on Death 14:32:07, shows the three related damage lines and the LOS sweep (2 of 5 candidates clear at the wide end of the travel window, none clear near t=0). Officer scrubs the sweep slider, pins the sweep reading and the full damage cluster, opens the killer's Dossier via "Track killer," checks the killer's weapon Dossier for the weapon's session-wide wall-kill suspicion score (low confidence, few samples) — pins that too, and sets Verdict = Inconclusive with a note pointing at the confidence caveat.

**Q8 — suspicious behaviour, evidence not verdicts (continuing the same Violation Investigation).** Officer opens the "Anomaly" evidence type from the killer's Dossier: reaction-time percentile is unremarkable (48th), no occluded-awareness flags in the session. Both get pinned as counter-evidence. The Case's completeness checklist now reads 4/5 (missing "prior-history dossier"); officer pulls up the killer's history across the session (any earlier disputes) — none — pins that as the fifth check, and exports the case as a notecard with the Inconclusive verdict and the full evidence trail attached as text.

**Intra-group skirmish scenario.** Two Ashguard teams fight each other; active-group tag is useless. Officer opens the Team Ledger, sees clustering has split "Ashguard" into two posterior clusters at threshold 0.55 with visible friendly-fire flags concentrated at the cluster boundary (people whose posterior sits near 0.5 — likely swapped sides or were caught in cross-fire). Officer drags the confidence slider down to 0.35 to see if the boundary is stable (it is, clusters don't merge) — evidence the split is real, not noise — pins the ledger at both thresholds into a Violation Investigation case about a friendly-fire complaint, and manually overrides two mis-clustered avatars whose Dossier shows they only ever damaged one side.

**FFA scenario.** A neutral sim deathmatch with no stable teams. Clustering churns every few minutes as damage graphs shift; the Team Ledger shows clusters count "6 (was 4, was 9)" as the officer scrubs. Officer treats this as expected and switches the Post-Raid Briefing Digest to per-encounter framing: pins short-lived local clusters ("cluster around the north platform, 22:40–22:52") as separate evidence cards rather than trying to force session-wide teams, and the exported digest reads as a sequence of skirmishes rather than a scoreboard — because the Case Binder never requires teams to be stable, only evidenced at the window it names.

## 8. Risks and open questions

- **Pin fatigue**: if pinning is too easy, Cases become unread dumps instead of curated arguments; the completeness checklist and a visible "N cards, read time ~M min" counter in the Case header are a partial mitigation, not a solved problem.
- **Verdict creep**: scorecards and suspicion scores are one editorial slip away from reading as verdicts; every score-bearing widget needs the same "derived, not a verdict" caption enforced as a shared XUI include, not left to per-panel discipline.
- **Threshold-sensitive team scenes** (intra-group, FFA) make "the Team Ledger" a moving target across a session; freezing a ledger snapshot into a pinned card (rather than a live reference) is required so exported Cases don't silently change meaning if reopened later against updated clustering.
- **Case store growth**: Cases are session-scoped per the plan's in-memory-only decision; exporting is the only durability, so the tool should nag (not block) an officer with open, unexported Cases as the session/floater closes.
- **Generic Dossier risk**: one parameterized Dossier class for four noun kinds keeps the UI small, but Weapon and Place dossiers need different stat tables than Avatar/Team; the shared chrome (breadcrumb, timeline ribbon, related/backlinks) must stay generic while the stat-table body is the only per-kind specialization, or the "one view type" promise breaks down into four views wearing a costume.
