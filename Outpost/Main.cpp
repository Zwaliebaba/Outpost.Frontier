#include "pch.h"

#include "AppConfig.h"
#include "ConfigLoad.h"
#include "ReplicatedWorldView.h"
#include "SelfTest.h"
#include "UniverseBake.h"
#include "ShaderTable.h"
#include "EconomyLoad.h"
#include "UniverseLoad.h"

// GameLogic, reached only from here: the executable is the one project
// entitled to know both halves (ADR-014 §1).
#include "FleetSummary.h"
#include "OrderMessages.h"
#include "Orders.h"
#include "SchemaHash.h"
#include "ShipClass.h"
#include "Snapshot.h"
#include "StationMessages.h"
#include "SummaryMessages.h"
#include "World.h"
#include "WorldRegistry.h"

#include "ClientApp.h"
#include "ClientConfig.h"

#include "ServerConfig.h"
#include "ServerHost.h"
#include "Simulation.h"

#include "Clock.h"
#include "Log.h"
#include "Telemetry.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/*
 * The composition root (ADR-008).
 *
 * Its whole job: load configuration, start the pieces the mode asks for, and
 * shut them down in the right order. No game logic, no rendering, no
 * networking of its own -- and, deliberately, no argument parsing: wWinMain
 * ignores what it is handed (ADR-012 §A1).
 */

namespace
{
volatile bool g_stopRequested = false;

BOOL WINAPI ConsoleHandler(DWORD _type)
{
  if (_type == CTRL_C_EVENT || _type == CTRL_CLOSE_EVENT || _type == CTRL_BREAK_EVENT)
  {
    g_stopRequested = true; // Let the loop unwind rather than dying mid-tick.
    return TRUE;
  }
  return FALSE;
}

/// Before the log file exists, a fatal problem still has to reach a person.
void ReportFatal(const std::string& _text)
{
  OutputDebugStringA(_text.c_str());
  std::fputs(_text.c_str(), stderr);

  const int wide = MultiByteToWideChar(CP_UTF8, 0, _text.c_str(), -1, nullptr, 0);
  std::wstring message(static_cast<std::size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _text.c_str(), -1, message.data(), wide);
  MessageBoxW(nullptr, message.c_str(), L"Outpost: Frontier", MB_OK | MB_ICONERROR);
}

void ReportStartupFailure(const Outpost::ConfigDiagnostics& _diagnostics)
{
  std::string text = "Outpost could not start.\n\n";
  for (const std::string& error : _diagnostics.errors)
  {
    text += error;
    text += '\n';
  }
  ReportFatal(text);
}

void LogResolvedConfig(const Outpost::AppConfig& _config, const Outpost::ConfigPaths& _paths)
{
  NEURON_LOG_INFO("config: %s", _paths.base.c_str());
  if (!_paths.userLayer.empty())
    NEURON_LOG_INFO("user settings: %s", _paths.userLayer.c_str());
  NEURON_LOG_INFO("mode: %s%s", Outpost::HostModeText(_config.mode), _config.selfTest ? " (self test)" : "");
  NEURON_LOG_INFO("server: port %u, max sessions %u", static_cast<unsigned>(_config.server.port),
                  static_cast<unsigned>(_config.server.maxSessions));
  NEURON_LOG_INFO("universe: %s", _config.universeDefinition.c_str());
  NEURON_LOG_INFO("economy: %s", _config.economyDefinition.c_str());
  const std::string meshDirectory = Outpost::ResolveContentPath(_config.content.meshDirectory);
  if (meshDirectory.empty())
  {
    // Said once, here, with both places named. Nine "could not read" lines from
    // the mesh loader say the same thing nine times and none of them says why.
    NEURON_LOG_ERROR("content: '%s' is not in the working directory or beside the executable (%s)",
                     _config.content.meshDirectory.c_str(), Outpost::ExecutableDirectory().c_str());
  }
  NEURON_LOG_INFO("content: %u meshes from %s", static_cast<unsigned>(_config.content.meshes.size()),
                  meshDirectory.empty() ? _config.content.meshDirectory.c_str() : meshDirectory.c_str());
  NEURON_LOG_INFO("window: %ux%u %s, vsync %s, ui scale %.2f", static_cast<unsigned>(_config.client.window.width),
                  static_cast<unsigned>(_config.client.window.height), _config.client.window.mode.c_str(),
                  _config.client.renderer.vsync ? "on" : "off", _config.client.ui.scale);
}

/*
 * The simulation the server hosts until GameLogic supplies a real one (S5c).
 *
 * From S6 it advances a real `Game::World`; from S7 it also says so, emitting a
 * full quantised snapshot every tick for `ServerHost` to fan out. That closes
 * the loop the whole build order has been assembling: the world moves, and
 * someone can see it.
 *
 * The adapter holds the vtable and forwards; the simulation is GameLogic's
 * (ADR-014 §2a). Everything in this class is a line of wiring.
 */
class UniverseSimulation final : public Simulation
{
public:
  /*
   * The universe runtime, hosted here (build order U2).
   *
   * The registry owns the worlds; this holds the registry and borrows the grid
   * the wire currently serves. Nothing the client sees changes -- there is
   * still exactly one grid and it is still the start anchor's -- and that is
   * the point of doing it now: the shape lands while it costs nothing, so U3a's
   * warp adds a destination rather than a runtime.
   */
  UniverseSimulation(std::uint64_t _contentHash, WorldMeta _worldMeta, const Game::UniverseDef& _universe,
                     std::uint64_t _sessionSeed)
    : m_contentHash(_contentHash),
      // Moved rather than copied: `WorldMeta` carries the HUD's display strings
      // as of main's UI slice, so a copy here is three allocations per boot for
      // nothing.
      m_worldMeta(std::move(_worldMeta)),
      m_startAnchor(_universe.StartAnchorId())
  {
    Game::RegistryConfig config;
    config.sessionSeed = _sessionSeed;
    config.hostId = 0; // ADR-019: three roles, one process, one host.
    m_registry.Reset(&_universe, config);

    // The session holds a viewer on the grid it serves, so the world is never
    // torn down under the wire (ADR-016 §7). U3b generalises this to the
    // player's actual view; U2 builds the hold.
    m_registry.AddViewer(m_startAnchor);
  }

