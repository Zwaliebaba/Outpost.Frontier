# Design/ — Outpost: Frontier

Output of the architecture & design session of 2026-08-17. The eight open questions from the
session brief are settled, in order, one ADR each; the four session deliverables sit beside
them. ADRs 009–014 record owner directives and rulings that arrived after the session and,
where they overturn an earlier decision, say so in their header. `ScreenPrints/` is the pre-existing
reference corpus these documents align with.

**Supersessions to be aware of when reading older ADRs:** ADR-012 replaces ADR-008's command
line (there is no argv) and ADR-009's line-oriented universe format (it is JSON); ADR-010
deletes the `NeuronCore/Math.h` the Dependency Map originally planned; ADR-013 replaces every
subdirectory path used illustratively in earlier documents with flat file names; **ADR-014
overturns Dependency Map ruling #2** — the engine libraries do not link GameLogic, and reach it
through injected interfaces instead.

## Decisions at a glance

| ADR | Question | Decision (one line) |
|---|---|---|
| [001](ADR/ADR-001-spatial-model.md) | Spatial model | **2D authoritative plane, 3D presentation**; cosmetic-only vertical offsets; 40 km float32 grid, grid-graph universe later |
| [002](ADR/ADR-002-server-tick-and-time.md) | Tick model | **Fixed 20 Hz** (50 ms, `u32` tick); snapshot per tick; client interpolates at −100 ms, extrapolates ≤ 250 ms → STALE |
| [003](ADR/ADR-003-transport.md) | Transport | **QUIC-shaped `Transport` now, UDP loopback impl first**, msquic spike slice pre-MVP; 1,152 B datagram cap everywhere |
| [004](ADR/ADR-004-wire-protocol.md) | Wire protocol | **Hand-rolled little-endian, full snapshots every tick** (delta field reserved), acked order stream with shared reason codes, fail-closed schema hash |
| [005](ADR/ADR-005-gamelogic-entities-orders-determinism.md) | Entity/state | **Fixed-schema SoA tables, no ECS**; group orders w/ 4-leg queues; pure shared formation-solve + validation; **same-binary replay determinism only** |
| [006](ADR/ADR-006-renderer.md) | Renderer | **Fixed forward pass list (no frame graph), ortho at 30°** (the 2:1 ring spec), instanced flat-shading, DWrite glyph atlas, plane-picking |
| [007](ADR/ADR-007-threading-model.md) | Threading | **Two owning threads (Main, Sim)**; single-writer worlds; transport-only crossings; lane registry + Extract seam from day one |
| [008](ADR/ADR-008-inprocess-hosting.md) | Hosting | **Composition-root exe; `ServerHost` service object**; headless mode proves the split continuously; normative shutdown order |
| [009](ADR/ADR-009-universe-model.md) | Universe *(owner directive)* | **`int64 × int64` metre universe plane**; systems with planets and 1–2 stations, gates as graph edges; grids anchored at exact universe positions with local float32 sim; authored `GameData/Universe/`, hash-guarded |
| [010](ADR/ADR-010-math-directxmath.md) | Math *(owner directive)* | **DirectXMath used natively** — no wrapper classes, functions, or aliases; `XMFLOAT*` stored / `XMVECTOR` computed; NeuronCore's math header deleted; `XM*Est` banned in GameLogic |
| [011](ADR/ADR-011-audio.md) | Audio *(owner directive)* | **XAudio2 graph + X3DAudio**; master + 5 submixes, pooled voices, mono 3D assets; **listener at the camera focus raised by zoom**; Doppler off; JSON sound bank |
| [012](ADR/ADR-012-configuration-and-json.md) | Configuration *(owner directive)* | **JSON config files only — no argv, no environment**; custom NeuronCore parser (exact `int64`, iterative, diagnostics) also serving universe + banks; settings persist to a LocalAppData user layer |
| [013](ADR/ADR-013-source-layout.md) | Source layout *(owner directive)* | **Flat project folders**, grouping via `.vcxproj.filters`; repo-wide unique file names; per-project include roots with unqualified includes |
| [014](ADR/ADR-014-engine-game-separation.md) | Engine/game split *(owner ruling)* | **`Neuron*` never references GameLogic** — the engine declares `Simulation` and `WorldView`, GameLogic implements them, `Outpost.exe` injects them; neutral `EntityRecord` for replication |

## Coding standard

Naming, layout, and the working rules live in **[AGENTS.md](../AGENTS.md)** at the repository
root, with `.clang-tidy` (identifiers) and `.clang-format` (whitespace) as the machine-readable
half. The convention is adopted from the sibling repository **Outpost.Warzone** so engine code
moves between the trees without a rename pass. Three things it changed in these documents:

- `ITransport` → **`Transport`** (R2 bans `I`/`C`/`Base` prefixes; `UdpTransport` and
  `QuicTransport` are the implementations).
- Namespaces are PascalCase: **`Neuron`** for the engine libraries, **`Game`** for GameLogic.
- Wire and aggregate fields carry units in camelCase: `posXCm`, `velXCmPerSec`,
  `headingTurns16`, `etaTicks` — never `posX_cm` or a type prefix.

## Deliverables

- [Architecture-Overview.md](Architecture-Overview.md) — process model, the one data flow,
  time model, frame/tick anatomy, deliberate omissions, corpus alignment.
- [Dependency-Map.md](Dependency-Map.md) — allowed edges, per-project public surface
  (header-level), the session's dependency rulings.
- [MVP-Build-Order.md](MVP-Build-Order.md) — S1–S15 vertical slices (S2b, S5b and S5c were
  added by later directives) with acceptance criteria and a **Built** line per landed slice;
  milestones M0 (heartbeat) / M1 (first commanded fleet) / MVP.
- [Risk-Register.md](Risk-Register.md) — R1–R14 with designed-in mitigations + standing spikes.
  R14 is marked realised.

## Implementation state (2026-08-18)

Slices S1, S2, S2b, S3 and S4 are in the tree and green in CI. The per-slice detail — what was
built, and what a "done" slice still owes — lives in
[MVP-Build-Order.md](MVP-Build-Order.md); it is not repeated here.

**Milestone M0** is half proven. Its automated half is green: 64 tests across four assemblies,
plus a `selfTest` mode that runs the whole handshake-and-heartbeat exchange over a real
loopback socket and returns an exit code. Its visible half — window open, swapchain
presenting, heartbeat live — has not been run by a person, because CI has no GPU and cannot
run it.

**Continuous integration:** `.github/workflows/build.yml` builds Debug|x64 (Release is
deliberately not built — see the note at the top of that file), restores NuGet per project,
checks header names against the CRT (R14), builds the four libraries, builds `Outpost.exe`
once an entry point exists, builds and runs the tests, and surfaces failing tests and
deduplicated warnings in the job summary. It is the only compiler this work has: every defect
listed in R14 and the S4 notes was found by pushing and reading the log.

## Repo observations for the owner

1. **Test project wiring — mostly done.** `NeuronCoreTests`, `NeuronServerTests` and
   `NeuronClientTests` now carry `ProjectReference`s and include paths, and all four projects
   were given `stdcpplatest` (they were generated without a `<LanguageStandard>`, defaulting to
   C++14, which failed the moment a `<span>` appeared). **`GameLogicTests` is still unwired** —
   its library is empty, so there is nothing to reference yet. Following ADR-014, each
   references **only** its library and that library's own dependencies; the engine test projects
   stay engine-only — they test the seam with a stub `Simulation`/`WorldView`, not with
   GameLogic, and `NeuronServerTests` already does exactly that.
1b. **Filters:** semantic filters per ADR-013 §5 are maintained for the files added so far; the
   generated `Source Files`/`Header Files` buckets remain wherever no file has been added yet.
1c. **Include roots stay per-project** (owner decision) — the qualified-include alternative was
   considered and declined; ADR-013 §4 records what that puts on the uniqueness rule, and R14
   records what it cost.
2. ~~Content gap: Fighter/Cruiser meshes missing.~~ **Resolved by ADR-009 §6:** the meshes in
   `GameData/Meshes` *are* the standard ship set (8 ships + `Structure` for stations);
   `HullClass` keeps the 11-value closed taxonomy with **Fighter and Cruiser as reserved,
   unused ids** so wire, icons, and palettes never renumber when content arrives.
3. Language standard is `stdcpplatest` across configs for the five main projects, and was added
   to the four `Tests/*` projects (see item 1); nothing to do.
4. Mesh conventions confirmed for the loader: triangulated `f v/vt/vn`, Y-up,
   **forward = −Z**, shared 5-material palette (`hull/plate/glass/accent/thruster`) identical
   across all `.mtl` files — `Frigate.mtl` omits `glass` entirely, which is an absent submesh
   rather than a different palette. Normals are per-face via duplicated vertices *for most of
   the corpus*, but not all of it: S5 measured 152 of `Structure.obj`'s 1,784 faces carrying a
   different normal per corner around a curved section. The loader keys a vertex on
   (position, normal) so both shade correctly (ADR-006 §5).
