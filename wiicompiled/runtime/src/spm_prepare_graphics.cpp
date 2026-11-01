// Prepare graphics (spm_prepare_graphics.h).
//
// One state machine on the game thread, driven by SpmMapSweep::Tick: ask for a warp to the next
// room (SpmMapSweep::RequestWarp, the Debug menu's own warp), wait for the map change to finish,
// give the room a moment to draw what it draws, then wait until Aurora's pipeline queue has been
// empty for a second, and go on. The design follows SwitchWaker's Prepare graphics (warp, wait for
// the shader worker to go idle, keep the save safe, resumable, B to stop); the code is this
// port's own.
#include "spm_prepare_graphics.h"

#include "runtime_config.h"
#include "runtime_log.h"
#include "nand_path.h"
#include "spm_map_sweep.h"

#include <aurora/gfx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#if defined(__SWITCH__)
extern "C" bool spm_switch_restart_program(void);   // switch/source/spm_switch_init.cpp
extern "C" void spm_switch_keep_awake(bool on);
#endif

namespace SpmPrepareGraphics {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Room { const char* map; const char* door; const char* group; const char* label; };
constexpr Room kRooms[] = {
#include "spm_warp_rooms.inc"
};

constexpr double kArriveTimeout = 45.0;  // s from the request to the new map running
constexpr double kSettle = 2.0;          // s in the room before the queue is watched (its draws meet the pipelines)
constexpr double kQuiet = 1.0;           // s of an empty pipeline queue that make a room done
constexpr double kRoomTimeout = 60.0;    // s at most in one room
constexpr double kHoldToStop = 1.5;      // s of B held
constexpr double kWaitForGame = 3.0;     // s in a map before a waiting sweep begins
constexpr int kMaxSkipsInARow = 3;       // warps the game would not take, then the sweep stops

enum class Phase { Idle, Waiting, Request, Arrive, Settle, Drain, Finish };

std::mutex g_mu;  // g_status (read by the UI thread)
Status g_status;
std::atomic<bool> g_stop{false}, g_startRequest{false}, g_stopHeld{false};
std::atomic<float> g_hold{0.0f};

Phase g_phase = Phase::Idle;
std::vector<const Room*> g_list;
size_t g_index = 0;
size_t g_doneThisRun = 0;
int g_skipsInARow = 0;
bool g_sawChange = false;
bool g_complete = false;
Clock::time_point g_phaseAt{}, g_quietSince{}, g_runStart{}, g_inMapSince{}, g_holdSince{}, g_messageAt{};

fs::path Dir() { return RuntimeConfigFile::ApplicationDataDirectory() / "PrepareGraphics"; }
fs::path BackupDir() { return Dir() / "save"; }
fs::path BackupDone() { return BackupDir() / "complete.txt"; }  // written last: a backup without it is not used
fs::path NandTitle() { return RuntimeNandPath::DiscoverNandRootPath() / "title"; }

bool Exists(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }
void Touch(const fs::path& p, const std::string& text = "1") {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::trunc) << text << "\n";
}
void Remove(const fs::path& p) { std::error_code ec; fs::remove(p, ec); }

size_t ReadIndex() {
    std::ifstream in(Dir() / "state.txt");
    size_t i = 0;
    return in >> i ? i : 0;
}
void WriteIndex(size_t i) { Touch(Dir() / "state.txt", std::to_string(i)); }

double Seconds(Clock::time_point since) { return std::chrono::duration<double>(Clock::now() - since).count(); }

std::vector<const Room*> BuildList() {
    std::vector<const Room*> out;
    std::set<std::string> seen;
    for (const Room& r : kRooms)
        if (seen.insert(r.map).second) out.push_back(&r);
    return out;
}

std::string CurrentMap() {
    char place[48] = {};
    SpmMapSweep::CurrentPlace(place, sizeof place);
    std::string s = place;
    const size_t space = s.find(' ');
    return space == std::string::npos ? s : s.substr(0, space);
}

void SetMessage(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_status.message = text;
    g_messageAt = Clock::now();
    RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s\n", text.c_str());
}

// The save folder (the NAND's title tree) is copied before the first warp. The sweep never saves,
// so putting it back is a safety net: files are copied over what is there, and nothing is deleted.
bool BackupSave() {
    if (Exists(BackupDone())) return true;  // a cut sweep's copy is still the original one
    std::error_code ec;
    fs::remove_all(BackupDir(), ec);
    fs::create_directories(BackupDir(), ec);
    if (Exists(NandTitle())) {
        fs::copy(NandTitle(), BackupDir() / "title", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            RT_LOGF(RT_TAG_RUNTIME, "[prepare] save copy failed: %s\n", ec.message().c_str());
            return false;
        }
    }
    Touch(BackupDone());
    return true;
}

