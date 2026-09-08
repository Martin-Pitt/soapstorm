# Completeness critique — combat_log_ux.md against BRIEF.md and the engineering plan

Reviewed: `doc/combat_log_ux.md` (1768 lines) against `doc/combat_log_design/BRIEF.md` in full
(including the six domain sections — SLMC domain facts; Sides, roles, rules; How officers actually
report; How one SLMC group is organised; Written sim rules; Promotion criteria and conduct rules —
plus the later Cross-group rule variance, physical-bullets and raycast-weapon sections) and
`C:\Users\nexii\.claude\plans\glistening-toasting-pebble.md`.

The design is unusually disciplined about uncertainty and about calling out its own open questions
and plan deltas. Most of what follows is genuine gaps, not nitpicks; a few are internal
inconsistencies the document would catch itself if it cross-referenced its own sections.

## Fatal

None found. No domain fact is flatly contradicted in a way that breaks the tool's core claims, no
officer question is left unanswerable, and no screen is structurally impossible in XUI. The issues
below are serious enough to fix before Phase 5 is coded, but none invalidates the design's thesis.

## Serious

1. **Merit "attended N of the last M events" has no computation path under the in-memory,
   single-session architecture.** §5.24 lists "raids and defences attended (session history —
   'attended N of the last M events' is a number officers compute by hand today)" as an Avatar-page
   merit observable, and the promotion walkthrough (line ~1383) states it flatly: *"Besieger:
   attended 7 of the last 9."* But both the brief ("Data is in-memory for the current session (no
   files in v1)") and the plan ("In-memory only for v1 (no session files). Retention is
   time-bounded.") fix the tool's scope to one region, one session, with no persistence across
   raids. Nothing in the design explains how "last 9 events" would be known once the viewer session
   ends or the retention sweep runs. This is presented as a working feature in a walkthrough, not
   flagged as an open question the way the multikill window is (§8 Q15).
   **Fix:** either scope this observable explicitly to "this session only" (and say so on the page,
   since it's nearly useless at n=1), or add it to §8's open questions alongside Q15/Q5 as requiring
   an opt-in cross-session store, and strike the confident "attended 7 of the last 9" line from the
   walkthrough until that's resolved.

2. **Escalation-class granularity (VEH vs MECH vs DRONE) is presented as solved in the mockups but
   is an acknowledged unknown in §8.** The R4 mockup and the Q1 walkthrough draw distinct "EXP",
   "VEH" and "MECH" brackets on the escalation lane as if the classifier reliably tells a mech from a
   generic vehicle. But the plan's `Equipment.kind` enum has only
   `WEAPON/PROJECTILE/HUD/DEPLOYABLE/VEHICLE/MOUNT/UNKNOWN` — no mech/drone/staff subclass exists
   anywhere in the data model — and §8 Q17 admits exactly this: *"Does Ashguard's tier system map
   onto those classes, or does the lane need the tiers themselves as configurable class sets per
   group?"* The escalation lane's whole value proposition (distinguishing "went to tier 3" moments)
   depends on a classification granularity that is explicitly unresolved. The walkthroughs should not
   show it as already working.
   **Fix:** either specify the extra classification signal that yields mech/drone/staff sub-kinds
   (e.g. object scale + seat count + HP-class heuristics), or downgrade the mockups/walkthroughs to
   show only the classes the current `Equipment.kind` enum actually supports (explosive, vehicle,
   mounted-deployable) until Q17 is answered.

3. **"Contested" is used as a load-bearing predicate but is never defined.** "Objective episode"
   (§5.24, §3.9), the Flagbearer/Pointman merit mapping, and the Ashguard courtyard beat all key off
   "time inside a named objective Zone while contested" — but no section defines what makes a Zone
   contested (both sides present? recent damage exchanged inside it? within N seconds of a death
   there?). Every other derived quantity in §5 states its formula; this one doesn't, despite feeding
   a promotion-relevant statistic.
   **Fix:** add a one-line definition to §5.24 or a new §5.x, e.g. "a Zone is contested in a window if
   avatars from ≥ 2 bands were inside it simultaneously for ≥ N s," with the same n/coverage
   treatment every other signal gets.

4. **The rule catalogue is framed as "data, not code" but Appendix A shows each rule needs bespoke
   measurement logic, which undercuts the brief's cross-group portability requirement.** §5.22's
   `Rule` struct holds text, a `ParamSet` of numbers, a validity window and exceptions — but the
   actual *test* behind "measurable" or "partial" (rez-to-first-shot interval; burst-cadence
   detection; DBSCAN over landing points; distance-to-blast-point; etc.) is a different bespoke
   function per rule, visible as 30+ distinct "what the tool measures" cells in Appendix A. A new
   group's ruleset with a genuinely new rule *shape* — e.g. Grand Federation's "one superweapon per
   side per hour after 60 minutes of attacking," which appears in the Cross-group rule variance
   section but nowhere in the design — cannot be authored by an officer typing numbers into a form;
   it needs a programmer to write a new measurement function. That directly weakens the brief's own
   "Consequence for the design: … Sim owners should be able to export and share their catalogue."
   Only numeric-parameter tweaks to already-coded rule *shapes* are actually data-driven; new rule
   shapes are not.
   **Fix:** say so plainly — the catalogue is data for parameters, code for predicates — and name the
   fixed set of predicate *shapes* the engine supports (threshold-vs-distance, count-per-owner-cap,
   rate-per-window, sequence/recharge, zone-membership) so it's clear which of a new group's rules
   can be typed in versus which need an engineering change. Consider whether "one superweapon per
   side per hour" fits an existing shape (it doesn't — it's a rate cap with a delayed start) and note
   it as a known catalogue-shape gap rather than silently dropping it.

5. **Friendly-fire exclusion from team-clustering evidence is circular with the team assignment it
   depends on, and the resolution order is unstated.** §5.10 says "Friendly fire is excluded from
   team evidence so it cannot split a team," but friendly fire is *defined* (same section) as
   "damage where attacker and target share the window's assignment" — which requires the team
   assignment §5.9 is trying to produce. Nothing in §5.9's ordered list (region-system seed → tag
   seed admission → spawn-cohort prior → tag-free second solve → three-outcome labelling → overrides)
   says where FF-exclusion sits, or whether it's a second pass using the previous window's labels, a
   fixed-point iteration, or something else. Every other place the document has an ordering
   dependency (tag prior vs. tag-free solve, spawn-cohort admission) spells out the exact sequence;
   this one doesn't.
   **Fix:** state explicitly, e.g. "solve once on the full damage graph, label FF edges from that
   result, then re-solve on the FF-stripped graph for the window actually shown; converge in at most
   one extra pass," and note what happens if the two solves disagree (presumably the same
   tag-dependence-style flag as §5.9's tag/no-tag disagreement).

