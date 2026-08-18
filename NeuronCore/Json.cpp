#include "pch.h"

#include "Json.h"

#include <charconv>
#include <cstring>

/*
 * The parser (ADR-012 §C).
 *
 * Iterative, with an explicit stack of open containers. Recursion would be
 * shorter and would also mean a file could choose our stack depth, which is not
 * a property a config loader should hand to its input.
 *
 * Errors accumulate rather than stopping at the first one where that is safe,
 * so a mistyped config reports what is wrong in one run instead of one problem
 * per launch. Structural damage still stops the parse -- past a broken brace
 * every later "error" would be noise.
 */

namespace Neuron
{
namespace
{

constexpr std::uint32_t INVALID_INDEX = JsonValue::INVALID_INDEX;

[[nodiscard]] bool IsWhitespace(char _c) noexcept
{
  return _c == ' ' || _c == '\t' || _c == '\r' || _c == '\n';
}

[[nodiscard]] bool IsDigit(char _c) noexcept
{
  return _c >= '0' && _c <= '9';
}

} // namespace

class JsonParser
{
public:
  JsonParser(std::string_view _text, JsonDocument& _document, std::vector<JsonError>& _errors, const JsonLimits& _limits) noexcept
    : m_text(_text),
      m_document(_document),
      m_errors(_errors),
      m_limits(_limits)
  {
  }

  [[nodiscard]] bool Run()
  {
    if (m_text.size() > m_limits.maxBytes)
    {
      Error("document is larger than the caller's limit");
      return false;
    }

    // A UTF-8 BOM is legal in the wild and invisible in an editor, so tolerate it.
    if (m_text.size() >= 3 && static_cast<unsigned char>(m_text[0]) == 0xef && static_cast<unsigned char>(m_text[1]) == 0xbb &&
        static_cast<unsigned char>(m_text[2]) == 0xbf)
    {
      m_position = 3;
    }

    SkipTrivia();
    if (AtEnd())
    {
      Error("document is empty");
      return false;
    }

    const std::uint32_t root = ParseValue();
    if (root == INVALID_INDEX)
    {
      return false;
    }

    while (!m_stack.empty() && !m_failed)
    {
      StepContainer();
    }
    if (m_failed)
    {
      return false;
    }

    SkipTrivia();
    if (!AtEnd())
    {
      Error("trailing content after the root value");
      return false;
    }
    return true;
  }

private:
  struct Frame
  {
    std::uint32_t node = INVALID_INDEX;
    std::uint32_t lastChild = INVALID_INDEX;
    bool isObject = false;
  };

  [[nodiscard]] bool AtEnd() const noexcept
  {
    return m_position >= m_text.size();
  }
  [[nodiscard]] char Peek() const noexcept
  {
    return AtEnd() ? '\0' : m_text[m_position];
  }

  char Advance() noexcept
  {
    const char c = m_text[m_position++];
    if (c == '\n')
    {
      ++m_line;
      m_column = 1;
    }
    else
    {
      ++m_column;
    }
    return c;
  }

  void Error(std::string _message)
  {
    m_failed = true;
    if (m_errors.size() < 32) // Enough to be useful; not a wall of noise.
    {
      m_errors.push_back(JsonError{m_line, m_column, std::move(_message)});
    }
  }

  /// Whitespace plus the two authoring relaxations: // and comments.
  void SkipTrivia()
  {
    for (;;)
    {
      while (!AtEnd() && IsWhitespace(Peek()))
      {
        Advance();
      }
      if (AtEnd() || Peek() != '/')
      {
        return;
      }
      if (m_position + 1 >= m_text.size())
      {
        Error("stray '/' at end of document");
        return;
      }
      const char kind = m_text[m_position + 1];
      if (kind == '/')
      {
        while (!AtEnd() && Peek() != '\n')
        {
          Advance();
        }
      }
      else if (kind == '*')
      {
        Advance();
        Advance();
        for (;;)
        {
          if (AtEnd())
          {
            Error("unterminated block comment");
            return;
          }
          if (Peek() == '*' && m_position + 1 < m_text.size() && m_text[m_position + 1] == '/')
          {
            Advance();
            Advance();
            break;
          }
          Advance();
        }
      }
      else
      {
        Error("stray '/' -- expected // or /*");
        return;
      }
    }
  }

