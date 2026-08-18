# Dependency Map & Public Interfaces

**Status:** Session output 2026-08-17 · refines the fixed dependency rules from the brief.
Header-level contracts only — signatures live in code. Namespaces: **`Neuron`** for the three
engine libraries and **`Game`** for GameLogic ([AGENTS.md](../AGENTS.md) §1 F9). File names are
flat per ADR-013 (no code subdirectories); the tables below are also the **file-name registry**
that keeps names unique repo-wide.

## The graph

```mermaid
flowchart BT
    NC["NeuronCore<br/><i>engine primitives, zero game semantics</i>"]
    GL["GameLogic<br/><i>deterministic planar sim + game wire schemas</i>"] --> NC
    NS["NeuronServer<br/><i>ServerHost, sessions, tick orchestration</i>"] --> NC
    NCL["NeuronClient<br/><i>render, input, camera, HUD, audio, interpolation</i>"] --> NC
    EXE["Outpost.exe<br/><i>composition root — the only project that knows both</i>"] --> NS
    EXE --> NCL
    EXE --> GL
    TGL["Tests/GameLogicTests"] --> GL
    TNC["Tests/NeuronCoreTests"] --> NC
    TNS["Tests/NeuronServerTests"] --> NS
    TNCL["Tests/NeuronClientTests"] --> NCL
```

Rules (hard): GameLogic depends **only** on NeuronCore. GameLogic never references
NeuronServer/NeuronClient. Nothing depends on the executable. Client and server never depend on
each other. **The engine libraries never reference GameLogic** — `Outpost.exe` is the only
project that knows both, and the seam is dependency inversion (ADR-014). The brief permitted
`NeuronClient`/`NeuronServer` to depend on GameLogic; that permission is deliberately declined,
because `Neuron*` is a shared engine serving a second game in the sibling repository.

## Rulings made this session (refinements, not bends)

1. **Game wire schemas live in GameLogic** (`Snapshot.h`, `OrderMessages.h`), not NeuronCore.
   Snapshot and order payloads *are* game semantics; NeuronCore's charter forbids them.
   NeuronCore owns only the semantics-free layer: byte IO, JSON, framing, handshake/ping
   messages, transport. *(ADR-004.)*
2. ~~NeuronClient links GameLogic from day one.~~ **Overturned by ADR-014.** The client needs
   GameLogic *code* (BounceParity's `ValidateOrder`, the footprint's `SolveFormation`) but not
   a GameLogic *dependency*: it declares `Neuron::WorldView`, and `Outpost.exe` implements it
   by forwarding to GameLogic's pure functions. Same function, same reason codes, same bounce —
   reached through an interface rather than a link-time symbol. *(The vtable sits in the
   executable rather than in GameLogic because `WorldView.h` is NeuronClient's; ADR-014 §2a
   records why that is the right side of the trade.)*
3. **The client never constructs an authoritative `World`.** It applies snapshots to a
   `ReplicatedView` (GameLogic type, quantised fields). When prediction arrives it will run a
   *separate* GameLogic world instance — never a reference to the server's. *(ADR-007 §5.)*
4. **NeuronCore's "zero game semantics" test:** every NeuronCore header must be plausible in
   an unrelated networked sim. A header mentioning ships, orders, formations, or hull classes
   is in the wrong library — flag it in review, no exceptions without a session decision.
5. **DirectXMath is toolchain, not a dependency.** It is header-only, OS-free SDK math, so
   **GameLogic includes it directly** without violating "GameLogic depends only on NeuronCore"
   (a rule about *our* libraries). There is no NeuronCore math header at all. *(ADR-010.)*
6. **The JSON parser is NeuronCore's**, and GameLogic uses it for universe content — in
   charter, since NeuronCore is GameLogic's permitted dependency. File *reading* stays in the
   hosts; GameLogic parses bytes. *(ADR-012.)*
7. **Surfaced, needs owner sign-off:** if the Schannel-flavour msquic package ever has to swap
   to the OpenSSL flavour (older-Windows support, cert-file loading), that is treated as
   *within* the existing "msquic via NuGet" approval — flagged here so the assumption is
   visible. *(ADR-003, Risk R3.)*

## Per-project contracts

### NeuronCore — engine primitives (static lib)
Allowed deps: Windows SDK (Win32, Winsock2, **DirectXMath**), msquic (NuGet), C++ standard
library. C++/WinRT only where genuinely useful (none identified in MVP).
**No math header** — DirectXMath is used natively at call sites (ADR-010).

