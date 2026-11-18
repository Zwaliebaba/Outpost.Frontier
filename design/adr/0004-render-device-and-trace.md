# 0004 — Render device API, trace format version 1, shader toolchain

Status: accepted by the owner on 2026-11-18 (P4.0); decision O25. Implements decisions T1, T2 and T10 for the
renderer; fixes the trace format that P0.5 left provisional.

## Context

The C++ draws through one renderer object behind seven interfaces. Paths are
relative to `CppSource/`.

- **The seam.** `include/render/IRenderPipeline.h` declares 110 methods:
  `IRenderPipeline` 74 (slots 3 to 76), `IVertexBufferManager` 6,
  `IRPDraw` 1, `IRPIndexBuffer` 9, `IRPVertexBuffer` 10, `IGammaControl` 5,
  `IRPTexture` 5. `DX8RENDERER/DX8RENDERER.cpp` implements them on
  Direct3D 8 without any shader. `DX9RENDERER` mirrors them and adds the
  port-only `IRenderPipelineEx` (17) and `IRPDrawEx` (2)
  (`include/render/IRenderPipelineEx.h`): programs, render targets, volume
  textures, the scene depth as a texture, pass bracketing that restores
  state at `EndExPass`. Only the volumetric nebula uses them (P6.4).
- **Callers.** Outside the renderers, the trace proxy and the SDK there are
  457 `SetRenderState` and 193 `SetTextureStageState` call sites. They name
  33 render states and 11 stage states by their `D3DRS_` and `D3DTSS_`
  names (most often `ZWRITEENABLE`, `ZFUNC`, `CULLMODE`, `ALPHAREF`,
  `LIGHTING`, `AMBIENT`, `COLORVERTEX`, the blend pair; `TEXTURETRANSFORMFLAGS`
  and `TEXCOORDINDEX`); the rest pass numbers or values from material
  recipe tables. No call site names a texture stage above 1 as a literal.
  `SetLookat` and `BlitTexture` have no caller; `GetBufferInterface`,
  `GetDeviceStats`, `GetTextureInterface`, both render-target methods and
  the two `Unsupported*` slots are stubs (`DX8RENDERER.cpp:3423`, `:3434`,
  `:4205`, `:4369-4388`).
- **State tables and defaults.** The renderer knows 76 render states with
  defaults (`kRenderStateProperties`, `DX8RENDERER.cpp:141-216`) and 27
  states per texture stage over 8 stages (`:222-260`); after `Initialize`
  every stage, stage 0 included, has `COLOROP` and `ALPHAOP` disabled
  (`:2544-2568`). `Startup` reads each render-state default again from the
  engine INI section (`:2688-2695`); the shipped `[RenderPipeline]` in
  `outdir/NeuronComponents.ini` sets `ALPHAREF 0`, `ALPHATESTENABLE true`,
  `ALPHAFUNC 5`, `LOCALVIEWER false` and `zfunc 4`. Those values, not the
  table's, are what `CreateBuffers` and `ResetRenderStatesToDefaults` apply
  (`ApplyDefaultRenderStates`, `:2377`; `:4389`).
- **When state takes effect.** `SetRenderState` only marks a pending value
  (`:4401`); `SetTextureStageState` writes the device at once through a
  cache (`:4422`). Both are seen by the GPU only at the next draw, which
  flushes render states, stream and index selection, the three transforms
  and the light manager (`FlushDrawState`, `:1907`). So the observable
  model is "state at draw time".
- **Transforms.** `SetModelview` puts a Z flip into the world matrix while
  pipeline state 7 (`VIEWSPACE_LIGHTS`, default 1) is on, and sets an
  identity view (flipped when it is off) the first time
  (`:3621-3660`). `SetPerspective` builds its matrix in double precision
  and rounds each element to `float` (`:3829-3861`).
- **Geometry.** Primitive counts come from `PrimCountFromVerts` with signed
  division (`:2132`, types 1 to 6, fans included). Strides come from
  DX8RENDERER's `VertexStrideFromFvf` (`:1773`), not from Direct3D: it
  ignores `PSIZE` and gives `XYZB2` no position size. `DrawIndexedPrimitiveVb`
  copies the caller's indices into a scratch index buffer per draw
  (`:4643-4688`); handle `-1` means the scratch buffer, the current vertex
  buffer or a pooled one per FVF. `CopyVertexData` and `CopyVertices`
  convert from a five-stream source descriptor (`FRpVertexSrcDesc`,
  `IRenderPipeline.h`) into the destination FVF.
