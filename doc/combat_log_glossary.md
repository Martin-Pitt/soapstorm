# Combat Log — shared-language glossary and naming policy

The tool speaks to officers who already have a vocabulary. This document fixes which words are
**theirs** (and must be used exactly as they use them), which words are **ours** (and must be justified,
defined and kept out of collisions), and which of ours should change before any of it reaches a label.

Three parts:

1. **SLMC shared language** — the native terms the tool must speak, their meaning, their source, and
   where the tool uses them. **Combatant** is fixed by the owner is the
   load-bearing word.
2. **Tool vocabulary audit** — every coined or repurposed word in `doc/combat_log_ux.md` and the
   design entries behind it, with a keep / rename / drop recommendation. **Every rename proposal in
   Part 2 needs owner approval before it is written into a label, a page title or a struct name.**
3. **Naming policy** — one page of rules that decides the next word without another audit.

Sources are marked: **[owner]** the owner's own answers, **[reports]** the ~130 Ashguard raid reports
and threads, **[rules]** written sim rules (Ashguard, Chaos Indivisvm, Epsilon, Grand Federation),
**[merits]** the Merit System / Enlisted Handbook / Combat Avatar Policy, **[c2]** the Linden Combat 2.0
wiki and log fields, **[ebcs]** the Experience Based Combat System and region combat systems generally,
**[trackers]** the owner's own
Ashguard-FLECS misc boards and `slmc-data.lsl` group/region registry, **[sl]** plain Second Life
platform vocabulary.

**[trackers] outranks everything except [owner].** Where the owner's own boards already define a term
in code — *active combatant*, *crew*, *vehicle*, `OBJECT_DEATH`, the region types, the army/group
registry — the tool uses that definition verbatim, so the owner's data file drops in unchanged and the
tool's counts match the boards the group already reads.

---

## Part 1 — SLMC shared language the tool must speak

These are not our words. Where the tool needs a noun and one of these fits, this is the noun. If a
label here disagrees with a label in the design, this document wins and the design moves.

### 1.1 People

| Term | Meaning in the community | Source | Where the tool uses it |
|---|---|---|---|
| **Combatant** | An active participant on the battlefield. **Fixed by the owner as the word for active participants.** | [owner] | The person page's subject noun. Headcounts, presence lane, Personnel list, per-person stats, merit observables, damage-adjustment patterns. Replaces "avatar" and "agent" in all UI copy about people. |
| **Active combatant** | The community's own operational test, already implemented in `active-combatants-board/tracker.lsl` (board subtitle: *"Recent damage or in mouselook in a combat area"*). An agent **becomes** a combatant when they are the `owner` of a `DAMAGE`, `DEATH` or custom `OBJECT_DEATH` event, or when `llGetAgentInfo` shows `MOUSELOOK` or `ON_OBJECT` while they stand in a **combat area**. They **stay** one while in mouselook or seated on an object; otherwise they **expire 2 minutes** after their last damage activity, and they **drop** when they leave the region. | [trackers] | **The tool's Combatant/Civilian split uses this test verbatim**, so the tool's headcount and the group's own board agree. It is what the presence lane counts, what "engaged" means, and what the team solve's per-second normalisation is denominated in. |
| **Combat area** | A parcel with `PARCEL_FLAG_ALLOW_DAMAGE` set. | [trackers][sl] | The predicate behind *active combatant*; also a usable "is this ground in play" test for the Zone/Cell pages and the firing-from-outside-the-battlefield check. |
| **Civilian** | Traditional word used in the SLMC past, not that much anymore -- A non-combatant present in the region: a bystander, spectator, builder, or someone under the region's safe-zone protection. Operationally, an agent present in the region who does not satisfy the active-combatant test. **Also a real group in the registry** — armies keep a `civilians` group (e.g. "Ashguard - Civilians"). **Fixed by the owner as the historical SLMC word for non-combatants.** | [owner][trackers] | Presence counts (drawn separately, never inside a side's headcount); the §5.14 immunity pattern currently written as "immune bystander" must read **immune civilian**; the session summary's combatant list never includes civilians. A civilians-group tag is a strong civilian signal and is never treated as a side in team clustering. |
| **Crew** | The agents seated on a vehicle. Crew membership is maintained per vehicle, and the vehicle is dropped when its last crew member leaves. LBA rules already price crew ("+750/min per extra crew"). | [trackers][rules] | The word for seated combatants everywhere: crew lists on the vehicle page, crew-seconds on the person page, and the rule that a mount fired by its crew uses the *vehicle's* track, not the seated body's offset. Replaces the design's "seated avatars" / "rider". |
| **OIC** — officer in command | The officer designated per event; "the OIC is law during the raid", complaints go OIC to OIC, and the OIC can be *taken* mid-raid through the OIC HUD. | [owner][reports][rules] | OIC tenure lane; the exception list on every Rule; the framing sentence on every candidate list; the authorship of the session summary. |
| **Militia** | Non-members who fight alongside a group without wearing its main tag. Listed separately from full members wherever a group writes up an event. **An army usually has a real militia group**, recorded as the `m` field of its registry entry. | [reports][trackers] | The militia split in the session summary's combatant list; the presence lane's separately-drawn militia row; the allegiance model's "fought with a side without its tag" case. **Check the registry's militia group first**: someone wearing the army's militia tag is militia as a fact, not an inference. Only where no militia tag is worn is it inferred, and then always with its confidence band and a check-before-posting warning. The group's own name for that militia group comes from the registry — the tool never hard-codes one. |
| **Recruit** / **trainee** | Someone in the group but not yet a full member. Every group has the concept; each has its own name for it. | [merits] | Generic UI word in promotion-review copy. **The tool never infers rank** and never prints a group's own rank name unless it came from the registry or the officer typed it. |
| **Command** / **officers** | The body that approves membership and runs the group. Every group has one; each has its own name for it. | [merits] | Generic UI word. Same rule: no inference, no hard-coded group name. |
| **Member** | A full member of the group, as against militia, recruits and civilians. | [merits] | Generic UI word for combatant-list entries and per-combatant summaries. |
| **Squad** | A sub-group of a side that moves and fights together; the owner wants analysis "at individual, squad and team level" and promotion criteria ask about "moving with the squad". | [owner] | **The proposed replacement for the design's "Band" noun** (Part 2). Co-movement / mutual-support clusters inside a side. |
| **Randoms** / **combat cuck** | Uninvited third parties who join a fight. "Randoms" is the printable one. | [reports] | The officer can demote a proposed militia entry to **randoms** in the session summary. The UI prints "randoms" or "uninvited"; it never prints the other one. |
| **Spectator** | A curious non-participant with the tool open. | [owner] | Audience model only (§2.9). A spectator is a Civilian in the data. |

### 1.2 Events, sides and the shape of a fight

