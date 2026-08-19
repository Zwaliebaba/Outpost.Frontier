#include "pch.h"

#include "AudioDevice.h"

#include "ClientConfig.h"
#include "Log.h"
#include "SoundBank.h"
#include "VoicePool.h"
#include "WavClip.h"

#include <xaudio2.h>
#include <x3daudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "xaudio2.lib")

namespace Neuron
{
namespace
{

/*
 * What a category is worth when the pool is full (ADR-011 §3).
 *
 * Higher wins, and the ordering is the only thing that matters: an alert must
 * out-rank a hum, or a fleet's worth of engines can drown the one cue that was
 * trying to say something. Indexed by `SoundCategory`.
 */
constexpr std::array<float, SOUND_CATEGORY_COUNT> CATEGORY_PRIORITY = {
    1.0f, // World3D  -- the engines; the thing there is most of
    0.5f, // Ambience -- background, first to go
    3.0f, // Ui       -- the player asked for this
    2.0f, // Music
    4.0f, // Alerts   -- outranks everything
};

/// Whether a category is spatialised. Only `World3D` is (ADR-011 §2), which is
/// also what decides which of the two pools a sound comes from.
[[nodiscard]] constexpr bool IsSpatial(SoundCategory _category) noexcept
{
  return _category == SoundCategory::World3D;
}

/// Win32 rather than <fstream>, as ObjMesh and WavClip read theirs: a missing
/// bank has to be a diagnostic, not an exception thrown past a caller whose
/// whole contract is that audio failing is survivable.
[[nodiscard]] bool ReadWholeTextFile(const std::string& _path, std::string& _outText)
{
  const int wide = MultiByteToWideChar(CP_UTF8, 0, _path.c_str(), -1, nullptr, 0);
  std::wstring widePath(static_cast<std::size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _path.c_str(), -1, widePath.data(), wide);

  const HANDLE file =
      CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    return false;
  }

  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == 0 || size.QuadPart < 0 || size.QuadPart > 0x7fffffff)
  {
    CloseHandle(file);
    return false;
  }

  _outText.resize(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok =
      _outText.empty() ? TRUE : ReadFile(file, _outText.data(), static_cast<DWORD>(_outText.size()), &read, nullptr);
  CloseHandle(file);
  return ok != 0 && read == _outText.size();
}

[[nodiscard]] std::string JoinAudioPath(const std::string& _directory, const std::string& _file)
{
  std::string path(_directory);
  if (!path.empty() && path.back() != '/' && path.back() != '\\')
  {
    path += '/';
  }
  path.append(_file);
  return path;
}

/// A cheap, self-contained generator for pitch variance. Not the game's seeded
/// stream on purpose: audio is cosmetic and outside replay determinism
/// (ADR-005 §5 scopes that to GameLogic), and borrowing the sim's generator
/// would make how a cue sounds part of what a replay has to reproduce.
class PitchJitter
{
public:
  [[nodiscard]] float Next() noexcept
  {
    // xorshift32: a handful of instructions, no state to own, and nothing here
    // is cryptography or statistics.
    m_state ^= m_state << 13;
    m_state ^= m_state >> 17;
    m_state ^= m_state << 5;
    return static_cast<float>(m_state & 0xffffffu) / static_cast<float>(0xffffffu) * 2.0f - 1.0f;
  }

private:
  std::uint32_t m_state = 0x9e3779b9u;
};

} // namespace

/*
 * One pooled source voice.
 *
 * The XAudio2 handle plus what it is currently being: `IXAudio2SourceVoice` is
 * bound to a format and a destination at creation, so a slot whose sound
 * changes format or category has its voice destroyed and rebuilt. With the
 * MVP's two cues that never happens -- the code is here because the day a third
 * cue arrives at a different sample rate, the alternative is silence nobody can
 * explain.
 */
struct PooledVoice
{
  IXAudio2SourceVoice* voice = nullptr;
  std::uint32_t soundIndex = 0xffffffffu;
  DirectX::XMFLOAT3 position{};
  bool looping = false;

  /// Declared again this frame. A loop that stops being declared is a ship that
  /// left, and the next `Update` retires it.
  bool touched = false;

  // What the live voice was built for, so reuse can be checked rather than
  // assumed.
  std::uint32_t builtSampleRate = 0;
  std::uint16_t builtChannels = 0;
  std::uint16_t builtBits = 0;
  SoundCategory builtCategory = SoundCategory::Count;
};

