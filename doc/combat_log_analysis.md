# Combat Log — analysis model

Section 5 of the Combat Log design, split out because the whole design read too long in one file.
Everything here is the *derived* half of the tool: what each quantity is, how uncertain it is, and
what it costs to compute. The screens that show these quantities, the levels they hang on and the
build order are in **[combat_log_ux.md](combat_log_ux.md)**; the rule catalogue's mechanics and the
worked rule set are in **[combat_log_rules.md](combat_log_rules.md)**.

Inline credits carry the same meaning as in the UX document: entry ids **[01]**–**[12]** for design
grafts, **[owner]**, **[reports]**, **[rules]** and **[merits]** for domain facts, the last of these
naming Ashguard's own internal handbooks and therefore always one group's practice rather than the
community's.

---

## 5. Analysis model

Every quantity below states its computation, its uncertainty representation and its cost strategy.

**5.1 Event time and ordering, on the simulator's own clock** [owner]. Script 2 stamps each event with
`llGetEnv("frame_number")` — the simulator's physics frame counter, an integer string, nominally 45
frames a second and slower under time dilation. That is the correct clock, not a convenience:
collisions are resolved and damage applied per physics frame, so two events in one frame are
genuinely simultaneous whatever a script's wall clock says.

The viewer maps **frame → viewer time** from the tracking ticks, which carry the same counter beside
their receive time, with a **piecewise-linear fit**: a slope per segment (seconds per frame, ~1/45 at
rest, visibly larger under dilation) and a break wherever the residual exceeds a frame. Events and
tracks therefore share one timeline and it is the simulator's, which is what makes "who hit whom
first" answerable at all.

Each event carries `t̂` from that fit and an uncertainty `u` combining the frame quantum with the
local residual: **0.05 s** `fd`-confirmed, **one frame at the local slope** (0.02 s at rest, worse
under dilation) frame-stamped, **1.0 s** receive-time fallback without a counter. The store sorts by
`t̂`, and **no ordering claim is made between two events whose `t̂ ± u` intervals overlap** [01] —
same-frame events are ambiguous by construction, which is honest rather than unfortunate. `fd`
records exist only for the reviewing officer's own avatar; the UI never implies sub-second precision
for anyone else [06]. R−1 shows the raw counter, the segment that mapped it and the residual.

**5.2 Position, quality, coverage.** `p(a,t)` from `sampleAt`; base σ by source (bridge 0.15 m,
viewer 0.3 m, coarse 4 m horizontal / 2 m vertical) plus `0.5·gap·|v|` when interpolating. Drawn as
foot-ring radius, chipped `●●○`. **Coverage** `C(a, window)` = fraction of the window with a sample
within 1 s; drives dashed trails, the ribbon quality row, and hatching of anything built from that
combatant. **Coverage is a property of this viewer, not of the fight**: the simulator's Interest List
decides which object updates reach us, so presence, accuracy and timing of every track-derived
quantity are unique to where the officer was standing and what their viewer asked for. That is why
coverage is printed beside everything rather than assumed, and why the STRAINED meter of §5.9 ships
off by default ([ux §3.6](combat_log_ux.md)).

**5.3 Attacker position tiers** [05]. Resolved in order and **printed beside every LOS verdict,
damage-in-flight line and suspicion tally**:

1. **TRACK** — `source`/`owner` is an avatar with a track sample in the window (handheld; mount fired
   by its rider uses the *mount's* track, not the seated avatar's offset [06]).
2. **LAST-SEEN** — the object (turret, trap, deployable, vehicle) has a position recorded by the
   equipment registry the last time viewer or bridge saw it, confidence decayed with staleness.
3. **APPROX-VIA-OWNER** — no object sighting exists; the rezzing avatar's own track stands in.

Sub-TRACK draws a hollow diamond in world, and **tier 3 is excluded from through-wall tallies by
default** with a clearly labelled opt-in, so a guessed position never launders itself into a weapon's
suspicion score. Events with no usable position land in the visible unattributed counter [10].

**5.4 Volley and admission filters.** Volley = `(owner, rezzer, target)` runs with ≤ 2 s gaps [01].
Before anything reaches an aggregate [08]: untrusted script-written events get weight **zero** (they
stay visible with an `untrusted` chip, and `+untrusted` is a sweep axis); events whose target is not
an agent never contribute hostility and are tallied as structure damage instead. Damage-type
semantics from the brief are honoured by name: type 100 with negative damage is **healing** (it
inverts into affinity), 106 **redeploy** is a respawn signal and not a hit at all, 102/103 splash is
discounted, 104 anti-armor usually means vehicles.

**5.5 Delivery classification, and what a through-wall kill actually is** [owner][rules]. Four
classes, decided per event, printed on every sweep, because LOS is a *different test* in each:

| Class | Decided by | Consequence for LOS |
|---|---|---|
| **hitscan / direct** | `source` attached, or `source == rezzer`, or the source's first and last observed positions are far apart with no plausible flight (the rezzed "damage prim" that teleports onto the victim) | no travel window; one strong test at `t`, run from **both** the eye and a plausible camera origin; never draw a trajectory |
| **projectile** | `source` unattached and short-lived; a ghost-projectile flight seen, or inferred from its lifetime | travel window applies; §5.11 sweeps τ |
| **lobbed / arc** | an observed flight whose vertical velocity changes sign, or a delivery whose straight chord is blocked while a ballistic arc between the same endpoints is not | tested against the arc; a blocked chord is **not** evidence |
| **area** | one `source` hitting ≥ 2 targets within 1 s, or damage type 102 | radii measured per target; attribution shares per target, never split |

