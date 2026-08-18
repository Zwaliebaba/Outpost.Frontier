#include "pch.h"
#include "CppUnitTest.h"

#include "Eta.h"
#include "Formation.h"
#include "SchemaHash.h"
#include "OrderMessages.h"
#include "Orders.h"
#include "ShipClass.h"
#include "Validate.h"
#include "Snapshot.h"
#include "World.h"
#include "WorldHash.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "EntityRecord.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
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

    // A byte off the wire that is not any formation. All three real ones are
    // solved as of S10, so this check stopped being about unfinished work and
    // is now about `formation` arriving as an untrusted `u8`.
    OrderSubmit nonsense = Order({1}, 0.0f, 0.0f);
    nonsense.formation = static_cast<FormationId>(200);
    Assert::IsTrue(ValidateOrder(ViewOf(ids), nonsense).reason == OrderReason::InvalidFormation);

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

  TEST_METHOD(NoShipsSolveNoStations)
  {
    ClassLookup table{{1}, {HullClass::Interceptor}};
    FormationStation stations[1] = {};

    Assert::AreEqual<std::uint32_t>(
        0, SolveFormation(FormationId::Line, {}, &ClassLookup::Of, &table, XMFLOAT2{0, 0}, 0.0f, stations));
    Assert::AreEqual<std::uint32_t>(0, SolveFormation(FormationId::Line, {}, nullptr, &table, XMFLOAT2{0, 0}, 0.0f, stations),
                                    L"and no lookup solves none either, rather than calling through a null");
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

/*
 * The three shapes (Build Order S10, ADR-005 §3).
 *
 * Geometry is asserted in the anchor's own axes -- facing +x, so "forward" is
 * x and "right" is -y -- because a shape written that way is readable and a
 * shape written in absolute coordinates is a table of numbers nobody can check.
 *
 * The load-bearing one is `StationsNeverOverlapInAnyFormationAtAnyCount`. ADR-005
 * §2 buys the absence of inter-ship avoidance in the MVP with the claim that
 * "stations don't overlap by construction", and that claim had been in the ADR
 * since S6 and asserted nowhere. It is a property of all three formulas at once,
 * so it is checked as one.
 */
TEST_CLASS(FormationShapeTests)
{
public:
  TEST_METHOD(EveryFormationPutsSomethingOnThePointThatWasClicked)
  {
    // The rule that makes the puck honest: the player pointed at a place, and
    // whichever formation is selected, something is there. A Line centres its
    // rank, a Wedge puts its tip, a Claw the middle of its arc.
    ClassLookup table{{5}, {HullClass::Corvette}};
    const ShipId ids[] = {5};

    for (const FormationId formation : FORMATION_IDS)
    {
      FormationStation stations[1] = {};
      Assert::AreEqual<std::uint32_t>(
          1, SolveFormation(formation, ids, &ClassLookup::Of, &table, XMFLOAT2{1234.0f, -567.0f}, 0.9f, stations));
      Assert::AreEqual(1234.0f, stations[0].positionMetres.x, 1e-3f, L"a single ship lands on the anchor");
      Assert::AreEqual(-567.0f, stations[0].positionMetres.y, 1e-3f, L"in every formation");
    }
  }

  TEST_METHOD(AWedgeIsAnArrowheadWithItsTipOnTheAnchor)
  {
    // Facing +x: the tip on the anchor and both arms trailing *behind* it, so
    // the shape points the way the fleet will be looking. An arrowhead whose
    // arms led the tip would be a fleet flying backwards into its own point.
    ClassLookup table{{1, 2, 3, 4, 5}, std::vector<HullClass>(5, HullClass::Corvette)};
    const ShipId ids[] = {1, 2, 3, 4, 5};
    const float spacing = ShipClass(HullClass::Corvette).formationSpacingMetres;

    FormationStation stations[5] = {};
    Assert::AreEqual<std::uint32_t>(
        5, SolveFormation(FormationId::Wedge, ids, &ClassLookup::Of, &table, XMFLOAT2{0.0f, 0.0f}, 0.0f, stations));

    Assert::AreEqual(0.0f, stations[0].positionMetres.x, 1e-3f, L"the tip is the anchor");
    Assert::AreEqual(0.0f, stations[0].positionMetres.y, 1e-3f);

    for (std::uint32_t index = 1; index < 5; ++index)
    {
      Assert::IsTrue(stations[index].positionMetres.x < -1.0f, L"every other ship trails the tip");
    }

    // Alternating arms, one rank at a time: 1 and 2 abreast, then 3 and 4.
    Assert::AreEqual(stations[1].positionMetres.x, stations[2].positionMetres.x, 1e-3f, L"the first pair is abreast");
    Assert::AreEqual(-stations[1].positionMetres.y, stations[2].positionMetres.y, 1e-3f, L"and on opposite sides");
    Assert::IsTrue(stations[3].positionMetres.x < stations[1].positionMetres.x - 1.0f, L"the second pair is a rank further back");

    // A step back and a step out are the same, which is what makes the arms 45
    // degrees and consecutive ships in one arm exactly a spacing apart.
    const float back = -stations[1].positionMetres.x;
    const float out = std::fabs(stations[1].positionMetres.y);
    Assert::AreEqual(back, out, 1e-3f, L"the arms lie at 45 degrees");
    Assert::AreEqual(spacing, std::sqrt(back * back + out * out), 1e-2f, L"and the tip's neighbours are one spacing away");
  }

  TEST_METHOD(AClawCupsTheWayTheFleetWillFace)
  {
    // A crescent with its arms reaching *ahead* of the anchor, which is what
    // makes it a claw rather than a bow. The middle of the arc is on the point.
    ClassLookup table{{1, 2, 3, 4, 5, 6, 7}, std::vector<HullClass>(7, HullClass::Corvette)};
    const ShipId ids[] = {1, 2, 3, 4, 5, 6, 7};

    FormationStation stations[7] = {};
    Assert::AreEqual<std::uint32_t>(
        7, SolveFormation(FormationId::Claw, ids, &ClassLookup::Of, &table, XMFLOAT2{0.0f, 0.0f}, 0.0f, stations));

    // Odd count, so the middle ship is exactly on the anchor.
    Assert::AreEqual(0.0f, stations[3].positionMetres.x, 1e-3f, L"the middle of the arc is the anchor");
    Assert::AreEqual(0.0f, stations[3].positionMetres.y, 1e-3f);

    Assert::IsTrue(stations[0].positionMetres.x > 1.0f, L"one arm reaches forward");
    Assert::IsTrue(stations[6].positionMetres.x > 1.0f, L"and so does the other");
    Assert::AreEqual(stations[0].positionMetres.x, stations[6].positionMetres.x, 1e-3f, L"symmetrically");
    Assert::AreEqual(-stations[0].positionMetres.y, stations[6].positionMetres.y, 1e-3f, L"on opposite sides");

    // Monotonic: every ship from the middle out is further forward than the
    // last, which is what makes it one arc rather than a zigzag.
    for (std::uint32_t index = 3; index + 1 < 7; ++index)
    {
      Assert::IsTrue(stations[index + 1].positionMetres.x > stations[index].positionMetres.x, L"the arc curves one way");
    }
  }

  TEST_METHOD(EveryFormationTurnsWithTheFacing)
  {
    /*
     * The whole shape rotates with the arrival facing -- the degree of freedom
     * the puck's drag chooses. A quarter turn CCW sends what was at +x to +y.
     */
    ClassLookup table{{1, 2, 3, 4}, std::vector<HullClass>(4, HullClass::Corvette)};
    const ShipId ids[] = {1, 2, 3, 4};
    const float quarter = 1.5707963f;

    for (const FormationId formation : FORMATION_IDS)
    {
      FormationStation east[4] = {};
      FormationStation north[4] = {};
      SolveFormation(formation, ids, &ClassLookup::Of, &table, XMFLOAT2{0.0f, 0.0f}, 0.0f, east);
      SolveFormation(formation, ids, &ClassLookup::Of, &table, XMFLOAT2{0.0f, 0.0f}, quarter, north);

      for (std::uint32_t index = 0; index < 4; ++index)
      {
        Assert::AreEqual(-east[index].positionMetres.y, north[index].positionMetres.x, 1e-2f, L"x' = -y");
        Assert::AreEqual(east[index].positionMetres.x, north[index].positionMetres.y, 1e-2f, L"y' = x");
      }
    }
  }

  TEST_METHOD(StationsNeverOverlapInAnyFormationAtAnyCount)
  {
    /*
     * ADR-005 §2's unpaid bill. There is no inter-ship avoidance in the MVP,
     * and the reason given is that stations do not overlap by construction --
     * a claim about three separate formulas, made in prose in S6 and never
     * checked. Every count the wire allows, against the spacing the class table
     * asked for.
     *
     * The Claw is why this is worth running rather than reasoning about: its
     * radius comes from the chord between adjacent ships, and the obvious
     * alternative -- deriving it from the arc length -- puts them a fifth of a
     * spacing too close at low counts while looking equally correct.
     */
    for (const FormationId formation : FORMATION_IDS)
    {
      for (std::uint16_t count = 2; count <= MAX_SHIPS_PER_ORDER; ++count)
      {
        ClassLookup table;
        std::vector<ShipId> ids;
        for (std::uint16_t index = 0; index < count; ++index)
        {
          const ShipId id = static_cast<ShipId>(index + 1);
          ids.push_back(id);
          table.ids.push_back(id);
          // A mixed fleet, so the spacing under test is the largest member's
          // rather than one class's everywhere.
          table.classes.push_back(index % 3 == 0   ? HullClass::Interceptor
                                  : index % 3 == 1 ? HullClass::Carrier
                                                   : HullClass::Bomber);
        }
        const float spacing = ShipClass(HullClass::Carrier).formationSpacingMetres;

        std::vector<FormationStation> stations(count);
        Assert::AreEqual<std::uint32_t>(count, SolveFormation(formation, ids, &ClassLookup::Of, &table,
                                                              XMFLOAT2{0.0f, 0.0f}, 0.37f, stations));

        float closest = 1e30f;
        for (std::uint16_t a = 0; a < count; ++a)
        {
          for (std::uint16_t b = static_cast<std::uint16_t>(a + 1); b < count; ++b)
          {
            const float dx = stations[a].positionMetres.x - stations[b].positionMetres.x;
            const float dy = stations[a].positionMetres.y - stations[b].positionMetres.y;
            closest = std::min(closest, std::sqrt(dx * dx + dy * dy));
          }
        }
        // Equal, not merely greater: all three formulas are built so adjacent
        // stations are exactly one spacing apart, and a formation that opened
        // up to satisfy this would be wasting the plane.
        Assert::AreEqual(spacing, closest, spacing * 0.01f, L"adjacent stations are one spacing apart");
      }
    }
  }

  TEST_METHOD(EveryFormationIsNamedAndAccepted)
  {
    // The two things a command surface needs of a formation: that validation
    // takes it, and that there is a word to put on the control.
    const ShipId ids[] = {1};
    for (const FormationId formation : FORMATION_IDS)
    {
      const char* name = FormationName(formation);
      Assert::IsTrue(name != nullptr && name[0] != '\0', L"a control the player cannot name is not a control");

      OrderSubmit order = Order({1}, 100.0f, 100.0f);
      order.formation = formation;
      Assert::IsTrue(ValidateOrder(ViewOf(ids), order).accepted, L"and validation takes all three");
    }
    Assert::AreEqual<std::size_t>(3, std::size(FORMATION_IDS), L"the MVP set is Line, Wedge and Claw (ADR-005 §3)");
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

    /*
     * Run until it gives up, bounded by the ceiling rather than by a constant
     * duration -- the deadline is the leg's own now (S12), so a test that
     * counted a fixed number of ticks would be asserting the old flat timeout
     * back into existence.
     */
    std::uint32_t tick = 2;
    while (tick < World::LEG_TIMEOUT_MAX_TICKS && !world.Groups().empty() && world.Groups()[0].legIndex == 0)
    {
      ++tick;
      world.Tick(tick);
    }
    Assert::IsTrue(world.Groups().empty() || world.Groups()[0].legIndex > 0,
                   L"one ship that cannot move must not hold the plan forever");
    Assert::IsTrue(tick < World::LEG_TIMEOUT_MAX_TICKS, L"and must not hold it until the ceiling either");
  }

  TEST_METHOD(ALegLongerThanHalfAMinuteIsNotCutShort)
  {
    /*
     * The defect S12 found, pinned so it cannot come back.
     *
     * The timeout was a flat 600 ticks -- thirty seconds -- documented as "long
     * enough that a Battleship crossing the grid is never cut short". A
     * Battleship crossing the grid takes 182 seconds. Every leg longer than
     * half a minute ended by *timing out*, and nothing looked wrong because
     * `Guidance` still held the station and the ships flew on: what ended was
     * the order, so the footprint and the ETA vanished mid-flight. With a queue
     * it skips to the last waypoint.
     */
    World world;
    world.Reset(21);
    const ShipId ship = SpawnAt(world, HullClass::Battleship, 0.0f, 0.0f);

    OrderSubmit far = Order({ship}, 18400.0f, 0.0f);
    Assert::IsTrue(world.SubmitOrder(far).accepted);

    std::uint32_t tick = 0;
    std::uint32_t arrived = 0;
    std::uint32_t finished = 0;
    while (tick < World::LEG_TIMEOUT_MAX_TICKS && (arrived == 0 || finished == 0))
    {
      ++tick;
      world.Tick(tick);
      if (finished == 0 && (world.Groups().empty() || world.Groups()[0].state == OrderState::Done))
      {
        finished = tick;
      }
      if (arrived == 0 && std::fabs(18400.0f - world.Positions()[0].x) <= World::ARRIVAL_TOLERANCE_METRES)
      {
        arrived = tick;
      }
    }

    Assert::IsTrue(arrived > 0, L"the ship never arrived");
    Assert::IsTrue(finished > 0, L"the order never finished");
    Assert::IsTrue(arrived * World::TICK_SECONDS > 60.0f, L"this journey has to be a long one to test anything");
    Assert::IsTrue(finished >= arrived, L"the order must not end before the fleet gets there");
  }

  TEST_METHOD(AnAppendJoinsTheOrderItAppendsToRatherThanInventingOne)
  {
    /*
     * The defect S12 found (S12b).
     *
     * `SubmitOrder` took the next server order id for an appending order, and
     * `IngestOrders` threw it away the moment the append landed on an existing
     * group. So the ack named an order that appeared in no snapshot ever, and
     * the counter skipped a number per queued waypoint.
     *
     * Worse was the sequence: the group kept the *original* order's
     * `clientOrderSeq`, so the appending order's sequence was never reported by
     * anything. The client's high-water mark passed it, `OnFeedback` read that
     * as "decided and no longer running", and the ghost the player had just
     * created retired with no bounce and no promotion -- the silent
     * disappearance `puck-and-wheel.png` §4 forbids.
     */
    World world;
    world.Reset(17);
    const ShipId ship = SpawnAt(world, HullClass::Corvette, 0.0f, 0.0f);

    OrderSubmit first = Order({ship}, 2000.0f, 0.0f);
    first.orderSeq = 100;
    const OrderVerdict firstVerdict = world.SubmitOrder(first);
    Assert::IsTrue(firstVerdict.accepted);
    Assert::AreEqual<std::uint32_t>(1, firstVerdict.serverOrderId, L"the first order gets the first id");
    world.Tick(1);

    OrderSubmit queued = Order({ship}, 4000.0f, 0.0f);
    queued.orderSeq = 101;
    queued.queueMode = QueueMode::Append;
    const OrderVerdict appendVerdict = world.SubmitOrder(queued);
    Assert::IsTrue(appendVerdict.accepted);
    Assert::AreEqual<std::uint32_t>(0, appendVerdict.serverOrderId,
                                    L"an append joins an existing order, and cannot name it until ingest");
    world.Tick(2);

    Assert::AreEqual<std::size_t>(1, world.Groups().size(), L"one group, two legs");
    Assert::AreEqual<std::uint8_t>(2, world.Groups()[0].legCount);
    Assert::AreEqual<std::uint32_t>(1, world.Groups()[0].serverOrderId, L"and it kept the id it was acked under");
    Assert::AreEqual<std::uint32_t>(101, world.Groups()[0].clientOrderSeq,
                                    L"named after the most recent order that shaped it, so that ghost is promoted");

    // A third order still gets id 2: the append burned nothing.
    OrderSubmit third = Order({ship}, 1000.0f, 0.0f);
    third.orderSeq = 102;
    Assert::AreEqual<std::uint32_t>(2, world.SubmitOrder(third).serverOrderId, L"the append must not consume an id");
  }

  TEST_METHOD(AnAppendWithNothingToJoinBecomesAnOrderOfItsOwn)
  {
    /*
     * The fallback, and it needs an id or it is not an order.
     *
     * Reachable the ordinary way: the player holds the queue modifier and drags
     * with nothing previously ordered, or with a selection whose group has
     * already finished. Validation passes -- there is no group, so there is no
     * full queue -- and the append has nowhere to land.
     *
     * Submit deliberately withholds an id from an appending order, so this is
     * the path that has to take one. Zero is the verdict's "no order" sentinel
     * (`World.h`), and a group carrying it would be a live order that every
     * client reads as the absence of one.
     */
    World world;
    world.Reset(29);
    const ShipId ship = SpawnAt(world, HullClass::Corvette, 0.0f, 0.0f);

    OrderSubmit lonely = Order({ship}, 3000.0f, 0.0f);
    lonely.orderSeq = 42;
    lonely.queueMode = QueueMode::Append;
    Assert::IsTrue(world.SubmitOrder(lonely).accepted, L"nothing queued means nothing full");
    world.Tick(1);

    Assert::AreEqual<std::size_t>(1, world.Groups().size());
    Assert::AreEqual<std::uint8_t>(1, world.Groups()[0].legCount, L"its own first leg");
    Assert::IsTrue(world.Groups()[0].serverOrderId != 0, L"a group with the no-order id is unidentifiable");
    Assert::AreEqual<std::uint32_t>(42, world.Groups()[0].clientOrderSeq);

    // And it did not take an id twice: the next real order follows it.
    OrderSubmit next = Order({ship}, 500.0f, 0.0f);
    next.orderSeq = 43;
    const OrderVerdict verdict = world.SubmitOrder(next);
    Assert::IsTrue(verdict.accepted);
    Assert::IsTrue(verdict.serverOrderId > world.Groups()[0].serverOrderId, L"ids move forward, once each");
  }

  TEST_METHOD(TheAppendedOrdersSequenceIsActuallyReported)
  {
    /*
     * The half a client can see, and the half that was broken: the appending
     * order's sequence has to reach the snapshot, or the ghost it belongs to
     * has nothing to be promoted by.
     */
    World world;
    world.Reset(19);
    const ShipId ship = SpawnAt(world, HullClass::Corvette, 0.0f, 0.0f);

    OrderSubmit first = Order({ship}, 2000.0f, 0.0f);
    first.orderSeq = 700;
    Assert::IsTrue(world.SubmitOrder(first).accepted);
    world.Tick(1);

    OrderSubmit queued = Order({ship}, 4000.0f, 0.0f);
    queued.orderSeq = 701;
    queued.queueMode = QueueMode::Append;
    Assert::IsTrue(world.SubmitOrder(queued).accepted);
    world.Tick(2);

    std::array<std::uint8_t, 2048> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));

    Neuron::ByteReader reader{writer.Written()};
    SnapshotHeader header;
    std::vector<Neuron::EntityRecord> ships;
    std::vector<OrderStateRecord> orders;
    Assert::IsTrue(ReadSnapshot(reader, header, ships, orders));

    Assert::AreEqual<std::size_t>(1, orders.size());
    Assert::AreEqual<std::uint32_t>(701, orders[0].clientOrderSeq, L"the ghost the player is watching");
    Assert::AreEqual<std::uint32_t>(1, orders[0].serverOrderId);
    Assert::AreEqual<std::uint8_t>(2, orders[0].legCount);
    Assert::AreEqual<std::uint32_t>(701, header.lastOrderSeqProcessed);
  }

  TEST_METHOD(TheFifthLegBouncesFromTheAuthorityWithQueueFull)
  {
    /*
     * S12's acceptance criterion: a **wire-enforced** four-leg cap, the fifth
     * bouncing. Wire-enforced is the whole phrase -- the client's pre-check
     * reports `queuedLegs = 0` on purpose (see `ReplicatedWorldView::PreCheck`),
     * because a snapshot carries a member count and not the members, so no
     * client can tell which group a selection is in. The refusal is the
     * authority's, and it comes back through the same ack and the same reason
     * string as any other.
     */
    World world;
    world.Reset(23);
    const ShipId ship = SpawnAt(world, HullClass::Corvette, 0.0f, 0.0f);

    OrderSubmit first = Order({ship}, 1000.0f, 0.0f);
    first.orderSeq = 1;
    Assert::IsTrue(world.SubmitOrder(first).accepted);
    world.Tick(1);

    // Fill it to the cap.
    for (std::uint32_t leg = 2; leg <= MAX_ORDER_LEGS; ++leg)
    {
      OrderSubmit more = Order({ship}, 1000.0f + static_cast<float>(leg) * 100.0f, 0.0f);
      more.orderSeq = leg;
      more.queueMode = QueueMode::Append;
      Assert::IsTrue(world.SubmitOrder(more).accepted, L"every leg up to the cap is taken");
      world.Tick(1 + leg);
    }
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(MAX_ORDER_LEGS), world.Groups()[0].legCount);

    // And the one past it bounces.
    OrderSubmit overflow = Order({ship}, 5000.0f, 0.0f);
    overflow.orderSeq = MAX_ORDER_LEGS + 1;
    overflow.queueMode = QueueMode::Append;
    const OrderVerdict refused = world.SubmitOrder(overflow);
    Assert::IsFalse(refused.accepted);
    Assert::IsTrue(refused.reason == OrderReason::QueueFull);
    Assert::IsNotNull(OrderReasonText(refused.reason), L"a refusal the player can be told about");

    // Refused, and therefore not queued: the plan is untouched.
    world.Tick(MAX_ORDER_LEGS + 2);
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(MAX_ORDER_LEGS), world.Groups()[0].legCount,
                                   L"a refused append must not land anyway");
  }

  TEST_METHOD(EachLegOfALongQueueGetsItsOwnDeadline)
  {
    /*
     * The deadline is per *leg*, and this is the test that says so.
     *
     * It is sized in `ApplyLeg`, which runs every tick to re-solve the
     * formation, so it is computed only when unset -- and therefore has to be
     * *un*set when a leg begins. Without that, leg two inherits leg one's
     * deadline, which by then is in the past, and times out on the tick it
     * starts. The fleet would fly the first waypoint and teleport its plan
     * through every one after it.
     *
     * Two legs of eighteen kilometres each: far enough that the old flat
     * thirty-second timeout could not have completed either.
     */
    World world;
    world.Reset(31);
    const ShipId ship = SpawnAt(world, HullClass::Corvette, 0.0f, 0.0f);

    OrderSubmit first = Order({ship}, 12000.0f, 0.0f);
    Assert::IsTrue(world.SubmitOrder(first).accepted);
    world.Tick(1);

    // Inside the 20 km half-extent: a waypoint outside it is refused as
    // `OutOfBounds` before the queue is ever consulted, and a test that hit
    // that would be asserting about the play area instead of the deadline.
    OrderSubmit second = Order({ship}, 18000.0f, 0.0f);
    second.orderSeq = 2;
    second.queueMode = QueueMode::Append;
    Assert::IsTrue(world.SubmitOrder(second).accepted);
    world.Tick(2);
    Assert::AreEqual<std::uint8_t>(2, world.Groups()[0].legCount);

    const std::uint32_t firstDeadline = world.Groups()[0].legDeadlineTick;
    Assert::IsTrue(firstDeadline > 2, L"leg one has to have been given a deadline");

    // Fly leg one and watch the hand-over.
    std::uint32_t tick = 2;
    while (tick < World::LEG_TIMEOUT_MAX_TICKS && !world.Groups().empty() && world.Groups()[0].legIndex == 0)
    {
      ++tick;
      world.Tick(tick);
    }
    Assert::IsFalse(world.Groups().empty(), L"the group must survive its first leg");
    Assert::AreEqual<std::uint8_t>(1, world.Groups()[0].legIndex, L"and move on to the second");

    // The moment of the bug: leg two must get a deadline of its own, ahead of
    // where it starts rather than the one leg one used up.
    const std::uint32_t secondDeadline = world.Groups()[0].legDeadlineTick;
    Assert::IsTrue(secondDeadline > tick, L"leg two inherited a deadline that had already passed");
    Assert::IsTrue(secondDeadline != firstDeadline, L"and it must be its own, not leg one's");

    // And it actually completes it rather than giving up on the spot.
    const float arrival = 18000.0f;
    std::uint32_t arrived = 0;
    while (tick < World::LEG_TIMEOUT_MAX_TICKS && arrived == 0)
    {
      ++tick;
      world.Tick(tick);
      if (std::fabs(arrival - world.Positions()[0].x) <= World::ARRIVAL_TOLERANCE_METRES)
      {
        arrived = tick;
      }
    }
    Assert::IsTrue(arrived > 0, L"the fleet never reached the second waypoint");
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

TEST_CLASS(OrderMessageTests)
{
public:
  TEST_METHOD(AnOrderRoundTripsFieldForField)
  {
    OrderSubmit sent = Order({3, 9, 400}, -12345.67f, 8901.23f, 2.5f);
    sent.orderSeq = 0xdeadbeef;
    sent.formation = FormationId::Wedge;   // Carried even though S9 refuses it:
    sent.queueMode = QueueMode::Append;    // the codec's job is bytes, not policy.

    std::array<std::uint8_t, 256> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteOrderSubmit(sent, writer));
    Assert::AreEqual(OrderSubmitBytes(3), writer.Written().size());

    Neuron::ByteReader reader{writer.Written()};
    OrderSubmit received;
    Assert::IsTrue(ReadOrderSubmit(reader, received));
    Assert::IsTrue(reader.FullyConsumed());

    Assert::AreEqual<std::uint32_t>(sent.orderSeq, received.orderSeq);
    Assert::IsTrue(sent.kind == received.kind);
    Assert::IsTrue(sent.formation == received.formation);
    Assert::IsTrue(sent.queueMode == received.queueMode);
    Assert::AreEqual<std::uint16_t>(3, received.shipCount);
    Assert::AreEqual<ShipId>(3, received.shipIds[0]);
    Assert::AreEqual<ShipId>(9, received.shipIds[1]);
    Assert::AreEqual<ShipId>(400, received.shipIds[2]);

    // The leg crosses as integers, so it survives exactly rather than nearly.
    Assert::AreEqual(sent.target.xCm, received.target.xCm);
    Assert::AreEqual(sent.target.yCm, received.target.yCm);
    Assert::AreEqual<std::uint16_t>(sent.target.facingTurns16, received.target.facingTurns16);
  }

  TEST_METHOD(AnEmptyOrderIsStillAValidPayload)
  {
    // The decoder's job is bytes; `EmptySelection` is validation's answer, and
    // a codec that refused here would return "malformed" where the client is
    // owed a reason it can render.
    const OrderSubmit empty = Order({}, 0.0f, 0.0f);
    std::array<std::uint8_t, 64> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteOrderSubmit(empty, writer));

    Neuron::ByteReader reader{writer.Written()};
    OrderSubmit received;
    Assert::IsTrue(ReadOrderSubmit(reader, received));
    Assert::AreEqual<std::uint16_t>(0, received.shipCount);

    const ShipId none[] = {1};
    Assert::IsTrue(ValidateOrder(ViewOf(none), received).reason == OrderReason::EmptySelection);
  }

  TEST_METHOD(AnUnknownKindDecodesAndIsRefusedByValidationNotByTheCodec)
  {
    OrderSubmit strange = Order({1}, 100.0f, 0.0f);
    strange.kind = static_cast<OrderKind>(200);
    std::array<std::uint8_t, 64> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteOrderSubmit(strange, writer));

    Neuron::ByteReader reader{writer.Written()};
    OrderSubmit received;
    Assert::IsTrue(ReadOrderSubmit(reader, received), L"bytes are bytes; the codec does not judge them");

    const ShipId ids[] = {1};
    Assert::IsTrue(ValidateOrder(ViewOf(ids), received).reason == OrderReason::UnknownKind);
  }

  TEST_METHOD(ATruncatedPayloadIsRefused)
  {
    const OrderSubmit order = Order({1, 2, 3}, 500.0f, 500.0f);
    std::array<std::uint8_t, 128> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteOrderSubmit(order, writer));

    const std::span<const std::uint8_t> whole = writer.Written();
    for (std::size_t length = 0; length < whole.size(); ++length)
    {
      Neuron::ByteReader reader{whole.subspan(0, length)};
      OrderSubmit received;
      Assert::IsFalse(ReadOrderSubmit(reader, received), L"every short prefix must be refused, not guessed");
    }
  }

  TEST_METHOD(AnAbsurdShipCountIsRefusedBeforeAnyIdIsRead)
  {
    /*
     * The boundary where a hostile payload has to stop. A count of 65,535 with
     * no ids behind it would otherwise run the read loop sixty-five thousand
     * times against an underflowing reader before anything noticed -- the
     * result is the same refusal, and the work is not.
     */
    std::array<std::uint8_t, 64> buffer{};
    Neuron::ByteWriter writer{buffer};
    writer.WriteUInt32(1);
    writer.WriteUInt8(0);
    writer.WriteUInt8(0);
    writer.WriteUInt8(0);
    writer.WriteUInt16(0xffff);

    Neuron::ByteReader reader{writer.Written()};
    OrderSubmit received;
    Assert::IsFalse(ReadOrderSubmit(reader, received));

    // One past the cap is refused for the same reason, with the payload intact.
    Neuron::ByteWriter overCap{buffer};
    overCap.WriteUInt32(1);
    overCap.WriteUInt8(0);
    overCap.WriteUInt8(0);
    overCap.WriteUInt8(0);
    overCap.WriteUInt16(static_cast<std::uint16_t>(MAX_SHIPS_PER_ORDER + 1));
    Neuron::ByteReader capReader{overCap.Written()};
    Assert::IsFalse(ReadOrderSubmit(capReader, received));
  }

  TEST_METHOD(TheLargestOrderFitsItsStatedBound)
  {
    OrderSubmit full;
    full.orderSeq = 1;
    for (std::uint32_t index = 0; index < MAX_SHIPS_PER_ORDER; ++index)
    {
      Assert::IsTrue(full.AddShip(static_cast<ShipId>(index)));
    }

    std::vector<std::uint8_t> buffer(MAX_ORDER_SUBMIT_BYTES);
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteOrderSubmit(full, writer));
    Assert::AreEqual(MAX_ORDER_SUBMIT_BYTES, writer.Written().size());

    // And a buffer one byte short refuses rather than writing a short order.
    std::vector<std::uint8_t> tooSmall(MAX_ORDER_SUBMIT_BYTES - 1);
    Neuron::ByteWriter cramped{tooSmall};
    Assert::IsFalse(WriteOrderSubmit(full, cramped));
  }
};