| Term | Meaning | Source | Where the tool uses it |
|---|---|---|---|
| **Raid** | The default event: a group teleports in, gathers, pushes out. | [owner][reports] | Session = one raid, one region. |
| **Session type** | What kind of event this was — an attack, a defence, a free-for-all, an operation. **Every group has its own list and its own names for them** (§1.4), so the tool holds this as a configurable list, not a shipped enum. | [reports] | The Session page's type field and the session summary export's heading. Seeded from the region type and the home side (a BASE region with a registered owning army defending is a defence) and always officer-editable. |
| **Side** | One of the two (or more) parties to a fight, as officers speak of them: "our side", "the other side's OIC", first use "per side". | [owner][reports] | Escalation lane rows, presence lane, report Personnel. Used where the grouping is being spoken about; **Team** is used where the grouping is the tool's inferred object. |
| **Team** | A detected/declared fighting group. The owner's own word: "detecting distinct groups/teams", "two teams from the same group". Also the `{name, color}` team object a region combat system publishes, where one does. | [owner][ebcs] | The Team page, team colours, team clustering, the Matrix blocks, friendly-fire definition. |
| **Army** | The registry's noun for one military organisation and its whole family of groups: `{g: main group, m?: militia group, c?: civilians group, l?: land group, n: name, s: shorthand}`. About 30 are registered today. | [trackers] | **The curated group list uses this exact schema** so the owner's `slmc-data` file drops in unchanged. An Army resolves four group ids to one name and one shorthand, which is what turns four unrelated tags into one side. The Team page names the Army when one matches. |
| **Group** | An SL group and its tag. One army holds several: main, militia, civilians, **land**. A *land-holding* group differs from the *membership* group, so a bare tag is a weak hint — but a tag matched against the registry is a strong one. | [owner][trackers][sl] | The group-tag seed prior, its admission tests, and its mandatory ablation. Registry-resolved tags (main / militia / civilians / land, all mapped to one Army) are a stronger seed than a raw unknown group id; an unregistered tag stays weak. Never a side on its own. |
| **Land group** | The group that holds the parcel, distinct from the membership group. Recorded as the `l` field of an army and of a region. | [owner][trackers] | Explains a tag that looks like a side but is not; resolves the region's owner even where nobody wears the main tag. |
| **Shorthand** | The army's short label in the registry (`s`), e.g. `[Ash]`. | [trackers] | The label on presence lanes, escalation rows and Matrix blocks where a full name will not fit. Note it is often — not always — the same string as the equipment maker prefix, and the tool must not assume it is. |
| **Region type** — **BASE / CONTINENT / NEUTRAL** | The registry's classification of a region: an army's home base, open continent, or neutral ground. About 12 regions registered. | [trackers] | Printed on the Session page. It is the honest prior for what kind of fight this is: a BASE session with a registered owning army has a **home side** (defence) and an attacker; a NEUTRAL region is where FFA, side-switching and uninvited randoms are expected, so the DEATHMATCH fallback and the "no single front" refusal are normal there, not degradations. |
| **Home side** | The army that owns the region being fought over — the defenders. | [trackers][reports] | A prior on the Session page and on the session type (a defence rather than a raid). **Replaces the design's coined "own side"** in the loadout-compliance surface, which is really "the side the reviewing officer belongs to" — two different things that must not share a word. |
| **Front** | Where the fighting line is; "fronts form, collapse, re-form". | [owner] | The Front face (unbent space), the contested surface, push arrows. Native and correct — keep. |
| **Team deathmatch / FFA / deathmatch** | The default game mode; objectives are rare and region-specific. | [owner][reports] | The `DEATHMATCH` state the structure index prints when it stops naming sides. |
| **Escalation / auth / tier** | The agreed gear level, negotiated OIC-to-OIC and raised through the fight: "low escalation", "max auth", "went to tier 3". The ladder runs prim throwers → explosives/crowd control → vehicles, mechs, drones, tanks. | [owner][reports][merits] | The escalation lane (first use per side per class), the R4 escalation row, the report line. The class sets come from the group's own armory ladder, not ours. |
| **Restriction** | A mid-fight negotiated ban agreed by IM: "please ask that basket grenades not be used". | [reports] | A Rule with a start time bound to an equipment family; later hits list as rule-breaking rather than merely suspicious. |
| **Last life / called it** | The OIC declares the final life, typically ~90 min in. Leaving before the call is itself an incident. | [reports] | The `⚑ last life` mark on the presence lane; departures after it are a listed finding; the duration line of the session summary. |
| **GG** | Closes the fight in chat. | [reports] | Vocabulary only; a possible session-end hint if the bridge ever relays it. |
| **Kill feed** | The Discord line per death: `<killer> <verb> <victim> with <weapon> from <distance> meters away`; `**` for an object/vehicle kill; self-kills happen. | [reports][ebcs] | Death article phrasing, distance-at-kill statistics per family, and the rule that object kills and self-kills are ordinary rows and never drawn as anomalies. |

### 1.3 Combat systems, equipment and rules

