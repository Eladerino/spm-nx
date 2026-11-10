// SDL 3 touch for the Switch shim: the console's one touchscreen, read from
// libnx hid. Before this file the shim had no touch at all (the absent file
// covers haptics, standalone sensors, keyboard and mouse), so every SDL touch
// query went unanswered and no touchscreen pointer could exist.
//
// Coordinates: libnx reports panel pixels (the panel is 1280x720); SDL finger
// coordinates are normalised over the device surface, so x/1280 and y/720,
// clamped — a finger at the panel edge reads exactly 0 or 1, never beyond.
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "sdl3_shim.h"

#define TOUCH_ID 1
#define PANEL_WIDTH 1280
#define PANEL_HEIGHT 720
#define MAX_FINGERS 16

static bool g_touch_ready;

static void touch_ensure(void) {
    if (!g_touch_ready) {
        hidInitializeTouchScreen();
        g_touch_ready = true;
    }
}

SDL_TouchID* SDL_GetTouchDevices(int* count) {
    if (count != NULL)
        *count = 0;
    touch_ensure();
    SDL_TouchID* ids = malloc(2 * sizeof(SDL_TouchID));
    if (ids == NULL)
        return NULL;
    ids[0] = TOUCH_ID;
    ids[1] = 0;
    if (count != NULL)
        *count = 1;
    return ids;
}

const char* SDL_GetTouchDeviceName(SDL_TouchID touchID) {
    return touchID == TOUCH_ID ? "Switch touchscreen" : NULL;
}

SDL_TouchDeviceType SDL_GetTouchDeviceType(SDL_TouchID touchID) {
    return touchID == TOUCH_ID ? SDL_TOUCH_DEVICE_DIRECT : SDL_TOUCH_DEVICE_INVALID;
}

// The returned array is a single allocation (the SDL 3 contract): the finger
// structs live behind the pointer array in the same block, so the caller's
// one SDL_free releases everything.
SDL_Finger** SDL_GetTouchFingers(SDL_TouchID touchID, int* count) {
    if (count != NULL)
        *count = 0;
    if (touchID != TOUCH_ID)
        return NULL;
    touch_ensure();
    HidTouchScreenState state;
    memset(&state, 0, sizeof state);
    hidGetTouchScreenStates(&state, 1);
    int n = state.count;
    if (n < 0)
        n = 0;
    if (n > MAX_FINGERS)
        n = MAX_FINGERS;
    SDL_Finger** fingers = malloc((size_t)(n + 1) * sizeof(SDL_Finger*) + (size_t)n * sizeof(SDL_Finger));
    if (fingers == NULL)
        return NULL;
    SDL_Finger* pool = (SDL_Finger*)(void*)(fingers + (n + 1));
    for (int i = 0; i < n; ++i) {
        const HidTouchState* t = &state.touches[i];
        const float x = (float)t->x / (float)PANEL_WIDTH;
        const float y = (float)t->y / (float)PANEL_HEIGHT;
        pool[i].id = t->finger_id;
        pool[i].x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
        pool[i].y = y < 0.0f ? 0.0f : y > 1.0f ? 1.0f : y;
        pool[i].pressure = 1.0f;
        fingers[i] = &pool[i];
    }
    fingers[n] = NULL;
    if (count != NULL)
        *count = n;
    return fingers;
}
