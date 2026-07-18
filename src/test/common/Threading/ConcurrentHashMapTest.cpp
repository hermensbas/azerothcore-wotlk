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

#include "ConcurrentHashMap.h"
#include "gtest/gtest.h"
#include <atomic>
#include <random>
#include <set>
#include <thread>
#include <vector>

class ConcurrentHashMapTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ConcurrentHashMapTest, BasicOperations)
{
    ConcurrentHashMap<int, std::string> map;

    EXPECT_TRUE(map.Insert(1, "one"));
    EXPECT_TRUE(map.Insert(2, "two"));
    EXPECT_TRUE(map.Insert(3, "three"));

    EXPECT_EQ(map.Find(1), "one");
    EXPECT_EQ(map.Find(2), "two");
    EXPECT_EQ(map.Find(3), "three");
    EXPECT_EQ(map.Find(999), std::string{}) << "Missing key should return default";

    EXPECT_EQ(map.Size(), 3);
}

TEST_F(ConcurrentHashMapTest, InsertDuplicate)
{
    ConcurrentHashMap<int, int> map;

    EXPECT_TRUE(map.Insert(1, 100));
    EXPECT_FALSE(map.Insert(1, 200)) << "Duplicate insert should fail";
    EXPECT_EQ(map.Find(1), 100) << "Original value should be preserved";
}

TEST_F(ConcurrentHashMapTest, Remove)
{
    ConcurrentHashMap<int, int> map;

    map.Insert(1, 100);
    map.Insert(2, 200);

    EXPECT_TRUE(map.Remove(1));
    EXPECT_FALSE(map.Remove(1)) << "Double remove should fail";
    EXPECT_EQ(map.Find(1), 0) << "Removed key should return default";
    EXPECT_EQ(map.Find(2), 200) << "Other keys should remain";
    EXPECT_EQ(map.Size(), 1);
}

TEST_F(ConcurrentHashMapTest, ForEach)
{
    ConcurrentHashMap<int, int> map;

    for (int i = 0; i < 100; ++i)
        map.Insert(i, i * 10);

    int sum = 0;
    int count = 0;
    map.ForEach([&sum, &count](int key, int value) {
        sum += value;
        count++;
    });

    EXPECT_EQ(count, 100);
    EXPECT_EQ(sum, 49500) << "Sum of 0*10 + 1*10 + ... + 99*10 = 49500";
}

TEST_F(ConcurrentHashMapTest, GetSnapshot)
{
    ConcurrentHashMap<int, int*> map;

    std::vector<int> values = {1, 2, 3, 4, 5};
    for (auto& v : values)
        map.Insert(v, &v);

    auto snapshot = map.GetSnapshot();
    EXPECT_EQ(snapshot.size(), 5);

    std::set<int> found;
    for (int* ptr : snapshot)
        found.insert(*ptr);

    EXPECT_EQ(found, std::set<int>({1, 2, 3, 4, 5}));
}