**Ghost projectiles make flights observable** [owner]. The viewer's existing heuristic tags in-view
objects shaped like bullets (long, thin) and tracks them, so for projectile weapons the tool records
spawn point, velocity, lifetime and stop point, and infers the shooter from where the bullet appeared
relative to the avatar tracks. This is the one place the tool has *positions the combat log does not
give it*, and it pays for three things the log alone cannot support: **shot origin** (so a hit whose
`owner` has no track still has a measured start point, tiered above APPROX-VIA-OWNER in §5.3),
**bullets into walls** (a flight terminating on geometry with no DAMAGE), and **fired-without-aiming**
(flight direction versus the 2 Hz body yaw). Flights are measured evidence and draw solid; anything
derived without one stays reconstructed and dashed. Seen flights are a small, capped store on the
same retention sweep as events, and they are the material the reconstruction ([ux §3.10](combat_log_ux.md)) animates.

**It is a strong secondary source and never a primary one** [owner]. Capture depends on the simulator
sending this viewer updates for the bullet at all — in view or not — so coverage is unknown, uneven
and unique to the officer's position. A flight therefore **corroborates**: it can raise confidence,
ground a shot origin and turn inferred evidence into measured. It never carries a verdict alone. No
candidate row exists because a flight exists, no through-wall tally counts a flight as a hit, and "no
flight was seen" is never evidence of anything.

**Shots fired is not a statistic this tool has, and flights are not a substitute for one.** Only hits
are logged and the viewer sees only some flights, so hit rate has no honest denominator and is **out
of scope for v1**. Where flights and hits are compared at all — "bullets into walls", "no infinite
ammo", raycast cadence — the comparison prints the region's **time dilation** over the same window
beside it. The physics reason is not a detail: collisions are detected per physics frame, so under
lag a fast bullet can pass between frames and register nothing, and a flight with no DAMAGE is not
evidence of a miss on a dilated sim [rules]. Dilation under 0.9 hatches the comparison and says why.

**Area damage against the written radii.** For an area event the tool measures the distance from the
blast point to every target hit by that source within 1 s, and compares it against the region's
configured kill and wound radii — 5 m / 10 m on Ashguard, other numbers elsewhere, both sweepable
([ux §1.2](combat_log_ux.md)). A kill outside the kill radius or a wound outside the wound radius is a **candidate row for
that rule**, printed with the distance interval (σ from §5.2), not a violation. Group-safe AoE is the
same measurement with the sign flipped: same-side targets inside the radius who took *no* damage
[rules].

**Through-wall legitimacy is a taxonomy, not a boolean** [owner]. The community's own line: legitimate
are landmines and tripwires (the source was already on the far side), ballistically thrown grenades
and mortar-style lobbed shots (arc, not chord), and minimal splash through a thin wall. Illegitimate
are bullets through thick walls or barricades, and explosives inflicting *direct* death with no
raycast beyond thin-wall splash. So a BLOCKED verdict is only interesting once the tool has answered
three further questions, all of which it prints beside the verdict: what was the **delivery class**
(an arc is not a chord); how many **occluders** did the line cross and how **thick** were they
([ux §4.5](combat_log_ux.md)'s occlusion point cloud gives the count, the raycast gives the near and far hit per occluder);
and for area damage, was the victim within **splash** distance of the far face. Nothing here becomes
a verdict: §5.12 surfaces models whose kills are repeatedly BLOCKED with FIRM geometry as
*candidates*, and the officer judges, because "nothing is set in stone and no weapon list of suspects
exists" [owner].

**Blindfire after a flash is a named confound**, promoted to a first-class part of the through-wall
tracking signal rather than a footnote [reports]. The group's own threads observe that long
flashbangs "are literally bait to make wireframing allegations because people blindfire in the
direction they are flashed from". So every awareness-lead or tracking-through-walls card (§5.15,
§5.19) carries a mandatory **context strip** of the preceding 15 s — incoming hits, explosive and AoE
events, nearby effect sources, the subject's own recent deaths — and the card cannot be pinned
without it [03]. The strip carries its own caveat: effects that deal no damage never reach the combat
log at all, so a *clean* strip is not exculpatory either.

**5.6 Death attribution** [06]. DAMAGE on the victim in `[t−W, t]` grouped into volleys by
`(owner, rezzer)`. Report **raw damage totals and hit counts, ranked**, tagged **dominant**
(> 2× runner-up) / **contested** (within 2×) / **long-shot** (< 15 %) — never a manufactured
percentage split, and always labelled "raw damage, not health", because health is unobservable. The
**killing blow** is the last event by `t̂` *only if* its margin over the runner-up exceeds the
combined uncertainty; otherwise the article reads "ambiguous between X and Y", both are drawn, and
equipment kill credit is split and hatched [01].

**5.7 Death, teleport and DOWNED are three separate signals.** A position jump of ~8–24 m in one
frame with no velocity to explain it is recorded as a **teleport** event, linked to but never implied
by a DEATH; custom DOWNED states written to the combat log are recorded as their own kind; type-106
redeploy deaths are labelled as such. Neither implies the other, and the Life noun draws all three
[owner]. The decoupling earns its keep in both directions. A **DEATH with no teleport within
2 s** is an anomaly row in the flagged panel — the respawn system failed, or the combatant left the
experience or detached the HUD, both of which are faults officers already chase by hand and write
into Incidents [reports]. A **teleport with no DEATH** is a redeploy, a sit-hack, or a movement
enhancer, and it is only ever listed, never named. Under a Battlefield-style DOWNED system the
simulator's DEATH is *not* followed by a teleport until the downed combatant fails to be revived, so
on such a region the anomaly rule inverts and the Life noun shows a downed interval. **The region says
which regime is in force, and it says it in `death_action`**: 0 teleports home, 1 sends the combatant
to the parcel landing point, 2 to a telehub, and **3 does nothing at all**, which is the signal that a
scripted combat system such as EBCS or FLECS is handling deaths in this region rather than the
simulator itself, not merely the signature of "a DOWNED-style system" [owner], and switches the
anomaly rule off rather than filling the flagged panel with
88 non-anomalies ([ux §3.7](combat_log_ux.md)). A region that reported no value leaves the tool printing `death-regime
unknown` rather than counting anomalies it cannot interpret.

