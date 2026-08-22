# ADR-022 — Interest and Delta Replication: Acked Baselines, One Culling Authority, One Guarantee

**Status:** Accepted · 2026-08-19 (design deliverable [ADR-018](ADR-018-scaling-baseline.md)
A14 — drafted during the station phase) · **Implemented 2026-08-22 as U3d**
([Universe-Build-Order.md](../Universe-Build-Order.md)): **U3d-a** the ranking and §7's
subtraction from the world hash, **U3d-b** the wire cluster — `SnapshotAck`, `DeltaHeader`,
the keyframe on the new `Bulk` channel, u32 ids, the relationship bits, the sent-view baseline
ring and priority truncation. Two readings this ADR left open were taken by the slice and are
recorded with it rather than here: **tier 1** reads as "inside the camera's extent" rather than
§4's literal "a visible relationship **and** inside the extent", because the first conjunct has
no producer until the combat phase gives `Allied`/`Hostile` a meaning; and §4's `InterestQuery`
needed a **`ViewFocus` message** the ADR does not name, because a server cannot keep §5a's
selection guarantee for a selection nobody told it about — which amends ADR-016 §7's "the
server has no business holding this". **R19 is retired**; the shared-grid gate (ADR-018 D3) is
lifted
**Depends on:** ADR-002 (tick, interpolation, STALE), ADR-003 (channels, the 1,152 B client
datagram cap), ADR-004 (wire, the reserved `baselineTick`, the schema hash), ADR-005
(determinism, the world hash), ADR-014 (the engine/game seam and its named relevance hook),
ADR-016 §6–§7 (per-grid snapshots, the view model), ADR-017 §1 (roster privacy) §5
(`statusBits`, the 43-record cap), ADR-018 (**D3** the two-client gate, **D4** this ADR's
scope, **D5** `PlayerId`, **D6** u32 ship ids, **D15.6** the per-commander envelope),
ADR-019 §5 (the session front door, and §5d which hands this ADR a changed problem)
**Amends:** ADR-003 §1 (the seam grows a **third channel** — §3c); ADR-004 §6 (the growth
path stops being a label: the ack, the baseline owner, the keyframe, the degradation rule)
and its message set (`SnapshotAck`, `Keyframe`) and its snapshot header; ADR-005 §5
(`lastOrderSeqProcessed` leaves the world hash — §7, a replay-contract edit); ADR-014
Consequences (the relevance hook lands as a `Simulation` method — §4); ADR-016 §6 (the
per-grid snapshot becomes a per-*viewer* stream) §7 (the over-cap affordance joins the icon
ladder — §5)
**Feeds:** the delta/interest slice after U3c; T2 (which must not build a broadcast-shaped
sender); R19

## Context

ADR-004 §6 named the growth path — "delta + interest management is the designed growth
path, not larger datagrams" — reserved `baselineTick` for it, and stopped. The review found
that every operative mechanism behind that sentence was undecided (NET-1): no snapshot-ack
exists in the message set, so delta-against-acked-baseline has nothing to work from; no
keyframe path exists, while ADR-016 §7's view switch **is** a mid-session join; the relevance
hook is one sentence in ADR-014's consequences; and `lastOrderSeqProcessed` is world-global
and folded into the world hash, so making it per-client later edits the replay contract
rather than just the wire.

**The arithmetic is what makes this urgent, and it is not close.** The budget chain is
1,152 − 2 framing − 16 header − 224 reserved order area = 910 B, which after T2's
`statusBits` byte holds **43 records at 21 B**. The MVP's own content is 41 fleet + 1
station = **42**. Margin: one record, for the entire U/T roadmap. And the designed
over-cap behaviour is total — `WriteSnapshot` refuses, the whole grid's snapshot is dropped
for every viewer, and "clients will see nothing move". A second commander meeting the first
at the starter station is 83 records, 62 % over budget, and the failure is a session-killing
outage by construction (R19).

