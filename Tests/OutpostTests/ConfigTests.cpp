#include "pch.h"
#include "CppUnitTest.h"

#include "AppConfig.h"
#include "Json.h"

#include <string>
#include <string_view>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Outpost;

/*
 * The configuration layers (`AppConfig.h/.cpp`, ADR-012).
 *
 * The first suite the composition root has, and it starts here because this is
 * the code that decides whether a **hand-edited file** is accepted. Everything
 * else in `Outpost.exe` is wiring between things their own suites already
 * cover; this is the one part with a decision in it, and the decision is taken
 * against text a person typed.
 *
 * **Every rule here is a refusal, and each has a reason worth not losing.** A
 * number out of range is an error rather than a clamp, because a clamped value
 * leaves the file saying one thing and the game doing another. A wrong type is
 * an error rather than a coercion, because silent coercion is how a
 * configuration bug hides. An unknown key is a *warning*, because a newer file
 * against an older build should still start. And the user layer accepts only
 * what the settings screen writes, which is what stops a settings file from
 * doing things a settings screen cannot.
 *
 * All of it is pure: a parsed `JsonValue` in, a config and a diagnostics list
 * out. No file is opened by anything in this file, which is why the layering
 * can be tested at all -- `ConfigLoad` owns the paths, and it is not this.
 */

