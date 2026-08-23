# Atmo Magic v3: a unified, opt-in environment system (proposal)

This is a design, not a build log — nothing here exists yet as its own
system, though the logic it needs largely already exists, tangled into v2.
It specifies a fully client-side, opt-in environment system that carries the
Atmo Magic branding forward as a **complete, self-contained renderer**, cleanly
separated from stock v2 (EEP/Windlight) rather than living inside it.

## Relationship to what exists — and the untangling this requires

- **v1** — Windlight: one flat, scrolling-noise cloud plane, one sky/water/day
  asset set per region.
- **v2** — **stock EEP**, upstream LL behaviour: region sky/water/day assets,
  up to four altitude-keyed sky tracks via
  `LLEnvironment::calculateSkyTrackForAltitude`. This is not Soapstorm's own
  system — it's Linden's, and it stays exactly as upstream ships it.
- **Atmo Magic (current)** — the already-shipped weather layer (see
  [`atmo_magic_tracks.md`](atmo_magic_tracks.md)) was built by injecting into
  and modifying v2 in place, riding its four altitude tracks rather than
  having its own. Its logic is what v3 is actually built from.
- **v3** (this document) — Atmo Magic's branding, moved to a wholly
  independent, fully parallel renderer with its own unified asset and its
  own notion of "track", decoupled entirely from `LLEnvironment`.

**The migration this implies, in order:**
1. **Fork** — duplicate the current Atmo Magic (`ss`-prefixed) code into its
   own independent v3 files. This starting point already contains working
   weather-in-tracks logic; it just needs to stop depending on
   `LLEnvironment` internals to run.
2. **Untangle** — `git diff master...feature-atmo-magic` on every EEP-proper
   file (`llenvironment.*`, `llsettingsvo.*`, `llsettings{sky,water,day,base}.*`,
   `llpaneleditsky.cpp`, `llpaneleditwater.cpp`, `llfloaterenvironmentadjust.cpp`,
   `lllegacyatmospherics.*`, `llvosky.*`) comes back **empty** — v2/EEP is
   already pristine, confirmed by both the diff and an absence of any
   `<SS:Nexii>`-tagged comment in those files. There is nothing to revert on
   the EEP side. The actual injection lives in roughly twenty core-engine
   files instead — `pipeline.cpp/h`, `llviewerdisplay.cpp`, `llagentcamera.cpp`,
   `llworld.cpp`, `llflexibleobject.cpp`, `llviewerpartsim.cpp`,
   `llviewerobject(list).cpp/h`, `llviewermessage.cpp`, `llvowater.cpp`,
   `lldrawpoolwater.cpp`, `lldrawpoolwlsky.cpp`, `llvoavatar.cpp/h`,
   `llviewershadermgr.cpp/h`, `llviewermenu.cpp`, `llappviewer.cpp`,
   `llviewerwindow.cpp`, `llviewercamera.h`, `llviewerfloaterreg.cpp`,
   `fsrezqueue.cpp/h` — each `<SS:Nexii>`-tagged hook reviewed in place rather
   than diffed against an upstream baseline.
3. **Diverge** — v3's forked copy evolves independently from there, into
   everything else this document specifies (multi-track, planetary system,
   per-param keyframes, the weather cube, and so on).
4. **Rename, once v2 is actually deleted.** The `V3`/`v3` in every class,
   file, and floater name (`SSAtmoV3Asset`, `ssatmov3mgr.cpp`,
   `floater_ss_atmo_v3.xml`, ...) exists only because the plain names
   (`SSAtmoMagic`, `SSAtmoTrackMgr`, `ssfloateratmo`) are still owned by the
   v2 code sitting next to it during the coexistence period - it is scaffolding,
   not the intended final name. Once v2's files are actually removed as the
   last step of the untangle, those names free up, and everything gets
   renamed in one pass to drop the suffix - "Atmo Magic" plain, same as the
   branding was always meant to read.

## Storage & discovery

- **Format:** LLSD (XML), matching the existing weather layer's notecard
  format and reusing its (de)serialization machinery rather than inventing a
  second one.
- **One asset, no tracks-as-separate-assets:** the whole environment — every
  track's sky/water/weather/planetary config, all keyframes — is one
  document.
