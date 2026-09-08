# Entry 02 — Hypertext-first: the raid is a wiki (revised)

## 1. Thesis

A raid is not a table of events; it is a *body of interlinked subjects*. So the tool is not a floater with tabs, it is a **reader over an auto-generated wiki of the session**. Every noun an officer can meet — an avatar, a death, a hit, a weapon model, a team, a **band** (a short-lived local alliance), a 16 m patch of ground, a minute, a phase, a line-of-sight verdict, an officer's own written claim — has a **stable address**, a **page** rendered from the stores, **outbound links** to every noun it mentions, and **backlinks** listing everything that mentions it. One page skeleton, one reader; the "sub floaters" the owner asked for are second instances of the same reader. The ladder of abstraction is not a mode, it *is* the link grammar: stepping up means following a link to a coarser noun, stepping down means following a link to a concrete instance — and because a raid has three independent dimensions, the design does not pretend to one rail. It shows **three axes — WHO / WHEN / WHERE** — each with its own step-up and step-down, always visible, so you can never be stranded in the clouds or on the ground on any dimension. The 3D overlay is the **illustration pane of the page you are reading**, drawn over a permanent dim session base layer so free alt-camming always has something to click. And the tool's own uncertainty gets the same discipline as combat's: every tunable claim carries its **flip point** in the headline, not behind a facet.

## 2. Ladder — rungs as link types, on three axes

Every page prints a **rung rail** of three axis chips. Each chip shows where you currently stand on that axis, `^` steps up, clicking the chip's subject steps down. A page occupies a position on all three axes at once; unavailable axes are dimmed, never guessed.

| Axis | Rungs (concrete → abstract) |
|---|---|
| **WHO** | `event`/`death` → `duel` → `avatar` \| `equip`(one object) → `model`(weapon type) \| `band` → `team` \| `class`(handheld / HUD / deployable / vehicle) → `session` |
| **WHEN** | `moment @t` → `span` → `phase` → `session` |
| **WHERE** | `spot`(one 16 m cell) → `place`(named area) → `region` |

This resolves the three ambiguities a single rail cannot. A **Death page carries two WHO subjects** — the chip reads `WHO Nyx ⇄ Corr` and each name is separately clickable and separately step-up-able, so "up to career" never silently picks a party. A **Model page has a real step-up** on WHO: `model → class → session arsenal`; a weapon type is not on a side, and the rail no longer pretends it is. **Phase and Place are on different axes** and can never collide in one button.

Two extra facets sit across all axes:

- **Sweep** (`?sweep=travel`, `?sweep=teamconf`) — Victor's "abstract over parameters" rung, available on *every* claim with a tunable: LOS across the bullet-travel window, team assignment across the confidence threshold, damage across adjustment scripts. Rendered as small multiples (a filmstrip of miniature top-down figures), each cell clickable to step straight down to that parameter value made concrete in the world.
- **Raw** (`?as=raw`) — the underlying records verbatim, on every page, so an abstraction can always be checked against the ground. Every aggregate page also has **"Show me a case"**, which jumps to a representative concrete instance and sets the world cursor to it.

**Flip points are not optional.** Because a sweep is computed over the grid the claim already used (the LOS candidate set *is* the travel-window grid; the team sweep reuses one precomputed affinity matrix), the nearest parameter value at which the verdict changes is free to compute. It is therefore printed **in the lede and the infobox**, not only inside the sweep: "BLOCKED for travel < 0.9 s; CLEAR at or beyond it (5 of 6 candidates)". A skimming officer gets the fragility; the sweep is for the officer who wants to see its shape.

## 3. Information architecture

**Address grammar.** `ss:<type>/<id>[@<t>][?facet=…]`. Addresses are rendered as real clickable links through a registered `LLUrlEntryBase` subclass on `secondlife:///app/sscombat/…` — the same mechanism that makes `secondlife:///app/agent/<id>/about` clickable — so links work inside every `LLTextBase`: page bodies, tooltips, notifications, chat, notes.

**Addresses are session-local, and the UI says so.** `seq` numbers are per-viewer, and per-viewer bridge polling, batch truncation and independent dedup mean another officer's session may simply not hold the same event. So: aggregate addresses (`avatar`, `model`, `team`, `place`) are keyed on UUIDs or derived keys and *do* travel; event addresses are labelled session-local, copying one offers a best-effort content-hash form (`owner+target+t±0.5 s+damage`), and an unresolvable link lands on a **stub page** naming the facts it sought and what it found nearby. Cross-viewer resolution is an open problem (§8), not a shipped feature.

