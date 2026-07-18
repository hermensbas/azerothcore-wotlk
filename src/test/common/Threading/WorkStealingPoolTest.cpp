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
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <random>
#include <set>
#include <thread>
#include <vector>

class WorkStealingPoolTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(WorkStealingPoolTest, BasicTaskExecution)
{
    WorkStealingPool pool(4);
    pool.Activate();

    std::atomic<int> counter{0};
    constexpr int NUM_TASKS = 100;

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool.Submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.WaitForAll();
    pool.Deactivate();

    EXPECT_EQ(counter.load(), NUM_TASKS) << "All tasks should have executed";
}

TEST_F(WorkStealingPoolTest, StressTest_HighTaskCount)
{
    WorkStealingPool pool(8);
    pool.Activate();

    std::atomic<int> counter{0};
    constexpr int NUM_TASKS = 100000;

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool.Submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.WaitForAll();
    pool.Deactivate();

    EXPECT_EQ(counter.load(), NUM_TASKS) << "All 100k tasks should complete";
}

TEST_F(WorkStealingPoolTest, WorkDistribution_AllThreadsUsed)
{
    constexpr std::size_t NUM_THREADS = 4;
    WorkStealingPool pool(NUM_THREADS);
    pool.Activate();

    std::atomic<uint64_t> threadMask{0};
    constexpr int TASKS_PER_THREAD = 1000;

    for (int i = 0; i < TASKS_PER_THREAD * NUM_THREADS; ++i)
    {
        pool.Submit([&threadMask]() {
            // Hash current thread id to a bit position
            std::hash<std::thread::id> hasher;
            uint64_t bit = 1ULL << (hasher(std::this_thread::get_id()) % 64);
            threadMask.fetch_or(bit, std::memory_order_relaxed);

            // Do some work to allow scheduling
            volatile int sum = 0;
            for (int j = 0; j < 100; ++j)
                sum += j;
        });
    }

    pool.WaitForAll();
    pool.Deactivate();

    // Count bits set - should have multiple threads represented
    int bitsSet = __builtin_popcountll(threadMask.load());
    EXPECT_GE(bitsSet, 2) << "At least 2 threads should have executed tasks (work stealing should distribute)";
}

TEST_F(WorkStealingPoolTest, BatchSubmission)
{
    WorkStealingPool pool(4);
    pool.Activate();

    std::atomic<int> counter{0};
    constexpr int BATCH_SIZE = 500;

    std::vector<WorkStealingPool::Task> tasks;
    tasks.reserve(BATCH_SIZE);
    for (int i = 0; i < BATCH_SIZE; ++i)
    {
        tasks.push_back([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.SubmitBatch(tasks);
    pool.WaitForAll();
    pool.Deactivate();

    EXPECT_EQ(counter.load(), BATCH_SIZE) << "All batch tasks should complete";
}

TEST_F(WorkStealingPoolTest, ConcurrentSubmitAndWait)
{
    WorkStealingPool pool(4);
    pool.Activate();

    std::atomic<int> counter{0};
    constexpr int ITERATIONS = 10;
    constexpr int TASKS_PER_ITERATION = 1000;

    for (int iter = 0; iter < ITERATIONS; ++iter)
    {
        for (int i = 0; i < TASKS_PER_ITERATION; ++i)
        {
            pool.Submit([&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        pool.WaitForAll();

        EXPECT_EQ(counter.load(), (iter + 1) * TASKS_PER_ITERATION)
            << "After iteration " << iter << ", count should be correct";
    }

    pool.Deactivate();
}

TEST_F(WorkStealingPoolTest, VaryingTaskDurations)
{
    WorkStealingPool pool(4);
    pool.Activate();

    std::atomic<int> counter{0};
    constexpr int NUM_TASKS = 100;

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(1, 1000);

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        int workAmount = dist(rng);
        pool.Submit([&counter, workAmount]() {
            // Variable work to trigger work stealing
            volatile int sum = 0;
            for (int j = 0; j < workAmount; ++j)
                sum += j;
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.WaitForAll();
    pool.Deactivate();

    EXPECT_EQ(counter.load(), NUM_TASKS) << "All varying-duration tasks should complete";
}

TEST_F(WorkStealingPoolTest, ActivateDeactivateCycle)
{
    WorkStealingPool pool(4);

    for (int cycle = 0; cycle < 5; ++cycle)
    {
        pool.Activate();
        EXPECT_TRUE(pool.IsActive()) << "Pool should be active after Activate()";

        std::atomic<int> counter{0};
        for (int i = 0; i < 100; ++i)
        {
            pool.Submit([&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }

        pool.WaitForAll();
        pool.Deactivate();

        EXPECT_FALSE(pool.IsActive()) << "Pool should be inactive after Deactivate()";
        EXPECT_EQ(counter.load(), 100) << "Cycle " << cycle << " should complete all tasks";
    }
}

TEST_F(WorkStealingPoolTest, NoDeadlock_EmptyWait)
{
    WorkStealingPool pool(4);
    pool.Activate();

    // WaitForAll with no pending tasks should not deadlock
    auto start = std::chrono::steady_clock::now();
    pool.WaitForAll();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100)
        << "WaitForAll on empty pool should return quickly";

    pool.Deactivate();
}

TEST_F(WorkStealingPoolTest, DataIntegrity_NoLostUpdates)
{
    WorkStealingPool pool(8);
    pool.Activate();

    constexpr int NUM_TASKS = 10000;
    std::vector<std::atomic<int>> results(NUM_TASKS);
    for (auto& r : results)
        r.store(0);

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        pool.Submit([&results, i]() {
            results[i].fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.WaitForAll();
    pool.Deactivate();

    for (int i = 0; i < NUM_TASKS; ++i)
    {
        EXPECT_EQ(results[i].load(), 1) << "Task " << i << " should have executed exactly once";
    }
}

TEST_F(WorkStealingPoolTest, SimulatedMapUpdates)
{
    WorkStealingPool pool(4);
    pool.Activate();

    // Simulate map update pattern: schedule updates, wait, repeat
    constexpr int NUM_MAPS = 50;
    constexpr int UPDATE_CYCLES = 20;

    for (int cycle = 0; cycle < UPDATE_CYCLES; ++cycle)
    {
        std::atomic<int> updatesCompleted{0};

        for (int mapId = 0; mapId < NUM_MAPS; ++mapId)
        {
            pool.Submit([&updatesCompleted, mapId]() {
                // Simulate map update work
                volatile int work = 0;
                for (int i = 0; i < 500 + (mapId * 10); ++i)
                    work += i;
                updatesCompleted.fetch_add(1, std::memory_order_relaxed);
            });
        }

        pool.WaitForAll();

        EXPECT_EQ(updatesCompleted.load(), NUM_MAPS)
            << "Cycle " << cycle << ": All map updates should complete";
    }

    pool.Deactivate();
}
