# Combat Log — the rule catalogue

Section 5.22 and Appendix A of the Combat Log design, split out because the whole design read too
long in one file. This document covers how a region's rule set is stored, authored and checked, and
maps one public rule set — the Ashguard Combat Rules — rule by rule as the worked example. The
screens and the levels are in **[combat_log_ux.md](combat_log_ux.md)**; every derived quantity a
rule check reads is in **[combat_log_analysis.md](combat_log_analysis.md)**.

**The tool ships with an empty catalogue** [owner]. Nothing below is a default.

---

## 5.22 The rule catalogue: rules are configurable claims per region

[rules][03] The same behaviour is legal on one sim and banned on the next — dodge distance 10 m on
Chaos ground and 15 m on Ashguard, sprint cooldown 5 s versus 6 s, flak height floors of 15 m or
20 m — so the checkable rule set is **data, bound to predicate shapes the tool does hard-code**.
A Rule is:

```cpp
struct Rule { std::string id, title, text, source;   // source = the sim's own rule page
              U8 measurability;                      // MEASURABLE | PARTIAL | ANNOTATE
              U8 shape;                              // one of the predicate shapes below, or NOTE
              ParamSet params;                       // the numbers, all sweepable
              F32 fromT, toT;                        // whole session, or a negotiated window
              std::vector<NounRef> exceptions; };    // OIC-granted, "rule of cool"
```

**Data for the parameters, code for the predicate — and the document should say so plainly**, because
"the catalogue is data" is only half true and the other half decides what an officer can author.
Every `Rule` names one **predicate shape** from a fixed set the viewer implements, and supplies the
numbers it takes. The shipped shapes, which between them cover the whole of Appendix A:

| Shape | Parameters | Example rule |
|---|---|---|
| `RADIUS` — distance from an area source to each target it hit | kill / wound distance | 5 m kill, 10 m wound |
| `RATIO_CAP` — a distribution of `damage/initial` per victim per script against a cap | cap, min attackers, min damage types | 25 % armour cap |
| `ZONE_EVENT` — events or tracks intersecting a marked spawn zone | zone id, event kinds | no shooting into red spawn |
| `BURST` — speed or fire segments: duration, displacement, recharge gap, in-air flag | max duration, max distance, min recharge | sprints, dashes, jetpacks |
| `INTERVAL` — time between two observable states of one object | min or max seconds | turret rez-to-first-shot |
| `CONCURRENCY` — objects of a class alive at once per owner | cap per person | one auto-turret per person |
| `HEIGHT` — a track's height above ground | max metres | vehicle hover ≤ 15 m |
| `LOS` — the [analysis §5.11](combat_log_analysis.md) sweep against a delivery class | occluder count, thickness, splash margin | no killing through walls |
| `FAMILY_AFTER_T` — hits by an equipment family after a start time | family key, start time | negotiated restrictions |
| `SCALE` — observed object dimensions | minimum box | mine size |
| `NOTE` — no predicate; the page prints the rule text and takes officer annotations | — | every **A** row |

Authoring a rule means picking a shape and typing its numbers; that is the part sim owners can do,
share and export as text. **A rule whose shape is not on that list cannot be authored without viewer
code, and the catalogue editor says so rather than accepting a rule it will silently never check.**
The known gap is real and worth naming: **rate-capped and delayed-start rules** — Grand Federation's
"one superweapon per side per hour of battle after 60 minutes of attacking", LBA's per-minute damage
caps, magazine-and-reload limits — need a *sliding-window count per side with an offset start*, which
is a twelfth shape nobody has implemented. Until it exists those rules are **A** rows carrying their
text and nothing else, which is the honest state, and adding the shape later adds rows to catalogues
without invalidating any that were already written.

**Measurability is printed on every rule, every candidate list and every empty result**, and it is
the load-bearing field. Three classes:

- **MEASURABLE** — a standing query exists and its result means something: explosive radii, the 25 %
  armour cap, healing limits, fire into a marked spawn zone, sprint and dash bursts, turret
  rez-to-first-shot, vehicle hover height, mine scale, AoE not group-safe.
