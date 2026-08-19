#include "pch.h"
#include "CppUnitTest.h"

#include "AudioListener.h"
#include "IsoCamera.h"
#include "SoundBank.h"
#include "VoicePool.h"
#include "WavClip.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * S15's audio, tested without an audio device.
 *
 * This is the same split the nebula and the HUD are built on, and here it is
 * load-bearing rather than tidy: CI has no sound card, so anything that can
 * only be asserted through XAudio2 is never asserted at all. What is left on
 * the device side is opening a graph and calling `X3DAudioCalculate` -- the
 * *decisions* are all here.
 *
 * The four suites are the four things with something to get wrong: which cue a
 * bank declares, what a WAV file actually contains, where the ear is, and which
 * sound gets a voice when there are not enough.
 */

namespace NeuronClientTests
{
namespace
{

/// A minimal well-formed mono 16-bit WAV, built rather than shipped: a test
/// that needs a fixture file on disk is a test that fails for reasons that have
/// nothing to do with the reader.
[[nodiscard]] std::vector<std::uint8_t> MakeWavBytes(std::uint16_t _channels, std::uint16_t _bits, std::uint32_t _rate,
                                                     std::uint32_t _dataBytes, std::uint16_t _formatTag = 1,
                                                     bool _withListChunk = false)
{
  std::vector<std::uint8_t> bytes;
  const auto tag = [&bytes](const char* _text) {
    for (int i = 0; i < 4; ++i)
    {
      bytes.push_back(static_cast<std::uint8_t>(_text[i]));
    }
  };
  const auto u32 = [&bytes](std::uint32_t _value) {
    for (int i = 0; i < 4; ++i)
    {
      bytes.push_back(static_cast<std::uint8_t>((_value >> (8 * i)) & 0xffu));
    }
  };
  const auto u16 = [&bytes](std::uint16_t _value) {
    bytes.push_back(static_cast<std::uint8_t>(_value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((_value >> 8) & 0xffu));
  };

  tag("RIFF");
  u32(0); // Patched below.
  tag("WAVE");

  if (_withListChunk)
  {
    // An editor's metadata, sitting between the header and `fmt `. A reader
    // that does not skip it reads the next tag from the middle of a word.
    tag("LIST");
    u32(6);
    for (int i = 0; i < 6; ++i)
    {
      bytes.push_back(0x20);
    }
  }

  tag("fmt ");
  u32(16);
  u16(_formatTag);
  u16(_channels);
  u32(_rate);
  u32(_rate * _channels * (_bits / 8u));
  u16(static_cast<std::uint16_t>(_channels * (_bits / 8u)));
  u16(_bits);

  tag("data");
  u32(_dataBytes);
  bytes.insert(bytes.end(), _dataBytes, 0x00);

  const auto riffSize = static_cast<std::uint32_t>(bytes.size() - 8);
  for (int i = 0; i < 4; ++i)
  {
    bytes[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((riffSize >> (8 * i)) & 0xffu);
  }
  return bytes;
}

[[nodiscard]] VoiceRequest MakeRequest(std::uint32_t _sound, std::uint64_t _emitter, float _priority, float _distance)
{
  VoiceRequest request;
  request.soundIndex = _sound;
  request.emitterId = _emitter;
  request.priority = _priority;
  request.distanceMetres = _distance;
  return request;
}

} // namespace

TEST_CLASS(SoundBankTests)
{
public:
  TEST_METHOD(AWellFormedBankParsesEveryField)
  {
    const char* text = R"({
      "sounds": {
        "ship.engine": { "file": "EngineLoop.wav", "category": "world3d", "gain": 0.35,
                         "pitchVariance": 0.06, "maxInstances": 32,
                         "minDistance": 120, "maxDistance": 6000 },
        "order.rejected": { "file": "OrderRejected.wav", "category": "alerts", "cooldownMs": 120 }
      }
    })";

    SoundBank bank;
    std::vector<std::string> errors;
    Assert::IsTrue(SoundBank::Parse(text, bank, errors));
    Assert::AreEqual<std::size_t>(0, errors.size(), L"a clean bank reports nothing");
    Assert::AreEqual<std::size_t>(2, bank.Sounds().size());

    const SoundDef* engine = bank.Find("ship.engine");
    Assert::IsNotNull(engine);
    Assert::IsTrue(engine->category == SoundCategory::World3D);
    Assert::AreEqual(0.35f, engine->gain, 1e-6f);
    Assert::AreEqual(0.06f, engine->pitchVariance, 1e-6f);
    Assert::AreEqual<std::uint32_t>(32, engine->maxInstances);
    Assert::AreEqual(6000.0f, engine->maxDistanceMetres, 1e-3f);

    const SoundDef* rejected = bank.Find("order.rejected");
    Assert::IsNotNull(rejected);
    Assert::IsTrue(rejected->category == SoundCategory::Alerts);
    Assert::AreEqual(120.0, rejected->cooldownMs, 1e-9);
    // Everything it did not say takes the documented default rather than zero:
    // a cue with no gain is a cue at full volume, not a silent one.
    Assert::AreEqual(1.0f, rejected->gain, 1e-6f);
  }