TEST_CLASS(SnapshotOrderStateTests)
{
public:
  TEST_METHOD(TheSnapshotCarriesTheGroupsAndTheHighWaterMark)
  {
    World world;
    world.Reset(5);
    const ShipId a = SpawnAt(world, HullClass::Interceptor, 0.0f, 0.0f);
    const ShipId b = SpawnAt(world, HullClass::Corvette, 300.0f, 0.0f);

    OrderSubmit order = Order({a, b}, 4000.0f, 0.0f);
    order.orderSeq = 99;
    const OrderVerdict verdict = world.SubmitOrder(order);
    world.Tick(1);

    std::array<std::uint8_t, 2048> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));

    Neuron::ByteReader reader{writer.Written()};
    SnapshotHeader header;
    std::vector<Neuron::EntityRecord> ships;
    std::vector<OrderStateRecord> orders;
    Assert::IsTrue(ReadSnapshot(reader, header, ships, orders));

    Assert::AreEqual<std::uint16_t>(1, header.orderCount);
    Assert::AreEqual<std::uint32_t>(99, header.lastOrderSeqProcessed);
    Assert::AreEqual<std::size_t>(1, orders.size());
    Assert::AreEqual<std::uint32_t>(verdict.serverOrderId, orders[0].serverOrderId);
    Assert::AreEqual<std::uint32_t>(99, orders[0].clientOrderSeq);
    Assert::AreEqual<std::uint8_t>(2, orders[0].memberCount);
    Assert::AreEqual<std::uint8_t>(1, orders[0].legCount);
    Assert::AreEqual<std::uint8_t>(0, orders[0].legIndex);
  }

  TEST_METHOD(AWorldWithNoOrdersCarriesNoOrderRecords)
  {
    World world;
    world.Reset(5);
    (void)SpawnAt(world, HullClass::Interceptor);
    world.Tick(1);

    std::array<std::uint8_t, 2048> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));

    Neuron::ByteReader reader{writer.Written()};
    SnapshotHeader header;
    std::vector<Neuron::EntityRecord> ships;
    std::vector<OrderStateRecord> orders;
    Assert::IsTrue(ReadSnapshot(reader, header, ships, orders));

    Assert::AreEqual<std::uint16_t>(0, header.orderCount);
    Assert::IsTrue(orders.empty());
    Assert::AreEqual(SnapshotBytes(1, 0), writer.Written().size(), L"an idle world pays nothing for the order area");
  }

  TEST_METHOD(OrdersAreTruncatedWhereShipsWouldBeRefused)
  {
    /*
     * The asymmetry, asserted. A missing ship record reads as a despawn and is
     * unrecoverable; a missing order record costs one frame of ETA and the
     * header's high-water mark still promotes the ghost. So more groups than
     * fit are truncated, and more ships than fit are refused outright.
     */
    World world;
    world.Reset(5);
    std::vector<ShipId> ships;
    for (std::uint32_t index = 0; index < MAX_ORDERS_PER_SNAPSHOT + 4; ++index)
    {
      ships.push_back(SpawnAt(world, HullClass::Interceptor, static_cast<float>(index) * 400.0f, 0.0f));
    }

    // One group per ship, so the group count passes the cap.
    for (std::uint32_t index = 0; index < ships.size(); ++index)
    {
      OrderSubmit order = Order({ships[index]}, 1000.0f + static_cast<float>(index), 3000.0f);
      order.orderSeq = index + 1;
      Assert::IsTrue(world.SubmitOrder(order).accepted);
      world.Tick(index + 1);
    }
    Assert::IsTrue(world.Groups().size() > MAX_ORDERS_PER_SNAPSHOT);

    std::array<std::uint8_t, 2048> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer), L"a busy world still produces a snapshot");

    Neuron::ByteReader reader{writer.Written()};
    SnapshotHeader header;
    std::vector<Neuron::EntityRecord> read;
    std::vector<OrderStateRecord> orders;
    Assert::IsTrue(ReadSnapshot(reader, header, read, orders));

    Assert::AreEqual<std::uint16_t>(MAX_ORDERS_PER_SNAPSHOT, header.orderCount);
    Assert::AreEqual<std::size_t>(ships.size(), read.size(), L"every ship is present even when orders were cut");
  }

  TEST_METHOD(TheOrderAreaIsReservedSoTheShipCapNeverMoves)
  {
    // A constant the client can rely on. If the ship cap were "whatever is
    // left", it would shrink as the player issued orders -- and the failure
    // would be a fleet that stops replicating when it gets busy.
    Assert::IsTrue(MAX_SHIPS_PER_SNAPSHOT >= 41);
    Assert::IsTrue(SnapshotBytes(MAX_SHIPS_PER_SNAPSHOT, MAX_ORDERS_PER_SNAPSHOT) <= SNAPSHOT_BUDGET_BYTES);
    Assert::IsTrue(SnapshotBytes(MAX_SHIPS_PER_SNAPSHOT + 1, MAX_ORDERS_PER_SNAPSHOT) > SNAPSHOT_BUDGET_BYTES);
  }
};

