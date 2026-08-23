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
#include "GpuLamps.h"
#include "GpuMeshes.h"
#include "GpuNebula.h"
#include "GpuPasses.h"
#include "GpuPipelines.h"
#include "GpuSwapChain.h"
#include "CommandRow.h"
#include "GhostLane.h"
#include "GpuUploadRing.h"
#include "ContrastAudit.h"
#include "CountedChip.h"
#include "HudPalette.h"
#include "HudRoster.h"
#include "InputMap.h"
#include "Gesture.h"
#include "InputRouter.h"
#include "IsoCamera.h"
#include "MapScreen.h"
#include "OrderGhost.h"
#include "OrderPuck.h"
#include "OverlayMark.h"
#include "RenderWorld.h"
#include "RosterSelection.h"
#include "RoutePlan.h"
#include "Selection.h"
#include "SettingsScreen.h"
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
#include <string_view>
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

  /*
   * What the client is running on now, including anything the settings screen
   * changed (N3).
   *
   * The read-back path the user layer needs: `MakeClientConfig` maps a shard's
   * `AppConfig` in, this maps out, and the composition root writes what differs
   * (ADR-012 §A3). It is a `ClientConfig` rather than the file's shape because
   * this library may not know the file exists -- which is the same reason
   * `Initialise` takes one.
   *
   * Valid after `Shutdown`, which is when the composition root asks: the config
   * outlives the device.
   */
  [[nodiscard]] const ClientConfig& Config() const noexcept { return m_config; }

  /*
   * Did the player change anything on the settings screen this session?
   *
   * False lets the composition root skip the write entirely rather than compose
   * a document and discover it matches -- and *"the file records changes and not
   * state"* (ADR-012 §A3) means an unchanged session should leave the file
   * exactly as it found it, including its timestamp.
   */
  [[nodiscard]] bool SettingsChanged() const noexcept { return m_settingsDirty; }

