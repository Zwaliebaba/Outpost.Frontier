#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/*
 * RIFF WAV, 16-bit PCM (ADR-011 §10).
 *
 * A few dozen lines rather than a decoder dependency, because the format is
 * four-byte tags and sizes and the MVP ships nothing compressed. Mono for
 * anything spatialised -- X3DAudio positions *channels*, not sources, so a
 * stereo "3D" sound smears across the field rather than sitting in it, and
 * ADR-011 makes mono a content rule instead of a runtime surprise.
 *
 * The parse is a pure function of bytes, which is the whole point: a WAV that
 * will not load is a content bug, and `NeuronClientTests` can prove the reader
 * rejects the malformed cases on a runner with no audio device at all.
 */

namespace Neuron
{

/// Everything XAudio2 needs to submit this clip, plus the samples themselves.
/// The bytes are kept raw rather than converted: a `WAVEFORMATEX` and a buffer
/// pointer is exactly what `SubmitSourceBuffer` wants, and a conversion here
/// would be one this build has no reason to do.
struct WavClip
{
  std::uint32_t sampleRate = 0;
  std::uint16_t channels = 0;
  std::uint16_t bitsPerSample = 0;
  std::vector<std::uint8_t> samples;

  [[nodiscard]] bool Valid() const noexcept
  {
    return sampleRate != 0 && channels != 0 && bitsPerSample != 0 && !samples.empty();
  }

  /// Sample frames, not bytes and not samples: one frame is one sample per
  /// channel, which is the unit loop points and durations are counted in.
  [[nodiscard]] std::uint32_t FrameCount() const noexcept
  {
    const std::uint32_t bytesPerFrame = static_cast<std::uint32_t>(channels) * (bitsPerSample / 8u);
    return bytesPerFrame == 0 ? 0 : static_cast<std::uint32_t>(samples.size()) / bytesPerFrame;
  }

  [[nodiscard]] double DurationSeconds() const noexcept
  {
    return sampleRate == 0 ? 0.0 : static_cast<double>(FrameCount()) / static_cast<double>(sampleRate);
  }
};

/*
 * Parses a RIFF WAV from bytes.
 *
 * Accepts what the MVP authors: `WAVE_FORMAT_PCM` (1) at 8 or 16 bits, any
 * channel count, with unknown chunks -- `LIST`, `fact`, whatever an editor
 * wrote -- skipped rather than refused. Rejects anything it would otherwise
 * have to guess about: a truncated header, a missing `fmt ` or `data`, a
 * compressed format tag, or a chunk size that runs off the end of the buffer.
 *
 * `_outError` is filled on failure and left alone on success.
 */
[[nodiscard]] bool ParseWav(std::span<const std::uint8_t> _bytes, WavClip& _outClip, std::string& _outError);

/// Reads and parses a file. The path is UTF-8, as every path in this tree is.
[[nodiscard]] bool LoadWav(const std::string& _path, WavClip& _outClip, std::string& _outError);

} // namespace Neuron
