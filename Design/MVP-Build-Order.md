# MVP Build Order — Vertical Slices

**Status:** Session output 2026-08-17 · **implementation progress appended 2026-08-18.**
Each slice is independently testable, lands green (`Tests/` + `selfTest` where applicable),
and is sized at "a few days" or less. Order matters — later slices assume earlier ones.
Milestones: **M0** = the brief's named first milestone; **M1** = first commanded fleet;
**MVP** = playable definition met.

Each landed slice carries a **Built** line: what is in the tree, and — where a slice is only
partly done — what is still owed. A slice is not called done because its interesting part
works; the leftovers are named so they cannot be quietly dropped. Manual checkpoints (anything
needing a GPU, a window, or a person watching) are listed as outstanding until someone has
actually run them, because CI cannot.

Test placement follows the Dependency Map: sim logic proves itself in `GameLogicTests`,
transport/foundation in `NeuronCoreTests`, host lifecycle in `NeuronServerTests`, client math
in `NeuronClientTests`, and anything needing the real loopback or GPU in `selfTest` /
manual checkpoints.

---

### S1 — Window, device, swapchain
Raw Win32 window; DX12 device + direct queue; flip-model swapchain (3 buffers, waitable, 2
frames in flight); clear-to-colour animates; resize + fullscreen-borderless toggle; clean
shutdown; debug layer clean.
**Accept:** runs 5 min without debug-layer messages; PresentMon shows flip model; close exits 0.
**Built ✅ (code):** `Window.h/.cpp`, `GpuDevice.h/.cpp`, `GpuSwapChain.h/.cpp`, `GpuCom.h`,
`ClearColour.h/.cpp`, `ClientApp.h/.cpp`. COM ownership is `winrt::com_ptr` (AGENTS.md §5).
**Accepted ✅ 2026-08-18:** the three manual measurements — five minutes with no debug-layer
messages, PresentMon showing the flip model, and a clean exit — were run by the owner on a GPU
machine and passed. Note the scope: they were run against S1–S4's clear-and-present frame, so
they do not cover the passes S5 added.
**Outstanding:** resize and borderless-fullscreen toggle are wired but still unexercised.

### S2 — NeuronCore foundations
`Assert/Log/Time(QPC)/Hash(FNV)/Random(PCG32)`, `ByteReader/Writer`, SPSC/MPSC rings,
telemetry lane registry + `NEURON_SPAN/COUNTER`, `TaskPool`. **No math layer** — DirectXMath
is called natively at use sites (ADR-010).
**Accept:** `NeuronCoreTests`: byte IO round-trip + underrun bounds, ring stress (2 threads),
PCG32 vectors, span timing sanity, `XMVerifyCPUSupport` gate.
**Built ✅:** `Debug.h`, `Log.h/.cpp`, `Clock.h/.cpp`, `Hash.h`, `Random.h`,
`ByteReader.h`, `ByteWriter.h`, `Arena.h`, `EntityRecord.h/.cpp`; `RingBuffer.h` carries both
the SPSC ring and `MpscRingBuffer` (Vyukov bounded queue — sequence numbers per slot, so a
producer cannot claim a slot the consumer has not finished with); `Telemetry.h/.cpp` (lane
registry capped at 16, per-lane SPSC rings, `NEURON_SPAN` / `NEURON_COUNTER`, and
`TelemetrySnapshot` to aggregate a drain by name); `TaskPool.h/.cpp` (`Submit`, `WaitGroup`,
and a `Wait` that runs tasks on the calling thread so a zero-worker pool still makes progress).

A lane is a **role, not a thread** — re-registering a name adopts the existing lane, so a task
pool that stops and starts reuses `Worker 0` instead of eating the cap a restart at a time.