**Object health is observable after all, and only the damage behind it is not** [owner]. The
universal, reliable part of the LBA standard is the object **description prefix `LBA.v.`**; the text
that follows it varies heavily across LBA variants and custom scripts, so the tool does not parse a
fixed `<rev>,<hp>,<maxhp>` layout. Hover text also varies a lot between LBA objects and is not always
present, but when it is, it typically contains a `#/#` pair (current over max points). Detection runs
in two steps: a simple regex for **two integers separated by a slash** in hover text flags a candidate
object, and the `LBA.v.` description prefix **confirms** it as LBA; once confirmed, the tool tracks
**name, owner, group, position, rotation and health over time**, sampled from the hover-text and
object-property updates the viewer already receives for anything in view. So the Object noun carries a
**health-over-time series**, with the coverage caveat that it is blank whenever the object was
out of view. What stays unobservable is the **damage source**: LBA damage is chat on a per-object
private channel, so only anti-armor type-104 hits appear in the log at all. Two findings become
measurable on the back of the series: **time to kill** against the region's HP class table, and
**instant regeneration after death**, which is the recurring equipment dispute in Incidents
[reports]. "The drone was not taking anti-armor damage" is now answerable as *"31 type-104 hits over
6 minutes, and its published HP went 400 -> 400"* rather than as a shrug.

**5.7b Session boundaries and declarations.** A session is one region, because combat and death
teleports stay inside one [owner]. Its start and end are inferred from event density, and the
region's own combat settings ([ux §3.7](combat_log_ux.md)) are read on handshake. **Region-system channels are out of v1**:
listening to a region combat system's teams, respawns, tracker and meta channels would seed teams and
spawn geometry at high confidence, but it is another listener, another parser and another format to
track, and every model below already works without it [owner]. The system to assume is **EBCS** (the
Experience Based Combat System), which is open source and runs on many combat regions; the owner's own
**FLECS** on Resdayn is one region's system and not a community standard [owner], so a listener written
against it would fit one sim and no others. The tool does not print a
`no-region-system` chip, because it is not degraded relative to a thing it never had; what it offers
instead is the debug channel log ([ux §7](combat_log_ux.md)), which records everything those platforms broadcast for anyone
who later wants to design a listener from evidence.

**No external source seeds a session** [owner]. SLMC groups share an unofficial raid-declaration
button on Discord that posts an alert when an attacker registers at a sim, but it is a community
convenience passed between groups, not a session-boundary marker and not an allegiance seed: no
session id, no party size and no allegiance fact from it ever crosses into the tool. There is no
external raid signal in this design; session boundaries come from event density alone, as above.

**5.7a Spawn hotspots, `d_spawn`, and the respawn test** [08][11]. Three quantities that several other
sections lean on, and one precondition that switches all three on or off together — the same shape as
§5.9's group-tag admission tests. Everything here is **auto-detected and unnamed**: a hotspot is a
cluster of landing points and claims to be nothing else. Officers are never asked to draw or mark a
spawn zone by hand [owner]: hotspots are the only spawn geometry v1 has, and owner-designed zones
remain a future region-owner feature ([ux §2.1](combat_log_ux.md)).

- **The respawn test**, decided before any spawn geometry is built: the session has ≥ 8 deaths, and
  ≥ 60 % of deaths are followed within 6 s by a teleport jump (§5.7) whose landing point lies within
  24 m of another death's landing point. Fail ⇒ no zones, no `d_spawn`, no cohort term; the degraded
  strip prints `no-respawn` and everything that would have used them is marked, never defaulted.
- **Spawn hotspots** = DBSCAN over those post-death landing points, ε = 12 m, minPts = 4, solved per
  team-window when teams exist and unlabelled otherwise. Hotspot radius = 90th percentile of member
  distance from the medoid. A hotspot with under 4 landings draws hatched and is never a denominator,
  and none of them is ever given a name by the tool.
- **`d_spawn(a, t)`** = horizontal distance from `p(a, t)` (§5.2) to the nearest hotspot centre of that
  combatant's current assignment (nearest of any when unassigned); `—` when the track has no sample
  within 1 s or the respawn test failed. Its uncertainty is §5.2's σ, so it prints as an interval
  whenever it is used as a threshold — including §5.14's "immune at the spawn hub" pattern, which is
  `d_spawn < hotspot radius + 10 m` and is reported with the interval, not the point value.
- **Spawn-cohort affinity**: two combatants landing in the same hotspot within 20 s accrue a decayed
  positive term (same 90 s half-life as §5.9) entering §5.9 **as a seed prior only** — never as a
  damage edge, and never able to raise a band on its own. Admitted only when the respawn test passes,
  ≥ 2 hotspots exist and they are > 40 m apart; otherwise off with a `no-respawn` / `one-spawn` chip.
  Like the group-tag prior it is ablated in the mandatory second solve, so a wrong cohort prior
  surfaces as a dependence flag rather than as silent structure.

**5.8 Bouts and phases** [01]. DBSCAN over `(x, y, t/κ)`, κ = 1.2 m/s, ε = 30 m, minPts = 4.
**Position-less DAMAGE is placed at the target's interpolated position** — not the attacker's, not
the midpoint: bouts are where people are being hit, and midpoints invent locations where nobody
stood. Placed events are flagged; edges dither when boundary density is within 20 % of threshold.
Phases merge bouts at session scale and split at lulls (> 90 s below 5 % of median intensity). All
constants are R2-sweepable and every bout carries a boundary-stability chip. A **spatial cluster
count** over the death-density grid tells "one front with noise" from "this is not one front", and
the phase ribbon **forks** rather than blending when more than one cluster is live [05].

