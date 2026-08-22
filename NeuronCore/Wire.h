#pragma once

#include "ByteReader.h"
#include "ByteWriter.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
 * Framing and the semantics-free messages (ADR-004 §2, §4).
 *
 * NeuronCore owns the envelope and the session handshake -- a message set that
 * would make sense in any networked simulation. Snapshots and orders are game
 * semantics and live in GameLogic, which is why nothing here mentions a ship.
 *
 * Framing is `[u16 type][payload]` per datagram; the transport already
 * delivers whole messages, so there is no length prefix to duplicate.
 */

namespace Neuron
{

/*
 * Breaking framing changes only (ADR-004 §5), and this is one.
 *
 * **2 as of the station phase's identity cluster** (ADR-018 D5/A12): `Hello`
 * and `Welcome` grew a `PlayerId` and a reserved resume token, so a build that
 * predates them reads past the end of a message it thinks it understands. The
 * schema hash does not cover this -- it covers *game payloads*, and `Hello` is
 * the message that carries the schema hash in the first place -- so the version
 * is the only thing that can refuse the connection, and it does.
 *
 * **3 as of the station phase's wire half:** `Welcome` grew `gridAnchor`
 * between `worldId` and the plane coordinates, which moves every field after it
 * -- a build reading a v2 `Welcome` as v3 would take two bytes of `anchorX` for
 * the anchor and then be shifted for the rest of the message. This is the
 * framing change the version exists for, and unlike the `Summary` type word it
 * really is one. The field is what lets a client *address* the grid it is on
 * rather than only describe it, which is what a Dock and a station command both
 * need (ADR-017 §2, §3).
 *
 * **4 as of U3d-b** (ADR-022). `Snapshot`'s payload stops being opaque game
 * bytes and becomes an engine-framed delta: a `DeltaHeader`, then records, then
 * an optional trailer. A build that predates this reads a `DeltaHeader` as the
 * game's old snapshot header and finds a plausible-looking tick with everything
 * after it shifted, which is precisely the failure the version exists to make
 * impossible. Both schema hashes move too -- the record's width changed and so
 * did the game's payload -- so this is belt and braces, and the belt is worth
 * having because the two hashes answer "do we agree about content" while this
 * answers "do we agree about the envelope".
 */
inline constexpr std::uint16_t PROTOCOL_VERSION = 4;

enum class WireType : std::uint16_t
{
  None = 0,
  Hello = 1,
  Welcome = 2,
  UpdateRequired = 3,
  Refuse = 4,
  Ping = 5,
  Pong = 6,
  Goodbye = 7,

  /*
   * A game state payload, framed by the engine and read only by the game
   * (ADR-004 §6, ADR-014 §5). There is no struct for it here on purpose: the
   * engine writes this type word, copies opaque bytes after it, and has no
   * opinion about what they mean. What is inside is GameLogic's schema and
   * travels under its own hash.
   */
  Snapshot = 8,

  /*
   * An order payload, framed by the engine and read only by the game
   * (ADR-004 §7). Opaque for the same reason `Snapshot` is: the engine carries
   * the bytes and has no opinion about what a move is. Reliable and ordered --
   * an order is the one thing in this protocol that must arrive (ADR-003).
   */
  OrderSubmit = 9,

  /*
   * What the authority decided, on the way back.
   *
   * This one *is* a struct, because every field is already a neutral number:
   * a sequence the client chose, an id the server assigned, and the two halves
   * of `OrderVerdict`. The reason code is the game's enum and the engine passes
   * it through unread, which is what lets a local refusal and a server refusal
   * say the same thing (ADR-014 §3).
   */
  OrderAck = 10,

