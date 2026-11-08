// Measurement only: time the GX worker thread spends blocked inside its replay on the renderer
// (Aurora's decode thread and frame worker, deko's locks, fences and shader builds), reported per
// frame by a [gxworker] line. Scopes on any other thread cost one call and record nothing. Weak so
// that targets without the runtime's GX worker still link.
#pragma once

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

extern "C" bool spm_gxworker_is_current_thread() noexcept __attribute__((weak));
extern "C" void spm_gxworker_note_blocked(uint32_t site, uint64_t micros) noexcept __attribute__((weak));

class SpmWorkerBlockedScope {
public:
    explicit SpmWorkerBlockedScope(SpmWorkerBlockedSite site) noexcept
        : site_(site), active_(spm_gxworker_is_current_thread != nullptr && spm_gxworker_is_current_thread()) {
        if (active_) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~SpmWorkerBlockedScope() {
        if (active_) {
            spm_gxworker_note_blocked(
                site_, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - start_)
                                                 .count()));
        }
    }
    SpmWorkerBlockedScope(const SpmWorkerBlockedScope&) = delete;
    SpmWorkerBlockedScope& operator=(const SpmWorkerBlockedScope&) = delete;

private:
    SpmWorkerBlockedSite site_;
    bool active_;
    std::chrono::steady_clock::time_point start_{};
};