- **Clear and present.** `ClearBuffers` takes colour, depth and stencil from
  pipeline states 8, 9 and 10 (`:3133`). `SwapBuffers` presents to the
  `SetWindow` override with source and destination rectangles when one is
  set (`:3162`). `LockBuffer` fails unless `LOCKABLE_BACKBUFFER` is set,
  which the shipped INI does not do (`:2625-2631`, `:3282`); its one caller
  locks and unlocks without reading (`OutpostFrontier/Client/texture_panels.cpp:1752`).
- **Gamma and screenshots.** The client calls `SetGammaEnable` and
  `SetGammaFunction(7, gamma, 0, 1, 0)` (`OutpostFrontier/Client/engine_init.cpp:491-493`),
  a per-channel curve on a hardware ramp. `PrintScreen` reads the front
  buffer and saves by extension, seven formats (`DX8RENDERER.cpp:6191`,
  `:6227`); both callers write BMP (`main_command_handler.cpp:113`,
  `oracle_mode.cpp:415`). A front-buffer copy carries no hardware ramp.
- **Lights.** A light is keyed by the caller's pointer; `AddLight` and
  `UpdateLight` store the pointer and the renderer reads the struct at the
  next draw (`IRenderPipeline.h` slot 75 notes, `DX8RENDERER.cpp:578-623`).
- **The trace.** `RenderTrace.dll` records every call as JSONL, format
  version 0 (`RenderTrace/README.md`, generated by `make_render_trace.py`):
  floats as hex bit patterns, handles as stable small ids, data in blobs
  named by FNV-1a 64, a state snapshot at the start of each selected frame,
  and a replay contract with recorded buffer offsets as the authority. The
  owner's menu trace has about 3,200 records per frame. Its listed limits:
  light data held at the call, not at the draw; no image of the frame; GPU
  content of skipped frames missing.
- **Pixel and texel rules.** Direct3D 8 puts pixel centres on integer
  coordinates; Direct3D 12 puts them at +0.5. Texel addressing is the same
  in both ([05-rendering](../assessment/05-rendering.md) section 10).

## Decision

### 1. The typed device (`neuron-client::render`)

1. **One value owns the renderer.** `render::Device<B: Backend>` holds the
   platform-free state model and a backend. There is no global; the
   `Engine` value of P4.16 owns the device. Two backends exist, so a trait
   is justified under T9: `D3d12Backend` (`cfg(windows)`) and
   `RecordingBackend` (any host), which keeps the draw packets for tests.
2. **Handles.** `TextureHandle`, `VertexBufferHandle`, `IndexBufferHandle`
   and `LightHandle` are generational (`index: u32, generation: u32`) into
   arenas the crate implements, as record 0001 does for objects. A stale
   handle is an error, not undefined behaviour. The `-1` handles become
   enum cases: `IndexSource::Scratch`, `VertexSource::Current` and
   `VertexSource::Pool(Fvf)`.
3. **States.** `RenderState` is an enum with one variant per render state
   in DX8RENDERER's table (76; corrected from 74 on 2026-11-18, P4.3), each carrying a typed value
   (`ZFunc(Compare)`, `SrcBlend(Blend)`, `FogStart(f32)`, `Ambient(Color)`,
   `AlphaRef(u32)`, …). `StageState` does the same for the 27 stage states;
   `Stage` is 0 to 7. `RenderState::from_raw(id, value)` and `to_raw()`
   carry the Direct3D 8 numbering, used only at the trace boundary and in
   tests; the numbers are format, so they are kept, the C++ names are not.
   The state model stores every state. The shader honours the states listed
   in the table under point 6; a draw that relies on any other state with a
   non-default value is logged once per state, not emulated.
4. **Defaults.** The model builds its default table as DX8RENDERER does:
   the built-in values, then the engine INI section through
   `neuron-core::ini` (T22). `reset_render_states()` and device creation
   apply that table, then the stage defaults, then identity transforms.