  /*
   * The scripted patrol (Build Order S7).
   *
   * Every wing is sent to a waypoint, and the waypoints rotate every few
   * seconds. It exists so the fleet moves without anyone touching an input
   * device -- which is what makes "is the motion smooth at 144 Hz against 20 Hz
   * snapshots?" a question that can be answered by looking at the screen.
   *
   * It is scripted from the tick index and nothing else, so it replays exactly
   * (ADR-005 §5) and a desync between two runs of the same build would be a
   * real defect rather than an artefact of when someone clicked.
   *
   * From S9 it goes through the real order path -- validated, assigned a server
   * order id, solved into a formation -- rather than writing guidance directly.
   * The fleet therefore arrives in a Line abreast rather than in a heap, which
   * is the formation solve visible without anyone having to click.
   */
  void AdvanceTick(std::uint32_t _tick) override
  {
    constexpr std::uint32_t LEG_TICKS = 200; // Ten seconds at 20 Hz.
    if (_tick % LEG_TICKS == 1 && !m_patrolShips.empty())
    {
      const std::uint32_t leg = (_tick / LEG_TICKS) % 4;
      constexpr float WAYPOINTS[4][2] = {{6000.0f, 0.0f}, {0.0f, 6000.0f}, {-6000.0f, 0.0f}, {0.0f, -6000.0f}};

      Game::OrderSubmit order;
      // The sequence is the leg number, not a counter: derived from the tick, so
      // two runs of the same build submit the same sequence at the same tick.
      order.orderSeq = _tick;
      order.kind = Game::OrderKind::Move;
      order.formation = Game::FormationId::Line;
      order.queueMode = Game::QueueMode::Replace;
      for (const Game::ShipId shipId : m_patrolShips)
      {
        if (!order.AddShip(shipId))
        {
          break; // A fleet past the per-order cap patrols with as much as fits.
        }
      }
      order.target.xCm = Neuron::MetresToCentimetres(WAYPOINTS[leg][0]);
      order.target.yCm = Neuron::MetresToCentimetres(WAYPOINTS[leg][1]);
      order.target.facingTurns16 = Neuron::RadiansToHeading(static_cast<float>(leg) * DirectX::XM_PIDIV2);

      const Game::OrderVerdict verdict = ServedWorld().SubmitOrder(order);
      if (!verdict.accepted)
      {
        NEURON_LOG_WARNING("scripted patrol refused: %s", Game::OrderReasonText(verdict.reason));
      }
    }

    // One number for every live world (ADR-019 §2). Today that is one world.
    m_registry.Tick(_tick);
  }

  /*
   * The tick argument is the loop's; the world knows its own and they are the
   * same number. Writing the world's is the one that cannot drift.
   *
   * The viewer is ignored, and ignoring it is the honest answer today: there is
   * one grid and one commander, so every viewer is owed the same bytes. It is
   * in the signature because the *seam* is what U3b's per-grid views and
   * ADR-022's culling both arrive through (ADR-018 A13) -- when this method
   * starts answering "which grid is this player watching", the call site above
   * it does not change.
   */
  [[nodiscard]] bool WriteSnapshot(PlayerId, std::uint16_t _grid, std::uint32_t, ByteWriter& _writer) override
  {
    /*
     * The grid the *viewer* asked for, not the one the session started on
     * (ADR-016 §4). The session role decides which anchor this is and has
     * already gated it through `MayView`; borrowing it here is what makes the
     * decision real.
     *
     * Borrowed and never stored, like every other use (ADR-019 §6.1). A grid
     * that has torn down under a viewer since the request answers `nullptr`,
     * and that is a refusal rather than a crash: no snapshot goes out, the
     * sender counts it, and the presence-edge rules that decide where the
     * viewer lands instead are A16's, not this function's.
     */
    Game::World* world = m_registry.Borrow(static_cast<Game::AnchorId>(_grid));
    return world != nullptr && Game::WriteSnapshot(*world, _writer);
  }

  /*
   * Whether this commander may watch that grid (ADR-016 §7).
   *
   * Presence-gated, and presence is *having ships there* -- standing on the
   * grid or docked at its station, which ADR-017 §7 folded into the same
   * answer. The registry knows both, so the whole rule is one question asked of
   * the thing that keeps the ship-to-location index.
   *
   * `UnknownAnchor` for a grid that is not live: an anchor nobody is on has no
   * world to show, and saying so with the order family's own reason keeps the
   * refusal in the vocabulary the player already reads. Presence for one
   * commander is presence for the only commander today -- when there are two,
   * this filters on `_viewer` and the shape does not change (ADR-018 D5).
   */
  [[nodiscard]] std::uint16_t MayView(PlayerId, std::uint16_t _grid) override
  {
    const auto anchor = static_cast<Game::AnchorId>(_grid);
    if (m_registry.Borrow(anchor) == nullptr)
    {
      return static_cast<std::uint16_t>(Game::OrderReason::UnknownAnchor);
    }
    for (const Game::ShipId ship : m_patrolShips)
    {
      Game::AnchorId where = Game::INVALID_ID;
      if (m_registry.LocationOf(ship, where) && where == anchor)
      {
        return 0;
      }
    }
    return static_cast<std::uint16_t>(Game::OrderReason::NoPresence);
  }

  /*
   * What this commander is owed at the summary cadence (ADR-016 §6, A13).
   *
   * Two members of the family answer together, and they answer *about each
   * other*: `Summaries()` says where the commander's ships are, and every row
   * that says `Docked` names a station whose roster says which ships those are.
   * So the docked rows are also the enumeration of the rosters worth sending --
   * no separate walk, and a frame that cannot disagree with itself.
   *
   * **Per viewer is the whole point** (ADR-017 §1). Today the registry answers
   * for the one commander there is, so the filter is the identity function and
   * the privacy rule costs nothing; what matters is that the *question* is
   * asked per viewer, because on the broadcast sender this replaced, a roster
   * reaching everyone would have been a leak nothing could catch until U3c
   * first ran two clients.
   *
   * Fleet summaries go in first and rosters fill what is left. That order is a
   * decision rather than an accident: "where is everything" is small, always
   * useful and the answer the strategic surfaces read, while a roster is one
   * station's detail. What does not fit is dropped and **counted**, because
   * paging the family is ADR-016 §6's own problem to solve -- the same sentence
   * `WriteStationRoster` already uses about a hangar outgrowing one message --
   * and a drop nobody counted is a hangar screen that is quietly short.
   */
  [[nodiscard]] bool WriteSummaries(PlayerId _viewer, std::uint32_t, ByteWriter& _writer) override
  {
    const std::vector<Game::FleetSummary> summaries = m_registry.Summaries();
    if (summaries.empty())
    {
      return false; // Nothing to say. An empty frame is a message with no content.
    }

    // Measure before writing anything, the way `WriteSnapshot` does: a frame
    // whose header promised records it could not fit reads as truncated.
    std::size_t budget = _writer.BytesRemaining();
    if (budget < Game::SUMMARY_FRAME_HEADER_BYTES)
    {
      return false;
    }
    budget -= Game::SUMMARY_FRAME_HEADER_BYTES;

    const std::size_t summariesBytes = Game::SUMMARY_RECORD_HEADER_BYTES + Game::FleetSummariesBytes(summaries.size());
    if (summariesBytes > budget)
    {
      return false;
    }
    budget -= summariesBytes;

    std::vector<Game::AnchorId> rosters;
    std::uint32_t dropped = 0;
    for (const Game::FleetSummary& row : summaries)
    {
      if (row.state != Game::FleetState::Docked)
      {
        continue;
      }
      const std::span<const Game::RosterEntry> docked = m_registry.Roster(row.anchor);
      const std::size_t bytes = Game::SUMMARY_RECORD_HEADER_BYTES + Game::StationRosterBytes(docked.size());
      if (bytes > budget || rosters.size() + 1 >= Game::MAX_SUMMARY_RECORDS)
      {
        ++dropped;
        continue;
      }
      budget -= bytes;
      rosters.push_back(row.anchor);
    }

    if (dropped > 0 && !m_summaryDropLogged)
    {
      m_summaryDropLogged = true;
      NEURON_LOG_WARNING("player %u: %u station roster(s) did not fit one summary frame; the family needs paging (ADR-016 §6)",
                         _viewer, dropped);
    }

    const auto records = static_cast<std::uint8_t>(rosters.size() + 1);
    if (!Game::BeginSummaryFrame(records, _writer))
    {
      return false;
    }
    if (!Game::BeginSummaryRecord(Game::SummaryKind::FleetSummaries, _writer) || !Game::WriteFleetSummaries(summaries, _writer))
    {
      return false;
    }
    for (const Game::AnchorId anchor : rosters)
    {
      if (!Game::BeginSummaryRecord(Game::SummaryKind::StationRoster, _writer) ||
          !Game::WriteStationRoster(anchor, m_registry.Roster(anchor), _writer))
      {
        return false;
      }
    }
    return _writer.Ok();
  }

