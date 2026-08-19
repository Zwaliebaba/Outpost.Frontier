#include "pch.h"

#include "DebugStrip.h"

#include <algorithm>
#include <cstdio>

namespace Neuron
{
namespace
{

/*
 * The stage rows, by the lane names the engine already records (ADR-007 §8).
 * This is the whole coupling between the strip and the frame loop: a stage is
 * on the strip because a NEURON_SPAN with this name exists, and retuning the
 * strip never touches the loop.
 */
constexpr const char* STAGE_NAMES[] = {"Frame", "Game", "Extract", "Audio", "Render", "Ui", "Net", "Tick"};

/*
 * Per-stage budgets, milliseconds, for the bars -- the print draws each stage
 * against its own allowance rather than against the frame. Tuning numbers in
 * the corpus's spirit: the four client stages share a 144 Hz frame with the
 * present, and the strip's job is to show which one is eating it. Over budget
 * is a colour change, not a clamp: a bar pinned at 100% hides how far over.
 */
constexpr double STAGE_BUDGET_MS[] = {6.9, 2.0, 0.6, 0.4, 2.5, 0.7, 0.5, 50.0};

static_assert(sizeof(STAGE_BUDGET_MS) / sizeof(STAGE_BUDGET_MS[0]) == sizeof(STAGE_NAMES) / sizeof(STAGE_NAMES[0]),
              "every stage row has a budget");

/// The upload-ring drop counters, one per pass that can drop its payload. The
/// names are the NEURON_COUNTER literals in GpuPasses.cpp.
constexpr const char* UPLOAD_DROP_NAMES[] = {"OpaqueInstanceDrops", "OverlayMarkDrops", "UiInstanceDrops"};

} // namespace

void DebugStripHistory::AccumulateFrame(const TelemetrySnapshot& _drained, double _nowSeconds)
{
  if (!m_windowStarted)
  {
    m_windowStarted = true;
    m_windowStartSeconds = _nowSeconds;
  }
  ++m_windowFrames;

  for (std::size_t stage = 0; stage < STAGE_COUNT; ++stage)
  {
    const TelemetryStat* stat = _drained.Find(STAGE_NAMES[stage]);
    if (stat == nullptr || stat->kind != TelemetryKind::Span || stat->count == 0)
    {
      continue;
    }
    const double ms = TelemetrySnapshot::Milliseconds(stat->total);
    m_windowMs[stage] += ms;
    ++m_windowStageFrames[stage];

    if (stage == StageFrame)
    {
      // One sample per drain -- the mean when a hiccup batched two frames'
      // spans into one drain, which is also the honest reading of that case.
      m_frameSamples[m_frameSampleNext] = static_cast<float>(ms / stat->count);
      m_frameSampleNext = (m_frameSampleNext + 1) % FRAME_WINDOW;
      m_frameSampleCount = std::min(m_frameSampleCount + 1, FRAME_WINDOW);
    }
  }

  // Counters accumulate for the strip's whole life: a drop that happened is a
  // drop that happened, and a counter that reset with the display window would
  // spend most of its life claiming zero.
  if (const TelemetryStat* overruns = _drained.Find("TickOverrun"); overruns != nullptr && overruns->kind == TelemetryKind::Counter)
  {
    m_tickOverruns += static_cast<std::uint64_t>(std::max<std::int64_t>(0, overruns->total));
  }
  for (const char* name : UPLOAD_DROP_NAMES)
  {
    if (const TelemetryStat* drops = _drained.Find(name); drops != nullptr && drops->kind == TelemetryKind::Counter)
    {
      m_uploadDrops += static_cast<std::uint64_t>(std::max<std::int64_t>(0, drops->total));
    }
  }

  if (_nowSeconds - m_windowStartSeconds >= REFRESH_SECONDS)
  {
    Latch(_nowSeconds);
  }
}

void DebugStripHistory::Latch(double _nowSeconds)
{
  const double elapsed = _nowSeconds - m_windowStartSeconds;

  for (std::size_t stage = 0; stage < STAGE_COUNT; ++stage)
  {
    m_displayStageSeen[stage] = m_windowStageFrames[stage] > 0;
    if (m_displayStageSeen[stage])
    {
      m_displayMs[stage] = m_windowMs[stage] / m_windowStageFrames[stage];
    }
    m_windowMs[stage] = 0.0;
    m_windowStageFrames[stage] = 0;
  }

  m_displayFps = elapsed > 0.0 ? m_windowFrames / elapsed : 0.0;
  m_windowFrames = 0;
  m_windowStartSeconds = _nowSeconds;

  if (m_frameSampleCount > 0)
  {
    // A copy, because nth_element reorders and the ring is still accumulating.
    float sorted[FRAME_WINDOW];
    std::copy_n(m_frameSamples, m_frameSampleCount, sorted);
    const std::size_t rank = (m_frameSampleCount * 95) / 100;
    std::nth_element(sorted, sorted + rank, sorted + m_frameSampleCount);
    m_displayP95Ms = sorted[rank];
  }
}

void DebugStripHistory::FillTimings(DebugStripReadout& _out) const
{
  _out.frameMs = m_displayMs[StageFrame];
  _out.frameP95Ms = m_displayP95Ms;
  _out.fps = m_displayFps;
  _out.gameMs = m_displayMs[StageGame];
  _out.extractMs = m_displayMs[StageExtract];
  _out.audioMs = m_displayMs[StageAudio];
  _out.renderMs = m_displayMs[StageRender];
  _out.uiMs = m_displayMs[StageUi];
  _out.netMs = m_displayMs[StageNet];
  _out.simTickMs = m_displayStageSeen[StageTick] ? m_displayMs[StageTick] : -1.0;
  _out.tickOverruns = m_tickOverruns;
  _out.uploadDrops = m_uploadDrops;
}

UiRect BuildDebugStrip(const DebugStripReadout& _readout, const DebugStripStyle& _style, const HudPalette& _palette,
                       UiDrawList& _ui)
{
  const float cell = _style.cellPixels;
  const float pad = 8.0f * _style.scale;
  const float lineStep = _style.smallLinePixels + 4.0f * _style.scale;

  // Sized by its columns rather than measured: the face is monospace, so the
  // widest row is a character count and the panel is that many cells.
  constexpr float PANEL_CELLS = 58.0f;
  const bool hasSimRow = _readout.simTickMs >= 0.0;
  // Title, FRAME, six stages, LINK, SNAP, [SIM], WORLD, DROPS. The stage rows
  // gained AUDIO in S15 and the WORLD row gained the voice count with it, which
  // is what widened the panel.
  const float rowCount = hasSimRow ? 13.0f : 12.0f;
  const UiRect panel{_style.x, _style.y, PANEL_CELLS * cell + 2.0f * pad, rowCount * lineStep + 2.0f * pad};

  _ui.AddQuad(panel, _palette.panel);
  _ui.AddBorder(panel, 1.0f * _style.scale, _palette.border);

  char buffer[96] = {};
  float rowY = panel.y + pad;
  const float textX = panel.x + pad;

  // The title row carries the strip's own cost -- honesty rule 1: the observer
  // effect cannot be removed, so it is displayed where it cannot be missed.
  _ui.AddText(textX, rowY, _style.smallSizeIndex, _palette.phosphorLabel, "DIAGNOSTICS - TIER 1");
  std::snprintf(buffer, sizeof(buffer), "dbg %.2f ms", _readout.stripMs);
  _ui.AddText(panel.Right() - pad - static_cast<float>(TextCellCount(buffer)) * cell, rowY, _style.smallSizeIndex,
              _palette.phosphorGhost, buffer);
  rowY += lineStep;

  // FRAME leads with the number the print makes the headline.
  std::snprintf(buffer, sizeof(buffer), "FRAME   %6.2f ms   %4.0f FPS   p95 %5.2f", _readout.frameMs, _readout.fps,
                _readout.frameP95Ms);
  _ui.AddText(textX, rowY, _style.smallSizeIndex, _palette.phosphorHot, buffer);
  rowY += lineStep;

  // The four client stage rows, each with a bar against its own budget. The
  // bar's track is always drawn, so "this stage cost nothing" is a visibly
  // empty bar rather than a missing one.
  struct StageRow
  {
    const char* name;
    double ms;
    double budgetMs;
  };
  const StageRow stageRows[] = {
      // The indices are STAGE_NAMES' own order, minus `Frame` at 0 and `Tick`
      // at the end -- those two are not stage bars.
      {"GAME", _readout.gameMs, STAGE_BUDGET_MS[1]},
      {"EXTRACT", _readout.extractMs, STAGE_BUDGET_MS[2]},
      {"AUDIO", _readout.audioMs, STAGE_BUDGET_MS[3]},
      {"RENDER", _readout.renderMs, STAGE_BUDGET_MS[4]},
      {"UI", _readout.uiMs, STAGE_BUDGET_MS[5]},
      {"NET", _readout.netMs, STAGE_BUDGET_MS[6]},
  };
  const float barX = textX + 18.0f * cell;
  const float barWidth = PANEL_CELLS * cell - 18.0f * cell - pad;
  for (const StageRow& row : stageRows)
  {
    std::snprintf(buffer, sizeof(buffer), "%-8s%6.2f ms", row.name, row.ms);
    _ui.AddText(textX, rowY, _style.smallSizeIndex, _palette.phosphorBody, buffer);

    const float barY = rowY + _style.smallLinePixels * 0.25f;
    const float barHeight = _style.smallLinePixels * 0.5f;
    _ui.AddQuad(UiRect{barX, barY, barWidth, barHeight}, _palette.trackHull);
    const double fraction = row.budgetMs > 0.0 ? row.ms / row.budgetMs : 0.0;
    const auto fill = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    // Over budget changes the colour rather than the length: a bar pinned at
    // full says "at the line", the amber says "past it".
    _ui.AddQuad(UiRect{barX, barY, barWidth * fill, barHeight}, fraction > 1.0 ? _palette.caution : _palette.phosphor);
    rowY += lineStep;
  }

  // The link. Loss on the reliable channel is resends; on the datagram path it
  // is drops -- both shown, neither renamed into a percentage the transport
  // never measured.
  if (_readout.joined)
  {
    // The tick leads the row: it moved here from the release top bar, which is
    // for the player -- the shared clock is a debugging fact about the link.
    std::snprintf(buffer, sizeof(buffer), "LINK    tick %u   rtt %5.1f ms   min %5.2f   resend %llu   drop %llu",
                  _readout.serverTick, _readout.rttMs, _readout.minRttMs,
                  static_cast<unsigned long long>(_readout.controlResends),
                  static_cast<unsigned long long>(_readout.datagramsDropped));
    _ui.AddText(textX, rowY, _style.smallSizeIndex, _palette.phosphorBody, buffer);
  }
  else
  {
    _ui.AddText(textX, rowY, _style.smallSizeIndex, AtHalfAlpha(_palette.neutral), "LINK    no session");
  }
  rowY += lineStep;

  // The snapshot clock. STALE is spelled out, in the caution colour, because
  // this row existing is most of why the marker can be trusted: the number the
  // per-ship marker summarises is readable here.
  if (_readout.hasEstimate)
  {
    std::snprintf(buffer, sizeof(buffer), "SNAP    age %4.0f ms   drift %+5.2f t   ooo %llu", _readout.snapAgeMs,
                  _readout.driftTicks, static_cast<unsigned long long>(_readout.outOfOrder));
    _ui.AddText(textX, rowY, _style.smallSizeIndex, _readout.stale ? _palette.caution : _palette.phosphorBody, buffer);
    if (_readout.stale)
    {
      _ui.AddText(panel.Right() - pad - 5.0f * cell, rowY, _style.smallSizeIndex, _palette.caution, "STALE");
    }
  }
  else
  {
    _ui.AddText(textX, rowY, _style.smallSizeIndex, AtHalfAlpha(_palette.neutral), "SNAP    no feed");
  }
  rowY += lineStep;

  // The sim row appears only when a Sim lane lives in this process -- host
  // mode. In client mode the row is absent rather than zero, because zero
  // would claim a measurement of a machine this process cannot see.
  if (hasSimRow)
  {
    std::snprintf(buffer, sizeof(buffer), "SIM     tick %5.2f ms   overrun %llu", _readout.simTickMs,
                  static_cast<unsigned long long>(_readout.tickOverruns));
    _ui.AddText(textX, rowY, _style.smallSizeIndex,
                _readout.tickOverruns > 0 ? _palette.caution : _palette.phosphorBody, buffer);
    rowY += lineStep;
  }

  // Viewer-held counts only: replicated is what the game handed the frame,
  // drawn is what the opaque pass issued. A gap between the two is a mesh or
  // mapping problem, and having both on one row is what makes it visible.
  //
  // The voice count rides here for the same reason: it is what the frame is
  // *holding*, and it is the only way to see the pool's cap doing its job --
  // a fleet of forty asking for engines and the mixer carrying its 32.
  std::snprintf(buffer, sizeof(buffer), "WORLD   %u repl   %u drawn   %u draws   %u glyphs   %u/%u voices",
                _readout.replicated, _readout.drawnInstances, _readout.drawCalls, _readout.glyphQuads,
                _readout.audioVoices, _readout.audioVoiceCap);
  _ui.AddText(textX, rowY, _style.smallSizeIndex, _palette.phosphorBody, buffer);
  rowY += lineStep;

  // Honesty rule 2: the drop counters, including the strip's own feed. When a
  // ring overflows the strip says how many samples it lost rather than drawing
  // confident graphs of the frames that went missing.
  const bool dropped = _readout.telemetryDrops + _readout.snapshotDrops + _readout.uploadDrops + _readout.missingGlyphs > 0;
  std::snprintf(buffer, sizeof(buffer), "DROPS   tele %llu   snap %llu   upload %llu   glyph %llu",
                static_cast<unsigned long long>(_readout.telemetryDrops), static_cast<unsigned long long>(_readout.snapshotDrops),
                static_cast<unsigned long long>(_readout.uploadDrops), static_cast<unsigned long long>(_readout.missingGlyphs));
  _ui.AddText(textX, rowY, _style.smallSizeIndex, dropped ? _palette.caution : _palette.phosphorBody, buffer);

  return panel;
}

} // namespace Neuron
