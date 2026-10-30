// The Switch settings menu (spm_switch_menu.h).
//
// The framework of this menu (its window, tab bar, focus and gamepad
// navigation) is adapted from SwitchWakerHD's settings overlay by centollOS
// (https://github.com/centollOS/SwitchWakerHD, commit b2e51df), which builds
// on the ZeldaWWHDRecomp overlay: a centred settings panel with a tab bar,
// each tab a scrolling page of ordinary ImGui widgets, the gamepad polled
// once per frame and fed to Dear ImGui's own gamepad navigation
// (io.AddKeyAnalogEvent + ImGuiConfigFlags_NavEnableGamepad), L / R
// switching tabs and B closing, through the overlay's edge-detected presses.
// Taken over, with credit and under MPL-2.0 (see THIRD_PARTY.md): the
// gamepad feed (their gamepad_nav.h), the settings window's shape and tab
// machinery (their overlay.cpp settings_window), the widget helpers and
// tooltip help (their widgets.h / tooltip.h), the style (their
// setup_style: dark sea-blue palette, gold navigation cursor), and the
// Controls tab's press-to-bind capture (their binding_capture.h, with its
// wait-for-release latch and the guards around it in their overlay.cpp).
// The rows, getters, apply calls and diagnostic plumbing below the
// framework are this port's own, as they were before the framework arrived.
//
// Adaptations at this port's seams, and nothing else:
// - Input values are read from the runtime's PAD HLE gamepad handle (the
//   SDL getters the game itself reads) once per frame inside Draw, where
//   Waker's SDL host polls its own input pump. The feed runs after Aurora's
//   NewFrame in this stack, so navigation consumes it on the next frame.
// - The trace hooks and the mouse-ownership handoff in their gamepad feed
//   have no host here (no trace sink, no mouse), so the controller is
//   simply always the navigation owner.
// - Their style is applied at start-up to their own ImGui context; this
//   context is shared with the runtime overlay, so the same values are
//   pushed around this window each frame and popped after it.
// - Their font (a 17 px system font) stays the host's; the panel geometry
//   and widget shapes are unchanged, only the glyphs are the overlay's.
// - The face buttons feed by label, as theirs do: the shim maps the pad by
//   label, so slot A is SDL south (confirm) and slot B is SDL east (back),
//   the mapping this menu has always used.
// - Open and close stay this port's seams: the overlay's hold-Minus / F10
//   handling calls SetVisible, and the guest pad block follows Visible().
//
// Rows are a view over live state, never a copy: every row reads its
// backing getter when it draws and writes through the same apply calls the
// PC handlers use when it changes. Rows the overlay exports accessors for
// go through settings_overlay.h; the rest read RuntimeConfigFile (the
// overlay persists every change there immediately) as before.

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
#include "virtual_remote.h"

#include <algorithm>
#include <array>
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
bool g_justOpened = false;  // the next drawn frame claims navigation focus
int g_tab = 0;              // the open tab; kept between opens, as theirs is
int g_selectTab = -1;       // a tab the shoulders asked for, consumed by the tab bar

// ---------------------------------------------------------------------------
// Rows

enum class RowId : int32_t {
    kResolution,
    kFrameInterp,
    kAspect,
    kVsync,
    kBloom,
    kCopyFilter,
    kSkipDraws,
    kTexReplacements,
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
    kTexDumps,
    kClearShaderCache,
};

struct RowDef {
    const char* section;
    RowId id;
    const char* label;
    const char* description;
};

// Description budget: a tooltip wraps at the tooltip window's own width, so
// the old two-line description-area budget no longer binds; the texts are
// kept as written for the description build (they are the row help now).
constexpr RowId DiagRow(int n) {
    return static_cast<RowId>(static_cast<int32_t>(RowId::kDiagFirst) + n);
}

