// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Adapted from SwitchWaker (https://github.com/centollOS/SwitchWaker) at 92307ff: switch/deko/device.cpp
// (device, queue, swapchain, debug callback, queue checks, set-up cost), itself adapted from
// SwitchWakerHD's runtime/src/gfx/deko/backend.cpp.
// The deko3d device and the window (dk.h, dk_aurora.h): created from Aurora's webgpu::initialize right
// after Dawn's device, before Aurora creates any texture or buffer, so that every one of them gets
// its deko3d shadow (wgpu_objects.cpp). The swapchain is three RGBA8 images of 1280x720 on the
// default window; the present (wgpu_encode.cpp) scales Aurora's finished picture into one of them.
#include <switch.h>
#include <unistd.h>

#include "dk.h"
#include "dk_aurora.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" char* fake_heap_end;

namespace swdk {

Renderer R;
Diag g_diag;

// The diagnostic switches (dk.h): one "key=1" per line in UserData/dk_diag.txt, read once here.
// A line naming a key without "=0" turns it on. The active set is logged so a test log always
// says which switches the run used.
void diag_init() {
    FILE* f = std::fopen("/switch/spm-nx/UserData/dk_diag.txt", "r");
    if (!f) {
        dklog("diag: no UserData/dk_diag.txt; all diagnostic switches off");
        return;
    }
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        const bool off = std::strstr(line, "=0") != nullptr;
        if (std::strstr(line, "depth_off")) g_diag.depthOff = !off;
        if (std::strstr(line, "cull_off")) g_diag.cullOff = !off;
        if (std::strstr(line, "magenta")) g_diag.magenta = !off;
        if (std::strstr(line, "picdump")) g_diag.picDump = !off;
        if (std::strstr(line, "efbclear")) g_diag.efbClear = !off;
        if (std::strstr(line, "bufdump")) g_diag.bufDump = !off;
        if (std::strstr(line, "texdump")) g_diag.texDump = !off;
        if (std::strstr(line, "shaderdump")) g_diag.shaderDump = !off;
        if (std::strstr(line, "nomerge")) g_diag.noMerge = !off;
        if (std::strstr(line, "uvlog")) g_diag.uvLog = !off;
        if (std::strstr(line, "mtxlog")) g_diag.mtxLog = !off;
        if (std::strstr(line, "perftimer")) g_diag.perfTimer = !off;
        if (std::strstr(line, "arrayup")) g_diag.arrayUp = !off;
    }
    std::fclose(f);
    // Aurora's switches initialise from these env vars on their first use; diag_init runs from
    // webgpu::initialize, before any texture, buffer or draw exists, so setting them here lands
    // in time. In-process libc env: Aurora's getenv sees what this setenv wrote. After init the
    // values live in Aurora's own flags and diag_set below is what changes them.
    if (g_diag.noMerge) setenv("NSMBW_NO_MERGE", "1", 1);
    // Same handshake for NSMBW_UV_LOG: build_uniform initialises its flag from it on the first draw it stages.
    if (g_diag.uvLog) setenv("NSMBW_UV_LOG", "1", 1);
    // Same handshake for NSMBW_MTX_LOG.
    if (g_diag.mtxLog) setenv("NSMBW_MTX_LOG", "1", 1);
    // Same handshake for NSMBW_STAGE_TIMERS.
    if (g_diag.perfTimer) setenv("NSMBW_STAGE_TIMERS", "1", 1);
    // Same handshake for NSMBW_ARRAY_REUPLOAD: handle_draw initialises its flag from it on the first draw it handles.
    if (g_diag.arrayUp) setenv("NSMBW_ARRAY_REUPLOAD", "1", 1);
    dklog("diag: depth_off=%d cull_off=%d magenta=%d picdump=%d efbclear=%d bufdump=%d texdump=%d shaderdump=%d nomerge=%d uvlog=%d mtxlog=%d perftimer=%d arrayup=%d",
          g_diag.depthOff ? 1 : 0, g_diag.cullOff ? 1 : 0, g_diag.magenta ? 1 : 0, g_diag.picDump ? 1 : 0,
          g_diag.efbClear ? 1 : 0, g_diag.bufDump ? 1 : 0, g_diag.texDump ? 1 : 0, g_diag.shaderDump ? 1 : 0,
          g_diag.noMerge ? 1 : 0, g_diag.uvLog ? 1 : 0, g_diag.mtxLog ? 1 : 0, g_diag.perfTimer ? 1 : 0,
          g_diag.arrayUp ? 1 : 0);
}

