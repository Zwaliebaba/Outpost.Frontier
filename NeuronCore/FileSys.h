#pragma once

namespace Neuron
{
using byte_buffer_t = std::vector<uint8_t>;

class FileSys
{
public:
  static void SetHomeDirectory(const std::wstring& _path)
  {
    m_homeDir = _path + L"\\GameData\\";
  }
  [[nodiscard]] static std::wstring GetHomeDirectory()
  {
    return m_homeDir;
  }
  /// The same directory as UTF-8, for the parts of the tree that speak
  /// `std::string` (config paths, mesh names, log lines). Defined out of line
  /// because it is a real encoding conversion rather than a cast: the
  /// wchar_t-by-wchar_t copy it replaced silently truncated every character
  /// above U+00FF, so a user whose profile folder is not pure ASCII got a path
  /// that did not exist.
  [[nodiscard]] static std::string GetHomeDirectoryA();

protected:
  inline static std::wstring m_homeDir;
};

class BinaryFile : public FileSys
{
public:
  [[nodiscard]] static byte_buffer_t ReadFile(const std::wstring& _fileName);
};

class TextFile : public FileSys
{
public:
  [[nodiscard]] static std::wstring ReadFile(const std::wstring& _fileName);
};
} // namespace Neuron
