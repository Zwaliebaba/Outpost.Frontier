#include "pch.h"
#include "CppUnitTest.h"

#include "Clock.h"
#include "RingBuffer.h"
#include "TaskPool.h"
#include "Telemetry.h"

#include <DirectXMath.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * Tasking and telemetry (ADR-007 §4, §8).
 *
 * The telemetry registry is process-wide and lanes are never released, so these
 * tests are careful about two things: they register under names nothing else
 * uses, and they never call Telemetry::Reset while another test's threads could
 * still be recording. A test that fights the shared registry proves nothing
 * about the registry.
 */

namespace NeuronCoreTests
{
namespace
{

/// Lane names are roles, and these are this file's roles alone.
constexpr const char* SPAN_LANE = "TestSpans";
constexpr const char* COUNTER_LANE = "TestCounters";

/// Drains a lane completely and returns what was in it.
std::vector<TelemetryEvent> DrainLane(LaneId _lane)
{
  std::vector<TelemetryEvent> events;
  TelemetryEvent event;
  while (Telemetry::Drain(_lane, event))
  {
    events.push_back(event);
  }
  return events;
}

} // namespace

TEST_CLASS(MpscRingBufferTests)
{
public:
  TEST_METHOD(PopsWhatWasPushed)
  {
    MpscRingBuffer<std::uint32_t, 8> ring;
    Assert::IsTrue(ring.Empty());

    for (std::uint32_t i = 0; i < 8; ++i)
    {
      Assert::IsTrue(ring.TryPush(i), L"a fresh ring rejected a push");
    }
    Assert::IsFalse(ring.TryPush(99), L"a full ring accepted a ninth item");
    Assert::AreEqual<std::uint64_t>(1, ring.DroppedCount());

    for (std::uint32_t i = 0; i < 8; ++i)
    {
      std::uint32_t value = 0xffffffffu;
      Assert::IsTrue(ring.TryPop(value));
      Assert::AreEqual(i, value); // Single producer: order is preserved.
    }

    std::uint32_t drained = 0;
    Assert::IsFalse(ring.TryPop(drained));
    Assert::IsTrue(ring.Empty());
  }

  TEST_METHOD(WrapsAroundWithoutLosingItems)
  {
    // The sequence numbers are the whole trick; this is the lap that proves a
    // slot is only reclaimed after its item was consumed.
    MpscRingBuffer<std::uint32_t, 4> ring;
    for (std::uint32_t lap = 0; lap < 100; ++lap)
    {
      Assert::IsTrue(ring.TryPush(lap));
      std::uint32_t value = 0;
      Assert::IsTrue(ring.TryPop(value));
      Assert::AreEqual(lap, value);
    }
  }

  TEST_METHOD(SurvivesFourProducersAndOneConsumer)
  {
    // What the SPSC ring cannot do, and the reason this type exists: several
    // threads pushing at once without corrupting each other's slots. The
    // consumer is this thread, so every assertion runs where the framework can
    // report it.
    constexpr std::uint32_t PRODUCER_COUNT = 4;
    constexpr std::uint32_t PER_PRODUCER = 20000;
    constexpr std::uint32_t TOTAL = PRODUCER_COUNT * PER_PRODUCER;

    static MpscRingBuffer<std::uint32_t, 256> ring;
    std::vector<std::thread> producers;
    for (std::uint32_t p = 0; p < PRODUCER_COUNT; ++p)
    {
      producers.emplace_back(
        [p]
        {
          for (std::uint32_t i = 0; i < PER_PRODUCER; ++i)
          {
            // Encode which producer sent it, so the consumer can prove nothing
            // was invented, torn, or reordered within a producer.
            const std::uint32_t item = (p << 24) | i;
            while (!ring.TryPush(item))
            {
              std::this_thread::yield(); // Full: wait for the consumer.
            }
          }
        });
    }

    std::uint32_t received = 0;
    std::uint32_t nextExpected[PRODUCER_COUNT]{};
    while (received < TOTAL)
    {
      std::uint32_t item = 0;
      if (!ring.TryPop(item))
      {
        std::this_thread::yield();
        continue;
      }
      const std::uint32_t producer = item >> 24;
      const std::uint32_t index = item & 0x00ffffffu;
      Assert::IsTrue(producer < PRODUCER_COUNT, L"an item arrived from a producer that does not exist");
      Assert::AreEqual(nextExpected[producer], index, L"a producer's items arrived out of order or were lost");
      ++nextExpected[producer];
      ++received;
    }

    for (std::thread& producer : producers)
    {
      producer.join();
    }

    Assert::AreEqual(TOTAL, received);
    Assert::IsTrue(ring.Empty());
  }
};

TEST_CLASS(TelemetryTests)
{
public:
  TEST_METHOD(ARegisteredLaneRecordsAndDrains)
  {
    const LaneId lane = Telemetry::RegisterLane(SPAN_LANE);
    Assert::IsTrue(lane != INVALID_LANE, L"the registry refused a lane");
    Assert::IsTrue(std::strcmp(SPAN_LANE, Telemetry::LaneName(lane)) == 0, L"the lane came back under a different name");
    Assert::AreEqual(lane, Telemetry::CurrentLane());

    (void)DrainLane(lane); // Whatever an earlier test left.

    NEURON_COUNTER("UnitTestCounter", 7);
    const std::vector<TelemetryEvent> events = DrainLane(lane);

    Assert::AreEqual<std::size_t>(1, events.size());
    Assert::IsTrue(events[0].kind == TelemetryKind::Counter);
    Assert::AreEqual<std::int64_t>(7, events[0].value);
  }

  TEST_METHOD(RegisteringTheSameNameTwiceReturnsTheSameLane)
  {
    // A lane is a role, not a thread: a restarted pool must reuse "Worker 0"
    // rather than spend another lane on it.
    const LaneId first = Telemetry::RegisterLane(SPAN_LANE);
    const LaneId again = Telemetry::RegisterLane(SPAN_LANE);
    Assert::AreEqual(first, again);

    // ...and a different name from this same thread does not steal a lane,
    // because this thread already has one.
    const LaneId other = Telemetry::RegisterLane(COUNTER_LANE);
    Assert::AreEqual(first, other);
  }

  TEST_METHOD(AnUnregisteredThreadRecordsNothingAndDoesNotFault)
  {
    // Telemetry never decides whether a thread may run. The result is carried
    // back and asserted here, so a failure is a failed test rather than a
    // terminate() from a thread the framework is not watching.
    LaneId observed = 0;
    std::thread stranger(
      [&observed]
      {
        observed = Telemetry::CurrentLane();
        NEURON_COUNTER("FromNowhere", 1);
        NEURON_SPAN("AlsoFromNowhere");
      });
    stranger.join();
    Assert::AreEqual(INVALID_LANE, observed, L"a thread that never registered was given a lane");
  }

  TEST_METHOD(SpanMeasuresRoughlyTheTimeItsScopeTook)
  {
    // The S2 acceptance item: span timing sanity. The bounds are loose on
    // purpose -- what is being checked is that a span measures its own scope
    // and reports it in counter ticks, not that a shared CI runner sleeps
    // accurately.
    const LaneId lane = Telemetry::RegisterLane(SPAN_LANE);
    (void)DrainLane(lane);

    {
      NEURON_SPAN("UnitTestSpan");
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    const std::vector<TelemetryEvent> events = DrainLane(lane);
    Assert::AreEqual<std::size_t>(1, events.size());
    Assert::IsTrue(events[0].kind == TelemetryKind::Span);

    const double milliseconds = TelemetrySnapshot::Milliseconds(events[0].value);
    Assert::IsTrue(milliseconds >= 25.0, L"a 30 ms scope measured as far too short");
    Assert::IsTrue(milliseconds < 2000.0, L"a 30 ms scope measured as absurdly long");
  }

  TEST_METHOD(NestedSpansBothRecordAndTheOuterIsLonger)
  {
    const LaneId lane = Telemetry::RegisterLane(SPAN_LANE);
    (void)DrainLane(lane);

    {
      NEURON_SPAN("Outer");
      {
        NEURON_SPAN("Inner");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TelemetrySnapshot snapshot;
    snapshot.DrainLane(lane);

    const TelemetryStat* outer = snapshot.Find("Outer");
    const TelemetryStat* inner = snapshot.Find("Inner");
    Assert::IsNotNull(outer);
    Assert::IsNotNull(inner);
    Assert::IsTrue(outer->total > inner->total, L"the outer span did not contain the inner one");
  }

  TEST_METHOD(SnapshotAggregatesRepeatsByName)
  {
    const LaneId lane = Telemetry::RegisterLane(SPAN_LANE);
    (void)DrainLane(lane);

    for (int i = 0; i < 5; ++i)
    {
      NEURON_COUNTER("Repeated", 3);
    }

    TelemetrySnapshot snapshot;
    snapshot.DrainLane(lane);

    const TelemetryStat* stat = snapshot.Find("Repeated");
    Assert::IsNotNull(stat);
    Assert::AreEqual<std::uint32_t>(5, stat->count);
    Assert::AreEqual<std::int64_t>(15, stat->total);
    Assert::AreEqual<std::int64_t>(3, stat->maximum);
    Assert::AreEqual<std::uint32_t>(0, snapshot.OverflowCount());
  }

  TEST_METHOD(AFullLaneDropsAndCountsRatherThanBlocking)
  {
    const LaneId lane = Telemetry::RegisterLane(SPAN_LANE);
    (void)DrainLane(lane);

    const std::uint64_t before = Telemetry::DroppedCount(lane);
    for (std::size_t i = 0; i < Telemetry::LANE_CAPACITY + 64; ++i)
    {
      NEURON_COUNTER("Flood", 1);
    }
    Assert::IsTrue(Telemetry::DroppedCount(lane) > before, L"an overfull lane did not count its drops");

    (void)DrainLane(lane);
  }
};

TEST_CLASS(TaskPoolTests)
{
public:
  TEST_METHOD(RunsEverySubmittedTask)
  {
    TaskPool pool;
    Assert::IsTrue(pool.Start(4));
    Assert::AreEqual<std::uint32_t>(4, pool.WorkerCount());

    std::atomic<std::uint32_t> ran{0};
    WaitGroup group;
    for (int i = 0; i < 256; ++i)
    {
      pool.Submit([&] { ran.fetch_add(1, std::memory_order_relaxed); }, &group);
    }

    pool.Wait(group);
    Assert::AreEqual<std::uint32_t>(256, ran.load());
    Assert::IsTrue(group.Finished());

    pool.Stop();
    Assert::IsFalse(pool.Running());
    Assert::AreEqual<std::uint64_t>(256, pool.CompletedCount());
  }

  TEST_METHOD(WorksWithNoWorkersAtAll)
  {
    // A single-core machine gets zero workers, and that must still work: Wait
    // runs the queue on the calling thread. If this ever hangs, the pool has
    // grown a dependency on somebody else making progress.
    TaskPool pool; // Never started, so there is nobody but this thread.
    Assert::AreEqual<std::uint32_t>(0, pool.WorkerCount());

    std::atomic<int> ran{0};
    WaitGroup group;
    for (int i = 0; i < 32; ++i)
    {
      pool.Submit([&] { ran.fetch_add(1); }, &group);
    }

    pool.Wait(group);
    Assert::AreEqual(32, ran.load());
    Assert::IsTrue(group.Finished());
  }

  TEST_METHOD(WaitGroupCountsDownExactlyOnce)
  {
    TaskPool pool;
    Assert::IsTrue(pool.Start(2));

    WaitGroup group;
    Assert::IsTrue(group.Finished());

    std::atomic<std::uint32_t> ran{0};
    for (int i = 0; i < 64; ++i)
    {
      pool.Submit([&] { ran.fetch_add(1, std::memory_order_relaxed); }, &group);
    }
    pool.Wait(group);

    Assert::AreEqual<std::uint32_t>(0, group.Outstanding());
    Assert::AreEqual<std::uint32_t>(64, ran.load());
    pool.Stop();
  }

  TEST_METHOD(ATaskThatThrowsDoesNotKillTheWorkerOrHangTheGroup)
  {
    // A throwing task used to be able to take a worker with it, which shows up
    // as a boot that never finishes and says nothing.
    TaskPool pool;
    Assert::IsTrue(pool.Start(2));

    std::atomic<std::uint32_t> ran{0};
    WaitGroup group;
    for (int i = 0; i < 16; ++i)
    {
      pool.Submit(
        [&, i]
        {
          ran.fetch_add(1, std::memory_order_relaxed);
          if (i % 4 == 0)
          {
            throw std::runtime_error("deliberate");
          }
        },
        &group);
    }

    pool.Wait(group);
    Assert::AreEqual<std::uint32_t>(16, ran.load());
    Assert::IsTrue(group.Finished(), L"a throwing task left its wait group outstanding");

    // The pool is still usable afterwards.
    std::atomic<int> after{0};
    WaitGroup second;
    pool.Submit([&] { after.fetch_add(1); }, &second);
    pool.Wait(second);
    Assert::AreEqual(1, after.load());

    pool.Stop();
  }

  TEST_METHOD(StopFinishesQueuedWorkRatherThanDiscardingIt)
  {
    TaskPool pool;
    Assert::IsTrue(pool.Start(1));

    std::atomic<std::uint32_t> ran{0};
    for (int i = 0; i < 64; ++i)
    {
      pool.Submit([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    }

    pool.Stop(); // Drains first: a half-baked atlas is worse than a slow exit.
    Assert::AreEqual<std::uint32_t>(64, ran.load());
  }

  TEST_METHOD(SurvivesBeingRestarted)
  {
    // Restarting must not leak telemetry lanes -- "Worker 0" is a role, and the
    // registry cap is 16.
    TaskPool pool;
    for (int cycle = 0; cycle < 4; ++cycle)
    {
      Assert::IsTrue(pool.Start(2));
      std::atomic<int> ran{0};
      WaitGroup group;
      pool.Submit([&] { ran.fetch_add(1); }, &group);
      pool.Wait(group);
      Assert::AreEqual(1, ran.load());
      pool.Stop();
    }
    Assert::IsTrue(Telemetry::LaneCount() <= Telemetry::MAX_LANES, L"restarting the pool leaked lanes");
  }

  TEST_METHOD(ADefaultPoolLeavesLanesForTheNamedRoles)
  {
    /*
     * The default sizing must fit the lane budget with room to spare.
     *
     * It used to be hardware_concurrency - 1 flat, so a twenty-core machine
     * asked for nineteen lanes against a cap of sixteen: five workers refused,
     * five error lines at every single startup, and five threads baking content
     * that no measurement could see. The machine this runs on decides whether
     * that reproduces, which is exactly why the bound is asserted rather than
     * left to whoever next opens the log on a big box.
     */
    Assert::IsTrue(TaskPool::DefaultWorkerCount() + 2u <= Telemetry::MAX_LANES,
                   L"a default pool must leave lanes for 'Main' and 'Sim'");
  }
};

TEST_CLASS(CpuSupportTests)
{
public:
  TEST_METHOD(TheCpuSupportsWhatThisBuildWasCompiledFor)
  {
    // The S2 acceptance gate (ADR-010, Risk R11). If this fails, /arch and the
    // machine disagree and every DirectXMath call is undefined -- the boot path
    // checks the same thing before it computes anything.
    Assert::IsTrue(DirectX::XMVerifyCPUSupport(), L"this CPU cannot run the instruction set this build targets");
  }
};

} // namespace NeuronCoreTests
