#include "pch.h"
#include "CppUnitTest.h"

#include "ReplicatedView.h"
#include "SchemaHash.h"
#include "ShipClass.h"
#include "Snapshot.h"

#include "EntityRecord.h"
#include "World.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;
using namespace DirectX;

/*
 * Replication (ADR-004 §6) and the client's view of it (ADR-002 §4).
 *
 * The round trip is the load-bearing test: emit, frame, read back, and compare
 * against the quantised source. It is checked in *integers* rather than metres,
 * because integers are what actually crossed the wire -- a metre-space
 * comparison would have to allow for float representation on top of the
 * quantiser, and would then pass on a bug that shifted a ship by a centimetre.
 */

namespace GameLogicTests
{
namespace
{

const HullClass SNAPSHOT_HULLS[] = {HullClass::Interceptor, HullClass::Bomber, HullClass::Corvette, HullClass::Frigate,
                                    HullClass::Hauler,      HullClass::Miner,  HullClass::Carrier,  HullClass::Battleship};

/// A world of `_shipCount` ships, flown for a while so nothing is at rest and
/// every quantised field has a non-trivial value.
void BuildFlyingWorld(World& _world, int _shipCount, std::vector<ShipId>& _outIds, std::uint32_t _ticks = 60)
{
  _world.Reset(99);
  _outIds.clear();
  for (int i = 0; i < _shipCount; ++i)
  {
    ShipSpawn spawn;
    spawn.hullClass = SNAPSHOT_HULLS[i % static_cast<int>(std::size(SNAPSHOT_HULLS))];
    spawn.xMetres = static_cast<float>(i * 97 - 2000);
    spawn.yMetres = static_cast<float>(i * -53 + 1100);
    spawn.headingRadians = static_cast<float>(i) * 0.3f;
    _outIds.push_back(_world.Spawn(spawn));
  }

  OrderSubmit move;
  move.orderSeq = 1;
  for (const ShipId id : _outIds)
  {
    (void)move.AddShip(id);
  }
  move.target.xCm = Neuron::MetresToCentimetres(7000.0f);
  move.target.yCm = Neuron::MetresToCentimetres(-3000.0f);
  move.target.facingTurns16 = Neuron::RadiansToHeading(1.0f);
  (void)_world.SubmitOrder(move);
  for (std::uint32_t tick = 1; tick <= _ticks; ++tick)
  {
    _world.Tick(tick);
  }
}

/// Whether the view's newest order area mentions a client sequence.
[[nodiscard]] bool HasOrder(const ReplicatedView& _view, std::uint32_t _clientOrderSeq)
{
  for (const OrderStateRecord& record : _view.LatestOrders())
  {
    if (record.clientOrderSeq == _clientOrderSeq)
    {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CLASS(SnapshotWireTests)
{
public:
  TEST_METHOD(EmitBytesApplyEqualsTheQuantisedSource)
  {
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 41, ids);

    std::array<std::uint8_t, 2048> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));

    Neuron::ByteReader reader{writer.Written()};
    SnapshotHeader header;
    std::vector<Neuron::EntityRecord> ships;
    Assert::IsTrue(ReadSnapshot(reader, header, ships));

    Assert::AreEqual<std::uint32_t>(world.Tick(), header.tick);
    Assert::AreEqual<std::uint16_t>(41, header.shipCount);
    Assert::AreEqual<std::uint32_t>(0, header.baselineTick, L"full snapshots only until deltas exist");
    Assert::AreEqual<std::size_t>(41, ships.size());
    Assert::IsTrue(reader.FullyConsumed(), L"the payload had bytes nobody read");

    // Integer-exact, field by field. This is the property the wire actually
    // guarantees, and the one a rounding change would break.
    for (std::uint32_t slot = 0; slot < world.ShipCount(); ++slot)
    {
      const Neuron::EntityRecord expected = MakeShipRecord(world, slot);
      const Neuron::EntityRecord& got = ships[slot];
      Assert::AreEqual<std::uint16_t>(expected.id, got.id);
      Assert::AreEqual<std::uint8_t>(expected.typeId, got.typeId);
      Assert::AreEqual<std::int32_t>(expected.posXCm, got.posXCm);
      Assert::AreEqual<std::int32_t>(expected.posYCm, got.posYCm);
      Assert::AreEqual<std::int16_t>(expected.velXCmPerSec, got.velXCmPerSec);
      Assert::AreEqual<std::int16_t>(expected.velYCmPerSec, got.velYCmPerSec);
      Assert::AreEqual<std::uint16_t>(expected.headingTurns16, got.headingTurns16);
      Assert::AreEqual<std::uint8_t>(expected.gaugeA, got.gaugeA);
      Assert::AreEqual<std::uint8_t>(expected.gaugeB, got.gaugeB);
    }
  }

  TEST_METHOD(QuantisationCostsWhatTheWireContractSays)
  {
    // Centimetres and 1/65,536 of a turn (ADR-004 §6). The bound has to allow
    // one float32 ulp on top of the quantiser: a dequantised centimetre near
    // 4 km is not exactly representable, and the ulp there is already 0.49 mm.
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 20, ids);

    for (std::uint32_t slot = 0; slot < world.ShipCount(); ++slot)
    {
      const Neuron::EntityRecord record = MakeShipRecord(world, slot);

      const float sourceX = world.Positions()[slot].x;
      const float ulp = std::nextafterf(std::fabs(sourceX), 1.0e30f) - std::fabs(sourceX);
      Assert::IsTrue(std::fabs(sourceX - Neuron::CentimetresToMetres(record.posXCm)) <= 0.005f + ulp,
                     L"position lost more than half a centimetre");

      float headingError = world.Headings()[slot] - Neuron::HeadingToRadians(record.headingTurns16);
      headingError = XMScalarModAngle(headingError);
      Assert::IsTrue(std::fabs(headingError) <= XM_2PI / 65536.0f, L"heading lost more than one wire step");
    }
  }

