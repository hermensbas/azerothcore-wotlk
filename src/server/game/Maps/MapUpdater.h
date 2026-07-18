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

#ifndef _MAP_UPDATER_H_INCLUDED
#define _MAP_UPDATER_H_INCLUDED

#include "Define.h"
#include "PCQueue.h"
#include "WorkStealingPool.h"
#include <condition_variable>
#include <thread>
#include <atomic>

class Map;
class UpdateRequest;

/**
 * @brief Map update scheduler with optional work-stealing
 *
 * Supports two modes:
 * 1. Legacy mode (default): Uses ProducerConsumerQueue with dedicated threads
 * 2. Work-stealing mode: Uses WorkStealingPool for better load balancing
 *
 * Work-stealing mode is enabled via activate_work_stealing() and provides
 * better performance when maps have varying update costs.
 */
class MapUpdater
{
public:
    MapUpdater();
    ~MapUpdater() = default;

    void schedule_task(UpdateRequest* request);
    void schedule_update(Map& map, uint32 diff, uint32 s_diff);
    void schedule_map_preload(uint32 mapid);
    void schedule_lfg_update(uint32 diff);
    void wait();

    /// Activate legacy mode with dedicated worker threads
    void activate(std::size_t num_threads);

    /// Activate work-stealing mode (recommended for better load balancing)
    void activate_work_stealing(std::size_t num_threads);

    void deactivate();
    bool activated();
    void update_finished();

    /// Check if using work-stealing mode
    [[nodiscard]] bool IsWorkStealingMode() const { return _useWorkStealing; }

private:
    void WorkerThread();

    // Legacy mode members
    ProducerConsumerQueue<UpdateRequest*> _queue;
    std::vector<std::thread> _workerThreads;

    // Work-stealing mode
    std::unique_ptr<WorkStealingPool> _workStealingPool;
    bool _useWorkStealing{false};

    // Shared state
    std::atomic<int> pending_requests{0};
    std::atomic<bool> _cancelationToken{false};
    std::mutex _lock;
    std::condition_variable _condition;
};

#endif //_MAP_UPDATER_H_INCLUDED