/// A pool and the voices behind it. Two of these exist -- spatial and flat --
/// because ADR-011 §3 caps them separately: the 3D pool is the one that meets a
/// fleet, and giving the UI its own means a battle can never make the interface
/// go quiet.
struct VoiceBank
{
  VoicePool pool;
  std::vector<PooledVoice> voices;
};

struct AudioDevice::Impl
{
  IXAudio2* xaudio = nullptr;
  IXAudio2MasteringVoice* master = nullptr;
  std::array<IXAudio2SubmixVoice*, SOUND_CATEGORY_COUNT> submixes{};

  X3DAUDIO_HANDLE x3d{};
  std::uint32_t outputChannels = 2;

  SoundBank bank;
  /// Parallel to `bank.Sounds()`: the decoded clip and the format XAudio2 needs
  /// for it. A cue whose file would not load keeps an invalid clip and is never
  /// played, which is why `Play` checks rather than trusting the bank.
  std::vector<WavClip> clips;

  VoiceBank spatial;
  VoiceBank flat;

  AudioListenerState listener;
  std::vector<std::uint32_t> pendingCues;
  std::vector<double> lastStartMs;
  std::array<float, SOUND_CATEGORY_COUNT> categoryGain{};
  std::vector<float> matrixScratch;
  PitchJitter jitter;
  bool ready = false;

  [[nodiscard]] std::uint32_t FindSound(std::string_view _id) const noexcept
  {
    const std::span<const SoundDef> sounds = bank.Sounds();
    for (std::uint32_t index = 0; index < sounds.size(); ++index)
    {
      if (sounds[index].id == _id)
      {
        return index;
      }
    }
    return 0xffffffffu;
  }

  [[nodiscard]] VoiceBank& BankFor(SoundCategory _category) noexcept
  {
    return IsSpatial(_category) ? spatial : flat;
  }

  /// Tears a slot's voice down. Stopping before destroying is not politeness:
  /// `DestroyVoice` blocks until the audio thread is done with it, and a voice
  /// still playing is one the mixer is actively reading.
  static void DestroyVoice(PooledVoice& _slot) noexcept
  {
    if (_slot.voice != nullptr)
    {
      _slot.voice->Stop(0);
      _slot.voice->FlushSourceBuffers();
      _slot.voice->DestroyVoice();
      _slot.voice = nullptr;
    }
    _slot.builtSampleRate = 0;
    _slot.builtChannels = 0;
    _slot.builtBits = 0;
    _slot.builtCategory = SoundCategory::Count;
  }

  void StopSlot(VoiceBank& _bank, std::uint32_t _slot) noexcept
  {
    PooledVoice& pooled = _bank.voices[_slot];
    if (pooled.voice != nullptr)
    {
      pooled.voice->Stop(0);
      pooled.voice->FlushSourceBuffers();
    }
    pooled.touched = false;
    pooled.looping = false;
    _bank.pool.Release(_slot);
  }

  /// Builds or reuses the slot's voice, then submits and starts the clip.
  [[nodiscard]] bool StartSlot(VoiceBank& _bank, std::uint32_t _slot, std::uint32_t _soundIndex, bool _looping);

  void ApplySpatial(PooledVoice& _pooled, const SoundDef& _def) noexcept;
};

