# Combat Log — file map

Compact map of every file the Combat Log feature owns or touches. Paste the relevant section into an agent prompt. Paths are under `indra/newview/` unless stated. Branch `t3code/combat-log-analysis-tool`, worktree `C:/Users/nexii/.t3/worktrees/soapstorm/t3code-4b4f3b29`.

## Contract (read first)
- `sscombatlog.h` — the store API and all `SSCombat::` types: `Event`, `Sample`, `Track`, `Equipment`, `Flight`, `RegionSettings`, `RawLine`, `GroupInfo`/`RegionInfo`, `NounRef`, `View` (cursor, window, live, playing, speed, level, subject, selection, hover, reconstruction window), Stage 0 analysis structs (`Life`, `Attribution`, `Engagement`, `TeamAssignment`). `SSCombatLog` singleton: ingest (`ingestRelay`, `ingestTrackReply`, `ingestRegionSettings`, `addSample`, `addFlight`, `noteEquipment`), clocks (`frameToTime`), queries (`events`, `event`, `eventIndexAt`, `tracks`, `sampleAt`, `equipmentFor`, `flights`, `rawLines`, `regionSettings`, `displayName`), analysis (`engagements`, `lives`, `attribution`, `team`, `teamName`, `teamColor`, `isCombatantAt`), view/navigation (`view`, `select`, `setLevel`, `enterReconstruction`, `leaveReconstruction`, `setVisibleEvents`/`visibleEvents`), `loadSynthetic`, signals `dataChangedSignal`/`viewChangedSignal`.
- `sscombaticons.h` — header-only: `SSCombatIcons::forType(type, variant)` -> UI image name, `typeName(type)`, `EEventIcon` (death, enemy/friendly/object/vehicle/aircraft death, downed, revive).
- `sscombatfeedline.h` — header-only: `SSCombat::killFeedLine(...)` shared one-line event text used by every list.

