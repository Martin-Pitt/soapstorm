# Attack on Entry 09 — Minimal Progressive Disclosure — COMPREHENSION lens

## Verdict up front

The ladder is real and the rung-transitions are mostly well-specified — this is not a pile of unconnected views. But the single-page, destructive-pop navigation model that the entry adopts as its "minimal" thesis directly contradicts the brief's own digging requirement ("digging never loses the thread," backlinks, history) and will cost officers real work in exactly the investigative sessions (Q4/Q7, promotion review, violation review) where they need to hold two or three threads at once. There is also a load-bearing gap: no equipment/team-composition index page, which breaks Q7 and weakens Q4. And the Q3 walkthrough quietly assumes a capability (click any historical death directly in-world with "no floater interaction") that section 5's own overlay spec does not support for the general case. Team clustering is described as if it can represent an avatar switching sides mid-session, but nothing in the analysis model or UI actually shows *when* a membership changed — a real hole given the brief calls out drifting allegiance by name.

## Walking the officer questions

### Q1 — "How did this raid go overall?"

The stated path (main floater → Range → drag-select hot zone → histogram → click peak → rung 0) works and is genuinely a helicopter-view-to-instance journey, which is the whole point of the ladder. Two real snags:

1. **Range mode's central visual (team colour) degrades exactly when Q1 is hardest to answer.** On a classic raid with two clean clusters, "two colour blobs converging on a courtyard" is legible. On a neutral/FFA sim — which the brief explicitly requires the design to survive — the Team page is allowed to say "unassigned is the largest group," but the entry never revisits what Range mode *draws* in that case. If team confidence is near 50/50 for most avatars, either everyone renders in the same washed-out grey (in which case the rung-1 picture that's supposed to answer "between whom" answers nothing) or the design silently falls back to a per-avatar palette that section 5 never mentions. This is a gap between the FFA walkthrough (which lives entirely in the 2D Team/Avatar pages) and the in-world overlay claims made for Q1 — the two aren't reconciled.
2. **"Where did the fighting happen, when, between whom" is three questions and the Range view only cleanly answers the first two.** "Between whom" requires trail colour to mean something; see above.

### Q3 — "Why did X die at 14:32?"

The described mechanism (attribution chain, LOS verdict, adjustments, friendly-fire check, "show in world" step-down) is the strongest part of the entry and matches the data model well. But the walkthrough's opening move — "Click X's death cross on the rung-1 or rung-0 overlay directly (no floater interaction needed)" — does not follow from section 5 as written:

- Section 5 says rung 0 draws crosses at **recent** death sites (i.e., within the current trail/cursor window), not at an arbitrary historical instant. If the officer is not already scrubbed to near 14:32, there is no cross to click.
- Range mode (rung 1) is only said to draw a **heatmap disc**, not individually pickable per-death crosses across the whole session. Clicking a disc, per the rung-1 spec, opens a Zone page, not a specific Death page.