bool RestoreSave() {
    if (!Exists(BackupDone())) return false;
    std::error_code ec;
    if (Exists(BackupDir() / "title"))
        fs::copy(BackupDir() / "title", NandTitle(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] save put back failed (%s): the copy stays in %s\n", ec.message().c_str(),
                BackupDir().string().c_str());
        return false;
    }
    fs::remove_all(BackupDir(), ec);
    RT_LOGF(RT_TAG_RUNTIME, "[prepare] save put back\n");
    return true;
}

void Quiet(bool on) {
    SpmMapSweep::SetInfiniteHp(on);  // enemies cannot end the sweep with a game over
#if defined(__SWITCH__)
    spm_switch_keep_awake(on);       // nobody touches the console for the better part of an hour
#endif
}

void Enter(Phase p) {
    g_phase = p;
    g_phaseAt = Clock::now();
}

void BeginRun() {
    if (!BackupSave()) {
        SetMessage("Prepare graphics could not copy your save, so it did not start.");
        Enter(Phase::Idle);
        return;
    }
    Remove(Dir() / "resume.txt");
    g_index = ReadIndex();
    if (g_index >= g_list.size()) g_index = 0;
    g_doneThisRun = 0;
    g_skipsInARow = 0;
    g_runStart = Clock::now();
    g_stop = false;
    Quiet(true);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.message.clear();
    }
    RT_LOGF(RT_TAG_RUNTIME, "[prepare] start at %zu of %zu\n", g_index + 1, g_list.size());
    Enter(Phase::Request);
}

// The end of a run: complete, stopped, or stuck. The game goes back to the title screen (a stuck
// map change cannot, so the program restarts) and the save is put back once it is there.
bool g_restoreAtTitle = false;
void EndRun(const char* why, bool complete, bool stuck) {
    Quiet(false);
    RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: %zu rooms this run in %.0f min, next %zu of %zu\n", why, g_doneThisRun,
            Seconds(g_runStart) / 60.0, g_index + 1, g_list.size());
    if (complete) {
        g_complete = true;
        WriteIndex(0);
        Touch(Dir() / "complete.txt");
        SetMessage("Graphics are ready. Effects should no longer stutter the first time you see them.");
    } else {
        WriteIndex(g_index);
        SetMessage(stuck ? "A room did not load, so Prepare graphics stopped. Load your save to carry on from the next room."
                         : "Prepare graphics stopped. It carries on from here next time.");
    }
    if (stuck) {
        Touch(Dir() / "resume.txt");  // starts again by itself once a save is loaded
        RestoreSave();
        std::fflush(nullptr);
#if defined(__SWITCH__)
        if (!spm_switch_restart_program())
            SetMessage("A room did not load. Close the game and open it again, then load your save to carry on.");
#endif
        Enter(Exists(Dir() / "resume.txt") ? Phase::Waiting : Phase::Idle);
        return;
    }
    SpmMapSweep::ReturnToTitle();
    g_restoreAtTitle = true;
    Enter(Phase::Finish);
}

void PublishStatus() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_status.running = g_phase >= Phase::Request && g_phase <= Phase::Drain;
    g_status.waiting = g_phase == Phase::Waiting;
    g_status.complete = g_complete;
    g_status.total = int(g_list.size());
    g_status.place = int(std::min(g_index + 1, g_list.size()));
    g_status.room = g_index < g_list.size() ? g_list[g_index]->label : "";
    g_status.minutesLeft = -1;
    if (g_status.running && g_doneThisRun >= 5) {
        const double perRoom = Seconds(g_runStart) / double(g_doneThisRun);
        g_status.minutesLeft = std::max(1, int(perRoom * double(g_list.size() - g_index) / 60.0 + 0.5));
    }
    g_status.stopHold = g_hold.load();
    if (!g_status.message.empty() && !g_status.running && Seconds(g_messageAt) > 15.0) g_status.message.clear();
}

