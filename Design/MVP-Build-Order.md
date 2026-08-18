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
**Outstanding:** resize and borderless-fullscreen toggle are wired but unexercised; the three
acceptance measurements are manual and nobody has run them yet — CI has no GPU.

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
**Outstanding:** the `GAME/EXTRACT/RENDER/UI` rows the corpus HUD shows need those stages to
exist — they arrive with S5. `GameLogicTests` is still unwired (its library is empty).

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
**Outstanding:** the 60-second period measurement is a manual run — the CI test asserts loose
bounds (5–40 ticks in 500 ms) because a shared runner is not a real-time system.

### S4 — Transport + handshake + heartbeat 🏁 **M0**
`Transport` + `UdpTransport` (non-blocking Winsock, 1,152 B datagram cap, minimal control-
channel reliability); `Hello/Welcome/UpdateRequired` with schema hash (and `universeHash` +
`worldMeta` once S5b lands); `Ping/Pong`; client half connects in-process; NET stats
(RTT/loss) logged both sides.
**Accept:** *window opens, swapchain presents, server ticks, heartbeat crosses the loopback* —
the brief's milestone, demonstrably. `NeuronCoreTests` handshake over real loopback socket;
schema-hash mismatch produces `UpdateRequired` + refusal (test forces a bad hash).
`selfTest` covers handshake + ping.
**Built ✅:** `Transport.h`, `UdpTransport.h/.cpp` (non-blocking Winsock, 1,152 B cap,
stop-and-wait control reliability), `Wire.h/.cpp`, `ClientConnection.h/.cpp`; NET stats logged
on both sides every 5 s; `SelfTest.h/.cpp` drives the whole M0 exchange headlessly and returns
an exit code. `NeuronCoreTests` covers the loopback and the wire; `NeuronServerTests` covers
the handshake and a forced hash mismatch producing `UpdateRequired`.
**Outstanding:** the visible half of M0 — window open, swapchain presenting, heartbeat live —
still needs a person at a machine. The engine half is green in CI.

---

## 🏁 M0 — status as of 2026-08-18

**Everything M0 asks for that a machine can check is done and green. One item remains, and it
is the visual one.**

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
| **Window opens, swapchain presents** | S1, S4 | **a person, at a machine with a GPU** | ⏳ **outstanding** |
| 5 min with no debug-layer messages | S1 | manual | ⏳ outstanding |
| PresentMon shows flip model | S1 | manual (`DXGI_SWAP_EFFECT_FLIP_DISCARD` + waitable object are in the code) | ⏳ outstanding |
| Close exits 0 | S1 | manual | ⏳ outstanding |
| Headless 60 s: mean period 50 ms ± 0.5, no overruns | S3 | `selfTest` measures and reports it; the ± 0.5 bound needs an **idle** machine | ⏳ outstanding |

**How to close the rest** — one run, on a machine with a GPU:

1. `"selfTest": true` in `Outpost.json`, run the exe, read `Outpost.log`. Exit code 0 and
   `self test: PASSED` closes the cadence measurement and re-proves the whole M0 exchange on
   real hardware rather than a CI runner. The log line to read is
   `N ticks in M ms -- mean period X ms`.
2. `"selfTest": false`, `"mode": "host"`, run it. A window that opens, clears to the animated
   near-black blue, and logs `first pong: server tick N, round trip X ms` is M0's visible half.
   Leave it five minutes, watch the debug-layer output, close it, check the exit code.

Everything else is signed off by CI, which at the time of writing runs **82 tests across four
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

### S5b — Universe definition & Vesta-3
`UniversePos`/`UniverseDef` types + pure JSON-backed parse in GameLogic (ADR-009 + ADR-012,
using S2b's parser); `GameData/Universe/` authored with Vesta-3 (star, two planets, one
station); `universeHash` over canonical parsed content; hosts read
the file via NeuronCore and both halves load it; grid anchored at the station; station renders
with the `Structure` mesh, celestials as distant backdrop.
**Accept:** `GameLogicTests` parse round-trip, malformed-input rejection, `universeHash`
stability across reorderings that shouldn't matter and change on ones that should,
anchor+local reconstruction property test; the client's rendered scene comes from the file
(edit a station position → it moves, no rebuild).

### S5c — The engine/game seams
`Neuron::Simulation` (NeuronServer) and `Neuron::WorldView` (NeuronClient) declared, with the
neutral types they speak — `EntityRecord` (NeuronCore), `RenderScene`, `OrderIntent`,
`FormationPreview`; stub implementations in the test projects; `ServerHost` and `ClientApp`
take them by reference; `Outpost.exe` constructs the GameLogic-backed ones (ADR-014).
**Accept:** `NeuronServerTests` and `NeuronClientTests` drive their library against a **stub**
simulation/world view with no GameLogic in sight — the proof the engine is game-free;
`Outpost.exe` is the only project referencing GameLogic (grep rule on the vcxprojs).

### S6 — GameLogic sim + replay determinism
World tables, `ShipClassTable` (11-value enum, 9 with content — Fighter/Cruiser reserved),
`Steering/Integrate` (seek-with-arrival, accel + turn-rate clamps), scripted-order harness,
`WorldHash`.
**Accept:** `GameLogicTests`: double-run replay hash equality (1,000 ticks, scripted orders);
movement envelope (top speed, turn radius, arrival overshoot < tolerance); zero clock/RNG
imports outside the seeded PCG32 (grep-able rule, asserted in review); no `UniversePos` in
per-tick sim math (grep rule, ADR-009 §2).

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
