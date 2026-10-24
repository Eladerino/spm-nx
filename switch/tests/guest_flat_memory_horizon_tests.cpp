// Host-side test of switch/runtime/guest_flat_memory_horizon.cpp: it is plain C++ (heap stores, no
// libnx calls), so it runs on a PC. Uses the region layout runtime/src/memory.cpp builds for the game
// and checks that every mirror of MEM1 and MEM2 lands on the same bytes and owned regions stay apart.
//   c++ -std=c++20 -I../../wiicompiled/runtime/include guest_flat_memory_horizon_tests.cpp \
//       ../runtime/guest_flat_memory_horizon.cpp -o t && ./t
#include "guest_flat_memory.h"

#include <cstdio>
#include <cstring>

using GuestFlat::Backing;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    constexpr uint32_t kMem1 = 24u << 20;
    constexpr uint32_t kMem2 = 128u << 20;
    GuestFlat::Initialize({
        {0x00000000u, kMem1, Backing::Mem1},
        {0x80000000u, kMem1, Backing::Mem1},
        {0xC0000000u, kMem1, Backing::Mem1},
        {0x10000000u, kMem2, Backing::Mem2},
        {0x90000000u, kMem2, Backing::Mem2},
        {0xD0000000u, kMem2, Backing::Mem2},
        {0x81800000u, 0x200000u, Backing::Owned},          // MEM1_KAMEK_OVERLAY
        {0xE0000000u, 0x100000u + 4096u, Backing::Owned},  // LOCKED_CACHE
    });
    CHECK(GuestFlat::IsActive());

    // MEM1 mirrors share bytes, at the same offset in each view.
    uint8_t* cached = GuestFlat::HostPointer(0x80001234u);
    CHECK(cached != nullptr);
    CHECK(GuestFlat::HostPointer(0xC0001234u) == cached);
    CHECK(GuestFlat::HostPointer(0x00001234u) == cached);
    *cached = 0x5A;
    CHECK(*GuestFlat::HostPointer(0xC0001234u) == 0x5A);

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

    // Locked cache: its own store, zero-filled.
    uint8_t* lc = GuestFlat::HostPointer(0xE0000000u);
    CHECK(lc != nullptr);
    CHECK(lc[0] == 0 && lc[0x100000] == 0);

    // Stores are page aligned (svcSetMemoryPermission and phase 2's svcMapProcessMemory need it).
    CHECK(reinterpret_cast<uintptr_t>(GuestFlat::HostPointer(0x80000000u)) % 0x1000 == 0);
    CHECK(reinterpret_cast<uintptr_t>(GuestFlat::HostPointer(0x90000000u)) % 0x1000 == 0);

    // A second Initialize with the same layout clears the RAM and keeps the stores.
    GuestFlat::Initialize({
        {0x00000000u, kMem1, Backing::Mem1},
        {0x80000000u, kMem1, Backing::Mem1},
        {0xC0000000u, kMem1, Backing::Mem1},
        {0x10000000u, kMem2, Backing::Mem2},
        {0x90000000u, kMem2, Backing::Mem2},
        {0xD0000000u, kMem2, Backing::Mem2},
        {0x81800000u, 0x200000u, Backing::Owned},
        {0xE0000000u, 0x100000u + 4096u, Backing::Owned},
    });
    CHECK(GuestFlat::HostPointer(0x80001234u) == cached);
    CHECK(*cached == 0);

    std::printf("%s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures != 0;
}
