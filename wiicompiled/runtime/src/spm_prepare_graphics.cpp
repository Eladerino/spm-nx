// Prepare graphics (spm_prepare_graphics.h).
//
// One state machine on the game thread, driven by SpmMapSweep::Tick: ask for a warp to the next
// room (SpmMapSweep::RequestWarp, the Debug menu's own warp), wait for the map change to finish,
// give the room a moment to draw what it draws, then wait until Aurora's pipeline queue has been
// empty for a second. Then flip Mario to 3D (a synthetic press of Wii A, the button a player
// flips with) and wait for the queue again, because the flipped view draws through pipelines
// the 2D view never touches. The design follows SwitchWaker's Prepare graphics (warp, wait for
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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
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
constexpr double kFlipTimeout = 1.0;     // s for the game to report 3D after the flip press before the room goes on unflipped
constexpr double kFlipDrainCap = 5.0;    // s at most draining in 3D: the flip meter runs about 10 s from full, and
                                         // Mario has been in 2D (where it refills) for the whole visit, so the press
                                         // poll plus this cap stay inside it without touching the player's cheat setting
constexpr int kMaxSkipsInARow = 3;       // warps the game would not take, then the sweep stops

enum class Phase { Idle, Waiting, Request, Arrive, Settle, Drain, FlipSettle, FlipDrain, Finish };

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
bool g_flipTried = false;  // this room's flip press has been sent (or found needless)
bool g_complete = false;
// The flip pulse, shared with the KPAD read (another thread): which presented frame it started
// on, and whether one is armed at all. Tick refreshes g_presented every frame.
std::atomic<uint32_t> g_presented{0};
std::atomic<uint32_t> g_flipPulseAt{0};
std::atomic<bool> g_flipPulse{false};
Clock::time_point g_phaseAt{}, g_quietSince{}, g_runStart{}, g_inMapSince{}, g_holdSince{}, g_messageAt{};

fs::path Dir() { return RuntimeConfigFile::ApplicationDataDirectory() / "PrepareGraphics"; }
fs::path BackupDir() { return Dir() / "save"; }
fs::path BackupDone() { return BackupDir() / "complete.txt"; }  // written last: a backup without it is not used
fs::path NandTitle() { return RuntimeNandPath::DiscoverNandRootPath() / "title"; }

bool Exists(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }
// Checked small-file write: the markers this file runs on (state, backup complete) used to be
// written unchecked, so a failed write could certify work that never landed.
bool Touch(const fs::path& p, const std::string& text = "1") {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(p, std::ios::trunc);
    if (!out) return false;
    out << text << "\n";
    out.flush();
    return out.good();
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
// so putting it back is a safety net: files in the copy replace what is there, and files the copy
// does not have are left alone. Both directions go through the same walk (CopyTree), because this
// filesystem will not copy a directory onto a directory that already exists: run 1's put back
// failed "File exists" on hardware even with overwrite_existing. Directories are therefore only
// created when missing, every file is removed then copied one by one, and every step is checked.
struct TreeEntry {
    std::string rel;  // path below the tree's root, generic separators
    std::uintmax_t size = 0;
    bool operator==(const TreeEntry& o) const { return rel == o.rel && size == o.size; }
};

bool ListTree(const fs::path& root, std::vector<TreeEntry>& out) {
    out.clear();
    std::error_code ec;
    if (!Exists(root)) return true;  // no tree: an empty listing, not an error
    fs::recursive_directory_iterator it(root, ec), end;
    if (ec) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] could not list %s: %s\n", root.string().c_str(), ec.message().c_str());
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            RT_LOGF(RT_TAG_RUNTIME, "[prepare] could not list %s: %s\n", root.string().c_str(), ec.message().c_str());
            return false;
        }
        std::error_code tec;
        if (!it->is_regular_file(tec) || tec) continue;
        TreeEntry e;
        e.rel = it->path().lexically_relative(root).generic_string();
        e.size = it->file_size(tec);
        if (tec || e.rel.empty()) {
            RT_LOGF(RT_TAG_RUNTIME, "[prepare] could not read %s\n", it->path().string().c_str());
            return false;
        }
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const TreeEntry& a, const TreeEntry& b) { return a.rel < b.rel; });
    return true;
}