  [[nodiscard]] std::uint32_t NewNode(JsonKind _kind)
  {
    JsonDocument::Node node;
    node.kind = _kind;
    node.line = m_line;
    m_document.m_nodes.push_back(node);
    return static_cast<std::uint32_t>(m_document.m_nodes.size() - 1);
  }

  /// Parses one value. Containers push a frame and are finished by StepContainer.
  [[nodiscard]] std::uint32_t ParseValue()
  {
    SkipTrivia();
    if (AtEnd())
    {
      Error("expected a value");
      return INVALID_INDEX;
    }

    const char c = Peek();
    switch (c)
    {
    case '{':
    case '[':
    {
      const bool isObject = c == '{';
      if (m_stack.size() >= m_limits.maxDepth)
      {
        Error("nesting is deeper than the limit");
        return INVALID_INDEX;
      }
      Advance();
      const std::uint32_t node = NewNode(isObject ? JsonKind::Object : JsonKind::Array);
      m_stack.push_back(Frame{node, INVALID_INDEX, isObject});
      return node;
    }
    case '"':
    {
      std::uint32_t offset = 0;
      std::uint32_t length = 0;
      if (!ParseString(offset, length))
      {
        return INVALID_INDEX;
      }
      const std::uint32_t node = NewNode(JsonKind::String);
      m_document.m_nodes[node].textOffset = offset;
      m_document.m_nodes[node].textLength = length;
      return node;
    }
    case 't':
    case 'f':
    case 'n':
      return ParseKeyword();
    default:
      if (c == '-' || IsDigit(c))
      {
        return ParseNumber();
      }
      Error("expected a value");
      return INVALID_INDEX;
    }
  }

  /// One iteration of the open container on top of the stack.
  void StepContainer()
  {
    const std::size_t frameIndex = m_stack.size() - 1;
    const bool isObject = m_stack[frameIndex].isObject;
    const char closer = isObject ? '}' : ']';

    SkipTrivia();
    if (AtEnd())
    {
      Error(isObject ? "unterminated object" : "unterminated array");
      return;
    }

    if (Peek() == closer)
    {
      Advance();
      m_stack.pop_back();
      return;
    }

    if (m_stack[frameIndex].lastChild != INVALID_INDEX)
    {
      if (Peek() != ',')
      {
        Error("expected ',' or the closing bracket");
        return;
      }
      Advance();
      SkipTrivia();
      if (Peek() == closer) // Trailing comma: allowed on the way in, never emitted.
      {
        Advance();
        m_stack.pop_back();
        return;
      }
    }

    std::uint32_t nameOffset = 0;
    std::uint32_t nameLength = 0;
    if (isObject)
    {
      if (Peek() != '"')
      {
        Error("expected a quoted member name");
        return;
      }
      if (!ParseString(nameOffset, nameLength))
      {
        return;
      }
      SkipTrivia();
      if (Peek() != ':')
      {
        Error("expected ':' after the member name");
        return;
      }
      Advance();

      if (HasMemberNamed(m_stack[frameIndex].node, nameOffset, nameLength))
      {
        // Last-one-wins is a config footgun: the file says two things and the
        // reader silently picks one. Refuse instead.
        Error("duplicate key '" + std::string(m_document.Text(nameOffset, nameLength)) + "'");
        return;
      }
    }

    const std::uint32_t child = ParseValue();
    if (child == INVALID_INDEX)
    {
      return;
    }

    JsonDocument::Node& childNode = m_document.m_nodes[child];
    childNode.nameOffset = nameOffset;
    childNode.nameLength = nameLength;

    Frame& frame = m_stack[frameIndex];
    JsonDocument::Node& parent = m_document.m_nodes[frame.node];
    if (frame.lastChild == INVALID_INDEX)
    {
      parent.firstChild = child;
    }
    else
    {
      m_document.m_nodes[frame.lastChild].nextSibling = child;
    }
    frame.lastChild = child;
    ++parent.childCount;
  }

