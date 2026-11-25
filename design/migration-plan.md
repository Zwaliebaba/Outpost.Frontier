# C++ to Rust migration plan

Status: proposed, 2026-11-15. Owner decisions are recorded in
[decisions.md](decisions.md); evidence is in [assessment/](assessment/).

## 1. What this plan delivers

`CppSource/` holds 39 Visual Studio projects, about 820,000 lines of 32-bit
Windows C++. This plan replaces all of it with the Rust workspace at the
repository root, built for `x86_64-pc-windows-msvc`, and ends by deleting
`CppSource/`.

The finished Rust build:

- produces `OutpostFrontier.exe` and `OutpostServer.exe`;
- loads the original data files, save games and text resources unchanged;
- behaves like the C++ build within stated tolerances (section 5);
- uses no legacy platform technology: no Direct3D 8/9, DirectDraw,
  DirectPlay, DirectSound, DirectInput, DirectShow, MFC or x87 assembly
  (section 4).

Out of scope: network compatibility with the C++ binaries, bit-exact
simulation, an internet server list, game controller support, new gameplay.

### How "library per library" works here

The C++ is 32-bit and cannot be recompiled for x64 (about 4,200 layout
assertions pin it), so Rust and C++ never share a process. Each C++ library
is ported to Rust in dependency order and accepted against reference output
captured from the C++ build. `CppSource/` stays in the tree, frozen, as the
reference, and is deleted in one commit at the end (decision O10).

The mapping is not one C++ library to one Rust module. Of the 39 projects,
11 are deleted without a port, 8 resource projects become text catalogs, and
the rest are redistributed over six Rust crates. The
[library ledger](library-ledger.md) accounts for every project.

## 2. Findings that shape the plan

Eight parallel assessments read the C++ tree. The findings that changed the
plan most:

| Finding | Consequence |
| --- | --- |
| Nothing can verify a port today: no tests, no game data, no INI files, and the C++ does not build from the repository alone | Phase 0 restores a C++ reference build and adds a harness that emits test vectors |
| The C++ reproduces the original binaries at instruction level, including x87 rounding that differs between the client (24-bit) and the dedicated server (53-bit) | The port does not chase this. Formats are exact, behaviour is equivalent within tolerance (decisions O2, T11) |
| About 78,000 lines of `Common` (space objects, equipment, flight AI) are used by both client and server, and single player runs the server inside the client process | A sixth crate, `game-sim`, holds shared simulation. `OutpostFrontier.exe` also links `game-logic` (decision O4) |
| The component system (`neuroncomponents`, `system`, `NeuronRuntime::Startup`) is late binding by string name through an INI file that is not in the repository | Replaced by static composition. Nothing is loaded by name at run time (decision T9) |
| DirectPlay is used as one reliable, ordered stream per client. The 162 message encodings never touch DirectPlay | Message encodings are kept; only the transport and framing are replaced (decisions O6, T17) |
| `rpclocal` (28,000 lines) duplicates the codecs in `RemoteCommon` (5,800 lines) | Only one codec set is ported; `rpclocal` becomes a loopback transport of a few hundred lines |
| The physics library is a 50,000-line reconstruction with no licence text, and the original is not reproducible run to run | Not ported. A facade over `parry3d` and a small solver replaces it, with free flight transliterated from IVP (decisions O5, O19) |
| The Direct3D 8 footprint is small: two texture stages, no shaders, no stencil, no render targets | One vertex and one pixel shader plus a pipeline-state cache cover it on Direct3D 12 |
| The renderer interface is used as a mutable state machine from about 650 call sites | The Rust renderer first offers the same state-machine shape as a typed API, so callers port without redesign (decision T2) |
| 110,000 of the client's 233,000 lines are user interface, built from 3D meshes and laid out by data files | The data contracts are kept; the toolkit internals are rebuilt (decision T20) |
| The original server list needs a closed third-party DLL that is not in the repository | Dropped. Players connect by address (decision O8) |
| Scene scripts are Lua 3.2 used only as a load-time data language | A Rust reader for the compiled chunks and the 15 opcodes they use replaces the C interpreter (decisions T8, O18) |

Several initial assumptions were wrong and are corrected here: there is no
Miles audio code, DirectInput is used for the mouse only, there is no
joystick support, and `luaprofile`, `qhull` and `zlib` have little or no
live use.

## 3. Target architecture

```text
OutpostFrontier.exe ─┬─> game-app ───> neuron-client ─┐
                     │       │                        ├─> neuron-core
                     │       └──> game-sim ───────────┤
                     │              ^                 │
                     └─> game-logic ┴─> neuron-server ┘
OutpostServer.exe ─────> game-logic
```

