#include "pch.h"

#include "DeltaReceiver.h"

#include "ByteReader.h"

#include <algorithm>

namespace Neuron
{

void DeltaReceiver::Reset() noexcept
{
  for (Picture& picture : m_ring)
  {
    picture.used = false;
    picture.tick = 0;
    picture.records.clear();
  }
  m_grid = 0;
  m_hasGrid = false;
  m_assembling = false;
  m_assemblingTick = 0;
  m_expectedParts = 0;
  m_partsSeen = {};
  m_partsCounted = 0;
  m_tail.clear();
  m_culledCount = 0;
  m_ackPending = false;
  m_ack = SnapshotAck{};
}

DeltaReceiver::Picture* DeltaReceiver::PictureAt(std::uint32_t _tick) noexcept
{
  Picture& picture = m_ring[_tick % BASELINE_RING_TICKS];
  return picture.used && picture.tick == _tick ? &picture : nullptr;
}

const DeltaReceiver::Picture* DeltaReceiver::PictureAt(std::uint32_t _tick) const noexcept
{
  const Picture& picture = m_ring[_tick % BASELINE_RING_TICKS];
  return picture.used && picture.tick == _tick ? &picture : nullptr;
}

void DeltaReceiver::BeginTick(std::uint32_t _tick, const std::vector<EntityRecord>& _baseline)
{
  Picture& picture = m_ring[_tick % BASELINE_RING_TICKS];
  // Copied before the slot is claimed, because the baseline may *be* this slot
  // when a tick wraps the ring onto itself -- assigning first would then be
  // clearing the thing being copied from.
  std::vector<EntityRecord> seed = _baseline;
  picture.records = std::move(seed);
  picture.tick = _tick;
  picture.used = true;

  m_assemblingTick = _tick;
  m_assembling = true;
  m_expectedParts = 0;
  m_partsSeen = {};
  m_partsCounted = 0;
  m_tail.clear();
  m_culledCount = 0;
}

bool DeltaReceiver::OnDelta(std::span<const std::uint8_t> _payload, ReplicatedFrame& _outFrame)
{
  ByteReader reader{_payload};
  DeltaHeader header;
  if (!Read(reader, header))
  {
    return false;
  }

  /*
   * A grid this receiver is not holding is a view switch, and a switch is a
   * mid-session join (ADR-022 §3a): the two grids share an id space without
   * sharing a ship, so every baseline here is worthless. The delta cannot be
   * applied and the server will answer the missing ack with a keyframe, which
   * is the one recovery path and needs no second mechanism.
   */
  if (!m_hasGrid || header.gridId != m_grid)
  {
    Reset();
    ++m_orphanedParts;
    return false;
  }

  const Picture* baseline = PictureAt(header.baselineTick);
  if (baseline == nullptr)
  {
    /*
     * **Refused, not misapplied** (§2b, and the accept says so in as many
     * words). A delta describes *changes* to a state; applied to a different
     * state it produces a picture that never existed on either side, and every
     * delta after it would compound the error silently. Not acking is how the
     * client asks for the keyframe that fixes it.
     */
    ++m_orphanedParts;
    return false;
  }

  /*
   * Start a fresh assembly whenever the tick moves, in either direction.
   *
   * Forward is the ordinary case. *Backward* happens when parts of two ticks
   * interleave on an unordered channel, and abandoning the newer for the older
   * would be drawing a state the server has already left -- so an older tick
   * whose picture is already in the ring is applied into that slot and does not
   * disturb the assembly.
   */
  if (!m_assembling || m_assemblingTick != header.tick)
  {
    if (header.tick < m_assemblingTick && m_assembling)
    {
      // Older than what is being assembled: its slot may exist from an earlier
      // part, and if it does not there is nothing useful to build -- the newer
      // tick supersedes it either way.
      Picture* existing = PictureAt(header.tick);
      if (existing == nullptr)
      {
        ++m_orphanedParts;
        return false;
      }
    }
    else
    {
      BeginTick(header.tick, baseline->records);
    }
  }

  Picture* target = PictureAt(header.tick);
  if (target == nullptr)
  {
    ++m_orphanedParts;
    return false;
  }

  // Read the whole part before touching the picture: a truncated part must
  // leave the view untouched, exactly as a truncated snapshot did.
  std::vector<EntityRecord> records;
  records.reserve(header.recordCount);
  for (std::uint16_t index = 0; index < header.recordCount; ++index)
  {
    records.push_back(ReadEntityRecord(reader));
  }

  std::vector<std::uint32_t> left;
  std::vector<std::uint8_t> tail;
  if (header.partIndex == 0)
  {
    // Part zero carries the two trailers (§5e and the game's tail). Their
    // absence is legal on a build that had nothing to say, so a reader that
    // runs out here is only an error if it ran out mid-field.
    const std::uint16_t leftCount = reader.ReadUInt16();
    if (reader.Ok())
    {
      left.reserve(leftCount);
      for (std::uint16_t index = 0; index < leftCount; ++index)
      {
        left.push_back(reader.ReadUInt32());
      }
      const std::uint16_t tailBytes = reader.ReadUInt16();
      if (reader.Ok())
      {
        tail.reserve(tailBytes);
        for (std::uint16_t index = 0; index < tailBytes; ++index)
        {
          tail.push_back(reader.ReadUInt8());
        }
      }
    }
  }

  if (!reader.Ok())
  {
    return false;
  }

  /*
   * Departures first, then changes.
   *
   * Order matters for the case where a ship leaves interest and another arrives
   * on the same tick: doing it the other way round would remove a record the
   * same part had just added if the two shared an id, which they can after a
   * despawn and a respawn.
   */
  if (!left.empty())
  {
    target->records.erase(std::remove_if(target->records.begin(), target->records.end(),
                                         [&left](const EntityRecord& _record) noexcept
                                         { return std::find(left.begin(), left.end(), _record.id) != left.end(); }),
                          target->records.end());
  }

  for (const EntityRecord& record : records)
  {
    const auto found = std::find_if(target->records.begin(), target->records.end(),
                                    [&record](const EntityRecord& _held) noexcept { return _held.id == record.id; });
    if (found == target->records.end())
    {
      target->records.push_back(record);
    }
    else
    {
      *found = record;
    }
  }

  if (header.tick == m_assemblingTick && m_assembling)
  {
    if (header.partIndex == 0)
    {
      m_tail = std::move(tail);
    }
    m_culledCount = header.culledCount;
    m_expectedParts = header.partCount;

    const std::size_t word = header.partIndex / 64u;
    const std::uint64_t bit = std::uint64_t{1} << (header.partIndex % 64u);
    if ((m_partsSeen[word] & bit) == 0)
    {
      m_partsSeen[word] |= bit;
      ++m_partsCounted;
    }

    /*
     * Ack only when the tick is whole (§2d).
     *
     * The parts have all been *applied* by now -- apply for freshness -- but
     * the ack is what makes this tick a baseline both sides agree on, and half
     * a tick is not a baseline.
     */
    if (m_partsCounted >= m_expectedParts)
    {
      m_ack = SnapshotAck{m_grid, header.tick};
      m_ackPending = true;
    }
  }

  _outFrame.tick = header.tick;
  _outFrame.gridId = header.gridId;
  _outFrame.culledCount = header.culledCount;
  _outFrame.entities = target->records;
  _outFrame.tail = header.tick == m_assemblingTick ? std::span<const std::uint8_t>{m_tail} : std::span<const std::uint8_t>{};
  return true;
}

bool DeltaReceiver::OnKeyframe(std::span<const std::uint8_t> _payload, ReplicatedFrame& _outFrame)
{
  ByteReader reader{_payload};
  KeyframeHeader header;
  if (!Read(reader, header))
  {
    return false;
  }

  std::vector<EntityRecord> records;
  records.reserve(header.recordCount);
  for (std::uint32_t index = 0; index < header.recordCount; ++index)
  {
    records.push_back(ReadEntityRecord(reader));
  }

  std::vector<std::uint8_t> tail;
  const std::uint16_t tailBytes = reader.ReadUInt16();
  if (reader.Ok())
  {
    tail.reserve(tailBytes);
    for (std::uint16_t index = 0; index < tailBytes; ++index)
    {
      tail.push_back(reader.ReadUInt8());
    }
  }

  if (!reader.Ok())
  {
    return false; // Truncated: leave whatever was held alone.
  }

  /*
   * **A keyframe replaces rather than merges** (§3), and it invalidates every
   * other baseline while it is at it. It *is* the baseline: an older picture
   * kept beside it could only be used to decode a delta the server will never
   * send, because the server sends a keyframe precisely when it has decided
   * that no older baseline is safe.
   */
  Reset();
  m_grid = header.gridId;
  m_hasGrid = true;

  Picture& picture = m_ring[header.tick % BASELINE_RING_TICKS];
  picture.tick = header.tick;
  picture.used = true;
  picture.records = std::move(records);

  m_assemblingTick = header.tick;
  m_assembling = true;
  m_expectedParts = 1;
  m_partsSeen[0] = 1;
  m_partsCounted = 1;
  m_tail = std::move(tail);
  m_culledCount = header.culledCount;

  // Whole by construction -- it arrived on a reliable stream -- so it is
  // ackable the moment it is read.
  m_ack = SnapshotAck{m_grid, header.tick};
  m_ackPending = true;
  ++m_keyframes;

  _outFrame.tick = header.tick;
  _outFrame.gridId = header.gridId;
  _outFrame.culledCount = header.culledCount;
  _outFrame.entities = picture.records;
  _outFrame.tail = m_tail;
  return true;
}

bool DeltaReceiver::TakeAck(SnapshotAck& _outAck) noexcept
{
  if (!m_ackPending)
  {
    return false;
  }
  m_ackPending = false;
  _outAck = m_ack;
  return true;
}

} // namespace Neuron
