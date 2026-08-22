#pragma once

#include "Formation.h" // `FormationStation`: the placement searches speak in solved stations.
#include "Ids.h"
#include "Orders.h"
#include "SiteField.h"
#include "Station.h"
#include "Transfer.h"
#include "Validate.h"
#include "ShipClass.h"

#include "OwnerThread.h"
#include "Random.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

/*
 * The authoritative simulation (ADR-005).
 *
 * Fixed-schema structure-of-arrays tables, stable ids, and systems that are
 * free functions run in a named order once per tick. No ECS: eleven fixed
 * classes and one movement model do not need dynamic composition, and the
 * iteration-order subtleties an archetype store brings are a determinism risk
 * this file exists to avoid (ADR-005 §1, alternatives).
 *
 * **Determinism is the load-bearing property**, and it is bought by
 * construction rather than by testing for it afterwards:
 *   - no wall clock, no OS entropy, no pointers as keys;
 *   - iteration is dense-array order and nothing else;
 *   - the only randomness is the seeded `Pcg32` carried in world state below,
 *     so a replay reproduces it exactly;
 *   - `float32` throughout at `/fp:precise`, and no `XM*Est` function anywhere
 *     in GameLogic -- their accuracy is instruction-set dependent, so swapping
 *     an exact call for an estimate breaks the replay suite, which is the alarm
 *     that rule wants (ADR-005 §5, ADR-010 §6).
 *
 * The scope is same-binary replay: same build, same seed, same tick-stamped
 * order log, bit-identical state forever. Cross-build determinism is explicitly
 * not bought (ADR-005 §6) -- it would cost fixed-point or strict-fp to serve a
 * lockstep model the architecture rejected.
 */

namespace Game
{

/// What a ship is currently doing. The enum is deliberately tiny: MVP steering
/// either seeks a point or holds one, and everything richer (patrol, follow,
/// dock) is a mode added here rather than a second movement path.
enum class GuidanceMode : std::uint8_t
{
  Hold = 0,
  Seek = 1
};

/*
 * Where one ship is going.
 *
 * ADR-005 §1 wrote this as `{mode, groupRef, stationIndex}` -- a reference into
 * the `OrderGroup` table. This carries the resolved target instead, and the
 * difference is deliberate: it keeps `Steering` from knowing that groups exist.
 * When S9 brings the group table and S10 the station solve, the group *writes*
 * these fields and steering does not change. The cost is that a station lives
 * in two places once groups exist, and the group is the authority; the benefit
 * is that the movement model can be tested with no order machinery at all,
 * which is what S6's replay harness needs.
 */
struct Guidance
{
  GuidanceMode mode = GuidanceMode::Hold;
  float targetXMetres = 0.0f;
  float targetYMetres = 0.0f;

  /// Heading to settle on once the target is reached. A ship that has arrived
  /// still turns, which is what makes a fleet face the same way at the end of
  /// a move rather than pointing wherever it happened to approach from.
  float arrivalFacingRadians = 0.0f;
};

/*
 * What one ship is carrying (ADR-024 §5a, build order E2).
 *
 * Units, not litres, and per `OreId`: units are what a cycle yields and what a
 * recipe consumes, and litres are what they occupy. Storing litres would make
 * "how much Astracite is aboard" a division, and a division is where two builds
 * round differently.
 *
 * Only the ore hold is here. The general hold every hull carries is E3's, and
 * it arrives when there is something other than ore to put in it -- a field with
 * no consumer is a byte in the world hash that nothing reads.
 */
struct ShipCargo
{
  std::uint32_t oreUnits[ORE_COUNT] = {};

  [[nodiscard]] std::uint32_t Units(OreId _ore) const noexcept { return oreUnits[static_cast<std::uint8_t>(_ore)]; }
};

/*
 * Where one ship is in its mining cycle, and what the field has done to it
 * (ADR-024 §3b, §4b).
 *
 * Per ship rather than per group, because every exit in §4b is per ship: one
 * Miner fills and stops while the others keep working, and one Miner's
 * radiation is its own. The group holds the *cluster*; this holds the work.
 *
 * All of it is hashed. Two worlds that agreed about positions and disagreed
 * about when a cycle lands have diverged, and the tick that credits the ore is
 * where it would show.
 */
struct MiningState
{
  /// The tick the current cycle completes on, or **zero for "not cycling"**.
  /// A cycle is at least 800 ticks long, so zero can never be a real end tick
  /// and the sentinel costs no field of its own.
  std::uint32_t cycleEndTick = 0;

  /// Which cluster this ship is working. Meaningless when it is not cycling.
  std::uint8_t cluster = 0;

  /// Radiation stacks (ADR-024 §3b): one per completed cycle in the field, and
  /// one shed for every `stackDecaySeconds` spent outside it.
  std::uint8_t radiationStacks = 0;

  /// Ticks spent outside the field since the last stack was shed. A counter
  /// rather than a timestamp, so a ship that crosses the boundary twice does
  /// not get credit for the time it was back inside.
  std::uint16_t outsideFieldTicks = 0;

  /// Nebula heat, and the ticks of laser-off time banked toward shedding it.
  /// Heat is added by a completed cycle and shed only while the laser is *off*,
  /// which is what makes the authored numbers mean what §3b says they mean: at
  /// 34 a cycle against a threshold of 100, the third cycle locks the laser out,
  /// and 60 seconds at 2 a second is more than enough to clear it.
  std::uint16_t heat = 0;
  std::uint16_t coolingTicks = 0;

