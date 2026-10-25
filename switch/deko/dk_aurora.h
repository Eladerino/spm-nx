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
