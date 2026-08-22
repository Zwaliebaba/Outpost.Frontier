#include "pch.h"

#include "ReplicatedWorldView.h"

#include "Eta.h"
#include "Formation.h"
#include "FleetSummary.h"
#include "OrderMessages.h"
#include "SchemaHash.h"
#include "SiteEpoch.h"
#include "ShipClass.h"
#include "StationMessages.h"
#include "SummaryMessages.h"
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
  _outOrder.queueMode = _intent.queued ? Game::QueueMode::Append : Game::QueueMode::Replace;

  /*
   * The intent carries **one** parameter, and which field it lands in is the
   * kind's business (ADR-014 §2b: the engine offers a number and a word, and
   * the game decides what the number means).
   *
   * For a Mine that number is the ore filter, so the formation stays the
   * default -- a Line around the worked cluster. The escorts in a mixed order
   * are still placed by the solve; what they cannot yet be given is a *chosen*
   * arrangement, because a surface that offers two dropdowns for one command is
   * E5's mining screen and not this seam's to invent.
   */
  if (_outOrder.kind == Game::OrderKind::Mine)
  {
    if (!Game::TryOreFilter(static_cast<std::uint8_t>(_intent.parameter), _outOrder.oreFilter))
    {
      // Refused rather than clamped to `Any`: a client that asked for an ore
      // this build has never heard of should not quietly be given a different
      // one. `InvalidFormation` is the parameter-is-wrong reason the enum has.
      _outReason = Game::OrderReason::InvalidFormation;
      return false;
    }
  }
  else
  {
    _outOrder.formation = static_cast<Game::FormationId>(_intent.parameter);
  }

  // What the order acts on, when it acts on something. The seam's sentinel and
  // the game's are different values on purpose -- `INVALID_ANCHOR` is the
  // engine's "no thing named" and `Game::INVALID_ID` is ours -- so the
  // translation is explicit here rather than a shared constant that would tie
  // the two vocabularies together (ADR-014).
  _outOrder.anchor = _intent.anchor == Neuron::INVALID_ANCHOR ? Game::INVALID_ID : static_cast<Game::AnchorId>(_intent.anchor);

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

