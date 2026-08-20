# ADR-025 — Persistence: the Journal, the Snapshot, and What a World May Forget

**Status:** Accepted · 2026-08-20 (owner-accepted the day it was written) · **deliverable
D-P1** of [Economy-Build-Order.md](../Economy-Build-Order.md), whose **E2 gate it now
clears** — the site ledger is the first durable state this tree will own. Nothing is built;
what acceptance changes is that E2 may build against this rather than around it.
**Depends on:** ADR-002 (tick), ADR-004 (little-endian byte discipline), ADR-005
(determinism, the replay contract), ADR-007 (single-writer worlds, foreign threads enqueue),
ADR-008 (boot and shutdown ordering), ADR-012 (JSON config, no argv, no environment),
ADR-014 (the engine declares, the game implements, the composition root wires), ADR-016 §4
(the transfer bus, worlds spun up and torn down), ADR-017 (the station roster), ADR-018 D2
(worlds forget — durable state lives at the universe layer) / D19 (the event record),
ADR-019 (the anchor is the placement unit; the tick is shard-global), ADR-024 (Bays, site
ledgers, refine jobs, upgrade projects — and §7a, which ruled this ADR into existence)
**Amends:** ADR-017's "no persistence" note (it named the roster as the obvious save anchor
and declined to create the save file; this creates it); ADR-024 §7a (which decided the
*shape* and the *venue* and deferred the format to here, exactly as it said).

## Context

ADR-024 §7a ruled it: the universe layer's durable state persists through an **engine-owned
append-only journal plus a periodic snapshot**, written off the Sim thread, replayed at boot,
with a hash proving the reload. A SQL instance was staged rather than rejected — its entry
point is the service layer, where ad-hoc queries actually live. This ADR is the format, the
cadence, the recovery, and the seam.

The deferral it ends is old and was named honestly each time. ADR-017 wrote *"the roster is
the obvious save anchor — the RESUME card's 'Docked at Vesta-3' becomes literally true when a
save file exists — but no save file exists, and this ADR does not create one."* ADR-018 D2
made the universe layer the home of durable state on the reasoning that worlds forget. What
changed is ADR-024: a Station Bay a player fills with ore, a refinery job that runs while
they sleep, and a communal station upgrade that took a corporation a week are not things a
process restart may discard. An economy whose state evaporates on restart is not a persistent
service; it is a demo.

Three things in the tree shape the answer before any design begins.

**The engine may not know what a roster is.** ADR-014 §1 forbids `Neuron*` from referencing
GameLogic, and §2a settled the pattern when the seam first had to be built for real: the
engine declares, GameLogic supplies pure types and pure functions, and **the composition root
holds the vtable**. Persistence has exactly that shape — durability is an OS concern (files,
flushes, atomic renames) and the *meaning* of the bytes is a game concern — so the split is
not a compromise here, it is the only arrangement that keeps `GameLogicTests` running with no
filesystem.

**The Sim thread owns the worlds, and nothing else may touch them.** ADR-007 §5 and §7 make
that mechanical rather than aspirational. So serialisation happens **on Sim**, where the state
legally can be read, and everything after that moves bytes that are already a snapshot of a
moment.

**The anchor is the placement unit** (ADR-019), and the tick is shard-global. Durable state is
therefore anchor-keyed and host-partitioned by construction, which is what makes "one file per
host" the multi-host answer rather than a redesign.

## Decision

### 1. The durable line: identity and location, never intention

ADR-018 D2's "worlds forget" is the right rule and it is routinely misread, so this ADR
states the line it actually draws:

> **A ship's identity and where it is are durable. Its intentions and its motion are not.**

Durable, at the universe layer:

- **Ships**, wherever they are — `(ShipId, HullClass, WingId, PlayerId)` plus **where**: an
  anchor and, for a ship standing on a grid, its position and heading.
