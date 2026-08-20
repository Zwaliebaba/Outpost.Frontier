#include "pch.h"

#include "SnapshotSender.h"

#include "ByteWriter.h"
#include "Log.h"
#include "Telemetry.h"

namespace Neuron
{

SnapshotSendResult SnapshotSender::Send(Simulation& _simulation, Transport& _transport, std::uint32_t _tick)
{
  // Named for the work, not the client: the row is what a profile reads to see
  // that serialisation now scales with viewers rather than with ticks, which is
  // the cost this shape accepts on purpose.
  NEURON_SPAN("Snapshot");

  ByteWriter writer{m_buffer};
  WriteWireType(writer, WireType::Snapshot);

  if (!_simulation.WriteSnapshot(_tick, m_viewer, writer) || !writer.Ok())
  {
    /*
     * The simulation refused -- almost certainly because the grid outgrew one
     * datagram, which is the point at which ADR-004 §6's growth path stops
     * being optional and ADR-022 §6's priority truncation replaces this
     * refusal outright.
     *
     * Loud, counted, and **named**: it is one client's stream that stopped, and
     * on the day two clients view different grids the difference between "the
     * server broke" and "player 3's grid is over the cap" is the whole
     * diagnosis. Once per sender, because a message every tick at 20 Hz is a
     * log nobody can read.
     */
    ++m_refused;
    NEURON_COUNTER("SnapshotDropped", 1);
    if (m_refused == 1)
    {
      NEURON_LOG_ERROR("the simulation could not fit a snapshot for client %u (player %u) in one datagram; it will see nothing move",
                       m_clientId, m_viewer);
    }
    return SnapshotSendResult::Refused;
  }

  // The overflow guard `ServerHost::SendTo` applies to every other message, kept
  // here rather than borrowed: this send is the one that does not go through it,
  // because the writer it fills is this object's own.
  (void)_transport.Send(m_connection, TransportChannel::State, writer.Written());
  ++m_sent;
  NEURON_COUNTER("SnapshotsSent", 1);
  return SnapshotSendResult::Sent;
}

} // namespace Neuron