  /*
   * A per-viewer game payload at the summary cadence (ADR-016 §6, ADR-018 A13).
   *
   * Opaque for the same reason `Snapshot` is, and **one type for the whole
   * family** rather than one per message kind: a roster, a fleet summary and
   * whatever ADR-016 §6 adds next are all "what this viewer is owed at about
   * 1 Hz", and which of them a payload carries is a distinction the game draws
   * inside its own bytes under its own hash. An enumerator per kind would spend
   * a slot of this enum on every game concept that ever wants a slow feed,
   * which is exactly the coupling ADR-014 §5 keeps out of NeuronCore.
   *
   * Adding it did **not** bump `PROTOCOL_VERSION`, and the reason is worth
   * stating because it is the first question a reader has: the version covers
   * breaking *framing* changes, and no existing message's layout moved. A build
   * that predates this type ignores it (the client's dispatch has always had a
   * `default`), and what actually fails a mismatch closed is the *game* schema
   * hash -- the frame's format is `GAME_SCHEMA_TEXT`'s, not this library's.
   */
  Summary = 11,

  /*
   * "Show me that grid" (ADR-016 §4, §7 — U3b), client to server.
   *
   * Reliable and ordered, because a view switch is a thing the player did once
   * and must not lose: a dropped request leaves them watching the world they
   * asked to leave, with nothing on screen saying why.
   *
   * The engine carries a **world id** and no more, which is the same neutral
   * thing `Welcome` already carries — "which world", never "which solar
   * system" (Dependency Map ruling 4). Whether the viewer is *allowed* to see
   * it is game policy and crosses the seam as a question, not as a rule this
   * library knows.
   */
  ViewRequest = 12,

  /*
   * The answer, server to client. Sent for every request, accepted or not, and
   * unprompted when the server moves a viewer itself — a grid torn down under
   * them, or a fleet arriving somewhere they were following.
   *
   * `reasonCode` is the game's enum, passed through unread exactly as
   * `OrderAck`'s is (ADR-014 §3): a refusal the player reads has to say the
   * same words whichever half refused it.
   */
  ViewChanged = 13,

  /*
   * "I have the whole of tick T for grid G" (ADR-022 §2a), client to server.
   *
   * On the **unreliable** channel on purpose: it is frequent, it is idempotent,
   * and the server takes the highest acked tick per (client, grid) and ignores
   * anything older -- so reordering is a non-event and a lost ack costs one
   * slightly larger delta rather than a stall.
   */
  SnapshotAck = 14,

  /*
   * The baseline everything after it is a delta against (ADR-022 §3), server to
   * client, on the `Bulk` channel.
   *
   * A view switch is a mid-session join, and so is a reconnect inside D5's
   * grace window and a client whose acked tick has fallen out of the server's
   * ring. All of them take this path, which is the point: one mechanism,
   * exercised on every switch rather than only on the rare true join.
   */
  Keyframe = 15,

  /*
   * Where the viewer is looking and what they have selected (ADR-022 §4),
   * client to server, unreliable.
   *
   * **ADR-016 §7 said the server has no business holding this**, and ADR-022 §1
   * is what changed that: *relevance is a property of a viewer*, and §4's
   * `InterestQuery` is a camera focus, a zoom extent and a selection. The
   * guarantee in §5a -- that a commander's owned **and selected** ships are
   * never culled -- cannot be kept by a server that does not know the
   * selection, so this is the message that makes the guarantee reachable rather
   * than aspirational.
   *
   * Unreliable and sent on change: a lost one costs one tick ranked against a
   * slightly stale camera, which is a worse ordering and never a wrong one.
   */
  ViewFocus = 16
};

/// Why a server turned a client away. On the wire, so the values are fixed.
enum class RefuseReason : std::uint16_t
{
  None = 0,
  ServerFull = 1,
  ProtocolMismatch = 2,
  Shutdown = 3
};

/// Client's opening message. The hashes are what make a mismatched build fail
/// at the door rather than halfway through a session (ADR-004 §3).
/*
 * Durable identity, distinct from the connection (ADR-018 D5).
 *
 * `clientId` names a *connection*: it is minted when one opens and it is gone
 * when the socket is. `PlayerId` names whoever is on the other end, across
 * disconnects and across sessions. The two are one number apart today -- there
 * is one player and accounts do not exist -- and the whole point of minting the
 * distinction now is that everything player-keyed (presence, view rights,
 * rosters, fleet summaries, order and transfer logs) can key on the durable one
 * from its first line, instead of keying on a connection and being rewritten
 * the day one drops.
 *
 * Zero is "no player", so a zeroed handshake is detectably anonymous rather
 * than accidentally player one.
 */
using PlayerId = std::uint32_t;
inline constexpr PlayerId INVALID_PLAYER_ID = 0;

/*
 * The one player there is (ADR-018 D5).
 *
 * A named constant rather than a literal `1` at the two places that use it,
 * because the day accounts arrive this is the symbol whose definition changes
 * and the call sites that must be found.
 */
inline constexpr PlayerId SOLE_PLAYER_ID = 1;

/// How long a session outlives its transport (ADR-018 D5). Reserved: the field
/// that carries a resume token exists, nothing mints one yet, and this is the
/// window the reconnect print promises when something does.
inline constexpr std::uint32_t SESSION_GRACE_SECONDS = 120;

struct Hello
{
  std::uint16_t protocolVersion = PROTOCOL_VERSION;
  std::uint64_t schemaHash = 0;
  std::uint64_t contentHash = 0;
  std::string playerName;

