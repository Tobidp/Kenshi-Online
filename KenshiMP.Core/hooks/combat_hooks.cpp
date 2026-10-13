#include "combat_hooks.h"
#include "../core.h"
#include "../game/game_types.h"
#include "kmp/health_sync.h"
#include "kmp/hook_manager.h"
#include "kmp/protocol.h"
#include "kmp/safe_hook.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace kmp::combat_hooks {

// ── Function Types ──
using CharacterDeathFn = void(__fastcall*)(void* character, void* killer);
using CharacterKOFn = void(__fastcall*)(void* character, void* attacker, int reason);

static CharacterDeathFn s_origCharDeath    = nullptr;
static CharacterKOFn    s_origCharKO       = nullptr;

// ── Hook Health ──
static HookHealth s_deathHealth{"CharacterDeath"};
static HookHealth s_koHealth{"CharacterKO"};

// ── Diagnostic Counters ──
static std::atomic<int> s_deathCount{0};
static std::atomic<int> s_koCount{0};

// ── Echo suppression flags ──
// Set by packet_handler before calling native CharacterDeath/CharacterKO from
// server-sourced events (S2C_CombatDeath, S2C_CombatKO). When set, the hook
// skips pushing to the deferred queue, preventing C2S→S2C→C2S echo loops.
static std::atomic<bool> s_serverSourcedDeath{false};
static std::atomic<bool> s_serverSourcedKO{false};

// ═══════════════════════════════════════════════════════════════════════════
//  DEFERRED EVENT QUEUE
//
//  Combat hooks fire inside MovRaxRsp naked detours where:
//  - spdlog calls allocate from heap → corrupt the 4KB stack gap
//  - SendReliable acquires ENet mutex → deadlock if game thread holds it
//  - PacketWriter has a destructor → stack unwinding crashes in detour context
//  - CharacterAccessor reads game memory → AV in wrong stack context
//
//  SOLUTION: The hook body does ONLY the minimum (call original + capture IDs
//  into a lock-free queue), then ProcessDeferredEvents() runs from the safe
//  OnGameTick context to do logging, packet building, and network sends.
// ═══════════════════════════════════════════════════════════════════════════

enum class CombatEventType : uint8_t { Death, KO };

struct DeferredCombatEvent {
    CombatEventType type;
    uint32_t entityId;
    uint32_t otherId;   // killer for death, attacker for KO
    uint8_t  reason;    // KO reason
    uint32_t generation;
    uint64_t sessionEpoch;
};

static constexpr int MAX_DEFERRED_EVENTS = 256;
static DeferredCombatEvent s_eventRing[MAX_DEFERRED_EVENTS];
static std::atomic<int> s_eventWriteIdx{0};
static std::atomic<int> s_eventReadIdx{0};
static std::atomic<int> s_dropCount{0}; // Tracks events dropped due to full buffer

struct HealthPollSample {
    uint32_t generation = 0;
    void* gameObject = nullptr;
    float health[KMP_LIMB_COUNT] = {};
    double lastSentTime = 0;
};

// Polling and memory access run only on the game thread. Disconnect can reset
// the cache from another thread, so cache/timer access shares one mutex.
static std::mutex g_healthMutex;
static std::unordered_map<EntityID, HealthPollSample> g_healthSamples;
static double g_healthTime = 0;
static float g_healthPollElapsed = KMP_HEALTH_POLL_INTERVAL;

static bool IsSimulationAuthority(const EntityInfo& info, PlayerID localPlayerId, bool isHost) {
    if (!info.gameObject || info.isRemote || info.state != EntityState::Active) {
        return false;
    }
    bool isCharacter = info.type == EntityType::PlayerCharacter ||
                       info.type == EntityType::NPC || info.type == EntityType::Animal;
    if (!isCharacter) {
        return false;
    }
    if (localPlayerId != INVALID_PLAYER && info.ownerPlayerId == localPlayerId) {
        return true;
    }
    return isHost && info.ownerPlayerId == 0 &&
           (info.type == EntityType::NPC || info.type == EntityType::Animal);
}

