#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
 * Strict JSON output (ADR-012 §C10).
 *
 * The parser accepts comments and trailing commas because people edit config by
 * hand; the writer emits neither, so anything this library produces is plain
 * RFC 8259 that any tool will read. Keys keep insertion order and indentation
 * is two spaces, which makes a settings write a reviewable diff rather than a
 * reshuffled file.
 *
 * The only writer in the MVP is the settings user layer, so this is deliberately
 * small: no pretty/compact modes, no reordering, no schema.
 */

namespace Neuron
{

class JsonWriter
{
public:
  explicit JsonWriter(std::string& _output) noexcept
    : m_output(&_output)
  {
  }

  void BeginObject();
  void EndObject();
  void BeginArray();
  void EndArray();

  /// Names the next value. Only valid inside an object.
  void Key(std::string_view _name);

  void Bool(bool _value);
  void Int(std::int64_t _value);
  void Number(double _value);
  void String(std::string_view _value);
  void Null();

  // Convenience for the common object-member case.
  void Member(std::string_view _name, bool _value);
  void Member(std::string_view _name, std::int64_t _value);
  void Member(std::string_view _name, double _value);
  void Member(std::string_view _name, std::string_view _value);

  /// False if the writer was misused (unbalanced containers, a non-finite
  /// number, a value where a key belongs). Checked once, before the file is
  /// written -- half-valid JSON must never reach disk.
  [[nodiscard]] bool Ok() const noexcept { return !m_failed; }

  /// True when every container has been closed.
  [[nodiscard]] bool Complete() const noexcept { return !m_failed && m_stack.empty(); }

private:
  void BeginValue();
  void EndValue();
  void WriteIndent();
  void WriteEscaped(std::string_view _text);

  std::string* m_output = nullptr;
  std::vector<bool> m_stack;      // true = object, false = array
  std::vector<bool> m_hasChild;   // per open container
  bool m_expectingValue = false;  // a Key() was written and wants its value
  bool m_failed = false;
};

} // namespace Neuron
