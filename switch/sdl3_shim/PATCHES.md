# Local changes to SwitchWaker's SDL 3 shim

Taken from SwitchWaker `switch/aurora/sdl3_shim/` at `92307ff`. SwitchWaker's shim covers the SDL
calls Aurora makes; WiiCompiled's runtime makes more, so this copy adds them. Changes, by file:

- `sdl3_shim_gamepad.c`: opening the pad is reference counted (`g_open`) so the runtime's gamepad
  and joystick views of the same controller can open and close it independently. New
  `sdl3_shim_gamepad_connected()` and `sdl3_shim_gamepad_if_open()` (declared in `sdl3_shim.h`)
  let the joystick API in `sdl3_shim_wiicompiled.c` reuse it.
- `sdl3_shim_audio.c`: a per-stream gain (starts at 1.0, applied when samples are queued) and
  `SDL_SetAudioStreamGain`, which the runtime's volume setting calls.
- `sdl3_shim_events.c`: `SDL_PumpEvents`, a public wrapper over the shim's existing pump.
- `sdl3_shim_wiicompiled.c` (new, this fork's code): the 37 remaining calls WiiCompiled makes. The
  joystick API as views of the one gamepad, `SDL_GetGamepads`/`SDL_GetJoysticks`, the
  name/string lookups, a fixed 1280x720 at 60 Hz display mode, cursor and scancode stubs,
  `SDL_GetPlatform`, `SDL_GetTicksNS` (from the CPU tick), the log setters and `SDL_strstr`.

Checked against the 3.4.4 headers: every `SDL_` symbol the runtime and Aurora reference resolves to
exactly one definition here, except the Android-only calls (compiled out on the Switch) and
`SDL_main` (libnx supplies `main`).
