#include "pch.h"
#include "CppUnitTest.h"

#include "DelayedTransport.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The latency shim (ADR-018 A15, R18).
 *
 * Every test here drives `Poll(double)` with a time it chose, so the suite
 * measures timing without waiting for it. That is deliberate: a delay shim
 * tested by sleeping would be slow in the ordinary case and flaky on a loaded
 * runner, which is the failure this repository has already paid for once in
 * `ACloseReasonCrossesTheWire`.
 */

namespace NeuronCoreTests
{
namespace
{

/*
 * A transport that delivers exactly what it is told to, when it is polled.
 *
 * A fake rather than a real QUIC pair, because the claim under test is *when*
 * an event comes out and nothing about sockets. Its payloads are deliberately
 * invalidated on every `Poll` -- the same contract the real one states -- so a
 * shim that forgot to copy them would fail here rather than in production.
 */
class ScriptedTransport final : public Transport
{
public:
  /// Queues a message to be handed over on the next Poll.
  void Deliver(ConnectionId _connection, std::initializer_list<std::uint8_t> _bytes)
  {
    TransportEvent event;
    event.type = TransportEvent::Type::Message;
    event.connection = _connection;
    event.channel = TransportChannel::State;
    m_pending.push_back(std::vector<std::uint8_t>{_bytes});
    m_pendingKinds.push_back(event);
  }

  void DeliverDisconnect(ConnectionId _connection, DisconnectReason _reason)
  {
    TransportEvent event;
    event.type = TransportEvent::Type::Disconnected;
    event.connection = _connection;
    event.reason = _reason;
    m_pending.push_back({});
    m_pendingKinds.push_back(event);
  }

  [[nodiscard]] bool Listen(std::uint16_t) override { return true; }
  [[nodiscard]] ConnectionId Connect(const std::string&, std::uint16_t) override { return 1; }

  void Poll() override
  {
    /*
     * The bytes the last Poll handed out die here, which is the real
     * transport's rule: *"valid until the next Poll()"*. A shim that kept the
     * span instead of copying reads freed memory in this test.
     */
    m_live.clear();
    while (!m_pending.empty())
    {
      m_live.push_back(std::move(m_pending.front()));
      m_liveKinds.push_back(m_pendingKinds.front());
      m_pending.pop_front();
      m_pendingKinds.pop_front();
    }
    m_drained = 0;
  }

  [[nodiscard]] bool NextEvent(TransportEvent& _outEvent) override
  {
    if (m_drained >= m_liveKinds.size())
    {
      m_liveKinds.clear();
      return false;
    }
    _outEvent = m_liveKinds[m_drained];
    _outEvent.payload = std::span<const std::uint8_t>{m_live[m_drained]};
    ++m_drained;
    return true;
  }

  [[nodiscard]] bool Send(ConnectionId, TransportChannel, std::span<const std::uint8_t> _payload) override
  {
    ++sentCount;
    sentBytes += _payload.size();
    return true;
  }

  void Close(ConnectionId, DisconnectReason) override { ++closeCount; }
  void Shutdown() override { ++shutdownCount; }
  [[nodiscard]] ConnectionState State(ConnectionId) const override { return ConnectionState::Connected; }
  [[nodiscard]] std::uint16_t BoundPort() const override { return 4242; }

  [[nodiscard]] TransportStats Stats(ConnectionId) const override
  {
    TransportStats stats;
    stats.minRoundTripMs = 0.4;
    stats.datagramsSent = sentCount;
    return stats;
  }

