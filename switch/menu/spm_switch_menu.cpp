// The Switch settings menu (spm_switch_menu.h). One ImGui window drawn from
// the runtime overlay's Draw pass in place of the PC top bar.
//
// Input is the part that has to be boring: while the menu is visible it polls
// the player-1 gamepad directly, once per frame, through the same SDL getters
// gameplay input reads (they return the shim's latest published sample), keeps
// the previous frame's sample for edge detection, and adds a held-repeat for
// directions and adjustments. Selection is this file's own index into the row
// list, and rows draw with that state explicitly. Nothing here uses ImGui
// navigation, window focus, or synthetic key events: two attempts at feeding
// this port's pad to ImGui nav failed on hardware, so the menu does not ask.
//
// Rows are a view over live state, never a copy: every row reads its backing
// getter when it draws and when it acts. Rows the overlay exports accessors
// for go through settings_overlay.h. The rest live in settings_overlay.cpp
// file statics, so those rows read RuntimeConfigFile (the overlay persists
// every change there immediately, and nothing else mutates those settings on
// Switch) and write through the same apply calls the PC handlers use.

#include "spm_switch_menu.h"

#include <SDL3/SDL_gamepad.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/pad.h>
#include <imgui.h>

#include "audio_backend.h"
#include "display_settings.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "settings_overlay.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Defined by the runtime's PAD HLE (also declared locally by
// settings_overlay.cpp, whose header for it is not on this include path).
extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);

// Defined in runtime/src/hle/audio/ax_mix.cpp; settings_overlay.cpp declares
// it locally for the same reason.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

// The deko interposer's diag surface (switch/deko/dk.h, defined in device.cpp
// in the spm_deko library, which the product links). Declared here rather
// than included: dk.h pulls in deko3d and the shader translator, which the
// runtime target does not build against. Signatures match dk.h.
namespace swdk {
bool diag_set(const char* key, bool on);
bool diag_get(const char* key);
void diag_flush_all(const char* reason);
uint64_t diag_present_count();
void log_flush();
}  // namespace swdk

