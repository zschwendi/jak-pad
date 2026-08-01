# Metal renderer path (GOALPad)

Status labels follow AGENTS.md: **Implemented**, **Experimental**, **Planned**, **Investigating**.

This document records the evidence-based inventory of OpenGOAL's OpenGL renderer, the
chosen path to Metal on iPadOS, and the current state of the backend seam.

## 1. Inventory of the existing renderer

All code lives under `game/graphics/` (~53.7k lines of C++/headers, measured with `wc -l`).
Rendering is organized as *bucket renderers*: the game produces a PS2-style DMA chain,
`OpenGLRenderer::render(DmaFollower, RenderOptions)` walks it and dispatches each bucket
to a renderer (`game/graphics/opengl_renderer/OpenGLRenderer.h:66-76`).

| Renderer | Purpose | Needed for Jak 1 |
|---|---|---|
| `OpenGLRenderer` | top-level: FBO/MSAA state, bucket dispatch per game, post/PCRTC effects, screenshots | yes |
| `DirectRenderer` / `DirectRenderer2` | GS-style immediate emulation (sky pre-draw, progress, HUD, debug) | yes (DR1) |
| `TextureUploadHandler` + `texture/TexturePool` | GS VRAM texture upload emulation | yes |
| `background/TFragment` | static terrain ("tfrag") | yes |
| `background/Tie3` | instanced environment geometry incl. wind ("tie") | yes |
| `background/Shrub` | vegetation | yes |
| `background/Hfrag` | heightmap terrain | no (Jak 3) |
| `foreground/Merc2` | skinned character meshes + envmap ("merc"/"emerc") | yes |
| `foreground/Generic2` | VU1 "generic" fallback path | yes |
| `foreground/Shadow2`, `ShadowRenderer` | shadow volumes (Shadow2 = Jak 2/3, ShadowRenderer = Jak 1) | ShadowRenderer |
| `sprite/Sprite3` (+`_Distort`, `_Glow`, `GlowRenderer`) | particles, screen distortion, glow probes | yes |
| `ocean/*` | ocean surface near/mid/far, generated ocean texture, envmap | yes |
| `SkyRenderer`, `SkyBlendCPU`/`SkyBlendGPU` | sky texture blending (CPU and GPU variants) | yes (CPU variant suffices initially) |
| `EyeRenderer` | renders eyes into small textures | yes |
| `DepthCue` | Jak 1 full-screen depth-cue effect | yes |
| `CollideMeshRenderer` | debug collision view | debug only |
| `TextureAnimator` | GPU texture animation (CLUT ops, 1D LUTs) | no (Jak 2/3) |
| `BlitDisplays`, `Warp`, `SlowTimeEffect`, `ProgressRenderer`, `VisDataHandler` | Jak 2/3 framebuffer effects, progress menu, vis data | partial |
| `loader/Loader` | background asset streaming; GL only in upload stages | yes |
| `pipelines/opengl.cpp` | SDL3 window + GL context, vsync/DMA thread sync, imgui, `GLDisplay` | replaced per-backend |

### Actual GL feature level

- Requested context: GL **4.3 core** on Windows/Linux, GL **4.1 core** on Apple
  (`game/graphics/pipelines/opengl.cpp:135-140`). The whole renderer already runs within
  GL 4.1 on macOS today, so 4.1 core is the true upper bound of required features.
- All 92 shader files declare `#version 410 core` (every file under
  `game/graphics/opengl_renderer/shaders/`).
- **Not used anywhere** (verified by grep over `game/graphics/`): compute shaders, SSBOs,
  geometry/tessellation shaders, texture buffers, bindless, indirect draws, buffer storage,
  logic ops.
