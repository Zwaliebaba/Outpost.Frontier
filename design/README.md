# Design

Plan for migrating the C++ code base in `CppSource/` to the Rust workspace.

| Document | Content |
| --- | --- |
| [migration-plan.md](migration-plan.md) | Scope, target architecture, legacy replacements, phases, work packages, gates, risks |
| [decisions.md](decisions.md) | Owner decisions and technical decisions with the alternatives rejected |
| [library-ledger.md](library-ledger.md) | All 39 C++ projects and shared source groups: disposition, Rust destination, work packages, status |
| [verification.md](verification.md) | Fidelity tiers, the oracle, test data handling, CI, definition of done, gates |
| [assessment/](assessment/) | Evidence: eight assessments of the C++ tree, made on 2026-11-15 |

Until work package P0.1 updates `AGENTS.md` and `docs/port-map.md`, this
plan takes precedence where they disagree. The differences are the new
`game-sim` crate, the message catalog living in `game-sim` instead of
`neuron-core`, and `OutpostFrontier.exe` linking `game-logic` for single
player.

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