## Data layer
- `sscombatlog.cpp` — store implementation: relay parser (`C2<frame>|` batches, foreign 72-key prefix, `FINAL_DAMAGE`, salvage of truncated arrays), tick parser (`TA|`, `K|`, `T|`), region settings (`RS|{json}` or CSV), frame->time fit, dedup of final-damage twins, decimation, `sampleAt` interpolation, retention, view/noun chains, `idle()` playback.
- `sscombatanalysis.h/.cpp` — Stage 0 stubs: engagements (greedy space-time clusters), lives, attribution (window, killing blow, ambiguous pair), teams (hints -> hostility graph label propagation), `isCombatantAt` (owner's tracker test).
- `sscombatsynth.h/.cpp` — deterministic scripted 20-minute mock raid; emits the exact wire strings through the public ingest; bulk and live-replay modes; `loadSynthetic(seed, live)`.

## UI (one floater per job, Combat > Combat Log > ...)
- `ssfloatercombatevents.h/.cpp` + `skins/default/xui/en/floater_ss_combat_events.xml` — "Events": chat-style pane of damage+death lines (icon + line), kind/name/mine filters, related pane (hits leading to a death / adjustments of a hit), Show in world, Reconstruct, Load mock raid, Replay mock live; publishes visible rows via `setVisibleEvents`, sets `view().mHover`; hosts `ss_combat_log_load_synthetic(const LLSD&)` for the menu. Registered `ss_combat_events`.
- `ssfloatercombatcombatant.h/.cpp` + `skins/default/xui/en/floater_ss_combat_combatant.xml` — "Combatant" details, multi-instance keyed by avatar UUID string: side, status at cursor, session stats, equipment families used around the selection time, recent events, Reconstruct last death. Registered `ss_combat_combatant`.
- `skins/default/xui/en/panel_ss_combat_reconstruct.xml` — bottom-centre transport panel for Reconstruction (play/pause, step, scrub, speed, loop, page, close).
- Icon atlas: `skins/default/textures/ss_combat/damage_types.png` (2048x512, 16x4 cells of 128 px) declared as clipped entries `SS_Dmg_*` / `SS_Combat_*` in `skins/default/textures/textures.xml` (search "Combat Log damage type"). Scroll-list icon rows need `setLineHeight(20)` + `setIconSize(16)` + `reshape()`.

## World layer
- `sscombatoverlay.h/.cpp` — in-world drawing under `gUIProgram` in `LLPipeline::renderDebug`: `wantsDraw()` gate, `render()` (icons for `visibleEvents()` + selection + hover, near-context "Ns ago" labels, detective freeze-frame for hover (ghosted) and selection (pinned)), `drawLegend()` (2D, bottom-centre), pick model (`handleMouseDown/Up`, `handleHover`), `SSCombatDraw` helper namespace (lines, rings, capsules, textured icon quads, pick list, palette).
- `sscombatreconstruct.h/.cpp` — ghost replay of one death (10 Hz prepared frames), transport panel creation (`createPanel`), `handleKey` (Space, comma, period, Esc), `render()`.
- `sscombatcamera.h/.cpp` — `SSCombatCamera::flyTo(eventId)`: slanted vantage with LOS to all parties, straight-above fallback, close-quarters fallback; applies via `gAgentCamera.setCameraPosAndFocusGlobal`.

## Shared-file hooks (all tagged `<SS:Nexii>`)
- `pipeline.cpp` — `renderDebug`: `SSCombatOverlay::render()` + `SSCombatReconstruct::render()` before `gUIProgram.unbind()`.
- `llviewerwindow.cpp` — `handleAnyMouseClick`: overlay mouse down/up; `updateUI`: overlay hover; `handleKey`: reconstruction keys then overlay keys; `initWorldUI`: `SSCombatReconstruct::createPanel()`; `draw`: `SSCombatOverlay::drawLegend()` next to `FSFloaterKillFeed::drawOverlay()`.
- `llappviewer.cpp` — `idle()`: `SSCombatLog::instance().idle()` guarded by `instanceExists()`.
- `llviewerfloaterreg.cpp` — registrations `ss_combat_events`, `ss_combat_combatant`.
- `llviewermenu.cpp` — `commit.add("SSCombat.LoadSynthetic", ...)`.
- `skins/default/xui/en/menu_viewer.xml` — Combat > Combat Log submenu (Events; Load mock raid; Replay mock raid live).
- `skins/default/xui/en/panel_preferences_combat.xml` — "Combat Log" section (`SSCombatLogEnabled`, `SSCombatLogOverlay`).
- `app_settings/settings.xml` — all `SSCombatLog*` keys (enabled, overlay, xray, trail seconds, mouselook grace, retention, death window, recon before/after, lanes, strain, signals, legend seen, label cull, LOD near/far, max labels, max damage lines, track Hz, poll seconds, channel/file debug logs).
- `CMakeLists.txt` — the `sscombat*` and `ssfloatercombat*` entries in both source and header lists.
- `indra/llrender/lluiimage.h` — added `getClipRegion()` accessor (SS-tagged) for atlas UV rects.

## Bridge (Stage 1, owner's LSL)
- `fs_resources/ssLSLBridge.lsltxt` — the owner's second bridge script (combat log relay, final_damage, region settings, tracking commands).
- `fs_resources/EBEDD1D2-A320-43f5-88CF-DD47BBCA5DFB.lsltxt` — main bridge script (needs the injection-guard whitelist and version bump).
- `fslslbridge.cpp/.h` — viewer bridge: second-script upload, validation count, `C2` relay routing into `SSCombatLog::ingestRelay` (not yet written), listener re-arm on handshake.

## Docs
- `doc/combat_log_ux.md` (design, screens, overlay, walkthroughs), `doc/combat_log_analysis.md` (derived quantities), `doc/combat_log_rules.md` (rule catalogue), `doc/combat_log_glossary.md` (vocabulary), `doc/combat_bridge_spec.md` (wire contract), `doc/combat_log_design/` (competition artefacts and BRIEF.md with all domain facts), this file.
- Plan: `C:/Users/nexii/.claude/plans/glistening-toasting-pebble.md`.

## Conventions for agents
- One-line `//` comment per function; `<SS:Nexii>` tags only in shared files; `[interaction: CombatLog]` at cross-system call sites; ASCII in source; no exceptions/RTTI; verify LL APIs against `indra/llui`, `indra/llrender`, `indra/newview` headers; nobody can build here, the owner builds.
- UI: stock XUI idioms; explicit `left`+`right` for stretching; negative `bottom` values anchor from the floater floor; no "rung"/"ladder" words; combatant/civilian for people; no promotion/merit/pay words.