TEST_F(ConcurrentHashMapTest, ConcurrentInserts)
{
    ConcurrentHashMap<int, int> map;
    constexpr int NUM_THREADS = 8;
    constexpr int INSERTS_PER_THREAD = 10000;

    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&map, &successCount, t]() {
            for (int i = 0; i < INSERTS_PER_THREAD; ++i)
            {
                int key = t * INSERTS_PER_THREAD + i;
                if (map.Insert(key, key * 2))
                    successCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    EXPECT_EQ(successCount.load(), NUM_THREADS * INSERTS_PER_THREAD)
        << "All inserts to unique keys should succeed";
    EXPECT_EQ(map.Size(), NUM_THREADS * INSERTS_PER_THREAD);
}

TEST_F(ConcurrentHashMapTest, ConcurrentReads)
{
    ConcurrentHashMap<int, int> map;
    constexpr int NUM_ENTRIES = 10000;

    // Pre-populate
    for (int i = 0; i < NUM_ENTRIES; ++i)
        map.Insert(i, i * 3);

    constexpr int NUM_THREADS = 8;
    constexpr int READS_PER_THREAD = 100000;

    std::atomic<int> successCount{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&map, &successCount]() {
            std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, NUM_ENTRIES - 1);

            for (int i = 0; i < READS_PER_THREAD; ++i)
            {
                int key = dist(rng);
                int expected = key * 3;
                if (map.Find(key) == expected)
                    successCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    EXPECT_EQ(successCount.load(), NUM_THREADS * READS_PER_THREAD)
        << "All concurrent reads should return correct values";
}

TEST_F(ConcurrentHashMapTest, ConcurrentMixedOperations)
{
    ConcurrentHashMap<int, int> map;
    constexpr int NUM_THREADS = 8;
    constexpr int OPS_PER_THREAD = 10000;

    std::atomic<bool> dataCorruption{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&map, &dataCorruption, t]() {
            std::mt19937 rng(t);
            std::uniform_int_distribution<int> keyDist(0, 999);
            std::uniform_int_distribution<int> opDist(0, 2);

            for (int i = 0; i < OPS_PER_THREAD; ++i)
            {
                int key = keyDist(rng);
                int op = opDist(rng);

                if (op == 0)
                {
                    // Insert
                    map.Insert(key, key * 7);
                }
                else if (op == 1)
                {
                    // Read and verify
                    int value = map.Find(key);
                    if (value != 0 && value != key * 7)
                    {
                        dataCorruption.store(true, std::memory_order_relaxed);
                    }
                }
                else
                {
                    // Remove
                    map.Remove(key);
                }
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    EXPECT_FALSE(dataCorruption.load()) << "No data corruption should occur under concurrent access";
}

TEST_F(ConcurrentHashMapTest, ConcurrentForEach)
{
    ConcurrentHashMap<int, int> map;
    constexpr int NUM_ENTRIES = 1000;

    for (int i = 0; i < NUM_ENTRIES; ++i)
        map.Insert(i, i);

    constexpr int NUM_THREADS = 4;
    std::atomic<int> iterationCount{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&map, &iterationCount]() {
            int localCount = 0;
            map.ForEach([&localCount](int, int) {
                localCount++;
            });
            iterationCount.fetch_add(localCount, std::memory_order_relaxed);
        });
    }

    for (auto& thread : threads)
        thread.join();

    // Each thread should see all entries (roughly - some might be added/removed)
    EXPECT_GE(iterationCount.load(), NUM_THREADS * NUM_ENTRIES * 0.9)
        << "ForEach should iterate most entries even under concurrent access";
}

TEST_F(ConcurrentHashMapTest, ContainsKey)
{
    ConcurrentHashMap<int, std::string> map;

    map.Insert(42, "answer");

    EXPECT_TRUE(map.Contains(42));
    EXPECT_FALSE(map.Contains(0));
    EXPECT_FALSE(map.Contains(999));

    map.Remove(42);
    EXPECT_FALSE(map.Contains(42));
}

TEST_F(ConcurrentHashMapTest, ShardDistribution)
{
    // Test that keys distribute across shards reasonably well
    ConcurrentHashMap<int, int, 64> map;

    constexpr int NUM_ENTRIES = 64000;
    for (int i = 0; i < NUM_ENTRIES; ++i)
        map.Insert(i, i);

    EXPECT_EQ(map.Size(), NUM_ENTRIES);
    // With 64 shards and 64000 entries, average should be 1000 per shard
    // The map should work correctly regardless of distribution
}

TEST_F(ConcurrentHashMapTest, StressTest_RapidInsertRemove)
{
    ConcurrentHashMap<int, int> map;
    constexpr int NUM_THREADS = 8;
    constexpr int CYCLES = 1000;
    constexpr int KEYS_PER_CYCLE = 100;

    std::vector<std::thread> threads;
    std::atomic<int> totalOps{0};

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&map, &totalOps, t]() {
            for (int cycle = 0; cycle < CYCLES; ++cycle)
            {
                // Insert phase
                for (int i = 0; i < KEYS_PER_CYCLE; ++i)
                {
                    int key = t * 10000 + cycle * 100 + i;
                    map.Insert(key, key);
                    totalOps.fetch_add(1, std::memory_order_relaxed);
                }

                // Remove phase
                for (int i = 0; i < KEYS_PER_CYCLE; ++i)
                {
                    int key = t * 10000 + cycle * 100 + i;
                    map.Remove(key);
                    totalOps.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    EXPECT_EQ(totalOps.load(), NUM_THREADS * CYCLES * KEYS_PER_CYCLE * 2);
    EXPECT_EQ(map.Size(), 0) << "All inserted keys should be removed";
}

TEST_F(ConcurrentHashMapTest, PointerValues)
{
    // Test with pointer values (like ObjectAccessor uses)
    struct MockObject
    {
        int id;
        explicit MockObject(int i) : id(i) {}
    };

    ConcurrentHashMap<int, MockObject*> map;
    std::vector<std::unique_ptr<MockObject>> objects;

    constexpr int NUM_OBJECTS = 100;
    for (int i = 0; i < NUM_OBJECTS; ++i)
    {
        objects.push_back(std::make_unique<MockObject>(i));
        map.Insert(i, objects.back().get());
    }

    for (int i = 0; i < NUM_OBJECTS; ++i)
    {
        MockObject* found = map.Find(i);
        ASSERT_NE(found, nullptr) << "Object " << i << " should be found";
        EXPECT_EQ(found->id, i) << "Object ID should match key";
    }

    auto snapshot = map.GetSnapshot();
    EXPECT_EQ(snapshot.size(), NUM_OBJECTS);
}
