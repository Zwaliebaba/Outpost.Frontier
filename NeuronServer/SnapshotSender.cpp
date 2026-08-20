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

  if (!_simulation.WriteSnapshot(m_viewer, _tick, writer) || !writer.Ok())
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
  return true;
}

} // namespace Neuron
