#include "pch.h"
#include "CppUnitTest.h"

#include "SnapshotBuffer.h"

#include <cmath>
#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The client's clock (ADR-002 §4), tested without a device or a network.
 *
 * Every case here is a *timing* scenario played through a fake clock: a steady
 * stream, a jittered one, a stall, a reorder, a discontinuity. None of them are
 * reproducible by hand at a keyboard, and all of them are what the player
 * actually experiences as smooth or not smooth -- which is the argument for the
 * buffer being device-free in the first place.
 */

namespace NeuronClientTests
{
namespace
{

constexpr double TICK_SECONDS = 0.05;         // 20 Hz server.
constexpr double FRAME_SECONDS = 1.0 / 144.0; // A fast display.

/// Runs a steady stream for `_seconds`, calling `_perFrame(buffer, renderTick)`.
template <typename PerFrame>
void RunSteadyStream(SnapshotBuffer& _buffer, double _seconds, std::uint32_t _firstTick, PerFrame&& _perFrame)
{
  double now = 0.0;
  double nextSnapshot = 0.0;
  std::uint32_t serverTick = _firstTick;

  const auto frames = static_cast<int>(_seconds / FRAME_SECONDS);
  for (int frame = 0; frame < frames; ++frame)
  {
    now += FRAME_SECONDS;
    while (now >= nextSnapshot)
    {
      _buffer.OnSnapshot(serverTick++, nextSnapshot);
      nextSnapshot += TICK_SECONDS;
    }
    _perFrame(_buffer.Advance(now), now);
  }
}

} // namespace

TEST_CLASS(SnapshotBufferTests)
{
public:
  TEST_METHOD(TheRenderTickNeverGoesBackwards)
  {
    // The one property everything else rests on. A render tick that stepped
    // backwards would run the interpolation in reverse: ships visibly reversing
    // for a frame, which reads as a physics bug and is a clock bug.
    SnapshotBuffer buffer;
    buffer.Configure(20);
    buffer.Reset();

    double previous = -1.0e30;
    int backwards = 0;
    RunSteadyStream(buffer, 20.0, 100, [&](double _renderTick, double) {
      if (_renderTick < previous)
      {
        ++backwards;
      }
      previous = _renderTick;
    });

    Assert::AreEqual(0, backwards);
    Assert::AreEqual<std::uint64_t>(0, buffer.SnapCount(), L"a steady stream should never need to snap");
  }

  TEST_METHOD(TheRenderTickIsExactlyTheInterpolationDelayBehind)
  {
    SnapshotBuffer buffer;
    buffer.Configure(20);
    buffer.Reset();
    RunSteadyStream(buffer, 5.0, 1, [](double, double) {});

    Assert::AreEqual(SnapshotBuffer::INTERP_DELAY_TICKS, buffer.EstimatedTick() - buffer.RenderTick(), 1e-9);
    Assert::IsTrue(buffer.HasEstimate());
  }

  TEST_METHOD(DriftStaysWellInsideATick)
  {
    SnapshotBuffer buffer;
    buffer.Configure(20);
    buffer.Reset();

    double worstDrift = 0.0;
    int frame = 0;
    RunSteadyStream(buffer, 20.0, 500, [&](double, double) {
      // Skip the first second: the estimate is still converging from its
      // starting snapshot, and that is not what this measures.
      if (++frame > 144)
      {
        worstDrift = std::max(worstDrift, std::fabs(buffer.DriftTicks()));
      }
    });

    Assert::IsTrue(worstDrift < 0.5, L"the estimate drifted more than half a tick from the truth");
  }

  TEST_METHOD(JitterIsAbsorbedRatherThanFollowed)
  {
    /*
     * The reason the estimate slews instead of tracking.
     *
     * Snapshots are delivered +/-15 ms early or late -- ordinary network
     * jitter, with the data itself perfect. Following arrival times directly
     * would push that jitter straight onto the render tick and make ships
     * stutter. The assertion is that each frame advances by very nearly the
     * same amount regardless.
     */
    SnapshotBuffer buffer;
    buffer.Configure(20);
    buffer.Reset();

    double now = 0.0;
    double nextSnapshot = 0.0;
    std::uint32_t serverTick = 500;
    double previousRender = -1.0e30;
    double worstDeviation = 0.0;
    const double steadyStep = FRAME_SECONDS / TICK_SECONDS;
    std::uint32_t noise = 12345;

    for (int frame = 0; frame < 144 * 20; ++frame)
    {
      now += FRAME_SECONDS;
      while (now >= nextSnapshot)
      {
        noise = noise * 1664525u + 1013904223u;
        const double jitter = static_cast<double>((noise >> 8) % 30000u) / 1.0e6 - 0.015;
        buffer.OnSnapshot(serverTick++, std::max(0.0, nextSnapshot + jitter));
        nextSnapshot += TICK_SECONDS;
      }

      const double render = buffer.Advance(now);
      if (previousRender > -1.0e29)
      {
        worstDeviation = std::max(worstDeviation, std::fabs((render - previousRender) - steadyStep));
        Assert::IsTrue(render >= previousRender, L"jitter rewound the render tick");
      }
      previousRender = render;
    }

    Assert::IsTrue(worstDeviation < steadyStep * 0.05, L"jitter reached the render tick instead of being absorbed");
  }

  TEST_METHOD(AStallKeepsTimeMovingAndFlagsItself)
  {
    // S7's induced-stall case. The clock must keep advancing -- the client is
    // still drawing -- and it must be able to say that what it is drawing is no
    // longer backed by fresh data.
    SnapshotBuffer buffer;
    buffer.Configure(20);
    buffer.Reset();

    double now = 0.0;
    double nextSnapshot = 0.0;
    std::uint32_t serverTick = 1000;
    for (int frame = 0; frame < 144 * 2; ++frame)
    {
      now += FRAME_SECONDS;
      while (now >= nextSnapshot)
      {
        buffer.OnSnapshot(serverTick++, nextSnapshot);
        nextSnapshot += TICK_SECONDS;
      }
      buffer.Advance(now);
    }

    Assert::IsFalse(buffer.Stale(now));
    const double beforeStall = buffer.RenderTick();

    // 400 ms with nothing arriving.
    for (int frame = 0; frame < static_cast<int>(0.4 / FRAME_SECONDS); ++frame)
    {
      now += FRAME_SECONDS;
      buffer.Advance(now);
    }

    Assert::IsTrue(buffer.Stale(now), L"a 400 ms gap is past the extrapolation cap");
    Assert::IsTrue(buffer.RenderTick() > beforeStall, L"the clock must keep moving while the stream is down");

    // The server kept ticking through the stall; the stream resumes there.
    serverTick += static_cast<std::uint32_t>(0.4 / TICK_SECONDS);
    nextSnapshot = now;
    for (int frame = 0; frame < 144 * 3; ++frame)
    {
      now += FRAME_SECONDS;
      while (now >= nextSnapshot)
      {
        buffer.OnSnapshot(serverTick++, nextSnapshot);
        nextSnapshot += TICK_SECONDS;
      }
      buffer.Advance(now);
    }

    Assert::IsFalse(buffer.Stale(now), L"recovery must clear the flag");
    Assert::IsTrue(std::fabs(buffer.DriftTicks()) < 0.5, L"recovery must converge, not merely stop failing");
  }

  TEST_METHOD(ReorderedAndDuplicateSnapshotsAreCountedAndIgnored)
  {
    // The state channel is unordered by design (ADR-003). Neither case is an
    // error; both are counted, because a rising number is the difference
    // between "the network is fine" and "the network is fine so far".
    SnapshotBuffer buffer;
    buffer.Configure(20);
    buffer.Reset();

    buffer.OnSnapshot(10, 0.0);
    buffer.Advance(0.0);
    const double afterFirst = buffer.EstimatedTick();

    buffer.OnSnapshot(9, 0.05);  // Older.
    buffer.OnSnapshot(10, 0.05); // Duplicate.
    buffer.OnSnapshot(11, 0.05); // Genuinely newer.

    Assert::AreEqual<std::uint64_t>(4, buffer.SnapshotCount());
    Assert::AreEqual<std::uint64_t>(2, buffer.OutOfOrderCount());
    Assert::IsTrue(buffer.EstimatedTick() >= afterFirst, L"an old snapshot must not drag the estimate back");
  }

  TEST_METHOD(ADiscontinuitySnapsRatherThanCrawling)
  {
    // Slewing at 2 % across a large gap would take minutes. Snapping is
    // visible, and visible beats a fleet gliding through positions it never
    // occupied for the next several minutes.
    SnapshotBuffer buffer;
    buffer.Configure(20);
    buffer.Reset();

    buffer.OnSnapshot(100, 0.0);
    buffer.Advance(0.0);
    Assert::AreEqual<std::uint64_t>(0, buffer.SnapCount());

    buffer.OnSnapshot(100000, 0.05);
    buffer.Advance(0.05);

    Assert::AreEqual<std::uint64_t>(1, buffer.SnapCount());
    Assert::IsTrue(std::fabs(buffer.EstimatedTick() - 100000.0) < 5.0, L"the snap should land on the new tick");
  }

  TEST_METHOD(AnAbsurdTickRateFallsBackRatherThanDividingByZero)
  {
    // The rate comes off the wire in `Welcome`. A server that reported nothing
    // must not take the client's clock down with it.
    SnapshotBuffer zero;
    zero.Configure(0);
    Assert::AreEqual(0.05, zero.TickSeconds(), 1e-9);

    SnapshotBuffer absurd;
    absurd.Configure(60000);
    Assert::AreEqual(0.05, absurd.TickSeconds(), 1e-9);

    SnapshotBuffer honest;
    honest.Configure(60);
    Assert::AreEqual(1.0 / 60.0, honest.TickSeconds(), 1e-9);
  }

  TEST_METHOD(NothingHappensBeforeTheFirstSnapshot)
  {
    // A client that has connected but not yet received anything must not
    // invent a time to render at.
    SnapshotBuffer buffer;
    buffer.Configure(20);
    buffer.Reset();

    Assert::IsFalse(buffer.HasEstimate());
    Assert::IsFalse(buffer.Stale(10.0), L"no data is not the same as stale data");
    Assert::AreEqual(buffer.RenderTick(), buffer.Advance(5.0), 1e-9, L"time must not advance from nothing");
  }
};

} // namespace NeuronClientTests