| Area | Files | Notes |
|---|---|---|
| Foundation | `Debug.h` (asserts) `Log.h` `Clock.h` (QPC + `WaitableTimer`) `Hash.h` (FNV-1a) `Random.h` (PCG32) | Named to avoid shadowing `<assert.h>`/`<time.h>` (ADR-013 §3a); no game types anywhere below |
| Memory/containers | `Arena.h` `RingBuffer.h` (`RingBuffer` SPSC + `MpscRingBuffer`) | std containers allowed; arenas for frame/tick scratch. Both bounded and allocation-free: a full queue drops and counts rather than stalling the thread that pushed |
| Tasking | `TaskPool.h` (`Submit`, `WaitGroup`, `Wait`) | Boot bakes only in MVP (ADR-007 §4). Mutex + condvar + `std::function` on purpose: a lock-free queue here would be fixed-capacity and would have to drop work, which for a mesh load is a bug rather than a trade |
| Telemetry | `Telemetry.h` (`NEURON_COUNTER`, `NEURON_SPAN`, lane registry, `TelemetrySnapshot`) | Per-lane SPSC rings drained by one collector; cap 16; a lane is a role, not a thread, so re-registering a name adopts it. Compiled into Release — `tickOverrun` ships (ADR-002) |
| Serialization | `ByteReader.h` `ByteWriter.h` `EntityRecord.h` `OrderIntent.h` | Bounds-checked, little-endian; `EntityRecord` is the neutral 20-byte replication record the engine speaks instead of a game type (ADR-014 §3). `OrderIntent.h` carries `OrderIntent`/`OrderVerdict`/`OrderPreview` — the command half of both seams, here rather than beside either because `WorldView::PreCheck` and `Simulation::ApplyOrderBytes` must return the same verdict type and NeuronClient cannot see NeuronServer (ADR-014 §2b) |
| JSON | `Json.h` (flat-node DOM, iterative parse, `int64`-exact numbers, diagnostics) `JsonWriter.h` (strict output) | Config + universe + sound banks (ADR-012 §C) |
| Transport | `Transport.h` (`Transport`, `Connection`, `Listener`, `Stats`) `UdpTransport.h` `QuicTransport.h` | QUIC-shaped contract, `Poll()` delivery, 1,152 B datagram cap (ADR-003) |
| Wire (semantics-free) | `Wire.h` (framing, `Hello/Welcome/UpdateRequired/Refuse/Ping/Pong/Goodbye`) | Schema-hash mechanism lives here; game payloads do not |

### GameLogic — deterministic sim (static lib)
Allowed deps: **NeuronCore only**, plus DirectXMath as toolchain (ruling 5). No Windows
headers, no rendering, no sockets, no clock, no file IO.

