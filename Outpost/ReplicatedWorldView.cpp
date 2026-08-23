#include "pch.h"

#include "ReplicatedWorldView.h"

#include "Eta.h"
#include "Formation.h"
#include "FleetSummary.h"
#include "OrderMessages.h"
#include "SchemaHash.h"
#include "SiteEpoch.h"
#include "SiteField.h"
#include "UniverseRoute.h"
#include "Transfer.h"
#include "ShipClass.h"
#include "Snapshot.h"
#include "StationMessages.h"
#include "SummaryMessages.h"
#include "Validate.h"

#include "EntityRecord.h"
#include "Log.h"
#include "SignalLamp.h"
#include "UploadBudget.h"

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
[[nodiscard]] bool MakeSubmit(const OrderIntent& _intent, Game::AnchorId _stationAnchor,
                              Game::OrderSubmit& _outOrder, Game::OrderReason& _outReason) noexcept
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

  /*
   * A Dock that names no anchor means **this grid's station**, and filling it
   * here is what lets the command row issue one without a gesture.
   *
   * Not a guess and not a convenience: `ValidationView::stationAnchor` is
   * singular because "a grid is anchored on one thing, so there is at most one
   * station to dock at". Pointing at it was never how the player chose *which*
   * station -- there is no which -- it was only how the anchor reached the
   * order. So the grid supplies it, and a gesture that named the same station
   * still works unchanged.
   *
   * Left `INVALID_ID` on a grid with no station, which `ValidateOrder` refuses
   * `UnknownStation` -- the same answer the pre-check greys the button with.
   */
  if (_outOrder.kind == Game::OrderKind::Dock && _outOrder.anchor == Game::INVALID_ID)
  {
    _outOrder.anchor = _stationAnchor;
  }

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

  /*
   * And the word for what the fleet will be *doing* once the legs are spent,
   * for the kinds that have one -- `MINING` (ADR-024 4b).
   *
   * Written here rather than at the moment the group starts working, because
   * there is no such moment on this side: the client learns a group has run out
   * of legs from the order state in a snapshot, and the order state carries no
   * kind. The preview is the one place both the kind and a buffer the ghost
   * will keep are in the same room, so the word is filled in advance and the
   * client reads it when the numbers say it applies.
   *
   * Left empty for every other kind, which is what tells the client that "no
   * leg left" means "finishing" rather than "working".
   */
  if (const char* working = Game::OrderWorkingName(_order.kind); working != nullptr)
  {
    UpperCaseInto(working, _outPreview.workingLabel, sizeof(_outPreview.workingLabel));
  }
}

} // namespace

ReplicatedWorldView::ReplicatedWorldView(Desc _desc)
  : m_desc(std::move(_desc))
{
  /*
   * The one line that makes a minted wing name safe to hand out as a pointer.
   *
   * `RosterRow::name` and `OrderOption::name` are borrowed `const char*` that
   * the seam says stay valid for the view's lifetime, and a short call sign
   * lives *inside* its `std::string` -- so a `push_back` that reallocated would
   * move every existing one and leave the words already on screen pointing at
   * freed storage. It would not even need two frames to bite: the options are
   * asked at the top of a frame and drawn at the bottom, and `BuildScene` mints
   * in between.
   *
   * Reserving the ceiling removes the reallocation rather than racing it, and
   * `EnsureWingName` refuses at capacity rather than trusting the arithmetic
   * that says it cannot be reached -- so the invariant is enforced where it is
   * relied on rather than argued for somewhere else.
   */
  m_playerWingNames.reserve(Neuron::MAX_ROSTER_ROWS);

  /*
   * The names the player chose in an earlier session (ADR-012 §3, N2).
   *
   * Applied here rather than by the caller because the two invariants above are
   * this class's: an entry past the reserve would move the words already handed
   * out, and an entry past the row cap would name a wing no roster can draw. A
   * hand-edited settings file is exactly where a list too long for either comes
   * from, so both are enforced against it rather than assumed of it.
   *
   * Silent about what it drops, and deliberately. `LoadAppConfig` has already
   * refused a malformed file loudly; what reaches here is well-formed and merely
   * too long, and the honest outcome for that is the first `MAX_ROSTER_ROWS`
   * names taken in file order, which is `wings` sorted by number.
   */
  for (const std::pair<Game::WingId, std::string>& saved : m_desc.savedWingNames)
  {
    if (saved.first == Game::INVALID_WING_ID || saved.second.empty())
    {
      continue; // Wing 0 draws no row, and a row with no word on it cannot be pointed at.
    }

    // One word per wing. The parser refuses a repeated id, so this is about a
    // caller composing its own list rather than about the file.
    const auto already = std::find_if(m_playerWingNames.begin(), m_playerWingNames.end(),
                                      [&saved](const std::pair<Game::WingId, std::string>& _entry) { return _entry.first == saved.first; });
    if (already != m_playerWingNames.end())
    {
      continue;
    }

    if (m_playerWingNames.size() == m_playerWingNames.capacity())
    {
      break; // The pointer-stability reserve, and it is absolute: no entry is worth moving the words on screen.
    }

    /*
     * The row cap costs a `continue` rather than a `break`, and the difference
     * is a rename.
     *
     * An entry naming a wing the content already named adds a word to an
     * existing row and no row at all, so it is legal at the cap -- and stopping
     * at the first entry the cap refused would drop the renames sitting behind
     * it in the list. Only an entry that would create a row the roster cannot
     * draw is skipped.
     */
    if (saved.first >= m_desc.wingNames.size() && NamedWingCount() >= Neuron::MAX_ROSTER_ROWS)
    {
      continue;
    }
    m_playerWingNames.emplace_back(saved.first, saved.second);
  }

  /*
   * A call sign the player is already using is struck off the pool.
   *
   * `spareWingNames` is consumed in order and never returned, so a word handed
   * to wing 9 last session must not be handed to wing 10 in this one -- that
   * would put one word on two things the player has told apart, which is the
   * failure `Desc` names. Restoring the names without also spending them would
   * do exactly that on the first new wing after a restart.
   */
  for (const std::pair<Game::WingId, std::string>& taken : m_playerWingNames)
  {
    const auto spare = std::find(m_desc.spareWingNames.begin(), m_desc.spareWingNames.end(), taken.second);
    if (spare != m_desc.spareWingNames.end())
    {
      m_desc.spareWingNames.erase(spare);
    }
  }

  /*
   * And the strategic map's graph, once, here (ADR-018 D14).
   *
   * In the constructor rather than on first use because `MapTopology` hands out
   * spans into these vectors and promises they stay valid for the session --
   * building lazily would mean the first `BuildMapTopology` could invalidate a
   * span an earlier one had handed over, which is a bug that needs two visits to
   * the surface to show itself.
   *
   * A client with no universe builds nothing and reports nothing, and the screen
   * opens on an empty viewport.
   */
  BuildMapGraph();

  /*
   * And the configured grid's reachable list, before any frame lands.
   *
   * `CurrentGrid` falls back to `Desc::gridAnchor`, so a pre-check asked in the
   * first second of a session is judged against the same list every later one
   * is -- rather than against an empty one, which reads as "nowhere is
   * reachable" and refuses every warp.
   */
  RefreshReachable();
}

bool ReplicatedWorldView::ApplyFrame(const Neuron::ReplicatedFrame& _frame)
{
  /*
   * Wiring, and the shape of it is the point (ADR-022 §1).
   *
   * The engine did the reassembly, held the baseline and decided the tick was
   * worth applying; the game is handed the finished picture and the one part of
   * it that is still its own -- the tail. Everything this function used to do
   * with a `ByteReader` now happens one library out, which is what "the session
   * host owns interest and delta" means at the client end.
   */
  if (!m_view.ApplyFrame(_frame.tick, static_cast<Game::AnchorId>(_frame.gridId), _frame.culledCount, _frame.entities,
                         _frame.tail))
  {
    // A malformed tail. Counted rather than logged per occurrence: on a lossy
    // link this would be the noisiest line in the file, and the number is what
    // actually says whether it is happening.
    ++m_rejectedSnapshots;
    return false;
  }

  /*
   * And the grid's own facts, when the grid has changed (U4).
   *
   * Here rather than in `MakeValidationView`, which is `const noexcept` and
   * hands out a *span* into this vector -- rebuilding it there would be
   * allocating inside a `noexcept` function and handing back a span into
   * storage the same call had just moved.
   */
  RefreshReachable();
  return true;
}

