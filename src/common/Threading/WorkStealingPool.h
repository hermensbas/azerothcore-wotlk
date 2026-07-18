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

#ifndef ACORE_WORK_STEALING_POOL_H
#define ACORE_WORK_STEALING_POOL_H

#include "Define.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

/**
 * @brief Work-Stealing Thread Pool
 *
 * A high-performance thread pool where each worker has its own task queue.
 * When a worker's queue is empty, it steals work from other workers.
 *
 * Benefits over simple producer-consumer:
 * - Better cache locality (workers process their own tasks LIFO)
 * - Automatic load balancing (busy workers get help from idle ones)
 * - Reduced contention (per-worker queues vs single global queue)
 *
 * Usage:
 *   WorkStealingPool pool(4);  // 4 worker threads
 *   pool.Activate();
 *
 *   // Submit tasks
 *   for (auto& task : tasks) {
 *       pool.Submit([&task]() { task.Execute(); });
 *   }
 *
 *   pool.WaitForAll();  // Wait for completion
 *   pool.Deactivate();
 */
class WorkStealingPool
{
public:
    using Task = std::function<void()>;

    explicit WorkStealingPool(std::size_t numThreads = 0);
    ~WorkStealingPool();

    // Non-copyable, non-movable
    WorkStealingPool(WorkStealingPool const&) = delete;
    WorkStealingPool& operator=(WorkStealingPool const&) = delete;

    /// Start the worker threads
    void Activate();

    /// Stop all workers and wait for completion
    void Deactivate();

    /// Check if pool is active
    [[nodiscard]] bool IsActive() const { return _active.load(std::memory_order_acquire); }

    /// Submit a task for execution
    void Submit(Task task);

    /// Submit multiple tasks efficiently
    void SubmitBatch(std::vector<Task>& tasks);

    /// Wait for all submitted tasks to complete
    void WaitForAll();

    /// Get number of pending tasks across all queues
    [[nodiscard]] std::size_t GetPendingCount() const;

    /// Get number of worker threads
    [[nodiscard]] std::size_t GetWorkerCount() const { return _numThreads; }

private:
    /// Per-worker task queue with stealing support
    class WorkerQueue
    {
    public:
        WorkerQueue() = default;

        /// Push task to back (owner's end)
        void Push(Task task);

        /// Pop task from back (owner's LIFO pop - cache friendly)
        bool Pop(Task& task);

        /// Steal task from front (thief's FIFO steal - fairness)
        bool Steal(Task& task);

        /// Check if empty
        [[nodiscard]] bool Empty() const;

        /// Get approximate size
        [[nodiscard]] std::size_t Size() const;

    private:
        mutable std::mutex _mutex;
        std::deque<Task> _tasks;
    };

    void WorkerThread(std::size_t workerId);
    bool TryExecuteOne(std::size_t workerId);
    bool TrySteal(std::size_t workerId, Task& task);

    std::size_t _numThreads;
    std::vector<std::thread> _workers;
    std::vector<std::unique_ptr<WorkerQueue>> _queues;

    std::atomic<bool> _active{false};
    std::atomic<bool> _stopping{false};
    std::atomic<std::size_t> _pendingTasks{0};

    // For waiting on completion
    std::mutex _completionMutex;
    std::condition_variable _completionCondition;

    // For waking idle workers
    std::mutex _wakeMutex;
    std::condition_variable _wakeCondition;

    // Round-robin submission counter
    std::atomic<std::size_t> _nextQueue{0};
};

#endif // ACORE_WORK_STEALING_POOL_H
