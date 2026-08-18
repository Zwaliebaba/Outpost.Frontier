#include "pch.h"
#include "CppUnitTest.h"

#include "Arena.h"
#include "ByteReader.h"
#include "ByteWriter.h"
#include "EntityRecord.h"
#include "Hash.h"
#include "Random.h"
#include "RingBuffer.h"
#include "Clock.h"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

namespace NeuronCoreTests
{

TEST_CLASS(ByteIoTests)
{
public:
  TEST_METHOD(RoundTripsEveryScalarWidth)
  {
    std::uint8_t storage[64]{};
    ByteWriter writer{std::span<std::uint8_t>{storage}};

    writer.WriteUInt8(0xab);
    writer.WriteInt8(-42);
    writer.WriteUInt16(0xbeef);
    writer.WriteInt16(-12345);
    writer.WriteUInt32(0xdeadbeef);
    writer.WriteInt32(-2000000000);
    writer.WriteUInt64(0x0123456789abcdefull);
    writer.WriteInt64(-9000000000000000000ll);
    writer.WriteFloat(0.5f);
    writer.WriteBool(true);
    Assert::IsTrue(writer.Ok());

    ByteReader reader{writer.Written()};
    Assert::AreEqual<std::uint8_t>(0xab, reader.ReadUInt8());
    Assert::AreEqual<std::int8_t>(-42, reader.ReadInt8());
    Assert::AreEqual<std::uint16_t>(0xbeef, reader.ReadUInt16());
    Assert::AreEqual<std::int16_t>(-12345, reader.ReadInt16());
    Assert::AreEqual<std::uint32_t>(0xdeadbeef, reader.ReadUInt32());
    Assert::AreEqual<std::int32_t>(-2000000000, reader.ReadInt32());
    Assert::AreEqual<std::uint64_t>(0x0123456789abcdefull, reader.ReadUInt64());
    Assert::AreEqual<std::int64_t>(-9000000000000000000ll, reader.ReadInt64());
    Assert::AreEqual(0.5f, reader.ReadFloat());
    Assert::IsTrue(reader.ReadBool());
    Assert::IsTrue(reader.FullyConsumed());
  }

  TEST_METHOD(WriterFlagsOverflowAndYieldsNothing)
  {
    std::uint8_t storage[4]{};
    ByteWriter writer{std::span<std::uint8_t>{storage}};

    writer.WriteUInt32(1);
    Assert::IsTrue(writer.Ok());
    writer.WriteUInt32(2); // One byte too many.
    Assert::IsTrue(writer.Overflowed());
    // An overflowed buffer must not be sendable, or a truncated message goes out.
    Assert::AreEqual<std::size_t>(0, writer.Written().size());
  }

  TEST_METHOD(ReaderUnderrunsInsteadOfReadingPastTheEnd)
  {
    const std::uint8_t storage[2]{0x01, 0x02};
    ByteReader reader{std::span<const std::uint8_t>{storage}};

    Assert::AreEqual<std::uint16_t>(0x0201, reader.ReadUInt16());
    Assert::IsTrue(reader.Ok());

    const std::uint32_t past = reader.ReadUInt32();
    Assert::AreEqual<std::uint32_t>(0, past); // Zero, not garbage.
    Assert::IsTrue(reader.Underflowed());
  }

  TEST_METHOD(TrailingBytesAreNotFullyConsumed)
  {
    const std::uint8_t storage[4]{1, 2, 3, 4};
    ByteReader reader{std::span<const std::uint8_t>{storage}};
    (void)reader.ReadUInt16();
    Assert::IsTrue(reader.Ok());
    Assert::IsFalse(reader.FullyConsumed());
  }

  TEST_METHOD(TextRoundTripsAndRejectsAShortBuffer)
  {
    std::uint8_t storage[32]{};
    ByteWriter writer{std::span<std::uint8_t>{storage}};
    writer.WriteText("frontier");
    Assert::IsTrue(writer.Ok());

    ByteReader reader{writer.Written()};
    Assert::IsTrue(reader.ReadText() == "frontier");
    Assert::IsTrue(reader.FullyConsumed());

    const std::uint8_t truncated[3]{0x10, 0x00, 0x41}; // Claims 16 bytes, carries one.
    ByteReader shortReader{std::span<const std::uint8_t>{truncated}};
    Assert::IsTrue(shortReader.ReadText().empty());
    Assert::IsTrue(shortReader.Underflowed());
  }
};

TEST_CLASS(EntityRecordTests)
{
public:
  TEST_METHOD(RecordIsTwentyBytesOnTheWire)
  {
    std::uint8_t storage[64]{};
    ByteWriter writer{std::span<std::uint8_t>{storage}};
    WriteEntityRecord(writer, EntityRecord{});
    Assert::AreEqual(EntityRecordBytes, writer.BytesWritten());
  }

  TEST_METHOD(RoundTripsThroughTheWire)
  {
    EntityRecord source{};
    source.id = 4242;
    source.typeId = 7;
    source.flags = 0x03;
    source.posXCm = -123456;
    source.posYCm = 987654;
    source.velXCmPerSec = -3210;
    source.velYCmPerSec = 4560;
    source.headingTurns16 = 40000;
    source.gaugeA = 200;
    source.gaugeB = 100;

    std::uint8_t storage[64]{};
    ByteWriter writer{std::span<std::uint8_t>{storage}};
    WriteEntityRecord(writer, source);

    ByteReader reader{writer.Written()};
    const EntityRecord result = ReadEntityRecord(reader);

    Assert::IsTrue(reader.FullyConsumed());
    Assert::AreEqual(source.id, result.id);
    Assert::AreEqual(source.typeId, result.typeId);
    Assert::AreEqual(source.flags, result.flags);
    Assert::AreEqual(source.posXCm, result.posXCm);
    Assert::AreEqual(source.posYCm, result.posYCm);
    Assert::AreEqual(source.velXCmPerSec, result.velXCmPerSec);
    Assert::AreEqual(source.velYCmPerSec, result.velYCmPerSec);
    Assert::AreEqual(source.headingTurns16, result.headingTurns16);
    Assert::AreEqual(source.gaugeA, result.gaugeA);
    Assert::AreEqual(source.gaugeB, result.gaugeB);
  }

  TEST_METHOD(QuantisationIsSymmetricAboutTheOrigin)
  {
    // Truncation would bias both sides toward zero and let a formation solved on
    // the client disagree with the server's (ADR-004 parity rule).
    Assert::AreEqual<std::int32_t>(150, MetresToCentimetres(1.5f));
    Assert::AreEqual<std::int32_t>(-150, MetresToCentimetres(-1.5f));
    Assert::AreEqual<std::int32_t>(1, MetresToCentimetres(0.006f));
    Assert::AreEqual<std::int32_t>(-1, MetresToCentimetres(-0.006f));
    Assert::AreEqual(2.5f, CentimetresToMetres(250));
  }

  TEST_METHOD(HeadingWrapsRatherThanOverflowing)
  {
    Assert::AreEqual<std::uint16_t>(0, RadiansToHeading(0.0f));
    Assert::AreEqual<std::uint16_t>(16384, RadiansToHeading(1.5707963f));   // quarter turn
    Assert::AreEqual<std::uint16_t>(0, RadiansToHeading(6.2831853f));       // full turn wraps to zero
    Assert::AreEqual<std::uint16_t>(49152, RadiansToHeading(-1.5707963f));  // negative wraps forward

    const float radians = HeadingToRadians(16384);
    Assert::IsTrue(radians > 1.5707f && radians < 1.5709f);
  }
};

TEST_CLASS(HashTests)
{
public:
  TEST_METHOD(MatchesKnownFnv1aVectors)
  {
    // Published FNV-1a 64 vectors: a wrong constant or a wrong order fails here
    // rather than silently changing every schema hash in the protocol.
    Assert::AreEqual<std::uint64_t>(0xcbf29ce484222325ull, HashText(""));
    Assert::AreEqual<std::uint64_t>(0xaf63dc4c8601ec8cull, HashText("a"));
    Assert::AreEqual<std::uint64_t>(0x85944171f73967e8ull, HashText("foobar"));
  }

  TEST_METHOD(IsUsableAtCompileTime)
  {
    // The schema hash is built from a constexpr string, so this must hold.
    static_assert(HashText("outpost") != 0);
    constexpr std::uint64_t hash = HashText("outpost");
    Assert::AreEqual(HashText("outpost"), hash);
  }

  TEST_METHOD(AccumulatesInOrder)
  {
    const std::uint64_t combined = HashText("bc", HashText("a"));
    Assert::AreEqual(HashText("abc"), combined);
    Assert::AreNotEqual(HashText("cba"), combined);
  }
};

TEST_CLASS(RandomTests)
{
public:
  TEST_METHOD(SameSeedGivesTheSameSequence)
  {
    Pcg32 first{12345};
    Pcg32 second{12345};
    for (int i = 0; i < 100; ++i)
    {
      Assert::AreEqual(first.Next(), second.Next());
    }
  }

  TEST_METHOD(DifferentSeedsDiverge)
  {
    Pcg32 first{1};
    Pcg32 second{2};
    bool differed = false;
    for (int i = 0; i < 8 && !differed; ++i)
    {
      differed = first.Next() != second.Next();
    }
    Assert::IsTrue(differed);
  }

  TEST_METHOD(StaysWithinBounds)
  {
    Pcg32 random{99};
    for (int i = 0; i < 1000; ++i)
    {
      const std::uint32_t value = random.NextBelow(10);
      Assert::IsTrue(value < 10);

      const float unit = random.NextUnitFloat();
      Assert::IsTrue(unit >= 0.0f && unit < 1.0f);
    }
    Assert::AreEqual<std::uint32_t>(0, random.NextBelow(0)); // Degenerate bound must not divide by zero.
  }
};

TEST_CLASS(RingBufferTests)
{
public:
  TEST_METHOD(PopsInPushOrder)
  {
    RingBuffer<int, 8> ring;
    Assert::IsTrue(ring.Empty());

    for (int i = 0; i < 5; ++i)
    {
      Assert::IsTrue(ring.TryPush(i));
    }
    Assert::AreEqual<std::size_t>(5, ring.Count());

    for (int i = 0; i < 5; ++i)
    {
      int value = -1;
      Assert::IsTrue(ring.TryPop(value));
      Assert::AreEqual(i, value);
    }
    int drained = 0;
    Assert::IsFalse(ring.TryPop(drained));
  }

  TEST_METHOD(DropsRatherThanBlockingWhenFull)
  {
    RingBuffer<int, 4> ring; // Capacity is 3: one slot separates full from empty.
    Assert::AreEqual<std::size_t>(3, ring.Capacity());

    Assert::IsTrue(ring.TryPush(1));
    Assert::IsTrue(ring.TryPush(2));
    Assert::IsTrue(ring.TryPush(3));
    Assert::IsFalse(ring.TryPush(4));
    Assert::AreEqual<std::uint64_t>(1, ring.DroppedCount());
  }

  TEST_METHOD(SurvivesAProducerAndAConsumerOnTwoThreads)
  {
    // The case the ring exists for: a foreign thread pushing while the owner drains.
    static RingBuffer<std::uint32_t, 1024> ring;
    constexpr std::uint32_t Count = 200000;

    std::thread producer(
      []()
      {
        for (std::uint32_t i = 0; i < Count; ++i)
        {
          while (!ring.TryPush(i))
          {
            std::this_thread::yield();
          }
        }
      });

    std::uint32_t expected = 0;
    while (expected < Count)
    {
      std::uint32_t value = 0;
      if (ring.TryPop(value))
      {
        Assert::AreEqual(expected, value); // Order must hold, or the queue is not FIFO.
        ++expected;
      }
    }
    producer.join();
    Assert::AreEqual(Count, expected);
  }
};

TEST_CLASS(ArenaTests)
{
public:
  TEST_METHOD(AllocatesAlignedAndTracksUse)
  {
    Arena arena{1024};
    auto* first = arena.AllocateArray<std::uint32_t>(4);
    Assert::IsNotNull(first);
    Assert::AreEqual<std::size_t>(0, static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(first) % alignof(std::uint32_t)));

    auto* second = arena.AllocateArray<double>(2);
    Assert::IsNotNull(second);
    Assert::AreEqual<std::size_t>(0, static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(second) % alignof(double)));
    Assert::IsTrue(arena.UsedBytes() >= 16 + 16);
  }

  TEST_METHOD(FailsInsteadOfGrowing)
  {
    Arena arena{64};
    Assert::IsNull(arena.AllocateArray<std::uint8_t>(65));
    Assert::AreEqual<std::uint32_t>(1, arena.FailedAllocationCount());
  }

  TEST_METHOD(ResetReusesTheMemoryAndKeepsTheHighWaterMark)
  {
    Arena arena{256};
    (void)arena.AllocateArray<std::uint8_t>(200);
    const std::size_t peak = arena.HighWaterMarkBytes();
    arena.Reset();

    Assert::AreEqual<std::size_t>(0, arena.UsedBytes());
    Assert::AreEqual(peak, arena.HighWaterMarkBytes()); // Sizing evidence must survive the reset.
    Assert::IsNotNull(arena.AllocateArray<std::uint8_t>(200));
  }
};

