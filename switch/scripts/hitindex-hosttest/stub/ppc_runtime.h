#pragma once
// Host-test stub for the runtime's ppc_runtime.h: only the part of
// CpuContext the hit_index helper touches (gpr[13], the SDA base).
#include <cstdint>

struct CpuContext {
    uint32_t gpr[32] = {};
};
