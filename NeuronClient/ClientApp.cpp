#include "pch.h"

#include "ClientApp.h"

#include "Clock.h"
#include "Log.h"
#include "Telemetry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Neuron
{
namespace
{

using namespace DirectX;

/*
 * The HUD's palette, packed r-in-the-low-byte like every other colour the
 * client sends (`OverlayMark::colourRgba`).
 *
 * The prints are a dark panel with green text and an amber accent for anything
 * the player has to act on, and these are those four. Here rather than in
 * `UiTuning` because a colour is not a size: the scale control retunes sizes,
 * and nothing retunes these.
 */
constexpr std::uint32_t PANEL_COLOUR = 0xd8140f0au;       // Near-black, mostly opaque.
constexpr std::uint32_t PANEL_EDGE_COLOUR = 0x60403020u;
constexpr std::uint32_t TEXT_COLOUR = 0xff64e664u;        // The sheets' green.
constexpr std::uint32_t TEXT_DIM_COLOUR = 0xa050a050u;
constexpr std::uint32_t TOAST_URGENT_COLOUR = 0xff20b4ffu; // Amber: act on this.
constexpr std::uint32_t TOAST_CRITICAL_COLOUR = 0xff4040ffu; // Red: act on it now.
constexpr std::uint32_t TEXT_DISABLED_COLOUR = 0x80303030u;  // A control with nothing behind it.
constexpr std::uint32_t ROW_SELECTED_COLOUR = 0x40206020u;   // The roster's highlight.
constexpr std::uint32_t BAR_TRACK_COLOUR = 0x60202020u;      // What an empty strip looks like.

/// The roster's strips, and the same two colours the world-space gauge bars
/// use (`OverlayTuning`): a wing's row and its ships' bars must not disagree
/// about which one is hull.
constexpr std::uint32_t HULL_COLOUR = 0xff50e050u;
constexpr std::uint32_t SHIELD_COLOUR = 0xff40c0e0u;

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

  // What the puck's orders are, from the side that knows. Once, at boot: there
  // is one command until the wheel exists, and asking every frame would be
  // asking a question whose answer is compiled in.
  m_orderDefaults = _worldView.DefaultOrder();
  m_orderOptionCount = _worldView.OrderOptions(m_orderDefaults.kind, m_orderOptions);

  // Start on the game's own default rather than on the list's first entry.
  // They are the same today, and "the default is whatever happens to be first"
  // is the kind of agreement that holds until someone reorders a menu.
  for (std::uint32_t index = 0; index < m_orderOptionCount; ++index)
  {
    if (m_orderOptions[index].parameter == m_orderDefaults.parameter)
    {
      m_orderOptionIndex = index;
      break;
    }
  }

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
      UpdateSelection();
      UpdateOrders();
    }
    {
      NEURON_SPAN("Extract");
      ExtractScene();
    }
    BuildHud();
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
  m_input = m_window.ConsumeInput();
  const CameraIntent intent = MapCameraInput(m_input, m_cameraTuning, _deltaSeconds);
  ApplyCameraIntent(m_camera, intent);
}

/*
 * Click, shift-click and box-select (ADR-006 §11).
 *
 * Runs before `ExtractScene`, and that is the interesting part: it tests
 * against `m_scene.entities` as the *previous* frame left them, which is the
 * arrangement of ships the player was actually looking at when they pressed the
 * button. Resolving against the scene built after the click would test against
 * a world up to a frame newer than the one on screen -- a fraction of a pixel
 * at tactical zoom and a real one at 40 km, and either way an answer to a
 * question nobody asked.
 *
 * Nothing here reaches the server. A selection is a client-side fact until an
 * order names it (ADR-006 §11: no round trip).
 */