TEST_CLASS(TimeTests)
{
public:
  TEST_METHOD(CounterAdvancesMonotonically)
  {
    Clock::Initialise();
    Assert::IsTrue(Clock::Frequency() > 0);

    const std::int64_t first = Clock::Counter();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const std::int64_t second = Clock::Counter();

    Assert::IsTrue(second > first);
    const double elapsed = Clock::MillisecondsBetween(first, second);
    Assert::IsTrue(elapsed >= 1.0 && elapsed < 500.0);
  }

  TEST_METHOD(WaitableTimerSleepsRoughlyTheRequestedTime)
  {
    Clock::Initialise();
    WaitableTimer timer;

    const std::int64_t start = Clock::Counter();
    const std::int64_t deadline = start + Clock::Frequency() / 50; // 20 ms
    Assert::IsTrue(timer.WaitUntil(deadline));

    const double slept = Clock::MillisecondsBetween(start, Clock::Counter());
    // Generous upper bound: a shared CI runner is not a real-time system.
    Assert::IsTrue(slept >= 15.0, L"woke far too early");
    Assert::IsTrue(slept < 250.0, L"woke far too late");
  }

  TEST_METHOD(APastDeadlineReturnsImmediately)
  {
    Clock::Initialise();
    WaitableTimer timer;
    const std::int64_t start = Clock::Counter();
    Assert::IsTrue(timer.WaitUntil(start - Clock::Frequency()));
    Assert::IsTrue(Clock::MillisecondsBetween(start, Clock::Counter()) < 50.0);
  }
};

} // namespace NeuronCoreTests
