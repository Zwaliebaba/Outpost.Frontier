#pragma once

#include "AudioListener.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

/*
 * The XAudio2 graph (ADR-011 §1-2, §7).
 *
 * One `IXAudio2`, one mastering voice, five submixes -- `World3D`, `Ambience`,
 * `Ui`, `Music`, `Alerts` -- and a pool of source voices feeding them. Only
 * `World3D` is spatialised; the rest are 2D. Category gains come from
 * configuration and are what a settings screen will write.
 *
 * Everything XAudio2 lives behind the pimpl, exactly as `QuicTransport` keeps
 * msquic behind one: `xaudio2.h` and `x3daudio.h` are heavy Windows headers,
 * and a client that included them everywhere would be a client where every file
 * could reach for a voice.
 *
 * **No audio device is not fatal** (ADR-011, Consequences). `Create` returning
 * false is a client that logs, disables audio and runs silently -- the caller
 * checks nothing and calls the rest of this interface regardless, because every
 * method is a no-op on a device that is not ready. A game that will not start
 * because a machine has no speakers is a worse bug than a game with no sound.
 */

namespace Neuron
{

struct AudioSettings;

/*
 * The two cues the client raises itself (ADR-011 §12).
 *
 * The engine names the *event*; the bank decides what it sounds like, which is
 * the line ADR-014 draws in every other direction too. A bank with no entry for
 * one of these is silent for it and says so once at load -- an id is a request,
 * not an assertion that a file exists.
 */
inline constexpr std::string_view ENGINE_LOOP_SOUND_ID = "ship.engine";
inline constexpr std::string_view ORDER_REJECTED_SOUND_ID = "order.rejected";

class AudioDevice
{
public:
  AudioDevice();
  ~AudioDevice();

  AudioDevice(const AudioDevice&) = delete;
  AudioDevice& operator=(const AudioDevice&) = delete;

  /*
   * Opens the device, builds the graph and loads the bank.
   *
   * `_soundBankPath` and `_audioDirectory` are resolved by the composition
   * root, as every other content path is -- the client is told where its assets
   * are and never goes looking (ADR-013 §6).
   *
   * False means audio is off for this run and says why in the log. It is not an
   * error the caller has to handle.
   */
  [[nodiscard]] bool Create(const AudioSettings& _settings, const std::string& _audioDirectory,
                            const std::string& _soundBankPath);

  void Destroy();

  [[nodiscard]] bool Ready() const noexcept;

  /// Where the ear is this frame. Cheap, and stored rather than applied: the
  /// listener is read once per voice inside `Update`.
  void SetListener(const AudioListenerState& _listener) noexcept;

  /*
   * A 2D cue, by bank id. Queued rather than started: every XAudio2 call
   * belongs to the one `AudioUpdate` stage (ADR-011 §9), so an event raised
   * from the order path costs a push here and is heard when the stage runs.
   */
  void PlayCue(std::string_view _soundId);

  /*
   * Declares a looping 3D emitter for this frame.
   *
   * Called every frame for every ship that should be humming. A loop already
   * playing is refreshed in place; a new one is started if the pool will have
   * it; one that stops being declared is retired by the next `Update`. So the
   * caller never tracks voices -- it describes what should be audible, and the
   * pool decides what fits (ADR-011 §3).
   */
  void UpdateLoop(std::string_view _soundId, std::uint64_t _emitterId, const DirectX::XMFLOAT3& _positionMetres);

  /*
   * The `AudioUpdate` stage: retire what finished or went undeclared, start
   * what was queued, and recalculate 3D parameters against the listener.
   *
   * One stage, on Main, immediately after Extract -- and the fifth budget row
   * on the diagnostics strip, which is why it is timed by the caller's span
   * rather than measuring itself.
   */
  void Update(double _nowSeconds);

  /// Live voices, for the strip. The number that says whether the pool's cap is
  /// doing its job under a fleet.
  [[nodiscard]] std::uint32_t ActiveVoices() const noexcept;

  /// Voices the pool will ever hand out. Zero when audio is off.
  [[nodiscard]] std::uint32_t VoiceCapacity() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace Neuron
