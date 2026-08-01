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
| `background/TFragment` | static terrain ("tfrag") | yes (ported) |
| `background/Tie3` | instanced environment geometry incl. wind ("tie") | yes (base + envmap second draw ported; wind deferred) |
| `background/Shrub` | vegetation | yes (ported) |
| `background/Hfrag` | heightmap terrain | no (Jak 3) |
| `foreground/Merc2` | skinned character meshes + envmap ("merc"/"emerc") | yes |
| `foreground/Generic2` | VU1 "generic" fallback path | yes (ported) |
| `foreground/Shadow2`, `ShadowRenderer` | shadow volumes (Shadow2 = Jak 2/3, ShadowRenderer = Jak 1) | ShadowRenderer (ported, unexercised) |
| `sprite/Sprite3` (+`_Distort`, `_Glow`, `GlowRenderer`) | particles, screen distortion, glow probes | yes (Sprite3 ported; distort/glow pending) |
| `ocean/*` | ocean surface near/mid/far, generated ocean texture, envmap | yes (ported; envmap is Jak 2/3 only) |
| `SkyRenderer`, `SkyBlendCPU`/`SkyBlendGPU` | sky texture blending (CPU and GPU variants) | yes (CPU variant suffices initially) |
| `EyeRenderer` | renders eyes into small textures | yes (ported) |
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
   Jak 1 chain structure, including real extracted sky texels, **and** by replaying a
   real captured game chain (`metal-proof --replay`, see §Current state) - the first
   frame of DMA the ARM64 runtime's engine main loop built from the player's own
   game data went through the full send_chain → dispatch → DirectRenderer path with
   every structural assert holding.
5. **Background geometry** — *Implemented* (see §Current state): the level-data
   stage (fr3 → MTLBuffers) plus TFragment, Tie3 (base draws) and Shrub, with the
   time-of-day 1D palette textures and the visibility → draw-run conversion that
   replaces GL's multidraw, plus TIE's envmap second draw and the tfrag-trans
   content of the sky-blend buckets. TIE wind instancing remains *Planned*.
6. Foreground — *Partially implemented*: Merc2 (both the merc2 and emerc passes, bones
   via per-draw buffer offsets), the EyeRenderer and Generic2 are ported and verified,
   and ShadowRenderer is ported but unexercised (see §Current state); merc's
   vertex-modification (blerc / mod-vtx) paths remain *Planned*.
7. Sprite/effects — *Partially implemented*: Sprite3's 2D / HUD / 3D sprite paths and
   the whole Jak 1 ocean path (ocean-mid-and-far, ocean-near, the generated ocean
   texture) are ported and verified (see §Current state); the distorter's drawing,
   glow, DepthCue and post effects remain *Planned*.
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
  - Verified by `metal-proof` (all previous checks still pass;
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
- **Implemented** (captured-chain replay, version 2 captures): `metal-proof --replay
  <capture.gpdma> [--replay-png <out.png>] [--replay-frames <n>]
  [--replay-common-fr3 <GAME.fr3>] [--replay-fr3 <level.fr3>]...` replays one frame of
  real game DMA captured by the runtime track's `__send-gfx-dma-chain` hook
  (`game/kernel/core/dma_capture.cpp` on the runtime branch, which documents the file
  format byte for byte).
  - `metal_chain_replay.{h,cpp}` reads both capture versions. Version 1 is the chain
    alone (`FixedChunkDmaCopier::serialize_last_result` form); version 2 adds an 80-byte
    `GPDMACAP` header, the same chain section unchanged, and a snapshot of EE main memory
    stored as the copier's 128 kB chunks with the all-zero ones omitted and each stored
    chunk LZO-compressed (lzokay, already vendored). The snapshot is what makes the
    chain's *pointers* resolve: the PC-port texture-upload packets carry EE addresses of
    GOAL `texture-page` structs that live in the level and global heaps, not in the chain.
  - The chain image is placed in the longest run of chunks the snapshot left empty
    (`place_chain_in_ee`), so replaying it cannot land on captured data, and the header's
    `s7` is installed with `metal_renderer::set_s7_override` because a replay has no
    booted GOAL kernel to ask.
  - `--replay-fr3` / `--replay-common-fr3` load extracted level textures into the pool
    the way the GL loader's `TextureLoaderStage` does. The Metal path does not run the
    streaming Loader yet and the runtime's `__pc-set-levels` is stubbed, so the caller
    names the levels the frame used.
  - The pool is then primed with every `texture-page` still live in the EE snapshot
    (`find_texture_pages`). A single frame only uploads the pages *that* frame touched,
    but the VRAM slots it draws from were filled over many earlier frames - the debug
    font among them. The pages are found without any symbol-table knowledge: a GOAL basic
    object stores its type pointer in the word before it, so one page address from the
    chain yields the `texture-page` type pointer and every other occurrence of that word
    marks another page. Candidates are validated (plausible texture count, in-range name
    and per-texture pointers) before the pool follows them.
  - Results on the runtime track's title-screen captures (Jak 1, title + Sandover
    village), all rendered through the real `send_chain` module hook:
    - frame 838/839/840 (~154 kB of payload each): the sky renders for real - the
      Sandover sunset, blended by the ported SkyBlendCPU and drawn by the ported
      SkyRenderer/DirectRenderer - and the title's **"PRESS START" draws in the game's
      own font**, as DEBUG-bucket DirectRenderer content sampling the real font texture.
      When only the sky and DirectRenderer existed these frames were 4 draws /
      88 triangles, ~160 k lit pixels, with 137 kB per frame consumed-but-not-drawn
      (ocean-mid-far 82.5 kB, ocean-near 38.9 kB, tie 5.6 kB, tfrag 5.8 kB, shrub,
      merc, generic, eyes, and 992 bytes of tfrag-trans inside the sky-blend bucket).
      The background, ocean and merc ports have since claimed all of those buckets -
      see §The complete title screen for what the same frames render today.
    - frame 100 (14 kB): originally rendered black - its content is merc models plus a
      160-byte sky-draw that is a black quad because nothing had been blended into the
      sky texture that frame. With the merc renderer it draws Daxter.
    - the **sprite bucket carries no sprites in any of these captures**. Its 4416 bytes
      are exactly the per-frame setup (distorter GS setup 112 B + sine tables 2224 B +
      aspect 16 B + direct setup 48 B + frame data 656 B + 3D matrix 80 B + HUD matrix
      1280 B); the title text is DirectRenderer content, not sprites. The sprite
      renderer consumes all of it and reports zero sprites.
  - Capture files and replay PNGs derive from the player's game data: they stay outside
    the repository (`.gitignore` carries guard patterns).
