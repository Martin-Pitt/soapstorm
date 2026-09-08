# Entry 09 — Minimal Progressive Disclosure

## 1. Thesis

The tool should look, at rest, like almost nothing: a scrub bar and one sentence of prose. Everything an officer can ask about a raid — teams, deaths, equipment, suspicion — is a *noun* reachable by drilling down, never a panel sitting open by default eating screen and frame budget. There are exactly two windows because the engineering plan fixes two (`ss_combat_log`, `ss_combat_inspector`), but only one of them ever shows content: the main floater is pure transport chrome (play/pause/scrub, a one-line auto-generated headline, an Inspect button), and the Inspector is a single content pane that behaves like a phone app or a Wikipedia tab — one page visible at a time, a breadcrumb trail above it, a backlink strip below it, and a small pinned-favourites rail for the handful of nouns you're actively comparing. The in-world overlay is not decoration for this floater pair; it is the primary display surface. At rung 0 and rung 1 it *is* the answer to "what happened and where" — names, teams, lines of fire drawn directly on the world — and the 2D UI only earns its keep when the officer wants a fact precise enough that geometry can't carry it (exact damage numbers, a probability, a list of contributing events). The organizing discipline is: if it can be drawn on the world, draw it on the world; if it must be text, put it one level deep in the Inspector's stack, never in a permanently visible panel.

## 2. Ladder

**Rung 0 — concrete, at the cursor.** The live/scrubbed 3D overlay: head markers, current mouselook glyph, an in-flight damage line if one is happening at this instant, a LOS ray if something is selected. *Step up*: press **Trails** in the main floater (or just wait — a short default trail is always drawn) to see the last N seconds as a fading line, easing you into rung 1 without a mode switch. *Step down*: nothing below rung 0 — it's the floor; scrubbing is the only motion within the rung.

**Rung 1 — abstract over time.** Press **Range** in the main floater: every avatar's entire-session track is drawn as one static picture (colour = inferred team, no fade), with a death-density heatmap and a home-made spawn marker. This is Bret Victor's helicopter view. *Step up*: drag-select a dense knot of trails → pushes a **Zone** page onto the Inspector stack, which shows a small-multiple activity histogram for that patch of ground across the whole session; picking a peak in that histogram is the step back down — it moves the cursor there and snaps the overlay back to rung 0 at that instant. *Step down*: click anywhere along any trail line in the rung-1 picture → cursor jumps to the nearest sampled time at that point, overlay drops to rung 0, Inspector opens that avatar's page.

**Rung 2 — abstract over a parameter.** Three concrete sweeps, all triggered from the Inspector, all drawn back onto the overlay: (a) LOS candidates swept across the bullet-travel window for one hit, each candidate position colour-coded CLEAR/BLOCKED; (b) team-assignment confidence swept across a threshold slider on the Team page, redrawing cluster membership live; (c) damage before/after each adjustment script, swept as a stacked bar. *Step up*: aggregate a sweep across every event that shares an equipment or avatar → a **Claim** page ("SMG-X may kill through walls, 7/9 BLOCKED, medium confidence"). *Step down*: click one candidate/bar in the sweep → jumps to that instant at rung 0 with the specific ray or hit drawn.

**Rung 3 — small multiples and unbent space.** The Team page shows one sparkline per member (kills/deaths/damage over time) side by side — many dimensions, one glance. The Zone page can unbend: pick two teams and it re-plots every position as signed distance-to-front instead of x/y, so "the line held" or "the line broke" becomes a single rising/falling curve. *Step up* from here is interpretive text only (the headline sentence); there is no rung 4 — by design, the ladder tops out at something a human still checks against rung 0.

## 3. Information architecture

**Nouns (pages), one class of object, one page shape each:**
- **Session** (root) — headline stats, team summary, zone summary, top claims.
- **Zone/Place** — auto-clustered patch of ground; activity histogram over time; unbend-to-front control.
- **Minute/Interval** — a time bucket (default 1 min, coarsens to 5 min at rung-1 zoom); what happened, who was where, links to events inside it.
- **Team** — members with confidence, threshold slider, friendly-fire ledger.
- **Avatar** — role stats, weapons used, mouselook %, collapsed event list.
- **Event** (Death or Damage) — attribution chain, LOS verdict, adjustments, friendly-fire check.
- **Equipment** — classification + evidence, users, damage/adjustment stats.
- **Claim** — a generalized assertion ("kills through walls", "unusually fast reactions") with its evidence list, confidence interval, and the through-wall/lag caveat text.

