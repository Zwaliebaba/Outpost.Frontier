#include "pch.h"
#include "CppUnitTest.h"

#include "Formation.h"
#include "Orders.h"
#include "ShipClass.h"
#include "Validate.h"
#include "World.h"
#include "WorldHash.h"

#include "EntityRecord.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;
using namespace DirectX;

/*
 * Orders: validation, the formation solve, and the group table (Build Order S9,
 * ADR-005 §1, §3, §4).
 *
 * The acceptance criterion S9 was given is validation *parity* -- identical
 * verdict and reason on quantised inputs, from either side of the wire. That is
 * bought here by there being one `ValidateOrder` rather than two, so the tests
 * that matter are the ones proving both sides can actually reach it with the
 * same inputs: the client has a list of ids off a snapshot, the server has its
 * own tables, and `ValidationView` is the shape they have in common.
 */

namespace GameLogicTests
{
namespace
{

[[nodiscard]] ShipId SpawnAt(World& _world, HullClass _class, float _x = 0.0f, float _y = 0.0f)
{
  ShipSpawn spawn;
  spawn.hullClass = _class;
  spawn.xMetres = _x;
  spawn.yMetres = _y;
  return _world.Spawn(spawn);
}

[[nodiscard]] OrderSubmit Order(std::initializer_list<ShipId> _ships, float _xMetres, float _yMetres, float _facing = 0.0f)
{
  OrderSubmit order;
  order.orderSeq = 1;
  for (const ShipId id : _ships)
  {
    (void)order.AddShip(id);
  }
  order.target.xCm = Neuron::MetresToCentimetres(_xMetres);
  order.target.yCm = Neuron::MetresToCentimetres(_yMetres);
  order.target.facingTurns16 = Neuron::RadiansToHeading(_facing);
  return order;
}

[[nodiscard]] ValidationView ViewOf(std::span<const ShipId> _ids, std::uint32_t _queuedLegs = 0)
{
  ValidationView view;
  view.shipIds = _ids;
  view.queuedLegs = _queuedLegs;
  return view;
}

/// The class of a ship, for `SolveFormation`, out of a flat table a test owns.
struct ClassLookup
{
  std::vector<ShipId> ids;
  std::vector<HullClass> classes;

  static HullClass Of(ShipId _shipId, void* _context) noexcept
  {
    const ClassLookup& table = *static_cast<const ClassLookup*>(_context);
    for (std::size_t index = 0; index < table.ids.size(); ++index)
    {
      if (table.ids[index] == _shipId)
      {
        return table.classes[index];
      }
    }
    return HullClass::Interceptor;
  }
};

} // namespace

TEST_CLASS(OrderValidationTests)
{
public:
  TEST_METHOD(AWellFormedOrderIsAccepted)
  {
    const ShipId ids[] = {1, 2, 3};
    const OrderVerdict verdict = ValidateOrder(ViewOf(ids), Order({1, 3}, 1000.0f, -500.0f));

    Assert::IsTrue(verdict.accepted);
    Assert::IsTrue(verdict.reason == OrderReason::Accepted);
    Assert::AreEqual<std::uint32_t>(0, verdict.serverOrderId, L"only the authority assigns an id");
  }

  TEST_METHOD(EachRefusalHasItsOwnReason)
  {
    const ShipId ids[] = {1, 2, 3};

    Assert::IsTrue(ValidateOrder(ViewOf(ids), Order({}, 0.0f, 0.0f)).reason == OrderReason::EmptySelection);
    Assert::IsTrue(ValidateOrder(ViewOf(ids), Order({7}, 0.0f, 0.0f)).reason == OrderReason::UnknownShip);

    OrderSubmit farOut = Order({1}, 0.0f, 0.0f);
    farOut.target.xCm = PLAY_AREA_HALF_EXTENT_CM + 1;
    Assert::IsTrue(ValidateOrder(ViewOf(ids), farOut).reason == OrderReason::OutOfBounds);

    OrderSubmit wedge = Order({1}, 0.0f, 0.0f);
    wedge.formation = FormationId::Wedge;
    Assert::IsTrue(ValidateOrder(ViewOf(ids), wedge).reason == OrderReason::InvalidFormation);

    OrderSubmit strange = Order({1}, 0.0f, 0.0f);
    strange.kind = static_cast<OrderKind>(77);
    Assert::IsTrue(ValidateOrder(ViewOf(ids), strange).reason == OrderReason::UnknownKind);

    OrderSubmit append = Order({1}, 0.0f, 0.0f);
    append.queueMode = QueueMode::Append;
    Assert::IsTrue(ValidateOrder(ViewOf(ids, MAX_ORDER_LEGS), append).reason == OrderReason::QueueFull);
    Assert::IsTrue(ValidateOrder(ViewOf(ids, MAX_ORDER_LEGS - 1), append).accepted, L"one slot left is not full");
  }

  TEST_METHOD(TooManyShipsIsRefusedRatherThanTruncated)
  {
    std::vector<ShipId> ids;
    OrderSubmit order;
    order.orderSeq = 1;
    for (std::uint32_t index = 0; index < MAX_SHIPS_PER_ORDER; ++index)
    {
      ids.push_back(static_cast<ShipId>(index));
      Assert::IsTrue(order.AddShip(static_cast<ShipId>(index)));
    }
    Assert::IsTrue(ValidateOrder(ViewOf(ids), order).accepted, L"exactly the cap is allowed");

    // The struct refuses the overflowing id rather than writing past its array,
    // so a caller cannot construct the over-cap case by accident.
    Assert::IsFalse(order.AddShip(9999));
    Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(MAX_SHIPS_PER_ORDER), order.shipCount);

    order.shipCount = static_cast<std::uint16_t>(MAX_SHIPS_PER_ORDER + 1);
    Assert::IsTrue(ValidateOrder(ViewOf(ids), order).reason == OrderReason::TooManyShips);
  }