5. **Calls.** The device mirrors the C++ groups with Rust types:
   transforms (`set_world`, `set_view`, `set_modelview` with the Z-flip
   rule, `set_projection`, `set_ortho`, `set_perspective` in the C++'s
   `f64`-then-`f32` order under T11, `set_world_n` for the bone palette);
   lights (`create_light(Light)`, `update_light`, `enable_light`,
   `add`/`remove`, `destroy`); `set_material(Material)`; textures
   (`create_texture`, `lock_level` returning a guard with the level's bytes
   and pitch, `set_level_data` through the `neuron-core::image`
   conversions, `load_texture`, `load_cubemap`, `image_info`); buffers
   (`create_vertex_buffer`, `lock`, `copy_vertices`, `resize`, and the same
   for 16-bit index buffers); dynamic memory (`lock_vertex_memory(fvf,
   count)` returns a guard over the bytes and a `VertexListing { buffer,
   fvf, base_vertex }`, which is `Copy` so materials can keep it after the
   guard drops; `copy_vertex_data` writes through it); draws (`draw(prim,
   VertexSource, Option<IndexSource>)`, one entry for the four C++ draw
   methods and `IRPDraw`); frame (`clear(ClearFlags, Option<Rect>)`,
   `begin_scene`, `end_scene`, `present`, `set_present_window`);
   `read_back_buffer` (honouring `LOCKABLE_BACKBUFFER` as the C++ does);
   gamma (`set_gamma_function`, `set_gamma_ramp`, `gamma_ramp`,
   `set_gamma_enabled`); `screenshot(path)`. Results are
   `Result<_, RenderError>`; the trace player maps errors back to the C++
   result codes. Getters return values instead of writing through
   pointers.
6. **The platform-free model** (`render::ff`, built and tested on every
   host) does everything that is not GPU work:

   | Part | Content | Tested by |
   | --- | --- | --- |
   | State | Render and stage states, defaults, reset, pipeline properties 0 to 13 | Unit tests; model replay of traces (section 2) |
   | Transforms | World, view, projection, modelview rule, texture transforms, palette | Model replay, Tier B for `set_perspective` and `set_ortho` |
   | Lights and material | Light table, enable set, data as Direct3D 8 lays it out | Model replay |
   | Resources | Arenas, CPU shadow of every buffer and lockable texture, dirty ranges, the pooled vertex rings and the scratch index buffer | Unit tests |
   | Vertex formats | FVF parsing into an input layout; DX8RENDERER's stride function with its quirks; the `CopyVertexData` and `CopyVertices` conversions | Tier A against trace unlock data |
   | Geometry | `PrimCountFromVerts`; fans converted to lists by index generation; strips, lines and points kept | Unit tests |
   | Draw packet | Pipeline-state key, per-draw constant block, resolved vertex and index ranges, bound textures and samplers | `RecordingBackend` tests |
   | Gamma | The 256-entry ramp of `SetGammaFunction` per channel | Tier B unit tests |
   | Image I/O | BMP and PNG writers for screenshots, the image comparison of section 2 | Unit tests |

   The pipeline-state key holds only what Direct3D 12 bakes into a pipeline
   state: input layout, topology type, blend enable, source and destination
   blend, blend operation, colour write mask, depth enable, write and
   function, depth bias, cull and fill mode, target formats. Everything
   else goes into the per-draw constant block: world-view and projection
   matrices, two texture transforms with their flags and coordinate
   sources, up to sixteen enabled lights, material and material sources,
   ambient, colour-vertex and specular switches, fog mode, start, end,
   density and colour, alpha test function and reference, texture factor,
   the two stages' colour and alpha operations and arguments, and the pixel
   offset of point 7. Lighting, fog, coordinate generation, the stage
   combiners and alpha test run in one vertex and one pixel shader that
   branch on the block (T2). The shader honours: `ZENABLE`, `ZWRITEENABLE`,
   `ZFUNC`, `ZBIAS`, `FILLMODE`, `CULLMODE`, `ALPHABLENDENABLE`,
   `SRCBLEND`, `DESTBLEND`, `BLENDOP`, `COLORWRITEENABLE`,
   `ALPHATESTENABLE`, `ALPHAREF`, `ALPHAFUNC`, `LIGHTING`, `AMBIENT`,
   `COLORVERTEX`, `SPECULARENABLE`, `LOCALVIEWER`, `NORMALIZENORMALS`, the
   four material sources, the fog states, `TEXTUREFACTOR`, `VERTEXBLEND`,
   `INDEXEDVERTEXBLENDENABLE`, `SHADEMODE`, and for stages 0 and 1 every
   operation, argument, address, filter, coordinate-index and transform
   state. `DITHERENABLE`, `CLIPPING`, `SOFTWAREVERTEXPROCESSING`,
   `LASTPIXEL` and `MULTISAMPLEANTIALIAS` have no effect on Direct3D 12
   and are stored only. P4.3 confirms the list from the v1 traces and adds
   any state they show in use.
