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
 * formation on the print, far enough that stations never overlap, which is what
 * lets MVP steering skip inter-ship avoidance entirely (ADR-005 §2).
 */
// Hover heights (the second-to-last column) are cosmetic (ADR-001 §2): a small
// per-class lift so a hull reads as riding above the plane its ring lies on,
// scaled roughly with the hull. The Structure sits on the plane -- a station
// that hovered would read as a ship.
constexpr std::array<ShipClassInfo, HULL_CLASS_COUNT> CLASS_TABLE = {{
    // name           maxSpeed  accel  turnRate  pickRadius  spacing  hover  content
    {"Interceptor", 320.0f, 90.0f, 1.60f, 45.0f, 70.0f, 10.0f, true},
    {"Fighter", 300.0f, 80.0f, 1.45f, 45.0f, 75.0f, 10.0f, false},
    {"Bomber", 240.0f, 55.0f, 1.00f, 55.0f, 100.0f, 12.0f, true},
    {"Corvette", 210.0f, 45.0f, 0.85f, 65.0f, 130.0f, 15.0f, true},
    {"Frigate", 170.0f, 30.0f, 0.55f, 90.0f, 200.0f, 20.0f, true},
    {"Cruiser", 140.0f, 22.0f, 0.40f, 120.0f, 280.0f, 26.0f, false},
    {"Battleship", 105.0f, 14.0f, 0.22f, 175.0f, 480.0f, 38.0f, true},
    {"Carrier", 120.0f, 16.0f, 0.26f, 160.0f, 430.0f, 34.0f, true},
    {"Hauler", 130.0f, 18.0f, 0.30f, 110.0f, 260.0f, 24.0f, true},
    {"Miner", 115.0f, 17.0f, 0.32f, 100.0f, 240.0f, 22.0f, true},
    {"Structure", 0.0f, 0.0f, 0.0f, 260.0f, 0.0f, 0.0f, true},
}};

// The table is indexed by the enum, so the two have to stay in step. Spelling
// the ends out means a class inserted in the middle fails to compile rather
// than quietly shifting every row after it.
static_assert(CLASS_TABLE.size() == HULL_CLASS_COUNT, "the table is indexed by HullClass");
static_assert(static_cast<std::uint8_t>(HullClass::Interceptor) == 0, "Interceptor is the first row");
static_assert(static_cast<std::uint8_t>(HullClass::Structure) == HULL_CLASS_COUNT - 1, "Structure is the last row");

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
