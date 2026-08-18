#pragma once

#include "ClientConfig.h"
#include "GpuCom.h"
#include "GpuDevice.h"
#include "GpuSwapChain.h"
#include "Window.h"

#include <cstdint>

/*
 * The client (ADR-007 §1): window, device and frame loop, all on the thread
 * that pumps messages.
 *
 * Slice S1 renders a clear colour and nothing else. The stages the debug HUD
 * reports -- GAME, EXTRACT, RENDER, UI -- arrive with the world; what exists
 * here is the loop they will hang from, and the seam ADR-014 defined
 * (a WorldView handed in by the composition root) lands in S5c.
 */

namespace Neuron
{

/// The animated clear colour, factored out so it can be tested without a device.
struct ClearColour
{
  float red = 0.0f;
  float green = 0.0f;
  float blue = 0.0f;
  float alpha = 1.0f;
};

/// Slow breathing between two near-black blues -- proof the loop is running
/// without pretending to be art (the real look arrives with ADR-006's passes).
[[nodiscard]] ClearColour AnimatedClearColour(double _seconds) noexcept;

class ClientApp
{
public:
  ClientApp() = default;
  ~ClientApp();

  ClientApp(const ClientApp&) = delete;
  ClientApp& operator=(const ClientApp&) = delete;

  [[nodiscard]] bool Initialise(const ClientConfig& _config);

  /// Runs until the window closes. Returns a process exit code.
  [[nodiscard]] int Run();

  void Shutdown();

private:
  [[nodiscard]] bool CreateFrameResources();
  void RenderFrame();
  void HandleResize();

  ClientConfig m_config;
  Window m_window;
  GpuDevice m_device;
  GpuSwapChain m_swapChain;

  GpuPtr<ID3D12CommandAllocator> m_commandAllocators[GpuSwapChain::BufferCount];
  GpuPtr<ID3D12GraphicsCommandList> m_commandList;
  std::uint64_t m_frameFenceValues[GpuSwapChain::BufferCount] = {};

  std::uint64_t m_frameCount = 0;
  bool m_initialised = false;
};

} // namespace Neuron