- **PARTIAL** — the query runs but the denominator is untrustworthy: "no infinite ammo" and raycast
  cadence (a miss produces no event), through-wall kills (bounded by everything in
  [analysis §5.11](combat_log_analysis.md)), LBA damage caps (only anti-armor 104 is logged at all).
- **ANNOTATE** — not observable, and the tool says so instead of returning an empty list: LBA
  compatibility, avatar height, prejump, respawn-timer formulas, mono-versus-LSO scripts, gear
  version currency, sound obnoxiousness.

Appendix A assigns the class rule by rule.

**An empty result under ANNOTATE is never compliance.** The Rule page prints "not observable from the
combat log — annotate by hand" where the candidate list would be, and the session summary's Incidents section
will not accept an ANNOTATE rule as evidence, only as a note. Under MEASURABLE and PARTIAL, results
are **candidate rows**, unranked, each linking to its instance and carrying its uncertainty, phrased
as "7 candidates" and never "7 violations" [03]; the officer weighs them against the rule text, which
is printed at the top of its own candidate list. Catalogues are per region, editable, copyable
between officers as text, and carry the OIC exception list, because "the OIC is law during the raid"
is itself one of the rules [rules]. Appendix A maps the worked example — the Ashguard set — rule by
rule.

**The catalogue ships empty, and only the region's own officers may edit it** [owner]. Shipping one
group's rules as everyone's default would be the tool taking sides in an argument the community has
not settled; a **universal core** — the few rules every compared set shares — may be curated later,
and if it is, it arrives as suggestions an officer accepts row by row
([ux §8](combat_log_ux.md)).

Officer status is derived, not asked, because a permission dialogue nobody can answer correctly is
worse than a guess that shows its working. Two signals, in order: **the estate managers list**,
easiest and already in the viewer, whose members count as officers of that region; then **the
dominant land-parcel group** and, within it, the roles carrying parcel-edit powers — the group's
owner and its officer roles — because a region is typically held by a land group distinct from the
military one ([analysis §5.9](combat_log_analysis.md)). The derivation prints beside the edit control
(*"editable: you are an estate manager of Resdayn"*, *"read-only: catalogue owned by Ashguard Land"*)
so nobody has to guess why a button is grey. Both signals are wrong sometimes, which is an open
question ([ux §8](combat_log_ux.md)).

## Appendix A — the worked rule catalogue and what the tool can say about it

The Ashguard Combat Rules, used as the worked example because they are public and complete; other
groups differ, the catalogue is per region, and **none of this ships** — it is here to show what a
filled catalogue looks like and to prove the eleven predicate shapes cover a real rule set. **M** =
measurable, a standing query whose
result means something. **P** = partial, the query runs but its denominator is untrustworthy and the
page says why. **A** = annotate only, not observable from the combat log — an empty result under A is
*never* compliance. Numbers in the "measures" column are the region's configured parameters and are
all sweepable ([ux §1.2](combat_log_ux.md)).

### General

| Rule as written | What the tool measures | |
|---|---|---|
| No entering or shooting into red spawn zones unless returning fire | membership of a spawn zone the officer marked by hand, from tracks × damage events with the shooter's position; no marked zone means no check, and the row says so; "returning fire" needs the officer's judgement, so the return-fire case is listed beside each candidate | **M** |
| Vehicles and armour must be LBA compatible | the LBA header in the object description, where the viewer saw the object; its absence is not proof of absence | **P** |
| Vehicles must have respawn timers | the interval from published HP reaching zero to the next rez of the same identity, where both were in view ([analysis §5.7](combat_log_analysis.md)); out of view a re-rez is indistinguishable from an unrelated deploy | **P** |
| LBA damage cap 2000/min per weapon (+750 per extra crew) | only anti-armor type 104 reaches the combat log; the rest of the damage is invisible, so any total is a floor | **P** |
| Avatars ≥ 1.5 m tall; prejump enabled; no obnoxious sounds | nothing | **A** |

### Weapons

