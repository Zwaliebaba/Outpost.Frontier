# ADR-004 — Wire Protocol: Hand-Rolled, Full Snapshots, Acked Order Stream

**Status:** Accepted · 2026-08-17 · amended by [ADR-018](ADR-018-scaling-baseline.md)
(2026-08-19): the schema text grows the verdict-affecting constants and the check-order
sequence (D9); the §6 growth path gets an owner and a scope — the interest/delta ADR (D4);
ship-id width staging on the wire (D6) · **§6's growth path is designed as of
[ADR-021](ADR-021-interest-and-delta.md)** (2026-08-19): `SnapshotAck`, baselines held as
*views as sent*, keyframes on a new reliable `Bulk` channel, priority truncation replacing
whole-snapshot refusal, `EntityRecord.id` → u32, and the relationship bits that spend no
byte at all
**Depends on:** ADR-002 (tick), ADR-003 (channels & 1,152 B datagram cap)
**Feeds:** ADR-005 (schema ownership), HUD feedback loop

## Context

Two channels exist (ADR-003): a reliable ordered message channel (control) and unreliable
datagrams (state). The corpus fixes the feedback contract: client-side **order ghosts** are
"the only client-side optimism in the game", promoted "on the next snapshot", rejected with a
**reason** that must be identical whether it came from the client pre-check or the server
(`puck-and-wheel.png` §4 "BounceParity"), and the session flow fail-closes on a **schema hash**
mismatch (`session-surfaces.png`). Library policy forbids protobuf/flatbuffers-style codegen
without approval — and with ~a dozen message types, generated serialization would be pure
overhead anyway.

## Decision

### Serialization
1. **Hand-rolled, explicit, little-endian**, over bounds-checked `ByteWriter`/`ByteReader`
   primitives in NeuronCore. Each message is a POD-ish struct with `Write(w)`/`Read(r)`
   free functions colocated with its definition. No reflection, no macros beyond a
   trivial `NEURON_WIRE_CHECK` for read-underrun.
2. **Message schemas for game state live in GameLogic** (`Game`), because snapshot and
   order payloads *are* game semantics and NeuronCore must have none (fixed charter). Transport
   framing, handshake, and ping messages live in NeuronCore (`Neuron`). The client gets
   game message types by linking GameLogic — the dependency rules explicitly allow this, and
   BounceParity independently forces the client to link GameLogic anyway (shared validation).
   *Recorded as a dependency-rule refinement in the Dependency Map.*