private:
  void CreateFrameResources();
  [[nodiscard]] bool CreateContent();
  void PollNetwork();
  void UpdateCamera(float _deltaSeconds);
  /*
   * The HUD's own update: resolve the zones, lay the command row out, and let
   * it take the pointer before the world sees it.
   *
   * Before `UpdateSelection` in the frame, which is the whole point -- chrome
   * gets first refusal on the pointer, so a tap on FORMATION does not also
   * select the fleet underneath it. Since I2 that first refusal covers the
   * gesture too: `InputRouter::Gesture` reports nothing once the pointer is
   * claimed, so a tap the row took never reaches the world at all.
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
   * Reports the camera and the selection to the server (ADR-022 §4).
   *
   * Sent when it changes rather than every frame, so the retained copy below is
   * what "changed" is measured against. Quantised before comparing, because the
   * wire is quantised and a difference the server cannot see is not a change.
   */
  void SendViewFocus();

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
   * The settings surface's presses (N3).
   *
   * Its own function beside the station's for the same reason that one exists:
   * a full-screen surface owns the pointer entirely, so the branch is at the top
   * of `UpdateHud` and each surface handles what it drew rather than sharing a
   * run of rect tests with the tactical HUD underneath.
   */
  void UpdateSettingsSurface();

  /*
   * The wing rename, opened by a tap on a column header (T3, ADR-018 D15.1).
   *
   * Two functions because a text field has two lives: the frame it opens on,
   * and every frame after it in which it owns the keyboard. `BeginWingRename`
   * seeds the buffer and takes focus; `UpdateWingRename` spends the frame's
   * characters and edges and reports whether it consumed the input, so the
   * screen behind it does not also act on a tap that was only meant to finish
   * a name.
   */
  void BeginWingRename(std::uint32_t _group);
  [[nodiscard]] bool UpdateWingRename();

  /*
   * `UpdateHud`'s map half, and the only surface whose update moves a *camera*
   * (ADR-020 §7: "the graph is a viewport").
   *
   * Split out for `UpdateSettingsSurface`'s reason -- each surface handles what
   * it drew -- and it is the first consumer of `GestureState::pinchScale`
   * anywhere in this client. I1 built the pinch and nothing had a use for a
   * zoom until this screen.
   */
  void UpdateMapSurface();

  /*
   * --- the route feeder (U4, ADR-016 §8) -----------------------------------
   *
   * SET DESTINATION asks the game for a plan and captures the fleet flying it;
   * the frame loop hands over one leg at a time. Two functions because the two
   * happen at different rates -- a plan is set on a press and fed on every
   * frame after it, which is exactly the split the print's ruling produces.
   */
  void SetRouteDestination(std::uint16_t _systemId, double _nowSeconds);
  void FeedRoutePlan(double _nowSeconds);

  /*
   * What one row is currently set to, 0..1 along its control.
   *
   * The one place that knows a row index means a config field. The screen lays
   * out and hit-tests; this translates -- which is the same split
   * `RosterRow::groupId` makes, an opaque handle out and the meaning kept where
   * the state is.
   */
  [[nodiscard]] float SettingsValueOf(SettingsSection _section, std::uint32_t _item) const noexcept;

  /// And the other direction. Returns false when the press changed nothing,
  /// which is what keeps a slider dragged across its own current value from
  /// marking the layer dirty fifty times.
  [[nodiscard]] bool SetSettingsValue(SettingsSection _section, std::uint32_t _item, float _normalised);

  /// Resolves a palette name and everything derived from it. Callable again,
  /// which is ADR-020 §8's requirement and the reason it is not inline in
  /// `Create`.
  void ApplyPalette(std::string_view _name);

  /*
   * What a `Readout` row says, from the live client rather than from the config.
   *
   * The distinction is the point: a readout is on the screen because it is
   * *true* and the player would otherwise have to guess. Reporting the
   * configured value would report what was asked for, and the whole reason the
   * display section is thin is that ADR-003 §7 lets the device decide.
   *
   * Fills `_buffer` and returns it, or returns a literal -- so the caller has
   * one `const char*` either way and no ownership question.
   */
  [[nodiscard]] const char* SettingsReadoutText(SettingsSection _section, std::uint32_t _item, char* _buffer,
                                                std::size_t _bufferBytes) const noexcept;

  /*
   * Sends the composer's first wave.
   *
   * One command per press rather than the whole selection at once: the cap is
   * the *game's* and a selection past it is announced as waves before the
   * player commits (`station-screen.png` §2), so the honest gesture is one
   * press per wave with the count on the button.
   */
  /*
   * Sends one wave of one composer action.
   *
   * By index into the action list rather than one function per verb, because
   * this side does not know the verbs -- what differs between the two the game
   * offers is a number, a parameter and one flag, and all three arrive in the
   * `StationAction`. A screen that had a `CommitUndock` and a `CommitAssign`
   * would have spelled both, which is the leak ADR-020 §6 is testing for.
   */
  void CommitStationAction(std::uint32_t _action, double _nowSeconds);

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
   * The tactical HUD's own content, built only when the tactical surface is
   * live (ADR-020 §1).
   *
   * Split out of `BuildHud` so it can be *skipped*, which is what T3a left
   * owed: a full-screen surface is opaque across the viewport, so the chrome
   * this builds used to be assembled underneath one and then covered -- a
   * `UiDrawList` fill of a screen nobody was looking at. The clock is the one
   * thing it cannot re-derive from `m_uiLayout` and `m_uiTuning`, so it is the
   * one parameter: reading it again here would animate the lanes off a
   * different instant than the toasts advanced on.
   */
  void BuildTacticalHud(double _nowSeconds);

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

  /// The settings surface's draw (N3, `settings.png` §1): the rail, the section
  /// body, and the right column's preview, audit and handedness panels.
  void BuildSettingsSurface();

  /// Reads `m_mapLayout` and the runs `UpdateMapSurface` projected, and adds
  /// nothing to them: the rule that makes a hit test on this screen mean
  /// anything is that the draw takes the same rects (ADR-020 §5.1).
  void BuildMapSurface();

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

  /// The signal lamps, derived from the meshes at boot and never touched again
  /// (ADR-006 §6a). After `m_meshes` in declaration order because it is built
  /// from their bounds, and destroyed before them for the same reason.
  GpuLampTable m_lamps;
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
  std::vector<EntityId> m_liveIds;

  /// The last `ViewFocus` this client actually put on the wire. Held so a
  /// camera that has not moved and a selection that has not changed cost
  /// nothing (ADR-022 §4).
  ViewFocus m_sentFocus;

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

  /*
   * What a view switch is allowed to cost, given what the link costs
   * (ADR-018 A15, risk register R18).
   *
   * **The acceptance was written flat and the flat number was wrong.** U3b's
   * accept says *"roster click switches view to smooth motion in under half a
   * second"*, which is true at loopback RTT and an unstated assumption about
   * the network everywhere else: a switch is a request, an answer, and the
   * settle over the interpolation refill, so its floor is the round trip and
   * half a second was a target *plus* a guess. A15's correction is to
   * parameterise it, and this is the parameterisation -- one function, so the
   * number a measurement is judged against and the number a document quotes
   * cannot drift apart.
   *
   * At 200 ms of settle a 40 ms link has 240 ms to beat and a 300 ms link has
   * 500, which is the whole finding: the old flat number was not conservative,
   * it was conditional.
   */
  [[nodiscard]] static constexpr double ViewSwitchBudgetSeconds(double _roundTripMs) noexcept
  {
    return VIEW_SETTLE_SECONDS + _roundTripMs / 1000.0;
  }

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
   * What this frame's contacts meant (I1, ADR-020's 2026-08-22 amendment).
   *
   * Here rather than inside the router because a gesture spans frames and the
   * router is latched fresh every one of them: this holds where a press began
   * and how long it has been down, and hands the answer over so that every
   * stage reads the same one through the pointer claim.
   *
   * **Nothing consumes it yet, and that is I1's whole shape.** The seam is
   * built, the mouse arrives through it, and the surfaces that will read it are
   * I2's selection and I3's order surfaces. Wiring it now is what makes those
   * slices a change of consumer rather than a change of architecture.
   */
  GestureRecognizer m_gestures;
  GestureTuning m_gestureTuning;

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
   * --- the settings surface (N3, `settings.png` §1) ------------------------
   *
   * The same two members the station screen has, for the same reason: one table
   * that the layout and the hit tests both read, and one resolved layout that
   * the presses and the draw both read.
   */
  /// The counted chip's sizes (U3d-c). Its own table beside the others, because
  /// the chip is the icon ladder's rung rather than a piece of one screen -- the
  /// density ladder is its second caller when it lands.
  CountedChipTuning m_countedChipTuning;

  /*
   * --- the strategic map (U5, `strategic-map.png`, ADR-018 D14) ------------
   *
   * The one surface with a *camera* rather than only a zone table, so it keeps
   * three kinds of state: what the game said (asked once at boot, and on a
   * selection), where the player is looking, and what this frame projected.
   */
  MapScreenTuning m_mapTuning;
  MapScreenLayout m_mapLayout;
  MapRailZones m_mapRail;
  MapPanelZones m_mapPanel;

  /// The graph, asked once and held for the session -- `MapTopology`'s own
  /// lifetime rule, which is why this is a member and not a local.
  MapTopology m_mapTopology;
  MapExtent m_mapExtent;
  bool m_mapAsked = false;

  MapCamera m_mapCamera;

  /*
   * Whether the camera still owes itself a fit.
   *
   * Set on entry and spent in `UpdateMapSurface`, rather than fitting on entry
   * directly -- a caught defect: `OnSurfaceChanged` runs *before* the surface
   * has ever resolved its layout, so the first fit would be against a zero-sized
   * graph rect and produce the degenerate fallback camera. The flag moves the
   * fit to the first frame that knows how big the viewport is.
   */
  bool m_mapNeedsFit = true;

  MapPinchLevel m_mapLevel = MapPinchLevel::Region;
  MapShowFlags m_mapShow;

  /*
   * The pinch's ratio as of the last frame that applied it.
   *
   * `GestureState::pinchScale` is measured against the separation when the
   * pinch *began*, not against the last frame -- so applying it directly every
   * frame would compound it into an exponential zoom. What the camera wants is
   * this frame's ratio, which is the state's over this.
   */
  float m_mapPinchApplied = 1.0f;

  /// What the game said. All asked-once or on-a-press, never per frame.
  MapOverlayOption m_mapOverlays[MAX_MAP_OVERLAYS];
  std::uint32_t m_mapOverlayCount = 0;
  std::uint16_t m_mapOverlay = 0;
  MapLegendRow m_mapLegend[MAX_MAP_LEGEND_ROWS];
  std::uint32_t m_mapLegendCount = 0;
  MapFactRow m_mapFacts[MAX_MAP_FACT_ROWS];
  std::uint32_t m_mapFactCount = 0;
  MapRouteHop m_mapHops[MAX_MAP_ROUTE_HOPS];
  std::uint32_t m_mapHopCount = 0;
  MapRouteSummary m_mapRouteSummary;

  /// Which system the panel is about. An opaque id echoed back to the game, and
  /// a separate flag rather than a sentinel because zero is a legal id.
  std::uint16_t m_mapSelected = 0;
  bool m_mapHasSelection = false;

  /*
   * Whether SET DESTINATION would do anything: a system is picked *and* there
   * is a fleet to send.
   *
   * Resolved in `UpdateMapSurface` and read by `BuildMapSurface`, which is this
   * screen's habit for everything the two halves must agree on -- and here the
   * agreement is the whole point. The hit test refuses on the same flag the
   * draw greys on, so a button that looks dead cannot be pressed and a button
   * that looks live always does something. Two expressions would drift, and the
   * drift is invisible until a player presses a lit button and nothing happens.
   */
  bool m_mapCanRoute = false;

  /*
   * Whether VIEW would do anything: the selected system holds ships of the
   * player's that are already standing there.
   *
   * A second flag beside `m_mapCanRoute` rather than one for both, because the
   * two buttons ask different questions -- and routing a fleet to a system you
   * have nothing in is the ordinary use of this screen, so a shared flag would
   * have lit VIEW on every reachable system.
   */
  bool m_mapCanView = false;

  /// The grid VIEW would ask for, from the selected system's marker.
  std::uint16_t m_mapViewAnchor = INVALID_MAP_ANCHOR;

  /*
   * The fleet markers, and the rects this frame placed them in (U3b).
   *
   * The marks come from the game at the summary family's rate and the rects are
   * a per-frame join against the projected graph -- two lists rather than one
   * for that reason alone: re-asking the seam sixty times a second would be
   * asking a question whose answer changes once.
   */
  MapMarker m_mapMarkers[MAX_MAP_MARKERS];
  std::uint32_t m_mapMarkerCount = 0;
  MapMarkerRect m_mapMarkerRects[MAX_MAP_MARKERS];
  std::uint32_t m_mapMarkerRectCount = 0;

  /// When the markers were last asked for. They arrive at ~1 Hz, so the screen
  /// asks at that rate rather than per frame -- `MapScreen.h`'s second shape.
  double m_mapMarkersAskedAt = -1.0;

  /// The rail's two pressable runs, laid out each frame.
  MapOverlayRowRect m_mapOverlayRows[MAX_MAP_OVERLAYS];
  std::uint32_t m_mapOverlayRowCount = 0;
  MapShowRowRect m_mapShowRows[MAP_SHOW_TOGGLE_COUNT];
  std::uint32_t m_mapShowRowCount = 0;

  /// §7's declared overflow answer for this surface: the panels scroll.
  UiScrollState m_mapLegendScroll;
  UiScrollState m_mapFactScroll;
  UiScrollState m_mapRouteScroll;

  /*
   * This frame's projected graph.
   *
   * `std::vector` where every other run on this client is a fixed array, and
   * the reason is size rather than taste: at the corpus's cap these are about
   * two hundred kilobytes, and `ClientApp` is a local in the composition root.
   * They are reserved **once**, on the first entry to the surface, so a session
   * that never opens the map never pays for it and one that does allocates on a
   * screen the player asked for rather than in a frame.
   */
  std::vector<MapNodeRect> m_mapNodeRects;
  std::vector<MapLinkSegment> m_mapLinkRects;
  std::vector<MapGroupDisc> m_mapGroupRects;
  MapGraphCounts m_mapCounts;

  /// The tactical top bar's location breadcrumb, which is the way *up* to the
  /// map -- `tactical-hud.png` draws it as `◈ VESTA-3 ▸ FRONTIER 0.4` with a
  /// drill-up chevron, and this is that chevron made real.
  UiRect m_locationChipRect;

  /*
   * The route being flown, and the fleet flying it (U4).
   *
   * `m_routeFleet` is captured at SET DESTINATION rather than read from the
   * selection each leg, because a player who selects something else mid-route
   * has not changed their mind about where the *first* fleet is going -- and a
   * feeder that sent the next leg for whatever happened to be selected would
   * send it to a fleet that never agreed to go.
   *
   * A `std::vector` filled once on a press rather than a fixed array, because
   * the cap is the *game's* (`MAX_SHIPS_PER_ORDER`) and the engine may not name
   * it. One allocation per route is not the per-frame allocation the HUD rules
   * are about.
   */
  RoutePlan m_routePlan;
  std::vector<EntityId> m_routeFleet;

  /*
   * Whether the plan has a leg ready that the client cannot judge yet.
   *
   * **A hold rather than a halt, and the distinction is the honest half of this
   * feeder.** The client can only pre-check an order for ships in its own
   * scene, and every leg of a route takes the fleet off the grid this client is
   * watching. So from the second leg the fleet is simply not there to judge --
   * a fact about this client's knowledge rather than about the world, and one
   * the client establishes by looking at its own scene rather than by reading a
   * meaning into the game's reason code.
   *
   * Held, retried every frame, and stated on the HUD. What lifts it is the view
   * following the fleet, which is U3b's client half and U6's auto-follow; until
   * then a route runs while the player watches it, which is the same shape as
   * §8's already-accepted cost for a player who stops watching entirely.
   */
  bool m_routeHeld = false;

  /*
   * The wing rename's state (T3).
   *
   * `TextEditState` was built at T3a and had no consumer for a slice; this is
   * it, and it is the first editable field anywhere in this client.
   *
   * `RENAME_WIDGET` is a `WidgetId` rather than the group number, because
   * `UiFocus` is asked "is the field open" and not "which wing" -- there is one
   * field on this screen and two of them would be two carets.
   */
  static constexpr WidgetId RENAME_WIDGET = 1;
  TextEditState m_wingRename;

  /// Which group the open field is renaming, by index into the frame's groups,
  /// and the opaque handle it will hand back. Both, because the first is what
  /// the draw needs and the second is what the commit needs -- and the roster
  /// can be rebuilt between them by a summary landing mid-edit.
  std::uint32_t m_renameGroup = 0;
  std::uint16_t m_renameGroupId = 0;

  /// Where the open field is, for the blur test. Resolved from the column's own
  /// header rect in `UpdateStationSurface`, so the rect a tap is tested against
  /// is the rect the header drew -- ADR-020 §5.1 on a control that appears.
  UiRect m_renameFieldRect;

  SettingsScreenTuning m_settingsTuning;
  SettingsScreenLayout m_settingsLayout;

  /// Which section the rail has selected. Survives leaving the screen, because a
  /// player who came back to change a second audio slider did not ask to be
  /// returned to the top.
  SettingsSection m_settingsSection = SettingsSection::Accessibility;

  /// The rail and the body, laid out each frame. Fixed arrays because both runs
  /// are bounded by their own tables -- the sections by the enum, the rows by
  /// the longest section.
  SettingsNavRow m_settingsNav[SETTINGS_SECTION_COUNT];
  std::uint32_t m_settingsNavCount = 0;
  static constexpr std::uint32_t MAX_SETTINGS_ROWS = 12;
  SettingsRowRect m_settingsRows[MAX_SETTINGS_ROWS];
  std::uint32_t m_settingsRowCount = 0;

  /// The body's offset, which is §7's declared overflow answer for this surface.
  UiScrollState m_settingsScroll;

  /*
   * The contrast audit, re-measured whenever the palette is (`ApplyPalette`).
   *
   * Held rather than computed in the draw because the panel and any test read
   * the same numbers, and because ten pairs of `std::pow` every frame to draw
   * four rows is arithmetic nobody asked for.
   */
  ContrastRow m_auditRows[CONTRAST_ROW_COUNT];
  std::uint32_t m_auditRowCount = 0;

  /// Which hand holds the device. Nothing reads it yet -- I3's wheel is what
  /// will -- and it is carried now because the screen that sets it is this one
  /// and a setting with nowhere to live is a setting that gets re-invented.
  Handedness m_handedness = Handedness::Right;

  /*
   * Something on this screen changed and the user layer has not been told.
   *
   * A flag rather than a write per press, which is N2's own finding turned into
   * a rule: it writes at shutdown, and *"names are written at shutdown rather
   * than at the keystroke"* was recorded there as the first thing N3 has to
   * revisit. This is that revisit -- a slider dragged across its track is fifty
   * presses and would be fifty file writes, so the screen marks and the
   * shutdown path writes.
   */
  bool m_settingsDirty = false;

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

  /*
   * Each action's parameter values, its own -- a row per action rather than one
   * list shared by all of them.
   *
   * Shared was right while there was one action and wrong the moment there were
   * two: the primary's parameter is a formation and the secondary's is a wing,
   * and a single list would be re-asked from a different verb every frame while
   * a single index pointed into whichever answer arrived last. The player would
   * cycle to a wing and watch the formation chip change.
   */
  OrderOption m_stationOptions[MAX_STATION_ACTIONS][MAX_STATION_OPTIONS] = {};
  std::uint32_t m_stationOptionCount[MAX_STATION_ACTIONS] = {};

  /// Which value each action's chip is showing, as an index into its own row
  /// above. Cycled by pressing the chip, which is the command row's idiom for
  /// the same shape of choice.
  std::uint32_t m_stationOptionIndex[MAX_STATION_ACTIONS] = {};

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
   * This frame's answer from the authority's own validator, per action.
   *
   * The whole of the parity claim on this screen: a button greys for the reason
   * the bounce would have carried, in the same words, because it is the same
   * function (ADR-014 §3, `station-screen.png` §2). Per action because the two
   * verbs are refused for different things -- a selection can be perfectly good
   * for a regrouping and too big for one undock command -- and one verdict
   * would have greyed both for whichever question was asked last.
   */
  OrderVerdict m_stationVerdict[MAX_STATION_ACTIONS] = {};

  /// How many commands the composer's selection needs at the game's cap, and
  /// stated before the press rather than after it. Per action for the verdicts'
  /// reason: the cap is the game's answer to a verb, not to a screen.
  std::uint32_t m_stationWaves[MAX_STATION_ACTIONS] = {};

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
  /*
   * Where each visible toast's action chip was drawn, so a click can find it.
   *
   * Laid out in the build and hit-tested from the same rects, which is
   * `CommandRow`'s rule generalised (ADR-020 5.1): laying out in one place and
   * hit-testing in another is untestable by construction, because the two
   * halves never meet.
   */
  UiRect m_toastActionRects[ToastStack::MAX_VISIBLE + 1] = {};
  std::size_t m_toastActionCount = 0;

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
  std::vector<EntityId> m_groupMembers;

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
