#include "pch.h"
#include "CppUnitTest.h"

#include "DurableStore.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * E4a: the store, with no game in it (ADR-025 §8, build order E4a).
 *
 * Everything here is about *files* -- framing, checksums, a torn tail, a
 * rotation interrupted at each of its three steps -- and nothing here knows
 * what a record means. That is the seam ADR-025 §2 draws, and testing it this
 * way is how the seam stays drawn: if one of these tests ever needed to know
 * that kind 3 is a Bay, the engine would have learned the game.
 */

namespace NeuronServerTests
{

namespace
{

/*
 * The store's own `OpenFile`, repeated here rather than exported.
 *
 * MSVC refuses `std::fopen` outright under this tree's conformance settings
 * (C4996 as an error, which is what `Log.cpp` uses `fopen_s` for), and these
 * tests have to open the files by hand to damage them. Exporting the store's
 * helper to spare four lines would put a file-opening function on a public
 * header for the sake of its test.
 */
[[nodiscard]] std::FILE* OpenRaw(const std::string& _path, const char* _mode) noexcept
{
#if defined(_MSC_VER)
  std::FILE* file = nullptr;
  return fopen_s(&file, _path.c_str(), _mode) == 0 ? file : nullptr;
#else
  return std::fopen(_path.c_str(), _mode);
#endif
}

/// A directory of its own per test, removed first so a previous run cannot make
/// this one pass or fail.
[[nodiscard]] std::string Scratch(const char* _name)
{
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "outpost-durable" / _name;
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
  std::filesystem::create_directories(path, ignored);
  return path.string();
}

[[nodiscard]] DurableStoreDesc Desc(const std::string& _directory)
{
  DurableStoreDesc desc;
  desc.directory = _directory;
  desc.hostId = 0;
  desc.universeHash = 0xad9555dd776008a6ull;
  desc.economyHash = 0x0b07707ec843431dull;
  return desc;
}

[[nodiscard]] std::vector<std::uint8_t> Payload(std::uint8_t _seed, std::size_t _bytes)
{
  std::vector<std::uint8_t> payload(_bytes);
  for (std::size_t index = 0; index < _bytes; ++index)
  {
    payload[index] = static_cast<std::uint8_t>(_seed + index);
  }
  return payload;
}

/// What a replay handed back, so a test can compare a whole run at once.
struct Seen
{
  std::uint16_t kind = 0;
  std::uint32_t tick = 0;
  std::vector<std::uint8_t> payload;
};

[[nodiscard]] DurableReplayHandler Collect(std::vector<Seen>& _into)
{
  return [&_into](std::uint16_t _kind, std::uint32_t _tick, std::span<const std::uint8_t> _payload)
  {
    Seen seen;
    seen.kind = _kind;
    seen.tick = _tick;
    seen.payload.assign(_payload.begin(), _payload.end());
    _into.push_back(std::move(seen));
    return true;
  };
}

[[nodiscard]] std::vector<std::uint8_t> ReadAll(const std::string& _path)
{
  std::vector<std::uint8_t> bytes;
  std::FILE* file = OpenRaw(_path, "rb");
  if (file == nullptr)
  {
    return bytes;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  bytes.resize(static_cast<std::size_t>(size < 0 ? 0 : size));
  if (!bytes.empty())
  {
    bytes.resize(std::fread(bytes.data(), 1, bytes.size(), file));
  }
  std::fclose(file);
  return bytes;
}

void WriteAll(const std::string& _path, const std::vector<std::uint8_t>& _bytes)
{
  std::FILE* file = OpenRaw(_path, "wb");
  Assert::IsNotNull(file, L"the fixture could not write a file it needs to damage");
  if (!_bytes.empty())
  {
    (void)std::fwrite(_bytes.data(), 1, _bytes.size(), file);
  }
  std::fclose(file);
}

} // namespace

TEST_CLASS(DurableStoreFramingTests)
{
public:
  TEST_METHOD(AnEmptyDirectoryIsAFreshShard)
  {
    const std::string directory = Scratch("fresh");
    DurableStore store;
    DurableLoadReport report;
    Assert::IsTrue(store.Open(Desc(directory), report), L"an empty directory would not open");
    Assert::IsTrue(report.result == DurableLoadResult::Fresh, L"an empty directory did not read as a fresh shard");
    Assert::AreEqual<std::size_t>(0, store.Snapshot().size(), L"a fresh shard produced a snapshot");
    Assert::IsTrue(report.message.find("fresh") != std::string::npos, L"the report did not say what happened");
  }

  TEST_METHOD(WhatIsAppendedIsWhatComesBack)
  {
    const std::string directory = Scratch("framing");
    DurableStoreDesc desc = Desc(directory);

    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(desc, report), L"the store would not open");
      store.Append(7, 100, Payload(1, 16));
      store.Append(9, 101, Payload(2, 0)); // A record with no payload is still a record.
      store.Append(7, 102, Payload(3, 4096));
      store.Flush();
      store.Close();
    }

