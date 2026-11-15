# Contributing

Run Cargo commands at the workspace root. Follow [AGENTS.md](AGENTS.md) for
the naming conventions, dependency direction, and Windows x64 checks.

Place game-specific behavior in GameApp or GameLogic and reusable engine
features in the Neuron crate that owns the subsystem. The
[port map](docs/port-map.md) covers the C++ projects and shared source groups.

When porting a subsystem, capture the relevant behavior in focused tests.
Keep data format identifiers and resource IDs consistent across producers
and consumers. Update the port map when a subsystem's ownership changes.
