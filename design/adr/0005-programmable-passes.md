# 0005 — Programmable passes: render targets, programs, volume textures, scene depth

Status: proposed (P4.9), for the owner's review. Fixes the detailed API
that record 0004, point 9, left to P4.9; implemented in the same change
(`neuron-client::render::pass`, `platform::d3d12::{pass, backend::ex,
dxc}`). Shader model 6.1 (DXIL from `dxc`, HLSL 2021) follows the owner's
change of the shader toolchain of 2026-11-19, which amends record 0004,
point 16, in its own change.

## Context

Paths are relative to `CppSource/`.

- **The interface.** `include/render/IRenderPipelineEx.h` declares the
  port-only `IRenderPipelineEx` (17 methods) and `IRPDrawEx` (2) of
  `DX9RENDERER.dll`; DX8RENDERER does not answer them, and the client keeps
  its fixed-function algorithms when the query fails. Every type is
  version neutral: opaque `FRPExProgram` and `FRPExTexture` handles, six
  formats (`ERPExFormat` 1 to 6: `A8R8G8B8`, `L8`, `A16B16G16R16F`, `R16F`,
  `R32F`, `G16R16F`), an explicit sampler state per binding (point or
  linear, no/point/linear mip, wrap/clamp/mirror per axis), a size-versioned
  caps struct, a render-target description whose zero width or height means
  "`BackBufferScale` of the back buffer, at least one texel, evaluated
  again at every reset", and a pass description (colour target or the back
  buffer, `KeepDepth`, `ClearColor`).
- **The implementation.** `DX9RENDERER/DX9RENDEREREx.cpp` (1,215 lines).
  Programs are compiled from a `FileSystem` path with `D3DXCompileShader`
  as `vs_3_0` and `ps_3_0`, includes resolved through the same file system;
  a handle is returned even when compilation fails, and `ReloadExPrograms`
  keeps the previous code of a program that fails again (`CompileProgram`,
  `:265-324`; `CreateExProgram`, `:385`; `ReloadExPrograms`, `:431`).
  Render targets are `D3DPOOL_DEFAULT` textures released and made again at
  every reset (`CreateRenderTargetGpu`, `ExReleaseDeviceResources`);
  volumes are managed one-level textures filled from tightly packed texels
  the caller hands in (`CreateVolumeGpu`). The scene depth is an `INTZ`
  depth texture made instead of the automatic depth surface when the
  device offers `INTZ` and the chosen depth format is 24-bit
  (`ExWantsIntzDepth`, `ExCreateDeviceResources`); otherwise
  `GetSceneDepth` fails.