**Page types.** `session`, `phase`, `span`, `moment`, `event`, `death`, `duel`, `avatar`, `equip`, `model`, `class`, `team`, `band`, `place`, `spot`, `verdict`, `why`, `query`, `claim`, `trail`.

**Band — the missing noun.** A *band* is a transient, local alliance: 2–5 avatars who, inside a sliding 90 s window, (a) did no damage to each other, (b) shared at least two targets or one common aggressor, and (c) held median mutual distance under 40 m. It is computed from the windowed damage graph plus track positions only — no new raycasts — and it is what the officer digs into when three people gang up for two minutes in a leaderless brawl. Bands also appear *inside* teams as de-facto squads. A band page shows members, formation and dissolution times, its shared targets, and links up to `team` (if one exists) and out to its members' duels.

**One skeleton, every page.** Title + type glyph + confidence band · **Lede** (2–3 auto-generated sentences, every noun a link, hedges and flip points mandatory) · **Infobox** (key/value facts, right column) · **Figure** (custom-drawn: minimap, timeline, sparkline, filmstrip) · **Sections** (accordion) · **What links here** (backlinks, always last) · footer facets `[figure][table][cards][sweep][raw]`. *Cards* is a grid of text + colour-swatch cards; there is no screenshot pipeline in the plan, so the design never implies imagery.

**Navigation.** Back/forward per reader (Alt+←/→, mouse 4/5); breadcrumb of the current descent; **pin rail** (middle-click parks a page as a chip — the officer's working set); **split** (Shift+click opens in pane B, which has its own history); **time coupling** (a padlock binds the page's `@t` to the global scrub cursor).

**Backlinks are real, and typed.** Page rendering emits outbound refs into a reverse index (`ref → set<address>`); cheap edges (event↔avatar, event↔object, avatar↔team) are maintained at ingest, expensive ones when a page is first built. Officer claims carry a **polarity** and render with it in every backlink list: `⚑ FLAG`, `✓ CLEAR` (reviewed, benign — e.g. "sound cue confirmed in voice"), `⇄ OVERRIDE`, `✎ NOTE`. A promotion reviewer seeing "3 claims" can tell at a glance that two are exonerations.

**Trails.** The visit sequence is recorded as a graph. `ss:trail/current` renders as a node-link figure; `Name this trail…` freezes it as `ss:trail/promotion-nyx`; a trail dumps to chat as a linked outline. That is the briefing artefact and the promotion/violation dossier.

**Cache coherency.** Every rendered page records `(analysis epoch, dep set)`. An officer override, a new claim, or an ingest recompute bumps the epoch of the analyses it touches; LRU entries (32 pages) holding a stale dep are dropped, and a visible "recomputed" flash marks numbers that changed under the officer. No page ever quotes a membership or confidence the overrides have already contradicted.

## 4. Screens

### 4.1 Reader (main floater, `ss_combat_reader`; Combat ▸ Combat Log)

