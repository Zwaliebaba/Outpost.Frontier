#pragma once

#include "Clock.h"
#include "Transport.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

/*
 * A transport with latency added, for measuring what latency costs
 * (ADR-018 A15, risk register R18).
 *
 * **Why this exists.** U3b's acceptance is *"roster click switches view to
 * smooth motion in under half a second"*, and A15's finding is that the number
 * is not a constant: a view switch is a request, an answer, and a designed
 * settle over the interpolation refill, so its budget is **RTT + settle** and a
 * loopback measurement -- where RTT is a tenth of a millisecond -- proves the
 * settle and nothing else. `Transport.h` had no latency hook of any kind, so
 * the shim was unbuilt work rather than a switch to flip. This is that hook.
 *
 * **A decorator rather than a mode on `QuicTransport`.** The shipping transport
 * does not learn that latency injection exists: it is wrapped, not modified, so
 * there is no branch in the real send path and no configuration that could
 * silently ship enabled. Anything that takes a `Transport&` takes this.
 *
 * **Delay lands on delivery, not on send**, which is both simpler and truer.
 * `Send` returning "accepted" is a statement about the local socket, and
 * deferring it would make that statement a guess about the future. What a real
 * link actually costs the caller is *when the bytes arrive*, so that is what
 * moves. Wrapping **both ends** of a loopback pair then produces a genuine
 * round trip -- the client's request reaches the server a delay late, and the
 * server's answer reaches the client a delay late -- so `RTT = 2 x oneWayMs`
 * and the two halves are the two directions rather than an arithmetic fiction.
 *
 * **Constant delay and never jitter.** Held events are released in the order
 * they arrived, which is only FIFO-safe while every event waits the same time;
 * a jittered shim would reorder a channel the real transport guarantees is
 * ordered, and would be measuring a different thing. Reordering and loss are
 * their own experiment with their own acceptance, and this is not it.
 *
 * **It reads a clock, which is exactly why it is not on the shipping path.**
 * A delay is a wall-clock fact by construction. `Poll(double)` takes the time
 * from the caller so a test can drive it without sleeping; the interface's
 * `Poll()` reads the real clock, and nothing else in this class asks the time.
 */

namespace Neuron
{

/*
 * How long one direction takes, and what that costs to hold.
 *
 * A payload is *copied* on the way in, because `TransportEvent::payload` is
 * only valid until the next `Poll()` and this class exists to outlive exactly
 * that. `maxHeldEvents` is the bound on how much a slow reader can be made to
 * buffer -- past it the oldest is dropped and counted, which is what a real
 * link does to a queue that will not drain and is better than growing until the
 * process dies.
 */
struct DelayedTransportTuning
{
  /// One direction, in milliseconds. Zero is a pass-through -- useful as the
  /// control case in the same run that measures a delayed one.
  double oneWayMs = 0.0;

  /// The held-event bound. At `MAX_DATAGRAM_BYTES` each this is a few
  /// megabytes, which is a development tool's worth of memory and not a
  /// server's.
  std::size_t maxHeldEvents = 4096;
};

/*
 * The wrapper.
 *
 * Borrows the inner transport or takes it over, and the two constructors are
 * for two real callers: a test that already holds a fake on the stack, and a
 * `ClientConnection` whose link lives in a `std::unique_ptr<Transport>` with
 * nowhere to put a second object. Borrowing is the default reading -- the inner
 * outlives this by contract, the way a `WorldView&` outlives the `ClientApp`
 * that reads it -- and owning is the same arrangement with the contract met by
 * holding the thing.
 */
class DelayedTransport final : public Transport
{
public:
  /// Wraps a transport somebody else owns, which must outlive this.
  DelayedTransport(Transport& _inner, const DelayedTransportTuning& _tuning) noexcept
    : m_inner(_inner)
    , m_tuning(_tuning)
  {
  }

  /*
   * Wraps a transport and takes it over, for the caller that holds its link in
   * a `std::unique_ptr<Transport>` and has nowhere to put a second object.
   *
   * The reference and the owner both point at the same thing; the owner is
   * declared *after* the reference so destruction runs the other way round,
   * which is the ordering that makes the reference safe for this object's whole
   * life. A caller using the borrowing constructor leaves the owner null and
   * nothing changes.
   */
  DelayedTransport(std::unique_ptr<Transport> _owned, const DelayedTransportTuning& _tuning) noexcept
    : m_inner(*_owned)
    , m_tuning(_tuning)
    , m_owned(std::move(_owned))
  {
  }

