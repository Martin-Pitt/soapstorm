# Combat Log — UX and UI design

Definitive design for the Soapstorm Combat Log tool. The engineering plan
(`glistening-toasting-pebble.md`) fixes the stores, the bridge wiring, the overlay gate, the two
floaters and the five analysis modules; this document fixes everything above them: the levels, the
page grammar, every screen, the overlay and the order they are built in.

| Document | Holds |
|---|---|
| **combat_log_ux.md** (this one) | thesis and levels, information architecture, screens, overlay, walkthroughs, build order, open questions |
| **[combat_log_analysis.md](combat_log_analysis.md)** | §5, the analysis model: every derived quantity, its uncertainty and its cost |
| **[combat_log_rules.md](combat_log_rules.md)** | the rule catalogue's mechanics (§5.22) and the worked Ashguard rule set (Appendix A) |
| **[combat_log_glossary.md](combat_log_glossary.md)** | the shared-language pass: which words are the community's, which are ours, and which should change before they become labels |

It is a synthesis. The skeleton is entry **[01]** (levels-first). Grafts are credited inline by
entry id: **[02]** hypertext, **[03]** officer-workflows, **[04]** cartographer, **[05]** timeline,
**[06]** forensics, **[07]** equipment, **[08]** allegiance, **[09]** progressive, **[10]**
in-world, **[11]** comparative, **[12]** fairness.

A second pass (2026-09-08) folded in domain material that arrived after the synthesis was written:
the owner's SLMC answers **[owner]**, a scan of ~130 Ashguard raid reports and their discussion
threads **[reports]**, the written sim rules with their cross-group variance **[rules]**, and
Ashguard's own internal handbooks **[merits]**. Those four tags carry the same weight as an entry
id, but they mark a different kind of claim: not "this design idea came from there" but "the world
is like this". Where a domain fact and a design idea disagreed, the fact won and the design moved.

A third pass (2026-09-08) closed the gaps a critical review found: attendance scoped honestly to one
session, escalation sub-classes given their own field and evidence, *contested* defined once
([analysis §5.9a](combat_log_analysis.md)), the rule catalogue's data/code line drawn where it
actually falls ([rules §5.22](combat_log_rules.md)), the friendly-fire circularity resolved into a
fixed two-pass order ([analysis §5.10](combat_log_analysis.md)), and the R4 peek headline brought
under the document's own "never a bare rate" rule (§3.7).

A fourth pass (2026-09-08) applied the owner's verdicts and split the document in three. The verdicts
moved real design: events are stamped with the simulator's frame counter rather than a script clock;
region-system integration left v1 for two debug-only log-to-file settings; the 16 m zone grid was
deleted outright, because ground here is prims and mesh and clicking it infers nothing; the rule
catalogue ships empty; the dev-view companion feature was dropped for a different one; and death
**reconstruction** — a ghosted, scrubable replay of one death in the world — became a first-class
feature (§3.10) [owner].

A fifth pass (2026-09-08) applied one further ruling, and it cut deeper than its size suggests
[owner]. **One group's internal practice is not the tool's shape.** Pay, raid pay and motes, merits
and merit categories, commendations, and the specific raid-report layout the fourth pass had built a
screen around — Personnel, (Militia), Report, Incidents, an "OIC HUD Output" block of
`uuid,minutes` lines, forum-post tags — are all *Ashguard's* practice, and every SLMC group runs its
own. So the Report noun became a generic **session summary** whose sections and line formats are
group data (§3.9), award observables became **combatant summary statistics** with no category names
([analysis §5.24](combat_log_analysis.md)), and the Ashguard format survives in exactly one place:
a boxed worked example, credited, next to the mechanism that would render it. Reward vocabulary is
gone from every label and every generated sentence. The rule is the same one the catalogue already
obeys — the tool ships the mechanism empty and the group fills it.

---

## 1. Thesis and the levels

**Thesis.** The design principle is the **Ladder of Abstraction**, and it is not a description of
this tool, it *is* the tool [01]. (That phrase is where the idea comes from and is the only place
this document uses it; everything downstream — every label, key and screen — says **level**.) The
officer stands on exactly one level, looking at exactly one subject, and two keys move them: `[`
zooms in toward the concrete, `]` zooms out toward the abstract. Navigation is two-dimensional and
orthogonal — **levels are vertical, the wiki is horizontal**. Vertical movement changes altitude and keeps the subject;
horizontal movement changes the subject (avatar → weapon → team → death) and keeps the altitude. The
breadcrumb draws the two differently so they can never be confused [01]. Three invariants hold it
up. **Up-then-down is the identity**: ascending writes the destination's *anchor* to the instance
you came from, so `[` returns that instance, and entering an aggregate cold lands on its *witness*
(medoid) with the rail printing the exact landing before the key is pressed [01]. **The haze rule**:
no aggregate may be drawn without the unknown fraction it was built from drawn in the same mark
[01], generalised from [06] as *the certainty of a mark is bounded by the certainty of its worst
input, never by the crispness of the computation*. And **the tool volunteers its own doubts**: an
officer never has to already suspect an answer to be shown it [08].

### 1.1 The six levels

Each level names its **subject type**, its **zoom out** (a compression operator that becomes a
breadcrumb token) and its **zoom in** (a grounding gesture that always lands on a *named* instance
the rail printed first).

| Level | Subject | Zoom out (`]`) | Zoom in (`[`) |
|---|---|---|---|
| **R−1 Record** | one received line: raw payload, trust flag, sender/owner keys, receive time, sim frame stamp, the frame→time fit that mapped it, log lag, `fd` dedup partner, salvage mask | `interpret` → typed event | — (ground) |
| **R0 Moment** | an instant `t ± 1 s`: the world overlay plus the Moment strip | `over time` → the bout containing it | `Ctrl+Enter` or the raw chevron → R−1 |
| **R1 Episode** | a *bout* (spatiotemporal cluster) or a *life* (spawn → death). Two faces: **Ledger** collapses space, **Plan** collapses time; Plan has two renderers, **World** and **Chart** | `over parameters` → R2; `over avatars`/`over bouts` → R3 | click any mark; bare `[` → anchor, else witness |
| **R2 Sweep** | one claim swept across one parameter, drawn as a strip: parameter on x, outcome as colour, current value marked | `across cases` → R3 (same sweep small-multipled) | click the strip → R0 at the moment that value implies |
| **R3 Field** | a set across two or more dimensions. Faces: **Matrix**, **Braid**, **Front**, **Roster**, **Board** | `summarise` → R4 | every cell, band and dot is a handle; `Shift+click` lands on its medoid instance |
| **R4 Session** | the raid: phases, ledgers, **Ground** face, headline counts, and the questions the tool flags on itself | none, and the rail says so | every number and row descends to its anchor or witness |

Faces and renderers are free switches: they do not touch the breadcrumb, because they are not a
change of level.

### 1.2 What is sweepable at R2

Not just line of sight. Travel time τ; the death-attribution window; team confidence threshold θ;
awareness cone θ_a; script inclusion; track-quality cutoff; the bout segmentation constants (ε, κ,
the lull rule) [01]; the group-tag ablation [08]; and the **event anchor itself**, re-run one second
either side of the stamped frame — the log's own lag, which is the same order as the travel window
being swept [12]. **Rule thresholds sweep too**: the 5 m kill / 10 m wound explosive radii, the 25 %
armour cap, sprint and dash limits — every numeric constant an officer types into the rule catalogue
([rules §5.22](combat_log_rules.md)) is a parameter, because the number differs by region and an
officer needs to see who sits either side of the one their own
sim wrote down [rules]. Every noun's R2 carries an ordered **claim list** drawn as chips; `]` opens the first, and
switching chips changes the claim without changing level. That is what welds the wiki to the levels.

### 1.3 `[sweep this]` — climbing from the reading you distrust

Every uncertainty badge, strain chip, matrix cell, confidence band and verdict miniature carries a
right-click `[sweep this]` [08]. It ascends to R2 with that specific reading as the sweep's subject,
so the climb always starts from the thing the officer doubts rather than from a generic parameter
page. This is the single most-used zoom-out in practice.

---

## 2. Information architecture

### 2.1 Nouns

