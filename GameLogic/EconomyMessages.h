#pragma once

#include "EconomyDef.h"
#include "Ids.h"
#include "SiteField.h"
#include "Station.h"

#include "ByteReader.h"
#include "ByteWriter.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/*
 * The economy's three summaries (ADR-024 §8, build order E3).
 *
 * All three join ADR-016 §6's family beside `StationRoster` and
 * `FleetSummaries`: **per viewer, about once a second, describing places rather
 * than entities**. They exist because the per-tick record deliberately cannot
 * carry them.
 *
 * That last point is the whole architectural content of this file, so it is
 * worth stating plainly. `Neuron::EntityRecord` is 21 bytes and 43 of them fit
 * one datagram, against ADR-018's floor of 41 ships on a grid. One cargo byte
 * would take the record to 22 and the fit to 41 -- landing exactly on the floor
 * `Snapshot.h` asserts, with nothing left for the next field anybody wants.
 * ADR-024 §4d refused that arithmetic in advance, and this is where the answer
 * lives instead: cargo is a **fact about property**, it changes on the scale of
 * a mining cycle rather than a tick, and a player who learns their hold filled
 * a second late has lost nothing. A snapshot is for what moves.
 *
 * **Two of the three are owner-only**, and that is a wire fact rather than a
 * policy the sender is trusted to remember. `CargoStatus` says what a
 * commander's own ships are holding and `BayStatus` says what they have stored;
 * neither has any business reaching anybody else, and both are written from
 * state keyed by the viewer -- so the privacy is in what the writer is *given*
 * rather than in a filter it applies. `SiteStatus` is public: how eaten a field
 * is, is what everybody standing at it can see.
 *
 * Bounded before it loops, like every other member of the family: a count past
 * its cap is refused before a single row is read.
 */

namespace Game
{

/*
 * How much of a field is left (ADR-024 §3d).
 *
 * Per-ore totals **and** per-cluster fractions, which is two answers to two
 * questions a player actually asks. The totals say whether the system is worth
 * flying to; the fractions say which rocks to point a wing at once you are
 * there, and they are what makes a field visibly empty out rather than
 * switching from "fine" to "gone".
 *
 * Fractions rather than counts per cluster, deliberately. A count would be the
 * ledger replicated, which is state the client has no business holding and no
 * way to keep current between summaries; a percentage of what the cluster
 * *started* with is a number that stays honest at any age and renders directly
 * as a bar. ADR-024 §4c's "the client derives the layout, the server counts the
 * ore" is the same split, one layer up.
 */
struct SiteStatusRow
{
  AnchorId anchor = INVALID_ID;

  /// Which epoch this describes. A client holding a status from before the
  /// field re-formed can say so rather than drawing yesterday's rocks.
  std::uint32_t epoch = 0;

  std::uint32_t remainingUnits[ORE_COUNT] = {};

  std::uint8_t clusterCount = 0;

  /// 0..100 of what each cluster held when the epoch laid it out. Saturating at
  /// 100 rather than wrapping, because a bar that reads 3 % when a cluster is
  /// untouched is worse than one that reads 100 %.
  std::uint8_t clusterFullPct[MAX_SITE_CLUSTERS] = {};
};

/// One ship's hold, for the commander who owns it.
struct CargoStatusRow
{
  ShipId shipId = INVALID_SHIP_ID;
  std::uint32_t oreUnits[ORE_COUNT] = {};
};

/// How many rows one `CargoStatus` may carry. The per-order ship cap, for the
/// reason the roster uses it: it is the number a commander can address at once,
/// so it is the number a screen has to be able to show.
inline constexpr std::uint16_t MAX_CARGO_STATUS_ROWS = MAX_SHIPS_PER_ORDER;

/// Bytes one `SiteStatus` occupies: the fixed part plus a byte per cluster.
[[nodiscard]] constexpr std::size_t SiteStatusBytes(std::size_t _clusterCount) noexcept
{
  return 2 + 4 + ORE_COUNT * 4 + 1 + _clusterCount;
}

/// Bytes one `CargoStatus` occupies: a count and then a row per ship.
[[nodiscard]] constexpr std::size_t CargoStatusBytes(std::size_t _shipCount) noexcept
{
  return 2 + _shipCount * (4 + ORE_COUNT * 4);
}

/// Bytes one `BayStatus` occupies. Fixed: a Bay is three counts at one station.
inline constexpr std::size_t BAY_STATUS_BYTES = 2 + ORE_COUNT * 4;

/*
 * Builds the row a field would report, from what it holds now and what its
 * epoch laid out.
 *
 * A pure function over two `SiteField`s, so the sender does not have to know
 * whether the current one came from a grid that is up or from the
 * bake-plus-ledger resolution the registry does for one that is not. Both are
 * the same question -- how eaten is that field -- and answering it two ways
 * would be two ways to be wrong.
 *
 * **The pristine field is a parameter and not a lookup**, because a cluster
 * does not remember what it started with: `SiteCluster::remainingUnits` is what
 * is left, and "zero" means emptied and never-held alike (that is deliberate,
 * and it is what keeps the per-tick structure small). The denominator therefore
 * has to come from the same `BuildSiteField` call the layout came from, which
 * only the caller holding the bake can make.
 */
[[nodiscard]] SiteStatusRow MakeSiteStatus(AnchorId _anchor, const SiteField& _field, const SiteField& _pristine,
                                           std::uint32_t _epoch) noexcept;

[[nodiscard]] bool WriteSiteStatus(const SiteStatusRow& _row, Neuron::ByteWriter& _writer) noexcept;
[[nodiscard]] bool ReadSiteStatus(Neuron::ByteReader& _reader, SiteStatusRow& _outRow) noexcept;

[[nodiscard]] bool WriteCargoStatus(std::span<const CargoStatusRow> _rows, Neuron::ByteWriter& _writer) noexcept;

/*
 * Reads one back. **Not `noexcept`**, and for `ReadStationRoster`'s reason: it
 * fills a caller-owned vector, so it allocates, so it can throw -- and a
 * `noexcept` over an allocation is `std::terminate` rather than a promise.
 */
[[nodiscard]] bool ReadCargoStatus(Neuron::ByteReader& _reader, std::vector<CargoStatusRow>& _outRows);

[[nodiscard]] bool WriteBayStatus(AnchorId _station, std::span<const std::uint32_t> _oreUnits,
                                  Neuron::ByteWriter& _writer) noexcept;
[[nodiscard]] bool ReadBayStatus(Neuron::ByteReader& _reader, AnchorId& _outStation,
                                 std::uint32_t (&_outOreUnits)[ORE_COUNT]) noexcept;

} // namespace Game
