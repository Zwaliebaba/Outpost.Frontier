#include "pch.h"

#include "GpuUploadRing.h"

#include "Log.h"

#include <algorithm>
#include <cstring>

namespace Neuron
{
namespace
{

[[nodiscard]] std::uint32_t AlignUp(std::uint32_t _value, std::uint32_t _alignment) noexcept
{
  return _alignment <= 1 ? _value : (_value + _alignment - 1) & ~(_alignment - 1);
}

} // namespace

GpuUploadRing::~GpuUploadRing()
{
  Destroy();
}

bool GpuUploadRing::Create(ID3D12Device* _device, std::uint32_t _bytesPerFrame, std::uint32_t _frameCount)
{
  if (_device == nullptr || _bytesPerFrame == 0 || _frameCount == 0)
  {
    return false;
  }

  Destroy();

  // Every segment starts constant-buffer aligned, so a frame's first CBV needs
  // no padding and the arithmetic below stays exact.
  m_bytesPerFrame = AlignUp(_bytesPerFrame, CONSTANT_BUFFER_ALIGNMENT);
  m_frameCount = _frameCount;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = static_cast<UINT64>(m_bytesPerFrame) * m_frameCount;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  check_hresult(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(m_buffer.put())));
  NAME_D3D12_OBJECT(m_buffer);

  // Mapped for the resource's whole life. An upload buffer written every frame
  // would otherwise pay a map/unmap pair per frame for nothing: the read range
  // is empty because the CPU never reads it back.
  const D3D12_RANGE readRange{0, 0};
  check_hresult(m_buffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mapped)));
  m_baseAddress = m_buffer->GetGPUVirtualAddress();

  NEURON_LOG_INFO("upload ring: %u KiB x %u frames", m_bytesPerFrame / 1024, m_frameCount);
  return true;
}

void GpuUploadRing::Destroy()
{
  if (m_buffer != nullptr && m_mapped != nullptr)
  {
    // Unmapped with an empty written range only because the whole resource is
    // going away; the written data is already consumed by a completed frame.
    const D3D12_RANGE writtenRange{0, 0};
    m_buffer->Unmap(0, &writtenRange);
  }
  m_mapped = nullptr;
  m_buffer = nullptr;
  m_baseAddress = 0;
  m_bytesPerFrame = 0;
  m_frameCount = 0;
  m_segmentBase = 0;
  m_cursor = 0;
}

void GpuUploadRing::BeginFrame(std::uint32_t _frameIndex) noexcept
{
  if (m_frameCount == 0)
  {
    return;
  }
  m_segmentBase = (_frameIndex % m_frameCount) * m_bytesPerFrame;
  m_cursor = 0;
}

bool GpuUploadRing::Allocate(std::uint32_t _bytes, std::uint32_t _alignment, Allocation& _outAllocation) noexcept
{
  if (m_mapped == nullptr || _bytes == 0)
  {
    return false;
  }

  const std::uint32_t offset = AlignUp(m_cursor, _alignment);
  if (offset > m_bytesPerFrame || _bytes > m_bytesPerFrame - offset)
  {
    NEURON_LOG_ERROR("upload ring exhausted: %u bytes requested with %u of %u used", _bytes, m_cursor, m_bytesPerFrame);
    return false;
  }

  _outAllocation.cpu = m_mapped + m_segmentBase + offset;
  _outAllocation.gpu = m_baseAddress + m_segmentBase + offset;
  _outAllocation.bytes = _bytes;

  m_cursor = offset + _bytes;
  m_peakBytesUsed = std::max(m_peakBytesUsed, m_cursor);
  return true;
}

bool GpuUploadRing::Write(const void* _data, std::uint32_t _bytes, std::uint32_t _alignment, Allocation& _outAllocation) noexcept
{
  if (!Allocate(_bytes, _alignment, _outAllocation))
  {
    return false;
  }
  std::memcpy(_outAllocation.cpu, _data, _bytes);
  return true;
}

} // namespace Neuron
