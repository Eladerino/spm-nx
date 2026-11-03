// The spm-nx commands for the shared debug server core (debug_server.h, vendored from
// SwitchWakerHD: the core knows sockets, lines and files; this file is the part that knows
// spm-nx). WakerHD's version of this file is runtime/src/platform/debug_switch.cpp,
// SwitchWaker's is switch/native/source/cos_debug.cpp; ours follows the same split.
//
// The gate is the dkserver switch in UserData/dk_diag.txt (dk.h): diag_init hands it to
// spm_debug_server_boot below, and with it off nothing here runs: no socket, no thread, no
// cost. It is boot-time only on purpose (the listener starts once, at renderer init), so
// the Diagnostics page has no row for it and diag_set("dkserver") changes nothing live;
// the server's own diag command refuses the key and says why.
//
// What the server adds over the core's built-ins (ping, help, get, put, ls, rm, mkdir,
// log, logtext, press, hold, release, stick):
//   info, diag (+ get/set inside it, and dk_cmd's flat set spelling; a flat get is the
//     core's own file command and keeps that meaning), flush, mark: the dk_diag
//     switches and the log, driven the same way the menu and dk_cmd.txt drive them, so a
//     test run can be steered and its log flushed without a reboot or a file edit;
//   warp, place: the Debug menu's own warp machinery (SpmMapSweep::RequestWarp), so a
//     scripted run can move the game and read back where it landed;
//   shot, peek, poke, quit, reload answer err and say they are not in this build yet:
//     the renderer has no frame capture, guest-memory reads need the crash handler's
//     mapping-checked read first, and quitting cleanly wants the hbloader next-load path
//     (the follow-ups are listed in switch/README.md's debug server paragraph).
// Input injection needs no command here: the core holds the injected state and the
// provider at the bottom of this file feeds it to the SDL gamepad shim's weak
// cos_switch_debug_input hook, exactly as SwitchWaker's cos_debug.cpp does.
//
// The log feed: the core streams what log_tap is handed, and spm-nx's log writer is
// stderr (redirected to UserData/Logs/stderr.log, line-buffered). A pump thread on the
// server's behalf tails that file from its start into log_tap, so the stream carries
// the whole session, boot lines included, without touching any of the game's log call
// sites. The pump and the core's threads run at the lowest thread priority (0x3B): a
// scripted run must never take time from the game it is measuring.
#include "debug_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

// The diag switches (switch/deko/device.cpp) and the map sweep (the runtime), declared
// here rather than included: dk.h pulls in deko3d, and device.cpp declares its runtime
// neighbours the same way. diag_set is documented "any thread" there; RequestWarp only
// queues, and the game's own frame does the work.
namespace swdk {
bool diag_set(const char* key, bool on);
bool diag_get(const char* key);
void diag_enumerate(void (*visit)(const char* name, bool on, void* user), void* user);
void diag_flush_all(const char* reason);
uint64_t diag_present_count();
void log_flush();
}  // namespace swdk

namespace SpmMapSweep {
void RequestWarp(const char* map, const char* door, const float* arrivalPos);
void CurrentPlace(char* out, size_t size);
int Seq();
}  // namespace SpmMapSweep

// source/spm_switch_init.cpp: the Prepare sweep's keep-awake switch, reused below (a
// console under test sits untouched for minutes at a time).
extern "C" void spm_switch_keep_awake(bool on);

