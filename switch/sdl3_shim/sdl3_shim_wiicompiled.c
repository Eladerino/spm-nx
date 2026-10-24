// SDL 3 functions that WiiCompiled's runtime (runtime/src) and its in-tree Aurora call beyond those
// SwitchWaker's shim implements (the other files here, from SwitchWaker; see PATCHES.md). Same rules
// as the rest of the shim: what the Switch has, on libnx and newlib, or an explicit failure.
//
// The console's controller is the shim's one gamepad, instance 1 (sdl3_shim_gamepad.c). The runtime
// also reads it through SDL's joystick API (the controller wizard and raw bindings), so the joystick
// calls below are views of that same gamepad: the joystick handle is the gamepad handle, buttons are
// numbered in SDL_GamepadButton order and axes in SDL_GamepadAxis order.
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include "sdl3_shim.h"

#define INSTANCE_ID 1

// ---- Joysticks and gamepads ---------------------------------------------------------------------

static SDL_JoystickID* connected_ids(int* count) {
    const bool connected = sdl3_shim_gamepad_connected();
    // SDL returns a 0-terminated array the caller frees with SDL_free (free() in this shim).
    SDL_JoystickID* ids = malloc(2 * sizeof *ids);
    if (ids == NULL) {
        if (count)
            *count = 0;
        return NULL;
    }
    ids[0] = connected ? INSTANCE_ID : 0;
    ids[1] = 0;
    if (count)
        *count = connected ? 1 : 0;
    return ids;
}

SDL_JoystickID* SDL_GetJoysticks(int* count) { return connected_ids(count); }
SDL_JoystickID* SDL_GetGamepads(int* count) { return connected_ids(count); }

bool SDL_IsGamepad(SDL_JoystickID instance_id) { return instance_id == INSTANCE_ID; }

SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID instance_id) {
    return (SDL_Joystick*)SDL_OpenGamepad(instance_id);
}

void SDL_CloseJoystick(SDL_Joystick* joystick) { SDL_CloseGamepad((SDL_Gamepad*)joystick); }

SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID instance_id) {
    return instance_id == INSTANCE_ID ? sdl3_shim_gamepad_if_open() : NULL;
}

SDL_Gamepad* SDL_GetGamepadFromPlayerIndex(int player_index) {
    return player_index == 0 ? sdl3_shim_gamepad_if_open() : NULL;
}

const char* SDL_GetJoystickNameForID(SDL_JoystickID instance_id) {
    return instance_id == INSTANCE_ID ? "Switch Controller" : NULL;
}

SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID instance_id) {
    return SDL_GetGamepadGUIDForID(instance_id);
}

int SDL_GetNumJoystickButtons(SDL_Joystick* joystick) {
    return joystick != NULL ? SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1 : -1;
}

int SDL_GetNumJoystickAxes(SDL_Joystick* joystick) {
    return joystick != NULL ? SDL_GAMEPAD_AXIS_COUNT : -1;
}

bool SDL_GetJoystickButton(SDL_Joystick* joystick, int button) {
    if (button < 0 || button > SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
        return false;
    return SDL_GetGamepadButton((SDL_Gamepad*)joystick, (SDL_GamepadButton)button);
}

Sint16 SDL_GetJoystickAxis(SDL_Joystick* joystick, int axis) {
    if (axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT)
        return 0;
    return SDL_GetGamepadAxis((SDL_Gamepad*)joystick, (SDL_GamepadAxis)axis);
}

// The Switch controller's layout is fixed; desktop mapping strings have nothing to change.
int SDL_AddGamepadMapping(const char* mapping) {
    if (mapping == NULL)
        return SDL_SetError("SDL_AddGamepadMapping: null mapping"), -1;
    return 0;
}

char* SDL_GetGamepadMappingForID(SDL_JoystickID instance_id) {
    (void)instance_id;
    SDL_SetError("the Switch controller has no mapping string");
    return NULL;
}

bool SDL_GamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type) {
    (void)gamepad;
    (void)type;
    return false;
}

