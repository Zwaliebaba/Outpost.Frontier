#include "pch.h"
#include "CppUnitTest.h"

#include "ByteReader.h"
#include "EntityRecord.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "ServerConfig.h"
#include "ServerHost.h"
#include "Simulation.h"
#include "QuicTransport.h"
#include "Wire.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The server is driven here through NeuronCore alone -- a raw transport and the
 * semantics-free wire messages. No GameLogic and no NeuronClient appear, which
 * is the point: if these tests can join a session, the engine really is
 * game-agnostic (ADR-014).
 */

namespace NeuronServerTests
{
namespace
{

/*
 * What a client makes of one state message (ADR-022 §2, §3).
 *
 * A test at this level drives a raw transport, so it has to do the two things a
 * real client's `DeltaReceiver` does: read whichever form arrived, and **ack
 * the tick**. The ack is not optional decoration -- without one the server has
 * no baseline for this viewer and answers every tick with a keyframe (§2c), so
 * a suite that never acked would test one code path forever and never see a
 * delta at all.
 *
 * The tail is the game's opaque bytes, which `CountingSimulation` fills with a
 * marker, the viewer and the tick's high-water mark -- so the assertions below
 * read the same fields they always did, one layer further in.
 */
struct FrameFacts
{
  bool ok = false;
  bool keyframe = false;
  std::uint32_t tick = 0;
  std::uint16_t gridId = 0;
  std::uint16_t culledCount = 0;
  std::uint32_t recordCount = 0;
  std::uint8_t partIndex = 0;
  std::uint8_t partCount = 1;

  /// The game's tail, decoded. `hasTail` is false on the parts that do not
  /// carry one, which is every part but the first.
  bool hasTail = false;
  std::uint32_t tailTick = 0;
  std::uint32_t marker = 0;
  std::uint32_t sequence = 0;
  std::uint32_t viewer = 0;
  std::uint32_t grid = 0;
};

[[nodiscard]] FrameFacts ReadFrame(WireType _type, ByteReader& _reader)
{
  FrameFacts facts;
  std::vector<std::uint8_t> tail;

  if (_type == WireType::Keyframe)
  {
    KeyframeHeader header;
    if (!Read(_reader, header))
    {
      return facts;
    }
    facts.keyframe = true;
    facts.tick = header.tick;
    facts.gridId = header.gridId;
    facts.culledCount = header.culledCount;
    facts.recordCount = header.recordCount;
    for (std::uint32_t index = 0; index < header.recordCount; ++index)
    {
      (void)ReadEntityRecord(_reader);
    }
  }
  else
  {
    DeltaHeader header;
    if (!Read(_reader, header))
    {
      return facts;
    }
    facts.tick = header.tick;
    facts.gridId = header.gridId;
    facts.culledCount = header.culledCount;
    facts.recordCount = header.recordCount;
    facts.partIndex = header.partIndex;
    facts.partCount = header.partCount;
    for (std::uint16_t index = 0; index < header.recordCount; ++index)
    {
      (void)ReadEntityRecord(_reader);
    }
    if (header.partIndex == 0)
    {
      // The departures trailer, which this suite does not otherwise care about.
      const std::uint16_t leftCount = _reader.ReadUInt16();
      for (std::uint16_t index = 0; index < leftCount && _reader.Ok(); ++index)
      {
        (void)_reader.ReadUInt32();
      }
    }
  }

  if (facts.partIndex == 0)
  {
    const std::uint16_t tailBytes = _reader.ReadUInt16();
    if (_reader.Ok() && tailBytes > 0)
    {
      facts.tailTick = _reader.ReadUInt32();
      facts.marker = _reader.ReadUInt32();
      facts.sequence = _reader.ReadUInt32();
      facts.viewer = _reader.ReadUInt32();
      facts.grid = _reader.ReadUInt32();
      facts.hasTail = _reader.Ok();
    }
  }

  facts.ok = _reader.Ok();
  return facts;
}

/*
 * Acks a tick the moment it is whole (§2d).
 *
 * `CountingSimulation`'s grid is twelve records, so a tick is always one part
 * and "whole" is "arrived". A test whose fixture grew past a datagram would
 * need the part bookkeeping `DeltaReceiver` does; asserting the single-part
 * assumption here is what makes that a failing test rather than a silent
 * change of what this suite covers.
 */
void AckFrame(QuicTransport& _client, ConnectionId _link, const FrameFacts& _facts)
{
  if (!_facts.ok || _facts.partCount != 1)
  {
    return;
  }
  std::array<std::uint8_t, 32> buffer{};
  ByteWriter writer{buffer};
  WriteWireType(writer, WireType::SnapshotAck);
  Write(writer, SnapshotAck{_facts.gridId, _facts.tick});
  if (writer.Ok())
  {
    (void)_client.Send(_link, TransportChannel::State, writer.Written());
  }
}

/// A simulation that only counts, so a test can assert the loop ran without
/// caring what a world is.
class CountingSimulation final : public Simulation
{
public:
  void AdvanceTick(std::uint32_t _tick) override
  {
    m_lastTick = _tick;
    ++m_ticks;
  }

  /*
   * A grid of `ENTITY_COUNT` entities that never move (ADR-022 §4).
   *
   * **The ranking is the seam now**, and this simulation implements it the
   * simplest honest way: every entity, in id order, all of them guaranteed. All
   * guaranteed is a deliberate choice rather than laziness -- it makes §5c's
   * "when the guaranteed prefix alone exceeds the budget, the budget loses"
   * reachable from a test by setting a small budget, which is the one branch
   * that cannot be reached any other way.
   */
  static constexpr std::uint32_t ENTITY_COUNT = 12;
  static constexpr std::uint32_t FIRST_ENTITY_ID = 100;

  [[nodiscard]] std::uint32_t RankRelevance(const InterestQuery& _query, std::vector<std::uint32_t>& _outRanked) override
  {
    m_lastGrid.store(_query.grid, std::memory_order_relaxed);
    ++m_rankCalls;
    _outRanked.clear();
    for (std::uint32_t index = 0; index < ENTITY_COUNT; ++index)
    {
      _outRanked.push_back(FIRST_ENTITY_ID + index);
    }
    return m_guaranteeAll.load(std::memory_order_relaxed) ? ENTITY_COUNT : 0;
  }

  /*
   * Records for whatever the engine chose, with the viewer written into a field
   * a test can read.
   *
   * `gaugeA` carries the viewer and `gaugeB` the tick's low byte, which is what
   * lets a test prove the *sender* knew which client it was serialising for
   * (ADR-018 A13) -- a claim no amount of identical bytes can make -- and that
   * the record it got is this tick's rather than a cached one.
   *
   * `RefuseSnapshots` makes the grid empty, which is what a torn-down world
   * looks like from here. It no longer stands in for "the fleet outgrew a
   * datagram", because ADR-022 §6 retired that outcome: a grid over budget is
   * partial and honest, never silent.
   */
  void WriteEntities(const SnapshotRequest& _request, std::span<const std::uint32_t> _ids,
                     std::vector<EntityRecord>& _outRecords) override
  {
    const PlayerId viewer = _request.viewer;
    m_lastViewer.store(viewer, std::memory_order_relaxed);

    /*
     * And EVERY viewer, not just the newest (U3c-b).
     *
     * `m_lastViewer` alone could carry A13's claim only while there was one
     * commander -- with a single player it reads `SOLE_PLAYER_ID` whether the
     * sender knew who it was serialising for or not. A set is what makes
     * "each session got its own serialisation" checkable, and a set is only
     * worth keeping once there is a second id to put in it.
     *
     * A bitmask because it is written on the Sim thread and read on the test's,
     * and `fetch_or` is the whole synchronisation this needs. Ids above 63 fold
     * back on themselves; no test here mints anywhere near that many, and a
     * mask that silently aliased would be worse than one that cannot.
     */
    if (viewer < 64)
    {
      m_viewersSeen.fetch_or(std::uint64_t{1} << viewer, std::memory_order_relaxed);
    }
    m_lastGrid.store(_request.grid, std::memory_order_relaxed);

    _outRecords.clear();
    if (m_refuseSnapshots.load(std::memory_order_relaxed))
    {
      return;
    }
    for (const std::uint32_t id : _ids)
    {
      EntityRecord record;
      record.id = id;
      record.typeId = 1;
      // Moves every tick, so a delta always has something to say and a test can
      // tell a fresh record from a repeated one.
      record.posXCm = static_cast<std::int32_t>(_request.tick * 100 + id);
      record.gaugeA = static_cast<std::uint8_t>(viewer);
      record.gaugeB = static_cast<std::uint8_t>(_request.tick & 0xffu);
      _outRecords.push_back(record);
    }
    ++m_snapshotsWritten;
  }