```
+ Combat Log — Reader A -----------------------------------------------[_][X]+
| < > ^ |ss:death/8814@14:32:07                   [pin][B|][raw][?] |
| Session > Phase 3 "Courtyard push" > Nyx Ashgrave > Death 14:32:07          |
| WHO Nyx ⇄ Corr ^  | WHEN 14:32:07 ^Phase3 | WHERE courtyard-NE ^   [fig|v]  |
+-----------------------------------------------------------------------------+
| pins: (S)ession (T)Red (A)Nyx (M)Tanto-SMG (Q)wall-kills                 [+]|
+--------------------------------------------+--------------------------------+
| DEATH of Nyx Ashgrave, 14:32:07.4      [!] | FACTS                          |
|                                            | victim   Nyx Ashgrave (Red)    |
| Nyx Ashgrave was killed by Corr Vell       | blow by  Corr Vell  uncontested|
| (Blue, likely) with a bullet from Tanto    | team     Blue  [likely]        |
| SMG (likely). Four hits in 2.3 s did 87    | weapon   Tanto SMG  [likely]   |
| damage; the killing blow was 32 and is     | blow     32 dmg, type 102      |
| uncontested. Line of sight is BLOCKED      | pre-hits 4 / 87 dmg / 2.3 s    |
| for travel < 0.9 s and CLEAR at or         | LOS      BLOCKED <0.9s [flip]  |
| beyond it (5 of 6 candidates); the         | verdict  2 FIRM, 3 MARGINAL,   |
| verdict turns on that assumption [why].    |          1 UNKNOWN (pos err)   |
|                                            | log lag  0.6 s (est)           |
| [figure: top-down 32 m, victim x, killer   | trust    sim log (authentic)   |
|  fan of 6 candidate rays, wall hatched]    +--------------------------------+
|                                            | WHAT LINKS HERE                |
| v Damage that led to this death        (4) |  ss:avatar/Nyx (deaths)        |
|   14:32:05.1 Corr Vell  18  102  [->]      |  ss:model/Tanto-SMG (wall-kill)|
|   14:32:05.9 Corr Vell  21  102  [->]      |  ss:query/wall-kills           |
|   14:32:06.6 Kest Wray  16  102  ff? [->]  |  ⚑ ss:claim/12 "Corr pre-aim?" |
|   14:32:07.4 Corr Vell  32  102  KILL[->]  |  ✓ ss:claim/19 "reviewed 9/8"  |
| > Line of sight (6 candidates)   [sweep]   +--------------------------------+
| > Equipment evidence                       | ALSO SEE                       |
| > Adjustments (armour scripts)             |  ss:moment/@14:32:07           |
| > Officer claims (2)              [+ note] |  ss:duel/Corr-Nyx  (3rd kill)  |
|                                            |  ss:band/7 (Corr+Kest, 14:31)  |
+--------------------------------------------+--------------------------------+
| [<< 1s] [>] [1s >>] 14:32:07 ---#--|-----  |                               |
+-----------------------------------------------------------------------------+
```

Regions: **chrome row** (back/forward/up, address `line_editor`, pin, split-to-B, raw); **breadcrumb**; **three-axis rung rail** (three button groups, current subject disabled, `^` = step up); **pin rail**; **body** = read-only `LLTextBase` with `parse_urls`, `LLAccordionCtrl` sections built only when opened, custom `LLView` figures; **infobox** = key/value `text`, italic + band chip for inferred values; **transport strip** (step, play, cursor, death ticks, damage density), global and shared by all readers.

Note the hedging rule at work: team and weapon carry bands; the killing blow is stated plainly *and labelled uncontested* because only one owner's damage window covers the death. When two owners' windows overlap, the same line reads "killing blow contested between Corr Vell and Kest Wray [why]" with a band chip.

### 4.2 Split / compare (pane B)

```
+ Combat Log — Reader A ------------------------------------------[_][X]+
| < > ^ |ss:avatar/Nyx          || < > ^ |ss:avatar/Corr        [x]|
| Nyx Ashgrave (Red, firm)      || Corr Vell (Blue, likely)        |
| kills 4  deaths 3  dmg 812    || kills 9  deaths 1  dmg 2140     |
| mouselook 41% | speed p95 5.9 || mouselook 88% | speed p95 6.1   |
| [minimap: trail, 2 kill x]    || [minimap: trail, 9 kill x]      |
| wall-kill 0/12 (0-24% CI)     || wall-kill 5/14 (14-59% CI)  [!] |
| reaction p50 0.62 s (n=9/14)  || reaction p50 0.21 s (n=11/16)   |
| allegiance: Red whole session || allegiance: Blue -> Red @14:51  |
+-----------------------------------------------------------------+
| [lock scales] [diff view] [both in world] [make claim from this] |
+-----------------------------------------------------------------+
```

`lock scales` forces identical axes (honest comparison); `diff view` renders B as deltas from A; `both in world` draws both figures with A/B glyph tags.

### 4.3 Session page (the landing page)

```
| SESSION — Bad Space, 13:58 to 15:44, 106 min, 41 agents           |
| Three phases detected. Fighting concentrated in the courtyard     |
| (ss:place/courtyard-NE, 38% of deaths). Two clusters, LIKELY;     |
| 11 friendly-fire hits, mostly in Phase 2; 2 allegiance changes    |
| detected [why].                                                   |
| [figure: 512 m top-down. death dots by team colour, heat cells,   |
|  front polyline per phase, spawn rings, click anything]           |
| [ribbon: |----P1 build-up----|=P2 courtyard=|--P3 collapse--|]    |
| [strip:  deaths/min sparkline with phase boundaries marked]       |
| v Phases   v Teams (Red 18 firm | Blue 16 likely | 7 unaligned)   |
| v Bands (6 transient)  v Top actors  v Equipment  v Anomalies [!] |
```

