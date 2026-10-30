#pragma once

// The Switch Developer shell: the rows of the port's old settings menu
// that upstream's Options menu does not carry (the deko diagnostics, the
// guest profiler, texture dumps, the shader-cache action, and a handful
// of renderer and audio switches). settings_overlay.cpp routes its
// DrawTopBar call here under __SWITCH__, so the shell draws where the PC
// top bar would; it opens only from Options > Game > Developer tools,
// whose row calls SetTopBarVisible(true) and lands in SetVisible below.
// Its framework (centred panel, scrolling page, gamepad navigation fed
// to Dear ImGui's own gamepad navigation once per frame) is adapted from
// SwitchWakerHD's settings overlay by centollOS. Implementation and the
// full credit: switch/menu/spm_switch_menu.cpp.
//
// The overlay owns the seams that reach this header: SetTopBarVisible /
// F10 toggle visibility through SetVisible, the guest input block
// follows Visible(), and the weak spm_settings_menu_open() provider
// reports Visible() (ORed with the Options menu's own state there).

namespace spm_switch_menu {

// True while the shell is open. Read by the overlay every frame for the
// input block and the FPS / shader-status window offsets.
bool Visible();

// Open or close the shell. Opening claims navigation focus on the next
// drawn frame and re-baselines the pad snapshot, so a button already
// held when the shell appears does not act until it is pressed again.
void SetVisible(bool visible);

// Read the pad, feed the navigation, and draw the window. Call once per
// frame from the overlay's Draw pass; draws nothing while hidden, and
// nothing while the startup screen is up.
void Draw();

}  // namespace spm_switch_menu
