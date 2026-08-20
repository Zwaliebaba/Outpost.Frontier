#include "pch.h"
#include "CppUnitTest.h"

#include "CommandRow.h"
#include "UiDrawList.h"
#include "UiLayout.h"

#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The command row (`tactical-hud.png`, S11d).
 *
 * The row is a layout and a hit test, and both live in one file precisely so a
 * test can drive them together: a HUD that lays out in the renderer and
 * hit-tests in the input handler is a HUD where the thing you press is not the
 * thing you see, and no test can catch it because the two never meet.
 */

namespace NeuronClientTests
{
namespace
{

/// A game with four commands, one working and three reserved -- the shape this
/// game actually reports, without depending on GameLogic to say so.
const OrderKindOption KINDS[] = {
    OrderKindOption{0, "Move", "Formation", true},
    OrderKindOption{1, "Attack", nullptr, false},
    OrderKindOption{2, "Stance", "Stance", false},
    OrderKindOption{3, "Abilities", "Ability", false},
};

const OrderOption FORMATIONS[] = {
    OrderOption{0, "Line"},
    OrderOption{1, "Wedge"},
    OrderOption{2, "Claw"},
};

/// The two ends of the enablement predicate. Most tests here are about layout
/// and availability, so they run with a selection -- the state a player is in
/// whenever the row is worth looking at.
constexpr CommandContext SELECTED{true};
constexpr CommandContext EMPTY{false};

/// A row the size the print draws, with room for every button.
[[nodiscard]] UiRect WideRow()
{
  return UiRect{0.0f, 836.0f, 1600.0f, 64.0f};
}

[[nodiscard]] const CommandButton* Find(std::span<const CommandButton> _buttons, const char* _label)
{
  for (const CommandButton& button : _buttons)
  {
    if (button.label != nullptr && std::strcmp(button.label, _label) == 0)
    {
      return &button;
    }
  }
  return nullptr;
}

/*
 * `Find`, but failing the test rather than the process when the button is not
 * there.
 *
 * Worth the extra name: a mutation that dropped the greyed commands made every
 * `Find(...)->enabled` in this file a null dereference, and a segfault takes
 * the whole run down instead of reporting which assertion noticed. A test suite
 * has to survive the code being wrong -- that is the situation it exists for.
 */
[[nodiscard]] const CommandButton& Required(std::span<const CommandButton> _buttons, const char* _label)
{
  const CommandButton* found = Find(_buttons, _label);
  Assert::IsNotNull(found, L"the row is missing a button this test is about");
  return *found;
}

} // namespace

TEST_CLASS(CommandRowTests)
{
public:
  TEST_METHOD(TheRowHoldsEveryCommandAndTheSelectedOnesParameter)
  {
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 2, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);

    // Four commands plus one parameter button.
    Assert::AreEqual<std::uint32_t>(5, count);

    const std::span<const CommandButton> laid{buttons, count};
    Assert::IsNotNull(Find(laid, "Move"));
    Assert::IsNotNull(Find(laid, "Attack"));
    Assert::IsNotNull(Find(laid, "Stance"));
    Assert::IsNotNull(Find(laid, "Abilities"));
    Assert::IsNotNull(Find(laid, "Formation"), L"the selected command's parameter is a button of its own");
  }

  TEST_METHOD(TheRowKeepsThePrintsOrderWithAttackSecond)
  {
    /*
     * `MOVE · ATTACK · FORMATION · STANCE · ABILITIES` -- the print's order,
     * and muscle memory forms early. The immediate verbs stay contiguous and
     * the parameter chip opens the picker cluster, rather than wedging itself
     * between MOVE and ATTACK.
     */
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    Assert::AreEqual<std::uint32_t>(5, count);
    Assert::AreEqual(std::string{"Move"}, std::string{buttons[0].label});
    Assert::AreEqual(std::string{"Attack"}, std::string{buttons[1].label});
    Assert::AreEqual(std::string{"Formation"}, std::string{buttons[2].label});
    Assert::AreEqual(std::string{"Stance"}, std::string{buttons[3].label});
    Assert::AreEqual(std::string{"Abilities"}, std::string{buttons[4].label});
  }

