# Entry 01 — LADDER-FIRST: the rung is the app

## 1. Thesis

Most review tools hand you a pile of views and hope you build the ladder in your head. Here the ladder *is* the application. Six rungs, −1 to 4, from the raw received record up to the whole session; the officer stands on exactly one, looking at exactly one subject, and the two keys that matter are `[` (down, toward the concrete) and `]` (up, toward the abstract). Navigation is two-dimensional and orthogonal: **the ladder is vertical, the wiki is horizontal.** Vertical movement changes the level of abstraction and keeps the subject; horizontal movement changes the subject (avatar → weapon → team → death) and keeps the level. Every step is a named operator written into the breadcrumb, drawn differently depending on whether it changed your altitude or your subject. Two invariants hold it together. **Up-then-down is the identity**: ascending sets the destination's *anchor* to the instance you came from, so `[` returns that instance — not a lookalike. Only when you enter an aggregate cold (a link, a pin, a search) is there nothing to return to, and then `[` lands on the aggregate's **witness**, its medoid instance, and the rail says `witness`, not `back`. And the **haze rule**: no aggregate may be drawn without the unknown fraction it was built from drawn in the same mark. Nobody gets stranded in the clouds with a number they cannot check, or on the ground with a moment they cannot situate.

## 2. Ladder

Six rungs. Each names its **subject type**, its **step up** (the compression operator, which becomes a breadcrumb token) and its **step down** (the grounding gesture, which always lands on a *named* instance the rail printed before you pressed the key).

**R−1 — Record.** Subject: one received line — raw payload as it arrived, trust flag, sender/owner keys when untrusted, receive time, script `t`, clock offset, log lag, the `fd` dedup partner, and the salvage mask if the batch was truncated. Any claim can be run to ground in the literal bytes. *Requires one addition to the decided plan — §8, "Plan delta".*
*Up:* `interpret` → typed event at R0. *Down from R0:* `Ctrl+Enter`, or the raw chevron in the article header.

**R0 — Moment.** Subject: an instant `t` ± 1 s. The world overlay at the cursor plus the Moment strip (the events in the window, one line each). The visceral rung: bodies where they were, tracers as they flew.
*Up:* `over time` → the moment's bout. *Down:* pointing — click any world marker or strip row.

**R1 — Episode.** Subject: a *bout* (spatiotemporal cluster of fighting) or a *life* (spawn → death). Two faces, because "all moments at once" can collapse either axis: **Ledger** collapses space (avatars as rows, time across, volleys and deaths as marks) and **Plan** collapses time (every trail, line of fire and death cross at once). Plan has two renderers — **World** (the overlay) and **Chart** (a 2D top-down panel in the floater) — with identical encodings and picking, so a fight's geography survives the officer not being parked and alt-camming. Switching face or renderer is free and does not touch the breadcrumb: it is not a change of rung.
*Up:* `over parameters` → R2, or `over avatars`/`over bouts` → R3. *Down:* click any mark → its moment; bare `[` → the anchor, else the witness.

