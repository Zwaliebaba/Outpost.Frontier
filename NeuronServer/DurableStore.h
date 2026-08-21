#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <vector>

/*
 * The durable store (ADR-025 §3, §5, §6; build order E4a).
 *
 * Files, framing, checksums, flushing, snapshot rotation and torn-tail
 * recovery -- and **it never learns what it is storing.** A record has a kind,
 * a length and a checksum; that one of those kinds is a station Bay is a fact
 * that lives one library up, in `Outpost.exe`, which is the only place allowed
 * to know both halves (ADR-014 §2a).
 *
 * That is not fastidiousness. `Neuron*` is a shared engine -- the sibling
 * repository runs a different game on it -- so a store that knew what a Bay was
 * would be a store that shipped a different game's vocabulary to it.
 *
 * **Single-writer.** One thread appends and flushes: the journal lane
 * (ADR-025 §4, ADR-007 §8). The Sim thread serialises records inside the apply
 * point where the state is legally readable, hands them to a ring, and never
 * touches this. Nothing here takes a lock, because nothing here is shared.
 */

namespace Neuron
{

/*
 * The framing version, in the file header (ADR-025 §3).
 *
 * **This versions the frame, not the payload.** GameLogic's blob carries its
 * own `DURABLE_FORMAT_VERSION` because the two really do move independently:
 * the slice that adds refine jobs changes what a payload contains and nothing
 * at all about how a record is wrapped, and one number covering both would
 * refuse every old shard for a change that could not affect it.
 */
inline constexpr std::uint32_t JOURNAL_FORMAT_VERSION = 1;

/// How long accepted outcomes may sit unwritten (ADR-025 §4). A hard kill loses
/// at most this much; a clean stop loses nothing, because shutdown flushes
/// before ADR-008's ordering releases Sim.
inline constexpr std::uint32_t JOURNAL_FLUSH_MILLISECONDS = 1000;

/// How often the whole durable state is written and the journal truncated
/// (ADR-025 §5).
inline constexpr std::uint32_t SNAPSHOT_INTERVAL_SECONDS = 300;

/// How often a checkpoint record carries a hash of everything (ADR-025 §5). One
/// minute at 20 Hz: hashing every record would double the cost of the cheapest
/// ones, and hashing only at snapshot time would let a divergence hide for five.
inline constexpr std::uint32_t CHECKPOINT_INTERVAL_TICKS = 1200;

/*
 * The biggest single record this will read.
 *
 * A bound on a length read from a file **before** a byte of it is trusted: a
 * corrupt `payloadBytes` is otherwise an allocation the size of whatever the
 * damaged bytes happened to say. Sixty-four megabytes is far past any snapshot
 * this shard produces and far short of a number that hurts to refuse.
 */
inline constexpr std::uint32_t MAX_DURABLE_RECORD_BYTES = 64u * 1024u * 1024u;

/// What `Open` found.
enum class DurableLoadResult : std::uint8_t
{
  /// Nothing was there. A new shard, and the caller starts from content.
  Fresh = 0,

  /// A snapshot and/or journal was read. `Snapshot()` and `Replay` have it.
  Loaded = 1,

  /*
   * A guard failed and **the caller must not start** (ADR-025 §6).
   *
   * A refusal is not a fallback. A shard configured to persist that comes up
   * with nothing is a service that looks healthy and is quietly amnesiac, which
   * is the failure this whole file exists to make impossible.
   */
  Refused = 2
};

/// What happened at boot, in the words a log line and an operator need.
struct DurableLoadReport
{
  DurableLoadResult result = DurableLoadResult::Fresh;

  /// The tick the snapshot was taken at, and the floor journal replay applies
  /// above (ADR-025 §6.4).
  std::uint32_t snapshotTick = 0;

  /*
   * The reload proof the snapshot recorded (ADR-025 §1a).
   *
   * Reported rather than checked: this library cannot compute a game's
   * `DurableHash`, and one that could would be a library that knows what a Bay
   * is. The caller compares after loading, which is also the only moment the
   * comparison means anything.
   */
  std::uint64_t snapshotDurableHash = 0;

  std::uint32_t recordsReplayed = 0;

  /// The torn tail: the write that was in flight when the power went. Expected,
  /// logged as a count rather than as an error, and truncated away.
  std::uint64_t bytesDiscarded = 0;

  /// True when the economy hash moved under the shard. Not fatal (ADR-025
  /// §6.2) -- retuning a hold size must not invalidate a shard -- and the
  /// caller is the one that knows what to clamp.
  bool economyChanged = false;

  /// What happened, or why it refused. Always populated.
  std::string message;
};

/// Where the files live and what the load is guarded on.
struct DurableStoreDesc
{
  /// A directory, resolved by the caller (ADR-012: libraries receive config,
  /// they do not read it). Created if it is not there.
  std::string directory;

