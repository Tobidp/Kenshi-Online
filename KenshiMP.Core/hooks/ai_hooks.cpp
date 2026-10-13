#include "ai_hooks.h"
#include "../core.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <mutex>
#include <unordered_set>

namespace kmp::ai_hooks {

using AICreateFn = void*(__fastcall*)(void* character, void* faction);
using AIPackagesFn = void(__fastcall*)(void* character, void* aiPackage);

static AICreateFn g_origAICreate = nullptr;
static AIPackagesFn g_origAIPackages = nullptr;
static std::atomic<int> g_createCount{0};
static std::atomic<int> g_packageCount{0};
static std::unordered_set<void*> g_remoteControlled;
static std::mutex g_remoteMutex;

void MarkRemoteControlled(void* character) {
    if (!character) {
        return;
    }
    std::lock_guard lock(g_remoteMutex);
    if (g_remoteControlled.insert(character).second) {
        spdlog::debug("ai_hooks: Tracking replicated character 0x{:X} ({} total)",
                      reinterpret_cast<uintptr_t>(character), g_remoteControlled.size());
    }
}

void UnmarkRemoteControlled(void* character) {
    std::lock_guard lock(g_remoteMutex);
    g_remoteControlled.erase(character);
}

bool IsRemoteControlled(void* character) {
    std::lock_guard lock(g_remoteMutex);
    return character && g_remoteControlled.count(character) > 0;
}

void ClearRemoteControlled() {
    std::lock_guard lock(g_remoteMutex);
    g_remoteControlled.clear();
}

// Keep SEH boundaries free of C++ objects that require stack unwinding (MSVC C2712).
static void* CallAICreate(void* character, void* faction) {
    if (!g_origAICreate) {
        return nullptr;
    }
    __try {
        return g_origAICreate(character, faction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("ai_hooks: AICreate crashed, char=0x{:X}",
                      reinterpret_cast<uintptr_t>(character));
        return nullptr;
    }
}

static bool CallAIPackages(void* character, void* aiPackage) {
    if (!g_origAIPackages) {
        return false;
    }
    __try {
        g_origAIPackages(character, aiPackage);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("ai_hooks: AIPackages crashed, char=0x{:X}",
                      reinterpret_cast<uintptr_t>(character));
        return false;
    }
}

static void TrackRegisteredCharacter(void* character) {
    auto& core = Core::Get();
    if (!core.IsConnected() || !character) {
        return;
    }
    auto& registry = core.GetEntityRegistry();
    auto info = registry.GetInfo(registry.GetNetId(character));
    if (info.has_value() && info->isRemote) {
        MarkRemoteControlled(character);
    } else {
        UnmarkRemoteControlled(character);
    }
}

static void* __fastcall Hook_AICreate(void* character, void* faction) {
    // Every character needs a valid controller for combat, pathfinding and animation.
    // Tracking a replica must never cause this initialization to return nullptr.
    void* result = CallAICreate(character, faction);
    TrackRegisteredCharacter(character);
    int createCount = ++g_createCount;
    if (createCount % 100 == 1) {
        spdlog::debug("ai_hooks: AICreate #{} (char=0x{:X}, faction=0x{:X})",
                      createCount, reinterpret_cast<uintptr_t>(character),
                      reinterpret_cast<uintptr_t>(faction));
    }
    return result;
}

static void __fastcall Hook_AIPackages(void* character, void* aiPackage) {
    // This loads behavior data. It is not an AI update hook and cannot gate decisions.
    if (!CallAIPackages(character, aiPackage)) {
        return;
    }
    TrackRegisteredCharacter(character);
    int packageCount = ++g_packageCount;
    if (packageCount % 200 == 1) {
        spdlog::debug("ai_hooks: AIPackages #{} (char=0x{:X}, pkg=0x{:X})",
                      packageCount, reinterpret_cast<uintptr_t>(character),
                      reinterpret_cast<uintptr_t>(aiPackage));
    }
}

bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hooks = HookManager::Get();
    int installed = 0;

    if (funcs.AICreate && hooks.InstallAt("AICreate", reinterpret_cast<uintptr_t>(funcs.AICreate),
                                         &Hook_AICreate, &g_origAICreate)) {
        installed++;
        spdlog::info("ai_hooks: AICreate hook installed");
    }
    if (funcs.AIPackages && hooks.InstallAt("AIPackages", reinterpret_cast<uintptr_t>(funcs.AIPackages),
                                           &Hook_AIPackages, &g_origAIPackages)) {
        installed++;
        spdlog::info("ai_hooks: AIPackages hook installed");
    }

    spdlog::info("ai_hooks: {}/2 initialization hooks installed; AI decision suppression unavailable", installed);
    return installed > 0;
}

void Uninstall() {
    auto& hooks = HookManager::Get();
    if (g_origAICreate) {
        hooks.Remove("AICreate");
    }
    if (g_origAIPackages) {
        hooks.Remove("AIPackages");
    }
    g_origAICreate = nullptr;
    g_origAIPackages = nullptr;
    ClearRemoteControlled();
}

} // namespace kmp::ai_hooks
