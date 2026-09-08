# Entry 02 — Hypertext-first: the raid is a wiki

## 1. Thesis

A raid is not a table of events; it is a *body of interlinked subjects*. So the tool is not a floater with tabs, it is a **reader over an auto-generated wiki of the session**. Every noun an officer can meet — an avatar, a death, a hit, a weapon model, a bullet, a team, a duel, a 16 m patch of ground, a minute, a phase, a line-of-sight verdict, an officer's own written claim — has a **stable address**, a **page** rendered from the stores, **outbound links** to every other noun it mentions, and **backlinks** listing everything that mentions it. There is one page skeleton and one reader; the "sub floaters" the owner asked for are second instances of the same reader, opened side by side. The ladder of abstraction is not a separate mode — it *is* the link grammar: stepping up a rung means following a link to a coarser noun, stepping down means following a link to a concrete instance, and every page carries both directions in a fixed rail so you can never be stranded in the clouds or on the ground. The 3D overlay is the **illustration pane of the page you are reading**: it draws what the open page asserts, nothing else, and every marker in it is an anchor that links back into the wiki. Digging never loses the thread because the thread is a first-class object: back/forward, a breadcrumb, a pin rail, and a named **trail** (Memex-style) that doubles as the post-raid briefing outline.

## 2. Ladder — rungs as link types

Every page prints a **rung rail** with the current rung highlighted. Left is concrete, right is abstract. Moving is always a link click, never a mode switch.

| Rung | Page type | Step down (→ concrete) | Step up (→ abstract) |
|---|---|---|---|
| R0 | **Moment** `ss:moment/@t` — the world frozen at one instant: who is where, facing where, in mouselook, seated | (floor) "Raw records at ±2 s" facet | any avatar/place/event visible in it |
| R0.5 | **Event** `ss:event/<seq>`, `ss:death/<seq>` — one hit or one death | `?as=raw` (the JSON as received, log lag, trust flag) | the duel, the minute, the model |
| R1 | **Duel** `ss:duel/<a>-<b>` — everything ever exchanged between two avatars | list of its events; "show me a case" | both avatar pages, the team pair |
| R2 | **Career**: `ss:avatar/<id>`, `ss:equip/<id>` (one object), `ss:model/<hash>` (a weapon *type*) | its events, its deaths, its instances | its team, its phase breakdown |
| R3 | **Region of space/time**: `ss:place/<cell>`, `ss:phase/<n>`, `ss:span/<t0>-<t1>`, `ss:team/<n>` | its moments, its deaths, its occupants | the session |
| R4 | **Session** `ss:session` — the whole raid in one picture | phases, fronts, top actors | (ceiling) |
| R2′ | **Sweep** — a facet, not a page: `ss:death/8814?sweep=travel`, `ss:session?sweep=teamconf` | pick one column of the sweep → that instance | back to the unswept page |

The sweep facet is the Victor "abstract over parameters" rung and it is available on *every* claim that has a tunable: LOS across the bullet-travel window, team assignment across the confidence threshold, damage across adjustment scripts, deadliness across the session. It renders as small multiples (a filmstrip of miniature top-down figures), each cell clickable to step straight back down to that parameter value made concrete in the world.

Two grounding rules borrowed straight from the north star: (a) every page has a `?as=raw` facet showing the underlying records verbatim, so you can always check the abstraction against the ground; (b) every aggregate page has a **"Show me a case"** button that jumps to a representative concrete instance and sets the world cursor to it.

## 3. Information architecture

**Address grammar.** `ss:<type>/<id>[@<t>][?facet=…]`. Addresses are minted by the analysis layer and are session-local. They are rendered as real clickable links through a registered `LLUrlEntryBase` subclass on `secondlife:///app/sscombat/…` (the same mechanism that makes `secondlife:///app/agent/<id>/about` clickable today), which means links work inside every `LLTextBase` in the viewer: page bodies, tooltips, notifications, and chat — an officer can paste a link into group IM and another Soapstorm user in the same session clicks it and lands on the same page.