- **Implemented** (stage 7 part 1, the sprite renderer): `metal_sprite_renderer.{h,mm}`
  + `shaders/sprite.metal`, the Sprite3 port for Jak 1.
  - The DMA walk, the GS state machine (adgif -> `DrawMode`), the per-(texture, mode)
    bucketing and the four-vertices-per-sprite expansion are the GL logic unchanged. At
    the flush boundary the `DrawMode` becomes a PSO key, a depth-stencil key and a
    sampler key - the Metal analog of `setup_opengl_from_draw_mode`, including all seven
    GS blend mappings, the constant-blend factor, the atest-NEVER depth-write trick and
    the alpha-fail double draw. Strips are drawn with one indexed draw per bucket;
    Metal restarts on the GL renderer's `0xFFFFFFFF` index natively.
  - `shaders/sprite.metal` is the MSL port of `sprite3_3d.{vert,frag}`, line for line,
    with two deliberate differences: Metal's clip-space z is `[0,1]`, so the GL
    `z / 8388608 - 1` becomes `z / 16777216`, and the GL `HEIGHT_SCALE`/`SCISSOR_ADJUST`
    text substitutions arrive as uniforms.
  - Flushes are bounded by one stream-buffer page (8192 sprites) rather than the GL
    renderer's 23040; the GL renderer already flushes mid-block at its own limit, so this
    is the same behaviour with a different threshold.
  - Not ported, counted and logged rather than dropped: the distorter's *drawing* (its
    DMA is walked exactly like `Sprite3::distort_dma`, so the chain stays in sync and the
    sprite count is reported); glow; the Jak 2/3 paths. View-frustum culling is skipped
    because the Metal path receives no PC vis data yet - the GL renderer skips it in
    exactly that case too.
  - Verified by `metal-proof` pixel readback over a constructed sprite bucket that
    matches every packet `Sprite3` asserts about a real chain (distorter GS setup + sine
    tables, frame data, 3D matrix, group-0 chunks, the fake-shadow flush, the HUD matrix
    and group-1 chunks): a world-space 2D quad and a 3D quad land on their computed
    screen positions with the expected modulated color and their edges fall where the
    scale says; two HUD sprites resolve their four texture quadrants correctly (which
    checks the `st_array`/vertex-id wiring end to end), and the second one moves by the
    `hud_hvdf_user[0]` entry its matrix index selects.