- Features that are used and their mapping risk:
  - `glMultiDrawElements` — background renderers (`background/TFragment.cpp:490,510`,
    `background/Tie3.cpp:604,624,697`, `background/Shrub.cpp:367,388`). Metal: loop of
    `drawIndexedPrimitives` (later: indirect command buffers). Trivial.
  - Primitive restart with index `UINT32_MAX` — 13 files (e.g. `DirectRenderer2.cpp:142`,
    `background/TFragment.cpp:438`). Metal restarts strips on `0xFFFFFFFF` natively.
  - 1D textures for time-of-day palettes, sampled in vertex shaders
    (`shaders/tfrag3.vert` `uniform sampler1D tex_T10`; used in TFragment, Tie3, Shrub,
    Hfrag, TextureAnimator). Metal has native 1D textures and vertex sampling.
  - Instancing — only `sprite/Sprite3_Distort.cpp` (`glDrawArraysInstanced`,
    `glVertexAttribDivisor`). Native in Metal.
  - UBOs — Merc2 bone matrices via `glBindBufferRange` (`foreground/Merc2.cpp:62-74,1326`)
    and `CollideMeshRenderer.cpp:173-254`. Metal: `setVertexBuffer:offset:`.
  - FBOs + `glBlitFramebuffer` + MSAA resolve — `OpenGLRenderer.cpp` (game framebuffer +
    resolve), `opengl_utils.cpp` (`FramebufferTexturePair`), `DepthCue.cpp`,
    `SkyBlendGPU.cpp`, `sprite/GlowRenderer.cpp`, `sprite/Sprite3_Distort.cpp`. Metal:
    render passes + blit encoder / resolve attachments.
  - `glPolygonMode(GL_LINE)` — debug only (`ShadowRenderer.cpp:386`,
    `CollideMeshRenderer.cpp:330`). Metal: `setTriangleFillMode:MTLTriangleFillModeLines`.
  - `glGetTexImage` — `TextureAnimator.cpp:68,2780` (Jak 2/3 debug); replaceable with
    blit-to-buffer readback.
- GL-specific state model that needs deliberate mapping: the renderers mutate global
  blend/test/mask state per draw (PS2 GS alpha-blend equations, alpha test via shader
  discard, fog). In Metal, blend state is baked into PSOs, so the port needs a small
  PSO cache keyed on (shader, blend mode, target format) — a well-understood pattern.

### Shaders

92 GLSL files = 46 vertex/fragment programs, ~3.1k lines total. They use only GLSL 4.10
core features (no compute, no geometry, no SSBO). Translation to MSL is mechanical:
uniforms become argument-buffer/`setBytes` constants, `sampler1D/2D` become
`texture1d/2d` + sampler, `discard` maps directly. The build step for this is
*Implemented*: checked-in `.metal` sources under
`game/graphics/pipelines/metal/shaders/` are compiled at build time by
`xcrun metal` into a metallib, embedded into the runtime as a byte array
(`embed_metallib.cmake`), and loaded with `newLibraryWithData:` — no runtime
shader generation or compilation, which keeps the iPadOS signing story clean.
The GLSL→MSL translation of the 92 game shaders happens per bucket renderer as
each is ported.

### The seam

The renderer already sits behind a backend module interface:

- `GfxRendererModule` (`game/graphics/gfx.h:31-54`): `init`, `make_display`, `exit`,
  `vsync`, `sync_path`, `send_chain`, texture upload/relocate, level set/reload hooks.
- Backend selection: `Gfx::GetRenderer(GfxPipeline)` (`game/graphics/gfx.cpp:33`).
- Display abstraction: `GfxDisplay` (`game/graphics/display.h:21`), implemented by
  `GLDisplay` in `pipelines/opengl.cpp`.

Everything **below** this seam (bucket renderers) calls GL directly; there is no per-draw
RHI, and none should be invented. The correct unit of porting is the bucket renderer:
each is a self-contained class with its own shaders and buffers.

Windowing/SDL coupling is confined to `pipelines/opengl.cpp` (SDL3 window + GL context +
imgui + vsync threads). SDL3 also provides Metal views (`SDL_Metal_CreateView` →
`CAMetalLayer`), which the Metal pipeline uses on desktop; on iPadOS the same layer can
come from a `UIView`/SwiftUI instead of SDL.

## 2. Candidate strategies

