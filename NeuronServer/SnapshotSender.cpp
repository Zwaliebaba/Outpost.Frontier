#include "pch.h"

#include "SnapshotSender.h"

#include "ByteWriter.h"
#include "Log.h"
#include "Simulation.h"
#include "Telemetry.h"

#include <algorithm>

namespace Neuron
{
namespace
{

/// What one part of a delta has left for records after its own framing.
constexpr std::size_t PART_PAYLOAD_BYTES = MAX_DATAGRAM_BYTES - sizeof(std::uint16_t) - DELTA_HEADER_BYTES;

/// A part carries at most this many records. `partCount` is a byte, so 255
/// parts is the ceiling on a tick -- about twelve thousand records, an order of
/// magnitude past ADR-018 D4's cap and therefore not a limit anything reaches.
constexpr std::size_t RECORDS_PER_FULL_PART = PART_PAYLOAD_BYTES / ENTITY_RECORD_BYTES;

/// `[u16 leftCount]` and `[u16 tailBytes]`, the two trailers part zero carries.
constexpr std::size_t TRAILER_COUNT_BYTES = sizeof(std::uint16_t);

/// Finds a record by id in a picture. Linear on purpose: pictures are built in
/// ranked order and compared against the previous tick's, so the ids line up and
/// the hint hits almost every time. A map would cost an allocation per tick to
/// avoid a scan that rarely happens.
[[nodiscard]] const EntityRecord* FindRecord(const std::vector<EntityRecord>& _records, std::uint32_t _id, std::size_t _hint) noexcept
{
  if (_hint < _records.size() && _records[_hint].id == _id)
  {
    return &_records[_hint];
  }
  for (const EntityRecord& record : _records)
  {
    if (record.id == _id)
    {
      return &record;
    }
  }
  return nullptr;
}

} // namespace

void SnapshotSender::NoteAck(const SnapshotAck& _ack) noexcept
{
  if (_ack.gridId != m_grid)
  {
    // A view switch invalidates every baseline, and an ack in flight across one
    // would otherwise resurrect a picture of a world the viewer has left.
    return;
  }
  if (!m_hasAck || _ack.tick > m_ackedTick)
  {
    m_ackedTick = _ack.tick;
    m_hasAck = true;
  }
}

void SnapshotSender::NoteFocus(const ViewFocus& _focus)
{
  m_focus = _focus;
  m_selection = _focus.selection;
}

void SnapshotSender::DropBaselines() noexcept
{
  for (SentView& view : m_ring)
  {
    view.used = false;
    view.tick = 0;
    view.records.clear();
  }
  m_picture.clear();
  m_pendingLeft.clear();
  m_hasAck = false;
  m_ackedTick = 0;
}

const std::vector<EntityRecord>* SnapshotSender::BaselineAt(std::uint32_t _tick) const noexcept
{
  const SentView& view = m_ring[_tick % BASELINE_RING_TICKS];
  return view.used && view.tick == _tick ? &view.records : nullptr;
}

void SnapshotSender::BuildInterest(Simulation& _simulation, std::uint32_t _tick)
{
  InterestQuery query;
  query.viewer = m_viewer;
  query.grid = m_grid;
  query.tick = _tick;

  /*
   * The camera, but only while it describes *this* grid.
   *
   * A focus naming another grid is a client mid-switch, and ranking against it
   * would put the ships nearest a camera that is not here at the front. An
   * undescribed camera is not an error either: it ranks everything unselected
   * into the far tier, which is what a client that has not spoken yet deserves
   * and is never wrong, only unhelpful.
   */
  if (m_focus.gridId == m_grid)
  {
    query.selection = m_selection;
    query.focusXMetres = CentimetresToMetres(m_focus.focusXCm);
    query.focusYMetres = CentimetresToMetres(m_focus.focusYCm);
    query.viewHalfExtentMetres = static_cast<float>(m_focus.halfExtentCm) * 0.01f;
  }

  const std::uint32_t guaranteed = _simulation.RankRelevance(query, m_ranked);

  /*
   * Truncate by priority, never refuse (ADR-022 §6).
   *
   * The budget is a byte figure over records, and the guaranteed prefix is
   * exempt from it (§5c): when the viewer's own fleet alone costs more than the
   * budget, the fleet goes out and the overrun is counted. Culling a player's
   * own ships to hit a bandwidth number is the one outcome this design exists
   * to prevent, so the number that loses is the budget.
   */
  const std::size_t affordable = m_tickBudgetBytes / ENTITY_RECORD_BYTES;
  std::size_t chosen = std::min(m_ranked.size(), affordable);
  if (guaranteed > chosen)
  {
    chosen = std::min<std::size_t>(guaranteed, m_ranked.size());
    ++m_interestOverrun;
    NEURON_COUNTER("InterestOverrun", 1);
  }

  _simulation.WriteEntities(SnapshotRequest{m_viewer, m_grid, _tick, m_lastOrderSeqProcessed},
                            std::span<const std::uint32_t>{m_ranked.data(), chosen}, m_records);
}

bool SnapshotSender::Send(Simulation& _simulation, Transport& _transport, std::uint32_t _tick)
{
  BuildInterest(_simulation, _tick);

  /*
   * The game's tail, asked for once and reused by whichever path sends
   * (ADR-022 §3b). It rides in part zero, or in the keyframe, and never in
   * both -- one tick, one tail.
   */
  std::array<std::uint8_t, MAX_DATAGRAM_BYTES> tailBuffer{};
  ByteWriter tailWriter{tailBuffer};
  const bool hasTail = _simulation.WriteTickTail(SnapshotRequest{m_viewer, m_grid, _tick, m_lastOrderSeqProcessed}, tailWriter) &&
                       tailWriter.Ok();
  const std::span<const std::uint8_t> tail = hasTail ? tailWriter.Written() : std::span<const std::uint8_t>{};

  /*
   * **No ack in the ring means a keyframe** (§2c), unconditionally.
   *
   * A join, a view switch, a long stall and a lost run of acks all arrive here,
   * which is the point: one recovery path, exercised on every switch rather
   * than only on the rare true join, and no partial-resync mode to get subtly
   * wrong.
   */
  const std::vector<EntityRecord>* baseline = m_hasAck ? BaselineAt(m_ackedTick) : nullptr;
  if (baseline == nullptr)
  {
    SendKeyframe(_simulation, _transport, _tick, tail);
  }
  else
  {
    /*
     * What changed since the view the client acked, and what has left.
     *
     * `m_left` is ids the baseline holds that are **not on the grid at all any
     * more** -- and specifically not ids that merely did not fit this tick's
     * budget. A record absent from a delta means *unchanged* (§5e), so a
     * truncated ship keeps its place in the client's picture and its last known
     * state; naming it as departed would blink a hull the player can still see.
     */
    m_changed.clear();
    for (std::size_t index = 0; index < m_records.size(); ++index)
    {
      const EntityRecord* was = FindRecord(*baseline, m_records[index].id, index);
      if (was == nullptr || !SameEntityState(*was, m_records[index]))
      {
        m_changed.push_back(m_records[index]);
      }
    }

    m_left.clear();
    for (const EntityRecord& held : *baseline)
    {
      if (std::find(m_ranked.begin(), m_ranked.end(), held.id) == m_ranked.end())
      {
        m_left.push_back(held.id);
      }
    }

    /*
     * The picture the client will hold after this tick: the baseline, minus
     * what left, updated with what changed. This is what goes in the ring -- the
     * view **as sent**, which is the only thing a later delta may be encoded
     * against.
     */
    m_picture = *baseline;
    if (!m_left.empty())
    {
      m_picture.erase(std::remove_if(m_picture.begin(), m_picture.end(),
                                     [this](const EntityRecord& _record) noexcept
                                     { return std::find(m_left.begin(), m_left.end(), _record.id) != m_left.end(); }),
                      m_picture.end());
    }
    for (const EntityRecord& record : m_changed)
    {
      const auto found = std::find_if(m_picture.begin(), m_picture.end(),
                                      [&record](const EntityRecord& _held) noexcept { return _held.id == record.id; });
      if (found == m_picture.end())
      {
        m_picture.push_back(record);
      }
      else
      {
        *found = record;
      }
    }

    m_pendingLeft.insert(m_pendingLeft.end(), m_left.begin(), m_left.end());
    SendDelta(_transport, _tick, m_ackedTick, tail);
  }

  // The ring records the view as sent, whichever path sent it.
  SentView& slot = m_ring[_tick % BASELINE_RING_TICKS];
  slot.tick = _tick;
  slot.used = true;
  slot.records = m_picture;

  ++m_sent;

  // After the update, never before it: the summary is the slow feed, and a
  // tick's freshness is worth more than a roster that is about to be sent again.
  SendSummaries(_simulation, _transport, _tick);
  return true;
}

void SnapshotSender::SendKeyframe(Simulation& _simulation, Transport& _transport, std::uint32_t _tick,
                                  std::span<const std::uint8_t> _tail)
{
  (void)_simulation;

  /*
   * A keyframe carries as much of the ranked grid as the `Bulk` cap allows,
   * rather than as much as the per-tick budget allows.
   *
   * The budget is a *bandwidth* number for the steady state; a keyframe is the
   * baseline every one of those ticks is measured against, and a deliberately
   * thin one would make the deltas after it fatter for as long as the client
   * stayed connected. `Bulk` exists precisely so this message does not have to
   * be datagram-shaped (§3c).
   */
  const std::size_t reserved = KEYFRAME_HEADER_BYTES + TRAILER_COUNT_BYTES + _tail.size();
  const std::size_t affordable = (MAX_BULK_BYTES - reserved) / ENTITY_RECORD_BYTES;
  const std::size_t count = std::min(m_records.size(), affordable);

  m_picture.assign(m_records.begin(), m_records.begin() + static_cast<std::ptrdiff_t>(count));

  // Everything the client held is gone with the keyframe, so nothing is owed a
  // departure notice: the keyframe *is* the notice.
  m_pendingLeft.clear();

  const auto culled = static_cast<std::uint16_t>(
    std::min<std::size_t>(m_ranked.size() - std::min(m_ranked.size(), m_picture.size()), 0xffffu));
  m_lastCulledCount = culled;

  m_bulkBuffer.assign(reserved + count * ENTITY_RECORD_BYTES + sizeof(std::uint16_t), 0);
  ByteWriter writer{m_bulkBuffer};
  WriteWireType(writer, WireType::Keyframe);
  Write(writer, KeyframeHeader{_tick, m_grid, culled, static_cast<std::uint32_t>(count)});
  for (const EntityRecord& record : m_picture)
  {
    WriteEntityRecord(writer, record);
  }
  writer.WriteUInt16(static_cast<std::uint16_t>(_tail.size()));
  for (const std::uint8_t byte : _tail)
  {
    writer.WriteUInt8(byte);
  }

  if (!writer.Ok())
  {
    // The buffer is sized from the same numbers the writer spends, so this is a
    // sizing bug rather than a runtime condition. Loud, and no partial send.
    NEURON_LOG_ERROR("player %u: keyframe for tick %u would not fit its own buffer", m_viewer, _tick);
    return;
  }

  (void)_transport.Send(m_connection, TransportChannel::Bulk, writer.Written());
  ++m_keyframesSent;
  NEURON_COUNTER("KeyframesSent", 1);
}

void SnapshotSender::SendDelta(Transport& _transport, std::uint32_t _tick, std::uint32_t _baselineTick,
                               std::span<const std::uint8_t> _tail)
{
  const auto culled = static_cast<std::uint16_t>(
    std::min<std::size_t>(m_ranked.size() - std::min(m_ranked.size(), m_picture.size()), 0xffffu));
  m_lastCulledCount = culled;

  /*
   * Part zero carries the two trailers, so it has fewer records than the rest.
   *
   * The tail is reserved *first* and the departures take what is left, because
   * a ghost that never gets its departure notice is recoverable on a later tick
   * (`m_pendingLeft` carries it) while an order record dropped for a tick is a
   * ghost with no ETA on the frame the player is watching.
   */
  const std::size_t tailBytes = TRAILER_COUNT_BYTES + _tail.size();
  const std::size_t leftRoom = PART_PAYLOAD_BYTES > tailBytes + TRAILER_COUNT_BYTES
                                 ? (PART_PAYLOAD_BYTES - tailBytes - TRAILER_COUNT_BYTES) / sizeof(std::uint32_t)
                                 : 0;
  const std::size_t leftCount = std::min(m_pendingLeft.size(), leftRoom);
  const std::size_t trailerBytes = tailBytes + TRAILER_COUNT_BYTES + leftCount * sizeof(std::uint32_t);

  const std::size_t firstPartRecords =
    PART_PAYLOAD_BYTES > trailerBytes ? (PART_PAYLOAD_BYTES - trailerBytes) / ENTITY_RECORD_BYTES : 0;

  std::size_t partCount = 1;
  if (m_changed.size() > firstPartRecords)
  {
    const std::size_t remainder = m_changed.size() - firstPartRecords;
    partCount += (remainder + RECORDS_PER_FULL_PART - 1) / RECORDS_PER_FULL_PART;
  }
  if (partCount > 255)
  {
    // `partCount` is a byte (ADR-022 §3b) and this is far past ADR-018 D4's
    // cap, so reaching it means the budget was configured absurdly high. The
    // tick is trimmed rather than the count wrapping, which would make every
    // part claim to be part of a message that does not exist.
    partCount = 255;
  }

  std::size_t at = 0;
  for (std::size_t part = 0; part < partCount; ++part)
  {
    const std::size_t capacity = part == 0 ? firstPartRecords : RECORDS_PER_FULL_PART;
    const std::size_t take = std::min(capacity, m_changed.size() - at);

    ByteWriter writer{m_buffer};
    WriteWireType(writer, WireType::Snapshot);
    DeltaHeader header;
    header.tick = _tick;
    header.baselineTick = _baselineTick;
    header.gridId = m_grid;
    header.culledCount = culled;
    header.partIndex = static_cast<std::uint8_t>(part);
    header.partCount = static_cast<std::uint8_t>(partCount);
    header.recordCount = static_cast<std::uint16_t>(take);
    Write(writer, header);

    for (std::size_t index = 0; index < take; ++index)
    {
      WriteEntityRecord(writer, m_changed[at + index]);
    }
    at += take;

    if (part == 0)
    {
      writer.WriteUInt16(static_cast<std::uint16_t>(leftCount));
      for (std::size_t index = 0; index < leftCount; ++index)
      {
        writer.WriteUInt32(m_pendingLeft[index]);
      }
      writer.WriteUInt16(static_cast<std::uint16_t>(_tail.size()));
      for (const std::uint8_t byte : _tail)
      {
        writer.WriteUInt8(byte);
      }
    }

    if (!writer.Ok())
    {
      NEURON_LOG_ERROR("player %u: delta part %u for tick %u overran its datagram", m_viewer, static_cast<unsigned>(part), _tick);
      return;
    }

    // Unreliable and unordered on purpose: each part names its own tick, grid
    // and baseline, so a lost one costs freshness for the entities it carried
    // and nothing else -- and the client simply does not ack the tick
    // (ADR-022 §2d), which costs one larger delta rather than a stall.
    (void)_transport.Send(m_connection, TransportChannel::State, writer.Written());
  }

  m_pendingLeft.erase(m_pendingLeft.begin(), m_pendingLeft.begin() + static_cast<std::ptrdiff_t>(leftCount));
}

bool SnapshotSender::RequestView(Simulation& _simulation, std::uint16_t _grid, std::uint16_t& _outReasonCode)
{
  _outReasonCode = _simulation.MayView(m_viewer, _grid);
  if (_outReasonCode != 0)
  {
    return false; // The feed stays exactly where it was.
  }
  if (_grid != m_grid)
  {
    m_grid = _grid;
    // A switch is a mid-session join (ADR-022 §3a): the two grids share an id
    // space without sharing a ship, so every baseline is worthless and the next
    // tick takes the keyframe path.
    DropBaselines();
  }
  return true;
}

void SnapshotSender::SendSummaries(Simulation& _simulation, Transport& _transport, std::uint32_t _tick)
{
  if ((_tick + m_viewer) % SUMMARY_INTERVAL_TICKS != 0)
  {
    return;
  }

  ByteWriter writer{m_buffer};
  WriteWireType(writer, WireType::Summary);

  // Nothing to say is the common answer and is not a failure: a commander with
  // no docked ships and no fleets elsewhere is owed no frame, and sending an
  // empty one every second would be a message whose only content is that there
  // is none.
  if (!_simulation.WriteSummaries(m_viewer, _tick, writer) || !writer.Ok())
  {
    return;
  }

  (void)_transport.Send(m_connection, TransportChannel::State, writer.Written());
  ++m_summariesSent;
  NEURON_COUNTER("SummariesSent", 1);
}

} // namespace Neuron