**Page types.** `session`, `phase`, `span`, `moment`, `event`, `death`, `duel`, `avatar`, `equip` (one object instance), `model` (equipment type: creator + normalised name + attach class), `team`, `place` (grid cell or officer-named area), `verdict` (one LOS claim), `why` (evidence page for one inference), `query` (a saved filter = a category page), `claim` (officer-authored note), `trail` (the investigation itself).

**One skeleton, every page.** Title + type glyph + confidence chip · **Lede** (2–3 auto-generated sentences, every noun a link, hedges mandatory) · **Infobox** (key/value facts, right column) · **Figure** (custom-drawn: minimap, timeline, sparkline, filmstrip) · **Sections** (accordion, page-type specific) · **What links here** (backlinks, always last) · footer facets `[figure][table][gallery][sweep][raw]`.

**Navigation model.** Back/forward stack per reader (Alt+←/→, mouse 4/5). Breadcrumb of the current descent, click any crumb to return. **Pin rail**: middle-click or `Pin` parks a page as a chip; pins survive navigation and are the officer's working set. **Split**: Shift+click opens the target in pane B of the same floater (or in Reader 2 if the floater is narrow); B has its own history. **Time coupling**: a padlock toggle binds the page's `@t` to the global scrub cursor, so scrubbing rewrites the open Moment page live; unlocked pages stay put while you scrub.

**Backlinks are real.** Page rendering emits its outbound refs into a reverse index (`ref → set<address>`); cheap edges (event↔avatar, event↔object, avatar↔team) are maintained incrementally at ingest, expensive ones (verdicts, claims, queries) are added when the page is first built. "What links here" therefore grows as you investigate, which is a feature: it shows what *you* have connected.

**Trails.** The visit sequence is recorded as a graph. `ss:trail/current` renders it as a node-link figure; `Name this trail…` freezes it as `ss:trail/promotion-nyx`. A trail can be dumped as a linked outline into chat — this is the briefing artefact and the promotion/violation dossier.

## 4. Screens

### 4.1 Reader (main floater, `ss_combat_reader`; Combat ▸ Combat Log)

```
+ Combat Log — Reader A -----------------------------------------------[_][X]+
| < > ^ |ss:death/8814@14:32:07                   [pin][B|][raw][?] |
| Session > Phase 3 "Courtyard push" > Nyx Ashgrave > Death 14:32:07          |
| rung: Moment | EVENT | Duel | Career | Phase/Place | Session   [figure|v]   |
+-----------------------------------------------------------------------------+
| pins: (S)ession (T)Red (A)Nyx (M)Tanto-SMG (Q)wall-kills                 [+]|
+--------------------------------------------+--------------------------------+
| DEATH of Nyx Ashgrave, 14:32:07.4      [!] | FACTS                          |
|                                            | victim   Nyx Ashgrave (Red)    |
| Nyx Ashgrave was killed by Corr Vell       | killer   Corr Vell  (Blue 0.81)|
| (Blue, conf 0.81) with a bullet from       | weapon   Tanto SMG  (likely)   |
| Tanto SMG. Four hits in 2.3 s did 87       | blow     32 dmg, type 102      |
| damage; the killing blow was 32. Line of   | pre-hits 4 / 87 dmg / 2.3 s    |
| sight over the travel window is BLOCKED    | LOS      BLOCKED (low conf) [?]|
| on 5 of 6 candidate positions — low        | log lag  0.6 s (est)           |
| confidence, see why.                       | trust    sim log (authentic)   |
|                                            +--------------------------------+
| [figure: top-down 32 m, victim x, killer   | WHAT LINKS HERE                |
|  fan of 6 candidate rays, wall hatched]    |  ss:avatar/Nyx (deaths)        |
|                                            |  ss:model/Tanto-SMG (wall-kill)|
| v Damage that led to this death        (4) |  ss:query/wall-kills           |
|   14:32:05.1 Corr Vell  18  102  [->]      |  ss:claim/12 "Corr pre-aim?"   |
|   14:32:05.9 Corr Vell  21  102  [->]      +--------------------------------+
|   14:32:06.6 Kest Wray  16  102  ff? [->]  | NEIGHBOURS IN TIME             |
|   14:32:07.4 Corr Vell  32  102  KILL[->]  |  -8 s  ss:death/8810 (Kest)    |
| > Line of sight (6 candidates)   [sweep]   |  +21 s  ss:death/8819 (Corr)   |
| > Equipment evidence                       +--------------------------------+
| > Adjustments (armour scripts)             | ALSO SEE                       |
| > Officer claims (1)              [+ note] |  ss:moment/@14:32:07           |
+--------------------------------------------+  ss:duel/Corr-Nyx  (3rd kill) |
| [<< 1s] [>] [1s >>] 14:32:07 ---#--|-----  |  ss:place/courtyard-NE        |
+-----------------------------------------------------------------------------+
```

