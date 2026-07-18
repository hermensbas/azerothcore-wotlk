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

#include "ObjectAccessor.h"
#include "Corpse.h"
#include "Creature.h"
#include "DynamicObject.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectDefines.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Transport.h"
#include <functional>

template<class T>
void HashMapHolder<T>::Insert(T* o)
{
    static_assert(std::is_same<Player, T>::value
        || std::is_same<MotionTransport, T>::value,
        "Only Player and Motion Transport can be registered in global HashMapHolder");

    std::unique_lock<std::shared_mutex> lock(*GetLock());

    GetContainer()[o->GetGUID()] = o;
}

template<class T>
void HashMapHolder<T>::Remove(T* o)
{
    std::unique_lock<std::shared_mutex> lock(*GetLock());

    GetContainer().erase(o->GetGUID());
}

template<class T>
T* HashMapHolder<T>::Find(ObjectGuid guid)
{
    std::shared_lock<std::shared_mutex> lock(*GetLock());

    typename MapType::iterator itr = GetContainer().find(guid);
    return (itr != GetContainer().end()) ? itr->second : nullptr;
}

template<class T>
auto HashMapHolder<T>::GetContainer() -> MapType&
{
    static MapType _objectMap;
    return _objectMap;
}

template<class T>
std::shared_mutex* HashMapHolder<T>::GetLock()
{
    static std::shared_mutex _lock;
    return &_lock;
}

void ObjectAccessor::ForEachPlayer(std::function<void(Player*)> func)
{
    ConcurrentHashMapHolder<Player>::ForEach(std::move(func));
}

std::vector<Player*> ObjectAccessor::GetPlayersSnapshot()
{
    return ConcurrentHashMapHolder<Player>::GetSnapshot();
}

// ConcurrentHashMapHolder implementations

template<class T>
void ConcurrentHashMapHolder<T>::Insert(T* o)
{
    static_assert(std::is_same<Player, T>::value
        || std::is_same<MotionTransport, T>::value,
        "Only Player and MotionTransport can be registered in ConcurrentHashMapHolder");

    GetContainer().Insert(o->GetGUID(), o);
}

template<class T>
void ConcurrentHashMapHolder<T>::Remove(T* o)
{
    GetContainer().Remove(o->GetGUID());
}

template<class T>
T* ConcurrentHashMapHolder<T>::Find(ObjectGuid guid)
{
    return GetContainer().Find(guid);
}

template<class T>
template<typename Func>
void ConcurrentHashMapHolder<T>::ForEach(Func&& func)
{
    GetContainer().ForEach([&func](ObjectGuid const& /*guid*/, T* obj) {
        func(obj);
    });
}

template<class T>
std::vector<T*> ConcurrentHashMapHolder<T>::GetSnapshot()
{
    return GetContainer().GetSnapshot();
}

template<class T>
std::size_t ConcurrentHashMapHolder<T>::Size()
{
    return GetContainer().Size();
}

template<class T>
auto ConcurrentHashMapHolder<T>::GetContainer() -> MapType&
{
    static MapType _objectMap;
    return _objectMap;
}

// Explicit template instantiations for ConcurrentHashMapHolder
template class ConcurrentHashMapHolder<Player>;
template class ConcurrentHashMapHolder<MotionTransport>;

// ForEach explicit instantiations for common function types
template void ConcurrentHashMapHolder<Player>::ForEach(std::function<void(Player*)>&&);
template void ConcurrentHashMapHolder<MotionTransport>::ForEach(std::function<void(MotionTransport*)>&&);

namespace PlayerNameMapHolder
{
    typedef std::unordered_map<std::string, Player*> MapType;
    static MapType PlayerNameMap;

    void Insert(Player* p)
    {
        PlayerNameMap[p->GetName()] = p;
    }

    void Remove(Player* p)
    {
        PlayerNameMap.erase(p->GetName());
    }

    void RemoveByName(std::string const& name)
    {
        PlayerNameMap.erase(name);
    }