- **Station rosters** (ADR-017 §1) — the docked set per station.
- **Station Bays** (ADR-024 §5c) — per `(PlayerId, station)` ore and alloy ledgers.
- **Ship cargo manifests** — what is in a hull, wherever that hull is.
- **Site ledgers** (ADR-024 §3d) — per site, the pool remaining and the epoch index it was
  last re-formed at.
- **Refine jobs and queues** (ADR-024 §6b), with the tick each completes at.
- **Refinery tiers and upgrade-project contributions** (ADR-024 §6c).
- **In-flight transfer records** — the bus, which is already hashed state.
- **The ship-id allocator's high-water mark** — see §1a, because this one is a trap.
- **The shard tick**, which every record above is stamped against.

Not durable, and deliberately:

- **Order queues, steering, ETAs, formation solves** — a fleet reloads at rest, holding
  position, with an empty queue. Intention is the player's to restate.
- **Undock protection windows** (ADR-017 §5) — fifteen seconds do not survive a restart, and
  a shard that restarts owes nobody a protected launch.
- **Wrecks** (ADR-024 §5b) — already ruled non-durable: a wreck is the world's.
- **Live grids and viewers** — a world is a runtime, spun up when someone arrives.

**Why ships in space are durable when "worlds forget."** They read as contradictory only if
"world" is heard as "everything at a place". A world is a *runtime*: the tick, the order
tables, the steering, the separation pass. A ship is the player's property. Discarding a
fleet parked at a mining field because the shard was restarted would be taking something the
player owns, and sweeping it to the nearest station instead would be inventing a teleport to
avoid writing a record. So the fleet is written down where it stood, restored at rest, and
told nothing about what it was doing.

### 1a. The two traps, named before they are sprung

**Ship ids must never be reused across a restart.** The registry allocates from a per-host
block (ADR-018 D6a, ADR-019 §5c). If the high-water mark is not durable, a restarted shard
re-issues ids that a Bay row, a transfer record, a log line and a client's selection all still
name — and the failure is silent, because every one of those lookups succeeds against the
wrong ship. The mark is durable, restored before anything spawns, and it only ever moves
forward. A restore that would lower it is a refusal, not a clamp.

**The reload-proof hash is not `WorldRegistry::Hash()`.** That hash is the *replay* domain
(ADR-018 D8) and it folds order queues, which §1 has just declared transient — so a reload
would legitimately fail to reproduce it, and a check written against it would either be
wrong or teach everyone to ignore it. This ADR introduces **`DurableHash()`**, folded over
exactly the list in §1 and nothing else, in anchor-id then id order. It is what a snapshot
records and what a replay verifies. The two hashes answer different questions and both keep
their own.

### 2. The seam: the engine persists bytes it cannot read

Three parts, in the ADR-014 §2a arrangement.

**GameLogic — pure, no filesystem.** Two functions and a hash, in the shape everything else in
that library already has:

```
bool WriteDurableState(const WorldRegistry&, Neuron::ByteWriter&);
bool ReadDurableState(Neuron::ByteReader&, WorldRegistry&, std::vector<PersistenceDiagnostic>&);
std::uint64_t DurableHash(const WorldRegistry&);
```

Bytes in, bytes out, diagnostics on malformed input, never a path and never a throw — the
`ParseUniverse` and `ParseEconomy` posture applied to a binary format. `GameLogicTests` keeps
its property of needing no fixtures: a round-trip test builds a registry in code, writes it to
a buffer, reads it into a second registry, and asserts the two `DurableHash()` values agree.

**NeuronServer — the store, which never learns what it is storing.** An engine-declared
`DurableStore` owning files, framing, checksums, flushing, snapshot rotation and torn-tail
recovery, with an interface in units of opaque records:

```
void Append(std::uint32_t recordKind, std::span<const std::uint8_t> payload);
bool Replay(const ReplayHandler&);          // hands each surviving record back, in order
bool WriteSnapshot(std::span<const std::uint8_t> state, std::uint64_t durableHash);
```

It knows a record has a kind, a length and a checksum. It does not know that kind 3 is a Bay.

