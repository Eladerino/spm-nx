#pragma once

// The Switch settings menu: the port's replacement for the runtime overlay's
// PC top bar, which is not drawn on Switch (settings_overlay.cpp routes its
// DrawTopBar call here under __SWITCH__). Its framework (centred panel, tab
// bar, scrolling pages, gamepad navigation fed to Dear ImGui's own gamepad
// navigation once per frame) is adapted from SwitchWakerHD's settings
// overlay by centollOS; the rows and their apply calls are this port's.
// Implementation and the full credit: switch/menu/spm_switch_menu.cpp.
//
// The overlay owns the seams that reach this header: F10 / Escape toggle
// visibility through SetVisible, the guest input block follows Visible(), and
// the weak spm_settings_menu_open() provider reports Visible().

namespace spm_switch_menu {

// True while the menu is open. Read by the overlay every frame for the input
// block and the FPS / shader-status window offsets.
bool Visible();

// Open or close the menu. Opening claims navigation focus on the next drawn
// frame and re-baselines the pad snapshot, so a button already held when the
// menu appears does not act until it is pressed again.
void SetVisible(bool visible);

// Read the pad, feed the navigation, and draw the window. Call once per
// frame from the overlay's Draw pass; draws nothing while hidden, and
// nothing while the startup screen is up.
void Draw();

}  // namespace spm_switch_menu
