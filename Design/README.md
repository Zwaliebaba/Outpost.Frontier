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
through injected interfaces instead; ADR-015 spends ADR-005 §2's "no inter-ship avoidance in
MVP" — ships collide now, and the tick gained a fifth system (`Separate`); **ADR-016**
rescales the universe (F13's ~300 systems → 2,500 across ~50 regions), expires ADR-009 §9's
no-traversal fence, appends a twelfth hull (`Gate`), and retires ADR-001 §3's
one-play-area-per-session clause — a session now hosts many grids, the client viewing one;
**ADR-017** expires the docking half of ADR-009 §9's fence, extends ADR-016 §3's anchor
record (undock point and facing) and §6's summary family (`StationRoster` lands first),
amends §7's presence rule (docked ships count as presence), and moves §4's transfer bus
earlier — it arrives with the station phase, and U3a inherits it; **ADR-018** records the
owner's scaling-baseline decisions over the five-lens review — the target is an **MMO
shard on a persistent service** — amending ADR-004/005/006/007/008/012/013/014/016/017
(each carries the note), adding U3c (the second-commander gate) and three named design
deliverables (the topology ADR, the interest/delta ADR, the UI-architecture ADR), and
extending the Risk Register with R19–R21; **ADR-021** completes ADR-015 §2's avoidance with the
limb the *other* ship applies — an idle hull steps out of a mover's lane and flies back to its
berth — and reads ADR-005's `GuidanceMode::Hold` as "stay where you were put" rather than
"stay where you are"; **ADR-022** delivers the interest/delta deliverable and amends ADR-003 §1
(a second reliable channel, `Bulk`), ADR-004 §6 (the growth path stops being a label),
ADR-005 §5 (`lastOrderSeqProcessed` leaves the world hash — the one replay re-baseline in the
set), ADR-014 (the relevance hook lands as rank-in-the-game, truncate-in-the-engine) and
ADR-016 §6 (per-grid becomes per-viewer); **ADR-023** delivers remote play and amends
ADR-003 §1/§3 (descriptors, and validation that is off only against loopback), ADR-008 §8 (its
"no architectural work remains" now names what the first remote deployment owes) and
ADR-012 §3.

**A numbering note.** The interest/delta and remote-play deliverables were written as ADR-021
and ADR-022 and **renumbered to 022 and 023** when they met ship make-way, which had taken 021
on main. Nothing about them changed; if an outside link points at ADR-021 expecting interest
and delta, this is why.

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
| [015](ADR/ADR-015-ship-collision.md) | Ship collision *(post-MVP)* | **Per-class contact radii; brake + tangent deflection in Steering; positional `Separate` after Integrate** — area-weighted, stations are terrain; no pathfinding, no momentum, no wire change |
| [016](ADR/ADR-016-procedural-universe-and-warp.md) | Universe & warp *(owner design session)* | **2,500 baked systems (~50 regions), authored anchors as the only warp destinations, timed warp over a deterministic transfer bus, per-grid snapshots + fleet summaries, client-fed routes** — commander stays disembodied; view is presence-gated |
| [017](ADR/ADR-017-station-docking.md) | Station docking *(owner design session)* | **Docked ships are an off-grid roster; a fleet docks together, instantly, inside 5 km; undock spawns at an authored point with 15 s command-broken protection and self-parks on a scanned berth ring; the hangar screen recombines emergent fleets and wings** — repair is the roster holding no damage |
| [018](ADR/ADR-018-scaling-baseline.md) | Scaling baseline *(owner decisions over the review)* | **MMO shard (hundreds of commanders), persistent service; durable `PlayerId` + u32 ship ids (staged by the snapshot arithmetic); footprint-derived dock radius; worlds forget — durable state lives at the universe layer; `(applyTick, transferId)` bus order; behaviour joins the fail-closed gate; Release in CI, dxc/SM 6.x; screens are engine surfaces, data-fed** — 19 decisions, a 26-action register the build orders carry |
| [019](ADR/ADR-019-shard-topology.md) | Shard topology *(deliverable A1 — blocks U2)* | **Three roles in one process today** (SimHost / SessionHost / Directory); the **anchor is the placement unit**, region-affine, never live-migrated; the tick is **shard-global**; transfers are **filed at departure** and ordered `(applyTick, hostId, counter)` with no coordination; **one client connection** through the session front door, so the client wire never learns the topology exists |
| [020](ADR/ADR-020-ui-architecture.md) | UI architecture *(deliverable A19 — blocks U5 and T3)* | **A surface is a value on a small stack** (re-pushing pops back, so `◀ TACTICAL` and `◀ BACK` are one mechanism); a full-screen surface **skips** the world passes rather than adding one; **input is claimed once by one router** over three independent channels, with the printable-key rule that makes "W" type *or* pan; the screen-data contract is **three shapes, not three methods**, and a **badge class index** crosses the seam, never a colour |
| [021](ADR/ADR-021-ship-make-way.md) | Ship make-way *(owner-reported defect)* | **A ship with nowhere to be steps out of a mover's lane and flies home** — a displaced target sought through the ordinary envelope, recomputed each tick and never stored; the corridor is tested against the *berth* so the sidestep cannot undo itself; side chosen by turn time, not distance; the occupied destination stays exempt |
| [022](ADR/ADR-022-interest-and-delta.md) | Interest & delta *(deliverable A14 — gates shared grids)* | **Culling and delta live in the session role alone**; `SnapshotAck` against a ring of **views as sent**, not world states; keyframes on a new reliable `Bulk` channel because a view switch is a mid-session join; the game **ranks** relevance and the engine **truncates** it; **owned and selected are never culled** and `culledCount` says what is missing; truncate, never refuse; ownership costs **no byte** — two spare `statusBits` carry the relationship |
| [023](ADR/ADR-023-remote-play.md) | Remote play *(deliverable A22 — blocks first remote deployment)* | **A two-pin key compiled into the build, never a config value**; `Listen`/`Connect` take descriptors and the validation policy is *derived from the address*, so "no validation off-loopback" is unrepresentable rather than discouraged; the token step lives in the front door and the game never sees it; four abuse rules, each closing something in the tree today |

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
- [Universe-Build-Order.md](Universe-Build-Order.md) — the post-MVP universe phase: U1–U6
  slices delivering ADR-016 (bake, anchors, warp, gates, strategic map, system view), plus
  the named content deliverables (system-view print, `Gate.obj`); milestones W0 (first warp)
  / W1 (first crossing) / W2 (the universe on screen). **U1, U2 and U3a are built, with
  U3b's sim half and U5's pure half beside them**; U4 is untouched, U3c is blocked on
  machinery rather than on a screen, and the rest of what is left is screen work.
- [Station-Build-Order.md](Station-Build-Order.md) — the docking phase: T1–T3 slices
  delivering ADR-017 (roster + transfer bus in the sim, the wire and tactical surfaces,
  the hangar screen), interleaved **after U2, before U3a**; milestones H0 (the headless
  loop) / H1 (the hangar loop); deliverables P1 (station-screen print, blocks T3) and P2
  (dock/undock audio). **T1 is built in all three halves and T2's identity cluster is on the
  wire**; T2's client half is not, and T3 is still gated on P1.
- [Risk-Register.md](Risk-Register.md) — R1–R23 with designed-in mitigations + standing spikes.
  R1, R6 and R14 are marked realised, with what actually happened; **R22 is realised and
  closed**, and R23 — a gating test that flakes — is the one question that did not close
  with it.
- [Scaling-Readiness-Review.md](Scaling-Readiness-Review.md) — five-lens review of the MVP
  and this corpus for scaling readiness (2026-08-19, **advisory**): consolidated findings
  (`UX-/NET-/CPP-/UI-/SIM-`), a decision list sequenced against the build orders, and the
  fourteen questions the owner answered the same day — **the answers are normative as
  [ADR-018](ADR/ADR-018-scaling-baseline.md)**; the review stays the evidence record.
  **Nineteen of the register's twenty-six actions are delivered, and every design deliverable
  in it is written** — A1–A10, A12, A14, A17, A19, A21, A22, A23, A24 and A26 — with A11
  **partly** done: the wire carries u32 ship ids, while the sim's `ShipId` stays u16 until the
  delta cluster lifts the full-fit constraint (D6's own staging). **Six are open** — A13,
  A15, A16, A18, A20 and A25 — and each lands with U3b, T2, U5 or U3c rather than waiting on
  a decision. A13 is the one several of the others stand behind: the per-client
  `SnapshotSender` does not exist yet, and U3b's client half, T2's roster privacy and U3c all
  need it.

## Implementation state (2026-08-19)

**Every MVP slice is in the tree: S1 through S14** (with the inserted S2b, S5b, S5c and S5d),
green in CI, **and the post-MVP audio slice S15 with them**. The per-slice detail — what was
built, and what a "done" slice still owes — lives in [MVP-Build-Order.md](MVP-Build-Order.md);
it is not repeated here.

**The MVP is met — the play test was signed off 2026-08-19.** The lap the Architecture Overview
calls "the one data flow" runs end to end — right-drag to plane point, pre-check, PENDING
ghost, the authority validating with **the same function**, promotion from the snapshot,
refusal bouncing with the game's own reason code — and S14 closed the milestone's machinery:
the Tier-1 diagnostics strip (F1, `client.diagnostics.strip`), the aggregated `selfTest` that
CI now runs headless in the shipping binary on every push (schema self-check, wire
round-trips, a replay-determinism run, then the whole handshake + order + snapshot loop over
QUIC loopback), 4× MSAA offscreen + resolve, cosmetic banking/hover, and the STALE marker.
The merged tree — S14 plus ADR-015's collision and ADR-021's make-way, and now S15's audio —
runs **593 tests green** across the four suites on MSVC, in Debug and Release alike.

**The half that needed a person and a GPU is done (2026-08-19):** the MVP definition
demonstrated in a live session, together with the visual items outstanding since the last
owner run — the strip's numbers against `debug-hud.png`, MSAA, banking/hover and the STALE
marker on screen, the visual checkpoint against the prints, ships visibly routing around
traffic (ADR-015's own eyes-on item), and the induced stall reading as
extrapolate-then-freeze. That last one finally has the debug key S7 had owed since it landed:
**F10 cuts the snapshot feed** while leaving the link up, so the staleness path can be seen at
all on a loopback session that never stalls on its own.

**What the play test cost, and why that is the point.** It opened by finding a defect no
automated check could: a deadlock in `QuicTransport::Poll` — `m_lock` held across a
connection-scoped `GetParam`, against the msquic worker every callback locks on — that froze
the client a second after connecting and left a live window showing nothing. 477 tests and a
green headless `selfTest` ran through it without noticing, because the suites that drive the
same call pass and the race only bites at frame cadence. The fix reads the stats outside the
lock, and exposed a second bug beside it: both send paths read `send->buffer.Length` after
handing the buffer to msquic, which may free it inline. Both are in the tree; MVP-Build-Order
carries the detail.

**Audio landed the same day (S15, ADR-011).** The XAudio2 graph — mastering voice, five
submixes, pooled source voices — sits behind one pimpl the way msquic does, with four
device-free halves in front of it carrying every decision: the JSON sound bank, a RIFF chunk
reader, the listener model, and the voice-allocation policy. That split is what makes 21 of the
new tests possible at all, since CI has no sound card. The listener is ADR-011 §4's: the ear at
the camera's focus, raised by the zoom, so panning moves the audio frame and zooming out lets
the battle recede. Two cues ship — a 3D engine bed per ship and the refusal alert on both
bounce paths — and the strip's WORLD row shows the pool doing its job: **41 ships ask and 32
sound**. Two ADR-011 §8 threading details are deliberately not built and are named in the slice
notes rather than left missing. **Nobody has heard it yet**; the WAVs are synthesised
placeholders and the manual pass is S15's real acceptance.

**The universe phase is designed and half built (ADR-016, 2026-08-19).** The owner design
session settled procedural generation (2,500 systems baked to authored content), warp
(timed, anchor-to-anchor, gate traversal between systems), the multi-grid session, the view
model, and the UI surfaces — with [Universe-Build-Order.md](Universe-Build-Order.md) as the
delivery plan.

**Built so far: U1, U2, U3a, U3b's sim half and U5's pure half.** The bake produces 2,500
systems and 18,618 anchors into committed content; `WorldRegistry` is the many-grids runtime
that owns ship ids, the ship→location index and the transfer bus; `OrderKind::Warp` stopped
being reserved and runs spool → crossing → arrival; `FleetSummary` answers where a
commander's ships are without subscribing to their grids; and `UniverseRoute` answers the
strategic map's two non-drawing questions (`SolveRoute`, `FindSystems`) over the bake.

**What is not built is, with one exception, screen work.** U3b's client half, U5's map itself
and U6 need a GPU and a person, which is the same wall S5 and R1 have been standing at since
S8. The exception is **U3c, the second-commander gate** — it is blocked on machinery rather
than on a screen: it needs T2's per-client `SnapshotSender` and U3b's view subscription, and
neither exists yet. U4 (gates and the twelfth hull) is untouched. The system-view print
remains the one missing design artifact.

**The station phase is designed and its simulation is built (ADR-017, 2026-08-19).** A second
owner design session settled docking: docked ships as an off-grid roster, the dock order
(together, instant, inside the radius, client-fed approach), undock with 15-second
command-broken protection and deterministic self-parking on a berth ring, and the hangar
screen where emergent fleets and wings are recombined.
[Station-Build-Order.md](Station-Build-Order.md) is the delivery plan, interleaved after U2
and before U3a — it introduces the transfer bus warp inherited.

**Built so far: all three halves of T1, and T2's identity cluster on the wire.** Docking, the
transfer bus, undocking with its fifteen seconds, the parking ring and the event record are
in the sim; `PlayerId` and the reserved resume token are on `Hello`/`Welcome`, and the schema
text grew the verdict-affecting constants and the check-order sequence (ADR-018 D9/A21).
**T2's client half is not built** — the DOCK context action, the approach chain, the fades,
the shimmer and the DOCKED roster blocks are all screen work — and neither is its per-client
`SnapshotSender`, which is the piece U3c waits on. T3, the hangar screen, is still gated on
the station-screen print (P1), its one missing design artifact.

**The first post-MVP feature is in the tree: ship collision (ADR-015, 2026-08-18).** Ships no
longer fly through each other — per-class contact radii in the class table, braking and
tangential deflection inside Steering, and a fifth tick system (`Separate`) resolving residual
overlap positionally, with stations as immovable terrain. Eight `ShipContactTests` scenarios
plus a converging-crowd replay test joined `GameLogicTests`; nothing on the wire changed. Two
things worth knowing: a target with a hull parked on it now ends with the mover parked
adjacent and the leg expiring by its deadline (the obstructed-footprint item stays open, only
its failure mode improved), and the authored starting fleet carried a real 6 m overlap between
the Carrier and Battleship wings' line ends that `Separate` now heals on tick 1 — re-parking
that layout is the owner's call. Originally verified on a Linux clang cross-build of GameLogic;
since the merge with S14 the full MSVC build and all four suites (collision and S14 together)
have run green locally, with CI's run standing behind it as usual.

**And its other half followed: ship make-way (ADR-021, 2026-08-19).** ADR-015 gave the mover
two ways to cope with traffic and gave traffic no way to cope with the mover, and both of the
mover's fail closed — the reported symptom was a ship stopping dead behind a parked hull. Now a
ship that is not under way steps out of the lane and flies back to its berth afterwards. It is
a displaced *target* rather than a shove, so a sidestep obeys the same envelope as any ordered
move; it is recomputed every tick and never stored, so nothing new reaches `WorldHash` or the
wire; and the corridor is measured against the ship's berth rather than its hull, which is what
stops the sidestep from cancelling itself the moment it starts. Four scenarios in
`ShipContactTests` cover it, including a six-hull row that clears for a Battleship and settles
bit-identically across two runs. Two deliberate non-changes: a hull berthed on the mover's own
destination still keeps its berth (ADR-015 §5's outcome, now pinned by its own test), and
`Steering` still knows nothing about groups.

**And the tactical HUD caught up with its print (2026-08-19).** The command row stopped being
a fixed list of verbs and became **the game's own lists, drawn by the engine**: `WorldView`
gained `OrderKinds` and `OrderOptions`, the game answers both with numbers to send and names
to show, and the client indexes its per-kind tables by *slot* rather than by the kind value —
because using an opaque number as an array index is assuming the game counts densely from
zero. That is ADR-014 §2b/§2c stated as a seam rather than as a paragraph, and it is what let
`STANCE` arrive as three words (`StanceId{Balanced, Aggressive, Evasive}`) with no engine
edit and no wire field: the kind stays reserved, `ValidateOrder` still refuses it, and the
context bar states the standing posture the same way it has always stated `FORMATION LINE`
before a Move is sent.

Six other things landed with it, each of them a print detail that had been approximated: the
row keeps the print's order (**ATTACK second**, parameter chips deferred past the immediate
verbs, so muscle memory forms on the right shape); only picker buttons carry the `▾` caret;
the `▥ MENU` chip and its stub list exist because a HUD with no menu affordance is a dead end
on a machine with no Escape key; the context bar counts the optimistic window (`⏳ N ORDERS
PENDING`); world-space gauge bars band on **the same two thresholds** the roster strips use,
so a wing's strip and its ships cannot disagree about where "worn" begins; and the selection
ring became the own-fleet phosphor rather than the allied cyan it had shipped as — allied cyan
is reserved, and a player reading colour fast would have parsed their own selection as someone
else's ship.

**One of those changed the renderer's shape**, which is the part worth knowing: the ghost's
lane now draws *under* the hulls. ADR-006's fixed pass list gained a node — a second `UiPass`
instance recording into the world target before `Opaque`, against a multisampled `UiWorld`
pipeline — so a ship standing on a lane covers it with its own silhouette instead of with a
radius somebody had to guess. The pass list is **Clear · UiWorld · Opaque · Nebula ·
OverlayWorld · Ui**, and this is the second insertion into the reserved list after `Nebula`,
at the same price: one pipeline, one instance, one line in `RecordWorld`.

**Two sim changes rode along.** `World.cpp` split, with the order pipeline moving to
**`WorldOrders.cpp`** — one header, two translation units, which ADR-013 §3's uniqueness rule
permits and its registry now records. And a finished order **lingers before it is retired**
(`ORDER_DONE_LINGER_TICKS`, 30 ticks): a client ghost retires on *seeing* `Done` in a
snapshot, so an order table that dropped the row the tick it finished would have raced the
thing that reads it. Three tests pin it, including that retirement replays exactly — the
linger is simulation state, so it is in the hash.

**Milestone M0 is complete (2026-08-18).** Its automated half was green at the time: 122 tests
across four assemblies with zero unique warnings, plus a `selfTest` mode that runs the whole
handshake-and-heartbeat exchange over a real loopback socket and returns an exit code. The
suite now stands at **593** — 300 client, 208 GameLogic, 75 core, 10 server. GameLogic is
where the growth is, and that is the universe and station phases arriving: it has gone from 136
to 208 without a single one of those tests needing a device. Its
visible half — window open, swapchain presenting, heartbeat live — together with the four
other criteria that need a GPU and a person (five minutes clean under the debug layer,
PresentMon showing the flip model, a clean exit, and the 60-second tick cadence on an idle
machine) was run by the owner and signed off.

**S5 is a separate matter and is not covered by that sign-off.** M0's manual run exercised the
S1–S4 frame, which cleared and presented; S5 added the opaque pass, pipeline state, upload
ring, depth buffer and atlas upload afterwards. S5's own GPU checks — the visual checkpoint
against `tactical-hud.png`, frame time at 41 instances, and a debug-layer-clean run of the new
passes — are still open and listed under S5 in the build order.

**Continuous integration:** `.github/workflows/build.yml` builds **Debug|x64 and Release|x64**
(the Release leg arrived with ADR-018 D11; both legs gate, the Debug leg having been
non-blocking for a day while R22 was open — the note at the top of that file says why),
restores NuGet per project, runs **seven source guards and a gating clang-tidy sweep over
GameLogic** before compiling anything, builds the four libraries, builds `Outpost.exe` once an entry point exists, builds and runs the
tests, runs the self test, and surfaces failing tests, deduplicated warnings, the two
configurations' replay hashes and **R10's tick-soak table** in the job summary. The soak's
first run put the authoritative Release number on the record: **a 1,024-ship grid ticks in
7.7 ms, 15 % of the budget**, against 78.3 ms in Debug — the 10× gap D11 exists to stop
anyone reading a phase acceptance number off the wrong leg. It is the only
compiler this work has: every defect listed in R14 and the S4 notes was found by pushing and
reading the log.

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
