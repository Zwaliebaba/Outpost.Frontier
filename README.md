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
| `neuron-core` | NeuronCore provides shared engine facilities. |
| `neuron-client` | NeuronClient provides the client engine. |
| `neuron-server` | NeuronServer provides the server engine. |

```text
OutpostFrontier.exe -> GameApp   -> NeuronClient -> NeuronCore
OutpostServer.exe   -> GameLogic -> NeuronServer -> NeuronCore
```

The [port map](docs/port-map.md) assigns the C++ subsystems to their Rust
owners. See [AGENTS.md](AGENTS.md) for naming, dependency boundaries, and
the checks run by [Windows CI](.github/workflows/build.yml).