  /// Who is connecting, if they already know (ADR-018 D5). `INVALID_PLAYER_ID`
  /// on a first connection; the id the last `Welcome` carried on a resume.
  PlayerId playerId = INVALID_PLAYER_ID;

  /*
   * The resume token. **Reserved and always zero** -- there is nothing to
   * authenticate against and inventing a token now would be inventing a
   * security model to go with it.
   *
   * It is in the message anyway because the alternative is a schema bump on the
   * day sessions first survive a disconnect, and this cluster is the one the
   * design already chose to spend (ADR-017 §8, widened by D5/A12). A field that
   * ships as zero costs eight bytes on one message per connection.
   */
  std::uint64_t resumeToken = 0;
};

/*
 * The server's answer, and the first thing that tells a client where it is.
 *
 * `worldId` and the anchor are ADR-009 §8's `worldMeta`, named in engine terms
 * on purpose: NeuronCore must stay plausible in an unrelated networked sim
 * (Dependency Map ruling 4), so it carries "which world, and where is its
 * origin" rather than "which solar system". The game's meaning of those numbers
 * is GameLogic's, and the engine never reads them.
 *
 * They matter the moment the client is a separate process: `mode: "client"`
 * connects to a server it shares no configuration with, and without an anchor
 * it cannot place a single replicated position (ADR-009 §2).
 *
 * The universe hash is *not* repeated here. It already travels as
 * `contentHash`, which is the field the handshake fails closed on; carrying it
 * twice would create two values that can disagree, and the one that refuses the
 * connection would win silently.
 */
struct Welcome
{
  std::uint32_t clientId = 0;
  std::uint32_t tick = 0;
  std::uint16_t tickRate = 0;
  std::uint64_t schemaHash = 0;
  std::uint64_t contentHash = 0;
  std::uint16_t worldId = 0;

  /// Which anchor this grid stands on (ADR-016 §3), so the client can name it
  /// back in a Dock or a station command. `worldId` says where in the universe;
  /// this says which grid.
  std::uint16_t gridAnchor = 0;

  std::int64_t anchorX = 0;
  std::int64_t anchorY = 0;

  /*
   * The world's display strings, from the simulation's `WorldMeta` and carried
   * exactly as unread as `worldId` is: a name for the world, a secondary line,
   * and a short status badge. The HUD's top bar draws them verbatim
   * (`tactical-hud.png`'s `VESTA-3 / FRONTIER 0.4 / SEC 0.4`), and the words
   * are the game's -- the engine ships them the way it ships `playerName`,
   * which is why they are plain strings rather than anything with structure.
   */
  std::string worldName;
  std::string worldDetail;
  std::string worldBadge;

  /// Who the server decided this is (ADR-018 D5). The client keeps it and
  /// offers it back on a resume; everything player-keyed is keyed on this and
  /// never on `clientId`, which is the connection and not the person.
  ///
  /// Last, after the variable-length strings, for the same reason `Hello` puts
  /// its pair last: the fields the handshake fails closed on keep fixed offsets
  /// from the front of the message.
  PlayerId playerId = INVALID_PLAYER_ID;

