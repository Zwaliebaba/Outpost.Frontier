#include "pch.h"

#include "SnapshotBuffer.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{
namespace
{

/// How hard the estimate is pulled toward the target, per second of error. The
/// clamp above is what actually bounds the correction; this only decides how
/// quickly a small error reaches that clamp, and half a tick per second is slow
/// enough to be invisible and fast enough to matter.
constexpr double SLEW_GAIN_PER_TICK_OF_ERROR = 0.5;

} // namespace

void SnapshotBuffer::Configure(std::uint16_t _tickRateHz) noexcept
{
  // A server that reported nothing, or something absurd, gets the design's
  // rate rather than a division by zero three call frames away (ADR-002 §1).
  const std::uint16_t rate = (_tickRateHz >= 1 && _tickRateHz <= 240) ? _tickRateHz : 20;
  m_tickSeconds = 1.0 / static_cast<double>(rate);
}

void SnapshotBuffer::Reset() noexcept
{
  m_estimatedTick = 0.0;
  m_driftTicks = 0.0;
  m_latestServerTick = 0;
  m_latestArrivalSeconds = 0.0;
  m_lastAdvanceSeconds = 0.0;
  m_hasEstimate = false;
  m_snapshotCount = 0;
  m_outOfOrderCount = 0;
  m_snapCount = 0;
}

void SnapshotBuffer::OnSnapshot(std::uint32_t _serverTick, double _nowSeconds) noexcept
{
  ++m_snapshotCount;

  if (m_hasEstimate && _serverTick <= m_latestServerTick)
  {
    // Reordered or duplicated. The channel is unordered by design (ADR-003) and
    // full snapshots are idempotent, so this costs nothing -- but it is counted,
    // because a rising number is the difference between "the network is fine"
    // and "the network is fine so far".
    ++m_outOfOrderCount;
    return;
  }

  m_latestServerTick = _serverTick;
  m_latestArrivalSeconds = _nowSeconds;

  if (!m_hasEstimate)
  {
    // First snapshot: there is nothing to slew from. Start at the tick that
    // arrived and let `Advance` carry it forward.
    m_estimatedTick = static_cast<double>(_serverTick);
    m_lastAdvanceSeconds = _nowSeconds;
    m_driftTicks = 0.0;
    m_hasEstimate = true;
    return;
  }

  const double error = static_cast<double>(_serverTick) - m_estimatedTick;
  if (std::fabs(error) > SNAP_THRESHOLD_TICKS)
  {
    // Too far to slew: a long stall, a resumed process, or a server restart.
    // Snapping is visible, and it is the honest answer -- pretending to
    // interpolate across a gap this size would show a fleet gliding through
    // positions it never occupied.
    m_estimatedTick = static_cast<double>(_serverTick);
    m_driftTicks = 0.0;
    ++m_snapCount;
  }
}

double SnapshotBuffer::Advance(double _nowSeconds) noexcept
{
  if (!m_hasEstimate)
  {
    return RenderTick();
  }

  const double elapsed = std::max(0.0, _nowSeconds - m_lastAdvanceSeconds);
  m_lastAdvanceSeconds = _nowSeconds;

  // The naive estimate: where the server must be by now, given when its last
  // snapshot arrived. Correct on average and jittery frame to frame, which is
  // exactly why it is a *target* rather than the answer.
  const double age = std::max(0.0, _nowSeconds - m_latestArrivalSeconds);
  const double targetTick = static_cast<double>(m_latestServerTick) + age / m_tickSeconds;

  m_driftTicks = m_estimatedTick - targetTick;

  // Run at the server's rate, bent by at most a couple of percent toward the
  // target. Time only ever moves forward: the correction adjusts the rate, it
  // never subtracts from the estimate, because a render tick that went
  // backwards would run the interpolation in reverse.
  const double correction = std::clamp(-m_driftTicks * SLEW_GAIN_PER_TICK_OF_ERROR, -MAX_RATE_CORRECTION, MAX_RATE_CORRECTION);
  m_estimatedTick += (elapsed / m_tickSeconds) * (1.0 + correction);
  return RenderTick();
}

double SnapshotBuffer::SecondsSinceSnapshot(double _nowSeconds) const noexcept
{
  return m_hasEstimate ? std::max(0.0, _nowSeconds - m_latestArrivalSeconds) : 0.0;
}

} // namespace Neuron
