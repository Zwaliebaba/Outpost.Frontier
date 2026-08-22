#include "pch.h"
#include "CppUnitTest.h"

#include "ReplicatedView.h"
#include "SchemaHash.h"
#include "ShipClass.h"
#include "Snapshot.h"

#include "EntityRecord.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
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

/*
 * The id the world would have minted before ADR-018 D6a moved allocation to the
 * registry: sequential from zero, per world.
 *
 * Sequential rather than "lowest free" on purpose -- the world used to mint
 * monotonically, and reusing a despawned id would quietly change what a test
 * about identity is testing. Per world rather than globally for a sharper
 * reason: the replay suites run a scenario twice and compare hashes, and a
 * counter that kept climbing between runs would make every one of them fail.
 */
[[nodiscard]] ShipId NextShipId(const World& _world) noexcept
{
  return static_cast<ShipId>(_world.ShipCount());
}

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
    _outIds.push_back(_world.Spawn(spawn, NextShipId(_world)));
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

/*
 * What the session host does for a whole grid, compressed into a helper.
 *
 * ADR-022 moved the envelope out of this library: the engine ranks, truncates,
 * delta-encodes and packs, and hands the game a finished picture plus the
 * game's own tail. These suites are about *meaning* -- quantisation,
 * interpolation, staleness, what a wing is -- so they stand in for the engine
 * with the simplest honest policy there is: take every ship on the grid in dense
 * order, cull nothing, and attach the tail.
 *
 * The engine's real path -- budgets, baselines, parts, keyframes -- is tested
 * where it lives, in `NeuronServerTests` and `NeuronClientTests`. A test about
 * whether a heading interpolates the short way round should not have to build a
 * link to ask.
 */
struct WireFrame
{
  std::vector<Neuron::EntityRecord> entities;
  std::vector<std::uint8_t> tail;
};

[[nodiscard]] WireFrame FrameOf(const World& _world, std::uint32_t _lastOrderSeq = 0,
                                Relationship _relationship = Relationship::Neutral)
{
  WireFrame frame;
  frame.entities.reserve(_world.ShipCount());
  for (std::uint32_t slot = 0; slot < _world.ShipCount(); ++slot)
  {
    frame.entities.push_back(MakeShipRecord(_world, slot, _relationship));
  }

  std::array<std::uint8_t, MAX_TICK_TAIL_BYTES> buffer{};
  Neuron::ByteWriter writer{buffer};
  if (WriteTickTail(_world, writer, _lastOrderSeq) && writer.Ok())
  {
    frame.tail.assign(writer.Written().begin(), writer.Written().end());
  }
  return frame;
}

/// A frame kept for later, for the tests that emit one tick, run the world on,
/// and then apply the two out of order.
struct CapturedFrame
{
  std::uint32_t tick = 0;
  AnchorId anchor = INVALID_ID;
  WireFrame wire;
};

[[nodiscard]] CapturedFrame CaptureFrame(const World& _world, std::uint32_t _lastOrderSeq = 0)
{
  return CapturedFrame{_world.Tick(), _world.Anchor(), FrameOf(_world, _lastOrderSeq)};
}

[[nodiscard]] bool Apply(ReplicatedView& _view, const CapturedFrame& _frame, std::uint16_t _culled = 0)
{
  return _view.ApplyFrame(_frame.tick, _frame.anchor, _culled, _frame.wire.entities, _frame.wire.tail);
}