  TEST_METHOD(TheOrderOfTheChecksIsPartOfTheContract)
  {
    // An order that fails two rules has to fail the same one on both sides, or
    // the player reads a different explanation depending on which machine
    // answered first. Empty-and-out-of-bounds says EmptySelection.
    const ShipId ids[] = {1};
    OrderSubmit both;
    both.orderSeq = 1;
    both.target.xCm = PLAY_AREA_HALF_EXTENT_CM * 2;
    both.formation = FormationId::Claw;

    Assert::IsTrue(ValidateOrder(ViewOf(ids), both).reason == OrderReason::EmptySelection);
  }

  TEST_METHOD(EveryReasonHasText)
  {
    constexpr OrderReason ALL[] = {OrderReason::Accepted,   OrderReason::EmptySelection,   OrderReason::NotOwned,
                                   OrderReason::UnknownShip, OrderReason::QueueFull,       OrderReason::OutOfBounds,
                                   OrderReason::InvalidFormation, OrderReason::TooManyShips, OrderReason::UnknownKind};
    for (const OrderReason reason : ALL)
    {
      const char* text = OrderReasonText(reason);
      Assert::IsTrue(text != nullptr && text[0] != '\0');
    }
  }

  TEST_METHOD(TheClientsViewAndTheServersAgreeAcrossAMatrix)
  {
    /*
     * The parity criterion, as directly as it can be stated here.
     *
     * The server's view comes from its own tables. The client's comes from what
     * a snapshot carried -- the same ids, in whatever order the wire put them.
     * Both go through the one `ValidateOrder`, so what this actually proves is
     * that the *inputs* the two sides can build are equivalent: no float, no
     * position, and no ordering assumption sneaks into the verdict.
     */
    World world;
    world.Reset(7);
    const ShipId a = SpawnAt(world, HullClass::Interceptor, 0.0f, 0.0f);
    const ShipId b = SpawnAt(world, HullClass::Carrier, 900.0f, 0.0f);
    const ShipId c = SpawnAt(world, HullClass::Bomber, -400.0f, 250.0f);

    // Reversed, because a snapshot's order is not the table's.
    const ShipId replicated[] = {c, b, a};

    OrderSubmit cases[6];
    cases[0] = Order({a, b}, 1000.0f, 1000.0f);
    cases[1] = Order({}, 0.0f, 0.0f);
    cases[2] = Order({a, static_cast<ShipId>(4242)}, 0.0f, 0.0f);
    cases[3] = Order({c}, 19999.99f, -19999.99f);
    cases[4] = Order({c}, 20000.01f, 0.0f);
    cases[5] = Order({a, b, c}, -12345.6f, 7890.1f, 2.5f);
    cases[5].formation = FormationId::Claw;

    for (const OrderSubmit& order : cases)
    {
      const OrderVerdict server = ValidateOrder(world.Validation(), order);
      const OrderVerdict client = ValidateOrder(ViewOf(replicated), order);

      Assert::AreEqual(server.accepted, client.accepted);
      Assert::IsTrue(server.reason == client.reason);
    }

    // And the matrix is not trivially all-accepted or all-refused.
    Assert::IsTrue(ValidateOrder(world.Validation(), cases[0]).accepted);
    Assert::IsFalse(ValidateOrder(world.Validation(), cases[4]).accepted);
  }
};

