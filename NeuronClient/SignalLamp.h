#pragma once

#include "ObjMesh.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

/*
 * Animated signal lights -- navigation lamps, landing sequences, hazard
 * strobes (ADR-006 §6a).
 *
 * Presentation only. Nothing here is replicated, nothing reaches `Tick`, and a
 * lamp is never a thing the player can click: picking and selection rings read
 * `SceneEntity`, which knows nothing about this file.
 *
 * Deliberately free of D3D12 and C++/WinRT, exactly as `ObjMesh.h` and
 * `ClearColour.h` are: placing a lamp is arithmetic over a bounding box, so it
 * has to be checkable without a device. `GpuLamps.h` turns the result into a
 * constant buffer and `LampPass` draws it.
 *
 * **Why placement is derived and not authored.** A lamp offset typed in by
 * hand is a number that was right for one export of one mesh. Every anchor
 * here is a fraction of something the loader already measured -- the hull's
 * bounding box, or its silhouette radius on the plane -- so a re-export at a
 * different scale, or a class-table retune that changes how big a hull is
 * drawn, moves the lamps with the art instead of leaving them hanging beside
 * it or buried inside it.
 */

namespace Neuron
{

/*
 * How a lamp varies over time.
 *
 * The value is what the shader compares against, so the enumerators are the
 * wire between this file and `Lamp.hlsli` and are not to be renumbered without
 * it. `LampLevel` is the CPU mirror of that comparison and exists so the modes
 * can be asserted without a GPU.
 */
enum class LampMode : std::uint8_t
{
  /// Always on. Navigation lamps -- a port light that blinked would be a
  /// different signal.
  Steady = 0,

  /// A short flash: on for `dutyFraction` of the cycle, off for the rest.
  Strobe = 1,

  /// Sinusoidal between `floorFraction` and full. Never off, so a berth reads
  /// as occupied rather than as failed.
  Pulse = 2,

  /// A flash that does not extinguish -- on for `dutyFraction`, resting at
  /// `floorFraction`. Phase is the lamp's index over the ring's count, which is
  /// what makes the run travel.
  Chase = 3
};

/*
 * Which fixture a class carries.
 *
 * Not "which mesh": the engine has no idea which index is a Carrier (ADR-014),
 * so the composition root hands one of these per class in mesh order, the same
 * way it hands the silhouette radii. `None` is the honest answer for a file the
 * game has no opinion about, and it costs the class nothing.
 */
enum class LampRig : std::uint8_t
{
  None = 0,

  /// Three lamps: port red, starboard green, dorsal white strobe. Every
  /// flyable hull, whatever its size.
  Ship = 1,

  /// The platform: an approach chase around the rim, a pulse and a strobe per
  /// berth, and a hazard strobe on the mast.
  Station = 2,

  /// The gate: a transit chase around the fore mouth, a strobe over the spire
  /// and a pulse under the ventral spikes.
  Gate = 3
};

/*
 * The four signal colours, in **linear** rgb.
 *
 * Converted here, once, from the sRGB the art direction names them in -- the
 * render target is `_SRGB`, so a colour reaches the screen unconverted and a
 * value typed straight out of a colour picker would arrive too bright. The
 * sRGB byte triples are in the comments so the two can be compared without
 * anyone having to recompute the transfer function to check.
 */
inline constexpr DirectX::XMFLOAT3 LAMP_RED{1.0000f, 0.0452f, 0.0452f};   // sRGB (255, 60, 60)
inline constexpr DirectX::XMFLOAT3 LAMP_GREEN{0.1878f, 1.0000f, 0.0802f}; // sRGB (120, 255, 80)
inline constexpr DirectX::XMFLOAT3 LAMP_WHITE{0.8308f, 1.0000f, 0.7157f}; // sRGB (235, 255, 220)
inline constexpr DirectX::XMFLOAT3 LAMP_AMBER{1.0000f, 0.5149f, 0.0612f}; // sRGB (255, 190, 70)

/*
 * One lamp, as the vertex shader wants it.
 *
 * Hull-local: the pass applies the same bank-then-yaw-then-translate the opaque
 * vertex shader does, so a lamp turns and banks with the ship it is bolted to
 * and the table is built once at boot rather than per frame.
 *
 * Forty-eight bytes, which is three HLSL constant registers exactly -- that is
 * why `mode` is a float and not the enum it came from. A cbuffer array element
 * is padded up to a 16-byte boundary whatever is in it, so packing the mode
 * into a spare lane costs nothing and saves a register per lamp.
 */
struct LampInstance
{
  DirectX::XMFLOAT3 localPosition; // Render space, hull-local: x port, y up, z aft (forward is -z).
  float sizeMetres = 0.0f;         // The billboard's width at full brightness.

  DirectX::XMFLOAT3 colour{};      // Linear rgb, one of the four above.
  float mode = 0.0f;               // A `LampMode`, as a float the shader compares.