namespace SpmDebugServer {
namespace {

std::atomic<bool> g_booted{false};
std::atomic<bool> g_pumpRun{false};
std::thread g_pump;
std::string g_root;  // the server's file root (Config.root), set at boot

void LogLine(const char* line) {
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
}

// ---- the diag switches, as commands ---------------------------------------------------

struct KeyProbe {
    const char* key;
    bool found = false;
    bool value = false;
};

void ProbeVisit(const char* name, bool on, void* user) {
    auto* probe = static_cast<KeyProbe*>(user);
    if (!std::strcmp(name, probe->key)) {
        probe->found = true;
        probe->value = on;
    }
}

KeyProbe Probe(const std::string& key) {
    KeyProbe probe{key.c_str()};
    swdk::diag_enumerate(&ProbeVisit, &probe);
    return probe;
}

void ListVisit(const char* name, bool on, void* user) {
    auto* out = static_cast<std::string*>(user);
    out->append(name);
    out->append(on ? "=1\n" : "=0\n");
}

debugsrv::Reply DiagSet(const std::string& key, const std::string& val) {
    if (val != "0" && val != "1") return debugsrv::err("usage: set <key> <0|1>");
    // The one switch a running server must not flip: it is read once at boot, so a live
    // set could only lie about what a restart will do.
    if (key == "dkserver")
        return debugsrv::err("dkserver is boot-time only: edit UserData/dk_diag.txt and restart");
    if (!Probe(key).found) return debugsrv::err("unknown diag switch \"" + key + "\"");
    const bool on = val == "1";
    swdk::diag_set(key.c_str(), on);
    // dk_cmd's set logs the flip the same way, so a pulled log shows which state each
    // segment ran under whichever channel flipped it.
    std::fprintf(stderr, "[diag] set %s=%d at present %llu (debug server)\n", key.c_str(), on ? 1 : 0,
                 (unsigned long long)swdk::diag_present_count());
    swdk::log_flush();
    return debugsrv::ok(key + "=" + val + "\n");
}

debugsrv::Reply DiagGet(const std::string& key) {
    const KeyProbe probe = Probe(key);
    if (!probe.found) return debugsrv::err("unknown diag switch \"" + key + "\"");
    return debugsrv::ok(key + (probe.value ? "=1\n" : "=0\n"));
}

debugsrv::Reply CmdDiag(const debugsrv::Args& a) {
    if (a.empty()) {
        std::string out;
        swdk::diag_enumerate(&ListVisit, &out);
        return debugsrv::ok(out);
    }
    if (a[0] == "get" && a.size() == 2) return DiagGet(a[1]);
    if (a[0] == "set" && a.size() == 3) return DiagSet(a[1], a[2]);
    return debugsrv::err("usage: diag | diag get <key> | diag set <key> <0|1>");
}

debugsrv::Reply CmdSet(const debugsrv::Args& a) {
    if (a.size() != 2) return debugsrv::err("usage: set <key> <0|1>");
    return DiagSet(a[0], a[1]);
}

debugsrv::Reply CmdFlush(const debugsrv::Args&) {
    swdk::diag_flush_all("server");
    return debugsrv::ok();
}

debugsrv::Reply CmdMark(const debugsrv::Args& a) {
    std::string text;
    for (size_t i = 0; i < a.size(); i++) {
        if (i) text += ' ';
        text += a[i];
    }
    // dk_cmd's mark line, verbatim format, so both channels bracket logs the same way.
    std::fprintf(stderr, "[diag] mark \"%s\" at present %llu\n", text.c_str(),
                 (unsigned long long)swdk::diag_present_count());
    swdk::diag_flush_all("mark");
    return debugsrv::ok();
}

// ---- where the game is, and moving it ---------------------------------------------------

debugsrv::Reply CmdInfo(const debugsrv::Args&) {
    char place[64] = "";
    SpmMapSweep::CurrentPlace(place, sizeof place);
    char text[320];
    std::snprintf(text, sizeof text,
                  "game spm-nx\nplace %s\nsequence %d\npresent %llu\nserver port 6543\nroot %s\n",
                  place[0] ? place : "?", SpmMapSweep::Seq(),
                  (unsigned long long)swdk::diag_present_count(), g_root.c_str());
    return debugsrv::ok(text);
}

debugsrv::Reply CmdPlace(const debugsrv::Args&) {
    char place[64] = "";
    SpmMapSweep::CurrentPlace(place, sizeof place);
    return debugsrv::ok(std::string(place[0] ? place : "?") + "\n");
}

debugsrv::Reply CmdWarp(const debugsrv::Args& a) {
    if (a.empty() || a.size() > 2) return debugsrv::err("usage: warp <map> [door]");
    SpmMapSweep::RequestWarp(a[0].c_str(), a.size() > 1 ? a[1].c_str() : nullptr, nullptr);
    return debugsrv::ok("warp to " + a[0] + " queued (it runs on the next frame the game is in a map)\n");
}

// ---- not in this build (the follow-ups, answered honestly) ------------------------------

debugsrv::Reply CmdShot(const debugsrv::Args&) {
    return debugsrv::err(
        "shot is not in this build: the renderer has no frame capture yet (picdump only logs the "
        "picture's brightness). A capture readback of the present surface is the follow-up");
}

debugsrv::Reply CmdPeek(const debugsrv::Args&) {
    return debugsrv::err(
        "peek is not in this build: the flat guest window faults on an unmapped read, so peeking "
        "first needs the crash handler's mapping-checked read. It is a follow-up");
}

debugsrv::Reply CmdPoke(const debugsrv::Args&) {
    return debugsrv::err("poke is not in this build (see peek); writing guest memory is a follow-up");
}

debugsrv::Reply CmdQuit(const debugsrv::Args&) {
    return debugsrv::err(
        "quit is not in this build: nothing yet ends the program on the server's word. It is a "
        "follow-up, with reload, through the hbloader next-load path");
}

debugsrv::Reply CmdReload(const debugsrv::Args&) {
    return debugsrv::err(
        "reload is not in this build: it needs the hbloader next-load path (put the new NRO, set "
        "the next load, shut down cleanly). It is a follow-up");
}

// ---- the log pump ------------------------------------------------------------------------

void PumpMain() {
#ifdef __SWITCH__
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
#endif
    const std::string path = g_root + "/UserData/Logs/stderr.log";
    std::vector<char> buf(64 << 10);
    long offset = 0;
    FILE* f = nullptr;
    while (g_pumpRun.load()) {
        if (!f) {
            f = std::fopen(path.c_str(), "r");
            offset = 0;
        }
        if (f) {
            std::fflush(stderr);
            if (std::fseek(f, 0, SEEK_END) == 0) {
                const long size = std::ftell(f);
                if (size < offset) offset = 0;  // the file was truncated: a new boot, start over
                if (size > offset && std::fseek(f, offset, SEEK_SET) == 0) {
                    long left = size - offset;
                    while (left > 0 && g_pumpRun.load()) {
                        const size_t want = size_t(std::min<long>(left, (long)buf.size()));
                        const size_t got = std::fread(buf.data(), 1, want, f);
                        if (!got) break;
                        debugsrv::log_tap(buf.data(), got);
                        offset += (long)got;
                        left -= (long)got;
                    }
                }
            }
        }
        // 100 ms between passes, in slices so stop() joins at once
        for (int i = 0; i < 10 && g_pumpRun.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (f) std::fclose(f);
}

void ThreadStart() {
#ifdef __SWITCH__
    // The lowest priority the port allows (dk.h): the server's threads only run in the
    // game's gaps, even while a client is streaming the log at them.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
#endif
}

}  // namespace
}  // namespace SpmDebugServer

extern "C" void spm_debug_server_boot(bool enabled) {
    using namespace SpmDebugServer;
    if (!enabled || g_booted.exchange(true)) return;
#ifdef __SWITCH__
    g_root = "/switch/spm-nx";
#else
    const char* e = std::getenv("SPM_DEBUG_ROOT");
    g_root = e && *e ? e : "/switch/spm-nx";
#endif
    // The pump replays the log file from its start, so the stream covers the boot too;
    // keep_log covers the window between here and start().
    debugsrv::keep_log();
    debugsrv::add_command("info", "info                   game, place, sequence, present count", &CmdInfo);
    debugsrv::add_command("diag", "diag                   every dk_diag switch as key=value", &CmdDiag);
    debugsrv::add_command("set", "set <key> <0|1>        flip a dk_diag switch (diag set; diag get reads)", &CmdSet);
    debugsrv::add_command("flush", "flush                  flush the log buffers to stderr.log now", &CmdFlush);
    debugsrv::add_command("mark", "mark <text>            a [diag] mark line in the log, then a flush", &CmdMark);
    debugsrv::add_command("warp", "warp <map> [door]      warp on the next frame in a map (the Debug warp list)", &CmdWarp);
    debugsrv::add_command("place", "place                  where the game is (map and 2D/3D, or the sequence)", &CmdPlace);
    debugsrv::add_command("shot", "shot                   not in this build (no frame capture yet)", &CmdShot);
    debugsrv::add_command("peek", "peek <addr> [bytes]    not in this build", &CmdPeek);
    debugsrv::add_command("poke", "poke <addr> <word>     not in this build", &CmdPoke);
    debugsrv::add_command("quit", "quit                   not in this build", &CmdQuit);
    debugsrv::add_command("reload", "reload                 not in this build", &CmdReload);

    debugsrv::Config config;
    config.port = 6543;
    config.root = g_root;
    config.log = &LogLine;
    config.threadStart = &ThreadStart;
    if (!debugsrv::start(config)) {
        debugsrv::drop_log();
        return;
    }
    LogLine("[server] debug server listening on port 6543: dev builds only, local network, NO "
            "password (anyone who can reach the port can flip diag switches, move files under "
            "the root and press buttons)");
    // A console under test is left alone for minutes at a time; HD's server does the same.
    spm_switch_keep_awake(true);
    g_pumpRun = true;
    g_pump = std::thread(&PumpMain);
}

extern "C" void spm_debug_server_stop(void) {
    using namespace SpmDebugServer;
    if (!g_booted.load()) return;
    g_pumpRun = false;
    if (g_pump.joinable()) g_pump.join();
    debugsrv::stop();
    spm_switch_keep_awake(false);
}

#ifdef __SWITCH__
// The SDL gamepad shim's input poll (switch/sdl3_shim/sdl3_shim_gamepad.c) calls this
// through a weak declaration, inherited from SwitchWaker: the injected presses and
// sticks as if from the controller, and while the server runs the controller counts as
// connected even with none attached. SwitchWaker's provider (cos_debug.cpp) is these
// same lines. This function must stay in this translation unit: spm_debug_server_boot
// above is what pulls the unit into the link (device.cpp references it), so the strong
// symbol is there for the shim's weak call whenever the server code is.
extern "C" int cos_switch_debug_input(uint64_t* buttons, int32_t sticks[4]) {
    if (!debugsrv::running()) return 0;
    const debugsrv::Injected in = debugsrv::injected();
    *buttons |= in.buttons;
    for (int s = 0; s < 2; s++) {
        if (in.stick[s]) {
            sticks[2 * s] = int32_t(in.x[s] * JOYSTICK_MAX);
            sticks[2 * s + 1] = int32_t(in.y[s] * JOYSTICK_MAX);
        }
    }
    return 1;
}
#endif