- **Implemented** (stage 7 part 2, the ocean): `metal_ocean_renderer.{h,mm}` +
  `shaders/ocean.metal`, the Jak 1 ocean path.
  - The VU1 emulators are *shared*, not copied. Roughly 10k of the ocean
    renderer's 14k lines are transcriptions of the VU1 microprograms
    (`OceanTexture_PC.cpp`, `OceanMid_PS2.cpp`, `OceanNear_PS2.cpp`) that touch
    nothing but VU registers, VU data memory and CPU-side vertex buffers. They
    now live on three backend-neutral base classes in
    `game/graphics/opengl_renderer/ocean/OceanVu.h` - `OceanTextureVu`,
    `OceanMidVu`, `OceanNearVu` - which both `OceanTexture`/`OceanMid`/
    `OceanNear` (GL) and `MetalOceanTexture`/`MetalOceanMid`/`MetalOceanNear`
    derive from, supplying only `xgkick` and the drawing. The upstream change is
    a mechanical one: the emulator functions are re-qualified onto the base
    classes (30 lines across the two `_PS2.cpp` files) and `OceanTexture_PC.cpp`
    is split into `OceanTexture_VU.cpp` (VU, GL-free) and the GL objects it kept.
    No VU instruction was retyped, so the two backends cannot drift.
  - What the Metal file ports is the rest: the ocean-mid-and-far and ocean-near
    DMA walks, the ocean-texture DMA walk, `CommonOceanRenderer`'s GIF/GS state
    machine and strip/fan assembly, and the drawing. Blend/depth state becomes
    PSO and depth-stencil keys - all five GL per-bucket blend configurations
    (`SRC_ALPHA/1-SRC_ALPHA`, `ZERO/ONE`, `DST_ALPHA/ONE`, blend off, and the
    reversed-index envmap pass), depth GEQUAL without writes for near and
    ALWAYS with writes for mid.
  - ocean-far needed no new code: it is plain GIF forwarded to the already-ported
    DirectRenderer, including the ta0 patch.
  - **The one architectural decision**: `OceanTexture` needs 1 + 8 offscreen
    render passes and `MetalFrameContext` hands bucket renderers an already-open
    encoder. The generator therefore runs on **its own command buffer**, encoded,
    committed and waited on while the frame's encoder is still recording; the
    frame's command buffer is committed later, so the generated texture is always
    complete before anything samples it - the ordering the immediate-mode GL
    renderer gets for free. This needed no change to the frame scaffolding.
  - `shaders/ocean.metal` is the MSL port of `ocean_texture.{vert,frag}`,
    `ocean_texture_mipmap.{vert,frag}` and `ocean_common.{vert,frag}`, line for
    line, with three deliberate differences: Metal clip-space z is `[0,1]` so
    GL's `z * 2 - 1` becomes `z`; the GL `SCISSOR_ADJUST`/`HEIGHT_SCALE` text
    substitutions arrive as a uniform; and the *generated* ocean texture is
    rendered y-flipped, because Metal render targets are top-down while GL
    framebuffers are bottom-up and that texture is rendered and then sampled.
    (On-screen geometry needs no flip - the same clip-space math shows the same
    image in both APIs.) The mip chain drops the GL renderer's `1/(1<<level)`
    quad scale: GL keeps the level-0 viewport for every mip level, while a Metal
    render pass targets the level directly, so the quad is a plain 1:1 copy. The
    deliberate `max(0, 1 - 0.51 * level)` alpha fade is kept.
  - Faithfulness notes preserved rather than "fixed": both `OceanTexture`
    instances publish to the same VRAM slot (8160 on Jak 1) and the last bucket
    of the frame wins, so ocean-near's single-level texture replaces
    ocean-mid-and-far's mipmapped one; and the mid adgif handler compares against
    the Jak 2 slot with no Jak 1 branch.
  - Not ported: `OceanEnvmap` (Jak 2/3 only) and the Jak 2/3 DMA layouts, matching
    the rest of the Jak 1-only Metal bucket table.
  - Verified by `metal-proof --replay` on the real captures (the ocean is entirely
    DMA driven, so it only has content when a captured frame carries the ocean
    buckets):
    - the ocean buckets are fully consumed - the frame's skipped payload drops
      from 137 kB to 15.7 kB, exactly the 82,544 + 38,864 bytes the two ocean
      buckets carry;
    - the texture VU program produces its full 32 x 66 vertex mesh, and the
      generated 128x128 texture reads back with every texel drawn and varied
      content;
    - the mip chain's alpha fade is checked by minifying all the way down: the
      mipmapped texture reads back with alpha 0 at its smallest level (level 7's
      intensity is `max(0, 1 - 0.51 * 7)` = 0) while the single-level texture
      ocean-near publishes keeps its alpha;
    - the ocean VRAM slot holds the last generated texture of the frame;
    - the band below the horizon, which read back **solid black** before this
      renderer (0 lit pixels in rows 260-340), is now drawn (~47.8 k lit pixels);
      the whole frame goes from ~160 k to ~301 k lit pixels.
  - Note on what the generated texture is: the VU feeds the GS an RGBAQ of
    `(0, 0, 0, 0x80)` for every vertex - the "vertex" quadwords of the
    ocean-texture input are `(0, 0, 0, scale)` - so `ocean_texture.frag` writes
    black with the envmap's alpha. It is an **alpha mask**, not a color map: the
    ocean's visible color comes from the ocean-mid envmap pass, which the mask's
    destination alpha gates. This is the GL behaviour, byte for byte, because the
    VU code is the same code.
