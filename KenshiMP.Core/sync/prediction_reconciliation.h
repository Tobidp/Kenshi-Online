// KenshiMP.Core - prediction_reconciliation.h
// Purpose: Reconcile locally predicted position samples with server acknowledgements.
// This is state-based prediction: Kenshi inputs cannot be replayed deterministically here.
// Network callbacks enqueue corrections; only the game thread applies them to actors.
#pragma once

#include "kmp/messages.h"
#include "kmp/types.h"
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace kmp {

enum class ReconciliationResult {
    Accepted,
    DuplicateOrStale,
    UnknownInput,
    Invalid,
};

class PredictionReconciliation {
public:
    // Record the actual position before sending sequence to the server. Sequences may wrap.
    // A trusted local generation change discards all state belonging to the previous actor.
    bool RecordLocalInput(EntityID entityId, uint32_t generation, uint32_t sequence,
                          const Vec3& position, const Quat& rotation);

    // The acknowledged inputSequence must exist in this generation's bounded history.
    // The delta is relative to that input, preserving movement made after the acknowledgement.
    ReconciliationResult ReconcileSnapshot(const CharacterPosition& snapshot,
                                          const Vec3& serverPosition);

    // Consume a correction to add to the actor's CURRENT position on the game thread.
    // Moderate errors decay over time; large errors snap once. Invalid deltaTime does not consume.
    bool GetCorrection(EntityID entityId, uint32_t generation, float deltaTime, Vec3& correctionDelta);

    // Call once immediately if the game-thread position write failed after GetCorrection.
    // Returns the consumed delta and reverses its effect on the unacknowledged history.
    bool RestoreCorrection(EntityID entityId, uint32_t generation, const Vec3& correctionDelta);

    // Remove on entity destruction; Clear on disconnect, reconnect, or world reset.
    void RemoveEntity(EntityID entityId);
    void Clear();

private:
    struct InputSample {
        uint32_t sequence = 0;
        Vec3 position;
        Quat rotation;
    };

    struct PredictionState {
        uint32_t generation = 0;
        uint32_t latestSequence = 0;
        uint32_t acknowledgedSequence = 0;
        bool hasSequence = false;
        bool hasAcknowledgement = false;
        bool snapPending = false;
        Vec3 remainingCorrection;
        std::deque<InputSample> history;
    };

    static bool IsNewerSequence(uint32_t sequence, uint32_t previous);
    static void RebaseHistory(PredictionState& state, const Vec3& correctionDelta);

    std::mutex m_mutex;
    std::unordered_map<EntityID, PredictionState> m_entities;
};

} // namespace kmp
