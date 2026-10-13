#pragma once
#include "kmp/types.h"
#include "server.h"

namespace kmp {

// Server-side authority validation (Phase 5)
// Ensures clients can only command entities they own
class ServerAuthorityValidator {
public:
    // Check if a client can command a specific entity
    // Returns false if:
    // - Entity doesn't exist
    // - Entity is not alive
    // - Entity authority is not Player type
    // - Entity owner doesn't match the client player ID
    static bool CanClientCommandEntity(
        PlayerID clientPlayerId,
        EntityID entityId,
        const std::unordered_map<EntityID, ServerEntity>& entities
    );

    /// @brief Validates ownership, authority and generation for one position update.
    /// @param clientPlayerId Authenticated player sending the update.
    /// @param entityId Server entity ID from the inbound record.
    /// @param generation Generation from the inbound CharacterPosition record.
    /// @param entities Server entity registry, protected by the caller's mutex.
    /// @return True when the record belongs to a living player-controlled entity.
    static bool ValidatePositionUpdate(
        PlayerID clientPlayerId,
        EntityID entityId,
        uint32_t generation,
        const std::unordered_map<EntityID, ServerEntity>& entities
    );
};

} // namespace kmp