TEST_CLASS(FormationSolveTests)
{
public:
  TEST_METHOD(OneShipLandsExactlyOnThePointThatWasClicked)
  {
    // The reason the line is centred rather than started at the anchor: a
    // single-ship move has to arrive where the player pointed.
    ClassLookup table{{5}, {HullClass::Corvette}};
    const ShipId ids[] = {5};
    FormationStation stations[4] = {};

    const std::uint32_t count =
        SolveFormation(FormationId::Line, ids, &ClassLookup::Of, &table, XMFLOAT2{1234.0f, -567.0f}, 0.0f, stations);

    Assert::AreEqual<std::uint32_t>(1, count);
    Assert::AreEqual(1234.0f, stations[0].positionMetres.x, 1e-3f);
    Assert::AreEqual(-567.0f, stations[0].positionMetres.y, 1e-3f);
  }

  TEST_METHOD(ALineLiesAcrossTheFacingAndIsCentredOnTheAnchor)
  {
    // Facing +x, so the line runs along y: ships stand shoulder to shoulder
    // looking the same way rather than nose to tail.
    ClassLookup table{{1, 2, 3}, {HullClass::Interceptor, HullClass::Interceptor, HullClass::Interceptor}};
    const ShipId ids[] = {1, 2, 3};
    FormationStation stations[4] = {};

    const std::uint32_t count =
        SolveFormation(FormationId::Line, ids, &ClassLookup::Of, &table, XMFLOAT2{0.0f, 0.0f}, 0.0f, stations);
    Assert::AreEqual<std::uint32_t>(3, count);

    const float spacing = ShipClass(HullClass::Interceptor).formationSpacingMetres;
    for (std::uint32_t index = 0; index < count; ++index)
    {
      Assert::AreEqual(0.0f, stations[index].positionMetres.x, 1e-3f, L"a line across +x should not spread along it");
    }
    Assert::AreEqual(spacing, stations[0].positionMetres.y - stations[1].positionMetres.y, 1e-3f);
    Assert::AreEqual(spacing, stations[1].positionMetres.y - stations[2].positionMetres.y, 1e-3f);

    // Centred: the middle ship of three is on the anchor.
    Assert::AreEqual(0.0f, stations[1].positionMetres.y, 1e-3f);
  }

  TEST_METHOD(TheLineTurnsWithTheFacing)
  {
    ClassLookup table{{1, 2}, {HullClass::Interceptor, HullClass::Interceptor}};
    const ShipId ids[] = {1, 2};
    FormationStation stations[2] = {};

    // Facing +y (a quarter turn CCW from +x): the line now runs along x.
    (void)SolveFormation(FormationId::Line, ids, &ClassLookup::Of, &table, XMFLOAT2{0.0f, 0.0f}, XM_PIDIV2, stations);

    const float spacing = ShipClass(HullClass::Interceptor).formationSpacingMetres;
    Assert::AreEqual(0.0f, stations[0].positionMetres.y, 1e-3f);
    Assert::AreEqual(0.0f, stations[1].positionMetres.y, 1e-3f);
    Assert::AreEqual(spacing, std::fabs(stations[0].positionMetres.x - stations[1].positionMetres.x), 1e-3f);
  }

  TEST_METHOD(SpacingIsTheLargestMembersAndNotTheAverage)
  {
    // A Line with a Battleship in it is a Battleship's Line: the spacing that
    // keeps the big hull clear is the one that keeps everything clear.
    ClassLookup table{{1, 2}, {HullClass::Interceptor, HullClass::Battleship}};
    const ShipId ids[] = {1, 2};
    FormationStation stations[2] = {};

    (void)SolveFormation(FormationId::Line, ids, &ClassLookup::Of, &table, XMFLOAT2{0.0f, 0.0f}, 0.0f, stations);

    const float expected = ShipClass(HullClass::Battleship).formationSpacingMetres;
    const float actual = std::fabs(stations[0].positionMetres.y - stations[1].positionMetres.y);
    Assert::AreEqual(expected, actual, 1e-3f);
    Assert::IsTrue(expected > ShipClass(HullClass::Interceptor).formationSpacingMetres);
  }

  TEST_METHOD(AssignmentIsByAscendingIdWhateverOrderTheCallerUsed)
  {
    /*
     * The client's selection and the server's table hold the same ships in
     * different orders, and both call this. A solve that followed array order
     * would put the preview one station out from where the fleet actually goes.
     */
    ClassLookup table{{4, 9, 11}, {HullClass::Corvette, HullClass::Corvette, HullClass::Corvette}};

    const ShipId ascending[] = {4, 9, 11};
    const ShipId scrambled[] = {11, 4, 9};
    FormationStation first[3] = {};
    FormationStation second[3] = {};

    (void)SolveFormation(FormationId::Line, ascending, &ClassLookup::Of, &table, XMFLOAT2{500.0f, 500.0f}, 1.1f, first);
    (void)SolveFormation(FormationId::Line, scrambled, &ClassLookup::Of, &table, XMFLOAT2{500.0f, 500.0f}, 1.1f, second);

    for (std::uint32_t index = 0; index < 3; ++index)
    {
      Assert::AreEqual<ShipId>(first[index].shipId, second[index].shipId);
      Assert::AreEqual(first[index].positionMetres.x, second[index].positionMetres.x, 1e-4f);
      Assert::AreEqual(first[index].positionMetres.y, second[index].positionMetres.y, 1e-4f);
    }
    Assert::AreEqual<ShipId>(4, first[0].shipId);
    Assert::AreEqual<ShipId>(11, first[2].shipId);
  }

  TEST_METHOD(TheExtentIsTheDistanceToTheFurthestStation)
  {
    ClassLookup table{{1, 2, 3}, {HullClass::Interceptor, HullClass::Interceptor, HullClass::Interceptor}};
    const ShipId ids[] = {1, 2, 3};
    FormationStation stations[3] = {};

    const std::uint32_t count =
        SolveFormation(FormationId::Line, ids, &ClassLookup::Of, &table, XMFLOAT2{0.0f, 0.0f}, 0.0f, stations);

    const float spacing = ShipClass(HullClass::Interceptor).formationSpacingMetres;
    Assert::AreEqual(spacing, FormationExtentMetres(std::span<const FormationStation>{stations, count}, XMFLOAT2{0, 0}),
                     1e-3f);
    Assert::AreEqual(0.0f, FormationExtentMetres({}, XMFLOAT2{0, 0}), 1e-6f);
  }

  TEST_METHOD(AFormationThisBuildCannotSolveSolvesNothing)
  {
    // Refused by ValidateOrder first, so this is the second line of defence:
    // returning zero rather than a Line nobody asked for.
    ClassLookup table{{1}, {HullClass::Interceptor}};
    const ShipId ids[] = {1};
    FormationStation stations[1] = {};

    Assert::AreEqual<std::uint32_t>(
        0, SolveFormation(FormationId::Wedge, ids, &ClassLookup::Of, &table, XMFLOAT2{0, 0}, 0.0f, stations));
    Assert::AreEqual<std::uint32_t>(
        0, SolveFormation(FormationId::Line, {}, &ClassLookup::Of, &table, XMFLOAT2{0, 0}, 0.0f, stations));
  }

  TEST_METHOD(MoreShipsThanStationsFillsWhatFits)
  {
    // The caller sized the buffer; a solve that grew it would allocate on the
    // tick thread.
    ClassLookup table{{1, 2, 3}, {HullClass::Interceptor, HullClass::Interceptor, HullClass::Interceptor}};
    const ShipId ids[] = {1, 2, 3};
    FormationStation stations[2] = {};

    Assert::AreEqual<std::uint32_t>(
        2, SolveFormation(FormationId::Line, ids, &ClassLookup::Of, &table, XMFLOAT2{0, 0}, 0.0f, stations));
  }
};