    DurableStore reopened;
    DurableLoadReport report;
    Assert::IsTrue(reopened.Open(desc, report), L"the store would not reopen");
    std::vector<Seen> seen;
    Assert::IsTrue(reopened.Replay(Collect(seen), report), L"the journal would not replay");
    Assert::AreEqual<std::size_t>(3, seen.size(), L"the journal lost a record");
    Assert::AreEqual<std::uint32_t>(3, report.recordsReplayed);
    Assert::AreEqual<std::uint64_t>(0, report.bytesDiscarded, L"an undamaged journal discarded bytes");
    Assert::AreEqual<std::uint16_t>(7, seen[0].kind);
    Assert::AreEqual<std::uint32_t>(100, seen[0].tick);
    Assert::IsTrue(seen[0].payload == Payload(1, 16), L"a payload came back different");
    Assert::AreEqual<std::size_t>(0, seen[1].payload.size(), L"an empty payload came back non-empty");
    Assert::IsTrue(seen[2].payload == Payload(3, 4096), L"a large payload came back different");
  }

  TEST_METHOD(TheJournalSurvivesTheOpenReplayAppendLifecycle)
  {
    /*
     * One handle at a time, across the whole boot sequence.
     *
     * `Open` leaves the journal open so a fresh shard can append before it has
     * replayed anything, and `Replay` reopens it after truncating the tail --
     * so the handle has to be handed over rather than shadowed. On Linux a
     * second writable handle merely leaks; on the platform that ships it is
     * refused outright, and the shard would then persist nothing while every
     * other check passed.
     */
    const std::string directory = Scratch("lifecycle");
    DurableStoreDesc desc = Desc(directory);

    DurableStore store;
    DurableLoadReport report;
    Assert::IsTrue(store.Open(desc, report), L"the store would not open");
    store.Append(1, 10, Payload(1, 16)); // Before any replay.

    std::vector<Seen> seen;
    Assert::IsTrue(store.Replay(Collect(seen), report), L"the journal would not replay");
    store.Append(2, 11, Payload(2, 16)); // And after it.
    store.Flush();
    store.Close();

    DurableStore reopened;
    DurableLoadReport reopenedReport;
    Assert::IsTrue(reopened.Open(desc, reopenedReport), L"the store would not reopen");
    std::vector<Seen> after;
    Assert::IsTrue(reopened.Replay(Collect(after), reopenedReport), L"the journal would not replay a second time");
    Assert::AreEqual<std::size_t>(2, after.size(), L"a record was lost across the open/replay/append lifecycle");
    Assert::AreEqual<std::uint16_t>(1, after[0].kind);
    Assert::AreEqual<std::uint16_t>(2, after[1].kind);
  }