  [[nodiscard]] bool HasMemberNamed(std::uint32_t _object, std::uint32_t _offset, std::uint32_t _length) const
  {
    // Linear, because config objects hold a handful of keys and a map here would
    // cost more than it saves.
    const std::string_view name = m_document.Text(_offset, _length);
    std::uint32_t child = m_document.m_nodes[_object].firstChild;
    while (child != INVALID_INDEX)
    {
      const JsonDocument::Node& node = m_document.m_nodes[child];
      if (m_document.Text(node.nameOffset, node.nameLength) == name)
      {
        return true;
      }
      child = node.nextSibling;
    }
    return false;
  }

  [[nodiscard]] std::uint32_t ParseKeyword()
  {
    if (m_text.compare(m_position, 4, "true") == 0)
    {
      for (int i = 0; i < 4; ++i)
      {
        Advance();
      }
      const std::uint32_t node = NewNode(JsonKind::Bool);
      m_document.m_nodes[node].boolean = true;
      return node;
    }
    if (m_text.compare(m_position, 5, "false") == 0)
    {
      for (int i = 0; i < 5; ++i)
      {
        Advance();
      }
      const std::uint32_t node = NewNode(JsonKind::Bool);
      m_document.m_nodes[node].boolean = false;
      return node;
    }
    if (m_text.compare(m_position, 4, "null") == 0)
    {
      for (int i = 0; i < 4; ++i)
      {
        Advance();
      }
      return NewNode(JsonKind::Null);
    }
    Error("expected true, false or null");
    return INVALID_INDEX;
  }

  [[nodiscard]] std::uint32_t ParseNumber()
  {
    const std::size_t start = m_position;
    bool isIntegral = true;

    if (Peek() == '-')
    {
      Advance();
    }
    if (!IsDigit(Peek()))
    {
      Error("expected a digit");
      return INVALID_INDEX;
    }
    while (IsDigit(Peek()))
    {
      Advance();
    }
    if (Peek() == '.')
    {
      isIntegral = false;
      Advance();
      if (!IsDigit(Peek()))
      {
        Error("expected a digit after the decimal point");
        return INVALID_INDEX;
      }
      while (IsDigit(Peek()))
      {
        Advance();
      }
    }
    if (Peek() == 'e' || Peek() == 'E')
    {
      isIntegral = false;
      Advance();
      if (Peek() == '+' || Peek() == '-')
      {
        Advance();
      }
      if (!IsDigit(Peek()))
      {
        Error("expected a digit in the exponent");
        return INVALID_INDEX;
      }
      while (IsDigit(Peek()))
      {
        Advance();
      }
    }

    const char* first = m_text.data() + start;
    const char* last = m_text.data() + m_position;

    if (isIntegral)
    {
      std::int64_t value = 0;
      const std::from_chars_result result = std::from_chars(first, last, value);
      if (result.ec == std::errc{} && result.ptr == last)
      {
        // Exactness here is why the parser exists in this shape: universe
        // coordinates are int64 metres and pass 2^53 (ADR-009).
        const std::uint32_t node = NewNode(JsonKind::Integer);
        m_document.m_nodes[node].integer = value;
        m_document.m_nodes[node].number = static_cast<double>(value);
        return node;
      }
      if (result.ec == std::errc::result_out_of_range)
      {
        Error("integer does not fit in 64 bits");
        return INVALID_INDEX;
      }
    }

    double value = 0.0;
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last)
    {
      Error("malformed number");
      return INVALID_INDEX;
    }
    const std::uint32_t node = NewNode(JsonKind::Number);
    m_document.m_nodes[node].number = value;
    m_document.m_nodes[node].integer = static_cast<std::int64_t>(value);
    return node;
  }

