#pragma once
#include <cstdint>

namespace kmp::ai_hooks {

bool Install();
void Uninstall();

// Tracks characters whose published state comes from another simulator.
// Creation/package hooks preserve controllers; they do not stop engine AI decisions.
void MarkRemoteControlled(void* character);
void UnmarkRemoteControlled(void* character);
bool IsRemoteControlled(void* character);

/// Clears non-owning character pointers on disconnect or game reload.
void ClearRemoteControlled();

/// No verified decision/tick hook is available for safely suppressing Kenshi AI.
constexpr bool SupportsDecisionSuppression() { return false; }

} // namespace kmp::ai_hooks

