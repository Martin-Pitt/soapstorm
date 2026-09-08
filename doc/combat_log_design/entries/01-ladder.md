# Entry 01 — LADDER-FIRST: the rung is the app

## 1. Thesis

Most review tools hand you a pile of views and hope you build the ladder in your head. Here the ladder *is* the application. Six rungs, −1 to 4, from the raw received record up to the whole session; the officer stands on exactly one, looking at exactly one subject, and the two keys that matter are `[` (down, toward the concrete) and `]` (up, toward the abstract). Navigation is two-dimensional and orthogonal: **the ladder is vertical, the wiki is horizontal.** Vertical movement changes the level of abstraction and keeps the subject; horizontal movement changes the subject (avatar → weapon → team → death) and keeps the level. Every step up is a named operator written into the breadcrumb, and every aggregate mark carries a **witness** — the concrete instance that best exemplifies it — so stepping down is a deterministic landing, never a search. Two invariants hold it together: **up-then-down is the identity** (ascend a death into a bout, descend, you get that death back), and the **haze rule** (no aggregate may be drawn without the unknown fraction it was built from drawn in the same mark). Nobody gets stranded in the clouds with a number they cannot check, or on the ground with a moment they cannot situate.

## 2. Ladder

Six rungs. Each names its **subject type**, its **step up** (the compression operator, which becomes a breadcrumb token) and its **step down** (the grounding gesture, which always lands on a named instance).

**R−1 — Record.** Subject: one received line — raw JSON as it arrived, trust flag, sender/owner keys when untrusted, receive time, script `t`, clock offset, log lag (`recv − t`), the `fd` dedup partner, and the salvage state if the batch was truncated. Any claim can be run to ground in the literal bytes.
*Up:* `interpret` → typed event at R0. *Down from R0:* `Ctrl+Enter`, or the raw chevron in the article header.

**R0 — Moment.** Subject: an instant `t` ± 1 s. The world overlay at the cursor plus the Moment strip (the events in the window, one line each). The visceral rung: bodies where they were, tracers as they flew.
*Up:* `over time` → the moment's bout. *Down:* pointing — click any world marker or strip row; `[` from R1 lands on the bout's **witness moment** (its medoid death, or peak damage rate if there were none).

**R1 — Episode.** Subject: a *bout* (spatiotemporal cluster of fighting) or a *life* (spawn → death). Two faces, because "all moments at once" can collapse either axis: **Ledger** collapses space (avatars as rows, time across, volleys and deaths as marks) and **Plan** collapses time (every trail, line of fire and death cross drawn at once in the world). Switching faces is free and does not touch the breadcrumb — it is not a change of rung.
*Up:* `over parameters` → R2, or `over avatars`/`over bouts` → R3. *Down:* click any mark → its moment; bare `[` → the witness moment.

