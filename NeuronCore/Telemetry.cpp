#include "pch.h"

#include "Telemetry.h"

#include "Hash.h"
#include "Log.h"

#include <atomic>
#include <cstring>

namespace Neuron
{
namespace
{

/// The calling thread's lane. thread_local so recording costs no lookup.
thread_local LaneId t_lane = INVALID_LANE;

/// Case-sensitive compare against a lane's fixed-size name buffer.
[[nodiscard]] bool NameMatches(const char* _laneName, std::string_view _name) noexcept
{
  const std::size_t length = std::strlen(_laneName);
  return length == _name.size() && std::memcmp(_laneName, _name.data(), length) == 0;
}

} // namespace

Telemetry::LaneTable& Telemetry::Table() noexcept
{
  static LaneTable table;
  return table;
}

LaneId Telemetry::RegisterLane(std::string_view _name) noexcept
{
  if (t_lane != INVALID_LANE)
  {
    return t_lane; // Already registered: registering twice is a mistake, not a second lane.
  }

  LaneTable& table = Table();

  // A lane is a role. If this name already has one, adopt it -- a task pool
  // that stops and starts again must reuse "Worker 0", not spend another lane
  // on it, or a handful of restarts exhausts the cap.
  const std::uint32_t existing = table.published.load(std::memory_order_acquire);
  for (std::uint32_t i = 0; i < existing; ++i)
  {
    if (NameMatches(table.lanes[i].name, _name))
    {
      table.lanes[i].threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
      t_lane = i;
      return i;
    }
  }

  const std::uint32_t index = table.claimed.fetch_add(1, std::memory_order_relaxed);
  if (index >= MAX_LANES)
  {
    // The corpus's client budget is 8 and its HUD refuses a ninth; this refuses
    // a seventeenth the same way -- loudly, at startup, rather than by quietly
    // dropping the lane's measurements for the rest of the run.
    NEURON_LOG_ERROR("telemetry: no lane left for '%.*s' (cap is %u)", static_cast<int>(_name.size()), _name.data(), MAX_LANES);
    table.claimed.store(MAX_LANES, std::memory_order_relaxed);
    return INVALID_LANE;
  }

  Lane& lane = table.lanes[index];
  const std::size_t length = _name.size() < sizeof(lane.name) - 1 ? _name.size() : sizeof(lane.name) - 1;
  std::memcpy(lane.name, _name.data(), length);
  lane.name[length] = '\0';
  lane.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());

  // Publish only after the slot is filled: a collector walking lanes must never
  // see a name that is still being written.
  table.published.store(index + 1, std::memory_order_release);

  t_lane = index;
  NEURON_LOG_DEBUG("telemetry: lane %u is '%s' (thread %u)", index, lane.name, lane.threadId);
  return index;
}

LaneId Telemetry::CurrentLane() noexcept
{
  return t_lane;
}

Telemetry::Lane* Telemetry::LaneAt(LaneId _lane) noexcept
{
  LaneTable& table = Table();
  return _lane < table.published.load(std::memory_order_acquire) ? &table.lanes[_lane] : nullptr;
}

void Telemetry::Record(TelemetryKind _kind, const char* _name, std::int64_t _value) noexcept
{
  Lane* lane = LaneAt(t_lane);
  if (lane == nullptr)
  {
    return; // An unregistered thread measures nothing; it does not fail.
  }
  (void)lane->events.TryPush(TelemetryEvent{_name, _value, _kind}); // A full lane drops and counts.
}

std::uint32_t Telemetry::LaneCount() noexcept
{
  return Table().published.load(std::memory_order_acquire);
}

const char* Telemetry::LaneName(LaneId _lane) noexcept
{
  const Lane* lane = LaneAt(_lane);
  return lane == nullptr ? "" : lane->name;
}

std::uint64_t Telemetry::DroppedCount(LaneId _lane) noexcept
{
  const Lane* lane = LaneAt(_lane);
  return lane == nullptr ? 0 : lane->events.DroppedCount();
}

bool Telemetry::Drain(LaneId _lane, TelemetryEvent& _outEvent) noexcept
{
  Lane* lane = LaneAt(_lane);
  return lane != nullptr && lane->events.TryPop(_outEvent);
}

void Telemetry::Reset() noexcept
{
  LaneTable& table = Table();
  table.published.store(0, std::memory_order_release);
  // Note what this cannot do: other threads cache their lane id in t_lane, and
  // this does not reach them. It is a test affordance, not a runtime one.
  table.claimed.store(0, std::memory_order_relaxed);
  for (Lane& lane : table.lanes)
  {
    TelemetryEvent discarded;
    while (lane.events.TryPop(discarded))
    {
    }
    lane.name[0] = '\0';
    lane.threadId = 0;
  }
  t_lane = INVALID_LANE;
}

void TelemetrySnapshot::DrainLane(LaneId _lane) noexcept
{
  TelemetryEvent event;
  while (Telemetry::Drain(_lane, event))
  {
    if (event.name == nullptr)
    {
      continue;
    }

    const std::uint64_t hash = HashText(event.name);
    TelemetryStat* stat = nullptr;
    for (std::size_t i = 0; i < m_statCount; ++i)
    {
      if (m_stats[i].nameHash == hash && m_stats[i].kind == event.kind)
      {
        stat = &m_stats[i];
        break;
      }
    }

    if (stat == nullptr)
    {
      if (m_statCount == MAX_STATS)
      {
        ++m_overflow; // Counted, not silently merged into someone else's row.
        continue;
      }
      stat = &m_stats[m_statCount++];
      stat->name = event.name;
      stat->nameHash = hash;
      stat->kind = event.kind;
      stat->count = 0;
      stat->total = 0;
      stat->maximum = 0;
    }

    ++stat->count;
    stat->total += event.value;
    if (event.value > stat->maximum)
    {
      stat->maximum = event.value; // The worst frame is the interesting one.
    }
  }
}

void TelemetrySnapshot::DrainAll() noexcept
{
  const std::uint32_t lanes = Telemetry::LaneCount();
  for (LaneId lane = 0; lane < lanes; ++lane)
  {
    DrainLane(lane);
  }
}

void TelemetrySnapshot::Clear() noexcept
{
  m_statCount = 0;
  m_overflow = 0;
}

const TelemetryStat* TelemetrySnapshot::Find(std::string_view _name) const noexcept
{
  const std::uint64_t hash = HashText(_name);
  for (std::size_t i = 0; i < m_statCount; ++i)
  {
    if (m_stats[i].nameHash == hash)
    {
      return &m_stats[i];
    }
  }
  return nullptr;
}

double TelemetrySnapshot::Milliseconds(std::int64_t _counterTicks) noexcept
{
  const std::int64_t frequency = Clock::Frequency();
  return frequency == 0 ? 0.0 : (static_cast<double>(_counterTicks) * 1000.0) / static_cast<double>(frequency);
}

} // namespace Neuron
