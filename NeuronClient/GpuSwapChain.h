#pragma once

#include "GpuCom.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>

/*
 * Flip-model swapchain and its render targets (ADR-006 §2, §12).
 *
 * Flip model is a fixed constraint and it comes with rules: the format cannot
 * be an _SRGB one, so the buffers are R8G8B8A8_UNORM and the render target
 * view is the _SRGB variant -- which is what puts the shading in linear space
 * without a conversion pass.
 */

namespace Neuron
{

class GpuDevice;

class GpuSwapChain
{
public:
  static constexpr std::uint32_t BufferCount = 3;

  GpuSwapChain() = default;
  ~GpuSwapChain();

  GpuSwapChain(const GpuSwapChain&) = delete;
  GpuSwapChain& operator=(const GpuSwapChain&) = delete;

  [[nodiscard]] bool Create(GpuDevice& _device, HWND _window, std::uint32_t _width, std::uint32_t _height);
  void Destroy();

  /// Recreates the buffers at the new size. The caller must have waited for idle.
  [[nodiscard]] bool Resize(std::uint32_t _width, std::uint32_t _height);

  /// Blocks until the swapchain is ready for another frame -- this is the
  /// latency control, and it is why the swapchain is created waitable.
  void WaitForFrameLatency();

  void Present(bool _vsync);

  [[nodiscard]] std::uint32_t CurrentIndex() const noexcept
  {
    return m_currentIndex;
  }
  [[nodiscard]] ID3D12Resource* CurrentBackBuffer() const noexcept
  {
    return m_backBuffers[m_currentIndex].get();
  }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CurrentRenderTargetView() const noexcept;
  [[nodiscard]] std::uint32_t Width() const noexcept
  {
    return m_width;
  }
  [[nodiscard]] std::uint32_t Height() const noexcept
  {
    return m_height;
  }

private:
  void CreateRenderTargets();
  void ReleaseRenderTargets();

  GpuDevice* m_device = nullptr;
  GpuPtr<IDXGISwapChain3> m_swapChain;
  GpuPtr<ID3D12DescriptorHeap> m_rtvHeap;
  GpuPtr<ID3D12Resource> m_backBuffers[BufferCount];
  HANDLE m_frameLatencyWaitable = nullptr;
  std::uint32_t m_rtvStride = 0;
  std::uint32_t m_currentIndex = 0;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  bool m_tearingSupported = false;
};

} // namespace Neuron
