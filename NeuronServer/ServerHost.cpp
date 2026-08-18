#include "pch.h"

#include "ServerHost.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "Log.h"
#include "Telemetry.h"
#include "UdpTransport.h"
#include "Wire.h"

#include <array>

namespace Neuron
{
namespace
{
/// ADR-002 §1: fixed 20 Hz. Deliberately a constant rather than configuration.
constexpr std::uint32_t TICK_RATE = 20;
constexpr double TICK_INTERVAL_MS = 1000.0 / TICK_RATE;

/// Past this much debt the loop stops trying to catch up and jumps to now
/// (ADR-002 §2). Chasing a 250 ms hole one tick at a time is how a stalled
/// server becomes a permanently late one.
constexpr double MAX_TICK_DEBT_MS = 250.0;
constexpr std::uint32_t MAX_CATCH_UP_TICKS = 2;

/// How often each session's NET line goes to the log. The client logs the same
/// cadence from its own side, so a disagreement between the two is itself the
/// diagnosis (S4).
constexpr double STATS_INTERVAL_MS = 5000.0;
} // namespace

ServerHost::~ServerHost()
{
  Stop();
  Join();
}

bool ServerHost::Start(const ServerConfig& _config, Simulation& _simulation)
{
  if (m_running.load(std::memory_order_acquire))
    return false;

  m_config = _config;
  m_simulation = &_simulation;
  m_sessions.clear();
  m_stopRequested.store(false, std::memory_order_release);

  // Only UDP exists today; QuicTransport slots in here without touching
  // anything above (ADR-003 §3).
  auto transport = std::make_unique<UdpTransport>();
  if (!transport->Listen(_config.port))
  {
    NEURON_LOG_ERROR("server could not listen on port %u", static_cast<unsigned>(_config.port));
    return false;
  }
  m_boundPort = transport->BoundPort();
  m_transport = std::move(transport);

  // Listening before the thread starts, so a caller may connect the moment
  // Start returns (ADR-008 §1).
  m_running.store(true, std::memory_order_release);
  m_thread = std::thread(&ServerHost::SimThread, this);

  NEURON_LOG_INFO("server host started on port %u at %u Hz", static_cast<unsigned>(m_boundPort), TICK_RATE);
  return true;
}

void ServerHost::Stop()
{
  m_stopRequested.store(true, std::memory_order_release);
}

void ServerHost::Join()
{
  if (m_thread.joinable())
    m_thread.join();
  m_running.store(false, std::memory_order_release);
}

SessionInfo* ServerHost::FindSession(ConnectionId _connection)
{
  for (SessionInfo& session : m_sessions)
  {
    if (session.connection == _connection)
      return &session;
  }
  return nullptr;
}

void ServerHost::BroadcastSnapshot(std::uint32_t _tick)
{
  if (m_sessions.empty())
  {
    return; // Nobody to tell. Serialising for an empty room is pure waste.
  }

  NEURON_SPAN("Snapshot");

  std::array<std::uint8_t, MAX_DATAGRAM_BYTES> buffer{};
  ByteWriter writer{buffer};
  WriteWireType(writer, WireType::Snapshot);

  if (!m_simulation->WriteSnapshot(_tick, writer) || !writer.Ok())
  {
    // The simulation refused -- almost certainly because the fleet outgrew one
    // datagram, which is the point at which ADR-004 §6's growth path (deltas
    // plus interest management) stops being optional. Loud and counted rather
    // than a silently missing tick.
    m_snapshotFailures.fetch_add(1, std::memory_order_relaxed);
    NEURON_COUNTER("SnapshotDropped", 1);
    if (m_snapshotFailures.load(std::memory_order_relaxed) == 1)
    {
      NEURON_LOG_ERROR("the simulation could not fit a snapshot in one datagram; clients will see nothing move");
    }
    return;
  }

  // Unreliable and unordered on purpose: full snapshots are idempotent, so a
  // lost one costs a tick of freshness and a resent one would arrive after the
  // snapshot that superseded it (ADR-004 §6).
  for (const SessionInfo& session : m_sessions)
  {
    SendTo(session.connection, TransportChannel::State, writer);
  }
  NEURON_COUNTER("SnapshotsSent", static_cast<std::int64_t>(m_sessions.size()));
}

void ServerHost::SendTo(ConnectionId _connection, TransportChannel _channel, const ByteWriter& _writer)
{
  if (!_writer.Ok())
  {
    NEURON_LOG_ERROR("refusing to send a message that overflowed its buffer");
    return;
  }
  (void)m_transport->Send(_connection, _channel, _writer.Written());
}

void ServerHost::HandleMessage(const TransportEvent& _event)
{
  ByteReader reader{_event.payload};
  const WireType type = ReadWireType(reader);

  switch (type)
  {
  case WireType::Hello:
  {
    Hello hello;
    if (!Read(reader, hello))
    {
      NEURON_LOG_WARNING("malformed Hello from connection %u", _event.connection);
      m_transport->Close(_event.connection, DisconnectReason::ProtocolError);
      return;
    }

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};

    // Fail closed on a mismatch (ADR-004 §3). Better a clear refusal at the
    // door than a session that disagrees about what the bytes mean.
    if (hello.protocolVersion != PROTOCOL_VERSION || hello.schemaHash != m_simulation->SchemaHash() ||
        hello.contentHash != m_simulation->ContentHash())
    {
      NEURON_LOG_WARNING("refusing connection %u: schema or content mismatch", _event.connection);
      WriteWireType(writer, WireType::UpdateRequired);
      Write(writer, UpdateRequired{m_simulation->SchemaHash(), m_simulation->ContentHash()});
      SendTo(_event.connection, TransportChannel::Control, writer);
      m_transport->Close(_event.connection, DisconnectReason::Refused);
      return;
    }

    if (m_sessions.size() >= m_config.maxSessions)
    {
      WriteWireType(writer, WireType::Refuse);
      Write(writer, Refuse{RefuseReason::ServerFull});
      SendTo(_event.connection, TransportChannel::Control, writer);
      m_transport->Close(_event.connection, DisconnectReason::Refused);
      return;
    }

    SessionInfo session;
    session.clientId = m_nextClientId++;
    session.connection = _event.connection;
    session.handshakeComplete = true;
    m_sessions.push_back(session);
    m_sessionCount.store(static_cast<std::uint32_t>(m_sessions.size()), std::memory_order_relaxed);

    Welcome welcome;
    welcome.clientId = session.clientId;
    welcome.tick = m_tick.load(std::memory_order_relaxed);
    welcome.tickRate = static_cast<std::uint16_t>(TICK_RATE);
    welcome.schemaHash = m_simulation->SchemaHash();
    welcome.contentHash = m_simulation->ContentHash();

    // Where the world is, so a client in another process can place a position
    // before any snapshot arrives (ADR-009 §8).
    const WorldMeta world = m_simulation->World();
    welcome.worldId = world.worldId;
    welcome.anchorX = world.anchorX;
    welcome.anchorY = world.anchorY;

    WriteWireType(writer, WireType::Welcome);
    Write(writer, welcome);
    SendTo(_event.connection, TransportChannel::Control, writer);

    NEURON_LOG_INFO("client %u joined as '%s'", session.clientId, hello.playerName.empty() ? "(unnamed)" : hello.playerName.c_str());
    return;
  }

  case WireType::Ping:
  {
    Ping ping;
    if (!Read(reader, ping))
      return;
    // Echoed untouched: the client measures its own round trip without the
    // two machines agreeing on a clock.
    std::array<std::uint8_t, 64> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Pong);
    Write(writer, Pong{ping.clientSendMicroseconds, m_tick.load(std::memory_order_relaxed)});
    SendTo(_event.connection, TransportChannel::State, writer);
    return;
  }