  /*
   * A tiny recognisable tail, so a test can prove the game's opaque bytes
   * reached the client unread.
   *
   * **The sequence written is the request's and not this class's** (ADR-022
   * §7, U3d-a). It used to be `m_lastAcceptedSeq` -- simulation state, global
   * across every submitter, which is exactly the shape §7 moved out of the
   * world. Reading it off the `SnapshotRequest` is what makes the order-loop
   * test below a claim about the *session's* per-viewer high-water mark rather
   * than about a counter the simulation happened to keep.
   */
  [[nodiscard]] bool WriteTickTail(const SnapshotRequest& _request, ByteWriter& _writer) override
  {
    if (m_refuseSnapshots.load(std::memory_order_relaxed))
    {
      return false;
    }
    _writer.WriteUInt32(_request.tick);
    _writer.WriteUInt32(SNAPSHOT_MARKER);
    _writer.WriteUInt32(_request.lastOrderSeqProcessed);
    _writer.WriteUInt32(_request.viewer);
    _writer.WriteUInt32(_request.grid);
    return _writer.Ok();
  }

  /// Whether the ranking claims every entity is guaranteed. Off is what lets a
  /// test watch the budget actually truncate (§6); on is what lets it watch the
  /// budget lose to the guarantee (§5c).
  void GuaranteeAll(bool _guarantee) noexcept { m_guaranteeAll.store(_guarantee, std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t RankCalls() const noexcept { return m_rankCalls.load(std::memory_order_relaxed); }

  /*
   * One grid is viewable and everything else is not, which is the smallest
   * shape that can tell an enforced gate from an ignored one. The refusal
   * carries a number of this test's own choosing, exactly as the order
   * refusals do -- the engine passes it through without knowing what it says.
   */
  static constexpr std::uint16_t VIEWABLE_GRID = 8317;
  /// A second grid this game also allows, so that a view *switch* is reachable
  /// from a suite with one simulation: with one legal grid the only outcomes
  /// are a refusal and a request for the grid already being watched, and
  /// neither of them moves a feed.
  static constexpr std::uint16_t SECOND_VIEWABLE_GRID = 8318;
  static constexpr std::uint16_t FORBIDDEN_GRID = 999;
  static constexpr std::uint16_t NO_PRESENCE_REASON = 4343;

  [[nodiscard]] std::uint16_t MayView(PlayerId, std::uint16_t _grid) override
  {
    ++m_viewChecks;
    return _grid == VIEWABLE_GRID || _grid == SECOND_VIEWABLE_GRID ? std::uint16_t{0} : NO_PRESENCE_REASON;
  }

  /*
   * Which grid each viewer is watching (ADR-016 §7, N5).
   *
   * Recorded rather than acted on: what a hold *means* is the game's business
   * and this game has no worlds to hold, which is exactly the point of the
   * default being to do nothing. What this suite can answer is whether the
   * engine says it at the right moments, and that is what these count.
   */
  void ViewerOpened(PlayerId _viewer, std::uint16_t _grid) override
  {
    ++m_viewerOpens;
    m_lastViewerOpened.store(_viewer, std::memory_order_relaxed);
    m_lastViewerGrid.store(_grid, std::memory_order_relaxed);
  }

  void ViewerClosed(PlayerId _viewer) override
  {
    ++m_viewerCloses;
    m_lastViewerClosed.store(_viewer, std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint32_t ViewerOpens() const noexcept { return m_viewerOpens.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t ViewerCloses() const noexcept { return m_viewerCloses.load(std::memory_order_relaxed); }
  [[nodiscard]] PlayerId LastViewerOpened() const noexcept { return m_lastViewerOpened.load(std::memory_order_relaxed); }
  [[nodiscard]] PlayerId LastViewerClosed() const noexcept { return m_lastViewerClosed.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint16_t LastViewerGrid() const noexcept { return m_lastViewerGrid.load(std::memory_order_relaxed); }

  [[nodiscard]] std::uint16_t LastGrid() const noexcept { return m_lastGrid.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t ViewChecks() const noexcept { return m_viewChecks.load(std::memory_order_relaxed); }

  /*
   * A summary frame carrying nothing but the viewer it was written for.
   *
   * The engine never learns what is inside one (ADR-014 §5) -- a roster and a
   * fleet summary are the game's, under the game's hash -- so a test at this
   * level can put anything in it, and the useful thing to put in it is the
   * question this suite can actually answer: *which client was this written
   * for, and which client received it.*
   */
  [[nodiscard]] bool WriteSummaries(PlayerId _viewer, std::uint32_t, ByteWriter& _writer) override
  {
    ++m_summariesWritten;
    _writer.WriteUInt32(SUMMARY_MARKER);
    _writer.WriteUInt32(_viewer);
    return _writer.Ok();
  }

  static constexpr std::uint32_t SUMMARY_MARKER = 0xd0cca5e5u;
  [[nodiscard]] std::uint32_t SummariesWritten() const noexcept { return m_summariesWritten.load(std::memory_order_relaxed); }

  /// Makes the simulation behave like a grid that has torn down under its
  /// viewer: nothing to rank, nothing to say.
  void RefuseSnapshots(bool _refuse) noexcept { m_refuseSnapshots.store(_refuse, std::memory_order_relaxed); }
  [[nodiscard]] PlayerId LastViewer() const noexcept { return m_lastViewer.load(std::memory_order_relaxed); }

  /// Whether a snapshot was ever serialised for this commander in particular.
  [[nodiscard]] bool SawViewer(PlayerId _viewer) const noexcept
  {
    return _viewer < 64 && (m_viewersSeen.load(std::memory_order_relaxed) & (std::uint64_t{1} << _viewer)) != 0;
  }

  static constexpr std::uint32_t SNAPSHOT_MARKER = 0xfeedbeefu;
  [[nodiscard]] std::uint32_t SnapshotsWritten() const noexcept { return m_snapshotsWritten.load(std::memory_order_relaxed); }

  /*
   * An order format this test invented, which is exactly the point.
   *
   * The engine hands over bytes it never read (ADR-004 ruling 4), so a
   * simulation is free to mean anything by them -- and a simulation that means
   * something no game does is the strongest way to show the engine is not
   * quietly assuming a layout. A payload is a marker and a sequence; anything
   * else is refused with a reason code of this test's own choosing, and the
   * engine passes that number through without knowing what it says.
   */
  static constexpr std::uint32_t ORDER_MARKER = 0xc0defaceu;
  static constexpr std::uint16_t REFUSED_REASON = 4242;

  [[nodiscard]] OrderVerdict ApplyOrderBytes(PlayerId, std::uint32_t _clientId, std::span<const std::uint8_t> _payload) override
  {
    ++m_ordersSeen;
    m_lastOrderClientId = _clientId;

    ByteReader reader{_payload};
    const std::uint32_t marker = reader.ReadUInt32();
    const std::uint32_t sequence = reader.ReadUInt32();

    OrderVerdict verdict;
    verdict.orderSeq = reader.Ok() ? sequence : 0;
    if (!reader.Ok() || marker != ORDER_MARKER)
    {
      verdict.reasonCode = REFUSED_REASON;
      return verdict;
    }

    verdict.accepted = true;
    verdict.serverOrderId = ++m_nextOrderId;
    m_lastAcceptedSeq.store(sequence, std::memory_order_relaxed);
    return verdict;
  }

  /// What the *simulation* last accepted, as distinct from what the session
  /// reports to a viewer. The two agree with one commander and are different
  /// facts (ADR-022 §7), which is why both are kept.
  [[nodiscard]] std::uint32_t LastAcceptedSeq() const noexcept { return m_lastAcceptedSeq.load(std::memory_order_relaxed); }

  [[nodiscard]] std::uint32_t OrdersSeen() const noexcept { return m_ordersSeen.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t LastOrderClientId() const noexcept { return m_lastOrderClientId.load(std::memory_order_relaxed); }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0xfeedfacecafebeefull; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return 0x0123456789abcdefull; }

  /// A world with a far, negative anchor: the plane is signed and full-width
  /// (ADR-009 §1), so a narrowed field folds here instead of in a session.
  /// The display strings ride along so the test proves they arrive verbatim,
  /// and `gridAnchor` — which grid, as opposed to where it is — rides with them.
  [[nodiscard]] WorldMeta World() const override
  {
    return WorldMeta{42, VIEWABLE_GRID, -4200000000ll, 1750000000ll, "Testfall-9", "Proving Grounds 0.0", "SEC -1.0"};
  }

  [[nodiscard]] std::uint32_t Ticks() const noexcept { return m_ticks.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t LastTick() const noexcept { return m_lastTick.load(std::memory_order_relaxed); }

private:
  std::atomic<std::uint32_t> m_ticks{0};
  std::atomic<std::uint32_t> m_snapshotsWritten{0};
  std::atomic<std::uint32_t> m_lastTick{0};
  std::atomic<std::uint32_t> m_ordersSeen{0};
  std::atomic<std::uint32_t> m_lastOrderClientId{0};
  std::atomic<std::uint32_t> m_lastAcceptedSeq{0};
  std::atomic<std::uint32_t> m_rankCalls{0};
  std::atomic<bool> m_guaranteeAll{true};
  std::atomic<std::uint32_t> m_nextOrderId{0};
  std::atomic<std::uint32_t> m_summariesWritten{0};
  std::atomic<PlayerId> m_lastViewer{INVALID_PLAYER_ID};
  std::atomic<std::uint64_t> m_viewersSeen{0};
  std::atomic<std::uint16_t> m_lastGrid{0};
  std::atomic<std::uint32_t> m_viewChecks{0};
  std::atomic<std::uint32_t> m_viewerOpens{0};
  std::atomic<std::uint32_t> m_viewerCloses{0};
  std::atomic<PlayerId> m_lastViewerOpened{INVALID_PLAYER_ID};
  std::atomic<PlayerId> m_lastViewerClosed{INVALID_PLAYER_ID};
  std::atomic<std::uint16_t> m_lastViewerGrid{0};
  std::atomic<bool> m_refuseSnapshots{false};
};

template <typename Predicate>
bool WaitUntil(QuicTransport& _transport, Predicate _predicate, double _timeoutMs = 5000.0)
{
  const std::int64_t start = Clock::Counter();
  while (Clock::MillisecondsBetween(start, Clock::Counter()) < _timeoutMs)
  {
    _transport.Poll();
    if (_predicate())
    {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

} // namespace

TEST_CLASS(ServerHostLifecycleTests)
{
public:
  TEST_METHOD(StartsListeningBeforeItReturns)
  {
    // A caller must be able to connect the moment Start returns (ADR-008 §1).
    NullSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;

    Assert::IsTrue(host.Start(config, simulation));
    Assert::IsTrue(host.Running());
    Assert::IsTrue(host.BoundPort() != 0, L"an ephemeral port was not reported");

    host.Stop();
    host.Join();
  }

  TEST_METHOD(SurvivesRepeatedStartStopCycles)
  {
    // A hundred cycles, per S3's acceptance: enough that a leaked socket, a
    // leaked thread or a handle left open shows up as a failure to bind rather
    // than as something noticed weeks later.
    NullSimulation simulation;
    for (int i = 0; i < 100; ++i)
    {
      ServerHost host;
      ServerConfig config;
      config.port = 0;
      Assert::IsTrue(host.Start(config, simulation));
      host.Stop();
      host.Join();
    }
  }

  TEST_METHOD(StopIsIdempotentAndJoinWithoutStartIsSafe)
  {
    ServerHost host;
    host.Stop();
    host.Join(); // Never started: must not fault or block.

    NullSimulation simulation;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));
    host.Stop();
    host.Stop();
    host.Join();
  }

  TEST_METHOD(AdvancesTheSimulationAtRoughlyTwentyHertz)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Every counter is read *after* the thread has stopped. Sampling one while
    // the loop is still running and comparing it to another read after the join
    // compares two different moments: CI caught exactly that, 10 against 11.
    host.Stop();
    host.Join();

    const std::uint32_t ticks = simulation.Ticks();

    // 500 ms at 20 Hz is ten ticks, plus the one or two the loop finishes while
    // it notices the stop. The bounds are loose because a shared CI runner is
    // not a real-time system; what is checked is that the loop ticks at all,
    // does not sprint, and reports the same count the simulation actually saw.
    Assert::IsTrue(ticks >= 5, L"the tick loop ran far too slowly");
    Assert::IsTrue(ticks <= 40, L"the tick loop ran far too fast");
    Assert::AreEqual(ticks, host.TickCount());
    Assert::AreEqual(simulation.LastTick(), host.TickCount());
  }

  /*
   * The authority stops, and says so (S7's open item).
   *
   * What this pins is the difference the client's F10 cannot make: F10 cuts a
   * client's feed while the server ticks on, and this stops the server while
   * the link stays bound. The two look identical from a client and are not the
   * same event, and until the injector existed only one of them could be
   * produced on demand.
   *
   * The assertions are about the loop, not about wall-clock precision: a
   * shared runner is not a real-time system. A stall longer than
   * `MAX_TICK_DEBT_MS` must be *noticed* -- counted as an overrun and the debt
   * dropped rather than sprinted through -- and the host must go on ticking
   * afterwards, because a stall is a hitch and not a death.
   */
  TEST_METHOD(AnInjectedStallStopsTheAuthorityAndIsCountedAsAnOverrun)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    // Long enough to be unmistakable and to exceed the 250 ms debt ceiling, so
    // the drop-the-debt branch is the one that runs.
    constexpr std::uint32_t STALL_MS = 400;
    host.InjectStall(STALL_MS);

    const std::int64_t start = Clock::Counter();
    while (host.StallCount() == 0 && Clock::MillisecondsBetween(start, Clock::Counter()) < 5000.0)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Assert::AreEqual<std::uint32_t>(1, host.StallCount(), L"the host never served the stall");

    // Sampled the instant the stall was served, so what follows measures the
    // loop after it rather than including it.
    const std::uint32_t ticksAtStall = host.TickCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    host.Stop();
    host.Join();

    Assert::IsTrue(host.OverrunCount() >= 1, L"a stall past the debt ceiling was not counted as an overrun");
    Assert::IsTrue(host.TickCount() > ticksAtStall, L"the host did not resume ticking after the stall");
    Assert::AreEqual<std::uint32_t>(1, host.StallCount(), L"one request produced more than one stall");
    Assert::AreEqual(simulation.LastTick(), host.TickCount(), L"the simulation and the host disagree about the tick");
  }
};

TEST_CLASS(ServerHandshakeTests)
{
public:
  TEST_METHOD(AcceptsAMatchingClientAndAnswersPings)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    Welcome welcome;
    bool joined = false;
    const bool answered = WaitUntil(client,
                                    [&]
                                    {
                                      TransportEvent event;
                                      while (client.NextEvent(event))
                                      {
                                        if (event.type != TransportEvent::Type::Message)
                                        {
                                          continue;
                                        }
                                        ByteReader reader{event.payload};
                                        if (ReadWireType(reader) == WireType::Welcome && Read(reader, welcome))
                                        {
                                          joined = true;
                                        }
                                      }
                                      return joined;
                                    });

    Assert::IsTrue(answered, L"the server never sent Welcome");
    Assert::IsTrue(welcome.clientId != 0);
    Assert::AreEqual<std::uint16_t>(20, welcome.tickRate);
    Assert::AreEqual(simulation.SchemaHash(), welcome.schemaHash);
    Assert::AreEqual(simulation.ContentHash(), welcome.contentHash);
    Assert::AreEqual<std::uint32_t>(1, host.SessionCount());

    // The world reaches the client from the simulation, untouched by the engine
    // in between (ADR-009 §8). Without it a client in another process has no
    // frame to place a replicated position in.
    const WorldMeta world = simulation.World();
    Assert::AreEqual<std::uint16_t>(world.worldId, welcome.worldId);
    Assert::AreEqual<std::uint16_t>(world.gridAnchor, welcome.gridAnchor,
                                    L"which grid, as opposed to where it is -- the number a Dock names");
    Assert::AreEqual(world.anchorX, welcome.anchorX);
    Assert::AreEqual(world.anchorY, welcome.anchorY);
    Assert::AreEqual(world.worldName, welcome.worldName);
    Assert::AreEqual(world.worldDetail, welcome.worldDetail);
    Assert::AreEqual(world.worldBadge, welcome.worldBadge);

    // The heartbeat: ping out on the unreliable channel, pong back with the
    // timestamp untouched. This is what milestone M0 is.
    const std::uint64_t stamp = 0x00c0ffee0000ull;
    ByteWriter pingWriter{buffer};
    WriteWireType(pingWriter, WireType::Ping);
    Write(pingWriter, Ping{stamp});
    Assert::IsTrue(client.Send(link, TransportChannel::State, pingWriter.Written()));

    Pong pong;
    bool ponged = false;
    const bool returned = WaitUntil(client,
                                    [&]
                                    {
                                      TransportEvent event;
                                      while (client.NextEvent(event))
                                      {
                                        if (event.type != TransportEvent::Type::Message)
                                        {
                                          continue;
                                        }
                                        ByteReader reader{event.payload};
                                        if (ReadWireType(reader) == WireType::Pong && Read(reader, pong))
                                        {
                                          ponged = true;
                                        }
                                      }
                                      return ponged;
                                    });

    Assert::IsTrue(returned, L"the heartbeat never came back");
    Assert::AreEqual(stamp, pong.clientSendMicroseconds);
    Assert::IsTrue(pong.serverTick > 0, L"the server answered without having ticked");

    host.Stop();
    host.Join();
  }

  /*
   * The loop closes: a joined client is sent the simulation's own bytes, on the
   * unreliable channel, without having asked (ADR-004 §6).
   *
   * This is the only place a snapshot is watched crossing a real socket. The
   * GameLogic suite proves the encoding round-trips and the client suite proves
   * the clock behaves; the server's fan-out sits between them and nothing else
   * touches it. The payload is checked for the simulation's marker rather than
   * merely for length, because the failure worth catching is the engine sending
   * something of its own devising -- a snapshot it framed but did not get from
   * the game would still be the right size.
   */
  TEST_METHOD(SendsTheSimulationsOwnBytesToAJoinedClient)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    // Nothing is sent to a room of nobody, so the join has to land first.
    bool joined = false;
    PlayerId welcomedPlayer = INVALID_PLAYER_ID;
    std::uint32_t snapshotTick = 0;
    std::uint32_t marker = 0;
    std::uint32_t snapshotViewer = INVALID_PLAYER_ID;
    std::uint32_t snapshotsSeen = 0;

    const bool sawSnapshots = WaitUntil(client,
                                        [&]
                                        {
                                          TransportEvent event;
                                          while (client.NextEvent(event))
                                          {
                                            if (event.type != TransportEvent::Type::Message)
                                            {
                                              continue;
                                            }
                                            ByteReader reader{event.payload};
                                            const WireType type = ReadWireType(reader);
                                            if (type == WireType::Welcome)
                                            {
                                              Welcome welcome;
                                              if (Read(reader, welcome))
                                              {
                                                welcomedPlayer = welcome.playerId;
                                              }
                                              joined = true;
                                            }
                                            else if (type == WireType::Snapshot || type == WireType::Keyframe)
                                            {
                                              // Recorded only once the whole
                                              // payload has been read, so a
                                              // truncated datagram cannot
                                              // overwrite a good one. Acked, so
                                              // the server gets a baseline and
                                              // the feed reaches its steady
                                              // state (ADR-022 §2c).
                                              const FrameFacts facts = ReadFrame(type, reader);
                                              AckFrame(client, link, facts);
                                              if (facts.hasTail)
                                              {
                                                snapshotTick = facts.tick;
                                                marker = facts.marker;
                                                snapshotViewer = facts.viewer;
                                                ++snapshotsSeen;
                                              }
                                            }
                                          }
                                          // Two, so this cannot pass on a single
                                          // snapshot that happened to be sent.
                                          return joined && snapshotsSeen >= 2;
                                        });

    Assert::IsTrue(sawSnapshots, L"the server never sent a snapshot to a joined client");
    Assert::AreEqual(CountingSimulation::SNAPSHOT_MARKER, marker, L"the payload was not the simulation's");
    Assert::IsTrue(snapshotTick > 0, L"a snapshot was sent for a tick the world never ran");
    Assert::IsTrue(snapshotTick <= host.TickCount(), L"a snapshot ran ahead of the tick counter");
    Assert::IsTrue(simulation.SnapshotsWritten() >= snapshotsSeen, L"more snapshots arrived than were written");
    Assert::AreEqual<std::uint32_t>(0, host.SnapshotFailureCount());

    /*
     * The per-client claim, and the only assertion here that a broadcast sender
     * could not also satisfy (ADR-018 A13, ADR-022 §1).
     *
     * The simulation wrote the viewer it was asked to serialise for into the
     * payload, so this compares the player the handshake named with the player
     * the snapshot was made for. With identical bytes going to one client there
     * is no other way to tell a per-client sender from a broadcast one -- which
     * is exactly why the seam had to grow the viewer before the day two clients
     * are owed different worlds, not on it.
     */
    Assert::IsTrue(welcomedPlayer != INVALID_PLAYER_ID, L"the Welcome named no player");
    Assert::AreEqual(welcomedPlayer, snapshotViewer, L"the snapshot was serialised for a different viewer than joined");
    Assert::AreEqual(welcomedPlayer, simulation.LastViewer(), L"the simulation was asked for no particular viewer");

    host.Stop();
    host.Join();
  }

  /*
   * The summary family reaches its viewer, and reaches it at its own cadence
   * (ADR-016 §6, ADR-017 §1, ADR-018 A13).
   *
   * ADR-017 §1's privacy rule -- other commanders cannot see what is docked at
   * a station -- is a *wire* fact, and the wire is the only place it can be
   * checked. It is checked here rather than left to U3c's two clients because
   * on the broadcast sender this replaced, the rule was satisfied by accident:
   * with one client, "everyone gets it" and "its owner gets it" are the same
   * observation, and the day they stop being the same is the day a leak ships.
   * What this test can prove today is the half that does not need a second
   * client -- that the frame was *written for* the player who joined, and
   * arrived on that player's connection -- and that is exactly the half that
   * had no mechanism before A13.
   *
   * The cadence is the other claim. A summary is not a snapshot: 20 Hz for the
   * grid you are watching, about 1 Hz for everywhere else, which is what makes
   * showing a fleet you are *not* watching affordable at all.
   */
  TEST_METHOD(ASummaryFrameIsWrittenForItsViewerAndArrivesAtItsOwnCadence)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    bool joined = false;
    PlayerId welcomedPlayer = INVALID_PLAYER_ID;
    std::uint32_t snapshotsSeen = 0;
    std::uint32_t summariesSeen = 0;
    std::uint32_t summaryViewer = INVALID_PLAYER_ID;
    std::uint32_t summaryMarker = 0;

    const bool sawSummaries = WaitUntil(client,
                                        [&]
                                        {
                                          TransportEvent event;
                                          while (client.NextEvent(event))
                                          {
                                            if (event.type != TransportEvent::Type::Message)
                                            {
                                              continue;
                                            }
                                            ByteReader reader{event.payload};
                                            const WireType type = ReadWireType(reader);
                                            if (type == WireType::Welcome)
                                            {
                                              Welcome welcome;
                                              if (Read(reader, welcome))
                                              {
                                                welcomedPlayer = welcome.playerId;
                                              }
                                              joined = true;
                                            }
                                            else if (type == WireType::Snapshot || type == WireType::Keyframe)
                                            {
                                              AckFrame(client, link, ReadFrame(type, reader));
                                              ++snapshotsSeen;
                                            }
                                            else if (type == WireType::Summary)
                                            {
                                              const std::uint32_t marker = reader.ReadUInt32();
                                              const std::uint32_t viewer = reader.ReadUInt32();
                                              if (reader.Ok())
                                              {
                                                summaryMarker = marker;
                                                summaryViewer = viewer;
                                                ++summariesSeen;
                                              }
                                            }
                                          }
                                          // Two, so a single frame that happened
                                          // to be sent cannot pass this.
                                          return joined && summariesSeen >= 2;
                                        });

    Assert::IsTrue(sawSummaries, L"no summary frame reached a joined client");
    Assert::AreEqual(CountingSimulation::SUMMARY_MARKER, summaryMarker, L"the payload was not the simulation's");
    Assert::IsTrue(welcomedPlayer != INVALID_PLAYER_ID, L"the Welcome named no player");
    Assert::AreEqual(welcomedPlayer, summaryViewer, L"the frame was written for a player other than the one it reached");

    // The cadence, stated as the relationship rather than as a count of ticks:
    // by the time two frames have arrived, many more snapshots have. Asserting
    // the ratio rather than an exact number keeps this from failing on a slow
    // runner, which is the failure mode a timing test earns its keep by not
    // having.
    Assert::IsTrue(snapshotsSeen > summariesSeen * 4,
                   L"summaries arrived at anything like the snapshot rate; the ~1 Hz cadence is not being applied");

    host.Stop();
    host.Join();
  }

  /*
   * A view request is answered, gated by the game, and enforced by the engine
   * (ADR-016 §4, §7 — U3b).
   *
   * The three claims worth separating, because a broken gate can satisfy two of
   * them: that the *game* is asked (`ViewChecks` climbs), that a refusal is
   * carried back with the game's own reason code intact, and that a refused
   * request **leaves the feed where it was** rather than half-moving it. The
   * last is the one a naive implementation gets wrong — setting the view first
   * and validating after leaves a client watching a grid it was just told it
   * could not have.
   */
  TEST_METHOD(AViewRequestIsGatedByTheGameAndEnforcedByTheEngine)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    bool joined = false;
    std::vector<ViewChanged> answers;
    std::uint32_t snapshotGrid = 0;
    const auto drain = [&]
    {
      TransportEvent event;
      while (client.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          joined = true;
        }
        else if (type == WireType::ViewChanged)
        {
          ViewChanged changed;
          if (Read(reader, changed))
          {
            answers.push_back(changed);
          }
        }
        else if (type == WireType::Snapshot || type == WireType::Keyframe)
        {
          const FrameFacts facts = ReadFrame(type, reader);
          AckFrame(client, link, facts);
          if (facts.hasTail)
          {
            snapshotGrid = facts.grid;
          }
        }
      }
    };

    Assert::IsTrue(WaitUntil(client, [&] { drain(); return joined && snapshotGrid != 0; }));
    Assert::AreEqual<std::uint32_t>(CountingSimulation::VIEWABLE_GRID, snapshotGrid,
                                    L"a session opens on the simulation's own grid");

    // A grid the game refuses.
    ByteWriter refuseWriter{buffer};
    WriteWireType(refuseWriter, WireType::ViewRequest);
    Write(refuseWriter, ViewRequest{CountingSimulation::FORBIDDEN_GRID});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, refuseWriter.Written()));

