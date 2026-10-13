// KenshiMP.Core - prediction_reconciliation.cpp
// Purpose: Bounded, thread-safe local position reconciliation without calling game memory.
// Applied corrections rebase later samples to avoid applying the same displacement twice.
#include "prediction_reconciliation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>

namespace kmp {

namespace {

constexpr size_t PREDICTION_HISTORY_LIMIT = 128;
constexpr size_t PREDICTION_ENTITY_LIMIT = 8192;
constexpr uint32_t SEQUENCE_HALF_RANGE = 0x80000000u;
constexpr float CORRECTION_DEADZONE = 0.05f;
constexpr float CORRECTION_SNAP_DISTANCE = 50.f;
constexpr float CORRECTION_BLEND_TIME_SEC = 0.15f;
constexpr float CORRECTION_MAX_STEP_SEC = 0.1f;
constexpr float CORRECTION_FINISH_DISTANCE = 0.001f;

bool IsFinitePosition(const Vec3& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

bool IsFiniteRotation(const Quat& rotation) {
    return std::isfinite(rotation.w) && std::isfinite(rotation.x) &&
           std::isfinite(rotation.y) && std::isfinite(rotation.z);
}

double CorrectionLength(const Vec3& correction) {
    // Double arithmetic prevents float LengthSq overflow for finite, extreme input positions.
    const double x = correction.x;
    const double y = correction.y;
    const double z = correction.z;
    return std::sqrt(x * x + y * y + z * z);
}

} // namespace

bool PredictionReconciliation::IsNewerSequence(uint32_t sequence, uint32_t previous) {
    const uint32_t distance = sequence - previous;
    return distance != 0 && distance < SEQUENCE_HALF_RANGE;
}

bool PredictionReconciliation::RecordLocalInput(EntityID entityId, uint32_t generation,
                                               uint32_t sequence, const Vec3& position,
                                               const Quat& rotation) {
    if (entityId == INVALID_ENTITY || !IsFinitePosition(position) || !IsFiniteRotation(rotation)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entities.find(entityId);
    if (it == m_entities.end()) {
        if (m_entities.size() >= PREDICTION_ENTITY_LIMIT) {
            return false;
        }
        it = m_entities.emplace(entityId, PredictionState{}).first;
        it->second.generation = generation;
    }

    auto& state = it->second;
    if (state.generation != generation) {
        state = PredictionState{};
        state.generation = generation;
    }
    if (state.hasSequence && !IsNewerSequence(sequence, state.latestSequence)) {
        return false;
    }

    state.history.push_back({sequence, position, rotation});
    state.latestSequence = sequence;
    state.hasSequence = true;
    if (state.history.size() > PREDICTION_HISTORY_LIMIT) {
        state.history.pop_front();
    }
    return true;
}

ReconciliationResult PredictionReconciliation::ReconcileSnapshot(
    const CharacterPosition& snapshot, const Vec3& serverPosition) {
    if (snapshot.entityId == INVALID_ENTITY || !IsFinitePosition(serverPosition)) {
        return ReconciliationResult::Invalid;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entities.find(snapshot.entityId);
    if (it == m_entities.end() || it->second.generation != snapshot.generation) {
        return ReconciliationResult::UnknownInput;
    }
    auto& state = it->second;
    if (state.hasAcknowledgement &&
        !IsNewerSequence(snapshot.inputSequence, state.acknowledgedSequence)) {
        return ReconciliationResult::DuplicateOrStale;
    }

    auto inputIt = std::find_if(state.history.begin(), state.history.end(),
        [&snapshot](const InputSample& sample) {
            return sample.sequence == snapshot.inputSequence;
        });
    if (inputIt == state.history.end()) {
        // Lost/evicted samples and unknown future ACKs cannot safely correct the actor.
        return ReconciliationResult::UnknownInput;
    }

    const Vec3 correction = serverPosition - inputIt->position;
    if (!IsFinitePosition(correction)) {
        return ReconciliationResult::Invalid;
    }

    const double distance = CorrectionLength(correction);
    state.remainingCorrection = distance > CORRECTION_DEADZONE ? correction : Vec3{};
    state.snapPending = distance > CORRECTION_SNAP_DISTANCE;
    state.acknowledgedSequence = snapshot.inputSequence;
    state.hasAcknowledgement = true;
    state.history.erase(state.history.begin(), std::next(inputIt));
    return ReconciliationResult::Accepted;
}

void PredictionReconciliation::RebaseHistory(PredictionState& state, const Vec3& correctionDelta) {
    for (auto it = state.history.begin(); it != state.history.end();) {
        const Vec3 rebasedPosition = it->position + correctionDelta;
        if (!IsFinitePosition(rebasedPosition)) {
            // Do not retain an overflowed sample that could contaminate a later acknowledgement.
            it = state.history.erase(it);
        } else {
            it->position = rebasedPosition;
            ++it;
        }
    }
}

bool PredictionReconciliation::GetCorrection(EntityID entityId, uint32_t generation,
                                            float deltaTime, Vec3& correctionDelta) {
    correctionDelta = Vec3{};
    if (!std::isfinite(deltaTime) || deltaTime <= 0.f) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entities.find(entityId);
    if (it == m_entities.end() || it->second.generation != generation) {
        return false;
    }
    auto& state = it->second;
    const double distance = CorrectionLength(state.remainingCorrection);
    if (distance == 0.0) {
        return false;
    }

    if (state.snapPending || distance <= CORRECTION_FINISH_DISTANCE) {
        correctionDelta = state.remainingCorrection;
        state.remainingCorrection = Vec3{};
        state.snapPending = false;
    } else {
        const float step = std::min(deltaTime, CORRECTION_MAX_STEP_SEC);
        const float alpha = -std::expm1(-step / CORRECTION_BLEND_TIME_SEC);
        correctionDelta = state.remainingCorrection * alpha;
        state.remainingCorrection = state.remainingCorrection - correctionDelta;
    }

    RebaseHistory(state, correctionDelta);
    return true;
}

bool PredictionReconciliation::RestoreCorrection(EntityID entityId, uint32_t generation,
                                                const Vec3& correctionDelta) {
    if (!IsFinitePosition(correctionDelta)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entities.find(entityId);
    if (it == m_entities.end() || it->second.generation != generation) {
        return false;
    }
    auto& state = it->second;
    const Vec3 restoredCorrection = state.remainingCorrection + correctionDelta;
    if (!IsFinitePosition(restoredCorrection)) {
        return false;
    }
    state.remainingCorrection = restoredCorrection;
    state.snapPending = CorrectionLength(restoredCorrection) > CORRECTION_SNAP_DISTANCE;
    RebaseHistory(state, correctionDelta * -1.f);
    return true;
}

void PredictionReconciliation::RemoveEntity(EntityID entityId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entities.erase(entityId);
}

void PredictionReconciliation::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entities.clear();
}

} // namespace kmp
