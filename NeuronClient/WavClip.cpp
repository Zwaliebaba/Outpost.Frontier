#include "pch.h"

#include "WavClip.h"

#include <cstring>

namespace Neuron
{
namespace
{

constexpr std::uint16_t WAVE_FORMAT_PCM_TAG = 1;
constexpr std::uint16_t WAVE_FORMAT_EXTENSIBLE_TAG = 0xfffe;

/// Little-endian reads by memcpy rather than by casting a pointer: the buffer
/// came off disk with no alignment promise, and a `*reinterpret_cast<u32*>` of
/// an odd address is undefined before it is slow.
[[nodiscard]] std::uint16_t ReadU16(const std::uint8_t* _at) noexcept
{
  std::uint16_t value = 0;
  std::memcpy(&value, _at, sizeof(value));
  return value;
}

[[nodiscard]] std::uint32_t ReadU32(const std::uint8_t* _at) noexcept
{
  std::uint32_t value = 0;
  std::memcpy(&value, _at, sizeof(value));
  return value;
}

[[nodiscard]] bool TagIs(const std::uint8_t* _at, const char (&_tag)[5]) noexcept
{
  return std::memcmp(_at, _tag, 4) == 0;
}

} // namespace

bool ParseWav(std::span<const std::uint8_t> _bytes, WavClip& _outClip, std::string& _outError)
{
  // RIFF header: "RIFF" <u32 size> "WAVE" -- twelve bytes before the first
  // chunk, and a file shorter than that is not a truncated WAV, it is not one.
  constexpr std::size_t RIFF_HEADER_BYTES = 12;
  if (_bytes.size() < RIFF_HEADER_BYTES)
  {
    _outError = "not a RIFF file (shorter than a RIFF header)";
    return false;
  }
  if (!TagIs(_bytes.data(), "RIFF") || !TagIs(_bytes.data() + 8, "WAVE"))
  {
    _outError = "not a RIFF/WAVE file";
    return false;
  }

  bool haveFormat = false;
  std::uint16_t formatTag = 0;
  WavClip clip;

  // Chunk walk. Every chunk is an 8-byte header and a body, and bodies are
  // padded to an even length -- the pad byte is not counted by the size field,
  // which is the one detail that turns a working reader into one that drifts
  // by a byte on some files and reads the next tag from the middle of a word.
  std::size_t cursor = RIFF_HEADER_BYTES;
  while (cursor + 8 <= _bytes.size())
  {
    const std::uint8_t* header = _bytes.data() + cursor;
    const std::uint32_t chunkBytes = ReadU32(header + 4);
    const std::size_t body = cursor + 8;

    if (chunkBytes > _bytes.size() - body)
    {
      _outError = "a chunk claims more bytes than the file holds";
      return false;
    }

    if (TagIs(header, "fmt "))
    {
      // 16 bytes is the PCM `fmt `; extensible formats carry more and are read
      // for their first sixteen, which is where the fields this needs live.
      constexpr std::uint32_t PCM_FORMAT_BYTES = 16;
      if (chunkBytes < PCM_FORMAT_BYTES)
      {
        _outError = "the 'fmt ' chunk is too short to be PCM";
        return false;
      }
      const std::uint8_t* format = _bytes.data() + body;
      formatTag = ReadU16(format);
      clip.channels = ReadU16(format + 2);
      clip.sampleRate = ReadU32(format + 4);
      clip.bitsPerSample = ReadU16(format + 14);
      haveFormat = true;
    }
    else if (TagIs(header, "data"))
    {
      const std::uint8_t* start = _bytes.data() + body;
      clip.samples.assign(start, start + chunkBytes);
    }
    // Anything else -- LIST, fact, cue, an editor's private chunk -- is skipped
    // rather than refused. A reader that only accepts the chunks it knows about
    // rejects perfectly good content the day someone opens it in an editor.

    cursor = body + chunkBytes + (chunkBytes & 1u);
  }

  if (!haveFormat)
  {
    _outError = "no 'fmt ' chunk";
    return false;
  }
  if (clip.samples.empty())
  {
    _outError = "no 'data' chunk, or it was empty";
    return false;
  }
  if (formatTag != WAVE_FORMAT_PCM_TAG && formatTag != WAVE_FORMAT_EXTENSIBLE_TAG)
  {
    _outError = "not uncompressed PCM (format tag " + std::to_string(formatTag) + ")";
    return false;
  }
  if (clip.bitsPerSample != 16 && clip.bitsPerSample != 8)
  {
    _outError = std::to_string(clip.bitsPerSample) + "-bit samples; the MVP reads 8 or 16";
    return false;
  }
  if (clip.channels == 0 || clip.sampleRate == 0)
  {
    _outError = "the format chunk declares no channels or no sample rate";
    return false;
  }

  // A partial final frame means the data chunk and the format disagree. Trimmed
  // rather than refused: the samples before it are still the sound, and a mixer
  // handed a fractional frame reads past the end of the last one.
  const std::uint32_t bytesPerFrame = static_cast<std::uint32_t>(clip.channels) * (clip.bitsPerSample / 8u);
  clip.samples.resize((clip.samples.size() / bytesPerFrame) * bytesPerFrame);
  if (clip.samples.empty())
  {
    _outError = "the 'data' chunk held less than one whole sample frame";
    return false;
  }

  _outClip = std::move(clip);
  return true;
}

bool LoadWav(const std::string& _path, WavClip& _outClip, std::string& _outError)
{
  // Win32 rather than <fstream>, as ObjMesh reads its meshes: the rest of the
  // tree opens files this way, and a missing file must be a diagnostic rather
  // than an exception.
  const int wide = MultiByteToWideChar(CP_UTF8, 0, _path.c_str(), -1, nullptr, 0);
  std::wstring widePath(static_cast<std::size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _path.c_str(), -1, widePath.data(), wide);

  const HANDLE file =
      CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    _outError = "could not be opened";
    return false;
  }

  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0 || size.QuadPart > 0x7fffffff)
  {
    CloseHandle(file);
    _outError = "is empty or unreasonably large";
    return false;
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
  CloseHandle(file);

  if (ok == 0 || read != bytes.size())
  {
    _outError = "could not be read to the end";
    return false;
  }
  return ParseWav(bytes, _outClip, _outError);
}

} // namespace Neuron