- **The pass.** `BeginExPass` (`:558`) saves the colour target, depth
  surface, cached viewport and every pending render state; sets
  `ALPHABLENDENABLE`, `ALPHATESTENABLE`, `FOGENABLE`, `STENCILENABLE`,
  `LIGHTING` off, `CULLMODE` none and `COLORWRITEENABLE` 0xF through the
  pipeline's cache; selects the target (`ExSelectTarget`, `:44`: a texture
  bound for sampling is unbound when it becomes the target; `KeepDepth`
  binds the depth surface and unbinds the scene depth from its samplers,
  otherwise no depth surface and `ZENABLE`, `ZWRITEENABLE` off); clears
  when asked. Inside a pass: `SetExRenderTarget`, `BindExProgram`
  (`nullptr` returns to fixed function), `SetExVertexConstants` and
  `SetExPixelConstants` (Direct3D 9 float registers, settable outside a
  pass too), `BindExTexture` and `BindExPipelineTexture` on samplers 0 to
  15 (binding the scene depth while it is the depth surface unbinds the
  surface for the rest of the pass), `DrawFullscreenQuad` (`XYZ | TEX1`,
  shifted by Direct3D 9's half pixel, `:729`) and `DrawExPrimitiveUp`.
  `EndExPass` (`ExEndPassInternal`, `:91`) unbinds the shaders, restores
  the stages and samplers the pass touched, the target, the depth surface
  and the viewport, and re-queues the saved render states. Passes do not
  nest; `DestroyExTexture` is refused inside one.
- **The callers.** `OutpostFrontier/Client/nebula_volume.cpp` (the volumetric
  nebula, P6.4) creates two programs, a 128³ `A8R8G8B8` noise volume and a
  render target at `resolution_scale` of the back buffer (FP16, falling
  back to 8-bit when refused, `:1060-1083`), and per nebula runs one pass:
  a ray march into the target sampling the scene depth (point) and the
  noise (linear, wrap), then `SetExRenderTarget(nullptr, 0)` and a
  composite onto the back buffer with `ONE, SRCALPHA` blending
  (`DrawEntry`, `:2408-2504`). `render_ex.cpp` draws a debug view of the
  scene depth the same way (`:250-308`) and polls the shader files once a
  second for a reload. The shaders and `volumetric.ini` are not in the
  data (fact F12); P6.4 writes them (decision of 2026-11-17).
- **What record 0004 fixed.** `ExProgram` and `ExTexture` handles, render
  targets sized by a back-buffer scale and recreated on resize behind the
  same handle, volume textures, the scene depth as a texture kept in a
  typeless format, a pass value from `begin_pass` that restores target,
  viewport, bindings and every state the pass changed when it drops,
  programs compiled ahead of time (T1) with run-time compilation only
  behind a development feature. The fixed-function root signature, the
  shader-visible view ring (four views per draw), the sampler heap of
  1,050 fixed samplers, the upload ring and the pipeline-state cache are
  record 0004's, point 8.

## Decision

### 1. Where it lives

1. **Platform-free part.** `neuron-client::render::pass` holds the types,
   the resource model (`model::ExModel`), the device calls and the pass
   (`device`), and the recording backend's side (`recording`). The device
   calls are an `impl<B: Backend + ExBackend> Device<B>` block: the device
   of record 0004 gains one field, `ex: ExModel`, and nothing else changes
   for fixed-function callers. `ExBackend` is the programmable half of a
   backend (`ex_caps`, `ex_create`, `ex_release`, `ex_clear`, `ex_draw`,
   `ex_end_pass`, `back_buffer_resized`); `RecordingBackend` and
   `D3d12Backend` implement it.
2. **Direct3D 12 part.** `platform::d3d12::pass` builds the pass root
   signature, pipeline states, samplers and views; `platform::d3d12::backend::ex`
   implements `ExBackend` on `D3d12Backend`; `platform::d3d12::dxc` is the
   run-time compiler of point 11. All of it is under `platform/`, the one
   unsafe directory; every unsafe block carries its `// SAFETY:` comment.

### 2. Handles and lifetimes

3. `ExProgram` and `ExTexture` are generational handles into two arenas of
   the `ExModel` (record 0004, point 2). A handle lives from its `create_*`
   call to its `destroy_*` call; afterwards it is stale and every call that
   takes it returns `PassError::Stale`. Render targets keep their handle
   across back-buffer changes. The scene depth is an `ExTexture` the device
   makes with itself (`Device::scene_depth`); it keeps its handle across
   resizes and `destroy_ex_texture` refuses it (`PassError::SceneDepth`).
   Because a pass borrows the device (point 9), nothing can be created or
   destroyed while a pass is open.
4. Calls and their C++ counterparts:

   | Rust (`Device`) | C++ |
   | --- | --- |
   | `ex_caps() -> ExCaps` | `GetExCaps` |
   | `create_program(&ProgramBlobs) -> Result<ExProgram>` | `CreateExProgram` |
   | `create_program_from_file`, `reload_programs` (feature `shader-compile`) | `CreateExProgram`, `ReloadExPrograms` |
   | `destroy_program(ExProgram)` | `DestroyExProgram` |
   | `create_render_target(RenderTargetDesc) -> Result<ExTexture>` | `CreateExRenderTarget` |
   | `create_volume_texture(w, h, d, ExFormat, Option<&[u8]>) -> Result<ExTexture>` | `CreateExVolumeTexture` |
   | `destroy_ex_texture(ExTexture)` | `DestroyExTexture` |
   | `ex_texture_size(ExTexture) -> Result<(u32, u32)>` | `GetExTextureSize` |
   | `scene_depth() -> ExTexture` | `GetSceneDepth` |
   | `begin_pass(PassDesc) -> Result<Pass>` | `BeginExPass` |
   | `resize(width, height)` | the re-evaluation in `CreateBuffers` after a reset |
   | `Pass::end` or dropping the pass | `EndExPass` |
   | `Pass::set_target(PassTarget, keep_depth)` | `SetExRenderTarget` |
   | `Pass::bind_program(Option<ExProgram>)` | `BindExProgram` |
   | `Pass::set_vertex_constants(start, &[[f32; 4]])`, `set_pixel_constants` | `SetExVertexConstants`, `SetExPixelConstants` |
   | `Pass::bind_texture(slot, Option<ExTexture>, SamplerState)` | `BindExTexture` |
   | `Pass::bind_device_texture(slot, Option<TextureHandle>, SamplerState)` | `BindExPipelineTexture` |
   | `Pass::set_render_state(RenderState)` | `IRenderPipeline::SetRenderState` inside a pass |
   | `Pass::draw_fullscreen_quad()` | `IRPDrawEx::DrawFullscreenQuad` |
   | `Pass::draw(prim, Fvf, &[u8], count)` | `IRPDrawEx::DrawExPrimitiveUp` |

   Errors are `PassError`: `InvalidParams` (`GR_INVALID_PARMS`), `Stale`,
   `NotRenderTarget`, `TargetBound`, `NoProgram`, `ProgramUnusable`,
   `SceneDepth`, `Backend(reason)` (the GPU refused a creation, which the
   nebula's FP16 fallback needs), `Compile { program, log }`.

### 3. Formats

5. `ExFormat` keeps the C++ numbers (`from_raw`, `to_raw`) and sizes:

   | `ExFormat` | C++ | Bytes | Resource | Sampled as |
   | --- | --- | --- | --- | --- |
   | `Bgra8` | `A8R8G8B8` | 4 | `B8G8R8A8_UNORM` | (r, g, b, a) |
   | `L8` | `L8` | 1 | `R8_UNORM` | (l, l, l, 1) |
   | `Rgba16Float` | `A16B16G16R16F` | 8 | `R16G16B16A16_FLOAT` | (r, g, b, a) |
   | `R16Float` | `R16F` | 2 | `R16_FLOAT` | (r, 1, 1, 1) |
   | `R32Float` | `R32F` | 4 | `R32_FLOAT` | (r, 1, 1, 1) |
   | `Rg16Float` | `G16R16F` | 4 | `R16G16_FLOAT` | (r, g, 1, 1) |

   The "sampled as" column is Direct3D 9's reading of each format, kept
   through the view's component mapping, so a shader written against the
   C++'s formats reads the same values.
6. **The scene depth.** The depth buffer is one `R24G8_TYPELESS` resource
   with a `D24_UNORM_S8_UINT` depth view (the fixed-function pipeline
   states keep that format) and an `R24_UNORM_X8_TYPELESS` shader view
   that reads the depth in every channel (the C++ promised red). It is in
   the depth-write state for every fixed-function draw and clear, moves to
   the pixel-shader-resource state when a pass samples it, and back when
   the pass ends. It cannot be sampled and bound at once: binding it to a
   slot unbinds the depth buffer for the rest of the pass, as in the C++,
   and binding the depth again (`set_target(_, true)`) unbinds it from its
   slots. It is the scene's size and is made again with the scene at a
   resize.

### 4. Render targets and volume textures

7. **Render targets.** `RenderTargetDesc { size: TargetSize, format }`,
   with `TargetSize::Fixed { width, height }` (both above zero) or
   `TargetSize::Scaled(scale)` (scale above zero). A scaled side is
   `CreateRenderTargetGpu`'s `(unsigned long)((float)W * scale)`: the
   product in `f64` (exact for these operands, T11), truncated through 64
   bits, at least 1 (`TargetSize::resolve`). `Device::resize` sets the new
   back-buffer size in the model and calls `back_buffer_resized`, which on
   Direct3D 12 runs `D3d12Backend::resize`: the scene, the depth buffer
   and its two views, the swap chain, and every scaled target are made
   again at their new sizes behind the same handles and view slots, their
   contents lost. Fixed-size targets are kept with their contents. Each
   target has its own render-target view and a shader view in the
   backend's view heap; it starts in the render-target state and moves to
   the shader-resource state when sampled. The scene target of record
   0004 (before the gamma blit) is the C++'s back buffer:
   `PassTarget::Scene`.
8. **Volume textures.** One level of `width * height * depth` texels,
   tightly packed, rows then slices; `None` is all zeros (the C++'s
   `memset`). Data of another length is refused. On Direct3D 12 a
   `TEXTURE3D` of the format, filled through the upload ring with
   `GetCopyableFootprints`' row and slice pitches, sampled through a 3D
   view with the format's mapping. Feature level 11_0 allows 2,048 texels
   per side.

### 5. Programs, constants and samplers

9. **The pass value.** `Device::begin_pass(PassDesc { target, keep_depth,
   clear })` returns `Pass<'_, B>`, which holds `&mut Device<B>`. While it
   lives the device cannot draw, create, destroy or begin another pass;
   nesting, ordinary draws inside a pass and `DestroyExTexture` inside a
   pass are compile errors instead of run-time refusals. The pass starts
   from a copy of the device's render states with `BeginExPass`'s
   overrides (listed under "The pass" above) and applies the target rules of
   `ExSelectTarget`; `set_render_state` changes only that copy. The
   restore set of the C++ (target, depth surface, viewport, stage textures
   and sampler states, every render state the pass changed, no program
   bound) holds by construction: the device's states, viewport, stage
   textures and transforms are never written by a pass, and when the pass
   drops `ex_end_pass` returns the depth buffer to the depth-write state.
   The next fixed-function draw sets its own root signature, pipeline
   state, views, viewport and targets, as every draw does. A failed
   `begin_pass` ends the pass it opened, as `BeginExPass` runs
   `ExEndPassInternal`.
10. **Draw packets.** Each programmable draw is an `ExDraw`: the
    `ExPipelineKey` (program; record 0004's `PipelineKey` with the draw's
    vertex format, topology, blend, colour writes, depth function and
    write, bias, cull and fill, never flat, depth `None` without a depth
    buffer; the target's format; whether depth is bound), the target and
    its size (the viewport covers it), every vertex and pixel register,
    the sixteen slot bindings with their sampler states, and the vertices
    (fans converted to lists, as for fixed-function draws). The backend
    keeps no pass state of its own. The full-screen quad is four
    `XYZ | TEX1` vertices in clip space with coordinates (0, 0) top left to
    (1, 1) bottom right, unshifted: Direct3D 12 samples pixel centres, so
    pixel (i, j) interpolates ((i + 0.5) / W, (j + 0.5) / H), the value the
    C++'s half-pixel shift produced.
11. **The pass root signature.** Programs have their own root signature,
    built in Rust beside the fixed-function one:

    | Parameter | Content | HLSL | Visibility |
    | --- | --- | --- | --- |
    | 0 | Root constant buffer: the 256 vertex registers (4,096 bytes) | `b0` | vertex |
    | 1 | Root constant buffer: the 224 pixel registers (3,584 bytes) | `b0` | pixel |
    | 2 | Table of 16 views | `t0` to `t15` | pixel |
    | 3 | Table of 16 samplers | `s0` to `s15` | pixel |

    Each stage keeps its own `c` registers as in Direct3D 9: a program
    declares `cbuffer ... : register(b0) { float4 c[256]; }` in its vertex
    shader and `{ float4 c[224]; }` in its pixel shader. The registers are
    device state (`ExModel`): set inside a pass, they stay set for later
    passes, as Direct3D 9's do; a range past the last register is refused
    and writes nothing. Both blocks go through the upload ring with every
    draw. Slot n is view `tn` sampled with sampler `sn`; an empty slot
    reads the null 2D view (Direct3D 12's binding tiers require every
    table entry to hold a descriptor). Views are copied per draw into the
    shader-visible view ring of record 0004 (sixteen per draw). The 162
    distinct sampler states (`SamplerState::index`) are made once in a CPU
    sampler heap and copied per draw into a ring of 992 samplers placed
    after the 1,050 fixed ones in the shader-visible sampler heap (2,042 of
    the 2,048 a heap may hold); a frame that fills either ring is flushed
    and goes on, as fixed-function draws do. The vertex input is record
    0004's layout for the draw's vertex format, with the defaults vertex in
    slot 1 for elements the format lacks. Pipeline states are cached by
    `ExPipelineKey` and released (after a flush) when their program is
    destroyed or gets new code.
12. **Program blobs.** Sources live in `neuron-client/shaders/`, one entry
    per stage in `shaders.toml` with profile `vs_6_1` or `ps_6_1` (HLSL
    2021: no vector `&&`, `||` or `?:`; `and()`, `or()`, `select()`), and
    `cargo xtask shaders` writes the blobs to `compiled_shaders/` with the
    toolchain of record 0004 point 16 as amended (DXIL from the pinned
    `dxc`). A program's blobs are named `<stem>_vs.cso` and
    `<stem>[_<variant>]_ps.cso`, the variant being the pixel entry without
    `ps_` (`pass_test.hlsl`: `pass_test_vs.cso`, `pass_test_constant_ps.cso`).
    `program_blobs!("name", "<vs blob>", "<ps blob>")` embeds both with
    `include_bytes!` into a `ProgramBlobs`, and `create_program` hands the
    backend the bytes, which it keeps for its pipeline states. The backend
    assumes DXIL and does nothing format specific. `shaders/pass_test.hlsl`
    is the example program and the WARP tests' fixture.
13. **Run-time compilation (development only).** The feature
    `shader-compile` (off by default, never in a release build) adds
    `ShaderCompiler` (`compile(path, entry, profile) -> Result<blob, log>`),
    `Device::create_program_from_file(path, vs_entry, ps_entry, compiler)`
    and `Device::reload_programs(compiler)`. As in the C++ a program that
    does not compile still gets its handle (in `PassError::Compile`),
    unusable until a reload compiles it (`bind_program` returns
    `ProgramUnusable`); a reload compiles every file program, keeps the
    previous code of one that fails and reports the first failure; both
    stages must compile before the backend gets new code. The Direct3D 12
    compiler (`platform::d3d12::dxc::Dxc`) loads `dxcompiler.dll` and
    `dxil.dll` from the pinned `Microsoft.Direct3D.DXC` package beside the
    executable (the feature links the DLL), reads the file from disk,
    resolves includes beside it with the default include handler and
    compiles with the same options as `cargo xtask shaders`. Watching the
    files for a reload is the caller's (P6.4).

### 6. Caps

14. `ExCaps` answers for feature level 11_0: shader model 6.1, textures to
    16,384 and volumes to 2,048 texels per side, scene depth, FP16 targets
    with blending, `R32F` targets and volume textures all present, and the
    adapter's description. The Direct3D 9 instruction-slot fields of caps
    version 2 have no Direct3D 12 counterpart and are not reported; the
    nebula's march budget is set by P6.4 from settings, not from the
    driver.

### 7. Tests

15. On every host: the formats, the sampler numbering, the scaled sizes,
    the quad, the model's resize and register ranges, and the pass rules
    against `RecordingBackend` (starting states, the device untouched
    after a pass, depth inherited with `keep_depth`, scene-depth sampling
    unbinding the depth, target and slot rules, fans, constants kept
    across passes, creation failures and the FP16 fallback, and with
    `shader-compile` the reload rules on a fake compiler).
16. On CI's Windows job (WARP, in `platform::d3d12::backend::ex`): a
    program draws into a target at half the back buffer and reads it back,
    then the same handle has the new size after a resize; a program
    samples the scene depth a fixed-function draw wrote, after which the
    depth buffer tests depth again; a program samples a volume texture at
    two depths; a pass into the scene with its own blending leaves the
    device's viewport and blend state as they were. The tests skip with a
    notice while the test blobs are empty placeholders, so that the job
    reaches `cargo xtask shaders --check`, which prints the blobs to
    commit.

## Consequences

- P6.4 writes its shaders against point 11's register layout and the
  formats of point 5, with `nebula_volume.cpp`'s register numbers, and
  compiles them with `cargo xtask shaders`; during shader work it turns on
  `shader-compile` for reloads.
- Nothing in the fixed-function path changes except the depth buffer's
  format (typeless behind the same depth view) and a larger shader-visible
  sampler heap; the WARP tests of P4.2 and the image replay keep running.
- Passes are cheap to get right: the borrow rules make nesting, draws
  inside a pass and destruction during one impossible, and the restore set
  needs no bookkeeping.
- Divergences D410 to D419 record the differences from DX9RENDEREREx.

## Options rejected

- **The C++'s save-and-restore of device state in the pass.** Needs a
  saved copy of every render state, the stage caches and the viewport and
  a restore path that can be forgotten; a pass-local state copy with the
  device borrowed gives the same result by construction.
- **Programs that may draw fixed function inside a pass.** The C++ allows
  it after `BindExProgram(nullptr)`; no caller does it, and it would need
  the fixed-function pipeline states for every pass target format and for
  "no depth".
- **Sampler states as static samplers or a sampler array indexed in the
  shader.** Static samplers cannot change per draw; an indexed array makes
  every program carry the sampler logic. A copied table keeps `tn` with
  `sn` as the C++'s sampler n.
- **One constant buffer for both stages.** Would merge the two Direct3D 9
  register files and break the nebula's register numbers.
- **Root constants for the registers.** 256 + 224 registers far exceed
  the 64 words of a root signature.
- **Depth as `D32_FLOAT` or a copy of the depth for sampling.** A copy
  costs a pass per frame; a format change would alter the fixed-function
  depth precision (24 bits, as Direct3D 8's `D24S8`).
- **Run-time compilation through the C++'s file system and include
  handler.** Development only, so reading the source tree on disk is what
  shader work wants; release builds have no compiler.
- **Reporting Direct3D 9-style instruction budgets.** No Direct3D 12 query
  answers them.

## Open questions for the owner

1. Is the register layout of point 11 (each stage's registers at `b0` of
   its own stage) the one P6.4 should write against?
2. Should fixed-size render targets lose their contents at a resize, as
   the C++'s did (D417)? Keeping them costs nothing and no caller relies
   on the loss.
3. `shader-compile` uses the same `dxc` options as `cargo xtask shaders`;
   `platform::d3d12::dxc::FLAGS` must follow the amended record 0004 point
   17 when the toolchain change lands.
