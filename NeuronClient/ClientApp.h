#pragma once

#include "ClearColour.h"
#include "ClientConfig.h"
#include "ClientConnection.h"
#include "GlyphAtlas.h"
#include "GpuCom.h"
#include "GpuDevice.h"
#include "GpuMeshes.h"
#include "GpuPasses.h"
#include "GpuPipelines.h"
#include "GpuSwapChain.h"
#include "GpuUploadRing.h"
#include "InputMap.h"
#include "IsoCamera.h"
#include "RenderWorld.h"
#include "TaskPool.h"
#include "Window.h"

#include <cstdint>

/*
 * The client (ADR-007 §1): window, device and frame loop, all on the thread
 * that pumps messages.
 *
 * The frame is the corpus's stage list, and from slice S5 every stage it has
 * is named and measured: NET, GAME, EXTRACT, RENDER, and a UI stage that is
 * declared and empty until S11 gives it something to draw. The debug HUD reads
 * those rows straight out of the telemetry lanes (ADR-007 §8), so the stage
 * boundaries have to exist before the HUD does -- retrofitting a measurement
 * means retrofitting the boundary it measures.
 *
 * What is deliberately still missing: the world. S5 renders a parked fleet the
 * client makes up for itself, because the seam that supplies a real one
 * (WorldView, ADR-014) lands in S5c and snapshots in S7.
 */

namespace Neuron
{

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
  void CreateFrameResources();
  [[nodiscard]] bool CreateContent();
  void PollNetwork();
  void UpdateCamera(float _deltaSeconds);
  void ExtractScene();
  void RenderFrame();
  void HandleResize();

  [[nodiscard]] FrameConstants BuildFrameConstants() const;
  [[nodiscard]] PassConstants BuildPassConstants() const;

  ClientConfig m_config;
  Window m_window;
  ClientConnection m_connection;
  GpuDevice m_device;
  GpuSwapChain m_swapChain;

  /// Boot only (ADR-007 §4): mesh parsing and the glyph bake. It is started
  /// before the content load and stopped straight after, so it cannot quietly
  /// become a frame-time pool.
  TaskPool m_taskPool;

  GpuPipelines m_pipelines;
  GpuMeshTable m_meshes;
  GlyphAtlas m_glyphAtlas;
  GpuUploadRing m_uploadRing;
  GpuPassList m_passes;

  /// The one shader-visible CBV/SRV heap (ADR-006 §12). The atlas takes slot 0
  /// and the per-frame tables the later passes need take the rest.
  GpuPtr<ID3D12DescriptorHeap> m_srvHeap;
  D3D12_GPU_DESCRIPTOR_HANDLE m_textureTable{};

  IsoCamera m_camera;
  CameraTuning m_cameraTuning;
  RenderScene m_scene;

  GpuPtr<ID3D12CommandAllocator> m_commandAllocators[GpuSwapChain::BUFFER_COUNT];
  GpuPtr<ID3D12GraphicsCommandList> m_commandList;
  std::uint64_t m_frameFenceValues[GpuSwapChain::BUFFER_COUNT] = {};

  std::uint64_t m_frameCount = 0;
  std::int64_t m_lastFrameCounter = 0;
  std::int64_t m_lastNetLogCounter = 0;
  bool m_initialised = false;
};

} // namespace Neuron
