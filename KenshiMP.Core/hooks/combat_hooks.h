#pragma once
#include <cstdint>

namespace kmp::combat_hooks {

bool Install();
void Uninstall();

// Process deferred combat events from the safe game-tick context.
// Called from Core::OnGameTick — NOT from inside a hook.
void ProcessDeferredEvents();

/// Samples authoritative player/NPC health from the game thread without an ApplyDamage hook.
void PollHealth(float deltaTime);

/// Clears polling and deferred event state on disconnect or save-world replacement.
void Reset();

// Echo suppression: set before calling native CharacterDeath/CharacterKO from
// packet handler (server-sourced events). The hook checks this flag and skips
// pushing to the deferred queue, preventing infinite C2S→S2C→C2S echo loops.
void SetServerSourcedDeath(bool active);
void SetServerSourcedKO(bool active);

} // namespace kmp::combat_hooks

