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

// ---- diagnostics (device.cpp): switches from UserData/dk_diag.txt, read at renderer init ------
// Runtime diagnostics for the renderer. Each switch disables or exposes one piece of the replay
// so a single boot can clear a whole suspect, or read state back (the picture, the most-sampled
// textures) for inspection. No file = every switch at its default below, which is off for all
// but cpthread, gxthread and zcull (a "cpthread=0", "gxthread=0" or "zcull=0" line turns those off too) = normal build. The fields are atomic:
// diag_set below flips them at runtime (the settings menu and the dk_cmd.txt channel are the
// callers), so a switch takes effect from its next use without a reboot.
struct Diag {
    std::atomic<bool> depthOff = false;  // depth/stencil test and write forced off at every pipeline bind
    std::atomic<bool> cullOff = false;   // rasterizer cull mode forced to None at every pipeline bind
    std::atomic<bool> zcull = true;      // the queue's zcull (early depth rejection). On in this test build: SwitchWakerHD fixed the depth-biased decal loss that turned it off here (bias invalidate in wgpu_encode.cpp) and measured +6 fps GPU-bound at stock; "zcull=0" restores the old queue exactly. Creation-time only: read once at queue creation, no menu row
    std::atomic<bool> magenta = false;   // the swapchain image is cleared magenta before every present blit
    std::atomic<bool> picDump = false;   // the picture is read back at a few presents and its content logged
    std::atomic<bool> efbClear = false;  // colour clears of passes with a depth target (the EFB) are magenta
    std::atomic<bool> bufDump = false;   // at a few presents (GPU idle), every buffer's size, usage and non-zero bytes
    std::atomic<bool> texDump = false;   // the most-sampled textures are read back to files at a few presents
    std::atomic<bool> shaderDump = false;   // every translated shader: its WGSL and GLSL are saved as it compiles
    std::atomic<bool> noMerge = false;   // Aurora draw merging off (sets NSMBW_NO_MERGE=1 before the first draw)
    std::atomic<bool> uvLog = false;   // Aurora logs staged textures whose texcoord scale and size disagree (sets NSMBW_UV_LOG=1 before the first draw)
    std::atomic<bool> mtxLog = false;   // Aurora logs the postex matrices staged per draw (sets NSMBW_MTX_LOG=1 before the first draw)
    std::atomic<bool> perfTimer = false;   // Aurora stage timers: producer, map wait and encode totals every 600 frames (sets NSMBW_STAGE_TIMERS=1 before the first draw)
    std::atomic<bool> arrayUp = false;   // Aurora re-uploads every draw's vertex arrays (sets NSMBW_ARRAY_REUPLOAD=1 before the first draw)
    std::atomic<bool> profile = false;   // the runtime's guest sampling profiler captures the game thread while on; turning it off prints the hot list
    std::atomic<bool> poseLog = false;   // the runtime logs each n_controler anim pose drawn (archive, group, pose, map, sequence), once per distinct combination; log only, no draw is ever skipped
    std::atomic<bool> fps30 = false;     // the 30 fps cap: while on, gameplay presents are shown one in two (the divider in aurora_switch_dk_present), so each shown frame is held for a second vblank and the picture updates at 30 Hz while the guest keeps its own pace. Gameplay only: the present path asks the runtime (spm_gameplay_present) and never caps the title, the loads or the Prepare sweep
    std::atomic<bool> gxSkip = false;    // the GX-suppression spike: while on, alternate gameplay frames have their GX output discarded before any of it is decoded (the runtime's gxskip state in gx_worker.h, forwarded through spm_gxskip_set; Aurora's FIFO sink drops the rest) and their presents are held here like fps30's. The guest tick still runs in full, so a perftimer window prices a tick whose GX costs nothing — L, the number that decides whether a double-step full-speed mode is worth building. Measurement only, not a player feature. Gameplay only, on the same gate as fps30
    std::atomic<bool> dlProbe = false;   // the display-list cache eligibility probe: while on, the runtime hashes every display list the guest submits (gx_dl.cpp's DlProbe, forwarded through spm_dlprobe_set) and each perftimer window prints a [dlprobe] line saying how many of those bytes were the same list at the same address as last time — the share a decoded-list cache would serve instead of re-decoding. Measurement only: nothing is cached, skipped or reordered, and while off the submit path pays one atomic load
    std::atomic<bool> gxDedup = false;   // the GX setter dedup experiment: while on, the runtime's vertex setters skip re-emitting a value Aurora's vertex state already holds (the gxdedup mirror in gx_vertex.cpp, forwarded through spm_gxdedup_set), never while a display list is being recorded. With perftimer on, the log's [gxdedup] line counts skipped/total setter calls per family — the no-change share that decides whether the early-out earns its keep. Takes effect at once
    std::atomic<bool> dstep = false;     // the double-step mode: while on, gameplay runs two guest ticks per shown frame — the first tick's GX output is discarded through the gxskip sink (the runtime's dstep state in gx_worker.h, forwarded through spm_dstep_set) and its present is held here like gxskip's, the second tick renders and shows — so the game advances at up to twice the shown rate; how close to full speed it gets is the rendered tick's price, and the runtime's [dstep] line reports the rate reached. Input is read again on the second tick, so controls answer at tick rate, not shown rate. Gameplay only, on the same gate as fps30; yields to gxskip when both are on, and fps30's divider stands down while this is on
    std::atomic<bool> cpThread = true;   // Aurora decodes GX commands on a thread of its own on core 1, from the end of the frame it is set in (lib/gx/fifo.cpp); on unless dk_diag.txt says cpthread=0
    std::atomic<bool> gxWorker = false;  // the runtime records guest GX calls and replays each one at once (gx_worker.h, Immediate), from the next frame
    std::atomic<bool> gxDefer = false;   // as gxworker, but the records wait for the next sync point (Deferred); implies gxworker
    std::atomic<bool> gxThread = true;   // as gxdefer, but the records replay on a worker thread on core 2 (Threaded); implies both. On by default: mode 3 soaked clean in Prepare run 2 (0 replay errors, 0 skipped draws); a "gxthread=0" line in dk_diag.txt still wins and drops back to mode 0
    std::atomic<bool> dkCmd = false;     // arms the dk_cmd.txt command channel: while off, the present path never opens the file (test build spec A2). Scripted runs set dkcmd=1
    std::atomic<bool> guestCore = false; // helper threads (pipeline compile, uam, presenter) are kept off the guest's core 0 by the pthread trampoline (test build spec B2). Boot-time only: the trampoline reads dk_diag.txt itself, before diag_init exists; this field mirrors it for the boot log
    std::atomic<bool> dkServer = false;  // the debug server (source/debug_server.h, commands in source/spm_debug_server.cpp): a plain-text TCP line server on port 6543 for dev runs. Boot-time only: diag_init starts it once when this is on, and diag_set storing the field starts and stops nothing (the server's own diag command refuses the key). Dev runs only: the server has NO authentication, so anyone on the LAN can flip the switches above, move files and press buttons while it runs
    std::atomic<bool> rttLog = false;    // the Return to Title hang log (spm_map_sweep.cpp's RttLogStep): while the game sits in SEQ_LOAD, the state the file select's startup waits on is written to the log once a second, reads only, nothing is changed. File-only like dkserver: no menu row; diag_init hands the boot value to the runtime through spm_rtt_log_set, and diag_set forwards flips the same way it forwards poselog
    std::atomic<bool> pmsMemo = false;   // the message-string patch memo (settings_overlay.cpp's PatchMessageStringsMemoised): while on, the passes that rewrite the game's instruction lines for the controller verify each already-patched line at its remembered address and only re-search lines whose check fails, instead of sweeping all of MEM1 per line on every sequence change. The patches applied are identical; a [pmsmemo] log line counts verifications, in-place re-patches, verify failures and full searches. Off: the original search-every-pass function runs untouched. diag_init hands the boot value to the runtime through spm_pmsmemo_set, and diag_set forwards flips the same way it forwards poselog
    std::atomic<bool> axfxNative = false; // the AXFX Hi reverb native experiment (ax_effects.cpp's AXFXReverbHiExpCallback registration): while on, the runtime's native wrapper for the game's AXFXReverbHiExp callback (0x802881C0, about 8.5% of the game thread) is armed and reports once in the log; while off, calls pass straight to the translated body, exactly as without the registration. diag_init hands the boot value to the runtime through spm_axfx_native_set, and diag_set forwards flips the same way
};
extern Diag g_diag;
void diag_init();
// Sets one switch by its dk_diag.txt name ("depth_off", "nomerge", ...), at runtime: the
// interposer switches flip here, the six Aurora switches are forwarded to Aurora's own flags
// (dk_aurora.h), the profile switch to the runtime's guest profiler, and the GX record switches
// to the record ring. False for an unknown name. Any thread.
bool diag_set(const char* key, bool on);
// The read side of diag_set: the current state of one switch by name, for the Switch
// settings menu's Diagnostics rows. Unknown names read as off.
bool diag_get(const char* key);
// Every switch, in dk_diag.txt order, for the listings that are not the settings menu
// (the debug server's "diag" command, which has to answer for keys it does not know by
// name in advance). The callback gets the file spelling and the current value.
void diag_enumerate(void (*visit)(const char* name, bool on, void* user), void* user);
// R.presents, for "[diag]" log markers written outside device.cpp.
uint64_t diag_present_count();
// Pushes every buffered diagnostic line to UserData/Logs/stderr.log now: Aurora's mtxlog
// buffer, a partial perftimer window while perftimer is on, the stderr stream itself, and a
// "[diag] logs flushed (<reason>)" marker last, so the pulled file's tail proves where the
// flush point was. Any thread.
void diag_flush_all(const char* reason);
// The dk_cmd.txt command channel: the present path calls this every 30th present; when
// UserData/dk_cmd.txt holds a command it is executed and the file truncated. The commands
// are listed at diag_cmd_poll in device.cpp.
void diag_cmd_poll();

inline uint64_t now_ns() {
    uint64_t ticks;
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks * 625 / 12;  // 19.2 MHz
}

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

}  // namespace swdk
