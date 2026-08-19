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
  Transit = 2 // U3a.
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
