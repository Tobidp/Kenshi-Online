#include "authority_validator.h"
#include "kmp/ai_authority.h"
#include <spdlog/spdlog.h>

namespace kmp {

bool ServerAuthorityValidator::CanClientCommandEntity(
    PlayerID clientPlayerId,
    EntityID entityId,
    const std::unordered_map<EntityID, ServerEntity>& entities,
    PlayerID hostPlayerId
) {
    // 1. Check if entity exists
    auto it = entities.find(entityId);
    if (it == entities.end()) {
        spdlog::warn("[ServerAuth] Player {} tried to command unknown entity {}",
                     clientPlayerId, entityId);
        return false;
    }

    const ServerEntity& entity = it->second;

    // 2. Check if entity is alive
    if (!entity.alive) {
        spdlog::warn("[ServerAuth] Player {} tried to command dead entity {}",
                     clientPlayerId, entityId);
        return false;
    }

    // World NPCs have one simulation authority: the current host.
    const bool playerAuthority = entity.authority == AuthorityType::Player;
    const bool worldAuthority = entity.authority == AuthorityType::Server &&
                                IsWorldNpc(entity.type, entity.owner);
    if ((!playerAuthority && !worldAuthority) ||
        !CanSimulateActor(entity.type, entity.owner, clientPlayerId, hostPlayerId)) {
        spdlog::warn("[ServerAuth] Player {} tried to command non-player entity {} (authority={})",
                     clientPlayerId, entityId, static_cast<int>(entity.authority));
        return false;
    }

    return true;
}

bool ServerAuthorityValidator::ValidatePositionUpdate(
    PlayerID clientPlayerId,
    EntityID entityId,
    uint32_t generation,
    const std::unordered_map<EntityID, ServerEntity>& entities,
    PlayerID hostPlayerId
) {
    if (!CanClientCommandEntity(clientPlayerId, entityId, entities, hostPlayerId)) {
        return false;
    }

    const ServerEntity& entity = entities.find(entityId)->second;
    if (entity.generation != generation) {
        spdlog::warn("[ServerAuth] Player {} sent entity {} generation {} (expected {})",
                     clientPlayerId, entityId, generation, entity.generation);
        return false;
    }

    return true;
}

} // namespace kmp

