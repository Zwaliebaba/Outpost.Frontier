#pragma once

#include "GpuCom.h"

#include <d3d12.h>

#include <cstdint>

/*
 * The per-frame linear upload allocator (ADR-006 §12).
 *
 * One upload-heap buffer, split into one segment per frame in flight and
 * persistently mapped. A frame bumps a cursor through its own segment and never
 * frees: the fence that lets the frame's command allocator be reset is the same
 * fence that says the segment is free, so there is nothing else to track.
 *
 * Deliberately not a general allocator. Constants and instance data are written
 * once, read once, and dead at the end of the frame -- which is exactly the
 * lifetime a linear allocator is for, and nothing here has any other lifetime.
 *
 * Exhaustion returns false rather than growing. A frame that asks for more than
 * its segment holds is a frame with a bug in it, and growing under it would hide
 * the bug behind a stall.
 */

namespace Neuron
{

class GpuUploadRing
{
public:
  /// Constant buffers must start on a 256-byte boundary; vertex data has no such
  /// rule, so instance streams pass their own alignment.
  static constexpr std::uint32_t CONSTANT_BUFFER_ALIGNMENT = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

  struct Allocation
  {
    void* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    std::uint32_t bytes = 0;
  };

  GpuUploadRing() = default;
  ~GpuUploadRing();

  GpuUploadRing(const GpuUploadRing&) = delete;
  GpuUploadRing& operator=(const GpuUploadRing&) = delete;

  [[nodiscard]] bool Create(ID3D12Device* _device, std::uint32_t _bytesPerFrame, std::uint32_t _frameCount);
  void Destroy();

  /// Resets the cursor to _frameIndex's segment. The caller has already waited
  /// on the fence for that slot, which is what makes the reset safe.
  void BeginFrame(std::uint32_t _frameIndex) noexcept;

  [[nodiscard]] bool Allocate(std::uint32_t _bytes, std::uint32_t _alignment, Allocation& _outAllocation) noexcept;

  /// Copies straight in. The upload heap is write-combined, so this must be a
  /// linear forward write and never a read.
  [[nodiscard]] bool Write(const void* _data, std::uint32_t _bytes, std::uint32_t _alignment, Allocation& _outAllocation) noexcept;

  [[nodiscard]] std::uint32_t BytesPerFrame() const noexcept { return m_bytesPerFrame; }

  /// High-water mark across every frame since Create, for the debug strip.
  [[nodiscard]] std::uint32_t PeakBytesUsed() const noexcept { return m_peakBytesUsed; }

private:
  GpuPtr<ID3D12Resource> m_buffer;
  std::uint8_t* m_mapped = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS m_baseAddress = 0;
  std::uint32_t m_bytesPerFrame = 0;
  std::uint32_t m_frameCount = 0;
  std::uint32_t m_segmentBase = 0;
  std::uint32_t m_cursor = 0;
  std::uint32_t m_peakBytesUsed = 0;
};

} // namespace Neuron
