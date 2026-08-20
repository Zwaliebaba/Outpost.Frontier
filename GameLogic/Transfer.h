#pragma once

#include "Ids.h"
#include "Orders.h"
#include "ShipClass.h"

#include <cstdint>

/*
 * The transfer bus (ADR-016 §4, ADR-017 §9, ADR-018 D17, build order T1).
 *
 * A ship can be in three places: on a grid, in transit, or docked. Moving
 * between them is the one thing a world may not do to itself -- a world knows
 * only its own ships -- so it is done *between* ticks, by the registry, from
 * records the worlds file.
 *
 * **The bus arrives with the station phase rather than with warp**, which is
 * the interleave ADR-017 chose: dock is its first record kind, undock its
 * second, and U3a's transit records inherit the mechanism instead of inventing
 * it. That is also why `TransferKind` names more than it implements.
 *
 * **Applied between ticks, in `(applyTick, transferId)` order** (ADR-018 D17,
 * settling ADR-016 §4's "arrival tick, order id" and ADR-017 §9's "apply tick,
 * record order" as the same rule). The id is stamped at filing and is
 * monotonic per host, so the total order needs no coordination between hosts:
 * the counter orders one host's records and the host id breaks ties.
 *
 * Nothing here is a pointer to a world. A record names an **anchor**, which is
 * how ADR-019 §6.1 says one grid refers to another, and the registry resolves
 * it at apply time -- so a transfer filed on a host that no longer owns the
 * destination still applies to the right grid.
 */

namespace Game
{

/// Which host owns a grid (ADR-019 §1). Always 0 today, and the point is that
/// every addressing path already goes through something that could say
/// otherwise.
using HostId = std::uint16_t;

/*
 * A transfer's identity (ADR-019 §6.3). The total order is
 * `(applyTick, hostId, counter)`: the counter is per-host and the host id
 * breaks ties, so two hosts filing in the same tick agree on the sequence
 * without talking to each other.
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

/*
 * What kind of crossing this is.
 *
 * `Undock` and `Transit` are numbered and unimplemented, for the same reason
 * `OrderKind::Warp` is: the slice that fills one in should not renumber the
 * records the slice before it wrote.
 */
enum class TransferKind : std::uint8_t
{
  Dock = 0,
  Undock = 1, // T1c.
  Transit = 2, // U3a.

  /*
   * A completed mining cycle's debit against the site ledger (ADR-024 §4b, E2).
   *
   * A crossing between a *world* and the universe layer, like a dock, and it
   * rides here for the same reason: the number that has to change is not the
   * world's, so the world may not change it. The tick credits the ship's hold
   * from its own copy of the field and files this; the registry applies it to
   * the ledger between ticks, in the same `(applyTick, transferId)` order every
   * other record obeys.
   *
   * That is what keeps the per-tick path clear of the universe (ADR-009 §2)
   * without inventing a second mechanism: mining extends the replay contract by
   * one record family rather than forking it.
   */
  MineYield = 3
};

/*
 * One ship in a crossing.
 *
 * Its identity survives intact -- that is the whole point of the roster keeping
 * ids (ADR-017 §1): every log, order and roster row means the same ship before
 * and after. Class and wing travel with it because the roster holds exactly
 * those three things and nothing else; a docked ship has no position to carry
 * and no gauges to repair, which *is* the repair rule.
 */
struct TransferMember
{
  ShipId shipId = INVALID_SHIP_ID;
  HullClass hullClass = HullClass::Interceptor;
  WingId wing = INVALID_WING_ID;
};

/*
 * What a world hands over, and what comes back: **a fleet, not a ship**.
 *
 * "A fleet docks together and instantly"; a fleet undocks together and arrives
 * in formation. If a crossing were one record per ship, "together" would be a
 * property of how the records happened to be ordered rather than a fact about
 * the record -- and the arrival solve, which needs every member at once to
 * place any of them, would have nothing to solve over.
 */
struct TransferRequest
{
  TransferKind kind = TransferKind::Dock;

  /// The anchor the fleet is crossing to: the station it docks at, the station
  /// it undocks from, and later the destination it warps to.
  AnchorId anchor = INVALID_ID;

  /// How it arranges itself on arrival. Meaningless for a dock -- the roster
  /// has no shape.
  FormationId formation = FormationId::Line;

  std::uint16_t memberCount = 0;
  TransferMember members[MAX_SHIPS_PER_ORDER] = {};

  /*
   * What one completed cycle took, and **meaningless for every other kind** --
   * the same arrangement `formation` has, which is meaningless for a dock.
   *
   * One record per cycle rather than per tick or per fleet: a cycle is 800
   * ticks and a field is worked by at most 64 Miners, so this is a handful of
   * records a minute against a bus that already carries every dock in the
   * shard. Batching them would buy nothing and would make the ledger's history
   * coarser than the thing it is a ledger of.
   */
  std::uint8_t cluster = 0;
  OreId ore = OreId::FerroChroma;
  std::uint32_t units = 0;

