// GuestFlat for Horizon (the Switch build replaces runtime/src/guest_flat_memory.cpp with this file).
//
// Bring-up mode (this file today): every guest access takes the checked Memory::* path, so there is
// no flat 4 GiB view at all (RequiresCheckedAccess() is a compile-time true on __SWITCH__, in
// guest_flat_memory.h). Each backing store (MEM1, MEM2, and each "owned" region such as the
// locked cache) is one page-aligned heap block, and HostPointer() hands out pointers into it, the
// same "host view" the other platforms create. The mirrors (MEM1 at 0x00000000, 0x80000000 and
// 0xC0000000, MEM2 at 0x10000000, 0x90000000 and 0xD0000000) need no aliasing here: the page table
// in memory.cpp maps every mirror region onto the same host pointer, because HostPointer() resolves
// each one into its shared store with the same offsets the desktop build uses.
//
// What that costs: every guest load and store pays the checked path's page-table lookup. Nothing is
// lost functionally: with no page protection, the checked path applies the MMIO, deferred-EFB-read
// and executable-write policies in software (memory_access.h), which is the same mode desktop
// AArch64 kernels with pages over 4 KiB already run in. The flat fast path (a real 4 GiB view
// at kFixedFlatGuestBase, aliased onto these stores with svcMapProcessMemory) is phase 2 of
// switch/docs/GUEST-MEMORY-PLAN.md.
//
// A fault whose address lies in [kFixedFlatGuestBase, +4 GiB) means some code still used the flat
// view; the crash report's far address makes that easy to spot.
#include "guest_flat_memory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "runtime_log.h"

namespace GuestFlat {

namespace {

constexpr size_t kAllocationGranularity = 0x10000;  // 64 KiB, as the desktop mapping requires

struct Store {
    Backing backing = Backing::Owned;
    uint32_t ownedBase = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

struct Mapping {
    uint32_t guestBase = 0;
    uint64_t guestSize = 0;
    uint8_t* host = nullptr;  // host address of guestBase
};

std::mutex g_mutex;
std::vector<Store> g_stores;
std::vector<Mapping> g_mappings;
std::vector<RegionRequest> g_layout;
bool g_active = false;

uint64_t RoundUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

// Same offsets as guest_flat_memory.cpp's SectionOffsetFor, so mirrors land on the same bytes.
uint64_t StoreOffset(const RegionRequest& region) {
    switch (region.backing) {
    case Backing::Mem1:
        return region.base & 0x01FFFFFFu;  // 32 MiB MEM1 window
    case Backing::Mem2:
        return region.base & 0x0FFFFFFFu;  // 256 MiB MEM2 window
    case Backing::Owned:
    default:
        return 0;
    }
}

uint32_t OwnedBase(const RegionRequest& region) {
    return region.backing == Backing::Owned ? region.base : 0u;
}

Store* FindStore(const RegionRequest& region) {
    const uint32_t owned = OwnedBase(region);
    for (auto& store : g_stores) {
        if (store.backing == region.backing && store.ownedBase == owned) return &store;
    }
    return nullptr;
}

bool SameLayout(const std::vector<RegionRequest>& lhs, const std::vector<RegionRequest>& rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](const auto& a, const auto& b) {
               return a.base == b.base && a.size == b.size && a.backing == b.backing;
           });
}

} // namespace

bool IsActive() {
    return g_active;
}

void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_active) {
        if (!SameLayout(g_layout, regions)) {
            throw std::runtime_error(
                "The guest memory layout is fixed once per process; a second Memory::Init asked "
                "for a different one.");
        }
        // Re-init keeps the stores and clears them, like the desktop build.
        for (auto& store : g_stores) std::memset(store.host, 0, static_cast<size_t>(store.size));
        return;
    }

    // Size every store from the highest byte any of its regions reaches.
    for (const auto& region : regions) {
        if (region.size == 0) continue;
        if ((region.base % kAllocationGranularity) != 0) {
            std::ostringstream oss;
            oss << "Guest region base 0x" << std::hex << region.base << " is not 64 KiB aligned.";
            throw std::runtime_error(oss.str());
        }
        const uint64_t end = StoreOffset(region) + region.size;
        if (Store* store = FindStore(region)) {
            store->size = std::max(store->size, end);
        } else {
            g_stores.push_back(Store{region.backing, OwnedBase(region), end, nullptr});
        }
    }

    uint64_t total = 0;
    for (auto& store : g_stores) {
        store.size = RoundUp(store.size, kAllocationGranularity);
        store.host = static_cast<uint8_t*>(std::aligned_alloc(kAllocationGranularity,
                                                              static_cast<size_t>(store.size)));
        if (store.host == nullptr) {
            std::ostringstream oss;
            oss << "Unable to allocate 0x" << std::hex << store.size << std::dec
                << " bytes of guest RAM. Title takeover (launching through a game, not the album "
                   "applet) gives the homebrew the memory it needs.";
            throw std::runtime_error(oss.str());
        }
        // The desktop stores are demand-zero; the heap is not.
        std::memset(store.host, 0, static_cast<size_t>(store.size));
        total += store.size;
    }

    for (const auto& region : regions) {
        if (region.size == 0) continue;
        const Store* store = FindStore(region);
        g_mappings.push_back(Mapping{region.base, region.size, store->host + StoreOffset(region)});
    }

    g_layout = regions;
    g_active = true;

    RT_LOG(RT_TAG_MEMORY) << "guest RAM on the heap (checked access, no flat view): "
                          << g_mappings.size() << " regions, " << g_stores.size()
                          << " backing stores, " << (total >> 20) << " MiB" << std::endl;
}

uint8_t* HostPointer(uint32_t guestAddress) {
    if (!g_active) return nullptr;
    for (const auto& mapping : g_mappings) {
        if (guestAddress < mapping.guestBase) continue;
        const uint64_t offset = static_cast<uint64_t>(guestAddress) - mapping.guestBase;
        if (offset >= mapping.guestSize) continue;
        return mapping.host + offset;
    }
    return nullptr;
}

// No page protection in bring-up mode; the checked path applies these policies itself.
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}

FaultCounters Counters() {
    return {};
}

void LogFaultSummary() noexcept {}

bool HandleAccessViolation(void*, bool) noexcept {
    return false;
}

} // namespace GuestFlat