// Lock-free single-producer (hook) single-consumer (game tick) ring buffer.
// Safe because: hooks run on game thread (single producer), ProcessDeferredEvents
// runs on game thread (single consumer), and both are the SAME thread.
static bool PushEvent(const DeferredCombatEvent& evt) {
    int writeIdx = s_eventWriteIdx.load(std::memory_order_relaxed);
    int nextIdx = (writeIdx + 1) % MAX_DEFERRED_EVENTS;
    if (nextIdx == s_eventReadIdx.load(std::memory_order_acquire)) {
        s_dropCount.fetch_add(1, std::memory_order_relaxed);
        return false; // Full — drop event (better than crash)
    }
    s_eventRing[writeIdx] = evt;
    s_eventWriteIdx.store(nextIdx, std::memory_order_release);
    return true;
}

static bool PopEvent(DeferredCombatEvent& out) {
    int readIdx = s_eventReadIdx.load(std::memory_order_relaxed);
    if (readIdx == s_eventWriteIdx.load(std::memory_order_acquire)) {
        return false; // Empty
    }
    out = s_eventRing[readIdx];
    s_eventReadIdx.store((readIdx + 1) % MAX_DEFERRED_EVENTS, std::memory_order_release);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  HOOK BODIES — MINIMAL WORK ONLY
//  No spdlog. No PacketWriter. No SendReliable. No CharacterAccessor.
//  Just: call original + push event IDs to ring buffer.
// ═══════════════════════════════════════════════════════════════════════════

static void __fastcall Hook_CharacterDeath(void* character, void* killer) {
    s_deathCount.fetch_add(1, std::memory_order_relaxed);

    // Call original FIRST (SEH-protected) — game death logic must always run
    SafeCall_Void_PtrPtr(reinterpret_cast<void*>(s_origCharDeath),
                          character, killer, &s_deathHealth);

    // Skip if this death was triggered by the packet handler applying a server
    // event (S2C_CombatDeath). Without this, receiving a remote death would
    // push a new C2S_CombatDeath → server re-broadcasts → infinite echo.
    if (s_serverSourcedDeath.load(std::memory_order_acquire)) return;

    // Capture entity IDs (cheap pointer lookups, no allocation)
    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    EntityID entityId = core.GetEntityRegistry().GetNetId(character);
    if (entityId == INVALID_ENTITY) return;

    auto info = core.GetEntityRegistry().GetInfo(entityId);
    if (!info || !IsSimulationAuthority(*info, core.GetLocalPlayerId(), core.IsHost())) return;

    EntityID killerId = killer ? core.GetEntityRegistry().GetNetId(killer) : INVALID_ENTITY;

    // Defer all heavy work (logging, packet send) to ProcessDeferredEvents
    DeferredCombatEvent evt{};
    evt.type = CombatEventType::Death;
    evt.entityId = entityId;
    evt.otherId = killerId;
    evt.generation = info->generation;
    evt.sessionEpoch = core.GetSessionEpoch();
    PushEvent(evt);
}

static void __fastcall Hook_CharacterKO(void* character, void* attacker, int reason) {
    s_koCount.fetch_add(1, std::memory_order_relaxed);

    // Call original FIRST (SEH-protected) — game KO logic must always run
    SafeCall_Void_PtrPtrI(reinterpret_cast<void*>(s_origCharKO),
                           character, attacker, reason, &s_koHealth);

    // Skip if this KO was triggered by the packet handler applying a server
    // event (S2C_CombatKO). Prevents infinite echo loops.
    if (s_serverSourcedKO.load(std::memory_order_acquire)) return;

    // Capture entity IDs (cheap pointer lookups, no allocation)
    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    EntityID entityId = core.GetEntityRegistry().GetNetId(character);
    if (entityId == INVALID_ENTITY) return;

    auto info = core.GetEntityRegistry().GetInfo(entityId);
    if (!info || !IsSimulationAuthority(*info, core.GetLocalPlayerId(), core.IsHost())) return;

    EntityID attackerId = attacker ? core.GetEntityRegistry().GetNetId(attacker) : INVALID_ENTITY;

    // Defer all heavy work to ProcessDeferredEvents
    DeferredCombatEvent evt{};
    evt.type = CombatEventType::KO;
    evt.entityId = entityId;
    evt.otherId = attackerId;
    evt.reason = static_cast<uint8_t>(reason);
    evt.generation = info->generation;
    evt.sessionEpoch = core.GetSessionEpoch();
    PushEvent(evt);
}

static void SendLimbHealthUpdate(const EntityInfo& info, const float health[KMP_LIMB_COUNT]) {
    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_LimbHealth);
    writer.WriteU32(info.netId);
    writer.WriteU32(info.generation);
    for (int limb = 0; limb < KMP_LIMB_COUNT; limb++) {
        writer.WriteF32(health[limb]);
    }
    Core::Get().GetClient().SendReliable(writer.Data(), writer.Size());
}