| Strategy | Verdict | Reasoning |
|---|---|---|
| **Direct Metal backend behind `GfxRendererModule`** | **Recommended** | Feature level is low (GL 4.1, no compute/SSBO/geometry) and every used feature maps natively to Metal (see above). The seam already exists; bucket renderers port one at a time while the GL renderer keeps working on desktop as the reference implementation. Matches the project's stated long-term direction and works identically on iPadOS. |
| ANGLE (GLES-on-Metal) | Viable fallback, not chosen | Feature usage fits GLES 3.0 after mechanical fixes (multidraw → loops, `sampler1D` → 2D, restart index already `0xFFFFFFFF`, polygon-mode lines is debug-only, `glGetTexImage` → FBO readback) plus a 92-file GLSL→ESSL port. But it adds a very large vendored dependency (its own GN/CMake build), a translation layer on every draw, and still requires touching most shaders — a significant fraction of the Metal port's work while ending at a dead end relative to the preferred direct-Metal end state. Keep as contingency: it is *not* blocked by any feature gap. |
| MoltenVK + Vulkan backend | Rejected | There is no Vulkan backend in this tree to reuse; one would have to be written from scratch — strictly more work than writing the Metal backend directly, then shipped through an extra translation layer with its own iOS caveats. No advantage for an Apple-only target. |
| GL-to-Metal translation layers (Zink et al.) | Rejected | Zink targets Vulkan (needs MoltenVK underneath → double translation); no production GL 4.1-on-Metal layer is shippable inside an ordinary signed iPadOS app. |

## 3. Staged plan to game frames on iPad

1. **Backend seam + first Metal frame** — *Implemented* (this change): `GfxPipeline::Metal`,
   `gRendererMetal` in `game/graphics/pipelines/metal/`, textured depth-tested frame
   rendered and pixel-verified via `metal-proof`.
2. Frame scaffolding — *Implemented*: per-frame command buffer, offscreen game render
   target + letterboxed present pass (mirror of `OpenGLRenderer::setup_frame` /
   `do_pcrtc_effects`, including brightness/contrast and pmode-alp blackout), PSO +
   depth-stencil caches, build-time MSL shader pipeline. MSAA resolve is still planned
   (the PSO key already carries a sample count).
3. Texture path — *Implemented* (see §Current state): backend-neutral TexturePool
   backed by an `MTLTexture` registry, uploads with GPU-generated mips, a sampler-state
   cache, the `texture_upload_now` / `texture_relocate` module hooks, and the
   TextureUploadHandler DMA walk. The handler's eye-renderer and texture-animator
   sub-paths ride with their renderers in later stages.
4. First game visuals — *Implemented* (see §Current state): `send_chain` consumes the
   game's DMA chain (copied per frame like the GL pipeline), the Jak 1 bucket table
   dispatches all 70 buckets, and DirectRenderer + SkyRenderer + SkyBlendCPU render
   title-screen-class content. Verified with constructed chains that follow the real
   Jak 1 chain structure, including real extracted sky texels; not yet with a live
   game or captured retail chain (none exists in this tree).
5. Background geometry — *Planned*: TFragment, Tie3, Shrub (+ time-of-day 1D LUTs,
   multidraw loops).
6. Foreground — *Planned*: Merc2 (bones via buffer offsets), Generic2, ShadowRenderer,
   EyeRenderer.
7. Sprite/effects — *Planned*: Sprite3, distort, glow, DepthCue, ocean, post effects.
8. iPad presentation — *Planned*: drive the same backend from a `CAMetalLayer` provided by
   the SwiftUI app instead of SDL; controller/input wiring; simulator first, then physical
   device per AGENTS.md gates.
9. Performance pass — measured on hardware only after correctness.

**Major risks**
- GS blend/alpha-test/fog fidelity: the GL renderers encode PS2 semantics in state +
  shaders; ports must copy that math verbatim and be diffed against the GL output
  (macOS can run both backends side by side — this is the main reason to keep GL healthy).
- PSO churn: per-draw blend changes must become a PSO cache; needs early design, low risk.
- Threading: `send_chain`/vsync synchronization model in `pipelines/opengl.cpp` must be
  reproduced exactly; subtle deadlock potential.
- imgui debug tooling: imgui ships a Metal backend, but wiring it is extra work; debug
  UI can lag the game path.

## 4. Current state

