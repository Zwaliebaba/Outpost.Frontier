# ADR-004 — Wire Protocol: Hand-Rolled, Full Snapshots, Acked Order Stream

**Status:** Accepted · 2026-08-17
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
2. **Message schemas for game state live in GameLogic** (`game::wire`), because snapshot and
   order payloads *are* game semantics and NeuronCore must have none (fixed charter). Transport
   framing, handshake, and ping messages live in NeuronCore (`neuron::wire`). The client gets
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
   - `ShipRecord{ u16 id, u8 class, u8 flags, i32 posX_cm, i32 posY_cm, i16 velX_cms,
     i16 velY_cms, u16 heading_turns16, u8 hull_pct, u8 shield_pct }` = **20 B**.
     Quantisation: position cm (±21,474 km range — covers any grid), velocity cm/s (±327 m/s;
     ship top speeds sit far below), heading 1/65,536 turn. Quantised values are what *all*
     clients see; the server sim keeps full float internally (ADR-005 owns the parity rule
     for client-side pre-checks).
   - `OrderStateRecord{ u32 serverOrderId, u16 groupSeq, u8 state, u8 legIndex, u8 legCount,
     Leg[≤4]{ i32 x_cm, i32 y_cm, u16 facing, u16 etaTicks } }` — drives ghost→underway
     promotion and per-leg ETAs exactly as drawn (`puck-and-wheel.png` §4, 4-leg queue cap
     from `strategic-map.png`).
   - Budget: header 16 B + 41 ships × 20 B + a few order records ≈ **~900 B** — one datagram.
     At the 1,024-entity cap a full snapshot is ~20 KB ⇒ **delta + interest management is the
     designed growth path, not larger datagrams**; cadence decimation per zoom tier is already
     legal (header carries the tick).

### Orders (C→S)
7. `OrderSubmit{ u32 orderSeq /*client monotonic*/, u8 kind /*Move only in MVP*/, u8 formationId,
   u8 queueMode /*replace|append*/, u16 shipCount, u16 shipIds[], Leg target{ i32 x_cm, i32 y_cm,
   u16 facing } }`.
   - The client pre-checks with the **same GameLogic validation function** the server runs
     (same reason-code enum, defined in GameLogic) against its latest snapshot; a local refusal
     renders the same bounce as a server `OrderAck{rejected}` — BounceParity by construction,
     not by discipline. Server verdict remains the only authority (pre-check runs on stale
     data by design and may pass what the server then refuses).
   - Ghost lifecycle mapping: send ⇒ PENDING (dashed); `OrderAck{accepted}` or the order
     appearing in `Snapshot.OrderStateRecord` (whichever first) ⇒ promoted; `OrderAck{rejected,
     reason}` ⇒ 150 ms bounce + reason toast; `queueMode=append` beyond 4 legs ⇒ rejected
     `QueueFull` (the strategic-map sheet's "client feeds one jump at a time" pattern relies on
     this being cheap and honest).

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