6. **Peek-depth headline numbers on the R4 stage violate the document's own "no bare rate, no fact
   without its uncertainty" rule, exactly where the officer is least likely to drill down.** §2.8
   states the global rule ("Quantities with a real denominator print as intervals with n. Never a
   bare rate") and the whole document is built around never showing a naked number for anything
   inferred. But the R4 mockup's flagged panel prints `wall-kill susp. 5 ▇▇░░░░░░` as a plain count
   with no n, no interval, no measurability chip — on the exact stage (§2.9's "Peek," "readable in
   about five seconds," the default view for a fighting OIC) that is designed to be read *without*
   opening anything. A skimming OIC reading "5" as "5 proven wall kills" is precisely the
   misunderstanding the rest of the document works hard to prevent.
   **Fix:** either give the Peek-mode count the same treatment as everything else (e.g. "5 candidates,
   2 FIRM"), or state explicitly that Peek-depth numbers are deliberately exempted from §2.8 as a
   size/cost tradeoff and explain why that's safe (e.g. word "susp." itself is the hedge) — currently
   neither is said.

## Minor

7. **Vehicle "respawn timer" measurability is both inconsistent with the brief and internally
   overclaimed.** The brief's General section parenthesises "vehicles must have respawn timers
   (annotate)"; Appendix A's General table classifies the same rule **P** via "rez-to-rez interval per
   vehicle if the same object identity is seen twice." But a respawn timer, by definition, measures
   the interval from *destruction* to *reappearance*, and Object health/destruction is stated
   elsewhere in the same document (§5.7, and Appendix A's own Vehicles row two lines below) as
   unobservable because LBA is invisible to the combat log. A rez-to-rez interval between two vehicle
   sightings could just as easily be a second vehicle being pulled out, unrelated to any respawn
   rule. The **P** classification here is not actually backed by a computation that tests the rule.
   **Fix:** either drop this row to **A** to match the brief, or specify what makes an observed
   rez-to-rez gap trustworthy evidence of a respawn cycle specifically (e.g. same object identity plus
   a nearby DEATH/OBJECT_DEATH-adjacent event) rather than an unrelated re-rez.

8. **The domain's own identified wireframing "tell" — elevation tracking — is silently replaced by a
   weaker yaw-only proxy with no stated reason.** The brief says the giveaway for wireframing is
   "elevation tracking rather than precise bearing," but §5.15's awareness-lead signal is explicitly
   "Yaw-only, no pitch." This is almost certainly the right engineering call (avatar body pitch isn't
   meaningfully tracked, and neither is another player's camera pitch), but the document never says
   *why* it diverges from the domain fact's own description of the tell, which reads as quietly
   dropping a piece of the brief rather than making an informed substitution.
   **Fix:** add one sentence to §5.15 stating that elevation-of-gaze isn't observable for other
   avatars (no pitch in viewer updates or bridge ticks), so the yaw+movement-heading proxy is
   understood as the closest available approximation, not an oversight.

9. **"Motes" — the pay/bonus vocabulary the brief explicitly asks the tool to speak — is never
   mentioned**, even though the tool already computes the exact two episodes the brief says earn
   mote bonuses (capturing an objective near the enemy spawn → the objective episode; holding out
   while outnumbered → the outnumbered-hold episode). The vocabulary list in "How officers actually
   report" names escalation/auth/tier, last life, combat cuck/randoms, motes and commendation as
   terms the tool should speak; all but motes get a design home.
   **Fix:** note in §5.24 or §3.9 that outnumbered-hold and objective episodes are exactly the acts
   groups pay motes bonuses for, so the Report beats can be captioned in that language when an
   officer's group uses it.

10. **The physical-bullets domain section's explicit display requirement — show region time
    dilation alongside any shots-fired/hit-rate statistic — is absent**, and "shots fired" as a
    quantity distinct from "hits" barely exists in the design (only via ghost-projectile flight
    counts, §5.5). The domain fact is specific: *"a fired bullet that produces no DAMAGE is not
    evidence of a miss under lag, so 'shots fired vs hits' per weapon must be shown with the region's
    time dilation alongside it."* No sim-dilation field appears anywhere in the data model, ribbon,
    or equipment ledger.
    **Fix:** either add a per-weapon "hits vs. observed flights" comparison with a dilation/RTT
    caveat where ghost-projectile coverage allows it, or note explicitly that "shots fired" is out of
    scope for v1 because only hits are logged, and drop the implied comparison.

11. **OIC handoff mid-raid is an unaddressed domain fact that touches both the escalation lane and
    OIC-tenure merit stat.** "How one SLMC group is organised" states an OIC can be "taken" by
    another officer mid-raid through the OIC HUD, and that officers sometimes OIC for the opposing
    side. The escalation lane's "as the OIC directs" framing and §5.24's "OIC tenure where the
    officer records it" both implicitly assume one continuous OIC per side per raid.
    **Fix:** either explicitly scope OIC tenure as "manually recorded, may include multiple named
    intervals," or note it as a known gap; no computation is needed, just an acknowledgement that a
    handoff isn't detected automatically.

12. **Q2's walkthrough never steps back down to a concrete instance**, unlike Q1, Q3, Q8 and the
    officer-job walkthroughs, which all demonstrate the "never get stuck in the clouds" principle by
    grounding the abstract reading in a real event. The team/friendly-fire walkthrough stops at the
    R3 Braid/Matrix reading (drift chevron, STRAINED %, tag-dependence flag, a sweep of θ) and never
    clicks through to see one actual friendly-fire death, so "how sure are we, where did friendly
    fire happen" is answered in the abstract only.
    **Fix:** add one sentence continuing the Q2 walkthrough down to R0/R1 on one of the flagged `!`
    cells, matching the pattern every other walkthrough uses.

13. **The multikill-streak window is shown as a settled "6 s" in the promotion walkthrough while §8
    Q15 lists it as an open, undecided parameter** ("Multikill needs a window (6 s? 10 s?)"). The
    walkthrough's "three multikills in a 6 s window (window printed, because it is a choice)" reads as
    if 6 s is the shipped default; nothing marks it as provisional pending the owner's answer to Q15.
    **Fix:** either pick 6 s as the shipped default and remove it from the open questions, or mark the
    walkthrough's number as illustrative/provisional.

14. **Scale and coherence risk: ~25 distinct noun types and up to five faces per rung sit uneasily
    against the brief's explicit "prefer few, deep, composable views over many shallow ones," and no
    onboarding path is designed for the ladder's own vocabulary** (rung rail, anchor vs. witness,
    breadcrumb vs. View chain, `[`/`]` semantics). §2.9's Peek/Study split addresses time pressure,
    not the learning curve — a first-time spectator or a non-technical individual combatant (the
    document's own stated most-common user by headcount) has no stated way to discover what "witness
    (back)" on the rung rail means, or that `[sweep this]` exists, before using the tool.
    **Fix:** not a redesign — add a short "first five minutes" note (a legend panel, or the rung
    rail's own hover text carrying a one-line explainer) so the composability the design already has
    is discoverable without reading this document.

15. **XUI feasibility risk: heavy reliance on niche Unicode glyphs as literal in-widget text**
    (⚑ ✓ ⇄ ✎ ▨ ◇ ▣ ● ○ and box-drawing shades used inside scroll-list cells and breadcrumb chips, not
    just in the ASCII wireframes) is never checked against `LLFontGL`'s actual glyph coverage on the
    viewer's font stack. Most of the ASCII art is explicitly custom-painted (§3.10 says so), which
    resolves the bulk of this concern, but labels like `⚑mark`, `✓ CLEAR`, `⇄ OVERRIDE`, `✎ NOTE` and
    the band-header `(!)` flag read as literal text glyphs inside stock or near-stock widgets
    (breadcrumb `text`/`button`, backlink lists), where a missing glyph renders as a tofu box on some
    systems.
    **Fix:** confirm these specific literal-text glyphs render in the viewer's default font, or swap
    them for small icon textures the way the rest of the overlay already uses colour/geometry instead
    of glyphs.

## Strengths to preserve

- The vertical ladder / horizontal wiki split, with three distinct, consistently-drawn navigation
  mechanisms (noun chain, View chain, anchor-vs-witness), is a genuinely disciplined answer to the
  Bret Victor brief and is applied uniformly across every screen, not just asserted once.
- The uncertainty machinery (haze rule, intervals-with-n vs. ordinal bands, Signal pages with
  confound lists, "never a bare rate") is specific enough to audit, and mostly follows its own rule —
  §6's finding above is the exception, not the pattern.
- The rule catalogue's three-tier measurability class (MEASURABLE/PARTIAL/ANNOTATE), printed on every
  candidate list including empty ones, and Appendix A's rule-by-rule mapping against the actual
  Ashguard document, is the single most load-bearing and best-executed piece of domain grounding in
  the document.
- The delivery-classification taxonomy (hitscan/projectile/lobbed-arc/area) and the dual eye/camera
  LOS test for alt-cam aiming translate the raycast-weapon research precisely into a testable
  mechanism, including the specific "damage prim teleports onto the victim" hitscan signature.
- The Report face reproducing the actual forum-post shape field-for-field, the "no verdicts, only
  candidates" stance, and the mandatory blindfire-context strip before a wireframing card can be
  pinned, are exactly the low-drama, non-accusatory posture the owner and the raid-report scan both
  ask for.
- The self-auditing sections — "Deltas to the plan's frozen contracts" and the eighteen open
  questions — proactively surface exactly the kind of plan conflicts and unresolved parameters this
  review looks for, which is why most of the findings above are refinements rather than surprises.
- The intra-group-skirmish and FFA walkthroughs (tag admission tests, tag-free second solve,
  STRAINED meter, DEATHMATCH detection with band-greying) are concrete, falsifiable mechanisms that
  match the brief's two hardest team-detection scenarios rather than hand-waving past them.
