# Atmo Magic: the Effects & LOD floater

`floater_ss_atmo_fx.xml`, registered as `ss_atmo_fx` in `llviewerfloaterreg.cpp`, class
`SSFloaterEffects` (`ssfloaterfx.cpp`). Opened from the Atmo Magic floater's own button
(`ssfloateratmo.cpp:45`).

## Why tabs

It was one column, 1008 pixels tall, `can_resize="false"`, with no scroll. On a 1080p
screen with a taskbar the lightning half was off the bottom edge and unreachable — the
floater was taller than the room it had, and nothing in its markup let a person do
anything about that. It had also collected two elements both named `impact_label`, which
is what happens when sections are appended to a list rather than placed in a structure.

Four tabs, split by **what a person is doing**, not by which subsystem owns the setting:

| Tab | Holds | Why together |
| --- | --- | --- |
| Rain | Density, particle budget, impact toggles, drop opacity, ripple size/speed/opacity, the water shader switch, glow master, drop roundness, streak contrast, sparkle | Tuning rain is one continuous act. You change the density, then the opacity, then the splash that the new density made too loud. Splitting that across tabs means paging back and forth mid-adjustment. |
| Lightning | Enable, strike triggers, pending-strike markers, 33 dials (the bolt's, then the ground strike's: amber zone, bead, plasma, late restrike, aura, flare, fire, crawl, sparks - `doc/atmo_magic_lightning_strike.md`), seasonal charge, bolt from the blue, hidden-ground-show skip, bolt texture | Nobody tunes lightning while tuning rain. This block alone is ~490 lines of markup and is the reason the floater outgrew one column. |
| Clouds | The volumetric field's three switches and the debug overlay | New tab. See below. |
| LOD | Precipitation's three distance tiers, the cloud field's density and puff budget | The one place to go when the weather costs more than the machine has. These are *how much to draw* decisions, and they belong together rather than each beside the look dials of the system it trims. |

Each tab is a `scroll_container` wrapping a fixed-height content panel, so the floater is
`can_resize="true"` and a tab taller than the window scrolls instead of being cut off.
Content rows kept their original geometry (`left="12"`, 368px wide); the floater widened
from 380 to 424 to leave room for the tab border and the scrollbar gutter.

## The Clouds tab

### The three switches were unreachable

`SSAtmoVolumetricClouds`, `SSAtmoCloudProceduralNoise` and `SSAtmoCloudTessellation` were
declared only by lazy `LLCachedControl` constructions inside `ssvolcloud.cpp`. That has two
consequences nobody wants:

- **They did not persist.** A control `LLCachedControl` declares itself gets no `Persist`
  flag, so every setting reverted on restart.
- **They did not exist until used.** The declaration happens on first execution of the
  function holding the static, so `SSAtmoCloudTessellation` was absent from Debug Settings
  until the cloud renderer had drawn at least one frame.

They are real `settings.xml` entries now, and they have a home in the UI.

### Tessellation

The toggle used to be `segs = tessellate ? 4 : 1` — one flat subdivision factor for every
puff at every range, with `skirt = puff.mAnvil * v²` as the only vertex displacement. It
subdivided, but it never *adapted*: a puff two hundred metres away and one four kilometres
away got the same sixteen sub-quads, so it cost 16× the vertices everywhere and bought
shaping only on anvil-bearing puffs. Nothing in it knew about camera distance or about
scene geometry.

It now does two things:

- **Subdivision by screen size.** `segs` comes from the puff's on-screen diameter — one row
  per `SS_TESS_PIXELS` (96), floored at 1 and capped at `SS_TESS_MAX_SEGS` (6). Measured in
  pixels because that is what "can it be seen" means, and off the *true* distance: the
  far-field squash moves every vertex along its own ray, so the projected size is the true
  one. A distant puff collapses back to the single card it always was; the cap exists
  because an overhead puff can fill the view and would otherwise outweigh the rest of the
  field.

- **Edge breakup** (`SSAtmoCloudEdgeBreakup`, default 0.25, fraction of puff radius). Worth
  being precise about why this works, because the obvious objection is that the visible
  silhouette is the fragment alpha carve rather than the quad outline. True — but the carve
  is a function of `vary_world`, the *interpolated world position*. Move a vertex and you
  move which piece of the cloud field that part of the card covers, so the carved outline
  moves with it. A rectangle of vertices therefore reads as a rectangle's worth of cloud,
  clipped wherever the field wanted to continue past the border. Pushing the rim vertices
  around in the billboard plane lets the same carve wander instead of clipping.

  Ramped by the squared distance from the quad centre, so the interior — where the texture
  content lives — barely moves and only the rim is disturbed. Keyed on the *shared* grid
  vertex, so neighbouring sub-quads displace identically and the mesh cannot tear. In-plane
  only: displacing along the view normal would move the puff in depth, past the sort that
  placed it.

