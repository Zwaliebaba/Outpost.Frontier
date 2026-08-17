# MVP Build Order — Vertical Slices

**Status:** Session output 2026-08-17 · Each slice is independently testable, lands green
(`Tests/` + `--selftest` where applicable), and is sized at "a few days" or less. Order
matters — later slices assume earlier ones. Milestones: **M0** = the brief's named first
milestone; **M1** = first commanded fleet; **MVP** = playable definition met.

Test placement follows the Dependency Map: sim logic proves itself in `GameLogicTests`,
transport/foundation in `NeuronCoreTests`, host lifecycle in `NeuronServerTests`, client math
in `NeuronClientTests`, and anything needing the real loopback or GPU in `--selftest` /
manual checkpoints.

---

### S1 — Window, device, swapchain
Raw Win32 window; DX12 device + direct queue; flip-model swapchain (3 buffers, waitable, 2
frames in flight); clear-to-colour animates; resize + fullscreen-borderless toggle; clean
shutdown; debug layer clean.
**Accept:** runs 5 min without debug-layer messages; PresentMon shows flip model; close exits 0.

### S2 — NeuronCore foundations
`Assert/Log/Time(QPC)/Hash(FNV)/Random(PCG32)`, `float2..mat4` + plane-ray math,
`ByteReader/Writer`, SPSC/MPSC rings, telemetry lane registry + `NEURON_SPAN/COUNTER`,
`TaskPool`.
**Accept:** `NeuronCoreTests`: byte IO round-trip + underrun bounds, ring stress (2 threads),
math cases, PCG32 vectors, span timing sanity. *(Test projects need their ProjectReferences
wired — owner task, see README.)*

### S3 — ServerHost skeleton + `--headless`
`ServerHost{Start/Stop/Join}` with Sim thread, 20 Hz waitable-timer loop (absolute schedule,
snap-forward rule), tick counter + `tick_overrun` telemetry; `Outpost.exe --headless` runs it
under console logging until Ctrl-C.
**Accept:** `NeuronServerTests` start/stop/join ×100 no leak/hang; headless 60 s: mean period
50 ms ± 0.5, no overruns on an idle machine.

### S4 — Transport + handshake + heartbeat 🏁 **M0**
`ITransport` + `UdpTransport` (non-blocking Winsock, 1,152 B datagram cap, minimal control-
channel reliability); `Hello/Welcome/UpdateRequired` with schema hash; `Ping/Pong`; client
half connects in-process; NET stats (RTT/loss) logged both sides.
**Accept:** *window opens, swapchain presents, server ticks, heartbeat crosses the loopback* —
the brief's milestone, demonstrably. `NeuronCoreTests` handshake over real loopback socket;
schema-hash mismatch produces `UpdateRequired` + refusal (test forces a bad hash).
`--selftest` covers handshake + ping.

### S5 — Meshes, atlas, opaque pass, camera
OBJ/MTL loader → submesh ranges (9 classes); DirectWrite glyph-atlas bake (TaskPool); opaque
instanced pass (flat shading, 5 materials, emissive accents); ortho camera 30° elevation, yaw
orbit + 45° snaps, zoom clamp, plane pan. A locally-faked parked fleet renders — no net yet.
**Accept:** `NeuronClientTests` OBJ parser (counts/ranges vs known meshes); visual checkpoint
vs `tactical-hud.png` vibe (dark space, green accents, silhouettes readable at min zoom);
frame time < 2 ms at 41 instances.

### S6 — GameLogic sim + replay determinism
World tables, `ShipClassTable` (11 classes), `Steering/Integrate` (seek-with-arrival, accel +
turn-rate clamps), scripted-order harness, `WorldHash`.
**Accept:** `GameLogicTests`: double-run replay hash equality (1,000 ticks, scripted orders);
movement envelope (top speed, turn radius, arrival overshoot < tolerance); zero clock/RNG
imports outside the seeded PCG32 (grep-able rule, asserted in review).

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
Right-drag order puck (plane point + facing), client pre-check via GameLogic `ValidateOrder`,
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
DATAGRAM = state; `--transport=quic`.
**Accept:** the *unmodified* game runs over QUIC on loopback; `--selftest` runs the full
handshake+order+snapshot loop over **both** transports; measured added latency < 1 ms
loopback. Friction findings feed Risk R3 disposition (stay Schannel vs flag OpenSSL flavour).

### S14 — Debug strip, selftest, polish 🏁 **MVP**
Tier-1 counters strip (frame/GAME/EXTRACT/UI ms, net RTT/loss/jitter, snap age/drift ticks,
`tick_overrun`, drops) behind a toggle; `--selftest` aggregates: schema self-check, both-
transport handshake, replay determinism run, wire round-trips — exit-code CI gate; polish:
4× MSAA offscreen + resolve, cosmetic banking/hover from velocity, STALE marker visual.
**Accept:** MVP playable definition demonstrated end-to-end — select fleet, issue queued
formation moves, watch execution with status + feedback — over both transports; `--selftest`
green on a GPU-less runner; counters strip numbers plausible vs `debug-hud.png` rows.

---

## Sequencing rationale (why this order)

- S1–S4 front-load the two integration unknowns (DX12 plumbing, loopback transport) and land
  the brief's named milestone in four slices.
- Rendering (S5) precedes sim (S6) so every sim-side slice after S7 is *visible* — but S6's
  determinism harness exists **before** the first networked ship moves, so replication bugs
  never masquerade as sim bugs.
- The order pipeline (S9) is deliberately after selection/overlay (S8): the ghost/bounce UX
  needs somewhere to draw.
- msquic (S13) sits after the protocol stabilises (S12) but before MVP-complete, per ADR-003 —
  late enough to test the real protocol, early enough that friction has schedule to land.
