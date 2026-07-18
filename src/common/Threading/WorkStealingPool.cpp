/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "WorkStealingPool.h"

// WorkerQueue implementation

void WorkStealingPool::WorkerQueue::Push(Task task)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _tasks.push_back(std::move(task));
}

bool WorkStealingPool::WorkerQueue::Pop(Task& task)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_tasks.empty())
        return false;

    // LIFO pop from back (cache friendly for owner)
    task = std::move(_tasks.back());
    _tasks.pop_back();
    return true;
}

bool WorkStealingPool::WorkerQueue::Steal(Task& task)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_tasks.empty())
        return false;

    // FIFO steal from front (fairness for thief)
    task = std::move(_tasks.front());
    _tasks.pop_front();
    return true;
}

bool WorkStealingPool::WorkerQueue::Empty() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _tasks.empty();
}

std::size_t WorkStealingPool::WorkerQueue::Size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _tasks.size();
}

// WorkStealingPool implementation

WorkStealingPool::WorkStealingPool(std::size_t numThreads)
    : _numThreads(numThreads > 0 ? numThreads : std::thread::hardware_concurrency())
{
}

WorkStealingPool::~WorkStealingPool()
{
    Deactivate();
}

void WorkStealingPool::Activate()
{
    if (_active.exchange(true, std::memory_order_acq_rel))
        return; // Already active

    _stopping.store(false, std::memory_order_release);

    // Create per-worker queues
    _queues.reserve(_numThreads);
    for (std::size_t i = 0; i < _numThreads; ++i)
        _queues.push_back(std::make_unique<WorkerQueue>());

    // Start worker threads
    _workers.reserve(_numThreads);
    for (std::size_t i = 0; i < _numThreads; ++i)
        _workers.emplace_back(&WorkStealingPool::WorkerThread, this, i);
}

void WorkStealingPool::Deactivate()
{
    if (!_active.exchange(false, std::memory_order_acq_rel))
        return; // Already inactive

    // Signal workers to stop
    _stopping.store(true, std::memory_order_release);

    // Wake all workers
    {
        std::lock_guard<std::mutex> lock(_wakeMutex);
        _wakeCondition.notify_all();
    }

    // Join all workers
    for (auto& worker : _workers)
    {
        if (worker.joinable())
            worker.join();
    }

    _workers.clear();
    _queues.clear();
}

void WorkStealingPool::Submit(Task task)
{
    if (!_active.load(std::memory_order_acquire))
        return;

    // Round-robin distribution to queues
    std::size_t queueIndex = _nextQueue.fetch_add(1, std::memory_order_relaxed) % _numThreads;

    _pendingTasks.fetch_add(1, std::memory_order_release);
    _queues[queueIndex]->Push(std::move(task));

    // Wake a worker
    {
        std::lock_guard<std::mutex> lock(_wakeMutex);
        _wakeCondition.notify_one();
    }
}

void WorkStealingPool::SubmitBatch(std::vector<Task>& tasks)
{
    if (!_active.load(std::memory_order_acquire) || tasks.empty())
        return;

    std::size_t tasksPerQueue = (tasks.size() + _numThreads - 1) / _numThreads;
    std::size_t taskIndex = 0;

    _pendingTasks.fetch_add(tasks.size(), std::memory_order_release);

    // Distribute tasks across queues for better balance
    for (std::size_t q = 0; q < _numThreads && taskIndex < tasks.size(); ++q)
    {
        std::size_t batchEnd = std::min(taskIndex + tasksPerQueue, tasks.size());
        for (; taskIndex < batchEnd; ++taskIndex)
            _queues[q]->Push(std::move(tasks[taskIndex]));
    }

    // Wake all workers for batch
    {
        std::lock_guard<std::mutex> lock(_wakeMutex);
        _wakeCondition.notify_all();
    }
}

void WorkStealingPool::WaitForAll()
{
    std::unique_lock<std::mutex> lock(_completionMutex);
    _completionCondition.wait(lock, [this]() {
        return _pendingTasks.load(std::memory_order_acquire) == 0;
    });
}

std::size_t WorkStealingPool::GetPendingCount() const
{
    return _pendingTasks.load(std::memory_order_acquire);
}

void WorkStealingPool::WorkerThread(std::size_t workerId)
{
    while (!_stopping.load(std::memory_order_acquire))
    {
        // Try to execute work
        if (!TryExecuteOne(workerId))
        {
            // No work found, wait for notification
            std::unique_lock<std::mutex> lock(_wakeMutex);
            _wakeCondition.wait_for(lock, std::chrono::milliseconds(1), [this, workerId]() {
                return _stopping.load(std::memory_order_acquire) ||
                       !_queues[workerId]->Empty() ||
                       _pendingTasks.load(std::memory_order_acquire) > 0;
            });
        }
    }

    // Drain remaining tasks before exit
    while (TryExecuteOne(workerId)) {}
}

bool WorkStealingPool::TryExecuteOne(std::size_t workerId)
{
    Task task;

    // First, try own queue (LIFO - cache friendly)
    if (_queues[workerId]->Pop(task))
    {
        task();
        if (_pendingTasks.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            // Last task completed, notify waiters
            std::lock_guard<std::mutex> lock(_completionMutex);
            _completionCondition.notify_all();
        }
        return true;
    }

    // Try to steal from other queues
    if (TrySteal(workerId, task))
    {
        task();
        if (_pendingTasks.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            std::lock_guard<std::mutex> lock(_completionMutex);
            _completionCondition.notify_all();
        }
        return true;
    }

    return false;
}

bool WorkStealingPool::TrySteal(std::size_t workerId, Task& task)
{
    // Try to steal from other workers, starting from random offset
    std::size_t startOffset = workerId + 1;

    for (std::size_t i = 0; i < _numThreads - 1; ++i)
    {
        std::size_t victimId = (startOffset + i) % _numThreads;
        if (victimId != workerId && _queues[victimId]->Steal(task))
            return true;
    }

    return false;
}
