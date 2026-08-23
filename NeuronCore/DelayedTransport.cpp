#include "pch.h"

#include "DelayedTransport.h"

namespace Neuron
{

void DelayedTransport::Poll()
{
  /*
   * The only place this class asks the time. `Clock::Counter` is the same
   * monotonic source the transport's own tests pump against, so a shimmed run
   * and an unshimmed one are measured off one clock.
   */
  Poll(Clock::MillisecondsBetween(0, Clock::Counter()));
}

void DelayedTransport::Poll(double _nowMs)
{
  m_inner.Poll();

  /*
   * Drain the wrapped transport completely before releasing anything.
   *
   * Its payloads are only valid until *its* next `Poll`, and that call already
   * happened above -- so every event it is holding has to be copied out now,
   * in this loop, before anything else can invalidate them.
   */
  TransportEvent event;
  while (m_inner.NextEvent(event))
  {
    if (m_held.size() >= m_tuning.maxHeldEvents)
    {
      /*
       * The oldest goes, not the newest.
       *
       * A queue past its bound is already a failed measurement, and dropping
       * the *newest* would additionally hide that the link had caught up --
       * the reader would see an ancient event last and conclude the delay was
       * enormous rather than that the shim ran out of room. `OverflowCount`
       * is what a run checks.
       */
      m_held.pop_front();
      ++m_overflowed;
    }

    HeldEvent& held = m_held.emplace_back();
    held.dueMs = _nowMs + m_tuning.oneWayMs;
    held.type = event.type;
    held.connection = event.connection;
    held.channel = event.channel;
    held.reason = event.reason;
    held.payload.assign(event.payload.begin(), event.payload.end());
  }

  /*
   * Then release what has come due, oldest first.
   *
   * `m_held` is sorted by `dueMs` for free, because the delay is constant and
   * everything is stamped on arrival -- so the front is always the soonest and
   * this stops at the first one that is not ready. A jittered delay would
   * break that and reorder a channel the transport promises is ordered, which
   * is the reason the header says constant and never jitter.
   */
  while (!m_held.empty() && m_held.front().dueMs <= _nowMs)
  {
    m_ready.push_back(std::move(m_held.front()));
    m_held.pop_front();
  }
}

bool DelayedTransport::NextEvent(TransportEvent& _outEvent)
{
  if (m_ready.empty())
  {
    return false;
  }

  /*
   * The bytes move into `m_delivered` and stay there until the next call,
   * which is `TransportEvent::payload`'s stated contract -- *"valid until the
   * next Poll()"* -- honoured exactly rather than widened. A caller that keeps
   * the span past that was already wrong against the real transport.
   */
  m_delivered = std::move(m_ready.front().payload);
  _outEvent.type = m_ready.front().type;
  _outEvent.connection = m_ready.front().connection;
  _outEvent.channel = m_ready.front().channel;
  _outEvent.reason = m_ready.front().reason;
  _outEvent.payload = std::span<const std::uint8_t>{m_delivered};
  m_ready.pop_front();
  return true;
}

TransportStats DelayedTransport::Stats(ConnectionId _connection) const
{
  TransportStats stats = m_inner.Stats(_connection);
  stats.minRoundTripMs += InjectedRoundTripMs();
  return stats;
}

} // namespace Neuron
