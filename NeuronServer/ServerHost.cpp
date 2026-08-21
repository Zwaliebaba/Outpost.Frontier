#include "pch.h"

#include "ServerHost.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "DurableStore.h"
#include "Log.h"
#include "Telemetry.h"
#include "QuicTransport.h"
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

  // QUIC only, per the S13 owner directive -- there is no transport knob, and
  // nothing above this line knows which implementation it is talking to.
  auto transport = std::make_unique<QuicTransport>();
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

void ServerHost::SendSnapshots(std::uint32_t _tick)
{
  if (m_sessions.empty())
  {
    return; // Nobody to tell. Serialising for an empty room is pure waste.
  }

  NEURON_SPAN("Snapshot");

  std::int64_t sent = 0;
  for (SessionInfo& session : m_sessions)
  {
    /*
     * Each client's own sender, asked for that client's own bytes. The refusal
     * -- the fleet outgrowing one datagram, the point at which ADR-004 §6's
     * growth path stops being optional -- is counted and logged inside the
     * sender, which is where "*which* client" is answerable. The host keeps
     * its own total because the debug strip reads one number for the session.
     */
    if (session.sender.Send(*m_simulation, *m_transport, _tick))
    {
      ++sent;
    }
    else
    {
      m_snapshotFailures.fetch_add(1, std::memory_order_relaxed);
    }
  }
  NEURON_COUNTER("SnapshotsSent", sent);
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

    /*
     * Who this is (ADR-018 D5). One value until accounts exist -- but a *value*,
     * not the connection id, and that distinction is the whole point of minting
     * it now: everything player-keyed can key on this from its first line
     * instead of keying on a connection and being rewritten the day one drops.
     *
     * It is decided here, before the session exists, because the session's own
     * `SnapshotSender` is constructed with it (ADR-018 A13).
     */
    const PlayerId playerId = SOLE_PLAYER_ID;

    /*
     * The grid a session opens on: the simulation's own (ADR-009 §8's
     * `worldMeta`). A client that has not asked for a view yet watches the world
     * the server would have shown it anyway, so there is no state in which a
     * session exists with no grid and nothing to send it.
     */
    const WorldMeta world = m_simulation->World();

    SessionInfo& session = m_sessions.emplace_back(m_nextClientId++, playerId, _event.connection, world.gridAnchor);
    session.handshakeComplete = true;
    m_sessionCount.store(static_cast<std::uint32_t>(m_sessions.size()), std::memory_order_relaxed);

    Welcome welcome;
    welcome.clientId = session.clientId;
    welcome.tick = m_tick.load(std::memory_order_relaxed);
    welcome.tickRate = static_cast<std::uint16_t>(TICK_RATE);
    welcome.schemaHash = m_simulation->SchemaHash();
    welcome.contentHash = m_simulation->ContentHash();

    // Where the world is, so a client in another process can place a position
    // before any snapshot arrives (ADR-009 §8).
    welcome.worldId = world.worldId;
    welcome.gridAnchor = world.gridAnchor;
    welcome.anchorX = world.anchorX;
    welcome.anchorY = world.anchorY;
    // The display strings ride along unread, like the id and the anchor: what
    // the world is called is the simulation's to say and the HUD's to draw.
    welcome.worldName = world.worldName;
    welcome.worldDetail = world.worldDetail;
    welcome.worldBadge = world.worldBadge;

    /*
     * The resume token stays zero. There is nothing to authenticate against,
     * and inventing a token now would be inventing a security model with it.
     */
    welcome.playerId = session.playerId;
    welcome.resumeToken = 0;

    WriteWireType(writer, WireType::Welcome);
    Write(writer, welcome);
    SendTo(_event.connection, TransportChannel::Control, writer);

    NEURON_LOG_INFO("client %u joined as '%s'", session.clientId, hello.playerName.empty() ? "(unnamed)" : hello.playerName.c_str());
    return;
  }

  case WireType::ViewRequest:
  {
    /*
     * "Show me that grid" (ADR-016 §4, §7).
     *
     * The engine's whole part: find the session, ask the game whether this
     * viewer may watch that world, and say what happened. It never learns why
     * the answer was no -- the reason code is the game's number travelling
     * through unread, exactly as `OrderAck`'s does (ADR-014 §3).
     */
    SessionInfo* session = FindSession(_event.connection);
    if (session == nullptr || !session->handshakeComplete)
    {
      // Before the handshake there is no viewer to move and no session to
      // answer on. Dropped rather than answered, like a pre-handshake order.
      return;
    }

    ViewRequest request;
    if (!Read(reader, request))
    {
      NEURON_LOG_WARNING("malformed view request from client %u", session->clientId);
      return;
    }

    std::uint16_t reasonCode = 0;
    const bool accepted = session->sender.RequestView(*m_simulation, request.gridAnchor, reasonCode);

    std::array<std::uint8_t, 64> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::ViewChanged);
    Write(writer, ViewChanged{request.gridAnchor, reasonCode, accepted});
    SendTo(session->connection, TransportChannel::Control, writer);
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

    const OrderVerdict verdict = m_simulation->ApplyOrderBytes(session->playerId, session->clientId, reader.Remaining());

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

