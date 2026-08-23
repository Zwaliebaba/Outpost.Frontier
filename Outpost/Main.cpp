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
#include "DurableState.h"
#include "EconomyMessages.h"
#include "FleetSummary.h"
#include "OrderMessages.h"
#include "Orders.h"
#include "Relevance.h"
#include "SchemaHash.h"
#include "ShipClass.h"
#include "Snapshot.h"
#include "StationMessages.h"
#include "SummaryMessages.h"
#include "World.h"
#include "WorldRegistry.h"

#include "ClientApp.h"
#include "ClientConfig.h"

#include "DurableStore.h"
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

/*
 * Whether a fatal report may block on a human.
 *
 * The modal error box exists for the double-click case: a Windows-subsystem
 * exe has no console, so without it a broken install fails into silence. The
 * same box is a deadlock for every unattended launch -- a headless host, a
 * bake, a self test -- because nothing ever clicks OK. Three CI runs proved it
 * the expensive way: a missing content file at boot raised the box, and both
 * configurations sat mute for 45 minutes until the job budget killed them.
 *
 * True until the config is read, because before the config is read the
 * double-click case cannot be told apart from the unattended ones -- and CI
 * plants a valid config, so the path that reports without one belongs to a
 * person at a desk.
 */
bool g_fatalDialogAllowed = true;

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

  if (!g_fatalDialogAllowed)
  {
    return; // Unattended: the log and the exit code are the whole report.
  }
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
                     const Game::EconomyDef& _economy, std::uint64_t _sessionSeed,
                     const Outpost::ScenarioSettings& _scenario = {})
    : m_contentHash(_contentHash),
      m_scenario(_scenario),
      // Moved rather than copied: `WorldMeta` carries the HUD's display strings
      // as of main's UI slice, so a copy here is three allocations per boot for
      // nothing.
      m_worldMeta(std::move(_worldMeta)),
      m_universe(&_universe),
      m_economy(&_economy),
      m_sessionSeed(_sessionSeed),
      m_startAnchor(_universe.StartAnchorId())
  {
    Game::RegistryConfig config;
    config.sessionSeed = _sessionSeed;
    config.hostId = 0; // ADR-019: three roles, one process, one host.
    m_registry.Reset(&_universe, &_economy, config);

    /*
     * The **shard's** own hold on its start grid, and it is no longer standing
     * in for a player's (ADR-016 §7).
     *
     * It said "the session holds a viewer on the grid it serves" and named U3b
     * as what would generalise it to the player's actual view. N5 did that --
     * `ViewerOpened`/`ViewerClosed` below take a hold per commander on the grid
     * they are actually watching -- and this one stays for the case that has
     * nothing to do with a session: a headless shard with no client at all,
     * whose start grid would otherwise be torn down the moment its fleet docked
     * and rebuilt when they undocked. The counts sum, so a player watching this
     * grid adds to it rather than replacing it.
     */
    m_registry.AddViewer(m_startAnchor);
  }

  /*
   * The tick, and nothing else in it.
   *
   * **The scripted patrol that used to live here is gone (was Build Order S7).**
   * It sent the whole boot fleet to one of four waypoints every ten seconds so
   * that the fleet moved without anyone touching an input device -- which was
   * the only way, in S7, to answer "is the motion smooth at 144 Hz against
   * 20 Hz snapshots?" by looking at the screen. Nobody could give an order yet;
   * the fixture stood in for a player.
   *
   * A player can give one now, so the fixture had become the thing it was
   * standing in for -- and worse than absent, because a fleet that flies a
   * circuit nobody asked for is a fleet whose owner cannot tell their own
   * commands from the simulation's. **These are the player's ships and they
   * move on the player's orders.** The smoothness question is now asked the way
   * it will be asked forever: issue a Move and watch it.
   *
   * Nothing replaced it. A world where the fleet stands still until told
   * otherwise is the world the game actually is.
   */
  void AdvanceTick(std::uint32_t _tick) override
  {
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
  /*
   * The relevance hook (ADR-022 §4), forwarded and not implemented here.
   *
   * The ranking is game semantics -- who owns what, what is a landmark, how
   * near the camera something is -- so it lives in GameLogic beside the tables
   * it reads (`Game::RankRelevance`). What this method is, like everything else
   * in this class, is a line of wiring: translate the engine's neutral query
   * into the game's, borrow the grid, and hand back the order.
   *
   * A grid that is not live ranks as empty rather than refusing. The engine
   * reads an empty ranking as an empty grid, which is the truth about an anchor
   * nobody is standing on, and the presence-edge rules that decide where the
   * viewer lands instead are A16's rather than this function's.
   */
  [[nodiscard]] std::uint32_t RankRelevance(const InterestQuery& _query, std::vector<std::uint32_t>& _outRanked) override
  {
    _outRanked.clear();
    const Game::World* world = m_registry.Borrow(static_cast<Game::AnchorId>(_query.grid));
    if (world == nullptr)
    {
      return 0;
    }

    Game::RelevanceQuery query;
    query.viewer = _query.viewer;
    query.grid = static_cast<Game::AnchorId>(_query.grid);
    query.selection = _query.selection;
    query.focusXMetres = _query.focusXMetres;
    query.focusYMetres = _query.focusYMetres;
    query.viewHalfExtentMetres = _query.viewHalfExtentMetres;
    query.tick = _query.tick;

    return Game::RankRelevance(m_registry, *world, query, _outRanked);
  }

  /*
   * The records for exactly the entities the engine chose (ADR-022 §1, §8).
   *
   * Wiring again, with one decision in it: the **relationship** each record
   * carries is the viewer's, computed here because it is viewer-relative and
   * could not live on a world (§8b). It costs two bits of a byte the record
   * already had, where an owner id would have cost four bytes on every entity
   * every tick.
   *
   * An id the world does not hold is skipped rather than filled with a
   * placeholder: a ship can despawn between the ranking and this call, and a
   * placeholder would be a hull the client draws at the origin.
   */
  void WriteEntities(const SnapshotRequest& _request, std::span<const std::uint32_t> _ids,
                     std::vector<EntityRecord>& _outRecords) override
  {
    _outRecords.clear();
    const Game::World* world = m_registry.Borrow(static_cast<Game::AnchorId>(_request.grid));
    if (world == nullptr)
    {
      return;
    }

    _outRecords.reserve(_ids.size());
    for (const std::uint32_t id : _ids)
    {
      std::uint32_t slot = 0;
      if (!world->FindSlot(static_cast<Game::ShipId>(id), slot))
      {
        continue;
      }
      _outRecords.push_back(
        Game::MakeShipRecord(*world, slot, Game::RelationshipOf(m_registry, _request.viewer, static_cast<Game::ShipId>(id))));
    }
  }

  /*
   * The game's per-tick tail (ADR-022 §3b): the order-state records that
   * promote a client's ghost, and the session's order high-water mark.
   *
   * Opaque to the engine, like the summaries and for the same reason. The
   * sequence comes in with the request rather than off the world (ADR-022 §7):
   * it is the session's count of what *this* commander has had accepted, and a
   * world has no viewers to count for.
   */
  [[nodiscard]] bool WriteTickTail(const SnapshotRequest& _request, ByteWriter& _writer) override
  {
    /*
     * Borrowed and never stored, like every other use (ADR-019 §6.1). A grid
     * that has torn down under a viewer since the request answers `nullptr`,
     * and that is "no tail" rather than a crash: the presence-edge rules that
     * decide where the viewer lands instead are A16's, not this function's.
     */
    const Game::World* world = m_registry.Borrow(static_cast<Game::AnchorId>(_request.grid));
    return world != nullptr && Game::WriteTickTail(*world, _writer, _request.lastOrderSeqProcessed);
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
   * refusal in the vocabulary the player already reads.
   *
   * **It filters on `_viewer` now** (U3c-a), and what it replaced is worth
   * naming because it was a privacy hole rather than a simplification: this
   * walked the scripted patrol's ship list -- a fixture of the composition root,
   * since removed -- and so returned the same answer for every viewer. The
   * second commander to connect could have watched the first one's grid, and
   * the promise in the old comment ("when there are two, this filters on
   * `_viewer`") is the one being kept here.
   *
   * The rule itself moved into the registry rather than being rewritten here,
   * because presence is a question about the ship->location index and that is
   * the registry's to answer.
   */
  [[nodiscard]] std::uint16_t MayView(PlayerId _viewer, std::uint16_t _grid) override
  {
    const auto anchor = static_cast<Game::AnchorId>(_grid);
    if (m_registry.Borrow(anchor) == nullptr)
    {
      return static_cast<std::uint16_t>(Game::OrderReason::UnknownAnchor);
    }
    return m_registry.HasPresence(_viewer, anchor)
             ? static_cast<std::uint16_t>(0)
             : static_cast<std::uint16_t>(Game::OrderReason::NoPresence);
  }

  /*
   * The hold that stops a world being torn down under somebody's camera
   * (ADR-016 §7, N5).
   *
   * `MayView` says whether a view is legal; this pair is what makes one *cost*
   * something. `WorldRegistry::TearDownIdle` removes a grid when the last ship
   * leaves **and nobody is watching**, and until now the second clause was
   * decided by a hold the composition root took on its own start grid at boot.
   * A player looking at any other empty grid -- a station they have ships
   * docked at, a site whose field they are scouting -- was watching a world
   * torn down and rebuilt on every tick, because `RankRelevance` borrows the
   * grid (which spins it up) and the sweep at the end of the tick finds it
   * empty and unwatched. A whole `World`, its authored occupants and a site's
   * `BuildSiteField` layout, once a tick, for as long as they looked.
   *
   * It never changed what they *saw* -- a rebuilt grid resolves its field from
   * the calendar rather than from the instance that went away -- so what the
   * gap cost was the work, and a rule ADR-016 §7 states that nothing enforced.
   *
   * **The table is here rather than in the registry, and that is ADR-022 §1's
   * rule rather than a preference.** Which grid a commander is watching is
   * session state, the sim tier has no viewers, and a registry that held a
   * viewer *table* would be the session's business living one library too deep.
   * What the registry holds is a count -- how many holds are on this grid --
   * which is a fact about the grid and nothing about who is looking.
   */
  void ViewerOpened(PlayerId _viewer, std::uint16_t _grid) override
  {
    const auto anchor = static_cast<Game::AnchorId>(_grid);
    const auto held = std::find_if(m_viewerGrids.begin(), m_viewerGrids.end(),
                                   [_viewer](const ViewerGrid& _entry) { return _entry.viewer == _viewer; });
    if (held != m_viewerGrids.end() && held->anchor == anchor)
    {
      return; // The seam reports the whole answer, so the same answer twice is not two holds.
    }

    /*
     * Taken before the old one is let go. The order does not decide anything --
     * `TearDownIdle` sweeps once, inside `Tick`, so this pair is atomic with
     * respect to it -- and it is this way round because `AddViewer` is the call
     * that can fail: a grid nobody authored takes no hold, and finding that out
     * before letting go of a real one keeps the failure to a viewer with no
     * grid rather than a viewer with a stale one.
     */
    m_registry.AddViewer(anchor);
    if (held != m_viewerGrids.end())
    {
      m_registry.RemoveViewer(held->anchor);
      held->anchor = anchor;
      return;
    }
    m_viewerGrids.push_back(ViewerGrid{_viewer, anchor});
  }

  void ViewerClosed(PlayerId _viewer) override
  {
    const auto held = std::find_if(m_viewerGrids.begin(), m_viewerGrids.end(),
                                   [_viewer](const ViewerGrid& _entry) { return _entry.viewer == _viewer; });
    if (held == m_viewerGrids.end())
    {
      return; // A handshake that never completed never took one.
    }
    m_registry.RemoveViewer(held->anchor);
    m_viewerGrids.erase(held);
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
   * **Per viewer is the whole point** (ADR-017 §1), and as of U3c-a the filter
   * is a filter rather than the identity function. The old note here said the
   * privacy rule cost nothing today and that what mattered was that the
   * *question* was asked per viewer, because a roster reaching everyone would
   * be a leak nothing could catch until U3c first ran two clients. Asking per
   * viewer is what made this a one-word change when the answer started to
   * differ.
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
    const std::vector<Game::FleetSummary> summaries = m_registry.Summaries(_viewer);
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
      const std::vector<Game::RosterEntry> docked = m_registry.DockedFor(_viewer, row.anchor);
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

    /*
     * And the economy's three (ADR-024 §8, E3), measured on the same budget.
     *
     * `SiteStatus` for every site this commander has a fleet at, `CargoStatus`
     * once for everything they are carrying on grids, `BayStatus` per station
     * they have stored anything at. All three are cheap enough to be
     * unconditional and all three are dropped rather than truncated when the
     * budget runs out, because a half-written record is a lie about a hold.
     */
    std::vector<Game::AnchorId> sites;
    for (const Game::FleetSummary& row : summaries)
    {
      if (row.state != Game::FleetState::OnGrid || !m_registry.FieldAt(row.anchor).Exists())
      {
        continue;
      }
      if (std::find(sites.begin(), sites.end(), row.anchor) != sites.end())
      {
        continue;
      }
      const std::size_t bytes =
        Game::SUMMARY_RECORD_HEADER_BYTES + Game::SiteStatusBytes(m_registry.FieldAt(row.anchor).clusterCount);
      if (bytes > budget || rosters.size() + sites.size() + 2 >= Game::MAX_SUMMARY_RECORDS)
      {
        continue;
      }
      budget -= bytes;
      sites.push_back(row.anchor);
    }

    // Owner-only, and keyed by the viewer rather than filtered afterwards: the
    // registry is asked what *this* player has, so there is no path on which
    // somebody else's could be written into this frame.
    const std::vector<Game::CargoStatusRow> cargo = m_registry.CargoFor(_viewer);
    const bool withCargo =
      !cargo.empty() && Game::SUMMARY_RECORD_HEADER_BYTES + Game::CargoStatusBytes(cargo.size()) <= budget;
    if (withCargo)
    {
      budget -= Game::SUMMARY_RECORD_HEADER_BYTES + Game::CargoStatusBytes(cargo.size());
    }

    std::vector<Game::AnchorId> bays;
    for (const Game::StationBay& bay : m_registry.Bays())
    {
      if (bay.owner != _viewer || bay.TotalUnits() == 0)
      {
        continue;
      }
      const std::size_t bytes = Game::SUMMARY_RECORD_HEADER_BYTES + Game::BAY_STATUS_BYTES;
      if (bytes > budget || rosters.size() + sites.size() + bays.size() + 2 >= Game::MAX_SUMMARY_RECORDS)
      {
        break;
      }
      budget -= bytes;
      bays.push_back(bay.station);
    }

    // And the refineries, on the Bays' terms: this player's, in anchor order,
    // and only where there is something to say (ADR-024 §6, E4b).
    std::vector<Game::AnchorId> refineries;
    for (const Game::AnchorId station : m_registry.RefineriesFor(_viewer))
    {
      const Game::RefineryStatusRow row = m_registry.RefineryStatusFor(_viewer, station);
      const std::size_t bytes = Game::SUMMARY_RECORD_HEADER_BYTES + Game::RefineryStatusBytes(row.jobs.size());
      if (bytes > budget
          || rosters.size() + sites.size() + bays.size() + refineries.size() + 2 >= Game::MAX_SUMMARY_RECORDS)
      {
        break;
      }
      budget -= bytes;
      refineries.push_back(station);
    }

    const auto records = static_cast<std::uint8_t>(rosters.size() + sites.size() + bays.size() + refineries.size()
                                                   + (withCargo ? 1u : 0u) + 1u);
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
          !Game::WriteStationRoster(anchor, m_registry.DockedFor(_viewer, anchor), _writer))
      {
        return false;
      }
    }
    for (const Game::AnchorId anchor : sites)
    {
      if (!Game::BeginSummaryRecord(Game::SummaryKind::SiteStatus, _writer) ||
          !Game::WriteSiteStatus(m_registry.SiteStatusFor(anchor), _writer))
      {
        return false;
      }
    }
    if (withCargo && (!Game::BeginSummaryRecord(Game::SummaryKind::CargoStatus, _writer) ||
                      !Game::WriteCargoStatus(cargo, _writer)))
    {
      return false;
    }
    for (const Game::AnchorId station : bays)
    {
      const Game::StationBay* bay = m_registry.Bay(_viewer, station);
      if (bay == nullptr || !Game::BeginSummaryRecord(Game::SummaryKind::BayStatus, _writer) ||
          !Game::WriteBayStatus(station, bay->oreUnits, bay->alloyUnits, _writer))
      {
        return false;
      }
    }
    for (const Game::AnchorId station : refineries)
    {
      if (!Game::BeginSummaryRecord(Game::SummaryKind::RefineryStatus, _writer) ||
          !Game::WriteRefineryStatus(m_registry.RefineryStatusFor(_viewer, station), _writer))
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
  [[nodiscard]] OrderVerdict ApplyOrderBytes(PlayerId _player, std::uint32_t, std::span<const std::uint8_t> _payload) override
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
      return ApplyStationCommand(_player, reader, _payload.size());
    }

    Game::OrderSubmit order;
    if (!Game::ReadOrderSubmit(reader, order))
    {
      NEURON_LOG_WARNING("malformed order payload (%zu bytes)", _payload.size());
      return Malformed();
    }

    /*
     * Whose ships these are, before anything is asked to move them
     * (ADR-018 D5, U3c-b). `Validate.cpp` has carried the answer to why
     * `NotOwned` existed and was unreachable since the MVP -- "there is one
     * player and every ship is theirs" -- and this is the slice where that
     * stops being true.
     *
     * Checked HERE rather than in the validator because a world knows only its
     * own ships and must never learn who owns one (ADR-018 D2). The registry
     * keeps the index, the composition root is what can see both, so the
     * refusal is composed here and travels through the engine as a number it
     * does not read (ADR-014 §3).
     */
    for (std::uint16_t index = 0; index < order.shipCount; ++index)
    {
      if (m_registry.OwnerOf(order.shipIds[index]) != _player)
      {
        OrderVerdict refused;
        refused.accepted = false;
        refused.reasonCode = static_cast<std::uint16_t>(Game::OrderReason::NotOwned);
        refused.orderSeq = order.orderSeq;
        return refused;
      }
    }

    /*
     * And the grid the order is FOR, which is where the ships are rather than
     * where the shard happens to start.
     *
     * This used to borrow the shard's *start* anchor and nothing else. With one
     * commander that was right by construction; with two it sent every order to
     * the first commander's grid, so a second commander's fleet could not be
     * ordered at all -- and the ownership check above would have been the only
     * thing standing between them and ordering somebody else's.
     *
     * The first named ship decides. Ships in the order that are somewhere else
     * are refused `UnknownShip` by that world's own validator, which is the
     * honest answer: they are not on the grid this order is about.
     */
    Game::AnchorId where = Game::INVALID_ID;
    if (order.shipCount == 0 || !m_registry.LocationOf(order.shipIds[0], where))
    {
      OrderVerdict refused;
      refused.accepted = false;
      refused.reasonCode = static_cast<std::uint16_t>(order.shipCount == 0 ? Game::OrderReason::EmptySelection
                                                                          : Game::OrderReason::UnknownShip);
      refused.orderSeq = order.orderSeq;
      return refused;
    }
    Game::World* world = m_registry.Borrow(where);
    if (world == nullptr)
    {
      OrderVerdict refused;
      refused.accepted = false;
      refused.reasonCode = static_cast<std::uint16_t>(Game::OrderReason::UnknownShip);
      refused.orderSeq = order.orderSeq;
      return refused;
    }

    const Game::OrderVerdict decided = world->SubmitOrder(order);

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
   * why this borrows no world at all.
   */
  [[nodiscard]] OrderVerdict ApplyStationCommand(PlayerId _player, Neuron::ByteReader& _reader, std::size_t _payloadBytes)
  {
    Game::StationCommand command;
    if (!Game::ReadStationCommand(_reader, command))
    {
      NEURON_LOG_WARNING("malformed station command (%zu bytes)", _payloadBytes);
      return Malformed();
    }

    // Whose command it is, carried rather than assumed: a transfer verb moves
    // ore into or out of *this* player's Bay (ADR-024 §5b), and a registry that
    // had to guess would be guessing about property.
    const Game::OrderVerdict decided = m_registry.SubmitStationCommand(_player, command);

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

  /// The balance this simulation runs on. Exposed for the same reason
  /// `ContentHash` is: a diagnostic that built its own content would be
  /// measuring something the shard is not.
  [[nodiscard]] const Game::EconomyDef& Economy() const noexcept { return *m_economy; }

  /*
   * Persistence (ADR-025 §2). Three lines of glue, and that is the whole point
   * of the arrangement: the format is GameLogic's, the file is NeuronServer's,
   * and this is the only place that knows they are about each other.
   */
  [[nodiscard]] bool WriteDurableState(ByteWriter& _writer) override { return Game::WriteDurableState(m_registry, _writer); }

  [[nodiscard]] std::uint64_t DurableHash() const override { return Game::DurableHash(m_registry); }

  /*
   * A load **replaces** a shard's state; it does not merge with it.
   *
   * So the registry goes back to freshly-reset before a byte is applied --
   * including the start grid the constructor spun up under the session's
   * viewer, whose authored occupants a load would otherwise find already
   * standing there. Then the viewer goes back, because a session still holds
   * the grid it serves (ADR-016 §7).
   */
  [[nodiscard]] bool ReadDurableState(std::span<const std::uint8_t> _state) override
  {
    Game::RegistryConfig config;
    config.sessionSeed = m_sessionSeed;
    config.hostId = 0;
    m_registry.Reset(m_universe, m_economy, config);

    ByteReader reader(_state);
    std::vector<Game::PersistenceDiagnostic> diagnostics;
    const bool loaded = Game::ReadDurableState(reader, m_registry, diagnostics);
    for (const Game::PersistenceDiagnostic& diagnostic : diagnostics)
    {
      NEURON_LOG_ERROR("durable state: %s", diagnostic.Text("shard.snapshot").c_str());
    }

    m_registry.AddViewer(m_startAnchor);
    if (loaded)
    {
      AdoptReloadedFleet();
    }
    return loaded;
  }

  /*
   * A commander this shard has not met (ADR-018 D5, U3c-b).
   *
   * Two refusals before anything is spawned, and both are the same mistake in
   * different clothes -- giving somebody a fleet they already have.
   *
   *   - A commander with ships anywhere is not new. A reloaded shard knows
   *     them, and spawning here would hand them a second fleet on every
   *     restart. This is `OpenShardState`'s rule about the boot fleet, applied
   *     per commander instead of per shard.
   *   - A commander with nowhere to go gets nothing rather than a fleet on top
   *     of somebody else's. `HomeAnchorFor` returning `INVALID_ID` means the
   *     universe ran out of disjoint grids, which is a shard that is full.
   *
   * **What a new commander is given is a placeholder, and is meant to be.**
   * They get the same authored fleet the shard boots with, on a grid of their
   * own. That generalises the existing rule rather than inventing a policy --
   * but onboarding is a design question nobody has answered, and this is the
   * one line to change when somebody does.
   */
  /*
   * The grid this commander's session opens on (ADR-018 D5, U3c-b).
   *
   * **Where most of their fleet is**, not merely where some of it is, and that
   * distinction is not hypothetical: the self test's local checks leave player
   * one owning a Miner or two at sites and stations all over the universe, so
   * "the lowest anchor they have a ship on" put the shard's own commander on a
   * grid holding two hulls instead of the forty-hull boot fleet. Most-ships
   * wins; `Summaries` is sorted by anchor and the comparison is strict, so a
   * tie goes to the lowest anchor and two runs agree.
   *
   * A placeholder in the same sense the starting fleet is: when there is a
   * screen to choose from, the choice is the player's.
   */
  [[nodiscard]] WorldMeta WorldFor(PlayerId _player) override
  {
    Game::AnchorId home = m_startAnchor;
    std::uint16_t most = 0;
    for (const Game::FleetSummary& row : m_registry.Summaries(_player))
    {
      if (row.shipCount > most)
      {
        most = row.shipCount;
        home = row.anchor;
      }
    }
    if (home == m_startAnchor)
    {
      return m_worldMeta; // The shard's own grid, described once at boot.
    }

    /*
     * Somebody else's home, described in the same neutral terms
     * (ADR-009 §8) -- the grid's number AND its origin together, because a
     * client given one without the other draws its whole world in the wrong
     * place.
     */
    const Game::Anchor* anchor = m_universe != nullptr ? m_universe->FindAnchor(home) : nullptr;
    if (anchor == nullptr)
    {
      return m_worldMeta;
    }

    WorldMeta meta = m_worldMeta;
    meta.worldId = anchor->system;
    meta.gridAnchor = home;
    meta.anchorX = anchor->origin.x;
    meta.anchorY = anchor->origin.y;
    const Game::SolarSystem* system = m_universe->FindSystem(anchor->system);
    meta.worldName = system != nullptr ? system->name : "?";
    return meta;
  }

  void PlayerJoined(PlayerId _player) override
  {
    if (!m_registry.Summaries(_player).empty())
    {
      NEURON_LOG_INFO("player %u is known to this shard already; no starting fleet", _player);
      return;
    }

    const Game::AnchorId home = HomeAnchorFor(_player);
    if (home == Game::INVALID_ID)
    {
      NEURON_LOG_WARNING("player %u joined and there was no free grid to put them on", _player);
      return;
    }

    /*
     * ONE wing, not the boot fleet's eight, and the reason is arithmetic rather
     * than generosity. A full snapshot carries `MAX_SHIPS_PER_SNAPSHOT` = 43
     * ships; the boot fleet is forty hulls and its grid also holds the station.
     * A second forty-hull fleet on a grid with any authored occupants of its
     * own would sit on that cap, and a grid that overflows the snapshot is the
     * failure the interest/delta slice exists to fix (ADR-022, D6) rather than
     * one to walk into here.
     *
     * Five hulls is enough for every claim U3c makes -- they are owned, they
     * are somewhere, and they are not the other commander's.
     */
    SpawnFleetFor(_player, home, 1);
    NEURON_LOG_INFO("player %u joined: starting fleet on anchor %u", _player, static_cast<unsigned>(home));
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


  /// The fleet, spawned into the start grid with ids the registry allocated
  /// (ADR-018 D6a). It stands where it is put; nothing moves it but its
  /// commander.
  void SpawnStartingFleet();

  /// The same fleet, for any commander, on any grid. Where the ships are put
  /// shard's own boot fleet -- it is a scripted demo of the start grid, not a
  /// thing that should start flying somebody else's hulls around.
  void SpawnFleetFor(Neuron::PlayerId _owner, Game::AnchorId _anchor, std::size_t _wings);

  /// Where a brand-new commander is put down. Deterministic and **disjoint**:
  /// U3c serves two commanders on separate grids, because two full fleets on
  /// one grid exceed the full-snapshot cap by arithmetic.
  [[nodiscard]] Game::AnchorId HomeAnchorFor(Neuron::PlayerId _player) const;

  /*
   * Says what came back (ADR-025 §1).
   *
   * It used to hand the reloaded fleet to the scripted patrol, which is where
   * the name comes from and why it walked the ids at all. With the patrol gone
   * nothing needs adopting -- a reloaded ship is already the registry's and
   * already where the store said it was -- and what is left is the count, which
   * is the first thing to look at when a restart comes back empty.
   *
   * The station is excluded from it for the reason it was excluded before:
   * `IsAuthoredOccupant` is what tells a hull the player owns from the
   * structure the anchor came with.
   */
  void AdoptReloadedFleet();

  /// Gives the worlds to whichever thread runs them next (ADR-007 §7). The last
  /// thing the composition root does to the simulation, and the reason the sim
  /// thread's first tick adopts rather than trips.
  void HandOff() noexcept { m_registry.HandOff(); }

private:
  std::uint64_t m_contentHash = 0;
  WorldMeta m_worldMeta;

  /// The content this simulation was built from, kept because the self test
  /// bakes a small universe of its own to drive the economy loop through and
  /// has to use the *same* balance the shard is running (E3's G0 scenario).
  const Game::EconomyDef* m_economy = nullptr;

  /// And the universe beside it, kept for the same reason plus one: a reload
  /// resets the registry, and a reset needs both halves of the content back.
  const Game::UniverseDef* m_universe = nullptr;

  /// The states the shipped world does not produce, asked for on purpose
  /// (ADR-012). All-zero is the world as it has always been.
  Outpost::ScenarioSettings m_scenario;

  /// The seed a reset has to reproduce. A reloaded shard whose grids were
  /// seeded differently would be the same universe with different randomness in
  /// it, which is a subtler way of losing state than losing it.
  std::uint64_t m_sessionSeed = 0;

  Game::WorldRegistry m_registry;
  Game::AnchorId m_startAnchor = Game::INVALID_ID;

  /*
   * Which grid each commander is watching, and therefore which hold is theirs
   * (ADR-016 §7, N5).
   *
   * One row per *live session*, so it is bounded by `server.maxSessions` and a
   * linear scan is the whole lookup. A vector rather than a map for that
   * reason, and because the order it iterates in has to be stable: this is read
   * on the Sim thread beside the registry it is about.
   *
   * It exists to answer one question the engine cannot: *what did this viewer
   * hold before?* `ViewerOpened` reports the whole answer rather than a delta,
   * which is what makes a missed release impossible -- but only if somebody
   * remembers the previous answer, and the engine deliberately does not know
   * that holds exist at all.
   */
  struct ViewerGrid
  {
    Neuron::PlayerId viewer = Neuron::INVALID_PLAYER_ID;
    Game::AnchorId anchor = Game::INVALID_ID;
  };
  std::vector<ViewerGrid> m_viewerGrids;

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

/*
 * Call signs kept back for wings the player composes in a hangar (ADR-017 §6).
 *
 * Here rather than beside a name generator, because these are the same kind of
 * thing as the eight above: content, in the composition root, in the table that
 * would move wholesale the day a fleet file exists. A wing the player makes is
 * as much a part of this game's vocabulary as TALON is -- it is only later.
 *
 * **Seven, and the number is derived rather than picked.** The roster draws at
 * most `MAX_ROSTER_ROWS` rows and the starting fleet already spends eight of
 * them, so this is what is left before a wing would be created with nowhere to
 * appear. Running out is not an error -- `EnsureWingName` falls back to a dull
 * generated word -- but running out *quietly* while rows were still free would
 * be, which is why the count is checked below rather than trusted.
 *
 * In the same register as the eight: one word, no digits, nothing that reads as
 * a serial number, because the player is going to be looking at these beside
 * ANVIL and SPUR on the same panel.
 */
constexpr const char* SPARE_WING_NAMES[] = {"VERGE", "CINDER", "HALYARD", "TESSERA", "QUILL", "BRAMBLE", "SLATE"};

static_assert(1u + WING_COUNT + std::size(SPARE_WING_NAMES) <= Neuron::MAX_ROSTER_ROWS,
              "more call signs than the roster has rows to draw them on: the extra wings would be "
              "created, carry ships, and never appear");
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

/// The unspent ones, in the order the hangar will hand them out. A second
/// function rather than a second argument to the first, because the two lists
/// are indexed differently: that one by `WingId`, this one by nothing at all.
[[nodiscard]] std::vector<std::string> SpareWingNames()
{
  return std::vector<std::string>{std::begin(SPARE_WING_NAMES), std::end(SPARE_WING_NAMES)};
}

Game::AnchorId UniverseSimulation::HomeAnchorFor(Neuron::PlayerId _player) const
{
  /*
   * The first commander gets the start grid, and every other gets the next
   * anchor nobody is standing on.
   *
   * Disjoint by construction rather than by luck, because U3c is scoped to
   * disjoint grids: two full fleets on one grid exceed the full-snapshot cap by
   * arithmetic (the interest/delta slice, D6, is what lifts that). "Nobody is
   * standing on it" is asked of the registry rather than tracked here, so a
   * grid that emptied is available again and this holds no state to go stale.
   *
   * Deterministic: the universe's anchors in bake order, which is the order
   * everything else iterates them in. Two runs of the same script put the same
   * commander in the same place.
   */
  if (_player == Neuron::SOLE_PLAYER_ID)
  {
    return m_startAnchor;
  }
  if (m_universe == nullptr)
  {
    return Game::INVALID_ID;
  }

  for (const Game::SolarSystem& system : m_universe->systems)
  {
    for (const Game::Anchor& anchor : system.anchors)
    {
      if (anchor.id == m_startAnchor)
      {
        continue; // Player one's: the start grid is the boot fleet's home.
      }
      /*
       * A grid somebody's ships are standing on belongs to them.
       *
       * Asked through the owner index rather than by counting hulls against
       * authored occupants, which is what this would have had to do a slice
       * ago: "is anybody here" is exactly the question U3c-a taught the
       * registry to answer, and the furniture drops out for free because it
       * belongs to `INVALID_PLAYER_ID`.
       *
       * `Peek` rather than `Borrow`: choosing a home must not spin up every
       * grid it looks at.
       */
      const Game::World* world = m_registry.Peek(anchor.id);
      if (world != nullptr)
      {
        bool occupied = false;
        for (const Game::ShipId id : world->Ids())
        {
          if (m_registry.OwnerOf(id) != Neuron::INVALID_PLAYER_ID)
          {
            occupied = true;
            break;
          }
        }
        if (occupied)
        {
          continue;
        }
      }
      return anchor.id;
    }
  }
  return Game::INVALID_ID;
}

void UniverseSimulation::AdoptReloadedFleet()
{
  const Game::World* world = m_registry.Peek(m_startAnchor);
  if (world == nullptr)
  {
    return;
  }
  /*
   * Counted rather than adopted.
   *
   * This function used to hand the reloaded fleet to the scripted patrol, which
   * is why it walked the ids at all. With the patrol gone there is nothing to
   * adopt them *into* -- a reloaded ship is already the registry's, already
   * owned, and already where the store said it was. What is left is worth
   * keeping on its own: a line in the log saying the shard came back with a
   * fleet on it, which is the first thing to look at when a restart comes back
   * empty.
   */
  std::size_t mobile = 0;
  const std::span<const Game::ShipId> ids = world->Ids();
  for (const Game::ShipId id : ids)
  {
    mobile += m_registry.IsAuthoredOccupant(m_startAnchor, id) ? 0u : 1u;
  }
  NEURON_LOG_INFO("reloaded shard: %zu ship(s) on the start grid, %zu of them the commander's", ids.size(), mobile);
}

void UniverseSimulation::SpawnStartingFleet()
{
  // The shard's own boot fleet: player one's, on the start grid, standing where
  // it is put until its commander says otherwise. `SpawnFleetFor` is the general
  // case this is one call to.
  SpawnFleetFor(Neuron::SOLE_PLAYER_ID, m_startAnchor, WING_COUNT);

  /*
   * And then whatever the scenario asked for, which by default is nothing at
   * all (`ScenarioSettings`).
   *
   * **Second, and never instead.** The starting fleet is the shipped world and
   * the replay hash is taken over it; a knob that ran before it, or in place of
   * it, would be a config option quietly rewriting a determinism baseline. This
   * only ever *adds*, on a grid the shipped world does not touch.
   *
   * The anchor comes from `HomeAnchorFor`, which already answers "the next
   * anchor in bake order nobody is standing on" for a second commander. Reused
   * rather than re-derived: the rule is tested, deterministic, and the config
   * is spared an anchor id that a re-bake could move under it. The second fleet
   * belongs to the *same* commander, which is what makes it a fleet elsewhere
   * rather than a rival -- two commanders is U3c's scenario and has its own
   * gate.
   *
   * Its ships stand where they are put, which is now true of every fleet this
   * shard spawns rather than a property of this one: the scripted patrol that
   * used to fly the boot fleet a circuit is gone. It was worth naming here
   * while it existed, because a fleet crossing a grid boundary on a timer makes
   * the location block's count flicker -- a moving target to check a readout
   * against.
   */
  if (m_scenario.secondFleetWings > 0)
  {
    const Game::AnchorId elsewhere = HomeAnchorFor(Neuron::SOLE_PLAYER_ID + 1);
    if (elsewhere == Game::INVALID_ID)
    {
      NEURON_LOG_WARNING("scenario: no free anchor for a second fleet; the universe has none spare");
      return;
    }

    // The grid has to be spun up before anything can stand on it, and it has to
    // stay up to be watchable -- which is what a viewer is for.
    m_registry.AddViewer(elsewhere);
    SpawnFleetFor(Neuron::SOLE_PLAYER_ID, elsewhere, m_scenario.secondFleetWings);
    NEURON_LOG_INFO("scenario: a second fleet of %u wing(s) on anchor %u", m_scenario.secondFleetWings, elsewhere);
  }
}

void UniverseSimulation::SpawnFleetFor(Neuron::PlayerId _owner, Game::AnchorId _anchor, std::size_t _wings)
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
    if (wing >= _wings)
    {
      break;
    }
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

      const Game::ShipId id = m_registry.Spawn(_anchor, spawn, _owner);
      if (id != Game::INVALID_SHIP_ID)
      {
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

/*
 * Which hull class each mesh file draws.
 *
 * At file scope because **two** things need it and they must not drift: the
 * world view's `renderClassByHull`, which turns a hull into a classId, and the
 * client config's per-mesh silhouette radii, which turn a classId back into how
 * big that hull is. Matching on the authored file name rather than on position
 * means reordering the list in `Outpost.json` reorders the meshes and nothing
 * breaks.
 */
/*
 * ... and which signal-light fixture it carries (ADR-006 §6a).
 *
 * In the same table because it is the same sentence -- "this file is the
 * station" -- said once. A flyable hull gets navigation lights whatever its
 * size; the two things that never move get the fixtures that suit what they
 * are, which is the only place in this build where a mesh's *identity* reaches
 * the renderer at all, and it reaches it as one enumerator per class rather
 * than as a list of lamp positions the engine would then have to be trusted
 * with (`SignalLamp.h` derives those from the mesh's own bounding box).
 */
constexpr struct
{
  Game::HullClass hullClass;
  std::string_view meshFile;
  Neuron::LampRig lampRig;
} MESH_FOR_HULL[] = {
  {Game::HullClass::Interceptor, "Interceptor.obj", Neuron::LampRig::Ship},
  {Game::HullClass::Bomber, "Bomber.obj", Neuron::LampRig::Ship},
  {Game::HullClass::Corvette, "Corvette.obj", Neuron::LampRig::Ship},
  {Game::HullClass::Frigate, "Frigate.obj", Neuron::LampRig::Ship},
  {Game::HullClass::Hauler, "Hauler.obj", Neuron::LampRig::Ship},
  {Game::HullClass::Miner, "Miner.obj", Neuron::LampRig::Ship},
  {Game::HullClass::Carrier, "Carrier.obj", Neuron::LampRig::Ship},
  {Game::HullClass::Battleship, "Battleship.obj", Neuron::LampRig::Ship},
  {Game::HullClass::Structure, "Structure.obj", Neuron::LampRig::Station},
  {Game::HullClass::Gate, "Stargate.obj", Neuron::LampRig::Gate},
};

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
 * The player's settings on the way out (ADR-012 §A3, N2).
 *
 * **The one file this program writes**, and it is written here rather than at
 * the moment a wing is renamed for a reason worth stating: a rename is a
 * keystroke and a save is a file rename, and doing the second on every one of
 * the first would put the settings file in the path of a fast typist. Shutdown
 * is when the session's answer is final.
 *
 * The cost of that choice is that a session killed rather than closed loses the
 * names it minted. That is the right trade while the layer holds call signs --
 * they are cheap to redo, and `EnsureWingName` already covers a wing whose name
 * did not survive -- and it is the thing to revisit first when the settings
 * screen lands, because a display mode the player cannot get back to is not.
 *
 * A failure is a log line and nothing more. Preferences that could not be saved
 * are never worth failing a shutdown over, and the exit code belongs to what the
 * client did rather than to what its settings file did afterwards.
 */
void SaveUserLayer(Outpost::AppConfig& _config, const Outpost::AppConfig& _shipped, const Outpost::ConfigPaths& _paths,
                   const Outpost::ReplicatedWorldView& _worldView, const Neuron::ClientApp& _client)
{
  _config.wings.clear();
  for (const std::pair<Game::WingId, std::string>& wing : _worldView.PlayerWingNames())
  {
    _config.wings.push_back(Outpost::WingName{static_cast<std::uint32_t>(wing.first), wing.second});
  }

  /*
   * And what the settings screen changed (N3) -- the write-back N2 recorded as
   * owed, in ADR-012 §A3's own words: *"the display and audio families are
   * written the moment something changes them and nothing does yet"*.
   *
   * Mapped field by field rather than by holding a pointer to the client's
   * config, which is `MakeClientConfig` run backwards and deliberately so: the
   * two directions sit in one file where a field added to one and forgotten in
   * the other is visible, and the client never learns that a settings file
   * exists (ADR-014).
   *
   * Asked only when the player touched the screen. An untouched session leaves
   * the file exactly as it found it, because a layer that records *changes*
   * should not be rewritten by a session that made none.
   */
  if (_client.SettingsChanged())
  {
    const Neuron::ClientConfig& live = _client.Config();
    _config.client.ui.scale = live.uiScale;
    _config.client.ui.palette = live.uiPalette;
    _config.client.ui.highContrast = live.uiHighContrast;
    _config.client.ui.reduceMotion = live.uiReduceMotion;
    _config.client.ui.alwaysShowHullBars = live.uiAlwaysShowHullBars;
    _config.client.input.handedness = live.uiHandedness;
    _config.client.input.longPressSeconds = live.longPressSeconds;
    _config.client.renderer.vsync = live.vsync;
    _config.client.renderer.frameCap = live.frameCap;
  }

  std::string error;
  if (!Outpost::SaveUserSettings(_config, _shipped, _paths, error))
  {
    NEURON_LOG_WARNING("user settings not saved: %s", error.c_str());
  }
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
                                                     std::uint64_t _contentHash, const Game::EconomyDef& _economy)
{
  Outpost::ReplicatedWorldView::Desc desc;
  desc.renderClassByHull.assign(Game::HULL_CLASS_COUNT, Outpost::ReplicatedWorldView::INVALID_RENDER_CLASS);
  desc.contentHash = _contentHash;
  desc.wingNames = WingNames();
  desc.spareWingNames = SpareWingNames();

  /*
   * And what the player has called them since (ADR-012 §3).
   *
   * Widened on the way in and narrowed here, which is the seam working rather
   * than a wart: `AppConfig` is spelled without game types so the composition
   * root's own test suite can compile it, so `WingId` is put back on at the one
   * point that knows both -- this project, which is the same reason
   * `renderClassByHull` is built here.
   */
  desc.savedWingNames.reserve(_config.wings.size());
  for (const Outpost::WingName& wing : _config.wings)
  {
    desc.savedWingNames.emplace_back(static_cast<Game::WingId>(wing.wing), wing.name);
  }

  // Which grid this client watches, so a station has a number the client can
  // address it by (ADR-017 8). The same anchor the `Welcome` carries; taken
  // from the universe here because the composition root builds both.
  desc.gridAnchor = _universe.universe.StartAnchorId();

  // The hold sizes and ore volumes the client needs to answer "is there room"
  // the way the authority answers it. Borrowed: the definition outlives the
  // view, and copying it would be a second copy of the balance file.
  desc.economy = &_economy;

  /*
   * And the universe itself, for the strategic map (ADR-018 D14, U5).
   *
   * Borrowed for `economy`'s reason -- it is loaded once here and outlives every
   * view -- and it is a pointer rather than pre-derived `Desc` fields because
   * the map asks two different questions of it: a graph handed over once at
   * boot, which could have been baked into a field, and a route solved wherever
   * the player points, which could not.
   */
  desc.universe = &_universe.universe;

  /*
   * What each anchor is called (ADR-017 1, ADR-016 9).
   *
   * **Every kind, because a fleet can stand on any of them.** The table held
   * stations only until a location block first had to name a grid that was not
   * one, and drew a question mark instead -- which a screenshot caught and no
   * test could, because `?` is what the panel is *supposed* to draw for a name
   * the content does not have.
   *
   * Walked from the anchor table rather than from the object lists, because the
   * anchor is what the wire carries and the object is what has a name: an
   * anchor's `owner` is a per-system id, so the join has to happen here. A site
   * has no name of its own in the bake, so it takes its system's -- "where the
   * field is" is the useful answer and the only one the content can give.
   */
  for (const Game::SolarSystem& system : _universe.universe.systems)
  {
    for (const Game::Anchor& anchor : system.anchors)
    {
      std::string name;
      switch (anchor.kind)
      {
      case Game::AnchorKind::Station:
        if (const Game::Station* station = _universe.universe.FindStation(system.id, anchor.owner); station != nullptr)
        {
          name = station->name;
        }
        break;
      case Game::AnchorKind::Planet:
        for (const Game::Celestial& body : system.celestials)
        {
          if (body.id == anchor.owner)
          {
            name = body.name;
            break;
          }
        }
        break;
      case Game::AnchorKind::Gate:
        for (const Game::Gate& gate : system.gates)
        {
          if (gate.id == anchor.owner)
          {
            name = gate.name;
            break;
          }
        }
        break;
      case Game::AnchorKind::Site:
        name = system.name; // A field has no authored name; its system is the answer.
        break;
      }

      if (name.empty())
      {
        continue; // Something the bake did not place. Drawn unnamed, honestly.
      }
      desc.anchorNames.push_back(Outpost::ReplicatedWorldView::AnchorName{anchor.id, std::move(name)});
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
/// What `OpenShardState` decided, because "did it load" has three answers and
/// two of them are not failures.
enum class ShardState : std::uint8_t
{
  Fresh = 0,   ///< Nothing was there, or nothing is persisted: build from content.
  Loaded = 1,  ///< The shard came back; do not spawn a starting fleet on top of it.
  Refused = 2  ///< A guard failed, and the shard must not start (ADR-025 §6).
};

/*
 * ADR-025 §6's boot, in its order: the header and its guards, the snapshot and
 * its proof, then the journal on top.
 *
 * All of it before the host starts, because a shard that began ticking and then
 * discovered it had a past would have to undo the ticks -- and because a
 * refusal has to happen while there is still nothing to lose.
 */
[[nodiscard]] ShardState OpenShardState(const Outpost::AppConfig& _config, std::uint64_t _universeHash,
                                        std::uint64_t _economyHash, UniverseSimulation& _simulation, DurableStore& _store)
{
  if (!_config.persistence.enabled)
  {
    // Said out loud rather than left to be noticed. A shard that persists
    // nothing is a real configuration -- it is what every slice before E4a ran
    // as -- and it is not the same thing as one that failed to.
    NEURON_LOG_INFO("persistence is off: this shard keeps nothing across a restart");
    return ShardState::Fresh;
  }

  DurableStoreDesc desc;
  desc.directory = Outpost::ResolveWritablePath(_config.persistence.directory);
  desc.hostId = 0; // ADR-019: three roles, one process, one host.
  desc.universeHash = _universeHash;
  desc.economyHash = _economyHash;

  DurableLoadReport report;
  if (!_store.Open(desc, report))
  {
    NEURON_LOG_ERROR("shard state refused: %s", report.message.c_str());
    return ShardState::Refused;
  }
  if (report.economyChanged)
  {
    // Recorded, compared, never fatal (ADR-025 §6.2): retuning a hold size must
    // not invalidate a shard.
    NEURON_LOG_WARNING("the economy has been retuned since this shard was written; loading anyway");
  }

  if (!_store.Snapshot().empty())
  {
    if (!_simulation.ReadDurableState(_store.Snapshot()))
    {
      NEURON_LOG_ERROR("shard state refused: the snapshot at %s did not read", _store.SnapshotPath().c_str());
      return ShardState::Refused;
    }

    /*
     * And the proof (ADR-025 §1a).
     *
     * The store recorded a number it cannot compute and this is the only moment
     * the comparison means anything: the state is in, and if the two disagree
     * then something between the write and the read changed what came back --
     * which is precisely the failure a checksum cannot see, because the bytes
     * were fine and their meaning was not.
     */
    const std::uint64_t reloaded = _simulation.DurableHash();
    if (reloaded != report.snapshotDurableHash)
    {
      NEURON_LOG_ERROR("shard state refused: the snapshot claims durable hash %016llx and reloaded as %016llx",
                       static_cast<unsigned long long>(report.snapshotDurableHash), static_cast<unsigned long long>(reloaded));
      return ShardState::Refused;
    }
  }

  /*
   * The journal, on top of the snapshot.
   *
   * There are no record kinds yet -- E4a persists through the snapshot, and the
   * per-outcome records that narrow the loss window to a second are the next
   * step -- so this refuses anything it finds rather than skipping it. A record
   * a build does not understand is a build that would come up missing whatever
   * that record said.
   */
  const DurableReplayHandler handler = [](std::uint16_t _kind, std::uint32_t _tick, std::span<const std::uint8_t>)
  {
    NEURON_LOG_ERROR("the journal holds a record of kind %u at tick %u that this build does not know",
                     static_cast<unsigned>(_kind), _tick);
    return false;
  };
  if (!_store.Replay(handler, report))
  {
    NEURON_LOG_ERROR("shard state refused: %s", report.message.c_str());
    return ShardState::Refused;
  }

  const bool loaded = !_store.Snapshot().empty() || report.recordsReplayed > 0;
  NEURON_LOG_INFO("shard state: %s (%s)", loaded ? "loaded" : "fresh", report.message.c_str());
  return loaded ? ShardState::Loaded : ShardState::Fresh;
}

ServerConfig MakeServerConfig(const Outpost::AppConfig& _config, DurableStore* _store)
{
  ServerConfig server;
  server.port = _config.server.port;
  server.maxSessions = _config.server.maxSessions;
  server.durableStore = _store;
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
  // Zero travels through unchanged: the client is the tier that knows what its
  // own passes cost, so "decide for yourself" is a thing this layer forwards
  // rather than resolves (ADR-018 A20).
  client.uploadBytesPerFrame = _config.client.renderer.uploadBytesPerFrame;
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

  /*
   * And how big each of them is, which the engine has no way of knowing
   * (ADR-014): a mesh is authored at whatever scale the modelling package
   * handed back, and only this side knows that one file is a 17 m interceptor
   * and another a 200 m station.
   *
   * **This is why the fleet read as specks.** The class table has always stated
   * a contact radius per hull -- formation spacing is four of them, and picking
   * rounds a silhouette up to reach its own -- but nothing enforced that the
   * art agreed. Measured against `Game::SilhouetteRadiusMetres`, `Structure`
   * and `Stargate` were within 1% (ADR-016 §10 wrote the class rows *from*
   * those two meshes), and every flyable hull was between a quarter and a
   * twelfth of the size its own row describes. An Interceptor was a 7 m dart
   * sitting inside a 90 m selection ring.
   *
   * A file with no hull -- one the list names and `MESH_FOR_HULL` does not --
   * gets a zero, which the loader reads as "as authored". Silence is the right
   * answer for content this table has no opinion about.
   */
  client.meshPlaneRadiiMetres.assign(_config.content.meshes.size(), 0.0f);
  // And which fixture it carries (ADR-006 §6a). `None` for a file the table has
  // no row for, on the same argument as the zero radius above: silence is the
  // right answer for content this build has no opinion about.
  client.meshLampRigs.assign(_config.content.meshes.size(), Neuron::LampRig::None);
  for (const auto& mapping : MESH_FOR_HULL)
  {
    for (std::size_t index = 0; index < _config.content.meshes.size(); ++index)
    {
      if (_config.content.meshes[index] == mapping.meshFile)
      {
        client.meshPlaneRadiiMetres[index] =
          Game::SilhouetteRadiusMetres(mapping.hullClass) * static_cast<float>(_config.client.renderer.hullScale);
        client.meshLampRigs[index] = mapping.lampRig;
        break;
      }
    }
  }
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

  // N3's two families. Carried across as words and numbers rather than resolved
  // here: the client is what owns `ResolveHandedness` and the palette table, and
  // the composition root's job is to hand over what the file said.
  client.uiHighContrast = _config.client.ui.highContrast;
  client.uiReduceMotion = _config.client.ui.reduceMotion;
  client.uiAlwaysShowHullBars = _config.client.ui.alwaysShowHullBars;
  client.uiHandedness = _config.client.input.handedness;
  client.longPressSeconds = static_cast<float>(_config.client.input.longPressSeconds);
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
  // The same configuration without the player's layer over it, kept for the
  // whole run because it is what a save is a difference against (ADR-012 §A3).
  Outpost::AppConfig shipped;
  Outpost::ConfigPaths paths;
  Outpost::ConfigDiagnostics diagnostics;
  if (!Outpost::LoadAppConfig(config, shipped, paths, diagnostics))
  {
    ReportStartupFailure(diagnostics);
    return 1;
  }

  // Decided the moment the config can say: only an attended, windowed launch
  // gets a dialog it can wait on. Everything else reports and exits.
  g_fatalDialogAllowed =
    (config.mode == Outpost::HostMode::Host || config.mode == Outpost::HostMode::Client) && !config.selfTest;

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
  UniverseSimulation simulation{contentHash,          MakeWorldMeta(universe.universe), universe.universe,
                                economy.economy,      universe.universeHash,            config.scenario};

  /*
   * What the shard already is, before it is given a fleet (ADR-025 §6).
   *
   * The starting fleet is what a *new* shard is built with, and spawning it on
   * top of a reloaded one would hand every commander a second fleet every time
   * the service restarted. So the load decides, and the three answers are
   * "there was nothing", "there was something" and "there was something wrong"
   * -- only the last of which is a failure.
   */
  DurableStore shardState;
  const ShardState opened = OpenShardState(config, universe.universeHash, economy.economyHash, simulation, shardState);
  if (opened == ShardState::Refused)
  {
    Log::Shutdown();
    return 4;
  }
  if (opened == ShardState::Fresh)
  {
    simulation.SpawnStartingFleet();
  }

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
    // Every line to disk as it happens: this gate runs unattended and CI's
    // watchdog kills it on a hang, and a log whose tail is sitting in a stdio
    // buffer at that moment says nothing about where it stopped.
    Log::SetFlushEveryLine(true);
    const int result = Outpost::RunSelfTest(config, simulation, economy.economy);
    Log::Shutdown();
    return result;
  }

  // Boot order is normative (ADR-008 §5): the server starts before the client,
  // and the client is torn down first.
  ServerHost server;

  int exitCode = 0;
  const bool hostsServer = config.mode != Outpost::HostMode::Client;
  if (hostsServer && !server.Start(MakeServerConfig(config, shardState.IsOpen() ? &shardState : nullptr), simulation))
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
      Outpost::ReplicatedWorldView worldView{MakeWorldViewDesc(config, universe, contentHash, economy.economy)};

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
      SaveUserLayer(config, shipped, paths, worldView, client);
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