// Copies the tree at src onto dst, one entry at a time (see the note above). A failure logs the
// path that failed and stops; whatever was already copied stays copied.
bool CopyTree(const fs::path& src, const fs::path& dst, const char* what) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    std::error_code dec;
    if (!fs::is_directory(dst, dec)) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s failed at %s: %s\n", what, dst.string().c_str(), ec.message().c_str());
        return false;
    }
    fs::recursive_directory_iterator it(src, ec), end;
    if (ec) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s failed at %s: %s\n", what, src.string().c_str(), ec.message().c_str());
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s failed in %s: %s\n", what, src.string().c_str(), ec.message().c_str());
            return false;
        }
        const fs::path dest = dst / it->path().lexically_relative(src);
        std::error_code tec;
        if (it->is_directory(tec)) {
            fs::create_directories(dest, tec);  // an already existing directory is fine, not an error
            std::error_code xec;
            if (!fs::is_directory(dest, xec)) {
                RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s failed at %s: %s\n", what, dest.string().c_str(), tec.message().c_str());
                return false;
            }
        } else if (it->is_regular_file(tec)) {
            fs::create_directories(dest.parent_path(), tec);
            if (tec) {
                RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s failed at %s: %s\n", what, dest.string().c_str(), tec.message().c_str());
                return false;
            }
            std::error_code xec;
            if (fs::is_directory(dest, xec)) {
                RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s failed at %s: a folder is in the way\n", what,
                        dest.string().c_str());
                return false;
            }
            if (Exists(dest)) {
                fs::remove(dest, xec);
                if (xec) {
                    RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s failed at %s: %s\n", what, dest.string().c_str(),
                            xec.message().c_str());
                    return false;
                }
            }
            fs::copy_file(it->path(), dest, xec);
            if (xec) {
                RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s failed at %s: %s\n", what, dest.string().c_str(),
                        xec.message().c_str());
                return false;
            }
        }
    }
    return true;
}

// Deletes the tree at root and reports whether it is really gone. remove_all is tried first;
// if it errors or the tree is still there afterwards, the tree is walked and deleted entry by
// entry, deepest first (a child's path string extends its parent's, so sorting by length,
// longest first, visits children before their folders). The Switch FAT layer let a remove_all
// fail silently under the old unchecked call in RestoreSave, so nothing in this file trusts
// the call alone any more: the result is always verified with Exists().
bool DeleteTree(const fs::path& root) {
    if (!Exists(root)) return true;
    std::error_code ec;
    fs::remove_all(root, ec);
    if (!Exists(root)) return true;
    std::vector<fs::path> entries;
    std::error_code iec;
    fs::recursive_directory_iterator it(root, iec), end;
    if (!iec) {
        for (; it != end; it.increment(iec)) {
            if (iec) break;
            entries.push_back(it->path());
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const fs::path& a, const fs::path& b) { return a.string().size() > b.string().size(); });
    for (const fs::path& p : entries) {
        std::error_code rec;
        fs::remove(p, rec);
    }
    std::error_code rec;
    fs::remove(root, rec);
    return !Exists(root);
}

// Best-effort tree deletion for the cleanups in BackupSave: a failed deletion there is not
// itself the safety mechanism (the complete marker, written last, is), but it used to pass
// silently, so it is logged with what was being cleaned up.
void RemoveTreeLogged(const fs::path& root, const char* what) {
    if (!Exists(root)) return;
    std::error_code ec;
    fs::remove_all(root, ec);
    if (ec || Exists(root))
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: could not delete %s\n", what, root.string().c_str());
}

fs::path ManifestPath() { return Dir() / "manifest.txt"; }

// The manifest is the backup's file list (path and size of every file, relative to the title
// folder), kept next to the backup. It is what lets a later start tell the sweep's own save
// apart from a save the player has played on since the sweep.
bool WriteManifest(const std::vector<TreeEntry>& entries) {
    std::string text;
    for (const TreeEntry& e : entries) {
        text += e.rel;
        text += '\t';
        text += std::to_string(e.size);
        text += '\n';
    }
    return Touch(ManifestPath(), text);
}