/// The whole grid, into a view, as one tick.
[[nodiscard]] bool ApplyWorld(ReplicatedView& _view, const World& _world, std::uint32_t _lastOrderSeq = 0,
                              std::uint16_t _culled = 0)
{
  return Apply(_view, CaptureFrame(_world, _lastOrderSeq), _culled);
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

    /*
     * The record round trip, which is where the wire contract actually lives.
     *
     * The *payload* is the engine's now (ADR-022 §3b), so what this asserts is
     * the half GameLogic still owns: that a record written by this library and
     * read back by `Neuron::ReadEntityRecord` is integer-identical. The framing
     * around it is tested in `NeuronCoreTests`.
     */
    std::array<std::uint8_t, 4096> buffer{};
    Neuron::ByteWriter writer{buffer};
    const WireFrame frame = FrameOf(world);
    Assert::AreEqual<std::size_t>(41, frame.entities.size());
    for (const Neuron::EntityRecord& record : frame.entities)
    {
      Neuron::WriteEntityRecord(writer, record);
    }
    Assert::IsTrue(writer.Ok());
    Assert::AreEqual<std::size_t>(41 * Neuron::ENTITY_RECORD_BYTES, writer.BytesWritten());

    Neuron::ByteReader reader{writer.Written()};
    std::vector<Neuron::EntityRecord> ships;
    for (int index = 0; index < 41; ++index)
    {
      ships.push_back(Neuron::ReadEntityRecord(reader));
    }
    Assert::IsTrue(reader.FullyConsumed(), L"the payload had bytes nobody read");

    // Integer-exact, field by field. This is the property the wire actually
    // guarantees, and the one a rounding change would break.
    for (std::uint32_t slot = 0; slot < world.ShipCount(); ++slot)
    {
      const Neuron::EntityRecord expected = MakeShipRecord(world, slot);
      const Neuron::EntityRecord& got = ships[slot];
      Assert::AreEqual<std::uint32_t>(expected.id, got.id);
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

  TEST_METHOD(TheTickTailFitsBesideAUsefulNumberOfRecords)
  {
    /*
     * **This was `TheMvpFleetFitsOneDatagram`, and the question it asked is
     * gone** (ADR-022 §5b). The fleet no longer has to fit one datagram: the
     * per-tick budget is a bandwidth figure packed into as many datagrams as it
     * takes, which is exactly what let `EntityRecord::id` widen to u32.
     *
     * What survives is the reservation the *tail* still needs. It rides in part
     * zero, and the engine reserves room for it before it starts packing
     * records -- so a busy tick can never be the reason a ghost loses its ETA.
     * The number this measures is what that reservation costs, and the assert
     * is that it leaves a datagram usefully full of records rather than being
     * the datagram.
     */
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 41, ids, 10);

    // One order record, since S9: `BuildFlyingWorld` gives the fleet somewhere
    // to go, and the group that carries it rides along.
    Assert::AreEqual<std::uint16_t>(1, static_cast<std::uint16_t>(world.Groups().size()));

    std::array<std::uint8_t, MAX_TICK_TAIL_BYTES> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteTickTail(world, writer, 7));
    Assert::AreEqual<std::size_t>(TICK_TAIL_HEADER_BYTES + ORDER_STATE_RECORD_BYTES, writer.BytesWritten());

    // The worst case -- a full order area -- still leaves most of a datagram.
    Assert::IsTrue(MAX_TICK_TAIL_BYTES < Neuron::MAX_DATAGRAM_BYTES / 4,
                   L"the tail must not be most of the part it rides in");
    Assert::AreEqual<std::size_t>(TICK_TAIL_HEADER_BYTES + ORDER_AREA_BYTES, MAX_TICK_TAIL_BYTES);
  }

  TEST_METHOD(AGridPastTheOldCapIsReplicatedRatherThanRefused)
  {
    /*
     * **The two tests this replaces were the refusal**, and ADR-022 §6 is what
     * retired them: `AFleetAtTheCapRoundTripsInsideOneDatagram` and
     * `AFleetTooBigForOneDatagramIsRefusedRatherThanTruncated` between them
     * asserted that 43 ships fit one datagram and 44 produced nothing at all.
     *
     * That behaviour was correct while a full snapshot in one datagram was the
     * only format -- a truncated snapshot reads as a mass despawn followed by a
     * mass respawn, so silence was the honest failure. It is also the
     * session-killing outage R19 was raised for: two commanders meeting at the
     * starter station is 83 records, and the designed response was to send
     * nobody anything.
     *
     * The replacement claim is the one this slice is for: **a grid past the old
     * cap replicates.** Sixty ships -- comfortably past 43 -- produce sixty
     * records, all of them, with nothing refused and nothing dropped. What
     * bounds a *tick* now is a byte budget the engine applies, and the honest
     * `culledCount` that goes with it; both are tested where they live.
     */
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 60, ids, 10);

    const WireFrame frame = FrameOf(world);
    Assert::AreEqual<std::size_t>(60, frame.entities.size(), L"a grid past the old cap lost records");

    for (const ShipId id : ids)
    {
      const auto found = std::find_if(frame.entities.begin(), frame.entities.end(),
                                      [id](const Neuron::EntityRecord& _record) { return _record.id == id; });
      Assert::IsTrue(found != frame.entities.end(), L"a ship past the old cap did not survive");
    }

    // And it reaches a client's view intact, which is the property that
    // actually matters: 60 hulls drawn where 44 used to be nothing at all.
    ReplicatedView view;
    Assert::IsTrue(ApplyWorld(view, world));
    Assert::AreEqual<std::uint16_t>(60, view.LatestShipCount());
  }

  TEST_METHOD(ATruncatedTailIsRejectedRatherThanGuessed)
  {
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 8, ids, 5);

    std::array<std::uint8_t, MAX_TICK_TAIL_BYTES> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteTickTail(world, writer, 5));

    const std::span<const std::uint8_t> whole = writer.Written();
    for (const std::size_t length : {std::size_t{1}, std::size_t{4}, whole.size() - 1})
    {
      Neuron::ByteReader reader{whole.subspan(0, length)};
      std::uint32_t seq = 0;
      std::vector<OrderStateRecord> orders;
      Assert::IsFalse(ReadTickTail(reader, seq, orders), L"a truncated tail must not read as a tail");
    }
  }

  TEST_METHOD(TheRelationshipRidesInTheStatusByteForFree)
  {
    /*
     * ADR-022 §8b: the icon sheet's colour channel, two bits of a byte that was
     * already on the wire. An owner id per record would have cost four bytes on
     * every entity every tick to answer a question a player asks once a session.
     *
     * The other bits must survive it, which is the half a mask gets wrong:
     * undock protection is bit 0 and shares the byte.
     */
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 4, ids, 2);

    for (const Relationship relationship : {Relationship::Own, Relationship::Allied, Relationship::Neutral, Relationship::Hostile})
    {
      const WireFrame frame = FrameOf(world, 0, relationship);
      for (const Neuron::EntityRecord& record : frame.entities)
      {
        Assert::IsTrue(RelationshipFrom(record.statusBits) == relationship, L"the relationship did not survive the byte");
      }
    }

    // And the record is still 23 bytes, which is the whole point of spending
    // bits rather than a field.
    Assert::AreEqual<std::size_t>(23, Neuron::ENTITY_RECORD_BYTES);

    // Packing a relationship must not disturb a bit somebody else owns.
    Assert::AreEqual<std::uint8_t>(SHIP_STATUS_PROTECTED,
                                   static_cast<std::uint8_t>(WithRelationship(SHIP_STATUS_PROTECTED, Relationship::Own)));
    const std::uint8_t hostileAndProtected = WithRelationship(SHIP_STATUS_PROTECTED, Relationship::Hostile);
    Assert::IsTrue((hostileAndProtected & SHIP_STATUS_PROTECTED) != 0, L"the relationship trod on bit 0");
    Assert::IsTrue(RelationshipFrom(hostileAndProtected) == Relationship::Hostile);
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

  TEST_METHOD(TheSchemaHashCoversEveryMessageOnTheWire)
  {
    /*
     * The hole this closes was open from S9 to S11b: `OrderStateRecord` and
     * `OrderSubmit` went on the wire and the schema string still said they
     * "arrive with S9". Two builds that disagreed about either would have
     * passed the handshake and then misparsed every order between them, which
     * is precisely the failure the hash exists to turn into a refusal.
     *
     * Asserted by name rather than by hash value: a literal hash here would be
     * a number to update on every intended change, which trains whoever does
     * it to update the number without reading what changed.
     */
    for (const std::string_view fragment : {std::string_view{"TickTail{"}, std::string_view{"OrderStateRecord{"},
                                            std::string_view{"OrderSubmit{"}, std::string_view{"statusBits.bit1_2="}})
    {
      Assert::IsTrue(GAME_SCHEMA_TEXT.find(fragment) != std::string_view::npos, L"a wire message is not in the schema");
    }

    // The neutral fields' meanings, which is the only place they are written
    // down -- a gauge that became a percentage would draw different bars on
    // two builds that agreed about every byte.
    Assert::IsTrue(GAME_SCHEMA_TEXT.find("groupId=WingId") != std::string_view::npos);
    Assert::IsTrue(GAME_SCHEMA_TEXT.find("gaugeA=hull255") != std::string_view::npos);

    // And the caps, because they bound a length prefix: a build that read 64
    // ids where the sender wrote 32 would run off the end of a valid payload.
    Assert::IsTrue(GAME_SCHEMA_TEXT.find("shipsPerOrder=64") != std::string_view::npos);
    Assert::AreEqual<std::uint32_t>(64, MAX_SHIPS_PER_ORDER, L"the schema string and the cap must not drift apart");
    Assert::AreEqual<std::uint16_t>(16, MAX_ORDERS_PER_SNAPSHOT);

    /*
     * The hull count, which is on the wire as a `typeId` byte and as the icon
     * and palette index the byte selects. U4 appended the twelfth (ADR-016
     * §10), and a build that grew the taxonomy must not match one that did not:
     * the older build would spawn nothing for the value and draw nothing where
     * a gate is.
     */
    Assert::IsTrue(GAME_SCHEMA_TEXT.find("hull{12 classes") != std::string_view::npos);
    Assert::AreEqual<std::uint8_t>(12, HULL_CLASS_COUNT, L"the schema string and the taxonomy must not drift apart");
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
    const ShipId ship = world.Spawn(spawn, NextShipId(world));
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

      Assert::IsTrue(ApplyWorld(view, world));

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
    const ShipId ship = world.Spawn(spawn, NextShipId(world));
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
      Assert::IsTrue(ApplyWorld(view, world));
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
    (void)world.Spawn(spawn, NextShipId(world));

    ReplicatedView view;
    std::array<std::uint8_t, 256> buffer{};

    world.Tick(10);
    Assert::IsTrue(ApplyWorld(view, world));

    // Rebuild the world with the far-side heading and emit a later tick.
    World turned;
    turned.Reset(1);
    ShipSpawn turnedSpawn = spawn;
    turnedSpawn.headingRadians = after;
    (void)turned.Spawn(turnedSpawn, NextShipId(turned));
    turned.Tick(11);

    Assert::IsTrue(ApplyWorld(view, turned));

    std::vector<ReplicatedShip> sampled;
    view.SampleAt(10.5, sampled);
    Assert::AreEqual<std::size_t>(1, sampled.size());

    // Halfway the short way is 180 degrees, not 0.
    const float halfway = std::fabs(sampled[0].headingRadians);
    Assert::IsTrue(halfway > 175.0f * XM_PI / 180.0f, L"the heading took the long way round");
  }

  /*
   * The smear guard (U3b): a snapshot from another grid resets the view.
   *
   * The failure it prevents needs two things to be true at once, and both are
   * ordinary: ship ids are allocated per registry, so two grids can each hold a
   * ship 1; and the view interpolates between the two most recent frames. Put
   * together without a guard, a view switch walks every hull from where it
   * stood on the grid the player left to where a ship of the same id happens to
   * stand on the grid they arrived at. That is not a rendering artefact — the
   * two records describe different ships — which is why the answer is to drop
   * the history rather than to smooth the transition.
   */
  TEST_METHOD(ASnapshotFromAnotherGridResetsTheViewInsteadOfInterpolatingIntoIt)
  {
    World alpha;
    World beta;
    alpha.SetAnchor(11, INVALID_SHIP_ID, {});
    beta.SetAnchor(42, INVALID_SHIP_ID, {});

    ShipSpawn west;
    west.hullClass = HullClass::Frigate;
    west.xMetres = -5000.0f;
    ShipSpawn east;
    east.hullClass = HullClass::Frigate;
    east.xMetres = 5000.0f;

    // The same id on both grids, which is legal and is the point.
    Assert::AreEqual<std::uint32_t>(1, alpha.Spawn(west, 1));
    Assert::AreEqual<std::uint32_t>(1, beta.Spawn(east, 1));

    ReplicatedView view;
    Assert::IsTrue(view.Grid() == INVALID_ID, L"a fresh view is on no grid");

    // Enough frames that the view has a pair to interpolate between.
    Assert::IsTrue(ApplyWorld(view, alpha));
    for (std::uint32_t tick = 1; tick <= 3; ++tick)
    {
      alpha.Tick(tick);
      Assert::IsTrue(ApplyWorld(view, alpha));
    }
    Assert::AreEqual<std::uint32_t>(11, view.Grid());
    Assert::IsTrue(view.SnapshotCount() > 1, L"history to smear through, if it were going to");

    /*
     * Beta's tick is *lower* than alpha's by now, so this also pins the
     * ordering: the grid check has to run before the staleness check, or the
     * switch would be dropped as old news and the player would keep watching
     * the world they left.
     */
    Assert::IsTrue(ApplyWorld(view, beta));
    Assert::AreEqual<std::uint32_t>(42, view.Grid());
    Assert::AreEqual<std::size_t>(1, view.SnapshotCount(), L"the old grid's history was dropped, not extended");

    std::vector<ReplicatedShip> sampled;
    view.SampleAt(static_cast<double>(view.LatestTick()), sampled);
    Assert::IsFalse(sampled.empty());
    for (const ReplicatedShip& ship : sampled)
    {
      Assert::IsTrue(ship.positionMetres.x > 0.0f, L"a hull was sampled part-way between two grids");
    }
  }

  TEST_METHOD(AStaleSnapshotDoesNotOverwriteANewerOne)
  {
    // The state channel is unordered by design (ADR-003) and full snapshots are
    // idempotent, so a reordered arrival is normal and must simply be dropped.
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 4, ids, 20);

    const CapturedFrame older = CaptureFrame(world);

    for (std::uint32_t tick = 21; tick <= 30; ++tick)
    {
      world.Tick(tick);
    }
    const CapturedFrame newer = CaptureFrame(world);

    ReplicatedView view;
    Assert::IsTrue(Apply(view, newer));
    Assert::AreEqual<std::uint32_t>(30, view.LatestTick());

    Assert::IsTrue(Apply(view, older), L"a reordered frame is not an error");
    Assert::AreEqual<std::uint32_t>(30, view.LatestTick(), L"but it must not overwrite newer state");
    Assert::AreEqual<std::size_t>(1, view.SnapshotCount(), L"and must not take a history slot");
  }

  TEST_METHOD(ADespawnedShipStopsBeingDrawn)
  {
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 6, ids, 10);

    ReplicatedView view;
    Assert::IsTrue(ApplyWorld(view, world));

    Assert::IsTrue(world.Despawn(ids[2]));
    world.Tick(11);

    Assert::IsTrue(ApplyWorld(view, world));

    std::vector<ReplicatedShip> sampled;
    view.SampleAt(10.5, sampled);
    Assert::AreEqual<std::size_t>(5, sampled.size(), L"a ship the server stopped sending must stop being drawn");
    for (const ReplicatedShip& ship : sampled)
    {
      Assert::IsTrue(ship.id != ids[2]);
    }
  }

  TEST_METHOD(AShipsWingSurvivesTheWire)
  {
    /*
     * The roster is grouped by this and by nothing else (S11b), so a wing that
     * failed to cross would put the whole fleet in one anonymous pile -- a HUD
     * that looks entirely plausible and is entirely wrong, which is the failure
     * the round trip exists to catch.
     *
     * Asserted after `SampleAt` rather than on the record, because the record
     * is only half the path: the field is `EntityRecord::groupId` on the wire
     * and `ReplicatedShip::wing` after it, and the sampler rebuilds every ship
     * from scratch on each call. A copy that dropped the wing on that second
     * hop would sail through a record-level check.
     */
    World world;
    world.Reset(3);

    // Two ships sharing a wing, one in another, and a station in none. The
    // last is not padding: `INVALID_WING_ID` is what keeps a station out of
    // the roster, and a wire that turned it into wing 0 would invent a row.
    const WingId wings[] = {4, 4, 7, INVALID_WING_ID};
    std::vector<ShipId> ids;
    for (const WingId wing : wings)
    {
      ShipSpawn spawn;
      spawn.hullClass = wing == INVALID_WING_ID ? HullClass::Structure : HullClass::Corvette;
      spawn.wing = wing;
      spawn.xMetres = static_cast<float>(ids.size()) * 120.0f;
      ids.push_back(world.Spawn(spawn, NextShipId(world)));
      Assert::IsTrue(ids.back() != INVALID_SHIP_ID);
    }
    world.Tick(1);

    ReplicatedView view;
    Assert::IsTrue(ApplyWorld(view, world));

    std::vector<ReplicatedShip> sampled;
    view.SampleAt(1.0, sampled);
    Assert::AreEqual<std::size_t>(std::size(wings), sampled.size());

    // Matched by id rather than by position, because the order ships come back
    // in is the snapshot's business and not this test's.
    int matched = 0;
    for (const ReplicatedShip& ship : sampled)
    {
      for (std::size_t i = 0; i < ids.size(); ++i)
      {
        if (ship.id == ids[i])
        {
          Assert::AreEqual<std::uint32_t>(wings[i], ship.wing, L"a ship came back in the wrong wing");
          ++matched;
        }
      }
    }
    Assert::AreEqual(static_cast<int>(ids.size()), matched, L"a spawned ship never came back");
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
    // The high-water mark is the session's as of U3d-a (ADR-022 §7), so a test
    // about what the *client* reads has to supply what a session would.
    Assert::IsTrue(ApplyWorld(view, world, 1));

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

    // What the session had acknowledged when this frame was made: one order.
    const CapturedFrame older = CaptureFrame(world, 1);

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

    // And two by the time this one was.
    const CapturedFrame newer = CaptureFrame(world, 2);

    ReplicatedView view;
    Assert::IsTrue(Apply(view, newer));
    Assert::IsTrue(HasOrder(view, 2), L"the newest frame is running the second order");
    Assert::AreEqual<std::uint32_t>(2, view.LastOrderSeqProcessed(), L"and has seen both");

    // The assertion has to be about the *records*, not the high-water mark: the
    // mark is protected by the frame ring, so a version that replaced the order
    // area before the staleness check would pass a mark test and still show a
    // ghost a leg behind. That mutation was written and did survive an earlier
    // draft of this test.
    Assert::IsTrue(Apply(view, older), L"a reordered frame is not an error");
    Assert::IsTrue(HasOrder(view, 2), L"and must not put the older frame's orders back");
    Assert::AreEqual<std::uint32_t>(2, view.LastOrderSeqProcessed(), L"nor rewind the high-water mark");
  }

  TEST_METHOD(AWorldWithNoOrdersReportsNone)
  {
    World world;
    world.Reset(7);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Corvette;
    (void)world.Spawn(spawn, NextShipId(world));
    world.Tick(1);

    ReplicatedView view;
    Assert::IsTrue(ApplyWorld(view, world));

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
    Assert::IsTrue(ApplyWorld(view, world));
    const std::uint32_t goodTick = view.LatestTick();

    /*
     * A tail that is not a tail: an order count past the cap, which the reader
     * refuses before it allocates. The entities beside it are perfectly good --
     * which is the point, because a frame is rejected as a whole or not at all.
     */
    const std::array<std::uint8_t, 6> garbage{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    Assert::IsFalse(view.ApplyFrame(goodTick + 1, world.Anchor(), 0, {}, garbage));
    Assert::AreEqual<std::uint32_t>(goodTick, view.LatestTick(), L"a rejected frame must leave the view alone");
    Assert::AreEqual<std::size_t>(1, view.SnapshotCount());
  }
};

