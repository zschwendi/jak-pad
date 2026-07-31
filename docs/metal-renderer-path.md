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
3. Texture path — *Planned*: TexturePool backed by `MTLTexture`, TextureUploadHandler port.
4. First game visuals — *Planned*: DirectRenderer + SkyRenderer (+ SkyBlendCPU) → boot
   splash/title screen content renders under Metal on macOS using the existing DMA chain.
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
- **Experimental**: the Metal pipeline ignores `send_chain` (logs once); it renders a
  fixed validation scene, no game content yet. The stand-in draw region is a 4:3 fit of
  the window until the game supplies real sizes.
- **Planned**: MSAA render/resolve (PSO key already carries sample count), stencil ops in
  the depth-stencil key (for ShadowRenderer), texture pool (stage 3).
- The OpenGL renderer is untouched and remains the default (`gfx.cpp` still selects
  `GfxPipeline::OpenGL`).