**5.9 Team assignment.** Nodes = avatars, edge weight = damage exchanged, symmetrised, log-scaled,
exponentially decayed (half-life 90 s), and **normalised by tracked-and-armed seconds** so a late
arrival is judged on rate, not volume [11]. Windows are phases or a 5-min slide. Then, in order:

- **The curated group list, a weak prior and optional** [owner]. The owner maintains a registry of
  SLMC groups and the regions they hold, and the tool reads it **in that registry's own schema, so
  the file drops in unchanged**: `armies` as `{g: group uuid, m?: militia group, c?: civilians group,
  l?: land group, n: name, s: shorthand}` and `regions` as `{n: region name, t: BASE | CONTINENT |
  NEUTRAL, g?: owning group, l?: land group}`. It is JSON in the settings directory
  (`ss_combat_groups.json`, user settings overriding `app_settings`), **absent by default**, and the
  tool says so when it has none. Three uses and no more: a **weak allegiance prior**, ablated like
  the group tag; a **home-side prior** from the region's type and owning group, which is how a
  defence is told from a raid and a neutral sim from someone's base; and the **own-arsenal**
  identification of §5.24, from each army's maker prefixes. It never assigns a side on its own.
- **Group where visible, else the group of a worn attachment** [owner]. Any worn attachment keeps the
  group it was worn under, so when a combatant's title is hidden the viewer probes an attachment's
  group instead of giving up; both are derived viewer-side, with the script only as a fallback. It
  enters on the same terms as the active tag — one prior, ablated in the second solve — and a
  disagreement between the two is printed on the Avatar page rather than resolved silently.
- **Group-tag seed admission is decided BEFORE clustering**, from the damage graph's vertex set
  alone, by three tests [01]: (a) the avatars with ≥ 1 damage edge carry ≥ 2 distinct group ids;
  (b) each such id covers ≥ 2 of them; (c) under half the window's damage weight runs between
  avatars sharing an id. If any fails, seeding disables itself and the header names the failed test.
  This removes the circularity that makes the same-tag case silently wrong.
- **Spawn-cohort seed prior**, per §5.7a: gated on the respawn test, weak, never band-forming, and
  ablated alongside the tag prior below.