namespace OutpostTests
{
namespace
{

/*
 * Parses `_text` and applies it as the main configuration layer.
 *
 * The document has to outlive the `JsonValue` that points into it, which is why
 * this takes the config and diagnostics by reference rather than returning
 * them: a helper that returned a `JsonValue` would hand back a dangling one.
 */
void ApplyConfig(std::string_view _text, AppConfig& _config, ConfigDiagnostics& _diagnostics)
{
  Neuron::JsonDocument document;
  std::vector<Neuron::JsonError> errors;
  Assert::IsTrue(Neuron::JsonDocument::Parse(_text, document, errors), L"the fixture's own JSON did not parse");
  ApplyConfigLayer(document.Root(), _config, _diagnostics);
}

void ApplyUser(std::string_view _text, AppConfig& _config, ConfigDiagnostics& _diagnostics)
{
  Neuron::JsonDocument document;
  std::vector<Neuron::JsonError> errors;
  Assert::IsTrue(Neuron::JsonDocument::Parse(_text, document, errors), L"the fixture's own JSON did not parse");
  ApplyUserLayer(document.Root(), _config, _diagnostics);
}

/// Whether any diagnostic mentions `_fragment`. Diagnostics are prose meant for
/// a person looking at their own file, so the tests below check that the
/// *path* is in the message rather than pinning the whole sentence.
[[nodiscard]] bool Mentions(const std::vector<std::string>& _messages, std::string_view _fragment)
{
  for (const std::string& message : _messages)
  {
    if (message.find(_fragment) != std::string::npos)
    {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CLASS(ConfigLayerTests)
{
public:
  TEST_METHOD(TheRootMustBeAnObject)
  {
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig("[1, 2, 3]", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "root must be an object"));
  }

  TEST_METHOD(AnEmptyObjectLeavesEveryDefaultStanding)
  {
    // The shipped file is a *layer*, so what it does not mention keeps the
    // value compiled in. A layer that reset unmentioned fields would make every
    // partial config a surprise.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig("{}", config, diagnostics);

    Assert::IsTrue(diagnostics.Ok());
    Assert::AreEqual<std::size_t>(0, diagnostics.warnings.size());
    Assert::IsTrue(config.mode == HostMode::Host);
    Assert::AreEqual<std::uint16_t>(7777, config.server.port);
    Assert::AreEqual<std::uint32_t>(1600, config.client.window.width);
  }

  TEST_METHOD(AnUnknownKeyWarnsAndTheFileStillLoads)
  {
    /*
     * The asymmetry that makes forward compatibility work: a config written for
     * a newer build names settings this one has never heard of, and it has to
     * start anyway. A warning says so without stopping anybody.
     */
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"mode": "headless", "warpDrive": true})", config, diagnostics);

    Assert::IsTrue(diagnostics.Ok(), L"an unknown key is not fatal");
    Assert::IsTrue(Mentions(diagnostics.warnings, "warpDrive"));
    Assert::IsTrue(config.mode == HostMode::Headless, L"and the keys it did understand still applied");
  }

  TEST_METHOD(AWrongTypeIsAnErrorRatherThanACoercion)
  {
    // "yes" is not true, and a layer that decided it was would be guessing at
    // what somebody meant -- which is the whole failure mode ADR-012 §A4 names.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"selfTest": "yes"})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "must be true or false"));
    Assert::IsFalse(config.selfTest, L"and the value is untouched");
  }

  TEST_METHOD(AnOutOfRangeNumberIsRefusedAndNotClamped)
  {
    /*
     * The rule the source comment argues for in one line: "Clamping silently
     * would leave the file saying one thing and the game doing another."
     *
     * So the assertion is in two halves, and the second is the one that matters
     * -- a build that clamped would still report an error *and* quietly move the
     * value, and only this half would notice.
     */
    AppConfig config;
    ConfigDiagnostics diagnostics;
    const double before = config.client.ui.scale;
    ApplyConfig(R"({"client": {"ui": {"scale": 3.0}}})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "client.ui.scale is out of range"));
    Assert::AreEqual(before, config.client.ui.scale, 1e-9, L"refused means unchanged, not clamped to the ceiling");
  }

  TEST_METHOD(APortPastSixteenBitsIsRefusedRatherThanWrapped)
  {
    /*
     * `ReadUInt` reads through `int64` and range-checks *before* narrowing, and
     * this is the case that pays for it: 70000 truncated to sixteen bits is
     * 4464, which is a plausible-looking port nobody asked for.
     */
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"server": {"port": 70000}})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "server.port is out of range"));
    Assert::AreEqual<std::uint16_t>(7777, config.server.port);
    Assert::AreNotEqual<std::uint16_t>(4464, config.server.port, L"the wrap this refusal exists to prevent");
  }

  TEST_METHOD(AWholeNumberFieldRefusesAFraction)
  {
    // A window 1600.5 pixels wide is a typo, and rounding it would be this
    // layer deciding which way.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"client": {"window": {"width": 1600.5}}})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "must be a whole number"));
    Assert::AreEqual<std::uint32_t>(1600, config.client.window.width);
  }

  TEST_METHOD(EveryHostModeInTheEnumParses)
  {
    // Four modes, and the test exists so that adding a fifth to `HostMode`
    // without teaching the parser about it fails here rather than in a launch.
    const struct
    {
      const char* text;
      HostMode mode;
    } cases[] = {
        {R"({"mode": "host"})", HostMode::Host},
        {R"({"mode": "headless"})", HostMode::Headless},
        {R"({"mode": "client"})", HostMode::Client},
        {R"({"mode": "bake"})", HostMode::Bake},
    };

    for (const auto& one : cases)
    {
      AppConfig config;
      ConfigDiagnostics diagnostics;
      ApplyConfig(one.text, config, diagnostics);
      Assert::IsTrue(diagnostics.Ok(), L"a documented mode was refused");
      Assert::IsTrue(config.mode == one.mode);
    }
  }

  TEST_METHOD(AnUnknownModeIsFatalRatherThanADefault)
  {
    // Falling back to Host would start a server on a machine somebody meant to
    // run as a client, which is a worse outcome than not starting.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"mode": "spectator"})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "mode must be"));
  }

  TEST_METHOD(AnEmptyContentListIsAnErrorRatherThanAFallback)
  {
    // The source says it: "a config that says 'no meshes' and gets nine of them
    // is a config that lies."
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"content": {"meshes": []}})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "content.meshes is empty"));
    Assert::IsFalse(config.content.meshes.empty(), L"the defaults are still there, which is why silence would lie");
  }

  TEST_METHOD(AContentListRefusesAnEntryThatIsNotAName)
  {
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"content": {"meshes": ["Miner.obj", 7]}})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "entry 1 must be text"), L"and it says which entry");
  }

  TEST_METHOD(WarningsAloneLeaveTheConfigUsable)
  {
    // `Ok()` is about errors, and the distinction is the whole point of having
    // two lists: a build that treated a warning as fatal would refuse to start
    // on a config from next month.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"nothingKnowsThis": 1, "norThis": {"orThis": 2}})", config, diagnostics);

    Assert::IsTrue(diagnostics.Ok());
    Assert::IsTrue(diagnostics.warnings.size() >= 1);
  }

  TEST_METHOD(ADiagnosticNamesThePathItIsAbout)
  {
    /*
     * Diagnostics are read by somebody looking at their own file, so a message
     * has to say *where*. `client.audio.master` and `client.ui.scale` are both
     * "out of range" and a message that said only that would send them hunting.
     */
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"client": {"audio": {"master": 4.0}, "ui": {"scale": 9.0}}})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "client.audio.master"));
    Assert::IsTrue(Mentions(diagnostics.errors, "client.ui.scale"));
  }

  TEST_METHOD(OneLayerReadsEverySectionRatherThanStoppingAtTheFirstProblem)
  {
    // Two mistakes in one file should both be reported: a pass that returned on
    // the first would make fixing a config a game of whack-a-mole.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"selfTest": 1, "server": {"port": 99999}})", config, diagnostics);

    Assert::IsTrue(diagnostics.errors.size() >= 2, L"both problems are named in one pass");
  }
};