void ClientApp::UpdateSelection()
{
  if (!m_input.windowFocused)
  {
    // A drag that was interrupted by alt-tab has no release coming, and
    // finishing it on the next click would apply a box the player drew a
    // minute ago across a camera move.
    m_selection.CancelDrag();
    return;
  }

  const auto cursorX = static_cast<float>(m_input.cursorX);
  const auto cursorY = static_cast<float>(m_input.cursorY);

  if (m_input.Pressed(InputButton::Left))
  {
    m_selection.BeginDrag(cursorX, cursorY, m_input.Down(InputAction::SelectAdd));
  }
  else if (m_selection.Dragging())
  {
    m_selection.UpdateDrag(cursorX, cursorY);
  }

  if (m_input.Released(InputButton::Left) && m_selection.Dragging())
  {
    m_selection.EndDrag(m_scene.entities, m_camera.PlaneMappingForNdc(), m_input.viewportWidth, m_input.viewportHeight,
                        m_camera.ScreenFloorMetres(Selection::PICK_FLOOR_PIXELS));
  }
}

void ClientApp::UpdateOrders()
{
  const double nowSeconds = Clock::SecondsSinceStart();

  /*
   * Answers first, gesture second, and the order matters.
   *
   * A ghost the authority decided about this frame should be promoted or
   * bounced before a new one is added, so the two never contend for the same
   * sequence and a bounce is never one frame stale. The gesture is the only
   * part that can *create* work, so it goes last.
   */
  for (const OrderVerdict& verdict : m_connection.PendingVerdicts())
  {
    m_ghosts.OnVerdict(verdict, nowSeconds);
    if (!verdict.accepted)
    {
      // The bounce toast (`alerts-and-toasts.png` §3), which the sheet is
      // explicit is not a new component: it is the same reason string the
      // 150 ms ghost bounce is already showing, on the second of the two
      // surfaces one refusal owes. Keyed on the reason code, so a burst of
      // out-of-bounds clicks is one row with a count rather than five rows.
      const char* reason = m_worldView->ReasonText(verdict.reasonCode);
      (void)m_toasts.Raise(ToastPriority::Urgent, verdict.reasonCode, "ORDER REJECTED", reason, nowSeconds);
      NEURON_LOG_INFO("order %u refused by the server: %s", verdict.orderSeq, reason);
    }
  }
  m_connection.ClearPendingVerdicts();

  // What the newest snapshot says is still running. This is what promotes a
  // ghost when the ack was lost, and what retires one whose order has finished.
  m_worldView->PollOrderFeedback(m_orderFeedback);
  m_ghosts.OnFeedback(m_orderFeedback, nowSeconds);
  m_ghosts.Advance(nowSeconds);

  if (m_connection.State() != ClientLinkState::Joined && !m_ghosts.Empty())
  {
    // The link went away. Every promise it was carrying died with it, and the
    // sequence numbers restart on a reconnect, so keeping them would leave one
    // session's ghosts hanging over the next one's fleet. The toasts go with
    // them for the same reason: last session's refusals over this session's
    // fleet is the same mistake.
    m_ghosts.Clear();
    m_toasts.Clear();
  }

  if (!m_input.windowFocused)
  {
    m_puck.Cancel(); // No release is coming for a drag that alt-tabbed away.
    return;
  }

  if (m_input.Pressed(InputAction::CycleParameter) && m_orderOptionCount > 0)
  {
    // Stepped between gestures, not during one: the puck sampled its queue
    // modifier at the press for the same reason, and an order that changed
    // formation halfway through the drag would be an order the footprint had
    // already lied about.
    m_orderOptionIndex = (m_orderOptionIndex + 1) % m_orderOptionCount;
    NEURON_LOG_INFO("formation: %s", m_orderOptions[m_orderOptionIndex].name);
  }

  const auto cursorX = static_cast<float>(m_input.cursorX);
  const auto cursorY = static_cast<float>(m_input.cursorY);

  if (m_input.Pressed(InputButton::Right))
  {
    m_puck.Begin(cursorX, cursorY, m_input.Down(InputAction::QueueOrder));
  }
  else if (m_puck.Active())
  {
    m_puck.Update(cursorX, cursorY);
  }

  if (!m_puck.Active() || !m_input.Released(InputButton::Right))
  {
    return;
  }

  // Resolved against the camera as it is now, not as it was at the press: the
  // player may have panned mid-drag, and the destination is where they are
  // looking when they let go.
  PuckSample sample = m_puck.Resolve(m_camera.PlaneMappingForNdc(), m_input.viewportWidth, m_input.viewportHeight);
  if (!sample.facingFromDrag)
  {
    // A click rather than an arc. The fleet arrives facing the way it went,
    // which is the answer a player who did not think about facing expects.
    sample.facingRadians = TravelFacingRadians(m_scene.entities, m_selection.Ids(), sample.targetMetres, 0.0f);
  }
  m_puck.Cancel();

  CommitOrder(sample, nowSeconds);
}

