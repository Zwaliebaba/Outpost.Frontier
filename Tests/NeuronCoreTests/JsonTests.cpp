#include "pch.h"
#include "CppUnitTest.h"

#include "Json.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

namespace NeuronCoreTests
{
namespace
{

/// Parses and asserts success, so each test reads as the thing it is checking.
JsonDocument ParseOk(std::string_view _text)
{
  JsonDocument document;
  std::vector<JsonError> errors;
  const bool ok = JsonDocument::Parse(_text, document, errors, {});
  if (!ok)
  {
    std::string message = "unexpected parse failure: ";
    message += errors.empty() ? "no diagnostic" : errors.front().message;
    Assert::Fail(std::wstring(message.begin(), message.end()).c_str());
  }
  return document;
}

/// Parses and asserts failure, returning the diagnostics for inspection.
std::vector<JsonError> ParseFails(std::string_view _text, const JsonLimits& _limits = {})
{
  JsonDocument document;
  std::vector<JsonError> errors;
  Assert::IsFalse(JsonDocument::Parse(_text, document, errors, _limits), L"expected this document to be rejected");
  Assert::IsTrue(!errors.empty(), L"a rejection must say why");
  return errors;
}

} // namespace

TEST_CLASS(JsonParseTests)
{
public:
  TEST_METHOD(ReadsAWholeConfigShapedDocument)
  {
    const JsonDocument document = ParseOk(R"({
      "mode": "host",
      "selfTest": false,
      "server": { "port": 7777, "transport": "udp" },
      "client": { "window": { "width": 1600, "height": 900 } },
      "levels": [1, 2, 3]
    })");

    const JsonValue root = document.Root();
    Assert::IsTrue(root.IsObject());
    Assert::IsTrue(root.Member("mode").AsString() == "host");
    Assert::IsFalse(root.Member("selfTest").AsBool(true));
    Assert::AreEqual<std::int64_t>(7777, root.Member("server").Member("port").AsInt64());
    Assert::IsTrue(root.Member("server").Member("transport").AsString() == "udp");
    Assert::AreEqual<std::int64_t>(1600, root.Member("client").Member("window").Member("width").AsInt64());

    const JsonValue levels = root.Member("levels");
    Assert::IsTrue(levels.IsArray());
    Assert::AreEqual<std::size_t>(3, levels.Count());
    Assert::AreEqual<std::int64_t>(2, levels.At(1).AsInt64());
  }

  TEST_METHOD(MissingPathsFallBackInsteadOfCrashing)
  {
    // Chaining through absent members has to stay safe: a config reader walks
    // paths that may not be there and reports at the end.
    const JsonDocument document = ParseOk(R"({ "a": { "b": 1 } })");
    const JsonValue root = document.Root();

    Assert::IsFalse(root.Member("nope").Valid());
    Assert::IsFalse(root.Member("nope").Member("deeper").Valid());
    Assert::AreEqual<std::int64_t>(42, root.Member("nope").Member("deeper").AsInt64(42));
    Assert::IsTrue(root.Member("a").Member("b").Valid());
  }

  TEST_METHOD(WrongTypeYieldsTheFallback)
  {
    const JsonDocument document = ParseOk(R"({ "text": "hello", "flag": true })");
    const JsonValue root = document.Root();

    Assert::AreEqual<std::int64_t>(-1, root.Member("text").AsInt64(-1));
    Assert::IsTrue(root.Member("flag").AsString("fallback") == "fallback");
    Assert::IsFalse(root.Member("text").AsBool(false));
  }

  TEST_METHOD(KeepsIntegersExactPastTwoToTheFiftyThird)
  {
    // The reason this parser exists in this shape: universe coordinates are
    // int64 metres (ADR-009) and a double-only parser rounds them silently.
    const JsonDocument document = ParseOk(R"({
      "big": 9007199254740993,
      "max": 9223372036854775807,
      "min": -9223372036854775808,
      "negative": -123456789012345
    })");
    const JsonValue root = document.Root();