void Step() {
    const int seq = SpmMapSweep::Seq();
    if (seq == 2) {
        if (g_inMapSince == Clock::time_point{}) g_inMapSince = Clock::now();
    } else {
        g_inMapSince = {};
    }
    const bool running = g_phase >= Phase::Request && g_phase <= Phase::Drain;

    if (g_startRequest.exchange(false) && !running && g_phase != Phase::Finish) Enter(Phase::Waiting);
    if (running && g_stop.exchange(false)) {
        EndRun("stopped", false, false);
        return;
    }

    switch (g_phase) {
    case Phase::Idle:
        g_stop = false;
        return;
    case Phase::Waiting:
        if (g_stop.exchange(false)) {
            Remove(Dir() / "resume.txt");
            Enter(Phase::Idle);
            return;
        }
        if (seq == 2 && Seconds(g_inMapSince) >= kWaitForGame && g_inMapSince != Clock::time_point{}) BeginRun();
        return;
    case Phase::Request: {
        if (g_index >= g_list.size()) {
            EndRun("complete", true, false);
            return;
        }
        const Room& r = *g_list[g_index];
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] %zu/%zu %s\n", g_index + 1, g_list.size(), r.map);
        SpmMapSweep::RequestWarp(r.map, r.door ? r.door : "", nullptr);
        g_sawChange = false;
        Enter(Phase::Arrive);
        return;
    }
    case Phase::Arrive: {
        g_sawChange = g_sawChange || seq != 2;
        if (g_sawChange && seq == 2) {
            const std::string actual = CurrentMap();
            if (actual != g_list[g_index]->map)
                RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: the game went to %s instead\n", g_list[g_index]->map, actual.c_str());
            g_skipsInARow = 0;
            Enter(Phase::Settle);
            return;
        }
        if (Seconds(g_phaseAt) < kArriveTimeout) return;
        if (g_sawChange) {
            // Stuck inside the map change: later warps would wait for it forever.
            RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: stuck in the map change (seq %d)\n", g_list[g_index]->map, seq);
            ++g_index;
            EndRun("stuck", false, true);
            return;
        }
        // The game never took the warp (it was busy in an event): skip the room.
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: the warp was not taken, skipped\n", g_list[g_index]->map);
        ++g_index;
        WriteIndex(g_index);
        if (++g_skipsInARow >= kMaxSkipsInARow) {
            EndRun("the game stopped taking warps", false, false);
            return;
        }
        Enter(Phase::Request);
        return;
    }
    case Phase::Settle:
        if (Seconds(g_phaseAt) >= kSettle) {
            g_quietSince = {};
            Enter(Phase::Drain);
        }
        return;
    case Phase::Drain: {
        const bool quiet = aurora_get_queued_pipeline_count() == 0;
        if (!quiet) g_quietSince = {};
        else if (g_quietSince == Clock::time_point{}) g_quietSince = Clock::now();
        const bool done = quiet && Seconds(g_quietSince) >= kQuiet;
        if (!done && Seconds(g_phaseAt) < kRoomTimeout) return;
        if (!done) RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: still compiling after %.0f s, going on\n", g_list[g_index]->map, kRoomTimeout);
        ++g_doneThisRun;
        ++g_index;
        WriteIndex(g_index);
        Enter(Phase::Request);
        return;
    }
    case Phase::Finish:
        if (g_restoreAtTitle && seq == 1) {
            g_restoreAtTitle = false;
            RestoreSave();
            Enter(Phase::Idle);
        } else if (Seconds(g_phaseAt) > 30.0) {  // the title never came: the copy is put back at the next start
            g_restoreAtTitle = false;
            Enter(Phase::Idle);
        }
        return;
    }
}

}  // namespace

void Tick(CpuContext* cpu, uint32_t) {
    if (!cpu) return;
    static bool started = false;
    if (!started) {
        started = true;
        g_list = BuildList();
        g_complete = Exists(Dir() / "complete.txt");
        g_index = std::min(ReadIndex(), g_list.size());  // the menu row shows where a stopped sweep continues
        // A sweep the game closed during: its save copy goes back now, before any save is loaded.
        if (RestoreSave()) RT_LOGF(RT_TAG_RUNTIME, "[prepare] a sweep was cut short: save put back\n");
        if (Exists(Dir() / "resume.txt")) Enter(Phase::Waiting);
    }
    Step();
    PublishStatus();
}

void Start() { g_startRequest = true; }
void Stop() { g_stop = true; }

void NoteStopButton(bool held) {
    if (!held) {
        g_holdSince = {};
        g_hold = 0.0f;
        return;
    }
    if (g_holdSince == Clock::time_point{}) g_holdSince = Clock::now();
    const double t = Seconds(g_holdSince) / kHoldToStop;
    g_hold = float(t > 1.0 ? 1.0 : t);
    if (t >= 1.0) Stop();
}

Status GetStatus() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_status;
}

std::atomic<int> g_offered{-1};  // -1: not looked up yet

bool ShouldOffer() {
    if (g_offered < 0) g_offered = Exists(Dir() / "offered.txt") || Exists(Dir() / "complete.txt") ? 1 : 0;
    return g_offered == 0;
}

void MarkOffered() {
    g_offered = 1;
    Touch(Dir() / "offered.txt");
}

}  // namespace SpmPrepareGraphics
