#pragma once

#include <cstdint>
#include <string_view>

/*
 * The ship class registry (ADR-005 §1).
 *
 * A compiled-in table, not content. Class parameters are the game's balance and
 * the simulation's determinism depends on them being identical on both halves
 * of a session -- authoring them in JSON would put the movement envelope behind
 * a file the two sides could disagree about, and the schema hash would have to
 * grow to cover it. When balance wants to be data (post-MVP), it arrives with a
 * hash, not before.
 *
 * **The taxonomy is closed at eleven and ordered by the icon sheet**
 * (`tactical-icon-system.png`, ADR-009 §6). Two of them -- `Fighter` and
 * `Cruiser` -- have no mesh in `GameData/Meshes` and no content behind them.
 * They keep their ids anyway: the enum is a wire value, an icon index and a
 * palette index, and inserting a class later would renumber every one of those
 * at once. A reserved id costs a row in a table; a renumber costs a migration.
 *
 * Note that a `HullClass` is **not** a render classId. The renderer's ids are
 * the order of the mesh list in `Outpost.json`, smallest hull to largest, and
 * the engine has no idea what any of them are (ADR-014). Mapping one to the
 * other is the game's job and happens where the two meet.
 */

namespace Game
{

enum class HullClass : std::uint8_t
{
  Interceptor = 0,
  Fighter = 1, // Reserved: no mesh, no content.
  Bomber = 2,
  Corvette = 3,
  Frigate = 4,
  Cruiser = 5, // Reserved: no mesh, no content.
  Battleship = 6,
  Carrier = 7,
  Hauler = 8,
  Miner = 9,
  Structure = 10
};

inline constexpr std::uint8_t HULL_CLASS_COUNT = 11;

/*
 * What the simulation needs to move one of these and keep two of them apart
 * (ADR-015), and what the client needs to point at and draw one.
 *
 * Every rate is per second and every distance is metres, because the tick is an
 * implementation detail of `Integrate` and a table written in per-tick units
 * would silently rebalance the game if the tick rate ever changed (ADR-002 §1).
 */
struct ShipClassInfo
{
  std::string_view name;

  float maxSpeedMetresPerSec = 0.0f;
  float accelMetresPerSecSq = 0.0f;

  /// Heading rate limit. With no strafing (ADR-005 §2) this and the speed are
  /// what set a hull's turn radius: v / omega, so a Battleship at full speed
  /// sweeps a wide arc and an Interceptor pivots.
  float turnRateRadiansPerSec = 0.0f;

  /// Click tolerance, in metres on the plane (ADR-006 §11). Larger than the
  /// hull so a moving ship is still clickable, and it is the *class's* number
  /// rather than the mesh's so picking does not change when art does.
  float pickRadiusMetres = 0.0f;

  /// Distance between neighbouring stations when this is the largest class in
  /// a group. The formation solve reads it (S10); nothing else should.
  float formationSpacingMetres = 0.0f;

  /// The hull's footprint on the plane: two ships are in contact when their
  /// centres are closer than the sum of these (ADR-015). Smaller than the pick
  /// radius -- picking is forgiving on purpose, contact must not be -- and at
  /// most a quarter of the formation spacing, so ships parked on adjacent
  /// stations are never in contact. A test holds both bounds.
  float collisionRadiusMetres = 0.0f;

  /// Cosmetic hover height above the plane (ADR-001 §2, S14), in metres.
  /// Presentation only, like `pickRadiusMetres`: nothing in `Tick` may read it,
  /// it is never replicated, and the selection ring -- and since ADR-015 the
  /// contact test too -- stays on the plane beneath the hull.
  float hoverMetres = 0.0f;

  /// False for the two reserved ids. A class with no content can still be named
  /// and still occupies its wire value; it simply must never be spawned.
  bool hasContent = true;
};

/*
 * The table. Indexed by `HullClass`, in enum order, and asserted to be so.
 *
 * `Structure` is in here with zero speed and zero acceleration on purpose: a
 * station is a ship-table entry that never moves, which keeps one movement
 * path rather than two and means a station can be selected, targeted and
 * replicated by the same code as everything else.
 */
[[nodiscard]] const ShipClassInfo& ShipClass(HullClass _hullClass) noexcept;

/// Bounds-checked lookup for values that came off the wire or out of a file.
/// Returns false rather than clamping: an unknown class is a mismatch to refuse
/// at the door, not a ship to spawn as something else.
[[nodiscard]] bool TryShipClass(std::uint8_t _rawClass, HullClass& _outClass) noexcept;

[[nodiscard]] std::string_view HullClassName(HullClass _hullClass) noexcept;

/// The two reserved ids, asked rather than remembered.
[[nodiscard]] bool HullClassHasContent(HullClass _hullClass) noexcept;

/*
 * Cosmetic banking (ADR-001 §2, ADR-006 §6): how far a hull rolls into the turn
 * it is observed making, in radians. Positive drops the starboard wing, which
 * is the roll a turn to starboard (heading rate < 0, CCW-positive) earns.
 *
 * Computed from replicated quantities only -- the heading *rate* the client
 * measures between two snapshots, and the sampled speed -- because the sim has
 * no roll to replicate: velocity is always along heading (the no-strafing
 * rule), so a slip angle is identically zero and the turn itself is the only
 * signal there is. Full bank is a hull turning at its class's full rate at its
 * class's full speed; a ship pivoting on the spot barely tips, because what
 * banking depicts is lateral acceleration and at rest there is none.
 *
 * Pure and clamped, so a wild rate off a corrupted sample cannot roll a ship
 * onto its back. A class that cannot turn (Structure) banks zero.
 */
[[nodiscard]] float CosmeticBankRadians(HullClass _hullClass, float _headingRateRadiansPerSec,
                                        float _speedMetresPerSec) noexcept;

/// The clamp `CosmeticBankRadians` promises: no hull ever rolls past this.
inline constexpr float MAX_COSMETIC_BANK_RADIANS = 0.42f;

} // namespace Game
