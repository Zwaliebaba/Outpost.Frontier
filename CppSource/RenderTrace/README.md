# RenderTrace: renderer logging proxy

Oracle instrumentation for work package P0.5 ([migration plan](../../design/migration-plan.md)):
a component that sits between the game and the Direct3D 8 renderer and
records every call made to it, so that a frame can be replayed into the Rust
renderer at Gate G4. The trace format is provisional (version 0); P4.0
fixes the final one. RenderTrace changes nothing the renderer does: each
method forwards its arguments unchanged to `DX8RENDERER.dll` and returns its
result.

| File | What it is |
| --- | --- |
| `RenderTrace.cpp` | DllMain and factory, aggregation, and the 29 methods with special recording |
| `RenderTraceCore.h`, `.cpp` | The trace session: JSONL writer, blob store, handle ids, pending locks, state slots |
| `make_render_trace.py` | Generates the two files below from `include/render/IRenderPipeline.h` |
| `RenderTraceGenerated.h`, `.cpp` | Generated: the proxy class and the 81 methods that only forward and record |

## Build

The project is `Tools\RenderTrace` in `OutpostFrontier.sln` and builds
`bin\RenderTrace.dll` with the rest of the solution. It links only
`neuroncomponents.lib`; the Direct3D 8 and D3DX 8 headers are used for types.

After a change to `IRenderPipeline.h`, run the generator and commit both
outputs. It fails when a method has no entry in its table or when the
argument names differ from the header, and `--check` fails when the
committed outputs are out of date:

```powershell
python CppSource\RenderTrace\make_render_trace.py
python CppSource\RenderTrace\make_render_trace.py --check
```

## Activation

Two changes to the engine INI (`NeuronComponents.ini` in the run folder):

```ini
[Libraries]
DX8RENDERER.dll        ; stays: RenderTrace creates its DirectX8 component
RenderTrace.dll

[System]
RenderTrace            ; in place of the DirectX8 line
```

The name must be written exactly `RenderTrace`. Text after `=` on the
`[System]` line is passed on to the renderer as its section name, as for
`DirectX8`. With these two changes alone the proxy only forwards. Tracing
starts when both an output folder and a frame list are set, in an INI
section or in environment variables (which win):

```ini
[RenderTrace]
output = C:\traces\menu
frames = 300-302, 1200
```

| Key | Environment variable | Value |
| --- | --- | --- |
| `output` | `OUTPOST_RENDER_TRACE_DIR` | Output folder, created if missing; relative to the working directory |
| `frames` | `OUTPOST_RENDER_TRACE_FRAMES` | Frames to record: `N`, `N-M`, `N-` (open), `all` or `*`, separated by commas or spaces |

Frame 0 is everything before the first `SwapBuffers`; frame N starts when
the Nth `SwapBuffers` returns. The trace closes after the `SwapBuffers` of
the last selected frame (never, for an open range) and the proxy then only
forwards. The engine log (`FLSpew.txt`) says which file was opened or why
tracing stayed off.

## Output

```text
<output>\trace.jsonl          one JSON object per line (trace-2.jsonl ... for a second renderer in the process)
<output>\blobs\<hash>.bin     raw bytes, named by the FNV-1a 64 hash of their content (16 hex digits)
```

A blob is written once; a later line with the same bytes refers to the same
file. Traces made from the original game data are game data: keep them out
of the repository (AGENTS.md, test data).

## Trace format (version 0)

Every line is one object. The first line is the header:

```json
{"format":"outpost-render-trace","version":0,"renderer":"DirectX8","section":null,"frames":"300-302, 1200"}
```

A call:

```json
{"seq":5120,"frame":300,"interface":"IRenderPipeline","method":"SetRenderState","args":{"State":7,"Value":1},"ret":0}
```

| Field | Meaning |
| --- | --- |
| `seq` | Order of the call among the recorded calls; increases by one per recorded call |
| `frame` | Frame number the call was made in |
| `interface`, `method` | The C++ interface (`IRenderPipeline`, `IVertexBufferManager`, `IRPDraw`, `IRPIndexBuffer`, `IRPVertexBuffer`, `IGammaControl`, `IRPTexture`, `IAggregateComponent`) and method name |
| `args` | Arguments by their names in `IRenderPipeline.h`, in declaration order, with any out-values written after the call |
| `ret` | The GENRESULT, signed decimal |
| `first_seen` | Ids this line used for the first time without a creating call (renderer-owned buffers, light keys) |
| `note` | Why some data was not recorded |
| `synthetic` | Present (true) on lines the proxy wrote for a side effect the renderer had internally |

Frame markers: `{"frame":N,"begin_frame":true,"seq":S,"snapshot_lines":K}`
opens a selected frame and is followed by K snapshot lines (below);
`{"frame":N,"end_frame":true,"seq":S}` closes it after its `SwapBuffers`
line. The last line is `{"end":true,"frame":N,"seq":S}`.

Values:

| C++ value | JSON |
| --- | --- |
| integer, enum, flags | number |
| `float` | its 32 bits as 8 lowercase hex digits, `"3f800000"` (as in [verification.md](../../design/verification.md)) |
| matrix (12 or 16 floats), `D3DLIGHT8` (26 words), `D3DMATERIAL8` (17 words), mode records (10 words), `D3DRECT` (4), `D3DXIMAGE_INFO` (7) | array of 32-bit words as 8 hex digits, in memory order |
| byte data | `{"size":N,"hex":"..."}` up to 64 bytes, `{"size":N,"blob":"<hash>"}` above |
| string | JSON string; bytes outside printable ASCII as `\u00XX` |
| null pointer | `null` |
| unused or opaque pointer, `HWND`, `IFileSystem*` | its address as a number (not stable between runs) |
| texture, vertex buffer, index buffer, light handle | id (below) |

Handles are written as ids: `0` for a null handle, `-1` for the renderer's
`(void*)-1` (the scratch index buffer, or the current or pooled vertex
buffer), a positive integer otherwise. Ids come from one counter, are
assigned when a creating call returns (`CreateTexture`, `LoadTexture`,
`LoadCubemap`, `CreateVb`, `CreateIb`) or on first sight, and are never
reused: a destroyed handle's id is retired, and a later object at the same
address gets a new one. The renderer's pooled vertex buffers (one per FVF,
from `CreateVb` with flag 1 and `LockVertexMemory`) keep one id for the
whole trace. Lights are keyed by the caller's pointer; `GetLight` passes its
argument to the device as an index and records it as a number.

### Methods with special recording