  /// The tick the laser comes back, or zero. Nothing cycles while this is in
  /// the future, and the ship sheds heat for the whole of it.
  std::uint32_t lockoutUntilTick = 0;
};

/*
 * One leg of a group's plan: where to go and which way to face on arrival.
 *
 * Metres and radians, not the wire's centimetres and turns. The quantised form
 * is `OrderLeg` and it is what validation reads (ADR-005 §4); by the time a leg
 * reaches the group table it has been accepted and converted once, and the
 * simulation works in the units it integrates in.
 */
struct OrderGroupLeg
{
  DirectX::XMFLOAT2 anchorMetres{};
  float facingRadians = 0.0f;
};

/*
 * One accepted order, and the thing a ghost is promoted against (ADR-005 §1).
 *
 * A group rather than per-ship orders, because a shared leg is what gives a
 * fleet one ETA, one ghost and one arrival -- ADR-005's alternatives section
 * rejects per-ship independent orders for exactly that. `Guidance` still
 * carries the resolved station, so `Steering` never learns that groups exist.
 *
 * Members are ids and not slots: slots move under swap-and-pop, and a group
 * outlives any particular arrangement of the tables.
 */
struct OrderGroup
{
  std::uint32_t serverOrderId = 0;

  /// The client's own counter, echoed so a ghost can be matched to its order
  /// even if the `OrderAck` is lost (ADR-004 §6).
  std::uint32_t clientOrderSeq = 0;

  FormationId formation = FormationId::Line;
  OrderState state = OrderState::Underway;

  /*
   * What the group is *doing* (U3a). A Move flies legs; a Warp spools and then
   * leaves.
   *
   * A kind on the group rather than a second table, because a warp is an order
   * in every way that matters -- it is acked, it is replaced by the next order,
   * it holds its members, and the client draws a ghost for it. What differs is
   * what happens when it completes, and that is one branch rather than a
   * parallel machine.
   */
  OrderKind kind = OrderKind::Move;

  /// Where a `Warp` is going. `INVALID_ID` for a Move.
  AnchorId anchor = INVALID_ID;

  /// Which ore a `Mine` is after (ADR-024 §4a). `Any` for every other kind.
  OreFilter oreFilter = OreFilter::Any;

  /*
   * Which cluster a `Mine` is working (ADR-024 §4b).
   *
   * Chosen once, at ingest, from the richest one the filter matches -- and
   * **not** re-chosen as it empties: a wing that hopped clusters on its own
   * would be moving without an order, and §4b's exit is that the order
   * completes and the client feeds the next one. That is route-feeding applied
   * to industry, and it is the same accepted cost the route planner already
   * pays.
   *
   * `MAX_SITE_CLUSTERS` means "none", which is what a Mine order at a field
   * with nothing the filter accepts carries into its immediate completion.
   */
  std::uint8_t cluster = MAX_SITE_CLUSTERS;

  /*
   * When the spool finishes (ADR-016 §5), for a `Warp`.
   *
   * The window in which a warp can still be called off -- by a replacing order,
   * which is the only cancel there is: ADR-016 §8 refuses a program of verbs, so
   * "cancel" is "tell them to do something else", and the group table already
   * makes that work. Zero for a Move.
   */
  std::uint32_t spoolUntilTick = 0;

  std::uint16_t memberCount = 0;
  ShipId members[MAX_SHIPS_PER_ORDER] = {};

  /*
   * Issued by the world to itself rather than by a player (ADR-017 §4).
   *
   * Exactly one flag, and it earns its place twice: the parking order is a real
   * group, so the ETA, the drawn lane, the straggler deadline and player
   * override all come free -- and ingesting a *player's* order is what ends
   * undock protection, which a system order must not do or the fleet would lose
   * its fifteen seconds to the very order that parks it.
   */
  bool systemIssued = false;

  std::uint8_t legCount = 0;
  std::uint8_t legIndex = 0;
  OrderGroupLeg legs[MAX_ORDER_LEGS] = {};

  /// When the current leg began, so a straggler cannot wedge the fleet
  /// (ADR-005 §2). Ticks, because the tick index is the simulation's only clock.
  std::uint32_t legStartTick = 0;

  /// The tick this leg gives up at, set from its own estimate when it starts
  /// (`World::LEG_TIMEOUT_FACTOR`). A tick count rather than a duration in
  /// seconds so that world state stays integral and hashes exactly.
  std::uint32_t legDeadlineTick = 0;

  /*
   * The tick this group reached `Done`, so it can be retired after its linger
   * (`World::ORDER_DONE_LINGER_TICKS`) instead of squatting in the table
   * forever. Meaningless until the state is `Done`; zero until then.
   *
   * The linger exists for the client: a ghost retires on seeing `Done` in an
   * order record, and a group erased the same tick it finished might never be
   * reported as finished at all under loss. Holding it for a moment gives the
   * snapshot a window of chances to say so.
   */
  std::uint32_t doneTick = 0;

  /// How many members the current leg's stations were last solved for. The
  /// formation is re-solved only when this disagrees with `memberCount` (a
  /// casualty since the solve) or when the leg is new -- not every tick.
  std::uint16_t solvedMemberCount = 0;
};

/*
 * An accepted order that has not been ingested yet.
 *
 * The queue mode rides *beside* the group rather than inside it, and that is
 * the point of the type existing at all: it was smuggled through `legIndex`
 * -- one meaning "append" -- which worked, was hashed by accident, and read as
 * a leg number to everyone including the line that overwrote it two statements
 * later. A mode is not a leg index.
 *
 * It is only meaningful before ingest, which is why it is not a field on
 * `OrderGroup`: a group in `m_groups` has already been placed and has no mode
 * left to have.
 */
struct PendingOrder
{
  OrderGroup group;
  QueueMode queueMode = QueueMode::Replace;