bool AudioDevice::Impl::StartSlot(VoiceBank& _bank, std::uint32_t _slot, std::uint32_t _soundIndex, bool _looping)
{
  const SoundDef& def = bank.Sounds()[_soundIndex];
  const WavClip& clip = clips[_soundIndex];
  PooledVoice& pooled = _bank.voices[_slot];

  const bool matches = pooled.voice != nullptr && pooled.builtSampleRate == clip.sampleRate &&
                       pooled.builtChannels == clip.channels && pooled.builtBits == clip.bitsPerSample &&
                       pooled.builtCategory == def.category;
  if (!matches)
  {
    DestroyVoice(pooled);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = clip.channels;
    format.nSamplesPerSec = clip.sampleRate;
    format.wBitsPerSample = clip.bitsPerSample;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    XAUDIO2_SEND_DESCRIPTOR send{0, submixes[static_cast<std::uint32_t>(def.category)]};
    XAUDIO2_VOICE_SENDS sends{1, &send};
    const HRESULT created =
        xaudio->CreateSourceVoice(&pooled.voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, &sends, nullptr);
    if (FAILED(created))
    {
      NEURON_LOG_WARNING("audio: no source voice for '%s' (hr 0x%08lx)", def.id.c_str(),
                         static_cast<unsigned long>(created));
      pooled.voice = nullptr;
      return false;
    }
    pooled.builtSampleRate = clip.sampleRate;
    pooled.builtChannels = clip.channels;
    pooled.builtBits = clip.bitsPerSample;
    pooled.builtCategory = def.category;
  }
  else
  {
    // Reused: whatever it was playing has to go before the new buffer, or the
    // two queue up and the cue arrives late by the length of the last one.
    pooled.voice->Stop(0);
    pooled.voice->FlushSourceBuffers();
  }

  XAUDIO2_BUFFER buffer{};
  buffer.AudioBytes = static_cast<UINT32>(clip.samples.size());
  buffer.pAudioData = clip.samples.data();
  buffer.Flags = XAUDIO2_END_OF_STREAM;
  buffer.LoopCount = _looping ? XAUDIO2_LOOP_INFINITE : 0;
  if (FAILED(pooled.voice->SubmitSourceBuffer(&buffer)))
  {
    return false;
  }

  pooled.voice->SetVolume(def.gain);
  if (def.pitchVariance > 0.0f)
  {
    pooled.voice->SetFrequencyRatio(1.0f + def.pitchVariance * jitter.Next());
  }
  else
  {
    pooled.voice->SetFrequencyRatio(1.0f);
  }

  pooled.soundIndex = _soundIndex;
  pooled.looping = _looping;
  pooled.touched = true;
  return SUCCEEDED(pooled.voice->Start(0));
}

void AudioDevice::Impl::ApplySpatial(PooledVoice& _pooled, const SoundDef& _def) noexcept
{
  X3DAUDIO_LISTENER x3dListener{};
  x3dListener.OrientFront = {listener.front.x, listener.front.y, listener.front.z};
  x3dListener.OrientTop = {listener.top.x, listener.top.y, listener.top.z};
  x3dListener.Position = {listener.position.x, listener.position.y, listener.position.z};
  x3dListener.Velocity = {0.0f, 0.0f, 0.0f};

  /*
   * The falloff the bank authored, as an explicit curve.
   *
   * X3DAudio's default curve is an inverse-square normalised by
   * `CurveDistanceScaler`, which honours `maxDistance` and has nowhere to put
   * `minDistance` -- so a bank could name a full-volume radius and be quietly
   * ignored. Three points say it exactly: flat inside `minDistance`, then a
   * straight fall to silence at `maxDistance`. Normalised, because curve
   * positions are fractions of the scaler.
   */
  const float minFraction = std::clamp(_def.minDistanceMetres / _def.maxDistanceMetres, 0.0f, 0.99f);
  const X3DAUDIO_DISTANCE_CURVE_POINT points[3] = {{0.0f, 1.0f}, {minFraction, 1.0f}, {1.0f, 0.0f}};
  X3DAUDIO_DISTANCE_CURVE volumeCurve{};
  volumeCurve.pPoints = const_cast<X3DAUDIO_DISTANCE_CURVE_POINT*>(points);
  volumeCurve.PointCount = 3;

  X3DAUDIO_EMITTER emitter{};
  emitter.ChannelCount = 1; // Mono, and it is a content rule (ADR-011 §5).
  emitter.OrientFront = {0.0f, 0.0f, 1.0f};
  emitter.OrientTop = {0.0f, 1.0f, 0.0f};
  emitter.Position = {_pooled.position.x, _pooled.position.y, _pooled.position.z};
  emitter.Velocity = {0.0f, 0.0f, 0.0f};
  // Metres, so falloff is authored in real distances rather than in mixer units.
  emitter.CurveDistanceScaler = _def.maxDistanceMetres;
  emitter.pVolumeCurve = &volumeCurve;
  // Doppler off in the MVP: ships move at hundreds of m/s past a detached
  // camera, and pitch-shifting all of it reads as a bug rather than as physics.
  emitter.DopplerScaler = 0.0f;

  X3DAUDIO_DSP_SETTINGS dsp{};
  dsp.SrcChannelCount = 1;
  dsp.DstChannelCount = outputChannels;
  dsp.pMatrixCoefficients = matrixScratch.data();

  // Only the flags whose results are consumed (ADR-011 §7): the pan matrix and
  // the direct low-pass. Asking for reverb or Doppler here would compute them
  // every frame for every voice and throw them away.
  X3DAudioCalculate(x3d, &x3dListener, &emitter, X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_LPF_DIRECT, &dsp);

  _pooled.voice->SetOutputMatrix(submixes[static_cast<std::uint32_t>(_def.category)], 1, outputChannels,
                                 matrixScratch.data());

  XAUDIO2_FILTER_PARAMETERS filter{};
  filter.Type = LowPassFilter;
  // The cutoff X3DAudio's coefficient means, in the form XAudio2 wants it.
  filter.Frequency = 2.0f * std::sin(X3DAUDIO_PI / 6.0f * dsp.LPFDirectCoefficient);
  filter.OneOverQ = 1.0f;
  _pooled.voice->SetFilterParameters(&filter);
}