// diag_set (dk.h): the runtime path for the same switches. The eight interposer switches are
// plain field flips (their use sites read g_diag per bind, per present, per compile); the five
// Aurora switches also forward to Aurora's flags, which initialise from the env vars above and
// are settable ever since. A flip lands from the switch's next use onward, not at a frame
// boundary.
bool diag_set(const char* key, bool on) {
    if (!key) return false;
    if (!std::strcmp(key, "depth_off")) g_diag.depthOff = on;
    else if (!std::strcmp(key, "cull_off")) g_diag.cullOff = on;
    else if (!std::strcmp(key, "magenta")) g_diag.magenta = on;
    else if (!std::strcmp(key, "picdump")) g_diag.picDump = on;
    else if (!std::strcmp(key, "efbclear")) g_diag.efbClear = on;
    else if (!std::strcmp(key, "bufdump")) g_diag.bufDump = on;
    else if (!std::strcmp(key, "texdump")) g_diag.texDump = on;
    else if (!std::strcmp(key, "shaderdump")) g_diag.shaderDump = on;
    else if (!std::strcmp(key, "nomerge")) {
        g_diag.noMerge = on;
        aurora::gx::fifo::nsmbw_set_no_merge(on ? 1 : 0);
    } else if (!std::strcmp(key, "uvlog")) {
        g_diag.uvLog = on;
        aurora::gx::nsmbw_set_uv_log(on);
    } else if (!std::strcmp(key, "mtxlog")) {
        g_diag.mtxLog = on;
        aurora::gx::nsmbw_set_mtx_log(on);
    } else if (!std::strcmp(key, "perftimer")) {
        g_diag.perfTimer = on;
        aurora::gfx::stage_timers_set_enabled(on);
    } else if (!std::strcmp(key, "arrayup")) {
        g_diag.arrayUp = on;
        aurora::gx::fifo::nsmbw_set_array_reupload(on);
    } else {
        return false;
    }
    return true;
}

// diag_flush_all (dk.h): everything that can be holding diagnostic output lets go of it, in
// dependency order. Aurora's mtxlog buffer first (its lines are the only ones buffered above
// stderr), then a partial perftimer window while perftimer is on, so the segment just played
// gets its own numbers, then the stderr stream itself, which every writer in the process
// shares. The marker goes last, and is flushed too.
void diag_flush_all(const char* reason) {
    aurora::gx::nsmbw_mtxlog_flush();
    if (g_diag.perfTimer) aurora::gfx::stage_time_print_now();
    log_flush();
    std::fprintf(stderr, "[diag] logs flushed (%s) at present %llu\n", reason ? reason : "",
                 (unsigned long long)R.presents);
    log_flush();
}