  void AppendUtf8(std::uint32_t _codePoint)
  {
    std::string& out = m_document.m_strings;
    if (_codePoint <= 0x7f)
    {
      out.push_back(static_cast<char>(_codePoint));
    }
    else if (_codePoint <= 0x7ff)
    {
      out.push_back(static_cast<char>(0xc0 | (_codePoint >> 6)));
      out.push_back(static_cast<char>(0x80 | (_codePoint & 0x3f)));
    }
    else if (_codePoint <= 0xffff)
    {
      out.push_back(static_cast<char>(0xe0 | (_codePoint >> 12)));
      out.push_back(static_cast<char>(0x80 | ((_codePoint >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (_codePoint & 0x3f)));
    }
    else
    {
      out.push_back(static_cast<char>(0xf0 | (_codePoint >> 18)));
      out.push_back(static_cast<char>(0x80 | ((_codePoint >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((_codePoint >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (_codePoint & 0x3f)));
    }
  }

  [[nodiscard]] bool ParseHex4(std::uint32_t& _outValue)
  {
    _outValue = 0;
    for (int i = 0; i < 4; ++i)
    {
      if (AtEnd())
      {
        Error("truncated \\u escape");
        return false;
      }
      const char c = Advance();
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9')
      {
        digit = static_cast<std::uint32_t>(c - '0');
      }
      else if (c >= 'a' && c <= 'f')
      {
        digit = static_cast<std::uint32_t>(c - 'a' + 10);
      }
      else if (c >= 'A' && c <= 'F')
      {
        digit = static_cast<std::uint32_t>(c - 'A' + 10);
      }
      else
      {
        Error("bad hex digit in \\u escape");
        return false;
      }
      _outValue = (_outValue << 4) | digit;
    }
    return true;
  }

  /// Unescapes into the document's string pool and yields its offset and length.
  [[nodiscard]] bool ParseString(std::uint32_t& _outOffset, std::uint32_t& _outLength)
  {
    Advance(); // opening quote
    const std::size_t begin = m_document.m_strings.size();

    for (;;)
    {
      if (AtEnd())
      {
        Error("unterminated string");
        return false;
      }
      const char c = Peek();
      if (c == '"')
      {
        Advance();
        break;
      }
      if (static_cast<unsigned char>(c) < 0x20)
      {
        Error("raw control character in string -- escape it");
        return false;
      }
      if (c != '\\')
      {
        m_document.m_strings.push_back(Advance());
        continue;
      }

      Advance(); // backslash
      if (AtEnd())
      {
        Error("unterminated escape");
        return false;
      }
      switch (const char escape = Advance())
      {
      case '"':
        m_document.m_strings.push_back('"');
        break;
      case '\\':
        m_document.m_strings.push_back('\\');
        break;
      case '/':
        m_document.m_strings.push_back('/');
        break;
      case 'b':
        m_document.m_strings.push_back('\b');
        break;
      case 'f':
        m_document.m_strings.push_back('\f');
        break;
      case 'n':
        m_document.m_strings.push_back('\n');
        break;
      case 'r':
        m_document.m_strings.push_back('\r');
        break;
      case 't':
        m_document.m_strings.push_back('\t');
        break;
      case 'u':
      {
        std::uint32_t code = 0;
        if (!ParseHex4(code))
        {
          return false;
        }
        if (code >= 0xd800 && code <= 0xdbff)
        {
          // High surrogate: the low half must follow, or the text is not UTF-16.
          if (m_position + 1 >= m_text.size() || m_text[m_position] != '\\' || m_text[m_position + 1] != 'u')
          {
            Error("high surrogate without a following low surrogate");
            return false;
          }
          Advance();
          Advance();
          std::uint32_t low = 0;
          if (!ParseHex4(low))
          {
            return false;
          }
          if (low < 0xdc00 || low > 0xdfff)
          {
            Error("expected a low surrogate");
            return false;
          }
          code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        }
        else if (code >= 0xdc00 && code <= 0xdfff)
        {
          Error("unpaired low surrogate");
          return false;
        }
        AppendUtf8(code);
        break;
      }
      default:
        Error(std::string("unknown escape '\\") + escape + "'");
        return false;
      }
    }

    _outOffset = static_cast<std::uint32_t>(begin);
    _outLength = static_cast<std::uint32_t>(m_document.m_strings.size() - begin);
    return true;
  }

  std::string_view m_text;
  JsonDocument& m_document;
  std::vector<JsonError>& m_errors;
  JsonLimits m_limits;

  std::vector<Frame> m_stack;
  std::size_t m_position = 0;
  std::uint32_t m_line = 1;
  std::uint32_t m_column = 1;
  bool m_failed = false;
};

bool JsonDocument::Parse(std::string_view _text, JsonDocument& _outDocument, std::vector<JsonError>& _outErrors, const JsonLimits& _limits)
{
  _outDocument.m_nodes.clear();
  _outDocument.m_strings.clear();
  _outErrors.clear();

  // The pool never reallocates away from under a string_view because every
  // accessor rebuilds the view from the offset, but reserving keeps parsing of
  // a large universe file from quadratic copying.
  _outDocument.m_strings.reserve(_text.size() / 4 + 16);
  _outDocument.m_nodes.reserve(_text.size() / 32 + 8);

  JsonParser parser{_text, _outDocument, _outErrors, _limits};
  if (parser.Run())
  {
    return true;
  }
  if (_outErrors.empty())
  {
    _outErrors.push_back(JsonError{1, 1, "parse failed"});
  }
  return false;
}

JsonValue JsonDocument::Root() const noexcept
{
  return m_nodes.empty() ? JsonValue{} : JsonValue{this, 0};
}

JsonKind JsonValue::Kind() const noexcept
{
  return Valid() ? m_document->m_nodes[m_index].kind : JsonKind::Null;
}

bool JsonValue::IsNumeric() const noexcept
{
  const JsonKind kind = Kind();
  return Valid() && (kind == JsonKind::Integer || kind == JsonKind::Number);
}

bool JsonValue::AsBool(bool _fallback) const noexcept
{
  return (Valid() && Kind() == JsonKind::Bool) ? m_document->m_nodes[m_index].boolean : _fallback;
}

std::int64_t JsonValue::AsInt64(std::int64_t _fallback) const noexcept
{
  if (!Valid())
  {
    return _fallback;
  }
  const JsonDocument::Node& node = m_document->m_nodes[m_index];
  if (node.kind == JsonKind::Integer)
  {
    return node.integer;
  }
  // A whole-valued double (1.0, 1e3) is accepted; 1.5 is not, because silently
  // truncating a fractional setting is how a config lies about what it says.
  if (node.kind == JsonKind::Number && node.number == static_cast<double>(static_cast<std::int64_t>(node.number)))
  {
    return static_cast<std::int64_t>(node.number);
  }
  return _fallback;
}

double JsonValue::AsDouble(double _fallback) const noexcept
{
  return IsNumeric() ? m_document->m_nodes[m_index].number : _fallback;
}

std::string_view JsonValue::AsString(std::string_view _fallback) const noexcept
{
  if (!Valid() || Kind() != JsonKind::String)
  {
    return _fallback;
  }
  const JsonDocument::Node& node = m_document->m_nodes[m_index];
  return m_document->Text(node.textOffset, node.textLength);
}

JsonValue JsonValue::Member(std::string_view _name) const noexcept
{
  if (!IsObject())
  {
    return {};
  }
  std::uint32_t child = m_document->m_nodes[m_index].firstChild;
  while (child != INVALID_INDEX)
  {
    const JsonDocument::Node& node = m_document->m_nodes[child];
    if (m_document->Text(node.nameOffset, node.nameLength) == _name)
    {
      return JsonValue{m_document, child};
    }
    child = node.nextSibling;
  }
  return {};
}

std::size_t JsonValue::Count() const noexcept
{
  return Valid() ? m_document->m_nodes[m_index].childCount : 0;
}

JsonValue JsonValue::At(std::size_t _index) const noexcept
{
  if (!Valid() || _index >= Count())
  {
    return {};
  }
  std::uint32_t child = m_document->m_nodes[m_index].firstChild;
  for (std::size_t i = 0; i < _index && child != INVALID_INDEX; ++i)
  {
    child = m_document->m_nodes[child].nextSibling;
  }
  return child == INVALID_INDEX ? JsonValue{} : JsonValue{m_document, child};
}

std::string_view JsonValue::Name() const noexcept
{
  if (!Valid())
  {
    return {};
  }
  const JsonDocument::Node& node = m_document->m_nodes[m_index];
  return m_document->Text(node.nameOffset, node.nameLength);
}

std::uint32_t JsonValue::Line() const noexcept
{
  return Valid() ? m_document->m_nodes[m_index].line : 0;
}

} // namespace Neuron