  std::uint64_t sentCount = 0;
  std::size_t sentBytes = 0;
  int closeCount = 0;
  int shutdownCount = 0;

private:
  std::deque<std::vector<std::uint8_t>> m_pending;
  std::deque<TransportEvent> m_pendingKinds;
  std::vector<std::vector<std::uint8_t>> m_live;
  std::vector<TransportEvent> m_liveKinds;
  std::size_t m_drained = 0;
};

/// Drains whatever the shim will give up right now.
[[nodiscard]] std::vector<std::uint8_t> DrainFirstBytes(DelayedTransport& _transport)
{
  TransportEvent event;
  if (!_transport.NextEvent(event))
  {
    return {};
  }
  return std::vector<std::uint8_t>{event.payload.begin(), event.payload.end()};
}

[[nodiscard]] std::size_t CountReady(DelayedTransport& _transport)
{
  std::size_t count = 0;
  TransportEvent event;
  while (_transport.NextEvent(event))
  {
    ++count;
  }
  return count;
}

} // namespace

TEST_CLASS(DelayedTransportTests)
{
public:
  /*
   * A note that applies to every test below: a size assertion is checked before
   * anything indexes past it.
   *
   * MSVC's `Assert` throws, so on the shipping harness a failed size stops the
   * test. Not every harness does -- one that records and continues would read
   * past the end and crash, turning a legible failure into a segfault. The
   * guard costs a branch and keeps the failure readable wherever it runs.
   */
  TEST_METHOD(AMessageWaitsItsDelayAndThenArrives)
  {
    /*
     * The whole of the shim in one assertion: nothing at 0 ms, nothing at
     * 49 ms, and the message at 50. A view-switch budget stated as RTT + settle
     * is only measurable if the RTT half is a number somebody chose.
     */
    ScriptedTransport inner;
    DelayedTransport link{inner, DelayedTransportTuning{50.0, 4096}};

    inner.Deliver(1, {7, 8, 9});
    link.Poll(0.0);
    Assert::AreEqual<std::size_t>(0, CountReady(link), L"nothing arrives in the frame it was sent");

    link.Poll(49.0);
    Assert::AreEqual<std::size_t>(0, CountReady(link), L"nor a millisecond early");

    link.Poll(50.0);
    const std::vector<std::uint8_t> bytes = DrainFirstBytes(link);
    Assert::AreEqual<std::size_t>(3, bytes.size(), L"and then it is there, whole");
    if (bytes.size() == 3)
    {
      Assert::AreEqual<std::uint8_t>(7, bytes[0]);
      Assert::AreEqual<std::uint8_t>(9, bytes[2]);
    }
  }

  TEST_METHOD(ThePayloadSurvivesTheTransportThatOwnedIt)
  {
    /*
     * The copy, and why it is not optional. `TransportEvent::payload` is valid
     * only until the next `Poll` -- so by the time a held event comes due, the
     * transport it came from has invalidated the bytes at least once. The
     * scripted transport clears them on every `Poll` precisely so a shim that
     * kept the span rather than copying fails here.
     */
    ScriptedTransport inner;
    DelayedTransport link{inner, DelayedTransportTuning{20.0, 4096}};

    inner.Deliver(1, {1, 2, 3, 4});
    link.Poll(0.0);
    for (double now = 1.0; now <= 20.0; now += 1.0)
    {
      link.Poll(now); // Each one invalidates the inner transport's buffers.
    }

    const std::vector<std::uint8_t> bytes = DrainFirstBytes(link);
    Assert::AreEqual<std::size_t>(4, bytes.size(), L"twenty polls later, the bytes are still the bytes");
    if (bytes.size() == 4)
    {
      Assert::AreEqual<std::uint8_t>(1, bytes[0]);
      Assert::AreEqual<std::uint8_t>(4, bytes[3]);
    }
  }

  TEST_METHOD(OrderIsKeptBecauseTheDelayIsConstant)
  {
    /*
     * Three messages sent across three frames come out in the order they went
     * in. This is the property that makes a constant delay safe and a jittered
     * one wrong: the transport promises a channel is ordered, and a shim that
     * reordered it would be measuring a different link from the one that ships.
     */
    ScriptedTransport inner;
    DelayedTransport link{inner, DelayedTransportTuning{30.0, 4096}};

    inner.Deliver(1, {10});
    link.Poll(0.0);
    inner.Deliver(1, {20});
    link.Poll(5.0);
    inner.Deliver(1, {30});
    link.Poll(10.0);

    link.Poll(40.0); // The first two are due; the third is not until 40.
    TransportEvent event;
    std::vector<std::uint8_t> seen;
    while (link.NextEvent(event))
    {
      // Guarded for the reason at the top of this class: an unexpectedly empty
      // payload should read as a failure rather than as a crash.
      seen.push_back(event.payload.empty() ? std::uint8_t{0} : event.payload[0]);
    }
    Assert::AreEqual<std::size_t>(3, seen.size(), L"all three are due by 40 ms");
    if (seen.size() == 3)
    {
      Assert::AreEqual<std::uint8_t>(10, seen[0]);
      Assert::AreEqual<std::uint8_t>(20, seen[1]);
      Assert::AreEqual<std::uint8_t>(30, seen[2]);
    }
  }

  TEST_METHOD(ZeroDelayIsAPassThrough)
  {
    // The control case, and it has to work in the *same* run that measures a
    // delayed one or the comparison is against a different code path.
    ScriptedTransport inner;
    DelayedTransport link{inner, DelayedTransportTuning{0.0, 4096}};

    inner.Deliver(1, {5, 6});
    link.Poll(0.0);
    const std::vector<std::uint8_t> bytes = DrainFirstBytes(link);
    Assert::AreEqual<std::size_t>(2, bytes.size(), L"no delay means the same frame");
  }

  TEST_METHOD(EverythingThatIsNotDeliveryGoesStraightThrough)
  {
    /*
     * Listening, connecting, sending, closing and the port are statements about
     * the local socket. A shim that delayed them would be modelling a machine
     * that is slow rather than a link that is long -- and `Send` in particular
     * must not be deferred, because its answer is about the socket now.
     */
    ScriptedTransport inner;
    DelayedTransport link{inner, DelayedTransportTuning{100.0, 4096}};

    const std::uint8_t payload[] = {1, 2, 3};
    Assert::IsTrue(link.Send(1, TransportChannel::State, payload), L"a send is answered now, not in 100 ms");
    Assert::AreEqual<std::uint64_t>(1, inner.sentCount, L"and it reached the real transport immediately");
    Assert::IsTrue(link.Listen(0));
    Assert::AreEqual<ConnectionId>(1, link.Connect("127.0.0.1", 1234));
    Assert::AreEqual<std::uint16_t>(4242, link.BoundPort());
    link.Close(1, DisconnectReason::ShuttingDown);
    Assert::AreEqual(1, inner.closeCount);
    link.Shutdown();
    Assert::AreEqual(1, inner.shutdownCount);
  }

  TEST_METHOD(TheStatsSayWhatTheShimAdded)
  {
    /*
     * A readout saying 0.4 ms while delivery visibly took 200 would be the shim
     * lying about itself, on the one number A15 exists to parameterise. The
     * round trip is *both* directions, because wrapping both ends of a pair is
     * how a real round trip is produced.
     */
    ScriptedTransport inner;
    DelayedTransport link{inner, DelayedTransportTuning{75.0, 4096}};

    Assert::AreEqual(150.0, link.InjectedRoundTripMs(), 0.0001, L"one way each way");
    Assert::AreEqual(150.4, link.Stats(1).minRoundTripMs, 0.0001, L"stated on top of what the link really costs");
    Assert::AreEqual<std::uint64_t>(0, link.Stats(1).datagramsSent, L"and everything else is the real transport's");
  }

  TEST_METHOD(AQueuePastItsBoundDropsTheOldestAndSaysSo)
  {
    /*
     * The bound exists so a shim cannot be made to buffer until the process
     * dies. Dropping the *oldest* is the deliberate half: a reader handed an
     * ancient event last would conclude the link was enormous rather than that
     * the shim ran out of room, so the newest survive and `OverflowCount` says
     * the measurement is void.
     */
    ScriptedTransport inner;
    DelayedTransport link{inner, DelayedTransportTuning{50.0, 2}};

    inner.Deliver(1, {1});
    inner.Deliver(1, {2});
    inner.Deliver(1, {3});
    link.Poll(0.0);
    Assert::AreEqual<std::uint64_t>(1, link.OverflowCount(), L"one too many, and it is counted");

    link.Poll(50.0);
    TransportEvent event;
    std::vector<std::uint8_t> seen;
    while (link.NextEvent(event))
    {
      // Guarded for the reason at the top of this class: an unexpectedly empty
      // payload should read as a failure rather than as a crash.
      seen.push_back(event.payload.empty() ? std::uint8_t{0} : event.payload[0]);
    }
    Assert::AreEqual<std::size_t>(2, seen.size());
    if (seen.size() == 2)
    {
      Assert::AreEqual<std::uint8_t>(2, seen[0], L"the oldest went, not the newest");
      Assert::AreEqual<std::uint8_t>(3, seen[1]);
    }
  }

  TEST_METHOD(ADisconnectIsDelayedLikeAnythingElse)
  {
    /*
     * A connection dropping is news that travels at the speed of the link, and
     * a shim that let it overtake the messages ahead of it would deliver a
     * goodbye before the sentence it interrupted.
     */
    ScriptedTransport inner;
    DelayedTransport link{inner, DelayedTransportTuning{25.0, 4096}};

    inner.Deliver(1, {9});
    inner.DeliverDisconnect(1, DisconnectReason::ShuttingDown);
    link.Poll(0.0);
    Assert::AreEqual<std::size_t>(0, CountReady(link));

    link.Poll(25.0);
    TransportEvent event;
    Assert::IsTrue(link.NextEvent(event));
    Assert::IsTrue(event.type == TransportEvent::Type::Message, L"the message first");
    Assert::IsTrue(link.NextEvent(event));
    Assert::IsTrue(event.type == TransportEvent::Type::Disconnected, L"and the goodbye after it");
    Assert::IsTrue(event.reason == DisconnectReason::ShuttingDown, L"carrying its reason");
  }

  TEST_METHOD(TheShimCanOwnTheTransportItWraps)
  {
    /*
     * The constructor `ClientConnection` needs: its link lives in a
     * `std::unique_ptr<Transport>` and there is nowhere to put a second object,
     * so the shim has to be able to *be* that pointer.
     *
     * The claim is that owning changes nothing about behaviour -- the same
     * delay, the same bytes -- and that the inner transport is still alive to
     * answer when the shim asks it something.
     */
    auto owned = std::make_unique<ScriptedTransport>();
    ScriptedTransport& watched = *owned;
    DelayedTransport link{std::move(owned), DelayedTransportTuning{15.0, 4096}};

    watched.Deliver(1, {42});
    link.Poll(0.0);
    Assert::AreEqual<std::size_t>(0, CountReady(link), L"held, exactly as when borrowed");

    link.Poll(15.0);
    const std::vector<std::uint8_t> bytes = DrainFirstBytes(link);
    Assert::AreEqual<std::size_t>(1, bytes.size());
    if (bytes.size() == 1)
    {
      Assert::AreEqual<std::uint8_t>(42, bytes[0]);
    }

    // And the owned transport is still there to be forwarded to.
    Assert::AreEqual<std::uint16_t>(4242, link.BoundPort(), L"the inner is alive and answering");
    link.Shutdown();
    Assert::AreEqual(1, watched.shutdownCount);
  }

  TEST_METHOD(ABudgetIsStatedAsRoundTripPlusSettle)
  {
    /*
     * A15's arithmetic, asserted so the number in the build order and the
     * number the shim produces cannot drift.
     *
     * U3b's acceptance was written as a flat *"under half a second"*, which is
     * only true at loopback RTT. The budget is `RTT + settle`: at 200 ms of
     * settle, a 40 ms link has 240 ms to beat and a 300 ms link has 500 -- so
     * the flat number was not a target, it was a target *and* an unstated
     * assumption about the network.
     */
    constexpr double SETTLE_MS = 200.0;
    ScriptedTransport inner;

    const DelayedTransport nearby{inner, DelayedTransportTuning{20.0, 4096}};
    Assert::AreEqual(240.0, nearby.InjectedRoundTripMs() + SETTLE_MS, 0.0001,
                     L"a 40 ms round trip still fits the old flat number");

    const DelayedTransport distant{inner, DelayedTransportTuning{150.0, 4096}};
    Assert::AreEqual(500.0, distant.InjectedRoundTripMs() + SETTLE_MS, 0.0001,
                     L"and a 300 ms one does not, which is the whole finding");
  }
};

} // namespace NeuronCoreTests
