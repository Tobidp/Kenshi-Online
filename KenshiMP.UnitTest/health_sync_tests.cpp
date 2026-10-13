// KenshiMP.UnitTest - health_sync_tests.cpp
// Regressions for invalid game memory and exact health snapshot replication.
#include "health_sync_tests.h"
#include "game/game_types.h"
#include "kmp/health_sync.h"
#include "kmp/memory.h"
#include <array>
#include <cstring>
#include <limits>

namespace kmp {
namespace {

struct FakeHealthCharacter {
    alignas(uintptr_t) std::array<uint8_t, 0x1000> character{};
    alignas(uintptr_t) std::array<uint8_t, 0x1000> first{};
    alignas(uintptr_t) std::array<uint8_t, 0x1000> second{};

    FakeHealthCharacter() {
        auto& offsets = kmp::game::GetOffsets().character;
        uintptr_t firstAddress = reinterpret_cast<uintptr_t>(first.data());
        uintptr_t secondAddress = reinterpret_cast<uintptr_t>(second.data());
        std::memcpy(character.data() + offsets.healthChain1, &firstAddress, sizeof(firstAddress));
        std::memcpy(first.data() + offsets.healthChain2, &secondAddress, sizeof(secondAddress));
        for (int limb = 0; limb < kmp::KMP_LIMB_COUNT; limb++) {
            float health = 100.f + limb;
            float stun = 5.f + limb;
            std::memcpy(second.data() + offsets.healthBase + limb * offsets.healthStride,
                        &health, sizeof(health));
            std::memcpy(second.data() + offsets.healthBase + limb * offsets.healthStride + sizeof(float),
                        &stun, sizeof(stun));
        }
    }
};

bool IsFilled(const float values[kmp::KMP_LIMB_COUNT], float value) {
    for (int limb = 0; limb < kmp::KMP_LIMB_COUNT; limb++) {
        if (values[limb] != value) {
            return false;
        }
    }
    return true;
}

} // namespace

void RunHealthSyncTests(void (*assertion)(bool, const char*)) {
    if (!assertion) {
        return;
    }
    auto& offsets = kmp::game::GetOffsets().character;
    kmp::game::CharacterOffsets originalOffsets = offsets;
    offsets.health = -1;
    offsets.healthChain1 = 0x2B8;
    offsets.healthChain2 = 0x5F8;
    offsets.healthBase = 0x40;
    offsets.healthStride = 8;

    float baseline[kmp::KMP_LIMB_COUNT] = {100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
    float health[kmp::KMP_LIMB_COUNT] = {-150.f, 250.f, 0.f, 80.f, 70.f, 60.f, 50.f};
    assertion(kmp::IsFiniteLimbHealth(health), "Health permits negative, zero and race-specific maximums");
    assertion(!kmp::IsFiniteLimbHealth(nullptr), "Health validation rejects a null snapshot");
    float changed[kmp::KMP_LIMB_COUNT] = {99.95f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
    assertion(!kmp::HasLimbHealthChanged(changed, baseline), "Sub-threshold health changes do not flood packets");
    changed[0] = 99.85f;
    assertion(kmp::HasLimbHealthChanged(changed, baseline), "Small health changes accumulate against last send");

    FakeHealthCharacter fake;
    kmp::game::CharacterAccessor accessor(fake.character.data());
    float read[kmp::KMP_LIMB_COUNT] = {};
    assertion(accessor.ReadLimbHealth(read) && read[0] == 100.f && read[6] == 106.f,
              "Confirmed health chain reads all seven body parts");
    assertion(accessor.WriteLimbHealth(health), "Validated snapshot writes to confirmed health chain");
    assertion(accessor.ReadLimbHealth(read) && std::memcmp(read, health, sizeof(health)) == 0,
              "Limb health round-trips exact values without clamping");
    bool stunPreserved = true;
    for (int limb = 0; limb < kmp::KMP_LIMB_COUNT; limb++) {
        float stun = 0.f;
        std::memcpy(&stun, fake.second.data() + offsets.healthBase + limb * offsets.healthStride + sizeof(float),
                    sizeof(stun));
        stunPreserved = stunPreserved && stun == 5.f + limb;
    }
    assertion(stunPreserved, "Health replication preserves every interleaved stun value");

    health[3] = std::numeric_limits<float>::quiet_NaN();
    assertion(!kmp::IsFiniteLimbHealth(health), "Health validation rejects NaN");
    assertion(!accessor.WriteLimbHealth(health), "NaN snapshot is rejected before any memory write");
    assertion(accessor.ReadLimbHealth(read) && read[0] == -150.f && read[3] == 80.f,
              "Invalid snapshot leaves all earlier and later limbs untouched");
    health[3] = std::numeric_limits<float>::infinity();
    assertion(!kmp::IsFiniteLimbHealth(health), "Health validation rejects infinity");

    float invalid = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(fake.second.data() + offsets.healthBase + 6 * offsets.healthStride, &invalid, sizeof(invalid));
    for (float& value : read) {
        value = 777.f;
    }
    assertion(!accessor.ReadLimbHealth(read) && IsFilled(read, 777.f),
              "An invalid last limb does not partially replace the output snapshot");

    uintptr_t nullAddress = 0;
    std::memcpy(fake.first.data() + offsets.healthChain2, &nullAddress, sizeof(nullAddress));
    assertion(!accessor.ReadLimbHealth(read) && IsFilled(read, 777.f),
              "A broken pointer chain reports failure without inventing health values");
    kmp::game::CharacterAccessor nullAccessor(nullptr);
    assertion(!nullAccessor.ReadLimbHealth(read), "Null character cannot produce a health snapshot");

    offsets.health = 0x100;
    offsets.healthStride = sizeof(float);
    std::memcpy(fake.character.data() + offsets.health, baseline, sizeof(baseline));
    assertion(accessor.ReadLimbHealth(read) && IsFilled(read, 100.f),
              "Scanner-discovered contiguous health layout can be read");
    assertion(accessor.WriteLimbHealth(changed) && accessor.ReadLimbHealth(read) && read[0] == changed[0],
              "Scanner-discovered contiguous health layout can be written");
    offsets.healthStride = 3;
    assertion(!accessor.WriteLimbHealth(baseline), "An unverified health stride cannot corrupt character memory");

    // Make a health span cross into an inaccessible page. A loop of individual
    // writes would change earlier limbs before failing on the protected page.
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    size_t pageSize = systemInfo.dwPageSize;
    auto* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, pageSize * 2, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE));
    assertion(pages != nullptr, "Allocate boundary memory for partial-write regression");
    if (pages) {
        uint8_t* boundary = pages + pageSize - 3 * sizeof(float);
        for (int limb = 0; limb < kmp::KMP_LIMB_COUNT; limb++) {
            std::memcpy(boundary + limb * 2 * sizeof(float), &baseline[limb], sizeof(float));
        }
        DWORD previousProtection = 0;
        bool protectedPage = VirtualProtect(pages + pageSize, pageSize, PAGE_NOACCESS,
                                             &previousProtection) != FALSE;
        assertion(protectedPage, "Protect the page containing later limbs");
        if (protectedPage) {
            offsets.health = 0;
            offsets.healthStride = 8;
            kmp::game::CharacterAccessor boundaryAccessor(boundary);
            for (float& value : read) {
                value = 777.f;
            }
            assertion(!boundaryAccessor.ReadLimbHealth(read) && IsFilled(read, 777.f),
                      "Unreadable later limbs preserve the entire read output");
            assertion(!boundaryAccessor.WriteLimbHealth(changed),
                      "Unreadable later limbs reject the entire write before modification");
            float firstLimb = 0.f;
            std::memcpy(&firstLimb, boundary, sizeof(firstLimb));
            assertion(firstLimb == 100.f, "Rejected cross-page write leaves the first limb unchanged");
        }
        VirtualFree(pages, 0, MEM_RELEASE);
    }
    offsets = originalOffsets;
}
} // namespace kmp
