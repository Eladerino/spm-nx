// The Switch settings menu (spm_switch_menu.h). Drawn from the runtime
// overlay's Draw pass in place of the PC top bar: a top bar of sections with
// one dropdown open at a time (JSRF-style), and a description window pinned
// to the bottom of the screen. The layout is deliberate: the previous single
// list ran to 38 rows and roughly 1,100 px of content with the Developer
// gate on, drawn in one centred window with no scroll region in a 720 px
// canvas, so the selection walked into rows below the visible area and the
// highlight seemed to vanish (on hardware the walk died at the matrix trace
// row, the last Developer row that fit). Here the longest dropdown
// (Developer, gate on: a warning line plus 19 rows at the 23 px row advance,
// under the bar, with the status line shown) needs about 520 px from the
// top of the canvas, inside it with the description window's roughly 65 px
// reserved at the bottom, and every drawn row is always on screen.
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
// Menu focus state: which bar section is focused, whether its dropdown is
// open, and which row inside the open dropdown is focused. The open section
// is always the focused bar section; opening a dropdown starts at its first
// row, and switching section from inside a dropdown lands on the
// neighbour's first row too, so the focus is never ambiguous.
int g_barSection = 0;
bool g_dropdownOpen = false;
int g_row = 0;

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

// Description budget, measured from the draw code below: the window is
// 560 px wide with the default 8 px window padding on each side, leaving
// 544 px. The built-in 13 px font advances 7 px per character, so a line
// holds 77 characters and the fixed two-line area holds 154. Every row's
// description stays inside that budget (the host harness checks it).
constexpr size_t kMaxDescriptionChars = 154;

constexpr RowId DiagRow(int n) {
    return static_cast<RowId>(static_cast<int32_t>(RowId::kDiagFirst) + n);
}

// Section order per the replacement-menu spec: Graphics, Display, Audio,
// Game, Developer.
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

constexpr bool DescriptionsWithinBudget() {
    for (const RowDef& row : kRows) {
        size_t length = 0;
        while (row.description[length] != '\0') ++length;
        if (length == 0 || length > kMaxDescriptionChars) return false;
    }
    return true;
}
static_assert(DescriptionsWithinBudget(), "every menu row needs a description inside the two-line budget");

// The bar's sections, in bar order; kRows groups its rows under these names.
// While no dropdown is open, the description area shows the focused
// section's own line instead of a row's, so each section carries one too.
// A section line is a single line at the same measured width as a row
// description line (77 characters), checked below.
const char* const kSections[] = {"Graphics", "Display", "Audio", "Game", "Developer"};
constexpr int kSectionCount = 5;
constexpr int kDeveloperSection = 4;
constexpr const char* const kSectionDescriptions[] = {
    "Render size, picture shape, and frame presentation.",
    "On-screen frame rate display and the frame-time graph.",
    "Volume levels, mute, and how the game audio is mixed.",
    "Controller vibration and animated tile patches.",
    "Renderer diagnostics for test runs and bug reports.",
};
static_assert(sizeof(kSectionDescriptions) / sizeof(kSectionDescriptions[0]) == kSectionCount,
              "one description per bar section");
constexpr bool SectionDescriptionsFitOneLine() {
    for (const char* description : kSectionDescriptions) {
        size_t length = 0;
        while (description[length] != '\0') ++length;
        if (length == 0 || length > 77) return false;
    }
    return true;
}
static_assert(SectionDescriptionsFitOneLine(), "section descriptions are one line at the menu's width");

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

// Rows Left and Right adjust in place. On any other row inside a dropdown,
// Left and Right move along the bar instead (see StepInput).
bool IsCycle(RowId id) {
    return id == RowId::kResolution || id == RowId::kFrameInterp || id == RowId::kAspect;
}
bool IsAdjustable(RowId id) { return IsCycle(id) || IsSlider(id); }

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