The anvil skirt is unchanged and still gated on `mAnvil`. Note that tessellation is not a
no-op even where nothing displaces: `ssVolCloudV.glsl` does non-linear per-vertex work (the
`pow()` glow hotspot, the radial squash, `calcAtmosphericVars` along each vertex's own ray,
the `max_y/|view_dir.z|` slab path) and puffs span hundreds of metres, so a finer grid
interpolates the sky lighting and aerial perspective more finely across the card.

**Still not structure-aware.** Nothing in the cloud render path knows where buildings or
terrain are. The only proximity handling is `ss_soft_m` in `ssVolCloudF.glsl`, a
fragment-stage alpha fade against the depth copy so a puff thins as it approaches a surface
instead of ending on a hard edge. Doing that in geometry would mean vertex-stage reads of
the depth copy at billboard corners.

### Field density and budget

Both live on the LOD tab, because both are *how much to draw* decisions.

- **Field density** (`SSAtmoCloudPuffsPerCell`, default 3, range 1-8) is how many puffs the
  builder places in each 260m cell it keeps. Sub-puff index 0 is always placed, so lowering
  it thins the field toward one body per cell rather than opening holes in it — holes are
  the coverage gate's job and the two must not be confused. Past the squash knee the loop
  already collapses to one puff per cell, so this is a near-field cost buying near-field
  body.

  `CELL_M` itself is deliberately **not** exposed. `ssVolCloudF.glsl` replicates the cell
  grid verbatim (`SS_CELL_M = 260.0`) to run the builder's gate per fragment, so moving the
  grid would desync the carving from the geometry it carves. The sub-count is the builder's
  alone and nothing downstream replicates it.

- **Puff budget** (`SSAtmoCloudPuffBudget`) caps how many puffs a deck may draw, replacing
  what was a hardcoded `MAX_PUFFS = 1260`. Applied **per deck**, after the depth sort, so
  the erase takes the field's far edge and leaves the sky directly overhead whole — the same
  shape as precipitation's distance tiers. A sky with an under deck draws up to twice the
  budget. Clamped to `[64, 8000]`.

There is **no viewer-side per-puff opacity dial**. `mPuffDensity` is the sky's own authored
figure and belongs to the environment editor; a multiplier on top of it here would just be a
second place to set the same thing.

## The debug overlay

`LLPipeline::RENDER_DEBUG_CLOUD_FIELD`, dispatched from `pipeline.cpp` to
`SSVolCloud::renderDebug()`. Reachable two ways, sharing one switch: the checkbox on the
Clouds tab, and Develop → Render Metadata → Cloud Field. This is the arrangement the
Simulation floater already uses for its five overlays, and it is why this floater needed a
C++ class at all — the mask is not a setting and `control_name` cannot bind it.

`SSAtmoCloudDebugView` picks what it draws. Both views also get both decks' bands: the
floor and lid each field was built between, as a ring at the field's fade radius.

| View | Shows | Answers |
| --- | --- | --- |
| 1 Cells and towers | The builder's 260m air-frame grid replayed across the field: green where the gate passed and cloud was placed, red where it did not, with a stalk as tall as the noise map's tower weight wants that column to climb | Where the holes come from, and whether the map's convection geography is the shape you meant |
| 2 Column profiles | For cells near the camera that the gate kept, the column that cell grows, outlined at its true position and altitude. Half-width at each height is exactly what the builder gives a puff there — waist, flare, the profile ramp's anvil term and the deck-wide one, replayed off the deck rather than approximated. Hue is the anvil figure in force at that height, so the altitude the ramp takes over at reads as the colour changing partway up | What the profile ramp actually does to a cloud's shape, standing beside the cloud it shaped |

Both views walk cells rather than puffs. Per-puff quad outlines were tried and cut: at ~2500
puffs they occlude the sky they are describing, and a rectangle per puff says nothing a
person could not already see. The cell grid is the register the field is actually authored
in, and it is legible.

View 2 uses a small neighbourhood (11x11 cells) on purpose. A column outline is a legible
thing and a thousand of them are not; whole-field geography is view 1's job.

### The squash

Every mark goes through `squashScale()`, the same far-field compression
`ssVolCloudV.glsl` applies to the field itself. Without it two things break: the marks land
kilometres behind the cloud they describe, and the far half of a 5km field never survives a
2km far plane to be drawn at all. Lines are subdivided *before* they are squashed, because
the squash is not linear along a segment and the band rings span kilometres.

## Related

- `doc/atmo_magic_env_ui.md` — the environment editor, which authors what these dials scale.
- `doc/atmo_magic_cloud_parallax.md` — the cloud field's rendering.
- `doc/viewer/ui_system.md` — floater registration and XUI mechanics.
