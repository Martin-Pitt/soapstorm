# Modern OpenGL: Approaching Zero Driver Overhead, Bindless Textures

Project code name **PTY** (owner, 2026-09-03) - the "approaching zero driver overhead"
pack for this viewer. Settings are `SSBindlessTextures`, `SSPersistentBuffers` and
`SSMultiDrawIndirect`, all default ON, all individually disableable, all silently
falling back to the legacy path on hardware that does not support the feature.

Status: NEW. Implements three pillars of the SIGGRAPH "Approaching Zero Driver
Overhead" (AZDO) agenda on top of the existing Firestorm GL 4.x plumbing:

1. **Bindless textures** (GL_ARB_bindless_texture) - textures stop being bound to
   texture units. A 64-bit opaque handle is passed straight into the shader's
   sampler uniform with `glUniformHandleui64ARB`, eliminating the per-draw
   `glActiveTexture`/`glBindTexture` pair (and the matrix-state flush that every
   `LLTexUnit::bind*` triggers through `gGL.flush()`).
2. **Persistent mapped buffers** (GL_ARB_buffer_storage, core 4.4) - the pooled
   vertex/index buffer backing store is allocated once with `glBufferStorage`
   (`GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT`) and keeps a
   permanent CPU mapping. The per-frame `glBufferSubData` upload calls disappear;
   dirty-region flushes become `memcpy`s into the mapping.
3. **Multi-draw indirect** (GL 4.3 `glMultiDrawElementsIndirect`) - the deferred
   simple opaque pass groups consecutive draws that share shader, vertex buffer,
   diffuse texture, model matrix and texture matrix into one indirect draw
   submission, removing one `glDrawRangeElements` submit per object.

## Verdicts (TLDR)

1. All three features are **opt-in capability gated and never load-bearing**: the
   identity of the renderer is unchanged when the GL version/extension check or any
   of the three settings fail. The legacy path (`LLTexUnit`, `glBufferSubData`,
   `glDrawRangeElements`) is untouched and remains the fallback. This matters
   because the viewer still ships GL 3.3/4.0 fallbacks (feature masks at
   `llfeaturemanager.cpp:749-783`) and `RenderMaxOpenGLVersion` walk-downs can leave
   `mGLVersion` at 3.3 on real hardware.
2. **Bindless binds through the existing `LLTexUnit::bindFast` entry point, driven by a
   per-program unit-to-uniform map** (`LLGLSLShader::mBindlessUnitToUniform`, built at
   link time from the reserved-uniform channel assignments). Any texture unit that a
   bound program exposes as a scalar sampler becomes a handle bind; every draw-loop path
   that ends in `bindFast` (the simple/alpha/bump/materials pools, `pushBatch`, the
   GLTF-adjacent unit binds) is covered without per-pool changes. Slow `bind()`/
   `unbind()`/`unbindFast()` keep their classic semantics: they reset the sampler to its
   unit first, so upload entry points (`setImage` + `glTexSubImage2D`) and "unbound"
   white fallbacks stay correct. Sampler ARRAYS (the indexed-texture `tex0..N` channel
   path) are never handle-bound and stay unit-driven, which is legal under the
   extension. Render targets are never handle-bound (`bindManual` path unchanged), which
   keeps per-bind filter overrides on RTs intact.
3. **Persistent mapping keeps the CPU mirror** (unlike a from-scratch
   persistent-upload rewrite). The pool's `data` pointer becomes the persistent
   mapping; `flush_vbo` becomes a no-op (writes already landed coherently). Dirty-region
   coalescing, the frame-end flush, and the Apple path are unchanged. This is the
   smallest correct diff that removes the per-frame GL upload.
4. **MDI batches conservatively**: same shader (single-pass simple pool), same
   vertex buffer, same diffuse texture, same model matrix, same texture matrix, no
   matrix palette (rigged), no alpha-mask cutoff. Batches are therefore always
   render-identical to the classic path; there are no shader changes. Batch hit rate
   is modest because SDL sorts by (matrix, texture) and objects rarely share a
   vertex buffer, but the infrastructure (`LLGLMultiDraw`) is in place for a
   matrix-per-draw UBO pass later, which is the change that makes batching
   texture-oriented.
5. GLSL is told about bindless with `#extension GL_ARB_bindless_texture : enable`
   (not `require`) so a driver that advertises the extension but mis-compiles it
   degrades to classic texturing rather than failing shader load. The extension is
   only injected when the GL context is at least 4.1 with GLSL 4.10+ (the shader
   `#version` is bumped from 400 to 410 on a 4.1 context for the same reason).

## Capability gates

| Feature | GL gate | Extension gate | Function pointers |
|---|---|---|---|
| Bindless | `mGLVersion >= 4.19f`, GLSL >= 4.10 | `GL_ARB_bindless_texture` | `glGetTextureHandleARB`, `glGetTextureSamplerHandleARB`, `glMakeTextureHandleResidentARB`, `glMakeTextureHandleNonResidentARB`, `glUniformHandleui64ARB` (+ `v`) |
| Persistent buffers | `mGLVersion >= 4.39f` (buffer_storage is core 4.4) | - (extension fallback not needed) | `glBufferStorage` (already loaded), `glMapBufferRange` (already loaded) |
| Multi-draw indirect | `mGLVersion >= 4.29f` | - | `glMultiDrawElementsIndirect` (already loaded) |