| Term | Meaning | Source | Where the tool uses it |
|---|---|---|---|
| **Combat 2.0 / LLCS** | The Linden combat system. All avatar damage appears in its log. | [c2] | The event store. `DAMAGE` / `DEATH` and their fields (`damage`, `initial`, `type`, `source`, `rezzer`, `owner`, `target`, `source_pos`, `modifications[]`) are used with their wiki names throughout. |
| **Damage types** — generic (0), piercing (8), electric, cold, explosive (102), crushing (103), **anti-armor (104)**, suffocation (105), **medical (100, negative = healing)**, **redeploy (106)** | The type int on every event. Generic dominates; creative weapons use exotic types; 106 is a respawn signal, not a hit. | [c2][owner] | Named by these words everywhere. Negative medical is **healing**; 106 is never counted as a hit. |
| **LBA** — Listen Based Armour | Chat-channel health for scripted objects (vehicles, deployables), invisible to the combat log. Anti-armor 104 is the Combat 2.0 replacement; adoption is slow and political. | [owner][rules] | The reason object health and object destruction print **unknown** *unless* an `OBJECT_DEATH` event arrives. Named on the Object page and in Appendix A. |
| **`OBJECT_DEATH`** | The custom combat-channel event some region systems emit when a scripted object is destroyed. The owner's own tracker already treats being its `owner` as making an agent an active combatant. | [trackers][ebcs] | **The named exception to "object death is unobservable."** When it arrives, the Object page states the destruction and cites the event and its sender keys; when it does not, the page still says **unknown** rather than inferring. It is an untrusted script event like any other: zero aggregate weight, sender object and owner keys shown. |
| **Vehicle** | The `OBJECT_ROOT` of a seated combatant. A root that moves is a vehicle; a static root is a mount or a seat. | [trackers][owner] | The definition used for vehicle detection, replacing the design's looser "object with seated avatars that moved". The bridge tick can carry `root` for seated agents at the cost of one `llGetObjectDetails` call. |
| **Region settings** — `agent_limit`, `allow_damage_adjust`, `restrict_combat_log`, `damage_throttle`, `damage_limit` | The `llGetEnv` block the owner's region board already reads and displays, alongside parcel flags at the landing point. | [trackers][sl] | The same block on the tool's Session page. Two of them are load-bearing for honesty: `restrict_combat_log` bounds what the tool could ever have seen, and `allow_damage_adjust` says whether the §5.14 adjustment surface can exist at all on this region. |
| **Deployable** | Something rezzed from a HUD onto the field: grenade, trap, mine, turret. | [owner][rules] | Equipment kind; deployable rules (concurrency, rez-to-first-shot, field of fire); the escalation ladder. |
| **Interceptor** | A deployable/projectile that shoots down incoming explosives; kill-radius >2 m explosives "must be interceptable". | [rules] | Rule text, per-region; the "no interceptor-defeating projectiles" candidate check. |
| **Primshooting / prim thrower** | Classic rezzed physical bullets with `llSetDamage`, dying on collision. The bottom rung of the escalation ladder. | [owner][rules] | The **projectile** delivery class; the travel-time window; ghost-projectile flights. |
| **Hitscan / raycast** | `llCastRay` + `llDamage` from the attachment, or a rezzed "damage prim" moved onto the victim. Rays originate at the **camera**, not the avatar. | [owner][rules] | The **hitscan** delivery class: no travel window, and the sightline test run from both the eye and a plausible camera position. |
| **Layer (1 / 2 / 3), Special, Vehicle** | The loadout system: combatants wear several weapons at once and switch without re-attaching; armory pages are organised by layer. | [owner][merits] | The loadout-over-time strip, one band per layer, matching the group's own documentation. "Their weapon" is a timeline, not a fact. |
| **Loadout** | What a combatant is carrying now. | [owner] | The loadout strip; the own-side compliance list. |
| **Wireframing** | Using viewer wireframe mode (or hitboxes, derender, bounding boxes) to see through geometry. Universally banned, frequently accused, never provable. The behavioural tell officers describe is **elevation** tracking through walls. | [owner][rules][reports] | The tracking-through-walls signal, with its confound list, its mandatory 15 s context strip, and the standing sentence that wireframe use cannot be observed. |
| **Sithacking / sit-hack** | Teleporting by sitting on a distant object. | [owner] | One of the named readings of a teleport with no DEATH — listed, never named as the cause. |
| **Haze** | **A written weapons rule term**: a cluster of rays around a central ray. Burst raycast weapons are allowed only *without* haze. | [rules] | Rule text and its candidate check. **This word is spoken for. See the "haze rule" row in Part 2.** |
| **Red spawn zone / safe zone / spawn hub / courtesy line** | The protected area around a spawn that must not be fired into or fought from; the courtesy line is its boundary. | [owner][rules] | The Zone noun, `d_spawn`, the spawn-zone standing query, the legitimate at-spawn immunity pattern. |
| **Spawn camping / spawn killing** | Fighting inside or into the spawn area. A standing officer complaint. | [reports][rules] | The `ZONE_EVENT` rule shape's worked example. |
| **Blindfire / flashbang** | Firing in the direction you were flashed from. Long flashbangs are "literally bait to make wireframing allegations". | [reports] | A named confound, promoted to a mandatory context strip on every awareness/tracking card. |
| **Flak, seeker, mortar, mech, drone, tank, gunship, riot shield, mine, turret** | The equipment classes rules and escalation ladders are written in. | [rules][reports] | Escalation classes; rule parameters; equipment classification. Groups configure their own class sets. |
| **Downed** | A Battlefield-style state written to the combat log where DEATH is not followed by a teleport until the combatant fails to be revived. | [owner] | Its own signal, separate from DEATH and from teleport; inverts the death-without-teleport anomaly rule on regions that run it. |
| **Redeploy** | Voluntary respawn; damage type 106. | [owner][c2] | Labelled as such, never counted as a kill. |
| **Experience** | The region's SL Experience: on-death teleport, safe zones, temp-attachment damage control. Leaving the experience or detaching the HUD mid-combat is an Incident. | [owner][ebcs] | The legitimate reading of an immunity pattern, resolved from `task_id`'s owner/creator; the death-without-teleport anomaly. |
| **EBCS** — Experience Based Combat System | **The common region combat system**: open source and running on a good many combat regions. It uses SL Experience permissions to teleport a combatant to their team's spawn hub on death, reset health, and enforce safe zones. This is the default regime the tool should assume when it has no better information. | [owner][ebcs] | The reference model for §5.7's death-and-teleport handling: DEATH followed within ~2 s by a teleport to a spawn hub is the *expected* shape, and a DEATH without one is the anomaly. Also the reason at-spawn immunity is read as legitimate first. Where a region announces itself, the tool takes what it declares; where it does not, it says `region system unknown` rather than assuming a regime. |
| **Region combat system** | The generic term for whatever platform runs combat on a region: EBCS is the common one, others exist and some are private to one region (§1.4). Systems may publish team configuration, spawn points, tracked agents and battlefield bounds on region chat channels, and some emit `OBJECT_DEATH`. | [owner][ebcs] | The optional high-confidence seed for teams, spawn Zones and the death regime, and the optional source of custom combat-channel events. Absent ⇒ `no-region-system`, and every inferred model carries on unchanged. The tool is written against the generic contract, never against one system. |
| **Sim / region** | The unit of ground. Combat and death teleports stay inside one. | [owner] | The session boundary. |
| **Rez / rezzer** | To create an object in-world; the object that did so. A combat-log field. | [sl][c2] | Equipment chains, deployable classification, volley keys. |

### 1.4 Group-specific examples (Ashguard) — **not shared language, and never in tool UI copy**

Everything above is vocabulary the SLMC broadly shares. Everything below is **one group's taxonomy and
lore**. It appears in this glossary only because the Ashguard material is the worked example the design
was built against — it is not the community's language, other groups have their own words for the same
things, and **none of it may be written into a label, a default, an enum name or generated prose.**
Where the tool needs one of these concepts it uses the generic word from §1.1–§1.3 and takes the
group's own name from the group registry or from per-group configuration.

| Ashguard term | What it is there | The generic word the tool uses |
|---|---|---|
| **Nammu** | T-1 trainee, before full membership | **recruit** / **trainee** |
| **Temple** | The council of officers that approves applications and votes on membership | **command** / **officers** |
| **Enlisted**, **E-1 … E-9** | The rank ladder for full members | **member** (the tool does not model rank ladders) |
| **Hireling**, **M-1** | The rank name for militia | **militia** (the group's militia *group* comes from the registry's `m` field) |
| **Besieger, Assassin, Defender, Pointman, Flagbearer, Specialist, Field Commander, Pillar** | The award categories promotion is judged against there | **combatant summary statistics**: the tool computes generic observables (raids attended, kills, K/D, multikill streaks with the window printed, time in contested Zones, anti-armor damage, deployments, melee and magic kills, OIC tenure) and labels rows only from group configuration. It ships no award scheme and no category names |
| **Motes**, and raid pay generally | The group's pay-per-raid and bonus scheme | nothing — **the tool does not model pay.** A bonus-worthy act is just a **notable episode** with its evidence; what a group pays for it is theirs |
| **The raid report** — one forum post per event | The group's report artefact and its whole shape | **session summary export**: a copyable text block whose *sections and field order are per-group configuration*. The tool ships no format |
| Post tags **Attack / Defense / Raid / Skirmish / Operation / Expedition / Interception / FFA / Paid** | That forum's tag list | **session type**, a configurable list per group |
| **Personnel**, and the **(Militia)** block | The report's member list and its militia split | **combatant list**, with militia shown separately because militia is shared language (§1.1) — but the block name, order and formatting are configuration |
| **Incidents** | The report's free-text section for equipment disputes, rule breaches, conduct and system faults | **findings** (§ Part 3): candidate rows an officer weighed and kept, each with its evidence links. The domain fact behind it stays true and is worth designing for — *"was this equipment doing what it should"* is the commonest thing officers actually investigate |
| **"OIC HUD Output"** — `agent-uuid,minutes-active` lines, rotation-change minutes in 5-minute windows | That group's HUD protocol, reproduced byte-compatibly so posts stay comparable | **combatant summary statistics**: the tool computes generic per-combatant activity (engaged minutes, active-combatant seconds) and can emit a group's legacy protocol as a *configured* extra column or block, never as the shipped default |
| **Commendation** | The group's citation for valour in a report | **notable episode** — an outnumbered hold, time on a contested objective — offered as a sentence with a link and never as a score |
| **Merit System**, **Merit Sheet** | The group's promotion scheme and its paperwork | **combatant summary statistics** plus **notable episodes**; the tool computes observables and ships no award scheme |
| **Hireling Militia**, **Ashguard - Civilians**, **Ashguard Land** | The army's militia, civilians and land groups | resolved from the registry's `m` / `c` / `l` fields; the tool prints whatever name the registry holds |
| **Artificery** | The group's equipment-making and balance-discussion practice and channel | nothing — the tool has no word for it. The domain fact it stands for is shared and stays: groups build their own arsenals, and balance disputes are settled OIC to OIC, which is why **equipment** pages and **restrictions** exist |
| **FLECS** | **The owner's own combat system**, currently running on Resdayn. It publishes team configuration `{name, color}[]` and per-agent assignments on a teams channel, spawn points on a respawns channel, tracked agents on a tracker channel, and gamemode plus battlefield bounds on a meta channel | **region combat system** (§1.3). FLECS is *one region's* system and is named in the design only as a worked example of the generic contract; **EBCS** is the common case and the default the tool assumes. Nothing in the tool may require FLECS, and no FLECS channel name or message shape may be hard-coded outside a per-region parser |
| **Resdayn** | The group's home region | the region name from the registry, with its type (BASE / CONTINENT / NEUTRAL) |
| **Tier 2 / tier 3 armory auth** | This group's escalation ladder | **escalation class**, with the class sets configured per group from their own armory pages |
| **The Ashguard Combat Rules** | The worked rule catalogue in Appendix A | **rule catalogue**, per region, editable, shipped as a named example rather than as the tool's opinion |