7. **Conventions.** The vertex shader moves every position by half a pixel
   (`+1/width`, `-1/height` in clip space, scaled by `w`: right and down;
   corrected on 2026-11-18, P4.2, from `-1/width`, `+1/height`, which is
   the shift that makes Direct3D 9 sample like Direct3D 10, the opposite
   direction) so pixel centres fall where Direct3D 8 put them: Direct3D 8
   samples a pixel at its integer coordinates, Direct3D 12 at its centre. Pre-transformed (`XYZRHW`) vertices take
   the same shift. Matrices are uploaded row-major as the C++ stores them;
   the shaders are compiled with row-major packing (section 3).
8. **The backend** (`render::d3d12`, `cfg(windows)`, P4.2) owns the device,
   queue, flip-model swap chain, fences, descriptor heaps, a fixed set of
   static samplers, a per-frame upload ring for dynamic and user-pointer
   data, default-heap copies of static buffers and textures, the
   pipeline-state cache keyed by the model's key, the scene target with
   the final blit that applies the gamma ramp, and readback. The root
   signature is built in Rust, not in HLSL. Screenshots read the scene
   target before the gamma blit, as a front-buffer copy did.
9. **The programmable surface for P4.9 and P6.4** keeps the shape of
   `IRenderPipelineEx`: `ExProgram` and `ExTexture` handles, render targets
   sized by a back-buffer scale and recreated on resize behind the same
   handle, volume textures, the scene depth as a texture (the backend keeps
   depth in a typeless format so it can be sampled), and a pass value
   returned by `begin_pass` that restores target, viewport, bindings and
   every state the pass changed when it drops. Programs are compiled ahead
   of time (T1); runtime compilation stays behind a development feature.
   Its detailed API is fixed in P4.9, not here.

### 2. Trace format version 1 and the replay contract

10. **Kept from version 0:** JSONL with one object per call; `seq`,
    `frame`, `interface`, `method`, `args` by the header's argument names,
    `ret`; floats and float arrays as hex words; handles as never-reused
    ids with `0` and `-1`; blobs named by FNV-1a 64; the three recording
    groups; the snapshot at the start of a selected frame; the recorded
    buffer offsets as the authority. The trace stays at the C++ interface
    level, so a replay exercises the Rust state model, not only the
    backend.
11. **Changes in the proxy** (oracle instrumentation, permitted by the
    Phase 0 rules):
    - The header says `"version":1` and adds `"defaults"`: the 76
      render-state values after the INI was read, the 27 stage defaults of
      stage 0 and of stages 1 to 7, and the pipeline-property values. A
      replayer then knows what `CreateBuffers` and
      `ResetRenderStatesToDefaults` apply.
    - After the `SwapBuffers` of each selected frame the proxy calls the
      inner `IRPTexture::PrintScreen` into a temporary BMP and writes a
      line `{"frame":N,"frame_image":"<hash>","width":W,"height":H}`. The
      image is the reference for that frame. The traced client must run
      unoccluded (a front-buffer copy reads the screen).
    - At each draw of a selected frame, for every light whose `AddLight`
      or `UpdateLight` came after the previous draw, the proxy writes a
      synthetic `LightData` line with the 26 words it reads from the
      caller's struct at that moment. This removes the version 0 limit on
      light data.
    - `README.md` and the generator move to version 1; the Rust reader
      rejects version 0. The owner records the menu trace again.
12. **Replay modes.** The player is `render::trace`, behind the feature
    `render-trace` so the game executable does not carry it.
    - *Model replay* (any host, `RecordingBackend`): every recorded call is
      applied to the model; every getter's recorded out-value and every
      `ret` must equal the model's. Values that pass through integer moves
      or `CopyVertexData` must be equal bit for bit (Tier A); the results of
      `SetPerspective`, `SetOrtho` and the gamma curve within the Tier B
      default bound. This runs on traces of the original data on the data
      machine and on the committed synthetic traces in CI.
    - *Image replay* (Windows): the replay contract of version 0 applied to
      the D3D12 backend; after the frame's `SwapBuffers` the scene target
      is read back and compared with `frame_image`.
