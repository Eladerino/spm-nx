// Native thread-local storage for the clang-compiled code (switch/cmake/clang-launcher.sh).
//
// libnx gives every thread an ELF TLS block, and devkitA64's GCC reaches it through a soft thread
// pointer: a call to libnx's __aarch64_read_tp (mrs tpidrro_el0, then a load of the thread's
// tls_tp). clang has no soft thread pointer for AArch64, so the clang half of the NRO (the runtime,
// Aurora and the translated game) used to compile with -femulated-tls instead, and every
// thread_local access became a call to libgcc's __emutls_get_address, then pthread_getspecific,
// then libnx's threadTlsGet: about 35 instructions and three calls, and it turned leaf functions
// into ones with a stack frame. The runtime reads thread_locals on hot paths it tuned for the
// one-instruction TLS of the desktop: every guest-to-guest call (g_currentCpuContext), every
// single-precision float operation (g_mkwNiFlushThreshold) and every indirect call (the dispatch
// memos and g_currentTranslatedExecutionAddress).
//
// clang now reads the thread pointer from TPIDR_EL0 (-mtp=el0), which Horizon keeps per thread
// (it is in every thread's saved context, ThreadContext::tpidr) and libnx leaves at zero. This file
// makes TPIDR_EL0 hold the same value __aarch64_read_tp returns, so both compilers find each
// thread_local at the same address (the linker's TP-relative offsets are the same for both):
// - the main thread: spm_switch_tls_init_main_thread(), the first thing userAppInit() does, which
//   libnx calls before the static constructors and main() (spm_switch_tls_check_main_thread() checks
//   it once stderr goes to the log);
// - every other thread: the NRO links with -Wl,--wrap=threadCreate (switch/cmake/SpmSwitch.cmake),
//   and the wrapper starts the thread in a trampoline that sets TPIDR_EL0 before the thread's own
//   entry runs. pthread_create (and so std::thread) creates its threads through threadCreate, and
//   so does everything else in the NRO.
//
// A probe with an initial value checks the arrangement on every thread the wrapper starts and on
// the main thread: a wrong thread pointer reads some other memory than the probe's initial value,
// and the process stops with a message in the log instead of running on with corrupt thread
// locals.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>

// libnx: the soft thread pointer GCC's code calls (readtp.o).
void* __aarch64_read_tp(void);

Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz, int prio,
                           int cpuid);
Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz, int prio,
                           int cpuid);
void spm_switch_tls_init_main_thread(void);
void spm_switch_tls_check_main_thread(void);

#define TLS_PROBE_VALUE 0x5EED7150u
static _Thread_local volatile uint32_t g_tls_probe = TLS_PROBE_VALUE;

static inline void set_thread_pointer(void)
{
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(__aarch64_read_tp()) : "memory");
}

static void check_probe(const char* where)
{
    if (g_tls_probe == TLS_PROBE_VALUE)
        return;
    fprintf(stderr,
            "[switch] FATAL: thread-local storage is misplaced on %s (probe read 0x%08x, expected 0x%08x, "
            "tpidr_el0 %p, libnx thread pointer %p)\n",
            where, (unsigned)g_tls_probe, TLS_PROBE_VALUE, __builtin_thread_pointer(), __aarch64_read_tp());
    fflush(stderr);
    abort();
}

void spm_switch_tls_init_main_thread(void)
{
    set_thread_pointer();
}

void spm_switch_tls_check_main_thread(void)
{
    check_probe("the main thread");
}

struct TlsTrampoline {
    ThreadFunc entry;
    void* arg;
};

static void tls_trampoline(void* raw)
{
    set_thread_pointer();
    check_probe("a new thread");
    const struct TlsTrampoline t = *(const struct TlsTrampoline*)raw;
    free(raw);
    t.entry(t.arg);
}

Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz, int prio,
                           int cpuid)
{
    struct TlsTrampoline* tramp = malloc(sizeof *tramp);
    if (tramp == NULL)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    tramp->entry = entry;
    tramp->arg = arg;
    const Result rc = __real_threadCreate(t, tls_trampoline, tramp, stack_mem, stack_sz, prio, cpuid);
    if (R_FAILED(rc))
        free(tramp);
    return rc;
}

// Nothing in the NRO may still use emulated TLS: an object compiled with -femulated-tls keeps its own
// copy of each thread_local it touches, apart from the native one the rest of the program uses (a
// stale object from a build before the switch to native TLS, say). This definition takes the place
// of libgcc's, so such an object stops the game at its first thread_local access with this message
// instead of running with two copies of the guest CPU context pointer.
void* __emutls_get_address(void* control)
{
    fprintf(stderr,
            "[switch] FATAL: an object in this build still uses emulated thread-local storage (control %p): "
            "rebuild from a clean build folder\n",
            control);
    fflush(stderr);
    abort();
}