// Section order: Graphics, Display, Audio, Game, Controls, Developer.
constexpr RowDef kRows[] = {
    {"Graphics", RowId::kResolution, "Resolution", "Scales the rendered picture from the game's 480-line base. Auto matches the screen size; lower values reduce renderer work."},
    {"Graphics", RowId::kFrameInterp, "Frame interpolation", "Presents extra frames between the game's own frames. On the 60 Hz screen it mainly duplicates frames and costs worker time. Off is recommended."},
    {"Graphics", RowId::kAspect, "Aspect", "Chooses the picture shape: the game's original 4:3, 16:9, filling the screen, or cinematic 2.39:1."},
    {"Graphics", RowId::kVsync, "Vsync", "Waits for the display refresh before showing a finished frame. This prevents tearing, but a missed refresh waits for the next one."},
    {"Graphics", RowId::kBloom, "Bloom", "Enables the game's bloom post-processing. Bloom is the only post-processing path this build can switch off."},
    {"Graphics", RowId::kCopyFilter, "Disable copy filter", "When on, EFB copies bypass the game's copy filter. Their filtering can look different from the game's intended result."},
    {"Graphics", RowId::kSkipDraws, "Skip draws while shaders compile", "When a shader is still compiling, skip its draws instead of pausing to wait. This avoids compile hitches, but some effects may be missing briefly."},
    {"Graphics", RowId::kTexReplacements, "Texture replacements", "Enables replacement textures, including button icons, prompt pictures, the Controls panel, and installed texture packs. Applies after a restart."},
    {"Display", RowId::kShowFps, "Show FPS", "Shows the presented frame rate over the game. If interpolation makes display and motion rates differ, it shows the motion rate too."},
    {"Display", RowId::kFrameGraph, "Frame graph", "Adds a rolling graph of the last 180 frame times to the FPS overlay, so spikes and uneven pacing are easy to see."},
    {"Audio", RowId::kMasterVolume, "Master volume", "Controls the overall loudness of all game audio."},
    {"Audio", RowId::kMusicVolume, "Music volume", "Controls the loudness of the game's music."},
    {"Audio", RowId::kSfxVolume, "Sound effects volume", "Controls the loudness of sound effects."},
    {"Audio", RowId::kUiVolume, "UI volume", "Controls the loudness of menu and interface sounds."},
    {"Audio", RowId::kVoicesVolume, "Voices volume", "Controls the loudness of character voices."},
    {"Audio", RowId::kMute, "Mute", "Silences all game audio without changing any volume slider."},
    {"Audio", RowId::kMixWorker, "Audio mix worker", "Runs the AX/DSP voice mix on a worker thread instead of inline. Turn it off if you suspect an audio problem."},
    {"Game", RowId::kRumble, "Controller vibration", "Allows the game to vibrate the controller for rumble effects."},
    {"Game", RowId::kSpinningCoins, "Spinning coins", "Enables Aurora's animated tile texture patches, including spinning coins and ? blocks. Turn it off if a patched tile looks wrong."},
    {"Developer", RowId::kDevGate, "Show developer options", "Shows the Developer rows in this menu. They expose renderer diagnostics for test runs and bug reports, and this choice is saved."},
    {"Developer", DiagRow(0), "Depth test off (depth_off)", "Forces depth and stencil tests and depth writes off for every pipeline. Geometry can show through other geometry; for isolating depth problems."},
    {"Developer", DiagRow(1), "Face culling off (cull_off)", "Forces back-face culling off for every pipeline, so faces that would normally be hidden are drawn. For checking geometry that vanishes."},
    {"Developer", DiagRow(2), "Magenta unbound textures (magenta)", "Clears the screen magenta before the finished picture is copied over it. Magenta left visible marks pixels the copy did not cover."},
    {"Developer", DiagRow(3), "Dump frame pictures (picdump)", "At a few presents, reads the picture back and logs sampled brightness, quadrant averages, and the centre pixel. It does not save an image."},
    {"Developer", DiagRow(4), "Clear the EFB each pass (efbclear)", "Clears colour to magenta on EFB passes that have a depth target. If the result reaches the screen, those passes are reaching the picture."},
    {"Developer", DiagRow(5), "Dump staged buffers (bufdump)", "At presents 600, 1800, and 3600, logs each live buffer's size, usage, and non-zero words. It waits for the GPU and can hitch."},
    {"Developer", DiagRow(6), "Dump textures (texdump)", "At a few presents, saves the most-sampled textures and every mip level under UserData/Logs, for comparison with what appeared on screen."},
    {"Developer", DiagRow(7), "Dump shaders (shaderdump)", "Saves each newly compiled shader's WGSL and translated GLSL side by side under its cache key, so the code behind a suspect draw can be read."},
    {"Developer", DiagRow(8), "No draw merging (nomerge)", "Stops Aurora from merging compatible draws into one. This can slow rendering, but isolates bugs caused by merging."},
    {"Developer", DiagRow(9), "UV trace (uvlog)", "Logs staged textures whose texture-coordinate scale and size disagree. For tracing textures that sample the wrong place."},
    {"Developer", DiagRow(10), "Matrix trace (mtxlog)", "Logs the texture matrices Aurora stages for each draw. For tracing draws that transform or sample textures incorrectly; output can be large."},
    {"Developer", DiagRow(11), "Performance timers (perftimer)", "Every 600 frames, prints producer, map-wait, encode, and total frame times to the log. Use it to measure where frame time goes."},
    {"Developer", DiagRow(12), "Re-upload vertex arrays (arrayup)", "Uploads every draw's vertex arrays again instead of trusting Aurora's cached copies. Slower; it tests for stale vertex data."},
    {"Developer", DiagRow(13), "Guest profiler (profile)", "Samples the guest thread 200 times a second while on. Turning it off prints the most-sampled guest functions to the log."},
    {"Developer", RowId::kFlushLogs, "Flush logs", "Writes buffered diagnostic output to stderr.log now. If performance timers are on, it also prints their partial 600-frame window."},
    {"Developer", RowId::kMarkLog, "Mark log", "Writes a 'menu' marker at the current frame and flushes the log, so a moment in a test run is easy to find later."},
    {"Developer", RowId::kTexDumps, "Texture dumps", "Saves textures with no replacement to Cache/texture_dumps under the names a replacement would use, for making new pictures. Applies after a restart."},
    {"Developer", RowId::kClearShaderCache, "Clear shader cache", "Requests deletion of the compiled shader cache at the next launch. Use it for a cache that has gone wrong; shaders then compile again as needed."},
};

// The tab bar's sections, in bar order; kRows groups its rows under these
// names.
const char* const kSections[] = {"Graphics", "Display", "Audio", "Game", "Controls", "Developer"};
constexpr int kSectionCount = 6;
constexpr int kControlsSection = 4;
constexpr int kDeveloperSection = 5;

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

bool IsAction(RowId id) {
    return id == RowId::kFlushLogs || id == RowId::kMarkLog || id == RowId::kClearShaderCache;
}

bool IsCycle(RowId id) {
    return id == RowId::kResolution || id == RowId::kFrameInterp || id == RowId::kAspect;
}

bool DevGateOn() { return RuntimeConfigFile::ShowDeveloperOptions(false); }

bool RowVisible(const RowDef& row) {
    if (row.id == RowId::kDevGate) return true;
    if (IsDiag(row.id) || IsAction(row.id) || row.id == RowId::kTexDumps) return DevGateOn();
    return true;
}

int SectionRowCount(int section) {
    int count = 0;
    for (const RowDef& row : kRows) {
        if (std::strcmp(row.section, kSections[section]) == 0 && RowVisible(row)) ++count;
    }
    return count;
}

