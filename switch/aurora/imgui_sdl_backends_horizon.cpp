// ImGui's SDL 3 platform and SDL renderer backends, as the Switch has them: none. The SDL here is
// the small libnx shim (switch/sdl3_shim), which has no windows, cursors, clipboard or SDL_Renderer,
// so imgui_impl_sdl3.cpp and imgui_impl_sdlrenderer3.cpp are taken out of Aurora's imgui_backends and
// these take their place (SpmSwitch.cmake). ImGui draws through imgui_impl_wgpu. Aurora's
// imgui::new_frame sets the display size; the one thing the platform backend's NewFrame still owes
// ImGui is the frame time. Same approach as SwitchWaker's aurora/patches/0002, without patching
// Aurora's own source.
#include <chrono>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

bool ImGui_ImplSDL3_InitForSDLRenderer(SDL_Window*, SDL_Renderer*) { return true; }
void ImGui_ImplSDL3_Shutdown() {}
bool ImGui_ImplSDL3_ProcessEvent(const SDL_Event*) { return false; }
void ImGui_ImplSDL3_NewFrame() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - last).count();
    last = now;
    ImGui::GetIO().DeltaTime = delta > 0.0f ? delta : 1.0f / 60.0f;
}

// Aurora only takes the SDL renderer path when the window has an SDL_Renderer, which never happens
// here (the shim's window has none).
bool ImGui_ImplSDLRenderer3_Init(SDL_Renderer*) { return false; }
void ImGui_ImplSDLRenderer3_Shutdown() {}
void ImGui_ImplSDLRenderer3_NewFrame() {}
void ImGui_ImplSDLRenderer3_RenderDrawData(ImDrawData*, SDL_Renderer*) {}