/*
 * The cosmetic bank's inputs (Build Order S14, ADR-006 §6).
 *
 * The sim's velocity is always along its heading -- the no-strafing rule -- so
 * a slip angle is identically zero and the *observed heading rate* between two
 * snapshots is the only replicated signal a roll can be derived from. These
 * cover both halves: the view measuring the rate, and the pure function
 * turning it into a clamped roll.
 */
TEST_CLASS(CosmeticBankTests)
{
public:
  TEST_METHOD(AnInterpolatedSampleMeasuresTheHeadingRateBetweenItsSnapshots)
  {
    // A ship ordered to a point behind it turns hard, so consecutive snapshots
    // carry different headings and the sampled rate must be their shortest-arc
    // difference over one tick -- not zero, and not a wild number.
    World world;
    world.Reset(3);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Interceptor;
    spawn.headingRadians = 0.0f;
    const ShipId ship = world.Spawn(spawn, NextShipId(world));

    OrderSubmit turn;
    turn.orderSeq = 1;
    (void)turn.AddShip(ship);
    turn.target.xCm = Neuron::MetresToCentimetres(-5000.0f);
    turn.target.yCm = Neuron::MetresToCentimetres(4000.0f);
    (void)world.SubmitOrder(turn);

    for (std::uint32_t tick = 1; tick <= 10; ++tick)
    {
      world.Tick(tick);
    }

    ReplicatedView view;
    Assert::IsTrue(ApplyWorld(view, world));

    // The two quantised headings, straight off the wire: the rate the view
    // reports has to be *their* difference, because those are what it holds.
    const std::vector<Neuron::EntityRecord> firstShips = FrameOf(world).entities;

    world.Tick(11);
    Assert::IsTrue(ApplyWorld(view, world));

    const std::vector<Neuron::EntityRecord> secondShips = FrameOf(world).entities;

    std::vector<ReplicatedShip> sampled;
    view.SampleAt(10.5, sampled);
    Assert::AreEqual<std::size_t>(1, sampled.size());

    const float fromHeading = Neuron::HeadingToRadians(firstShips[0].headingTurns16);
    const float toHeading = Neuron::HeadingToRadians(secondShips[0].headingTurns16);
    const auto expected = static_cast<float>(XMScalarModAngle(toHeading - fromHeading) / World::TICK_SECONDS);

    Assert::IsTrue(std::fabs(expected) > 0.01f, L"the scenario must actually turn, or this test asserts nothing");
    Assert::AreEqual(expected, sampled[0].headingRateRadiansPerSec, 1e-4f);
  }

  TEST_METHOD(AnExtrapolatedSampleReportsNoTurn)
  {
    // Past the newest snapshot the heading is held, and a held heading is not
    // a turn: a frozen ship banking would be the view inventing motion.
    World world;
    std::vector<ShipId> ids;
    BuildFlyingWorld(world, 1, ids, 10);

    ReplicatedView view;
    Assert::IsTrue(ApplyWorld(view, world));

    std::vector<ReplicatedShip> sampled;
    view.SampleAt(static_cast<double>(view.LatestTick()) + 2.0, sampled);
    Assert::AreEqual<std::size_t>(1, sampled.size());
    Assert::AreEqual(0.0f, sampled[0].headingRateRadiansPerSec);
  }

  TEST_METHOD(TheBankIsClampedSignedAndFadesWithSpeed)
  {
    const ShipClassInfo& interceptor = ShipClass(HullClass::Interceptor);

    // Full rate at full speed is the full bank, and a turn to port (CCW,
    // positive rate) banks port-down, which is negative in the record's
    // starboard-down-positive convention.
    const float fullBank =
        CosmeticBankRadians(HullClass::Interceptor, interceptor.turnRateRadiansPerSec, interceptor.maxSpeedMetresPerSec);
    Assert::AreEqual(-MAX_COSMETIC_BANK_RADIANS, fullBank, 1e-6f);

    // The other way rolls the other way.
    const float starboard =
        CosmeticBankRadians(HullClass::Interceptor, -interceptor.turnRateRadiansPerSec, interceptor.maxSpeedMetresPerSec);
    Assert::AreEqual(MAX_COSMETIC_BANK_RADIANS, starboard, 1e-6f);

    // A wild rate off a corrupted sample cannot roll a ship onto its back.
    const float wild = CosmeticBankRadians(HullClass::Interceptor, 1000.0f, interceptor.maxSpeedMetresPerSec);
    Assert::IsTrue(std::fabs(wild) <= MAX_COSMETIC_BANK_RADIANS + 1e-6f);

    // Half speed halves the roll -- banking depicts lateral acceleration, and
    // a ship pivoting on the spot has almost none.
    const float halfSpeed =
        CosmeticBankRadians(HullClass::Interceptor, interceptor.turnRateRadiansPerSec, interceptor.maxSpeedMetresPerSec * 0.5f);
    Assert::AreEqual(-MAX_COSMETIC_BANK_RADIANS * 0.5f, halfSpeed, 1e-6f);

    // At rest there is nothing to bank into.
    Assert::AreEqual(0.0f, CosmeticBankRadians(HullClass::Interceptor, interceptor.turnRateRadiansPerSec, 0.0f));
  }

  TEST_METHOD(AHullThatCannotTurnNeverBanks)
  {
    // The Structure's turn rate is zero, and a division by it would be the
    // quiet way to bank a station onto its side.
    Assert::AreEqual(0.0f, CosmeticBankRadians(HullClass::Structure, 1.0f, 1.0f));
  }
};

} // namespace GameLogicTests
