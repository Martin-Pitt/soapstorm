# Entry 03 (revised) — Officer Workflows: the tool that writes the raid report

## 1. Thesis

An officer never opens this tool to "look at data". They open it because a job landed on them: *write tonight's raid report*, *decide whether this Nammu gets E-2*, *find out whether the enemy drone was really eating anti-armor damage or the complaint was noise*. All three jobs already have artefacts — the raid report (duration, Personnel with a `(Militia)` split, Report, Incidents, an "OIC HUD Output" block of `agent-uuid,minutes-active` lines), the Merit System's award categories, and the Incidents paragraph officers currently write from memory. So this design works backwards from the artefacts. The organising unit is the **Case**: a named container in one of three templates — **Raid Report**, **Merit Sheet**, **Incident File** — opened *before* digging (the Raid Report opens with recording and fills itself live) and into which every view drops **Evidence Cards** through a single **Pin** gesture. A card is a frozen snapshot of an already-computed fact plus its uncertainty and a permalink back to where it came from. Analysis stays the five modules the engineering plan fixes; workflow-first adds that a Case is finished when its pinned evidence, read in order, *is* the post the officer was going to write anyway — and that the tool emits no verdict, no flag to anyone but the reviewer, and no broadcast. Officers asked for "a tool for OICs to use to investigate potential issues", not policing, and every surface here is private to the person looking at it.

## 2. Ladder

Every rung is entered from a job, and every rung is pinnable.

- **Rung 0 — the instance.** One death, one damage line, one LOS candidate, the overlay at the cursor time. *Down:* clicking anything anywhere — roster row, heat cell, trail segment, rule-query hit — opens the **Inspector**. *Up:* every Inspector offers "this minute", "this avatar's session", "this weapon's session".
- **Rung 1 — over time.** Trails instead of dots; the Overview's session-shape strip; a Dossier ribbon carrying activity minutes, mouselook bands, damage spikes, deaths and loadout changes in one band. *Down:* click a spike or tick. *Up:* compare to the session distribution.
- **Rung 2 — over a parameter.** The LOS sweep across the travel-time window; the **team cut slider** moving a cut height through a stored merge tree (the cluster count changes with it); damage replayed as `initial` vs `damage` to show what armour scripts removed. *Down:* "Pin this reading" freezes the parameter value into a card.
- **Rung 3 — many dimensions.** The Team Ledger, the Equipment grid keyed on *families* (maker prefix + stem, version stripped), the **Merit Sheet** (Merit category × counts × episodes), the **Rule Book** (written rule × measurability × candidate hits). *Down:* any cell opens the Dossier or candidate list behind it.
- **Rung 4 — the Case.** An ordered stack of cards plus officer prose, the only rung a human authored. *Down:* every card links to its origin. *Up:* Export renders the artefact.

Entry rungs differ by job: Raid Report enters at Rung 1 and samples down; Merit Sheet enters at Rung 3 and must step down at least once per category before an officer can claim they *observed* anything; Incident File enters at Rung 0 (the complaint) or Rung 3 (a Rule Book row) and climbs to find pattern. **Peek mode** is the Overview plus the live Raid Report skeleton — duration, who is here, escalation tier, deaths since last look — readable in five seconds between engagements.

## 3. Information architecture

**Nouns:** Session, Place (officer-named zone; region-system respawn points and battlefield bounds pre-populate), Encounter (a brushed space+time region), Minute, Avatar, Side, Squad, Equipment family, Equipment instance, Death, Damage cluster, LOS verdict, Adjustment script, Rule, Case, Evidence Card.

**Panels.** *Combat Log* (hub): transport, lane timeline, tabs **Overview / Roster / Sides / Equipment / Rules / Cases**. *Dossier*: one chrome — breadcrumb, ribbon, related, backlinks — with a per-kind stat body (Avatar, Side, Squad, Equipment family, Place). *Inspector* (Rung 0). *Case Binder* (three templates, one chrome). *Export sheet*. Evidence Cards are widgets, not floaters.

**Navigation.** Each floater instance owns its **own** nav stack and breadcrumb; the Inspector is a singleton by default, but any Dossier or Inspector can be **torn off** into an independent instance with its own history — what a Q3 investigation needs when the victim's and the killer's pages must sit side by side. Related rows are outbound links; Backlinks are computed lazily by scanning the store for references to this id ("killer in 6 deaths, cited by 2 cases, matched by 1 rule query").

**Exactly one Case is Active** at a time. The Cases tab has a radio column; the active Case's name and accent colour show in the main floater's status line and in every Pin button's label ("Pin → Raid Report 2026-09-08"). Pins, tack glyphs and overlay accents all refer to that one Case; switching is one click. Multiple Binders may be open; only one receives pins.

