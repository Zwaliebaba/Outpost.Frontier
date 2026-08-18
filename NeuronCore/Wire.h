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

inline constexpr std::uint16_t ProtocolVersion = 1;

enum class WireType : std::uint16_t
{
  None = 0,
  Hello = 1,
  Welcome = 2,
  UpdateRequired = 3,
  Refuse = 4,
  Ping = 5,
  Pong = 6,
  Goodbye = 7
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
  std::uint16_t protocolVersion = ProtocolVersion;
  std::uint64_t schemaHash = 0;
  std::uint64_t contentHash = 0;
  std::string playerName;
};

struct Welcome
{
  std::uint32_t clientId = 0;
  std::uint32_t tick = 0;
  std::uint16_t tickRate = 0;
  std::uint64_t schemaHash = 0;
  std::uint64_t contentHash = 0;
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
void Write(ByteWriter& _writer, const Goodbye& _message) noexcept;

[[nodiscard]] bool Read(ByteReader& _reader, Hello& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Welcome& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, UpdateRequired& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Refuse& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Ping& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Pong& _outMessage) noexcept;
[[nodiscard]] bool Read(ByteReader& _reader, Goodbye& _outMessage) noexcept;

/*
 * The schema hash covers this file's message layout. Any field added, removed
 * or retyped must change the string beside it, or two builds will disagree
 * silently instead of refusing each other at the handshake.
 */
inline constexpr std::string_view CoreSchemaText =
  "Hello{u16 protocolVersion,u64 schemaHash,u64 contentHash,str playerName}"
  "Welcome{u32 clientId,u32 tick,u16 tickRate,u64 schemaHash,u64 contentHash}"
  "UpdateRequired{u64 serverSchemaHash,u64 serverContentHash}"
  "Refuse{u16 reason}"
  "Ping{u64 clientSendMicroseconds}"
  "Pong{u64 clientSendMicroseconds,u32 serverTick}"
  "Goodbye{u16 reason}";

[[nodiscard]] std::uint64_t CoreSchemaHash() noexcept;

} // namespace Neuron