  case WireType::OrderSubmit:
  {
    /*
     * The engine's whole part in an order: find the session, hand the rest of
     * the bytes to the game, and send back what it decided (ADR-004 §7).
     *
     * It never parses the payload. `Remaining()` is the bytes after the type
     * word, and what they mean is GameLogic's schema under GameLogic's hash --
     * the same arrangement `Snapshot` has in the other direction (ADR-014 §5).
     */
    const SessionInfo* session = FindSession(_event.connection);
    if (session == nullptr || !session->handshakeComplete)
    {
      // Before the handshake there is no client id to attribute an order to,
      // and no agreement that the two builds share a schema. Dropped rather
      // than acked: acking would imply the order was considered.
      NEURON_LOG_WARNING("order from connection %u before it joined", _event.connection);
      return;
    }

    const OrderVerdict verdict = m_simulation->ApplyOrderBytes(session->clientId, reader.Remaining());

    OrderAck ack;
    ack.orderSeq = verdict.orderSeq; // The game's echo: the engine never read it.
    ack.serverOrderId = verdict.serverOrderId;
    ack.reasonCode = verdict.reasonCode;
    ack.accepted = verdict.accepted ? std::uint8_t{1} : std::uint8_t{0};

    // Control channel, because an ack is exactly the kind of message ADR-003
    // put a reliable ordered channel there for: a lost refusal leaves a ghost
    // on screen forever, and there is no later snapshot that corrects it.
    std::array<std::uint8_t, 64> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::OrderAck);
    Write(writer, ack);
    SendTo(_event.connection, TransportChannel::Control, writer);

    if (!verdict.accepted)
    {
      m_ordersRefused.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }

  case WireType::Goodbye:
  {
    NEURON_LOG_INFO("connection %u said goodbye", _event.connection);
    m_transport->Close(_event.connection, DisconnectReason::ClosedByPeer);
    return;
  }

  default:
    NEURON_LOG_WARNING("unexpected message type %u from connection %u", static_cast<unsigned>(type), _event.connection);
  }
}

void ServerHost::LogNetStats()
{
  m_lastStatsCounter = Clock::Counter();
  for (const SessionInfo& session : m_sessions)
  {
    const TransportStats stats = m_transport->Stats(session.connection);
    NEURON_LOG_INFO("net(session %u): rtt %.2f ms, resends %llu, dropped %llu, dgrams %llu/%llu, bytes %llu/%llu", session.clientId,
                    stats.roundTripMs, static_cast<unsigned long long>(stats.controlResends),
                    static_cast<unsigned long long>(stats.datagramsDropped), static_cast<unsigned long long>(stats.datagramsSent),
                    static_cast<unsigned long long>(stats.datagramsReceived), static_cast<unsigned long long>(stats.bytesSent),
                    static_cast<unsigned long long>(stats.bytesReceived));
  }
}

