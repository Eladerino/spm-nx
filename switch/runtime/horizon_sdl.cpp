// Switch stand-ins for three SDL functions the pinned SDL 3.4.4 headers declare but the
// Switch SDL build leaves unimplemented: Horizon has no virtual joysticks and no text
// input, so the final link of spm-nx.elf failed on them. Each stub returns SDL's
// documented failure value, and both call sites tolerate failure:
//   - settings_overlay.cpp's RebindSelfTest() is an upstream test that runs only when
//     SPM_TEST_REBIND is set; it checks the attach result and stops when it is 0, and
//     the two SDL_SetJoystickVirtualButton calls sit behind that check.
//   - aurora's imgui::initialize() calls SDL_StartTextInput() (its Windows IME
//     workaround) and ignores the return value.
// The definitions are checked against the real header declarations by including them.

#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_keyboard.h>

SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc*) { return 0; }

bool SDL_SetJoystickVirtualButton(SDL_Joystick*, int, bool) { return false; }

bool SDL_StartTextInput(SDL_Window*) { return false; }
