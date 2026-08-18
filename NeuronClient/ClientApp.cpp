#include "pch.h"

#include "ClientApp.h"

#include "Clock.h"
#include "Log.h"
#include "Telemetry.h"

#include <algorithm>

namespace Neuron
{
namespace
{

using namespace DirectX;

/// One descriptor for the glyph atlas, and room for the per-frame tables the
/// overlay and UI passes will want. Small and fixed: a heap that grows is a
/// heap that has to be rebound mid-frame.
constexpr std::uint32_t SHADER_VISIBLE_DESCRIPTORS = 16;

/// 256 KiB per frame in flight. The parked fleet uses under a kilobyte of it;
/// the number is sized for the corpus's 1,024 instances plus the overlay and
/// text streams that join them, so the first busy frame does not discover a cap.
constexpr std::uint32_t UPLOAD_BYTES_PER_FRAME = 256 * 1024;

/// The HUD's three sizes before the UI scale multiplier (ADR-006 §9).
constexpr float BASE_FONT_SIZES_PIXELS[] = {13.0f, 16.0f, 22.0f};

/*
 * Emissive strength per canonical material (ADR-006 §6).
 *
 * Not content: the .mtl files carry albedo, and which of the five materials
 * *glows* is a renderer decision the exporter has no way to express. Accent and
 * thruster carry the emissive channel; glass is simply dark, which is what
 * makes a cockpit read as glass against a lit hull rather than as a hole.
 */
constexpr float MATERIAL_EMISSIVE[MESH_MATERIAL_COUNT] = {0.0f, 0.0f, 0.0f, 1.6f, 2.4f};

} // namespace

ClientApp::~ClientApp()
{
  Shutdown();
}

bool ClientApp::Initialise(const ClientConfig& _config, const PipelineShaders& _shaders, WorldView& _worldView)
{
  m_config = _config;
  m_shaders = _shaders;
  m_worldView = &_worldView;

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

  CreateFrameResources();

  if (!CreateContent())
  {
    return false;
  }

  m_camera.SetViewport(m_window.Width(), m_window.Height());
  m_camera.SetZoomMetres(_config.cameraZoomMetres);
  m_camera.SetYawSnapDegrees(_config.cameraYawSnapDegrees);
  m_camera.SetFocus(XMFLOAT2{0.0f, 0.0f});

  // After the device: a network failure should not arrive dressed as a
  // graphics one, and the window is worth having either way.
  // Asked of the world view rather than read from configuration (ADR-014 §2).
  // A client told its own content hash cannot detect that its content changed;
  // a client that asks the game reports what it actually loaded.
  if (!m_connection.Connect(_config.serverHost, _config.serverPort, m_worldView->SchemaHash(), m_worldView->ContentHash(),
                            _config.playerName))
  {
    NEURON_LOG_ERROR("could not open a connection to %s:%u", _config.serverHost.c_str(), static_cast<unsigned>(_config.serverPort));
    return false;
  }

  m_initialised = true;
  NEURON_LOG_INFO("client initialised (%s, vsync %s)", m_device.AdapterName(), _config.vsync ? "on" : "off");
  return true;
}

void ClientApp::CreateFrameResources()
{
  // One allocator per back buffer: an allocator cannot be reset while the GPU
  // is still executing commands recorded from it, and the fence value stored
  // beside it is what proves that it is safe.
  for (std::uint32_t i = 0; i < GpuSwapChain::BUFFER_COUNT; ++i)
  {
    check_hresult(m_device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_commandAllocators[i].put())));
  }

  check_hresult(m_device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].get(), nullptr,
                                                     IID_PPV_ARGS(m_commandList.put())));
  // Command lists are created open; close it so the loop can treat every frame
  // the same way.
  check_hresult(m_commandList->Close());

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = SHADER_VISIBLE_DESCRIPTORS;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check_hresult(m_device.Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_srvHeap.put())));
  NAME_D3D12_OBJECT(m_srvHeap);
  m_textureTable = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
}

