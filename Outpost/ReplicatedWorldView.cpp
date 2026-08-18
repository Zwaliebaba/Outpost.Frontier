#include "pch.h"

#include "ReplicatedWorldView.h"

#include "Formation.h"
#include "OrderMessages.h"
#include "SchemaHash.h"
#include "ShipClass.h"
#include "Validate.h"

#include "EntityRecord.h"

#include <algorithm>
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

    InstanceRecord instance;
    // Local plane metres straight into render space: x east, y the cosmetic
    // height, z north (ADR-001 §3). The cosmetic height stays zero until the
    // per-class hover the same ADR reserves.
    instance.posWorld = DirectX::XMFLOAT3{ship.positionMetres.x, 0.0f, ship.positionMetres.y};
    instance.heading = ship.headingRadians;
    instance.teamColorId = 0;
    // Selection and LOD bias are the overlay's channels (S8). The stale flag
    // rides here so the marker the icon sheet draws has something to read.
    instance.selectionAndLodBias = ship.stale ? 1u : 0u;
    instance.classId = renderClass;
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
    entity.pickRadiusMetres = Game::ShipClass(static_cast<Game::HullClass>(ship.classId)).pickRadiusMetres;
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
