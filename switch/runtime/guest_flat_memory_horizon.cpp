// GuestFlat for Horizon (the Switch build replaces runtime/src/guest_flat_memory.cpp with this file).
//
// The flat path: one 4 GiB window, taken from free code-region space at startup. Horizon randomizes
// its address space, so the base is a runtime value (mkw_flat_guest_base, guest_flat_memory.h) and
// not the fixed constant the other platforms use. Each backing store (MEM1, MEM2, and each "owned"
// region such as the locked cache) is a heap block that svcMapProcessCodeMemory maps into the window
// at its primary view's address, made read/write with svcSetProcessMemoryPermission. Every other
// view of the store, the mirrors (MEM1 at 0x00000000, 0x80000000 and 0xC0000000, MEM2 at 0x10000000,
// 0x90000000 and 0xD0000000), is an svcMapProcessMemory alias of that primary view. A guest access is
// then `*(window + address)` plus a byte swap, as on the other platforms.
//
// Why code memory: svcMapProcessMemory only takes source pages whose kernel state allows it, and plain
// heap doesn't, while code memory does. The flatmem-spike test proved this sequence on hardware,
// including the reverse order of unmapping (switch/docs/GUEST-MEMORY-PLAN.md).
//
// What this gives up against Windows and Linux (macOS ships the same way): there are no page traps.
// An MMIO read or a wild guest pointer is a data abort, which the crash handler
// (switch/source/spm_switch_init.cpp) reports with the guest address. Deferred EFB reads aren't
// intercepted (nothing in this build registers one), and the executable-write guard is gone. MMIO
// writes still reach GX HLE through the inline check in memory_access.h.
//
// HostPointer() gives every mirror of a store the same pointer, into its primary view, which is what
// the bring-up mode below always did.
//
// Bring-up mode (configure with SPM_SWITCH_CHECKED_GUEST_MEMORY=ON): no window and no SVCs.
// HostPointer() points into the heap blocks, and every guest access takes the checked Memory::* path
// (RequiresCheckedAccess() is a compile-time true). It is slower, but it runs where the SVCs aren't
// granted and it reports a bad access with the guest's CPU state.
//
// Unmapping matters: hbloader runs the next NRO in this same process, and heap pages still mapped as
// code memory would break it. userAppExit() calls spm_switch_guest_memory_release().
//
// switch/tests/guest_flat_memory_horizon_tests.cpp builds this file on a PC (no __SWITCH__), where
// mmap of a shared file stands in for the SVCs, so the test checks the same layout and aliasing.
#include "guest_flat_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "runtime_log.h"

extern "C" void spm_switch_set_flat_guest_base(uint8_t* base);  // runtime/horizon_flat_base.c

namespace GuestFlat {

namespace {

constexpr size_t kAllocationGranularity = 0x10000;  // 64 KiB, as the desktop mapping requires
// The heap blocks and the window are 2 MiB aligned, so every view sits at the same offset in a 2 MiB
// block as the memory under it. Where the heap's physical memory allows, the kernel can then map the
// views with 2 MiB blocks instead of 4 KiB pages: fewer TLB misses across 155 MiB of guest RAM.
constexpr size_t kLargePage = 0x200000;

struct Store {
    Backing backing = Backing::Owned;
    uint32_t ownedBase = 0;
    uint64_t size = 0;
    uint32_t primary = 0;      // guest address of the view the heap block is mapped at
    bool hasPrimary = false;
    uint8_t* heap = nullptr;   // the block itself
    uint8_t* host = nullptr;   // where runtime code reaches it: the primary view (or the heap block)
    bool codeMapped = false;
#if !defined(__SWITCH__)
    int fd = -1;
#endif
};

struct View {
    uint32_t guestBase = 0;
    uint64_t guestSize = 0;  // the region's own size, which bounds HostPointer()
    uint64_t mapSize = 0;    // page-rounded, what the window maps
    uint64_t offset = 0;     // into the store
    size_t store = 0;
    bool mirror = false;     // an alias of the primary view, not the primary view itself
    bool mapped = false;
};

// Never destroyed, unlike memory.cpp's function-local statics: userAppExit() unmaps the window after
// exit() has run the static destructors, so the bookkeeping it walks has to outlive them.
struct State {
    std::mutex mutex;
    std::vector<Store> stores;
    std::vector<View> views;
    std::vector<RegionRequest> layout;
};

State& GetState() {
    static State* const state = new State();
    return *state;
}

std::mutex& Mutex() { return GetState().mutex; }
std::vector<Store>& Stores() { return GetState().stores; }
std::vector<View>& Views() { return GetState().views; }
std::vector<RegionRequest>& Layout() { return GetState().layout; }

uint8_t* g_window = nullptr;
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

size_t FindStore(const RegionRequest& region) {
    const uint32_t owned = OwnedBase(region);
    for (size_t i = 0; i < Stores().size(); ++i) {
        if (Stores()[i].backing == region.backing && Stores()[i].ownedBase == owned) return i;
    }
    return Stores().size();
}

bool SameLayout(const std::vector<RegionRequest>& lhs, const std::vector<RegionRequest>& rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](const auto& a, const auto& b) {
               return a.base == b.base && a.size == b.size && a.backing == b.backing;
           });
}

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error("Flat guest memory: " + message);
}