**Outpost.exe — the wiring**, as §2a requires: it constructs the store, calls GameLogic's pure
functions, and hands the results across. Thin enough that there is nothing to test, which is
the standing price of the arrangement and the reason the interfaces get stubbed in the engine
test projects instead.

### 3. The journal

Append-only, little-endian, framed, written with `Neuron::ByteWriter` — ADR-004's discipline,
reused rather than reinvented, because a second serialisation convention in one tree is a
second set of endianness bugs.

A frame:

| Field | Width | Why |
|---|---|---|
| `magic` | u32 | Recognises the file and refuses somebody else's |
| `payloadBytes` | u32 | Bounds the read before a single byte of payload is trusted |
| `recordKind` | u16 | Numbered, never renumbered — the `OrderKind` discipline |
| `shardTick` | u32 | Every record is stamped, so replay is ordered and idempotent |
| `payload` | n | Whatever GameLogic wrote |
| `crc32` | u32 | Over kind, tick and payload — what makes a torn tail detectable |

The file header carries `JOURNAL_FORMAT_VERSION`, the `universeHash`, the `economyHash`, the
`hostId` and the shard tick the last snapshot was taken at.

**Records are outcomes, not commands, and the distinction is load-bearing.** A journal entry
says *"these ships are now docked at anchor 412"*, never *"a Dock order arrived"*. Replaying
an outcome needs no simulation, cannot diverge, and cannot be affected by a later balance
change; replaying a command would make the journal a second replay engine that must agree
with the first forever. **The journal is therefore not the replay log** (ADR-016 §4,
ADR-017 §9) and must never be used as one: the replay log holds inputs and proves the
simulation is deterministic, and this holds results and proves the service did not forget.
Both may exist; neither substitutes for the other.

### 4. Threading: serialise on Sim, write anywhere else

The Sim thread, at the between-ticks apply point where the transfer bus already runs, appends
finished records into a **lock-free SPSC ring** — the mechanism ADR-007 §7 already sanctions
for foreign threads, run in the other direction. A **journal lane** (registered like every
other lane, §8 of that ADR) drains the ring, writes, and flushes.

What that buys: the tick never waits on a disk, and the disk never sees a half-written state,
because what reaches the ring was serialised inside the apply point where the state was
legally readable and momentarily still.

**The durability window is bounded and named rather than implied.** The journal lane flushes
when the ring crosses a watermark or every `JOURNAL_FLUSH_MILLISECONDS` (1,000), whichever
comes first, and always on a clean shutdown before ADR-008's ordering releases the sim. So:

- **Clean shutdown loses nothing.**
- **A hard kill or power loss may lose up to the last second of accepted outcomes.**

A second is the right trade for this game and the sentence is here so the next person reads it
as a decision instead of discovering it as a bug. Per-record fsync was measured against what it
buys — an economy in which nothing is lost on power failure — and rejected on cost: the shard
would fsync on every dock, every mining cycle's ledger debit and every transfer, which is a
disk write on the tick's own cadence.

### 5. The snapshot, and the rotation that makes it safe

Every `SNAPSHOT_INTERVAL_SECONDS` (300) and on clean shutdown, the whole durable state is
written as one blob with its `DurableHash()`. The rotation is the ordinary safe sequence, and
each step is there because the crash between it and the next one has to be survivable:

1. Write `shard-<hostId>.snapshot.tmp`, then flush it.
2. Atomically rename it over `shard-<hostId>.snapshot`.
3. Truncate the journal and write its header with the new snapshot tick.

A crash before (2) leaves the previous snapshot and a journal that still covers everything
after it. A crash between (2) and (3) leaves a good snapshot and a journal with records older
than it, which replay skips by tick. There is no window in which both files are needed and one
is missing.

**Checkpoint records** carry a `DurableHash()` every `CHECKPOINT_INTERVAL_TICKS` (1,200 — one
minute). Replay verifies at each. Hashing every record would double the cost of the cheapest
ones; hashing only at snapshot time would let a divergence hide for five minutes. A minute is
the compromise, and the number is a constant so it can be argued with.