void PollHealth(float deltaTime) {
    auto& core = Core::Get();
    if (!core.IsConnected() || !core.IsGameLoaded()) {
        Reset();
        return;
    }
    if (!std::isfinite(deltaTime) || deltaTime < 0.f) {
        return;
    }

    struct PendingHealth {
        EntityInfo info;
        float health[KMP_LIMB_COUNT];
    };
    std::vector<PendingHealth> updates;
    const uint64_t sessionEpoch = core.GetSessionEpoch();
    std::unique_lock<std::mutex> lock(g_healthMutex);
    g_healthTime += deltaTime;
    g_healthPollElapsed += deltaTime;
    if (g_healthPollElapsed < KMP_HEALTH_POLL_INTERVAL) {
        return;
    }
    g_healthPollElapsed = std::fmod(g_healthPollElapsed, KMP_HEALTH_POLL_INTERVAL);

    auto& registry = core.GetEntityRegistry();
    PlayerID localPlayerId = core.GetLocalPlayerId();
    bool isHost = core.IsHost();
    for (auto it = g_healthSamples.begin(); it != g_healthSamples.end();) {
        auto info = registry.GetInfo(it->first);
        if (!info || !IsSimulationAuthority(*info, localPlayerId, isHost) ||
            info->generation != it->second.generation || info->gameObject != it->second.gameObject) {
            it = g_healthSamples.erase(it);
        } else {
            ++it;
        }
    }

    for (EntityID entityId : registry.GetSimulationEntities(localPlayerId, isHost)) {
        auto info = registry.GetInfo(entityId);
        if (!info || !IsSimulationAuthority(*info, localPlayerId, isHost)) {
            continue;
        }
        float health[KMP_LIMB_COUNT];
        game::CharacterAccessor accessor(info->gameObject);
        if (!accessor.ReadLimbHealth(health)) {
            continue;
        }
        auto previous = g_healthSamples.find(entityId);
        if (previous != g_healthSamples.end() &&
            !HasLimbHealthChanged(health, previous->second.health) &&
            g_healthTime - previous->second.lastSentTime < KMP_HEALTH_KEYFRAME_INTERVAL) {
            continue;
        }

        // Observe the engine's final state after damage/healing. No native damage
        // hook or guessed damage signature is required, and unchanged limbs stay exact.
        registry.UpdateLimbHealth(entityId, health);
        PendingHealth update{};
        update.info = *info;
        std::copy(health, health + KMP_LIMB_COUNT, update.health);
        updates.push_back(update);
        auto& sample = g_healthSamples[entityId];
        sample.generation = info->generation;
        sample.gameObject = info->gameObject;
        sample.lastSentTime = g_healthTime;
        for (int limb = 0; limb < KMP_LIMB_COUNT; limb++) {
            sample.health[limb] = health[limb];
        }
    }
    // Never hold health state while taking the ENet mutex: disconnect callbacks reset health state.
    lock.unlock();
    for (const auto& update : updates) {
        if (!core.IsConnected() || core.GetSessionEpoch() != sessionEpoch) {
            break;
        }
        SendLimbHealthUpdate(update.info, update.health);
    }
}

