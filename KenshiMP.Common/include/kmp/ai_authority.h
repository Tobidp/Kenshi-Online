#pragma once
#include "types.h"

namespace kmp {
constexpr float KMP_NPC_LINK_RADIUS = 3.f;

/// Identifies character entities whose simulation can be delegated to the elected host.
constexpr bool IsWorldNpc(EntityType type) {
    return type == EntityType::NPC || type == EntityType::Animal;
}

/// Owner zero distinguishes world NPCs from legacy player characters tagged as NPCs.
constexpr bool IsWorldNpc(EntityType type, PlayerID ownerId) {
    return ownerId == INVALID_PLAYER && IsWorldNpc(type);
}

/// Selects the one client allowed to publish a physical character's simulation state.
/// This policy does not disable any Kenshi AI controller or behavior tree.
constexpr bool CanPublishSimulation(EntityType type, PlayerID ownerId, PlayerID localPlayerId,
                                    bool isHost, bool isRemote, EntityState state,
                                    bool hasGameObject) {
    if (localPlayerId == INVALID_PLAYER || isRemote || state != EntityState::Active ||
        !hasGameObject) {
        return false;
    }
    if (IsWorldNpc(type, ownerId)) {
        return isHost;
    }
    bool isCharacter = type == EntityType::PlayerCharacter || type == EntityType::NPC ||
                       type == EntityType::Animal;
    return isCharacter && ownerId != INVALID_PLAYER && ownerId == localPlayerId;
}

/// Accepts world NPC state only from the server's current, nonzero host identity.
constexpr bool CanPublishWorldNpc(EntityType type, PlayerID ownerId,
                                 PlayerID senderId, PlayerID hostPlayerId) {
    return IsWorldNpc(type, ownerId) && senderId != INVALID_PLAYER &&
           senderId == hostPlayerId;
}

/// Server-side sender authorization; the server separately validates entity lifecycle/generation.
constexpr bool CanSimulateActor(EntityType type, PlayerID ownerId,
                               PlayerID senderId, PlayerID hostPlayerId) {
    if (senderId == INVALID_PLAYER) {
        return false;
    }
    if (ownerId == INVALID_PLAYER) {
        return CanPublishWorldNpc(type, ownerId, senderId, hostPlayerId);
    }
    bool isCharacter = type == EntityType::PlayerCharacter || IsWorldNpc(type);
    return isCharacter && ownerId == senderId;
}

} // namespace kmp
