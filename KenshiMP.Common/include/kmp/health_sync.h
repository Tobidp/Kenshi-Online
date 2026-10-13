// KenshiMP.Common - health_sync.h
// Shared validation and change detection for replicated limb health.
#pragma once
#include "types.h"
#include <cmath>

namespace kmp {

constexpr int KMP_LIMB_COUNT = static_cast<int>(BodyPart::Count);
constexpr float KMP_HEALTH_CHANGE_EPSILON = 0.1f;
constexpr float KMP_HEALTH_POLL_INTERVAL = 0.1f;
constexpr float KMP_HEALTH_KEYFRAME_INTERVAL = 2.f;

/// Rejects invalid numbers without clamping legitimate negative or race-specific health.
inline bool IsFiniteLimbHealth(const float health[KMP_LIMB_COUNT]) {
    if (!health) {
        return false;
    }
    for (int limb = 0; limb < KMP_LIMB_COUNT; limb++) {
        if (!std::isfinite(health[limb])) {
            return false;
        }
    }
    return true;
}

/// Compares against the last transmitted snapshot so small changes accumulate.
inline bool HasLimbHealthChanged(const float health[KMP_LIMB_COUNT],
                                 const float previous[KMP_LIMB_COUNT]) {
    for (int limb = 0; limb < KMP_LIMB_COUNT; limb++) {
        if (std::abs(health[limb] - previous[limb]) >= KMP_HEALTH_CHANGE_EPSILON) {
            return true;
        }
    }
    return false;
}

} // namespace kmp