    Player* Find(std::string const& name)
    {
        std::string charName(name);
        if (!normalizePlayerName(charName))
            return nullptr;

        auto itr = PlayerNameMap.find(charName);
        return (itr != PlayerNameMap.end()) ? itr->second : nullptr;
    }

} // namespace PlayerNameMapHolder

WorldObject* ObjectAccessor::GetWorldObject(WorldObject const& p, ObjectGuid const& guid)
{
    switch (guid.GetHigh())
    {
        case HighGuid::Player:
            return GetPlayer(p, guid);
        case HighGuid::Transport:
        case HighGuid::Mo_Transport:
        case HighGuid::GameObject:
            return GetGameObject(p, guid);
        case HighGuid::Vehicle:
        case HighGuid::Unit:
            return GetCreature(p, guid);
        case HighGuid::Pet:
            return GetPet(p, guid);
        case HighGuid::DynamicObject:
            return GetDynamicObject(p, guid);
        case HighGuid::Corpse:
            return GetCorpse(p, guid);
        default:
            return nullptr;
    }

    return nullptr;
}

Object* ObjectAccessor::GetObjectByTypeMask(WorldObject const& p, ObjectGuid const& guid, uint32 typemask)
{
    switch (guid.GetHigh())
    {
        case HighGuid::Item:
            if (typemask & TYPEMASK_ITEM && p.IsPlayer())
                return ((Player const&)p).GetItemByGuid(guid);
            break;
        case HighGuid::Player:
            if (typemask & TYPEMASK_PLAYER)
                return GetPlayer(p, guid);
            break;
        case HighGuid::Transport:
        case HighGuid::Mo_Transport:
        case HighGuid::GameObject:
            if (typemask & TYPEMASK_GAMEOBJECT)
                return GetGameObject(p, guid);
            break;
        case HighGuid::Unit:
        case HighGuid::Vehicle:
            if (typemask & TYPEMASK_UNIT)
                return GetCreature(p, guid);
            break;
        case HighGuid::Pet:
            if (typemask & TYPEMASK_UNIT)
                return GetPet(p, guid);
            break;
        case HighGuid::DynamicObject:
            if (typemask & TYPEMASK_DYNAMICOBJECT)
                return GetDynamicObject(p, guid);
            break;
        default:
            return nullptr;
    }

    return nullptr;
}

Corpse* ObjectAccessor::GetCorpse(WorldObject const& u, ObjectGuid const& guid)
{
    return u.GetMap()->GetCorpse(guid);
}

GameObject* ObjectAccessor::GetGameObject(WorldObject const& u, ObjectGuid const& guid)
{
    return u.GetMap()->GetGameObject(guid);
}

Transport* ObjectAccessor::GetTransport(WorldObject const& u, ObjectGuid const& guid)
{
    return u.GetMap()->GetTransport(guid);
}

DynamicObject* ObjectAccessor::GetDynamicObject(WorldObject const& u, ObjectGuid const& guid)
{
    return u.GetMap()->GetDynamicObject(guid);
}

Unit* ObjectAccessor::GetUnit(WorldObject const& u, ObjectGuid const& guid)
{
    if (guid.IsPlayer())
        return GetPlayer(u, guid);

    if (guid.IsPet())
        return GetPet(u, guid);

    return GetCreature(u, guid);
}

Creature* ObjectAccessor::GetCreature(WorldObject const& u, ObjectGuid const& guid)
{
    return u.GetMap()->GetCreature(guid);
}

Pet* ObjectAccessor::GetPet(WorldObject const& u, ObjectGuid const& guid)
{
    return u.GetMap()->GetPet(guid);
}

Player* ObjectAccessor::GetPlayer(Map const* m, ObjectGuid const& guid)
{
    if (Player* player = ConcurrentHashMapHolder<Player>::Find(guid))
        if (player->IsInWorld() && player->GetMap() == m)
            return player;

    return nullptr;
}