## 4. Screens

**Combat Log (main floater)**

```
+-- Combat Log ------------------------------------------------------------+
| [Live] [<<][Play/Pause][>>]  1.0x  [-1s][+1s]      23:14:06   dur 1h14m  |
+--------------------------------------------------------------------------+
| deaths   | . ... .:| ..::|. .  ::|.. .           :|.               |     |
| escalat. |--basic-----|EXP 22:41|VEH 23:02 (them)|MECH 23:09 (us)  |     |
| presence |=us 9=====|=+2 militia=======|=8=|   them ~7 -> 11 -> 6       |
|          ^cursor 23:14                          ^last life (set 23:31)  |
+--------------------------------------------------------------------------+
| [Overview] [Roster] [Sides] [Equipment] [Rules] [Cases]                   |
+--------------------------------------------------------------------------+
| Hotspots (auto, unnamed):  A 22:10-22:45 (34 deaths) [name...]           |
|                            B 23:00-23:20 (19 deaths) [name...]           |
| Brush: [box]+[time] -> [Make Encounter]                                   |
| Last 5 deaths ................................. [open in world]          |
+--------------------------------------------------------------------------+
| rec ON | bridge OK | ev 8,412 | agents 37 | RTT 210ms | drop 0           |
| ACTIVE CASE: Raid Report 2026-09-08 (live)  [switch v]                    |
+--------------------------------------------------------------------------+
```

Three lanes matter: **deaths**, **escalation** (first appearance per side of explosives, vehicles, mechs, drones, staffs — the "went to tier 3" question), and **presence** (headcount per side, militia band drawn separately). Spatial clusters are labelled `Hotspot A/B` — auto-detected, *unnamed* — until an officer names them, because the tool has no place names; naming makes a Place noun and the label is thereafter marked officer-authored.

**Sides tab — Team Ledger with a dendrogram cut**

```
Cut height: [-----o--------] 0.42   -> 2 sides, 4 squads   [why? ] [snapshot]
Seeds in use: region-system teams: none | group tag: 1 value (uninformative)
              attachment prefix: [Ash]x9 [Ex]x6 | spawn cluster: 2 | damage graph: primary
Name             Side     Conf  Squad  K  D  DmgOut DmgIn  FF-out FF-in  Mil
Owen Vance       A (Ash)  0.86  A1     14  6  8220   3110    0     1      -
Doran Reyes      A (Ash)  0.61  A1      9  4  4010   3900    1     0      -
"Kite" (no tag)  A (Ash)  0.74  A2      3  5  1180   4400    0     0     yes
Mara Lindqvist   B (Ash)  0.90  B1      7  9  3300   6100    0     2      -
[row -> Dossier]  [override side v]  [mark militia]  [Pin ledger snapshot]
```

The slider does not re-run clustering: the analysis stores an agglomerative merge tree over the damage-affinity graph, and the slider moves the **cut height**, which legitimately changes the cluster *count*. "Why?" opens the seed panel above, which is also the honesty display: it states which allegiance signals were informative and which degenerated.

**Inspector (Death)** — LOS notation fixed to the plan's rule

```
+-- Inspector: Death - Mara Lindqvist @ 23:14:06 --------------------- [x] -+
| < back | Overview > Hotspot B > Death 23:14:06        [tear off] [Pin v] |
+--------------------------------------------------------------------------+
| Victim Mara Lindqvist (B)   Killer Owen Vance (A)   dist 41.3 m          |
| Weapon [Ash] Kagrenac bolt-caster v1.1.0 (family: Kagrenac bolt-caster)  |
| Delivery: PROJECTILE (source unattached, lifetime 0.9 s) -> travel window|
| Killing blow 41 dmg, type 8 piercing. Feed line: "Owen Vance erased      |
| Mara Lindqvist with [Ash] Kagrenac bolt-caster v1.1.0 from 41 m away"    |
+--------------------------------------------------------------------------+
| Damage in window -6.0s..0s (3 events, 2 contributors)                    |
|  -3.8s 12 piercing  LOS CLEAR    (7 pts: 3 clear / 4 blocked / 0 gap)   |
|  -1.1s 18 piercing  LOS UNKNOWN  (7 pts: 0 clear / 3 blocked / 4 gap)   |
|  -0.0s 41 piercing  LOS BLOCKED  (7 pts: 0 clear / 7 blocked / 0 gap)   |
|  occluders on the blocked line: 2 (0.34 m, 0.9 m thick)                  |
+--------------------------------------------------------------------------+
| Sweep travel window [0.4s ---o------ 3.0s]  step 0.25 s   [Pin reading]  |
| Adjustments on this victim in window: none                               |
| Context: no teleport within 2 s after death (expected: spawn TP) [!]     |
+--------------------------------------------------------------------------+
| [Show in world] [Track killer] [Killer dossier] [Weapon family]          |
+--------------------------------------------------------------------------+
```

