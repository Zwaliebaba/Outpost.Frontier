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

/// A failure message that names the key it is about. `Assert` takes a wide
/// string and these keys are narrow literals, so the widening happens once here
/// rather than at nineteen call sites. ASCII throughout: they are JSON keys
/// this file spells itself.
[[nodiscard]] std::wstring KeyMessage(const char* _key, const wchar_t* _what)
{
  std::wstring message;
  for (const char* c = _key; *c != '\0'; ++c)
  {
    message.push_back(static_cast<wchar_t>(*c));
  }
  message += L' ';
  message += _what;
  return message;
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

/*
 * Writing the user layer (`WriteUserLayer`, N2 / ADR-012 §A3).
 *
 * A second class rather than more methods on the first, because the property
 * under test is different in kind. The layer tests above ask what a *file* is
 * allowed to say; these ask whether what this program writes is what it reads
 * back -- and the answer has to be yes for every key, or a setting is one that
 * silently does not survive a restart.
 *
 * Still pure: text out, text in, no file opened. `SaveUserSettings` owns the
 * path, the temporary and the rename, and it is not this.
 */
TEST_CLASS(UserLayerWriterTests)
{
public:
  /// Write `_config` against `_shipped`, then read the result back over a fresh
  /// copy of `_shipped`. What comes out is what the next boot would see.
  static void RoundTrip(const AppConfig& _config, const AppConfig& _shipped, AppConfig& _outReloaded, std::string& _outText)
  {
    Assert::IsTrue(WriteUserLayer(_config, _shipped, _outText), L"the writer did not finish its document");

    _outReloaded = _shipped;
    ConfigDiagnostics diagnostics;
    ApplyUser(_outText, _outReloaded, diagnostics);
    Assert::IsTrue(diagnostics.Ok(), L"the file this program wrote is one it refuses to read");
  }

  TEST_METHOD(APlayerWhoChangedNothingGetsAnEmptyDocument)
  {
    // The property that keeps an untouched installation free of a settings
    // file at all: `SaveUserSettings` declines to create one for "{}".
    const AppConfig shipped;
    std::string text;
    Assert::IsTrue(WriteUserLayer(shipped, shipped, text));
    Assert::AreEqual(std::string("{}"), text);
  }

  TEST_METHOD(OnlyTheChangedKeyIsWritten)
  {
    /*
     * The whole reason the writer takes a baseline. A file that captured the
     * shipped values would go on overriding them after the shipped file moved
     * on, and the player would be pinned to last year's defaults by a file they
     * never edited.
     */
    AppConfig shipped;
    shipped.client.window.width = 1920;
    shipped.client.audio.master = 0.5;

    AppConfig config = shipped;
    config.client.audio.master = 0.25;

    std::string text;
    AppConfig reloaded;
    RoundTrip(config, shipped, reloaded, text);

    Assert::IsTrue(text.find("master") != std::string::npos, L"the value the player changed is not in the file");
    Assert::IsTrue(text.find("width") == std::string::npos, L"a value the player never touched was captured");
    Assert::IsTrue(text.find("window") == std::string::npos, L"an untouched section was written as an empty object");
    Assert::AreEqual(0.25, reloaded.client.audio.master, 1e-9);
  }

  TEST_METHOD(EveryKeyTheUserLayerReadsIsAKeyItWrites)
  {
    /*
     * The asymmetry this is here to catch is silent: a key `ApplyUserLayer`
     * accepts and `WriteUserLayer` never emits is a setting the player can
     * change, can see take effect, and loses on the next boot with nothing
     * anywhere saying why.
     *
     * Every value below differs from the default, so every one of them has to
     * make the round trip.
     */
    const AppConfig shipped;
    AppConfig config = shipped;
    config.client.window.width = 2560;
    config.client.window.height = 1440;
    config.client.window.mode = "borderless";
    config.client.renderer.vsync = false;
    config.client.renderer.msaa = 8;
    config.client.renderer.frameCap = 144;
    config.client.renderer.hullScale = 1.75;
    config.client.audio.enabled = false;
    config.client.audio.voiceCap3D = 64;
    config.client.audio.voiceCap2D = 32;
    config.client.audio.master = 0.9;
    config.client.audio.world = 0.8;
    config.client.audio.ui = 0.7;
    config.client.audio.music = 0.35;
    config.client.audio.alerts = 0.5;
    config.client.audio.ambience = 0.4;
    config.client.ui.scale = 1.25;
    config.client.ui.palette = "high-contrast";
    config.client.ui.font = "Cascadia Mono";
    config.client.diagnostics.strip = true;

    std::string text;
    AppConfig reloaded;
    RoundTrip(config, shipped, reloaded, text);

    Assert::AreEqual<std::uint32_t>(2560, reloaded.client.window.width);
    Assert::AreEqual<std::uint32_t>(1440, reloaded.client.window.height);
    Assert::AreEqual(std::string("borderless"), reloaded.client.window.mode);
    Assert::IsFalse(reloaded.client.renderer.vsync);
    Assert::AreEqual<std::uint32_t>(8, reloaded.client.renderer.msaa);
    Assert::AreEqual<std::uint32_t>(144, reloaded.client.renderer.frameCap);
    Assert::AreEqual(1.75, reloaded.client.renderer.hullScale, 1e-9);
    Assert::IsFalse(reloaded.client.audio.enabled);
    Assert::AreEqual<std::uint32_t>(64, reloaded.client.audio.voiceCap3D);
    Assert::AreEqual<std::uint32_t>(32, reloaded.client.audio.voiceCap2D);
    Assert::AreEqual(0.9, reloaded.client.audio.master, 1e-9);
    Assert::AreEqual(0.8, reloaded.client.audio.world, 1e-9);
    Assert::AreEqual(0.7, reloaded.client.audio.ui, 1e-9);
    Assert::AreEqual(0.35, reloaded.client.audio.music, 1e-9);
    Assert::AreEqual(0.5, reloaded.client.audio.alerts, 1e-9);
    Assert::AreEqual(0.4, reloaded.client.audio.ambience, 1e-9);
    Assert::AreEqual(1.25, reloaded.client.ui.scale, 1e-9);
    Assert::AreEqual(std::string("high-contrast"), reloaded.client.ui.palette);
    Assert::AreEqual(std::string("Cascadia Mono"), reloaded.client.ui.font);
    Assert::IsTrue(reloaded.client.diagnostics.strip);

    /*
     * And every key is actually *in* the file.
     *
     * The equality checks above are necessary and not sufficient: a value that
     * happens to match the shipped default is not written, is not read, and
     * still compares equal -- so a key the writer forgot passes silently. This
     * caught exactly that while the slice was being built, on `music`, whose
     * fixture value was the default.
     */
    for (const char* key : {"width", "height", "mode", "vsync", "msaa", "frameCap", "hullScale", "enabled", "voiceCap3D",
                            "voiceCap2D", "master", "world", "music", "alerts", "ambience", "scale", "palette", "font", "strip"})
    {
      Assert::IsTrue(text.find(key) != std::string::npos,
                     KeyMessage(key, L"is a key the user layer reads and the writer never emits").c_str());
    }
  }

  TEST_METHOD(WingNamesSurviveTheRoundTrip)
  {
    // 🏁 H1's last clause, at the layer that has to hold it up: rename a wing
    // and see it survive a restart.
    const AppConfig shipped;
    AppConfig config = shipped;
    config.wings.push_back(WingName{9, "VERGE"});
    config.wings.push_back(WingName{2, "TALON PRIME"});

    std::string text;
    AppConfig reloaded;
    RoundTrip(config, shipped, reloaded, text);

    Assert::AreEqual<std::size_t>(2, reloaded.wings.size());
    // Sorted by number on the way out, so the file is the same file for the
    // same settings however the session happened to mint them.
    Assert::AreEqual<std::uint32_t>(2, reloaded.wings[0].wing);
    Assert::AreEqual(std::string("TALON PRIME"), reloaded.wings[0].name);
    Assert::AreEqual<std::uint32_t>(9, reloaded.wings[1].wing);
    Assert::AreEqual(std::string("VERGE"), reloaded.wings[1].name);
  }

  TEST_METHOD(TheSameSettingsMintedInAnyOrderProduceTheSameFile)
  {
    /*
     * What makes `SaveUserSettings`'s "write nothing when nothing changed"
     * actually hold. Names arrive in the order the session minted them; without
     * a canonical order the same settings would rewrite the file on every exit
     * and a diff would show a reshuffle where nothing had changed.
     */
    const AppConfig shipped;
    AppConfig first = shipped;
    first.wings.push_back(WingName{9, "VERGE"});
    first.wings.push_back(WingName{2, "ANVIL"});

    AppConfig second = shipped;
    second.wings.push_back(WingName{2, "ANVIL"});
    second.wings.push_back(WingName{9, "VERGE"});

    std::string firstText;
    std::string secondText;
    Assert::IsTrue(WriteUserLayer(first, shipped, firstText));
    Assert::IsTrue(WriteUserLayer(second, shipped, secondText));
    Assert::AreEqual(firstText, secondText);
  }

  TEST_METHOD(AWingNameWithAQuoteInItComesBackWhole)
  {
    // The player types the name, so it is the one string in this file that is
    // not authored content. The writer escapes; the parser unescapes; nothing
    // in between gets to be clever about it.
    const AppConfig shipped;
    AppConfig config = shipped;
    config.wings.push_back(WingName{3, R"(THE "REAL" WING	\)"});

    std::string text;
    AppConfig reloaded;
    RoundTrip(config, shipped, reloaded, text);

    Assert::AreEqual<std::size_t>(1, reloaded.wings.size());
    Assert::AreEqual(std::string(R"(THE "REAL" WING	\)"), reloaded.wings[0].name);
  }
};

/*
 * Reading the `wings` family (`ApplyUserLayer`, ADR-012 §3).
 *
 * Every refusal here drops the **whole** list rather than the bad entry. A
 * partial roster of call signs is the failure hardest to see -- one wing renamed
 * and one not, with nothing saying why -- while losing all of them says
 * something is wrong with the file, which is true.
 */
TEST_CLASS(WingNameLayerTests)
{
public:
  TEST_METHOD(AListOfIdAndNamePairsIsRead)
  {
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"wings": [{"id": 9, "name": "VERGE"}, {"id": 14, "name": "SLATE"}]})", config, diagnostics);

    Assert::IsTrue(diagnostics.Ok());
    Assert::AreEqual<std::size_t>(2, config.wings.size());
    Assert::AreEqual<std::uint32_t>(9, config.wings[0].wing);
    Assert::AreEqual(std::string("VERGE"), config.wings[0].name);
    Assert::AreEqual<std::uint32_t>(14, config.wings[1].wing);
  }

  TEST_METHOD(WingZeroIsRefused)
  {
    // `INVALID_WING_ID`: the wing the stations are in, which draws no row. A
    // name for it would be a word nothing can ever show.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"wings": [{"id": 0, "name": "NOWHERE"}]})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "wings[0].id"));
    Assert::IsTrue(config.wings.empty());
  }

  TEST_METHOD(AWingPastTheIdRangeIsRefusedRatherThanWrapped)
  {
    // `Game::WingId` is a u8, so 256 is not a wing -- and narrowing it silently
    // would rename wing 0.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"wings": [{"id": 256, "name": "OVERFLOW"}]})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(config.wings.empty());
  }

  TEST_METHOD(AnEmptyNameIsRefused)
  {
    // Worse than an unnamed wing: the roster would draw a row the player can
    // neither read nor point at.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"wings": [{"id": 4, "name": ""}]})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.errors, "wings[0].name"));
  }

  TEST_METHOD(TwoNamesForOneWingIsRefused)
  {
    // Which one wins would be an arbitrary answer to a question the file should
    // not be able to ask.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"wings": [{"id": 4, "name": "FIRST"}, {"id": 4, "name": "SECOND"}]})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(config.wings.empty());
  }

  TEST_METHOD(ABadEntryLosesTheWholeListRatherThanItself)
  {
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"wings": [{"id": 4, "name": "GOOD"}, {"id": 5, "name": 7}]})", config, diagnostics);

    Assert::IsFalse(diagnostics.Ok());
    Assert::IsTrue(config.wings.empty(), L"a partial roster of call signs is worse than none");
  }

  TEST_METHOD(WingsAreNotAKeyTheShippedConfigOwns)
  {
    // The starting fleet's names are content in the composition root. A shipped
    // file reaching for the player's vocabulary is an unknown key.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyConfig(R"({"wings": [{"id": 9, "name": "VERGE"}]})", config, diagnostics);

    Assert::IsTrue(diagnostics.Ok(), L"an unknown key must not stop an older build starting");
    Assert::IsTrue(Mentions(diagnostics.warnings, "wings"));
    Assert::IsTrue(config.wings.empty());
  }

  TEST_METHOD(AnUnknownKeyInsideAWingWarnsAndTheEntryStands)
  {
    // Forward compatibility, the same rule the rest of the file runs under: a
    // newer build's extra field must not cost an older build the name.
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ApplyUser(R"({"wings": [{"id": 9, "name": "VERGE", "colour": "amber"}]})", config, diagnostics);

    Assert::IsTrue(diagnostics.Ok());
    Assert::IsTrue(Mentions(diagnostics.warnings, "wings[0].colour"));
    Assert::AreEqual<std::size_t>(1, config.wings.size());
  }
};

} // namespace OutpostTests