AudioDevice::AudioDevice()
  : m_impl(std::make_unique<Impl>())
{
}

AudioDevice::~AudioDevice()
{
  Destroy();
}

bool AudioDevice::Create(const AudioSettings& _settings, const std::string& _audioDirectory,
                         const std::string& _soundBankPath)
{
  Impl& impl = *m_impl;

  if (!_settings.enabled)
  {
    NEURON_LOG_INFO("audio: disabled by configuration");
    return false;
  }

  // Every failure below is the same failure: the client runs silently. It logs
  // once, at the point that knows why, and never again.
  if (FAILED(XAudio2Create(&impl.xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
  {
    NEURON_LOG_WARNING("audio: XAudio2 would not start; running silently");
    return false;
  }

  if (FAILED(impl.xaudio->CreateMasteringVoice(&impl.master)))
  {
    NEURON_LOG_WARNING("audio: no output device; running silently");
    Destroy();
    return false;
  }

  XAUDIO2_VOICE_DETAILS details{};
  impl.master->GetVoiceDetails(&details);
  DWORD channelMask = 0;
  if (FAILED(impl.master->GetChannelMask(&channelMask)) || channelMask == 0)
  {
    channelMask = SPEAKER_STEREO;
  }
  impl.outputChannels = details.InputChannels;
  impl.matrixScratch.assign(impl.outputChannels, 0.0f);

  if (FAILED(X3DAudioInitialize(channelMask, X3DAUDIO_SPEED_OF_SOUND, impl.x3d)))
  {
    NEURON_LOG_WARNING("audio: X3DAudio would not initialise; running silently");
    Destroy();
    return false;
  }

  // The five submixes of ADR-011 §2, in the enum's order, each at the master's
  // own rate and width so nothing resamples between them.
  const std::array<float, SOUND_CATEGORY_COUNT> gains = {_settings.world, _settings.ambience, _settings.ui,
                                                         _settings.music, _settings.alerts};
  for (std::uint32_t index = 0; index < SOUND_CATEGORY_COUNT; ++index)
  {
    if (FAILED(impl.xaudio->CreateSubmixVoice(&impl.submixes[index], details.InputChannels, details.InputSampleRate)))
    {
      NEURON_LOG_WARNING("audio: submix '%.*s' would not be created; running silently",
                         static_cast<int>(SoundCategoryName(static_cast<SoundCategory>(index)).size()),
                         SoundCategoryName(static_cast<SoundCategory>(index)).data());
      Destroy();
      return false;
    }
    impl.categoryGain[index] = std::max(0.0f, gains[index]);
    impl.submixes[index]->SetVolume(impl.categoryGain[index]);
  }
  impl.master->SetVolume(std::max(0.0f, _settings.master));

  // The bank, then the clips it names. A bank that will not parse is fatal to
  // audio and to nothing else; a single cue that will not load costs that cue.
  std::string bankText;
  if (!ReadWholeTextFile(_soundBankPath, bankText))
  {
    NEURON_LOG_WARNING("audio: '%s' could not be read; running silently", _soundBankPath.c_str());
    Destroy();
    return false;
  }

  std::vector<std::string> bankErrors;
  if (!SoundBank::Parse(bankText, impl.bank, bankErrors))
  {
    for (const std::string& error : bankErrors)
    {
      NEURON_LOG_ERROR("audio: %s", error.c_str());
    }
    NEURON_LOG_WARNING("audio: the sound bank would not parse; running silently");
    Destroy();
    return false;
  }
  for (const std::string& error : bankErrors)
  {
    NEURON_LOG_WARNING("audio: %s", error.c_str());
  }

  const std::span<const SoundDef> sounds = impl.bank.Sounds();
  impl.clips.resize(sounds.size());
  impl.lastStartMs.assign(sounds.size(), -1.0e9);
  std::uint32_t loaded = 0;
  for (std::uint32_t index = 0; index < sounds.size(); ++index)
  {
    const std::string path = JoinAudioPath(_audioDirectory, sounds[index].file);
    std::string error;
    if (!LoadWav(path, impl.clips[index], error))
    {
      NEURON_LOG_ERROR("audio: '%s' %s; the cue '%s' will not sound", path.c_str(), error.c_str(),
                       sounds[index].id.c_str());
      continue;
    }
    if (IsSpatial(sounds[index].category) && impl.clips[index].channels != 1)
    {
      // Mono is a content rule, not a preference: X3DAudio positions channels
      // rather than sources, so a stereo "3D" cue smears instead of sitting
      // somewhere. Refused here, where the file name is still in hand.
      NEURON_LOG_ERROR("audio: '%s' is %u-channel but '%s' is spatialised; mono is required (ADR-011 §5)", path.c_str(),
                       static_cast<unsigned>(impl.clips[index].channels), sounds[index].id.c_str());
      impl.clips[index] = WavClip{};
      continue;
    }
    ++loaded;
  }

  impl.spatial.pool.Reset(_settings.voiceCap3D);
  impl.spatial.voices.assign(_settings.voiceCap3D, PooledVoice{});
  impl.flat.pool.Reset(_settings.voiceCap2D);
  impl.flat.voices.assign(_settings.voiceCap2D, PooledVoice{});

  impl.ready = true;
  NEURON_LOG_INFO("audio: %u of %u cue(s) loaded, %u output channel(s) at %u Hz, %u 3D + %u 2D voices", loaded,
                  static_cast<unsigned>(sounds.size()), impl.outputChannels, details.InputSampleRate,
                  _settings.voiceCap3D, _settings.voiceCap2D);
  return true;
}

void AudioDevice::Destroy()
{
  Impl& impl = *m_impl;
  impl.ready = false;

  // Voices before submixes before the master before the engine: XAudio2 will
  // not let a destination go while a source still sends to it.
  for (VoiceBank* bank : {&impl.spatial, &impl.flat})
  {
    for (PooledVoice& pooled : bank->voices)
    {
      Impl::DestroyVoice(pooled);
    }
    bank->voices.clear();
    bank->pool.Reset(0);
  }

  for (IXAudio2SubmixVoice*& submix : impl.submixes)
  {
    if (submix != nullptr)
    {
      submix->DestroyVoice();
      submix = nullptr;
    }
  }
  if (impl.master != nullptr)
  {
    impl.master->DestroyVoice();
    impl.master = nullptr;
  }
  if (impl.xaudio != nullptr)
  {
    impl.xaudio->Release();
    impl.xaudio = nullptr;
  }
}

bool AudioDevice::Ready() const noexcept
{
  return m_impl->ready;
}

void AudioDevice::SetListener(const AudioListenerState& _listener) noexcept
{
  m_impl->listener = _listener;
}

void AudioDevice::PlayCue(std::string_view _soundId)
{
  Impl& impl = *m_impl;
  if (!impl.ready)
  {
    return;
  }
  const std::uint32_t index = impl.FindSound(_soundId);
  if (index == 0xffffffffu || !impl.clips[index].Valid())
  {
    return;
  }
  impl.pendingCues.push_back(index);
}

void AudioDevice::UpdateLoop(std::string_view _soundId, std::uint64_t _emitterId, const DirectX::XMFLOAT3& _positionMetres)
{
  Impl& impl = *m_impl;
  if (!impl.ready || _emitterId == 0)
  {
    return;
  }

  const std::uint32_t soundIndex = impl.FindSound(_soundId);
  if (soundIndex == 0xffffffffu || !impl.clips[soundIndex].Valid())
  {
    return;
  }

  const SoundDef& def = impl.bank.Sounds()[soundIndex];
  VoiceBank& bank = impl.BankFor(def.category);
  const float distance = AudioDistanceMetres(impl.listener, _positionMetres);
  const float priority = CATEGORY_PRIORITY[static_cast<std::uint32_t>(def.category)];

  // Past its falloff there is nothing to hear, so there is nothing to spend a
  // voice on. This is what keeps a 200-ship scene from asking the pool 200
  // times a frame for voices that would all be silent.
  if (distance > def.maxDistanceMetres)
  {
    return;
  }

  const std::uint32_t existing = bank.pool.Find(_emitterId, soundIndex);
  if (existing != INVALID_VOICE_SLOT)
  {
    bank.pool.Touch(existing, priority, distance);
    bank.voices[existing].position = _positionMetres;
    bank.voices[existing].touched = true;
    return;
  }

  VoiceRequest request;
  request.soundIndex = soundIndex;
  request.emitterId = _emitterId;
  request.priority = priority;
  request.distanceMetres = distance;
  request.maxInstances = def.maxInstances;

  bool stolen = false;
  const std::uint32_t slot = bank.pool.Acquire(request, stolen);
  if (slot == INVALID_VOICE_SLOT)
  {
    return; // The pool is full of things that matter more. Nothing to do.
  }

  bank.voices[slot].position = _positionMetres;
  if (!impl.StartSlot(bank, slot, soundIndex, true))
  {
    bank.pool.Release(slot);
  }
}

void AudioDevice::Update(double _nowSeconds)
{
  Impl& impl = *m_impl;
  if (!impl.ready)
  {
    impl.pendingCues.clear();
    return;
  }

  const double nowMs = _nowSeconds * 1000.0;

  // The one-shots raised since the last stage. Started here rather than where
  // they were raised, so every XAudio2 call in a frame is inside the one
  // `AudioUpdate` stage the strip measures (ADR-011 §9).
  for (const std::uint32_t soundIndex : impl.pendingCues)
  {
    const SoundDef& def = impl.bank.Sounds()[soundIndex];
    if (def.cooldownMs > 0.0 && nowMs - impl.lastStartMs[soundIndex] < def.cooldownMs)
    {
      continue; // A click that retriggers every frame is a buzz.
    }

    VoiceBank& bank = impl.BankFor(def.category);
    VoiceRequest request;
    request.soundIndex = soundIndex;
    request.emitterId = 0; // A one-shot belongs to nothing and is never reused.
    request.priority = CATEGORY_PRIORITY[static_cast<std::uint32_t>(def.category)];
    request.distanceMetres = 0.0f;
    request.maxInstances = def.maxInstances;

    bool stolen = false;
    const std::uint32_t slot = bank.pool.Acquire(request, stolen);
    if (slot == INVALID_VOICE_SLOT)
    {
      continue;
    }
    if (impl.StartSlot(bank, slot, soundIndex, false))
    {
      impl.lastStartMs[soundIndex] = nowMs;
    }
    else
    {
      bank.pool.Release(slot);
    }
  }
  impl.pendingCues.clear();

  // Retire, then update what is left. Loops that stopped being declared are
  // ships that despawned or went out of range; one-shots are retired when the
  // voice has run dry.
  //
  // Polled rather than driven by `IXAudio2VoiceCallback`. ADR-011 §8 reserves a
  // callback plus an SPSC ring for buffer events, and that is the right shape
  // the moment anything *needs* the event -- streaming music, or a cue that
  // must trigger another. Nothing here does: "has this finished" is one cheap
  // read on the thread that would drain the ring anyway, and the ADR's actual
  // constraint -- no game or render state touched from an audio callback -- is
  // met trivially by having no callback at all.
  for (VoiceBank* bankPtr : {&impl.spatial, &impl.flat})
  {
    VoiceBank& bank = *bankPtr;
    for (std::uint32_t slot = 0; slot < bank.voices.size(); ++slot)
    {
      if (!bank.pool.Slots()[slot].active)
      {
        continue;
      }
      PooledVoice& pooled = bank.voices[slot];

      if (pooled.looping)
      {
        if (!pooled.touched)
        {
          impl.StopSlot(bank, slot);
          continue;
        }
        pooled.touched = false;
      }
      else
      {
        XAUDIO2_VOICE_STATE state{};
        pooled.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0)
        {
          impl.StopSlot(bank, slot);
          continue;
        }
      }

      const SoundDef& def = impl.bank.Sounds()[pooled.soundIndex];
      if (IsSpatial(def.category))
      {
        impl.ApplySpatial(pooled, def);
      }
    }
  }
}

std::uint32_t AudioDevice::ActiveVoices() const noexcept
{
  return m_impl->spatial.pool.ActiveCount() + m_impl->flat.pool.ActiveCount();
}

std::uint32_t AudioDevice::VoiceCapacity() const noexcept
{
  return m_impl->spatial.pool.Capacity() + m_impl->flat.pool.Capacity();
}

} // namespace Neuron
