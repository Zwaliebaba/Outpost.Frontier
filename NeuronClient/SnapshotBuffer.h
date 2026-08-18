#pragma once

#include <cstdint>

/*
 * The client's estimate of server time (ADR-002 §4).
 *
 * Snapshots arrive twenty times a second and the client draws as often as the
 * display allows -- seven frames per snapshot at 144 Hz. Something has to turn
 * a sequence of arrivals into a smooth, continuously advancing time to render
 * at, and that something is this. It is the *only* place the client decides
 * *when* it is looking at; what the world looks like at that instant is the
 * game's answer, through `WorldView::BuildScene`.
 *
 * **Engine-side on purpose.** None of this arithmetic knows what a ship is: it
 * is the same in any game that replicates state at a fixed rate, which is what
 * makes it NeuronClient's rather than GameLogic's (ADR-014). It is also free of
 * D3D12 and Windows, so the timing behaviour that is hardest to eyeball -- what
 * a 400 ms stall does to the render tick -- is testable without a device.
 *
 * **Why it slews rather than tracks.** The naive estimate,
 * `latestTick + age / tickSeconds`, is correct on average and jumps on every
 * arrival: network jitter lands snapshots early and late, and following that
 * directly makes ships stutter even though the data is perfect. So the estimate
 * runs at its own steady rate and is nudged toward the target by at most a
 * couple of percent -- fast enough to absorb real drift within a second or two,
 * slow enough that no single late packet is visible. The cost is that a genuine
 * discontinuity takes a moment to catch up, which is why a large error snaps
 * instead: slewing across a five-second gap would take minutes.
 */

namespace Neuron
{

class SnapshotBuffer
{
public:
  /// Render this far behind the estimate, so there is normally a newer snapshot
  /// to interpolate toward (ADR-002 §4). Two ticks is 100 ms at 20 Hz: one tick
  /// of expected spacing plus one of slack for jitter.
  static constexpr double INTERP_DELAY_TICKS = 2.0;

  /// Beyond this the client is extrapolating rather than interpolating, and the
  /// icon sheet's STALE marker is earned. 250 ms (ADR-002 §4).
  static constexpr double MAX_EXTRAPOLATION_SECONDS = 0.25;

  /// How far the estimate's rate may be bent to chase the target: +/-2 %, which
  /// is imperceptible frame to frame and closes a tick of drift in a few
  /// seconds.
  static constexpr double MAX_RATE_CORRECTION = 0.02;

  /// Past this the estimate snaps instead of slewing. Chosen as the point where
  /// slewing would take longer than a player will wait: at 2 % it takes fifty
  /// seconds to absorb one second of error.
  static constexpr double SNAP_THRESHOLD_TICKS = 10.0;

  /// The server's tick rate, learned from `Welcome`. Zero or absurd values fall
  /// back to 20 Hz rather than dividing by zero somewhere less obvious.
  void Configure(std::uint16_t _tickRateHz) noexcept;

  /// Forgets everything. The next snapshot re-establishes the estimate by
  /// snapping to it, which is what a fresh join should do.
  void Reset() noexcept;

  /// A snapshot arrived. `_nowSeconds` is the client's own monotonic clock --
  /// the two machines never have to agree on it, only on the tick.
  void OnSnapshot(std::uint32_t _serverTick, double _nowSeconds) noexcept;

  /// Advances the estimate to now. Call once per frame, before building the
  /// scene; returns the tick to render at.
  double Advance(double _nowSeconds) noexcept;

  /// `t_est - InterpDelay`, the fractional tick the scene is built at.
  [[nodiscard]] double RenderTick() const noexcept { return m_estimatedTick - INTERP_DELAY_TICKS; }

  [[nodiscard]] double EstimatedTick() const noexcept { return m_estimatedTick; }
  [[nodiscard]] bool HasEstimate() const noexcept { return m_hasEstimate; }

  /// Estimate minus the naive target, in ticks. The debug HUD's drift row: near
  /// zero in steady state, and the first thing to look at when motion is wrong.
  [[nodiscard]] double DriftTicks() const noexcept { return m_driftTicks; }

  /// Seconds since the last snapshot arrived. Past `MAX_EXTRAPOLATION_SECONDS`
  /// the world on screen is frozen and flagged, not merely late.
  [[nodiscard]] double SecondsSinceSnapshot(double _nowSeconds) const noexcept;
  [[nodiscard]] bool Stale(double _nowSeconds) const noexcept
  {
    return m_hasEstimate && SecondsSinceSnapshot(_nowSeconds) > MAX_EXTRAPOLATION_SECONDS;
  }

  /// How many snapshots have been seen, and how many arrived out of order or
  /// duplicated. Both are debug-strip rows, and the second is the honest
  /// measure of an unordered channel (ADR-003).
  [[nodiscard]] std::uint64_t SnapshotCount() const noexcept { return m_snapshotCount; }
  [[nodiscard]] std::uint64_t OutOfOrderCount() const noexcept { return m_outOfOrderCount; }

  /// How many times the estimate had to snap rather than slew. Non-zero means
  /// the client saw a discontinuity worth explaining.
  [[nodiscard]] std::uint64_t SnapCount() const noexcept { return m_snapCount; }

  [[nodiscard]] double TickSeconds() const noexcept { return m_tickSeconds; }

private:
  double m_tickSeconds = 0.05;
  double m_estimatedTick = 0.0;
  double m_driftTicks = 0.0;

  std::uint32_t m_latestServerTick = 0;
  double m_latestArrivalSeconds = 0.0;
  double m_lastAdvanceSeconds = 0.0;

  bool m_hasEstimate = false;

  std::uint64_t m_snapshotCount = 0;
  std::uint64_t m_outOfOrderCount = 0;
  std::uint64_t m_snapCount = 0;
};

} // namespace Neuron
