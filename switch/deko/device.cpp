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
    }
    std::fclose(f);
    dklog("diag: depth_off=%d cull_off=%d magenta=%d picdump=%d efbclear=%d bufdump=%d texdump=%d",
          g_diag.depthOff ? 1 : 0, g_diag.cullOff ? 1 : 0, g_diag.magenta ? 1 : 0, g_diag.picDump ? 1 : 0,
          g_diag.efbClear ? 1 : 0, g_diag.bufDump ? 1 : 0, g_diag.texDump ? 1 : 0);
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