  TEST_METHOD(TheMvpFleetFitsOneDatagram)
  {
    // ADR-004 §6's budget, measured rather than believed. The static asserts in
    // Snapshot.h cover the arithmetic; this covers the bytes actually written.
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 41, ids, 10);

    std::array<std::uint8_t, 2048> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));

    // One order record too, since S9: `BuildFlyingWorld` gives the fleet
    // somewhere to go, and the group that carries it rides along.
    Assert::AreEqual<std::uint16_t>(1, static_cast<std::uint16_t>(world.Groups().size()));
    Assert::AreEqual<std::size_t>(SnapshotBytes(41, 1), writer.BytesWritten());
    Assert::IsTrue(writer.BytesWritten() <= SNAPSHOT_BUDGET_BYTES, L"41 ships must fit one datagram");
    Assert::IsTrue(writer.BytesWritten() < 1000, L"and with room to spare, per the budget in the ADR");

    // And the worst case -- a full order area on top of that fleet -- is still
    // inside the budget, which is what reserving the area buys.
    Assert::IsTrue(SnapshotBytes(41, MAX_ORDERS_PER_SNAPSHOT) <= SNAPSHOT_BUDGET_BYTES);
  }

  TEST_METHOD(AFleetTooBigForOneDatagramIsRefusedRatherThanTruncated)
  {
    // The growth path is deltas plus interest management, not a bigger
    // datagram (ADR-004 §6). Until then the honest failure is to send nothing:
    // a truncated snapshot would read as a mass despawn followed by a mass
    // respawn on the next tick.
    World world;
    world.Reset(1);
    for (std::uint16_t i = 0; i < MAX_SHIPS_PER_SNAPSHOT + 5; ++i)
    {
      ShipSpawn spawn;
      spawn.hullClass = HullClass::Interceptor;
      (void)world.Spawn(spawn);
    }

    std::array<std::uint8_t, 4096> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsFalse(WriteSnapshot(world, writer));
    Assert::AreEqual<std::size_t>(0, writer.BytesWritten(), L"a refused snapshot must write nothing");
  }

  TEST_METHOD(ATruncatedPayloadIsRejectedRatherThanGuessed)
  {
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 8, ids, 5);

    std::array<std::uint8_t, 512> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));

    const std::span<const std::uint8_t> whole = writer.Written();
    for (const std::size_t length : {std::size_t{0}, std::size_t{4}, std::size_t{15}, whole.size() - 1})
    {
      Neuron::ByteReader reader{whole.subspan(0, length)};
      SnapshotHeader header;
      std::vector<Neuron::EntityRecord> ships;
      Assert::IsFalse(ReadSnapshot(reader, header, ships), L"a truncated payload must not read as a snapshot");
    }
  }

  TEST_METHOD(TheSchemaHashCoversTheQuantisationConstants)
  {
    // Two builds that agreed on every field but disagreed about centimetres
    // versus millimetres would pass a layout check and then place ships ten
    // metres apart. The constants are in the string, so they are in the hash.
    Assert::IsTrue(GameSchemaHash() != 0);
    Assert::AreEqual(GameSchemaHash(), GameSchemaHash(), L"same build, same answer");
    Assert::IsTrue(GAME_SCHEMA_TEXT.find("quantisation{position=cm") != std::string_view::npos);
    Assert::IsTrue(GAME_SCHEMA_TEXT.find("heading=turns/65536") != std::string_view::npos);
    Assert::IsTrue(Neuron::HashText(GAME_SCHEMA_TEXT) == GameSchemaHash());
  }
};

