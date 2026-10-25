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
// - detached threads: libnx has no __syscall_thread_detach, so newlib's pthread_detach fails with
//   ENOSYS, std::thread::detach() throws, and the temporary std::thread's destructor, still joinable,
//   calls std::terminate(). Boot 8: the runtime's stall watchdog (hle/vi.cpp StartStallWatchdogOnce,
//   std::thread(...).detach()) ended the game at its first frame that way. pthread_detach hands
//   the thread to a reaper thread, which joins it once its handle signals that it ended (its stack
//   and kernel thread are freed then).
//
// SwitchWaker's version also keeps a thread registry for its performance lines; not needed yet.
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>

#define MINIMUM_STACK_SIZE (4u * 1024u * 1024u)
#define THREAD_PRIORITY 0x2C

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
int __wrap_pthread_detach(pthread_t thread);

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

// ---- reaper of detached threads -------------------------------------------------------------------
// newlib's pthread_t is libnx's struct __pthread_t*, whose first member is the libnx Thread.
#define MAX_REAP 64 // svcWaitSynchronization's limit

static Mutex g_reap_lock;
static pthread_t g_reap[MAX_REAP];
static int g_reap_count;
static atomic_int g_reaper_started;

static Handle thread_handle(pthread_t t)
{
    return ((const Thread*)t)->handle;
}

static void reap_remove(pthread_t t)
{
    mutexLock(&g_reap_lock);
    for (int i = 0; i < g_reap_count; i++) {
        if (g_reap[i] == t) {
            g_reap[i] = g_reap[--g_reap_count];
            break;
        }
    }
    mutexUnlock(&g_reap_lock);
}

static void* reaper(void* unused)
{
    (void)unused;
    for (;;) {
        Handle handles[MAX_REAP];
        pthread_t threads[MAX_REAP];
        mutexLock(&g_reap_lock);
        const int n = g_reap_count;
        for (int i = 0; i < n; i++) {
            threads[i] = g_reap[i];
            handles[i] = thread_handle(g_reap[i]);
        }
        mutexUnlock(&g_reap_lock);
        if (n == 0) {
            svcSleepThread(100000000ull);
            continue;
        }
        s32 index = -1;
        // 100 ms: threads detached meanwhile are picked up at the next round
        const Result rc = svcWaitSynchronization(&index, handles, n, 100000000ull);
        if (R_VALUE(rc) == KERNELRESULT(InvalidHandle)) {
            // not a thread any more (should not happen): drop the ones that are not, keep the others
            for (int i = 0; i < n; i++) {
                s32 one = -1;
                if (R_VALUE(svcWaitSynchronization(&one, &handles[i], 1, 0)) == KERNELRESULT(InvalidHandle))
                    reap_remove(threads[i]);
            }
            continue;
        }
        if (R_FAILED(rc) || index < 0 || index >= n)
            continue;
        reap_remove(threads[index]);
        pthread_join(threads[index], NULL); // it has ended: frees its stack and closes its handle
    }
    return NULL;
}

int __wrap_pthread_detach(pthread_t thread)
{
    if (atomic_exchange(&g_reaper_started, 1) == 0) {
        pthread_t r;
        if (__wrap_pthread_create(&r, NULL, reaper, NULL) != 0)
            fprintf(stderr, "[switch] the reaper of detached threads could not start\n");
    }
    mutexLock(&g_reap_lock);
    bool listed = false;
    for (int i = 0; i < g_reap_count; i++)
        listed = listed || g_reap[i] == thread;
    const bool full = !listed && g_reap_count >= MAX_REAP;
    if (!listed && !full)
        g_reap[g_reap_count++] = thread;
    mutexUnlock(&g_reap_lock);
    if (full) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[switch] %d detached threads already wait to be joined: more are left unjoined\n",
                    MAX_REAP);
    }
    // Detached either way: the caller (std::thread::detach) must not see an error.
    return 0;
}
