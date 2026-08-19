#pragma once

#include "AudioDevice.h"
#include "ClearColour.h"
#include "ClientConfig.h"
#include "ClientConnection.h"
#include "DebugStrip.h"
#include "GlyphAtlas.h"
#include "GpuCom.h"
#include "GpuDevice.h"
#include "GpuMeshes.h"
#include "GpuNebula.h"
#include "GpuPasses.h"
#include "GpuPipelines.h"
#include "GpuSwapChain.h"
#include "CommandRow.h"
#include "GhostLane.h"
#include "GpuUploadRing.h"
#include "HudPalette.h"
#include "HudRoster.h"
#include "InputMap.h"
#include "IsoCamera.h"
#include "OrderGhost.h"
#include "OrderPuck.h"
#include "OverlayMark.h"
#include "RenderWorld.h"
#include "Selection.h"
#include "SnapshotBuffer.h"
#include "ToastStack.h"
#include "TaskPool.h"
#include "UiDrawList.h"
#include "UiLayout.h"
#include "Window.h"
#include "WorldView.h"

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
 * From S5c the world arrives through `Neuron::WorldView`, injected by the
 * composition root. This class no longer invents a scene, holds one, or knows
 * what is in it: it asks for one per frame and draws what it gets. The parked
 * fleet still exists, but it is on the other side of the seam now -- which is
 * the difference between a placeholder the engine ships and a placeholder the
 * game supplies. S7 replaced the implementation and nothing here changed, which
 * was the point of the seam. Shaders now arrive the same way, as compiled bytes
 * rather than a directory to go looking in.
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

  /*
   * The world view is borrowed, not owned, and must outlive the client -- the
   * same contract `ServerHost::Start` has with its simulation. The composition
   * root owns both (ADR-008).
   *
   * `_shaders` is borrowed on the same terms and for the same reason: the
   * engine has no opinion about which shaders a game draws with, so the
   * composition root supplies the compiled bytes. In practice they are byte
   * arrays with static storage duration, so outliving the client costs the
   * caller nothing.
   */
  [[nodiscard]] bool Initialise(const ClientConfig& _config, const PipelineShaders& _shaders, WorldView& _worldView);

  /// Runs until the window closes. Returns a process exit code.
  [[nodiscard]] int Run();

  void Shutdown();

