#include "pch.h"

#include "SnapshotSender.h"

#include "ByteWriter.h"
#include "Log.h"
#include "Simulation.h"
#include "Telemetry.h"

namespace Neuron
{

bool SnapshotSender::Send(Simulation& _simulation, Transport& _transport, std::uint32_t _tick)
{
  ByteWriter writer{m_buffer};
  WriteWireType(writer, WireType::Snapshot);

  // The session's facts travel in; nothing about this viewer is stored on a
  // world (ADR-022 §1).
  const SnapshotRequest request{m_viewer, m_grid, _tick, m_lastOrderSeqProcessed};
  if (!_simulation.WriteSnapshot(request, writer) || !writer.Ok())
  {
    ++m_overCap;
    NEURON_COUNTER("SnapshotDropped", 1);
    if (!m_overCapLogged)
    {
      m_overCapLogged = true;
      NEURON_LOG_ERROR("player %u: the simulation could not fit a snapshot in one datagram; this client will see nothing move", m_viewer);
    }
    return false;
  }

  // Unreliable and unordered on purpose: full snapshots are idempotent, so a
  // lost one costs a tick of freshness and a resent one would arrive after the
  // snapshot that superseded it (ADR-004 §6).
  (void)_transport.Send(m_connection, TransportChannel::State, writer.Written());
  ++m_sent;

  // After the snapshot, never before it: the summary is the slow feed, and a
  // tick's freshness is worth more than a roster that is about to be sent
  // again anyway.
  SendSummaries(_simulation, _transport, _tick);
  return true;
}

bool SnapshotSender::RequestView(Simulation& _simulation, std::uint16_t _grid, std::uint16_t& _outReasonCode)
{
  _outReasonCode = _simulation.MayView(m_viewer, _grid);
  if (_outReasonCode != 0)
  {
    return false; // The feed stays exactly where it was.
  }
  m_grid = _grid;
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