// The command channel (dk.h): UserData/dk_cmd.txt, one command per write, executed by the
// present path once per present and truncated whether or not it parsed, so a command can
// never run twice. Commands:
//   flush           diag_flush_all, now
//   mark <text>     a "[diag] mark" line bracketing what follows it in the log, then a flush
//   set <key> <0|1> diag_set for any dk_diag.txt switch; the flip is logged with its present
//                   count, so a pulled log shows which state each segment ran under
// Anything else, an unknown key, or a malformed set logs a [diag] error line and changes
// nothing. The channel exists so a test run can be driven, and its log flushed, without
// rebooting to edit dk_diag.txt or waiting out a buffer.
void diag_cmd_poll() {
    FILE* f = std::fopen("/switch/spm-nx/UserData/dk_cmd.txt", "r");
    if (!f) return;
    char cmd[256];
    const bool got = std::fgets(cmd, sizeof(cmd), f) != nullptr;
    std::fclose(f);
    // Truncate before executing: a command that logs heavily must not meet itself again at
    // the next present.
    if (FILE* t = std::fopen("/switch/spm-nx/UserData/dk_cmd.txt", "w")) std::fclose(t);
    if (!got) return;
    // One line: drop the newline and any trailing whitespace.
    size_t len = std::strlen(cmd);
    while (len > 0 && (cmd[len - 1] == '\n' || cmd[len - 1] == '\r' || cmd[len - 1] == ' ')) cmd[--len] = '\0';
    if (len == 0) return;
    const unsigned long long present = (unsigned long long)R.presents;
    if (!std::strcmp(cmd, "flush")) {
        diag_flush_all("dk_cmd");
    } else if (!std::strncmp(cmd, "mark", 4) && (cmd[4] == '\0' || cmd[4] == ' ')) {
        std::fprintf(stderr, "[diag] mark \"%s\" at present %llu\n", cmd[4] == ' ' ? cmd + 5 : "", present);
        diag_flush_all("mark");
    } else if (!std::strncmp(cmd, "set ", 4)) {
        char key[64];
        char val[8];
        if (std::sscanf(cmd + 4, "%63s %7s", key, val) == 2 && (val[0] == '0' || val[0] == '1') &&
            val[1] == '\0') {
            if (diag_set(key, val[0] == '1')) {
                std::fprintf(stderr, "[diag] set %s=%s at present %llu\n", key, val, present);
            } else {
                std::fprintf(stderr, "[diag] error: unknown diag switch \"%s\"\n", key);
            }
        } else {
            std::fprintf(stderr, "[diag] error: malformed dk_cmd command \"%s\" (want: set <key> <0|1>)\n", cmd);
        }
        log_flush();
    } else {
        std::fprintf(stderr, "[diag] error: unknown dk_cmd command \"%s\"\n", cmd);
        log_flush();
    }
}

namespace {

constexpr uint32_t kSwapImages = 3;
constexpr uint32_t kQueueCommandMemory = 1u << 20;

DkSwapchain g_swapchain = nullptr;
DkImage g_swapImages[kSwapImages];
ImageAlloc g_swapMem[kSwapImages];
int g_debugMessages = 0;

// the heap malloc has never grown into, in MiB (libnx's heap end against malloc's break): deko3d's
// memory blocks come from it, so the difference across the set-up is its cost
size_t heap_never_used_mib() {
    char* top = static_cast<char*>(sbrk(0));
    if (!fake_heap_end || !top || top == reinterpret_cast<char*>(-1)) return 0;
    return size_t(fake_heap_end - top) >> 20;
}

// deko3d's messages: only the debug library calls this (the release library aborts with 2359-xxxx)
void debug_message(void*, const char* context, DkResult result, const char* message) {
    if (result != DkResult_Success) {
        fatal("deko3d error in %s: result %d: %s", context ? context : "?", int(result), message ? message : "");
    }
    const int n = g_debugMessages++;
    if (n < 200) {
        dklog("deko3d %s: %s", context ? context : "?", message ? message : "");
    } else if (n == 200) {
        dklog("deko3d: more messages, not logged");
    }
}

void init_swapchain() {
    const DkImage* images[kSwapImages];
    for (uint32_t i = 0; i < kSwapImages; i++) {
        DkImageLayoutMaker m;
        dkImageLayoutMakerDefaults(&m, R.device);
        m.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
        m.format = DkImageFormat_RGBA8_Unorm;
        m.dimensions[0] = R.width;
        m.dimensions[1] = R.height;
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &m);
        g_swapMem[i] = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
        dkImageInitialize(&g_swapImages[i], &layout, g_swapMem[i].block, g_swapMem[i].offset);
        images[i] = &g_swapImages[i];
    }
    DkSwapchainMaker m;
    dkSwapchainMakerDefaults(&m, R.device, nwindowGetDefault(), images, kSwapImages);
    dklog("creating the swapchain on the default window");
    log_flush();
    g_swapchain = dkSwapchainCreate(&m);
    dkSwapchainSetSwapInterval(g_swapchain, 1);
    dklog("swapchain: %u RGBA8 images of %ux%u, swap interval 1", kSwapImages, R.width, R.height);
}

}  // namespace

