#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/*
 * The boot task pool (ADR-007 §4).
 *
 * A fixed pool with Submit and WaitGroup, for startup work only: OBJ loads, the
 * DirectWrite glyph-atlas bake. The sim tick and the render frame are
 * single-threaded **by rule** in MVP -- the first parallel consumer inside a
 * frame or a tick has to bring the deterministic-partitioning story (ADR-005
 * §5) and a profile that justifies it. This pool is not that, and must not
 * quietly become it.
 *
 * Which is why it is built the boring way: a mutex, a condition variable, and a
 * std::deque of std::function. Each task costs an allocation and a lock, and at
 * a few hundred boot tasks that is invisible. A lock-free queue here would be
 * fixed-capacity and would therefore have to drop work, which for a mesh load
 * is not a trade-off, it is a bug.
 *
 * Workers register telemetry lanes ("Worker 0", ...), so boot cost shows up in
 * the same rails as everything else (ADR-007 §8).
 */

namespace Neuron
{

/*
 * Counts outstanding work so a caller can wait for a batch.
 *
 * Wait() is on TaskPool rather than here, because a WaitGroup that blocks
 * cannot help run the very tasks it is waiting for, and that deadlocks a pool
 * with no workers. TaskPool::Wait runs tasks while it waits.
 */
class WaitGroup
{
public:
  WaitGroup() = default;

  WaitGroup(const WaitGroup&) = delete;
  WaitGroup& operator=(const WaitGroup&) = delete;

  void Add(std::uint32_t _count = 1) noexcept { m_outstanding.fetch_add(_count, std::memory_order_relaxed); }

  void Done() noexcept { m_outstanding.fetch_sub(1, std::memory_order_release); }

  [[nodiscard]] std::uint32_t Outstanding() const noexcept { return m_outstanding.load(std::memory_order_acquire); }

  [[nodiscard]] bool Finished() const noexcept { return Outstanding() == 0; }

private:
  std::atomic<std::uint32_t> m_outstanding{0};
};

class TaskPool
{
public:
  using Task = std::function<void()>;

  TaskPool() = default;
  ~TaskPool();

  TaskPool(const TaskPool&) = delete;
  TaskPool& operator=(const TaskPool&) = delete;

  /// Starts _workerCount threads. Zero asks for hardware_concurrency - 1
  /// capped at the telemetry lane budget (see DefaultWorkerCount), so the
  /// calling thread is not counted twice; the result may still be zero on a
  /// single-core machine, which is a supported configuration -- Submit still
  /// queues and Wait still runs the work inline.
  ///
  /// An explicit count is taken as given. Asking for more workers than there
  /// are free lanes is legal and costs the surplus workers their measurements,
  /// which the registry says so about, loudly, once each.
  [[nodiscard]] bool Start(std::uint32_t _workerCount = 0);

  /// Finishes what is queued, then joins. Safe to call more than once.
  void Stop();

  /// Queues a task. If _group is given, it is incremented now and decremented
  /// when the task has finished -- taken here rather than by the caller so the
  /// two can never disagree.
  void Submit(Task _task, WaitGroup* _group = nullptr);

  /// Runs queued tasks on the calling thread until the group is finished. The
  /// caller is a worker for the duration, which is what makes a zero-worker
  /// pool work and what stops a task that submits from deadlocking its parent.
  void Wait(WaitGroup& _group);

  /// Runs queued tasks until there are none left. Used by Stop.
  void Drain();

  [[nodiscard]] std::uint32_t WorkerCount() const noexcept { return static_cast<std::uint32_t>(m_workers.size()); }
  [[nodiscard]] bool Running() const noexcept { return m_running.load(std::memory_order_acquire); }

  /// Tasks completed since Start. A counter, not a gauge.
  [[nodiscard]] std::uint64_t CompletedCount() const noexcept { return m_completed.load(std::memory_order_relaxed); }

  /// How many threads to start when Start is asked for zero: one per core less
  /// the caller's, capped so the pool cannot claim more telemetry lanes than
  /// the registry has left for it.
  [[nodiscard]] static std::uint32_t DefaultWorkerCount() noexcept;

private:
  struct Job
  {
    Task task;
    WaitGroup* group = nullptr;
  };

  void WorkerLoop(std::uint32_t _index);

  /// Takes one job if there is one. False means the queue was empty.
  [[nodiscard]] bool TryTakeJob(Job& _outJob);

  static void RunJob(Job& _job) noexcept;

  std::vector<std::thread> m_workers;
  std::deque<Job> m_queue;
  mutable std::mutex m_mutex;
  std::condition_variable m_wakeUp;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stopRequested{false};
  std::atomic<std::uint64_t> m_completed{0};
};

} // namespace Neuron