const RowDef& SectionRowAt(int section, int index) {
    for (const RowDef& row : kRows) {
        if (std::strcmp(row.section, kSections[section]) != 0 || !RowVisible(row)) continue;
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
        case RowId::kTexReplacements: return RuntimeConfigFile::TextureReplacements(true);
        case RowId::kTexDumps: return RuntimeConfigFile::TextureDumps(false);
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

void SetDiag(RowId id, bool on) {
    const char* key = kDiagKeys[static_cast<int>(id) - static_cast<int>(RowId::kDiagFirst)];
    if (!swdk::diag_set(key, on)) return;
    // diag_cmd_poll's marker format (device.cpp): diag_set does not log, so
    // the menu emits the marker itself, then persists the file like the file
    // had been edited by hand.
    std::fprintf(stderr, "[diag] set %s=%s at present %llu\n", key, on ? "1" : "0",
                 static_cast<unsigned long long>(swdk::diag_present_count()));
    swdk::log_flush();
    WriteDiagFileBack();
}

void ApplyToggle(RowId id, bool on) {
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
        case RowId::kTexReplacements:
            // No Set* wrapper exists for this key (the PC launcher owns it
            // there), so the row does what a setter does: update the live
            // config, then write the file through WriteSetting. The
            // replacement index itself is built once at startup, so the
            // flip only takes effect on the next launch (the row says so).
            RuntimeConfigFile::Mutable().textureReplacements = on;
            RuntimeConfigFile::WriteSetting("video", "texture_replacements", on ? "true" : "false");
            break;
        case RowId::kTexDumps:
            // Same shape as Texture replacements above; main.cpp reads the
            // pair once while Aurora initializes.
            RuntimeConfigFile::Mutable().textureDumps = on;
            RuntimeConfigFile::WriteSetting("video", "texture_dumps", on ? "true" : "false");
            break;
        case RowId::kDevGate: RuntimeConfigFile::SetShowDeveloperOptions(on); break;
        default:
            if (IsDiag(id)) SetDiag(id, on);
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
    } else if (id == RowId::kClearShaderCache) {
        // The cache cannot be deleted from inside a running session:
        // shaders.cpp appends a record for every shader compiled, all
        // session long, so the next compile would recreate the file (and
        // recreate it headerless, to be discarded at load anyway). Leave
        // the sentinel instead (path must match kClearPath in
        // switch/deko/shaders.cpp); shaders_init deletes cache + sentinel
        // before the cache is read on the next launch.
        FILE* file = std::fopen("/switch/spm-nx/UserData/dksh_cache.clear", "w");
        if (file != nullptr) {
            std::fclose(file);
            SetStatus("Shader cache will be cleared on next launch");
        } else {
            std::fprintf(stderr, "[diag] menu: could not write dksh_cache.clear\n");
            SetStatus("Could not request the shader cache clear");
        }
    }
}

// The cycle rows' option sets. Resolution cycles the Switch subset only
// (the full PC table goes to 8x, which the console's frame budget cannot
// feed); frame interpolation the Switch subset of Aurora's targets (240
// needs a 240 Hz panel this console does not have).
const float kResolutionSubset[] = {0.75f, 1.0f, 1.5f, 0.0f};
const char* const kResolutionLabels[] = {"0.75x", "1x", "1.5x", "Auto"};
const uint32_t kFrameInterpSubset[] = {0, 120, 180};
const char* const kFrameInterpLabels[] = {"Off", "120", "180"};
const char* const kAspectLabels[] = {"Original 4:3", "16:9", "Fill Window", "Cinematic 2.39:1"};

// Index of the current value in the subset, or -1 when the config holds a
// value outside it (an older config's 2.25x, a PC-written 240): the combo
// then shows the value itself as its preview and checks no item.
int ResolutionIndex() {
    const float current = settings_overlay::RenderScale();
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(current - kResolutionSubset[i]) < 0.01f) return i;
    }
    return -1;
}
int FrameInterpIndex() {
    const uint32_t current = RuntimeConfigFile::FrameInterpolationFps(0);
    for (int i = 0; i < 3; ++i) {
        if (current == kFrameInterpSubset[i]) return i;
    }
    return -1;
}