Regions: **chrome row** (back/forward/up, address `line_editor`, pin, split-to-B, raw facet); **breadcrumb** (`text` with links); **rung rail** (buttons, current disabled); **pin rail** (button row, right-click ▸ unpin/rename); **body** = `LLTextBase` read-only with `parse_urls` for prose, `LLAccordionCtrl` of sections, custom `LLView` for figures; **infobox column** = key/value `text` widgets, italic + chip for inferred values; **transport strip** at the bottom (step, play, cursor, timeline with death ticks and damage density) — global, shared by all readers.

### 4.2 Split / compare (same floater, pane B)

```
+ Combat Log — Reader A ------------------------------------------[_][X]+
| < > ^ |ss:avatar/Nyx          || < > ^ |ss:avatar/Corr        [x]|
| Nyx Ashgrave (Red 0.93)       || Corr Vell (Blue 0.81)           |
| kills 4  deaths 3  dmg 812    || kills 9  deaths 1  dmg 2140     |
| mouselook 41% | speed p95 5.9 || mouselook 88% | speed p95 6.1   |
| [minimap: trail, 2 kill x]    || [minimap: trail, 9 kill x]      |
| wall-kill 0/12 (0-24% CI)     || wall-kill 5/14 (14-59% CI)  [!] |
| reaction p50 0.62 s           || reaction p50 0.21 s   [why]     |
+-----------------------------------------------------------------+
| [lock scales] [diff view] [both in world] [make claim from this] |
+-----------------------------------------------------------------+
```

`lock scales` forces identical axes on both figures (honest comparison); `diff view` renders B's numbers as deltas from A; `both in world` draws both subjects' overlay figures together with A/B glyph tags.

### 4.3 Session page (rung 4, the landing page)

```
| SESSION — Bad Space, 13:58 to 15:44, 106 min, 41 agents           |
| Three phases detected. Fighting concentrated in the courtyard     |
| (ss:place/courtyard-NE, 38% of deaths). Two clusters, confidence  |
| 0.74; 11 friendly-fire hits, mostly in Phase 2 [why].             |
|                                                                   |
| [figure: 512 m top-down. death dots by team colour, heat cells,   |
|  front polyline per phase, spawn rings, click anything]           |
| [ribbon: |----P1 build-up----|=P2 courtyard=|--P3 collapse--|]    |
| [strip:  deaths/min sparkline with phase boundaries marked]       |
|                                                                   |
| v Phases     P1 13:58 muster, 2 deaths | P2 14:19 courtyard, 31   |
| v Teams      Red 18 (0.93) | Blue 16 (0.81) | unaligned 7 [?]     |
| v Top actors Corr 9k/1d | Nyx 4k/3d | ...       [table][gallery]  |
| v Equipment  Tanto SMG 41 kills | GX grenade 12 | 6 unknown       |
| v Anomalies  3 wall-kill clusters, 2 fast-reaction, 1 speed  [!]  |
```