Verdict rule, restated and used consistently: **CLEAR** if any tested candidate is clear; **BLOCKED** only if every sample point in the window had track data and all were blocked; **UNKNOWN** otherwise, captioned "leaning blocked, 4 of 7 points had no track data". A gap is never rendered as blocked. Hitscan deliveries (source attached, or `source == rezzer`) get one candidate at *t* and no travel window, so those verdicts are the strong ones; projectiles carry the window and the caveat.

**Case Binder — Raid Report template (the artefact)**

```
+-- Case: Raid Report - Resdayn - 2026-09-08 ------------------------- [x] -+
| Type [Defense v]  Duration 1h14m (auto)   Last life called [23:31] [set] |
+--------------------------------------------------------------------------+
| Personnel (9)   auto from side A, group tag [Ash]        [edit list]     |
|   Owen Vance, Doran Reyes, ...                                          |
| (Militia) (2)   fought with A, no group tag, conf 0.74/0.66             |
|   "Kite", Sera Blackwood                                    [demote v]  |
+--------------------------------------------------------------------------+
| Report (prose): [ .................................................. ]  |
|   suggested beats: 2 hotspots, escalation to MECH 23:09, 3 v 7 hold     |
|   at Hotspot B 23:02-23:19 (outnumbered 20 min)              [insert]   |
| Incidents: 2 linked Incident Files + [new]                              |
|   #1 drone not taking anti-armor  #2 death without spawn TP x3          |
+--------------------------------------------------------------------------+
| OIC HUD Output (activity minutes)          [OIC-compatible v] [copy]    |
|   a1b2...c3,64      d4e5...f6,71      (yaw-change-in-5-min protocol)    |
|   alt column: engaged minutes (any damage/track motion): 58 / 69        |
+--------------------------------------------------------------------------+
| Pinned evidence (6)  [+]        Departures after last life: 1 [view]    |
+--------------------------------------------------------------------------+
|                             [Save Draft]  [Export ▾ forum post / chat]  |
+--------------------------------------------------------------------------+
```

Export renders the forum shape officers already post: type tag, duration, Personnel, `(Militia)`, Report, Incidents, `OIC HUD Output` as `uuid,minutes`. Activity minutes use the **same definition the OIC HUD uses** (minutes in which rotation changed, bucketed in 5-minute windows) so numbers stay comparable; the better measure is a second column, never a silent substitution.

**Merit Sheet template** replaces the report block with the Merit System's categories:

```
Subject "Kestrel Voss" (E-3)      Window [this session v] / [last 30 days]
Besieger   raids attended/offered   7 / 9      Assassin  K, K/D, multikill 14, 2.3, 3
Defender   defences attended        4          Pointman  time in enemy zone  0 (none)
Flagbearer contested hold time      6m40s      Specialist anti-armor / built 1240 / 2
Field Cmdr raids as OIC             0          Pillar    not observable      annotate
Melee / magic kills by damage type  2 / 0      [each cell -> episode list]
"counts, not judgements - open at least one episode per category you cite"
```

## 5. In-world overlay

The overlay is where study time is spent. At Rung 0 it draws trails (side colour, alpha ramping old→new, dashed where a track gap means *unknown*, never solid), damage lines from attacker track to victim track coloured by damage type, and death crosses at `target_pos` with a killer line from `source_pos` and a label phrased like the Discord feed line the group already reads (`Owen erased Mara — bolt-caster — 41 m`). LOS candidates draw green for clear, red for blocked, **grey dashed for no-track**, width by candidate count. Named Places draw as translucent floor rings; the red spawn zone draws permanently once defined, because "deaths and AoE inside it" is a standing query. Explosive events draw their 5 m / 10 m rule radii so the officer can *see* whether the third victim was inside the written limit. Rung 1 in-world is the whole-session picture: every trail at once for a brushed Encounter, deaths as a scatter of crosses, and a **through-wall ribbon** connecting repeated blocked kills by one equipment family.

Pinned things get a small **tack glyph** in the Active Case's accent colour, so alt-camming becomes a walk through the evidence gathered so far, and gaps in the tack pattern are gaps in the case.

