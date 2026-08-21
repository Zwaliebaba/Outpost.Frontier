#include "pch.h"

#include "DurableStore.h"

#include "ByteReader.h"
#include "ByteWriter.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace Neuron
{

namespace
{

/// Recognises a journal, and refuses somebody else's. "OPFJ" / "OPFS".
constexpr std::uint32_t JOURNAL_MAGIC = 0x4a46504fu;
constexpr std::uint32_t SNAPSHOT_MAGIC = 0x5346504fu;

/// The frame's own marker (ADR-025 §3), and what a forward scan looks for when
/// telling a torn tail from corruption.
constexpr std::uint32_t FRAME_MAGIC = 0x52465000u;

constexpr std::size_t JOURNAL_HEADER_BYTES = 4 + 4 + 8 + 8 + 2 + 4;
constexpr std::size_t FRAME_PREFIX_BYTES = 4 + 4 + 2 + 4; // magic, length, kind, tick.
constexpr std::size_t FRAME_TRAILER_BYTES = 4;            // crc32.

/*
 * CRC-32 (IEEE), which is what makes a torn tail *detectable* rather than
 * plausible (ADR-025 §6.5).
 *
 * Here rather than in NeuronCore beside FNV-1a because nothing else needs one:
 * FNV is for hashes that must be stable across builds and reimplementable by
 * hand, and this is for noticing that a write did not finish. The day a second
 * caller wants it, it moves.
 */
[[nodiscard]] const std::array<std::uint32_t, 256>& Crc32Table() noexcept
{
  static const std::array<std::uint32_t, 256> CRC_TABLE = []
  {
    std::array<std::uint32_t, 256> built{};
    for (std::uint32_t index = 0; index < 256; ++index)
    {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit)
      {
        value = (value & 1u) != 0u ? 0xedb88320u ^ (value >> 1) : value >> 1;
      }
      built[index] = value;
    }
    return built;
  }();
  return CRC_TABLE;
}

[[nodiscard]] std::uint32_t Crc32(std::span<const std::uint8_t> _bytes, std::uint32_t _seed = 0) noexcept
{
  const std::array<std::uint32_t, 256>& table = Crc32Table();
  std::uint32_t crc = ~_seed;
  for (const std::uint8_t byte : _bytes)
  {
    crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8);
  }
  return ~crc;
}

[[nodiscard]] std::FILE* OpenFile(const std::string& _path, const char* _mode) noexcept
{
#if defined(_MSC_VER)
  std::FILE* file = nullptr;
  return fopen_s(&file, _path.c_str(), _mode) == 0 ? file : nullptr;
#else
  return std::fopen(_path.c_str(), _mode);
#endif
}

/*
 * Past the C runtime's buffer and onto the disk.
 *
 * `fflush` alone hands the bytes to the OS, which is not the same as having
 * them survive a power cut -- and the difference is precisely the promise
 * ADR-025 §4 makes about a clean shutdown.
 */
void CommitToDisk(std::FILE* _file) noexcept
{
  if (_file == nullptr)
  {
    return;
  }
  std::fflush(_file);
#if defined(_WIN32)
  (void)_commit(_fileno(_file));
#else
  (void)::fsync(::fileno(_file));
#endif
}

[[nodiscard]] std::string Join(const std::string& _directory, const std::string& _leaf)
{
  if (_directory.empty())
  {
    return _leaf;
  }
  std::filesystem::path path(_directory);
  path /= _leaf;
  return path.string();
}

[[nodiscard]] std::string Hex(std::uint64_t _value)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string text(16, '0');
  for (int index = 15; index >= 0; --index)
  {
    text[static_cast<std::size_t>(index)] = DIGITS[_value & 0xfu];
    _value >>= 4;
  }
  return text;
}