| Crate | Owns | Platform code |
| --- | --- | --- |
| `neuron-core` | Hashes and IDs, INI readers, file system and UTF container, math, asset data parsers, scene and animation runtime, physics core, wire primitives and the `Transport` trait, text catalog loader, diagnostics | None. Builds and tests on any host |
| `game-sim` (new) | Game data (archetypes, universe, goods, reputation), object model, equipment, flight AI, the 162-message protocol | None |
| `neuron-server` | Save container, account store, session slots, controller bus and timers, QUIC host, tick driver | Sockets only |
| `game-logic` | Authoritative world, players, missions, encounters, NPCs, random missions, admin operations | None |
| `neuron-client` | Window, input, Direct3D 12 renderer, materials, meshes, particles, skinning, audio, text, UI toolkit, scene scripts, movie playback, QUIC connection | All Windows API use lives here, behind `cfg(windows)` |
| `game-app` | Client flow, client object view, cameras, HUD, base screens, front end, options | None |
| `apps/outpost-frontier` | Process start; builds the engine, the in-process server and the client and connects them | — |
| `apps/outpost-server` | Process start; config file, admin console | — |

Rules that hold across the workspace:

- `game-app` does not depend on `game-logic`. The executable package hands
  the client a `Transport`; in single player it is a loopback to the
  in-process server.
- `neuron-core` owns wire primitives and framing. The message catalog
  carries game types, so it lives in `game-sim`. This amends the ownership
  line in `AGENTS.md`, which work package P0.1 updates together with the
  port map.
- State is owned by explicit context values (`Engine`, `World`,
  `ServerContext`, `ClientContext`). The roughly 3,400 C++ globals are not
  reproduced as statics.
- Objects refer to each other by typed handles into arenas, not by pointer.
- Only layouts that are formats are preserved: files, save text and wire
  payloads. In-memory layouts, vtable slots and allocation sizes are
  reconstruction artefacts and are dropped.

## 4. Legacy technology and its replacement

No legacy API enters the Rust code. Gate G7 checks this mechanically.

| Legacy (where used) | Replacement | Work package |
| --- | --- | --- |
| Direct3D 8 fixed function, D3DX8 (`DX8RENDERER`) | Direct3D 12 through the `windows` crate; fixed-function behaviour in HLSL; Rust image loading and conversion | P4.2, P4.3, P4.4 |
| Direct3D 9, D3DX9, runtime HLSL (`DX9RENDERER`) | The same Direct3D 12 renderer; a programmable-pass API for the volumetric nebula | P4.9, P6.4 |
| DirectDraw 7 video-memory query, GPU quirk database | Removed; DXGI adapter description where a value is still shown | P4.2 |
| Hardware gamma ramp | Gamma curve in the final blit shader | P4.2 |
| DirectPlay 8 (`NeuronRuntime`) | QUIC through `quinn`, TLS 1.3, behind the `Transport` trait | P1.9, P3.8 |
| GUN list server (`gundll.dll`) | Removed; address book and direct connect | P6.1 |
| DirectSound, EAX 2.0 (`SoundManager`, `SoundStreamer`) | XAudio2 with the original attenuation, pan and pitch arithmetic ported as plain Rust; XAudio2 reverb for the former EAX settings | P4.10 |
| Audio Compression Manager, MP3 codec found by display name | `symphonia` decoder | P4.10 |
| DirectShow, Windows Media reader, Video for Windows (`movie`) | Media Foundation source reader into a texture and an audio voice | P4.15 |
| DirectInput 8 mouse | Raw Input | P4.11 |
| IMM32 private calls, per-IME DLL probing (`imeui`) | Rust edit-box model; system-drawn IME windows positioned through documented calls | P4.11 |
| GDI text rasterising and metrics | DirectWrite in GDI-compatible measuring mode, into the same glyph atlas | P4.12 |
| MSXML SAX2 for rich-text markup | `quick-xml` | P4.12 |
| MFC admin application (`OutpostServer`) | Headless server with a config file and a command console | P3.9 |
| Registry for accounts, login history, table layouts | Files under `Documents\My Games\OutpostFrontier`, with a one-time import | P4.1 |
| Component manager, plug-in DLLs loaded by name, resource-only DLLs | Static composition; text catalogs built from the resource sources | P4.16, P1.6 |
| `LoadLibrary` back-links between executable, server and transport DLLs | Traits passed at startup | P3.2, P5.3 |
| x87 inline assembly (228 blocks), precision-control switching | `f32`/`f64` arithmetic with explicit helpers for conversions | P1.1, P1.4 |
| `timeGetTime`, named cross-process events | `std::time::Instant`; the cross-process tick handshake is removed | P1.5 |
| zlib DLL, minizip | `flate2` for network batches only | P1.9 |
| Lua 3.2 C interpreter (two copies) | Rust load-time evaluator | P4.14 |
| qhull, IVP physics | `parry3d` plus the project's own solver | P2.3 |
| EULA DLL, crash-report launcher, `Flasher.exe`, `dxdiag` launch, network-alive probe | Removed | P5.1 |
| ANSI Win32 calls, multi-byte strings | UTF-8 inside; UTF-16 only at file, wire and Windows boundaries | all |

## 5. How a port is accepted

[verification.md](verification.md) has the detail. In short:

- Every piece of behaviour is assigned a fidelity tier. Tier A is
  bit-exact (formats, hashes, parsers, the random generator, money rules,
  wire encodings). Tier B is equal within a numeric tolerance (math
  kernels, curves, audio arithmetic, physics and AI scenarios). Tier C is
  compared as images or by play-test (rendering, particles, text layout).
- Reference output comes from the C++ build through an oracle harness built
  in Phase 0.
- Vectors made from synthetic input are committed and run in CI. The
  original game data is committed in `outdir/` (decision O15); vectors made
  from it are not committed but produced when a test runs, and tests that
  need them read `OUTPOST_DATA_DIR` and `OUTPOST_ORACLE_DIR`.
- A work package is done when its tests pass at the assigned tier, its
  deliberate differences are entered in the divergence register, and its
  ledger rows are updated.

## 6. Phases

Sizes are relative (S, M, L, XL) and derive from the assessed C++ volume
and difficulty. The plan fixes order and gates, not dates; Phases 0 and 1
are the calibration for any schedule.

```text
Phase 0 ──> Phase 1 ──┬──> Phase 2 ──> Phase 3 ──┐
                      │                          ├──> Phase 5 ──> Phase 6 ──> Phase 7
                      └──> Phase 4 ──────────────┘
```

Phase 4 (client engine) depends only on Phase 1 and runs alongside Phases 2
and 3.

The owner started Phase 5 beside the rest of Phase 3 on 2026-11-25 (decision
O50), in a separate session that the owner runs; the session carrying Phase 3
does no Phase 5 work. Phase 5 begins with its own design record and with the
check whether the C++ client's `+oracle` mode runs in CI. Gate G5 still needs
the whole of Phase 3, and P5.9 and P5.10 wait for P3.6e and P3.7b on their
server side. O50 sets which session owns which crates and how the two keep the
divergence and decision registers apart.

### Phase 0 — make the migration verifiable

