#include "pch.h"
#include "CppUnitTest.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "DeltaReceiver.h"
#include "EntityRecord.h"
#include "Wire.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The client's half of interest and delta (ADR-022 §2, §3) — U3d-b's reader.
 *
 * Four rules from the ADR land in `DeltaReceiver`, and every one of them is a
 * rule about what happens when something goes *wrong*, which is why they need
 * tests rather than a careful read:
 *
 *   §2b  a part naming a baseline this client does not hold is **refused**,
 *        never applied to whatever baseline it does have;
 *   §2d  every part is applied on arrival, and the tick is acked only when all
 *        of them have landed;
 *   §3   a keyframe **replaces** and does not merge;
 *   §5e  an id in `leftInterest` is retired, so no ghost hull is left frozen.
 *
 * The frames here are hand-built rather than produced by a server, because what
 * is under test is the reader's behaviour against payloads a real server would
 * not send: a delta with no baseline, parts out of order, a tick that never
 * completes. A round trip against the sender would only prove the two agree.
 */

namespace NeuronClientTests
{
namespace
{

constexpr std::uint16_t GRID = 42;

[[nodiscard]] EntityRecord Record(EntityId _id, std::int32_t _x) noexcept
{
  EntityRecord record;
  record.id = _id;
  record.typeId = 3;
  record.posXCm = _x;
  return record;
}

/// A keyframe payload: header, records, and the game's opaque tail.
[[nodiscard]] std::vector<std::uint8_t> Keyframe(std::uint32_t _tick, std::span<const EntityRecord> _records,
                                                 std::uint16_t _culled = 0, std::span<const std::uint8_t> _tail = {})
{
  std::vector<std::uint8_t> bytes(KEYFRAME_HEADER_BYTES + _records.size() * ENTITY_RECORD_BYTES + _tail.size() + 4);
  ByteWriter writer{bytes};
  Write(writer, KeyframeHeader{_tick, GRID, _culled, static_cast<std::uint32_t>(_records.size())});
  for (const EntityRecord& record : _records)
  {
    WriteEntityRecord(writer, record);
  }
  writer.WriteUInt16(static_cast<std::uint16_t>(_tail.size()));
  for (const std::uint8_t byte : _tail)
  {
    writer.WriteUInt8(byte);
  }
  Assert::IsTrue(writer.Ok(), L"the fixture could not build a keyframe");
  return std::vector<std::uint8_t>{writer.Written().begin(), writer.Written().end()};
}

/// One part of a delta. Only part zero carries the trailers, exactly as the
/// sender writes them.
[[nodiscard]] std::vector<std::uint8_t> DeltaPart(std::uint32_t _tick, std::uint32_t _baselineTick, std::uint8_t _partIndex,
                                                  std::uint8_t _partCount, std::span<const EntityRecord> _records,
                                                  std::span<const EntityId> _left = {}, std::span<const std::uint8_t> _tail = {},
                                                  std::uint16_t _culled = 0)
{
  std::vector<std::uint8_t> bytes(DELTA_HEADER_BYTES + _records.size() * ENTITY_RECORD_BYTES + _left.size() * 4 + _tail.size() + 8);
  ByteWriter writer{bytes};
  DeltaHeader header;
  header.tick = _tick;
  header.baselineTick = _baselineTick;
  header.gridId = GRID;
  header.culledCount = _culled;
  header.partIndex = _partIndex;
  header.partCount = _partCount;
  header.recordCount = static_cast<std::uint16_t>(_records.size());
  Write(writer, header);
  for (const EntityRecord& record : _records)
  {
    WriteEntityRecord(writer, record);
  }
  if (_partIndex == 0)
  {
    writer.WriteUInt16(static_cast<std::uint16_t>(_left.size()));
    for (const EntityId id : _left)
    {
      writer.WriteUInt32(id);
    }
    writer.WriteUInt16(static_cast<std::uint16_t>(_tail.size()));
    for (const std::uint8_t byte : _tail)
    {
      writer.WriteUInt8(byte);
    }
  }
  Assert::IsTrue(writer.Ok(), L"the fixture could not build a delta part");
  return std::vector<std::uint8_t>{writer.Written().begin(), writer.Written().end()};
}

/// The ids in a frame, sorted, so a comparison is about membership rather than
/// about the order the receiver happened to build its picture in.
[[nodiscard]] std::vector<EntityId> SortedIds(const ReplicatedFrame& _frame)
{
  std::vector<EntityId> ids;
  for (const EntityRecord& record : _frame.entities)
  {
    ids.push_back(record.id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

} // namespace

TEST_CLASS(DeltaReceiverTests)
{
public:
  TEST_METHOD(AKeyframeIsTheFirstThingThatCanBeApplied)
  {
    /*
     * A delta before any keyframe names a baseline nobody holds, and there is
     * no honest way to apply it. Refused, and **not acked** -- which is exactly
     * how the client asks for the keyframe that fixes it (§2c). A receiver that
     * guessed here would build a picture out of changes to a state that never
     * existed on this machine.
     */
    DeltaReceiver receiver;
    ReplicatedFrame frame;

    const EntityRecord one[] = {Record(1, 100)};
    Assert::IsFalse(receiver.OnDelta(DeltaPart(5, 4, 0, 1, one), frame), L"a delta with no baseline was applied");
    Assert::AreEqual<std::uint32_t>(1, receiver.OrphanedPartCount());

    SnapshotAck ack;
    Assert::IsFalse(receiver.TakeAck(ack), L"an orphaned part must not produce an ack");

    Assert::IsTrue(receiver.OnKeyframe(Keyframe(5, one), frame), L"a keyframe is applicable from nothing");
    Assert::AreEqual<std::size_t>(1, frame.entities.size());
    Assert::AreEqual<std::uint32_t>(5, frame.tick);

    // Whole by construction -- it came off a reliable stream -- so it is
    // ackable the moment it is read.
    Assert::IsTrue(receiver.TakeAck(ack));
    Assert::AreEqual<std::uint32_t>(5, ack.tick);
    Assert::AreEqual<std::uint16_t>(GRID, ack.gridId);
    Assert::IsFalse(receiver.TakeAck(ack), L"an ack is offered once, not until it is asked for again");
  }

  TEST_METHOD(AKeyframeReplacesRatherThanMerges)
  {
    /*
     * It **is** the baseline (§3), not an update to one. A receiver that merged
     * would keep hulls the server has decided this client should not have --
     * and would keep them forever, because nothing later names them.
     */
    DeltaReceiver receiver;
    ReplicatedFrame frame;

    const EntityRecord before[] = {Record(1, 10), Record(2, 20), Record(3, 30)};
    Assert::IsTrue(receiver.OnKeyframe(Keyframe(5, before), frame));
    Assert::AreEqual<std::size_t>(3, frame.entities.size());

    const EntityRecord after[] = {Record(9, 90)};
    Assert::IsTrue(receiver.OnKeyframe(Keyframe(9, after), frame));
    Assert::AreEqual<std::size_t>(1, frame.entities.size(), L"the keyframe merged instead of replacing");
    Assert::AreEqual<std::uint32_t>(9, frame.entities[0].id);
  }

  TEST_METHOD(PartsOutOfOrderLeaveTheSameViewAsInOrder)
  {
    /*
     * The state channel is unordered by design (ADR-003), so a two-part tick
     * arrives either way round. Each part is a set of whole records, so applying
     * them is order-independent -- and this is the test that says so rather than
     * the comment.
     */
    const EntityRecord seed[] = {Record(1, 1), Record(2, 2)};
    const EntityRecord firstHalf[] = {Record(1, 111)};
    const EntityRecord secondHalf[] = {Record(2, 222)};

    std::vector<EntityId> inOrder;
    std::vector<std::int32_t> inOrderX;
    {
      DeltaReceiver receiver;
      ReplicatedFrame frame;
      Assert::IsTrue(receiver.OnKeyframe(Keyframe(4, seed), frame));
      Assert::IsTrue(receiver.OnDelta(DeltaPart(5, 4, 0, 2, firstHalf), frame));
      Assert::IsTrue(receiver.OnDelta(DeltaPart(5, 4, 1, 2, secondHalf), frame));
      inOrder = SortedIds(frame);
      for (const EntityRecord& record : frame.entities)
      {
        inOrderX.push_back(record.posXCm);
      }
      std::sort(inOrderX.begin(), inOrderX.end());
    }

    std::vector<EntityId> reversed;
    std::vector<std::int32_t> reversedX;
    {
      DeltaReceiver receiver;
      ReplicatedFrame frame;
      Assert::IsTrue(receiver.OnKeyframe(Keyframe(4, seed), frame));
      Assert::IsTrue(receiver.OnDelta(DeltaPart(5, 4, 1, 2, secondHalf), frame));
      Assert::IsTrue(receiver.OnDelta(DeltaPart(5, 4, 0, 2, firstHalf), frame));
      reversed = SortedIds(frame);
      for (const EntityRecord& record : frame.entities)
      {
        reversedX.push_back(record.posXCm);
      }
      std::sort(reversedX.begin(), reversedX.end());
    }

    Assert::IsTrue(inOrder == reversed, L"the ids differ depending on which part arrived first");
    Assert::IsTrue(inOrderX == reversedX, L"the field values differ depending on which part arrived first");
  }

  TEST_METHOD(ATickWithAMissingPartIsAppliedButNotAcked)
  {
    /*
     * **Apply for freshness, ack for baseline** (§2d), and the two are separate
     * jobs. The records that did arrive are drawn, because a player watching a
     * fleet is owed whatever is true; the tick is not acked, because a baseline
     * both sides do not agree on is worse than an old one -- the server keeps
     * encoding against the last whole tick and the client stays correct.
     */
    DeltaReceiver receiver;
    ReplicatedFrame frame;

    const EntityRecord seed[] = {Record(1, 1), Record(2, 2)};
    Assert::IsTrue(receiver.OnKeyframe(Keyframe(4, seed), frame));

    SnapshotAck ack;
    Assert::IsTrue(receiver.TakeAck(ack), L"the keyframe is ackable");

    // Part one of two. The record lands.
    const EntityRecord half[] = {Record(1, 999)};
    Assert::IsTrue(receiver.OnDelta(DeltaPart(5, 4, 0, 2, half), frame), L"a part must be applied on arrival");
    bool found = false;
    for (const EntityRecord& record : frame.entities)
    {
      found = found || (record.id == 1 && record.posXCm == 999);
    }
    Assert::IsTrue(found, L"the part that arrived was not applied");

    Assert::IsFalse(receiver.TakeAck(ack), L"half a tick is not a baseline and must not be acked");

    // And the moment the other part lands, the tick is whole and ackable.
    const EntityRecord rest[] = {Record(2, 888)};
    Assert::IsTrue(receiver.OnDelta(DeltaPart(5, 4, 1, 2, rest), frame));
    Assert::IsTrue(receiver.TakeAck(ack), L"a whole tick must be acked");
    Assert::AreEqual<std::uint32_t>(5, ack.tick);
  }

  TEST_METHOD(LeftInterestRetiresAHullRatherThanFreezingIt)
  {
    /*
     * §5e, and the ghost it prevents. A record absent from a delta means
     * *unchanged*, so a ship that has left has to be **named** -- otherwise the
     * client draws a hull that is not there for the rest of the session, at the
     * last position anyone mentioned it.
     */
    DeltaReceiver receiver;
    ReplicatedFrame frame;

    const EntityRecord seed[] = {Record(1, 1), Record(2, 2), Record(3, 3)};
    Assert::IsTrue(receiver.OnKeyframe(Keyframe(4, seed), frame));
    Assert::AreEqual<std::size_t>(3, frame.entities.size());

    // Nothing changed and one ship left. The other two are *absent* from the
    // delta and must survive it, which is the half a naive "replace with what
    // arrived" would get wrong.
    const EntityId left[] = {2};
    Assert::IsTrue(receiver.OnDelta(DeltaPart(5, 4, 0, 1, {}, left), frame));

    const std::vector<EntityId> ids = SortedIds(frame);
    Assert::AreEqual<std::size_t>(2, ids.size(), L"a departure took the wrong number of hulls with it");
    Assert::AreEqual<std::uint32_t>(1, ids[0]);
    Assert::AreEqual<std::uint32_t>(3, ids[1], L"an unchanged ship was dropped along with the one that left");
  }

  TEST_METHOD(TheGamesTailAndTheCulledCountRideThroughUnread)
  {
    /*
     * The tail is opaque (ADR-014 §5) and `culledCount` is §5d's honesty: the
     * player is never told a grid is empty when it is not, they are told how
     * many they are not being shown. Both are carried, neither is interpreted.
     */
    DeltaReceiver receiver;
    ReplicatedFrame frame;

    const EntityRecord seed[] = {Record(1, 1)};
    const std::uint8_t tail[] = {0xde, 0xad, 0xbe, 0xef};
    Assert::IsTrue(receiver.OnKeyframe(Keyframe(4, seed, 17, tail), frame));
    Assert::AreEqual<std::uint16_t>(17, frame.culledCount);
    Assert::AreEqual<std::size_t>(4, frame.tail.size());
    Assert::AreEqual<std::uint8_t>(0xde, frame.tail[0]);
    Assert::AreEqual<std::uint8_t>(0xef, frame.tail[3]);

    const std::uint8_t other[] = {0x01, 0x02};
    Assert::IsTrue(receiver.OnDelta(DeltaPart(5, 4, 0, 1, {}, {}, other, 40), frame));
    Assert::AreEqual<std::uint16_t>(40, frame.culledCount);
    Assert::AreEqual<std::size_t>(2, frame.tail.size());
    Assert::AreEqual<std::uint8_t>(0x01, frame.tail[0]);
  }

  TEST_METHOD(AGridSwitchDropsEveryBaseline)
  {
    /*
     * Two grids share an id space without sharing a single ship (ADR-022 §3a),
     * so a delta from elsewhere cannot be decoded against a picture of here --
     * and must not be, because the result would look plausible. Refused, and
     * the missing ack is what brings the keyframe.
     */
    DeltaReceiver receiver;
    ReplicatedFrame frame;

    const EntityRecord seed[] = {Record(1, 1)};
    Assert::IsTrue(receiver.OnKeyframe(Keyframe(4, seed), frame));

    // Hand-build a part that names a different grid.
    std::array<std::uint8_t, 64> bytes{};
    ByteWriter writer{bytes};
    DeltaHeader header;
    header.tick = 5;
    header.baselineTick = 4;
    header.gridId = GRID + 1;
    header.partCount = 1;
    Write(writer, header);
    writer.WriteUInt16(0); // no departures
    writer.WriteUInt16(0); // no tail
    Assert::IsTrue(writer.Ok());

    Assert::IsFalse(receiver.OnDelta(writer.Written(), frame), L"a delta from another grid was applied here");
    Assert::AreEqual<std::uint16_t>(0, receiver.Grid(), L"and the receiver did not drop what it was holding");

    SnapshotAck ack;
    Assert::IsFalse(receiver.TakeAck(ack), L"nothing about a refused part is ackable");
  }

  TEST_METHOD(AMalformedPartLeavesThePictureAlone)
  {
    DeltaReceiver receiver;
    ReplicatedFrame frame;

    const EntityRecord seed[] = {Record(1, 1), Record(2, 2)};
    Assert::IsTrue(receiver.OnKeyframe(Keyframe(4, seed), frame));

    // A header that claims more records than the payload holds.
    std::array<std::uint8_t, 64> bytes{};
    ByteWriter writer{bytes};
    DeltaHeader header;
    header.tick = 5;
    header.baselineTick = 4;
    header.gridId = GRID;
    header.partCount = 1;
    header.recordCount = 9;
    Write(writer, header);
    Assert::IsTrue(writer.Ok());

    Assert::IsFalse(receiver.OnDelta(writer.Written(), frame), L"a truncated part must not read as a part");

    // The picture is still the keyframe's, which the next good delta proves.
    const EntityRecord change[] = {Record(1, 77)};
    Assert::IsTrue(receiver.OnDelta(DeltaPart(6, 4, 0, 1, change), frame));
    Assert::AreEqual<std::size_t>(2, frame.entities.size(), L"the malformed part damaged the picture");
  }

  TEST_METHOD(ResetForgetsEverythingIncludingTheAck)
  {
    DeltaReceiver receiver;
    ReplicatedFrame frame;

    const EntityRecord seed[] = {Record(1, 1)};
    Assert::IsTrue(receiver.OnKeyframe(Keyframe(4, seed), frame));

    receiver.Reset();

    SnapshotAck ack;
    Assert::IsFalse(receiver.TakeAck(ack), L"a reset must not leave an ack for a baseline that is gone");

    // And the baseline it held is gone too, so a delta against it is refused.
    const EntityRecord change[] = {Record(1, 77)};
    Assert::IsFalse(receiver.OnDelta(DeltaPart(5, 4, 0, 1, change), frame));
  }
};

} // namespace NeuronClientTests
