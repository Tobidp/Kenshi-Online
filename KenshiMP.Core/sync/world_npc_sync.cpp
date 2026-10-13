#include "world_npc_sync.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../hooks/ai_hooks.h"
#include "kmp/ai_authority.h"
#include "kmp/memory.h"
#include "kmp/protocol.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <utility>
#include <vector>

namespace kmp {
namespace {
constexpr float NPC_DISCOVERY_INTERVAL = 1.f;
constexpr float NPC_MATCH_RADIUS = KMP_NPC_LINK_RADIUS;
constexpr uint32_t MAX_WORLD_ARRAY_CAPACITY = 100000;
constexpr uintptr_t MIN_GAME_POINTER = 0x10000;
constexpr uintptr_t LEKTOR_SIZE_OFFSET = 0x08;
constexpr uintptr_t LEKTOR_CAPACITY_OFFSET = 0x0C;
constexpr uintptr_t LEKTOR_DATA_OFFSET = 0x10;

std::vector<void*> ReadLoadedWorldCharacters() {
    std::vector<void*> characters;
    const uintptr_t singleton = game::GetResolvedGameWorld();
    const int listOffset = game::GetOffsets().world.characterList;
    if (singleton == 0 || listOffset < 0) {
        return characters;
    }
    uintptr_t world = 0;
    if (!Memory::Read(singleton, world) || world < MIN_GAME_POINTER) {
        return characters;
    }
    const uintptr_t list = world + listOffset;
    uint32_t count = 0, capacity = 0;
    uintptr_t data = 0;
    if (!Memory::Read(list + LEKTOR_SIZE_OFFSET, count) ||
        !Memory::Read(list + LEKTOR_CAPACITY_OFFSET, capacity) ||
        !Memory::Read(list + LEKTOR_DATA_OFFSET, data) || count > capacity ||
        capacity > MAX_WORLD_ARRAY_CAPACITY || data < MIN_GAME_POINTER) {
        return characters;
    }
    for (uint32_t index = 0; index < count; index++) {
        uintptr_t character = 0;
        if (Memory::Read(data + index * sizeof(uintptr_t), character) && character >= MIN_GAME_POINTER) {
            characters.push_back(reinterpret_cast<void*>(character));
        }
    }
    return characters;
}

bool ReadNpcIdentity(void* object, uint32_t& templateId, uint32_t& factionId) {
    game::CharacterAccessor character(object);
    if (!character.IsValid()) {
        return false;
    }
    const uintptr_t faction = character.GetFactionPtr();
    const uintptr_t gameData = character.GetGameDataPtr();
    const auto& offsets = game::GetOffsets();
    if (faction < MIN_GAME_POINTER || gameData < MIN_GAME_POINTER ||
        offsets.faction.id < 0 || offsets.gameData.id < 0) {
        return false;
    }
    if (game::FactionAccessor(reinterpret_cast<void*>(faction)).IsPlayerFaction() ||
        character.GetName().compare(0, 7, "Player ") == 0) {
        return false;
    }
    return Memory::Read(faction + offsets.faction.id, factionId) &&
           Memory::Read(gameData + offsets.gameData.id, templateId) &&
           factionId != 0 && templateId != 0;
}
} // namespace

void WorldNpcSync::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.clear();
    m_resetRequested.store(true);
}

void WorldNpcSync::QueueExistingNpc(EntityID entityId, uint32_t generation, uint32_t templateId,
                                   uint32_t factionId, const Vec3& position) {
    if (entityId == INVALID_ENTITY || templateId == 0 || factionId == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.size() < KMP_MAX_SYNC_ENTITIES || m_pending.count(entityId) != 0) {
        m_pending[entityId] = {generation, templateId, factionId, position};
    }
}

