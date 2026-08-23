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
     * Each client's own sender, asked for that client's own bytes.
     *
     * **The refusal this used to count is gone** (ADR-022 §6). It was the fleet
     * outgrowing one datagram, and the correct answer while a full snapshot in
     * one datagram was the only format; a grid over budget now produces a
     * *partial* view with an honest `culledCount` instead of silence. What is
     * left to count is a sender that had nothing to send at all -- a grid the
     * simulation would not rank -- which is a different and much quieter
     * condition, and it keeps the strip's row honest rather than retiring it.
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
     * Who this is (ADR-018 D5, U3c-b).
     *
     * Two paths, and the order matters: a client that can prove it is coming
     * BACK gets its own player before anything mints a new one. The other way
     * round, a reconnect inside the grace window would be handed a fresh id and
     * a fresh fleet, and the commander whose ships were still standing on a grid
     * would watch a stranger wearing their number.
     *
     * The distinction D5 spent a schema bump on at T2 is what makes this a
     * lookup rather than a rewrite: everything player-keyed already keys on the
     * PLAYER, so a resumed session picks its whole world back up by carrying one
     * integer across.
     */
    const std::uint32_t nowTick = m_tick.load(std::memory_order_relaxed);
    LapsedSession lapsed;
    const bool resumed = m_resume.TryResume(hello.playerId, hello.resumeToken, nowTick, lapsed);

    PlayerId playerId = INVALID_PLAYER_ID;
    if (resumed)
    {
      playerId = lapsed.playerId;
    }
    else
    {
      playerId = m_nextPlayerId++;

      /*
       * A player who is genuinely new to this shard. The composition root
       * decides what that means -- a starting fleet, or nothing at all if this
       * commander already has ships from a reloaded shard -- because what a
       * commander is given is a game question and this library must not have an
       * opinion about it (ADR-014).
       */
      m_simulation->PlayerJoined(playerId);
    }

    /*
     * The grid a session opens on: the one it was watching if it is coming
     * back, and the simulation's own otherwise (ADR-009 §8's `worldMeta`). A
     * client that has not asked for a view yet watches the world the server
     * would have shown it anyway, so there is no state in which a session
     * exists with no grid and nothing to send it -- and a client that HAS asked
     * does not lose the answer to a dropped socket.
     */
    const WorldMeta world = m_simulation->WorldFor(playerId);

    // Where they were watching if they are coming back, and their own grid if
    // they are not (U3c-b).
    const std::uint16_t grid = resumed ? lapsed.grid : world.gridAnchor;
    SessionInfo& session = m_sessions.emplace_back(m_nextClientId++, playerId, _event.connection, grid);
    session.handshakeComplete = true;

    /*
     * And the game is told, so the grid this session opens on is the grid held
     * alive (ADR-016 §7, N5).
     *
     * After the session exists rather than before, because a hold whose session
     * failed to be created would be a world nothing is ever going to release.
     */
    m_simulation->ViewerOpened(playerId, grid);

    // The per-tick byte budget is a deployment number (ADR-022 §5b), so it
    // comes from config and the sender is told rather than deciding. Zero in
    // the config means "whatever the engine thinks is sane", which is what a
    // config written before interest management existed says.
    if (m_config.tickBudgetBytes != 0)
    {
      session.sender.SetTickBudgetBytes(m_config.tickBudgetBytes);
    }

    /*
     * A fresh token every time, including on a resume.
     *
     * Rotating it means a token seen once on the wire is worth one reconnect
     * rather than every reconnect until the commander logs off for good. It is
     * still a bearer token and still not authentication -- `SessionResume.h`
     * is explicit about that -- but a single-use one is strictly less to lose.
     */
    session.resumeToken = m_tokenSource();

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

    /*
     * The grid this SESSION is on, which is the commander's own and not the
     * shard's (U3c-b).
     *
     * It has to be the session's, because the client keeps no other record of
     * where it is: `ClientConnection` sets its grid from this field and from
     * nothing else. A `Welcome` that advertised the shard's grid while the feed
     * sent another's would leave every client believing it was somewhere it was
     * not -- which is how a commander comes to compose a Dock naming a station
     * on a grid they have never been to.
     */
    welcome.gridAnchor = grid;
    welcome.anchorX = world.anchorX;
    welcome.anchorY = world.anchorY;
    // The display strings ride along unread, like the id and the anchor: what
    // the world is called is the simulation's to say and the HUD's to draw.
    welcome.worldName = world.worldName;
    welcome.worldDetail = world.worldDetail;
    welcome.worldBadge = world.worldBadge;

    /*
     * And who they are, with the handle that gets them back (ADR-018 D5).
     *
     * T2 reserved both fields and shipped them as zero, on the ground that
     * inventing a token then would have been inventing a security model with
     * it. That reasoning has not changed and is repeated at length in
     * `SessionResume.h`: this is a resume handle, nobody is authenticated, and
     * what U3c-b changed is that resume became a requirement rather than that
     * the security question got an answer.
     *
     * The layout is untouched, which is the point of having reserved them: two
     * fields that were always on the wire start carrying values, and no schema
     * hash moves.
     */
    welcome.playerId = session.playerId;
    welcome.resumeToken = session.resumeToken;

    WriteWireType(writer, WireType::Welcome);
    Write(writer, welcome);
    SendTo(_event.connection, TransportChannel::Control, writer);

    NEURON_LOG_INFO("client %u %s as '%s' (player %u)", session.clientId, resumed ? "resumed" : "joined",
                    hello.playerName.empty() ? "(unnamed)" : hello.playerName.c_str(), session.playerId);
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
    if (accepted)
    {
      // The camera moved, so the hold moves with it (ADR-016 §7, N5). Reported
      // as the whole answer rather than as a release and a take, so the grid
      // they are arriving on is held before the one they are leaving is let go
      // -- and a switch back to the same grid is not a teardown and a respawn.
      m_simulation->ViewerOpened(session->playerId, session->sender.Grid());
    }

    std::array<std::uint8_t, 64> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::ViewChanged);
    Write(writer, ViewChanged{request.gridAnchor, reasonCode, accepted});
    SendTo(session->connection, TransportChannel::Control, writer);
    return;
  }

  case WireType::SnapshotAck:
  {
    /*
     * "I hold the whole of tick T for grid G" (ADR-022 §2a).
     *
     * The engine's whole part, and it is entirely link semantics: the game is
     * never told what a client has, because what a client has is a fact about a
     * socket. Dropped silently before the handshake, like every other message
     * from a connection that has not joined.
     */
    SessionInfo* session = FindSession(_event.connection);
    if (session == nullptr || !session->handshakeComplete)
    {
      return;
    }
    SnapshotAck ack;
    if (!Read(reader, ack))
    {
      return; // A malformed ack costs one larger delta and nothing else.
    }
    session->sender.NoteAck(ack);
    return;
  }

  case WireType::ViewFocus:
  {
    /*
     * Where this viewer is looking, and what they have selected (ADR-022 §4).
     *
     * ADR-016 §7 said the server had no business holding this and ADR-022 §1 is
     * what changed it: relevance is a property of a viewer, and §5a's guarantee
     * cannot be kept for a selection nobody told the server about.
     *
     * A malformed one -- a selection past `MAX_VIEW_SELECTION` -- is dropped
     * rather than clamped. The reader refuses it for the reason recorded there:
     * a truncated selection is a *different* selection, and the guarantee would
     * then be kept for the wrong set of ships, which is worse than not keeping
     * it because it would look kept.
     */
    SessionInfo* session = FindSession(_event.connection);
    if (session == nullptr || !session->handshakeComplete)
    {
      return;
    }
    ViewFocus focus;
    if (!Read(reader, focus))
    {
      NEURON_LOG_WARNING("malformed view focus from client %u", session->clientId);
      return;
    }
    session->sender.NoteFocus(focus);
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
    SessionInfo* session = FindSession(_event.connection);
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
    else
    {
      // The feedback loop's backstop, kept per viewer (ADR-022 §7). The ack
      // above is the primary path and this is what still promotes the ghost
      // when the ack is lost -- so it advances on acceptance and never on a
      // refusal, which would promote a ghost the authority turned down.
      session->sender.NoteOrderAccepted(verdict.orderSeq);
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
          /*
           * The SOCKET ends; the commander does not (ADR-018 D5, U3c-b).
           *
           * Their fleet is still standing on a grid, their Bay is still full
           * and their refine jobs are still counting down, because all of that
           * is keyed on the player at the universe layer rather than on this
           * connection. So the session lapses onto the resume table with the
           * grid it was watching, and a client back inside the grace window is
           * the same commander rather than a new one.
           *
           * A handshake that never completed has no player to keep, and
           * `Lapse` refuses `INVALID_PLAYER_ID` rather than filing a row nobody
           * can ever claim.
           */
          const std::uint32_t nowTick = m_tick.load(std::memory_order_relaxed);
          m_resume.Lapse(session->playerId, session->resumeToken, session->sender.Grid(),
                         ResumeDeadlineTick(nowTick, TICK_RATE));

          /*
           * And the hold goes with the socket (ADR-016 §7, N5).
           *
           * A commander inside the grace window still owns everything that is
           * keyed on the player -- the paragraph above is the whole list -- but
           * they have no camera, and a hold is about a camera. Worlds forget by
           * design (ADR-018 D2), so a grid that empties while they are away is
           * rebuilt from content on the tick they resume onto it.
           */
          m_simulation->ViewerClosed(session->playerId);

          NEURON_LOG_INFO("client %u left (reason %u); player %u may resume for %u s", session->clientId,
                          static_cast<unsigned>(event.reason), session->playerId, SESSION_GRACE_SECONDS);
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
    /*
     * Sessions nobody came back for (ADR-018 D5).
     *
     * On the tick because the tick is the clock: a lapsed row whose window has
     * closed should go whether or not anybody happens to be connecting. The
     * claim path checks the deadline again for itself, since "did I make it
     * back in time" must not depend on whether a sweep has run since.
     */
    m_resume.Expire(tick);

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
