# ADR-018 — Scaling Baseline: the Review Decisions

**Status:** Accepted · 2026-08-19 (owner decision session over
[Scaling-Readiness-Review.md](../Scaling-Readiness-Review.md))
**Depends on:** ADR-002…008, ADR-012…017 — this ADR decides *between* options those
documents left open or answers questions they never asked; it introduces no new mechanism
beyond what the review's findings proposed.
**Amends:** ADR-004 (schema-text coverage §D9; replication growth path ownership §D3/D4;
ship-id width staging §D6), ADR-005 §4a (check order joins the compatibility gate, §D9),
ADR-006 (§9 player-text charset, §10 DPI/scale model, §12 shader toolchain, and the
device-removed posture — §D12/D13/D15), ADR-007 §4 (world-level parallelism pre-approved,
§D1a), ADR-008 §8 (the packaging split acquires a remote-play gate, §D10), ADR-012 §3
(user-layer key families, §D15), ADR-013 §1a (shader toolchain, §D12), ADR-014 §2c (the
screen-data contract, §D14), ADR-016 §3 (arrival offsets, §D18) §4 (identity, transfer
order, teardown/hash domain, world isolation — §D5–D8, D17) §7 (presence edges, §D16),
ADR-017 §2 (dock radius is footprint-derived, §D7) §5 (protection arithmetic corrected,
§D7). Delivery lands through the build orders — see the action register (§A).

## Context

The MVP closed code-complete and the universe (ADR-016) and station (ADR-017) phases were
designed but unstarted — the cheapest moment this project will ever have to fix its
baseline. A five-lens review (user experience, MMO & network, C++ & DirectX, UI
architecture, game logic) examined the corpus and the tree for scaling readiness and
returned zero Critical findings, ten High, and fourteen questions — most of them decisions
the corpus had deferred without naming an owner or a date, sitting exactly where U2/T1
build next. The owner answered every question on 2026-08-19. This ADR records the answers
(**D1–D16**), adopts three of the review's uncontested defaults (**D17–D19**), and turns
them into the action register (**§A**) the build orders now carry. Review finding ids
(`UX-/NET-/CPP-/UI-/SIM-`) refer to the review document's appendices.

## Decisions

### D1 — Target scale: an MMO shard

One running server is a **persistent shard supporting hundreds of concurrent commanders**
on one universe. Consequences, in force now:

- **D1a.** The universe runtime is built **location-transparent**: worlds share no mutable
  state during `Tick`; the transfer bus and extract are the only crossings; all cross-world
  traffic is serialised records, never pointers. World-level fan-out across a sim worker
  pool is the **pre-approved first parallel consumer** under ADR-007 §4's rule (the
  deterministic-partitioning story is per-world independence; the replay gate still
  judges). A permuted-world-tick-order bit-identity test keeps this true by suite, not by
  review. *(CPP-1, SIM-6.)*