- **Implemented**: `GfxPipeline::Metal` seam and the stage-2 frame scaffolding under
  `game/graphics/pipelines/metal/`:
  - `metal_pipeline.mm` — SDL3 window/`CAMetalLayer` setup and `GfxRendererModule` glue.
  - `metal_renderer.{h,mm}` — per-frame structure: one command buffer per frame carrying
    the game pass(es) into an offscreen game-resolution target
    (BGRA8 + Depth32Float_Stencil8, cleared like Jak 1's `setup_frame`: color 0, depth 0,
    PS2-style reversed depth with GEQUAL) followed by the PCRTC-style present pass that
    draws the game frame into the centered letterboxed draw region of the drawable with
    the POST_PROCESSING brightness/contrast math and the pmode-alp blackout.
  - `metal_pso_cache.{h,mm}` — PSO cache keyed on (shader, sample count, color/depth
    formats, blend enable/ops/factors, color write mask) plus a separate
    `MTLDepthStencilState` cache keyed on (test, compare, write). Ported renderers
    request GL-style per-draw state; each distinct combination is baked once and reused.
  - `shaders/scaffold.metal` — checked-in MSL compiled at build time to an embedded
    metallib (see §Shaders); nothing is compiled at runtime.
  - Verified by the `metal-proof` target (`ninja -C build metal-proof &&
    build/game/metal-proof`): renders 60 windowed frames, then readback-checks the
    offscreen target (opaque/additive/alpha/reverse-subtract/write-masked draws, texture
    sampling, GEQUAL depth rejection), the cache counters (each PSO built exactly once,
    reused across frames), and the present pass (1:1 and 1.5x letterboxing with black
    bars, blackout, brightness).
- **Implemented** (stage 3, texture path):
  - The shared `TexturePool` (`game/graphics/texture/TexturePool.{h,cpp}`) is now
    graphics-API-neutral: handles are opaque `u64` (a GL texture name or a Metal registry
    handle) and each backend uploads the 16x16 placeholder itself
    (`placeholder_data()` / `set_placeholder()`). The GL pipeline does this in
    `GraphicsData`'s constructor, at the same point in initialization as before.
  - `metal_texture.{h,mm}` — the `MTLTexture` registry behind the pool's u64 handles
    (mutex-protected, the Metal analog of GL's texture-name namespace);
    `metal_upload_texture_rgba8` (RGBA8Unorm, shared storage = one copy in unified
    memory on macOS-on-Apple-silicon and iPadOS, full mip chain generated with a blit
    encoder — the analog of `glGenerateMipmap`); `metal_add_texture`, a mirror of the GL
    loader's `add_texture`; and `MetalSamplerCache`, which bakes each distinct
    min/mag/mip filter + wrap + anisotropy combination into an `MTLSamplerState` once
    (GL renderers set these per draw with `glTexParameteri`).
  - `metal_texture_upload_handler.{h,cpp}` — plain-C++ port of the TextureUploadHandler
    bucket renderer's DMA walk: collects PC-port upload packets (PC_PORT vifcode,
    vif1 == 3) and applies them to the pool. Eye-renderer DMA and texture-animator
    packets flush at the same points as the GL handler but are counted and skipped until
    their renderers are ported.
  - `metal_pipeline.mm` now owns a `TexturePool` (created with the display) and
    implements the `texture_upload_now` / `texture_relocate` module hooks with the same
    contract as the GL pipeline.
  - Verified by `metal-proof` pixel readbacks, all through the public seams:
    RGBA8888 upload + nearest/linear filters, clamp/repeat wrap, GPU mip generation and
    mip selection (trilinear minification averages a 1px checker; mips-off stays pure),
    anisotropic sampler creation; all five PS2 format combinations the GL path converts
    (PSMT8+PSMCT32, PSMT8+PSMCT16, PSMT4+PSMCT16, PSMT4+PSMCT32, PSMCT16) authored
    swizzled in emulated VRAM with the real GS address translation, converted by the
    shared `TextureConverter`, uploaded, GPU-sampled, and compared byte-exact;
    `texture_upload_now` with a synthetic GOAL texture-page (given texture, `#f` slot,
    and never-loaded texture → placeholder), `texture_relocate` (plain and mt4hh
    format 44), pool unload → placeholder fallback; and the TextureUploadHandler DMA
    walk over a synthetic chain. Optionally (`metal-proof <file.fr3>`) real extracted
    level textures are uploaded and verified byte-exact against their CPU data; no game
    data is bundled or required.
