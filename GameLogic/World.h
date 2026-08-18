#pragma once

#include "Ids.h"
#include "Orders.h"
#include "Validate.h"
#include "ShipClass.h"

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

  std::uint16_t memberCount = 0;
  ShipId members[MAX_SHIPS_PER_ORDER] = {};

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
};

/// What a ship looks like at the moment it enters the world.
struct ShipSpawn
{
  HullClass hullClass = HullClass::Interceptor;
  WingId wing = INVALID_WING_ID;
  float xMetres = 0.0f;
  float yMetres = 0.0f;
  float headingRadians = 0.0f;
};

class World
{
public:
  /// Fixed step (ADR-005 §2), matching the 20 Hz server tick (ADR-002 §1).
  /// A constant rather than a parameter: a variable step is the shortest path
  /// to a simulation that cannot replay.
  static constexpr float TICK_SECONDS = 0.05f;

  /// How close counts as arrived. Set against the wire's centimetre
  /// quantisation (ADR-004) with room to spare: a tolerance below the
  /// replicated resolution would have ships arriving on the server and not on
  /// the client.
  static constexpr float ARRIVAL_TOLERANCE_METRES = 2.0f;

  /// One 40 km grid per session (ADR-001 §3). Targets are clamped into it on
  /// ingest, so no scripted or malformed order can send a ship into the part of
  /// float space where the resolution stops being millimetres.
  static constexpr float PLAY_AREA_HALF_EXTENT_METRES = 20000.0f;

  /// Empties the world and seeds it. The seed is part of the replay contract:
  /// same build, same seed, same orders, same state.
  void Reset(std::uint64_t _seed) noexcept;

  /// Adds a ship and returns its id, or `INVALID_SHIP_ID` if the class has no
  /// content. Reserved classes are nameable and never spawnable (ADR-009 §6).
  [[nodiscard]] ShipId Spawn(const ShipSpawn& _spawn);

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
   * One fixed step: IngestOrders -> GroupAdvance -> Steering -> Integrate.
   *
   * The order is named and fixed (ADR-005 §2). `EmitSnapshot` is not a step
   * here -- `WriteSnapshot` reads the finished state from outside, which keeps
   * the world from knowing what a datagram is.
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

  /// The accepted orders, and what they are doing. Read by `WriteSnapshot` for
  /// the order-state records that promote a client's ghost (ADR-004 §6).
  [[nodiscard]] std::span<const OrderGroup> Groups() const noexcept { return m_groups; }

  /// The highest client sequence this world has ingested, for the snapshot
  /// header. Closes the feedback loop when an `OrderAck` is lost.
  [[nodiscard]] std::uint32_t LastOrderSeqProcessed() const noexcept { return m_lastOrderSeqProcessed; }

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
  [[nodiscard]] ValidationView Validation() const noexcept;

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

  /// Solves the current leg's stations and writes them into the members'
  /// guidance. The one place a group touches a ship.
  void ApplyLeg(OrderGroup& _group);

  /// Drops any group a ship still belongs to, so an order cannot outlive its
  /// last member. Called on despawn.
  void ForgetShipInGroups(ShipId _shipId) noexcept;

  std::uint32_t m_tick = 0;

  /// Index is `ShipId`, value is the slot or `INVALID_SHIP_ID`. A flat table
  /// rather than a map: it is O(1), it has no iteration order to get wrong, and
  /// a pointer-keyed container is exactly what ADR-005 §5 forbids.
  std::vector<ShipId> m_slotById;
  ShipId m_nextShipId = 0;

  std::vector<ShipId> m_ids;
  std::vector<std::uint8_t> m_classes;
  std::vector<WingId> m_wings;
  std::vector<DirectX::XMFLOAT2> m_positions;
  std::vector<DirectX::XMFLOAT2> m_velocities;
  std::vector<float> m_headings;
  std::vector<Guidance> m_guidances;
  std::vector<std::uint8_t> m_hulls;
  std::vector<std::uint8_t> m_shields;

  std::vector<OrderGroup> m_groups;

  /// Accepted, waiting for the next tick to ingest. World state, so a replay
  /// reproduces orders that were in flight across a tick boundary.
  std::vector<PendingOrder> m_pending;

  std::uint32_t m_nextOrderId = 1; // Zero means "no order" in the verdict.
  std::uint32_t m_lastOrderSeqProcessed = 0;

  /// In world state, not a global, because a replay has to reproduce it
  /// (ADR-005 §5). Unused by S6's movement -- steering is deterministic and
  /// wants no noise -- and carried anyway so the replay contract covers it from
  /// the first slice rather than from the first slice that needs it.
  Neuron::Pcg32 m_random;
};

} // namespace Game
