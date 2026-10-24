// The Horizon side of switch/include/sys/mman.h; see that header for the scope.
#include <sys/mman.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#define HORIZON_PAGE_SIZE 0x1000u

static size_t round_up_page(size_t length)
{
    return (length + HORIZON_PAGE_SIZE - 1u) & ~(size_t)(HORIZON_PAGE_SIZE - 1u);
}

static int is_page_aligned(const void* addr, size_t length)
{
    return ((uintptr_t)addr % HORIZON_PAGE_SIZE) == 0 && (length % HORIZON_PAGE_SIZE) == 0;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)offset;
    // Only what HostContext asks for: a fresh anonymous private block anywhere.
    if (addr != NULL || (flags & MAP_FIXED) || !(flags & MAP_ANONYMOUS) || (flags & MAP_SHARED) ||
        fd != -1 || length == 0) {
        errno = ENODEV;
        return MAP_FAILED;
    }
    const size_t size = round_up_page(length);
    void* block = aligned_alloc(HORIZON_PAGE_SIZE, size);
    if (block == NULL) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    // POSIX anonymous mappings are zero-filled; the heap is not.
    memset(block, 0, size);
    if (prot != (PROT_READ | PROT_WRITE) && mprotect(block, size, prot) != 0) {
        free(block);
        return MAP_FAILED;
    }
    return block;
}

int munmap(void* addr, size_t length)
{
    if (addr == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }
    // Give the allocator back ordinary read/write pages: a guard page left at PROT_NONE would fault
    // the next owner of that memory.
    mprotect(addr, round_up_page(length), PROT_READ | PROT_WRITE);
    free(addr);
    return 0;
}

int mprotect(void* addr, size_t length, int prot)
{
    if (!is_page_aligned(addr, length)) {
        errno = EINVAL;
        return -1;
    }
    // svcSetMemoryPermission takes none, R or RW on heap pages; executable heap is not a thing.
    u32 perm;
    if (prot & PROT_EXEC) {
        errno = ENOTSUP;
        return -1;
    } else if (prot & PROT_WRITE) {
        perm = Perm_Rw;
    } else if (prot & PROT_READ) {
        perm = Perm_R;
    } else {
        perm = Perm_None;
    }
    const Result rc = svcSetMemoryPermission(addr, length, perm);
    if (R_FAILED(rc)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

// newlib declares getpagesize() but nothing in devkitA64's libc or libnx defines it; HostContext
// sizes its guard page with it. Horizon's page size is fixed at 4 KiB.
int getpagesize(void)
{
    return (int)HORIZON_PAGE_SIZE;
}