  /*
   * Which epoch's pool those units came out of (ADR-024 §3d).
   *
   * Carried rather than looked up at apply time, and the reason is a race that
   * cannot happen today and would be invisible when it did: a field is never
   * re-formed under a live grid, so a debit and its ledger always agree -- but
   * "always" there is a fact about two pieces of code that cannot see each
   * other, and a record that says which pool it emptied needs no such
   * agreement. A debit against an epoch that has already been refilled is
   * dropped rather than eating the new pool.
   */
  std::uint32_t epoch = 0;

  /*
   * Whether that cycle was the one that filled the ship (ADR-024 §4b).
   *
   * A flag on the record rather than a second record kind, because it is a fact
   * *about this cycle* and it is needed exactly where this record is applied:
   * the event log lives at the universe layer, so the world cannot write "your
   * Miners filled up at VEI-4 II" and the registry cannot know it without being
   * told.
   */
  bool filledHold = false;

  /// Appends, or reports the crossing is full. Returning false rather than
  /// dropping keeps half a fleet from reading as the whole one.
  [[nodiscard]] bool AddMember(const TransferMember& _member) noexcept
  {
    if (memberCount >= MAX_SHIPS_PER_ORDER)
    {
      return false;
    }
    members[memberCount] = _member;
    ++memberCount;
    return true;
  }
};

/*
 * The fixed part of a warp, in seconds (ADR-016 §5).
 *
 * Every crossing costs this much before distance is counted -- the alignment,
 * the entry, the arrival -- so a hop to the next planet is not instantaneous
 * and the difference between two nearby destinations is not the whole cost of
 * choosing. The distance term does the rest.
 */
inline constexpr float WARP_BASE_SECONDS = 5.0f;

/*
 * What a gate jump costs, in ticks (ADR-016 §5, U4).
 *
 * **Flat, and that is the decision rather than a simplification.** An in-system
 * warp is priced by distance because in-system distance is real (ADR-009 §3);
 * *between* systems it is map fiction -- the strategic map spaces systems for
 * legibility, not for astronomy -- so pricing a jump by it would charge the
 * player for a number nobody chose to mean anything. Every gate crossing costs
 * the same, wherever the two systems are drawn.
 *
 * Four hundred ticks is twenty seconds at ADR-002's 20 Hz, and twenty is set
 * against the number the route planner print prices: an eleven-jump route at
 * 4m 10s, about twenty-three seconds a hop. A hop is the spool plus the
 * crossing, and a light fleet spools in four (`spoolSeconds`, `ShipClass`), so
 * twenty here is what makes that arithmetic land. A capital fleet pays more, in
 * the spool, where the difference between hulls belongs.
 *
 * **In ticks and not seconds, unlike `WARP_BASE_SECONDS`**, and the reason is
 * that this one is *flat*. A warp's duration is a division either way -- the
 * distance term has to be converted -- but a constant whose whole point is that
 * it is identical for every crossing should not arrive by one: `20.0 / 0.05` is
 * 399.99999 in binary floating point and truncates to 399, so the number the
 * design states would not be the number the bus used. The tick is the only
 * clock (ADR-016 §5), and this is the one place it is cheapest to say so.
 *
 * Unlike a warp it is not governed by the slowest member: the gate does the
 * moving, and a hull's warp drive has nothing to do with it.
 */
inline constexpr std::uint32_t GATE_JUMP_TICKS = 400;

/*
 * The shortest a *transit* may be, in ticks (ADR-019 §4b).
 *
 * Not a game-feel number: it is the slack a cross-host transfer needs. A
 * transit's `applyTick` is seconds in the future, and the destination host has
 * until then to receive the record -- which turns latency into slack instead of
 * a race, but only if the slack is guaranteed rather than assumed. One second
 * bounds worst-case host skew plus inter-host RTT with room to spare.
 *
 * It applies to crossings that take time -- warp and, since U4, the gate jump
 * -- and not to a dock or an undock, which are not journeys: ADR-017 §2's "the
 * whole fleet, one moment" is a crossing between a world and the universe layer
 * on one host, with no other host to be raced by.
 *
 * There is one host, so nothing needs this yet, and that is exactly when a
 * timing table can be tuned under a floor without anybody noticing.
 */
inline constexpr std::uint32_t TRANSFER_FLOOR_TICKS = 20;

/// A filed request, stamped. The registry stamps -- the counter is the host's,
/// not a world's, and a world minting its own would give two grids the same
/// number in the same tick.
struct TransferRecord
{
  TransferId id;

  /// The tick this applies *before*. Filed during tick N, applied between N and
  /// N+1, which is what makes "no world ever reads another mid-tick" true.
  std::uint32_t applyTick = 0;

  TransferRequest what;

  /// The total order ADR-018 D17 fixes. Written as a function rather than left
  /// to each caller's comparator so that "the bus order" is one thing.
  [[nodiscard]] friend bool operator<(const TransferRecord& _a, const TransferRecord& _b) noexcept
  {
    return _a.applyTick != _b.applyTick ? _a.applyTick < _b.applyTick : _a.id < _b.id;
  }
};

} // namespace Game
