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
 * The HUD's colours live in `HudPalette`, resolved once at boot from
 * `client.ui.palette` and read through `m_palette` everywhere -- there are no
 * colour constants in this file any more, and a packed literal in `BuildHud`
 * is a defect (the accept criterion is a grep).
 */

/// One descriptor for the glyph atlas, and room for the per-frame tables the
/// overlay and UI passes will want. Small and fixed: a heap that grows is a
/// heap that has to be rebound mid-frame.
constexpr std::uint32_t SHADER_VISIBLE_DESCRIPTORS = 16;

/// 256 KiB per frame in flight. The parked fleet uses under a kilobyte of it;
/// the number is sized for the corpus's 1,024 instances plus the overlay and
/// text streams that join them, so the first busy frame does not discover a cap.
constexpr std::uint32_t UPLOAD_BYTES_PER_FRAME = 256 * 1024;

/// The HUD's three sizes before the UI scale multiplier (ADR-006 §9). The
/// middle one is the chrome band the prints are set in -- 15, not 16.
constexpr float BASE_FONT_SIZES_PIXELS[] = {13.0f, 15.0f, 22.0f};

/*
 * Emissive strength per canonical material (ADR-006 §6).
 *
 * Not content: the .mtl files carry albedo, and which of the five materials
 * *glows* is a renderer decision the exporter has no way to express. Accent and
 * thruster carry the emissive channel; glass is simply dark, which is what
 * makes a cockpit read as glass against a lit hull rather than as a hole.
 */
constexpr float MATERIAL_EMISSIVE[MESH_MATERIAL_COUNT] = {0.0f, 0.0f, 0.0f, 1.6f, 2.4f};

