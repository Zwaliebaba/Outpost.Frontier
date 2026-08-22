#pragma once

#include "EntityRecord.h"
#include "Wire.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

/*
 * The client's half of interest and delta (ADR-022 §2, §3).
 *
 * The mirror of `SnapshotSender`'s ring, and deliberately built the same way:
 * both sides keep a ring of **pictures**, keyed by tick, and a delta names the
 * one it was encoded against. When the two rings agree the arithmetic is
 * trivial; when they disagree the client simply cannot apply the delta and says
 * so by not acking, which costs one larger delta rather than a stall.
 *
 * **Engine-side on purpose.** None of this knows what a ship is. Reassembling a
 * multi-part tick, holding a baseline, deciding when a tick is whole and
 * therefore ackable -- all of it is the same in any game that replicates state
 * against acked baselines, which is what makes it NeuronClient's rather than
 * GameLogic's (ADR-014 §4). The game is handed the finished picture and the
 * opaque tail that came with it.
 *
 * Three rules from ADR-022 land here and are worth stating before the code:
 *
 * - **Apply each part on arrival; ack the tick only when every part has
 *   arrived** (§2d). Freshness and baselines are different jobs: a lost part
 *   costs staleness for the entities it carried, while a baseline both sides do
 *   not agree on costs correctness.
 * - **A part naming a baseline this client does not hold is refused, not
 *   misapplied** (§2b). Guessing would build a picture out of changes to a
 *   state that never existed here.
 * - **A keyframe replaces; it does not merge** (§3). It is the baseline, not an
 *   update to one.
 */

namespace Neuron
{

/*
 * One tick's replicated state, assembled and ready for the game.
 *
 * Spans into the receiver's own storage, valid until the next call that
 * modifies it -- the same borrow-and-drop rule the rest of this codebase uses
 * for scratch handed across a seam.
 */
struct ReplicatedFrame
{
  std::uint32_t tick = 0;
  std::uint16_t gridId = 0;

  /// How many entities on this grid are **not** in `entities` (ADR-022 §5d).
  /// Carried so culling can be stated to the player rather than being silent.
  std::uint16_t culledCount = 0;

  std::span<const EntityRecord> entities;

  /// The game's own bytes for this tick, opaque to this library (ADR-014 §5).
  /// Empty when the part that carried them has not arrived, which is a real
  /// case and not an error.
  std::span<const std::uint8_t> tail;
};

class DeltaReceiver
{
public:
  /// Forgets everything. A fresh connection, a deliberate resync, or a view
  /// switch: all three want the same thing, which is for the next keyframe to
  /// be believed and every stale baseline to be gone.
  void Reset() noexcept;

  /*
   * A `WireType::Snapshot` payload -- one part of one tick's delta.
   *
   * Returns true when the payload left the client with a picture worth drawing,
   * and fills `_outFrame` with it. False means the part was refused: malformed,
   * or naming a baseline this client does not hold. Refused parts leave the
   * receiver untouched.
   */
  [[nodiscard]] bool OnDelta(std::span<const std::uint8_t> _payload, ReplicatedFrame& _outFrame);

  /// A `WireType::Keyframe` payload. Replaces the picture wholesale and makes
  /// its tick the only baseline this client holds.
  [[nodiscard]] bool OnKeyframe(std::span<const std::uint8_t> _payload, ReplicatedFrame& _outFrame);

  /*
   * A tick has become whole since the last time this was asked (§2d).
   *
   * Returns true once per completed tick and fills the ack to send. Only the
   * newest is offered: acks are a high-water mark on the server, so a backlog
   * of them would be one useful message and several the server ignores.
   */
  [[nodiscard]] bool TakeAck(SnapshotAck& _outAck) noexcept;

  /// Which grid the receiver is holding, or zero before anything has arrived.
  [[nodiscard]] std::uint16_t Grid() const noexcept
  {
    return m_grid;
  }

  /// Parts that named a baseline this client does not hold. In steady state
  /// this is zero; a number that climbs means acks are not getting through, and
  /// the server should be answering with keyframes rather than deltas.
  [[nodiscard]] std::uint32_t OrphanedPartCount() const noexcept
  {
    return m_orphanedParts;
  }

  [[nodiscard]] std::uint32_t KeyframeCount() const noexcept
  {
    return m_keyframes;
  }

private:
  struct Picture
  {
    std::uint32_t tick = 0;
    bool used = false;
    std::vector<EntityRecord> records;
  };

  /// The slot a tick occupies, remembering which tick it holds so a wrap cannot
  /// be mistaken for a hit.
  [[nodiscard]] Picture* PictureAt(std::uint32_t _tick) noexcept;
  [[nodiscard]] const Picture* PictureAt(std::uint32_t _tick) const noexcept;

  /// Starts assembling `_tick` from `_baseline`, discarding whatever tick was
  /// being assembled before. A superseded tick is not an error: the newer one
  /// is what the player is owed.
  void BeginTick(std::uint32_t _tick, const std::vector<EntityRecord>& _baseline);

  std::array<Picture, BASELINE_RING_TICKS> m_ring{};

  std::uint16_t m_grid = 0;
  bool m_hasGrid = false;

  /// The tick currently being assembled, and which of its parts have landed.
  /// 256 bits because `partCount` is a byte -- a bound the format gives rather
  /// than one this class chose.
  std::uint32_t m_assemblingTick = 0;
  bool m_assembling = false;
  std::uint8_t m_expectedParts = 0;
  std::array<std::uint64_t, 4> m_partsSeen{};
  std::uint32_t m_partsCounted = 0;

  /// The game's bytes from part zero of the tick being assembled.
  std::vector<std::uint8_t> m_tail;

  std::uint16_t m_culledCount = 0;

  bool m_ackPending = false;
  SnapshotAck m_ack;

  std::uint32_t m_orphanedParts = 0;
  std::uint32_t m_keyframes = 0;
};

} // namespace Neuron