  TEST_METHOD(ATornTailRecoversToTheLastGoodRecord)
  {
    /*
     * The expected failure. A journal ends mid-frame because the power went
     * during a write; that is not corruption, it is Tuesday, and it is logged
     * as a count of bytes discarded rather than as an error.
     */
    const std::string directory = Scratch("torn");
    DurableStoreDesc desc = Desc(directory);

    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(desc, report), L"the store would not open");
      store.Append(1, 10, Payload(1, 32));
      store.Append(2, 11, Payload(2, 32));
      store.Append(3, 12, Payload(3, 32));
      store.Close();
    }

    const std::string journal = std::filesystem::path(directory).append("shard-0.journal").string();
    std::vector<std::uint8_t> bytes = ReadAll(journal);
    const std::size_t full = bytes.size();
    bytes.resize(full - 20); // Halfway through the third frame.
    WriteAll(journal, bytes);

    DurableStore store;
    DurableLoadReport report;
    Assert::IsTrue(store.Open(desc, report), L"a torn journal refused to open");
    std::vector<Seen> seen;
    Assert::IsTrue(store.Replay(Collect(seen), report), L"a torn tail was treated as corruption");
    Assert::AreEqual<std::size_t>(2, seen.size(), L"a torn tail lost more than the interrupted write");
    Assert::IsTrue(report.bytesDiscarded > 0, L"the report did not say anything was discarded");
    Assert::IsTrue(report.message.find("interrupted write") != std::string::npos, L"the report did not name what it discarded");

    // And the file is now exactly its good part, so the next append lands
    // cleanly rather than after a hole.
    store.Append(4, 13, Payload(4, 8));
    store.Flush();
    store.Close();

    DurableStore after;
    DurableLoadReport afterReport;
    Assert::IsTrue(after.Open(desc, afterReport), L"the repaired journal would not open");
    std::vector<Seen> afterSeen;
    Assert::IsTrue(after.Replay(Collect(afterSeen), afterReport), L"the repaired journal would not replay");
    Assert::AreEqual<std::size_t>(3, afterSeen.size(), L"the append after a torn tail did not survive");
    Assert::AreEqual<std::uint16_t>(4, afterSeen[2].kind);
  }

  TEST_METHOD(CorruptionInTheMiddleRefusesAndTouchesNothing)
  {
    /*
     * The unexpected failure, and the one place this file says no. A damaged
     * record with good records after it cannot be an interrupted write, so
     * truncating there would throw away good state to make a bad file parse --
     * and the shard would come up looking healthy.
     */
    const std::string directory = Scratch("corrupt");
    DurableStoreDesc desc = Desc(directory);

    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(desc, report), L"the store would not open");
      store.Append(1, 10, Payload(1, 64));
      store.Append(2, 11, Payload(2, 64));
      store.Append(3, 12, Payload(3, 64));
      store.Close();
    }

    const std::string journal = std::filesystem::path(directory).append("shard-0.journal").string();
    std::vector<std::uint8_t> bytes = ReadAll(journal);
    const std::size_t before = bytes.size();
    bytes[60] = static_cast<std::uint8_t>(bytes[60] ^ 0xffu); // Inside the first record's payload.
    WriteAll(journal, bytes);

    DurableStore store;
    DurableLoadReport report;
    Assert::IsTrue(store.Open(desc, report), L"the header is undamaged, so opening should still work");
    std::vector<Seen> seen;
    Assert::IsFalse(store.Replay(Collect(seen), report), L"corruption in the middle was replayed anyway");
    Assert::IsTrue(report.result == DurableLoadResult::Refused, L"corruption did not refuse");
    Assert::IsTrue(report.message.find("corruption") != std::string::npos, L"the refusal did not say what it found");
    Assert::AreEqual<std::size_t>(before, ReadAll(journal).size(), L"a refused load damaged the evidence");
  }

  TEST_METHOD(ACorruptLengthIsBoundedRatherThanBelieved)
  {
    // A length read from a file is not trusted until it has been bounded. The
    // failure this prevents is an allocation the size of whatever the damaged
    // bytes happened to say.
    const std::string directory = Scratch("length");
    DurableStoreDesc desc = Desc(directory);
    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(desc, report), L"the store would not open");
      store.Append(1, 10, Payload(1, 16));
      store.Close();
    }

    const std::string journal = std::filesystem::path(directory).append("shard-0.journal").string();
    std::vector<std::uint8_t> bytes = ReadAll(journal);
    // The frame's length field: magic (4 bytes) into the frame, which starts
    // right after the journal header.
    const std::size_t lengthAt = 30 + 4;
    bytes[lengthAt + 3] = 0x7fu; // ~2 GB, which is past the cap and past the file.
    WriteAll(journal, bytes);

    DurableStore store;
    DurableLoadReport report;
    Assert::IsTrue(store.Open(desc, report), L"the header is undamaged, so opening should still work");
    std::vector<Seen> seen;
    Assert::IsTrue(store.Replay(Collect(seen), report), L"an impossible length should read as a torn tail, not corruption");
    Assert::AreEqual<std::size_t>(0, seen.size(), L"a record with an impossible length was replayed");
  }
};

