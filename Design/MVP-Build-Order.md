# MVP Build Order — Vertical Slices

**Status:** Session output 2026-08-17 · **implementation progress appended 2026-08-18 · S14
landed; play test signed off 2026-08-19 — the MVP is met.** · **This document is closed as a
plan** (2026-08-20): S1–S15 are all built, and post-MVP work lives in
[Universe-Build-Order.md](Universe-Build-Order.md) and
[Station-Build-Order.md](Station-Build-Order.md). What still lands here is a change *inside* a
slice this document owns — see the two recorded under the MVP sign-off.
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

## Open items across the whole document (swept 2026-08-19)

Every `**Outstanding:**` line below was re-read against the tree on this date and either closed
with a strikethrough and a dated note, or left open and sharpened. **Six things are genuinely
open**, and none of them blocks the MVP or M1 — both are met:

| # | Open item | Slice | Why it is still open |
|---|---|---|---|
| 1 | **Borderless-fullscreen has no runtime toggle** — the mode is read once at `Window::Create` and Alt+Enter is swallowed without doing anything | S1 | Never built, despite the old note saying "wired". Costs an action, a key and a swapchain resize; nothing has asked for it |
| 2 | **The station-moves-the-grid demonstration has not been run** — edit `stations[0].position`, restart, watch the anchor follow | S5b | Needs a deliberate content edit; a play test does not perform one |
| 3 | **No sim-side stall injector** — F10 cuts the client's *feed*, which is indistinguishable from the client's side but does not pause the authority | S7 | Only matters the day something tests the server stalling rather than the link going quiet |
| 4 | **What the puck should do when a station lands inside a gate or another fleet** | S10 | A design decision nobody has taken; `puck-and-wheel.png` §6 lists it OPEN |
| 5 | **Nobody has heard the audio** — the manual pass is S15's real acceptance, and the two WAVs are synthesised placeholders | S15 | Landed 2026-08-19; no listening pass since |
| 6 | ~~**The Debug CI leg does not gate** (`continue-on-error`) while R22 is open, so a green tick certifies **Release only**~~ **Closed 2026-08-20** | — | Risk-Register R22. The green Debug legs the row asked for arrived, the owner closed it, and `continue-on-error` came out of the build job. What survives R22 is a different question, now **R23**: `QuicTransportTests` has flaked on both configurations, and a flaky test that gates is a red build nobody caused |

