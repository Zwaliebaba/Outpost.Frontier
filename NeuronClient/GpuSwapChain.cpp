#include "pch.h"

#include "GpuSwapChain.h"

#include "GpuDevice.h"
#include "Log.h"

namespace Neuron
{
namespace
{

constexpr DXGI_FORMAT BACK_BUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT RENDER_TARGET_VIEW_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
constexpr DXGI_FORMAT DEPTH_BUFFER_FORMAT = DXGI_FORMAT_D32_FLOAT;

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
  desc.Format = BACK_BUFFER_FORMAT;
  desc.SampleDesc.Count = 1; // Flip model forbids multisampled buffers; MSAA resolves into these.
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = BUFFER_COUNT;
  desc.Scaling = DXGI_SCALING_NONE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (m_tearingSupported)
  {
    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  GpuPtr<IDXGISwapChain1> swapChain1;
  check_hresult(_device.Factory()->CreateSwapChainForHwnd(_device.Queue(), _window, &desc, nullptr, nullptr, swapChain1.put()));

  // DXGI's own Alt+Enter would fight the client's fullscreen handling.
  _device.Factory()->MakeWindowAssociation(_window, DXGI_MWA_NO_ALT_ENTER);

  m_swapChain = swapChain1.try_as<IDXGISwapChain3>();
  if (!m_swapChain)
  {
    NEURON_LOG_ERROR("the swapchain does not implement IDXGISwapChain3");
    return false;
  }

  // Two frames of latency: enough to keep the GPU fed, few enough that input
  // does not lag behind what is on screen.
  m_swapChain->SetMaximumFrameLatency(2);
  m_frameLatencyWaitable = m_swapChain->GetFrameLatencyWaitableObject();

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.NumDescriptors = BUFFER_COUNT;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // RTVs are never shader-visible.
  check_hresult(_device.Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_rtvHeap.put())));
  m_rtvStride = _device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  D3D12_DESCRIPTOR_HEAP_DESC depthHeapDesc{};
  depthHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  depthHeapDesc.NumDescriptors = 1; // One depth buffer, shared by every frame:
                                    // it is cleared at the top of each one, so
                                    // there is nothing to double-buffer.
  depthHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  check_hresult(_device.Device()->CreateDescriptorHeap(&depthHeapDesc, IID_PPV_ARGS(m_dsvHeap.put())));

  CreateRenderTargets();
  CreateDepthBuffer();
  return true;
}

void GpuSwapChain::CreateDepthBuffer()
{
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = m_width;
  desc.Height = m_height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DEPTH_BUFFER_FORMAT;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  // The clear value must match what the pass actually clears to, or the driver
  // loses its fast-clear path and the debug layer says so every frame.
  D3D12_CLEAR_VALUE clear{};
  clear.Format = DEPTH_BUFFER_FORMAT;
  clear.DepthStencil.Depth = 1.0f;

  check_hresult(m_device->Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                            IID_PPV_ARGS(m_depthBuffer.put())));
  NAME_D3D12_OBJECT(m_depthBuffer);

  D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc{};
  viewDesc.Format = DEPTH_BUFFER_FORMAT;
  viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  m_device->Device()->CreateDepthStencilView(m_depthBuffer.get(), &viewDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void GpuSwapChain::CreateRenderTargets()
{
  D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
  viewDesc.Format = RENDER_TARGET_VIEW_FORMAT; // sRGB view over a UNORM buffer.
  viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

  D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (std::uint32_t i = 0; i < BUFFER_COUNT; ++i)
  {
    check_hresult(m_swapChain->GetBuffer(i, IID_PPV_ARGS(m_backBuffers[i].put())));
    m_device->Device()->CreateRenderTargetView(m_backBuffers[i].get(), &viewDesc, handle);
    handle.ptr += m_rtvStride;
  }

  m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void GpuSwapChain::ReleaseRenderTargets()
{
  for (GpuPtr<ID3D12Resource>& buffer : m_backBuffers)
  {
    // put() asserts the pointer is empty, so every buffer must be released
    // before CreateRenderTargets fills them again.
    buffer = nullptr;
  }
  m_depthBuffer = nullptr; // Same rule, and it is the swapchain's size too.
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

  check_hresult(m_swapChain->ResizeBuffers(BUFFER_COUNT, _width, _height, BACK_BUFFER_FORMAT, flags));

  m_width = _width;
  m_height = _height;
  NEURON_LOG_DEBUG("swapchain resized to %ux%u", _width, _height);
  CreateRenderTargets();
  CreateDepthBuffer();
  return true;
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

D3D12_CPU_DESCRIPTOR_HANDLE GpuSwapChain::DepthStencilView() const noexcept
{
  return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void GpuSwapChain::Destroy()
{
  ReleaseRenderTargets();
  m_rtvHeap = nullptr;
  m_dsvHeap = nullptr;
  if (m_frameLatencyWaitable != nullptr)
  {
    CloseHandle(m_frameLatencyWaitable);
    m_frameLatencyWaitable = nullptr;
  }
  m_swapChain = nullptr;
  m_device = nullptr;
}

} // namespace Neuron
