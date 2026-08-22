#include "pch.h"

#include "SignalLamp.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace Neuron
{
namespace
{

/*
 * The animation vocabulary, in one block because it is one decision.
 *
 * Every number here is shared with `Lamp.hlsli`, which declares the same
 * constants for the shader half of `LampLevel`. They are duplicated rather than
 * uploaded because they are vocabulary and not content: a strobe is 8% of its
 * cycle in the same sense that a selection ring is an ellipse, and putting them
 * in a constant buffer would invite them to be different per lamp, which is the
 * opposite of a vocabulary.
 */

/// Strobe: on for 8% of the cycle. Long enough to register at 60 Hz, short
/// enough that a fleet of them reads as blinking rather than as lit.
constexpr float STROBE_DUTY = 0.08f;

/// Chase: a wider flash, and one that rests at 38% rather than going out, so
/// the ring is always a ring and the travelling part is what the eye follows.
constexpr float CHASE_DUTY = 0.18f;
constexpr float CHASE_FLOOR = 0.38f;

/// Pulse: never below 45%. A berth light that went out would read as a fault.
constexpr float PULSE_FLOOR = 0.45f;

/// Brightness, as opacity. The bottom is not zero: an unlit lamp is still a
/// fitting on the hull, and one that disappears entirely reads as a hole in the
/// geometry rather than as a light that is off.
constexpr float OPACITY_MIN = 0.18f;
constexpr float OPACITY_MAX = 0.80f;

/// And as size. +/-28% about the lamp's stated metres, which is what makes a
/// flash read as a flash: the eye catches a change in area faster than a change
/// in value, especially against a near-black background.
constexpr float SCALE_SWELL = 0.28f;

/// Ship lamp sizing (`ShipLampSizeMetres`).
constexpr float SHIP_LAMP_BASE_METRES = 3.5f;
constexpr float SHIP_LAMP_PER_LENGTH = 0.1f;
constexpr float SHIP_LAMP_MAX_METRES = 14.0f;

/*
 * Clearance, in multiples of the lamp's own size, between an anchor and the
 * geometry it is anchored to.
 *
 * **This is the whole reason the rigs are written the way they are.** A glow
 * billboard whose centre is inside a hull is either half-buried -- which reads
 * as an explosion, not a lamp -- or entirely gone, because the depth test
 * discards it against the very hull it is bolted to. Clearance in multiples of
 * the lamp rather than in metres is what makes that hold at every hull size:
 * the sprite that has to clear the surface is the thing being measured against.
 */
constexpr float NAV_LAMP_CLEARANCE = 0.3f;
constexpr float DORSAL_LAMP_CLEARANCE = 0.7f;
constexpr float FIXTURE_CLEARANCE = 0.5f;
constexpr float BOOM_TIP_CLEARANCE = 0.7f;

/// Where along the hull the dorsal strobe sits: 55% from the bow toward the
/// stern, which on every mesh in the corpus is the top of the superstructure
/// rather than the engine deck behind it.
constexpr float DORSAL_LENGTH_FRACTION = 0.55f;

/*
 * The window `LampBoundsFor` measures a deck height through, as a fraction of
 * the hull's own beam and length.
 *
 * Wide enough that a lamp is not anchored to one stray vertex, narrow enough
 * that it is measuring the deck it sits on rather than the superstructure
 * amidships -- which is the whole distinction the deck heights exist to draw.
 */
constexpr float DECK_PROBE_BEAM_FRACTION = 0.15f;
constexpr float DECK_PROBE_LENGTH_FRACTION = 0.20f;

/*
 * The station rig, as fractions of the platform's silhouette radius.
 *
 * Calibrated against the shipped `Structure.obj` -- at its 250 m drawn radius
 * these come out as the 168 m rim, the 14 m rim lamps and the 252 m boom tips
 * the art direction names. Stated as fractions so that a class-table retune
 * moves the whole fixture with the hull instead of leaving a ring of lamps
 * floating where the rim used to be.
 *
 * The bearings are measured rather than chosen: the mesh puts its four berth
 * booms on the cardinals and its four pylons on the diagonals, and the berth
 * lamps go on the booms.
 */
constexpr std::uint32_t STATION_RIM_LAMPS = 16;
constexpr float STATION_RIM_RADIUS = 0.672f;   // 168 m at R = 250.
constexpr float STATION_RIM_HEIGHT = 0.094f;   // The deck edge's top, 23.5 m.
constexpr float STATION_RIM_SIZE = 0.056f;     // 14 m.
constexpr float STATION_RIM_RATE_HZ = 0.55f;

constexpr std::uint32_t STATION_BERTHS = 4;
constexpr float STATION_BERTH_RADIUS = 0.920f; // 230 m -- the boom's mast, not its tip.
constexpr float STATION_BERTH_HEIGHT = 0.144f; // The mast's top, 36 m.
constexpr float STATION_BERTH_SIZE = 0.072f;   // 18 m.
constexpr float STATION_BERTH_RATE_HZ = 0.65f;

constexpr float STATION_HAZARD_RADIUS = 1.000f; // The boom tip, 250 m.
constexpr float STATION_HAZARD_HEIGHT = 0.015f; // The boom's own top, 3.8 m -- low, as asked.
constexpr float STATION_HAZARD_SIZE = 0.048f;   // 12 m.
constexpr float STATION_HAZARD_RATE_HZ = 0.90f;

constexpr float STATION_MAST_SIZE = 0.080f; // 20 m.
constexpr float STATION_MAST_RATE_HZ = 0.60f;

/*
 * The gate rig, likewise, against `Stargate.obj`'s 168.8 m drawn radius.
 *
 * The gate's ring stands up in Y rather than lying on the plane, so its lamps
 * ring the mouth in x/y at a fixed z -- which is why this rig cannot be the
 * station's with different numbers.
 */
constexpr std::uint32_t GATE_MOUTH_LAMPS = 8;
constexpr float GATE_MOUTH_RADIUS = 0.482f;  // 81 m in the ring's own plane.
constexpr float GATE_MOUTH_DEPTH = -0.660f;  // 111 m forward: 4 m clear of the ring face.
constexpr float GATE_MOUTH_SIZE = 0.214f;    // 36 m.
constexpr float GATE_MOUTH_RATE_HZ = 0.80f;
/// The mesh's eight rim studs sit half a step off the axes; the lamps line up
/// with them rather than with the axes, because that is where the art put the
/// fittings.
constexpr float GATE_MOUTH_BEARING_OFFSET = 0.0625f;

constexpr float GATE_SPIRE_SIZE = 0.118f;   // 20 m.
constexpr float GATE_SPIRE_OFFSET = 0.084f; // The spire tip's own z, 14 m aft of centre.
constexpr float GATE_SPIRE_RATE_HZ = 0.70f;

constexpr float GATE_KEEL_SIZE = 0.107f; // 18 m.
constexpr float GATE_KEEL_RATE_HZ = 0.62f;

[[nodiscard]] LampInstance MakeLamp(const XMFLOAT3& _position, float _size, const XMFLOAT3& _colour, LampMode _mode, float _phase,
                                    float _rateHz) noexcept
{
  LampInstance lamp;
  lamp.localPosition = _position;
  lamp.sizeMetres = _size;
  lamp.colour = _colour;
  lamp.mode = static_cast<float>(_mode);
  lamp.phaseTurns = _phase;
  lamp.rateHz = _rateHz;

  // The duty and floor come from the mode rather than from the caller: they are
  // what *makes* a strobe a strobe, and a per-lamp override would let one drift
  // into being a slow pulse with a strobe's name.
  switch (_mode)
  {
  case LampMode::Strobe:
    lamp.dutyFraction = STROBE_DUTY;
    lamp.floorFraction = 0.0f;
    break;
  case LampMode::Pulse:
    lamp.dutyFraction = 0.0f;
    lamp.floorFraction = PULSE_FLOOR;
    break;
  case LampMode::Chase:
    lamp.dutyFraction = CHASE_DUTY;
    lamp.floorFraction = CHASE_FLOOR;
    break;
  case LampMode::Steady:
  default:
    lamp.dutyFraction = 0.0f;
    lamp.floorFraction = 0.0f;
    break;
  }
  return lamp;
}

/// A point on a ring lying *on the plane*, at `_bearingTurns` from forward
/// (-z) turning toward port (+x) -- the station's convention.
[[nodiscard]] XMFLOAT3 PlaneRingPoint(float _radius, float _height, float _bearingTurns) noexcept
{
  const float angle = _bearingTurns * XM_2PI;
  return XMFLOAT3{_radius * std::sin(angle), _height, -_radius * std::cos(angle)};
}

/// A point on a ring lying in the hull's x/y plane at a fixed z -- the gate's
/// mouth. Bearing 0 is straight up (+y), turning toward port (+x).
[[nodiscard]] XMFLOAT3 UprightRingPoint(float _radius, float _depth, float _bearingTurns) noexcept
{
  const float angle = _bearingTurns * XM_2PI;
  return XMFLOAT3{_radius * std::sin(angle), _radius * std::cos(angle), _depth};
}

/*
 * Three lamps, placed off the bounding box and nothing else.
 *
 * Port is +x: `OpaqueVS` states it (the bank rotates +x toward +y and that is
 * what lifts the port wing), and the whole corpus is authored to it. Forward is
 * -z, so mid-length is the box's own centre and the stern is +z.
 */
void BuildShipRig(const MeshLampBounds& _bounds, std::vector<LampInstance>& _outLamps)
{
  const float length = _bounds.boundsMax.z - _bounds.boundsMin.z;
  const float size = ShipLampSizeMetres(length);
  const float midLength = (_bounds.boundsMin.z + _bounds.boundsMax.z) * 0.5f;

  // The deck *at each anchor*, falling back to the box's ceiling for bounds
  // nobody measured -- which is the old behaviour, kept so a hand-built
  // `MeshLampBounds` still places something sane.
  const float portDeck = _bounds.deckPortMetres != 0.0f ? _bounds.deckPortMetres : _bounds.boundsMax.y;
  const float starboardDeck = _bounds.deckStarboardMetres != 0.0f ? _bounds.deckStarboardMetres : _bounds.boundsMax.y;
  const float spineDeck = _bounds.deckSpineMetres != 0.0f ? _bounds.deckSpineMetres : _bounds.boundsMax.y;

  _outLamps.push_back(MakeLamp(XMFLOAT3{_bounds.boundsMax.x, portDeck + NAV_LAMP_CLEARANCE * size, midLength}, size, LAMP_RED,
                               LampMode::Steady, 0.0f, 0.0f));
  _outLamps.push_back(MakeLamp(XMFLOAT3{_bounds.boundsMin.x, starboardDeck + NAV_LAMP_CLEARANCE * size, midLength}, size,
                               LAMP_GREEN, LampMode::Steady, 0.0f, 0.0f));

  // The dorsal strobe carries no phase of its own: the per-instance phase is
  // what separates one ship from the next, and adding a per-class offset on top
  // would only shift the whole fleet together.
  const float sternward = _bounds.boundsMin.z + length * DORSAL_LENGTH_FRACTION;
  _outLamps.push_back(MakeLamp(XMFLOAT3{0.0f, spineDeck + DORSAL_LAMP_CLEARANCE * size, sternward}, size, LAMP_WHITE,
                               LampMode::Strobe, 0.0f, 0.72f));
}

void BuildStationRig(const MeshLampBounds& _bounds, std::vector<LampInstance>& _outLamps)
{
  const float radius = _bounds.planeRadiusMetres;

  // The approach sequence: sixteen white lamps around the deck rim, each
  // phased by its own position in the ring, which is what makes the flash
  // travel rather than blink.
  const float rimSize = STATION_RIM_SIZE * radius;
  for (std::uint32_t i = 0; i < STATION_RIM_LAMPS; ++i)
  {
    const float bearing = static_cast<float>(i) / static_cast<float>(STATION_RIM_LAMPS);
    XMFLOAT3 point = PlaneRingPoint(STATION_RIM_RADIUS * radius, STATION_RIM_HEIGHT * radius, bearing);
    point.y += FIXTURE_CLEARANCE * rimSize;
    _outLamps.push_back(MakeLamp(point, rimSize, LAMP_WHITE, LampMode::Chase, bearing, STATION_RIM_RATE_HZ));
  }

  // Four berths on the cardinal booms: an amber pulse over the mast, and a red
  // hazard strobe out at the tip where a hull would come alongside.
  const float berthSize = STATION_BERTH_SIZE * radius;
  const float hazardSize = STATION_HAZARD_SIZE * radius;
  for (std::uint32_t i = 0; i < STATION_BERTHS; ++i)
  {
    const float bearing = static_cast<float>(i) / static_cast<float>(STATION_BERTHS);

    XMFLOAT3 mast = PlaneRingPoint(STATION_BERTH_RADIUS * radius, STATION_BERTH_HEIGHT * radius, bearing);
    mast.y += FIXTURE_CLEARANCE * berthSize;
    _outLamps.push_back(MakeLamp(mast, berthSize, LAMP_AMBER, LampMode::Pulse, bearing, STATION_BERTH_RATE_HZ));

    XMFLOAT3 tip = PlaneRingPoint(STATION_HAZARD_RADIUS * radius, STATION_HAZARD_HEIGHT * radius, bearing);
    tip.y += BOOM_TIP_CLEARANCE * hazardSize;
    _outLamps.push_back(MakeLamp(tip, hazardSize, LAMP_RED, LampMode::Strobe, bearing, STATION_HAZARD_RATE_HZ));
  }

  // And the hazard beacon on the mast, anchored to the box's own ceiling: the
  // mast *is* what makes `boundsMax.y` what it is, so this clears the tallest
  // thing on the station by construction rather than by a measured number that
  // a re-export could invalidate.
  const float mastSize = STATION_MAST_SIZE * radius;
  _outLamps.push_back(MakeLamp(XMFLOAT3{0.0f, _bounds.boundsMax.y + FIXTURE_CLEARANCE * mastSize, 0.0f}, mastSize, LAMP_RED,
                               LampMode::Strobe, 0.0f, STATION_MAST_RATE_HZ));
}

void BuildGateRig(const MeshLampBounds& _bounds, std::vector<LampInstance>& _outLamps)
{
  const float radius = _bounds.planeRadiusMetres;

  // The transit sequence, running round the fore mouth. The depth is forward of
  // the ring face rather than level with it, so the lamps sit in open space
  // ahead of the barrel and the ring occludes them honestly as the view orbits
  // behind it.
  const float mouthSize = GATE_MOUTH_SIZE * radius;
  for (std::uint32_t i = 0; i < GATE_MOUTH_LAMPS; ++i)
  {
    const float step = static_cast<float>(i) / static_cast<float>(GATE_MOUTH_LAMPS);
    const XMFLOAT3 point = UprightRingPoint(GATE_MOUTH_RADIUS * radius, GATE_MOUTH_DEPTH * radius,
                                            step + GATE_MOUTH_BEARING_OFFSET);
    _outLamps.push_back(MakeLamp(point, mouthSize, LAMP_AMBER, LampMode::Chase, step, GATE_MOUTH_RATE_HZ));
  }

  // Above the spire and below the ventral spikes, both anchored to the box:
  // those two features are exactly what its ceiling and floor are.
  const float spireSize = GATE_SPIRE_SIZE * radius;
  _outLamps.push_back(MakeLamp(XMFLOAT3{0.0f, _bounds.boundsMax.y + FIXTURE_CLEARANCE * spireSize, GATE_SPIRE_OFFSET * radius},
                               spireSize, LAMP_RED, LampMode::Strobe, 0.0f, GATE_SPIRE_RATE_HZ));

  const float keelSize = GATE_KEEL_SIZE * radius;
  _outLamps.push_back(MakeLamp(XMFLOAT3{0.0f, _bounds.boundsMin.y - FIXTURE_CLEARANCE * keelSize, 0.0f}, keelSize, LAMP_AMBER,
                               LampMode::Pulse, 0.0f, GATE_KEEL_RATE_HZ));
}

} // namespace

MeshLampBounds LampBoundsFor(const ObjMesh& _mesh)
{
  MeshLampBounds bounds;
  bounds.boundsMin = _mesh.boundsMin;
  bounds.boundsMax = _mesh.boundsMax;
  bounds.planeRadiusMetres = PlaneRadiusMetres(_mesh);

  const float beam = _mesh.boundsMax.x - _mesh.boundsMin.x;
  const float length = _mesh.boundsMax.z - _mesh.boundsMin.z;
  if (beam <= 0.0f || length <= 0.0f)
  {
    return bounds; // Nothing to probe. The rigs read the zeros as "use the box".
  }

  const float halfX = DECK_PROBE_BEAM_FRACTION * beam;
  const float halfZ = DECK_PROBE_LENGTH_FRACTION * length;
  const float midLength = (_mesh.boundsMin.z + _mesh.boundsMax.z) * 0.5f;
  const float sternward = _mesh.boundsMin.z + length * DORSAL_LENGTH_FRACTION;

  bounds.deckPortMetres = LocalTopMetres(_mesh, _mesh.boundsMax.x, midLength, halfX, halfZ, _mesh.boundsMax.y);
  bounds.deckStarboardMetres = LocalTopMetres(_mesh, _mesh.boundsMin.x, midLength, halfX, halfZ, _mesh.boundsMax.y);
  bounds.deckSpineMetres = LocalTopMetres(_mesh, 0.0f, sternward, halfX, halfZ, _mesh.boundsMax.y);
  return bounds;
}

float ShipLampSizeMetres(float _hullLengthMetres) noexcept
{
  const float length = std::max(_hullLengthMetres, 0.0f);
  return std::min(SHIP_LAMP_BASE_METRES + SHIP_LAMP_PER_LENGTH * length, SHIP_LAMP_MAX_METRES);
}

float LampPhaseTurns(std::uint32_t _entityId) noexcept
{
  /*
   * A 32-bit integer avalanche (the finaliser from MurmurHash3), so that ids
   * one apart -- which is exactly what a wing of freshly spawned ships is --
   * land nowhere near each other.
   *
   * Not `rand`, and not the clock. A phase has to be the *same* every frame for
   * a given ship or the strobe would jitter instead of blinking, and it has to
   * be the same in a replay as in the session it was recorded from, even though
   * nothing about this reaches the simulation.
   */
  std::uint32_t hash = _entityId;
  hash ^= hash >> 16;
  hash *= 0x85ebca6bu;
  hash ^= hash >> 13;
  hash *= 0xc2b2ae35u;
  hash ^= hash >> 16;

  // Over 2^32 rather than a modulus: every bit of the avalanche reaches the
  // result, and a power-of-two modulus would keep only the low ones.
  return static_cast<float>(hash) * (1.0f / 4294967296.0f);
}

void BuildLampRig(LampRig _rig, const MeshLampBounds& _bounds, std::vector<LampInstance>& _outLamps)
{
  const bool hasExtent = _bounds.boundsMax.x > _bounds.boundsMin.x && _bounds.boundsMax.y >= _bounds.boundsMin.y &&
                         _bounds.boundsMax.z > _bounds.boundsMin.z;
  if (!hasExtent)
  {
    return; // A mesh with no box has nowhere to hang a lamp. Silence, not a
            // lamp at the origin, which would be inside whatever is there.
  }

  switch (_rig)
  {
  case LampRig::Ship:
    BuildShipRig(_bounds, _outLamps);
    break;
  case LampRig::Station:
    if (_bounds.planeRadiusMetres > 0.0f)
    {
      BuildStationRig(_bounds, _outLamps);
    }
    break;
  case LampRig::Gate:
    if (_bounds.planeRadiusMetres > 0.0f)
    {
      BuildGateRig(_bounds, _outLamps);
    }
    break;
  case LampRig::None:
  default:
    break;
  }
}

float LampLevel(const LampInstance& _lamp, float _instancePhaseTurns, float _seconds) noexcept
{
  // Edited in step with `LampAnimatedLevel` in Lamp.hlsli. Four lines each, and
  // the reason there are two copies is that only one of them can be tested.
  const float turns = _seconds * _lamp.rateHz + _lamp.phaseTurns + _instancePhaseTurns;
  const float cycle = turns - std::floor(turns);

  switch (static_cast<LampMode>(static_cast<std::uint8_t>(_lamp.mode)))
  {
  case LampMode::Strobe:
    return cycle < _lamp.dutyFraction ? 1.0f : 0.0f;
  case LampMode::Pulse:
    return _lamp.floorFraction + (1.0f - _lamp.floorFraction) * (0.5f + 0.5f * std::sin(turns * XM_2PI));
  case LampMode::Chase:
    return cycle < _lamp.dutyFraction ? 1.0f : _lamp.floorFraction;
  case LampMode::Steady:
  default:
    return 1.0f;
  }
}

float LampOpacity(float _level) noexcept
{
  const float level = std::clamp(_level, 0.0f, 1.0f);
  return OPACITY_MIN + (OPACITY_MAX - OPACITY_MIN) * level;
}

float LampScale(float _level) noexcept
{
  const float level = std::clamp(_level, 0.0f, 1.0f);
  return 1.0f - SCALE_SWELL + 2.0f * SCALE_SWELL * level;
}

} // namespace Neuron