void SetResolutionValue(float scale) { settings_overlay::SetRenderScale(scale); }
void SetFrameInterpValue(uint32_t fps) {
    // The PC combo's apply pair (settings_overlay.cpp): the Aurora call
    // retargets the running frame pacing, the config write persists it.
    aurora_set_frame_interpolation_fps(fps);
    RuntimeConfigFile::SetFrameInterpolationFps(fps);
}
void SetAspectValue(int32_t aspect) { settings_overlay::SetAspect(aspect); }

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
    if (id == RowId::kFrameInterp) {
        const uint32_t fps = RuntimeConfigFile::FrameInterpolationFps(0);
        if (fps == 0) return "Off";
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%u", fps);
        return buffer;
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
// The framework: gamepad feed, navigation and drawing, adapted from
// SwitchWakerHD's settings overlay (see the note at the top of this file).

// Pad slots, in SwitchWakerHD's input_map order: every device combined,
// analogue values 0..1 (a pressed button is 1).
enum PadSlot : int {
    kPadNone,
    kPadA, kPadB, kPadX, kPadY, kPadLB, kPadRB, kPadLT, kPadRT,
    kPadMenu, kPadOptions, kPadHome, kPadL3, kPadR3,
    kPadDUp, kPadDDown, kPadDLeft, kPadDRight,
    kPadLSUp, kPadLSDown, kPadLSLeft, kPadLSRight,
    kPadRSUp, kPadRSDown, kPadRSLeft, kPadRSRight,
    kPadCapture, kPadRightPaddle1, kPadLeftPaddle1, kPadRightPaddle2, kPadLeftPaddle2, kPadTouchpad,
    kPadMisc2, kPadMisc3, kPadMisc4, kPadMisc5, kPadMisc6,
    kPadCount
};
using PadValues = std::array<float, kPadCount>;

PadValues g_values{};
PadValues g_prev{};

SDL_Gamepad* AcquirePad() {
    static SDL_Gamepad* pad = nullptr;
    if (pad != nullptr) return pad;
    // The runtime's own PAD layer handle for player 1, the same one
    // settings_overlay's ReleaseControllers releases through.
    const int32_t index = PADGetIndexForPort(0);
    if (index >= 0) pad = PADGetSDLGamepadForIndex(static_cast<uint32_t>(index));
    return pad;
}

// One snapshot of the pad, in the framework's slot order. The face buttons
// fill by label (the shim maps buttons by label: Switch A is SDL "south",
// Switch B is SDL "east"); the stick slots carry the axis magnitude in
// their direction and the trigger slots the trigger axes' pull; the right
// stick is read for completeness of the slot map but nothing here
// navigates with it, as in the framework.
PadValues ReadPadValues() {
    PadValues values{};
    SDL_Gamepad* pad = AcquirePad();
    if (pad == nullptr) return values;
    auto button = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b) ? 1.0f : 0.0f; };
    values[kPadA] = button(SDL_GAMEPAD_BUTTON_SOUTH);
    values[kPadB] = button(SDL_GAMEPAD_BUTTON_EAST);
    values[kPadX] = button(SDL_GAMEPAD_BUTTON_WEST);
    values[kPadY] = button(SDL_GAMEPAD_BUTTON_NORTH);
    values[kPadLB] = button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    values[kPadRB] = button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    values[kPadMenu] = button(SDL_GAMEPAD_BUTTON_START);
    values[kPadOptions] = button(SDL_GAMEPAD_BUTTON_BACK);
    values[kPadDUp] = button(SDL_GAMEPAD_BUTTON_DPAD_UP);
    values[kPadDDown] = button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    values[kPadDLeft] = button(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    values[kPadDRight] = button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    auto axis = [&](SDL_GamepadAxis a) { return static_cast<float>(SDL_GetGamepadAxis(pad, a)) / 32767.0f; };
    // The triggers are axes in SDL3, not buttons. The framework's slots are
    // analogue 0..1 and its SDL host fills these two from the trigger axes,
    // clamped at rest, so the same here; the feed's thresholds do the rest.
    const float lt = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    const float rt = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    values[kPadLT] = lt > 0.0f ? lt : 0.0f;
    values[kPadRT] = rt > 0.0f ? rt : 0.0f;
    const float lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
    const float ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    values[kPadLSUp] = ly < 0.0f ? -ly : 0.0f;
    values[kPadLSDown] = ly > 0.0f ? ly : 0.0f;
    values[kPadLSLeft] = lx < 0.0f ? -lx : 0.0f;
    values[kPadLSRight] = lx > 0.0f ? lx : 0.0f;
    const float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    const float ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    values[kPadRSUp] = ry < 0.0f ? -ry : 0.0f;
    values[kPadRSDown] = ry > 0.0f ? ry : 0.0f;
    values[kPadRSLeft] = rx < 0.0f ? -rx : 0.0f;
    values[kPadRSRight] = rx > 0.0f ? rx : 0.0f;
    return values;
}

// The framework's gamepad navigation (their gamepad_nav.h): the pad is fed
// to Dear ImGui's gamepad navigation as analogue key events, and the
// navigation keys belong to the controller whenever it is enabled. Their
// mouse-ownership handoff and trace hooks are the parts with no host here:
// there is no mouse, so the controller is always the owner, and there is no
// trace sink. The face mapping is by label, as theirs: the values in slots
// A, B, X, Y are the pad's labelled buttons, and ImGui confirms with
// FaceDown (A) and goes back with FaceRight (B), as the game does.
class GamepadNavigation {
public:
    bool pressed(const float* values, const float* previous, int input) const {
        return values[input] > 0.5f && previous[input] <= 0.5f;
    }
    void feed(ImGuiIO& io, const float* values, const float* previous, bool enabled) const {
        constexpr int navigation[] = {kPadA, kPadB, kPadX, kPadY,
            kPadDUp, kPadDDown, kPadDLeft, kPadDRight,
            kPadLSUp, kPadLSDown, kPadLSLeft, kPadLSRight,
            kPadLT, kPadRT, kPadLB, kPadRB, kPadMenu};
        (void)navigation;  // the set the framework watches for ownership; ownership is constant here
        (void)previous;
        // Suspending gamepad navigation clears the flag, as theirs does
        // when the mouse owns the menu: the keys are then fed as zero.
        if (enabled) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        auto key = [&](ImGuiKey k, int p) {
            const float v = enabled && values[p] >= 0.35f ? values[p] : 0.0f;
            io.AddKeyAnalogEvent(k, v > 0.5f, v);
        };
        key(ImGuiKey_GamepadFaceDown, kPadA);
        key(ImGuiKey_GamepadFaceRight, kPadB);
        key(ImGuiKey_GamepadFaceLeft, kPadX);
        key(ImGuiKey_GamepadFaceUp, kPadY);
        key(ImGuiKey_GamepadDpadUp, kPadDUp);
        key(ImGuiKey_GamepadDpadDown, kPadDDown);
        key(ImGuiKey_GamepadDpadLeft, kPadDLeft);
        key(ImGuiKey_GamepadDpadRight, kPadDRight);
        key(ImGuiKey_GamepadLStickUp, kPadLSUp);
        key(ImGuiKey_GamepadLStickDown, kPadLSDown);
        key(ImGuiKey_GamepadLStickLeft, kPadLSLeft);
        key(ImGuiKey_GamepadLStickRight, kPadLSRight);
        // Right-stick movement is not overlay navigation.
        key(ImGuiKey_GamepadL2, kPadLT);
        key(ImGuiKey_GamepadR2, kPadRT);
        key(ImGuiKey_GamepadStart, kPadMenu);
    }
};