3. **Schema hash:** `SchemaHash = FNV-1a64` over a compile-time string that concatenates every
   message's name, field names, types, and quantisation constants (maintained next to the
   definitions; a `selfTest` check catches drift). Exchanged in `Hello`; mismatch ⇒
   `UpdateRequired` + close (the corpus's guaranteed post-patch screen, an MVP log line).

### Framing
4. **Control channel:** `[u16 length][u16 type][payload]` per message.
   **Datagram channel:** `[u16 type][payload]`, one message per datagram, ≤ 1,152 B total.
5. **Protocol constants:** `ProtocolVersion u16` (breaking framing changes only), ALPN `opf/1`.

### Message set (MVP, complete)
| Channel | C→S | S→C |
|---|---|---|
| control | `Hello{ver, schemaHash, name}` | `Welcome{clientId, tick, tickRate, worldMeta}` / `UpdateRequired{serverSchemaHash}` / `Refuse{reason}` |
| control | `OrderSubmit` (below) | `OrderAck{orderSeq, verdict, reasonCode, serverOrderId}` |
| control | `Goodbye{reason}` | `Goodbye{reason}` |
| datagram | `Ping{clientSendUs}` | `Pong{clientSendUs, serverTick}` |
| datagram | — | `Snapshot` (below) |

### State replication
6. **MVP replicates full snapshots every tick** — no deltas, no baselines to track, loss-proof
   by construction (any snapshot fully replaces the previous).
   `Snapshot = Header + ShipRecord[] + OrderStateRecord[]`.
   - `Header{ u32 tick, u32 baselineTick /*=0: full*/, u16 shipCount, u16 orderCount,
     u32 lastOrderSeqProcessed }` — `baselineTick` is reserved so delta-vs-acked-baseline
     (Quake-3 model) slots in later without a format break; `lastOrderSeqProcessed` closes the
     order feedback loop even if an `OrderAck` is delayed.
   - `ShipRecord{ u16 id, u8 class, u8 flags, i32 posXCm, i32 posYCm, i16 velXCmPerSec,
     i16 velYCmPerSec, u16 headingTurns16, u8 hullPct, u8 shieldPct }` = **20 B**.
     Quantisation: position cm (±21,474 km range — covers any grid), velocity cm/s (±327 m/s;
     ship top speeds sit far below), heading 1/65,536 turn. Quantised values are what *all*
     clients see; the server sim keeps full float internally (ADR-005 owns the parity rule
     for client-side pre-checks).
   - `OrderStateRecord{ u32 serverOrderId, u16 groupSeq, u8 state, u8 legIndex, u8 legCount,
     Leg[≤4]{ i32 xCm, i32 yCm, u16 facing, u16 etaTicks } }` — drives ghost→underway
     promotion and per-leg ETAs exactly as drawn (`puck-and-wheel.png` §4, 4-leg queue cap
     from `strategic-map.png`).
   - Budget: header 16 B + 41 ships × 20 B + a few order records ≈ **~900 B** — one datagram.
     At the 1,024-entity cap a full snapshot is ~20 KB ⇒ **delta + interest management is the
     designed growth path, not larger datagrams**; cadence decimation per zoom tier is already
     legal (header carries the tick).

### Orders (C→S)
7. `OrderSubmit{ u32 orderSeq /*client monotonic*/, u8 kind /*Move only in MVP*/, u8 formationId,
   u8 queueMode /*replace|append*/, u16 shipCount, u16 shipIds[], Leg target{ i32 xCm, i32 yCm,
   u16 facing } }`.
   - The client pre-checks with the **same GameLogic validation function** the server runs,
     reached through `WorldView::PreCheck` rather than a link-time dependency (ADR-014),
     (same reason-code enum, defined in GameLogic) against its latest snapshot; a local refusal
     renders the same bounce as a server `OrderAck{rejected}` — BounceParity by construction,
     not by discipline. Server verdict remains the only authority (pre-check runs on stale
     data by design and may pass what the server then refuses).
   - Ghost lifecycle mapping: send ⇒ PENDING (dashed); `OrderAck{accepted}` or the order
     appearing in `Snapshot.OrderStateRecord` (whichever first) ⇒ promoted; `OrderAck{rejected,
     reason}` ⇒ 150 ms bounce + reason toast; `queueMode=append` beyond 4 legs ⇒ rejected
     `QueueFull` (the strategic-map sheet's "client feeds one jump at a time" pattern relies on
     this being cheap and honest).

7a. **The case "whichever first" does not cover** (S9). A snapshot can pass a ghost's sequence
   in `lastOrderSeqProcessed` while listing no record for it — order records exist only while
   an order does, so the order either **finished** or was **refused with the ack still in
   flight**, and from the snapshot alone those are the same picture. Retiring on the spot makes
   a refusal vanish with no bounce, which `puck-and-wheel.png` §4 calls indistinguishable from
   a dropped packet.

   So a still-PENDING ghost in that state waits half a second for its ack before retiring. The
   window normally never opens: the ack is on the **control** channel and the snapshot is not,
   so the ack arrives first unless a lost datagram puts it behind its own resend. This is also
   the concrete reason the ack is reliable rather than riding the snapshot — a lost refusal
   leaves a promise on screen that no later snapshot can correct.

   A ghost nobody ever answers is dropped after five seconds and **counted**, not bounced.
   There is no reason code for "the link died", and borrowing one would put a word in the
   game's mouth; the §4 rule is about refusals, which always carry a reason.

7b. **`orderSeq` travels out of the game to come back** (S9). The ack echoes the client's
   sequence, which lives *inside* the payload the engine frames and never parses (ruling 4). An
   engine that dug four bytes out of it to fill in the ack would have started reading game
   semantics for the sake of one field — so `Neuron::OrderVerdict` carries `orderSeq` back out
   of the game instead. The game already parsed it; the engine echoes what it is handed.

## Alternatives rejected

- **Generated serialization (protobuf/flatbuffers/custom codegen)** — external-library policy,
  build complexity, and zero benefit at this message count. Hand-rolled + schema-hash selftest
  gives the safety codegen would.
- **Delta compression in MVP** — needs per-client ack/baseline bookkeeping and a fragmentation
  story, to compress ~900 B on loopback. Deferred; wire field already reserved.
- **Reliable-channel snapshots** — head-of-line blocking on a hitch would stall fresh state
  exactly when it matters; snapshots are idempotent and belong on datagrams. Orders are the
  opposite (rare, must-arrive, ordered) and belong on the stream.
- **Float32 on the wire** — 20 B/ship quantised beats 26 B float, kills cross-client float
  nondeterminism worries for presentation, and gives delta-friendly integer fields later.

## Consequences

- The client's rendered world is a pure function of quantised replicated fields — which is what
  the de-clutter/determinism requirement (F10, icon sheet) demands.
- Full-snapshot MVP means zero recovery logic; a lost datagram costs 50 ms of freshness,
  covered by interpolation (ADR-002).
- Schema evolution is manual: any change to a message must touch the schema string beside it
  (checked by selftest). Cross-version compatibility is explicitly *not* attempted — the
  UpdateRequired path is the product answer (matches corpus).
- The 4-leg queue cap is wire-level; the strategic route planner's "feed one jump at a time"
  client behaviour (post-MVP) needs no server change.
