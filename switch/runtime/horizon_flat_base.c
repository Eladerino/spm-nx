// The flat guest window's base, MKW_FLAT_GUEST_BASE on Switch (wiicompiled/runtime/include/
// guest_flat_memory.h). Every C++ file sees it as `uint8_t* const`, so translated code can keep it
// in a register for a whole function; this C file is the only place it is defined, and written,
// non-const. guest_flat_memory_horizon.cpp sets it once, from Memory::Init, before any guest code
// runs, and clears it again when the window is unmapped at exit.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {  // in case a PC test compiles this file as C++
#endif

uint8_t* mkw_flat_guest_base;

void spm_switch_set_flat_guest_base(uint8_t* base) {
    mkw_flat_guest_base = base;
}

#ifdef __cplusplus
}
#endif