// The platform layer: the window, a store's primary view, and a mirror view.
#if defined(__SWITCH__)

Handle g_self = INVALID_HANDLE;
VirtmemReservation* g_reservation = nullptr;

std::string SvcError(const char* what, Result rc) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed: 0x%X (module %u, description %u)", what, rc,
                  R_MODULE(rc), R_DESCRIPTION(rc));
    return text;
}

void ReserveWindow() {
    struct Svc { unsigned id; const char* name; };
    static constexpr Svc kSvcs[] = {
        {0x73, "SetProcessMemoryPermission"}, {0x74, "MapProcessMemory"},
        {0x75, "UnmapProcessMemory"}, {0x77, "MapProcessCodeMemory"},
        {0x78, "UnmapProcessCodeMemory"},
    };
    for (const Svc& svc : kSvcs) {
        if (!envIsSyscallHinted(svc.id)) {
            char text[400];
            std::snprintf(text, sizeof(text),
                          "svc 0x%02X (%s) isn't available to this NRO, and the flat guest window "
                          "needs it. Launch spm-nx from the Homebrew Menu with title takeover. A "
                          "build configured with SPM_SWITCH_CHECKED_GUEST_MEMORY=ON runs without it, "
                          "more slowly.",
                          svc.id, svc.name);
            Fail(text);
        }
    }
    g_self = envGetOwnProcessHandle();

    const size_t span = kGuestSpaceSize + kLargePage;
    virtmemLock();
    void* found = virtmemFindCodeMemory(span, kAllocationGranularity);
    if (found != nullptr) g_reservation = virtmemAddReservation(found, span);
    virtmemUnlock();
    if (g_reservation == nullptr) Fail("no free 4 GiB range in the code region for the window.");
    g_window = reinterpret_cast<uint8_t*>(RoundUp(reinterpret_cast<uintptr_t>(found), kLargePage));
}

void MapPrimary(Store& store) {
    const u64 view = reinterpret_cast<u64>(g_window + store.primary);
    Result rc = svcMapProcessCodeMemory(g_self, view, reinterpret_cast<u64>(store.heap), store.size);
    if (R_FAILED(rc)) Fail(SvcError("svcMapProcessCodeMemory", rc));
    store.codeMapped = true;
    rc = svcSetProcessMemoryPermission(g_self, view, store.size, Perm_Rw);
    if (R_FAILED(rc)) Fail(SvcError("svcSetProcessMemoryPermission", rc));
}

void MapMirror(View& view, const Store& store) {
    const Result rc = svcMapProcessMemory(g_window + view.guestBase, g_self,
                                          reinterpret_cast<u64>(store.host + view.offset), view.mapSize);
    if (R_FAILED(rc)) Fail(SvcError("svcMapProcessMemory", rc));
    view.mapped = true;
}

bool UnmapMirror(View& view, const Store& store) {
    const Result rc = svcUnmapProcessMemory(g_window + view.guestBase, g_self,
                                            reinterpret_cast<u64>(store.host + view.offset), view.mapSize);
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "[switch] guest memory: %s\n", SvcError("svcUnmapProcessMemory", rc).c_str());
        return false;
    }
    view.mapped = false;
    return true;
}

