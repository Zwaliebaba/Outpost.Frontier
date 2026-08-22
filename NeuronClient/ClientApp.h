#pragma once

#include "ApproachChain.h"
#include "AutoFollow.h"
#include "EntityTransits.h"
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
#include "InputRouter.h"
#include "IsoCamera.h"
#include "OrderGhost.h"
#include "OrderPuck.h"
#include "OverlayMark.h"
#include "RenderWorld.h"
#include "RosterSelection.h"
#include "Selection.h"
#include "StationScreen.h"
#include "StationView.h"
#include "SurfaceStack.h"
#include "SnapshotBuffer.h"
#include "TextEditState.h"
#include "UiFocus.h"
#include "ToastStack.h"
#include "TaskPool.h"
#include "UiDrawList.h"
#include "UiLayout.h"
#include "UiScrollState.h"
#include "Window.h"
#include "WorldView.h"

#include <cstdint>
#include <vector>

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

  /*
   * Starts a context action if the release landed on something that affords
   * one. Returns true when it did, in which case the ordinary order the gesture
   * would otherwise have composed is not sent.
   */
  [[nodiscard]] bool BeginContextAction(const PuckSample& _sample, double _nowSeconds);

  /// Sends the chained verb once the pre-check stops refusing it -- the same
  /// function the authority judges with, which is what makes "close enough"
  /// one definition rather than two (ADR-014 3).
  void AdvanceApproach(double _nowSeconds);

  /*
   * What the client throws away when the feed points somewhere else
   * (ADR-016 9's ~200 ms settle).
   *
   * A grid switch is the one event that changes *every* id on screen at once,
   * and almost everything this class keeps between frames is keyed on an id or
   * on a comparison with the last frame. None of it survives the crossing, and
   * the ones that would survive *wrongly* are the dangerous ones.
   */
  void OnViewChanged(std::uint16_t _gridAnchor, double _nowSeconds);

  /*
   * Runs one navigation's exit and entry (ADR-020 §1).
   *
   * Takes the change rather than the two ids because a navigation that went
   * nowhere -- pressing STATION with the hangar already up -- must run neither,
   * and a caller that had to check `changed` itself would eventually not.
   */
  void OnSurfaceChanged(const SurfaceChange& _change);

  /*
   * The station surface's frame: what the game says, where it goes, and what
   * the player just did to it (ADR-020 §5.1).
   *
   * `UpdateHud`'s station half, split out rather than inlined because the
   * tactical half already fills that function and because this one owns the
   * whole pointer while it runs -- there is no world underneath to fall
   * through to.
   */
  void UpdateStationSurface();

  /*
   * Sends the composer's first wave.
   *
   * One command per press rather than the whole selection at once: the cap is
   * the *game's* and a selection past it is announced as waves before the
   * player commits (`station-screen.png` §2), so the honest gesture is one
   * press per wave with the count on the button.
   */
  void CommitUndock(double _nowSeconds);

  /*
   * Follows the fleet when the grid being watched stops holding any of it
   * (ADR-016 §9's auto-follow).
   *
   * The whole policy is one sentence: **a player watching a place they have
   * nothing at is watching the wrong place.** That is a presentation decision
   * and it lives here; where the ships *are* is the game's answer, arriving as
   * location blocks, and this file never works out what a warp is.
   */
  void AdvanceAutoFollow();

  /// The slot in `m_orderKinds` holding this kind value, or `m_orderKindCount`
  /// when the game never listed it -- the per-kind option tables are indexed by
  /// slot, never by the opaque kind number.
  [[nodiscard]] std::uint32_t KindSlot(std::uint16_t _kind) const noexcept;
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
   * A full-screen surface's own content (ADR-020 §1).
   *
   * The hangar's, for now, and deliberately thin: the ground, the way back, and
   * which place this is. Its tab row and the roster behind it are the hangar's
   * own slice, and they need a seam call before they can exist at all -- the
   * engine may not learn that a tab is called REFIT (ADR-020 §6), so the words
   * have to arrive as data.
   */
  void BuildStationSurface();

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

  /*
   * The chained half of a context action (ADR-017 2): the fleet is flying at
   * something, and the verb goes the moment the authority would take it.
   *
   * One at a time on purpose. A second approach replaces the first, because
   * two chips promising two different arrivals is a HUD saying something the
   * player did not ask for.
   */
  ApproachChain m_approach;

  /// Scratch for the approach's liveness check, kept rather than made each
  /// frame: this runs at display rate.
  std::vector<std::uint16_t> m_liveIds;

  /// Reused across frames, like the scene: polled every frame and thrown away.
  OrderFeedback m_orderFeedback;
  OrderPreview m_orderPreview;

  /// What kind of order the puck makes, asked of the game once at boot. It
  /// cannot change while a build runs -- there is one command until the wheel
  /// exists -- so asking per order would be asking the same question forever.
  OrderDefaults m_orderDefaults;

  /*
   * Every kind's parameters, and which one each kind will carry next.
   *
   * The game's lists, verbatim: numbers to send and names to show, neither
   * interpreted here (ADR-014 §2c). Asked once per kind at boot -- the lists
   * are as static as the kind list itself -- and held **per kind** rather than
   * for the selected one only, because the context bar states every standing
   * parameter at once (`STANCE AGGRESSIVE ▸ FORMATION LINE` on the print) and
   * a readout that only existed while its verb was selected would blank the
   * moment the player reached for MOVE.
   *
   * All indexed by the kind's *slot* in `m_orderKinds`, not by the kind value:
   * the value is the game's opaque number, and using it as an index would be
   * assuming the game numbers its commands densely from zero.
   */
  OrderOption m_kindOptions[MAX_ORDER_KINDS][MAX_ORDER_OPTIONS] = {};
  std::uint32_t m_kindOptionCounts[MAX_ORDER_KINDS] = {};

  /// Which option each kind currently has chosen, and which one the game calls
  /// its default -- the summary row draws a non-default value in caution amber.
  std::uint32_t m_kindOptionIndex[MAX_ORDER_KINDS] = {};
  std::uint32_t m_kindDefaultIndex[MAX_ORDER_KINDS] = {};

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
   * How long after a grid switch the client stops reacting to the scene
   * changing under it (ADR-016 9).
   *
   * **The window exists because the two signals race.** `ViewChanged` is
   * reliable and ordered; snapshots are datagrams, so the first frame of the
   * new grid can arrive on either side of the notice that the grid changed. A
   * single frame caught on the wrong side of that race is a scene where every
   * id vanished and forty new ones appeared, which is what the transit fades
   * exist to notice -- and would report as forty ships leaving and forty
   * arriving. Nobody went anywhere. The camera moved.
   *
   * Two hundred milliseconds is the interpolation buffer's own refill time
   * rather than a number picked to feel right: by the time it has passed, the
   * view holds frames from the new grid on both sides of the render tick and
   * every id on screen belongs to it.
   */
  static constexpr double VIEW_SETTLE_SECONDS = 0.2;

  /// When the current settle ends, or negative when the feed is not settling.
  double m_settleUntilSeconds = -1.0;

  /*
   * The grid auto-follow has already asked for, so it asks once.
   *
   * A view request is reliable and answered, but the answer takes a round trip
   * and the location blocks do not change while it is in flight -- so without
   * this the condition stays true and the client asks again every frame until
   * the reply lands. `INVALID_FOLLOW_ANCHOR` means nothing is in flight; the
   * request clears it when `ViewChanged` arrives, accepted **or** refused,
   * because a refusal is an answer and asking again would be a loop with a
   * toast on every turn of it.
   */
  static constexpr std::uint16_t INVALID_FOLLOW_ANCHOR = NO_FOLLOW_TARGET;
  std::uint16_t m_followRequested = INVALID_FOLLOW_ANCHOR;

  /// The grid a refused follow named, so the same one is not asked for again
  /// the moment the reply clears the request above.
  std::uint16_t m_followRefused = INVALID_FOLLOW_ANCHOR;

  /// Which ships arrived and which left, and when (ADR-017 4). Fed from the
  /// scene rather than from anything the game says, because an id appearing and
  /// disappearing is a fact about the scene and the engine is allowed to know
  /// its own scene.
  EntityTransitList m_transits;

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

  /*
   * The `▥ MENU` chip and its stub list (RESUME · SETTINGS · EXIT).
   *
   * The chip must exist now even though the surfaces behind it do not: a HUD
   * with no menu affordance is a dead end on a tablet with no Escape key
   * (`tactical-hud.png`, session-surfaces). The rects are resolved in
   * `UpdateHud` and drawn in `BuildHud` for the same reason `m_uiLayout` is a
   * member -- one answer for the click and the quads.
   */
  static constexpr std::uint32_t MENU_RESUME = 0;
  static constexpr std::uint32_t MENU_SETTINGS = 1;
  static constexpr std::uint32_t MENU_EXIT = 2;
  static constexpr std::uint32_t MENU_ITEM_COUNT = 3;
  UiRect m_menuButtonRect;
  UiRect m_menuItemRects[MENU_ITEM_COUNT] = {};
  bool m_menuOpen = false;

  /// The `◀ TACTICAL` chip on a full-screen surface, resolved in `UpdateHud`
  /// and drawn from the same rect -- one control, because ◀ TACTICAL and ◀ BACK
  /// are one mechanism (ADR-020 §1).
  UiRect m_backChipRect;

  /*
   * Which screen is live, who owns the keyboard, and who got this frame's
   * input (ADR-020 §1-§3).
   *
   * The router replaces `m_uiConsumedPress`, which said the one thing a client
   * with a single surface needed to say -- "this press landed on chrome above
   * the world" -- and had no way to say the other two: that a wheel notch was
   * spoken for while the click was not, and that a key belongs to a field
   * rather than to the camera.
   */
  SurfaceStack m_surfaces{SurfaceId::Tactical};
  UiFocus m_focus;
  InputRouter m_router;

  /*
   * Which station the hangar was opened for.
   *
   * The game's anchor, echoed from the block that was pressed and never read
   * here. It is client state rather than a request: the roster for every place
   * holding this commander's ships is already replicated (ADR-017 §8), so
   * opening a remote hangar asks the authority for nothing -- which is what
   * makes "the screen opens for any station holding your ships, viewed or not"
   * true without a round trip.
   */
  std::uint16_t m_stationAnchor = 0;

  /// The station screen's sizes, and the one table its layout and its hit tests
  /// both read.
  StationScreenTuning m_stationTuning;

  /// Where its zones landed this frame. Resolved once, read by the presses and
  /// by the draw.
  StationScreenLayout m_stationLayout;

  /*
   * What the player has picked out of the roster.
   *
   * Session lifetime, reconciled on every look (ADR-017 §6a.2): re-picking
   * thirty ships after one glance at the map is a real cost on the one screen
   * whose whole purpose is composing selections, and `Reconcile` is what makes
   * keeping it safe.
   */
  RosterSelection m_composer;

  /// The wing columns' shared offset. One for the panel rather than one per
  /// column -- two wings scrolling out of step would put a chip beside the
  /// wrong header.
  UiScrollState m_stationScroll;

  /*
   * The game's words for this station, asked once a frame while the surface is
   * up. Every one of them is drawn and none is read: the tab row's words, the
   * wings' names, the hull classes and the action's verb are all the game's
   * (ADR-020 §6's leak test).
   */
  StationTab m_stationTabs[MAX_STATION_TABS] = {};
  std::uint32_t m_stationTabCount = 0;
  StationGroup m_stationGroups[MAX_STATION_GROUPS] = {};
  StationChip m_stationChips[MAX_STATION_CHIPS] = {};
  StationRosterCounts m_stationRoster;

  StationAction m_stationActions[MAX_STATION_ACTIONS] = {};
  std::uint32_t m_stationActionCount = 0;
  OrderOption m_stationOptions[MAX_ORDER_OPTIONS] = {};
  std::uint32_t m_stationOptionCount = 0;

  /// Which formation UNDOCK will leave in, as an index into the options above.
  /// Cycled by pressing the chip, which is the command row's idiom for the
  /// same shape of choice.
  std::uint32_t m_stationOptionIndex = 0;

  /// Which tab is live. The game's number, echoed from the tab that was
  /// pressed -- only the hangar has content today, and the client cannot tell.
  std::uint16_t m_stationTab = 0;

  /// Where the screen's controls landed this frame.
  StationTabButton m_stationTabButtons[MAX_STATION_TABS] = {};
  std::uint32_t m_stationTabButtonCount = 0;
  StationColumnRect m_stationColumns[MAX_STATION_GROUPS] = {};
  StationChipRect m_stationChipRects[MAX_STATION_CHIPS] = {};
  StationRosterCounts m_stationLaid;

  /*
   * This frame's answer from the authority's own validator, for UNDOCK's face.
   *
   * The whole of the parity claim on this screen: the button greys for the
   * reason the bounce would have carried, in the same words, because it is the
   * same function (ADR-014 §3, `station-screen.png` §2).
   */
  OrderVerdict m_undockVerdict;

  /// How many commands the composer's selection needs at the game's cap, and
  /// stated before the press rather than after it.
  std::uint32_t m_undockWaves = 0;

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

  /// The column's own sizes, and the one table both halves of it read.
  RosterColumnTuning m_rosterTuning;

  /*
   * Scratch for the ids behind one roster row, refilled on the press that asks
   * for them.
   *
   * A member rather than a local for `m_liveIds`' reason -- a press should not
   * allocate -- and sized from the row's own `shipCount`, which is the game's
   * count of that wing in this frame. Exact by construction, so the seam call
   * can never truncate and no cap has to be invented for it.
   */
  std::vector<std::uint16_t> m_groupMembers;

  /// Where the selected ships are, for the camera to frame. A member for
  /// `m_groupMembers`' reason: a press should not allocate.
  std::vector<DirectX::XMFLOAT2> m_focusPoints;

  /*
   * Where the ELSEWHERE blocks landed this frame.
   *
   * Resolved in `UpdateHud` and read by both the press and the draw, which is
   * the same single-answer rule `m_uiLayout` and the menu rects exist for --
   * and it stopped being optional when the button on these blocks became the
   * way into the hangar (ADR-020 §5.1).
   */
  LocationBlockLayout m_locationLayouts[MAX_LOCATION_BLOCKS] = {};
  std::uint32_t m_locationLayoutCount = 0;

  /// The other list: where the player's ships are when they are nowhere the
  /// scene can show them -- docked, on another grid, or mid-warp (ADR-017 1,
  /// ADR-016 9). Refilled with the roster rows, because the two are one answer
  /// about one fleet and a frame that rebuilt only half of it could show a ship
  /// in both places at once.
  LocationBlock m_locationBlocks[MAX_LOCATION_BLOCKS] = {};
  std::uint32_t m_locationBlockCount = 0;
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
