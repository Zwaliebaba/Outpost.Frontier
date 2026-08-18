#pragma once

#include "Ids.h"
#include "Snapshot.h"

#include "EntityRecord.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

/*
 * What the client knows about the world (ADR-002 §4, ADR-005 §1).
 *
 * The server's `World` is float and authoritative; this is its quantised
 * shadow, rebuilt from whole snapshots and never written by anything local.
 * That is F10's requirement made structural: presentation is a pure function of
 * replicated fields, so a client cannot invent a ship, move one, or keep one
 * alive after the server stopped sending it.
 *
 * **It holds a short history and interpolates.** A 20 Hz snapshot rate against
 * a render loop that may run at 144 Hz means seven frames in a row have no new
 * information, and drawing the newest snapshot each time is the difference
 * between motion and a slideshow. So the view keeps the last few snapshots and
 * is sampled at a fractional tick between two of them.
 *
 * The fractional tick comes from the engine (`Neuron::SnapshotBuffer`), which
 * owns the clock estimate because that arithmetic is the same for any game.
 * What is *here* is everything that needs to know what the bytes mean:
 * which fields interpolate linearly, which wrap around a circle, and what
 * happens to a ship the stream stops mentioning.
 */

namespace Game
{

/// One ship as the client sees it: dequantised into the units it draws in.
struct ReplicatedShip
{
  ShipId id = INVALID_SHIP_ID;
  std::uint8_t classId = 0;
  DirectX::XMFLOAT2 positionMetres{};
  DirectX::XMFLOAT2 velocityMetresPerSec{};
  float headingRadians = 0.0f;
  std::uint8_t hullPercent = 0;
  std::uint8_t shieldPercent = 0;

  /// Extrapolated past the cap and frozen. The icon sheet draws a marker for
  /// this; the point of surfacing it is that a frozen ship must never be
  /// mistaken for a stopped one.
  bool stale = false;
};

class ReplicatedView
{
public:
  /*
   * How much history to keep.
   *
   * Interpolation needs the two snapshots bracketing the render tick, which
   * sits two ticks behind the estimate. Eight snapshots is 400 ms at 20 Hz --
   * comfortably past the 250 ms extrapolation cap, so the buffer can never be
   * the reason a ship goes stale.
   */
  static constexpr std::size_t HISTORY = 8;

  /// Ticks of extrapolation before a ship freezes and is flagged. 250 ms at
  /// 20 Hz (ADR-002 §4), and the number the icon sheet's STALE marker means.
  static constexpr double MAX_EXTRAPOLATION_TICKS = 5.0;

  /// Applies one snapshot payload, exactly as it came off the wire. Returns
  /// false on a malformed or truncated payload, leaving the view untouched --
  /// a half-applied snapshot is a corrupt view, and the next one is 50 ms away.
  [[nodiscard]] bool ApplySnapshot(std::span<const std::uint8_t> _payload);

  /*
   * Samples the world at a fractional tick.
   *
   * Between two snapshots this interpolates; past the newest it extrapolates
   * from the last known velocity, for at most `MAX_EXTRAPOLATION_TICKS`, after
   * which the ship freezes where the extrapolation left it and is flagged
   * stale. Freezing rather than continuing is deliberate: a ship that kept
   * gliding on stale velocity would look right and be wrong, and would arrive
   * somewhere the server never put it.
   */
  void SampleAt(double _renderTick, std::vector<ReplicatedShip>& _outShips) const;

  [[nodiscard]] bool HasSnapshot() const noexcept { return m_count > 0; }
  [[nodiscard]] std::uint32_t LatestTick() const noexcept;
  [[nodiscard]] std::uint32_t OldestTick() const noexcept;
  [[nodiscard]] std::size_t SnapshotCount() const noexcept { return m_count; }

  /// Ships in the newest snapshot. The count the HUD reports, and not the same
  /// question as how many `SampleAt` returns at a given render tick.
  [[nodiscard]] std::uint16_t LatestShipCount() const noexcept;

  void Clear() noexcept;

private:
  struct Frame
  {
    SnapshotHeader header;
    std::vector<Neuron::EntityRecord> ships;
  };

  /// Newest first. A ring would save the copy; eight entries of a few hundred
  /// bytes, rotated twenty times a second, is not where this loop spends its
  /// time, and "index 0 is newest" is worth more than the copy costs.
  Frame m_frames[HISTORY];
  std::size_t m_count = 0;
};

} // namespace Game