- **Parcel discovery:** reuses the same `atmo:<uuid>` marker convention
  already used by the v2 weather layer, resolving to a v3 unified asset
  instead. **Needs a disambiguator** — see Open Items.
- **Fetch protocol: a plain HTTP round trip through the existing Bridge
  plumbing, not a chunked chat relay.** An earlier pass in this doc
  specified `llOwnerSay`-chunked chat tags (`<Notecard:UUID:index.total>`);
  that turned out to be solving a problem the Bridge doesn't actually have.
  The Bridge already runs an HTTP channel for every other command
  (`FSLSLBridge::viewerToLSL` → `llRequestSecureURL`/`llHTTPResponse`, with
  `FSLSLBridgeRequestResponder`-style async callbacks on the viewer side),
  and `llHTTPResponse`'s body isn't chat-length-limited the way
  `llOwnerSay` is — so there's no chunking problem to solve at all once the
  right transport is used.
  - **LSL side:** a `FetchNotecard|<uuid>` command
    (see `indra/newview/fs_resources/EBEDD1D2-...-D47BBCA5DFB.lsltxt`) reads
    the notecard synchronously (`llGetNotecardLineSync`, NAK-sleep-retry, a
    3-second timeout), then replies over `llHTTPResponse` with the body
    passed straight through when it already starts with `<llsd>` — true for
    every v3 asset, since that's exactly what `LLSDSerialize::toPrettyXML`
    emits — or wrapped as `<llsd><string>...</string></llsd>` otherwise.
  - **Viewer side:** `SSAtmoV3DiscoveryMgr::requestFetch()` calls
    `FSLSLBridge::instance().viewerToLSL("FetchNotecard|" + uuid, callback)`.
    The HTTP layer already parses the `<llsd>` response body before the
    callback ever sees it, so a v3 asset's own content arrives as a real
    LLSD map directly — no text reassembly step of any kind. A response
    that isn't a map (the wrapped-string fallback path, or a hand-edited
    notecard) falls back to parsing it as LLSD-XML/notation text instead.
  - The UTF-8/chunk-size concerns this section used to carry (byte budgets,
    character-vs-byte counting) were specific to chat-based chunking and no
    longer apply — an HTTP body has no such limit to work around.
- **Manual save/load:** the floater gets **Save to Notecard** (writes a plain
  inventory notecard) and **Load from Notecard** (inventory picker +
  drag-and-drop onto the floater). No new asset type, no scripted delivery
  required for this path.
- **Creation has two entry points, same underlying action, no floater-side
  default.** There is no implicit environment that springs into existence
  just from opening the floater, and no "Defaults" button that resets to
  some standing state — v3 is opt-in end to end, so nothing runs until
  something has actually been created or loaded:
  1. **Inventory context menu.** The existing **New Settings** submenu
     (`menu_inventory_add.xml`, and its twin in the My Environments floater's
     `menu_settings_add.xml`) already holds `New Sky` / `New Water` /
     `New Day Cycle`, each a `menu_item_call` firing `Inventory.DoCreate`
     with a parameter string through `LLPanelMainInventory::doCreate()` →
     `menu_create_inventory_item(...)`. **New Atmo Magic** is a fourth entry
     in that same submenu, taking the same path except its creation branch
     writes a plain Notecard pre-filled with the default v3 LLSD body,
     instead of following Sky/Water/Day Cycle down the real
     Settings-asset-creation path. Kept because it's the discoverable,
     inventory-native way to start one — important on its own UX merits, not
     just as a fallback.
  2. **Floater's own one-time create button.** When the floater has nothing
     loaded, it shows a single "Create New Environment"-style button in
     place of the normal editing UI, calling the exact same
     creation-and-write path as New Atmo Magic above, then immediately
     loading the result. This exists purely so opening the floater with
     nothing active doesn't dead-end into an empty panel with no visible way
     forward — it's a bootstrap affordance, not an ongoing control, and it
     disappears once something is loaded (at that point Save/Save As/Load/
     Revert are the relevant buttons, not this one).

  Either path produces the same thing: a real, inert notecard in inventory,
  pre-filled with the default v3 LLSD body, doing nothing until it's loaded.
- **Auto-apply on parcel entry**, unless the v3 floater is currently open
  (treated as "mid-edit, don't clobber").

## Rendering model

- Fully parallel to `LLEnvironment` — borrows heavily from it rather than
  starting from scratch, but does not plug into its personal-override slot.
