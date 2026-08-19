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
 *
 * The depth buffer lives here too, and that is a deliberate choice rather than
 * a convenience: it is the one other resource whose size is the swapchain's
 * size, so a resize that recreated the back buffers and forgot the depth
 * buffer would be a mismatched-extent error at the first draw. One owner, one
 * Resize, one place to get it right.
 */

namespace Neuron
{

class GpuDevice;

class GpuSwapChain
{
public:
  static constexpr std::uint32_t BUFFER_COUNT = 3;

  /// What the MSAA target resolves as (S14). The world renders through an sRGB
  /// view, but the back buffer is typed UNORM and a resolve's format must match
  /// the typed resource exactly -- so the resolve averages in gamma space,
  /// which on a near-black scene is a subtler wrong than a debug-layer error.
  static constexpr DXGI_FORMAT RESOLVE_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

  GpuSwapChain() = default;
  ~GpuSwapChain();

  GpuSwapChain(const GpuSwapChain&) = delete;
  GpuSwapChain& operator=(const GpuSwapChain&) = delete;

  /*
   * `_msaaSamples` asks for the offscreen MSAA colour target and a matching
   * depth buffer (S14, ADR-006 §13). Asked for, not demanded: an unsupported
   * count falls back to 1 with a log line, because "this GPU cannot do 4x"
   * is a frame without smoothing, not a client that refuses to start.
   * `SampleCount()` reports what was actually built.
   */
  [[nodiscard]] bool Create(GpuDevice& _device, HWND _window, std::uint32_t _width, std::uint32_t _height,
                            std::uint32_t _msaaSamples = 1);
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

  /// D32_FLOAT, written by Opaque and read test-only by OverlayWorld (ADR-006 §2).
  /// Multisampled to match the colour target when MSAA is on.
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const noexcept;
  [[nodiscard]] ID3D12Resource* DepthBuffer() const noexcept
  {
    return m_depthBuffer.get();
  }

  /// 1 when MSAA is off or unsupported; otherwise what was asked for.
  [[nodiscard]] std::uint32_t SampleCount() const noexcept
  {
    return m_sampleCount;
  }

  /*
   * The offscreen MSAA colour target and its view (S14). Valid only when
   * `SampleCount() > 1`. The world passes render here and the frame loop
   * resolves into the back buffer before the Ui pass; the resource is left in
   * RENDER_TARGET state between frames, so the frame loop's two transitions
   * around the resolve are the only barriers it ever needs.
   */
  [[nodiscard]] ID3D12Resource* MsaaColour() const noexcept
  {
    return m_msaaColour.get();
  }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE MsaaRenderTargetView() const noexcept;

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
  void CreateDepthBuffer();
  void CreateMsaaColourTarget();
  [[nodiscard]] std::uint32_t ResolveSampleCount(std::uint32_t _requested) const;

  GpuDevice* m_device = nullptr;
  GpuPtr<IDXGISwapChain3> m_swapChain;
  GpuPtr<ID3D12DescriptorHeap> m_rtvHeap;
  GpuPtr<ID3D12DescriptorHeap> m_dsvHeap;
  GpuPtr<ID3D12Resource> m_backBuffers[BUFFER_COUNT];
  GpuPtr<ID3D12Resource> m_depthBuffer;
  GpuPtr<ID3D12Resource> m_msaaColour;
  HANDLE m_frameLatencyWaitable = nullptr;
  std::uint32_t m_rtvStride = 0;
  std::uint32_t m_currentIndex = 0;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  std::uint32_t m_sampleCount = 1;
  bool m_tearingSupported = false;
};

} // namespace Neuron