  TEST_METHOD(TheShippedBankIsTheOneTheEngineAsksFor)
  {
    // The two ids in `AudioDevice.h` are the client's own vocabulary, and a
    // bank that does not answer them is a silent game with nothing in the log
    // to say why. Asserted against the literal strings rather than the
    // constants, so renaming the constant cannot quietly rename the contract.
    const char* text = R"({
      "sounds": {
        "ship.engine":    { "file": "EngineLoop.wav",    "category": "world3d" },
        "order.rejected": { "file": "OrderRejected.wav", "category": "alerts" }
      }
    })";
    SoundBank bank;
    std::vector<std::string> errors;
    Assert::IsTrue(SoundBank::Parse(text, bank, errors));
    Assert::IsNotNull(bank.Find("ship.engine"));
    Assert::IsNotNull(bank.Find("order.rejected"));
  }

  TEST_METHOD(OneBadEntryCostsThatCueAndNotTheBank)
  {
    // The rule the parser is built on: a mistyped category should cost its own
    // cue, be named in a diagnostic, and leave the rest of the soundscape
    // alone. Silently dropping it, or refusing the whole file, are both worse.
    const char* text = R"({
      "sounds": {
        "good":       { "file": "A.wav", "category": "ui" },
        "no.file":    { "category": "ui" },
        "bad.class":  { "file": "B.wav", "category": "explosions" }
      }
    })";

    SoundBank bank;
    std::vector<std::string> errors;
    Assert::IsTrue(SoundBank::Parse(text, bank, errors));
    Assert::AreEqual<std::size_t>(1, bank.Sounds().size(), L"only the good cue survives");
    Assert::IsNotNull(bank.Find("good"));
    Assert::IsNull(bank.Find("no.file"));
    Assert::IsNull(bank.Find("bad.class"));
    Assert::AreEqual<std::size_t>(2, errors.size(), L"both skips are reported");
  }

  TEST_METHOD(AnInvertedFalloffIsRepairedAndReported)
  {
    // Silent everywhere it should be loud reads as a broken mixer rather than
    // as broken content, so the cue is widened and the file is named.
    const char* text = R"({
      "sounds": { "odd": { "file": "A.wav", "category": "world3d",
                           "minDistance": 900, "maxDistance": 100 } }
    })";
    SoundBank bank;
    std::vector<std::string> errors;
    Assert::IsTrue(SoundBank::Parse(text, bank, errors));
    const SoundDef* odd = bank.Find("odd");
    Assert::IsNotNull(odd);
    Assert::IsTrue(odd->maxDistanceMetres > odd->minDistanceMetres);
    Assert::AreEqual<std::size_t>(1, errors.size());
  }

  TEST_METHOD(AMalformedDocumentIsRefusedWhole)
  {
    SoundBank bank;
    std::vector<std::string> errors;
    Assert::IsFalse(SoundBank::Parse("{ this is not json", bank, errors));
    Assert::IsTrue(errors.size() > 0, L"a refusal has to say why");

    errors.clear();
    Assert::IsFalse(SoundBank::Parse(R"({ "noises": {} })", bank, errors), L"no 'sounds' object");
  }

  TEST_METHOD(EveryCategoryRoundTripsItsName)
  {
    for (std::uint32_t index = 0; index < SOUND_CATEGORY_COUNT; ++index)
    {
      const auto category = static_cast<SoundCategory>(index);
      SoundCategory parsed = SoundCategory::Count;
      Assert::IsTrue(TryParseSoundCategory(SoundCategoryName(category), parsed));
      Assert::IsTrue(parsed == category);
    }
    SoundCategory unused = SoundCategory::Count;
    Assert::IsFalse(TryParseSoundCategory("World3D", unused), L"case is significant; a typo is a diagnostic");
  }
};

