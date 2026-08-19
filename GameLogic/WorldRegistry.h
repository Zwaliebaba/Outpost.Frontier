#pragma once

#include "EventRecord.h"
#include "Ids.h"
#include "Station.h"
#include "Transfer.h"
#include "Universe.h"
#include "World.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/*
 * The universe runtime (ADR-016 §4, ADR-019, build order U2).
 *
 * A session is many grids, not one. The registry owns them: which anchors are
 * live, what each world's ships are, where any ship is, and the one tick number
 * they all advance on.
 *
 * **Shaped by ADR-019 even though it runs in one process.** Every constraint in
 * its §6 costs a shape here and a mechanism later, which is the entire reason
 * that ADR was written before this slice: worlds are addressed by `AnchorId`
 * and never by a pointer held across a tick, nothing reaches into a world it
 * does not own, `HostForAnchor` exists and returns zero, and player-scoped
 * questions go through an index rather than a walk. At `HostId = 0` the cost is
 * a type and a function returning a constant.
 */

namespace Game
{

struct RegistryConfig
{
  /// Mixed with an anchor id to seed each world, so a grid's randomness is a
  /// function of *which* grid it is (ADR-016 §4) and two sessions of the same
  /// seed reproduce each other exactly.
  std::uint64_t sessionSeed = 0;

  /// This process's host id. Zero, and the block of ship ids it may issue is
  /// derived from it (ADR-019 §5c).
  HostId hostId = 0;
};

class WorldRegistry
{
public:
  WorldRegistry() = default;
  WorldRegistry(const WorldRegistry&) = delete;
  WorldRegistry& operator=(const WorldRegistry&) = delete;

  /// Points the registry at the baked universe. The universe outlives it -- it
  /// is read at boot and never changes -- so this keeps a pointer rather than
  /// a copy of several megabytes.
  void Reset(const UniverseDef* _universe, const RegistryConfig& _config);

  /*
   * The live world for an anchor, spinning it up if it is not live.
   *
   * **Borrow, never hold** (ADR-019 §6.1). The returned pointer is valid until
   * the next call that can spin up or tear down -- which is to say until the
   * next tick -- and a caller that stores it has written code that cannot
   * survive the day a grid lives on another machine.
   */
  [[nodiscard]] World* Borrow(AnchorId _anchor);

  /// The live world, or null. Never spins one up: this is the question "is
  /// anyone there?", which must not be answerable only by making it true.
  [[nodiscard]] const World* Peek(AnchorId _anchor) const noexcept;

  /*
   * One tick, for every live world, with one number (ADR-019 §2).
   *
   * The shard tick is a *contract*, not a convention: a world spun up at shard
   * tick N begins there, and a teardown/recreate comparison is only meaningful
   * at equal shard ticks. Worlds are ticked in anchor-id order, stated so that
   * nothing may depend on it -- they share nothing during `Tick`, so the order
   * cannot matter, and the suite holds that by permuting it.
   */
  /// The tick is **given**, not counted here: the server owns the number, and a
  /// registry keeping its own would be a second clock to drift.
  void Tick(std::uint32_t _shardTick);

  /// A viewer holds a grid alive even when nothing is on it (ADR-016 §7): a
  /// world is never torn down under someone's camera. U3b is what starts
  /// calling these; U2 builds the hold.
  void AddViewer(AnchorId _anchor);
  void RemoveViewer(AnchorId _anchor);

  /*
   * What is docked at a station (ADR-017 §1), in the order it docked.
   *
   * Empty for an anchor nobody has docked at, which is the same answer as an
   * anchor that is not a station -- the roster is a fact about ships, not a
   * property a station has to be given.
   *
   * A span into the registry's storage: borrowed, like everything else here,
   * and invalidated by the next dock or undock.
   */
  [[nodiscard]] std::span<const RosterEntry> Roster(AnchorId _anchor) const noexcept;

  /*
   * What happened, in order (ADR-018 D19).
   *
   * Read-only from outside, because the producers are all in here: a surface
   * that could write to the log could describe something that never occurred.
   * One record per session today and one per commander when there are
   * commanders -- the emissions already carry everything that split needs.
   */
  [[nodiscard]] const EventRecord& Events() const noexcept { return m_events; }