### 4.4 Model (equipment type) page — the "does it kill through walls" page

```
| MODEL — "Tanto SMG" (creator Ryn Voss, handheld, 3 instances)     |
| Used by 6 avatars, both teams. 41 kills, 512 hits, type 102.      |
| Through-wall rate 5/14 resolved verdicts = 36% (Wilson 90% CI     |
| 16-61%), 27 verdicts UNKNOWN (no track). Treat as SUSPECT, not    |
| proven [why]. Damage 32 nominal; adjusted to 21 median vs Red.    |
| [figure: all shot lines, blocked in red, clear in green, faint]   |
| [sweep: travel window 0.0 .. 1.5 s -> verdict flips at 0.9 s ]    |
| v Instances (3)  v Users (6)  v Kills (41)  v Verdicts (41)       |
| v Adjustment: median damage/initial 0.66 by script "AshArmour"    |
```

### 4.5 Team page

```
| TEAM Red — 18 members, confidence 0.93 (margin 0.41)              |
| Inferred from 214 damage edges + shared group tag (12 of 18) +    |
| co-movement. 11 intra-team hits flagged friendly fire, 9 of them  |
| in a 40 s window in Phase 2 [why]. Officer overrides: 2.          |
| [figure: force graph, node=avatar, edge=damage, red edges=FF]     |
| [sweep: cluster threshold 0.3..0.9 -> membership churn per step]  |
| v Members (18, each with confidence bar + [override v])           |
| v Friendly fire (11)   v Enemies faced   v Equipment used         |
```

### 4.6 Peek (the planned inspector floater)

A small transient card on hover over any link or in-world marker: title, one-line lede, confidence chip, three top links. `Tab` promotes it to a full page in pane B. This is the plan's `ss_combat_inspector`; the deeper "inspector" role is served by a second reader instance.

## 5. In-world overlay — the figure of the open page

The overlay draws **the union of the active page and the pinned pages**, gated exactly as the plan specifies (stationary + alt-cam + 5 s grace). Each page type has a *figure*:

- **Session/Phase**: death crosses (team colour, hollow when team inferred below 0.6), 16 m deadliness cells as low filled quads, the front polyline per phase with push arrows, spawn rings.
- **Place**: the cell raised and outlined; every death inside it; sightlines that reach into it drawn as thin rays from where shots came from.
- **Avatar**: full trail, alpha ramp old→new, thick segments where `AGENT_MOUSELOOK`, eye glyph, kill ✕ and death ⊘ markers, seated segments drawn as a dashed ribbon riding the vehicle.
- **Duel**: only two trails plus exchange lines, each labelled with damage.
- **Death**: the reconstruction — victim marker, the fan of candidate shooter positions across the travel window with per-candidate ray, green CLEAR / red BLOCKED / grey UNKNOWN, wall hit points marked with a small ◇.
- **Moment**: everyone frozen, yaw cones, position-uncertainty spheres.
- **Model**: all shots by that model, faint, blocked ones red.

**Encodings.** Colour = team only. Direction = geometry (arrowheads), never colour. Width buckets {1,3,6} = evidence quality (bridge sample > viewer update > coarse). Alpha = age within the trail window. Glyph = state (eye = mouselook, chevron = crouch, ▲ = flying, ▭ = seated).

**Uncertainty drawing.** Interpolated track segments are dashed; gaps >5 s are simply absent with a "?" tick at the break. Inferred team = hollow ring with a gap count proportional to (1−confidence). Coarse-location positions get a translucent sphere of radius = position error (≥2 m in z). Every LOS ray gets its verdict colour *and* a dotted continuation past the wall so you can see what it hit. Nothing inferred is ever drawn in the same style as something measured.

**Clickable.** Every marker registers a screen rect + address. Left-click navigates Reader A, Shift+click opens it in B, middle-click pins it, hover shows Peek. Conversely, the elements the open page mentions get a subtle halo — the in-world equivalent of link underlining — so "what am I looking at that the article talks about" is answerable at a glance.