void Reset() {
    std::lock_guard<std::mutex> lock(g_healthMutex);
    g_healthSamples.clear();
    g_healthTime = 0;
    g_healthPollElapsed = KMP_HEALTH_POLL_INTERVAL;
    s_eventReadIdx.store(s_eventWriteIdx.load(std::memory_order_acquire), std::memory_order_release);
    s_dropCount.store(0, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════════════════════════
//  DEFERRED PROCESSING — called from Core::OnGameTick (safe context)
//  Here we can safely: log, build packets, send via ENet, read game memory.
// ═══════════════════════════════════════════════════════════════════════════

void ProcessDeferredEvents() {
    auto& core = Core::Get();
    if (!core.IsConnected()) {
        // Drain queue without processing if disconnected
        DeferredCombatEvent discard;
        while (PopEvent(discard)) {}
        return;
    }

    // Report and reset drop counter (can't log from hook context, so we log here)
    int dropped = s_dropCount.exchange(0, std::memory_order_relaxed);
    if (dropped > 0) {
        spdlog::warn("combat_hooks: {} combat events DROPPED (ring buffer was full)", dropped);
    }

    DeferredCombatEvent evt;
    int processed = 0;
    while (processed < 64 && PopEvent(evt)) { // Cap per tick without discarding the 65th event
        processed++;

        auto infoCopy = core.GetEntityRegistry().GetInfo(evt.entityId);
        if (!infoCopy || evt.sessionEpoch != core.GetSessionEpoch() ||
            infoCopy->generation != evt.generation ||
            !IsSimulationAuthority(*infoCopy, core.GetLocalPlayerId(), core.IsHost())) {
            continue;
        }

        float limbHealth[KMP_LIMB_COUNT];
        game::CharacterAccessor accessor(infoCopy->gameObject);
        bool hasHealth = accessor.ReadLimbHealth(limbHealth);

        if (evt.type == CombatEventType::Death) {
            spdlog::info("combat_hooks: [deferred] Death — entity {} killer {}",
                         evt.entityId, evt.otherId);

            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_CombatDeath);
            writer.WriteU32(evt.entityId);
            writer.WriteU32(evt.otherId);
            core.GetClient().SendReliable(writer.Data(), writer.Size());

            // Piggyback limb health snapshot after death event
            if (hasHealth) {
                SendLimbHealthUpdate(*infoCopy, limbHealth);
            }

        } else if (evt.type == CombatEventType::KO) {
            spdlog::info("combat_hooks: [deferred] KO — entity {} attacker {} reason {}",
                         evt.entityId, evt.otherId, evt.reason);

            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_CombatKO);
            writer.WriteU32(evt.entityId);
            writer.WriteU32(evt.otherId);
            writer.WriteU8(static_cast<uint8_t>(BodyPart::Chest));

            // A failed health read must not invent a fatal health value.
            if (!hasHealth) {
                continue;
            }
            writer.WriteF32(limbHealth[static_cast<int>(BodyPart::Chest)]);
            core.GetClient().SendReliable(writer.Data(), writer.Size());

            // Piggyback limb health snapshot after KO event
            SendLimbHealthUpdate(*infoCopy, limbHealth);
        }
    }
}

// ── Install/Uninstall ──

bool Install() {
    auto& core = Core::Get();
    auto& hookMgr = HookManager::Get();
    auto& funcs = core.GetGameFunctions();

    // ═══ DO NOT HOOK ApplyDamage ═══
    // ApplyDamage (0x7A33A0) uses `mov rax, rsp` prologue and fires hundreds
    // of times per combat tick. The MovRaxRsp wrapper's global RSP slots corrupt
    // under rapid-fire calls → deterministic crash on "attack unprovoked".
    // Combat damage sync uses death/KO hooks + health polling instead.
    if (funcs.ApplyDamage) {
        spdlog::info("combat_hooks: ApplyDamage at 0x{:X} — NOT hooked (mov rax,rsp crash risk)",
                     reinterpret_cast<uintptr_t>(funcs.ApplyDamage));
    }

    if (funcs.CharacterDeath) {
        hookMgr.InstallAt("CharacterDeath",
                          reinterpret_cast<uintptr_t>(funcs.CharacterDeath),
                          &Hook_CharacterDeath, &s_origCharDeath);
    }

    if (funcs.CharacterKO) {
        hookMgr.InstallAt("CharacterKO",
                          reinterpret_cast<uintptr_t>(funcs.CharacterKO),
                          &Hook_CharacterKO, &s_origCharKO);
    }

    spdlog::info("combat_hooks: Installed (damage=SKIPPED, death={}, ko={})",
                 funcs.CharacterDeath != nullptr, funcs.CharacterKO != nullptr);
    return true;
}

void Uninstall() {
    HookManager::Get().Remove("CharacterDeath");
    HookManager::Get().Remove("CharacterKO");
    Reset();
    spdlog::info("combat_hooks: Uninstalled");
}

void SetServerSourcedDeath(bool active) {
    s_serverSourcedDeath.store(active, std::memory_order_release);
}

void SetServerSourcedKO(bool active) {
    s_serverSourcedKO.store(active, std::memory_order_release);
}

} // namespace kmp::combat_hooks

