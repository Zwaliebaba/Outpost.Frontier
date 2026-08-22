#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/*
 * A one-line editable field, all of it (ADR-020 §3).
 *
 * A UTF-8 buffer, a caret byte offset, a selection anchor and a codepoint cap.
 * Wing renames are the first thing that needs one (ADR-017 §6, and the names
 * live in the user-settings layer rather than on the wire); the map's search box
 * is the second.
 *
 * **Device-free, and testable exactly like `CommandRow`.** Nothing here knows
 * what a font is or how wide a cell measures. That matters more than it sounds:
 * text editing is the one part of a HUD where the defects are all in the
 * arithmetic -- a caret that lands mid-codepoint, a backspace that eats one byte
 * of a three-byte character, a selection that survives the text it pointed at --
 * and every one of those is reachable without a device.
 *
 * **Bytes outside, codepoints inside.** The caret and anchor are byte offsets
 * because that is what indexes the buffer, and they are *always on a codepoint
 * boundary* because every mutator here moves them a whole character at a time.
 * The cap is in codepoints because that is what a player counts and what a
 * layout has room for -- a name capped in bytes would fit sixteen Latin letters
 * and five of something else.
 *
 * **What this does not do.** No word motion, no undo, no clipboard, no multiple
 * lines, no shaping and no bidirectional text -- ADR-020's own exclusion list,
 * with the reopen trigger already named (ADR-018 D15.1). And no charset
 * filtering beyond "this is a paintable scalar value": *which* codepoints this
 * build can draw is the atlas's answer, asked at the widget as a character is
 * typed, because a second copy of the baked list is how a name renders at one
 * size and boxes at another.
 */

namespace Neuron
{

/*
 * How long a field may get when nobody says otherwise.
 *
 * Thirty-two codepoints is a wing name with room to be silly in, and it is a
 * *field* cap rather than a storage limit -- the buffer is a `std::string` and
 * would hold more. Capping is what stops a name from being a paragraph that no
 * column can draw, which is a layout promise rather than a memory one.
 */
inline constexpr std::uint32_t DEFAULT_TEXT_CODEPOINT_CAP = 32;

/*
 * Whether a codepoint may be stored at all.
 *
 * Scalar values only -- no surrogate halves, nothing past U+10FFFF -- and no C0
 * or C1 control character, DEL among them. The controls are the ones that would otherwise
 * arrive as ordinary text: `WM_CHAR` delivers `\b`, `\r` and `\t` through the
 * same door as letters, and they belong to the editing keys rather than to the
 * buffer. Filtering them here as well as at the window is deliberate belt and
 * braces: this is the layer that promises nothing unpaintable is ever stored.
 */
[[nodiscard]] bool IsStorableCodepoint(char32_t _codepoint) noexcept;

/// UTF-8 encodes `_codepoint` into `_outBytes`, returning how many it wrote, or
/// zero for anything `IsStorableCodepoint` refuses.
[[nodiscard]] std::uint32_t EncodeUtf8(char32_t _codepoint, char (&_outBytes)[4]) noexcept;

/// How many codepoints `_text` holds, counting every byte that is not a
/// continuation byte. Malformed input counts as whatever it looks like rather
/// than failing: this is a measure, not a validator.
[[nodiscard]] std::uint32_t Utf8CodepointCount(std::string_view _text) noexcept;

class TextEditState
{
public:
  TextEditState() = default;
  explicit TextEditState(std::uint32_t _codepointCap) noexcept : m_codepointCap{_codepointCap} {}

  /*
   * Replaces the whole field and puts the caret at the end, which is where a
   * player who just opened a rename box expects it.
   *
   * Text past the cap is **truncated on a codepoint boundary** rather than
   * refused: this is the loading path (a name out of the user layer, an older
   * build, a hand-edited `Settings.json`), and ADR-012's posture toward a
   * hand-edited artefact is to fail soft rather than to reject the file.
   */
  void SetText(std::string_view _text);

  /// Empties the field. The caret and the selection go with it -- an anchor
  /// left pointing into a buffer that no longer exists is the bug this whole
  /// type is arithmetic against.
  void Clear() noexcept;

  [[nodiscard]] std::string_view Text() const noexcept { return m_text; }
  [[nodiscard]] std::uint32_t CodepointCap() const noexcept { return m_codepointCap; }
  [[nodiscard]] std::uint32_t CodepointCount() const noexcept { return Utf8CodepointCount(m_text); }
  [[nodiscard]] bool Empty() const noexcept { return m_text.empty(); }

  [[nodiscard]] std::uint32_t Caret() const noexcept { return m_caretByte; }
  [[nodiscard]] std::uint32_t Anchor() const noexcept { return m_anchorByte; }
  [[nodiscard]] bool HasSelection() const noexcept { return m_caretByte != m_anchorByte; }

  /// The selected span as byte offsets, ordered whichever way it was dragged.
  [[nodiscard]] std::uint32_t SelectionBegin() const noexcept;
  [[nodiscard]] std::uint32_t SelectionEnd() const noexcept;
  [[nodiscard]] std::string_view SelectedText() const noexcept;

  /*
   * Types one character. Any selection is replaced first, which is what makes
   * select-all-then-type the fastest way to rename a wing.
   *
   * False when the codepoint is unstorable or the field is full -- and full is
   * measured *after* the selection is removed, so replacing a selection in a
   * full field works rather than refusing on a count the edit was about to
   * reduce.
   */
  [[nodiscard]] bool Insert(char32_t _codepoint);

  /// Removes the selection, or the codepoint before the caret. False when there
  /// was nothing to remove.
  [[nodiscard]] bool Backspace() noexcept;

  /// Removes the selection, or the codepoint after the caret.
  [[nodiscard]] bool DeleteForward() noexcept;

  /*
   * Caret motion, one whole codepoint at a time.
   *
   * `_select` extends the selection instead of dropping it -- Shift on an arrow
   * key. Without it, an arrow key on an existing selection **collapses to the
   * near edge** rather than moving from the caret: the player is dismissing the
   * selection and stepping from the end they pressed towards, which is what
   * every text field they have ever used does.
   */
  void MoveLeft(bool _select) noexcept;
  void MoveRight(bool _select) noexcept;
  void MoveHome(bool _select) noexcept;
  void MoveEnd(bool _select) noexcept;

  void SelectAll() noexcept;

  /// Drops the selection without moving the caret.
  void ClearSelection() noexcept { m_anchorByte = m_caretByte; }

private:
  /// Removes the selected span and leaves the caret where it was. False when
  /// there was no selection.
  bool RemoveSelection() noexcept;

  std::string m_text;

  /// Byte offsets, always on codepoint boundaries and always within the buffer.
  /// Every mutator restores both properties before it returns.
  std::uint32_t m_caretByte = 0;
  std::uint32_t m_anchorByte = 0;

  std::uint32_t m_codepointCap = DEFAULT_TEXT_CODEPOINT_CAP;
};

} // namespace Neuron
