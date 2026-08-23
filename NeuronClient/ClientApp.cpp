#include "pch.h"

#include "ClientApp.h"

#include "UploadBudget.h"

#include "Clock.h"
#include "Log.h"
#include "Telemetry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

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

/*
 * What this frame's upload segment is, given what the config asked for
 * (ADR-018 A20).
 *
 * The 256 KiB constant this replaces said it was "sized for the corpus's 1,024
 * instances plus the overlay and text streams that join them" -- and for the
 * *tactical* view that was true, which is why nothing ever caught it. It is
 * roughly `MIN_UPLOAD_BYTES_PER_FRAME` below, and the streams it left out are
 * the strategic map's: at the corpus's full size those alone are four times the
 * whole segment, and every pass drops its stream **entirely** when the ring is
 * short, so U5 would have opened on a blank screen.
 *
 * Raising rather than refusing a too-small configured value, which is the one
 * place this file departs from ADR-012's no-clamping posture. The config layer
 * is where a bad number is an error, and it cannot be one there: `AppConfig`
 * may not name a NeuronClient constant (Dependency-Map). So the floor is
 * enforced where it is known, and said out loud rather than applied quietly.
 */
[[nodiscard]] std::uint32_t ResolveUploadBytesPerFrame(std::uint32_t _configured) noexcept
{
  if (_configured == 0)
  {
    return UPLOAD_BYTES_PER_FRAME;
  }
  if (_configured < MIN_UPLOAD_BYTES_PER_FRAME)
  {
    NEURON_LOG_WARNING("upload ring: %u B is below the %u B a full grid costs; using the floor", _configured,
                       MIN_UPLOAD_BYTES_PER_FRAME);
    return MIN_UPLOAD_BYTES_PER_FRAME;
  }
  return _configured;
}

/// The HUD's sizes before the UI scale multiplier (ADR-006 §9). The second is
/// the chrome band the prints are set in -- 15, not 16. The trailing micro
/// size is for secondary lines under world marks (the order label's
/// `2.1 KM · ETA 19S`), appended rather than inserted so the three indices the
/// tuning already names keep meaning what they meant.
constexpr float BASE_FONT_SIZES_PIXELS[] = {13.0f, 15.0f, 22.0f, 11.0f};

/// The top bar's menu chip, spelled as UTF-8 bytes like every marker glyph in
/// this file: U+25A5 and the word.
constexpr const char* MENU_CHIP_LABEL = "\xE2\x96\xA5 MENU";

/*
 * The way off a full-screen surface (`◀`, U+25C0).
 *
 * A surface *name* rather than game vocabulary, which is what makes it the
 * engine's to write: `SurfaceId` already enumerates the screens this client
 * has, so naming one is not the leak that naming a station's tabs would be
 * (ADR-020 §6).
 */
constexpr const char* BACK_CHIP_LABEL = "\xE2\x97\x80 TACTICAL";

/*
 * Air left around a fleet when a roster press frames it, on top of whatever the
 * HUD is already covering.
 *
 * A framing that put the outermost hull exactly on the edge would read as the
 * fleet being clipped rather than as it being shown.
 */
constexpr float FLEET_FRAME_AIR = 0.12f;

/*
 * Emissive strength per canonical material (ADR-006 §6).
 *
 * Not content: the .mtl files carry albedo, and which of the five materials
 * *glows* is a renderer decision the exporter has no way to express. Accent and
 * thruster carry the emissive channel; glass is simply dark, which is what
 * makes a cockpit read as glass against a lit hull rather than as a hole.
 *
 * Read as a multiple of the material's own albedo, which is what the pixel
 * shader does with it. `accent` glows *mildly* -- it is a stripe on a dark
 * hull, and a stripe that outshines the hull stops being a marking -- while
 * `thruster` glows past white, because an engine bell is the one place on a
 * hull that is genuinely a light source. Both are unlit by the key light, so
 * they hold their colour whichever way the ship is facing.
 *
 * They were 1.6 and 2.4 against the previous rig, whose fill was a fifth of
 * this one's; the accents kept their contrast against a hull that is now four
 * times brighter by coming down rather than by the hull going back.
 */
constexpr float MATERIAL_EMISSIVE[MESH_MATERIAL_COUNT] = {0.0f, 0.0f, 0.0f, 0.6f, 1.3f};

/*
 * The lighting rig (ADR-006 §6): a key, a hemispherical fill, and a bounce.
 *
 * Constants rather than content, and all of them here rather than spread down
 * `BuildFrameConstants`, because they are one decision: the two fill tints and
 * the bounce are quoted as *fractions of the key* and are meaningless read
 * apart from it. The only thing content says about light is the nebula's tint,
 * which is the field the fleet sits inside rather than what lands on a hull.
 *
 * Flat-shaded facets are the whole reason there are three terms. One light
 * gives adjacent faces distinct tones; without the fill the faces turned away
 * from it are holes; without the bounce a hull's shadow side has nothing to
 * separate it from a background that is nearly the same colour.
 */

/// Where the key travels, in render space (east, up, north). ~55 degrees of
/// elevation -- deliberately higher than the 30 the camera sits at, so the top
/// facets of a hull carry warm light under a top-down view and the scene never
/// reads as backlit. Normalised at use; the literal is a direction, not a unit
/// vector somebody has to keep unit by hand.
constexpr DirectX::XMFLOAT3 KEY_DIRECTION{-0.35f, -0.82f, 0.45f};

/// Warm white. The one warm thing in the frame, which is what makes the green
/// everywhere else read as green rather than as a colour cast.
constexpr DirectX::XMFLOAT3 KEY_COLOUR{1.00f, 0.96f, 0.90f};
constexpr float KEY_INTENSITY = 1.0f;

/// The hemispherical fill's two ends: a green-white sky above, near-black
/// ground below. Lerped by the normal's Y, which costs two constants and is the
/// difference between a faceted hull and a flat unlit one.
constexpr DirectX::XMFLOAT3 FILL_SKY_TINT{0.87f, 0.96f, 0.75f};
constexpr DirectX::XMFLOAT3 FILL_GROUND_TINT{0.06f, 0.11f, 0.06f};
constexpr float FILL_FRACTION_OF_KEY = 0.28f;

/// The bounce, at the accent hue. Weak on purpose: enough that a shadow-side
/// hull has an edge against the background, far too little to read as a hull
/// painted green. It comes from the camera's side, so it is the one term that
/// moves when the view orbits.
constexpr DirectX::XMFLOAT3 BOUNCE_TINT{0.61f, 0.94f, 0.12f};
constexpr float BOUNCE_FRACTION_OF_KEY = 0.17f;

/// UPPERCASE into a fixed buffer, for the command verbs and the formation
/// value: the game supplies mixed-case names and the print sets the chrome in
/// capitals. ASCII only -- a byte outside a-z passes through untouched, so a
/// marker glyph in a name would survive rather than be mangled.
template <std::size_t N>
void UpperCaseInto(const char* _text, char (&_out)[N])
{
  static_assert(N > 0, "somewhere to put the terminator");
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

  // The colour table, and everything derived from it. Through `ApplyPalette` so
  // the settings screen can run the same resolve again on a change rather than
  // only at boot (ADR-020 §8) -- which is what makes a swap total instead of
  // partial.
  ApplyPalette(_config.uiPalette);

  // **Which** status bits get a mark is the composition root's answer and
  // arrives in the config; the colour is the palette's and is set beside the
  // rest in `ApplyPalette`.
  m_overlayTuning.statusMarkBits = _config.statusMarkBits;

  /*
   * N3's two input settings, applied here for the same reason the palette is:
   * they are config words on the way in and live state from here on, and the
   * settings screen changes both without a restart.
   */
  m_gestureTuning.longPressSeconds = _config.longPressSeconds;
  m_handedness = ResolveHandedness(_config.uiHandedness);

  // What the puck's orders are, from the side that knows. Once, at boot: there
  // is one command until the wheel exists, and asking every frame would be
  // asking a question whose answer is compiled in.
  m_orderDefaults = _worldView.DefaultOrder();
  m_selectedKind = m_orderDefaults.kind;

  // The command row's buttons. Asked once: a game's command list does not
  // change while a session runs, and asking every frame would imply it could.
  // Once here for the boot-time facts the rest of `Initialise` reads off it --
  // the parameter names and the option lists. `BuildHud` asks again every frame,
  // because availability is about the selection and the grid rather than about
  // the build (see there).
  m_orderKindCount = _worldView.OrderKinds({}, m_orderKinds);

  /*
   * Every kind's parameter list, once. Per kind rather than for the selected
   * one, because the context bar states each standing parameter whether or not
   * its verb is selected. Each kind starts on the game's own default rather
   * than on its list's first entry -- they are the same today, and "the default
   * is whatever happens to be first" is the kind of agreement that holds until
   * someone reorders a menu.
   */
  for (std::uint32_t slot = 0; slot < m_orderKindCount; ++slot)
  {
    m_kindOptionCounts[slot] = _worldView.OrderOptions(m_orderKinds[slot].kind, m_kindOptions[slot]);
    m_kindDefaultIndex[slot] = 0;
    if (m_orderKinds[slot].kind == m_orderDefaults.kind)
    {
      for (std::uint32_t index = 0; index < m_kindOptionCounts[slot]; ++index)
      {
        if (m_kindOptions[slot][index].parameter == m_orderDefaults.parameter)
        {
          m_kindDefaultIndex[slot] = index;
          break;
        }
      }
    }
    m_kindOptionIndex[slot] = m_kindDefaultIndex[slot];
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
  ok = ok && m_meshes.Create(m_device, m_config.meshDirectory, m_config.meshFiles, m_config.meshPlaneRadiiMetres,
                             m_taskPool);
  // After the meshes and from nothing but them: every lamp is a fraction of a
  // bounding box the loader has just measured, so this cannot be told a hull
  // size the renderer is not drawing (ADR-006 §6a).
  ok = ok && m_lamps.Create(m_device, m_meshes, m_config.meshLampRigs);
  ok = ok && m_uploadRing.Create(m_device.Device(), ResolveUploadBytesPerFrame(m_config.uploadBytesPerFrame),
                                GpuSwapChain::BUFFER_COUNT);

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
      /*
       * What the contacts meant, before anything reads it (I1, I2).
       *
       * Ahead of the camera because the camera is the first consumer: a drag
       * pans, and `UpdateCamera` has to know whether this contact became one.
       * The router is handed the same answer inside `UpdateHud`, which is where
       * `Begin` latches the frame.
       */
      m_gestures.Update(m_input, m_gestureTuning, deltaSeconds);
      UpdateCamera(deltaSeconds);
      UpdateHud();
      UpdateSelection();
      UpdateOrders();
      // After the camera and the selection, because it reports both.
      SendViewFocus();
    }
    {
      /*
       * Extract runs only when the world half will be recorded (ADR-020 §1).
       *
       * The *network* half runs on every surface -- snapshots keep arriving and
       * keep the interpolation buffer full while a screen is up, so coming back
       * to tactical costs no refill and owes no settle: a surface switch is not
       * a view switch. What there is no point doing is building a scene nothing
       * will draw.
       *
       * The span is recorded either way, at zero, because a budget row that
       * disappeared would read as a stage that broke rather than one that had
       * nothing to do.
       */
      NEURON_SPAN("Extract");
      if (SurfaceRecordsWorld(m_surfaces.Active()))
      {
        ExtractScene();
      }
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
    for (const ClientConnection::ReceivedFrame& received : m_connection.PendingFrames())
    {
      /*
       * The tick comes off the *header* now rather than back out of the game
       * (ADR-022 §3b): the envelope is the engine's, so the number the clock
       * estimate tracks is the number the framing already carries. A frame the
       * game refuses -- a malformed tail -- must not move the clock, which is
       * the same rule as before with a different owner.
       */
      ReplicatedFrame frame;
      frame.tick = received.tick;
      frame.gridId = received.gridId;
      frame.culledCount = received.culledCount;
      frame.entities = received.entities;
      frame.tail = received.tail;
      if (m_worldView->ApplyFrame(frame))
      {
        m_snapshots.OnSnapshot(received.tick, nowSeconds);
      }
    }
  }
  m_connection.ClearPendingFrames();

  /*
   * The 1 Hz family, across the same seam and with the same ignorance
   * (ADR-016 6). No clock comes back: a summary describes a state rather than
   * an instant, so nothing here drives the render tick.
   *
   * Dropped while the feed is frozen for the reason the snapshots are -- F10
   * reproduces a sender that stopped, and a hangar panel that kept updating
   * behind a frozen fleet would be the one surface still telling the truth in a
   * mode whose whole purpose is that none of them can.
   */
  if (!m_feedFrozen)
  {
    for (const std::vector<std::uint8_t>& payload : m_connection.PendingSummaries())
    {
      if (!m_worldView->ApplySummary(payload))
      {
        // Counted by the game and logged at debug here: a summary that failed
        // to parse is a stale panel, which is a diagnostic rather than an
        // event the player is owed.
        NEURON_LOG_DEBUG("net: a summary payload was refused by the game");
      }
    }
  }
  m_connection.ClearPendingSummaries();

  /*
   * Where the feed is pointed, which the connection has been holding and
   * nothing has been reading (ADR-014 5: the engine moved the bytes, the client
   * decides what a changed view looks like).
   *
   * Both answers matter and they are not symmetric. An accepted switch is a new
   * world and everything keyed on the old one has to go; a refused one changed
   * nothing at all -- the wire half is explicit that a refusal leaves the feed
   * exactly where it was -- so the only thing owed is telling the player why,
   * which is a toast and not a state change.
   */
  for (const ViewChanged& change : m_connection.PendingViewChanges())
  {
    // Either answer ends the request auto-follow may have in flight. A refusal
    // additionally remembers the grid, so the condition that produced it cannot
    // immediately produce it again.
    m_followRequested = INVALID_FOLLOW_ANCHOR;
    if (change.accepted)
    {
      m_followRefused = INVALID_FOLLOW_ANCHOR;
      OnViewChanged(change.gridAnchor, nowSeconds);
    }
    else
    {
      m_followRefused = change.gridAnchor;
      // A18's toast row for a refused switch. The words are the game's, on the
      // same path a refused order takes, because a player who cannot go
      // somewhere is owed the same sentence whichever surface they asked from.
      const char* reason = m_worldView->ReasonText(change.reasonCode);
      (void)m_toasts.Raise(ToastPriority::Urgent, change.reasonCode, "VIEW REFUSED",
                           reason != nullptr ? reason : "", nowSeconds);
      NEURON_LOG_INFO("view request for grid %u refused: %s", change.gridAnchor, reason != nullptr ? reason : "");
    }
  }
  m_connection.ClearPendingViewChanges();

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
  // Latched first and unconditionally: the frame has to be consumed whatever
  // is on screen, or the next one arrives holding two frames of edges.
  m_input = m_window.ConsumeInput();

  /*
   * A full-screen surface has no world to move the camera over (ADR-020 §1).
   *
   * This runs *before* `UpdateHud`, so it reads the frame directly rather than
   * through the router -- which means a claim made downstream cannot reach it,
   * and the surface has to be asked here. Without it, a wheel spun over the
   * hangar's wing columns scrolls the list *and* zooms a camera nobody can see,
   * and W pans the same invisible view a rename box will want the key for.
   *
   * Asked of the surface rather than solved by reordering the frame, because
   * the reason is not a routing accident: there is no world on screen, so
   * there is nothing for a camera move to mean.
   */
  if (!SurfaceRecordsWorld(m_surfaces.Active()))
  {
    return;
  }

  /*
   * **Drag pans** (I2, Plan-of-Record §1's rule 1), and only a drag that began
   * over the world.
   *
   * The same rule the box-select drag carried before it: a gesture may only
   * *begin* in the world zone, because the HUD is a border rather than an
   * overlay and everything outside `world` has a panel on it. Once begun it may
   * leave freely -- a pan that stopped when the finger crossed the roster would
   * be worse than the bug it prevented.
   *
   * Tested against where the contact went **down** rather than where it is now,
   * which is what makes "began in the world" a fact about the gesture rather
   * than about this frame. And read from the recognizer rather than the router,
   * because this runs before `Begin` latches the frame -- the claim cannot
   * reach here, which is the same reason the surface is asked above.
   *
   * `m_uiLayout` is the previous frame's, resolved in `UpdateHud`. It changes
   * only on a resize, and a drag that began before one is a drag whose zone
   * test was made against the layout the player could see.
   */
  const GestureState& gesture = m_gestures.State();
  const bool panning =
      gesture.phase == GesturePhase::Dragging && m_uiLayout.world.Contains(gesture.originX, gesture.originY) && !m_menuOpen;

  const CameraIntent intent = MapCameraInput(m_input, m_cameraTuning, _deltaSeconds, panning ? gesture.deltaX : 0.0f,
                                             panning ? gesture.deltaY : 0.0f);
  ApplyCameraIntent(m_camera, intent);
}

