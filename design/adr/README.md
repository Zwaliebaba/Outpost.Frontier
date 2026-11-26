# Decision records

Design decisions that need more room than a row in
[decisions.md](../decisions.md). Each record has a number, a status
(`proposed`, `accepted`, `superseded by NNNN`), the facts it rests on with
file references into `CppSource/`, the decision, its consequences and the
options rejected. A record is `accepted` when the owner approves it; the
decision register then carries a one-line entry that points here.

| Record | Title | Status | Work package |
| --- | --- | --- | --- |
| [0001](0001-object-model.md) | Object model: arena, handles and kinds | accepted | P2.0 |
| [0002](0002-single-player-world.md) | Single player keeps one shared world | accepted | P2.0 |
| [0003](0003-simulation-oracle.md) | Replay and scenario oracle; G2 envelope method | accepted; revised 2026-11-19 (seed ensemble; outcome bounds approved) | P2.0 |
| [0004](0004-render-device-and-trace.md) | Render device API, trace format version 1, shader toolchain | accepted | P4.0 |
| [0005](0005-programmable-passes.md) | Programmable passes: render targets, programs, volume textures, scene depth | accepted | P4.9 |
| [0006](0006-controller-bus.md) | Controller bus and re-entrant dispatch | accepted | P3.0 |
| [0007](0007-server-world.md) | The server world over the shared arena | accepted | P3.3 |
| [0008](0008-content.md) | Content in game-logic | accepted | P3.4–P3.7 |
| [0009](0009-game-client.md) | The game client: context, frame, session and screen oracle | proposed | P5.1–P5.3 |