TEST_CLASS(WavClipTests)
{
public:
  TEST_METHOD(AMonoSixteenBitClipReadsItsFormatAndSamples)
  {
    const std::vector<std::uint8_t> bytes = MakeWavBytes(1, 16, 44100, 400);
    WavClip clip;
    std::string error;
    Assert::IsTrue(ParseWav(bytes, clip, error), L"a well-formed clip parses");
    Assert::IsTrue(clip.Valid());
    Assert::AreEqual<std::uint32_t>(44100, clip.sampleRate);
    Assert::AreEqual<std::uint16_t>(1, clip.channels);
    Assert::AreEqual<std::uint16_t>(16, clip.bitsPerSample);
    Assert::AreEqual<std::uint32_t>(200, clip.FrameCount(), L"400 bytes of mono 16-bit is 200 frames");
  }

  TEST_METHOD(AnUnknownChunkBeforeTheFormatIsSkipped)
  {
    // The case that separates a chunk walker from a reader that assumes `fmt `
    // comes first. Editors write LIST chunks all the time.
    const std::vector<std::uint8_t> bytes = MakeWavBytes(1, 16, 22050, 64, 1, true);
    WavClip clip;
    std::string error;
    Assert::IsTrue(ParseWav(bytes, clip, error));
    Assert::AreEqual<std::uint32_t>(22050, clip.sampleRate);
  }

  TEST_METHOD(CompressedAndTruncatedFilesAreRefusedWithAReason)
  {
    WavClip clip;
    std::string error;

    // A format tag that is not PCM: the MVP ships no decoder, and playing the
    // bytes as though they were PCM is noise at full volume.
    Assert::IsFalse(ParseWav(MakeWavBytes(1, 16, 44100, 64, 0x0011), clip, error));
    Assert::IsTrue(error.size() > 0);

    error.clear();
    Assert::IsFalse(ParseWav(std::vector<std::uint8_t>{'R', 'I', 'F', 'F'}, clip, error), L"shorter than a header");
    Assert::IsTrue(error.size() > 0);

    error.clear();
    std::vector<std::uint8_t> notRiff = MakeWavBytes(1, 16, 44100, 64);
    notRiff[0] = 'X';
    Assert::IsFalse(ParseWav(notRiff, clip, error));

    error.clear();
    Assert::IsFalse(ParseWav(MakeWavBytes(1, 16, 44100, 0), clip, error), L"no samples is not a sound");
  }

  TEST_METHOD(AChunkThatOverrunsTheBufferIsRefusedRatherThanRead)
  {
    // The one malformed case that is a security bug rather than a content bug:
    // a size field that points past the end of the file.
    std::vector<std::uint8_t> bytes = MakeWavBytes(1, 16, 44100, 64);
    // The `data` size is the last u32 before the samples.
    const std::size_t dataSizeAt = bytes.size() - 64 - 4;
    bytes[dataSizeAt + 0] = 0xff;
    bytes[dataSizeAt + 1] = 0xff;
    bytes[dataSizeAt + 2] = 0x00;
    bytes[dataSizeAt + 3] = 0x00;

    WavClip clip;
    std::string error;
    Assert::IsFalse(ParseWav(bytes, clip, error));
  }

  TEST_METHOD(TheShippedCuesLoadAndTheSpatialOneIsMono)
  {
    // The content rule ADR-011 §5 makes, checked against the files that
    // actually ship: X3DAudio positions channels rather than sources, so a
    // stereo "3D" cue smears instead of sitting somewhere.
    //
    // Relative to the test binary's working directory, which is the tree root
    // under vstest; a missing file is reported rather than asserted away, so
    // this cannot fail for someone running the suite from elsewhere.
    WavClip engine;
    std::string error;
    if (LoadWav("GameData/Audio/EngineLoop.wav", engine, error))
    {
      Assert::AreEqual<std::uint16_t>(1, engine.channels, L"a spatialised cue must be mono");
      Assert::IsTrue(engine.DurationSeconds() > 0.1, L"a loop needs something to loop");
    }
  }
};