std::uint32_t ClientApp::KindSlot(std::uint16_t _kind) const noexcept
{
  for (std::uint32_t slot = 0; slot < m_orderKindCount; ++slot)
  {
    if (m_orderKinds[slot].kind == _kind)
    {
      return slot;
    }
  }
  return m_orderKindCount;
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

  /*
   * This frame's input, latched, with the focus owner's word for what the
   * keyboard looks like (ADR-020 §2). Everything below asks the router rather
   * than the frame, and the order it asks in *is* the stage order: focused
   * widget, then the active surface, then the cross-surface layers, then the
   * world -- which is `UpdateSelection` and `UpdateOrders`, further down the
   * frame.
   */
  m_router.Begin(m_input, m_focus);

  /*
   * And what the contacts meant, on the same latched frame (I1).
   *
   * After `Begin`, which clears the router's copy: the recognizer is what
   * carries a gesture across frames, and handing the answer over here is what
   * puts it behind the pointer claim like every other pointer question. It was
   * *updated* at the top of the frame, because the camera reads it too and runs
   * before this.
   */
  m_router.NoteGesture(m_gestures.State());

  if (m_router.WindowDeactivated())
  {
    // One of focus's three clearing rules, and the only one the router can
    // notice on its own. The other two -- a surface exiting, a pointer claim
    // landing outside -- are raised where they happen.
    m_focus.Clear();
  }

  /*
   * What the game says about the fleet, asked before anything is laid out
   * because the column's geometry depends on how many rows there are, and the
   * column has a live control on it now.
   *
   * It moved here from `BuildHud` for that reason alone. The selection it is
   * asked with is the one `UpdateSelection` left last frame, which is the same
   * relationship the command row's `hasSelection` has always had -- one frame,
   * at display rate, on a highlight.
   */
  m_rosterRowCount = m_worldView->BuildRoster(m_selection.Ids(), m_rosterRows);
  m_locationBlockCount = m_worldView->BuildLocationBlocks(m_locationBlocks);

  // Straight after the blocks, because they are what it reads. A frame late
  // would be a frame of empty grid the player had to look at.
  AdvanceAutoFollow();

  // The ELSEWHERE column, laid out where it is pressed and handed to the draw
  // (ADR-020 §5.1).
  m_locationLayoutCount = BuildLocationBlockColumn(
      std::span<const LocationBlock>{m_locationBlocks, m_locationBlockCount}, m_uiLayout.roster,
      RosterBlocksTop(m_uiLayout.roster, m_rosterRowCount, m_uiLayout.scale, m_rosterTuning), m_uiLayout.scale,
      m_rosterTuning, m_locationLayouts);

  /*
   * The way back off a full-screen surface, laid out here and drawn from the
   * same rect (ADR-020 §5.1).
   *
   * `◀ TACTICAL` and `◀ BACK` are one control because they are one mechanism --
   * `SurfaceStack::Back` -- so there is one chip rather than one per screen.
   */
  {
    const float scale = m_uiLayout.scale;
    const float cell = 8.0f * scale;
    const float pad = m_uiTuning.padding * scale;
    const float chipWidth = static_cast<float>(TextCellCount(BACK_CHIP_LABEL)) * cell + 2.0f * cell;
    m_backChipRect = UiRect{m_uiLayout.viewport.x + pad, m_uiLayout.viewport.y + pad, chipWidth,
                            m_uiTuning.topBarHeight * scale - pad};
  }

  // The diagnostics toggle, before anything can consume the frame's edges. A
  // level edge rather than a chord: the strip is a setting with a shortcut,
  // not a gesture (debug-hud.png §6).
  if (m_router.Pressed(InputAction::ToggleDiagnostics))
  {
    m_diagnosticsVisible = !m_diagnosticsVisible;
  }

  // The induced stall, beside the strip's toggle because it is the instrument
  // the strip is read with. Logged rather than drawn: the screen is what is
  // being judged while this is on, so the record of it belongs in the file.
  if (m_router.Pressed(InputAction::ToggleFeedFreeze))
  {
    m_feedFrozen = !m_feedFrozen;
    NEURON_LOG_INFO("feed %s (F10)", m_feedFrozen ? "cut -- the world will go stale" : "restored");
  }

  /*
   * Escape, in ADR-020 §2's order: the focused field first to cancel an edit,
   * then the active surface to go back, then nothing.
   *
   * No field can hold focus yet -- the first one is the hangar's wing rename --
   * so the first clause is written as the router's `Characters`/`ClaimKeyboard`
   * pair rather than as a special case, and the surface's clause is the one
   * that does work today. The open menu counts as something to leave: a player
   * pressing Escape with a list up means the list.
   */
  if (m_router.Pressed(InputAction::Back))
  {
    (void)m_router.ClaimKeyboard();
    if (m_menuOpen)
    {
      m_menuOpen = false;
    }
    else
    {
      OnSurfaceChanged(m_surfaces.Back());
    }
  }

  const std::uint32_t selectedSlot = KindSlot(m_selectedKind);
  const std::span<const OrderOption> selectedOptions =
      selectedSlot < m_orderKindCount
          ? std::span<const OrderOption>{m_kindOptions[selectedSlot], m_kindOptionCounts[selectedSlot]}
          : std::span<const OrderOption>{};
  // The one thing that decides whether a verb is live, read off the selection
  // this frame -- local UI state and nothing else, so cutting the feed cannot
  // leave the row claiming a fleet that is no longer there.
  CommandContext commandContext;
  commandContext.hasSelection = !m_selection.Ids().empty();

  m_commandButtonCount =
      BuildCommandRow(std::span<const OrderKindOption>{m_orderKinds, m_orderKindCount}, m_selectedKind,
                      selectedOptions, selectedSlot < m_orderKindCount ? m_kindOptionIndex[selectedSlot] : 0,
                      commandContext, m_uiLayout.commandRow, m_uiLayout.scale, m_commandTuning, m_commandButtons);

  /*
   * The `▥ MENU` chip and, when open, its list -- laid out here so the rect a
   * click is tested against is the rect the draw uses, the same single-answer
   * rule `m_uiLayout` exists for. The chip hangs off the top bar's right edge;
   * the list drops under it, over the world, which is why the world's input
   * yields to it below.
   */
  {
    const float scale = m_uiLayout.scale;
    const float cell = 8.0f * scale;
    const float pad = m_uiTuning.padding * scale;
    const float chipWidth = static_cast<float>(TextCellCount(MENU_CHIP_LABEL)) * cell + 2.0f * cell;
    m_menuButtonRect = UiRect{m_uiLayout.topBar.Right() - pad - chipWidth, m_uiLayout.topBar.y + pad * 0.5f, chipWidth,
                              m_uiLayout.topBar.height - pad};

    const float itemWidth = m_commandTuning.buttonWidth * scale;
    const float itemHeight = 34.0f * scale;
    float itemY = m_uiLayout.topBar.Bottom() + pad * 0.5f;
    for (std::uint32_t item = 0; item < MENU_ITEM_COUNT; ++item)
    {
      m_menuItemRects[item] = UiRect{m_uiLayout.topBar.Right() - pad - itemWidth, itemY, itemWidth, itemHeight};
      itemY += itemHeight;
    }
  }

  /*
   * A full-screen surface is the active surface, and it owns the pointer --
   * every button of it, every frame it is up.
   *
   * Ahead of the left-press gate below for exactly that reason. Gating on a
   * left press first would leave the *right* button unclaimed, and the order
   * puck lives on the right button: a right-drag across the hangar would have
   * sent a fleet somewhere on a grid the player cannot see.
   *
   * Its own control goes first, and whatever else the press landed on is the
   * surface's own air rather than the tactical HUD's -- the chrome underneath
   * is not showing, so a press reaching the command row would be a button
   * nobody could see being pressed.
   */
  if (!SurfaceRecordsWorld(m_surfaces.Active()))
  {
    // Which full-screen surface is up. Each handles what it drew, for the reason
    // the branch exists at all: a press against a screen that is not showing is
    // a press nobody could have aimed.
    if (m_surfaces.Active() == SurfaceId::Settings)
    {
      UpdateSettingsSurface();
    }
    else
    {
      UpdateStationSurface();
    }
    return;
  }

  if (!m_router.Pressed(InputButton::Left))
  {
    return;
  }

  const float cursorX = m_router.CursorX();
  const float cursorY = m_router.CursorY();

  /*
   * The menu takes the press before everything under it: while the list is open
   * every left press belongs to it -- an item acts, anywhere else closes --
   * because a press that both closed the menu and box-selected the fleet under
   * it would be two answers to one gesture.
   *
   * It claims the pointer rather than setting a flag the world remembers to
   * check, which is the same guarantee `m_uiConsumedPress` gave and the general
   * form of it (ADR-020 §2).
   */
  if (m_menuOpen)
  {
    (void)m_router.ClaimPointer();
    m_menuOpen = false; // Whatever was pressed, the list has had its answer.
    if (m_menuItemRects[MENU_SETTINGS].Contains(cursorX, cursorY))
    {
      // Live since N3. It was drawn dead and honest about it -- the list stayed
      // so the entry would not appear from nowhere the day the screen landed.
      OnSurfaceChanged(m_surfaces.Push(SurfaceId::Settings));
    }
    else if (m_menuItemRects[MENU_EXIT].Contains(cursorX, cursorY))
    {
      // The window's own close path, so EXIT and the title bar's X are one
      // shutdown rather than two.
      PostMessageW(m_window.Handle(), WM_CLOSE, 0, 0);
    }
    return; // RESUME, the chip, or anywhere else: closed, and nothing more.
  }
  if (m_router.ClaimPointerIn(m_menuButtonRect))
  {
    m_menuOpen = true;
    return;
  }

  /*
   * The way into the hangar (ADR-017 §6, T3).
   *
   * A docked block's button is the roster's own route to the station screen,
   * and it opens the screen for **any** station holding the player's ships,
   * viewed or not -- focus never gates command, so a remote hangar is the same
   * press as the one you are looking at. The anchor rides along because the
   * screen has to know which hangar it is; the engine echoes it and never reads
   * it.
   */
  /*
   * A tap on a roster row takes that wing; a second tap looks at it.
   *
   * It used to be one gesture that did both, and separating them is
   * Plan-of-Record §1's rule 2 -- see the note on the framing below for what
   * that cost a player mid-order.
   *
   * The row already knew how: `RosterRow::groupId` has been documented since
   * S11 as "what a click on the row would hand back to the game, once rows are
   * clickable", and `BuildGroupMembers` is the other end of that sentence. The
   * engine hands back an opaque number and gets entity ids; it never learns
   * that a group is a wing.
   *
   * **The rows are the fleets on *this* grid, which is what makes the gesture
   * whole.** `BuildRoster` counts the frame's own sampled fleet, so a row is by
   * construction a group the camera can be pointed at -- ships anywhere else
   * are ELSEWHERE blocks below, and those open the station screen instead. An
   * empty row is still a row (a wing whose ships all left is drawn at zero), and
   * that is the one case here that does nothing: there is nothing to select and
   * nowhere to look.
   *
   * **Driven by the gesture rather than by the press, since I2.** A tap takes
   * the wing, a second tap frames it and a long-press adds it -- three meanings
   * on one row, which a raw button edge cannot tell apart. The point tested is
   * where the gesture *is*: a tap reports where the finger lifted, and a
   * long-press where it is still holding.
   */
  const GestureState& gesture = m_router.Gesture();
  const bool rosterEdge = gesture.tapped || gesture.longPressed;
  const float gestureX = gesture.tapped ? gesture.tapX : gesture.x;
  const float gestureY = gesture.tapped ? gesture.tapY : gesture.y;

  if (const std::uint32_t row = rosterEdge ? HitRosterChip(m_uiLayout.roster, m_rosterRowCount, m_uiLayout.scale,
                                                           m_rosterTuning, gestureX, gestureY)
                                           : m_rosterRowCount;
      row < m_rosterRowCount && m_router.ClaimPointer())
  {
    const RosterRow& fleet = m_rosterRows[row];

    // Sized from the game's own count of this wing in this frame, so the call
    // below cannot truncate.
    m_groupMembers.assign(fleet.shipCount, 0);
    const std::uint32_t members = m_worldView->BuildGroupMembers(fleet.groupId, m_groupMembers);
    m_groupMembers.resize(members);

    if (members > 0)
    {
      /*
       * **Taking a wing, and adding to one, are two gestures now** (I2,
       * Plan-of-Record §1's rules 2 and 4).
       *
       * A tap takes the wing. A **long-press** adds it to what is already
       * selected -- the world's long-press is spent on the order surfaces, the
       * roster's is not, and T3b's press-versus-hold reasoning already
       * establishes the idiom where it collides with nothing. The shift
       * accelerator still adds, because a desk player will reach for it and
       * demoting keybindings is not dropping them; what changed is that the
       * design no longer *requires* one (ADR-020's amendment).
       */
      const bool adding = gesture.longPressed || m_router.Down(InputAction::SelectAdd);
      if (adding)
      {
        for (const EntityId id : m_groupMembers)
        {
          m_selection.Add(id);
        }
      }
      else
      {
        m_selection.Set(m_groupMembers);
      }

      /*
       * **And framing is a second tap, not a side effect of the first**
       * (rule 2).
       *
       * This used to frame unconditionally, and that was the defect the plan
       * names: a player taking a fleet in order to command it had the plane
       * move under the order they were about to give. Taking and looking are
       * two intentions, so they are two gestures -- and because `tapped` fires
       * on both taps of a double, the second one *adds* the framing rather than
       * replacing what the first did. Nothing waits for a timeout to find out
       * whether a second tap is coming.
       *
       * A long-press never frames: it is composing a selection, and moving the
       * camera part-way through composing one is the same defect wearing a
       * different gesture.
       *
       * The whole *selection* rather than the wing pressed, which is the same
       * thing on a plain tap and the honest answer once more than one wing is
       * in it.
       *
       * A snap rather than a glide, which is `ResetView`'s idiom -- the one
       * other thing in this client that puts the camera somewhere.
       *
       * **The margin is the client's to supply because the chrome is.** The
       * camera fits to its viewport and cannot see that a roster column and a
       * command row are standing on part of it, so the fraction below is the
       * larger of the two dimensions the HUD eats, plus air. A known limit
       * this does not close: the fleet ends up centred on the *viewport*
       * rather than on the world zone, so it sits a little toward the roster
       * -- closing that means the camera taking a frame rect rather than a
       * fraction, which is a bigger change than this control earns.
       */
      if (gesture.tapped && gesture.tapCount >= 2)
      {
        m_focusPoints.clear();
        for (const SceneEntity& entity : m_scene.entities)
        {
          if (m_selection.Contains(entity.id))
          {
            m_focusPoints.push_back(entity.planeMetres);
          }
        }

        const UiRect& world = m_uiLayout.world;
        const float chromeWidth =
            world.width > 0.0f ? 1.0f - world.width / std::max(1.0f, m_uiLayout.viewport.width) : 0.0f;
        const float chromeHeight =
            world.height > 0.0f ? 1.0f - world.height / std::max(1.0f, m_uiLayout.viewport.height) : 0.0f;
        m_camera.FocusOn(m_focusPoints, std::max(chromeWidth, chromeHeight) + FLEET_FRAME_AIR);
      }
    }
    return;
  }

  if (const LocationBlockLayout* block =
          HitLocationBlockButton(std::span<const LocationBlockLayout>{m_locationLayouts, m_locationLayoutCount},
                                 cursorX, cursorY);
      block != nullptr && m_router.ClaimPointerIn(block->button))
  {
    m_stationAnchor = block->anchor;
    OnSurfaceChanged(m_surfaces.Push(SurfaceId::Station));
    return;
  }

  /*
   * A toast's action chip, before the command row.
   *
   * Before, because a critical is centre-top and the stack is bottom-right --
   * neither overlaps the row -- but the ordering is stated rather than left to
   * geometry: a chip the player is reaching for must win against anything that
   * happens to be behind it, and "whichever rect the loop reached first" is not
   * a rule anybody can rely on. The claim goes through the router, so the press
   * is spent once and in a stated order (ADR-020 2).
   *
   * The stack marks the row acted and hands back the key; **this file does not
   * know what the key means** and does not try to. Today nothing consumes it
   * beyond the log line -- the actions the sheet names (JUMP TO, VIEW) need
   * surfaces that do not exist yet -- so the honest thing is to complete the
   * mechanism and say so, rather than invent a destination for the number.
   */
  for (std::size_t index = 0; index < m_toastActionCount; ++index)
  {
    if (m_toastActionRects[index].width <= 0.0f || !m_router.ClaimPointerIn(m_toastActionRects[index]))
    {
      continue;
    }
    std::uint32_t actionKey = 0;
    if (m_toasts.Act(index, actionKey, Clock::SecondsSinceStart()))
    {
      NEURON_LOG_INFO("toast action %u taken", actionKey);
    }
    return;
  }

  const CommandButton* pressed =
      HitCommandRow(std::span<const CommandButton>{m_commandButtons, m_commandButtonCount}, cursorX, cursorY);
  if (pressed == nullptr)
  {
    return;
  }
  (void)m_router.ClaimPointer();

  switch (pressed->action)
  {
  case CommandAction::SelectKind:
    // Only the kind changes. Each kind keeps its own chosen parameter, so
    // coming back to MOVE finds the formation where the player left it.
    m_selectedKind = pressed->payload;
    break;

  case CommandAction::IssueNow:
  {
    /*
     * A verb that names no destination, issued by the press that lit it.
     *
     * The button was enabled because the *game* said, this frame, that it would
     * take this order for this selection -- `OrderKinds` ran `ValidateOrder`
     * over it. So the press has nothing left to ask for, and asking anyway
     * would be the puck waiting for a point that changes no answer.
     *
     * Selected as well as issued, and that is not a side effect: the parameter
     * chip belongs to the selected command, so a DOCK that could never be
     * selected would be a DOCK whose formation could never be chosen. Pressing
     * it docks the fleet *and* leaves its FORMATION on the row to change before
     * the next press.
     */
    m_selectedKind = pressed->payload;

    /*
     * Aimed at the fleet's own centre rather than at nothing.
     *
     * The target is not read for these verbs -- that is what makes them
     * destination-free -- but it is still what a refused ghost bounces from and
     * to, and a zeroed one would send that ghost to the map origin. The
     * selection's centre is the honest answer: this order acts *here*.
     */
    PuckSample here;
    (void)SelectionCentre(m_scene.entities, m_selection.Ids(), here.targetMetres);
    CommitOrder(here, Clock::SecondsSinceStart());
    break;
  }

  case CommandAction::CycleParameter:
    // The same step the `F` binding makes, through the same index, because a
    // button and a key that did the same thing by two routes would drift.
    if (const std::uint32_t slot = KindSlot(pressed->payload); slot < m_orderKindCount && m_kindOptionCounts[slot] > 0)
    {
      m_kindOptionIndex[slot] = (m_kindOptionIndex[slot] + 1) % m_kindOptionCounts[slot];
      NEURON_LOG_INFO("%s: %s", pressed->label, m_kindOptions[slot][m_kindOptionIndex[slot]].name);
    }
    break;
  }
}

void ClientApp::UpdateSelection()
{
  /*
   * **Tap selects** (I2, Plan-of-Record §1's rule 1).
   *
   * This replaced a press/move/release drag that resolved to a click or a box.
   * The gesture layer decides which a contact became -- a tap that stayed put,
   * a drag that went to the camera, a long-press that will go to the order
   * surfaces -- so what is left here is one question: did a tap happen in the
   * world, and what is under it.
   *
   * Nothing to cancel on focus loss any more, which is the shape of the change:
   * a gesture that outlives a frame lives in `GestureRecognizer`, and it drops
   * its own on an unfocused frame.
   */
  const GestureState& gesture = m_router.Gesture();
  if (!gesture.tapped)
  {
    return;
  }

  /*
   * A tap only *lands* in the world zone.
   *
   * The HUD is a border rather than an overlay (`UiLayout`), so everything
   * outside `world` has a panel on it. The menu is the one thing that floats
   * *over* the world zone, so it is excluded by name: without it a tap on
   * RESUME would also clear the fleet underneath.
   *
   * The pointer claim covers the rest -- a tap a surface took never reaches
   * here at all -- and this is the zone test the claim cannot make, because the
   * world is what is left rather than a widget that could claim itself.
   */
  if (!m_uiLayout.world.Contains(gesture.tapX, gesture.tapY) || m_menuOpen)
  {
    return;
  }

  m_selection.Tap(gesture.tapX, gesture.tapY, m_router.Down(InputAction::SelectAdd), m_scene.entities,
                  m_camera.PlaneMappingForNdc(), m_input.viewportWidth, m_input.viewportHeight,
                  m_camera.ScreenFloorMetres(Selection::PICK_FLOOR_PIXELS));
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
      // If that was an approach's first half, there is no fleet on its way and
      // the chip is promising an arrival that cannot happen.
      m_approach.NoteOrderRefused(verdict.orderSeq);
      NEURON_LOG_INFO("order %u refused by the server: %s", verdict.orderSeq, reason);
    }
  }
  m_connection.ClearPendingVerdicts();

  /*
   * Whatever the game has to say, into the stack (`alerts-and-toasts.png`).
   *
   * `Routine`, and one level for all of them: what a notice *is* is the game's
   * to write and how loudly it is said is the surface's, and everything that
   * arrives here is something that finished rather than something that went
   * wrong -- the loud levels are the refusals, which come down their own path
   * with a reason string attached.
   */
  std::array<Notice, MAX_NOTICES_PER_POLL> notices{};
  const std::uint32_t noticeCount = m_worldView->PollNotices(notices);
  for (std::uint32_t index = 0; index < noticeCount; ++index)
  {
    const Notice& notice = notices[index];
    (void)m_toasts.Raise(ToastPriority::Routine, notice.code, notice.title != nullptr ? notice.title : "",
                         notice.body != nullptr ? notice.body : "", nowSeconds);
  }

  // What the newest snapshot says is still running. This is what promotes a
  // ghost when the ack was lost, and what retires one whose order has finished.
  m_worldView->PollOrderFeedback(m_orderFeedback);
  m_ghosts.OnFeedback(m_orderFeedback, nowSeconds);
  m_ghosts.Advance(nowSeconds);

  /*
   * The chained verb, before the gesture and before every early return below.
   *
   * A fleet keeps flying while the window is unfocused and while the menu is
   * open, and the authority would take the dock in either -- so an approach
   * that only advanced while the player was looking at it would be a promise
   * kept or broken depending on where the mouse was.
   */
  AdvanceApproach(nowSeconds);

  if (m_connection.State() != ClientLinkState::Joined && !m_ghosts.Empty())
  {
    // The link went away. Every promise it was carrying died with it, and the
    // sequence numbers restart on a reconnect, so keeping them would leave one
    // session's ghosts hanging over the next one's fleet. The toasts go with
    // them for the same reason: last session's refusals over this session's
    // fleet is the same mistake.
    m_ghosts.Clear();
    m_toasts.Clear();
    // The approach's own reason: its sequence numbers restart on a reconnect,
    // so a chain held across one would send its verb against a stranger's.
    m_approach.Cancel();
    // And the transit history, for the same reason in the other direction: the
    // fleet on screen would all read as departures the frame the scene emptied,
    // and the next session's fleet as arrivals. A link dropping is not a fleet
    // going anywhere.
    m_transits.Clear();
  }

  if (!m_input.windowFocused)
  {
    m_puck.Cancel(); // No release is coming for a drag that alt-tabbed away.
    return;
  }

  if (m_menuOpen)
  {
    // The menu is the surface being used; a right-drag under it would issue an
    // order the player could not see themselves giving.
    m_puck.Cancel();
    return;
  }

  const std::uint32_t selectedSlot = KindSlot(m_selectedKind);
  if (m_router.Pressed(InputAction::CycleParameter) && selectedSlot < m_orderKindCount &&
      m_kindOptionCounts[selectedSlot] > 0)
  {
    // Stepped between gestures, not during one: the puck sampled its queue
    // modifier at the press for the same reason, and an order that changed
    // formation halfway through the drag would be an order the footprint had
    // already lied about.
    m_kindOptionIndex[selectedSlot] = (m_kindOptionIndex[selectedSlot] + 1) % m_kindOptionCounts[selectedSlot];
    NEURON_LOG_INFO("parameter: %s", m_kindOptions[selectedSlot][m_kindOptionIndex[selectedSlot]].name);
  }

  const auto cursorX = static_cast<float>(m_input.cursorX);
  const auto cursorY = static_cast<float>(m_input.cursorY);

  if (m_router.Pressed(InputButton::Right))
  {
    m_puck.Begin(cursorX, cursorY, m_router.Down(InputAction::QueueOrder));
  }
  else if (m_puck.Active())
  {
    m_puck.Update(cursorX, cursorY);
  }

  if (!m_puck.Active() || !m_router.Released(InputButton::Right))
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

  // "Select ships, act on that thing" comes first, because the same gesture
  // means both and only the game can tell them apart (ADR-017 2). A release
  // over something that affords a verb is that verb; anywhere else it is the
  // move it has always been.
  if (BeginContextAction(sample, nowSeconds))
  {
    return;
  }

  // An order the player gave themselves is the plan now. Leaving the chain
  // running would fire a verb at a station the fleet was told to fly away from,
  // which is the HUD acting on an intent the player has visibly replaced.
  m_approach.Cancel();
  CommitOrder(sample, nowSeconds);
}

