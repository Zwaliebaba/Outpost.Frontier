#include "pch.h"
#include "FileSys.h"

namespace Neuron
{
namespace
{

/*
 * UTF-8 <-> UTF-16, the conversion Windows makes unavoidable.
 *
 * File-local on purpose: five other translation units carry their own copy of
 * one or both of these (ConfigLoad, Window, GlyphAtlas, ObjMesh, Main), and
 * hoisting a public pair into a header that every project already includes
 * would put a new overload beside each of those and make some existing
 * unqualified call ambiguous. Collapsing all six is worth doing; it is worth
 * doing on its own, not inside a bug fix.
 *
 * Explicit lengths rather than -1, so the terminator is not counted and a view
 * into the middle of a buffer works.
 */
[[nodiscard]] std::wstring ToWide(std::string_view _utf8)
{
  if (_utf8.empty())
  {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, 0, _utf8.data(), static_cast<int>(_utf8.size()), nullptr, 0);
  if (count <= 0)
  {
    return {}; // Not decodable as UTF-8 at all.
  }
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _utf8.data(), static_cast<int>(_utf8.size()), result.data(), count);
  return result;
}

[[nodiscard]] std::string ToUtf8(std::wstring_view _wide)
{
  if (_wide.empty())
  {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, _wide.data(), static_cast<int>(_wide.size()), nullptr, 0, nullptr, nullptr);
  if (bytes <= 0)
  {
    return {};
  }
  std::string result(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, _wide.data(), static_cast<int>(_wide.size()), result.data(), bytes, nullptr, nullptr);
  return result;
}

} // namespace

std::string FileSys::GetHomeDirectoryA()
{
  return ToUtf8(m_homeDir);
}

byte_buffer_t BinaryFile::ReadFile(const std::wstring& _fileName)
{
  byte_buffer_t data;

  std::wstring fullName = FileSys::GetHomeDirectory() + _fileName;
  ScopedHandle hFile(safe_handle(CreateFile2(fullName.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr)));
  if (!hFile)
    return {};

  // Get the file size.
  FILE_STANDARD_INFO fileInfo;
  if (!GetFileInformationByHandleEx(hFile.get(), FileStandardInfo, &fileInfo, sizeof(fileInfo)))
    return {};

  // File is too big for 32-bit allocation, so reject read.
  if (fileInfo.EndOfFile.HighPart > 0)
    return {};

  // Create enough space for the file data.
  data.resize(fileInfo.EndOfFile.LowPart);

  // Read the data in.
  DWORD bytesRead = 0;

  if (!::ReadFile(hFile.get(), data.data(), fileInfo.EndOfFile.LowPart, &bytesRead, nullptr))
    return {};

  if (bytesRead < fileInfo.EndOfFile.LowPart)
    return {};

  return data;
}

std::wstring TextFile::ReadFile(const std::wstring& _fileName)
{
  // Read the bytes, then decode them. The previous version was BinaryFile's
  // body with the buffer type changed to std::wstring and nothing else: it
  // sized the string in wchar_ts from a count of bytes and then read that many
  // bytes into it, so a 100-byte file became a 100-character string: 50
  // characters of adjacent byte pairs read as UTF-16, then 50 nulls. It also
  // opened _fileName directly while BinaryFile prefixed the
  // home directory, so the two classes disagreed about what a file name meant.
  // Going through BinaryFile settles both: one reader, one meaning.
  const byte_buffer_t bytes = BinaryFile::ReadFile(_fileName);
  if (bytes.empty())
  {
    return {};
  }

  // A byte-order mark for an encoding that has no byte order. Editors write one
  // anyway, and passing it through leaves U+FEFF as the first character of
  // every file that has one -- which a parser reports as a syntax error on
  // line 1 with nothing visibly wrong there.
  static constexpr std::uint8_t UTF8_BOM[] = {0xEF, 0xBB, 0xBF};
  const bool hasBom = bytes.size() >= std::size(UTF8_BOM) && std::equal(std::begin(UTF8_BOM), std::end(UTF8_BOM), bytes.begin());
  const std::size_t offset = hasBom ? std::size(UTF8_BOM) : 0;

  return ToWide(std::string_view{reinterpret_cast<const char*>(bytes.data()) + offset, bytes.size() - offset});
}

} // namespace Neuron
