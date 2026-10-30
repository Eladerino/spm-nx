#pragma once

// The Switch settings menu: the port's replacement for the runtime overlay's
// PC top bar, which is not drawn on Switch (settings_overlay.cpp routes its
// DrawTopBar call here under __SWITCH__). One plain ImGui window, driven by
// polling the gamepad directly each frame it is visible; it uses no ImGui
// navigation, focus, or synthetic key input. Implementation:
// switch/menu/spm_switch_menu.cpp.
//
// The overlay owns the seams that reach this header: F10 / Escape toggle
// visibility through SetVisible, the guest input block follows Visible(), and
// the weak spm_settings_menu_open() provider reports Visible().

namespace spm_switch_menu {

// True while the menu is open. Read by the overlay every frame for the input
// block and the FPS / shader-status window offsets.
bool Visible();

// Open or close the menu. Opening re-baselines the pad sample, so a button
// already held when the menu appears does not act until it is pressed again.
void SetVisible(bool visible);

// Poll the pad, apply input, and draw the window. Call once per frame from
// the overlay's Draw pass; draws nothing while hidden, and nothing (and polls
// nothing) while the startup screen is up.
void Draw();

}  // namespace spm_switch_menu