### 4.4 Model (equipment type) page — "does it kill through walls"

```
| MODEL — "Tanto SMG" (creator Ryn Voss, handheld, 3 instances)     |
| WHO ^ class:handheld ^ arsenal                                    |
| Used by 6 avatars, both teams. 41 kills, 512 hits, type 102.      |
| Through-wall rate over FIRM verdicts 4/11 = 36% (Wilson 90% CI    |
| 15-64%); 9 MARGINAL (position error), 27 UNKNOWN (no track).      |
| Verdicts resolved 128/512, computing... [pause]  SUSPECT [why].   |
| Damage 32 nominal; adjusted to 21 median vs Red.                  |
| [figure: resolved shot lines, blocked red, clear green, faint;    |
|  unresolved shots drawn grey and thin]                            |
| [sweep: travel window 0.0..1.5 s -> rate 9% .. 41%, flip 0.9 s]   |
| v Instances (3) v Users (6) v Kills (41) v Verdicts (128/512)     |
| v Adjustment: median damage/initial 0.66 by script "AshArmour"    |
```

### 4.5 Team page (time-scoped)

```
| TEAM Red — 18 members at cursor 14:32, confidence LIKELY (0.41 m) |
| Inferred from 214 damage edges + shared group tag (12 of 18) +    |
| co-movement + restraint (n=63 probes). 11 intra-team hits flagged |
| friendly fire, 9 in a 40 s window in Phase 2 [why]. Overrides: 2. |
| [figure: force graph, node=avatar, edge=damage, red edges=FF]     |
| [strip: membership over time, one row per avatar, colour per      |
|  segment, defection boundaries marked, cursor line]               |
| [sweep: cluster threshold 0.3..0.9 -> churn per step]             |
| v Members (18; each: band chip, segments, [override from..to v])  |
| v Allegiance changes (2)  v Bands inside team (3)                 |
| v Friendly fire (11)  v Enemies faced  v Equipment used           |
```

### 4.6 Peek (the planned inspector floater)

A transient card on hover over any link or in-world marker: title, one-line lede, band chip, three top links. `Tab` promotes it to a full page in pane B. This is the plan's `ss_combat_inspector`; the deeper inspector role is a second reader.

## 5. In-world overlay — the figure of the open page

The overlay draws, gated exactly as the plan specifies (stationary + alt-cam + 5 s grace):

1. **Base layer, always on**: the session figure at 25% alpha — death crosses, deadliness cells, spawn rings, place outlines. An officer who alt-cams somewhere out of curiosity always has geography to click, and clicking bare ground opens that 16 m `spot`. Exploration never requires having navigated first.
2. **Focus layer**: the active page's figure, full strength.
3. **Pinned layers**: the pinned pages' figures, subject to the scale rule.

**Figures.** *Session/Phase*: death crosses (team colour, hollow when allegiance is weak), deadliness cells, front polyline with push arrows. *Place/Spot*: cell raised and outlined, deaths inside, sightlines reaching in. *Avatar*: full trail, alpha ramp old→new, thick where MOUSELOOK, eye glyph, kill ✕ / death ⊘, seated segments as a dashed ribbon riding the vehicle. *Duel*: two trails plus labelled exchange lines. *Band*: members' trails, a soft hull, arrows to shared targets. *Death*: the reconstruction — victim marker, fan of candidate positions across the travel window, per-candidate ray green CLEAR / red BLOCKED / amber MARGINAL / grey UNKNOWN, wall hits ◇. *Moment*: everyone frozen, yaw cones, uncertainty spheres. *Model*: resolved shots, faint.

**Encodings.** Colour = team only. Direction = geometry (arrowheads), never colour. Width buckets {1,3,6} = evidence quality (bridge > viewer > coarse). Alpha = age within the trail window. Glyph = state (eye = mouselook, chevron = crouch, ▲ = flying, ▭ = seated).

