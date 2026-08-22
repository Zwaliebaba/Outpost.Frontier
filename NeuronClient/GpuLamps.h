#pragma once

#include "GpuCom.h"
#include "SignalLamp.h"

#include <d3d12.h>

#include <cstdint>
#include <span>
#include <vector>

/*
 * The lamp table, resident on the GPU (ADR-006 §6a).
 *
 * One constant buffer holding every lamp of every class, built once at boot
 * from the mesh bounds the loader already measured, and never written again.
 * `LampPass` binds it once per frame and picks a lamp with a root constant.
 *
 * **Why it is static and the instances are not.** A lamp's position is
 * hull-local, so it does not change when the ship moves -- what moves is the
 * transform the vertex shader already has, from the opaque pass's own instance
 * stream. That is the whole performance argument for this shape: a fleet of
 * four hundred ships costs four hundred instance records the frame was writing
 * anyway, and *zero* additional per-lamp work on the CPU. The alternative --
 * one instance per lamp per ship, built each frame -- is twelve hundred records
 * to compose, upload and sort, every frame, for data that has not changed since
 * boot.
 *
 * The table is indexed by `classId`, exactly as `GpuMeshTable` is, and knows no
 * more about which index is a Carrier than that one does (ADR-014): it is given
 * a rig per class, in order, by the composition root.
 */

namespace Neuron
{

class GpuDevice;
class GpuMeshTable;

/*
 * How many lamps the table can hold across every class.
 *
 * A fixed array in a constant buffer rather than a structured buffer, because
 * the whole table is under five kilobytes and a root CBV costs no descriptor
 * heap slot and no barrier. The shipped corpus uses 59 of these -- eight ships
 * at three, the station's twenty-five, the gate's ten -- so the ceiling is
 * headroom rather than a constraint, and `Create` refuses rather than
 * overrunning if a rig is ever added that would exceed it.
 */
inline constexpr std::uint32_t LAMP_TABLE_CAPACITY = 96;

class GpuLampTable
{
public:
  GpuLampTable() = default;
  ~GpuLampTable();

  GpuLampTable(const GpuLampTable&) = delete;
  GpuLampTable& operator=(const GpuLampTable&) = delete;

  /*
   * Builds the table from `_meshes` and one rig per class, in class order.
   *
   * A shorter span than the mesh table means `None` for the classes it does not
   * reach, and an empty one is a client with no lamps at all -- which draws
   * exactly as it did before this pass existed, and is what a game with no
   * opinion about signal lights should get.
   *
   * Never fails for want of lamps: a corpus that produces none still creates
   * successfully and reports `Count() == 0`, because a fleet without navigation
   * lights is a look, not a broken boot.
   */
  [[nodiscard]] bool Create(GpuDevice& _device, const GpuMeshTable& _meshes, std::span<const LampRig> _rigs);
  void Destroy();

  /// Where the constant buffer lives. Zero when there is nothing to draw.
  [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS Constants() const noexcept;

  /// Which run of the table a class owns. Empty for a class with no rig, and
  /// out-of-range ids read as empty rather than asserting -- the pass walks
  /// whichever of the two tables is shorter.
  [[nodiscard]] LampRange Range(std::uint32_t _classId) const noexcept;

  [[nodiscard]] std::uint32_t ClassCount() const noexcept { return static_cast<std::uint32_t>(m_ranges.size()); }
  [[nodiscard]] std::uint32_t Count() const noexcept { return m_lampCount; }

  /// The lamps themselves, as they were uploaded. For the boot log and for a
  /// test that wants to check placement without a device in the loop.
  [[nodiscard]] std::span<const LampInstance> Lamps() const noexcept { return m_lamps; }

private:
  GpuPtr<ID3D12Resource> m_buffer;
  std::vector<LampRange> m_ranges;
  std::vector<LampInstance> m_lamps;
  std::uint32_t m_lampCount = 0;
};

} // namespace Neuron