- **Implemented** (stage 5, level geometry): `metal_level_data.{h,mm}` +
  `metal_tfrag.{h,mm}` + `metal_tie.{h,mm}` + `metal_shrub.{h,mm}` +
  `shaders/background.metal`. This is what makes a level's *world* appear:
  frame 838 goes from the sky and "PRESS START" over a black band to Sandover
  village - terrain, huts, walkways, palms and shrubs - drawn from the player's
  own extracted `.fr3` data.
  - **The level-data stage** is the Metal analog of `loader/Loader` +
    `LoaderStages`: an `.fr3` is decompressed and deserialized with the shared
    `common/loader` / `common/custom_data/Tfrag3Data` code, `tfrag3`'s own
    `unpack()` turns packed vertices and index runs into the GPU forms, and each
    tree becomes one static vertex `MTLBuffer`, one static index `MTLBuffer` and
    one 1D `MTLTexture` for its time-of-day palette. Textures go into the pool
    through the existing `metal_add_texture`, and the per-level handle vector is
    kept parallel to `level->textures` exactly like `LevelData::textures`, so a
    draw's `tree_tex_id` resolves the same way it does in GL. Levels live in a
    name-keyed registry; `metal_level_data::get(name)` is the analog of
    `Loader::get_tfrag3_level(name)`, and a bucket that names a level which is
    not loaded is counted and logged, never drawn from stale data. What is *not*
    ported is the streaming: a level loads in one call instead of in
    time-budgeted chunks across frames (village1: 667 textures, 57 MB of
    vertices, 7.4 MB of indices, ~150 ms).
  - **`background_common` analogs** live in the same file because the GL one
    pulls in the entire OpenGL renderer header chain. The camera math
    (`make_new_cam_mat`, `init_etie_cam_uniforms`) is transcribed verbatim; the
    `DrawMode` → PSO/depth/sampler mapping is the same seven-blend-mode table
    the DirectRenderer and sprite ports use. The two pure computations that
    could silently drift - `interp_time_of_day` (hand-written SSE upstream,
    reaching arm64 through the vendored sse2neon) and `cull_check_all_slow` -
    are re-implemented portably *and diffed against the GL originals by
    metal-proof*, byte for byte, on constructed data (including a case that
    saturates the 16-bit accumulator) and on a real level's palettes and BVH.
  - **Visibility and culling match GL, with no relaxation.** The occlusion
    strings the game hangs off `TFRAG_LEVEL0` are copied out of the chain by the
    Metal TFragment exactly as `SharedRenderState::bucket_for_vis_copy`
    prescribes, and TIE reads them for its level. Every frame runs the BVH
    frustum test plus the occlusion string, then converts the per-vis-group
    result into runs of the tree's static index buffer - the same runs GL packs
    into one `glMultiDrawElements`, issued here as one `drawIndexedPrimitives`
    each (Metal has no multidraw; indirect command buffers are a later, measured
    change). `debug_all_visible` exists, mirroring the GL debug toggle, and is
    off.
  - **`shaders/background.metal`** is the MSL port of `tfrag3.{vert,frag}`,
    `etie_base.{vert,frag}` and `shrub.{vert,frag}`, line for line. The
    deliberate differences are the target conventions: Metal's `[0,1]` clip
    depth (GL's post-divide `[-1,1]` z becomes `(z + w) / 2`, and etie's
    `z / 8388608 - 1` becomes `z / 16777216`, the same rule the sprite port
    uses), `sampler1D` + `texelFetch` becomes `texture1d` + `read()` (both
    unfiltered integer lookups, so the palette colors are identical), and
    HEIGHT_SCALE / SCISSOR_ADJUST arrive as uniforms instead of text
    substitutions. Vertices are read from a device buffer by `vertex_id`, so the
    `tfrag3` vertex structs are used with their exact on-disk layout.
  - **TIE covers the two draws Jak 1's bucket issues**: the NORMAL category
    through the tfrag3 shader and the base draw of NORMAL_ENVMAP through
    etie_base - the same shader split, for the same reason, as
    `Tie3::draw_matching_draws_for_tree`. Not ported and honestly missing rather
    than faked, each counted per frame: the envmap **second** draw (the shiny
    reflective pass), wind-instanced draws, and per-proto visibility (Jak 2/3).
  - Results on frame 838 (Sandover title screen), through the real `send_chain`
    hook: **4 draws / 88 triangles / 160 210 lit pixels → 243 draws / 350 085
    triangles / 245 516 lit pixels**, with tfrag 101 draws / 55 806 tris, tie
    117 draws / 192 772 tris and shrub 21 draws / 101 419 tris. Frames 839 and
    840 behave the same; frame 100 (no level buckets) is unchanged. The water is
    still black - that is the ocean renderer, not this stage.
  - These renderers consume user-supplied game data, so a bucket whose DMA does
    not match is reported (logged once, counted in the frame stats as
    `unexpected_dma`) and skipped whole, rather than aborting the process. The
    replay checks that counter is zero.