bool ClientApp::BeginContextAction(const PuckSample& _sample, double _nowSeconds)
{
  /*
   * What is under the cursor, by the same pick the selection uses -- so the
   * thing acted on is the thing that would have been clicked, and a player
   * cannot act on something they could not have selected.
   *
   * **The floor is the camera's, and it used to be `INVALID_ENTITY_ID`.** That
   * was a real defect and not a spelling: the third parameter is a pick radius
   * in *metres*, so passing the id sentinel made the floor 65 km -- everything
   * on the grid was under the cursor, and the "same pick" this comment promises
   * was not the same pick at all. U3d-b widened the sentinel to u32 and turned
   * 65 km into 4.29 million, which is how it was found.
   */
  const EntityId hit = PickPoint(m_scene.entities, _sample.targetMetres, m_camera.ScreenFloorMetres(Selection::PICK_FLOOR_PIXELS));
  if (hit == INVALID_ENTITY_ID)
  {
    return false;
  }

  ContextAction action;
  if (!m_worldView->ContextActionFor(hit, m_selection.Ids(), action) || !action.available)
  {
    return false;
  }

  if (!m_approach.Begin(action, m_selection.Ids()))
  {
    return false; // More ships than one order can carry; the move stands instead.
  }

  /*
   * The approach leg: a move at the thing itself.
   *
   * Not at a computed perimeter point, and that is worth the sentence. The
   * authority already refuses to put a formation inside a hull and slides it to
   * the nearest free placement (ADR-026), so aiming at the station *is* aiming
   * at its perimeter -- and a perimeter this side computed would be a second
   * piece of geometry to keep in step with the first.
   */
  PuckSample approach = _sample;
  for (const SceneEntity& entity : m_scene.entities)
  {
    if (entity.id == hit)
    {
      approach.targetMetres = entity.planeMetres;
      break;
    }
  }
  approach.facingRadians = TravelFacingRadians(m_scene.entities, m_selection.Ids(), approach.targetMetres, 0.0f);
  approach.queued = false; // A chained verb replaces the plan; it does not join one.

  const std::uint32_t seq = m_nextOrderSeq;
  CommitOrder(approach, _nowSeconds);
  m_approach.NoteApproachSent(seq);

  NEURON_LOG_INFO("%s: approaching entity %u", action.label, hit);
  return true;
}

/*
 * Where the fleet went, and whether to go with it.
 *
 * **The trigger is arrival, not departure, and that is forced rather than
 * chosen.** `MayView` gates a view request on presence (ADR-016 §7, ADR-017 §7),
 * so the grid a fleet is warping *to* refuses the client until the fleet is
 * standing on it. Following at departure would mean asking for a grid the
 * authority is right to refuse, and the honest window between the two is the
 * crossing itself -- which U6's transit view is for, and which this is not.
 *
 * **The guard is "nothing of mine is here", not "my fleet moved".** A player
 * watching a station where their ships are docked, or a grid where half the
 * fleet stayed behind, has a reason to be there; yanking the camera because the
 * *other* half arrived somewhere would be the HUD overruling a decision the
 * player made. Only an empty place follows, and a place with a docked roster is
 * not empty.
 *
 * Ties break by anchor rather than by count, because two grids holding the same
 * number of ships is a real state and the camera has to land somewhere the same
 * way twice.
 */
void ClientApp::AdvanceAutoFollow()
{
  if (m_connection.State() != ClientLinkState::Joined || m_settleUntilSeconds >= 0.0 ||
      m_followRequested != INVALID_FOLLOW_ANCHOR)
  {
    return;
  }

  const std::uint16_t here = m_connection.GridAnchor();
  const std::uint16_t best =
    FollowTarget(std::span<const LocationBlock>{m_locationBlocks, m_locationBlockCount}, here, m_followRefused);
  if (best == NO_FOLLOW_TARGET)
  {
    return;
  }

  if (m_connection.RequestView(best))
  {
    m_followRequested = best;
    NEURON_LOG_INFO("auto-follow: nothing of ours on grid %u, asking for %u", here, best);
  }
}

/*
 * Resolves a palette name and everything downstream of it (ADR-020 §8).
 *
 * **Callable again**, which is the whole point and the reason this is a
 * function rather than a run of lines in `Create`. The ADR asks for it in as
 * many words -- *"the settings screen re-runs the resolve on change rather than
 * only at boot"* -- and what makes that sufficient is `HudPalette`'s own rule
 * that a packed colour literal in `BuildHud` is a defect: everything the chrome
 * draws already reads `m_palette`, so a swap that also re-derives the overlay
 * tuning below is total.
 *
 * The world-space marks are the half that would otherwise be missed. The
 * selection ring is the **own-fleet phosphor**, matching the roster's selected
 * chip -- allied cyan is reserved for allied assets and shield fills, and a
 * player reading colour fast would parse a cyan ring as someone else's ship.
 * The ghost states keep their meaning through alpha and the hostile red, but
 * the hues are the palette's, so a colour-vision swap recolours the world
 * overlays with the chrome.
 */
void ClientApp::ApplyPalette(std::string_view _name)
{
  m_palette = ResolveHudPalette(_name);

  m_overlayTuning.ringColourRgba = m_palette.phosphor;
  m_overlayTuning.hullColourRgba = m_palette.phosphor;
  m_overlayTuning.hullWornColourRgba = m_palette.caution;
  m_overlayTuning.hullLowColourRgba = m_palette.hostile;
  m_overlayTuning.shieldColourRgba = m_palette.allied;
  m_overlayTuning.ghostPendingColourRgba = WithAlpha(m_palette.phosphor, 0xa0);
  m_overlayTuning.ghostUnderWayColourRgba = m_palette.phosphor;
  m_overlayTuning.ghostRejectedColourRgba = m_palette.hostile;

  // The caution amber, this palette's word for a temporary condition -- which is
  // what a status bit is, whatever this game means by one.
  m_overlayTuning.statusMarkColourRgba = m_palette.caution;

  // Something of the player's arriving or leaving: the allied cyan, the same hue
  // the shield fill uses, because both are statements about your own.
  m_overlayTuning.transitRingColourRgba = m_palette.allied;

  // And the audit, re-measured with it. Cheap -- ten pairs of arithmetic -- and
  // it means the panel is never showing the ratios of the palette before last.
  m_auditRowCount = AuditPalette(m_palette, m_auditRows);
}

void ClientApp::OnSurfaceChanged(const SurfaceChange& _change)
{
  if (!_change.changed)
  {
    // Pressing the button for the screen you are already looking at, or ◀ BACK
    // at the base. Running entry work anyway would reconcile a composer against
    // a roster on every press of a control that did nothing (ADR-017 §6a.2).
    return;
  }

  /*
   * Exit first, then entry, and neither destroys retained state (ADR-020 §1).
   *
   * Exit clears the leaving surface's focus and cancels an in-flight drag: a
   * box half-drawn across the tactical view must not finish itself against the
   * hangar, and a rename box on a screen nobody is looking at must not still
   * own the keyboard.
   *
   * Entry is where staleness dies -- a surface validates what it kept against
   * the data it holds *now*, because it could not know, as it left, which of
   * the things it named would still exist when it returned. Nothing retains
   * anything yet; the hangar's composer is the first that will.
   */
  m_focus.ClearIfOn(_change.exited);
  // The gesture rather than a selection drag, since I2: the contact in flight
  // belongs to the surface that is leaving, and a long-press whose dwell
  // completed against the screen behind it is an order nobody gave.
  m_gestures.Cancel();
  m_puck.Cancel();

  if (_change.entered == SurfaceId::Station)
  {
    /*
     * The scroll resets and the composer does not, and the pair is ADR-017
     * §6a.2 in two lines.
     *
     * A different station's hangar is not the same list scrolled, so an offset
     * carried across would open a screen forty rows down a column that has
     * four. The *selection* is the opposite case: re-picking thirty ships
     * after one glance at the map is the cost the section weighed, and
     * `Reconcile` -- which `UpdateStationSurface` runs on every look -- is
     * what makes keeping it safe rather than stale.
     */
    m_stationScroll.Reset();
  }

  NEURON_LOG_INFO("surface: %u -> %u", static_cast<unsigned>(_change.exited), static_cast<unsigned>(_change.entered));
}

/*
 * The station surface's frame (`station-screen.png` §1-§2, ADR-020 §5.1, §5.4).
 *
 * The whole screen in one function, in the order a frame has to happen: ask,
 * reconcile, lay out, judge, then take the gesture. Every rect a press is
 * tested against below is a rect this function put in a member, and the draw
 * takes the same members -- which is §5.1's rule and the reason a hit test on
 * this screen can be tested at all.
 *
 * It owns the whole pointer while it runs. There is no world underneath to
 * fall through to, and a press that reached the tactical command row would be
 * a button nobody could see being pressed.
 */
void ClientApp::UpdateStationSurface()
{
  const float scale = m_uiLayout.scale;
  m_stationLayout =
      ResolveStationScreen(m_input.viewportWidth, m_input.viewportHeight, scale, m_stationTuning);

  /*
   * What the game says about this place.
   *
   * Asked every frame the screen is up rather than once on entry, and the
   * difference is the point: the roster arrives at about 1 Hz and a fleet can
   * leave from somewhere else, so a hangar that answered once would keep
   * drawing ships that are no longer in it.
   */
  m_stationTabCount = m_worldView->BuildStationTabs(m_stationAnchor, m_stationTabs);
  m_stationRoster =
      m_worldView->BuildStationRoster(m_stationAnchor, m_composer.Ids(), m_stationGroups, m_stationChips);

  /*
   * ADR-017 §6a.2's clause, and what makes a composer that outlives navigation
   * safe: the ids the roster no longer holds are dropped.
   *
   * Skipped when the roster did not fit, because a truncated list is not a
   * statement that the missing ships have gone -- reconciling against one
   * would silently shrink a selection the player built. Two ways it can not
   * fit and both are checked: more chips than the span holds, which shows up
   * as counted-but-not-emitted, and more wings than there are columns, which
   * shows up as nothing at all -- a ship in an unrepresented wing is skipped
   * before it is counted, so only the group cap itself gives it away.
   */
  std::uint32_t held = 0;
  for (std::uint32_t group = 0; group < m_stationRoster.groups; ++group)
  {
    held += m_stationGroups[group].chipCount;
  }

  if (held == m_stationRoster.chips && m_stationRoster.groups < MAX_STATION_GROUPS)
  {
    std::uint32_t available[MAX_STATION_CHIPS] = {};
    for (std::uint32_t index = 0; index < m_stationRoster.chips; ++index)
    {
      available[index] = m_stationChips[index].shipId;
    }

    const std::size_t before = m_composer.Count();
    m_composer.Reconcile(std::span<const std::uint32_t>{available, m_stationRoster.chips});
    if (m_composer.Count() != before)
    {
      // Asked again rather than left as it is: the counts the game just filled
      // were counted against a selection that has since lost ships, and a
      // column reading `. 3` over two highlighted chips is a wrong number.
      m_stationRoster =
          m_worldView->BuildStationRoster(m_stationAnchor, m_composer.Ids(), m_stationGroups, m_stationChips);
    }
  }

  /*
   * The composer's actions and the values their parameters may take -- the
   * words and the verbs all the game's (ADR-020 §6).
   *
   * Asked per action rather than for the first one, which is what let the
   * secondary verb exist at all: `MAX_STATION_ACTIONS` has been four since T3
   * and this loop read `[0]`, so a game offering two actions had its second one
   * silently dropped between the ask and the draw.
   *
   * **The index is clamped, not reset**, and the difference shows every frame:
   * the list is re-asked at frame rate, and resetting to zero on any change
   * would snap a player's chosen wing back to the first one the moment somebody
   * else's ship docked and added a column.
   */
  m_stationActionCount = m_worldView->BuildStationActions(m_stationAnchor, m_stationActions);
  for (std::uint32_t action = 0; action < MAX_STATION_ACTIONS; ++action)
  {
    m_stationOptionCount[action] =
      action < m_stationActionCount ? m_worldView->StationActionOptions(m_stationActions[action].verb, m_stationOptions[action]) : 0;
    if (m_stationOptionIndex[action] >= m_stationOptionCount[action])
    {
      m_stationOptionIndex[action] = 0;
    }
  }

  /*
   * What the columns scroll over, and how much of it shows.
   *
   * Both from `StationScreen`, and both from the same arithmetic the layout
   * uses -- a visible count derived separately would let the scroll believe in
   * a row the layout had already drawn.
   */
  m_stationScroll.SetExtent(
      StationColumnRows(std::span<const StationChip>{m_stationChips, m_stationRoster.chips}, m_stationRoster.groups),
      StationVisibleRows(m_stationLayout.roster, scale, m_stationTuning));

  m_stationTabButtonCount =
      BuildStationTabRow(std::span<const StationTab>{m_stationTabs, m_stationTabCount}, m_stationLayout.tabRow, scale,
                         m_stationTuning, m_stationTabButtons);
  m_stationLaid = BuildStationColumns(std::span<const StationGroup>{m_stationGroups, m_stationRoster.groups},
                                      std::span<const StationChip>{m_stationChips, m_stationRoster.chips},
                                      m_stationLayout.roster, m_stationScroll.FirstVisibleRow(), scale, m_stationTuning,
                                      m_stationColumns, m_stationChipRects);

  /*
   * Whether UNDOCK is a promise, from the authority's own validator.
   *
   * `PreCheckStation` runs `ValidateStationCommand` over the same `RosterView`
   * the server judges with, so the button greys for the reason the bounce would
   * have carried and in the same words. That is the whole of ADR-014 §3's
   * parity claim on this screen, and the print states it as a rule: "a tappable
   * UNDOCK is a promise".
   *
   * Judged on the *first wave* rather than on the whole selection, because the
   * first wave is what a press sends. Sixty-six ships is two commands and the
   * screen says so before the press; asking the validator about all sixty-six
   * would refuse a gesture that is going to work.
   */
  const std::uint32_t cap = m_worldView->ShipsPerStationCommand();
  const std::uint32_t waves = m_composer.WaveCount(cap);
  for (std::uint32_t action = 0; action < MAX_STATION_ACTIONS; ++action)
  {
    m_stationWaves[action] = action < m_stationActionCount ? waves : 0;
    m_stationVerdict[action] = OrderVerdict{};
    if (m_stationWaves[action] == 0)
    {
      continue;
    }

    const std::span<const std::uint32_t> wave = m_composer.Wave(0, cap);
    StationIntent probe;
    probe.verb = m_stationActions[action].verb;
    probe.parameter = m_stationOptionCount[action] > 0 ? m_stationOptions[action][m_stationOptionIndex[action]].parameter : 0;
    probe.anchor = m_stationAnchor;
    probe.shipIds = wave.data();
    probe.shipCount = static_cast<std::uint32_t>(wave.size());
    m_stationVerdict[action] = m_worldView->PreCheckStation(probe);
  }

  // --- and now the gesture, in ADR-020 §2's order -------------------------
  //
  // The wheel first, and by position rather than by press: a list under the
  // cursor claims the notch before anything else sees it, and hovering is the
  // whole gesture (§4).
  if (const float notches = m_router.ClaimWheelIn(m_stationLayout.roster); notches != 0.0f)
  {
    m_stationScroll.ScrollByWheel(notches);
  }

  // The way back, before anything else can take the press.
  if (m_router.ClaimPointerIn(m_backChipRect))
  {
    OnSurfaceChanged(m_surfaces.Back());
    return;
  }

  const float cursorX = m_router.CursorX();
  const float cursorY = m_router.CursorY();

  if (m_router.Pressed(InputButton::Left))
  {
    if (const StationTabButton* tab = HitStationTab(
            std::span<const StationTabButton>{m_stationTabButtons, m_stationTabButtonCount}, cursorX, cursorY);
        tab != nullptr)
    {
      // Only the hangar is live today and this client cannot tell which one it
      // is -- the tab hands back the game's number and the screen echoes it.
      // A disabled tab never reaches here; the hit test refused it.
      m_stationTab = tab->id;
    }
    else if (const StationColumnRect* column = HitStationColumnHeader(
                 std::span<const StationColumnRect>{m_stationColumns, m_stationLaid.groups}, cursorX, cursorY);
             column != nullptr)
    {
      /*
       * The print's second gesture: taking a whole wing.
       *
       * **A press rather than a hold**, which is a departure from the print
       * and worth naming. A hold needs a dwell timer this frame loop does not
       * otherwise keep, and it earns its cost on a *chip*, where holding has
       * to be told apart from tapping. A header has no competing gesture: it
       * is not a chip, and nothing else on it does anything.
       *
       * Additive, which is the part that matters: taking a wing extends the
       * composition rather than replacing it, so "Talon plus two spare
       * Battleships from Reserve" stays three gestures.
       */
      std::uint32_t wing[MAX_STATION_CHIPS] = {};
      std::uint32_t count = 0;
      for (std::uint32_t index = 0; index < m_stationRoster.chips; ++index)
      {
        if (m_stationChips[index].group == column->group)
        {
          wing[count] = m_stationChips[index].shipId;
          ++count;
        }
      }
      m_composer.AddAll(std::span<const std::uint32_t>{wing, count});
    }
    else if (const StationChipRect* chip = HitStationChip(
                 std::span<const StationChipRect>{m_stationChipRects, m_stationLaid.chips}, cursorX, cursorY);
             chip != nullptr && chip->chip < m_stationRoster.chips)
    {
      // The first gesture: tap toggles.
      (void)m_composer.Toggle(m_stationChips[chip->chip].shipId);
    }
    else if (m_stationOptionCount[0] > 0 && m_stationLayout.formation.Contains(cursorX, cursorY))
    {
      // Cycled rather than dropped down, which is the command row's idiom for
      // the same shape of choice -- three values with words for them, and a
      // list that opened would be a second surface over a full-screen one.
      m_stationOptionIndex[0] = (m_stationOptionIndex[0] + 1) % m_stationOptionCount[0];
    }
    else if (m_stationVerdict[0].accepted && m_stationLayout.undock.Contains(cursorX, cursorY))
    {
      CommitStationAction(0, Clock::SecondsSinceStart());
    }
    /*
     * And the secondary pair, which is the same two gestures over the same two
     * shapes of control -- a chip that cycles and a button that sends.
     *
     * The screen binds the game's actions to its control pairs by position:
     * action 0 gets the print's primary controls, action 1 gets the ones under
     * the diagram. That is a layout fact rather than a fact about the verbs, and
     * it is why this side can offer a wing assignment without knowing that is
     * what it is offering.
     *
     * **The wing chip cycles even when the button is dead.** A player whose
     * selection is empty has nothing to assign and every reason to be reading
     * which wings exist; greying the chip with the button would hide the list at
     * exactly the moment it is being consulted.
     */
    else if (m_stationOptionCount[1] > 0 && m_stationLayout.assignWing.Contains(cursorX, cursorY))
    {
      m_stationOptionIndex[1] = (m_stationOptionIndex[1] + 1) % m_stationOptionCount[1];
    }
    else if (m_stationVerdict[1].accepted && m_stationLayout.assign.Contains(cursorX, cursorY))
    {
      CommitStationAction(1, Clock::SecondsSinceStart());
    }
  }

  // Whatever the press landed on is the surface's own air. Claimed after the
  // stages above rather than instead of them, and unconditionally, because a
  // full-screen surface owns every button of the pointer for as long as it is
  // up (ADR-020 §1).
  (void)m_router.ClaimPointer();
}

/*
 * One wave onto the wire.
 *
 * `CommitOrder`'s shape without the ghost: an order bounces in the world
 * because there is a world to bounce it in, and a docked ship is in no
 * snapshot -- so a refused station command is a toast and nothing else.
 *
 * The pre-check has already run this frame, against this wave, with this
 * parameter; what follows is the send. Refusing again here would be asking the
 * same function the same question twice in one frame.
 *
 * **Nothing below names a verb**, which is the point of taking an index. The
 * two things that differ between the game's actions -- what goes in the intent
 * and what happens to the composer afterwards -- both arrive in the
 * `StationAction`, so this function sends a wing assignment and an undock with
 * the same lines and cannot tell which it just sent.
 */
void ClientApp::CommitStationAction(std::uint32_t _action, double _nowSeconds)
{
  const std::uint32_t cap = m_worldView->ShipsPerStationCommand();
  const std::span<const std::uint32_t> picked = m_composer.Wave(0, cap);
  if (_action >= m_stationActionCount || picked.empty())
  {
    return;
  }

  // Copied out of the composer rather than referenced into it: the last thing
  // this function does is remove these ids, and a span into the vector it is
  // erasing from would be a span into a vector that has moved.
  std::uint32_t wave[MAX_STATION_CHIPS] = {};
  const std::uint32_t waveCount = static_cast<std::uint32_t>(std::min(picked.size(), std::size(wave)));
  for (std::uint32_t index = 0; index < waveCount; ++index)
  {
    wave[index] = picked[index];
  }

  StationIntent intent;
  intent.verb = m_stationActions[_action].verb;
  intent.parameter = m_stationOptionCount[_action] > 0 ? m_stationOptions[_action][m_stationOptionIndex[_action]].parameter : 0;
  intent.anchor = m_stationAnchor;
  intent.orderSeq = m_nextOrderSeq++;
  intent.shipIds = wave;
  intent.shipCount = waveCount;

  // The game writes its own bytes and the connection frames them; neither
  // looks at the other's half (ADR-004 ruling 4). Station commands ride the
  // order stream, which is why this is `SendOrder` (ADR-017 §8).
  std::array<std::uint8_t, MAX_DATAGRAM_BYTES> payload{};
  ByteWriter writer{payload};
  if (!m_worldView->EncodeStationCommand(intent, writer) || !writer.Ok() ||
      !m_connection.SendOrder(writer.Written()))
  {
    /*
     * The game's word for the action, in the failure the player reads.
     *
     * A fixed "UNDOCK NOT SENT" was correct while there was one action and is a
     * lie about the other, and this side has no business writing either word --
     * so the title is composed from `StationAction::name`, which is the same
     * string the button that was just pressed is drawn with.
     */
    char title[64] = {};
    const char* verb = m_stationActions[_action].name;
    std::snprintf(title, sizeof(title), "%s NOT SENT", verb != nullptr ? verb : "COMMAND");
    (void)m_toasts.Raise(ToastPriority::Urgent, 0, title, "the link did not take it", _nowSeconds);
    NEURON_LOG_WARNING("station command %u could not be sent", intent.orderSeq);
    return;
  }

  /*
   * The ships this action consumed stop being pickable, and the composer drops
   * them -- if it consumed any.
   *
   * Cleared by the action that consumed them rather than by navigation --
   * §6a.2's other half. Only this wave: a selection of sixty-six is two
   * presses, and clearing all of it on the first would lose the second.
   *
   * **Whether it consumed them is the game's answer**, not a guess from the
   * verb this side cannot read. An undock sends the ships away and the ids must
   * go with them; a regrouping leaves them docked, and dropping the selection
   * would charge the player a fresh thirty taps for the next thing they wanted
   * to do to the same thirty ships.
   */
  if (m_stationActions[_action].consumesSelection)
  {
    for (std::uint32_t index = 0; index < waveCount; ++index)
    {
      m_composer.Remove(wave[index]);
    }
  }

  NEURON_LOG_INFO("station command %u sent: %u ship(s)", intent.orderSeq, intent.shipCount);
}

