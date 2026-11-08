// Measurement only: time the GX worker and the frame worker spend blocked on the renderer. For the GX
// worker, inside its replay
// (Aurora's decode thread and frame worker, deko's locks, fences and shader builds), reported per
// frame by a [gxworker] line. Scopes on any other thread cost one call and record nothing. Weak so
// that targets without the runtime's GX worker still link.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

enum SpmWorkerBlockedSite : uint32_t {
    kSpmBlockedDecodeQueue = 0,  // Aurora's decode queue full
    kSpmBlockedDecodeSync,       // waiting for the decode thread to finish (copies, syncs)
    kSpmBlockedFrameWorker,      // waiting on a frame worker phase
    kSpmBlockedStagingMap,       // waiting for the staging buffer map
    kSpmBlockedQueueLock,        // contending for deko's queue mutex
    kSpmBlockedGpuFence,         // waiting for the GPU
    kSpmBlockedShader,           // waiting for a shader build
    kSpmBlockedSiteCount,
};

// Set by the frame worker for itself; its blocked time is read by the [fseal] line.
inline thread_local bool t_spmFrameWorkerThread = false;
inline std::atomic<uint64_t> g_spmFrameWorkerBlockedMicros[kSpmBlockedSiteCount]{};

extern "C" bool spm_gxworker_is_current_thread() noexcept __attribute__((weak));
extern "C" void spm_gxworker_note_blocked(uint32_t site, uint64_t micros) noexcept __attribute__((weak));

class SpmWorkerBlockedScope {
public:
    explicit SpmWorkerBlockedScope(SpmWorkerBlockedSite site) noexcept
        : site_(site),
          gxWorker_(spm_gxworker_is_current_thread != nullptr && spm_gxworker_is_current_thread()),
          active_(gxWorker_ || t_spmFrameWorkerThread) {
        if (active_) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~SpmWorkerBlockedScope() {
        if (active_) {
            const uint64_t micros = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count());
            if (gxWorker_) {
                spm_gxworker_note_blocked(site_, micros);
            } else {
                g_spmFrameWorkerBlockedMicros[site_].fetch_add(micros, std::memory_order_relaxed);
            }
        }
    }
    SpmWorkerBlockedScope(const SpmWorkerBlockedScope&) = delete;
    SpmWorkerBlockedScope& operator=(const SpmWorkerBlockedScope&) = delete;

private:
    SpmWorkerBlockedSite site_;
    bool gxWorker_;
    bool active_;
    std::chrono::steady_clock::time_point start_{};
};