namespace spm_switch_menu {

bool g_visible = false;
int g_selected = 0;

// ---------------------------------------------------------------------------
// Rows

enum class RowId : int32_t {
    kResolution,
    kAspect,
    kVsync,
    kBloom,
    kCopyFilter,
    kSkipDraws,
    kShowFps,
    kFrameGraph,
    kMasterVolume,
    kMusicVolume,
    kSfxVolume,
    kUiVolume,
    kVoicesVolume,
    kMute,
    kMixWorker,
    kRumble,
    kSpinningCoins,
    kDevGate,
    kDiagFirst,  // kDiagFirst + n indexes kDiagKeys / kDiagLabels
    kDiagLast = kDiagFirst + 13,
    kFlushLogs,
    kMarkLog,
};

struct RowDef {
    const char* section;
    RowId id;
    const char* label;
};

constexpr RowId DiagRow(int n) {
    return static_cast<RowId>(static_cast<int32_t>(RowId::kDiagFirst) + n);
}

// Section order per the replacement-menu spec: Graphics, Display, Audio,
// Game, Developer.
const RowDef kRows[] = {
    {"Graphics", RowId::kResolution, "Resolution"},
    {"Graphics", RowId::kAspect, "Aspect"},
    {"Graphics", RowId::kVsync, "Vsync"},
    {"Graphics", RowId::kBloom, "Bloom"},
    {"Graphics", RowId::kCopyFilter, "Disable copy filter"},
    {"Graphics", RowId::kSkipDraws, "Skip draws while shaders compile"},
    {"Display", RowId::kShowFps, "Show FPS"},
    {"Display", RowId::kFrameGraph, "Frame graph"},
    {"Audio", RowId::kMasterVolume, "Master volume"},
    {"Audio", RowId::kMusicVolume, "Music volume"},
    {"Audio", RowId::kSfxVolume, "Sound effects volume"},
    {"Audio", RowId::kUiVolume, "UI volume"},
    {"Audio", RowId::kVoicesVolume, "Voices volume"},
    {"Audio", RowId::kMute, "Mute"},
    {"Audio", RowId::kMixWorker, "Audio mix worker"},
    {"Game", RowId::kRumble, "Controller vibration"},
    {"Game", RowId::kSpinningCoins, "Spinning coins"},
    {"Developer", RowId::kDevGate, "Show developer options"},
    {"Developer", DiagRow(0), "Depth test off (depth_off)"},
    {"Developer", DiagRow(1), "Face culling off (cull_off)"},
    {"Developer", DiagRow(2), "Magenta unbound textures (magenta)"},
    {"Developer", DiagRow(3), "Dump frame pictures (picdump)"},
    {"Developer", DiagRow(4), "Clear the EFB each pass (efbclear)"},
    {"Developer", DiagRow(5), "Dump staged buffers (bufdump)"},
    {"Developer", DiagRow(6), "Dump textures (texdump)"},
    {"Developer", DiagRow(7), "Dump shaders (shaderdump)"},
    {"Developer", DiagRow(8), "No draw merging (nomerge)"},
    {"Developer", DiagRow(9), "UV trace (uvlog)"},
    {"Developer", DiagRow(10), "Matrix trace (mtxlog)"},
    {"Developer", DiagRow(11), "Performance timers (perftimer)"},
    {"Developer", DiagRow(12), "Re-upload vertex arrays (arrayup)"},
    {"Developer", DiagRow(13), "Guest profiler (profile)"},
    {"Developer", RowId::kFlushLogs, "Flush logs"},
    {"Developer", RowId::kMarkLog, "Mark log"},
};

// The dk_diag.txt keys, in the spec's table order; kDiagFirst + n indexes it.
const char* const kDiagKeys[] = {
    "depth_off", "cull_off", "magenta", "picdump", "efbclear", "bufdump", "texdump",
    "shaderdump", "nomerge", "uvlog", "mtxlog", "perftimer", "arrayup", "profile",
};

bool IsDiag(RowId id) { return id >= RowId::kDiagFirst && id <= RowId::kDiagLast; }

bool IsSlider(RowId id) {
    return id == RowId::kMasterVolume || id == RowId::kMusicVolume || id == RowId::kSfxVolume ||
           id == RowId::kUiVolume || id == RowId::kVoicesVolume;
}

bool IsAction(RowId id) { return id == RowId::kFlushLogs || id == RowId::kMarkLog; }

bool DevGateOn() { return RuntimeConfigFile::ShowDeveloperOptions(false); }

bool RowVisible(const RowDef& row) {
    if (row.id == RowId::kDevGate) return true;
    if (IsDiag(row.id) || IsAction(row.id)) return DevGateOn();
    return true;
}

int VisibleRowCount() {
    int count = 0;
    for (const RowDef& row : kRows) {
        if (RowVisible(row)) ++count;
    }
    return count;
}

const RowDef& VisibleRowAt(int index) {
    for (const RowDef& row : kRows) {
        if (!RowVisible(row)) continue;
        if (index == 0) return row;
        --index;
    }
    return kRows[0];  // unreachable: callers clamp the index to the count
}

// ---------------------------------------------------------------------------
// Row state: read live from the backing getter, every time.

int VolumePercent(RowId id) {
    float value = 1.0f;
    switch (id) {
        case RowId::kMasterVolume: value = RuntimeConfigFile::AudioVolume(1.0f); break;
        case RowId::kMusicVolume: value = RuntimeConfigFile::MusicVolume(1.0f); break;
        case RowId::kSfxVolume: value = RuntimeConfigFile::SoundEffectsVolume(1.0f); break;
        case RowId::kUiVolume: value = RuntimeConfigFile::UiVolume(1.0f); break;
        case RowId::kVoicesVolume: value = RuntimeConfigFile::VoicesVolume(1.0f); break;
        default: break;
    }
    const int percent = static_cast<int>(std::lround(value * 100.0f));
    return percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

void SetVolumePercent(RowId id, int percent) {
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    const float value = static_cast<float>(percent) / 100.0f;
    // The apply + persist pairs are the PC Audio menu handlers' own calls.
    switch (id) {
        case RowId::kMasterVolume:
            AudioBackend::Instance().SetMasterVolume(value);
            RuntimeConfigFile::SetAudioVolume(value);
            break;
        case RowId::kMusicVolume:
            MusicAttenuation::SetMusicVolume(value);
            RuntimeConfigFile::SetMusicVolume(value);
            break;
        case RowId::kSfxVolume:
            MusicAttenuation::SetSoundEffectsVolume(value);
            RuntimeConfigFile::SetSoundEffectsVolume(value);
            break;
        case RowId::kUiVolume:
            MusicAttenuation::SetUiVolume(value);
            RuntimeConfigFile::SetUiVolume(value);
            break;
        case RowId::kVoicesVolume:
            MusicAttenuation::SetVoicesVolume(value);
            RuntimeConfigFile::SetVoicesVolume(value);
            break;
        default: break;
    }
}

bool ToggleState(RowId id) {
    switch (id) {
        case RowId::kVsync: return settings_overlay::VSync();
        case RowId::kBloom:
            // Bloom is bit 0x10 of the disable mask; the row is ON when the
            // bit is CLEAR. (Inverting this is the spec's named trap.)
            return (RuntimeConfigFile::DisabledPostProcessingPaths(0) & 0x10u) == 0;
        case RowId::kCopyFilter: return RuntimeConfigFile::DisableCopyFilter(true);
        case RowId::kSkipDraws: return RuntimeConfigFile::SkipUnreadyPipelines(true);
        case RowId::kShowFps: return settings_overlay::ShowFps();
        case RowId::kFrameGraph: return settings_overlay::FrameGraph();
        case RowId::kMute: return RuntimeConfigFile::AudioMuted(false);
        case RowId::kMixWorker: return RuntimeConfigFile::AudioMixWorkerEnabled(true);
        case RowId::kRumble: return RuntimeConfigFile::RumbleEnabled(true);
        case RowId::kSpinningCoins: return settings_overlay::SpinningCoins();
        case RowId::kDevGate: return DevGateOn();
        default:
            if (IsDiag(id)) return swdk::diag_get(kDiagKeys[static_cast<int>(id) - static_cast<int>(RowId::kDiagFirst)]);
            return false;
    }
}

// Persist the full diag state after a Developer flip, in the line format
// diag_init parses (a line naming the key, "=0" for off, anything else on).
void WriteDiagFileBack() {
    FILE* file = std::fopen("/switch/spm-nx/UserData/dk_diag.txt", "w");
    if (file == nullptr) {
        std::fprintf(stderr, "[diag] menu: could not write back dk_diag.txt\n");
        return;
    }
    for (const char* key : kDiagKeys) {
        std::fprintf(file, "%s=%d\n", key, swdk::diag_get(key) ? 1 : 0);
    }
    std::fclose(file);
}

void FlipDiag(RowId id) {
    const char* key = kDiagKeys[static_cast<int>(id) - static_cast<int>(RowId::kDiagFirst)];
    const bool on = !swdk::diag_get(key);
    if (!swdk::diag_set(key, on)) return;
    // diag_cmd_poll's marker format (device.cpp): diag_set does not log, so
    // the menu emits the marker itself, then persists the file like the file
    // had been edited by hand.
    std::fprintf(stderr, "[diag] set %s=%s at present %llu\n", key, on ? "1" : "0",
                 static_cast<unsigned long long>(swdk::diag_present_count()));
    swdk::log_flush();
    WriteDiagFileBack();
}

void FlipToggle(RowId id) {
    const bool on = !ToggleState(id);
    switch (id) {
        case RowId::kVsync: settings_overlay::SetVSync(on); break;
        case RowId::kBloom: {
            const uint32_t paths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
            const uint32_t next = on ? (paths & ~0x10u) : (paths | 0x10u);
            RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(next);
            RuntimeConfigFile::SetDisabledPostProcessingPaths(next);
            break;
        }
        case RowId::kCopyFilter:
            aurora_set_disable_copy_filter(on);
            RuntimeConfigFile::SetDisableCopyFilter(on);
            break;
        case RowId::kSkipDraws:
            aurora_set_skip_unready_pipelines(on);
            RuntimeConfigFile::SetSkipUnreadyPipelines(on);
            break;
        case RowId::kShowFps: settings_overlay::SetShowFps(on); break;
        case RowId::kFrameGraph: settings_overlay::SetFrameGraph(on); break;
        case RowId::kMute:
            AudioBackend::Instance().SetMuted(on);
            RuntimeConfigFile::SetAudioMuted(on);
            break;
        case RowId::kMixWorker:
            AxDspHle::SetMixWorkerEnabled(on);
            RuntimeConfigFile::SetAudioMixWorker(on);
            break;
        case RowId::kRumble:
            PAD_HLE_SetRumbleEnabled(on);
            RuntimeConfigFile::SetRumbleEnabled(on);
            break;
        case RowId::kSpinningCoins: settings_overlay::SetSpinningCoins(on); break;
        case RowId::kDevGate: RuntimeConfigFile::SetShowDeveloperOptions(on); break;
        default:
            if (IsDiag(id)) FlipDiag(id);
            break;
    }
}

// ---------------------------------------------------------------------------
// Transient status line (Flush / Mark feedback), shown for a few seconds.

std::string g_status;
double g_statusUntil = 0.0;

void SetStatus(const char* text) {
    g_status = text;
    g_statusUntil = ImGui::GetTime() + 3.0;
}

// ---------------------------------------------------------------------------
// Actions

void FireAction(RowId id) {
    if (id == RowId::kFlushLogs) {
        swdk::diag_flush_all("menu");
        SetStatus("Logs flushed");
    } else if (id == RowId::kMarkLog) {
        // diag_cmd_poll's mark, verbatim format: the line, then the flush.
        std::fprintf(stderr, "[diag] mark \"%s\" at present %llu\n", "menu",
                     static_cast<unsigned long long>(swdk::diag_present_count()));
        swdk::diag_flush_all("mark");
        SetStatus("Mark written to the log");
    }
}

// Resolution cycles the Switch subset only (the full PC table goes to 8x,
// which the console's frame budget cannot feed). From a value outside the
// subset (an older config's 2.25x, say), cycling starts at 1x.
const float kResolutionSubset[] = {1.0f, 1.5f, 0.0f};

void CycleResolution(int dir) {
    const float current = settings_overlay::RenderScale();
    int index = -1;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(current - kResolutionSubset[i]) < 0.01f) index = i;
    }
    const float next = index < 0 ? 1.0f : kResolutionSubset[(index + dir + 3) % 3];
    settings_overlay::SetRenderScale(next);
}

