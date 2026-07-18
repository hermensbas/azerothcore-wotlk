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

#ifndef ACORE_CONCURRENT_HASH_MAP_H
#define ACORE_CONCURRENT_HASH_MAP_H

#include "Define.h"
#include <array>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

/**
 * @brief High-performance concurrent hash map using striped locking
 *
 * Instead of a single lock protecting the entire map, this uses N independent
 * segments (shards), each with its own lock. This provides:
 * - 1/N contention compared to single-lock approach
 * - Excellent read scalability (shared locks per shard)
 * - Safe concurrent reads and writes to different keys
 *
 * The number of shards should be a power of 2 for fast modulo via bitmask.
 * Default is 64 shards which works well for servers with many cores.
 *
 * @tparam K Key type (must be hashable)
 * @tparam V Value type
 * @tparam NumShards Number of independent segments (power of 2)
 */
template<typename K, typename V, std::size_t NumShards = 64>
class ConcurrentHashMap
{
    static_assert((NumShards & (NumShards - 1)) == 0, "NumShards must be power of 2");

public:
    using KeyType = K;
    using ValueType = V;
    using MapType = std::unordered_map<K, V>;

    ConcurrentHashMap() = default;
    ~ConcurrentHashMap() = default;

    // Non-copyable, non-movable for thread safety
    ConcurrentHashMap(ConcurrentHashMap const&) = delete;
    ConcurrentHashMap& operator=(ConcurrentHashMap const&) = delete;

    /**
     * @brief Insert a key-value pair if key doesn't exist
     * @return true if inserted, false if key already exists (value unchanged)
     */
    bool Insert(K const& key, V value)
    {
        std::size_t shardIndex = GetShardIndex(key);
        std::unique_lock<std::shared_mutex> lock(_shards[shardIndex].mutex);

        auto result = _shards[shardIndex].map.try_emplace(key, std::move(value));
        return result.second; // true if inserted, false if already existed
    }

    /**
     * @brief Insert or update a key-value pair
     * @return true if inserted new key, false if updated existing
     */
    bool InsertOrAssign(K const& key, V value)
    {
        std::size_t shardIndex = GetShardIndex(key);
        std::unique_lock<std::shared_mutex> lock(_shards[shardIndex].mutex);

        auto result = _shards[shardIndex].map.insert_or_assign(key, std::move(value));
        return result.second;
    }

    /**
     * @brief Remove a key
     * @return true if removed, false if key didn't exist
     */
    bool Remove(K const& key)
    {
        std::size_t shardIndex = GetShardIndex(key);
        std::unique_lock<std::shared_mutex> lock(_shards[shardIndex].mutex);

        return _shards[shardIndex].map.erase(key) > 0;
    }

    /**
     * @brief Find a value by key (returns nullptr if not found)
     * @note The returned pointer is valid only while no Remove is called for this key
     */
    V Find(K const& key) const
    {
        std::size_t shardIndex = GetShardIndex(key);
        std::shared_lock<std::shared_mutex> lock(_shards[shardIndex].mutex);

        auto it = _shards[shardIndex].map.find(key);
        return (it != _shards[shardIndex].map.end()) ? it->second : V{};
    }

    /**
     * @brief Check if key exists
     */
    bool Contains(K const& key) const
    {
        std::size_t shardIndex = GetShardIndex(key);
        std::shared_lock<std::shared_mutex> lock(_shards[shardIndex].mutex);

        return _shards[shardIndex].map.count(key) > 0;
    }

    /**
     * @brief Get approximate total size (not atomic across shards)
     */
    [[nodiscard]] std::size_t Size() const
    {
        std::size_t total = 0;
        for (std::size_t i = 0; i < NumShards; ++i)
        {
            std::shared_lock<std::shared_mutex> lock(_shards[i].mutex);
            total += _shards[i].map.size();
        }
        return total;
    }

    /**
     * @brief Check if map is empty
     */
    [[nodiscard]] bool Empty() const
    {
        for (std::size_t i = 0; i < NumShards; ++i)
        {
            std::shared_lock<std::shared_mutex> lock(_shards[i].mutex);
            if (!_shards[i].map.empty())
                return false;
        }
        return true;
    }

    /**
     * @brief Clear all entries
     */
    void Clear()
    {
        for (std::size_t i = 0; i < NumShards; ++i)
        {
            std::unique_lock<std::shared_mutex> lock(_shards[i].mutex);
            _shards[i].map.clear();
        }
    }

    /**
     * @brief Execute function for each entry (read-only snapshot per shard)
     * @note Iterates shard-by-shard, not a global atomic snapshot
     */
    template<typename Func>
    void ForEach(Func&& func) const
    {
        for (std::size_t i = 0; i < NumShards; ++i)
        {
            std::shared_lock<std::shared_mutex> lock(_shards[i].mutex);
            for (auto const& pair : _shards[i].map)
            {
                func(pair.first, pair.second);
            }
        }
    }

    /**
     * @brief Execute function for each entry (with modification allowed)
     */
    template<typename Func>
    void ForEachMutable(Func&& func)
    {
        for (std::size_t i = 0; i < NumShards; ++i)
        {
            std::unique_lock<std::shared_mutex> lock(_shards[i].mutex);
            for (auto& pair : _shards[i].map)
            {
                func(pair.first, pair.second);
            }
        }
    }

    /**
     * @brief Get a snapshot of all values (thread-safe copy)
     */
    std::vector<V> GetSnapshot() const
    {
        std::vector<V> result;
        result.reserve(Size());

        for (std::size_t i = 0; i < NumShards; ++i)
        {
            std::shared_lock<std::shared_mutex> lock(_shards[i].mutex);
            for (auto const& pair : _shards[i].map)
            {
                result.push_back(pair.second);
            }
        }
        return result;
    }

    /**
     * @brief Get a snapshot as key-value pairs
     */
    std::vector<std::pair<K, V>> GetSnapshotPairs() const
    {
        std::vector<std::pair<K, V>> result;
        result.reserve(Size());

        for (std::size_t i = 0; i < NumShards; ++i)
        {
            std::shared_lock<std::shared_mutex> lock(_shards[i].mutex);
            for (auto const& pair : _shards[i].map)
            {
                result.emplace_back(pair.first, pair.second);
            }
        }
        return result;
    }

    /**
     * @brief Update value atomically using a function
     * @return true if key existed and was updated
     */
    template<typename Func>
    bool Update(K const& key, Func&& updateFunc)
    {
        std::size_t shardIndex = GetShardIndex(key);
        std::unique_lock<std::shared_mutex> lock(_shards[shardIndex].mutex);

        auto it = _shards[shardIndex].map.find(key);
        if (it == _shards[shardIndex].map.end())
            return false;

        updateFunc(it->second);
        return true;
    }

    /**
     * @brief Get or insert with default
     */
    V GetOrInsert(K const& key, V defaultValue)
    {
        std::size_t shardIndex = GetShardIndex(key);
        std::unique_lock<std::shared_mutex> lock(_shards[shardIndex].mutex);

        auto result = _shards[shardIndex].map.try_emplace(key, std::move(defaultValue));
        return result.first->second;
    }

private:
    struct alignas(64) Shard
    {
        mutable std::shared_mutex mutex;
        MapType map;
    };

    [[nodiscard]] std::size_t GetShardIndex(K const& key) const
    {
        return std::hash<K>{}(key) & (NumShards - 1);
    }

    mutable std::array<Shard, NumShards> _shards;
};

#endif // ACORE_CONCURRENT_HASH_MAP_H
