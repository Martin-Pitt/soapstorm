# Attack: Comprehension — Entry 02 "Hypertext-first: the raid is a wiki"

Lens: does an officer actually move up and down a ladder of abstraction, or is this a pile of views wearing a wiki costume? Does it survive real freeform combat? Is uncertainty honest? Is the overlay usable while alt-camming?

## Verdict up front

This is the most structurally serious attempt at "digging without losing the thread" I'd expect to see: one page skeleton, real backlinks, a persistent trail object, and a sweep facet that actually implements Victor's "abstract over parameters" rung with a concrete payoff (the LOS travel-window flip in the Q3 walkthrough is the single best moment in the design — it shows the ladder doing real epistemic work, not just navigation). That mechanism should survive into whatever gets built.

But the "ladder" is oversold. What's actually built is a **lattice of three independent hierarchies** (entity: event→avatar/equipment→team; time: moment→span→session; space: moment→place→session) forced into a single linear rung rail with one row of buttons. That seam shows up exactly where the brief's hardest cases live: freeform allegiance drift, mid-session team changes, and equipment that has no "team" to step up to. Below is where it breaks.

## Walking the officer questions myself

### Q1 — "How did this raid go?"

Session page → phase ribbon → click P2 → phase page with animated front polyline → click hottest cell → place page → click a death → rung 0.5 reconstruction. This works, and it's a genuine up-down traversal: whole-raid to single hit in four clicks, each one a link, each one leaving a breadcrumb. This is the design doing exactly what it claims. No complaint here except one: the session page's figure ("[figure: 512 m top-down. death dots by team colour, heat cells, front polyline per phase, spawn rings, click anything]") is described as *the* rung-4 illustration, but the doc never says whether it is the **default overlay content when nothing is pinned**. If it isn't, an officer who alt-cams to the courtyard before opening any specific page sees an empty world — see the alt-cam finding below.

### Q3 — "Why did X die at 14:32?"

