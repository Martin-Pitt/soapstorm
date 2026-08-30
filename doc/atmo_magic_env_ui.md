# Atmo Magic: the environment editor's information architecture

The Atmo environment floater grew tab by tab as features landed, and the shape it ended up with
describes the renderer rather than the world. `Clouds` splits into `Volumetric Field` and `Sky
Dome` - two engine lineages, not two things a builder thinks about. `Atmosphere` contains a
sub-tab also called `Atmosphere`, and that page mixes scattering colour, optical phenomena and
scene tonemapping in one list. `Planetary` is a 122-line stub ranked equal to the 1038-line
Weather panel. This document records the re-cut around what an author is actually building, and the
reasoning behind each cut, so the next person to add a tab knows where it goes.

The assumed author arrives with a theme, not a parameter category: a realistic coastline, an
urban region, a geocentric fantasy world with two moons, a sky archipelago where the landmass
floats above a cloud sea and the ocean sits kilometres below, an alien planet, or a permanent
artillery barrage built by pointing the precipitation system at something that is not weather.
The architecture has to make all of those reachable without the author knowing which subsystem
draws what.

## Two rules for where a control lives

These are separate axes, and conflating them is what produced the current layout.

**Sub-floater versus panel is decided by workflow weight.** A celestial body editor needs a list,
per-body orbits, radii and inclinations, and room to work - that earns `floater_ss_atmo_planetary`.
A single dropdown does not. The environment floater's panels are for dial-and-see work; anything
with its own list-and-detail workflow gets a floater launched from the panel that owns it.

**Keyframe buttons are decided by animatability.** A param that varies across the day cycle gets
the prev/keyframe/next triplet and a `mFloatRows`-style binding. A param that is a property of the
track rather than of a moment does not. Most controls are panel and animatable; the planetary
scales are floater-launched with two animatable scales left behind in the panel; "weather falls
from" is panel and non-animatable. All three combinations are legitimate.

## Target structure

```
Track | Water | Clouds | Sky | Weather | Space | Look
  |              |        |       |
Template      Main Deck  Scattering   Conditions
dropdown      Under Deck Light & Glow Precipitation
              Dome                    Lightning
```

`Water, Land, Sky, Space` is the series the tab names sit in - hence `Space` over `Cosmos` or
`Astronomy`, the first overselling a two-slider panel and the second naming the study rather than
the thing.

`Look` holds the controls that are not the world at all: scene gamma, reflection probe ambiance
and horizon clipping are renderer output. Giving them a named home stops them accreting onto Sky
and gives future exposure or post dials somewhere to land. Look is also the only tab with no
position on the vertical axis, which is a decent confirmation it is carved correctly.

Templates live as a dropdown on Track rather than as their own tab. Seeding a world archetype sets
tracks, deck heights, water height and celestial bodies together - a sky world is not one setting,
it is water at 3km and a deck below the landmass and another above - but it is a track operation
and does not earn top-level rank.

### Renames and why

`Moisture Level`, `Droplet Radius` and `Ice Level` are not scattering dials. `skyF.glsl` multiplies
the rainbow map lookup by `moisture_level` and the 22-degree halo lookup by `ice_level`, and
droplet radius sets the arc geometry between them. They become **Rainbow**, **Rainbow Width** and
**Ice Halo** under a `Rainbow & Halo` header, which also reconnects them to the influence floater's
existing "Rainbows after rain" checkbox that has been driving a slider labelled "Moisture Level"
this whole time.

The UI prefix `atmo_moisture_level` becomes `atmo_rainbow` to match. The asset field stays
`mSkyMoistureLevel` and the LLSD key stays `moisture_level`: the serialised form is EEP's and must
not drift, so the UI name and the storage name diverge deliberately.

`Volumetric Field` and `Sky Dome` become `Main Deck`, `Under Deck` and `Dome` - what they are in
the sky, not which code path draws them.

The Weather Influence floater's haze row becomes **Rain thickens water fog** under a `Water`
header. It gated moisture -> haze (haze density up, distance multiplier down) as well as
precipitation -> water fog, and the haze half is retired: on skies authored with heavy haze the
+1.5 haze density lifted the fog term's airlight past what the tonemapper could hold and whole
scenes read as overexposed. The authored haze density and distance multiplier now render exactly
as keyframed, and the row is what it actually still gates. The asset fields follow
(`mHazeEnabled`/`mHazeStrength` become `mWaterFogEnabled`/`mWaterFogStrength`, LLSD keys
`water_fog_*` with the old `haze_*` keys still read); the mapping removal lives in
ssatmoenvskymodulator.cpp.

