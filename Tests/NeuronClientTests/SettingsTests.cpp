#include "pch.h"
#include "CppUnitTest.h"

#include "ContrastAudit.h"
#include "SettingsScreen.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The settings surface (N3, `settings.png`, ADR-020 §8).
 *
 * Two files, and both are here because both are the kind of thing that is
 * asserted rather than looked at.
 *
 * `SettingsScreen` is laid out and hit-tested in one place, so the thing you
 * press is the thing you see -- and the property that keeps *is* a test: that
 * every target clears 48 real pixels at every UI scale the screen itself
 * offers. The print states it as a promise (*"the floor is enforced, not
 * scaled"*), which means the one control that sets the scale must not be able
 * to shrink itself out of reach.
 *
 * `ContrastAudit` is the screen's other promise. A palette offered from a menu
 * labelled "Deuteranopia" asks a player to trust a word; the audit is what lets
 * the screen show them a number instead, and these are the tests that say the
 * three shipped tables actually clear the floor.
 *
 * **Nothing here needs a window.** Rects go in and rects come out, and a
 * palette is a struct of packed colours -- so the screen's geometry and its
 * accessibility claims are both checkable before anyone has looked at either.
 */

namespace NeuronClientTests
{
namespace
{

/// The scale range the screen itself offers, which is the range every promise
/// about it has to hold across.
constexpr float MIN_UI_SCALE = 0.8f;
constexpr float MAX_UI_SCALE = 1.6f;

/// Room for the longest section plus slack, so a test never fails because its
/// buffer was the constraint rather than the layout.
constexpr std::size_t ROW_CAPACITY = 32;

} // namespace

TEST_CLASS(SettingsLayoutTests)
{
public:
  TEST_METHOD(EveryTargetClearsTheFloorAtEveryScaleTheScreenOffers)
  {
    /*
     * The promise the screen exists to enforce, tested on the screen that
     * enforces it.
     *
     * `settings.png` §1: *"Touch targets never drop below 48 px at any scale --
     * the floor is enforced, not scaled."* At 0.8x a 46 px header is 36.8, and
     * an earlier draft of this file shipped exactly that -- a BACK control below
     * the floor, on the one control a player reaches for when they have already
     * changed something they did not mean to.
     */
    const SettingsScreenTuning tuning;

    for (float scale = MIN_UI_SCALE; scale <= MAX_UI_SCALE + 0.001f; scale += 0.1f)
    {
      const SettingsScreenLayout layout = ResolveSettingsScreen(1440, 900, scale, tuning);
      Assert::IsTrue(layout.back.height >= TARGET_FLOOR_PIXELS, L"BACK never drops below the floor");
      Assert::IsTrue(layout.header.height >= TARGET_FLOOR_PIXELS, L"nor the bar it sits in");

      SettingsNavRow nav[SETTINGS_SECTION_COUNT] = {};
      const std::uint32_t navCount = BuildSettingsNav(layout.nav, scale, tuning, nav);
      for (std::uint32_t index = 0; index < navCount; ++index)
      {
        Assert::IsTrue(nav[index].rect.height >= TARGET_FLOOR_PIXELS, L"nor a rail row");
      }

      std::vector<SettingsRowRect> rows(ROW_CAPACITY);
      const std::uint32_t count =
          BuildSettingsRows(SettingsItemsFor(SettingsSection::Accessibility), layout.body, scale, tuning, 0, rows);
      for (std::uint32_t index = 0; index < count; ++index)
      {
        Assert::IsTrue(rows[index].rect.height >= TARGET_FLOOR_PIXELS, L"nor a body row");
        if (rows[index].kind == SettingsControlKind::Toggle)
        {
          Assert::IsTrue(rows[index].control.width >= TARGET_FLOOR_PIXELS, L"nor a switch's own target");
        }
      }
    }
  }

  TEST_METHOD(TheFloorRaisesTheDesignHeightAndOnlyWhenItHasTo)
  {
    /*
     * The two rules meeting, stated so neither can be quietly dropped: the print
     * draws a 46 px header, the floor is 48, and *the floor wins*. A target that
     * overhung its own bar would keep the 2 px at the cost of "the thing you
     * press is the thing you see", which is this file's whole reason to exist.
     */
    const SettingsScreenTuning tuning;

    const SettingsScreenLayout small = ResolveSettingsScreen(1440, 900, 0.8f, tuning);
    Assert::AreEqual(TARGET_FLOOR_PIXELS, small.header.height, 0.001f, L"at 0.8x the 36.8 px header is raised");
    Assert::AreEqual(small.header.height, small.back.height, 0.001f, L"and BACK is the bar, not something over it");

    const SettingsScreenLayout large = ResolveSettingsScreen(1440, 900, 1.6f, tuning);
    Assert::AreEqual(tuning.headerHeight * 1.6f, large.header.height, 0.001f,
                     L"and past the floor the print's height is what scales");
  }

  TEST_METHOD(TheZonesAreThePrintsAtTheAuthoredSize)
  {
    // ADR-020 §7: the prints are normative at 1440x900, so these are read off
    // `settings.png` rather than chosen.
    const SettingsScreenTuning tuning;
    const SettingsScreenLayout layout = ResolveSettingsScreen(1440, 900, 1.0f, tuning);

    Assert::AreEqual(232.0f, layout.nav.width, 0.001f, L"the rail is the print's 232");
    Assert::AreEqual(232.0f, layout.body.x, 0.001f, L"the body starts where the rail ends");
    Assert::AreEqual(1440.0f - 232.0f - 380.0f, layout.body.width, 0.001f, L"and takes what the columns leave");
    Assert::IsTrue(layout.resetAll.Right() <= 1440.001f, L"the right column stays inside the window");
    Assert::IsTrue(layout.footer.Bottom() <= 900.001f, L"and the footer inside it");
  }

  TEST_METHOD(ThePreviewIsLetterboxedRatherThanFilled)
  {
    /*
     * §7's declared overflow rule for this surface, and the print's reason:
     * the preview is a proof about proportion and colour, so distorting it
     * breaks the proof. It is the one rect here that holds its aspect.
     */
    const SettingsScreenTuning tuning;
    for (const std::uint32_t height : {600u, 900u, 1400u})
    {
      const SettingsScreenLayout layout = ResolveSettingsScreen(1440, height, 1.0f, tuning);
      Assert::AreEqual(tuning.previewAspect, layout.preview.width / layout.preview.height, 0.01f,
                       L"the preview holds 16:9 whatever room it is given");
    }
  }

  TEST_METHOD(AWindowDraggedSmallerThanItsOwnChromeNeverInverts)
  {
    // `ResolveUiLayout`'s posture, applied here: dragging a window small must
    // not put a quad with a flipped winding on screen.
    const SettingsScreenTuning tuning;
    const SettingsScreenLayout layout = ResolveSettingsScreen(120, 90, 1.0f, tuning);

    Assert::IsTrue(layout.body.width >= 0.0f, L"the body never inverts");
    Assert::IsTrue(layout.body.height >= 0.0f);
    Assert::IsTrue(layout.nav.height >= 0.0f, L"nor the rail");
    Assert::IsTrue(layout.preview.width >= 0.0f, L"nor the preview");
    Assert::IsTrue(layout.handedness.height >= 0.0f, L"nor the panel that absorbs the slack");
  }

  TEST_METHOD(AScaleOfZeroIsRefusedRatherThanDividedBy)
  {
    // The same guard every layout in this library carries: a scale that has not
    // been set yet is 1.0 rather than a division.
    const SettingsScreenTuning tuning;
    const SettingsScreenLayout layout = ResolveSettingsScreen(1440, 900, 0.0f, tuning);
    Assert::AreEqual(1.0f, layout.scale, 0.001f, L"a zero scale reads as 1.0");
    Assert::IsTrue(layout.nav.width > 0.0f);
  }
};

TEST_CLASS(SettingsControlTests)
{
public:
  TEST_METHOD(APressOnALabelIsNotAPressOnItsSwitch)
  {
    /*
     * Why `SettingsRowRect` resolves two rects rather than one. A row runs the
     * width of the body and carries an explanatory note; the *target* is the
     * 48 px switch at the right of it. Treating the row as the target would fire
     * the toggle when a player pressed the sentence explaining what it does.
     */
    const SettingsScreenTuning tuning;
    const SettingsScreenLayout layout = ResolveSettingsScreen(1440, 900, 1.0f, tuning);

    std::vector<SettingsRowRect> rows(ROW_CAPACITY);
    const std::uint32_t count =
        BuildSettingsRows(SettingsItemsFor(SettingsSection::Accessibility), layout.body, 1.0f, tuning, 0, rows);
    Assert::IsTrue(count >= 3, L"the accessibility section fits at the authored size");

    const SettingsRowRect& toggle = rows[2];
    Assert::IsTrue(toggle.kind == SettingsControlKind::Toggle, L"the third row is a switch");
    Assert::IsNull(HitSettingsRow({rows.data(), count}, toggle.rect.x + 4.0f, toggle.rect.y + 4.0f),
                   L"a press on the label misses");
    Assert::IsNotNull(HitSettingsRow({rows.data(), count}, toggle.control.x + 2.0f, toggle.control.y + 2.0f),
                      L"and a press on the switch lands");
  }

  TEST_METHOD(ACardStripTilesItsControlAndAnswersByCard)
  {
    const SettingsScreenTuning tuning;
    const SettingsScreenLayout layout = ResolveSettingsScreen(1440, 900, 1.0f, tuning);

    std::vector<SettingsRowRect> rows(ROW_CAPACITY);
    const std::uint32_t count =
        BuildSettingsRows(SettingsItemsFor(SettingsSection::Accessibility), layout.body, 1.0f, tuning, 0, rows);
    Assert::IsTrue(count >= 1);

    const SettingsRowRect& palette = rows[0];
    Assert::IsTrue(palette.kind == SettingsControlKind::Choice, L"the palette row is a choice");
    Assert::AreEqual<std::uint32_t>(3, palette.optionCount, L"of the three tables that exist");

    const UiRect first = SettingsChoiceCard(palette, 0, 1.0f, tuning);
    const UiRect last = SettingsChoiceCard(palette, 2, 1.0f, tuning);
    Assert::AreEqual(palette.control.x, first.x, 0.001f, L"the first card starts at the control");
    Assert::IsTrue(last.Right() <= palette.control.Right() + 0.001f, L"and the last ends inside it");

    Assert::AreEqual<std::uint32_t>(0, HitSettingsChoice(palette, 1.0f, tuning, first.x + 2.0f, first.y + 2.0f));
    Assert::AreEqual<std::uint32_t>(2, HitSettingsChoice(palette, 1.0f, tuning, last.x + 2.0f, last.y + 2.0f));
    Assert::AreEqual<std::uint32_t>(palette.optionCount,
                                    HitSettingsChoice(palette, 1.0f, tuning, palette.control.x - 40.0f, first.y),
                                    L"and a press beside the strip picks nothing");

    // An out-of-range card collapses rather than running off the end.
    const UiRect none = SettingsChoiceCard(palette, 9, 1.0f, tuning);
    Assert::AreEqual(0.0f, none.width, 0.001f, L"a card nobody drew has no width");
  }

  TEST_METHOD(ASliderReadsAlongItsTrackAndClampsPastTheEnds)
  {
    /*
     * A drag that leaves the end of a slider should hold at the end rather than
     * jumping back, which is what clamping buys and what a raw ratio would not.
     */
    const SettingsScreenTuning tuning;
    const SettingsScreenLayout layout = ResolveSettingsScreen(1440, 900, 1.0f, tuning);

    std::vector<SettingsRowRect> rows(ROW_CAPACITY);
    const std::uint32_t count =
        BuildSettingsRows(SettingsItemsFor(SettingsSection::Accessibility), layout.body, 1.0f, tuning, 0, rows);
    Assert::IsTrue(count >= 2);

    const SettingsRowRect& slider = rows[1];
    Assert::IsTrue(slider.kind == SettingsControlKind::Slider, L"UI scale is a slider");

    Assert::AreEqual(0.0f, SettingsSliderValueAt(slider, slider.control.x), 0.001f, L"the left end reads zero");
    Assert::AreEqual(1.0f, SettingsSliderValueAt(slider, slider.control.Right()), 0.001f, L"the right end reads one");
    Assert::AreEqual(0.0f, SettingsSliderValueAt(slider, slider.control.x - 900.0f), 0.001f, L"and a drag off clamps");
    Assert::AreEqual(1.0f, SettingsSliderValueAt(slider, slider.control.Right() + 900.0f), 0.001f, L"at both ends");

    const UiRect handle = SettingsSliderHandle(slider, 0.5f, 1.0f, tuning);
    const float centre = handle.x + handle.width * 0.5f;
    Assert::AreEqual(slider.control.x + slider.control.width * 0.5f, centre, 1.0f,
                     L"and the handle sits where the value says");
  }

  TEST_METHOD(ARowNobodyCanPressIsDrawnAndRefused)
  {
    /*
     * The print's instruction for ACCOUNT, followed literally: *"the section
     * exists so the shape is known, not so it can be built"*. The rows are laid
     * out -- so the screen does not grow entries later for no visible reason --
     * and none of them is a target.
     */
    const SettingsScreenTuning tuning;
    const SettingsScreenLayout layout = ResolveSettingsScreen(1440, 900, 1.0f, tuning);

    std::vector<SettingsRowRect> rows(ROW_CAPACITY);
    const std::uint32_t count =
        BuildSettingsRows(SettingsItemsFor(SettingsSection::Account), layout.body, 1.0f, tuning, 0, rows);
    Assert::IsTrue(count > 0, L"the account section draws its rows");

    for (std::uint32_t index = 0; index < count; ++index)
    {
      Assert::IsFalse(rows[index].enabled, L"and none of them is a target");
      Assert::IsNull(HitSettingsRow({rows.data(), count}, rows[index].rect.x + 2.0f, rows[index].rect.y + 2.0f),
                     L"so a press anywhere on one hits nothing");
    }
  }

  TEST_METHOD(ABlockedSectionIsDrawnRefusedAndSaysWhy)
  {
    /*
     * Two sections are blocked and the reasons differ in kind: ACCOUNT waits on
     * a system nobody has built, AUDIO on a client API -- `AudioDevice` takes
     * its gains once and has no setter for a running mixer. Both are in the
     * rail, both refuse a press, and both carry a sentence.
     */
    Assert::IsTrue(SettingsSectionAvailable(SettingsSection::Accessibility));
    Assert::IsTrue(SettingsSectionAvailable(SettingsSection::Input));
    Assert::IsTrue(SettingsSectionAvailable(SettingsSection::Display));
    Assert::IsFalse(SettingsSectionAvailable(SettingsSection::Audio), L"audio has no live gain path");
    Assert::IsFalse(SettingsSectionAvailable(SettingsSection::Account), L"and no account system exists");

    for (std::uint32_t index = 0; index < SETTINGS_SECTION_COUNT; ++index)
    {
      const auto section = static_cast<SettingsSection>(index);
      const bool blocked = !SettingsSectionAvailable(section);
      const std::string reason = SettingsSectionBlockedReason(section);
      Assert::AreEqual(blocked, !reason.empty(), L"a blocked section has a reason and a live one does not");
      Assert::IsTrue(SettingsSectionName(section)[0] != '\0', L"every section has a word");
      Assert::IsTrue(SettingsSectionNote(section)[0] != '\0', L"and a note under its heading");
    }

    const SettingsScreenTuning tuning;
    const SettingsScreenLayout layout = ResolveSettingsScreen(1440, 900, 1.0f, tuning);
    SettingsNavRow nav[SETTINGS_SECTION_COUNT] = {};
    const std::uint32_t count = BuildSettingsNav(layout.nav, 1.0f, tuning, nav);
    Assert::AreEqual<std::uint32_t>(SETTINGS_SECTION_COUNT, count, L"every section gets a rail row");

    for (std::uint32_t index = 0; index < count; ++index)
    {
      const SettingsNavRow* hit = HitSettingsNav({nav, count}, nav[index].rect.x + 4.0f, nav[index].rect.y + 4.0f);
      Assert::AreEqual(SettingsSectionAvailable(nav[index].section), hit != nullptr,
                       L"the rail answers for exactly the sections that are live");
    }
  }

  TEST_METHOD(TheBodyScrollsByWholeRowsAndNeverDrawsOutsideItself)
  {
    /*
     * §7's declared overflow answer for this surface, against ADR-020's fence:
     * *"No clip rectangle on the Ui instance and no virtualised list beyond
     * §4's whole-row cull"*.
     *
     * An earlier draft took a pixel offset, which lands mid-row on a screen
     * whose rows are three different heights -- and with no clip rectangle, the
     * half hanging above the body would have been drawn over the section
     * heading on every scrolled frame.
     */
    const SettingsScreenTuning tuning;
    const std::span<const SettingsItem> items = SettingsItemsFor(SettingsSection::Accessibility);

    // A body too short for the run, so there is something to scroll.
    const SettingsScreenLayout squat = ResolveSettingsScreen(1440, 420, 1.0f, tuning);
    Assert::IsTrue(SettingsRowsHeight(items, 1.0f, tuning) > squat.body.height, L"the run outgrows the body");

    std::vector<SettingsRowRect> top(ROW_CAPACITY);
    const std::uint32_t fitted = BuildSettingsRows(items, squat.body, 1.0f, tuning, 0, top);
    Assert::IsTrue(fitted > 0 && fitted < items.size(), L"so some rows drop");

    std::vector<SettingsRowRect> scrolled(ROW_CAPACITY);
    const std::uint32_t after = BuildSettingsRows(items, squat.body, 1.0f, tuning, 2, scrolled);
    Assert::IsTrue(after > 0, L"scrolling still yields rows");
    Assert::AreEqual<std::uint32_t>(2, scrolled[0].item, L"and starts exactly at the row asked for");

    for (std::uint32_t index = 0; index < after; ++index)
    {
      Assert::IsTrue(scrolled[index].rect.y >= squat.body.y - 0.001f, L"no row is drawn above the body");
      Assert::IsTrue(scrolled[index].rect.Bottom() <= squat.body.Bottom() + 0.001f, L"or below it");
    }

    // Scrolled past the end yields nothing rather than reading off the table.
    const std::uint32_t none = BuildSettingsRows(items, squat.body, 1.0f, tuning, 99, scrolled);
    Assert::AreEqual<std::uint32_t>(0, none, L"and scrolling past the end draws nothing");
  }

  TEST_METHOD(EveryChoiceRowHasAWordForEveryCard)
  {
    /*
     * The one way this table can rot: an item that says it has three options
     * against a label function that knows two. A card with no word is a card a
     * player cannot choose by looking, which is the whole complaint the print
     * makes about a dropdown labelled "Deuteranopia".
     */
    for (std::uint32_t index = 0; index < SETTINGS_SECTION_COUNT; ++index)
    {
      const auto section = static_cast<SettingsSection>(index);
      const std::span<const SettingsItem> items = SettingsItemsFor(section);
      for (std::uint32_t item = 0; item < items.size(); ++item)
      {
        if (items[item].kind != SettingsControlKind::Choice)
        {
          continue;
        }
        Assert::IsTrue(items[item].optionCount > 1, L"a choice of one is not a choice");
        for (std::uint32_t option = 0; option < items[item].optionCount; ++option)
        {
          Assert::IsTrue(SettingsChoiceLabel(section, item, option)[0] != '\0', L"and every card has a word");
        }
      }
    }
  }

  TEST_METHOD(EverySliderHasARangeItCanReport)
  {
    // A slider whose ends are equal has no value to show and no position to
    // put its handle at -- it would read 0.0 for every press.
    for (std::uint32_t index = 0; index < SETTINGS_SECTION_COUNT; ++index)
    {
      const std::span<const SettingsItem> items = SettingsItemsFor(static_cast<SettingsSection>(index));
      for (const SettingsItem& item : items)
      {
        if (item.kind == SettingsControlKind::Slider)
        {
          Assert::IsTrue(item.maximum > item.minimum, L"a slider's ends are apart");
        }
      }
    }
  }
};

TEST_CLASS(ContrastAuditTests)
{
public:
  TEST_METHOD(EveryShippedPaletteClearsTheFloorAgainstTheVoid)
  {
    /*
     * The accessibility claim, as a test rather than a sentence.
     *
     * The floor is `settings.png` §1's 4.5:1 and the ground is the colour the
     * renderer actually clears to -- so this fails the day a palette is edited
     * into something a player cannot read against space, which is the one way
     * a colour-vision table can be worse than useless.
     */
    const HudPalette tables[] = {HudPalette{}, DeuteranopiaPalette(), TritanopiaPalette()};
    for (const HudPalette& table : tables)
    {
      ContrastRow rows[CONTRAST_ROW_COUNT] = {};
      const std::uint32_t count = AuditPalette(table, rows);
      Assert::AreEqual<std::uint32_t>(CONTRAST_ROW_COUNT, count, L"every pair is measured");

      std::uint32_t grounds = 0;
      for (std::uint32_t index = 0; index < count; ++index)
      {
        if (!rows[index].Floored())
        {
          continue;
        }
        ++grounds;
        Assert::IsTrue(rows[index].MeetsFloor(), L"a glyph a player cannot read against space");
      }
      Assert::AreEqual<std::uint32_t>(STANDING_COLOUR_COUNT, grounds, L"one ground row per standing");
      Assert::IsTrue(WorstGroundContrast(table) >= CONTRAST_FLOOR, L"and the palette's worst still clears it");
    }
  }

  TEST_METHOD(TheFloorIsAGroundRuleBecauseNoPaletteCouldMeetItBetweenGlyphs)
  {
    /*
     * Why "4.5:1 on every glyph pair" is read here as *a glyph against its
     * ground* rather than literally, and it is arithmetic rather than taste.
     *
     * Clearing the floor against a void this dark forces every glyph to a
     * luminance of at least 0.19; clearing it again *between* two such glyphs
     * forces the brighter one past 1.03 -- brighter than white. Two glyphs
     * cannot satisfy both, so no palette that has ever existed could pass the
     * literal reading, and a rule nothing can pass is a rule nothing checks.
     *
     * The print says the same thing one panel higher, which is what settles it:
     * *"hull glyphs stay distinguishable by shape in every palette -- colour is
     * never the only carrier"*. Telling two glyphs apart is geometry's job.
     */
    const float voidLuminance = RelativeLuminance(SpaceClearColour());
    const float floorLuminance = CONTRAST_FLOOR * (voidLuminance + 0.05f) - 0.05f;
    const float nextLuminance = CONTRAST_FLOOR * (floorLuminance + 0.05f) - 0.05f;

    Assert::IsTrue(floorLuminance > 0.0f, L"a glyph over this void has to be reasonably bright");
    Assert::IsTrue(nextLuminance > 1.0f,
                   L"and a second glyph 4.5:1 above it would have to be brighter than white");
  }

  TEST_METHOD(APairReadingIsNotAVerdict)
  {
    // The two row kinds, and the reason `Floored` is the question a caller asks
    // before `MeetsFloor`: a pair row has no floor, so reading one as a pass or
    // a fail would condemn every palette in the corpus.
    ContrastRow rows[CONTRAST_ROW_COUNT] = {};
    const std::uint32_t count = AuditPalette(HudPalette{}, rows);

    std::uint32_t pairs = 0;
    for (std::uint32_t index = 0; index < count; ++index)
    {
      if (rows[index].Floored())
      {
        continue;
      }
      ++pairs;
      Assert::IsFalse(rows[index].MeetsFloor(), L"a pair row never reports a verdict");
      Assert::IsTrue(rows[index].ratio >= 1.0f, L"but it does carry a reading");
    }
    Assert::AreEqual<std::uint32_t>(6, pairs, L"four standings make six pairs");
    Assert::IsTrue(ClosestPairSeparation(HudPalette{}) >= 1.0f, L"and the closest of them is a real number");
  }

  TEST_METHOD(TheGroundRowsAreAPrefixSoThePanelNeedsNoSearch)
  {
    // The print's panel draws four rows and the audit measures ten. Ordering the
    // grounds first is what lets the screen draw the first four without
    // filtering -- and what stops the four visible rows being the only ones
    // anything checks.
    ContrastRow rows[CONTRAST_ROW_COUNT] = {};
    const std::uint32_t count = AuditPalette(DeuteranopiaPalette(), rows);
    Assert::IsTrue(count >= STANDING_COLOUR_COUNT);

    for (std::uint32_t index = 0; index < STANDING_COLOUR_COUNT; ++index)
    {
      Assert::IsTrue(rows[index].Floored(), L"the first four rows are the grounds");
      Assert::IsTrue(rows[index].first == static_cast<StandingColour>(index), L"in standing order");
    }
    for (std::uint32_t index = STANDING_COLOUR_COUNT; index < count; ++index)
    {
      Assert::IsFalse(rows[index].Floored(), L"and everything after them is a pair");
    }
  }

  TEST_METHOD(TheArithmeticIsWcagsAndNotAnApproximationOfIt)
  {
    /*
     * The two ends and the middle, against values the standard fixes: white on
     * black is 21:1, a colour against itself is 1:1, and the linear toe near
     * black is real rather than a curve everywhere.
     */
    Assert::AreEqual(1.0f, RelativeLuminance(0xFFFFFFFFu), 0.001f, L"white is 1.0");
    Assert::AreEqual(0.0f, RelativeLuminance(0xFF000000u), 0.001f, L"black is 0.0");
    Assert::AreEqual(21.0f, ContrastRatio(1.0f, 0.0f), 0.01f, L"and white on black is 21:1");
    Assert::AreEqual(1.0f, ContrastRatio(0.5f, 0.5f), 0.001f, L"a colour against itself is 1:1");

    // Symmetric: brighter over darker, whichever order it is asked in.
    Assert::AreEqual(ContrastRatio(0.7f, 0.1f), ContrastRatio(0.1f, 0.7f), 0.0001f, L"and the order does not matter");

    /*
     * The clear colour is *linear already* -- the render target is `_SRGB`, so
     * the hardware encodes it -- and running it through the curve a second time
     * would report the void as darker than it is drawn, flattering every ratio.
     * The weighted sum of its channels is what this must be.
     */
    const ClearColour space = SpaceClearColour();
    const float expected = 0.2126f * space.red + 0.7152f * space.green + 0.0722f * space.blue;
    Assert::AreEqual(expected, RelativeLuminance(space), 1e-6f, L"the void is not linearised twice");
  }

  TEST_METHOD(EveryStandingHasAColourAndAName)
  {
    // The four the wire can carry (ADR-022 §8b spends two bits), each mapped to
    // a palette entry and each with a word for the audit panel.
    const HudPalette table;
    for (std::uint32_t index = 0; index < STANDING_COLOUR_COUNT; ++index)
    {
      const auto standing = static_cast<StandingColour>(index);
      Assert::IsTrue(StandingColourName(standing)[0] != '\0', L"every standing has a word");
      Assert::AreNotEqual<std::uint32_t>(0, StandingColourOf(table, standing), L"and a colour");
    }
    // Own is the phosphor rather than an entry of its own, which is the same
    // statement `OverlayTuning` makes when it takes its ring colour from there.
    Assert::AreEqual(table.phosphor, StandingColourOf(table, StandingColour::Own));
  }
};

} // namespace NeuronClientTests