TEST_CLASS(DurableStoreRotationTests)
{
public:
  TEST_METHOD(ASnapshotComesBackWithItsTickAndItsProof)
  {
    const std::string directory = Scratch("snapshot");
    DurableStoreDesc desc = Desc(directory);
    const std::vector<std::uint8_t> state = Payload(9, 512);

    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(desc, report), L"the store would not open");
      store.Append(1, 10, Payload(1, 8));
      Assert::IsTrue(store.WriteSnapshot(state, 0xfeedfacecafebeefull, 500), L"the snapshot would not write");
      store.Close();
    }

    DurableStore store;
    DurableLoadReport report;
    Assert::IsTrue(store.Open(desc, report), L"the snapshot would not reopen");
    Assert::IsTrue(report.result == DurableLoadResult::Loaded, L"a shard with a snapshot read as fresh");
    Assert::AreEqual<std::uint32_t>(500, report.snapshotTick, L"the snapshot tick did not survive");
    Assert::AreEqual<std::uint64_t>(0xfeedfacecafebeefull, report.snapshotDurableHash, L"the reload proof did not survive");
    Assert::IsTrue(std::vector<std::uint8_t>(store.Snapshot().begin(), store.Snapshot().end()) == state,
                   L"the snapshot came back different");

    // Step 3 of the rotation: the journal was truncated, so the record that
    // preceded the snapshot is not replayed on top of it.
    std::vector<Seen> seen;
    Assert::IsTrue(store.Replay(Collect(seen), report), L"the journal would not replay");
    Assert::AreEqual<std::size_t>(0, seen.size(), L"a snapshot did not truncate its journal");
  }

  TEST_METHOD(ACrashBeforeTheRenameLeavesThePreviousSnapshotWhole)
  {
    /*
     * Rotation step 1, interrupted. The temporary is on disk and the rename
     * never happened, so the previous snapshot is still the snapshot and the
     * journal still covers everything after it -- which is the entire reason
     * the write goes to a temporary at all.
     */
    const std::string directory = Scratch("rotate1");
    DurableStoreDesc desc = Desc(directory);
    const std::vector<std::uint8_t> first = Payload(1, 128);

    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(desc, report), L"the store would not open");
      Assert::IsTrue(store.WriteSnapshot(first, 111, 100), L"the first snapshot would not write");
      store.Append(5, 150, Payload(5, 16));
      store.Close();
    }

    // A half-written temporary, exactly as a crash between the write and the
    // rename would leave it.
    WriteAll(std::filesystem::path(directory).append("shard-0.snapshot.tmp").string(), Payload(0xEE, 64));

    DurableStore store;
    DurableLoadReport report;
    Assert::IsTrue(store.Open(desc, report), L"an abandoned temporary stopped the shard from opening");
    Assert::AreEqual<std::uint32_t>(100, report.snapshotTick, L"the previous snapshot was not the one that came back");
    Assert::IsTrue(std::vector<std::uint8_t>(store.Snapshot().begin(), store.Snapshot().end()) == first,
                   L"a half-written temporary was read as the snapshot");
    std::vector<Seen> seen;
    Assert::IsTrue(store.Replay(Collect(seen), report), L"the journal would not replay");
    Assert::AreEqual<std::size_t>(1, seen.size(), L"the record written after the previous snapshot was lost");
  }

  TEST_METHOD(ACrashBeforeTheTruncateSkipsTheOldRecordsByTick)
  {
    /*
     * Rotation step 2, interrupted: a good snapshot and a journal of records
     * older than it. Replaying them would apply outcomes the snapshot already
     * contains, so they are skipped by tick -- which is why every frame is
     * stamped (ADR-025 §3) and why the snapshot's tick is the floor.
     */
    const std::string directory = Scratch("rotate2");
    DurableStoreDesc desc = Desc(directory);
    const std::string journal = std::filesystem::path(directory).append("shard-0.journal").string();

    std::vector<std::uint8_t> journalBeforeSnapshot;
    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(desc, report), L"the store would not open");
      store.Append(1, 10, Payload(1, 16));
      store.Append(2, 20, Payload(2, 16));
      store.Append(3, 400, Payload(3, 16)); // Newer than the snapshot, so it survives.
      store.Flush();
      journalBeforeSnapshot = ReadAll(journal);
      Assert::IsTrue(store.WriteSnapshot(Payload(7, 64), 222, 300), L"the snapshot would not write");
      store.Close();
    }

    // The rename landed and the truncate did not.
    WriteAll(journal, journalBeforeSnapshot);

    DurableStore store;
    DurableLoadReport report;
    Assert::IsTrue(store.Open(desc, report), L"the shard would not open");
    Assert::AreEqual<std::uint32_t>(300, report.snapshotTick, L"the new snapshot did not survive");
    std::vector<Seen> seen;
    Assert::IsTrue(store.Replay(Collect(seen), report), L"the stale journal would not replay");
    Assert::AreEqual<std::size_t>(1, seen.size(), L"records the snapshot already contains were applied on top of it");
    Assert::AreEqual<std::uint32_t>(400, seen[0].tick, L"the wrong record survived");
    Assert::IsTrue(report.message.find("older than the snapshot") != std::string::npos,
                   L"the report did not say what it skipped");
  }
};