- **Implemented** (stage 6 part 1, the merc renderer): `metal_merc.{h,mm}` +
  `metal_merc_model_pool.{h,mm}` + `shaders/merc2.metal`, the Merc2 port for Jak 1.
  All eight Jak 1 merc buckets route to one shared `MetalMerc2`, as in the GL table.
  - The DMA walk (setup packet, per-model PC_PORT packets: name, `VuLights`, the Jak 1
    water quadword, the matrix-slot string, EE pointers to bone matrices, flags, fades),
    the level/draw/bone/light bookkeeping and the effect -> draw expansion are the GL
    logic unchanged. `shaders/merc2.metal` is the MSL port of `merc2.{vert,frag}` and
    `emerc.{vert,frag}` line for line, with the two deliberate target differences the
    sprite port already uses: Metal's `[0,1]` clip z (`z / 8388608 - 1` becomes
    `z / 16777216`) and HEIGHT_SCALE / SCISSOR_ADJUST as uniforms.
  - **Bones**: the GL renderer keeps a persistent 512 kB `GL_UNIFORM_BUFFER` and binds a
    per-draw window with `glBindBufferRange(..., 16 * draw.first_bone,
    128 * sizeof(ShaderMercMat))` (`foreground/Merc2.cpp:1326`). Here each *flush* takes
    one range out of the frame's stream buffer and each *draw* binds into it with
    `setVertexBuffer:offset:` at the same `16 * first_bone`. Per-flush allocation (rather
    than rewriting one buffer) is required because earlier draws in the same encoder still
    reference the previous contents. The allocation is padded by `MAX_SKEL_BONES`
    matrices so every draw's 128-matrix view stays inside it, exactly as it stays inside
    the GL buffer, and `alloc_bones` rounds `first_bone` up to 16 bone vectors (256 bytes)
    so every offset satisfies Metal's buffer-offset alignment. The `std140` padding of
    the bone struct (mat3 columns padded to `vec4`, plus the trailing `vec4`) is
    reproduced exactly, so the CPU-side layout is the GL one. This layout is checked end
    to end by the proof's **Y-rotation sweep**: one bone is driven through
    `translate * rotate_y` at nine yaws and the model's silhouette is measured against the
    same transform computed on the CPU. The model is a cross of two quads - one spanning
    model x above the origin, one spanning model z below it - so its width is
    `h * max(|cos|, |sin|)`, which never falls below `h/sqrt(2)`; the two quads carry
    different normals, so their shading reads back two columns of the normal matrix in the
    same frame. Diagonal bone matrices (all the merc checks used before) map onto
    themselves under a transposed, short-strided or column-dropping read, so they cannot
    see any of those. The sweep fails on all three: dropping the `X[2]` column takes the
    model to 0 px wide at 90 degrees, a row-major read to 0 px at every yaw, and reading
    the mat3 columns at a packed 12-byte stride mis-shades it.
  - **Draw-buffer overflow**: a flush retires every level draw bucket, so the bucket
    pointer is re-acquired after the "out of draws" flush. Holding the pre-flush pointer
    appended to a bucket outside the live range, and those draws were never issued.
  - **One deliberate divergence from the GL source**: the GL renderer resolves the bone
    matrices with `setup.data - setup.data_offset + addr`, which works there only because
    the GL pipeline renders from the *original* EE memory (`run_dma_copy = false` in
    `pipelines/opengl.cpp:48`). The Metal pipeline renders from the
    `FixedChunkDmaCopier` copy, whose chunk indices are compacted, so the Metal port
    resolves bone pointers against `render_state->ee_memory` - the same thing the ported
    texture-upload handler does with its `texture-page` pointers. Addresses are bounds-
    checked before dereferencing and reported (`bad_bone_pointers`) rather than faulting.
  - **Model geometry** does not travel in the chain. `metal_merc_model_pool.{h,mm}` is the
    merc-scoped equivalent of what the GL loader's `MercLoaderStage` produces: it reads an
    extracted level, uploads its textures the way `add_texture` does, puts
    `merc_data.vertices` / `merc_data.indices` into two `MTLBuffer`s, keeps the per-level
    texture handle array that `MercDraw::tree_tex_id` indexes, and answers
    `get_merc_model(name)` with the GL `Loader::get_merc_model` semantics (first match
    wins). The GL `Loader` itself is unusable here - its `LevelData` holds `GLuint`s and
    it includes glad - and a general level-geometry loader is separate, larger work; this
    duplication is deliberate and should be unified when that lands.
  - Consumed and counted rather than silently dropped: merc's **vertex-modification paths**
    (`blerc` and mod-vtx) - those effects draw from the level's unmodified vertices, which
    is what `sidekick-lod0` (Daxter) does in the captures; **eye textures**, which need the
    unported EyeRenderer and fall back to the pool placeholder; anim-slot textures (Jak
    2/3 only). The Jak 1-dead color-mask double draw (`Merc2.cpp:1332-1342`) is not
    ported: Jak 1 always forces `lights.w1` to 0, so the GL renderer never takes that
    branch either.
  - Verified by `metal-proof` pixel readback over a constructed merc bucket that matches
    every packet `Merc2` asserts about a real chain, plus a synthetic level registered
    with the model pool (so the check needs no game data): two instances of one model with
    different bone matrices land on their computed screen positions with the expected
    modulated color and edges, which is what proves the per-draw bone-buffer offset; a
    non-zero fade adds the emerc pass and its `SRC_0_DST_DST` blend over the merc pass
    reads back the predicted value.
  - On the captured frames (`--replay`, with the level art named), with merc as the only
    background/foreground renderer enabled:
    - **frame 838/839/840 render the Jak & Daxter title logo** - `logo-english-lod0` from
      `title.fr3`, 7 draws / 14 750 triangles (2 of them the emerc envmap pass) - above
      the already-working "PRESS START". 62 007 lit pixels, up from 5 330.
    - frame 100 renders **Daxter** (`sidekick-lod0`, 14 draws + 4 envmap draws, ~1 480
      triangles), up from a completely black frame. The frame's other two models,
      `ndi-lod0` (the Naughty Dog logo) and `ndi-cam-lod0`, are correctly *off camera*:
      their bone matrices are clean rigid transforms placing them ~40 000 units to the
      side, so they contribute no pixels. His mod/blerc effects draw unmodified
      vertices; his eyes were the placeholder texture until the eye renderer landed
      (below).
