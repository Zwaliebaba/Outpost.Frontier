#include "pch.h"
#include "CppUnitTest.h"

#include "DebugStrip.h"

#include "Clock.h"
#include "Telemetry.h"

#include <string>
#include <string_view>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The Tier-1 counters strip (Build Order S14, `debug-hud.png`).
 *
 * Both halves are device-free by design, so both are asserted here: the
 * history's latching and accumulation as arithmetic, and the builder as
 * *words* -- the same style the HUD tests use, because a run's text is what a
 * person reads and a quad count is not.
 */

namespace NeuronClientTests
{
namespace
{

/// Every run's text, concatenated with separators -- for "does the strip say
/// X anywhere", which is what most of these assertions are.
[[nodiscard]] std::string AllText(const UiDrawList& _ui)
{
  std::string text;
  for (const UiTextRun& run : _ui.Runs())
  {
    text += _ui.Text(run);
    text += '\n';
  }
  return text;
}

[[nodiscard]] bool Says(const UiDrawList& _ui, std::string_view _needle)
{
  return AllText(_ui).find(_needle) != std::string::npos;
}

/// The first run whose text contains the needle, or null.
[[nodiscard]] const UiTextRun* FindRun(const UiDrawList& _ui, std::string_view _needle)
{
  for (const UiTextRun& run : _ui.Runs())
  {
    if (_ui.Text(run).find(_needle) != std::string_view::npos)
    {
      return &run;
    }
  }
  return nullptr;
}

[[nodiscard]] DebugStripReadout HealthyReadout()
{
  DebugStripReadout readout;
  readout.frameMs = 6.5;
  readout.frameP95Ms = 7.2;
  readout.fps = 144.0;
  readout.gameMs = 1.1;
  readout.extractMs = 0.3;
  readout.renderMs = 2.0;
  readout.uiMs = 0.4;
  readout.netMs = 0.1;
  readout.joined = true;
  readout.rttMs = 4.2;
  readout.minRttMs = 0.3;
  readout.hasEstimate = true;
  readout.snapAgeMs = 32.0;
  readout.driftTicks = 0.2;
  readout.replicated = 41;
  readout.drawnInstances = 41;
  readout.drawCalls = 9;
  readout.glyphQuads = 512;
  return readout;
}

} // namespace

TEST_CLASS(DebugStripBuildTests)
{
public:
  TEST_METHOD(TheStripSaysEveryRowThePrintNames)
  {
    UiDrawList ui;
    const UiRect panel = BuildDebugStrip(HealthyReadout(), DebugStripStyle{}, HudPalette{}, ui);

    Assert::IsTrue(Says(ui, "DIAGNOSTICS - TIER 1"));
    Assert::IsTrue(Says(ui, "FRAME"), L"the headline number");
    Assert::IsTrue(Says(ui, "GAME"));
    Assert::IsTrue(Says(ui, "EXTRACT"));
    Assert::IsTrue(Says(ui, "RENDER"));
    Assert::IsTrue(Says(ui, "UI"));
    Assert::IsTrue(Says(ui, "LINK"));
    Assert::IsTrue(Says(ui, "SNAP"));
    Assert::IsTrue(Says(ui, "WORLD"));
    Assert::IsTrue(Says(ui, "DROPS"));
    Assert::IsTrue(Says(ui, "dbg"), L"honesty rule 1: the strip's own cost is a line item");
    Assert::IsTrue(panel.width > 0.0f && panel.height > 0.0f);
  }

  TEST_METHOD(EveryTextRunStaysInsideThePanel)
  {
    // The panel is the one surface allowed to occlude world -- but only where
    // it *is*: a run escaping the ground would be text floating over the fleet.
    UiDrawList ui;
    const UiRect panel = BuildDebugStrip(HealthyReadout(), DebugStripStyle{}, HudPalette{}, ui);

    for (const UiTextRun& run : ui.Runs())
    {
      Assert::IsTrue(run.x >= panel.x && run.x < panel.Right(), L"a run starts inside the panel");
      Assert::IsTrue(run.y >= panel.y && run.y < panel.Bottom(), L"and on a row inside it");
    }
  }

  TEST_METHOD(AStaleFeedSaysStaleInTheCautionColour)
  {
    DebugStripReadout readout = HealthyReadout();
    readout.stale = true;
    readout.snapAgeMs = 400.0;

    const HudPalette palette;
    UiDrawList ui;
    (void)BuildDebugStrip(readout, DebugStripStyle{}, palette, ui);

    const UiTextRun* stale = FindRun(ui, "STALE");
    Assert::IsNotNull(stale, L"a frozen feed is spelled out, not implied");
    Assert::AreEqual(palette.caution, stale->colourRgba, L"and it carries the caution colour");
  }

  TEST_METHOD(NoSessionReadsAsNoSessionRatherThanZeros)
  {
    // F10's spot check applied to the strip itself: kill the feed and the rows
    // go stale or empty -- an RTT of zero would be an invented measurement.
    DebugStripReadout readout = HealthyReadout();
    readout.joined = false;
    readout.hasEstimate = false;

    UiDrawList ui;
    (void)BuildDebugStrip(readout, DebugStripStyle{}, HudPalette{}, ui);

    Assert::IsTrue(Says(ui, "no session"));
    Assert::IsTrue(Says(ui, "no feed"));
    Assert::IsFalse(Says(ui, "rtt"), L"no link statistic is printed for a link that does not exist");
  }

  TEST_METHOD(TheSimRowAppearsOnlyWhenASimLaneReported)
  {
    // In client mode there is no Sim lane in this process, and a row of zeros
    // would claim a measurement of a machine this process cannot see.
    DebugStripReadout hostReadout = HealthyReadout();
    hostReadout.simTickMs = 3.1;

    UiDrawList hostUi;
    (void)BuildDebugStrip(hostReadout, DebugStripStyle{}, HudPalette{}, hostUi);
    Assert::IsTrue(Says(hostUi, "SIM"));
    Assert::IsTrue(Says(hostUi, "overrun"));

    DebugStripReadout clientReadout = HealthyReadout();
    clientReadout.simTickMs = -1.0;

    UiDrawList clientUi;
    (void)BuildDebugStrip(clientReadout, DebugStripStyle{}, HudPalette{}, clientUi);
    Assert::IsFalse(Says(clientUi, "SIM"));
  }

  TEST_METHOD(DropsChangeTheRowsColour)
  {
    // Honesty rule 2: a strip that quietly discards data under load is worse
    // than none. The row exists either way; losing samples changes its colour.
    const HudPalette palette;

    UiDrawList quietUi;
    (void)BuildDebugStrip(HealthyReadout(), DebugStripStyle{}, palette, quietUi);
    const UiTextRun* quiet = FindRun(quietUi, "DROPS");
    Assert::IsNotNull(quiet);
    Assert::AreEqual(palette.phosphorBody, quiet->colourRgba);

    DebugStripReadout dropping = HealthyReadout();
    dropping.telemetryDrops = 17;

    UiDrawList droppingUi;
    (void)BuildDebugStrip(dropping, DebugStripStyle{}, palette, droppingUi);
    const UiTextRun* loud = FindRun(droppingUi, "DROPS");
    Assert::IsNotNull(loud);
    Assert::AreEqual(palette.caution, loud->colourRgba);
    Assert::IsTrue(Says(droppingUi, "tele 17"));
  }

  TEST_METHOD(AStageOverItsBudgetFillsItsBarInTheCautionColour)
  {
    // The bar pins at full and the colour says how it got there -- a clamp that
    // also recoloured nothing would hide "how far over" and "over at all".
    const HudPalette palette;

    DebugStripReadout over = HealthyReadout();
    over.gameMs = 50.0; // Far past any budget.

    UiDrawList ui;
    (void)BuildDebugStrip(over, DebugStripStyle{}, palette, ui);

    bool cautionBar = false;
    for (const UiQuad& quad : ui.Quads())
    {
      cautionBar = cautionBar || quad.colourRgba == palette.caution;
    }
    Assert::IsTrue(cautionBar, L"an over-budget stage draws its fill in the caution colour");
  }
};

TEST_CLASS(DebugStripHistoryTests)
{
public:
  TEST_METHOD(NumbersLatchAtTheRefreshCadenceRatherThanFlickering)
  {
    Clock::Initialise();
    (void)Telemetry::RegisterLane("StripTest");

    DebugStripHistory history;
    DebugStripReadout readout;

    // Ten frames inside one refresh window: nothing is displayed yet, because
    // a number that moved every frame would be a flicker rather than a reading.
    double now = 0.0;
    for (int frame = 0; frame < 10; ++frame)
    {
      Telemetry::Record(TelemetryKind::Span, "Frame", 1000);
      TelemetrySnapshot drained;
      drained.DrainAll();
      history.AccumulateFrame(drained, now);
      now += 0.01;
    }
    history.FillTimings(readout);
    Assert::AreEqual(0.0, readout.frameMs, 1e-12, L"still inside the first window");

    // One more frame past the refresh threshold latches the display.
    Telemetry::Record(TelemetryKind::Span, "Frame", 1000);
    TelemetrySnapshot drained;
    drained.DrainAll();
    history.AccumulateFrame(drained, DebugStripHistory::REFRESH_SECONDS + 0.01);

    history.FillTimings(readout);
    Assert::IsTrue(readout.frameMs > 0.0, L"the window's mean is now on display");
    Assert::IsTrue(readout.fps > 0.0);
  }

  TEST_METHOD(CounterTotalsAccumulateAcrossWindowsRatherThanResetting)
  {
    Clock::Initialise();
    (void)Telemetry::RegisterLane("StripTest");

    DebugStripHistory history;

    // An overrun in the first window and another in the third: the total is
    // two, because a drop counter that reset with the display window would
    // spend most of its life claiming zero.
    double now = 0.0;
    for (int window = 0; window < 3; ++window)
    {
      if (window != 1)
      {
        Telemetry::Record(TelemetryKind::Counter, "TickOverrun", 1);
      }
      TelemetrySnapshot drained;
      drained.DrainAll();
      history.AccumulateFrame(drained, now);
      now += DebugStripHistory::REFRESH_SECONDS + 0.01;
    }

    DebugStripReadout readout;
    history.FillTimings(readout);
    Assert::AreEqual<std::uint64_t>(2, readout.tickOverruns);
  }

  TEST_METHOD(ASimLaneThatNeverReportsLeavesTheSimRowAbsent)
  {
    Clock::Initialise();
    (void)Telemetry::RegisterLane("StripTest");

    DebugStripHistory history;
    Telemetry::Record(TelemetryKind::Span, "Frame", 1000);
    TelemetrySnapshot drained;
    drained.DrainAll();
    history.AccumulateFrame(drained, 0.0);

    TelemetrySnapshot second;
    second.DrainAll();
    history.AccumulateFrame(second, DebugStripHistory::REFRESH_SECONDS + 0.01);

    DebugStripReadout readout;
    history.FillTimings(readout);
    Assert::IsTrue(readout.simTickMs < 0.0, L"no Tick span was ever drained, so there is no sim to report");
  }
};

} // namespace NeuronClientTests