**ADR-019 §5d changed the problem before this ADR was written, and changed it for the
better.** The 1,152-byte cap is a property of the *client link*, not of the building: a
SimHost emits its grid to a session host over an internal link with no such constraint, and
the session host — which alone knows the client's view, selection, acked baseline and link —
is where culling, delta encoding and packing belong. So this is one mechanism in one place
that already has every input it needs, rather than relevance split across a tier that cannot
see the client.

**What this ADR is for.** Not to be built now: D4 schedules the slice after U3c. It exists so
that T2 and U3b do not build shapes it has to undo — chiefly the per-client sender (A13) —
and so the decisions with wire, hash and seam consequences are taken while they are cheap.

## Decision

### 1. One culling authority: the session role, and nowhere else

Interest and delta are **the session host's job** (ADR-019 §5d), for a reason worth stating
as a rule rather than a fact: *relevance is a property of a viewer, and the sim tier has no
viewers.* A SimHost knows a grid; a session host knows a **player** — which grid they are
watching, what they have selected, what their link is doing, and what they last acked.

Consequences that follow immediately, and that T2/U3b must respect before this slice exists:

- **`SnapshotSender` is per client from its first line** (ADR-018 A13). There is no
  "broadcast the bytes we already made" path to grow out of later; the object that serialises
  is the object that knows a client.