  /// What the order actually was. A Dock never becomes a group -- ingest
  /// consumes it into transfer records instead (ADR-017 §2) -- so the kind has
  /// to survive submission rather than being inferred from a group that will
  /// not exist.
  OrderKind kind = OrderKind::Move;

  /// The anchor a Dock named. Meaningless for a Move.
  AnchorId anchor = INVALID_ID;
};

/// What a ship looks like at the moment it enters the world.
struct ShipSpawn
{
  HullClass hullClass = HullClass::Interceptor;
  WingId wing = INVALID_WING_ID;
  float xMetres = 0.0f;
  float yMetres = 0.0f;
  float headingRadians = 0.0f;

  /// Undock protection (ADR-017 §5): the tick this ship stops being protected,
  /// or zero for "never was". A spawn parameter rather than a later call
  /// because a ship that is protected from its first tick and a ship that
  /// becomes protected on its second are different things on the wire.
  std::uint32_t protectedUntilTick = 0;

  /*
   * What it arrives holding (ADR-024 §5, E3), and empty for a ship that was not
   * handed over by a crossing.
   *
   * A spawn parameter for the same reason `protectedUntilTick` is one: a ship
   * that is loaded on its first tick and a ship that is loaded on its second
   * are different things to everything downstream -- the hold check, the
   * summary, the hash. Setting it afterwards would leave one tick in which the
   * ore existed on the record and not in the world.
   */
  ShipCargo cargo;
};

class World
{
public:
  /// Fixed step (ADR-005 §2), matching the 20 Hz server tick (ADR-002 §1).
  /// A constant rather than a parameter: a variable step is the shortest path
  /// to a simulation that cannot replay.
  static constexpr float TICK_SECONDS = 0.05f;

  /*
   * The same rate, and the integer one has to agree with the float one. A build
   * where they drifted apart would have durations converted one way and motion
   * integrated the other, which is a divergence with no symptom until something
   * lands a tick early.
   *
   * A band rather than `== 1.0f`, and the reason is the very arithmetic this
   * pair exists to avoid: `0.05f` is not one twentieth, so twenty of them is
   * 1.0000000149 before rounding, and whether that lands exactly on `1.0f`
   * depends on how a compiler folds the constant. Asserting the answer to that
   * question would make the whole build hostage to it; asserting that the two
   * spellings mean the same rate is what was actually meant.
   */
  static_assert(static_cast<float>(TICKS_PER_SECOND) * TICK_SECONDS > 0.999f &&
                  static_cast<float>(TICKS_PER_SECOND) * TICK_SECONDS < 1.001f,
                "the tick rate is one number, written twice");

  /// How close counts as arrived. Set against the wire's centimetre
  /// quantisation (ADR-004) with room to spare: a tolerance below the
  /// replicated resolution would have ships arriving on the server and not on
  /// the client.
  static constexpr float ARRIVAL_TOLERANCE_METRES = 2.0f;

  /// One 40 km grid per session (ADR-001 §3). Targets are clamped into it on
  /// ingest, so no scripted or malformed order can send a ship into the part of
  /// float space where the resolution stops being millimetres.
  static constexpr float PLAY_AREA_HALF_EXTENT_METRES = 20000.0f;

  /*
   * Contact (ADR-015). Two ships are in contact when their centres are closer
   * than the sum of their class contact radii; steering avoids reaching that
   * state and `Separate` resolves whatever it could not avoid.
   *
   * Avoidance aims to pass traffic with this much of the combined radius to
   * spare. It must stay below sqrt(2): a Wedge's arms put formation neighbours
   * 2·sqrt(2) radii off each other's course, and a factor past that would have
   * a formation in cruise trying to avoid itself.
   */
  static constexpr float AVOID_CLEARANCE_FACTOR = 1.2f;

  /// How far ahead, in combined radii, a ship starts steering around traffic --
  /// on top of its own braking distance, which is speed- and class-dependent.
  static constexpr float AVOID_LOOKAHEAD_RADII = 4.0f;

  /*
   * Making way (ADR-021): a ship with nowhere to be steps out of the lane of
   * one that has somewhere to be, and flies home when the lane is clear.
   *
   * How far off the mover's course a berth is stood aside to, in combined
   * radii. **The same number is the trigger and the destination** -- a berth
   * further off than this is already clear, and a berth inside it is moved to
   * exactly this -- so the sidestep shrinks continuously to nothing at the
   * boundary and there is no edge for a ship to flicker across.
   *
   * It sits above `AVOID_CLEARANCE_FACTOR` on purpose: a ship that has made
   * room should end up far enough out that the mover flies *straight* past it
   * rather than still bending around what has already moved. And below sqrt(2)
   * for the reason that bound exists at all (ADR-015 §2) -- formation
   * neighbours sit 2*sqrt(2) radii off each other's course, and a wider
   * corridor would have a parked wing scattering itself the moment one of its
   * own members was ordered out of it.
   */
  static constexpr float MAKE_WAY_CLEARANCE_FACTOR = 1.35f;

  /*
   * How far ahead a berth starts clearing, as the mover's own travel time at
   * its class's **top** speed.
   *
   * Top speed rather than the speed it is actually doing, and that is the whole
   * point. The defect this answers is a mover brought to a *stop* by traffic; a
   * horizon measured from current speed would be zero at exactly the moment the
   * lane most needs clearing, and the two ships would sit nose to nose forever.
   * Class top speed is table data, so the horizon is a distance that cannot be
   * closed by the jam it exists to break.
   */
  static constexpr float MAKE_WAY_LOOKAHEAD_SECONDS = 6.0f;

