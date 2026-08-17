# ADR-014 — Engine/Game Separation: Neuron Knows No Game

**Status:** Accepted · 2026-08-17 (owner ruling)
**Depends on:** the fixed project structure, ADR-004 (wire), ADR-008 (composition root)
**Supersedes:** Dependency Map ruling #2 ("NeuronClient links GameLogic from day one") and
every consequence drawn from it.

## Context

The brief's dependency rules say NeuronClient and NeuronServer **may** depend on GameLogic. The
design session exercised that permission: the client linked GameLogic so it could call
`ValidateOrder` (BounceParity — a local refusal must be indistinguishable from a server one)
and `SolveFormation` (the order puck's footprint must be the real solve, not a decorative
ellipse), and the server linked it because it hosts the simulation.

The owner's ruling overturns it: **`Neuron*` is engine, `GameLogic` is the game, and only the
executable knows both.**

The decisive evidence is outside this repository. The sibling **Outpost.Warzone** ships its own
`NeuronCore`, `NeuronClient` and `NeuronServer` hosting an entirely different game. Neuron is a
**two-game engine**. The moment `NeuronClient` links `GameLogic`, it stops being an engine and
becomes this game's client, and the sibling cannot take an improvement without inheriting
fleets and formations. "May depend" optimised for MVP convenience against the property that
makes those libraries worth having.

## Decision

1. **Compile-time dependencies:** `NeuronCore`, `NeuronServer` and `NeuronClient` depend on
   NeuronCore and the Windows SDK **only**. Neither engine library references `GameLogic`.
   `GameLogic` depends on NeuronCore (and DirectXMath as toolchain, ADR-010). **`Outpost.exe`
   is the only project that references GameLogic**, and it is where engine meets game.
2. **The seam is dependency inversion:** the engine declares the interface, GameLogic
   implements it, the composition root injects it. Two interfaces, both engine-owned:
   - `Neuron::Simulation` (NeuronServer) — `AdvanceTick()`,
     `ApplyOrderBytes(span<const byte>) → verdict`, `WriteSnapshot(ByteWriter&)`,
     `SchemaHash()`, `ContentHash()`. `ServerHost` drives the tick loop, sessions, fan-out and
     transport, and never learns what a ship is.
   - `Neuron::WorldView` (NeuronClient) — `ApplySnapshot(bytes, tick)`,
     `BuildScene(renderTick, RenderScene&)`, `PreCheck(const OrderIntent&) → verdict`,
     `SolvePreview(const OrderIntent&, FormationPreview&)`, `EncodeOrder(const OrderIntent&,
     ByteWriter&)`. `ClientApp` owns window, device, passes, camera, picking and HUD; the game
     supplies meaning.
3. **BounceParity survives intact.** The client still runs the *identical* validation function
   — it is reached through an interface instead of a link-time symbol. Same code, same reason
   codes, same bounce. This was the one thing worth checking before accepting the ruling, and
   it costs nothing.
4. **The replication record is engine-level and game-neutral.** NeuronCore defines
   `EntityRecord { id, typeId, pos, vel, heading, gaugeA, gaugeB }` — it names no ship, order,
   formation or hull class, so it passes NeuronCore's zero-game-semantics test (ADR-004
   ruling 4), and it lets `SnapshotBuffer` interpolate and the renderer draw without game
   knowledge. HUD labels come from **data** (the class/display table) rather than GameLogic
   enums.
5. **Game wire schemas stay in GameLogic** (ADR-004 ruling 1) — and this ruling strengthens
   that: the engine moves framed, opaque payloads; GameLogic alone decides what the bytes mean.
6. **Outpost.exe grows by exactly one responsibility:** construct the GameLogic implementations
   and hand them to `ServerHost` and `ClientApp`. It remains free of game logic — wiring is not
   logic, and ADR-008's "composition root and nothing else" still holds.
7. **Runtime behaviour is unchanged.** Outpost links everything, so the same code executes on
   the same threads. What changes is the *direction of the compile-time dependency*, which is
   the part that decides whether the engine is reusable.
8. **Prediction (post-MVP) does not reopen this.** The client process will run a GameLogic
   world; it arrives through the same injected interface, from the same composition root.

## Alternatives rejected

- **Client links GameLogic** (the previous decision) — simplest, allowed by the brief's "may",
  and wrong once the engine serves two games. Rejected by owner ruling and on merit.
- **Game-side client code in Outpost.exe** — keeps the engine clean by making the composition
  root fat, which contradicts ADR-008 and hides presentation logic in an unlinkable, untestable
  place. Rejected.
- **A fifth "GameClient" project** — the honest home for game-specific presentation, but the
  project structure is fixed at four libraries plus the executable. Not taken; revisit only if
  the `WorldView` seam starts leaking game shapes.
- **Opaque bytes all the way (GameLogic interpolates)** — keeps NeuronCore free of even a
  neutral entity record, but moves presentation timing into the game library and leaves the
  renderer unable to interpolate anything. Rejected in favour of the neutral record.

## Consequences

- The engine libraries become genuinely reusable, which is the point: improvements flow between
  Frontier and Warzone instead of forking.
- Two interfaces must be designed *before* the slices that cross them — `Simulation` before S7
  (snapshots) and `WorldView` before S9 (orders). This is new work the previous design avoided;
  it is the price of the property, and it is small.
- One virtual call per tick and per frame stage. Irrelevant at any scale this game reaches.
- The engine cannot inspect game state, so anything the engine needs *about* entities must be
  in the neutral record or exposed through the seam. Interest management later needs a generic
  relevance hook on `Simulation` — designable, and better than the engine reading ship tables.
- Watch for leakage: if `RenderScene`, `OrderIntent` or `FormationPreview` start growing
  fleet-shaped fields, the seam is failing and the fifth-project question reopens rather than
  being answered by quietly re-adding the dependency.