- **`StationRoster` is addressed per viewer** (ADR-017 §1's privacy rule). On a broadcast
  sender that is a silent information leak nothing tests, because nothing on the roadmap
  before U3c ever runs two clients.
- **The sim tier never learns what a client can see.** `World` gains nothing from this ADR.
  That keeps the replay domain exactly where ADR-005 put it, and it is why §7's one
  subtraction from the world hash is the only determinism cost here.

### 2. The ack, and who owns baselines

**2a. `SnapshotAck` is a datagram, C→S:** `{ u16 gridId, u32 tick }`. It rides the unreliable
channel on purpose — it is frequent, it is idempotent, and a lost ack costs one slightly
larger delta rather than a stall. The server takes the **highest** acked tick per (client,
grid) and ignores anything older; that makes reordering a non-event.

**2b. The baseline is what was *sent*, not what the world was.** This is the subtlety that
makes interest and delta safe together, and getting it wrong is the classic bug: under
culling the client's picture of a grid is a *subset*, so delta-encoding against the grid's
true state at tick *T* would describe changes the client never had a baseline for. The
session host therefore retains, per client and per viewed grid, a ring of the **views it
transmitted** — the record set and field values as sent — for the last `BASELINE_RING_TICKS`
(32 ticks, 1.6 s, comfortably past any plausible ack RTT). The delta is computed against the
retained view at the acked tick.

**2c. No ack in the ring ⇒ keyframe.** If the client's acked tick has fallen out of the ring
(or there is no ack at all — a join, a switch, a long stall), the session host sends a
keyframe (§3) and starts again. This is the only recovery path, and it is unconditional:
there is no partial-resync mode to get subtly wrong.

**2d. A tick is acked only when it is whole.** A tick's update may span several datagrams
(§3b). Each part is **applied on arrival** for freshness, but the client acks tick *T* only
once every part of *T* has arrived. Apply-for-freshness, ack-for-baseline: a lost part
degrades to staleness for the ships it carried — which is exactly ADR-002's existing posture
for a lost snapshot — while the baseline stays a thing both sides agree on.

### 3. The keyframe path, and the channel it needs

**3a. A view switch is a mid-session join** (NET-1), and so is a reconnect inside D5's grace
window. All three take the same path, which is the point: one mechanism, exercised on every
switch rather than only on the rare true join.

**3b. Steady-state deltas are datagrams, and may span several.** The per-tick update is
`[u16 type][DeltaHeader][records]`, where
`DeltaHeader{ u32 tick, u32 baselineTick, u16 gridId, u16 culledCount, u8 partIndex,
u8 partCount, u16 recordCount }`. Every part is independently applicable — it names its own
tick, grid and baseline — so there is no reassembly buffer and no fragmentation timeout.
`partCount` is what makes §2d's whole-tick rule checkable.

**3c. A keyframe is reliable, and takes a channel of its own** *(this amends ADR-003 §1,
which promised exactly one reliable ordered channel)*. `TransportChannel` gains **`Bulk`**: a
second reliable ordered stream, separate from `Control`.

The reason is head-of-line blocking, and it cuts both ways. ADR-004 rejected reliable
snapshots because a hitch would stall fresh state behind a resend; that argument still holds
for the per-tick path and is why §3b stays on datagrams. But a keyframe is not fresh state —
it is the *baseline* for all the fresh state after it, it must arrive intact, and at the D4
cap it is ~21 KB, which is not a datagram-shaped object. Putting it on `Control` would park
it in front of the player's orders. QUIC gives independent streams for nothing, so the
correct answer is a stream of its own, and the cost of deciding it now is one enumerator.

**3d. Between the keyframe and the first delta**, the client renders what it has and the
session host keeps sending datagram updates for the ships already in the keyframe's set. The
keyframe's tick becomes the baseline the moment its stream completes; the switch settles
inside ADR-016 §7's designed ~200 ms because the interpolation buffer refill (~100 ms) is
the same cost it already was.

### 4. The relevance hook: the game ranks, the engine truncates

ADR-014 named this hook and left its shape open. Its shape is the ADR-014 pattern applied
literally — **the game states policy, the engine applies mechanism** — because relevance is
game semantics (who owns what, what is hostile, what is selected) and budget is link
semantics (how many bytes fit right now).

So `Simulation` grows one method, and it returns a **priority-ordered list**, not a filtered
one:

```
struct InterestQuery
{
  PlayerId viewer;
  AnchorId grid;
  std::span<const ShipId> selection;
  float focusXMetres, focusYMetres;   // the camera's plane focus
  float viewHalfExtentMetres;         // what the zoom actually shows
};

/// Ranks the grid's entities for one viewer, most relevant first. Never
/// truncates: the caller knows the budget, the callee knows the game.
virtual void RankRelevance(const InterestQuery& _query, std::vector<ShipId>& _outRanked) = 0;
```

The engine truncates the ranked list to whatever this tick's budget affords. That split is
the whole design: a game rule change (a new hostility tier, a new "always show" case) is a
GameLogic edit that touches no engine code, and a budget change is an engine edit that
touches no game rule.

**The ranking GameLogic implements, in tiers:**

| Tier | What | Rule |
|---|---|---|
| 0 | The viewer's **owned** ships on this grid; anything **selected**; the grid's structures | **Never truncated** (§5) |
| 1 | Ships with a visible relationship to the viewer (allied, hostile) inside the camera's extent | Nearest to focus first |
| 2 | Everything else on the grid | Nearest to focus first, round-robin across ticks |

Structures are tier 0 because there are one or two of them and they are the grid's landmarks:
a station that flickered out of interest would take the player's sense of where they are with
it.

### 5. The guarantee, and the honesty when it binds

**5a. The guarantee (ADR-018 D4, UX-2):** *a commander's owned and selected ships are never
culled from the grid they are viewing.* This is not a quality-of-service nicety, it is a
correctness requirement with two independent causes:

- **Pre-check parity.** `ValidationView` is built from ids off the snapshot (ADR-004 §7). A
  culled owned ship is a ship the client cannot pre-check an order for, so the client would
  bounce — or worse, silently disagree with the server — on the player's own fleet. The
  entire BounceParity property rests on the client seeing what it commands.
- **The command grammar.** Selection, order footprints and the puck are drawn from replicated
  state; culling a selected ship makes the player's own selection blink.

**5b. Tier 0 can exceed one datagram, and that is fine.** At D15.6's envelope (~200 owned
ships) tier 0 alone is ~4.2 KB — four datagrams. The per-tick budget is therefore a
**bandwidth** figure, not the datagram size: `TICK_BUDGET_BYTES`, packed into as many
1,152-byte datagrams as it takes (§3b). The datagram cap never moves; what moves is how many
of them a tick may use. Sizing is a deployment number, not a compiled-in one, and the honest
starting point is the one already on the record: the D4 cap uncompressed is ~21.5 KB/tick
≈ 3.4 Mbps at 20 Hz, so a budget well under that is what interest exists to enforce.