| Method | Recorded |
| --- | --- |
| `LockTexture` / `UnlockTexture` | Lock: the level's width, height, pitch and pfenum format. Unlock: those again and `data`, the level's bytes read before the unlock |
| `LockVb` / `UnlockVb` | Lock: `OffsetIn` (a byte offset for static buffers), `Offset` (the first vertex out), `Count`, `Stride`. Unlock: `FirstVertex`, `Count`, `Stride`, `data` |
| `LockIb` / `UnlockIb` | Lock: `OffsetIn`, `Offset` (first index out), `Count`. Unlock: `FirstIndex`, `Count`, `data` (16-bit indices) |
| `LockVertexMemory` / `UnlockVertexMemory` | Lock: `Out` = the pooled buffer's id, its FVF and `BaseVertex`. Unlock: `FirstVertex`, `Count`, `Stride`, `data` |
| `LockBuffer` | The back-buffer copy's size, pitch, format and `data` (a screenshot of the frame so far) |
| `CopyVertices` | `OffsetIn`, `OutOffset` (where the vertices landed), `DstFvf` and `SrcData`, the source descriptor resolved (below) |
| `CopyVertexData` | `Dest` as the locked buffer and byte offset it points into, and `Source` resolved. Selected frames only: the unlock records the result |
| `CopyIndices` | `OffsetIn`, `OutOffset`, `SrcData` (Count 16-bit indices) |
| `SetTextureLevelData` | `Pixels`: what D3DX reads for SrcW x SrcH at Pitch in `SrcFormat` (`(rows - 1) * Pitch + one row`, rows of 4x4 blocks for DXT); `B`: the 768-byte RGB palette |
| `LoadSurfaceData`, `GetImageInfo` | `SrcData`: the encoded image in memory (SrcSize bytes) |
| `LoadTexture`, `LoadCubemap` | `FileName`; the file is read by the renderer through the file system and is not copied |
| `DrawPrimitive`, `DrawIndexedPrimitiveUp`, `DrawIndexedPrimitiveVb` | `Verts` / `Indices`: the vertices and 16-bit indices Direct3D reads, sized from the FVF (DX8RENDERER's stride) and the primitive count |
| `SetLight`, `AddLight`, `UpdateLight` | `Light`: the `D3DLIGHT8` (for AddLight and UpdateLight the handle points at it; the renderer reads it at the next draw, the trace holds it at the call) |
| `CreateTexture`, `CreateVb`, `CreateIb` | `Format` (the pfenum the caller asked for), the new handle's id |
| `DestroyVb`, `DestroyIb` | The id of the handle the argument pointed to |

A resolved vertex source (`FRpVertexSrcDesc`) is
`{"Flags":..,"SrcFvf":..,"SrcCount":..,<streams>}`. When the destination FVF
equals the source's and the source is interleaved (`Flags & 6 == 2`), the
renderer copies whole vertices and the object has `"Interleaved":{"Stride",
"VertexBytes","first","data"}`, Count vertices from vertex `first`.
Otherwise it has one object per stream the source FVF uses (`Position`,
`Normal`, `Diffuse`, `Tex0`, `Tex1`) with the descriptor's `ByteSize`,
`Stride`, `Count`, the `Element` bytes read per vertex, `first` and `last`
(the element indices the region covers) and `data`, the bytes from element
`first` to the end of element `last`; byte k of `data` is byte
`first * Stride + k` of the stream. An indexed source (`Flags & 4`) also
has `index`, the stream's Count DWORD indices from StartVertex.

## Recorded frames

Calls fall in three groups:

- Always written, from the start of the trace to its end: lifecycle and
  configuration (`Initialize`, `Startup`, `ShutdownPipeline`, `SelectMode`,
  `CreateBuffers`, `DestroyGpuResources`, `SetPipelineState`, `SetWindow`,
  the gamma setters, `VbMgrInit`, `VbMgrReleaseAll`, `SwapBuffers`), and every
  resource call: creation, destruction, resizing, locks and unlocks with
  their data, `CopyVertices`, `CopyIndices`, texture uploads and loads,
  `GetImageInfo`, `BlitTexture`. The generator's table
  (`make_render_trace.py`, `SPEC`) gives each method's group.
- State setters (render and texture-stage states, stage textures and
  transforms, the transforms, lights, material, viewport, depth range,
  render target, `ResetRenderStatesToDefaults`): written in selected frames,
  and in every frame kept as state slots, the last call per slot.
- Everything else (draws, scenes, clears, getters): selected frames only.

A selected frame starts with a snapshot: the kept state slots, each written
once with `"snapshot":N` before its original fields, in the order of their
last update, and then the vertex and index buffer selection
(`SelectVb`, `SelectIb`, synthetic), which draws and buffer calls change as a
side effect. The proxy models the renderer's side effects on its state:
`DestroyTexture` and `SetTextureLevelData` unbind the texture from its
stages (a synthetic `SetTextureStageTexture` with handle 0 replaces the
slot), `CreateBuffers` resets every slot but the lights, `RemoveLight` and
`DestroyLight` drop the light's slots, and the selection follows `SelectVb`,
`SelectIb`, the draws, `LockVertexMemory`, pooled `CreateVb`,
`CopyVertices(-1)`, `ResizeVb`, the destroy calls, `DestroyGpuResources`,
`VbMgrReleaseAll` and changes of `D3DRS_SOFTWAREVERTEXPROCESSING`.

## Replay contract

To replay frame N into a renderer that implements the same seven
interfaces:

1. Replay every line before frame N's `begin_frame` that is not a snapshot
   line, in `seq` order. These are the always-written calls: they create
   every resource frame N uses and put its data in place. State setters and
   draws of earlier selected frames may be replayed or skipped.
2. Replay frame N's snapshot lines in file order, then its call lines up to
   `end_frame`.

Rules for the replayer:

- Map each id to the replayer's own object; resolve `-1` the way the
  renderer does (scratch IB, current VB, the pooled VB of the source FVF).
  A snapshot `SelectVb` may name `{"pool_fvf":F}`: the pooled buffer for
  FVF F, never returned to the caller before.
- Buffer data goes where the C++ renderer put it: write unlock data at the
  recorded `FirstVertex` / `FirstIndex` and `CopyVertices` / `CopyIndices`
  output at the recorded `OutOffset`, whatever offsets the replayer's own
  ring cursors would give. Draws in skipped frames advanced the C++ cursors
  (`DrawIndexedPrimitiveVb` locks the scratch IB itself), so the cursors
  cannot be reproduced; the recorded offsets are authoritative.
- Texture unlock data is a whole level at the recorded pitch; uploads
  (`SetTextureLevelData`, `LoadSurfaceData`) go through the replayer's own
  conversion, as in the C++ renderer.
- `CopyVertices` and `CopyVertexData` carry the source, not the converted
  vertices: the replayer converts (DX8RENDERER's `CopyVertexData` rules).
  For `CopyVertexData` the following unlock already holds the result.
- Ret values are the C++ results; a replayer may compare them.

## Limits of version 0

- Content made on the GPU in skipped frames (render targets, `BlitTexture`
  results) is not in the trace; a frame that samples it replays with what
  the replayer's own earlier calls produced.
- The light data of `AddLight` and `UpdateLight` is the caller's struct at
  the call; the renderer reads it at the next draw.
- Nested locks of one buffer are matched last in, first out.
- A lock whose offset pointer was null has `FirstVertex` / `FirstIndex` null.
- Surface data is recorded for pfenum formats 1 to 8 and DXT1 to DXT5
  (10 to 14); other formats get a `note` and no data.
- A `SwapBuffers` that recreates a lost device (`HANDLE_SWAPLOSS`, off in the
  shipped INI) is not modelled as a device reset.
- Writing the blobs of the always-written uploads in skipped frames costs
  disk space until the last selected frame; pick early frames, or a short
  range, for large scenes. Blobs with the same bytes are stored once.
- Reading locked vertex and index buffers back is slow on some drivers
  (write-combined memory); traced frames run slower than untraced ones.
- Float arguments are forwarded by the compiler like any other `float`; on
  x87 a signalling NaN argument could arrive quieted (none is expected in
  renderer calls). The trace records each float from its argument slot.