TEST_CLASS(ReplicatedViewTests)
{
public:
  TEST_METHOD(MotionIsSmoothBetweenSnapshots)
  {
    /*
     * The point of interpolating at all. Twenty snapshots a second against a
     * 144 Hz display means seven frames in a row carry no new information, and
     * drawing the newest snapshot each time is the difference between motion
     * and a slideshow.
     *
     * The assertion is on the *step size*: no frame may move a ship further
     * than it could travel in one frame at its top speed. A view that snapped
     * to each new snapshot would take one tick-sized step every seventh frame,
     * which is seven times too far.
     */
    World world;
    world.Reset(7);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Corvette;
    const ShipId ship = world.Spawn(spawn);
    const ShipId ships[] = {ship};

    OrderSubmit move;
    move.orderSeq = 1;
    (void)move.AddShip(ships[0]);
    move.target.xCm = Neuron::MetresToCentimetres(6000.0f);
    (void)world.SubmitOrder(move);

    ReplicatedView view;
    std::vector<ReplicatedShip> sampled;
    std::array<std::uint8_t, 512> buffer{};

    constexpr int FRAMES_PER_TICK = 7;
    const float topSpeed = ShipClass(HullClass::Corvette).maxSpeedMetresPerSec;
    const float frameBudget = topSpeed * World::TICK_SECONDS / FRAMES_PER_TICK * 1.5f;

    float previousX = 0.0f;
    bool havePrevious = false;
    float worstStep = 0.0f;
    for (std::uint32_t tick = 1; tick <= 120; ++tick)
    {
      world.Tick(tick);

      Neuron::ByteWriter writer{buffer};
      Assert::IsTrue(WriteSnapshot(world, writer));
      Assert::IsTrue(view.ApplySnapshot(writer.Written()));

      for (int frame = 0; frame < FRAMES_PER_TICK; ++frame)
      {
        // Two ticks behind the newest, which is where the client renders.
        const double renderTick = static_cast<double>(tick) - 2.0 + static_cast<double>(frame) / FRAMES_PER_TICK;
        view.SampleAt(renderTick, sampled);
        if (sampled.empty())
        {
          continue;
        }
        Assert::IsFalse(sampled[0].stale, L"nothing is stale while the stream flows");
        if (havePrevious)
        {
          worstStep = std::max(worstStep, std::fabs(sampled[0].positionMetres.x - previousX));
        }
        previousX = sampled[0].positionMetres.x;
        havePrevious = true;
      }
    }

    Assert::IsTrue(worstStep > 0.0f, L"the ship never moved");
    Assert::IsTrue(worstStep < frameBudget, L"a frame moved further than one frame of travel: this is a slideshow");
  }

  TEST_METHOD(ItExtrapolatesThenFreezesAndFlags)
  {
    // The induced-stall case. Carrying a ship forward on stale velocity looks
    // entirely correct and is entirely wrong -- it ends up somewhere the server
    // never put it -- so the extrapolation is capped and the freeze is visible.
    World world;
    world.Reset(3);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Interceptor;
    const ShipId ship = world.Spawn(spawn);
    const ShipId ships[] = {ship};

    OrderSubmit move;
    move.orderSeq = 1;
    (void)move.AddShip(ships[0]);
    move.target.xCm = Neuron::MetresToCentimetres(12000.0f);
    (void)world.SubmitOrder(move);

    ReplicatedView view;
    std::array<std::uint8_t, 512> buffer{};
    for (std::uint32_t tick = 1; tick <= 80; ++tick)
    {
      world.Tick(tick);
      Neuron::ByteWriter writer{buffer};
      Assert::IsTrue(WriteSnapshot(world, writer));
      Assert::IsTrue(view.ApplySnapshot(writer.Written()));
    }

    const auto lastTick = static_cast<double>(view.LatestTick());
    std::vector<ReplicatedShip> sampled;

    view.SampleAt(lastTick, sampled);
    const float atLastSnapshot = sampled[0].positionMetres.x;

    view.SampleAt(lastTick + ReplicatedView::MAX_EXTRAPOLATION_TICKS, sampled);
    const float atCap = sampled[0].positionMetres.x;
    Assert::IsFalse(sampled[0].stale, L"at the cap it is extrapolated, not yet stale");
    Assert::IsTrue(atCap > atLastSnapshot, L"extrapolation should carry the ship forward");

    for (const double beyond : {1.0, 5.0, 50.0})
    {
      view.SampleAt(lastTick + ReplicatedView::MAX_EXTRAPOLATION_TICKS + beyond, sampled);
      Assert::IsTrue(sampled[0].stale, L"past the cap a ship must be flagged");
      Assert::AreEqual(atCap, sampled[0].positionMetres.x, 1e-3f, L"and frozen where extrapolation stopped");
    }
  }

  TEST_METHOD(AHeadingCrossingPiTurnsTheShortWay)
  {
    // Headings live on a circle. Lerping the raw numbers spins a ship the long
    // way round whenever it crosses pi -- which looks like the ship suddenly
    // whipping through a full turn for no reason a player can see.
    World world;
    world.Reset(1);

    // Two snapshots hand-built either side of pi: +170 and -170 degrees.
    const float before = 170.0f * XM_PI / 180.0f;
    const float after = -170.0f * XM_PI / 180.0f;

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Structure; // Never moves, so only heading changes.
    spawn.headingRadians = before;
    (void)world.Spawn(spawn);

    ReplicatedView view;
    std::array<std::uint8_t, 256> buffer{};

    world.Tick(10);
    Neuron::ByteWriter first{buffer};
    Assert::IsTrue(WriteSnapshot(world, first));
    Assert::IsTrue(view.ApplySnapshot(first.Written()));

    // Rebuild the world with the far-side heading and emit a later tick.
    World turned;
    turned.Reset(1);
    ShipSpawn turnedSpawn = spawn;
    turnedSpawn.headingRadians = after;
    (void)turned.Spawn(turnedSpawn);
    turned.Tick(11);

    std::array<std::uint8_t, 256> secondBuffer{};
    Neuron::ByteWriter second{secondBuffer};
    Assert::IsTrue(WriteSnapshot(turned, second));
    Assert::IsTrue(view.ApplySnapshot(second.Written()));

    std::vector<ReplicatedShip> sampled;
    view.SampleAt(10.5, sampled);
    Assert::AreEqual<std::size_t>(1, sampled.size());

    // Halfway the short way is 180 degrees, not 0.
    const float halfway = std::fabs(sampled[0].headingRadians);
    Assert::IsTrue(halfway > 175.0f * XM_PI / 180.0f, L"the heading took the long way round");
  }

  TEST_METHOD(AStaleSnapshotDoesNotOverwriteANewerOne)
  {
    // The state channel is unordered by design (ADR-003) and full snapshots are
    // idempotent, so a reordered arrival is normal and must simply be dropped.
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 4, ids, 20);

    std::array<std::uint8_t, 512> older{};
    Neuron::ByteWriter olderWriter{older};
    Assert::IsTrue(WriteSnapshot(world, olderWriter));

    for (std::uint32_t tick = 21; tick <= 30; ++tick)
    {
      world.Tick(tick);
    }
    std::array<std::uint8_t, 512> newer{};
    Neuron::ByteWriter newerWriter{newer};
    Assert::IsTrue(WriteSnapshot(world, newerWriter));

    ReplicatedView view;
    Assert::IsTrue(view.ApplySnapshot(newerWriter.Written()));
    Assert::AreEqual<std::uint32_t>(30, view.LatestTick());

    Assert::IsTrue(view.ApplySnapshot(olderWriter.Written()), L"a reordered snapshot is not an error");
    Assert::AreEqual<std::uint32_t>(30, view.LatestTick(), L"but it must not overwrite newer state");
    Assert::AreEqual<std::size_t>(1, view.SnapshotCount(), L"and must not take a history slot");
  }

  TEST_METHOD(ADespawnedShipStopsBeingDrawn)
  {
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 6, ids, 10);

    ReplicatedView view;
    std::array<std::uint8_t, 512> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));
    Assert::IsTrue(view.ApplySnapshot(writer.Written()));

    Assert::IsTrue(world.Despawn(ids[2]));
    world.Tick(11);

    std::array<std::uint8_t, 512> second{};
    Neuron::ByteWriter secondWriter{second};
    Assert::IsTrue(WriteSnapshot(world, secondWriter));
    Assert::IsTrue(view.ApplySnapshot(secondWriter.Written()));

    std::vector<ReplicatedShip> sampled;
    view.SampleAt(10.5, sampled);
    Assert::AreEqual<std::size_t>(5, sampled.size(), L"a ship the server stopped sending must stop being drawn");
    for (const ReplicatedShip& ship : sampled)
    {
      Assert::IsTrue(ship.id != ids[2]);
    }
  }

  TEST_METHOD(TheOrderRecordsComeThroughForTheGhostToRead)
  {
    // The client's ghost is promoted by these, so a view that read the ships
    // and dropped the order area would leave a PENDING ghost on screen with no
    // path to promotion (`puck-and-wheel.png` §4).
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 4, ids, 10);

    ReplicatedView view;
    std::array<std::uint8_t, 512> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));
    Assert::IsTrue(view.ApplySnapshot(writer.Written()));

    Assert::AreEqual<std::size_t>(1, view.LatestOrders().size(), L"the move BuildFlyingWorld gave is still running");
    Assert::AreEqual<std::uint32_t>(1, view.LatestOrders()[0].clientOrderSeq, L"and it is the one that was submitted");
    Assert::AreEqual<std::uint8_t>(4, view.LatestOrders()[0].memberCount, L"with all four ships in it");
    Assert::AreEqual<std::uint32_t>(1, view.LastOrderSeqProcessed(), L"and the high-water mark says so too");
  }

  TEST_METHOD(AStaleSnapshotDoesNotRewindTheOrderState)
  {
    /*
     * The same rule the ships follow, and it matters more here: an order state
     * is not interpolated, it is *read*, so a reordered arrival would show a
     * ghost a leg behind and then jump it forward again. The orders must be
     * replaced only past the staleness check, not beside it.
     */
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 3, ids, 10);

    std::array<std::uint8_t, 512> older{};
    Neuron::ByteWriter olderWriter{older};
    Assert::IsTrue(WriteSnapshot(world, olderWriter));

    OrderSubmit second;
    second.orderSeq = 2;
    (void)second.AddShip(ids[0]);
    second.target.xCm = Neuron::MetresToCentimetres(-1500.0f);
    second.target.yCm = Neuron::MetresToCentimetres(900.0f);
    Assert::IsTrue(world.SubmitOrder(second).accepted);
    for (std::uint32_t tick = 11; tick <= 20; ++tick)
    {
      world.Tick(tick);
    }

    std::array<std::uint8_t, 512> newer{};
    Neuron::ByteWriter newerWriter{newer};
    Assert::IsTrue(WriteSnapshot(world, newerWriter));

    ReplicatedView view;
    Assert::IsTrue(view.ApplySnapshot(newerWriter.Written()));
    Assert::IsTrue(HasOrder(view, 2), L"the newest snapshot is running the second order");
    Assert::AreEqual<std::uint32_t>(2, view.LastOrderSeqProcessed(), L"and has seen both");

    // The assertion has to be about the *records*, not the header: the header
    // is protected by the frame ring, so a version that replaced the order area
    // before the staleness check would pass a high-water-mark test and still
    // show a ghost a leg behind. That mutation was written and did survive an
    // earlier draft of this test.
    Assert::IsTrue(view.ApplySnapshot(olderWriter.Written()), L"a reordered snapshot is not an error");
    Assert::IsTrue(HasOrder(view, 2), L"and must not put the older snapshot's orders back");
    Assert::AreEqual<std::uint32_t>(2, view.LastOrderSeqProcessed(), L"nor rewind the high-water mark");
  }

  TEST_METHOD(AWorldWithNoOrdersReportsNone)
  {
    World world;
    world.Reset(7);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Corvette;
    (void)world.Spawn(spawn);
    world.Tick(1);

    ReplicatedView view;
    std::array<std::uint8_t, 512> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));
    Assert::IsTrue(view.ApplySnapshot(writer.Written()));

    Assert::IsTrue(view.LatestOrders().empty(), L"nothing ordered, nothing reported");
    Assert::AreEqual<std::uint32_t>(0, view.LastOrderSeqProcessed(), L"and no sequence has been processed");

    view.Clear();
    Assert::IsTrue(view.LatestOrders().empty(), L"and clearing leaves nothing behind either");
  }

  TEST_METHOD(AGarbagePayloadIsRejectedAndChangesNothing)
  {
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 4, ids, 10);

    ReplicatedView view;
    std::array<std::uint8_t, 512> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));
    Assert::IsTrue(view.ApplySnapshot(writer.Written()));
    const std::uint32_t goodTick = view.LatestTick();

    const std::array<std::uint8_t, 6> garbage{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    Assert::IsFalse(view.ApplySnapshot(garbage));
    Assert::AreEqual<std::uint32_t>(goodTick, view.LatestTick(), L"a rejected payload must leave the view alone");
    Assert::AreEqual<std::size_t>(1, view.SnapshotCount());
  }
};

} // namespace GameLogicTests