bool WorldNpcSync::LinkExistingNpc(Core& core, EntityID entityId, uint32_t templateId,
                                  uint32_t factionId, const Vec3& position) {
    if (!core.GetConfig().syncWorldNpcs || !core.IsGameLoaded()) {
        return false;
    }
    auto& registry = core.GetEntityRegistry();
    void* match = nullptr;
    for (void* object : ReadLoadedWorldCharacters()) {
        if (registry.GetNetId(object) != INVALID_ENTITY) {
            continue;
        }
        uint32_t candidateTemplate = 0, candidateFaction = 0;
        if (!ReadNpcIdentity(object, candidateTemplate, candidateFaction) ||
            candidateTemplate != templateId || candidateFaction != factionId ||
            game::CharacterAccessor(object).GetPosition().DistanceTo(position) > NPC_MATCH_RADIUS) {
            continue;
        }
        if (match) {
            // A template/faction pair is not a persistent NPC instance ID. Reject ambiguity.
            return false;
        }
        match = object;
    }
    if (!match) {
        return false;
    }
    registry.SetGameObject(entityId, match);
    registry.SetServerSimulation(entityId, core.IsHost());
    if (!core.IsHost()) {
        ai_hooks::MarkRemoteControlled(match);
    }
    return true;
}

void WorldNpcSync::Tick(Core& core, float deltaTime) {
    if (m_resetRequested.exchange(false)) {
        m_discoveryElapsed = 0.f;
    }
    if (!core.GetConfig().syncWorldNpcs || !core.IsConnected() || !core.IsGameLoaded() ||
        core.IsLoading() || !std::isfinite(deltaTime) || deltaTime <= 0.f) {
        return;
    }
    std::vector<std::pair<EntityID, PendingNpc>> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending.assign(m_pending.begin(), m_pending.end());
    }
    auto& registry = core.GetEntityRegistry();
    for (const auto& [entityId, npc] : pending) {
        const auto info = registry.GetInfo(entityId);
        if (!info || info->generation != npc.generation ||
            !IsWorldNpc(info->type, info->ownerPlayerId)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.erase(entityId);
            continue;
        }
        if (info->gameObject || LinkExistingNpc(core, entityId, npc.templateId, npc.factionId, npc.position)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.erase(entityId);
        }
    }
    for (EntityID entityId : registry.GetPlayerEntities(INVALID_PLAYER)) {
        const auto info = registry.GetInfo(entityId);
        if (!info || !IsWorldNpc(info->type, info->ownerPlayerId) || !info->gameObject ||
            info->isRemote != core.IsHost()) {
            continue;
        }
        registry.SetServerSimulation(entityId, core.IsHost());
        if (core.IsHost()) {
            ai_hooks::UnmarkRemoteControlled(info->gameObject);
        } else {
            ai_hooks::MarkRemoteControlled(info->gameObject);
        }
    }
    m_discoveryElapsed += deltaTime;
    if (!core.IsHost() || m_discoveryElapsed < NPC_DISCOVERY_INTERVAL) {
        return;
    }
    m_discoveryElapsed = 0.f;
    for (void* object : ReadLoadedWorldCharacters()) {
        if (registry.GetNetId(object) != INVALID_ENTITY || registry.GetEntityCount() >= KMP_MAX_SYNC_ENTITIES) {
            continue;
        }
        uint32_t templateId = 0, factionId = 0;
        if (!ReadNpcIdentity(object, templateId, factionId)) {
            continue;
        }
        game::CharacterAccessor character(object);
        const Vec3 position = character.GetPosition();
        const Quat rotation = character.GetRotation();
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
            continue;
        }
        const EntityID entityId = registry.Register(object, EntityType::NPC, INVALID_PLAYER);
        registry.SetSpawnIdentity(entityId, templateId, factionId);
        registry.UpdatePosition(entityId, position);
        registry.UpdateRotation(entityId, rotation);
        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_EntitySpawnReq);
        writer.WriteU32(entityId);
        writer.WriteU8(static_cast<uint8_t>(EntityType::NPC));
        writer.WriteU32(INVALID_PLAYER);
        writer.WriteU32(templateId);
        writer.WriteVec3(position.x, position.y, position.z);
        writer.WriteU32(rotation.Compress());
        writer.WriteU32(factionId);
        writer.WriteString(character.GetName());
        core.GetClient().SendReliable(writer.Data(), writer.Size());
        spdlog::debug("World NPC candidate {} template={} faction={} published by host {}",
                      entityId, templateId, factionId, core.GetLocalPlayerId());
    }
}
} // namespace kmp