    Assert::AreEqual<std::int64_t>(9007199254740993ll, root.Member("big").AsInt64());
    Assert::AreEqual<std::int64_t>(9223372036854775807ll, root.Member("max").AsInt64());
    Assert::AreEqual<std::int64_t>(INT64_MIN, root.Member("min").AsInt64());
    Assert::AreEqual<std::int64_t>(-123456789012345ll, root.Member("negative").AsInt64());
  }

  TEST_METHOD(DistinguishesIntegersFromRealNumbers)
  {
    const JsonDocument document = ParseOk(R"({ "i": 3, "d": 3.5, "e": 1e3, "whole": 4.0 })");
    const JsonValue root = document.Root();

    Assert::IsTrue(root.Member("i").Kind() == JsonKind::Integer);
    Assert::IsTrue(root.Member("d").Kind() == JsonKind::Number);
    Assert::IsTrue(root.Member("e").Kind() == JsonKind::Number);

    Assert::AreEqual(3.5, root.Member("d").AsDouble(), 1e-12);
    Assert::AreEqual(1000.0, root.Member("e").AsDouble(), 1e-12);
    // A whole-valued double reads as an integer; a fractional one refuses rather
    // than truncating, because a silently truncated setting is a lie.
    Assert::AreEqual<std::int64_t>(4, root.Member("whole").AsInt64(-1));
    Assert::AreEqual<std::int64_t>(-1, root.Member("d").AsInt64(-1));
  }

  TEST_METHOD(RejectsAnIntegerThatDoesNotFit)
  {
    const auto errors = ParseFails(R"({ "tooBig": 99999999999999999999 })");
    Assert::IsTrue(errors.front().message.find("64 bits") != std::string::npos);
  }

  TEST_METHOD(AcceptsCommentsAndTrailingCommas)
  {
    // The two deliberate relaxations for hand-authored files (ADR-012 §C5).
    const JsonDocument document = ParseOk(R"({
      // the port the server binds
      "port": 7777, /* inline */
      "hosts": [ "a", "b", ],
    })");
    const JsonValue root = document.Root();

    Assert::AreEqual<std::int64_t>(7777, root.Member("port").AsInt64());
    Assert::AreEqual<std::size_t>(2, root.Member("hosts").Count());
  }

  TEST_METHOD(RejectsDuplicateKeys)
  {
    // Last-one-wins would let a file say two things while the reader picks one.
    const auto errors = ParseFails(R"({ "port": 1, "port": 2 })");
    Assert::IsTrue(errors.front().message.find("duplicate") != std::string::npos);
  }

  TEST_METHOD(DecodesEscapesIncludingSurrogatePairs)
  {
    const JsonDocument document = ParseOk(R"({
      "plain": "a\tb\nc",
      "quote": "say \"hi\"",
      "slash": "a\\b",
      "bmp": "é",
      "emoji": "🚀"
    })");
    const JsonValue root = document.Root();

    Assert::IsTrue(root.Member("plain").AsString() == "a\tb\nc");
    Assert::IsTrue(root.Member("quote").AsString() == "say \"hi\"");
    Assert::IsTrue(root.Member("slash").AsString() == "a\\b");
    Assert::IsTrue(root.Member("bmp").AsString() == "\xc3\xa9");          // U+00E9 as UTF-8
    Assert::IsTrue(root.Member("emoji").AsString() == "\xf0\x9f\x9a\x80"); // U+1F680 as UTF-8
  }

  TEST_METHOD(RejectsBrokenSurrogatesAndBadEscapes)
  {
    (void)ParseFails(R"({ "s": "\ud83d" })");        // high surrogate, nothing after it
    (void)ParseFails(R"({ "s": "\udc00" })");        // lone low surrogate
    (void)ParseFails(R"({ "s": "\uZZZZ" })");        // not hex
    (void)ParseFails(R"({ "s": "\q" })");            // unknown escape
    (void)ParseFails("{ \"s\": \"raw\tcontrol\" }"); // raw control character
  }

  TEST_METHOD(RejectsStructuralDamage)
  {
    (void)ParseFails("");
    (void)ParseFails("{");
    (void)ParseFails("[1, 2");
    (void)ParseFails(R"({ "a" 1 })");     // missing colon
    (void)ParseFails(R"({ a: 1 })");      // unquoted key
    (void)ParseFails(R"({ "a": })");      // missing value
    (void)ParseFails(R"({ "a": 1 } junk)"); // trailing content
    (void)ParseFails("{ \"a\": 'x' }");   // single quotes are not JSON
    (void)ParseFails(R"({ "a": NaN })");
  }

  TEST_METHOD(ReportsWhereTheProblemIs)
  {
    const auto errors = ParseFails("{\n  \"a\": 1,\n  \"b\" 2\n}");
    // Line 3 is where the colon is missing; an editor counts from 1.
    Assert::AreEqual<std::uint32_t>(3, errors.front().line);
    Assert::IsTrue(errors.front().column > 1);
  }

  TEST_METHOD(EnforcesTheDepthCap)
  {
    // Deep input must not smash the C++ stack -- the parser is iterative and
    // refuses instead.
    JsonLimits limits;
    limits.maxDepth = 8;

    std::string deep;
    for (int i = 0; i < 64; ++i)
    {
      deep += '[';
    }
    deep += '1';
    for (int i = 0; i < 64; ++i)
    {
      deep += ']';
    }

    const auto errors = ParseFails(deep, limits);
    Assert::IsTrue(errors.front().message.find("deep") != std::string::npos);

    // Just inside the cap still parses.
    JsonDocument document;
    std::vector<JsonError> ok;
    Assert::IsTrue(JsonDocument::Parse("[[[[1]]]]", document, ok, limits));
  }

  TEST_METHOD(EnforcesTheSizeCap)
  {
    JsonLimits limits;
    limits.maxBytes = 8;
    (void)ParseFails(R"({ "key": "a much longer document than eight bytes" })", limits);
  }

  TEST_METHOD(TakesAByteOrderMarkAndScalarRoots)
  {
    const JsonDocument bom = ParseOk("\xef\xbb\xbf{ \"a\": 1 }");
    Assert::AreEqual<std::int64_t>(1, bom.Root().Member("a").AsInt64());

    const JsonDocument scalar = ParseOk("42");
    Assert::AreEqual<std::int64_t>(42, scalar.Root().AsInt64());

    const JsonDocument empty = ParseOk("{}");
    Assert::IsTrue(empty.Root().IsObject());
    Assert::AreEqual<std::size_t>(0, empty.Root().Count());
  }

  TEST_METHOD(KeepsMemberNamesAndLines)
  {
    const JsonDocument document = ParseOk("{\n  \"first\": 1,\n  \"second\": 2\n}");
    const JsonValue root = document.Root();

    Assert::IsTrue(root.At(0).Name() == "first");
    Assert::IsTrue(root.At(1).Name() == "second");
    Assert::AreEqual<std::uint32_t>(2, root.At(0).Line());
    Assert::AreEqual<std::uint32_t>(3, root.At(1).Line());
  }
};

} // namespace NeuronCoreTests