GamepadNavigation g_navigation;

// Binding capture state for the Controls page (defined with the page,
// below): the action and slot being bound (-1 when no capture runs), and
// the completion flag for the frame a capture ends on.
int g_capAction = -1;
int g_capSlot = -1;
bool g_captureCompleted = false;

// The framework's own edge-detected presses, for the inputs the window
// handles itself (the shoulders switch tabs, B closes): the same values
// and previous snapshot the feed consumed.
bool NavigationPressed(int slot) {
    // During a binding capture (and on its completion frame) the pad
    // belongs to the capture: no tab switches, and a press bound to B
    // does not also close the menu.
    return g_capAction < 0 && !g_captureCompleted &&
           g_navigation.pressed(g_values.data(), g_prev.data(), slot);
}

// Their setup_style, scoped to this window: SwitchWakerHD sets these once
// on its own ImGui context at start-up; this context is shared with the
// runtime overlay, so the same values are pushed around the settings
// window each frame and popped after it. StyleColorsDark is their base,
// then their metrics, then their deep-sea palette with the gold
// navigation cursor.
void PushMenuStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.07f, 0.12f, 0.975f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.13f, 0.20f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.05f, 0.11f, 0.18f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.62f, 0.70f, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.05f, 0.20f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.06f, 0.27f, 0.38f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.22f, 0.32f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.14f, 0.34f, 0.46f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.17f, 0.42f, 0.54f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.36f, 0.48f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.50f, 0.62f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.22f, 0.60f, 0.70f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.36f, 0.48f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.50f, 0.62f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.22f, 0.60f, 0.70f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.08f, 0.24f, 0.34f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.20f, 0.52f, 0.64f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.16f, 0.44f, 0.56f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.55f, 0.95f, 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.40f, 0.80f, 0.80f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.55f, 0.95f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.30f, 0.62f, 0.70f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4(0.08f, 0.24f, 0.34f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 1.0f, 1.0f, 0.03f));
}

void PopMenuStyle() {
    ImGui::PopStyleColor(25);
    ImGui::PopStyleVar(12);
}

// Their widget helpers (widgets.h / tooltip.h): a row's help is a tooltip
// of the item before it, and a note is a grey wrapped line of its own.
bool TooltipHovered(ImGuiHoveredFlags flags = 0) {
    return ImGui::GetIO().MouseSource != ImGuiMouseSource_TouchScreen && ImGui::IsItemHovered(flags);
}

void Help(const char* text) {
    if (TooltipHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", text);
}

void Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.78f, 0.84f, 1.0f));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Controls page: press-to-bind over the [gamepad] table, for the connected
// pad's style. The capture is SwitchWakerHD's BindingCapture (their
// binding_capture.h, MPL-2.0), ported against this menu's pad snapshot:
// wait until every input is released, then record the first input past
// 0.6. Their overlay guards travel with it, in Draw and NavigationPressed:
// the navigation feed is suspended while a capture is active, the
// edge-detected tab and close presses report nothing during a capture or
// on its completion frame, switching tabs cancels it, and a captured press
// is consumed by the binding, so pressing B to bind it does not also close
// the menu. Capture ends on a press, on an 8 second timeout, or when
// either stick is clicked (the explicit cancel; stick clicks are the one
// pad input outside the bindable set, so they are free for this).

class BindingCapture {
    bool released_ = false;

public:
    void reset() { released_ = false; }
    bool released() const { return released_; }
    int poll(const float* values) {
        bool any = false;
        int pressed = -1;
        for (int p = 1; p < kPadCount; ++p) {
            any |= values[p] > 0.3f;
            if (values[p] > 0.6f && pressed < 0) pressed = p;
        }
        if (!released_) {
            released_ = !any;
            return -1;
        }
        return pressed;
    }
};

double g_capStarted = 0.0;
BindingCapture g_bindingCapture;

std::string g_controlStatus;
double g_controlStatusUntil = 0.0;

void SetControlStatus(const std::string& text) {
    g_controlStatus = text;
    g_controlStatusUntil = ImGui::GetTime() + 3.0;
}

// The 12 actions, in Keybinds order (the order of the config keys and of
// VirtualRemote's defaults table).
const char* const kControlLabels[] = {
    "Up", "Down", "Left", "Right", "Jump", "Run", "A", "Shake", "Tilt left", "Tilt right", "Plus", "Minus",
};
const char* const kControlDescriptions[] = {
    "Moves up: menus, ladders and aiming where the game reads up.",
    "Moves down: menus, crouching and aiming where the game reads down.",
    "Moves left, in menus and in the world.",
    "Moves right, in menus and in the world.",
    "Jump. In menus it confirms, like the game's 2 button.",
    "Run and pick up or throw, the game's 1 button.",
    "Use Tippi and interact where the game reads the remote's A button.",
    "Shake the remote: spin jumps, charging and breaking free.",
    "Tilt the remote left, in the places the game reads a tilt.",
    "Tilt the remote right, in the places the game reads a tilt.",
    "Pause and menus, the game's + button.",
    "The game's - button. Hold it outside this menu to open these settings.",
};

std::vector<std::string> SplitBindingNames(const std::string& text) {
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        std::string name = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t first = name.find_first_not_of(" \t");
        const size_t last = name.find_last_not_of(" \t");
        name = first == std::string::npos ? "" : name.substr(first, last - first + 1);
        if (!name.empty()) names.push_back(name);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return names;
}

std::string JoinBindingNames(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        if (!joined.empty()) joined += ",";
        joined += name;
    }
    return joined;
}

