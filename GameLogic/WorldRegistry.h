#pragma once

#include "Ids.h"
#include "Universe.h"
#include "World.h"

#include <cstdint>
#include <memory>
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

/// Which host owns a grid (ADR-019 §1). Always 0 today, and the point is that
/// every addressing path already goes through something that could say
/// otherwise.
using HostId = std::uint16_t;

/*
 * A transfer's identity (ADR-019 §6.3), defined here so T1 inherits it rather
 * than inventing it. The total order is `(applyTick, hostId, counter)`, which
 * needs no coordination between hosts: the counter is per-host and the host id
 * breaks ties.
 */
struct TransferId
{
  HostId host = 0;
  std::uint32_t counter = 0;

  [[nodiscard]] friend bool operator==(const TransferId& _a, const TransferId& _b) noexcept
  {
    return _a.host == _b.host && _a.counter == _b.counter;
  }
  [[nodiscard]] friend bool operator<(const TransferId& _a, const TransferId& _b) noexcept
  {
    return _a.host != _b.host ? _a.host < _b.host : _a.counter < _b.counter;
  }
};

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

  /// Which host owns an anchor (ADR-019 §6.4). Zero, today and by design --
  /// what matters is that every cross-world addressing path already asks.
  [[nodiscard]] static HostId HostForAnchor(AnchorId _anchor) noexcept;

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
};

} // namespace Game