## 6. Analysis model — derived quantities

| Quantity | Computation | Uncertainty shown as |
|---|---|---|
| **Attribution set** (death) | DAMAGE with same `target` in `[t−20 s, t]`, grouped by (`owner`,`rezzer`); killing blow = last; share % = damage/total | log-lag estimate per event; events merged from `fd` marked "precise" |
| **Model key** | (creator, name normalised of digits/serials, attach class) → one Model; instances aggregated | "n facts known of 6" bar on the infobox |
| **Hostility(a,b)** | Σ damage a→b with 5-min half-life decay | — |
| **Restraint(a,b)** | fraction of opportunities (LOS CLEAR, range <40 m, ≥2 s) with zero damage a→b | count of opportunities; small n = "insufficient" |
| **Team clusters** | label propagation on hostility − λ·restraint − μ·co-movement; seeds = shared active group | **confidence = margin** between best and 2nd affinity; sweep over threshold |
| **Friendly fire** | intra-cluster damage; suppressed as "team evidence" so FF cannot split a team | listed per team; each FF hit links to its event |
| **Front line** | per phase, midpoints of opposing pairs within 60 m, least-squares polyline; **distance-to-front** as a coordinate transform for the unbent view | dashed where <4 pairs support it |
| **Phases** | change-point detection on (deaths/min, cluster centroid separation, front displacement) | boundaries draggable; officer can rename → span page |
| **Place stats** | 16 m cells: deaths, exposure (agent-minutes), lethality = deaths/exposure | cells with <2 agent-min drawn hatched, not coloured |
| **LOS verdict** | raycast from each candidate shooter position over the travel window (+1.6 m eye, retry +0.6) to victim chest; CLEAR if any clear | per-candidate results always listed; sweep over travel window; standing caveat text |
| **Through-wall rate** (model) | BLOCKED / (BLOCKED+CLEAR) | **Wilson 90% interval**, plus UNKNOWN count shown separately; never a single number |
| **Adjustment ratio** | `damage/initial`, attributed to scripts in `modifications` | median + IQR per script/team; n shown |
| **Reaction latency** | first CLEAR LOS onto target → first damage onto that target | histogram; flagged only below the 2 Hz sampling floor caveat |
| **Pre-aim fraction** | share of time yaw is within 8° of a target while LOS is BLOCKED and that target is later hit | explicitly labelled "suspicion signal, not evidence of cheating"; links to `why` page listing false-positive causes |
| **Behaviour profile** | mouselook %, p50/p95 speed, distance, seated time, airtime | source mix (bridge vs viewer vs coarse) as a stacked bar |

Every inferred value in the UI is rendered italic with a confidence chip, and **the chip is a link to a `why` page** listing inputs, the formula in one sentence, the counter-evidence, and the known false-positive modes. Measured values are plain. Unknown values are a dim `—` plus the reason ("no track samples in window").

## 7. Walkthroughs

**Q1 — how did this raid go?** Open Combat ▸ Combat Log; the reader lands on `ss:session`. The lede names three phases and the courtyard hotspot; the world shows the whole-session figure (rung 4). Scrub the ribbon: phase boundaries snap. Click **P2 courtyard** → phase page; the front polyline animates its push. Click the biggest heat cell → `ss:place/courtyard-NE` → "31 deaths, 78% victims from Red". Click one death → step to rung 0.5 with the reconstruction in the world. Back twice, pin the phase, name the trail "after-action". The briefing is the trail dumped as an outline.

**Q3 — why did X die at 14:32?** Type `Nyx` in the address bar (autocomplete on nouns), open `ss:avatar/Nyx` → Deaths section → 14:32:07 → the death page in §4.1. The four contributing hits are listed with a `[->]` that sets the world cursor to each hit and flashes the line. Section *Line of sight*: 6 candidates, 5 BLOCKED; open the **sweep** facet — the filmstrip shows the verdict flipping to CLEAR at travel = 0.9 s, i.e. the claim is entirely dependent on the travel-time assumption. Click that column → the world shows exactly that geometry. Shift+click Corr Vell → pane B; compare. Add a claim note; it becomes `ss:claim/12` and appears in the death page's backlinks forever.

