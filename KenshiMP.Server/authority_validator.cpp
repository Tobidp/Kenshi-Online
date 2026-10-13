#include "authority_validator.h"
#include <spdlog/spdlog.h>

namespace kmp {

bool ServerAuthorityValidator::CanClientCommandEntity(
    PlayerID clientPlayerId,
    EntityID entityId,
    const std::unordered_map<EntityID, ServerEntity>& entities
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

    // 3. Check authority type (only Player-owned entities can be commanded by clients)
    if (entity.authority != AuthorityType::Player) {
        spdlog::warn("[ServerAuth] Player {} tried to command non-player entity {} (authority={})",
                     clientPlayerId, entityId, static_cast<int>(entity.authority));
        return false;
    }

    // 4. Check ownership match
    if (entity.owner != clientPlayerId) {
        spdlog::warn("[ServerAuth] AUTHORITY VIOLATION: Player {} tried to command entity {} owned by player {}",
                     clientPlayerId, entityId, entity.owner);
        return false;
    }

    return true;
}

bool ServerAuthorityValidator::ValidatePositionUpdate(
    PlayerID clientPlayerId,
    EntityID entityId,
    uint32_t generation,
    const std::unordered_map<EntityID, ServerEntity>& entities
) {
    if (!CanClientCommandEntity(clientPlayerId, entityId, entities)) {
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

