# Entry 08 — Allegiance: combat as a social field

## 1. Thesis

A raid is not a list of damage events; it is a *relationship* between people that changes shape over an hour. Every question an officer actually asks — was that a violation or an accident, is this candidate a team player, did the line hold, who is that guy fighting for — is a question about sides. So the tool's primary object is not the event and not the avatar: it is the **allegiance field**, a signed, time-decaying, directed graph over avatars in which hostility subtracts and cooperation adds. Teams are never stored; they are *solved* from the field at a moment, with a confidence, and re-solved when the officer asserts something the machine got wrong. Every other analysis in the brief — equipment, armour adjustment, line of sight, suspicious behaviour — becomes a property *of a relationship or of a side*, because that is the frame in which it is actionable. The spine is one ladder over this field: a moment in world (rung 0), an engagement (rung 1), the session as braid, front and matrix (rung 2), and the session re-solved across the parameters of the inference itself (rung 3). Two promises hold it up. The tool is honest about not knowing: every derived quantity carries the evidence that produced it and switches itself off rather than guessing. And it volunteers its own doubts — an officer never has to already suspect the answer to be shown it, because rung 2 surfaces the anomalies that motivate climbing to rung 3.

## 2. Ladder

**Rung 0 — The Moment.** The in-world overlay at the cursor time: bodies, halos, yaw, the shot in flight. Concrete, one second wide.
*Step up*: `[` or the trail slider — the moment grows a tail and becomes an engagement. *Step down from anywhere*: every row, cell, tick and thread has "Show in world", which parks the cursor and flashes the actors.

**Rung 1 — The Engagement (burst).** A window (default 20 s) of trails, damage lines and deaths, participants named, with the burst's relationship verdict (cross-faction / friendly fire / defection). This is the unit the machine reasons about.
*Step up*: "Show in session" lights the burst as a tick on the Ribbon. *Step down*: scrub inside it; the overlay collapses back to a moment.

**Rung 2 — The Session, in three renderings of one solve (Ribbon / Front / Matrix).** All moments at once. These are not three views with three opinions: they read the same snapshot sequence from the same field under the same parameter set (§3) and differ only in projection. The **Ribbon** braids — bands are factions, threads are people, threads cross bands when allegiance changes, bands fork on splits, and the background texture is the *structure index*, how team-shaped the fight is at all. The **Front** unbends space: y is signed distance to the contested surface, so pushes and collapses are one glance. The **Matrix** cross-tabulates: the pairwise field as a grid, where the block structure *is* the clustering. Time, space, relation; nothing is computed twice.
*Step up*: `]`, or the **[sweep this]** affordance on any uncertainty badge, strain chip or matrix cell — you climb from the specific reading you distrust, which becomes the sweep's subject. *Step down*: click anywhere to set the cursor and load that minute.

**Rung 3 — The Inference (Sweep).** The session re-solved across the parameters of the analysis, as small multiples: seven confidence thresholds, three half-lives, and evidence classes ablated (group tags, splash, proximity, untrusted). The officer sees which readings are robust ("two-sided everywhere from tau 0.18 to 0.47") and which are one knob's artefact, each with a stability count rather than a yes/no. Same for LOS: a through-wall verdict is swept across the whole travel window, never asserted at one instant.
*Step up*: none — this is the top, and the design says so. *Step down*: click any small multiple to adopt that parameter set everywhere.

Nothing is a dead end: every abstraction cell knows which concrete events produced it and can list them.

## 3. Information architecture

**Two floaters.** `ss_combat_log` (Session: transport, Ribbon, Matrix, Sweep, Roster, Events, Kit, Ledger) and `ss_combat_inspector` (Page floater: one noun at a time, with history). This matches the fixed plan and refuses a floater zoo.