const char* const kAspectLabels[] = {"Original 4:3", "16:9", "Fill Window", "Cinematic 2.39:1"};

void CycleAspect(int dir) {
    const int32_t current = settings_overlay::Aspect();
    const int32_t next = (current + dir + 4) % 4;
    settings_overlay::SetAspect(next);
}

void ActivateRow(RowId id) {
    if (IsAction(id)) {
        FireAction(id);
    } else if (id == RowId::kResolution) {
        CycleResolution(1);
    } else if (id == RowId::kAspect) {
        CycleAspect(1);
    } else if (IsSlider(id)) {
        SetVolumePercent(id, VolumePercent(id) + 10);
    } else {
        FlipToggle(id);
    }
}

void AdjustRow(RowId id, int dir) {
    if (id == RowId::kResolution) {
        CycleResolution(dir);
    } else if (id == RowId::kAspect) {
        CycleAspect(dir);
    } else if (IsSlider(id)) {
        SetVolumePercent(id, VolumePercent(id) + 2 * dir);
    } else if (!IsAction(id)) {
        FlipToggle(id);
    }
}

std::string ValueText(RowId id) {
    if (id == RowId::kResolution) {
        const float scale = settings_overlay::RenderScale();
        if (scale == 0.0f) return "Auto";
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%gx", static_cast<double>(scale));
        return buffer;
    }
    if (id == RowId::kAspect) {
        const int32_t aspect = settings_overlay::Aspect();
        return (aspect >= 0 && aspect < 4) ? kAspectLabels[aspect] : "?";
    }
    if (IsSlider(id)) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%d%%", VolumePercent(id));
        return buffer;
    }
    if (IsAction(id)) return "";
    return ToggleState(id) ? "On" : "Off";
}

