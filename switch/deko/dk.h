// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Adapted from SwitchWaker (https://github.com/centollOS/SwitchWaker) at 92307ff: switch/deko/dk.h,
// itself adapted from SwitchWakerHD's runtime/src/gfx/deko/dk.h. Reworked for spm-nx: no frame
// slots (WebGPU has no frames), one submission serial per queue submit instead.
#pragma once
// The deko3d side of the Switch renderer (switch/README.md, "Graphics"). Aurora draws through
// WebGPU on Dawn's Null device, which keeps the object model and draws nothing; wgpu_objects.cpp and
// wgpu_encode.cpp take Dawn's place for the calls that create GPU resources and record commands
// (the linker's --wrap), and replay them here:
//   - one device, one graphics queue and one command buffer, recorded only under queue_mutex();
//   - submission serials: every queue submit ends with a fence; memory a submit used (command memory,
//     upload space, freed images and buffers) is reused once the GPU has passed its serial;
//   - the image heap, buffers as memory blocks of their own, the shader code block and the
//     descriptor sets;
//   - the shader registry: WGSL -> GLSL (Tint, shader_translate.cpp) -> DKSH (uam, on one worker
//     thread), cached on the SD card across runs.
// Device conventions: DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne, which with the
// Dawn patches dawn-switch-tint-position-y-up and -depth-zero-to-one is WebGPU's clip space and
// window origin, so nothing is flipped anywhere (SwitchWaker checked this on the console).
#include <deko3d.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "shader_translate.h"

