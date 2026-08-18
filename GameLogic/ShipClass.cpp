#include "pch.h"

#include "ShipClass.h"

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
constexpr std::array<ShipClassInfo, HULL_CLASS_COUNT> CLASS_TABLE = {{
    // name           maxSpeed  accel  turnRate  pickRadius  spacing  content
    {"Interceptor", 320.0f, 90.0f, 1.60f, 45.0f, 70.0f, true},
    {"Fighter", 300.0f, 80.0f, 1.45f, 45.0f, 75.0f, false},
    {"Bomber", 240.0f, 55.0f, 1.00f, 55.0f, 100.0f, true},
    {"Corvette", 210.0f, 45.0f, 0.85f, 65.0f, 130.0f, true},
    {"Frigate", 170.0f, 30.0f, 0.55f, 90.0f, 200.0f, true},
    {"Cruiser", 140.0f, 22.0f, 0.40f, 120.0f, 280.0f, false},
    {"Battleship", 105.0f, 14.0f, 0.22f, 175.0f, 480.0f, true},
    {"Carrier", 120.0f, 16.0f, 0.26f, 160.0f, 430.0f, true},
    {"Hauler", 130.0f, 18.0f, 0.30f, 110.0f, 260.0f, true},
    {"Miner", 115.0f, 17.0f, 0.32f, 100.0f, 240.0f, true},
    {"Structure", 0.0f, 0.0f, 0.0f, 260.0f, 0.0f, true},
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

} // namespace Game