**R2 — Sweep.** Subject: one claim swept across one parameter. Not "was this a wall kill" but "here is the LOS verdict at every point in the 1.5 s travel window, and here is where it flips". Sweepable: travel time τ, attribution window, team-confidence threshold, awareness cone θ, adjustment-script inclusion, track-quality cutoff. A sweep is a strip: parameter on x, outcome as colour, current value marked.
*Up:* `across cases` → R3, the same sweep small-multipled over every case of its kind. *Down:* click the strip → R0 at the moment that value implies (τ = 0.9 s puts the cursor at the killer's position 0.9 s before the hit, that one ray drawn).

**R3 — Field.** Subject: a set (deaths, avatars, weapons, bouts) across two or more dimensions. Four faces: **Hostility Matrix**, **Allegiance Braid**, **Front Chart** (space unbent into front-coordinate `u` vs time), **Board** (small multiples of the pins).
*Up:* `summarise` → R4. *Down:* every cell, band and dot is a witness handle; `Shift+click` a matrix cell lands on the medoid damage event at R0 with R1 context preloaded.

**R4 — Session.** Subject: the raid — phases, ledgers (avatars, teams, equipment, scripts), headline counts with confidence gutters, and the open questions the tool flags on itself ("3 deaths unattributable", "team split unresolved 14:44–14:51").
*Up:* none, and the rail says so. *Down:* every number, ledger row and phase band descends to its witness.

The rail shows all six subjects at once, so ascending is never a leap: you read the destination before you press `]`.

## 3. Information architecture

**Nouns** (each is an article, each has backlinks): Session, Phase, Bout, Moment, Life, Avatar, Team, Death, Damage, Volley, Equipment (Weapon / Projectile / HUD / Deployable / Vehicle / Mount), Script (a damage-adjustment script seen in `modifications`), Zone (a named spatial cell), Verdict (a stored LOS result with its sweep), Case (an officer-assembled bundle: pins + notes), Record.

**Volley** is the load-bearing noun the raw log lacks: consecutive DAMAGE events sharing `(owner, rezzer, target)` with gaps ≤ 2 s. It is what an officer means by "he shot him", and death attribution, equipment stats and the Bout Ledger all speak in it.

**Two floaters, one overlay.** `ss_combat_log` is the **Ladder** floater: fixed chrome (breadcrumb, rung rail, ribbon, transport) with a centre stage that swaps by rung — six stages, nothing duplicated. `ss_combat_inspector` is the **Article** floater, keyed by noun id so several can be open and compared; it is rung-agnostic, the horizontal axis.

**Linking.** Article body is an ordinary `LLTextEditor` with a registered `LLUrlEntry` for `secondlife:///app/sscombat/<type>/<id>`, so noun mentions in prose are real clickable links with tooltips, exactly like agent links today. Lists are scroll lists whose rows carry the same noun id.

**Every article carries a Rung Row** — six buttons `[-1]…[4]`, enabled only where that noun means something at that rung, each labelled with what you would see. On a Weapon: `[1] its volleys`, `[2] its LOS sweep`, `[3] its through-wall field`, `[4] its ledger row`. This is what welds wiki to ladder: any noun, any rung, one click.

**History** is one shared back/forward stack across both floaters storing `(rung, subject, face, cursor, window)`; Backspace goes back; the breadcrumb prints the operators.

**Pinning** (`Ctrl+click`, or `P`) drops a noun in the Pin tray, and the R3 Board draws one cell per pin on shared axes — that is how small multiples get built. Pins plus a note field make a **Case**, the artifact of the violation workflow, copyable to the clipboard as plain text for a group notice.

## 4. Screens

### 4.1 The Ladder floater — fixed chrome

```
┌ COMBAT LOG ─ 14:02–15:47 ─ ●REC ─ bridge ok ─ 41 agents ─ drop 0 ─ rtt 141ms ─┐
│ ◀ ▶ │ Session ▸over time▸ Bout "West Gate" ▸over avatars▸ Team B ▸ Cadmus     │
├──────┬──────────────────────────────────────────────────────────┬─────────────┤
│ RUNG │                                                          │ EVIDENCE    │
│  ▲ ] │                                                          │ ┌─────────┐ │
│[4]Ses│                                                          │ │ what is │ │
│[3]Fld│                    S T A G E                             │ │ this    │ │
│[2]Swp│              (swaps with the rung)                       │ │ built   │ │
│[1]Bou│                                                          │ │ from    │ │
│[0]Mom│                                                          │ └─────────┘ │
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

- **Breadcrumb row.** Back/forward plus the path; clicking a token jumps there, and the operator token itself links to its own explanation.
- **Rung rail** (86 px). Six cells, abstract at top, ground at bottom, each showing the subject it holds. `]`/`[` or click to move. A cell greys when its subject is undefined (R2 with no claim) and says what would define it.
- **Stage.** A `layout_stack` of six panels, one visible: custom `LLView`s drawing with `gl_rect_2d` + `LLFontGL::renderUTF8`, plus stock scroll lists — well inside XUI's immediate-mode budget.
- **Evidence column** (collapsible, 180 px). Always answers "what is this made of": inputs, sample counts, coverage, and the assumptions in force (window lengths, thresholds), each editable in place with live re-render. Right-clicking a parameter here is the entry to R2: "sweep this".
- **Ribbon** (the constant). Death ticks by victim team; stacked damage-rate band per team; bout bands, dithered at fuzzy edges; a track-quality row hatched below 60 % coverage. Drag scrubs, Shift-drag sets the R1 window, double-click a bout ascends to it. **The ribbon never changes; the stage always does** — it is the spine that makes vertical movement legible.
- **Transport.** Step ±1 event / ±1 s, play, speed, Live, and "follow cursor" which drives the camera focus to the current subject.

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

Rows are concrete events; the selected one pulses in the world. `▾` opens R−1 inline as a two-column table. Note the honest `▒UNKNOWN` where a lesser tool would print a green tick.

### 4.3 Stage — R1 Face A, the Bout Ledger

```
│ BOUT "West Gate"  14:29:12–14:36:40  6 deaths  1.4k dmg  ⟨Plan|Ledger⟩       │
│           14:29        :30         :31         :32         :33         :34   │
│ Cadmus B  ░░▓▓▓▓══╪═▓▓▓░░░░░░░░░▓▓▓▓▓▓╪▓▓░░░░░░░░░░░░░░░░░░░░░░▓▓▓▓╪░░░░░░  │
│ Rook   B  ░░░░░░░░░░░░▓▓╪▓▓▓░░░░░░░░░░░░░░✖▁▁▁▁▁▁▁▁▁▁▁▁▁░░░░░░░▓▓▓░░░░░░░░  │
│ Vex    A  ▓▓▓▓╪▓▓░░░░░░░░░░░░✖▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▓▓▓╪▓▓▓░░░░░░░░░░✖▁▁▁▁▁▁  │
│ Ilse   A  ░░░░░░░░▓▓▓▓▓▓▓▓╪▓▓▓▓░░░░░⚠╪░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  │
│ Tam    ?  ····························▓▓▓╪▓░░░░░░░░········✖▁▁▁▁▁▁▁▁▁▁▁▁▁▁  │
│           ▓ volley  ╪ death-causing volley  ✖ death  ▁ dead  ⚠ friendly fire │
│           ░ alive, tracked   · no track (coverage gap)                        │
```

One row per avatar; team letter and colour in the gutter, hatched when the assignment is tentative. Volley block height encodes damage rate. Every mark selects on click, descends on `Shift+click`, pins on `Ctrl+click`. One widget answers "who fought whom, when, and who was out of it" for an engagement.

### 4.4 Stage — R2 Sweep

```
│ SWEEP: line of sight for DEATH Vex ← Cadmus 14:31:07.4                        │
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

The sweep replaces the scalar verdict everywhere one would have been shown; verdict badges in lists are 8 px miniatures of this strip, so flip-flopping is visible before a word is read.

### 4.5 Stage — R3 Field (four faces)

```
│ FIELD ⟨Matrix | Braid | Front | Board⟩            set: all avatars, 14:02–now │
│ MATRIX  damage dealt (row ▶ col), log-scaled, ⌗ = n events, ▨ = low support   │
│            Cad  Rook  Vex  Ilse  Tam  Kai                                     │
│    Cadmus   ·   ▁²    ██⁴¹ ▆¹⁷  ▃⁶   ▁¹                                       │
│    Rook    ▁²    ·    ▅¹⁴  ██³⁸ ▂⁴   ·                                        │
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

The **Braid** is the answer to freeform, FFA and intra-group combat: allegiance as a time-varying per-avatar inference drawn with its confidence, switches and unresolved stretches visible rather than smoothed away. Dragging a row segment onto another letter is the officer override; a delta chip then reports "2 friendly-fire flags removed, 1 added", keeping overrides auditable.

The **Front** face plots events in unbent coordinates: `u` (signed position along the axis joining the team centroids, ±1 at the centroids) on y, time on x, deaths as crosses. Pushes and collapses read as slopes. With no two-team structure the y axis switches, with a visible label, to "distance to nearest hostile".

The **Board** draws one cell per pin on identical axes: six weapons' through-wall sweeps, or six avatars' lives, side by side.

### 4.6 Stage — R4 Session Board

```
│ SESSION 14:02–15:47   3 phases   41 agents   88 deaths   19.4k dmg           │
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

### 4.7 The Article floater

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
│ ── Links ── Cadmus · Vex · Mk4 Carbine · Ward-7 · Bout "West Gate" · Team B  │
│ ── Backlinks ── Case "Vex report" · Phase "West Gate push" · Cadmus's lives  │
└──────────────────────────────────────────────────────────────────────────────┘
```

Prose first, table second, evidence third, links last — the wikipedia shape. Every underlined noun is an `LLUrlEntry` link.

## 5. In-world overlay

The overlay is itself rung-indexed. Pressing `]` changes what the world shows — the officer watches abstraction happen in the place they already understand.

- **R0 Moment.** Avatars as feet-rings with a short yaw arrow and name label; a small eye glyph for mouselook; seated avatars ride their vehicle marker. Damage lines attacker→target at `t`, coloured by damage type, brightness by damage, pulsing 1 s. Deaths as cross + ring at `target_pos` with a killer line from `source_pos`.
- **R1 Plan.** Trails for the whole bout, per-vertex alpha old→new; volleys as fans of thin lines between shooter and victim trails; persistent death crosses with labels; the bout hull as a faint ground quad. Every street at once.
- **R2 Sweep.** The **LOS fan**: each candidate shooter position over τ as a marker on the killer's trail, each with its ray to the victim's chest — green clear, red blocked, grey dashed unknown — and the first blocking hit ticked on the wall. Scrubbing the sweep strip walks a bright cursor along the fan.
- **R3 Field.** Team centroid tracks as two thick lines; the front axis as a ruled ground line with `u` iso-marks at ±0.5/±1; death density as ground quads; and a faint grid that *is* the unbent coordinate system the Front chart uses.
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
4. **Death attribution.** DAMAGE on the victim in `[t−20 s, t]` grouped into volleys; killing blow = last event; each contributor gets a **damage share**, labelled "share of damage, not of health" in the mark itself, because health is unobservable.
5. **Bout.** DBSCAN over `(x, y, t/κ)`, κ = 1.2 m/s, ε = 30 m, minPts = 4; position-less events sit at the participants' interpolated positions and are flagged. Edges dither when boundary density is within 20 % of the threshold.
6. **Phase.** Bouts merged at session scale, split at lulls (> 90 s below 5 % of median intensity).
7. **Hostility graph.** Directed `W(a→b)` = damage dealt in the window, log-compressed for display; cells show `n` and hatch below `n = 3`.
8. **Team assignment (5-min sliding window).** Label propagation minimising intra-cluster damage, seeded by shared active group id *only when* group ids differ across the graph's blocks — in an intra-group skirmish the seeds disable themselves and the header says so. **Margin** = `(best − second)/best`: ≥ 0.35 confident, 0.15–0.35 tentative, below unresolved. That is the Braid's saturation and case.
9. **Friendly fire.** Damage where attacker and target share the window's assignment; every flag inherits that assignment's confidence, so a tentative team yields a hollow-drawn FF flag.
10. **LOS `V(τ)`.** For τ over `[0, SSCombatLogTravelSeconds]` in 0.1 s steps, cast `p(killer, t−τ) + eye` → `p(victim, t) + chest` with the +0.6 m retry. The output is the function; any scalar shown (clear/blocked/mixed/unknown) always carries the 8 px miniature.
11. **Through-wall rate per equipment.** `BLOCKED / (BLOCKED + CLEAR)` over its kills with a Wilson 95 % interval, `UNKNOWN` counted separately as hatch. Two of three blocked reads `67 % [21–94 %] · n=3` — never "kills through walls".
12. **Awareness lead `A`.** Earliest time before `t` at which the killer's yaw was within θ = 12° of the victim's bearing *while LOS was blocked*, minus the time LOS first cleared. Positive means tracking through geometry. Yaw-only (no pitch), sample-rate limited, and must be swept over θ at R2 before it may be cited.
13. **Reaction interval.** First-clear-LOS to first hit, reported as a percentile of the session's own distribution, not against an absolute threshold.
14. **Exposure.** Fraction of a life with clear LOS to ≥ 1 hostile, sampled at 2 Hz; feeds the Life strip.
15. **Adjustment ratio `ρ = damage/initial`,** attributed to scripts via `modifications`, aggregated per team, avatar and damage type. Script articles list the observed ρ distribution and who carries them.
16. **Equipment classification.** The plan's rules as a weighted evidence checklist; the article shows which fired and which failed, confidence as `●●●…○○○`. UNKNOWN is a first-class outcome with its evidence listed.
17. **Motion plausibility.** Max/mean speed against the flags (flying, on-object); shown as a distribution with the avatar's percentile, never as an accusation.
18. **Witness.** The medoid instance under the aggregate's own metric (nearest the matrix cell's centroid; the death nearest the bout's space-time centre), with min/max extremes on `[`+Left/Right. This is what makes step-down deterministic.

## 7. Walkthroughs

**Q1 — how did this raid go?** Open the floater; the rail sits at R4. Three phases, two damage crescendos. `[` on "West Gate push" → R3 Front chart: Team B's `u` marches −0.8 → +0.4 over eleven minutes, then snaps back in ninety seconds at 15:03. Click the snap, `[` → R1 Plan: B's trails funnel into one alley with six crosses in it. `[` again → R0 at the witness death, where the officer alt-cams to the alley mouth and sees the geometry that made it a trap. Four keypresses, clouds to ground.

**Q3 — why did X die at 14:32?** Scrub to the tick, click the death marker. The Article: 71 % Cadmus / Mk4, 29 % Rook / grenade, reduced by Ward-7. `[2] LOS sweep` shows BLOCKED below τ = 0.5 s, CLEAR from 0.5–1.1 s. Click τ = 0.75 → R0 with that one clear ray drawn: the killer was past the pillar. Into the Case: "no wall kill; line confirmed at 0.75 s travel".

**Q8 — did anyone behave suspiciously?** R4's flagged panel says "Rook: 4 deaths with positive awareness lead". Click → R3 Board, four small multiples of the awareness sweep over θ. Three collapse to zero lead past θ = 15° — noise. One holds 1.8 s out to θ = 30° with a `●●●` coverage chip (bridge track, no gaps). Click it → R1 Plan: Rook's yaw arrow tracks the victim behind a wall for two seconds before the first shot; `[` → R0 at 0.25× to watch it. The Case stores the sweep, the coverage and the tool's own sentence: *consistent with wall tracking and also with a lucky pre-aim; wireframe use cannot be observed.*

**Intra-group skirmish.** Both sides wear the Ashguard tag, so seeding disables itself and the Braid header reads "group seeds unusable — identical tags". The damage graph splits cleanly after four minutes; before that the Braid is `?`. Tam flickers between letters; `why?` shows he damaged both blocks in the first window. The officer drags his early segment to A and the delta chip reports "3 friendly-fire flags removed". The now-honest FF list has one real incident, which opens as a moment and turns out to be a grenade at a doorway.

**FFA on a neutral sim.** The Matrix has no block structure, so the tool refuses to draw teams, gives every avatar its own colour, and leaves the Braid mostly `?`. The Front face relabels its y axis "distance to nearest hostile". The officer works the Matrix instead: two mutual-damage pairs stand out as running duels; pin both, and the Board shows their bouts side by side. Nothing in the UI ever asserts a team the data does not support.

## 8. Risks and open questions

- **Awareness lead is yaw-only** — no pitch, 2 Hz sampling, laggy viewer yaw. It is the strongest suspicion signal here and the easiest to over-read; sweep-before-citing mitigates but does not remove that.
- **Log lag (up to 1 s) sits inside the reaction-interval scale.** Reaction percentiles are only meaningful for `fd`-confirmed events or where bridge `t` stamps exist; elsewhere the metric must be greyed, and the coverage chip may be too subtle a way to do it.
- **Coarse z is quantised to 4 m,** so LOS from a COARSE sample returns UNKNOWN by rule. Distant kills will therefore be mostly unresolvable, and officers will want an answer anyway.
- **Witness medoids can mislead:** a bout's representative death may be atypical, and the default landing carries rhetorical weight. `[`+Left/Right to the extremes is the only mitigation.
- **Officer overrides anchor.** The delta chip keeps them auditable inside a session; nothing persists across sessions in v1, so at least a wrong override is not permanent.
- **Cost.** Sweeps are 16 casts per event and the Board can request dozens at once; caching per `(event, τ-grid)` is essential, with a 200-cast-per-frame budget filling in behind a visible "computing" hatch rather than blocking.
- **Open:** naming zones without parcel data (grid coordinates are unmemorable, typed names are work); whether the braid window should adapt to event density; whether Cases should survive the session given the no-files decision (clipboard is the current answer); multi-region raids, where the front transform has no single frame.