- **Implemented** (stage 6 part 2, the eye renderer): `metal_eye_renderer.{h,mm}` +
  `shaders/eye.metal`, the EyeRenderer port. The game sends a bucket of GS sprite
  draws that compose each character's eye - background, iris, pupil, eyelid - into a
  small texture that merc then samples; before this, every merc eye draw fell back to
  the texture-pool placeholder. The DMA decode and the four-draw composition are the
  GL logic unchanged. Two structural differences: GL's `FramebufferTexturePair`
  becomes one 128x128 `MTLTexture` per eye with one render pass each (its clear
  replaces `glClearBufferfv`, the pupil's `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` becomes a
  PSO key); and those passes must be encoded while the frame's game-target encoder is
  open, so - exactly like the generated ocean texture - they run on **their own
  command buffer**, committed and waited on before the frame's.
  `shaders/eye.metal` is the MSL port of `eye.{vert,frag}` line for line, with the y
  negation the generated ocean texture already uses (Metal render targets are
  top-down; this texture is rendered and then sampled). Merc resolves eye draws
  through the renderer, published on `MetalSharedRenderState` the way the GL table
  publishes `render_state->eye_renderer`. The eye DMA lives outside the bucket's own
  address range, so the decode loop is bounded by the number of eye pairs the
  renderer has textures for. Verified on frame 100: the bucket's 1568 bytes go from
  skipped to consumed and matched, 2 eyes are composed in 8 draws / 16 triangles with
  0 missing source textures and 0 unexpected-DMA reports, and the composed texture
  reads back as a recognizable eye (4096/4096 texels drawn, no debug-clear texels
  left). Daxter faces away from the camera in that capture, so the frame's pixels are
  unchanged; the evidence is the readback.
- **Implemented** (stage 6 part 3, Generic2): `metal_generic2.{h,mm}` +
  `shaders/generic.metal`, the "generic" VU1 fallback whose buckets sit next to
  merc's in every level slot. Ten Jak 1 buckets route to one shared `MetalGeneric2`,
  as in the GL table. Jak 1 drives every generic bucket in `Mode::NORMAL`, so that is
  what is ported; the LIGHTNING / WARP / PRIM modes, the Jak 2/3 DMA layout and the
  full-matrix path those modes set up are not. The DMA walk (the VIF unpack emulation
  in `Generic2_DMA.cpp`) and the whole of `Generic2_Build.cpp` (adgif -> draw mode,
  the seven GS blend mappings, the two z-write-disabling alpha-test tricks, the
  adgif -> draw-bucket linked lists, the projection/HUD matrix split and the
  adc-driven index buffer) are the GL logic unchanged; what changes is the drawing.
  The GL alpha-mode draw order is copied exactly. Verified by `metal-proof` pixel
  readback over a constructed generic bucket that matches every packet Generic2
  asserts about a real chain (the 48-byte test/zbuf setup, the 160-byte VU constants
  unpack, the 32-byte VU register setup, then one fragment carrying its 7-quadword
  header, one adgif and the STCYCL/UNPACK_V3_32/V4_8/V2_16 vertex streams): one draw
  of two triangles whose four texture quadrants land in the right screen quadrants -
  which checks the texture coordinates and the strip's vertex order end to end - with
  the quad's edges where the projection puts them. The captures carry no generic
  *geometry* (each bucket holds only the 240-byte setup), which is why the drawing
  path is proven by the constructed bucket instead; their 2160 bytes/frame are now
  consumed and matched.
