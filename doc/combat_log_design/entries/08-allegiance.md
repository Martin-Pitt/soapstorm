# Entry 08 — Allegiance: combat as a social field

## 1. Thesis

A raid is not a list of damage events; it is a *relationship* between people that changes shape over an hour. Every question an officer actually asks — was that a violation or an accident, is this candidate a team player, did the line hold, who is that guy fighting for — is a question about sides. So the tool's primary object is not the event and not the avatar: it is the **allegiance field**, a signed, time-decaying, directed graph over avatars in which hostility subtracts and cooperation adds. Teams are never stored; they are *solved* from the field at a moment, with a confidence, and re-solved when the officer asserts something the machine got wrong. Every other analysis in the brief — equipment, armour adjustment, line of sight, suspicious behaviour — is presented as a property *of a relationship or of a side*, because that is the frame in which it is actionable. The design's spine is one abstraction ladder over this field: a moment in world (rung 0), an engagement (rung 1), the session as a braid of factions (rung 2), and the same session re-solved across the parameters of the inference itself (rung 3). The tool's most important promise is that it is honest about not knowing, and cheap to correct: two clicks to say "these two are the same side", and it immediately shows what that claim changed.

## 2. Ladder

**Rung 0 — The Moment.** The in-world overlay at the cursor time: bodies, halos, yaw, the shot that is in flight. Concrete, visceral, one second wide.
*Step up*: press `[` or drag the trail slider — the moment grows a tail and becomes an engagement. *Step down from anywhere*: any row, cell, tick or thread in any 2D view has "Show in world", which parks the cursor at that time and flashes the actors.

**Rung 1 — The Engagement (burst).** A window (default 20 s) of trails, damage lines and deaths, with the participants named and the burst's relationship verdict shown (cross-faction / friendly fire / defection). This is the unit the machine actually reasons about.
*Step up*: "Show in session" puts the burst as a lit tick on the Ribbon. *Step down*: scrub inside the burst; the overlay collapses back to a moment.

**Rung 2 — The Session (Ribbon / Front).** All moments at once. The Ribbon is a braid: horizontal bands are factions, threads inside them are people, threads cross bands when allegiance changes, bands fork when a group splits, and the background texture is the *structure index* — how team-shaped the fight is at all, which decays to visible noise when the sim collapses into deathmatch. The Front mode replaces the braid with a coordinate transform: y is signed distance to the contested line, so pushes, collapses and who leads them are one glance.
*Step up*: the Sweep. *Step down*: click anywhere on the ribbon to set the cursor and load that minute.

**Rung 3 — The Inference (Sweep).** The session re-solved across the parameters of the analysis, as small multiples: allegiance at seven confidence thresholds, three memory half-lives, and with evidence classes ablated (drop group tags; drop splash damage; drop proximity). The officer sees which readings are robust ("two-sided everywhere from tau 0.18 to 0.47") and which are artefacts of one knob. Same for LOS: a through-wall verdict is swept across the whole bullet-travel window rather than asserted at one instant.
*Step up*: none — this is the top, and the design says so. *Step down*: click any small multiple to adopt that parameter set for every other view.

Nothing in the UI is a dead end: every abstraction cell knows which concrete events produced it and can list them.

## 3. Information architecture

**Two floaters.** `ss_combat_log` (the Session floater — transport, Ribbon, Matrix, Sweep, Roster, Events, Kit, Ledger) and `ss_combat_inspector` (the Page floater — one noun at a time, with history). This matches the fixed plan and refuses a floater zoo.