**Scale rule (alt-cam legibility).** Each figure declares a scale class: **micro** (<20 m), **local** (20–120 m), **region** (>120 m). The camera's ground footprint selects the in-scale class each frame. The focus layer always draws at full strength. Pinned layers one class away draw at 40% alpha and decimate to their top-N elements; two classes away collapse to a single centroid marker with a count label ("Tanto SMG · 512 shots ▸") that expands on click. Every figure has a hard primitive cap and prints "drawn 200 of 512" in the page's figure caption when it decimates. A pinned region-wide shot cloud can therefore never bury the six-ray LOS fan the officer is leaning in to read.

**Uncertainty drawing.** Interpolated segments dashed; gaps >5 s absent with a "?" tick. Weak allegiance = hollow ring with gap count proportional to (1−margin). Coarse positions get a translucent sphere of radius = position error (≥2 m xy, ≥4 m z), and a candidate ray whose verdict flips under that sphere is drawn amber (MARGINAL), not green or red. Every ray gets a dotted continuation past the wall so you can see what it hit. Nothing inferred is drawn like something measured.

**Clickable.** Every marker registers a screen rect + address. Left-click navigates Reader A, Shift+click opens in B, middle-click pins, hover shows Peek. Elements the open page mentions get a subtle halo — in-world link underlining — capped at 64 haloed elements (nearest to camera first, "+N more" in the caption) so aggregate and query pages cannot flood the scene.

## 6. Analysis model — derived quantities

| Quantity | Computation (bounded) | Uncertainty shown as |
|---|---|---|
| **Attribution set** (death) | DAMAGE with same `target` in `[t−20 s, t]`, grouped by (`owner`,`rezzer`); killing blow = last; share % = damage/total | *contested* flag when >1 owner's window (incl. ±1 s log lag) covers the blow; only then is the killer hedged |
| **Model key** | (creator, name normalised of digits/serials, attach class) → one Model | "n facts known of 6" bar |
| **Hostility(a,b)** | Σ damage a→b, 5-min half-life decay, computed per 60 s window | — |
| **Co-presence index** | per 5 s slot, agents bucketed into 32 m grid cells from track ticks; candidate pairs = same/adjacent cell in ≥2 slots. O(agents) per slot, never O(pairs) | pair count shown on the team why-page |
| **Restraint(a,b)** | **only** over co-presence candidate pairs; **one** LOS probe per 10 s of co-presence, **max 24 probes/pair/session**, drawn from a priority queue at a global budget of **32 raycasts/frame** in idle time. Restraint = probes that were CLEAR, range <40 m, and had no a→b damage within ±3 s. Typical courtyard fight: a few hundred candidate pairs × ≤24 ≈ 10⁴ raycasts spread over an hour | n printed; n<6 → "insufficient", term weight 0 |
| **Team clusters** | label propagation per 60 s window on hostility − λ·restraint − μ·co-movement, with hysteresis; seeds = shared active group when informative. **Runs immediately on hostility + co-movement alone; restraint enters as a refinement as probes land**, and the team page names which terms are currently in play | band chip from the margin between best and 2nd affinity; sweep over threshold; churn strip |
| **Allegiance timeline** | per-avatar piecewise-constant labels from the windowed clustering; adjacent equal segments merged; a boundary with firm margins on both sides is reported as an **allegiance change** | segments drawn on the team strip; each boundary links to its why-page |
| **Officer override** | an **interval claim**: "X on team T from t0 to t1" (default t0 = cursor, t1 = session end); overrides compose, later claims win, conflicts are listed | override claims appear in backlinks; conflicting overrides raise a conflict row on the team page |
| **Band** | sliding 90 s window over the damage graph + tracks: 2–5 avatars, zero mutual damage, ≥2 shared targets or 1 common aggressor, median mutual distance <40 m. Damage-edge counting only, no raycasts | membership fraction of the window; bands under 30 s are listed but not drawn |
| **Friendly fire** | intra-cluster damage; excluded from team evidence so FF cannot split a team | listed per team; each hit links to its event |
| **Front line** | per phase, midpoints of opposing pairs within 60 m, least-squares polyline; distance-to-front as an "unbending" coordinate | dashed where <4 pairs support it |
| **Phases** | change-point detection on (deaths/min, centroid separation, front displacement) | boundaries draggable; rename → span page |
| **Place stats** | 16 m cells: deaths, exposure (agent-minutes), lethality | cells with <2 agent-min hatched, not coloured |
| **LOS verdict** | candidates = attacker track positions across the travel window (≤7) + `source_pos`; cast eye (+1.6 m, retry +0.6) to victim chest. Then **jitter the deciding candidate** by its position error along the two axes perpendicular to the ray (4 extra rays). Agreeing → **FIRM**; disagreeing → **MARGINAL**; no samples → UNKNOWN | per-candidate list; **flip point printed in the lede**; sweep over the window; MARGINAL always counted separately |
| **Through-wall rate** | BLOCKED/(BLOCKED+CLEAR) **over FIRM verdicts only**; MARGINAL and UNKNOWN counted beside it. Verdicts for a model are resolved lazily in the same 32 raycasts/frame background queue; the page shows "resolved n/N" and the CI narrowing live | Wilson 90% interval, never a bare number |
| **Adjustment ratio** | `damage/initial` attributed to scripts in `modifications` | median + IQR per script/team; n shown |
| **Reaction latency** | backward search from first damage, bounded to **12 s**, probing at 0.5 s steps (≤24 raycasts) for the first CLEAR; no CLEAR in window → "unresolved", excluded from p50 and counted | "p50 0.62 s (n=9 of 14 resolved)"; 2 Hz sampling floor caveat |
| **Pre-aim fraction** | domain is **exactly (attacker, victim) pairs where a hit later landed**, over the 15 s before the engagement's first hit, yaw from bridge ticks, LOS probed at 1 s (≤15 raycasts/engagement). Never all-pairs | fraction over engagements with **Wilson 90% CI**, labelled "suspicion signal, not evidence"; why-page lists false-positive modes |
| **Behaviour profile** | mouselook %, p50/p95 speed, distance, seated time, airtime | source mix (bridge/viewer/coarse) as a stacked bar |