void ReplicatedWorldView::BuildScene(double _renderTick, RenderScene& _outScene)
{
  m_view.SampleAt(_renderTick, m_sampled);

  _outScene.Clear();
  _outScene.instances.reserve(m_sampled.size());
  _outScene.entities.reserve(m_sampled.size());

  /*
   * And how many this grid holds that were not sent (ADR-022 §5d, U3d-c).
   *
   * From the newest frame's header rather than from the sample: the sample is
   * interpolated between two ticks and a count is not a quantity to blend --
   * halfway between "9 hidden" and "11 hidden" is a number the authority never
   * stated. `ReplicatedView::CulledCount` reports the latest, which is the same
   * choice `statusBits` makes one field over and for the same reason.
   *
   * This is the last step of U3d: the count has reached the client since
   * U3d-b and stopped there, which the build order recorded as the whole of
   * what U3d-c had left.
   */
  _outScene.culledCount = m_view.CulledCount();
  m_validationIds.clear();
  m_validationMarks.clear();
  m_stationEntityId = Game::INVALID_SHIP_ID;
  m_gateEntityId = Game::INVALID_SHIP_ID;
  m_validationIds.reserve(m_sampled.size());

  std::uint32_t renderClassCount = 0;
  for (const Game::ReplicatedShip& ship : m_sampled)
  {
    /*
     * Before the exclusions, and it is the roster this is for rather than the
     * scene: a wing whose number arrived without a name gets one, so ships the
     * player grouped in an earlier session come back as a row instead of
     * silently leaving the roster (see `EnsureWingName`). Above the mesh check
     * because a hull this build cannot draw is still a hull in a wing.
     */
    (void)EnsureWingName(ship.wing);

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
    // Where this hull is in its own blink cycle (ADR-006 §6a). Hashed from the
    // ship's id and nothing else, so it is the same every frame for as long as
    // the ship lives -- a phase taken from the position would drift as the ship
    // moved and the strobe would speed up and slow down with it.
    instance.lampPhaseTurns = Neuron::LampPhaseTurns(ship.id);
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

    /*
     * And which of them is the gate (U4's client half).
     *
     * The station's twin, in the same loop and for the same two reasons: it has
     * the hull class in hand, and an entity a player cannot see must not be one
     * the client measures a jump against. `ValidationView::gateXCm` is exactly
     * that measurement -- "is every member inside the jump radius of the
     * structure" -- so a client with no gate on screen fills no `jumpAnchor`
     * and lets the authority answer, which is the posture every optional field
     * on that view already takes.
     */
    if (hull == Game::HullClass::Gate)
    {
      m_gateEntityId = ship.id;
      m_gateXCm = Neuron::MetresToCentimetres(ship.positionMetres.x);
      m_gateYCm = Neuron::MetresToCentimetres(ship.positionMetres.y);
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
Game::AnchorId ReplicatedWorldView::StationAnchor() const noexcept
{
  // Only nameable once a station has actually been drawn -- a `Welcome` naming
  // an anchor whose structure has not arrived in a snapshot yet is a station
  // with no position to measure against. One function because the validation
  // view, the submit and the row's availability must all mean the same station
  // or none.
  return m_stationEntityId != Game::INVALID_SHIP_ID ? m_desc.gridAnchor : Game::INVALID_ID;
}

Game::ValidationView ReplicatedWorldView::MakeValidationView() const noexcept
{
  Game::ValidationView view;
  view.shipIds = m_validationIds;
  view.shipMarks = m_validationMarks;

  if (StationAnchor() != Game::INVALID_ID)
  {
    view.stationAnchor = StationAnchor();
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
   * Where a `Warp` may go, and which of those is a jump (U4's client half).
   *
   * **A `Warp` had never been pre-checkable on this side.** `reachableAnchors`
   * and `jumpAnchor` were the two fields nothing here filled, so every warp and
   * every jump reached the authority to be refused there -- and ADR-014 §3's
   * whole claim is that the two halves answer alike. They are filled from the
   * *same pure function* the registry fills its half with (`ReachableAnchors`),
   * which is what makes that a fact rather than a hope: not two lists that
   * agree, one list asked twice.
   *
   * `jumpAnchor` is gated on having *seen* the gate, exactly as `stationAnchor`
   * is gated on having seen the station, and the reason is the field beside it:
   * a jump is judged on where the fleet is standing relative to the structure,
   * so naming the destination without a position to measure against would be
   * inventing the measurement. A client that cannot see the gate fills neither
   * and the authority answers alone.
   */
  view.reachableAnchors = m_reachable;
  if (m_gateEntityId != Game::INVALID_SHIP_ID && m_desc.universe != nullptr)
  {
    const Game::AnchorId paired = m_desc.universe->PairedGateAnchor(CurrentGrid());
    if (paired != Game::INVALID_ID)
    {
      view.jumpAnchor = paired;
      view.gateXCm = m_gateXCm;
      view.gateYCm = m_gateYCm;
    }
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
                                                             std::span<const Neuron::EntityId> _selectedIds) const noexcept
{
  Game::OrderSubmit order;
  order.kind = _kind;
  order.shipCount = static_cast<std::uint16_t>(std::min<std::size_t>(_selectedIds.size(), Game::MAX_SHIPS_PER_ORDER));
  for (std::uint16_t index = 0; index < order.shipCount; ++index)
  {
    order.shipIds[index] = _selectedIds[index];
  }

  const Game::ValidationView view = MakeValidationView();

  // The same defaulting `MakeSubmit` does, for the same reason: the row is
  // asking "would this be taken *right now*", and an answer computed against a
  // command missing the anchor the send will carry is an answer about a
  // different command. Without it a Dock greys `UnknownStation` from anywhere.
  if (_kind == Game::OrderKind::Dock)
  {
    order.anchor = view.stationAnchor;
  }

  return Game::ValidateOrder(view, order);
}

OrderVerdict ReplicatedWorldView::PreCheck(const OrderIntent& _intent)
{
  Game::OrderSubmit order;
  Game::OrderReason reason = Game::OrderReason::Accepted;
  if (!MakeSubmit(_intent, StationAnchor(), order, reason))
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
  if (!MakeSubmit(_intent, StationAnchor(), order, reason) || order.shipCount == 0)
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
  if (!MakeSubmit(_intent, StationAnchor(), order, reason))
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

std::uint32_t ReplicatedWorldView::OrderKinds(std::span<const Neuron::EntityId> _selectedIds,
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
    _outKinds[count].namesDestination = Game::OrderKindNamesDestination(kind);
    _outKinds[count].reasonCode = 0;

    /*
     * And then the part that is about *now* rather than about this build.
     *
     * Asked of every kind that names no destination, because everything that
     * decides one is already on screen -- so the row can say before the gesture
     * what the authority would say after it. A Move or a Warp is judged partly
     * on where it points, and pre-judging one without a point would grey a verb
     * the authority would have taken.
     *
     * **Dock belongs here and did not used to.** It reads like a targeted verb
     * because a player points at a station to issue one, but the validator
     * never consults where they pointed: it asks whether the named anchor is
     * this grid's, and a grid is anchored on one thing. The gesture was
     * supplying the anchor, which `MakeSubmit` now takes from the grid.
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

    /*
     * And the general form of the same idea, which arrived with T3: a kind that
     * *names no destination* can be judged from the selection alone, because
     * everything that decides it is already on screen. The row reads the bool
     * the game set and never keys on a kind number -- which is what stops this
     * gate needing a new branch every time a verb is added.
     */
    if (_outKinds[count].available && !_outKinds[count].namesDestination)
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

std::uint32_t ReplicatedWorldView::BuildRoster(std::span<const Neuron::EntityId> _selectedIds,
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
    /*
     * **Only this commander's**, which is a fix rather than a refinement (T3).
     *
     * A wing number is a byte every commander numbers from one, so a hostile
     * fleet flying their wing 3 on this grid was being counted into *this*
     * player's row 3 -- inflating its count and dragging its gauges towards a
     * fleet they do not command. It needed two commanders on one grid to show,
     * which U3c made possible and no gate had built.
     *
     * The bits that make the distinction are ADR-022 §8b's, which arrived after
     * this function did: before them there was no way to ask, and the code that
     * could not ask read like code that had decided not to.
     */
    if (Game::RelationshipFrom(ship.statusBits) != Game::Relationship::Own)
    {
      continue;
    }

    /*
     * Wing zero is `INVALID_WING_ID`, and since T3 it is a **row**.
     *
     * It used to be skipped with "the stations are in it", which was true and
     * was not the reason: the stations are in it because they are in nobody's
     * wing *and* nobody's fleet, and the ownership filter above now tells those
     * two apart. What is left in wing zero after it is exactly this
     * commander's strays -- ships an `AssignWing` to zero disbanded, or that
     * arrived unassigned -- and they need a row for the reason
     * `StationActionOptions` refused to offer the disband at all: a fleet with
     * no row is a fleet that vanished off the HUD.
     */
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
   *
   * **Declared, not authored** -- and the distinction arrived with the hangar's
   * wing assignment. A wing the player creates is one the game declared too:
   * they picked the number and were shown the call sign at the moment they
   * pressed it. So the loop walks every number a `WingId` can hold and asks
   * `WingName`, which knows both tables, rather than walking the authored one.
   * The invented-label objection stands unchanged -- `EnsureWingName` is the
   * only thing that invents, and only where the alternative is ships with no
   * row at all.
   */
  std::uint32_t rows = 0;
  for (std::uint32_t wingId = 1; wingId <= std::numeric_limits<Game::WingId>::max(); ++wingId)
  {
    if (rows >= _outRows.size())
    {
      break;
    }
    const char* wingName = WingName(static_cast<Game::WingId>(wingId));
    if (wingName == nullptr)
    {
      continue;
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
    row.name = wingName;
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

  /*
   * And the strays, last (T3).
   *
   * **Last rather than first**, because they are the leftovers: a row at the
   * top would push TALON down the panel every time a ship came out of a wing,
   * and the roster's order is the one thing a player points at without reading.
   *
   * **Only when there are any**, unlike a wing, which keeps its row at zero.
   * The difference is that a wing is a thing the player made and an empty one
   * is news -- *which* wing did I just lose -- while "no ships are unassigned"
   * is the ordinary state and a permanent row saying so is a row that has to be
   * read before it can be ignored. `CountedChip`'s argument, one panel along.
   */
  const Accumulator& strays = byWing[Game::INVALID_WING_ID];
  if (strays.ships > 0 && rows < _outRows.size())
  {
    RosterRow& row = _outRows[rows];
    row.name = WingName(Game::INVALID_WING_ID);
    row.groupId = Game::INVALID_WING_ID;
    row.shipCount = strays.ships;
    row.selectedCount = strays.selected;
    row.hullGauge = static_cast<std::uint8_t>(strays.hullTotal / strays.ships);
    row.shieldGauge = static_cast<std::uint8_t>(strays.shieldTotal / strays.ships);
    row.carriesCargo = strays.holdLitres > 0;
    row.cargoGauge = row.carriesCargo ? static_cast<std::uint8_t>(std::min<std::uint64_t>(
                                          255u, strays.usedLitres * 255u / strays.holdLitres))
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
bool ReplicatedWorldView::ContextActionFor(Neuron::EntityId _entityId, std::span<const Neuron::EntityId> _selectedIds,
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
    /*
     * The anchor **and the state**, because one anchor can be two places.
     *
     * `WorldRegistry::Summaries` walks the grids and then the rosters, so a
     * commander flying at a station while some of their hulls sit inside it
     * gets two rows for that anchor -- `OnGrid` first, `Docked` second -- and
     * one `StationRoster` record, because a roster is a fact about a station
     * rather than about a fleet.
     *
     * Matching on the anchor alone therefore handed the roster to the *grid*
     * row and, because it is moved rather than copied, left the docked row with
     * an empty list. The hangar drew NOTHING DOCKED HERE over a station holding
     * a full roster while its own status bar read the count off the other row
     * and said the ships were there -- one screen contradicting itself, which
     * is what two sources for one fact buys.
     *
     * The state is the client comparing an enumerator it was given rather than
     * a word it was told, which is the line `LocationBlock::stateLabel` draws:
     * the *number* is the game's and this side may key on it, the *vocabulary*
     * is the game's and this side may not read it.
     */
    if (block.state != Game::FleetState::Docked)
    {
      continue;
    }
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

  /*
   * The docked half of `BuildScene`'s sweep, and it needs its own because a
   * docked ship is in no snapshot -- docking despawned it. Without this a
   * player who reconnected to a hangar holding a wing they made last session
   * would get a column with no header word and no roster row to match it.
   */
  for (const FleetPlace& block : staged)
  {
    for (const Game::RosterEntry& row : block.docked)
    {
      (void)EnsureWingName(row.wing);
    }
  }

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
 * --- what a wing is called (ADR-017 §6) -----------------------------------
 *
 * A wing is a number a ship carries and nothing else -- there is no wing table,
 * on this side or the authority's -- so "what is it called" is a question only
 * this side can answer, and these five functions are the whole of that answer.
 *
 * Two sources, in one order: what the **player** called it, then what the
 * content did. The first list is the settings layer's (ADR-012 §3) and the
 * second is the composition root's, and every function below reads them through
 * `WingName` so the precedence is stated once.
 */

const char* ReplicatedWorldView::WingName(Game::WingId _wing) const noexcept
{
  /*
   * The player's word first, and that ordering *is* the rename (ADR-017 §6).
   *
   * The authored table is content and stays as authored -- nothing overwrites
   * TALON -- so a rename is a second word that outranks it rather than an edit
   * to the first. Two things fall out of doing it this way. Deleting the
   * settings file restores the shipped call signs exactly, because the content
   * was never touched. And one list holds everything the player owns, which is
   * what makes saving it a read rather than a diff against the content.
   */
  const auto found = std::find_if(m_playerWingNames.begin(), m_playerWingNames.end(),
                                  [_wing](const std::pair<Game::WingId, std::string>& _entry) { return _entry.first == _wing; });
  if (found != m_playerWingNames.end())
  {
    return found->second.c_str();
  }
  return _wing < m_desc.wingNames.size() ? m_desc.wingNames[_wing].c_str() : nullptr;
}

std::size_t ReplicatedWorldView::NamedWingCount() const noexcept
{
  /*
   * Authored plus player-owned, counted once each -- not added.
   *
   * The two lists overlap the moment a player renames an authored wing, and the
   * arithmetic this replaced (`wingNames.size() - 1 + playerNames.size()`) read
   * that overlap as two named wings. It would have spent the roster's rows on
   * words rather than on rows, so renaming eight wings would have stopped the
   * hangar offering a ninth for no reason the player could see.
   *
   * Index 0 is `INVALID_WING_ID` and is never a row, which is the `- 1u`.
   */
  std::size_t count = m_desc.wingNames.empty() ? 0u : m_desc.wingNames.size() - 1u;
  for (const std::pair<Game::WingId, std::string>& entry : m_playerWingNames)
  {
    if (entry.first >= m_desc.wingNames.size())
    {
      ++count; // A wing the content never named: a row the authored table has not already counted.
    }
  }
  return count;
}

std::vector<std::pair<Game::WingId, std::string>> ReplicatedWorldView::PlayerWingNames() const
{
  return m_playerWingNames;
}

bool ReplicatedWorldView::EnsureWingName(Game::WingId _wing)
{
  if (_wing == Game::INVALID_WING_ID)
  {
    // Wing zero belongs to nothing and is never a row, so a name for it would
    // be a word nothing draws -- `BuildRoster` skips it before it looks one up.
    return false;
  }
  if (WingName(_wing) != nullptr)
  {
    return true;
  }

  /*
   * The cap, and it counts *names* rather than wings in play.
   *
   * `MAX_ROSTER_ROWS` is how many rows the HUD will ask for, and a wing with no
   * row is a wing the player cannot see -- so a name past the cap buys nothing
   * and costs the call sign it spent. Refusing here leaves the wing unnamed,
   * which is the state the roster already knows how to skip.
   */
  if (NamedWingCount() >= Neuron::MAX_ROSTER_ROWS || m_playerWingNames.size() == m_playerWingNames.capacity())
  {
    // The second clause is the pointer-stability guard, not a second row cap:
    // growing this vector would move the names already handed out. See the
    // constructor.
    return false;
  }

  /*
   * An authored call sign if the content held one back, and a dull one if not.
   *
   * The fallback is not a failure mode to be ashamed of: it is what a wing
   * whose name did not survive the session looks like, and `WING 9` is a word
   * the player can point at and re-name once ADR-012's settings layer gives
   * them somewhere to put the new one. The alternative -- no name -- is the
   * ships vanishing off the roster, which is the outcome this exists to stop.
   */
  std::string name;
  if (!m_desc.spareWingNames.empty())
  {
    name = std::move(m_desc.spareWingNames.front());
    m_desc.spareWingNames.erase(m_desc.spareWingNames.begin());
  }
  else
  {
    char generated[16] = {};
    std::snprintf(generated, sizeof(generated), "WING %u", static_cast<unsigned>(_wing));
    name = generated;
  }

  m_playerWingNames.emplace_back(_wing, std::move(name));
  return true;
}

Game::WingId ReplicatedWorldView::FreeWingId() const noexcept
{
  /*
   * The lowest unnamed number, which is also the lowest number no *row* is
   * about -- and those are the same question, because a wing exists on this
   * screen iff it has a name. A player who disbanded wing 3 does not get 3 back
   * until its name is released, and names are never released (see `Desc`), so
   * in practice this counts upward for the life of the session.
   */
  for (std::uint32_t wing = 1; wing <= std::numeric_limits<Game::WingId>::max(); ++wing)
  {
    const auto candidate = static_cast<Game::WingId>(wing);
    if (WingName(candidate) == nullptr)
    {
      return NamedWingCount() >= Neuron::MAX_ROSTER_ROWS ? Game::INVALID_WING_ID : candidate;
    }
  }
  return Game::INVALID_WING_ID;
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

/*
 * One intent, one command, and both callers read it from here.
 *
 * `verb` and `parameter` are the game's numbers coming back through the seam
 * unread, which is the contract `OrderIntent::kind` has: the client offered a
 * verb the game listed and hands the same number back, so nothing in
 * `NeuronClient` ever had to know what it meant.
 *
 * The parameter is a formation for one verb and a wing for another, and the
 * switch below is the only place that distinction exists on this side.
 */
bool ReplicatedWorldView::MakeStationCommand(const Neuron::StationIntent& _intent,
                                             Game::StationCommand& _outCommand) const noexcept
{
  _outCommand = Game::StationCommand{};
  _outCommand.orderSeq = _intent.orderSeq;
  _outCommand.verb = static_cast<Game::StationVerb>(_intent.verb);
  _outCommand.station = static_cast<Game::AnchorId>(_intent.anchor);

  switch (_outCommand.verb)
  {
  case Game::StationVerb::Undock:
    _outCommand.formation = static_cast<Game::FormationId>(_intent.parameter);
    break;
  case Game::StationVerb::AssignWing:
    _outCommand.wing = static_cast<Game::WingId>(_intent.parameter);
    break;
  default:
    // The economy's transfer verbs carry an ore and a count rather than one
    // number, so they are not expressible as a `StationIntent` and E5's screens
    // will need their own shape. Refusing here is better than sending a command
    // with a default ore in it.
    return false;
  }

  for (std::uint32_t index = 0; index < _intent.shipCount; ++index)
  {
    if (!_outCommand.AddShip(static_cast<Game::ShipId>(_intent.shipIds[index])))
    {
      return false;
    }
  }
  return true;
}

/*
 * The station's tab row (`station-screen.png` §2, ADR-020 §6).
 *
 * The words are spelled here because this is the composition root, which is the
 * one project entitled to know both vocabularies (ADR-014 §2a) -- the same
 * licence `ReasonText` uses. `NeuronClient` gets a name, a reason and a number,
 * and never learns that one of them opens a hangar.
 *
 * **A tab with no service behind it is drawn and disabled with its reason**,
 * rather than hidden, exactly as the strategic map stubs its unbuilt overlays.
 * Which is why REPAIR reads FREE and not FUTURE: repair is not missing, it is
 * free, because the roster holds no damage and undocking repairs by
 * construction (ADR-017 §1). The tag says which kind of absence this is.
 *
 * CARGO and REFINERY are E5's and are stubbed here until it lands; the print
 * already reserves their place in the row, which is the point of a fixed row.
 */
std::uint32_t ReplicatedWorldView::BuildStationTabs(std::uint16_t _anchor, std::span<Neuron::StationTab> _outTabs) const
{
  // Nothing of this commander's is docked there, so there is no screen to tab
  // across. Refusing here rather than drawing an empty row is the same rule the
  // location blocks take: a place holding nothing of yours is not a place.
  if (DockedAt(static_cast<Game::AnchorId>(_anchor)) == nullptr)
  {
    return 0;
  }

  struct Entry
  {
    const char* name;
    const char* tag;
    bool enabled;
  };

  // The seven the print draws, in the print's order.
  static constexpr Entry ROW[] = {
      {"HANGAR", nullptr, true},   {"CARGO", "SOON", false},   {"REFINERY", "SOON", false},
      {"REPAIR", "FREE", false},   {"REFIT", "FUTURE", false}, {"CONSTRUCT", "FUTURE", false},
      {"MARKET", "FUTURE", false},
  };

  std::uint32_t written = 0;
  for (std::uint32_t index = 0; index < std::size(ROW) && written < _outTabs.size(); ++index)
  {
    Neuron::StationTab& tab = _outTabs[written];
    tab = Neuron::StationTab{};
    tab.name = ROW[index].name;
    tab.tag = ROW[index].tag;
    tab.id = static_cast<std::uint16_t>(index);
    tab.enabled = ROW[index].enabled;
    ++written;
  }
  return written;
}

/*
 * The docked roster, grouped by wing (ADR-017 §6, ADR-020 §6).
 *
 * One walk that fills both spans, because a chip's group is an index into the
 * groups this same call wrote and two walks would be two chances to disagree
 * about it.
 *
 * **The sort is ADR-017 §6a.4's and it is a decision rather than a
 * convenience:** class descending, then **ship id** -- not name. Names live in
 * the user settings layer, so sorting by them would give two clients two
 * different readings of one hangar, and the id is the part they agree on.
 *
 * `StationChip::name` is left null, and that is honest rather than unfinished:
 * **nothing in this game names an individual ship.** The print draws a name
 * beside the class, and what would fill it -- a call sign scheme, or authored
 * names in the fleet file -- is a content decision nobody has taken. A null
 * name is the arrangement `LocationBlock::buttonLabel` already uses for "the
 * game supplied nothing", and the screen draws what it does have.
 */
Neuron::StationRosterCounts ReplicatedWorldView::BuildStationRoster(std::uint16_t _anchor,
                                                                    std::span<const std::uint32_t> _selectedIds,
                                                                    std::span<Neuron::StationGroup> _outGroups,
                                                                    std::span<Neuron::StationChip> _outChips) const
{
  Neuron::StationRosterCounts counts;

  const FleetPlace* place = DockedAt(static_cast<Game::AnchorId>(_anchor));
  if (place == nullptr)
  {
    return counts;
  }

  // Sorted into a scratch list rather than in place: `m_places` is what the
  // wire wrote and a draw call must not reorder it.
  m_rosterSort.assign(place->docked.begin(), place->docked.end());
  std::sort(m_rosterSort.begin(), m_rosterSort.end(), [](const Game::RosterEntry& _a, const Game::RosterEntry& _b) {
    if (_a.hullClass != _b.hullClass)
    {
      return static_cast<int>(_a.hullClass) > static_cast<int>(_b.hullClass);
    }
    return _a.shipId < _b.shipId;
  });

  const auto isSelected = [_selectedIds](Game::ShipId _shipId) {
    return std::find(_selectedIds.begin(), _selectedIds.end(), static_cast<std::uint32_t>(_shipId)) !=
           _selectedIds.end();
  };

  for (const Game::RosterEntry& entry : m_rosterSort)
  {
    // Counted first, and before every cap below it: this is how many ships are
    // *there*, which stays true however little of the roster fits.
    ++counts.docked;

    // Which column this ship's wing is, adding one if it has not been seen.
    std::uint32_t group = 0;
    for (; group < counts.groups; ++group)
    {
      if (_outGroups[group].idTag == entry.wing)
      {
        break;
      }
    }
    if (group == counts.groups)
    {
      if (counts.groups == _outGroups.size())
      {
        // More wings than the caller has columns for. The ships in them are
        // dropped rather than folded into another column, because a chip in the
        // wrong wing is worse than a chip that is not drawn.
        continue;
      }
      Neuron::StationGroup& fresh = _outGroups[counts.groups];
      fresh = Neuron::StationGroup{};
      fresh.idTag = entry.wing;
      fresh.name = WingName(entry.wing);
      ++counts.groups;
    }

    Neuron::StationGroup& column = _outGroups[group];
    ++column.chipCount;

    const bool selected = isSelected(entry.shipId);
    if (selected)
    {
      ++column.selectedCount;
    }

    if (counts.chips == _outChips.size())
    {
      // Counted in its column and not drawn: the column's total stays true even
      // when the caller's chip array runs out, which is what lets the screen say
      // "12" over eight chips rather than lying about both.
      continue;
    }

    Neuron::StationChip& chip = _outChips[counts.chips];
    chip = Neuron::StationChip{};
    chip.shipId = static_cast<std::uint32_t>(entry.shipId);
    chip.className = Game::HullClassName(entry.hullClass).data();
    chip.group = static_cast<std::uint16_t>(group);
    chip.selected = selected;
    ++counts.chips;
  }

  return counts;
}

/*
 * Would this command be taken? (ADR-014 §3, extended to station commands.)
 *
 * The same `ValidateStationCommand` over the same `RosterView` the authority
 * judges with -- which is what the print means by "a tappable UNDOCK is a
 * promise". The roster this side is the replicated one, arriving at about 1 Hz,
 * so the two views can differ by a second; that is the ordinary staleness every
 * pre-check on this seam has, and the ack is still what decides.
 */
/*
 * Who is in a roster group (ADR-020 §6, `tactical-hud.png` §1).
 *
 * `BuildRoster`'s other half: that one counts the sampled fleet per wing and
 * hands back a row with an opaque `groupId`, and this turns the number back
 * into the ships it stood for.
 *
 * **The same population, walked the same way.** `m_sampled` rather than the
 * newest snapshot's records, exactly as `BuildRoster` does and for the same
 * reason -- these are the ships the frame drew, including the exclusions
 * `BuildScene` makes. Answering from anywhere else would let a press select a
 * ship the player cannot see, or miss one they can.
 *
 * **Wing zero answers now**, which it did not until T3 gave the strays a row.
 * The refusal here was a consequence of there being no row to press rather than
 * a rule of its own, and leaving it in place would have made the new row the one
 * row on the panel that does nothing when tapped.
 *
 * **And only this commander's ships**, for `BuildRoster`'s reason exactly: a
 * wing number is a byte every commander numbers from one, so without the
 * ownership filter a press on TALON would have selected a hostile wing 3 along
 * with it -- and then offered an order over the pair.
 */
std::uint32_t ReplicatedWorldView::BuildGroupMembers(std::uint16_t _groupId, std::span<Neuron::EntityId> _outIds) const
{
  std::uint32_t count = 0;
  for (const Game::ReplicatedShip& ship : m_sampled)
  {
    if (count >= _outIds.size())
    {
      break;
    }
    if (ship.wing == _groupId && Game::RelationshipFrom(ship.statusBits) == Game::Relationship::Own)
    {
      _outIds[count] = ship.id;
      ++count;
    }
  }
  return count;
}

/*
 * A wing's new call sign (ADR-017 §6, ADR-012 §3, T3).
 *
 * `EnsureWingName`'s twin, and deliberately not the same function: that one
 * *invents* a word for a wing that has none, refusing at capacity because the
 * alternative is spending a spare call sign nobody asked for. This one carries
 * a word the player typed, so it must not be refused for want of a spare and
 * must not consume one.
 *
 * **It replaces rather than appends when the wing already has a player word**,
 * which is what stops a player renaming one wing four times from spending four
 * of the sixteen entries the pointer-stability reserve holds.
 */
bool ReplicatedWorldView::RenameGroup(std::uint16_t _groupId, std::string_view _name)
{
  const auto wing = static_cast<Game::WingId>(_groupId);
  if (_groupId > std::numeric_limits<Game::WingId>::max() || wing == Game::INVALID_WING_ID || _name.empty())
  {
    /*
     * Wing zero is refused, and that is a rule rather than a bounds check: it
     * is not a wing, it is the absence of one. The row the strays draw is a
     * heading over "ships in no wing", so a player who renamed it would have
     * named a category the game does not have and the next disband would fill
     * it with somebody else's idea of what it meant.
     */
    return false;
  }

  const auto found = std::find_if(m_playerWingNames.begin(), m_playerWingNames.end(),
                                  [wing](const std::pair<Game::WingId, std::string>& _entry) { return _entry.first == wing; });
  if (found != m_playerWingNames.end())
  {
    /*
     * Assigned in place, which keeps the pointer this entry has already handed
     * out valid **only because a `std::string` owns its own storage**: the
     * vector does not move, the string may reallocate, and every borrowed
     * `const char*` came from `c_str()` on *this* string. So a rename between a
     * frame's `BuildRoster` and its draw would leave that frame pointing at
     * freed bytes -- which is why the caller commits a rename on an input edge,
     * before the frame's rows are built, and never mid-draw.
     */
    found->second = _name;
    return true;
  }

  /*
   * A first player word for this wing costs an entry, and the reserve is
   * absolute: growing the vector would move every name already on screen.
   *
   * The *row cap* is deliberately not checked here, unlike `EnsureWingName`.
   * That function decides whether a wing is worth naming at all; this one is
   * told that it is, by a player looking at its row. A wing being renamed
   * already has a row -- it is where the gesture landed -- so refusing it for
   * the roster's row budget would be refusing to rename something the roster is
   * currently drawing.
   */
  if (m_playerWingNames.size() == m_playerWingNames.capacity())
  {
    return false;
  }
  m_playerWingNames.emplace_back(wing, std::string(_name));

  /*
   * And the word is struck off the spare pool if it was one of them.
   *
   * A player who types `KESTREL` on to wing 4 has taken the call sign the
   * hangar would otherwise have handed to the next new wing, and two things
   * with one word is the failure the constructor already guards against on
   * reload. Cheap, and it keeps that invariant true through the one path that
   * can introduce a collision at runtime.
   */
  const auto spare = std::find(m_desc.spareWingNames.begin(), m_desc.spareWingNames.end(), _name);
  if (spare != m_desc.spareWingNames.end())
  {
    m_desc.spareWingNames.erase(spare);
  }
  return true;
}

/*
 * The composer's primary action (`station-screen.png` §2, ADR-020 §6).
 *
 * One verb today, and the word for it lives here rather than in the client for
 * the reason the tab row's words do: `NeuronClient` may send `Undock` and must
 * not be able to spell it.
 *
 * Refused for a station this commander has nothing docked at, which is the
 * same gate `BuildStationTabs` opens with. An action against an empty hangar
 * would be a live button whose every press bounced.
 */
std::uint32_t ReplicatedWorldView::BuildStationActions(std::uint16_t _anchor,
                                                       std::span<Neuron::StationAction> _outActions) const
{
  if (_outActions.empty() || DockedAt(static_cast<Game::AnchorId>(_anchor)) == nullptr)
  {
    return 0;
  }

  _outActions[0] = Neuron::StationAction{};
  _outActions[0].name = "UNDOCK";
  _outActions[0].parameterName = "FORMATION";
  _outActions[0].verb = static_cast<std::uint16_t>(Game::StationVerb::Undock);
  // The ships leave the roster the moment this is accepted, so a composer that
  // still named them would build its next command against rows that have gone.
  _outActions[0].consumesSelection = true;
  if (_outActions.size() < 2)
  {
    return 1;
  }

  /*
   * And the reorganisation room's own verb (ADR-017 §6).
   *
   * It was the last thing on this screen that the authority could do and the
   * player could not ask for: `AssignWing` has had a format, a validator, a
   * server path and a test since T1, and no control anywhere that emitted one.
   * The consequence was narrow and unmistakable in play -- dock two ships out
   * of one wing and two out of another, and there was no way to make the four
   * of them a wing, because the only thing that could rewrite a `WingId` was a
   * command with no button.
   *
   * **Undocking them together was never the same answer.** ADR-017 §3 is right
   * that the undock selection composes a *fleet* -- fleets are emergent from
   * location, and four ships leaving on one command arrive as one. A wing is
   * not: it is a number that rides on the ship, it is what the roster groups
   * by, and it survives the fleet dispersing. So the four ships flew together
   * and still read as two wings on the HUD, which is the player's report.
   *
   * Second rather than first because it is the secondary action: the print
   * gives the primary a 68 px button and this a 30 px one, and the order here
   * is the order the screen fills its two control pairs in.
   */
  _outActions[1] = Neuron::StationAction{};
  _outActions[1].name = "ASSIGN TO WING";
  _outActions[1].parameterName = "WING";
  _outActions[1].verb = static_cast<std::uint16_t>(Game::StationVerb::AssignWing);
  /*
   * And it leaves the selection alone, which is the opposite of UNDOCK's answer
   * and the reason that flag exists.
   *
   * These ships are still docked, still on the roster, still pickable -- the
   * command rewrote a number on them. Clearing here would make the obvious next
   * gesture, assign-then-undock, cost the whole selection twice; worse, it
   * would make a mis-picked wing unrecoverable without re-tapping every chip.
   */
  _outActions[1].consumesSelection = false;
  return 2;
}

/*
 * And what its parameter may be.
 *
 * `OrderOptions`' body for `OrderKind::Move`, over the same `FORMATION_IDS` and
 * naming them with the same function -- a fleet leaving a hangar forms up the
 * way a fleet crossing a grid does, and two lists of formations that could
 * drift apart would be two.
 *
 * **`AssignWing`'s list is built rather than authored, and that is the whole
 * difference between the two verbs here.** A formation is a short fixed table
 * and always the same three words; a wing is emergent, so the values are
 * whatever wings currently have names plus the next number that does not --
 * which is exactly ADR-017 §6's "new wing is picking an unused number", offered
 * as a value rather than as a second verb. It is still a cycling chip: the list
 * is bounded by the roster's row cap, which is what makes it short.
 *
 * ~~Wing zero is **not** offered~~ **and since T3 it is, because the thing that
 * blocked it was built.** The objection was never to the verb: assigning to
 * zero is how a wing disbands, and the reason to refuse was that `BuildRoster`
 * drew no row for it, so the button would have made ships disappear off the HUD.
 * The strays have a row now -- last on the panel, and only when there are any --
 * so a disband moves a fleet from one row to another where it can still be seen,
 * selected and re-grouped. It is offered **last**, after the wings and after the
 * next unused number, because it is the destructive one.
 *
 * The economy's four answer zero: they carry ores, alloys and quantities, none
 * of them a short list, so a screen for them is E5's rather than a longer table
 * here.
 */
std::uint32_t ReplicatedWorldView::StationActionOptions(std::uint16_t _verb,
                                                        std::span<Neuron::OrderOption> _outOptions) const
{
  if (_verb == static_cast<std::uint16_t>(Game::StationVerb::AssignWing))
  {
    std::uint32_t wings = 0;
    for (std::uint32_t wing = 1; wing <= std::numeric_limits<Game::WingId>::max(); ++wing)
    {
      if (wings >= _outOptions.size())
      {
        break;
      }
      if (const char* name = WingName(static_cast<Game::WingId>(wing)); name != nullptr)
      {
        _outOptions[wings].parameter = static_cast<std::uint16_t>(wing);
        _outOptions[wings].name = name;
        ++wings;
      }
    }

    /*
     * And the one at the end that is not a wing yet.
     *
     * Its number is real -- `FreeWingId` picks it, the intent carries it, and
     * the authority writes it into the roster row like any other -- so this is
     * a value in the same list rather than a special case the command has to
     * know about. What is not real yet is the *name*, which nothing mints until
     * the command is actually sent (`EncodeStationCommand`): a player who
     * cycles past this option and settles on something else must not have
     * spent a call sign doing it.
     *
     * Absent when the roster has no room for another row, which is the honest
     * shape of that limit: the chip simply stops offering new wings, rather
     * than offering one that would go somewhere the player cannot see.
     */
    const Game::WingId fresh = FreeWingId();
    if (wings < _outOptions.size() && fresh != Game::INVALID_WING_ID)
    {
      _outOptions[wings].parameter = fresh;
      _outOptions[wings].name = "NEW WING";
      ++wings;
    }

    /*
     * And the disband, last (T3).
     *
     * Its word is the strays' own row heading rather than a second one, so the
     * chip says where the ships are going and the player can then read the row
     * they went to. A verb whose destination is named one way in the control
     * and another on the panel is a verb the player has to learn twice.
     */
    if (wings < _outOptions.size())
    {
      _outOptions[wings].parameter = Game::INVALID_WING_ID;
      _outOptions[wings].name = WingName(Game::INVALID_WING_ID);
      ++wings;
    }
    return wings;
  }

  if (_verb != static_cast<std::uint16_t>(Game::StationVerb::Undock))
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

Neuron::OrderVerdict ReplicatedWorldView::PreCheckStation(const Neuron::StationIntent& _intent)
{
  Game::StationCommand command;
  if (!MakeStationCommand(_intent, command))
  {
    // More ships than one command can name. The screen is supposed to have
    // split them into waves before reaching here, so this is a refusal rather
    // than a truncation -- sending 64 of 66 would undock most of a fleet and
    // report success.
    return Neuron::OrderVerdict{};
  }

  const FleetPlace* place = DockedAt(command.station);
  Game::RosterView view;
  view.station = command.station;
  if (place != nullptr)
  {
    view.docked = place->docked;
  }

  /*
   * And this commander's ships *flying* at the same anchor (I2, ADR-017 §6's
   * 2026-08-23 amendment), which is the other half of what an `AssignWing` may
   * name.
   *
   * Only when the watched grid is the one the command is about. The client has
   * exactly one grid's worth of entities at a time (ADR-022 §1), so a command
   * naming any other anchor gets an empty span -- which the validator reads as
   * "the caller cannot say" and refuses, on the same designed asymmetry
   * `bayUnits` already runs on. The authority is still what decides.
   *
   * **`m_sampled` rather than the newest snapshot's records**, which is what
   * `BuildRoster` and `BuildGroupMembers` both do and for the same reason:
   * these are the ships the frame drew, so a pre-check cannot accept a ship the
   * player cannot see or refuse one they can.
   *
   * The relationship bits are what stand in for the server's owner filter
   * (ADR-022 §8b): the wire spends two bits saying what a viewer is to a ship,
   * so `Own` here is the same set `OnGridFor` builds from the registry's index.
   * That the two halves reach it by different routes is the point -- neither
   * machine can see what the other used.
   */
  std::vector<Game::RosterEntry> flying;
  if (m_view.Grid() == command.station)
  {
    flying.reserve(m_sampled.size());
    for (const Game::ReplicatedShip& ship : m_sampled)
    {
      if (Game::RelationshipFrom(ship.statusBits) != Game::Relationship::Own)
      {
        continue;
      }
      Game::RosterEntry row;
      row.shipId = ship.id;
      row.hullClass = static_cast<Game::HullClass>(ship.classId);
      row.wing = ship.wing;
      flying.push_back(row);
    }
    view.grid = command.station;
    view.onGrid = flying;
  }

  const Game::OrderVerdict decided = Game::ValidateStationCommand(view, command);
  return ToSeam(decided, _intent.orderSeq);
}

/*
 * The bytes, and the reason this function is the one T3b could not start
 * without.
 *
 * `StationCommand` has had a format, a validator, a server path and a
 * `selfTest` since T2 -- and no line in the client that could produce one, so
 * UNDOCK was a verb the authority knew and nobody could speak. `EncodeOrder`
 * writes `CommandKind::Order`; this writes the other one.
 */
bool ReplicatedWorldView::EncodeStationCommand(const Neuron::StationIntent& _intent, Neuron::ByteWriter& _writer)
{
  Game::StationCommand command;
  if (!MakeStationCommand(_intent, command))
  {
    return false;
  }

  /*
   * A wing the player just invented gets its call sign here, and here is the
   * only place it could go.
   *
   * The number went on the wire and the word did not -- the server knows wings
   * as numbers and nothing else (ADR-017 §6) -- so the name has to be recorded
   * on this side, and the moment to record it is the moment the command is
   * committed to. Not while the chip is being cycled, which would spend a call
   * sign on every option the player looked at; and not when the roster comes
   * back carrying the new number, which is a round trip later and would leave
   * the wing nameless on the screen that just created it.
   *
   * Its answer is deliberately not checked. `EnsureWingName` refuses past the
   * roster's row cap, and the right response to that is to send the command
   * anyway: the player asked for a regrouping the authority will perform, and
   * refusing it here would be this side vetoing a valid command over a display
   * limit. The wing exists, carries ships, and draws no row -- which is what an
   * unnamed wing has always done.
   */
  if (command.verb == Game::StationVerb::AssignWing)
  {
    (void)EnsureWingName(command.wing);
  }

  return Game::WriteCommandKind(Game::CommandKind::Station, _writer) && Game::WriteStationCommand(command, _writer);
}

std::uint32_t ReplicatedWorldView::ShipsPerStationCommand() const
{
  return Game::MAX_SHIPS_PER_ORDER;
}

const ReplicatedWorldView::FleetPlace* ReplicatedWorldView::DockedAt(Game::AnchorId _anchor) const noexcept
{
  const auto found = std::find_if(m_places.begin(), m_places.end(), [&](const FleetPlace& _entry) {
    return _entry.anchor == _anchor && _entry.state == Game::FleetState::Docked;
  });
  return found == m_places.end() ? nullptr : &*found;
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

/*
 * --- the strategic map (ADR-018 D14, ADR-016 §9, U5) -----------------------
 *
 * The composition root doing exactly what ADR-014 §6 says it is for: reading
 * the universe and handing over neutral data. Every game word on that screen is
 * written in this section, and the engine draws all of it without learning what
 * any of it means.
 */
namespace
{

/*
 * How wide the map's own space is.
 *
 * Arbitrary, which is what `MapNode::x` means by "map units" -- the screen fits
 * whatever it is given. It exists because universe coordinates are `std::int64_t`
 * **metres** over roughly a thousand light years, and a float32 handed one of
 * those directly keeps about seven significant digits -- so a graph converted
 * without recentring would quantise every system in a constellation onto the
 * same point, which is the map's whole subject collapsing. The conversion
 * therefore recentres first and scales second, and a thousand units across is a
 * range float32 holds exactly enough of to draw.
 */
constexpr double MAP_UNITS_ACROSS = 1000.0;

/// The bands a per-system security number is drawn in.
///
/// Three, over `StandingColour`'s four, because a system has no "own" -- that is
/// the sovereignty overlay's word and sovereignty does not exist yet. The exact
/// number still reaches the player, in the badge beside it, which is what
/// `settings.png` means by colour never being the only carrier.
constexpr std::uint8_t SECURE_AT_LEAST = 60;
constexpr std::uint8_t CONTESTED_AT_LEAST = 30;

[[nodiscard]] Neuron::StandingColour SecurityTint(std::uint8_t _security) noexcept
{
  if (_security >= SECURE_AT_LEAST)
  {
    return Neuron::StandingColour::Allied;
  }
  if (_security >= CONTESTED_AT_LEAST)
  {
    return Neuron::StandingColour::Neutral;
  }
  return Neuron::StandingColour::Hostile;
}

/// `SEC 0.2`, the print's own spelling of a 0..100 byte.
[[nodiscard]] std::string SecurityBadge(std::uint8_t _security)
{
  char buffer[16] = {};
  std::snprintf(buffer, sizeof buffer, "SEC %u.%u", static_cast<unsigned>(_security) / 100u,
                (static_cast<unsigned>(_security) % 100u) / 10u);
  return buffer;
}

/// A region's band badge -- the print's `FRONTIER`, from the ceiling of the
/// range its systems vary inside. Two numbers become one word, which is the
/// distinction `strategic-map.png` §4 asks to keep visible: the band is a badge
/// and the number is per-system.
[[nodiscard]] const char* BandName(std::uint8_t _ceiling) noexcept
{
  if (_ceiling >= SECURE_AT_LEAST)
  {
    return "CORE";
  }
  if (_ceiling >= CONTESTED_AT_LEAST)
  {
    return "MARCH";
  }
  return "FRONTIER";
}

/// `4m 10s`, or `40s`. Minutes are dropped when there are none rather than
/// written as zero, because `0m 40s` reads like a placeholder.
[[nodiscard]] std::string EtaText(std::uint32_t _seconds)
{
  char buffer[24] = {};
  if (_seconds >= 60u)
  {
    std::snprintf(buffer, sizeof buffer, "ETA %um %02us", _seconds / 60u, _seconds % 60u);
  }
  else
  {
    std::snprintf(buffer, sizeof buffer, "ETA %us", _seconds);
  }
  return buffer;
}

/// An index lookup by id, sized to the largest id rather than to the count --
/// ids are not promised dense and a hand-written universe need not make them so.
[[nodiscard]] std::vector<std::uint16_t> MakeIndexTable(std::size_t _size)
{
  return std::vector<std::uint16_t>(_size, 0xffffu);
}

} // namespace

void ReplicatedWorldView::BuildMapGraph()
{
  const Game::UniverseDef* universe = m_desc.universe;
  if (universe == nullptr || universe->systems.empty())
  {
    return;
  }

  /*
   * The bounding box, in universe centimetres, over the systems *and* the
   * constellation centres.
   *
   * Both, because a constellation's centre is placed rather than derived
   * (`Constellation::centre`) and can therefore sit outside the hull of its own
   * members -- and a box that excluded it would let a hull's centre fall off the
   * side of a fitted map.
   */
  std::int64_t minX = universe->systems.front().centre.x;
  std::int64_t maxX = minX;
  std::int64_t minY = universe->systems.front().centre.y;
  std::int64_t maxY = minY;
  const auto stretch = [&](const Game::UniversePos& _pos) noexcept {
    minX = std::min(minX, _pos.x);
    maxX = std::max(maxX, _pos.x);
    minY = std::min(minY, _pos.y);
    maxY = std::max(maxY, _pos.y);
  };
  for (const Game::SolarSystem& system : universe->systems)
  {
    stretch(system.centre);
  }
  for (const Game::Constellation& constellation : universe->constellations)
  {
    stretch(constellation.centre);
  }

  // Midpoints written as `min + span/2` rather than `(min + max)/2`: the sum of
  // two galaxy-scale coordinates is where an int64 would overflow, and the
  // difference is not.
  const std::int64_t spanX = maxX - minX;
  const std::int64_t spanY = maxY - minY;
  const double originX = static_cast<double>(minX) + static_cast<double>(spanX) * 0.5;
  const double originY = static_cast<double>(minY) + static_cast<double>(spanY) * 0.5;
  const double widest = std::max({static_cast<double>(spanX), static_cast<double>(spanY), 1.0});
  const double perUnit = widest / MAP_UNITS_ACROSS;

  const auto toMap = [&](std::int64_t _value, double _origin) noexcept {
    return static_cast<float>((static_cast<double>(_value) - _origin) / perUnit);
  };

  /*
   * The badges first, and the records that borrow from them second.
   *
   * `MapNode::badge` is a borrowed `const char*` into a short `std::string`, so
   * a `push_back` that reallocated after the nodes were filled would leave every
   * badge already handed out pointing at freed storage. Filling this vector to
   * its final size before anything points into it removes the hazard rather than
   * racing it -- `m_playerWingNames`' reserve, one file along, for one reason.
   */
  m_mapBadges.clear();
  m_mapBadges.reserve(universe->systems.size());
  for (const Game::SolarSystem& system : universe->systems)
  {
    m_mapBadges.push_back(SecurityBadge(system.security));
  }

  std::vector<std::uint16_t> constellationIndex = MakeIndexTable(universe->constellations.size() + 1u);
  std::vector<std::uint16_t> regionIndex = MakeIndexTable(universe->regions.size() + 1u);

  m_mapRegions.clear();
  m_mapRegions.reserve(universe->regions.size());
  for (const Game::Region& region : universe->regions)
  {
    if (region.id < regionIndex.size())
    {
      regionIndex[region.id] = static_cast<std::uint16_t>(m_mapRegions.size());
    }
    Neuron::MapRegion out;
    out.id = region.id;
    out.label = region.name.c_str();
    out.badge = BandName(region.securityCeiling);
    out.tint = SecurityTint(region.securityCeiling);
    m_mapRegions.push_back(out);
  }

  m_mapGroups.clear();
  m_mapGroups.reserve(universe->constellations.size());
  for (const Game::Constellation& constellation : universe->constellations)
  {
    if (constellation.id < constellationIndex.size())
    {
      constellationIndex[constellation.id] = static_cast<std::uint16_t>(m_mapGroups.size());
    }
    Neuron::MapGroup out;
    out.id = constellation.id;
    out.region = constellation.region < regionIndex.size() ? regionIndex[constellation.region] : 0u;
    out.x = toMap(constellation.centre.x, originX);
    out.y = toMap(constellation.centre.y, originY);
    out.label = constellation.name.c_str();
    m_mapGroups.push_back(out);
  }

  /*
   * A region has no placed centre in the bake -- ADR-009 §4 gives it a name and
   * a security band and nothing spatial -- so the map derives one from its
   * constellations. Derived rather than authored is exactly what `Constellation`
   * refuses for itself, and the difference is that a constellation's centre is
   * load-bearing (U1's clustering invariant is stated against it) while a
   * region's is a label's anchor.
   */
  for (std::size_t index = 0; index < m_mapRegions.size(); ++index)
  {
    double sumX = 0.0;
    double sumY = 0.0;
    std::uint32_t members = 0;
    for (const Neuron::MapGroup& group : m_mapGroups)
    {
      if (group.region != index)
      {
        continue;
      }
      sumX += static_cast<double>(group.x);
      sumY += static_cast<double>(group.y);
      ++members;
    }
    if (members > 0)
    {
      m_mapRegions[index].x = static_cast<float>(sumX / members);
      m_mapRegions[index].y = static_cast<float>(sumY / members);
    }
  }

  m_mapNodeBySystem = MakeIndexTable(universe->systems.size() + 1u);
  m_mapNodes.clear();
  m_mapNodes.reserve(universe->systems.size());
  for (std::size_t index = 0; index < universe->systems.size(); ++index)
  {
    const Game::SolarSystem& system = universe->systems[index];
    if (system.id < m_mapNodeBySystem.size())
    {
      m_mapNodeBySystem[system.id] = static_cast<std::uint16_t>(m_mapNodes.size());
    }
    Neuron::MapNode out;
    out.id = system.id;
    out.group = system.constellation < constellationIndex.size() ? constellationIndex[system.constellation] : 0u;
    out.x = toMap(system.centre.x, originX);
    out.y = toMap(system.centre.y, originY);
    out.label = system.name.c_str();
    out.badge = m_mapBadges[index].c_str();
    out.tint = SecurityTint(system.security);
    m_mapNodes.push_back(out);
  }

  /*
   * The gate graph, **stored once per pair**.
   *
   * The bake authors gates as symmetric pairs, so walking every system's gate
   * list would produce each link twice -- two lines over each other at double
   * the alpha, which reads as a deliberate highlight. Keeping only the edge that
   * runs from the lower node index is the cheapest way to pick one of the two,
   * and it does not depend on which end the walk reached first.
   */
  m_mapLinks.clear();
  std::size_t dropped = 0;
  for (const Game::SolarSystem& system : universe->systems)
  {
    const std::uint16_t from = system.id < m_mapNodeBySystem.size() ? m_mapNodeBySystem[system.id] : 0xffffu;
    if (from == 0xffffu)
    {
      continue;
    }
    for (const Game::Gate& gate : system.gates)
    {
      const std::uint16_t to = gate.toSystem < m_mapNodeBySystem.size() ? m_mapNodeBySystem[gate.toSystem] : 0xffffu;
      if (to == 0xffffu || to <= from)
      {
        continue;
      }
      if (m_mapLinks.size() >= Neuron::MAX_MAP_LINKS)
      {
        ++dropped;
        continue;
      }
      m_mapLinks.push_back(Neuron::MapLink{from, to});
    }
  }

  /*
   * And it says so when it drops one.
   *
   * `MAX_MAP_LINKS` is 3,000 and the committed 2,500-system bake produces
   * **exactly** 3,000 -- measured, not estimated -- so there is no headroom and
   * the next content pass that adds an edge lands here. That is the same bargain
   * `MAX_MAP_NODES` states (a shard that outgrows the client retunes the config),
   * and what makes it safe to leave tight is that outgrowing it is *audible*.
   * A map quietly missing a gate is a map a player plans a route around.
   */
  if (dropped > 0)
  {
    NEURON_LOG_WARNING("strategic map: %zu gate links past the %u the client is built for", dropped,
                       Neuron::MAX_MAP_LINKS);
  }
}

bool ReplicatedWorldView::BuildMapTopology(Neuron::MapTopology& _outTopology) const
{
  if (m_mapNodes.empty())
  {
    _outTopology = Neuron::MapTopology{};
    return false;
  }

  // Spans into storage this view owns, valid for the session -- which is what
  // "asked once at boot" means on this side of the seam.
  _outTopology.nodes = m_mapNodes;
  _outTopology.links = m_mapLinks;
  _outTopology.groups = m_mapGroups;
  _outTopology.regions = m_mapRegions;
  return true;
}

std::uint32_t ReplicatedWorldView::BuildMapOverlays(std::span<Neuron::MapOverlayOption> _outOverlays) const
{
  /*
   * The print's five, in the print's order, and four of them are stubs
   * (ADR-016 §9, §9a.3) -- *drawn, disabled, and carrying the reason*, which is
   * `BuildStationTabs`' treatment of a service that has not shipped.
   *
   * The reasons differ in kind and the words say which: three wait on content
   * that does not exist at all, and RESOURCES waits on a *layer* whose content
   * does exist in the bake (ADR-024 splits archetype and grade as public) but
   * whose presence-gated half needs the site receipts U-phase economy work
   * carries. Only SECURITY BAND can be drawn from what is committed today.
   */
  struct Entry
  {
    const char* name;
    const char* tag;
    bool enabled;
  };
  static constexpr Entry ENTRIES[] = {
      {"SOVEREIGNTY", "NO CONTENT", false}, {"ACTIVITY HEAT", "NO STREAM", false},
      {"INTEL PINGS", "NO INTEL", false},   {"SECURITY BAND", nullptr, true},
      {"RESOURCES", "NO RECEIPTS", false},
  };

  std::uint32_t written = 0;
  for (const Entry& entry : ENTRIES)
  {
    if (written >= _outOverlays.size())
    {
      break;
    }
    Neuron::MapOverlayOption& out = _outOverlays[written];
    out.name = entry.name;
    out.tag = entry.tag;
    out.id = static_cast<std::uint16_t>(written + 1u);
    out.enabled = entry.enabled;
    ++written;
  }
  return written;
}

std::uint32_t ReplicatedWorldView::BuildMapLegend(std::uint16_t _overlay, std::span<Neuron::MapLegendRow> _outRows) const
{
  /*
   * The one overlay with content gets a real key, and the four stubs get
   * nothing -- which is what a visible stub looks like from this side: a heading
   * with no rows under it rather than a block that vanishes.
   *
   * **The counting is here rather than in the client**, which is the whole
   * reason this is a seam call: the engine has the nodes and their tints and
   * could tally them in four lines, and would thereby have decided that a legend
   * counts *systems* -- rather than jumps, or hulls, or days held.
   */
  constexpr std::uint16_t SECURITY_OVERLAY = 4;
  if (_overlay != SECURITY_OVERLAY || m_desc.universe == nullptr)
  {
    return 0;
  }

  struct Band
  {
    const char* name;
    Neuron::StandingColour tint;
  };
  static constexpr Band BANDS[] = {{"SECURE", Neuron::StandingColour::Allied},
                                   {"CONTESTED", Neuron::StandingColour::Neutral},
                                   {"LAWLESS", Neuron::StandingColour::Hostile}};

  std::uint32_t written = 0;
  for (const Band& band : BANDS)
  {
    if (written >= _outRows.size())
    {
      break;
    }
    std::uint32_t count = 0;
    for (const Game::SolarSystem& system : m_desc.universe->systems)
    {
      if (SecurityTint(system.security) == band.tint)
      {
        ++count;
      }
    }
    Neuron::MapLegendRow& out = _outRows[written];
    out.name = band.name;
    out.count = count;
    out.tint = band.tint;
    ++written;
  }
  return written;
}

std::uint32_t ReplicatedWorldView::BuildMapFacts(std::uint16_t _systemId, std::span<Neuron::MapFactRow> _outRows) const
{
  if (m_desc.universe == nullptr)
  {
    return 0;
  }
  const Game::SolarSystem* system = m_desc.universe->FindSystem(static_cast<Game::SystemId>(_systemId));
  if (system == nullptr)
  {
    return 0;
  }

  /*
   * Five rows, and they are **not** the print's five.
   *
   * `strategic-map.png` draws SOVEREIGNTY, HELD SINCE, VULNERABLE, STATIONS and
   * GATES; the first three are about a system nobody has built (ADR-016 §9's
   * stub list), and the rule `MapFactRow` states is that a game reports only the
   * facts it has. A label with an empty value beside it is a promise this game
   * has not kept; a missing row is a fact it does not have -- and the panel is
   * shorter, visibly, which is the honest shape.
   *
   * What replaces them is what the bake does know, and it is not filler: the
   * constellation and the region are the two levels above this system, which is
   * the hierarchy the whole screen is organised around.
   */
  m_factValues.clear();
  m_factValues.reserve(3);

  const Game::Constellation* constellation = nullptr;
  for (const Game::Constellation& candidate : m_desc.universe->constellations)
  {
    if (candidate.id == system->constellation)
    {
      constellation = &candidate;
      break;
    }
  }
  const Game::Region* region = nullptr;
  for (const Game::Region& candidate : m_desc.universe->regions)
  {
    if (candidate.id == system->region)
    {
      region = &candidate;
      break;
    }
  }

  m_factValues.push_back(SecurityBadge(system->security));
  {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof buffer, "%zu", system->stations.size());
    m_factValues.push_back(buffer);
  }
  {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof buffer, "%zu", system->gates.size());
    m_factValues.push_back(buffer);
  }

  const Neuron::MapFactRow rows[] = {
      {"CONSTELLATION", constellation != nullptr ? constellation->name.c_str() : nullptr},
      {"REGION", region != nullptr ? region->name.c_str() : nullptr},
      {"SECURITY", m_factValues[0].c_str()},
      {"STATIONS", m_factValues[1].c_str()},
      {"GATES", m_factValues[2].c_str()},
  };

  std::uint32_t written = 0;
  for (const Neuron::MapFactRow& row : rows)
  {
    if (written >= _outRows.size())
    {
      break;
    }
    if (row.value == nullptr)
    {
      continue; // The rule above: a fact this game does not have is a row that
                // is not reported, never a label with nothing beside it.
    }
    _outRows[written] = row;
    ++written;
  }
  return written;
}

std::uint32_t ReplicatedWorldView::SolveMapRoute(std::uint16_t _toSystem, std::span<Neuron::MapRouteHop> _outHops,
                                                 Neuron::MapRouteSummary& _outSummary) const
{
  _outSummary = Neuron::MapRouteSummary{};
  if (m_desc.universe == nullptr)
  {
    return 0;
  }

  // The origin is this view's, not the caller's: the client holds a grid id,
  // and which system that grid is in is a fact about the universe.
  const Game::SystemId from = MapOriginSystem();
  if (from == Game::INVALID_ID)
  {
    return 0; // No world yet, so no route -- rather than a route from nowhere.
  }

  /*
   * Straight through to the shared planner, which is D14's whole point: *"search
   * and route-solve are GameLogic pure functions reached through seam calls"*. A
   * route solved in the client would be a route that cannot be replayed, and a
   * second solver the day one is planned from anywhere else.
   *
   * **One route, where the print offers three.** `strategic-map.png` §3 asks for
   * fastest, safest and hostile-avoiding, on the argument that presenting one
   * hides the only interesting decision on the screen. `SolveRoute` is
   * breadth-first because a gate is a gate today; when jumps stop being equal it
   * becomes Dijkstra over the same graph and the signature does not move, and
   * the second and third routes arrive then. The note below says which one this
   * is rather than implying it is the only one.
   */
  const Game::Route route = Game::SolveRoute(*m_desc.universe, from, static_cast<Game::SystemId>(_toSystem));
  if (route.systems.empty())
  {
    return 0;
  }

  m_routeBadges.clear();
  m_routeBadges.reserve(route.systems.size());
  for (const Game::SystemId id : route.systems)
  {
    const Game::SolarSystem* system = m_desc.universe->FindSystem(id);
    m_routeBadges.push_back(SecurityBadge(system != nullptr ? system->security : 0u));
  }

  std::uint32_t written = 0;
  for (std::size_t index = 0; index < route.systems.size(); ++index)
  {
    if (written >= _outHops.size())
    {
      break;
    }
    const Game::SystemId id = route.systems[index];
    const Game::SolarSystem* system = m_desc.universe->FindSystem(id);
    Neuron::MapRouteHop& out = _outHops[written];
    out.node = id < m_mapNodeBySystem.size() ? m_mapNodeBySystem[id] : 0u;
    out.name = system != nullptr ? system->name.c_str() : nullptr;
    out.badge = m_routeBadges[index].c_str();
    out.tint = SecurityTint(system != nullptr ? system->security : 0u);
    ++written;
  }

  {
    char buffer[40] = {};
    // The separator as an escape rather than as a literal, which is the
    // convention `GhostLane` already prints its `%s \xC2\xB7 ETA %s` under:
    // these files carry no byte-order mark and no `/utf-8`, so a literal is
    // read in whatever code page the compiler is running under.
    std::snprintf(buffer, sizeof buffer, "FASTEST \xC2\xB7 %zu JUMPS", route.Jumps());
    m_routeNote = buffer;
  }

  /*
   * The ETA, from the game's own constants rather than from a number chosen to
   * look like the print's.
   *
   * `GATE_JUMP_TICKS` at `TICKS_PER_SECOND` is what a jump costs by design
   * (ADR-016 §5), and this is that times the hops. **It does not count the
   * flight between gates inside each system**, which is a real omission rather
   * than a rounding: that is the tactical view's spool-and-transit arithmetic
   * and it needs a hull class to answer. Stating the jump time alone makes the
   * number a floor, which is the direction an ETA should be wrong in.
   */
  const auto seconds =
      static_cast<std::uint32_t>(route.Jumps() * Game::GATE_JUMP_TICKS / Game::TICKS_PER_SECOND);
  m_routeEta = EtaText(seconds);

  _outSummary.note = m_routeNote.c_str();
  _outSummary.eta = m_routeEta.c_str();
  return written;
}

namespace
{

/*
 * The gate anchor in `_from` that leads to `_to`, or `INVALID_ID`.
 *
 * Two lookups because the bake keeps two things: a `Gate` says where it goes,
 * and an `Anchor` is the grid you fly to. They are joined by the gate's id in
 * the anchor's `owner`, which is scoped by `kind` -- so the kind has to be
 * checked as well as the number, or a planet with the same id would answer.
 *
 * Local to this file rather than public in `UniverseRoute`, because nothing
 * else asks it yet and a public function with one caller is a guess about the
 * second one.
 */
[[nodiscard]] Game::AnchorId GateAnchorTowards(const Game::UniverseDef& _universe, Game::SystemId _from,
                                               Game::SystemId _to)
{
  const Game::SolarSystem* system = _universe.FindSystem(_from);
  if (system == nullptr)
  {
    return Game::INVALID_ID;
  }
  for (const Game::Gate& gate : system->gates)
  {
    if (gate.toSystem != _to)
    {
      continue;
    }
    for (const Game::Anchor& anchor : system->anchors)
    {
      if (anchor.kind == Game::AnchorKind::Gate && anchor.owner == gate.id)
      {
        return anchor.id;
      }
    }
  }
  return Game::INVALID_ID;
}

} // namespace

std::uint32_t ReplicatedWorldView::BuildRoutePlan(std::uint16_t _toSystem, std::span<Neuron::RouteLeg> _outLegs) const
{
  if (m_desc.universe == nullptr)
  {
    return 0;
  }
  const Game::SystemId from = MapOriginSystem();
  if (from == Game::INVALID_ID)
  {
    return 0;
  }

  /*
   * The same solve `SolveMapRoute` runs, which is what makes two calls safe.
   *
   * `SolveRoute` is breadth-first with ties broken by **system id**, so two
   * equally short routes are *the same* route -- the panel's line and the
   * fleet's path come out of one deterministic function asked twice rather
   * than out of two functions that happen to agree.
   */
  const Game::Route route = Game::SolveRoute(*m_desc.universe, from, static_cast<Game::SystemId>(_toSystem));
  if (route.systems.size() < 2)
  {
    return 0; // Nowhere to go, or already there. Both are "no plan".
  }

  /*
   * Two orders a hop, minus the one the fleet does not need.
   *
   * Crossing a gate is a warp *to* the gate on this side and then a warp
   * *through* it -- ADR-016 §5 judges the second on where the fleet is
   * standing, which is what makes the first necessary. A fleet already sitting
   * on the gate skips it, and that can only ever be the first hop: after a
   * jump the fleet is on the *arrival* gate, and the gate onwards is a
   * different anchor because a gate leads to exactly one system.
   */
  Game::AnchorId standing = CurrentGrid();
  std::uint32_t legs = 0;
  const auto push = [&](Game::AnchorId _anchor, std::size_t _hop) {
    if (legs >= _outLegs.size())
    {
      return false;
    }
    _outLegs[legs].kind = static_cast<std::uint16_t>(Game::OrderKind::Warp);
    _outLegs[legs].anchor = static_cast<std::uint16_t>(_anchor);

    // Which jump this order is part of, so the HUD can count in the unit the
    // map's panel counts in. Stamped here because this loop is the only place
    // that knows the two orders of a hop are one hop.
    _outLegs[legs].hop = static_cast<std::uint16_t>(_hop);
    ++legs;
    return true;
  };

  for (std::size_t hop = 0; hop + 1 < route.systems.size(); ++hop)
  {
    const Game::AnchorId here = GateAnchorTowards(*m_desc.universe, route.systems[hop], route.systems[hop + 1]);

    // `farSide` rather than `far`: `far` is a legacy Windows SDK macro
    // (`minwindef.h`) that expands to *nothing*, so the declaration would lose
    // its name and the file would not compile on the only platform that
    // matters here. `near` is the same, and its pair is in the self test.
    const Game::AnchorId farSide =
      here != Game::INVALID_ID ? m_desc.universe->PairedGateAnchor(here) : Game::INVALID_ID;
    if (here == Game::INVALID_ID || farSide == Game::INVALID_ID)
    {
      /*
       * A route through a gate the bake did not pair, which authored content
       * cannot produce and a hand-written file can. **Refused whole**, for
       * `RoutePlan::Set`'s reason: a plan that stopped four jumps in would
       * strand a fleet somewhere the player believes is on the way.
       */
      return 0;
    }

    if (standing != here && !push(here, hop))
    {
      return 0; // Past the caller's span, and a truncated plan is worse than none.
    }
    if (!push(farSide, hop))
    {
      return 0;
    }
    standing = farSide;
  }
  return legs;
}

Game::AnchorId ReplicatedWorldView::CurrentGrid() const noexcept
{
  /*
   * The live grid if frames have said one, and the configured one before they
   * have.
   *
   * The fallback is not a guess: `Desc::gridAnchor` is where this client was
   * told to watch, and a strategic map that could not plan a route until a
   * datagram landed would be blank for the first second of every session -- on
   * the one screen whose whole content is F15's complete universe copy, which
   * needs no connection at all to be true.
   */
  return m_view.Grid() != Game::INVALID_ID ? m_view.Grid() : m_desc.gridAnchor;
}

void ReplicatedWorldView::RefreshReachable()
{
  const Game::AnchorId grid = CurrentGrid();
  if (grid == m_reachableFor)
  {
    return; // The bake does not move, so neither does this answer.
  }
  m_reachableFor = grid;
  m_reachable = m_desc.universe != nullptr ? Game::ReachableAnchors(*m_desc.universe, grid) : std::vector<Game::AnchorId>{};
}

Game::SystemId ReplicatedWorldView::MapOriginSystem() const noexcept
{
  if (m_desc.universe == nullptr)
  {
    return Game::INVALID_ID;
  }
  /*
   * Through the *anchor*, because a grid is a place inside a system rather than
   * the system itself -- the same indirection a warp order takes, and the
   * reason `Anchor` carries its system at all.
   */
  const Game::Anchor* anchor = m_desc.universe->FindAnchor(CurrentGrid());
  return anchor != nullptr ? anchor->system : Game::INVALID_ID;
}

} // namespace Outpost