  TEST_METHOD(OnlyThePickerButtonsCarryTheCaret)
  {
    /*
     * The `▾` affordance: every parameter chip, and every command whose only
     * possible act is opening a picker -- a named parameter with no content.
     * MOVE is immediate and ATTACK takes a target, so neither is marked.
     */
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    const std::span<const CommandButton> laid{buttons, count};

    Assert::IsFalse(Required(laid, "Move").opensPicker);
    Assert::IsFalse(Required(laid, "Attack").opensPicker);
    Assert::IsTrue(Required(laid, "Formation").opensPicker);
    Assert::IsTrue(Required(laid, "Stance").opensPicker);
    Assert::IsTrue(Required(laid, "Abilities").opensPicker);
  }

  TEST_METHOD(OnlyTheAvailableCommandsAreEnabled)
  {
    /*
     * The whole of S11's "MOVE/FORMATION live + ATTACK/STANCE/ABILITIES
     * rendered disabled", and the word *rendered* is the point: they are in the
     * row, laid out, drawable, and refused.
     */
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    const std::span<const CommandButton> laid{buttons, count};

    Assert::IsTrue(Required(laid, "Move").enabled);
    Assert::IsTrue(Required(laid, "Formation").enabled);
    Assert::IsFalse(Required(laid, "Attack").enabled);
    Assert::IsFalse(Required(laid, "Stance").enabled);
    Assert::IsFalse(Required(laid, "Abilities").enabled);

    // And exactly one is the active command.
    int active = 0;
    for (const CommandButton& button : laid)
    {
      active += button.active ? 1 : 0;
    }
    Assert::AreEqual(1, active);
    Assert::IsTrue(Required(laid, "Move").active);
    Assert::IsFalse(Required(laid, "Formation").active, L"a parameter is not a command and is never the active one");
  }

  TEST_METHOD(AnEmptySelectionDisablesEveryVerb)
  {
    /*
     * A verb with no subject is a lie about what a tap will do. The summary row
     * says NO SELECTION and the row has to agree: every button greyed, the hit
     * test refusing all of them, and -- the part that was actually wrong -- no
     * filled MOVE, because a fill claims an input mode nobody is in.
     */
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, EMPTY, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    const std::span<const CommandButton> laid{buttons, count};

    Assert::IsTrue(count > 0, L"the row keeps its shape; only its buttons go dead");
    for (const CommandButton& button : laid)
    {
      Assert::IsFalse(button.enabled, L"nothing is enabled without something to act on");
      Assert::IsFalse(button.active, L"and nothing is filled, because no mode is engaged");
      Assert::IsNull(HitCommandRow(laid, button.rect.x + 4.0f, button.rect.y + 4.0f), L"and none of them takes a tap");
    }
  }

  TEST_METHOD(SelectingAWingEnablesTheVerbsTheGameHasContentFor)
  {
    // The other half of the predicate: the selection turns on what `available`
    // already allowed, and turns on nothing it did not.
    CommandButton empty[MAX_COMMAND_BUTTONS] = {};
    CommandButton selected[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t emptyCount =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, EMPTY, WideRow(), 1.0f, CommandRowTuning{}, empty);
    const std::uint32_t selectedCount =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, selected);

    Assert::AreEqual(emptyCount, selectedCount, L"the row is the same shape either way");
    const std::span<const CommandButton> laid{selected, selectedCount};
    Assert::IsTrue(Required(laid, "Move").enabled);
    Assert::IsTrue(Required(laid, "Formation").enabled);
    Assert::IsTrue(Required(laid, "Move").active, L"the armed command is the mode, once there is a fleet in it");