TEST_CLASS(OrderGroupTests)
{
public:
  TEST_METHOD(AnAcceptedOrderBecomesAGroupWithAnId)
  {
    World world;
    world.Reset(3);
    const ShipId a = SpawnAt(world, HullClass::Corvette, 0.0f, 0.0f);
    const ShipId b = SpawnAt(world, HullClass::Corvette, 100.0f, 0.0f);

    const OrderVerdict verdict = world.SubmitOrder(Order({a, b}, 4000.0f, 0.0f));
    Assert::IsTrue(verdict.accepted);
    Assert::IsTrue(verdict.serverOrderId != 0, L"an accepted order is given an id to be promoted against");

    // Queued, not applied: the state change belongs to a tick.
    Assert::AreEqual<std::uint32_t>(1, world.PendingOrderCount());
    Assert::AreEqual<std::size_t>(0, world.Groups().size());

    world.Tick(1);
    Assert::AreEqual<std::uint32_t>(0, world.PendingOrderCount());
    Assert::AreEqual<std::size_t>(1, world.Groups().size());
    Assert::AreEqual<std::uint32_t>(verdict.serverOrderId, world.Groups()[0].serverOrderId);
    Assert::AreEqual<std::uint16_t>(2, world.Groups()[0].memberCount);
  }

  TEST_METHOD(OrderIdsAreMonotonicAndNeverZero)
  {
    World world;
    world.Reset(3);
    const ShipId ship = SpawnAt(world, HullClass::Corvette);

    std::uint32_t previous = 0;
    for (std::uint32_t index = 0; index < 5; ++index)
    {
      const OrderVerdict verdict = world.SubmitOrder(Order({ship}, 1000.0f + static_cast<float>(index), 0.0f));
      Assert::IsTrue(verdict.accepted);
      Assert::IsTrue(verdict.serverOrderId > previous);
      previous = verdict.serverOrderId;
      world.Tick(index + 1);
    }

    // A refused order consumes no id: the counter is the authority's record of
    // what it accepted, not of what it was asked.
    Assert::IsFalse(world.SubmitOrder(Order({}, 0.0f, 0.0f)).accepted);
    Assert::AreEqual<std::uint32_t>(previous + 1, world.SubmitOrder(Order({ship}, 500.0f, 0.0f)).serverOrderId);
  }

  TEST_METHOD(TheGroupWritesEveryMembersGuidance)
  {
    World world;
    world.Reset(3);
    const ShipId a = SpawnAt(world, HullClass::Interceptor, 0.0f, 0.0f);
    const ShipId b = SpawnAt(world, HullClass::Interceptor, 50.0f, 0.0f);
    const ShipId c = SpawnAt(world, HullClass::Interceptor, 100.0f, 0.0f);

    (void)world.SubmitOrder(Order({a, b, c}, 3000.0f, 0.0f));
    world.Tick(1);

    const float spacing = ShipClass(HullClass::Interceptor).formationSpacingMetres;
    for (std::uint32_t slot = 0; slot < world.ShipCount(); ++slot)
    {
      const Guidance& guidance = world.Guidances()[slot];
      Assert::IsTrue(guidance.mode == GuidanceMode::Seek);
      Assert::AreEqual(3000.0f, guidance.targetXMetres, 1e-2f, L"the line lies across the facing, not along it");
      Assert::IsTrue(std::fabs(guidance.targetYMetres) <= spacing + 1e-3f);
    }

    // Three distinct stations, not three ships sent to one point.
    Assert::AreNotEqual(world.Guidances()[0].targetYMetres, world.Guidances()[1].targetYMetres);
    Assert::AreNotEqual(world.Guidances()[1].targetYMetres, world.Guidances()[2].targetYMetres);
  }

  TEST_METHOD(AGroupIsDoneWhenTheFleetHasArrived)
  {
    World world;
    world.Reset(3);
    const ShipId a = SpawnAt(world, HullClass::Interceptor, 0.0f, 0.0f);
    const ShipId b = SpawnAt(world, HullClass::Interceptor, 0.0f, 100.0f);

    (void)world.SubmitOrder(Order({a, b}, 2000.0f, 0.0f));

    bool done = false;
    for (std::uint32_t tick = 1; tick <= 600 && !done; ++tick)
    {
      world.Tick(tick);
      done = !world.Groups().empty() && world.Groups()[0].state == OrderState::Done;
    }

    Assert::IsTrue(done, L"a fleet that arrived should have finished its order");
    for (std::uint32_t slot = 0; slot < world.ShipCount(); ++slot)
    {
      Assert::AreEqual(2000.0f, world.Positions()[slot].x, World::ARRIVAL_TOLERANCE_METRES * 2.0f);
    }
  }

  TEST_METHOD(AppendAddsALegAndKeepsTheGroup)
  {
    World world;
    world.Reset(3);
    const ShipId ship = SpawnAt(world, HullClass::Interceptor);

    const OrderVerdict first = world.SubmitOrder(Order({ship}, 2000.0f, 0.0f));
    world.Tick(1);
    Assert::AreEqual<std::size_t>(1, world.Groups().size());
    Assert::AreEqual<std::uint8_t>(1, world.Groups()[0].legCount);

    OrderSubmit second = Order({ship}, 2000.0f, 2000.0f);
    second.orderSeq = 2;
    second.queueMode = QueueMode::Append;
    Assert::IsTrue(world.SubmitOrder(second).accepted);
    world.Tick(2);

    Assert::AreEqual<std::size_t>(1, world.Groups().size(), L"appending must not start a second group");
    Assert::AreEqual<std::uint8_t>(2, world.Groups()[0].legCount);
    Assert::AreEqual<std::uint32_t>(first.serverOrderId, world.Groups()[0].serverOrderId,
                                    L"the ghost is identified by this id and the plan only grew");
  }

  TEST_METHOD(AppendingPastTheLegCapIsRefused)
  {
    World world;
    world.Reset(3);
    const ShipId ship = SpawnAt(world, HullClass::Interceptor);

    (void)world.SubmitOrder(Order({ship}, 1000.0f, 0.0f));
    world.Tick(1);

    for (std::uint32_t leg = 1; leg < MAX_ORDER_LEGS; ++leg)
    {
      OrderSubmit more = Order({ship}, 1000.0f + 100.0f * static_cast<float>(leg), 0.0f);
      more.queueMode = QueueMode::Append;
      Assert::IsTrue(world.SubmitOrder(more).accepted);
      world.Tick(1 + leg);
    }
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(MAX_ORDER_LEGS), world.Groups()[0].legCount);

    OrderSubmit overflow = Order({ship}, 5000.0f, 0.0f);
    overflow.queueMode = QueueMode::Append;
    const OrderVerdict verdict = world.SubmitOrder(overflow);
    Assert::IsFalse(verdict.accepted);
    Assert::IsTrue(verdict.reason == OrderReason::QueueFull);
  }

  TEST_METHOD(ReplaceDropsTheOldPlanEntirely)
  {
    World world;
    world.Reset(3);
    const ShipId ship = SpawnAt(world, HullClass::Interceptor);

    (void)world.SubmitOrder(Order({ship}, 1000.0f, 0.0f));
    world.Tick(1);
    const std::uint32_t firstId = world.Groups()[0].serverOrderId;

    OrderSubmit replacement = Order({ship}, -1000.0f, 0.0f);
    replacement.orderSeq = 2;
    (void)world.SubmitOrder(replacement);
    world.Tick(2);

    Assert::AreEqual<std::size_t>(1, world.Groups().size(), L"a ship belongs to one group at a time");
    Assert::IsTrue(world.Groups()[0].serverOrderId != firstId);
    Assert::AreEqual(-1000.0f, world.Guidances()[0].targetXMetres, 1e-2f);
  }

  TEST_METHOD(AGroupWhoseShipsAllDiedIsFinished)
  {
    World world;
    world.Reset(3);
    const ShipId ship = SpawnAt(world, HullClass::Interceptor);

    (void)world.SubmitOrder(Order({ship}, 4000.0f, 0.0f));
    world.Tick(1);
    Assert::AreEqual<std::size_t>(1, world.Groups().size());

    Assert::IsTrue(world.Despawn(ship));
    world.Tick(2);
    Assert::AreEqual<std::size_t>(0, world.Groups().size(), L"an order cannot outlive its last member");
  }

  TEST_METHOD(ASurvivorKeepsFlyingWhenAWingmateDies)
  {
    World world;
    world.Reset(3);
    const ShipId a = SpawnAt(world, HullClass::Interceptor, 0.0f, 0.0f);
    const ShipId b = SpawnAt(world, HullClass::Interceptor, 0.0f, 100.0f);

    (void)world.SubmitOrder(Order({a, b}, 3000.0f, 0.0f));
    world.Tick(1);
    Assert::IsTrue(world.Despawn(b));
    world.Tick(2);

    Assert::AreEqual<std::size_t>(1, world.Groups().size());
    Assert::AreEqual<std::uint16_t>(1, world.Groups()[0].memberCount);

    // And the survivor re-solves onto the centre rather than holding the station
    // it had when there were two of them.
    Assert::AreEqual(0.0f, world.Guidances()[0].targetYMetres, 1e-2f);
  }

  TEST_METHOD(ALegTimesOutRatherThanWedgingTheFleet)
  {
    /*
     * A Structure has zero speed (ADR-005 §1), so a group containing one can
     * never all arrive. Without the timeout the group would sit on leg one
     * forever and the ships that *can* move would never see leg two.
     */
    World world;
    world.Reset(3);
    const ShipId mover = SpawnAt(world, HullClass::Interceptor, 0.0f, 0.0f);
    const ShipId stuck = SpawnAt(world, HullClass::Structure, 0.0f, 5000.0f);

    OrderSubmit first = Order({mover, stuck}, 1000.0f, 0.0f);
    (void)world.SubmitOrder(first);
    world.Tick(1);

    OrderSubmit second = Order({mover, stuck}, 2000.0f, 0.0f);
    second.orderSeq = 2;
    second.queueMode = QueueMode::Append;
    (void)world.SubmitOrder(second);
    world.Tick(2);
    Assert::AreEqual<std::uint8_t>(2, world.Groups()[0].legCount);
    Assert::AreEqual<std::uint8_t>(0, world.Groups()[0].legIndex);

    for (std::uint32_t tick = 3; tick <= World::LEG_TIMEOUT_TICKS + 10; ++tick)
    {
      world.Tick(tick);
    }
    Assert::IsTrue(world.Groups().empty() || world.Groups()[0].legIndex > 0,
                   L"one ship that cannot move must not hold the plan forever");
  }

  TEST_METHOD(TheHighestSequenceIngestedIsReported)
  {
    World world;
    world.Reset(3);
    const ShipId ship = SpawnAt(world, HullClass::Interceptor);

    Assert::AreEqual<std::uint32_t>(0, world.LastOrderSeqProcessed());

    OrderSubmit order = Order({ship}, 1000.0f, 0.0f);
    order.orderSeq = 42;
    (void)world.SubmitOrder(order);
    world.Tick(1);
    Assert::AreEqual<std::uint32_t>(42, world.LastOrderSeqProcessed());

    // An older sequence arriving late must not wind it back: it is a high-water
    // mark, and the client uses it to decide what is still outstanding.
    OrderSubmit late = Order({ship}, 1200.0f, 0.0f);
    late.orderSeq = 7;
    (void)world.SubmitOrder(late);
    world.Tick(2);
    Assert::AreEqual<std::uint32_t>(42, world.LastOrderSeqProcessed());
  }

  TEST_METHOD(ThePlanIsPartOfTheStateTheHashCovers)
  {
    // Two worlds with identical ships and different plans have diverged; the
    // next leg is where it would show, and by then it is a hundred ticks from
    // its cause (ADR-005 §5).
    World a;
    World b;
    a.Reset(11);
    b.Reset(11);
    const ShipId shipA = SpawnAt(a, HullClass::Interceptor);
    const ShipId shipB = SpawnAt(b, HullClass::Interceptor);
    Assert::AreEqual<ShipId>(shipA, shipB);

    Assert::AreEqual(ComputeWorldHash(a), ComputeWorldHash(b));

    (void)a.SubmitOrder(Order({shipA}, 1000.0f, 0.0f));
    Assert::AreNotEqual(ComputeWorldHash(a), ComputeWorldHash(b), L"a queued order is state too");

    (void)b.SubmitOrder(Order({shipB}, 1000.0f, 0.0f));
    Assert::AreEqual(ComputeWorldHash(a), ComputeWorldHash(b));

    a.Tick(1);
    b.Tick(1);
    Assert::AreEqual(ComputeWorldHash(a), ComputeWorldHash(b));

    // Same ships, same positions, different destination.
    World c;
    c.Reset(11);
    const ShipId shipC = SpawnAt(c, HullClass::Interceptor);
    (void)c.SubmitOrder(Order({shipC}, -1000.0f, 0.0f));
    c.Tick(1);
    Assert::AreNotEqual(ComputeWorldHash(a), ComputeWorldHash(c));
  }

  TEST_METHOD(TwoWorldsFlyingTheSameLegWithDifferentPlansStillDiffer)
  {
    /*
     * The case the previous test does not actually make.
     *
     * There, the two worlds differ in where the ships are *going*, so the
     * divergence shows in `Guidance` -- which the hash has covered since S6.
     * Deleting the group fold entirely left that test passing, which is a test
     * passing for the wrong reason.
     *
     * Here both worlds are flying the same first leg to the same point, with
     * identical positions, velocities, headings and guidance. One of them has a
     * second leg queued behind it. The only state that differs is the plan, and
     * the same client sequence is used for both submissions so the high-water
     * mark cannot carry the difference either.
     */
    World withOneLeg;
    World withTwoLegs;
    withOneLeg.Reset(21);
    withTwoLegs.Reset(21);
    const ShipId first = SpawnAt(withOneLeg, HullClass::Interceptor);
    const ShipId second = SpawnAt(withTwoLegs, HullClass::Interceptor);

    (void)withOneLeg.SubmitOrder(Order({first}, 3000.0f, 0.0f));
    (void)withTwoLegs.SubmitOrder(Order({second}, 3000.0f, 0.0f));
    withOneLeg.Tick(1);
    withTwoLegs.Tick(1);
    Assert::AreEqual(ComputeWorldHash(withOneLeg), ComputeWorldHash(withTwoLegs), L"identical so far");

    OrderSubmit queued = Order({second}, 3000.0f, 3000.0f);
    queued.orderSeq = 1; // The same sequence, so the high-water mark matches.
    queued.queueMode = QueueMode::Append;
    Assert::IsTrue(withTwoLegs.SubmitOrder(queued).accepted);
    withOneLeg.Tick(2);
    withTwoLegs.Tick(2);

    // Same ship, same place, same destination for this leg.
    Assert::AreEqual(withOneLeg.Positions()[0].x, withTwoLegs.Positions()[0].x, 1e-6f);
    Assert::AreEqual(withOneLeg.Guidances()[0].targetXMetres, withTwoLegs.Guidances()[0].targetXMetres, 1e-6f);
    Assert::AreEqual(withOneLeg.LastOrderSeqProcessed(), withTwoLegs.LastOrderSeqProcessed());
    Assert::AreEqual<std::uint32_t>(0, withOneLeg.PendingOrderCount());
    Assert::AreEqual<std::uint32_t>(0, withTwoLegs.PendingOrderCount());

    Assert::AreNotEqual(ComputeWorldHash(withOneLeg), ComputeWorldHash(withTwoLegs),
                        L"a queued leg is state, and only the group table carries it");
  }
};

} // namespace GameLogicTests