// The inputs an action currently answers to: the config value when one is
// saved, the parser's built-in defaults when the key is absent.
std::vector<std::string> EffectiveBindingNames(int style, int action) {
    const char* styleName = VirtualRemote::PadStyleName(style);
    const char* actionKey = VirtualRemote::PadActionKey(action);
    if (styleName == nullptr || actionKey == nullptr) return {};
    const std::string custom = RuntimeConfigFile::GamepadBinding(std::string(styleName) + "_" + actionKey);
    const char* fallback = VirtualRemote::PadDefaultBinding(style, action);
    return SplitBindingNames(custom.empty() ? (fallback != nullptr ? fallback : "") : custom);
}

void WriteBindingNames(int style, int action, std::vector<std::string> names) {
    // One entry per input: binding the same input twice keeps the first.
    std::vector<std::string> unique;
    for (const std::string& name : names) {
        if (std::find(unique.begin(), unique.end(), name) == unique.end()) unique.push_back(name);
    }
    std::string joined = JoinBindingNames(unique);
    // An empty value in the config means "use the defaults", so it cannot
    // say "nothing bound". A lone comma parses back to zero inputs, which
    // is the one true unbound encoding.
    if (joined.empty()) joined = ",";
    const char* styleName = VirtualRemote::PadStyleName(style);
    const char* actionKey = VirtualRemote::PadActionKey(action);
    if (styleName == nullptr || actionKey == nullptr) return;
    RuntimeConfigFile::SetGamepadBinding(std::string(styleName) + "_" + actionKey, joined);
}

void BindControlSlot(int style, int action, int slot, const std::string& name) {
    std::vector<std::string> names = EffectiveBindingNames(style, action);
    if (slot < static_cast<int>(names.size())) {
        names[slot] = name;
    } else {
        names.push_back(name);  // slot 1 with a shorter list appends as the only entry
    }
    WriteBindingNames(style, action, names);
    VirtualRemote::ReloadPadLayouts();
}

void ClearControlSlot(int style, int action, int slot) {
    std::vector<std::string> names = EffectiveBindingNames(style, action);
    if (slot >= static_cast<int>(names.size())) return;
    names.erase(names.begin() + slot);
    WriteBindingNames(style, action, names);
    VirtualRemote::ReloadPadLayouts();
    SetControlStatus(std::string(kControlLabels[action]) + " slot cleared");
}

void ResetControls(int style) {
    for (int action = 0; action < 12; ++action) {
        const char* fallback = VirtualRemote::PadDefaultBinding(style, action);
        WriteBindingNames(style, action, SplitBindingNames(fallback != nullptr ? fallback : ""));
    }
    VirtualRemote::ReloadPadLayouts();
    SetControlStatus("Bindings reset to the defaults");
}

// Friendly names for config inputs, per style. On the Switch the SDL names
// read as the printed Nintendo letters (the shim maps buttons by label):
// a is A, b is B, x is X, y is Y, the shoulders are L and R, the triggers
// ZL and ZR, back is Minus and start is Plus.
std::string BindingLabel(const std::string& name, int style) {
    std::string base = name;
    char sign = 0;
    if (base.size() > 1 && (base.back() == '+' || base.back() == '-')) {
        sign = base.back();
        base.pop_back();
    }
    if (base == "leftx" || base == "lefty" || base == "rightx" || base == "righty") {
        const bool left = base[0] == 'l';
        const bool horizontal = base.back() == 'x';
        const char* direction = horizontal ? (sign == '-' ? "left" : "right") : (sign == '-' ? "up" : "down");
        return std::string(left ? "Left stick " : "Right stick ") + direction;
    }
    if (base == "lefttrigger") return style == 0 ? "LT" : (style == 1 ? "L2" : "ZL");
    if (base == "righttrigger") return style == 0 ? "RT" : (style == 1 ? "R2" : "ZR");
    if (base == "a") return style == 1 ? "Cross" : "A";
    if (base == "b") return style == 1 ? "Circle" : "B";
    if (base == "x") return style == 1 ? "Square" : "X";
    if (base == "y") return style == 1 ? "Triangle" : "Y";
    if (base == "back") return style == 0 ? "Back" : (style == 1 ? "Share" : "Minus");
    if (base == "start") return style == 0 ? "Menu" : (style == 1 ? "Options" : "Plus");
    if (base == "guide") return style == 2 ? "Home" : "Guide";
    if (base == "leftstick") return "Left stick click";
    if (base == "rightstick") return "Right stick click";
    if (base == "leftshoulder") return style == 0 ? "LB" : (style == 1 ? "L1" : "L");
    if (base == "rightshoulder") return style == 0 ? "RB" : (style == 1 ? "R1" : "R");
    if (base == "dpup") return "D-pad up";
    if (base == "dpdown") return "D-pad down";
    if (base == "dpleft") return "D-pad left";
    if (base == "dpright") return "D-pad right";
    if (base == "touchpad") return "Touchpad";
    return name;
}