// False when there is no manifest to read or it does not parse; the caller tells "none" apart
// from "unreadable" with Exists().
bool ReadManifest(std::vector<TreeEntry>& out) {
    std::ifstream in(ManifestPath());
    if (!in) return false;
    out.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t tab = line.rfind('\t');
        if (tab == std::string::npos) return false;
        TreeEntry e;
        e.rel = line.substr(0, tab);
        try {
            e.size = std::stoull(line.substr(tab + 1));
        } catch (...) {
            return false;
        }
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const TreeEntry& a, const TreeEntry& b) { return a.rel < b.rel; });
    return true;
}

std::string TimestampSuffix() {
    char buf[32] = {};
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    if (t == static_cast<std::time_t>(-1) || std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tmv) == 0) return {};
    return buf;
}

// The live save no longer matches the copy the sweep made, so putting the copy back would erase
// play the sweep never saw. The copy is renamed aside instead of restored, restore marker and
// all, so no later start tries again.
void SetBackupAside() {
    std::string name = "save-diverged";
    const std::string stamp = TimestampSuffix();
    if (!stamp.empty()) name += "-" + stamp;
    fs::path target = Dir() / name;
    for (int n = 2; Exists(target); ++n) target = Dir() / (name + "-" + std::to_string(n));
    std::error_code ec;
    fs::rename(BackupDir(), target, ec);
    if (ec) {
        // Even stranded in place, the copy must never be restored by a later start: drop its marker.
        Remove(BackupDone());
        RT_LOGF(RT_TAG_RUNTIME,
                "[prepare] the save changed since the sweep, and its copy could not be moved aside (%s); it stays in %s and will not be put back\n",
                ec.message().c_str(), BackupDir().string().c_str());
        return;
    }
    if (Exists(ManifestPath())) {
        std::error_code mec;
        fs::rename(ManifestPath(), Dir() / (target.filename().string() + "-manifest.txt"), mec);
    }
    RT_LOGF(RT_TAG_RUNTIME,
            "[prepare] the save changed since the sweep, so the sweep copy was NOT put back over it; the copy is kept in %s\n",
            target.string().c_str());
}

bool BackupSave() {
    if (Exists(BackupDone())) {
        // A cut sweep's copy is still the original one. Give it a manifest if it predates them,
        // so the boot restore can still spot a save played on since.
        if (!Exists(ManifestPath())) {
            std::vector<TreeEntry> entries;
            if (ListTree(BackupDir() / "title", entries)) {
                if (!WriteManifest(entries))
                    RT_LOGF(RT_TAG_RUNTIME, "[prepare] could not write the save manifest; the copy is still used\n");
            } else {
                // ListTree has already logged why the listing failed; this line records the
                // consequence, so a copy without a manifest is never a silent state (the boot
                // restore's diverged-save check cannot run without one).
                RT_LOGF(RT_TAG_RUNTIME, "[prepare] the save copy has no manifest and none could be written; the copy is still used\n");
            }
        }
        return true;
    }
    std::error_code ec;
    RemoveTreeLogged(BackupDir(), "save copy");
    Remove(ManifestPath());
    fs::create_directories(BackupDir(), ec);
    if (ec) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] save copy failed: %s\n", ec.message().c_str());
        return false;
    }
    std::vector<TreeEntry> source;
    if (!ListTree(NandTitle(), source)) return false;
    if (Exists(NandTitle()) && !CopyTree(NandTitle(), BackupDir() / "title", "save copy")) {
        RemoveTreeLogged(BackupDir(), "save copy");  // a partial copy is never left for a restore to find
        return false;
    }
    // The copy is certified only once every file in it matches the save it came from, in size;
    // the complete marker is written last, and only then.
    std::vector<TreeEntry> copied;
    if (!ListTree(BackupDir() / "title", copied) || copied != source) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] save copy did not verify; the partial copy is deleted\n");
        RemoveTreeLogged(BackupDir(), "save copy");
        return false;
    }
    if (!WriteManifest(source)) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] could not write the save manifest; the copy is deleted\n");
        RemoveTreeLogged(BackupDir(), "save copy");
        return false;
    }
    if (!Touch(BackupDone())) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] could not mark the save copy complete; the copy is deleted\n");
        RemoveTreeLogged(BackupDir(), "save copy");
        return false;
    }
    return true;
}

enum class RestoreResult { None, Restored, Failed, Diverged };