namespace swdk {

// ---- the device (device.cpp) ---------------------------------------------------------------------
struct Renderer {
    DkDevice device = nullptr;
    DkQueue queue = nullptr;
    DkCmdBuf cmd = nullptr;  // recorded only with queue_mutex() held
    uint32_t width = 1280, height = 720;  // the swapchain images
    uint64_t presents = 0;
};
extern Renderer R;

// The window (device.cpp): an image to draw into, then shown. swapchain_acquire blocks until an image
// is free (up to a vsync) and is called outside queue_mutex(), so submits from other threads carry on;
// swapchain_wait (under queue_mutex()) makes the queue wait for the image's fence. The present's
// commands are flushed between that and swapchain_present (under queue_mutex()).
int swapchain_acquire(DkFence* fence);
void swapchain_wait(DkFence* fence);
const DkImage& swapchain_image(int slot);
void swapchain_present(int slot);

// Everything that records into R.cmd or submits holds this: the replay of a queue submit, queue
// writes, the present. Any thread (Aurora's frame worker, its presenter, its FIFO thread).
std::mutex& queue_mutex();

// ---- submissions (memory.cpp) ----------------------------------------------------------------------
// The serial R.cmd records into now; flush() ends it (fence, submit) and starts the next.
uint64_t recording_serial();
// Under queue_mutex(): the commands recorded so far to the GPU, with a fence for their serial.
void flush();
// Any thread: the newest serial the GPU has finished (polls, never waits).
uint64_t completed_serial();
// Any thread, without queue_mutex(): blocks until the GPU has finished `serial` (flushing first if
// that serial is still being recorded). False if the GPU did not get there in 10 s (logged).
bool wait_serial(uint64_t serial);
// Under queue_mutex(): keeps `obj` alive until the GPU has finished the recording serial.
void keep_until_done(std::shared_ptr<void> obj);
void keep_until_done(std::vector<std::shared_ptr<void>>&& objs);

// ---- memory (memory.cpp) -----------------------------------------------------------------------------
constexpr uint32_t kCodeSize = 32u << 20;  // shader code (DKSH), bump allocated, never freed
constexpr uint32_t kImageDescriptors = 4096, kSamplerDescriptors = 512;
// Thread priorities: 0x3B at most (SwitchWakerHD's first deko3d NRO died on a thread at 0x3C).
constexpr int kMaxThreadPriority = 0x3B;

void memory_init();

// Upload space for queue writes, valid for the recording serial (under queue_mutex()).
struct UploadAlloc {
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = 0;
    explicit operator bool() const { return cpu != nullptr; }
};
UploadAlloc upload_alloc(uint32_t size, uint32_t alignment);

// A suballocation of the image heap. Thread-safe.
struct ImageAlloc {
    DkMemBlock block = nullptr;
    uint32_t offset = 0, size = 0;
    int chunk = -1;
};
ImageAlloc image_alloc(uint32_t size, uint32_t alignment);
void image_free(const ImageAlloc& a);  // the GPU no longer uses it (the caller made sure)
// Before dkImageLayoutInitialize of a block-linear 2D image whose level 0 is `rows` rows tall:
// deko3d 0.5.0's level-0 tile can overlap the next level for 6-8-row images (SwitchWakerHD).
void image_tile_size_fix(DkImageLayoutMaker& m, uint32_t rows);

// A memory block of its own, CPU-uncached and GPU-cached (WebGPU buffers). Thread-safe.
struct Block {
    DkMemBlock block = nullptr;
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = 0;
    uint32_t size = 0;
    explicit operator bool() const { return block != nullptr; }
};
Block block_create(uint32_t size, const char* what);
void block_destroy(Block& b);  // the GPU no longer uses it (the caller made sure)

// Copies DKSH into the code block and initializes the shader; false (logged) if full or invalid.
// Thread-safe.
bool code_load(DkShader& shader, const void* dksh, uint32_t size, const char* name);
// Under queue_mutex(): whether code was loaded since the last call (then the shader caches are
// invalidated before the next bind)
bool code_take_written();
uint32_t code_used();
DkGpuAddr image_descriptors();
DkGpuAddr sampler_descriptors();

struct MemoryStats {
    uint64_t imageBytes = 0, imageChunks = 0, blockBytes = 0, blocks = 0, uploadChunks = 0, cmdChunks = 0;
    uint64_t submits = 0;
};
MemoryStats memory_stats();

// ---- descriptors (descriptors.cpp), under queue_mutex() ------------------------------------------------
// Image slots below kReservedImages and sampler slots below kReservedSamplers are the renderer's own;
// the rest go to WebGPU texture views and samplers. Writes are commands (dkCmdBufPushData).
constexpr uint32_t kEmptyImage = 1;  // a 1x1 black texture (views without a shadow)
constexpr uint32_t kReservedImages = 16;
enum : uint32_t { kSamplerLinearClamp = 0, kSamplerNearestClamp = 1, kReservedSamplers = 16 };
void descriptors_bind();  // at the start of every command list: binds the sets
void write_image_view_descriptor(uint32_t slot, const DkImageView& view);
uint32_t image_slot_alloc();          // 0 when none is free (logged)
void image_slot_release(uint32_t slot);  // any thread; reused at the next flush
void descriptors_reclaim();           // under queue_mutex(): released slots back to the free list
// A sampler slot for this sampler (key: a hash of it), written now if new; the least recently used
// slot the GPU is done with is reused when all are taken (kSamplerNearestClamp if none can be).
uint32_t sampler_slot(const DkSampler& sampler, uint64_t key);
void empty_image_init();  // kEmptyImage, once

// ---- shaders (shaders.cpp) ---------------------------------------------------------------------------
// A compiled stage: the DKSH loaded into code memory, and what each WGSL binding became.
struct CompiledStage {
    DkShader shader{};
    std::vector<SlotBinding> bindings;
    uint32_t workgroup[3] = {1, 1, 1};
};
// One entry point of a WGSL module as a deko3d shader: from the cache file (SD card) when this
// exact WGSL, entry point and stage were compiled before, else translated by Tint here and compiled
// by the uam worker (this thread waits). Null if it cannot be made (logged, GLSL dumped to the SD).
// Results are kept for the process; any thread.
const CompiledStage* compile_stage(const std::string& wgsl, const std::string& entryPoint, ShaderStage stage);
void shaders_init();
struct ShaderStats {
    uint64_t fromFile = 0, compiled = 0, failed = 0, compileNs = 0, compileMaxNs = 0;
};
ShaderStats shader_stats();

// ---- logging -------------------------------------------------------------------------------------
void dklog(const char* format, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void fatal(const char* format, ...) __attribute__((format(printf, 1, 2)));
// deko3d aborts without a word when a creation fails (release library: 2359-xxxx), so creations
// log what they are about to do and flush first.
void log_flush();
// The queue is in an error state after a GPU fault, and deko3d aborts on any call that touches it:
// checked before each submit and present.
void check_queue(const char* before);
bool env_flag(const char* name, bool def);

// ---- diagnostics (device.cpp): switches from UserData/dk_diag.txt, read once at renderer init ----
// A bisect aid for the black-game-picture problem (the startup cover, an overlay, reaches the
// screen; the game's own GX content does not). Each switch disables or exposes one piece of the
// replay so a single boot can clear a whole suspect. No file = every switch off = normal build.
struct Diag {
    bool depthOff = false;  // depth/stencil test and write forced off at every pipeline bind
    bool cullOff = false;   // rasterizer cull mode forced to None at every pipeline bind
    bool magenta = false;   // the swapchain image is cleared magenta before every present blit
    bool picDump = false;   // the picture is read back at a few presents and its content logged
    bool efbClear = false;
    bool bufDump = false;   // at a few presents (GPU idle), every buffer's size, usage and non-zero bytes  // colour clears of passes with a depth target (the EFB) are magenta
    bool texDump = false;   // the most-sampled textures are read back to files at a few presents
};
extern Diag g_diag;
void diag_init();

inline uint64_t now_ns() {
    uint64_t ticks;
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks * 625 / 12;  // 19.2 MHz
}

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

}  // namespace swdk
