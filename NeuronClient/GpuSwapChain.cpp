#include "pch.h"

#include "GpuSwapChain.h"

#include "GpuDevice.h"
#include "Log.h"

namespace Neuron
{
namespace
{

constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT RenderTargetViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

bool Check(HRESULT _result, const char* _what)
{
  if (FAILED(_result))
  {
    NEURON_LOG_ERROR("%s failed (hr 0x%08lx)", _what, static_cast<unsigned long>(_result));
    return false;
  }
  return true;
}

} // namespace

GpuSwapChain::~GpuSwapChain()
{
  Destroy();
}

bool GpuSwapChain::Create(GpuDevice& _device, HWND _window, std::uint32_t _width, std::uint32_t _height)
{
  m_device = &_device;
  m_width = _width;
  m_height = _height;
  m_tearingSupported = _device.TearingSupported();

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = _width;
  desc.Height = _height;
  desc.Format = BackBufferFormat;
  desc.SampleDesc.Count = 1; // Flip model forbids multisampled buffers; MSAA resolves into these.
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = BufferCount;
  desc.Scaling = DXGI_SCALING_NONE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (m_tearingSupported)
  {
    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  GpuPtr<IDXGISwapChain1> swapChain1;
  if (!Check(_device.Factory()->CreateSwapChainForHwnd(_device.Queue(), _window, &desc, nullptr, nullptr, &swapChain1),
             "CreateSwapChainForHwnd"))
  {
    return false;
  }

  // DXGI's own Alt+Enter would fight the client's fullscreen handling.
  _device.Factory()->MakeWindowAssociation(_window, DXGI_MWA_NO_ALT_ENTER);

  if (!Check(swapChain1.As(&m_swapChain), "IDXGISwapChain3 query"))
  {
    return false;
  }

  // Two frames of latency: enough to keep the GPU fed, few enough that input
  // does not lag behind what is on screen.
  m_swapChain->SetMaximumFrameLatency(2);
  m_frameLatencyWaitable = m_swapChain->GetFrameLatencyWaitableObject();

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.NumDescriptors = BufferCount;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // RTVs are never shader-visible.
  if (!Check(_device.Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap)), "CreateDescriptorHeap"))
  {
    return false;
  }
  m_rtvStride = _device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  return CreateRenderTargets();
}

bool GpuSwapChain::CreateRenderTargets()
{
  D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
  viewDesc.Format = RenderTargetViewFormat; // sRGB view over a UNORM buffer.
  viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

  D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (std::uint32_t i = 0; i < BufferCount; ++i)
  {
    if (!Check(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])), "swapchain GetBuffer"))
    {
      return false;
    }
    m_device->Device()->CreateRenderTargetView(m_backBuffers[i].Get(), &viewDesc, handle);
    handle.ptr += m_rtvStride;
  }

  m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
  return true;
}

void GpuSwapChain::ReleaseRenderTargets()
{
  for (GpuPtr<ID3D12Resource>& buffer : m_backBuffers)
  {
    buffer.Reset();
  }
}

bool GpuSwapChain::Resize(std::uint32_t _width, std::uint32_t _height)
{
  if (m_swapChain == nullptr || _width == 0 || _height == 0)
  {
    return false;
  }
  if (_width == m_width && _height == m_height)
  {
    return true;
  }

  // Every reference to a back buffer must be gone before ResizeBuffers, or it
  // fails with DXGI_ERROR_INVALID_CALL and the message says nothing useful.
  ReleaseRenderTargets();

  UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (m_tearingSupported)
  {
    flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  if (!Check(m_swapChain->ResizeBuffers(BufferCount, _width, _height, BackBufferFormat, flags), "ResizeBuffers"))
  {
    return false;
  }

  m_width = _width;
  m_height = _height;
  NEURON_LOG_DEBUG("swapchain resized to %ux%u", _width, _height);
  return CreateRenderTargets();
}

void GpuSwapChain::WaitForFrameLatency()
{
  if (m_frameLatencyWaitable != nullptr)
  {
    WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, TRUE);
  }
}

void GpuSwapChain::Present(bool _vsync)
{
  // Tearing is only legal with no vsync, and only when the swapchain was
  // created for it.
  const UINT interval = _vsync ? 1 : 0;
  const UINT flags = (!_vsync && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;

  const HRESULT result = m_swapChain->Present(interval, flags);
  if (FAILED(result))
  {
    NEURON_LOG_ERROR("Present failed (hr 0x%08lx)", static_cast<unsigned long>(result));
    return;
  }
  m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuSwapChain::CurrentRenderTargetView() const noexcept
{
  D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(m_currentIndex) * m_rtvStride;
  return handle;
}

void GpuSwapChain::Destroy()
{
  ReleaseRenderTargets();
  m_rtvHeap.Reset();
  if (m_frameLatencyWaitable != nullptr)
  {
    CloseHandle(m_frameLatencyWaitable);
    m_frameLatencyWaitable = nullptr;
  }
  m_swapChain.Reset();
  m_device = nullptr;
}

} // namespace Neuron
