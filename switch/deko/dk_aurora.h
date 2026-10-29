// The deko3d renderer's entry points for Aurora (switch/README.md, "Graphics"). Aurora draws through
// WebGPU on Dawn's Null device; the WebGPU calls that create resources and record commands are taken
// over at link time (wgpu_objects.cpp, wgpu_encode.cpp), so Aurora itself only calls these three.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The device, queue, swapchain and shader cache. From webgpu::initialize right after Dawn's device
// is created and before Aurora creates any texture or buffer.
void aurora_switch_dk_init(void);

// Shows `texture` (a WGPUTexture: Aurora's finished picture, RGBA8, ImGui included) on the screen,
// scaled to the window with its aspect kept. Blocks until a swapchain image is free (vsync). False
// if the texture has no deko3d shadow (logged once).
bool aurora_switch_dk_present(void* texture);

// Waits for the GPU and gives the window back (Aurora's shutdown).
void aurora_switch_dk_shutdown(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// The diag bridge, the other direction (dk.h, swdk::diag_set): Aurora's five diagnostic
// switches, settable at runtime. Each flag still initialises from its NSMBW_* env var on first
// use, exactly where the one-time getenv latches used to sit; these setters are what changes
// it afterwards.
namespace aurora::gx {
void nsmbw_set_mtx_log(bool on);
void nsmbw_set_uv_log(bool on);
void nsmbw_mtxlog_flush();
} // namespace aurora::gx
namespace aurora::gx::fifo {
void nsmbw_set_no_merge(int mode);
void nsmbw_set_array_reupload(bool on);
} // namespace aurora::gx::fifo
namespace aurora::gfx {
void stage_timers_set_enabled(bool on) noexcept;
void stage_time_print_now();
} // namespace aurora::gfx
#endif