TEST_CLASS(UserLayerTests)
{
public:
  TEST_METHOD(TheUserLayerCannotRedirectTheClientToAnotherServer)
  {
    /*
     * **The invariant this suite exists for.**
     *
     * `ApplyUserLayer` accepts `client.{window, renderer, audio, ui,
     * diagnostics}` and nothing else -- so `client.connect` in a user settings
     * file is ignored. That boundary is held today by one argument list inside
     * `WarnUnknownKeys`, and a widening of it would be a settings file able to
     * point the game at a host the player never chose.
     */
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"client": {"connect": {"host": "10.0.0.1", "port": 31337}}})", config, diagnostics);

    Assert::AreEqual(std::string{"127.0.0.1"}, config.client.connectHost);
    Assert::AreEqual<std::uint16_t>(7777, config.client.connectPort);
    Assert::IsTrue(Mentions(diagnostics.warnings, "connect"), L"ignored, and said so rather than silently");
  }

  TEST_METHOD(TheUserLayerWritesWhatTheSettingsScreenWrites)
  {
    // The other half of the boundary: the five sections it *does* own have to
    // work, or the settings screen has nowhere to save to.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"client": {
                    "window": {"width": 2560, "height": 1440},
                    "renderer": {"vsync": false},
                    "ui": {"scale": 1.25},
                    "diagnostics": {"strip": true}
                  }})",
              config, diagnostics);

    Assert::IsTrue(diagnostics.Ok());
    Assert::AreEqual<std::uint32_t>(2560, config.client.window.width);
    Assert::AreEqual<std::uint32_t>(1440, config.client.window.height);
    Assert::IsFalse(config.client.renderer.vsync);
    Assert::AreEqual(1.25, config.client.ui.scale, 1e-9);
    Assert::IsTrue(config.client.diagnostics.strip);
  }

  TEST_METHOD(TheUserLayerOwnsNothingOutsideTheClient)
  {
    // A settings file is one player's preferences on one machine. The server's
    // port and the universe it loads are not preferences.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"server": {"port": 31337}, "universe": {"definition": "Elsewhere.json"}})", config, diagnostics);

    Assert::AreEqual<std::uint16_t>(7777, config.server.port);
    Assert::AreEqual(std::string{"GameData/Universe/Frontier.json"}, config.universeDefinition);
    Assert::IsTrue(Mentions(diagnostics.warnings, "server"));
  }

  TEST_METHOD(TheUserLayerStillRefusesABadValueInWhatItDoesOwn)
  {
    // Fail-soft is about the file's *shape*, not about its contents: a scale of
    // nine is as wrong here as in the main layer, and is refused the same way.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"client": {"ui": {"scale": 9.0}}})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "client.ui.scale is out of range"));
  }

  TEST_METHOD(AUserFileThatIsNotAnObjectIsIgnoredRatherThanFatal)
  {
    /*
     * ADR-012's posture toward a hand-edited artefact, and the difference
     * between the two layers: a broken `Outpost.json` is the build's own file
     * and stops it, while a broken settings file is one a player edited and
     * costs them their preferences rather than their game.
     */
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser("\"not a settings file\"", config, diagnostics);

    Assert::IsTrue(diagnostics.Ok(), L"a user file cannot stop the game starting");
    Assert::IsTrue(Mentions(diagnostics.warnings, "user settings file"));
  }

  TEST_METHOD(AnEmptyUserFileChangesNothingAndSaysNothing)
  {
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser("{}", config, diagnostics);

    Assert::IsTrue(diagnostics.Ok());
    Assert::AreEqual<std::size_t>(0, diagnostics.warnings.size());
    Assert::AreEqual<std::uint32_t>(1600, config.client.window.width);
  }

  TEST_METHOD(TheTwoLayersStackInOrder)
  {
    // What `LoadAppConfig` does with them: the shipped file first, the player's
    // over the top. The user layer wins where it is entitled to, and only there.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"client": {"window": {"width": 1920}, "connect": {"port": 8000}}})", config, diagnostics);
    Assert::AreEqual<std::uint32_t>(1920, config.client.window.width);
    Assert::AreEqual<std::uint16_t>(8000, config.client.connectPort);

    ApplyUser(R"({"client": {"window": {"width": 2560}, "connect": {"port": 9000}}})", config, diagnostics);
    Assert::AreEqual<std::uint32_t>(2560, config.client.window.width, L"the player's window size wins");
    Assert::AreEqual<std::uint16_t>(8000, config.client.connectPort, L"and their connect settings never applied");
  }
};

} // namespace OutpostTests