    // A reserved command stays greyed: a selection is not content.
    Assert::IsFalse(Required(laid, "Attack").enabled);
    Assert::IsFalse(Required(laid, "Stance").enabled);
    Assert::IsFalse(Required(laid, "Abilities").enabled);
  }

  TEST_METHOD(TheParameterButtonCarriesTheCurrentChoice)
  {
    // `FORMATION` over `Claw` -- the print's dropdown, showing what is chosen
    // rather than only what could be.
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 2, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    const std::span<const CommandButton> laid{buttons, count};
    const CommandButton& formation = Required(laid, "Formation");
    Assert::IsNotNull(formation.value);
    Assert::AreEqual(std::string{"Claw"}, std::string{formation.value});
    Assert::IsNull(Required(laid, "Move").value, L"a command button is one line");
  }

  TEST_METHOD(AParameterWithOneValueIsNotAChoice)
  {
    // A button that visibly does nothing when pressed is worse than one that is
    // visibly not for pressing.
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const OrderOption single[] = {OrderOption{0, "Line"}};
    const std::uint32_t count = BuildCommandRow(KINDS, 0, single, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    const CommandButton& formation = Required(std::span<const CommandButton>{buttons, count}, "Formation");
    Assert::IsFalse(formation.enabled);
    Assert::AreEqual(std::string{"Line"}, std::string{formation.value}, L"but it still says what is in force");
  }

  TEST_METHOD(SelectingAnotherCommandMovesTheParameterButton)
  {
    // Nothing here decides whether a reserved command *may* be selected -- that
    // is the hit test's refusal. This is about the layout following the
    // selection wherever it goes.
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 2, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    Assert::AreEqual<std::uint32_t>(5, count);
    Assert::AreEqual(std::string{"Stance"}, std::string{buttons[2].label});
    Assert::AreEqual(std::string{"Stance"}, std::string{buttons[3].label}, L"the parameter shares its command's word");
    Assert::IsFalse(buttons[3].enabled, L"a reserved command's parameter is not live either");

    Assert::IsNull(Find(std::span<const CommandButton>{buttons, count}, "Formation"),
                   L"Move's parameter must not still be in the row");
  }

  TEST_METHOD(ACommandWithNoParameterGetsNoParameterButton)
  {
    // Attack takes a target, not a parameter. A button labelled with nothing is
    // still a button, so there must not be one.
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 1, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    Assert::AreEqual<std::uint32_t>(4, count, L"four commands and no parameter button");
    for (std::uint32_t index = 0; index < count; ++index)
    {
      Assert::IsTrue(buttons[index].action == CommandAction::SelectKind);
    }
  }

  TEST_METHOD(ANarrowRowDropsWhatDoesNotFitRatherThanReflowing)
  {
    /*
     * R9's boundary, made mechanical. Wrapping or shrinking is where a zone
     * table becomes a layout engine; dropping is a clamp, and the count says
     * how many survived so a caller could report it.
     */
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const UiRect narrow{0.0f, 836.0f, 320.0f, 64.0f};
    const std::uint32_t count = BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, narrow, 1.0f, CommandRowTuning{}, buttons);

    Assert::IsTrue(count > 0 && count < 5);
    for (std::uint32_t index = 0; index < count; ++index)
    {
      Assert::IsTrue(buttons[index].rect.Right() <= narrow.Right(), L"a button ran outside the row it belongs to");
      Assert::AreEqual(CommandRowTuning{}.buttonWidth, buttons[index].rect.width, 0.001f, L"and none was shrunk");
    }
  }

  TEST_METHOD(TheButtonsGrowWithTheUiScaleAboveTheTouchFloor)
  {
    // The settings sheet's 0.8-1.6x is a requirement, and a row that stayed at
    // 1.0 while the text around it grew would be the control not working. The
    // height is the one dimension with a floor under it, so above 1.0x it
    // scales and below it it refuses -- see the floor test beside this one.
    CommandButton normal[MAX_COMMAND_BUTTONS] = {};
    CommandButton large[MAX_COMMAND_BUTTONS] = {};
    (void)BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, normal);
    (void)BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.6f, CommandRowTuning{}, large);

    Assert::AreEqual(large[0].rect.width, normal[0].rect.width * 1.6f, 0.01f);
    Assert::AreEqual(large[0].rect.height, normal[0].rect.height * 1.6f, 0.01f);
  }

  TEST_METHOD(AVerbNeverResolvesUnderTheTouchFloor)
  {
    /*
     * The U2 floor: 48 px is the smallest target a thumb can be asked to hit,
     * and the scale control does not get to shrink a verb below it -- at 0.8x
     * the button holds its 1.0x height and gives up its top inset instead.
     * The retuned command row (64 at 1.0x) exists to hold exactly this.
     */
    CommandButton small[MAX_COMMAND_BUTTONS] = {};
    const UiRect row{0.0f, 848.8f, 1600.0f, 64.0f * 0.8f};
    const std::uint32_t count = BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, row, 0.8f, CommandRowTuning{}, small);
    Assert::IsTrue(count > 0);

    for (std::uint32_t index = 0; index < count; ++index)
    {
      Assert::AreEqual(CommandRowTuning{}.buttonHeight, small[index].rect.height, 0.001f,
                       L"the floor holds at 0.8x");
      Assert::IsTrue(small[index].rect.y >= row.y, L"and the button still sits inside its row");
      Assert::IsTrue(small[index].rect.Bottom() <= row.Bottom() + 0.001f);
    }
  }

  TEST_METHOD(NothingIsLaidOutWithoutCommandsOrRoom)
  {
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    Assert::AreEqual<std::uint32_t>(0, BuildCommandRow({}, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons));
    Assert::AreEqual<std::uint32_t>(
        0, BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, UiRect{}, 1.0f, CommandRowTuning{}, buttons));
    Assert::AreEqual<std::uint32_t>(0, BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{},
                                                       std::span<CommandButton>{}));
  }
};

