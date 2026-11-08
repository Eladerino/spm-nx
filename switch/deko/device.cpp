// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Adapted from SwitchWaker (https://github.com/centollOS/SwitchWaker) at 92307ff: switch/deko/device.cpp
// (device, queue, swapchain, debug callback, queue checks, set-up cost), itself adapted from
// SwitchWakerHD's runtime/src/gfx/deko/backend.cpp.
// The deko3d device and the window (dk.h, dk_aurora.h): created from Aurora's webgpu::initialize right
// after Dawn's device, before Aurora creates any texture or buffer, so that every one of them gets
// its deko3d shadow (wgpu_objects.cpp). The swapchain is three RGBA8 images of 1280x720 on the
// default window; the present (wgpu_encode.cpp) scales Aurora's finished picture into one of them.
#include <switch.h>
#include <unistd.h>

#include "dk.h"
#include "dk_aurora.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

extern "C" char* fake_heap_end;

// The guest sampling profiler (runtime library; declared in guest_profiler.h). Declared here
// rather than included, the same way the settings menu declares the swdk surface: diag_set
// forwards the "profile" switch to it.
namespace GuestProfiler {
void SetSwitchProfileEnabled(bool enabled);
}

// The runtime's anim pose log hook (spm_map_sweep.cpp), declared here the same way:
// diag_init and diag_set forward the "poselog" switch to it.
extern "C" void spm_pose_log_set(bool enabled);

// The runtime's Wii Remote teaching-pose suppression (spm_map_sweep.cpp), declared here
// the same way: diag_init and diag_set forward the "wiisuppress" switch to it.
extern "C" void spm_wii_suppress_set(bool enabled);

// The runtime's Return to Title hang log (spm_map_sweep.cpp), declared here the same way:
// diag_init and diag_set forward the "rttlog" switch to it.
extern "C" void spm_rtt_log_set(bool enabled);

// The runtime's GX-suppression spike (gx_worker.cpp), declared here the same way:
// diag_init and diag_set forward the "gxskip" switch to it.
extern "C" void spm_gxskip_set(bool enabled);
// The runtime's display-list cache eligibility probe (gx_dl.cpp), declared here the same
// way: diag_init and diag_set forward the "dlprobe" switch to it.
extern "C" void spm_dlprobe_set(bool enabled);
// The runtime's AXFX Hi reverb native experiment (ax_effects.cpp), declared here the same
// way: diag_init and diag_set forward the "axfxnative" switch to it.
extern "C" void spm_reverboff_set(bool enabled);  // ax_effects.cpp, the same pattern
extern "C" void spm_godmode_set(bool enabled);    // spm_map_sweep.cpp, the same pattern
extern "C" void spm_axfx_native_set(bool enabled);
// The runtime's GX setter dedup experiment (gx_vertex.cpp), declared here the same way:
// diag_init and diag_set forward the "gxdedup" switch to it.
extern "C" void spm_gxdedup_set(bool enabled);

// The runtime's double-step mode (gx_worker.cpp), declared here the same way:
// diag_init and diag_set forward the "dstep" switch to it.
extern "C" void spm_dstep_set(bool enabled);

// The runtime's DCFlushRange drain coalescing experiment (gx_worker.cpp), declared
// here the same way: diag_init and diag_set forward the "dflushcoal" switch to it.
extern "C" void spm_dflushcoal_set(bool enabled);

// The runtime's DCFlushRange no-wait experiment (gx_worker.cpp), declared here the same
// way: diag_init and diag_set forward the "dcnosync" switch to it.
extern "C" void spm_dcnosync_set(bool enabled);

// The runtime's GXInvalidateTexAll queue experiment (gx_worker.cpp), declared here the same
// way: diag_init and diag_set forward the "invtexq" switch to it.
extern "C" void spm_invtexq_set(bool enabled);

// The runtime's KPADRead no-wait experiment (gx_worker.cpp), declared here the same way:
// diag_init and diag_set forward the "kpadnosync" switch to it.
extern "C" void spm_kpadnosync_set(bool enabled);
// The natnosync experiment's hook (gx_worker.cpp), forwarded the same way.
extern "C" void spm_natnosync_set(bool enabled);

// The GX worker's poll-before-sleep (gx_worker.cpp), declared here the same way: diag_init
// and diag_set forward the "gxspin" switch to it.
extern "C" void spm_gxspin_set(bool enabled);
extern "C" void spm_gxfutex_set(bool enabled);
extern "C" void spm_gxsvclate_set(bool enabled);

// The debug server (source/spm_debug_server.cpp), declared here the same way: diag_init
// hands it the dkserver switch once the file has been read.
extern "C" void spm_debug_server_boot(bool enabled);

// The runtime's message-string patch memo (settings_overlay.cpp), declared here the same
// way: diag_init and diag_set forward the "pmsmemo" switch to it.
extern "C" void spm_pmsmemo_set(bool enabled);

// The runtime's Grade 1 KPAD input recorder (input_recorder.cpp), declared here the same
// way: diag_init and diag_set forward the "padrec" switch to it.
extern "C" void spm_input_record_set(bool enabled);
// The recorder's replay half, declared here the same way: diag_init and diag_set forward
// the "padreplay" switch to it. Unlike the bool handshakes above it carries the file to
// replay (the parameters padreplay_parse_params took from this key's dk_diag.txt value,
// resolved under UserData/Logs/ by the runtime; "" means the default pad-record.bin),
// and a live flip off passes nullptr to disarm.
extern "C" void spm_input_replay_set(const char* filename);

