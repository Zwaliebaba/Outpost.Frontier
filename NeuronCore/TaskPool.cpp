#include "pch.h"

#include "TaskPool.h"

#include "Log.h"
#include "Telemetry.h"

#include <cstdio>
#include <exception>

namespace Neuron
{

TaskPool::~TaskPool()
{
  Stop();
}

std::uint32_t TaskPool::DefaultWorkerCount() noexcept
{
  const unsigned hardware = std::thread::hardware_concurrency();
  if (hardware <= 1)
  {
    return 0; // One core: the submitting thread is the pool.
  }
  return static_cast<std::uint32_t>(hardware - 1); // Leave the caller its core.
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

  m_stopRequested.store(true, std::memory_order_release);
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