  /// The token to offer back. Reserved, always zero, and paired with `Hello`'s
  /// for the same reason.
  std::uint64_t resumeToken = 0;
};

/*
 * Which grid a client is asking to watch.
 *
 * One field, and it stays one: everything else about a view — where the camera
 * is, what is selected, how far it is zoomed — is client state the server has
 * no business holding (ADR-016 §7). What the server needs is the answer to
 * "which world do I serialise for this viewer", and that is this number.
 */
struct ViewRequest
{
  std::uint16_t gridAnchor = 0;
};

/*
 * What the server did about it.
 *
 * `accepted` and a `reasonCode` rather than a bare bool, for `OrderAck`'s
 * reason: the player is owed the *same* refusal wording from the client's
 * pre-check and from the authority, and a bool cannot carry one. Zero is
 * accepted, and the non-zero values are the game's to define.
 *
 * The grid is echoed even on a refusal, so a client that had two requests in
 * flight can tell which one this answers.
 */
struct ViewChanged
{
  std::uint16_t gridAnchor = 0;
  std::uint16_t reasonCode = 0;
  bool accepted = false;
};

/*
 * How far back the session host keeps the views it sent (ADR-022 §2b).
 *
 * 32 ticks is 1.6 s at ADR-002's 20 Hz -- comfortably past any plausible ack
 * round trip, and bounded so a stalled client cannot make the server retain its
 * picture forever. A client whose acked tick has fallen out of this window gets
 * a keyframe (§2c), unconditionally: there is no partial-resync mode to get
 * subtly wrong.
 */
inline constexpr std::uint32_t BASELINE_RING_TICKS = 32;

/*
 * "I have the whole of tick T for grid G" (ADR-022 §2a).
 *
 * A tick is acked only when it is **whole** (§2d). Each part of a multi-part
 * delta is applied on arrival for freshness, but the ack waits for every part,
 * because the ack is what makes a baseline something both sides agree on and
 * half a tick is not a baseline. Apply-for-freshness, ack-for-baseline: a lost
 * part degrades to staleness for the entities it carried, which is exactly
 * ADR-002's existing posture for a lost snapshot.
 */
struct SnapshotAck
{
  std::uint16_t gridId = 0;
  std::uint32_t tick = 0;
};

/*
 * The header on every part of a per-tick update (ADR-022 §3b).
 *
 * **Every part is independently applicable** -- it names its own tick, grid and
 * baseline -- so there is no reassembly buffer and no fragmentation timeout,
 * which is the whole reason the fields are repeated rather than sent once.
 * `partCount` is what makes §2d's whole-tick rule checkable by the receiver.
 *
 * `culledCount` is §5d: how many entities on this grid the client is **not**
 * being shown. It exists so culling is stated rather than silent -- the player
 * is never told a grid is empty when it is not; they are told how many they are
 * not seeing, which is the honest version of the same sentence.
 */
struct DeltaHeader
{
  std::uint32_t tick = 0;

  /// The tick whose *sent view* this delta is encoded against (§2b). Not the
  /// world at that tick: under culling the client's picture is a subset, so
  /// delta-encoding against the grid's true state would describe changes the
  /// client never had a baseline for.
  std::uint32_t baselineTick = 0;

  std::uint16_t gridId = 0;
  std::uint16_t culledCount = 0;
  std::uint8_t partIndex = 0;
  std::uint8_t partCount = 1;
  std::uint16_t recordCount = 0;
};

inline constexpr std::size_t DELTA_HEADER_BYTES = 16;

/*
 * The header on a keyframe (ADR-022 §3).
 *
 * No parts and no baseline: it rides the reliable `Bulk` stream, so it arrives
 * whole or the connection is gone, and it *is* the baseline. `recordCount` is
 * u32 rather than u16 because a keyframe is not datagram-shaped and there is no
 * reason to build in a ceiling the channel does not have.
 */
struct KeyframeHeader
{
  std::uint32_t tick = 0;
  std::uint16_t gridId = 0;
  std::uint16_t culledCount = 0;
  std::uint32_t recordCount = 0;
};

inline constexpr std::size_t KEYFRAME_HEADER_BYTES = 12;

/*
 * How many selected entities one `ViewFocus` may name.
 *
 * A cap rather than "whatever fits", because this message is read from a
 * hostile client and an unbounded count is an allocation an attacker chooses.
 * 256 is past any selection a person makes with a gesture and still leaves the
 * message inside one datagram: 12 bytes of header plus 256 ids at four bytes is
 * 1,036, under `MAX_DATAGRAM_BYTES`.
 */
inline constexpr std::uint16_t MAX_VIEW_SELECTION = 256;

/*
 * The viewer's camera and selection (ADR-022 §4).
 *
 * Centimetres, like every other length on this wire (ADR-004): the client and
 * the server must agree about a position to the same precision whether they are
 * validating an order or ranking relevance, and a second unit here would be a
 * second rounding boundary.
 *
 * Engine-neutral, like everything else in this file: a point on a plane, how
 * much of it is visible, and a set of entity ids. That the entities are ships
 * and the plane is a tactical grid is the game's business (ADR-014).
 */
struct ViewFocus
{
  std::uint16_t gridId = 0;
  std::int32_t focusXCm = 0;
  std::int32_t focusYCm = 0;

