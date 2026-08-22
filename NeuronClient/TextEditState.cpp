#include "pch.h"

#include "TextEditState.h"

namespace Neuron
{
namespace
{

[[nodiscard]] bool IsContinuationByte(char _byte) noexcept
{
  return (static_cast<unsigned char>(_byte) & 0xC0u) == 0x80u;
}

/*
 * The previous codepoint boundary before `_byte`.
 *
 * Walks back over continuation bytes rather than trusting a lead byte's
 * advertised length, which is what makes it total: a buffer that somehow held a
 * truncated sequence still yields a boundary and an edit that shortens the
 * string, instead of an offset past the end.
 */
[[nodiscard]] std::uint32_t StepBack(std::string_view _text, std::uint32_t _byte) noexcept
{
  if (_byte == 0)
  {
    return 0;
  }
  std::uint32_t at = _byte - 1;
  while (at > 0 && IsContinuationByte(_text[at]))
  {
    --at;
  }
  return at;
}

/// The next codepoint boundary after `_byte`, by the same reasoning.
[[nodiscard]] std::uint32_t StepForward(std::string_view _text, std::uint32_t _byte) noexcept
{
  const auto size = static_cast<std::uint32_t>(_text.size());
  if (_byte >= size)
  {
    return size;
  }
  std::uint32_t at = _byte + 1;
  while (at < size && IsContinuationByte(_text[at]))
  {
    ++at;
  }
  return at;
}

[[nodiscard]] char Utf8Byte(std::uint32_t _value) noexcept
{
  return static_cast<char>(static_cast<unsigned char>(_value & 0xFFu));
}

} // namespace

bool IsStorableCodepoint(char32_t _codepoint) noexcept
{
  const auto value = static_cast<std::uint32_t>(_codepoint);

  if (value > 0x10FFFFu)
  {
    return false;
  }
  if (value >= 0xD800u && value <= 0xDFFFu)
  {
    // A surrogate half is not a character. `WM_CHAR` delivers UTF-16 code
    // units, so a pair arrives as two messages and must be assembled *before*
    // it reaches this type -- one arriving alone is a bug upstream, and storing
    // it would put an unpaintable byte sequence in a name.
    return false;
  }
  if (value < 0x20u || value == 0x7Fu || (value >= 0x80u && value <= 0x9Fu))
  {
    // C0, DEL and C1. `\b`, `\r` and `\t` come through the same door as
    // letters and belong to the editing keys instead.
    return false;
  }
  return true;
}

std::uint32_t EncodeUtf8(char32_t _codepoint, char (&_outBytes)[4]) noexcept
{
  if (!IsStorableCodepoint(_codepoint))
  {
    return 0;
  }

  const auto value = static_cast<std::uint32_t>(_codepoint);
  if (value < 0x80u)
  {
    _outBytes[0] = Utf8Byte(value);
    return 1;
  }
  if (value < 0x800u)
  {
    _outBytes[0] = Utf8Byte(0xC0u | (value >> 6));
    _outBytes[1] = Utf8Byte(0x80u | (value & 0x3Fu));
    return 2;
  }
  if (value < 0x10000u)
  {
    _outBytes[0] = Utf8Byte(0xE0u | (value >> 12));
    _outBytes[1] = Utf8Byte(0x80u | ((value >> 6) & 0x3Fu));
    _outBytes[2] = Utf8Byte(0x80u | (value & 0x3Fu));
    return 3;
  }
  _outBytes[0] = Utf8Byte(0xF0u | (value >> 18));
  _outBytes[1] = Utf8Byte(0x80u | ((value >> 12) & 0x3Fu));
  _outBytes[2] = Utf8Byte(0x80u | ((value >> 6) & 0x3Fu));
  _outBytes[3] = Utf8Byte(0x80u | (value & 0x3Fu));
  return 4;
}

std::uint32_t Utf8CodepointCount(std::string_view _text) noexcept
{
  std::uint32_t count = 0;
  for (const char byte : _text)
  {
    if (!IsContinuationByte(byte))
    {
      ++count;
    }
  }
  return count;
}

void TextEditState::SetText(std::string_view _text)
{
  m_text.assign(_text);

  // Truncate on a boundary rather than refuse. This is the loading path, and a
  // name that arrived over-long from a hand-edited file or an older build
  // should come back shortened rather than take the file down with it.
  if (Utf8CodepointCount(m_text) > m_codepointCap)
  {
    std::uint32_t at = 0;
    std::uint32_t kept = 0;
    while (kept < m_codepointCap && at < m_text.size())
    {
      at = StepForward(m_text, at);
      ++kept;
    }
    m_text.resize(at);
  }

  m_caretByte = static_cast<std::uint32_t>(m_text.size());
  m_anchorByte = m_caretByte;
}

void TextEditState::Clear() noexcept
{
  m_text.clear();
  m_caretByte = 0;
  m_anchorByte = 0;
}

std::uint32_t TextEditState::SelectionBegin() const noexcept
{
  return m_caretByte < m_anchorByte ? m_caretByte : m_anchorByte;
}

std::uint32_t TextEditState::SelectionEnd() const noexcept
{
  return m_caretByte < m_anchorByte ? m_anchorByte : m_caretByte;
}

std::string_view TextEditState::SelectedText() const noexcept
{
  return std::string_view{m_text}.substr(SelectionBegin(), SelectionEnd() - SelectionBegin());
}

bool TextEditState::RemoveSelection() noexcept
{
  if (!HasSelection())
  {
    return false;
  }
  const std::uint32_t begin = SelectionBegin();
  m_text.erase(begin, SelectionEnd() - begin);
  m_caretByte = begin;
  m_anchorByte = begin;
  return true;
}

bool TextEditState::Insert(char32_t _codepoint)
{
  char bytes[4] = {};
  const std::uint32_t written = EncodeUtf8(_codepoint, bytes);
  if (written == 0)
  {
    return false;
  }

  /*
   * The selection goes first, and the cap is measured after it.
   *
   * That ordering is what makes typing over a full field's selection work. It
   * cannot leave a half-done edit either: removing a selection always lowers
   * the count by at least one, and the count never exceeds the cap, so the
   * refusal below is only ever reachable when there was no selection to remove.
   */
  (void)RemoveSelection();
  if (CodepointCount() >= m_codepointCap)
  {
    return false;
  }

  m_text.insert(m_caretByte, bytes, written);
  m_caretByte += written;
  m_anchorByte = m_caretByte;
  return true;
}

bool TextEditState::Backspace() noexcept
{
  if (RemoveSelection())
  {
    return true;
  }
  if (m_caretByte == 0)
  {
    return false;
  }

  const std::uint32_t previous = StepBack(m_text, m_caretByte);
  m_text.erase(previous, m_caretByte - previous);
  m_caretByte = previous;
  m_anchorByte = previous;
  return true;
}

bool TextEditState::DeleteForward() noexcept
{
  if (RemoveSelection())
  {
    return true;
  }
  if (m_caretByte >= m_text.size())
  {
    return false;
  }

  const std::uint32_t next = StepForward(m_text, m_caretByte);
  m_text.erase(m_caretByte, next - m_caretByte);
  m_anchorByte = m_caretByte;
  return true;
}

void TextEditState::MoveLeft(bool _select) noexcept
{
  if (!_select && HasSelection())
  {
    // Collapse to the near edge rather than step from the caret: the player is
    // dismissing the selection, and every field they have used goes to the end
    // they pressed towards.
    m_caretByte = SelectionBegin();
    m_anchorByte = m_caretByte;
    return;
  }

  m_caretByte = StepBack(m_text, m_caretByte);
  if (!_select)
  {
    m_anchorByte = m_caretByte;
  }
}

void TextEditState::MoveRight(bool _select) noexcept
{
  if (!_select && HasSelection())
  {
    m_caretByte = SelectionEnd();
    m_anchorByte = m_caretByte;
    return;
  }

  m_caretByte = StepForward(m_text, m_caretByte);
  if (!_select)
  {
    m_anchorByte = m_caretByte;
  }
}

void TextEditState::MoveHome(bool _select) noexcept
{
  m_caretByte = 0;
  if (!_select)
  {
    m_anchorByte = 0;
  }
}

void TextEditState::MoveEnd(bool _select) noexcept
{
  m_caretByte = static_cast<std::uint32_t>(m_text.size());
  if (!_select)
  {
    m_anchorByte = m_caretByte;
  }
}

void TextEditState::SelectAll() noexcept
{
  m_anchorByte = 0;
  m_caretByte = static_cast<std::uint32_t>(m_text.size());
}

} // namespace Neuron
