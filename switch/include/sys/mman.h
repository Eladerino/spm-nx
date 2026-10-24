// sys/mman.h for Horizon (Switch build only; switch/include is on the include path before newlib's).
//
// Horizon has no mmap. The runtime's only remaining user on the Switch is HostContext
// (runtime/src/host_context.cpp), which maps a fiber stack and protects its lowest page as a guard.
// So this covers exactly that: anonymous private mappings, backed by page-aligned heap memory, and
// mprotect on page-aligned heap ranges through svcSetMemoryPermission. A file-backed or shared
// mapping fails with ENODEV instead of pretending to work. Guest memory does not come through here;
// it has its own Horizon file (switch/runtime/guest_flat_memory_horizon.cpp).
#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000

#define MAP_FAILED ((void*)-1)

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t length, int prot);

#ifdef __cplusplus
}
#endif
