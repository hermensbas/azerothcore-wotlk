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

#include "MapUpdater.h"
#include "DatabaseEnv.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Metric.h"

class UpdateRequest
{
public:
    UpdateRequest() = default;
    virtual ~UpdateRequest() = default;

    virtual void call() = 0;
};

class MapUpdateRequest : public UpdateRequest
{
public:
    MapUpdateRequest(Map& m, MapUpdater& u, uint32 d, uint32 sd)
        : m_map(m), m_updater(u), m_diff(d), s_diff(sd)
    {
    }

    void call() override
    {
        METRIC_TIMER("map_update_time_diff", METRIC_TAG("map_id", std::to_string(m_map.GetId())));
        m_map.Update(m_diff, s_diff);
        m_updater.update_finished();
    }

private:
    Map& m_map;
    MapUpdater& m_updater;
    uint32 m_diff;
    uint32 s_diff;
};

class MapPreloadRequest : public UpdateRequest
{
public:
    MapPreloadRequest(uint32 mapId, MapUpdater& updater)
        : _mapId(mapId), _updater(updater)
    {
    }

    void call() override
    {
        Map* map = sMapMgr->CreateBaseMap(_mapId);
        LOG_INFO("server.loading", ">> Loading All Grids For Map {} ({})", map->GetId(), map->GetMapName());
        map->LoadAllGrids();
        _updater.update_finished();
    }

private:
    uint32 _mapId;
    MapUpdater& _updater;
};

class LFGUpdateRequest : public UpdateRequest
{
public:
    LFGUpdateRequest(MapUpdater& u, uint32 d) : m_updater(u), m_diff(d) {}

    void call() override
    {
        sLFGMgr->Update(m_diff, 1);
        m_updater.update_finished();
    }
private:
    MapUpdater& m_updater;
    uint32 m_diff;
};

MapUpdater::MapUpdater() = default;

void MapUpdater::activate(std::size_t num_threads)
{
    if (num_threads == 0)
        return;

    _useWorkStealing = false;
    _workerThreads.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
        _workerThreads.push_back(std::thread(&MapUpdater::WorkerThread, this));

    LOG_INFO("server.loading", ">> MapUpdater activated with {} legacy worker threads", num_threads);
}

void MapUpdater::activate_work_stealing(std::size_t num_threads)
{
    if (num_threads == 0)
        return;

    _useWorkStealing = true;
    _workStealingPool = std::make_unique<WorkStealingPool>(num_threads);
    _workStealingPool->Activate();

    LOG_INFO("server.loading", ">> MapUpdater activated with {} work-stealing threads", num_threads);
}

void MapUpdater::deactivate()
{
    _cancelationToken = true;

    wait();

    if (_useWorkStealing)
    {
        if (_workStealingPool)
        {
            _workStealingPool->Deactivate();
            _workStealingPool.reset();
        }
    }
    else
    {
        _queue.Cancel();

        for (auto& thread : _workerThreads)
        {
            if (thread.joinable())
                thread.join();
        }
        _workerThreads.clear();
    }

    _useWorkStealing = false;
}

void MapUpdater::wait()
{
    if (_useWorkStealing)
    {
        if (_workStealingPool)
            _workStealingPool->WaitForAll();
    }
    else
    {
        std::unique_lock<std::mutex> guard(_lock);
        _condition.wait(guard, [this] {
            return pending_requests.load(std::memory_order_acquire) == 0;
        });
    }
}

void MapUpdater::schedule_task(UpdateRequest* request)
{
    if (_useWorkStealing)
    {
        pending_requests.fetch_add(1, std::memory_order_release);
        _workStealingPool->Submit([this, request]() {
            request->call();
            delete request;
        });
    }
    else
    {
        pending_requests.fetch_add(1, std::memory_order_release);
        _queue.Push(request);
    }
}

void MapUpdater::schedule_update(Map& map, uint32 diff, uint32 s_diff)
{
    schedule_task(new MapUpdateRequest(map, *this, diff, s_diff));
}

void MapUpdater::schedule_map_preload(uint32 mapid)
{
    schedule_task(new MapPreloadRequest(mapid, *this));
}

void MapUpdater::schedule_lfg_update(uint32 diff)
{
    schedule_task(new LFGUpdateRequest(*this, diff));
}

bool MapUpdater::activated()
{
    return _useWorkStealing ? (_workStealingPool && _workStealingPool->IsActive())
                            : !_workerThreads.empty();
}

void MapUpdater::update_finished()
{
    if (_useWorkStealing)
    {
        // Work-stealing pool handles completion tracking internally
        pending_requests.fetch_sub(1, std::memory_order_acquire);
    }
    else
    {
        if (pending_requests.fetch_sub(1, std::memory_order_acquire) == 1)
        {
            std::lock_guard<std::mutex> lock(_lock);
            _condition.notify_all();
        }
    }
}

void MapUpdater::WorkerThread()
{
    LoginDatabase.WarnAboutSyncQueries(true);
    CharacterDatabase.WarnAboutSyncQueries(true);
    WorldDatabase.WarnAboutSyncQueries(true);

    while (!_cancelationToken)
    {
        UpdateRequest* request = nullptr;

        _queue.WaitAndPop(request);

        if (!_cancelationToken && request)
        {
            request->call();
            delete request;
        }
    }
}