/// Reads a whole file, or reports that it is not there. Errors other than
/// absence are reported as absence-with-a-message by the caller, because at
/// boot the only two useful answers are "there is state" and "there is not".
[[nodiscard]] bool ReadWholeFile(const std::string& _path, std::vector<std::uint8_t>& _outBytes)
{
  std::FILE* file = OpenFile(_path, "rb");
  if (file == nullptr)
  {
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size < 0)
  {
    std::fclose(file);
    return false;
  }
  _outBytes.resize(static_cast<std::size_t>(size));
  const std::size_t read = _outBytes.empty() ? 0 : std::fread(_outBytes.data(), 1, _outBytes.size(), file);
  std::fclose(file);
  _outBytes.resize(read);
  return true;
}

/*
 * One frame, parsed and checked.
 *
 * `Ok` is the whole vocabulary this needs: a frame is either wholly there with
 * a checksum that agrees, or it is the end of the good part of the file.
 */
struct ParsedFrame
{
  bool ok = false;
  std::uint16_t recordKind = 0;
  std::uint32_t shardTick = 0;
  std::size_t payloadAt = 0;
  std::uint32_t payloadBytes = 0;
  std::size_t totalBytes = 0;
};

[[nodiscard]] ParsedFrame ParseFrame(std::span<const std::uint8_t> _bytes, std::size_t _at) noexcept
{
  ParsedFrame frame;
  if (_at + FRAME_PREFIX_BYTES + FRAME_TRAILER_BYTES > _bytes.size())
  {
    return frame;
  }
  ByteReader reader(_bytes.subspan(_at));
  if (reader.ReadUInt32() != FRAME_MAGIC)
  {
    return frame;
  }
  const std::uint32_t payloadBytes = reader.ReadUInt32();
  const std::uint16_t recordKind = reader.ReadUInt16();
  const std::uint32_t shardTick = reader.ReadUInt32();
  if (payloadBytes > MAX_DURABLE_RECORD_BYTES)
  {
    return frame; // A length is bounded before a byte of it is trusted.
  }
  const std::size_t total = FRAME_PREFIX_BYTES + payloadBytes + FRAME_TRAILER_BYTES;
  if (_at + total > _bytes.size())
  {
    return frame;
  }

  // Over kind, tick and payload -- the frame's contents, not its own marker or
  // its length, both of which have already had to be right to get here.
  const std::size_t checkedAt = _at + 4 + 4;
  const std::size_t checkedBytes = 2 + 4 + payloadBytes;
  const std::uint32_t expected = Crc32(_bytes.subspan(checkedAt, checkedBytes));
  ByteReader trailer(_bytes.subspan(_at + FRAME_PREFIX_BYTES + payloadBytes, FRAME_TRAILER_BYTES));
  if (trailer.ReadUInt32() != expected)
  {
    return frame;
  }

  frame.ok = true;
  frame.recordKind = recordKind;
  frame.shardTick = shardTick;
  frame.payloadAt = _at + FRAME_PREFIX_BYTES;
  frame.payloadBytes = payloadBytes;
  frame.totalBytes = total;
  return frame;
}

/*
 * Is there a good frame *after* this point (ADR-025 §6.6)?
 *
 * The one question that separates the expected failure from the unexpected one.
 * A bad frame at the end is the write that was in flight when the power went;
 * a bad frame with good frames after it is corruption, and the difference is
 * whether the file is truncated or the shard refuses to start.
 */
[[nodiscard]] bool AnyGoodFrameAfter(std::span<const std::uint8_t> _bytes, std::size_t _from) noexcept
{
  for (std::size_t at = _from + 1; at + FRAME_PREFIX_BYTES + FRAME_TRAILER_BYTES <= _bytes.size(); ++at)
  {
    if (ParseFrame(_bytes, at).ok)
    {
      return true;
    }
  }
  return false;
}

} // namespace

DurableStore::~DurableStore()
{
  Close();
}

std::string DurableStore::JournalPath() const
{
  return Join(m_desc.directory, "shard-" + std::to_string(m_desc.hostId) + ".journal");
}