bool UnmapPrimary(Store& store) {
    const Result rc = svcUnmapProcessCodeMemory(g_self, reinterpret_cast<u64>(g_window + store.primary),
                                                reinterpret_cast<u64>(store.heap), store.size);
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "[switch] guest memory: %s\n", SvcError("svcUnmapProcessCodeMemory", rc).c_str());
        return false;
    }
    store.codeMapped = false;
    return true;
}

void ReleaseWindow() {
    if (g_reservation != nullptr) {
        virtmemLock();
        virtmemRemoveReservation(g_reservation);
        virtmemUnlock();
        g_reservation = nullptr;
    }
    g_window = nullptr;
}

#else  // PC stand-ins for the test: a shared file per store, mapped at each of its views.

void* g_reservation = nullptr;

void ReserveWindow() {
    const size_t span = kGuestSpaceSize + kLargePage;
    void* found = mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (found == MAP_FAILED) Fail("no 4 GiB of address space for the window.");
    g_reservation = found;
    g_window = reinterpret_cast<uint8_t*>(RoundUp(reinterpret_cast<uintptr_t>(found), kLargePage));
}

void MapAt(uint8_t* address, uint64_t size, int fd, uint64_t offset) {
    if (mmap(address, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
             static_cast<off_t>(offset)) != address) {
        Fail("mmap of a view failed.");
    }
}

void MapPrimary(Store& store) {
    char name[] = "/tmp/spm-guest-XXXXXX";
    store.fd = mkstemp(name);
    if (store.fd < 0) Fail("mkstemp failed.");
    unlink(name);
    if (ftruncate(store.fd, static_cast<off_t>(store.size)) != 0) Fail("ftruncate failed.");
    MapAt(g_window + store.primary, store.size, store.fd, 0);
    store.codeMapped = true;
}

void MapMirror(View& view, const Store& store) {
    MapAt(g_window + view.guestBase, view.mapSize, store.fd, view.offset);
    view.mapped = true;
}

bool UnmapMirror(View& view, const Store&) {
    view.mapped = false;
    return true;
}

bool UnmapPrimary(Store& store) {
    close(store.fd);
    store.fd = -1;
    store.codeMapped = false;
    return true;
}

void ReleaseWindow() {
    if (g_reservation != nullptr) munmap(g_reservation, kGuestSpaceSize + kLargePage);
    g_reservation = nullptr;
    g_window = nullptr;
}

#endif

// Undoes Initialize, in reverse; also the cleanup after a failed one. A store whose primary view
// won't unmap keeps its heap block, which the kernel still has mapped.
void ReleaseLocked() {
    spm_switch_set_flat_guest_base(nullptr);
    g_active = false;
    for (auto view = Views().rbegin(); view != Views().rend(); ++view) {
        if (view->mapped) UnmapMirror(*view, Stores()[view->store]);
    }
    for (auto store = Stores().rbegin(); store != Stores().rend(); ++store) {
        if (store->codeMapped && !UnmapPrimary(*store)) continue;
        std::free(store->heap);
    }
    ReleaseWindow();
    Stores().clear();
    Views().clear();
    Layout().clear();
}