Click semantics, deliberately unoverloaded and modifier-free (ALT and CTRL+ALT belong to the camera): **plain left click always opens the Inspector** on the marker, whatever its tack state. The Inspector header shows "in Case: card #4 [go]" when the thing is already pinned, and its `[Pin ▾]` button is the only pin gesture, plus a `P` hotkey that pins the current Inspector subject. Hovering a marker shows a one-line tooltip and the tack state before any click. Nothing ever yanks a Case Binder into view unasked.

## 6. Analysis model

- **Sides, squads, militia.** Affinity = damage dealt (negative weight) plus co-movement (median separation while both moving) and shared spawn cluster. Agglomerative clustering stores a **merge tree**; the cut slider is a Rung-2 view over it. Seeds are *optional priors* listed with their informativeness: FLECS-style region-system team broadcasts (high confidence when present), active group tag, shared attachment-name prefixes, spawn zone. **When the group tag collapses to one value — the intra-group case — that prior is dropped and the tree is built on damage and co-movement alone**, which the seed panel says outright rather than silently. Squads are sub-cuts by co-movement. **Militia** = clustered onto a side but lacking its group tag; an inference with confidence and an override, because that is exactly the `(Militia)` list the report needs.
- **Friendly fire.** Flagged per event when attacker and victim share a side across the *plausible cut range*; if the flag flips as the cut moves it shows as **disputed**, and that instability is the uncertainty signal.
- **Death attribution.** Proportional assist credit by cumulative damage in the window; distance-at-kill from `source_pos`→`target_pos`; feed-line text per death, with `**` for owner-less object/vehicle kills and ordinary rows for self-kills.
- **Equipment families.** Key = maker prefix + name stem, versions stripped, so `[Ex] PP19 Bizon v 1.04` and `v 1.05` aggregate. Per family: users, hits, kills, kill-distance distribution, hit-vs-effect visibility (a raycast weapon with no visible projectile reads "misses unobservable"), adjustment ratios, blocked-kill rate. **Unresolved keys are first-class rows**: `source 3f21… (unknown, seen 0.4 s, 12 hits)` with its evidence, never hidden.
- **Rule Book.** Each written rule is a stored **claim per region** with a measurability tag and, where possible, a standing query: 360° armour ≤25% reduction → `damage/initial` per victim per script from `modifications`; AoE 5 m kill / 10 m wound → distances from source to every target hit by that source within 1 s; explosives must not kill through walls → LOS from the blast point; sprint ≤5 s with ≥6 s recharge, dash/roll ≤15 m and not in air → burst-speed segments plus `AGENT_IN_AIR`; no shooting into the red zone → Place + track; turret ≤180° field of fire, constructed not instant → rez-to-first-shot and fire bearings. Unobservable rules (LBA compatibility, avatar height, prejump) are **annotate-only**, so an empty result is never read as compliance. Rules negotiated mid-fight ("asked not to use basket grenades at 17:26") attach to an equipment family as a timed claim, and later hits are listed as **rule-breaking after the request** rather than merely suspicious.
- **Immunity patterns.** Per combatant: which `task_id` objects adjusted incoming damage, resolved to owner/creator so the region's experience attachment is distinguished from a personal HUD, plus the *pattern* — immune at the spawn hub (expected), immune bystander who never dealt damage (fine), immune while dealing damage mid-field (the one worth a card).
- **Death without teleport.** DEATH and the 8–24 m unexplained position jump are recorded separately and linked; a death with no teleport within 2 s is an anomaly row — a fault officers already chase by hand.
- **Behavioural aids (never verdicts, never broadcast).** *Tracking-through-walls*: body **yaw** from the bridge poll at ~2 Hz — labelled body heading, not aim, because no other avatar's camera vector exists in the data — plus movement heading, held toward the nearest occluded enemy over time, with elevation tracking and shots into occluders strengthening it. Every such card carries a mandatory **context strip** of the preceding 15 s (incoming hits, explosive/AoE events, nearby effect sources) because blindfire after a flashbang is a documented false positive, with the note that effects dealing no damage are invisible to the log, so a clean strip is not exculpatory. *Response latency*: first incoming damage to first outgoing damage against that attacker, straight from the combat log, with no notion of "targetable" (the data cannot support one), shown as a percentile of the session's own distribution. *Dev-view toggles*: if a companion Soapstorm feature writes hitbox/wireframe toggles to the combat channel, they ingest as untrusted self-reported events, visible only in the reviewer's Incident File, with the caveat that only Soapstorm users emit them and silence proves nothing.
- **Activity minutes.** OIC-compatible (yaw changed within 5-minute windows) and engaged (any damage or >2 m motion), both per participant.
- **Case completeness.** Incident Files show `3/5 checks` from which evidence *kinds* are pinned (instance, parameter sweep, context strip, prior history, counter-evidence), so nobody writes a conclusion on a thin file.