    Assert::IsTrue(WaitUntil(client, [&] { drain(); return !answers.empty(); }));
    Assert::IsFalse(answers[0].accepted, L"the game refused and the engine agreed");
    Assert::AreEqual<std::uint16_t>(CountingSimulation::FORBIDDEN_GRID, answers[0].gridAnchor,
                                    L"the grid is echoed, so a client with two in flight knows which was refused");
    Assert::AreEqual<std::uint16_t>(CountingSimulation::NO_PRESENCE_REASON, answers[0].reasonCode,
                                    L"the game's own reason came back unread");
    Assert::IsTrue(simulation.ViewChecks() > 0, L"the game was actually asked");

    // And the feed did not move. Several more snapshots have to arrive for this
    // to mean anything, so wait for them rather than sampling once.
    snapshotGrid = 0;
    Assert::IsTrue(WaitUntil(client, [&] { drain(); return snapshotGrid != 0; }));
    Assert::AreEqual<std::uint32_t>(CountingSimulation::VIEWABLE_GRID, snapshotGrid,
                                    L"a refused request moved the feed anyway");

    // The grid it is allowed to have is accepted, and the same request path works.
    answers.clear();
    ByteWriter allowWriter{buffer};
    WriteWireType(allowWriter, WireType::ViewRequest);
    Write(allowWriter, ViewRequest{CountingSimulation::VIEWABLE_GRID});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, allowWriter.Written()));

