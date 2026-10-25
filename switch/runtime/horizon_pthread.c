// Adapted from SwitchWaker (MIT, switch/native/source/thread_wrap.c at 92307ff); see switch/THIRD_PARTY.md.
// Every pthread (and so every std::thread) of the NRO, which links with -Wl,--wrap=pthread_create
// (switch/cmake/SpmSwitch.cmake):
//
// - a stack of at least 4 MiB, page-aligned: libnx gives a pthread 128 KiB by default, which Tint
//   (Dawn's shader compiler) overflows. Boot 6 crashed that way on Aurora's pipeline compile worker
//   right after it was handed the 1156 seeded pipelines: a write one page below a thread's stack.
//   libnx also refuses a size that is not a multiple of 4 KiB.
// - the application cores: libnx creates pthreads on the process's default core with priority 0x3B,
//   below the main thread's 0x2C, so every helper would share core 0 with the guest scheduler and run
//   only when it waits (Horizon does not time-slice). Each new thread prefers core 1 or 2 in turn,
//   may run on any of the process's cores among 0-2, and gets the main thread's priority.
//
// SwitchWaker's version also reaps detached threads (libnx has no pthread_detach) and keeps a thread
// registry for its performance lines; neither is needed to boot yet.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <switch.h>

#define MINIMUM_STACK_SIZE (4u * 1024u * 1024u)
#define THREAD_PRIORITY 0x2C

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);

static atomic_int g_next_core = 1;

struct Trampoline {
    void* (*start)(void*);
    void* arg;
    int core;
};

static u32 application_core_mask(void)
{
    static u32 mask;
    if (mask == 0) {
        u64 process_mask = 0;
        if (R_FAILED(svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
            process_mask = 0x7;
        mask = (u32)process_mask & 0x7;
        if (mask == 0)
            mask = 0x1;
    }
    return mask;
}

static void* trampoline(void* raw)
{
    struct Trampoline t = *(struct Trampoline*)raw;
    free(raw);
    const u32 mask = application_core_mask();
    int core = t.core;
    if ((mask & (1u << core)) == 0)
        core = __builtin_ctz(mask);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask);
    svcSetThreadPriority(CUR_THREAD_HANDLE, THREAD_PRIORITY);
    return t.start(t.arg);
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg)
{
    pthread_attr_t sized;
    size_t requested = 0;
    if (attr != NULL) {
        sized = *attr;
        if (pthread_attr_getstacksize(attr, &requested) != 0)
            requested = 0;
    } else {
        pthread_attr_init(&sized);
    }
    size_t size = requested < MINIMUM_STACK_SIZE ? MINIMUM_STACK_SIZE : requested;
    size = (size + 0xFFF) & ~(size_t)0xFFF;
    pthread_attr_setstacksize(&sized, size);

    struct Trampoline* t = malloc(sizeof *t);
    if (t == NULL) {
        const int result = __real_pthread_create(thread, &sized, start, arg);
        if (attr == NULL)
            pthread_attr_destroy(&sized);
        return result;
    }
    t->start = start;
    t->arg = arg;
    t->core = 1 + (atomic_fetch_add(&g_next_core, 1) - 1) % 2; // 1, 2, 1, 2...
    const int result = __real_pthread_create(thread, &sized, trampoline, t);
    if (result != 0)
        free(t);
    if (attr == NULL)
        pthread_attr_destroy(&sized);
    return result;
}