| Rule as written | What the tool measures | |
|---|---|---|
| No infinite ammo | sustained fire cadence and gaps between volleys per family; misses are invisible, so a low-rate weapon can hide | **P** |
| **No weapons designed to kill through obstacles that would typically stop them** | the whole LOS analysis ([analysis §5.5](combat_log_analysis.md), [analysis §5.11](combat_log_analysis.md), [analysis §5.12](combat_log_analysis.md)): delivery class, occluder count and thickness, flip fraction over τ, per-family through-wall interval over FIRM verdicts only | **P** |
| No fully automatic raycast weapons | hit cadence for hitscan-classified families; hits are not shots | **P** |
| Burst raycast only without "haze" (a ray cluster) | multiple simultaneous hits on one or more targets from one hitscan source at the same instant | **P** |
| No sensor or agent-list weapons (except flamethrowers, explosives, melee, lock-on) | hits with no plausible line and no projectile, and hits on targets never in the shooter's frontal arc; suggestive only | **P** |

### Explosives and area effects

| Rule as written | What the tool measures | |
|---|---|---|
| **5 m kill / 10 m wound radius** unless highly directional | distance from the blast point to every target hit by that source within 1 s, with σ intervals ([analysis §5.5](combat_log_analysis.md)) | **M** |
| Larger radius only with a visible and audible wind-up | rez-to-detonation interval where the object was seen | **P** |
| Explosives with kill radius > 2 m must be interceptable | a deployable whose published HP fell to zero before it detonated, where the viewer saw it | **P** |
| No non-physical or interceptor-defeating projectiles | ghost-projectile flights: a hit with no flight and no attachment is the signature | **P** |
| **Explosives must not kill through walls or solid objects** | LOS from the blast point to each victim, with the thin-wall-splash exemption drawn explicitly ([analysis §5.5](combat_log_analysis.md)) | **M** |
| Mines ≥ 0.25 × 0.25 × 0.04 m | object scale where the viewer saw the object | **P** |
| AoE must not be group-safe | same-side avatars inside the measured radius who took no damage from that source | **M** |

### Movement

| Rule as written | What the tool measures | |
|---|---|---|
| Jetpacks: no in-air recharge, ≥ 3 s recharge, reasonable distance | vertical speed bursts against `IN_AIR` / `FLYING`, interval between bursts | **M** |
| Sprints ≤ 5 s with ≥ 6 s recharge | burst-speed segments from tracks, duration and gap | **M** |
| Dive-rolls and dashes ≤ 15 m, not in air, ≥ 4 s recharge | displacement per burst, `IN_AIR` at burst start, recharge interval | **M** |

Each of these is a distance or duration measured at 2 Hz, so every candidate prints an interval, and
the cross-group spread (dash 10 m on Chaos, 15 m on Ashguard) is exactly why the number is a
parameter [rules].

### Avatar equipment