// ---------------------------------------------------------------------------
// Pad polling: one sample per frame, previous sample for edges, held-repeat
// for directions and adjustments. The shim maps buttons by label: Switch A
// is SDL "south" (confirm), Switch B is SDL "east" (back).

struct Sample {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool confirm = false;
    bool back = false;
};

Sample g_prev;
bool g_stickUp = false;
bool g_stickDown = false;
bool g_stickLeft = false;
bool g_stickRight = false;

struct RepeatState {
    bool down = false;
    double since = 0.0;
    double fired = 0.0;
};
RepeatState g_repeat[4];

SDL_Gamepad* AcquirePad() {
    static SDL_Gamepad* pad = nullptr;
    if (pad != nullptr) return pad;
    // The runtime's own PAD layer handle for player 1, the same one
    // settings_overlay's ReleaseControllers releases through.
    const int32_t index = PADGetIndexForPort(0);
    if (index >= 0) pad = PADGetSDLGamepadForIndex(static_cast<uint32_t>(index));
    return pad;
}

bool StickEngage(Sint16 value, bool negative, bool was) {
    // SDL stick Y is down-positive. Engage past 17000, release inside 10500:
    // the pump's old navigation filter used the same hysteresis shape.
    const int v = negative ? -static_cast<int>(value) : static_cast<int>(value);
    return was ? v > 10500 : v > 17000;
}

