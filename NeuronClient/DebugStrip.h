#pragma once

#include "HudPalette.h"
#include "Telemetry.h"
#include "UiDrawList.h"

#include <cstddef>
#include <cstdint>

/*
 * The Tier-1 counters strip (`debug-hud.png`, Build Order S14).
 *
 * A thin diagnostics readout over the world zone: frame time and the stage
 * budgets, network quality, the snapshot clock, replicated and drawn counts,
 * and the drop counters. Off by default, behind a setting and a key -- it
 * ships in every build, because the number a player can read in a support
 * conversation is the whole reason it exists.
 *
 * Three constraints from the print, and each is structural here:
 *
 *  - **It cannot have its own thread.** It drains the per-thread telemetry
 *    rings the engine already writes (ADR-007 §8), on the game thread, once
 *    per frame. `DebugStripHistory` is that drain's accumulator.
 *  - **It shows only fields the viewer already holds** -- link statistics,
 *    the clock estimate, last frame's pass counters. Never a world-truth
 *    count, because a diagnostic that reports what replication withheld is a
 *    wallhack with a frame counter attached.
 *  - **It is honest about itself.** Its own cost is a line item, and the
 *    telemetry lanes' drop counters are on it: an instrument that quietly
 *    discards data under load is worse than none, because it produces
 *    confident graphs of exactly the frames that went wrong.
 *
 * Split device-free on purpose: `DebugStripHistory` is arithmetic over drained
 * stats and `BuildDebugStrip` emits quads and text runs, so both halves are
 * tested without a GPU, a window or a live connection.
 */

namespace Neuron
{

/// Everything the strip prints, latched at the refresh cadence so the numbers
/// hold still long enough to read. Filled half by `DebugStripHistory` (the
/// telemetry-derived rows) and half by the client (link, clock and pass
/// counters it already holds).
struct DebugStripReadout
{
  // Frame timing, milliseconds. `frameP95Ms` is over the recent window.
  double frameMs = 0.0;
  double frameP95Ms = 0.0;
  double fps = 0.0;
  double gameMs = 0.0;
  double extractMs = 0.0;
  /// The `AudioUpdate` stage (ADR-011 §9), the fifth budget row.
  double audioMs = 0.0;
  double renderMs = 0.0;
  double uiMs = 0.0;
  double netMs = 0.0;

  /// The sim thread's tick, when its lane lives in this process (host mode).
  /// Negative means no Sim lane was seen and the row is not drawn.
  double simTickMs = -1.0;

  /// The strip's own cost last frame -- collection plus build. Honesty rule 1.
  double stripMs = 0.0;

  // The link.
  bool joined = false;
  double rttMs = 0.0;
  double minRttMs = 0.0;
  std::uint64_t controlResends = 0;
  std::uint64_t datagramsDropped = 0;

  /// The server's tick as last heard -- the shared clock (ADR-002 §1). Debug
  /// telemetry, not player information, which is why it lives here and not on
  /// the release top bar (`tactical-hud.png` keeps the top bar for the player).
  std::uint32_t serverTick = 0;

  // The snapshot clock (ADR-002 §4).
  bool hasEstimate = false;
  bool stale = false;
  double snapAgeMs = 0.0;
  double driftTicks = 0.0;
  std::uint64_t outOfOrder = 0;

  // What the frame held and what the passes issued -- viewer-held, never
  // world truth: `replicated` is the scene the game handed over, not the
  // server's ship table.
  std::uint32_t replicated = 0;
  std::uint32_t drawnInstances = 0;
  std::uint32_t drawCalls = 0;
  std::uint32_t glyphQuads = 0;

  /// Live source voices and what the two pools could ever hand out (ADR-011
  /// §3). Zero capacity is a client running silently.
  std::uint32_t audioVoices = 0;
  std::uint32_t audioVoiceCap = 0;

  // The drop counters -- the measurement about the measurement.
  std::uint64_t telemetryDrops = 0;
  std::uint64_t snapshotDrops = 0;
  std::uint64_t uploadDrops = 0;
  std::uint64_t missingGlyphs = 0;
  std::uint64_t tickOverruns = 0;
};

/*
 * The rolling half: fed one drained `TelemetrySnapshot` per frame, it keeps a
 * short window of stage times and latches display values every quarter
 * second -- a number that changes 144 times a second is a flicker, not a
 * reading. Counter totals accumulate monotonically, because a drop counter
 * that reset with the window would spend most of its life claiming zero.
 */
class DebugStripHistory
{
public:
  /// How often the displayed numbers move. A quarter second is fast enough to
  /// watch a stall arrive and slow enough to read while it does.
  static constexpr double REFRESH_SECONDS = 0.25;

  /// Frame samples kept for the p95. At 144 Hz this is most of two seconds.
  static constexpr std::size_t FRAME_WINDOW = 256;

  /// Accumulates one frame's drained stats. `_drained` is the collector's
  /// aggregate for exactly one frame -- clear it, drain every lane, hand it in.
  void AccumulateFrame(const TelemetrySnapshot& _drained, double _nowSeconds);

  /// Writes the telemetry-derived rows into a readout: the stage times, fps,
  /// p95, the sim tick if a Sim lane reported, and the overrun/upload-drop
  /// totals. The client fills the rest from state it already holds.
  void FillTimings(DebugStripReadout& _out) const;

private:
  // One slot per named stage row. `Tick` is the Sim lane's and may never fire.
  enum Stage : std::size_t
  {
    StageFrame = 0,
    StageGame,
    StageExtract,
    StageAudio,
    StageRender,
    StageUi,
    StageNet,
    StageTick,
    STAGE_COUNT
  };

  void Latch(double _nowSeconds);

  double m_windowMs[STAGE_COUNT] = {};
  std::uint32_t m_windowStageFrames[STAGE_COUNT] = {};
  std::uint32_t m_windowFrames = 0;
  double m_windowStartSeconds = 0.0;
  bool m_windowStarted = false;

  double m_displayMs[STAGE_COUNT] = {};
  bool m_displayStageSeen[STAGE_COUNT] = {};
  double m_displayFps = 0.0;
  double m_displayP95Ms = 0.0;

  float m_frameSamples[FRAME_WINDOW] = {};
  std::size_t m_frameSampleCount = 0;
  std::size_t m_frameSampleNext = 0;

  std::uint64_t m_tickOverruns = 0;
  std::uint64_t m_uploadDrops = 0;
};

/// Where and how big. The strip goes top-left of the world zone (the print
/// forces placement rather than choosing it); everything scales with the HUD.
struct DebugStripStyle
{
  float x = 0.0f;
  float y = 0.0f;
  float scale = 1.0f;
  float cellPixels = 8.0f;      // The monospace cell, same as the HUD's.
  float smallLinePixels = 13.0f; // The small face's height at this scale.
  std::uint8_t smallSizeIndex = 0;
  std::uint8_t bodySizeIndex = 1;
};

/// Emits the strip into the HUD's own draw list -- it is chrome like any other
/// panel, drawn by the same pass, costing no pipeline and no pixel of its own
/// machinery. Returns the panel's rect so a caller (or a test) knows what it
/// covered.
UiRect BuildDebugStrip(const DebugStripReadout& _readout, const DebugStripStyle& _style, const HudPalette& _palette,
                       UiDrawList& _ui);

} // namespace Neuron
