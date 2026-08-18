#pragma once

#include "Clock.h"
#include "RingBuffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

/*
 * Per-thread telemetry lanes (ADR-007 §8).
 *
 * A thread registers a named lane at startup and thereafter records spans and
 * counters into that lane's ring. One collector -- the Main thread in MVP --
 * drains every lane and aggregates. The producer is the lane's owner and the
 * consumer is the collector, which is exactly the SPSC contract: no locks, and
 * a thread that is late to be drained loses its oldest events rather than
 * stalling the thread that produced them.
 *
 * This exists from day one rather than when the debug HUD needs it, because the
 * corpus reports frame budgets as named stage rows and retrofitting measurement
 * means retrofitting the stage boundaries too. The MVP cost is an array and two
 * macros; ADR-007 §8 is the argument.
 *
 * It stays compiled in for Release. `tickOverrun` is a shipping counter
 * (ADR-002), and a measurement channel that only exists in Debug measures a
 * build nobody runs.
 *
 * What it is not: a profiler, and not a log. Names are string literals, values
 * are integers, and there is no formatting anywhere on the recording path.
 */

namespace Neuron
{

enum class TelemetryKind : std::uint8_t
{
  Span,   // A duration in Clock::Counter() ticks.
  Counter // An amount to add up: events, drops, bytes.
};

/// One recorded measurement. Trivially copyable by construction -- it crosses a
/// thread boundary, and `name` points at a string literal rather than at
/// anything with a lifetime.
struct TelemetryEvent
{
  const char* name = nullptr;
  std::int64_t value = 0;
  TelemetryKind kind = TelemetryKind::Counter;
};

/// Per-name aggregate produced by draining. Spans keep their totals in counter
/// ticks; converting to milliseconds is the reader's job (TelemetrySnapshot).
struct TelemetryStat
{
  const char* name = nullptr;
  std::uint64_t nameHash = 0;
  TelemetryKind kind = TelemetryKind::Counter;
  std::uint32_t count = 0;
  std::int64_t total = 0;
  std::int64_t maximum = 0;
};

using LaneId = std::uint32_t;
inline constexpr LaneId InvalidLane = 0xffffffffu;

/*
 * The registry.
 *
 * Capped and static: the corpus's client budget is 8 lanes and its debug HUD
 * "fails at startup if it asks for a ninth" (ADR-007). The cap here is 16 so a
 * server's lanes fit the same array, and asking for one too many is a refusal
 * that says so rather than a silent resize.
 */
class Telemetry
{
public:
  static constexpr std::uint32_t MaxLanes = 16;
  static constexpr std::size_t LaneCapacity = 512; // Events per lane between drains.

  /*
   * Registers the calling thread under a lane name.
   *
   * A lane is a **role, not a thread**: "Main", "Sim", "Worker 0". Registering
   * a name that already has a lane adopts that lane rather than taking a new
   * one, so a task pool that is stopped and started again reuses its lanes
   * instead of eating the cap a restart at a time.
   *
   * The rule that makes this safe is that a name has one live owner. Two
   * threads registering the same name at once would share a ring and break the
   * single-producer contract, which is why the pool numbers its workers.
   */
  static LaneId RegisterLane(std::string_view _name) noexcept;

  /// For threads we do not own -- msquic workers, XAudio2 callbacks. Identical
  /// mechanism; named separately because the call site means something
  /// different, and because a foreign thread must register before it records.
  static LaneId RegisterExternalThread(std::string_view _name) noexcept { return RegisterLane(_name); }

  /// The calling thread's lane, or InvalidLane if it never registered.
  [[nodiscard]] static LaneId CurrentLane() noexcept;

  /// Records into the calling thread's lane. An unregistered thread is a
  /// no-op: telemetry never decides whether a thread may run.
  static void Record(TelemetryKind _kind, const char* _name, std::int64_t _value) noexcept;

  [[nodiscard]] static std::uint32_t LaneCount() noexcept;
  [[nodiscard]] static const char* LaneName(LaneId _lane) noexcept;

  /// Events dropped because a lane filled before the collector drained it.
  /// A rising count means the collector is too slow or the lane too chatty --
  /// either way it is a measurement about the measurement, and it is reported.
  [[nodiscard]] static std::uint64_t DroppedCount(LaneId _lane) noexcept;

