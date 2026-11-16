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
| P0.2 Reference build | In progress: Debug and Release `Win32` build without errors on toolset v145 (commit `a86a199`). Open: the run on the original data, which is the Gate G0 check. Steps in [reference-build.md](reference-build.md) |
| P0.3 Facts | Waiting for P0.2; form in [facts.md](facts.md) |
| P0.4 Oracle harness | Not started |

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