- A master enable switch gates everything, mirroring the v2 layer's
  `SSAtmoEnabled`: turning v3 support on doesn't load an environment by
  itself.

## Tracks

- **1 mandatory (ground) + up to 3 optional — 4 total**, deliberately
  matching the existing track count, but **not the same tracks**: v3 track
  thresholds are freely author-defined, entirely decoupled from the region's
  actual EEP altitude-track settings. A skybox city at 900m can declare
  itself "ground zero" for its own isolated biome regardless of what the
  terrain below is doing.
- **Each track is a complete, isolated environment** — its own Atmosphere &
  Lighting, Clouds, Planetary, and Weather configuration, in full. Nothing
  blends *across* tracks; a Mars biome at altitude and an arctic ground track
  share nothing but the notecard they're saved in.
- **Activation:** the avatar's actual world Z crossing a track's configured
  threshold switches which track is active.
- **Transitions:**
  - Physically crossing a boundary → soft cross-fade over a configurable
    buffer zone (default ~10–20m).
  - Arriving via teleport, sit-teleport, or region position change → instant
    cut.
  - Water involved in the crossing → always instant cut, regardless of how
    it was entered.

## Water

- One optional water plane per track: enabled/disabled, keyframable height
  (tides), independent of that track's own altitude-activation band.
- Only one water plane is ever globally visible: whichever *enabled* track
  sits lowest — regardless of which track is currently active for the
  avatar, so a high skybox can still see the sea far below it.
- **Exclusion volumes:** any simple, non-hollow, non-deformed prim (same
  shape constraints as reflection probes) with "Hide Water" set on a face
  acts as one — camera-inside suppresses the underwater state
  (breathing/audio/post-process) without killing water fog or visibility
  where the surface is still in view, so underwater tunnels/domes work.

## Weather tab

Per-track. Only simulated for whichever track the camera currently occupies
— no bleed-through from neighboring tracks.

**Inputs:** Moisture `[0,1]`, Convection `[0,1]`, Temperature
`[-30°C, +40°C]`, Wind Heading `[0°,360°)`, Wind Speed, Gust (depth/length/veer
— same role turbulence played, renamed), Lightning intensity. Every derived
value below defaults from the M/C/T cube and can be individually overridden.

**Precipitation type** (what's actually falling — separate from ground
state, see below):

```
if Temp < -1°C:
    Blizzard if Convection > 0.7 else Snow
elif -1°C <= Temp <= 0°C:
    Freezing Rain if Convection > 0.5 else Sleet
elif 0°C < Temp <= 1.5°C:
    Slushy Sleet / Wet Rain mix
else:
    Hail if Convection > 0.8 else Rain
```