| ID | Work | Where it runs | Size |
| --- | --- | --- | --- |
| P0.1 | Harden the workspace: pin the toolchain, add workspace lints and the unsafe policy, `cargo-deny`, the `game-sim` crate, the corrected crate graph, an `xtask` crate for tooling, a host-target test job in CI, the test-data conventions. Update `AGENTS.md`, `README.md` and the port map | Anywhere | M |
| P0.2 | Restore the 32-bit C++ Release build. Needs Visual Studio with the v145 toolset and MFC for x86, the DirectX 8.1 SDK in `CppSource\DX8\` (committed, decision O11), Python, and the original `DATA` and `EXE` trees. Record the steps in `design/reference-build.md`. Permitted changes to `CppSource/` are build fixes and oracle instrumentation only | Owner's Windows machine | M |
| P0.3 | Read the facts the repository cannot answer from the real installation and record them in `design/facts.md` (list below) | Owner's Windows machine | S |
| P0.4 | Build the oracle harness: a console program that links the C++ DLLs and dumps vectors, and a data-dump mode that runs it over the installation. Emit the first synthetic suites. Each later work package adds the suites and dump modes it needs | Owner's Windows machine | L |
| P0.5 | Client instrumentation: a `+oracle` mode in the C++ client for interface-tree and screenshot dumps, and a logging proxy for the renderer interface that records call traces. Split from P0.4 on 2026-11-16; not part of Gate G0. Runs alongside Phases 1 to 4 and is needed before P4.0 fixes the trace format, and in any case before Gate G4 | Owner's Windows machine | M |

Facts for P0.3: the `[Libraries]` and `[System]` lists of the engine INI;
the `[Resources]` DLL order that fixes text ID prefixes; the `local_server`
value; whether `PHYSICAL_SIM_RATE` is overridden; whether `.thn` scripts
ship as text or as compiled chunks; the `maxSoundChannels` value; the movie
codec versions; the font faces the data expects; whether single player
keeps one object world or two (see P2.0; answered: one, decision O21).

**Gate G0.** The C++ build runs the game on the original data. The harness
produces vectors for at least name hashing, INI parsing and one data dump.
CI runs format, lint, Windows tests and host-target tests.

G0 passed on 2026-11-16. The game ran on the original data with commit
`816f556a`; the oracle vectors for hashing, IDs, numbers, the random
generator and both INI parsers pass in CI; the data dump of all 1256 INI
files in the master copy (`outdir\`) equals the Rust dump; all four CI jobs
passed on that commit. Facts F1 to F13 are recorded; F14 moved to P2.0. Evidence is
in [reference-build.md](reference-build.md) sections 5 to 7 and in
[facts.md](facts.md).

Phase 0 closed on 2026-11-17, when the owner ran P0.5 on the original data.
The `+oracle` mode ran `menu_walk.txt` with byte-identical output across
runs. `RenderTrace.dll` traced three frames of the same walk without
changing anything the client oracle records. `single_player.txt` reaches
the first base. Results are in [reference-build.md](reference-build.md)
sections 8 and 9.

If P0.2 fails for good (for example, a required closed binary cannot be
found), the fallback is decision O3's second option: vectors transliterated
from the C++ source plus tests against real data. That weakens every
Tier B and C claim and must be recorded as a change to this plan.

**Phase 1 before Gate G0.** Decided 2026-11-16: work packages P1.1 and P1.2
start while Phase 0 is open, together with the part of P0.4 they need. The
console harness in `CppSource/Oracle/` covers name hashing, IDs, number
parsing and formatting, the random generator and both INI parsers, and CI's
oracle job builds it and checks the committed vectors on every push. A
Phase 1 package started this way stays `in progress` until its oracle
vectors pass in CI and its data-dependent tests pass on the original data;
tests written from the C++ source alone count as provisional. The other
Phase 1 packages wait for G0: P1.4 needs x87 results the source cannot
give, P1.6 needs fact F3, and P1.7 and P1.8 need the format variants that
only the real `DATA` tree shows.

### Phase 1 — core formats and primitives (`neuron-core`)

| ID | C++ source | Rust destination | Size | Tier |
| --- | --- | --- | --- | --- |
| P1.1 | Name CRC and `stricmp` (`neuroncomponents`); `CreateID`, `MakeId`, `MakeLocationID`, sub-object ranges (`Common`); `msvcrt_compat.h`; legacy `atof` and the UCRT `%g`; float-to-int helpers; CRT random generator | `hash`, `id`, `msvcrt`, `num`, `rng` | S | A |
| P1.2 | Engine INI parser (`neuroncomponents`); game INI reader, legacy `atof` (`Common`) | `ini` | M | A |
| P1.3 | `ReadFile`: directory, UTF and memory file systems, search path | `vfs`, `utf` | M | A |
| P1.4 | `x86math` kernels in original operation order | `math` | M | B |
| P1.5 | `FDUMP` log sink, error callbacks, timing, tick-profile spans; rect, spline, MD5 text form | `diag`, `time`, `util` | S | A |
| P1.6 | Eight resource projects: convert the `.rc` and `.txt` sources to TOML in `outdir/data/text/` (T21, amended 2026-11-17), build catalogs with `xtask`, load by numeric ID | `catalog` | M | A |
| P1.7 | Pure parsers: TGA, DDS and pixel formats (`Shading`, D3DX use); mesh data (`RendComp` headers); animation channels; effect libraries (`particlefx`); material and texture nodes; `.sur` and compact surfaces (`Common`) | `image`, `mesh_data`, `anim`, `fx_data`, `sur` | L | A |
| P1.8 | `SceneRuntime`: instance tree, compound models, hardpoints, joints, animation mixer | `scene` | L | A for loading, B for evaluation |
| P1.9 | Wire reader and writer, quantisers, frame and handshake types, `Transport` trait, loopback transport | `net` | M | A |

Status on 2026-11-17: Phase 1 is complete. P1.1 to P1.9 are ported, their
oracle vectors pass in CI, and the data dumps over the original data in
`outdir/` (decision O15) equal the oracle dumps (`data.yml`, Data dumps run [44051215922](https://github.com/Zwaliebaba/Outpost.Frontier/actions/runs/44051215922)).
Gate G1 passed and accepted every package. The progress table in
[README.md](README.md) has the detail.

**Gate G1 (milestone "data loads").** Every file in the original `DATA`
tree that these readers cover loads without error, and the Rust dumps equal
the oracle dumps.

### Phase 2 — shared simulation (`game-sim`, `neuron-core::physics`)

| ID | C++ source | Rust destination | Size | Tier |
| --- | --- | --- | --- | --- |
| P2.0 | Design spikes, each ending in a short decision record under `design/adr/`: object model representation (kinds, handles, lifetimes); single-player world ownership (one shared world, decision O21; the earlier default of two worlds joined by loopback rested on a misreading of the `SP*` calls); the replay and scenario oracle and the G2 envelope bounds (O20) | — | S | — |
| P2.1 | `Common` data: constants, data config, the string cache (`common_stringcache.cpp`, moved from P1.1), archetypes, loadouts, fuse data, goods, reputation data, universe, zones, asteroid IDs, bases, room names and markets, spatial partition (room INI contents moved to P5.9, owner 2026-11-17) | `game-sim::data` | XL | A |
| P2.2 | `Common` marshal buffer, format strings, costume, binary rich text; the 162 packet types and codecs from `RemoteCommon` | `game-sim::protocol` | L | A |
| P2.3 | `PhySys` facade, `[PhySys]` constants, hull shapes from compact surfaces, integrator and impulse solver, controllers hook, rays, sphere queries, phantoms, remote correction, dead reckoning | `neuron-core::physics` | L | B |
| P2.4 | `Common` object model (`CObject` to `CShip`, `CSolar`, projectiles, loot), attachments, object database, equipment (`CE*`), damage. The shared remainder (asteroid field walk, bay, dock and gate animations, scanner and tractor updates, turret aiming, explosion bounding spheres, `HardpointSummary`, the inspection queries the AI needs) is a follow-up alongside P2.5, before Gate G2 (owner, 2026-11-17). The server's guided-missile controller (`Guided`: motor, seeker and steering, proximity fuse, detonation through `StarSystem::explode`) moved into this remainder from P3.3 (owner, 2026-11-18) | `game-sim::object`, `::equip` | XL | B |
| P2.5 | `Common` AI: steering, ideas, obstacle avoidance, pathfinding, targeting, fire control, 23 behaviours, behaviour manager, state graphs | `game-sim::ai` | XL | B |

Order and review (owner, 2026-11-17): P2.0 comes first and stops for the
owner to approve its decision records. P2.1, P2.2 and P2.3 then run in
parallel, followed by P2.4 and then P2.5. Each work package is one pull
request. P2.3 follows O19: free flight transliterated from IVP, contacts
through `parry3d` and the project's solver. From 2026-11-17 the owner lets
Claude merge a Phase 2 pull request once its CI and the real-data runs are
green; new decision records and new divergence rows still wait for the
owner. P2.5 starts with the parts that need no world (steering
mathematics, intercept, ray-cast helpers, the state-graph data) and the
harness's world-snapshot recorder while P2.4 is open. Before Gate G2 the
protocol suites also drive the C++ decoders, so that decoding is checked
against the C++ as encoding is.

**Gate G2.** A headless program loads a star system from the original data,
spawns ships and runs scripted scenarios (fly to a point, fire,
collide, and a dogfight between two hostile AI fighters, added by the owner
on 2026-11-18 so that targeting, fire control and the combat behaviours are
checked against the oracle). The dock scenario moved to Gate G3, because
docking is a server part of P3.3 (owner, 2026-11-18). The collide scenario
is `ram_ship`, in which a fighter flies into another one with the multiplayer
server's collision filter switched to its single-player branch (on the
multiplayer path only bullets and missiles collide; [reference-build.md](reference-build.md)
section 10, "Collisions"). It is judged to its first contact (record 0003
point 9): every object equals the oracle exactly before it, the contact
comes in the same tick, the first contact's damage is within 5 % of the
oracle's (owner, 2026-11-19), and after it only the outcome (which objects
are alive) is compared. The fights (`dogfight`, `skirmish`, `strafe`) are
judged the same way: exactly up to their first contact or munition impact,
whichever comes first, which must come in the oracle's tick (owner,
2026-11-19); after it their outcomes and trajectories are compared within
the approved envelopes, because the differences after a contact come from
the accepted contact solver (D87, decision O19) and the fuse's timing
(D177) (owner, 2026-11-18). The oracle's runs on one machine are identical
(its run-to-run spread is 0), so each of these scenarios, `fly_to_point`
and `fire_at_target` too, runs over a seed ensemble (owner, 2026-11-19;
record 0003 points 9 and 10): 60 fixed seeds at the seeding point on both
sides, every seed judged as above, and after the first contact the fights'
outcome statistics over the seeds (survival count, kill tick (the first
tick at hull 0), munition hits and collision events up to it, final hull
and shield) compared, Rust's distribution against the oracle's, within
the bounds of record 0003 point 10 (approved by the owner on
2026-11-19: Fisher's and the two-sample Kolmogorov–Smirnov tests at 1 %,
medians within 0.44 × the oracle's interquartile range or a floor). The scenarios
without contact keep the strict outcome check. `ram_asteroid`, in which neither the C++ pilot nor
the Rust one touches the rock, stays as a near-miss check but does not count for the
gate (owner, 2026-11-18). The scenarios are judged as recorded from the
seeding point: the harness calls `srand(seed)` again once the system and
Content's population of it have loaded, and the Rust world seeds there;
the `rand()` draws of the population (4,837 in he01, from Content's
`SystemLoaded` handler; [sim-records.md](sim-records.md), `load_rand.jsonl`)
are P3.6's and are checked at Gate G3 (owner, 2026-11-18). Replay tests of
the integrator, AI and damage code equal the oracle within Tier B bounds,
and scenario trajectories, times and damage fall inside the envelopes
approved in P2.0 (decision O20). All protocol codecs round-trip and match
the C++ encodings byte for byte. The gate's headless program is the
`scenario_replay` test in CI, which runs the oracle's scenario files
through the Rust world, together with the other replays; there is no
separate runner (owner, 2026-11-18).

### Phase 3 — server and network (`neuron-server`, `game-logic`, `apps/outpost-server`)

| ID | C++ source | Rust destination | Size | Tier |
| --- | --- | --- | --- | --- |
| P3.0 | Design spike: controller bus and re-entrant dispatch. The C++ nests message dispatch; Rust needs an event queue that preserves the observable order | `design/adr/` | S | — |
| P3.1 | `Server`: save container, account hashing and files, paths, session slots, controller registry, heartbeats, timers, tick driver | `neuron-server` | M | A |
| P3.2 | `Server`: typed events replacing 81 message IDs, client sink, character file schema and templates, player database, worth and cheat checks, groups, chat | `game-logic::{events, player, character_file}` | L | A |
| P3.3 | `Server`: star systems, object wrappers, munitions (with the mine and counter-measure controllers; the guided-missile controller moved to P2.4's remainder, owner 2026-11-18), fuses, loot drops, debris, docking, observers and replication, asteroid fields, base database (the server parts moved from P2.4, owner 2026-11-17; they remove D125, narrowed to the mines and counter-measures, and D126; D124 was removed by the P2.4 follow-up); the `pub::` functions become methods on the world | `game-logic::{world, objects, replication, bases}` | XL | B |
| P3.4 | `Content`: static tables, factions, loot, cargo, system connections | `game-logic::content::{tables, game_support}` (decision O47) | L | A |
| P3.5 | `Content`: mission script parser (44 conditions, 85 actions as enums) and runtime, mission object, object lists, dialogs, save sections, story shell | `game-logic::content::{mission, story}` | XL | A for parse and save, B for runtime |
| P3.6 | `Content`: encounters, spawn placement, patrol paths, NPC ships and pilots, guides, comm chatter, space support, dock queues, trade lanes | `game-logic::content::{population, npc, gcs, space_support, base_support}` | XL | B |
| P3.7 | `Content`: random mission database, offer generator, vignette tree, runtime machines | `game-logic::content::{random_mission, router}` | XL | A for offers from a seed, B for runtime |
| P3.8 | `NeuronRuntime` transport, `RemoteClient`, `RemoteServer`: QUIC host and connection, handshake with version and password, length-prefixed frames, batching, send-backlog governor, certificate pinning | `neuron-server::net`, `neuron-client::net` | M | B |
| P3.9 | `OutpostServer`: TOML config, command console (accounts, kick, ban, chat, news, status), logging | `apps/outpost-server`, `game-logic::admin` | M | — |
| P3.10a | `Server`: the client's flight and combat handlers (fire, equipment, cruise, thrusters, target, tractor, trade lanes, jettison, collision reports, mining, use item, invincibility, manoeuvre, dock); before Gate G3 (decision O49) | `game-logic::{session, receive}` and the world's owners | M | B |
| P3.10b | `Server`: the player-state requests (equipment and items, collision groups, visited state, mission log, interface state, rank, best path, group positions, pop-up dialogs, difficulty scale) and the player trades; after Gate G3 (decision O49) | `game-logic::{session, player}` | M | A |

**Gate G3 (milestone "headless server").** `OutpostServer.exe` starts on
the original data. A scripted test client connects over QUIC, logs in,
creates a character, launches and receives object updates. The
`dock_at_station` scenario (moved from Gate G2, owner 2026-11-18) and the
`trade_lane` and `dock_trade_lane` scenarios (moved from Gate G2 with the
lane request, `LaneAction` and `get_lane_start`, owner 2026-11-19) run
through the Rust world and fall inside their approved envelopes. Saves written by
Rust load in the C++ build and the reverse. Mission script dumps equal the
oracle. A random mission offer rebuilt from a saved seed equals the oracle.
P3.5b checks the save item for the story missions' and Story's sections
(`[MissionState]`, `[TriggerSave]`, the mission object's sections,
`[NNObjective]`, `[RandomEncounter]`, `[StoryInfo]`): the C++ writer's text
equals the port's for every mission in three states, the C++ build restores
the port's saves as the port does (the data job's "Load the port's saves
with the oracle"), and the port's saves round-trip; the `[RandomMission]`
sections follow with P3.7c. The owner decided on 2026-11-25 that the
two-client scripted test (`apps/outpost-server/tests/g3_client.rs`) meets
the scripted client criterion; NPC population (P3.6d) comes after the
milestone.

The owner re-sequenced Phase 3 on 2026-11-25 (decision O48): the packages
Gate G3 needs (P3.7a, P3.7c, P3.8, P3.9) come first, P3.8 and P3.9 on their
own branches beside Content, and the other Content packages finish the phase
after the milestone.

The owner signed off Gate G3 on 2026-11-25, on the data run of #165
(run 45614252212): every criterion above is met, the three scenarios bit for
bit equal to the oracle through the Rust server.

### Phase 4 — client engine (`neuron-client`)

| ID | C++ source | Rust destination | Size | Tier |
| --- | --- | --- | --- | --- |
| P4.0 | Design spike: the typed render-device API and the trace format used to replay C++ renderer calls | `design/adr/` | S | — |
| P4.1 | Client window, message loop, clock, user paths, per-user state files with registry import | `platform` | M | — |
| P4.2 | Direct3D 12 device, flip-model swap chain, heaps, per-frame upload ring, pipeline-state cache, scene target with final blit and gamma, screenshot readback | `render::d3d12` | L | C |
| P4.3 | `DX8RENDERER` behaviour: render and texture-stage state, transforms, lights, fog, texture coordinate generation, alpha test, vertex formats, primitive conversion, pixel-centre and handedness conventions; HLSL compiled at build time | `render::ff` | XL | C |
| P4.4 | `Shading` texture library and animation, `ximage` | `render::texture` | M | A for decode, C for output |
| P4.5 | `Shading` materials (22 names), material animation, batcher; `surfacematerials` (19 names), backdrop, starsphere, shadows | `render::material`, `render::appearance` | L | C |
| P4.6 | `RendComp`: render manager, level of detail, mesh kinds, wireframes | `render::mesh` | L | C |
| P4.7 | `Deformable2`: skeleton, CPU skinning, palette skinning in the vertex shader, eye material, aim solving | `render::deformable` | L | B, C |
| P4.8 | `particlefx`: node graph, emitters, fields, appearances, draw context | `render::particles` | XL | C |
| P4.9 | Programmable passes: render targets, custom HLSL programs, scene depth as a texture | `render::pass` | M | C |
| P4.10 | `SoundManager`, `SoundStreamer`: decode, mixer arithmetic, voice ranking, XAudio2 device, streaming, reverb | `audio` | L | B for arithmetic |
| P4.11 | Raw Input mouse, keyboard routing, cursor; `imeui` edit-box model and IME positioning | `input`, `text_input` | M | — |
| P4.12 | Client font code: DirectWrite rasteriser, atlas, font config; rich-text display lists and markup | `text` | L | C |
| P4.13 | Client UI toolkit: control tree, 62 control factories, INI and page-text loaders, draw layers, modal stack, typed messages, immediate-mode layer, widescreen anchoring; number-string formatting (`MsvcrtNumericVsnprintf` in `msvcrt_numeric_format.h`) | `ui` | XL | A for layout dumps, C for output |
| P4.14 | `SceneScript`: compiled-chunk reader and evaluator for the 15-opcode subset (O18) and its number text (`MsvcrtFormatLuaNumber` in `msvcrt_numeric_format.h`), script loader, curves, motion paths, timeline and events | `scene_script` | XL | A for load, B for playback |
| P4.15 | `movie`: Media Foundation playback of the startup clips | `media` | S | — |
| P4.16 | `neuroncomponents`, `system`, `NeuronRuntime::Startup`: the `Engine` value, construction and drop order | `engine` | S | — |

**Gate G4 (milestone "model viewer").** A viewer tool built on
`neuron-client` shows ships, stations, a skinned character and particle
effects from the original data, plays sounds and a scene script, and shows
the main menu layout. Recorded C++ renderer traces replay into the Rust
renderer and the images match the C++ screenshots within the image
tolerance. Synthetic scenes render on the software adapter in CI and match
committed images.

### Phase 5 — game client (`game-app`, `apps/outpost-frontier`)

| ID | Client sub-system | Size |
| --- | --- | --- |
| P5.1 | Boot sequence, config, loading screen, state machine | M |
| P5.2 | Front-end pages, options model, key map | L |
| P5.3 | Session: client event receiver, in-process server wiring, new game and load game | L |
| P5.4 | Client object database, game objects, star system, cameras, flight controls | XL |
| P5.5 | Sound archetypes, music, voices, ducking | M |
| P5.6 | Combat effects: projectiles, beams, explosions, debris, engine and equipment effects, lights | XL |
| P5.7 | HUD: status, target, contact list, waypoints, manoeuvres, weapon groups, comm | XL |
| P5.8 | Environment: asteroid fields, nebulae, trade lanes, jump tunnel, dust | XL |
| P5.9 | Bases: rooms (including the room INI contents, `RoomData::read_from_ini`, and the room sub-records of `common_roominfo.cpp`, moved from P2.1 because loading them runs the scene-script engine of P4.14), hotspots, scene-script hosts, characters, navigation bar | L |
| P5.10 | Base screens and shared windows: commerce, ship dealer, repair, news, mission vendor, navigation map, load and save, preferences, chat, mission briefing | XL |

Demonstrable steps inside the phase: main menu (P5.2), a loaded system
(P5.3), flight (P5.4, milestone "fly"), combat (P5.7), landing and trading
(P5.10).

**Gate G5 (milestone "campaign").** The single-player campaign is playable
from a new game to the end on the original data. Saves interchange with the
C++ build at any point. Interface-tree dumps equal the oracle for every
screen.

### Phase 6 — multiplayer and extras

| ID | Work | Size |
| --- | --- | --- |
| P6.1 | Multiplayer client flow: address book and direct connect, login, character pages, chat, player trade, groups | L |
| P6.2 | Dedicated server under load: 128 slots, governor tuning against QUIC backlog, soak test | M |
| P6.3 | Walk mode in base rooms | M |
| P6.4 | Volumetric nebula on the programmable-pass API. The C++ loads `nebula_volume.hlsl`, `nebula_composite.hlsl`, `common.hlsli` and `debug_depth.hlsl` from `DATA_EXTRA\SHADERS` and its settings from `DATA_EXTRA\VOLUMETRIC\volumetric.ini`; none of these exist (fact F12). Decided 2026-11-17: new shaders are written from the register layout, loop bounds and settings that `nebula_volume.cpp` documents, kept in `neuron-client/shaders/` with compiled blobs in `neuron-client/compiled_shaders/` (decision T1), and the settings get defaults in code. Accepted by eye (Tier C without reference images) | M |
| P6.5 | Tick-profile overlay on the tracing spans | S |

**Gate G6 (milestone "multiplayer").** Several clients play on one
dedicated server across a session with launches, docking, combat, trade and
chat. The three extras work and can each be switched off.

### Phase 7 — sign-off and removal

| ID | Work |
| --- | --- |
| P7.1 | Parity audit: every ledger row accepted; divergence register reviewed and signed off by the owner |
| P7.2 | Legacy scan: the executables import none of `d3d8`, `d3d9`, `ddraw`, `dsound`, `dinput8`, `msacm32`, `quartz`, `msvfw32`, `mfc*`; the source enables no DirectPlay, DirectShow, DirectSound or DirectInput API; no `asm!` |
| P7.3 | Tag the last commit that contains the C++ as `cpp-reference-final`. Archive the oracle vectors. Delete `CppSource/` in one commit. Turn the port map into an architecture note; update `README.md`, `AGENTS.md` and `CONTRIBUTING.md` |
| P7.4 | Keep the committed vectors as regression tests; drop tests that need a live oracle |

**Gate G7 (milestone "C++ gone").** `CppSource/` is absent from the main
branch, CI is green, and the legacy scan is part of CI.

## 7. Working rules during the migration

- `CppSource/` is frozen. The only permitted changes are build fixes and
  oracle instrumentation from Phase 0. The recorded exceptions are the
  plain-text save writer (decision O12) the directory `FindFirst` fix
  (decision O14), the costume hash in `SetRellan` (decision O17) and the
  zero-filled `BuildOrientationBasis` result (decision O24).
- One work package is one or more pull requests against the owning crate.
  Each updates the ledger.
- Unknown fields in the reconstruction (about 11,000) are carried as
  explicitly named unknowns. Their meaning is not invented; behaviour is
  captured from the oracle.
- Baseline undefined behaviour is not reproduced. Each case gets defined
  behaviour and an entry in the divergence register.
- Port-only features (widescreen, walk mode, volumetric nebula, tick
  profiler) become Cargo features, all in scope (decision O7).
- Windows API calls appear only in `neuron-client` and in the executable
  packages. Everything else must build and test on a non-Windows host, so
  that contributors and agents without a Windows machine can run the tests
  for most of the code.

## 8. Risks

| Risk | Effect | Response |
| --- | --- | --- |
| The C++ reference build cannot be restored | No oracle; fidelity claims weaken | Phase 0 comes first and has a recorded fallback |
| Volume: about 820,000 lines, of which roughly half needs a real port | The effort is large even with the deletions | Milestones give usable results early; Phase 4 runs in parallel; size is recalibrated after Phase 1 |
| Reconstruction semantics are incomplete (unknown fields, unnamed virtual slots) | A port may copy a misunderstanding | Behaviour is checked against the oracle, not against the reading of the source |
| Nested message dispatch between server and content does not fit Rust borrowing | A wrong queue design changes event order | Spike P3.0 before any content code; scheduler vectors |
| Physics replacement changes flight feel and collision damage | Gameplay drift | Original constants honoured literally; scenario envelopes at Gate G2; owner play-test |
| Fixed-function emulation differs from Direct3D 8 (lighting, fog, pixel centres) | Visual drift, blurred text | Trace replay and image comparison at Gate G4 |
| Text metrics differ between GDI and DirectWrite | Line breaks and autoscaling move | GDI-compatible measuring mode; layout dumps compared; a GDI backend may be kept as a test-only reference |
| The original data was not in the repository (resolved by decision O15: it is in `outdir/`) | CI could not run data tests | Synthetic vectors in CI; data tests can now run in CI against `outdir/` |
| The C++ tree derives from a commercial binary, the physics code carries no licence, and the original data is published in the repository (decision O15) | Project-level legal exposure outside this plan's control; the owner accepted it for the data | Not ported: IVP, the EULA and list-server binaries. Dumps and vectors made from the data are not committed |
| QUIC needs an async runtime inside engine crates | Threading complexity | One I/O thread owns the runtime; the game thread polls a channel, as the DirectPlay code already does |
| The rebrand commit `89eda18` changed names in the reference; the data's text files carry the new names, but binary files and hashes compiled into the code kept old ones ([rebrand-names.md](rebrand-names.md)) | The reference misses the matching data where the code compares with an old name | Decision O17 renamed the binary data and fixed the one hash found; a name built at run time or a hash of a name not in the scan can still turn up in later phases |
| Deleting the C++ only at the end hides progress | Stakeholders see no shrinkage | The ledger and the seven milestones are the progress record |

## 9. What happens first

1. P0.1 can start now; it needs no Windows machine.
2. P0.2 and P0.3 need the owner's machine, the DirectX 8.1 SDK and the
   original installation.
3. P1.1 to P1.3 can be written against vectors transliterated from the
   source while P0.4 is in progress, and are re-checked against the oracle
   before Gate G1.