  /// Relaxation passes for contact resolution. Chains of overlapped ships
  /// resolve across passes within the tick and across ticks after that; four is
  /// enough that avoidance-sized penetrations vanish the tick they appear.
  static constexpr std::uint32_t SEPARATION_PASSES = 4;

  /// The most of the combined radius one pass may push a pair apart by. A cap,
  /// because overlap can be authored (spawns) as well as flown into, and a deep
  /// overlap resolved in one step would read as a teleport on the client.
  static constexpr float SEPARATION_STEP_FACTOR = 0.25f;

  /// Empties the world and seeds it. The seed is part of the replay contract:
  /// same build, same seed, same orders, same state.
  void Reset(std::uint64_t _seed) noexcept;

  /*
   * Adds a ship **with the id it is given** (ADR-018 D6a), and returns it --
   * or `INVALID_SHIP_ID` if the class has no content (reserved classes are
   * nameable and never spawnable, ADR-009 §6) or the id is already in use.
   *
   * The world does not mint ids and must not: under the universe runtime a ship
   * keeps its id across grids, so an id space owned per-world would collide on
   * the first transfer -- which is exactly what it did, from zero, in every
   * world at once. Allocation belongs to the registry, and an authored
   * occupant's id is derived from its anchor so that spin-up, teardown and
   * recreate reproduce it bit-exactly.
   */
  [[nodiscard]] ShipId Spawn(const ShipSpawn& _spawn, ShipId _shipId);

  /*
   * Takes a ship out of the world **without destroying it** (ADR-017 §9).
   *
   * The transfer seam: the ship's id, class and wing come back so the roster --
   * or, later, the destination grid -- can hold the same ship rather than a new
   * one that looks like it. `Spawn` is the other half, and it already takes an
   * id for exactly this reason.
   *
   * False when the id is not here, which is a question worth being able to ask.
   */
  [[nodiscard]] bool TransferOut(ShipId _shipId, TransferMember& _outMember);

  /*
   * The transfers this world filed and has not handed over yet.
   *
   * A world files; the registry stamps and applies. It cannot do both, because
   * the ordering counter is the host's and a world minting its own would give
   * two grids the same number in the same tick (ADR-018 D17).
   */
  [[nodiscard]] std::span<const TransferRequest> FiledTransfers() const noexcept { return m_filed; }
  void ClearFiledTransfers() noexcept { m_filed.clear(); }

  /// Gives the world up, so another thread may take it (ADR-007 §7). Boot's
  /// Main-to-Sim hand-off is the sanctioned use and, today, the only one.
  void ReleaseOwner() noexcept;

  /*
   * Which anchor's grid this is, and which of its ships is the station
   * (ADR-016 §3, ADR-017 §2).
   *
   * The registry says this once, on spin-up. The world needs it because `Dock`
   * names an anchor and the validator has to answer "is that this grid's?" --
   * and it is an *id*, not a universe coordinate, so this stays on the right
   * side of ADR-009 §2's line: the tick still knows nothing about where in the
   * galaxy it is.
   *
   * `INVALID_SHIP_ID` for a grid with no station -- a gate, a planet, open
   * space -- which is what makes every Dock there `UnknownStation`.
   *
   * `_reachable` is where a `Warp` from here may go: **a list of ids**, copied
   * because it is content and never changes, and ids because the world may not
   * read a universe (ADR-009 §2). The registry knows both halves, so the
   * registry is what says this once, on spin-up.
   */
  void SetAnchor(AnchorId _anchor, ShipId _stationShip, std::span<const AnchorId> _reachable);

  /*
   * The gate on this grid, and where it leads (ADR-016 §5, §10, U4).
   *
   * Said separately from `SetAnchor` rather than as two more parameters,
   * because it is a different sentence: `SetAnchor` says which grid this is and
   * where a fleet may go from it, and this says that one of those destinations
   * is reached by crossing something a fleet has to be standing at. A grid is
   * anchored on one thing, so a gate's grid has exactly one gate and it leads
   * to exactly one other.
   *
   * Left unsaid on every other grid, which is what makes `jumpAnchor` invalid
   * there and every `Warp` from it judged the ordinary way.
   */
  void SetJump(AnchorId _jumpAnchor, ShipId _gateShip);

  /*
   * The economy's numbers, and the field on this grid (ADR-024 §4b, E2).
   *
   * Said by the registry at spin-up, exactly as `SetAnchor` is, and for the same
   * reason: the registry is what knows both halves. The economy outlives every
   * world -- it is read at boot and never changes -- so this keeps a pointer
   * rather than a copy of it, the way the registry keeps one to the universe.
   *
   * The **field** is a copy, and that is the durability rule rather than an
   * oversight. What is left of a site is universe-layer state (ADR-024 §3d): the
   * registry seeds this from bake plus ledger at spin-up, the tick eats it, the
   * debits travel the transfer bus back, and teardown throws this copy away
   * without losing anything. Worlds forget; ledgers do not.
   *
   * A grid with no field is left unsaid, which is what makes `Site().Exists()`
   * false there and every Mine ordered on it `NotAtSite`.
   */
  void SetEconomy(const EconomyDef* _economy) noexcept;
  void SetSite(const SiteField& _field, std::uint32_t _epoch) noexcept;