`Precipitation` is kept. It reads as forecast language, which is intended, and "stuff falling from
the sky onto people" covers the artillery case honestly enough that renaming it to a
mechanism-neutral word would cost more in tone than it gains in reach.

## The altitude rail

The floater already carries a vertical multislider on the left whose thumbs are the track selector.
It gains a second mode instead of a second widget, because the rail is already doing two jobs
(altitude and selection) and adding a third kind of object to the same markers is the exact
conflation this rework removes.

| | Track mode (Track tab) | Layer mode (every other tab) |
| --- | --- | --- |
| Scale | fixed 0-4096, increment 64 | fitted to the selected track's contents |
| Markers | every track, toggling, selecting | water, main deck, under deck, weather bracket |
| Anchors | none | Space at the top, Dome directly below, both out of scale |
| Buttons | Add Track / Remove Track | Add Deck / Remove Deck |
| Click | selects that track | selects that layer and switches to its tab |

```
  +---+
  | * |  Space                    fixed anchor, selects Space
  +---+
  | ~ |  Dome        6000 m       fixed anchor, height as text, selects Clouds > Dome
  |===|  --------------------     fit range top
  | # |  Main Deck    800 m       selects Clouds > Main Deck
  | : |   weather                 bracket, selects Weather
  | = |  Water         20 m       selects Water
  |   |  -- track floor 0 m --    baseline tick
  | # |  Under Deck  -200 m       only when enabled
  +---+  --------------------     fit range bottom
   [+ Add Deck]  [- Remove Deck]
```

A fixed scale cannot serve both a coastal build with everything inside 100m and a sky archipelago
spanning 10km, and a piecewise scale is worse: a rail where the same mouse travel means 64m at one
end and 800m at the other is unpredictable to drag, breaks the increment model, and makes
`overlap_threshold` enforce a different pixel gap per segment. Fitting to content keeps the scale
linear everywhere and the precision uniform, and the mode switch means the author never has to ask
for a zoom level - Track mode shows the region, every other tab shows the track you are inside.

The transition interpolates over roughly 200ms on both the scale and the marker cross-fade, so it
reads as diving into the selected track rather than as the widget swapping its contents. Re-fitting
happens on drag commit, never during a drag, so the axis cannot move under the cursor.

The fit reads decks through the same auto derivation the renderer resolves, so a deck with `Auto`
ticked tracks its height rather than the stale authored row it greys out. The fit's own bottom and
top round to the nearest 256m: an auto deck's height wanders with moisture and convection, and an
exact fit would glide the whole scale under every step of that drift, where a quantised one only
moves once the content crosses a block boundary.

Space and Dome are fixed anchors excluded from the fit. The dome is a backdrop rather than a placed
layer, its height is frequently on `mAuto`, and a cirrus dome at 6km would otherwise squash the
decks being edited into the bottom eighth of the rail.

Sky and Look have no marker. Sky is the medium filling the band rather than an object in it; Look
is viewer-facing output. Both leave the rail in layer mode holding context with nothing highlighted.

### The weather bracket

Precipitation occupies a span, so it draws as a bracket rather than a thumb. The layer rail and
everything on it live in the track's own frame: the water plane's height and both decks' base
heights are metres relative to the track floor (`SSAtmoEnvTrack::mFloorZ`), negative below it, and
the renderer's resolvers add the floor back where they render (`visibleWaterHeight()`, the cloud
field resolver). The reference surface is therefore `max(0, water height)` in the rail's frame,
with the track's own `mWater.mEnabled` deciding whether the water term counts at all. The top is
the delivering deck.

The delivering deck defaults to the lowest enabled deck above the reference surface, which resolves
correctly for the sky case by construction - the under deck hangs below the platform floor, so it
is under the surface and the main deck delivers. A **Weather falls from** dropdown at the top of
Weather > Precipitation lets an author override it, for the case of wanting weather from the upper
deck while a lower one is enabled for looks. It is not animatable. If the selected deck is disabled
or removed it reverts to the main deck rather than leaving weather with no source.

## Precipitation types: two tiers

There is one preset tier where there need to be two.

| Tier | Edited by | Stored in | Editor |
| --- | --- | --- | --- |
| Shipped types | us | viewer install | `floater_ss_atmo_preset`, kept as the dev tool it is |
| Environment types | authors | the Atmo environment asset | new floater launched from Weather |