Every one is an article with links and backlinks: **Session**, **Phase**, **Bout**, **Moment**,
**Life**, **Avatar**, **Team**, **Band** (a transient 2–5 person local alliance [02]), **Death**,
**Damage**, **Volley**, **Equipment** (Weapon / Projectile / HUD / Deployable / Vehicle / Mount),
**Model** (an equipment family keyed across instances), **Object** (a non-agent damage target —
turret, vehicle, deployable [05]), **Script** (a damage-adjustment script from `modifications`),
**Zone** (a spawn safe-zone an officer marked by hand, below), **Verdict** (a stored LOS result with its sweep), **Signal**
(the documentation page for one behavioural signal [12]), **Claim** (an officer assertion),
**Rule** (one entry of the region's rule catalogue [rules]), **Summary** (the session summary, a
configurable export the officer assembles from the tool's own facts), **Case** (pins + notes),
**Trail** (a named visit sequence [02]), **Record**.

Four are load-bearing and absent from the raw log. **Volley** — consecutive DAMAGE sharing
`(owner, rezzer, target)` with gaps ≤ 2 s [01] — is the unit an officer means by "he shot him";
attribution, equipment stats and every hostility edge speak in volleys, so a stray splash tick can
never carry an execution's weight [08]. **Object** exists because an object has a health story the combat log never tells: LBA objects
publish their HP in their description and hover text, which the viewer reads for anything in view, so
the Object page carries a health series with a coverage caveat and says **unknown** only where the
object was out of sight [05][owner].

**Rule** exists because the checkable rule set is *data for its numbers and its text*, wired to a
fixed set of predicate shapes the viewer does implement in code
([rules §5.22](combat_log_rules.md)). Each Rule is a claim
scoped to a region — text, numeric parameters, a measurability class, an OIC exception list, and an
optional validity window when it was negotiated mid-fight ("asked not to use basket grenades at
17:26"). The same behaviour is legal on Resdayn and banned on Chaos ground, so the catalogue is
per-region and editable, and every rule check produces candidate rows for an officer to weigh, never
a verdict [rules][03]. **Summary** exists because *some* artefact always exists: every group writes
something up after a raid, and a tool that cannot fill it will not be used. But the shape of that
write-up is the group's, not ours [owner]. So Summary is a set of sections assembled from facts the
tool already holds — duration, present combatants and civilians by side, escalation timeline where
enabled, deaths and kills, notable episodes, incidents with their evidence links, per-combatant
activity — and **which sections appear, what they are called, in what order, and how one participant
renders as a line is group configuration**, data beside the rule catalogue and the group registry.
The tool ships no template. Its job is to *fill whatever shape the group defined*, not to invent one
and not to adopt somebody else's [owner][03]. Both are first-class pages with backlinks: a Rule's
backlinks are its candidate hits, a Summary's are the evidence pinned into it.

**Zone is almost nothing in v1, and that is a correction rather than a cut** [owner]. An earlier draft
divided the region into 16 m cells, gave every cell a page and made clicking bare ground a navigation
gesture. That was wrong about the medium: ground here is prims and mesh, parcels are region-wide, and
nothing about a patch of terrain can be inferred by clicking it. So v1 has exactly one kind of Zone —
a **spawn safe-zone the officer marks by hand**, a box or simple polygon, named, living for the
session. Everything else spatial is either an **auto-detected hotspot**, unnamed and derived from the
landings themselves ([analysis §5.7a](combat_log_analysis.md)), or it waits: the future shape is
**region owners designing polygon zones**, mesh-based the way a navmesh is, shareable with the
officers who fight there. Anything that wanted richer zones — objective time, Place and Hotspot
naming — prints *requires owner-designed zones (future)* rather than being faked from a grid.

**Two words are fixed, and the rest wait for a glossary pass.** A **combatant** is an active
participant on the battlefield; a **civilian** is the historical SLMC word for someone who is not one.
Prose and UI copy use those two. **Avatar** stays the name of the noun and of the page type, and means
the raw entity — the thing with a UUID, a track and a name — so "the Avatar page of a combatant" is
the correct sentence and not a slip [owner].

The split is not ours to invent: the owner's own active-combatants board already runs the test
[owner]. An agent **becomes** a combatant on being the `owner` of a DAMAGE, DEATH or OBJECT_DEATH
event, or on showing `AGENT_MOUSELOOK` or `AGENT_ON_OBJECT` while standing on a damage-enabled parcel
— which is what "combat area" means. They **stay** one while in mouselook or seated, and otherwise
expire **two minutes** after their last damage activity; leaving the region drops them. Everyone else
present is a civilian. The tool uses that test verbatim, so its headcounts and the board's agree.

### 2.2 The Level Row rule

Every article carries six buttons `[-1]…[4]`, enabled only where that noun means something at that
level, each labelled with what you would see. The mapping follows one rule so two implementers cannot
disagree [01]:

| Level | Rule for any noun N | Avatar | Team | Model | Death | Script |
|---|---|---|---|---|---|---|
| R0 | N at the cursor instant | body, yaw, flags | members drawn | its shots in flight | the moment | its adjustments |
| R1 | the smallest episode(s) containing N | its life | Ledger filtered to members | its volleys | its bout | ρ per volley |
| R2 | N's **default claim**, swept | awareness lead vs θ_a | membership vs margin | through-wall vs τ | LOS vs τ | ρ vs inclusion set |
| R3 | the peer set N belongs to | its Matrix row+column, its Braid strand | Braid and Front | through-wall field across models | its matrix cell | ρ across teams |
| R4 | N's row in the session ledger | avatar ledger row | team ledger row | equipment ledger row | one tick in the counts | script ledger row |

### 2.3 Addressing and links

Address grammar `ss:<type>/<id>[@t][?facet=…]`, rendered through a registered `LLUrlEntryBase`
subclass on `secondlife:///app/sscombat/<type>/<id>` [02], so noun mentions are real clickable links
inside any `LLTextBase` — article bodies, tooltips, notifications, chat. Aggregate addresses
(avatar, model, team, zone) are keyed on UUIDs or derived keys and travel between officers; **event
addresses are session-local and the UI says so**, offering a best-effort content-hash form
(`owner+target+t±0.5 s+damage`) on copy, and landing unresolvable links on a stub page that names
what it sought [02].

### 2.4 Backlinks and typed claims

Page rendering emits outbound refs into a reverse index (`ref → set<address>`). Cheap edges
(event↔avatar, event↔object, avatar↔team) are maintained at ingest; expensive ones when a page is
first built [02].

Officer claims carry a **polarity** rendered in every backlink list: `⚑ FLAG`, `✓ CLEAR`,
`⇄ OVERRIDE`, `✎ NOTE` [02]. An officer seeing "3 claims" can tell two are exonerations
without opening them. Every claim is data, not a mutation: listed in the **Ledger**, undoable,
attributed, and always reporting what it changed and what evidence it overrules [08]. Flags
additionally carry a **disposition** — `open` / `checked — plausible cause` / `escalated` — plus a
note, officer initials and a timestamp that travel into the exported debrief text [11].

### 2.5 Two navigation histories, drawn identically, never merged

Because "what I was reading about" and "where I was reading it" fail separately [08]:

- **Noun chain** — `[<] [>]` in the Article floater over the subject sequence, with the breadcrumb.
- **View chain** — `[<] [>]` in the Levels floater over the *View*, which is one struct that a
  breadcrumb chip, a pin, a compare slot, a link and a marked chapter all literally **are** [11]:

```cpp
struct View { U8 level; NounRef subject; U8 face, renderer;
              F32 cursor; F32 window; ParamSet params;
              Filter filter; Facet rowFacet, colFacet; Selection sel; };
```

Anything that moves the cursor or a parameter pushes the previous View first, so a multi-hop
violation dig ends with one click back to the framing it started from. `[mark]` names the current
View; marked Views become the briefing chapter list [08]. Back/forward restores query **and** cursor
time exactly [11].

Separately from both, every `(level, subject)` pair holds an **anchor**: the concrete instance last
visited beneath it this session. `]` writes it, `[` reads it, and the rail labels which of `(back)`
or `(witness)` you are about to get [01]. Three mechanisms, three questions: *my path*, *my framing*,
*this subject's ground truth*.

### 2.6 Pinning, Board, Case, Trail

`Ctrl+click` or `P` drops a noun in the Pin tray. The R3 **Board** draws one cell per pin on shared,
scale-locked axes — that is how small multiples get built [01][11]. Pins plus notes make a **Case**,
copyable to the clipboard as plain text. **Clipboard is the whole persistence story and the owner
accepted it**: a Case and a Trail die with the session, and what survives is the text the officer
pasted somewhere [owner]. The visit sequence is separately recorded
as a graph that can be named and frozen (`ss:trail/west-gate-review`) and dumped as a linked outline
[02]; the fight review, the violation case file and the post-raid briefing fall out of
navigation the officer already did. The only files v1 ever writes are the two debug logs of §7, which
are off by default and are not this.

### 2.7 One ParamSet, one degraded-terms strip

`τ`, `θ`, half-life, window, ablations and `include untrusted` live in exactly one **ParamSet** owned
by the session [08]. Two R2/R3 faces can never disagree about teams at the same moment, because
there is one moment and one set of knobs. The Sweep stage is the only place that shows more than one
ParamSet, explicitly as comparison; adopting one there mutates the global set and every open view
redraws.

When a term cannot be computed for this raid — no respawn signal, proximity budget exceeded, no
single front, no marked spawn zone — the status line prints a chip (`no-respawn`, `prox-capped`,
`no-front`, `no-zones`) and every confidence that would have used it is marked [08]. The tool
never silently loses a term.

### 2.8 Two kinds of number

Enforced globally [02]:

- Quantities with a real denominator print as **intervals with n**: `67% [21–94%] · n=3`. Never a
  bare rate.
- Anything from clustering or classification prints as an **ordinal band** — FIRM / LIKELY / WEAK /
  UNDETERMINED — with the raw margin visible only on the why-page. No clustering margin ever appears
  as a decimal in a lede, because an officer will quote "0.81" in a violation report as if it were a
  probability.

Every band chip links to its **Signal page** [12]: definition, one-sentence formula, known
false-positive causes, live instance list. Measured values print plain. Unknown values print a dim
`—` plus the reason.

### 2.9 Who is looking, and how long they have

Four audiences, not one [owner]. The **OIC who stayed behind** has the whole session and a report to
write. The **OIC who also fought** has ninety seconds between engagements. An **individual combatant
reviewing themselves** — the most common user by headcount, and the one who most wants their own
kill distances, deaths and loadout timeline — opens the tool on a subject they already care about.
A **spectator** is just curious. Nothing in the design gates on rank: every page works for all four,
because everything the tool shows is derived from a region-wide log that all of them can already
read.

Two modes, and they are not a preference but a *depth of entry* [03]:

- **Peek** — the R4 stage plus the ribbon, readable in about five seconds while standing still:
  duration so far, headcount per side with the militia band drawn separately, deaths since the last
  look, the current escalation tier when that lane is switched on ([analysis §5.23](combat_log_analysis.md), off by default), and any
  question the tool has raised about itself. Live mode
  follows now; the overlay's permanent base layer is enough geography to orient on. Peek is what a
  fighting OIC uses, and it is the default when the floater opens in Live [owner].
- **Study** — the levels proper: descend, sweep, dig, pin, file claims. Study assumes the officer
  has parked and is alt-camming.

The one behavioural rule that follows: **peeking never costs a raycast**. Everything on the R4 stage
and the ribbon is an ingest-time accumulator ([analysis §5.21](combat_log_analysis.md)), so a live peek during a firefight cannot
stutter the frame the officer is about to die in.

**Privacy is a default, not a feature.** Officers asked for "a tool for OICs to use to investigate
potential issues", explicitly not active policing, "because people like to panic" [owner]. So: every
behaviour surface, every rule candidate list and every compliance list is visible to the person
looking at it and to nobody else; nothing is broadcast, chatted, or written to the region; there is
no export path that produces a public accusation, only Case text the officer has read and chosen to
paste. The two debug log files (§7) are the sole exception and are not one in spirit: they are off by
default, they write to the user's own log directory, and they record what arrived rather than what
the tool concluded. The session summary export is the one artefact designed to be shared, and it
contains prose the officer wrote plus counts, never a tool-authored verdict.

---

## 3. Screens

Two floaters, one overlay, one shared 2D renderer.

`ss_combat_log` is the **Levels** floater: fixed chrome with a centre stage that swaps by level — six
stages, nothing duplicated. `ss_combat_inspector` is the **Article** floater, keyed by noun id so
several can be open and compared; it is level-agnostic, the horizontal axis [01].

### 3.1 Levels floater — fixed chrome

```
┌ COMBAT LOG ─ 14:02–15:47 ─ ●REC ─ bridge ok ─ 41 agents ─ drop 0 ─ rtt 141ms ─┐
│ ◀ ▶ ⚑mark │[4]Session ⌄into [1]Bout "West Gate" ⌄anchor [0]DEATH Vex←Cadmus  │
│ params: τ 0.0–1.5 │ θ 0.45 │ half-life 90s │ window 20s │ untrusted OFF [edit]│
├──────┬──────────────────────────────────────────────────────────┬─────────────┤
│LEVEL │                                                          │ EVIDENCE    │
│  ▲ ] │                                                          │ ┌─────────┐ │
│[4]Ses│                                                          │ │ what is │ │
│[3]Fld│                    S T A G E                             │ │ this    │ │
│[2]Swp│              (swaps with the level)                      │ │ built   │ │
│[1]Bou│                                                          │ │ from    │ │
│[0]Mom│  ← [ lands on: DEATH Vex←Cadmus  (back)                  │ └─────────┘ │
│[-1]Rc│                                                          │ coverage 87%│
│  ▼ [ │                                                          │ pins: 4  ▤  │
├──────┴──────────────────────────────────────────────────────────┴─────────────┤
│ deaths  ▏ ▏  ▏▏▏ ▏     ▏▏▏▏▏  ▏ ▏      ▏▏ ▏▏▏▏▏▏ ▏  ▏     ▏▏  ▏   ▏ ▏   ▏▏▏  │
│ dmg A/B ▁▂▃▅▇█▇▅▃▂▁▁▂▄▆█▇▄▂▁▁▁▂▅▇█▆▃▁▁▁▂▃▅▆▇█▇▆▄▂▁▁▁▂▃▄▃▂▁▁▁▁▂▃▅▇█▇▅▃▂▁▁▁▁  │
│ bouts   ▓▓▓▓▓░░░░▓▓▓▓▓▓▓▓░░░░░░▓▓▓▓▓▓▓▓▓▓▓░░░░▓▓▓▓▓▓▓▓▓▓▓░░░░░░░▓▓▓▓▓▓▓▓▓▓▓  │
│ escal A ──basic────────────┤EXP 14:41├────────┤VEH 15:02├───────────────────  │
│       B ──basic──────────────────────────────────┤EXP 15:07├┤VEH▨ 15:09├────  │
│ presnt  A 9 ▁▁▂▂▂▂▂▂▁▁▁▁▂▂▂▂▂▂▂▂▂ +2 militia ▏  B ~7 ▁▁▂▂▃▃▃▃▂▂▂▂▁▁▁ ⚑ last  │
│ struct  ████│████│████│██~~│~~~~│~~··│····│~~~~│████│████│████  S .71 null .42│
│ quality ████████████▒▒▒▒████████████████▒▒▒▒▒▒███████████████████████████████ │
│         └──────────────────────[▮]══════════════════────────────────────────┘ │
│ ◀◀ ◀ ▶ ▮▮ ▶ ▶▶  0.25× 1× 4×  │ 14:31:07.4 │ ±20 s │ LIVE ○   ⌖ follow cursor  │
│ degraded: no-respawn · prox-capped                                            │
└───────────────────────────────────────────────────────────────────────────────┘
```

**The level keys live in this floater and nowhere else.** `[` and `]` are active only while
`ss_combat_log` is open **and** focused; the moment focus leaves it they are ordinary viewer keys, so
nobody moves between levels by accident from a chat bar [owner]. The overlay is level-indexed and reacts
to the keys because the floater consumed them, not because it listens on its own.

**Breadcrumb row.** Vertical moves print as a tinted operator chip carrying a direction glyph and the
destination level (`⌄into [1]`); horizontal moves print as a plain untinted `→` at the same indent
[01]. Clicking a token jumps there; clicking the operator opens its explanation article. The chip bar
is editable: click to truncate, drag a chip into compare slot A/B [11]. `⚑mark` names the current
View as a briefing chapter [08]. XUI: `layout_stack` of `text`/`button` rebuilt on View change.

**Parameter strip.** The global ParamSet, read-only in every other screen with `[edit]` focusing here
[08]. XUI: `slider_bar` + `combo_box` + `check_box`, `control_name`-bound.

**Level rail** (86 px). Six cells, abstract at top, ground at bottom, each showing the subject it
holds; the cell below the current one prints the exact landing and whether it is `(back)` or
`(witness)` [01]. A cell greys when its subject is undefined and says what would define it. Custom
`LLView` (`SSLevelRail`) drawing `gl_rect_2d` + `LLFontGL::renderUTF8`.

**Stage.** A `layout_stack` of six panels, one visible; each a custom `LLView` plus stock scroll
lists.

**Legend, because this vocabulary is ours and nobody was born knowing it.** The tool invents words —
level, anchor versus witness, noun chain versus View chain, `[` and `]`, band versus team — and a
combatant opening it for the first time has none of them. A `?` button beside the breadcrumb opens a
single non-modal **Legend** panel (stock `text_editor`, one screen, no tabs) naming exactly those six
things with one line each and a picture of the rail; it opens by itself the first time the floater is
ever opened and never again unless asked. Every level cell, operator chip and band chip carries the
same sentence as its XUI tooltip, so the vocabulary is also learnable by hovering without reading
anything. This is a discoverability affordance, not a tutorial: nothing in the tool is gated behind
having read it.

**Evidence column** (collapsible, 180 px). Always answers "what is this made of": inputs, sample
counts, coverage, assumptions in force, each editable in place with live re-render. Right-clicking a
parameter is the entry to R2 [01].

**Ribbon** (the constant). Death ticks by victim team; stacked damage-rate band per team; bout bands
dithered at fuzzy edges; the **escalation lane** and the **presence lane** (below); the **structure
index** band [08] (one glyph per 30 s, `DEATHMATCH` printed
when `S − S_null < 0.08` for a minute); a track-quality row hatched below 60 % coverage. Drag scrubs,
Shift-drag sets the R1 window, double-click a bout ascends to it. **The ribbon never changes; the
stage always does** — it is the spine that makes vertical movement legible [01]. `SSRibbonView`,
rebuilt only on cursor/filter change; mind the `RenderUIBuffer` dirty-rect gotcha for animated custom
views (`doc/viewer/ui_system.md`).

**The escalation lane** [reports][03], two rows, one per side, because "when did the fight escalate"
is a question officers ask in the vocabulary they already use — *escalation*, *auth*, *tier*, "went
to tier 3". **It ships behind a preference and is off by default** [owner]: the class ladder is a
group's own, not a universal one, and a lane drawn from the wrong class ladder is worse than no lane. The
class set is configurable per group beside the rule catalogue, and the wireframes above draw the lane
as it looks once an officer has switched it on. Each row draws the **first use per side** of the
classes that constitute a step up:
explosives, staffs and other exotic damage types, vehicles, mechs, drones, mounted deployables. A
step is a labelled bracket at the first attributable hit of that class by that side, with the
equipment linked; hovering prints "first EXP: RX Grenade, Rook → Ilse, 14:41:22, class conf ●●○".
Because escalation is only as good as the equipment classification ([analysis §5.13](combat_log_analysis.md)) — which is thinnest
exactly for vehicles and mechs — a step whose class confidence is below `●●●` draws its bracket
hatched, and a step the officer disagrees with can be re-dated by hand as a Claim. Steps are also
where a **negotiated restriction** lands: an OIC who agreed by IM not to use something attaches the
restriction to the equipment family at that minute ([rules §5.22](combat_log_rules.md)), and the lane draws it as a downward tick
so a later bracket after a restriction reads as *rule-breaking*, not merely as escalation.

**The presence lane** [reports]. Headcount per side over time from the track store, with the
**militia band drawn separately** (clustered onto a side, no group tag — [analysis §5.9](combat_log_analysis.md)), and the OIC's `⚑ last
life` call marked when the officer sets it. Departures after that mark are the incident officers
already record by hand, and the lane is where they are visible. Both lanes are pure accumulators, so
they cost nothing at peek time (§2.9).

**Transport.** Step ±1 event / ±1 s, play, speed, Live, follow-cursor. Stock `button` + `combo_box`.

### 3.2 Stage — R0 Moment

```
│  MOMENT  14:31:07.4      ⟨world overlay is the primary view — see §4⟩         │
│  ┌ within ±1.0 s ─────────────────────────────────────────────────────────┐   │
│  │ 07.1  DMG  Cadmus ──▶ Vex   18.0 (init 24.0 ρ.75)  ballistic  ⛨sc.2 f │   │
│  │ 07.2  DMG  Cadmus ──▶ Vex   18.0                   ballistic  ⛨sc.2 f │   │
│  │ 07.4  DEATH Vex ← Cadmus  "Mk4 Carbine" 28.2 m  LOS ▓▓▒▒░░▒▒  MIXED f │   │
│  │ 07.6  DMG  Rook ──▶ Ilse     6.0  explosive ◇deployable?  area×3    ~ │   │
│  └────────────────────────────────────────────────────────────────────────┘   │
│  selected: DEATH Vex ← Cadmus   [article ▸] [sweep LOS ▸] [raw record ▾]      │
```

Rows are concrete events; the selected one pulses in the world. Each row carries a time-source glyph
(`f` sim-frame stamp, `fd` own-hit, `~` receive-time fallback), because ordering is only as good as
its stamp [01]. Verdict badges are **8 px miniatures of the sweep strip**, never scalars [01]. `▾` opens
R−1 inline as a two-column table. XUI: `scroll_list` with a custom cell painter for the miniature.

### 3.3 Stage — R1 Face A: the Bout Ledger

```
│ BOUT "West Gate"  14:29:12–14:36:40  6 deaths  1.4k dmg  ⟨Plan|Ledger⟩       │
│           14:29        :30         :31         :32         :33         :34   │
│ Cadmus B  ░░▓▓▓▓══╪═▓▓▓░░░░░░░░░▓▓▓▓▓▓╪▓▓░░░░░░░░░░░░░░░░░░░░░░▓▓▓▓╪░░░░░░  │
│ Rook   B  ░░░░░░░░░░░░▓▓╪▓▓▓░░░░░░░░░░░░░░✖▁▁▁▁▁▁▁▁▁▁▁▁▁░░░░░░░▓▓▓░░░░░░░░  │
│ +6 more B ▁▂▂▃▂▁▁▁▂▃▃▂▁▁▁▂▂▁▁▁▁▁▂▃▄▃▂▁▁▁▁▁▂▂▁▁▁▁▁▁▁▁▁▁▁▁▁▂▃▂▁▁▁▁▁▁▁▁▁▁▁▁  │
│ Vex    A  ▓▓▓▓╪▓▓░░░░░░░░░░░░✖▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▓▓▓╪▓▓▓░░░░░░░░░░✖▁▁▁▁▁▁  │
│ Ilse   A  ░░░░░░░░▓▓▓▓▓▓▓▓╪▓▓▓▓░░░░░⚠╪░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  │
│ Tam    ?  ····························▓▓▓╪▓░░░░░░░░········✖▁▁▁▁▁▁▁▁▁▁▁▁▁▁  │
│ Trret-3 ▣ ░░░░░░░░░░░░▓▓▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░░░░░◇destroyed? unknown──  │
│           ▓ volley  ╪ death-causing volley  ✖ death  ▁ dead  ⚠ friendly fire │
│           ░ alive, tracked   · no track   ▣ object row   +N = folded rows    │
```

One row per combatant, team letter and colour in the gutter, hatched when tentative. A collapsed
**Objects band** carries turrets, vehicles and deployables that were themselves damage targets, with
destruction drawn from their published LBA health where the viewer saw it and `unknown` where it did
not [05]. Degradation is
specified: rows sort by team block then by involvement; the top twelve draw individually, the rest
fold into `+N more` rows whose strip is the summed damage-rate density; anything with a death, a
friendly-fire flag or a pin is force-promoted out of the fold [01]. Click selects, `Shift+click`
descends, `Ctrl+click` pins.

### 3.4 Stage — R1 Face B: Plan, with the World|Chart parity rule

```
│ BOUT "West Gate" · PLAN ⟨World | Chart⟩   ▣ 128 m   N↑   ⊕ zoom  ✋ pan       │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │            ░░░░  ▄▄▄▄▄▄▄▄▄  wall (occlusion cloud)                     │  │
│  │      B ───────╮  ▀▀▀▀▀▀▀▀▀                    ░ death density          │  │
│  │      B ────────╯╲   ╱────── A       ✖ = death cross (measured)         │  │
│  │             ✖ ✖  ╳ ╱      ░░░░      ─ trail (alpha = age)              │  │
│  │      B ──────────╳╱  ✖  ░░▒▒▒░░     ╌╌ dashed = reconstructed          │  │
│  │            ╱     ╲╲      ░░░░░      · · dotted = coverage gap          │  │
│  │      · · · ·      ╲──── A           ◇ = sub-TRACK attacker position    │  │
│  │   (Tam, no track)                   ⌖ cursor 14:31:07.4                │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
│  region SS-Verdance · 96×112 m · coverage 87% ▨13% · drawn 200 of 512 (LOD)  │
```

The **Chart** renderer is a custom `LLView` drawing the overlay's own primitives orthographically
from above, north up, wheel zoom, drag pan, screen-rect picking identical to the world's [01]. It
exists because the overlay is gated to stationary + alt-cam — unavailable exactly when the officer is
walking, re-gearing or briefing — and "where did the fighting happen" must not become unanswerable
for a UI reason. **Parity rule: anything the overlay draws at level R has a Chart twin with identical
encodings and picking; the world adds only depth, occlusion and a free camera.** Parity is enforced
as a *shared draw-list* — one `SSCombatDrawList` of typed primitives emitted by the analysis layer,
consumed by two back ends — not as a convention, because a convention drifts under maintenance [01].
R4's Ground face uses the same renderer.

### 3.5 Stage — R2 Sweep

```
│ SWEEP: line of sight for DEATH Vex ← Cadmus 14:31:07.4                        │
│ claim ⟨LOS τ | anchor t±1s | attribution window | awareness θ | segment ε⟩   │
│ delivery: PROJECTILE (unattached, 0.4 s, ghost flight seen) ⇒ travel window  │
│           ⟨hitscan · projectile · lobbed/arc · area⟩  rule: no kill through  │
│           obstacles that would typically stop them [Ashguard/Weapons] ▸      │
│                                                                               │
│  τ=0.0   0.2   0.4   0.6   0.8   1.0   1.2   1.4 s                            │
│  head  ████████████▒▒▒▒▒▒▒░░░░░░░░░░░░░░░░░░░░████████    █ BLOCKED           │
│  chest ██████████████▒▒▒▒░░░░░░░░░░░░░░░░░░░░░████████    ░ CLEAR             │
│  feet  ████████████████▒▒░░░░░░░░░░░░░░░░░░███████████    ▒ MARGINAL/UNKNOWN  │
│                        ▲ chosen 0.75                                          │
│  distance ──▶ 26.1  26.8  27.4  28.2  29.0  30.1  31.4  32.9 m                │
│  track src  B  B  B  V  V  V  C  C   tier: TRACK                              │
│  anchor sweep: t−1s → flip at τ 0.35 · t+1s → flip at τ 0.95 · NOT ROBUST     │
│                                                                               │
│  flip fraction 0.42 · coverage 11/13 · [a clear shot exists]                  │
│  verdict: MIXED — clear only for 0.5 ≤ τ ≤ 1.1 s, and the flip point moves    │
│  under the log's own ±1 s lag. Not evidence of a wall kill. [pin] [to case]   │
```

The sweep replaces the scalar verdict everywhere one would have been shown. The **headline is flip
fraction plus coverage**; the existential result is demoted to a small `[a clear shot exists]` chip,
because that test is uninformative when true and decisive when false — "no clear line at any τ" is
the only real wall-kill claim [11]. The **anchor sweep** row re-runs the whole verdict at t−1 s and
t+1 s and labels it ROBUST or shows both outcomes [12]. Claim chips make segmentation constants
sweepable too, and a bout whose membership shifts under a 20 % nudge of ε gets a boundary-stability
chip [01].

### 3.6 Stage — R3 Field (five faces)

```
│ FIELD ⟨Matrix | Braid | Front | Roster | Board⟩    set: all avatars, 14:02–now│
│ MATRIX  damage dealt (row ▶ col), log-scaled, ⌗ = n events, ▨ = low support   │
│            Cad  Rook  Vex  Ilse  Tam  Kai      ⟨41 agents: blocks collapsed,  │
│    Cadmus   ·   ▁²    ██⁴¹ ▆¹⁷  ▃⁶   ▁¹         6 shown, click to expand⟩    │
│    Rook    ▁²    ·    ▅¹⁴  ██³⁸ ▂⁴   ·                                        │
│    Vex     ██³⁴ ▆¹⁹    ·    ▁¹   ▨¹  ▅¹²                                      │
│    Ilse    ▇²⁸  ██⁴⁴  ▁²    ·    ·   ▃⁷                                       │
│    Tam     ▃⁵   ▂³    ▨¹   ·     ·   ██²⁹      ← outside both blocks          │
│   blocks: {Cadmus,Rook,Ilse} vs {Vex}  |  {Tam,Kai} mutual — third party?     │
├───────────────────────────────────────────────────────────────────────────────┤
│ BRAID  allegiance over time (5-min windows; saturation = θ-stability)         │
│         14:05      14:20      14:35      14:50      15:05      15:20          │
│ Cadmus  BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB   .94 412 │
│ Rook    BBBBBBBBBBBBBBBBBBBB!BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB   .91 377 │
│ Tam     ····aaaaaaaaaAAAAAAAAAAAAAAAA▶bbbbbbbbBBBBBBBBBBBBBBBBBBBBB   .86 203 │
│ Marek   ///////AAAAAAAAAA///////AAAAAAA////////////////////////////   .38 141 │
│ Juno                        AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA   .77  46 │
│  ▶ DRIFT (changed side 14:41 ±1 window)  /// noise, unclassified              │
│  short = small sample (Juno, arrived 14:20)  ! = friendly fire  [L] = locked  │
│  band header: RED  solid|##  STRAINED 41% [test split]  tag-dependent (!)     │
```

The Matrix's block structure *is* the clustering, shown raw; an off-diagonal `!` inside a block is
friendly fire, and a row that does not match its block is a mis-assignment you can see before the
machine admits it [08]. At 41 agents it collapses to blocks and expands on click [01].

The Braid reports **three distinct outcomes, never one confidence number** [11]: **drift** (modal
cluster changes between adjacent windows while θ-stability stays high on both sides → chevron plus a
printed change time), **noise** (stability low inside the window → hatch, no chevron) and **short**
(small sample → its own badge). Band headers can carry the **STRAINED nn%** internal-hostility meter
and the **(!) tag-dependence flag** from the always-run tag-free second solve [08] — see [analysis §5.9](combat_log_analysis.md) — and
the wireframe above draws them switched on.

**Both are behind a preference and off by default** [owner]. The second solve still runs and
`[test split]` stays reachable from R2; what the setting controls is whether an accusation-shaped
inference sits in the default view. The preference carries a Soapstorm-style warning beside it:

> *This inference is inherently flawed and is not confident.* It is built from object updates the
> simulator chose to send **this** viewer, and the Interest List decides that — by your camera
> position, your draw distance and what it thinks you care about. Two officers thirty metres apart
> get different data, different timing and different gaps, so this meter is a property of where you
> stood as much as of how anyone fought. Treat it as a prompt to look, never as a finding.

The caveat is true of every track-derived quantity, but this is the surface where being wrong costs
someone their reputation, so it is printed rather than filed under general honesty.

```
│ FRONT  unbent space · x = signed distance to the contested surface (m)        │
│ level ⟨0–4 m | 4–8 m | 8 m+⟩   contour: 1 component, ε-stable   n = 612       │
│            B side ◀──────────────  0  ──────────────▶ A side                  │
│           −40   −30   −20   −10    │   +10   +20   +30   +40    front   conf  │
│ 14:26–28  ····░░▒▒▓▓█████████████ █│█████▓▓▒▒░░·······          −0.8    ●●●   │
│ 14:29–31  ···░░▒▒▓▓██████████████ █│███✖█▓▓▒░░········          −0.6    ●●●   │
│ 14:32–34  ··░░▒▓▓████████████████ █│█✖✖✖▒▒░···········          −0.2    ●●○   │
│ 14:35–37  ·░▒▒▓█████████████████ ██│✖██▒░░············          +0.4    ●●○   │
│ 15:03–05  ▒▓████████✖✖✖✖✖████████ █│░·················          −0.9    ●○○ ▨ │
│           █ damage density   ✖ death, plotted on whose ground it fell         │
│           ▨ contour weak in this window: bar hatched, front value greyed      │
│ ── if no contour survives, the face draws this instead, and nothing else: ──  │
│  "no single front 14:02–15:47 — 3 live clusters. Geography is in Chart and    │
│   Ground." [open Ground ▸] [why: cluster count 3, ε-sweep ▸]                  │
├───────────────────────────────────────────────────────────────────────────────┤
│ ROSTER  a filter, not a rank · no summed score · nothing here is a verdict    │
│ threshold ◀━━━━━━━━━━━━━━●━━━━━━━━▶ 4.0× median  ☑ keep n<8 out of ordering   │
│ 41 agents · 2 above threshold · 39 greyed (never hidden — you can see them)   │
│              wall-blk    aware-lead   reaction   speed   n    coverage        │
│  Kai          3 [1–7]     6 [3–10]      97th      —      31    ●●●    [⚑]     │
│  Marek        1 [0–4]▨    2 [0–5]▨      61st      1 ▨     9    ●●○    [⚑]     │
│ ── below threshold ────────────────────────────────────────────────────────   │
│  Cadmus       0           1 ▨           44th      —      88    ●●●            │
│  Rook         2 [0–6]     0             51st      —      74    ●●●            │
│  Vex          0           0             — no cue  —      52    ●●○            │
│  Ilse         —           —             —         —      —     ○○○ insuff.    │
│           ▨ thin (n < 8): shown, never ordered on   right-click → [sweep this]│
│           each column header links to its Signal page (confounds printed) ▸   │
├───────────────────────────────────────────────────────────────────────────────┤
│ BOARD  3 pins · ☑ lock scales · cell face ⟨Plan | Sweep | Life | Matrix row⟩  │
│ ┌ Mk4 · death 14:31 ─────────┐ ┌ RX Gren · death 14:44 ─────┐                 │
│ │ τ ████▒▒░░░░░░░░░░████     │ │ τ ███████████████████      │                 │
│ │ flip .42  BLOCKED < 0.5 s  │ │ flip .00  NO CLEAR, ANY τ  │                 │
│ └────────────────────────────┘ └────────────────────────────┘                 │
│ ┌ Mk4 · death 15:12 ─────────┐ ┌ + drop a pin here ─────────┐                 │
│ │ τ ░░░░░░░░░░░░░░░░░░░      │ │                            │                 │
│ │ flip 1.00 CLEAR at every τ │ │                            │                 │
│ └────────────────────────────┘ └────────────────────────────┘                 │
│  one paint routine per face (SSCellView), identical axes across all cells; a  │
│  cell that cannot honour the locked scale hatches and says so, never rescales │
```

**Front** plots events in unbent coordinates (signed distance to the contested surface, per 4 m
vertical level [08]) and refuses to draw when no contour survives, handing geography back to the
Chart [01][08] — the refusal, with its cluster count and a link to the ε-sweep, is the whole face in
that case. **Roster** is the behaviour face; **it ships behind a preference and is off by default**,
and with the preference off the face is absent from the face switcher rather than empty [owner]. It
is a **filter, not a rank** [12]: one column per
signal with its own raw count, no summed score, `thin` below n ≈ 8 excluded from ordering, interval
printed before the point estimate, disposition and note per flag [11]. Its only control is the
**threshold slider in ×-median units** (stock `slider_bar`, 1.0×–6.0×, the drag Q8 uses); rows under
threshold grey out but stay on screen, so the officer always sees how many names the filter is
hiding, and a combatant with too little track prints `insufficient` rather than a zero. **Board** draws
one cell per pin on identical, scale-locked axes; the lock is a `check_box`, and a cell whose data
will not fit the locked scale hatches instead of rescaling itself.

### 3.7 Stage — R4 Session

```
│ SESSION 14:02–15:47   3 phases   41 agents   88 deaths   19.4k dmg           │
│  ⟨Board | Ground | Summary⟩                                                  │
│  PHASES ▸ [ Staging 14:02–14:26 ] [ West Gate push 14:26–15:03 ]             │
│           [ Collapse & counter 15:03–15:47 ]                                 │
│  ESCALATION ▸ A: basic → EXP 14:41 → VEH 15:02   B: basic → EXP 15:07 →      │
│               VEH ▨ 15:09 (mech? ●●○)   restrictions: 1 (basket gren 15:14)  │
│  REGION ▸ damage on · foreign msgs BLOCKED · throttle 300 · limit 100 · 60 av│
│           restore 100 · regen 0.0/s · invuln 3.0 s · death: no action (3)    │
│  ┌ teams ─────────────┐ ┌ headline ──────────────────┐ ┌ flagged ──────────┐ │
│  │ A  17 av  38 kills │ │ deaths 88 · attributed     │ │ 3 deaths with no  │ │
│  │  +2 militia        │ │   72 of 88 ▇▇▇▇▇▇▇▇░░      │ │   attributable dmg│ │
│  │ B  19 av  44 kills │ │ wall-kill candidates 5     │ │ split unresolved  │ │
│  │ ?   5 av   6 kills │ │   of 41 FIRM verdicts      │ │   14:44–14:51     │ │
│  └────────────────────┘ │   (31 still resolving) ▨   │ │ 3 deaths with no  │ │
│                         │ rule candidates 11         │ │   teleport after  │ │
│                         │   (7 measurable 4 partial) │ │                   │ │
│                         └ (gutter = evidence support)┘ └───────────────────┘ │
│  LEDGERS ⟨Avatars | Teams | Equipment | Scripts | Objects | Rules⟩           │
│   equip family        users hits  dmg  kills  ρ   dist(kill)  wall%  class    │
│   [Ash] Kagrenac bolt   9   611  4.1k   22   .81  41 m [8–96]  4% ▨  wpn ●●●  │
│   [Ash] RX Grenade      6    88  1.2k    9   .93   6 m [2–11] 31% ██  dep ●●○  │
│   ** object / vehicle   —    41   610    3    —   12 m [3–31]  —     obj ●●○  │
│   UNATTRIBUTED          ?   142   900    4    —    —          12% ▨  —   ○○○  │
│   Ward-7 armour script 14     —     —    —   .62   —           —     scr ●●●  │
```

**The headline box obeys §2.8, and this is the stage where it matters most**, because a fighting OIC
reads it in five seconds and never drills down. So every count on it is printed as **candidates over
their denominator with the resolved fraction beside them** — "wall-kill candidates 5 of 41 FIRM
verdicts, 31 still resolving", never "wall-kill susp. 5", and never a percentage on its own. A count
whose denominator is still filling carries the `▨` hatch until the background cast queue drains, so a
peek at minute 20 cannot read as a settled number. There is no exemption here: a bare number on the
peek stage is exactly the number that ends up quoted in a report by someone who never opened the
sweep behind it.

The **UNATTRIBUTED** row is permanent and never merged into a named weapon [11]. The flagged panel is
the tool's own list of questions about itself [01], and death-without-teleport ([analysis §5.7](combat_log_analysis.md)) sits in it
because it is a fault officers already chase by hand [reports].

The **Escalation** row is the R4 face of the ribbon's escalation lane and appears only when that lane
is switched on; clicking a step lands on the first hit that made it. The **Rules ledger** is the
region's rule catalogue ([rules §5.22](combat_log_rules.md)): one row per rule, its measurability
class, the count of candidate rows, and whether it is a written rule or an in-fight restriction. It
sorts by candidate count and never by "severity", because the tool does not have one. **It is empty
until an officer types a rule**, and an empty ledger says so with a link to the editor rather than
pretending the region has no rules.

**The REGION row is the sim's own combat configuration, read rather than inferred** [owner]. Script 2
reports `llGetEnv` on handshake and on every region change, and nine values land here:
`allow_damage_adjust`, `restrict_combat_log`, `damage_throttle`, `damage_limit`, `restore_health`,
`health_regen_rate`, `invulnerability_time`, `death_action` and `agent_limit` — the same block the
owner's own region-activity board reads. Each one changes how a reading downstream should be
believed, and the row links to the analysis that uses it: `restrict_combat_log` says whether foreign,
script-written messages can exist at all; `damage_throttle` and `damage_limit` explain capped or
dropped damage that would otherwise read as a weapon anomaly; `invulnerability_time` is a legitimate
source of post-death immunity the adjustment analysis must exclude before calling anything suspect;
`death_action` says whether a death implies a teleport at all, and `3` (no action) is the signature
of a DOWNED-style system. `restore_health` and `health_regen_rate` do more than caption: with them
the tool estimates each combatant's **health over time** from the damage stream, a new derived
quantity with its own uncertainty ([analysis §5.25](combat_log_analysis.md)) feeding the Life noun
and the reconstruction (§3.10). Values the region did not report print `—`.

The equipment ledger keys on **family** — maker prefix plus name stem, version stripped —
and carries the **distance-at-kill distribution** per family, the statistic the group's Discord kill
feed has trained everyone to expect [reports]. The `**` row is object and vehicle kills with no
owning avatar, and self-kills sit as ordinary rows on their killer's own page: both are normal
entries in the feed officers read, so neither is drawn as an anomaly [reports].

### 3.8 Article floater (`ss_combat_inspector`)

```
┌ DEATH · Vex ← Cadmus · 14:31:07.4 ────────────────────────────── [pin] [case]┐
│ ◀ ▶ │ levels: [-1 raw][0 moment][1 in bout][2 LOS sweep][3 in matrix][4 —]   │
├──────────────────────────────────────────────────────────────────────────────┤
│ Vex was killed by Cadmus at 14:31:07.4, 28.2 m away, with Mk4 Carbine        │
│ (ballistic, projectile). The killing blow was 18.0, reduced from 24.0 by     │
│ Ward-7 armour. Line of sight is BLOCKED for travel < 0.5 s and CLEAR from    │
│ 0.5–1.1 s, and that flip point moves under the log's ±1 s lag [why].         │
│ ── Contributing damage (window 20 s) ──────────────────────────────────────  │
│  Cadmus / Mk4 Carbine   volley ×3   54.0  dominant   (2.5× runner-up)        │
│  Rook   / RX Grenade    volley ×1   22.0  contested                          │
│  ⚠ raw damage totals, not health — health values are not observable.         │
│ ── Evidence ──────────────────────────────────────────────────────────────   │
│  killer track: bridge 2 Hz, gap 0.0 s, σ ±0.15 m, tier TRACK   ●●●           │
│  victim track: viewer, gap 0.4 s, σ ±0.5 m                     ●●○           │
│  equipment: attached pt 6, source≠rezzer, lifetime 0.4 s ⇒ projectile ●●●    │
│  log lag 0.62 s (not fd-confirmed — fd covers only your own avatar)  ●○○     │
│  ordering: margin 0.31 s > combined uncertainty 0.12 s ⇒ blow unambiguous ●●● │
│ ── Links ── Cadmus · Vex · Mk4 Carbine · Ward-7 · Bout "West Gate" · Team B  │
│ ── What links here ── ⚑ Case "Vex report" · ✓ claim/19 "reviewed 9/8" ·      │
│                        Phase "West Gate push" · Cadmus's lives               │
└──────────────────────────────────────────────────────────────────────────────┘
```

Prose first, table second, evidence third, links last — the wikipedia shape [02]. Body is a read-only
`LLTextEditor` with `parse_urls` and the registered `LLUrlEntry`; sections are `LLAccordionCtrl`
panels built only when opened; the infobox is key/value `text` widgets, italic plus a band chip for
inferred values. Every article carries an always-populated **Compared with** block (peer context is
not optional) [11] and a `[sweep this]` on every badge [08].

A transient **Peek card** appears on hover over any link or in-world marker: title, one-line lede,
band chip, three top links; `Tab` promotes it to a full article [02]. (It is the hover card; **Peek
mode** in §2.9 is the shallow entry depth. Two different things, and the UI never uses the bare word
alone.)

### 3.9 R4 Summary face — the artefact the officer was going to write anyway, in their own format

```
│ SESSION SUMMARY · Resdayn · 2026-09-08 · ⟨Attack|Defense|Raid|Skirmish|…⟩    │
│ template: (none — this group has not defined one)  [edit template ▾] [copy ▾]│
│ ── sections ──────────────────── ☑ include · drag to reorder · rename inline │
│ ☑ Duration        1h45m (14:02–15:47, last life called 15:31 ⚑)              │
│ ☑ Present · A     17 combatants, group tag [Ash] ────────────── [edit list]  │
│     Cadmus, Rook, Ilse, …                                                    │
│ ☑ Present · A, untagged (2) ─ clustered onto A, FIRM only ─ [add ▾] [drop ▾] │
│     "Kite" FIRM · Sera Blackwood FIRM     ⚠ inference — check before sending │
│ ☐ Present · B (19)   ☑ Civilians present (5)   ☐ Side unresolved (5)         │
│ ☑ Escalation      A: EXP 14:41 · VEH 15:02   B: EXP 15:07 · VEH ▨ 15:09      │
│                   restrictions: 1 (basket gren 15:14)   ⟨lane is switched on⟩│
│ ☑ Deaths & kills  88 deaths · 72 attributed ▇▇▇▇▇▇▇▇░░ · A 38 · B 44 · ? 6   │
│ ☑ Prose           [                                                     ]    │
│     facts offered: · 3 phases, push east 14:26 then collapse 15:03           │
│                    · escalation: our EXP 14:41, theirs MECH ▨ 15:09          │
│                    · outnumbered hold: Ilse+Rook, 2 v 6, West Gate, 17 min ▸ │
│                    · objective capture: needs owner-designed zones (future)  │
│                                                                     [insert] │
│ ☑ Incidents (3) ─────────────────────────────────── [new] [from flagged ▾]   │
│     #1 3 deaths with no spawn teleport 14:44–14:51     → evidence ▸ (4 cards)│
│     #2 RX Grenade: 2 kills with no clear line at any τ → sweep ▸             │
│     #3 basket grenades used 15:19, restricted 15:14    → rule/ash-negot-3 ▸  │
│ ☑ Per-combatant activity ─ line format from the template ── [edit format ▾]  │
│     Cadmus   present 1h43m · active 64m · 14 kills · 6 deaths                │
│     Rook     present 1h45m · active 71m · 9 kills · 11 deaths                │
│ ☑ Pinned evidence (6) ─ from this session's Cases ───────────────  [manage]  │
│ ⌗ preview ▾ — plain text, exactly what the clipboard gets                    │
```

The Summary face renders the `ss:summary/session` article inside the R4 stage. Every section is
built from facts the tool already holds — nothing here is a new measurement — and **the section
list, the labels, the ordering and the per-participant line format are group data, defined
alongside the rule catalogue and the group registry. The tool ships no template** [owner]. With
none defined the face shows every section it can fill, unchecked into a default order, and the
officer checks and drags until it looks like what their group posts; that arrangement is what
`[edit template ▾]` saves for next time.

Everything auto-fills and everything is editable. Duration runs first to last event, with the OIC's
`⚑ last life` mark if one was set. **Present** is one section per side plus civilians, from the
combatant test of §2.1, split into the combatants carrying the side's group tag and those the
clustering put on that side **without** it — the militia inference
([analysis §5.9](combat_log_analysis.md)) — printed with its ordinal band and a standing warning, because calling someone militia from a tag they forgot to wear
is an insult with a confidence score attached [03]. **The tool proposes a name only when that
inference is FIRM** [owner]; below FIRM the list arrives empty and the officer fills it, with "these
fought alongside A without its tag" available as a hint on request and never as a row the tool
wrote. **Escalation** appears only where that lane is switched on (§3.1). **Deaths & kills** carries
its attributed fraction, never a bare count (§2.8). **Prose** is the officer's; the tool offers
*facts*, one sentence plus a link each, taken from phase shape, escalation steps and the notable
episodes of [analysis §5.24](combat_log_analysis.md) — an outnumbered hold, a long survival, an
objective capture where zones exist — inserted only when the officer clicks. None of them carries a
recommendation and none of them is a reward: they are the beats an officer would have typed from
memory. **Incidents** are Cases promoted into the summary with their evidence links, seeded by the
flagged panel and by rule candidates. **Per-combatant activity** renders one line per participant in
whatever format the template names, from the same statistics as the combatant page
([analysis §5.24](combat_log_analysis.md)).

`[copy ▾]` renders the assembled sections to the clipboard as plain text, and the preview shows
exactly what will land there. There is no posting from the viewer, and no format is privileged.

**Worked example — the Ashguard forum raid post** [reports]. One group's template, reproduced
because it is the format the ~130 scanned reports were written in, and because it is the evidence
that this mechanism is sufficient for a real one. **It is an example and nothing else**: not a
default, not a shipped template, not a shape the tool prefers. Every SLMC group runs its own
processes and this is Ashguard's [owner].

```
┌ template · Ashguard forum raid post ────────────────────────────────────────┐
│ sections   Duration · Personnel · (Militia) · Report · Incidents ·          │
│            OIC HUD Output                                                   │
│ labels     "Personnel" = present combatants carrying the group tag          │
│            "(Militia)" = those clustered onto the side without it           │
│            "Report"    = the prose block                                    │
│ per-line   {uuid},{active_minutes}      under OIC HUD Output                │
│ render     forum post carrying the group's own thread tags                  │
├─────────────────────────────────────────────────────────────────────────────┤
│ ── OIC HUD Output ── activity minutes reproduced from posted reports,       │
│                      unverified ──────────────────────────────────────────  │
│  a1b2…c3,64   d4e5…f6,71   …           alt column: engaged minutes 58 / 69  │
└─────────────────────────────────────────────────────────────────────────────┘
```

That caveat belongs to this example and travels with it. This group's activity protocol — minutes
in which the avatar's rotation changed, bucketed in 5-minute windows — is not documented anywhere we
can read, so a template that asks for it gets a block headed *reproduced from posted reports,
unverified*, with the tool's own measure (minutes with any damage or > 2 m of motion) beside it as a
second column and never as a silent substitution. A difference in rounding, in who is included, or
in whether untagged combatants appear would otherwise make the block paste wrong without anyone
noticing
[reports][owner]. A template that does not ask for minutes never shows any of this.

Widgets: stock `text_editor`, `scroll_list`, `combo_box`, `check_box` and `button`. No custom drawing.

### 3.10 Reconstruction — the ghosted replay of one death

The owner's verdict on the R−1 raw arena was "reconstruction is nifty", and the idea it names is
bigger than a raw-record level [owner]. **Clicking a death marker in the world opens a reconstruction of
that death**: the officer stands where it happened and watches it happen again, translucent, without
opening the big floater at all. It is the chalk outline made temporal, and it is the one feature here
a spectator understands in three seconds.

**In these terms it is a mode, not a level**: R0 given a window instead of an instant, driven by its
own transport rather than the session cursor. Entering it sets the subject to that Death and the
window to `[t − 6 s, t + 2 s]` (both ParamSet entries) and prints `⌄reconstruct [0]` as a breadcrumb
operator, so leaving is `[<]` or `Esc`. The level rail does not change, because the level did not.

**What the world draws**, all ghosted at 35 % alpha with no depth writes so the region stays readable
underneath, and all obeying §4.5's measured-versus-reconstructed rule:

- **Ghost combatants** — everyone with a damage edge into or out of the death, plus anyone inside the
  bout hull at `t` — as translucent capsules at their `sampleAt` positions with yaw arrow and label.
  Victim and killer at 60 %, everyone else at 25 %; a track gap draws hollow and stops rather than
  sliding.
- **Bullet flights** from the ghost-projectile store, animated along their measured polylines at
  measured speed, solid because they were seen; a flight with no DAMAGE keeps its terminal tick on
  the geometry it hit.
- **Hit points**: a disc at each DAMAGE on the victim's body, sized by damage, coloured by type,
  fading after 1 s to a faint mark.
- **The killer's line** at full alpha, carrying the LOS verdict's colour if a sweep already ran —
  never computed *by* the reconstruction, which is a viewer and not an analysis.
- The victim's health estimate ([analysis §5.25](combat_log_analysis.md)) as a hatched bar over the
  ghost, hatched because it is estimated.

**The control panel** is an XUI panel, not a floater: `panel_ss_reconstruct`, docked bottom-centre,
about 420 × 64 px, dismissible with `Esc`, stock widgets only — play/pause and step `button`s, a
`slider_bar` scrub, a speed `combo_box` (0.1× / 0.25× / 1×), a `text` readout relative to the death
(`−2.40 s`) and a *loop* `check_box`. The slider spans the death window and nothing else, so a scrub
cannot wander into the rest of the session; it is bottom-centre because that is where the officer's
camera is not, and small enough to leave the world visible.

**Keys and picking.** `Space` toggles play and `,` `.` step one event; the level keys keep their
meanings, `[` descending to R−1 on the event under the cursor and `]` ascending to the containing
bout and closing the panel. Picking is unchanged (§4.6): ghosts push their rects like any marker, so
clicking one opens that Avatar page at that instant and the pick-stack resolves overlapping ghosts
exactly as it resolves live ones.

**Budget.** At most 24 ghosts plus the window's flights and discs — the order of a busy R1 Plan and
cheaper, because eight seconds is not a bout. **It runs zero raycasts**: every geometric claim was
computed before it opened, and an un-swept killer's line draws neutral grey captioned `not tested`.
Opening the panel resamples tracks at 10 Hz into a small ring of prepared frames, so scrubbing costs
an array index. The gate is the overlay's own (§4), which is the posture the feature assumes anyway.

### 3.11 Custom drawing inventory

Everything else is stock XUI. The custom `LLView` bill, stated honestly:

| Class | Draws |
|---|---|
| `SSLevelRail` | six level cells plus the landing line |
| `SSRibbonView` | deaths / damage / bouts / structure / quality bands, scrub |
| `SSStageMoment` | event strip with verdict miniatures (mostly a `scroll_list` painter) |
| `SSStageLedger` | avatar rows, volley blocks, fold rows |
| `SSChartView` | the 2D top-down twin, consuming `SSCombatDrawList` |
| `SSStageSweep` | sweep strips, aim-point rows, anchor row |
| `SSStageField` | matrix / braid / front / roster / board painters |
| `SSCellView` | one small-multiple cell, seven paint routines [11] |
| `SSPickStack` | the world's pick-disambiguation list, one small popup panel (§4.6) |
| `SSReconstruct` | ghost capsules, animated flights and hit discs for §3.10 (draw-list primitives) |

The reconstruction's control panel is stock XUI and adds no drawing.
Roughly 2.5–3k lines of primitive drawing [11]. The world overlay is one more consumer of
`SSCombatDrawList`, not a separate renderer.

**One XUI check before any of this is drawn.** The wireframes above use Unicode glyphs as *literal
label text* inside stock widgets — `⚑ ✓ ⇄ ✎` on claim polarity, `▨ ◇ ▣` on hatch, sub-track and object
rows, `●●○` on confidence — and a glyph the viewer's default font does not carry renders as a tofu
box in a scroll list, where it cannot be worked around at draw time. So: confirm coverage in
`LLFontGL`'s default face for that exact set during Phase 3, and for anything missing use a small icon
texture (in custom-drawn views) or a two-character ASCII fallback such as `[F]`, `[OK]`, `[~]`
(in stock widgets). Inside `SSRibbonView`, `SSChartView` and the other custom painters the same marks
are geometry, not text, so they are never at risk; the exposure is exactly the stock-widget labels.

---

## 4. In-world overlay

Gated exactly as the plan fixes it: floater open, `SSCombatLogOverlay` on, stationary, alt-cam,
5 s after leaving mouselook/OTS. The overlay is **level-indexed**: pressing `]` changes what the
*world* draws, so the officer watches abstraction happen in the place they already understand rather
than in a chart beside it [01].

### 4.1 Layers

1. **Permanent base layer** — the whole-session figure at ~25 % alpha: death crosses, deadliness
   cells, auto-detected spawn hotspot rings, and the outline of any spawn zone the officer marked
   [02]. An officer who alt-cams somewhere out of curiosity always has something drawn to click —
   a cross, a ring, a trail — and exploration never requires having navigated first. What they cannot
   click is bare ground, because bare ground is prims and mesh and knows nothing about the fight.
2. **Focus layer** — the current level's figure at full strength.
3. **Pinned layers** — pinned pages' figures, subject to the scale rule.

### 4.2 What each level draws

- **R0 Moment.** Avatars as feet-rings with a yaw arrow and name label; eye glyph for
  `AGENT_MOUSELOOK`; crouch/air ticks; seated avatars ride their vehicle marker. Damage lines
  attacker→target at `t`, coloured by damage type, brightness by damage, pulsing 1 s. Deaths as
  cross + ring at `target_pos` with a killer line from `source_pos`. **Area events draw their rule
  radii**: two ground rings at the region's configured kill and wound distances (5 m / 10 m on
  Ashguard) centred on the blast point, so an officer can *see* whether the third victim was inside
  the written limit rather than reading a number about it [rules]. **Observed bullet flights** from
  the ghost-projectile tracker ([analysis §5.5](combat_log_analysis.md)) draw as measured polylines from spawn point to stop point,
  solid because they were seen; a bullet that stopped in a wall keeps its terminal tick there, which
  is what "shot into an occluder" looks like in the world [owner].
- **R1 Plan.** Trails for the whole bout, per-vertex alpha old→new; volleys as fans between shooter
  and victim trails; persistent labelled death crosses; the bout hull as a faint ground quad.
- **R2 Sweep.** The **LOS fan**: each candidate shooter position over τ as a marker on the killer's
  trail, each with its ray — green CLEAR, red BLOCKED, amber MARGINAL, grey dashed UNKNOWN — and the
  first blocking hit ticked on the wall with a 1 m normal disc (the *wall witness*) [11]. Scrubbing
  the strip walks a bright cursor along the fan. For a **lobbed/arc** delivery the fan is a family of
  ballistic arcs rather than straight rays, and the straight chord draws greyed with the caption "not
  the tested path" — a mortar shot over a building is a legitimate through-wall kill and must not be
  drawn as an illegitimate one [owner][rules]. For **hitscan** the fan collapses to two rays from the
  same instant: one from the avatar's eye and one from a plausible camera position, because
  open-source raycast kits cast from `llGetCameraPos()`, so a third-person or alt-cammed shooter
  sees around corners the avatar cannot. When the camera ray is CLEAR and the eye ray is BLOCKED and
  `AGENT_MOUSELOOK` was false, the overlay labels it `camera-origin` — that pair *is* the alt-cam
  aiming pattern, and it is the only place the tool says so [owner].
- **R3 Field.** Team centroid tracks as two thick lines; the front axis as a ruled ground line with
  iso-marks; a 4 m ground grid tinted by the dominant faction per vertical level, hatched where the
  margin is under threshold — that hatch *is* the contested line the Front chart measures against
  [08]; death density as ground quads.
- **R4 Session.** The ground plan: death heat quads, staging rings, push arrows between phase
  centroids, phase labels floating at them.

### 4.3 LOD, because each level owns a distance band

The officer alt-cams freely, so three layers must never fight for the same pixels [08]:

| Layer | ≤ 40 m | 40–120 m | > 120 m |
|---|---|---|---|
| halo / feet ring | wedge ring, radius `max(0.6 m, 8 px)` | 5 px dominant-colour disc, uncertainty as dashed/two-tone edge | not drawn |
| label | name + flags | name only, pinned/flagged/dead/top-damage, ≤ 12 on screen | not drawn |
| damage lines | all in window | top 40 by damage, 3 width buckets | one aggregate arc per ordered team pair |
| trails | 1 vertex / 2 screen px | 1 vertex / 4 px | not drawn |
| LOS fan | all candidates | 3 rays (first, first-blocked, last) | not drawn |
| ground tint / heat | on | on | on — the helicopter view |

A one-line HUD caption names the regime ("level 2 — territory; zoom in for bodies"), and a corner chip
reads `drawing 62 of 214 markers (LOD)` with click-to-widen [08][11]. **A view that silently drops
glyphs is a lying view.**

The 40 m and 120 m thresholds, and the ~200-cast frame budget of
[analysis §5.21](combat_log_analysis.md), are **hidden debug settings, not user preferences** [owner]:
guesses until a real 40-agent raid measures them, and the person who should turn that dial is whoever
is diagnosing a frame-rate problem. They ship as `SSCombatLogLodNear`, `SSCombatLogLodFar` and
`SSCombatLogCastBudget`, absent from the preferences panel, with the corner chip still visible so a
changed value is never invisible in its effects.

Cross-cutting **scale classes** govern pinned layers [02]: micro (< 20 m), local (20–120 m), region
(> 120 m), selected each frame from the camera's ground footprint. Pinned layers one class away
decimate to 40 % alpha and top-N; two classes away they collapse to a labelled centroid ("Mk4
Carbine · 512 shots ▸"), so a pinned region-wide shot cloud can never bury the six-ray fan the
officer is leaning in to read.

### 4.4 Encodings

Colour is team (seeded from the officer's existing contact-set / group-colour / netmap chain) or
damage type on event lines. Direction is geometry — arrowheads — never colour. Width buckets
`{1,3,6}` by evidence quality (bridge > viewer > coarse). Alpha carries age within the trail window.
Glyphs carry state: eye = mouselook, chevron = crouch, ▲ = flying, ▭ = seated, ◇ = sub-TRACK
attacker position, ▣ = object, `?` = unknown equipment.

### 4.5 Uncertainty in the world

- σ is the foot-ring radius, so a coarse-located avatar visibly sits inside a 4 m disc [01].
- **Measured versus reconstructed are drawn differently, everywhere** [07]: DEATH positions solid at
  full alpha; DAMAGE lines rebuilt from `sampleAt` dashed at ~60 % with a "reconstructed" tag; heat
  cells weighted by COARSE/stale samples blurred and desaturated [10].
- **Sub-TRACK attacker positions draw as a hollow diamond**, never a solid ring [05].
- Any event whose attacker has no usable position goes into a visible **unattributed** tally rather
  than being smeared onto a nearby cell [10].
- Track gaps dashed and thin, extrapolated positions hollow, gaps > 5 s absent with a `?` tick.
- **A BLOCKED ray draws red only when at least one dense (viewer- or bridge-quality) sample was
  tested; otherwise grey, whatever the raycast returned** [06]. And a BLOCKED ray is only ever drawn
  beside its sibling rays from the sweep — a lone red ray is a lie [01].
- **Occlusion as the through-wall cue.** With x-ray off, lines draw twice: depth-tested at full
  alpha and depth-off at 30 %. A shot ghosted through its middle went through geometry, and you can
  see the wall doing it.
- **Occlusion point cloud** [06]: every BLOCKED raycast's hit point, already returned by
  `lineSegmentIntersectWorldGeometry`, accretes into a session index drawn as a sparse scatter in the
  Plan and on the ground. Chokepoints become recognisable for free from casts already paid for, and
  untested ground stays honestly sparse. **Bounded like every other store**: 1 m grid bins with one
  representative and a hit count, capped at **32k cells** (≈ 0.6 MB), evicted LRU and swept by the
  same retention pass as events, with `occlusion 12k/32k` in the legend so a capped cloud never reads
  as a complete one.
- A ground-projected shadow dot sits under each 3D field point so density stays readable from an
  oblique alt-cam angle [07].

### 4.6 The pick model

Markers push `(LLRect, NounRef)` during render, via `LLViewerCamera::projectPosAgentToScreen`. Then
[11]:

- Selection resolves on **mouse-up**, and only if pointer travel since mouse-down stayed under ~4 px
  and the press was under ~350 ms. Any drag cancels the pick.
- ALT / CTRL / SHIFT-modified presses are **never consumed**, so alt-cam orbit and the pick tool keep
  their gestures.
- Pick radius 12 px, nearest first; when more than one candidate falls inside it — the staging-area
  heap — a **pick-stack list** opens at the cursor ("4 here: Kira RED · Mara RED · Tam RED · Quill ?")
  and nothing is selected until the officer chooses [08]. Filing an officer claim from the world
  *must* route through that list, so an assertion can never land on the wrong overlapping combatant.

Left-click selects and navigates the Article; `Shift+click` zooms in one level onto that noun;
`Ctrl+click` pins; hover shows Peek. **Left-clicking a death marker opens the reconstruction**
(§3.10) rather than the Death article, because in the world the replay *is* the article and the text
is one `[article ▸]` away on the panel. Clicking bare ground clears the selection and does nothing
else [owner].

**The overlay is a first-class navigator, not an exception.** Every overlay-initiated level or subject
change pushes the **View chain** exactly as a floater-initiated one does (§2.5): select, `Shift+click`
descend, `Ctrl+click` pin and entering a reconstruction all push the previous View before they act,
and the breadcrumb gains the same operator chip it would have gained from a click in the Levels
floater. Because the primary workflow is alt-camming rather than clicking widgets, an exemption here
is precisely where a dig would lose its thread. The one deliberate exception is **Peek on hover**: it
pushes nothing, because it is transient — `Tab`-promoting a Peek into a full article is the act that
pushes.

---

*§5, the analysis model, lives in **[combat_log_analysis.md](combat_log_analysis.md)**; its §5.22 and
Appendix A live in **[combat_log_rules.md](combat_log_rules.md)**. The numbering is kept so every
reference in the three documents still resolves.*

---

## 6. Walkthroughs

### The nine officer questions

**Q1 — How did this raid go, overall? Where, when, between whom?** The floater opens at R4 in Peek
depth, which for a fighting OIC is the whole answer in five seconds: 1h45m so far, A 17 + 2 militia
against B 19, escalation basic → our EXP at 14:41 → their vehicle-class step at 15:09, drawn hatched
because the mech sub-class is only `●●○` ([analysis §5.23](combat_log_analysis.md)); 88 deaths of which 72 attributed; five wall-kill
candidates out of 41 FIRM verdicts resolved so far; two questions flagged. For the officer who has
parked, the same stage is the top level: three phases and two damage crescendos in the
ribbon, a teams box reading A 17 / 38 kills against B 19 / 44 plus 5 unresolved, and a **Ground** face
— no alt-cam needed, because the Chart renderer works while walking — showing death heat in three
lobes and phase-centroid arrows marching east then snapping back west. Then dig: `[` on "West Gate push" → R3
Front, where B's front coordinate marches −0.8 → +0.4 over eleven minutes and snaps back in ninety
seconds at 15:03. Click the snap, `[` → R1 Plan: B's trails funnel into one alley with six crosses in
it. `[` again → R0; park, alt-cam the alley mouth, see the geometry that made it a trap. `]]]`
returns up the same path, each landing named in the rail before the key is pressed.

**Q2 — Who were the teams, how sure are we, where was friendly fire?** R3 Braid. Cadmus and Rook
solid B all session; Tam carries a drift chevron with "changed side at 14:41 ± 1 window"; Marek is
hatched (noise, unclassified); Juno carries `short`. With the meter switched on (§3.6) the RED header
also reads `STRAINED 41%` and a `(!)` tag-dependence flag. Friendly-fire `!` marks sit on the Braid and as off-diagonal cells inside the
Matrix blocks; each is bucket-local and says when the neighbouring bucket disagrees. `[sweep this]`
on θ shows the split holding from 0.15 to 0.7 — stable.

Then down, because a Braid nobody has grounded is a picture. `Shift+click` the `!` on Rook's strand at
14:38 → R1 Ledger, his row with one `⚠` block in it; click the block → **R0: DMG Rook ──▶ Ilse 22.0,
explosive, area×3, 14:38:51**, both of them B in that window and in the window either side, so the
flag is not bucket-local noise. Alt-cam the spot: Ilse's trail comes round the corner into the blast
ring 0.6 s after the grenade's rez, inside the 5 m kill radius drawn on the ground (§4.2). One
concrete friendly-fire death, one obvious cause, and the officer has seen the geometry rather than a
letter in a strip. `]]` returns to the Braid with the strand still selected.

**Q3 — Why did X die at 14:32?** Scrub to the tick, click the death marker in the world: the
reconstruction opens (§3.10), and eight ghosted seconds later the officer has watched two bullets
arrive from the pillar line and a grenade land late. That is usually enough; when it is not,
`[article ▸]` on the panel. The Article: Cadmus / Mk4
dominant at 54 raw damage, Rook / grenade contested at 22, reduced by Ward-7, killing blow
unambiguous (margin 0.31 s against 0.12 s uncertainty). The lede has already told a skimming officer
the fragility: "BLOCKED for travel < 0.5 s, CLEAR from 0.5–1.1 s, and that flip point moves under the
log's ±1 s lag." `[2] LOS sweep` shows the strip, the aim-point rows, the anchor row (NOT ROBUST) and
flip fraction 0.42. Click τ = 0.75 → R0 with that one ray drawn: the killer was past the pillar.
Into the Case: "not a wall kill; verdict unstable across travel time and anchor."

**Q4 — Which equipment kills through walls, how often, how confident?** R4 Equipment ledger, sort by
wall %. RX Grenade reads `31% ██` — click → the Model article: through-wall rate over FIRM verdicts
only, `4/11 = 36% [15–64%] · n=11`, with 9 MARGINAL and 27 UNKNOWN counted beside it and "resolved
128/512, computing…". `]` → R2 sweeps τ across every one of its kills as small multiples: four flip
to CLEAR somewhere, two never do, and **"no clear line at any τ, twice"** is the claim that survives.
Both survivors are TRACK tier, not APPROX-VIA-OWNER. Then the taxonomy does its work
([analysis §5.5](combat_log_analysis.md)): delivery reads AREA for both, the occluder count is 1 at
0.11 m, and the victims sat 1.4 m and 2.2 m from the far face — *thin-wall splash*, legitimate by the
community's own line, so the survivors collapse and the honest answer is "no". Had they read 2
occluders at 0.34 m and 0.9 m, the same rows would be a candidate under the region's "explosives must
not kill through walls" rule [rules], filed with the sweep attached and the officer's name on it.

**Q5 — How do damage adjustments change incoming damage per team/combatant?** R4 Scripts ledger, or the
Team article's Adjustments section: "RED absorbs 0.71× incoming, BLUE 0.98×", each a distribution
with n. Click Ward-7 → the Script article: which objects carry it, whose creator it is (the region's
experience attachment or a combatant's own HUD), the ρ distribution, and the **pattern** rows —
immune-at-spawn, immune-on-battlefield-while-dealing-damage, immune-bystander. `]` → R2 sweeps ρ
across the inclusion set, so an officer can see the effect of each script separately.

**Q6 — What did combatant Y do all session?** The Avatar article. Life strip with Contact by default,
Exposure as a costed on-demand overlay; movement and speed percentile; mouselook share from the
bridge flags; time seated on vehicles with the vehicle linked; **loadout over time**, because the
Layer system makes "their weapon" a timeline; kills and deaths as backlinks; the allegiance strand
with its drift/noise/short marks; every behaviour signal as a raw count with its own n. `]` puts them
in the Matrix; `[` lands on their anchor life.

**Q7 — Which equipment did each team use, which items are suspect?** The Team article's Armory
section: a roster × equipment-kind matrix with adoption share, mean ρ with adjusting script names,
and per-item through-wall tally with its interval, each family titled the way the kill feed titles it
and carrying its distance-at-kill strip [reports]. Sorting by suspicion sorts by *interval excluding
baseline*, not by point estimate [11]. UNATTRIBUTED sits at the bottom with its own interval, never
hidden. Two rows below the matrix answer the questions officers ask more often than "is it suspect":
the **escalation strip** (what each side brought and when), and, on the officer's *own* team page,
the **loadout compliance list** — families used by our members carrying a maker prefix other than
ours, with n and the caveats printed, unsorted by anything but user count and flagged as nothing at
all. That is a check Ashguard's own handbook asks its command staff to run by hand [merits]; a group
whose rules ask for nothing of the kind simply never opens the list.

**Q8 — Did anyone behave suspiciously?** Only for an officer who switched the behaviour preference on
([analysis §5.19](combat_log_analysis.md)); otherwise there is no Roster face and this question has
no screen. With it on, R3 **Roster** is a filter and not a leaderboard: through-wall count,
awareness-lead count, reaction percentile and speed flag, each with its own n. Drag the threshold
from 2× to 4× median and rows below it grey out, still on screen, so the officer sees the 39 names
the filter is setting aside. Two survive. Open the first: awareness lead 1.8 s, `●●●` coverage, n = 6
engagements, percentile 97, confound list printed. `[sweep this]` → R2 small multiples over θ_a:
three collapse past 15° (noise), one holds to 30°. `[` → R1 Plan: the yaw arrow tracks the victim
behind a wall for two seconds before the first shot. Then the mandatory context strip
([analysis §5.5](combat_log_analysis.md)) refuses to let the card be pinned bare and earns its place:
4 s earlier a `[UGL Blast]` landed on the subject, which is the documented blindfire-after-flash
pattern, and the second-strongest episode dissolves on the spot [reports]. The Case stores the sweep,
the coverage, the strip and the tool's own sentence: *consistent with wall tracking and also with
blindfire after a flash; wireframe use cannot be observed.*

### The officer jobs (Q9)

**Exporting a session summary in your group's format.** Something like this happens after every
single raid, whatever the group calls it, so it is first. The officer opens the R4 Summary face,
which has been filling since recording started, and it is already arranged the way they left it last
time — their group's template, saved beside the rule catalogue. Duration and the tagged roster are
already right; the untagged-but-fought-with-us list proposes one name, the only one the militia
inference called FIRM, and the officer adds a second by hand from the hint list and removes a third
who turned up uninvited. The prose block offers four facts; the officer takes two — the escalation
step at 15:09 and the outnumbered hold at West Gate — and writes three sentences around them, which
is the length these write-ups actually are. Incidents already contains the flagged panel's
death-without-teleport anomaly and the restriction breach at 15:19; the officer opens each, reads the
evidence links, drops one and keeps two. The per-combatant section renders in the line format the
template names. `[copy ▾]`, paste wherever this group posts, done. The tool has authored no judgement
anywhere in that flow and imposed no shape on it; it filled sections the officer would otherwise have
retyped from memory, and attached evidence to the two sentences that will be argued about [03].

**Settling an equipment dispute.** The commonest Incident category in practice is not misconduct, it
is "was this equipment doing what it should" [reports]. An enemy drone is reported as not taking
anti-armor damage. Open the Object page: 31 hits of type 104 landed on it over six minutes, from four
attackers — and its published LBA health, sampled from hover text whenever it was in view, reads
400 → 400 across all of them ([analysis §5.7](combat_log_analysis.md)). That is a finding rather than
a shrug: the complaint is now "we hit it 31 times and its own HP display never moved", which is what
the other OIC can be asked about. Two neighbouring shapes work the same way:
a mech that regenerated after death shows it in the same series, as HP snapping back to maximum; a
raycast weapon with no visible effects gets an equipment page saying "misses unobservable — hits 44,
shots not observable", so the dispute is framed as unmeasurable rather than lost.

**Reviewing how a combatant fought.** The job is teamwork, tactics and strategy, not a verdict on a
person [owner]. Open their Avatar article, pin it, open a second Article floater on a peer; `lock
scales` forces identical axes so the comparison is honest [02]. Teamplay reads: focus-fire share
against the team median, assists, mean distance to the line, friendly fire given with its test counts
and the bucket it was flagged in, and the allegiance strand. Backlinks show `⚑ 1 flag, ✓ 2 cleared` —
the officer sees two are exonerations without opening them [02]. The **summary statistics** section
([analysis §5.24](combat_log_analysis.md)) is a set of plain counts with no scheme attached to them:
*events attended* reads **this session only**, present 1h45m, 62 active minutes, captioned flatly
that session history is not computed here; *kills* reads 14, K/D 2.3, with a streak strip
`3 kills · gaps 4.1 s, 5.8 s` and no window asserted anywhere, because the tool ships no multikill
window and the officer reads the gaps against whatever number their group uses; *time in contested
areas* is absent, needing owner-designed zones. What any of it is worth is the group's business and
lives in the group's own configuration, not on this page. The officer cannot carry a count anywhere
without opening one concrete episode behind it, which is the step in to the concrete turned into a rule
[03]. Name the trail `nyx-west-gate` and dump it as a linked outline.

**Investigating a reported violation.** Start from an event id, a name or a time; every route lands
on the same evidence. Dig — death → sweep → killer → their other kills → the model's wall record —
with the View chain pushing each framing, so one `[<]` on the Levels floater returns to the framing
the dig started from [08]. File the claim with polarity and a note; the Ledger records what it
overrules and what changed. Copy the Case to the clipboard for a group notice.

**Briefing the group after the raid.** During the review the officer pressed `⚑mark` at each moment
worth showing. Marked Views are the chapter list [08]; lineage events (splits, merges, collapses) and
the biggest volleys pre-populate it. Stepping through a chapter parks cursor, window, ParamSet and
selection together, so the officer briefs by alt-camming — and when they have to walk to show
something, the Chart twin keeps the same picture available [01].

### The other users (§2.9)

**A combatant reviewing themselves.** No rank, no report, no case: a fighter parks at spawn after
dying and opens the tool on their own name. The Avatar article is the whole product for them — the
loadout strip showing they spent eleven minutes on a Layer 2 weapon they meant to switch off; the
kill-distance strip against the family median, which is the number the Discord feed has taught them
to care about; deaths with their killers and the delivery class of each; their own `fd` records,
which are the only sub-second-accurate events in the session and exist *only* for them ([analysis §5.1](combat_log_analysis.md)), so
their own page is quietly the most precise page in the tool. Their adjustments row shows the region
experience zeroing damage at spawn, which answers "why did that shot do nothing" without anyone
being accused of anything.

**A spectator.** Opens at R4, reads the ribbon, clicks a death cross in the base overlay layer,
lands on a Moment. Nothing gates. The one thing they cannot do is file a Claim under someone else's
name, because claims carry initials.

**A live peek between engagements.** The OIC is fighting. They stop behind cover, the overlay comes
up 5 s after leaving mouselook, and they read the R4 stage: their side is down to 6 tracked from 9,
the enemy took a vehicle-class step ninety seconds ago (hatched, `mech?` unconfirmed), deaths in the
last two minutes are all in one lobe on the
Ground face. They set `⚑ last life` from the transport, press nothing else, and go back. No raycast
ran, because Peek depth touches only accumulators (§2.9), and the frame they die in a moment later is
not the tool's fault.

### Intra-group skirmish

Both sides wear the same group tag. Three things happen without the officer suspecting anything. The
seed admission test fails condition (a) — one group id among the graph's vertices — so seeding
disables itself *before* any clustering runs, and the Braid header reads "group seeds unusable —
identical tags (test a failed)" [01]. The always-run tag-free second solve disagrees with the
tag-seeded one. This officer has switched the meter on (§3.6), so the `(!)` flag lights and the RED
band header reads `STRAINED 41%` [08]; an officer who has not still reaches the same place from R2,
one step later. Either way `[test split]` opens R2 with RED as the subject and the tag ablation
pre-run:
"drop group tags → same 3 sides, 17/20 agreement". They `[adopt this set]`; the Braid forks at 14:24,
the Matrix shows a clean block cut with `!` cells only along the boundary, and the bands are renamed
"1st" and "2nd" in place. Two boundary people get time-bounded overrides
(`effective_from 14:24`) so their earlier history is untouched [09]. The delta chip reports "3
friendly-fire flags removed"; the now-honest friendly-fire list has one real incident, which opens as
a moment and turns out to be a grenade at a doorway.

### FFA on a neutral sim

The structure index goes to noise inside three minutes. The tool prints DEATHMATCH, greys the bands,
stops naming factions, and the Braid becomes a per-person hostility heat strip that scales to 80 rows
[08]. The Front face refuses to draw — no contour survives — and says why; the Chart and the Ground
face carry the geography instead [01]. The respawn test ([analysis §5.7a](combat_log_analysis.md)) fails, so `d_spawn`, spawn rings and
the spawn-cohort affinity term all switch off with a `no-respawn` chip rather than inventing an anchor
[08][11]. Segmentation finds no troughs and rows arrive as labelled 3-min slices [11]. The working
nouns become **Pair**, **Band** and **Model**: two mutual-damage pairs stand out as running
duels; pin both and the Board shows their bouts side by side. Deaths remain fully attributable and
equipment stats are unaffected, because they never needed teams. Around 14:47 a stable positive block
of four re-forms and the tool raises one unnamed band, asking nothing. The important behaviour is the
refusal.

### Mid-fight arrival

Kai teleports in at 14:47 with no track history. His Braid strand is `·` until the first update, then
`short` — never a letter, because margin is undefined on one damage edge. His Ledger row draws `·`
until coverage arrives, so his volleys are visible while his position is not, and any bout he is
placed into carries the placed-event flag. His article headline reads "first seen 14:47:12 · 38 min
unobserved · team unresolved (n = 1 edge)".

---

## 7. Build order, mapped onto the five phases

### Phase 1 — Bridge (five deltas to the fixed plan)

Ship as planned, plus:

1. **Stamp every event with the simulator's frame number, not a script clock.** Script 2 calls
   `llGetEnv("frame_number")` and puts the integer string on each relayed event; the viewer fits
   frame→viewer time from the tracking ticks, which carry the same counter
   ([analysis §5.1](combat_log_analysis.md)). **The message must stay compact** — few function calls
   per event, short payload — and the exact wire format is the LSL author's to design against that
   requirement [owner]; what the viewer needs is the counter on every event and every tick.
2. **`CombatObjectInfo` carries the object's position at sighting.** This is what makes the
   LAST-SEEN tier real [05]; it is one extra field in an already-planned reply, not a new store.
3. **Region combat settings on handshake and on region change.** The nine `llGetEnv` values of §3.7,
   re-sent whenever the region changes, because `invulnerability_time` and `death_action` change how
   the whole death analysis reads and a stale value is worse than none.
4. **Seated agents' root object per tick — a placeholder, and a fallback.** The tick may carry
   `OBJECT_ROOT` for any agent showing `AGENT_ON_OBJECT`, from one `llGetObjectDetails` call [owner].
   Vehicles are derived viewer-side: a seated avatar is a child of the vehicle's linkset and the
   viewer already knows the root and moves the avatar with it, so this field matters only for agents
   outside this viewer's interest list. The owner is writing a board that reports it, so the field
   should exist in the format before it exists in the script.
5. **Raw-line arena for R−1, and it is in** [owner]: an append-only char arena plus
   `RawLine { F32 recv; U32 offset, len; U8 trust, salvageMask; U32 firstEvent, nEvents; }`, where
   `salvageMask` records which elements a truncated-array parse dropped. ≈ 25 MB at 100k events,
   evicted on the same retention sweep. The level says `raw` because it is raw.

**Region combat-system channels (teams / respawns / meta) are out of v1 and unlikely later** [owner].
The common case is **EBCS**, the Experience Based Combat System, which is open source and runs on many
combat regions; **FLECS** is the owner's own system on Resdayn and is one region's, not the community's
[owner]. Neither is listened to.
Nothing depends on them; every model that could have been seeded by one already works unseeded, and
the degraded chips naming their absence are gone rather than permanently lit. What replaces them is
two **debug settings**, the only file writes v1 has: `SSCombatLogChannelLog` writes every message seen
on the combat channel — system events and foreign, untrusted, script-written ones alike — to a log
file, and `SSCombatLogFileLog` writes the parsed combat log itself. Both off by default, both hidden
debug settings rather than preferences, and both for the same reason: the honest way to find out what
a region system broadcasts is to record a raid and read it afterwards, which is also the cheapest way
to design a listener later if one is ever wanted.

One optional listener stays on the list, cheap now and impossible to retrofit: **weapon self-reports**
on kit channels (MRCG-style hit lines carrying falloff, spread, magazine and reload —
unauthenticated, per-kit, so they enrich an equipment page and confirm nothing).

**Viewer-side, one addition that is not the bridge's:** the ghost-projectile tracker ([analysis §5.5](combat_log_analysis.md)) already
exists in the viewer and must be tapped in Phase 1, not Phase 5, because a flight not recorded while
it flies is gone. `SSProjectileFlight { U32 objId; F32 t0, t1; LLVector3 p0, p1, v; U8 endedOn; }`,
capped and evicted on the same retention sweep as events.

### Deltas to the plan's frozen contracts (decide before Phase 5 is coded)

Three places where this design changes a struct or enum the engineering plan treats as fixed. They
are cheap now and expensive after the analysis module is written against the old shape.

1. **Killing blow is not always a single event** ([analysis §5.6](combat_log_analysis.md)). The plan's Phase 5.1 says "killing blow =
   last DAMAGE". Here the last DAMAGE wins *only if* its `t̂` margin over the runner-up exceeds the
   combined `u` ([analysis §5.1](combat_log_analysis.md)). The attribution result becomes
   `struct DeathAttribution { EventRef blow, blowAlt; U8 blowState /*UNAMBIGUOUS, AMBIGUOUS_PAIR*/;
   F32 margin, uncertainty; std::vector<VolleyShare> ranked; }`, and **kill credit turns from an
   integer into `F32 credit`** (0.5 each on an ambiguous pair) in the per-avatar and per-equipment
   stats, plus a `killsHatched` counter so a hatched half-kill never prints as a whole one. The
   type/damage graft onto the DEATH still comes from the last DAMAGE, unchanged.
2. **LOS is a four-state enum and a curve, not a three-state scalar** ([analysis §5.11–5.12](combat_log_analysis.md)). `CLEAR /
   BLOCKED / UNKNOWN` becomes `CLEAR / MARGINAL / BLOCKED / UNKNOWN`, and the stored result is
   `struct LosResult { U8 verdict; F32 flipTau, flipFraction; U8 nClear, nBlocked, nMarginal,
   nUnknown; U8 tier /*§5.3*/; U8 anchorRobust; }`. The plan's single per-equipment "through-wall
   suspicion count" becomes three counters, because [analysis §5.12](combat_log_analysis.md) denominates over FIRM verdicts only and
   MARGINAL must be visible beside them rather than folded into either side.
3. **Pick model** (§4.6) extends the plan's Phase 4.4 rule ("consume plain left-click") with mouse-up
   resolution, the 4 px / 350 ms movement and dwell limits, never consuming modified presses, a 12 px
   radius and the pick-stack list. No data-model change — mouse-down state in `SSCombatOverlay` and
   one small popup `LLView` — but it changes the click contract, so it is listed here rather than
   discovered during Phase 4.

A fourth contract delta, cheap now and structural later: **the rule catalogue is a store, not a
screen** ([rules §5.22](combat_log_rules.md)). `Rule` records with a measurability class and a `ParamSet` need to exist before any
rule check is written, because a check coded against a hard-coded 5 m radius is a check that cannot
move to another region, and the second region is the point. It is a small store — tens of rows, text
and floats, **shipped empty** and filled per region by that region's own officers — and it belongs in
Phase 2 beside the ParamSet it borrows its sweep machinery from. Beside it sits one more small piece
of data, the **curated group list** ([analysis §5.9](combat_log_analysis.md)): optional, absent by
default, and read from the settings directory when present.

Four ingest-side additions belong with them and land in Phase 2, not Phase 5. Three are on
`CombatEvent`: `U8 delivery` — **four classes, not three** (hitscan / projectile / lobbed-arc / area,
[analysis §5.5](combat_log_analysis.md)) — `F32 u` / `U8 tSrc` for stamp quality ([analysis §5.1](combat_log_analysis.md)), and `U32 flightId` linking an event to the ghost
projectile that delivered it where one was seen. The fourth is on `Equipment`: **`U8 escalClass` plus
its own confidence byte** ([analysis §5.23](combat_log_analysis.md)). It is deliberately *not* an extension of the plan's `kind` enum —
`kind` keeps its six values and its existing evidence rules, and the sub-classes escalation needs
(mech, drone, staff, tank) live in a separate field with a different evidence path and a lower bar,
read only by the escalation lane, the R4 row and one offered summary fact. Extending `kind` instead would let a
`●●○` name-keyword guess leak into equipment ledgers, LOS tallies and rule checks, which is precisely
the confusion the split avoids.

### Phase 2 — Store and tracks

All accumulators land here, because [analysis §5.21](combat_log_analysis.md)'s contract is a property of ingest, not of the UI:
`t̂ ± u`, the clock offset, the `fd` dedup, the admission filters ([analysis §5.4](combat_log_analysis.md)), delivery-method
classification ([analysis §5.5](combat_log_analysis.md)), volley formation, the hostility graph, per-window damage, coverage, per-avatar
per-signal counts, teleport/DOWNED detection ([analysis §5.7](combat_log_analysis.md)), the equipment family key with its name-family
display key ([analysis §5.13](combat_log_analysis.md)) and the position-tier resolver ([analysis §5.3](combat_log_analysis.md)). Five more land here for the same reason —
cheap per event, and they are exactly what Peek depth reads (§2.9): **first use per side per
escalation class**, **headcount per side with the militia split**, **activity minutes in the OIC
HUD's own protocol** plus engaged minutes beside them, **distance-at-kill per family**, and the
**death-without-teleport** anomaly counter. The rule catalogue store lands here too, beside the
ParamSet whose sweep machinery it borrows. The synthetic-session generator from the plan is promoted
from a dev aid to the primary development surface: every screen below is built against it, and it
now has to emit the awkward cases on purpose — same-tag sides, militia without tags, an area weapon,
a lobbed weapon, a hitscan kit that teleports its damage prim onto the victim, a region whose
`death_action` is 3 so no death is followed by a teleport, and a stretch of heavy time dilation where
the sim frame counter advances at half rate.

### Phase 3 — Levels floater, Article floater, navigation

Chrome first, stages second. Breadcrumb with tinted-vs-plain operators, level rail with the printed
landing, ParamSet strip, ribbon, transport, evidence column, degraded strip. Then the View struct,
the two histories, anchors, pins, the `LLUrlEntry` registration and the Article floater skeleton with
the Level Row rule. Stages R0 (Moment) and R1 Ledger ship here; they need no analysis beyond Phase 2.
Prefs, menu item and floater registration as planned.

### Phase 4 — Overlay and the Chart twin

`SSCombatDrawList` first, then both consumers. Overlay levels R0 and R1, the permanent base layer, the
LOD table and caption chip, scale classes, measured-vs-reconstructed styling, the pick model with
mouse-up/4 px/350 ms and the pick-stack list. The **Chart renderer ships in this phase, not later** —
it is the same draw list and it is what makes the tool usable while walking; deferring it is how
parity drifts. **Reconstruction (§3.10) ships here too**, at the end of the phase: it is ghosts,
flights and hit discs on the draw list already built, plus one stock XUI panel, and it is the payoff
that makes the overlay worth having for someone who will never open the floater.

### Phase 5 — Analysis, sweeps, and the officer artifacts

In order: attribution and equipment classification (Q3, Q4, Q7); team clustering with seed admission,
the tag-free second solve, drift/noise/short and time-bounded overrides (Q2); LOS as a function with
MARGINAL, the anchor sweep and the input-quality gate (Q3, Q4); the occlusion point cloud; adjustment
patterns *including the 25 % armour cap and healing limits* (Q5); behaviour signals with Signal pages
and their mandatory context strips (Q8). Stages R2, R3 and R4 light up as their
inputs arrive. Claims, flags with disposition, Case, Trail and clipboard export close Q9. R−1 ships
last: it is the cheapest stage and it validates every other one.

**One reordering inside Phase 5, forced by the domain material.** The **Summary face** (§3.9) and the
rule checks that need no geometry — armour cap, healing limits, area radii against the written
kill/wound distances, fire into the spawn zone, restriction breaches — move to the *front* of Phase 5,
ahead of the LOS work. Two reasons. They are what makes the tool get used at all: an officer writes
something up after every single raid and investigates a wall kill perhaps weekly [reports], and the
adjustments surface is invisible today, so anything it shows is new information [owner]. And they are
cheap and self-contained, so shipping them first buys real 40-agent sessions to measure the raycast
budget against, instead of designing the expensive half around guesses. Combatant summary statistics
([analysis §5.24](combat_log_analysis.md)) and
the own-side compliance list follow immediately, since both are queries over accumulators Phase 2
already keeps. The template mechanism ships with them, empty. Escalation brackets need only equipment classification and land with it.

**A separate Soapstorm feature the owner wants, recorded here so it is not lost.** The dev-view
*broadcast* idea is dropped: the tool ingests no toggle events and never treats one as a signal. What
the owner asked for instead has no combat log in it at all — Soapstorm should **disable its own
misuse-prone debug features while the user is actively participating in combat**: wireframe, hitbox
and bounding-box displays, Atmo Magic footstep markers, scripted-object beacons, sound-source beacons
and their kin. The gate is the overlay's own run the other way round: disabled in mouselook and OTS
and for the 5 s exit window, available again once the user has clearly stopped fighting. It belongs
to whatever part of Soapstorm owns those displays and needs its own design [owner].

Deferred past v1: cross-viewer address resolution, session files, multi-region raids, catalogue
exchange between officers as anything richer than pasted text, and any composite score of any kind.
Three named future items, each recorded rather than designed: **owner-designed polygon zones** and
everything that waits on them (objective time, Place and Hotspot naming); **a cross-session store**,
which starts by logging the interesting facts of each session — equipment families seen, groups seen,
region, duration, who was present — and which the tool should be shaped to feed later even though it
writes nothing today; and **squad-level teamwork, tactics and strategy analysis**, which the owner
calls the most interesting thing the data could support and which is large enough to deserve its own
design competition rather than a paragraph here.

---

## 8. Open questions

The owner answered the first round on 2026-09-08 and those answers are now design rather than
questions. What is left is genuinely open, plus what the answers themselves raised.

1. **The compact wire format.** The requirement is fixed — the simulator's frame number on every
   event and every tick, few function calls, short payload — but the encoding is the LSL author's
   [owner]. Two details worth settling early: whether the frame number rides every event or once per
   batch with per-event deltas, and whether `fd` records carry it too (they should; they are the only
   sub-frame-accurate events in the session).
2. **Escalation class sets per group.** [analysis §5.23](combat_log_analysis.md) fixes how a
   sub-class is *derived*; whose tier ladder it draws is still open, which is part of why the lane now
   ships off. Does a group configure its tiers as named class sets beside the rule catalogue, with
   keyword lists lifted from its own armory pages, or does the lane draw a fixed
   explosives / vehicles / mechs / drones / staffs tier ladder that groups map onto? Only the first is
   honest about "went to tier 3" meaning different things on different sims.
3. **Zone authoring, whenever it happens.** The future shape is region owners drawing polygon zones,
   mesh-based and shareable (§2.1). Open: who authors them, what they are stored as, and how they
   travel between officers when the tool writes no files. Everything marked *requires owner-designed
   zones* waits on this.
4. **A universal rule core, later.** The catalogue ships empty. If a curated core is ever added — no
   wireframing, no shooting through walls, safe zones, LBA HP classes — who curates it, and does it
   arrive as suggestions accepted row by row or as a starting catalogue? Shipping it as a default is
   the one option already ruled out.
5. **Officer status, derived how far?** Estate managers first, the dominant land group's
   parcel-edit roles second ([rules §5.22](combat_log_rules.md)). Both are cheap and both are wrong
   sometimes — an estate manager may be a builder, a land group's roles may not match its command
   structure. Is a manual override needed, and whose?
6. **Health estimation, how far to trust it.** The estimate
   ([analysis §5.25](combat_log_analysis.md)) is genuinely new and genuinely uncertain, and draws
   hatched everywhere. May it ever be *cited* — in a Case, in an Incident — or is it strictly a
   reading aid for the reconstruction and the Life strip?
7. **Reconstruction window and cast.** §3.10 opens on `[t − 6 s, t + 2 s]` with everyone holding a
   damage edge plus the bout hull. Both are guesses: longer tells more story and costs more ghosts,
   narrower reads more easily and may omit the person who mattered. Worth setting against a real raid.
8. **The known-group list, and who maintains it.** Using the owner's `slmc-data` schema settles the
   format, not the practice ([analysis §5.9](combat_log_analysis.md)). Does Soapstorm ship a copy
   that ages, or read whatever is in the settings directory and say plainly when it has none?
9. **Group export templates and summary-stat mappings.** What is the smallest template language that
   covers the formats groups actually use, and does a group ship it with its rule catalogue? §3.9
   assumes a section list, labels, an ordering and a per-participant line format, which covers the one
   format we have read end to end and may cover no other. The same question decides where a group's
   mapping from summary statistics ([analysis §5.24](combat_log_analysis.md)) onto its own scheme
   lives — beside the template, beside the catalogue, or nowhere in the viewer at all.