  [[nodiscard]] const SiteField& Site() const noexcept { return m_site; }

  /*
   * Which epoch's field this is, as an opaque number.
   *
   * The world is *told* it and never computes it -- what an epoch is, and when
   * one turns, is spin-up work and map work, and the CI rule that keeps that
   * arithmetic out of these three files is what makes the promise mechanical
   * rather than remembered.
   *
   * It exists so a filed yield can say which pool it came out of. A field is
   * never re-formed under a live grid, so a debit and the ledger it lands on
   * always agree -- and a record that carries the epoch is how that stays true
   * rather than being assumed by two pieces of code that cannot see each other.
   *
   * Identity, not state: it is not hashed, for the reason `m_anchor` is not.
   * What *is* hashed is the field itself, which is what an epoch decides.
   */
  [[nodiscard]] std::uint32_t FieldEpoch() const noexcept { return m_fieldEpoch; }

  [[nodiscard]] AnchorId Anchor() const noexcept { return m_anchor; }
  [[nodiscard]] ShipId StationShip() const noexcept { return m_stationShip; }
  [[nodiscard]] AnchorId JumpAnchor() const noexcept { return m_jumpAnchor; }

  /*
   * A move the world issues to itself (ADR-017 §4).
   *
   * The same path a player's order takes -- validated, queued, ingested,
   * solved -- with one difference: it does not end undock protection. The
   * parking order arrives in the same tick the fleet does, and a fleet that
   * lost its protection to the order that parks it would have none at all.
   */
  [[nodiscard]] OrderVerdict SubmitSystemOrder(const OrderSubmit& _order);

  /*
   * Where a just-undocked fleet should park itself (ADR-017 §4).
   *
   * Scans the parking ring's 24 candidates in the fixed order §4 states and
   * returns the first **free** one -- free meaning the fleet's *solved*
   * formation there clears every hull on the grid by the avoidance model's own
   * clearance, and lands inside no other group's final-leg intention.
   *
   * That second clause is what makes two fleets undocking on the same tick pick
   * different berths **with no reserved-berth state to store or hash**: a berth
   * is taken exactly when live positions or live intentions say so. It is also
   * why this reads the group table rather than a reservation list.
   *
   * False means all 24 are taken, which is **not** a refusal: the fleet holds
   * at the undock point with its protection still ticking and separation
   * keeping it honest. Undocking is never refused for clutter.
   */
  [[nodiscard]] bool FindBerth(std::span<const ShipId> _ships, FormationId _formation,
                               const DirectX::XMFLOAT2& _stationCentreMetres,
                               const DirectX::XMFLOAT2& _undockPointMetres, DirectX::XMFLOAT2& _outBerthMetres,
                               float& _outFacingRadians) const;

  /*
   * Where this fleet can actually form up, given where it was asked to
   * (ADR-026 -- **solve, then slide**).
   *
   * The point the player named is tried **first**, so a destination with room
   * in it never moves and this costs one predicate evaluation. If the solved
   * formation lands in a hull, a gate or another fleet's intention, the *whole*
   * formation slides to the nearest free placement: shape and facing are
   * preserved and only the anchor moves. A dented Claw is not what the player
   * composed.
   *
   * The candidate pattern is the parking ring's shape and not its numbers.
   * `FindBerth` fans around a *station*, at radii a station's doorway
   * justifies; this fans around a *player's click*, at radii derived from the
   * formation's own extent -- what "near enough to where I pointed" means
   * scales with the fleet, and one number cannot serve a three-Interceptor wing
   * and a sixty-ship line.
   *
   * Bearings fan from `_approachBearingRadians`, the direction the fleet is
   * coming from, so a fleet blocked by something in its path stops **short** of
   * it. Arriving beyond the obstruction reads as the fleet ignoring the order.
   *
   * False means all 24 candidates are taken, and it is **not** a refusal: the
   * caller flies to the asked point anyway and ADR-015 parks the fleet at
   * contact range while the leg expires by its deadline (ADR-026 §4). An order
   * never wedges, and it is never bounced for clutter.
   */
  [[nodiscard]] bool FindClearPlacement(std::span<const ShipId> _ships, FormationId _formation, float _facingRadians,
                                        const DirectX::XMFLOAT2& _askedMetres, float _approachBearingRadians,
                                        std::uint32_t _ignoreServerOrderId, DirectX::XMFLOAT2& _outAnchorMetres) const;

  /// Is this ship still under undock protection at `_tick` (ADR-017 §5)? False
  /// for an unknown ship, which is the same answer as an unprotected one --
  /// the question is about immunity, not existence.
  [[nodiscard]] bool IsProtected(ShipId _shipId, std::uint32_t _tick) const noexcept;

  /// Removes a ship. Returns false if the id is not present, which is a
  /// question worth being able to ask rather than an error worth asserting.
  bool Despawn(ShipId _shipId);

  /*
   * How long a leg may wait for its slowest member (ADR-005 §2).
   *
   * **This was a flat 600 ticks and the flat number was wrong** (S12). Thirty
   * seconds was documented as "long enough that a Battleship crossing the grid
   * is never cut short"; a Battleship crossing the grid takes 182 seconds, and
   * even four kilometres takes 45. Every leg longer than half a minute was
   * *timing out* rather than completing. Nothing looked broken, because
   * `Guidance` still held the station and the ships flew on -- what the timeout
   * actually ended was the **order**, so the footprint vanished mid-flight and
   * the fleet's ETA with it. With a queue it is worse than cosmetic: leg two
   * would begin while leg one was a sixth flown, and the fleet would skip to
   * the last waypoint.
   *
   * So the deadline is now relative to the leg's own expected duration, which
   * S12 made computable (`LegEtaSeconds`). Three times the estimate plus a
   * grace: the estimate is optimistic by at most a third even on a full
   * reversal, so three times it is generous without being unbounded, and a
   * straggler still frees the fleet in a time proportional to the journey it
   * was asked to make rather than a constant that fits no journey.
   */
  static constexpr float LEG_TIMEOUT_FACTOR = 3.0f;