  float phaseTurns = 0.0f;    // 0..1. Offsets this lamp against every other one.
  float rateHz = 0.0f;        // Cycles per second.
  float dutyFraction = 0.0f;  // Strobe and chase: the fraction of the cycle that is lit.
  float floorFraction = 0.0f; // Chase and pulse: what the lamp rests at between flashes.
};

static_assert(sizeof(LampInstance) == 48, "LampInstance is a constant-buffer layout mirrored in Lamp.hlsli; three 16-byte "
                                          "registers is the shape both sides agree on");

/// Where one class's lamps sit in the table. A class draws a run of it, exactly
/// as `InstanceRange` lets the opaque pass draw a run of the instance stream.
struct LampRange
{
  std::uint32_t firstLamp = 0;
  std::uint32_t lampCount = 0;
};

/*
 * What a rig needs to know about the mesh it is being bolted to.
 *
 * Measured from the mesh and never authored, so nothing here can be told a size
 * the renderer is not drawing. Build it with `LampBoundsFor`; the fields are
 * public so a test can state a hull rather than load one.
 */
struct MeshLampBounds
{
  DirectX::XMFLOAT3 boundsMin{};
  DirectX::XMFLOAT3 boundsMax{};

  /// The silhouette radius on the plane -- `PlaneRadiusMetres` after the fit,
  /// which is the number the class table states and the number the station and
  /// gate rigs express every radius and height as a fraction of.
  float planeRadiusMetres = 0.0f;

  /*
   * How high the hull's *surface* is at the three places a ship hangs a lamp:
   * out at each beam, and on the spine where the dorsal strobe goes.
   *
   * **Not `boundsMax.y`, and this is the correction that matters.** The
   * proposal placed the navigation lamps "just above bbox top", and on this
   * corpus that is wrong by a wide margin: a `Battleship`'s box has its ceiling
   * 100 m up because a thin antenna reaches there, while the deck out at the
   * beam -- where the lamp goes -- is 37 m. Anchored to the box, the wing lamps
   * hung four lamp-widths clear of the hull in open space, which is visibly
   * wrong from every angle and passes a "never inside geometry" check with room
   * to spare. Anchored here, they sit on the deck, which is what the proposal's
   * own acceptance asks for ("above the deck line").
   *
   * Still derived rather than authored -- `LocalTopMetres` is the loader
   * measuring its own vertices -- so a re-export still moves the lamps with the
   * art. Zero when nothing measured them, which is what a hand-built
   * `MeshLampBounds` gets and what the rigs then read as "use the box".
   */
  float deckPortMetres = 0.0f;
  float deckStarboardMetres = 0.0f;
  float deckSpineMetres = 0.0f;
};

/*
 * Measures a loaded mesh into the shape the rigs want.
 *
 * Call it *after* `FitObjMeshToPlaneRadius`: every number in the result is in
 * drawn metres, because a lamp is placed on the hull that is on the screen.
 */
[[nodiscard]] MeshLampBounds LampBoundsFor(const ObjMesh& _mesh);

/*
 * How big a ship's lamps are: `min(3.5 + 0.1 x length, 14)` metres.
 *
 * Both ends are legibility rather than taste. Below about 4 m a lamp is
 * sub-pixel at the zoom the whole fleet is framed at, so a strike craft would
 * simply have no lights; above about 14 m the glow is wider than the
 * superstructure it sits on and a capital ship reads as being on fire.
 */
[[nodiscard]] float ShipLampSizeMetres(float _hullLengthMetres) noexcept;

/*
 * A stable per-ship phase offset in turns, hashed from an entity id.
 *
 * The lamp table is per *class*, so without this every ship of a class would
 * strobe on the same frame and a parked wing would flash like one object. The
 * hash is of the id and nothing else, so a ship keeps its phase while it lives
 * however the scene is sorted, and two ships that are neighbours are no more
 * likely to agree than any other pair.
 */
[[nodiscard]] float LampPhaseTurns(std::uint32_t _entityId) noexcept;

/*
 * Builds the lamps for one rig, appending to `_outLamps`.
 *
 * Appending rather than replacing, because the table is one array over every
 * class and a class is a run of it. A `None` rig, or bounds with no extent,
 * appends nothing -- which is what a class with no lights looks like and is not
 * an error.
 */
void BuildLampRig(LampRig _rig, const MeshLampBounds& _bounds, std::vector<LampInstance>& _outLamps);

/*
 * What a lamp's brightness is at `_seconds`, in 0..1 -- the CPU mirror of the
 * vertex shader's animation.
 *
 * It exists to be tested. A strobe's duty cycle, a chase's travel and a pulse's
 * floor are the kind of thing that is either right or subtly wrong for months,
 * and neither a screenshot nor a frame capture settles it. The shader is four
 * lines and this is the same four lines; the test asserts the shape, and the
 * comment in `Lamp.hlsli` names this function as the pair to edit with it.
 */
[[nodiscard]] float LampLevel(const LampInstance& _lamp, float _instancePhaseTurns, float _seconds) noexcept;

/// The animated opacity a level maps to. The lamp is never fully out: a dark
/// body reads as a fitting, and a fitting that vanishes reads as a hole.
[[nodiscard]] float LampOpacity(float _level) noexcept;

/// The animated size multiplier a level maps to -- the +/-28% swell that makes
/// a flash read as a flash rather than as a colour change.
[[nodiscard]] float LampScale(float _level) noexcept;

} // namespace Neuron
