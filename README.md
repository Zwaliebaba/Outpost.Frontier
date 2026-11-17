# Outpost:Frontier

Outpost:Frontier is a Windows x64 game workspace using Rust 2024. Its two
executables currently start their respective Rust crate stacks, print the
application role and engine version, and exit.

## Build on Windows

Install Rust with rustup and the x64 MSVC build tools and Windows SDK. The
workspace selects stable Rust and `x86_64-pc-windows-msvc`. See the
[Rust Windows MSVC target documentation](https://doc.rust-lang.org/rustc/platform-support/windows-msvc.html).

```powershell
cargo build --locked --release --workspace --bins
.\target\x86_64-pc-windows-msvc\release\OutpostFrontier.exe
.\target\x86_64-pc-windows-msvc\release\OutpostServer.exe
```

## Workspace

| Package | Responsibility |
| --- | --- |
| `outpost-frontier` | Builds `OutpostFrontier.exe` and enters GameApp. |
| `outpost-server` | Builds `OutpostServer.exe` and enters GameLogic. |
| `game-app` | GameApp handles client game flow and presentation. |
| `game-logic` | GameLogic handles authoritative game rules and state. |
| `game-sim` | GameSim holds the simulation that client and server share. |
| `neuron-core` | NeuronCore provides shared engine facilities. |
| `neuron-client` | NeuronClient provides the client engine. |
| `neuron-server` | NeuronServer provides the server engine. |
| `xtask` | Workspace tooling, run as `cargo xtask`. |

```text
OutpostFrontier.exe ─┬─> GameApp ───> NeuronClient ─┐
                     │      └──> GameSim ───────────┤
                     │             ^                ├─> NeuronCore
                     └─> GameLogic ┴─> NeuronServer ┘
OutpostServer.exe ─────> GameLogic
```

The executable links GameLogic to host the server in-process for single
player. Edges appear in the manifests as the code that needs them is ported;
`cargo xtask check-policy` rejects any edge outside this graph.

NeuronCore already holds the ported Phase 1 modules: `hash`, `id`, `num`,
`msvcrt`, `rng`, `ini`, `utf`, `vfs`, `math`, `diag`, `time`, `util`,
`catalog` and `net`. `cargo xtask` offers `check-policy` (crate graph,
legacy APIs, unsafe placement), `ini` (list INI files through the ported
reader) and `catalog` (build the text catalogs from `resources/text/`).

## Migration from C++

`CppSource/` holds the C++ code base being migrated. The plan, decisions and
per-library status are in [design/](design/README.md). To build the C++ as
the reference, follow [design/reference-build.md](design/reference-build.md).

The [port map](design/port-map.md) assigns the C++ subsystems to their Rust
owners. See [AGENTS.md](AGENTS.md) for naming, dependency boundaries, and
the checks run by [CI](.github/workflows/build.yml).