// Resolution cycles the Switch subset only (the full PC table goes to 8x,
// which the console's frame budget cannot feed). From a value outside the
// subset (an older config's 2.25x, say), cycling starts at 1x.
const float kResolutionSubset[] = {0.75f, 1.0f, 1.5f, 0.0f};

void CycleResolution(int dir) {
    const float current = settings_overlay::RenderScale();
    int index = -1;
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(current - kResolutionSubset[i]) < 0.01f) index = i;
    }
    const float next = index < 0 ? 1.0f : kResolutionSubset[(index + dir + 4) % 4];
    settings_overlay::SetRenderScale(next);
}

// Frame interpolation cycles the Switch subset of Aurora's targets: the
// overlay's table is {0, 120, 180, 240}, and 240 needs a 240 Hz panel this
// console does not have. From a value outside the subset (a 240 a PC config
// wrote, say), cycling starts at Off.
const uint32_t kFrameInterpSubset[] = {0, 120, 180};

void CycleFrameInterp(int dir) {
    const uint32_t current = RuntimeConfigFile::FrameInterpolationFps(0);
    int index = -1;
    for (int i = 0; i < 3; ++i) {
        if (current == kFrameInterpSubset[i]) index = i;
    }
    const uint32_t next = index < 0 ? 0 : kFrameInterpSubset[(index + dir + 3) % 3];
    // The PC combo's apply pair (settings_overlay.cpp): the Aurora call
    // retargets the running frame pacing, the config write persists it.
    aurora_set_frame_interpolation_fps(next);
    RuntimeConfigFile::SetFrameInterpolationFps(next);
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
    } else if (id == RowId::kFrameInterp) {
        CycleFrameInterp(1);
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
    } else if (id == RowId::kFrameInterp) {
        CycleFrameInterp(dir);
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

// The pad model. On the bar: Left and Right move between sections, A or
// Down opens the focused section's dropdown, B closes the menu. Inside a
// dropdown: Up and Down move between its rows; Left and Right adjust the
// focused row where the row adjusts (cycles and sliders, as before), and on
// any other row they switch to the neighbouring section's dropdown; A
// activates the row; B closes the dropdown, and B on the bar closes the
// menu. Only the left stick feeds the direction state (ReadSample): the
// right stick is the guest's pointer stick, and this file never reads it.
void StepInput() {
    const Sample sample = ReadSample(AcquirePad());
    const double now = ImGui::GetTime();
    const bool up = DirectionFired(0, sample.up, now);
    const bool down = DirectionFired(1, sample.down, now);
    const bool left = DirectionFired(2, sample.left, now);
    const bool right = DirectionFired(3, sample.right, now);
    const bool confirm = sample.confirm && !g_prev.confirm;
    const bool back = sample.back && !g_prev.back;
    g_prev = sample;

    if (!g_dropdownOpen) {
        if (left) g_barSection = (g_barSection + kSectionCount - 1) % kSectionCount;
        if (right) g_barSection = (g_barSection + 1) % kSectionCount;
        if (confirm || down) {
            g_dropdownOpen = true;
            g_row = 0;
        }
        if (back) SetVisible(false);
        return;
    }

    const int count = SectionRowCount(g_barSection);
    if (count == 0) {
        g_dropdownOpen = false;
        return;
    }
    if (g_row >= count) g_row = count - 1;
    if (g_row < 0) g_row = 0;
    if (up) g_row = (g_row + count - 1) % count;
    if (down) g_row = (g_row + 1) % count;
    if (left || right) {
        const int dir = right ? 1 : -1;
        const RowDef& row = SectionRowAt(g_barSection, g_row);
        if (IsAdjustable(row.id)) {
            AdjustRow(row.id, dir);
        } else {
            g_barSection = (g_barSection + dir + kSectionCount) % kSectionCount;
            g_row = 0;
        }
    }
    if (confirm) {
        ActivateRow(SectionRowAt(g_barSection, g_row).id);
        // A flip can change the section's own list (the Developer gate):
        // re-clamp against the new count before anything draws.
        const int nextCount = SectionRowCount(g_barSection);
        if (nextCount == 0) {
            g_dropdownOpen = false;
        } else if (g_row >= nextCount) {
            g_row = nextCount - 1;
        }
    }
    if (back) g_dropdownOpen = false;
}

// ---------------------------------------------------------------------------
// The overlay-facing interface.

bool Visible() { return g_visible; }

void SetVisible(bool visible) {
    g_visible = visible;
    if (visible) g_dropdownOpen = false;  // the menu always opens on the bar
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

    if (g_dropdownOpen) {
        const int count = SectionRowCount(g_barSection);
        if (count == 0) {
            g_dropdownOpen = false;
        } else {
            if (g_row >= count) g_row = count - 1;
            if (g_row < 0) g_row = 0;
        }
    }

    // The same full-screen dim the top bar drew.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 110));
    // NoNav: this menu is driven by its own pad polling above; ImGui's
    // navigation must never move anything here.
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    const float centerX = viewport->Pos.x + viewport->Size.x * 0.5f;

    // The bar, pinned to the top of the screen, with the open section's
    // dropdown under it. Only one dropdown is open at a time, and its
    // height is bounded by the longest section (see the note at the top of
    // this file), so every row it draws is on screen.
    ImGui::SetNextWindowPos(ImVec2(centerX, viewport->Pos.y), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    if (ImGui::Begin("Switch Settings", nullptr, kFlags)) {
        for (int section = 0; section < kSectionCount; ++section) {
            if (section > 0) ImGui::SameLine();
            ImGui::PushID(section);
            ImGui::Selectable(kSections[section], section == g_barSection, ImGuiSelectableFlags_None);
            ImGui::PopID();
        }
        if (g_dropdownOpen) {
            ImGui::Separator();
            if (g_barSection == kDeveloperSection && DevGateOn()) {
                ImGui::TextDisabled("Diagnostics can hurt performance or distort the picture.");
            }
            const int count = SectionRowCount(g_barSection);
            for (int index = 0; index < count; ++index) {
                const RowDef& row = SectionRowAt(g_barSection, index);
                ImGui::PushID(static_cast<int>(row.id));
                const std::string value = ValueText(row.id);
                const std::string text =
                    value.empty() ? std::string(row.label) : (std::string(row.label) + "   " + value);
                ImGui::Selectable(text.c_str(), index == g_row, ImGuiSelectableFlags_None);
                ImGui::PopID();
                if (row.id == RowId::kAspect &&
                    (settings_overlay::Aspect() != DisplaySettings::kAspect4x3) !=
                        DisplaySettings::GameWidescreen()) {
                    ImGui::TextDisabled("Switching between 4:3 and widescreen takes effect after a restart");
                }
            }
            if (!g_status.empty() && ImGui::GetTime() < g_statusUntil) {
                ImGui::Separator();
                ImGui::Text("%s", g_status.c_str());
            }
        }
    }
    ImGui::End();

    // The description window, pinned to the bottom of the screen: the
    // focused row's description while a dropdown is open, the focused
    // section's own line while none is. Its height is the same whether the
    // text fills it or not (two text lines are reserved), so nothing above
    // it ever moves while navigating.
    ImGui::SetNextWindowPos(ImVec2(centerX, viewport->Pos.y + viewport->Size.y), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    if (ImGui::Begin("Switch Settings Description", nullptr, kFlags)) {
        const char* description = g_dropdownOpen ? SectionRowAt(g_barSection, g_row).description
                                                 : kSectionDescriptions[g_barSection];
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::BeginChild("Selected row description",
                              ImVec2(0.0f, ImGui::GetTextLineHeight() * 2.0f), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoNav)) {
            ImGui::TextWrapped("%s", description);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::Separator();
        ImGui::TextDisabled(g_dropdownOpen ? "[Up/Down] Move   [Left/Right] Adjust   [A] Select   [B] Back"
                                           : "[Left/Right] Section   [A] Open   [B] Close menu");
    }
    ImGui::End();
}

}  // namespace spm_switch_menu
