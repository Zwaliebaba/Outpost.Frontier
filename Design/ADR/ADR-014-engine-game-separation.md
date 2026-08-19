# ADR-014 — Engine/Game Separation: Neuron Knows No Game

**Status:** Accepted · 2026-08-17 (owner ruling) · §2c amended by
[ADR-018](ADR-018-scaling-baseline.md) (2026-08-19): screens are engine surfaces fed
neutral data — static topology crosses once at boot as a neutral graph, live data as
summary-keyed rows, search/route-solve as game pure functions; the leak test extends to
security/sovereignty/service semantics; the fifth-project revisit gains its tripwire (D14)
· given its mechanism by [ADR-020](ADR-020-ui-architecture.md) §6 (2026-08-19): a screen's
seam budget is **three shapes, not three methods**; a **badge class index**, never a literal
colour, crosses the seam; and the fifth-project question reopens when a screen needs a game
*rule* rather than game *data* to render · the "generic relevance hook on `Simulation`" its
Consequences reserved is designed by [ADR-022](ADR-022-interest-and-delta.md) §4
(2026-08-19), and lands as this seam's own pattern rather than an exception to it: the game
**ranks** (relevance is game semantics) and the engine **truncates** (budget is link
semantics), so a hostility rule stays a GameLogic edit and a bandwidth change stays an
engine one
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
     supplies meaning. **S9 added three, S10 a fourth, S11b a fifth and S11d a sixth** — see §2c.
2a. **Who holds the vtable** (settled by S5c, because the ADR as written could not be
   implemented). §1 says GameLogic depends on **NeuronCore only**. §2 says **GameLogic
   implements** `Neuron::Simulation` and `Neuron::WorldView`. Those interfaces are declared in
   NeuronServer and NeuronClient, so a GameLogic class implementing one would need that
   project on its include path. Both statements cannot hold.

   **Resolved in favour of §1: the composition root holds the vtable.** GameLogic supplies
   pure types and pure functions; `Outpost.exe` implements the engine's interfaces by
   forwarding to them. That is wiring rather than logic, which §6 already allows.

   The deciding argument is what §1 is protecting. GameLogic's freedom from Windows, D3D12,
   sockets and file IO is why `GameLogicTests` runs with no device, no fixtures and no
   filesystem — a property S5b leaned on hard, since the whole universe parser is tested on
   text held in the test file. Putting `$(SolutionDir)NeuronClient` on GameLogic's include
   path would make every D3D12-bearing header in that project reachable from the game, and the
   discipline would survive only as long as everyone remembered it. A structural guarantee is
   worth an adapter.

   What it costs, stated rather than discovered later: the adapters live in the one project
   with no test project of its own. The mitigation is to keep them thin enough that there is
   nothing to test — they hold data and forward — and to put the *stubs* in the test projects,
   where they prove the interfaces are implementable with no game in sight.

2b. **`FormationPreview` is `OrderPreview`** (S5c). §2 named the preview type
   `FormationPreview`, and §4 of this same ADR is why it could not keep that name: `EntityRecord`
   earns its place in the engine by naming "no ship, order, formation or hull class", and a
   type called `FormationPreview` fails that test in the library the test was written for. What
   the engine needs to know is that a proposed command has marks and an extent worth drawing.
   That some games arrange them into a formation is the game's business.

   `OrderIntent`, `OrderVerdict` and `OrderPreview` live in **NeuronCore**, not beside either
   seam: `WorldView::PreCheck` and `Simulation::ApplyOrderBytes` must return the same verdict
   type or §3's BounceParity is a claim nothing can check — and NeuronClient cannot see
   NeuronServer, so the shared type has to sit below both.

