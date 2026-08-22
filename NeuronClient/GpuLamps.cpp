#include "pch.h"

#include "GpuLamps.h"

#include "GpuDevice.h"
#include "GpuMeshes.h"
#include "Log.h"

#include <cstring>

namespace Neuron
{
namespace
{

/// Constant buffers are bound in 256-byte multiples whatever they contain.
constexpr std::uint64_t CONSTANT_BUFFER_ALIGNMENT = 256;

[[nodiscard]] std::uint64_t AlignedBytes(std::uint64_t _bytes) noexcept
{
  return (_bytes + CONSTANT_BUFFER_ALIGNMENT - 1) & ~(CONSTANT_BUFFER_ALIGNMENT - 1);
}

} // namespace

GpuLampTable::~GpuLampTable()
{
  Destroy();
}

bool GpuLampTable::Create(GpuDevice& _device, const GpuMeshTable& _meshes, std::span<const LampRig> _rigs)
{
  Destroy();

  const std::uint32_t classCount = _meshes.Count();
  m_ranges.assign(classCount, LampRange{});
  m_lamps.clear();
  m_lamps.reserve(LAMP_TABLE_CAPACITY);

  for (std::uint32_t classId = 0; classId < classCount; ++classId)
  {
    const LampRig rig = classId < _rigs.size() ? _rigs[classId] : LampRig::None;
    const GpuMesh& mesh = _meshes.Mesh(classId);

    const auto first = static_cast<std::uint32_t>(m_lamps.size());
    BuildLampRig(rig, mesh.lampBounds, m_lamps);

    if (m_lamps.size() > LAMP_TABLE_CAPACITY)
    {
      // Refused rather than truncated. A table that quietly dropped the last
      // class's lamps would be a station with no beacon and no line anywhere
      // saying why.
      NEURON_LOG_ERROR("lamps: the rigs want %u lamps and the table holds %u; raise LAMP_TABLE_CAPACITY",
                       static_cast<unsigned>(m_lamps.size()), static_cast<unsigned>(LAMP_TABLE_CAPACITY));
      Destroy();
      return false;
    }

    m_ranges[classId].firstLamp = first;
    m_ranges[classId].lampCount = static_cast<std::uint32_t>(m_lamps.size()) - first;
  }

  m_lampCount = static_cast<std::uint32_t>(m_lamps.size());
  if (m_lampCount == 0)
  {
    // A corpus with no rigs at all. Not an error: the pass sees a count of zero
    // and records nothing, which is the frame this client drew before lamps
    // existed.
    NEURON_LOG_INFO("lamps: no class carries a rig; the signal pass will draw nothing");
    return true;
  }

  /*
   * An upload heap, written once and left mapped.
   *
   * Not a default heap with a staging copy, which is what `GpuMeshTable` does
   * for its vertices and is right there: this is under five kilobytes, read
   * once per vertex rather than once per vertex *stream*, and moving it would
   * cost a command list, a queue submission and a wait at boot to save a PCIe
   * read of a page. Left mapped because the CPU never touches it again and
   * unmapping a persistently-mapped upload buffer buys nothing.
   */
  const std::uint64_t bytes = AlignedBytes(static_cast<std::uint64_t>(LAMP_TABLE_CAPACITY) * sizeof(LampInstance));

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  check_hresult(_device.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(m_buffer.put())));
  NAME_D3D12_OBJECT(m_buffer);

  void* mapped = nullptr;
  const D3D12_RANGE readRange{0, 0}; // Write-only: the CPU never reads this back.
  check_hresult(m_buffer->Map(0, &readRange, &mapped));
  // The whole allocation, so the tail past the last lamp is zeroes rather than
  // whatever the heap came with -- a root constant that ever pointed past the
  // end would then draw a zero-sized lamp instead of noise.
  std::memset(mapped, 0, static_cast<std::size_t>(bytes));
  std::memcpy(mapped, m_lamps.data(), m_lamps.size() * sizeof(LampInstance));
  m_buffer->Unmap(0, nullptr);

  NEURON_LOG_INFO("lamps: %u across %u classes", static_cast<unsigned>(m_lampCount), static_cast<unsigned>(classCount));
  return true;
}

void GpuLampTable::Destroy()
{
  m_buffer = nullptr;
  m_ranges.clear();
  m_lamps.clear();
  m_lampCount = 0;
}

D3D12_GPU_VIRTUAL_ADDRESS GpuLampTable::Constants() const noexcept
{
  return m_buffer == nullptr ? 0 : m_buffer->GetGPUVirtualAddress();
}

LampRange GpuLampTable::Range(std::uint32_t _classId) const noexcept
{
  return _classId < m_ranges.size() ? m_ranges[_classId] : LampRange{};
}

} // namespace Neuron