- **D1b.** A **topology ADR** (grid-to-host assignment, cross-host transfer-bus ordering
  authority, client connection handoff on view switch) is a named design deliverable that
  **blocks U2** — the registry must be shaped by it, even though the U-phase implementation
  stays single-process. *(NET-3.)* **Delivered 2026-08-19 as
  [ADR-019](ADR-019-shard-topology.md);** its §6 is U2's acceptance, and it amends this
  ADR's D6a (the ship→location index is the session role's projection; id allocation stays
  registry-owned) and ADR-016 §4 (the world's tick becomes shard-global).
- **D1c.** The tick budget gets numbers: the 50 ms tick must hold the shard's target of
  concurrent live grids at up to the per-grid cap (D4); R10's 1,024-entity soak runs in
  Release **before U2 shapes the registry**, and its result decides whether a spatial
  broadphase for Steering/`Separate` lands first.

### D2 — Persistence: a long-lived service

The shard runs for weeks/months; the reconnect print's RESUME is meant literally. No
save/load is designed yet, but from now on **shapes are kept serialization-friendly
knowingly**: durable truth lives in universe-layer records (rosters are the precedent —
D8), identity is durable (D5, D6), and the u32 tick epoch (~2.4 days per session-continuous
year at 20 Hz; wraps at ~6.8 years cumulative) is a recorded liability for the persistence
design to answer, not a surprise.

### D3 — The first two-real-clients milestone lands right after U3b

U3b/T2 build their wire per-client from day one (`SnapshotSender` per client; `StationRoster`
addressed per viewer — ADR-017 §1's privacy rule makes broadcast a leak). A **second-
commander gate (U3c)** follows U3b: two clients, distinct `PlayerId`s, on **disjoint
grids** — the shared-grid case is explicitly gated behind the interest/delta slice (D4),
because two fleets on one grid exceed the full-snapshot cap by arithmetic. *(NET-1/2,
SIM-4.)*

### D4 — 1,024 entities per grid stays normative; delta + interest gets an owner

The corpus cap stands as the number the **interest/delta ADR** must deliver a bounded view
of, under the unchanged 1,152 B datagram budget. That ADR is a named deliverable: drafted
during the station phase, implemented in a scheduled slice after U3c and **before any
shared-grid play or per-grid population above the full-snapshot cap**. Its scope, fixed
now: the snapshot-ack message and baseline ownership; the keyframe/initial-sync path (a
view switch is a mid-session join — this touches the `Transport` channel surface);
`Simulation`'s relevance hook; the graceful-degradation rule when interest still exceeds
budget; `lastOrderSeqProcessed` becoming per-session state outside the world hash; the
interest guarantee — **a commander's owned and selected ships are never culled from their
viewed grid**, and unreplicated presence is stated to the player through the icon ladder's
counted chips. Until it lands, bake and scenario invariants keep authored per-grid
population under the cap, and T2's accept tests the over-cap refusal loudly. *(NET-1,
SIM-4, UX-2, SIM-7 — ownership replication rides this slice, not a 22nd full-snapshot
byte.)*

### D5 — Durable player identity, minted now

A **`PlayerId`** distinct from the connection's `clientId` exists from the station phase's
schema cluster (T2), fixed to one value until accounts arrive. Sessions survive transport
disconnect for a grace window (the reconnect print's requirement); `Hello`/`Welcome` grow
the id plus a reserved token/resume field in the same bump. **Everything player-keyed that
U3b/T1 introduce — presence, view rights, fleet summaries, rosters, order and transfer
logs — keys on `PlayerId`, never on a connection id.** *(NET-2.)*

### D6 — `ShipId` widens to u32 everywhere, staged by the physics

One id everywhere is the owner's pick — logs, rosters, orders, and the wire always mean the
same ship. The staging is forced by arithmetic: a 23-byte `EntityRecord` (statusBits + a
u32 id) fits 39 records per datagram, under the 41-ship floor, so **the snapshot record
cannot widen until delta/interest removes the full-fit constraint**. Therefore:

- The sim, the universe registry, transfer/replay logs, rosters, and the order/command
  messages (control channel) go u32 in the T1/T2 clusters.
- `EntityRecord.id` stays u16 **as a wire truncation that is statically safe**: the
  registry allocator (D6a) does not issue ids ≥ 2¹⁶ until the delta cluster widens the
  record — an assert, not a runtime cast.
- **D6a.** Allocation is a **universe-registry-owned** concern: `World::Spawn` takes an id
  (injection), never mints one; authored occupants take deterministic ids derived from
  their anchor, partitioned from dynamic ids, so spin-up/teardown/recreate reproduce ids
  bit-exactly and no two worlds ever collide. The registry also owns the session-wide
  ship→location index that order routing, rosters, and summaries imply. *(SIM-1.)*

### D7 — The dock radius derives from the footprint

ADR-017 §2's rule becomes: every member inside
`max(DOCK_RADIUS_METRES, FormationExtentMetres(order) + margin)` of the structure —
computed from the same pure function both halves already share, so parity holds by
construction. "Together, one moment" is preserved exactly; only the number scales with the
fleet. ADR-017 §5's protection arithmetic is corrected in the same edit: fifteen seconds
covers ~1.2 km for a Battleship from rest (~1.6 km at cruise), not ~3 km — the protection
window and the parking design are re-checked against the class table, not the prose.
*(UX-1.)*

### D8 — Worlds forget, permanently

"Durable state lives at the universe layer; a torn-down world forgets everything but its
authored occupants" is the **standing rule**, not an MVP convenience. Future durable
content — depleted sites, wreck fields, player structures — is universe-layer records
spawned back in on spin-up, the roster pattern. Two repairs ride with it: the
**empty-world quiescence invariant** (a world holding only authored occupants ticks to the
same hash as its recreation) becomes a registry test, and the **registry hash/replay
domain excludes ship-less viewer-held worlds** (or, equivalently, spin-up/teardown join the
transfer log) — so viewer behaviour can never become a hidden sim input. *(SIM-2.)*

### D9 — Behaviour joins the compatibility gate; retunes refuse at the door

The schema text grows the **verdict-affecting constants** (`MAX_ORDER_LEGS`, the play-area
bound, `DOCK_RADIUS_METRES` and the D7 formula version when they land) **and the
validation check-order sequence**. A balance retune of any validation bound is thereby a
compatibility event — stale clients get `UpdateRequired`, the existing fail-closed
philosophy extended from shape to behaviour. No tolerance window: skew becomes impossible
rather than detected. *(SIM-5.)*

### D10 — Official service first; the remote-play ADR has its parameters

Operation model: **hosted servers only** for the foreseeable roadmap. That fixes the
remote-play ADR's inputs (a named deliverable gating the first remote deployment): trust
is a **pinned key shipped with the build**; client certificate validation is **on by
default off-loopback**; the transport gains its config surface (bind address, credential
source, validation policy); a first-order abuse posture (per-session order/message budget
with a named refusal) is in scope. The **platform floor is recorded: Windows 11 / Server
2022+** — Schannel QUIC stands, the OpenSSL flavour remains a listed swap, not a plan.
ADR-008 §8's "no architectural work remains" is amended to point here: the packaging split
is architecture-complete *and* gated on the remote-play ADR. Player-hosted servers are a
future decision, not a latent promise. *(NET-4, CPP's platform question.)*

### D11 — Release joins CI; performance numbers mean Release

Before U1: CI gains a **Release|x64 compile + `selfTest` leg**; standing spike 2 runs once
(Debug/Release replay hashes differ **by design** — recorded, so it never reads as a
defect); the CI toolset is pinned with a recorded upgrade cadence. **Every perf-gated
acceptance number — R17's parse threshold, U5's frame budget, D1c's tick budget — is
measured in Release**, with the Debug ratio noted beside it. *(CPP-3.)*

### D12 — dxc / SM 6.x everywhere

The shader build commits to **dxc and one SM 6.x target in both configurations**, ending
the undecided per-config fork (Debug was already 6.7 via per-file overrides while the
never-built Release path said 5.1). ADR-006 §12, ADR-013 §1a and the S7a note are amended;
the reserved GpuCull slot gets the compiler it will want. *(CPP-3.)*

### D13 — Device removal: relaunch-and-reconnect, plus the invariant

For the first remote-client milestone, a removed device **exits the client cleanly and the
player relaunches and reconnects** — acceptable because the server survives and reconnect
is first-class (D5). What is recorded now so in-session recovery stays cheap to add later:
**no session state may hold a device reference** (connection, snapshots, camera, selection
stay device-free — today's true property, promoted to a rule), and every device resource
keeps its re-runnable Create/Destroy pair. A risk row tracks it; a recovery slice is
scheduled only when it earns itself. *(CPP-2.)*

### D14 — Screens are engine surfaces, data-fed

The strategic map, hangar, and settings screens live in NeuronClient, fed **neutral data
through the seam**: the baked universe topology crosses **once, at boot, as a neutral
graph** (nodes/edges/labels/badge classes — the `OrderKinds` asked-once pattern); live
per-node data crosses as summary-keyed rows at summary rate; search and route-solve are
GameLogic pure functions reached through seam calls (the `SolveFormation` precedent).
ADR-014 §2c's leak test extends in words: **no security, sovereignty, or station-service
semantics in engine code — labels, badge classes and colours arrive as data.** The
fifth-project idea remains a named revisit trigger, now with a tripwire: if a screen needs
a game *rule* (not game *data*) to render, the question reopens. *(UI-5.)*

### D15 — The UI baseline package

Adopted as stated, unblocking the **UI-architecture ADR** (a named deliverable that blocks
U5 and T3; its scope is the review's UI-1/UI-2: the active-surface model and which passes
run beneath each surface, input routing as ordered consumption, the focus/text-input
machinery, one scrolling-list primitive, widget conventions generalised from CommandRow):

1. Editable text accepts **only the atlas-baked charset**, validated at the input widget
   and on user-layer load (fail-soft to substitution); i18n/localisation is post-roadmap,
   and its trigger — reopening ADR-006 §9 with per-locale bake lists and shaping — is
   named here. *(UI-3.)*
2. **Effective UI scale = DPI-derived default × the user's 0.8–1.6 preference**;
   `WM_DPICHANGED` is a resize-plus-rescale; minimum client area **1280×720**; the prints
   are normative at 1440×900. *(UI-4.)*
3. Keybinding capture belongs to the settings screen's first slice.
4. Touch/pad input is off the roadmap; the 48 px floor is target-size discipline only.
5. Toast **action payloads** (engine carries an opaque target, the game interprets — the
   `groupId` pattern) and the universe/station rows of the alerts taxonomy land at U3b;
   the palette becomes re-resolvable state at the settings slice; ADR-012 §3's user-layer
   sentence widens to enumerate its families (settings, wing names, route avoid-list) with
   unknown-key tolerance as forward-compat. *(UX-7, UI-6.)*
6. Surfaces are sized against a default per-commander envelope of **~200 owned ships /
   ~12 concurrent fleets** until the owner states otherwise. *(UX's envelope question.)*

### D16 — Presence-gated viewing is a placeholder, revocable

Docked-from-the-hangar viewing and any camera outliving presence are single-commander
conveniences the future intel-overlay design **may revoke** — recorded so nothing hardens
into a player right. The undefined edges get their rules now: **presence lost under a
pinned camera → the view falls to the map; every fleet in transit → the map is the view.**
*(UX-3.)*

### D17 — The transfer bus's total order *(adopted default)*

A universe-layer monotonic **`transferId`**, stamped at filing, is the tie-break:
transfers apply between ticks in **(apply tick, transferId)** order; registry iteration is
anchor-id order, stated; in-flight transit records and the bus queue fold into the
registry-level hash beside the rosters. This settles the divergent spellings in ADR-016 §4
("arrival tick, order id") and ADR-017 §9 ("apply tick, record order") — both now mean
this. *(SIM-3.)*

### D18 — Arrival contention *(adopted default)*

Anchors keep **one authored warp-in/undock point**; contention is answered by a
**deterministic per-order offset around the authored bearing** (a function of the transfer
record, not randomness), so simultaneous arrivals at a hub spread instead of stacking on
one point. Recorded before U1 so the bake's anchor record never needs a schema migration
for it. *(UX-6.)*

### D19 — The event record *(adopted default)*

A **per-commander, append-only event record at the universe layer** — beside the transfer
bus and the rosters — is the single producer behind the toast backlog/UNREAD counter,
REVIEW LOSSES, the reconnect away-log, and the strategic stream. U/T slices emit into it
from their first events (arrivals, halts, docks, berth holds), even while the only
consumer is the UNREAD count. *(UX-4.)*

## §A — The action register

Actions the build orders and the next design sessions now carry. "Gate" means the named
work must exist before the slice starts; finding ids point into the review's appendices.

| # | Action | Lands | From |
|---|---|---|---|
| A1 | ~~Topology ADR: grid-to-host assignment, transfer-bus ordering authority, connection handoff~~ **Delivered: [ADR-019](ADR-019-shard-topology.md)** — three roles (SimHost/SessionHost/Directory), anchor placement with region affinity, `(applyTick, hostId, counter)` order with departure-time filing, one client connection through the session front door; U2's eight constraints are its §6 | **Design deliverable, blocks U2** | D1b (NET-3) |
| A2 | ~~Release\|x64 CI leg + `selfTest`; run spike 2 once; pin the toolset; perf numbers stated as Release~~ **Done 2026-08-19:** the workflow is a `[Debug, Release]` matrix (`fail-fast: false`, source guards on the Debug leg only), the self test emits its replay hash, and **spike 2 is a standing job** that tables the two configurations' hashes and explains why differing is the expected result. *Toolset pinning deliberately not done* — the projects are on `v145`, which only the newest images carry, so a wrong pin trades a floating toolchain for a broken build; a "Record the toolchain" step prints image/VS/MSBuild/dxc every run so the pin can be taken from evidence | **Before U1** | D11 (CPP-3) |
| A3 | ~~Shader build → dxc/SM 6.x both configs; amend ADR-006 §12 / ADR-013 §1a / S7a note~~ **Done 2026-08-19:** `ShaderModel` is 6.7 in both `ItemDefinitionGroup`s and the eight per-file Debug-only overrides — the mechanism of the fork — are gone, so the setting lives once per configuration; ADR-006 §12, ADR-013 §1a, Dependency-Map and the S7a record are amended | **Before U1** (with A2) | D12 (CPP-3) |
| A4 | ~~Run R10's 1,024-entity soak, record the number; broadphase decision follows it~~ **Measured 2026-08-19 (indicative): 1,024 ships = 10.6 ms mean / 21.9 ms worst, 21 % of the tick — a capped grid costs ~⅕ core, so the broadphase is a later tuning decision, not a U2 prerequisite.** Recorded in R10. ~~*Still owed:* … an in-repo soak so the number is re-taken automatically~~ **Done 2026-08-19: `Outpost/TickSoak.h/.cpp`**, run by the self test in the shipping binary on every push. A four-rung ladder (41 / 256 / 512 / 1,024 — the authored fleet through D4's cap) over a converging lattice, wall-clock bounded so an unoptimised build measures fewer ticks rather than taking a minute over them; CI lifts the table into the run summary per leg, and D1c's derived figure (capped grids per core) is printed beside it. Two checks ride it: the populations are asserted unconditionally, and a **tripwire at twice the tick budget** is armed in Release only — deliberately not the acceptance number, which is a judgement made from the logged figures. ~~*Still owed:* the authoritative MSVC Release figure~~ **— taken by the first run, 2026-08-19: 41 ships 0.016 ms · 256 0.568 · 512 2.164 · 1,024 7.728 ms mean / 13.6 ms worst, 15 % of the tick, so ~6.5 capped grids per core.** Better than the indicative cross-build (10.6 ms), so D1c's headroom conclusion holds with more room than it was granted, and no broadphase is owed before U2. The same run priced D11: the Debug figure is **78.3 ms, 10.1× Release** — a capped grid that misses the tick by 3× in the configuration that ships nothing, which is precisely why the tripwire is Release-only | **Before U2** | D1c (SIM-6, NET-5) |
| A5 | ~~Anchor record carries the D18 offset rule and D6a's derived occupant ids from the first bake; warp-in invariant restated against the D7 radius~~ **Done 2026-08-19 with U1:** `Anchor` carries `warpInPoint`, `warpInFacingTurns16`, `arrivalSpreadRadiusCm`, `undockPoint`, `undockFacingTurns16` and the `occupantIdBase`/`occupantCount` pair, all baked and round-tripped. The offset *rule* is U3a's — what U1 owed was that contention never forces an anchor-schema migration, and the field it needs is in the file. Occupant ids are derived from the anchor rather than counted at spawn, so a recreated grid reproduces them exactly; blocks go only to anchors that author something, which is what keeps the highest authored id (26,848) inside the u16 window D6 holds | **U1** | D18, D6a, D7 |
| A6 | ~~Registry-owned id allocator (injection into `World::Spawn`), deterministic authored ids, ship→location index; u16-window assert until A14~~ **Done 2026-08-19 with U2:** `World::Spawn(spawn, id)` takes the id and `World` no longer mints one — the allocator is `WorldRegistry::AllocateShipId`, authored occupants take their anchor's derived ids, and `LocationOf` answers where a ship is without walking the registry. The u16 window is enforced by returning `INVALID_SHIP_ID` when the dynamic block is spent rather than by wrapping, because a wrapped id would put two ships on one number and the wire would never know | **U2** | D6a (SIM-1) |
| A7 | ~~Quiescence invariant test; registry hash/replay domain excludes ship-less worlds (or logs spin-up/teardown); "worlds forget" recorded as the standing rule~~ **Done 2026-08-19 with U2:** `AnEmptyWorldTicksToTheSameHashAsItsRecreation` is the quiescence test; `WorldRegistry::Hash` skips a world that holds only its authored occupants and is alive only because someone is watching it, and `AViewerHeldEmptyGridIsOutsideTheReplayDomain` holds that — without it, where a commander pointed a camera would be a simulation input. "Worlds forget" is stated on `TearDownIdle`: nothing is saved on the way out, so a world that comes back is rebuilt from content rather than restored from a memory of itself | **U2** | D8 (SIM-2) |
| A8 | ~~World-isolation invariant in the registry API; permuted-world-tick-order bit-identity test; build the ADR-007 §7 owner-assert~~ **Done 2026-08-19 with U2:** the isolation rule is stated on `WorldRegistry::Tick` (worlds share nothing during a tick; the bus and extract are the only crossings) and held by `TickOrderCannotMatterBecauseWorldsShareNothing`, which ticks the same worlds in a permuted order and demands the same hash — which is what makes world-level fan-out the pre-approved first parallel consumer. `NeuronCore/OwnerThread.h/.cpp` is the owner-assert, armed on `World::Tick`/`Spawn`/`Despawn`/`SubmitOrder`; it is debug-only and never simulation state, because an owner id that reached the world hash would make a replay depend on which thread ran it | **U2** | D1a (CPP-1, SIM-6) |
| A9 | ~~Transfer bus lands with `transferId` total order; transits + bus fold into the registry hash~~ **Done 2026-08-19 with T1's first slice:** `GameLogic/Transfer.h` is the bus vocabulary and `WorldRegistry` runs it — records are filed by a world during its tick, stamped `(hostId, counter)` by the registry because the counter is the host's, and applied *between* ticks in `(applyTick, transferId)` order before any world runs again. The in-flight bus and the rosters both fold into `WorldRegistry::Hash`, so a replay is the order logs plus the transfer log. Sorted at apply time rather than assumed sorted: the collection order is anchor-id order, which is an implementation detail nothing may depend on. Transit records are still U3a's — what T1 delivered is the mechanism and dock as its first rider | **T1** | D17 (SIM-3) |
| A10 | ~~Dock validation uses the footprint-derived radius~~ **Done 2026-08-19 with T1's first slice:** `DockRadiusMetres` is `max(DOCK_RADIUS_METRES, footprint + margin)` over the order's own solved formation — the same `SolveFormation`/`FormationExtentMetres` both halves share, exposed rather than hidden inside `ValidateOrder` because the client draws the circle the server judges against, and held by a parity test that compares both the verdict and the radius. The margin is one largest-class formation spacing, the same unit the berth scan will pad by. *Still owed:* ADR-017 §5's protection arithmetic and the window re-check, which land with the undock half | **T1** | D7 (UX-1) |
| A11 | ~~u32 ship ids in ... rosters, `StationCommand`~~ **Partly done 2026-08-19:** `StationCommand` and `StationRoster` carry u32 ship ids on the wire, and an id past this build's u16 range narrows to `INVALID_SHIP_ID` rather than truncating -- a truncating cast would validate a command against the wrong hull. The *sim's* `ShipId` stays u16, which is D6's own staging: `EntityRecord` cannot widen until the delta cluster removes the full-fit constraint | **T1/T2 clusters** | D6 (SIM-1) |
| A12 | ~~`PlayerId` + reserved token/resume in `Hello`/`Welcome`~~ **Done 2026-08-19 with T2's wire half:** both messages carry a `PlayerId` and a reserved `resumeToken`, `PROTOCOL_VERSION` went to 2 because the schema hash cannot gate the message that carries the schema hash, and the server answers `SOLE_PLAYER_ID` -- a *value*, not the connection id, which is the entire point of minting the distinction before accounts exist. *Still owed:* sessions actually surviving a disconnect for the grace window, and the player-keyed state that keys on it | **T2 cluster** | D5 (NET-2) |
| A13 | `SnapshotSender` built per-client; `StationRoster` addressed per viewer; over-cap refusal tested loudly | **T2 / U3b** | D3, D4 (NET-1, SIM-4) |
| A14 | ~~Interest/delta ADR drafted (scope fixed in D4, includes `EntityRecord` → u32 id and the ownership field); implementation slice scheduled after U3c, gating shared grids~~ **Delivered: [ADR-022](ADR-022-interest-and-delta.md)** — the session role is the one culling authority (ADR-019 §5d), `SnapshotAck` on datagrams against a ring of **views as sent** rather than world states, keyframes on a new reliable `Bulk` channel because a view switch is a mid-session join, a relevance hook that **ranks in the game and truncates in the engine**, the owned-and-selected guarantee with `culledCount` as its honesty, truncate-never-refuse, `lastOrderSeqProcessed` out of the world hash, and **relationship bits rather than an owner id** — two spare `statusBits`, zero bytes, exactly what the icon sheet asks for | **Draft during T-phase; slice after U3c** | D4, D6 (NET-1, SIM-4, SIM-7, UX-2) |
| A15 | View-switch acceptance parameterised over injected RTT; target = RTT + settle; the map named as the slow-switch surface | **U3b (and R18)** | UX-5 |
| A16 | Presence edge rules (pinned-camera fallback, all-in-transit → map); docked-viewing marked revocable | **U3b/T2** | D16 (UX-3) |
| A17 | ~~Event record producer at the universe layer; U/T slices emit from first events~~ **Built 2026-08-19 with T1:** `GameLogic/EventRecord.h/.cpp`, held by the registry, emitting on dock, undock, wing assignment and berth hold. Three numbers and no text -- a string could not be translated and the surfaces already know how to name a station -- with `count` making "eight ships docked" one line instead of eight; capped at 512 with the drop counted, because a truncated log and a quiet one are different statements. **Outside the hash**, which is the half worth stating: an event describes something the simulation already did, so folding the description in would make a replay depend on how talkative the build was. U3a onward keeps emitting into it; the consumers (UNREAD, REVIEW LOSSES, the away-log, the strategic feed) are still theirs to build | **T1/U3a onward** | D19 (UX-4) |
| A18 | Toast action payload + universe/station alert rows; palette re-resolvable; ADR-012 §3 families widened | **U3b / settings slice** | D15.5 (UX-7, UI-6) |
| A19 | ~~UI-architecture ADR (surface model, input consumption, text input/focus, scroll primitive, screen-data contract per D14)~~ **Delivered: [ADR-020](ADR-020-ui-architecture.md)** — `SurfaceId` stack with pop-back navigation, an `InputRouter` claiming pointer/wheel/keyboard independently, `UiFocus` + text edit state with the printable-key rule, one row-quantised scrolling list, and a screen-data contract of **three shapes, not three methods** (asked-once graph, summary-rate rows, pure query functions) | **Design deliverable, blocks U5 and T3** | D14, D15 (UI-1/2/5) |
| A20 | Run spike 3 (1,024-instance draw) + the S5 frame check; upload-ring and fixed GPU budgets sized from corpus caps (1,024 entities / 2,500 nodes), made config | **Before U5** | CPP-5 |
| A21 | ~~Schema text grows the verdict constants + check-order sequence (D9), clustered with T2's bump~~ **Done 2026-08-19 with T2's wire half:** `GAME_SCHEMA_TEXT` carries a `caps{...}` clause — ships per order, orders per snapshot, dock radius, undock protection, the parking ring's two radii and its bearing count, warp base seconds — and a `checkOrder{...}` clause giving the **sequence** the order and station checks run in, not just their names. The sequence is the part worth spelling out: two builds that check the same rules in a different order return *different reasons* for an order that breaks two of them, and the reason is what the player reads, so a bounce that says one thing on the client and another on the server is a compatibility failure even though both builds have the same enum. `OrderReason` is also numbered explicitly in the text now (through `UnknownAnchor = 14`), so a renumber is a hash change rather than a silent re-meaning | **T2 cluster** | D9 (SIM-5) |
| A22 | ~~Remote-play ADR (pinned key, validation on off-loopback, transport config surface, abuse budget); ADR-008 §8 gains the gate sentence~~ **Delivered: [ADR-023](ADR-023-remote-play.md)** — a **two-pin build constant** (never a config key, so the trust anchor is not user-editable and rotation is not an outage), `Listen`/`Connect` taking descriptors so the validation policy is *derived from the address* and the insecure combination cannot be spelled, the token step in the front door with the game never seeing it, and four budget rules that each close something in the tree today (the answered pre-join `Ping`, the duplicate `Hello`, connection-keyed sessions, unbounded order rate). ADR-008 §8's completeness claim is amended in its §7 | **Design deliverable, blocks first remote deployment** | D10 (NET-4) |
| A23 | ~~Device-removal risk row; "no session state holds a device reference" invariant recorded; recovery slice deferred~~ **Done 2026-08-19: R20** | **Risk register, now** | D13 (CPP-2) |
| A24 | ~~Common MSBuild props (toolset, `stdcpplatest`, conformance, explicit `/fp:precise`, no `/arch` overrides) or a CI guard over `.vcxproj`s; guard file-lists derived from the tree; clang-tidy CI step~~ **Done 2026-08-19**, all three clauses. **The props:** two sheets rather than one, because MSBuild gives the two kinds of setting two homes — `Outpost.Toolset.props` (the toolset, imported ahead of `Microsoft.Cpp.Default.props`) and `Outpost.Compile.props` (the four compiler switches, imported from each PropertySheets group). `/fp:precise` and no-`/arch` are **stated for the first time**; they were MSVC defaults the corpus described as decisions. Nine projects lost their eighteen copies, and two CI steps hold the line: one fails on a `.vcxproj` that re-spells any of the five or stops importing a sheet, the other reads the switches back off the compiler command lines. **The lists:** the project, engine-project and engine-test-project lists are now globbed, and the determinism guard's per-tick file list became an *exclusion* (every GameLogic file is per-tick code unless its job is the universe), so a file U1 or T1 adds is guarded the day it lands. **clang-tidy:** a step over GameLogic, non-blocking until a run comes back clean on the runner (the tree has never met clang-tidy on Windows), plus a **blocking** guard for the two rules AGENTS.md §1 says the config cannot express — R2's banned prefixes/suffixes and R7's file naming and dual registration. Writing it swept GameLogic clean: two identifier findings, a truncating division stated as one, `performance-enum-size` excluded for the reason the padding check already carried, and three unregistered files found in the project/filters pair. **The first Windows run then found a third thing the Linux sweep had not** (LLVM 20.1.8 against clang-tidy 18): `ComputeUniverseHash` was `noexcept` while sorting indices into a fresh vector per entity list, so `bad_alloc` on the boot path that reads the largest file this game owns (R17) was `std::terminate`. The annotation was incidental and is gone. **Both CI checks passed the same run** — 36 compiler command lines, every one carrying `/fp:precise`, none carrying `/arch:` or `/fp:fast`, which is the first time that invariant has been observed rather than assumed | **Before a second contributor** | CPP-4 |
| A25 | U3c — the second-commander gate: two clients, distinct `PlayerId`s, disjoint grids, full loop each over real loopback | **New slice after U3b** | D3 (NET-1/2) |
| A26 | ~~Annotate R10 (sim-only vs wire halves), R17 (Release numbers), R18 (RTT parameterisation); add rows for the replication cliff, device removal, topology~~ **Done 2026-08-19: R10/R17/R18 annotated (R10 now carries the measured soak), R19 (replication cliff), R20 (device removal), R21 (topology, mitigated by ADR-019) added** | **Risk register, now** | NET-5, D11, D13 |

## Consequences

- ADR-004, 006, 007, 008, 013, 014, 016, 017 carry amendment notes pointing here; the
  README's supersession list and decisions table grow one row.
- [Universe-Build-Order.md](../Universe-Build-Order.md) and
  [Station-Build-Order.md](../Station-Build-Order.md) carry the delivery half of §A —
  slice-accept additions, the three design deliverables (A1, A14, A19), and U3c.
- The [Risk-Register](../Risk-Register.md) gains R19–R21 and annotations per A23/A26.
- The [review document](../Scaling-Readiness-Review.md) stays the evidence record; where
  its decision list and this ADR differ, **this ADR wins** — it is the owner's answer.
- Three schema clusters absorb every wire change decided here (T1: bus + dock; T2:
  identity + u32 command ids + D9's schema-text growth + statusBits; delta slice:
  `EntityRecord` u32 + ownership) — each riding the existing fail-closed hash, none
  dribbling.