**5c. When tier 0 alone exceeds the budget, the budget loses.** Owned ships are sent
regardless, and the overrun is **counted and logged** (`interestOverrun`, beside
`tickOverrun`). The alternative — culling a player's own fleet to hit a bandwidth number — is
the one outcome this ADR is written to prevent.

**5d. Culling is stated to the player, never silent.** `DeltaHeader.culledCount` is how many
entities on the viewed grid are not being sent. The client renders it through the icon
ladder's existing counted-chip rung (`tactical-icon-system.png` §5) — the same affordance
that already answers "there are more ships here than there are pixels". The player is never
told a grid is empty when it is not; they are told *how many* they are not being shown, which
is the honest version of the same sentence.

**5e. Leaving interest is an event, not an absence.** A record absent from a delta means
"unchanged". A record that has *left* the viewer's interest set is named in an explicit
`leftInterest` id list in the delta, so the client can retire it rather than leaving a ghost
hull frozen on the plane forever. The list is small and only appears on transitions.

### 6. Degradation: truncate by priority, never refuse

The rule that replaces "refuse the whole snapshot":

1. Fill the tick's budget from the ranked list, highest priority first.
2. What does not fit is **not sent this tick** and keeps its place for the next one —
   round-robin within tier 2, so a distant ship updates at a lower cadence rather than never.
   (ADR-004 §6 already recorded that cadence decimation is legal; this is what it looks like.)
3. **Nothing is ever refused wholesale.** A grid over budget produces a *partial* view with an
   honest `culledCount`, not silence.

`WriteSnapshot`'s current refuse-above-cap behaviour stays exactly as it is until this slice
lands — it is the loud failure T2's accept tests (ADR-018 A13) and the only correct behaviour
while the full-snapshot format is the only format. This ADR replaces it; it does not weaken
it in the meantime.

### 7. `lastOrderSeqProcessed` leaves the world hash

It is per-session state living in shared state: world-global, written as a max across all
submitters, and **folded into the world hash**. With one commander that is invisible. With
two it is wrong twice over — one player's order sequence perturbs the other's feedback loop,
and a replay's hash depends on which client happened to submit.

So it moves to the session, and out of `WorldHash`. Two things follow that must be said out
loud rather than discovered:

- **This is a replay-contract edit** (ADR-005 §5), not a wire edit. Every recorded replay
  golden re-baselines when it lands. That is cheap now and expensive after U2 fills the
  corpus with per-grid logs, which is the argument for taking the decision in this document
  even though the slice is later.
- The wire field stays where it is. `Snapshot.Header.lastOrderSeqProcessed` remains, and
  becomes **per-viewer** — which it always read as, and now is.

### 8. The record grows: u32 ids, and relationship rather than ownership

**8a. `EntityRecord.id` widens to u32 here** (ADR-018 D6). This is the cluster that removes
the full-fit constraint, which is precisely why D6 staged the wire half to wait for it: a
23-byte record fits 39 per datagram, under the 41-ship floor, so the widening was unsafe
while one datagram had to hold everything. Under §5b it no longer does.

**8b. Replicate the *relationship*, not the owner.** The obvious design — an owner id per
record — costs four bytes on every entity and answers a question the client never asks. What
the client needs is `tactical-icon-system.png` §3's colour channel: OWN / ALLIED / NEUTRAL /
HOSTILE. That is **viewer-relative**, and the session host is exactly the tier that knows the
viewer. So it is **two bits of the `statusBits` byte** ADR-017 §5 introduced with bit 0 used
and seven spare: zero extra bytes, and the icon system gets the data it was designed around.

The owner's *identity* — for a "whose is that?" inspection — is a separate low-rate query on
the control channel, not a per-tick per-entity field. Answering it at snapshot rate would be
paying 20 Hz for something a player asks once.

**8c. The sim-side ownership column is independent and can land any time** (SIM-7): an owner
array on `World`, its hash fold, and `ValidationView`'s owner field are cheap, unblock
`NotOwned` becoming reachable, and do not touch the wire.

### 9. What this deliberately does not do