// The runtime's GX record ring (gx_worker.h), declared here the same way: the gxworker, gxdefer and gxthread
// switches pick its mode, which it applies at the next frame boundary.
namespace GxWorker {
enum class Mode : uint8_t;
void RequestMode(Mode mode) noexcept;
}

namespace swdk {

Renderer R;
Diag g_diag;

namespace {
// Every switch by its dk_diag.txt name. diag_read_file, diag_set, diag_get and diag_init's log
// line all go through this table, so a new switch is one line here plus its field in dk.h.
struct DiagKey {
    const char* name;
    std::atomic<bool> Diag::*field;
};
constexpr DiagKey kDiagKeys[] = {
    {"depth_off", &Diag::depthOff},   {"cull_off", &Diag::cullOff},     {"magenta", &Diag::magenta},
    {"picdump", &Diag::picDump},      {"efbclear", &Diag::efbClear},    {"bufdump", &Diag::bufDump},
    {"texdump", &Diag::texDump},      {"shaderdump", &Diag::shaderDump}, {"nomerge", &Diag::noMerge},
    {"uvlog", &Diag::uvLog},          {"mtxlog", &Diag::mtxLog},        {"perftimer", &Diag::perfTimer},
    {"arrayup", &Diag::arrayUp},      {"profile", &Diag::profile},      {"fps30", &Diag::fps30},
    {"cpthread", &Diag::cpThread},    {"gxworker", &Diag::gxWorker},    {"gxdefer", &Diag::gxDefer},
    {"gxthread", &Diag::gxThread},    {"poselog", &Diag::poseLog},      {"dkcmd", &Diag::dkCmd},
    {"zcull", &Diag::zcull},          {"guestcore", &Diag::guestCore},  {"dkserver", &Diag::dkServer},
    {"rttlog", &Diag::rttLog},        {"pmsmemo", &Diag::pmsMemo},      {"gxskip", &Diag::gxSkip},
    {"dlprobe", &Diag::dlProbe},      {"gxdedup", &Diag::gxDedup},      {"axfxnative", &Diag::axfxNative},
    {"reverboff", &Diag::reverbOff},  {"godmode", &Diag::godMode},
    {"dstep", &Diag::dstep},          {"dflushcoal", &Diag::dflushCoal}, {"vcache", &Diag::vcache},
    {"abtoggle", &Diag::abToggle},    {"wiisuppress", &Diag::wiiSuppress}, {"dcnosync", &Diag::dcNoSync},
    {"invtexq", &Diag::invTexQ},       {"kpadnosync", &Diag::kpadNoSync},
    {"natnosync", &Diag::natNoSync},
    {"gxspin", &Diag::gxSpin},       {"gxfutex", &Diag::gxFutex},     {"gxsvclate", &Diag::gxSvcLate},
    {"presentcpuwait", &Diag::presentCpuWait}, {"gpuperf", &Diag::gpuPerf},
    {"padrec", &Diag::padRecord},     {"padreplay", &Diag::padReplay},
};

const DiagKey* find_diag_key(const char* name) {
    if (!name) return nullptr;
    for (const DiagKey& key : kDiagKeys) {
        if (!std::strcmp(key.name, name)) return &key;
    }
    return nullptr;
}

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The A/B alternator's parameters (the "abtoggle" switch, dk.h). The file's switches are
// booleans — diag_read_file reads every value as on/off — so this one key's value carries
// more: the name of the switch to alternate and, after a ':', how many perftimer windows
// each state runs before the flip (no ':' means 1: a flip at every window close).
// diag_read_file hands the value to ab_parse_params, which copies the parameters into
// these statics; ab_arm (below, after apply_gx_worker_mode) validates and acts on them.
// A bare "abtoggle=1" names nothing: the statics stay empty and arming refuses, loudly.
char g_abTarget[64] = "";
uint32_t g_abPeriod = 1;
bool g_abParamsSeen = false;

void ab_parse_params(const char* value) {
    g_abTarget[0] = '\0';
    g_abPeriod = 1;
    g_abParamsSeen = false;
    if (!value || !std::strcmp(value, "0") || !strcasecmp(value, "off") ||
        !strcasecmp(value, "false") || !std::strcmp(value, "1"))
        return; // off, or on with no parameters to carry
    const char* colon = std::strchr(value, ':');
    const size_t nameLength = colon ? size_t(colon - value) : std::strlen(value);
    if (nameLength == 0 || nameLength >= sizeof(g_abTarget)) return;
    if (colon) {
        char* end = nullptr;
        const long period = std::strtol(colon + 1, &end, 10);
        if (end == colon + 1 || *end != '\0' || period < 1 || period > 100000) return;
        g_abPeriod = uint32_t(period);
    }
    std::memcpy(g_abTarget, value, nameLength);
    g_abTarget[nameLength] = '\0';
    g_abParamsSeen = true;
}

// The input replay's parameter (the "padreplay" switch, dk.h): like abtoggle, this one
// key's value carries more than on/off -- the name of the file under UserData/Logs/ to
// replay. diag_read_file hands the value to padreplay_parse_params, which copies it into
// this static; diag_init and diag_set pass it on to spm_input_replay_set. A bare
// "padreplay=1" (or an off value) names nothing: the static stays empty and the runtime
// resolves that to the default pad-record.bin.
char g_padReplayFile[128] = "";

void padreplay_parse_params(const char* value) {
    g_padReplayFile[0] = '\0';
    if (!value || !std::strcmp(value, "0") || !strcasecmp(value, "off") ||
        !strcasecmp(value, "false") || !std::strcmp(value, "1"))
        return; // off, or on with no file named: the default applies
    const size_t length = std::strlen(value);
    if (length >= sizeof(g_padReplayFile)) return; // unnameable: the default applies
    std::memcpy(g_padReplayFile, value, length + 1);
}

// dk_diag stagingslots=3|4: how many staging buffers Aurora rotates through (gfx/common.cpp).
uint32_t g_stagingSlots = 3;

// dk_diag.txt: one switch per line as "key=1" or "key=0". A "#" starts a comment, blank lines and
// spaces around the "=" are fine, a bare "key" means on, and "0", "off" or "false" mean off. A key
// the file does not name keeps its default (dk.h); an unknown one is logged and skipped.
void diag_read_file(FILE* f) {
    char line[256];
    int lineNumber = 0;
    while (std::fgets(line, sizeof(line), f)) {
        ++lineNumber;
        if (char* comment = std::strchr(line, '#')) *comment = '\0';
        char* key = line;
        while (is_blank(*key)) ++key;
        size_t length = std::strlen(key);
        while (length > 0 && is_blank(key[length - 1])) key[--length] = '\0';
        if (length == 0) continue;
        const char* value = "1";
        char* keyEnd = key + length;
        if (char* equals = std::strchr(key, '=')) {
            keyEnd = equals;
            value = equals + 1;
            while (is_blank(*value)) ++value;
        }
        while (keyEnd > key && is_blank(keyEnd[-1])) --keyEnd;
        *keyEnd = '\0';
        const bool off = !std::strcmp(value, "0") || !strcasecmp(value, "off") || !strcasecmp(value, "false");
        // abtoggle's value carries its parameters (ab_parse_params, above); the generic
        // on/off store below still applies to its field.
        if (!std::strcmp(key, "abtoggle")) ab_parse_params(value);
        // padreplay's value carries its file the same way (padreplay_parse_params, above).
        if (!std::strcmp(key, "padreplay")) padreplay_parse_params(value);
        // stagingslots takes a count, not on/off.
        if (!std::strcmp(key, "stagingslots")) {
            g_stagingSlots = std::strtol(value, nullptr, 10) == 3 ? 3 : 4;
            dklog("diag: stagingslots=%u", g_stagingSlots);
            continue;
        }
        if (const DiagKey* entry = find_diag_key(key)) {
            g_diag.*(entry->field) = !off;
        } else {
            dklog("diag: dk_diag.txt line %d: unknown switch '%s', skipped", lineNumber, key);
        }
    }
}

// The strongest GX record switch wins: 3 (Threaded), 2 (Deferred), 1 (Immediate), 0 (off), as in
// gx_worker.h.
void apply_gx_worker_mode() {
    const uint8_t mode = g_diag.gxThread ? 3 : g_diag.gxDefer ? 2 : (g_diag.gxWorker ? 1 : 0);
    GxWorker::RequestMode(static_cast<GxWorker::Mode>(mode));
}

// The automatic A/B alternator (the "abtoggle" switch, dk.h): while armed, the perftimer
// window hook below flips the target switch at window closes, through diag_set — the same
// path the settings menu, the dk_cmd channel and the debug server flip through, so the
// experiment sees exactly its normal on/off transitions. The windows are the stage timers'
// 600-frame windows (aurora-main/lib/gfx/common.cpp): the hook runs at the end of a
// window's lines, after every line that window produced, so each window runs under exactly
// one state and the [abtoggle] line that closes it says which. That is the whole point: a
// manual flip lands mid-window and mixes both states into that window's means (the
// November 4 lab session's toggle windows are all mixed), while an alternated dwell
// interleaves clean on/off window pairs over the same spot and the same minutes of play.
uint64_t g_abWindowsClosed = 0;  // windows closed since arming; the hook is the only writer
bool g_abState = false;          // the state the target ran under in the window now closing
bool g_abHookRegistered = false; // the window hook is registered once, at the first arm

void ab_window_hook(uint64_t firstFrame, uint64_t lastFrame) {
    if (!g_diag.abToggle.load()) return; // disarmed: stay registered, stay silent
    ++g_abWindowsClosed;
    const unsigned long long first = (unsigned long long)firstFrame;
    const unsigned long long last = (unsigned long long)lastFrame;
    const unsigned long long present = (unsigned long long)R.presents;
    if (g_abWindowsClosed % g_abPeriod != 0) {
        std::fprintf(stderr,
                     "[abtoggle] frames %llu-%llu: %s=%d (no flip: window %llu of %u under this state) at present %llu\n",
                     first, last, g_abTarget, g_abState ? 1 : 0,
                     (unsigned long long)(g_abWindowsClosed % g_abPeriod), g_abPeriod, present);
    } else {
        const bool next = !g_abState;
        std::fprintf(stderr, "[abtoggle] frames %llu-%llu: %s=%d, flipping to %d at present %llu\n", first,
                     last, g_abTarget, g_abState ? 1 : 0, next ? 1 : 0, present);
        diag_set(g_abTarget, next);
        g_abState = next;
    }
    log_flush();
}

// Arms the alternator with the parameters ab_parse_params took from dk_diag.txt. Called
// from diag_init (a file line) and from diag_set (a live flip of the abtoggle switch, which
// re-arms: the window count restarts and the target's current state is re-read, so the
// first window after it runs a known state). A refusal logs a line and clears the switch,
// so nothing ever looks armed that is not.
bool ab_arm() {
    if (!g_abParamsSeen) {
        dklog("diag: abtoggle: dk_diag.txt names no target (want abtoggle=<switch> or "
              "abtoggle=<switch>:<windows>); not arming");
        g_diag.abToggle = false;
        return false;
    }
    if (!find_diag_key(g_abTarget) || !std::strcmp(g_abTarget, "abtoggle") ||
        !std::strcmp(g_abTarget, "perftimer")) {
        dklog("diag: abtoggle: '%s' cannot be alternated (an unknown switch, abtoggle itself, or "
              "perftimer, whose windows drive the alternator); not arming",
              g_abTarget);
        g_diag.abToggle = false;
        return false;
    }
    g_abState = diag_get(g_abTarget);
    g_abWindowsClosed = 0;
    if (!g_abHookRegistered) {
        AuroraSetPerfWindowHook(&ab_window_hook);
        g_abHookRegistered = true;
    }
    dklog("diag: abtoggle armed: %s flips every %u perftimer window(s); the first window runs %s=%d",
          g_abTarget, g_abPeriod, g_abTarget, g_abState ? 1 : 0);
    if (!g_diag.perfTimer.load())
        dklog("diag: abtoggle: perftimer is off, so no windows close and nothing flips until it is turned on");
    return true;
}
} // namespace

// The diagnostic switches (dk.h): UserData/dk_diag.txt, read once here (format at diag_read_file).
// The active set is logged so a test log always says which switches the run used.
void diag_init() {
    if (FILE* f = std::fopen("/switch/spm-nx/UserData/dk_diag.txt", "r")) {
        diag_read_file(f);
        std::fclose(f);
    } else {
        dklog("diag: no UserData/dk_diag.txt; every switch at its default");
    }
    // Aurora's switches initialise from these env vars on their first use; diag_init runs from
    // webgpu::initialize, before any texture, buffer or draw exists, so setting them here lands
    // in time. In-process libc env: Aurora's getenv sees what this setenv wrote. After init the
    // values live in Aurora's own flags and diag_set below is what changes them.
    if (g_diag.noMerge) setenv("NSMBW_NO_MERGE", "1", 1);
    // Same handshake for NSMBW_UV_LOG: build_uniform initialises its flag from it on the first draw it stages.
    if (g_diag.uvLog) setenv("NSMBW_UV_LOG", "1", 1);
    // Same handshake for NSMBW_MTX_LOG.
    if (g_diag.mtxLog) setenv("NSMBW_MTX_LOG", "1", 1);
    // Same handshake for NSMBW_STAGE_TIMERS.
    if (g_diag.perfTimer) setenv("NSMBW_STAGE_TIMERS", "1", 1);
    // Same handshake for NSMBW_ARRAY_REUPLOAD: handle_draw initialises its flag from it on the first draw it handles.
    if (g_diag.arrayUp) setenv("NSMBW_ARRAY_REUPLOAD", "1", 1);
    // The profiler's env bridge: NSMBW_PROFILE is set like the five above so the state is
    // visible the same way, but the value is only a marker here - the profiler's start:length
    // config reader is part of its Windows capture path and is not compiled on Switch, and the
    // Switch sampler is toggle-driven (one session, reported when the switch turns off), so it
    // has no window to read. diag_set and the end of this function drive it directly instead.
    if (g_diag.profile) setenv("NSMBW_PROFILE", "1", 1);
    // No env var for the decode thread: Aurora reads the request at each frame's end, so setting
    // it now has it take over from the first frame's end.
    if (g_diag.cpThread) aurora::gx::fifo::set_decode_thread_requested(true);
    // The GX record ring takes its mode at the first frame boundary, before any guest frame ends.
    apply_gx_worker_mode();
    char active[384] = "";
    size_t used = 0;
    for (const DiagKey& key : kDiagKeys) {
        const int n = std::snprintf(active + used, sizeof(active) - used, "%s%s=%d", used ? " " : "", key.name,
                                    (g_diag.*(key.field)).load() ? 1 : 0);
        if (n < 0 || size_t(n) >= sizeof(active) - used) break;
        used += size_t(n);
    }
    dklog("diag: %s", active);
    // A profile=1 in dk_diag.txt starts the sampler now. Renderer init runs before the guest
    // starts, so the session covers the boot too; samples resolve at report time, so nothing
    // about the early start hurts attribution. The clean session is still the menu toggle:
    // on when the heavy scene is on screen, off when it is done.
    if (g_diag.profile) GuestProfiler::SetSwitchProfileEnabled(true);
    // A poselog=1 in dk_diag.txt arms the runtime's anim pose log from the start, so the
    // session covers the poses drawn during boot and the title as well.
    if (g_diag.poseLog) spm_pose_log_set(true);
    // A wiisuppress=1 in dk_diag.txt arms the runtime's teaching-pose suppression from
    // the start. It only skips the one measured tutorial tuple, so boot and the title
    // are untouched either way.
    if (g_diag.wiiSuppress) spm_wii_suppress_set(true);
    // The runtime's message-string patch memo, on unless dk_diag.txt says pmsmemo=0, armed
    // from the start so the session's first area changes already run memoised passes. The
    // runtime's own default stays off (other ports never call this), so the boot value is
    // always handed over, on or off.
    spm_pmsmemo_set(g_diag.pmsMemo);
    // A dkserver=1 in dk_diag.txt starts the debug server (TCP 6543, LAN only, no auth).
    // With the switch off this call returns at once: no socket, no thread.
    spm_debug_server_boot(g_diag.dkServer);
    // A rttlog=1 in dk_diag.txt arms the runtime's Return to Title hang log from the start,
    // so the session covers the boot's own file select too: the working arrival the hanging
    // one gets compared against.
    if (g_diag.rttLog) spm_rtt_log_set(true);
    // A gxskip=1 in dk_diag.txt arms the runtime's GX-suppression spike from the start. The
    // runtime only sinks frames while its gameplay gate is open, so boot and the title are
    // untouched either way.
    if (g_diag.gxSkip) spm_gxskip_set(true);
    // A dlprobe=1 in dk_diag.txt arms the runtime's display-list eligibility probe from
    // the start. The probe only hashes and counts, so boot and the title are measured the
    // same as gameplay; the first perftimer window is the warmup either way.
    if (g_diag.dlProbe) spm_dlprobe_set(true);
    // An axfxnative=1 in dk_diag.txt arms the runtime's AXFX Hi reverb native wrapper from
    // the start. The wrapper reads the switch on every callback, so this only sets the
    // initial state; the menu and the command channel flip it live.
    if (g_diag.axfxNative) spm_axfx_native_set(true);
    if (g_diag.reverbOff) spm_reverboff_set(true);
    if (g_diag.godMode) spm_godmode_set(true);
    // A gxdedup=1 in dk_diag.txt arms the runtime's GX setter dedup experiment from the start.
    if (g_diag.gxDedup) spm_gxdedup_set(true);
    // A dstep=1 in dk_diag.txt arms the runtime's double-step mode from the start. The
    // runtime only doubles frames while its gameplay gate is open, so boot and the title
    // are untouched either way.
    if (g_diag.dstep) spm_dstep_set(true);
    // A dflushcoal=1 in dk_diag.txt arms the runtime's DCFlushRange drain coalescing
    // from the start. The witness it reads is kept from the first frame either way, so
    // this only sets the initial switch state; the menu flips it live.
    if (g_diag.dflushCoal) spm_dflushcoal_set(true);
    // DCFlushRange without waiting is on unless dk_diag.txt says dcnosync=0 (the Diag
    // default, like cpthread's). DcRangeOp reads the switch on every data-cache op, so this
    // only sets the initial state; the menu and the command channel flip it live.
    if (g_diag.dcNoSync) spm_dcnosync_set(true);
    // Queued texture invalidates are on unless dk_diag.txt says invtexq=0 (the Diag default);
    // the dispatch layer reads the switch on every call, so the menu flips it live.
    if (g_diag.invTexQ) spm_invtexq_set(true);
    // A kpadnosync=1 in dk_diag.txt arms the KPADRead no-wait experiment from the start; the
    // dispatch layer reads the switch on every call, so the menu flips it live.
    if (g_diag.kpadNoSync) spm_kpadnosync_set(true);
    // natnosync is on unless dk_diag.txt says natnosync=0 (the Diag default), so its state is
    // forwarded either way.
    spm_natnosync_set(g_diag.natNoSync);
    // gxspin is on unless dk_diag.txt says gxspin=0 (the Diag default), so its state is
    // forwarded either way; the worker reads it each time it catches up.
    spm_gxspin_set(g_diag.gxSpin);
    spm_gxfutex_set(g_diag.gxFutex);
    spm_gxsvclate_set(g_diag.gxSvcLate);
    // A vcache=1 in dk_diag.txt arms Aurora's decoded display-list vertex cache from
    // the start. The cache lives inside Aurora's decode path and reads the switch on
    // every list, so this only sets the initial state; the menu flips it live.
    if (g_diag.vcache) aurora::gx::fifo::vcache_set_enabled(true);
    // An abtoggle=<switch>[:<windows>] line arms the automatic A/B alternator (ab_arm,
    // above): it flips the named switch at perftimer window closes so a dwell measures
    // both of its states interleaved. The parameters rode the value because the file's
    // switches are booleans; ab_parse_params has them by now.
    if (g_diag.abToggle) ab_arm();
    // A padrec=1 in dk_diag.txt starts the input recorder from boot, so the recording
    // covers the boot and title reads the replay will also consume; a
    // padreplay=<file> line arms replay from the file padreplay_parse_params took
    // from the value ("" is the default pad-record.bin, resolved by the runtime).
    if (g_diag.padRecord) spm_input_record_set(true);
    if (g_diag.padReplay) spm_input_replay_set(g_padReplayFile);
}

// diag_set (dk.h): the runtime path for the same switches. Every switch stores its field, which
// the interposer's use sites read per bind, per present or per compile. Aurora's six also forward
// to Aurora's own flags (initialised from the env vars in diag_init, settable since), the profile
// switch to the runtime's guest profiler (its hot list prints when it turns off), and the GX record
// switches to the record ring. A flip lands from the switch's next use onward, except cpthread and
// the GX record switches, which take effect at the end of the frame.
bool diag_set(const char* key, bool on) {
    const DiagKey* entry = find_diag_key(key);
    if (!entry) return false;
    g_diag.*(entry->field) = on;
    if (!std::strcmp(key, "nomerge")) aurora::gx::fifo::nsmbw_set_no_merge(on ? 1 : 0);
    else if (!std::strcmp(key, "uvlog")) aurora::gx::nsmbw_set_uv_log(on);
    else if (!std::strcmp(key, "mtxlog")) aurora::gx::nsmbw_set_mtx_log(on);
    else if (!std::strcmp(key, "perftimer")) aurora::gfx::stage_timers_set_enabled(on);
    else if (!std::strcmp(key, "arrayup")) aurora::gx::fifo::nsmbw_set_array_reupload(on);
    else if (!std::strcmp(key, "profile")) GuestProfiler::SetSwitchProfileEnabled(on);
    else if (!std::strcmp(key, "poselog")) spm_pose_log_set(on);
    else if (!std::strcmp(key, "wiisuppress")) spm_wii_suppress_set(on);
    else if (!std::strcmp(key, "pmsmemo")) spm_pmsmemo_set(on);
    else if (!std::strcmp(key, "rttlog")) spm_rtt_log_set(on);
    else if (!std::strcmp(key, "gxskip")) spm_gxskip_set(on);
    else if (!std::strcmp(key, "dlprobe")) spm_dlprobe_set(on);
    else if (!std::strcmp(key, "axfxnative")) spm_axfx_native_set(on);
    else if (!std::strcmp(key, "reverboff")) spm_reverboff_set(on);
    else if (!std::strcmp(key, "godmode")) spm_godmode_set(on);
    else if (!std::strcmp(key, "gxdedup")) spm_gxdedup_set(on);
    else if (!std::strcmp(key, "dstep")) spm_dstep_set(on);
    else if (!std::strcmp(key, "dflushcoal")) spm_dflushcoal_set(on);
    else if (!std::strcmp(key, "dcnosync")) spm_dcnosync_set(on);
    else if (!std::strcmp(key, "invtexq")) spm_invtexq_set(on);
    else if (!std::strcmp(key, "kpadnosync")) spm_kpadnosync_set(on);
    else if (!std::strcmp(key, "natnosync")) spm_natnosync_set(on);
    else if (!std::strcmp(key, "gxspin")) spm_gxspin_set(on);
    else if (!std::strcmp(key, "gxfutex")) spm_gxfutex_set(on);
    else if (!std::strcmp(key, "gxsvclate")) spm_gxsvclate_set(on);
    else if (!std::strcmp(key, "padrec")) spm_input_record_set(on);
    else if (!std::strcmp(key, "padreplay")) {
        // Arming passes the file padreplay_parse_params took from dk_diag.txt (a live
        // "set padreplay 1" through dk_cmd or the debug server carries no file of its
        // own, so it re-arms with that one, or the default when the file named none);
        // disarming passes nullptr.
        spm_input_replay_set(on ? g_padReplayFile : nullptr);
    }
    else if (!std::strcmp(key, "abtoggle")) {
        // Arming takes the target and period from dk_diag.txt (ab_parse_params), so a live
        // "set abtoggle 1" re-arms with them. Disarming needs no call: the window hook
        // checks the switch at every window close and stays silent, and the target keeps
        // whichever state it was last flipped to (the last [abtoggle] line says which).
        if (on) ab_arm();
    }
    else if (!std::strcmp(key, "cpthread")) aurora::gx::fifo::set_decode_thread_requested(on);
    else if (!std::strcmp(key, "vcache")) aurora::gx::fifo::vcache_set_enabled(on);
    else if (!std::strcmp(key, "gxworker") || !std::strcmp(key, "gxdefer") || !std::strcmp(key, "gxthread"))
        apply_gx_worker_mode();
    return true;
}

// diag_get (dk.h): the read side of diag_set, for the settings menu's Diagnostics rows.
bool diag_get(const char* key) {
    const DiagKey* entry = find_diag_key(key);
    return entry && (g_diag.*(entry->field)).load();
}

// diag_enumerate (dk.h): every switch in table order, for the debug server's listing.
void diag_enumerate(void (*visit)(const char* name, bool on, void* user), void* user) {
    if (!visit) return;
    for (const DiagKey& key : kDiagKeys) visit(key.name, (g_diag.*(key.field)).load(), user);
}

// diag_reset_defaults (dk.h): a fresh Diag carries the defaults; pushing each one through
// diag_set runs that switch's forwarding, so the runtime lands in the same state a boot
// with no dk_diag.txt would arm. Table order ends on gxthread, so apply_gx_worker_mode's
// last word is the default mode.
void diag_reset_defaults() {
    const Diag defaults;
    for (const DiagKey& key : kDiagKeys) diag_set(key.name, (defaults.*(key.field)).load());
}

// diag_present_count (dk.h): R.presents for "[diag]" log markers written
// outside this file (the menu's set/mark lines use diag_cmd_poll's format).
uint64_t diag_present_count() { return R.presents; }

// diag_flush_all (dk.h): everything that can be holding diagnostic output lets go of it, in
// dependency order. Aurora's mtxlog buffer first (its lines are the only ones buffered above
// stderr), then a partial perftimer window while perftimer is on, so the segment just played
// gets its own numbers, then the stderr stream itself, which every writer in the process
// shares. The marker goes last, and is flushed too.
void diag_flush_all(const char* reason) {
    aurora::gx::nsmbw_mtxlog_flush();
    if (g_diag.perfTimer) aurora::gfx::stage_time_print_now();
    log_flush();
    std::fprintf(stderr, "[diag] logs flushed (%s) at present %llu\n", reason ? reason : "",
                 (unsigned long long)R.presents);
    log_flush();
}

// The command channel (dk.h): UserData/dk_cmd.txt, one command per write, looked for by the
// present path every 30th present (twice a second; the look is an SD card file open) and
// truncated whether or not it parsed, so a command can never run twice. Commands:
//   flush           diag_flush_all, now
//   mark <text>     a "[diag] mark" line bracketing what follows it in the log, then a flush
//   set <key> <0|1> diag_set for any dk_diag.txt switch; the flip is logged with its present
//                   count, so a pulled log shows which state each segment ran under
// Anything else, an unknown key, or a malformed set logs a [diag] error line and changes
// nothing. The channel exists so a test run can be driven, and its log flushed, without
// rebooting to edit dk_diag.txt or waiting out a buffer.
void diag_cmd_poll() {
    // Armed only for test and scripted runs (spec A2): the poll is an SD file open every 30
    // presents forever, and normal play has no commands to receive. dkcmd=1 is the explicit arm;
    // a session switch (a dump, the timers, the profiler, a trace) also counts, because a run
    // driving one of those is a test run. The default-on switches (cpthread, gxthread) do not
    // count: they are defaults, not an arming signal.
    if (!g_diag.dkCmd && !g_diag.picDump && !g_diag.bufDump && !g_diag.texDump && !g_diag.shaderDump &&
        !g_diag.perfTimer && !g_diag.profile && !g_diag.mtxLog && !g_diag.uvLog)
        return;
    FILE* f = std::fopen("/switch/spm-nx/UserData/dk_cmd.txt", "r");
    if (!f) return;
    char cmd[256];
    const bool got = std::fgets(cmd, sizeof(cmd), f) != nullptr;
    std::fclose(f);
    // Truncate before executing: a command that logs heavily must not meet itself again at
    // the next present.
    if (FILE* t = std::fopen("/switch/spm-nx/UserData/dk_cmd.txt", "w")) std::fclose(t);
    if (!got) return;
    // One line: drop the newline and any trailing whitespace.
    size_t len = std::strlen(cmd);
    while (len > 0 && (cmd[len - 1] == '\n' || cmd[len - 1] == '\r' || cmd[len - 1] == ' ')) cmd[--len] = '\0';
    if (len == 0) return;
    const unsigned long long present = (unsigned long long)R.presents;
    if (!std::strcmp(cmd, "flush")) {
        diag_flush_all("dk_cmd");
    } else if (!std::strncmp(cmd, "mark", 4) && (cmd[4] == '\0' || cmd[4] == ' ')) {
        std::fprintf(stderr, "[diag] mark \"%s\" at present %llu\n", cmd[4] == ' ' ? cmd + 5 : "", present);
        diag_flush_all("mark");
    } else if (!std::strncmp(cmd, "set ", 4)) {
        char key[64];
        char val[8];
        if (std::sscanf(cmd + 4, "%63s %7s", key, val) == 2 && (val[0] == '0' || val[0] == '1') &&
            val[1] == '\0') {
            if (diag_set(key, val[0] == '1')) {
                std::fprintf(stderr, "[diag] set %s=%s at present %llu\n", key, val, present);
            } else {
                std::fprintf(stderr, "[diag] error: unknown diag switch \"%s\"\n", key);
            }
        } else {
            std::fprintf(stderr, "[diag] error: malformed dk_cmd command \"%s\" (want: set <key> <0|1>)\n", cmd);
        }
        log_flush();
    } else {
        std::fprintf(stderr, "[diag] error: unknown dk_cmd command \"%s\"\n", cmd);
        log_flush();
    }
}

namespace {

constexpr uint32_t kSwapImages = 3;
constexpr uint32_t kQueueCommandMemory = 1u << 20;

DkSwapchain g_swapchain = nullptr;
DkImage g_swapImages[kSwapImages];
ImageAlloc g_swapMem[kSwapImages];
int g_debugMessages = 0;

// the heap malloc has never grown into, in MiB (libnx's heap end against malloc's break): deko3d's
// memory blocks come from it, so the difference across the set-up is its cost
size_t heap_never_used_mib() {
    char* top = static_cast<char*>(sbrk(0));
    if (!fake_heap_end || !top || top == reinterpret_cast<char*>(-1)) return 0;
    return size_t(fake_heap_end - top) >> 20;
}

// deko3d's messages: only the debug library calls this (the release library aborts with 2359-xxxx)
void debug_message(void*, const char* context, DkResult result, const char* message) {
    if (result != DkResult_Success) {
        fatal("deko3d error in %s: result %d: %s", context ? context : "?", int(result), message ? message : "");
    }
    const int n = g_debugMessages++;
    if (n < 200) {
        dklog("deko3d %s: %s", context ? context : "?", message ? message : "");
    } else if (n == 200) {
        dklog("deko3d: more messages, not logged");
    }
}

void init_swapchain() {
    const DkImage* images[kSwapImages];
    for (uint32_t i = 0; i < kSwapImages; i++) {
        DkImageLayoutMaker m;
        dkImageLayoutMakerDefaults(&m, R.device);
        m.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
        m.format = DkImageFormat_RGBA8_Unorm;
        m.dimensions[0] = R.width;
        m.dimensions[1] = R.height;
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &m);
        g_swapMem[i] = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
        dkImageInitialize(&g_swapImages[i], &layout, g_swapMem[i].block, g_swapMem[i].offset);
        images[i] = &g_swapImages[i];
    }
    DkSwapchainMaker m;
    dkSwapchainMakerDefaults(&m, R.device, nwindowGetDefault(), images, kSwapImages);
    dklog("creating the swapchain on the default window");
    log_flush();
    g_swapchain = dkSwapchainCreate(&m);
    dkSwapchainSetSwapInterval(g_swapchain, 1);
    dklog("swapchain: %u RGBA8 images of %ux%u, swap interval 1", kSwapImages, R.width, R.height);
}

}  // namespace