void InitializeLocked(const std::vector<RegionRequest>& regions) {
    // Size every store from the highest byte any of its regions reaches.
    for (const auto& region : regions) {
        if (region.size == 0) continue;
        if ((region.base % kAllocationGranularity) != 0) {
            std::ostringstream oss;
            oss << "guest region base 0x" << std::hex << region.base << " is not 64 KiB aligned.";
            Fail(oss.str());
        }
        const uint64_t end = StoreOffset(region) + region.size;
        const size_t index = FindStore(region);
        if (index < Stores().size()) {
            Stores()[index].size = std::max(Stores()[index].size, end);
        } else {
            Store store;
            store.backing = region.backing;
            store.ownedBase = OwnedBase(region);
            store.size = end;
            Stores().push_back(store);
        }
    }
    for (auto& store : Stores()) store.size = RoundUp(store.size, kAllocationGranularity);

    // Each store's primary view is the first region that covers all of it; the rest alias it.
    for (const auto& region : regions) {
        if (region.size == 0) continue;
        View view;
        view.guestBase = region.base;
        view.guestSize = region.size;
        view.mapSize = RoundUp(region.size, kGuestPageSize);
        view.offset = StoreOffset(region);
        view.store = FindStore(region);
        Store& store = Stores()[view.store];
        if (!store.hasPrimary && view.offset == 0 &&
            RoundUp(region.size, kAllocationGranularity) == store.size) {
            store.primary = region.base;
            store.hasPrimary = true;
        } else {
            view.mirror = true;
        }
        Views().push_back(view);
    }
    for (const auto& view : Views()) {
        if (!Stores()[view.store].hasPrimary) {
            std::ostringstream oss;
            oss << "no region covers all of the backing store behind guest address 0x" << std::hex
                << view.guestBase << ", so nothing can be its primary view.";
            Fail(oss.str());
        }
    }

    for (auto& store : Stores()) {
        store.heap = static_cast<uint8_t*>(
            std::aligned_alloc(kLargePage, static_cast<size_t>(RoundUp(store.size, kLargePage))));
        if (store.heap == nullptr) {
            std::ostringstream oss;
            oss << "unable to allocate 0x" << std::hex << store.size << std::dec
                << " bytes of guest RAM. Title takeover (launching through a game, not the album "
                   "applet) gives the homebrew the memory it needs.";
            Fail(oss.str());
        }
        // The desktop stores are demand-zero; the heap is not. Once mapped, this address is the
        // kernel's until it is unmapped, so it is cleared first.
        std::memset(store.heap, 0, static_cast<size_t>(store.size));
        store.host = store.heap;
    }

    if (!RequiresCheckedAccess()) {
        ReserveWindow();
        for (auto& store : Stores()) {
            MapPrimary(store);
            store.host = g_window + store.primary;
        }
        for (auto& view : Views()) {
            if (view.mirror) MapMirror(view, Stores()[view.store]);
        }
        spm_switch_set_flat_guest_base(g_window);
    }
}

} // namespace

bool IsActive() {
    return g_active;
}

void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard<std::mutex> lock(Mutex());

    if (g_active) {
        if (!SameLayout(Layout(), regions)) {
            throw std::runtime_error(
                "The guest memory layout is fixed once per process; a second Memory::Init asked "
                "for a different one.");
        }
        // Re-init keeps the stores and clears them, like the desktop build.
        for (auto& store : Stores()) std::memset(store.host, 0, static_cast<size_t>(store.size));
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    try {
        InitializeLocked(regions);
    } catch (...) {
        ReleaseLocked();
        throw;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    Layout() = regions;
    g_active = true;

    uint64_t total = 0;
    for (const auto& store : Stores()) total += store.size;
    if (RequiresCheckedAccess()) {
        RT_LOG(RT_TAG_MEMORY) << "guest RAM on the heap (checked access, no flat view): "
                              << Views().size() << " regions, " << Stores().size()
                              << " backing stores, " << (total >> 20) << " MiB" << std::endl;
    } else {
        RT_LOG(RT_TAG_MEMORY) << "guest RAM in a flat 4 GiB window at "
                              << static_cast<const void*>(g_window) << ": " << Views().size()
                              << " views of " << Stores().size() << " backing stores, "
                              << (total >> 20) << " MiB, mapped in " << ms << " ms" << std::endl;
    }
}

uint8_t* HostPointer(uint32_t guestAddress) {
    if (!g_active) return nullptr;
    for (const auto& view : Views()) {
        if (guestAddress < view.guestBase) continue;
        const uint64_t delta = static_cast<uint64_t>(guestAddress) - view.guestBase;
        if (delta >= view.guestSize) continue;
        return Stores()[view.store].host + view.offset + delta;
    }
    return nullptr;
}

// No page protection on Horizon; in bring-up mode the checked path applies these policies itself.
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

// userAppExit() (switch/source/spm_switch_init.cpp): unmaps the window so hbloader gets its heap back.
// Every pointer into guest RAM dies with it, so nothing may touch guest memory afterwards. The crash
// handler's exit comes through here too, possibly on the thread that holds the lock, so it doesn't wait.
extern "C" void spm_switch_guest_memory_release(void) {
    std::unique_lock<std::mutex> lock(GuestFlat::Mutex(), std::try_to_lock);
    if (lock.owns_lock()) GuestFlat::ReleaseLocked();
}