### 6. Boot, recovery, and what a refusal looks like

1. Read the header. Wrong magic or a `JOURNAL_FORMAT_VERSION` this build does not know ⇒
   **refuse to start**, naming both versions. There is no silent upgrade path: a shard that
   guesses at an older format writes a newer one over it.
2. **Guard on `universeHash`, and only on it.** A re-baked universe renumbers anchors, so a
   roster keyed by anchor is nonsense against it — that is a refusal with a message naming
   both hashes and the migration nobody has written yet (§9). `economyHash` is **recorded and
   compared but never fatal**: retuning a hold size must not invalidate a shard. Where a
   retune shrinks a hold below what a ship carries, the load **clamps and logs** per ship
   rather than refusing — the numbers moved under the content, which is a content decision,
   not a corruption. *(This is the reason E1a kept the two hashes separate in the log while
   the wire carries them mixed: the handshake cares that the pair matches, and persistence
   cares about exactly one of them.)*
3. Load the snapshot, verify its `DurableHash()`.
4. Replay journal records with `shardTick` greater than the snapshot's, in file order,
   verifying each checkpoint.
5. **Stop at the first bad frame** — a length that runs past the end, or a failed CRC — and
   truncate the file there. That is the torn tail of the write that was in flight when the
   power went, it is expected, and it is logged as a count of records recovered and bytes
   discarded rather than as an error.
6. A bad frame **in the middle**, with good frames after it, is corruption rather than a torn
   tail: refuse, and leave both files untouched for whoever has to look.

Nothing here is silent. A boot that recovered a torn tail says so; a boot that refused says
which check failed and what the two values were.

### 7. Where the files live

A configured directory, JSON like everything else (ADR-012 §A — no argv, no environment):

```jsonc
"persistence": { "directory": "ShardState", "enabled": true }
```

Resolved by `ResolveContentPath`'s sibling for writable paths, defaulting beside the
executable. **Not** the LocalAppData user layer: that is one player's settings on one machine,
and this is a service's state.

**One pair of files per host**, named by `hostId` — `shard-0.snapshot`, `shard-0.journal`.
At one host that is a naming convention doing nothing, which is exactly the ADR-019 posture:
the anchor is the placement unit and a host owns anchors, so the day a second SimHost exists
it writes its own pair and nothing has to be split.

`"enabled": false` is the headless-test and `selfTest` posture — a shard that persists nothing
and says so at boot. It is not a fallback: a shard configured to persist that cannot open its
directory **refuses to start**, because the alternative is a service that looks healthy and is
quietly amnesiac.

### 8. Testing, and the property that matters

- **`GameLogicTests`** — the pure round-trip: build a registry with rosters, Bays, ledgers,
  jobs and ships both docked and in space; write; read into a second registry; assert equal
  `DurableHash()`. Plus the id high-water mark surviving, and a truncated buffer producing
  diagnostics rather than a half-built registry (the `ParseEconomy` posture).
- **`NeuronServerTests`** — the store with no game in it: framing, CRC, a deliberately torn
  tail recovering to the last good record, a mid-file corruption refusing, snapshot rotation
  interrupted at each of its three steps, and a journal older than its snapshot being skipped.
- **`selfTest`** — the property the whole ADR exists for, headless and end to end: dock a
  fleet, move ore into a Bay, start a refine job, **stop the host and start it again**, and
  find the roster, the ore and the job still there with the job's completion tick unmoved.
  That is E4's milestone G1 claim, and this is where it is actually proven.

### 9. What this deliberately does not do

- **No SQL, and no rejection of SQL.** The shard's durable state is single-writer and
  key-addressed, which is journal-shaped. Ad-hoc queries are where a database earns its keep
  and they live at the **service layer** — market order books, the Directory role, web tooling,
  analytics — which is SQL's named entry point when those arrive. Embedded SQLite remains
  available to a later revision of this ADR under the external-library rule (AGENTS.md §5),
  which needs the owner's explicit approval and does not have it here because nothing yet
  needs it.