**R2 — Sweep.** Subject: one claim swept across one parameter. Not "was this a wall kill" but "here is the LOS verdict at every point in the 1.5 s travel window, and here is where it flips". Sweepable: travel time τ, attribution window, team-confidence threshold, awareness cone θ, script inclusion, track-quality cutoff, **and the segmentation constants** (ε, κ, the lull rule) that decide which bout a death belongs to. A sweep is a strip: parameter on x, outcome as colour, current value marked.
*Up:* `across cases` → R3, the same sweep small-multipled over every case of its kind. *Down:* click the strip → R0 at the moment that value implies (τ = 0.9 s puts the cursor at the killer's position 0.9 s before the hit, that one ray drawn).

**R3 — Field.** Subject: a set (deaths, avatars, weapons, bouts) across two or more dimensions. Four faces: **Hostility Matrix**, **Allegiance Braid**, **Front Chart** (space unbent into front-coordinate `u` vs time), **Board** (small multiples of the pins).
*Up:* `summarise` → R4. *Down:* every cell, band and dot is a handle; `Shift+click` a matrix cell lands on its medoid damage event at R0 with R1 context preloaded.

**R4 — Session.** Subject: the raid — phases, ledgers (avatars, teams, equipment, scripts), a **Ground** face (2D top-down heat and phase movement, same renderer as R1 Chart), headline counts with confidence gutters, and the questions the tool flags on itself ("3 deaths unattributable", "team split unresolved 14:44–14:51").
*Up:* none, and the rail says so. *Down:* every number, ledger row and phase band descends to its anchor or its witness.

The rail shows all six subjects *and the exact instance `[` will land on*, so descending is never a leap either: you read the destination before you press the key.

## 3. Information architecture

**Nouns** (each is an article, each has backlinks): Session, Phase, Bout, Moment, Life, Avatar, Team, Death, Damage, Volley, Equipment (Weapon / Projectile / HUD / Deployable / Vehicle / Mount), Script (a damage-adjustment script seen in `modifications`), Zone (a named spatial cell), Verdict (a stored LOS result with its sweep), Case (an officer-assembled bundle: pins + notes), Record.

**Volley** is the load-bearing noun the raw log lacks: consecutive DAMAGE events sharing `(owner, rezzer, target)` with gaps ≤ 2 s. It is what an officer means by "he shot him", and death attribution, equipment stats and the Bout Ledger all speak in it.

**Two floaters, one overlay.** `ss_combat_log` is the **Ladder** floater: fixed chrome (breadcrumb, rung rail, ribbon, transport) with a centre stage that swaps by rung — six stages, nothing duplicated. `ss_combat_inspector` is the **Article** floater, keyed by noun id so several can be open and compared; it is rung-agnostic, the horizontal axis.

**Linking.** Article body is an ordinary `LLTextEditor` with a registered `LLUrlEntry` for `secondlife:///app/sscombat/<type>/<id>`, so noun mentions in prose are real clickable links with tooltips, exactly like agent links today. Lists are scroll lists whose rows carry the same noun id.

**Every article carries a Rung Row** — six buttons `[-1]…[4]`, enabled only where that noun means something at that rung, each labelled with what you would see. The mapping is not per-noun improvisation; it follows one rule, so two implementers cannot disagree:

| rung | rule for any noun N | Avatar | Team | Weapon | Death | Script |
|---|---|---|---|---|---|---|
| R0 | N at the cursor instant | body, yaw, flags | members drawn | its shots in flight | the moment | its adjustments |
| R1 | the smallest episode(s) containing N | its life | Ledger filtered to members | its volleys | its bout | ρ per volley |
| R2 | N's **default claim**, swept | awareness lead vs θ | membership vs margin | through-wall vs τ | LOS vs τ | ρ vs inclusion set |
| R3 | the peer set N belongs to | its row+column in the Matrix, its Braid strand | Braid and Front | through-wall field across weapons | its matrix cell | ρ across teams |
| R4 | N's row in the session ledger | avatar ledger row | team ledger row | equipment ledger row | one tick in the counts | script ledger row |

Every noun's R2 has an ordered **claim list** drawn as chips; `]` opens the first, and switching chips changes claim without changing rung. That is what welds wiki to ladder: any noun, any rung, one click, one rule.

**History and anchors.** One shared back/forward stack across both floaters storing `(rung, subject, face, renderer, cursor, window)`; Backspace goes back. Separately, every `(rung, subject)` pair holds an **anchor**: the concrete instance last visited beneath it this session. `]` writes the anchor; `[` reads it. The two mechanisms answer different questions — Backspace retraces *your path*, `[` returns *this subject's* ground truth — and the rail labels which you are about to get.

**Pinning** (`Ctrl+click`, or `P`) drops a noun in the Pin tray, and the R3 Board draws one cell per pin on shared axes — that is how small multiples get built. Pins plus a note field make a **Case**, the artifact of the violation workflow, copyable to the clipboard as plain text for a group notice.

## 4. Screens

### 4.1 The Ladder floater — fixed chrome

```
┌ COMBAT LOG ─ 14:02–15:47 ─ ●REC ─ bridge ok ─ 41 agents ─ drop 0 ─ rtt 141ms ─┐
│ ◀ ▶ │[4]Session ⌄into [1]Bout "West Gate" ⌄anchor [0]DEATH Vex←Cadmus → Cadmus│
├──────┬──────────────────────────────────────────────────────────┬─────────────┤
│ RUNG │                                                          │ EVIDENCE    │
│  ▲ ] │                                                          │ ┌─────────┐ │
│[4]Ses│                                                          │ │ what is │ │
│[3]Fld│                    S T A G E                             │ │ this    │ │
│[2]Swp│              (swaps with the rung)                       │ │ built   │ │
│[1]Bou│                                                          │ │ from    │ │
│[0]Mom│  ← [ lands on: DEATH Vex←Cadmus  (back)                  │ └─────────┘ │
│[-1]Rc│                                                          │ coverage 87%│
│  ▼ [ │                                                          │ pins: 4  ▤  │
├──────┴──────────────────────────────────────────────────────────┴─────────────┤
│ deaths  ▏ ▏  ▏▏▏ ▏     ▏▏▏▏▏  ▏ ▏      ▏▏ ▏▏▏▏▏▏ ▏  ▏     ▏▏  ▏   ▏ ▏   ▏▏▏  │
│ dmg A/B ▁▂▃▅▇█▇▅▃▂▁▁▂▄▆█▇▄▂▁▁▁▂▅▇█▆▃▁▁▁▂▃▅▆▇█▇▆▄▂▁▁▁▂▃▄▃▂▁▁▁▁▂▃▅▇█▇▅▃▂▁▁▁▁  │
│ bouts   ▓▓▓▓▓░░░░▓▓▓▓▓▓▓▓░░░░░░▓▓▓▓▓▓▓▓▓▓▓░░░░▓▓▓▓▓▓▓▓▓▓▓░░░░░░░▓▓▓▓▓▓▓▓▓▓▓  │
│ quality ████████████▒▒▒▒████████████████▒▒▒▒▒▒███████████████████████████████ │
│         └──────────────────────[▮]══════════════════────────────────────────┘ │
│ ◀◀ ◀ ▶ ▮▮ ▶ ▶▶  0.25× 1× 4×  │ 14:31:07.4 │ ±20 s │ LIVE ○   ⌖ follow cursor  │
└───────────────────────────────────────────────────────────────────────────────┘
```

- **Breadcrumb row.** Vertical moves print as a tinted operator chip carrying a direction glyph and the destination rung (`⌄into [1]`, `⌃over avatars [3]`); horizontal moves print as a plain untinted `→` at the same indent. Altitude change and subject change never look alike. Clicking a token jumps there; clicking the operator opens its own explanation article.
- **Rung rail** (86 px). Six cells, abstract at top, ground at bottom, each showing the subject it holds; the cell below the current one prints the exact landing and whether it is `(back)` — the anchor — or `(witness)`. A cell greys when its subject is undefined and says what would define it.
- **Stage.** A `layout_stack` of six panels, one visible: custom `LLView`s drawing with `gl_rect_2d` + `LLFontGL::renderUTF8`, plus stock scroll lists — well inside XUI's immediate-mode budget.
- **Evidence column** (collapsible, 180 px). Always answers "what is this made of": inputs, sample counts, coverage, assumptions in force (windows, thresholds), each editable in place with live re-render. Right-clicking a parameter is the entry to R2: "sweep this".
- **Ribbon** (the constant). Death ticks by victim team; stacked damage-rate band per team; bout bands dithered at fuzzy edges; a track-quality row hatched below 60 % coverage. Drag scrubs, Shift-drag sets the R1 window, double-click a bout ascends to it. **The ribbon never changes; the stage always does** — it is the spine that makes vertical movement legible.
- **Transport.** Step ±1 event / ±1 s, play, speed, Live, and "follow cursor", which drives camera focus to the current subject.

### 4.2 Stage — R0 Moment

```
│  MOMENT  14:31:07.4      ⟨world overlay is the primary view — see §5⟩         │
│  ┌ within ±1.0 s ─────────────────────────────────────────────────────────┐   │
│  │ 07.1  DMG  Cadmus ──▶ Vex      18.0 (init 24.0 ρ.75)  ballistic   ⛨sc.2│   │
│  │ 07.2  DMG  Cadmus ──▶ Vex      18.0                    ballistic   ⛨sc.2│   │
│  │ 07.4  DEATH Vex   ← Cadmus     "Mk4 Carbine"   28.2 m   LOS ▒UNKNOWN   │   │
│  │ 07.6  DMG  Rook   ──▶ Ilse       6.0  explosive  ◇deployable?          │   │
│  └────────────────────────────────────────────────────────────────────────┘   │
│  selected: DEATH Vex ← Cadmus   [article ▸] [sweep LOS ▸] [raw record ▾]      │
```

Rows are concrete events; the selected one pulses in the world. Each row carries a time-source glyph (`t` script stamp, `fd` own-hit, `~` receive-time fallback), because ordering is only as good as its stamp. `▾` opens R−1 inline as a two-column table. Note the honest `▒UNKNOWN` where a lesser tool would print a green tick.

### 4.3 Stage — R1 Face A, the Bout Ledger

```
│ BOUT "West Gate"  14:29:12–14:36:40  6 deaths  1.4k dmg  ⟨Plan|Ledger⟩       │
│           14:29        :30         :31         :32         :33         :34   │
│ Cadmus B  ░░▓▓▓▓══╪═▓▓▓░░░░░░░░░▓▓▓▓▓▓╪▓▓░░░░░░░░░░░░░░░░░░░░░░▓▓▓▓╪░░░░░░  │
│ Rook   B  ░░░░░░░░░░░░▓▓╪▓▓▓░░░░░░░░░░░░░░✖▁▁▁▁▁▁▁▁▁▁▁▁▁░░░░░░░▓▓▓░░░░░░░░  │
│ +6 more B ▁▂▂▃▂▁▁▁▂▃▃▂▁▁▁▂▂▁▁▁▁▁▂▃▄▃▂▁▁▁▁▁▂▂▁▁▁▁▁▁▁▁▁▁▁▁▁▂▃▂▁▁▁▁▁▁▁▁▁▁▁▁  │
│ Vex    A  ▓▓▓▓╪▓▓░░░░░░░░░░░░✖▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▓▓▓╪▓▓▓░░░░░░░░░░✖▁▁▁▁▁▁  │
│ Ilse   A  ░░░░░░░░▓▓▓▓▓▓▓▓╪▓▓▓▓░░░░░⚠╪░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  │
│ Tam    ?  ····························▓▓▓╪▓░░░░░░░░········✖▁▁▁▁▁▁▁▁▁▁▁▁▁▁  │
│           ▓ volley  ╪ death-causing volley  ✖ death  ▁ dead  ⚠ friendly fire │
│           ░ alive, tracked   · no track (coverage gap)   +N = folded rows    │
```

One row per avatar; team letter and colour in the gutter, hatched when the assignment is tentative. Volley block height encodes damage rate. **Degradation is specified, not left to scrolling:** rows sort by team block then by involvement (damage dealt + taken + deaths); the top twelve draw individually, the rest fold per block into a `+N more` row whose strip is the summed damage-rate density, expandable in place. Any avatar with a death, a friendly-fire flag or a pin is force-promoted out of the fold, so a twenty-man bout still reads in one glance and nothing consequential hides. Every mark selects on click, descends on `Shift+click`, pins on `Ctrl+click`.

### 4.4 Stage — R1 Face B, Plan · Chart renderer

```
│ BOUT "West Gate" · PLAN ⟨World | Chart⟩   ▣ 128 m   N↑   ⊕ zoom  ✋ pan       │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │            ░░░░  ▄▄▄▄▄▄▄▄▄  wall                                       │  │
│  │      B ───────╮  ▀▀▀▀▀▀▀▀▀                    ░ death density          │  │
│  │      B ────────╯╲   ╱────── A       ✖ = death cross                    │  │
│  │             ✖ ✖  ╳ ╱      ░░░░      ─ trail (alpha = age)              │  │
│  │      B ──────────╳╱  ✖  ░░▒▒▒░░     ╱╲ fire lines (volleys)            │  │
│  │            ╱     ╲╲      ░░░░░      · · dashed = coverage gap          │  │
│  │      · · · ·      ╲──── A                                              │  │
│  │   (Tam, no track)                   ⌖ cursor 14:31:07.4                │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
│  region SS-Verdance · extent 96×112 m · coverage 87% ▨13% untracked          │
```

The **Chart** renderer is a custom `LLView` drawing the overlay's own primitives orthographically from above: trails, fire lines, death crosses, density quads, a 32 m region grid, north up, wheel zoom, drag pan, screen-rect picking identical to the world's. It exists because the overlay is gated to stationary + alt-cam, i.e. unavailable exactly when an officer is walking, briefing or re-gearing — and "where did the fighting happen" must not become unanswerable for that reason. **Parity rule: anything the overlay draws at rung R has a Chart twin with identical encodings; the overlay adds only depth, occlusion and a free camera.** R4's **Ground** face uses the same renderer (heat quads, staging rings, phase-centroid push arrows). Two renderers, one view — not a new pile.

### 4.5 Stage — R2 Sweep

```
│ SWEEP: line of sight for DEATH Vex ← Cadmus 14:31:07.4                        │
│ claim ⟨LOS τ | attribution window | awareness θ | segmentation ε⟩             │
│ parameter: bullet travel time τ (candidate shooter position = killer at t−τ)  │
│                                                                               │
│  τ=0.0   0.2   0.4   0.6   0.8   1.0   1.2   1.4 s                            │
│  ████████████▒▒▒▒▒▒▒▒░░░░░░░░░░░░░░░░░░░░████████    █ BLOCKED                │
│                        ▲ chosen 0.75                 ░ CLEAR  ▒ UNKNOWN       │
│  distance ──▶ 26.1  26.8  27.4  28.2  29.0  30.1  31.4  32.9 m                │
│  track src  B  B  B  V  V  V  C  C     (bridge / viewer / coarse)             │
│                                                                               │
│  verdict: MIXED — clear only for 0.5 s ≤ τ ≤ 1.1 s, where the killer had      │
│  stepped past the gate pillar. Not evidence of a wall kill.  [pin] [to case]  │
```

The sweep replaces the scalar verdict everywhere one would have been shown; verdict badges in lists are 8 px miniatures of this strip, so flip-flopping is visible before a word is read. The claim chips make the *segmentation* constants sweepable too: `segmentation ε` redraws the strip as "which bout does this death belong to, for ε from 10 to 60 m", and a bout whose membership shifts under a 20 % nudge of ε gets a **boundary-stability** chip on its article. Structure that exists at only one parameter value must not be quoted as fact.

### 4.6 Stage — R3 Field (four faces)

```
│ FIELD ⟨Matrix | Braid | Front | Board⟩            set: all avatars, 14:02–now │
│ MATRIX  damage dealt (row ▶ col), log-scaled, ⌗ = n events, ▨ = low support   │
│            Cad  Rook  Vex  Ilse  Tam  Kai      ⟨41 agents: blocks collapsed,  │
│    Cadmus   ·   ▁²    ██⁴¹ ▆¹⁷  ▃⁶   ▁¹         6 shown, click a block to     │
│    Rook    ▁²    ·    ▅¹⁴  ██³⁸ ▂⁴   ·          expand its rows⟩              │
│    Vex     ██³⁴ ▆¹⁹    ·    ▁¹   ▨¹  ▅¹²                                      │
│    Ilse    ▇²⁸  ██⁴⁴  ▁²    ·    ·   ▃⁷                                       │
│    Tam     ▃⁵   ▂³    ▨¹   ·     ·   ██²⁹      ← Tam sits outside both blocks │
│    Kai     ▁¹   ·     ▅¹¹  ▃⁸   ██³¹  ·                                       │
│   blocks: {Cadmus,Rook,Ilse} vs {Vex}  |  {Tam,Kai} mutual — third party?     │
├───────────────────────────────────────────────────────────────────────────────┤
│ BRAID  inferred allegiance over time (5-min window, saturation = confidence)  │
│         14:05      14:20      14:35      14:50      15:05      15:20          │
│ Cadmus  BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB     │
│ Rook    BBBBBBBBBBBBBBBBBBBBBBBB!BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB     │
│ Vex     AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA     │
│ Tam     ····aaaaaaaaaAAAAAAAAAAAAAAAA??????bbbbbbbbBBBBBBBBBBBBBBBBBBBBB     │
│ Kai     ····························??????????ccccccccccccccccc·······       │
│  UPPER = confident (margin ≥ .35)  lower = tentative  ? = unresolved          │
│  ! = friendly-fire event   · = absent/untracked   [override ▸] [why? ▸]       │
```

At 41 agents the Matrix is 1 681 ordered pairs, so it does not draw 41 rows: rows and columns order by block, each block collapses to one summary row/column, and clicking a block expands its members in place. The values behind it are never recomputed on a paint tick — §6, "Cost and caching".

The **Braid** is the answer to freeform, FFA and intra-group combat: allegiance as a time-varying per-avatar inference drawn with its confidence, switches and unresolved stretches visible rather than smoothed away. Dragging a row segment onto another letter is the officer override; a delta chip then reports "2 friendly-fire flags removed, 1 added", keeping overrides auditable.

The **Front** face plots events in unbent coordinates: `u` (signed position along the axis joining the team centroids) on y, time on x, deaths as crosses; pushes and collapses read as slopes. With no two-team structure the y axis switches, with a visible label, to "distance to nearest hostile" — a front the data does not support is not drawn, and the Chart renderer carries the geography meanwhile.

The **Board** draws one cell per pin on identical axes: six weapons' through-wall sweeps, or six avatars' lives, side by side.

### 4.7 Stage — R4 Session Board

```
│ SESSION 14:02–15:47   3 phases   41 agents   88 deaths   19.4k dmg           │
│  ⟨Board | Ground⟩                                                            │
│  PHASES ▸ [ Staging 14:02–14:26 ] [ West Gate push 14:26–15:03 ]             │
│           [ Collapse & counter 15:03–15:47 ]                                 │
│  ┌ teams ─────────────┐ ┌ headline ──────────────────┐ ┌ flagged ──────────┐ │
│  │ A  17 av  38 kills │ │ deaths 88   ▇▇▇▇▇▇▇▇░░ 82% │ │ 3 deaths with no  │ │
│  │ B  19 av  44 kills │ │ attributed 72              │ │   attributable dmg│ │
│  │ ?   5 av   6 kills │ │ wall-kill susp. 5 ▇▇░░░░░░ │ │ split unresolved  │ │
│  └────────────────────┘ └ (gutter = evidence support)┘ │   14:44–14:51     │ │
│  LEDGERS ⟨Avatars | Teams | Equipment | Scripts⟩                             │
│   equip                 users  hits  dmg   kills  ρ    wall%   class conf     │
│   Mk4 Carbine [B]         9    611  4.1k    22   .81   4% ▨    weapon  ●●●    │
│   RX Grenade  [B]         6     88  1.2k     9   .93  31% ██   deploy  ●●○    │
│   ??? bullet 0x8f1a       ?    142   900     4    —    12% ▨   unknown ○○○    │
│   Ward-7 armour script   14      —     —     —   .62    —      script  ●●●    │
```

### 4.8 The Article floater

```
┌ DEATH · Vex ← Cadmus · 14:31:07.4 ────────────────────────────── [pin] [case]┐
│ rungs:  [-1 record] [0 moment] [1 in bout] [2 LOS sweep] [3 in matrix] [4 —] │
├──────────────────────────────────────────────────────────────────────────────┤
│ Vex was killed by Cadmus at 14:31:07.4, 28.2 m away, with Mk4 Carbine        │
│ (ballistic). The killing blow was 18.0, reduced from 24.0 by Ward-7 armour.  │
│ Line of sight is MIXED across the travel window — see the sweep.             │
│ ── Contributing damage (window 20 s) ──────────────────────────────────────  │
│  Cadmus / Mk4 Carbine   volley ×3   54.0   ██████████████░░░  (share 71%)    │
│  Rook   / RX Grenade    volley ×1   22.0   █████░░░░░░░░░░░░  (share 29%)    │
│  ⚠ share of damage, not of health — health values are not observable.        │
│ ── Evidence ──────────────────────────────────────────────────────────────   │
│  killer track: bridge 2 Hz, gap 0.0 s, σ ±0.15 m        ●●●                  │
│  victim track: viewer, gap 0.4 s, σ ±0.5 m              ●●○                  │
│  equipment: attached pt 6, source≠rezzer ⇒ projectile   ●●●                  │
│  log lag 0.62 s (this event was not fd-confirmed)       ●○○                  │
│  ordering: all contributors carry script `t`; last-event                     │
│    margin 0.31 s > uncertainty 0.12 s ⇒ killing blow unambiguous  ●●●        │
│ ── Links ── Cadmus · Vex · Mk4 Carbine · Ward-7 · Bout "West Gate" · Team B  │
│ ── Backlinks ── Case "Vex report" · Phase "West Gate push" · Cadmus's lives  │
└──────────────────────────────────────────────────────────────────────────────┘
```

Prose first, table second, evidence third, links last — the wikipedia shape. Every underlined noun is an `LLUrlEntry` link.

## 5. In-world overlay

The overlay is itself rung-indexed. Pressing `]` changes what the world shows — the officer watches abstraction happen in the place they already understand. Every rung below also has its 2D Chart twin (§4.4); the world adds depth, occlusion and a free camera, and takes them away when the officer moves.

- **R0 Moment.** Avatars as feet-rings with a yaw arrow and name label; an eye glyph for mouselook; seated avatars ride their vehicle marker. Damage lines attacker→target at `t`, coloured by damage type, brightness by damage, pulsing 1 s. Deaths as cross + ring at `target_pos` with a killer line from `source_pos`.
- **R1 Plan.** Trails for the whole bout, per-vertex alpha old→new; volleys as fans of thin lines between shooter and victim trails; persistent labelled death crosses; the bout hull as a faint ground quad. Every street at once.
- **R2 Sweep.** The **LOS fan**: each candidate shooter position over τ as a marker on the killer's trail, each with its ray to the victim's chest — green clear, red blocked, grey dashed unknown — and the first blocking hit ticked on the wall. Scrubbing the strip walks a bright cursor along the fan.
- **R3 Field.** Team centroid tracks as two thick lines; the front axis as a ruled ground line with `u` iso-marks; death density as ground quads; a faint grid that *is* the unbent coordinate system the Front chart uses.
- **R4 Session.** The ground plan: death heat quads, staging rings, push arrows between phase centroids, phase labels floating at them.

**Encodings.** Colour is team (from the officer's existing contact-set / group-colour chain) or damage type on event lines; direction is geometry, never colour; width in buckets `{1,3,6}`; alpha for age and confidence.

**Uncertainty in the world.** σ is the foot-ring radius, so a coarse-located avatar visibly sits inside a 4 m disc. Track gaps draw dashed and thin, extrapolated positions hollow, unknown equipment gets a `?` on its lines. A BLOCKED ray is only ever drawn beside its sibling rays from the sweep — a lone red ray is a lie.

**Occlusion as the through-wall cue.** With x-ray off, lines draw twice: depth-tested at full alpha and depth-off at 30 %. A shot ghosted through its middle went through geometry, and you can see the wall doing it.

**Clicking.** Every marker pushes a screen rect with a noun id during render. Left-click selects, `Shift+click` descends a rung onto that noun, `Ctrl+click` pins; ALT stays untouched so alt-cam keeps working. Hover gives the noun's headline fact.

## 6. Analysis model

Derived quantities, how each is computed, and how its uncertainty is shown.

1. **Position `p(a,t)`, confidence `σ(a,t)`.** From `sampleAt`; base σ by source (bridge 0.15 m, viewer 0.3 m, coarse 4 m horizontal / 2 m vertical) plus `0.5 · gap · |v|` when interpolating. Drawn as the foot-ring radius, chipped as `●●○`.
2. **Coverage `C(a, window)`** — fraction of the window with a sample within 1 s. Drives dashed trails, the ribbon quality row, and hatching of anything built from that avatar.
3. **Volley** — `(owner, rezzer, target)` runs with ≤ 2 s gaps; carries hits, damage, initial damage, duration, mean range.
4. **Event time and ordering.** Each event carries `t̂` (script `t` when present, else `recv − ê` where `ê` is the running median relay lag) and an uncertainty `u` (0.05 s for `fd`-confirmed, 0.15 s for script-stamped, 1.0 s for receive-time fallback). The store sorts by `t̂`; **no ordering claim is made between two events whose `t̂ ± u` intervals overlap.**
5. **Death attribution.** DAMAGE on the victim in `[t−20 s, t]` grouped into volleys; each contributor gets a **damage share**, labelled "share of damage, not of health" in the mark itself, because health is unobservable. The **killing blow** is the last event by `t̂` *only if* its margin over the runner-up exceeds the combined uncertainty; otherwise the article reads "killing blow ambiguous between X and Y", both are drawn, and equipment kill credit is split and marked `▨`. The evidence block prints margin and uncertainty, so an officer sees when the optional `t` stamp is all that stands between two suspects.
6. **Bout.** DBSCAN over `(x, y, t/κ)`, κ = 1.2 m/s, ε = 30 m, minPts = 4. **A position-less DAMAGE event is placed at the target's interpolated position** — not the attacker's, not the midpoint: bouts are where people are being hit, a long-range shooter is a separate spatial fact, and midpoints invent locations where nobody stood. The choice is a labelled toggle inside the segmentation sweep. Placed events are flagged; edges dither when boundary density is within 20 % of the threshold.
7. **Phase.** Bouts merged at session scale, split at lulls (> 90 s below 5 % of median intensity). ε, κ, minPts and the lull rule are sweepable at R2 (§4.5) and every bout carries a boundary-stability chip.
8. **Hostility graph.** Directed `W(a→b)` = damage dealt in the window, log-compressed for display; cells show `n` and hatch below `n = 3`.
9. **Team assignment (5-min sliding window).** Label propagation minimising intra-cluster damage. **Seeding is decided before clustering, from the graph's vertex set alone** — no circularity: group-id seeds are enabled only when (a) the avatars with ≥ 1 damage edge in the window carry ≥ 2 distinct group ids, (b) each such id covers ≥ 2 of them, and (c) under half the window's damage weight runs between avatars sharing an id. In an intra-group skirmish (a) or (c) fails, seeds disable themselves, and the header names the failed test. Clustering also runs unseeded; if the partitions disagree on more than 10 % of members, the unseeded result is shown and the header says so. **Margin** = `(best − second)/best`: ≥ 0.35 confident, 0.15–0.35 tentative, below unresolved. That is the Braid's saturation.
10. **Friendly fire.** Damage where attacker and target share the window's assignment; every flag inherits that assignment's confidence, so a tentative team yields a hollow-drawn FF flag.
11. **LOS `V(τ)`.** For τ over `[0, SSCombatLogTravelSeconds]` in 0.1 s steps (≤ 16 casts), cast `p(killer, t−τ) + eye` → `p(victim, t) + chest` with the +0.6 m retry. The output is the function; any scalar shown always carries the 8 px miniature. COARSE-sourced candidates return UNKNOWN by rule.
12. **Through-wall rate per equipment.** `BLOCKED / (BLOCKED + CLEAR)` over its kills with a Wilson 95 % interval, `UNKNOWN` counted separately as hatch. Two of three blocked reads `67 % [21–94 %] · n=3` — never "kills through walls".
13. **Awareness lead `A`.** A **free prefilter runs first**: yaw within θ of the victim's bearing for ≥ 0.5 s before the first hit, from track samples with no raycasting at all. Only kills that pass it spend casts — a 6 s backward window at 0.5 s steps, ≤ 12 casts — to find the earliest tracked instant whose LOS was blocked, minus the time LOS first cleared. Positive means tracking through geometry. Yaw-only (no pitch), sample-rate limited, and must be swept over θ at R2 before it may be cited.
14. **Reaction interval.** First-clear-LOS to first hit, per hit: a bounded backward sweep on the same 0.1 s τ-grid as #11, sharing its cache, so a reaction interval costs one LOS sweep and no more. Reported as a percentile of the session's own distribution, never against an absolute threshold, and greyed where the hit lacks a script `t` or `fd` stamp (see #4).
15. **Contact and Exposure.** The Life strip shows **Contact** by default: fraction of a life with a hostile inside weapon range and inside the subject's broad frontal arc, from tracks alone at 2 Hz with **zero raycasts** — O(N) per sample, always available, labelled "geometry not tested". **Exposure** (raycast-confirmed clear LOS to ≥ 1 hostile) is an explicit per-life, on-demand overlay on that strip: 0.5 Hz anchors, three nearest hostiles each, hard-capped at 600 casts per life (≈ 3 frames at the 200-cast budget), filling progressively behind a hatch. Continuous session-wide exposure would be hundreds of thousands of casts and is not offered at any price; the strip says where it is uncomputed rather than pretending the number exists.
16. **Adjustment ratio `ρ = damage/initial`,** attributed to scripts via `modifications`, aggregated per team, avatar and damage type. Script articles list the observed ρ distribution and who carries them.
17. **Equipment classification.** The plan's rules as a weighted evidence checklist; the article shows which fired and which failed, confidence as `●●●…○○○`. UNKNOWN is a first-class outcome with its evidence listed.
18. **Motion plausibility.** Max/mean speed against the flags (flying, on-object); shown as a distribution with the avatar's percentile, never as an accusation.
19. **Witness and anchor.** The **witness** is the medoid instance under the aggregate's own metric (nearest the matrix cell's centroid; the death nearest the bout's space-time centre), with min/max extremes on `[`+Left/Right — deterministic, used when you arrive cold. The **anchor** is the instance you last stood on beneath this subject, written by `]` and read by `[`. Together they make step-down deterministic *and* lossless: you always land somewhere named, and if you climbed here from somewhere, that somewhere is where you land.

**Cost and caching.** The hostility graph, per-window damage totals, volleys and coverage are **accumulators updated on event append** (O(1) per event), never recomputed on paint. At 41 agents a 5-min window holds ≤ 1 681 ordered pairs, sparse in practice, ~20 KB per window and under 1 MB for a 2 h session; Matrix and Braid read that cache and never touch the event vector during a paint tick. Per-cell sparklines compute on hover only, from a binary search into the event vector. LOS results cache per `(event, τ-grid)` and are shared by #11, #13 and #14. The only unbounded consumer left is raycasting, and it is budgeted: 200 casts per frame, filling behind a visible "computing" hatch, never blocking a frame.

## 7. Walkthroughs

**Q1 — how did this raid go, where, when, between whom?** Open the floater; the rail sits at R4. *When:* three phases and two damage crescendos in the ribbon — 24 min staging, 37 min push, 44 min collapse-and-counter. *Between whom:* the teams box reads A 17 avatars / 38 kills, B 19 / 44, plus 5 unresolved; clicking A opens its ledger row — mostly one group tag, one mid-session defector, `?` for two arrivals. *Where:* the **Ground** face, no alt-cam needed, shows death heat in three lobes and phase-centroid arrows marching east then snapping back west. Q1 answered in three glances at the top rung, before any drilling. Then drill: `[` on "West Gate push" → R3 Front, where B's `u` marches −0.8 → +0.4 over eleven minutes and snaps back in ninety seconds at 15:03. Click the snap, `[` → R1 Plan (Chart, since the officer is standing at staging): B's trails funnel into one alley with six crosses in it. `[` again → R0; walk to a window, alt-cam the alley mouth, see the geometry that made it a trap. `]]]` returns up the same path, each landing named in the rail before the key is pressed.

**Q3 — why did X die at 14:32?** Scrub to the tick, click the death marker. The Article: 71 % Cadmus / Mk4, 29 % Rook / grenade, reduced by Ward-7, killing blow unambiguous (margin 0.31 s against 0.12 s of uncertainty). `[2] LOS sweep` shows BLOCKED below τ = 0.5 s, CLEAR from 0.5–1.1 s. Click τ = 0.75 → R0 with that one clear ray drawn: the killer was past the pillar. Into the Case: "no wall kill; line confirmed at 0.75 s travel".

**Q8 — did anyone behave suspiciously?** R4's flagged panel says "Rook: 4 **kills** with positive awareness lead" — awareness lead is a killer-side quantity, and the panel names it as such. Click → R3 Board, four small multiples of the awareness sweep over θ. Three collapse to zero lead past θ = 15° — noise. One holds 1.8 s out to θ = 30° with a `●●●` coverage chip (bridge track, no gaps). Click it → R1 Plan: Rook's yaw arrow tracks the victim behind a wall for two seconds before the first shot; `[` → R0 at 0.25× to watch it. The Case stores the sweep, the coverage and the tool's own sentence: *consistent with wall tracking and also with a lucky pre-aim; wireframe use cannot be observed.*

**Intra-group skirmish.** Both sides wear the Ashguard tag. The pre-clustering seed test fails condition (a) — one group id among the graph's vertices — so seeding disables itself before any clustering runs, and the Braid header reads "group seeds unusable — identical tags (test a failed)". The damage graph splits cleanly after four minutes; before that the Braid is `?`. Tam flickers between letters; `why?` shows he damaged both blocks in the first window. The officer drags his early segment to A and the delta chip reports "3 friendly-fire flags removed". The now-honest FF list has one real incident, which opens as a moment and turns out to be a grenade at a doorway.

**FFA on a neutral sim.** The Matrix has no block structure, so the tool refuses to draw teams, gives every avatar its own colour, and leaves the Braid mostly `?`. The Front face relabels its y axis "distance to nearest hostile", and the Ground chart carries the geography instead. The officer works the Matrix: two mutual-damage pairs stand out as running duels; pin both, and the Board shows their bouts side by side. Nothing in the UI ever asserts a team the data does not support.

**Arriving mid-fight, cold.** Kai teleports in at 14:47 with no track history and no group tag the tool has seen. His Braid strand is `·` until the first update, then `?` — never a letter, because margin is undefined on one damage edge. His Ledger row draws `·` until coverage arrives, so his volleys are visible while his position is not, and any bout he is placed into by rule #6 carries the placed-event flag. His article headline reads "first seen 14:47:12 · 38 min unobserved · team unresolved (n=1 edge)". Nothing is hidden and nothing invented; at his third damage edge the strand goes lowercase-`c` (tentative) and the officer can watch it firm up, or override it.

## 8. Risks and open questions

- **Plan delta required for R−1.** The decided `CombatEvent` struct keeps no raw bytes. R−1 needs one addition: an append-only char arena plus `RawLine { F32 recv; U32 offset, len; U8 trust; U8 salvageMask; U32 firstEvent, nEvents; }`, `salvageMask` recording which fields a truncated-array parse dropped per element (the plan salvages complete `{…}` elements but does not record what it lost). Cost ≈ 100k × 250 B ≈ 25 MB, inside the plan's envelope, evicted on the same retention sweep. If refused, R−1 degrades to a *reconstructed* record view and the rail must say `reconstructed`, not `raw` — the one thing it may not do is pretend.
- **Awareness lead is yaw-only** — no pitch, 2 Hz sampling, laggy viewer yaw. The free prefilter makes it affordable, not truer; it is the strongest suspicion signal here and the easiest to over-read, and sweep-before-citing mitigates without removing that.
- **Log lag sits inside the reaction-interval scale,** and the wire contract makes `t` optional. Rule #4 makes the ambiguity visible instead of silent, but an officer who wants one name will resent being given two. The fix that actually works is upstream: require `t` per event in the bridge spec.
- **Coarse z is quantised to 4 m,** so LOS from a COARSE sample returns UNKNOWN by rule. Distant kills are therefore mostly unresolvable, and officers will want an answer anyway.
- **Cold landings can still mislead.** With an anchor, up-then-down is exact; without one, the witness is a representative and a bout's representative death may be atypical. The rail's `(witness)` label and `[`+Left/Right to the extremes are the mitigation.
- **The Front face still refuses to draw without two-team structure,** and that stays; the Chart renderer covers the geography instead, so refusal costs nothing but a coordinate system the data never earned.
- **Officer overrides anchor.** The delta chip keeps them auditable inside a session; nothing persists across sessions in v1, so a wrong override is at least not permanent.
- **Two renderers, one drift risk.** World and Chart must share encoding code, not just intent, or they diverge under maintenance; parity has to be a shared draw-list, not a convention.
- **Open:** naming zones without parcel data; whether the braid window should adapt to event density; whether Cases should survive the session given the no-files decision (clipboard is the current answer); multi-region raids, where the front transform and the Chart's extent both lack a single frame.
