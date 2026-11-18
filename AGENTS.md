# Outpost:Frontier development

Build and test the Rust workspace from the repository root. Cargo defaults to
`x86_64-pc-windows-msvc`. The Windows binary targets are `OutpostFrontier`
and `OutpostServer`. The toolchain is pinned in `rust-toolchain.toml`.

The C++ code base in `CppSource/` is being migrated to this workspace. The
plan, decisions and per-library ledger are in [design/](design/README.md).
`CppSource/` is frozen: change it only to fix the reference build or to add
oracle instrumentation (design/migration-plan.md, Phase 0). The recorded
exceptions are the plain-text save writer (design/decisions.md O12) the
directory `FindFirst` fix (O14), the costume hash in `SetRellan` (O17), the
zero-filled `BuildOrientationBasis` result (O24) and the three data names the
rebrand broke in code literals (O30).

## Ownership and dependencies

- `apps/outpost-frontier` starts GameApp, which uses NeuronClient. For single
  player it also hosts GameLogic in the same process.
- `apps/outpost-server` starts GameLogic, which uses NeuronServer.
- GameApp and GameLogic both use GameSim. GameApp never depends on GameLogic;
  the executable connects them through a transport.
- NeuronClient, NeuronServer and GameSim use NeuronCore.
- NeuronCore owns shared engine data, formats, geometry, physics primitives,
  wire primitives and the transport interface. It contains no platform code.
- GameSim owns game data, the space object model, equipment, flight AI and
  the message catalog that client and server share.
- NeuronClient owns rendering, audio, input, text, the UI toolkit and
  client-side transport. GameApp owns game screens, presentation, and player
  input actions.
- NeuronServer owns hosting, sessions, and persistence infrastructure.
  GameLogic owns authoritative simulation rules, missions, encounters, and
  player state.
- Keep startup wiring in `apps/` and put reusable code in the owning crate
  under `crates/`.
- `cargo xtask check-policy` enforces these edges. The table is in
  `xtask/src/graph.rs`; a new crate needs a row there and a line here.

## Platform code and unsafe

- Windows API calls belong in NeuronClient and in the executable packages,
  behind `cfg(windows)`. Every other crate must build and test on any host.
- Unsafe code is denied in the whole workspace and forbidden in GameSim,
  GameLogic, GameApp, NeuronServer and the executables. A module may allow
  it only in a directory listed in `xtask/src/scan.rs`. Every unsafe block
  carries a `// SAFETY:` comment.
- No legacy platform APIs (Direct3D 8/9, D3DX, DirectDraw, DirectPlay,
  DirectSound, DirectInput, DirectShow, ACM, winmm timers, GDI text) and no
  inline assembly. `cargo xtask check-policy` scans for them.

## Naming and implementation

Use kebab-case for Cargo package names and their directories. Use snake_case
for imported crate names, modules, files, functions, and fields. Use
UpperCamelCase for types, traits, and enum variants, and SCREAMING_SNAKE_CASE
for constants. Keep the two Windows binary names in PascalCase. Run rustfmt.

Use Rust ownership and data types to express engine interfaces. Make byte
order and field widths explicit for persistent or network data.

Numbers follow design/decisions.md T11: no fused multiply-add (clippy
rejects `mul_add`); C++ arithmetic evaluated in `f64` and rounded to `f32`
at each store the reference makes to a `float`, as its 53-bit x87 mode
does; and the conversion helpers in NeuronCore instead of bare float-to-int
`as` casts where the C++ result matters.

Use the current subsystem responsibilities in the [port map](design/port-map.md)
to place new modules. Validate ports with tests for the relevant input format
or observable behavior.

## Tests and test data

- Vectors made from synthetic input go in `<crate>/tests/vectors/`,
  hand-made fixtures in `<crate>/tests/fixtures/`. Both are committed.
- Committed vectors are the C++ oracle's output. CI's oracle job rebuilds
  `CppSource/Oracle` and fails when its output differs from them. Rewrite
  them from Rust (`OUTPOST_BLESS_VECTORS=1`) only to add a suite, never to
  make a failing comparison pass.
- The original game data is committed in `outdir/` (design/decisions.md
  O15): the master copy's INI files at its top, the `DATA` tree in
  `outdir/data/`. Add no other copy of it, and commit no dumps or vectors
  made from it; tests produce those from `outdir/` when they run. Tests
  that compare with the C++ oracle's data dumps read `OUTPOST_DATA_DIR`
  and `OUTPOST_ORACLE_DIR` and return early with a printed notice when a
  variable is unset.
- The DirectX 8.1 SDK headers and libraries in `CppSource/DX8/` are
  committed for the reference build (design/decisions.md O11). Commit no
  other SDK, and do not use these files outside `CppSource/`.
- Record every deliberate difference from the C++ behaviour in
  [design/divergences.md](design/divergences.md).
- Text resources live in `outdir/data/text/`, one TOML file per module,
  converted from `CppSource/Resources/`; `cargo xtask catalog` builds the
  catalogs. Until P7.3 a text change must be made in both places, because
  CI compares the resource DLLs with the catalogs.

## Local checks

```powershell
cargo fmt --all --check
cargo clippy --locked --workspace --all-targets -- -D warnings
cargo test --locked --workspace --all-targets
cargo build --locked --release --workspace --bins
cargo xtask check-policy
cargo deny check
```

On a host other than Windows, add `--target host-tuple` to the clippy and
test commands; the Windows build and tests run in CI.