int swapchain_acquire(DkFence* fence) {
    int slot = 0;
    dkSwapchainAcquireImage(g_swapchain, &slot, fence);
    return slot;
}

void swapchain_wait(DkFence* fence) {
    check_queue("acquiring a swapchain image");
    dkQueueWaitFence(R.queue, fence);
}

const DkImage& swapchain_image(int slot) { return g_swapImages[slot]; }

void swapchain_present(int slot) {
    check_queue("the present");
    dkQueuePresentImage(R.queue, g_swapchain, slot);
    R.presents++;
}

bool env_flag(const char* name, bool def) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0';
}

void dklog(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    fprintf(stderr, "[dk] %s\n", line);
}

void log_flush() { fflush(stderr); }

void fatal(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    fprintf(stderr, "[dk] FATAL: %s\n", line);
    log_flush();
    abort();
}

void check_queue(const char* before) {
    if (!dkQueueIsInErrorState(R.queue)) return;
    fatal("the deko3d queue is in an error state (a GPU fault) before %s, after %llu presents; deko3d messages %d",
          before, (unsigned long long)R.presents, g_debugMessages);
}

}  // namespace swdk

using namespace swdk;

extern "C" void aurora_switch_dk_init(void) {
    if (R.device) return;
    diag_init();
    const uint64_t t0 = now_ns();
    const size_t heapBefore = heap_never_used_mib();
    dklog("deko3d renderer: Aurora's WebGPU calls replayed on deko3d; heap never used before the set-up %zu MiB",
          heapBefore);
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_message;
    // window origin top left, depth 0 to 1, clip-space y up: WebGPU's conventions (dk.h)
    dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
    dklog("creating the device");
    log_flush();
    R.device = dkDeviceCreate(&dm);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, R.device);
    // zcull off: SwitchWakerHD saw depth-biased decals lose whole tiles with it. Compute stays on:
    // Aurora's depth peek (GXPeekZ) is a compute pass.
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_MediumPrio | DkQueueFlags_DisableZcull;
    qm.commandMemorySize = kQueueCommandMemory;
    qm.flushThreshold = qm.commandMemorySize / 8;
    dklog("creating the queue");
    log_flush();
    R.queue = dkQueueCreate(&qm);

    memory_init();
    init_swapchain();
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        empty_image_init();
    }
    shaders_init();
    const size_t heapAfter = heap_never_used_mib();
    dklog("set-up cost: %zu MiB of heap (never used: %zu MiB before, %zu MiB after), %.0f ms",
          heapBefore > heapAfter ? heapBefore - heapAfter : 0, heapBefore, heapAfter, double(now_ns() - t0) / 1e6);
    log_flush();
}

extern "C" void aurora_switch_dk_shutdown(void) {
    if (R.queue == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        flush();
    }
    dkQueueWaitIdle(R.queue);
    if (g_swapchain != nullptr) {
        dkSwapchainDestroy(g_swapchain);  // gives the window's buffers back
        g_swapchain = nullptr;
    }
    dklog("shut down after %llu presents", (unsigned long long)R.presents);
}