std::string DurableStore::SnapshotPath() const
{
  return Join(m_desc.directory, "shard-" + std::to_string(m_desc.hostId) + ".snapshot");
}

bool DurableStore::WriteJournalHeader(std::uint32_t _snapshotTick)
{
  std::array<std::uint8_t, JOURNAL_HEADER_BYTES> header{};
  ByteWriter writer(header);
  writer.WriteUInt32(JOURNAL_MAGIC);
  writer.WriteUInt32(JOURNAL_FORMAT_VERSION);
  writer.WriteUInt64(m_desc.universeHash);
  writer.WriteUInt64(m_desc.economyHash);
  writer.WriteUInt16(m_desc.hostId);
  writer.WriteUInt32(_snapshotTick);
  if (!writer.Ok() || m_journal == nullptr)
  {
    return false;
  }
  if (std::fwrite(header.data(), 1, header.size(), m_journal) != header.size())
  {
    return false;
  }
  CommitToDisk(m_journal);
  return true;
}

bool DurableStore::ReadSnapshotFile(DurableLoadReport& _outReport)
{
  std::vector<std::uint8_t> bytes;
  if (!ReadWholeFile(SnapshotPath(), bytes) || bytes.empty())
  {
    return true; // Not there is not an error: a shard that has never snapshotted.
  }

  ByteReader reader(bytes);
  const std::uint32_t magic = reader.ReadUInt32();
  const std::uint32_t version = reader.ReadUInt32();
  if (!reader.Ok() || magic != SNAPSHOT_MAGIC)
  {
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = SnapshotPath() + ": not a shard snapshot";
    return false;
  }
  if (version != JOURNAL_FORMAT_VERSION)
  {
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = SnapshotPath() + ": format version " + std::to_string(version) + ", this build knows "
                         + std::to_string(JOURNAL_FORMAT_VERSION);
    return false;
  }

  const std::uint64_t universeHash = reader.ReadUInt64();
  const std::uint64_t economyHash = reader.ReadUInt64();
  const std::uint16_t hostId = reader.ReadUInt16();
  const std::uint32_t shardTick = reader.ReadUInt32();
  const std::uint64_t durableHash = reader.ReadUInt64();
  const std::uint32_t payloadBytes = reader.ReadUInt32();
  if (!reader.Ok() || payloadBytes > MAX_DURABLE_RECORD_BYTES || payloadBytes > reader.BytesRemaining())
  {
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = SnapshotPath() + ": truncated before its state";
    return false;
  }

  if (universeHash != m_desc.universeHash)
  {
    /*
     * The one fatal content guard (ADR-025 §6.2). A re-baked universe
     * renumbers anchors, so a roster keyed by one is not merely stale, it
     * names somewhere else -- and the migration that would fix it is named as
     * unwritten in §9 rather than guessed at here.
     */
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = SnapshotPath() + ": universe hash " + Hex(universeHash) + ", this shard has " + Hex(m_desc.universeHash)
                         + " -- a re-baked universe renumbers anchors and there is no migration";
    return false;
  }
  _outReport.economyChanged = economyHash != m_desc.economyHash;

  const std::size_t payloadAt = reader.BytesRead();
  const std::uint32_t expected = Crc32(std::span<const std::uint8_t>(bytes.data() + payloadAt, payloadBytes));
  ByteReader trailer(std::span<const std::uint8_t>(bytes).subspan(payloadAt + payloadBytes));
  const std::uint32_t stored = trailer.ReadUInt32();
  if (!trailer.Ok() || stored != expected)
  {
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = SnapshotPath() + ": checksum does not match, so the file is damaged rather than old";
    return false;
  }

  (void)hostId;
  m_snapshot.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payloadAt),
                    bytes.begin() + static_cast<std::ptrdiff_t>(payloadAt + payloadBytes));
  m_snapshotTick = shardTick;
  _outReport.snapshotTick = shardTick;
  _outReport.snapshotDurableHash = durableHash;
  return true;
}