Flags live on `LLGLManager` (`mHasBindlessTexture`, `mHasBufferStorage`,
`mHasMultiDrawIndirect`) following the existing `mHasBPTC` pattern in
`llgl.cpp:1483-1490`, and are exposed through `getGLInfo`/`asLLSD`.

## File map

- `indra/llrender/llgltypes.h` - `LLGLuint64` typedef.
- `indra/llrender/llglheaders.h` - bindless function pointer declarations for the
  LL_WINDOWS block (Linux/Mesa get them from glext.h prototypes) and the
  `LL_AZDO_GL_ENTRY_POINTS_AVAILABLE` guard for platforms that can never expose them
  (macOS tops out at 4.1).
- `indra/llrender/llgl.h`, `llgl.cpp` - flags, loader section (guarded by
  `mHasBindlessTexture`), LLSD info strings.
- `indra/llrender/llimagegl.h/.cpp` - bindless handle + residency management and
  invalidation (`getBindlessHandle`, `makeBindlessResident`, `makeBindlessNonResident`,
  `invalidateBindlessHandle`, `applyTexOptions`).
- `indra/llrender/llglslshader.h/.cpp` - `sUseBindlessTextures` static, the
  unit-to-uniform map built at link time, `bindTextureBindless`, `resetTextureUnitBinding`,
  per-uniform handle cache.
- `indra/llrender/llrender.h/.cpp` - `LLTexUnit::getAddressModeGL`, and the bindless
  branch in `bindFast` plus sampler resets in `bind`/`unbind`/`unbindFast`.
- `indra/llrender/llshadermgr.cpp` - GLSL `#extension` + `HAS_BINDLESS_TEXTURES`
  injection after `#version`, `#version 410` bump on 4.1 contexts.
- `indra/llrender/llvertexbuffer.cpp` - persistent-mapped path in
  `LLDefaultVBOPool::allocate/free/clean/clear` and `flush_vbo`.
- `indra/llrender/llmultidraw.h/.cpp` (new) - `LLGLMultiDraw` indirect command
  buffer helper. Added to `indra/llrender/CMakeLists.txt`.
- `indra/newview/lldrawpool.h/.cpp` - MDI batching in `LLRenderPass::pushBatches`
  (opt-in via `pushBatches`'s last parameter), thread-local batch state in `pushBatch`.
- `indra/newview/lldrawpoolsimple.cpp` - enables MDI for PASS_SIMPLE.
- `indra/newview/gltfscenemanager.cpp` - publishes its out-of-band glTexParameter
  sampler mutations to the bindless handle cache.
- `indra/newview/ssazdo.h/.cpp` (new) - settings refresh mirroring the Squeeze pattern.
- `indra/newview/llviewercontrol.cpp` - settings listeners (bindless toggle also
  reloads shaders).
- `indra/newview/llappviewer.cpp` - startup refresh in `settings_to_globals`.
- `indra/newview/app_settings/settings.xml` - the three settings.

## Residency policy (bindless)

Handles are created lazily on first bindless use and made resident via
`glMakeTextureHandleResidentARB`; residency is retained until the GL texture is
destroyed or the handle is regenerated (option state change). This matches the
common viewer practice of not per-frame de-residency churn. Lifetime management
(evicting residency of long-dead textures) is a follow-up.

Handle invalidation rules:
- `setAddressMode` / `setFilteringOption` bump the texture's state version on every call
  (both the deferred flag and the immediate-apply-to-current-unit branch mutate the GL
  object, so both can stale a handle).
- `createGLTexture` / `syncTexName` / `setTexName` / `setImage` / `setSubImage` /
  `destroyGLTexture` bump the state version. Note that data-only updates do not affect
  handle validity on any conformant driver (the handle names the resource + sampler
  state, not the image), so the bump is belt-and-braces for drivers that re-allocate on
  image upload.
- Out-of-band sampler mutations that bypass `LLImageGL` (the GLTF binder's direct
  `glTexParameter` calls) publish themselves through `invalidateBindlessHandle`.

## Verification

- Compile-checked with `cl.exe /Zs` against the existing MSVC build harness in
  `build-vc180-64` (llrender + newview units touched).
- Runtime verification is TBD on a machine with a GL > 4.1 context; fallbacks are
  exercised automatically on older hardware or with any of the three settings off.

## Follow-ups (not in this change)

- Matrix-per-draw UBO so MDI can batch across matrices (the actual big win).
- Texture-array / atlas path for ground terrain chunks.
- VAO caching for vertex-format setup (`glVertexAttribFormat` + `glBindVertexArray`),
  the largest remaining per-object CPU cost after this change.
- Bindless residency de-aging for long-lived sessions.