- **Experimental** (stage 6 part 4, the shadow renderer): `metal_shadow_renderer.{h,mm}`
  + `shaders/shadow.metal`. The VU1 program that builds the shadow volume is
  **shared, not copied**: the ~1.9k lines of `Shadow_PS2.cpp` touch nothing but VU
  registers, VU data memory and the CPU-side vertex/index buffers, so they now live on
  a backend-neutral base class - `ShadowVu`
  (`game/graphics/opengl_renderer/ShadowVu.h`) - that both `ShadowRenderer` (GL) and
  `MetalShadowRenderer` derive from. The upstream change is mechanical, the same shape
  as the ocean's `OceanVu`: the four Shadow_PS2 functions are re-qualified onto the
  base class and `xgkick` moves to a new GL-free `ShadowVu.cpp`. No VU instruction was
  retyped. What the Metal file ports is the DMA walk and the three-pass stencil draw.
  `MetalDepthStencilKey` gains the stencil state GL sets with `glStencilFunc` /
  `glStencilOp`; the game target already carried a `Depth32Float_Stencil8` attachment
  cleared to 0 each frame. **Not validated**: none of the available captures carries
  shadow content (the SHADOW bucket is empty in all four), so the VU run, `xgkick` and
  the stencil draw are code-complete but unexercised on this backend, and so is PSO
  creation for their state combination. That needs a capture taken during gameplay
  with a character casting a shadow.