  /// Added on top, so a leg with a near-zero estimate -- a station-keeping
  /// nudge -- still gets a moment rather than a deadline of now.
  static constexpr std::uint32_t LEG_TIMEOUT_GRACE_TICKS = 200;

  /// The ceiling, for a leg whose estimate is refused (a group of stations).
  /// Twenty minutes: past any journey this grid affords, and still not forever.
  static constexpr std::uint32_t LEG_TIMEOUT_MAX_TICKS = 24000;

  /*
   * How long a finished group stays in the table before it is erased.
   *
   * Finished groups used to stay **forever** -- nothing erased a group that
   * still had members -- and the leak was not abstract: the snapshot reports
   * the first `MAX_ORDERS_PER_SNAPSHOT` groups in table order, so sixteen
   * completed orders were enough to crowd every *live* order out of the wire
   * for the rest of the session. The ack still arrived and the ships still
   * flew; what died was the feedback -- no state, no ETA, no `Done` -- which
   * the client's ghost machinery reads as "decided and gone".
   *
   * Why not erase on the tick it finishes: the ghost's designed retirement is
   * *seeing* `Done` in an order record. Thirty ticks is thirty snapshots'
   * worth of chances for that record to land under loss, for the cost of a
   * bounded tail -- the table now holds at most one live group per ordered
   * ship plus whatever finished in the last second and a half.
   */
  static constexpr std::uint32_t ORDER_DONE_LINGER_TICKS = 30;

  /*
   * Validates an order, and queues it for the next tick if it passes.
   *
   * Returns the verdict the server acks with, including the `serverOrderId` an
   * accepted order was assigned (ADR-004 §7). The verdict is available
   * immediately and the state change is not: validation is pure and can answer
   * now, but a world that mutated between ticks would have no tick to attribute
   * the mutation to, and replay would have nothing to reproduce.
   */
  [[nodiscard]] OrderVerdict SubmitOrder(const OrderSubmit& _order);

  /*
   * One fixed step: IngestOrders -> GroupAdvance -> Steering -> Integrate ->
   * Separate.
   *
   * The order is named and fixed (ADR-005 §2; Separate joined it in ADR-015).
   * `EmitSnapshot` is not a step here -- `WriteSnapshot` reads the finished
   * state from outside, which keeps the world from knowing what a datagram is.
   *
   * Orders are not a parameter: they arrive through `SubmitOrder` between ticks
   * and wait in world state, so a replay reproduces the queue rather than
   * needing the harness to remember what was in flight.
   */
  void Tick(std::uint32_t _tick);

  [[nodiscard]] std::uint32_t Tick() const noexcept { return m_tick; }
  [[nodiscard]] std::uint32_t ShipCount() const noexcept { return static_cast<std::uint32_t>(m_ids.size()); }

  /// Slot for an id, or false. Ids are stable for a ship's life; slots are not,
  /// because removal is swap-and-pop and dense arrays are the point.
  [[nodiscard]] bool FindSlot(ShipId _shipId, std::uint32_t& _outSlot) const noexcept;

  /*
   * The tables, read-only.
   *
   * Parallel arrays rather than an array of structs, and exposed as spans so a
   * caller can walk one field over every ship without touching the others --
   * which is what `EmitSnapshot`, `WorldHash` and every test here actually do.
   */
  [[nodiscard]] std::span<const ShipId> Ids() const noexcept { return m_ids; }
  [[nodiscard]] std::span<const std::uint8_t> Classes() const noexcept { return m_classes; }
  [[nodiscard]] std::span<const WingId> Wings() const noexcept { return m_wings; }
  [[nodiscard]] std::span<const DirectX::XMFLOAT2> Positions() const noexcept { return m_positions; }
  [[nodiscard]] std::span<const DirectX::XMFLOAT2> Velocities() const noexcept { return m_velocities; }
  [[nodiscard]] std::span<const float> Headings() const noexcept { return m_headings; }
  [[nodiscard]] std::span<const Guidance> Guidances() const noexcept { return m_guidances; }
  [[nodiscard]] std::span<const std::uint8_t> Hulls() const noexcept { return m_hulls; }
  [[nodiscard]] std::span<const std::uint8_t> Shields() const noexcept { return m_shields; }
  [[nodiscard]] std::span<const std::uint32_t> ProtectedUntil() const noexcept { return m_protectedUntil; }

  /// What every ship is carrying, and where every ship is in its cycle. Read by
  /// `WorldHash` and by the summaries E3 puts on the wire.
  [[nodiscard]] std::span<const ShipCargo> Cargo() const noexcept { return m_cargo; }
  [[nodiscard]] std::span<const MiningState> MiningStates() const noexcept { return m_mining; }

  /*
   * Room left in a ship's ore hold, in litres, and zero for a hull with no ore
   * hold at all.
   *
   * Derived from the class table and the manifest rather than stored, so a hold
   * cannot drift out of step with what is in it. It is what `Validation` fills
   * `oreHoldFreeLitres` from and what a cycle's yield is capped by, which is
   * the point of there being one answer.
   */
  [[nodiscard]] std::uint32_t OreHoldFreeLitres(std::uint32_t _slot) const noexcept;