- **Implemented** (stage 4, DMA chain consumption and first bucket renderers):
  - `send_chain` is real: the chain is snapshotted with the shared
    `FixedChunkDmaCopier` under the same dma/sync mutex + condition-variable model as
    the GL pipeline (`metal_vsync` / `metal_sync_path` mirror `gl_vsync` /
    `gl_sync_path`, including the `MasterExit` wakeup). The display consumes the copy:
    `MetalRenderer::render_chain_frame` waits out the previous frame (stream-buffer
    pages are reused in place; pipelining is a later, measured change), opens one
    render pass over the game target, and walks the chain exactly like
    `OpenGLRenderer::dispatch_buckets_jak1` (initial CALL to the default-registers
    chain, fog color pickup, per-bucket `next_bucket` asserts,
    `vif_interrupt_callback` per bucket via a plain-C++ kernel bridge).
  - `metal_bucket_renderer.{h,mm}` — the bucket framework: `MetalSharedRenderState` /
    `MetalFrameContext` (encoder + PSO/sampler caches + per-frame `MetalStreamBuffer`
    vertex bump allocator), the asserting empty-bucket renderer, the texture-bucket
    adapter, and `MetalSkipRenderer`, which consumes buckets whose renderer is not
    ported yet while counting payload bytes and logging once — deferred content
    (tfrag, tie, shrub, merc, generic, ocean, sprite, shadow, eyes, depth-cue) is
    visible, never silently dropped.
  - `metal_direct_renderer.{h,mm}` + `shaders/direct.metal` — the DirectRenderer port.
    The GS state machine (GIF PACKED/REGLIST walk, A+D registers, strip/sprite/tri/
    fan/line assembly) is byte-for-byte the GL logic; at the flush boundary the GS
    state becomes a PSO key (all six GL blend mappings incl. the fix-value constant
    blend via `setBlendColor`, color write mask), a depth-stencil key (ztest,
    depth-write, the NEVER+FB_ONLY write trick) and shader uniforms (alpha test
    min/max/greater, ta0, fog, GS scissor, color/alpha mult). The afail
    FB_ONLY/RGB_ONLY double-draw (depth-writing pass for alpha-passing fragments,
    non-writing pass for the rest) is ported as two encoder draws. Unsupported GS
    blend modes are counted and logged once; states the GL renderer asserts on
    (AA1, CTXT, DATE, GS blits) assert here too so divergence is loud.
  - `metal_sky_renderer.{h,mm}` — SkyRenderer (both DMA layouts), SkyBlendHandler, and
    SkyBlendCPU. Note: the GL SkyBlendCPU kernels are x86 SSE and compile to no-ops on
    arm64; the Metal port uses portable scalar math with the same fixed-point
    semantics, so the sky actually blends on Apple silicon. The sky-blend buckets'
    trailing tfrag-trans content is stage-5 territory: consumed, counted, logged once.
  - Verified by `metal-proof` (117 checks; all previous checks still pass;
    `MTL_DEBUG_LAYER=1` clean): a constructed Jak 1-shaped chain (initial CALL,
    70-slot bucket array, empty-bucket CALL/RET structure, NOP+DIRECT vif transfers)
    goes through the real `send_chain` module hook and is verified by readback -
    opaque/blended strips, GEQUAL depth rejection, textured sprites through the
    TexturePool VRAM slots, alpha test (GREATER discard and the FB_ONLY double-draw
    with its depth-write split), GS scissor clipping, an in-chain PC-port texture
    upload packet, sky blend (draw + accumulate, clouds) with byte-exact CPU/GPU
    comparison, and the sky-draw bucket sampling the blended texture. With a real
    `.fr3` (optional argument), the sky path runs on real extracted sky texels
    (`vil1-sky-00` verified) end to end. Evidence level: constructed chains matching
    the asserted real-chain structure — upstream has no DMA capture/replay mechanism
    in this tree, and the live game does not yet drive this branch.
- **Experimental**: the validation scene still renders when no chain is pending (keeps
  the window alive and the scaffold checks meaningful); its draw region is a 4:3 fit
  of the window. The Metal pipeline does not run the Loader yet, so nothing feeds
  `metal_add_texture` outside the proof.
- **Planned**: MSAA render/resolve (PSO key already carries sample count), stencil ops in
  the depth-stencil key (for ShadowRenderer), a Metal loader upload stage (stage 5),
  eye-renderer and texture-animator paths of the upload handler, background renderers
  (TFragment/Tie3/Shrub) for the skipped buckets, live-game validation once the ARM64
  runtime branch and this renderer branch meet.
- The OpenGL renderer is untouched and remains the default (`gfx.cpp` still selects
  `GfxPipeline::OpenGL`).