// Inverses of the shim's SDL_GetGamepadStringForButton/Axis, so config files round-trip.
SDL_GamepadButton SDL_GetGamepadButtonFromString(const char* str) {
    if (str == NULL)
        return SDL_GAMEPAD_BUTTON_INVALID;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        const char* name = SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
        if (name != NULL && strcasecmp(name, str) == 0)
            return (SDL_GamepadButton)b;
    }
    return SDL_GAMEPAD_BUTTON_INVALID;
}

SDL_GamepadAxis SDL_GetGamepadAxisFromString(const char* str) {
    if (str == NULL)
        return SDL_GAMEPAD_AXIS_INVALID;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const char* name = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)a);
        if (name != NULL && strcasecmp(name, str) == 0)
            return (SDL_GamepadAxis)a;
    }
    return SDL_GAMEPAD_AXIS_INVALID;
}

// ---- Keyboard and mouse (none on the Switch) ----------------------------------------------------

// Keyboard bindings in a desktop config parse to "unknown" and are ignored.
SDL_Scancode SDL_GetScancodeFromName(const char* name) {
    (void)name;
    return SDL_SCANCODE_UNKNOWN;
}

const char* SDL_GetScancodeName(SDL_Scancode scancode) {
    (void)scancode;
    return "";
}

bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }

// ---- Display: one fixed 1280x720 mode, the size the deko3d swapchain presents ---------------------

static const SDL_DisplayMode kMode = {
    .displayID = 1,
    .format = SDL_PIXELFORMAT_ABGR8888,
    .w = SDL3_SHIM_WIDTH,
    .h = SDL3_SHIM_HEIGHT,
    .pixel_density = 1.0f,
    .refresh_rate = 60.0f,
    .refresh_rate_numerator = 60,
    .refresh_rate_denominator = 1,
};

const SDL_DisplayMode* SDL_GetCurrentDisplayMode(SDL_DisplayID displayID) {
    return displayID == 1 ? &kMode : NULL;
}

const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID displayID) {
    return displayID == 1 ? &kMode : NULL;
}

bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID displayID, int w, int h, float refresh_rate,
                                         bool include_high_density_modes, SDL_DisplayMode* closest) {
    (void)w;
    (void)h;
    (void)refresh_rate;
    (void)include_high_density_modes;
    if (displayID != 1 || closest == NULL)
        return SDL_SetError("SDL_GetClosestFullscreenDisplayMode: no such display");
    *closest = kMode;
    return true;
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window* window) { return window != NULL ? 1 : 0; }

// The window is always the whole screen; NULL means "fullscreen desktop", which it is.
const SDL_DisplayMode* SDL_GetWindowFullscreenMode(SDL_Window* window) {
    (void)window;
    return NULL;
}

bool SDL_SetWindowFullscreenMode(SDL_Window* window, const SDL_DisplayMode* mode) {
    (void)window;
    (void)mode;
    return true;
}

// ---- Core ---------------------------------------------------------------------------------------

const char* SDL_GetPlatform(void) { return "Nintendo Switch"; }

Uint64 SDL_GetTicksNS(void) { return armTicksToNs(armGetSystemTick()); }

void SDL_QuitSubSystem(SDL_InitFlags flags) { (void)flags; }

// SDL's own log goes to stderr in the shim (nxlink/USB log); the runtime keeps its own logger.
static SDL_LogOutputFunction g_log_callback;
static void* g_log_userdata;

void SDL_SetLogOutputFunction(SDL_LogOutputFunction callback, void* userdata) {
    g_log_callback = callback;
    g_log_userdata = userdata;
}

void SDL_SetLogPriority(int category, SDL_LogPriority priority) {
    (void)category;
    (void)priority;
}

char* SDL_strstr(const char* haystack, const char* needle) { return strstr(haystack, needle); }