Player* ObjectAccessor::GetPlayer(WorldObject const& u, ObjectGuid const& guid)
{
    return GetPlayer(u.GetMap(), guid);
}

Creature* ObjectAccessor::GetCreatureOrPetOrVehicle(WorldObject const& u, ObjectGuid const& guid)
{
    if (guid.IsPet())
        return GetPet(u, guid);

    if (guid.IsCreatureOrVehicle())
        return GetCreature(u, guid);

    return nullptr;
}

Player* ObjectAccessor::FindPlayer(ObjectGuid const guid)
{
    Player* player = ConcurrentHashMapHolder<Player>::Find(guid);
    return player && player->IsInWorld() ? player : nullptr;
}

Player* ObjectAccessor::FindPlayerByLowGUID(ObjectGuid::LowType lowguid)
{
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowguid);
    return ObjectAccessor::FindPlayer(guid);
}

Player* ObjectAccessor::FindConnectedPlayer(ObjectGuid const guid)
{
    return ConcurrentHashMapHolder<Player>::Find(guid);
}

void ObjectAccessor::SaveAllPlayers()
{
    ConcurrentHashMapHolder<Player>::ForEach([](Player* player) {
        player->SaveToDB(false, false);
    });
}

Player* ObjectAccessor::FindPlayerByName(std::string const& name, bool checkInWorld)
{
    if (Player* player = PlayerNameMapHolder::Find(name))
        if (!checkInWorld || player->IsInWorld())
            return player;

    return nullptr;
}

/**
 * @brief Get a spawned creature by DB `guid` column. MODULE USAGE ONLY - USE IT FOR CUSTOM CONTENT.
 *
 * @param uint32 mapId The map id where the creature is spawned.
 * @param uint64 guid Database guid of the creature we are accessing.
 */
Creature* ObjectAccessor::GetSpawnedCreatureByDBGUID(uint32 mapId, uint64 guid)
{
    if (Map* map = sMapMgr->FindBaseMap(mapId))
    {
        auto bounds = map->GetCreatureBySpawnIdStore().equal_range(guid);

        if (bounds.first == bounds.second)
        {
            return nullptr;
        }

        if (Creature* creature = bounds.first->second)
        {
            return creature;
        }
    }

    return nullptr;
}

/**
 * @brief Get a spawned gameobject by DB `guid` column. MODULE USAGE ONLY - USE IT FOR CUSTOM CONTENT.
 *
 * @param uint32 mapId The map id where the gameobject is spawned.
 * @param uint64 guid Database guid of the gameobject we are accessing.
 */
GameObject* ObjectAccessor::GetSpawnedGameObjectByDBGUID(uint32 mapId, uint64 guid)
{
    if (Map* map = sMapMgr->FindBaseMap(mapId))
    {
        auto bounds = map->GetGameObjectBySpawnIdStore().equal_range(guid);

        if (bounds.first == bounds.second)
        {
            return nullptr;
        }

        if (GameObject* go = bounds.first->second)
        {
            return go;
        }
    }

    return nullptr;
}

template<>
void ObjectAccessor::AddObject(Player* player)
{
    ConcurrentHashMapHolder<Player>::Insert(player);
    PlayerNameMapHolder::Insert(player);
}

template<>
void ObjectAccessor::RemoveObject(Player* player)
{
    ConcurrentHashMapHolder<Player>::Remove(player);
    PlayerNameMapHolder::Remove(player);
}

template<>
void ObjectAccessor::AddObject(MotionTransport* transport)
{
    ConcurrentHashMapHolder<MotionTransport>::Insert(transport);
}

template<>
void ObjectAccessor::RemoveObject(MotionTransport* transport)
{
    ConcurrentHashMapHolder<MotionTransport>::Remove(transport);
}

void ObjectAccessor::UpdatePlayerNameMapReference(std::string oldname, Player* player)
{
    PlayerNameMapHolder::RemoveByName(oldname);
    PlayerNameMapHolder::Insert(player);
}
