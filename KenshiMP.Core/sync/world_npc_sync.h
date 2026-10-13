// KenshiMP.Core - world_npc_sync.h
// Experimental host-state replication for loaded NPCs; this does not suppress local AI decisions.
#pragma once
#include "kmp/types.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace kmp {
class Core;

class WorldNpcSync {
public:
    /// Runs only on the game thread and only when syncWorldNpcs is explicitly enabled.
    void Tick(Core& core, float deltaTime);
    /// Queues a server NPC for matching; the template/faction pair is not a persistent instance ID.
    void QueueExistingNpc(EntityID entityId, uint32_t generation, uint32_t templateId,
                          uint32_t factionId, const Vec3& position);
    /// Links only one unambiguous nearby NPC from the loaded world on the game thread.
    bool LinkExistingNpc(Core& core, EntityID entityId, uint32_t templateId,
                         uint32_t factionId, const Vec3& position);
    /// Clears pending work; the game thread resets its discovery timer on the next tick.
    void Reset();

private:
    struct PendingNpc {
        uint32_t generation;
        uint32_t templateId;
        uint32_t factionId;
        Vec3 position;
    };
    std::mutex m_mutex;
    std::unordered_map<EntityID, PendingNpc> m_pending;
    std::atomic<bool> m_resetRequested{false};
    float m_discoveryElapsed = 0.f;
};
} // namespace kmp