This walkthrough is the strongest evidence for the design. Avatar page → Deaths section → death page → sweep facet on LOS → discover the BLOCKED verdict flips to CLEAR at travel=0.9s → click that column → world shows the flipped geometry. That is a real step up (from one blocked verdict to the shape of the *claim's fragility*) and a real step down (from the fragility back to one concrete geometry). Keep this exact interaction; it's the design's thesis proven in one paragraph.

The complaint: the fragility is discovered only if the officer *opens the sweep facet*. The Death page's default, always-visible line reads "LOS BLOCKED (low conf)". An officer skimming (which the brief explicitly wants supported — "quickly skim over it, or stop at interesting events") will read "blocked, low confidence" and move on, never learning that the entire verdict depends on a travel-time assumption that flips at 0.9s. For the exact case the brief calls out by name ("taking into account false positives due to bullet travel time"), the headline claim should either state the flip point inline ("BLOCKED for travel <0.9s; if actual travel time is longer this becomes CLEAR — 5/6 candidates") or the confidence chip should encode fragility, not just a probability-shaped number. As written, the honest information exists but is opt-in, which is the definition of decorative uncertainty at the level an officer actually reads.

### Q8 — "Did anyone behave suspiciously?"

Session → Anomalies → why-page for pre-aim fraction, with the false-positive list spelled out (travel time, log lag, 2 Hz sampling, minimap is legal, sound cues) and "Show me a case" dropping to a concrete moment. This is honest — it explicitly refuses a verdict and shows n=12 with named counter-explanations. This is the correct posture for a comprehension tool and should be kept as the template for every suspicion-flavoured page (through-wall rate already does this with a Wilson interval; pre-aim should be forced to the same rigor rather than a chip).

One gap: the walkthrough never shows what happens when the officer *disagrees* — is there a way to record "reviewed, not suspicious, sound cue confirmed by voice chat" as a durable answer, distinct from a generic claim note? The `claim` page type exists for this, but nothing distinguishes a dismissal from an accusation in the "what links here" list on the avatar's page — both would show up identically as `ss:claim/N`, which means six months later a promotion reviewer sees "3 claims" on someone's page and cannot tell from the backlink list whether those are exonerations or flags without opening each one.

## Freeform combat, allegiance drift, mid-fight arrival

The FFA and intra-group scenarios are both addressed head-on, which is more than most designs bother with, and the fallback mechanism (hostility − restraint − co-movement, seeded by group tag only when informative) is genuinely well thought through. Two real failures remain:

1. **Team overrides are not time-scoped.** The brief is explicit: "Team membership can change during a session... present allegiance as an inference with confidence, never a hard fact, and let the officer override." The design's Team page shows "Members (18, each with confidence bar + [override])" — one override value per avatar, applied to the whole session. The intra-group walkthrough itself needs this to be wrong: it describes six avatars whose alignment is ambiguous across the *whole* session, not six avatars who *switched sides partway through*, which is the harder and more realistic case (a defector, a raid where a squad reassigns after a respawn). Nothing in the data model or the Team page supports "Red until 14:20, then Blue." This is a direct, checkable gap against the brief's own wording, not a nice-to-have.

2. **No support for transient local alliances in a true FFA.** When clustering finds no stable global partition, the design falls back to "colour by recent aggressor" and pushes the officer to Duels and Places instead of Teams. That is a reasonable admission that global teams don't exist — but it silently gives up on the case the owner explicitly named: "it may become a deathmatch where everyone fights everyone" *or* group allegiances "may drift" mid-FFA, meaning two or three people cooperate for a few minutes inside an otherwise leaderless brawl. The design has no page type for a short-lived, spatially/temporally local alliance — only global Team (session-wide clusters) and pairwise Duel (two people only). An officer trying to understand "did these three actually team up against that guy for two minutes" has no destination to dig into; they'd have to manually cross-reference three separate Duel pages and eyeball correlated timing themselves, which is exactly the "pile of views, do the synthesis yourself" failure mode this design is supposed to avoid.

Mid-fight arrival itself is handled fine by construction — Duel pages accrete facts as they happen, phases recompute via change-point detection, nothing requires an avatar to have been present since t=0. No real gap there.

## Is it a ladder, or a pile of views with breadcrumbs?

Mostly a real ladder, with one structural crack: the rung rail in §4.1 shows a single row — "Moment | EVENT | Duel | Career | Phase/Place | Session" — implying one line the officer climbs. But:

- **"Career" (R2) covers both avatar and equipment/model pages**, and the table's stated step-up for R2 is "its team, its phase breakdown." A Model (weapon type) has no team to step up to — a weapon isn't on a side. The design never resolves what "up" means from a Model page; §4.4's Model wireframe shows sections (Instances, Users, Kills, Verdicts, Adjustment) but no stated step-up target at all, breaking the very table that defines the ladder.
- **"Phase/Place" is presented as one rung slot**, but a phase (a time interval) and a place (a spatial cell) are answers to different questions and don't have a natural ordering relative to each other — a phase can span the whole map, a place can span the whole session. Collapsing them into one button in the rung rail will produce a UI where clicking "Phase/Place" from a Death page is ambiguous about which one you get, or requires a hidden disambiguation the doc never specifies.
- From a **Death page**, "step up to Career" is ambiguous between the victim's career and the killer's career (Duel is unambiguous — both — but Career is not). The doc doesn't say which the single "Career" rail button navigates to, and either choice loses the other party.

None of this breaks the overall reading experience (links still work, breadcrumbs still work), but it means the "rung rail" as drawn is not the clean single ladder the design claims — it's a projection of a multi-axis graph onto one row of buttons, and the seams show at exactly the pages (Model, Death) that matter most for the brief's hardest questions (Q4 wall-kills, Q3 death attribution).

## In-world overlay while alt-camming

The per-page-figure model is coherent and the encoding table (colour=team only, direction=geometry, width=evidence quality, alpha=age, glyph=state) is disciplined and matches the plan's existing style rules. Two concrete usability problems for an officer who is, per the owner's own framing, "primarily... standing still and alt-camming":

1. **No stated default/ambient layer.** The overlay draws "the union of the active page and the pinned pages." If an officer alt-cams to an arbitrary spot in the region out of spatial curiosity — the exact behaviour the owner is designing the overlay *for* ("good UX for showing logs overlayed in-world instead of just through a 2D interface") — and has not first opened or pinned a page that covers that location, nothing draws and there is nothing to click. The click-to-navigate mechanic is elegant but depends on markers already existing at the point of interest; the design never states that the Session figure is always-on by default, so the overlay is navigation-second, exploration-second, in a use case the owner described as camera-first.
2. **Overlap and scale mismatch when multiple pages are pinned.** Pinning a Session figure (region-wide heat cells, spawn rings) alongside a Duel figure (two trails, tight local exchange lines) and a Model figure (all shots by one weapon across the whole session, faint) means the overlay simultaneously renders a whole-region abstraction and a metre-scale reconstruction with no stated priority, dimming, or focus rule. The plan's style guide (one named colour per meaning, alpha for age) doesn't cover *scale* conflicts between simultaneously-pinned pages, and the design doesn't say what happens when a Model's "all shots, faint" layer (potentially hundreds of lines across the map) visually competes with the currently-open Death page's six-candidate LOS fan the officer is trying to read closely. This is a legibility risk specific to alt-cam, where the officer can't easily reframe by walking closer along a path — they're flying a free camera through a cluttered scene.

Neither of these needs a redesign — an "always show the session base layer, dim pinned layers outside a scale-appropriate radius" rule would fix both — but as written they're unaddressed, and the brief flags "the world shows tracks and events at the cursor time" as a first-class requirement the design should have closed the loop on.

## Uncertainty: honest where it's forced by statistics, thin where it's just a number

The through-wall rate (Wilson 90% CI, UNKNOWN counted separately) and the pre-aim why-page (explicit false-positive list) are the gold standard here and should be the template applied everywhere else. But the ubiquitous "confidence chip" (team confidence 0.81, 0.93, 0.58) is a bare margin-of-affinity number dressed as a probability. Nothing in the analysis model (§6) says these numbers are calibrated, bounded, or comparable across contexts — a team confidence of 0.81 and an LOS confidence descriptor "(low conf)" for the same death page use entirely different scales and the officer has no way to know that. Two decimal places on an uncalibrated clustering margin is false precision, which is the opposite failure mode from decorative uncertainty but just as dishonest: it invites an officer writing a promotion or violation report to quote "confidence 0.81" as if it meant something statistically comparable to the Wilson interval two sections over. This should be flagged as a real risk in §8 (it currently isn't) — the doc's own §8 risk list is otherwise unusually self-aware (address stability, backlink memory, auto-prose overconfidence, clustering churn, override conflicts) but misses this one about its own confidence numbers.

## What must be kept

- The single page skeleton (lede / infobox / figure / sections / backlinks / facets) applied uniformly — this is the actual mechanism that prevents "pile of views."
- Real backlinks via a maintained reverse index, growing as the officer investigates — this is the wiki metaphor earning its keep, not just decoration.
- The sweep facet as literal Victor rung 2, with a genuine payoff in the Q3 walkthrough (LOS flip at travel=0.9s). This is the design's best idea; do not cut it under implementation pressure.
- The named, promotable trail object doubling as the briefing/dossier artefact — directly answers Q9 without inventing a separate "report builder."
- The through-wall rate's Wilson CI + separate UNKNOWN count, and the pre-aim why-page's explicit false-positive list — apply this rigor to every suspicion-flavoured quantity, not just these two.
- FFA fallback to "colour by recent aggressor" + Duels/Places as the working nouns when no stable team partition exists — the right instinct even though it needs a "small local alliance" noun added.
- The restraint signal (opportunities to hit with zero damage) as the counterweight to friendly-fire-as-team-splitter in the intra-group case — this is the one piece of the clustering model that specifically survives the brief's hardest scenario (two teams, same group tag).

## Summary of what must change

1. Make team overrides time-scoped (an override is "X was on team T from t0 to t1"), not a single session-wide value — required by the brief's own wording on allegiance drift.
2. Add a noun for a transient, local, sub-session alliance in FFA/neutral-sim contexts, or explicitly scope the design to say local alliances are out of v1 (currently it's silently absent).
3. Resolve the rung-rail ambiguity for multi-subject pages (Death's "Career" target, Model's missing step-up, Phase/Place's forced single slot) — either split the rail into per-axis controls or state the disambiguation rule.
4. Surface LOS/verdict fragility (the sweep result) in the default lede for decisive claims, not only behind an opt-in facet.
5. State whether the Session figure (or some ambient layer) draws by default with nothing pinned, so alt-cam exploration has something to click before the officer has navigated to a specific page.
6. Add a scale/priority rule for simultaneously pinned overlay figures at different spatial scales.
7. Either calibrate/bound the confidence-chip numbers or downgrade them to qualitative bands so they aren't mistaken for the same kind of statistic as the Wilson-interval claims elsewhere, and add this to §8's risk list.
