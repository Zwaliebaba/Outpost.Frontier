#include "pch.h"

#include "ShipClass.h"

#include <algorithm>
#include <array>

namespace Game
{
namespace
{

/*
 * The registry, in enum order.
 *
 * The numbers are a first pass at the movement envelope the prints imply: an
 * Interceptor that crosses the tactical grid in well under a minute and turns
 * almost on the spot, a Battleship that takes its time doing either, and the
 * support hulls in between. They are balance rather than physics, and the
 * movement-envelope suite asserts the *shape* they have to keep -- speed rises
 * with mass, turn rate falls with it -- rather than the values themselves, so
 * retuning does not mean rewriting tests.
 *
 * `formationSpacing` is roughly four hull radii: close enough to read as a
 * formation on the print, far enough that stations never overlap -- which is
 * what let MVP steering skip inter-ship avoidance, and is now what keeps a
 * formation in flight from fighting its own contact model (ADR-015 §3).
 *
 * `warpSpeed` and `spool` are U3a's, and they scale the same way: a capital
 * spools longer and travels slower, so a fleet's transit is set by its heaviest
 * member. The magnitudes are astronomical because the distances are.
 *
 * **Retuned x10 on 2026-08-23, from a playtest rather than from arithmetic.**
 * They were set so that 1.5e9 m/s crossed an astronomical unit in about a
 * hundred seconds -- *"the order the strategic map's 'a few minutes across a
 * system' implies"* -- and a few minutes turns out to be the wrong order for
 * the thing it is actually spent on. An ordinary hop between two anchors in one
 * system was reading **2:01** on the roster block, which is two minutes of a
 * fleet the player cannot command and a screen with nothing happening on it.
 * Ten times faster puts the same hop near twenty seconds: long enough that the
 * decision to go somewhere costs something, short enough to stay watching it.
 *
 * **The two fixed costs are deliberately untouched**, and they are what stops
 * this from making distance meaningless: `WARP_BASE_SECONDS` still charges five
 * seconds for any crossing at all, and `spoolSeconds` below still makes a
 * capital take longer to get moving than an interceptor. What was retuned is
 * the *distance* term alone -- the part that was pricing a system's width in
 * minutes.
 *
 * `GATE_JUMP_TICKS` is also untouched: a jump is flat by design (`Transfer.h`)
 * and priced against the route planner's own print, so it is a separate number
 * with a separate reason and is not "warp" retuned by another name.
 *
 * `collisionRadius` is that same hull radius made explicit: a quarter of the
 * spacing, rounded *down* to whole metres -- the table test holds the quarter
 * as a ceiling, because it is what guarantees hulls parked on adjacent
 * stations clear water. `Structure` has no spacing to derive from
 * -- it never flies in formation -- so its radius is set against its pick
 * radius instead, at the same "pick is wider than the hull" proportion the
 * capital ships carry.
 *
 * `Gate` is the twelfth row (ADR-016 §10, U4), and it is a `Structure`'s
 * reasoning at the size the art turned out to be. It never moves, so speed,
 * turn, spacing, warp and hover are all zero; it is picked and it has a
 * footprint, so those two are not.
 *
 * Both of those come off `Stargate.obj` rather than being guessed, the same way
 * the Structure's do: the ring's silhouette on the plane is 168 m of radius
 * against the station's 253, so picking rounds up to 175 (the station rounds
 * 253 up to 260) and contact sits at the same proportion under the silhouette
 * the station keeps -- 135 m, where 200 of 253 is the station's. That order
 * matters: a contact radius chosen before the mesh existed would have been a
 * number the art then had to live with.
 *
 * It clears the bake either way. A gate's warp-in point is 1,200 m out
 * (`GATE_WARP_IN_STANDOFF_METRES`), so a fleet arriving to make a jump lands
 * nowhere near the structure, and the whole ring sits well inside
 * `JUMP_RADIUS_METRES`.
 */
// Hover heights (the second-to-last column) are cosmetic (ADR-001 §2, S14): a
// small per-class lift so a hull reads as riding above the plane its ring lies
// on, scaled roughly with the hull. The Structure sits on the plane -- a
// station that hovered would read as a ship. Hover and contact never meet:
// the contact test runs on the plane and the lift is presentation only.
constexpr std::array<ShipClassInfo, HULL_CLASS_COUNT> CLASS_TABLE = {{
    // name           maxSpeed  accel  turnRate  pickRadius  spacing  collision  warpSpeed  spool  hover  content
    {"Interceptor", 320.0f, 90.0f, 1.60f, 45.0f, 70.0f, 17.0f, 2.4e10f, 4.0f, 10.0f, true},
    {"Fighter", 300.0f, 80.0f, 1.45f, 45.0f, 75.0f, 18.0f, 2.3e10f, 4.5f, 10.0f, false},
    {"Bomber", 240.0f, 55.0f, 1.00f, 55.0f, 100.0f, 25.0f, 2.0e10f, 6.0f, 12.0f, true},
    {"Corvette", 210.0f, 45.0f, 0.85f, 65.0f, 130.0f, 32.0f, 1.8e10f, 7.0f, 15.0f, true},
    {"Frigate", 170.0f, 30.0f, 0.55f, 90.0f, 200.0f, 50.0f, 1.5e10f, 9.0f, 20.0f, true},
    {"Cruiser", 140.0f, 22.0f, 0.40f, 120.0f, 280.0f, 70.0f, 1.3e10f, 11.0f, 26.0f, false},
    {"Battleship", 105.0f, 14.0f, 0.22f, 175.0f, 480.0f, 120.0f, 0.9e10f, 16.0f, 38.0f, true},
    {"Carrier", 120.0f, 16.0f, 0.26f, 160.0f, 430.0f, 107.0f, 1.0e10f, 15.0f, 34.0f, true},
    {"Hauler", 130.0f, 18.0f, 0.30f, 110.0f, 260.0f, 65.0f, 1.2e10f, 12.0f, 24.0f, true},
    {"Miner", 115.0f, 17.0f, 0.32f, 100.0f, 240.0f, 60.0f, 1.1e10f, 13.0f, 22.0f, true},
    {"Structure", 0.0f, 0.0f, 0.0f, 260.0f, 0.0f, 200.0f, 0.0f, 0.0f, 0.0f, true},
    {"Gate", 0.0f, 0.0f, 0.0f, 175.0f, 0.0f, 135.0f, 0.0f, 0.0f, 0.0f, true},
}};

// The silhouette rule has to hold for the two meshes it was measured from, or
// it is a number somebody typed. Contact radii of 200 and 135 against authored
// plane silhouettes of 253 and 168 (ADR-016 §10): both within a metre and a
// half of the ratio, which is what makes it a rule.
static_assert(200.0f * SILHOUETTE_RADIUS_PER_CONTACT_RADIUS > 248.0f &&
                  200.0f * SILHOUETTE_RADIUS_PER_CONTACT_RADIUS < 258.0f,
              "the station's authored silhouette is 253 m over a 200 m contact radius");
static_assert(135.0f * SILHOUETTE_RADIUS_PER_CONTACT_RADIUS > 163.0f &&
                  135.0f * SILHOUETTE_RADIUS_PER_CONTACT_RADIUS < 173.0f,
              "the stargate's authored silhouette is 168 m over a 135 m contact radius");

// The table is indexed by the enum, so the two have to stay in step. Spelling
// the ends out means a class inserted in the middle fails to compile rather
// than quietly shifting every row after it.
static_assert(CLASS_TABLE.size() == HULL_CLASS_COUNT, "the table is indexed by HullClass");
static_assert(static_cast<std::uint8_t>(HullClass::Interceptor) == 0, "Interceptor is the first row");
static_assert(static_cast<std::uint8_t>(HullClass::Gate) == HULL_CLASS_COUNT - 1, "Gate is the last row");

} // namespace

const ShipClassInfo& ShipClass(HullClass _hullClass) noexcept
{
  const auto index = static_cast<std::size_t>(_hullClass);
  // Clamping rather than reading past the end: this overload takes an enum, so
  // an out-of-range value already means someone cast a bad number, and the
  // bounds-checked entry point below is the one that says so.
  return CLASS_TABLE[index < CLASS_TABLE.size() ? index : 0];
}

bool TryShipClass(std::uint8_t _rawClass, HullClass& _outClass) noexcept
{
  if (_rawClass >= HULL_CLASS_COUNT)
  {
    return false;
  }
  _outClass = static_cast<HullClass>(_rawClass);
  return true;
}

std::string_view HullClassName(HullClass _hullClass) noexcept
{
  return ShipClass(_hullClass).name;
}

bool HullClassHasContent(HullClass _hullClass) noexcept
{
  return ShipClass(_hullClass).hasContent;
}

float SilhouetteRadiusMetres(HullClass _hullClass) noexcept
{
  const ShipClassInfo& info = ShipClass(_hullClass);
  if (!info.hasContent)
  {
    // A reserved class has no mesh to size, and answering a number for one
    // would be this table having an opinion about art that does not exist.
    return 0.0f;
  }
  return info.collisionRadiusMetres * SILHOUETTE_RADIUS_PER_CONTACT_RADIUS;
}

float CosmeticBankRadians(HullClass _hullClass, float _headingRateRadiansPerSec, float _speedMetresPerSec) noexcept
{
  const ShipClassInfo& info = ShipClass(_hullClass);
  if (info.turnRateRadiansPerSec <= 0.0f || info.maxSpeedMetresPerSec <= 0.0f)
  {
    return 0.0f; // A hull that cannot turn -- or move -- has nothing to bank into.
  }

  // Each factor is the observed quantity over the class's own limit, so a
  // Battleship at its full (slow) rate banks as deliberately as an Interceptor
  // at its full (fast) one -- the roll depicts how hard *this* hull is turning,
  // not the absolute angular rate.
  const float turnFraction = std::clamp(_headingRateRadiansPerSec / info.turnRateRadiansPerSec, -1.0f, 1.0f);
  const float speedFraction = std::clamp(_speedMetresPerSec / info.maxSpeedMetresPerSec, 0.0f, 1.0f);

  // Negative because heading is CCW-positive (ADR-001 §3): a turn to port
  // (rate > 0) drops the port wing, which is a roll away from starboard-down.
  return -MAX_COSMETIC_BANK_RADIANS * turnFraction * speedFraction;
}

} // namespace Game
