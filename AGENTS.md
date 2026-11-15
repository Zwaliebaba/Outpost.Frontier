# Outpost:Frontier development

Build and test the Rust workspace from the repository root. Cargo defaults to
`x86_64-pc-windows-msvc`. The Windows binary targets are `OutpostFrontier`
and `OutpostServer`.

## Ownership and dependencies

- `apps/outpost-frontier` starts GameApp, which uses NeuronClient.
- `apps/outpost-server` starts GameLogic, which uses NeuronServer.
- NeuronClient and NeuronServer both use NeuronCore.
- NeuronCore owns shared engine data, formats, geometry, physics primitives,
  and message definitions.
- NeuronClient owns rendering, audio, input, and client-side transport.
  GameApp owns game screens, presentation, and player input actions.
- NeuronServer owns hosting, sessions, and persistence infrastructure.
  GameLogic owns authoritative simulation rules, missions, encounters, and
  player state.
- Keep startup wiring in `apps/` and put reusable code in the owning crate
  under `crates/`. Keep platform calls at the engine boundary.

## Naming and implementation

Use kebab-case for Cargo package names and their directories. Use snake_case
for imported crate names, modules, files, functions, and fields. Use
UpperCamelCase for types, traits, and enum variants, and SCREAMING_SNAKE_CASE
for constants. Keep the two Windows binary names in PascalCase. Run rustfmt.

Use Rust ownership and data types to express engine interfaces. Make byte
order and field widths explicit for persistent or network data.

Use the current subsystem responsibilities in the [port map](docs/port-map.md)
to place new modules. Validate ports with tests for the relevant input format
or observable behavior.

## Local checks

```powershell
cargo fmt --all --check
cargo clippy --locked --workspace --all-targets -- -D warnings
cargo test --locked --workspace --all-targets
cargo build --locked --release --workspace --bins
```
