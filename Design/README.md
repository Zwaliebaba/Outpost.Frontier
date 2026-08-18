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

- `ITransport` → **`Transport`** (R2 bans `I`/`C`/`Base` prefixes; `QuicTransport` is the
  implementation — `UdpTransport` was the MVP scaffold until the S13 owner directive).
- Namespaces are PascalCase: **`Neuron`** for the engine libraries, **`Game`** for GameLogic.
- Wire and aggregate fields carry units in camelCase: `posXCm`, `velXCmPerSec`,
  `headingTurns16`, `etaTicks` — never `posX_cm` or a type prefix.

## Deliverables

- [Architecture-Overview.md](Architecture-Overview.md) — process model, the one data flow,
  time model, frame/tick anatomy, deliberate omissions, corpus alignment.
- [Dependency-Map.md](Dependency-Map.md) — allowed edges, per-project public surface
  (header-level), the session's dependency rulings.
- [MVP-Build-Order.md](MVP-Build-Order.md) — S1–S15 vertical slices (S2b, S5b, S5c and S5d were
  added by later directives) with acceptance criteria and a **Built** line per landed slice;
  milestones M0 (heartbeat) / M1 (first commanded fleet) / MVP.
- [Risk-Register.md](Risk-Register.md) — R1–R14 with designed-in mitigations + standing spikes.
  R6 and R14 are marked realised, with what actually happened.

## Implementation state (2026-08-18)

Slices S1, S2, S2b, S3, S4, S5, S5b, S5c, S5d, S6, S7, S8, S9 and S10 are in the tree and green
in CI, along with **S11a** — the Ui pass, its device-free draw list and layout, and the toast
stack that finally gives S9's bounce its second surface. The per-slice detail — what was built, and what a "done" slice still owes — lives in
[MVP-Build-Order.md](MVP-Build-Order.md); it is not repeated here.

**Milestone M1 — first commanded fleet — is code-complete and awaiting its play test.** The lap
the Architecture Overview calls "the one data flow" now runs end to end: a right-drag becomes a
plane point and an arrival facing, the game pre-checks it against the replicated view, the
client draws a PENDING ghost and sends the order on the reliable channel, the authority
validates it with **the same function**, the ghost promotes when the snapshot agrees, and a
refusal bounces over 150 ms carrying the game's own reason code. What is left of M1 is what a
unit test cannot see: promotion arriving within 100 ms on screen, and a deliberate
out-of-bounds order looking identical whether the local pre-check or the server refused it.

**The frame was run for the first time since S5, and it found three defects in one sitting.**
Two overlay colours byte-swapped since S8; a ring whose thickness scaled with its own radius,
so a large footprint drew as a forty-pixel band; and a puck sized to circumscribe the formation
rather than mark the point the order was given, which for an eleven-ship Line put an ellipse
across the whole viewport. All three are fixed. Every device-free test passed throughout — each
defect lives in the product of two pieces of arithmetic that are individually right, which is
the category a unit test cannot reach.

**The same run closed two criteria that had been open since S7 and S8.** Interpolated motion
reads as motion at 144 Hz over 20 Hz snapshots, and rings occlude behind a Carrier hull while
gauge bars never do — `overlay-pass.png`'s rule holding, and the ring pipeline's depth bias
promoted from a textbook guess to a measurement.

**What still needs a person and a GPU:** a second look at the overlay after the two size fixes,
the visual checkpoint against the prints at min and max zoom, and the induced 400 ms stall
reading as extrapolate-then-freeze rather than as a stutter.

**Milestone M0 is complete (2026-08-18).** Its automated half was green at the time: 122 tests
across four assemblies with zero unique warnings, plus a `selfTest` mode that runs the whole
handshake-and-heartbeat exchange over a real loopback socket and returns an exit code. The
suite now stands at **362** — 188 client, 91 GameLogic, 73 core, 10 server. Its
visible half — window open, swapchain presenting, heartbeat live — together with the four
other criteria that need a GPU and a person (five minutes clean under the debug layer,
PresentMon showing the flip model, a clean exit, and the 60-second tick cadence on an idle
machine) was run by the owner and signed off.

**S5 is a separate matter and is not covered by that sign-off.** M0's manual run exercised the
S1–S4 frame, which cleared and presented; S5 added the opaque pass, pipeline state, upload
ring, depth buffer and atlas upload afterwards. S5's own GPU checks — the visual checkpoint
against `tactical-hud.png`, frame time at 41 instances, and a debug-layer-clean run of the new
passes — are still open and listed under S5 in the build order.

**Continuous integration:** `.github/workflows/build.yml` builds Debug|x64 (Release is
deliberately not built — see the note at the top of that file), restores NuGet per project,
checks header names against the CRT (R14), builds the four libraries, builds `Outpost.exe`
once an entry point exists, builds and runs the tests, and surfaces failing tests and
deduplicated warnings in the job summary. It is the only compiler this work has: every defect
listed in R14 and the S4 notes was found by pushing and reading the log.

## Repo observations for the owner

1. ~~**Test project wiring — mostly done.**~~ **Done.** All four test projects carry
   `ProjectReference`s and include paths and are on `stdcpplatest` (they were generated without
   a `<LanguageStandard>`, defaulting to C++14, which failed the moment a `<span>` appeared);
   `GameLogicTests` was wired with S5b, when its library stopped being empty. Following
   ADR-014, each references **only** its library and that library's own dependencies, and the
   engine test projects stay engine-only — they drive the seam with a stub
   `Simulation`/`WorldView` rather than with GameLogic. **CI enforces it**: the build fails if
   any `Neuron*` project or engine test project so much as names GameLogic.
1b. **Filters:** semantic filters per ADR-013 §5 are maintained for the files added so far; the
   generated `Source Files`/`Header Files` buckets remain wherever no file has been added yet.
   *Worth knowing:* Visual Studio regenerated `Outpost.vcxproj.filters` during the shader move
   and dropped every `<Filter Include>` definition, leaving items pointing at a filter that no
   longer existed. It was rebuilt by hand. Filters are IDE metadata and the IDE will rewrite
   them, so a semantic tree is a thing to check after opening the solution, not a thing to set
   once.
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