// fromBoot is the pending restore at the start of a session: the one place a save played on
// since the sweep can be met, so it is the one place the manifest is checked before anything
// is overwritten. The end-of-run restore is not checked on purpose: undoing whatever the warps
// themselves saved is its whole job.
RestoreResult RestoreSave(bool fromBoot) {
    if (!Exists(BackupDone())) return RestoreResult::None;
    if (fromBoot && Exists(ManifestPath()) && Exists(NandTitle())) {
        std::vector<TreeEntry> manifest, live;
        const bool checked = ReadManifest(manifest) && ListTree(NandTitle(), live);
        if (!checked || live != manifest) {
            SetBackupAside();
            return RestoreResult::Diverged;
        }
    }
    if (Exists(BackupDir() / "title") && !CopyTree(BackupDir() / "title", NandTitle(), "save put back")) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] save put back failed: the copy stays in %s\n", BackupDir().string().c_str());
        return RestoreResult::Failed;
    }
    // The restore landed, so this copy must never be put back again. The complete marker is
    // what a later start keys on, so it is removed first, ahead of the tree, and what remains
    // is checked rather than assumed: run 3 restored successfully at boot and at the end of
    // the run, but the old code deleted the tree with an unchecked remove_all that failed
    // silently on this filesystem, the marker survived with the copy, and the next start
    // would have put the same stale copy back again (with no manifest, the diverged-save
    // check above could not have caught it). DeleteTree's walk gets a second attempt at the
    // marker as part of the tree. If anything is left behind, one line says what state it is
    // in; files without their marker are never restored.
    std::error_code mec;
    fs::remove(BackupDone(), mec);
    Remove(ManifestPath());
    const bool treeGone = DeleteTree(BackupDir());
    const bool markerGone = !Exists(BackupDone());
    if (!treeGone || !markerGone) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] the save was put back, but its copy in %s could not be fully deleted; %s\n",
                BackupDir().string().c_str(),
                markerGone ? "its complete marker is gone, so it will not be put back again"
                           : "its complete marker could not be removed either, so a later start may put it back");
    }
    if (!fromBoot) RT_LOGF(RT_TAG_RUNTIME, "[prepare] save put back\n");
    return RestoreResult::Restored;
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
    g_flipPulse = false;
    // The rows this run recorded reach the pipeline database through a queued writer thread,
    // while the compiled shaders were already written as they compiled: an end that skips the
    // queue (the restart below, or the process dying) keeps the shaders and loses the rows the
    // next start would have prewarmed from. Every ending funnels through here, so the queue is
    // drained here, before anything else in this function can end the process, and the result
    // is logged either way: a run whose flush failed is not the recording it claims to be.
    if (aurora_flush_pipeline_cache_writes()) {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] pipeline cache flushed\n");
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "[prepare] pipeline cache flush failed; some rows from this run may not be recorded\n");
    }
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
#if defined(__SWITCH__)
        // The restart is attempted before anything touches the save copy: the copy is only ever
        // deleted by a restore that succeeded, and a restart (or the manual close the message
        // asks for) leaves the pending restore for the next start, where it runs before any
        // save is loaded. If there is no restart from here (applet mode), the copy is kept for
        // that start too, instead of being spent where its landing cannot be confirmed.
        std::fflush(nullptr);
        if (!spm_switch_restart_program()) {
            RT_LOGF(RT_TAG_RUNTIME, "[prepare] no restart from here; the save copy stays in %s for the next start\n",
                    BackupDir().string().c_str());
            SetMessage("A room did not load. Close the game and open it again, then load your save to carry on.");
        }
#else
        RestoreSave(false);
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
    g_status.running = g_phase >= Phase::Request && g_phase <= Phase::FlipDrain;
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

// The room is visited in full (both dimensions drained, or the flip settled as far as this
// room allows): count it and move on. The index is written here, not at the first drain, so a
// run stopped mid-flip does this room again next time rather than skipping its flipped view.
void FinishRoom() {
    g_flipPulse = false;
    ++g_doneThisRun;
    ++g_index;
    WriteIndex(g_index);
    Enter(Phase::Request);
}

