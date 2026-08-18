#pragma once

#include "Ids.h"
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
 * One scripted move, and the whole of S6's order vocabulary.
 *
 * Not `OrderSubmit` (ADR-005 §1): there is no queue, no formation, no
 * validation and no server order id here, and naming it as though there were
 * would claim four things this slice does not do. S9 brings the real type and
 * the validation behind it; this is what the replay harness feeds the world in
 * the meantime, and what proves the tick pipeline is order-driven rather than
 * scripted inside itself.
 */
struct ScriptedMove
{
  const ShipId* shipIds = nullptr;
  std::uint32_t shipCount = 0;

  float targetXMetres = 0.0f;
  float targetYMetres = 0.0f;
  float arrivalFacingRadians = 0.0f;

  /// Stop where you are, ignoring the target. The one order a ship can always
  /// be given, and the reason `GuidanceMode::Hold` is the default rather than a
  /// state a ship falls into by accident.
  bool hold = false;
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
   * One fixed step: IngestOrders -> Steering -> Integrate.
   *
   * The order is named and fixed (ADR-005 §2). `GroupAdvance` belongs between
   * ingest and steering and arrives with S10; `EmitSnapshot` belongs after
   * integrate and arrives with S7. Both are absent rather than stubbed, so the
   * pipeline reads as what it does.
   */
  void Tick(std::uint32_t _tick, std::span<const ScriptedMove> _moves);

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

  /// The RNG's state, for the world hash. Exposed rather than hashed here so
  /// `WorldHash` stays the single place that decides what "the state" means.
  [[nodiscard]] const Neuron::Pcg32& Random() const noexcept { return m_random; }

  /// Scalar speed along the heading. Convenience for tests and the HUD; the
  /// stored velocity is the authority and this is derived from it.
  [[nodiscard]] float SpeedAt(std::uint32_t _slot) const noexcept;

private:
  void IngestOrders(std::span<const ScriptedMove> _moves);
  void Steering();
  void Integrate();

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

  /// In world state, not a global, because a replay has to reproduce it
  /// (ADR-005 §5). Unused by S6's movement -- steering is deterministic and
  /// wants no noise -- and carried anyway so the replay contract covers it from
  /// the first slice rather than from the first slice that needs it.
  Neuron::Pcg32 m_random;
};

} // namespace Game
