# Design

Plan for migrating the C++ code base in `CppSource/` to the Rust workspace.

| Document | Content |
| --- | --- |
| [migration-plan.md](migration-plan.md) | Scope, target architecture, legacy replacements, phases, work packages, gates, risks |
| [decisions.md](decisions.md) | Owner decisions and technical decisions with the alternatives rejected |
| [library-ledger.md](library-ledger.md) | All 39 C++ projects and shared source groups: disposition, Rust destination, work packages, status |
| [verification.md](verification.md) | Fidelity tiers, the oracle, test data handling, CI, definition of done, gates |
| [reference-build.md](reference-build.md) | How to build and run the C++ as the reference: DirectX 8.1 SDK, folder layout, build, run |
| [facts.md](facts.md) | Facts to collect from the original installation (P0.3) |
| [divergences.md](divergences.md) | Deliberate differences between the Rust port and the C++ |
| [assessment/](assessment/) | Evidence: eight assessments of the C++ tree, made on 2026-11-15 |

## Progress

| Work package | State |
| --- | --- |
| P0.1 Workspace hardening | Done: pinned toolchain, workspace lints, `game-sim` crate, `xtask check-policy`, `cargo-deny`, host and policy CI jobs, updated `AGENTS.md`, `README.md` and port map |
| P0.2 Reference build | Done: Debug and Release `Win32` build on toolset v145 (commit `a86a199`); single player runs on the original data (commit `816f556a`), the Gate G0 check. Steps and fixes in [reference-build.md](reference-build.md). The plain-text save writer in that commit is decision O12 |
| P0.3 Facts | Done: F1 to F13 recorded in [facts.md](facts.md); F14 moved to the P2.0 design spike |
| P0.4 Oracle harness | Done: the console harness in `CppSource/Oracle/` covers name hashing, IDs, number parsing and formatting, the random generator and both INI parsers; CI's oracle job builds it and checks the committed vectors. The data dump of the master in `outdir\` (1256 INI files) equals the Rust dump. Later work packages add their own suites |
| P0.5 Client instrumentation | Not started: the `+oracle` client mode and the renderer logging proxy, split from P0.4 on 2026-11-16. Needed before P4.0 and Gate G4; not part of G0 |
| Gate G0 | Passed 2026-11-16 ([migration plan](migration-plan.md) section 6) |
| P1.1 Core primitives | In progress, started before G0 (migration plan section 6): `hash`, `id`, `msvcrt`, `num`, `rng` in `neuron-core`. Oracle vectors confirmed in CI. The string cache moved to P2.1. Open: acceptance at Gate G1 |
| P1.2 INI formats | In progress, started before G0: `neuron-core::ini` (engine parser, `INI_Reader`; text only, decision O13). Oracle vectors confirmed in CI; the data dump of the master in `outdir\` shows 0 differences. Open: acceptance at Gate G1 |

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
