#pragma once
// Host-test stub for the runtime's memory_access.h: a flat guest view over
// a synthetic byte array, big-endian like the guest. Guest addresses are
// masked into the array; the harness keeps its regions collision-free.
#include <cstdint>

namespace TestMem {
inline constexpr uint32_t kMemMask = 0x3FFFFFF; // 64 MB window
inline uint8_t g_mem[1u << 26];
} // namespace TestMem

namespace MemoryInline {

inline uint8_t FlatRead8(uint32_t address) {
    return TestMem::g_mem[address & TestMem::kMemMask];
}
inline uint16_t FlatRead16(uint32_t address) {
    return static_cast<uint16_t>((FlatRead8(address) << 8) | FlatRead8(address + 1));
}
inline uint32_t FlatRead32(uint32_t address) {
    return (static_cast<uint32_t>(FlatRead16(address)) << 16) | FlatRead16(address + 2);
}
inline void FlatWrite8(uint32_t address, uint8_t value) {
    TestMem::g_mem[address & TestMem::kMemMask] = value;
}
inline void FlatWrite16(uint32_t address, uint16_t value) {
    FlatWrite8(address, static_cast<uint8_t>(value >> 8));
    FlatWrite8(address + 1, static_cast<uint8_t>(value));
}
inline void FlatWrite32(uint32_t address, uint32_t value) {
    FlatWrite16(address, static_cast<uint16_t>(value >> 16));
    FlatWrite16(address + 2, static_cast<uint16_t>(value));
}

} // namespace MemoryInline