Wired at the use sites rather than left as a library nobody calls: the sim thread registers
`Sim` and spans `Poll` and `Tick`, with `TickOverrun` and `TickCatchUp` as counters (R10, and
ADR-002's "release counter" taken literally — telemetry is not compiled out of Release); the
frame loop registers `Main` and spans `Frame`, `Net`, `Render`; `wWinMain` calls
`DirectX::XMVerifyCPUSupport()` before anything computes (ADR-010, R11).
**Outstanding:** ~~the `GAME/EXTRACT/RENDER/UI` rows the corpus HUD shows need those stages to
exist — they arrive with S5.~~ **Closed by S5:** the frame loop now spans `Net`, `Game`,
`Extract`, `Render` and `Ui`, and the opaque pass spans `Opaque` inside `Render`; `Ui` is
declared and empty until S11 gives it something to draw. `GameLogicTests` is still unwired
(its library is empty).

### S2b — JSON parser & configuration
`Json.h/.cpp` (iterative parse, flat-node DOM, exact `int64`, comments + trailing commas,
diagnostics with line/column) and `JsonWriter`; `ConfigLoad` in the exe (cwd → exe-dir
resolution, LocalAppData user layer, deep merge) producing `ServerConfig`/`ClientConfig`;
S1's window/renderer values move out of code into `Outpost.json`; `mode` honoured.
**Accept:** `NeuronCoreTests` JSON corpus — valid/invalid cases, `int64` exactness at and past
2⁵³, depth-cap rejection, duplicate-key rejection, `\uXXXX` surrogate decoding, writer
round-trip stability; missing base config exits fatally with file/line/column; a corrupt user
layer is ignored with a warning; **no argv or environment reads anywhere** (grep rule).
**Built ✅:** `Json.h/.cpp`, `JsonWriter.h/.cpp` in NeuronCore; `AppConfig.h/.cpp`,
`ConfigLoad.h/.cpp` in the exe; `Outpost.json` at the repo root. `wWinMain` ignores its
arguments by signature.

### S3 — ServerHost skeleton + headless mode
`ServerHost{Start/Stop/Join}` with Sim thread, 20 Hz waitable-timer loop (absolute schedule,
snap-forward rule), tick counter + `tickOverrun` telemetry; `Outpost.exe` with `mode: "headless"` runs it
under console logging until Ctrl-C.
**Accept:** `NeuronServerTests` start/stop/join ×100 no leak/hang; headless 60 s: mean period
50 ms ± 0.5, no overruns on an idle machine.
**Built ✅:** `ServerHost.h/.cpp` (absolute-schedule waitable-timer loop, snap-forward past
250 ms, at most 2 catch-up ticks), `ServerConfig.h`, `Simulation.h`; `mode: "headless"` runs it
until Ctrl-C. `NeuronServerTests` covers start/stop/join, repeated cycles, idempotent stop, and
the tick rate.
**Accepted ✅ 2026-08-18:** the 60-second period measurement was run by the owner on an idle
machine and met the 50 ms ± 0.5 bound with no overruns. The CI test still asserts only loose
bounds (5–40 ticks in 500 ms), because a shared runner is not a real-time system and never
will be — the tight bound is a manual check by design, not by omission.

### S4 — Transport + handshake + heartbeat 🏁 **M0**
`Transport` + `UdpTransport` (non-blocking Winsock, 1,152 B datagram cap, minimal control-
channel reliability); `Hello/Welcome/UpdateRequired` with schema hash (and `universeHash` +
`worldMeta`, which **landed with S5b** — the hash as `contentHash`, the rest as
`worldId`/`anchorX`/`anchorY`; see ADR-009 §8 for why it is not carried twice); `Ping/Pong`;
client half connects in-process; NET stats (RTT/loss) logged both sides.
**Accept:** *window opens, swapchain presents, server ticks, heartbeat crosses the loopback* —
the brief's milestone, demonstrably. `NeuronCoreTests` handshake over real loopback socket;
schema-hash mismatch produces `UpdateRequired` + refusal (test forces a bad hash).
`selfTest` covers handshake + ping.
**Built ✅:** `Transport.h`, `UdpTransport.h/.cpp` (non-blocking Winsock, 1,152 B cap,
stop-and-wait control reliability), `Wire.h/.cpp`, `ClientConnection.h/.cpp`; NET stats logged
on both sides every 5 s; `SelfTest.h/.cpp` drives the whole M0 exchange headlessly and returns
an exit code. `NeuronCoreTests` covers the loopback and the wire; `NeuronServerTests` covers
the handshake and a forced hash mismatch producing `UpdateRequired`.
**Accepted ✅ 2026-08-18:** the visible half of M0 — window open, swapchain presenting,
heartbeat live — was run by the owner and passed. The engine half has been green in CI
throughout. **M0 is complete.**

---

## 🏁 M0 — **complete**, 2026-08-18

**Every criterion M0 rests on is met.** The machine-checkable ones are green in CI on every
push; the five that need a GPU, a window and a person were run by the owner on 2026-08-18 and
signed off.

The slices M0 rests on are S1–S4. Their acceptance criteria, and how each stands:

| Criterion | Slice | How it is verified | Status |
|---|---|---|---|
| Byte IO round-trip + underrun bounds | S2 | `NeuronCoreTests` | ✅ |
| Ring stress, two threads (SPSC) | S2 | `NeuronCoreTests` | ✅ |
| MPSC stress, four producers, per-producer ordering | S2 | `NeuronCoreTests` | ✅ |
| PCG32 vectors | S2 | `NeuronCoreTests` | ✅ |
| Span timing sanity | S2 | `NeuronCoreTests` | ✅ |
| `XMVerifyCPUSupport` gate | S2 | `NeuronCoreTests`, and `wWinMain` before it computes | ✅ |
| JSON corpus: `int64` past 2⁵³, depth cap, duplicate keys, surrogates, writer round-trip | S2b | `NeuronCoreTests` (21 cases) | ✅ |
| No `argv` or environment reads anywhere | S2b | grep rule over every source file | ✅ |
| Start/stop/join ×100, no leak or hang | S3 | `NeuronServerTests` | ✅ |
| Server ticks at 20 Hz | S3 | `NeuronServerTests`, and `selfTest` reports the mean period | ✅ |
| Handshake over a real loopback socket | S4 | `NeuronCoreTests` + `NeuronServerTests` | ✅ |
| Schema-hash mismatch ⇒ `UpdateRequired` + refusal | S4 | `NeuronServerTests` (forces a bad hash) | ✅ |
| Datagram cap enforced (1,152 B) | S4 | `NeuronCoreTests` | ✅ |
| Heartbeat crosses the loopback and returns | S4 | `NeuronServerTests` + `selfTest` | ✅ |
| NET stats (RTT/loss) logged both sides | S4 | both loops log on a 5 s cadence | ✅ |
| `selfTest` covers handshake + ping | S4 | 15 checks, exit code 0 or 3 | ✅ |
| Engine libraries never reference GameLogic | ADR-014 | include paths declared per project; no engine source includes a GameLogic header or names `Game::` | ✅ |
| **Window opens, swapchain presents** | S1, S4 | a person, at a machine with a GPU | ✅ **owner-validated 2026-08-18** |
| 5 min with no debug-layer messages | S1 | manual | ✅ owner-validated 2026-08-18 |
| PresentMon shows flip model | S1 | manual (`DXGI_SWAP_EFFECT_FLIP_DISCARD` + waitable object are in the code) | ✅ owner-validated 2026-08-18 |
| Close exits 0 | S1 | manual | ✅ owner-validated 2026-08-18 |
| Headless 60 s: mean period 50 ms ± 0.5, no overruns | S3 | `selfTest` measures and reports it; the ± 0.5 bound needs an **idle** machine | ✅ owner-validated 2026-08-18 |

**How the manual five were closed**, and how to reproduce them — one run each, on a machine
with a GPU:

1. `"selfTest": true` in `Outpost.json`, run the exe, read `Outpost.log`. Exit code 0 and
   `self test: PASSED` closes the cadence measurement and re-proves the whole M0 exchange on
   real hardware rather than a CI runner. The log line to read is
   `N ticks in M ms -- mean period X ms`.
2. `"selfTest": false`, `"mode": "host"`, run it. A window that opens, clears to the animated
   near-black blue, and logs `first pong: server tick N, round trip X ms` is M0's visible half.
   Leave it five minutes, watch the debug-layer output, close it, check the exit code.

*The sign-off is recorded as a verdict, not as measurements* — the observed mean tick period
and round-trip were not captured into this document. If those numbers are wanted as a baseline
to regress against later, they have to come off that run's `Outpost.log`.

**M0 being closed does not close S5's GPU items, and the distinction is not pedantry.** These
five were validated against the S1–S4 renderer, which cleared the screen and presented. S5
added the opaque pass, the pipeline state, the upload ring, a depth buffer and an atlas
texture upload — none of which existed when the debug layer was watched for five minutes. S5
carries its own outstanding list for exactly that reason.

Everything else is signed off by CI, which at the time of writing runs **122 tests across four
assemblies with zero unique warnings** on every push.

---

### S5 — Meshes, atlas, opaque pass, camera
OBJ/MTL loader → submesh ranges (8 ship classes + Structure); DirectWrite glyph-atlas bake
(TaskPool); opaque instanced pass (flat shading, 5 materials, emissive accents); ortho camera
30° elevation, yaw orbit + 45° snaps, zoom clamp, plane pan. A locally-faked parked fleet
renders — no net yet.
**Accept:** `NeuronClientTests` OBJ parser (counts/ranges vs known meshes); visual checkpoint
vs `tactical-hud.png` vibe (dark space, green accents, silhouettes readable at min zoom);
frame time < 2 ms at 41 instances.
**Built ✅ (code):**
`ObjMesh.h/.cpp` — hand-rolled OBJ/MTL parse, faces regrouped into submesh ranges in canonical
material order (so Structure's 117 interleaved `usemtl` groups become five draws), vertices
deduplicated on (position, normal) so hard edges stay hard, bounds and hull radius, and
`(line, column, message)` diagnostics on every malformed input. Free of D3D and C++/WinRT
headers, like `ClearColour.h`, so the tests need no device.
`IsoCamera.h/.cpp` — orthographic, elevation fixed at 30°, focus on the plane, free orbit with
45° detents, multiplicative zoom clamped to 0.5–40 km, and pan that undoes the foreshortening
(screen-up costs 1/sin 30° = 2× screen-right, the same factor as the 2:1 rings).
`InputMap.h/.cpp` + `Window` input — `InputFrame` → `CameraIntent`, pure and testable; the
virtual-key table lives in `Window`, which is the only file entitled to know what `VK_OEM_PLUS`
is. Bindings avoid the left and right buttons on purpose: S8 needs one for box-select and S9
the other for the order puck, so the camera takes the middle button, the wheel, the screen edge
and the keyboard, with Alt turning a middle-drag from a pan into an orbit.
`RenderWorld.h/.cpp` — `InstanceRecord` (20 bytes, ADR-006 §6's field names, static-asserted
because it *is* the per-instance vertex stream), `RenderScene` sorted by `classId` into
contiguous per-class runs, and the parked-fleet placeholder S7 deletes.
`GpuUploadRing.h/.cpp` `GpuPipelines.h/.cpp` `GpuMeshes.h/.cpp` `GpuPasses.h/.cpp` — a
per-frame linear upload allocator, the shared root signature and the opaque PSO, VB/IB per
class in a default heap, and the `Clear → Opaque` pass list with the rest of ADR-006 §1's nodes
written out as reserved slots. The depth buffer joins `GpuSwapChain`, which is the one other
thing that owns the swapchain's size.
`GlyphAtlas.h/.cpp` — DirectWrite bakes printable ASCII plus the box and marker glyphs into one
R8 texture, one task per size on the boot `TaskPool`, packed and blitted single-threaded so the
layout does not depend on the thread schedule. Baked but not yet drawn: the Ui pass is S11.
`GameData/Shaders/Opaque.hlsl` — compiled at boot, so a shader is content like a mesh. Nine
mesh file names move into `Outpost.json` under `content`, and their order *is* the `classId`
order: the engine loads the list it is handed and has no opinion about which index is a Carrier
(ADR-014).

**One convention settled here rather than papered over.** ADR-006 §3a named the `RH`
DirectXMath entry points, while ADR-001 §3 fixes render space as `(sim.x, h, sim.y)` with `+Y`
up — `(east, up, north)`, which is a **left-handed** basis. Together they mirrored the view:
east projected to the *left* of the screen. An `RH` call over a left-handed world does not
fail, it mirrors, so nothing but a projection test could have caught it — and one did.

Rather than patch the camera, the tree now **standardises on left-handed** and ADR-006 §3a
states it as a standing rule with the call sites enumerated: DirectXMath's `LH` matrices,
D3D12's default winding, `BoundingFrustum`'s `rhcoords: false`, and X3DAudio's native
coordinates. That last one is the reason to hold the line rather than merely fix the camera —
a right-handed tree owes X3DAudio a `.z` negation on four fields of two structures on every
listener update, forever, and X3DAudio is what ADR-011's audio slice is built on. Left-handed
is also every one of those APIs' own default, so the convention costs no conversion anywhere.
ADR-001 §3, ADR-010 §5, ADR-011 §4 and AGENTS.md §5 were brought in line;
`NeuronClientTests`' `EastIsOnTheRightOfTheScreen` is the guard.

**A content correction fell out of the same work.** ADR-006 §5 said the corpus carries per-face
normals so "plain vertex normals shade flat". Mostly true, and now measured: 152 of
`Structure.obj`'s 1,784 faces carry a different normal per corner, around a curved section, and
a few faces in four other meshes do too. Nothing had to change — the loader keys a vertex on
(position, normal), so a smooth corner becomes its own vertex and interpolates — but the ADR no
longer claims a property the content does not have. It surfaced because the first version of
the handedness test asserted the derived normal *equalled* the authored one; that assertion was
testing flatness, not handedness, and the corpus said so.

**Verified:** `NeuronClientTests` covers the OBJ parser against hand-written text *and* against
the nine shipped meshes' real counts and ranges, eight malformed-input diagnostics, the
derived-normal fallback's orientation against all 5,276 authored triangles (the loader's one
handedness-sensitive path, and the only one the corpus does not otherwise exercise), camera
projection at six yaws (focus centred, viewport edges exact, ground circles 2:1, cosmetic height
lifts without shifting, the whole 40 km grid inside the depth range), zoom/detent/pan state, the
input bindings, and the extract layout.
**Outstanding:** everything needing a GPU — the visual checkpoint against `tactical-hud.png`,
the `< 2 ms at 41 instances` frame-time measurement, and a debug-layer-clean run. CI has no GPU
and no display, so nothing below the parser and the maths has been *executed* anywhere; the
GPU-side code is compiled-and-reviewed only. The atlas is baked and resident but nothing samples
it until S11, and the animated S1 clear colour is still under the scene — it stays inside the
art direction's blue, but it is the first thing to delete if the visual checkpoint dislikes it.

### S5b — Universe definition & Vesta-3
`UniversePos`/`UniverseDef` types + pure JSON-backed parse in GameLogic (ADR-009 + ADR-012,
using S2b's parser); `GameData/Universe/` authored with Vesta-3 (star, two planets, one
station); `universeHash` over canonical parsed content; hosts read
the file via NeuronCore and both halves load it; grid anchored at the station; station renders
with the `Structure` mesh; celestials are parsed and hashed but not drawn (ADR-009 §9a).
**Accept:** `GameLogicTests` parse round-trip, malformed-input rejection, `universeHash`
stability across reorderings that shouldn't matter and change on ones that should,
anchor+local reconstruction property test; the client's rendered scene comes from the file
(edit a station position → it moves, no rebuild).
**Built ✅ (code):**
`GameLogic/Ids.h` `Universe.h/.cpp` `UniverseParse.h/.cpp` — the first code GameLogic has ever
carried. `UniversePos` is `int64` metres and **there is no float anywhere in the model**: that
is what makes the hash exact, the equality meaningful and the reconstruction bit-perfect.
`ParseUniverse` takes bytes rather than a path, so GameLogic stays OS-free and its tests need
no filesystem; it reports *every* problem in one pass (ADR-012 §C8) with a line and a JSON path
(`systems[0].stations[1].position.x`), and it refuses rather than rounds a coordinate written
with a fraction — the reason ADR-012 §C7 demanded exact `int64` from the parser.
`ComputeUniverseHash` walks entities **in id order, not file order**, so reformatting or moving
a system up the array is not a content change while editing one is.
`Outpost/UniverseLoad.h/.cpp` — locates and reads the file, exactly as `ConfigLoad` does, then
hands the bytes over. `Main.cpp` loads it once and feeds both halves, and is the one place
universe metres become the grid's local metres for the renderer.
`GameData/Universe/Frontier.json` — Vesta-3: a star, two planets (Kessler and Halgren) and
Vesta-3 Anchorage, in a Vesta Reach region, with the grid anchored at the station.
`Welcome` gains `worldId`/`anchorX`/`anchorY` and `Simulation` gains `World()`, so a client in
another process learns where it is; `CORE_SCHEMA_TEXT` changed with them, which is the
mismatch mechanism working rather than a break.

**Two departures from the ADR text, both recorded in ADR-009 §8.** The universe hash travels as
the existing `contentHash` rather than a second copy inside `worldMeta` — two fields holding
one number can disagree, and the one that decides whether to refuse a client would win
silently. And the other two fields are named in engine terms (`worldId`, not `systemId`)
because they live in `NeuronCore/Wire.h`, which has to stay plausible in an unrelated
networked sim; the mapping is one-to-one and the engine never reads them.

**Verified:** `GameLogicTests` is live for the first time — 15 cases covering the parse of every
field, `int64` exactness past 2⁵³, comments and trailing commas, twelve malformed inputs each
refused *with* a diagnostic, all-problems-in-one-pass, hash stability under reformatting and
entity reordering, hash movement under six kinds of real edit, the start anchor, and the
anchor+local reconstruction property over 121 positions, and — separately — the ends of the
int64 plane, where an unguarded subtraction wraps a distant position into a false accept.
`NeuronClientTests` covers the scenery path into the render scene. The wire change is covered
at both ends: `NeuronCoreTests` round-trips a `Welcome` carrying a full-width negative anchor,
and `NeuronServerTests` asserts the simulation's `WorldMeta` arrives intact at a raw
core-level client.
**Closed, not built:** this slice was written owing "celestials as distant backdrop", and that
debt turned out not to exist. No slice in S1–S15 schedules it, and neither print draws a
celestial body — `tactical-hud.png` is empty space with an ambient haze, `strategic-map.png`
is a node graph. ADR-006's reserved `Nebula` node is that haze, composited after `Opaque`, not
a celestial renderer; reading it as one is what made a design gap look like queued work.
Owner decision on 2026-08-18: celestials are data the game reads, not content the frame draws
(ADR-009 §9a). Parsed, hashed, and loaded identically by both halves is the whole requirement,
and it is met.
**Outstanding, and needing a GPU:** confirming on screen that moving the station in the file
moves it in the world. The demonstration is to edit `stations[0].position` and restart — the
logged grid anchor moves with it, and a second station added to the array appears at its
offset, with nothing rebuilt.

### S5d — The Nebula node *(taken out of order, at the owner's request)*
The reserved `Nebula` slot in ADR-006 §1, built: a CPU-baked periodic field
(`NebulaField.h/.cpp`), its GPU upload (`GpuNebula.h/.cpp`), an additive full-screen pass after
`Opaque`, `Nebula.hlsl`, and the parameters as content under `client.nebula`. The animated
clear colour S1 used to prove the loop was running is retired for a static near-black.
**Accept:** `NeuronClientTests` covers the field without a device — determinism, sparseness,
smoothness, seamless wrap, and that the settings do what they are named — plus the NDC→plane
mapping round-tripped against the real view-projection; visual confirmation on a GPU that the
haze belongs to the world rather than the screen.

**Out of order on purpose, and safe.** It sits before S5c here because S5c has not landed yet
and this was asked for first. Nothing about it touches S5c's seams: it is one pass reading one
baked texture and the camera's own mapping, with no simulation, no world view and no wire
involved. Numbered S5d rather than renumbering anything, because the build order is a plan and
this is a record of what happened to it.

**Built ✅ (code):**
`NebulaField.h/.cpp` — the field, device-free. Periodic value noise, four octaves, FNV-1a for
the lattice so the bake is stable across builds by construction, shaped by a coverage floor and
a contrast curve so most of the tile is black. "Sparse space" is the art direction, and an even
grey fog would have passed every test that did not measure the distribution — so one does.
`GpuNebula.h/.cpp` — bake, upload, SRV, in the shape `GlyphAtlas` already established.
`NebulaPass` in `GpuPasses` — additive, depth-blind, one full-screen triangle built from
`SV_VertexID` so there is no geometry to keep in sync with the shader.
`IsoCamera::PlaneMappingForNdc` — the affine NDC→plane map, which is what anchors the field to
the world; S8's overlay will want the same thing.
`ClearColour` — `AnimatedClearColour` and its three tests are gone, replaced by
`SpaceClearColour` and tests that assert it does *not* move.

**The reserved list did what it promised.** ADR-006 §1 claimed a new node would be an insertion
rather than a redesign, and that claim had never been tested. Inserting this one cost a struct,
a line in `GpuPassList::Record`, a PSO, one SRV and one sampler. No pass was reordered and no
existing pass changed. That is now recorded in ADR-006 §1a as a measurement rather than an
intention.

**Two decisions worth keeping.** The field is baked on the CPU rather than evaluated in the
shader, because a procedural shader would put the only copy of the function where no test in
this tree can reach it — and the alternative, a second copy in C++ to test against, is two
implementations free to disagree. And the tile is *periodic* rather than sized to the play
area: `MAX_ZOOM_METRES` is a 40 km half-height against a 40 km grid, so the camera can see past
the play area and a clamped tile would smear its edge over everything beyond. Periodicity
removes that failure instead of sizing around it, and "does the seam show" became a test.

**Verified:** `NeuronClientTests` is 59 cases, up from 46. The thirteen new ones cover the
field's determinism, its distribution, its seamless wrap, the continuous function's period, the
refusal of settings that describe no field, wrapping fetches, and the plane mapping — including
3,200 round trips through the real view-projection across eight yaws, four zooms and three
focus positions, worst NDC error 7.6e-6.
**Outstanding, and needing a GPU:** what the haze actually looks like. The numbers say sparse,
smooth and world-anchored; whether it reads as space behind a fleet is a judgement no test
makes. The knobs for that are in `client.nebula` and take effect on restart.

### S5c — The engine/game seams
`Neuron::Simulation` (NeuronServer) and `Neuron::WorldView` (NeuronClient) declared, with the
neutral types they speak — `EntityRecord` (NeuronCore), `RenderScene`, `OrderIntent`,
`OrderPreview` (ADR-014 §2b renamed it from `FormationPreview`); stub implementations in the
test projects; `ServerHost` and `ClientApp`
take them by reference; `Outpost.exe` constructs the GameLogic-backed ones (ADR-014).
**Accept:** `NeuronServerTests` and `NeuronClientTests` drive their library against a **stub**
simulation/world view with no GameLogic in sight — the proof the engine is game-free;
`Outpost.exe` is the only project referencing GameLogic (grep rule on the vcxprojs).
**Built ✅ (code):**
`NeuronCore/OrderIntent.h` — `OrderIntent`, `OrderVerdict` and `OrderPreview`, in NeuronCore
rather than beside either seam because both seams speak them: `WorldView::PreCheck` and
`Simulation::ApplyOrderBytes` must return the same verdict type or ADR-014 §3's BounceParity is
unverifiable, and NeuronClient cannot see NeuronServer. `OrderVerdict` moved down out of
`Simulation.h` for exactly that reason.
`NeuronClient/WorldView.h` — the client seam: `ApplySnapshot`, `BuildScene`, `PreCheck`,
`SolvePreview`, `EncodeOrder`, `SchemaHash`, `ContentHash`, plus `NullWorldView`, which builds
an *empty* scene because a client wired to nothing should look like one.
`Outpost/ParkedFleetView.h/.cpp` — the placeholder world, now on the game's side of the seam.
`ClientApp::Initialise` takes a `WorldView&` the way `ServerHost::Start` takes a `Simulation&`,
and `ExtractScene` is one `BuildScene` call.

**The engine stopped carrying world data.** `ClientConfig` lost `staticScenery`, `worldId`,
`gridAnchorXMetres`, `gridAnchorYMetres`, `schemaHash` and `contentHash`. Configuration is how
the client is *set up*, not what it is *looking at*, and the handshake hashes are now asked of
the world view at connect time — a client told its own content hash by its configuration cannot
notice that its content changed underneath it.

**ADR-014 could not be implemented as written, and S5c is where that surfaced.** §1 says
GameLogic depends on NeuronCore only; §2 says GameLogic implements interfaces declared in
NeuronServer and NeuronClient. A class cannot implement an interface it may not include.
Resolved in favour of §1 — the composition root holds the vtable — because GameLogic's freedom
from Windows, D3D12 and file IO is what lets `GameLogicTests` run with no device and no
fixtures, which is precisely what S5b leaned on. Recorded as ADR-014 §2a, with the cost stated:
the adapters sit in the one project with no test project, so they are kept thin enough that
there is nothing in them to test. `FormationPreview` became `OrderPreview` in the same pass,
against ADR-014 §4's own rule that the engine may name no formation (§2b).

**Verified:** `NeuronClientTests` is 71 cases, up from 59. The twelve new ones write a world
view out of nothing but engine headers and drive it through every method — a scene that is
rebuilt rather than accumulated, a snapshot that crosses as bytes nothing reinterprets, a
verdict whose reason code the engine never reads, a preview that reports its cap instead of
truncating quietly, and an encode that refuses rather than sending half an order. The file
includes no GameLogic header and the project references no GameLogic, which is the claim.
CI now fails if any engine or test project names GameLogic, or if any engine source includes a
GameLogic header.
**Outstanding:** the seam is declared and driven, but nothing real is behind it yet —
`ParkedFleetView` invents a fleet and refuses every order, because GameLogic has no world until
S6 and no snapshots until S7. `ClientApp` calling `BuildScene` every frame is not covered by a
test: that needs a device and a window, and remains a manual checkpoint.

### S6 — GameLogic sim + replay determinism
World tables, `ShipClassTable` (11-value enum, 9 with content — Fighter/Cruiser reserved),
`Steering/Integrate` (seek-with-arrival, accel + turn-rate clamps), scripted-order harness,
`WorldHash`.
**Accept:** `GameLogicTests`: double-run replay hash equality (1,000 ticks, scripted orders);
movement envelope (top speed, turn radius, arrival overshoot < tolerance); zero clock/RNG
imports outside the seeded PCG32 (grep-able rule, asserted in review); no `UniversePos` in
per-tick sim math (grep rule, ADR-009 §2).
**Built ✅ (code):**
`ShipClass.h/.cpp` — the icon sheet's closed eleven, compiled in rather than authored: class
parameters decide the movement envelope, and putting them in a file the two halves could
disagree about would need the schema hash to grow to cover it. Fighter and Cruiser keep their
ids with `hasContent = false` and cannot be spawned. `Structure` is in the table with zero
speed, so a station is a ship that never moves and there is one movement path rather than two.
`World.h/.cpp` — structure-of-arrays tables, stable `ShipId`↔slot indirection over swap-and-pop
removal, seeded `Pcg32` in world state, and `Tick` running IngestOrders → Steering → Integrate.
`GroupAdvance` (S10) and `EmitSnapshot` (S7) are absent rather than stubbed, so the pipeline
reads as what it does.
`WorldHash.h/.cpp` — FNV-1a over the state in dense-array order, folding **float bit patterns
rather than values**: a tolerance would forgive exactly the class of bug this exists to find.
`ComputeReplicatedHash` answers the weaker question — would the two runs have *looked* the
same — which is what a desync report is actually about (F10).
`Main.cpp` — the server advances a real world now. It does not yet say so: `WriteSnapshot` is
still empty, so the fleet moves on the server and nothing sees it. That is one function away
from not being true, and the function is S7's.

**One departure from ADR-005 §1, recorded here because it is a real choice.** The ADR wrote
guidance as `{mode, groupRef, stationIndex}` — a reference into the `OrderGroup` table. This
carries the resolved target instead, which keeps `Steering` from knowing that groups exist:
when S9 brings the group table and S10 the station solve, the group *writes* these fields and
steering does not change. The cost is that a station will live in two places once groups exist,
and the group is the authority. The benefit is that the movement model is testable with no
order machinery at all, which is exactly what this slice's replay harness needed.

**Two defects the harness found, both invisible to review.**
*Ships orbited their targets.* A hull's turn radius at cruise is `speed / turnRate` — 477 m for
a Battleship — so a target sixty metres astern sits deep inside the circle it can turn on. With
only an alignment factor damping the approach, the ship held its speed, swept past, and came
round again: **three full laps and a hundred seconds of simulated time to travel sixty metres.**
Fixed by bounding speed at `omega · distance / (2 · error)` — the arc has to fit inside the
distance remaining — measured across five hulls and five approach geometries to pick it.
*Every fast hull overshot.* The textbook braking curve `v = sqrt(2 a s)` is exactly achievable
only in continuous time; stepped at 20 Hz a ship sits fractionally above it, covers more ground
per tick, finds the curve has dropped by more than one tick of braking, and falls further
behind — compounding all the way in. An Interceptor braking flat-out from cruise arrived 1.2 m
from its target still doing **38 m/s**, sailed through, and settled 5.9 m past it, three times
the arrival tolerance. Fixed by solving the same equation for the steps actually taken:
`v = -a·dt/2 + sqrt((a·dt/2)² + 2 a s)`. Every playable hull now stops at the ring edge with no
overshoot at all.

**Verified:** `GameLogicTests` is 32 cases, up from 15. Replay: 1,000 ticks of scripted orders
run twice, fifty checkpoints, bit-identical — plus the control that makes it mean something,
where **one order moved by one metre, once** changes every later checkpoint and none before it.
Envelope: every playable hull over four approach geometries, asserting no tick exceeds its top
speed, turn rate or acceleration; arrival inside tolerance with the commanded facing; overshoot
bounded; and the orbit regression measured as accumulated bearing, because distance alone
cannot tell circling from a wide approach. Tables: reserved classes refuse to spawn, an unknown
class from the wire is refused rather than clamped, and ids survive the swap-and-pop that keeps
the arrays dense. CI now fails GameLogic for reading a clock, drawing unseeded randomness,
calling an `XM*Est` function, or naming a `UniversePos` in per-tick code.
**Outstanding:** nothing replicates. The world moves and no one can see it until `EmitSnapshot`
and the snapshot format land in S7 — which is also when the server gets a scripted patrol to
give these ships somewhere to go.

### S7 — Snapshots over the wire → moving ships on screen
`EmitSnapshot` (full, quantised) → datagram → `ReplicatedView.ApplySnapshot` →
`SnapshotBuffer` (interp delay 2 ticks, extrapolation ≤ 250 ms → STALE flag) → Extract →
instances. Server runs a scripted patrol so ships move without input.
**Accept:** motion visually smooth at 144 Hz render / 20 Hz snapshots; induced 400 ms sim-
thread stall (debug key) shows extrapolate-then-freeze, recovers clean; `GameLogicTests`
wire round-trip: emit→bytes→apply equals quantised source; snapshot ≤ 1,152 B at 41 ships
(static assert + runtime check).

### S8 — Picking, selection, world-space overlay
Ray∩plane picking; click / shift-click / box-select; OverlayWorld pass: selection ellipses
(2:1 on plane, screen-floor clamp, depth-test vs hulls), hull/shield bars.
**Accept:** `NeuronClientTests` picking math (point/box vs known layouts, floor clamp cases);
manual: rings occlude behind a Carrier hull but bars never do (`overlay-pass.png` rule).

### S9 — Move orders end-to-end 🏁 **M1**
Right-drag order puck (plane point + facing), client pre-check via `WorldView::PreCheck`
(GameLogic's `ValidateOrder` behind the seam, ADR-014),
PENDING ghost, `OrderSubmit` → server validate (same function) → `OrderGroup` + station solve
(Line only) → steering to stations → promotion via snapshot order-state; `OrderAck` bounce +
reason on the failure paths (out-of-bounds, empty selection).
**Accept:** `NeuronServerTests` session-level order flow against a headless host (submit →
ack → state in next snapshot); `GameLogicTests` validation parity: identical verdict/reason on
quantised inputs for a case matrix; on-screen promotion ≤ 100 ms; deliberate out-of-bounds
order bounces with reason toast, visually identical local vs server refusal.

### S10 — Formations & the real footprint
`SolveFormation` for Line/Wedge/Claw; puck footprint preview = the same solve (one tick per
ship); arrival facing (drag-perpendicular or wheel while dragging); `GroupAdvance` leg
completion (tolerance + straggler timeout) and re-solve per leg.
**Accept:** `GameLogicTests` formation geometry (spacing from largest class, station count,
stable id→station assignment) + "preview equals outcome" (client-solve stations == server
final stations for same quantised inputs); fleet of 12 arrives in Claw matching the print's
footprint pattern.

### S11 — HUD v1
Glyph-quad Ui pass: top bar (ships, net RTT bars, sim indicator), fleet roster with wing rows
+ health strips, selection context bar (`N SHIPS : WING → FORMATION`), MOVE/FORMATION live +
ATTACK/STANCE/ABILITIES rendered disabled, order-pending indicator, minimal toast stack
(reject reasons).
**Accept:** HUD state is a pure function of replicated fields + local UI state (F10 spot-
check: kill the feed → HUD shows stale/empty, never invents); layout matches
`tactical-hud.png` zones; UI scale multiplier honoured at 0.8/1.0/1.6.

### S12 — Order queue & ETAs
`queueMode=append` up to 4 legs; ghost polyline with per-leg ETA labels; `QueueFull` bounce;
ETA = remaining-distance/speed model server-side, replicated in order state.
**Accept:** wire-enforced 4-leg cap (5th bounces); ETA error < 10 % on straight runs;
queued-chain rendering matches `puck-and-wheel.png` §4.

### S13 — msquic behind the same interface ⚡ spike
`QuicTransport`: ALPN `opf/1`, in-memory self-signed cert (`CertCreateSelfSignCertificate` +
`CERTIFICATE_CONTEXT`), client `NO_CERTIFICATE_VALIDATION` (loopback), stream 0 = control,
DATAGRAM = state. QUIC replaces `UdpTransport` outright — there is no transport config knob
(owner directive: QUIC only); `UdpTransport` is deleted when this slice lands.
**Accept:** the *unmodified* game runs over QUIC on loopback; `selfTest` runs the full
handshake+order+snapshot loop; measured added latency < 1 ms
loopback. Friction findings feed Risk R3 disposition (stay Schannel vs flag OpenSSL flavour).

### S14 — Debug strip, selftest, polish 🏁 **MVP**
Tier-1 counters strip (frame/GAME/EXTRACT/UI ms, net RTT/loss/jitter, snap age/drift ticks,
`tickOverrun`, drops) behind a toggle; `selfTest` aggregates: schema self-check,
transport handshake, replay determinism run, wire round-trips — exit-code CI gate; polish:
4× MSAA offscreen + resolve, cosmetic banking/hover from velocity, STALE marker visual.
**Accept:** MVP playable definition demonstrated end-to-end — select fleet, issue queued
formation moves, watch execution with status + feedback; `selfTest`
green on a GPU-less runner; counters strip numbers plausible vs `debug-hud.png` rows.

### S15 — Audio thin slice *(post-MVP-core; must not displace S1–S14)*
XAudio2 device + mastering voice + five submixes with gains from config; pooled source
voices; RIFF WAV loader; JSON sound bank; X3DAudio listener at camera focus raised by zoom
(ADR-011 §4) with mono emitters at render positions; one 2D UI cue (order rejected) and one
3D engine loop; `AudioUpdate` stage timed as the fifth budget row.
**Accept:** `NeuronClientTests` listener/emitter math + bank parsing headless; no audio device
⇒ client logs, disables audio, runs on; manual check: panning moves the audio frame, zooming
out attenuates, voice pool never exceeds its cap under a 200-ship stress scene.

---

## Sequencing rationale (why this order)

- S1–S4 front-load the two integration unknowns (DX12 plumbing, loopback transport) and land
  the brief's named milestone in four slices.
- Rendering (S5) precedes sim (S6) so every sim-side slice after S7 is *visible* — but S6's
  determinism harness exists **before** the first networked ship moves, so replication bugs
  never masquerade as sim bugs.
- The seams (S5c) land before the sim and before anything crosses them: `Simulation` is needed
  by S7's snapshots and `WorldView` by S9's orders, and an interface retrofitted after its
  callers exist is an interface shaped by its first caller.
- The universe definition (S5b) lands before the sim so the world is authored content from the
  first ship that moves — a hardcoded scene would be thrown away and would let sim code form
  habits around coordinates the universe model forbids.
- The order pipeline (S9) is deliberately after selection/overlay (S8): the ghost/bounce UX
  needs somewhere to draw.
- msquic (S13) sits after the protocol stabilises (S12) but before MVP-complete, per ADR-003 —
  late enough to test the real protocol, early enough that friction has schedule to land.
- Config (S2b) lands second because everything downstream reads it, and because the moment a
  value is hardcoded it acquires callers; S1 is the only slice allowed to hardcode, and S2b
  takes those values back.
- Audio (S15) is deliberately after the MVP: it is absent from the playable definition, and
  its architecture (ADR-011) is fixed so nothing before it has to guess.