  /*
   * How far anything on this grid can see, as a whole percentage of its own
   * range (ADR-024 §3b). 100 everywhere except inside a nebula pocket.
   *
   * A property of the *grid*, not of a ship: the pocket dampens everyone in it,
   * which is what makes the highest-value ore sit where the hunter cannot be
   * seen coming and the hunter cannot see either. ADR-022's relevance seam is
   * what will read this; it is exposed now, beside the field that decides it,
   * so that seam finds a number rather than inventing one.
   */
  [[nodiscard]] std::uint8_t SensorDampingPct() const noexcept;

  /// The accepted orders, and what they are doing. Read by `WriteSnapshot` for
  /// the order-state records that promote a client's ghost (ADR-004 §6).
  [[nodiscard]] std::span<const OrderGroup> Groups() const noexcept { return m_groups; }

  /*
   * Seconds until this group finishes the leg it is on, or a negative number
   * when it cannot say (S12).
   *
   * The authority's own answer, replicated in the order state so the HUD is
   * reading a fact rather than the prediction its pre-check made. It is the
   * **slowest member's remaining journey to its own station** -- a leg completes
   * when every member is inside tolerance (ADR-005 §2), so the arrival that
   * matters is the last one.
   *
   * **Derived, never stored.** A field on `OrderGroup` would be a float in the
   * world's state and therefore in `WorldHash`, and two builds whose class
   * tables differed by an ulp would diverge on a number nothing simulates. It
   * is recomputed when a snapshot asks, which is twenty times a second over at
   * most sixteen groups.
   */
  [[nodiscard]] float LegEtaSeconds(const OrderGroup& _group) const noexcept;

  /// Orders accepted but not yet ingested. Non-zero only between a `SubmitOrder`
  /// and the next `Tick`.
  [[nodiscard]] std::uint32_t PendingOrderCount() const noexcept { return static_cast<std::uint32_t>(m_pending.size()); }

  /// The view `ValidateOrder` runs against, built from this world's own tables.
  /// Exposed because the server pre-checks with it and a test asserts parity
  /// against the client's (ADR-005 §4).
  /*
   * The view `ValidateOrder` reads, over this world's ships.
   *
   * **Non-const, and the span it hands back is borrowed.** A `ShipMark` is
   * wire-quantised and the world stores metres, so the marks have to be
   * materialised somewhere; they live in a scratch table here rather than in a
   * temporary the returned span would outlive. The next call overwrites it,
   * which is the same borrow-never-hold rule the registry states for worlds.
   */
  [[nodiscard]] ValidationView Validation();

  /// The RNG's state, for the world hash. Exposed rather than hashed here so
  /// `WorldHash` stays the single place that decides what "the state" means.
  [[nodiscard]] const Neuron::Pcg32& Random() const noexcept { return m_random; }

  /// Scalar speed along the heading. Convenience for tests and the HUD; the
  /// stored velocity is the authority and this is derived from it.
  [[nodiscard]] float SpeedAt(std::uint32_t _slot) const noexcept;

private:
  void IngestOrders();
  void GroupAdvance();
  void Steering();
  void Integrate();
  void Separate();

  /*
   * Mining cycles, hazards and the three exits (ADR-024 §4b), last in the tick.
   *
   * **Last, and deliberately.** A cycle completes against where the ship
   * actually is at the end of the tick -- after steering, integration and
   * contact have all had their say -- so "inside the field" is a fact about the
   * finished tick rather than about a position two systems were still arguing
   * over. It also means a group this step sends to `Done` is retired by the
   * *next* tick's `GroupAdvance`, which is exactly how a completed leg already
   * behaves.
   */
  void Mining();

  /// Seconds until a working Mine order ends -- the cluster running dry or the
  /// last Miner filling, whichever comes first. `LegEtaSeconds`'s answer for a
  /// group that has no leg left to fly.
  [[nodiscard]] float MiningEtaSeconds(const OrderGroup& _group) const noexcept;

  /// Sends off the warps whose spool has run out (U3a). Run at the top of
  /// `GroupAdvance`, because a fleet that has left is not a fleet with a leg.
  void DepartFinishedWarps();

  /// Solves the current leg's stations and writes them into the members'
  /// guidance. The one place a group touches a ship.
  void ApplyLeg(OrderGroup& _group);

  /*
   * The freedom test both placement searches share (ADR-026 §2). See its
   * definition in `WorldOrders.cpp` for what "free" means and why the second
   * clause reads the pending queue as well as the live groups.
   */
  [[nodiscard]] bool IsPlacementFree(std::span<const FormationStation> _stations, HullClass (*_hullClassOf)(ShipId, void*),
                                     void* _context, std::span<const ShipId> _exclude,
                                     const DirectX::XMFLOAT2& _candidateMetres, float _boundingMetres,
                                     std::uint32_t _ignoreServerOrderId, bool _forMovePlacement) const noexcept;

  /*
   * Is this ship going somewhere? (ADR-026 §2.)
   *
   * Membership of a live group or a pending order, **not** `GuidanceMode` --
   * two fleets ordered to swap places are accepted in the same batch, so at
   * the moment the first is placed the second has not been given its guidance
   * yet and still reads as idle. Orders are the thing that is already true by
   * then.
   */
  [[nodiscard]] bool ShipIsUnderOrders(ShipId _shipId) const noexcept;