/*
 * The travel-time model behind the ghost's `ETA 41s` (S11c).
 *
 * Asserted against the *simulation* wherever it can be: the point of the model
 * is that it predicts what `World::Tick` will actually do, and the cheapest way
 * to know is to run the world and count the ticks.
 */
TEST_CLASS(EtaTests)
{
public:
  TEST_METHOD(ThePredictionMatchesTheSimulationItPredicts)
  {
    /*
     * The load-bearing test, and the only one that could catch a model that is
     * self-consistent and wrong. Fly a ship a known distance and compare the
     * ticks it took against what `TravelSeconds` promised.
     *
     * The bound is 3 %, and it is measured rather than guessed: across three
     * hulls and three distances the worst straight-run error is 0.4 %. Three
     * leaves room for float variation and still fails loudly on the mistake
     * this is really guarding -- dropping the `v/a` ramp term, which costs 4 %
     * on a Battleship's longest leg and 46 % on its shortest.
     *
     * Straight run only: the ship is spawned already pointing at its target.
     * The turning case is `TheEstimateIsOptimisticWhenTheFleetMustTurn` below,
     * and it is a different claim with a different bound.
     */
    for (const HullClass hull : {HullClass::Interceptor, HullClass::Corvette, HullClass::Battleship})
    {
      // 120 m is well inside every hull's ramp distance and 18.4 km is well
      // past it, so the sweep exercises both halves of the profile. The short
      // end is the one that matters: at 900 m the two formulas differ by half a
      // per cent, and a build that dropped the triangular case entirely would
      // sail through a sweep that started there.
      for (const float distance : {120.0f, 900.0f, 4000.0f, 18400.0f})
      {
        World world;
        world.Reset(5);

        ShipSpawn spawn;
        spawn.hullClass = hull;
        spawn.xMetres = 0.0f;
        spawn.yMetres = 0.0f;
        spawn.headingRadians = 0.0f; // Already pointing at +x, which is where it is sent.
        const ShipId ship = world.Spawn(spawn);

        OrderSubmit move;
        move.orderSeq = 1;
        Assert::IsTrue(move.AddShip(ship));
        move.target.xCm = Neuron::MetresToCentimetres(distance);
        Assert::IsTrue(world.SubmitOrder(move).accepted);

        const float predicted = TravelSeconds(hull, distance);
        Assert::IsTrue(predicted > 0.0f);

        std::uint32_t tick = 0;
        const auto cap = static_cast<std::uint32_t>(predicted * 4.0f / World::TICK_SECONDS) + 200u;
        while (tick < cap)
        {
          ++tick;
          world.Tick(tick);
          const float remaining = distance - world.Positions()[0].x;
          if (std::fabs(remaining) <= World::ARRIVAL_TOLERANCE_METRES)
          {
            break;
          }
        }
        Assert::IsTrue(tick < cap, L"the ship never arrived, so there is nothing to compare");

        const float actual = static_cast<float>(tick) * World::TICK_SECONDS;
        const float error = std::fabs(actual - predicted) / actual;
        Assert::IsTrue(error < 0.03f, L"the estimate and the simulation disagree by more than a rounding");
      }
    }
  }

  TEST_METHOD(TheEstimateIsOptimisticWhenTheFleetMustTurn)
  {
    /*
     * The model's one known blind spot, pinned down rather than left as a
     * caveat in a comment.
     *
     * `World`'s steering scales desired speed by how well the ship is aligned
     * and caps it by the turn radius, and this estimate does neither. So a
     * fleet ordered to reverse course arrives *later* than promised -- 33 % for
     * a Battleship turned right around on a short hop, which is the worst case
     * across the table.
     *
     * The assertion is the **direction**, not the magnitude. A prediction that
     * ran late would be the dangerous one: a player who is told 40 seconds and
     * gets 30 is early, and one told 40 who gets 53 has planned around a
     * number that was never achievable. This is what stops a future "improve
     * the model" change from overshooting.
     */
    World world;
    world.Reset(9);

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Battleship;
    spawn.headingRadians = XM_PI; // Pointing at -x; the target is at +x.
    const ShipId ship = world.Spawn(spawn);

    const float distance = 900.0f;
    OrderSubmit move;
    move.orderSeq = 1;
    Assert::IsTrue(move.AddShip(ship));
    move.target.xCm = Neuron::MetresToCentimetres(distance);
    Assert::IsTrue(world.SubmitOrder(move).accepted);

    std::uint32_t tick = 0;
    while (tick < 4000)
    {
      ++tick;
      world.Tick(tick);
      if (std::fabs(distance - world.Positions()[0].x) <= World::ARRIVAL_TOLERANCE_METRES)
      {
        break;
      }
    }
    Assert::IsTrue(tick < 4000, L"the ship never arrived");

    const float actual = static_cast<float>(tick) * World::TICK_SECONDS;
    const float predicted = TravelSeconds(HullClass::Battleship, distance);
    Assert::IsTrue(predicted < actual, L"a turning fleet must never beat its own estimate");
    Assert::IsTrue(predicted > actual * 0.5f, L"and the estimate must still be in the right country");
  }

  TEST_METHOD(AShortHopIsNotInstant)
  {
    // `distance / maxSpeed` would call a forty-metre nudge a fifth of a second
    // for an Interceptor, when it never gets near top speed and takes over a
    // second. Station-keeping nudges are the *common* order, so the triangular
    // case is the one that shows most.
    const ShipClassInfo& info = ShipClass(HullClass::Interceptor);
    const float shortHop = 40.0f;
    Assert::IsTrue(shortHop < info.maxSpeedMetresPerSec * info.maxSpeedMetresPerSec / info.accelMetresPerSecSq,
                   L"this distance has to be inside the ramp for the test to mean anything");

    /*
     * Asserted against the triangular closed form rather than against a loose
     * "bigger than distance/speed" bound. The loose version passes for the
     * *trapezoid* too -- `d/v + v/a` is even larger -- so it would have proved
     * nothing about which branch ran.
     */
    const float reach = shortHop - World::ARRIVAL_TOLERANCE_METRES;
    const float triangular = 2.0f * std::sqrt(reach / info.accelMetresPerSecSq);
    const float trapezoid = reach / info.maxSpeedMetresPerSec + info.maxSpeedMetresPerSec / info.accelMetresPerSecSq;
    Assert::IsTrue(trapezoid > triangular * 1.5f, L"the two branches have to differ here or this proves nothing");
    Assert::AreEqual(triangular, TravelSeconds(HullClass::Interceptor, shortHop), 0.001f);
  }

  TEST_METHOD(TheTwoProfilesAgreeWhereTheyMeet)
  {
    // Trapezoid and triangle meet at `v^2/a` and both give `2v/a` there. A
    // discontinuity would show up as an ETA that jumped while the player
    // dragged the puck across one particular radius, which is the kind of thing
    // that gets reported as "the number flickers" and never reproduced.
    for (const HullClass hull : {HullClass::Interceptor, HullClass::Bomber, HullClass::Corvette, HullClass::Frigate,
                                 HullClass::Battleship, HullClass::Carrier, HullClass::Hauler, HullClass::Miner})
    {
      const ShipClassInfo& info = ShipClass(hull);
      const float meeting = info.maxSpeedMetresPerSec * info.maxSpeedMetresPerSec / info.accelMetresPerSecSq;
      const float below = TravelSeconds(hull, meeting + World::ARRIVAL_TOLERANCE_METRES - 0.5f);
      const float above = TravelSeconds(hull, meeting + World::ARRIVAL_TOLERANCE_METRES + 0.5f);
      Assert::IsTrue(std::fabs(above - below) < 0.02f, L"the estimate jumps at the boundary between its two cases");
    }
  }

  TEST_METHOD(AStationCannotMakeTheJourneyAndSaysSo)
  {
    // Not slow -- unable. Returning a huge number would read as an answer and
    // put `ETA 99999s` under a ghost; returning zero would read as "already
    // there". Negative is the only honest third thing.
    Assert::IsTrue(TravelSeconds(HullClass::Structure, 5000.0f) < 0.0f);

    // And already-there is genuinely zero, at any class.
    Assert::AreEqual(0.0f, TravelSeconds(HullClass::Interceptor, 0.0f));
    Assert::AreEqual(0.0f, TravelSeconds(HullClass::Structure, 0.0f));
  }

  TEST_METHOD(TheReplicatedEtaTracksTheRealArrivalAllTheWayDown)
  {
    /*
     * S12's accept criterion -- "ETA error < 10 % on straight runs" -- measured
     * against the simulation rather than asserted about the formula.
     *
     * Sampled at **every tick of the whole leg**, not once at the start, which
     * is the only way to catch the failure this replaces: a rest-to-rest
     * estimate of a *remaining* distance is accurate at the start and hopeless
     * at the end, because it charges an acceleration ramp the ships already
     * paid. Measured, that version peaks at 44.8 % with a second left; this one
     * peaks at 2.4 %.
     *
     * The last second is excluded: below one second the wire's whole-second
     * quantisation is the dominant error, and asserting through it would be
     * asserting about rounding.
     */
    for (const HullClass hull : {HullClass::Interceptor, HullClass::Corvette, HullClass::Battleship})
    {
      for (const float distance : {4000.0f, 18400.0f})
      {
        World world;
        world.Reset(3);
        ShipSpawn spawn;
        spawn.hullClass = hull;
        spawn.headingRadians = 0.0f; // Straight run: already pointing at the target.
        const ShipId ship = world.Spawn(spawn);

        OrderSubmit move;
        move.orderSeq = 1;
        Assert::IsTrue(move.AddShip(ship));
        move.target.xCm = Neuron::MetresToCentimetres(distance);
        Assert::IsTrue(world.SubmitOrder(move).accepted);

        std::vector<float> sampled;
        std::uint32_t tick = 0;
        while (tick < 20000)
        {
          ++tick;
          world.Tick(tick);
          sampled.push_back(world.Groups().empty() ? -1.0f : world.LegEtaSeconds(world.Groups()[0]));
          if (std::fabs(distance - world.Positions()[0].x) <= World::ARRIVAL_TOLERANCE_METRES)
          {
            break;
          }
        }
        Assert::IsTrue(tick < 20000, L"the ship never arrived, so there is nothing to compare");

        int compared = 0;
        for (std::size_t index = 0; index < sampled.size(); ++index)
        {
          if (sampled[index] < 0.0f)
          {
            continue;
          }
          const float trueRemaining = static_cast<float>(tick - (index + 1)) * World::TICK_SECONDS;
          if (trueRemaining < 1.0f)
          {
            continue;
          }
          const float error = std::fabs(sampled[index] - trueRemaining) / trueRemaining;
          Assert::IsTrue(error < 0.10f, L"the replicated ETA drifted more than a tenth from the real arrival");
          ++compared;
        }
        Assert::IsTrue(compared > 20, L"the sweep has to have actually compared something");
      }
    }
  }

  TEST_METHOD(SpeedAlreadyGainedShortensTheEstimate)
  {
    // The generalisation, stated on its own. A ship at top speed with a
    // kilometre to run does not have to accelerate into it again.
    const ShipClassInfo& info = ShipClass(HullClass::Corvette);
    const float distance = 1000.0f;

    const float fromRest = RemainingSeconds(HullClass::Corvette, distance, 0.0f);
    const float moving = RemainingSeconds(HullClass::Corvette, distance, info.maxSpeedMetresPerSec);
    Assert::IsTrue(moving < fromRest, L"already moving must never take longer");
    Assert::IsTrue(moving < fromRest * 0.85f, L"and the difference has to be worth the arithmetic");

    // And the two forms are one profile rather than two kept in step: the
    // rest-to-rest name is the general one at zero speed, exactly.
    for (const float sweep : {40.0f, 400.0f, 4000.0f, 40000.0f})
    {
      Assert::AreEqual(TravelSeconds(HullClass::Corvette, sweep), RemainingSeconds(HullClass::Corvette, sweep, 0.0f), 0.0f);
    }
  }

  TEST_METHOD(ARedirectedFleetIsNotCreditedWithSpeedItMustFirstShed)
  {
    /*
     * Speed is counted **along the way the ship is going**, not outright.
     *
     * The case is ordinary: a fleet at cruise is sent back the way it came. Its
     * velocity is large and points entirely the wrong way, and crediting the
     * magnitude promises an arrival that velocity is carrying it *away* from.
     * The projection is negative here and the model clamps it to zero, which is
     * the honest reading -- no progress is being made toward the new station.
     *
     * Measured: 22.4 s against a real 24.2 s. Taking the speed outright gives
     * 20.1 s. The bound below passes the first and fails the second.
     */
    World world;
    world.Reset(7);
    const ShipId ship = SpawnAt(world, HullClass::Corvette, 0.0f, 0.0f);

    OrderSubmit outbound = Order({ship}, 15000.0f, 0.0f);
    Assert::IsTrue(world.SubmitOrder(outbound).accepted);
    for (std::uint32_t tick = 1; tick <= 400; ++tick)
    {
      world.Tick(tick);
    }
    Assert::IsTrue(world.Velocities()[0].x > 100.0f, L"the fleet has to actually be moving for this to test anything");

    OrderSubmit back = Order({ship}, 0.0f, 0.0f);
    back.orderSeq = 2;
    Assert::IsTrue(world.SubmitOrder(back).accepted);
    world.Tick(401);

    const float predicted = world.LegEtaSeconds(world.Groups()[0]);
    Assert::IsTrue(predicted > 0.0f);

    std::uint32_t tick = 401;
    std::uint32_t arrived = 0;
    while (tick < World::LEG_TIMEOUT_MAX_TICKS && arrived == 0)
    {
      ++tick;
      world.Tick(tick);
      if (std::fabs(world.Positions()[0].x) <= World::ARRIVAL_TOLERANCE_METRES)
      {
        arrived = tick;
      }
    }
    Assert::IsTrue(arrived > 0, L"the fleet never got back");

    const float actual = static_cast<float>(arrived - 401) * World::TICK_SECONDS;
    Assert::IsTrue(std::fabs(predicted - actual) / actual < 0.12f,
                   L"the estimate credited speed that was pointing the wrong way");
  }

  TEST_METHOD(TheGroupEtaIsMeasuredToEachStationNotToTheAnchor)
  {
    /*
     * Each member's own station, not the group's anchor. A wide formation on a
     * short hop is where the two diverge: eight Battleships in a Line are 480 m
     * apart, so the far station is well over two kilometres past the anchor --
     * and a leg completes when the *last* of them arrives.
     *
     * The same claim `SolvePreview` makes on the client's side, asserted here
     * against the authority's own number.
     */
    World world;
    world.Reset(41);
    const float hop = 6000.0f;
    OrderSubmit line;
    line.orderSeq = 1;
    line.formation = FormationId::Line;
    line.target.xCm = Neuron::MetresToCentimetres(hop);
    for (int index = 0; index < 8; ++index)
    {
      Assert::IsTrue(line.AddShip(SpawnAt(world, HullClass::Battleship, 0.0f, static_cast<float>(index) * 60.0f)));
    }
    Assert::IsTrue(world.SubmitOrder(line).accepted);
    world.Tick(1);

    const float groupEta = world.LegEtaSeconds(world.Groups()[0]);
    const float anchorOnly = TravelSeconds(HullClass::Battleship, hop);
    Assert::IsTrue(groupEta > 0.0f);
    Assert::IsTrue(groupEta > anchorOnly * 1.05f,
                   L"the far end of the formation has to set the ETA, not the anchor");
  }

  TEST_METHOD(AGroupWithNothingUnderWayReportsNoEta)
  {
    // Distinct from zero, which means *arriving now*. A `Done` group that
    // reported zero would put `ETA 0s` under a ghost that has finished.
    World world;
    world.Reset(8);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Corvette;
    const ShipId ship = world.Spawn(spawn);

    OrderSubmit move;
    move.orderSeq = 1;
    Assert::IsTrue(move.AddShip(ship));
    move.target.xCm = Neuron::MetresToCentimetres(300.0f);
    Assert::IsTrue(world.SubmitOrder(move).accepted);

    std::uint32_t tick = 0;
    while (tick < 2000 && (world.Groups().empty() || world.Groups()[0].state != OrderState::Done))
    {
      ++tick;
      world.Tick(tick);
    }
    Assert::IsFalse(world.Groups().empty());
    Assert::IsTrue(world.Groups()[0].state == OrderState::Done);
    Assert::IsTrue(world.LegEtaSeconds(world.Groups()[0]) < 0.0f);
  }

  TEST_METHOD(TheEtaCrossesTheWireAsWholeSeconds)
  {
    /*
     * Rounded **up**, for the reason the ghost's label rounds up: `0s` while
     * the ships are visibly still moving reads as a broken readout, and a
     * second of pessimism at the end costs nothing.
     */
    World world;
    world.Reset(12);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Battleship;
    const ShipId ship = world.Spawn(spawn);

    OrderSubmit move;
    move.orderSeq = 5;
    Assert::IsTrue(move.AddShip(ship));
    move.target.xCm = Neuron::MetresToCentimetres(9000.0f);
    Assert::IsTrue(world.SubmitOrder(move).accepted);
    world.Tick(1);

    const float source = world.LegEtaSeconds(world.Groups()[0]);
    Assert::IsTrue(source > 0.0f);

    std::array<std::uint8_t, 2048> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteSnapshot(world, writer));

    Neuron::ByteReader reader{writer.Written()};
    SnapshotHeader header;
    std::vector<Neuron::EntityRecord> ships;
    std::vector<OrderStateRecord> orders;
    Assert::IsTrue(ReadSnapshot(reader, header, ships, orders));
    Assert::AreEqual<std::size_t>(1, orders.size());

    Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(std::ceil(source)), orders[0].etaSeconds);
    Assert::IsTrue(orders[0].etaSeconds != NO_ETA, L"a live leg has an answer");
    Assert::IsTrue(static_cast<float>(orders[0].etaSeconds) >= source, L"rounded up, never down");
  }

  TEST_METHOD(TheOrderAreaStillFitsWithTheEtaOnIt)
  {
    // The record grew from 12 bytes to 14, and the comment on it says a field
    // added here costs ships. It cost two.
    Assert::AreEqual<std::size_t>(14, ORDER_STATE_RECORD_BYTES);
    Assert::IsTrue(MAX_SHIPS_PER_SNAPSHOT >= 41, L"the MVP fleet still has to fit one datagram");
    Assert::IsTrue(SnapshotBytes(MAX_SHIPS_PER_SNAPSHOT, MAX_ORDERS_PER_SNAPSHOT) <= SNAPSHOT_BUDGET_BYTES);
    Assert::IsTrue(GAME_SCHEMA_TEXT.find("u16 etaSeconds") != std::string_view::npos,
                   L"a field on the wire that is not in the schema is two builds disagreeing silently");
  }

  TEST_METHOD(AGroupWaitsForItsSlowestMember)
  {
    /*
     * A leg completes when *every* member is within tolerance (ADR-005 §2), so
     * the arrival that matters is the last one. An average would promise a
     * fleet of Interceptors with one Battleship in it the Interceptors' time --
     * wrong at exactly the moment the player is counting on it.
     */
    const TravelLeg legs[] = {{HullClass::Interceptor, 6000.0f}, {HullClass::Interceptor, 6000.0f},
                              {HullClass::Battleship, 6000.0f}};
    const float group = GroupTravelSeconds(legs);
    Assert::AreEqual(TravelSeconds(HullClass::Battleship, 6000.0f), group, 0.001f, L"the Battleship decides");
    Assert::IsTrue(group > TravelSeconds(HullClass::Interceptor, 6000.0f) * 1.5f);
  }

  TEST_METHOD(AStationInTheGroupDoesNotSilenceTheRest)
  {
    // A selection with a station in it should still report when its ships get
    // there. Only a group that is *entirely* immobile has nothing to say.
    const TravelLeg mixed[] = {{HullClass::Structure, 3000.0f}, {HullClass::Corvette, 3000.0f}};
    Assert::AreEqual(TravelSeconds(HullClass::Corvette, 3000.0f), GroupTravelSeconds(mixed), 0.001f);

    const TravelLeg stations[] = {{HullClass::Structure, 3000.0f}};
    Assert::IsTrue(GroupTravelSeconds(stations) < 0.0f);
    Assert::IsTrue(GroupTravelSeconds({}) < 0.0f, L"an empty group has no ETA either");
  }

  TEST_METHOD(TheOrderAndFormationBothHaveNames)
  {
    // The `MOVE - CLAW` half of the ghost's label. The engine may name neither
    // (ADR-014 §2b), so a null here is a ghost with no caption.
    Assert::IsNotNull(OrderKindName(OrderKind::Move));
    for (const FormationId formation : FORMATION_IDS)
    {
      Assert::IsNotNull(FormationName(formation));
      Assert::IsTrue(std::string_view{FormationName(formation)}.size() > 0);
    }
  }
};

