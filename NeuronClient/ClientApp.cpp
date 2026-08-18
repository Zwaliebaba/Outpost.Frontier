#include "pch.h"

#include "ClientApp.h"

#include "Clock.h"
#include "Log.h"

namespace Neuron
{

ClientApp::~ClientApp()
{
  Shutdown();
}

bool ClientApp::Initialise(const ClientConfig& _config)
{
  m_config = _config;

  WindowDesc windowDesc;
  windowDesc.width = _config.windowWidth;
  windowDesc.height = _config.windowHeight;
  windowDesc.title = _config.windowTitle;
  windowDesc.borderlessFullscreen = _config.borderlessFullscreen;
  if (!m_window.Create(windowDesc))
  {
    return false;
  }

  if (!m_device.Create(_config.enableDebugLayer))
  {
    return false;
  }

  if (!m_swapChain.Create(m_device, m_window.Handle(), m_window.Width(), m_window.Height()))
  {
    return false;
  }

  if (!CreateFrameResources())
  {
    return false;
  }

  m_initialised = true;
  NEURON_LOG_INFO("client initialised (%s, vsync %s)", m_device.AdapterName(), _config.vsync ? "on" : "off");
  return true;
}

bool ClientApp::CreateFrameResources()
{
  // One allocator per back buffer: an allocator cannot be reset while the GPU
  // is still executing commands recorded from it, and the fence value stored
  // beside it is what proves that it is safe.
  for (std::uint32_t i = 0; i < GpuSwapChain::BufferCount; ++i)
  {
    if (FAILED(m_device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_commandAllocators[i].put()))))
    {
      NEURON_LOG_ERROR("CreateCommandAllocator failed for frame %u", i);
      return false;
    }
  }

  if (FAILED(m_device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].get(), nullptr,
                                                 IID_PPV_ARGS(m_commandList.put()))))
  {
    NEURON_LOG_ERROR("CreateCommandList failed");
    return false;
  }
  // Command lists are created open; close it so the loop can treat every frame
  // the same way.
  m_commandList->Close();
  return true;
}

int ClientApp::Run()
{
  if (!m_initialised)
  {
    return 1;
  }

  NEURON_LOG_INFO("entering frame loop");
  while (m_window.PumpMessages())
  {
    HandleResize();

    if (m_window.Minimised())
    {
      // Nothing to present to, and a zero-sized swapchain is an error. Idle
      // politely instead of spinning.
      WaitMessage();
      continue;
    }

    m_swapChain.WaitForFrameLatency();
    RenderFrame();
  }

  NEURON_LOG_INFO("frame loop ended after %llu frames", static_cast<unsigned long long>(m_frameCount));
  return 0;
}

void ClientApp::HandleResize()
{
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (!m_window.ConsumeResize(width, height))
  {
    return;
  }

  // The back buffers are about to be released, so everything referencing them
  // must have finished first.
  m_device.WaitForIdle();
  for (std::uint64_t& value : m_frameFenceValues)
  {
    value = 0;
  }
  if (!m_swapChain.Resize(width, height))
  {
    NEURON_LOG_ERROR("swapchain resize to %ux%u failed", width, height);
  }
}

void ClientApp::RenderFrame()
{
  const std::uint32_t frameIndex = m_swapChain.CurrentIndex();

  // This slot's previous frame must be off the GPU before its allocator is reset.
  m_device.WaitForValue(m_frameFenceValues[frameIndex]);

  ID3D12CommandAllocator* allocator = m_commandAllocators[frameIndex].get();
  allocator->Reset();
  m_commandList->Reset(allocator, nullptr);

  ID3D12Resource* backBuffer = m_swapChain.CurrentBackBuffer();

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = backBuffer;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  m_commandList->ResourceBarrier(1, &barrier);

  const D3D12_CPU_DESCRIPTOR_HANDLE renderTarget = m_swapChain.CurrentRenderTargetView();
  const ClearColour colour = AnimatedClearColour(Clock::SecondsSinceStart());
  const float clear[4] = {colour.red, colour.green, colour.blue, colour.alpha};

  m_commandList->OMSetRenderTargets(1, &renderTarget, FALSE, nullptr);
  m_commandList->ClearRenderTargetView(renderTarget, clear, 0, nullptr);

  // Back to PRESENT: the runtime requires this state at Present time.
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  m_commandList->ResourceBarrier(1, &barrier);

  m_commandList->Close();

  ID3D12CommandList* lists[] = {m_commandList.get()};
  m_device.Queue()->ExecuteCommandLists(1, lists);

  m_swapChain.Present(m_config.vsync);
  m_frameFenceValues[frameIndex] = m_device.Signal();
  ++m_frameCount;
}

void ClientApp::Shutdown()
{
  if (m_device.Device() != nullptr)
  {
    m_device.WaitForIdle(); // Release nothing the GPU is still using.
  }

  m_commandList = nullptr;
  for (GpuPtr<ID3D12CommandAllocator>& allocator : m_commandAllocators)
  {
    allocator = nullptr;
  }

  m_swapChain.Destroy();
  m_device.Destroy();
  m_window.Destroy();
  m_initialised = false;
}

} // namespace Neuron