**Nouns (pages in the Inspector).** Person, Faction, Burst, Death, Damage, Equipment (item), Kit (an owner's loadout), Assertion, Moment (a named minute), Place (a kill-density cluster the officer can name: "Bridge"), Verdict (one LOS solve), Spawn cohort.

**Every page has the same three-part shape**: header (what it is, plus the honest-uncertainty badge), body (its own content), and two standard sections — *Links* (what it points at) and *Referenced by* (backlinks: which bursts, factions, verdicts and assertions mention this thing). Backlinks are what make digging feel like Wikipedia rather than like drilling down a tree; from a weapon you can walk back to the deaths, from a death to the faction, from a faction to the assertion that shaped it.

**Navigation.** Inspector has `[<] [>]` history, a breadcrumb line ("Session > RED > 14:31 burst > Kira Vex"), and a `[pin]` button that promotes the current page into a tab strip so an officer can hold a suspect, a weapon and a faction open side by side while walking around. Middle-click any link opens it pinned. Selection is global: whatever page is front sets the overlay highlight.

**One uncertainty vocabulary, everywhere.** Three states, drawn identically in ribbon, matrix, roster, halo and text: **solid** = confident; **grey hatch** = thin evidence (we have barely seen this person interact); **two-tone stripe** = conflicting evidence (they fight both sides). Never a fourth encoding, never a bare percentage without one of these three.

## 4. Screens

### 4.1 Session floater — Ribbon tab

```
+= COMBAT LOG - Bad Space ============================ session 01:47:12 =[_][#][X]=+
| [*LIVE] |< << [>] >> >|  1x  | -1s +1s |  cursor 14:31:58  win[20s]--o------      |
|  ( Ribbon )( Matrix )( Sweep )( Roster )( Events )( Kit )( Ledger )               |
+-----------------------------------------------------------------------------------+
| mode: (*)Threads ( )Front     tau 0.40 --o---   half-life 90s --o---   [Explain]   |
| structure  ####|####|####|##~~|~~~~|~~..|....|~~~~|####|####|####   S=0.71 (n .42) |
|      14:00     14:10     14:20     14:30 :    14:40     14:50     15:00           |
| RED   =========================================:============================  (9)  |
|  .82  ---------\      /--------------------- :  --------------------------        |
| BLUE  ==========\====/=========\=============:============================  (7)   |
|  .77                            \  split     :                                    |
| GRN                              \===========:======================= (4) "2nd"   |
|  .58                                         :   (thin evidence 14:22-14:29)      |
| ????  ~~~~~~~~~~~~~~~~~~~~~~ unaligned / new arrivals ~~~~~~~~~~~~~~~ (3)          |
| deaths  x   x x    xx  x    x  x   x  xx  x  x|x   x    x   x    x                |
| ff/def      ^                    ^^ ^         :        ^                          |
+-----------------------------------------------------------------------------------+
| 14:31:58 DEATH  Kira Vex  <- Orion Starchild  Kestrel AR   32.1m  LOS: BLOCKED(?)  |
| 14:31:57 DMG    Kira Vex  <- Orion Starchild  18.0 (24.0 x0.75 ARMOR-3)  type 0    |
| 14:31:55 DMG    Kira Vex  <- Orion Starchild  22.0                       type 0    |
+-----------------------------------------------------------------------------------+
| rec ON | bridge 44ms | 38 agents | 12,884 ev | 3 assertions | mean conf 0.71       |
+-----------------------------------------------------------------------------------+
```

*Transport bar*: live toggle, play/pause/speed, ±1 s step, cursor readout, and the trail-window slider that also drives the overlay — the rung 0↔1 control, so it is the most prominent slider in the tool.
*Parameter strip*: tau (confidence threshold for calling a side), half-life (how fast the field forgets), and `[Explain]`, which opens the Faction page for the band under the mouse.
*Structure band*: one glyph per 30 s of the structure index S, stripes → noise, with the null baseline as a number. When S − S_null stays under 0.08 for a minute the band prints "DEATHMATCH" and the faction bands go grey-hatched: the tool stops naming teams rather than inventing them.
*Bands*: one per live faction, height by headcount, fill alpha by cohesion, name editable in place. Threads inside a band are per-avatar polylines; a thread that leaves rides a diagonal into its new band; forks are drawn as a band splitting. Click a thread → Person page.
*Ticks*: deaths as x on the owning band; friendly-fire and defection carets on a separate lane so they are never confused with kills.
*Detail list*: the events at the cursor, always visible, so the abstraction never floats free of instances.
*Status line*: recording, bridge RTT, agents, events, assertions, mean confidence — the honesty gauge for the whole tool.

Implementation: one custom `LLView` drawing rects, lines and `SansSerifSmall`, marking itself dirty every frame the cursor moves (the `RenderUIBuffer` dirty-rect gotcha in `doc/viewer/ui_system.md` bites animated custom views). Threads decimate to one point per pixel column.

### 4.2 Session floater — Front mode (same tab, radio switch)

```
| mode: ( )Threads (*)Front     [contested band 12m]   [ ] show dead                 |
|  +40m RED rear ...........................................................        |
|       ...RED............/‾‾‾\......................                               |
|  +10m ........./‾‾‾‾‾‾‾‾      ‾‾‾\......                        push  collapse    |
|   0m  ==CONTESTED=======================\=========/‾‾‾‾‾‾‾‾‾\====================  |
|  -10m ......BLUE...........  x   x       \_______/           \______              |
|  -40m BLUE rear ..........................x......................                 |
|         14:00      14:10      14:20      14:30      14:40      14:50              |
```

Signed distance to the contested surface on y, time on x, one faint line per avatar coloured by faction, deaths as x. This is the "unbent" space: a push is a whole bundle of lines crossing zero together, a collapse is a bundle snapping back, and the person who is always 8 m ahead of their own bundle is visible without any leaderboard. Clicking a crossing loads that minute.

### 4.3 Sides Matrix tab

```
+= MATRIX  window [14:20 - 14:40]  half-life 90s ===================================+
| sort [by faction v]  cell [signed A v]  [x] hide unengaged   click cell = assert   |
|                    O K M T A | J R S P | Q Z |                                    |
|  RED Orion S.      # + + + ~ | X X - . | . . |  legend                            |
|      Kira V.       + # + + + | X - X . | . . |   #  self                          |
|      Mara D.       + + # ! + | - X X . | . . |   +  ally evidence                 |
|      Tam O.        + + ! # + | X X - . | ~ . |   ~  weak / thin                   |
|      Ash R.        ~ + + + # | . - . . | . . |   .  no evidence                   |
|  BLU Jax P.        X X - X . | # + + + | . . |   -  weak hostile                  |
|      Rue S.        X - X X - | + # + + | X . |   X  strong hostile                |
|      Sol T.        - X X X . | + + # + | . . |   !  intra-side hostility          |
|      Pim L.        . . . . . | + + + # | . . |                                    |
|  ??  Quill A.      . . ~ ~ . | . X . . | # ~ |  Quill: thin evidence (2 bursts)   |
|      Zed M.        . . . . . | . . . . | ~ # |  Zed:  no combat contact           |
+-----------------------------------------------------------------------------------+
| selected cell: Mara D. <-> Tam O.  A=+2.1 with one hostile burst 14:27:11 (12 dmg, |
| explosive, no reciprocity, shared targets recovered)  -> FRIENDLY FIRE (0.81)      |
| [Open burst] [Assert SAME] [Assert DIFFERENT] [Show in world]                      |
+-----------------------------------------------------------------------------------+
```

The block structure *is* the team detection, shown raw. Off-diagonal `!` cells inside a block are friendly fire; a person whose row does not match their block is a mis-assignment you can see before the machine admits it. A grid of coloured rects with a one-character glyph is trivially XUI-feasible (100×100 worst case, ~10k rects, drawn only when the tab is front), stable in a way no force-directed node graph would be, and directly clickable for assertions.

### 4.4 Sweep tab (rung 3)

```
+= SWEEP  ==========================================================================+
| axis: (*)tau ( )half-life ( )evidence ablation                                     |
|   tau .10      tau .25      tau .40*     tau .55      tau .70      tau .85         |
|  [2 sides ]   [2 sides ]   [3 sides ]   [4 sides ]   [7 sides ]   [19 singles]     |
|  |=====|      |=====|      |====|       |===|        |==|         |.|              |
|  |=====|      |=====|      |===|        |==|         |=|          |.|              |
|                            |==|         |=|          |.|          |.|              |
|  VERDICT: two-sided reading is stable across tau 0.18-0.47 (29% of range).         |
|           The GREEN split appears only above tau 0.33 -- it is a real split, but    |
|           a weak one; 4 people, 61% mean confidence.                                |
| ablation:  drop group tags -> same 3 sides (structure survives)   [adopt]           |
|            drop splash dmg -> 2 sides (the GREEN split was partly explosive FF)     |
|            drop proximity  -> Quill A. becomes unaligned                            |
+-----------------------------------------------------------------------------------+
```

Six mini-ribbons, each a stack of bars. The ablation rows are the honesty engine: they say out loud which conclusions depend on which evidence, which is exactly what an officer needs before disciplining anyone.

### 4.5 Inspector — Person page

```
+= INSPECTOR ========================================== [<] [>] [pin] [x] =+
| Session > RED > burst 14:31 > Kira Vex                                    |
+---------------------------------------------------------------------------+
| KIRA VEX          RED  conf .86  solid    joined RED 14:07 (from unaligned)|
| allegiance  ????|~~~~|====|====|====|====|====   [assert...] [why?]        |
+---------------------------------------------------------------------------+
| COMBAT     kills 7  deaths 3  dmg out 1,904  dmg in 1,210 (x0.78 adjusted) |
| TEAMPLAY   focus-fire share 41% (RED median 28%)   support 12 assists      |
|            friendly fire  1 given (accident .81) / 0 taken                 |
|            front lead     mean -2.1m behind line   spawn->front 41s        |
| BEHAVIOUR  mouselook 62% of engaged time   reaction 210ms (p54 of peers)   |
|            pre-turn while BLOCKED: 1 of 34 engagements (p61)  no flag      |
| KIT        Kestrel AR (weapon) | ARMOR-3 (HUD, x0.75) | Wasp mine (deploy) |
+---------------------------------------------------------------------------+
| REFERENCED BY  4 deaths as victim | 7 as killer | burst 14:27 (FF) |        |
|                assertion #2 (SAME as Mara D., you, 14:33)                   |
| [Show in world] [Follow track] [Open faction RED] [Compare with...]         |
+---------------------------------------------------------------------------+
```

Every number in the page is a link: "friendly fire 1 given" opens the burst; "p54 of peers" opens the peer distribution with Kira's mark on it; the allegiance sparkline is scrubbable and drives the cursor.

### 4.6 Inspector — Burst page (the relationship verdict)

```
| Session > 14:27:11 burst > Mara D. -> Tam O.                              |
| INTRA-SIDE HOSTILITY  RED -> RED   12.0 dmg / 2 hits / 1.4s / explosive    |
| verdict  FRIENDLY FIRE  .81      DEFECTION .07     SKIRMISH .12            |
|   reciprocity        none in +-30s                        -> accident      |
|   damage type        explosive (splash, discounted x0.35)  -> accident      |
|   aftermath          shared targets J .62 -> .59 (recovered) -> accident    |
|   cluster response   no member moved sides within 120s      -> accident     |
|   third party        Jax P. (BLUE) died 0.6s earlier at 3.1m -> plausible   |
|                      grenade overlap                                        |
| [Show in world] [Open Mara D.] [Open Tam O.] [Open Wasp mine] [Dispute...] |
```

Four scored tests, each shown with its evidence and its direction. `[Dispute]` files an assertion and records who filed it.

### 4.7 Ledger tab (assertions and their consequences)

```
| #  time     assertion                     by     effect on solve            |
| 1  14:33:02 SAME  Kira V. = Mara D.       you    +0 moved, conf .71 -> .74  |
| 2  14:41:20 DIFFERENT Quill A. != RED     you    2 moved, 1 new FF flag,    |
|                                                   overrules 3 bursts (open) |
| 3  14:52:10 NAME  GREEN = "Ashguard 2nd"  you    cosmetic                   |
| [undo] [export briefing notes]  contradictions: 1  [review]                 |
```

An assertion is data, not a mutation: it is listed, undoable, attributed, and it always reports what it changed and what evidence it overrules. Nothing else in the tool is allowed to silently disagree with the officer, and the officer is never allowed to silently disagree with the evidence.

Remaining tabs are conventional and shallow on purpose: **Roster** (sortable list of everyone with faction, confidence badge, kills/deaths, FF count), **Events** (the filterable firehose), **Kit** (equipment table with faction usage columns, adjustment ratio, through-wall tally).

## 5. In-world overlay

Drawn only under the fixed gate (stationary + alt-cam + 5 s after mouselook/OTS). Three layers, matched to the ladder.

**Rung 0 — bodies.** Each present avatar gets an **allegiance halo**: a ring at the feet, 0.6 m radius, divided into wedges by the faction posterior. A confident member is one solid arc; a 60/40 avatar is literally two-tone; a thin-evidence avatar is a dashed grey ring. This one glyph carries the whole social model into the world. Above it: a short yaw arrow (direction is geometry, never colour), a SansSerifSmall drop-shadowed name, an eye glyph while `AGENT_MOUSELOOK` is set, and a crouch/air tick.

**Rung 1 — the exchange.** Damage lines from attacker track to victim track, coloured by the *attacker's faction* with brightness by damage, plus a relationship style: solid = cross-faction, double dashed with a caret = intra-faction (friendly-fire suspect), heavy magenta = confirmed defection strike. Trails over the window ramp alpha old→new, go thin and dashed where the track came from coarse locations, and — the key touch — a trail whose owner changed faction inside the window is drawn as a colour gradient at the crossing time, so you *see* the moment someone switched. Deaths persist as cross+ring at `target_pos` with a killer line from `source_pos`.

**Rung 2 — the ground.** A 4 m grid of translucent quads on the terrain: each cell tinted by the faction with the highest presence influence over the window (Gaussian-weighted occupancy), alpha proportional to the margin, and hatched where the margin is under threshold — that hatch *is* the contested line, the same surface the Front chart measures against. Spawn cohorts get a labelled ring where respawn points cluster.

**Uncertainty in world** uses the same three states as 2D: solid, dashed-grey (thin), two-tone (conflicted). LOS candidates when a death is selected draw as green (clear) / red (blocked) rays with the blocking hit point marked, one ray per candidate position in the travel window, so the officer sees the *sweep*, not a verdict.

**Clicking** (plain left-click, no modifier, consumed only when the overlay is live): halo → Person page; damage line → Burst page; death cross → Death page; ground cell → Place page; spawn ring → Cohort page. Shift-click two halos → the assert dialog pre-filled with SAME/DIFFERENT.

## 6. Analysis model

**Bursts.** DAMAGE events with the same (owner, target) and gaps ≤ 3 s collapse into one burst with total damage `D`, hit count `n`, dominant type. Bursts, not events, are the evidence unit — a single stray splash tick must never carry the weight of an execution.

`h_burst = w_type · log(1+D) · min(1, n/3)` where `w_type` = 1.0 for direct types (−1, 0, 1..14, 104), 0.35 for explosive/crushing (102/103, splash is friendly-fire prone), 0.6 for suffocation (105). Types 100/101/106 (medical, repair, redeploy) invert: they emit affinity `log(1+D)`.

**Hostility** `H(a→b, t) = Σ h_burst · 2^(−(t − t_burst)/T½)`, `T½` = 90 s (slider; the Sweep varies it).

**Affinity**, same decay, from five sources: shared target (bursts by a and b on the same victim within 8 s: `+0.5·min(h_a,h_b)` — focus fire is the strongest ally signal in FFA and needs no group tag); proximity-under-fire (`+0.02`/s while within 12 m, at least one engaging a third party, neither engaging the other); co-riding a vehicle (`+0.2`/s, capped); heal/repair damage types; spawn cohort (respawn positions clustered at 15 m within 60 s: `+1.0` once). Shared active group id contributes a `+0.5` **prior**, explicitly tagged as a prior and ablatable in the Sweep, because the intra-group case makes it a lie.

**Field** `A(a,b,t) = Aff − H`, symmetrised as `A_sym = A(a,b) + A(b,a)`; the asymmetry is retained separately to surface one-sided aggression (bullying, executions of non-combatants).

**Factions** = signed correlation clustering: maximise `Σ_{same} A_sym − Σ_{diff} A_sym` by greedy moves plus local search, warm-started from the previous snapshot so labels stay continuous and bands do not flicker. Solved every 10 s and at every death, over avatars present in the window. Cluster identity across snapshots is matched by membership overlap, producing a **lineage** of BIRTH / JOIN / LEAVE / SPLIT / MERGE / DISSOLVE events — the ribbon's forks and diagonals are literally this list.

**Confidence.** `s(x,F) = Σ_{y∈F} A_sym(x,y) / sqrt(|F|)`; margin `M = (s(x,F1) − s(x,F2)) / (|s1|+|s2|+ε)`; evidence mass `E(x) = Σ_y |A_sym(x,y)|`. `conf = clamp01(M) · min(1, E/E0)`, `E0` ≈ three bursts. The two factors are kept separate in the UI because they mean different things: low `M` = conflicted (stripe), low `E` = ignorant (hatch).

**Structure index** `S = (Σ_within A⁺ + Σ_across |A⁻|) / Σ|A_sym|`, compared against `S_null` from 20 size-preserving random partitions. `S − S_null < 0.08` sustained for 60 s = DEATHMATCH: the tool stops naming factions and falls back to pairwise hostility.

**Intra-side hostility classification** (friendly fire vs defection vs skirmish) scores four tests: reciprocity within ±30 s; damage type (splash discounted); aftermath — shared-target overlap `J` in the 60–180 s after versus the 180 s before; cluster response — did the solve move anyone within 120 s. Friendly fire = small, non-reciprocated, `ΔJ ≥ −0.1`, no cluster response. Defection = cluster response plus `ΔJ < −0.3`. Skirmish/split = a cut with ≥3 members each side carrying >25% of the faction's internal weight. Otherwise DISPUTED, with all three scores shown.

**Exposure and non-aggression** (for Q8's social side): `Exposure(a,b)` = seconds both alive, within a's 95th-percentile observed hit distance, with a CLEAR LOS. `Spared(a,b)` = high exposure with near-zero hostility across a faction boundary — surfaced as an observation ("Rue S. had 96 s of clear exposure to Pim L. and never fired"), never as an accusation.

**Behaviour percentiles.** Reaction latency = time from a burst's first hit until the victim's yaw turns within 30° of the attacker or evasive acceleration begins; negative values are pre-turns. The suspicious combination is a pre-turn while LOS was BLOCKED, reported as a count and a peer percentile against avatars with comparable kit and engagement volume — never a verdict, always with the sample size and the travel-time caveat inline.

**Equipment and adjustments** attach to the social frame: each faction's Kit page lists items with adoption share, the mean adjustment ratio `damage/initial` with the adjusting script names from `modifications`, and the through-wall tally per item with its confidence. "RED absorbs 0.71× incoming, BLUE 0.98×" is a fairness fact about sides, which is what officers act on.

**Cost.** Bursts and affinity accumulate streaming, O(events). Snapshot solves run on ≤100 nodes every 10 s (sub-millisecond, off the draw path, budgeted one solve per frame). Matrix and Ribbon rebuild only when their tab is front and only for the visible window. Sweeps are on demand and cached by parameter tuple.

## 7. Walkthroughs

**Q1 — how did this raid go?** Open the floater; the Ribbon already shows the whole session. Read top to bottom: structure clean at the start, noisy 14:20–14:35, clean after; two bands become three at 14:24. Switch to Front mode: RED's bundle pushes past zero at 14:12, holds, collapses at 14:31 with six deaths at the crossing. Click the collapse → cursor lands there and the overlay shows the trails and the contested band moving. Fifteen seconds to the shape of the raid, one click back down to the ground.

**Q3 — why did Kira die at 14:31:58?** Click the death tick (ribbon) or the cross (world). Death page: killing blow, cumulative damage from the attribution window grouped by owner/rezzer, the weapon card, and the LOS sweep — six candidate shooter positions across the 1.5 s travel window, five BLOCKED, one CLEAR near the start, so the verdict reads "CLEAR at the earliest candidate; travel-time false positives are possible" rather than a through-wall accusation. Links up to the burst, sideways to the killer's Person page, down to "Show in world" which draws the rays.

**Q8 — did anyone behave suspiciously?** Roster sorted by the behaviour flag column. Zed M. shows two pre-turns while BLOCKED at p97. Open his Person page, click the percentile → peer distribution with his mark and n=34; click each flagged engagement → the LOS sweep for that one. Two of the three "wall" cases collapse when the sweep shows an early clear candidate. The remaining one is one event, which the page states plainly. Separately, the Matrix shows Zed's row as `.` against half of BLUE despite exposure — the Spared observation — worth a conversation, not a charge. The officer files a note; the tool never renders a verdict.

**Intra-group skirmish (two Ashguard teams).** Group tags are identical, so the group prior would make one blob. Open the Sweep, ablation row "drop group tags": the structure survives at 3 sides — the split is real, not a tag artefact. Adopt that parameter set; the Ribbon now forks at 14:24 into two bands, both unnamed. Matrix confirms a clean block cut with `!` cells only along the boundary. Rename the bands "Ashguard 1st" and "Ashguard 2nd" in place. Two people sitting on the boundary get pinned with SAME assertions; the Ledger reports mean confidence 0.58 → 0.74 and lists the three bursts the second assertion overrules, which the officer opens and agrees with.

**FFA on a neutral sim.** Structure band goes to noise inside three minutes; the bands grey out and the header prints DEATHMATCH. The Ribbon switches to per-person threads with no bands, the Matrix loses its blocks, and the halos become dashed grey. What still works: the Front view is disabled (there is no line), but the Place pages, kill-density clusters, Kit pages and per-person stats stay. Around 14:47 a stable positive block of four re-forms; the tool raises a band again and asks nothing — the officer names it. The important behaviour is the refusal: no fake teams during the deathmatch.

**Promotion and briefing (Q9).** Person page → Teamplay block: focus-fire share against the faction median, assists, spawn-to-front latency, mean distance to the line, friendly fire given with verdicts, plus the allegiance sparkline showing they never wavered; `[Compare with...]` pins two Person pages side by side. For the briefing, the lineage events plus the biggest bursts auto-generate a chapter list ("14:12 RED pushes the bridge", "14:24 split", "14:31 collapse"); stepping through it parks the cursor and the overlay at each, so the officer briefs by alt-camming.

## 8. Risks and open questions

- **The affinity model can be wrong.** Proximity-under-fire will bond two enemies who fight a third party from the same rooftop. Mitigation: proximity is the weakest term and is ablatable; the Sweep says out loud when a conclusion rests on it.
- **Focus-fire affinity is circular near the FFA boundary** (shared targets define allies, allies define targets). The warm-started solve damps oscillation but can lock in an early mistake; the Ledger's "what changed" report is the escape hatch.
- **Latency and missing positions** poison the aftermath test at the 1 s scale; all four intra-side tests are therefore scored, not thresholded, and shown together.
- **Calibration constants** (`E0`, the 3 s burst gap, `w_type`, the 0.08 deathmatch margin) are guesses until they meet a real raid; they must be settings, and the Sweep must cover the two that matter most.
- **Ribbon legibility above ~30 threads** per band: plan is a density fill with named threads only for pinned people, unresolved for 60-avatar fights. Halo wedges likewise clutter a dense spawn and may need a distance-based fade to one dominant colour.
- **Social risk**: the tool makes accusations cheap. Every suspicion surface shows sample size, peer distribution and the caveat inline, and no page prints the word "cheating" — but nothing stops an officer screenshotting a percentile out of context, so the share action should always export the evidence block with the number.