2c. **Three calls the client half needed** (S9, and each one replaces a guess the engine was
   otherwise going to make):

   - `DefaultOrder() → OrderDefaults{kind, parameter}`. The puck turns a gesture into a place
     and a facing; *which command* that is belongs to a surface that does not exist until S11.
     A client filling the fields in itself would have started choosing game semantics, and the
     tempting shortcut — leave them zero — works only because `OrderKind::Move` and
     `FormationId::Line` both happen to be zero. That is a coincidence of two enumerations, and
     coincidences are what renumber.
   - `PollOrderFeedback(OrderFeedback&)`. The snapshot's order-state records, as six numbers
     per order. It is what promotes a PENDING ghost when the ack is lost and what retires one
     whose order has finished — order records exist only while an order does, so absence is
     itself the signal. Polled rather than pushed, so the ghost list changes at a point in the
     frame the client chose. `state` crosses as a number the engine compares for change and
     never names.
   - `ReasonText(u16) → const char*`. The bounce toast has to say *why*, and the reason code is
     a number the engine cannot read. Asking the side that assigned it is what makes a local
     refusal and a server refusal say the same words rather than two tables agreeing today —
     which is §3's parity claim applied to the string as well as the code. Never null: a toast
     with no text is the silent disappearance `puck-and-wheel.png` §4 forbids.

   `OrderIntent` also gained `orderSeq`, and it travels the awkward way for the same reason
   `OrderVerdict::orderSeq` does (ADR-004 §7): the client allocates it because the client is
   what matches an ack to a ghost, and `EncodeOrder` places it inside a payload the engine
   frames and never parses.

   **A fourth arrived with S10, and the reason is worth keeping.** `OrderOptions(kind) →
   [{parameter, name}]` reports which values a kind's parameter may take. S10 added two
   formations and the command wheel that would select one is S11, so without this the slice
   would have shipped two shapes no player could reach. The tempting shortcut was a client that
   cycles `parameter` from 0 upward — and that client would have learned how many formations
   this game has and that they are numbered contiguously, which is game semantics arriving by
   the back door and breaking silently against a second game with four stances. The engine's
   binding is named `CycleParameter` rather than `CycleFormation` for the same reason: the list
   is asked for, so the word for what is in it is asked for too.

   **A fifth arrived with S11b, and it is the one that was nearly got wrong.**
   `BuildRoster(selectedIds) → [{name, groupId, shipCount, selectedCount, hullGauge,
   shieldGauge}]` fills the HUD's left panel. The engine has the replicated entities and the
   byte to group them by, so it could plainly have done the aggregation itself — and that is
   exactly the shortcut worth naming, because taking it would have decided, in the engine,
   that groups are worth showing at all, that they have names, that the two gauges *average*
   rather than take a minimum, and that a group whose ships have all died vanishes rather than
   showing as empty. Every one of those is a design question about *this* game. What crosses
   instead is a row: a name and four numbers. The engine draws it, highlights it when
   `selectedCount` is non-zero, and never learns that the word is "wing" — a second game on
   these libraries reads squads or convoys off the same panel and the pass does not change.

   The same rule is why the wire byte is called `groupId` rather than `wingId` (§4).

   **A sixth arrived with S11d, and it is the one that would have slipped through.**
   `OrderKinds() → [{kind, name, parameterName, available}]` is what the command row's buttons
   are made of. The tempting version is five string literals in `ClientApp` — `MOVE`, `ATTACK`,
   `FORMATION`, `STANCE`, `ABILITIES` — and it is wrong in exactly the way this ADR exists to
   prevent: those are one game's verbs, compiled into a library that is meant to serve a second
   game with different ones. **No CI rule would have caught it.** The build's engine-references-
   game check greps for includes and project references; a string literal is neither. That is
   worth naming, because it is the first time the seam had to be held by judgement rather than
   by the guard.

   The three reserved kinds cross too, marked `available = false`, and the engine greys them.
   That is `HullClass`'s Fighter and Cruiser again (ADR-009 §6): nameable, numbered, never
   acted on. The row draws them rather than hiding them because `puck-and-wheel.png` §3 keeps
   the wheel's sectors in fixed positions "so the ring stays learnable as a shape rather than a
   lookup" — and a row whose buttons moved as content arrived is the same mistake in a line.

3. **BounceParity survives intact.** The client still runs the *identical* validation function
   — it is reached through an interface instead of a link-time symbol. Same code, same reason
   codes, same bounce. This was the one thing worth checking before accepting the ruling, and
   it costs nothing.
4. **The replication record is engine-level and game-neutral.** NeuronCore defines
   `EntityRecord { id, typeId, groupId, pos, vel, heading, gaugeA, gaugeB }` — it names no
   ship, order, formation or hull class, so it passes NeuronCore's zero-game-semantics test
   (ADR-004 ruling 4), and it lets `SnapshotBuffer` interpolate and the renderer draw without
   game knowledge. HUD labels come from **data** (the class/display table) rather than
   GameLogic enums.

   The third field was called `flags` and carried nothing; S11b needed a ship's wing on the
   wire and renamed it `groupId`. The rename is the whole change — the byte was always there
   and always spare — but it matters in the direction this ADR cares about. `flags` is a
   promise the engine will *interpret* the bits; `groupId` is a promise it will only carry
   them. GameLogic writes the wing into it, `ReplicatedView` reads it back out as one, and
   nothing between the two is allowed to know which. Had it stayed `flags`, the first thing
   anyone would have wanted was a `FLAG_` constant in NeuronCore, which is a game rule in the
   engine arriving one bit at a time.
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

- **The build enforces it.** From S5c, CI fails if any engine or test `.vcxproj` names
  GameLogic, or if any engine source includes a GameLogic header. This ruling is a property
  nobody can hold in their head across a year of slices; the one that matters most is the one
  worth automating.
- The engine libraries become genuinely reusable, which is the point: improvements flow between
  Frontier and Warzone instead of forking.
- Two interfaces must be designed *before* the slices that cross them — `Simulation` before S7
  (snapshots) and `WorldView` before S9 (orders). This is new work the previous design avoided;
  it is the price of the property, and it is small.
- One virtual call per tick and per frame stage. Irrelevant at any scale this game reaches.
- The engine cannot inspect game state, so anything the engine needs *about* entities must be
  in the neutral record or exposed through the seam. Interest management later needs a generic
  relevance hook on `Simulation` — designable, and better than the engine reading ship tables.
- Watch for leakage: if `RenderScene`, `OrderIntent` or `OrderPreview` start growing
  fleet-shaped fields, the seam is failing and the fifth-project question reopens rather than
  being answered by quietly re-adding the dependency.
