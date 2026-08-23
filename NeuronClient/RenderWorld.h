#pragma once

#include "EntityRecord.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

/*
 * What Extract hands the renderer (ADR-006 §6, ADR-007).
 *
 * `InstanceRecord` is the whole bridge between replicated world state and the
 * GPU, and its field names are the corpus's own. It is the per-instance vertex
 * stream, so a field added here is a field added to the input layout and to
 * every mesh draw in the frame -- which happened exactly once, when S14's
 * cosmetic bank was appended (20 -> 24 bytes, recorded in ADR-006 §6).
 *
 * Nothing in this header knows what a ship is. `classId` is an index into a
 * mesh table the composition root supplies, and `teamColorId` indexes a palette
 * -- the engine draws what it is given (ADR-014).
 *
 * Device-free, like the rest of the maths, so an Extract test needs no GPU.
 */

namespace Neuron
{

/*
 * One ship, as everything that is not the opaque draw needs it.
 *
 * Picking needs an identity, a point and a radius; a selection ring needs the
 * same point and radius; hull and shield bars need the gauges. Those could be
 * three arrays kept in lockstep, and the first edit that appended to one and
 * not the others would put a Carrier's shield bar over an Interceptor. One
 * record, filled once per frame, cannot do that.
 *
 * Not `InstanceRecord`, which is the other half of the same sample: that is a
 * vertex stream sorted by class for the draw, carrying a render classId rather
 * than an identity, and it is twenty bytes because that is the stride the input
 * layout declares. This one is free to grow.
 *
 * `id` is opaque. The engine hands back whatever the game put in and never
 * interprets it (ADR-014). It is `EntityRecord::id`'s type, and
 * `Neuron::INVALID_ENTITY_ID` is NeuronCore's sentinel rather than a second one
 * -- a u32 version of that name compiled here and collided there, which is what
 * ADR-013 §3's uniqueness rule is about when the duplicate is an identifier
 * rather than a file. Sixteen bits is also what `OrderIntent::entityIds` wants,
 * so a selection feeds an order with no conversion.
 */
struct SceneEntity
{
  EntityId id = INVALID_ENTITY_ID;
  DirectX::XMFLOAT2 planeMetres{};  // Sim plane metres (ADR-001 §3), not render space.
  float pickRadiusMetres = 0.0f;    // The class's, so a Battleship is easier to hit than a fighter.
  std::uint8_t hullGauge = 0;    // 0-255, not a percentage. Full is 255.
  std::uint8_t shieldGauge = 0;  // Likewise.
  bool stale = false;               // Extrapolated past the cap and frozen (ADR-002 §4).

  /*
   * The game's own per-entity flags, carried and not read (ADR-014 §4).
   *
   * `EntityRecord::statusBits` arrives at the view and stopped there until the
   * overlay needed one of the bits. What each bit *means* is the game's -- bit
   * zero is undock protection today -- and the engine's whole part is to hand
   * the byte to whoever draws, exactly as `groupId` is a number it carries
   * without knowing that this game means a wing by it.
   *
   * A byte rather than a bool per state, because the next flag then costs
   * nothing here and the one after that costs nothing either.
   */
  std::uint8_t statusBits = 0;
};

/// Per-instance data, matching ADR-006 §6 field for field.
struct InstanceRecord
{
  DirectX::XMFLOAT3 posWorld;             // Render space: (sim.x, cosmetic height, sim.y).
  float heading;                          // Radians CCW from +x in sim space (ADR-001 §3).
  std::uint8_t teamColorId;               // Indexes the emissive palette, never the hull (ADR-006 §7).
  std::uint8_t selectionAndLodBias;       // Bit 0 carries the stale flag; carried, not read here.
  std::uint16_t classId;                  // Index into the mesh table.

  /// Cosmetic roll into the turn, radians; positive drops the starboard wing
  /// (ADR-001 §2, ADR-006 §6 -- S14). **Appended rather than inserted**, like
  /// `UiInstance::axis`: every field above keeps the offset the input layout
  /// already declares for it.
  float bank = 0.0f;