`floater_ss_atmo_preset` already holds archetype (liquid, flake, solid, riser), drop shape,
emissive, shatter, the three render tiers, landing rings, splash crowns, texture lists and sound
packs. That is the most theme-defining editor in the system and it is currently reached through the
viewer settings floater, two floaters away from the world it describes. Authors get their own tier
reached from Weather > Precipitation: the type combo lists shipped types and this environment's
own, visually distinguished, with **New from this...** deriving a local type and **Edit...**
opening it.

A derived type carries a full copy rather than a reference to its parent, so that a viewer update
retuning stock rain cannot silently change a shipped region. For the same reason, shipped type
definitions are copied into the asset on save as well: every environment is then self-contained and
a keyframe referencing a type that a given build does not have becomes impossible rather than
needing a fallback policy.

Camera shock and impact sound land in that editor as disabled stubs, revealed when the archetype is
impact-capable, so the UI documents the intent before the feature exists.

## What the rework touched

**The panels.** `panel_ss_atmo_env_atmosphere*.xml` became `panel_ss_atmo_env_sky.xml` with
`_sky_scattering` and `_sky_light` beneath it; `panel_ss_atmo_env_planetary.xml` became
`_space.xml` and took star brightness with it; `_clouds_volumetric.xml` became `_clouds_main.xml`;
`panel_ss_atmo_env_look.xml` is new; and the single Weather panel split into `_weather_conditions`,
`_weather_precipitation` and `_weather_lightning` behind a thin container.

Relocating a control between panel files needed no C++ at all - the floater reaches everything
through recursive name-based `getChild`, so a name that stays unique and still exists somewhere in
the tree keeps working. The only code change the whole regrouping pass required was one row prefix,
`atmo_moisture_level` to `atmo_rainbow`.

**The rail.** `railCentreForValue()` now maps through `mRailMin`/`mRailMax` rather than reading the
slider's own range, which is what lets the scale animate. `refreshRailMode()` reads the selected
tab and does the widget swap; `refreshLayerRail()` populates the layer markers; `draw()`
smoothsteps the range across the mode change. Thumbs are only added once the zoom settles, because
the slider clamps values to its own range and would otherwise snap a deck sitting outside the
interpolated window onto its edge and write that back. `LLMultiSlider` gained a
`setOverlapThreshold()` setter: the authored 304m gap is sized for the 0-4096m scale and rejects
every marker on a fitted one.

**Precipitation tiers.** `SSAtmoEnvAsset::mPrecipitationTypes` holds the environment's own types as
serialised `SSPrecipPreset` documents, staged into the live preset list by
`ssAtmoEnvStagePrecipTypes()` when an environment is adopted and dropped on unload.
`ssAtmoEnvEmbedReferencedPrecipTypes()` runs at save. `SSAtmoEnvBridge::presetNameForType()` now
passes unrecognised names straight through instead of returning an empty string, which is what lets
an author-named type resolve at all; a name that resolves to nothing still falls back to the active
preset exactly as before.

The editor is `SSFloaterPreset` in a second scope rather than a second floater. Opened with a map
key carrying `scope: environment`, the same widgets read and write the asset instead of disk, and
the viewer's own running precipitation is left alone. One editor rather than two so the tiers
cannot drift apart field by field; a bare string key is still the viewer-scope call it always was.

## Still to do

Camera shock and impact radius exist as disabled stubs on the preset editor's Impact tab. Nothing
reads them yet.

The world template values in `ssAtmoEnvTemplates()` are starting points rather than authored
presets. They place a coherent stack for each archetype - the sky archipelago puts water 2000m
below the track floor with an under deck 900m above it and the main deck 2600m up, the barrage
puts a thick dark deck 700m up (all heights floor-relative) - but the colours and weather numbers
want dialling against the real renderer.

## Deliberately not done

Accordions were considered and rejected: the panels are navigated often enough that managing
open/closed state is a cost, and the wide keyframe-button rows do not survive the squeeze. Grouping
is done with in-panel headers, which the panels already use.

A basic/advanced disclosure was considered and rejected: hiding EEP-parity dials treats the author
as a novice and papers over grouping problems rather than fixing them.

Generalising `mCloudField` and `mUnderField` into a vector of decks is deferred. Two named decks
with distinct semantics - the main deck always on and emptying via coverage, the under deck opt-in
and seeded off - is what the sky-build case needs, and the duplicated `cloud_*` / `ucloud_*` control
sets are already built and working. The rail's Add Deck and Remove Deck read as generic but bottom
out on the under deck's enable flag. If a third deck is ever wanted, the rail does not change: only
the asset gains a vector, `fromLLSD` gains a compatibility path for the old `cloud_field` and
`under_field` keys, and the deck panels collapse into one rebound to a selection.