bool DurableStore::ReadJournalHeader(DurableLoadReport& _outReport, bool& _outPresent)
{
  _outPresent = false;
  std::vector<std::uint8_t> bytes;
  if (!ReadWholeFile(JournalPath(), bytes) || bytes.empty())
  {
    return true;
  }
  _outPresent = true;

  ByteReader reader(bytes);
  const std::uint32_t magic = reader.ReadUInt32();
  const std::uint32_t version = reader.ReadUInt32();
  if (!reader.Ok() || magic != JOURNAL_MAGIC)
  {
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = JournalPath() + ": not a shard journal";
    return false;
  }
  if (version != JOURNAL_FORMAT_VERSION)
  {
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = JournalPath() + ": format version " + std::to_string(version) + ", this build knows "
                         + std::to_string(JOURNAL_FORMAT_VERSION);
    return false;
  }
  const std::uint64_t universeHash = reader.ReadUInt64();
  const std::uint64_t economyHash = reader.ReadUInt64();
  (void)reader.ReadUInt16();
  const std::uint32_t snapshotTick = reader.ReadUInt32();
  if (!reader.Ok())
  {
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = JournalPath() + ": truncated inside its header";
    return false;
  }
  if (universeHash != m_desc.universeHash)
  {
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = JournalPath() + ": universe hash " + Hex(universeHash) + ", this shard has " + Hex(m_desc.universeHash);
    return false;
  }
  _outReport.economyChanged = _outReport.economyChanged || economyHash != m_desc.economyHash;

  /*
   * The snapshot the *journal* thinks it follows, kept only when there is no
   * snapshot file to say otherwise. It is the crash-between-rename-and-truncate
   * case in reverse: a journal whose header names a tick the snapshot no longer
   * agrees with loses, because the snapshot is the file the rename made
   * authoritative.
   */
  if (m_snapshot.empty())
  {
    m_snapshotTick = snapshotTick;
    _outReport.snapshotTick = snapshotTick;
  }
  return true;
}

bool DurableStore::Open(const DurableStoreDesc& _desc, DurableLoadReport& _outReport)
{
  Close();
  m_desc = _desc;
  m_snapshot.clear();
  m_snapshotTick = 0;
  _outReport = DurableLoadReport{};

  std::error_code error;
  if (!m_desc.directory.empty())
  {
    std::filesystem::create_directories(m_desc.directory, error);
    if (error && !std::filesystem::is_directory(m_desc.directory))
    {
      /*
       * A shard configured to persist that cannot open its directory refuses
       * to start (ADR-025 §6). The alternative is a service that looks healthy
       * and is quietly amnesiac, which is the failure this file exists to make
       * impossible.
       */
      _outReport.result = DurableLoadResult::Refused;
      _outReport.message = m_desc.directory + ": cannot be created or is not a directory";
      return false;
    }
  }

  if (!ReadSnapshotFile(_outReport))
  {
    return false;
  }
  bool journalPresent = false;
  if (!ReadJournalHeader(_outReport, journalPresent))
  {
    return false;
  }

  const bool anything = !m_snapshot.empty() || journalPresent;
  _outReport.result = anything ? DurableLoadResult::Loaded : DurableLoadResult::Fresh;
  if (!anything)
  {
    m_journal = OpenFile(JournalPath(), "wb");
    if (m_journal == nullptr)
    {
      _outReport.result = DurableLoadResult::Refused;
      _outReport.message = JournalPath() + ": cannot be written";
      return false;
    }
    if (!WriteJournalHeader(0))
    {
      _outReport.result = DurableLoadResult::Refused;
      _outReport.message = JournalPath() + ": its header could not be written";
      return false;
    }
    _outReport.message = "no shard state found: starting fresh at " + JournalPath();
  }
  return true;
}

