# Contributing

Run Cargo commands at the workspace root. Follow [AGENTS.md](AGENTS.md) for
the naming conventions, dependency direction, platform and unsafe rules, and
the local checks.

Place game-specific behavior in GameApp, GameLogic or GameSim and reusable
engine features in the Neuron crate that owns the subsystem. The
[port map](docs/port-map.md) covers the C++ projects and shared source groups;
the [library ledger](design/library-ledger.md) tracks each project's status.

Work follows the work packages of the
[migration plan](design/migration-plan.md). A package is done when it meets
the definition of done in [design/verification.md](design/verification.md):
tests at the assigned fidelity tier, divergences recorded, ledger and port
map updated.

When porting a subsystem, capture the relevant behavior in focused tests.
Keep data format identifiers and resource IDs consistent across producers
and consumers. Update the port map when a subsystem's ownership changes.
