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
| [0003](0003-simulation-oracle.md) | Replay and scenario oracle; G2 envelope method | accepted | P2.0 |
| [0004](0004-render-device-and-trace.md) | Render device API, trace format version 1, shader toolchain | accepted | P4.0 |