void ServerHost::WriteDurableSnapshot(std::uint32_t _tick)
{
  if (m_config.durableStore == nullptr || !m_config.durableStore->IsOpen())
  {
    return;
  }

  /*
   * Grow until it fits, rather than persisting half a shard.
   *
   * `WriteDurableState` is the `ByteWriter` contract: it says whether the whole
   * thing went in, and a short write is not a smaller snapshot, it is a
   * corrupt one. Doubling from a megabyte reaches the cap in six tries and the
   * buffer is kept, so the growth happens once in a shard's life.
   */
  if (m_durableBuffer.empty())
  {
    m_durableBuffer.resize(1u << 20);
  }
  bool written = false;
  while (!written)
  {
    ByteWriter writer{m_durableBuffer};
    written = m_simulation->WriteDurableState(writer);
    if (written)
    {
      const std::uint64_t hash = m_simulation->DurableHash();
      if (!m_config.durableStore->WriteSnapshot(writer.Written(), hash, _tick))
      {
        NEURON_LOG_ERROR("the shard snapshot could not be written; the journal still covers everything since the last one");
      }
      else
      {
        NEURON_LOG_INFO("shard snapshot at tick %u: %zu bytes, durable hash %016llx", _tick, writer.BytesWritten(),
                        static_cast<unsigned long long>(hash));
      }
      return;
    }
    if (m_durableBuffer.size() >= MAX_DURABLE_RECORD_BYTES)
    {
      NEURON_LOG_ERROR("the shard's durable state does not fit %zu bytes and was not written", m_durableBuffer.size());
      return;
    }
    m_durableBuffer.resize(m_durableBuffer.size() * 2);
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

    /*
     * The injected stall, taken before anything else this tick does.
     *
     * Here rather than around the tick itself, because a server in trouble is
     * not one whose simulation is slow -- it is one whose *loop* is not
     * running. Nothing is polled, nothing is ticked and nothing is sent for
     * the duration, and the deadline arithmetic below then sees the debt and
     * takes it seriously, which is the whole point: the overrun counter and
     * the catch-up path are reached by the same route a real hitch reaches
     * them.
     *
     * Exchanged rather than read, so one request produces one stall.
     */
    if (const std::uint32_t stallMs = m_stallRequestMs.exchange(0, std::memory_order_relaxed); stallMs != 0)
    {
      NEURON_LOG_WARNING("injected stall: the authority stops for %u ms", stallMs);
      const auto stallCounts = static_cast<std::int64_t>(static_cast<double>(frequency) * stallMs / 1000.0);
      timer.WaitUntil(Clock::Counter() + stallCounts);
      m_stalls.fetch_add(1, std::memory_order_relaxed);
    }

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
    SendSnapshots(tick);

    /*
     * The snapshot cadence, counted in ticks rather than in seconds.
     *
     * The tick is the only clock (ADR-002 §1) and the Sim thread already holds
     * one, so a wall-clock timer here would be a second clock to drift -- and
     * a snapshot taken at a tick number is a snapshot two runs of one script
     * agree about, which is what makes the interrupted-rotation cases testable
     * at all.
     */
    constexpr std::uint32_t SNAPSHOT_INTERVAL_TICKS = SNAPSHOT_INTERVAL_SECONDS * TICK_RATE;
    if (tick % SNAPSHOT_INTERVAL_TICKS == 0)
    {
      WriteDurableSnapshot(tick);
    }

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
        SendSnapshots(extra);
        nextDeadline += tickInterval;
        NEURON_COUNTER("TickCatchUp", 1);
      }
    }
  }

  /*
   * The clean shutdown's snapshot, before anything else is torn down
   * (ADR-025 §4): **a clean stop loses nothing.**
   *
   * Here rather than in `Stop`, because this is the thread that owns the state
   * and `Stop` is called from whichever thread noticed the window close. It is
   * also before the goodbyes, so a host that fails to write says so while the
   * log is still about shutting down rather than about something else.
   */
  WriteDurableSnapshot(m_tick.load(std::memory_order_relaxed));
  if (m_config.durableStore != nullptr)
  {
    m_config.durableStore->Flush();
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