void Step(CpuContext* cpu) {
    const int seq = SpmMapSweep::Seq();
    if (seq == 2) {
        if (g_inMapSince == Clock::time_point{}) g_inMapSince = Clock::now();
    } else {
        g_inMapSince = {};
    }
    const bool running = g_phase >= Phase::Request && g_phase <= Phase::FlipDrain;

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
        g_flipTried = false;
        Enter(Phase::FlipSettle);
        return;
    }
    case Phase::FlipSettle: {
        const Room& r = *g_list[g_index];
        if (seq != 2) {  // the game left the room mid-visit: nothing to flip here any more
            RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: flip skipped, the game left the room\n", r.map);
            FinishRoom();
            return;
        }
        if (SpmMapSweep::MarioIn3d(cpu)) {
            RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: %s\n", r.map, g_flipTried ? "flipped to 3D" : "already in 3D");
            g_quietSince = {};
            Enter(Phase::FlipDrain);
            return;
        }
        if (!g_flipTried) {
            g_flipTried = true;
            // The press is only a flip when Mario can take it as one: another character's A is
            // their ability (Luigi's charged jump can move him), and without control the press
            // lands in whatever event or message owns him instead. Both are checked with the
            // game's own state, the same reads the warp service's arrival placement waits on.
            const int character = SpmMapSweep::MarioCharacter(cpu);
            if (character < 0) {
                RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: flip skipped, Mario could not be checked\n", r.map);
                FinishRoom();
                return;
            }
            if (character != 0) {
                RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: flip skipped, not playing as Mario\n", r.map);
                FinishRoom();
                return;
            }
            if (!SpmMapSweep::MarioHasControl(cpu)) {
                RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: flip skipped, Mario does not have control\n", r.map);
                FinishRoom();
                return;
            }
            g_flipPulseAt = g_presented.load();
            g_flipPulse = true;
            return;
        }
        if (Seconds(g_phaseAt) >= kFlipTimeout) {
            // Locked here, blocked, or the room forbids it: the attempt is the probe, and the
            // room simply goes on unflipped, at the cost of this timeout only.
            g_flipPulse = false;
            RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: flip did not take, going on in 2D\n", r.map);
            FinishRoom();
        }
        return;
    }
    case Phase::FlipDrain: {
        if (seq != 2) {  // the draws queued so far still drain in the background; the room is over either way
            FinishRoom();
            return;
        }
        const bool quiet = aurora_get_queued_pipeline_count() == 0;
        if (!quiet) g_quietSince = {};
        else if (g_quietSince == Clock::time_point{}) g_quietSince = Clock::now();
        const bool done = quiet && Seconds(g_quietSince) >= kQuiet;
        if (!done && Seconds(g_phaseAt) < kFlipDrainCap) return;
        if (!done) RT_LOGF(RT_TAG_RUNTIME, "[prepare] %s: still compiling in 3D after %.0f s, going on\n", g_list[g_index]->map, kFlipDrainCap);
        FinishRoom();
        return;
    }
    case Phase::Finish:
        if (g_restoreAtTitle && seq == 1) {
            g_restoreAtTitle = false;
            RestoreSave(false);
            Enter(Phase::Idle);
        } else if (Seconds(g_phaseAt) > 30.0) {  // the title never came: the copy is put back at the next start
            g_restoreAtTitle = false;
            Enter(Phase::Idle);
        }
        return;
    }
}

}  // namespace

void Tick(CpuContext* cpu, uint32_t presentedFrames) {
    if (!cpu) return;
    g_presented = presentedFrames;
    static bool started = false;
    if (!started) {
        started = true;
        g_list = BuildList();
        g_complete = Exists(Dir() / "complete.txt");
        g_index = std::min(ReadIndex(), g_list.size());  // the menu row shows where a stopped sweep continues
        // A sweep the game closed during: its save copy goes back now, before any save is loaded.
        if (RestoreSave(true) == RestoreResult::Restored) RT_LOGF(RT_TAG_RUNTIME, "[prepare] save restored\n");
        if (Exists(Dir() / "resume.txt")) Enter(Phase::Waiting);
    }
    Step(cpu);
    PublishStatus();
}

uint32_t FlipAutoPressBits() {
    if (!g_flipPulse.load()) return 0;
    // Four presented frames down, then off for good: a tap long enough for the game's own
    // input reads to catch between frames (the startup cover's auto-press uses the same four),
    // and a single tap, so a flip the game applies late is never flipped back by a second one.
    return g_presented.load() - g_flipPulseAt.load() < 4 ? 0x0800u : 0u;
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
