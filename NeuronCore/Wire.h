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

inline constexpr std::uint16_t PROTOCOL_VERSION = 1;

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
  OrderAck = 10
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
struct Hello
{
  std::uint16_t protocolVersion = PROTOCOL_VERSION;
  std::uint64_t schemaHash = 0;
  std::uint64_t contentHash = 0;
  std::string playerName;
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
  std::int64_t anchorX = 0;
  std::int64_t anchorY = 0;
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

[[nodiscard]] bool Read(ByteReader& _reader, Hello& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Welcome& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, UpdateRequired& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Refuse& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Ping& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Pong& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, OrderAck& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Goodbye& _outMessage) noexcept;

/*
 * The schema hash covers this file's message layout. Any field added, removed
 * or retyped must change the string beside it, or two builds will disagree
 * silently instead of refusing each other at the handshake.
 */
inline constexpr std::string_view CORE_SCHEMA_TEXT = "Hello{u16 protocolVersion,u64 schemaHash,u64 contentHash,str playerName}"
                                                     "Welcome{u32 clientId,u32 tick,u16 tickRate,u64 schemaHash,u64 contentHash,"
                                                     "u16 worldId,i64 anchorX,i64 anchorY}"
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
                                                     "OrderAck{u32 orderSeq,u32 serverOrderId,u16 reasonCode,u8 accepted}";

[[nodiscard]] std::uint64_t CoreSchemaHash() noexcept;

} // namespace Neuron