- **Implemented** (TIE's envmap second draw): `shaders/background.metal` gains
  `etie_vs`, the MSL port of `etie.vert` line for line. Two things GL gets from its
  vertex array are unpacked in the shader instead, because Metal reads
  `tfrag3::PreloadedVertex` directly: `normal` (`GL_INT_2_10_10_10_REV`, normalized -
  three sign-extended 10-bit fields over 511) and `proto_tint` (the vertex's
  normalized r/g/b/a). The fragment stage is the existing `tfrag3_fs`, which is
  `etie.frag` character for character. `MetalEtieVsParams` gains `envmap_tod_tint`,
  and MetalTie3 keeps the frame's envmap color from the chain with the Jak 1 scaling.
  On frame 838 the pass is 2 draws / 3664 triangles (839/840: 2 draws / 5864). The
  frames stay pixel-identical, and that is correct rather than a failure: forcing the
  *base* envmap draw to opaque red also changes zero pixels, so the envmapped TIE
  surfaces in these title frames are off-camera or fully occluded and the reflective
  pass has nothing to add. It needs a capture with an envmapped TIE prop on screen.
- **Implemented** (tfrag-trans in the sky-blend buckets): the sky-blend handler owns
  its own `MetalTFragment` for the TRANS and LOWRES_TRANS tree kinds and calls it at
  the same two points `SkyBlendHandler` does. `MetalTFragment` gains GL's
  `child_mode` (the parent already consumed the bucket's opening NEXT). On frame 838
  this is 1 draw / 80 triangles; the frames stay pixel-identical because those 80
  triangles land on nothing visible at the title camera. `skipped_tfrag_bytes` is
  gone: the replay now checks that **no** bucket content is left unconsumed.
- **Experimental**: the validation scene still renders when no chain is pending (keeps
  the window alive and the scaffold checks meaningful); its draw region is a 4:3 fit
  of the window. The Metal pipeline does not run the streaming Loader yet: level data
  reaches the GPU only through the proof and the merc-scoped model pool.
- **Planned**: MSAA render/resolve (PSO key already carries sample count), streaming
  (time-budgeted) level loads and level unloading, TIE wind instancing (7 draws
  deferred and counted in village1), merc's vertex-modification paths (blerc /
  mod-vtx), DepthCue, the sprite distorter's drawing and glow, the texture-animator
  path of the upload handler (Jak 2/3), the Jak 2/3 bucket tables, and live-game
  validation once the ARM64 runtime branch and this renderer branch meet.
  `MetalSkipRenderer` still owns exactly one Jak 1 bucket - DEPTH_CUE - and it
  carries no payload in any available capture.
- **Headless by default**: `metal-proof` (and therefore every replay) creates its SDL
  window with `SDL_WINDOW_HIDDEN` via `metal_renderer::set_window_hidden`. The
  `CAMetalLayer` still renders and is read back, so nothing about the checks changes,
  but no window appears or steals focus. Pass `--show-window` to watch.
- The OpenGL renderer is untouched and remains the default (`gfx.cpp` still selects
  `GfxPipeline::OpenGL`).

### The complete title screen

The sky/direct/sprite, background-geometry, ocean and merc ports were developed on
separate branches and land together here. With all of them enabled, replaying the
captured title frames draws the whole screen at once for the first time: the Sandover
sunset sky, the village terrain and its huts, palms and shrubs, the ocean, the Jak &
Daxter logo and "PRESS START". The eye, Generic2, shadow, TIE-envmap-second-draw and
tfrag-trans ports then closed every remaining payload gap.
`metal-proof --replay`, `MTL_DEBUG_LAYER=1`, headless, 45 checks per frame (42 on
frame 100), no Metal validation diagnostics. The argument-less proof runs
224 checks, 240 with an `.fr3` - the union of every branch's checks, none dropped.

| frame | draws | triangles | lit pixels |
| --- | --- | --- | --- |
| 838 | 256 | 372 462 | 279 762 |
| 839 | 279 | 437 166 | 279 770 |
| 840 | 279 | 438 480 | 279 755 |
| 100 | 24 | 2 556 | 5 964 |

Per renderer on frame 838: tfrag 102 draws / 55 886 tris (1 draw / 80 tris of it
tfrag-trans inside a sky-blend bucket), tie 119 draws / 196 436 tris (2 draws / 3664
tris of it the envmap second pass), shrub 21 draws / 101 419 tris, ocean 12 draws /
7 817 tris (2112 texture verts + 3180 mid verts), merc 1 model / 7 draws / 14 750 tris
(2 emerc), sky 1 draw + 1 blend, cloud 1 draw. Frame 100 carries no level or ocean
buckets: its 24 draws are 3 merc models (23 draws, 4 of them emerc, 2554 tris) plus the
black sky quad, and its eye bucket composes Daxter's two eyes in 8 offscreen draws.

Zero missing levels, zero missing models, zero missing textures, zero unexpected-DMA
reports (bucket, background, eye, generic2 and shadow alike), zero bad bone pointers,
zero bad draw ranges, zero unsupported blends and **zero skipped bucket bytes** on
every frame - down from the 137 kB/frame the sky-only renderer skipped and the
4960 + 1984 bytes/frame that were still consumed-but-not-drawn after the merc port.
What is still deferred and counted rather than dropped: TIE wind instancing
(7 draws in village1), merc's blerc / mod-vtx effects (3 on frame 100), the sprite
distorter's drawing, glow, and DepthCue.

## 5. Ocean and Merc2 (both done)

The ocean (~121 kB/frame) and merc (~4.5 kB/frame) were the last two payload
carriers in the captured title frames; both are now *Implemented* - see §4 for
what was built, how the 1 + 8 offscreen ocean-texture passes fit the frame
structure, and the readback evidence for each.

**Ocean: what shipped and what did not.** It had *no* Loader or level-data
dependency - nothing under `game/graphics/opengl_renderer/ocean/` mentions
`tfrag3`, `LevelData` or `Loader` - so it went in ahead of the background
renderers. Entry points `OceanMidAndFar` (`ocean/OceanMidAndFar.cpp:25`, jak1
path `:48`, `BucketId::OCEAN_MID_AND_FAR`) and `OceanNear`
(`ocean/OceanNear.cpp:25`, jak1 `:48`, `BucketId::OCEAN_NEAR`) both have Metal
counterparts. Still open:

- `OceanEnvmap` is Jak 2/3 only and is not ported.
- The Jak 2/3 DMA layouts (`render_jak2`, `handle_ocean_texture_jak2`,
  `run_jak2`) are not ported; their VU emulators are on the shared base classes
  and compile, so those paths are a DMA-walk port away.
- Neither capture available here exercises **ocean-near geometry**: the
  ocean-near bucket runs its texture generator but its VU produces zero vertices
  at the title camera (one `call0`, no `call39`), so `flush_near`'s three-bucket
  draw path is code-complete but has only been exercised with an empty vertex
  set. It needs a capture taken near the water.
- The ocean-mid envmap does not cover every tile the base pass covers (2476 vs
  1229 indices in frame 838), so some tiles read back black. That is the GL
  result too - the base pass samples an all-black color texture - but it has not
  been diffed against a GL render of the same frame, because this tree has no GL
  capture-replay harness.

### What merc left behind

Merc2 itself is ported (`metal_merc.{h,mm}`, `metal_merc_model_pool.{h,mm}`,
`shaders/merc2.metal`); the details of the bone-buffer design and the one deliberate
divergence from the GL source are in §4. Of the four items it left behind, the
EyeRenderer and Generic2 have since landed (see §4). What remains:

- **A real Metal loader stage.** `metal_merc_model_pool` loads whole levels eagerly for
  merc only: no streaming, no unloading, no `Loader*` on `MetalSharedRenderState`, and
  only `merc_data` reaches the GPU. The background renderers (TFragment/Tie3/Shrub) have
  their own level pool in `metal_level_data.{h,mm}`, so the same `.fr3` is decompressed
  and deserialized **twice**. Unifying them into one Metal loader stage that owns both
  is the right fix and is still *Planned*.
- **Vertex modification**: `Merc2::model_mod_draws` (reads the game's fragment data out of
  EE memory and re-unpacks vertices) and `model_mod_blerc_draws` (blend shapes; note the
  GL kernel is x86 SSE and would need portable math like SkyBlendCPU did). Those effects
  currently draw the level's unmodified vertices and are counted as
  `mod_effects_deferred`.