TEST_CLASS(CommandHitTests)
{
public:
  TEST_METHOD(AClickInsideALiveButtonFindsIt)
  {
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    const std::span<const CommandButton> laid{buttons, count};

    const CommandButton& move = Required(laid, "Move");
    const CommandButton* hit = HitCommandRow(laid, move.rect.x + 4.0f, move.rect.y + 4.0f);
    Assert::IsNotNull(hit);
    Assert::AreEqual(std::string{"Move"}, std::string{hit->label});
    Assert::IsTrue(hit->action == CommandAction::SelectKind);
    Assert::AreEqual<std::uint16_t>(0, hit->payload);

    const CommandButton& formation = Required(laid, "Formation");
    const CommandButton* cycled =
        HitCommandRow(laid, formation.rect.x + formation.rect.width * 0.5f, formation.rect.y + 2.0f);
    Assert::IsNotNull(cycled);
    Assert::IsTrue(cycled->action == CommandAction::CycleParameter);
  }

  TEST_METHOD(ADisabledButtonIsNotHit)
  {
    /*
     * Refused here rather than checked by the caller. A greyed ATTACK that
     * silently made itself the active command would be a fleet that stops
     * responding to the puck with no message anywhere -- and the caller that
     * forgot the `enabled` check would look correct.
     */
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    const std::span<const CommandButton> laid{buttons, count};

    const CommandButton& attack = Required(laid, "Attack");
    Assert::IsNull(HitCommandRow(laid, attack.rect.x + 4.0f, attack.rect.y + 4.0f));
  }

  TEST_METHOD(AClickBetweenOrOutsideButtonsHitsNothing)
  {
    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, WideRow(), 1.0f, CommandRowTuning{}, buttons);
    const std::span<const CommandButton> laid{buttons, count};

    // In the gap between the first two.
    const float gapX = buttons[0].rect.Right() + CommandRowTuning{}.buttonGap * 0.5f;
    Assert::IsNull(HitCommandRow(laid, gapX, buttons[0].rect.y + 4.0f));

    // Above the row, where the world is.
    Assert::IsNull(HitCommandRow(laid, buttons[0].rect.x + 4.0f, buttons[0].rect.y - 20.0f));

    // Past the last one.
    Assert::IsNull(HitCommandRow(laid, 1590.0f, buttons[0].rect.y + 4.0f));
  }

  TEST_METHOD(TheRowSitsInsideTheZoneTheLayoutGaveIt)
  {
    /*
     * Against the real `ResolveUiLayout` rather than a hand-made rect: the row
     * has to land in the zone the rest of the HUD reserved for it, and the
     * thing that would break that is a tuning change here or there, not a bug
     * in either.
     */
    const UiTuning uiTuning;
    const UiLayout layout = ResolveUiLayout(1600, 900, 1.0f, uiTuning);

    CommandButton buttons[MAX_COMMAND_BUTTONS] = {};
    const std::uint32_t count =
        BuildCommandRow(KINDS, 0, FORMATIONS, 0, SELECTED, layout.commandRow, layout.scale, CommandRowTuning{}, buttons);
    Assert::IsTrue(count > 0);

    for (std::uint32_t index = 0; index < count; ++index)
    {
      const UiRect& rect = buttons[index].rect;
      Assert::IsTrue(rect.y >= layout.commandRow.y, L"a button rode up over the context bar");
      Assert::IsTrue(rect.Bottom() <= layout.commandRow.Bottom(), L"a button hung off the bottom of the window");
      Assert::IsTrue(rect.x >= layout.commandRow.x && rect.Right() <= layout.commandRow.Right());

      // And clear of the world, which is what makes "a drag may only begin in
      // the world zone" enough to stop a button press selecting ships.
      Assert::IsFalse(layout.world.Contains(rect.x + 1.0f, rect.y + 1.0f));
    }
  }
};

} // namespace NeuronClientTests
