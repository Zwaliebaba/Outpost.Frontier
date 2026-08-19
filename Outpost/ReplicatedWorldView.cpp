#include "pch.h"

#include "ReplicatedWorldView.h"

#include "Eta.h"
#include "Formation.h"
#include "OrderMessages.h"
#include "SchemaHash.h"
#include "ShipClass.h"
#include "Validate.h"

#include "EntityRecord.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

using namespace Neuron;

namespace Outpost
{
namespace
{

/*
 * A seam intent as the order both sides validate, or a reason it cannot be one.
 *
 * The one conversion, used by `PreCheck` and by `EncodeOrder` alike. Two copies
 * would be two roundings, and ADR-005 §4's parity rule is precisely that the
 * numbers validated are the numbers sent -- a pre-check that quantised
 * differently from the encoder would pass an order the server then refused, and
 * the client would have no way to explain it.
 *
 * The two refusals here are the ones the struct cannot hold rather than the
 * ones the rules forbid: a selection larger than `MAX_SHIPS_PER_ORDER` has
 * nowhere to go, and a kind or formation past a byte would truncate into a
 * *different valid* value. Both reuse `ValidateOrder`'s own reason codes, so a
 * client refusal here and a server refusal there still say the same thing.
 */
[[nodiscard]] bool MakeSubmit(const OrderIntent& _intent, Game::OrderSubmit& _outOrder, Game::OrderReason& _outReason) noexcept
{
  if (_intent.entityCount > Game::MAX_SHIPS_PER_ORDER)
  {
    _outReason = Game::OrderReason::TooManyShips;
    return false;
  }
  if (_intent.kind > 0xffu)
  {
    _outReason = Game::OrderReason::UnknownKind;
    return false;
  }
  if (_intent.parameter > 0xffu)
  {
    _outReason = Game::OrderReason::InvalidFormation;
    return false;
  }

  _outOrder = Game::OrderSubmit{};
  _outOrder.orderSeq = _intent.orderSeq;
  _outOrder.kind = static_cast<Game::OrderKind>(_intent.kind);
  _outOrder.formation = static_cast<Game::FormationId>(_intent.parameter);
  _outOrder.queueMode = _intent.queued ? Game::QueueMode::Append : Game::QueueMode::Replace;

  // Quantised once, here, and never again. Everything downstream -- the bounds
  // check, the wire, the server's own validation -- sees these integers.
  _outOrder.target.xCm = Neuron::MetresToCentimetres(_intent.targetXMetres);
  _outOrder.target.yCm = Neuron::MetresToCentimetres(_intent.targetYMetres);
  _outOrder.target.facingTurns16 = Neuron::RadiansToHeading(_intent.facingRadians);

  for (std::uint32_t index = 0; index < _intent.entityCount; ++index)
  {
    if (!_outOrder.AddShip(_intent.entityIds[index]))
    {
      // Unreachable: the count was checked against the same cap above. Refusing
      // rather than asserting keeps a future caller that skips the check from
      // sending a silently shortened order.
      _outReason = Game::OrderReason::TooManyShips;
      return false;
    }
  }
  return true;
}

/// `Game::SolveFormation`'s hull lookup, over the ships this frame sampled.
/// A ship the view does not have takes the smallest class: the solve maxes the
/// spacings, so an unknown member can never widen the formation, and
/// `ValidateOrder` has already refused the order it would have belonged to.
[[nodiscard]] Game::HullClass HullClassOf(Game::ShipId _shipId, void* _context) noexcept
{
  const auto& sampled = *static_cast<const std::vector<Game::ReplicatedShip>*>(_context);
  const auto found =
      std::find_if(sampled.begin(), sampled.end(), [_shipId](const Game::ReplicatedShip& _s) { return _s.id == _shipId; });
  return found == sampled.end() ? Game::HullClass::Interceptor : static_cast<Game::HullClass>(found->classId);
}

[[nodiscard]] OrderVerdict ToSeam(const Game::OrderVerdict& _verdict, std::uint32_t _orderSeq) noexcept
{
  OrderVerdict verdict;
  verdict.accepted = _verdict.accepted;
  verdict.reasonCode = static_cast<std::uint16_t>(_verdict.reason);
  verdict.serverOrderId = _verdict.serverOrderId;
  verdict.orderSeq = _orderSeq;
  return verdict;
}

} // namespace

ReplicatedWorldView::ReplicatedWorldView(Desc _desc)
  : m_desc(std::move(_desc))
{
}

std::uint32_t ReplicatedWorldView::ApplySnapshot(std::span<const std::uint8_t> _payload)
{
  if (!m_view.ApplySnapshot(_payload))
  {
    // Malformed or truncated. Counted rather than logged per occurrence: on a
    // lossy link this would be the noisiest line in the file, and the number
    // is what actually says whether it is happening.
    ++m_rejectedSnapshots;
    return 0;
  }
  return m_view.LatestTick();
}

void ReplicatedWorldView::BuildScene(double _renderTick, RenderScene& _outScene)
{
  m_view.SampleAt(_renderTick, m_sampled);

  _outScene.Clear();
  _outScene.instances.reserve(m_sampled.size());
  _outScene.entities.reserve(m_sampled.size());
  m_validationIds.clear();
  m_validationIds.reserve(m_sampled.size());

  std::uint32_t renderClassCount = 0;
  for (const Game::ReplicatedShip& ship : m_sampled)
  {
    if (ship.classId >= m_desc.renderClassByHull.size())
    {
      continue; // A hull class this build has no mapping for.
    }
    const std::uint16_t renderClass = m_desc.renderClassByHull[ship.classId];
    if (renderClass == INVALID_RENDER_CLASS)
    {
      // A hull with no mesh -- the two reserved classes. Drawing nothing is the
      // honest answer; substituting another hull would put a ship on screen
      // that is not the ship the server is simulating.
      continue;
    }

    const auto hull = static_cast<Game::HullClass>(ship.classId);
    const Game::ShipClassInfo& classInfo = Game::ShipClass(hull);

    InstanceRecord instance;
    // Local plane metres straight into render space: x east, y the cosmetic
    // height, z north (ADR-001 §3). The height is the class's hover -- purely
    // cosmetic, so the entity below keeps the plane point and the selection
    // ring stays on the ground beneath the hull.
    instance.posWorld = DirectX::XMFLOAT3{ship.positionMetres.x, classInfo.hoverMetres, ship.positionMetres.y};
    instance.heading = ship.headingRadians;
    instance.teamColorId = 0;
    // Selection and LOD bias are the overlay's channels (S8). The stale flag
    // rides here so the marker the icon sheet draws has something to read.
    instance.selectionAndLodBias = ship.stale ? 1u : 0u;
    instance.classId = renderClass;
    // The bank, from replicated quantities only (ADR-006 §6): the heading rate
    // the view measured between its two bracketing snapshots, and the sampled
    // speed. This is the "computed in Extract" the ADR reserves, running on the
    // game's side of the seam because the class envelope it normalises by is
    // the game's.
    const float speed = std::sqrt(ship.velocityMetresPerSec.x * ship.velocityMetresPerSec.x +
                                  ship.velocityMetresPerSec.y * ship.velocityMetresPerSec.y);
    instance.bank = Game::CosmeticBankRadians(hull, ship.headingRateRadiansPerSec, speed);
    _outScene.instances.push_back(instance);

    // The same ship again, in the shape picking and the overlay want. `id`
    // crosses the seam as an opaque number -- the engine hands back whatever it
    // was given, and only this project knows it is a `ShipId`. A hull the wire
    // named but this build has no class for was already skipped above; a ship
    // that is not drawn must not be selectable either, or a player picks
    // something they cannot see.
    Neuron::SceneEntity entity;
    entity.id = ship.id;
    entity.planeMetres = ship.positionMetres;
    entity.pickRadiusMetres = classInfo.pickRadiusMetres;
    entity.hullGauge = ship.hullGauge;
    entity.shieldGauge = ship.shieldGauge;
    entity.stale = ship.stale;
    _outScene.entities.push_back(entity);

    // The same id again, for `ValidateOrder`. Filled here rather than in
    // `PreCheck` so that the ships an order may name are exactly the ships this
    // frame drew -- including the exclusions above, which is the point: a hull
    // with no mesh cannot be clicked, so it must not be orderable either.
    m_validationIds.push_back(ship.id);

    renderClassCount = std::max(renderClassCount, static_cast<std::uint32_t>(renderClass) + 1u);
  }

  // Sorted so the opaque pass draws each class in one instanced run. The count
  // is the highest class actually present, not the mesh table's size: a scene
  // with only Interceptors needs one range, not nine.
  _outScene.SortByClass(renderClassCount);
}

OrderVerdict ReplicatedWorldView::PreCheck(const OrderIntent& _intent)
{
  Game::OrderSubmit order;
  Game::OrderReason reason = Game::OrderReason::Accepted;
  if (!MakeSubmit(_intent, order, reason))
  {
    return ToSeam(Game::OrderVerdict{false, reason}, _intent.orderSeq);
  }

  Game::ValidationView view;
  view.shipIds = m_validationIds;

  /*
   * Zero, and knowingly so.
   *
   * The server resolves `queuedLegs` from the group the first named ship
   * belongs to (`World::SubmitOrder`), and the client cannot: a snapshot order
   * record carries a member *count* and not the members, so there is no way to
   * ask which group a selection is in. Reporting zero means an append that
   * would fill the queue passes here and is refused by the authority a round
   * trip later -- ADR-005 §4's designed and accepted case, and the direction
   * that costs the least. The other direction is worse: a client that guessed
   * high would refuse locally an order the server would have taken, and no
   * amount of waiting would get the player past it.
   *
   * Fixing it properly means the snapshot carrying group membership -- two
   * bytes per member per order, against a 1,150-byte datagram -- to make one
   * refusal instant that is already correct. Not paid.
   *
   * **Rechecked in S12 and still zero.** The queue slice replicates
   * `legCount`, which looks like the missing number and is not: it is per
   * *order record*, and the question here is which record a *selection* belongs
   * to -- still unanswerable from a member count. The client could instead
   * track membership itself, since it built every order it sent, but the rule
   * that resolves it ("the group the first named ship is in") is a game rule
   * and the ghost list it would read is engine state; wiring one to the other
   * to save a round trip is a seam crossing for a refusal that already works.
   *
   * S12's own acceptance criterion says **wire**-enforced, and this is what
   * that means: the fifth leg bounces from the authority with `QueueFull`,
   * through the same ack, the same 150 ms retraction and the same reason string
   * as any other refusal. ADR-005 §4's parity claim is that a local refusal and
   * a remote one are indistinguishable *to the player*, not that every refusal
   * is local.
   */
  view.queuedLegs = 0;

  return ToSeam(Game::ValidateOrder(view, order), _intent.orderSeq);
}

void ReplicatedWorldView::SolvePreview(const OrderIntent& _intent, OrderPreview& _outPreview)
{
  _outPreview.Clear();

  Game::OrderSubmit order;
  Game::OrderReason reason = Game::OrderReason::Accepted;
  if (!MakeSubmit(_intent, order, reason) || order.shipCount == 0)
  {
    return;
  }

  // The real solve, one station per ship, from the same quantised leg the
  // order will carry -- not the raw click. The corpus is explicit that the
  // footprint must be the formation solve and never a decorative ellipse: a
  // preview showing nine stations for twelve ships has lied to the player
  // before the order left the client.
  const DirectX::XMFLOAT2 anchor{Neuron::CentimetresToMetres(order.target.xCm),
                                 Neuron::CentimetresToMetres(order.target.yCm)};
  const float facing = Neuron::HeadingToRadians(order.target.facingTurns16);

  Game::FormationStation stations[Neuron::MAX_ORDER_PREVIEW_MARKS];
  static_assert(Neuron::MAX_ORDER_PREVIEW_MARKS >= Game::MAX_SHIPS_PER_ORDER,
                "an order the game will accept must have a footprint the client can draw whole");

  const std::uint32_t count = Game::SolveFormation(order.formation, std::span<const Game::ShipId>{order.shipIds, order.shipCount},
                                                   &HullClassOf, &m_sampled, anchor, facing, stations);
  for (std::uint32_t index = 0; index < count; ++index)
  {
    if (!_outPreview.AddMark(stations[index].positionMetres.x, stations[index].positionMetres.y))
    {
      break; // Cannot happen: the assert above sizes the preview for the cap.
    }
  }
  _outPreview.extentMetres = Game::FormationExtentMetres(std::span<const Game::FormationStation>{stations, count}, anchor);

  /*
   * The ghost's label, in the game's own words and by the game's own movement
   * model. Both halves are here because the engine may compute neither: `MOVE`
   * is a kind and `CLAW` is a formation (ADR-014 §2b), and seconds-to-arrive
   * needs acceleration curves that live in the class table.
   *
   * The ETA is **each ship's journey to its own station**, not the group's
   * centre to the anchor. Those differ by the formation's radius, which for a
   * Line of twelve is most of a kilometre -- and it is the far end of the line
   * that decides when the leg completes.
   */
  Game::TravelLeg legs[Neuron::MAX_ORDER_PREVIEW_MARKS];
  std::uint32_t legCount = 0;
  for (std::uint32_t index = 0; index < count; ++index)
  {
    const auto found = std::find_if(m_sampled.begin(), m_sampled.end(), [&stations, index](const Game::ReplicatedShip& _ship) {
      return _ship.id == stations[index].shipId;
    });
    if (found == m_sampled.end())
    {
      continue; // Despawned between the selection and this solve.
    }

    // Off the wire, so bounds-checked rather than cast. An unknown class here
    // is a build mismatch that the handshake should already have refused; what
    // it must not do is index the table with it.
    Game::HullClass hullClass = Game::HullClass::Interceptor;
    if (!Game::TryShipClass(found->classId, hullClass))
    {
      continue;
    }

    const float dx = stations[index].positionMetres.x - found->positionMetres.x;
    const float dy = stations[index].positionMetres.y - found->positionMetres.y;
    legs[legCount] = Game::TravelLeg{hullClass, std::sqrt(dx * dx + dy * dy)};
    ++legCount;
  }
  _outPreview.etaSeconds = Game::GroupTravelSeconds(std::span<const Game::TravelLeg>{legs, legCount});

  std::snprintf(_outPreview.label, sizeof(_outPreview.label), "%s - %s", Game::OrderKindName(order.kind),
                Game::FormationName(order.formation));
}

bool ReplicatedWorldView::EncodeOrder(const OrderIntent& _intent, ByteWriter& _writer)
{
  Game::OrderSubmit order;
  Game::OrderReason reason = Game::OrderReason::Accepted;
  if (!MakeSubmit(_intent, order, reason))
  {
    return false; // Not sent, rather than sent malformed.
  }
  return Game::WriteOrderSubmit(order, _writer);
}

OrderDefaults ReplicatedWorldView::DefaultOrder() const
{
  // The MVP's one command, in the MVP's one formation. Named rather than left
  // to the client's zeroed fields, so that adding Attack is a change here and
  // not a coincidence that stops holding.
  OrderDefaults defaults;
  defaults.kind = static_cast<std::uint16_t>(Game::OrderKind::Move);
  defaults.parameter = static_cast<std::uint16_t>(Game::FormationId::Line);
  return defaults;
}

std::uint32_t ReplicatedWorldView::OrderOptions(std::uint16_t _kind, std::span<OrderOption> _outOptions) const
{
  // Move's parameter is a formation. No other kind exists yet, and a kind that
  // took no parameter would answer zero here rather than be special-cased at
  // the client -- which is what makes "an empty answer is legitimate" a real
  // path rather than a line in a comment.
  if (_kind != static_cast<std::uint16_t>(Game::OrderKind::Move))
  {
    return 0;
  }

  std::uint32_t count = 0;
  for (const Game::FormationId formation : Game::FORMATION_IDS)
  {
    if (count >= _outOptions.size())
    {
      break;
    }
    _outOptions[count].parameter = static_cast<std::uint16_t>(formation);
    _outOptions[count].name = Game::FormationName(formation);
    ++count;
  }
  return count;
}

std::uint32_t ReplicatedWorldView::OrderKinds(std::span<OrderKindOption> _outKinds) const
{
  // Every kind the game has a value for, including the three with no content.
  // Reporting only the working one would give the row a single button today and
  // five later, moving the one the player had learned to reach for -- which is
  // the same argument `puck-and-wheel.png` §3 makes for the wheel's sectors
  // keeping fixed positions.
  std::uint32_t count = 0;
  for (const Game::OrderKind kind : Game::ORDER_KIND_IDS)
  {
    if (count >= _outKinds.size())
    {
      break;
    }
    _outKinds[count].kind = static_cast<std::uint16_t>(kind);
    _outKinds[count].name = Game::OrderKindName(kind);
    _outKinds[count].parameterName = Game::OrderKindParameterName(kind);
    _outKinds[count].available = Game::OrderKindHasContent(kind);
    ++count;
  }
  return count;
}

std::uint32_t ReplicatedWorldView::BuildRoster(std::span<const std::uint16_t> _selectedIds,
                                              std::span<RosterRow> _outRows) const
{
  /*
   * One pass over the sampled fleet, accumulating per wing.
   *
   * `m_sampled` rather than the newest snapshot's records, so the roster counts
   * exactly the ships the frame drew -- including the exclusions `BuildScene`
   * makes. A roster that listed a ship the player cannot see would be a roster
   * they cannot act on.
   */
  struct Accumulator
  {
    std::uint16_t ships = 0;
    std::uint16_t selected = 0;
    std::uint32_t hullTotal = 0;
    std::uint32_t shieldTotal = 0;
  };
  /*
   * Indexed by `WingId` directly, so the table is every wing that can exist
   * rather than every wing that is named -- one byte, so 256 entries, three
   * kilobytes, on the stack.
   *
   * A `vector` sized from the name list is the obvious spelling and is an
   * allocation on every frame, inside the one function whose entire job is to
   * describe the frame. It also needed a bounds check that this does not: a
   * `WingId` cannot index past a table with an entry per `WingId`.
   */
  std::array<Accumulator, static_cast<std::size_t>(std::numeric_limits<Game::WingId>::max()) + 1u> byWing{};

  for (const Game::ReplicatedShip& ship : m_sampled)
  {
    // Wing zero is `INVALID_WING_ID` and belongs to nothing -- the stations
    // are in it. A row for "no wing" would be a row the player cannot command.
    if (ship.wing == Game::INVALID_WING_ID)
    {
      continue;
    }

    Accumulator& wing = byWing[ship.wing];
    ++wing.ships;
    wing.hullTotal += ship.hullGauge;
    wing.shieldTotal += ship.shieldGauge;

    if (std::find(_selectedIds.begin(), _selectedIds.end(), ship.id) != _selectedIds.end())
    {
      ++wing.selected;
    }
  }

  /*
   * The *names* decide which wings are rows, not the tally above.
   *
   * A ship replicated in a wing this build has no name for is counted into the
   * table and then never emitted, which is the right way round: the roster is
   * a list of the wings the game declared, and a row whose label had to be
   * invented would be a row naming something the player was never told about.
   */
  std::uint32_t rows = 0;
  for (std::size_t wingId = 1; wingId < m_desc.wingNames.size(); ++wingId)
  {
    if (rows >= _outRows.size())
    {
      break;
    }
    const Accumulator& wing = byWing[wingId];

    /*
     * A wing with nothing left in it is still a row.
     *
     * The print draws one -- `ECHO` with a dash where its count should be --
     * and it is the right answer rather than a spare pixel: a wing that
     * vanished from the roster the moment its last ship died would tell the
     * player nothing about *which* wing they just lost, at the one moment they
     * most need to know. The gauges read zero, which is what an empty wing has.
     */
    RosterRow& row = _outRows[rows];
    row.name = m_desc.wingNames[wingId].c_str();
    row.groupId = static_cast<std::uint16_t>(wingId);
    row.shipCount = wing.ships;
    row.selectedCount = wing.selected;

    // The mean, not the minimum. A strip that dropped to the worst member would
    // read as the whole wing being hurt when one Interceptor is; the print
    // draws a bar per wing rather than per ship for exactly the opposite
    // reason, and the roster is a summary.
    row.hullGauge = wing.ships == 0 ? 0 : static_cast<std::uint8_t>(wing.hullTotal / wing.ships);
    row.shieldGauge = wing.ships == 0 ? 0 : static_cast<std::uint8_t>(wing.shieldTotal / wing.ships);
    ++rows;
  }
  return rows;
}

void ReplicatedWorldView::PollOrderFeedback(OrderFeedback& _outFeedback)
{
  static_assert(static_cast<std::uint32_t>(Game::MAX_ORDERS_PER_SNAPSHOT) <= Neuron::MAX_ORDER_PROGRESS,
                "the client must be able to hold every order a snapshot can describe, or a ghost it is drawing has no "
                "path to promotion");

  _outFeedback.Clear();
  _outFeedback.lastOrderSeqProcessed = m_view.LastOrderSeqProcessed();

  for (const Game::OrderStateRecord& record : m_view.LatestOrders())
  {
    OrderProgress progress;
    progress.serverOrderId = record.serverOrderId;
    progress.clientOrderSeq = record.clientOrderSeq;
    progress.state = record.state;
    progress.legIndex = record.legIndex;
    progress.legCount = record.legCount;
    progress.memberCount = record.memberCount;

    // Which of this game's states means "over", answered here because this is
    // the only side that knows. The engine retires the ghost on the bool and
    // never learns that `Done` is a 2.
    progress.finished = record.state == static_cast<std::uint8_t>(Game::OrderState::Done);

    // Dequantised here, where the wire's `NO_ETA` sentinel is still a game
    // concept. What crosses is a float or a negative number, so the engine
    // never learns that 65,535 meant anything in particular.
    progress.etaSeconds = record.etaSeconds == Game::NO_ETA ? -1.0f : static_cast<float>(record.etaSeconds);

    if (!_outFeedback.Add(progress))
    {
      break; // Bounded by the assert above; the break is what makes that true.
    }
  }
}

const char* ReplicatedWorldView::ReasonText(std::uint16_t _reasonCode) const
{
  // Straight through to the game's own diagnostic string, which is what makes a
  // local bounce and a server bounce read identically: the code took different
  // routes to get here and the words come from one place.
  return Game::OrderReasonText(static_cast<Game::OrderReason>(_reasonCode));
}

std::uint64_t ReplicatedWorldView::SchemaHash() const
{
  // The same number the server states, from the same string in the same build.
  // Asking the game for it rather than passing it in is what makes a content
  // mismatch detectable at all (ADR-004 §2).
  return Game::GameSchemaHash();
}

} // namespace Outpost
