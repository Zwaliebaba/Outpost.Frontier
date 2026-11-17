# Design

Plan for migrating the C++ code base in `CppSource/` to the Rust workspace.

| Document | Content |
| --- | --- |
| [migration-plan.md](migration-plan.md) | Scope, target architecture, legacy replacements, phases, work packages, gates, risks |
| [decisions.md](decisions.md) | Owner decisions and technical decisions with the alternatives rejected |
| [library-ledger.md](library-ledger.md) | All 39 C++ projects and shared source groups: disposition, Rust destination, work packages, status |
| [port-map.md](port-map.md) | Which Rust crate owns each C++ project and source group, and what it is responsible for |
| [verification.md](verification.md) | Fidelity tiers, the oracle, test data handling, CI, definition of done, gates |
| [reference-build.md](reference-build.md) | How to build and run the C++ as the reference: DirectX 8.1 SDK, folder layout, build, run |
| [sim-records.md](sim-records.md) | The record layouts of the scenario and replay oracle, `Oracle.exe sim` (P2.3) |
| [facts.md](facts.md) | Facts to collect from the original installation (P0.3) |
| [divergences.md](divergences.md) | Deliberate differences between the Rust port and the C++ |
| [sim-records.md](sim-records.md) | Layout of the scenario oracle's per-PSI free-flight records (P2.3) |
| [rebrand-names.md](rebrand-names.md) | Names the rebrand changed in the reference that the original data still uses (open) |
| [assessment/](assessment/) | Evidence: eight assessments of the C++ tree, made on 2026-11-15 |

## Progress