/// UPPERCASE into a fixed buffer, for the command verbs and the formation
/// value: the game supplies mixed-case names and the print sets the chrome in
/// capitals. ASCII only -- a byte outside a-z passes through untouched, so a
/// marker glyph in a name would survive rather than be mangled.
void UpperCaseInto(const char* _text, char (&_out)[32])
{
  std::size_t i = 0;
  for (; _text[i] != '\0' && i + 1 < sizeof(_out); ++i)
  {
    const char c = _text[i];
    _out[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
  }
  _out[i] = '\0';
}

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

  // The colour table, once. Everything `BuildHud` draws resolves through it,
  // which is what makes the settings sheet's colour-vision palettes a config
  // string rather than a migration.
  m_palette = ResolveHudPalette(_config.uiPalette);

  // What the puck's orders are, from the side that knows. Once, at boot: there
  // is one command until the wheel exists, and asking every frame would be
  // asking a question whose answer is compiled in.
  m_orderDefaults = _worldView.DefaultOrder();
  m_selectedKind = m_orderDefaults.kind;
  m_orderOptionCount = _worldView.OrderOptions(m_orderDefaults.kind, m_orderOptions);

  // The command row's buttons. Asked once: a game's command list does not
  // change while a session runs, and asking every frame would imply it could.
  m_orderKindCount = _worldView.OrderKinds(m_orderKinds);

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

  if (!m_swapChain.Create(m_device, m_window.Handle(), m_window.Width(), m_window.Height(), _config.msaaSamples))
  {
    return false;
  }

  // The strip's starting visibility is the setting; F1 is a shortcut to the
  // same bit, not a second source of truth (debug-hud.png §6).
  m_diagnosticsVisible = _config.diagnosticsStrip;

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
  // Zero asks for hardware_concurrency - 1, capped at the telemetry lane
  // budget. A single-core machine gets no workers at all, which is supported:
  // TaskPool::Wait runs the work inline.
  if (!m_taskPool.Start(0))
  {
    NEURON_LOG_ERROR("the boot task pool would not start");
    return false;
  }

  // The world pipelines take the sample count the swapchain actually built --
  // which may be 1 if the requested MSAA was unsupported and it fell back.
  bool ok = m_pipelines.Create(m_device.Device(), m_shaders, m_swapChain.SampleCount());
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

  // Not part of `ok` either, and for a stronger reason than the nebula's: a
  // machine with no audio device must still run the game (ADR-011,
  // Consequences). Every call on a device that did not open is a no-op, so
  // nothing downstream asks whether there are speakers.
  (void)m_audio.Create(m_config.audio, m_config.audioDirectory, m_config.soundBankFile);

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
      UpdateHud();
      UpdateSelection();
      UpdateOrders();
    }
    {
      NEURON_SPAN("Extract");
      ExtractScene();
    }
    {
      // The fifth budget row (ADR-011 §9), immediately after Extract because
      // it reads the scene Extract just built.
      NEURON_SPAN("Audio");
      AudioUpdate();
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
  //
  // Unless F10 has cut the feed, in which case they are dropped where they
  // stand -- dropped rather than held, because a sender that stopped is what
  // this reproduces, and a queue released on the way out would replay a burst
  // of history the world never lived through. Everything else stays up: the
  // clock is not touched either, so the age the SNAP row reports is the real
  // time since the last snapshot the world actually saw.
  const double nowSeconds = Clock::SecondsSinceStart();
  if (!m_feedFrozen)
  {
    for (const std::vector<std::uint8_t>& payload : m_connection.PendingSnapshots())
    {
      // The tick comes back from the game, because the game is the only side
      // that can read it. Zero means the payload was rejected, and a rejected
      // snapshot must not move the clock.
      const std::uint32_t tick = m_worldView->ApplySnapshot(payload);
      if (tick != 0)
      {
        m_snapshots.OnSnapshot(tick, nowSeconds);
      }
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
void ClientApp::UpdateHud()
{
  m_uiLayout = ResolveUiLayout(m_input.viewportWidth, m_input.viewportHeight, m_config.uiScale, m_uiTuning);

  // The diagnostics toggle, before anything can consume the frame's edges. A
  // level edge rather than a chord: the strip is a setting with a shortcut,
  // not a gesture (debug-hud.png §6).
  if (m_input.Pressed(InputAction::ToggleDiagnostics))
  {
    m_diagnosticsVisible = !m_diagnosticsVisible;
  }

  // The induced stall, beside the strip's toggle because it is the instrument
  // the strip is read with. Logged rather than drawn: the screen is what is
  // being judged while this is on, so the record of it belongs in the file.
  if (m_input.Pressed(InputAction::ToggleFeedFreeze))
  {
    m_feedFrozen = !m_feedFrozen;
    NEURON_LOG_INFO("feed %s (F10)", m_feedFrozen ? "cut -- the world will go stale" : "restored");
  }

  m_commandButtonCount =
      BuildCommandRow(std::span<const OrderKindOption>{m_orderKinds, m_orderKindCount}, m_selectedKind,
                      std::span<const OrderOption>{m_orderOptions, m_orderOptionCount}, m_orderOptionIndex,
                      m_uiLayout.commandRow, m_uiLayout.scale, m_commandTuning, m_commandButtons);

  if (!m_input.windowFocused || !m_input.Pressed(InputButton::Left))
  {
    return;
  }

  const CommandButton* pressed =
      HitCommandRow(std::span<const CommandButton>{m_commandButtons, m_commandButtonCount},
                    static_cast<float>(m_input.cursorX), static_cast<float>(m_input.cursorY));
  if (pressed == nullptr)
  {
    return;
  }

  switch (pressed->action)
  {
  case CommandAction::SelectKind:
    // Only the kind changes. The options and the chosen index belong to it, so
    // both are re-asked -- a formation index left over from another command
    // would index a list that is not the one it came from.
    if (pressed->payload != m_selectedKind)
    {
      m_selectedKind = pressed->payload;
      m_orderOptionCount = m_worldView->OrderOptions(m_selectedKind, m_orderOptions);
      m_orderOptionIndex = 0;
    }
    break;

  case CommandAction::CycleParameter:
    // The same step the `F` binding makes, through the same index, because a
    // button and a key that did the same thing by two routes would drift.
    if (m_orderOptionCount > 0)
    {
      m_orderOptionIndex = (m_orderOptionIndex + 1) % m_orderOptionCount;
      NEURON_LOG_INFO("%s: %s", pressed->label, m_orderOptions[m_orderOptionIndex].name);
    }
    break;
  }
}

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

  /*
   * A drag may only *begin* in the world zone.
   *
   * The HUD is a border rather than an overlay (`UiLayout`), so everything
   * outside `world` has a panel on it -- and until now a press on the roster or
   * the command row also started a box selection across the fleet underneath.
   * Once begun a drag may leave the zone freely: the box is meant to extend to
   * wherever the cursor goes, and a selection that cancelled when it touched
   * the ability rack would be worse than the bug.
   */
  if (m_input.Pressed(InputButton::Left))
  {
    if (m_uiLayout.world.Contains(cursorX, cursorY))
    {
      m_selection.BeginDrag(cursorX, cursorY, m_input.Down(InputAction::SelectAdd));
    }
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
      // The third surface one refusal owes, beside the bouncing ghost and the
      // toast (ADR-011 §12a). Raised on both bounce paths for the same reason
      // the other two are: a local refusal and a remote one must be
      // indistinguishable to the player (ADR-005 §4).
      m_audio.PlayCue(ORDER_REJECTED_SOUND_ID);
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
  // are numbers this client copies and never reads. The kind is the command row's
  // now rather than the default's -- the default is only where it started.
  OrderDefaults chosen = m_orderDefaults;
  chosen.kind = m_selectedKind;
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
    m_audio.PlayCue(ORDER_REJECTED_SOUND_ID);
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

void ClientApp::AudioUpdate()
{
  if (!m_audio.Ready())
  {
    return;
  }

  // The listener is the camera's focus, raised by the zoom (ADR-011 §4). This
  // is the whole spatialisation model, and it is three numbers from the camera.
  m_audio.SetListener(MakeAudioListener(m_camera.Focus(), m_camera.YawRadians(), m_camera.ZoomMetres()));

  /*
   * An engine loop declared for every ship on screen.
   *
   * Declared rather than started: the pool decides how many of these actually
   * sound, so a fleet of forty asks forty times and the cap answers. The id is
   * the scene entity's own, which is what lets a loop be recognised next frame
   * instead of retriggered -- and what retires it the frame its ship dies.
   *
   * The station is a `Structure` and hums like everything else here. That is
   * the thin slice being thin: one 3D cue, applied uniformly, is what proves
   * the architecture. Which hull makes which noise is the sound designer's
   * question and it is answered in the bank, not here.
   */
  for (const SceneEntity& entity : m_scene.entities)
  {
    // The plane point, without the cosmetic hover the instance carries. ADR-011
    // §5 says emitters are render positions, and this is metres below one: the
    // entity list is the only side with ids to key a loop on, and a hover of a
    // few metres against a falloff measured in kilometres is inaudible.
    m_audio.UpdateLoop(ENGINE_LOOP_SOUND_ID, entity.id, MakeAudioEmitter(entity.planeMetres, 0.0f));
  }

  m_audio.Update(Clock::SecondsSinceStart());
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
  m_uiWorld.Clear();

  // Resolved in `UpdateHud`, so the button a click lands on and the button that
  // is drawn are laid out from one answer rather than two.
  const UiLayout& layout = m_uiLayout;
  if (layout.viewport.width <= 0.0f || layout.viewport.height <= 0.0f)
  {
    return; // A minimised window. Nothing to lay out and nothing to draw.
  }

  const float pad = m_uiTuning.padding * layout.scale;
  const float cell = 8.0f * layout.scale; // The monospace grid, near enough for
                                          // placement; the pass measures glyphs.

  // The chrome band's height, for centring a line inside its zone. From the
  // size table rather than a repeated 13.0f, so retuning the band retunes the
  // centring with it.
  const float bodyPx = BASE_FONT_SIZES_PIXELS[m_uiTuning.bodySizeIndex] * layout.scale;
  const float smallPx = BASE_FONT_SIZES_PIXELS[m_uiTuning.smallSizeIndex] * layout.scale;

  /*
   * --- world-space marks, first so the panels cover them -------------------
   *
   * `overlay-pass.png` §1: panels and toasts always composite over world-space
   * marks, and the context bar is never occluded by one. The Ui pass has one
   * pipeline and no sort, so build order *is* draw order and putting these
   * first is the whole of that rule's implementation.
   */
  GhostLaneView laneView;
  laneView.mapping = m_camera.PlaneMappingForNdc();
  laneView.viewportWidth = m_input.viewportWidth;
  laneView.viewportHeight = m_input.viewportHeight;
  laneView.worldRect = layout.world;
  laneView.cellPixels = cell;
  laneView.scale = layout.scale;
  // The dashes go into the world layer, drawn before the hulls so the ships
  // cover them; the labels stay here, on top of everything, because a readout
  // behind a ship is a readout nobody can read.
  BuildGhostLanes(m_ghosts.Ghosts(), m_scene.entities, laneView, m_overlayTuning, m_laneTuning, nowSeconds, m_uiWorld,
                  m_ui);

  /*
   * The drag rectangle S8 deferred here.
   *
   * A screen-space quad and never a world mark: the box is axis-aligned in
   * *pixels* and an arbitrary parallelogram on the plane, which is the same
   * reason `PickBox` tests ships in screen space rather than mapping four
   * corners onto the plane (ADR-006 §11). Drawn only once the gesture has left
   * the click slop, so a click never flashes a box.
   */
  if (m_selection.DragIsBox())
  {
    const UiRect box = UiRect::FromCorners(m_selection.DragStartX(), m_selection.DragStartY(), m_selection.DragCurrentX(),
                                           m_selection.DragCurrentY());
    // The wash is the ring colour at low alpha rather than a colour of its
    // own: a box and the rings it is about to produce must obviously be the
    // same gesture. The ring colour itself is `OverlayTuning`'s -- a
    // world-space colour, outside the HUD palette's remit.
    m_ui.AddQuad(box, WithAlpha(m_overlayTuning.ringColourRgba, 0x28));
    m_ui.AddBorder(box, 1.0f, m_overlayTuning.ringColourRgba);
  }

  /*
   * --- the top status row -------------------------------------------------
   *
   * Everything on it is a replicated field, a link statistic or local UI
   * state, which is the whole acceptance criterion: kill the feed and these
   * go stale -- drawn in `neutral` at reduced alpha -- rather than holding
   * their last value or inventing one. The print's location and security
   * readouts are not here because nothing replicates them yet; a slot with no
   * source stays empty rather than lying.
   */
  m_ui.AddQuad(layout.topBar, m_palette.panel);
  m_ui.AddQuad(UiRect{0.0f, layout.topBar.Bottom() - 1.0f, layout.topBar.width, 1.0f}, m_palette.borderStrong);

  const bool joined = m_connection.State() == ClientLinkState::Joined;
  const float textY = layout.topBar.y + (layout.topBar.height - bodyPx) * 0.5f;
  const std::uint32_t staleColour = AtHalfAlpha(m_palette.neutral);

  char buffer[96] = {};

  // Left to right: session name, then the shared clock, "|"-separated. The
  // tick is the only clock both sides agree on (ADR-002 §1), which is what
  // makes it a readout rather than a decoration.
  float pen = pad;
  const char* sessionName = m_config.playerName.empty() ? "OUTPOST FRONTIER" : m_config.playerName.c_str();
  m_ui.AddText(pen, textY, m_uiTuning.bodySizeIndex, m_palette.phosphorHot, sessionName);
  pen += static_cast<float>(TextCellCount(sessionName)) * cell + cell;

  m_ui.AddText(pen, textY, m_uiTuning.bodySizeIndex, m_palette.phosphorGhost, "|");
  pen += 2.0f * cell;

  if (joined)
  {
    std::snprintf(buffer, sizeof(buffer), "TICK %u", m_connection.ServerTick());
    m_ui.AddText(pen, textY, m_uiTuning.bodySizeIndex, m_palette.neutral, buffer);
  }
  else
  {
    m_ui.AddText(pen, textY, m_uiTuning.bodySizeIndex, staleColour, "NO SESSION");
  }

  // Right to left: the link, then the alert and pending chips. Each chip is a
  // glyph and a count -- the glyph carries the meaning and the colour repeats
  // it, because colour is never the only signal (icon sheet §3).
  if (joined)
  {
    std::snprintf(buffer, sizeof(buffer), "NET %.0f ms", m_connection.RoundTripMs());
  }
  else
  {
    std::snprintf(buffer, sizeof(buffer), "NET --");
  }
  float right = layout.topBar.Right() - pad - static_cast<float>(TextCellCount(buffer)) * cell;
  m_ui.AddText(right, textY, m_uiTuning.bodySizeIndex, joined ? m_palette.phosphorDim : staleColour, buffer);

  std::size_t alertCount = 0;
  for (const Toast& toast : m_toasts.Visible())
  {
    alertCount += toast.priority <= ToastPriority::Urgent ? 1 : 0;
  }
  const float chipY = layout.topBar.y + (layout.topBar.height - smallPx) * 0.5f;
  // The glyphs are spelled as UTF-8 bytes for the same reason the atlas's
  // bake table is spelled as codepoints: these files carry no byte-order
  // mark, and a non-ASCII literal would be read in the compiler's current
  // code page. U+25B2 is the alert triangle, U+23F3 the pending hourglass.
  if (alertCount > 0)
  {
    std::snprintf(buffer, sizeof(buffer), "\xE2\x96\xB2 %zu", alertCount);
    right -= (static_cast<float>(TextCellCount(buffer)) + 2.0f) * cell;
    m_ui.AddText(right, chipY, m_uiTuning.smallSizeIndex, m_palette.hostile, buffer);
  }
  if (!m_ghosts.Empty())
  {
    std::snprintf(buffer, sizeof(buffer), "\xE2\x8F\xB3 %zu", m_ghosts.Count());
    right -= (static_cast<float>(TextCellCount(buffer)) + 2.0f) * cell;
    m_ui.AddText(right, chipY, m_uiTuning.smallSizeIndex, m_palette.caution, buffer);
  }

  // The feed-level STALE readout (S14): the whole world on screen is frozen
  // past the extrapolation cap (ADR-002 §4). The per-ship markers say which
  // ships; this says it is the *feed*, in the place the eye checks the link.
  if (joined && m_snapshots.Stale(nowSeconds))
  {
    const char* staleText = "STALE";
    right -= (static_cast<float>(TextCellCount(staleText)) + 2.0f) * cell;
    m_ui.AddText(right, chipY, m_uiTuning.smallSizeIndex, m_palette.caution, staleText);
  }

  // --- the fleet roster ---------------------------------------------------
  //
  // The rows are the game's answer, not a grouping this file performs: it has
  // `EntityRecord::groupId` and could aggregate in four lines, and doing so
  // would be deciding that groups are named and how their health combines
  // (ADR-014 §2c).
  m_rosterRowCount = m_worldView->BuildRoster(m_selection.Ids(), m_rosterRows);

  m_ui.AddQuad(layout.roster, m_palette.panel);
  m_ui.AddQuad(UiRect{layout.roster.Right() - 1.0f, layout.roster.y, 1.0f, layout.roster.height},
               m_palette.borderStrong);
  m_ui.AddText(layout.roster.x + pad, layout.roster.y + pad, m_uiTuning.smallSizeIndex, m_palette.phosphorLabel,
               "FLEET ROSTER");

  /*
   * One chip per row: name left, count right, a border and a ground. The
   * selected chip is the lit one -- `phosphor` frame on a `rule` fill, hot
   * text -- and an empty wing keeps its frame at half strength with no ground,
   * so the roster's shape survives the fleet it describes.
   *
   * The gauge strips draw only when a gauge says something: a full pair on
   * every chip was ink repeating "nothing is wrong" eight times.
   */
  const float chipHeight = 34.0f * layout.scale;
  const float chipGap = 6.0f * layout.scale;
  const float barHeight = 3.0f * layout.scale;
  float rowY = layout.roster.y + pad + 18.0f * layout.scale;

  for (std::uint32_t index = 0; index < m_rosterRowCount; ++index)
  {
    const RosterRow& row = m_rosterRows[index];
    const UiRect chip{layout.roster.x + pad * 0.5f, rowY, layout.roster.width - pad, chipHeight};
    if (chip.Bottom() > layout.roster.Bottom())
    {
      break; // The panel is full. The print's "8/8" footer is where scrolling
             // would go, and scrolling is a surface rather than a clamp.
    }

    const bool selected = row.selectedCount > 0;
    const bool empty = row.shipCount == 0;

    const std::uint32_t textColour =
        empty ? m_palette.phosphorGhost : (selected ? m_palette.phosphorHot : m_palette.phosphor);
    if (selected)
    {
      m_ui.AddQuad(chip, m_palette.rule);
    }
    else if (!empty)
    {
      m_ui.AddQuad(chip, m_palette.chipBg);
    }
    const std::uint32_t frameColour =
        selected ? m_palette.phosphor : (empty ? AtHalfAlpha(m_palette.border) : m_palette.border);
    m_ui.AddBorder(chip, 1.0f * layout.scale, frameColour);

    m_ui.AddText(chip.x + pad * 0.5f, chip.y + pad * 0.5f, m_uiTuning.smallSizeIndex, textColour,
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
    const auto countWidth = static_cast<float>(TextCellCount(buffer)) * cell;
    m_ui.AddText(chip.Right() - pad * 0.5f - countWidth, chip.y + pad * 0.5f, m_uiTuning.smallSizeIndex, textColour,
                 buffer);

    /*
     * Two strips, hull over shield, from `RosterRow`'s own 0-255 gauges --
     * and only when either is below full. The hull's fill moves through the
     * palette's three bands as it falls; the shield is always the allied
     * cyan. Same order and same gauges as the world-space bars (ADR-006 §8),
     * so a wing's row and its ships' bars cannot disagree about what full
     * means.
     */
    if (row.hullGauge < 255 || row.shieldGauge < 255)
    {
      const float barWidth = chip.width - pad;
      const float barX = chip.x + pad * 0.5f;
      const float hullY = chip.Bottom() - pad * 0.5f - barHeight * 2.0f - 2.0f * layout.scale;

      m_ui.AddQuad(UiRect{barX, hullY, barWidth, barHeight}, m_palette.trackHull);
      m_ui.AddQuad(UiRect{barX, hullY, barWidth * (static_cast<float>(row.hullGauge) / 255.0f), barHeight},
                   HullGaugeFill(m_palette, row.hullGauge));

      const float shieldY = hullY + barHeight + 2.0f * layout.scale;
      m_ui.AddQuad(UiRect{barX, shieldY, barWidth, barHeight}, m_palette.trackShield);
      m_ui.AddQuad(UiRect{barX, shieldY, barWidth * (static_cast<float>(row.shieldGauge) / 255.0f), barHeight},
                   m_palette.allied);
    }

    rowY += chipHeight + chipGap;
  }

  /*
   * --- the ability rack ---------------------------------------------------
   *
   * The S11 stub: the zone, its frame, and four dead slots. Everything in it
   * is `phosphorDead` because no ability exists game-side yet, and the labels
   * are dashes for the same reason the roster draws one for an empty wing --
   * the engine must not name MWD or REPAIR, because ability names are game
   * vocabulary and this is the engine (ADR-014). The slots exist so the zone
   * reads as reserved rather than broken.
   */
  m_ui.AddQuad(layout.abilityRack, m_palette.panel);
  m_ui.AddQuad(UiRect{layout.abilityRack.x, layout.abilityRack.y, 1.0f, layout.abilityRack.height},
               m_palette.borderStrong);
  m_ui.AddText(layout.abilityRack.x + pad, layout.abilityRack.y + pad, m_uiTuning.smallSizeIndex,
               m_palette.phosphorLabel, "ABILITY");

  const float slotHeight = 58.0f * layout.scale;
  const float slotWidth = layout.abilityRack.width - 2.0f * pad;
  float slotY = layout.abilityRack.y + pad + 18.0f * layout.scale;
  for (int slot = 0; slot < 4 && slotWidth > 0.0f; ++slot)
  {
    const UiRect slotRect{layout.abilityRack.x + pad, slotY, slotWidth, slotHeight};
    if (slotRect.Bottom() > layout.abilityRack.Bottom())
    {
      break;
    }
    m_ui.AddQuad(slotRect, m_palette.chipBg);
    m_ui.AddBorder(slotRect, 1.0f * layout.scale, m_palette.border);

    // U+25A1, the icon sheet's empty-slot square, over a dash of a label.
    // One cell wide at the head size, centred by the same cell arithmetic
    // every centred label on this HUD uses.
    const float headCell = BASE_FONT_SIZES_PIXELS[m_uiTuning.headSizeIndex] * 0.55f * layout.scale;
    m_ui.AddText(slotRect.x + (slotRect.width - headCell) * 0.5f, slotRect.y + 8.0f * layout.scale,
                 m_uiTuning.headSizeIndex, m_palette.phosphorDead, "\xE2\x96\xA1");
    m_ui.AddText(slotRect.x + (slotRect.width - 2.0f * cell) * 0.5f,
                 slotRect.Bottom() - 6.0f * layout.scale - smallPx, m_uiTuning.smallSizeIndex, m_palette.phosphorDead,
                 "--");

    slotY += slotHeight + pad;
  }

  /*
   * --- the context bar ----------------------------------------------------
   *
   * The selection summary: `N SHIPS SELECTED | WING | FORMATION VALUE`, with
   * ghost separators. It reads off the selection and the roster the game just
   * built, so it cannot claim a wing the roster does not list -- and the
   * formation value lives here rather than as a second line on the command
   * row's button, so the verb stays one word and the summary carries the
   * state.
   */
  m_ui.AddQuad(layout.contextBar, m_palette.panel);
  m_ui.AddQuad(UiRect{0.0f, layout.contextBar.y, layout.contextBar.width, 1.0f}, m_palette.borderStrong);

  const float contextY = layout.contextBar.y + (layout.contextBar.height - bodyPx) * 0.5f;
  const std::size_t selectedCount = m_selection.Ids().size();
  if (selectedCount == 0)
  {
    m_ui.AddText(pad, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphorDim, "NO SELECTION");
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

    float contextPen = pad;
    std::snprintf(buffer, sizeof(buffer), "%zu SHIPS SELECTED", selectedCount);
    m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphorHot, buffer);
    contextPen += static_cast<float>(TextCellCount(buffer)) * cell + cell;

    m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphorGhost, "|");
    contextPen += 2.0f * cell;

    char upper[32] = {};
    UpperCaseInto(wingName, upper);
    m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphor, upper);
    contextPen += static_cast<float>(TextCellCount(upper)) * cell + cell;

    if (m_orderOptionIndex < m_orderOptionCount && m_orderOptions[m_orderOptionIndex].name != nullptr)
    {
      m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphorGhost, "|");
      contextPen += 2.0f * cell;

      UpperCaseInto(m_orderOptions[m_orderOptionIndex].name, upper);
      std::snprintf(buffer, sizeof(buffer), "FORMATION %s", upper);
      m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphor, buffer);
    }
  }

  // --- the command row ----------------------------------------------------
  //
  // Laid out in `UpdateHud` and only drawn here. Every word on it came from the
  // game through `OrderKinds`: a row that spelled MOVE and ATTACK in this file
  // would be one game's verbs compiled into a two-game engine (ADR-014 §2b).
  m_ui.AddQuad(layout.commandRow, m_palette.panel);
  m_ui.AddQuad(UiRect{0.0f, layout.commandRow.y, layout.commandRow.width, 1.0f}, m_palette.rule);

  float lastButtonRight = layout.commandRow.x;
  for (std::uint32_t index = 0; index < m_commandButtonCount; ++index)
  {
    const CommandButton& button = m_commandButtons[index];
    lastButtonRight = std::max(lastButtonRight, button.rect.Right());

    /*
     * Three states, three treatments, and outline-plus-no-fill is the default
     * control on this HUD: the active command is the only filled one, an
     * available one is a `border` frame around `phosphor` text, and one with
     * no content behind it keeps its frame at half strength around dead text.
     * The print keeps all three in the row -- greying rather than hiding is
     * what lets the row stay the same shape as content arrives.
     */
    const std::uint32_t edge =
        button.active ? m_palette.phosphor : (button.enabled ? m_palette.border : AtHalfAlpha(m_palette.border));
    const std::uint32_t text =
        button.enabled ? (button.active ? m_palette.phosphorHot : m_palette.phosphor) : m_palette.phosphorDead;

    if (button.active)
    {
      m_ui.AddQuad(button.rect, m_palette.rule);
    }
    m_ui.AddBorder(button.rect, 1.0f * layout.scale, edge);

    // One line per verb, UPPERCASE, centred -- the parameter's current value
    // is the context bar's to say, so the button stays a single word. Measured
    // the same way every other centred label on this HUD is: cells times the
    // cell, because the face is fixed-pitch.
    char upper[32] = {};
    UpperCaseInto(button.label, upper);
    const auto labelWidth = static_cast<float>(TextCellCount(upper)) * cell;
    const float labelY = button.rect.y + (button.rect.height - bodyPx) * 0.5f;
    m_ui.AddText(button.rect.x + (button.rect.width - labelWidth) * 0.5f, labelY, m_commandTuning.labelSizeIndex, text,
                 upper);
  }

  /*
   * The queue chip, right-aligned in the row: `caution` text and frame, no
   * fill. It names the append gesture (`InputAction::QueueOrder` held at the
   * puck's press) rather than being a button of its own, which is why it is
   * not in `m_commandButtons` and takes no click -- and it yields entirely on
   * a window narrow enough that the verbs reach it.
   */
  {
    const char* queueLabel = "+ QUEUE";
    const bool hasButtons = m_commandButtonCount > 0;
    const float chipWidth =
        static_cast<float>(TextCellCount(queueLabel)) * cell + 2.0f * m_commandTuning.paddingX * layout.scale;
    const UiRect chipRect{layout.commandRow.Right() - m_commandTuning.paddingX * layout.scale - chipWidth,
                          hasButtons ? m_commandButtons[0].rect.y : layout.commandRow.y + pad, chipWidth,
                          hasButtons ? m_commandButtons[0].rect.height : m_commandTuning.buttonHeight * layout.scale};
    if (chipRect.x > lastButtonRight + m_commandTuning.buttonGap * layout.scale)
    {
      m_ui.AddBorder(chipRect, 1.0f * layout.scale, m_palette.caution);
      const float labelWidth = static_cast<float>(TextCellCount(queueLabel)) * cell;
      m_ui.AddText(chipRect.x + (chipRect.width - labelWidth) * 0.5f,
                   chipRect.y + (chipRect.height - bodyPx) * 0.5f, m_uiTuning.bodySizeIndex, m_palette.caution,
                   queueLabel);
    }
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

    /*
     * The frame is `border` like every chip on this HUD; the priority speaks
     * through the head's colour and, for a critical, the alert triangle --
     * the accent is never the frame, because a five-colour stack of frames
     * would be the palette shouting over its own hierarchy. Urgent keeps the
     * amber the old TOAST_URGENT_COLOUR intended; below urgent the head is
     * ordinary chrome, because a market fill is not a warning.
     */
    std::uint32_t accent = m_palette.phosphor;
    if (isCritical)
    {
      accent = m_palette.critical;
    }
    else if (toast.priority == ToastPriority::Urgent)
    {
      accent = m_palette.caution;
    }
    m_ui.AddQuad(rect, m_palette.panel);
    m_ui.AddBorder(rect, 1.0f * layout.scale, m_palette.border);

    // A count only when there is one to report -- the sheet draws the coalesced
    // form as a suffix, and "x1" on every row would be noise. The critical head
    // leads with U+25B2, the sheet's alert triangle: colour is never the only
    // signal.
    const char* lead = isCritical ? "\xE2\x96\xB2 " : "";
    if (toast.count > 1)
    {
      std::snprintf(buffer, sizeof(buffer), "%s%s x%u", lead, toast.head.c_str(), toast.count);
    }
    else
    {
      std::snprintf(buffer, sizeof(buffer), "%s%s", lead, toast.head.c_str());
    }
    m_ui.AddText(rect.x + pad, rect.y + pad * 0.5f, m_uiTuning.bodySizeIndex, accent, buffer);
    m_ui.AddText(rect.x + pad, rect.y + pad * 0.5f + bodyPx + 2.0f * layout.scale, m_uiTuning.smallSizeIndex,
                 m_palette.phosphorBody, toast.detail);
  }

  // --- the Tier-1 diagnostics strip (S14) ---------------------------------
  //
  // Collection always, drawing behind the toggle: the drain is what keeps the
  // lanes from overflowing whether or not anyone is looking. Cost is measured
  // around both halves and displayed by the strip itself next frame -- the
  // observer effect cannot be removed, so it is reported (debug-hud.png §4).
  {
    const std::int64_t stripStart = Clock::Counter();
    CollectDiagnostics(nowSeconds);
    if (m_diagnosticsVisible)
    {
      DebugStripStyle style;
      style.x = layout.world.x + pad;
      style.y = layout.world.y + pad;
      style.scale = layout.scale;
      style.cellPixels = cell;
      style.smallLinePixels = smallPx;
      style.smallSizeIndex = m_uiTuning.smallSizeIndex;
      style.bodySizeIndex = m_uiTuning.bodySizeIndex;
      (void)BuildDebugStrip(m_stripReadout, style, m_palette, m_ui);
    }
    m_stripCostMs = Clock::MillisecondsBetween(stripStart, Clock::Counter());
  }
}

