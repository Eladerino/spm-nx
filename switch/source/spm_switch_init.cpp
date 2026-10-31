// Process setup for the Switch NRO, outside the upstream runtime. libnx calls userAppInit() before
// main() and userAppExit() after it returns, so the runtime's own main (runtime/src/main.cpp) runs
// unchanged on top of what this file prepares:
//   - the SD card layout: sdmc:/switch/spm-nx/ holds the NRO's companion files (DSP ROM, pipeline
//     cache, Wii bootstrap NAND, CA bundle: the files the desktop build keeps beside its .exe) and
//     UserData/ (settings, saves, caches, logs). The runtime finds both through the same environment
//     variables its Android build uses (runtime/src/platform/host_platform.cpp);
//   - stderr into UserData/Logs/stderr.log, because RT_LOG writes there and a log file is how a run
//     on the console reports back;
//   - BSD sockets (netplay, nxlink) when the network is up;
//   - a crash handler: a data abort or other exception writes its registers, the NRO's load address
//     and a backtrace to UserData/Logs/crash.txt.
#include <switch.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>

#include "guest_flat_memory.h"

namespace {
// Without the "sdmc:" prefix: fsdevMountSdmc makes the SD card the default device, so "/switch/..."
// is the same place, and it is an absolute path to std::filesystem and sqlite. With the prefix both
// treat the path as relative: std::filesystem::create_directories tries to make "sdmc:" and fails
// (the NAND folders), and sqlite's unix VFS looks for "/sdmc:" (Aurora's caches).
constexpr const char* kRoot = "/switch/spm-nx";
constexpr const char* kUserData = "/switch/spm-nx/UserData";
constexpr const char* kLogs = "/switch/spm-nx/UserData/Logs";
constexpr const char* kStderrLog = "/switch/spm-nx/UserData/Logs/stderr.log";

// The crash report goes to its own file through write(): through stderr it came out interleaved with,
// and overwritten by, the other threads' log lines (boot 6).
constexpr const char* kCrashLog = "/switch/spm-nx/UserData/Logs/crash.txt";

bool g_sockets = false;
u64 g_mainThreadId = 0;
std::atomic<bool> g_crashing{false};
std::atomic<u64> g_exitingThread{0};  // the thread in userAppExit(), which unmaps guest memory
int g_crashFd = -1;
char g_crashLine[512];

void Say(const char* format, ...) {
    if (g_crashFd < 0) {
        return;
    }
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(g_crashLine, sizeof(g_crashLine), format, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    if (n >= static_cast<int>(sizeof(g_crashLine))) {
        n = sizeof(g_crashLine) - 1;
    }
    write(g_crashFd, g_crashLine, static_cast<size_t>(n));
}

const char* ExceptionName(u32 desc) {
    switch (desc) {
    case ThreadExceptionDesc_InstructionAbort: return "instruction abort";
    case ThreadExceptionDesc_MisalignedPC: return "misaligned PC";
    case ThreadExceptionDesc_MisalignedSP: return "misaligned SP";
    case ThreadExceptionDesc_SError: return "SError";
    case ThreadExceptionDesc_BadSVC: return "bad SVC";
    case ThreadExceptionDesc_Trap: return "trap";
    case ThreadExceptionDesc_Other: return "data abort or other";
    default: return "unknown";
    }
}

// The NRO's load address: the start of the code mapping this function is in.
uintptr_t ImageBase() {
    static uintptr_t base;
    if (base == 0) {
        MemoryInfo info{};
        u32 page = 0;
        if (R_SUCCEEDED(svcQueryMemory(&info, &page, reinterpret_cast<uintptr_t>(&ImageBase))) &&
            info.type != MemType_Unmapped) {
            base = info.addr;
        }
    }
    return base;
}

// An address in the NRO also as an offset for addr2line.
void SayAddress(const char* label, uint64_t address) {
    const uintptr_t base = ImageBase();
    if (base != 0 && address >= base && address - base < (1ull << 30)) {
        Say("%s0x%016" PRIx64 " (spm-nx.elf+0x%" PRIx64 ")", label, address, address - base);
    } else {
        Say("%s0x%016" PRIx64, label, address);
    }
}

bool ReadWord(uintptr_t address, uintptr_t* out) {
    if (address == 0 || (address & 7) != 0) {
        return false;
    }
    MemoryInfo info{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&info, &page, address)) || info.type == MemType_Unmapped ||
        (info.perm & Perm_R) == 0 || address + 8 > info.addr + info.size) {
        return false;
    }
    *out = *reinterpret_cast<const uintptr_t*>(address);
    return true;
}