## 7. Walkthroughs

**Q1 — how did the raid go?** The Raid Report Case has filled since recording started, so the first move is Peek: 1h14m, 9 + 2 militia, explosives at 22:41 (them) and our mech at 23:09, two hotspots. Then the *step down and back*: the officer clicks Hotspot B, alt-cams to it, and the overlay draws 23:02–23:19 — three of our trails against seven of theirs holding a courtyard. He clicks one death cross → Inspector → confirms the 3 v 7 was real and not an artefact of people being out of tracking range (the presence lane says tracked, not inferred), then climbs the breadcrumb back to Overview and pins the Encounter as a **commendation card** reading "held Hotspot B 3 v 7 for 17 minutes until reinforcements returned". Personnel is checked (one militia demoted to "randoms" by override), Incidents linked, Export produces the forum post.

**Q3 — why did X die at 23:14?** A no-LOS complaint arrives. New Incident File, subject = the death. Roster → Mara's Dossier → death tick on her ribbon → Inspector. Delivery is PROJECTILE, so the travel window applies; the killing blow reads BLOCKED with full coverage and two occluders, 0.34 m and 0.9 m thick — walls, not thin-wall splash. Sweeping the window to 3.0 s keeps it blocked. He tears off the killer's Dossier alongside, opens the **weapon family** page (`Kagrenac bolt-caster`, 4 of 21 kills blocked, coverage 17 of 21 — weak but not suppressed), then climbs back to Overview to place the death: 23:14 sits inside Hotspot B, not a quiet rear area, which raises the plausibility of lag and crossfire. Family stat and hotspot context are pinned as evidence *and* counter-evidence; the officer sets Verdict = Inconclusive and links the file into tonight's report.

**Q8 — did anyone behave suspiciously?** In the same file, the officer opens the tracking-through-walls aid on the killer: 22 s of sustained yaw toward an occluded enemy at 22:58 with elevation following, and two shots into an occluder. Before it can be pinned, the context strip shows a `[UGL Blast]` on him 4 s earlier — the blindfire-after-flash pattern the group's own threads warn about. He pins the episode *with* its context strip and writes "consistent with blindfire after a flash; not pursued". Completeness reads 5/5 and the file exports as an Incidents entry containing no accusation.

**Intra-group skirmish.** Two Ashguard teams, one defending. The seed panel reads: region-system teams none; group tag 1 value — **uninformative, prior dropped**; attachment prefixes identical on both sides; spawn clusters 2. The tree is therefore built from damage and co-movement alone, and the cut at 0.42 gives two sides whose boundary survives being dragged to 0.30. Friendly-fire flags concentrate on three avatars whose membership flips — shown as disputed, not as violations. The officer overrides two from their Dossiers (each only ever damaged one side) and pins the ledger snapshot, frozen at that cut.

**FFA on a neutral sim.** No stable sides, so the officer stops trying: on Overview he drags a box around the north platform plus a range of 22:40–22:52, hits **Make Encounter**, and gets a scoped noun with its own ledger, deaths and equipment. Three such Encounters become the report's structure — a sequence of skirmishes with side-switchers listed as such, and uninvited randoms recorded as an Incidents line rather than forced into a scoreboard.

## 8. Risks and open questions

- **The report is the point of failure.** If the exported post does not paste cleanly into the group's forum shape, officers keep using the OIC HUD and ignore this tool. The `uuid,minutes` block must match the existing protocol even where our own measure is better.
- **Militia inference is socially loaded.** Calling someone militia from a tag they forgot to wear is an insult with a confidence score attached; override is one click and the exported list reads as the officer's, not the tool's.
- **Rule queries invite mechanical enforcement.** "7 candidate hits" is one editorial slip from being pasted as "7 violations". Candidate lists stay private, unranked, and captioned with the measurability tag.
- **Merit counts flatten judgement.** Multikill windows, objective zones and "won't die holding it" approximate things command staff award by observation; the sheet must keep forcing an episode open before a category is cited.
- **Cut-tree cost.** Merge trees are held per time bucket for scrubbing; incremental update across a 2 h session at 40+ agents is unproven and needs a measured budget.
- **Escalation depends on classification.** "First mech at 23:09" is only as good as vehicle/mount detection, which is where equipment evidence is thinnest.
- **Cases are session-scoped.** Export is the only durability; losing an Incident File is worse than losing the report, because the report can be rebuilt and the investigation cannot.