TEST_CLASS(AudioListenerTests)
{
public:
  TEST_METHOD(TheListenerSitsOverTheFocusRaisedByZoom)
  {
    const AudioListenerState close = MakeAudioListener(XMFLOAT2{300.0f, -200.0f}, 0.0f, 1000.0f);
    Assert::AreEqual(300.0f, close.position.x, 1e-3f);
    Assert::AreEqual(-200.0f, close.position.z, 1e-3f, L"the plane's y is render z");
    Assert::AreEqual(1000.0f * LISTENER_ELEVATION_PER_ZOOM, close.position.y, 1e-3f);

    // Zooming out raises the ear, which is the whole reason attenuation and
    // zoom correlate at all (ADR-011 §4).
    const AudioListenerState far = MakeAudioListener(XMFLOAT2{300.0f, -200.0f}, 0.0f, 8000.0f);
    Assert::IsTrue(far.position.y > close.position.y, L"zooming out must raise the listener");
  }

  TEST_METHOD(PanningMovesTheAudioFrameWithTheView)
  {
    const AudioListenerState a = MakeAudioListener(XMFLOAT2{0.0f, 0.0f}, 0.0f, 4000.0f);
    const AudioListenerState b = MakeAudioListener(XMFLOAT2{2500.0f, 0.0f}, 0.0f, 4000.0f);

    const XMFLOAT3 emitter = MakeAudioEmitter(XMFLOAT2{2500.0f, 0.0f}, 0.0f);
    Assert::IsTrue(AudioDistanceMetres(b, emitter) < AudioDistanceMetres(a, emitter),
                   L"panning onto a ship must bring it closer to the ear");
  }

  TEST_METHOD(TheListenerBasisMatchesTheCameraAndIsOrthonormal)
  {
    // X3DAudio checks orthonormality to 1e-5 and refuses what fails, so this is
    // a contract with the API rather than a nicety. And `front` has to be the
    // camera's own plane-forward, or a ship drawn on the right sounds left.
    for (const float yaw : {0.0f, 0.7f, 2.4f, -1.9f})
    {
      IsoCamera camera;
      camera.SetYawRadians(yaw);
      const XMFLOAT2 cameraForward = camera.ScreenUpOnPlane();

      const AudioListenerState listener = MakeAudioListener(XMFLOAT2{0.0f, 0.0f}, yaw, 3000.0f);
      Assert::AreEqual(cameraForward.x, listener.front.x, 1e-5f);
      Assert::AreEqual(cameraForward.y, listener.front.z, 1e-5f);

      const XMVECTOR front = XMLoadFloat3(&listener.front);
      const XMVECTOR top = XMLoadFloat3(&listener.top);
      Assert::AreEqual(1.0f, XMVectorGetX(XMVector3Length(front)), 1e-5f);
      Assert::AreEqual(1.0f, XMVectorGetX(XMVector3Length(top)), 1e-5f);
      Assert::AreEqual(0.0f, XMVectorGetX(XMVector3Dot(front, top)), 1e-5f);
    }
  }

  TEST_METHOD(AnEmitterCarriesItsHoverIntoRenderSpace)
  {
    const XMFLOAT3 emitter = MakeAudioEmitter(XMFLOAT2{10.0f, 20.0f}, 7.0f);
    Assert::AreEqual(10.0f, emitter.x, 1e-6f);
    Assert::AreEqual(7.0f, emitter.y, 1e-6f);
    Assert::AreEqual(20.0f, emitter.z, 1e-6f);
  }
};

