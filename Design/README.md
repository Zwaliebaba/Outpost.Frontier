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
extending the Risk Register with R19–R21; **ADR-026** closes the corpus's oldest open item, the
obstructed footprint ADR-005 §3a named before the MVP and ADR-015 §5 left standing — a blocked
formation slides whole rather than being refused or deformed; **ADR-021** completes ADR-015 §2's avoidance with the
limb the *other* ship applies — an idle hull steps out of a mover's lane and flies back to its
berth — and reads ADR-005's `GuidanceMode::Hold` as "stay where you were put" rather than
"stay where you are"; **ADR-022** delivers the interest/delta deliverable and amends ADR-003 §1
(a second reliable channel, `Bulk`), ADR-004 §6 (the growth path stops being a label),
ADR-005 §5 (`lastOrderSeqProcessed` leaves the world hash — the one replay re-baseline in the
set), ADR-014 (the relevance hook lands as rank-in-the-game, truncate-in-the-engine) and
ADR-016 §6 (per-grid becomes per-viewer); **ADR-023** delivers remote play and amends
ADR-003 §1/§3 (descriptors, and validation that is off only against loopback), ADR-008 §8 (its
"no architectural work remains" now names what the first remote deployment owes) and
ADR-012 §3; **ADR-024** cashes in ADR-016 §3's reserved `Site` kind (sites bake with an
authored orbit ring, their bearing epoch-derived) and ADR-012 §D13 (the first hash-guarded
balance content, `Economy.json`), answers ADR-016's named mined-out-fields and wrecks
questions (a durable site ledger; bounded, non-durable wrecks), grows ADR-017 §1's roster
record with a per-ship cargo manifest, activates ADR-017 §6's CARGO and REFINERY tabs, and
ends ADR-017's "no persistence" note — the universe layer's durable state gains a journal
(ADR-024 §7a) — whose format, cadence and recovery are **ADR-025**, which spends
ADR-017's "no persistence" note outright.

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
| [024](ADR/ADR-024-mining-economy.md) | Mining economy *(economy design session — **accepted** 2026-08-20, nine owner rulings recorded across two review rounds)* | **Three ores across 2–3 `Site` anchors per system** (ADR-016 §3's reserved kind cashed in) that **re-form on a daily epoch** — bearing on an authored orbit ring, warp-in, layout, pools — banded by the existing security value with hazards staged pre/post-combat; a fleet `Mine` order with deterministic cycles and a durable site ledger (worlds forget, ledgers do not); every economy number in hash-guarded content (`Economy.json`, ADR-012 §D13), movement staying compiled; per-station Bays, manual transfer, deterministic ME refining with communal station-tier upgrades — Nova-Steel refinable only outside High-Sec; **persistence becomes due**: an engine-owned journal + snapshot at the universe layer, its ADR a named deliverable blocking implementation |
| [025](ADR/ADR-025-persistence.md) | Persistence *(deliverable D-P1 — **accepted** 2026-08-20; clears E2's gate)* | **An engine-owned append-only journal plus a periodic snapshot**, serialised on Sim and written on its own lane; the durable line is **identity and location, never intention** — a fleet reloads at rest with an empty queue; records are **outcomes, not commands**, so the journal is explicitly *not* the replay log; a separate **`DurableHash()`** proves the reload because the replay hash folds transient state; the load guards on `universeHash` **only**, so retuning balance never invalidates a shard; a **named one-second** durability window on hard kill and nothing on a clean stop; SQL staged to the service layer |
| [026](ADR/ADR-026-obstructed-footprints.md) | Obstructed footprints *(closes the corpus's oldest open item — **accepted** 2026-08-20, four owner rulings)* | **Solve, then slide**: a formation whose solved stations land in a hull, a gate or another fleet moves **whole** to the nearest free placement, shape and facing preserved — never deformed, never refused. Free is `FindBerth`'s predicate exactly (ADR-015's clearance factor + no other group's final-leg anchor, **pending orders included**), extracted so there is one copy and two callers. Two rings sized from the formation's own extent, fanned from the **approach** bearing so a blocked fleet stops short rather than overshooting; all 24 taken means fly to the asked point anyway, which **demotes ADR-015 §5's occupied-destination outcome to the fallback**. Placed when the leg becomes **active**, not at submission, so a queued leg is judged against the world it will actually fly in. The puck's preview is **advisory and allowed to differ** — the one such place in the game, safe because §4 means nothing can bounce — since `OrderStateRecord` carries no leg anchors and buying them costs ~2 ships off a cap with margin one |

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
- [Archive/MVP-Build-Order.md](Archive/MVP-Build-Order.md) — **closed and archived 2026-08-20.**
  S1–S15 vertical slices (S2b, S5b, S5c and S5d were added by later directives) with
  acceptance criteria and a **Built** line per landed slice; milestones M0 (heartbeat) / M1
  (first commanded fleet) / MVP. Every slice built, every criterion ✅, every swept open item
  closed — it is kept as the record of what each slice put in the tree, not as a plan.
- [Universe-Build-Order.md](Universe-Build-Order.md) — the post-MVP universe phase: U1–U6
  slices delivering ADR-016 (bake, anchors, warp, gates, strategic map, system view), plus
  the named content deliverables (system-view print, `Gate.obj`); milestones W0 (first warp)
  / W1 (first crossing) / W2 (the universe on screen). **U1, U2, U3a, U3b's sim and wire
  halves, U4's sim half and U5's pure half are built**; U3c's blockers have cleared and what
  is left there is the second commander's identity, and the rest of what is left is screen
  work.
- [Station-Build-Order.md](Station-Build-Order.md) — the docking phase: T1–T3 slices
  delivering ADR-017 (roster + transfer bus in the sim, the wire and tactical surfaces,
  the hangar screen), interleaved **after U2, before U3a**; milestones **H0 (the headless
  loop) — met 2026-08-20, every named criterion covered** / H1 (the hangar loop); deliverables P1 (station-screen print, **delivered** — its
  open questions answered as ADR-017 §6a) and P2 (dock/undock audio, gated on S15). **T1 is
  built in all three halves, and T2 has its identity cluster on the wire and its per-client
  sender**; T2's client half is not built, and T3 has no design gate left.
- [Economy-Build-Order.md](Economy-Build-Order.md) — the mining and refining phase: E1a–E5
  slices delivering ADR-024 (the economy content layer, sites in the bake and the epoch that
  moves them, the Mine order and the site ledger, cargo and the Bay and the wire cluster,
  refining with its tiers and projects, the two screens); milestones G0 (the headless mining
  loop) / G1 (the first alloy) / G2 (the loop on screen); deliverables **D-P1, the persistence
  ADR — it blocks E2** and is where ADR-024 §7a's journal gets its format — plus the CARGO and
  REFINERY prints (block E5), icons, the site field's visual treatment, and audio last.
  **Nothing is built yet.** It splits the E1 the ADR sketched and moves the screens out of E4,
  both recorded in its sequencing rationale.
- [Archive/](Archive/) — corpus documents that are **finished rather than wrong**. A plan moves
  here when every slice in it is built and every open item it tracked is closed; it stays
  readable and linked because it is the record of what was built and why, and it leaves
  `Design/` so that the build orders still at the top level are the live ones. Nothing here is
  the only home of an owed item — anything still outstanding is rehomed to a live document
  before its plan is archived.
- [Risk-Register.md](Risk-Register.md) — R1–R26 with designed-in mitigations + standing spikes.
  R1, R6 and R14 are marked realised, with what actually happened; **R22 is realised and
  closed**, and R23 — a gating test that flakes — is the one question that did not close
  with it. **R24 and R25 arrived with ADR-024** — the economy's two: faucet-without-sink
  inflation, and High-Sec site contention — and **R26 with ADR-025**, the one persistence
  brings: a torn journal or a refused load taking a shard's state with it.
- [Archive/Scaling-Readiness-Review.md](Archive/Scaling-Readiness-Review.md) — **archived
  2026-08-20.** Five-lens review of the MVP
  and this corpus for scaling readiness (2026-08-19, **advisory**): consolidated findings
  (`UX-/NET-/CPP-/UI-/SIM-`), a decision list sequenced against the build orders, and the
  fourteen questions the owner answered the same day — **the answers are normative as
  [ADR-018](ADR/ADR-018-scaling-baseline.md)**; the review stays the evidence record.
  **Twenty of the register's twenty-six actions are delivered, and every design deliverable
  in it is written** — A1–A10, A12–A14, A17, A19, A21, A22, A23, A24 and A26 — with A11
  **partly** done: the wire carries u32 ship ids, while the sim's `ShipId` stays u16 until
  the delta cluster lifts the full-fit constraint (D6's own staging). **A13 closed
  2026-08-20** — the per-client `SnapshotSender`, the over-cap refusal tested loudly, and
  `StationRoster` addressed per viewer through the summary family's own frame — which is
  what U3b's client half, T2's roster privacy and U3c were all standing behind. **Five
  remain: A15, A16, A18, A20 and A25**, each landing with U3b, T2, U5 or U3c rather than
  waiting on a decision. The review moved to the archive once the register had absorbed all of
  it: it decided nothing, and nothing it tracks is homed only there.

## Implementation state (2026-08-20)

**Every MVP slice is in the tree: S1 through S14** (with the inserted S2b, S5b, S5c and S5d),
green in CI, **and the post-MVP audio slice S15 with them**. The per-slice detail — what was
built, and what a "done" slice still owed — lives in
[Archive/MVP-Build-Order.md](Archive/MVP-Build-Order.md), closed and archived 2026-08-20; it is
not repeated here.

**The economy phase is designed, accepted, and its first two slices are in the tree
(ADR-024, ADR-025, 2026-08-20).** A third design session settled mining and refining: three
ores across 2–3 `Site` anchors per system, a fleet `Mine` order, per-station Bays, and
deterministic refining with communal station upgrades — with nine owner rulings recorded
across two review rounds, of which **R7 is the one that changed the shape of the content**
(a site's bearing is re-derived every epoch, so a field re-forms overnight). ADR-025 followed
as the deliverable ADR-024 §7a named: persistence, drawn at **identity and location are
durable, intention and motion are not**, which ends the "no save file exists" era ADR-017
opened. [Economy-Build-Order.md](Economy-Build-Order.md) is the delivery plan.

**Both are green in CI (run 150, 2026-08-20)**: 650 tests across the four suites in Debug and
Release, the self test passing end to end, the replay hash matching across configurations, and
the universe parsing in **183 ms in Release** — a third more content than U1 for a tenth more
time, so R17's per-region split stays reserved. The tick soak is unchanged in character
(7.000 ms for a capped grid), which answers the question 6,223 new anchors raise: a site costs
the tick nothing until somebody warps to it.

**Built so far: E1a, E1b, E2 and E3.** `Economy.json` is the **first hash-guarded balance content in
the tree** — ADR-012 §D13's hook cashed in, with `economyHash` mixed into the handshake's
existing `contentHash` so an economy mismatch is refused with no wire field added. And
`AnchorKind::Site` stopped being reserved: the committed universe was re-baked to **24,841
anchors in 18.93 MB at `universeHash ad9555dd776008a6`**, of which 6,223 are mining fields.
That re-bake is **purely additive** — 180,467 lines added, zero removed, every station,
planet and gate anchor keeping its id — because sites are appended after every other anchor
is numbered and every site roll comes from a per-system stream that never advances the main
sequence.

**E2 landed on top of that (2026-08-20).** Mining is in the tick — `OrderKind::Mine`, an ore
filter as its parameter, 6-12 clusters split out of a pool by largest remainder so not one unit
is lost to rounding, deterministic cycles that take **no RNG draw at all**, and the three
per-ship exits ADR-024 §4b names — and the **site ledger** is the phase's first durable state,
sitting beside the station rosters at the universe layer and folded into the registry hash on
the same terms. Three things about it are worth carrying forward. The tick's named step order
gained a sixth entry, `Mining`, *last*, so a cycle is judged against where a ship finished the
tick. A working Mine order is the first group in this tree that **outlives its own plan**,
which turned an implied guard into a written one — the stale-solve pass now skips a group with
no leg left, where `ApplyLeg` would otherwise have read past the plan. And the ledger obeys
D8's viewer rule in its own right: a ledger the shard owes a refill is skipped by the hash, or
whether anybody happened to walk past a field would change the session's number. One thing is
deliberately still broken and written down where it will be found — **ore does not survive a
crossing** until E3 gives a station its Bay.

**CI's verdict on E2 (run 155, 2026-08-20):** Debug|x64, Release|x64 and Spike 2 all green,
`self test: PASSED`, no clang-tidy finding and no failing test, one pre-existing Release
warning. Content is untouched — `universe ad9555dd776008a6, economy 0b07707ec843431d, mixed
1965b853a23a5115`, parsed and hashed in **213 ms** — and the replay hash moved to
**`69c58e2751c0df22`**, which it had to, because the site ledger and the cargo arrays joined
the world hash; Spike 2 confirms Debug and Release agree on the new number. The tick soak is
the figure to keep an eye on rather than to celebrate: a capped grid now costs **9.020 ms mean
/ 16.538 ms worst** against E1b's 7.000 / 8.644, so headroom falls from 7.1 capped grids per
core to 5.5. Inside the tripwire, and a trend R10 should be read against after E3.

**E3 closed the loop's last hole (2026-08-21).** Ore stopped evaporating at boundaries: a
manifest rides the transfer record and the roster row, so a hold survives a dock, an undock and
a warp alike. The **Station Bay** joins the site ledgers and the station rosters as the third
resident of "worlds forget, the universe layer does not" — per `(owner, station)`, created only
when something is stored, folded into the registry hash, and with no currency rule, because
committed property has no epoch to go stale against. `TransferToBay` and `TransferToShip` are
**manual in both directions** (ADR-024 §5c's ruling), applied on the spot beside `AssignWing`
since neither end of the move is on a grid. The wire cluster landed in one fail-closed bump: two
verbs, an ore byte and a count on the command, an 18-byte roster row, and three new summary
kinds — `SiteStatus`, `CargoStatus`, `BayStatus` — in a new `EconomyMessages.h`. **`EntityRecord`
is untouched and a test asserts the arithmetic**: one cargo byte would take the record 21 → 22
and the ship cap 43 → 41, exactly onto `Snapshot.h`'s floor, which is what ADR-024 §4d refused in
advance.

*One change fell outside the economy: `Simulation::ApplyOrderBytes` now carries a `PlayerId`
beside the client id. A command that moves a commander's property has to say whose, and a
registry that guessed would be guessing about ownership — the command half of what the outbound
seam already does.*

**And the accept found what the unit suite could not, for the second slice running.** The G0
scenario — mine, warp, dock, commit, tear the grid down — was written to prove the loop composes
and immediately proved it did not: `ApplyTransit` spawned arrivals with empty holds, so a fleet
that warped anywhere lost its cargo silently. Dock and undock each had a test; transit had none.
Fixed, with the unit test that should have caught it first added beside it.

**CI's verdict on E3 (run 161, commit `93956dc`, 2026-08-21):** Debug|x64, Release|x64 and
Spike 2 all green in eleven minutes, **717 tests on MSVC** with none failing, `self test:
PASSED` with all thirteen 🏁 G0 checks named and passing, no clang-tidy finding and the one
pre-existing Release warning. Content is untouched — `universe ad9555dd776008a6, economy
0b07707ec843431d, mixed 1965b853a23a5115`, parsed and hashed in **212 ms** — and the tick soak
is flat rather than up: a capped grid costs **8.729 ms mean / 17.573 ms worst** against E2's
9.020 / 16.538, holding the headroom R10 watches at 5.7 capped grids per core. **The replay
hash did not move** — `69c58e2751c0df22`, byte for byte E2's — and the build-order note
predicted that it would. The reason is worth carrying: the replay scenario is six ships in a
bare `World` hashed with `ComputeWorldHash`, while the Bay and the manifests are
`WorldRegistry` state. **The replay hash is a world hash, not a shard hash**, so an unmoved
number says nothing either way about the universe layer.

**And the content copy is a post-build script now, on the owner's call (2026-08-21).**
`Outpost/CopyGameData.cmd` replaces the `CopyGameData` MSBuild target below: one robocopy
script called once per configuration, holding the whole rationale, with robocopy's bitmask exit
code translated at the one place that can get it right and failures printed in MSBuild's
canonical error form. The mechanism is the smaller half of the change. The build also gained a
step that **names the files that must be beside the executable** — which is precisely what the
45-minute hang lacked, because the self test reads the repo's content rather than the build's
and so cannot notice a deployment that shipped nothing at all.

*It took two runs. Run 162 died on `robocopy exited 16`: a line that stripped `$(TargetDir)`'s
trailing backslash by comparison had been tidied into `for %%I in (...) do set "DEST=%%~fI"` on
the belief that `%~f` trims one — **it does not**, and while cmd has no backslash escape,
robocopy parses its own command line with the C runtime's rules, where `\"` is an escaped quote,
so the closing quote and four switches were swallowed into the destination path. The comparison
is back and the result is now asserted. The half that worked is worth as much as the fix: the
script raised a named MSBuild error and failed the build in seven minutes, which is precisely
what the xcopy version could not do. Run 163 is green — `27 files, 25 copied, 2 skipped`, and
`content beside the executable: 25 files, 21.5 MB`.*

*(The merge with `main`'s station-progress work first inherited a CI hang — `a6dd412`'s
xcopy rewrite of the content copy left the exe bootless on a fresh clone, and a startup
failure raised a modal dialog no headless runner could dismiss. Diagnosed by instrumentation
(per-line log flushing plus a 300 s watchdog, run 158) and fixed on this branch: the
`CopyGameData` target is restored and the fatal dialog is gated to attended launches. Run 159
is green in both configurations — **694 tests on MSVC**, the replay hash unmoved from run
155's `69c58e2751c0df22`.)*

**What the economy phase cost in corrections is worth reading before the next slice**, because
all four were found by building rather than by review: the ADR's field radius did not fit the
grid (it read the 40 km grid as a radius when the half-extent is 20,000 m); two authored
guarantees contradicted each other in High-Sec, where only a grade-capped pocket carries
Nebulite; the faded-pocket repair ate the new-player floor in six regions; and CI's
universe-coordinate guard correctly refused three new files before a line was compiled. Each
is corrected at its source, and the guard's exclusion list grew a **narrower** rule beside it
so the tick still cannot reach a site's placement.

**T2's wire half is complete and H0's loop runs (2026-08-20).** `StationCommand` had a
format, a validator and tests, and no line of `NeuronServer`, `NeuronClient` or `Outpost`
mentioned either — so **UNDOCK could not be commanded over the wire at all**. It has its path
now, through a `CommandKind` byte leading the acked stream's payload, and `selfTest` drives
the whole headless loop over real QUIC loopback: dock a fleet, watch it leave the snapshot,
read its roster off the summary feed, undock a subset on the same acked stream, and watch the
pair come back wearing the protection bit while the ships that never left do not. Two
prerequisites fell out of it, both gaps rather than additions: `Welcome` grew **`gridAnchor`**
(PROTOCOL_VERSION 3) because a client had no number with which to address the station it
could see, and `ReplicatedShip` grew **`statusBits`** because the protection bit reached the
wire and stopped there. **P1 also turns out to exist** — the station-screen print landed
2026-08-19 and this file called it missing until now. Its four open review questions were
answered the same day as [ADR-017 §6a](ADR/ADR-017-station-docking.md), so **every design
gate in the station phase is now cleared** and what is left of it is screen work.

**Three things landed on 2026-08-20, none of them a slice.** R22 closed and the Debug leg went
back to gating, leaving R23 behind it. The build stopped relying on a hand-maintained
deployment: `Outpost.vcxproj` gained a `CopyGameData` target, so content and `Outpost.json`
arrive beside the executable and a fresh clone can press F5 — the build-order note that
recorded that gap is closed. And the boot fleet was re-parked on the owner's call, closing the
one item ADR-015 left open (see the ship-collision entry below). The next move is **A13**, the
per-client `SnapshotSender`.

**The MVP is met — the play test was signed off 2026-08-19.** The lap the Architecture Overview
calls "the one data flow" runs end to end — right-drag to plane point, pre-check, PENDING
ghost, the authority validating with **the same function**, promotion from the snapshot,
refusal bouncing with the game's own reason code — and S14 closed the milestone's machinery:
the Tier-1 diagnostics strip (F1, `client.diagnostics.strip`), the aggregated `selfTest` that
CI now runs headless in the shipping binary on every push (schema self-check, wire
round-trips, a replay-determinism run, then the whole handshake + order + snapshot loop over
QUIC loopback), 4× MSAA offscreen + resolve, cosmetic banking/hover, and the STALE marker.
The merged tree — S14 plus ADR-015's collision and ADR-021's make-way, and now S15's audio —
runs **593 tests green** across the four suites on MSVC, in Debug and Release alike. *(The suite stands at **717** as of E3 — 593 before the economy phase, 650 at E1b, 694 after E2 and the merge with `main` — green on MSVC in Debug and Release alike, with no failing test, 2026-08-21.)*

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
handing the buffer to msquic, which may free it inline. Both are in the tree;
[Archive/MVP-Build-Order.md](Archive/MVP-Build-Order.md) carries the detail.

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

**Built so far: U1, U2, U3a, U3b's sim and wire halves, U4's sim half and U5's pure half.**
The bake produces 2,500 systems and 18,618 anchors into committed content; `WorldRegistry` is
the many-grids runtime that owns ship ids, the ship→location index and the transfer bus;
`OrderKind::Warp` stopped being reserved and runs spool → crossing → arrival; `FleetSummary`
answers where a commander's ships are without subscribing to their grids; and `UniverseRoute`
answers the strategic map's two non-drawing questions (`SolveRoute`, `FindSystems`) over the
bake.

**A fleet can leave its system (U4's sim half, 2026-08-20).** `HullClass::Gate = 11` is the
twelfth hull, every gate anchor authors its entity, a gate grid can reach the far side of its
own gate, and a `Warp` naming that anchor is judged on where the fleet is standing —
`NotAtGate` otherwise, which is `UnknownStation`/`NotAtStation`'s pair one verb along. The
crossing is priced **flat** (`GATE_JUMP_TICKS = 400`, stated in ticks because `20.0 / 0.05`
truncates to 399 and a flat number should not depend on how it was divided), because between
systems distance is the map's spacing rather than a journey. Two things fell out of doing it.
The occupant id block **shrank from eight to two**: 6,000 gate anchors authoring at eight ids
each would have been 74,848 authored ids against a 32,767-id window, so the content is
re-baked and the bake now refuses rather than wrapping. And the gate's art **landed with the
slice** as `Stargate.obj` rather than the `Gate.obj` the ADR named, so the Structure stand-in
was never used; its export carried a sixth material the five-material palette does not have,
authored onto `accent`, whose colour it already was.

**What is not built is, with one exception, screen work.** U3b's client half, U4's route feeder
and icons, U5's map itself and U6 need a GPU and a person, which is the same wall S5 and R1
have been standing at since S8. The exception is **U3c, the second-commander gate** — and its
blockers cleared on 2026-08-20: T2's per-client `SnapshotSender` and U3b's view subscription
are both in the tree, so what is left there is the second commander's *identity*
(`ServerHost` mints `SOLE_PLAYER_ID`; summaries and rosters answer for everyone) rather than
the machinery to serve one. The system-view print remains the one missing design artifact.

**Every open design decision in both post-MVP phases is now answered (2026-08-20).** The two
prints that carried an OPEN list have been ruled on: `station-screen.png` §3's four questions
as [ADR-017 §6a](ADR/ADR-017-station-docking.md), and `strategic-map.png` §4's as
[ADR-016 §9a](ADR/ADR-016-procedural-universe-and-warp.md). Route execution needed no new
ruling — ADR-016 §8 decided it when the phase was designed and it stands — but its accepted
cost now has somewhere to be *reported*, because D19's event record landed after the print was
drawn and the reconnect away-log is one of its consumers. Of the other three: the history
scrubber **keeps its rail** as a drawn, inert stub, since the irreversible thing is the layout
and not the feature, and build-or-cut waits on a strategic stream that does not exist; intel
ping provenance is **deferred behind a named trigger** rather than guessed at, on ADR-018 D5's
own reasoning about inventing a security model for something that cannot yet be attacked; and
the map is **landscape only**, because aspect belongs to the display envelope and not to a
surface — one portrait screen in ADR-020's stack is a navigation model whose surfaces disagree
about shape. **What blocks the universe phase now is one missing artifact, not a decision: D1,
the system-view print.**

**The station phase is designed and its simulation is built (ADR-017, 2026-08-19).** A second
owner design session settled docking: docked ships as an off-grid roster, the dock order
(together, instant, inside the radius, client-fed approach), undock with 15-second
command-broken protection and deterministic self-parking on a berth ring, and the hangar
screen where emergent fleets and wings are recombined.
[Station-Build-Order.md](Station-Build-Order.md) is the delivery plan, interleaved after U2
and before U3a — it introduces the transfer bus warp inherited.

**Built so far: all three halves of T1, and T2's wire half in full.** Docking, the
transfer bus, undocking with its fifteen seconds, the parking ring and the event record are
in the sim; `PlayerId` and the reserved resume token are on `Hello`/`Welcome`, and the schema
text grew the verdict-affecting constants and the check-order sequence (ADR-018 D9/A21).
**T2's client half is not built** — the DOCK context action, the approach chain, the fades,
the shimmer and the DOCKED roster blocks are all screen work. Its per-client `SnapshotSender`,
the piece U3c waited on, landed with A13. T3, the hangar screen, **has no design gate left**: the
four questions [P1](ScreenPrints/station-screen.png) marked open for review were answered on
2026-08-20 as [ADR-017 §6a](ADR/ADR-017-station-docking.md) — the wave-2 trigger (the point
clears, bounded by a timeout so §4's full-ring hold cannot stall it), the composer's lifetime
(persists, reconciled against the roster), wing colour (none — colour already means
relationship), and the sort inside a wing (class descending, then ship id, because names are
client-side).

**The first post-MVP feature is in the tree: ship collision (ADR-015, 2026-08-18).** Ships no
longer fly through each other — per-class contact radii in the class table, braking and
tangential deflection inside Steering, and a fifth tick system (`Separate`) resolving residual
overlap positionally, with stations as immovable terrain. Eight `ShipContactTests` scenarios
plus a converging-crowd replay test joined `GameLogicTests`; nothing on the wire changed. Two
things worth knowing: a target with a hull parked on it now ends with the mover parked
adjacent and the leg expiring by its deadline (the obstructed-footprint item stayed open, only
its failure mode improved — **closed 2026-08-20 by [ADR-026](ADR/ADR-026-obstructed-footprints.md)**,
which makes that outcome the fallback rather than the rule), and the authored starting fleet carried a real 6 m overlap between
the Carrier and Battleship wings' line ends that `Separate` now heals on tick 1 — **re-parked
2026-08-20 on the owner's call**: the ring deals its slots widest-with-narrowest rather than in
table order, which moves the tightest cross-wing pair from −5.9 m to +90.3 m and leaves ship
ids, wing ids and call signs exactly as they were. The boot log now states the number every
run, because ADR-015 found that overlap only by measuring for it. Originally verified on a
Linux clang cross-build of GameLogic; since the merge with S14 the full MSVC build and all
four suites (collision and S14 together)
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
suite now stands at **717** — it was 593 before the economy phase, and the growth is again
GameLogic's (`EconomyParseTests`, `UniverseSiteTests`' twelve, E2's `MiningTests` with 38
more, and E3's `CargoTests` with 23). GameLogic is
where the growth is, and that is the universe, station and economy phases arriving: it has gone
from 136 to 324 without a single one of those tests needing a device. Its
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