void ServerHost::PollTransport()
{
  m_transport->Poll();

  TransportEvent event;
  while (m_transport->NextEvent(event))
  {
    switch (event.type)
    {
    case TransportEvent::Type::Connected:
      NEURON_LOG_DEBUG("connection %u opened", event.connection);
      break;

    case TransportEvent::Type::Disconnected:
    {
      // A client leaving returns the server to an empty session table, not to
      // a shutdown (ADR-008 §1).
      for (auto session = m_sessions.begin(); session != m_sessions.end(); ++session)
      {
        if (session->connection == event.connection)
        {
          NEURON_LOG_INFO("client %u left (reason %u)", session->clientId, static_cast<unsigned>(event.reason));
          m_sessions.erase(session);
          m_sessionCount.store(static_cast<std::uint32_t>(m_sessions.size()), std::memory_order_relaxed);
          break;
        }
      }
      break;
    }

    case TransportEvent::Type::Message:
      HandleMessage(event);
      break;

    default:
      break;
    }
  }
}

void ServerHost::SimThread()
{
  // One of the two owned lanes MVP registers (ADR-007 §8). Named for the role,
  // not the thread, so a restarted host reuses it.
  (void)Telemetry::RegisterLane("Sim");

  WaitableTimer timer;
  const std::int64_t frequency = Clock::Frequency();
  const auto tickInterval = static_cast<std::int64_t>(static_cast<double>(frequency) / TICK_RATE);

  // Absolute schedule: each deadline is derived from the last one, so a slow
  // tick is absorbed instead of added to every tick after it (ADR-002 §2).
  std::int64_t nextDeadline = Clock::Counter() + tickInterval;
  m_lastStatsCounter = Clock::Counter();

  while (!m_stopRequested.load(std::memory_order_acquire))
  {
    timer.WaitUntil(nextDeadline);

    {
      NEURON_SPAN("Poll");
      PollTransport();
    }

    if (!m_sessions.empty() && Clock::MillisecondsBetween(m_lastStatsCounter, Clock::Counter()) >= STATS_INTERVAL_MS)
      LogNetStats();

    const std::uint32_t tick = m_tick.fetch_add(1, std::memory_order_relaxed) + 1;
    {
      // The row the debug HUD and the server's metrics both read. Measured from
      // the first slice that has a tick, so the budget is never retrofitted.
      NEURON_SPAN("Tick");
      m_simulation->AdvanceTick(tick);
    }
    BroadcastSnapshot(tick);

    nextDeadline += tickInterval;

    const std::int64_t now = Clock::Counter();
    const double behindMs = Clock::MillisecondsBetween(nextDeadline, now);
    if (behindMs > MAX_TICK_DEBT_MS)
    {
      // Too far behind to catch up honestly: drop the debt and say so, rather
      // than sprinting through ticks the world never really experienced.
      m_overruns.fetch_add(1, std::memory_order_relaxed);
      NEURON_COUNTER("TickOverrun", 1); // A shipping counter, not a debug one (ADR-002, Risk R10).
      NEURON_LOG_WARNING("tick loop %.0f ms behind; dropping the debt", behindMs);
      nextDeadline = now + tickInterval;
    }
    else if (behindMs > 0.0)
    {
      // A little behind: run at most a couple of extra ticks to close the gap.
      const auto owed = static_cast<std::uint32_t>(behindMs / TICK_INTERVAL_MS);
      for (std::uint32_t i = 0; i < owed && i < MAX_CATCH_UP_TICKS; ++i)
      {
        const std::uint32_t extra = m_tick.fetch_add(1, std::memory_order_relaxed) + 1;
        {
          NEURON_SPAN("Tick"); // Same row as a scheduled tick: it is the same work.
          m_simulation->AdvanceTick(extra);
        }
        // A catch-up tick is a tick, and a client that did not hear about it
        // would interpolate across a gap the server did not actually have.
        BroadcastSnapshot(extra);
        nextDeadline += tickInterval;
        NEURON_COUNTER("TickCatchUp", 1);
      }
    }
  }

  // Tell whoever is still connected, rather than vanishing.
  std::array<std::uint8_t, 32> buffer{};
  for (const SessionInfo& session : m_sessions)
  {
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Goodbye);
    Write(writer, Goodbye{static_cast<std::uint16_t>(DisconnectReason::ShuttingDown)});
    SendTo(session.connection, TransportChannel::Control, writer);
  }
  m_transport->Poll(); // One last service so the goodbyes actually leave.

  m_transport->Shutdown();
  NEURON_LOG_INFO("server host stopped after %u ticks (%u overruns)", m_tick.load(std::memory_order_relaxed),
                  m_overruns.load(std::memory_order_relaxed));
}
} // namespace Neuron
