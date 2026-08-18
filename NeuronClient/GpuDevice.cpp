#include "pch.h"

#include "GpuDevice.h"

#include "Log.h"

#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace Neuron
{

GpuDevice::~GpuDevice()
{
  Destroy();
}

bool GpuDevice::Create(bool _enableDebugLayer)
{
  UINT factoryFlags = 0;

  if (_enableDebugLayer)
  {
    // Must happen before the device exists, or the layer is not attached.
    GpuPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put()))))
    {
      debug->EnableDebugLayer();
      factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
      NEURON_LOG_INFO("d3d12 debug layer enabled");
    }
    else
    {
      NEURON_LOG_WARNING("d3d12 debug layer unavailable (graphics tools not installed?)");
    }
  }

  check_hresult(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(m_factory.put())));

  BOOL allowTearing = FALSE;
  if (SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
  {
    m_tearingSupported = allowTearing != FALSE;
  }

  GpuPtr<IDXGIAdapter1> adapter;
  if (!PickAdapter(adapter))
  {
    return false;
  }

  check_hresult(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_device.put())));

  if (_enableDebugLayer)
  {
    // Break where the mistake is made. Corruption and errors only: warnings
    // include plenty the runtime raises for legitimate patterns.
    const GpuPtr<ID3D12InfoQueue> infoQueue = m_device.try_as<ID3D12InfoQueue>();
    if (infoQueue)
    {
      infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
      infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    }
  }

  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  check_hresult(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_queue.put())));

  check_hresult(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put())));

  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (m_fenceEvent == nullptr)
  {
    NEURON_LOG_ERROR("CreateEventW failed (%lu)", GetLastError());
    return false;
  }

  NEURON_LOG_INFO("d3d12 device created on %s (tearing %s)", m_adapterName, m_tearingSupported ? "supported" : "unsupported");
  return true;
}

bool GpuDevice::PickAdapter(GpuPtr<IDXGIAdapter1>& _outAdapter)
{
  // High-performance preference asks the OS which adapter to prefer, which is
  // the right answer on a laptop with two of them.
  for (UINT index = 0;; ++index)
  {
    GpuPtr<IDXGIAdapter1> adapter;
    if (m_factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put())) ==
        DXGI_ERROR_NOT_FOUND)
    {
      break;
    }

    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
    {
      continue;
    }
    // Probe with a null device: cheaper than creating one to find out.
    if (FAILED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
      continue;
    }

    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, m_adapterName, sizeof(m_adapterName) - 1, nullptr, nullptr);
    _outAdapter = adapter;
    return true;
  }

  // WARP keeps the client running on a machine with no usable GPU -- slowly,
  // but well enough to see that everything else works.
  GpuPtr<IDXGIAdapter1> warp;
  if (SUCCEEDED(m_factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put()))))
  {
    NEURON_LOG_WARNING("no hardware adapter supports d3d12; falling back to WARP");
    strncpy_s(m_adapterName, "WARP (software)", _TRUNCATE);
    _outAdapter = warp;
    return true;
  }

  NEURON_LOG_ERROR("no d3d12 adapter available");
  return false;
}

std::uint64_t GpuDevice::Signal()
{
  // Not check_hresult: Signal runs on the shutdown path (Destroy -> WaitForIdle),
  // and throwing during teardown would turn an orderly exit into a terminate.
  ++m_lastSignalled;
  if (FAILED(m_queue->Signal(m_fence.get(), m_lastSignalled)))
  {
    NEURON_LOG_ERROR("queue signal failed");
  }
  return m_lastSignalled;
}

void GpuDevice::WaitForValue(std::uint64_t _value)
{
  if (m_fence == nullptr || m_fence->GetCompletedValue() >= _value)
  {
    return;
  }
  if (SUCCEEDED(m_fence->SetEventOnCompletion(_value, m_fenceEvent)))
  {
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
}

void GpuDevice::WaitForIdle()
{
  if (m_queue != nullptr && m_fence != nullptr)
  {
    WaitForValue(Signal());
  }
}

void GpuDevice::Destroy()
{
  // Nothing may be released while the GPU is still reading it.
  WaitForIdle();

  if (m_fenceEvent != nullptr)
  {
    CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
  }
  m_fence = nullptr;
  m_queue = nullptr;
  m_device = nullptr;
  m_factory = nullptr;
}

} // namespace Neuron