| Rule as written | What the tool measures | |
|---|---|---|
| **360° body armour ≤ 25 % damage reduction** | `1 − median(damage/initial)` per victim per script, over ≥ 2 attackers and ≥ 2 damage types ([analysis §5.14](combat_log_analysis.md)) | **M** |
| Healing limited per life or by recharge | negative type-100 events per life, totals and inter-heal intervals, self versus received ([analysis §5.14](combat_log_analysis.md)) | **M** |
| Riot shields ≤ 50 HP and directional | published max HP from the LBA header where the shield was in view; directionality, nothing | **P** |
| Blinding / movement-limiting effects must recharge longer than they last | only if the effect emits combat-log events; most do not | **A** |
| Immunity beyond limits (the officers' phrasing) | the immunity patterns of [analysis §5.14](combat_log_analysis.md): at-spawn versus mid-field-while-dealing-damage, bystander versus while-killing, with `task_id` resolved to owner and creator | **M** |

### Deployables and turrets

| Rule as written | What the tool measures | |
|---|---|---|
| Explosive deployables ≤ 1 HP, destroyable by projectiles | published max HP from the LBA header where the object was in view | **P** |
| One auto-turret per person | turret-classed rezzers alive concurrently per owner | **M** |
| Turrets must be constructed, not instant | rez-to-first-shot interval | **M** |
| Turret field of fire ≤ 180° | bearings from the turret to its targets relative to its facing, where the object was seen | **P** |
| Explosive turret ammo only with lock-on time | target-acquisition-to-fire interval; approximate | **P** |

### Vehicles

| Rule as written | What the tool measures | |
|---|---|---|
| No avatar-hitbox aircraft; hitbox must represent visual scale | nothing | **A** |
| Hover or jump ≤ 15 m above ground | seated-avatar track height above ground height | **M** |
| Self-repair cooldown ≥ 1 min | upward steps in the published health series, and the gaps between them | **P** |
| Respawn timer formula; HP caps 400 / 200 / 150 / 10 | published max HP against the region's class table, plus time to kill from the health series | **M** |
| Anti-armor damage actually landing | type-104 hits on the object, counted, with health explicitly **unknown** ([analysis §5.7](combat_log_analysis.md)) — the answer to the commonest Incident | **M** |

### Conduct and command rules — one group's, as an example [merits]

These come from Ashguard's own internal handbooks and are **example only**: conduct and command
practice is the part of a rule set that varies most between SLMC groups, and nothing in this table is
shipped, assumed or offered as a default [owner]. It is here to show what the measurability classes
do to rules of this kind.

| Rule as written | What the tool measures | |
|---|---|---|
| Raids begin in low escalation and rise as the OIC directs | first use per side per escalation class, as a timeline lane ([analysis §5.23](combat_log_analysis.md)) — off by default, and only once the group has told the tool what its classes are | **M** |
| Only group-issued or authorised equipment during raids | the own-side loadout list: what each combatant used, with our own arsenal marked and nothing flagged ([analysis §5.24](combat_log_analysis.md)) | **P** |
| No non-group combat attachments without approval | the same list; a weapon never fired is never seen | **P** |
| Clean combat avatars, no unnecessary scripts | nothing | **A** |
| The OIC is law; cross-faction complaints go OIC to OIC | encoded as the exception list on every Rule, and as the framing of every candidate list | **A** |
| Negotiated in-fight restrictions ("do not use X from 17:26") | a Rule with a start time bound to an equipment family; later hits list as rule-breaking rather than suspicious ([analysis §5.23](combat_log_analysis.md)) | **M** |
| Leaving before the OIC's last-life call | departures after the `⚑ last life` mark, from the presence lane | **M** |

### Universal rules every compared rule set shares [rules]

| Rule | What the tool measures | |
|---|---|---|
| No client-side assistance (wireframe, hitboxes, derendering, ARC) | never provable. Behavioural signals only ([analysis §5.15](combat_log_analysis.md), [analysis §5.19](combat_log_analysis.md)), each with its confound list and its mandatory context strip | **P** |
| No killing through walls by phantom bullets, non-raycast explosions or rez offsets | [analysis §5.5](combat_log_analysis.md)'s delivery taxonomy plus [analysis §5.12](combat_log_analysis.md) | **P** |
| Safe zones and courtesy lines around spawns | a marked spawn zone or an auto-detected spawn hotspot, plus tracks, damage and `d_spawn` ([analysis §5.7a](combat_log_analysis.md)) | **M** |
| "You should always be killable" — no invincible armour | [analysis §5.14](combat_log_analysis.md)'s immunity patterns | **M** |
| Renaming gear to evade a banned-equipment enforcer | same creator and damage-type signature, new name, mid-session ([analysis §5.13](combat_log_analysis.md)) | **M** |
| Doxxing, GPU crashing, mass abuse-reporting, content theft | nothing, and nothing here should ever pretend otherwise | **A** |

Counting the table: roughly a third of the written rules are measurable, half partial, and the
remainder annotate-only — the balance moved toward *partial* once LBA health turned out to be
readable from object descriptions and hover text
([analysis §5.7](combat_log_analysis.md)), which is exactly the kind of correction the measurability
field exists to absorb. That ratio is the honest headline of this document. A tool that presented
rule checking as coverage would be lying about most of the rule book; one that presents it as
*candidates with a measurability class printed on every row* tells an officer precisely which of
their own rules a log can help them with.