int swapchain_acquire(DkFence* fence) {
    int slot = 0;
    dkSwapchainAcquireImage(g_swapchain, &slot, fence);
    return slot;
}

void swapchain_wait(DkFence* fence) {
    check_queue("acquiring a swapchain image");
    dkQueueWaitFence(R.queue, fence);
}

void swapchain_wait_cpu(DkFence* fence) {
    if (dkFenceWait(fence, 10'000'000'000ll) != DkResult_Success) dklog("a swapchain image was not free after 10 s");
}

const DkImage& swapchain_image(int slot) { return g_swapImages[slot]; }

void swapchain_present(int slot) {
    // Measurement: heap headroom through the session, about every 20 s of play.
    static uint64_t s_heapLogPresents = 0;
    if (++s_heapLogPresents % 1200 == 0) {
        dklog("heap never used %zu MiB at present %llu", heap_never_used_mib(), (unsigned long long)s_heapLogPresents);
    }
    check_queue("the present");
    dkQueuePresentImage(R.queue, g_swapchain, slot);
    R.presents++;
}

bool env_flag(const char* name, bool def) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0';
}

void dklog(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    fprintf(stderr, "[dk] %s\n", line);
}

void log_flush() { fflush(stderr); }

void fatal(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    fprintf(stderr, "[dk] FATAL: %s\n", line);
    log_flush();
    abort();
}

void check_queue(const char* before) {
    if (!dkQueueIsInErrorState(R.queue)) return;
    fatal("the deko3d queue is in an error state (a GPU fault) before %s, after %llu presents; deko3d messages %d",
          before, (unsigned long long)R.presents, g_debugMessages);
}

}  // namespace swdk