bool DurableStore::Replay(const DurableReplayHandler& _handler, DurableLoadReport& _outReport)
{
  std::vector<std::uint8_t> bytes;
  const bool present = ReadWholeFile(JournalPath(), bytes) && bytes.size() >= JOURNAL_HEADER_BYTES;

  std::size_t at = present ? JOURNAL_HEADER_BYTES : 0;
  std::size_t goodBytes = at;
  std::uint32_t replayed = 0;
  std::uint32_t skipped = 0;
  bool torn = false;

  while (present && at < bytes.size())
  {
    const ParsedFrame frame = ParseFrame(bytes, at);
    if (!frame.ok)
    {
      if (AnyGoodFrameAfter(bytes, at))
      {
        // Corruption, not a torn tail: refuse and leave both files untouched
        // for whoever has to look (ADR-025 §6.6).
        _outReport.result = DurableLoadResult::Refused;
        _outReport.message = JournalPath() + ": a damaged record at byte " + std::to_string(at)
                             + " with good records after it -- this is corruption, not an interrupted write";
        return false;
      }
      torn = true;
      break;
    }

    // Records older than the snapshot are the crash-between-rename-and-truncate
    // case (ADR-025 §5): the snapshot already contains them.
    if (frame.shardTick > m_snapshotTick)
    {
      if (!_handler(frame.recordKind, frame.shardTick, std::span<const std::uint8_t>(bytes).subspan(frame.payloadAt, frame.payloadBytes)))
      {
        _outReport.result = DurableLoadResult::Refused;
        _outReport.message = JournalPath() + ": record " + std::to_string(replayed + skipped) + " of kind "
                             + std::to_string(frame.recordKind) + " was refused by the shard";
        return false;
      }
      ++replayed;
    }
    else
    {
      ++skipped;
    }

    at += frame.totalBytes;
    goodBytes = at;
  }

  _outReport.recordsReplayed = replayed;
  _outReport.bytesDiscarded = present ? static_cast<std::uint64_t>(bytes.size() - goodBytes) : 0;

  if (torn && _outReport.bytesDiscarded > 0)
  {
    std::error_code error;
    std::filesystem::resize_file(JournalPath(), goodBytes, error);
  }

  /*
   * The file is now exactly its good part, so appends continue from there --
   * and the handle `Open` left behind is closed first.
   *
   * Not tidiness: `Open` opens the journal so that a fresh shard can append
   * before it has replayed anything, and reopening over that handle would leak
   * it on every platform and be *refused* on the one that ships, where a second
   * writable handle to an open file is an error rather than a second handle.
   */
  if (m_journal != nullptr)
  {
    std::fclose(m_journal);
    m_journal = nullptr;
  }
  m_journal = OpenFile(JournalPath(), present ? "r+b" : "wb");
  if (m_journal == nullptr)
  {
    _outReport.result = DurableLoadResult::Refused;
    _outReport.message = JournalPath() + ": cannot be reopened for writing";
    return false;
  }
  if (!present)
  {
    if (!WriteJournalHeader(m_snapshotTick))
    {
      _outReport.result = DurableLoadResult::Refused;
      _outReport.message = JournalPath() + ": its header could not be written";
      return false;
    }
  }
  else
  {
    std::fseek(m_journal, 0, SEEK_END);
  }

  _outReport.result = DurableLoadResult::Loaded;
  _outReport.message = "recovered " + std::to_string(replayed) + " record(s) from " + JournalPath();
  if (_outReport.bytesDiscarded > 0)
  {
    // Expected, and logged as a count rather than as an error: this is the
    // write that was in flight when the power went.
    _outReport.message += ", discarding " + std::to_string(_outReport.bytesDiscarded) + " byte(s) of an interrupted write";
  }
  if (skipped > 0)
  {
    _outReport.message += ", skipping " + std::to_string(skipped) + " record(s) older than the snapshot";
  }
  return true;
}