/*
 * The command row's commands (S11d).
 *
 * Three of the four are reserved -- nameable, numbered, never submittable, the
 * same arrangement `HullClass`'s Fighter and Cruiser have (ADR-009 §6).
 */
TEST_CLASS(OrderKindTests)
{
public:
  TEST_METHOD(EveryKindIsNamedAndOnlyMoveHasContent)
  {
    // A greyed ATTACK button still has to be *named* by something, and the
    // engine may not name it -- so a null here is a button with no word on it.
    Assert::AreEqual<std::size_t>(4, std::size(ORDER_KIND_IDS));
    for (const OrderKind kind : ORDER_KIND_IDS)
    {
      Assert::IsNotNull(OrderKindName(kind));
      Assert::IsTrue(std::string_view{OrderKindName(kind)}.size() > 0);
    }

    Assert::IsTrue(OrderKindHasContent(OrderKind::Move));
    Assert::IsFalse(OrderKindHasContent(OrderKind::Attack));
    Assert::IsFalse(OrderKindHasContent(OrderKind::Stance));
    Assert::IsFalse(OrderKindHasContent(OrderKind::Abilities));
  }

  TEST_METHOD(AReservedKindIsRefusedRatherThanSimulated)
  {
    /*
     * The half that matters. `OrderKindHasContent` says a surface may not
     * *offer* it; this says the world will not *act* on it -- and the two are
     * separate on purpose, so a kind gaining content has to change both rather
     * than one of them silently letting it through.
     *
     * Reserved kinds are on the wire the moment they are enumerated, so this is
     * also the check that a client from a build that has Attack cannot make
     * this build do anything with it.
     */
    World world;
    world.Reset(4);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Corvette;
    const ShipId ship = world.Spawn(spawn);

    for (const OrderKind kind : ORDER_KIND_IDS)
    {
      OrderSubmit order;
      order.orderSeq = 1;
      order.kind = kind;
      Assert::IsTrue(order.AddShip(ship));
      order.target.xCm = Neuron::MetresToCentimetres(1000.0f);

      const auto verdict = world.SubmitOrder(order);
      if (OrderKindHasContent(kind))
      {
        Assert::IsTrue(verdict.accepted, L"the one kind with content has to work");
      }
      else
      {
        Assert::IsFalse(verdict.accepted, L"a reserved kind must never be acted on");
        Assert::IsTrue(verdict.reason == OrderReason::UnknownKind, L"and must say why in the game's own words");
      }
    }
  }

  TEST_METHOD(OnlyTheKindsThatVaryHaveAParameterName)
  {
    // `FORMATION` is the word for what a Move varies by. Attack takes a target
    // rather than a parameter, and a button labelled with nothing is still a
    // button -- so it answers null rather than an empty string.
    Assert::IsNotNull(OrderKindParameterName(OrderKind::Move));
    Assert::AreEqual(std::string_view{"Formation"}, std::string_view{OrderKindParameterName(OrderKind::Move)});
    Assert::IsNull(OrderKindParameterName(OrderKind::Attack));
    Assert::IsNotNull(OrderKindParameterName(OrderKind::Stance));
    Assert::IsNotNull(OrderKindParameterName(OrderKind::Abilities));
  }

  TEST_METHOD(TheKindsAreOnTheSchemaAndNumberedFromZero)
  {
    // They cross the wire in `OrderSubmit`, so two builds disagreeing about the
    // numbering would misread every order between them.
    Assert::AreEqual<std::uint8_t>(0, static_cast<std::uint8_t>(OrderKind::Move));
    for (std::size_t index = 0; index < std::size(ORDER_KIND_IDS); ++index)
    {
      Assert::AreEqual<std::size_t>(index, static_cast<std::size_t>(ORDER_KIND_IDS[index]),
                                    L"contiguous from zero, in the order a surface shows them");
    }
    Assert::IsTrue(GAME_SCHEMA_TEXT.find("OrderKind:Move=0,Attack=1,Stance=2,Abilities=3") != std::string_view::npos);
  }
};

} // namespace GameLogicTests