  /// Both `SubmitOrder` and `SubmitSystemOrder`, which differ by one flag and
  /// must not differ by anything else -- a system order that took a shortcut
  /// through validation would be the one order the parity contract does not
  /// cover.
  [[nodiscard]] OrderVerdict Submit(const OrderSubmit& _order, bool _systemIssued);

  /// The group this ship currently belongs to, or null. A ship is in at most
  /// one group -- `ForgetShipInGroups` on ingest is what makes that true -- so
  /// there is exactly one answer. The pointer is invalidated by anything that
  /// grows or erases `m_groups`, so callers use it and drop it.
  [[nodiscard]] OrderGroup* FindGroupOf(ShipId _shipId) noexcept;

  /// Removes a ship from whichever group holds it, and erases the group if
  /// that was its last member -- an order cannot outlive its last member.
  /// Called on despawn and when a Replace takes the members elsewhere.
  void ForgetShipInGroups(ShipId _shipId) noexcept;

  std::uint32_t m_tick = 0;

  /// Index is `ShipId`, value is the slot or `INVALID_SHIP_ID`. A flat table
  /// rather than a map: it is O(1), it has no iteration order to get wrong, and
  /// a pointer-keyed container is exactly what ADR-005 §5 forbids.
  std::vector<ShipId> m_slotById;
  /*
   * Single-writer enforcement (ADR-007 §7), debug-only and never hashed: an
   * owner id that reached the world hash would make a replay depend on which
   * thread ran it.
   */
  Neuron::OwnerThread m_owner;

  /// Which anchor's grid this is, and its station if it has one. Set once on
  /// spin-up and never hashed: they are identity, not state, and a world that
  /// folded its own address into its hash could not be compared against its
  /// recreation on another host.
  AnchorId m_anchor = INVALID_ID;
  ShipId m_stationShip = INVALID_SHIP_ID;

  /// The gate's grid, if this is one: where it leads, and which ship is the
  /// structure a jump has to be ordered from. Identity like the two above it,
  /// set once on spin-up and never hashed.
  AnchorId m_jumpAnchor = INVALID_ID;
  ShipId m_gateShip = INVALID_SHIP_ID;

  /// Where a warp from this grid may go. Content, set once, never hashed --
  /// it is the same list in every run of the same universe.
  std::vector<AnchorId> m_reachable;

  std::vector<ShipId> m_ids;
  std::vector<std::uint8_t> m_classes;
  std::vector<WingId> m_wings;
  std::vector<DirectX::XMFLOAT2> m_positions;
  std::vector<DirectX::XMFLOAT2> m_velocities;
  std::vector<float> m_headings;
  std::vector<Guidance> m_guidances;
  std::vector<std::uint8_t> m_hulls;
  std::vector<std::uint8_t> m_shields;

  /*
   * The economy's numbers and this grid's field (ADR-024 §4b).
   *
   * The pointer is content and is never hashed -- it is the same table in every
   * run. The field *is* hashed, through `WorldHash`, because what is left of it
   * is simulation state that the next cycle reads.
   */
  const EconomyDef* m_economy = nullptr;
  SiteField m_site;
  std::uint32_t m_fieldEpoch = 0;

  /// Per slot, what it carries and where it is in its cycle. Parallel to
  /// `m_ids` like every other table here, and swapped and popped with them.
  std::vector<ShipCargo> m_cargo;
  std::vector<MiningState> m_mining;

  /// Undock protection, per slot (ADR-017 §5). Zero means unprotected, which is
  /// every ship that did not arrive out of a station. Hashed like every other
  /// per-ship field: it is simulation state, and a replay that disagreed about
  /// who was protected would disagree about the fight that follows.
  std::vector<std::uint32_t> m_protectedUntil;

  std::vector<OrderGroup> m_groups;

  /// Accepted, waiting for the next tick to ingest. World state, so a replay
  /// reproduces orders that were in flight across a tick boundary.
  std::vector<PendingOrder> m_pending;

  /// Filed this tick, drained by the registry between ticks. World state until
  /// it is drained -- a replay that lost a filed transfer would lose a dock.
  std::vector<TransferRequest> m_filed;

  /// Scratch for `Validation()`, not state: the quantised mirror of
  /// `m_positions` and `m_classes`, rebuilt on demand and hashed by nothing.
  /// A member so the span it is handed out as has somewhere to live.
  std::vector<ShipMark> m_validationMarks;

  /// And the room left in each ship's ore hold, for the same reason and on the
  /// same terms: derived on demand, borrowed by the view, hashed by nothing.
  std::vector<std::uint32_t> m_validationOreRoom;

  /*
   * Zero means "no order" in the verdict.
   *
   * `m_lastOrderSeqProcessed` used to sit beside this and **left with U3d-a**
   * (ADR-022 §7). It was per-session state living in shared state: world-global,
   * written as a max across every submitter and folded into `WorldHash`. With
   * one commander that was invisible; with two it is wrong twice over -- one
   * player's order sequence perturbs the other's feedback loop, and a replay's
   * hash depends on which client happened to submit. It is the session's now
   * (`SnapshotSender`), the wire field did not move, and `WriteSnapshot` takes
   * the number rather than reading it off a world that has no viewers.
   */
  std::uint32_t m_nextOrderId = 1;

  /// In world state, not a global, because a replay has to reproduce it
  /// (ADR-005 §5). Unused by S6's movement -- steering is deterministic and
  /// wants no noise -- and carried anyway so the replay contract covers it from
  /// the first slice rather than from the first slice that needs it.
  Neuron::Pcg32 m_random;
};

} // namespace Game
