// Host-side test of switch/runtime/guest_flat_memory_horizon.cpp. Built without __SWITCH__, the file
// maps its 4 GiB window with mmap of a shared file per store in place of the SVCs, so this runs on a
// PC. Uses the region layout runtime/src/memory.cpp builds for the game and checks that every mirror
// of MEM1 and MEM2 lands on the same bytes, through HostPointer() and through the flat window the
// translated code uses, and that owned regions stay apart.
//   c++ -std=c++20 -I../../wiicompiled/runtime/include guest_flat_memory_horizon_tests.cpp \
//       ../runtime/guest_flat_memory_horizon.cpp ../runtime/horizon_flat_base.c -o t && ./t
#include "guest_flat_memory.h"

#include <cstdio>
#include <cstring>
#include <vector>

using GuestFlat::Backing;

// runtime/horizon_flat_base.c and the release hook; on the console guest_flat_memory.h and
// spm_switch_init.cpp declare these.
extern "C" uint8_t* mkw_flat_guest_base;
extern "C" void spm_switch_guest_memory_release(void);

namespace {
uint32_t LoadThroughWindow(uint32_t guestAddress) {
    uint32_t value = 0;
    std::memcpy(&value, mkw_flat_guest_base + guestAddress, sizeof(value));
    return value;
}
void StoreThroughWindow(uint32_t guestAddress, uint32_t value) {
    std::memcpy(mkw_flat_guest_base + guestAddress, &value, sizeof(value));
}
} // namespace

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

constexpr uint32_t kMem1 = 24u << 20;
constexpr uint32_t kMem2 = 128u << 20;

const std::vector<GuestFlat::RegionRequest> kLayout = {
    {0x00000000u, kMem1, Backing::Mem1},
    {0x80000000u, kMem1, Backing::Mem1},
    {0xC0000000u, kMem1, Backing::Mem1},
    {0x10000000u, kMem2, Backing::Mem2},
    {0x90000000u, kMem2, Backing::Mem2},
    {0xD0000000u, kMem2, Backing::Mem2},
    {0x81800000u, 0x200000u, Backing::Owned},          // MEM1_KAMEK_OVERLAY
    {0xE0000000u, 0x100000u + 4096u, Backing::Owned},  // LOCKED_CACHE
};

int main() {
    GuestFlat::Initialize(kLayout);
    CHECK(GuestFlat::IsActive());

    // The window: published for translated code, 2 MiB aligned so the views can use 2 MiB blocks.
    CHECK(mkw_flat_guest_base != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(mkw_flat_guest_base) % 0x200000u == 0);

    // MEM1 mirrors share bytes, at the same offset in each view.
    uint8_t* cached = GuestFlat::HostPointer(0x80001234u);
    CHECK(cached != nullptr);
    CHECK(GuestFlat::HostPointer(0xC0001234u) == cached);
    CHECK(GuestFlat::HostPointer(0x00001234u) == cached);
    *cached = 0x5A;
    CHECK(*GuestFlat::HostPointer(0xC0001234u) == 0x5A);

    // Through the window: a store through any mirror is seen by every other one, and by HostPointer().
    for (const uint32_t writer : {0x00002000u, 0x80002000u, 0xC0002000u}) {
        StoreThroughWindow(writer, writer ^ 0xA5A5A5A5u);
        for (const uint32_t reader : {0x00002000u, 0x80002000u, 0xC0002000u}) {
            CHECK(LoadThroughWindow(reader) == (writer ^ 0xA5A5A5A5u));
        }
    }
    CHECK(GuestFlat::HostPointer(0x80002000u)[0] == mkw_flat_guest_base[0xC0002000u]);
    StoreThroughWindow(0xD0000000u + kMem2 - 4u, 0x12345678u);
    CHECK(LoadThroughWindow(0x10000000u + kMem2 - 4u) == 0x12345678u);
    CHECK(LoadThroughWindow(0x90000000u + kMem2 - 4u) == 0x12345678u);

    // MEM2 mirrors, including the last byte of the NDEV-sized 128 MiB.
    uint8_t* mem2End = GuestFlat::HostPointer(0x90000000u + kMem2 - 1u);
    CHECK(mem2End != nullptr);
    CHECK(GuestFlat::HostPointer(0xD0000000u + kMem2 - 1u) == mem2End);
    CHECK(GuestFlat::HostPointer(0x10000000u + kMem2 - 1u) == mem2End);

    // Past the end of a region: unmapped.
    CHECK(GuestFlat::HostPointer(0xC0000000u + kMem1) == nullptr);
    CHECK(GuestFlat::HostPointer(0x90000000u + kMem2) == nullptr);
    CHECK(GuestFlat::HostPointer(0xCC000000u) == nullptr);  // Hollywood registers: never backed

    // The Kamek overlay sits right after MEM1's 24 MiB and is its own store, not part of MEM1.
    uint8_t* kamek = GuestFlat::HostPointer(0x81800000u);
    uint8_t* mem1 = GuestFlat::HostPointer(0x80000000u);
    CHECK(kamek != nullptr);
    CHECK(kamek < mem1 || kamek >= mem1 + kMem1);
    *kamek = 0x11;
    CHECK(*mem1 != 0x11);

    // The overlay through the window is the same bytes as through HostPointer().
    CHECK(mkw_flat_guest_base[0x81800000u] == 0x11);

    // Locked cache: its own store, zero-filled.
    uint8_t* lc = GuestFlat::HostPointer(0xE0000000u);
    CHECK(lc != nullptr);
    CHECK(lc[0] == 0 && lc[0x100000] == 0);
    lc[0x40] = 0x77;
    CHECK(mkw_flat_guest_base[0xE0000040u] == 0x77);

    // Views are page aligned (the SVCs that map them need it).
    CHECK(reinterpret_cast<uintptr_t>(GuestFlat::HostPointer(0x80000000u)) % 0x1000 == 0);
    CHECK(reinterpret_cast<uintptr_t>(GuestFlat::HostPointer(0x90000000u)) % 0x1000 == 0);

    // A second Initialize with the same layout clears the RAM and keeps the stores.
    uint8_t* const window = mkw_flat_guest_base;
    GuestFlat::Initialize(kLayout);
    CHECK(GuestFlat::HostPointer(0x80001234u) == cached);
    CHECK(mkw_flat_guest_base == window);
    CHECK(*cached == 0);
    CHECK(LoadThroughWindow(0xC0002000u) == 0);
    CHECK(LoadThroughWindow(0x90000000u + kMem2 - 4u) == 0);

    // The exit hook unmaps everything; hbloader's next NRO in this process can map it all again.
    spm_switch_guest_memory_release();
    CHECK(!GuestFlat::IsActive());
    CHECK(mkw_flat_guest_base == nullptr);
    CHECK(GuestFlat::HostPointer(0x80001234u) == nullptr);
    GuestFlat::Initialize(kLayout);
    CHECK(GuestFlat::IsActive());
    CHECK(mkw_flat_guest_base != nullptr);
    StoreThroughWindow(0x80000100u, 0xCAFEF00Du);
    CHECK(LoadThroughWindow(0x00000100u) == 0xCAFEF00Du);
    spm_switch_guest_memory_release();

    std::printf("%s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures != 0;
}