    Assert::IsTrue(WaitUntil(client, [&] { drain(); return !answers.empty(); }));
    Assert::IsTrue(answers[0].accepted, L"the grid the game allows was refused");
    Assert::AreEqual<std::uint16_t>(0, answers[0].reasonCode, L"an accepted view carries no reason");

    host.Stop();
    host.Join();
  }

  /*
   * **The game is told which grid each viewer is watching** (ADR-016 §7, N5).
   *
   * `MayView` has always asked permission; nothing ever reported the answer.
   * `WorldRegistry::AddViewer` shipped with U2 and its header said "U3b is what
   * starts calling these", and every caller in the tree turned out to be the
   * composition root holding its own start grid -- so the hold that stops a
   * world being torn down under somebody's camera was held on a grid chosen at
   * boot rather than on the one being looked at.
   *
   * What this suite can answer is the engine's half: that the report happens at
   * session open, that an accepted switch moves it, that a refused one does
   * not, and that the socket closing takes it back. What a hold *means* is the
   * game's, and `CountingSimulation` has no worlds to hold -- which is why the
   * seam defaults to doing nothing rather than being pure.
   */
  TEST_METHOD(TheGameIsToldWhichGridEachViewerIsWatching)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    PlayerId player = INVALID_PLAYER_ID;
    std::vector<ViewChanged> answers;
    const auto drain = [&]
    {
      TransportEvent event;
      while (client.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          Welcome welcome;
          if (Read(reader, welcome))
          {
            player = welcome.playerId;
          }
        }
        else if (type == WireType::ViewChanged)
        {
          ViewChanged changed;
          if (Read(reader, changed))
          {
            answers.push_back(changed);
          }
        }
      }
    };

    // A session opens on a grid, so the hold is taken before the first snapshot
    // rather than by whatever happens to borrow the world first.
    Assert::IsTrue(WaitUntil(client, [&] { drain(); return player != INVALID_PLAYER_ID && simulation.ViewerOpens() > 0; }));
    Assert::AreEqual<std::uint32_t>(1, simulation.ViewerOpens(), L"a session opening is one report, not one per tick");
    Assert::AreEqual<std::uint32_t>(player, simulation.LastViewerOpened(), L"the durable player, never the connection");
    Assert::AreEqual<std::uint16_t>(CountingSimulation::VIEWABLE_GRID, simulation.LastViewerGrid(),
                                    L"a session opens watching the grid the handshake settled on");

    /*
     * A refused request reports nothing.
     *
     * This is the half that would leak if the engine reported the *requested*
     * grid rather than the feed's: the game would be holding a grid the player
     * was never shown, and nothing would ever release it.
     */
    ByteWriter refuseWriter{buffer};
    WriteWireType(refuseWriter, WireType::ViewRequest);
    Write(refuseWriter, ViewRequest{CountingSimulation::FORBIDDEN_GRID});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, refuseWriter.Written()));

    Assert::IsTrue(WaitUntil(client, [&] { drain(); return !answers.empty(); }));
    Assert::IsFalse(answers[0].accepted, L"the fixture's forbidden grid was allowed");
    Assert::AreEqual<std::uint32_t>(1, simulation.ViewerOpens(), L"a refused view moved the hold anyway");
    Assert::AreEqual<std::uint16_t>(CountingSimulation::VIEWABLE_GRID, simulation.LastViewerGrid());

    // An accepted one moves it, and reports the grid the feed is now on rather
    // than the one that was asked for -- the same number, and the feed is the
    // one that decides.
    answers.clear();
    ByteWriter moveWriter{buffer};
    WriteWireType(moveWriter, WireType::ViewRequest);
    Write(moveWriter, ViewRequest{CountingSimulation::SECOND_VIEWABLE_GRID});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, moveWriter.Written()));

    Assert::IsTrue(WaitUntil(client, [&] { drain(); return !answers.empty(); }));
    Assert::IsTrue(answers[0].accepted, L"the fixture's second grid was refused");
    Assert::AreEqual<std::uint32_t>(2, simulation.ViewerOpens(), L"an accepted view left the hold where it was");
    Assert::AreEqual<std::uint16_t>(CountingSimulation::SECOND_VIEWABLE_GRID, simulation.LastViewerGrid());

    // And the socket closing takes it back. The commander may resume inside the
    // grace window -- they still own their fleet -- but they have no camera
    // while they are gone, and a hold is about a camera.
    Assert::AreEqual<std::uint32_t>(0, simulation.ViewerCloses(), L"nothing has disconnected yet");
    client.Close(link, DisconnectReason::ClosedByPeer);
    Assert::IsTrue(WaitUntil(client, [&] { return simulation.ViewerCloses() > 0; }));
    Assert::AreEqual<std::uint32_t>(player, simulation.LastViewerClosed(),
                                   L"the hold that was released belonged to somebody else");

    host.Stop();
    host.Join();
  }

  /*
   * **A grid over budget is partial and honest, never silent** (ADR-022 §6).
   *
   * This replaces `AGridPastTheDatagramCapRefusesLoudlyAndRecovers`, and the
   * test it replaces is worth naming because its behaviour was *correct* and is
   * now wrong. Until U3d-b a grid that would not fit one datagram sent nothing
   * at all, counted the tick, and logged a line naming the client -- the honest
   * failure, because a truncated snapshot reads as a mass despawn followed by a
   * mass respawn. It is also R19: two commanders meeting at the starter station
   * is 83 records against a 43-record cap, and the designed response was to
   * show nobody anything.
   *
   * §6 replaces refusal with **priority truncation**. A tick fills its byte
   * budget from the ranked list, what does not fit keeps its place for the next
   * tick, and the header carries an honest `culledCount` so the player is told
   * *how many* they are not being shown rather than being shown an empty grid.
   *
   * The budget here is set small enough that the simulation's twelve entities
   * cannot all fit, which is the only way to reach the truncation branch from a
   * suite with no GameLogic in it (ADR-014).
   */
  TEST_METHOD(AGridOverBudgetIsTruncatedAndCounted)
  {
    CountingSimulation simulation;
    // Nothing guaranteed, so the budget is what decides -- §5c's "the budget
    // loses to the guarantee" is the *other* test.
    simulation.GuaranteeAll(false);

    ServerHost host;
    ServerConfig config;
    config.port = 0;
    // Room for five of the twelve records, so seven are culled every tick.
    config.tickBudgetBytes = 5 * static_cast<std::uint32_t>(ENTITY_RECORD_BYTES);
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    bool joined = false;
    std::uint32_t framesSeen = 0;
    std::uint32_t recordsSeen = 0;
    std::uint16_t culled = 0;
    const auto drain = [&]
    {
      TransportEvent event;
      while (client.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          joined = true;
        }
        else if (type == WireType::Snapshot || type == WireType::Keyframe)
        {
          const FrameFacts facts = ReadFrame(type, reader);
          AckFrame(client, link, facts);
          if (facts.ok)
          {
            ++framesSeen;
            recordsSeen += facts.recordCount;
            culled = facts.culledCount;
          }
        }
      }
    };

    const bool sent = WaitUntil(client,
                                [&]
                                {
                                  drain();
                                  return joined && framesSeen >= 3;
                                });

    Assert::IsTrue(sent, L"a grid over budget sent nothing at all; §6 says partial, never silent");
    Assert::IsTrue(recordsSeen > 0, L"a partial view still has to carry records");
    Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(CountingSimulation::ENTITY_COUNT - 5), culled,
                                    L"the client was not told how many it is not being shown");
    Assert::AreEqual<std::uint32_t>(0, host.SnapshotFailureCount(), L"truncation is not a failure");

    host.Stop();
    host.Join();
  }

  /*
   * **A client that stops acking is resynced with a keyframe** (ADR-022 §2c).
   *
   * The ring holds `BASELINE_RING_TICKS` (32, 1.6 s) views as sent. Past that
   * the acked tick has fallen out and there is nothing to encode a delta
   * against, so the server sends a keyframe and starts again -- unconditionally,
   * because a partial-resync mode is a thing to get subtly wrong and this design
   * deliberately does not have one.
   *
   * The stall is simulated by simply not acking, which is what a client with a
   * dead uplink looks like from the server. What is asserted is that a *second*
   * keyframe arrives: the first is the join, and a test that counted one would
   * pass against a server that never recovered at all.
   */
  TEST_METHOD(AClientThatStopsAckingIsResyncedWithAKeyframe)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    bool joined = false;
    bool acking = true;
    std::uint32_t keyframes = 0;
    std::uint32_t deltas = 0;
    const auto drain = [&]
    {
      TransportEvent event;
      while (client.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          joined = true;
        }
        else if (type == WireType::Keyframe)
        {
          const FrameFacts facts = ReadFrame(type, reader);
          ++keyframes;
          if (acking)
          {
            AckFrame(client, link, facts);
          }
        }
        else if (type == WireType::Snapshot)
        {
          const FrameFacts facts = ReadFrame(type, reader);
          ++deltas;
          if (acking)
          {
            AckFrame(client, link, facts);
          }
        }
      }
    };

    // First the steady state, so the run below is a *stall* and not a join.
    const bool settled = WaitUntil(client,
                                   [&]
                                   {
                                     drain();
                                     return joined && keyframes >= 1 && deltas >= 3;
                                   });
    Assert::IsTrue(settled, L"the feed never reached its steady state");
    Assert::AreEqual<std::uint32_t>(1, keyframes, L"a healthy feed sends one keyframe and then deltas");

    // Now go quiet. The ring is 32 ticks, so at 20 Hz this takes about 1.6 s.
    acking = false;
    const bool resynced = WaitUntil(
      client,
      [&]
      {
        drain();
        return keyframes >= 2;
      },
      6000.0);

    Assert::IsTrue(resynced, L"a client whose ack fell out of the ring was never sent a keyframe");

    host.Stop();
    host.Join();
  }

  /*
   * **An ack older than the highest is ignored** (ADR-022 §2a).
   *
   * The acks ride the unreliable channel, so they arrive out of order as a
   * matter of course. Taking the highest per (client, grid) is what makes that
   * a non-event -- and taking the *latest* instead would let a reordered
   * datagram walk the server's baseline backwards, which costs a larger delta
   * every time and, worse, is invisible.
   *
   * Watched through `baselineTick`, which is the server saying out loud which
   * sent view it encoded against.
   */
  TEST_METHOD(AnAckOlderThanTheHighestIsIgnored)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    bool joined = false;
    std::uint16_t grid = 0;
    std::uint32_t firstAcked = 0;
    std::uint32_t highestBaseline = 0;
    std::uint32_t lowestBaselineAfterStale = 0xffffffffu;
    bool sentStaleAck = false;
    std::uint32_t deltasAfterStale = 0;

    const auto drain = [&]
    {
      TransportEvent event;
      while (client.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          joined = true;
        }
        else if (type == WireType::Keyframe || type == WireType::Snapshot)
        {
          const FrameFacts facts = ReadFrame(type, reader);
          if (!facts.ok)
          {
            continue;
          }
          grid = facts.gridId;
          AckFrame(client, link, facts);
          if (firstAcked == 0)
          {
            firstAcked = facts.tick;
          }
          if (type == WireType::Snapshot)
          {
            highestBaseline = std::max(highestBaseline, facts.tick);
            if (sentStaleAck)
            {
              ++deltasAfterStale;
              lowestBaselineAfterStale = std::min(lowestBaselineAfterStale, static_cast<std::uint32_t>(facts.tick));
            }
          }
        }
      }
    };

    // Let the feed run so the server's acked tick is well past the join.
    const bool settled = WaitUntil(client,
                                   [&]
                                   {
                                     drain();
                                     return joined && highestBaseline > firstAcked + 10;
                                   });
    Assert::IsTrue(settled, L"the feed never got far enough past the join to have a stale ack to send");

    /*
     * Now a flood of stale acks naming the *join* tick, which is far behind.
     * A server that took the latest rather than the highest would start
     * encoding against it -- and, once it fell out of the ring, would answer
     * with keyframes forever.
     */
    const std::uint32_t staleTick = firstAcked;
    sentStaleAck = true;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
      ByteWriter stale{buffer};
      WriteWireType(stale, WireType::SnapshotAck);
      Write(stale, SnapshotAck{grid, staleTick});
      Assert::IsTrue(stale.Ok());
      (void)client.Send(link, TransportChannel::State, stale.Written());
    }

    const bool sawMore = WaitUntil(client,
                                   [&]
                                   {
                                     drain();
                                     return deltasAfterStale >= 5;
                                   });
    Assert::IsTrue(sawMore, L"the feed stopped after the stale acks, which is itself the failure");
    Assert::IsTrue(lowestBaselineAfterStale > staleTick,
                   L"a stale ack walked the server's baseline backwards; §2a says highest wins");

    host.Stop();
    host.Join();
  }

  /*
   * **When the guaranteed prefix alone exceeds the budget, the budget loses**
   * (ADR-022 §5c).
   *
   * The one outcome the whole ADR is written to prevent is culling a player's
   * own fleet to hit a bandwidth number, so the rule is stated as an
   * asymmetry: the budget truncates tier 1 and 2 and is simply overridden by
   * tier 0. The overrun is *counted* rather than swallowed, because a
   * deployment whose budget is smaller than its fleet envelope is a decision
   * for a person and not a thing the server should quietly absorb.
   *
   * Every entity is guaranteed here and the budget holds five, so the overrun
   * is unavoidable and the count is what proves it was noticed.
   */
  TEST_METHOD(TheGuaranteedPrefixOverridesTheBudgetAndIsCounted)
  {
    CountingSimulation simulation;
    simulation.GuaranteeAll(true);

    ServerHost host;
    ServerConfig config;
    config.port = 0;
    config.tickBudgetBytes = 5 * static_cast<std::uint32_t>(ENTITY_RECORD_BYTES);
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    bool joined = false;
    std::uint32_t everyRecord = 0;
    std::uint16_t culled = 0xffffu;
    const auto drain = [&]
    {
      TransportEvent event;
      while (client.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          joined = true;
        }
        else if (type == WireType::Keyframe)
        {
          const FrameFacts facts = ReadFrame(type, reader);
          AckFrame(client, link, facts);
          if (facts.ok)
          {
            everyRecord = facts.recordCount;
            culled = facts.culledCount;
          }
        }
      }
    };

    const bool sent = WaitUntil(client,
                                [&]
                                {
                                  drain();
                                  return joined && everyRecord > 0;
                                });

    Assert::IsTrue(sent, L"the guaranteed fleet never went out");
    Assert::AreEqual<std::uint32_t>(CountingSimulation::ENTITY_COUNT, everyRecord,
                                    L"a guaranteed entity was culled to hit a bandwidth number");
    Assert::AreEqual<std::uint16_t>(0, culled, L"nothing was culled, so nothing should be claimed as culled");
    Assert::AreEqual<std::uint32_t>(0, host.SnapshotFailureCount());

    host.Stop();
    host.Join();
  }

  /*
   * Two clients, two serialisations (ADR-022 §1, ADR-018 A13).
   *
   * The rule this pins is not "snapshots arrive" -- the test above covers that
   * -- but *where the bytes are made*. A broadcast-shaped host serialises once
   * per tick and sends the result twice, so the count of snapshots the
   * simulation was asked to write would sit at about half the count the two
   * clients received. Per client, writes can never be fewer than sends.
   *
   * It is written now, before anything culls, because this is the assertion
   * that stops the shape regressing: ADR-017 §1's private roster and ADR-022's
   * per-viewer delta both rest on it, and neither is here yet to notice.
   */
  TEST_METHOD(EverySessionIsServedItsOwnSerialisation)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport first;
    QuicTransport second;
    const ConnectionId firstLink = first.Connect("127.0.0.1", host.BoundPort());
    const ConnectionId secondLink = second.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(firstLink != INVALID_CONNECTION, L"the first client could not open a link");
    Assert::IsTrue(secondLink != INVALID_CONNECTION, L"the second client could not open a link");

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(first.Send(firstLink, TransportChannel::Control, writer.Written()));
    Assert::IsTrue(second.Send(secondLink, TransportChannel::Control, writer.Written()));

    bool firstJoined = false;
    bool secondJoined = false;
    std::uint32_t firstSnapshots = 0;
    std::uint32_t secondSnapshots = 0;
    PlayerId firstPlayer = INVALID_PLAYER_ID;
    PlayerId secondPlayer = INVALID_PLAYER_ID;

    const auto drain = [](QuicTransport& _transport, bool& _joined, std::uint32_t& _snapshots, PlayerId& _player)
    {
      TransportEvent event;
      while (_transport.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          _joined = true;

          // Who the server says this connection is (U3c-b). Read rather than
          // assumed, because assuming is what this test used to do.
          Welcome welcome;
          if (Read(reader, welcome))
          {
            _player = welcome.playerId;
          }
        }
        else if (type == WireType::Snapshot || type == WireType::Keyframe)
        {
          ++_snapshots;
        }
      }
    };

    // Three each, so the comparison below has enough ticks in it to separate
    // "one serialisation sent twice" from "two serialisations".
    constexpr std::uint32_t ENOUGH = 3;
    const std::int64_t start = Clock::Counter();
    while (Clock::MillisecondsBetween(start, Clock::Counter()) < 5000.0)
    {
      first.Poll();
      second.Poll();
      drain(first, firstJoined, firstSnapshots, firstPlayer);
      drain(second, secondJoined, secondSnapshots, secondPlayer);
      if (firstJoined && secondJoined && firstSnapshots >= ENOUGH && secondSnapshots >= ENOUGH)
      {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    Assert::IsTrue(firstJoined && secondJoined, L"both clients did not join");
    Assert::IsTrue(firstSnapshots >= ENOUGH && secondSnapshots >= ENOUGH, L"both clients were not served snapshots");
    Assert::IsTrue(simulation.SnapshotsWritten() >= firstSnapshots + secondSnapshots,
                   L"more snapshots arrived than were serialised, so one serialisation was sent to more than one client");
    /*
     * A13's actual claim, finally checkable (U3c-b).
     *
     * This asserted `LastViewer() == SOLE_PLAYER_ID` while every session was
     * player one -- which a broadcast sender that ignored the viewer entirely
     * would also have satisfied, since there was only one value it could
     * possibly have been. Two commanders is what turns "the sender knew which
     * client it was serialising for" from a sentence into a test.
     */
    Assert::IsTrue(firstPlayer != INVALID_PLAYER_ID && secondPlayer != INVALID_PLAYER_ID,
                   L"a session was welcomed as nobody");
    Assert::IsTrue(firstPlayer != secondPlayer, L"two sessions on one shard were given one identity");
    Assert::IsTrue(simulation.SawViewer(firstPlayer), L"no snapshot was serialised for the first commander");
    Assert::IsTrue(simulation.SawViewer(secondPlayer), L"no snapshot was serialised for the second commander");
    Assert::AreEqual<std::uint32_t>(0, host.SnapshotFailureCount());

    host.Stop();
    host.Join();
  }

  /*
   * Submit, ack, and see it in the next snapshot (Build Order S9's acceptance).
   *
   * The whole loop over a real socket, and every part of it engine-side: the
   * order format belongs to `CountingSimulation` and the engine never parses
   * it, the verdict comes back out of the game and the engine echoes it, and
   * the sequence in the ack is the one the simulation read -- because the
   * engine could not have read it (ADR-004 ruling 4).
   */
  TEST_METHOD(AnOrderIsAckedAndThenAppearsInTheSnapshot)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter hello{buffer};
    WriteWireType(hello, WireType::Hello);
    Write(hello, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, hello.Written()));

    bool joined = false;
    Assert::IsTrue(WaitUntil(client,
                             [&]
                             {
                               TransportEvent event;
                               while (client.NextEvent(event))
                               {
                                 if (event.type != TransportEvent::Type::Message)
                                 {
                                   continue;
                                 }
                                 ByteReader reader{event.payload};
                                 Welcome welcome;
                                 if (ReadWireType(reader) == WireType::Welcome && Read(reader, welcome))
                                 {
                                   joined = true;
                                 }
                               }
                               return joined;
                             }),
                   L"the server never sent Welcome");

    constexpr std::uint32_t ORDER_SEQ = 31337;
    ByteWriter order{buffer};
    WriteWireType(order, WireType::OrderSubmit);
    order.WriteUInt32(CountingSimulation::ORDER_MARKER);
    order.WriteUInt32(ORDER_SEQ);
    Assert::IsTrue(client.Send(link, TransportChannel::Control, order.Written()));

    OrderAck ack;
    bool acked = false;
    std::uint32_t seenSeq = 0;
    bool seenInSnapshot = false;

    const bool closed = WaitUntil(client,
                                  [&]
                                  {
                                    TransportEvent event;
                                    while (client.NextEvent(event))
                                    {
                                      if (event.type != TransportEvent::Type::Message)
                                      {
                                        continue;
                                      }
                                      ByteReader reader{event.payload};
                                      const WireType type = ReadWireType(reader);
                                      if (type == WireType::OrderAck && Read(reader, ack))
                                      {
                                        acked = true;
                                      }
                                      else if (type == WireType::Snapshot || type == WireType::Keyframe)
                                      {
                                        const FrameFacts facts = ReadFrame(type, reader);
                                        AckFrame(client, link, facts);
                                        if (facts.hasTail && facts.marker == CountingSimulation::SNAPSHOT_MARKER &&
                                            facts.tailTick > 0)
                                        {
                                          seenSeq = facts.sequence;
                                          seenInSnapshot = seenInSnapshot || facts.sequence == ORDER_SEQ;
                                        }
                                      }
                                    }
                                    // Both halves, and the snapshot half only
                                    // counts after the ack: a snapshot that
                                    // already carried the order before the ack
                                    // arrived would still be the loop closing.
                                    return acked && seenInSnapshot;
                                  });

    Assert::IsTrue(closed, L"the order loop never closed");
    Assert::AreEqual<std::uint8_t>(1, ack.accepted);
    Assert::AreEqual<std::uint32_t>(ORDER_SEQ, ack.orderSeq, L"the sequence is the game's echo, not the engine's guess");
    Assert::IsTrue(ack.serverOrderId != 0, L"an accepted order is given an id");
    Assert::AreEqual<std::uint16_t>(0, ack.reasonCode);
    Assert::AreEqual<std::uint32_t>(ORDER_SEQ, seenSeq);

    Assert::AreEqual<std::uint32_t>(1, simulation.OrdersSeen());
    Assert::IsTrue(simulation.LastOrderClientId() != 0, L"the order was attributed to the session that sent it");
    Assert::AreEqual<std::uint32_t>(0, host.RefusedOrderCount());

    host.Stop();
    host.Join();
  }

  /*
   * The half of ADR-022 §7 that landed on this side of the seam (U3d-a).
   *
   * `lastOrderSeqProcessed` was world-global state folded into the world hash;
   * it is session state now, and `SnapshotSender` is the object there is one of
   * per viewer. The two rules it has to obey are cheap to state and were both
   * true of the world's version, so losing either in the move would be silent.
   */
  TEST_METHOD(TheSessionsOrderSequenceIsAHighWaterMark)
  {
    SnapshotSender sender{SOLE_PLAYER_ID, INVALID_CONNECTION, 0};
    Assert::AreEqual<std::uint32_t>(0, sender.LastOrderSeqProcessed(), L"a fresh session has acknowledged nothing");

    sender.NoteOrderAccepted(42);
    Assert::AreEqual<std::uint32_t>(42, sender.LastOrderSeqProcessed());

    // An older sequence arriving late must not wind it back: the client reads
    // this to decide what is still outstanding, so a rewind would un-promote a
    // ghost that had already been promoted.
    sender.NoteOrderAccepted(7);
    Assert::AreEqual<std::uint32_t>(42, sender.LastOrderSeqProcessed(), L"a late order rewound the high-water mark");

    sender.NoteOrderAccepted(43);
    Assert::AreEqual<std::uint32_t>(43, sender.LastOrderSeqProcessed());
  }

  TEST_METHOD(TwoViewersKeepTheirOwnOrderSequences)
  {
    /*
     * The whole reason §7 moved the field. World-global, one commander's
     * counter set the number every *other* commander's ghosts were promoted
     * against -- so a busy player could promote a quiet player's ghosts for
     * orders that had never been given, and a replay's hash depended on which
     * client happened to submit first.
     *
     * Two senders, two numbers, no shared state. This is the assertion the
     * world-owned version could not have passed.
     */
    SnapshotSender busy{1, INVALID_CONNECTION, 0};
    SnapshotSender quiet{2, INVALID_CONNECTION, 0};

    busy.NoteOrderAccepted(900000);
    quiet.NoteOrderAccepted(3);

    Assert::AreEqual<std::uint32_t>(900000, busy.LastOrderSeqProcessed());
    Assert::AreEqual<std::uint32_t>(3, quiet.LastOrderSeqProcessed(), L"one commander's counter reached another's feed");
  }

  TEST_METHOD(ARefusedOrderDoesNotAdvanceTheSequenceTheSnapshotReports)
  {
    /*
     * Accepted only, over the real loopback (ADR-022 §7).
     *
     * The field is what still promotes a client's ghost when the `OrderAck` is
     * lost, so advancing it on a *refusal* would promote a ghost the authority
     * turned down -- the exact opposite of what it is for. The world's version
     * had this property for free, because only accepted orders ever reached
     * `IngestOrders`; the session's version has to be told, and this is what
     * says it was.
     */
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter hello{buffer};
    WriteWireType(hello, WireType::Hello);
    Write(hello, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, hello.Written()));

    bool joined = false;
    Assert::IsTrue(WaitUntil(client,
                             [&]
                             {
                               TransportEvent event;
                               while (client.NextEvent(event))
                               {
                                 if (event.type != TransportEvent::Type::Message)
                                 {
                                   continue;
                                 }
                                 ByteReader reader{event.payload};
                                 Welcome welcome;
                                 if (ReadWireType(reader) == WireType::Welcome && Read(reader, welcome))
                                 {
                                   joined = true;
                                 }
                               }
                               return joined;
                             }),
                   L"the server never sent Welcome");

    constexpr std::uint32_t GOOD_SEQ = 100;
    constexpr std::uint32_t REFUSED_SEQ = 900000; // Far higher, so a leak is loud.

    ByteWriter accepted{buffer};
    WriteWireType(accepted, WireType::OrderSubmit);
    accepted.WriteUInt32(CountingSimulation::ORDER_MARKER);
    accepted.WriteUInt32(GOOD_SEQ);
    Assert::IsTrue(client.Send(link, TransportChannel::Control, accepted.Written()));

    // A payload this simulation does not recognise, which is how a refusal is
    // produced here without the engine knowing what makes one.
    ByteWriter rejected{buffer};
    WriteWireType(rejected, WireType::OrderSubmit);
    rejected.WriteUInt32(CountingSimulation::ORDER_MARKER + 1);
    rejected.WriteUInt32(REFUSED_SEQ);
    Assert::IsTrue(client.Send(link, TransportChannel::Control, rejected.Written()));

    int acks = 0;
    std::uint32_t highest = 0;
    bool sawTheAcceptedOne = false;
    const bool settled = WaitUntil(client,
                                   [&]
                                   {
                                     TransportEvent event;
                                     while (client.NextEvent(event))
                                     {
                                       if (event.type != TransportEvent::Type::Message)
                                       {
                                         continue;
                                       }
                                       ByteReader reader{event.payload};
                                       const WireType type = ReadWireType(reader);
                                       OrderAck ack;
                                       if (type == WireType::OrderAck && Read(reader, ack))
                                       {
                                         ++acks;
                                       }
                                       else if (type == WireType::Snapshot || type == WireType::Keyframe)
                                       {
                                         const FrameFacts facts = ReadFrame(type, reader);
                                         AckFrame(client, link, facts);
                                         if (facts.hasTail && facts.marker == CountingSimulation::SNAPSHOT_MARKER &&
                                             facts.tailTick > 0)
                                         {
                                           highest = std::max(highest, facts.sequence);
                                           sawTheAcceptedOne = sawTheAcceptedOne || facts.sequence == GOOD_SEQ;
                                         }
                                       }
                                     }
                                     // Both acks and a snapshot made after them,
                                     // or the refusal might simply not have been
                                     // processed yet.
                                     return acks >= 2 && sawTheAcceptedOne;
                                   });

    Assert::IsTrue(settled, L"the two orders were never both answered");
    Assert::AreEqual<std::uint32_t>(GOOD_SEQ, highest, L"a refused order advanced the viewer's high-water mark");
    Assert::AreEqual<std::uint32_t>(1, host.RefusedOrderCount());

    host.Stop();
    host.Join();
  }

  TEST_METHOD(ARefusedOrderBouncesWithTheGamesOwnReason)
  {
    // The engine passes a reason code it cannot interpret. This simulation's
    // enum is a number no game uses, which is the point: if the value arrives
    // intact, nothing in between looked at it.
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter hello{buffer};
    WriteWireType(hello, WireType::Hello);
    Write(hello, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, hello.Written()));

    bool joined = false;
    Assert::IsTrue(WaitUntil(client,
                             [&]
                             {
                               TransportEvent event;
                               while (client.NextEvent(event))
                               {
                                 if (event.type != TransportEvent::Type::Message)
                                 {
                                   continue;
                                 }
                                 ByteReader reader{event.payload};
                                 Welcome welcome;
                                 if (ReadWireType(reader) == WireType::Welcome && Read(reader, welcome))
                                 {
                                   joined = true;
                                 }
                               }
                               return joined;
                             }));

    constexpr std::uint32_t ORDER_SEQ = 8;
    ByteWriter bad{buffer};
    WriteWireType(bad, WireType::OrderSubmit);
    bad.WriteUInt32(0xdeadbeefu); // Not the marker this simulation accepts.
    bad.WriteUInt32(ORDER_SEQ);
    Assert::IsTrue(client.Send(link, TransportChannel::Control, bad.Written()));

    OrderAck ack;
    bool acked = false;
    Assert::IsTrue(WaitUntil(client,
                             [&]
                             {
                               TransportEvent event;
                               while (client.NextEvent(event))
                               {
                                 if (event.type != TransportEvent::Type::Message)
                                 {
                                   continue;
                                 }
                                 ByteReader reader{event.payload};
                                 if (ReadWireType(reader) == WireType::OrderAck && Read(reader, ack))
                                 {
                                   acked = true;
                                 }
                               }
                               return acked;
                             }),
                   L"a refusal must come back; a lost one leaves a ghost on screen forever");

    Assert::AreEqual<std::uint8_t>(0, ack.accepted);
    Assert::AreEqual<std::uint16_t>(CountingSimulation::REFUSED_REASON, ack.reasonCode);
    Assert::AreEqual<std::uint32_t>(ORDER_SEQ, ack.orderSeq, L"a refusal still says which order it was about");
    Assert::AreEqual<std::uint32_t>(0, ack.serverOrderId, L"nothing refused is given an id");

    Assert::IsTrue(WaitUntil(client, [&] { return host.RefusedOrderCount() == 1; }));

    host.Stop();
    host.Join();
  }

  TEST_METHOD(AnOrderBeforeTheHandshakeIsDroppedRatherThanAcked)
  {
    // There is no client id to attribute it to and no agreement that the two
    // builds share a schema. Acking would imply it was considered.
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 128> buffer{};
    ByteWriter order{buffer};
    WriteWireType(order, WireType::OrderSubmit);
    order.WriteUInt32(CountingSimulation::ORDER_MARKER);
    order.WriteUInt32(1);
    Assert::IsTrue(client.Send(link, TransportChannel::Control, order.Written()));

    bool acked = false;
    (void)WaitUntil(client,
                    [&]
                    {
                      TransportEvent event;
                      while (client.NextEvent(event))
                      {
                        if (event.type == TransportEvent::Type::Message)
                        {
                          ByteReader reader{event.payload};
                          if (ReadWireType(reader) == WireType::OrderAck)
                          {
                            acked = true;
                          }
                        }
                      }
                      return acked;
                    },
                    400.0);

    Assert::IsFalse(acked, L"an order from a connection that never joined must not be acked");
    Assert::AreEqual<std::uint32_t>(0, simulation.OrdersSeen(), L"nor reach the simulation");

    host.Stop();
    host.Join();
  }

  TEST_METHOD(RefusesAMismatchedBuildAtTheDoor)
  {
    // Fail closed: a client built against different content is turned away with
    // a reason, not allowed in to disagree quietly (ADR-004 §3).
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), 0xdeadbeef, "wrong-content"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    UpdateRequired update;
    bool refused = false;
    const bool answered = WaitUntil(client,
                                    [&]
                                    {
                                      TransportEvent event;
                                      while (client.NextEvent(event))
                                      {
                                        if (event.type != TransportEvent::Type::Message)
                                        {
                                          continue;
                                        }
                                        ByteReader reader{event.payload};
                                        if (ReadWireType(reader) == WireType::UpdateRequired && Read(reader, update))
                                        {
                                          refused = true;
                                        }
                                      }
                                      return refused;
                                    });

    Assert::IsTrue(answered, L"the server accepted a mismatched build, or said nothing");
    Assert::AreEqual(simulation.ContentHash(), update.serverContentHash);
    Assert::AreEqual<std::uint32_t>(0, host.SessionCount());

    host.Stop();
    host.Join();
  }
};

} // namespace NeuronServerTests