Chosen specifically because it's a pure function of the instantaneous
Temp/Convection state — no transitionary/temporal type (e.g. "this drop
*used to be* freezing rain and is becoming sleet") needs to be tracked frame
to frame.

- Drop size varies continuously (drizzle → downpour) directly per-drop; the
  underlying drop *texture* is only regenerated when the size category has
  changed **and** hasn't been regenerated recently, to avoid texture churn
  from a slowly drifting size value.

**Ground state** (separate system — how precipitation behaves once landed):

| Band | Behaviour |
|---|---|
| Deep Freeze (< −5°C) | Puddles freeze solid; fast, dry powder snow accumulation |
| Wet Snow (−2°C to 0°C) | Packing snow |
| Slush & Sleet (0°C to 1.5°C) | Semi-transparent, reflective melt patches; snow accumulation capped and actively decaying |
| Cold Rain / Thaw (1.5°C to 5°C) | Puddles form; existing snow rapidly thaws |
| Warm (> 5°C) | Normal wetness shading; puddles evaporate as moisture drops to 0 |

**Convection thresholds** (cloud shape, fall behaviour, lightning cadence —
also drives the Clouds tab's volumetric field, below):

| Band | Cloud shape | Precipitation motion | Lightning |
|---|---|---|---|
| Stable (0.0–0.2) | Flat, uniform grey overcast, slow pan | Straight vertical descent | None |
| Breezy (0.3–0.5) | Slight definition, rolling shapes | Angled fall from wind force | None |
| Turbulent (0.6–0.7) | Dark, heavy, boiling cumulus | Choppy, stretched, faster fall | Every 30–60s, random |
| Severe (0.8–1.0) | Pitch-black, rapid pan, max churn | Chaotic/sideways, splash ripples on collision | Every 2–5s |

- **Forecast text** is generated from this same threshold data (e.g.
  "Thundery showers and a gentle breeze").

## Clouds tab (+ Volumetric Field sub-tab)

- The existing Windlight flat-plane scrolling-noise layer is **kept, demoted
  to cirrus only** — it already looks right that high up and needs no
  rework.
- The new **Volumetric Field** sub-tab configures the storm-capable layer:
  a small per-track queryable coverage/density/base-height/thickness field
  (`SSCloudField`, sketched in an earlier working session), driven by
  Convection (shape/churn/height) and Moisture (thickness/opacity).
- That field is the shared data source both rain-shaft placement and
  lightning-fork generation query — rain only spawns under columns above a
  density threshold, and lightning branches only grow through cells above
  that same threshold before exiting toward a strike point. This is what
  gives "rain that falls from the actual cloud shape" and "lightning that
  forks within the cloud" without a full volumetric raymarcher.
- This sub-tab is the least settled part of the spec — treat its exact
  parameters as provisional pending an actual build-and-look pass.

## Planetary tab

- **Hierarchy:** exactly three levels, Sun → Planet → Moon, no deeper
  nesting. Any number of bodies per level.
- **Per-body fields:** diameter, mass, orbital radius, inclination, phase
  (position along orbit), orbital period, rotation period, axial tilt.
  Eccentricity deferred.
- **Position model:** everything is fixed/authored, not simulated — radius +
  inclination + phase deterministically produce a fixed apparent direction
  and angular size (via distance falloff) for every non-home body. Orbital
  and rotation period are stored now, unused for motion today, specifically
  so a future "actually animate this" toggle is a pure function-of-time
  evaluation rather than a schema change.
- **Hierarchical binary pairs:** any two sibling bodies (two suns, or a
  planet + moon of comparable mass) may be flagged as a bound pair with a
  mass ratio, giving a closed-form computed barycenter that the parent-level
  orbit targets. True N-body (3+ mutually-orbiting bodies) is explicitly out
  of scope — there's no closed form for it, which breaks the
  "fixed-now/simulate-later for free" property this whole model relies on.
  Anything not explicitly paired just orbits its parent's point directly.
  Implemented as a symmetric `mBoundPartnerIndex` on each paired body; a
  third body "orbits the pair" simply by pointing its own `mParentIndex` at
  either paired member — the resolver recognizes that member has a partner
  and substitutes their shared mass-weighted barycenter as the orbit
  anchor, so no separate "orbit target: pair" concept is needed in the
  schema at all.
- **Home body:** exactly one body, of any type, flagged "home" (freely
  movable to any other body). Supplies the axial tilt + rotation period that
  drive the *computed* primary sun's azimuth/elevation arc, replacing
  keyframed sun position entirely for that arc. A body flagged home cannot
  also be a light emitter. **Default new asset:** an Earth-sized planet as
  home, orbiting a 1-solar-mass sun.
- **Light emitters:** hard-capped at 2 (the renderer's sun+moon light slots).
  The UI disables further "light emitter" checkboxes once 2 are set. If a
  notecard somehow specifies more, the first two in list order light the
  scene; the rest are simply left unchecked for that session — nothing is
  deleted from the saved asset unless the user explicitly saves again.
- **Distance scale:** two independent dials, Sun↔Planet and Planet↔Moon,
  compress authored orbital *distances* for artistic effect without touching
  any body's own independently-authored physical size.
- **Rendering:** quad/billboard only for v1, with an equirectangular custom
  texture supported per body. True spherical geometry, real 3D rings, and
  any visible effect of axial tilt/spin are deferred until that rendering
  mode exists.
- **Day length:** stored per track, defaulting to the region's current EEP
  day-length/offset at creation time, free to diverge afterward.

## Keyframes

- Universal per parameter (slider, colour, dropdown/enum) — no
  whole-sky-snapshot keyframe concept like EEP.
- **Rule:** no keyframe on a param → plain permanent value. Head sitting on
  an existing keyframe → editing the value edits that keyframe. Head *not*
  on an existing keyframe → editing the value inserts a new one there.
  Non-tweenable values (e.g. a forced precipitation-type override) hold from
  one keyframe up to, but not including, the next.
- **Curves**, hidden behind a simple interface: Ease-in-out by default,
  Linear where more appropriate, Hold/step for non-tweenables. No
  curve-editing UI exposed in v1 — defaults are chosen per field type.
- **Editing surface:** an After-Effects-style 2D parameter table — one row
  per parameter: label, slider, numeric input, a keyframe-diamond toggle, and
  chevrons either side of it to jump the preview head to that parameter's
  previous/next keyframe (wrapping around the loop). A dot/tick strip under
  each row mirrors the shared timeline position against that parameter's own
  keyframes. The master scrubber at the top of the same column sets preview
  time only — it is not itself interactive for keyframes.

## Deferred / explicitly out of scope for v1

- True N-body simulation beyond hierarchical binary pairs.
- Live/simulated orbital or rotational motion — schema is ready, math isn't
  wired up.
- Spherical-geometry celestial bodies and real 3D rings.
- Orbital eccentricity.
- Seeder-feeder cross-track rain interaction (out, per the multiple-isolated-
  tracks discussion).
- Multiple distinct water bodies within one track.
- A full drag-and-tween graph editor for keyframes (v1 ships the simpler
  table + popup model).

## Open items needing one more decision

- **Existing v2 notecards, on upgrade.** Since v3 retires the v2 weather
  layer rather than running alongside it, there's no live tag collision to
  disambiguate — but any `atmo:<uuid>` notecard already saved in the *old*
  format will still be sitting on parcels out there the moment v3 ships.
  Does a v3 viewer encountering one of those (i) silently ignore it and fall
  back to no-weather, since it can't be told apart from a corrupt/foreign
  document without inspecting it, (ii) auto-migrate it — read the old
  `precipitation`/`turbulence`/`wind_*` keys and derive an equivalent
  Moisture/Convection/Temperature-cube starting point — or (iii) is a clean
  break acceptable, since this is a personal client-side feature and not
  something anyone has a durable dependency on? Not decided yet.

## Implementation status

Everything below is written and build-verified (compiles and links clean
against `firestorm-bin`); "wired" means it's actually consumed by something
that runs, not just sitting next to it.

| Phase | Files | State |
|---|---|---|
| 1 — schema, notecard round-trip | `ssatmov3asset.*`, `ssatmov3mgr.*`, `ssfloateratmov3.*`, `floater_ss_atmo_v3.xml` | Done. Inventory `New Atmo Magic` creation wired into `llviewerinventory.cpp`. |
| 3 — keyframe engine | `ssatmov3keyframe.h` | Done. Proven end-to-end on `SSAtmoV3Weather::mMoisture` in the floater. |
| 4 — multi-track resolution | `ssatmov3trackstate.*` | Done. Not yet fed a live camera position — pure function, no agent hookup. |
| 5 — weather derivation | `ssatmov3weatherstate.*` | Done. Forecast text wired into the floater as a proof. |
| 6 — planetary | `ssatmov3planetarystate.*` | Done. No rendering (quad/billboard drawing) yet. |
| 7 — cloud field | `ssatmov3cloudfieldstate.*` | Done. Derivation only — no noise field, no shader. |
| bridge — v3→v2 renderer translator | `ssatmov3legacybridge.*`, spliced into `ssatmomagic.cpp`'s `refreshParams()` | **Verified live.** Loading a v3 environment and raising Moisture produces actual visible rain through the existing renderer - confirmed in a running client, not just compiled. |
| 8 — parcel/Bridge discovery | `ssatmov3discovery.*`, calling `FSLSLBridge::viewerToLSL` (HTTP, not chat), bootstrapped from the existing per-frame Atmo Magic touch-point in `llviewerdisplay.cpp`; LSL side is the `FetchNotecard` command in `indra/newview/fs_resources/EBEDD1D2-...-D47BBCA5DFB.lsltxt` | **Verified live end-to-end.** A parcel's `atmo:<uuid>` marker triggers the Bridge fetch, the named environment loads, and it actually drives the renderer (see the bridge row above) - confirmed in a running client. |

Not yet done anywhere: actual rendering (bodies as quads, cloud noise field,
rain shafts, lightning forks), the Atmosphere & Lighting tab (still opaque
LLSD), and the floater's real per-tab editing UI beyond the phase-3/5 proof
controls.