So the actual path to "why did X die at 14:32" for a cold-start officer who doesn't already know where to alt-cam is: open the main floater → find 14:32 on the scrub strip (there are death ticks there) → click/scrub to it → *then* the cross becomes clickable in-world, or alternatively search from the Session page's event list (which doesn't appear to exist as a nameable, filterable page in the IA — "Top claims" and per-avatar "Events [show all]" are the closest things, neither of which is "look up a death by approximate time"). The walkthrough's claim of a floater-free click-through is either an overclaim or is quietly relying on a general chronological death browser that the information architecture never names as a noun. This matters for comprehension because it's the officer's single most common query ("why did we lose so-and-so") and the design doesn't actually give a clean, few-click answer to it from cold start — only from a lucky in-world click or an already-narrowed time window.

### Q8 — "Did anyone behave suspiciously?"

The three-signal, non-combined suspicion model is honest and matches the brief's instruction to show evidence, not verdicts — this is a keeper. The stated path (Session → Top Claims, or Avatar page → same panel) is fine *if* the suspect avatar is already known. But there's a discoverability gap symmetrical to Q3's: if the officer doesn't have a named suspect and no claim happened to cross the auto-surface threshold, there is no page that lets them scan *all* avatars ranked by any suspicion signal (e.g., "show me the 5 fastest reaction times session-wide"). The Session page's "Top claims" is a curated, thresholded feed, not a sortable list — an officer doing due diligence on a violation report needs to rule things out, not just be told what already crossed a bar. This is a real comprehension gap for exactly the workflow the brief names ("investigating a reported violation," §9 of the brief) where the officer often starts with a complaint about a specific-but-unnamed pattern, not a specific avatar.

## Digging without losing the thread — where the model breaks its own promise

The brief is explicit: "navigation keeps a history/breadcrumb so digging never loses the thread; backlinks show what refers to the thing you are looking at." The entry's single-stack, **destructive-pop** breadcrumb violates this in a way the entry itself half-admits (§8, risk 1 and risk 6) but underweights:

- If an officer digs `Session ▸ Team A ▸ Kira ▸ Death@14:32 ▸ SMG-X ▸ Claim("kills through walls")` and then wants to check a *different* death of Kira's to compare, popping back to `Kira` via the breadcrumb **discards** the SMG-X/Claim branch. To get back to the claim, they re-walk it from scratch. This is worse than a real web browser, which the entry invokes as its own analogy ("matching a browser's back button") — browsers preserve forward history until you navigate somewhere new; this design discards it on pop unconditionally. The analogy oversells the model's comprehension characteristics.
- The pin rail (max 5, oldest evicted) is offered as the escape hatch, but pins are bookmarks to *starting points*, not saved traversal state — pinning Kira does not preserve "I had also opened her SMG-X claim." For a promotion review or violation investigation, which by nature compares several nouns across several digressions, this is a real tax on exactly the workflows the brief names in §9.
- The entry is honest about this cost in its risks section, which is to its credit, but a COMPREHENSION reviewer has to weigh that the *chosen* trade (strict linearity for teachability) actively works against the brief's stated wiki-digging requirement, not just against generic convenience.

## Freeform / drift / arrival mid-fight

- **Intra-group skirmish**: handled plausibly — label propagation on who-shoots-whom rather than group tag, manual rename, threshold slider emphasized. This is a keeper.
- **FFA**: the "unassigned is the largest group" degrade is honest and the right call, but as noted above the in-world Range view isn't reconciled with it.
- **Allegiance drift mid-session** (brief line 20, explicitly required): this is where the entry is weakest. The analysis model computes "a probability vector over inferred clusters" per avatar and a single confidence number, with friendly-fire confidence computed as time-indexed. But nothing in the IA or screens shows an avatar's team assignment **over time**. If Wren joins Ashguard-A at minute 10 and defects to Ashguard-B at minute 40, the Team page's single "Wren (uncertain) 54%" line and the Avatar page's single "Team: Ashguard-A (81% conf) [override]" both imply one static verdict for the whole session. There is no timeline/sparkline of "assigned team by minute" anywhere in the mocked screens, and the manual override ("writes a pinned assignment... hard seed from then on") is explicitly session-wide rather than time-bounded — it can't even represent "correct from minute 40 onward." This is a specific, named requirement in the brief that the design's own analysis model computes the ingredients for (time-indexed confidence) but never surfaces to the officer. Worth calling a serious gap, not a nitpick — it's line-of-sight to a wrong promotion/violation call ("she was friendly fire the whole raid" when she wasn't, for half of it).
- **People arriving mid-fight**: not addressed at all in walkthroughs or analysis (e.g., does a late arrival get a track/team from t=0 that's meaningless, or does the Zone/Avatar page correctly start their history at first-sighted time?). Minor but unaddressed.

## Alt-cam usability of the overlay

The entry correctly treats the overlay as primary rather than decorative, which matches the owner's brief better than a 2D-first design would. The self-identified risk about pick-radius vs. drag is the right risk to flag and is handled reasonably (minimum pixel radius, no-fire-on-drag). One thing under-specified: the entry never states what happens when the officer alt-cams to a location where dozens of stale trail lines from unrelated times overlap (a 1–2 h session's full Range picture at a busy zone) — pick disambiguation when multiple pickable rects nearly coincide isn't addressed, and that's precisely the alt-cam-at-a-distance case the risk section gestures at but doesn't resolve.

## Equipment/team index gap (affects Q4, Q7)

Equipment is a first-class noun with its own page, but there is no **index** of equipment reachable from Session — only via an event's attribution chain or an avatar's collapsed weapon list. Q7 ("which equipment did each team use") has no route in this IA except manually visiting every avatar on a team and union-ing their weapon lists by hand; there is no "Team → equipment breakdown" page or link, despite Team, Avatar, and Equipment all being modeled as nouns with backlinks. Q4 ("which equipment kills through walls, how confident per weapon") is answerable only for equipment that happens to have crossed the auto-surfaced claim threshold on the Session page — an officer who wants to audit a *specific* weapon that hasn't triggered a claim (e.g., checking a rumor) has no browse-all-equipment entry point. Given the brief lists Equipment as a page and explicitly asks for Q4/Q7, this is a real missing link, not a stylistic quibble.

## What must be kept

- The rung-0↔rung-1↔rung-2 transitions that are specified (drag-select → Zone → histogram → click-peak → rung 0; sweep → aggregate → Claim → click-candidate → rung 0) are genuinely reversible, concrete, and well-matched to Bret Victor's "step up, step down" requirement. Keep this pattern.
- Overlay uncertainty encoding (dashed tracks for degraded position quality, hollow+`?` for UNKNOWN, alpha tied to confidence, "never invent a value to fill a gap") is honest and specific — keep verbatim.
- The suspicion model's refusal to combine three signals into a verdict, and its literal "here is what happened" phrasing discipline, directly satisfies Q8's requirement and should be kept regardless of what else changes.
- The headline sentence's hedge-from-actual-confidence rule (not a fixed string) is a good small honesty mechanism — keep.
- The intra-group-skirmish handling (ignore group tag, cluster on who-shoots-whom, emphasize the threshold slider precisely when auto-labels are least trustworthy) is a correct, specific response to a named brief scenario — keep.
- Time-indexed friendly-fire confidence in the analysis model (§6) is the right computation — it just needs a UI surface (see gap above) to be worth anything.

## What must change

- Replace or loosen the destructive-pop single-stack breadcrumb, or at minimum give the pin mechanism enough memory to restore a branch's sub-navigation, not just its root — otherwise the design's core interaction model fights the brief's explicit digging requirement.
- Add an equipment index and a per-team equipment rollup reachable from Session/Team, or explicitly justify why Q4/Q7 are meant to be answered only through the claims feed.
- Either give the overlay a documented FFA/low-confidence colour fallback for Range mode, or state plainly that Range mode is reduced to per-avatar colour with no team read in that case — right now it's silently unspecified.
- Reconcile the Q3 walkthrough with the actual overlay/IA capabilities: either add a chronological/searchable death list (a real noun) so "why did X die at 14:32" has a floater-based cold-start path, or rewrite the walkthrough to show the realistic (slightly longer) path through the scrub strip.
- Surface team-assignment-over-time per avatar (even a simple sparkline of "assigned cluster by minute" on the Avatar page) and let manual overrides be time-bounded ("correct from t onward"), not session-wide hard seeds — this is required by a named brief scenario (allegiance drift) that the current design computes but never shows.
