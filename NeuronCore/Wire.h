#pragma once

#include "ByteReader.h"
#include "ByteWriter.h"

#include <cstdint>
#include <string>
#include <string_view>

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
 */
inline constexpr std::uint16_t PROTOCOL_VERSION = 3;

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
  ViewChanged = 13
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
                                                     "ViewChanged{u16 gridAnchor,u16 reasonCode,u8 accepted}";

[[nodiscard]] std::uint64_t CoreSchemaHash() noexcept;

} // namespace Neuron
