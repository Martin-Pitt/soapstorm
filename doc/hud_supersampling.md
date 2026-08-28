# HUD supersampling

Antialiasing for HUD attachment geometry, controlled by `RenderHUDSupersample` (1 = off, 2 or 4 = supersample factor).

## Why HUDs have no antialiasing

`render_hud_attachments()` in `indra/newview/llviewerdisplay.cpp` is called from `display()` *after* `gPipeline.renderFinalize()`. FXAA and SMAA both live inside `renderFinalize`, ahead of the final present blit, so by the time HUD geometry is drawn the post chain has finished and resolved to the default framebuffer. The pipeline has no MSAA either — `RenderFSAAType` selects none, FXAA, or SMAA, all of them post-process. HUD attachments therefore land in a single-sample buffer with no antialiasing of any kind.

## Why supersampling rather than post AA

Running FXAA or SMAA over the HUD would only address polygon silhouettes, and it would do that badly: both detect edges from luma, and a HUD render is mostly transparent, so there is no meaningful background to find edges against.

Most visible HUD jaggedness is not on polygon edges at all. It is on the alpha edges inside the texture — a transparent PNG magnified across a large screen region. Supersampling antialiases those too, because it supersamples the shading, including the texture fetch. HUD geometry is trivial and low-overdraw, so the fill cost is affordable in a way it would not be for the world.

MSAA on a HUD-only target was the alternative considered. It is cheaper, but it only covers geometry edges, which is the less noticeable half of the problem.

## The alpha channel problem

This is the part that makes the feature more than "render bigger, then average".

Compositing a supersampled HUD requires the offscreen target to hold **coverage** in its alpha channel. The main render path does not produce that, for three separate reasons:

1. `LLPipeline::renderGeomPostDeferred` sets `gGL.setColorMask(true, false)` — alpha writes are masked off for the whole pass. Nothing reaches the alpha channel at all.
2. Where alpha *is* written, the pipeline treats destination alpha as **glow**, not coverage. `LLDrawPoolAlpha` sets its alpha blend factors to `(BF_ZERO, BF_ONE_MINUS_SOURCE_ALPHA)` for glow suppression, and the emissive pass switches to `(BF_ONE, BF_ONE)` to accumulate glow additively.
3. The HUD passes that draw with `GL_BLEND` off write whatever their shader emits straight into alpha. `pbropaqueF.glsl` emits a hard `0.0` (glow suppression); the fullbright alpha-mask variant emits the diffuse texture's own alpha.

None of this is wrong for the direct-to-screen path — the alpha channel there is genuinely dead, since glow is generated inside `renderFinalize`, which has already run. It only becomes a problem once something wants to read that channel back.

### How it is fixed

**`LLRender::setCoverageAlphaMode(bool)`** (`indra/llrender/llrender.cpp`) is the override, applied at the single choke point every draw pool funnels through. The draw pools reset blend state and colour masks repeatedly mid-pass, so patching each pool individually would be both invasive and easy to get wrong as pools change. While the mode is set:

- `setColorMask` forces alpha writes on — but only when colour writes are also on. A pass that writes no colour (depth-only prepasses, invisiprims) contributes no coverage either, and forcing alpha on for those would stamp opaque holes into the target.
- `blendFunc` forces the alpha channel to `(BF_ONE, BF_ONE_MINUS_SOURCE_ALPHA)`, the "over" operator, regardless of what the pool asked for.

The colour factors are left alone. Against a target cleared to `(0,0,0,0)`, the pool's own `(BF_SOURCE_ALPHA, BF_ONE_MINUS_SOURCE_ALPHA)` accumulates **premultiplied** colour, which is what the resolve wants — so the composite back to the screen is a straight `(BF_ONE, BF_ONE_MINUS_SOURCE_ALPHA)` with no source alpha multiply. Getting this wrong is the classic way to produce dark fringing around every HUD edge.

Because the override rewrites what the caller asked for, `LLRender` also records the pre-override request (`mRequestedAlpha*`). Toggling the mode re-issues that request through the new override so GL and the shadowed state agree again; the `mColorMaskDirty` flag and the `BF_UNDEF` sentinel exist to defeat the early-outs in both setters.

**The `hud_coverage` uniform** handles case 3, the passes that draw with `GL_BLEND` off and so bypass the blend override entirely. `gHUDFullbrightAlphaMaskProgram` and `gHUDPBROpaqueProgram` take a float that is 0 in the direct path (preserving today's behaviour exactly) and 1 under supersampling.

This is safe whichever way the blend state actually sits when those pools run. With blending off — which is what `pbropaqueF.glsl` emitting a hard `0.0` alpha implies, since a `BF_SOURCE_ALPHA` blend against it would make PBR HUDs vanish entirely — the uniform only decides what lands in alpha and cannot touch colour. With blending on, forcing alpha to 1 turns the blend into a replace, which is the intended semantics of an opaque pass anyway.

The blend-enabled HUD passes need no equivalent. Their real alpha is simultaneously the correct colour blend factor and the correct coverage contribution: a face that is half transparent should contribute half coverage, and the coverage-mode blend accumulates exactly that.

**The emissive pass** in `LLDrawPoolAlpha::renderAlpha` is skipped for HUDs. It has never done anything there — it writes glow into a buffer that nothing samples, because glow generation happens before HUD attachments draw. Skipping it is free in the direct path and necessary under supersampling, where its additive alpha blend would punch through the coverage channel.

## The resolve

`hudDownsampleF.glsl` box filters each factor × factor source block into one destination pixel. Both premultiplied colour and coverage are linear in coverage, so an unweighted average is the correct resolve for each. Sampling is point filtered so every tap lands on exactly one source texel; bilinear taps would smear neighbouring blocks in.

## Known limitations

- **The resolve averages in display space.** HUDs render after `renderFinalize` has gamma corrected, so the target holds sRGB-ish values and the box filter averages those rather than linear light. This is slightly incorrect and consistent with how FXAA already behaves in this pipeline. Not worth chasing.
- **Scope is the geometry pass only.** `render_hud_elements()` stays outside the supersampled target. It draws text and selection overlays sized in screen pixels, which would come out at half scale in a target this size. It is documented as "stuff without z writes", so it has no depth dependency on the attachment geometry it now no longer shares a depth buffer with.
- **Depth is cleared to 1.0** in the offscreen target, where the direct path inherited whatever depth the present pass left behind. This is strictly more permissive; HUD-vs-HUD ordering is unaffected.
- **Memory.** The target is world-view sized times the factor, RGBA plus depth. At 1080p and 2x that is roughly 33 MB each; 4x on a 4K display is far larger, which is why the factor steps down automatically when it would exceed `mGLMaxTextureSize`, and why the default is 2.

## Where the pieces are

| Piece | Location |
| --- | --- |
| Setting | `RenderHUDSupersample` in `indra/newview/app_settings/settings.xml` |
| Target, begin/resolve | `LLPipeline::beginHUDSupersample` / `endHUDSupersample`, `indra/newview/pipeline.cpp` |
| Call site | `render_hud_attachments()`, `indra/newview/llviewerdisplay.cpp` |
| Coverage override | `LLRender::setCoverageAlphaMode`, `indra/llrender/llrender.cpp` |
| Resolve shader | `indra/newview/app_settings/shaders/class1/deferred/hudDownsampleF.glsl` |
| Coverage uniform | `fullbrightF.glsl`, `pbropaqueF.glsl` |
| Emissive skip | `LLDrawPoolAlpha::renderAlpha`, `indra/newview/lldrawpoolalpha.cpp` |
