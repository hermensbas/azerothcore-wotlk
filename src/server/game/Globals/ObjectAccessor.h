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

#ifndef ACORE_OBJECTACCESSOR_H
#define ACORE_OBJECTACCESSOR_H

#include "Define.h"
#include "GridDefines.h"
#include "Object.h"
#include "ConcurrentHashMap.h"
#include <functional>
#include <shared_mutex>
#include <vector>

class Creature;
class Corpse;
class Unit;
class GameObject;
class DynamicObject;
class WorldObject;
class Vehicle;
class Map;
class WorldRunnable;
class Transport;
class StaticTransport;
class MotionTransport;

/**
 * @brief Legacy HashMapHolder - kept for backward compatibility
 * @deprecated Use ConcurrentHashMapHolder for new code
 */
template <class T>
class HashMapHolder
{
    //Non instanceable only static
    HashMapHolder() = default;

public:

    typedef std::unordered_map<ObjectGuid, T*> MapType;

    static void Insert(T* o);

    static void Remove(T* o);

    static T* Find(ObjectGuid guid);

    static MapType& GetContainer();

    static std::shared_mutex* GetLock();
};

/**
 * @brief High-performance concurrent holder using sharded locking
 *
 * Provides ~64x reduced lock contention compared to HashMapHolder.
 * Use ForEach() instead of GetContainer() for iteration.
 */
template <class T>
class ConcurrentHashMapHolder
{
    ConcurrentHashMapHolder() = default;

public:
    using MapType = ConcurrentHashMap<ObjectGuid, T*, 64>;

    static void Insert(T* o);
    static void Remove(T* o);
    static T* Find(ObjectGuid guid);

    /// Iterate all entries with a callback (thread-safe, shard-by-shard)
    template<typename Func>
    static void ForEach(Func&& func);

    /// Get a snapshot of all values (thread-safe copy)
    static std::vector<T*> GetSnapshot();

    /// Get approximate count
    static std::size_t Size();

private:
    static MapType& GetContainer();
};

namespace ObjectAccessor
{
    // these functions return objects only if in map of specified object
    WorldObject* GetWorldObject(WorldObject const&, ObjectGuid const& guid);
    Object* GetObjectByTypeMask(WorldObject const&, ObjectGuid const& guid, uint32 typemask);
    Corpse* GetCorpse(WorldObject const& u, ObjectGuid const& guid);
    GameObject* GetGameObject(WorldObject const& u, ObjectGuid const& guid);
    Transport* GetTransport(WorldObject const& u, ObjectGuid const& guid);
    DynamicObject* GetDynamicObject(WorldObject const& u, ObjectGuid const& guid);
    Unit* GetUnit(WorldObject const&, ObjectGuid const& guid);
    Creature* GetCreature(WorldObject const& u, ObjectGuid const& guid);
    Pet* GetPet(WorldObject const&, ObjectGuid const& guid);
    Player* GetPlayer(Map const*, ObjectGuid const& guid);
    Player* GetPlayer(WorldObject const&, ObjectGuid const& guid);
    Creature* GetCreatureOrPetOrVehicle(WorldObject const&, ObjectGuid const&);

    // these functions return objects if found in whole world
    // ACCESS LIKE THAT IS NOT THREAD SAFE
    Player* FindPlayer(ObjectGuid const guid);
    Player* FindPlayerByLowGUID(ObjectGuid::LowType lowguid);
    Player* FindConnectedPlayer(ObjectGuid const guid);
    Player* FindPlayerByName(std::string const& name, bool checkInWorld = true);
    Creature* GetSpawnedCreatureByDBGUID(uint32 mapId, uint64 guid);
    GameObject* GetSpawnedGameObjectByDBGUID(uint32 mapId, uint64 guid);

    /// Thread-safe iteration over all players using ConcurrentHashMapHolder
    void ForEachPlayer(std::function<void(Player*)> func);

    /// Get a thread-safe snapshot of all players
    std::vector<Player*> GetPlayersSnapshot();

    template<class T>
    void AddObject(T* object)
    {
        HashMapHolder<T>::Insert(object);
    }

    template<class T>
    void RemoveObject(T* object)
    {
        HashMapHolder<T>::Remove(object);
    }

    void SaveAllPlayers();

    template<>
    void AddObject(Player* player);

    template<>
    void RemoveObject(Player* player);

    template<>
    void AddObject(MotionTransport* transport);

    template<>
    void RemoveObject(MotionTransport* transport);

    void UpdatePlayerNameMapReference(std::string oldname, Player* player);
}

#endif