Sample ReadSample(SDL_Gamepad* pad) {
    Sample sample;
    if (pad == nullptr) return sample;
    sample.confirm = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    sample.back = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
    sample.up = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    sample.down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    sample.left = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    sample.right = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    const Sint16 x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    const Sint16 y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    g_stickUp = StickEngage(y, true, g_stickUp);
    g_stickDown = StickEngage(y, false, g_stickDown);
    g_stickLeft = StickEngage(x, true, g_stickLeft);
    g_stickRight = StickEngage(x, false, g_stickRight);
    sample.up = sample.up || g_stickUp;
    sample.down = sample.down || g_stickDown;
    sample.left = sample.left || g_stickLeft;
    sample.right = sample.right || g_stickRight;
    return sample;
}

// True on the press edge, then again while held: first repeat after 0.40 s,
// then every 0.08 s.
bool DirectionFired(int dir, bool down, double now) {
    RepeatState& state = g_repeat[dir];
    if (!down) {
        state.down = false;
        return false;
    }
    if (!state.down) {
        state.down = true;
        state.since = now;
        state.fired = now;
        return true;
    }
    if (now - state.since >= 0.40 && now - state.fired >= 0.08) {
        state.fired = now;
        return true;
    }
    return false;
}

void StepInput() {
    const int count = VisibleRowCount();
    if (count == 0) return;
    if (g_selected >= count) g_selected = count - 1;
    if (g_selected < 0) g_selected = 0;

    const Sample sample = ReadSample(AcquirePad());
    const double now = ImGui::GetTime();
    const bool up = DirectionFired(0, sample.up, now);
    const bool down = DirectionFired(1, sample.down, now);
    const bool left = DirectionFired(2, sample.left, now);
    const bool right = DirectionFired(3, sample.right, now);
    const bool confirm = sample.confirm && !g_prev.confirm;
    const bool back = sample.back && !g_prev.back;
    g_prev = sample;

    if (up) g_selected = (g_selected + count - 1) % count;
    if (down) g_selected = (g_selected + 1) % count;
    const RowId id = VisibleRowAt(g_selected).id;
    if (left) AdjustRow(id, -1);
    if (right) AdjustRow(id, 1);
    if (confirm) ActivateRow(id);
    if (back) SetVisible(false);
}