  /// What the zoom shows, as a half-extent. Zero is a camera that has not been
  /// described yet, which ranks everything unselected into the far tier rather
  /// than being an error.
  std::uint32_t halfExtentCm = 0;

  std::vector<std::uint32_t> selection;
};

struct UpdateRequired
{
  std::uint64_t serverSchemaHash = 0;
  std::uint64_t serverContentHash = 0;
};

struct Refuse
{
  RefuseReason reason = RefuseReason::None;
};

/// Timestamps are the client's own counter, echoed back untouched: the client
/// measures the round trip without the two machines agreeing on a clock.
struct Ping
{
  std::uint64_t clientSendMicroseconds = 0;
};

struct Pong
{
  std::uint64_t clientSendMicroseconds = 0;
  std::uint32_t serverTick = 0;
};

/*
 * The verdict on one submitted order (ADR-004 §7).
 *
 * `accepted` is a byte rather than a `bool` because a wire field needs a width
 * the standard guarantees. `serverOrderId` is zero on a refusal -- nothing was
 * given an id -- and the client's ghost is matched by `orderSeq`, which is the
 * only field it chose itself and therefore the only one it can match on before
 * the ack arrives.
 */
struct OrderAck
{
  std::uint32_t orderSeq = 0;
  std::uint32_t serverOrderId = 0;
  std::uint16_t reasonCode = 0;
  std::uint8_t accepted = 0;
};

struct Goodbye
{
  std::uint16_t reason = 0;
};

/// Writes the type word. Every message starts with one.
void WriteWireType(ByteWriter& _writer, WireType _type) noexcept;

/// Reads the type word. Returns WireType::None if the buffer is too short.
[[nodiscard]] WireType ReadWireType(ByteReader& _reader) noexcept;

void Write(ByteWriter& _writer, const Hello& _message) noexcept;
void Write(ByteWriter& _writer, const Welcome& _message) noexcept;
void Write(ByteWriter& _writer, const UpdateRequired& _message) noexcept;
void Write(ByteWriter& _writer, const Refuse& _message) noexcept;
void Write(ByteWriter& _writer, const Ping& _message) noexcept;
void Write(ByteWriter& _writer, const Pong& _message) noexcept;
void Write(ByteWriter& _writer, const OrderAck& _message) noexcept;
void Write(ByteWriter& _writer, const Goodbye& _message) noexcept;
void Write(ByteWriter& _writer, const ViewRequest& _message) noexcept;
void Write(ByteWriter& _writer, const ViewChanged& _message) noexcept;
void Write(ByteWriter& _writer, const SnapshotAck& _message) noexcept;
void Write(ByteWriter& _writer, const DeltaHeader& _message) noexcept;
void Write(ByteWriter& _writer, const KeyframeHeader& _message) noexcept;
void Write(ByteWriter& _writer, const ViewFocus& _message) noexcept;

[[nodiscard]] bool Read(ByteReader& _reader, Hello& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Welcome& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, UpdateRequired& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Refuse& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Ping& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Pong& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, OrderAck& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Goodbye& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, ViewRequest& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, ViewChanged& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, SnapshotAck& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, DeltaHeader& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, KeyframeHeader& _outMessage) noexcept;

/// Refuses a selection past `MAX_VIEW_SELECTION` rather than truncating it: a
/// truncated selection is a *different* selection, and the guarantee in
/// ADR-022 §5a would then be kept for the wrong set of ships.
[[nodiscard]] bool Read(ByteReader& _reader, ViewFocus& _outMessage);

/*
 * The schema hash covers this file's message layout. Any field added, removed
 * or retyped must change the string beside it, or two builds will disagree
 * silently instead of refusing each other at the handshake.
 */
inline constexpr std::string_view CORE_SCHEMA_TEXT = "Hello{u16 protocolVersion,u64 schemaHash,u64 contentHash,str playerName,"
                                                     "u32 playerId,u64 resumeToken}"
                                                     "Welcome{u32 clientId,u32 tick,u16 tickRate,u64 schemaHash,u64 contentHash,"
                                                     "u16 worldId,u16 gridAnchor,i64 anchorX,i64 anchorY,"
                                                     "str worldName,str worldDetail,str worldBadge,"
                                                     "u32 playerId,u64 resumeToken}"
                                                     "UpdateRequired{u64 serverSchemaHash,u64 serverContentHash}"
                                                     "Refuse{u16 reason}"
                                                     "Ping{u64 clientSendMicroseconds}"
                                                     "Pong{u64 clientSendMicroseconds,u32 serverTick}"
                                                     "Goodbye{u16 reason}"
                                                     // Type word only: the payload is the game's, under the
                                                     // game's own schema hash. The value is still part of this
                                                     // contract, because two builds disagreeing about which
                                                     // number means "snapshot" is a wire break.
                                                     "Snapshot{u16 type,opaque payload}"
                                                     // Same argument as Snapshot: the submitted payload is the
                                                     // game's, and only the type word is this contract's. The ack
                                                     // is fully described here because every field of it is a
                                                     // number this library defines the meaning of.
                                                     "OrderSubmit{u16 type,opaque payload}"
                                                     "OrderAck{u32 orderSeq,u32 serverOrderId,u16 reasonCode,u8 accepted}"
                                                     // Type word only, again: one type for ADR-016 §6's whole
                                                     // summary family, and which member a payload carries is a
                                                     // byte inside the game's own schema.
                                                     "Summary{u16 type,opaque payload}"
                                                     // Fully described here: a world id and a verdict are numbers
                                                     // this library defines the shape of, even though what makes a
                                                     // view legal is the game's (ADR-016 §7).
                                                     "ViewRequest{u16 gridAnchor}"
                                                     "ViewChanged{u16 gridAnchor,u16 reasonCode,u8 accepted}"
                                                     // ADR-022's cluster. Unlike `Snapshot`, these are not type
                                                     // words over opaque bytes: the delta header, the entity
                                                     // records and the keyframe are the *engine's* format now,
                                                     // because interest and delta are the session host's job
                                                     // (§1) and the record is this library's type (ADR-014 §4).
                                                     // So the layout is described here in full and a mismatched
                                                     // build fails at the handshake rather than misreading a
                                                     // header.
                                                     "SnapshotAck{u16 gridId,u32 tick}"
                                                     "DeltaHeader{u32 tick,u32 baselineTick,u16 gridId,u16 culledCount,"
                                                     "u8 partIndex,u8 partCount,u16 recordCount}"
                                                     "KeyframeHeader{u32 tick,u16 gridId,u16 culledCount,u32 recordCount}"
                                                     "ViewFocus{u16 gridId,i32 focusXCm,i32 focusYCm,u32 halfExtentCm,"
                                                     "u16 selectionCount,u32 selection[]}"
                                                     // The record itself, spelled here because its width is now
                                                     // part of this contract rather than of the game's: u32 ids
                                                     // as of ADR-022 §8a, and two of `statusBits` carrying the
                                                     // viewer-relative relationship (§8b).
                                                     "EntityRecord{u32 id,u8 typeId,u8 groupId,i32 posXCm,i32 posYCm,"
                                                     "i16 velXCmPerSec,i16 velYCmPerSec,u16 headingTurns16,"
                                                     "u8 gaugeA,u8 gaugeB,u8 statusBits}";

[[nodiscard]] std::uint64_t CoreSchemaHash() noexcept;

} // namespace Neuron
