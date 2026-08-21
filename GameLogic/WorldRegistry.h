#pragma once

#include "EconomyMessages.h"
#include "EventRecord.h"
#include "FleetSummary.h"
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

  /*
   * Points the registry at the baked content. Both files outlive it -- they are
   * read at boot and never change -- so this keeps pointers rather than copies
   * of several megabytes and a balance table.
   *
   * The economy may be null, and that is a real configuration rather than a
   * defensive check: a registry with no economy has no mining fields, no cargo
   * and no refineries, which is exactly what every slice before E2 ran as and
   * what a test about warp or docking still wants.
   */
  void Reset(const UniverseDef* _universe, const EconomyDef* _economy, const RegistryConfig& _config);

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
   * What is left of a mining field (ADR-024 §3d), or null for a site nobody has
   * worked this epoch.
   *
   * Null is the *pristine* answer and not an absence: a ledger exists only once
   * something has been taken, so "no ledger" and "the bake's pools, untouched"
   * are the same statement. That is what keeps the durable set proportional to
   * what has actually happened rather than to how many fields the bake authored
   * -- six thousand sites, and storage for the ones being mined.
   *
   * Borrowed like the roster, and invalidated by the next debit or spin-up.
   */
  [[nodiscard]] const SiteLedger* Ledger(AnchorId _anchor) const noexcept;

  /// All of them, in anchor order -- the order the hash folds them and the
  /// order ADR-025's journal will write them.
  [[nodiscard]] std::span<const SiteLedger> Ledgers() const noexcept { return m_siteLedgers; }

  /*
   * What a commander has committed at a station (ADR-024 §5b), or null for a
   * Bay nobody has put anything in.
   *
   * Null is the empty answer and not an absence, exactly as it is for a site
   * ledger: a Bay exists once something has been stored, so "no Bay" and "an
   * empty Bay" are the same statement and the durable set stays proportional to
   * what has happened rather than to how many stations exist.
   *
   * Takes the owner because a Bay without one is not addressable -- that is the
   * privacy rule living in the key rather than in a filter somebody has to
   * remember to apply (ADR-017 §1).
   */
  [[nodiscard]] const StationBay* Bay(Neuron::PlayerId _owner, AnchorId _station) const noexcept;

  /// All of them, in `(station, owner)` order -- the order the hash folds them
  /// and the order ADR-025's journal will write them.
  [[nodiscard]] std::span<const StationBay> Bays() const noexcept { return m_bays; }

  /*
   * The field a site's grid would be spun up with right now.
   *
   * Bake plus ledger, resolved through the current epoch -- the same function
   * `SpinUp` uses, exposed because "how eaten is that field" is a question the
   * summaries E3 puts on the wire have to answer for a grid that is not live.
   * A grid that *is* live is the authority on its own field; this is what the
   * universe layer knows without one.
   */
  [[nodiscard]] SiteField FieldAt(AnchorId _anchor) const;

  /*
   * That field as the wire reports it (ADR-024 §8, E3).
   *
   * Here rather than in the sender because answering it takes three things only
   * the registry has together: which grid is live (a live one is the authority
   * on its own field), what the ledger has taken out, and what the epoch's
   * layout started with. A sender that assembled those itself would be a second
   * place for "how eaten is that field" to be computed, and the two would drift
   * the first time one of them learned about a new hazard.
   *
   * A grid that is not a site answers a zeroed row, which writes as a status
   * with no clusters -- honest, and cheaper than making every caller ask twice.
   */
  [[nodiscard]] SiteStatusRow SiteStatusFor(AnchorId _anchor) const;

  /*
   * What one commander's ships are carrying, across every grid they are on
   * (ADR-024 §5, E3).
   *
   * **Keyed by the viewer, not filtered for them.** The privacy of a
   * `CargoStatus` is a property of what this function is asked rather than of
   * what the sender remembers to leave out -- there is no version of this that
   * returns everybody's holds and trusts the caller.
   *
   * Docked ships are not here: their holds ride on the roster, which is already
   * sent per viewer. One fact in one place, and a hangar screen reading two
   * sources for the same number is exactly the drift this avoids.
   */
  [[nodiscard]] std::vector<CargoStatusRow> CargoFor(Neuron::PlayerId _owner) const;

  /*
   * What happened, in order (ADR-018 D19).
   *
   * Read-only from outside, because the producers are all in here: a surface
   * that could write to the log could describe something that never occurred.
   * One record per session today and one per commander when there are
   * commanders -- the emissions already carry everything that split needs.
   */
  [[nodiscard]] const EventRecord& Events() const noexcept { return m_events; }

  /*
   * Where the commander's ships are, without looking at any of them
   * (ADR-016 §6).
   *
   * One row per (place, state): ships standing on a grid, ships docked at a
   * station, and ships crossing to somewhere -- the last carrying the ETA no
   * grid can, because a fleet in transit is in no world at all.
   *
   * Ordered by anchor and then by state, so two runs of one script produce the
   * same message and a client can diff two of them without sorting.
   *
   * Everything today, because there is one commander. When there are more this
   * takes a `PlayerId` and filters (ADR-018 D5) -- the shape does not change,
   * only what it counts.
   */
  [[nodiscard]] std::vector<FleetSummary> Summaries() const;

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
  [[nodiscard]] OrderVerdict SubmitStationCommand(Neuron::PlayerId _owner, const StationCommand& _command);

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

    /// Which epoch's field this grid was spun up with, on a site. It does not
    /// move while the grid is live -- that is the whole of "an occupied world
    /// is never re-formed under the players" (ADR-024 §3d).
    std::uint32_t fieldEpoch = 0;
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
  void ApplyTransit(const TransferRequest& _request);

  /// Debits a completed cycle against the ledger, creating one for a site that
  /// has not been worked this epoch (ADR-024 §4b).
  void ApplyMineYield(const TransferRequest& _request);

  /// Which epoch a site is in *now*. `SpinUp` is the only thing allowed to act
  /// on the answer; everything else uses the epoch the live grid was built with.
  [[nodiscard]] std::uint32_t EpochNow(AnchorId _anchor) const noexcept;

  /*
   * Is this ledger describing a pool that still exists?
   *
   * ADR-018 D8's rule, applied to the ledger: a ledger the shard owes a refill
   * is a memory rather than state -- the pool it counts was replaced at the
   * epoch boundary -- and folding it would make the session's hash depend on
   * whether anybody happened to spin the grid up and notice. A field held live
   * by *ships* is judged against that grid's own epoch, because a worked field
   * is not re-formed under them; a field held live only by a **viewer** is
   * judged against the calendar, for the same reason D8 leaves a viewer-only
   * grid out of the hash entirely.
   */
  [[nodiscard]] bool LedgerIsCurrent(const SiteLedger& _ledger) const noexcept;

  /// How long a crossing between two anchors takes, in ticks (U3a): a base
  /// plus the universe distance over the slowest member's warp speed, never
  /// below `TRANSFER_FLOOR_TICKS` (ADR-019 §4b).
  [[nodiscard]] std::uint32_t TransitTicks(AnchorId _from, const TransferRequest& _request) const;

  /// Which anchors a grid may warp to (ADR-016 §5). Same system, itself
  /// excluded -- gates are how a fleet leaves one, and that is U4's.
  [[nodiscard]] std::vector<AnchorId> ReachableFrom(AnchorId _anchor) const;

  /// Writes the index, growing it as ids climb. The one place the projection is
  /// written, so it cannot drift into being a second source of truth.
  void RecordLocation(ShipId _shipId, AnchorId _anchor);


  void ApplyDueTransfers();
  void CollectFiledTransfers();

  /// The ledger for an anchor, made if it is not there. The mutable half of
  /// `Ledger`, and the only thing that creates one -- which is what keeps a
  /// pristine field out of the durable set.
  [[nodiscard]] SiteLedger& LedgerFor(AnchorId _anchor);

  /// The Bay for an owner at a station, made if it is not there. The mutable
  /// half of `Bay`, and the only thing that creates one -- which is what keeps
  /// an empty Bay out of the durable set.
  [[nodiscard]] StationBay& BayFor(Neuron::PlayerId _owner, AnchorId _station);

  /// Moves ore between the holds on a station's roster and a Bay, in roster
  /// order, and returns how much actually moved. Validation has already said
  /// the source can cover it; this is the arithmetic.
  std::uint32_t MoveOre(StationRoster& _roster, StationBay& _bay, const StationCommand& _command, bool _toBay);

  [[nodiscard]] LiveWorld* Find(AnchorId _anchor) noexcept;
  [[nodiscard]] const LiveWorld* Find(AnchorId _anchor) const noexcept;
  [[nodiscard]] LiveWorld& SpinUp(const Anchor& _anchor);

  /// The field a site's grid gets: the bake laid out by this epoch's salt, with
  /// whatever the ledger says has already been taken out of it.
  [[nodiscard]] SiteField ResolveField(const Anchor& _anchor, std::uint32_t& _outEpoch) const;
  void TearDownIdle();

  const UniverseDef* m_universe = nullptr;
  const EconomyDef* m_economy = nullptr;
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

  /*
   * And the site ledgers beside them (ADR-024 §3d), on exactly the same terms:
   * durable state that outlives the grids that produced it, sorted by anchor,
   * folded into `Hash`. "Worlds forget, ledgers do not" is one rule with two
   * residents now.
   */
  std::vector<SiteLedger> m_siteLedgers;

  /*
   * And the Bays (ADR-024 §5b), the third resident of the same rule.
   *
   * Sorted by `(station, owner)`, which is the order the hash folds them in and
   * the order a station's screen would read them: everything about one place
   * together, and one commander's row findable inside it. The reverse ordering
   * would put a commander's whole estate together, which is a question nothing
   * asks -- a Bay is a fact about a station first.
   */
  std::vector<StationBay> m_bays;

  /// Beside them, and unlike them **not** in the hash: an event describes
  /// something the simulation already did, and folding the description in as
  /// well would make a replay depend on how talkative the build was.
  EventRecord m_events;

  /// Stamped at filing, monotonic per host (ADR-018 D17). Never reset except by
  /// `Reset`, so no two records of one session share an id.
  std::uint32_t m_nextTransferCounter = 1;
};

} // namespace Game