  /// How many records are filed and not yet applied. The bus is world state --
  /// it folds into the hash -- so this is how a test asks whether a tick
  /// actually moved anything.
  [[nodiscard]] std::uint32_t PendingTransferCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_bus.size());
  }

  /*
   * Hands every live world over (ADR-007 §7).
   *
   * The composition root builds the start grid on Main -- spinning it up,
   * spawning the authored fleet -- and the server then runs it on Sim. This is
   * where the first says it is finished, so the second adopts rather than
   * trips. It is the one hand-off the design sanctions, and it is a single call
   * because the registry is what knows how many worlds there are.
   *
   * Worlds spun up *after* this are claimed by whichever thread spun them up,
   * which is the sim thread for every path that exists today.
   */
  void HandOff() noexcept;

  /// Which host owns an anchor (ADR-019 §6.4). Zero, today and by design --
  /// what matters is that every cross-world addressing path already asks.
  [[nodiscard]] static HostId HostForAnchor(AnchorId _anchor) noexcept;

  /*
   * Puts a ship on a grid, and tells the index where it is.
   *
   * The path everything that is not an authored occupant should take. Spawning
   * straight into a borrowed world still works and still needs an id from
   * `AllocateShipId` -- but it leaves the ship out of the ship->location index,
   * so "where are my ships" cannot answer for it. That gap is why this exists:
   * the registry is what knows both halves, so the registry is what does it.
   *
   * `INVALID_SHIP_ID` when the anchor has no world, or the id space is spent,
   * or the class has no content -- the same three refusals `World::Spawn` makes,
   * reported the same way.
   */
  [[nodiscard]] ShipId Spawn(AnchorId _anchor, const ShipSpawn& _spawn);

  /*
   * Undock or reassign a wing (ADR-017 §3, §6).
   *
   * The station half of order submission, and it goes through the same shared
   * validator both machines run. An accepted `Undock` files a transfer, so the
   * fleet arrives between ticks like everything else that crosses; an accepted
   * `AssignWing` writes the roster row on the spot, because nothing crosses --
   * a wing is a number a ship carries, not a place it is.
   */
  [[nodiscard]] OrderVerdict SubmitStationCommand(const StationCommand& _command);

  /// Takes a ship off a grid and out of the index. The counterpart of `Spawn`,
  /// and the path a caller should take for the same reason: `World::Despawn`
  /// knows nothing about the index, so a ship removed through it would still
  /// answer "where are my ships" until the grid tore down.
  bool Despawn(AnchorId _anchor, ShipId _shipId);

  /*
   * The next dynamic ship id, from this host's block (ADR-018 D6a, ADR-019 §5c).
   *
   * Authored occupants do not come through here -- their ids are baked into
   * their anchor, which is what makes a recreated world reproduce them exactly.
   * Returns `INVALID_SHIP_ID` when the block is spent, which is the u16 ceiling
   * D6 keeps until the delta cluster widens the record.
   */
  [[nodiscard]] ShipId AllocateShipId();

  /// Where a ship is, without walking the registry (ADR-019 §6.6). This is the
  /// index rosters, summaries and order routing all ask; U2 builds it because
  /// every one of those is about to.
  [[nodiscard]] bool LocationOf(ShipId _shipId, AnchorId& _outAnchor) const noexcept;

  /*
   * The replay domain (ADR-018 D8).
   *
   * Folds the shard tick and every live world's hash, in anchor-id order --
   * **except worlds that hold only their authored occupants and are alive only
   * because someone is looking at them.** That exclusion is the whole point of
   * the rule: without it, whether a commander happened to be watching an empty
   * grid would change the session's hash, and viewer behaviour would be a
   * hidden simulation input.
   */
  [[nodiscard]] std::uint64_t Hash() const;

  [[nodiscard]] std::uint32_t ShardTick() const noexcept { return m_shardTick; }
  [[nodiscard]] std::uint32_t LiveWorldCount() const noexcept { return static_cast<std::uint32_t>(m_live.size()); }

  /// The live anchors, in the order everything iterates them.
  [[nodiscard]] std::vector<AnchorId> LiveAnchors() const;

private:
  struct LiveWorld
  {
    AnchorId anchor = INVALID_ID;
    std::unique_ptr<World> world;
    std::uint32_t viewers = 0;
    std::uint16_t authoredCount = 0;
    std::uint32_t spunUpAtTick = 0;
  };

  /// One station's roster. Sorted into `m_rosters` by anchor id, which is the
  /// order the hash folds them in.
  struct StationRoster
  {
    AnchorId anchor = INVALID_ID;
    std::vector<RosterEntry> docked;
  };

  [[nodiscard]] StationRoster& RosterFor(AnchorId _anchor);
  void ApplyUndock(const TransferRequest& _request);

  /// Writes the index, growing it as ids climb. The one place the projection is
  /// written, so it cannot drift into being a second source of truth.
  void RecordLocation(ShipId _shipId, AnchorId _anchor);


  void ApplyDueTransfers();
  void CollectFiledTransfers();

  [[nodiscard]] LiveWorld* Find(AnchorId _anchor) noexcept;
  [[nodiscard]] const LiveWorld* Find(AnchorId _anchor) const noexcept;
  [[nodiscard]] LiveWorld& SpinUp(const Anchor& _anchor);
  void TearDownIdle();

  const UniverseDef* m_universe = nullptr;
  RegistryConfig m_config;
  std::uint32_t m_shardTick = 0;

  /// Sorted by anchor id, which is the iteration order every other rule cites.
  std::vector<LiveWorld> m_live;

  /// The ship->location index (ADR-019 §5c): a projection, rebuilt as ships
  /// arrive and leave, never a second source of truth.
  std::vector<AnchorId> m_locationByShip;

  std::uint32_t m_nextDynamicId = 0;

  /*
   * The transfer bus and the rosters (ADR-017 §1, §9; ADR-018 D17).
   *
   * Both are **universe-layer durable state**, which is the other half of
   * "worlds forget" (D8): a station grid can tear down with a full roster and
   * the roster is untouched, because it was never world state to begin with.
   * Both fold into `Hash`, so a replay that lost a dock in flight would say so.
   */
  std::vector<TransferRecord> m_bus;
  std::vector<StationRoster> m_rosters;

  /// Beside them, and unlike them **not** in the hash: an event describes
  /// something the simulation already did, and folding the description in as
  /// well would make a replay depend on how talkative the build was.
  EventRecord m_events;

  /// Stamped at filing, monotonic per host (ADR-018 D17). Never reset except by
  /// `Reset`, so no two records of one session share an id.
  std::uint32_t m_nextTransferCounter = 1;
};

} // namespace Game