13. **Method mapping.** Each of the 110 methods maps to one device call or
    to a fixed disposition:

    | Group (methods) | Disposition |
    | --- | --- |
    | Lifecycle and mode (12): `Startup` … `DestroyGpuResources` | Device created with the recorded back-buffer size and the header defaults; `CreateBuffers` resets as the C++ does; pipeline properties stored; mode enumeration, device info and abilities answered from recorded values, not replayed |
    | Frame (8) | `clear`, `present`, scene markers, `read_back_buffer`; `GetBufferInterface`, `GetDeviceStats` return the recorded result |
    | Viewport and window (6) | `set_viewport`, `set_depth_range`, `set_present_window` (the `HWND` is ignored) |
    | Transforms (12), lights (8), material (2) | The device calls of point 5; `LightData` lines update the light before the draw |
    | Textures (10) | Device calls; `GetTextureInterface` and `BlitTexture` return the recorded result |
    | Render targets (2), `Unsupported*` (2), `VbMgrInit` (1) | Recorded result, no call |
    | State (10) | Device calls; `VerifyState` returns success when the model can draw the state |
    | Draws (4 + `IRPDraw`) | `draw` |
    | Vertex buffer manager (remaining 4), index buffers (9), vertex buffers (10) | Device calls; unlock and copy data written at the recorded offsets |
    | Gamma (5) | Device calls; image replay compares before the gamma blit |
    | `IRPTexture` (5) | `screenshot` is not replayed; loads read the file from the data tree through `neuron-core::vfs`; `GetImageInfo` and `LoadSurfaceData` from the blob |

    Every trace line maps; an unknown method or argument name fails the
    replay.
14. **Image comparison** (Tier C). The method is fixed now; the numbers
    are proposals that the owner approves before Gate G4 is judged, after
    the first image replays (as record 0003 did for envelopes):
    - Both images as 8-bit RGB, alpha ignored, same size.
    - A pixel's error is the largest channel difference against the
      reference pixel. A pixel *matches* when its error, or its error
      against one of the eight reference neighbours, is at most `t`; the
      neighbour rule absorbs edge coverage that differs between
      rasterisers.
    - The frame passes when at most `p` of its pixels do not match and the
      mean error over all pixels is at most `m`.
    - Starting numbers, approved on 2026-11-18 and confirmed after the
      first real replays: `t = 8`, `p = 0.5 %`, `m = 1.5`. A failing frame writes a
      difference image beside the test output.
15. **Synthetic traces in CI.** Hand-made traces in the version 1 format,
    each exercising one state group (blend, depth, alpha test, fog, the
    light types, the stage operations, coordinate generation, fans, the
    pre-transformed path, each texture format), live in
    `crates/neuron-client/tests/vectors/render/<name>/` with `trace.jsonl`,
    `blobs/` and `expected.bmp`. Model replay runs on every host. Image
    replay runs on the Windows CI job on the WARP adapter and compares with
    `expected.bmp`. The expected images are the C++ output: a replay mode
    of the console oracle (`Oracle.exe render-replay`) plays the trace
    into DX8RENDERER and writes the front buffer. Where it cannot run
    (owner answer 2) the owner's machine produces them; they
    are never blessed from Rust.

### 3. Shader toolchain (T1)

16. **Compiler:** `fxc.exe` with `d3dcompiler_47.dll` from Windows SDK
    10.0.26100.0 (`bin\10.0.26100.0\x64`), DXBC, shader model 5.1
    (`vs_5_1`, `ps_5_1`). The fixed set needs nothing from shader model 6;
    DXBC needs no validator or signing DLL and runs on every Direct3D 12
    driver at feature level 11_0 and on WARP; `fxc` is frozen, so a blob
    changes only when its source does. The DLL's file version is recorded
    in `crates/neuron-client/shaders/README.md`, and the xtask step refuses
    another.
17. **Flags:** `/nologo /WX /O3 /Ges /Gis /Zpr /Qstrip_debug
    /Qstrip_reflect /Qstrip_priv /T <profile> /E <entry> /Fo <blob>`. Strict
    mode and IEEE strictness keep the compiler from reassociating
    arithmetic; row-major packing matches the uploaded matrices; stripping
    debug, reflection and private data makes the blob depend only on the
    source and the compiler.