void ClientApp::CommitOrder(const PuckSample& _sample, double _nowSeconds)
{
  const std::uint32_t orderSeq = m_nextOrderSeq++;

  // The game's kind, and whichever of the game's parameters is selected. Both
  // are numbers this client copies and never reads.
  OrderDefaults chosen = m_orderDefaults;
  if (m_orderOptionIndex < m_orderOptionCount)
  {
    chosen.parameter = m_orderOptions[m_orderOptionIndex].parameter;
  }
  const OrderIntent intent = MakeOrderIntent(_sample, chosen, m_selection.Ids(), orderSeq);

  // The footprint, from the game's own formation solve -- the real one, one
  // station per ship. Solved before the pre-check because a refused order still
  // needs a ghost to bounce, and a ghost with no footprint is a bounce of
  // nothing (`puck-and-wheel.png` §4).
  m_worldView->SolvePreview(intent, m_orderPreview);

  // Where the ghost bounces back to. The target itself when nothing selected is
  // present, which makes a refused order with no fleet a fade rather than a
  // flight to the origin.
  DirectX::XMFLOAT2 origin = _sample.targetMetres;
  (void)SelectionCentre(m_scene.entities, m_selection.Ids(), origin);

  if (!m_ghosts.Add(intent, m_orderPreview, origin, _nowSeconds))
  {
    // More orders in flight than the snapshot can report on. Sending anyway
    // would put an order on the wire whose refusal has nowhere to land.
    NEURON_LOG_WARNING("too many orders in flight; order %u was not sent", orderSeq);
    return;
  }

  const OrderVerdict local = m_worldView->PreCheck(intent);
  if (!local.accepted)
  {
    // Refused here, and it must look exactly like being refused there -- same
    // ghost, same bounce, same reason string, one round trip sooner. That is
    // the whole reason the pre-check exists (ADR-014 §3).
    m_ghosts.Refuse(orderSeq, local.reasonCode, true, _nowSeconds);

    // The same toast the authority's refusal raises, from the same function.
    // BounceParity is about the player being unable to tell which side said no,
    // and two surfaces reading identically is most of what that means.
    const char* reason = m_worldView->ReasonText(local.reasonCode);
    (void)m_toasts.Raise(ToastPriority::Urgent, local.reasonCode, "ORDER REJECTED", reason, _nowSeconds);
    NEURON_LOG_INFO("order %u refused locally: %s", orderSeq, reason);
    return;
  }

  // The game writes its own bytes and the connection frames them; neither looks
  // at the other's half (ADR-004 ruling 4).
  std::array<std::uint8_t, MAX_DATAGRAM_BYTES> payload{};
  ByteWriter writer{payload};
  if (!m_worldView->EncodeOrder(intent, writer) || !writer.Ok() || !m_connection.SendOrder(writer.Written()))
  {
    // It never left, so there was never a promise. Forgotten rather than
    // bounced: a bounce says the game refused it, and nothing did.
    m_ghosts.Forget(orderSeq);
    NEURON_LOG_WARNING("order %u could not be sent", orderSeq);
    return;
  }
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

  // A ship that died while selected leaves the selection here, with the fresh
  // list. Holding a dead id would draw a ring around empty space and, from S9,
  // send an order for something that no longer exists.
  m_selection.Retain(m_scene.entities);

  // The overlay is built from the same entities the pick ran over, so a ring is
  // drawn exactly where a click would have landed (ADR-006 §8, §11).
  BuildOverlayMarks(m_scene.entities, m_selection.Ids(), m_overlayTuning, m_camera.MetresPerPixel(), m_overlayMarks);

  // Then the ghosts, which are plane-lying and go in beside the rings. After
  // the selection because `BuildOverlayMarks` clears the list, and in this
  // stage rather than in `UpdateOrders` so a ghost added this frame is drawn
  // this frame -- a promise that appeared one frame late would be a promise
  // made after the player had already looked.
  BuildGhostMarks(m_ghosts.Ghosts(), m_overlayTuning, m_camera.MetresPerPixel(), Clock::SecondsSinceStart(), m_overlayMarks);
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

void ClientApp::BuildHud()
{
  NEURON_SPAN("Ui");

  const double nowSeconds = Clock::SecondsSinceStart();
  m_toasts.Advance(nowSeconds);

  m_ui.Clear();

  const UiLayout layout = ResolveUiLayout(m_input.viewportWidth, m_input.viewportHeight, m_config.uiScale, m_uiTuning);
  if (layout.viewport.width <= 0.0f || layout.viewport.height <= 0.0f)
  {
    return; // A minimised window. Nothing to lay out and nothing to draw.
  }

  const float pad = m_uiTuning.padding * layout.scale;
  const float cell = 8.0f * layout.scale; // The monospace grid, near enough for
                                          // placement; the pass measures glyphs.

  // --- the top status row -------------------------------------------------
  //
  // Everything on it is a replicated field or a link statistic, which is the
  // whole acceptance criterion: kill the feed and these go stale rather than
  // holding their last value.
  m_ui.AddQuad(layout.topBar, PANEL_COLOUR);
  m_ui.AddQuad(UiRect{0.0f, layout.topBar.Bottom() - 1.0f, layout.topBar.width, 1.0f}, PANEL_EDGE_COLOUR);

  const float textY = layout.topBar.y + (layout.topBar.height - 13.0f * layout.scale) * 0.5f;
  m_ui.AddText(pad, textY, m_uiTuning.bodySizeIndex, TEXT_DIM_COLOUR, "OUTPOST FRONTIER");

  char buffer[96] = {};

  // Ships, from the newest snapshot rather than from anything the client keeps.
  const std::uint32_t shipCount = static_cast<std::uint32_t>(m_scene.entities.size());
  std::snprintf(buffer, sizeof(buffer), "%u SHIPS", shipCount);
  m_ui.AddText(layout.topBar.width * 0.5f, textY, m_uiTuning.bodySizeIndex, TEXT_COLOUR, buffer);

  // The link. `--` rather than a stale number when there is no session, because
  // a round-trip figure from a connection that has gone is a lie with a unit.
  if (m_connection.State() == ClientLinkState::Joined)
  {
    std::snprintf(buffer, sizeof(buffer), "NET %.0f ms   TICK %u", m_connection.RoundTripMs(), m_connection.ServerTick());
  }
  else
  {
    std::snprintf(buffer, sizeof(buffer), "NET --   NO SESSION");
  }
  const auto netWidth = static_cast<float>(std::strlen(buffer)) * cell;
  m_ui.AddText(layout.topBar.Right() - pad - netWidth, textY, m_uiTuning.bodySizeIndex, TEXT_DIM_COLOUR, buffer);

  // --- the fleet roster ---------------------------------------------------
  //
  // The rows are the game's answer, not a grouping this file performs: it has
  // `EntityRecord::groupId` and could aggregate in four lines, and doing so
  // would be deciding that groups are named and how their health combines
  // (ADR-014 §2c).
  m_rosterRowCount = m_worldView->BuildRoster(m_selection.Ids(), m_rosterRows);

  m_ui.AddQuad(layout.roster, PANEL_COLOUR);
  m_ui.AddQuad(UiRect{layout.roster.Right() - 1.0f, layout.roster.y, 1.0f, layout.roster.height}, PANEL_EDGE_COLOUR);
  m_ui.AddText(layout.roster.x + pad, layout.roster.y + pad, m_uiTuning.smallSizeIndex, TEXT_DIM_COLOUR, "FLEET ROSTER");

  const float rowHeight = 38.0f * layout.scale;
  const float barHeight = 3.0f * layout.scale;
  float rowY = layout.roster.y + pad + 18.0f * layout.scale;

  for (std::uint32_t index = 0; index < m_rosterRowCount; ++index)
  {
    const RosterRow& row = m_rosterRows[index];
    const UiRect rowRect{layout.roster.x + pad * 0.5f, rowY, layout.roster.width - pad, rowHeight - 4.0f * layout.scale};
    if (rowRect.Bottom() > layout.roster.Bottom())
    {
      break; // The panel is full. The print's "8/8" footer is where scrolling
             // would go, and scrolling is a surface rather than a clamp.
    }

    // A wing the player has any of is lit; the print highlights the row rather
    // than the name, so the whole row reads as the unit of selection.
    const bool selected = row.selectedCount > 0;
    const bool empty = row.shipCount == 0;
    const std::uint32_t nameColour = empty ? TEXT_DISABLED_COLOUR : (selected ? TEXT_COLOUR : TEXT_DIM_COLOUR);
    if (selected)
    {
      m_ui.AddQuad(rowRect, ROW_SELECTED_COLOUR);
      m_ui.AddBorder(rowRect, 1.0f * layout.scale, TEXT_COLOUR);
    }

    m_ui.AddText(rowRect.x + pad * 0.5f, rowRect.y + pad * 0.4f, m_uiTuning.bodySizeIndex, nameColour,
                 row.name != nullptr ? row.name : "?");

    // A dash rather than a zero for a wing with nothing left. The print draws
    // one, and it is the honest glyph: zero reads as a count and this is the
    // absence of one.
    if (empty)
    {
      std::snprintf(buffer, sizeof(buffer), "-");
    }
    else if (selected)
    {
      std::snprintf(buffer, sizeof(buffer), "%u/%u", row.selectedCount, row.shipCount);
    }
    else
    {
      std::snprintf(buffer, sizeof(buffer), "%u", row.shipCount);
    }
    const auto countWidth = static_cast<float>(std::strlen(buffer)) * cell;
    m_ui.AddText(rowRect.Right() - pad * 0.5f - countWidth, rowRect.y + pad * 0.4f, m_uiTuning.bodySizeIndex, nameColour,
                 buffer);

    // Two strips: hull over shield, the same order and the same gauges the
    // world-space bars use (ADR-006 §8), so a wing's row and its ships' bars
    // cannot disagree about what full means.
    const float barWidth = rowRect.width - pad;
    const float barX = rowRect.x + pad * 0.5f;
    const float barY = rowRect.Bottom() - pad * 0.5f - barHeight * 2.0f - 2.0f * layout.scale;

    m_ui.AddQuad(UiRect{barX, barY, barWidth, barHeight}, BAR_TRACK_COLOUR);
    m_ui.AddQuad(UiRect{barX, barY, barWidth * (static_cast<float>(row.hullGauge) / 255.0f), barHeight}, HULL_COLOUR);

    const float shieldY = barY + barHeight + 2.0f * layout.scale;
    m_ui.AddQuad(UiRect{barX, shieldY, barWidth, barHeight}, BAR_TRACK_COLOUR);
    m_ui.AddQuad(UiRect{barX, shieldY, barWidth * (static_cast<float>(row.shieldGauge) / 255.0f), barHeight}, SHIELD_COLOUR);

    rowY += rowHeight;
  }

  // --- the context bar ----------------------------------------------------
  //
  // `N SHIPS : WING -> FORMATION`, which is the print's own line. It reads off
  // the selection and the roster the game just built, so it cannot claim a wing
  // the roster does not list.
  m_ui.AddQuad(layout.contextBar, PANEL_COLOUR);
  m_ui.AddQuad(UiRect{0.0f, layout.contextBar.y, layout.contextBar.width, 1.0f}, PANEL_EDGE_COLOUR);

  const float contextY = layout.contextBar.y + (layout.contextBar.height - 13.0f * layout.scale) * 0.5f;
  const std::size_t selectedCount = m_selection.Ids().size();
  if (selectedCount == 0)
  {
    m_ui.AddText(pad, contextY, m_uiTuning.bodySizeIndex, TEXT_DISABLED_COLOUR, "NO SELECTION");
  }
  else
  {
    // The wing named is the one the selection is mostly in. A selection
    // spanning two wings is a real thing a box-drag produces, and naming the
    // largest share beats naming the first or claiming both.
    const char* wingName = "MIXED";
    std::uint16_t best = 0;
    for (std::uint32_t index = 0; index < m_rosterRowCount; ++index)
    {
      if (m_rosterRows[index].selectedCount > best)
      {
        best = m_rosterRows[index].selectedCount;
        wingName = m_rosterRows[index].name != nullptr ? m_rosterRows[index].name : "?";
      }
    }
    // Cast rather than compare across the promotion: `best` is a `uint16_t`
    // that promotes to `int`, and `int < size_t` is C4018 at /W3.
    if (static_cast<std::size_t>(best) < selectedCount)
    {
      wingName = "MIXED";
    }

    const char* formation = "-";
    if (m_orderOptionIndex < m_orderOptionCount && m_orderOptions[m_orderOptionIndex].name != nullptr)
    {
      formation = m_orderOptions[m_orderOptionIndex].name;
    }
    std::snprintf(buffer, sizeof(buffer), "%zu SHIPS : %s  >  FORMATION %s", selectedCount, wingName, formation);
    m_ui.AddText(pad, contextY, m_uiTuning.bodySizeIndex, TEXT_COLOUR, buffer);
  }

  // What the fleet is waiting on. The print puts it at the right of this bar,
  // and it reads off the ghost list rather than off anything the client wishes
  // were true.
  if (!m_ghosts.Empty())
  {
    std::snprintf(buffer, sizeof(buffer), "%zu ORDER PENDING", m_ghosts.Count());
    const auto pendingWidth = static_cast<float>(std::strlen(buffer)) * cell;
    m_ui.AddText(layout.contextBar.Right() - pad - pendingWidth, contextY, m_uiTuning.bodySizeIndex, TOAST_URGENT_COLOUR,
                 buffer);
  }

  // --- the toast stack ----------------------------------------------------
  //
  // Criticals lead the visible list and go centre-top on their own surface;
  // the rest stack bottom-right, clear of the context bar (§2).
  const std::span<const Toast> toasts = m_toasts.Visible();
  const std::size_t criticals = m_toasts.CriticalCount();

  for (std::size_t index = 0; index < toasts.size(); ++index)
  {
    const Toast& toast = toasts[index];
    const bool isCritical = index < criticals;
    const UiRect rect = isCritical ? layout.criticalToast : layout.ToastSlot(index - criticals, m_uiTuning);
    if (!isCritical && index - criticals >= ToastStack::MAX_VISIBLE)
    {
      break; // The stack showed what it can; the rest are already dropped.
    }

    const std::uint32_t accent = isCritical ? TOAST_CRITICAL_COLOUR : TOAST_URGENT_COLOUR;
    m_ui.AddQuad(rect, PANEL_COLOUR);
    m_ui.AddBorder(rect, 1.0f * layout.scale, accent);

    // A count only when there is one to report -- the sheet draws the coalesced
    // form as a suffix, and "x1" on every row would be noise.
    if (toast.count > 1)
    {
      std::snprintf(buffer, sizeof(buffer), "%s x%u", toast.head.c_str(), toast.count);
    }
    else
    {
      std::snprintf(buffer, sizeof(buffer), "%s", toast.head.c_str());
    }
    m_ui.AddText(rect.x + pad, rect.y + pad * 0.5f, m_uiTuning.bodySizeIndex, accent, buffer);
    m_ui.AddText(rect.x + pad, rect.y + pad * 0.5f + 15.0f * layout.scale, m_uiTuning.smallSizeIndex, TEXT_DIM_COLOUR,
                 toast.detail);
  }
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
  context.overlayMarks = &m_overlayMarks;
  context.ui = &m_ui;
  context.glyphAtlas = &m_glyphAtlas;
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