TEST_CLASS(DurableStoreGuardTests)
{
public:
  TEST_METHOD(ARebakedUniverseIsRefusedAndBothNumbersAreNamed)
  {
    /*
     * ADR-025 §6.2's one fatal guard. A re-baked universe renumbers anchors, so
     * a roster keyed by one does not name a stale place, it names somewhere
     * else -- and the migration that would fix it is named as unwritten rather
     * than guessed at.
     */
    const std::string directory = Scratch("universe");
    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(Desc(directory), report), L"the store would not open");
      Assert::IsTrue(store.WriteSnapshot(Payload(1, 32), 5, 50), L"the snapshot would not write");
      store.Close();
    }

    DurableStoreDesc rebaked = Desc(directory);
    rebaked.universeHash = 0x1111222233334444ull;
    DurableStore store;
    DurableLoadReport report;
    Assert::IsFalse(store.Open(rebaked, report), L"a shard opened against a universe it was not written for");
    Assert::IsTrue(report.result == DurableLoadResult::Refused, L"a re-baked universe did not refuse");
    Assert::IsTrue(report.message.find("1111222233334444") != std::string::npos, L"the refusal did not name the shard's hash");
    Assert::IsTrue(report.message.find("ad9555dd776008a6") != std::string::npos, L"the refusal did not name the file's hash");
  }

  TEST_METHOD(ARetunedEconomyIsNotedAndNotFatal)
  {
    // Retuning a hold size must not invalidate a shard (ADR-025 §6.2). The
    // numbers moved under the content, which is a content decision and not a
    // corruption -- so it is reported and the shard starts.
    const std::string directory = Scratch("economy");
    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(Desc(directory), report), L"the store would not open");
      Assert::IsTrue(store.WriteSnapshot(Payload(1, 32), 5, 50), L"the snapshot would not write");
      store.Close();
    }

    DurableStoreDesc retuned = Desc(directory);
    retuned.economyHash = 0x5555666677778888ull;
    DurableStore store;
    DurableLoadReport report;
    Assert::IsTrue(store.Open(retuned, report), L"a retuned economy stopped a shard from starting");
    Assert::IsTrue(report.economyChanged, L"a retuned economy was not noticed at all");
    Assert::AreEqual<std::uint32_t>(50, report.snapshotTick, L"the state did not survive a retune");
  }

  TEST_METHOD(ADamagedSnapshotRefusesRatherThanLoadingHalfAShard)
  {
    const std::string directory = Scratch("damaged");
    {
      DurableStore store;
      DurableLoadReport report;
      Assert::IsTrue(store.Open(Desc(directory), report), L"the store would not open");
      Assert::IsTrue(store.WriteSnapshot(Payload(1, 256), 5, 50), L"the snapshot would not write");
      store.Close();
    }

    const std::string path = std::filesystem::path(directory).append("shard-0.snapshot").string();
    std::vector<std::uint8_t> bytes = ReadAll(path);
    bytes[bytes.size() / 2] = static_cast<std::uint8_t>(bytes[bytes.size() / 2] ^ 0xffu);
    WriteAll(path, bytes);

    DurableStore store;
    DurableLoadReport report;
    Assert::IsFalse(store.Open(Desc(directory), report), L"a damaged snapshot was loaded");
    Assert::IsTrue(report.message.find("checksum") != std::string::npos, L"the refusal did not say the file was damaged");
  }

  TEST_METHOD(SomebodyElsesFileIsRefused)
  {
    const std::string directory = Scratch("foreign");
    WriteAll(std::filesystem::path(directory).append("shard-0.snapshot").string(), Payload(0x11, 256));

    DurableStore store;
    DurableLoadReport report;
    Assert::IsFalse(store.Open(Desc(directory), report), L"a foreign file was read as a snapshot");
    Assert::IsTrue(report.message.find("not a shard snapshot") != std::string::npos, L"the refusal did not say what it was refusing");
  }
};

} // namespace NeuronServerTests