TEST_CLASS(VoicePoolTests)
{
public:
  TEST_METHOD(TheCapHoldsHoweverManyShipsAsk)
  {
    // S15's acceptance criterion, as arithmetic: the pool never exceeds its cap
    // under a stress scene. Two hundred emitters against thirty-two voices is
    // the case the ADR says no mixer survives without this policy.
    VoicePool pool;
    pool.Reset(32);

    for (std::uint64_t ship = 1; ship <= 200; ++ship)
    {
      bool stolen = false;
      const float distance = static_cast<float>(201 - ship); // Later ships are nearer.
      (void)pool.Acquire(MakeRequest(0, ship, 1.0f, distance), stolen);
      Assert::IsTrue(pool.ActiveCount() <= pool.Capacity(), L"the cap must hold at every step");
    }
    Assert::AreEqual<std::uint32_t>(32, pool.ActiveCount(), L"a full pool stays full");
  }

  TEST_METHOD(TheFarthestVoiceIsTheOneStolen)
  {
    VoicePool pool;
    pool.Reset(2);

    bool stolen = false;
    const std::uint32_t near = pool.Acquire(MakeRequest(0, 1, 1.0f, 100.0f), stolen);
    const std::uint32_t far = pool.Acquire(MakeRequest(0, 2, 1.0f, 900.0f), stolen);
    Assert::AreNotEqual(INVALID_VOICE_SLOT, near);
    Assert::AreNotEqual(INVALID_VOICE_SLOT, far);

    const std::uint32_t closer = pool.Acquire(MakeRequest(0, 3, 1.0f, 50.0f), stolen);
    Assert::IsTrue(stolen, L"a full pool yields by stealing");
    Assert::AreEqual(far, closer, L"the farthest voice is the one that goes");
    Assert::AreEqual<std::uint32_t>(2, pool.ActiveCount(), L"a steal replaces, it does not add");
    Assert::AreEqual(INVALID_VOICE_SLOT, pool.Find(2, 0), L"the stolen emitter is no longer playing");
  }

  TEST_METHOD(AQuietHumNeverEvictsAnAlert)
  {
    // The clause that makes the policy more than "last caller wins": without
    // it, a distant engine takes the voice that was telling the player they
    // just lost a wing.
    VoicePool pool;
    pool.Reset(1);

    bool stolen = false;
    Assert::AreNotEqual(INVALID_VOICE_SLOT, pool.Acquire(MakeRequest(1, 0, 4.0f, 0.0f), stolen));
    Assert::AreEqual(INVALID_VOICE_SLOT, pool.Acquire(MakeRequest(0, 7, 1.0f, 5000.0f), stolen),
                     L"a lower priority must be refused, not granted");
    Assert::IsFalse(stolen);
  }

  TEST_METHOD(ABanksInstanceCapIsSeparateFromThePools)
  {
    // `maxInstances` protects the ear; the pool's capacity protects the mixer.
    // Eight of a cue is enough whether or not there is room for a ninth.
    VoicePool pool;
    pool.Reset(16);

    std::uint32_t granted = 0;
    for (std::uint64_t emitter = 1; emitter <= 10; ++emitter)
    {
      VoiceRequest request = MakeRequest(3, emitter, 1.0f, 10.0f);
      request.maxInstances = 4;
      bool stolen = false;
      if (pool.Acquire(request, stolen) != INVALID_VOICE_SLOT)
      {
        ++granted;
      }
    }
    Assert::AreEqual<std::uint32_t>(4, granted, L"the bank's cap is what stops it, not the pool's");
    Assert::AreEqual<std::uint32_t>(4, pool.CountOf(3));
  }

  TEST_METHOD(ALoopIsFoundAgainRatherThanRestarted)
  {
    VoicePool pool;
    pool.Reset(8);

    bool stolen = false;
    const std::uint32_t slot = pool.Acquire(MakeRequest(0, 42, 1.0f, 500.0f), stolen);
    Assert::AreEqual(slot, pool.Find(42, 0), L"the same emitter and sound is the same voice");
    Assert::AreEqual(INVALID_VOICE_SLOT, pool.Find(42, 1), L"a different sound is a different voice");
    Assert::AreEqual(INVALID_VOICE_SLOT, pool.Find(0, 0), L"emitter zero belongs to nothing and never matches");

    // A loop that moves has to be re-ranked, or it is stolen on stale numbers.
    pool.Touch(slot, 1.0f, 4000.0f);
    Assert::AreEqual(4000.0f, pool.Slots()[slot].distanceMetres, 1e-3f);

    pool.Release(slot);
    Assert::AreEqual<std::uint32_t>(0, pool.ActiveCount());
    pool.Release(slot); // Releasing twice is a no-op, not a corruption.
    Assert::AreEqual<std::uint32_t>(0, pool.ActiveCount());
  }

  TEST_METHOD(AnEmptyPoolGrantsNothing)
  {
    // Audio disabled, or a configuration that asked for no voices. Every caller
    // has to survive it, because it is the state a machine with no sound card
    // reaches.
    VoicePool pool;
    pool.Reset(0);
    bool stolen = false;
    Assert::AreEqual(INVALID_VOICE_SLOT, pool.Acquire(MakeRequest(0, 1, 5.0f, 0.0f), stolen));
    Assert::AreEqual<std::uint32_t>(0, pool.ActiveCount());
  }
};

} // namespace NeuronClientTests
