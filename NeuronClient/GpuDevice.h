#pragma once

#include "GpuCom.h"

#include <cstdint>

/*
 * The D3D12 device, its direct queue, and frame synchronisation (ADR-006 §12).
 *
 * Deliberately small: one direct queue, one fence. There is no compute queue
 * and no copy queue because nothing in the MVP frame would use them, and an
 * idle queue is machinery that still has to be synchronised correctly.
 */

namespace Neuron
{

class GpuDevice
{
public:
  GpuDevice() = default;
  ~GpuDevice();

  GpuDevice(const GpuDevice&) = delete;
  GpuDevice& operator=(const GpuDevice&) = delete;

  /// _enableDebugLayer costs a lot of CPU and is worth every bit of it in dev.
  [[nodiscard]] bool Create(bool _enableDebugLayer);
  void Destroy();

  [[nodiscard]] ID3D12Device* Device() const noexcept { return m_device.Get(); }
  [[nodiscard]] ID3D12CommandQueue* Queue() const noexcept { return m_queue.Get(); }
  [[nodiscard]] IDXGIFactory6* Factory() const noexcept { return m_factory.Get(); }
  [[nodiscard]] bool TearingSupported() const noexcept { return m_tearingSupported; }
  [[nodiscard]] const char* AdapterName() const noexcept { return m_adapterName; }

  /// Signals the fence and returns the value to wait on later.
  std::uint64_t Signal();
  void WaitForValue(std::uint64_t _value);

  /// Blocks until the GPU has finished everything. Only for resize and teardown.
  void WaitForIdle();

private:
  [[nodiscard]] bool PickAdapter(GpuPtr<IDXGIAdapter1>& _outAdapter);

  GpuPtr<IDXGIFactory6> m_factory;
  GpuPtr<ID3D12Device> m_device;
  GpuPtr<ID3D12CommandQueue> m_queue;
  GpuPtr<ID3D12Fence> m_fence;
  HANDLE m_fenceEvent = nullptr;
  std::uint64_t m_lastSignalled = 0;
  bool m_tearingSupported = false;
  char m_adapterName[128] = {};
};

} // namespace Neuron