  /// One pair of files per host (ADR-025 §7): `shard-0.journal`,
  /// `shard-0.snapshot`. At one host that is a naming convention doing nothing,
  /// which is exactly the ADR-019 posture.
  std::uint16_t hostId = 0;

  /// The fatal guard (ADR-025 §6.2): a re-bake renumbers anchors, so a roster
  /// keyed by one is nonsense against it.
  std::uint64_t universeHash = 0;

  /// Recorded, compared, and never fatal. Retuning balance must not invalidate
  /// a shard.
  std::uint64_t economyHash = 0;
};

/*
 * Each surviving journal record, in file order.
 *
 * Returning false stops the replay and fails the load -- which is how a caller
 * refuses a record it cannot apply, and how a **checkpoint** record fails when
 * its hash does not match. Checkpoints are the caller's own record kind, not
 * this library's: verifying one takes a game hash, and a store that could
 * compute one would know what it was storing.
 */
using DurableReplayHandler = std::function<bool(std::uint16_t _recordKind, std::uint32_t _shardTick,
                                                std::span<const std::uint8_t> _payload)>;

class DurableStore
{
public:
  DurableStore() = default;
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  ~DurableStore();

  /*
   * Opens the store and recovers whatever is there (ADR-025 §6), in order:
   * the header, the guards, the snapshot, and then `Replay`'s journal.
   *
   * False means **do not start**, with `_outReport.message` naming which check
   * failed and what the two values were. True with `Fresh` is a new shard; true
   * with `Loaded` means `Snapshot()` holds the blob and `Replay` has records to
   * hand back.
   */
  [[nodiscard]] bool Open(const DurableStoreDesc& _desc, DurableLoadReport& _outReport);

  /// The snapshot blob, for the caller to read into its own state. Empty for a
  /// fresh shard.
  [[nodiscard]] std::span<const std::uint8_t> Snapshot() const noexcept { return m_snapshot; }

  /*
   * Hands back every journal record newer than the snapshot, in file order.
   *
   * Called once, after `Open` and after the snapshot has been loaded, because a
   * record is an outcome applied *on top of* the state the snapshot describes.
   */
  [[nodiscard]] bool Replay(const DurableReplayHandler& _handler, DurableLoadReport& _outReport);

  /*
   * Appends one outcome.
   *
   * The tick is a parameter rather than store state: every frame is stamped
   * (ADR-025 §3) and a store holding "the current tick" would be a second clock
   * to forget to advance. Buffered -- `Flush` is what makes it durable.
   */
  void Append(std::uint16_t _recordKind, std::uint32_t _shardTick, std::span<const std::uint8_t> _payload);

  /// Pushes what is buffered all the way to the disk. Called on the flush
  /// cadence and unconditionally on a clean shutdown.
  void Flush();

  /*
   * The whole durable state, and the rotation that makes it safe (ADR-025 §5).
   *
   * Three steps, each one there because the crash between it and the next has
   * to be survivable: write the `.tmp` and flush it; rename it over the
   * snapshot; truncate the journal and rewrite its header with the new tick. A
   * crash before the rename leaves the previous snapshot with a journal that
   * still covers everything after it; a crash between the rename and the
   * truncate leaves a good snapshot and a journal of records older than it,
   * which replay skips by tick. There is no window in which both files are
   * needed and one is missing.
   */
  [[nodiscard]] bool WriteSnapshot(std::span<const std::uint8_t> _state, std::uint64_t _durableHash,
                                   std::uint32_t _shardTick);

  /// Flushes and closes. Safe to call twice, because shutdown paths get called
  /// twice and must not care.
  void Close();

  [[nodiscard]] bool IsOpen() const noexcept { return m_journal != nullptr; }

  /// Where the two files are, for a log line that has to be actionable.
  [[nodiscard]] std::string JournalPath() const;
  [[nodiscard]] std::string SnapshotPath() const;

private:
  [[nodiscard]] bool WriteJournalHeader(std::uint32_t _snapshotTick);
  [[nodiscard]] bool ReadJournalHeader(DurableLoadReport& _outReport, bool& _outPresent);
  [[nodiscard]] bool ReadSnapshotFile(DurableLoadReport& _outReport);

  DurableStoreDesc m_desc;
  std::FILE* m_journal = nullptr;

  /// The snapshot's own tick, and the floor for journal replay.
  std::uint32_t m_snapshotTick = 0;

  std::vector<std::uint8_t> m_snapshot;

  /// One scratch buffer, reused: a frame is assembled here and written in a
  /// single call, so a torn write is torn at the end and never in the middle of
  /// a field.
  std::vector<std::uint8_t> m_frame;
};

} // namespace Neuron