18. **Layout and check:** sources in `crates/neuron-client/shaders/`, a
    manifest `shaders.toml` (file, entry, profile, output), blobs in
    `crates/neuron-client/compiled_shaders/` embedded with `include_bytes!`.
    `cargo xtask shaders` compiles them on Windows; `--check` recompiles
    and fails when a committed blob differs. The Windows CI job runs the
    check. Building the crate needs no compiler on any host.

### 4. Unsafe code

19. All Direct3D 12, DXGI and window FFI lives under
    `crates/neuron-client/src/platform/`, the directory `xtask/src/scan.rs`
    already lists; `platform/d3d12/` holds the graphics part. Its types
    (`Gpu`, `Queue`, `Fence`, `SwapChain`, `CommandList`, `Heap`,
    `Resource`, `PipelineState`, `RootSignature`, `Readback`) wrap the COM
    interfaces with safe methods: sizes and offsets are checked, resource
    lifetime follows Rust ownership plus a fence value that keeps a
    resource alive until the GPU is done with it, and every `unsafe` block
    carries its `// SAFETY:` comment. `render::d3d12` builds the backend on
    those types in safe code; `render::ff`, `render::trace` and the image
    comparison are safe and platform-free. `scan.rs` needs no new entry.
    Later FFI (XAudio2, Raw Input, DirectWrite, Media Foundation) follows
    the same rule in its own subdirectory of `platform/`.

## Consequences

- Most of P4.3 is built and tested on Linux: the state model, defaults,
  vertex conversion, primitive conversion, the draw packet and model replay
  need no GPU. Only the backend, the shaders and image replay need Windows.
- Model replay of a real trace checks the model against every value the
  C++ handed back to its callers, bit for bit where the C++ only moves
  bits, before any image is compared.
- The proxy changes in three places and the owner records new traces;
  version 0 traces are not read.
- The oracle gains a `render-replay` mode, a C++ trace player over the
  seven interfaces, built like the other oracle modes.
- Expected divergences, to be entered when the code lands: light data is
  copied at `update_light`, not read from the caller at the next draw;
  `screenshot` writes BMP and PNG only (callers use BMP); states outside
  the honoured list are stored but not emulated; Direct3D 8 hardware
  limits (`MaxActiveLights`, stage count) become fixed numbers.
- Dependencies: `windows` (in T13) with the Direct3D 12, DXGI and
  foundation features, pinned in the workspace when P4.2 starts. No new
  crate for images: the BMP reader and writer are small and `png` is
  already allowed.
- The renderer is single-threaded like the C++; the device is not `Send`.

## Options rejected

- **A higher-level renderer API first.** Rejected by T2: every material
  and the client's 2D layer would be rewritten before anything could be
  compared.
- **Raw `u32` state ids and values in the API.** Keeps call sites close to
  the C++, but loses type checking at 650 sites; the numbers stay at the
  trace boundary only.
- **Shader permutations per feature combination.** More pipeline states
  and a permutation generator for no gain at about 3,000 draws per frame;
  branching on the constant block is enough. Revisit only if profiling
  shows a cost.
- **`dxc` and shader model 6.** Needs the validator and signing DLL or a
  recent runtime, and an external release to pin; no feature here needs it.
- **Recording resolved draws instead of interface calls.** Smaller traces,
  but the replay would skip the state model, which is where parity is lost.
- **A binary trace format.** JSONL at a few thousand lines per frame is
  small enough and readable in review.
- **A new unsafe directory `render/d3d12/`.** Spreads FFI over two trees;
  keeping all of it under `platform/` leaves one place to audit.

## Owner answers (2026-11-18)

1. The image metric of point 14 is accepted, with `t = 8`, `p = 0.5 %` and
   `m = 1.5` as starting numbers, confirmed after the first real replays
   and before Gate G4 is judged.
2. The synthetic expected images come from `Oracle.exe render-replay` on
   the Windows CI runner. If that runner cannot create a Direct3D 8 device,
   the owner produces them and they are committed as oracle output; CI then
   checks the Rust side only.
3. The shader supports sixteen enabled lights per draw. A draw with more
   uses the sixteen lowest indices and logs once.
4. `screenshot` writes BMP and PNG only; the other C++ formats are a
   divergence.
5. Real traces replay on the owner's GPU through hardware Direct3D 12; WARP
   is used for the synthetic traces in CI.
6. The shader emulates texture stages 0 and 1; the model stores all eight.
   The shader is extended if a version 1 trace shows a draw that enables
   stage 2.
