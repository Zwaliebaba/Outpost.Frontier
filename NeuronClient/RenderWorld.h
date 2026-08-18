#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

/*
 * What Extract hands the renderer (ADR-006 §6, ADR-007).
 *
 * `InstanceRecord` is the whole bridge between replicated world state and the
 * GPU, and its field names are the corpus's own. It is 20 bytes, and it stays
 * 20 bytes: it is the per-instance vertex stream, so a field added here is a
 * field added to the input layout and to every mesh draw in the frame.
 *
 * Nothing in this header knows what a ship is. `classId` is an index into a
 * mesh table the composition root supplies, and `teamColorId` indexes a palette
 * -- the engine draws what it is given (ADR-014).
 *
 * Device-free, like the rest of the maths, so an Extract test needs no GPU.
 */

namespace Neuron
{

/// Per-instance data, matching ADR-006 §6 field for field.
struct InstanceRecord
{
  DirectX::XMFLOAT3 posWorld;             // Render space: (sim.x, cosmetic height, sim.y).
  float heading;                          // Radians CCW from +x in sim space (ADR-001 §3).
  std::uint8_t teamColorId;               // Indexes the emissive palette, never the hull (ADR-006 §7).
  std::uint8_t selectionAndLodBias;       // Reserved for S8's overlay channel; carried, not read.
  std::uint16_t classId;                  // Index into the mesh table.
};

static_assert(sizeof(InstanceRecord) == 20, "InstanceRecord is a vertex stream layout; its size is the stride the input "
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

  void Clear() noexcept;

  /// Sorts by classId and rebuilds classRanges for _classCount classes.
  void SortByClass(std::uint32_t _classCount);
};

/*
 * The locally-faked parked fleet (Build Order S5).
 *
 * S5 renders before there is anything to replicate, so the scene is authored
 * here: one structure at the anchor and eight wings around it, one wing per
 * ship class, so every mesh in GameData is on screen the first time the pass
 * runs. Frame time is measured against this at 41 instances.
 *
 * S7 deletes it. It is deliberately one function with no state, so deleting it
 * is deleting a call.
 */
struct ParkedFleetDesc
{
  std::uint32_t shipClassCount = 8;   // classId 0..7 are ships...
  std::uint32_t structureClassId = 8; // ...and the station is the ninth mesh.
  std::uint32_t shipsPerWing = 5;     // 8 wings x 5 + 1 station = 41 instances.
  float wingRadiusMetres = 1400.0f;
  float wingSpacingMultiplier = 3.0f; // Of the class's own radius, so a Carrier
                                      // wing is not parked at Interceptor spacing.
};

/// _classRadiiMetres is indexed by classId -- the mesh bounds the loader
/// produced, so station spacing follows the hull that is actually being parked.
void BuildParkedFleet(const ParkedFleetDesc& _desc, std::span<const float> _classRadiiMetres, RenderScene& _outScene);

} // namespace Neuron