void DurableStore::Append(std::uint16_t _recordKind, std::uint32_t _shardTick, std::span<const std::uint8_t> _payload)
{
  if (m_journal == nullptr || _payload.size() > MAX_DURABLE_RECORD_BYTES)
  {
    return;
  }

  const std::size_t total = FRAME_PREFIX_BYTES + _payload.size() + FRAME_TRAILER_BYTES;
  m_frame.assign(total, std::uint8_t{0});
  ByteWriter writer(m_frame);
  writer.WriteUInt32(FRAME_MAGIC);
  writer.WriteUInt32(static_cast<std::uint32_t>(_payload.size()));
  writer.WriteUInt16(_recordKind);
  writer.WriteUInt32(_shardTick);
  writer.WriteBytes(_payload.data(), _payload.size());
  const std::uint32_t crc = Crc32(std::span<const std::uint8_t>(m_frame).subspan(4 + 4, 2 + 4 + _payload.size()));
  writer.WriteUInt32(crc);
  if (!writer.Ok())
  {
    return;
  }

  // One call, so a torn write is torn at the end of a frame and never in the
  // middle of a field -- which is what makes the tail detectable rather than
  // plausible.
  (void)std::fwrite(m_frame.data(), 1, m_frame.size(), m_journal);
}

void DurableStore::Flush()
{
  CommitToDisk(m_journal);
}

bool DurableStore::WriteSnapshot(std::span<const std::uint8_t> _state, std::uint64_t _durableHash, std::uint32_t _shardTick)
{
  if (m_journal == nullptr || _state.size() > MAX_DURABLE_RECORD_BYTES)
  {
    return false;
  }

  const std::string temporary = SnapshotPath() + ".tmp";
  const std::size_t headerBytes = 4 + 4 + 8 + 8 + 2 + 4 + 8 + 4;
  std::vector<std::uint8_t> file(headerBytes + _state.size() + 4);
  ByteWriter writer(file);
  writer.WriteUInt32(SNAPSHOT_MAGIC);
  writer.WriteUInt32(JOURNAL_FORMAT_VERSION);
  writer.WriteUInt64(m_desc.universeHash);
  writer.WriteUInt64(m_desc.economyHash);
  writer.WriteUInt16(m_desc.hostId);
  writer.WriteUInt32(_shardTick);
  writer.WriteUInt64(_durableHash);
  writer.WriteUInt32(static_cast<std::uint32_t>(_state.size()));
  writer.WriteBytes(_state.data(), _state.size());
  writer.WriteUInt32(Crc32(_state));
  if (!writer.Ok())
  {
    return false;
  }

  // 1. Write the temporary and push it to the disk. A crash before step 2
  //    leaves the previous snapshot and a journal that still covers everything
  //    after it.
  std::FILE* out = OpenFile(temporary, "wb");
  if (out == nullptr)
  {
    return false;
  }
  const bool written = std::fwrite(file.data(), 1, file.size(), out) == file.size();
  CommitToDisk(out);
  std::fclose(out);
  if (!written)
  {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }

  // 2. Rename it over the snapshot. A crash between here and step 3 leaves a
  //    good snapshot and a journal of records older than it, which replay
  //    skips by tick.
  std::error_code error;
  std::filesystem::rename(temporary, SnapshotPath(), error);
  if (error)
  {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }

  // 3. Truncate the journal and write its header with the new snapshot tick.
  std::fclose(m_journal);
  m_journal = OpenFile(JournalPath(), "wb");
  if (m_journal == nullptr)
  {
    return false;
  }
  if (!WriteJournalHeader(_shardTick))
  {
    return false;
  }

  m_snapshotTick = _shardTick;
  return true;
}

void DurableStore::Close()
{
  if (m_journal != nullptr)
  {
    CommitToDisk(m_journal);
    std::fclose(m_journal);
    m_journal = nullptr;
  }
}

} // namespace Neuron