bool ClientApp::CreateContent()
{
  // Zero asks for hardware_concurrency - 1. A single-core machine gets no
  // workers at all, which is supported: TaskPool::Wait runs the work inline.
  if (!m_taskPool.Start(0))
  {
    NEURON_LOG_ERROR("the boot task pool would not start");
    return false;
  }

  bool ok = m_pipelines.Create(m_device.Device(), m_shaders);
  ok = ok && m_meshes.Create(m_device, m_config.meshDirectory, m_config.meshFiles, m_taskPool);
  ok = ok && m_uploadRing.Create(m_device.Device(), UPLOAD_BYTES_PER_FRAME, GpuSwapChain::BUFFER_COUNT);

  if (ok)
  {
    GlyphAtlas::Desc atlasDesc;
    atlasDesc.fontFamily = m_config.fontFamily;
    atlasDesc.sizesPixels.clear();
    for (float size : BASE_FONT_SIZES_PIXELS)
    {
      atlasDesc.sizesPixels.push_back(size * m_config.uiScale);
    }
    ok = m_glyphAtlas.Create(m_device, atlasDesc, m_taskPool, m_srvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  if (ok)
  {
    // Slot 1, straight after the atlas: t0 and t1 of the root signature's one
    // descriptor table (GpuPipelines::CreateRootSignature).
    D3D12_CPU_DESCRIPTOR_HANDLE nebulaSlot = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    nebulaSlot.ptr += m_device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Not part of `ok`. A configuration that cannot describe a field is a frame
    // without haze, not a client that refuses to start -- the pass checks
    // `nebulaReady` and draws nothing. GpuNebula has already logged why.
    (void)m_nebula.Create(m_device, m_config.nebula, nebulaSlot);
  }

  // Stopped here rather than at shutdown, and that is the rule rather than
  // tidiness: this pool is for boot bakes, and a pool that outlives boot is a
  // pool something will eventually submit a frame's work to (ADR-007 §4).
  m_taskPool.Stop();
  return ok;
}

int ClientApp::Run()
{
  if (!m_initialised)
  {
    return 1;
  }

  // The client's owned lane (ADR-007 §8). Every stage row the corpus debug HUD
  // shows is recorded below; UI is declared and empty until S11.
  (void)Telemetry::RegisterLane("Main");

  m_lastFrameCounter = Clock::Counter();

  NEURON_LOG_INFO("entering frame loop");
  while (m_window.PumpMessages())
  {
    HandleResize();

    if (m_window.Minimised())
    {
      // Nothing to present to, and a zero-sized swapchain is an error. Idle
      // politely instead of spinning. Deliberately outside the Frame span: a
      // blocked WaitMessage is not a slow frame, and recording it as one would
      // put a multi-second maximum in the row that is supposed to say 2 ms.
      WaitMessage();
      // Drained, not kept: a minimised window still receives wheel notches and
      // key edges, and applying a minute of them in one frame on restore would
      // be a camera that teleports.
      (void)m_window.ConsumeInput();
      m_lastFrameCounter = Clock::Counter();
      continue;
    }

    const std::int64_t now = Clock::Counter();
    // Clamped: a frame that took a quarter of a second was a stall, and letting
    // it drive a quarter-second of camera motion would turn every hitch into a
    // lurch.
    const auto deltaSeconds = static_cast<float>(std::min(Clock::SecondsBetween(m_lastFrameCounter, now), 0.25));
    m_lastFrameCounter = now;

    NEURON_SPAN("Frame");
    {
      NEURON_SPAN("Net");
      PollNetwork();
    }
    {
      NEURON_SPAN("Game");
      UpdateCamera(deltaSeconds);
    }
    {
      NEURON_SPAN("Extract");
      ExtractScene();
    }
    {
      // Declared, measured and empty. The Ui pass arrives in S11; the row it
      // reports into is here now so the HUD does not have to add a stage
      // boundary to itself later.
      NEURON_SPAN("Ui");
    }
    m_swapChain.WaitForFrameLatency();
    {
      NEURON_SPAN("Render");
      RenderFrame();
    }
  }

  NEURON_LOG_INFO("frame loop ended after %llu frames", static_cast<unsigned long long>(m_frameCount));
  return 0;
}

void ClientApp::PollNetwork()
{
  m_connection.Poll();

  // Cheap and idempotent, so it costs less than tracking the transition: the
  // rate is zero until `Welcome` lands, and the buffer falls back to the
  // design's 20 Hz rather than dividing by it.
  m_snapshots.Configure(m_connection.ServerTickRate());

  // Every snapshot that arrived goes straight across the seam, oldest first.
  // The engine has framed and ordered them and has not looked inside; what the
  // bytes mean is the world view's business (ADR-014 §5).
  const double nowSeconds = Clock::SecondsSinceStart();
  for (const std::vector<std::uint8_t>& payload : m_connection.PendingSnapshots())
  {
    // The tick comes back from the game, because the game is the only side that
    // can read it. Zero means the payload was rejected, and a rejected snapshot
    // must not move the clock.
    const std::uint32_t tick = m_worldView->ApplySnapshot(payload);
    if (tick != 0)
    {
      m_snapshots.OnSnapshot(tick, nowSeconds);
    }
  }
  m_connection.ClearPendingSnapshots();

  // Once a second, and only while joined: enough to see the link is alive in a
  // log, not enough to bury anything else in it.
  const std::int64_t now = Clock::Counter();
  if (m_connection.State() == ClientLinkState::Joined && Clock::MillisecondsBetween(m_lastNetLogCounter, now) >= 1000.0)
  {
    m_lastNetLogCounter = now;
    NEURON_LOG_DEBUG("net: server tick %u, rtt %.3f ms, %llu pongs", m_connection.ServerTick(), m_connection.RoundTripMs(),
                     static_cast<unsigned long long>(m_connection.PongCount()));
  }
}

void ClientApp::UpdateCamera(float _deltaSeconds)
{
  const InputFrame input = m_window.ConsumeInput();
  const CameraIntent intent = MapCameraInput(input, m_cameraTuning, _deltaSeconds);
  ApplyCameraIntent(m_camera, intent);
}

void ClientApp::ExtractScene()
{
  // The world, from the other side of the seam (ADR-014 §2). This function used
  // to invent a fleet and merge in authored scenery; it now asks for a scene
  // and has no idea what it is getting -- which is the entire point of S5c.
  //
  // The render tick comes from the snapshot buffer: a slew-limited estimate of
  // server time, two ticks back so there is normally a newer snapshot to
  // interpolate toward (ADR-002 §4). Following arrivals directly would be
  // correct on average and visibly jittery, which is the whole reason the
  // buffer exists.
  m_worldView->BuildScene(m_snapshots.Advance(Clock::SecondsSinceStart()), m_scene);
}

FrameConstants ClientApp::BuildFrameConstants() const
{
  FrameConstants constants{};
  constants.viewProjection = m_camera.ViewProjectionMatrix();

  // One directional light from high and behind the default camera, so a hull
  // reads by its top faces and its silhouette rather than by a rim.
  XMFLOAT3 direction;
  XMStoreFloat3(&direction, XMVector3Normalize(XMVectorSet(-0.35f, -0.82f, 0.45f, 0.0f)));
  constants.sunDirection = XMFLOAT4{direction.x, direction.y, direction.z, 0.0f};
  constants.sunColour = XMFLOAT4{0.62f, 0.70f, 0.66f, 1.0f};

  // Near-black space, with just enough sky term that an unlit face is a shape
  // rather than a hole (ADR-006: the look is silhouette, not shading detail).
  constants.ambientSky = XMFLOAT4{0.055f, 0.085f, 0.110f, 1.0f};
  constants.ambientGround = XMFLOAT4{0.014f, 0.018f, 0.024f, 1.0f};

  const MeshMaterialPalette& palette = m_meshes.Palette();
  for (std::uint32_t i = 0; i < MESH_MATERIAL_COUNT; ++i)
  {
    const XMFLOAT3& albedo = palette.albedo[i];
    constants.materialAlbedo[i] = XMFLOAT4{albedo.x, albedo.y, albedo.z, MATERIAL_EMISSIVE[i]};
  }

  // Relationship colour, and only on the emissive channel -- hulls are never
  // tinted (ADR-006 §7). Team 0 is untinted, so a mesh's authored accent green
  // is exactly what the corpus prints show.
  constants.teamEmissive[0] = XMFLOAT4{1.00f, 1.00f, 1.00f, 1.0f};
  constants.teamEmissive[1] = XMFLOAT4{1.00f, 0.42f, 0.28f, 1.0f};
  constants.teamEmissive[2] = XMFLOAT4{0.42f, 0.62f, 1.00f, 1.0f};
  constants.teamEmissive[3] = XMFLOAT4{0.82f, 0.70f, 1.00f, 1.0f};
  return constants;
}

PassConstants ClientApp::BuildPassConstants() const
{
  const XMFLOAT2 right = m_camera.ScreenRightOnPlane();
  const XMFLOAT2 up = m_camera.ScreenUpOnPlane();
  const auto width = static_cast<float>(m_swapChain.Width());
  const auto height = static_cast<float>(m_swapChain.Height());

  // The affine NDC->plane map, which is what anchors the nebula to the world
  // (and what the S8 overlay will want). NeuronClientTests round-trips it
  // against the real view-projection rather than trusting the algebra.
  const PlaneMapping mapping = m_camera.PlaneMappingForNdc();
  const NebulaSettings& nebula = m_config.nebula;
  const float tile = nebula.tileMetres > 0.0f ? 1.0f / nebula.tileMetres : 0.0f;

  PassConstants constants{};
  constants.viewportSize = XMFLOAT4{width, height, width > 0.0f ? 1.0f / width : 0.0f, height > 0.0f ? 1.0f / height : 0.0f};
  constants.planeAxes = XMFLOAT4{right.x, right.y, up.x, up.y};
  constants.planeOrigin = XMFLOAT4{mapping.origin.x, mapping.origin.y, 0.0f, 0.0f};
  constants.planeRightPerNdc = XMFLOAT4{mapping.rightPerNdc.x, mapping.rightPerNdc.y, 0.0f, 0.0f};
  constants.planeUpPerNdc = XMFLOAT4{mapping.upPerNdc.x, mapping.upPerNdc.y, 0.0f, 0.0f};
  constants.nebulaTint = XMFLOAT4{nebula.tintRed, nebula.tintGreen, nebula.tintBlue, nebula.intensity};
  constants.nebulaTile = XMFLOAT4{tile, 0.0f, 0.0f, 0.0f};
  return constants;
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
  m_camera.SetViewport(m_swapChain.Width(), m_swapChain.Height());
}

void ClientApp::RenderFrame()
{
  const std::uint32_t frameIndex = m_swapChain.CurrentIndex();

  // This slot's previous frame must be off the GPU before its allocator is
  // reset -- and before its slice of the upload ring is written over.
  m_device.WaitForValue(m_frameFenceValues[frameIndex]);
  m_uploadRing.BeginFrame(frameIndex);

  ID3D12CommandAllocator* allocator = m_commandAllocators[frameIndex].get();
  check_hresult(allocator->Reset());
  check_hresult(m_commandList->Reset(allocator, nullptr));

  ID3D12Resource* backBuffer = m_swapChain.CurrentBackBuffer();

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = backBuffer;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  m_commandList->ResourceBarrier(1, &barrier);

  ID3D12DescriptorHeap* heaps[] = {m_srvHeap.get()};
  m_commandList->SetDescriptorHeaps(1, heaps);

  // Static, and near-black. The breathing clear was S1's proof that the loop
  // ran when nothing else was on screen; there is a fleet and a nebula on
  // screen now, and a background that changes on its own is a background
  // competing with them (ADR-006 §2).
  const ClearColour colour = SpaceClearColour();

  FrameContext context;
  context.commandList = m_commandList.get();
  context.uploadRing = &m_uploadRing;
  context.pipelines = &m_pipelines;
  context.meshes = &m_meshes;
  context.scene = &m_scene;
  context.renderTargetView = m_swapChain.CurrentRenderTargetView();
  context.depthStencilView = m_swapChain.DepthStencilView();
  context.textureTable = m_textureTable;
  context.viewportWidth = m_swapChain.Width();
  context.viewportHeight = m_swapChain.Height();
  context.clearColour[0] = colour.red;
  context.clearColour[1] = colour.green;
  context.clearColour[2] = colour.blue;
  context.clearColour[3] = colour.alpha;
  context.nebulaReady = m_nebula.Ready();

  // Constants first, so a frame that cannot fit them draws nothing rather than
  // drawing this frame's geometry through last frame's camera.
  const FrameConstants frameConstants = BuildFrameConstants();
  const PassConstants passConstants = BuildPassConstants();
  GpuUploadRing::Allocation frameAllocation;
  GpuUploadRing::Allocation passAllocation;
  const bool constantsReady =
      m_uploadRing.Write(&frameConstants, static_cast<std::uint32_t>(sizeof(frameConstants)),
                         GpuUploadRing::CONSTANT_BUFFER_ALIGNMENT, frameAllocation) &&
      m_uploadRing.Write(&passConstants, static_cast<std::uint32_t>(sizeof(passConstants)),
                         GpuUploadRing::CONSTANT_BUFFER_ALIGNMENT, passAllocation);
  if (constantsReady)
  {
    context.frameConstants = frameAllocation.gpu;
    context.passConstants = passAllocation.gpu;
  }
  else
  {
    context.scene = nullptr; // Clear and present; the ring already logged why.
  }

  m_passes.Record(context);

  // Back to PRESENT: the runtime requires this state at Present time.
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  m_commandList->ResourceBarrier(1, &barrier);

  check_hresult(m_commandList->Close());

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

  m_taskPool.Stop(); // Idempotent; boot normally stopped it already.

  m_glyphAtlas.Destroy();
  m_meshes.Destroy();
  m_uploadRing.Destroy();
  m_pipelines.Destroy();
  m_srvHeap = nullptr;

  m_commandList = nullptr;
  for (GpuPtr<ID3D12CommandAllocator>& allocator : m_commandAllocators)
  {
    allocator = nullptr;
  }

  m_connection.Disconnect(); // Say goodbye before the socket goes.
  m_swapChain.Destroy();
  m_device.Destroy();
  m_window.Destroy();
  m_initialised = false;
}

} // namespace Neuron