Two more that belong to no slice and are recorded where they live rather than here: ADR-011 §8's
callback ring and external-lane registration (deliberately deferred, named in S15's notes), and
~~the fact that **nothing in the build copies `GameData/` beside the executable** — the deployed
copy under `x64/<config>/` is maintained by hand, which is a trap for the next person to add
content and find it missing at runtime.~~ **Closed 2026-08-20:** `Outpost.vcxproj` gained a
`CopyGameData` target that runs after Build and puts the content beside the executable, with
`Outpost.json` lifted to the output root because that — not inside the content folder — is
where `LoadAppConfig` reads it from. The layout was implied in three places and produced in
none: `ResolveContentPath` and `LoadAppConfig` both fall back to the executable's own folder,
and `FileSys::SetHomeDirectory` names `<exe>\GameData\` outright. `SkipUnchangedFiles` keeps
the ~15 MB bake from being re-copied every incremental build, and the copy does not mirror, so
content *deleted* from `GameData/` still lingers in an old output folder. CI is unaffected: the
self test runs from `ci-selftest/` with its own config, and the working directory wins over
beside-the-exe in both lookups.

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
**Outstanding, and sharpened 2026-08-19 — this was not quite the truth.** Resize *is* wired end
to end (`Window::ConsumeResize` → the frame loop's `HandleResize`, which owns the swapchain) and
has run in anger since. **The borderless-fullscreen "toggle" was never wired at all**: the mode
is read once from `client.window.mode` at `Window::Create` and picks `WS_POPUP` over
`WS_OVERLAPPEDWINDOW` (`Window.cpp:60`), and there is no runtime path to change it — no
`InputAction`, no entry in the virtual-key table, no method on `Window`. Alt+Enter is
*swallowed* rather than handled (`Window.cpp:400-406`), and that is deliberate for the right
reason and misleading for another: the comment says the chord "belongs to the client, not to
DXGI's own fullscreen handling", which is true, but the client then does nothing with it. So
the honest state is **config-only borderless, and a chord reserved for a feature that does not
exist**. Left open rather than built: it costs an action, a key and a swapchain resize, and
nothing in the MVP or the universe phase has asked for it yet.

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
declared and empty until S11 gives it something to draw. `GameLogicTests` was unwired at this
point, its library being empty; S5b wired it.

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

Everything else is signed off by CI, which at the time M0 closed ran **122 tests across four
assemblies with zero unique warnings** on every push. *(As of S9 the same four assemblies run
331: 163 client, 85 GameLogic, 73 core, 10 server.)*

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
`GameData/Shaders/Opaque.hlsl` — compiled at boot, so a shader is content like a mesh.
*(Superseded by S7a: shaders are built into the executable and this file no longer exists.)* Nine
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
**Outstanding:** ~~everything needing a GPU — the visual checkpoint against `tactical-hud.png`,
the `< 2 ms at 41 instances` frame-time measurement, and a debug-layer-clean run. CI has no GPU
and no display, so nothing below the parser and the maths has been *executed* anywhere; the
GPU-side code is compiled-and-reviewed only.~~ **All three closed by 2026-08-19.** The visual
checkpoint went with the play test; the budget is **measured** rather than judged — the strip's
RENDER row reads 1.10–1.62 ms on the 41-instance scene, against the 2 ms the criterion asked
for; and the client runs debug-layer-clean, which every windowed session since has confirmed by
logging nothing from it. The atlas is baked and resident but nothing samples it until S11, and
the animated S1 clear colour was replaced by S14's static near-black — a background that changes
on its own competes with the fleet in front of it.

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
**Outstanding, and still open as of 2026-08-19 — a play test does not cover this one.** The
demonstration is to edit `stations[0].position` and restart: the logged grid anchor moves with
it, and a second station added to the array appears at its offset, with nothing rebuilt. That
is a deliberate content edit, not something looking at a running session performs, so the
2026-08-19 pass leaves it exactly where it was. It is cheap to run and nobody has run it.

### S5d — The Nebula node *(taken out of order, at the owner's request)*
The reserved `Nebula` slot in ADR-006 §1, built: a CPU-baked periodic field
(`NebulaField.h/.cpp`), its GPU upload (`GpuNebula.h/.cpp`), an additive full-screen pass after
`Opaque`, `Nebula.hlsl` *(now `NebulaVS/PS.hlsl`, S7a)*, and the parameters as content under
`client.nebula`. The animated
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
**Outstanding, and needing a GPU:** ~~what the haze actually looks like. The numbers say sparse,
smooth and world-anchored; whether it reads as space behind a fleet is a judgement no test
makes.~~ **Seen and accepted 2026-08-19** — the field is under every frame of the play test, so
this closed with it rather than as a separate look. The knobs are in `client.nebula` and take
effect on restart, which is where a retune goes if the art direction moves.

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
**Outstanding:** ~~the seam is declared and driven, but nothing real is behind it yet —
`ParkedFleetView` invents a fleet and refuses every order, because GameLogic has no world until
S6 and no snapshots until S7.~~ **Closed by S6/S7.** `ParkedFleetView` is gone; the seam's
implementors are `Outpost::ReplicatedWorldView` (548 lines — `BuildScene`, `PreCheck`,
`SolvePreview`, `EncodeOrder`, `ApplySnapshot`, `ReasonText`) and `UniverseSimulation` in
`Main.cpp`, both injected by the composition root. The header keeps a tombstone naming what it
replaced, which is the right way to retire a placeholder.
`ClientApp` calling `BuildScene` every frame is still not covered by a test: that needs a
device and a window, and remains a manual checkpoint — discharged with the play test on
2026-08-19.

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
**Outstanding:** ~~nothing replicates. The world moves and no one can see it until
`EmitSnapshot` and the snapshot format land in S7 — which is also when the server gets a
scripted patrol to give these ships somewhere to go.~~ **Closed by S7**, exactly as written:
`Game::WriteSnapshot` fills the datagram, `ServerHost::BroadcastSnapshot` fans it out per
session per tick, and the client applies it through the seam into `ReplicatedView`. The
scripted patrol arrived with it, in `UniverseSimulation::AdvanceTick`.

### S7 — Snapshots over the wire → moving ships on screen
`EmitSnapshot` (full, quantised) → datagram → `ReplicatedView.ApplySnapshot` →
`SnapshotBuffer` (interp delay 2 ticks, extrapolation ≤ 250 ms → STALE flag) → Extract →
instances. Server runs a scripted patrol so ships move without input.
**Accept:** motion visually smooth at 144 Hz render / 20 Hz snapshots; induced 400 ms sim-
thread stall (debug key) shows extrapolate-then-freeze, recovers clean; `GameLogicTests`
wire round-trip: emit→bytes→apply equals quantised source; snapshot ≤ 1,152 B at 41 ships
(static assert + runtime check).
**Built ✅ (code):**
`Snapshot.h/.cpp` — full quantised snapshots, no deltas, so loss is not a case to handle: any
snapshot completely replaces the last. **The ship record is `Neuron::EntityRecord`, not a type
of ours** — ADR-004 §6 specifies exactly the twenty bytes NeuronCore already defines, and
declaring a matching `ShipRecord` here would be two layouts to keep in step, one of which would
eventually lose. GameLogic owns the *meaning* (`typeId` is a `HullClass`, the gauges are hull
and shield); the engine moves the bytes and reads none of them.
`SchemaHash.h` — the game's wire schema, covering the field layout **and the quantisation
constants**. Two builds agreeing on every field but disagreeing about centimetres versus
millimetres would pass a layout check and then place ships ten metres apart.
`ReplicatedView.h/.cpp` — the client's quantised shadow: a short history, interpolation between
the two snapshots bracketing the render tick, extrapolation capped at 250 ms, then a freeze and
a flag. Headings interpolate the short way round the circle.
`SnapshotBuffer.h/.cpp` (NeuronClient) — the clock. Slew-limited server-time estimate, render
tick two ticks behind, staleness, and the drift row the debug HUD reads.
`ServerHost::BroadcastSnapshot`, `ClientConnection`'s snapshot receipt, and
`Outpost/ReplicatedWorldView` — the chain, end to end.

**The placeholder is gone, exactly as its own comment promised.** `BuildParkedFleet`,
`ParkedFleetDesc`, `AddScenery`, `ScenePlacement` and `ParkedFleetView` are deleted, and their
seven tests with them. Keeping the tests would have meant keeping the placeholder alive to be
tested. **The station is now a ship**: the server spawns it as a `Structure` — zero speed, zero
turn rate (ADR-005 §1) — and it replicates through the same twenty bytes as everything else.
One path instead of two, and a station the selection and targeting code will get for free.

**One seam change, and it is the interesting one.** `WorldView::ApplySnapshot` took a tick as a
parameter and now *returns* one. The engine frames and orders the payload and does not look
inside, so it cannot know which tick the bytes describe — only the game can read that. Passing
a tick in meant the engine supplying a number it had guessed from somewhere else (the last
`Pong`), and the clock estimate would then be built on a value drifting from the payload it is
supposed to time. Putting the tick in the framing as well would fix that and create two copies
of one number — the arrangement S5b already refused for the content hash.

**Verified:** `GameLogicTests` 32 → 44, `NeuronClientTests` 64 → 73, `NeuronServerTests` 6 → 7,
195 across the four suites. The round trip is asserted in *integers*, because integers are what
crossed the wire — a metre-space comparison would have to allow for float representation on top of
the quantiser and would then pass on a bug that shifted a ship by a centimetre. Smoothness is
asserted as a bound on the per-frame step: at seven frames per snapshot, a view that snapped to
each arrival would take one tick-sized step every seventh frame, which is seven times too far. The
stall is a timing scenario played through a fake clock — 400 ms of nothing, then recovery — and
the slew absorbs it without a single snap. Jitter of ±15 ms lands within 2 % of a steady step
instead of reaching the screen. 41 ships is 836 bytes of the 1,150 available.

`NeuronServerTests` gained the one seam nothing had been watching: a client joins over a real
loopback socket and waits for two snapshots, then checks the payload for the simulation's own
marker rather than for its length — an engine sending eight bytes of its own devising would
pass a length check. That gap was found by a build failure, not by a test: `MakeStartingWorld`
called `SpawnStations` before it was defined, and while fixing the one-line ordering slip it
became clear `BroadcastSnapshot` had encoding tests on one side of it and clock tests on the
other and nothing at all in between.
**Outstanding:** ~~the visual half. That motion *looks* smooth at 144 Hz, and that the induced
stall reads as extrapolate-then-freeze rather than as a stutter, are judgements no test here
makes — they need a GPU. The numbers say both hold.~~
**Smoothness confirmed by the owner, 2026-08-18.** Interpolation at 144 Hz render over 20 Hz
snapshots reads as motion, which is the claim `SnapshotBuffer` was built to make and the one
its unit tests could only make in numbers. ~~Still owed: the **induced 400 ms stall** reading
as extrapolate-then-freeze rather than as a stutter — a separate gesture, and it needs the
debug key that triggers it.~~
**Closed 2026-08-19, and the debug key is the reason it could be.** The gesture arrived as
**F10** (S14's notes carry it): it cuts the snapshot feed inside `PollNetwork` while leaving
the link up, so the extrapolate-then-freeze path runs on a loopback session that never stalls
on its own. Signed off with the rest of the play test that day.

**Read the closure precisely, because F10 is not literally what this row asked for.** The
criterion said an induced *400 ms sim* stall; F10 is an **indefinite, client-side feed cut**.
From the client's own side those are the same event — no snapshot arrives, the view extrapolates
to `MAX_EXTRAPOLATION_TICKS`, freezes, marks stale, and snaps clean on the next one — which is
the whole of what this row was ever able to observe. What it does *not* reproduce is a server
that stopped ticking, and it crosses the 250 ms extrapolation cap by how long the toggle is
held rather than by a bounded stall. **A sim-side stall injector is still unwritten**, and the
day something needs to test the *authority* pausing rather than the feed going quiet, that is
the gap to fill.

Worth keeping the shape of this entry regardless: the item sat open for seven slices not
because it was hard but because **nothing could produce the condition it described**, and an
acceptance criterion no one can stage is one that quietly never closes.

### S7a — Shaders are built, not loaded *(out of order, at the owner's request)*
Owner directive: split each shader into a vertex and a pixel file, move them into
`Outpost/Shaders`, and pre-compile them to headers under `Outpost/CompiledShaders` with the
variable name `g_p<shader>`.
**Built ✅ (code):**
`Outpost/Shaders/OpaqueVS.hlsl` `OpaquePS.hlsl` `NebulaVS.hlsl` `NebulaPS.hlsl`, compiled by
`fxc` as part of `Outpost.vcxproj` (`ShaderModel` 5.1, warnings as errors, `/Od /Zi` in Debug —
the same flags `D3DCompileFromFile` was being handed) into `g_pOpaqueVS` and friends.
*Superseded 2026-08-19 (ADR-018 D12): the compiler is **`dxc` at SM 6.7 in both
configurations**. What this line recorded had already stopped being true without a decision —
Debug acquired per-file SM 6.7 overrides (so it was dxc), while Release kept 5.1 and had never
once been compiled. The setting now lives once per configuration.*
`Outpost/ShaderTable.h/.cpp` — the one translation unit that includes the generated headers,
and the only place their names appear. The arrays have internal linkage, so a second includer
would put a second copy of every shader in the binary.

**The split needed a third kind of file.** Two stages that used to be one file both declare
`VertexOutput`, and the two declarations are what links them: a field added to one copy and not
the other is a mismatch at PSO creation with a message naming neither file. So the shared
declarations moved to `Opaque.hlsli`, `Nebula.hlsli` and `PassConstants.hlsli` rather than being
copied — four copies of the `PassConstants` layout was the alternative, and there were already
two with a comment asking whoever edited one to remember the other.

**The engine stopped knowing where shaders come from.** `GpuPipelines::Create` took a directory
and called `D3DCompileFromFile`; it takes `PipelineShaders` — four spans of bytes — and the
composition root supplies them. This is not incidental to the move: `NeuronClient` may not
include a header out of `Outpost/`, because that is the executable and the dependency points
the other way (ADR-014). It is the same shape S5c gave the world, and it deletes the
`d3dcompiler` dependency, `ToWide`, the error-blob printing and the `shaderDirectory` config key
along the way.

**What the trade actually is.** A shader edit is a rebuild now rather than a restart, which is
the cost. In exchange a broken shader fails the build, in CI, on a machine with no GPU — where
it used to fail at boot on a machine with one, which is a much later and much narrower place to
find out. The `content.shaderDirectory` key is gone from `Outpost.json`; ADR-012's parser warns
on unknown keys, so a stale user layer says so rather than being quietly ignored.
**Verified:** the four suites still pass unchanged (195). The split itself is checked by
expanding the includes and comparing declaration-for-declaration against the files as they were
— every declaration preserved, none added, one entry point per file. There is no HLSL compiler
outside Windows, so that textual check plus CI's HLSL-compiler run is the whole of it.
**Outstanding:** ~~nothing renders differently, and that is the claim a GPU has to confirm. The
bytes are compiled from the same source with the same flags to the same shader model, so a
visible difference would be a surprise rather than a risk — but "should be identical" is not
"was identical", and only a frame on screen settles it.~~
**Closed 2026-08-19, and worth being exact about what closed.** The frame is *correct* — the
play test looked at hulls, overlays and HUD drawn by the compiled-in bytes and accepted all of
it. What was never going to be provable after the fact is *identity with the loaded path*: that
path no longer exists to compare against, so "was identical" stopped being a question the day
the loader was deleted. Correctness is the claim that mattered and it is the one that holds.

### S8 — Picking, selection, world-space overlay
Ray∩plane picking; click / shift-click / box-select; OverlayWorld pass: selection ellipses
(2:1 on plane, screen-floor clamp, depth-test vs hulls), hull/shield bars.
**Accept:** `NeuronClientTests` picking math (point/box vs known layouts, floor clamp cases);
manual: rings occlude behind a Carrier hull but bars never do (`overlay-pass.png` rule).
**Built ✅ (code, picking half):**
`Picking.h/.cpp` — `PlaneToNdc` (the inverse of `PlaneMappingForNdc`), point pick, box pick.
There is no ray: the projection is orthographic and the world is a plane, so a pixel maps to a
plane point through an affine transform and picking is a distance test against circles. That is
what ADR-006 §2 bought by fixing the camera as ortho, and it is why the accept criterion is a
unit test rather than a screenshot.
`Selection.h/.cpp` — the set of selected ids, and the press/move/release state machine that
turns a gesture into a click or a box. Ids, not indices: ships arrive re-sorted every frame and
a despawn shifts everything after it, so an index would select a different ship next frame.
`IsoCamera::ScreenFloorMetres` — ADR-006 §11's screen floor. Divided by the elevation sine,
because a plane circle draws as a 2:1 ellipse and guaranteeing the *tight* screen direction is
what a floor is for; the cost is being twice as generous horizontally, which is a floor being a
floor.
`RenderScene::pickTargets` — the frame's ships as circles, filled by the same `BuildScene` call
that fills the instances. Not derived from `InstanceRecord`: that is a vertex stream, sorted by
class for the draw, carrying a render classId rather than an identity and no room for a radius.
The overlay will read this array too, so the ring and the hit test agree by construction.

**One seam question, answered the way S5c answered its own.** Picking needs a ship's identity
and its class radius, and neither is the engine's to know. The game supplies them as
`{opaque id, plane point, radius}` and the engine does the arithmetic — the same division that
puts interpolation behind `BuildScene` and validation behind `PreCheck`.

**Verified:** `NeuronClientTests` 73 → 102. The strongest is the inverse round trip: plane
points pushed through the real camera's `PlaneMappingForNdc` and mapped back, across five yaws,
three zooms and two foci, because a hand-derived 2x2 inverse is exactly the kind of thing that
reads correctly with one sign wrong — which is how ADR-006 §3a's handedness defect survived
review. Each behaviour was then mutation-tested: flipping that sign, taking the first pick
instead of the nearest, making shift-click replace instead of toggle, and widening the box to
overlap rather than centre each fail exactly the test named for it and nothing else.
**Built ✅ (code, overlay half):**
`OverlayMark.h/.cpp` — which marks exist and where, on the CPU. One instanced quad per mark,
built from `SV_VertexID`, with the pixel shader deciding what is inside it: a ring is a distance
test and a bar is a fill fraction. Adding a mark type is a branch, not a mesh.
`Outpost/Shaders/OverlayVS.hlsl` `OverlayPS.hlsl` `Overlay.hlsli`, and `FrameConstants.hlsli`
split out of `Opaque.hlsli` — the overlay reads `g_viewProjection` and nothing else from b0, and
a shader declaring its own copy of that block to reach one field would be a second copy of two
array sizes free to drift.
`GpuPipelines::CreateOverlayPipelines`, `OverlayWorldPass` — one upload, two draws.

**Two pipelines, one shader pair, and the difference is the acceptance criterion.** A selection
ring lies on the plane, so its quad is built in plane metres and pushed through the
view-projection — which is what makes it foreshorten into the 2:1 ellipse ADR-006 §4 fixed the
elevation to produce, with no ellipse maths anywhere — and it depth-tests, so a ring behind a
Carrier is occluded by it. A bar is anchored in clip space and then offset in *pixels*, so it
stays the same size at every zoom, and its pipeline has depth disabled entirely: a health readout
that hides behind the thing it describes is not a readout. That is `overlay-pass.png`'s rule, and
it is a property of the pipeline rather than the shader — which is why `OverlayMarkList` keeps
its rings contiguous and the pass draws two ranges of one upload.

**A misnomer fixed on the way past.** `ReplicatedShip::hullPercent` and `shieldPercent` carry
`EntityRecord`'s gauges verbatim, which are 0–255. A ship spawns at 255, and a reader who
believed the name would divide by a hundred and put every bar two and a half times past its own
end. They are `hullGauge`/`shieldGauge` now, with the range written down.

**Verified:** `NeuronClientTests` 102 → 116. Ring radius against the screen clamp in both
directions, gauge 255 mapping to a full bar rather than 2.55 of one, the shield bar clearing the
hull bar by exactly the configured gap, rings preceding bars so the contiguous split holds, and a
selected ship that has despawned drawing nothing. Mutation-tested again: scaling the gauge by 255
instead of 257, and clamping the ring with `min` instead of `max`, each fail exactly the tests
named for them.
**Outstanding:** ~~the visual half, and it is the whole acceptance criterion.~~
**Confirmed by the owner, 2026-08-18: rings occlude behind a Carrier hull and bars never do.**
That is `overlay-pass.png`'s rule holding on a real frame, and it closes the one thing S8 was
accepted without. The depth bias on the ring pipeline (`-100`, slope-scaled `-1`) was the
classic decal pair chosen from the textbook and flagged here as a guess; it is a measurement
now. Worth keeping the note, because the *next* plane-lying kind to join that pass inherits
those numbers without re-earning them — S9's order footprints and station ticks already have.

The drag rectangle is deliberately not here: it is a screen-space quad, which ADR-006 §10 puts
in the Ui pass, and it arrives with the rest of that pass in S11.

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
**Built ✅ (code, GameLogic half):**
`Orders.h/.cpp` — the vocabulary: kinds, formations, queue modes, the reason enum that crosses
the wire, and `OrderSubmit` with fixed storage so decoding a datagram on the tick thread
allocates nothing. **Legs are wire-quantised** — centimetres and turns/65536, not metres and
radians — because ADR-005 §4's parity rule says validation consumes quantised values only, and a
struct that stored metres would make the mistake available.
`Validate.h/.cpp` — `ValidateOrder`, pure, over a `ValidationView` that is the *intersection* of
what the two sides have. The client has ids off a snapshot and no `World`; if this took a
`World` the client could not call it and the parity claim would be about two different
functions. The order of the checks is part of the contract: an order failing two rules has to
fail the same one on both machines, or the player reads a different explanation depending on
which answered.
`Formation.h/.cpp` — the Line solve, centred on the anchor so a single-ship move lands exactly
where the player pointed, spaced by the largest member's class, and **assigned by ascending
ShipId**. That last is not tidiness: the client's selection and the server's dense table hold
the same ships in different orders, and a solve that followed array order would put the preview
one station out from where the fleet actually goes.
`World` — the `OrderGroup` table, `SubmitOrder`, `IngestOrders`, `GroupAdvance`, and a leg
timeout so one ship that cannot move never wedges the fleet behind it. `Guidance` still carries
the resolved station, so `Steering` never learns that groups exist — which is what S6 built it
for.

**`ScriptedMove` is gone, as its own comment promised.** The replay harness feeds real
`OrderSubmit`s through the real validation now, which is strictly stronger. Two S6 tests changed
their answers and both changes are the point: an absurd target is **refused** rather than
clamped, and an order naming a ship that does not exist is **refused whole** rather than applied
to the ships that do. A partial application would succeed on the server and fail locally in a
way no reason code describes.

**A defect found on the way, in NeuronCore.** `MetresToCentimetres` cast a float straight to
`int32`. Beyond ±21,474 km that is undefined behaviour, and where it did not trap it wrapped — a
target 10,000,000 km east arriving as one somewhere west, small enough for the bounds check to
wave through. Reachable from any client that sends a large coordinate, so a validation hole
rather than a rounding curiosity. It saturates now, and NaN saturates to the low end where
validation refuses it.

**Verified:** `GameLogicTests` 44 → 72. Parity is asserted across a six-case matrix run through
both a server view (its own tables) and a client view (the same ids in snapshot order), with the
matrix checked for being neither all-accepted nor all-refused. Mutation-tested: reordering the
validation checks, dropping the formation's sort by id, and taking the smallest spacing instead
of the largest each fail exactly the test named for them — **and one mutation caught a bad
test**. Deleting the group fold from the world hash changed nothing, because the hash test's two
worlds also differed in `Guidance`, which has been hashed since S6. The replacement flies both
worlds down the same leg with identical ship state and a different queued plan, so only the
group table can carry the difference.
**Built ✅ (code, the wire):**
`WireType::OrderSubmit` and `OrderAck` in NeuronCore. The submission is **opaque**, exactly as
`Snapshot` is: the engine frames it and never parses it, because an order is game semantics and
an engine that could read one would have learned what a move is (ADR-004 ruling 4). The ack
*is* a struct, because all four of its fields are numbers this library defines.
`OrderMessages.h/.cpp` — the layout ADR-004 §7 states, and the boundary a hostile payload stops
at: a ship count past the cap is refused before a single id is read, rather than after the loop
has run sixty-five thousand times against an underflowing reader.
Snapshot order-state records and a real `lastOrderSeqProcessed`, with the order area **reserved**
so the ship cap is a constant rather than a function of how busy the player has been.
`ServerHost` routes `OrderSubmit` to the simulation and acks on the **control** channel — a lost
refusal leaves a ghost on screen forever and no later snapshot corrects it.
`Outpost`'s `ApplyOrderBytes` — decode, validate, queue, translate back.

**One field had to travel the wrong way, and that is the interesting part.** The ack echoes the
client's `orderSeq`, which lives *inside* the payload the engine must not parse. An engine that
dug four bytes out to fill in the ack would have started reading game semantics for the sake of
one field. So `Neuron::OrderVerdict` carries `orderSeq` back *out* of the game: the game already
parsed it, and the engine echoes what it is handed.

**Orders truncate where ships are refused.** A missing ship record reads as a despawn and is
unrecoverable; a missing order record costs one frame of ETA, and the header's high-water mark
still promotes the ghost. Asserted both ways.

**Verified:** `GameLogicTests` 72 → 82, `NeuronCoreTests` +2, `NeuronServerTests` 7 → 10. The
acceptance criterion — submit → ack → state in the next snapshot against a headless host over a
real loopback socket — is a test, and the order format it uses is one the *test* invented. That
is the strongest available demonstration that the engine assumes no layout: a simulation meaning
something no game means still round-trips, and its refusal reason (4242, a number no game uses)
arrives intact because nothing in between looked at it. A payload from a connection that never
completed the handshake is dropped rather than acked, and never reaches the simulation.
**Built ✅ (code, the client half):**
`OrderPuck.h/.cpp` — the right-drag gesture. **Press names the place, the drag names the
arrival facing, release commits.** The print draws a touch gesture where the puck follows one
finger and a second finger twists the footprint, and is explicit that the second degree of
freedom is only free because planar movement bought it. A mouse has one pointer, so the two
decisions are sequenced rather than concurrent — anchor-then-drag keeps both, where dragging
the puck and taking the facing from elsewhere would have thrown the saving away instead of
spending it. Below a 12-pixel slop the gesture reports *no* facing rather than a facing of
zero, and the caller substitutes the obvious one: the way the fleet is already travelling.
`OrderGhost.h/.cpp` — PENDING → UNDER WAY → REJECTED, with the print's 150 ms bounce. **A
locally refused order gets a ghost too**, which is the whole point of the pre-check: §4's
BounceParity gate says a local refusal must be indistinguishable from a remote one, and a
local refusal that was only a log line would be trivially distinguishable.
`OverlayMark` — two new kinds, `OrderFootprint` and `OrderStation`, and the enum renumbered
so plane-lying kinds sort below screen-facing ones. Ghost marks are *inserted* at the
ring/bar boundary rather than appended: both lie on the plane, and one appended past the
split would be drawn in the half that never depth-tests, where a footprint under a Carrier
would refuse to be occluded by it.
`ReplicatedWorldView` — the real `PreCheck`, `SolvePreview` and `EncodeOrder`, all three
through **one** intent-to-`OrderSubmit` conversion. Two copies would be two roundings, and
ADR-005 §4's parity rule is exactly that the numbers validated are the numbers sent.
`ReplicatedView` keeps the snapshot's order records, and `PollOrderFeedback` carries them
across the seam as six numbers per order.
`ClientConnection::SendOrder` on the **control** channel, and `OrderAck` drained as
`OrderVerdict` — the same type the local pre-check produces, so a ghost cannot tell where
its answer came from.

**Three seam calls were added, and each replaces a guess.** `DefaultOrder` because a client
filling in `kind` itself would be choosing game semantics, and zero meaning Move-in-Line is a
coincidence of two enumerations rather than an agreement. `PollOrderFeedback` because the
snapshot's order area is the promotion path that survives a lost ack. `ReasonText` because
the bounce toast has to say *why*, and the code is a number the engine cannot read — so the
words come from the side that assigned it, which is also what keeps a local and a remote
refusal reading identically.

**The client's `queuedLegs` is zero, knowingly.** The server resolves it from the group the
first named ship belongs to; a snapshot order record carries a member *count* and not the
members, so the client cannot. Reporting zero means an append that would fill the queue
passes locally and is refused a round trip later — ADR-005 §4's designed case. The other
direction is worse: a client guessing high would locally refuse an order the server would
have taken, and no amount of waiting gets the player past that. Fixing it properly costs two
bytes per member per order in a 1,150-byte datagram, to make instant a refusal that is
already correct. Not paid.

**A defect found on the way, in S8b's own tuning.** `OverlayTuning`'s selection ring and
shield bar were written `0xAARRGGBB` out of habit and are read `R8G8B8A8_UNORM`, so the ring
rendered amber and the shield bar blue. Neither had ever been looked at — S8b said in writing
that its visual half was outstanding because a depth state needs a GPU, and a swapped colour
needs exactly the same frame. The green hull bar survived only because `0x50e050` is a
palindrome in bytes.

**Verified:** `NeuronClientTests` 116 → 163, `GameLogicTests` 82 → 85. Mutation-tested:
taking the drag's angle in pixels instead of on the plane, appending ghost marks past the
ring/bar split, retiring a settled pending ghost with no grace, clamping the footprint with
`min` instead of `max`, and letting a repeated refusal restart the bounce each fail exactly
the tests named for them — **and one mutation caught a bad test again**. Replacing the order
area *before* the staleness check changed nothing, because the test asserted on the header's
high-water mark, which the frame ring already protects. It asserts on the records now.

**First frame on a screen, and it found three things** (2026-08-18, owner ran it). This is the
run R1 has been owed since S5, and it earned its keep immediately:

1. **The ring's thickness grew with its radius.** `RingAlpha` took
   `max(fwidth(distance) * 1.2, 0.03)`. `fwidth` of a normalised distance is about
   `1 / radiusInPixels`, so the first term is a constant *screen* thickness — correct — and the
   floor is a constant *fraction of the radius*, which past about forty pixels overtakes it. A
   700-pixel footprint came out as a forty-pixel green band. The floor's comment said it kept
   the ring from disappearing when the derivative is tiny; a tiny derivative is exactly the
   large-ring case where `fwidth` is already right. It is an epsilon against zero now.
2. **The puck was sized to circumscribe the formation.** `max(extentMetres, floor)`, and the
   extent of a Line is *half its length* — so eleven ships drew a circle touching the fleet at
   two points and spanning the viewport. The station ticks are the footprint and always were;
   the ring marks **where the player pointed**, so it is a fixed 22 px. An outline that hugs a
   formation is a polyline, which is draw list *(B)*.
3. **Every mesh buffer logged a D3D12 warning.** `CreateCommittedResource` was given
   `COPY_DEST` for a `DEFAULT`-heap buffer; buffers are effectively created in `COMMON`
   whatever is asked for, and the debug layer says so (#1328). Harmless — common-state
   promotion means the copy still works and the barrier after it is still valid — but it is
   eighteen lines of noise per boot, which is how a real message gets missed. `CreateBuffer`
   now derives the state from the heap type, so the choice cannot be made wrongly again.

*The first two are one lesson.* Both are cases where a number that reads as a safety floor is
actually a size, and neither is visible in any device-free test: the mark builder's arithmetic
was right, the shader's arithmetic was right, and the *product* of the two was a green ellipse
across the screen. That is the third defect this slice found which only a frame could find,
after S8's byte-swapped colours.

**Outstanding:** ~~draw list *(B)* — the dashed lane from the fleet to the destination and the
per-leg ETA labels. A line between two points is not a quad around one and a label is text,
so both belong with the Ui pass (ADR-006 §10) and arrive with it in S11; the reason toast
is logged until then, and the ghost's own bounce is the part the player can already see.~~
**Closed by S11, as scheduled.** `GhostLane` builds the dashed polyline and its per-leg ETA
labels into the HUD's own `UiDrawList` — its header opens "ADR-006 §8's draw list *(B)*, at
last" — and `LaneTests` covers the dashing and the labels. The reason toast landed with it.
~~The acceptance criteria that need a running session — promotion ≤ 100 ms on screen, and a
deliberate out-of-bounds order bouncing identically local and remote — are a play test, not a
unit test.~~ **Both signed off by the owner 2026-08-19** with the rest of the M1 table.

## 🏁 M1 — first commanded fleet: **met**, 2026-08-19

~~**Every criterion a machine can check is green.** What is left is what no unit test can see —
three things that need a GPU, a window and a person, and one of them (the overlay's depth
behaviour) has been owed since S8.~~

**Every criterion is green, machine-checked and human alike.** The five manual rows below were
signed off by the owner on 2026-08-19, in the same session that closed the MVP — including the
induced stall, which had been unstageable until **F10** arrived to cut the feed.

The slices M1 rests on are S5–S9. Their acceptance criteria, and how each stands:

| Criterion | Slice | How it is verified | Status |
|---|---|---|---|
| OBJ parser: counts and submesh ranges vs the shipped meshes | S5 | `NeuronClientTests` | ✅ |
| Replay hash equality, 1,000 ticks with scripted orders | S6 | `GameLogicTests`, with a control that moves one order by a metre and requires every later checkpoint to notice | ✅ |
| Movement envelope: top speed, turn radius, arrival overshoot | S6 | `GameLogicTests` | ✅ |
| No clock, no unseeded RNG, no `UniversePos` in per-tick math | S6 | CI grep steps, not review | ✅ |
| Snapshot round-trip: emit → bytes → apply equals the quantised source | S7 | `GameLogicTests`, compared in **integers** rather than metres | ✅ |
| Snapshot ≤ 1,152 B at 41 ships | S7 | static assert + runtime check | ✅ |
| Picking: point and box against known layouts, floor-clamp cases | S8 | `NeuronClientTests` | ✅ |
| Session-level order flow: submit → ack → state in the next snapshot | S9 | `NeuronServerTests`, over a real loopback socket, against an order format the *test* invented | ✅ |
| Validation parity: identical verdict and reason on quantised inputs | S9 | `GameLogicTests`, a six-case matrix run through both a server view and a client view, checked for being neither all-accepted nor all-refused | ✅ |
| A refused order bounces rather than vanishing, local and remote alike | S9 | `NeuronClientTests` compares the two ghosts field by field at the same instant | ✅ |
| **Motion visually smooth at 144 Hz render / 20 Hz snapshots** | S7 | a person, at a machine with a GPU | ✅ **owner-validated 2026-08-18** |
| Induced 400 ms sim stall extrapolates, freezes, recovers clean | S7 | manual, with a debug key — **the key is F10** (S14), which is why this could finally be staged. It cuts the *feed*, not the sim: identical from the client's side, but a sim-side stall injector remains unwritten (see S7's note) | ✅ **owner-validated 2026-08-19**, with that caveat |
| Visual checkpoint vs `tactical-hud.png`; frame time < 2 ms at 41 instances | S5 | manual for the look; the budget is **measured** — the strip's RENDER row reads 1.10–1.62 ms on the 41-instance scene | ✅ **owner-validated 2026-08-19** |
| **Rings occlude behind a Carrier hull; bars never do** | S8 | manual — `overlay-pass.png`'s rule; the depth-bias pair (`-100`, slope-scaled `-1`) was a guess and is now a measurement | ✅ **owner-validated 2026-08-18** |
| Overlay marks are legible at fleet scale | S8, S9 | manual — **run once, 2026-08-18, and it failed**: ring thickness scaled with radius and the puck circumscribed the formation. Both fixed, and the second look was taken | ✅ **owner-validated 2026-08-19** |
| On-screen promotion ≤ 100 ms | S9 | manual | ✅ **owner-validated 2026-08-19** |
| The out-of-bounds bounce looks identical local vs server | S9 | manual — the code paths are asserted identical; whether a *person* can tell them apart is the actual criterion | ✅ **owner-validated 2026-08-19** |

**The frame has now been run, and it both found and closed things.** It found three defects no
device-free test could reach — two overlay colours byte-swapped since S8, a ring whose
thickness grew with its own radius, and a puck sized to circumscribe the formation rather than
mark a point — with every unit test involved passing throughout. It also **closed two criteria
that had been open since S7 and S8**: interpolated motion reads as motion at 144 Hz over 20 Hz
snapshots, and rings occlude behind a Carrier hull while bars never do, which promotes the ring
pipeline's depth bias from a guess to a measurement.

That is the argument for the manual pass in one paragraph: it is not polish, it is the only
instrument that covers a whole category of defect *and* the only one that can retire a
criterion no number can settle. Its cost was measured in the three slices that shipped before
anyone looked.

**The second pass, 2026-08-19, closed the remaining five** — and made the same argument again,
harder. It began by finding a deadlock that made the session undemonstrable at all
(`QuicTransport::Poll` holding its lock across a blocking `GetParam`; S14's notes carry the
mechanism), which 477 tests and a green headless `selfTest` had run straight through. Four of
the five rows it then retired had been waiting on nothing but a person; the fifth, the induced
stall, had been waiting on a **key that did not exist**. Both are the same lesson from
different ends: a criterion is only as closeable as the instrument that stages it.


### S10 — Formations & the real footprint
`SolveFormation` for Line/Wedge/Claw; puck footprint preview = the same solve (one tick per
ship); arrival facing (drag-perpendicular or wheel while dragging); `GroupAdvance` leg
completion (tolerance + straggler timeout) and re-solve per leg.
**Accept:** `GameLogicTests` formation geometry (spacing from largest class, station count,
stable id→station assignment) + "preview equals outcome" (client-solve stations == server
final stations for same quantised inputs); fleet of 12 arrives in Claw matching the print's
footprint pattern.

**S9 took most of this.** Line was solved, `GroupAdvance` and the leg timeout were in `World`,
the arrival facing was the puck's drag, and the footprint preview already called
`SolveFormation` through the seam — so "preview equals outcome" became structural rather than a
test to write: the same function, the same quantised leg, the same sort by ascending `ShipId`.
The per-leg re-solve turned out to be there too; `ApplyLeg` runs on every leg advance.

**Built ✅:**
`Formation.cpp` — the **Wedge** and the **Claw**, and the Line rewritten alongside them so all
three are one function over two numbers (how far right of the anchor, how far ahead of it)
rather than three copies of the basis. Two properties hold across all three and both are
asserted rather than argued:

- **Every formation puts something on the point the player clicked.** A Line centres its rank
  there, a Wedge its tip, a Claw the middle of its arc. So a single-ship order lands exactly
  where they pointed whichever formation is selected, and the puck never marks a place the
  fleet then avoids.
- **Adjacent stations are exactly one spacing apart.** ADR-005 §2 buys the absence of any
  inter-ship avoidance with "stations don't overlap by construction", and that claim had been
  in the ADR since S6 and asserted nowhere. It is now a test over every count from 2 to 64 on a
  mixed-class fleet, and it holds at exactly 1.00× spacing for all three shapes.

**The Claw's radius comes from the chord, not the arc**, and that is the one number in this
slice that is easy to get plausibly wrong. Solving `R = arcLength / sweep` reads correctly and
puts adjacent ships a *chord* apart, which is shorter than the arc they were spaced along — at
low counts by seventeen per cent. Deriving `R` from the chord requirement instead
(`R = spacing / 2·sin(Δθ/2)`) makes adjacent separation exactly the spacing at every count. The
mutation is in the suite: it fails with Carriers 355.6 m apart where the table asked for 430.

`Validate.cpp` — all three accepted. The check stays rather than becoming a tautology, because
`formation` arrives as a byte off the wire and a value outside the enum is reachable from any
client.

**A formation has to be selectable, and that needed a seam call.** The command wheel is S11, so
S10 would otherwise have shipped two formations no player could reach. The client cannot cycle
`parameter` itself — counting from zero to two would be the client learning how many formations
this game has and that they are numbered contiguously — so `WorldView::OrderOptions(kind)`
returns the parameters a kind accepts with a name for each, and a key steps them. The command
wheel's formation sub-ring will be drawn from the same list, so this is the surface's data
arriving one slice early rather than a stand-in S11 has to take back. The **key is** the
stand-in, and it is named `CycleParameter` rather than `CycleFormation` for the same reason the
list is asked for rather than assumed.

**Verified:** `GameLogicTests` 85 → 91, `NeuronClientTests` 163 → 165. Mutation-tested: the
Claw's radius from arc length instead of chord, the Wedge's arms leading its tip instead of
trailing it, the Claw cupping backward, and the Wedge stepping a whole spacing on each axis
each fail exactly the tests named for them.

**Outstanding:** the print's own open question — *what the puck should do when a station lands
inside a gate or another fleet.* `puck-and-wheel.png` §6 lists it under OPEN and **it is still
open as of 2026-08-19**: it is a design decision nobody has taken, not a build item, and
nothing in this slice makes it worse, because stations that do not overlap each other can still
overlap the world. ~~And the acceptance criterion that needs a person: **a fleet of twelve
arriving in Claw matching the print's footprint pattern.**~~ **That one closed with the play
test.** The geometry was already measured and the separation exact; whether the crescent *reads*
as the sheet's crescent at tactical zoom was the frame-not-a-number part, and it was looked at.

### S11 — HUD v1
Glyph-quad Ui pass: top bar (ships, net RTT bars, sim indicator), fleet roster with wing rows
+ health strips, selection context bar (`N SHIPS : WING → FORMATION`), MOVE/FORMATION live +
ATTACK/STANCE/ABILITIES rendered disabled, order-pending indicator, minimal toast stack
(reject reasons).
**Accept:** HUD state is a pure function of replicated fields + local UI state (F10 spot-
check: kill the feed → HUD shows stale/empty, never invents); layout matches
`tactical-hud.png` zones; UI scale multiplier honoured at 0.8/1.0/1.6.

**Built ✅ (S11a — the pass and its device-free half):**
`UiDrawList.h/.cpp` — screen-space quads and *text runs*, in pixels with the origin top-left,
which is the space the prints are drawn in. **Text stays text until the last moment:** a run
carries a string and a position and the pass expands it against the atlas, so every decision
about what the HUD *says* is device-free and a test asserts words rather than quads. Text goes
into one pooled buffer with runs holding offsets, so a HUD rebuilt sixty times a second is not
sixty allocations a second.
`UiLayout.h/.cpp` — the print's zones from a viewport and a scale. Constants are pixels at 1.0
rather than fractions of the viewport, because a HUD that scaled with the window would shrink
its own text on a small screen — the opposite of what a scale control is for. The scale clamps
to the settings sheet's 0.8–1.6×, and a viewport too small for its own chrome collapses rather
than producing negative sizes.
`ToastStack.h/.cpp` — `alerts-and-toasts.png`, which turned out to be the most precisely
specified sheet in the corpus: five priorities, dwell per level, mandatory coalescing, a cap of
five that drops oldest first, Urgent in the top slot, Critical centre-top on its own surface,
and **no toast ever overlapping the context bar** — the bar is where orders are issued, and a
toast covering ATTACK mid-fight is a lost engagement.
`Ui.hlsli`/`UiVS.hlsl`/`UiPS.hlsl` and one pipeline. **Panels and glyphs are one instance
stream** differing by a flag: same quad, same space, same blend, and two pipelines would mean
two draws over one upload plus a sort to separate them — for a HUD whose natural build order is
panel, text, panel, text. The atlas is R8 coverage, so a glyph is its run's colour with the
coverage as alpha, which is what lets one bake serve every colour of text.

**The bounce toast S9 has owed since the ghost landed is now raised**, from both refusal paths
and through the same `ReasonText` call. The sheet is explicit that this is not a new component:
it is the same reason string the 150 ms ghost bounce is already showing, on the second of the
two surfaces one refusal owes. Keyed on the reason code, so a burst of out-of-bounds clicks is
one row with a count rather than five rows.

**Suppression is built and nothing drives it.** The sheet keys it on a replicated combat flag
the MVP has no combat to set. It is here anyway because queue-not-drop is a property of the
*type* — a held toast's dwell has to start when it is *shown*, and retrofitting that would mean
rewriting when every dwell begins. What is deliberately not built is the collapsed "6
SUPPRESSED" row, which is a rendering feature with no data behind it yet; `SuppressedCount` is
what it would draw from.

**Verified:** `NeuronClientTests` 165 → 188. Mutation-tested: a critical consuming a stack slot,
suppressed toasts dropped instead of queued, dwell measured from when a toast was raised rather
than shown, coalescing not restarting the dwell, and toasts anchored to the viewport instead of
the context bar each fail exactly the tests named for them.

**Built ✅ (S11b — the wing on the wire, the roster and the context bar):**
The decision S11a left open was: the print's rows are *wings*, `World` has a `WingId` per ship,
and `EntityRecord` did not replicate it — so either the wire grew a field or the roster showed
something other than what the print draws. **The wire grew a field, and it cost nothing:**
`EntityRecord`'s third byte was a `flags` that carried zero, and it is now `groupId`. The rename
is the whole change and it is the part worth keeping: `flags` promises the engine will
*interpret* the bits, `groupId` promises it will only carry them. Had it stayed `flags` the next
thing anyone wanted would have been a `FLAG_` constant in NeuronCore, which is a game rule
arriving in the engine one bit at a time.

`HudRoster.h` — one row: a name, a group id, two counts and two gauges. **The word for what a
row *is* never crosses the seam.** This game means a wing; another game on these libraries means
a squad or a convoy, and the pass that draws the panel does not change.

`WorldView::BuildRoster(selectedIds, outRows)` — the fifth seam call, and the one most worth
resisting. The engine has the replicated entities and the byte to group them by; aggregating in
the engine would have taken four lines and would have decided, in the engine, that groups are
worth showing, that they are *named*, that the two gauges **average** rather than take a
minimum, and that a group whose ships all died vanishes rather than showing as empty. Every one
of those is a design question about this game (ADR-014 §2c). `selectedCount` crosses for the
same reason: it is the one number the engine needs to highlight a row, and the alternative is
the engine matching selected ids against group membership — the aggregation it must not do.

The **context bar** reads `N SHIPS : WING > FORMATION` off the selection and the roster the game
just built, so it cannot name a wing the roster does not list. A selection spanning two wings
reads `MIXED` rather than naming the first or claiming both — a box-drag produces that case
routinely. The order-pending count sits at the right, read off the ghost list.

**Verified:** `GameLogicTests` 91 → 92, and the new one is the whole point: a wing survives
`emit → bytes → ReadSnapshot → SampleAt`, asserted after sampling rather than on the record,
because the record is only half the path. Mutation-tested — emit not reading the wing, apply
dropping it, and emit reading slot 0 for everyone each fail exactly that test. `BuildRoster`'s
aggregation is exercised by a scratch harness against a real world and a real snapshot; six
mutations (emitting from wing 0, bounding the emit loop on the table instead of the name list,
totalling instead of averaging, counting every ship as selected, ignoring the output span, and
dropping an emptied wing) each fail it.

**One thing was rewritten on the way in.** The accumulator started as a `std::vector` sized from
the name list — an allocation on every frame, inside the one function whose entire job is to
describe the frame, and next door to a comment claiming a HUD must not allocate to describe
itself. It is now a fixed 256-entry array indexed by `WingId` directly, which also retires the
bounds check: a `WingId` cannot index past a table with an entry per `WingId`. The emit loop
then had to bound on the *name list* rather than the table, and that pairing is load-bearing —
getting it wrong segfaults, which is how the harness caught it.

**Built ✅ (S11c — the drag rectangle and the ghost's lane):** the two items S8 and S9 both
deferred to this pass, and the second turned out to be the interesting one.

`UiRect::FromCorners` and six lines in `BuildHud` are the whole **drag rectangle**. A
screen-space quad and never a world mark, for the reason `PickBox` already gives: the box is
axis-aligned in *pixels* and an arbitrary parallelogram on the plane. The only arithmetic worth
a test is corner normalisation — the naive `{startX, startY, currentX - startX, ...}` gives a
drag up-and-left a negative width, and `AddQuad` declines a non-positive size, so the box would
simply not draw on half the gestures people make.

**The lane needed a new primitive, and ADR-006 had already said so.** "A dashed lane between
two points is not a quad around one" (§8a) reads as a filing decision and is actually a
requirement: the Ui instance was a top-left and a size, and a lane at 45° is neither. So the
pass grew an **oriented quad** — `rect` becomes centre and (length, thickness), swept along a
unit axis, under `UI_FLAG_ORIENTED`. The axis-aligned branch is left byte-identical rather than
rewritten as a special case of the sweep: every panel and glyph in the HUD goes through it, and
"the general form reduces to the old one" is a claim no test here can check. The instance grew
40 → 48 bytes with the axis **appended**, so every existing field keeps the offset the input
layout already declares. One primitive for a class rather than a special case for a feature —
`overlay-pass.png`'s mechanism-B list is polylines, engagement arcs and off-screen indicators,
all oriented. The alternative, square dots along the line, needs no GPU change at all and draws
a dotted route rather than a dashed one.

`GhostLane.h/.cpp` projects, clips and dashes. Three decisions worth keeping:
- **Clipped to the world zone for cost, not looks** — the panels already cover anything outside
  it. A target 40 km off with the camera zoomed in is a lane megapixels long, and dashing all
  of it is tens of thousands of quads nobody sees.
- **Dashes are phased off the lane's own origin, not the clip point.** The clip point moves as
  the camera pans, so the other way round makes every dash crawl along the lane while the
  player scrolls — motion that reads as the *order* doing something.
- **Screen-space, therefore never occluded**, which is the sheet's ruling: `overlay-pass.png`
  carries the order ghost as `MECH B · UIDRAWLIST` with no OCCLUDES badge, unlike the footprint
  beside it. A route a hull can hide is a route you cannot follow.

**The ETA is the game's answer, not the client's.** The label reads `18.4 km - ETA 41s`; the
engine can measure the kilometres from two points it already holds and can compute neither the
seconds nor the words. So `OrderPreview` gained `etaSeconds` and a `label` buffer, and
`GameLogic/Eta.h` gained the model — a trapezoid matching what `World::Integrate` actually
does, since its `ArrivalSpeed` is the braking leg of exactly that profile. Measured against the
simulation it predicts: **0.4 % worst on a straight run**, 4.7 % after a ninety-degree turn,
33 % for a Battleship turned right around on a short hop — and every one of them *optimistic*,
because a player told forty seconds who gets thirty is early and one told forty who gets
fifty-three planned around a number that was never achievable. It lives in GameLogic rather
than in the composition root because S12's authority needs the same function for the replicated
per-leg ETA, and one function is what stops the number changing when the source does.

`overlay-pass.png` §2 is what settles that this may be a client-side answer at all: mechanism
B's content "comes from the client pre-check ... **not from replication**".

**Verified:** `GameLogicTests` 93 → 101, `NeuronClientTests` 129 → 158. Mutation-tested — ten
on the lane and the oriented quad (an un-normalised axis, a top-left where the centre belongs,
a drag rectangle that keeps its sign, an unflipped y, dashes phased off the clip point, an
under-way lane drawn dashed, an unclipped lane, a distance measured in pixels, a bounce that
does not retract, a label drawn for an off-screen footprint) and three on the seam glue (the
fastest member's ETA instead of the slowest, distances from the anchor instead of each ship's
own station, a label that names the kind twice). Each fails exactly the test named for it.

**Built ✅ (S11d — the command row):** `MOVE | FORMATION | ATTACK | STANCE | ABILITIES`, with
the three commands this build has no content for drawn greyed and refused by the hit test.
**S11 is complete.**

**The row's words come from the game, and that is the whole design.** The tempting version is
five string literals in `ClientApp`, and it is wrong in the way ADR-014 exists to prevent:
those are this game's verbs compiled into a library meant to serve a second game with different
ones. **No CI rule would have caught it** — the engine-references-game check greps includes and
project references, and a string literal is neither. So `OrderKinds()` is the sixth seam call,
and `OrderKind` gained `Attack`, `Stance` and `Abilities` as **reserved** enumerators: nameable,
numbered, never submittable, exactly as `HullClass` holds Fighter and Cruiser (ADR-009 §6).
`ValidateOrder` already refused everything but Move, so the reserved three were refused the day
they were added — and there is a test that walks every kind and checks precisely that, because
"the validator happens to be strict enough" is not a property to leave unasserted.

`CommandRow.h/.cpp` is a **layout and a hit test in one file**, which is the point rather than
tidiness: a HUD that lays out in the renderer and hit-tests in the input handler is a HUD where
the thing you press is not the thing you see, and no test can catch it because the two never
meet. The row is built in `UpdateHud` — before `UpdateSelection` — and only *drawn* in
`BuildHud`, so the click and the quads come from one answer.

Three decisions worth keeping:
- **The parameter button is not a command.** `FORMATION` sits in the row beside the verbs and
  is a different kind of thing — the name of what the selected command varies by, with the
  current choice under it. It is placed immediately after the command it belongs to, so the
  pair reads as one control, and it is disabled when there is only one value: a button that
  visibly does nothing when pressed is worse than one that is visibly not for pressing.
- **A narrow row drops buttons rather than wrapping or shrinking them.** Reflowing is where a
  zone table becomes the layout engine R9 says this pass must not become.
- **A drag may only *begin* in the world zone.** Until now a press on the roster or the command
  row also started a box selection across the fleet underneath it — a bug that arrived with the
  panels in S11b and had nothing to catch it. Once begun a drag may leave freely: a selection
  that cancelled when it touched the ability rack would be worse than the bug.

**Verified:** `GameLogicTests` 101 → 105, `NeuronClientTests` 158 → 172. Nine mutations —
greyed commands dropped instead of drawn, the hit test returning disabled buttons, a parameter
button on every command rather than the selected one, a single-value parameter left live,
buttons shrunk instead of dropped, a parameter marked active, a reserved kind claiming content,
validation no longer refusing an unknown kind, and Attack growing a parameter name — each fails
exactly the test named for it. One of them segfaulted the first time rather than failing: the
tests dereferenced a lookup that had become null, so they now assert before dereferencing. A
suite has to survive the code being wrong, which is the situation it exists for.

**Four things were queued for this pass and all four are drawn.** They were listed at S9 so
S11 would be scoped against them rather than surprised by them, and the list is worth keeping
now that it is closed — the queue emptied without growing, which is what R9 was watching for:

- the **selection drag rectangle** (S8 deferred it: a screen-space quad, not a world mark) —
  S11c;
- the **bounce toast's reason string** (S9; the code and the text both crossed the seam through
  `WorldView::ReasonText` and only the drawing was missing) — S11a;
- the ghost's **dashed lane and per-leg ETA labels** (S9 — draw list *(B)*) — S11c;
- the **NET and tick readouts** the debug strip logged — S11a's top bar.

**None of them was a widget, and one of them cost a primitive.** The lane needed an oriented
quad, because the pass could place a rect and a lane at 45° is not one (ADR-006 §8c). That is
the shape to watch next: not a widget, but the pass's vocabulary growing an entry at a time.
The test of whether it was the right entry is that `overlay-pass.png`'s two remaining
mechanism-B classes — engagement arcs and off-screen indicators — are already expressible with
it.

### S12 — Order queue & ETAs
`queueMode=append` up to 4 legs; ghost polyline with per-leg ETA labels; `QueueFull` bounce;
ETA = remaining-distance/speed model server-side, replicated in order state.
**Accept:** wire-enforced 4-leg cap (5th bounces); ETA error < 10 % on straight runs;
queued-chain rendering matches `puck-and-wheel.png` §4.

**More of this slice was already standing than the line above implies.** S9 built
`MAX_ORDER_LEGS = 4`, the `QueueFull` refusal, the append path through `IngestOrders`, and the
per-leg re-solve. What it did not have was an authoritative ETA, and — as S12a found — a leg
timeout that let a long leg finish at all.

**Built ✅ (S12a — the authority's ETA, and two defects that blocked the queue):**

`OrderStateRecord` gained `u16 etaSeconds`: seconds until the group's current leg completes,
computed by the authority and replicated. The ghost prefers it the moment it exists and falls
back to `OrderPreview::etaSeconds` — the prediction made before sending — only while PENDING.
The record grew 12 → 14 bytes, and the comment saying a field here costs ships was right: the
fleet cap fell 47 → 45, still above the 41 the MVP fields.

**The model needed a moving start.** S11c's `TravelSeconds` is rest-to-rest, which is correct
for a *preview* of a fleet standing still and wrong for a *remaining* journey — it charges an
acceleration ramp the ships already paid, and the error grows as they arrive. Measured over
whole legs: rest-to-rest peaks at **44.8 %** error with a second left; `RemainingSeconds`, which
takes the speed already gained, peaks at **2.4 %**. The two are one profile — the rest-to-rest
name is the general one at zero speed — so there is nothing to keep in step.

Speed counts **along the way the ship is going**, not outright. Redirecting a fleet at cruise is
the ordinary case: its velocity is large and points entirely the wrong way, and crediting the
magnitude promises an arrival it is moving away from.

**The leg timeout was six times too short, and it blocked the queue.** `LEG_TIMEOUT_TICKS` was a
flat 600 — thirty seconds — documented as "long enough that a Battleship crossing the grid is
never cut short". A Battleship crossing the grid takes **182 seconds**; even four kilometres
takes 45. Every leg longer than half a minute ended by *timing out*. Nothing looked broken
because `Guidance` still held the station and the ships flew on — what the timeout ended was the
**order**, so the footprint and the ETA vanished mid-flight. With a queue it is not cosmetic:
leg two would begin while leg one was a sixth flown, and the fleet would skip to the last
waypoint.

The deadline is now the leg's own — three times its estimate plus a grace, which S12a's own ETA
made computable. It is set in `ApplyLeg`, which runs *every tick* to re-solve the formation, so
it is computed only when unset and cleared wherever a leg begins. Getting that wrong in either
direction is a live failure: recompute every tick and the deadline never arrives, so a group
that can never advance holds forever; forget to clear it and leg two inherits leg one's expired
deadline and gives up on the tick it starts. There is a test for each.

**Verified:** `GameLogicTests` 105 → 114. Eight mutations, each failing exactly the test named
for it. Two survived the first pass and both were the same weakness — every ETA test used a
single ship on a straight run, where speed-toward equals speed-outright and a station equals the
anchor. A redirected fleet and a wide Line separate them.

**Built ✅ (S12b — the append's identity, and the wire-enforced cap):**

`SubmitOrder` used to take the next server order id for an appending order, and `IngestOrders`
threw it away the moment the append landed on an existing group. So the ack named an order that
appeared in no snapshot ever, and the counter skipped a number per queued waypoint. An append is
now acked with `serverOrderId = 0` — already the verdict's "no order" — and the client learns the
real id from the next snapshot, a path `OnFeedback` already walked.

**It cannot be resolved any earlier, and that is worth writing down.** The group an append joins
is found at *ingest*. A Replace and an Append submitted between the same pair of ticks are both
pending when the second is validated, so at submit the Append can only see the group the Replace
is about to destroy — resolving early would append to a corpse.

**The worse half was the sequence.** The group kept the *original* order's `clientOrderSeq`, so
the appending order's sequence was reported by nothing: the client's high-water mark passed it,
`OnFeedback` read that as "decided and no longer running", and the ghost the player had just
created retired with no bounce and no promotion. The group is now named after the most recent
order that shaped it, which makes the newest ghost the one that gets promoted — and it is also
the ghost that knows about the whole queue.

**The queue mode stopped riding in `legIndex`.** It was smuggled as "1 means append", which
worked, and read as a leg number to everyone including the line that overwrote it two statements
later. `m_pending` now holds a `PendingOrder` — the group and its mode — and `WorldHash`'s
group fold was split so it can be reused per group.

**`queuedLegs = 0` in the client's pre-check was rechecked and kept.** `legCount` looks like the
missing number and is not: it is per *order record*, and the question is which record a
*selection* belongs to, still unanswerable from a member count. S12's criterion says
**wire**-enforced, and that is what this is — the fifth leg bounces from the authority with
`QueueFull`, through the same ack, the same 150 ms retraction and the same reason string as any
other refusal. ADR-005 §4's parity claim is that a local refusal and a remote one are
indistinguishable *to the player*, not that every refusal is local.

**Verified:** `GameLogicTests` 114 → 118. Four mutations — an append burning an id, the group
keeping the original sequence, the append fallback taking no id, and the queue mode ignored
entirely — each failing exactly the test named for it. The fallback one survived first time:
nothing covered an append with no group to join, which is reachable by holding the queue
modifier with nothing previously ordered and would have left a live group carrying the
"no order" id.

**Built ✅ (S12c — the queued chain):** one order is one ghost with a leg per waypoint, drawn as
a polyline with a per-leg ETA. **S12 is complete.**

**The merge happens on *acceptance*, not on send, and that one decision is most of the design.**
A queued waypoint is its own pending ghost from the moment it is sent — it has to be, or a
refusal would have nowhere to land (§4: an order in flight with no ghost is an order whose
refusal has nowhere to go). When the authority accepts it, it joins the chain and stops existing
on its own. When the authority refuses it — a fifth leg, `QueueFull` — it bounces alone and the
chain never notices, which is what `Refuse` already did with no special case for queues in it.
Merging on send would have had that refusal retract the four legs the player still had.

The chain then **takes the appending order's sequence**, exactly as `World::IngestOrders` makes
the group take it (S12b). Both sides name the plan after the most recent order that shaped it, so
the record the next snapshot carries matches the ghost that is left.

**The one thing the ghost predicts rather than replicates** is which chain an append joins:
"the plan whose first named ship is this one's", mirroring the authority's own rule. That is what
a ghost is for — the only client-side optimism in the game — and the authority's `legCount` is
what corrects it when the guess is wrong. The alternative was replicating the leg anchors, and
the budget refuses: four legs at even 4 bytes each is 16 bytes per order, 256 across the area,
and the fleet cap falls to 32 against the 41 the MVP fields.

Only the **last** waypoint retracts on a refusal, and only to the one before it.
`OrderGhost::RetractTowardMetres` is read by both the footprint ring and the lane, so they cannot
come home to two different places for the 150 ms the animation lasts — and for a single-leg
ghost it returns the fleet, which is S11c's picture unchanged.

The label gained a line: `MOVE - CLAW`, the whole plan's distance with the current leg's ETA,
and `3 LEGS` when there is a queue. The distance is walked leg by leg **on the plane**, and the
ETA of the leg the fleet is actually flying is the authority's (S12a) while the legs ahead keep
the game's prediction — the authority has not started them and has nothing measured to say.

**Verified:** `NeuronClientTests` 172 → 183. Five mutations — merging on send, finding the chain
by any live ghost rather than a shared ship, keeping the original sequence, growing past the
engine's buffer, and retracting to the fleet from a queued chain — each failing exactly the test
named for it.

### S13 — msquic behind the same interface ⚡ spike
`QuicTransport`: ALPN `opf/1`, in-memory self-signed cert (`CertCreateSelfSignCertificate` +
`CERTIFICATE_CONTEXT`), client `NO_CERTIFICATE_VALIDATION` (loopback), stream 0 = control,
DATAGRAM = state. QUIC replaces `UdpTransport` outright — there is no transport config knob
(owner directive: QUIC only); `UdpTransport` is deleted when this slice lands.
**Accept:** the *unmodified* game runs over QUIC on loopback; `selfTest` runs the full
handshake+order+snapshot loop; measured added latency < 1 ms
loopback. Friction findings feed Risk R3 disposition (stay Schannel vs flag OpenSSL flavour).

**Built ✅:** `QuicTransport.h/.cpp` behind the unchanged seam — msquic 2.6 (Schannel), ALPN
`opf/1`, client-opened stream 0 re-framed by a u16 length prefix as the control channel,
DATAGRAM frames as state, in-memory `CertCreateSelfSignCertificate` + `CERTIFICATE_CONTEXT`
over a persisted CNG key (Schannel reaches the key through the certificate's provider info,
and an ephemeral key has no container to name; the key is deleted with the transport),
`NO_CERTIFICATE_VALIDATION` on the client. `UdpTransport` deleted per the owner directive;
`ServerHost` and `ClientConnection` each changed one type name, which is the seam doing its
job.

**Verified:** the unmodified game runs windowed over QUIC on loopback — fleet patrolling past
tick 300, NET readout live, clean shutdown. `selfTest` now runs the whole loop rather than
stopping at the heartbeat: a snapshot decoded to ships, a real order for a real ship up the
reliable channel, the authority's ack back accepted — and it gates on latency: **transport
min RTT 0.305 ms** on loopback in a Debug build, under the 1 ms accept. (Min rather than
smoothed, because the smoothed figure is dominated by msquic's deliberate ~25 ms ack
batching, which is not latency the transport adds to the game's bytes; the seam's `Stats()`
now reports both.) All four suites pass — 453 tests, the transport suite retargeted to QUIC
plus a close-reason-crosses-the-wire test QUIC made expressible. One defect found during
bring-up by an existing test, which is what the suite is for: `ConnectionShutdown` abandons
queued stream data, so the build-mismatch refusal never left the server — `Close()` now
shuts the control stream down gracefully and defers the connection close until
`SEND_SHUTDOWN_COMPLETE` says the refusal was actually delivered. Friction findings are in
Risk R3; the disposition is **stay Schannel**.

### S14 — Debug strip, selftest, polish 🏁 **MVP**
Tier-1 counters strip (frame/GAME/EXTRACT/UI ms, net RTT/loss/jitter, snap age/drift ticks,
`tickOverrun`, drops) behind a toggle; `selfTest` aggregates: schema self-check,
transport handshake, replay determinism run, wire round-trips — exit-code CI gate; polish:
4× MSAA offscreen + resolve, cosmetic banking/hover from velocity, STALE marker visual.
**Accept:** MVP playable definition demonstrated end-to-end — select fleet, issue queued
formation moves, watch execution with status + feedback; `selfTest`
green on a GPU-less runner; counters strip numbers plausible vs `debug-hud.png` rows.

**Built ✅ (the strip):**
`DebugStrip.h/.cpp` — the Tier-1 strip, split device-free exactly as the HUD is:
`DebugStripHistory` accumulates the per-frame telemetry drain and **latches display values on
a quarter-second cadence** (a number changing 144 times a second is a flicker, not a
reading), and `BuildDebugStrip` emits panels and text runs into the HUD's own `UiDrawList` —
the strip is chrome drawn by the pass that already exists, costing no pipeline of its own.
Rows: FRAME with FPS and a p95 over the last 256 frames, the five stage budgets as bars
(over-budget is a colour, not a clamp — a bar pinned at full must still say *how* it got
there), LINK, SNAP with the drift row `SnapshotBuffer` has carried since S7, SIM (drawn only
when a Sim lane lives in this process — a row of zeros in `mode: "client"` would claim a
measurement of a machine this process cannot see), WORLD, and DROPS.

**The client became the collector ADR-007 §8 promised.** Until now nothing drained the
telemetry lanes in windowed mode — they filled, wrapped, and counted drops nobody read. The
drain now runs every frame whether or not the strip is visible, because a collector that only
ran while someone watched would report drops caused by nobody watching. The print's two
honesty rules are both structural: the strip's own cost is measured around its collection and
build and displayed as the title row's `dbg` figure, and the DROPS row carries the lanes' own
overflow counters — an instrument that quietly discards data under load produces confident
graphs of exactly the frames that went wrong.

**The toggle is a setting with a shortcut, not a gesture** (`debug-hud.png` §6):
`client.diagnostics.strip` (shipped `false`, user-layer writable — it is a preference), and
F1 flips the same bit at runtime. `tickOverrun` reaches the strip through the ordinary lane
mechanism: in host mode the Sim thread's lane lives in the same process and the collector
drains it like any other, so nothing crossed the transport seam to put it on screen.

**Built ✅ (the selfTest aggregate):** the S14 list, in the order that keeps a transport
failure from masking a determinism one — schema self-check (nonzero, stable, and the shipping
simulation states the same number `GameSchemaHash()` computes), an order round-tripped
byte-exactly, a snapshot round-tripped **emit → bytes → apply and compared in integers**
against the records read straight off the payload, and a replay-determinism run: the same
scripted 400 ticks twice with hash checkpoints at 200 and 400, plus the control that makes
agreement mean something — a third run with one target moved a metre must diverge. All in the
shipping binary on the machine actually running it, which is the one place a stray `/fp`
switch or a local compiler difference can be caught before it is a desync report.

CI runs it: a new workflow step launches `Outpost.exe` from a scratch directory whose
`Outpost.json` sets `mode: "headless"` + `selfTest: true`, waits on the process, and fails the
build on a nonzero exit code with the log tailed into the summary. One check changed meaning
for it: "no tick overran" became "ticks did not persistently overrun" (≤ 2, count logged),
because a shared runner is not a real-time system — the same argument S3's loose CI bound
already made, applied to the same loop.

**Built ✅ (polish):**
*4× MSAA offscreen + resolve.* `GpuSwapChain` owns the multisampled colour target and depth
buffer beside the back buffers whose size all three share; the world passes render there and
the frame loop resolves before the Ui pass draws single-sampled on the back buffer.
`GpuPassList::Record` split into `RecordWorld`/`RecordUi` so the resolve and its barriers sit
with the frame loop — barriers are not a pass, and neither is a resolve. The S2b-era
`client.renderer.msaa` key is finally read; an unsupported count falls back to 1 with a log
line. The resolve averages in gamma space (format `UNORM`, matching the typed back buffer)
rather than risking a typed-format mismatch — recorded in ADR-006 §12 with the trade stated.

*Cosmetic banking/hover.* The interesting discovery: ADR-006 §6 said "from replicated
velocity/heading only", and the sim's **no-strafing rule makes a slip angle identically
zero** — velocity is always along heading, so there is nothing to derive a roll from in one
sample. The bank therefore comes from the heading *rate* the client measures between its two
bracketing snapshots (`ReplicatedShip::headingRateRadiansPerSec`, zero while extrapolating —
a held heading is not a turn), normalised by the class's own turn-rate and speed limits in
`Game::CosmeticBankRadians` so a Battleship at its full slow rate banks as deliberately as an
Interceptor at its fast one. `InstanceRecord` grew its first field since S5 — `f32 bank`,
20 → 24 bytes, **appended** so every existing offset holds, the discipline `UiInstance::axis`
established. Hover is a per-class constant in the class table beside `pickRadiusMetres`,
cosmetic like it; the `SceneEntity` keeps the plane point, so rings, bars and picking never
learn about either (ADR-001 §2's "never pickable" held by construction).

*STALE marker.* The icon sheet's state marker (§4), as a new screen-facing overlay kind: a
dashed ring in the stale colour on **every** frozen ship, selected or not — staleness is a
fact about the feed, and the ship the player most needs warned about is the one they were not
looking at. Screen-facing because a readout about the feed must never hide behind the hull it
warns about; dashed because that is the sheet's convention for the unresolved. The top bar
gains a feed-level `STALE` chip beside NET when the whole world is frozen past the 250 ms cap,
and the strip's SNAP row spells it out with the age and drift numbers behind it.

**Verified:** 469 tests across the four suites, up from 453. The strip's two halves are
asserted separately — the history's latching, counter accumulation across windows, and the
absent-SIM rule as arithmetic; the builder as *words* (a stale feed says STALE in the caution
colour, no session reads `no session` rather than an invented zero, drops recolour their row,
every run stays inside the panel). The bank is mutation-shaped at its edges: full rate at
full speed is exactly the clamp, the sign flips with the turn, half speed halves it, a wild
rate cannot roll a ship onto its back, and a Structure (turn rate zero) never banks rather
than dividing by it. The heading rate is asserted against the two quantised headings read
straight off the wire, and an extrapolated sample reports no turn. The full selfTest was run
headless on this machine: **33 checks, exit 0** — the ten new ones green, transport min RTT
0.232 ms, mean tick period 50.130 ms, no overruns. The windowed client ran 18 s under the
debug layer with MSAA 4× and the strip enabled, and exited 0.

**Outstanding at landing, and closed 2026-08-19:** everything a person at a GPU must
judge. The MVP playable definition demonstrated end-to-end is a play test, not a unit test.
The strip's numbers being *plausible* against `debug-hud.png`'s rows needs eyes on a live
frame, as does everything this slice drew: whether 4× MSAA reads as smoothing (and whether
the gamma-space resolve's slightly darker edges matter on a near-black scene), whether the
bank's sign and magnitude read as a ship leaning *into* its turn rather than out of it,
whether the hover heights sit well under the rings, and whether the STALE marker earns its
place — which wanted the induced-stall debug key S7 had owed since it landed, because a
healthy loopback session never goes stale on its own.

**F10 is that key (2026-08-19).** `InputAction::ToggleFeedFreeze` cuts the snapshot feed
inside `PollNetwork`, dropping what arrives rather than queueing it: a sender that stopped is
what it reproduces, and a queue released on the way out would replay a burst of history the
world never lived through. The link stays up, so what runs is exactly the path a lost sender
takes — the top bar's STALE chip past the 250 ms cap, the SNAP row's age and drift in the
caution colour, and the dashed screen-facing marker on every hull, selected or not. It is
logged rather than drawn, because an on-screen "feed cut" badge would alter the very picture
it exists to let someone look at. F10 is a *system* key, so it arrives as `WM_SYSKEYDOWN` and
is answered there: left to `DefWindowProc` it opens the window menu and takes the keyboard
with it. `INPUT_ACTION_COUNT` gained a `static_assert` on the way past — `InputFrame`'s action
arrays are sized by it, so an action added without it writes past their end rather than
failing to build.

**And the play test found what 477 tests and a green `selfTest` could not: the client
deadlocked about a second after connecting.** `QuicTransport::Poll` held `m_lock` across a
connection-scoped `GetParam`, which msquic services by queueing to that connection's worker
and waiting — the same worker every callback in the file takes `m_lock` on. Main thread
holding the lock and waiting for the worker; worker inside a callback waiting for the lock.
The frame loop froze on its first frames after the handshake, the window stayed up showing
nothing, and the server timed the client out with `reason 2`. The file's own locking rules had
named this exact hazard for `ConnectionClose`/`ListenerClose`/`RegistrationClose` and had not
thought of `GetParam`. Fixed by reading the stats outside the lock, together with a
use-after-free the fix exposed: both send paths read `send->buffer.Length` *after* handing the
buffer to msquic, which may free it inline from `SEND_COMPLETE`, and had been returning
plausible numbers by luck.

## 🏁 MVP — **met**, play test signed off 2026-08-19

**Every criterion is green, the machine-checked and the human alike.** CI runs 499 tests
across the four suites *and* the aggregated `selfTest` in the shipping binary on a GPU-less
runner, on every push; and the definition itself — *select fleet, issue queued formation
moves, watch execution with status and feedback* — was demonstrated by the owner in a live
session, together with S14's visual half (strip plausibility, MSAA, banking/hover, the STALE
marker behind its new F10) and the manual items carried open from earlier slices.

None of those could move a test count, which is the argument the M1 section already made: the
manual pass is not polish, it is the only instrument that covers a whole category of defect.
Its price was known — the first frame anyone looked at found three defects every unit test had
passed over — and this milestone raised it. The session meant to *demonstrate* the MVP began
by finding a deadlock that made it undemonstrable: engine code that 477 tests and a green
`selfTest` ran straight through, in a client that froze one second in (the mechanism is in
S14's notes above). A green tick said the tree was ready. Only a person at a GPU could say
whether the game was, and the first thing they saw was that it was not.

**The qualification that stood here has been discharged.** The Debug leg of CI was
`continue-on-error` while R22 was open, so a green run certified **Release** only. That
deadlock was a strong candidate for R22's cause — it is in the file R22 names, in a call
`NeuronCoreTests`' loopback pump drives hard, and it is a race, which is the shape R22
reported — but R22's own hypothesis was a different mechanism in `Teardown`, and settling it
took green Debug runs rather than an argument. **They came**, the owner closed the row on
2026-08-20, and `continue-on-error` is out of the build job: a green tick certifies both
configurations again. Worth keeping from the episode: the deadlock was found by a person at a
GPU and confirmed by CI, in that order, and the fix was gated on the second rather than the
first — which is the only reason the closing note here is a record and not a hope.

**What follows the MVP is already moving.** The first post-MVP feature landed beside S14 and
merged with it: ship collision (ADR-015) — contact radii in the class table, braking and
deflection inside Steering, a fifth tick system (`Separate`) — recorded in that ADR rather
than as a slice here, because it belongs to no build order's sequence. **S15 below is built as
of 2026-08-19** and stays this document's own tail.

**The two phases now have their own plans and their own progress**, and this document is not
where to read it: the universe phase (ADR-016,
[Universe-Build-Order.md](Universe-Build-Order.md)) has U1, U2, U3a, U3b's sim half and U5's
pure half built; the station phase (ADR-017,
[Station-Build-Order.md](Station-Build-Order.md)) has all of T1 and T2's identity cluster.
Both plans carry a **Built** line per landed slice, which is where the detail lives.

**Two changes did land inside this document's territory, and are recorded here because
nothing else covers them.**

*The order table grew a lifetime.* A group that reaches `Done` now **lingers 30 ticks**
(`ORDER_DONE_LINGER_TICKS`) before it is retired, instead of leaving the table on the tick it
finishes. S12 built the ghost that retires on *seeing* `Done` in a snapshot (ADR-014 §2c —
order records exist only while an order does, so absence is the signal), and that reading is
safe only if the record outlives the event it reports; without the linger the ghost races the
snapshot at exactly the rate the link is slow. Two consequences worth having in one place:
`doneTick` is simulation state and folds into the hash — `RetirementReplaysExactly` pins it —
and snapshot order selection became **two passes, live groups first**, so when the sixteen-slot
cap bites the record that gets dropped is a corpse serving out its linger rather than the order
the player just gave. `World.cpp` split in the same work, with the order pipeline moving to
`WorldOrders.cpp`.

*S11's HUD caught up with its print.* The command row stopped being a fixed list of verbs and
became the game's own lists, asked once through `WorldView::OrderKinds`/`OrderOptions` and held
**per kind** rather than for the selected one, indexed by slot rather than by kind value. That
is what let `StanceId{Balanced, Aggressive, Evasive}` arrive as three words with **no engine
edit and no wire field** — ADR-014 §2c's argument tested by the game growing a parameter rather
than by a second game appearing. Six print details landed with it: the row keeps ATTACK second
with parameter chips deferred past the immediate verbs, only picker buttons carry the `▾`
caret, the `▥ MENU` chip and its stub list exist, the context bar counts the optimistic window
(`⏳ N ORDERS PENDING`), world-space gauge bars band on the same two thresholds the roster
strips use, and the selection ring became the own-fleet phosphor instead of the allied cyan it
had shipped as. One of them moved the renderer: **the ghost's lane now draws under the hulls**,
which took ADR-006's reserved pass list a second insertion (`UiWorld`, §1c) — a second `UiPass`
instance into the world target before `Opaque`, at the same price `Nebula` paid.

---

### S15 — Audio thin slice *(post-MVP-core; must not displace S1–S14)*
XAudio2 device + mastering voice + five submixes with gains from config; pooled source
voices; RIFF WAV loader; JSON sound bank; X3DAudio listener at camera focus raised by zoom
(ADR-011 §4) with mono emitters at render positions; one 2D UI cue (order rejected) and one
3D engine loop; `AudioUpdate` stage timed as the fifth budget row.
**Accept:** `NeuronClientTests` listener/emitter math + bank parsing headless; no audio device
⇒ client logs, disables audio, runs on; manual check: panning moves the audio frame, zooming
out attenuates, voice pool never exceeds its cap under a 200-ship stress scene.

**Built ✅ (2026-08-19):** the graph is `AudioDevice`, one pimpl deep, exactly as
`QuicTransport` keeps msquic — `IXAudio2` + mastering voice + the five submixes, category
gains from config on the submixes and the master gain on the master, so a settings screen
writes one number per voice rather than multiplying anything. Four device-free halves sit in
front of it and carry all the decisions: `SoundBank` (the JSON bank), `WavClip` (a RIFF
chunk walker, unknown chunks skipped and an overrunning size field refused), `AudioListener`
(the model below), and `VoicePool` (the allocation policy). That split is the reason 21 of
these tests exist at all: CI has no sound card, so anything only assertable through XAudio2 is
never asserted.

*The listener.* ADR-011 §4's model, and the one thing here with something to get wrong: the
ear at the camera's focus, raised by the zoom, `front` the camera's own plane-forward so a
ship drawn right sounds right. **Its consequence is bigger than it looks and it is a content
trap** — the ear is `zoomMetres` above the plane, so at the default 8 km zoom every ship is
already ~8 km away however tightly the fleet is parked. The first bank shipped a 6 km falloff,
which reads as generous against a 1.4 km wing radius and is silence on a fresh session. The
distances are now set against the *camera* (2 km to 24 km), which is what makes zooming out
recede to nothing rather than being off from the start. `minDistance` is honoured by an
explicit three-point volume curve; X3DAudio's default curve has nowhere to put it, so a bank
could otherwise name a full-volume radius and be ignored.

*The pool.* Priority, then distance, and it only steals from something the incoming sound
actually beats — without that last clause a full pool always yields and a distant hum evicts
the alert. Two pools, 32 spatial and 16 flat, so a battle can never make the interface go
quiet. Live on the strip's WORLD row: **41 ships ask and 32 sound**, which is the 200-ship
acceptance case reproducible at the default zoom.

*Events.* Both bounce paths raise `order.rejected` — the third surface one refusal owes beside
the ghost and the toast, and on both paths for the same reason the other two are (ADR-005 §4).
Every ship declares `ship.engine` each frame from `m_scene.entities`, so audio positions are
the interpolated ones the renderer used and **F10 freezes the fleet's sound with its hulls**.
`AudioUpdate` is the sixth stage row, budgeted 0.4 ms and measuring 0.08 ms on a 41-ship
scene.

**Two ADR-011 §8 details are deliberately not built, and neither is needed yet.** Finished
voices are retired by polling `GetState` on the stage that would have drained the ring, rather
than by `IXAudio2VoiceCallback` pushing into an SPSC ring — nothing consumes a buffer event in
the MVP, and the ADR's actual constraint (no game or render state from an audio callback) is
met by having no callback. XAudio2's own threads therefore do not register as external
telemetry lanes. Both arrive with streaming music, which is the first thing that needs the
event.

**Verified:** 499 tests (21 new), `selfTest` still exit 0, the client exits 0 with audio up —
`2 of 2 cue(s) loaded, 8 output channel(s) at 48000 Hz, 32 3D + 16 2D voices`. **The two WAVs
are synthesised placeholders** — a hum and a blip — and exist so the graph, the pool and the
listener can be heard working; they are the first thing a sound designer replaces, and only
`SoundBank.json` changes when they are.

**Outstanding:** the manual half, which is all of it that matters — panning moving the audio
frame, zooming out attenuating, and whether the engine bed is pleasant rather than merely
present. Nobody has heard this yet.

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
