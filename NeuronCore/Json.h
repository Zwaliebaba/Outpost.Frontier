#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
 * JSON (ADR-012).
 *
 * One parser serves configuration, the universe definition and sound banks, so
 * it is written for the case that actually hurts: a file a human edited by hand
 * and got slightly wrong. It never throws and never asserts on input. It
 * reports every problem it can find with a line and a column, and the caller
 * decides what is fatal.
 *
 * Shape: a flat array of nodes with index handles. No per-node allocation, no
 * pointer chasing, no ownership questions, and a document that can be moved
 * without invalidating anything. Parsing is iterative with an explicit stack
 * and a depth cap -- deep input must not smash the C++ stack.
 *
 * Numbers: an integral token keeps its exact std::int64_t. This is load-bearing
 * rather than fussy. Universe coordinates are int64 metres (ADR-009) and pass
 * 2^53, where a double-only parser starts silently rounding them.
 *
 * Deliberate relaxations for hand-authored files: // and comments, and
 * trailing commas. The writer emits neither, so anything this library produces
 * is strict JSON. Duplicate keys are an error rather than last-one-wins, which
 * is a config footgun rather than a convenience.
 */

namespace Neuron
{

enum class JsonKind : std::uint8_t
{
  Null,
  Bool,
  Integer, // Integral token: exact int64.
  Number,  // Had a fraction or an exponent: double.
  String,
  Array,
  Object
};

struct JsonError
{
  std::uint32_t line = 0;   // 1-based, as an editor counts.
  std::uint32_t column = 0; // 1-based.
  std::string message;
};

struct JsonLimits
{
  std::size_t maxBytes = std::size_t{16} * 1024 * 1024;
  std::uint32_t maxDepth = 64;
};

class JsonDocument;

/// A handle into a JsonDocument. Cheap to copy; valid only while the document lives.
class JsonValue
{
public:
  JsonValue() noexcept = default;
  JsonValue(const JsonDocument* _document, std::uint32_t _index) noexcept
    : m_document(_document),
      m_index(_index)
  {
  }

  [[nodiscard]] bool Valid() const noexcept
  {
    return m_document != nullptr && m_index != INVALID_INDEX;
  }
  [[nodiscard]] JsonKind Kind() const noexcept;

  [[nodiscard]] bool IsNull() const noexcept
  {
    return Valid() && Kind() == JsonKind::Null;
  }
  [[nodiscard]] bool IsObject() const noexcept
  {
    return Valid() && Kind() == JsonKind::Object;
  }
  [[nodiscard]] bool IsArray() const noexcept
  {
    return Valid() && Kind() == JsonKind::Array;
  }
  [[nodiscard]] bool IsNumeric() const noexcept;

  /// Typed reads. Each returns the fallback when this value is missing or the wrong kind,
  /// so a caller can read a whole config and inspect the diagnostics once, at the end.
  [[nodiscard]] bool AsBool(bool _fallback = false) const noexcept;
  [[nodiscard]] std::int64_t AsInt64(std::int64_t _fallback = 0) const noexcept;
  [[nodiscard]] double AsDouble(double _fallback = 0.0) const noexcept;
  [[nodiscard]] std::string_view AsString(std::string_view _fallback = {}) const noexcept;

  /// Object member by name. Invalid handle when absent -- chaining stays safe.
  [[nodiscard]] JsonValue Member(std::string_view _name) const noexcept;
  [[nodiscard]] bool HasMember(std::string_view _name) const noexcept
  {
    return Member(_name).Valid();
  }

  /// Element count for arrays and objects; 0 otherwise.
  [[nodiscard]] std::size_t Count() const noexcept;
  [[nodiscard]] JsonValue At(std::size_t _index) const noexcept;

  /// The key of this value when it is an object member.
  [[nodiscard]] std::string_view Name() const noexcept;

  /// Line the value starts on, for diagnostics that point at the file.
  [[nodiscard]] std::uint32_t Line() const noexcept;

  static constexpr std::uint32_t INVALID_INDEX = 0xffffffffu;

private:
  const JsonDocument* m_document = nullptr;
  std::uint32_t m_index = INVALID_INDEX;
};

class JsonDocument
{
public:
  JsonDocument() = default;

  /// Parses text. Returns false when the document is unusable; errors always
  /// carries what went wrong, and may carry several problems from one pass.
  [[nodiscard]] static bool Parse(std::string_view _text, JsonDocument& _outDocument, std::vector<JsonError>& _outErrors,
                                  const JsonLimits& _limits = {});

  [[nodiscard]] JsonValue Root() const noexcept;
  [[nodiscard]] bool Empty() const noexcept
  {
    return m_nodes.empty();
  }

private:
  friend class JsonValue;
  friend class JsonParser;

  struct Node
  {
    JsonKind kind = JsonKind::Null;
    std::uint32_t line = 0;
    std::uint32_t nameOffset = 0;
    std::uint32_t nameLength = 0;
    std::uint32_t textOffset = 0;
    std::uint32_t textLength = 0;
    std::int64_t integer = 0;
    double number = 0.0;
    bool boolean = false;
    std::uint32_t firstChild = JsonValue::INVALID_INDEX;
    std::uint32_t nextSibling = JsonValue::INVALID_INDEX;
    std::uint32_t childCount = 0;
  };

  [[nodiscard]] std::string_view Text(std::uint32_t _offset, std::uint32_t _length) const noexcept
  {
    return std::string_view{m_strings}.substr(_offset, _length);
  }

  std::vector<Node> m_nodes;
  std::string m_strings; // Unescaped at parse time; nodes hold offsets into this.
};

} // namespace Neuron