// ---------------------------------------------------------------------------
// The overlay-facing interface.

bool Visible() { return g_visible; }

void SetVisible(bool visible) {
    g_visible = visible;
    // Re-baseline input on the pad's current state: whatever is held as the
    // menu opens or closes counts as already down, so nothing acts on it
    // until a fresh press (a held direction starts its repeat delay over).
    g_stickUp = g_stickDown = g_stickLeft = g_stickRight = false;
    const Sample sample = ReadSample(AcquirePad());
    g_prev = sample;
    const double now = ImGui::GetTime();
    const bool dirs[4] = {sample.up, sample.down, sample.left, sample.right};
    for (int i = 0; i < 4; ++i) {
        g_repeat[i].down = dirs[i];
        g_repeat[i].since = now;
        g_repeat[i].fired = now;
    }
}

void Draw() {
    if (!g_visible) return;
    // Under the startup card the menu neither draws nor polls: the card is
    // full-screen, input is blocked, and its strap A-press must not double
    // as a menu activation. Visibility survives; the menu appears once the
    // card dismisses.
    if (settings_overlay::StartupScreenVisible()) return;

    StepInput();
    if (!g_visible) return;  // B closed it this frame

    const int count = VisibleRowCount();
    if (g_selected >= count) g_selected = count - 1;
    if (g_selected < 0) g_selected = 0;

    // The same full-screen dim the top bar drew.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 110));
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    // NoNav: this menu is driven by its own pad polling above; ImGui's
    // navigation must never move anything here.
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Switch Settings", nullptr, kFlags)) {
        ImGui::Text("New Super Paper Bros.");
        ImGui::TextDisabled("Switch settings");
        const char* section = nullptr;
        int index = 0;
        for (const RowDef& row : kRows) {
            if (!RowVisible(row)) continue;
            if (section == nullptr || std::strcmp(section, row.section) != 0) {
                section = row.section;
                ImGui::SeparatorText(section);
                if (std::strcmp(section, "Developer") == 0 && DevGateOn()) {
                    ImGui::TextDisabled("Diagnostics can hurt performance or distort the picture.");
                }
            }
            ImGui::PushID(static_cast<int>(row.id));
            const std::string value = ValueText(row.id);
            const std::string text = value.empty() ? std::string(row.label) : (std::string(row.label) + "   " + value);
            ImGui::Selectable(text.c_str(), index == g_selected, ImGuiSelectableFlags_None);
            ImGui::PopID();
            if (row.id == RowId::kAspect &&
                (settings_overlay::Aspect() != DisplaySettings::kAspect4x3) != DisplaySettings::GameWidescreen()) {
                ImGui::TextDisabled("Switching between 4:3 and widescreen takes effect after a restart");
            }
            ++index;
        }
        if (!g_status.empty() && ImGui::GetTime() < g_statusUntil) {
            ImGui::Separator();
            ImGui::Text("%s", g_status.c_str());
        }
        ImGui::Separator();
        ImGui::TextDisabled("[A] Select   [Left/Right] Adjust   [B] Close");
    }
    ImGui::End();
}

}  // namespace spm_switch_menu
