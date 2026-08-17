# Design/ — Outpost: Frontier

Output of the architecture & design session of 2026-08-17. The eight open questions from the
session brief are settled, in order, one ADR each; the four session deliverables sit beside
them. `ScreenPrints/` is the pre-existing reference corpus these documents align with.

## Decisions at a glance

| ADR | Question | Decision (one line) |
|---|---|---|
| [001](ADR/ADR-001-spatial-model.md) | Spatial model | **2D authoritative plane, 3D presentation**; cosmetic-only vertical offsets; 40 km float32 grid, grid-graph universe later |
| [002](ADR/ADR-002-server-tick-and-time.md) | Tick model | **Fixed 20 Hz** (50 ms, `u32` tick); snapshot per tick; client interpolates at −100 ms, extrapolates ≤ 250 ms → STALE |
| [003](ADR/ADR-003-transport.md) | Transport | **QUIC-shaped `ITransport` now, UDP loopback impl first**, msquic spike slice pre-MVP; 1,152 B datagram cap everywhere |
| [004](ADR/ADR-004-wire-protocol.md) | Wire protocol | **Hand-rolled little-endian, full snapshots every tick** (delta field reserved), acked order stream with shared reason codes, fail-closed schema hash |
| [005](ADR/ADR-005-gamelogic-entities-orders-determinism.md) | Entity/state | **Fixed-schema SoA tables, no ECS**; group orders w/ 4-leg queues; pure shared formation-solve + validation; **same-binary replay determinism only** |
| [006](ADR/ADR-006-renderer.md) | Renderer | **Fixed forward pass list (no frame graph), ortho at 30°** (the 2:1 ring spec), instanced flat-shading, DWrite glyph atlas, plane-picking |
| [007](ADR/ADR-007-threading-model.md) | Threading | **Two owning threads (Main, Sim)**; single-writer worlds; transport-only crossings; lane registry + Extract seam from day one |
| [008](ADR/ADR-008-inprocess-hosting.md) | Hosting | **Composition-root exe; `ServerHost` service object**; `--headless` proves the split continuously; normative shutdown order |
| [009](ADR/ADR-009-universe-model.md) | Universe *(owner directive, session follow-up)* | **`int64 × int64` metre universe plane**; systems with planets and 1–2 stations, gates as graph edges; grids anchored at exact universe positions with local float32 sim; authored `GameData/Universe/`, hash-guarded |

## Deliverables

- [Architecture-Overview.md](Architecture-Overview.md) — process model, the one data flow,
  time model, frame/tick anatomy, deliberate omissions, corpus alignment.
- [Dependency-Map.md](Dependency-Map.md) — allowed edges, per-project public surface
  (header-level), the session's dependency rulings.
- [MVP-Build-Order.md](MVP-Build-Order.md) — S1–S14 vertical slices with acceptance criteria;
  milestones M0 (heartbeat) / M1 (first commanded fleet) / MVP.
- [Risk-Register.md](Risk-Register.md) — R1–R10 with designed-in mitigations + standing spikes.

## Repo observations for the owner (no project files were modified this session)

1. **Test projects aren't wired yet:** the four `Tests/*` vcxprojs (added on main) contain no
   `ProjectReference` to the libraries they test and no include paths — they'll need both
   before the S2/S6 suites can exist.
2. ~~Content gap: Fighter/Cruiser meshes missing.~~ **Resolved by ADR-009 §6:** the meshes in
   `GameData/Meshes` *are* the standard ship set (8 ships + `Structure` for stations);
   `HullClass` keeps the 11-value closed taxonomy with **Fighter and Cruiser as reserved,
   unused ids** so wire, icons, and palettes never renumber when content arrives.
3. **Package hygiene (harmless, trimmable):** every project references the msquic *and*
   C++/WinRT NuGet packages; by the dependency map only NeuronCore needs msquic, and no MVP
   code needs C++/WinRT.
4. Language standard is now consistently `stdcpplatest` across configs after the testing
   commit — matches the fixed constraints; nothing to do.
5. Mesh conventions confirmed for the loader: triangulated `f v/vt/vn`, per-face normals via
   duplicated vertices, Y-up, **forward = −Z**, shared 5-material palette
   (`hull/plate/glass/accent/thruster`) identical across all `.mtl` files.