**Linking model.** Every page ends in a *Related* strip of clickable chips (backlinks: who points at this noun — e.g. an Equipment page's Related strip lists every death it caused). Clicking a chip **pushes** the target noun onto the Inspector's single navigation stack; the breadcrumb bar at the top is that stack rendered as `Session ▸ Team A ▸ Kira ▸ Death @14:32:07`. Clicking any breadcrumb segment pops back to it (destructive pop — deeper history is discarded, matching a browser's back button, not a tree). There is exactly one stack, exactly one visible page, ever — this is the "minimal" commitment: no split panes, no tabs holding parallel content.

**Pinning.** A star icon in each page header toggles a pin. Pins render as a thin row of chips above the breadcrumb (max 5, oldest evicted, e.g. `★Kira ★West-Wall ★SMG-X`). Clicking a pin *pushes* it onto the current stack like any other link — it's a bookmark, not a second pane. This is the one deliberate escape hatch from strict linearity: officers comparing two avatars pin both and hop between them without re-walking the breadcrumb each time.

**History vs. breadcrumb.** The breadcrumb *is* the history; there is no separate back-stack to reason about. This keeps the model teachable in one sentence, which matters more here than supporting arbitrary DAG navigation.

## 4. Screens

### Main floater — `ss_combat_log` (fixed content, ~340×120, not resizable)

```
┌ Combat Log ─────────────────────────────────── [x]┐
│ ● LIVE   ⏮  ⏸  ⏭   1x▾        [Trails] [Range] [?]│
│ ───●═════════▓▓▓░────────────────────────────────  │
│ 14:32:07 — "3 deaths near NW Wall in the last 10s.  │
│  Team split looks stable (62% conf)."               │
│                                        [Inspect ▸]  │
└──────────────────────────────────────────────────┘
```
- Row 1: recording state dot, transport buttons, speed dropdown, then two toggle buttons that set the overlay rung (**Trails** = rung 1 short trail always-on vs. off, **Range** = rung 1 full-session picture), and a `?` that pops a tiny legend card (glyph key) over the overlay — closes itself after 4 s or on click-away.
- Row 2: `LLView`-subclass scrub strip — session span, cursor triangle, death ticks (small red marks), a damage-density band (alpha by event rate), drag anywhere to scrub. This is the one custom-drawn chart in the whole floater.
- Row 3: the **headline** — one deterministically-templated sentence for the current cursor window (see §6). This is the ambient, always-on disclosure: an officer glancing at the corner of their screen gets a sentence, not a wall of numbers.
- Row 4: **Inspect** opens/focuses the Inspector at the Session page, cursor time preserved. A gear icon (not drawn above, top-right corner) pops start/stop/clear/retention controls inline — kept out of the default view entirely.

### Inspector — `ss_combat_inspector` (resizable, default ~420×320, grows on demand)

```
┌ ★Kira ★West-Wall            [pin: ★] ───────── [x]┐
│ Session ▸ Team Ashguard-A ▸ Kira Nightshade         │
├────────────────────────────────────────────────────┤
│  < content pane, one page type at a time >          │
│                                                      │
├────────────────────────────────────────────────────┤
│ Related: Team Ashguard-A · West Wall · SMG-X        │
└──────────────────────────────────────────────────┘
```
Header = pin rail + pin toggle; sub-header = breadcrumb (click any segment to pop); footer = backlink chips. The content pane swaps by page type:

**Session page**
```
Deaths 47   Damage events 2,340   Avatars 23
Teams (auto, 2 clusters, conf 78%) ▸
 ▓▓▓▓▓▓▓▓▓▓▓ Ashguard-A (11)
 ░░░░░░░░░░░ Ashguard-B (9)      3 unassigned
Zones ▸                 Activity (small multiples)
 NW Wall     ▂▃▅▇▆▃▁
 Courtyard   ▁▂▂▃▇▇▅
 Spawn       ▇▃▁▁▁▁▁
Top claims ▸
 "SMG-X may kill through walls" (7/9 blocked)
 "Y shows unusually fast reactions" (evidence only)
```

**Avatar page**
```
Team: Ashguard-A (81% conf)  [override ▾]
Kills 4  Deaths 1  Dmg dealt 1,240  taken 640
Weapons: SMG-X (912 dmg) · Frag-Trap (deployed)
Mouselook 38% of session · Seated 4 min
Activity  ▂▃▅▇▆▃▁▂▃▅▇   (click bar → scrub + jump)
Events [show all 62 ▾]   ← collapsed by default
```

**Death/Event page**
```
Kira Nightshade died 14:32:07 (Impact)
Killing blow: Marrow Voss, SMG-X, 46 dmg
Contributing (6s window):
 14:32:01 Marrow→SMG-X  18 dmg
 14:32:04 Marrow→SMG-X  22 dmg
 14:32:07 Marrow→SMG-X  46 dmg  (killing blow)
LOS: CLEAR (4/4 candidates)         [see world ▸]
Adjustments: none
Friendly fire: no (conf 81%/74%)
        [ ▶ Show in world ]  [ ⏱ Jump cursor ]
```

**Team page**
```
Confidence threshold  ░░░●░░░░░░  62%  (drag → live redraw)
Members at threshold (11)
 Kira Nightshade    92% ▸
 Talon Reyes         88% ▸
 Wren (uncertain)    54% ▸  [reassign ▾]
Friendly fire (this team): 3 events [list ▾]
```

**Claim page**
```
SMG-X — "kills through walls"
7 of 9 lethal hits BLOCKED at hit time
Confidence: MEDIUM (n=9, 95% CI 41–86%)
Caveat: travel time + lag can produce false BLOCKED.
Evidence [expand ▾]
 Hit @14:32:07 (Kira): t-0.0 BLOCKED, t-0.3 BLOCKED,
   t-0.6 CLEAR → leans BLOCKED   [see world ▸]
```

All lists (`show all`, `Evidence [expand]`) start collapsed — a single row summary plus a disclosure arrow — which is the "nothing shown until asked for" rule applied inside the one window, not just across windows.

## 5. In-world overlay

- **Rung 0**: head ring + short yaw arrow + name label (culled >256 m) per avatar, coloured by team (grey if unassigned); a small eye glyph over anyone in mouselook; an in-flight damage line (colour by damage type, brightness by magnitude) that pulses for 1 s; a persistent cross+ring at recent death sites. Selected marker gets a white outline.
- **Rung 1 (trails/range)**: polylines per avatar; alpha ramps by recency in Trails mode, flat in Range mode; a translucent heatmap disc under high death-density patches.
- **Rung 2 (sweeps)**: LOS candidates as small dots along the attacker's pre-hit trail, green (CLEAR) or red (BLOCKED), joined to the victim by a thin ray; hollow dot + question-mark glyph for UNKNOWN (no track sample in the window). Team-threshold drag recolours all rung-0/1 markers live as the slider moves.
- **Uncertainty, always**: confidence maps to alpha (low-confidence team colour is washed out toward grey), missing/low-quality position data draws the trail dashed instead of solid, and anything UNKNOWN gets the hollow+`?` glyph rather than a colour guess — never invent a value to fill a gap.
- **Clicking**: any marker (avatar, death cross, LOS candidate dot) is a screen-rect pick; a plain left-click (ALT/CTRL reserved for camera) opens the Inspector on that noun, pushing it onto the current stack exactly like clicking a chip — the world and the floater share one navigation model. Clicking empty space does nothing (no accidental deselect-and-lose-place).

## 6. Analysis model

- **Death attribution**: DAMAGE events with the same `target` in `(t−window, t]`, grouped by `(owner, rezzer)`; cumulative damage per group; killing blow = last event. Uncertainty: events beyond the window are shown greyed as "possibly related" rather than silently dropped.
- **Team clustering**: label propagation over a graph where avatars are nodes and edge weight is signed net damage exchanged (negative = mostly hurting each other), seeded by active group id where available. Output per avatar is a probability vector over inferred clusters; **confidence** = margin between top two probabilities. Friendly-fire flag on a damage event = `argmax(owner) == argmax(target)` at the time of the hit, tagged with the *minimum* of both avatars' confidence at that instant (teams can drift, so confidence is time-indexed, not session-wide). Manual override at the Avatar/Team page writes a pinned assignment that clustering treats as a hard seed from then on.
- **LOS verdict**: candidate attacker positions at `t − k·Δ` for `k = 0..N` across the travel window (from track samples, falling back to `source_pos` when present); ray-cast per candidate; verdict is CLEAR if any candidate is clear, BLOCKED if all are blocked, UNKNOWN if no track sample exists in the window. Fraction-clear is the per-event confidence.
- **Through-wall claim per equipment**: proportion of that equipment's hits with verdict BLOCKED, reported with a Wilson score interval (not a raw percentage) so a 2/2 sample reads as low-confidence rather than "100%". The travel-time/lag caveat text is permanently attached to the Claim page, not a one-time tooltip.
- **Damage adjustment ratio**: `Σfinal / Σinitial` per avatar/team/equipment, with the list of adjusting script names pulled from `modifications`.
- **Suspicion score** (Q8): three independently-labelled, evidence-only signals, never combined into a single verdict — (1) reaction time: time from entering an attacker's plausible view cone to returning accurate fire, flagged when far below a human baseline; (2) fire-while-occluded: shooting accurately at a target with no LOS and no recent line-of-sound cue (e.g., no shots fired nearby); (3) yaw-rate smoothness outside normal human range. Each shows its raw numbers and a link to the concrete events; the UI text is deliberately "here is what happened", never "this player is cheating".
- **Zone/place**: grid-based density clustering of event+track positions over the session; each cluster centroid is labelled by compass offset + distance from the first-detected staging cluster ("spawn"), e.g. "NW, 80 m". No dependency on parcel names, which aren't guaranteed available.
- **Front unbending**: for a chosen team pair, project every position onto signed distance to the instantaneous boundary between the two teams' kernel density estimates; plotted as one line over time on the Zone page.
- **Headline sentence**: a small deterministic template picks the single most salient fact in the trailing 10–30 s window (death count + zone, or team-confidence delta, or a new claim crossing a confidence threshold) and renders one sentence with its own confidence phrase ("looks stable", "unclear") — never invented certainty.

## 7. Walkthroughs

**Q1 — how did the raid go?** Officer opens the main floater; the headline already answers the gist as it scrubs. They press **Range**: the whole session appears as trails on the world, two colour blobs converging on a courtyard with a bright death-heat patch at "NW Wall". They drag-select that patch → Zone page opens, small-multiple histogram shows activity peaking three times (three pushes). Clicking the tallest peak snaps the cursor there and drops to rung 0, where the actual push is visible as moving dots and damage lines. Total: two clicks from cold open to a concrete instant of the raid's turning point.

**Q3 — why did X die at 14:32?** Click X's death cross on the rung-1 or rung-0 overlay directly (no floater interaction needed) → Inspector opens the Death page: killing blow, contributing hits, LOS verdict CLEAR, no adjustments, not friendly fire. Officer clicks "Show in world" → camera-relevant markers highlight and the LOS ray is drawn at that instant for visual confirmation — the step back down to rung 0 that keeps the officer grounded.

**Q8 — anyone behaving suspiciously?** From the Session page, "Top claims" lists a suspicion-evidence entry if one crossed a display threshold this session; otherwise the officer opens an Avatar page for a candidate and finds the same three-signal panel with raw numbers and event links, never a verdict — clicking a flagged reaction-time instance drops to rung 0 at that moment so the officer eyeballs it directly.

**Intra-group skirmish.** Two Ashguard sub-teams, active group tag useless. Label propagation still splits them by who-shoots-whom; the Team page shows two clusters both labelled "Ashguard" with a manual rename affordance, and confidence is visibly lower (the drag-threshold slider is emphasized here specifically because auto-labels are less trustworthy) — the officer drags the threshold down, watches boundary avatars reshuffle live on the overlay, and hand-confirms via the reassign control before trusting the friendly-fire ledger.

**FFA scenario.** No stable clusters form; most avatars sit near 50/50 confidence. The Team page shows "unassigned" as the largest group and a note ("clustering unstable — high mutual damage, low margin") instead of forcing two clusters. The officer works avatar-by-avatar and event-by-event instead of team-by-team — the tool degrades to per-avatar/per-event nouns gracefully rather than presenting a false team split.

## 8. Risks and open questions

- **Single-stack navigation can feel restrictive** when comparing two unrelated nouns; the pin rail mitigates this but is capped at 5 — heavy investigative sessions may want more, at the cost of the "minimal" promise.
- **Auto-generated headline sentences risk sounding more certain than they are**; the template must always carry a hedge phrase sourced from the actual confidence number, not a fixed string.
- **Zone naming from compass-offset-from-spawn is weak** on sims with no clear staging area (FFA neutral sims); it degrades to numbered clusters, which is honest but less readable.
- **Rung-1 full-session trail rendering at 100k events** needs a decimated, precomputed polyline (not exact per-sample) to stay under frame budget — a cache invalidated only on manual "recompute", not every frame.
- **Click-to-drill on tiny in-world markers at distance** competes with alt-cam mouse movement; screen-rect pick radius needs a minimum size independent of world distance, and must not fire on drag (only on a clean click within a small pixel/time threshold).
- **Collapsing everything to one visible page** means officers cannot eyeball two avatars' stat blocks side by side without switching back and forth via pins — acceptable for the "minimal" thesis, but worth flagging as the direct cost of this angle versus a multi-pane design.
