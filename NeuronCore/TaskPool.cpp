#include "pch.h"

#include "TaskPool.h"

#include "Log.h"
#include "Telemetry.h"

#include <algorithm>
#include <cstdio>
#include <exception>

namespace Neuron
{
namespace
{

/*
 * Lanes the pool leaves alone, for the roles that are not workers.
 *
 * Every worker registers its own numbered telemetry lane, because two threads
 * sharing one would break the ring's single-producer contract (Telemetry.h).
 * That makes the lane table a real ceiling on the pool, and the engine's own
 * roles -- "Main" in the client, "Sim" in the server host -- claim theirs
 * first. Four is those two and a spare each.
 */
constexpr std::uint32_t RESERVED_LANES = 4;

/// The most workers the default sizing will ask for. Derived from the lane
/// table so it cannot drift away from it.
constexpr std::uint32_t MAX_DEFAULT_WORKERS = Telemetry::MAX_LANES - RESERVED_LANES;

} // namespace

TaskPool::~TaskPool()
{
  Stop();
}

/*
 * Workers for a default-sized pool.
 *
 * One per core less the caller's, capped at the lane budget. Without the cap a
 * large machine asks for more lanes than exist and the surplus workers are
 * refused one error line each at every startup -- threads that run tasks but
 * cannot be measured, which is the silent-partial-measurement the registry
 * exists to refuse.
 *
 * The cap costs nothing that is being used: this pool is the boot bake pool,
 * and its widest batch is the nine OBJ files.
 */
std::uint32_t TaskPool::DefaultWorkerCount() noexcept
{
  const unsigned hardware = std::thread::hardware_concurrency();
  if (hardware <= 1)
  {
    return 0; // One core: the submitting thread is the pool.
  }
  const auto leaveOneForTheCaller = static_cast<std::uint32_t>(hardware - 1);
  return std::min(leaveOneForTheCaller, MAX_DEFAULT_WORKERS);
}

bool TaskPool::Start(std::uint32_t _workerCount)
{
  if (m_running.load(std::memory_order_acquire))
  {
    return false;
  }

  const std::uint32_t count = _workerCount == 0 ? DefaultWorkerCount() : _workerCount;

  m_stopRequested.store(false, std::memory_order_release);
  m_running.store(true, std::memory_order_release);
  m_completed.store(0, std::memory_order_relaxed);

  m_workers.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i)
  {
    m_workers.emplace_back(&TaskPool::WorkerLoop, this, i);
  }

  NEURON_LOG_INFO("task pool started with %u worker(s)", count);
  return true;
}

void TaskPool::Stop()
{
  if (!m_running.load(std::memory_order_acquire))
  {
    return;
  }

  // Queued work is finished, not abandoned: a half-baked atlas is worse than a
  // slightly slower shutdown, and every caller of this is at boot.
  Drain();

  /*
   * Under the mutex, and it has to be.
   *
   * A worker that has just evaluated its predicate as false still HOLDS this
   * mutex, and does not release it until it is inside the wait. Publishing the
   * flag beside that -- atomically, but not under the lock -- lands in the gap:
   * the worker checks (false), Stop stores and notifies with nobody yet
   * waiting, and only then does the worker block, on a condition that will
   * never be signalled again. Every task has run and the pool still never
   * stops. Taking the lock makes the gap unreachable, because the store cannot
   * happen while a worker is in it.
   *
   * The notify stays outside: it costs the woken worker a second wait for a
   * mutex the notifier is still holding, and it is not what makes this correct.
   */
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_stopRequested.store(true, std::memory_order_release);
  }
  m_wakeUp.notify_all();

  for (std::thread& worker : m_workers)
  {
    if (worker.joinable())
    {
      worker.join();
    }
  }
  m_workers.clear();
  m_running.store(false, std::memory_order_release);

  NEURON_LOG_INFO("task pool stopped after %llu task(s)", static_cast<unsigned long long>(CompletedCount()));
}

void TaskPool::Submit(Task _task, WaitGroup* _group)
{
  if (!_task)
  {
    return;
  }

  // The group is incremented here rather than by the caller, so a task can
  // never run before the count that is meant to cover it exists.
  if (_group != nullptr)
  {
    _group->Add(1);
  }

  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(Job{std::move(_task), _group});
  }
  m_wakeUp.notify_one();
}

bool TaskPool::TryTakeJob(Job& _outJob)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (m_queue.empty())
  {
    return false;
  }
  _outJob = std::move(m_queue.front());
  m_queue.pop_front();
  return true;
}

void TaskPool::RunJob(Job& _job) noexcept
{
  // A task that throws must not take a worker thread with it: the pool would
  // lose a thread permanently and the WaitGroup would never reach zero, which
  // presents as a hang at boot with no message.
  try
  {
    _job.task();
  }
  catch (const std::exception& error)
  {
    NEURON_LOG_ERROR("task threw: %s", error.what());
  }
  catch (...)
  {
    NEURON_LOG_ERROR("task threw a non-standard exception");
  }

  if (_job.group != nullptr)
  {
    _job.group->Done();
  }
}

void TaskPool::WorkerLoop(std::uint32_t _index)
{
  char laneName[32];
  std::snprintf(laneName, sizeof(laneName), "Worker %u", _index);
  (void)Telemetry::RegisterLane(laneName);

  for (;;)
  {
    Job job;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_wakeUp.wait(lock, [&] { return !m_queue.empty() || m_stopRequested.load(std::memory_order_acquire); });

      if (m_queue.empty())
      {
        if (m_stopRequested.load(std::memory_order_acquire))
        {
          return;
        }
        continue; // Spurious wake-up.
      }
      job = std::move(m_queue.front());
      m_queue.pop_front();
    }

    NEURON_SPAN("Task");
    RunJob(job);
    m_completed.fetch_add(1, std::memory_order_relaxed);
  }
}

void TaskPool::Wait(WaitGroup& _group)
{
  // Run rather than block. With no workers this is the only thing that makes
  // progress; with workers it stops a task that submits and waits from sitting
  // on a thread the child needs.
  while (!_group.Finished())
  {
    Job job;
    if (TryTakeJob(job))
    {
      RunJob(job);
      m_completed.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    if (m_workers.empty())
    {
      // Nothing queued, nobody to queue it, and the group is not finished:
      // the count is wrong. Returning beats spinning forever on a bad caller.
      NEURON_LOG_ERROR("wait group has %u outstanding with an empty queue and no workers", _group.Outstanding());
      return;
    }

    std::this_thread::yield(); // A worker holds the remaining task.
  }
}

void TaskPool::Drain()
{
  Job job;
  while (TryTakeJob(job))
  {
    RunJob(job);
    m_completed.fetch_add(1, std::memory_order_relaxed);
  }
}

} // namespace Neuron