**Nouns (Inspector pages).** Person, Faction, Burst, Death, Damage, Equipment, Kit (an owner's loadout), Assertion, Moment, Place (a kill-density cluster the officer names: "Bridge"), Verdict (one LOS solve), Spawn cohort, and **View**. Every page has the same shape: header (what it is, plus the uncertainty badge), body, then *Links* (what it points at) and *Referenced by* (which bursts, factions, verdicts and assertions mention this thing). Backlinks are what make digging feel like Wikipedia rather than like drilling down a tree.

**One parameter set, globally.** `tau`, `half-life`, `window`, the ablations and `include untrusted` live in exactly one **ParamSet** owned by the session. The Ribbon's parameter strip *is* that object; Matrix and Front headers render the same values in the same widget, and an edit anywhere is an edit everywhere. Two rung-2 tabs can never disagree about teams "at the same moment" because there is one moment and one set of knobs. The Sweep is the only screen that shows more than one ParamSet, and it does so explicitly as comparison; adopting one there mutates the global set and every open view redraws.

**Two navigation histories, not one.** The Inspector has `[<] [>]` over the *noun* chain, a breadcrumb ("Session > RED > 14:31 burst > Kira Vex"), and `[pin]` to promote a page into a tab strip. The Session floater has its own `[<] [>]` over the *View* chain: the tuple (cursor, window, tab, mode/level, ParamSet, selection). Anything that moves the cursor or the parameters — a death tick, a Matrix cell, "Show in world" from four pages deep — pushes the previous View first, and `[mark]` names the current one as a Moment page. A multi-hop violation dig therefore ends with one click back to the framing it started from. The two stacks are drawn identically and never merged, because "where I was reading" and "what I was reading about" fail separately.

**One uncertainty vocabulary, everywhere.** Three states, drawn identically in ribbon, matrix, roster, halo and text: **solid** = confident; **grey hatch** = thin evidence; **two-tone stripe** = conflicting evidence (they fight both sides). Never a fourth encoding. Numbers never appear bare: a confidence is *word + two-part bar (margin | evidence) + `[why?]`*, and `[why?]` lists every constant in the chain with its current value.

**A degraded-terms strip.** When a term cannot be computed for this raid — no respawn signal, proximity budget exceeded, no single front — the status line prints a chip (`no-respawn`, `prox-capped`) and every confidence that would have used it is marked. The tool never silently loses a term.

## 4. Screens

### 4.1 Session floater — Ribbon tab

```
+= COMBAT LOG - Bad Space ============================ session 01:47:12 =[_][#][X]=+
| [<][>][mark] [*LIVE] |< << [>] >> >|  1x | -1s +1s | cur 14:31:58 win[20s]--o---- |
|  ( Ribbon )( Matrix )( Sweep )( Roster )( Events )( Kit )( Ledger )               |
+-----------------------------------------------------------------------------------+
| mode: (*)Threads ( )Front   tau 0.40 --o--  half-life 90s --o--  untrusted[off]    |
| structure  ####|####|####|##~~|~~~~|~~..|....|~~~~|####|####|####  S 0.71 null 0.42|
|      14:00     14:10     14:20     14:30 :    14:40     14:50     15:00           |
| RED  [solid|##] STRAINED 41% [test split]     tag-dependent (!)             (9)   |
|      =========================================:============================       |
|       ---------\      /--------------------- :  --------------------------        |
| BLUE [solid|##]                                                            (7)    |
|      ==========\====/=========\=============:=============================        |
| GRN  [thin |#.]                  \  split    :                        (4) "2nd"   |
|                                   \==========:========================            |
| ????  ~~~~~~~~~~~~~~~~~~ unaligned / new arrivals ~~~~~~~~~~~~~~~~~~~ (3)          |
| deaths  x   x x    xx  x    x  x   x  xx  x  x|x   x    x   x    x                 |
| ff/def      ^                    ^^ ^         :        ^                          |
+-----------------------------------------------------------------------------------+
| 14:31:58 DEATH  Kira Vex  <- Orion Starchild  Kestrel AR   32.1m  LOS: BLOCKED(?)  |
| 14:31:57 DMG    Kira Vex  <- Orion Starchild  18.0 (24.0 x0.75 ARMOR-3)  type 0    |
| 14:31:55 DMG    Kira Vex  <- Orion Starchild  22.0                       type 0    |
+-----------------------------------------------------------------------------------+
| rec ON | bridge 44ms | 38 agents | 12,884 ev | 3 assertions | conf solid 24/38     |
| degraded: no-respawn                                                              |
+-----------------------------------------------------------------------------------+
```

*Transport bar*: View history, mark, live, transport, cursor, and the trail-window slider that also drives the overlay — the rung 0↔1 control, so the most prominent slider in the tool. *Parameter strip*: the global ParamSet, including the untrusted-evidence switch. *Structure band*: one glyph per 30 s, stripes → noise, both terms spelled out (`S 0.71 null 0.42`) because an abbreviation nobody can expand is not honesty; under 0.08 for a minute it prints DEATHMATCH and the bands grey-hatch. *Ticks*: deaths as x on the owning band, friendly-fire and defection carets on their own lane. *Detail list*: events at the cursor, always visible, so the abstraction never floats free of instances.

*Band headers* carry three things beyond name and headcount. The **confidence badge** (word + bar). The **strain meter**: the share of the band's internal pair weight that is negative, printed as `STRAINED nn%` above 25% with a one-click `[test split]`. The **tag-dependence flag** `(!)`: every solve runs twice, with and without the group-tag prior (§6 — one extra solve on ≤100 nodes, not a new cost class), and the flag lights when they disagree about this band. Between them, an officer who has never heard the phrase "intra-group skirmish" is told in the default view that RED is internally hostile and that its shape depends on group tags. That is the whole discovery, and it arrives unasked.

*Bands and threads*: height by headcount, fill alpha by cohesion, name editable in place. Threads decimate to one point per pixel column; above a budget (default 40 visible threads, a slider) they become a stacked density fill with named lines only for pinned or flagged people, so the officer trades legibility against completeness rather than the tool doing it silently. Implementation: one custom `LLView` of rects, lines and `SansSerifSmall`, marked dirty every frame the cursor moves (the `RenderUIBuffer` dirty-rect gotcha in `doc/viewer/ui_system.md` bites animated custom views).

### 4.2 Front mode (same tab, radio switch)

```
| mode: ( )Threads (*)Front   [contested band 12m]  level: (*)ground ( )+8m ( )all   |
|  +40m RED rear ...........................................................        |
|       ...RED............/‾‾‾\......................                               |
|  +10m ........./‾‾‾‾‾‾‾‾      ‾‾‾\......                        push  collapse    |
|   0m  ==CONTESTED=======================\=========/‾‾‾‾‾‾‾‾‾\====================  |
|  -10m ......BLUE...........  x   x       \_______/           \______              |
|  -40m BLUE rear ..........................x......................                 |
|         14:00      14:10      14:20      14:30      14:40      14:50              |
|  level +8m: 3 avatars, own front   [show levels stacked]                           |
```

Signed distance to the contested surface on y, time on x, one faint line per avatar coloured by faction, deaths as x. A push is a bundle crossing zero together, a collapse is a bundle snapping back, and the person always 8 m ahead of their own bundle is visible without a leaderboard. Occupancy is computed **per 4 m vertical level** (§6), so a balcony fight is its own strip instead of being smeared into the floor below; `[show levels stacked]` turns Front into small multiples, one per occupied level. When no single contested contour survives, Front refuses to draw and says why.

### 4.3 Sides Matrix tab (rung 2, cross-tabulated)

```
+= MATRIX  window [14:20 - 14:40]  tau 0.40  half-life 90s  (global set) [edit] ====+
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
| selected: Mara D. <-> Tam O.  A=+2.1 (of which group prior +0.5, capped)           |
| one hostile burst 14:27:11 (12 dmg, explosive, no reciprocity)                     |
| -> FRIENDLY FIRE leading, 3 of 4 tests agree  [why?]                               |
| [Open burst] [Assert SAME] [Assert DIFFERENT] [Test exposure] [Sweep this] [World] |
+-----------------------------------------------------------------------------------+
```

The block structure *is* the team detection, shown raw. Off-diagonal `!` cells inside a block are friendly fire; a row that does not match its block is a mis-assignment you can see before the machine admits it. The header shows the global ParamSet read-only, with `[edit]` focusing the Ribbon's sliders — one control, two places. A grid of coloured rects with one-character glyphs is trivially XUI-feasible (100×100 worst case, ~10k rects, drawn only when front) and, unlike the Ribbon, degrades by sorting and filtering rather than by legibility collapse as headcount grows.

### 4.4 Sweep tab (rung 3)

```
+= SWEEP   subject: RED band 14:20-14:40  (from [test split]) =======================+
| axis: (*)tau ( )half-life ( )evidence ablation                                     |
|   tau .10      tau .25      tau .40*     tau .55      tau .70      tau .85         |
|  [2 sides ]   [2 sides ]   [3 sides ]   [4 sides ]   [7 sides ]   [19 singles]     |
|  |=====|      |=====|      |====|       |===|        |==|         |.|              |
|  |=====|      |=====|      |===|        |==|         |=|          |.|              |
|                            |==|         |=|          |.|          |.|              |
|  VERDICT: two-sided reading stable across tau 0.18-0.47 (29% of range).            |
|           GREEN split appears only above tau 0.33 -- real but weak; 4 people.      |
| ablation           outcome                 agreement over 20 perturbed solves      |
|  drop group tags   same 3 sides            17/20  (evidence mass 41 bursts: ok)    |
|  drop splash dmg   2 sides                 19/20  (GREEN split was partly FF)      |
|  drop proximity    Quill A. unaligned      20/20                                   |
|  +untrusted        same 3 sides            20/20  (3 untrusted events, no effect)  |
| [adopt this set]                                                                   |
```

Six mini-ribbons, each a stack of bars. The ablation rows are the honesty engine, and each reports an **agreement count** rather than a claim: the solve re-runs 20 times with affinity weights jittered ±10% and tie-break order shuffled, and the row says how often the outcome held. At 11/20 it prints "inconclusive — not enough evidence to test this", which is the outcome an officer most needs and the one a binary answer would hide.

### 4.5 Roster tab

```
| sort [flags v]  [x] only flagged                                                   |
| name          side  conf        K/D   FF  flags                    exposure        |
| Zed M.        ??    thin  |#.|  6/1   0   pre-turn-blocked x2 (p97) reaction p88   |
| Rue S.        BLUE  solid |##|  4/3   1   -                        spared: Pim L.  |
| Kira V.       RED   solid |##|  7/3   1   -                        -               |
```

The **flags** column is Q8's entry point and therefore specified, not assumed into existence by a walkthrough. A flag is a named behavioural observation with a peer percentile and sample size (§6): `pre-turn-blocked`, `reaction`, `through-wall-kills`, `speed`. Sorting by flags orders by the most extreme percentile with n ≥ 6; rows whose peer set is too small to rank sort last and print `n<6, no percentile`. Clicking a flag opens the Person page at that flag's evidence. Nothing here is a verdict and no cell is ever red. **Events** (filterable firehose, untrusted rows chipped) and **Kit** (faction usage, adjustment ratio, through-wall tally) are conventional and shallow on purpose.

### 4.6 Inspector — Person page

```
+= INSPECTOR ========================================== [<] [>] [pin] [x] =+
| Session > RED > burst 14:31 > Kira Vex                                    |
+---------------------------------------------------------------------------+
| KIRA VEX          RED  solid  |##|margin |###|evidence  [why?]             |
|                   joined RED 14:07 (from unaligned)                        |
| allegiance  ????|~~~~|====|====|====|====|====   [assert...] [sweep this]  |
+---------------------------------------------------------------------------+
| COMBAT     kills 7  deaths 3  dmg out 1,904  dmg in 1,210 (x0.78 adjusted) |
|            +212 dmg to objects (not counted toward allegiance)             |
| TEAMPLAY   focus-fire share 41% (RED median 28%)   support 12 assists      |
|            friendly fire  1 given (accident, 3 of 4 tests) / 0 taken       |
|            front lead     mean -2.1m behind line   spawn->front n/a        |
| BEHAVIOUR  mouselook 62% of engaged time   reaction 210ms (p54, n=19)      |
|            pre-turn while BLOCKED: 1 of 34 engagements (p61, n=19) no flag |
| EXPOSURE   opportunity 96s vs Pim L. (untested)  [run LOS sweep ~40 rays]  |
| KIT        Kestrel AR (weapon) | ARMOR-3 (HUD, x0.75) | Wasp mine (deploy) |
+---------------------------------------------------------------------------+
| REFERENCED BY  4 deaths as victim | 7 as killer | burst 14:27 (FF) |        |
|                assertion #2 (SAME as Mara D., you, 14:33)                   |
| [Show in world] [Follow track] [Open faction RED] [Compare with...]         |
+---------------------------------------------------------------------------+
```

Every number is a link: the friendly-fire line opens the burst; a percentile opens the peer distribution with Kira's mark, the peer-set rule and n; the allegiance sparkline is scrubbable and drives the cursor. Two honesty details are baked into the layout: `spawn->front n/a` because this raid produced no respawn signal (§6), and exposure shown as an **untested opportunity** with the cost of testing it printed on the button.

### 4.7 Inspector — Burst page (the relationship verdict)

```
| Session > 14:27:11 burst > Mara D. -> Tam O.                              |
| INTRA-SIDE HOSTILITY  RED -> RED   12.0 dmg / 2 hits / 1.4s / explosive    |
| verdict  FRIENDLY FIRE (3 of 4)   DEFECTION (0 of 4)   SKIRMISH (1 of 4)   |
|   reciprocity        none in +-30s                        -> accident      |
|   damage type        explosive (splash, discounted x0.35)  -> accident      |
|   aftermath*         shared victims J .62 -> .59 (leave-one-out) -> accident|
|   cluster response   no member moved sides within 120s      -> accident     |
|   third party        Jax P. (BLUE) died 0.6s earlier at 3.1m -> plausible   |
|                      grenade overlap                                        |
| * aftermath uses raw shared-victim sets with this pair's own bursts and     |
|   their derived affinity excluded; it stays correlated with that term, so   |
|   it is scored, never decisive alone.                                       |
| [Show in world] [Open Mara D.] [Open Tam O.] [Open Wasp mine] [Dispute...] |
```

Four scored tests, each with its evidence and direction, never collapsed into one word without the count. `[Dispute]` files an assertion and records who filed it.

### 4.8 Ledger tab

```
| #  time     assertion                     by     effect on solve            |
| 1  14:33:02 SAME  Kira V. = Mara D.       you    +0 moved, 22->24 solid     |
| 2  14:41:20 DIFFERENT Quill A. != RED     you    2 moved, 1 new FF flag,    |
|                                                   overrules 3 bursts (open) |
| 3  14:52:10 NAME  GREEN = "Ashguard 2nd"  you    cosmetic                   |
| [undo] [export briefing notes]  contradictions: 1  [review]                 |
```

An assertion is data, not a mutation: listed, undoable, attributed, always reporting what it changed and what evidence it overrules. Nothing else in the tool may silently disagree with the officer, and the officer may never silently disagree with the evidence.

## 5. In-world overlay

Drawn only under the fixed gate (stationary + alt-cam + 5 s after mouselook/OTS). Three layers matched to the ladder — and, because the brief says alt-camming is the *primary* mode, an explicit LOD rule that decides which layer owns the pixels.

**LOD by camera distance to the drawn thing** (thresholds are settings):

| layer | ≤ 40 m | 40–120 m | > 120 m ("helicopter") |
|---|---|---|---|
| halo | wedge ring, radius `max(0.6 m, 8 px)` | single dominant-colour disc, 5 px, uncertainty as dashed / two-tone edge | not drawn |
| label | name + flags | name only, for pinned / flagged / dead / top-damage, capped at 12 on screen | not drawn |
| damage lines | all in window | top 40 by damage, width in 3 buckets | one aggregate arc per ordered faction pair |
| trails | 1 vertex per 2 screen px | 1 vertex per 4 px | not drawn |
| LOS fan | all candidates | 3 rays (first, first-blocked, last) | not drawn |
| ground tint | on | on | on — the only layer left, and it is the helicopter view |

The rule behind the table: **each rung owns a distance band**, so three layers never fight for the same pixels. Up close you are at rung 0 looking at bodies; far out you are at rung 2 looking at territory — the structure band rendered on the terrain. A one-line HUD caption ("rung 2 — territory; zoom in for bodies") states the current regime, because a view that silently drops glyphs is a lying view.

**Rung 0 — bodies.** The **allegiance halo**: a foot ring divided into wedges by the faction posterior. A confident member is one solid arc; a 60/40 avatar is literally two-tone; a thin-evidence avatar is a dashed grey ring. Above it a yaw arrow (direction is geometry, never colour), a drop-shadowed `SansSerifSmall` name, an eye glyph while `AGENT_MOUSELOOK` is set, a crouch/air tick.

**Rung 1 — the exchange.** Damage lines from attacker track to victim track, coloured by the *attacker's* faction, brightness by damage, styled by relationship: solid = cross-faction, double-dashed with a caret = intra-faction (friendly-fire suspect), heavy magenta = confirmed defection. Trails ramp alpha old→new and go thin and dashed where the track came from coarse locations; a trail whose owner changed faction inside the window is a colour gradient at the crossing time, so you *see* the switch. Deaths persist as cross+ring at `target_pos` with a killer line from `source_pos`.

**Rung 2 — the ground.** A 4 m grid of translucent quads, per vertical level (§6), tinted by the faction with the highest presence influence over the window, alpha by margin, hatched where the margin is under threshold — that hatch *is* the contested line the Front chart measures against. Only the level nearest the camera focus draws opaque; other occupied levels are thin outlines, so a multi-storey base does not stack into mud.

**Uncertainty in world** uses the same three states as 2D. LOS candidates draw green (clear) / red (blocked) with the blocking hit point marked — the officer sees the *sweep*, not a verdict.

**Clicking and declutter.** Plain left-click (consumed only when the overlay is live): halo → Person, damage line → Burst, death cross → Death, ground cell → Place, spawn ring → Cohort. Picks come from the screen-rect list in the fixed plan; when more than one candidate falls within 12 px — the staging-area case, thirty avatars in a heap — the click opens a **stack list** ("4 here: Kira V. RED · Mara D. RED · Tam O. RED · Quill A. ??") and selects nothing until the officer chooses. Shift-click-to-assert uses that list for each pick, so an assertion is never filed against the wrong overlapping avatar; the Matrix stays the reliable bulk path in a crowd, and the assert dialog records which route filed it.

## 6. Analysis model

**Bursts.** DAMAGE events with the same (owner, target) and gaps ≤ 3 s collapse into one burst with total damage `D`, hit count `n`, dominant type. Bursts, not events, are the evidence unit — a stray splash tick must never carry the weight of an execution.

**Two admission filters run before any burst reaches the field.** *Trust*: events whose `trusted` flag is false (script-written custom events, which the brief says are spoofable) get weight **zero** by default. They stay visible in Events and on pages with an `untrusted` chip, and `+untrusted` is a Sweep axis so an officer can see whether including them would change anything — a promotion or a violation must never rest on data any resident can forge, and a small nonzero weight is still an attack surface. *Target kind*: bursts whose target is not an agent (the store's `targetIsAgent` flag) never contribute hostility or affinity — shooting a wall is not a social act — and are tallied as structure damage on Person and Kit pages.

`h_burst = w_type · log(1+D) · min(1, n/3)`, `w_type` = 1.0 for direct types (−1, 0, 1..14, 104), 0.35 for explosive/crushing (102/103), 0.6 for suffocation (105). Types 100/101/106 (medical, repair, redeploy) invert into affinity `log(1+D)`.

**Hostility** `H(a→b, t) = Σ h_burst · 2^(−(t − t_burst)/T½)`, `T½` = 90 s (global ParamSet).

**Affinity**, same decay, from five sources.
*Shared target*: bursts by a and b on the same victim within 8 s, `+0.5·min(h_a,h_b)` — focus fire is the strongest ally signal in FFA and needs no group tag.
*Proximity-under-fire* is the one term not keyed to events, so it gets a stated bounded algorithm rather than a hope. It is never evaluated over the whole session: each burst opens an **engagement window** of ±15 s, whose union is typically 10–25% of a raid. Inside a window only, tracks are sampled on a fixed 2 Hz grid; at each tick the present agents go into a uniform spatial hash with 12 m cells, and only pairs in the same or adjacent cells are tested (within 12 m, at least one engaging a third party, neither engaging the other) at `+0.02`/s. Cost per tick is O(k + p) for k agents near fire and p genuinely-near pairs — in SL fights k rarely exceeds 30, p a few dozen — and the accumulator advances incrementally as bursts arrive, never recomputed. On overrunning a hard budget (default 20k pair-tests per session-second) the term is skipped for that window, `prox-capped` shows in the degraded strip, and every confidence it fed is marked. This is the weakest term and the Sweep's first ablation.
*Co-riding*: `+0.2`/s capped, from `sitting/parent` in the tracks. *Heal/repair types*, as above.
*Spawn cohort*, **conditional on the raid having one**. No respawn event exists in the data, so it is inferred: the first track sample of an avatar within 30 s after its own DEATH, more than 20 m from the recorded `target_pos`, or reappearing beyond 20 m after a track gap ≥ 3 s. Fewer than 8 such discontinuities in the first 10 minutes means this raid's death mechanic does not relocate people: the term switches off, `no-respawn` shows in the degraded strip, the Person page prints `spawn->front n/a`, no cohort rings are drawn. Where the signal exists, positions clustered within 15 m over 60 s form a cohort: `+1.0` once.
*Group tag* contributes a `+0.5` **prior** — labelled as one, capped at 30% of a pair's `|A|` once real evidence exists, ablatable, reported separately in the Matrix readout. Every solve also runs a second time with the prior removed, and disagreement lights the band's tag-dependent flag (§4.1). A second solve on ≤100 nodes costs what the first did, sub-millisecond and off the draw path, which is why this is a default background check and not something the officer must think to request.

**Field** `A(a,b,t) = Aff − H`, symmetrised `A_sym = A(a,b) + A(b,a)`; the asymmetry is retained separately to surface one-sided aggression.

**Factions** = signed correlation clustering: maximise `Σ_{same} A_sym − Σ_{diff} A_sym` by greedy moves plus local search, warm-started from the previous snapshot so labels stay continuous, solved every 10 s and at every death over avatars present in the window. Identity across snapshots is matched by membership overlap, producing a **lineage** of BIRTH / JOIN / LEAVE / SPLIT / MERGE / DISSOLVE events — the Ribbon's forks and diagonals are literally this list. Because this is a tie-prone heuristic, every lineage event carries an agreement count from 20 jittered re-solves; a fork appearing in 12 of 20 draws is hatched and labelled "unstable", never drawn as a crisp topology change.

**Confidence.** `s(x,F) = Σ_{y∈F} A_sym(x,y) / sqrt(|F|)`; margin `M = (s(x,F1) − s(x,F2)) / (|s1|+|s2|+ε)`; evidence mass `E(x) = Σ_y |A_sym(x,y)|`; `conf = clamp01(M) · min(1, E/E0)` with `E0` ≈ three bursts. The two factors stay separate in the UI because they mean different things: low `M` = conflicted (stripe), low `E` = ignorant (hatch). The scalar is kept — officers sort and cite it — but never printed bare: always word + two-part bar, with `[why?]` listing `w_type`, `T½`, `E0`, the deathmatch margin and the burst gap at current values, so the reader can see how many constants deep it is.

**Structure index** `S = (Σ_within A⁺ + Σ_across |A⁻|) / Σ|A_sym|` against `S_null` from 20 size-preserving random partitions. `S − S_null < 0.08` for 60 s = DEATHMATCH: the tool stops naming factions and falls back to pairwise hostility. In that mode the Ribbon does *not* attempt 100 per-person threads; it becomes a **hostility heat strip** — rows are people sorted by activity, one cell per 30 s, colour = hostility emitted minus received. The Matrix's temporal cousin: it scales to 100 rows as easily as 10 and keeps the per-person story readable once the team story has evaporated.

**Front surface extraction.** The occupancy grid is 2.5D: cells are `(x, y, level)` with level = `floor(z/4 m)`, presence Gaussian-weighted per level. Per level, the margin field `m = p_A − p_B` is contoured by marching squares at `m = 0` and components shorter than 12 m are discarded as noise. Signed distance comes from a two-pass chamfer distance transform over that level's 64×64 grid (a few hundred microseconds, cached per window), signed by `m` — not from projecting onto a polyline, which is exactly where a branching or disconnected front breaks. A level yielding zero surviving components or more than three prints "no single front — territory is interleaved" and offers the ground overlay and Place pages instead. Levels are independent, so two avatars 2 m apart on stacked floors are correctly two fights. Front is an opinionated view that admits when its opinion does not apply, which beats a chart that always draws something.

**Intra-side hostility classification** (friendly fire / defection / skirmish) scores four tests: reciprocity within ±30 s; damage type (splash discounted); aftermath; cluster response within 120 s. Aftermath is **leave-one-out**: `J` is the Jaccard overlap of the pair's raw shared-victim sets 60–180 s after versus 180 s before, read from the event store with this pair's own bursts and their derived affinity excluded, so the classifier is not reading back the term it adjudicates. It stays correlated with that term — people who focus-fire together look allied by construction — so it is scored, never decisive alone, and the Burst page says so inline. Friendly fire = small, non-reciprocated, `ΔJ ≥ −0.1`, no cluster response. Defection = cluster response plus `ΔJ < −0.3`. Skirmish/split = a cut with ≥3 members each side carrying >25% of the faction's internal weight. Otherwise DISPUTED, all scores shown.

**Exposure and non-aggression** is **two-tier**, never a discovery sweep of raycasts. Tier 1 (cheap, always on): *opportunity* = seconds both alive and within a's 95th-percentile observed hit distance, computed inside the proximity engagement-window hash with no raycasts, yielding candidate pairs and the phrase "96 s of opportunity (untested)". Tier 2 (on demand only): `[run LOS sweep]` on one chosen pair samples those intervals at 1 Hz, caps the job at 120 rays, runs as a budgeted background task of ≤2 ms per frame with a progress line, and caches. The page then reads "clear exposure 61 s of 96 s tested (120 rays)". `Spared` is only ever stated after tier 2, and always as an observation.

**Behaviour percentiles.** Reaction latency = time from a burst's first hit until the victim's yaw turns within 30° of the attacker or evasive acceleration begins; negatives are pre-turns, and the suspicious combination is a pre-turn while LOS was BLOCKED. The **peer set** is explicit: avatars in this session with ≥8 scored engagements whose primary weapon shares the subject's projectile class; if n < 6, widen to all avatars with ≥8 engagements and label it "wide peer set"; if still n < 6, no percentile at all, only the raw value and the count. n is printed beside every percentile.

**Equipment and adjustments** attach to the social frame: each faction's Kit page lists items with adoption share, mean `damage/initial` with the adjusting script names from `modifications`, and the per-item through-wall tally with its confidence. "RED absorbs 0.71× incoming, BLUE 0.98×" is a fairness fact about sides, which is what officers act on.

**Cost, itemised rather than asserted.** Burst formation, hostility and the four event-keyed affinity terms stream in O(events). Proximity is O(engaged seconds × 2 Hz × (k + p)) under a hard budget. Snapshot solves are ≤100 nodes every 10 s, run twice (with and without the tag prior) plus 20 jittered re-solves amortised across frames, off the draw path, one solve per frame. Front extraction is one 64×64 contour plus distance transform per level per window, cached. Matrix and Ribbon rebuild only when front and only for the visible window. LOS is on demand, budgeted, capped per job. Sweeps are on demand, cached by parameter tuple.

## 7. Walkthroughs

**Q1 — how did this raid go, where, when, between whom?** The Ribbon already shows the session: structure clean at the start, noisy 14:20–14:35, clean after; two bands become three at 14:24. Switch to Front — RED's bundle pushes past zero at 14:12, holds, collapses at 14:31 with six deaths at the crossing. That is *when* and *between whom*. For *where*, click the contested band at the collapse: the **Place page** for the kill-density cluster there, already named "Bridge", with its deaths, the two Kit profiles that fought over it and its 40 m footprint. `[Show in world]` parks the cursor, and at helicopter distance the ground tint alone shows RED territory crossing the bridge and snapping back. Twenty seconds to the shape of the raid, one click down to the ground and one sideways to the place.

**Q3 — why did Kira die at 14:31:58?** Click the death tick or the world cross. Death page: killing blow, cumulative damage from the attribution window grouped by owner/rezzer, the weapon card, and the LOS sweep — six candidate shooter positions across the 1.5 s travel window, five BLOCKED, one CLEAR near the start, so the verdict reads "CLEAR at the earliest candidate; travel-time false positives are possible" rather than a through-wall accusation. That sentence shape is the house style for every probabilistic claim in the tool. Links up to the burst, sideways to the killer, down to "Show in world". `[<]` on the Session floater restores the 14:20–14:40 framing afterwards.

**Q8 — did anyone behave suspiciously?** Roster, sorted by the flags column (§4.5). Zed M. shows `pre-turn-blocked x2 (p97, n=19)`. His Person page: click the percentile for the peer distribution with his mark, the peer-set rule and n; click each flagged engagement for its own LOS sweep. Two of the three "wall" cases collapse when the sweep finds an early clear candidate; the third is a single event, which the page says plainly. Separately his Matrix row is `.` against half of BLUE despite tier-1 opportunity, so the officer runs the LOS sweep on that pair — 120 rays, two seconds — and reads "clear exposure 61 s of 96 s tested". Worth a conversation, not a charge. The officer files a note; the tool never renders a verdict.

**Intra-group skirmish (two Ashguard teams) — discovered, not assumed.** The officer opens the Ribbon knowing nothing. RED's header reads `STRAINED 41%` with a `(!)` tag-dependent flag: two fifths of RED's internal pair weight is hostile, and the background tag-free solve disagrees about who is in RED. Neither required a hypothesis. `[test split]` opens the Sweep with RED as subject and the group-tag ablation pre-run: `drop group tags → same 3 sides, 17/20 agreement, evidence mass 41 bursts: ok`. That is a stability report, not a promise — a weak case reads `11/20 — inconclusive, not enough evidence to test the tags` and offers to wait for more bursts rather than confirm a split. Satisfied, they `[adopt this set]`; the Ribbon forks at 14:24 into two unnamed bands, the Matrix shows a clean block cut with `!` cells only along the boundary, and the bands are renamed "Ashguard 1st" and "Ashguard 2nd" in place. Two people on the boundary get SAME assertions; the Ledger reports solid confidence rising from 22 to 24 of 38 and lists the three bursts the second assertion overrules, which the officer opens and agrees with.

**FFA on a neutral sim (80 agents).** The structure band goes to noise inside three minutes; bands grey out, the header prints DEATHMATCH, halos become dashed grey. The Ribbon does not attempt 80 threads — it becomes the hostility heat strip, one row per person, sorted by who emits the most hostility, which in a deathmatch is the actual story. The Matrix loses its blocks but keeps working (sort by hostility received; `hide unengaged` culls the tourists). Front disables itself: no contour survives, and it says so. Place pages, kill-density clusters, Kit pages and per-person stats all stay. Around 14:47 a stable positive block of four re-forms, `S − S_null` holds above the margin for a minute, and the tool raises one band — unnamed, asking nothing. The important behaviour is the refusal.

**Mid-fight arrival.** Pim L. arrives at 14:38 and opens fire nine seconds later. She sits in the `????` band with a grey-hatched halo: thin evidence, not "no team". Her first burst against a BLUE creates a hostility edge, but `E` is one burst so she stays hatched, and an officer alt-camming over sees a dashed grey ring beside four solid red ones — the honest picture. At her third burst `E` crosses `E0`, the solve moves her into RED, and her thread rides a diagonal out of the unaligned band at 14:39:20: a visible join, timestamped, clickable, undoable. In DEATHMATCH she would simply be a new row in the heat strip.

**Promotion and briefing (Q9).** Person page → Teamplay: focus-fire share against the faction median, assists, mean distance to the line, friendly fire given with its test counts, and the allegiance sparkline showing they never wavered; `[Compare with...]` pins two Person pages side by side. For the briefing, lineage events plus the biggest bursts auto-generate a chapter list ("14:12 RED pushes the bridge", "14:24 split", "14:31 collapse"); each chapter is a marked View, so stepping through parks cursor, window and parameters together and the officer briefs by alt-camming.

## 8. Risks and open questions

- **The affinity model can be wrong.** Proximity-under-fire will bond two enemies who fight a third party from one rooftop. It is the weakest term, budgeted and ablatable, and the Sweep says out loud when a conclusion rests on it.
- **Focus-fire affinity is circular near the FFA boundary** (shared targets define allies, allies define targets). Warm-starting damps oscillation but can lock in an early mistake; the jittered agreement counts expose it, the Ledger's "what changed" report is the escape hatch. The aftermath test has the same shape and is leave-one-out for that reason — still not fully independent, and the UI says so.
- **Calibration constants** (`E0`, the 3 s gap, `w_type`, the 0.08 margin, the LOD distances) are guesses until they meet a real raid. They are settings, the Sweep covers the two that matter most, and `[why?]` prints them beside any number they produced.
- **Ribbon legibility** is handled by budget rather than hope: density fills above 40 threads, heat strip in DEATHMATCH. Unresolved: a genuinely two-sided 60-agent fight, where the fill gives you the shape but individual threads are lost unless people are pinned.
- **Deliberate refusals.** Front draws nothing when no contour survives; percentiles vanish below n = 6; untrusted events carry zero weight; spawn-cohort affinity switches off when the raid has no respawn mechanic. Each will occasionally frustrate an officer who wanted an answer, and each is better than the answer they would have got.
- **Social risk**: the tool makes accusations cheap. Every suspicion surface shows sample size, peer set, distribution and caveat inline, and no page prints the word "cheating" — but nothing stops an officer screenshotting a percentile out of context, so the share action always exports the evidence block with the number.