  /*
   * Where in its blink cycle this hull's signal lamps are, in turns (0..1).
   *
   * Appended, on the same discipline as `bank`. It is here rather than in the
   * lamp table because the table is per *class*: without a per-hull offset
   * every ship of a class would strobe on the same frame and a parked wing
   * would flash as one object (ADR-006 §6a). Hashed from the entity id by
   * `Neuron::LampPhaseTurns`, so it is stable for a ship's whole life however
   * the scene is sorted, and constant per frame rather than drifting with the
   * hull's position.
   *
   * Filled by Extract and read only by the lamp pass. The opaque input layout
   * does not declare it, which costs nothing: the stride is this struct's, so
   * the bytes are simply not fetched.
   */
  float lampPhaseTurns = 0.0f;
};

static_assert(sizeof(InstanceRecord) == 28, "InstanceRecord is a vertex stream layout; its size is the stride the input "
                                            "layout declares, so a change here is a change in three places");

/// Where one class's instances sit in the scene's instance array. The opaque
/// pass draws a class at a time, so a class has to be one contiguous run.
struct InstanceRange
{
  std::uint32_t firstInstance = 0;
  std::uint32_t instanceCount = 0;
};

/*
 * One frame's worth of drawable world.
 *
 * Instances are sorted by `classId` and `classRanges` indexes them, because the
 * opaque pass binds one mesh per class: unsorted instances would mean either a
 * bind per instance or a scatter, and both are worse than a sort of a few
 * hundred 20-byte records.
 */
struct RenderScene
{
  std::vector<InstanceRecord> instances;
  std::vector<InstanceRange> classRanges;

  /*
   * The same ships again, in the shape everything that is not the draw needs
   * (ADR-006 §11).
   *
   * Picking runs over this, and so does the overlay: a selection ring is drawn
   * at a ship's position with a radius from its class, which is exactly what
   * the hit test uses, so ring and pick agree by construction rather than by
   * two pieces of code being kept in step. ADR-006 §11's "same plane point
   * feeds the order puck -- one code path" is that argument one call earlier.
   */
  std::vector<SceneEntity> entities;

  /*
   * How many entities on this grid the game is **not** sending (ADR-022 §5d,
   * U3d-c).
   *
   * A count and nothing else -- no position, no extent, not even a bearing --
   * because a culled entity is one that never crossed the wire. That is why it
   * rides on the scene rather than appearing as an entity in it: there is
   * nothing to place, and `CountedChip` turns it into a screen-space sentence
   * rather than a mark on the plane.
   *
   * **The engine does not know what was culled or why.** Interest and priority
   * are the game's ranking (ADR-022 §1), and this is the one number that
   * crosses back: how many did not fit. `Neuron::WorldView` fills it in
   * `BuildScene` beside the hulls it *did* build, so a client drawing a frame
   * has both halves of the same statement.
   *
   * Zero means "everything on this grid is here", which is the ordinary case
   * and draws nothing.
   */
  std::uint16_t culledCount = 0;

  void Clear() noexcept;

  /// Sorts by classId and rebuilds classRanges for _classCount classes.
  void SortByClass(std::uint32_t _classCount);
};

/*
 * `ScenePlacement`/`AddScenery` and `BuildParkedFleet` used to live here.
 *
 * They were S5's answer to having a renderer and no world: a fleet the client
 * invented and a station the composition root converted out of the universe
 * file. S7 deleted both, exactly as `BuildParkedFleet`'s own comment promised
 * it would -- the scene now comes from replicated snapshots through
 * `WorldView::BuildScene`, and the station is a `Structure` the server spawns
 * and replicates like any other ship.
 *
 * What survives is everything that was never a placeholder: `InstanceRecord`,
 * the class ranges, and the sort that lets the opaque pass draw a class in one
 * instanced run.
 */

} // namespace Neuron