**Two kinds of number, never mixed.** Statistical quantities with a real denominator print as **intervals with n** (through-wall rate, pre-aim, adjustment ratio). Everything inferred from clustering or classification prints as a **qualitative band** — FIRM / LIKELY / WEAK / UNDETERMINED — mapped from the affinity margin, with the raw margin visible only on the why-page. No two-decimal clustering margin ever appears in a lede, because an officer will otherwise quote "0.81" in a violation report as if it were a probability. Every band chip is a link to its `why` page: inputs, the formula in one sentence, counter-evidence, known false-positive modes. Measured values are plain. Unknown values are a dim `—` plus the reason.

## 7. Walkthroughs

**Q1 — how did this raid go?** Combat ▸ Combat Log lands on `ss:session`; the world shows the whole-session figure. Scrub the ribbon; phase boundaries snap. Click **P2 courtyard** → phase page, front polyline animates its push. Click the hottest cell → `ss:place/courtyard-NE` → "31 deaths, 78% victims from Red". Click a death → the reconstruction. Back twice, pin the phase, name the trail "after-action" and dump it as the briefing outline.

**Q3 — why did X die at 14:32?** Type `Nyx` in the address bar → `ss:avatar/Nyx` → Deaths → 14:32:07 → §4.1. The four contributing hits each carry `[->]` which sets the world cursor and flashes the line. The lede has already told the skimming officer the verdict's fragility ("BLOCKED for travel < 0.9 s; CLEAR beyond") and that one candidate is MARGINAL because the attacker was on coarse location beside the wall. Open the **sweep** to see the shape of that dependence and click the 0.9 s column to put exactly that geometry in the world. Shift+click Corr → pane B. Add a claim; choose polarity `⚑` or `✓`; it lands in this death's backlinks forever.

**Q8 — did anyone behave suspiciously?** `ss:session` ▸ Anomalies → `ss:query/anomalies`. Open the top row: `ss:why/preaim-corr` — pre-aim in 4 of 12 engagements, 33% (Wilson 90% CI 13–62%), with the false-positive list (bullet travel, log lag up to 1 s, 2 Hz yaw sampling, minimap is legal, sound cues). "Show me a case" → the strongest instance as a moment page; the world shows Corr's yaw cone tracking a wall with Nyx behind it. The officer checks voice and records `✓ ss:claim/19 "reviewed, sound cue confirmed"` — a dismissal that a promotion reviewer six months later can distinguish from a flag without opening it.