  /// Collector side: takes one event from a lane. False when the lane is empty.
  [[nodiscard]] static bool Drain(LaneId _lane, TelemetryEvent& _outEvent) noexcept;

  /// Forgets every lane. For tests only, and only when the calling thread is
  /// the last one still registered -- other threads keep a cached lane id that
  /// this invalidates.
  static void Reset() noexcept;

private:
  struct Lane
  {
    char name[32]{};
    std::uint32_t threadId = 0;
    RingBuffer<TelemetryEvent, LaneCapacity> events;
  };

  /*
   * The lane table is a fixed array, deliberately: lanes are registered at
   * startup and never removed, so the only thing to synchronise is the append.
   * A registering thread claims an index with one fetch_add and then fills its
   * slot; readers only look below the published count, so they cannot observe a
   * lane that is still being written. The rings hold atomics and so cannot be
   * moved, which is the other reason this is an array and not a vector.
   */
  struct LaneTable
  {
    Lane lanes[MaxLanes];
    std::atomic<std::uint32_t> claimed{0};   // Indices handed out.
    std::atomic<std::uint32_t> published{0}; // Indices finished being filled in.
  };

  [[nodiscard]] static LaneTable& Table() noexcept;
  [[nodiscard]] static Lane* LaneAt(LaneId _lane) noexcept;
};

/*
 * A drain plus aggregation, which is what every reader actually wants: the
 * debug HUD shows "GAME 1.2 ms", not four hundred span events.
 *
 * Fixed capacity and a linear scan: at a few dozen distinct names that beats a
 * map, and it cannot allocate while the collector is walking lanes. Names are
 * matched by hash rather than by pointer, because string pooling is a compiler
 * option and two identical literals in two translation units are not required
 * to share an address.
 */
class TelemetrySnapshot
{
public:
  static constexpr std::size_t MaxStats = 64;

  /// Drains one lane into the aggregate. Repeated calls accumulate, so a
  /// collector can walk every lane and then read the totals once.
  void DrainLane(LaneId _lane) noexcept;

  /// Drains every registered lane.
  void DrainAll() noexcept;

  void Clear() noexcept;

  [[nodiscard]] std::span<const TelemetryStat> Stats() const noexcept { return {m_stats, m_statCount}; }

  /// The named stat, or nullptr. For a HUD row or a test.
  [[nodiscard]] const TelemetryStat* Find(std::string_view _name) const noexcept;

  /// Events that arrived after the aggregate was full. Distinct from a lane's
  /// own drops: this one says the snapshot ran out of names, not the ring.
  [[nodiscard]] std::uint32_t OverflowCount() const noexcept { return m_overflow; }

  /// Counter ticks to milliseconds. Only meaningful for spans.
  [[nodiscard]] static double Milliseconds(std::int64_t _counterTicks) noexcept;

private:
  TelemetryStat m_stats[MaxStats]{};
  std::size_t m_statCount = 0;
  std::uint32_t m_overflow = 0;
};

/// Times its scope and records the duration when it ends. Use NEURON_SPAN.
class TelemetrySpan
{
public:
  explicit TelemetrySpan(const char* _name) noexcept
    : m_name(_name)
    , m_start(Clock::Counter())
  {
  }

  ~TelemetrySpan() noexcept { Telemetry::Record(TelemetryKind::Span, m_name, Clock::Counter() - m_start); }

  TelemetrySpan(const TelemetrySpan&) = delete;
  TelemetrySpan& operator=(const TelemetrySpan&) = delete;

private:
  const char* m_name;
  std::int64_t m_start;
};

} // namespace Neuron

#define NEURON_TELEMETRY_JOIN2(_a, _b) _a##_b
#define NEURON_TELEMETRY_JOIN(_a, _b) NEURON_TELEMETRY_JOIN2(_a, _b)

/// Times the enclosing scope under a literal name: NEURON_SPAN("Tick").
#define NEURON_SPAN(_name) const ::Neuron::TelemetrySpan NEURON_TELEMETRY_JOIN(neuronSpan, __LINE__){_name}

/// Adds to a named running total: NEURON_COUNTER("TickOverrun", 1).
#define NEURON_COUNTER(_name, _value) ::Neuron::Telemetry::Record(::Neuron::TelemetryKind::Counter, _name, (_value))
