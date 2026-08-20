#include "pch.h"

#include "JsonWriter.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace Neuron
{

void JsonWriter::WriteIndent()
{
  m_output->push_back('\n');
  m_output->append(m_stack.size() * 2, ' ');
}

void JsonWriter::BeginValue()
{
  if (m_stack.empty())
  {
    return; // Root value.
  }
  const bool inObject = m_stack.back();
  if (inObject && !m_expectingValue)
  {
    m_failed = true; // A value where a key belongs.
    return;
  }
  if (!inObject)
  {
    if (m_hasChild.back())
    {
      m_output->push_back(',');
    }
    WriteIndent();
  }
}

void JsonWriter::EndValue()
{
  if (!m_stack.empty())
  {
    m_hasChild.back() = true;
  }
  m_expectingValue = false;
}

void JsonWriter::Key(std::string_view _name)
{
  if (m_stack.empty() || !m_stack.back() || m_expectingValue)
  {
    m_failed = true;
    return;
  }
  if (m_hasChild.back())
  {
    m_output->push_back(',');
  }
  WriteIndent();
  WriteEscaped(_name);
  m_output->append(": ");
  m_expectingValue = true;
}

void JsonWriter::BeginObject()
{
  BeginValue();
  m_output->push_back('{');
  m_stack.push_back(true);
  m_hasChild.push_back(false);
  m_expectingValue = false;
}

void JsonWriter::EndObject()
{
  if (m_stack.empty() || !m_stack.back())
  {
    m_failed = true;
    return;
  }
  const bool hadChildren = m_hasChild.back();
  m_stack.pop_back();
  m_hasChild.pop_back();
  if (hadChildren)
  {
    WriteIndent(); // Closing brace lines up with its opening line's indent.
  }
  m_output->push_back('}');
  EndValue();
}

void JsonWriter::BeginArray()
{
  BeginValue();
  m_output->push_back('[');
  m_stack.push_back(false);
  m_hasChild.push_back(false);
  m_expectingValue = false;
}

void JsonWriter::EndArray()
{
  if (m_stack.empty() || m_stack.back())
  {
    m_failed = true;
    return;
  }
  const bool hadChildren = m_hasChild.back();
  m_stack.pop_back();
  m_hasChild.pop_back();
  if (hadChildren)
  {
    WriteIndent();
  }
  m_output->push_back(']');
  EndValue();
}

void JsonWriter::Bool(bool _value)
{
  BeginValue();
  m_output->append(_value ? "true" : "false");
  EndValue();
}

void JsonWriter::Int(std::int64_t _value)
{
  BeginValue();
  char buffer[24];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), _value);
  m_output->append(buffer, result.ptr);
  EndValue();
}

void JsonWriter::Number(double _value)
{
  BeginValue();
  if (!std::isfinite(_value))
  {
    // JSON has no NaN or infinity, and emitting one would produce a file this
    // library's own parser rejects. Fail loudly instead.
    m_failed = true;
    m_output->push_back('0');
    EndValue();
    return;
  }
  char buffer[32];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), _value);
  m_output->append(buffer, result.ptr);
  EndValue();
}

void JsonWriter::String(std::string_view _value)
{
  BeginValue();
  WriteEscaped(_value);
  EndValue();
}

void JsonWriter::Null()
{
  BeginValue();
  m_output->append("null");
  EndValue();
}

void JsonWriter::Member(std::string_view _name, const char* _value)
{
  Member(_name, _value == nullptr ? std::string_view{} : std::string_view(_value));
}

void JsonWriter::Member(std::string_view _name, bool _value)
{
  Key(_name);
  Bool(_value);
}

void JsonWriter::Member(std::string_view _name, std::int64_t _value)
{
  Key(_name);
  Int(_value);
}

void JsonWriter::Member(std::string_view _name, double _value)
{
  Key(_name);
  Number(_value);
}

void JsonWriter::Member(std::string_view _name, std::string_view _value)
{
  Key(_name);
  String(_value);
}

void JsonWriter::WriteEscaped(std::string_view _text)
{
  m_output->push_back('"');
  for (const char c : _text)
  {
    switch (c)
    {
      case '"':
        m_output->append("\\\"");
        break;
      case '\\':
        m_output->append("\\\\");
        break;
      case '\b':
        m_output->append("\\b");
        break;
      case '\f':
        m_output->append("\\f");
        break;
      case '\n':
        m_output->append("\\n");
        break;
      case '\r':
        m_output->append("\\r");
        break;
      case '\t':
        m_output->append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20)
        {
          // Any other control character has to go out as \u00XX.
          char escape[7];
          std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          m_output->append(escape);
        }
        else
        {
          // Everything else, UTF-8 included, is already valid JSON text.
          m_output->push_back(c);
        }
        break;
    }
  }
  m_output->push_back('"');
}

} // namespace Neuron