using namespace swdk;

extern "C" uint32_t aurora_switch_staging_slots() { return g_stagingSlots; }

extern "C" void aurora_switch_dk_init(void) {
    if (R.device) return;
    diag_init();
    const uint64_t t0 = now_ns();
    const size_t heapBefore = heap_never_used_mib();
    dklog("deko3d renderer: Aurora's WebGPU calls replayed on deko3d; heap never used before the set-up %zu MiB",
          heapBefore);
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_message;
    // window origin top left, depth 0 to 1, clip-space y up: WebGPU's conventions (dk.h)
    dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
    dklog("creating the device");
    log_flush();
    R.device = dkDeviceCreate(&dm);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, R.device);
    // zcull per the diag switch (default on in this test build): SwitchWakerHD's old decal
    // finding is fixed by the depth-bias invalidate at the pipeline bind (wgpu_encode.cpp).
    // Compute stays on: Aurora's depth peek (GXPeekZ) is a compute pass.
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_MediumPrio |
               (g_diag.zcull ? DkQueueFlags_EnableZcull : DkQueueFlags_DisableZcull);
    qm.commandMemorySize = kQueueCommandMemory;
    qm.flushThreshold = qm.commandMemorySize / 8;
    dklog("creating the queue");
    log_flush();
    R.queue = dkQueueCreate(&qm);

    memory_init();
    init_swapchain();
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        empty_image_init();
    }
    shaders_init();
    const size_t heapAfter = heap_never_used_mib();
    dklog("set-up cost: %zu MiB of heap (never used: %zu MiB before, %zu MiB after), %.0f ms",
          heapBefore > heapAfter ? heapBefore - heapAfter : 0, heapBefore, heapAfter, double(now_ns() - t0) / 1e6);
    log_flush();
}

extern "C" void aurora_switch_dk_shutdown(void) {
    if (R.queue == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        flush();
    }
    dkQueueWaitIdle(R.queue);
    if (g_swapchain != nullptr) {
        dkSwapchainDestroy(g_swapchain);  // gives the window's buffers back
        g_swapchain = nullptr;
    }
    dklog("shut down after %llu presents", (unsigned long long)R.presents);
}