**Q8 — did anyone behave suspiciously?** `ss:session` ▸ Anomalies → `ss:query/anomalies`, a category page grouping wall-kill clusters, fast reactions and speed outliers, each row a link. Open the top row: `ss:why/preaim-corr` — pre-aim fraction 0.34 over 12 engagements, with the false-positive list (bullet travel, log lag up to 1 s, 2 Hz yaw sampling, minimap use is legal, sound cues). "Show me a case" → the strongest instance as a moment page; the world shows Corr's yaw cone tracking a wall with Nyx behind it. Nothing says "cheater": the page says "signal, n=12, three benign explanations".

**Intra-group skirmish.** Two Ashguard teams; active group tag is identical for all 22, so the seed term is useless. The clustering falls back to hostility plus **restraint** and co-movement; `ss:session` reports confidence 0.58 with a warning "group tag uninformative". Open `ss:team/1?sweep=teamconf`: at threshold 0.4 the split is clean at 11/11; above 0.7 six avatars drop to unaligned. The officer opens the six, sees three that only ever fired at one side, and applies overrides from the team page; overrides are recorded as claims with backlinks, so the provenance of the final roster is auditable. Friendly-fire hits inside each 11 stay flagged but never re-split the cluster.

**FFA on a neutral sim.** Clustering finds no stable partition (margin < 0.15). The session page says so plainly — "no teams detected; showing everyone as unaligned" — and the overlay drops team colour entirely, colouring by *recent aggressor* instead (each avatar tinted by whoever last hit them). The useful nouns become duels and places: the Duels section ranks pairs by exchanged damage, and `ss:place/*` shows where the deathmatch actually clustered. The same reader, different links; nothing about the UI changes mode.

**Remaining officer questions.** Q2 teams → `ss:team/*` + sweep. Q4 wall-kills → `ss:model/*` through-wall rate with CI. Q5 adjustments → the Adjustments section on model/team/avatar pages, and `ss:query/adjustments` for the cross-team table. Q6 avatar career → `ss:avatar/*`. Q7 team equipment → Team ▸ Equipment section, sorted by suspicion. Q9 workflows → named trails: `promotion-nyx`, `violation-corr`, `briefing-2026-09-08`, each a pinnable, shareable page.

## 8. Risks and open questions

- **Text-widget cost.** Long pages in `LLTextBase` with many URL segments are not free. Mitigation: cap the lede + infobox, build accordion sections only when opened, LRU page cache (32), and hard row caps with "show all (n)" links.
- **Address stability.** Event `seq` is session-local, so a link pasted to another officer only resolves if their session recorded the same events. Open question: mint content-hashed addresses for events (`owner+target+t+damage`) so links survive across viewers.
- **Backlink index memory.** The reverse index is bounded by ingest edges (~4 per event); expensive backlinks are lazy. Needs eviction alongside the retention sweep.
- **Auto-prose overconfidence.** A generated lede reads as authority. Mitigation is a hard style rule: every inferred clause must carry a hedge word and a chip; the lede generator refuses to emit an unhedged inference. This should be lint-tested against synthetic sessions.
- **Getting lost anyway.** Wikis lose people. The trail page and the pin rail are the antidote; if user testing shows drift, add a persistent "you are N clicks from the session page" crumb and a `Home` key.
- **Clustering churn.** Team assignment can flip mid-session; pages must be honest that membership is time-varying. Current design shows membership at the cursor time and a churn strip on the team page — needs validation on a real intra-group fight.
- **Officer overrides vs. evidence.** Overrides are claims with backlinks, which is right for auditability, but there is no conflict UI when two overrides disagree with the clustering. Open.