  /*
   * Decode, validate, queue -- and say which order it was (ADR-004 §7).
   *
   * The engine handed over bytes it did not read. Everything that turns them
   * into a decision is GameLogic's, and everything this function does is
   * translation: `OrderMessages` for the layout, `World::SubmitOrder` for the
   * verdict, and the neutral `Neuron::OrderVerdict` on the way back.
   *
   * `orderSeq` travels out through the verdict because the engine never parsed
   * the payload and so cannot fill in the ack's echo itself. A malformed
   * payload has no sequence to echo, which is why it acks zero: the client's
   * ghost then falls back on the snapshot's `lastOrderSeqProcessed`, which will
   * never mention it.
   */
  [[nodiscard]] OrderVerdict ApplyOrderBytes(std::uint32_t, std::span<const std::uint8_t> _payload) override
  {
    Neuron::ByteReader reader{_payload};

    /*
     * Which of the two messages this stream carries (ADR-017 §8).
     *
     * A station command shares the order stream -- one sequence counter, one
     * `OrderAck`, one reason enum -- so the kind byte is the first thing read
     * and the *only* thing that could say which decoder to run. Both payloads
     * open with `u32 orderSeq` and then a byte whose value spaces overlap, so
     * guessing from the body would read an Undock as a Move.
     */
    Game::CommandKind kind = Game::CommandKind::Order;
    if (!Game::ReadCommandKind(reader, kind))
    {
      NEURON_LOG_WARNING("order payload names no command kind (%zu bytes)", _payload.size());
      return Malformed();
    }

    if (kind == Game::CommandKind::Station)
    {
      return ApplyStationCommand(reader, _payload.size());
    }

    Game::OrderSubmit order;
    if (!Game::ReadOrderSubmit(reader, order))
    {
      NEURON_LOG_WARNING("malformed order payload (%zu bytes)", _payload.size());
      return Malformed();
    }

    const Game::OrderVerdict decided = ServedWorld().SubmitOrder(order);

    OrderVerdict verdict;
    verdict.accepted = decided.accepted;
    verdict.reasonCode = static_cast<std::uint16_t>(decided.reason);
    verdict.serverOrderId = decided.serverOrderId;
    verdict.orderSeq = order.orderSeq;
    return verdict;
  }

  /*
   * The station half of the acked stream (ADR-017 §3, §6).
   *
   * The whole verb goes through `WorldRegistry::SubmitStationCommand`, which
   * runs the *same* `ValidateStationCommand` the client's pre-check runs over
   * its replicated `RosterView` -- the station surfaces' half of ADR-014 §3's
   * bounce parity, bought the way the order side bought it: one function, two
   * callers, no forked rules.
   *
   * A station command is universe-layer work and never a world's: an Undock
   * files a transfer against the registry and an AssignWing writes a roster row,
   * and neither touches the grid this session happens to be serving. That is
   * why this does not go through `ServedWorld()`.
   */
  [[nodiscard]] OrderVerdict ApplyStationCommand(Neuron::ByteReader& _reader, std::size_t _payloadBytes)
  {
    Game::StationCommand command;
    if (!Game::ReadStationCommand(_reader, command))
    {
      NEURON_LOG_WARNING("malformed station command (%zu bytes)", _payloadBytes);
      return Malformed();
    }

    const Game::OrderVerdict decided = m_registry.SubmitStationCommand(command);

    OrderVerdict verdict;
    verdict.accepted = decided.accepted;
    verdict.reasonCode = static_cast<std::uint16_t>(decided.reason);
    verdict.serverOrderId = decided.serverOrderId;
    // The client's own counter, shared with orders, which is what lets one ack
    // stream serve both without a ghost being confused with a command.
    verdict.orderSeq = command.orderSeq;
    return verdict;
  }

  /*
   * A payload the decoder would not take.
   *
   * `UnknownKind` and an `orderSeq` of zero, which is deliberate on both
   * counts: there is no sequence to echo when the bytes did not parse, and the
   * client's ghost then falls back on the snapshot's `lastOrderSeqProcessed`,
   * which will never mention it.
   */
  [[nodiscard]] static OrderVerdict Malformed() noexcept
  {
    OrderVerdict malformed;
    malformed.reasonCode = static_cast<std::uint16_t>(Game::OrderReason::UnknownKind);
    return malformed;
  }

  [[nodiscard]] std::uint64_t SchemaHash() const override
  {
    return Game::GameSchemaHash();
  }

  /// The universe and the economy, mixed (ADR-024 §7). One number because the
  /// handshake carries one; both halves state what they read rather than being
  /// told, so a client whose litres drifted is refused at the door exactly as
  /// one whose systems drifted is.
  [[nodiscard]] std::uint64_t ContentHash() const override
  {
    return m_contentHash;
  }

  [[nodiscard]] WorldMeta World() const override
  {
    return m_worldMeta;
  }

  /*
   * The grid the wire serves.
   *
   * Borrowed on every use and never stored (ADR-019 §6.1): a world pointer held
   * across a tick is code that cannot survive the day the grid lives on another
   * machine, and the whole reason to borrow now -- while there is one world and
   * it never moves -- is that the habit is free to form and expensive to
   * retrofit.
   */
  [[nodiscard]] Game::World& ServedWorld()
  {
    Game::World* world = m_registry.Borrow(m_startAnchor);
    ASSERT_TEXT(world != nullptr, L"the session's own start anchor is not in the universe");
    return *world;
  }

  /// The fleet, spawned into the start grid with ids the registry allocated
  /// (ADR-018 D6a), recording the mobile half as it goes.
  void SpawnStartingFleet();

  /// Gives the worlds to whichever thread runs them next (ADR-007 §7). The last
  /// thing the composition root does to the simulation, and the reason the sim
  /// thread's first tick adopts rather than trips.
  void HandOff() noexcept { m_registry.HandOff(); }

private:
  std::uint64_t m_contentHash = 0;
  WorldMeta m_worldMeta;