| Work package | State |
| --- | --- |
| P0.1 Workspace hardening | Done: pinned toolchain, workspace lints, `game-sim` crate, `xtask check-policy`, `cargo-deny`, host and policy CI jobs, updated `AGENTS.md`, `README.md` and port map |
| P0.2 Reference build | Done: Debug and Release `Win32` build on toolset v145 (commit `a86a199`); single player runs on the original data (commit `816f556a`), the Gate G0 check. Steps and fixes in [reference-build.md](reference-build.md). The plain-text save writer in that commit is decision O12 |
| P0.3 Facts | Done: F1 to F13 recorded in [facts.md](facts.md); F14 moved to the P2.0 design spike |
| P0.4 Oracle harness | Done: the console harness in `CppSource/Oracle/` covers name hashing, IDs, number parsing and formatting, the random generator and both INI parsers; CI's oracle job builds it and checks the committed vectors. The data dump of the master in `outdir\` (1256 INI files) equals the Rust dump. Later work packages add their own suites |
| P0.5 Client instrumentation | In progress: the `+oracle` client mode (`CppSource/OutpostFrontier/Client/oracle_*`, interface trees, key map, layouts, screenshots, the `IClient` call log, scripted sessions) and the renderer trace proxy `RenderTrace.dll` (all seven renderer interfaces, trace format version 0), described in [reference-build.md](reference-build.md) sections 8 and 9. CI's Reference build compiles both and checks their generated proxies. Open: one owner run of each on the original data, which closes Phase 0 |
| Gate G0 | Passed 2026-11-16 ([migration plan](migration-plan.md) section 6) |
| P1.1 Core primitives | Accepted at Gate G1: `hash`, `id`, `msvcrt`, `num`, `rng` in `neuron-core`. Oracle vectors confirmed in CI. The string cache moved to P2.1. |
| P1.2 INI formats | Accepted at Gate G1: `neuron-core::ini` (engine parser, `INI_Reader`; text only, decision O13). Oracle vectors confirmed in CI; the data dump of the master in `outdir\` shows 0 differences. |
| P1.3 File systems | Accepted at Gate G1: `neuron-core::utf` (UTF container, `FindFirst` matcher, DOS times) and `neuron-core::vfs` (`FileSystem` trait; directory, UTF and memory file systems; search path; path rules). Seven oracle suites; oracle vectors confirmed in CI; the Python UTF walker agrees with the Rust reader on the fixtures. The directory `FindFirst` defect is fixed in both builds (decision O14), checked against `FindFirstFileA` on Windows. The data dump over the original data's UTF files (3,072 files) equals the oracle |
| P1.4 Math kernels | Accepted at Gate G1: `neuron-core::math`, every `x86math` kernel in the reference's operation order on the 53-bit model, aliased-call variants included. Oracle vectors equal the C++ output in CI (NaN written as one pattern). |
| P1.5 Diagnostics, time, utilities | Accepted at Gate G1: `neuron-core::diag` (FDUMP flags, the `ErrorHandler` channels, error callbacks, tick profile behind feature `tick-profile`), `neuron-core::time` (`Timing`, `Delta`), `neuron-core::util` (rect, Catmull-Rom spline, state machine, MD5 text form). Oracle vectors confirmed in CI. |
| P1.6 Text catalogs | Accepted at Gate G1: sources in `outdir/data/text/`, one TOML file per module (converted from the `.rc` scripts and cards on 2026-11-17, catalogs byte-identical), `cargo xtask catalog` builds one catalog per module, `neuron-core::catalog` resolves text IDs. CI builds the eight resource DLLs and finds every string and card equal to the catalogs. |
| P1.7 Asset parsers | Accepted at Gate G1: `neuron-core::{sur, anim, fx_data, image, material_data, mesh_data}`. Data dumps equal the oracle on the original data in CI (`data.yml`): 798 `.sur`, 192 animation archives, 596 `.ale`, 1,939 mesh archives, 204 `.dfm`, 1,431 material archives, 1,413 texture libraries. |
| P1.8 Scene runtime | Accepted at Gate G1: `neuron-core::scene` (engine, compound models from `Cmpnd`, instance trees, joints, hardpoints, channel sampling, typed channels, the mixer, script playback). Three oracle suites (`scene_load`, `scene_sample`, `scene_eval`) confirmed in CI; data dumps over the original data: 2,056 archetype files load and equal the oracle (715 compounds, 15,126 parts, 14,411 constraints), 181 animation files evaluate equal to it. Undefined behaviour of the reference in D74 to D85 |
| P1.9 Wire and transport | Accepted at Gate G1: `neuron-core::net` (wire reader and writer, the RemoteCommon quantisers, T17 framing and handshake, `Transport` trait, loopback transport). Oracle vectors for the quantisers and field primitives confirmed in CI. |
| Gate G1 | Passed 2026-11-17 on Data dumps run [44051215922](https://github.com/Zwaliebaba/Outpost.Frontier/actions/runs/44051215922): every data dump equals the oracle ([migration plan](migration-plan.md) section 6) |
| P2.0 Design spikes | Done: decision records [0001](adr/0001-object-model.md) (object model), [0002](adr/0002-single-player-world.md) (single player keeps one world, answering F14) and [0003](adr/0003-simulation-oracle.md) (scenario and replay oracle, envelope method) accepted by the owner (O21 to O23). The `sim` oracle harness is built with P2.3 |
| P2.2 Protocol | Merged (PR #33); encodings byte-exact with the C++ codecs in CI: `game-sim::protocol` holds the 162 message types of `RemoteCommon` as three channel enums (`CommonMessage` 21, `ServerMessage` 81, `ClientMessage` 60) with little-endian codecs that keep wire values (quantised bytes, small archetype IDs), the single-player forms of the seven messages whose C++ codecs ask `SinglePlayer()` or `IsMPServer()`, and `Common`'s `MarshalBuffer`, `FmtStr`, costume forms and binary RDL records. Six oracle suites (`CppSource/Oracle/oracle_protocol*.cpp`, vectors in `crates/game-sim/tests/vectors/`, 161 packets encoded by their own C++ `Encode`; `TRADERESPONSE` has none, D107) blessed from Rust and not yet run in CI; property tests for every message. Differences D97 to D109 |
| P2.3 Physics | Implemented; replay of all 15,840 recorded PSIs within the bound, scenarios with zero run-to-run spread: `neuron-core::physics`. `[PhySys]` constants, the facade keyed by body handles, free flight transliterated from IVP (core step, controllers through a hook and the push API, damping, `calc_next_PSI_matrix`, anomaly limits, sleep), both stepping modes, contacts on `parry3d` with the project's impulse solver and continuous collision, rays, sphere queries, phantoms, beams, `CalcCollisionDamage`, dead reckoning, remote correction and golems. The per-PSI record layout is in [sim-records.md](sim-records.md); the `sim_replay` test waits for the harness's records. Divergences D87 to D96 |

## Assessments

The assessments were written by parallel read-only reviews of the source.
Nothing was built or run. Counts from text searches are approximate, and
each report marks what it could not verify.

| Report | Covers |
| --- | --- |
| [01-foundation](assessment/01-foundation.md) | `neuroncomponents`, `system`, `NeuronRuntime`, `zlib`, `ReadFile`, `luaprofile`, `x86math`, `MemoryDiagnostics` |
| [02-simulation-core](assessment/02-simulation-core.md) | `Common`, `IVP`, `qhull` |
| [03-network-stack](assessment/03-network-stack.md) | `rpclocal`, `RemoteCommon`, `RemoteClient`, `RemoteServer`, `OutpostServer`, DirectPlay use |
| [04-server-content](assessment/04-server-content.md) | `Server`, `Content` |
| [05-rendering](assessment/05-rendering.md) | Both renderers, `RendComp`, `Shading`, `ximage`, `surfacematerials`, `SceneRuntime`, `Deformable2`, `particlefx` |
| [06-client-app](assessment/06-client-app.md) | `OutpostFrontier` |
| [07-media-script-resources](assessment/07-media-script-resources.md) | `SceneScript`, `movie`, `SoundManager`, `SoundStreamer`, `imeui`, the resource projects, DirectInput use |
| [08-cross-cutting](assessment/08-cross-cutting.md) | Build system, dependency graph, 64-bit and numeric hazards, formats, third-party code, workspace readiness |

Each report ends with open questions. Those that needed the owner are
answered in [decisions.md](decisions.md); those that need the original
installation are listed as facts to collect in work package P0.3.