// The stack the frame pointer is in, and whether sp has left it: a stack overflow faults just below.
void SayStack(uint64_t sp, uint64_t fp) {
    MemoryInfo info{};
    u32 page = 0;
    if (fp == 0 || R_FAILED(svcQueryMemory(&info, &page, fp)) || info.type == MemType_Unmapped) {
        return;
    }
    Say("  stack 0x%016" PRIx64 "-0x%016" PRIx64 " (%" PRIu64 " KiB, memory type 0x%" PRIx32 ")", info.addr,
        info.addr + info.size, info.size / 1024, info.type);
    if (sp < info.addr) {
        Say(": sp is 0x%" PRIx64 " bytes below it, a stack overflow", info.addr - sp);
    }
    Say("\n");
}

void MakeDirectory(const char* path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "[switch] cannot create %s (errno %d)\n", path, errno);
    }
}
} // namespace

extern "C" {

// runtime/horizon_tls.c: the thread pointer clang's thread_locals use (TPIDR_EL0) for this thread.
void spm_switch_tls_init_main_thread(void);
void spm_switch_tls_check_main_thread(void);
// runtime/guest_flat_memory_horizon.cpp: unmaps the flat guest window, which hbloader needs gone.
void spm_switch_guest_memory_release(void);

// The exception handler runs on its own stack: the faulting thread's may be the problem.
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void userAppInit(void) {
    // Before anything that could touch a thread_local: libnx runs the static constructors after this.
    spm_switch_tls_init_main_thread();
    MakeDirectory(kRoot);
    MakeDirectory(kUserData);
    MakeDirectory(kLogs);
    if (std::freopen(kStderrLog, "w", stderr) != nullptr) {
        std::setvbuf(stderr, nullptr, _IOLBF, 0);  // line-buffered: a crash keeps the last lines
    }
    spm_switch_tls_check_main_thread();
    setenv("WIICOMPILED_RUNTIME_DIR", kRoot, 1);
    setenv("WIICOMPILED_DATA_DIR", kUserData, 1);

    g_sockets = R_SUCCEEDED(socketInitializeDefault());
    svcGetThreadId(&g_mainThreadId, CUR_THREAD_HANDLE);
    ImageBase();

    AppletType type = appletGetAppletType();
    std::fprintf(stderr, "[switch] spm-nx starting: %s, %s mode, sockets %s\n",
                 type == AppletType_Application || type == AppletType_SystemApplication
                     ? "title takeover"
                     : "applet mode (little memory: launch through a game instead)",
                 appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
                 g_sockets ? "up" : "off");
}

void userAppExit(void) {
    // After main() and the static destructors. A thread still running (the fatal exit leaves
    // Aurora's alive) may touch guest memory once it is unmapped; the crash handler parks it.
    u64 self = 0;
    svcGetThreadId(&self, CUR_THREAD_HANDLE);
    g_exitingThread = self;
    spm_switch_guest_memory_release();
    std::fflush(stderr);
    if (g_sockets) {
        socketExit();
    }
}

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    u64 threadId = 0;
    svcGetThreadId(&threadId, CUR_THREAD_HANDLE);
    // During exit, another thread's fault is one that outlived the unmapped guest memory, not a
    // crash worth a report over the one that may have caused the exit: it waits, like a second
    // crash does. The exiting thread's own fault is still reported.
    const u64 exiting = g_exitingThread;
    bool expected = false;
    if ((exiting != 0 && exiting != threadId) || !g_crashing.compare_exchange_strong(expected, true)) {
        for (;;) {  // a second thread's exception waits for the first report to end the process
            svcSleepThread(1000000000ULL);
        }
    }
    g_crashFd = open(kCrashLog, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    Say("[switch] CRASH: %s (0x%" PRIx32 ") esr=0x%" PRIx32 " far=0x%016" PRIx64 " on thread %" PRIu64 "%s\n",
        ExceptionName(ctx->error_desc), ctx->error_desc, ctx->esr, ctx->far.x, threadId,
        threadId == g_mainThreadId ? " (the main thread: guest scheduler)" : " (not the main thread)");
    const uint64_t flatBase = reinterpret_cast<uint64_t>(MKW_FLAT_GUEST_BASE);
    if (flatBase != 0 && ctx->far.x >= flatBase && ctx->far.x - flatBase < GuestFlat::kGuestSpaceSize) {
        Say("  -> guest address 0x%08" PRIx64 " through the flat window, where nothing is mapped: a wild "
            "guest pointer or an MMIO read (the window has no page traps)\n",
            ctx->far.x - flatBase);
    }
    SayAddress("  pc=", ctx->pc.x);
    SayAddress(" lr=", ctx->lr.x);
    Say(" sp=0x%016" PRIx64 " fp=0x%016" PRIx64 "\n", ctx->sp.x, ctx->fp.x);
    SayStack(ctx->sp.x, ctx->fp.x);
    for (int i = 0; i < 29; i += 4) {
        Say(" ");
        for (int j = i; j < i + 4 && j < 29; ++j) {
            Say(" x%-2d=0x%016" PRIx64, j, ctx->cpu_gprs[j].x);
        }
        Say("\n");
    }
    Say("  image base=0x%016" PRIxPTR ": aarch64-none-elf-addr2line -f -C -i -e spm-nx.elf <offset>\n",
        ImageBase());
    Say("  backtrace (pc, lr, then the frame records' call sites):\n");
    SayAddress("    #0 ", ctx->pc.x);
    Say("\n");
    SayAddress("    #1 ", ctx->lr.x);
    Say("\n");
    uintptr_t fp = ctx->fp.x;
    for (int depth = 2; fp != 0 && depth < 48; ++depth) {
        uintptr_t next = 0;
        uintptr_t ret = 0;
        if (!ReadWord(fp, &next) || !ReadWord(fp + 8, &ret) || ret == 0) {
            break;
        }
        char label[16];
        std::snprintf(label, sizeof(label), "    #%d ", depth);
        SayAddress(label, ret - 4);
        Say("\n");
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    if (g_crashFd >= 0) {
        close(g_crashFd);
    }
    std::_Exit(EXIT_FAILURE);
}

// The host stack for the runtime's fatal paths (runtime/src/main.cpp FormatHostStackTrace), which
// has no walker on this platform: a std::terminate() reported only the guest's state (boot 7). The
// unwind tables first (C++ code has them), the frame records if they give out early. Offsets are
// for aarch64-none-elf-addr2line -f -C -i -e spm-nx.elf.
namespace {
struct UnwindFrames {
    uintptr_t pcs[48];
    unsigned count = 0;
    unsigned skip = 0;
};

_Unwind_Reason_Code CollectFrame(_Unwind_Context* context, void* argument) {
    auto* frames = static_cast<UnwindFrames*>(argument);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc == 0) {
        return _URC_END_OF_STACK;
    }
    if (frames->skip > 0) {
        --frames->skip;
        return _URC_NO_REASON;
    }
    frames->pcs[frames->count++] = pc;
    return frames->count < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
} // namespace

size_t spm_switch_host_backtrace(char* out, size_t size, unsigned skip) {
    if (out == nullptr || size == 0) {
        return 0;
    }
    UnwindFrames frames;
    frames.skip = skip + 1;  // this function
    _Unwind_Backtrace(CollectFrame, &frames);
    const char* method = "unwind tables";
    if (frames.count < 3) {
        frames.count = 0;
        method = "frame records";
        uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        while (fp != 0 && frames.count < 48) {
            uintptr_t next = 0;
            uintptr_t ret = 0;
            if (!ReadWord(fp, &next) || !ReadWord(fp + 8, &ret) || ret == 0) {
                break;
            }
            frames.pcs[frames.count++] = ret;
            if (next <= fp) {
                break;
            }
            fp = next;
        }
    }

    u64 threadId = 0;
    svcGetThreadId(&threadId, CUR_THREAD_HANDLE);
    const uintptr_t base = ImageBase();
    size_t used = 0;
    auto append = [&](const char* format, auto... args) {
        if (used + 1 >= size) {
            return;
        }
        const int n = std::snprintf(out + used, size - used, format, args...);
        if (n > 0) {
            used += static_cast<size_t>(n) < size - used ? static_cast<size_t>(n) : size - used - 1;
        }
    };
    append("[switch] host stack on thread %" PRIu64 "%s (%s; image base 0x%" PRIxPTR
           ", aarch64-none-elf-addr2line -f -C -i -e spm-nx.elf <offset>):\n",
           threadId, threadId == g_mainThreadId ? " (main)" : "", method, base);
    for (unsigned i = 0; i < frames.count; ++i) {
        // Return addresses: the call is the instruction before each.
        const uintptr_t pc = frames.pcs[i] - 4;
        if (base != 0 && pc >= base && pc - base < (1ull << 30)) {
            append("[switch]   #%u spm-nx.elf+0x%" PRIxPTR "\n", i, pc - base);
        } else {
            append("[switch]   #%u 0x%016" PRIxPTR "\n", i, pc);
        }
    }
    return used;
}

} // extern "C"
