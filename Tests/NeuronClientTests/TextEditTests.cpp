#include "pch.h"
#include "CppUnitTest.h"

#include "TextEditState.h"

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The editable field (ADR-020 §3, T3a).
 *
 * Wing renames are the first thing that needs one, and they live in the user
 * settings layer rather than on the wire (ADR-017 §6) -- so every defect this
 * file is about is a defect a player sees and the server never hears.
 *
 * They are all arithmetic. A caret that lands mid-codepoint, a backspace that
 * eats one byte of a three-byte character, a cap that counts bytes and so fits
 * sixteen Latin letters and five of something else: none of them needs a
 * device, a font or a keyboard to reach, which is exactly why they are here.
 */

namespace NeuronClientTests
{
namespace
{

/// Three characters, one of each interesting UTF-8 length: `A` (one byte),
/// U+00E9 (two) and U+4E2D (three). Written as escapes rather than as literal
/// bytes so the test says what it means whatever encoding the file is read in.
constexpr char32_t ASCII = U'A';
constexpr char32_t TWO_BYTE = U'\u00E9';
constexpr char32_t THREE_BYTE = U'\u4E2D';
constexpr char32_t FOUR_BYTE = U'\U0001F680';

[[nodiscard]] TextEditState FieldHolding(std::initializer_list<char32_t> _codepoints, std::uint32_t _cap = 32)
{
  TextEditState field{_cap};
  for (const char32_t codepoint : _codepoints)
  {
    Assert::IsTrue(field.Insert(codepoint), L"the fixture could not type its own text");
  }
  return field;
}

} // namespace

TEST_CLASS(Utf8Tests)
{
public:
  TEST_METHOD(EveryLengthEncodesToItsOwnNumberOfBytes)
  {
    char bytes[4] = {};
    Assert::AreEqual<std::uint32_t>(1, EncodeUtf8(ASCII, bytes));
    Assert::AreEqual<std::uint32_t>(2, EncodeUtf8(TWO_BYTE, bytes));
    Assert::AreEqual<std::uint32_t>(3, EncodeUtf8(THREE_BYTE, bytes));
    Assert::AreEqual<std::uint32_t>(4, EncodeUtf8(FOUR_BYTE, bytes));
  }

  TEST_METHOD(TheCountIsInCodepointsAndNotInBytes)
  {
    TextEditState field = FieldHolding({ASCII, TWO_BYTE, THREE_BYTE, FOUR_BYTE});

    Assert::AreEqual<std::uint32_t>(4, field.CodepointCount());
    Assert::AreEqual<std::size_t>(10, field.Text().size(), L"1 + 2 + 3 + 4 bytes");
  }

  TEST_METHOD(ControlCharactersAreNotText)
  {
    /*
     * `\b`, `\r` and `\t` arrive through the same door as letters and belong to
     * the editing keys instead. This is the layer that promises nothing
     * unpaintable is ever stored, so it refuses them itself rather than trusting
     * the window to have done it.
     */
    Assert::IsFalse(IsStorableCodepoint(U'\b'));
    Assert::IsFalse(IsStorableCodepoint(U'\r'));
    Assert::IsFalse(IsStorableCodepoint(U'\t'));
    Assert::IsFalse(IsStorableCodepoint(U'\n'));
    Assert::IsFalse(IsStorableCodepoint(static_cast<char32_t>(0x7F)), L"DEL");
    Assert::IsFalse(IsStorableCodepoint(static_cast<char32_t>(0x85)), L"a C1 control");

    Assert::IsTrue(IsStorableCodepoint(U' '), L"a space is a character a name may hold");
  }

  TEST_METHOD(ASurrogateHalfIsNotACharacter)
  {
    // `WM_CHAR` delivers UTF-16 code units and the pair is assembled in
    // `Window`; one arriving alone here is a bug upstream, and storing it would
    // put an unpaintable byte sequence in a name.
    Assert::IsFalse(IsStorableCodepoint(static_cast<char32_t>(0xD800)));
    Assert::IsFalse(IsStorableCodepoint(static_cast<char32_t>(0xDFFF)));
    Assert::IsFalse(IsStorableCodepoint(static_cast<char32_t>(0x110000)), L"past the last scalar value");

    char bytes[4] = {};
    Assert::AreEqual<std::uint32_t>(0, EncodeUtf8(static_cast<char32_t>(0xD800), bytes));
  }
};

TEST_CLASS(TextEditStateTests)
{
public:
  TEST_METHOD(TypingAppendsAndTheCaretFollows)
  {
    TextEditState field;
    Assert::IsTrue(field.Insert(U'R'));
    Assert::IsTrue(field.Insert(U'E'));
    Assert::IsTrue(field.Insert(U'D'));

    Assert::AreEqual(std::string{"RED"}, std::string{field.Text()});
    Assert::AreEqual<std::uint32_t>(3, field.Caret());
    Assert::IsFalse(field.HasSelection());
  }

  TEST_METHOD(BackspaceRemovesAWholeCodepointAndNotOneByte)
  {
    // The defect this whole type is arithmetic against: a three-byte character
    // half-deleted is a buffer no atlas can draw.
    TextEditState field = FieldHolding({ASCII, THREE_BYTE});
    Assert::AreEqual<std::size_t>(4, field.Text().size());

    Assert::IsTrue(field.Backspace());
    Assert::AreEqual<std::uint32_t>(1, field.CodepointCount());
    Assert::AreEqual<std::size_t>(1, field.Text().size());
    Assert::AreEqual<std::uint32_t>(1, field.Caret());
  }

  TEST_METHOD(TheCaretStepsWholeCodepointsInBothDirections)
  {
    TextEditState field = FieldHolding({TWO_BYTE, THREE_BYTE, FOUR_BYTE});

    field.MoveHome(false);
    Assert::AreEqual<std::uint32_t>(0, field.Caret());

    field.MoveRight(false);
    Assert::AreEqual<std::uint32_t>(2, field.Caret());
    field.MoveRight(false);
    Assert::AreEqual<std::uint32_t>(5, field.Caret());
    field.MoveRight(false);
    Assert::AreEqual<std::uint32_t>(9, field.Caret());

    // And past the end it stays put rather than walking off the buffer.
    field.MoveRight(false);
    Assert::AreEqual<std::uint32_t>(9, field.Caret());

    field.MoveLeft(false);
    Assert::AreEqual<std::uint32_t>(5, field.Caret());
  }

  TEST_METHOD(TheCapCountsCodepointsSoEveryNameFitsTheSameColumn)
  {
    /*
     * A cap in bytes would fit four Latin letters and one emoji, which is a
     * layout promise this game cannot keep -- the hangar's wing columns are a
     * fixed width whatever alphabet a player names a wing in.
     */
    TextEditState field = FieldHolding({FOUR_BYTE, FOUR_BYTE, FOUR_BYTE, FOUR_BYTE}, 4);

    Assert::AreEqual<std::uint32_t>(4, field.CodepointCount());
    Assert::AreEqual<std::size_t>(16, field.Text().size());
    Assert::IsFalse(field.Insert(ASCII), L"the fifth codepoint is refused, whatever it weighs");
    Assert::AreEqual<std::uint32_t>(4, field.CodepointCount());
  }

  TEST_METHOD(TypingOverASelectionWorksEvenInAFullField)
  {
    /*
     * Select-all-then-type is the fastest way to rename a wing, and it must not
     * be the one gesture a full field refuses. The selection is removed before
     * the cap is measured, which is what makes that true.
     */
    TextEditState field = FieldHolding({U'O', U'L', U'D'}, 3);
    Assert::IsFalse(field.Insert(U'X'), L"the field really is full");

    field.SelectAll();
    Assert::IsTrue(field.HasSelection());
    Assert::IsTrue(field.Insert(U'N'));

    Assert::AreEqual(std::string{"N"}, std::string{field.Text()});
    Assert::IsFalse(field.HasSelection());
  }

  TEST_METHOD(ARefusedInsertLeavesTheFieldExactlyAsItWas)
  {
    // The refusal path removes a selection before it checks the cap, so it has
    // to be unreachable with one -- otherwise a rejected keystroke would delete
    // the player's text and put nothing back.
    TextEditState field = FieldHolding({U'A', U'B'}, 2);
    const std::string before{field.Text()};

    Assert::IsFalse(field.Insert(U'C'));
    Assert::AreEqual(before, std::string{field.Text()});
    Assert::AreEqual<std::uint32_t>(2, field.Caret());
  }

  TEST_METHOD(AnUnstorableCodepointIsRefusedWithoutTouchingTheSelection)
  {
    TextEditState field = FieldHolding({U'A', U'B', U'C'});
    field.SelectAll();

    Assert::IsFalse(field.Insert(U'\t'));
    Assert::AreEqual(std::string{"ABC"}, std::string{field.Text()});
    Assert::IsTrue(field.HasSelection(), L"a tab is not an edit, so it undoes nothing");
  }

  TEST_METHOD(ShiftExtendsTheSelectionAndAPlainArrowCollapsesIt)
  {
    TextEditState field = FieldHolding({U'A', U'B', U'C', U'D'});

    field.MoveLeft(true);
    field.MoveLeft(true);
    Assert::IsTrue(field.HasSelection());
    Assert::AreEqual(std::string{"CD"}, std::string{field.SelectedText()});

    // Plain left goes to the *near* edge rather than one step from the caret:
    // the player is dismissing the selection, not moving through it.
    field.MoveLeft(false);
    Assert::IsFalse(field.HasSelection());
    Assert::AreEqual<std::uint32_t>(2, field.Caret());
  }

  TEST_METHOD(APlainRightArrowCollapsesToTheFarEdge)
  {
    TextEditState field = FieldHolding({U'A', U'B', U'C', U'D'});
    field.MoveHome(false);
    field.MoveRight(true);
    field.MoveRight(true);
    Assert::AreEqual(std::string{"AB"}, std::string{field.SelectedText()});

    field.MoveRight(false);
    Assert::IsFalse(field.HasSelection());
    Assert::AreEqual<std::uint32_t>(2, field.Caret());
  }

  TEST_METHOD(BackspaceAndDeleteBothTakeTheSelectionWhenThereIsOne)
  {
    TextEditState backspacing = FieldHolding({U'A', U'B', U'C'});
    backspacing.SelectAll();
    Assert::IsTrue(backspacing.Backspace());
    Assert::IsTrue(backspacing.Empty());

    TextEditState deleting = FieldHolding({U'A', U'B', U'C'});
    deleting.SelectAll();
    Assert::IsTrue(deleting.DeleteForward());
    Assert::IsTrue(deleting.Empty());
  }

  TEST_METHOD(NothingToRemoveIsReportedRatherThanPretended)
  {
    TextEditState field;
    Assert::IsFalse(field.Backspace());
    Assert::IsFalse(field.DeleteForward());

    TextEditState atTheEnd = FieldHolding({U'A'});
    Assert::IsFalse(atTheEnd.DeleteForward(), L"the caret is past the last character");
    Assert::IsTrue(atTheEnd.Backspace());
  }

  TEST_METHOD(DeleteForwardTakesTheWholeCodepointAhead)
  {
    TextEditState field = FieldHolding({THREE_BYTE, ASCII});
    field.MoveHome(false);

    Assert::IsTrue(field.DeleteForward());
    Assert::AreEqual(std::string{"A"}, std::string{field.Text()});
    Assert::AreEqual<std::uint32_t>(0, field.Caret());
  }

  TEST_METHOD(LoadingAnOverLongNameTruncatesOnABoundaryRatherThanRefusing)
  {
    /*
     * The loading path: a name out of the user layer, a hand-edited
     * `Settings.json`, or an older build with a longer cap. ADR-012's posture
     * toward a hand-edited artefact is to fail soft, so the name comes back
     * shortened rather than taking the file down -- and shortened *on a
     * boundary*, or what comes back is not text at all.
     */
    TextEditState loaded{2};
    char bytes[4] = {};
    std::string source;
    for (std::uint32_t index = 0; index < 4; ++index)
    {
      const std::uint32_t written = EncodeUtf8(THREE_BYTE, bytes);
      source.append(bytes, written);
    }

    loaded.SetText(source);
    Assert::AreEqual<std::uint32_t>(2, loaded.CodepointCount());
    Assert::AreEqual<std::size_t>(6, loaded.Text().size(), L"two whole three-byte characters");
    Assert::AreEqual<std::uint32_t>(6, loaded.Caret(), L"a rename box opens with the caret at the end");
  }

  TEST_METHOD(ClearingTakesTheCaretAndTheAnchorWithIt)
  {
    // An anchor left pointing into a buffer that no longer exists is the bug
    // this type is arithmetic against.
    TextEditState field = FieldHolding({U'A', U'B', U'C'});
    field.SelectAll();
    field.Clear();

    Assert::IsTrue(field.Empty());
    Assert::AreEqual<std::uint32_t>(0, field.Caret());
    Assert::IsFalse(field.HasSelection());
  }
};

} // namespace NeuronClientTests