- **No migration across a universe re-bake.** §6 refuses, and the trigger for designing one is
  named: **the first re-bake of a universe a live shard has state against.** Until a shard has
  run long enough for that to hurt, a migration tool would be written against guesses.
- **No player accounts, no authentication, no cross-shard state.** `PlayerId` is durable here
  as a key; who owns it is the Directory role's question (ADR-019) and ADR-023's.
- **No backups, no point-in-time restore, no retention policy.** Copying two files while the
  shard is down is the whole operational story today, and saying so is better than implying
  more.
- **No encryption and no tamper resistance.** The files are as trusted as the machine.
- **No compaction beyond snapshot-and-truncate**, and no attempt to keep history: this records
  what is, not what happened. What happened is the event record, and it is capped.
- **The event record is durable but unhashed.** ADR-018 D19 keeps it out of the hash on purpose
  — it is a description of what the simulation did, and folding it in would make a replay
  depend on how talkative the build was. It rides the journal as its own record kind and stays
  outside `DurableHash()`, which is the same line drawn twice.

## Alternatives rejected

- **Snapshot only, no journal.** One file, no framing, no CRC, no replay — and a five-minute
  hole on every crash. The journal is what turns "we lost the afternoon" into "we lost a
  second". Rejected.
- **Journal only, no snapshot.** Boot time grows without bound and a week-old shard replays a
  week. Rejected.
- **Per-record fsync.** Buys losing nothing to power failure, at the price of a disk flush on
  the tick's cadence. §4 names the window instead. Rejected, revisitable if a shard ever holds
  something a second of loss is unacceptable for.
- **Persisting commands rather than outcomes.** Makes the journal a second simulation that has
  to agree with the first across every future balance change. §3's reasoning. Rejected.
- **Persisting the whole world, orders and all.** Restores a fleet mid-manoeuvre and makes
  every transient field a compatibility surface forever. §1's line is narrower on purpose.
  Rejected.
- **Writing on the Sim thread.** Simplest, and it puts a disk in the tick budget. Rejected on
  ADR-002's arithmetic.
- **Reusing `WorldRegistry::Hash()` as the reload proof.** §1a: it folds transient state, so
  the check would be wrong by construction. Rejected.
- **A SQL server from day one.** A second service to operate, a client library, schema
  migrations against a tick loop, and async discipline so a write never blocks it — bought to
  serve queries nothing yet asks. Staged, not rejected. See §9.
- **The LocalAppData user layer as the home.** It is one player's settings on one machine;
  this is a service's state. Rejected on what the two things are.

## Consequences

- **E2 unblocks.** The site ledger is the first durable state, and it now has somewhere to
  live and a hash that proves it came back.
- **ADR-017's "no persistence" note is spent**, and the RESUME card's *"Docked at Vesta-3 · 4
  days 6 hours since last session"* becomes literally true for the first time.
- **New files, in three projects**, each registered in both registries and both project files
  when they land: `GameLogic/DurableState.{h,cpp}`, `NeuronServer/DurableStore.{h,cpp}`, and
  the composition-root wiring in `Outpost/`.
- **New constants join the envelope suite's guardianship**: `JOURNAL_FORMAT_VERSION`,
  `JOURNAL_FLUSH_MILLISECONDS` (1,000), `SNAPSHOT_INTERVAL_SECONDS` (300),
  `CHECKPOINT_INTERVAL_TICKS` (1,200) — table data, retunable as table edits.
- **`Outpost.json` grows a `persistence` block** (§7), and `AppConfig` grows with it.
- **Risk-register row R26 lands with acceptance**: *a torn journal or a refused load takes a
  shard's state with it.* Mitigation is designed in — CRC per frame, snapshot rotation with no
  both-files-bad window, refusal rather than partial load, and the `selfTest` restart scenario
  — and the early-validation signal is that restart scenario running on every push.
- **The replay contract is untouched.** The journal is not a replay log (§3) and the
  double-run suite keeps proving exactly what it proved before.