| Area | Files | Notes |
|---|---|---|
| Identity | `Ids.h` (`ShipId`, `WingId`, `OrderId`, `SystemId`, `CelestialId`, `StationId`, `GateId`) `ShipClass.h` | 11-value closed taxonomy; Fighter/Cruiser reserved ids (ADR-009 §6) |
| Universe | `Ids.h` (`RegionId`/`SystemId`/`CelestialId`/`StationId`/`GateId`, all `u16`, authored not assigned) `Universe.h` (`UniversePos{i64,i64}`, `UniverseDef`, region/system/celestial/station/gate tables, `GridAnchor`, `LocalFromUniverse`/`UniverseFromLocal`) `UniverseParse.h` (JSON bytes → `UniverseDef`, pure) | Exact integer metres — **no float anywhere in the model**, which is what makes the hash and the reconstruction exact; `universeHash` over canonical parsed content, walked in id order so reordering a file is not a content change (ADR-012 §D) |
| World | `World.h` (SoA tables, `ShipId`↔slot indirection, seeded `Pcg32`, `Tick` = IngestOrders → Steering → Integrate) `ShipClass.h` (the closed eleven, compiled in) `WorldHash.h` (FNV over raw state bits) `ReplicatedView.h` (quantised client view + `ApplySnapshot`) | Single-writer; state stored as `XMFLOAT2` (ADR-010 §3); guidance carries a resolved target rather than ADR-005 §1's `groupRef` so Steering never learns groups exist; the hash folds float *bits* because same-binary replay means bit-identical |
| Orders | `Orders.h` (`OrderSubmit`, `OrderGroup`, 4-leg queue) `Validate.h` (`ValidateOrder`, `ReasonCode`) | Validation consumes wire-quantised values only (ADR-005 §4) |
| Formations | `Formation.h` (`FormationId{Line,Wedge,Claw}`, `SolveFormation`) | Pure; client footprint preview calls this exact function |
| Wire | `Snapshot.h` (full quantised snapshots over `Neuron::EntityRecord`) `ReplicatedView.h` (client's quantised shadow + interpolation) `SchemaHash.h` `OrderMessages.h` | Full snapshots, no deltas, so loss is not a case to handle. The ship record is NeuronCore's `EntityRecord` rather than a `ShipRecord` of ours — ADR-004 §6 specifies exactly those twenty bytes and two matching layouts would eventually diverge. The schema string covers the **quantisation constants**, not just the fields |
| Test hooks | `WorldHash.h` (FNV over state) | Replay determinism harness |

### NeuronServer — hosting the authority (static lib)
Allowed deps: **NeuronCore only** (ADR-014). It hosts *a* simulation, not *this* one.

| Area | Files | Notes |
|---|---|---|
| Seam | `Simulation.h` (`AdvanceTick`, `ApplyOrderBytes`, `WriteSnapshot`, `SchemaHash`, `ContentHash`) | Engine-declared, GameLogic-implemented, exe-injected (ADR-014 §2) |
| Host | `ServerHost.h` (`Start/Stop/Join`, takes a `Simulation&`) `ServerConfig.h` (plain struct, assembled by the exe) | The future OutpostServer.exe API, verbatim (ADR-008) |
| Sessions | `Session.h` (per-connection state, handshake, order seq/ack bookkeeping) | Session *table*; empty server keeps ticking |
| Replication | `SnapshotSender.h` (emit → datagram per client) | Per-client path; interest/delta land here later |

### NeuronClient — the player's machine (static lib)
Allowed deps: **NeuronCore only**, plus the Windows SDK (D3D12, DXGI, DirectXMath, DirectWrite
for the atlas bake, **XAudio2 + X3DAudio**). No GameLogic (ADR-014).

| Area | Files | Notes |
|---|---|---|
| Seam | `WorldView.h` (`ApplySnapshot`, `BuildScene`, `PreCheck`, `SolvePreview`, `EncodeOrder`, `SchemaHash`, `ContentHash`) + `NullWorldView`; the neutral types it speaks are `RenderScene` (here) and `OrderIntent`/`OrderVerdict`/`OrderPreview` (NeuronCore) | Engine-declared, exe-implemented, exe-injected (ADR-014 §2a); BounceParity runs the same function through it (§3). `FormationPreview` was renamed `OrderPreview` — the engine may name no formation (§2b) |
| App | `ClientApp.h` (`Run`, takes a `WorldView&`) `ClientConfig.h` (plain struct) `Window.h` `ClearColour.h` | Frame loop on Main thread (ADR-007); `ClearColour` is deliberately free of D3D and C++/WinRT headers so presentation maths is testable without a device |
| Net/state | `ClientConnection.h` (handshake, ping, snapshot receipt) `SnapshotBuffer.h` (slew-limited server-time estimate, render tick, staleness, drift) | The buffer owns the *clock*, not the payloads: it turns snapshot arrivals into a smooth render tick and the game answers what the world looks like at that instant. The estimate slews rather than tracking arrivals, because following jitter directly puts it straight on screen. Device-free, so the timing hardest to eyeball is the part most tested |
| Extract | `RenderWorld.h` (`InstanceRecord`, `RenderScene`, overlay lists, HUD state) | The future Game/Render thread seam. `InstanceRecord` is 20 bytes *and is the per-instance vertex stream*, so its size is a static assert rather than a comment |
| GPU | `GpuCom.h` (`GpuPtr` = `winrt::com_ptr`) `GpuDevice.h` `GpuSwapChain.h` (+ the depth buffer, which is the only other resource the swapchain's size owns) `GpuUploadRing.h` `GpuMeshes.h` (VB/IB per class) `GpuPasses.h` (Clear/Opaque/OverlayWorld/Ui) `GpuPipelines.h` | Fixed pass list w/ reserved slots (ADR-006); COM ownership is `winrt::com_ptr` throughout, created via `IID_PPV_ARGS(x.put())` (AGENTS.md §5) |
| Assets | `ObjMesh.h` (loader → submesh ranges) `GlyphAtlas.h` (DWrite bake) | Boot-time, TaskPool. Both parsers are free of D3D and C++/WinRT headers so `NeuronClientTests` can drive them with no device |
| Camera/input | `IsoCamera.h` (ortho 30°, yaw snap, zoom) `Picking.h` (`XMPlaneIntersectLine` + 2D tests) `InputMap.h` (`InputFrame` → `CameraIntent`; `Window` owns the virtual-key table) | Client-only state |
| HUD | `HudLayout.h` `HudRoster.h` (context bar, ability rack stub, toasts) `OrderPuck.h` (drag/facing/ghost lifecycle) | Prints are the spec |
| Nebula | `NebulaField.h` (periodic value-noise tile, device-free) `GpuNebula.h` (bake + upload + SRV) | ADR-006 §1a's built node; the field is CPU-baked so its maths is testable without a device, and periodic so the tile wraps at any zoom |
| Audio | `AudioSystem.h` (XAudio2 graph, voice pool) `AudioListener.h` (focus/zoom listener model) `AudioBank.h` (JSON bank + RIFF WAV) | 5 submixes; `AudioUpdate` stage after Extract (ADR-011) |

### Outpost.exe — composition root
Allowed deps: NeuronServer, NeuronClient, GameLogic (and transitively the rest). Files:
`Main.cpp` (`wWinMain`, **arguments ignored**), `ConfigLoad.h/.cpp` (locate + parse + merge
`Outpost.json` and the user layer), `AppConfig.h/.cpp` (JSON → `ServerConfig`/`ClientConfig`
structs), `UniverseLoad.h/.cpp` (locate + read the universe definition, then hand the bytes to
GameLogic's pure parser — file IO stays here so GameLogic stays OS-free, ADR-009 §7),
boot/shutdown ordering (ADR-008), `SelfTest.h/.cpp` (the `selfTest` driver: server up,
handshake, heartbeat, exit code). **Nothing else.** Nothing depends on it.

It is also the only place universe metres become render metres: it reads the authored
placements, converts them into the tactical grid's local frame, and hands them to the world
view. GameLogic owns the exact coordinates and the engine owns the drawing; the conversion
belongs to the one thing that knows both (ADR-014).

**And it is where the engine's interfaces are implemented** (ADR-014 §2a). `ParkedFleetView.h/.cpp`
is a `Neuron::WorldView`; the `Simulation` in `Main.cpp` is the server's half. Both forward to
GameLogic rather than containing logic. They are here rather than in GameLogic because
`WorldView.h` is NeuronClient's and `Simulation.h` is NeuronServer's, and putting either
project on GameLogic's include path would trade a structural guarantee — GameLogic is free of
Windows, D3D12 and file IO, so its tests need no device — for a convention.

### Tests/* — VS CppUnitTestFramework (added on main, 2026-08-17)
Each test project depends on its library under test plus that library's allowed deps.
- `GameLogicTests` — replay determinism (double-run hash equality), wire round-trip/underrun,
  formation geometry, validation parity (quantised inputs), universe JSON parse round-trip +
  rejection, `universeHash` stability, anchor+local reconstruction. **Live from S5b**: the
  universe suite runs on text held in the test file, so it needs no filesystem and no fixtures
  — the practical payoff of keeping file IO out of GameLogic.
- `NeuronCoreTests` — byte IO, ring buffers, **JSON conformance corpus** (valid/invalid,
  `int64` exactness past 2⁵³, depth cap, duplicate keys, escapes/surrogates, writer
  round-trip), UDP loopback handshake (real socket), `Welcome` round-trip over a full-width
  negative anchor — a field narrowed anywhere in the chain folds there rather than in a
  session where the world quietly renders somewhere else.
- `NeuronServerTests` — `ServerHost` start/stop/join, session handshake against a raw
  core-level client, the simulation's `WorldMeta` arriving intact in `Welcome`, config structs
  built in code (no files).
- `NeuronClientTests` — interpolation/extrapolation timeline, picking math, OBJ parser,
  camera projection and input mapping, the NDC→plane mapping round-tripped against the real
  view-projection, the nebula field's determinism/sparseness/seamless wrap, extract layout,
  **a `WorldView` written from engine headers alone and driven through every method** (S5c's
  proof that the seam carries no game shape), audio listener/emitter math, sound-bank parsing. **No D3D device and no audio device in unit tests**; GPU/audio smoke
  lives in `selfTest`-adjacent manual slices.

> Resolved (S5): the test .vcxprojs now carry `ProjectReference`s and matching include paths.
> The observation that they did not is kept here only because it is why the client's
> device-free headers are device-free.

## Include discipline (ADR-013)

- **Flat project folders**, no code subdirectories; IDE grouping via `.vcxproj.filters`.
- **File names are unique repo-wide** — the tables above are the registry; check before adding.
- **Each project lists the libraries it uses as `$(SolutionDir)<Project>` include paths**
  (owner decision, already configured), so includes are unqualified: `#include "Json.h"`
  reaches NeuronCore from anywhere entitled to it. Uniqueness is what keeps this unambiguous,
  which makes the registry above load-bearing rather than advisory: a duplicate file name
  silently resolves to whichever path comes first.
- The include path is *not* the dependency rule. NeuronClient can see NeuronCore's headers
  because it lists them; it must not list GameLogic (ADR-014).
- A project's non-exported headers are simply those no other project includes; there is no
  `Public/` folder split (dropped by ADR-013).