The same rule applies to the two slang terms in §1.1–§1.2 that are Ashguard-flavoured rather than
universal: the printable community word for uninvited third parties is **randoms**, and *artificery* is
that group's channel name, not a concept the UI needs.

---

## Part 2 — Tool vocabulary audit

**Every proposal below needs owner approval.** Nothing here is a change already made; this is the list
of words to decide about before Phase 3 writes them into XUI labels and Phase 5 writes them into struct
names.

Origin column: **N** = SLMC-native, **P** = plain English used plainly, **C** = coinage (ours).

### 2.1 Nouns — the pages an officer can open

| # | Term | What it means in the design | Origin | Problem | Proposal | Rec. |
|---|---|---|---|---|---|---|
| 1 | **Session** | One raid in one region, start to end | P | Fine. Officers say "the raid" | Title the page **Session** but let the lede and the summary export say **raid** | keep |
| 2 | **Phase** | A session-scale segment split at lulls | P/N | None. "Phases" reads naturally beside "escalation" | — | keep |
| 3 | **Bout** | A spatiotemporal cluster of fighting | C | "Bout" is boxing, not SLMC. Officers say *engagement* ("between engagements", "n = 6 engagements" appears in our own doc) | **Engagement** — plain, military, already in the owner's vocabulary. **Not "Skirmish"**: that is a post tag for a whole event | rename |
| 4 | **Moment** | An instant `t ± 1 s` | P | None | — | keep |
| 5 | **Life** | Spawn → death | N | Native: "last life", "healing limited per life" | — | keep |
| 6 | **Avatar** | Used as both the SL entity and the person page's title | P/[sl] | **The owner's core complaint.** "Avatar" is the SL platform's word for a body; the person on the battlefield is a **Combatant** | Reserve **Avatar** for the technical layer (the agent, the UUID, the body being tracked). Title the person page **Combatant**, and **Civilian** otherwise, split by the community's own active-combatant test [trackers] rather than by a definition of ours: owner of a DAMAGE/DEATH/`OBJECT_DEATH`, or mouselook/seated in a combat area, expiring 2 min after last damage. Headline counts read "41 present — 36 combatants, 5 civilians", never "41 agents" | rename |
| 7 | **Team** | An inferred (or region-declared) fighting group | N | None — the owner's own word | Keep. Use **Side** in prose where the thing is being spoken about rather than computed | keep |
| 8 | **Band** | (a) a transient 2–5 person local alliance; (b) in §5.9/§5.9a, *the inferred team itself* ("the band's internal pair weight", "avatars from ≥ 2 bands"); (c) the ordinal confidence chip; (d) a drawn horizontal strip ("militia band", "structure band") | C | **Four incompatible senses of one word inside one document.** This is the single worst collision in the design | (a) → **Squad** [SLMC-native, matches "moving with the squad"]. (b) → **Team**, everywhere, with no exceptions. (c) → keep the word only as **confidence band**, never bare. (d) → **row** / **lane** / **strip** ("militia row", "structure lane") | rename |
| 9 | **Death** | The `DEATH` event | N/[c2] | None | — | keep |
| 10 | **Damage** | The `DAMAGE` event | N/[c2] | None | — | keep |
| 11 | **Volley** | Consecutive DAMAGE sharing `(owner, rezzer, target)` with ≤ 2 s gaps — "he shot him" | C/P | Considered: **Burst** collides with the `BURST` rule shape and with "burst raycast weapons" [rules]; **Exchange** is two-way and would be wrong for a one-directional run of hits | Keep **Volley**. It is one-directional, plainly military, and unclaimed by the rule sets | keep |
| 12 | **Equipment** | Weapon / projectile / HUD / deployable / vehicle / mount | N | None; the handbook's own word | — | keep |
| 13 | **Model** | An equipment family keyed across instances | C | "Model" reads like a car model or a 3D mesh model — in a viewer, badly so. The doc itself already calls this a **family** in nine places ("the equipment ledger keys on family") | **Family** (page title: the maker prefix + stem, e.g. `[Ash] Kagrenac bolt-caster`) | rename |
| 14 | **Object** | A non-agent damage target: turret, vehicle, deployable | P/[sl]/N | In SL *everything* is an object, including every weapon and bullet on the page beside it. An earlier draft of this glossary proposed **Materiel**; that is **withdrawn**, because the community's own custom event for exactly this thing is `OBJECT_DEATH` [trackers], and a native word beats a tidier coinage | Keep **Object**, always qualified on first use as a **damageable object**, and let the page state its kind (Turret / Vehicle / Deployable / Mount) in the title. Never bare "Object" in a list header where SL objects are also listed | keep (qualified) |
| 15 | **Script** | A damage-adjustment script from `modifications[]` | N/[sl] | Every object has scripts; unqualified it means nothing | Always **Adjustment script**; page title `Adjustment script — Ward-7` | rename |
| 16 | **Zone** | A named 16 m spatial cell | N/C | The SLMC word **zone** means a named area with rules attached (red spawn zone, safe zone) — not a grid square. The design overloads it onto both | Split: **Zone** = an officer-named or region-declared area (spawn, gate, courtyard, roof) — SLMC-native. **Cell** = the 16 m grid square that always has a page (`ss:cell/12,-4`, "Cell 12,−4") | rename |
| 17 | **Verdict** | A stored line-of-sight result, and the strip's headline word ("verdict: MIXED") | C | Directly contradicts the tool's own rule that it never issues verdicts. In a pasted report, "verdict" reads as a judgement about a person even when it is a statement about geometry | **Sightline** as the noun (page: "Sightline — Vex ← Cadmus"), with the outcome stated as **CLEAR / MARGINAL / BLOCKED / UNKNOWN** and no summary word above it. Delete "verdict" from the vocabulary entirely | rename |
| 18 | **Signal** | The documentation page for one behavioural signal | C/P | Mild: "signal" can read as comms. But it is honest about being an indicator rather than a fact | Keep, always as **behaviour signal** on first use in any page | keep |
| 19 | **Claim** | An officer assertion (flag / clear / override / note) | C | Legalistic, and doubly used: Rules are also called "claims" in §2.1 | Keep **Claim** for officer assertions with their four polarities. **Stop calling Rules claims** — a Rule is a Rule | keep |
| 20 | **Rule** | One entry of the region's rule catalogue | N | None | — | keep |
| 21 | **Report** | The page and export that fills the group's raid-report artefact field for field | N (one group's) | The artefact is **one group's format**, not shared practice — every SLMC group runs its own processes, and a tool that ships a forum-post shape ships that group's paperwork. "Report" is also badly overloaded in plain English (a bug report, reporting a violation, the OIC HUD's own output) | **Session summary**, whose *sections and field order are per-group configuration* held beside the rule catalogue. The tool computes duration, the combatant list with its militia split, per-combatant statistics, the escalation timeline, findings and notable episodes, and pours them into whatever shape the group configured. Ship no format, and no group's format as a default | rename |
| 22 | **Case** | Pins + notes forming a dossier | C/P | Acceptable, but "Case Binder" (from [03]) adds a word that does nothing | Keep **Case**, with three generically-named templates — **Session summary**, **Combatant review**, **Investigation** — none of which is a group's own paperwork name. Drop "Binder" | keep |
| 23 | **Trail** | A named visit sequence (navigation history frozen as an artefact) | C | **Hard collision**: "trail" is also the drawn movement path in the overlay, the trail-window slider, and "trails instead of moving points" — the Bret Victor phrase the whole design rests on | Keep **Trail** for the movement path. Rename the navigation artefact to **Path** (or fold it into **Case**, which is what officers actually export) | rename |
| 24 | **Record** | The lowest level's subject: one received line, raw | P | Generic; "record" also means "to record the session", which the ●REC chrome does two lines away | **Log line** for the noun; the *level* is named **Raw** (“Raw — the log lines behind this”). The raw arena stores log lines) | rename |

### 2.2 Levels, faces and renderers

| # | Term | What it means | Origin | Problem | Proposal | Rec. |
|---|---|---|---|---|---|---|
| 25 | **Ladder** / **Rung** | The vertical abstraction axis and its six positions | C (Bret Victor, *Up and Down the Ladder of Abstraction*) | **Design-theory vocabulary, not the officer's.** It is borrowed from an essay to explain *why* the tool is shaped this way; it is not casual language and nobody opening the floater has it. Unknown-on-first-open is not a discoverability problem to be papered over with a Legend — it is a sign the word belongs in the rationale, not on the screen | UI word is **level**: *zoom out a level*, *zoom in a level*, the **level bar**, the **level keys**. Copy names the level rather than numbering it — **Raw, Moment, Engagement, Sweep, Comparison, Session**. "Ladder of Abstraction" survives **only** as the cited design principle in the thesis and the rationale docs, never in a label, tooltip, breadcrumb or generated sentence | rename |
| 25a | **step up** / **step down** | Moving one position along the abstraction axis | C (same essay) | Same problem, and worse in copy: "step down to the concrete" tells an officer nothing about what will appear | **zoom out a level** / **zoom in a level**, and the rail prints the destination by name ("zoom in to Moment 14:31:07.4"). The `[` and `]` keys become the **level keys** | rename |
| 26 | **Ledger** | Three different things: R1 Face A (one row per combatant over time), the R4 summary tables, and the claim audit list | C | Three senses | R1 Face A → **Timeline**. R4 tables → keep **Ledger**. Claim audit list → **Claim log** | rename |
| 27 | **Plan** | R1 Face B: the top-down view (time collapsed) | C | "Plan" in military usage is a plan of action, and "the plan" in this project is the engineering plan | **Map**. [04] already called its equivalent the War Map | rename |
| 28 | **Chart** | The 2D orthographic renderer that twins the world overlay | C | If Plan → Map, "Chart" is redundant; and "chart" elsewhere means a graph | Faces become **Timeline** and **Map**; Map has two renderers, **In-world** and **Overhead**. Drop "Chart" | rename |
| 29 | **Sweep** | R2: one claim swept across one parameter | C/P | Plain enough, and the verb ("sweep this", "sweepable") is already load-bearing | Keep | keep |
| 30 | **Field** | R3: a set across two or more dimensions | C | **Collides three ways** with SLMC usage: the battlefield ("mid-field while dealing damage"), *field of fire* (a written turret rule), and *Field Commander* (a merit category) | **Comparison** (R3 Comparison), which is what the rung actually is | rename |
| 31 | **Matrix** | Who damaged whom, as a grid | P | Fine; label it "who damaged whom" in the header | — | keep |
| 32 | **Braid** | Allegiance over time, one strand per person | C | Nobody will guess it | **Allegiance** (face name), rows called **strands** in the legend only | rename |
| 33 | **Front** | Unbent space: signed distance to the contested surface | N | None — the owner's own word, used correctly | — | keep |
| 34 | **Roster** | The behaviour-signal filter face | N (misused) | **Roster** in SLMC means the membership list — groups keep one in a spreadsheet, and every event write-up has a combatant list. Using it for a behaviour-signal filter takes an innocent word and attaches it to the most misuse-prone surface in the tool | Rename the face to **Signals** (or **Behaviour**). Reserve **Roster** for the actual list of members on the Team page and in the Report | rename |
| 35 | **Board** | The small-multiples grid, one cell per pin | C | Ambiguous (message board, officers' board) | **Compare** | rename |
| 36 | **Ground** | The R4 face: the whole-session top-down view | C | Another word for the same picture as Plan/Chart, and it collides with "on the ground" (the design's own metaphor for the lowest levels) | Fold into **Map**: one word for the top-down view at every rung | drop |
| 37 | **Stage** | The swapping centre panel of the main floater | C | Fine as an implementation word; must never appear in a label | Keep, internal only | keep |
| 38 | **Article** | A wiki page for one noun | C | Officers do not say "article". "Page" is what the design itself calls it in §2 | **Page**, and the floater is the **Inspector** (its registered name already) | rename |
| 39 | **Rung Row** | The six `[-1]…[4]` buttons on every page | C | Carries the essay word, and "Row" reads like a table row. An earlier draft of this glossary proposed "Rung bar"; that is **withdrawn** — it keeps the borrowed word | **Level bar**, its buttons labelled with the level names (Raw, Moment, Engagement, Sweep, Comparison, Session) rather than with numbers | rename |
| 40 | **Ribbon** | The permanent bottom strip: deaths, damage, bouts, escalation, presence, structure, quality | C | Fine as chrome; the lanes inside it carry the meaning | Keep, and always name the lane rather than the ribbon in copy ("the escalation lane") | keep |

### 2.3 Navigation and mechanics

| # | Term | What it means | Origin | Problem | Proposal | Rec. |
|---|---|---|---|---|---|---|
| 41 | **anchor** | The concrete instance last visited beneath a subject, written by `]` and read by `[` | C | Used for three unrelated things: this; the "anchor sweep" (re-running a verdict at t ± 1 s); and "0.5 Hz anchors" in the Exposure sampler; plus "3D-anchored labels" in the overlay | Keep **anchor** for the navigation memory only. "Anchor sweep" → **timestamp sweep** (it sweeps the event's timestamp). "0.5 Hz anchors" → **sample points** | keep + fix collisions |
| 42 | **witness** | The medoid instance of an aggregate, landed on when entering cold | C | **Collides with the domain's evidentiary vocabulary**: "evidence today is eyewitness plus an OIC HUD" [owner]. An officer reading "(witness)" in the rung rail will read *someone who saw it* | **Example** — the rail prints `(back)` or `(example)`. Related: the "wall witness" disc in §4.2 → **wall hit marker** | rename |
| 43 | **Peek** (mode) | The five-second entry depth for a fighting OIC | N | The owner's own word: "short stationary peeks between engagements" | Keep | keep |
| 44 | **Study** (mode) | The full depth of levels for a parked officer | P | Fine | Keep | keep |
| 45 | **Peek card** | The transient hover card over a link or marker | C | Collides with Peek mode; the design already notes this and forbids the bare word | **Hover card**, and the collision disappears | rename |
| 46 | **Pin** | Drop a noun into the tray / into a Case | P | None | — | keep |
| 47 | **View** (the struct) | `(level, subject, face, renderer, cursor, window, params, filter, selection)` — what a breadcrumb chip, a pin, a compare slot and a marked chapter all are | C | Fine in code; in UI copy "view" is too vague to mean this precise thing | Keep as the code name. In UI say **framing** ("back to the framing you started from") or name the concrete artefact (chapter, pin, chip) | keep (code) |
| 48 | **ParamSet** | The single global set of tunable parameters | C (identifier) | An engineering identifier leaking into the UI strip | UI word: **Parameters** (the strip is already labelled `params:`). Code keeps `ParamSet` | rename (UI) |
| 49 | **degraded strip** | The chrome line printing `no-respawn`, `prox-capped`, `no-front` | C | "Degraded" is engineering; an officer reads it as *something is broken* | **Not available** strip: `not available: respawn zones · single front`. Keep the chip codes in tooltips | rename |

### 2.4 Uncertainty and analysis vocabulary

| # | Term | What it means | Origin | Problem | Proposal | Rec. |
|---|---|---|---|---|---|---|
| 50 | **haze rule** | The invariant that no aggregate is drawn without the unknown fraction it was built from | C | **Direct clash with a written sim rule.** "Haze" in the Ashguard weapons rules is a cluster of rays around a central ray, and burst raycast weapons are allowed *only without haze*. Two meanings of "haze", one of them checkable, is indefensible | **The unknown-fraction rule** (or "no aggregate without its unknowns"). Leave "haze" entirely to the rule catalogue | rename |
| 51 | **STRAINED nn%** | The share of a team's internal pair weight that is hostile | C | Reads like a diagnosis, and it is the accusation-shaped fact that already worries the owner (§8 Q4) | **internal fire nn%** — plainly names what it measures, using friendly fire's own vocabulary | rename |
| 52 | **tag-dependence (!)** | The flag lit when the tag-seeded and tag-free solves disagree | C | Precise, but compressed | Keep the mechanism; label it in words: **depends on group tags (!)** | keep (reword) |
| 53 | **flip point** | The parameter value at which a sightline result changes | C | Self-explanatory in place ("BLOCKED for travel < 0.9 s; CLEAR at or beyond it") | Keep, defined on first use | keep |
| 54 | **flip fraction** | The share of swept values that come out CLEAR | C | Not self-explanatory, and it is the *headline* number — the one most likely to be quoted out of context | Print the sentence instead of the coinage: **"clear for 5 of 13 tested travel times"**. Keep `flipFraction` in code | rename |
| 55 | **FIRM / LIKELY / WEAK / UNDETERMINED** | The ordinal confidence bands for anything from clustering or classification | C | Two problems. (a) The four words are not stated consistently across the material — "APPROX" appears in briefings but nowhere in the design, which uses WEAK. (b) **FIRM is also used as a sightline robustness state** ("agreeing → FIRM, disagreeing → MARGINAL"; "over FIRM verdicts only") | Fix the ladder to exactly four words: **FIRM / LIKELY / WEAK / UNDETERMINED**, and never introduce a fifth. Rename the sightline robustness state to **STABLE** vs **MARGINAL**, freeing FIRM for confidence only | rename |
| 56 | **MARGINAL** | A sightline result that flips under the position error jitter | P | None | — | keep |
| 57 | **CLEAR / BLOCKED / UNKNOWN** | Sightline outcomes | P | None | — | keep |
| 58 | **drift / noise / short** | The three distinct outcomes of allegiance over a window | P/N | "Drift" is native ("allegiances may drift"); the other two are plain | Keep; print them as words with their meaning in the legend | keep |
| 59 | **ff-unstable** | The chip when the two friendly-fire passes disagree on > 10 % of members | C (identifier) | A code identifier in a label | **friendly-fire unstable** | rename |
| 60 | **structure index (S, S_null)** | Sign-flipped Newman modularity vs a null model — how team-shaped the fight is | C | Statistical jargon; correctly confined to the why-page already | Keep as a why-page term. On the ribbon the lane is labelled **how team-shaped** and prints `DEATHMATCH` when it collapses | keep |
| 61 | **DEATHMATCH** | The state where the tool stops naming sides | N | Native | — | keep |
| 62 | **contested** | A Zone with ≥ 2 sides simultaneously inside it and damage occurring | N/P | Native and defined once. Good | — | keep |
| 63 | **awareness lead** | Seconds a shooter tracked an occluded target before the first hit | C | "Awareness" is vague; officers describe this as *tracking* (wireframing "shows behaviourally as locking onto a specific person") | **tracking lead**, keeping the confound list and context strip on it | rename |
| 64 | **reaction interval** | First-clear-sightline to first hit, as a percentile of the session | P | None | — | keep |
| 65 | **Contact** | Fraction of a life with a hostile in range and in the frontal arc, geometry not tested | N/P | Native military ("contact"). Fine, but must always carry "geometry not tested" | Keep | keep |
| 66 | **Exposure** | Raycast-confirmed clear sightline to ≥ 1 hostile, on demand | P | None | Keep | keep |
| 67 | **TRACK / LAST-SEEN / APPROX-VIA-OWNER** | The three attacker-position quality tiers | C | The third is an identifier, not a phrase | **tracked / last seen / owner's position (estimate)** | rename |
| 68 | **UNATTRIBUTED** | The permanent row for everything that could not be attributed | P | None; its permanence is the point | — | keep |
| 69 | **delivery class** — hitscan / projectile / lobbed-arc / area | How the damage was delivered | N | Native to the owner's own description of delivery methods | — | keep |
| 70 | **escalation class** | The sub-class (mech, drone, staff, tank) driving the escalation lane, separate from `kind` | N | Native root, and correctly separated from the equipment `kind` enum | — | keep |
| 71 | **restriction** | A Rule with a start time bound to an equipment family | N | Native | — | keep |
| 72 | **coverage** | Fraction of a window with a position sample within 1 s | P | Fine, but it also reads as "rule coverage" in Appendix A | Keep for tracks; in the rule catalogue say **measurability**, never "coverage" | keep |
| 73 | **measurable / partial / annotate** | The measurability class on every Rule | C/P | Clear and load-bearing | Keep, spelled out on every row | keep |

### 2.5 Workflow artefacts and imported terms not adopted

| # | Term | Where from | Problem | Proposal | Rec. |
|---|---|---|---|---|---|
| 74 | **Hotspot** | [03] — an auto-detected, unnamed death cluster | Overlaps exactly with Bout/Engagement and with Zone; the synthesis already dropped it | Do not reintroduce. An unnamed cluster of fighting is an **Engagement**; a piece of ground is a **Zone** or a **Cell** | drop |
| 75 | **Place** | [03] — an officer-named zone | Same concept as Zone, and **Zone** is the SLMC word (red spawn zone, safe zone) | Drop; use **Zone** | drop |
| 76 | **Encounter** | [03] — a manually brushed space+time region | Collides with Engagement (proposed for Bout) and with "between engagements" | Drop. If a manual selection artefact is wanted, call it a **Selection** and let the officer name it as a Zone + window | drop |
| 77 | **Case Binder** | [03] | "Binder" adds nothing; the container is the Case | Drop the word; keep the three templates | drop |
| 78 | **Evidence Card** | [03] — a frozen snapshot pinned into a Case | "Card" collides with the hover card; the synthesis already uses **pin** for the gesture | **Pinned evidence** (the gesture is **Pin**, the item is **evidence**) | rename |
| 79 | **beats** | Journalism — the suggested report sentences | No officer will read "beats offered" and know what it means | **Suggested lines** (each inserts one sentence plus a link) | rename |
| 80 | **Dossier** | [03][04] — the per-noun stat page | Superseded by Page/Inspector | Drop | drop |
| 81 | **War Map** | [04] | Superseded by **Map** | Drop the "War" | drop |
| 82 | **Kit** | [02][08] — an equipment profile | Collides with the community's "kit" (MRCG kit, weapon kit = the whole product) and with **Family** | Drop; use **Family** | drop |
| 83 | **Duel** | [02][04] — a mutual-damage pair | Actually useful in FFA, where the design already reaches for **Pair**. But "duel" implies agreement to fight | Keep the concept, call it a **Pair** (as the FFA walkthrough already does) | rename |
| 84 | **Minute** | [02][03] — a page for one minute of the session | Superseded by Moment + Engagement; a minute is not a thing officers name | Drop | drop |
| 85 | **Side** | [03] — used as the noun where the synthesis uses Team | Not a collision, a register difference | Keep both: **Team** is the computed object, **Side** is how prose refers to it | keep |
| 86 | **Squad** | [03][merits] — a co-movement cluster inside a side | Currently unused by the synthesis, which uses **Band** for a similar concept | **Adopt Squad as the replacement for Band (a)**. It is the word the promotion criteria are written in | adopt |
| 87 | **combat cuck** | [reports] — uninvited third parties | Native but unprintable in a tool label | Never in UI copy. The officer-facing word is **randoms** or **uninvited** | drop (from UI) |
| 88 | **bystander** | [owner], used in the §5.14 immunity patterns | The owner has now fixed **Civilian** as the SLMC word for non-combatants | Replace "immune bystander" with **immune civilian** throughout §5.14 and its Signal page | rename |
| 89 | **agent / agents** | [sl], used in headline counts ("41 agents", "≤ 100 agents") | The owner's complaint exactly: a platform word standing in for people | **Combatant** in all UI copy (and **active combatant** where the count is the live one); "agent" survives only where it names an SL API concept (`llGetAgentInfo`, `AGENT_MOUSELOOK`, agent UUID) | rename |

### 2.6 Where the owner's trackers already have the word

Six places where the design coined or paraphrased something the owner's own boards and registry already
name. In each, **prefer the native term** — the point is that the tool's counts and the group's boards
agree, and that `slmc-data` drops in unchanged.

| # | Design term | What it means | Native term | Why | Rec. |
|---|---|---|---|---|---|
| 90 | **"seated avatars" / "rider" / "the seated avatar's offset"** | Agents sitting on a vehicle or mount | **Crew** | The tracker maintains crew membership per vehicle and drops the vehicle when the last crew member leaves; the LBA rules already price "extra crew". "Rider" is ours and nobody else's | rename |
| 91 | **"object with seated avatars that moved"** | The vehicle classification test | **Vehicle** = the `OBJECT_ROOT` of a seated combatant; a root that **moves** is a vehicle, a **static** root is a mount or a seat | The tracker's test is sharper than ours and is already implemented; it also gives the bridge a one-call source for `root` | rename |
| 92 | **"own side"** (own-side loadout compliance, §5.24) | The reviewing officer's own group | **Home side** is the *defending army on its own region* — a different thing, and the native one | Two distinct concepts sharing one phrase. Use **home side** for the region's owning army (a Session-page prior, from region type BASE + owning group), and say **your own army** for the compliance list, which is about the reviewer, not the ground | rename |
| 93 | **"tracked-and-armed seconds"** (§5.9's per-second normalisation) | The denominator that judges a late arrival on rate, not volume | **Active-combatant seconds** | The community already defines exactly this state, expiry rule included; using our own near-miss definition would make our rates disagree with the board's | rename |
| 94 | **"neutral sim"** (the FFA walkthrough) | A free-for-all region | **NEUTRAL region** (registry type) | It is a registry field, not a description. It also makes the FFA behaviours — DEATHMATCH, no single front, side-switching, randoms — *expected on a NEUTRAL region* rather than degradations | rename |
| 95 | **"team colours seeded from contact sets / group colours / netmap marks"**, and unnamed factions generally | Where a side's name and colour come from | **Army** (`{g, m?, c?, l?, n, s}`) | The registry already maps four group ids to one name and one shorthand. A Team that matches an Army gets its name, shorthand and colour from there; only unmatched clusters stay unnamed. The officer's netmap/contact-set colours remain the fallback | rename |

**Terms audited: 96.**

---

## Part 3 — Naming policy

One page. It decides the next word without another audit.

### Where nouns come from, in order

0. **If the owner's own boards or registry already define it in code, use that definition verbatim** —
   not a paraphrase, not a near-miss of our own. Active combatant (with its 2-minute expiry and its
   combat-area test), crew, vehicle (`OBJECT_ROOT` of a seated combatant), `OBJECT_DEATH`, army,
   militia/civilians/land groups, shorthand, region type BASE / CONTINENT / NEUTRAL, home side. The
   test is not "is our definition better" — it is "do our numbers match the board the group already
   reads, and does `slmc-data` drop in unchanged".
1. **Otherwise, if the SLMC broadly shares a word, use theirs, spelled and scoped exactly as they use
   it.** OIC, militia, escalation, tier, auth, last life, loadout, layer,
   deployable, LBA, front, zone, restriction, squad, roster, personnel, raid, skirmish (an event type
   only). Do not extend a native word to cover something adjacent — that is how "Roster" ends up naming
   a behaviour filter and "haze" ends up naming an uncertainty invariant.
2. **A term only one group uses is lore, not shared language, and never appears in tool UI copy.**
   Rank names, council names, award names, pay-scheme names, equipment-culture names, group and region
   names, and one region's own combat system are that group's taxonomy (§1.4). The tool ships the
   **generic** word — recruit/trainee, command/officers, member, militia, session summary, combatant
   summary statistics, notable episodes, region combat system — and takes the group's own name from the
   **group registry** or from per-group configuration at run time. A group name hard-coded in a label,
   an enum, a default catalogue title or generated prose is a bug: it makes the tool one group's tool.
   The test is simple — *would this word be wrong on another group's screen?*
3. **Report formats, award schemes and pay are per-group configuration the tool never ships.** Every
   SLMC group runs its own processes: its own write-up shape and section names, its own promotion
   categories, its own pay and bonuses, its own event-type list. The tool computes the observables —
   duration, the combatant list with its militia split, per-combatant activity, the escalation
   timeline, findings, notable episodes — and pours them into whatever shape a group configured. It
   ships **no** default format, **no** award category names and **no** pay model, and it never treats
   one group's paperwork as the shape of the problem.
4. **Design-theory vocabulary stays in the rationale docs and never reaches the UI.** The tool is
   built on Bret Victor's *Up and Down the Ladder of Abstraction*, and that is worth citing — in the
   thesis, in this glossary, in a design note. It is not the officer's language and it is not casual
   language, so **ladder**, **rung**, **step up** and **step down** do not appear in a label, a
   tooltip, a breadcrumb, a key legend or a generated sentence. The UI word is **level**: *zoom out a
   level*, *zoom in a level*, the **level bar**, the **level keys**, and copy names the level — Raw,
   Moment, Engagement, Sweep, Comparison, Session — rather than numbering it or describing the motion
   in the essay's terms. The same rule governs any other borrowed theory word: it explains the design
   to us, it does not name anything to them.
5. **Otherwise use plain English an officer already uses in the sentence.** Map, Timeline, Comparison,
   Page, Family, Engagement, Pair, Moment, Life, Sweep.
6. **Coin only for a genuinely new concept**, and define it on first use in the UI, not only in the
   Legend. The coinages that survive this policy are: **level**, the **level keys** `[` and `]`, **anchor**,
   **example**, **Volley**, **Sweep**, **flip point**, **Signal**, **Claim**, **Case**, **Pin**,
   **Peek / Study**, **the unknown-fraction rule**. Each is a thing that does not exist outside this
   tool. Everything else in the tool must be a native or plain word.
7. **One word, one meaning, tool-wide.** If a word already means something in the tool, in the SLMC,
   or in Second Life, it is taken. Check all three before coining.

### Form

- **Page and noun names are singular and capitalised**: Session, Phase, Engagement, Moment, Life,
  Combatant, Civilian, Army, Team, Squad, Crew, Death, Damage, Volley, Equipment, Family, Object,
  Vehicle, Adjustment script, Zone, Cell, Sightline, Signal, Claim, Rule, Case, Pair, Log line, Session
  summary.
- **Faces, lanes and modes are lower-case in prose and Title Case as labels**: the escalation lane,
  the Map face, Peek depth.
- **Code identifiers may keep their engineering form** (`ParamSet`, `flipFraction`, `escalClass`,
  `LosResult`) as long as no label, tooltip, article body or exported text shows them.
- **A level is named for what it holds**, not numbered and not for its rendering: **Raw, Moment,
  Engagement, Sweep, Comparison, Session**. Copy says "zoom in a level" and names the destination;
  it never says rung, ladder, step up or step down.

### People

- **UI copy never says "avatar" when it means a combatant.** "Avatar" names the SL entity being
  tracked (a body, a UUID, a position sample). "Combatant" names the person in the fight. Headline
  counts read "36 combatants, 5 civilians", never "41 agents".
- **The Combatant/Civilian split is the community's test, not ours** [trackers]: owner of a `DAMAGE`,
  `DEATH` or `OBJECT_DEATH`, or in mouselook or seated on an object while in a combat area (a
  damage-enabled parcel); held while mouselook or seated, expiring 2 minutes after the last damage
  activity, dropped on leaving the region. Any count the tool prints of "combatants" is this test, so
  the tool and the group's own active-combatants board never disagree.
- **Civilian is the word for a present non-combatant.** Civilians are counted separately, never inside
  a side's headcount, never in the combatant list, never in K/D, and the legitimate at-spawn immunity pattern is
  described as protecting *civilians*. A registry civilians-group tag is a civilian signal and is never
  clustered as a side.
- **Militia is checked before it is inferred.** An army's militia group is in the registry; someone
  wearing it is militia as a fact. Only where no militia tag is worn is it inferred, and then it is
  printed with its confidence band, with the check-before-posting warning, and demotable to *randoms*
  in one click.
- **Crew, not "seated avatars".** People on a vehicle are its crew, and the vehicle is the root object
  they are seated on.
- **Rank is never assigned, and rank names are never shipped.** The tool models no rank ladder. Where a
  person's standing matters it says *member*, *militia*, *recruit* or *civilian*; a group's own rank
  and council names (§1.4) appear only where the registry supplied them or an officer typed them.
- **Per-combatant summary rows are labelled from group configuration.** The tool computes the
  observables and ships no category names of its own.

### Judgement words

- **No verdict words anywhere.** Banned in labels, article prose, tooltips, exported text and code that
  produces any of them: *cheater, guilty, innocent, violation, offender, suspect (as a noun), proven,
  caught, verdict*. This includes "verdict" as the name of a sightline result — see Part 2 #17.
- **The two words a rule check may produce are "finding" and "candidate."**
  - A **candidate** is a row a rule's predicate matched: "7 candidates", never "7 violations". It links
    to its instance, carries its uncertainty, and prints the rule text above the list.
  - A **finding** is what an officer takes away after weighing candidates: it is theirs, filed as a
    Claim with their initials, and the tool never authors one.
- **Behaviour surfaces are filters, not ranks.** No summed score, no leaderboard, no red cells, no
  ordering on thin samples. Every column carries its own n and links to its behaviour Signal page with
  the confound list printed.
- **An empty result is never a clearance.** Under an ANNOTATE rule the page says "not observable from
  the combat log"; under a self-reported signal the page says silence proves nothing.
- **Private by default.** Nothing about a person's behaviour is broadcast, chatted or written to the
  region. The only shareable artefact is the session summary export, containing prose the officer wrote.

### Uncertainty words

Four words, one ladder, used for anything from clustering or classification. Never a fifth, never a
decimal in a lede:

| Word | Means |
|---|---|
| **FIRM** | The evidence separates this answer from the alternatives by more than its own error. Quote it. |
| **LIKELY** | The best answer, but the runner-up is inside the error. Say so when you quote it. |
| **WEAK** | Distinguishable from nothing much; shown so the officer can see the tool tried. |
| **UNDETERMINED** | Not enough evidence to rank the answers at all. Distinct from "checked, nothing found". |

And the plain-language words for everything else:

- **unknown** — the data does not contain it (object health under LBA, another viewer's settings, shots
  that missed). Printed as a dim `—` plus the reason. **Unknown is not zero**, and an unknown that a
  region system can lift — object destruction, once an `OBJECT_DEATH` arrives — says which event would
  have answered it rather than implying nothing could.
- **not observable** — a stronger unknown attached to a rule: the check cannot be run, so an empty
  result is not compliance.
- **approximate** — a measured value carrying a stated error. Always printed as an interval with n:
  `67% [21–94%] · n=3`. **Never a bare rate.**
- **insufficient** — measured, but below the sample floor (n ≈ 8). Shown, never ordered on, never a
  denominator.
- **likely / firm** — reserved for the ordinal ladder above. They never describe a measurement that has
  a real denominator; that gets an interval instead.

Two further rules that fall out of these:

- **No aggregate without its unknowns** (the invariant formerly called the haze rule): the unknown
  fraction an aggregate was built from is drawn in the same mark as the aggregate. The certainty of a
  mark is bounded by the certainty of its worst input, never by the crispness of the computation.
- **Measured and reconstructed look different, everywhere**: measured positions solid, reconstructed
  ones dashed and tagged, in the world, in the Map, and in every table.