private:
  void CreateFrameResources();
  [[nodiscard]] bool CreateContent();
  void PollNetwork();
  void UpdateCamera(float _deltaSeconds);
  /*
   * The HUD's own update: resolve the zones, lay the command row out, and let
   * it take a click before the world sees one.
   *
   * Before `UpdateSelection` in the frame, which is the whole point -- chrome
   * gets first refusal on the pointer, so pressing FORMATION does not also
   * start a box selection across the fleet underneath it.
   */
  void UpdateHud();

  void UpdateSelection();
  void UpdateOrders();
  void CommitOrder(const PuckSample& _sample, double _nowSeconds);
  void ExtractScene();

  /*
   * The `AudioUpdate` stage (ADR-011 §9): the listener from the camera, an
   * engine loop declared for every ship the frame drew, and the queued cues
   * started -- all of it on Main, immediately after Extract, and timed as its
   * own budget row.
   *
   * It reads `m_scene` rather than the snapshot, and that is deliberate: the
   * positions are the interpolated ones the renderer used, so audio and
   * visuals never disagree about where a ship is. Cutting the feed with F10
   * therefore freezes the fleet's sound exactly as it freezes the hulls, which
   * is ADR-011 §7's "F10 holds for audio too".
   */
  void AudioUpdate();

  void BuildHud();

  /*
   * The Tier-1 strip's collection and build (S14). Runs inside `BuildHud`'s
   * `Ui` span, so its cost lands in the UI row it is part of -- and measures
   * itself besides, because the print's first honesty rule is that the
   * observer effect is displayed rather than removed.
   *
   * The telemetry drain runs every frame whether or not the strip is visible:
   * the collector is what keeps the lanes from overflowing (ADR-007 §8), and a
   * drain that only ran while someone watched would report drops caused by
   * nobody watching.
   */
  void CollectDiagnostics(double _nowSeconds);

  void RenderFrame();
  void HandleResize();

  [[nodiscard]] FrameConstants BuildFrameConstants() const;
  [[nodiscard]] PassConstants BuildPassConstants() const;

  ClientConfig m_config;
  /// Copied, but the spans inside still point at the caller's arrays.
  PipelineShaders m_shaders;
  WorldView* m_worldView = nullptr;
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
  GpuNebula m_nebula;

  /// The XAudio2 graph (ADR-011). A device that would not open leaves this not
  /// ready and every call on it a no-op, so nothing downstream branches on
  /// whether the machine has speakers.
  AudioDevice m_audio;
  GpuUploadRing m_uploadRing;
  GpuPassList m_passes;

  /// The one shader-visible CBV/SRV heap (ADR-006 §12). The atlas takes slot 0
  /// and the nebula field slot 1 -- t0 and t1 in the root signature's table --
  /// with the per-frame tables the later passes need taking the rest.
  GpuPtr<ID3D12DescriptorHeap> m_srvHeap;
  D3D12_GPU_DESCRIPTOR_HANDLE m_textureTable{};

  /// Turns snapshot arrivals into a smooth render tick (ADR-002 §4). The one
  /// place the client decides *when* it is looking at; what the world looks
  /// like at that instant is the world view's answer.
  SnapshotBuffer m_snapshots;

  IsoCamera m_camera;
  CameraTuning m_cameraTuning;
  RenderScene m_scene;

  /// This frame's input, kept between `UpdateCamera` (which consumes it from
  /// the window) and `UpdateSelection` (which needs the same edges).
  InputFrame m_input;

  /// Which ships the player has, and the drag that changes it (ADR-006 §11).
  /// Client-only and never sent: the server learns what was selected only when
  /// an order names it.
  Selection m_selection;

  /// The right-drag that turns the selection into a command, and the promises
  /// made on the server's behalf until it answers (`puck-and-wheel.png` §2, §4).
  OrderPuck m_puck;
  OrderGhostList m_ghosts;

  /// Reused across frames, like the scene: polled every frame and thrown away.
  OrderFeedback m_orderFeedback;
  OrderPreview m_orderPreview;

  /// What kind of order the puck makes, asked of the game once at boot. It
  /// cannot change while a build runs -- there is one command until the wheel
  /// exists -- so asking per order would be asking the same question forever.
  OrderDefaults m_orderDefaults;

  /*
   * The parameters that kind accepts, and which one the next order will carry.
   *
   * The game's list, verbatim: numbers to send and names to show, neither
   * interpreted here (ADR-014 §2c). `CycleParameter` steps the index; the
   * command wheel's sub-ring will select from the same list in S11, which is
   * why the client holds it rather than asking per order.
   */
  OrderOption m_orderOptions[MAX_ORDER_OPTIONS] = {};
  std::uint32_t m_orderOptionCount = 0;
  std::uint32_t m_orderOptionIndex = 0;

  /*
   * The client's order counter, which the ack matches a ghost on.
   *
   * From 1, because zero means "not sent" everywhere it appears (`OrderIntent`,
   * `OrderVerdict`, the snapshot's high-water mark). Monotonic and never
   * reused; a locally refused order consumes one anyway, so a gap in the
   * sequence the server sees is normal and means nothing.
   */
  std::uint32_t m_nextOrderSeq = 1;

  /// What the selection looks like: rings and gauge bars, rebuilt each frame
  /// from the selection and the scene, and reused rather than reallocated.
  OverlayMarkList m_overlayMarks;
  OverlayTuning m_overlayTuning;

  /*
   * The HUD (ADR-006 §10). Rebuilt every frame from replicated fields and local
   * UI state and nothing else -- which is the acceptance criterion for it, not
   * a style: kill the feed and the readouts must go stale or empty rather than
   * hold their last value, because a HUD that keeps talking after the world
   * stopped is the one failure mode F10 exists to prevent.
   */
  UiDrawList m_ui;

  /// The half of the HUD that belongs *under* the hulls -- the ghost's lane, and
  /// so far only that. Drawn into the world target before the Opaque pass, so a
  /// ship standing on a lane covers it with its own silhouette rather than with
  /// a radius somebody had to guess.
  UiDrawList m_uiWorld;
  UiTuning m_uiTuning;

  /// The colour table every HUD element resolves through, chosen once at boot
  /// by `client.ui.palette`. A packed literal in `BuildHud` is a defect.
  HudPalette m_palette;

  GhostLaneTuning m_laneTuning;
  CommandRowTuning m_commandTuning;
  ToastStack m_toasts;

  /*
   * The HUD's zones, resolved once a frame in `UpdateHud` and read by both the
   * input path and the draw.
   *
   * A member rather than a local in `BuildHud` because the command row has to
   * be hit-tested *before* the frame is drawn and laid out in the same place it
   * is hit-tested from. Two resolutions -- one for the click, one for the quads
   * -- would be two chances to disagree about where a button is, which is the
   * classic HUD bug where the thing you press is not the thing you see.
   */
  UiLayout m_uiLayout;

  /// The game's commands, asked once at startup: this list does not change
  /// while a session runs, and asking every frame would imply it could.
  OrderKindOption m_orderKinds[MAX_ORDER_KINDS] = {};
  std::uint32_t m_orderKindCount = 0;

  /// Which command the puck will issue. The game's number, chosen from the list
  /// above and never invented here.
  std::uint16_t m_selectedKind = 0;

  CommandButton m_commandButtons[MAX_COMMAND_BUTTONS] = {};
  std::uint32_t m_commandButtonCount = 0;

  /// The roster's rows, asked of the game once a frame. A fixed array because
  /// the count is capped and a HUD must not allocate to describe itself.
  RosterRow m_rosterRows[MAX_ROSTER_ROWS] = {};
  std::uint32_t m_rosterRowCount = 0;

  /*
   * The Tier-1 strip (S14). `m_telemetry` is the collector's aggregate --
   * cleared, refilled from every lane and folded into the history once a
   * frame, on this thread, because the debug HUD is the collector ADR-007 §8
   * promised the lanes. Visibility starts from configuration and F1 flips it.
   */
  TelemetrySnapshot m_telemetry;
  DebugStripHistory m_stripHistory;
  DebugStripReadout m_stripReadout;
  double m_stripCostMs = 0.0;
  bool m_diagnosticsVisible = false;

  /*
   * The induced stall, toggled by F10 (`InputMap.h`).
   *
   * The one thing a loopback session cannot do on its own is stop, and the
   * whole staleness half of this client -- the STALE marker on every frozen
   * hull, the top bar's chip, the strip's SNAP age and drift -- only says
   * anything when it has. So the feed is cut here rather than in the transport:
   * the link stays up, the server keeps ticking, and what is being exercised is
   * exactly the path a lost sender would take.
   *
   * It is also the acceptance test for the rule above `m_ui`: with this on, a
   * readout that keeps talking is a readout holding its last value.
   */
  bool m_feedFrozen = false;

  GpuPtr<ID3D12CommandAllocator> m_commandAllocators[GpuSwapChain::BUFFER_COUNT];
  GpuPtr<ID3D12GraphicsCommandList> m_commandList;
  std::uint64_t m_frameFenceValues[GpuSwapChain::BUFFER_COUNT] = {};

  std::uint64_t m_frameCount = 0;
  std::int64_t m_lastFrameCounter = 0;
  std::int64_t m_lastNetLogCounter = 0;
  bool m_initialised = false;
};

} // namespace Neuron
