// ImGui's SDL 3 platform and SDL renderer backends, as the Switch has them: none. The SDL here is
// the small libnx shim (switch/sdl3_shim), which has no windows, cursors, clipboard or SDL_Renderer,
// so imgui_impl_sdl3.cpp and imgui_impl_sdlrenderer3.cpp are taken out of Aurora's imgui_backends and
// these take their place (SpmSwitch.cmake). ImGui draws through imgui_impl_wgpu. Aurora's
// imgui::new_frame sets the display size; the one thing the platform backend's NewFrame still owes
// ImGui is the frame time. Same approach as SwitchWaker's aurora/patches/0002, without patching
// Aurora's own source.
//
// ProcessEvent below is real, though: the only key events in the shim's SDL queue are the ones the
// gamepad shim synthesises (the F10 menu tap, and the pad translated to navigation keys while the
// menu is open), and those are mapped into ImGui key events so the menus can be driven.
#include <chrono>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

bool ImGui_ImplSDL3_InitForSDLRenderer(SDL_Window*, SDL_Renderer*) { return true; }
void ImGui_ImplSDL3_Shutdown() {}

// The navigation set, plus Space and the F-keys so F10/F11 stay visible to
// ImGui; anything else the queue might carry is not menu input.
static ImGuiKey ImGuiKeyFromScancode(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    default: break;
    }
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (scancode - SDL_SCANCODE_F1));
    return ImGuiKey_None;
}

bool ImGui_ImplSDL3_ProcessEvent(const SDL_Event* event) {
    if (ImGui::GetCurrentContext() == nullptr)
        return false;
    ImGuiIO& io = ImGui::GetIO();
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const ImGuiKey key = ImGuiKeyFromScancode(event->key.scancode);
        if (key == ImGuiKey_None)
            return false;
        const bool down = event->type == SDL_EVENT_KEY_DOWN;
        // Backends report modifier state through the Mod keys as well as the
        // physical ones; Shift+Tab only reads as "previous" with both set.
        if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift)
            io.AddKeyEvent(ImGuiMod_Shift, down);
        io.AddKeyEvent(key, down);
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        io.AddInputCharactersUTF8(event->text.text);
        return true;
    default:
        return false;
    }
}

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
