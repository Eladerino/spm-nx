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
//   - a crash handler: a data abort or other exception writes its registers to that same log.
#include <switch.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

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

bool g_sockets = false;

void MakeDirectory(const char* path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "[switch] cannot create %s (errno %d)\n", path, errno);
    }
}
} // namespace

extern "C" {

// The exception handler runs on its own stack: the faulting thread's may be the problem.
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void userAppInit(void) {
    MakeDirectory(kRoot);
    MakeDirectory(kUserData);
    MakeDirectory(kLogs);
    if (std::freopen(kStderrLog, "w", stderr) != nullptr) {
        std::setvbuf(stderr, nullptr, _IOLBF, 0);  // line-buffered: a crash keeps the last lines
    }
    setenv("WIICOMPILED_RUNTIME_DIR", kRoot, 1);
    setenv("WIICOMPILED_DATA_DIR", kUserData, 1);

    g_sockets = R_SUCCEEDED(socketInitializeDefault());

    AppletType type = appletGetAppletType();
    std::fprintf(stderr, "[switch] spm-nx starting: %s, %s mode, sockets %s\n",
                 type == AppletType_Application || type == AppletType_SystemApplication
                     ? "title takeover"
                     : "applet mode (little memory: launch through a game instead)",
                 appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
                 g_sockets ? "up" : "off");
}

void userAppExit(void) {
    std::fflush(stderr);
    if (g_sockets) {
        socketExit();
    }
}

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    std::fprintf(stderr, "\n[switch] CRASH: exception 0x%" PRIx32 " esr=0x%" PRIx32 "\n", ctx->error_desc,
                 ctx->esr);
    std::fprintf(stderr, "  pc=0x%016" PRIx64 " lr=0x%016" PRIx64 " sp=0x%016" PRIx64 " fp=0x%016" PRIx64 "\n",
                 ctx->pc.x, ctx->lr.x, ctx->sp.x, ctx->fp.x);
    std::fprintf(stderr, "  far=0x%016" PRIx64 " (fault address)\n", ctx->far.x);
    const uint64_t flatBase = GuestFlat::kFixedFlatGuestBase;
    if (ctx->far.x >= flatBase && ctx->far.x - flatBase < GuestFlat::kGuestSpaceSize) {
        std::fprintf(stderr,
                     "  -> guest address 0x%08" PRIx64 " through the flat view, which the Switch build does not map "
                     "yet: some code bypassed RequiresCheckedAccess()\n",
                     ctx->far.x - flatBase);
    }
    for (int i = 0; i < 29; i += 4) {
        std::fprintf(stderr, " ");
        for (int j = i; j < i + 4 && j < 29; ++j) {
            std::fprintf(stderr, " x%-2d=0x%016" PRIx64, j, ctx->cpu_gprs[j].x);
        }
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "  addr2line -e spm-nx.elf 0x<pc - load base> gives the source line\n");
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

} // extern "C"
