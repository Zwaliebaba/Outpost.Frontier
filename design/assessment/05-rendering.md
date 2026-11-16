# 05 — Rendering assessment (D3D8/D3D9 → D3D12)

Scope: `DX8RENDERER`, `DX9RENDERER`, `RendComp`, `Shading`, `ximage`, `surfacematerials`,
`SceneRuntime`, `Deformable2`, `particlefx`, `include/render/*`, `particlefx.h`, `ximage.h`,
`font.h`, `include/engine/sceneruntime.h`. Paths are relative to `CppSource/`. About 92k lines
(DX8 8.6k, DX9 10.8k, RendComp 9.9k, Shading 11.3k, ximage 1.1k, surfacematerials 5.9k,
SceneRuntime 10.3k, Deformable2 8.9k, particlefx 24.9k). The code is a reconstruction of
shipped binaries: layouts are pinned with `static_assert` and comments cite baseline addresses.
Counts marked *grep* are text matches (they include wrappers and comments).

## 1. The renderer seam

One object (`FDirectX8`, `DX8RENDERER/DX8RENDERER.h:330`) implements seven COM-like
interfaces declared in `include/render/IRenderPipeline.h`. Consumers obtain them by IID string
from the global `NeuronRuntime::Pipeline`. `FDirectX9` implements the same seven plus two
port-only "Ex" interfaces (`include/render/IRenderPipelineEx.h`).

| Interface | Methods | Purpose |
| --- | --- | --- |
| `IRenderPipeline` | 74 | device, frame, state, transforms, lights, textures, UP draws |
| `IVertexBufferManager` | 6 | per-FVF pooled dynamic VB: `LockVertexMemory`, `CopyVertexData` |
| `IRPVertexBuffer` | 10 | explicit VBs: create, resize, lock, copy, select |
| `IRPIndexBuffer` | 9 | explicit 16-bit IBs; handle `-1` is a 0x4000-index scratch IB |
| `IRPDraw` | 1 | `DrawIndexedPrimitive` on the selected VB/IB |
| `IGammaControl` | 5 | gamma function, ramp get/set, enable |
| `IRPTexture` | 5 | `PrintScreen`, `LoadTexture`, `LoadSurfaceData`, `GetImageInfo`, `LoadCubemap` |
| **Total** | **110** | |
| `IRenderPipelineEx`, `IRPDrawEx` (DX9 only) | 17 + 2 | HLSL programs, render targets, volume textures, scene depth |

`IRenderPipeline` by group:

| Group | n | Methods and notes |
| --- | --- | --- |
| Device and mode | 12 | `Startup`, `ShutdownPipeline`, `Set/GetPipelineState` (14 ids: `TEXTURE`, `LIGHTING`, `VIEWSPACE_LIGHTS`, `CLEAR_COLOR`…, `DX8RENDERER.cpp:105`), `GetDeviceInfo` (0x520-byte D3D8 adapter blob), `QueryDeviceAbility` (21 slots), mode count/enum/select, `CreateBuffers` (create or `Reset`), `GetModeBlob`, `DestroyGpuResources` |
| Frame | 8 | `ClearBuffers` (flags 1/2/4, optional rect), `SwapBuffers`, `BeginScene`, `EndScene`, `LockBuffer`/`UnlockBuffer` (back-buffer lock), `GetBufferInterface` and `GetDeviceStats` (stubs) |
| Viewport and window | 6 | viewport rect, depth range, `SetWindow` (present to another HWND with src/dest rects) |
| Transforms | 12 | world, view, modelview (12-float affine), projection (16-float), `SetLookat` (no caller found), `SetOrtho`, `SetPerspective`, `SetWorldN` (bone palette) |
| Lights | 8 | `D3DLIGHT8` by opaque handle; add/remove/update/enable |
| Material | 2 | `D3DMATERIAL8` (17 floats) |
| Textures | 10 | create, destroy, lock/unlock level, format, dimensions, `SetTextureLevelData` (source format + pitch + optional RGB palette), `BlitTexture` (no caller found) |
| Render targets | 2 | **stubs**: both return `0xFFFFFFF9` (`DX8RENDERER.cpp:4369-4388`) |
| State | 10 | render states (D3D8 numbers), texture-stage states, stage transform, stage texture, reset to defaults, `VerifyState` (`ValidateDevice`) |
| Draw | 4 | `DrawPrimitive` (UP), `DrawIndexedPrimitiveUp`, `DrawPrimitiveVb`, `DrawIndexedPrimitiveVb` (caller's index array is copied into the scratch IB per draw, `DX8RENDERER.cpp:4643-4688`) |

Consumer surface (*grep*): `SetRenderState` 464 sites (client 315), `SetTextureStageState` 194
(client 128), `SetMaterial` 57, `SetModelview` 27, `VerifyState` 32, `LockVertexMemory` 23,
`CopyVertexData` 23, UP draws 20 (all in the client). 68 files under `OutpostFrontier/Client`
name a renderer interface. The seam is wide and is used as a mutable state machine.

Above this seam sit further interfaces that are renderer-facing but API-neutral:
`IMaterial` (29), `IMaterialLibrary` (24), `ITextureLibrary2` (43), `IMaterialBatcher2` (17),
`IBatchedMaterial` (6), `IRenderer2` (20), `IRenderComponent` (15), `IMesh` (27),
`IParticleFx` (28), `IFLAppearance` (11), `IBigImage2` (9).

## 2. Fixed-function inventory

DX8RENDERER creates no vertex or pixel shader. `SetVertexShader` only receives an FVF
(`DX8RENDERER.cpp:1907-1965`). No call to `SetRenderTarget`, `SetClipPlane`, or any stencil
state exists in the renderer or in callers I read.

**Render states set by callers** (defaults table at `DX8RENDERER.cpp:141-217`):

| Area | States and values observed |
| --- | --- |
| Depth | `ZENABLE` 0/1; `ZWRITEENABLE` 0/1; `ZFUNC` 4 (LESSEQUAL) and 8 (ALWAYS) |
| Alpha test | `ALPHATESTENABLE`; `ALPHAFUNC` 5 (GREATER) and 2 (LESS); `ALPHAREF` 1, 4, 5, 0x7F, 0x80, 0xFE |
| Blend | `ALPHABLENDENABLE`; `SRCBLEND`/`DESTBLEND` from {ZERO, ONE, INVSRCCOLOR, SRCALPHA, INVSRCALPHA, DESTCOLOR} plus a data-driven pair per particle appearance (`BasicApp_BlendInfo`); `BLENDOP` ADD only |
| Raster | `CULLMODE` 1/2/3 (two-sided materials flip CW↔CCW for a second pass); `FILLMODE` (particle debug); `DITHERENABLE`; `COLORWRITEENABLE` 0xF; `CLIPPING` 1 |
| Lighting | `LIGHTING`, `COLORVERTEX`, `AMBIENT`, `DIFFUSEMATERIALSOURCE` 1, `LOCALVIEWER`, `NORMALIZENORMALS`; `SPECULARENABLE` is 0 in every table and literal site found |
| Fog | `FOGENABLE`, `FOGCOLOR`, `FOGSTART`, `FOGEND`, `FOGDENSITY`, `FOGTABLEMODE` and `FOGVERTEXMODE` (0, 3 = LINEAR, or data-driven), `RANGEFOGENABLE` 0 |
| Skinning | `VERTEXBLEND` 3 weights, `INDEXEDVERTEXBLENDENABLE`, `SOFTWAREVERTEXPROCESSING` (`Deformable2/Deformable2.cpp:6329-6336`) |

Not found at any call site: stencil, point sprites/point scale, clip planes, depth bias,
line pattern, texture factor, bump env. Clears use flags 3 or 2 (colour+Z, or Z in a rect).

**Texture stages.** At most two textured stages plus a terminating `DISABLE` stage in the
libraries I read (client not checked exhaustively). `VerifyState` hard-codes one or two
stages (`DX8RENDERER.cpp:4542`).
Almost all stage setup is literal {type, value} tables (`Shading/Material.cpp:15-79`,
`surfacematerials/MaterialImpl.cpp:33-76`, `surfacematerials/FLAppearance.cpp:53-55`).

| State | Values used |
| --- | --- |
| `COLOROP` | DISABLE, SELECTARG1, MODULATE, MODULATE2X, ADD |
| `ALPHAOP` | DISABLE, SELECTARG1, MODULATE |
| Args | TEXTURE, DIFFUSE, CURRENT only |
| `TEXCOORDINDEX` | 0, 1, `CAMERASPACEPOSITION` (projected shadow), `CAMERASPACEREFLECTIONVECTOR` (env map) |
| `TEXTURETRANSFORMFLAGS` | off, COUNT2 (UV animation, detail tiling), COUNT3\|PROJECTED (shadow) |
| Address U/V | stored value + 1 (WRAP, MIRROR, CLAMP); shadows force CLAMP |
| Filters | MAG/MIN POINT or LINEAR; MIP from a global option |

**FVFs seen:** `0x002`, `0x012`, `0x042`, `0x112`, `0x142` (particles, effects, GL-style
immediate layer), `0x152`, `0x212`, `0x252`, `0x1C4` (XYZRHW\|DIFFUSE\|SPECULAR\|TEX1, HUD and
screen quads), and a blend-weight position (`XYZB4`, 0x1C bytes) for palette-skinned VMeshes.
`CopyVertexData` converts between these on the CPU (`DX8RENDERER.cpp:5400-5449`).
**Primitive types:** all six, including point list and triangle fan
(`OutpostFrontier/Client/win3d_immediate.cpp:879-921`). Indices are always 16-bit.

**Lighting model:** `D3DLIGHT8` passes through unchanged, so directional, point and spot with
attenuation, range, theta/phi/falloff are all reachable
(`OutpostFrontier/Client/engine_light.cpp:239-295`). The light index pool starts at 64
(`DX8RENDERER.cpp:578`). I found no engine-side cap on simultaneously enabled lights.

**Textures.** Internal format ids: 1 P8, 2 R8G8B8, 3 R5G6B5, 4 X1R5G5B5, 5 A4R4G4B4,
6 A1R5G5B5, 7 A8R8G8B8, 8 X8R8G8B8, 10-14 DXT1-5, 15-21 "format classes" (`DAOP`, `DAOT`,
`DAA1`, `DAA4`, `DAA8`…) that resolve through a fallback catalog (`DX8RENDERER.cpp:422-426`,
`1630-1690`). P8 is refused by default (`TEXTURE_ALLOW_8BIT` 0), so D3DX expands palettized
uploads. All textures are `D3DPOOL_MANAGED`. Mips are never generated: they come from the file.
Cubemaps exist behind an ability flag.

**D3DX use (D3DX8 statically linked):** `D3DXLoadSurfaceFromMemory` (format conversion, palette,
TRIANGLE-filter resample on size mismatch), `D3DXLoadSurfaceFromSurface`,
`D3DXLoadSurfaceFromFileInMemory`, `D3DXCreateTextureFromFileInMemory`,
`D3DXCreateCubeTextureFromFileInMemory`, `D3DXGetImageInfoFromFileInMemory`,
`D3DXSaveSurfaceToFileA` (screenshot: BMP/JPG/TGA/PNG/DDS/PPM/DIB chosen by extension),
`D3DXGetErrorStringA`. Callers only reach `GetImageInfo`, `LoadCubemap` and `PrintScreen`.

## 3. DX9RENDERER

A line-for-line mirror of DX8 (same IIDs, same INI section), compiled only when
`OutpostFrontierDx9Renderer=1` (`Directory.Build.props`). Differences:

- The public interface still speaks D3D8 numbers. DX9 translates them: stage states that became
  sampler states, `SetVertexShader(FVF)` → `SetFVF` (`DX9RENDERER.cpp:2254-2256`),
  `EDGEANTIALIAS`, `ZBIAS` → `DEPTHBIAS`, `COPY_VSYNC` swap effect, image surfaces →
  `CreateOffscreenPlainSurface`. `Dx9RendererD3d8Types.h` re-declares D3D8 caps structs for
  `GetDeviceInfo` and the MGSDB quirk database.
- Ex extensions (`DX9RENDEREREx.cpp`, 1.2k lines): runtime `D3DXCompileShader` of
  `vs_3_0`/`ps_3_0` HLSL from `DATA_EXTRA\SHADERS` with hot reload, render targets (RGBA8, L8,
  FP16, R16F, R32F, G16R16F), one-mip volume textures, the scene depth as an INTZ texture,
  pass bracketing that saves and restores state, a fullscreen quad with the D3D9 half-pixel
  shift (`DX9RENDEREREx.cpp:1184`). The client uses it for a volumetric nebula and a depth
  debug view (`OutpostFrontier/Client/render_ex.cpp`, `nebula_volume.cpp`). No `.hlsl` file is
  in the repository.
- Still fixed function for everything else, still lost-device/`Reset` based, needs
  `d3dx9_43.dll`.

**As a reference:** DX8 is the behavioural baseline. DX9 is the better reference for three
things only: the D3D8-number translation table, the pixel-centre handling, and the Ex API,
which is already a D3D-version-neutral sketch of what a programmable pass needs.

**DirectDraw:** both renderers use DirectDraw 7 only for `GetAvailableVidMem`, rounded to an
8/32/64 MB grain and exposed as ability slot 3 (`DX8RENDERER.cpp:1205-1247`,
`DX9RENDERER.cpp:1463`). Gamma goes through the D3D device (`SetGammaRamp`), not DirectDraw.

## 4. RendComp

- `RenderManager.cpp`: `IRenderer2`. Recursive walk of the engine instance tree, sphere
  classification against the camera, LOD pick, child recursion (`RenderManager.cpp:722-876`).
  LOD data: node `MultiLevel` holds `Switch2` (float array of switch distances) and
  `Level0..N`; each level is claimed by a plugin (`RenderManager.cpp:412-510`).
- Plugins (`Plugins.cpp`, `TriMesh.cpp`): `TriMesh` (legacy multi-stream mesh: `Points`,
  `Vertex_normals`, `UV0`, `UV1`, `Color0`, per-stream index arrays, `Face_groups/Group%d`,
  `Tristrip_indices`, UV animation), `SphereMesh` (tessellated planets with `M%d` materials and
  an atmosphere shell), `VMesh`.
- `VMeshLibrary.cpp`/`VMeshPart.cpp`: the main static-mesh path. One VB and one IB per
  `VMeshData` blob; parts reference ranges. `VMeshWire.cpp`: line-list wireframes (`VWireData`).
- No sorting here. Draws go to `IMaterial::Submit` or to the material batcher.

## 5. Shading

Material = `Bind` → `Submit` → `Teardown`, with a 0x4C-byte property schema loaded from the
`Material library` node (`Type`, `Dc`, `Dt_name`, `Dt_flags`, `Ec`, `Et_name`, `Oc`, `Dm_name`,
`Bt_name`…). 22 factory names map to 9 bind kinds and 11 submit kinds
(`Shading/Material.cpp:3728-3749`):

| Factory names | Fixed-function recipe |
| --- | --- |
| `DcDt`, `DcDtEc`, `DcDtOcOt`, `DcDtEcOcOt` | lit; stage 0 MODULATE(tex, diffuse); blend SRCALPHA/INVSRCALPHA when `Oc`; optional UV-animation matrix on stage 0; alpha test GREATER 0x80 when flagged |
| `…Two` variants (4) | same, then a second pass with flipped cull and negated normals |
| `DcDtEt` | stage 1 ADD(emissive tex, current) |
| `EcEt`, `EcEtTwo` | lighting on, MODULATE(tex, diffuse), additive blend ONE/ONE |
| `Nebula`, `NebulaTwo` | unlit, SRCALPHA/ONE |
| `AtmosphereMaterial` | lit, SRCALPHA/INVSRCALPHA, CPU-built vertices with a fade texture |
| `LightMap` | SELECTARG1 texture, blend DESTCOLOR/ZERO, ZFUNC LESSEQUAL |
| `DetailMapMaterial`, `BtDetailMapMaterial`, `BtDetailMapTwoMaterial` | stage 1 MODULATE2X detail with a tiling texture transform |
| `DetailMap2Dm1Msk2PassMaterial`, `IllumDetailMapMaterial`, `Masked2DetailMapMaterial` | one or two passes depending on `VerifyState`; second pass alpha-tested and blended |
| `NullMaterial` | draws nothing |

- `TextureLibrary.cpp`: per texture node tries `MIPS` (DDS, DXT1-5 or uncompressed, mip chain
  walked by hand, `TextureLibrary.cpp:1162-1252`), then `MIP0..MIP9` (one uncompressed TGA per
  level: 8-bit palettized, 15/16, 24, 32, greyscale; RLE not decoded), then `CUBE`, then the
  legacy animated form (`Texture count`, `FPS`, `Frame count`, `Frame rects` = 20-byte
  {child, 4 floats}). `TEXTURE_LOD_LOAD` skips top mips.
- `MaterialAnimation.cpp`: `MaterialAnim` nodes (`MAFlags`, `MACount`, `MAKeys`, `MADeltas`)
  produce a 4×4 UV matrix per instance.
- `MaterialBatcher.cpp`: two bins. Records are keyed by `IMaterial*` and carry copies of
  world/view/combined matrices. Sort kinds 2/3 sort records by view depth; 4/5 also sort
  triangles inside each record and rewrite indices (`MaterialBatcher.cpp:573-668`). Kinds 6/7
  are unimplemented in the original. Regex rules on material names choose the bin.

## 6. surfacematerials, ximage, fonts

- `surfacematerials/MaterialImpl.cpp`: 19 names, 15 kinds: Glass (three names), Veilborn (two),
  Backdrop/Starsphere, HUDIcon, HUDAnim, Colorize, Cloak, Wire, Wire2, ExclusionZone, EnvMap
  (reflection texgen), Shadow (projected `efxshadow` texture), EngineGlow, VisibleCounter,
  PlanetWater. Same table-driven recipe as Shading; several wrap a "candidate" inner material.
- `FLAppearance.cpp`: per-instance values, backdrop and starsphere draws with their own
  perspective, fading and fixed blob shadows (FVF 0x142 quads through `LockVertexMemory`).
- `ximage` is not a decoder. `BigImage` tiles a large CPU image into several power-of-two
  textures and draws through a caller callback. Image decoding lives in Shading (TGA, DDS mip
  walk) and D3DX.
- Fonts (`include/font.h`): GDI. Whole strings are drawn with `ExtTextOutW` into a memory DC and
  copied into A1R5G5B5 atlas pages through `LockTexture`
  (`OutpostFrontier/Client/font_atlas.cpp:92-105`, `434-490`).

## 7. SceneRuntime

No renderer call anywhere in it: the whole library is render-independent.

- Engine: archetypes and instances in a parent/child tree, joints, extents, user data
  (`IEngine`, 55 methods). Compound models: node `Cmpnd` with `Root` and `Part_*` children
  (`File name`, `Object name`) and `Cons` constraint files `Fix`, `Rev`, `Pris`, `Sphere`,
  `Loose`, `Cyl`, `Trans` with record sizes 0xB0/0xD0/0xD0/0xD4/0xB0/0xD8/0xB0
  (`SceneRuntime.cpp:5136-5147`, `5203-5236`).
- Hardpoints: `Hardpoints/{Fixed,Revolute,Prismatic}` with `Position`, `Orientation`, `Axis`,
  `Min`, `Max`.
- Animation: `Animation/Script/<name>` with `Object map`/`Joint map` entries (`Parent name`,
  `Child name`, `Channel`). A channel is `Header` (12 bytes: u32 frame count, f32 interval
  where negative means per-frame time stamps, u32 flags) and `Frames`. Frame stride from flags:
  1 → f32, 2 → vec3, 4 → quat, 0x40/0x80 → 6-byte compressed, 0x100 → 8 bytes
  (`SceneRuntime.cpp:1395-1412`, `4035-4100`). A mixer tree (leaf, blend, overlay) evaluates them.
- Mesh container layouts consumed by RendComp (`include/render/rendcomp.h:140-199`, packed):
  `VMeshData` header 0x10 {u8 flags, 3 unread, u32 primitive type, u16 facegroups, u16 indices,
  u16 FVF, u16 vertices}, then facegroups 0xC {u32 material CRC, u16 vertex begin, u16 vertex
  end, u16 index count, u16 unread}, then u16 indices, then vertices; `VMeshRef` 0x3C;
  `VWireData` header 0x10. The outer container is the `UTF ` tree (magic `0x20465455`, version
  0x101, 0x38-byte header) owned by ReadFile.

Split: all of SceneRuntime, plus the pure parsers for VMesh/TriMesh/DFM/ALE data, can live in
neuron-core. GPU buffers, material binding, and draw submission stay in neuron-client.

## 8. Deformable2 and particlefx

**Deformable2.** Skeleton up to 150 bones (`Deformable2.h:31`). DFM mesh nodes: `Geometry`
(`Points`, `Vertex_normals`, `UV0`, index streams, `Point_bone_count`, `Bone_id_chain`,
`Bone_weight_chain`), `Face_groups` (`Material_name`, `Tristrip_indices`), `MultiLevel`
(`Fractions`), UV-deform data for eyes. Two draw paths:

- DFM archetype: **CPU skinning**, incremental per dirty bone, into a VB of FVF 0x112, drawn
  through materials (`Deformable2.cpp:777`, `939`).
- VMesh wrapper: **fixed-function indexed palette skinning**. Bones go to
  `D3DTS_WORLDMATRIX(n)` through `SetWorldN`, with software vertex processing forced on
  (`Deformable2.cpp:6298-6338`).

`EyeMaterial` moves eye textures with a stage texture transform. Aim/IK solving is CPU.

**particlefx.** A node graph loaded from `ALEffectLib` and `ParticleFxNodeLibrary` chunks
(float version 1.0 or 1.1, typed property sets, ramps and animated transforms). Emitters: cone,
cube, sphere. Fields: air, collide, gravity, radial, turbulence, plus game dust and beam.
Appearances: basic, particle, rect, perp, oriented, mesh, dust, beam. Simulation and
**billboarding are entirely CPU**. Vertices (FVF 0x142, 0x18 bytes) are written into one
dynamic VB of `G_MaxVertices` (default 4096) between `BeginDraw`/`EndDraw`, indices into the
scratch IB, then `IRPDraw::DrawIndexedPrimitive`, optionally through the material batcher
(`particlefx_common.cpp:191`, `1563-1620`). 134 x87-related comments mark float-sequence
sensitivity.

## 9. Frame structure (as visible here)

Verified: `BeginScene` → dither and mip-filter globals → scene → `MaterialBatcher::FlushAll` →
`EndScene` → reset a fixed set of states → `SwapBuffers`
(`OutpostFrontier/Client/client_frame_tick.cpp:95-178`). Pieces visible from these libraries:
starsphere layers and backdrop with their own projection; geometry drawn immediately or queued
in batcher bin 1 and depth-sorted; particles between `BeginDraw`/`EndDraw`; beam and
screen-quad effects with Z write off; projected blob shadows; HUD/2D. Their exact order inside
the scene is the client assessor's to confirm (my ordering is a guess). The first-person camera
partitions the depth range (`SetDepthRange(0, 0.75)` and `(0.75, 1)`,
`space_camera_first.cpp:573-576`). The ship preview clears Z in a rectangle. 2D and text use
XYZRHW quads (FVF 0x1C4) and a GL-1.x-style immediate layer over `DrawPrimitiveUP`. The
baseline has no render-to-texture and no post-processing. The only offscreen passes are the
DX9 Ex nebula. Sun flares and baseline nebulae are presumably plain blended geometry (guess).

## 10. Hazards

| Hazard | Evidence | Consequence |
| --- | --- | --- |
| Lost-device protocol in callers | `SwapBuffers` re-creates on `DEVICELOST` (`DX8RENDERER.cpp:3190-3200`); the client treats a failed `BeginScene` as "sleep 1 s and `CreateBuffers`", and gates on `SwapBuffers() == 0` | Map to resize and device-removed; keep the return contract |
| `VerifyState` drives material logic | 32 call sites; materials pick 1 or 2 passes from `ValidateDevice` | Emulate deterministically (always "two stages OK") |
| CPU-visible locks | `LockTexture` (font atlas), `LockBuffer` (back buffer), `LockVb`/`LockIb` with DISCARD/NOOVERWRITE (`DX8RENDERER.cpp:5902-5975`) | Shadow copies and an upload ring |
| Per-draw state churn | Get/Set save-restore around most draws; every draw flushes a dirty-state cache | PSO lookup must be a cheap hash on shadow state |
| Immediate-mode volume | All HUD, text, effects and the GL-style layer use UP draws; `DrawIndexedPrimitiveVb` copies indices per draw | Transient geometry allocator is on the hot path |
| Light handle aliasing | `AddLight(Handle)` keeps the caller's `D3DLIGHT8*` and reads it at the next draw (`DX8RENDERER.cpp:4690`) | Needs explicit ownership in Rust |
| Handedness | Engine is right-handed with 12-float affine transforms; the renderer multiplies `diag(1,1,-1)` into view, or into world when `VIEWSPACE_LIGHTS` is on, and transposes (`DX8RENDERER.cpp:2256-2275`, `3621-3660`). The client layer is named after OpenGL | Keep the flip in one place; cull sense depends on it |
| Projection | D3D left-handed, depth 0..1 (matches D3D12). `SetPerspective` uses `1/tan(fov)` with no halving, so its argument is a half-angle (`DX8RENDERER.cpp:3845-3858`) | Port the exact formula |
| Pixel centres | D3D8/9 put pixel centres on integers; D3D12 on half-integers. XYZRHW HUD quads and all viewport mapping shift by half a pixel | Add the offset in the vertex shader, or text and UI blur |
| Gamma | No sRGB anywhere; blending is in gamma space; gamma is a hardware ramp | Use UNORM formats; gamma needs a new mechanism |
| x87 | Built `/arch:IA32`; `CreateDevice` without `FPU_PRESERVE` leaves the main thread at 24-bit precision (`Directory.Build.props`), so `double` temporaries in the source were presumably single-rounded at run time (inference). The client toggles `_control87` around the frame. 13 `__asm` sites in scope, 10 of them in `TextureLibrary.cpp` | Bit-exact parity with baseline images is unrealistic; f32 is closer than f64 |
| Globals | Single-threaded. File-level singletons (light records, math engine, VB handles, schema tables); no locks in scope | Rust needs an explicit renderer context |
| `SetWindow` present override | Present to a second HWND with src/dest rects | Flip-model swapchains cannot do this directly |
| Hardware quirk code | MGSDB database, `BROKEN_*` flags, square-only textures, format fallback catalog | Delete, but callers query the abilities |

## 11. Tests and verification

None exist for these libraries. The only checks are layout `static_assert`s. Hooks that help:
`PrintScreen`, particle `CopyCounters`, `VisibleCounterMaterial`.

Ideas:

1. Parser unit tests with fixture blobs: VMeshData/VMeshRef, TriMesh streams, DFM, channel
   header and frames, ALE, DDS mip cursor, TGA decode, material and texture library nodes.
2. Pure state-mapping tests: feed the literal RS/TSS tables from `Material.cpp` and
   `MaterialImpl.cpp` to the "fixed-function state → PSO key + constants" function.
3. Golden images on WARP (deterministic, runs in CI): one scene per material kind, per particle
   appearance, light types, fog ramp, skinned pose, and a pixel-alignment grid for XYZRHW text.
   Compare with tolerance.
4. Call-trace replay: the seam is 110 IID-addressed methods, so a logging proxy component in the
   C++ build can record a frame's calls and buffers. Replaying that trace into the Rust renderer
   gives reference comparisons before any game code is ported. Reference images can come from
   the C++ build's `PrintScreen`.

## 12. D3D12 design inputs

- **Shader permutations.** The space is small. Vertex side: about ten input layouts from FVF;
  lighting on/off with up to N lights of three types; colour-vertex; texgen per stage
  (passthrough, camera-space position, reflection); texture transform (none, 2D, projected);
  palette skinning; pre-transformed XYZRHW; linear vertex fog. Pixel side: two stages with five
  colour ops and three alpha ops over three argument sources; alpha test GREATER or LESS with a
  reference; fog blend. This fits one vertex and one pixel "uber" shader driven by a per-draw
  constant buffer, or a few dozen specialised variants. PSO key = {input layout, topology type,
  blend pair, depth enable/write/func, cull, fill, colour write, target formats}, created lazily
  and cached. Alpha test and fog move into the pixel shader.
- **Primitive conversion.** Triangle fans must become lists. Point lists and line strips exist
  natively. 16-bit indices only.
- **Resources.** Static VB/IB/textures in default heaps with upload copies. A persistently
  mapped per-frame upload ring for UP draws, `LockVertexMemory`, dynamic VBs and the scratch IB.
  CPU shadow for lockable textures. Descriptor needs are tiny (two SRVs per draw, a handful of
  static samplers covering wrap/mirror/clamp × point/linear × mip modes); no bindless required.
- **Format conversion.** P8 + palette, R8G8B8, X1R5G5B5, A1R5G5B5, A4R4G4B4, R5G6B5 → expand to
  BGRA8 on upload (16-bit DXGI formats are optional). DXT1-5 → BC1/BC2/BC3. A box/triangle
  resampler replaces D3DX for size mismatches. DDS cubemap and TGA readers in Rust. Screenshot
  writer for at least BMP/PNG/TGA.
- **Presentation.** Flip-model swapchain, BGRA8 UNORM, sync interval 0-4 from the mode blob,
  tearing for interval 0, borderless instead of exclusive fullscreen. Rendering the scene to an
  intermediate target and blitting gives a place for the gamma curve, back-buffer lock,
  screenshots, the `SetWindow` rectangle present, and the depth texture the nebula needs. HDR is
  not needed by anything here.
- **Shader toolchain.** A fixed shader set means build-time compilation and embedded bytecode;
  no compiler DLL at run time. SM 5.1 via FXC is sufficient; DXC only if SM 6 is wanted. The DX9
  nebula hot-reload is a developer feature and can be optional.
- **Feature level.** 11_0 is enough for everything observed.

**Raw D3D12 (`windows` crate) vs wgpu on DX12.**

| | `windows` crate | wgpu (DX12 backend) |
| --- | --- | --- |
| Matches the owner's explicit request and D3D12 experience | yes | indirectly |
| Control of PSOs, heaps, swapchain, presentation rects, readback | full | abstracted; WebGPU pass model |
| Safety | unsafe COM calls behind our own safe layer | safe API, built-in validation |
| Shaders | HLSL, PIX-friendly | WGSL/naga (HLSL only by translation) |
| 16-bit packed formats, DXGI gamma, tearing details | available | partly unavailable |
| Dependency weight and churn | small | large, fast-moving API |
| Portability | Windows only (acceptable: target is `x86_64-pc-windows-msvc`) | cross-platform (not required) |

Recommendation: raw D3D12 through `windows`, wrapped in a small internal module. The feature
set is small and fixed, and wgpu's abstraction solves problems this project does not have.

**Where the Rust seam goes.** Re-implement the fixed-function-style device first, as a typed
Rust API (enums instead of D3D numbers, generational handles instead of pointers, an explicit
context instead of globals) with the same state-machine semantics. Reasons: about 650 state-set
sites and 22 + 19 materials are written against it; the recipes are already data tables; trace
replay and golden tests work at this level. Once parity is shown, material recipes can be turned
into declarative pass descriptions and the save/restore idiom removed, one library at a time.
Designing a higher-level API first would force a rewrite of every material and of the client's
2D layer before anything can be compared with the baseline.

## 13. Rust mapping

**neuron-core** (render-independent): `scene::{archetype, instance, joint, hardpoint}`,
`anim::{script, channel, mixer}`, `mesh_data::{vmesh, trimesh, sphere, dfm, fvf}`,
`image::{tga, dds, pixel_format}`, `fx_data` (ALE and node-library parsing).

**neuron-client**:

```
render/
  d3d12/      device, swapchain, heaps, upload_ring, pso_cache, readback
  ff/         state, render_state, texture_stage, transforms, lights, vertex_format, draw,
              shaders/ (HLSL, compiled at build time)
  texture/    library, formats, animation, big_image
  material/   library, batcher, animation, kinds/ (shading), surface/ (surfacematerials)
  mesh/       render_manager, lod, vmesh, trimesh, sphere, wire
  appearance/ backdrop, starsphere, shadows
  particles/  nodes, emitters, fields, appearances, draw_context
  deformable/ skeleton, skin_cpu, skin_palette, eye_material
  gamma, screenshot
```

| Order | Piece | Effort | Notes |
| --- | --- | --- | --- |
| 1 | Data parsers and SceneRuntime in core | L | 10k lines, pointer-heavy graph, no GPU |
| 2 | `d3d12` backend | L | swapchain, upload ring, PSO cache |
| 3 | `ff` emulation and shaders | L-XL | the parity-critical piece; trace replay target |
| 4 | Texture library and image formats | M | |
| 5 | Shading materials, animation, batcher | L | |
| 6 | RendComp | M-L | |
| 7 | surfacematerials and FLAppearance | M-L | |
| 8 | Deformable2 and EyeMaterial | L | |
| 9 | particlefx | XL | largest and most float-sensitive |
| 10 | BigImage, gamma, screenshot | S | |
| 11 | Nebula Ex equivalent | M | optional |
| — | DX8/DX9 renderers, MGSDB, DirectDraw query | not ported | replaced |

Top risks: (1) fixed-function emulation fidelity (lighting, fog, texgen, half-pixel) without a
bit-exact oracle; (2) the width of the state-machine seam inside the client (another
assessor's scope) dictating the Rust API shape; (3) particlefx size and float sensitivity;
(4) presentation and gamma behaviours with no flip-model equivalent; (5) baseline assets are
not in the repository, so format parsers cannot be validated here.

## Open questions for the owner

1. Parity target: tolerance-based golden images against the C++ D3D8/D3D9 build, or "looks
   right"? Is a running C++ build with assets available to produce references and call traces?
2. Is the DX9 volumetric nebula in scope for the D3D12 renderer, and are its HLSL sources
   available (`DATA_EXTRA\SHADERS` is not in the repository)?
3. Is a scene render target plus final blit acceptable (enables gamma, screenshots, depth
   sampling), or must the scene render straight to the swapchain?
4. Keep the gamma/brightness option? If so, a shader curve instead of a hardware ramp?
5. Exclusive fullscreen and display-mode switching: required, or borderless only?
6. Maximum simultaneous lights the D3D12 path must support (no engine cap found; D3D8 hardware
   typically allowed 8)?
7. Runtime shader compilation and hot reload: wanted for development, or build-time only?
8. Should hardware-quirk INI keys (`BROKEN_*`, `TEXTURE_ALLOW_*`, MGSDB) be dropped outright?
9. Minimum GPU/OS: is feature level 11_0 on Windows 10+ the floor?
10. Fonts: keep GDI rasterisation into an atlas, or is DirectWrite acceptable?