/// ASCII uppercase into a fixed buffer, always terminated. The names it is fed
/// are the game's own English words, so no locale question arises.
void UpperCaseInto(const char* _text, char* _out, std::size_t _capacity) noexcept
{
  std::size_t i = 0;
  for (; _text[i] != '\0' && i + 1 < _capacity; ++i)
  {
    const char c = _text[i];
    _out[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
  }
  _out[i] = '\0';
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

/*
 * The order label, lifted out because two callers want it now: a placed order
 * fills it after solving a footprint, and a warp fills it *instead* of solving
 * one.
 */
void FillPreviewLabel(const Game::OrderSubmit& _order, OrderPreview& _outPreview)
{
  /*
   * `MOVE ▸ CLAW` -- uppercase, token-separated, the print's own spelling
   * (`tactical-hud.png`'s order label). Uppercased here rather than by the
   * drawing pass because the label is the game's sentence: the engine copies
   * the bytes and must not re-spell words it is not allowed to know.
   */
  char kindUpper[16] = {};
  char parameterUpper[16] = {};
  UpperCaseInto(Game::OrderKindName(_order.kind), kindUpper, sizeof(kindUpper));
  // The second token is whatever the kind *varies by*, not always a formation
  // -- `MINE - NEBULITE` is the sentence a mining ghost has to read, and a
  // label that said `MINE - LINE` would be naming the wrong choice back at the
  // player who just made one.
  UpperCaseInto(_order.kind == Game::OrderKind::Mine ? Game::OreFilterName(_order.oreFilter)
                                                    : Game::FormationName(_order.formation),
                parameterUpper, sizeof(parameterUpper));
  std::snprintf(_outPreview.label, sizeof(_outPreview.label), "%s \xE2\x96\xB8 %s", kindUpper, parameterUpper);
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
  m_validationMarks.clear();
  m_stationEntityId = Game::INVALID_SHIP_ID;
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
    // The protection bit and whatever joins it, carried across for the overlay
    // to draw. The engine gets the byte and not its meaning (ADR-014 4).
    entity.statusBits = ship.statusBits;
    _outScene.entities.push_back(entity);

    // Which of these is the station, for the context action to offer a verb on.
    // Recorded here because this loop already has the hull class in hand and is
    // the one place that sees every entity the frame will draw -- a structure
    // this build has no mesh for was skipped above, and an entity a player
    // cannot see must not be one they can act on.
    if (hull == Game::HullClass::Structure)
    {
      m_stationEntityId = ship.id;
      m_stationXCm = Neuron::MetresToCentimetres(ship.positionMetres.x);
      m_stationYCm = Neuron::MetresToCentimetres(ship.positionMetres.y);
    }

    // The same id again, for `ValidateOrder`. Filled here rather than in
    // `PreCheck` so that the ships an order may name are exactly the ships this
    // frame drew -- including the exclusions above, which is the point: a hull
    // with no mesh cannot be clicked, so it must not be orderable either.
    m_validationIds.push_back(ship.id);
    // And where it is, which is what a Dock or a Mine is judged on. Centimetres
    // because that is the wire's unit and the validator's: a view built in
    // metres would round differently from the order it is judging.
    m_validationMarks.push_back(Game::ShipMark{Neuron::MetresToCentimetres(ship.positionMetres.x),
                                               Neuron::MetresToCentimetres(ship.positionMetres.y), hull});

    renderClassCount = std::max(renderClassCount, static_cast<std::uint32_t>(renderClass) + 1u);
  }

  // Sorted so the opaque pass draws each class in one instanced run. The count
  // is the highest class actually present, not the mesh table's size: a scene
  // with only Interceptors needs one range, not nine.
  _outScene.SortByClass(renderClassCount);
}

/*
 * What this client knows, in the shape the shared validator wants.
 *
 * **Everything it can fill, it fills; everything it cannot, it leaves empty**,
 * and the validator's optional-field rules do the rest -- an absent
 * `oreHoldFreeLitres` means the hold check is the authority's alone, which
 * `Validate.cpp` calls the designed asymmetry rather than a hole. What must
 * *not* happen is the third case, and it is the one that shipped: a field left
 * empty that the client could have filled, turning a check the authority passes
 * into a refusal the client invents. `stationAnchor` was that field, and it
 * refused every Dock this side ever pre-checked.
 */
Game::ValidationView ReplicatedWorldView::MakeValidationView() const noexcept
{
  Game::ValidationView view;
  view.shipIds = m_validationIds;
  view.shipMarks = m_validationMarks;

  // The station is this grid's, and it is only nameable once one has actually
  // been drawn -- a `Welcome` naming an anchor whose structure has not arrived
  // in a snapshot yet is a station with no position to measure against.
  if (m_stationEntityId != Game::INVALID_SHIP_ID)
  {
    view.stationAnchor = m_desc.gridAnchor;
    view.stationXCm = m_stationXCm;
    view.stationYCm = m_stationYCm;
  }

  /*
   * The field this grid stands on, if the summaries have said so.
   *
   * `SiteStatus` is the public member of the family -- how eaten a field is, is
   * what anybody standing at it can see -- so its mere arrival is the client's
   * evidence that there is one, and its anchor is the only name a Mine needs.
   * Nothing arrives on a grid with no field, so the absence is the answer.
   */
  if (!m_siteStatus.empty())
  {
    view.siteAnchor = m_siteStatus.front().anchor;
  }

  /*
   * What a unit of each ore displaces, and the room each hull has left.
   *
   * Both halves of one subtraction, and **both are needed or neither counts**:
   * the validator's hold check asks for the smallest unit that matches the
   * filter and skips itself entirely when that is zero, so a view carrying the
   * free litres but not the unit volumes reads as "cannot answer" rather than
   * as "no room". That is not a rounding difference, it is the refusal never
   * firing -- which is what the gate caught, and why they are filled together
   * here rather than wherever each happened to be convenient.
   */
  if (m_desc.economy != nullptr)
  {
    for (std::uint8_t ore = 0; ore < Game::ORE_COUNT; ++ore)
    {
      view.oreUnitLitres[ore] = m_desc.economy->Ore(static_cast<Game::OreId>(ore)).unitVolumeLitres;
    }
  }

  FillHoldRoom(m_holdRoom);
  view.oreHoldFreeLitres = m_holdRoom;

  return view;
}

/*
 * How much ore room each drawn ship has left.
 *
 * Authored numbers on both sides of the subtraction: the hold's size and each
 * ore's unit volume both come from the parsed economy content, because a client
 * that hard-coded either would be a second copy of the balance file (ADR-024 7).
 *
 * Left **empty** when the content or the cargo summary is missing, and that is
 * deliberate rather than defensive: `ValidateOrder` reads an empty span as "the
 * caller cannot answer this", so the hold check becomes the authority's alone
 * and the player meets it as a bounce instead of as a locally invented refusal.
 * A ship the cargo summary does not mention is holding nothing, which is the
 * honest reading -- the summary lists what is aboard, so silence is an empty
 * hold rather than an unknown one.
 */
void ReplicatedWorldView::FillHoldRoom(std::vector<std::uint32_t>& _outFree) const
{
  _outFree.clear();
  if (m_desc.economy == nullptr)
  {
    return;
  }

  _outFree.reserve(m_validationMarks.size());
  for (std::size_t index = 0; index < m_validationMarks.size(); ++index)
  {
    const std::uint32_t capacity = m_desc.economy->Cargo(m_validationMarks[index].hullClass).oreHoldLitres;
    std::uint32_t used = 0;
    const Game::ShipId ship = m_validationIds[index];
    const auto row = std::find_if(m_cargo.begin(), m_cargo.end(),
                                  [&](const Game::CargoStatusRow& _row) { return _row.shipId == ship; });
    if (row != m_cargo.end())
    {
      for (std::uint8_t ore = 0; ore < Game::ORE_COUNT; ++ore)
      {
        used += row->oreUnits[ore] * m_desc.economy->Ore(static_cast<Game::OreId>(ore)).unitVolumeLitres;
      }
    }
    _outFree.push_back(used >= capacity ? 0u : capacity - used);
  }
}

Game::OrderVerdict ReplicatedWorldView::SelectionOnlyVerdict(Game::OrderKind _kind,
                                                             std::span<const std::uint16_t> _selectedIds) const noexcept
{
  Game::OrderSubmit order;
  order.kind = _kind;
  order.shipCount = static_cast<std::uint16_t>(std::min<std::size_t>(_selectedIds.size(), Game::MAX_SHIPS_PER_ORDER));
  for (std::uint16_t index = 0; index < order.shipCount; ++index)
  {
    order.shipIds[index] = _selectedIds[index];
  }

  const Game::ValidationView view = MakeValidationView();
  return Game::ValidateOrder(view, order);
}

OrderVerdict ReplicatedWorldView::PreCheck(const OrderIntent& _intent)
{
  Game::OrderSubmit order;
  Game::OrderReason reason = Game::OrderReason::Accepted;
  if (!MakeSubmit(_intent, order, reason))
  {
    return ToSeam(Game::OrderVerdict{false, reason}, _intent.orderSeq);
  }

  Game::ValidationView view = MakeValidationView();

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

  /*
   * A warp is not a place on this grid, and this is where that is said.
   *
   * Ships warp to **anchors, never to coordinates** (ADR-016 3), so the target
   * a warp carries is a destination id and the point beside it means nothing
   * here. Solving a formation at it would draw the fleet assembling wherever
   * the gesture landed while it was actually leaving the system -- and the more
   * carefully the footprint was solved, the more convincing the wrong answer
   * would look.
   *
   * The label still crosses (`WARP > LINE`) because the chrome draws it, and so
   * does the ETA: what the player loses is a ring, not the promise.
   */
  if (order.kind == Game::OrderKind::Warp)
  {
    _outPreview.onThisGrid = false;
    FillPreviewLabel(order, _outPreview);
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

  FillPreviewLabel(order, _outPreview);
}

bool ReplicatedWorldView::EncodeOrder(const OrderIntent& _intent, ByteWriter& _writer)
{
  Game::OrderSubmit order;
  Game::OrderReason reason = Game::OrderReason::Accepted;
  if (!MakeSubmit(_intent, order, reason))
  {
    return false; // Not sent, rather than sent malformed.
  }
  // The kind byte first: station commands share this stream (ADR-017 §8), and
  // nothing in either payload could tell a reader which one it is holding.
  return Game::WriteCommandKind(Game::CommandKind::Order, _writer) && Game::WriteOrderSubmit(order, _writer);
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
  // Move's parameter is a formation. A kind that takes no parameter answers
  // zero here rather than being special-cased at the client -- which is what
  // makes "an empty answer is legitimate" a real path rather than a line in a
  // comment.
  if (_kind == static_cast<std::uint16_t>(Game::OrderKind::Move))
  {
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

  /*
   * Stance's parameter is a posture. The kind itself is reserved -- nothing
   * validates or simulates it, and `OrderKindHasContent` keeps its verb greyed
   * -- but the context bar's `STANCE <value>` readout has to name the standing
   * posture and may not invent the words (ADR-014 §2b). What the client holds
   * is a chosen value for a future order, exactly the standing `FORMATION
   * LINE` already is before a Move is sent.
   */
  if (_kind == static_cast<std::uint16_t>(Game::OrderKind::Stance))
  {
    std::uint32_t count = 0;
    for (const Game::StanceId stance : Game::STANCE_IDS)
    {
      if (count >= _outOptions.size())
      {
        break;
      }
      _outOptions[count].parameter = static_cast<std::uint16_t>(stance);
      _outOptions[count].name = Game::StanceName(stance);
      ++count;
    }
    return count;
  }

  /*
   * And Mine's parameter is an ore (ADR-024 §4a) -- the first one that is not a
   * formation or a posture, which is the whole reason this function is a lookup
   * rather than a constant.
   *
   * `Any` is first because it is the default and the value a zeroed intent
   * carries, so the option the client shows before anyone has chosen is the one
   * the order would actually be sent with.
   */
  if (_kind == static_cast<std::uint16_t>(Game::OrderKind::Mine))
  {
    std::uint32_t count = 0;
    for (const Game::OreFilter filter : Game::ORE_FILTER_IDS)
    {
      if (count >= _outOptions.size())
      {
        break;
      }
      _outOptions[count].parameter = static_cast<std::uint16_t>(filter);
      _outOptions[count].name = Game::OreFilterName(filter);
      ++count;
    }
    return count;
  }

  return 0;
}

std::uint32_t ReplicatedWorldView::OrderKinds(std::span<const std::uint16_t> _selectedIds,
                                              std::span<OrderKindOption> _outKinds) const
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
    _outKinds[count].reasonCode = 0;

    /*
     * And then the part that is about *now* rather than about this build.
     *
     * Asked only of the kinds that name no destination, which today is Mine
     * alone: everything that decides a Mine is already on screen -- the field
     * the grid stands on, and what is in the selection -- so the row can say
     * before the gesture what the authority would say after it. A Move or a
     * Dock is judged partly on where it points, and pre-judging one without a
     * point would grey a verb the authority would have taken.
     *
     * The answer comes from `ValidateOrder` rather than from three checks
     * written here, which is the whole of the parity claim: the button greys
     * for the reason the bounce would have carried, in the same words, because
     * it is the same function (ADR-014 3).
     */
    /*
     * Warp is real, simulated and reachable -- **from a surface this build does
     * not have**.
     *
     * A warp names an anchor (ADR-016 3), and the two screens that let a player
     * pick one are the strategic map and the system view, neither of which is
     * built. Nothing in the tactical HUD can supply the field, so every warp it
     * could compose carries no destination and comes back `UnknownAnchor` --
     * which made the verb a live-looking button that could only ever bounce.
     *
     * Greyed with that same reason, so the row says now what the authority
     * would have said a round trip later. This is a statement about the
     * *surfaces this build has* rather than about the game, which is why it
     * lives in the composition root: it is the one place that knows both, and
     * the gate lifts by deletion the day a destination picker exists.
     */
    if (_outKinds[count].available && kind == Game::OrderKind::Warp)
    {
      _outKinds[count].available = false;
      _outKinds[count].reasonCode = static_cast<std::uint16_t>(Game::OrderReason::UnknownAnchor);
    }

    if (_outKinds[count].available && kind == Game::OrderKind::Mine)
    {
      if (_selectedIds.empty())
      {
        // No subject. Not a refusal the validator has a word for -- an order
        // naming nothing never reaches it -- so the verb greys silently, the
        // way it would for any command with nothing selected.
        _outKinds[count].available = false;
      }
      else if (const Game::OrderVerdict verdict = SelectionOnlyVerdict(kind, _selectedIds); !verdict.accepted)
      {
        _outKinds[count].available = false;
        _outKinds[count].reasonCode = static_cast<std::uint16_t>(verdict.reason);
      }
    }

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

    /*
     * Litres, summed across the wing's carrying hulls only.
     *
     * A ratio of totals rather than a mean of per-ship ratios, and the
     * difference shows on a mixed wing: three empty Miners beside one full
     * Hauler is a wing that is nearly full *by volume*, which is what the
     * player is deciding with, and about a fifth full by ship. The gauge is
     * about the cargo, so the cargo is what it counts.
     */
    std::uint64_t holdLitres = 0;
    std::uint64_t usedLitres = 0;
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

    /*
     * And what it is carrying, from the authored numbers and the summary.
     *
     * Both halves are content: how much a hull holds and what a unit of each
     * ore displaces are in `EconomyDef`, which is why this sum happens on this
     * side of the seam at all (ADR-024 §5c). A hull with no ore hold adds
     * nothing to either total and so cannot make a combat wing look like a
     * freight one.
     */
    if (m_desc.economy != nullptr && ship.classId < Game::HULL_CLASS_COUNT)
    {
      const std::uint32_t capacity = m_desc.economy->Cargo(static_cast<Game::HullClass>(ship.classId)).oreHoldLitres;
      if (capacity > 0)
      {
        wing.holdLitres += capacity;
        const auto row = std::find_if(m_cargo.begin(), m_cargo.end(),
                                      [&](const Game::CargoStatusRow& _row) { return _row.shipId == ship.id; });
        if (row != m_cargo.end())
        {
          for (std::uint8_t ore = 0; ore < Game::ORE_COUNT; ++ore)
          {
            wing.usedLitres +=
              static_cast<std::uint64_t>(row->oreUnits[ore]) * m_desc.economy->Ore(static_cast<Game::OreId>(ore)).unitVolumeLitres;
          }
        }
      }
    }

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

    // Capacity, not ships: a wing carries if anything in it has a hold.
    row.carriesCargo = wing.holdLitres > 0;
    row.cargoGauge = row.carriesCargo
                       ? static_cast<std::uint8_t>(std::min<std::uint64_t>(255u, wing.usedLitres * 255u / wing.holdLitres))
                       : 0u;
    ++rows;
  }
  return rows;
}

/*
 * What acting on this entity means (ADR-017 2).
 *
 * One verb today, and the shape is built for the ones after it: a station is
 * the natural home for trade, repair and missions, and each arrives here as
 * another kind rather than as another button in a row the print froze.
 *
 * Two conditions, and both are about the *game* rather than about the click.
 * The entity has to be this grid's station -- the anchor the `Welcome` named,
 * which is the only structure a client can address today -- and the player has
 * to have ships selected, because DOCK with an empty selection is a verb with
 * no subject and offering it would be offering a refusal.
 *
 * Deliberately not checked here: whether the fleet is *in range*. That is not a
 * condition on the action, it is what the approach chain exists to fix -- the
 * client flies the fleet to the perimeter and submits the Dock when they
 * arrive. Greying the action out at distance would remove the affordance
 * exactly where it is most useful.
 */
bool ReplicatedWorldView::ContextActionFor(std::uint16_t _entityId, std::span<const std::uint16_t> _selectedIds,
                                           ContextAction& _outAction) const
{
  _outAction = ContextAction{};
  if (_selectedIds.empty() || m_desc.gridAnchor == Game::INVALID_ID)
  {
    return false;
  }
  if (m_stationEntityId == Game::INVALID_SHIP_ID || _entityId != m_stationEntityId)
  {
    return false;
  }

  // A selection holding only the station is asking the station to dock at
  // itself, which is a refusal dressed as an affordance.
  const bool anyShip =
    std::any_of(_selectedIds.begin(), _selectedIds.end(), [&](std::uint16_t _id) { return _id != m_stationEntityId; });
  if (!anyShip)
  {
    return false;
  }

  _outAction.kind = static_cast<std::uint16_t>(Game::OrderKind::Dock);
  _outAction.label = "DOCK";
  _outAction.anchor = static_cast<std::uint16_t>(m_desc.gridAnchor);
  _outAction.available = true;
  return true;
}

/*
 * The 1 Hz family, decoded (ADR-016 6, ADR-017 1).
 *
 * **A frame is a complete statement, so the block list is replaced wholesale.**
 * The alternative -- merging each record into what was there -- keeps a station
 * whose last ship undocked, because the writer stops sending a roster for a
 * place with none of your ships in it and silence is indistinguishable from
 * "unchanged". Replacing means the panel can only ever be one second stale,
 * never permanently wrong.
 *
 * **The blocks come from the fleet summaries and the ship lists from the
 * rosters**, which is a split worth stating: the summary rows are always
 * written and say *where* and *how many*, while a roster is one station's
 * detail and is dropped first when the frame runs out of room. So a station
 * whose roster did not fit still draws a correct block with the right count,
 * and only the hangar behind it is empty -- the honest failure, and the one the
 * paging ADR-016 6 owes will remove.
 *
 * Staged and committed at the end rather than applied as it reads, so the
 * answer does not depend on the order the writer happened to put the records
 * in. Two builds disagreeing about that would disagree about a panel rather
 * than fail a handshake, which is the worst kind of version skew.
 */
bool ReplicatedWorldView::ApplySummary(std::span<const std::uint8_t> _payload)
{
  ByteReader reader{_payload};
  std::uint8_t records = 0;
  if (!Game::ReadSummaryFrame(reader, records))
  {
    ++m_rejectedSummaries;
    return false;
  }

  std::vector<FleetPlace> staged;
  std::vector<FleetPlace> stagedRosters;
  std::vector<Game::SiteStatusRow> stagedSites;
  std::vector<BayHolding> stagedBays;
  std::vector<Game::RefineryStatusRow> stagedRefineries;
  m_decodedCargo.clear();
  bool sawCargo = false;

  /*
   * Every kind is handled, and there is no `default:` on purpose.
   *
   * The frame carries no length prefix -- `SummaryMessages.h` says so and gives
   * the reason -- so a body this switch does not read is not a record skipped,
   * it is every record after it misparsed. That is exactly what happened when
   * E3 added three kinds to a decoder written when there were two: the client
   * quietly dropped whole frames, and took the docked blocks and their toasts
   * with them. It went unnoticed because the only frames the starting world
   * sends carry the two old kinds.
   *
   * The pragma is the guard that failure earned. C4062 is a level-4 warning and
   * this project builds at /W3, so an unhandled enumerator was invisible; here
   * it is an error, which makes the *next* kind added to the family a build
   * break in the one file that has to know about it.
   */
#pragma warning(push)
#pragma warning(1 : 4062)
  for (std::uint8_t index = 0; index < records; ++index)
  {
    Game::SummaryKind kind{};
    if (!Game::ReadSummaryRecord(reader, kind))
    {
      // An unknown kind is a schema disagreement the handshake was supposed to
      // have refused, and there is no way to skip a body whose length only its
      // own reader knows -- so the frame stops here rather than guessing.
      ++m_rejectedSummaries;
      return false;
    }

    switch (kind)
    {
    case Game::SummaryKind::FleetSummaries:
    {
      m_decodedSummaries.clear();
      if (!Game::ReadFleetSummaries(reader, m_decodedSummaries))
      {
        ++m_rejectedSummaries;
        return false;
      }
      for (const Game::FleetSummary& row : m_decodedSummaries)
      {
        // **Every** row now, where T2 kept only the docked ones. The panel this
        // feeds is about ships the scene cannot show, and a fleet on a grid the
        // player is not watching is as invisible as a docked one -- which is
        // U3b's whole observation. Which of them the viewed grid already draws
        // is a question for the build, not for the decode.
        staged.push_back(FleetPlace{row.anchor, row.state, row.etaSeconds, row.shipCount, {}});
      }
      break;
    }

    case Game::SummaryKind::StationRoster:
    {
      Game::AnchorId station = Game::INVALID_ID;
      m_decodedRoster.clear();
      if (!Game::ReadStationRoster(reader, station, m_decodedRoster))
      {
        ++m_rejectedSummaries;
        return false;
      }
      stagedRosters.push_back(
        FleetPlace{station, Game::FleetState::Docked, Game::FLEET_ETA_NONE, 0, std::move(m_decodedRoster)});
      m_decodedRoster.clear(); // Moved from, and reused by the next record.
      break;
    }

    case Game::SummaryKind::SiteStatus:
    {
      Game::SiteStatusRow row;
      if (!Game::ReadSiteStatus(reader, row))
      {
        ++m_rejectedSummaries;
        return false;
      }
      stagedSites.push_back(row);
      break;
    }

    case Game::SummaryKind::CargoStatus:
    {
      // At most one per frame, but the loop does not assume it: a second would
      // replace the first, which is the same "a frame is a complete statement"
      // rule the block list follows.
      m_decodedCargo.clear();
      if (!Game::ReadCargoStatus(reader, m_decodedCargo))
      {
        ++m_rejectedSummaries;
        return false;
      }
      sawCargo = true;
      break;
    }

    case Game::SummaryKind::BayStatus:
    {
      BayHolding holding;
      if (!Game::ReadBayStatus(reader, holding.station, holding.oreUnits, holding.alloyUnits))
      {
        ++m_rejectedSummaries;
        return false;
      }
      stagedBays.push_back(holding);
      break;
    }

    case Game::SummaryKind::RefineryStatus:
    {
      // The guard did its job: E4b added this kind and the build refused to
      // compile until the decoder named it. That is exactly the failure the
      // pragma above exists to convert from a silent misparse into an error.
      Game::RefineryStatusRow row;
      if (!Game::ReadRefineryStatus(reader, row))
      {
        ++m_rejectedSummaries;
        return false;
      }
      stagedRefineries.push_back(row);
      break;
    }
    }
  }
#pragma warning(pop)

  for (FleetPlace& block : staged)
  {
    const auto match = std::find_if(stagedRosters.begin(), stagedRosters.end(),
                                    [&](const FleetPlace& _entry) { return _entry.anchor == block.anchor; });
    if (match != stagedRosters.end())
    {
      block.docked = std::move(match->docked);
    }
  }

  // By anchor, so the panel's order is the universe's rather than the order the
  // records happened to arrive in: a list that reshuffles between frames is a
  // list the player cannot point at.
  std::sort(staged.begin(), staged.end(),
            [](const FleetPlace& _left, const FleetPlace& _right) { return _left.anchor < _right.anchor; });

  NoteRosterChanges(staged);
  m_places = std::move(staged);

  // Wholesale, like the blocks: a kind the frame did not mention is a kind with
  // nothing to say, and keeping the last one would leave a hold reading full
  // after it emptied or a field reading eaten after the epoch re-laid it.
  m_siteStatus = std::move(stagedSites);
  m_bays = std::move(stagedBays);
  m_refineries = std::move(stagedRefineries);
  m_cargo = sawCargo ? std::move(m_decodedCargo) : std::vector<Game::CargoStatusRow>{};
  m_decodedCargo.clear();

  m_haveSummary = true;
  return true;
}

/*
 * What changed, said out loud (ADR-017 2 -- "toasts on dock and undock
 * complete").
 *
 * A dock finishing is an *event*, and the only evidence of it the client has is
 * that a count went up: docked ships despawn, so nothing in the scene marks the
 * moment. Comparing the two statements is therefore not a stand-in for a wire
 * message that ought to exist -- the roster is the authority's own record of
 * the fact, and a separate event message would be a second copy of it to keep
 * in step.
 *
 * **The first summary of a session says nothing**, and that is the load-bearing
 * line. Everything already docked when a client joins would otherwise arrive as
 * a stack of "docking complete" toasts about things that happened before the
 * player was watching -- a state reported as an event, which is exactly the
 * mistake this function exists to avoid making in the other direction.
 */
void ReplicatedWorldView::NoteRosterChanges(const std::vector<FleetPlace>& _next)
{
  if (!m_haveSummary)
  {
    return;
  }

  /*
   * Docked rows on both sides of the comparison, and nothing else.
   *
   * The list grew the other two states in U3b, and this function must not grow
   * with it: a fleet that warps away changes its `OnGrid` count at one anchor
   * and its `InTransit` row at another, and a delta that counted those would
   * announce "undock complete" at a station nobody undocked from. Docking is
   * the event this raises; the others are movement, which the player is already
   * watching happen.
   */
  const auto countAt = [](const std::vector<FleetPlace>& _blocks, Game::AnchorId _anchor) -> std::uint16_t {
    const auto found = std::find_if(_blocks.begin(), _blocks.end(), [&](const FleetPlace& _entry) {
      return _entry.anchor == _anchor && _entry.state == Game::FleetState::Docked;
    });
    return found == _blocks.end() ? std::uint16_t{0} : found->shipCount;
  };

  const auto raise = [&](Game::AnchorId _anchor, std::uint16_t _delta, bool _docked) {
    char body[96] = {};
    const char* station = AnchorNameFor(_anchor);
    std::snprintf(body, sizeof(body), "%u SHIP%s %s %s", _delta, _delta == 1 ? "" : "S", _docked ? "AT" : "FROM",
                  station != nullptr ? station : "STATION");
    PendingNotice notice;
    // Keyed on the place as well as the kind: two stations receiving fleets in
    // the same six seconds is two things happening, and one key would fold the
    // second into the first as though it were a repeat.
    notice.code = (static_cast<std::uint32_t>(_anchor) << 1) | (_docked ? 0u : 1u);
    notice.title = _docked ? "DOCKING COMPLETE" : "UNDOCK COMPLETE";
    notice.body = body;
    m_notices.push_back(std::move(notice));
  };

  for (const FleetPlace& block : _next)
  {
    if (block.state != Game::FleetState::Docked)
    {
      continue;
    }
    const std::uint16_t before = countAt(m_places, block.anchor);
    if (block.shipCount > before)
    {
      raise(block.anchor, static_cast<std::uint16_t>(block.shipCount - before), true);
    }
    else if (block.shipCount < before)
    {
      raise(block.anchor, static_cast<std::uint16_t>(before - block.shipCount), false);
    }
  }

  // A station that left the list entirely: its last ships undocked, and the
  // loop above cannot see it because there is no row left to compare against.
  for (const FleetPlace& block : m_places)
  {
    if (block.state != Game::FleetState::Docked)
    {
      continue;
    }
    const auto still = std::find_if(_next.begin(), _next.end(), [&](const FleetPlace& _entry) {
      return _entry.anchor == block.anchor && _entry.state == Game::FleetState::Docked;
    });
    if (still == _next.end() && block.shipCount > 0)
    {
      raise(block.anchor, block.shipCount, false);
    }
  }
}

const char* ReplicatedWorldView::AnchorNameFor(Game::AnchorId _anchor) const
{
  const auto found = std::find_if(m_desc.anchorNames.begin(), m_desc.anchorNames.end(),
                                  [&](const AnchorName& _entry) { return _entry.anchor == _anchor; });
  return found == m_desc.anchorNames.end() ? nullptr : found->name.c_str();
}

/*
 * The panel's rows (ADR-017 1).
 *
 * Three fields per block and nothing else: what the place is called, how many
 * of the player's ships are in it, and the number a click hands back. The
 * engine never learns that the place is a station, that being there is called
 * docked, or that the button beside it will one day open a hangar.
 *
 * A block with no name still draws -- the count is the true part and the name
 * is content that may simply be missing -- for the same reason the roster draws
 * a "?" for a wing it cannot name rather than dropping the row.
 */
std::uint32_t ReplicatedWorldView::BuildLocationBlocks(std::span<LocationBlock> _outBlocks) const
{
  std::uint32_t written = 0;
  for (const FleetPlace& block : m_places)
  {
    if (written >= _outBlocks.size())
    {
      break;
    }
    if (block.shipCount == 0)
    {
      continue; // An empty place is not a place the player has ships in.
    }

    /*
     * **Every place, including the one being watched.** The panel does not draw
     * that row -- those ships are on screen as hulls and listing them again
     * would be one fleet counted twice on one HUD -- but *not drawing* it is a
     * presentation decision, and it belongs where the drawing is.
     *
     * It matters because a second reader wants the row the panel throws away:
     * auto-follow asks "does the grid I am watching still hold anything of
     * mine", and a list that had already silently removed the answer could not
     * be asked. Filtering here would have been the game deciding what the
     * client is allowed to notice.
     */
    LocationBlock& out = _outBlocks[written];
    out.name = AnchorNameFor(block.anchor);
    out.shipCount = block.shipCount;
    out.anchor = static_cast<std::uint16_t>(block.anchor);
    out.stateLabel = Game::FleetStateName(block.state);
    // The one thing only this side can answer: are these the hulls the scene
    // just drew? A fleet standing on the grid being watched is; everything
    // else -- docked here, on another grid, mid-crossing -- is not.
    out.inScene = block.state == Game::FleetState::OnGrid && block.anchor == m_view.Grid();
    out.etaSeconds = block.etaSeconds == Game::FLEET_ETA_NONE ? -1.0f : static_cast<float>(block.etaSeconds);

    /*
     * A button only where there is somewhere to go.
     *
     * Docked opens the hangar (T3); a fleet on another grid offers VIEW, which
     * is the switch U3b's wire half already serves and `MayView` already gates.
     * A crossing offers nothing, and that is the honest answer rather than a
     * greyed control: a fleet in no world has no surface to open, and the ETA
     * beside it is the only thing anybody can do about it.
     */
    out.buttonLabel = block.state == Game::FleetState::Docked   ? "STATION"
                      : block.state == Game::FleetState::OnGrid ? "VIEW"
                                                                : nullptr;
    ++written;
  }
  return written;
}

std::uint16_t ReplicatedWorldView::DockedCountAt(Game::AnchorId _anchor) const noexcept
{
  const auto found = std::find_if(m_places.begin(), m_places.end(), [&](const FleetPlace& _entry) {
    return _entry.anchor == _anchor && _entry.state == Game::FleetState::Docked;
  });
  return found == m_places.end() ? std::uint16_t{0} : found->shipCount;
}

/*
 * How eaten the field under the fleet is (ADR-024 §3d, ADR-016 §6).
 *
 * The public member of the summary family: how much of a field is left is what
 * anybody standing at it can see, so this needs no ownership check and answers
 * for whichever grid the client is watching.
 *
 * **The staleness check is the client doing arithmetic rather than trusting a
 * flag, and that is the point of it.** A site re-forms on a schedule
 * (`regenSeconds`, staggered per anchor), and `SiteEpochIndex` is a pure
 * function of the tick and the anchor -- so this side can work out which epoch
 * it is in *right now* and compare it against the epoch the status describes.
 * A status from before the field re-formed is not merely old, it describes rock
 * that is not there any more, and drawing it would be drawing yesterday's
 * field with today's confidence.
 */
bool ReplicatedWorldView::BuildFieldReadout(Neuron::FieldReadout& _outReadout) const
{
  _outReadout = Neuron::FieldReadout{};
  if (m_siteStatus.empty() || m_desc.economy == nullptr)
  {
    return false;
  }

  // The grid being watched, if the summaries mentioned it. A field elsewhere is
  // real and is not what this readout is about.
  const Game::AnchorId viewing = m_view.Grid();
  const auto row = std::find_if(m_siteStatus.begin(), m_siteStatus.end(),
                                [&](const Game::SiteStatusRow& _row) { return _row.anchor == viewing; });
  if (row == m_siteStatus.end())
  {
    return false;
  }

  const std::uint32_t bars = std::min<std::uint32_t>(row->clusterCount, Neuron::MAX_FIELD_BARS);
  for (std::uint32_t index = 0; index < bars; ++index)
  {
    _outReadout.fullPct[index] = row->clusterFullPct[index];
  }
  _outReadout.barCount = bars;

  /*
   * Ticks rather than seconds, because the epoch is defined in ticks and the
   * conversion is where a flat number stops being flat -- the same reason
   * `GATE_JUMP_TICKS` is a tick count (ADR-016 §10).
   */
  const std::uint32_t epochTicks =
    static_cast<std::uint32_t>(m_desc.economy->sites.regenSeconds) * Game::TICKS_PER_SECOND;
  if (epochTicks > 0)
  {
    const std::uint32_t nowEpoch = Game::SiteEpochIndex(m_view.LatestTick(), viewing, epochTicks);
    if (nowEpoch != row->epoch)
    {
      _outReadout.staleLabel = "LAST EPOCH";
    }
  }
  return true;
}

std::uint32_t ReplicatedWorldView::PollNotices(std::span<Notice> _outNotices)
{
  // Drained, and the overflow is dropped rather than held: a notice that waited
  // a frame would arrive after the thing it describes had stopped being news,
  // and the seam promises "since you last asked" rather than "eventually".
  m_noticesHandedOver = std::move(m_notices);
  m_notices.clear();

  std::uint32_t written = 0;
  for (const PendingNotice& notice : m_noticesHandedOver)
  {
    if (written >= _outNotices.size())
    {
      break;
    }
    _outNotices[written] = Notice{notice.code, notice.title, notice.body.c_str()};
    ++written;
  }
  return written;
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