  /*
   * Everything that is not delivery is forwarded untouched.
   *
   * Listening, connecting, sending, closing and the port are all statements
   * about the local socket, and a shim that delayed them would be modelling a
   * machine that is slow rather than a link that is long.
   */
  [[nodiscard]] bool Listen(std::uint16_t _port) override { return m_inner.Listen(_port); }
  [[nodiscard]] ConnectionId Connect(const std::string& _host, std::uint16_t _port) override
  {
    return m_inner.Connect(_host, _port);
  }
  [[nodiscard]] bool Send(ConnectionId _connection, TransportChannel _channel,
                          std::span<const std::uint8_t> _payload) override
  {
    return m_inner.Send(_connection, _channel, _payload);
  }
  void Close(ConnectionId _connection, DisconnectReason _reason) override { m_inner.Close(_connection, _reason); }
  void Shutdown() override { m_inner.Shutdown(); }
  [[nodiscard]] ConnectionState State(ConnectionId _connection) const override { return m_inner.State(_connection); }
  [[nodiscard]] std::uint16_t BoundPort() const override { return m_inner.BoundPort(); }

  /*
   * The link's cost as the caller experiences it, which is the wrapped
   * transport's plus what this added.
   *
   * Stated rather than passed through untouched: a readout saying 0.1 ms while
   * delivery visibly took 200 would be the shim lying about itself, on the one
   * number A15 exists to parameterise.
   */
  [[nodiscard]] TransportStats Stats(ConnectionId _connection) const override;

  /// Drains the wrapped transport and releases whatever has waited long enough.
  void Poll() override;

  /*
   * The same, with the time supplied.
   *
   * A test drives this directly and never sleeps: the whole point of a delay
   * shim is timing, and a suite that measured timing by waiting for it would be
   * slow in the ordinary case and flaky on a loaded runner -- which is the
   * failure this repository has already paid for once, in
   * `ACloseReasonCrossesTheWire`.
   */
  void Poll(double _nowMs);

  [[nodiscard]] bool NextEvent(TransportEvent& _outEvent) override;

  /// How many held events were dropped for want of room. Zero in every run that
  /// means anything; non-zero says the measurement was taken against a queue
  /// that overflowed rather than against a link that was long.
  [[nodiscard]] std::uint64_t OverflowCount() const noexcept { return m_overflowed; }

  /// What this is adding, as a round trip: both directions of a wrapped pair.
  [[nodiscard]] double InjectedRoundTripMs() const noexcept { return m_tuning.oneWayMs * 2.0; }

private:
  /// One event, with its bytes copied and the time it becomes deliverable.
  struct HeldEvent
  {
    double dueMs = 0.0;
    TransportEvent::Type type = TransportEvent::Type::None;
    ConnectionId connection = INVALID_CONNECTION;
    TransportChannel channel = TransportChannel::Control;
    DisconnectReason reason = DisconnectReason::None;
    std::vector<std::uint8_t> payload;
  };

  Transport& m_inner;
  DelayedTransportTuning m_tuning;

  /// Null when the inner transport is borrowed. Declared after `m_inner` so it
  /// is destroyed after the reference stops being used, never before.
  std::unique_ptr<Transport> m_owned;

  /*
   * Two queues, and the split is what makes `NextEvent` cheap.
   *
   * `m_held` is waiting and `m_ready` has come due. `Poll` moves across the
   * boundary once per event; the alternative -- one queue that `NextEvent`
   * re-checks the clock against -- would put a time read in the drain loop and
   * make "what has arrived" depend on how long the caller took to ask.
   */
  std::deque<HeldEvent> m_held;
  std::deque<HeldEvent> m_ready;

  /// The bytes the last `NextEvent` handed out, kept alive until the one after
  /// it -- which is `TransportEvent::payload`'s own contract, honoured rather
  /// than widened.
  std::vector<std::uint8_t> m_delivered;

  std::uint64_t m_overflowed = 0;
};

} // namespace Neuron