- **Mandatory tag-free second solve** [08]. Every solve runs twice, with and without the priors above.
  Disagreement can light a **(!) tag-dependence flag** on the band header beside a **STRAINED nn%**
  meter (share of the band's internal pair weight that is negative) with a one-click `[test split]`.
  Cost is one extra solve on ≤ 100 nodes, sub-millisecond, off the draw path. If the two partitions
  disagree on more than 10 % of members, the unseeded result is shown — that behaviour is
  unconditional. **What is conditional is the display**: the meter and the flag sit behind a
  preference and are off by default ([ux §3.6](combat_log_ux.md)), because the inference leans on track data the Interest
  List shaped, and an accusation-shaped chip in the default view is a social hazard the owner would
  rather choose than inherit [owner]. `[test split]` stays reachable from R2 either way.
- **Three outcomes, not one number** [11]. Label propagation runs for K = 2..5 × θ ∈ {0…1 step 0.05}
  per window; cluster ids match across θ and across adjacent windows by maximum overlap. **Drift**
  (modal cluster changes between windows, stability high both sides) → chevron plus a printed change
  time. **Noise** (stability low inside the window) → hatch, unclassified. **Short** (small n) → its
  own badge. Low n is never conflated with low clarity.
- **Structure index** `S` is **sign-flipped Newman modularity of the current partition on the same
  symmetrised, decayed, log-scaled hostility graph the solve ran on**: the share of the window's
  damage weight running *between* bands minus its expectation under the degree-preserving
  configuration model. A clean two-sided fight scores high, a deathmatch scores ≈ 0. `S_null` is the
  mean of `S` over 20 size-preserving random partitions of the same vertex set;
  `S − S_null < 0.08` for 60 s prints DEATHMATCH, the tool stops naming factions, and the Braid
  becomes a per-person hostility heat strip that scales to 80 rows [08].
- **Overrides are time-bounded** [09]: `(avatar, team, effective_from)`, treated as a hard seed only
  from that point on, so a correction never overwrites correctly-inferred earlier history. Overrides
  appear in backlinks, are undoable, and report their delta ("2 friendly-fire flags removed, 1
  added") [01][08].

**5.9a Contested, defined once and used everywhere.** Objective episodes, the time-in-contested-areas
rows, the courtyard episode and the Front face's "contested surface" all lean on one word, so it
gets the same treatment as every other derived quantity. **An area is contested over a window when
combatants from ≥ 2 bands were simultaneously inside it (or within 4 m of its edge) for ≥ 20 s of
that window, and ≥ 1 damage event with a target inside it occurred in the same window.** The areas
it can be applied to in v1 are the auto-detected spawn hotspots and the Front face's ground grid;
**objective areas require owner-designed zones and are a future item** ([ux §2.1](combat_log_ux.md)), so every summary statistic and
offered fact that wanted one prints *requires owner-designed zones* instead of a number. The dwell
threshold and the edge margin are ParamSet entries and sweepable at R2 ([ux §1.2](combat_log_ux.md)). Reported as
**contested seconds with n and coverage** — `contested 6m40s [±40 s] · coverage ●●○` — never as a
bare boolean, because the seconds come from `sampleAt` and inherit §5.2's σ and gaps. Two degradations
are named rather than defaulted: when the structure index says DEATHMATCH (§5.9) there are no bands,
so the band test falls back to **≥ 2 combatants who damaged each other within the window**, and the area
header prints `contested (no bands)`; when track coverage in the area is under 60 % the contested
seconds hatch and cannot seed a summary statistic or an offered fact. "Contested surface" in the Front face is
the same predicate applied to the 4 m ground grid rather than to a spawn hotspot.

**5.10 Friendly fire, and the ordering that keeps it non-circular.** Friendly fire is damage where
attacker and target share the *window's* assignment — which is produced by the very clustering that
wants friendly fire excluded from its evidence. That circle is broken by fixing the order rather than
by iterating to a fixpoint:

1. **Solve once on the full graph**, every damage edge included, giving provisional labels.
2. **Label friendly fire from that provisional partition**, and from nothing else.
3. **Re-solve once on the FF-stripped graph.** The result of this second solve is the window's
   assignment, and it is what the Braid, the Matrix and every flag are drawn from.

Exactly two passes, never more, so the assignment cannot oscillate between runs. **Disagreement is a
reported outcome, not a tie-break**: if the two solves differ on more than 10 % of members the
FF-stripped result is *discarded*, the full-graph partition stands, and the band header carries an
`ff-unstable` chip whose why-page lists the members that moved — because a set of edges large enough
to change the sides is a set too large to have been friendly fire. The whole two-pass sequence runs
inside *each* arm of §5.9's mandatory tag-free second solve, so a window costs four label-propagation
solves on ≤ 100 nodes; still sub-millisecond and still off the draw path (§5.21).

Every flag inherits its window's confidence and is **bucket-local**: a flag whose neighbouring bucket
disagrees says so on its face [06]. Flags under an `ff-unstable` header print that chip too, so a
friendly-fire list is never read as firmer than the team split it was derived from [02].

**5.11 LOS as a function.** For projectile events, τ over `[0, SSCombatLogTravelSeconds]` in 0.1 s
steps × 3 aim points (head/chest/feet) × 2 eye heights (with the +0.6 m Exodus-style retry), ≤ 78
casts [11]; for hitscan, one test at `t`. Outputs, in this order of prominence:

1. **Flip fraction and coverage** as the headline [11].
2. The **flip point printed in the lede and infobox** — free, because the sweep runs on the grid the
   claim already used: "BLOCKED for travel < 0.9 s; CLEAR at or beyond it, 5 of 6 candidates" [02].
3. **MARGINAL as a first-class outcome** [02]: jitter the deciding candidate by its own position
   error along the two axes perpendicular to the ray (4 extra rays); agreeing → FIRM, disagreeing →
   MARGINAL.
4. **Anchor robustness** [12]: re-run the whole verdict at t−1 s and t+1 s and label it ROBUST, or
   show both possible verdicts.
5. `[a clear shot exists]` demoted to a small chip [11].

COARSE-sourced candidates return UNKNOWN by rule; a BLOCKED result from a thin-input candidate draws
grey, not red [06].

**5.12 Through-wall rate per model.** `BLOCKED / (BLOCKED + CLEAR)` **over FIRM verdicts only**, with
MARGINAL and UNKNOWN counted beside them, a Wilson 95 % interval, and "resolved n/N" narrowing live
as the background cast queue drains [02]. Never "kills through walls" — always `67% [21–94%] · n=3`.
A model is interesting only when its interval excludes the session baseline [11].

**5.13 Equipment identity and classification.** Names are user-editable and spoofable, so the family
key is `(creator, root name normalised, attach-point class, damage-type signature)` [11]; collisions
raise a chip with officer merge/split. Classification is the plan's rules as a weighted evidence
checklist with the fired and failed lines shown and confidence as `●●●…○○○`; UNKNOWN is a first-class
outcome [07]. Everything that cannot be attributed lives permanently in the **UNATTRIBUTED** row
[11]. **VEHICLE and MOUNT come from the viewer's own parenting, not from a heuristic**: a seated
avatar is a child of the vehicle's linkset and the viewer already knows the root object and moves the
avatar with it, so the root *is* the vehicle, its velocity is the vehicle's, and the crew is every
avatar parented to it — a moving root is a vehicle, a static one a mount or a seat [owner]. The
script tick's root key ([ux §7](combat_log_ux.md)) is a fallback for avatars outside this viewer's
interest list, not the primary signal.

**The family key the officer sees is the one they already use.** Weapon names in the community carry
a **maker prefix and a version** — `[Ash] - Kagrenac bolt-caster v1.1.0`, `[Ex] PP19 Bizon v 1.04`,
`[AI] AKSZU CS 6.2.4` — and equipment identity by *name* is already the enforcement key everywhere
(banned-equipment enforcers key on object names, and renaming gear to evade one is itself a ban
offence) [reports][rules]. So the Model page's title is `prefix + stem, version stripped`, and
versions aggregate into one family with a version breakdown inside it. The robust key above stays as
the *internal* identity, and the two are reconciled explicitly: a name-family spanning more than one
creator raises a merge/split chip, and a **rename mid-session** — same creator, same damage-type
signature, new name — is listed on the family page as a signal in its own right, because that is
what evading a name-based enforcer looks like [rules]. The maker prefix is also what makes §5.24's
own-side loadout list possible.

**Statistics the feed has trained officers to expect** [reports]. Per family: users, hits, kills, `ρ`,
through-wall interval (§5.12), and the **distance-at-kill distribution** as a small strip, because the
group's Discord kill feed prints "*from 41 meters away*" on every line and distances legitimately
span 1.5 m to 230 m+. A full-damage hit far beyond a family's usual falloff is an anomaly row, not a
verdict. **Kills with no owning avatar** (the feed's `**`) and **self-kills** are ordinary rows
everywhere: drawing normal feed entries as anomalies would train officers to ignore the styling.

**Loadout is a timeline, not a fact** [owner]. Combatants wear several weapons at once and switch
between them without re-attaching, and swap whole layer sets, so attribution is per hit and the
Avatar article carries a **loadout-over-time** strip with one band per layer: Layer 1, Layer 2,
Layer 3, plus Special and Vehicle bands, matching how the group's own equipment documentation is
organised. Each band shows attach/detach where the viewer saw it and *first and last use* where it
did not, since a weapon is often only visible to us at the moment it fires. Bands are hatched where
they rest on use rather than sighting, and a question like "was he carrying an unauthorised weapon"
is answered against the strip, with the honest note that a weapon never fired may never be seen.

**5.14 Damage adjustments.** `ρ = damage/initial` per event, attributed to scripts via
`modifications[].task_id`, aggregated per script, avatar, team and damage type as a distribution
(median + IQR + n), never one number. The important cases are named: **complete immunity**
(`new_damage 0`) and reduction. `task_id` resolves to an object whose owner/creator separates the
region's experience attachment from a combatant's own HUD. The Script article prints the **pattern**,
which is the thing officers actually need [owner]:

- immune **at the spawn hub** vs immune **in the middle of the battlefield while dealing damage**;
- immune **bystander** who never dealt or took damage (fine) vs immune **while killing** (suspect).

Each pattern is a Signal page with its false-positive list, not a verdict. The spawn-hub case is
computed against `d_spawn` (§5.7a) and reported with its interval, and the legitimate case is named
first on the page, because the region owner's experience temp-attachment doing exactly this — safe
zones at spawn, non-combatant bystanders — is the *expected* reading and the officer should meet it
before the suspicious one.

Two written rules attach here as measurable claims ([rules §5.22](combat_log_rules.md)), and they are the reason this section
exists at all rather than being a curiosity [rules]:

- **360° body armour may not exceed 25 % damage reduction.** Directly `1 − median(ρ)` per victim per
  script, over hits from ≥ 2 distinct attackers and ≥ 2 damage types, so a single directional plate
  is not mistaken for a 360° one. Reported as a distribution with n against the region's configured
  cap, with the cap sweepable; the candidate row reads "Ward-7 on Kai: 0.38 reduction [0.31–0.44],
  n=41, cap 0.25" and stops there.
- **Healing is limited per life or by recharge.** Negative-damage type-100 events grouped per life,
  giving heals per life, total healed, and the interval between consecutive heals against the
  region's configured recharge. Self-heals and heals received from a medic are separated by `owner`,
  because the rules differ and so does the officer's interest.

Both are unusual among rule checks in being *directly* measurable from the log rather than inferred
from geometry, which is why the adjustments surface — invisible today — is the highest-value thing in
this document for an officer who does not care about the levels [owner].

**5.15 Awareness lead** [01]. A free prefilter runs first: yaw within θ_a of the victim's bearing for
≥ 0.5 s before the first hit, from track samples with **no raycasting at all**. Only kills that pass
spend casts — a 6 s backward window at 0.5 s steps, ≤ 12 casts — to find the earliest tracked instant
whose LOS was blocked, minus the time LOS first cleared. Positive means tracking through geometry.
Yaw-only, no pitch, 2 Hz sample floor, and it **must be swept over θ_a at R2 before it may be cited**.

**Why yaw, when the domain's tell is elevation.** The give-away officers describe is *elevation*
tracking — a head following a target up a staircase through the building between them — not precise
bearing [owner]. That is the better signal and we cannot have it: neither viewer updates nor the
bridge's `llGetAgentInfo` tick exposes another avatar's pitch or gaze, only body yaw, and body yaw is
not head yaw. So the tool substitutes the closest observable — body yaw plus movement heading versus
the occluded target's bearing, sustained, with shots into the occluder counted beside it — and the
Signal page says so in its confound list, because a proxy that does not announce itself is how a
weaker signal gets quoted as the stronger one. It is why duration and occluder count carry more
weight here than the angle.

**5.16 Reaction interval.** First-clear-LOS to first hit, on the same 0.1 s τ-grid as §5.11 and
sharing its cache, so a reaction interval costs one LOS sweep and no more. Reported as a percentile
of the session's own distribution, never against an absolute threshold, greyed where the stamp is
weak, and with an explicit **"no reaction cue available"** state for a clean ambush [12] and an
explicit **"insufficient track density to assess"** state that is distinct from "checked, nothing
anomalous" [06].

**5.17 Contact and Exposure** [01][08], two-tier and never a discovery sweep. **Contact** (default,
zero raycasts): fraction of a life with a hostile inside observed weapon range and inside the
subject's broad frontal arc, from tracks alone at 2 Hz, labelled "geometry not tested". **Exposure**
(on demand): raycast-confirmed clear LOS to ≥ 1 hostile, 0.5 Hz anchors, three nearest hostiles,
capped at ~600 casts per life, filling behind a hatch, with the cost printed on the button before it
is pressed. Continuous session-wide exposure is not offered at any price; the strip says where it is
uncomputed.

**5.18 Motion plausibility.** Max/mean speed against the bridge flags (flying, on-object, sitting),
from BRIDGE/VIEWER samples only, shown as a distribution with the combatant's percentile, never as an
accusation [12].

**5.19 Behaviour signals are a filter, not a rank, and they ship switched off** [12][owner]. §5.15
through §5.19 sit behind one preference, off by default, and with it off the Roster face is absent
rather than empty ([ux §3.6](combat_log_ux.md)). The design answer to misuse is everything below; the owner's answer is that
the design answer should not have to be enough on the first day. No summed score. One column per signal with
its own raw count; `thin` below n ≈ 8, excluded from ordering; interval printed before the point
estimate; explicit peer-set rule with n printed beside every percentile [08]; a printed,
explicitly non-exhaustive confound list [11]. Nothing here is a verdict and no cell is ever red.

**5.20 Witness and anchor** [01]. The **witness** is the medoid instance under the aggregate's own
metric (nearest the matrix cell's centroid; the death nearest the bout's space-time centre), with
min/max extremes on `[`+Left/Right. The **anchor** is the instance you last stood on beneath this
subject, written by `]`, read by `[`. Together they make zooming in deterministic *and* lossless.

**5.21 Cost and caching — one contract for the whole tool** [01][11].

- The hostility graph, volleys, per-window damage totals, coverage, and per-avatar per-signal counts
  are **accumulators updated on event append**, O(1) per event. **No paint tick ever touches the
  event vector.** Matrix, Braid, Roster and the ribbon read the cache. Per-cell sparklines compute on
  hover only, from a binary search into the event vector.
- Snapshot team solves: ≤ 100 nodes every 10 s, run twice (tag / no-tag) plus 20 jittered re-solves
  amortised across frames, one solve per frame, off the draw path [08].
- All raycasting shares a **single ~200/frame budget** — a hidden debug setting, not a preference
  ([ux §4.3](combat_log_ux.md)) — with priority `selected > visible rows >
  background`, a visible "computing" hatch and a cancel, plus **LRU memoisation keyed on positions
  quantised to 1 m** — avatars stand still, so the hit rate is high — invalidated on track backfill.
- Front extraction: one 64×64 contour plus chamfer distance transform per vertical level per window,
  cached, 100 ms debounce during scrub-drag [08][11].
- Article rendering: LRU of 32 pages, each recording `(analysis epoch, dep set)`; an override or a
  new claim bumps the epoch of the analyses it touches, stale pages drop, and numbers that changed
  under the officer flash "recomputed" [02].

**5.23 Escalation, auth, and negotiated restrictions** [reports][owner]. **The escalation lane ships
behind a preference and is off by default, and the class set is configurable per group.** How a group
ladders its tiers is undecided and is not ours to decide — the approach itself is still open ([ux §8](combat_log_ux.md)) —
so the lane draws only for an officer who has switched it on and told it what the classes are.
Restrictions below are unaffected and always available. Escalation is a phase
variable, not a decoration: raids begin in low escalation with basic equipment and rise as the OIC
directs, through explosives and crowd control to vehicles, mechs, drones and tanks, and some groups
gate the higher steps behind an armoury authorisation of their own (Ashguard's tier-2 / tier-3 auth
is one such [merits]). The tool computes, per side, the **first attributable use
of each escalation class**, which is a `min(t̂)` over events whose equipment class matches, carrying
that classification's confidence forward (§5.13); those are the brackets on the ribbon's escalation
lane and the R4 escalation row.

**Escalation class is a separate field from `Equipment.kind`, and this is where its extra evidence
comes from.** The plan's `kind` enum stops at WEAPON / PROJECTILE / HUD / DEPLOYABLE / VEHICLE /
MOUNT, which cannot say MECH, DRONE or STAFF — so escalation gets its own `U8 escalClass` resolved
from four signals, never from `kind` alone:

1. **`kind` as the floor.** Nothing is a MECH that is not already VEHICLE-classed, nothing is a STAFF
   that is not a handheld WEAPON. The class can only refine `kind`, never contradict it.
2. **Damage type.** Exotic types are what "staffs and other exotics" means concretely: electric, cold
   and the rest of the named types (a lightning staff is electric, an ice staff is cold), 102
   explosive for EXP, 104 anti-armor as corroboration for the vehicle branch.
3. **Name-family keywords**, matched against the class sets in the group's catalogue ([rules §5.22](combat_log_rules.md)), on the
   maker-prefix-plus-stem key the community already uses (§5.13) — the same list an armory page is
   written from, so it is the group's own tier ladder and not ours.
4. **Seat and motion evidence** for the vehicle branch: seated-avatar count from the tracks, whether
   the object flew (`FLYING` / sustained height above ground), and its observed scale where the
   viewer saw it — which is what separates a drone (no seats, moved, small) from a light vehicle
   (seated, moved) from a mech (seated, walked rather than flew, large).

Confidence is the weakest of the signals that fired, printed as `●●●…○○○`. **Below `●●●` the bracket
draws hatched and is labelled with the base `kind` rather than the sub-class** — `VEH ▨` where the
evidence supports a vehicle but not specifically a mech — and the officer can re-date or re-label it
as a Claim. Groups whose tier ladder does not match these classes configure their own class sets in
the catalogue; the lane draws whatever the group's tier ladder says (the open question in [ux §8](combat_log_ux.md)). Nothing else in the tool
depends on `escalClass`: it drives the lane, the R4 row and one offered summary fact, and no ledger, count or
rule check reads it. A **restriction** is a Rule with a start time and an equipment family
as its subject — the mid-fight IM negotiation officers actually run ("please ask that basket grenades
not be used", "the sensor shotgun isn't used on Resdayn due to our rules"). The officer files it in
one gesture from the equipment page: `[restrict from now]`, optionally back-dated to the minute the
IM was sent. Its effect is precise and is the entire point: **hits by that family before the
restriction are ordinary; hits after it are listed as rule-breaking rather than merely suspicious**,
which is a different sentence in a report and a different conversation between two OICs. Restrictions
are Claims, so they are attributed, undoable, visible in backlinks, and exported with the officer's
initials and time.

**5.24 The officer's artefacts: session summary, combatant statistics, compliance** [reports][owner][03].

- **The session summary** ([ux §3.9](combat_log_ux.md)) is a first-class export, because some such
  artefact always exists and a tool that cannot fill it will not be used — but its *shape is group
  configuration and the tool ships no template* [owner]. Its sections are listed there; what belongs
  here are the two **notable episode** detectors, both cheap, both offered as one sentence with a
  link and never inserted on their own, and neither carrying any suggestion of what the episode is
  worth. An **outnumbered hold** is a bout where one side's tracked headcount inside the hull stays
  ≤ half the other's for ≥ 5 minutes while it keeps dealing damage and does not lose the ground — the
  shape of "fought 3 v 7 for 20 minutes until reinforcements came home". An **objective capture** is
  *not available in v1*: it needs a designed objective area, so the fact prints **requires
  owner-designed zones (future)** where the sentence would be ([ux §2.1](combat_log_ux.md)). A **long
  survival** — a life far above the session's median with damage taken throughout — is the third and
  is free from the same accumulators.
- **Combatant summary statistics.** The Avatar page grows one section of plain counts, and it names
  no categories, because a group's award or ranking scheme is that group's lore and mapping these
  numbers onto it is group configuration, not a viewer feature [owner]. The statistics are: **events
  attended in session history**, kills, deaths, kill/death, **streaks with their actual gaps**, time
  in contested areas, anti-armor damage dealt to objects, deployments, kills by damage type (with
  distance-at-kill), and time as OIC where that is known.
  **The tool ships no windows and no thresholds of its own** [owner]: a multikill is not counted
  against a 6 s or a 10 s rule, it is drawn as a streak with its gaps — `3 kills · gaps 4.1 s, 5.8 s`
  — and the reader compares it against whatever number their group uses, because the count changes
  with the number and the number is not ours.
  Every count carries its n and its coverage, several are `—` in most sessions, and the section is
  captioned *counts, not judgements*. Three carry a hard scope limit printed on the row itself,
  because the alternative is a number that looks like it spans a career and does not:

  - **Events attended is this session only, for now.** "Attended N of the last M" is a number officers
    keep in a spreadsheet by hand today, but the store is in-memory, single-session and time-bounded
    by the engineering plan, so the tool has no yesterday to count. The row therefore reads
    **"present this raid: 1h45m, 62 active minutes"** and nothing more, captioned *this session only
    — session history is not computed*. **Cross-session data is a later feature** [owner]; the future
    item recorded now is to log the interesting facts of a session — equipment families seen, groups
    seen, region, duration, who was present — as the material a cross-session store would be built
    from ([ux §7](combat_log_ux.md)).
  - **Time in contested areas is future.** *contested* is defined once (§5.9a), but an *area* is not:
    the row prints **requires owner-designed zones (future)** rather than being faked from a grid
    ([ux §2.1](combat_log_ux.md)).
  - **Time as OIC is recorded, not detected.** The OIC is designated per event and can be *taken* by
    another officer mid-raid through the OIC HUD [owner], so it is a list of intervals the officer
    types in, possibly several per raid and possibly overlapping the escalation lane's "as the OIC
    directs" reading. The tool stores `(avatar, from, to)` claims, draws them as a lane under the
    presence lane so a handoff is visible beside the fight it happened in, and never infers a handoff
    from behaviour.

  The one interaction rule that matters: **no statistic may be carried out of the tool without
  opening at least one concrete episode behind it**, which is the step down to the concrete enforced
  as a workflow [03].
- **Own-side loadout is a list, never a flag** [owner]. The framing matters more than the
  data: the page shows **what equipment each of our combatants used this session**, family by family,
  and marks the ones that look like **our own arsenal** — a maker prefix belonging to this group in
  the curated list (§5.9), or a family already seen in our own hands often enough to be one of ours.
  Nothing is marked as wrong. Anything unidentified simply lacks the mark and stands out by
  contrast, which is how an officer notices without the tool accusing. Sorted by user count, with the
  honest caveats printed on it — prefixes are cosmetic and spoofable, an approved item may carry any
  prefix, and a weapon never fired is never seen. No cell is red, no combatant is scored, and the
  list is private to the reviewer ([ux §2.9](combat_log_ux.md)). It is the *low-drama* use of equipment identity: an officer
  checking their own side against their own handbook, which is a check some groups' handbooks ask
  for by name (Ashguard's does [merits]) and others do not ask for at all.

**5.25 Region combat settings, and health estimated from them** [owner]. Script 2 reports the region's
`llGetEnv` combat configuration on handshake and on every region change, and
[ux §3.7](combat_log_ux.md) prints it. Four values are wired into the analysis:

- `restrict_combat_log` decides whether foreign, script-written combat-channel messages can exist at
  all. When it is set, an untrusted event is an anomaly rather than a normal admission-filter case
  (§5.4), and R−1 says so.
- `damage_throttle` and `damage_limit` explain **capped and dropped damage**. A volley whose per-hit
  damage sits on the limit, or whose events thin out under sustained fire, is the region doing its
  job and not a weapon behaving oddly; every equipment anomaly row checks both first.
- `invulnerability_time` is a **legitimate source of post-death immunity** which the adjustment
  analysis (§5.14) excludes by name. Zero damage inside that window after a death is the simulator,
  not a script; without the subtraction the tool would manufacture an immune-while-killing row for
  every combatant who respawned into a firefight.
- `death_action` says whether a death implies a teleport at all (§5.7).

**Estimated health** is what `restore_health` and `health_regen_rate` make possible: start each life
at the restore value, subtract each DAMAGE at its `t̂`, add regeneration between hits and negative
type-100 healing, clamp at the top. It draws as a hatched band with an interval, never a number, and
its uncertainty is named — unobserved LBA damage, damage dropped under throttle, adjustments whose
`ρ` is itself a distribution, and a respawn moment we can only infer. It feeds two places: the
**Life** noun, where the shape of a life becomes legible for the first time, and the
**reconstruction** ([ux §3.10](combat_log_ux.md)), where a bar over the victim's ghost is the thing
an officer is trying to imagine anyway. **It is a reading aid**: no rule check and no candidate row
may be built on it, because "he should have been dead" is exactly the sentence an estimate must not
be allowed to write.