- **No compression.** No zlib, no bit-packing beyond the field masks. The delta *is* the
  compression, and a second scheme on top would be a second thing to get wrong for a fraction
  of what culling already saves.
- **No client-side prediction of other commanders' ships.** ADR-004's "the only client-side
  optimism in the game is the order ghost" stands.
- **No change to fleet summaries.** ADR-016 §6's ~1 Hz per-fleet records are how the player
  sees everything they are *not* watching, and they are already cheap and already per-player.
  Interest is about the one grid in view.
- **No spatial index.** Ranking walks the grid's dense arrays; at the D4 cap that is a
  thousand distance comparisons per viewer per tick, which is noise beside the tick itself
  (ADR-018 A4). If it ever is not, the broadphase R10 defers is the same structure.
- **No per-player bandwidth negotiation.** One budget figure per deployment; adaptive rate
  control is a later decision with its own evidence.

## Alternatives rejected

- **Bigger datagrams / raising the 1,152 B cap.** ADR-003 chose it to sit safely under the
  QUIC datagram MTU after overheads; raising it trades a designed property for PMTU
  discovery bugs on other people's networks, and buys one linear factor against a problem
  that is 25× at the cap.
- **Delta without interest.** A delta of 1,024 moving ships is still ~1,024 records — every
  ship on a busy grid moves every tick, so field masks alone save the gauges and the class
  byte and little else. Interest is where the order of magnitude lives; delta is what makes
  the *quiet* ships free.
- **Interest without delta.** Workable, and briefly tempting because it needs no ack. But the
  culled set changes as the camera moves, so every pan re-sends full records for ships already
  on the client, and the guarantee's tier 0 — the ships that matter most — is exactly the set
  that never leaves interest and would therefore be re-sent at full width forever.
- **Keyframes fragmented over datagrams with an ack/resend scheme.** This is re-implementing a
  reliable stream beside a protocol that has one, which ADR-003 already rejected in its
  general form ("re-implements QUIC badly over time"). One enum value is cheaper than a
  reassembly buffer with a timeout policy.
- **Relevance computed in the SimHost tier.** It cannot see the viewer's camera, selection or
  link, so it would need all three shipped to it every tick — inverting ADR-019 §5d's data
  flow to no benefit.
- **An owner id per entity record.** Four bytes per ship per tick to answer a question asked
  once a session, when two bits of an existing byte answer the question actually being asked
  every frame (§8b).
- **Priority truncation inside `WriteSnapshot`.** Tempting because it is where the cap check
  already is — but `WriteSnapshot` is GameLogic, and truncation is a budget decision. Putting
  it there would give the game a link model and make the sim tier viewer-aware, which is the
  one thing §1 exists to prevent.

## Consequences

- **T2 and U3b are constrained now, before this slice exists**: the per-client sender (A13),
  the per-viewer roster, and the over-cap refusal test. None of them costs anything extra to
  build the right shape first; all of them are expensive to unpick.
- **One schema cluster** carries the wire half: `SnapshotAck`, the `Keyframe` message and the
  `Bulk` channel, `DeltaHeader`, `EntityRecord.id` → u32, and the relationship bits — riding
  the existing fail-closed hash, all at once, as ADR-018's Consequences require.
- **`Transport` grows a channel** (`Bulk`), which is an ADR-003 §1 amendment and a
  `QuicTransport` change: a second stream, opened by the client alongside stream 0.
- **ADR-005's replay contract changes once**, at §7, and every replay golden re-baselines with
  it.
- **R19 gets its mitigation**: the cliff's owner is this document and its trigger is U3c.
  Until the slice lands, bake and scenario invariants keep authored per-grid population under
  the cap and shared grids stay gated (ADR-018 D3).
- **NET-5's open half closes when this lands**: fan-out, datagram scheduling and client apply
  at 1,024 become testable for the first time, because a world past the cap can finally be
  serialised. R10's wire half should be scheduled with the slice, not after it.
- **The interest guarantee is a test, not a promise**: the slice's accept must include a
  culled-grid run in which every owned and selected ship is present in every tick's union of
  parts, and a `culledCount` the client actually renders.