void ClientApp::OnViewChanged(std::uint16_t _gridAnchor, double _nowSeconds)
{
  /*
   * Everything below is keyed on an id or on the last frame, and a switch
   * invalidates both. They are cleared for three different reasons, which is
   * why this is a list rather than one call:
   *
   *  - the **transits** would read a whole grid's ids changing as a whole
   *    fleet leaving and another arriving, and draw forty rings saying so;
   *  - the **ghosts** promise things about ships on a grid this client is no
   *    longer watching, and a promise nobody can see kept is one that hangs;
   *  - the **approach chain** is mid-way through a two-step order whose second
   *    step names ships that are about to stop being on screen.
   *
   * The selection needs no line here: `Retain` drops ids the scene no longer
   * holds, and it already runs every frame for exactly this class of reason.
   */
  m_transits.Clear();
  m_ghosts.Clear();
  m_approach.Cancel();

  m_settleUntilSeconds = _nowSeconds + VIEW_SETTLE_SECONDS;
  NEURON_LOG_INFO("view changed to grid %u; settling for %.0f ms", _gridAnchor, VIEW_SETTLE_SECONDS * 1000.0);
}

void ClientApp::SendViewFocus()
{
  /*
   * Tells the server where this viewer is looking and what they have selected
   * (ADR-022 §4).
   *
   * **ADR-016 §7 said the server had no business holding this**, and ADR-022 §1
   * is what changed it: relevance is a property of a viewer, so the ranking
   * needs a focus, an extent and a selection. Without them the server ranks
   * against a camera at the origin showing nothing, which puts every ship the
   * player does not own into the far tier -- correct, and useless.
   *
   * **On change, not every frame.** At 144 Hz an unconditional send would be
   * seven messages per tick of ranking that could use them, and the server
   * cannot act on more than one a tick. The comparison is on the *quantised*
   * values, so a camera drifting by less than a centimetre is not a change --
   * which is the same rounding boundary the wire uses, so the client cannot
   * hold an opinion the server never sees.
   *
   * Unreliable, so a lost one costs a single tick ranked against a slightly
   * stale camera: a worse ordering, never a wrong one.
   */
  if (m_connection.State() != ClientLinkState::Joined)
  {
    return;
  }

  ViewFocus focus;
  focus.gridId = m_connection.GridAnchor();
  focus.focusXCm = MetresToCentimetres(m_camera.Focus().x);
  focus.focusYCm = MetresToCentimetres(m_camera.Focus().y);
  focus.halfExtentCm = static_cast<std::uint32_t>(std::max(0.0f, m_camera.ZoomMetres()) * 100.0f);

  const std::span<const EntityId> selected = m_selection.Ids();
  const std::size_t count = std::min<std::size_t>(selected.size(), MAX_VIEW_SELECTION);
  focus.selection.assign(selected.begin(), selected.begin() + static_cast<std::ptrdiff_t>(count));

  const bool changed = focus.gridId != m_sentFocus.gridId || focus.focusXCm != m_sentFocus.focusXCm ||
                       focus.focusYCm != m_sentFocus.focusYCm || focus.halfExtentCm != m_sentFocus.halfExtentCm ||
                       focus.selection != m_sentFocus.selection;
  if (!changed)
  {
    return;
  }

  if (m_connection.SendViewFocus(focus))
  {
    m_sentFocus = std::move(focus);
  }
}

void ClientApp::AdvanceApproach(double _nowSeconds)
{
  if (!m_approach.Active())
  {
    return;
  }

  // A member that despawned is one the chained order can no longer name, and
  // the authority would refuse the whole thing for it.
  m_liveIds.clear();
  for (const SceneEntity& entity : m_scene.entities)
  {
    m_liveIds.push_back(entity.id);
  }
  m_approach.NoteWorld(m_liveIds);
  if (!m_approach.Active())
  {
    NEURON_LOG_INFO("approach cancelled: a member left the world");
    return;
  }

  /*
   * The readiness test, and it is not a distance.
   *
   * The order is composed exactly as it will be sent and handed to the game's
   * own pre-check. While that refuses, the fleet is still on its way; the frame
   * it accepts, the verb goes. There is no second definition of "close enough"
   * on this side to drift from the authority's, which is ADR-014 3's parity
   * rule earning its keep twice.
   */
  const std::span<const EntityId> ships = m_approach.Ships();
  OrderIntent intent;
  intent.kind = m_approach.Kind();
  intent.anchor = m_approach.Anchor();
  intent.entityIds = ships.data();
  intent.entityCount = static_cast<std::uint32_t>(ships.size());
  intent.orderSeq = m_nextOrderSeq;

  if (!m_worldView->PreCheck(intent).accepted)
  {
    return; // Still flying.
  }

  ++m_nextOrderSeq;
  std::array<std::uint8_t, MAX_DATAGRAM_BYTES> payload{};
  ByteWriter writer{payload};
  if (!m_worldView->EncodeOrder(intent, writer) || !writer.Ok() || !m_connection.SendOrder(writer.Written()))
  {
    NEURON_LOG_WARNING("the chained order could not be sent");
    m_approach.Cancel();
    return;
  }

  NEURON_LOG_INFO("chained order %u sent: the fleet is in range", intent.orderSeq);
  m_approach.Cancel();
  (void)_nowSeconds;
}