void ClientApp::CollectDiagnostics(double _nowSeconds)
{
  // The collector's drain (ADR-007 §8): every lane, once a frame, on the game
  // thread. This is bounded by ring capacity rather than by event count, which
  // is what keeps the instrument inside a fixed budget under exactly the load
  // that makes it interesting.
  m_telemetry.Clear();
  m_telemetry.DrainAll();
  m_stripHistory.AccumulateFrame(m_telemetry, _nowSeconds);

  if (!m_diagnosticsVisible)
  {
    return; // Accumulated but not asked: the readout is a display value.
  }

  m_stripHistory.FillTimings(m_stripReadout);
  m_stripReadout.stripMs = m_stripCostMs; // Last frame's; this frame's is still being spent.

  const bool joined = m_connection.State() == ClientLinkState::Joined;
  m_stripReadout.joined = joined;
  if (joined)
  {
    // Asked only while the strip is up: the transport query walks into msquic,
    // and a hidden strip should cost nothing but the drain above.
    const TransportStats stats = m_connection.Stats();
    m_stripReadout.rttMs = m_connection.RoundTripMs();
    m_stripReadout.minRttMs = stats.minRoundTripMs;
    m_stripReadout.controlResends = stats.controlResends;
    m_stripReadout.datagramsDropped = stats.datagramsDropped;
  }

  m_stripReadout.hasEstimate = m_snapshots.HasEstimate();
  m_stripReadout.stale = m_snapshots.Stale(_nowSeconds);
  m_stripReadout.snapAgeMs = m_snapshots.SecondsSinceSnapshot(_nowSeconds) * 1000.0;
  m_stripReadout.driftTicks = m_snapshots.DriftTicks();
  m_stripReadout.outOfOrder = m_snapshots.OutOfOrderCount();

  // Viewer-held counts: the scene the game handed over and what the passes
  // issued last frame. Never a world-truth count (debug-hud.png §1).
  m_stripReadout.replicated = static_cast<std::uint32_t>(m_scene.entities.size());
  m_stripReadout.drawnInstances = m_passes.Opaque().InstanceCount();
  m_stripReadout.drawCalls = m_passes.Opaque().DrawCount();
  m_stripReadout.glyphQuads = m_passes.Ui().GlyphCount();
  m_stripReadout.missingGlyphs = m_passes.Ui().MissingGlyphCount();
  m_stripReadout.snapshotDrops = m_connection.SnapshotOverflowCount();
  m_stripReadout.audioVoices = m_audio.ActiveVoices();
  m_stripReadout.audioVoiceCap = m_audio.VoiceCapacity();

  std::uint64_t telemetryDrops = 0;
  for (LaneId lane = 0; lane < Telemetry::LaneCount(); ++lane)
  {
    telemetryDrops += Telemetry::DroppedCount(lane);
  }
  m_stripReadout.telemetryDrops = telemetryDrops;
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
  const bool msaa = m_swapChain.SampleCount() > 1;

  // With MSAA the back buffer's first job this frame is to receive the
  // resolve, not to be drawn on -- the world renders into the offscreen
  // target and only the Ui pass touches the back buffer directly (S14).
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = backBuffer;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = msaa ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET;
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
  context.uiWorld = &m_uiWorld;
  context.glyphAtlas = &m_glyphAtlas;
  context.meshes = &m_meshes;
  context.scene = &m_scene;
  // The world's target: the MSAA offscreen when one exists, the back buffer
  // otherwise. The Ui pass never reads this -- it draws on whatever the frame
  // loop has bound by then, which after a resolve is the back buffer.
  context.renderTargetView = msaa ? m_swapChain.MsaaRenderTargetView() : m_swapChain.CurrentRenderTargetView();
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

  m_passes.RecordWorld(context);

  if (msaa)
  {
    /*
     * The resolve, between the two halves of the frame (S14). The barriers are
     * the frame loop's for the same reason the PRESENT transitions are: a pass
     * that transitioned resources it does not own could not be reordered
     * without reading every other pass first.
     *
     * The MSAA target ends the block back in RENDER_TARGET, so it is always in
     * that state at frame start and never needs a barrier there.
     */
    ID3D12Resource* msaaColour = m_swapChain.MsaaColour();

    D3D12_RESOURCE_BARRIER toResolve = barrier;
    toResolve.Transition.pResource = msaaColour;
    toResolve.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toResolve.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    m_commandList->ResourceBarrier(1, &toResolve);

    m_commandList->ResolveSubresource(backBuffer, 0, msaaColour, 0, GpuSwapChain::RESOLVE_FORMAT);

    D3D12_RESOURCE_BARRIER afterResolve[2] = {barrier, barrier};
    afterResolve[0].Transition.pResource = msaaColour;
    afterResolve[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    afterResolve[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    afterResolve[1].Transition.pResource = backBuffer;
    afterResolve[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    afterResolve[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(2, afterResolve);

    // The HUD draws on the resolved image, single-sampled, with no depth --
    // its pipeline declares DSVFormat UNKNOWN, so none is bound. The viewport
    // and scissor persist from ClearPass; only the target changes.
    const D3D12_CPU_DESCRIPTOR_HANDLE backBufferView = m_swapChain.CurrentRenderTargetView();
    m_commandList->OMSetRenderTargets(1, &backBufferView, FALSE, nullptr);
  }

  m_passes.RecordUi(context);

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

  // Before the GPU teardown rather than after: XAudio2 owns threads that are
  // still mixing until this returns, and they have nothing to do with D3D12 --
  // stopping them first keeps the two shutdowns from interleaving.
  m_audio.Destroy();

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