  Game::WorldRegistry m_registry;
  Game::AnchorId m_startAnchor = Game::INVALID_ID;

  /// The mobile half of the fleet. The station is deliberately absent: sending
  /// a `Structure` a waypoint is harmless -- it has no speed -- but listing it
  /// would imply it might move.
  std::vector<Game::ShipId> m_patrolShips;

  /// Said once, not once a second: a summary frame that cannot hold every
  /// roster will not hold them next second either, and a warning at 1 Hz is a
  /// log nobody reads the rest of.
  bool m_summaryDropLogged = false;
};

/*
 * The starting fleet, on the grid the universe definition anchored.
 *
 * Deliberately modest and deliberately here: what ships a session begins with
 * is a scenario, not a rule, and a scenario belongs in the composition root
 * until there is a save file to read one from.
 *
 * The layout -- one wing per playable hull -- was chosen in S6 to match the
 * client's placeholder, so that when the replicated fleet replaced the invented
 * one the picture would change as little as possible and any difference would be
 * a real difference. The placeholder is gone and this is now the only fleet
 * there is, but the layout stays: it puts every hull class on screen at once,
 * which is what makes a rendering or replication fault obvious rather than
 * subtle.
 */
/*
 * The starting fleet: one wing per playable hull, and what to call each one.
 *
 * The names are content and the ships are content, so they sit together in one
 * table rather than in two arrays that have to be kept the same length -- which
 * is the arrangement that eventually puts a Carrier's call sign on a wing of
 * Interceptors. A fleet file would move the whole table; splitting it now would
 * split it into two files.
 *
 * The call signs are the ones on `tactical-hud.png`, which is where the roster
 * this feeds is drawn.
 */
struct FleetWing
{
  Game::HullClass hullClass;
  const char* name;
};

constexpr FleetWing STARTING_FLEET[] = {
    {Game::HullClass::Interceptor, "TALON"}, {Game::HullClass::Bomber, "ANVIL"},
    {Game::HullClass::Corvette, "SPUR"},     {Game::HullClass::Frigate, "LANTERN"},
    {Game::HullClass::Hauler, "DRIFT"},      {Game::HullClass::Miner, "KILN"},
    {Game::HullClass::Carrier, "MARROW"},    {Game::HullClass::Battleship, "ECHO"},
};

constexpr std::size_t WING_COUNT = std::size(STARTING_FLEET);
constexpr std::uint32_t SHIPS_PER_WING = 5;

/// The tangential room a wing takes up on the ring: its line from end to end,
/// plus a hull radius at each end, which is what has to clear its neighbour.
[[nodiscard]] float WingWidthMetres(const FleetWing& _wing)
{
  const Game::ShipClassInfo& info = Game::ShipClass(_wing.hullClass);
  return static_cast<float>(SHIPS_PER_WING - 1) * info.formationSpacingMetres + 2.0f * info.collisionRadiusMetres;
}

/*
 * Which slot on the ring each wing parks in, indexed by its place in the table
 * above.
 *
 * The wings used to take the slots in table order, which put the two widest
 * next to each other and parked the fleet in an invalid state: MARROW's line
 * end and ECHO's sat 221 m apart against a 227 m contact, and `Separate` healed
 * the 6 m on tick 1 (ADR-015 §5, which left the re-park to the scenario owner).
 * The overlap was the visible end of the real fault, which is that the ring
 * divides evenly among wings that are not evenly sized: a Battleship wing is
 * 1,920 m end to end and an Interceptor wing 280 m, and 45 degrees was handed
 * to both. The table happened to run smallest hull to largest, so the two
 * capitals landed adjacent -- the worst arrangement of the eight.
 *
 * So the ring is dealt widest-with-narrowest: the widest wing takes a slot, the
 * narrowest takes the next, and so on inward, which leaves every capital
 * flanked by two of the smallest wings there are. At the authored radius that
 * turns a 6 m overlap into 90 m of clearance, and the tightest pair stops being
 * a pair of capitals.
 *
 * Derived rather than authored on purpose. An order hand-picked today is an
 * arrangement the next person breaks by adding the reserved Fighter and Cruiser
 * wings or by retuning one spacing in the class table -- which is exactly how
 * this defect arrived. Widening the ring does not substitute for it: the wing
 * lines are chords, so past a point they cross rather than separate, and this
 * overlap gets *worse* between 1,400 m and 1,800 m.
 */
[[nodiscard]] std::array<std::size_t, WING_COUNT> WingRingSlots()
{
  std::array<std::size_t, WING_COUNT> byWidth{};
  for (std::size_t index = 0; index < WING_COUNT; ++index)
  {
    byWidth[index] = index;
  }

  std::sort(byWidth.begin(), byWidth.end(),
            [](std::size_t _a, std::size_t _b)
            {
              const float widthA = WingWidthMetres(STARTING_FLEET[_a]);
              const float widthB = WingWidthMetres(STARTING_FLEET[_b]);
              // Table position breaks the tie, so this is a total order and two wings of
              // equal width cannot trade slots between runs. The fleet feeds the world
              // hash; a layout that sorted unstably would be a replay that disagreed with
              // itself for no reason anyone could see.
              return widthA != widthB ? widthA > widthB : _a < _b;
            });

  std::array<std::size_t, WING_COUNT> slotOf{};
  std::size_t widest = 0;
  std::size_t narrowest = WING_COUNT;
  for (std::size_t slot = 0; widest < narrowest; ++slot)
  {
    // Even slots take the next-widest wing and odd slots the next-narrowest,
    // which is the alternation: the two ends of the sorted list meet as
    // neighbours, so a capital never stands beside another capital.
    const std::size_t wing = (slot % 2 == 0) ? byWidth[widest++] : byWidth[--narrowest];
    slotOf[wing] = slot;
  }
  return slotOf;
}

/// One parked hull, kept only as long as it takes to measure the layout.
struct ParkedHull
{
  float xMetres = 0.0f;
  float yMetres = 0.0f;
  float radiusMetres = 0.0f;
  std::size_t wing = 0;
  const char* wingName = "";
};

/*
 * Measure the parked fleet, and put the number in the log.
 *
 * ADR-015 §5's phrasing about the overlap it found is the reason this exists:
 * "invisible until something measured it". `Separate` is a repair, so authored
 * content that parks two hulls inside each other reads as a fleet that shuffles
 * on tick 1 rather than as content that is wrong -- which is a defect that
 * hides in a working game. Measuring it every boot costs 780 pairs, once.
 *
 * Cross-wing pairs only. Two ships in one wing stand a formation spacing apart
 * against radii of at most a quarter of it, so their clearance is half the
 * spacing by construction and the class table's own test already holds that
 * bound. What this reports is the number the *ring* controls.
 */
void ReportParkedFleet(const std::vector<ParkedHull>& _parked)
{
  float tightest = std::numeric_limits<float>::max();
  const ParkedHull* nearestA = nullptr;
  const ParkedHull* nearestB = nullptr;

  for (std::size_t a = 0; a + 1 < _parked.size(); ++a)
  {
    for (std::size_t b = a + 1; b < _parked.size(); ++b)
    {
      if (_parked[a].wing == _parked[b].wing)
      {
        continue;
      }

      const float dx = _parked[a].xMetres - _parked[b].xMetres;
      const float dy = _parked[a].yMetres - _parked[b].yMetres;
      const float clearance = std::sqrt(dx * dx + dy * dy) - (_parked[a].radiusMetres + _parked[b].radiusMetres);

      if (clearance < 0.0f)
      {
        NEURON_LOG_WARNING("parked fleet: %s and %s overlap by %.1f m; Separate will heal it on tick 1", _parked[a].wingName,
                           _parked[b].wingName, -clearance);
      }
      if (clearance < tightest)
      {
        tightest = clearance;
        nearestA = &_parked[a];
        nearestB = &_parked[b];
      }
    }
  }

  if (nearestA != nullptr && nearestB != nullptr)
  {
    NEURON_LOG_INFO("parked fleet: %zu hulls, tightest cross-wing clearance %.1f m (%s to %s)", _parked.size(), tightest,
                    nearestA->wingName, nearestB->wingName);
  }
}

/// The names as the world view wants them: indexed by `WingId`, so index 0 is
/// `INVALID_WING_ID` and is a placeholder nothing draws.
[[nodiscard]] std::vector<std::string> WingNames()
{
  std::vector<std::string> names;
  names.reserve(std::size(STARTING_FLEET) + 1);
  names.emplace_back("-"); // WingId 0: no wing. The stations are here.
  for (const FleetWing& entry : STARTING_FLEET)
  {
    names.emplace_back(entry.name);
  }
  return names;
}

void UniverseSimulation::SpawnStartingFleet()
{
  /*
   * The authored fleet, into the grid the registry spun up.
   *
   * Two things changed with U2 and neither is visible from outside. The station
   * is no longer spawned here -- it is the start anchor's *authored occupant*,
   * so the registry put it there on spin-up with the id the bake derived, which
   * is what makes a torn-down and recreated grid come back identical. And every
   * ship is spawned *through the registry* rather than into a borrowed world,
   * because the registry is what allocates the id (ADR-018 D6a -- a ship keeps
   * its id across grids, and a per-world counter would collide on the first
   * transfer) and what records where the ship went, and a spawn that did one
   * without the other would leave the fleet out of "where are my ships".
   */
  // Where each wing stands. Read here rather than folded into the loop because
  // the ring order and the table order are two different things now: the table
  // still decides `WingId`, the call sign and the roster row, so ids and names
  // are exactly what they were before the re-park.
  const std::array<std::size_t, WING_COUNT> ringSlot = WingRingSlots();

  std::vector<ParkedHull> parked;
  parked.reserve(WING_COUNT * SHIPS_PER_WING);

  std::uint32_t wing = 0;
  for (const FleetWing& entry : STARTING_FLEET)
  {
    const Game::HullClass hullClass = entry.hullClass;
    const float wingAngle = (static_cast<float>(ringSlot[wing]) / static_cast<float>(WING_COUNT)) * DirectX::XM_2PI;
    const Game::ShipClassInfo& info = Game::ShipClass(hullClass);
    const float spacing = info.formationSpacingMetres;

    for (std::uint32_t index = 0; index < SHIPS_PER_WING; ++index)
    {
      constexpr float WING_RADIUS_METRES = 1400.0f;
      const float offset = (static_cast<float>(index) - 0.5f * static_cast<float>(SHIPS_PER_WING - 1)) * spacing;

      Game::ShipSpawn spawn;
      spawn.hullClass = hullClass;
      spawn.wing = static_cast<Game::WingId>(wing + 1);
      // Ships face the anchor, which is where the station is -- a fleet parked
      // facing outward would read as a fleet about to leave.
      spawn.headingRadians = wingAngle + DirectX::XM_PI;
      spawn.xMetres = std::cos(wingAngle) * WING_RADIUS_METRES - std::sin(wingAngle) * offset;
      spawn.yMetres = std::sin(wingAngle) * WING_RADIUS_METRES + std::cos(wingAngle) * offset;

      const Game::ShipId id = m_registry.Spawn(m_startAnchor, spawn);
      if (id != Game::INVALID_SHIP_ID)
      {
        m_patrolShips.push_back(id);
        parked.push_back({spawn.xMetres, spawn.yMetres, info.collisionRadiusMetres, wing, entry.name});
      }
    }
    ++wing;
  }

  ReportParkedFleet(parked);
}

/*
 * The version-of-space on the top bar's `FRONTIER 0.4` line, and the start
 * system's security rating on its `SEC 0.4` badge (`tactical-hud.png`).
 *
 * Authored here, beside the fleet and the wing names, for the same reason those
 * are: the universe format has no version or security field yet, and growing it
 * would grow the hashed content (ADR-012 §D) for two strings the session merely
 * displays. When ADR-016's security bands land in the bake, the badge reads
 * from the system instead of from this constant -- the seam it travels through
 * does not change.
 */
constexpr std::string_view SPACE_VERSION_TEXT = "0.4";
constexpr std::string_view START_SECURITY_TEXT = "SEC 0.4";

/// The universe's world meta, in the neutral terms the engine speaks
/// (ADR-009 §8): which world, where its tactical grid is anchored, and the
/// display strings the HUD's top bar draws for it.
WorldMeta MakeWorldMeta(const Game::UniverseDef& _universe)
{
  const Game::GridAnchor anchor = _universe.StartAnchor();

  WorldMeta meta;
  meta.worldId = anchor.system;
  // Which grid, as opposed to where it is. The client needs a number it can put
  // in a Dock's `anchor` field and in a station command's `station` field
  // (ADR-017 §2, §3); before this it had neither.
  meta.gridAnchor = _universe.StartAnchorId();
  meta.anchorX = anchor.origin.x;
  meta.anchorY = anchor.origin.y;

  // The prime slot is the player's location -- the system, not the product
  // (`tactical-hud.png` §top-bar). The detail line is the region of space and
  // its version: the universe's own name, which for this game is "Frontier".
  const Game::SolarSystem* system = _universe.FindSystem(anchor.system);
  meta.worldName = system != nullptr ? system->name : "?";
  meta.worldDetail = _universe.name + " " + std::string(SPACE_VERSION_TEXT);
  meta.worldBadge = std::string(START_SECURITY_TEXT);
  return meta;
}

void LogResolvedUniverse(const Outpost::UniverseLoadResult& _universe)
{
  const Game::GridAnchor anchor = _universe.universe.StartAnchor();
  NEURON_LOG_INFO("universe: '%s' from %s (%u system(s), hash %016llx)", _universe.universe.name.c_str(), _universe.path.c_str(),
                  static_cast<unsigned>(_universe.universe.systems.size()), static_cast<unsigned long long>(_universe.universeHash));

  const Game::SolarSystem* system = _universe.universe.FindSystem(anchor.system);
  NEURON_LOG_INFO("start: system %u '%s', grid anchored at (%lld, %lld)", static_cast<unsigned>(anchor.system),
                  system != nullptr ? system->name.c_str() : "?", static_cast<long long>(anchor.origin.x),
                  static_cast<long long>(anchor.origin.y));
}

/*
 * The client's half of the seam (ADR-014 §2a).
 *
 * The one table that maps the game's hull taxonomy onto the renderer's mesh
 * ids. They are different orderings of overlapping sets on purpose: `HullClass`
 * is the icon sheet's closed eleven, ordered so wire values never renumber
 * (ADR-009 §6), while the mesh list in `Outpost.json` runs smallest hull to
 * largest. Neither side should learn the other's, so the translation lives
 * here, in the only project entitled to know both.
 *
 * The content hash is the universe and the economy mixed (ADR-024 §7). Both
 * halves load the identical files and each states what it read rather than
 * being told (ADR-009 §8) -- in `mode: "client"` that is the whole safety
 * property, because a client whose content drifted is refused at the door
 * instead of rendering a world nobody is simulating. Mixing rather than adding
 * a second wire field is what makes the economy free to guard; its price is
 * that the number cannot say *which* file differs, which is why the boot log
 * states both separately.
 */
Outpost::ReplicatedWorldView::Desc MakeWorldViewDesc(const Outpost::AppConfig& _config, const Outpost::UniverseLoadResult& _universe,
                                                     std::uint64_t _contentHash)
{
  // The mesh list, by name, is the renderer's classId order. Matching on the
  // authored file name rather than on position means reordering the list in
  // `Outpost.json` reorders the meshes and nothing breaks.
  static constexpr struct
  {
    Game::HullClass hullClass;
    std::string_view meshFile;
  } MESH_FOR_HULL[] = {
    {Game::HullClass::Interceptor, "Interceptor.obj"}, {Game::HullClass::Bomber, "Bomber.obj"},
    {Game::HullClass::Corvette, "Corvette.obj"},       {Game::HullClass::Frigate, "Frigate.obj"},
    {Game::HullClass::Hauler, "Hauler.obj"},           {Game::HullClass::Miner, "Miner.obj"},
    {Game::HullClass::Carrier, "Carrier.obj"},         {Game::HullClass::Battleship, "Battleship.obj"},
    {Game::HullClass::Structure, "Structure.obj"}, {Game::HullClass::Gate, "Stargate.obj"},
  };

  Outpost::ReplicatedWorldView::Desc desc;
  desc.renderClassByHull.assign(Game::HULL_CLASS_COUNT, Outpost::ReplicatedWorldView::INVALID_RENDER_CLASS);
  desc.contentHash = _contentHash;
  desc.wingNames = WingNames();
  // Which grid this client watches, so a station has a number the client can
  // address it by (ADR-017 8). The same anchor the `Welcome` carries; taken
  // from the universe here because the composition root builds both.
  desc.gridAnchor = _universe.universe.StartAnchorId();

  /*
   * What each station is called, by the anchor it stands on (ADR-017 1).
   *
   * Walked from the anchor table rather than from the station list, because the
   * anchor is what the wire carries and the station is what has a name -- an
   * anchor's `owner` is the station's per-system id, so the join has to happen
   * on this side. Every station anchor is named even for a system this session
   * never visits: the list is a few dozen strings for a bake of this size, and
   * a lazily filled one would be a cache invalidated by nothing.
   */
  for (const Game::SolarSystem& system : _universe.universe.systems)
  {
    for (const Game::Anchor& anchor : system.anchors)
    {
      if (anchor.kind != Game::AnchorKind::Station)
      {
        continue;
      }
      const Game::Station* station = _universe.universe.FindStation(system.id, anchor.owner);
      if (station == nullptr)
      {
        continue; // An anchor whose station the bake did not place. Drawn unnamed.
      }
      desc.stationNames.push_back(Outpost::ReplicatedWorldView::StationName{anchor.id, station->name});
    }
  }

  for (const auto& mapping : MESH_FOR_HULL)
  {
    for (std::size_t index = 0; index < _config.content.meshes.size(); ++index)
    {
      if (_config.content.meshes[index] == mapping.meshFile)
      {
        desc.renderClassByHull[static_cast<std::size_t>(mapping.hullClass)] = static_cast<std::uint16_t>(index);
        break;
      }
    }
  }

  // Fighter and Cruiser stay INVALID_RENDER_CLASS: reserved ids with no mesh
  // and no content, which the view draws as nothing rather than as a stand-in
  // (ADR-009 §6).
  for (std::size_t hull = 0; hull < desc.renderClassByHull.size(); ++hull)
  {
    const auto hullClass = static_cast<Game::HullClass>(hull);
    if (Game::HullClassHasContent(hullClass) && desc.renderClassByHull[hull] == Outpost::ReplicatedWorldView::INVALID_RENDER_CLASS)
    {
      NEURON_LOG_WARNING("no mesh configured for hull class '%s'; it will not be drawn",
                         std::string(Game::HullClassName(hullClass)).c_str());
    }
  }
  return desc;
}

/// The server takes the same treatment: a plain struct, assembled here.
ServerConfig MakeServerConfig(const Outpost::AppConfig& _config)
{
  ServerConfig server;
  server.port = _config.server.port;
  server.maxSessions = _config.server.maxSessions;
  return server;
}

/// Maps the file's settings onto what the client library asks for. The client
/// never sees AppConfig: libraries take plain structs from the composition root.
ClientConfig MakeClientConfig(const Outpost::AppConfig& _config)
{
  ClientConfig client;
  client.windowWidth = _config.client.window.width;
  client.windowHeight = _config.client.window.height;
  client.windowTitle = "Outpost: Frontier";
  client.borderlessFullscreen = _config.client.window.mode == "borderless";
  client.vsync = _config.client.renderer.vsync;
  client.frameCap = _config.client.renderer.frameCap;
  client.msaaSamples = _config.client.renderer.msaa; // S14: the 4x offscreen target.
  client.serverHost = _config.client.connectHost;
  client.serverPort = _config.client.connectPort;

  // Content: where it is and which meshes to load, in classId order. The client
  // opens the files, but it is told what to open -- the engine has no opinion
  // about which index is a Carrier (ADR-014).
  //
  // Resolved here rather than passed through, because resolving is the
  // composition root's job and the engine's rule is "open what you are handed".
  // A bare `GameData/Meshes` is relative to the working directory, which is the
  // project folder when Visual Studio launches the debugger and the deployment
  // folder when anything else does; the universe file has always been found by
  // this rule and the meshes were the one content path that was not.
  const std::string meshDirectory = Outpost::ResolveContentPath(_config.content.meshDirectory);
  client.meshDirectory = meshDirectory.empty() ? _config.content.meshDirectory : meshDirectory;
  client.meshFiles = _config.content.meshes;
  client.fontFamily = _config.client.ui.font;

  // The audio content, resolved by the same rule the meshes are: a bare
  // `GameData/Audio` is relative to a working directory that differs between
  // the debugger and a deployment.
  const std::string audioDirectory = Outpost::ResolveContentPath(_config.content.audioDirectory);
  client.audioDirectory = audioDirectory.empty() ? _config.content.audioDirectory : audioDirectory;
  const std::string soundBank = Outpost::ResolveContentPath(_config.content.soundBank);
  client.soundBankFile = soundBank.empty() ? _config.content.soundBank : soundBank;

  // Mapped field by field for the same reason the nebula is: the engine may not
  // take a type from the host (ADR-012, ADR-014).
  client.audio.enabled = _config.client.audio.enabled;
  client.audio.master = static_cast<float>(_config.client.audio.master);
  client.audio.world = static_cast<float>(_config.client.audio.world);
  client.audio.ambience = static_cast<float>(_config.client.audio.ambience);
  client.audio.ui = static_cast<float>(_config.client.audio.ui);
  client.audio.music = static_cast<float>(_config.client.audio.music);
  client.audio.alerts = static_cast<float>(_config.client.audio.alerts);
  client.audio.voiceCap3D = _config.client.audio.voiceCap3D;
  client.audio.voiceCap2D = _config.client.audio.voiceCap2D;

  client.cameraZoomMetres = static_cast<float>(_config.client.camera.zoomMetres);

  // Two structs describing the same thing, mapped rather than shared: the
  // engine may not take a type from the host, and the host may not take one
  // from the engine's config layer either (ADR-012, ADR-014). The cost is this
  // block; the benefit is that NeuronClient has no idea Outpost.json exists.
  client.nebula.tintRed = static_cast<float>(_config.client.nebula.tintRed);
  client.nebula.tintGreen = static_cast<float>(_config.client.nebula.tintGreen);
  client.nebula.tintBlue = static_cast<float>(_config.client.nebula.tintBlue);
  client.nebula.intensity = static_cast<float>(_config.client.nebula.intensity);
  client.nebula.tileMetres = static_cast<float>(_config.client.nebula.tileMetres);
  client.nebula.resolution = _config.client.nebula.resolution;
  client.nebula.octaves = _config.client.nebula.octaves;
  client.nebula.coverage = static_cast<float>(_config.client.nebula.coverage);
  client.nebula.contrast = static_cast<float>(_config.client.nebula.contrast);
  client.nebula.seed = _config.client.nebula.seed;
  client.cameraYawSnapDegrees = static_cast<float>(_config.client.camera.yawSnapDegrees);
  client.uiScale = static_cast<float>(_config.client.ui.scale);
  client.uiPalette = _config.client.ui.palette;
  client.diagnosticsStrip = _config.client.diagnostics.strip; // S14: the Tier-1 strip's setting.

  /*
   * Which status bits the overlay marks (ADR-017 5).
   *
   * One bit today, and this line is the only place in the build that says what
   * it means: `SHIP_STATUS_PROTECTED` is GameLogic's name for undock
   * protection, and setting it here tells the engine "put a mark on ships
   * carrying this flag" without telling it -- or letting it work out -- what
   * the flag is about. NeuronClient compiles with no knowledge of docking, and
   * a second game on these libraries names its own bits here instead.
   */
  client.statusMarkBits = Game::SHIP_STATUS_PROTECTED;

  // The world is not here any more. Scenery, the grid anchor and the handshake
  // hashes went behind `Neuron::WorldView` with S5c: configuration is how the
  // client is set up, not what it is looking at.
#if defined(_DEBUG)
  client.enableDebugLayer = true; // Every debug run gets the validation layer.
#endif
  return client;
}
} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
  // Before anything computes with DirectXMath: the library is compiled for the
  // instruction set /arch selects, and running it on a CPU without that set is
  // an illegal instruction somewhere far from here (ADR-010, Risk R11).
  // Called directly rather than through a helper -- DirectXMath is used
  // natively, and boot is the use site.
  if (!DirectX::XMVerifyCPUSupport())
  {
    MessageBoxW(nullptr, L"This CPU does not support the instruction set this build requires.", L"Outpost: Frontier", MB_OK | MB_ICONERROR);
    return 4;
  }

  wchar_t filename[MAX_PATH];
  GetModuleFileNameW(nullptr, filename, MAX_PATH);
  auto path = std::wstring(filename);
  path = path.substr(0, path.find_last_of('\\'));

  FileSys::SetHomeDirectory(path);

  Clock::Initialise();
  SetConsoleCtrlHandler(&ConsoleHandler, TRUE);

  // The composition root owns the Main lane so headless mode -- which never
  // constructs a ClientApp -- still has one (ADR-007 §8).
  (void)Telemetry::RegisterLane("Main");

  Outpost::AppConfig config;
  Outpost::ConfigPaths paths;
  Outpost::ConfigDiagnostics diagnostics;
  if (!Outpost::LoadAppConfig(config, paths, diagnostics))
  {
    ReportStartupFailure(diagnostics);
    return 1;
  }

  Log::Initialise(config.logging.file, config.logging.level);
  NEURON_LOG_INFO("Outpost: Frontier starting");
  for (const std::string& warning : diagnostics.warnings)
    NEURON_LOG_WARNING("%s", warning.c_str());
  LogResolvedConfig(config, paths);

  /*
   * The bake (build order U1), and it exits: this mode writes content rather
   * than running the game, so there is no window, no server and no universe to
   * load -- it is about to make one.
   *
   * It lives in the composition root because writing a file is the one thing
   * GameLogic may not do (ADR-009 §7): the generator is pure and hands back
   * bytes, and the host is what turns bytes into a path on disk. Config-driven
   * per ADR-012 -- there is no argv to pass a seed on.
   */
  if (config.mode == Outpost::HostMode::Bake)
  {
    const int result = Outpost::RunBake(config);
    Log::Shutdown();
    return result;
  }

  // The universe, read here and parsed by GameLogic (ADR-009 §7). Both halves
  // load the identical definition, so this happens once and feeds both -- and
  // it happens before anything starts, because a universe that will not parse
  // is not a degraded mode, it is a refusal to run.
  //
  // Timed, because R17 is a risk with a threshold rather than a worry: the
  // committed universe is 2,500 systems of JSON, the fallback if parsing it
  // costs about a second is a *structural* per-region content split, and a
  // threshold nobody measures is a threshold nobody notices crossing. The
  // number is logged on every boot, so CI's self test re-takes it on every push
  // in the configuration ADR-018 D11 says perf numbers mean.
  const std::int64_t universeStart = Clock::Counter();
  Outpost::UniverseLoadResult universe;
  std::vector<std::string> universeErrors;
  if (!Outpost::LoadUniverse(config.universeDefinition, universe, universeErrors))
  {
    Outpost::ConfigDiagnostics universeDiagnostics;
    universeDiagnostics.errors = universeErrors;
    for (const std::string& error : universeErrors)
      NEURON_LOG_ERROR("%s", error.c_str());
    ReportStartupFailure(universeDiagnostics);
    Log::Shutdown();
    return 1;
  }
  NEURON_LOG_INFO("universe: read, parsed and hashed in %.0f ms (R17's threshold is ~1000 ms)",
                  Clock::MillisecondsBetween(universeStart, Clock::Counter()));
  LogResolvedUniverse(universe);

  /*
   * The economy, beside the universe and guarded the same way (ADR-024 §7).
   *
   * Timed for the same reason the universe is, though this file is kilobytes
   * rather than megabytes: the number that matters is the one a boot reports,
   * not the one anybody expected, and CI re-takes it on every push.
   */
  const std::int64_t economyStart = Clock::Counter();
  Outpost::EconomyLoadResult economy;
  std::vector<std::string> economyErrors;
  if (!Outpost::LoadEconomy(config.economyDefinition, economy, economyErrors))
  {
    Outpost::ConfigDiagnostics economyDiagnostics;
    economyDiagnostics.errors = economyErrors;
    for (const std::string& error : economyErrors)
      NEURON_LOG_ERROR("%s", error.c_str());
    ReportStartupFailure(economyDiagnostics);
    Log::Shutdown();
    return 1;
  }
  NEURON_LOG_INFO("economy: read, parsed and hashed in %.0f ms from %s",
                  Clock::MillisecondsBetween(economyStart, Clock::Counter()), economy.path.c_str());

  /*
   * One number for the handshake, two in the log.
   *
   * The wire carries a single `contentHash`, so the economy rides into the
   * fail-closed check with no new field and no schema bump -- and the cost of
   * mixing is that the number cannot name the file that differs. Hence both
   * hashes, stated separately, right here: when a client is refused, this line
   * is what tells an operator which half drifted.
   */
  const std::uint64_t contentHash = Game::MixContentHashes(universe.universeHash, economy.economyHash);
  NEURON_LOG_INFO("content: universe %016llx, economy %016llx, mixed %016llx",
                  static_cast<unsigned long long>(universe.universeHash), static_cast<unsigned long long>(economy.economyHash),
                  static_cast<unsigned long long>(contentHash));

  // GameLogic implements Simulation and the composition root injects it
  // (ADR-014 §2). Until it does (S5c), the server hosts one that advances
  // nothing but knows its content hash and where its world is anchored -- which
  // is enough to prove the loop, the sessions, the wire and the handshake.
  // The session seed is the **universe** hash, while the hash the wire carries
  // is the mixed one: same universe, same session, same worlds -- and a
  // universe change is a different session by construction. The economy is
  // deliberately not in the seed. It guards the handshake because both halves
  // must agree about a litre, but a hold size has no business reshuffling every
  // grid's randomness, and folding it in would make retuning balance silently
  // re-roll the worlds.
  UniverseSimulation simulation{contentHash, MakeWorldMeta(universe.universe), universe.universe, universe.universeHash};
  simulation.SpawnStartingFleet();

  /*
   * And that is the last thing this thread does to the world (ADR-007 §7).
   *
   * Everything above ran on Main -- the registry spun the start grid up, the
   * fleet was spawned into it -- and everything below runs it on the server's
   * Sim thread. The hand-off is explicit because the alternative is a rule
   * nobody can check: the owner-assert adopts the first thread to touch a
   * world, so without this line the sim's first tick would trip on Main's
   * fingerprints. It did, in CI, which is how this line came to exist.
   */
  simulation.HandOff();

  // Before anything opens a window: the self test is a diagnostic, and its
  // answer is an exit code (Build Order S4).
  if (config.selfTest)
  {
    const int result = Outpost::RunSelfTest(config, simulation);
    Log::Shutdown();
    return result;
  }

  // Boot order is normative (ADR-008 §5): the server starts before the client,
  // and the client is torn down first.
  ServerHost server;

  int exitCode = 0;
  const bool hostsServer = config.mode != Outpost::HostMode::Client;
  if (hostsServer && !server.Start(MakeServerConfig(config), simulation))
  {
    NEURON_LOG_ERROR("server failed to start");
    Log::Shutdown();
    return 2;
  }

  // check_hresult failures (device, swapchain, command lists) and assertion
  // fatals arrive here as exceptions; the boundary turns them into a log line
  // and a message box instead of a silent crash, and still stops the server.
  try
  {
    switch (config.mode)
    {
    case Outpost::HostMode::Host:
    case Outpost::HostMode::Client:
    {
      ClientConfig clientConfig = MakeClientConfig(config);
      if (config.mode == Outpost::HostMode::Host)
      {
        // Port 0 asked the OS to choose, so the client is told what it chose.
        // This convenience dies with the split, which is the point of it being
        // the only thing the two halves share besides the socket.
        clientConfig.serverHost = "127.0.0.1";
        clientConfig.serverPort = server.BoundPort();
      }

      // Engine meets game here and nowhere else (ADR-014 §6). The client gets
      // a world view by reference and never learns what is behind it; hosting
      // exercises exactly the handshake a separate client would, because both
      // sides state their own hashes rather than sharing a variable.
      Outpost::ReplicatedWorldView worldView{MakeWorldViewDesc(config, universe, contentHash)};

      ClientApp client;
      // The shader table is a temporary and the client keeps it, which is safe
      // and worth saying: it is four spans over byte arrays with static storage
      // duration, so what survives the copy is what the spans point at.
      if (!client.Initialise(clientConfig, Outpost::ShaderTable(), worldView))
      {
        NEURON_LOG_ERROR("client failed to initialise");
        exitCode = 2;
        break;
      }
      exitCode = client.Run();
      // Client first, always: it must never render against a server that has
      // already gone (ADR-008 §6).
      client.Shutdown();
      break;
    }

    case Outpost::HostMode::Headless:
    {
      NEURON_LOG_INFO("headless: serving on port %u until Ctrl-C", static_cast<unsigned>(server.BoundPort()));
      // The standing proof that the server needs no client at all.
      while (server.Running() && !g_stopRequested)
      {
        Sleep(100);
      }
      break;
    }
    }
  }
  catch (const hresult_error& error)
  {
    const std::string text = std::format("fatal error 0x{:08x}: {}", static_cast<std::uint32_t>(static_cast<std::int32_t>(error.code())),
                                         to_string(error.message()));
    NEURON_LOG_ERROR("%s", text.c_str());
    ReportFatal(text);
    exitCode = 5;
  }
  catch (const std::exception& error)
  {
    const std::string text = std::string("fatal error: ") + error.what();
    NEURON_LOG_ERROR("%s", text.c_str());
    ReportFatal(text);
    exitCode = 5;
  }

  if (hostsServer)
  {
    server.Stop();
    server.Join();
    NEURON_LOG_INFO("server ran %u ticks (%u overruns)", server.TickCount(), server.OverrunCount());
  }

  NEURON_LOG_INFO("Outpost: Frontier exiting cleanly (%d)", exitCode);
  Log::Shutdown();
  return exitCode;
}