// The config-grammar name for a captured pad slot, through SDL's own name
// tables where the grammar has one (buttons, triggers), and as an axis
// name with a direction suffix for the stick directions.
const char* PadSlotBindingName(int slot) {
    auto buttonName = [](SDL_GamepadButton button) { return SDL_GetGamepadStringForButton(button); };
    switch (slot) {
        case kPadA: return buttonName(SDL_GAMEPAD_BUTTON_SOUTH);
        case kPadB: return buttonName(SDL_GAMEPAD_BUTTON_EAST);
        case kPadX: return buttonName(SDL_GAMEPAD_BUTTON_WEST);
        case kPadY: return buttonName(SDL_GAMEPAD_BUTTON_NORTH);
        case kPadLB: return buttonName(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        case kPadRB: return buttonName(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        case kPadLT: return SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        case kPadRT: return SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        case kPadMenu: return buttonName(SDL_GAMEPAD_BUTTON_START);
        case kPadOptions: return buttonName(SDL_GAMEPAD_BUTTON_BACK);
        case kPadHome: return buttonName(SDL_GAMEPAD_BUTTON_GUIDE);
        case kPadDUp: return buttonName(SDL_GAMEPAD_BUTTON_DPAD_UP);
        case kPadDDown: return buttonName(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        case kPadDLeft: return buttonName(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        case kPadDRight: return buttonName(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        case kPadLSUp: return "lefty-";
        case kPadLSDown: return "lefty+";
        case kPadLSLeft: return "leftx-";
        case kPadLSRight: return "leftx+";
        case kPadRSUp: return "righty-";
        case kPadRSDown: return "righty+";
        case kPadRSLeft: return "rightx-";
        case kPadRSRight: return "rightx+";
        default: return nullptr;  // slots the pad snapshot never fills (Home aside, paddles, touchpad, misc)
    }
}

void StartCapture(int action, int slot) {
    if (g_capAction >= 0) return;
    g_capAction = action;
    g_capSlot = slot;
    g_capStarted = ImGui::GetTime();
    g_bindingCapture.reset();
}

void EndCapture() {
    g_captureCompleted = true;  // the completion frame's presses stay consumed
    g_capAction = -1;
    g_capSlot = -1;
}

void CancelCapture() {
    if (g_capAction < 0) return;
    EndCapture();
    SetControlStatus("Binding cancelled, nothing changed");
}

// One capture step, run at the top of Draw before the navigation feed, so
// a completing press never reaches the feed or the close handling below.
void ApplyCapture(int style) {
    if (g_capAction < 0) return;
    SDL_Gamepad* pad = AcquirePad();
    if (pad != nullptr && g_bindingCapture.released() &&
        (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK) ||
         SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK))) {
        CancelCapture();  // the explicit cancel: click either stick
        return;
    }
    const int pressed = g_bindingCapture.poll(g_values.data());
    if (pressed > 0) {
        if (const char* name = PadSlotBindingName(pressed)) {
            BindControlSlot(style, g_capAction, g_capSlot, name);
            SetControlStatus(std::string(kControlLabels[g_capAction]) + " is now bound to " +
                             BindingLabel(name, style));
        }
        EndCapture();
        return;
    }
    if (ImGui::GetTime() - g_capStarted > 8.0) {
        EndCapture();
        SetControlStatus("Binding timed out, nothing changed");
    }
}

// The style the Controls page edits: the connected pad's own style, the
// same choice the runtime makes for it. With no pad to ask, the page
// falls back to the Nintendo table.
int ControlsStyle(bool& detected) {
    SDL_Gamepad* pad = AcquirePad();
    detected = pad != nullptr;
    return detected ? VirtualRemote::PadStyleOf(pad) : 2;
}

void DrawControlsPage() {
    bool detected = false;
    const int style = ControlsStyle(detected);
    const char* styleName = VirtualRemote::PadStyleName(style);
    if (g_capAction >= 0) {
        Note("Press the new input now. Click either stick to cancel, or wait 8 seconds to give up. "
             "The binding applies in game as soon as it is set.");
    } else if (!g_controlStatus.empty() && ImGui::GetTime() < g_controlStatusUntil) {
        Note(g_controlStatus.c_str());
    } else if (detected) {
        const std::string note = std::string("Controller style: ") + (styleName != nullptr ? styleName : "") +
                                 ". Select a slot, then press the input to bind it to. Press X on a focused "
                                 "slot to clear it. Changes apply right away.";
        Note(note.c_str());
    } else {
        Note("No controller was detected, so the Nintendo bindings are shown. Select a slot, then press "
             "the input to bind it to. Press X on a focused slot to clear it. Changes apply right away.");
    }
    for (int action = 0; action < 12; ++action) {
        ImGui::PushID(action);
        const std::vector<std::string> names = EffectiveBindingNames(style, action);
        ImGui::Text("%s", kControlLabels[action]);
        for (int slot = 0; slot < 2; ++slot) {
            ImGui::SameLine(slot == 0 ? 190.0f : 430.0f);
            std::string label;
            if (g_capAction == action && g_capSlot == slot) {
                label = "...";
            } else if (slot < static_cast<int>(names.size())) {
                label = BindingLabel(names[slot], style);
            } else {
                label = "-";
            }
            label += slot == 0 ? "##primary" : "##secondary";
            const bool clicked = ImGui::Button(label.c_str(), ImVec2(230.0f, 0.0f));
            const bool focused = ImGui::IsItemFocused();
            if (clicked) {
                StartCapture(action, slot);
            } else if (focused && g_capAction < 0 && !g_captureCompleted &&
                       ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
                ClearControlSlot(style, action, slot);
            }
        }
        Help(kControlDescriptions[action]);
        ImGui::PopID();
    }
    if (ImGui::Button("Reset all to defaults")) {
        if (g_capAction < 0) ResetControls(style);
    }
    Help("Restores every binding above to the built-in defaults for this controller style.");
}

// ---------------------------------------------------------------------------
// Pages: one tab's rows, drawn inside the tab's scrolling child window.

void DrawCycleRow(const RowDef& row) {
    const std::string preview = ValueText(row.id);
    if (!ImGui::BeginCombo(row.label, preview.c_str())) return;
    if (row.id == RowId::kResolution) {
        const int current = ResolutionIndex();
        for (int i = 0; i < 4; ++i) {
            const bool selected = i == current;
            if (ImGui::Selectable(kResolutionLabels[i], selected)) SetResolutionValue(kResolutionSubset[i]);
            if (selected) ImGui::SetItemDefaultFocus();
        }
    } else if (row.id == RowId::kFrameInterp) {
        const int current = FrameInterpIndex();
        for (int i = 0; i < 3; ++i) {
            const bool selected = i == current;
            if (ImGui::Selectable(kFrameInterpLabels[i], selected)) SetFrameInterpValue(kFrameInterpSubset[i]);
            if (selected) ImGui::SetItemDefaultFocus();
        }
    } else {  // kAspect
        const int32_t current = settings_overlay::Aspect();
        for (int i = 0; i < 4; ++i) {
            const bool selected = i == current;
            if (ImGui::Selectable(kAspectLabels[i], selected)) SetAspectValue(i);
            if (selected) ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

void DrawRow(const RowDef& row) {
    ImGui::PushID(static_cast<int>(row.id));
    if (IsCycle(row.id)) {
        DrawCycleRow(row);
    } else if (IsSlider(row.id)) {
        int percent = VolumePercent(row.id);
        if (ImGui::SliderInt(row.label, &percent, 0, 100, "%d%%")) SetVolumePercent(row.id, percent);
    } else if (IsAction(row.id)) {
        if (ImGui::Button(row.label)) FireAction(row.id);
    } else {
        bool value = ToggleState(row.id);
        if (ImGui::Checkbox(row.label, &value)) ApplyToggle(row.id, value);
    }
    Help(row.description);
    ImGui::PopID();
    if (row.id == RowId::kAspect &&
        (settings_overlay::Aspect() != DisplaySettings::kAspect4x3) != DisplaySettings::GameWidescreen()) {
        Note("Switching between 4:3 and widescreen takes effect after a restart");
    }
    if (row.id == RowId::kDevGate && DevGateOn()) {
        Note("Diagnostics can hurt performance or distort the picture.");
    }
}

void DrawSectionPage(int section) {
    if (section == kControlsSection) {
        DrawControlsPage();
        return;
    }
    if (section == kDeveloperSection && !g_status.empty() && ImGui::GetTime() < g_statusUntil) {
        Note(g_status.c_str());
    }
    for (const RowDef& row : kRows) {
        if (std::strcmp(row.section, kSections[section]) != 0 || !RowVisible(row)) continue;
        DrawRow(row);
    }
}

// ---------------------------------------------------------------------------
// The overlay-facing interface.

bool Visible() { return g_visible; }

void SetVisible(bool visible) {
    g_visible = visible;
    if (visible) g_justOpened = true;
    if (!visible) g_capAction = -1;  // a capture never outlives the menu
    // Re-baseline input on the pad's current state: whatever is held as the
    // menu opens or closes counts as already down in both snapshots, so
    // nothing acts on it until a fresh press.
    const PadValues now = ReadPadValues();
    g_values = now;
    g_prev = now;
}

void Draw() {
    // The pad is read and fed every frame, open or not (the framework
    // reads its controller in its frame loop the same way): while the menu
    // is closed or the startup screen is up, the feed runs disabled, which
    // hands the navigation keys back and feeds them as zero.
    g_prev = g_values;
    g_values = ReadPadValues();
    ImGuiIO& io = ImGui::GetIO();
    const bool enabled = g_visible && !settings_overlay::StartupScreenVisible();
    // The capture runs before the feed: while it is active the feed is
    // suspended, and a press that completes it (B included) is consumed
    // here, so it reaches neither the navigation keys nor the close
    // handling at the bottom of this function.
    g_captureCompleted = false;
    const bool capturingAtStart = g_capAction >= 0;
    if (enabled && capturingAtStart) {
        bool detected = false;
        ApplyCapture(ControlsStyle(detected));
    }
    const bool feedEnabled = enabled && g_capAction < 0 && !capturingAtStart;
    g_navigation.feed(io, g_values.data(), g_prev.data(), feedEnabled);
    if (!enabled) return;

    PushMenuStyle();

    // Their settings window: a centred panel, sized to the display, with
    // the tab bar across the top and one scrolling page per tab.
    const ImVec2 ds = io.DisplaySize;
    const ImVec2 size(std::min(960.0f, ds.x * 0.94f), std::min(620.0f, ds.y * 0.92f));
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoDocking;
    bool open = true;
    if (g_justOpened) {
        ImGui::SetNextWindowFocus();  // keyboard / controller navigation starts in the menu
        g_justOpened = false;
    }
    if (ImGui::Begin("Super Paper Bros - Settings", &open, kFlags)) {
        // L / R on a controller switch tabs (not while a list is open).
        if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            for (int direction : {-1, +1}) {
                if (NavigationPressed(direction < 0 ? kPadLB : kPadRB)) {
                    g_selectTab = (g_tab + direction + kSectionCount) % kSectionCount;
                }
            }
        }
        // FittingPolicyResizeDown is the shrink-to-fit policy's name in the
        // vendored ImGui (v1.91.9b); ImGui 1.92 renamed it FittingPolicyShrink,
        // the name SwitchWakerHD builds against.
        if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
            for (int i = 0; i < kSectionCount; ++i) {
                const ImGuiTabItemFlags flags = g_selectTab == i ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(kSections[i], nullptr, flags)) {
                    if (i != g_tab) CancelCapture();  // switching tabs cancels a capture
                    g_tab = i;
                    // The page ends one row above the bottom of the window:
                    // the bottom row is the controls line below.
                    ImGui::BeginChild("page", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
                                      ImGuiChildFlags_NavFlattened);
                    DrawSectionPage(i);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            g_selectTab = -1;
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled("[L/R] Tab   [Up/Down] Move   [Left/Right] Adjust   [A] Select   [B] Close");
    }
    ImGui::End();
    PopMenuStyle();

    if (!open) SetVisible(false);
    // B (not while choosing from a list) closes the menu.
    if (NavigationPressed(kPadB) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        SetVisible(false);
    }
}

}  // namespace spm_switch_menu