void ClientApp::CommitOrder(const PuckSample& _sample, double _nowSeconds)
{
  const std::uint32_t orderSeq = m_nextOrderSeq++;

  // The game's kind, and whichever of the game's parameters that kind has
  // chosen. Both are numbers this client copies and never reads. The kind is
  // the command row's now rather than the default's -- the default is only
  // where it started.
  OrderDefaults chosen = m_orderDefaults;
  chosen.kind = m_selectedKind;
  if (const std::uint32_t slot = KindSlot(m_selectedKind);
      slot < m_orderKindCount && m_kindOptionIndex[slot] < m_kindOptionCounts[slot])
  {
    chosen.parameter = m_kindOptions[slot][m_kindOptionIndex[slot]].parameter;
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

  /*
   * Then the two screen-facing families, which append and move no index.
   *
   * The transits are noted here rather than in `UpdateOrders` because this is
   * where the scene is: what left the world is exactly the difference between
   * the list `BuildScene` just filled and the one it filled last frame, and any
   * other place to ask would be asking about a scene a frame out of date.
   */
  const double nowSeconds = Clock::SecondsSinceStart();

  /*
   * While the feed is settling, the transit list is re-baselined every frame
   * rather than asked what changed.
   *
   * Clearing repeatedly rather than once at the switch is what makes this
   * robust to the race the window exists for: whichever frame the new grid's
   * first snapshot lands on, the frame after the window is the one that becomes
   * the baseline, and no frame inside it can raise an event. A single `Clear()`
   * at the notice would still leave one frame exposed if a snapshot arrived
   * first.
   */
  if (m_settleUntilSeconds >= 0.0 && nowSeconds < m_settleUntilSeconds)
  {
    m_transits.Clear();
  }
  else
  {
    m_settleUntilSeconds = -1.0;
    m_transits.Note(m_scene.entities, nowSeconds);
  }
  BuildStatusMarks(m_scene.entities, m_overlayTuning, nowSeconds, m_overlayMarks);
  BuildTransitMarks(m_transits.Transits(), m_overlayTuning, m_camera.MetresPerPixel(), nowSeconds, m_overlayMarks);
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

  // The key: one hard directional from high on one side, fixed in world space.
  // Fixed, and not attached to the camera, because that is what makes orbiting
  // worth doing -- a hull's lit side is a property of the hull's facing, so
  // turning the view shows a ship from its dark side and says something.
  XMFLOAT3 key;
  XMStoreFloat3(&key, XMVector3Normalize(XMLoadFloat3(&KEY_DIRECTION)));
  constants.sunDirection = XMFLOAT4{key.x, key.y, key.z, 0.0f};
  constants.sunColour = XMFLOAT4{KEY_COLOUR.x, KEY_COLOUR.y, KEY_COLOUR.z, KEY_INTENSITY};

  // The fill, as a fraction of the key. Folded in here rather than in the
  // shader so the pixel cost stays two constants and a lerp, and so the numbers
  // the constants declare are the numbers the rig was tuned in.
  const float fill = KEY_INTENSITY * FILL_FRACTION_OF_KEY;
  constants.ambientSky = XMFLOAT4{FILL_SKY_TINT.x * fill, FILL_SKY_TINT.y * fill, FILL_SKY_TINT.z * fill, 1.0f};
  constants.ambientGround = XMFLOAT4{FILL_GROUND_TINT.x * fill, FILL_GROUND_TINT.y * fill, FILL_GROUND_TINT.z * fill, 1.0f};

  /*
   * The bounce, arriving from where the camera is.
   *
   * `ScreenUpOnPlane` is the view direction flattened onto the plane and
   * pointing away from the camera, which is exactly the horizontal direction a
   * light shining from behind the viewer travels; the vertical part is the
   * camera's own elevation, so the term is "what the viewer's side of the
   * scene is lit by" rather than a fourth arbitrary angle.
   *
   * Sim space is (east, north) and render space is (east, up, north), so the
   * plane vector's y becomes the render z.
   */
  const XMFLOAT2 awayFromCamera = m_camera.ScreenUpOnPlane();
  const float elevation = IsoCamera::ELEVATION_DEGREES * XM_PI / 180.0f;
  const float horizontal = std::cos(elevation);
  XMFLOAT3 bounce;
  XMStoreFloat3(&bounce,
                XMVector3Normalize(XMVectorSet(awayFromCamera.x * horizontal, -std::sin(elevation), awayFromCamera.y * horizontal, 0.0f)));
  constants.bounceDirection = XMFLOAT4{bounce.x, bounce.y, bounce.z, 0.0f};
  constants.bounceColour = XMFLOAT4{BOUNCE_TINT.x, BOUNCE_TINT.y, BOUNCE_TINT.z, KEY_INTENSITY * BOUNCE_FRACTION_OF_KEY};

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

  /*
   * The clock the signal lamps blink on (ADR-006 §6a).
   *
   * `Clock::SecondsSinceStart` and deliberately not the tick: the lamps are
   * cosmetic, so two clients watching the same fleet should see it blinking
   * differently rather than in lockstep, and a synchronised one would be a
   * per-frame float on the wire for no gameplay at all.
   *
   * Narrowed to float here, once. It is a double because the frame loop times
   * itself with it, and after a few hours of uptime a float's ulp is bigger
   * than a strobe's on-window -- so the narrowing is wrapped to the hour rather
   * than taken raw. An hour is over two thousand cycles of the slowest lamp, so
   * the wrap lands mid-blink and nothing about it is visible.
   */
  constexpr double LAMP_CLOCK_WRAP_SECONDS = 3600.0;
  const double seconds = Clock::SecondsSinceStart();
  constants.frameTime = XMFLOAT4{static_cast<float>(std::fmod(seconds, LAMP_CLOCK_WRAP_SECONDS)), 0.0f, 0.0f, 0.0f};
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

  char buffer[96] = {};
  char upper[48] = {};

  /*
   * --- the surface ----------------------------------------------------------
   *
   * One or the other, never both: screens are mutually exclusive and the debug
   * strip is the only layer allowed to compose over one (ADR-020 1). A
   * full-screen surface is opaque across the viewport, so building the tactical
   * chrome underneath it was a fill nobody ever saw -- T3a named that as owed
   * and this is the early return it asked for, now that the tactical half is a
   * function of its own.
   *
   * The toast layer below is deliberately outside the branch: toasts are a
   * cross-surface layer and are drawn above whichever of the two ran.
   */
  if (SurfaceRecordsWorld(m_surfaces.Active()))
  {
    BuildTacticalHud(nowSeconds);
  }
  else if (m_surfaces.Active() == SurfaceId::Settings)
  {
    BuildSettingsSurface();
  }
  else
  {
    BuildStationSurface();
  }

  // --- the toast stack ----------------------------------------------------
  //
  // Criticals lead the visible list and go centre-top on their own surface;
  // the rest stack bottom-right, clear of the context bar (§2).
  const std::span<const Toast> toasts = m_toasts.Visible();
  const std::size_t criticals = m_toasts.CriticalCount();
  m_toastActionCount = 0;
  for (UiRect& rect : m_toastActionRects)
  {
    rect = UiRect{};
  }

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

    /*
     * The action chip, on the row's right (`alerts-and-toasts.png` 2).
     *
     * A critical is the one level that waits for the player, so it is the one
     * level with somewhere for the player to press. The word is the game's --
     * this file draws it and never compares it -- and the key it hands back is
     * the game's too.
     *
     * **48 px square at 1.0x**, the same U2 touch floor the command verbs clamp
     * to. An action nobody can hit on a tablet is an action that does not
     * exist, and a critical is exactly the row where that matters.
     *
     * An acted chip stays drawn and goes dead. Removing it would take the
     * confirmation away at the instant the player looked for it; leaving it
     * live would invite a second press that does nothing.
     */
    if (toast.HasAction())
    {
      const float chipHeightPx = std::max(m_commandTuning.buttonHeight * layout.scale, 48.0f * layout.scale);
      UpperCaseInto(toast.actionLabel, upper);
      const float labelWidth = static_cast<float>(TextCellCount(upper)) * cell;
      const float chipWidth = labelWidth + 2.0f * pad;
      const UiRect chip{rect.Right() - pad - chipWidth, rect.y + (rect.height - chipHeightPx) * 0.5f, chipWidth,
                        chipHeightPx};

      const bool live = !toast.Acted();
      m_ui.AddBorder(chip, 1.0f * layout.scale, live ? accent : AtHalfAlpha(m_palette.border));
      m_ui.AddText(chip.x + (chip.width - labelWidth) * 0.5f, chip.y + (chip.height - bodyPx) * 0.5f,
                   m_uiTuning.bodySizeIndex, live ? accent : m_palette.phosphorDead, upper);
      m_toastActionRects[index] = chip;
      m_toastActionCount = index + 1;
    }
  }

  /*
   * --- the menu list ------------------------------------------------------
   *
   * The `▥ MENU` chip's stub: RESUME · SETTINGS · EXIT, from the same rects
   * `UpdateHud` hit-tests. Drawn after everything else so the list covers
   * whatever it floats over -- build order is draw order in this pass.
   * SETTINGS is dead until 07h's sheet lands; a menu entry that exists and is
   * visibly not ready beats one that appears later in a spot the player never
   * learned.
   */
  if (m_menuOpen)
  {
    const UiRect menuPanel = UiRect::FromCorners(m_menuItemRects[0].x - pad * 0.5f, m_menuItemRects[0].y - pad * 0.5f,
                                                 m_menuItemRects[MENU_ITEM_COUNT - 1].Right() + pad * 0.5f,
                                                 m_menuItemRects[MENU_ITEM_COUNT - 1].Bottom() + pad * 0.5f);
    // Opaque, unlike every other panel on this HUD. The 12% that reads through
    // a panel is what keeps the chrome a border on a *view*; this list floats
    // over the ability rack, and a menu you can read the panel behind is a menu
    // with two sets of words in the same pixels.
    m_ui.AddQuad(menuPanel, WithAlpha(m_palette.panel, 0xFF));
    m_ui.AddBorder(menuPanel, 1.0f * layout.scale, m_palette.borderStrong);

    const char* menuLabels[MENU_ITEM_COUNT] = {"RESUME", "SETTINGS", "EXIT"};
    for (std::uint32_t item = 0; item < MENU_ITEM_COUNT; ++item)
    {
      // Nothing in this list is drawn dead since N3. SETTINGS was the one entry
      // that was, and it opens a screen now -- the half-alpha border and the
      // dead phosphor went with it rather than staying as a branch that could
      // never be taken.
      const UiRect& itemRect = m_menuItemRects[item];
      m_ui.AddBorder(itemRect, 1.0f * layout.scale, m_palette.border);
      const float itemWidth = static_cast<float>(TextCellCount(menuLabels[item])) * cell;
      m_ui.AddText(itemRect.x + (itemRect.width - itemWidth) * 0.5f, itemRect.y + (itemRect.height - bodyPx) * 0.5f,
                   m_uiTuning.bodySizeIndex, m_palette.phosphor, menuLabels[item]);
    }
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

void ClientApp::BuildTacticalHud(double _nowSeconds)
{
  /*
   * The tactical HUD, built only when the tactical surface is live.
   *
   * A function of its own because it has to be skippable, which is what T3a
   * left owed: the chrome below used to be built underneath a full-screen
   * surface and then covered by it, a `UiDrawList` fill of a screen nobody was
   * looking at. `BuildHud` chooses between this and the surface now, so the
   * cost of a screen is the screen.
   *
   * The zone metrics are re-derived rather than passed in: every one of them is
   * a function of `m_uiLayout` and `m_uiTuning`, which `UpdateHud` resolved
   * before this frame drew anything, and a parameter list of five numbers that
   * are all already members is a second place for them to be computed
   * differently. The clock is the exception and is the one parameter.
   */
  const UiLayout& layout = m_uiLayout;
  const float pad = m_uiTuning.padding * layout.scale;
  const float cell = 8.0f * layout.scale;
  const float bodyPx = BASE_FONT_SIZES_PIXELS[m_uiTuning.bodySizeIndex] * layout.scale;
  const float smallPx = BASE_FONT_SIZES_PIXELS[m_uiTuning.smallSizeIndex] * layout.scale;

  // The one thing that is *not* a member: reading the clock a second time would
  // animate the lanes off a different instant than the toasts advanced on.
  const double nowSeconds = _nowSeconds;

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
   * **The drag rectangle S8 deferred here is gone** (I2, Plan-of-Record §1's
   * rule 5).
   *
   * It was a screen-space quad drawn while a left-drag was resolving to a box.
   * Once drag is the camera there is no such gesture: box select lost its
   * binding rather than its argument, and two-finger drag is reserved for it.
   * `PickBox` survives in `Picking.h` for that day; what has no reason to
   * survive is a rectangle nothing can draw, so the quad went with the gesture
   * rather than being left behind as paint.
   */

  // The roster's rows and the location blocks were asked for in `UpdateHud`,
  // because the column's geometry depends on how many there are and the column
  // carries a live control now. The rows are the game's answer, not a grouping
  // this file performs: it has `EntityRecord::groupId` and could aggregate in
  // four lines, and doing so would be deciding that groups are named and how
  // their health combines (ADR-014 §2c).

  /*
   * Which verbs the row may offer *this* frame.
   *
   * Asked every frame rather than once at boot, and with the selection, because
   * a verb can be real and still not offerable now -- the game's answer for a
   * mining order depends on the field under the fleet and on what is in the
   * selection, neither of which is a boot-time fact. What comes back is a name,
   * a flag and a reason code; this file never learns what makes a verb
   * available, only that the game said so (ADR-014 2b).
   */
  m_orderKindCount = m_worldView->OrderKinds(m_selection.Ids(), m_orderKinds);

  /*
   * --- the top status row -------------------------------------------------
   *
   * Everything on it is a replicated field, a link statistic or local UI
   * state, which is the whole acceptance criterion: kill the feed and these
   * go stale -- drawn in `neutral` at reduced alpha -- rather than holding
   * their last value or inventing one. The prime slot is the player's
   * *location*, not the product: the session strings arrive in `Welcome` and
   * are drawn verbatim, because what a world is called is the game's to say.
   */
  m_ui.AddQuad(layout.topBar, m_palette.panel);
  m_ui.AddQuad(UiRect{0.0f, layout.topBar.Bottom() - 1.0f, layout.topBar.width, 1.0f}, m_palette.borderStrong);

  const bool joined = m_connection.State() == ClientLinkState::Joined;
  const float textY = layout.topBar.y + (layout.topBar.height - bodyPx) * 0.5f;
  const float chipY = layout.topBar.y + (layout.topBar.height - smallPx) * 0.5f;
  const std::uint32_t staleColour = AtHalfAlpha(m_palette.neutral);

  char buffer[96] = {};
  char upper[48] = {};

  // Left to right: `◈ VESTA-3 ▸ FRONTIER 0.4` -- the system hot, the region
  // and version-of-space a step back. The glyphs are spelled as UTF-8 bytes
  // for the same reason the atlas's bake table is spelled as codepoints: these
  // files carry no byte-order mark. U+25C8 marks the location, U+25B8 is the
  // token separator.
  float pen = pad;
  if (joined && !m_connection.WorldName().empty())
  {
    UpperCaseInto(m_connection.WorldName().c_str(), upper);
    std::snprintf(buffer, sizeof(buffer), "\xE2\x97\x88 %s", upper);
    m_ui.AddText(pen, textY, m_uiTuning.bodySizeIndex, m_palette.phosphorHot, buffer);
    pen += static_cast<float>(TextCellCount(buffer)) * cell + cell;

    m_ui.AddText(pen, textY, m_uiTuning.bodySizeIndex, m_palette.phosphorGhost, "\xE2\x96\xB8");
    pen += 2.0f * cell;

    UpperCaseInto(m_connection.WorldDetail().c_str(), upper);
    m_ui.AddText(pen, textY, m_uiTuning.bodySizeIndex, m_palette.phosphorDim, upper);
    pen += static_cast<float>(TextCellCount(upper)) * cell + cell;
  }
  else
  {
    m_ui.AddText(pen, textY, m_uiTuning.bodySizeIndex, staleColour, "NO SESSION");
    pen += static_cast<float>(TextCellCount("NO SESSION")) * cell + cell;
  }

  /*
   * The alerts chip, still left-cluster: caution amber, U+26A0, and only when
   * undismissed alerts exist -- an alert counter that read `0 ALERTS` all
   * session would train the eye to skip the slot that matters.
   *
   * **It counts what is queued as well as what is shown.** A fight holds
   * everything but Critical and Urgent back (`ToastStack::SetCombat`), so a
   * count of the visible rows alone would *fall* exactly when the most is
   * happening -- the number the eye checks would go quiet during the only event
   * that fills the backlog. Visible plus suppressed is the honest total, and it
   * is what makes the sheet's SUPPRESSED row a thing the player can already see
   * coming.
   *
   * **And it pulses**, because a static count is a readout and this is a
   * summons. The alpha swings between full and half on a 1.8 s period taken
   * from the frame clock -- no timer, no state, nothing to reset: the phase is
   * a function of the time, so two clients showing the same alert pulse in step
   * and a paused one does not drift.
   */
  const std::size_t alertCount = m_toasts.Visible().size() + m_toasts.SuppressedCount();
  if (alertCount > 0)
  {
    std::snprintf(buffer, sizeof(buffer), "\xE2\x9A\xA0 %zu ALERT%s", alertCount, alertCount == 1 ? "" : "S");
    pen += cell;

    constexpr double ALERT_PULSE_SECONDS = 1.8;
    const auto phase = static_cast<float>(nowSeconds / ALERT_PULSE_SECONDS);
    const float swing = 0.5f + 0.5f * std::sin(phase * 6.2831853f);
    const auto full = static_cast<float>(m_palette.caution >> 24);
    const auto half = static_cast<float>(AtHalfAlpha(m_palette.caution) >> 24);
    const std::uint32_t pulsed =
        WithAlpha(m_palette.caution, static_cast<std::uint8_t>(half + (full - half) * swing));
    m_ui.AddText(pen, chipY, m_uiTuning.smallSizeIndex, pulsed, buffer);
  }

  /*
   * Right to left: `⌖ 41 SHIPS | SEC 0.4 | NET ▂▄▆ | ▥ MENU`, the print's own
   * order. The menu chip is the anchor -- its rect was resolved in `UpdateHud`
   * because it takes a click -- and everything else pens leftward from it,
   * "|"-separated in the ghost step.
   */
  m_ui.AddBorder(m_menuButtonRect, 1.0f * layout.scale, m_palette.border);
  {
    const float menuTextWidth = static_cast<float>(TextCellCount(MENU_CHIP_LABEL)) * cell;
    m_ui.AddText(m_menuButtonRect.x + (m_menuButtonRect.width - menuTextWidth) * 0.5f, textY,
                 m_uiTuning.bodySizeIndex, m_menuOpen ? m_palette.phosphorHot : m_palette.phosphor, MENU_CHIP_LABEL);
  }

  float right = m_menuButtonRect.x - cell;
  // Right-to-left, so a separator claims its own cell and a cell of air on
  // each side before the next item pens further left.
  const auto rightSeparator = [&] {
    right -= 2.0f * cell;
    m_ui.AddText(right, textY, m_uiTuning.bodySizeIndex, m_palette.phosphorGhost, "|");
    right -= cell;
  };

  /*
   * NET as four signal bars bucketed from the round trip -- under 40 ms all
   * four, under 80 three, under 140 two, else one -- drawn as 3/5/7/9 px quads
   * sharing a baseline. The raw millisecond figure is debug telemetry and lives
   * on the strip's LINK row; the top bar answers "is the link good", not "how
   * good".
   *
   * Four rather than three because three buckets cannot say "good": the old
   * top bucket opened at 60 ms, which is a link a player would notice, so a
   * perfectly healthy loopback and a merely tolerable connection drew the same
   * meter. The extra bar buys a band that means nothing is wrong.
   */
  rightSeparator();
  {
    constexpr int NET_BARS = 4;
    const float barWidth = 3.0f * layout.scale;
    const float barGap = 2.0f * layout.scale;
    const float baseline = textY + bodyPx;
    const double rtt = m_connection.RoundTripMs();
    const int lit = !joined ? 0 : (rtt < 40.0 ? 4 : (rtt < 80.0 ? 3 : (rtt < 140.0 ? 2 : 1)));

    right -= static_cast<float>(NET_BARS) * barWidth + static_cast<float>(NET_BARS - 1) * barGap;
    for (int bar = 0; bar < NET_BARS; ++bar)
    {
      const float barHeight = (3.0f + 2.0f * static_cast<float>(bar)) * layout.scale;
      const float barX = right + static_cast<float>(bar) * (barWidth + barGap);
      m_ui.AddQuad(UiRect{barX, baseline - barHeight, barWidth, barHeight},
                   bar < lit ? m_palette.phosphor : m_palette.phosphorDead);
    }
    right -= (static_cast<float>(TextCellCount("NET")) + 1.0f) * cell;
    m_ui.AddText(right, textY, m_uiTuning.bodySizeIndex, joined ? m_palette.phosphorDim : staleColour, "NET");
  }

  // The feed-level STALE readout (S14): the whole world on screen is frozen
  // past the extrapolation cap (ADR-002 §4). The per-ship markers say which
  // ships; this says it is the *feed*, beside the link readout the eye checks.
  if (joined && m_snapshots.Stale(nowSeconds))
  {
    const char* staleText = "STALE";
    right -= (static_cast<float>(TextCellCount(staleText)) + 2.0f) * cell;
    m_ui.AddText(right, chipY, m_uiTuning.smallSizeIndex, m_palette.caution, staleText);
  }

  // The zone badge -- `SEC 0.4` -- verbatim from the session strings. The
  // engine does not know what SEC means, and must not (ADR-020's leak test);
  // it shows the badge the game sent or nothing.
  if (joined && !m_connection.WorldBadge().empty())
  {
    rightSeparator();
    UpperCaseInto(m_connection.WorldBadge().c_str(), upper);
    right -= static_cast<float>(TextCellCount(upper)) * cell;
    m_ui.AddText(right, textY, m_uiTuning.bodySizeIndex, m_palette.phosphorDim, upper);
  }

  // `⌖ N SHIPS` -- the player's fleet in the zone, summed from the roster the
  // game just built so the two cannot disagree. `1 SHIP`, never `1 SHIPS`.
  if (joined)
  {
    std::uint32_t ownedShips = 0;
    for (std::uint32_t index = 0; index < m_rosterRowCount; ++index)
    {
      ownedShips += m_rosterRows[index].shipCount;
    }
    rightSeparator();
    std::snprintf(buffer, sizeof(buffer), "\xE2\x8C\x96 %u SHIP%s", ownedShips, ownedShips == 1 ? "" : "S");
    right -= static_cast<float>(TextCellCount(buffer)) * cell;
    m_ui.AddText(right, textY, m_uiTuning.bodySizeIndex, m_palette.phosphor, buffer);
  }

  // --- the fleet roster ---------------------------------------------------
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
  const float barHeight = 3.0f * layout.scale;

  // What the panel managed to draw, against what it was given. The print's
  // `8/11` footer is the difference, and a readout rather than a control:
  // scrolling is a future surface, and until it exists the honest thing is to
  // say how much is not being shown rather than to pretend the list ends here.
  std::uint32_t rosterShown = 0;
  for (std::uint32_t index = 0; index < m_rosterRowCount; ++index)
  {
    const RosterRow& row = m_rosterRows[index];
    // From the same function `UpdateHud` measured the column with, rather than
    // from a running total kept here: the blocks under these chips carry a
    // button now, and a column whose draw and whose hit test each did their own
    // arithmetic is the HUD bug where the two disagree (ADR-020 §5.1).
    const UiRect chip = RosterChipRect(layout.roster, layout.scale, m_rosterTuning, index);
    if (chip.Bottom() > layout.roster.Bottom())
    {
      // The panel is full; the footer below says by how much. Scrolling is
      // still the answer rather than a smaller number, and the primitive it
      // wants now exists (`UiScrollState`) -- wiring it in is the hangar's
      // slice, and the footer is what the player gets until then.
      break;
    }
    ++rosterShown;

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
     * `HOLD FULL`, beside the count, when the wing cannot take another unit
     * (ADR-024 §5c).
     *
     * Only the full state gets a word. A partial hold is a number the player
     * does not have to act on, and a tag on every mining wing all session is
     * the ink that trains an eye to skip the slot -- the same argument the
     * alerts chip makes for hiding at zero. Full is different: it is the moment
     * a Miner stops earning and starts waiting, and it is the one the player
     * has to notice without looking for it.
     *
     * Caution amber, which on this HUD means "not settled" rather than "wrong":
     * a full hold is a thing to deal with, not a failure.
     *
     * `carriesCargo` and not `cargoGauge > 0`, because a combat wing has no
     * hold to be full of -- the game says which wings carry, and this file
     * never works it out from a hull class it is not allowed to know.
     */
    if (row.carriesCargo && row.cargoGauge == 255)
    {
      const char* holdText = "HOLD FULL";
      const auto holdWidth = static_cast<float>(TextCellCount(holdText)) * cell;
      m_ui.AddText(chip.Right() - pad * 0.5f - countWidth - cell - holdWidth, chip.y + pad * 0.5f,
                   m_uiTuning.smallSizeIndex, m_palette.caution, holdText);
    }

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

    /*
     * And the cargo strip, on the same terms as the health pair: drawn only
     * when it has something to say.
     *
     * "Something to say" is the opposite condition here, and that is not an
     * inconsistency. A health strip matters when it is *below* full -- damage
     * is the news -- and a cargo strip matters as it *fills*, because an empty
     * hold is the ordinary state of a fleet that has not been mining. So an
     * untouched hold draws nothing and a working one fills up.
     */
    if (row.carriesCargo && row.cargoGauge > 0)
    {
      const float barWidth = chip.width - pad;
      const float barX = chip.x + pad * 0.5f;
      const float cargoY = chip.Bottom() - pad * 0.5f - barHeight;
      m_ui.AddQuad(UiRect{barX, cargoY, barWidth, barHeight}, m_palette.trackHull);
      m_ui.AddQuad(UiRect{barX, cargoY, barWidth * (static_cast<float>(row.cargoGauge) / 255.0f), barHeight},
                   row.cargoGauge == 255 ? m_palette.caution : m_palette.phosphorDim);
    }

  }

  /*
   * --- the docked blocks --------------------------------------------------
   *
   * The same column, under the wings, because they are the same question: where
   * are my ships. A docked one is absent from the scene entirely -- it is a row
   * in a roster the authority keeps, with no position to draw -- so the panel
   * that lists wings has nothing to list it as, and this is the other list.
   *
   * **Laid out in `UpdateHud`, drawn here.** The button was paint until T3 and
   * the layout could live in the draw; it opens the hangar now, so the rect the
   * press is tested against has to be the rect the quad is drawn from
   * (ADR-020 §5.1). Which blocks were skipped, which fitted, and where each one
   * sits are all decided there -- this loop draws what it was handed.
   *
   * Hidden at zero rather than drawn empty, unlike a wing with no ships: an
   * empty wing is a thing that exists and has lost its members, while "no ships
   * anywhere but here" is not a place at all. The heading follows the same
   * rule, which is why it is inside this branch.
   */
  if (m_locationLayoutCount > 0)
  {
    const float headingTop =
        RosterBlocksTop(layout.roster, m_rosterRowCount, layout.scale, m_rosterTuning) -
        m_rosterTuning.headingHeight * layout.scale;
    m_ui.AddText(layout.roster.x + pad, headingTop, m_uiTuning.smallSizeIndex, m_palette.phosphorLabel, "ELSEWHERE");

    for (std::uint32_t index = 0; index < m_locationLayoutCount; ++index)
    {
      const LocationBlockLayout& placed = m_locationLayouts[index];
      const LocationBlock& block = m_locationBlocks[placed.block];
      const UiRect& blockRect = placed.panel;

      m_ui.AddQuad(blockRect, m_palette.chipBg);
      m_ui.AddBorder(blockRect, 1.0f * layout.scale, m_palette.border);

      UpperCaseInto(block.name != nullptr ? block.name : "?", upper);
      m_ui.AddText(blockRect.x + pad * 0.5f, blockRect.y + pad * 0.5f, m_uiTuning.smallSizeIndex, m_palette.phosphor,
                   upper);

      std::snprintf(buffer, sizeof(buffer), "%u", block.shipCount);
      const auto dockedCountWidth = static_cast<float>(TextCellCount(buffer)) * cell;
      m_ui.AddText(blockRect.Right() - pad * 0.5f - dockedCountWidth, blockRect.y + pad * 0.5f,
                   m_uiTuning.smallSizeIndex, m_palette.phosphor, buffer);

      /*
       * The state word, and the ETA when there is one.
       *
       * The word is the game's (`DOCKED`, `IN WARP`, `ON GRID`) and this file
       * never compares it to anything -- it is drawn, not switched on. What the
       * client *does* decide is the colour, because how loud a readout is
       * belongs to the surface: a crossing is the one state that is going to
       * stop being true on its own, so it reads in the caution amber that means
       * "not settled yet" everywhere else on this HUD, and the two that are
       * simply somewhere read dim.
       */
      const bool crossing = block.etaSeconds >= 0.0f;
      const std::uint32_t stateColour = crossing ? m_palette.caution : m_palette.phosphorDim;
      if (block.stateLabel != nullptr)
      {
        UpperCaseInto(block.stateLabel, upper);
        m_ui.AddText(blockRect.x + pad * 0.5f, blockRect.y + pad * 0.5f + 13.0f * layout.scale,
                     m_uiTuning.smallSizeIndex, stateColour, upper);
      }

      if (crossing)
      {
        // `M:SS`, and never a bare count of seconds: a fleet 214 seconds out is
        // a number the player has to do arithmetic on to plan around.
        const auto whole = static_cast<std::uint32_t>(block.etaSeconds + 0.5f);
        std::snprintf(buffer, sizeof(buffer), "%u:%02u", whole / 60u, whole % 60u);
        const auto etaWidth = static_cast<float>(TextCellCount(buffer)) * cell;
        m_ui.AddText(blockRect.Right() - pad * 0.5f - etaWidth, blockRect.y + pad * 0.5f + 13.0f * layout.scale,
                     m_uiTuning.smallSizeIndex, m_palette.caution, buffer);
      }

      /*
       * The button, and it is live now.
       *
       * It was drawn `phosphorDead` while there was no surface behind it -- the
       * ability rack's arrangement, and the right one for a HUD that must not
       * promise a screen that does not exist. The screen exists, so the word
       * reads at full strength and the frame is a real border.
       */
      if (placed.hasButton)
      {
        m_ui.AddBorder(placed.button, 1.0f * layout.scale, m_palette.border);
        UpperCaseInto(block.buttonLabel, upper);
        const float labelWidth = static_cast<float>(TextCellCount(upper)) * cell;
        m_ui.AddText(placed.button.x + (placed.button.width - labelWidth) * 0.5f,
                     placed.button.y + 2.0f * layout.scale, m_uiTuning.smallSizeIndex, m_palette.phosphor, upper);
      }

    }
  }

  /*
   * `8/11` at the panel's foot, and only when something did not fit.
   *
   * One count over both lists, because the panel is one column to the player:
   * a footer that said 8/8 wings while three location blocks were clipped off
   * the bottom would be technically true and useless. Hidden when everything
   * fits -- a footer reading `11/11` all session is ink that never says
   * anything, which is the same argument the alerts chip makes for hiding at
   * zero.
   */
  {
    /*
     * Both counts come from the two passes that actually placed things, not
     * from a tally kept beside them.
     *
     * `m_locationLayoutCount` is what the column pass fitted; the blocks it was
     * handed are what it was asked to fit, minus the one the scene is already
     * drawing (`inScene`) -- which is skipped for a reason that has nothing to
     * do with room, so counting it as clipped would report a shortfall that is
     * not one.
     */
    std::uint32_t offeredBlocks = 0;
    for (std::uint32_t index = 0; index < m_locationBlockCount; ++index)
    {
      offeredBlocks += m_locationBlocks[index].inScene ? 0u : 1u;
    }

    const std::uint32_t shownTotal = rosterShown + m_locationLayoutCount;
    const std::uint32_t listTotal = m_rosterRowCount + offeredBlocks;
    if (shownTotal < listTotal)
    {
      std::snprintf(buffer, sizeof(buffer), "%u/%u", shownTotal, listTotal);
      const auto footerWidth = static_cast<float>(TextCellCount(buffer)) * cell;
      m_ui.AddText(layout.roster.Right() - pad - footerWidth, layout.roster.Bottom() - pad - smallPx,
                   m_uiTuning.smallSizeIndex, m_palette.phosphorLabel, buffer);
    }
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
   * The selection summary, in the print's spelling: `▣ 1 SHIP : MARROW ▸
   * STANCE AGGRESSIVE ▸ FORMATION LINE`, with the pending-orders chip
   * right-aligned. It reads off the selection, the roster the game just built,
   * and each command's standing parameter -- so it cannot claim a wing the
   * roster does not list or a value no order would carry.
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

    // `▣ 1 SHIP`, never `1 SHIPS` -- it is one branch, and the print insists.
    float contextPen = pad;
    std::snprintf(buffer, sizeof(buffer), "\xE2\x96\xA3 %zu SHIP%s", selectedCount, selectedCount == 1 ? "" : "S");
    m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphorHot, buffer);
    contextPen += static_cast<float>(TextCellCount(buffer)) * cell + cell;

    m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphorGhost, ":");
    contextPen += 2.0f * cell;

    UpperCaseInto(wingName, upper);
    m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphor, upper);
    contextPen += static_cast<float>(TextCellCount(upper)) * cell + cell;

    /*
     * Every standing parameter, `▸`-separated: the label dim, the value bright
     * -- and **caution amber when it is anything but the kind's default**, so
     * a fleet left on an aggressive posture announces itself. The selected
     * command's parameter reads last, beside the verbs that will send it; the
     * others state postures that stand regardless of what the puck is armed
     * with, which is the print's `STANCE AGGRESSIVE ▸ FORMATION LINE` order.
     */
    const std::uint32_t currentSlot = KindSlot(m_selectedKind);
    for (std::uint32_t pass = 0; pass < 2; ++pass)
    {
      for (std::uint32_t slot = 0; slot < m_orderKindCount; ++slot)
      {
        const bool isSelected = slot == currentSlot;
        if ((pass == 0) == isSelected)
        {
          continue; // Non-selected readouts first, the selected one last.
        }
        if (m_kindOptionCounts[slot] == 0 || m_orderKinds[slot].parameterName == nullptr ||
            m_kindOptionIndex[slot] >= m_kindOptionCounts[slot])
        {
          continue;
        }

        m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphorGhost, "\xE2\x96\xB8");
        contextPen += 2.0f * cell;

        UpperCaseInto(m_orderKinds[slot].parameterName, upper);
        m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphorDim, upper);
        contextPen += static_cast<float>(TextCellCount(upper)) * cell + cell;

        UpperCaseInto(m_kindOptions[slot][m_kindOptionIndex[slot]].name, upper);
        const std::uint32_t valueColour =
            m_kindOptionIndex[slot] != m_kindDefaultIndex[slot] ? m_palette.caution : m_palette.phosphor;
        m_ui.AddText(contextPen, contextY, m_uiTuning.bodySizeIndex, valueColour, upper);
        contextPen += static_cast<float>(TextCellCount(upper)) * cell + cell;
      }
    }
  }

  /*
   * The pending-orders chip, right-aligned and hidden at zero: F10's
   * optimistic window made visible. `⏳ 1 ORDER PENDING` counts orders sent
   * and not yet answered -- the caution amber, because a promise the
   * authority has not confirmed is exactly what that colour means here.
   */
  float chipRight = layout.contextBar.Right() - pad;
  if (const std::size_t pendingOrders = m_ghosts.PendingCount(); pendingOrders > 0)
  {
    std::snprintf(buffer, sizeof(buffer), "\xE2\x8F\xB3 %zu ORDER%s PENDING", pendingOrders,
                  pendingOrders == 1 ? "" : "S");
    chipRight -= static_cast<float>(TextCellCount(buffer)) * cell;
    m_ui.AddText(chipRight, contextY, m_uiTuning.bodySizeIndex, m_palette.caution, buffer);
    chipRight -= 2.0f * cell;
  }

  /*
   * The approach chip: the verb that is going to happen when the fleet gets
   * there (ADR-017 2).
   *
   * Beside the pending-orders chip and in the same amber, because it is the
   * same kind of statement -- something the client has promised and the
   * authority has not confirmed. The word is the game's, taken from the context
   * action the player invoked; the long arrow and the ellipsis are the engine's
   * way of saying "on its way" without knowing what is on its way.
   *
   * Visible for exactly as long as the chain is, which is what makes it
   * trustworthy: every path that cancels an approach clears the chip in the
   * same frame, so it can never promise an arrival that stopped being coming.
   */
  /*
   * Why the armed verb is dark, when it is.
   *
   * The print's rule for a disabled primary action is that it is disabled *with
   * a reason* rather than hidden, and this is that rule reaching the command
   * row: a player who has selected MINE with no field under them should not
   * have to send the order to find out. The words are the game's, through
   * `ReasonText` on the code it handed back -- the same words the bounce toast
   * would use, because it is the same code (ADR-005 4).
   *
   * Only the *armed* verb explains itself. Every greyed button carrying its own
   * sentence would be a row of excuses; the one the player is holding is the
   * one they are asking about.
   */
  if (const std::uint32_t armed = KindSlot(m_selectedKind); armed < m_orderKindCount && !m_orderKinds[armed].available &&
                                                            m_orderKinds[armed].reasonCode != 0)
  {
    const char* why = m_worldView->ReasonText(m_orderKinds[armed].reasonCode);
    if (why != nullptr)
    {
      UpperCaseInto(why, upper);
      chipRight -= static_cast<float>(TextCellCount(upper)) * cell;
      m_ui.AddText(chipRight, contextY, m_uiTuning.bodySizeIndex, m_palette.phosphorDim, upper);
      chipRight -= 2.0f * cell;
    }
  }

  /*
   * The warp chips: promises the plane cannot carry (ADR-016 9).
   *
   * Every other order draws its promise where it will happen -- a footprint, a
   * lane, one tick per ship. A warp happens on a grid this client is not
   * looking at, so there is no such place, and the ghost would otherwise be a
   * promise with nowhere to be. It comes here instead, into the chrome, beside
   * the other chips that report things the world cannot show.
   *
   * The word is the game's (`WARP ▸ LINE`, from the preview it solved) and
   * the state is the ghost's: amber while the authority has not answered, the
   * own-fleet phosphor once it has. That is the same two-colour promise the
   * plane draws for a Move, said in text because text is what fits.
   *
   * A refused warp needs nothing here: the bounce toast already carries the
   * reason, on the path every refusal takes, and a chip that lingered to say
   * "that did not work" would be the same sentence twice.
   */
  for (const OrderGhost& ghost : m_ghosts.Ghosts())
  {
    if (ghost.preview.onThisGrid || ghost.state == GhostState::Rejected || ghost.preview.label[0] == 0)
    {
      continue;
    }

    const bool underWay = ghost.state == GhostState::UnderWay;
    std::snprintf(buffer, sizeof(buffer), "â¡ %s", ghost.preview.label);
    UpperCaseInto(buffer, upper);
    chipRight -= static_cast<float>(TextCellCount(upper)) * cell;
    m_ui.AddText(chipRight, contextY, m_uiTuning.bodySizeIndex,
                 underWay ? m_palette.phosphor : m_palette.caution, upper);
    chipRight -= 2.0f * cell;
  }

  if (const char* approachLabel = m_approach.Label(); approachLabel != nullptr)
  {
    std::snprintf(buffer, sizeof(buffer), "\xE2\x9F\xA1 %s\xE2\x80\xA6", approachLabel);
    UpperCaseInto(buffer, upper);
    chipRight -= static_cast<float>(TextCellCount(upper)) * cell;
    m_ui.AddText(chipRight, contextY, m_uiTuning.bodySizeIndex, m_palette.caution, upper);
  }

  /*
   * How eaten the field under the fleet is (ADR-024 §3d), in the context zone.
   *
   * One thin bar per cluster, left of the chips, drawn only on a grid that is
   * that kind of place. The engine asks the game and draws a percentage; it
   * does not learn that the place is a mining field or that the bars are rocks.
   *
   * **The stale word is the game's answer, not a colour this file picks.** A
   * status describing a field that has since re-formed is not merely old --
   * it describes rock that is not there -- so the readout says so in the game's
   * own word and the bars go dead rather than dim. Drawing yesterday's field in
   * today's colour is the failure worth spending a branch on.
   */
  if (Neuron::FieldReadout field; m_worldView->BuildFieldReadout(field) && field.barCount > 0)
  {
    const bool stale = field.staleLabel != nullptr;
    const float barWidth = 3.0f * layout.scale;
    const float barGap = 2.0f * layout.scale;
    const float barMaxHeight = 12.0f * layout.scale;
    const float baseline = layout.contextBar.y + layout.contextBar.height - pad * 0.5f;

    // Right-aligned with the chips rather than trailing the selection summary:
    // that summary's width is a function of the fleet's name, so a readout
    // starting where it ended would move whenever the player selected a
    // different wing.
    const float fieldWidth = static_cast<float>(field.barCount) * (barWidth + barGap);
    chipRight -= fieldWidth + 2.0f * cell;
    float fieldPen = chipRight;
    for (std::uint32_t bar = 0; bar < field.barCount; ++bar)
    {
      // Saturating at 100 is the wire's guarantee; the clamp is here because a
      // bar taller than its zone would draw over the row above it whatever the
      // wire promised.
      const float share = std::min(static_cast<float>(field.fullPct[bar]), 100.0f) / 100.0f;
      const float height = std::max(1.0f * layout.scale, barMaxHeight * share);
      m_ui.AddQuad(UiRect{fieldPen, baseline - barMaxHeight, barWidth, barMaxHeight}, m_palette.trackHull);
      m_ui.AddQuad(UiRect{fieldPen, baseline - height, barWidth, height},
                   stale ? m_palette.phosphorDead : m_palette.phosphor);
      fieldPen += barWidth + barGap;
    }

    if (stale)
    {
      UpperCaseInto(field.staleLabel, upper);
      const auto staleWidth = static_cast<float>(TextCellCount(upper)) * cell;
      chipRight -= staleWidth + cell;
      m_ui.AddText(chipRight, contextY, m_uiTuning.smallSizeIndex, m_palette.neutral, upper);
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
    // is the context bar's to say, so the button stays a single word. A verb
    // that opens a picker carries the print's `▾` caret, because without it
    // nothing distinguishes an immediate verb from one that opens a mode.
    // Measured the same way every other centred label on this HUD is: cells
    // times the cell, because the face is fixed-pitch.
    UpperCaseInto(button.label, upper);
    char verbLabel[56] = {};
    if (button.opensPicker)
    {
      std::snprintf(verbLabel, sizeof(verbLabel), "%s \xE2\x96\xBE", upper);
    }
    else
    {
      std::snprintf(verbLabel, sizeof(verbLabel), "%s", upper);
    }
    const auto labelWidth = static_cast<float>(TextCellCount(verbLabel)) * cell;
    const float labelY = button.rect.y + (button.rect.height - bodyPx) * 0.5f;
    m_ui.AddText(button.rect.x + (button.rect.width - labelWidth) * 0.5f, labelY, m_commandTuning.labelSizeIndex, text,
                 verbLabel);
  }

  /*
   * The far-right chips: `+ QUEUE`, then the undo chip `⎌` at the row's edge.
   * Both yield entirely on a window narrow enough that the verbs reach them.
   *
   * The queue chip names the append gesture (`InputAction::QueueOrder` held at
   * the puck's press) rather than being a button of its own, which is why it
   * is not in `m_commandButtons` and takes no click.
   *
   * The undo chip is the print's revoke affordance for the last
   * unacknowledged order -- the optimistic window again. **Stubbed disabled**:
   * no order-cancel path exists on the wire yet, and a chip that looked
   * pressable while pressing it could revoke nothing would be worse than one
   * that is visibly not for pressing. The zone is drawn now, per the print, so
   * the wire feature lands in a slot the player already knows.
   */
  {
    const bool hasButtons = m_commandButtonCount > 0;
    const float chipTop = hasButtons ? m_commandButtons[0].rect.y : layout.commandRow.y + pad;
    const float chipHeightPx = hasButtons ? m_commandButtons[0].rect.height : m_commandTuning.buttonHeight * layout.scale;
    const float gap = m_commandTuning.buttonGap * layout.scale;

    /*
     * The same predicate the verbs use. QUEUE modifies an order, so it needs
     * the same subject a verb does; undo needs an order to take back instead,
     * which is why it is the one chip the selection does not gate.
     *
     * `ORDER_REVOKE_WIRED` is false because there is no revoke path: the
     * protocol has `OrderSubmit` and `OrderAck` and nothing that retracts
     * (ADR-004 §7). Drawing the chip live while pressing it did nothing would
     * be the same lie as an armed verb with no selection, so the predicate is
     * written out and pinned -- when the message lands, this is one word.
     */
    constexpr bool ORDER_REVOKE_WIRED = false;
    const bool queueEnabled = !m_selection.Ids().empty();
    const bool undoEnabled = ORDER_REVOKE_WIRED && m_ghosts.PendingCount() > 0;

    // 48x48 at 1.0x -- the U2 touch floor, the same square the verbs clamp to.
    const UiRect undoRect{layout.commandRow.Right() - m_commandTuning.paddingX * layout.scale - chipHeightPx, chipTop,
                          chipHeightPx, chipHeightPx};
    if (undoRect.x > lastButtonRight + gap)
    {
      m_ui.AddBorder(undoRect, 1.0f * layout.scale, undoEnabled ? m_palette.border : AtHalfAlpha(m_palette.border));
      const char* undoGlyph = "\xE2\x8E\x8C"; // U+238C.
      const float glyphWidth = static_cast<float>(TextCellCount(undoGlyph)) * cell;
      m_ui.AddText(undoRect.x + (undoRect.width - glyphWidth) * 0.5f, undoRect.y + (undoRect.height - bodyPx) * 0.5f,
                   m_uiTuning.bodySizeIndex, undoEnabled ? m_palette.phosphor : m_palette.phosphorDead, undoGlyph);
    }

    const char* queueLabel = "+ QUEUE";
    const float chipWidth =
        static_cast<float>(TextCellCount(queueLabel)) * cell + 2.0f * m_commandTuning.paddingX * layout.scale;
    const UiRect chipRect{undoRect.x - gap - chipWidth, chipTop, chipWidth, chipHeightPx};
    if (chipRect.x > lastButtonRight + gap)
    {
      const std::uint32_t queueColour = queueEnabled ? m_palette.caution : m_palette.phosphorDead;
      m_ui.AddBorder(chipRect, 1.0f * layout.scale, queueEnabled ? m_palette.caution : AtHalfAlpha(m_palette.border));
      const float labelWidth = static_cast<float>(TextCellCount(queueLabel)) * cell;
      m_ui.AddText(chipRect.x + (chipRect.width - labelWidth) * 0.5f,
                   chipRect.y + (chipRect.height - bodyPx) * 0.5f, m_uiTuning.bodySizeIndex, queueColour, queueLabel);
    }
  }
}

/*
 * --- the settings surface (N3, `settings.png` §1, ADR-020 §8) --------------
 *
 * What one row is set to, as 0..1 along its own control.
 *
 * Normalised on the way out and back in, which is what lets `SettingsScreen`
 * lay out a slider without knowing whether it is carrying a UI scale, a volume
 * or a frame cap -- and what stops a caller converting units twice and getting
 * the two conversions out of step.
 *
 * A `Choice` reports the selected option over the option count, so the same
 * `float` carries both shapes. That is a small compression and it earns itself:
 * the alternative is a variant, and a variant here would be a type for two
 * cases that never travel further than these two functions.
 */
namespace
{

/// Flips a bool to what a normalised value says, and reports whether it moved.
/// A helper rather than three copies, because "did this change" is the question
/// the user layer is written from and getting it wrong once is a file write per
/// frame.
[[nodiscard]] bool Toggled(bool& _flag, float _normalised) noexcept
{
  const bool wanted = _normalised >= 0.5f;
  if (_flag == wanted)
  {
    return false;
  }
  _flag = wanted;
  return true;
}

} // namespace

const char* ClientApp::SettingsReadoutText(SettingsSection _section, std::uint32_t _item, char* _buffer,
                                          std::size_t _bufferBytes) const noexcept
{
  if (_buffer == nullptr || _bufferBytes == 0)
  {
    return "";
  }
  if (_section != SettingsSection::Display)
  {
    return "";
  }

  switch (_item)
  {
  case 2:
    // The swap chain's, not the window's and not the config's: what the client
    // is actually drawing at, after whatever the device did with the request.
    std::snprintf(_buffer, _bufferBytes, "%u x %u", m_swapChain.Width(), m_swapChain.Height());
    return _buffer;
  case 3:
  {
    /*
     * What multisampling was actually built, which `GpuSwapChain::SampleCount`
     * reports for exactly this reason -- the config asks and the device
     * answers, and ADR-003 §7's degradation ladder is allowed to answer with
     * less. A readout that echoed the request would be the one place on this
     * screen that told the player something untrue.
     */
    const std::uint32_t samples = m_swapChain.SampleCount();
    if (samples <= 1)
    {
      return "OFF";
    }
    std::snprintf(_buffer, _bufferBytes, "%ux", samples);
    return _buffer;
  }
  default:
    return "";
  }
}

float ClientApp::SettingsValueOf(SettingsSection _section, std::uint32_t _item) const noexcept
{
  const auto normalise = [](float _value, float _low, float _high) {
    return _high > _low ? std::clamp((_value - _low) / (_high - _low), 0.0f, 1.0f) : 0.0f;
  };
  const auto choice = [](std::uint32_t _index, std::uint32_t _count) {
    return _count > 1 ? static_cast<float>(_index) / static_cast<float>(_count - 1) : 0.0f;
  };

  switch (_section)
  {
  case SettingsSection::Accessibility:
    switch (_item)
    {
    case 0:
    {
      // The palette, by the order the cards are drawn in. Spelled here rather
      // than asked of `HudPalette`, because a name is what the config carries
      // and `ResolveHudPalette` is deliberately one-way -- an unknown word
      // resolves to the default table and must show as the default card.
      const std::uint32_t index = m_config.uiPalette == "deuteranopia" ? 1u : m_config.uiPalette == "tritanopia" ? 2u : 0u;
      return choice(index, 3);
    }
    case 1:
      return normalise(m_config.uiScale, 0.8f, 1.6f);
    case 2:
      return m_config.uiHighContrast ? 1.0f : 0.0f;
    case 3:
      return m_config.uiReduceMotion ? 1.0f : 0.0f;
    case 4:
      return m_config.uiAlwaysShowHullBars ? 1.0f : 0.0f;
    default:
      return 0.0f;
    }
  case SettingsSection::Input:
    switch (_item)
    {
    case 0:
      // LEFT is drawn first and RIGHT second, which is the print's order.
      return choice(m_handedness == Handedness::Left ? 0u : 1u, 2);
    case 1:
      return normalise(m_gestureTuning.longPressSeconds, 0.200f, 0.800f);
    default:
      return 0.0f;
    }
  case SettingsSection::Display:
    switch (_item)
    {
    case 0:
      return m_config.vsync ? 1.0f : 0.0f;
    case 1:
      return normalise(static_cast<float>(m_config.frameCap), 0.0f, 240.0f);
    default:
      // The readouts. Nothing to report as a value -- `BuildSettingsSurface`
      // draws what they say from the live client rather than from here.
      return 0.0f;
    }
  case SettingsSection::Audio:
  case SettingsSection::Account:
  default:
    // The two blocked sections (`SettingsSectionAvailable`). Their rows are
    // drawn and refused, so nothing asks for a value and nothing sets one.
    return 0.0f;
  }
}

/*
 * And the other direction, returning whether anything actually moved.
 *
 * The return is what keeps the user layer honest. A slider dragged across its
 * track is one press and fifty moves, and a screen that marked the layer dirty
 * on every one of them would be a screen that wrote a file for a gesture that
 * changed nothing -- so a set that lands on the value already there says so.
 *
 * **Changes apply immediately**, which is the promise the print writes across
 * its own header. So this does not stage anything: the palette is re-resolved
 * on the spot, the dwell reaches `GestureTuning` on the spot, and the player
 * sees the result of the control they are still holding.
 */
bool ClientApp::SetSettingsValue(SettingsSection _section, std::uint32_t _item, float _normalised)
{
  const float value = std::clamp(_normalised, 0.0f, 1.0f);
  const auto denormalise = [value](float _low, float _high) { return _low + (_high - _low) * value; };
  const auto option = [value](std::uint32_t _count) {
    return _count == 0 ? 0u : std::min(_count - 1u, static_cast<std::uint32_t>(value * static_cast<float>(_count - 1u) + 0.5f));
  };

  switch (_section)
  {
  case SettingsSection::Accessibility:
    switch (_item)
    {
    case 0:
    {
      const char* names[] = {"default", "deuteranopia", "tritanopia"};
      const char* picked = names[option(3)];
      if (m_config.uiPalette == picked)
      {
        return false;
      }
      m_config.uiPalette = picked;
      // The whole cascade, not just the table: ADR-020 §8's "a swap is total
      // rather than partial", and the reason `ApplyPalette` is a function.
      ApplyPalette(m_config.uiPalette);
      return true;
    }
    case 1:
    {
      const float scale = denormalise(0.8f, 1.6f);
      if (std::fabs(scale - m_config.uiScale) < 1e-4f)
      {
        return false;
      }
      m_config.uiScale = scale;
      return true;
    }
    case 2:
      return Toggled(m_config.uiHighContrast, value);
    case 3:
      return Toggled(m_config.uiReduceMotion, value);
    case 4:
      return Toggled(m_config.uiAlwaysShowHullBars, value);
    default:
      return false;
    }
  case SettingsSection::Input:
    switch (_item)
    {
    case 0:
    {
      const Handedness picked = option(2) == 0 ? Handedness::Left : Handedness::Right;
      if (picked == m_handedness)
      {
        return false;
      }
      m_handedness = picked;
      m_config.uiHandedness = HandednessName(picked);
      return true;
    }
    case 1:
    {
      const float seconds = denormalise(0.200f, 0.800f);
      if (std::fabs(seconds - m_gestureTuning.longPressSeconds) < 1e-4f)
      {
        return false;
      }
      // Straight onto the tuning the recognizer reads, so the next press the
      // player makes already holds for the new interval.
      m_gestureTuning.longPressSeconds = seconds;
      m_config.longPressSeconds = seconds;
      return true;
    }
    default:
      return false;
    }
  case SettingsSection::Display:
    switch (_item)
    {
    case 0:
      // Recorded, and it reaches the swap chain on the next present rather than
      // here: a flip model's sync interval is a per-present argument, so there
      // is nothing to re-create and nothing to restart.
      return Toggled(m_config.vsync, value);
    case 1:
    {
      const auto cap = static_cast<std::uint32_t>(denormalise(0.0f, 240.0f) + 0.5f);
      if (cap == m_config.frameCap)
      {
        return false;
      }
      m_config.frameCap = cap;
      return true;
    }
    default:
      return false;
    }
  case SettingsSection::Audio:
  case SettingsSection::Account:
  default:
    return false;
  }
}

/*
 * The presses.
 *
 * Order is the screen's own hierarchy: the way off first, then the rail, then
 * the body. The same shape `UpdateStationSurface` has, and for its reason -- a
 * press that both left the screen and moved a slider would be two answers to
 * one gesture.
 */
void ClientApp::UpdateSettingsSurface()
{
  const float scale = m_uiLayout.scale;
  m_settingsLayout = ResolveSettingsScreen(m_input.viewportWidth, m_input.viewportHeight, scale, m_settingsTuning);

  const std::span<const SettingsItem> items = SettingsItemsFor(m_settingsSection);
  m_settingsNavCount = BuildSettingsNav(m_settingsLayout.nav, scale, m_settingsTuning, m_settingsNav);
  m_settingsRowCount = BuildSettingsRows(items, m_settingsLayout.body, scale, m_settingsTuning,
                                         m_settingsScroll.Offset(), m_settingsRows);
  // The rail is laid out before the scroll is sized, because a press on it must
  // be answerable on the frame the body is still catching up on.

  // The way back, before anything else can take the press.
  if (m_router.ClaimPointerIn(m_settingsLayout.back))
  {
    OnSurfaceChanged(m_surfaces.Back());
    return;
  }

  /*
   * The body's wheel, which is §7's declared overflow answer for this surface.
   *
   * Counted in *rows* rather than pixels, which is what `UiScrollState` already
   * measures and what `BuildSettingsRows` takes -- so there is no conversion
   * between the two for a caller to get wrong in one direction. The visible
   * count is the run's own tallest row into the body, which under-counts a body
   * of short rows and is the safe direction: it scrolls a little further than it
   * strictly must rather than hiding a row the player cannot reach.
   */
  const float tallestRow = SettingsRowHeightPixels(m_settingsTuning.sliderRowHeight, scale);
  m_settingsScroll.SetExtent(static_cast<std::uint32_t>(items.size()),
                             VisibleRowCount(m_settingsLayout.body.height, tallestRow));
  if (m_settingsLayout.body.Contains(m_router.CursorX(), m_router.CursorY()) && m_router.WheelAvailable())
  {
    m_settingsScroll.ScrollByWheel(m_router.WheelSteps());
    (void)m_router.ClaimWheel();
  }

  if (!m_router.Pressed(InputButton::Left))
  {
    return;
  }

  const float cursorX = m_router.CursorX();
  const float cursorY = m_router.CursorY();

  if (const SettingsNavRow* row = HitSettingsNav({m_settingsNav, m_settingsNavCount}, cursorX, cursorY);
      row != nullptr && m_router.ClaimPointer())
  {
    if (row->section != m_settingsSection)
    {
      m_settingsSection = row->section;
      // A new section starts at its top. Unlike the section *choice*, which
      // survives leaving the screen, a scroll offset from another list is
      // meaningless against this one.
      m_settingsScroll.Reset();
    }
    return;
  }

  if (const SettingsRowRect* row = HitSettingsRow({m_settingsRows, m_settingsRowCount}, cursorX, cursorY);
      row != nullptr && m_router.ClaimPointer())
  {
    switch (row->kind)
    {
    case SettingsControlKind::Toggle:
      m_settingsDirty = SetSettingsValue(m_settingsSection, row->item,
                                         SettingsValueOf(m_settingsSection, row->item) >= 0.5f ? 0.0f : 1.0f) ||
                        m_settingsDirty;
      break;
    case SettingsControlKind::Slider:
      m_settingsDirty =
          SetSettingsValue(m_settingsSection, row->item, SettingsSliderValueAt(*row, cursorX)) || m_settingsDirty;
      break;
    case SettingsControlKind::Choice:
    {
      const std::uint32_t picked = HitSettingsChoice(*row, scale, m_settingsTuning, cursorX, cursorY);
      if (picked < row->optionCount && row->optionCount > 1)
      {
        const float value = static_cast<float>(picked) / static_cast<float>(row->optionCount - 1);
        m_settingsDirty = SetSettingsValue(m_settingsSection, row->item, value) || m_settingsDirty;
      }
      break;
    }
    case SettingsControlKind::Readout:
    case SettingsControlKind::Placeholder:
    default:
      // Not reachable: `HitSettingsRow` refuses a row that is not a target.
      break;
    }
  }
}

/*
 * The settings surface's draw (N3, `settings.png` §1).
 *
 * The print's own reading of what this screen is: *"every control here is either
 * an accessibility requirement or a decision the design deliberately refused to
 * make for the player"*. So the draw's job is to make the controls legible and
 * the refusals visible, and there is nothing else on it.
 *
 * Reads `m_settingsLayout` and the two runs `UpdateSettingsSurface` resolved
 * this frame rather than resolving them again -- one answer, so a press and a
 * pixel cannot disagree.
 */
void ClientApp::BuildSettingsSurface()
{
  const SettingsScreenLayout& screen = m_settingsLayout;
  const float scale = screen.scale;
  const float cell = 8.0f * scale;
  const float pad = m_uiTuning.padding * scale;
  const float line = 1.0f * scale;
  const float bodyPx = BASE_FONT_SIZES_PIXELS[m_uiTuning.bodySizeIndex] * scale;
  const float smallPx = BASE_FONT_SIZES_PIXELS[m_uiTuning.smallSizeIndex] * scale;

  char buffer[128] = {};

  const auto centred = [&](const UiRect& _rect, std::uint8_t _size, std::uint32_t _colour, const char* _text) {
    const float width = static_cast<float>(TextCellCount(_text)) * cell;
    const float height = BASE_FONT_SIZES_PIXELS[_size] * scale;
    m_ui.AddText(_rect.x + (_rect.width - width) * 0.5f, _rect.y + (_rect.height - height) * 0.5f, _size, _colour,
                 _text);
  };

  // The ground, opaque: there is no world behind a full-screen surface, so
  // anything reading through would be whatever the back buffer last held.
  m_ui.AddQuad(screen.viewport, WithAlpha(m_palette.panel, 0xFF));

  // --- the header ---------------------------------------------------------
  m_ui.AddBorder(screen.back, line, m_palette.border);
  centred(screen.back, m_uiTuning.bodySizeIndex, m_palette.phosphor, "< BACK");
  m_ui.AddText(screen.back.Right() + pad * 2.0f, screen.header.y + (screen.header.height - bodyPx) * 0.5f,
               m_uiTuning.bodySizeIndex, m_palette.phosphorHot, "SETTINGS");

  /*
   * The apply notice, right-aligned, and it is a promise rather than a label:
   * there is no OK and no CANCEL on this screen, so a player has to be told
   * that what they just moved has already happened.
   */
  {
    const char* notice = "CHANGES APPLY IMMEDIATELY";
    const float width = static_cast<float>(TextCellCount(notice)) * cell;
    m_ui.AddText(screen.header.Right() - pad - width, screen.header.y + (screen.header.height - smallPx) * 0.5f,
                 m_uiTuning.smallSizeIndex, m_palette.phosphorLabel, notice);
  }
  m_ui.AddQuad(UiRect{0.0f, screen.header.Bottom() - line, screen.viewport.width, line}, m_palette.rule);

  // --- the rail -----------------------------------------------------------
  for (std::uint32_t index = 0; index < m_settingsNavCount; ++index)
  {
    const SettingsNavRow& row = m_settingsNav[index];
    const bool selected = row.section == m_settingsSection;
    if (selected)
    {
      m_ui.AddQuad(row.rect, m_palette.rule);
      m_ui.AddBorder(row.rect, line, m_palette.borderStrong);
    }
    const std::uint32_t colour = !row.enabled ? m_palette.phosphorDead
                                 : selected   ? m_palette.phosphorHot
                                              : m_palette.phosphorDim;
    m_ui.AddText(row.rect.x + pad, row.rect.y + (row.rect.height - bodyPx) * 0.5f, m_uiTuning.bodySizeIndex, colour,
                 SettingsSectionName(row.section));
  }

  /*
   * The footer, which is not decoration: it is what a player is asked to read
   * out when something has gone wrong, so it is on the one screen they can
   * always reach.
   */
  m_ui.AddQuad(UiRect{screen.footer.x + pad, screen.footer.y, screen.footer.width - pad * 2.0f, line}, m_palette.rule);
  std::snprintf(buffer, sizeof buffer, "SCHEMA %08X", m_worldView->SchemaHash());
  m_ui.AddText(screen.footer.x + pad, screen.footer.y + pad, m_uiTuning.smallSizeIndex, m_palette.phosphorGhost,
               buffer);

  // --- the section heading ------------------------------------------------
  m_ui.AddText(screen.bodyHeading.x + m_settingsTuning.bodyPaddingX * scale,
               screen.bodyHeading.y + m_settingsTuning.bodyPaddingY * scale, m_uiTuning.bodySizeIndex,
               m_palette.phosphorHot, SettingsSectionName(m_settingsSection));
  m_ui.AddText(screen.bodyHeading.x + m_settingsTuning.bodyPaddingX * scale,
               screen.bodyHeading.y + m_settingsTuning.bodyPaddingY * scale + bodyPx + line * 4.0f,
               m_uiTuning.smallSizeIndex, m_palette.phosphorBody, SettingsSectionNote(m_settingsSection));

  /*
   * A blocked section says what it is waiting for, where its controls would be.
   *
   * Drawn instead of the rows rather than over them: the rows exist so the
   * shape is known (`settings.png` §2's instruction for ACCOUNT), and the
   * sentence is what stops a player concluding the feature is missing rather
   * than pending.
   */
  if (!SettingsSectionAvailable(m_settingsSection))
  {
    m_ui.AddText(screen.body.x + m_settingsTuning.bodyPaddingX * scale,
                 screen.body.y + m_settingsTuning.bodyPaddingY * scale, m_uiTuning.smallSizeIndex,
                 m_palette.phosphorLabel, SettingsSectionBlockedReason(m_settingsSection));
  }

  // --- the rows -----------------------------------------------------------
  const std::span<const SettingsItem> items = SettingsItemsFor(m_settingsSection);
  for (std::uint32_t index = 0; index < m_settingsRowCount; ++index)
  {
    const SettingsRowRect& row = m_settingsRows[index];
    if (row.item >= items.size())
    {
      continue;
    }
    const SettingsItem& item = items[row.item];
    const std::uint32_t labelColour = row.enabled ? m_palette.phosphor : m_palette.phosphorDead;

    m_ui.AddText(row.rect.x, row.rect.y + m_settingsTuning.rowNoteOffset * scale * 0.2f, m_uiTuning.bodySizeIndex,
                 labelColour, item.label);
    if (item.note[0] != '\0')
    {
      m_ui.AddText(row.rect.x, row.rect.y + m_settingsTuning.rowNoteOffset * scale, m_uiTuning.smallSizeIndex,
                   m_palette.phosphorBody, item.note);
    }

    const float value = SettingsValueOf(m_settingsSection, row.item);

    switch (row.kind)
    {
    case SettingsControlKind::Toggle:
    {
      // The switch: a track and a knob at one end of it. Drawn at the print's
      // size inside the (larger) target rect, centred on it.
      const float switchWidth = m_settingsTuning.toggleWidth * scale;
      const float switchHeight = m_settingsTuning.toggleHeight * scale;
      const UiRect track{row.control.x + (row.control.width - switchWidth) * 0.5f,
                         row.control.y + (row.control.height - switchHeight) * 0.5f, switchWidth, switchHeight};
      const bool on = value >= 0.5f;
      m_ui.AddQuad(track, on ? m_palette.rule : m_palette.chipBg);
      m_ui.AddBorder(track, line, on ? m_palette.borderStrong : m_palette.border);
      const float knob = switchHeight - line * 4.0f;
      m_ui.AddQuad(UiRect{on ? track.Right() - knob - line * 2.0f : track.x + line * 2.0f, track.y + line * 2.0f, knob,
                          knob},
                   on ? m_palette.phosphor : m_palette.phosphorDim);
      break;
    }
    case SettingsControlKind::Slider:
    {
      const UiRect track{row.control.x, row.control.y + (row.control.height - m_settingsTuning.sliderTrackHeight * scale) * 0.5f,
                         row.control.width, m_settingsTuning.sliderTrackHeight * scale};
      m_ui.AddQuad(track, m_palette.chipBg);
      m_ui.AddQuad(UiRect{track.x, track.y, track.width * std::clamp(value, 0.0f, 1.0f), track.height},
                   m_palette.phosphorDim);
      const UiRect handle = SettingsSliderHandle(row, value, scale, m_settingsTuning);
      m_ui.AddQuad(handle, m_palette.phosphor);
      m_ui.AddBorder(handle, line, m_palette.borderStrong);

      // The value in its own units, right of the label, which is where the
      // print puts the "1.0x".
      const float shown = item.minimum + (item.maximum - item.minimum) * value;
      std::snprintf(buffer, sizeof buffer, item.maximum > 3.0f ? "%.0f" : "%.2f",
                    static_cast<double>(shown));
      const float width = static_cast<float>(TextCellCount(buffer)) * cell;
      m_ui.AddText(row.rect.Right() - width, row.rect.y + m_settingsTuning.rowNoteOffset * scale * 0.2f,
                   m_uiTuning.bodySizeIndex, m_palette.phosphorHot, buffer);
      break;
    }
    case SettingsControlKind::Choice:
    {
      const std::uint32_t selected =
          row.optionCount > 1
              ? std::min(row.optionCount - 1u, static_cast<std::uint32_t>(value * static_cast<float>(row.optionCount - 1u) + 0.5f))
              : 0u;
      for (std::uint32_t option = 0; option < row.optionCount; ++option)
      {
        const UiRect card = SettingsChoiceCard(row, option, scale, m_settingsTuning);
        const bool picked = option == selected;
        m_ui.AddQuad(card, picked ? m_palette.rule : m_palette.chipBg);
        m_ui.AddBorder(card, line, picked ? m_palette.borderStrong : m_palette.border);
        centred(card, m_uiTuning.smallSizeIndex, picked ? m_palette.phosphorHot : m_palette.phosphorDim,
                SettingsChoiceLabel(m_settingsSection, row.item, option));

        /*
         * A palette card wears its own table's swatches, which is the print's
         * whole argument for this control: *"a player choosing a colour-vision
         * palette from a dropdown labelled Deuteranopia is being asked to trust
         * a word rather than judge a result"*.
         */
        if (m_settingsSection == SettingsSection::Accessibility && row.item == 0)
        {
          const HudPalette table = option == 1   ? DeuteranopiaPalette()
                                   : option == 2 ? TritanopiaPalette()
                                                 : HudPalette{};
          const float swatch = std::max(0.0f, (card.width - pad * 2.0f) / 4.0f);
          const float swatchTop = card.y + pad;
          for (std::uint32_t standing = 0; standing < STANDING_COLOUR_COUNT; ++standing)
          {
            m_ui.AddQuad(UiRect{card.x + pad + swatch * static_cast<float>(standing), swatchTop, swatch - line * 2.0f,
                                swatch},
                         StandingColourOf(table, static_cast<StandingColour>(standing)));
          }
        }
      }
      break;
    }
    case SettingsControlKind::Readout:
    {
      // What the client actually got, right-aligned where a value would be.
      const char* shown = SettingsReadoutText(m_settingsSection, row.item, buffer, sizeof buffer);
      const float width = static_cast<float>(TextCellCount(shown)) * cell;
      m_ui.AddText(row.rect.Right() - width, row.rect.y + m_settingsTuning.rowNoteOffset * scale * 0.2f,
                   m_uiTuning.bodySizeIndex, m_palette.phosphorDim, shown);
      break;
    }
    case SettingsControlKind::Placeholder:
    default:
      break;
    }
  }

  // --- the right column ---------------------------------------------------
  //
  // The preview panel. Its contents are I3's -- a scrap of the tactical view
  // with real hulls in it -- and what is here is the frame plus the four
  // standing swatches, which is the part of the proof this slice can make.
  m_ui.AddQuad(screen.preview, m_palette.chipBg);
  m_ui.AddBorder(screen.preview, line, m_palette.border);
  m_ui.AddText(screen.preview.x + pad, screen.preview.y + pad, m_uiTuning.smallSizeIndex, m_palette.phosphorLabel,
               "LIVE PREVIEW");
  {
    const float swatch = std::max(0.0f, screen.preview.height * 0.28f);
    const float top = screen.preview.Bottom() - pad - swatch;
    for (std::uint32_t standing = 0; standing < STANDING_COLOUR_COUNT; ++standing)
    {
      m_ui.AddQuad(UiRect{screen.preview.x + pad + (swatch + pad) * static_cast<float>(standing), top, swatch, swatch},
                   StandingColourOf(m_palette, static_cast<StandingColour>(standing)));
    }
  }

  /*
   * The contrast audit, which is the panel that turns an accessibility claim
   * into a proof (`settings.png` §1).
   *
   * Four rows, which is what the print draws -- the ground rows, in standing
   * order. `ContrastAudit` measures ten; the six pair readings are not drawn
   * because they have no floor to be judged against and a column of numbers
   * nobody can act on is the kind of thing a settings screen accumulates.
   */
  m_ui.AddBorder(screen.audit, line, m_palette.border);
  m_ui.AddText(screen.audit.x + pad, screen.audit.y + pad, m_uiTuning.smallSizeIndex, m_palette.phosphorLabel,
               "CONTRAST AUDIT");
  {
    float pen = screen.audit.y + m_settingsTuning.auditHeadingHeight * scale;
    for (std::uint32_t index = 0; index < m_auditRowCount; ++index)
    {
      const ContrastRow& audit = m_auditRows[index];
      if (!audit.Floored())
      {
        continue;
      }
      const float rowHeight = m_settingsTuning.auditRowHeight * scale;
      if (pen + rowHeight > screen.audit.Bottom())
      {
        break;
      }
      std::snprintf(buffer, sizeof buffer, "%s vs void", StandingColourName(audit.first));
      m_ui.AddText(screen.audit.x + pad, pen + (rowHeight - smallPx) * 0.5f, m_uiTuning.smallSizeIndex,
                   StandingColourOf(m_palette, audit.first), buffer);

      std::snprintf(buffer, sizeof buffer, "%.1f:1", static_cast<double>(audit.ratio));
      const float width = static_cast<float>(TextCellCount(buffer)) * cell;
      m_ui.AddText(screen.audit.Right() - pad - width, pen + (rowHeight - smallPx) * 0.5f, m_uiTuning.smallSizeIndex,
                   audit.MeetsFloor() ? m_palette.phosphorHot : m_palette.hostile, buffer);
      pen += rowHeight;
    }
  }

  /*
   * The handedness panel, which the print marks as the wheel's hard
   * prerequisite. Drawn here rather than only in the INPUT section because that
   * is where the print puts it -- beside the preview, where a player changing
   * palettes can also answer the one question the command wheel cannot ship
   * without.
   */
  if (screen.handedness.height > pad * 3.0f)
  {
    m_ui.AddBorder(screen.handedness, line, m_palette.border);
    m_ui.AddText(screen.handedness.x + pad, screen.handedness.y + pad, m_uiTuning.smallSizeIndex, m_palette.caution,
                 "HANDEDNESS - THE WHEEL DEPENDS ON THIS");
    const float buttonTop = screen.handedness.y + m_settingsTuning.auditHeadingHeight * scale;
    const float buttonHeight = SettingsRowHeightPixels(m_settingsTuning.resetHeight, scale);
    if (buttonTop + buttonHeight <= screen.handedness.Bottom())
    {
      const float half = std::max(0.0f, (screen.handedness.width - pad * 3.0f) * 0.5f);
      const UiRect left{screen.handedness.x + pad, buttonTop, half, buttonHeight};
      const UiRect right{left.Right() + pad, buttonTop, half, buttonHeight};
      const bool isLeft = m_handedness == Handedness::Left;
      m_ui.AddQuad(isLeft ? left : right, m_palette.rule);
      m_ui.AddBorder(left, line, isLeft ? m_palette.borderStrong : m_palette.border);
      m_ui.AddBorder(right, line, isLeft ? m_palette.border : m_palette.borderStrong);
      centred(left, m_uiTuning.bodySizeIndex, isLeft ? m_palette.phosphorHot : m_palette.phosphorDim, "< LEFT");
      centred(right, m_uiTuning.bodySizeIndex, isLeft ? m_palette.phosphorDim : m_palette.phosphorHot, "RIGHT >");
    }
  }

  // The resets. Drawn and refused: what "reset" means against a layer that
  // records *changes* rather than state is N3's remainder, and a button that
  // did the wrong thing to a settings file is worse than one that waits.
  for (const UiRect* reset : {&screen.resetSection, &screen.resetAll})
  {
    m_ui.AddBorder(*reset, line, AtHalfAlpha(m_palette.border));
  }
  centred(screen.resetSection, m_uiTuning.smallSizeIndex, m_palette.phosphorDead, "RESET SECTION");
  centred(screen.resetAll, m_uiTuning.smallSizeIndex, m_palette.phosphorDead, "RESET ALL");
}

void ClientApp::BuildStationSurface()
{
  const StationScreenLayout& screen = m_stationLayout;
  const float scale = screen.scale;
  const float cell = 8.0f * scale;
  const float pad = m_uiTuning.padding * scale;
  const float line = 1.0f * scale;
  const float bodyPx = BASE_FONT_SIZES_PIXELS[m_uiTuning.bodySizeIndex] * scale;
  const float smallPx = BASE_FONT_SIZES_PIXELS[m_uiTuning.smallSizeIndex] * scale;

  char buffer[96] = {};
  char upper[96] = {};

  /// A line of text centred in a rect, which every chip on this screen wants.
  const auto centred = [&](const UiRect& _rect, std::uint8_t _size, std::uint32_t _colour, const char* _text) {
    const float width = static_cast<float>(TextCellCount(_text)) * cell;
    const float height = BASE_FONT_SIZES_PIXELS[_size] * scale;
    m_ui.AddText(_rect.x + (_rect.width - width) * 0.5f, _rect.y + (_rect.height - height) * 0.5f, _size, _colour,
                 _text);
  };

  /*
   * The ground, at full alpha across the whole viewport.
   *
   * `HudPalette::panel` is translucent by design -- 12% reads through, because
   * a HUD is a border around a world. A surface is not: there is no world
   * behind it this frame (`SurfaceRecordsWorld` said so, and the frame skipped
   * the world half), so anything showing through would be the last thing the
   * back buffer happened to hold.
   */
  m_ui.AddQuad(screen.viewport, WithAlpha(m_palette.panel, 0xFF));

  /*
   * --- the status bar -------------------------------------------------------
   *
   * The way back, and which place this is.
   *
   * The name is the game's answer and is drawn rather than interpreted -- and
   * it is found by the anchor rather than remembered, because the roster
   * arrives at about 1 Hz and the fleet may have moved since the press. A
   * hangar whose ships all undocked from another surface has a block that no
   * longer exists, and the honest screen for that is one that says so rather
   * than one still showing the count it opened with.
   *
   * **The count is the roster's own, and that is a correction.** It used to
   * come from the block beside the name, which is a different fact from a
   * different record: a commander with ships on a grid *and* docked at one
   * anchor has two blocks there, this loop takes the first, and the word
   * `DOCKED` was written under it regardless -- so the bar reported a fleet in
   * space as docked and disagreed with the list underneath it. Taking it from
   * the roster this screen is drawing makes the bar and the list one statement,
   * which is the only arrangement in which they cannot contradict each other.
   *
   * `StationRosterCounts::docked` rather than the columns' sum, because the bar
   * is a statement about the station and not about what fitted: a hangar with
   * more wings than there are columns drops ships out of both other totals.
   */
  m_ui.AddBorder(m_backChipRect, line, m_palette.border);
  centred(m_backChipRect, m_uiTuning.bodySizeIndex, m_palette.phosphor, BACK_CHIP_LABEL);

  const LocationBlock* here = nullptr;
  for (std::uint32_t index = 0; index < m_locationBlockCount; ++index)
  {
    if (m_locationBlocks[index].anchor == m_stationAnchor)
    {
      here = &m_locationBlocks[index];
      break;
    }
  }

  const float statusTextY = screen.statusBar.y + (screen.statusBar.height - bodyPx) * 0.5f;
  const float titleX = m_backChipRect.Right() + pad * 2.0f;
  if (here != nullptr)
  {
    UpperCaseInto(here->name != nullptr ? here->name : "?", upper);
    m_ui.AddText(titleX, statusTextY, m_uiTuning.bodySizeIndex, m_palette.phosphorHot, upper);

    std::snprintf(buffer, sizeof(buffer), "%u SHIP%s DOCKED", static_cast<unsigned>(m_stationRoster.docked),
                  m_stationRoster.docked == 1 ? "" : "S");
    const float countWidth = static_cast<float>(TextCellCount(buffer)) * cell;
    m_ui.AddText(screen.statusBar.Right() - pad * 2.0f - countWidth,
                 screen.statusBar.y + (screen.statusBar.height - smallPx) * 0.5f, m_uiTuning.smallSizeIndex,
                 m_palette.phosphorLabel, buffer);
  }
  else
  {
    m_ui.AddText(titleX, statusTextY, m_uiTuning.bodySizeIndex, m_palette.phosphorDim, "NO SHIPS HERE");
  }
  m_ui.AddSegment(screen.statusBar.x, screen.statusBar.Bottom(), screen.statusBar.Right(), screen.statusBar.Bottom(),
                  line, m_palette.rule);

  /*
   * --- the tab row ----------------------------------------------------------
   *
   * Every tab the game reported, drawn whether or not it has a service behind
   * it: "tabs, not tiles", so the station's future arrives as siblings in a
   * fixed row and the layout never reshuffles when one lands. A tab with
   * nothing behind it is dimmed and carries its reason -- FREE, SOON, FUTURE --
   * which is the game's word and one this file never spells.
   */
  for (std::uint32_t index = 0; index < m_stationTabButtonCount; ++index)
  {
    const StationTabButton& button = m_stationTabButtons[index];
    const StationTab& tab = m_stationTabs[button.tab];
    const bool live = button.enabled && tab.id == m_stationTab;

    if (live)
    {
      m_ui.AddQuad(button.rect, m_palette.rule);
    }
    m_ui.AddBorder(button.rect, line, button.enabled ? m_palette.border : m_palette.phosphorGhost);

    const float textY = button.rect.y + (button.rect.height - bodyPx) * 0.5f;
    const float textX = button.rect.x + m_stationTuning.tabPaddingCells * cell;
    const std::uint32_t nameColour =
        !button.enabled ? m_palette.phosphorDead : (live ? m_palette.phosphorHot : m_palette.phosphor);
    m_ui.AddText(textX, textY, m_uiTuning.bodySizeIndex, nameColour, tab.name != nullptr ? tab.name : "");

    if (tab.tag != nullptr)
    {
      const float nameWidth = static_cast<float>(TextCellCount(tab.name != nullptr ? tab.name : "")) * cell;
      m_ui.AddText(textX + nameWidth + cell, button.rect.y + (button.rect.height - smallPx) * 0.5f,
                   m_uiTuning.smallSizeIndex, m_palette.phosphorLabel, tab.tag);
    }
  }

  /*
   * --- the wing columns -----------------------------------------------------
   *
   * A column per wing, chips down it, and the vocabulary is the whole of it: a
   * class word and a wing. **No gauges, and their absence is a rule rather
   * than an omission** -- the roster holds no damage state, which is what makes
   * undocking repair a ship by construction (ADR-017 §1). A hull bar here would
   * be drawing a number that does not exist.
   */
  for (std::uint32_t index = 0; index < m_stationLaid.groups; ++index)
  {
    const StationColumnRect& column = m_stationColumns[index];
    const StationGroup& group = m_stationGroups[column.group];

    UpperCaseInto(group.name != nullptr ? group.name : "UNASSIGNED", upper);
    m_ui.AddText(column.header.x, column.header.y + (column.header.height - bodyPx) * 0.5f - 2.0f * scale,
                 m_uiTuning.bodySizeIndex, m_palette.phosphor, upper);

    // The count beside the name is the game's, and it is on screen at all
    // because names live in the user settings layer: two clients without
    // shared settings see different words for one wing, and the number is the
    // part they agree on.
    if (group.selectedCount > 0)
    {
      std::snprintf(buffer, sizeof(buffer), "%u/%u", static_cast<unsigned>(group.selectedCount),
                    static_cast<unsigned>(group.chipCount));
    }
    else
    {
      std::snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(group.chipCount));
    }
    const float countWidth = static_cast<float>(TextCellCount(buffer)) * cell;
    m_ui.AddText(column.header.Right() - countWidth, column.header.y + (column.header.height - smallPx) * 0.5f,
                 m_uiTuning.smallSizeIndex, group.selectedCount > 0 ? m_palette.phosphorHot : m_palette.phosphorLabel,
                 buffer);
    m_ui.AddSegment(column.header.x, column.header.Bottom() - line, column.header.Right(),
                    column.header.Bottom() - line, line, m_palette.rule);
  }

  for (std::uint32_t index = 0; index < m_stationLaid.chips; ++index)
  {
    const StationChipRect& laid = m_stationChipRects[index];
    if (laid.chip >= m_stationRoster.chips)
    {
      continue; // The roster shrank between the layout and the draw. Cannot
                // happen in one frame; costs one compare to be sure of.
    }
    const StationChip& chip = m_stationChips[laid.chip];

    m_ui.AddQuad(laid.rect, chip.selected ? m_palette.rule : m_palette.chipBg);
    if (chip.selected)
    {
      m_ui.AddBorder(laid.rect, line, m_palette.border);
    }

    const float textY = laid.rect.y + (laid.rect.height - smallPx) * 0.5f;
    m_ui.AddText(laid.rect.x + cell, textY, m_uiTuning.smallSizeIndex,
                 chip.selected ? m_palette.phosphorHot : m_palette.phosphorBody,
                 chip.className != nullptr ? chip.className : "?");

    // The durable roster id, small and to the right. Nothing in this game names
    // an individual ship (`StationChip::name` is null and that is honest rather
    // than unfinished), so the number is what a player can point at.
    std::snprintf(buffer, sizeof(buffer), "%u", chip.shipId);
    const float idWidth = static_cast<float>(TextCellCount(buffer)) * cell;
    m_ui.AddText(laid.rect.Right() - cell - idWidth, textY, m_uiTuning.smallSizeIndex, m_palette.phosphorGhost, buffer);
  }

  // That the list continues, in the two directions it can. A marker rather than
  // a bar: the wheel is the gesture and a scrollbar nobody drags would be a
  // control that does nothing (ADR-020 §4).
  if (m_stationScroll.CanScrollUp())
  {
    m_ui.AddText(screen.roster.Right() - pad * 2.0f, screen.roster.y + pad, m_uiTuning.smallSizeIndex,
                 m_palette.phosphorDim, "\xE2\x96\xB2");
  }
  if (m_stationScroll.CanScrollDown())
  {
    m_ui.AddText(screen.roster.Right() - pad * 2.0f, screen.roster.Bottom() - pad - smallPx,
                 m_uiTuning.smallSizeIndex, m_palette.phosphorDim, "\xE2\x96\xBC");
  }

  if (m_stationRoster.chips == 0)
  {
    m_ui.AddText(screen.roster.x + m_stationTuning.rosterPaddingX * scale,
                 screen.roster.y + m_stationTuning.rosterPaddingY * scale, m_uiTuning.bodySizeIndex,
                 m_palette.phosphorDim, "NOTHING DOCKED HERE");
  }

  /*
   * --- the composer ---------------------------------------------------------
   *
   * The station screen "is not a management screen over fleet objects; there
   * are none. It is a selection surface: pick docked ships, pick a formation,
   * UNDOCK." This is the second half of that sentence.
   */
  m_ui.AddSegment(screen.composer.x, screen.composer.y, screen.composer.x, screen.composer.Bottom(), line,
                  m_palette.rule);

  m_ui.AddText(screen.selectionHead.x, screen.composer.y + m_stationTuning.composerPaddingY * scale,
               m_uiTuning.smallSizeIndex, m_palette.phosphorLabel, "COMPOSER");

  if (m_composer.Empty())
  {
    m_ui.AddText(screen.selectionHead.x, screen.selectionHead.y + (screen.selectionHead.height - bodyPx) * 0.5f,
                 m_uiTuning.bodySizeIndex, m_palette.phosphorDim, "NOTHING SELECTED");
  }
  else
  {
    std::snprintf(buffer, sizeof(buffer), "%u SELECTED", static_cast<unsigned>(m_composer.Count()));
    m_ui.AddText(screen.selectionHead.x, screen.selectionHead.y + (screen.selectionHead.height - bodyPx) * 0.5f,
                 m_uiTuning.bodySizeIndex, m_palette.phosphorHot, buffer);
  }

  /*
   * A parameter, as a cycling chip rather than a dropdown.
   *
   * The print draws a list; this cycles values with words for them in place,
   * which is exactly the shape the command row already uses -- and a list that
   * opened would be a second surface floating over a full-screen one, for a
   * choice that fits on the control.
   *
   * **One lambda, called twice**: the primary's chip under the selection head,
   * the secondary's under the diagram. They are the same control -- a label the
   * game wrote and a value the game named -- and the only thing that varies is
   * which rect and which option row. The formation list is three values and the
   * wing list is as many wings as there are, which is the one place the shape
   * stretches; it stays a chip because the wing list is bounded by the roster's
   * row cap and so is never long either.
   *
   * The label falls back to nothing rather than to a word: this side spelling
   * FORMATION when the game went quiet would be spelling a word it may not
   * know, which is the leak the whole file is arranged to avoid.
   */
  const auto drawOptionChip = [&](const UiRect& _rect, std::uint32_t _action)
  {
    const StationAction* chipAction = _action < m_stationActionCount ? &m_stationActions[_action] : nullptr;
    const bool hasValue = m_stationOptionCount[_action] > 0 && m_stationOptionIndex[_action] < m_stationOptionCount[_action];
    m_ui.AddBorder(_rect, line, hasValue ? m_palette.border : m_palette.phosphorGhost);

    if (chipAction != nullptr && chipAction->parameterName != nullptr)
    {
      m_ui.AddText(_rect.x + cell, _rect.y + (_rect.height - smallPx) * 0.5f, m_uiTuning.smallSizeIndex, m_palette.phosphorLabel,
                   chipAction->parameterName);
    }

    if (hasValue)
    {
      UpperCaseInto(m_stationOptions[_action][m_stationOptionIndex[_action]].name, upper);
      std::snprintf(buffer, sizeof(buffer), "%s \xE2\x96\xB8", upper);
      const float valueWidth = static_cast<float>(TextCellCount(buffer)) * cell;
      m_ui.AddText(_rect.Right() - cell - valueWidth, _rect.y + (_rect.height - bodyPx) * 0.5f, m_uiTuning.bodySizeIndex,
                   m_palette.phosphor, buffer);
    }
  };

  const StationAction* action = m_stationActionCount > 0 ? &m_stationActions[0] : nullptr;
  drawOptionChip(screen.formation, 0);

  /*
   * UNDOCK: the only primary action on the screen, always drawn, and disabled
   * with a reason rather than hidden.
   *
   * The reason is the authority's -- `PreCheckStation` ran
   * `ValidateStationCommand` over the same `RosterView` the server judges with,
   * so the words under a greyed button are the words the bounce would have
   * carried. That is what the print means by "a tappable UNDOCK is a promise".
   */
  const bool live = m_stationVerdict[0].accepted;
  m_ui.AddQuad(screen.undock, live ? m_palette.rule : m_palette.chipBg);
  m_ui.AddBorder(screen.undock, live ? 2.0f * scale : line, live ? m_palette.phosphor : m_palette.phosphorGhost);

  const char* verb = (action != nullptr && action->name != nullptr) ? action->name : "UNDOCK";
  if (m_stationWaves[0] > 1)
  {
    std::snprintf(buffer, sizeof(buffer), "%s \xC2\xB7 WAVE 1 OF %u", verb, m_stationWaves[0]);
  }
  else
  {
    std::snprintf(buffer, sizeof(buffer), "%s", verb);
  }

  const float verbWidth = static_cast<float>(TextCellCount(buffer)) * cell;
  m_ui.AddText(screen.undock.x + (screen.undock.width - verbWidth) * 0.5f, screen.undock.y + pad,
               m_uiTuning.bodySizeIndex, live ? m_palette.phosphorHot : m_palette.phosphorDead, buffer);

  const char* under = nullptr;
  if (live)
  {
    under = m_stationWaves[0] > 1 ? "PRESS AGAIN FOR THE REST" : nullptr;
  }
  else if (m_composer.Empty())
  {
    under = "SELECT SHIPS TO UNDOCK";
  }
  else
  {
    under = m_worldView->ReasonText(m_stationVerdict[0].reasonCode);
  }

  if (under != nullptr)
  {
    const float underWidth = static_cast<float>(TextCellCount(under)) * cell;
    m_ui.AddText(screen.undock.x + (screen.undock.width - underWidth) * 0.5f,
                 screen.undock.Bottom() - pad - smallPx, m_uiTuning.smallSizeIndex,
                 live ? m_palette.phosphorLabel : m_palette.caution, under);
  }

  /*
   * The parking diagram (ADR-017 §4).
   *
   * A readout of a promise the game made -- ships that dock park themselves,
   * deterministically, in rings around the station -- and it is on the screen
   * because a promise the player cannot see is one they have to take on trust.
   *
   * **The rings and the population are drawn; the arrangement is not claimed.**
   * Which berth a given hull ends up in is the authority's solve and this
   * client does not run it, so the dots say "this many are parked here" and
   * nothing about which is which. Square and centred rather than stretched to
   * fill, which is §7's letterbox: a ring system drawn out of round would be a
   * readout of a different arrangement.
   */
  if (screen.parking.width > 4.0f * cell)
  {
    const float cx = screen.parking.x + screen.parking.width * 0.5f;
    const float cy = screen.parking.y + screen.parking.height * 0.5f;
    const float outer = screen.parking.width * 0.42f;

    const auto ring = [&](float _radius, std::uint32_t _colour) {
      constexpr std::uint32_t SEGMENTS = 36;
      float lastX = cx + _radius;
      float lastY = cy;
      for (std::uint32_t step = 1; step <= SEGMENTS; ++step)
      {
        const float angle = 6.2831853f * static_cast<float>(step) / static_cast<float>(SEGMENTS);
        const float nextX = cx + _radius * std::cos(angle);
        const float nextY = cy + _radius * std::sin(angle);
        m_ui.AddSegment(lastX, lastY, nextX, nextY, line, _colour);
        lastX = nextX;
        lastY = nextY;
      }
    };

    ring(outer, m_palette.phosphorGhost);
    ring(outer * 0.62f, m_palette.phosphorGhost);

    const float hub = 3.0f * scale;
    m_ui.AddQuad(UiRect{cx - hub, cy - hub, hub * 2.0f, hub * 2.0f}, m_palette.phosphor);

    // One dot per docked ship, spread evenly and spilling outward, capped so a
    // full hangar stays a diagram rather than a ring of solid pixels.
    constexpr std::uint32_t MAX_DOTS = 24;
    const std::uint32_t parked = std::min<std::uint32_t>(m_stationRoster.chips, MAX_DOTS);
    const float dot = 2.0f * scale;
    for (std::uint32_t index = 0; index < parked; ++index)
    {
      const bool spilled = index >= MAX_DOTS / 2;
      const float radius = spilled ? outer : outer * 0.62f;
      const std::uint32_t onRing = MAX_DOTS / 2;
      const float angle = 6.2831853f * static_cast<float>(index % onRing) / static_cast<float>(onRing);
      const float x = cx + radius * std::cos(angle);
      const float y = cy + radius * std::sin(angle);
      m_ui.AddQuad(UiRect{x - dot, y - dot, dot * 2.0f, dot * 2.0f}, m_palette.phosphorDim);
    }

    if (m_stationRoster.chips > MAX_DOTS)
    {
      std::snprintf(buffer, sizeof(buffer), "+%u", m_stationRoster.chips - MAX_DOTS);
      m_ui.AddText(cx + outer * 0.7f, cy + outer * 0.7f, m_uiTuning.smallSizeIndex, m_palette.phosphorLabel, buffer);
    }
  }

  /*
   * The reorganisation room (ADR-017 §6), which is what the hangar is *for*
   * once the ships are inside it.
   *
   * It read SOON here until the game grew a second action, and the two controls
   * were drawn dead for a reason that had stopped being true: the note said
   * both needed ADR-012's user settings layer, "because a wing's name is
   * client-side and neither renaming nor creating one has anywhere to be
   * stored yet". Renaming does still need it. **Creating does not** -- a wing
   * exists iff a ship carries its number, so making one is a command the
   * authority already takes, and all the name has to survive is the session
   * that made it. Waiting for durable storage before allowing the gesture was
   * holding a working verb behind a feature it does not depend on.
   *
   * Drawn as the pair above it: a chip that cycles the game's values, and a
   * button that greys with the authority's own reason. Nothing here is a
   * special case -- if the game stopped offering a second action tomorrow, both
   * controls would go quiet on their own.
   */
  drawOptionChip(screen.assignWing, 1);

  const StationAction* secondary = m_stationActionCount > 1 ? &m_stationActions[1] : nullptr;
  const bool secondaryLive = m_stationVerdict[1].accepted;
  m_ui.AddQuad(screen.assign, secondaryLive ? m_palette.rule : m_palette.chipBg);
  m_ui.AddBorder(screen.assign, line, secondaryLive ? m_palette.phosphor : m_palette.phosphorGhost);

  if (secondary != nullptr && secondary->name != nullptr)
  {
    /*
     * The word the game wrote -- and the same string `CommitStationAction` puts
     * in the toast when the link drops the command, so the player reads one name
     * for one action rather than two for the same press.
     *
     * Left, where UNDOCK's is centred, because this row is a third the height
     * and has to share it with the refusal beside it.
     */
    m_ui.AddText(screen.assign.x + cell, screen.assign.y + (screen.assign.height - smallPx) * 0.5f, m_uiTuning.smallSizeIndex,
                 secondaryLive ? m_palette.phosphorHot : m_palette.phosphorDead, secondary->name);
  }

  /*
   * And why it is dark, in the authority's words -- the rule the print states
   * for UNDOCK, applied to the control beside it because a dead button with no
   * reason is the thing that rule exists to forbid.
   *
   * Nothing when the composer is empty. The primary button directly above is
   * already saying SELECT SHIPS TO UNDOCK in letters twice this size, and a
   * second copy of that sentence is noise rather than information.
   */
  if (!secondaryLive && !m_composer.Empty())
  {
    if (const char* why = m_worldView->ReasonText(m_stationVerdict[1].reasonCode); why != nullptr)
    {
      const float whyWidth = static_cast<float>(TextCellCount(why)) * cell;
      m_ui.AddText(screen.assign.Right() - cell - whyWidth, screen.assign.y + (screen.assign.height - smallPx) * 0.5f,
                   m_uiTuning.smallSizeIndex, m_palette.caution, why);
    }
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
    // The shared clock, moved here from the release top bar: debug telemetry,
    // not player information.
    m_stripReadout.serverTick = m_connection.ServerTick();
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
  // Hulls *and* their signal lamps, because the field says what the passes
  // issued and the lamp pass issues draws (ADR-006 §6a). Counting only the
  // opaque pass would make the strip quietly understate the frame the moment a
  // second world pass started drawing -- which is now.
  m_stripReadout.drawCalls = m_passes.Opaque().DrawCount() + m_passes.Lamps().DrawCount();
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

  /*
   * Whether this frame draws a world at all (ADR-020 §1).
   *
   * **No pass is added, removed, reordered or branched by this**: the loop
   * already records the list in two halves, and this chooses between two calls
   * it already makes. A full-screen surface clears the back buffer directly,
   * skips Opaque, Nebula and OverlayWorld and the resolve between them, and
   * records `Ui` -- which is where the headroom a 2,500-node map will want
   * comes from (A20).
   */
  const bool recordsWorld = SurfaceRecordsWorld(m_surfaces.Active());
  const bool msaa = m_swapChain.SampleCount() > 1 && recordsWorld;

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
  context.lamps = &m_lamps;
  context.scene = &m_scene;
  // The world's target: the MSAA offscreen when one exists, the back buffer
  // otherwise. The Ui pass never reads this -- it draws on whatever the frame
  // loop has bound by then, which after a resolve is the back buffer.
  context.renderTargetView = msaa ? m_swapChain.MsaaRenderTargetView() : m_swapChain.CurrentRenderTargetView();
  context.depthStencilView = m_swapChain.DepthStencilView();
  context.textureTable = m_textureTable;
  context.viewportWidth = m_swapChain.Width();
  context.viewportHeight = m_swapChain.Height();
  // The band the world is allowed to rasterise into, from the same resolved
  // zones the chrome is drawn from -- one answer, so "chrome always occludes
  // world" holds at every UI scale and window size rather than by coincidence.
  context.worldRect = m_uiLayout.world;
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

  if (recordsWorld)
  {
    m_passes.RecordWorld(context);
  }
  else
  {
    /*
     * The whole viewport, cleared straight onto the back buffer.
     *
     * Written here rather than reached through `ClearPass` because that pass
     * binds the depth buffer and narrows the scissor to the world's band, and
     * this frame has no depth and no band -- the surface *is* the viewport. The
     * viewport and scissor `RecordUi` relies on are set here for the same
     * reason they are set there: it inherits them rather than declaring its own.
     */
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(context.viewportWidth);
    viewport.Height = static_cast<float>(context.viewportHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor{};
    scissor.right = static_cast<LONG>(context.viewportWidth);
    scissor.bottom = static_cast<LONG>(context.viewportHeight);

    const D3D12_CPU_DESCRIPTOR_HANDLE backBufferView = m_swapChain.CurrentRenderTargetView();
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);
    m_commandList->OMSetRenderTargets(1, &backBufferView, FALSE, nullptr);
    m_commandList->ClearRenderTargetView(backBufferView, context.clearColour, 0, nullptr);
  }

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
  m_lamps.Destroy(); // Before the meshes it was built from, mirroring Initialise.
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