**Intra-group skirmish.** Twenty-two Ashguard; the group-tag seed is useless, and the session page says "group tag uninformative". Clustering runs immediately on hostility + co-movement (WEAK), and firms to LIKELY over the next few minutes as restraint probes land for the ~180 co-present pairs — the team page names restraint as active with n=63. `ss:team/1?sweep=teamconf`: at threshold 0.4 the split is a clean 11/11; above 0.7 six drop to unaligned. Two of those six turn out to be a **band** that formed at 14:31 and dissolved at 14:36 — visible as one page, not three duels to cross-reference. One avatar's allegiance strip shows a real change at 14:51; the officer overrides *that interval only* ("Red until 14:51, Blue after"), the epoch bump recomputes the affected pages, and the provenance sits in the backlinks. Friendly fire inside each 11 stays flagged and never re-splits the cluster.

**FFA on a neutral sim.** No stable partition (margin below the WEAK floor). The session page says "no teams detected"; the overlay drops team colour and tints each avatar by their recent aggressor. The working nouns become **bands, duels and places**: the Bands section lists six transient alliances by duration and shared targets — "did those three actually gang up on him for two minutes" is one page with its own figure, hull and target arrows — and `ss:place/*` shows where the deathmatch clustered. Same reader, different links; no mode change.

**Remaining questions.** Q2 teams → `ss:team/*` + strip + sweep. Q4 → `ss:model/*` with the FIRM-only rate and CI. Q5 → Adjustments sections plus `ss:query/adjustments`. Q6 → `ss:avatar/*`. Q7 → Team ▸ Equipment, sorted by suspicion. Q9 → named trails (`promotion-nyx`, `violation-corr`, `briefing-2026-09-08`), pinnable and dumpable.

## 8. Risks and open questions

- **Text-widget cost.** Long `LLTextBase` pages with many URL segments are not free. Cap the lede and infobox, build accordion sections lazily, LRU 32 pages, hard row caps with "show all (n)".
- **Raycast budget contention.** Restraint probes, model verdicts and reaction searches share one 32-rays/frame idle queue. Priority is: open page > pinned pages > clustering ambiguity > background. If the queue starves, pages must keep showing "resolved n/N" honestly rather than a rate over whatever happened to be computed. Needs profiling on a real 40-agent fight.
- **Restraint validity, not just cost.** Even bounded, a CLEAR probe with no damage may mean "did not see", not "chose not to shoot". λ must be small and the term is deliberately excluded from the *first* clustering pass; if validation shows it mostly encodes "was looking elsewhere", it should degrade to a per-pair on-demand check rather than a clustering input.
- **Address stability.** Aggregate addresses travel between officers; event addresses do not. Content-hashed event addresses and a stub page for unresolvable links are the current answer; a shared session id negotiated over the bridge is the open question.
- **Confidence semantics.** Bands hide a real problem rather than solving it: nothing calibrates a clustering margin, because the session contains no ground truth to calibrate against. The bands are ordinal only, and the why-page must keep saying so. If officers start treating "LIKELY" as a number anyway, the next step is to show the *sweep* instead of the chip on team pages.
- **Auto-prose overconfidence.** A generated lede reads as authority. The style rule (every inferred clause carries a hedge, a band and a flip point; the generator refuses to emit an unhedged inference) should be lint-tested against synthetic sessions — including the inverse failure, hedging things that are not in doubt.
- **Band false positives.** Two strangers shooting the same enemy from the same corner satisfy the band test. Bands are therefore never drawn as allegiance and never feed clustering; they are a *lead*, and the page says so.
- **Getting lost anyway.** The trail page and pin rail are the antidote; if testing shows drift, add a "N clicks from session" crumb and a Home key.
- **Override conflicts.** Interval overrides compose and later wins, but two officers' contradicting intervals still need a conflict UI beyond the list row. Open.

---

**Critiques rejected.** (1) *Make confidence chips calibrated probabilities* — no in-session ground truth exists to calibrate against; ordinal bands plus a visible margin on the why-page are the honest option. (2) *Drop restraint from clustering and demote it to an on-demand tool* — it is the only term that survives the same-group-tag case both reviewers said to keep; bounding it is the fix, not removing it. (3) *The three-axis structure shows the ladder was oversold* — Victor's ladder is per-dimension; drawing WHO/WHEN/WHERE explicitly is the ladder done properly, not an admission that there isn't one. (4) *Scope local alliances out of v1* — the owner named the case, and a band costs only a windowed pass over damage edges, no new raycasts. (5) *Always hedge the killer's identity* — hedging uncontested attributions trains officers to ignore hedges; hedge exactly when >1 owner's damage window (plus log lag) covers the killing blow.
