extern "C" unsigned g_spmAudit[8];
extern "C" unsigned g_spmGameClear;
extern "C" int g_spmSweepDump;
#include "spm_map_sweep.h"
#include "spm_prepare_graphics.h"
#include "abi_bridge.h"
#include "memory.h"
#include "mod_runtime.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <dolphin/gx/GXAurora.h>

#include <tuple>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <string>
#include <vector>

namespace SpmMapSweep {
namespace {
using Clock = std::chrono::steady_clock;
// Diagnostic (env SPM_MTX_SELFTEST): runs the game's own matrix/vector routines through the recompiled code and compares
// with plain C++ math. Any "MISMATCH" line names a routine whose recompiled result is wrong.
// SPM_MATH_SELFTEST=1: recompiled math helpers vs host math, logged as [mathtest] (movement debugging).
void RunMathSelfTest(CpuContext* cpu) {
    CpuContext a = *cpu;
    a.gpr[3] = 0;
    a.gpr[4] = 64;
    InvokeIndirectCpu(0x801A5634, &a);  // __memAlloc(HEAP_MAIN, 64)
    const uint32_t base = a.gpr[3];
    if (!base || !Memory::Contains(base, 64)) return;
    auto call = [&](uint32_t fn, std::initializer_list<double> f, uint32_t r3 = 0, uint32_t r4 = 0) {
        CpuContext c = *cpu;
        c.gpr[3] = r3; c.gpr[4] = r4;
        int i = 1;
        for (double d : f) c.fpr[i++].d = d;
        InvokeIndirectCpu(fn, &c);
        return c.fpr[1].d;
    };
    for (double d : {0.0, 0.5, 1.5707963, 2.0, 3.1415927, 4.712389, -1.5707963, 6.0}) {
        RT_LOGF(RT_TAG_RUNTIME, "[mathtest] x=%g sin %g (host %g) cos %g (host %g) floor %g fmod2 %g\n", d, call(0x80261A5C, {d}), std::sin(d),
                call(0x80261650, {d}), std::cos(d), call(0x80261724, {d}), call(0x80261BB4, {d, 2.0}));
    }
    for (double deg : {0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0, -90.0, 450.0}) {
        call(0x8019BA28, {deg}, base, base + 4);  // sincosf(angle, &s, &c)
        RT_LOGF(RT_TAG_RUNTIME, "[mathtest] deg=%g sincosf s %g c %g | reviseAngle %g | angleABf(0,0,sin,-cos) %g\n", deg, Memory::ReadFloat32(base),
                Memory::ReadFloat32(base + 4), call(0x8019B7A8, {deg}),
                call(0x8019B874, {0.0, 0.0, std::sin(deg * 3.14159265 / 180), -std::cos(deg * 3.14159265 / 180)}));
    }
}

void RunMtxSelfTest(CpuContext* cpu) {
    CpuContext a = *cpu;
    a.gpr[3] = 0;
    a.gpr[4] = 1024;
    InvokeIndirectCpu(0x801A5634, &a);  // __memAlloc(HEAP_MAIN, 1024)
    const uint32_t base = a.gpr[3];
    if (!base || !Memory::Contains(base, 1024)) {
        RT_LOGF(RT_TAG_RUNTIME, "[mtxtest] alloc failed\n");
        return;
    }
    auto put = [&](uint32_t off, const float* v, int n) { for (int i = 0; i < n; ++i) Memory::WriteFloat32(base + off + i * 4, v[i]); };
    auto get = [&](uint32_t off, float* v, int n) { for (int i = 0; i < n; ++i) v[i] = Memory::ReadFloat32(base + off + i * 4); };
    auto call = [&](uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, std::initializer_list<double> f) {
        CpuContext c = *cpu;
        c.gpr[3] = r3; c.gpr[4] = r4; c.gpr[5] = r5; c.gpr[6] = r6;
        int i = 1;
        for (double d : f) c.fpr[i++].d = d;
        InvokeIndirectCpu(fn, &c);
    };
    int bad = 0;
    auto cmp = [&](const char* name, uint32_t off, const float* exp, int n) {
        float got[16] = {};
        get(off, got, n);
        float md = 0, mx = 1;
        for (int i = 0; i < n; ++i) { md = std::max(md, std::fabs(got[i] - exp[i])); mx = std::max(mx, std::fabs(exp[i])); }
        const bool ok = md <= 1e-3f * mx;
        bad += !ok;
        RT_LOGF(RT_TAG_RUNTIME, "[mtxtest] %s %s maxdiff=%g got=[%g %g %g %g] exp=[%g %g %g %g]\n", ok ? "ok      " : "MISMATCH", name, md,
                got[0], got[1], got[2], got[3], exp[0], exp[1], exp[2], exp[3]);
    };
    const uint32_t A = 0x000, B = 0x040, R = 0x080, V = 0x0C0, VO = 0x0D0, CAM = 0x100, UP = 0x110, TGT = 0x120, M4 = 0x140;
    const float ma[12] = {0.8f, 0.1f, -0.3f, 5.f, 0.2f, 0.9f, 0.4f, -7.f, -0.1f, 0.3f, 1.1f, 2.f};
    const float mb[12] = {1.2f, -0.4f, 0.5f, 3.f, 0.3f, 0.7f, -0.2f, 11.f, 0.6f, 0.1f, 0.9f, -4.f};
    put(A, ma, 12); put(B, mb, 12);
    float e[16] = {};
    // Concat
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) { float v = j == 3 ? ma[i * 4 + 3] : 0; for (int k = 0; k < 3; ++k) v += ma[i * 4 + k] * mb[k * 4 + j]; e[i * 4 + j] = v; }
    call(0x80277934, base + A, base + B, base + R, 0, {});
    cmp("PSMTXConcat", R, e, 12);
    // Inverse
    {
        const float* m = ma;
        float c00 = m[5] * m[10] - m[6] * m[9], c01 = m[2] * m[9] - m[1] * m[10], c02 = m[1] * m[6] - m[2] * m[5];
        float c10 = m[6] * m[8] - m[4] * m[10], c11 = m[0] * m[10] - m[2] * m[8], c12 = m[2] * m[4] - m[0] * m[6];
        float c20 = m[4] * m[9] - m[5] * m[8], c21 = m[1] * m[8] - m[0] * m[9], c22 = m[0] * m[5] - m[1] * m[4];
        const float det = m[0] * c00 + m[1] * c10 + m[2] * c20;
        const float r[9] = {c00 / det, c01 / det, c02 / det, c10 / det, c11 / det, c12 / det, c20 / det, c21 / det, c22 / det};
        for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) e[i * 4 + j] = r[i * 3 + j]; e[i * 4 + 3] = -(r[i * 3] * m[3] + r[i * 3 + 1] * m[7] + r[i * 3 + 2] * m[11]); }
        call(0x80277A00, base + A, base + R, 0, 0, {});
        cmp("PSMTXInverse", R, e, 12);
    }
    // MultVec
    {
        const float v[3] = {3.f, -2.f, 7.f};
        put(V, v, 3);
        for (int i = 0; i < 3; ++i) e[i] = ma[i * 4] * v[0] + ma[i * 4 + 1] * v[1] + ma[i * 4 + 2] * v[2] + ma[i * 4 + 3];
        call(0x80278354, base + A, base + V, base + VO, 0, {});
        cmp("PSMTXMultVec", VO, e, 3);
        const float cr[3] = {v[1] * 4.f - v[2] * 0.5f, v[2] * 1.f - v[0] * 4.f, v[0] * 0.5f - v[1] * 1.f};
        const float w[3] = {1.f, 0.5f, 4.f};
        put(R, w, 3);
        call(0x80278978, base + V, base + R, base + VO, 0, {});
        const float ce[3] = {v[1] * w[2] - v[2] * w[1], v[2] * w[0] - v[0] * w[2], v[0] * w[1] - v[1] * w[0]};
        (void)cr;
        cmp("PSVECCrossProduct", VO, ce, 3);
        call(0x802788B8, base + V, base + VO, 0, 0, {});
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const float ne[3] = {v[0] / len, v[1] / len, v[2] / len};
        cmp("PSVECNormalize", VO, ne, 3);
    }
    // LookAt
    {
        const float cam[3] = {10.f, 20.f, 830.f}, up[3] = {0.f, 1.f, 0.f}, tgt[3] = {10.f, 20.f, 30.f};
        put(CAM, cam, 3); put(UP, up, 3); put(TGT, tgt, 3);
        float look[3] = {cam[0] - tgt[0], cam[1] - tgt[1], cam[2] - tgt[2]};
        auto norm = [](float* v) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); v[0] /= l; v[1] /= l; v[2] /= l; };
        norm(look);
        float right[3] = {up[1] * look[2] - up[2] * look[1], up[2] * look[0] - up[0] * look[2], up[0] * look[1] - up[1] * look[0]};
        norm(right);
        const float u[3] = {look[1] * right[2] - look[2] * right[1], look[2] * right[0] - look[0] * right[2], look[0] * right[1] - look[1] * right[0]};
        const float* rows[3] = {right, u, look};
        for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) e[i * 4 + j] = rows[i][j]; e[i * 4 + 3] = -(rows[i][0] * cam[0] + rows[i][1] * cam[1] + rows[i][2] * cam[2]); }
        call(0x80277FBC, base + R, base + CAM, base + UP, base + TGT, {});
        cmp("C_MTXLookAt", R, e, 12);
    }
    // Trans / Scale / RotRad(y)
    {
        call(0x80277E18, base + R, 0, 0, 0, {12.5, -34.25, 800.0});
        const float te[12] = {1, 0, 0, 12.5f, 0, 1, 0, -34.25f, 0, 0, 1, 800.f};
        cmp("PSMTXTrans", R, te, 12);
        call(0x80277E98, base + R, 0, 0, 0, {2.0, 3.0, 0.5});
        const float se[12] = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0.5f, 0};
        cmp("PSMTXScale", R, se, 12);
        const double ang = 0.6;
        call(0x80277BC0, base + R, 'y', 0, 0, {ang});
        const float c = std::cos(static_cast<float>(ang)), sn = std::sin(static_cast<float>(ang));
        const float re[12] = {c, 0, sn, 0, 0, 1, 0, 0, -sn, 0, c, 0};
        cmp("PSMTXRotRad(y)", R, re, 12);
    }
    // Ortho (4x4): t,b,l,r,n,f
    {
        call(0x80278610, base + M4, 0, 0, 0, {143.0, -143.0, -250.0, 250.0, 0.0, 10000.0});
        const float oe[16] = {2.f / 500.f, 0, 0, 0, 0, 2.f / 286.f, 0, 0, 0, 0, -1.f / 10000.f, -1.f, 0, 0, 0, 1};
        float tmp[16] = {2.f / 500.f, 0, 0, 0, 0, 2.f / 286.f, 0, 0, 0, 0, -1.f / 10000.f, -0.f, 0, 0, 0, 1};
        (void)tmp;
        float got[16];
        get(M4, got, 16);
        const float exp[16] = {oe[0], 0, 0, 0, 0, oe[5], 0, 0, 0, 0, oe[10], -0.f, 0, 0, 0, 1};
        RT_LOGF(RT_TAG_RUNTIME, "[mtxtest] C_MTXOrtho raw: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g (expect m00=%g m11=%g m22=%g m23=%g)\n",
                got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7], got[8], got[9], got[10], got[11], got[12], got[13], got[14], got[15],
                exp[0], exp[5], exp[10], -0.f);
    }
    RT_LOGF(RT_TAG_RUNTIME, "[mtxtest] DONE mismatches=%d\n", bad);
}


// US0 seqSetSeq (0x8017B3C8) stores seq/map/entry; seq_gameInit reads gp->mapName at +0x44.
constexpr uint32_t kNowSeq = 0x8056D0D8, kNextSeq = 0x8056D0DC, kGp = 0x8056D198;

// Return to Title hang log (RttLogStep below): the guest words it reads. The stage word is
// seqdrv's SeqWork.stage, the same one LogSceneChanges reads in hle/vi.cpp; the three work
// pointers are the .sdata pointer variables of the same names in projects/spm/pal/addr_map.tsv.
constexpr uint32_t kSeqWorkStage = 0x804CF364, kSeqLoadSubWp = 0x8056D0F0, kMsgDrvMsgW = 0x8056CF38,
                   kPauseWinWp = 0x8056D950;

// The guide subsystem's work struct (guest 0x804CD398, guide.c): +8 holds the
// guide pose's AnimPose slot and +12 the current animation's name pointer. Every
// message/event teaching demo draws through that one pose, so the pose log reads
// both (wiimote-logic-trace-2026-11-04.md).
constexpr uint32_t kGuidePoseSlot = 0x804CD3A0, kGuideAnimName = 0x804CD3A4;

// Debug menu warps (settings_overlay.cpp): the request is stored by the UI and served here, on the game's own frame.
std::mutex g_warpMutex;
bool g_warpPending = false, g_warpHasDoor = false;
bool g_warpHasPos = false;
float g_warpPos[3] = {};
int g_arrivalStage = 0;        // 1: warp issued, 2: new map loading, then Mario is placed at g_arrivalPos
float g_arrivalPos[3] = {};
char g_warpMap[32], g_warpDoor[32];
std::vector<std::function<void(CpuContext*)>> g_jobs;
std::atomic<int> g_lastResult{0};

uint32_t CallGuest(CpuContext* cpu, uint32_t fn, uint32_t r3, uint32_t r4 = 0) {
    CpuContext call = *cpu;
    call.gpr[3] = r3;
    call.gpr[4] = r4;
    InvokeIndirectCpu(fn, &call);
    return call.gpr[3];
}

void Enqueue(std::function<void(CpuContext*)> job) {
    std::lock_guard<std::mutex> lock(g_warpMutex);
    g_jobs.push_back(std::move(job));
}

void RunJobs(CpuContext* cpu) {
    std::vector<std::function<void(CpuContext*)>> jobs;
    {
        std::lock_guard<std::mutex> lock(g_warpMutex);
        uint32_t now = 0, after = 0;
        if (g_jobs.empty() || !Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after) || now != 2 || after != 2) return;
        jobs.swap(g_jobs);
    }
    for (auto& job : jobs) job(cpu);
}

void WatchStep(CpuContext* cpu);

// A map change started before the game has finished loading its effects hangs at stage 11 of the map change (nothing
// is ever shown). The sweep tool waits for the same state before it warps, so the Debug menu does too, and it also
// waits for the map to have been running for a moment.
[[maybe_unused]] bool WarpEffectsReady() {
    uint32_t effects = 0, pending = 0, ready = 0, data = 0, a = 0, b = 0;
    Memory::TryRead32(0x8056CFB0, effects);
    if (!effects || !Memory::Contains(effects, 16)) return false;
    Memory::TryRead32(effects + 8, pending);
    Memory::TryRead32(effects + 12, ready);
    Memory::TryRead32(0x8056D804, data);
    if (!pending || !ready || !data || !Memory::Contains(data, 20)) return false;
    Memory::TryRead32(data + 12, a);
    Memory::TryRead32(data + 16, b);
    return a && b;
}

void ServiceWarp(CpuContext* cpu) {
    if (!cpu) return;
    RunJobs(cpu);
    WatchStep(cpu);
    static uint32_t inMapFrames = 0;
    {
        uint32_t now = 0, after = 0;
        if (Memory::TryRead32(kNowSeq, now) && Memory::TryRead32(kNextSeq, after) && now == 2 && after == 2) ++inMapFrames;
        else inMapFrames = 0;
    }
    if (g_arrivalStage == 1 && inMapFrames == 0) g_arrivalStage = 2;
    if (g_arrivalStage == 2) {
        // Place Mario only once the arrival is over: the door/pipe entrance moves Mario itself and waits for him, so a
        // teleport during it left the event waiting forever. Wait for marioWork.ctrl (0x34) and keyOff (0x35) at 0
        // and a position unchanged for 10 frames; give up after 15 s.
        static float last[3] = {};
        static uint32_t still = 0;
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                                                  // marioGetPtr
        if (inMapFrames > 900 || !Memory::Contains(mw, 0x70)) {
            g_arrivalStage = 0;
        } else if (inMapFrames >= 30) {
            float pos[3];
            for (int i = 0; i < 3; ++i) pos[i] = Memory::ReadFloat32(mw + 0x5C + 4 * i);
            const bool control = Memory::Read8(mw + 0x34) == 0 && Memory::Read8(mw + 0x35) == 0;
            still = control && pos[0] == last[0] && pos[1] == last[1] && pos[2] == last[2] ? still + 1 : 0;
            std::memcpy(last, pos, sizeof last);
            if (still >= 10) {
                g_arrivalStage = 0;
                still = 0;
                for (int i = 0; i < 3; ++i) Memory::WriteFloat32(mw + 0x5C + 4 * i, g_arrivalPos[i]);
            }
        }
    }
    char map[32], door[32];
    bool hasDoor;
    {
        std::lock_guard<std::mutex> lock(g_warpMutex);
        if (!g_warpPending) return;
        uint32_t now = 0, after = 0;
        if (!Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after) || now != 2 || after != 2) return;
        if (inMapFrames < 120) return;   // WarpEffectsReady() is only true during cutscenes, so it cannot gate normal play
        std::memcpy(map, g_warpMap, sizeof map);
        std::memcpy(door, g_warpDoor, sizeof door);
        hasDoor = g_warpHasDoor;
        g_warpPending = false;
        g_arrivalStage = g_warpHasPos ? 1 : 0;
        std::memcpy(g_arrivalPos, g_warpPos, sizeof g_arrivalPos);
    }
    static uint32_t buffer = 0;
    if (!buffer) {
        CpuContext call = *cpu;
        call.gpr[3] = 0;  // __memAlloc(HEAP_MAIN, 64), kept across map changes
        call.gpr[4] = 64;
        InvokeIndirectCpu(0x801A5634, &call);
        buffer = call.gpr[3];
    }
    if (!buffer || !Memory::Contains(buffer, 64)) return;
    for (uint32_t i = 0; i < 32; ++i) {
        Memory::Write8(buffer + i, uint8_t(map[i]));
        Memory::Write8(buffer + 32 + i, uint8_t(door[i]));
    }
    CpuContext call = *cpu;
    call.gpr[3] = 3;  // seqSetSeq(SEQ_MAPCHANGE, mapName, doorName)
    call.gpr[4] = buffer;
    call.gpr[5] = hasDoor ? buffer + 32 : 0;
    InvokeIndirectCpu(0x8017B3C8, &call);
    RT_LOGF(RT_TAG_RUNTIME, "[spmwarp] %s %s\n", map, hasDoor ? door : "(default door)");
}

struct Sweep {
    bool enabled = false, pending = false, changed = false, sampling = false;
    bool startOnly = false;  // start_map.txt: one warp for the player, no sampling, no exits
    int startGsw0 = -1;
    std::vector<std::string> maps;
    size_t next = 0;
    uint32_t buffer = 0, firstFrame = 0;
    int dwellSeconds = 3;
    Clock::time_point started = Clock::now(), requested{}, stable{};

    Sweep() {
        const char* path = std::getenv("SPM_MAP_SWEEP_FILE");
        if (!path || !*path) {
            // start_map.txt next to the exe: "<map> [gsw0]". Once the player is in any map, warp there once and delete
            // the file (a test shortcut; saving afterwards keeps the story value it sets).
            std::ifstream start("start_map.txt");
            std::string map;
            if (start >> map && map.size() < 32 && map.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos) {
                if (!(start >> startGsw0)) startGsw0 = -1;
                enabled = startOnly = true;
                maps.push_back(map);
                RT_LOGF(RT_TAG_RUNTIME, "[spmstart] will warp to %s (gsw0 %d) once a map is loaded\n", map.c_str(), startGsw0);
            }
            start.close();
            if (startOnly) std::remove("start_map.txt");
            return;
        }
        enabled = true;
        std::ifstream in(path);
        std::string name;
        while (std::getline(in, name)) {
            if (!name.empty() && name.back() == '\r') name.pop_back();
            if (name.empty() || name.size() >= 32 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) {
                RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] invalid-map-list\n");
                Finish(3);
            }
            maps.push_back(name);
        }
        if (!in.eof() || maps.empty()) {
            RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] unreadable-or-empty-map-list\n");
            Finish(3);
        }
        if (const char* value = std::getenv("SPM_MAP_SWEEP_SECONDS")) {
            const int seconds = std::atoi(value);
            if (seconds >= 1 && seconds <= 60) dwellSeconds = seconds;
        }
        RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] ready %zu maps dwell=%d\n", maps.size(), dwellSeconds);
    }

    [[noreturn]] static void Finish(int code) {
        std::cerr.flush();
        std::fflush(nullptr);
        std::_Exit(code);
    }

    // GSW(0) to set before entering `map`: longest matching prefix in SPM_SWEEP_GSW0_TABLE ("prefix value" lines, # comments,
    // tools\sweep-gsw0.txt), else SPM_SWEEP_GSW0, else -1 (leave the save alone).
    static int Gsw0For(const std::string& map) {
        static const auto table = [] {
            std::vector<std::pair<std::string, int>> t;
            if (const char* path = std::getenv("SPM_SWEEP_GSW0_TABLE")) {
                std::ifstream in(path);
                std::string prefix;
                int value = 0;
                while (in >> prefix) {
                    if (prefix[0] == '#') { std::getline(in, prefix); continue; }
                    if (in >> value) t.emplace_back(prefix, value);
                }
                RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] gsw0 table %s: %zu entries\n", path, t.size());
            }
            return t;
        }();
        size_t best = 0;
        int value = -1;
        for (const auto& [prefix, v] : table)
            if (prefix.size() > best && map.compare(0, prefix.size(), prefix) == 0) best = prefix.size(), value = v;
        if (value < 0)
            if (const char* g0 = std::getenv("SPM_SWEEP_GSW0")) value = std::atoi(g0);
        return value;
    }

    static std::string CurrentMap() {
        uint32_t gp = 0;
        if (!Memory::TryRead32(kGp, gp) || gp < 0x80004000 || !Memory::Contains(gp, 0x64)) return {};
        std::string name;
        for (uint32_t i = 0; i < 32; ++i) {
            const char c = static_cast<char>(Memory::Read8(gp + 0x44 + i));
            if (!c) return name;
            name += c;
        }
        return {};
    }

    static bool EffectsReady() {
        uint32_t effects = 0, pending = 0, ready = 0, data = 0, a = 0, b = 0;
        Memory::TryRead32(0x8056CFB0, effects);
        if (!effects || !Memory::Contains(effects, 16)) return false;
        Memory::TryRead32(effects + 8, pending);
        Memory::TryRead32(effects + 12, ready);
        Memory::TryRead32(0x8056D804, data);  // effSync's final check: r13-0x771C.
        if (!pending || !ready || !data || !Memory::Contains(data, 20)) return false;
        Memory::TryRead32(data + 12, a);
        Memory::TryRead32(data + 16, b);
        return a && b;
    }

    void Tick(CpuContext* cpu, uint32_t frames) {
        if (!enabled || !cpu) return;
        uint32_t now = 0, after = 0;
        if (!Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after)) return;
        const auto time = Clock::now();
        static Clock::time_point holdUntil{};
        if (!pending) {
            if (now != 2 || after != 2 || CurrentMap().empty() || (!buffer && !EffectsReady())) {
                if (!startOnly && time - started > std::chrono::seconds(90)) {
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] boot-timeout seq=%u next=%u\n", now, after);
                    Finish(2);
                }
                return;
            }
            if (next == maps.size()) {
                if (time < holdUntil) return;  // the last map's screenshot needs the window too
                RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] complete\n");
                Finish(0);
            }
            if (!buffer) {
                CpuContext call = *cpu;
                call.gpr[3] = 0;  // __memAlloc(HEAP_MAIN, 64), persistent across map changes.
                call.gpr[4] = 64;
                InvokeIndirectCpu(0x801A5634, &call);
                buffer = call.gpr[3];
                if (!buffer || !Memory::Contains(buffer, 64)) {
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] allocation-failed\n");
                    Finish(3);
                }
            }
            { static bool mathDone = false; if (!mathDone && std::getenv("SPM_MATH_SELFTEST")) { mathDone = true; RunMathSelfTest(cpu); } }
            { static bool mtxDone = false; if (!mtxDone && std::getenv("SPM_MTX_SELFTEST")) { mtxDone = true; RunMtxSelfTest(cpu); } }
            if (time < holdUntil) return;
            const auto& name = maps[next++];
            for (uint32_t i = 0; i < 32; ++i) Memory::Write8(buffer + i, i < name.size() ? name[i] : 0);
            pending = true;
            changed = sampling = false;
            requested = time;
            RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] attempt %s\n", name.c_str());
            if (const int g0 = startOnly ? startGsw0 : Gsw0For(name); g0 >= 0) {  // story progress (gp->gsw0) the map scripts branch on
                uint32_t gp = 0;
                if (Memory::TryRead32(kGp, gp) && Memory::Contains(gp, 0x144)) Memory::Write32(gp + 0x140, uint32_t(g0));
            }
            if (const char* g1 = std::getenv("SPM_SWEEP_GSW1")) {  // Pit floor (gp->gsw[1], u8): chest rooms need 9,19.. / 109,119..
                uint32_t gp = 0;
                if (Memory::TryRead32(kGp, gp) && Memory::Contains(gp, 0x546)) Memory::Write8(gp + 0x545, uint8_t(std::atoi(g1)));
            }
            if (const char* item = std::getenv("SPM_GIVE_ITEM")) {  // test: pouchAddItem(id) once, e.g. 67 Thunder Rage
                static bool given = false;
                if (!given) {
                    given = true;
                    CpuContext add = *cpu;
                    add.gpr[3] = static_cast<uint32_t>(std::atoi(item));
                    InvokeIndirectCpu(0x8014CB04, &add);
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] gave item %s\n", item);
                }
            }
            CpuContext call = *cpu;
            call.gpr[3] = 3;  // seqSetSeq(SEQ_MAPCHANGE, mapName, nullptr).
            call.gpr[4] = buffer;
            call.gpr[5] = 0;
            InvokeIndirectCpu(0x8017B3C8, &call);
            if (startOnly) enabled = false;  // the game takes it from here
            return;
        }
        const auto& name = maps[next - 1];
        changed = changed || now == 3;
        if (changed && now == 2 && after == 2) {
            const std::string actual = CurrentMap();
            if (actual == name) {
                if (!sampling) {
                    stable = time;
                    firstFrame = frames;
                    sampling = true;
                    for (unsigned& c : g_spmAudit) c = 0;
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] loaded %s\n", name.c_str());
                }
                {   // draw dump of 2 frames, SPM_SWEEP_DUMP_FRAME (default 200) frames after load, for the map named in SPM_SWEEP_DUMP
                    static const char* dumpMap = std::getenv("SPM_SWEEP_DUMP");
                    static const unsigned dumpAt = [] { const char* e = std::getenv("SPM_SWEEP_DUMP_FRAME"); return e ? unsigned(std::atoi(e)) : 200u; }();
                    const unsigned rel = frames - firstFrame;
                    g_spmSweepDump = (dumpMap && name == dumpMap && rel >= dumpAt && rel < dumpAt + 2) ? 1 : 0;
                }
                if (static const bool hideHud = std::getenv("SPM_HIDE_HUD") != nullptr; hideHud) {  // clean captures (launcher art)
                    CpuContext hud = *cpu;
                    InvokeIndirectCpu(0x80198CA0, &hud);  // hudHide()
                }
                {   // SPM_SWEEP_WPAD: wpadmgr state once a second (cursor flag, pointingPos[0], KPAD pos/dpd of the newest status)
                    static const bool wpadLog = std::getenv("SPM_SWEEP_WPAD") != nullptr;
                    uint32_t wp = 0;
                    if (wpadLog && (frames - firstFrame) % 60 == 0 && Memory::TryRead32(0x80574F20u - 0x7D68u, wp) && Memory::Contains(wp, 0x9DC8)) {
                        const uint32_t st = wp + 0x6C;
                        RT_LOGF(RT_TAG_RUNTIME, "[spmwpad] flags %08X pointing %.3f,%.3f kpad pos %.3f,%.3f dpd %u hold %08X\n",
                                Memory::Read32(wp), Memory::ReadFloat32(wp + 0x4C), Memory::ReadFloat32(wp + 0x50),
                                Memory::ReadFloat32(st + 0x20), Memory::ReadFloat32(st + 0x24), Memory::Read8(st + 0x5E), Memory::Read32(st));
                    }
                }
                if (time - stable >= std::chrono::seconds(dwellSeconds) && frames - firstFrame >= 10) {
                    // fadeIsFinish(): 0 means a screen fade/curtain is still active, so a black shot is not a render bug.
                    CpuContext fadeCall = *cpu;
                    InvokeIndirectCpu(0x800676E0, &fadeCall);
                    RT_LOGF(RT_TAG_RUNTIME, "[spmaudit] %s draws=%u dropped=%u nohandle=%u badtexmap=%u clear=%06X fade=%u xfskip=%u nesteddl=%u texnull=%u palnull=%u\n", name.c_str(),
                            g_spmAudit[0], g_spmAudit[1], g_spmAudit[2], g_spmAudit[3], g_spmGameClear, fadeCall.gpr[3] & 0xFF,
                            g_spmAudit[4], g_spmAudit[5], g_spmAudit[6], g_spmAudit[7]);
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] sampled %s frames=%u\n", name.c_str(), frames - firstFrame);
                    { uint32_t g = 0, gsw0 = 0; if (Memory::TryRead32(kGp, g) && Memory::Contains(g, 0x144) && Memory::TryRead32(g + 0x140, gsw0)) RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] gsw0 %d\n", int32_t(gsw0)); }
                    { // MapObj flag0 of interesting objects (mapdrv_wp at r13-0x7F10; spm-headers MapEntry/MapObj layout)
                      uint32_t wp = 0, hidden = 0;
                      if (Memory::TryRead32(0x8056D010u, wp) && Memory::Contains(wp, 0x6AC)) {
                          const uint32_t grp = Memory::Read32(wp + 0x698) & 1u;
                          const uint32_t e = wp + grp * 0x34C;
                          const uint32_t n = Memory::Read32(e + 0x180), objs = Memory::Read32(e + 0x184);
                          for (uint32_t k = 0; k < n && k < 4096 && Memory::Contains(objs + k * 0x144, 0x144); ++k) {
                              const uint32_t o = objs + k * 0x144, f0 = Memory::Read32(o), j = Memory::Read32(o + 0xC);
                              uint32_t np = 0; if (!Memory::Contains(j, 4) || !Memory::TryRead32(j, np) || !Memory::Contains(np, 16)) continue;
                              std::string nm; for (int c = 0; c < 31; ++c) { const char ch = char(Memory::Read8(np + c)); if (!ch || ch < 0x20 || ch > 0x7e) break; nm += ch; }
                              if (f0 & 1u) ++hidden;
                              if (nm == "sky_msk" || nm == "group4" || nm == "dai_k_ani_01" || nm == "sc_doa_02" || nm == "polySurface62_q00197" || nm == "mae_kusa_10")
                                  RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] mapobj %s f0 %08X\n", nm.c_str(), f0);
                          }
                          RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] mapobjs %u hidden %u\n", n, hidden);
                          const uint32_t na = Memory::Read32(e + 0x190), anims = Memory::Read32(e + 0x194);
                          for (uint32_t k = 0; k < na && k < 64 && Memory::Contains(anims + k * 0x28, 0x28); ++k) {
                              const uint32_t a = anims + k * 0x28, fd = Memory::Read32(a + 0x18);
                              uint32_t np = 0; std::string nm;
                              if (Memory::Contains(fd, 4) && Memory::TryRead32(fd, np) && Memory::Contains(np, 16))
                                  for (int c = 0; c < 31; ++c) { const char ch = char(Memory::Read8(np + c)); if (!ch || ch < 0x20 || ch > 0x7e) break; nm += ch; }
                              RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] anim %s flags %04X time %.1f rate %.2f\n", nm.c_str(), Memory::Read16(a), Memory::ReadFloat32(a + 0x10), Memory::ReadFloat32(a + 0x14));
                          }
                      } }
                    holdUntil = time + std::chrono::milliseconds(4000);  // let the audit screenshot see this map (the runner polls the log, then PrintWindows a full frame)
                    pending = false;
                    return;
                }
            } else if (!actual.empty()) {
                RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] redirected %s actual=%s\n", name.c_str(), actual.c_str());
                pending = false;
                return;
            }
        } else {
            sampling = false;
        }
        if (time - requested > std::chrono::seconds(30 + dwellSeconds)) {
            uint32_t stage = 0, rel = 0, effects = 0, effectPending = 0, effectReady = 0;
            Memory::TryRead32(0x804CF364, stage);
            Memory::TryRead32(0x8056D1C0, rel);  // isRelLoaded: r13-0x7D60, byte +8.
            Memory::TryRead32(0x8056CFB0, effects);  // effSync: r13-0x7F70, words +8/+12.
            if (effects && Memory::Contains(effects, 16)) {
                Memory::TryRead32(effects + 8, effectPending);
                Memory::TryRead32(effects + 12, effectReady);
            }
            const uint32_t loaded = rel && Memory::Contains(rel, 9) ? Memory::Read8(rel + 8) : 0;
            RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] timeout %s seq=%u next=%u stage=%u rel=%u effects=%u/%u\n",
                    name.c_str(), now, after, stage, loaded, effectPending, effectReady);
            Finish(2);  // Restart externally; forcing another warp during a failed transition is unsafe.
        }
    }
};
} // namespace

namespace {
std::atomic<int> g_saveStatus{0};
std::atomic<bool> g_infiniteHp{false}, g_maxCoins{false}, g_infiniteFlip{false}, g_maxTokens{false};

void SaveAndCheatsStep(CpuContext* cpu) {
    uint32_t seq = 0;
    if (!cpu || !Memory::TryRead32(kNowSeq, seq) || seq != 2) return;
    if (g_saveStatus == 1 && CallGuest(cpu, 0x8023C324, 0) == 0) {                      // nandIsExec: the write is done
        g_saveStatus = 2;
        RT_LOGF(RT_TAG_RUNTIME, "save anywhere: saved\n");
    }
    if (!g_infiniteHp && !g_maxCoins && !g_infiniteFlip && !g_maxTokens) return;
    const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);                                // pouchGetPtr
    if (!Memory::Contains(pouch, 0x20)) return;
    if (g_infiniteHp) Memory::Write32(pouch + 0x0C, Memory::Read32(pouch + 0x10));        // hp = max hp
    if (g_maxCoins) Memory::Write32(pouch + 0x1C, 999);
    if (g_infiniteFlip) Memory::Write32(pouch + 0x14, 10);                                // flip meter: pouchResetFlip's full value
    if (g_maxTokens) CallGuest(cpu, 0x8014EEF8, 9999);                                    // pouchSetArcadeTokens (caps at 9999)
}
} // namespace

// SPM_TEST_CHEATS=1: in a map, turn on every Options > Cheats cheat, then log what the pouch shows (tests).
void CheatSelfTest(CpuContext* cpu) {
    static const bool on = std::getenv("SPM_TEST_CHEATS") != nullptr;
    static uint32_t frames = 0;
    uint32_t seq = 0;
    if (!on || !cpu || !Memory::TryRead32(kNowSeq, seq) || seq != 2) return;
    const auto stats = [&](const char* when) {
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);
        RT_LOGF(RT_TAG_RUNTIME,
                "cheat test %s: hp %u/%u coins %u flip %u tokens %u score %u level %u attack %u cards %u maps %u items %u chars %u pixls %u\n",
                when, Memory::Read32(pouch + 0x0C), Memory::Read32(pouch + 0x10), Memory::Read32(pouch + 0x1C),
                Memory::Read32(pouch + 0x14), CallGuest(cpu, 0x8014EEE8, 0), CallGuest(cpu, 0x8014CA04, 0),
                CallGuest(cpu, 0x8014C6FC, 0), CallGuest(cpu, 0x8014C8D4, 0), CallGuest(cpu, 0x8014F5BC, 0),
                CallGuest(cpu, 0x8014F394, 0), CallGuest(cpu, 0x8014EF2C, 0), CallGuest(cpu, 0x8014F12C, 0),
                CallGuest(cpu, 0x8014F17C, 0));
    };
    if (++frames == 120) {
        stats("before");
        SetInfiniteHp(true);
        SetMaxCoins(true);
        SetInfiniteFlip(true);
        SetMaxArcadeTokens(true);
        AddScore(10000);
        AddAttack(1);
        AddMaxHp(5);
        AddItem(82);  // Ultra Shroom Shake
        AddItemRange(282, 537);
        AddItemRange(234, 281);
        for (int id = 217; id <= 231; ++id) Unlock(id, id >= 220);
    } else if (frames == 240) {
        stats("after");
        SwitchCharacter(2);  // Bowser
    } else if (frames == 360) {
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);
        RT_LOGF(RT_TAG_RUNTIME, "cheat test: character now %d (2 = Bowser)\n", int8_t(Memory::Read8(mw + 0x38)));
        CycleCharacter();  // the V key: on to the next one the player has
    } else if (frames == 480) {
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);
        RT_LOGF(RT_TAG_RUNTIME, "cheat test: after the swap button, character %d\n", int8_t(Memory::Read8(mw + 0x38)));
    }
}

// SPM_TEST_ASSERT57=1: raise extdrv.c:364 (issue #57) with the game's own strings; the game must carry on.
void Assert57SelfTest(CpuContext* cpu) {
    static const bool on = std::getenv("SPM_TEST_ASSERT57") != nullptr;
    static uint32_t frames = 0;
    uint32_t seq = 0;
    if (!on || !cpu || !Memory::TryRead32(kNowSeq, seq) || seq != 2 || ++frames != 120) return;
    CpuContext call = *cpu;
    const uint32_t strings = 0x80320000u + 21408u;  // extMakeTexture's string block
    call.gpr[3] = strings + 44;
    call.gpr[4] = 364;
    call.gpr[5] = strings + 157;
    call.gpr[6] = strings + 187;
    InvokeIndirectCpu(0x8019B6C8u, &call);  // __assert2
    RT_LOGF(RT_TAG_RUNTIME, "assert57 test: the game carried on\n");
}

// Where the game is, for logs: "<map> 2D" or "<map> 3D" while in a map (gp->mapName, marioCheck3d), else the
// sequence ("title", "mapchange", ...). A change is logged as one [place] line with its frame, and handed to
// aurora so the perftimer's [place] line names the rooms each window covered.
std::mutex g_placeMutex;
char g_place[48] = "boot";

void TrackPlace(CpuContext* cpu, uint32_t presentedFrames) {
    static constexpr const char* kSeqNames[] = {"logo", "title", "game", "mapchange", "gameover", "load"};
    uint32_t now = 0, gp = 0;
    if (!Memory::TryRead32(kNowSeq, now)) return;
    char place[48];
    if (now == 2 && cpu) {
        char map[32] = {};
        if (Memory::TryRead32(kGp, gp) && gp >= 0x80004000 && Memory::Contains(gp + 0x44, sizeof map)) {
            for (uint32_t i = 0; i + 1 < sizeof map && (map[i] = char(Memory::Read8(gp + 0x44 + i))); ++i) {}
        }
        const bool is3d = CallGuest(cpu, 0x8012A624, 0) != 0;  // marioCheck3d
        std::snprintf(place, sizeof place, "%s %s", map[0] ? map : "?", is3d ? "3D" : "2D");
    } else {
        std::snprintf(place, sizeof place, "%s", now < 6 ? kSeqNames[now] : "none");
    }
    {
        std::lock_guard<std::mutex> lock(g_placeMutex);
        if (std::strcmp(place, g_place) == 0) return;
        RT_LOGF(RT_TAG_RUNTIME, "[place] %s -> %s (frame %u)\n", g_place, place, presentedFrames);
        std::memcpy(g_place, place, sizeof g_place);
    }
    AuroraSetPerfPlace(place);
}

bool MarioIn3d(CpuContext* cpu) {
    return cpu && CallGuest(cpu, 0x8012A624, 0) != 0;  // marioCheck3d
}

int MarioCharacter(CpuContext* cpu) {
    if (!cpu) return -1;
    const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);  // marioGetPtr
    if (!Memory::Contains(mw, 0x40)) return -1;
    return int8_t(Memory::Read8(mw + 0x38));
}

bool MarioHasControl(CpuContext* cpu) {
    if (!cpu) return false;
    const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);  // marioGetPtr
    if (!Memory::Contains(mw, 0x40)) return false;
    // The same pair ServiceWarp's arrival placement waits on before it moves Mario.
    return Memory::Read8(mw + 0x34) == 0 && Memory::Read8(mw + 0x35) == 0;
}

// ---- Return to Title hang log (the "rttlog" dk_diag key) ----------------------------
// ReturnToTitle borrows the game-over path's seqSetSeq(SEQ_TITLE) from live gameplay,
// and after a direct jump the file select never becomes usable: seq_load sits in stage 0
// polling guest state that a mapchange transit would have torn down (the hunt is written
// up in spm-audit-findings/return-to-title-hunt-2026-11-03.md). This logger is the
// evidence run for naming that state. While the game is in SEQ_LOAD it writes, once at
// entry and then once every 60 frames, the load sequence's own stage, the file select's
// work words (its state, flags and save-update event pointer live in the first words of
// SeqLoadSubWork), the event manager's work words, the head of gp, Mario's flags and
// control bytes, the spmario system level, and the message/pause window pointers and
// the first words of the two window work structs themselves. It also takes one
// snapshot at the moment ReturnToTitle's job fires (tagged "invoke"): the state the
// jump is made FROM, in gameplay, before seqSetSeq runs. The
// struct layouts are the generated code's, not this repo's, so the work structs are
// logged as raw words: a value frozen across a whole hang is the finding, whichever
// word it turns out to be. READS ONLY: the guest calls are the game's own pure
// getters (marioGetPtr as above, spmarioGetSystemLevel as in SetGamePaused, and the
// event manager's evtGetWork); nothing here writes guest memory or changes any
// state. Off by default, and with the switch off Tick pays one atomic load for it.
std::atomic<bool> g_rttLog{false};
uint32_t g_rttFrame = 0;  // presented frames at the latest Tick, for the invoke line

// The first 8 words at a window work pointer, in the same guarded style as the other
// structs here: a null pointer and an unreadable one each get a marker line instead of
// a read. `tag` is "[rttlog]" for the load-step lines, "[rttlog] invoke" for the
// ReturnToTitle snapshot, so the two never blur together in the log.
void RttLogWindowWords(const char* tag, const char* name, uint32_t wp) {
    if (!wp) {
        RT_LOGF(RT_TAG_RUNTIME, "%s %s 00000000 (null)\n", tag, name);
    } else if (!Memory::Contains(wp, 32)) {
        RT_LOGF(RT_TAG_RUNTIME, "%s %s %08X (unreadable)\n", tag, name, wp);
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "%s %s %08X words %08X %08X %08X %08X %08X %08X %08X %08X\n", tag, name, wp,
                Memory::Read32(wp), Memory::Read32(wp + 4), Memory::Read32(wp + 8), Memory::Read32(wp + 12),
                Memory::Read32(wp + 16), Memory::Read32(wp + 20), Memory::Read32(wp + 24), Memory::Read32(wp + 28));
    }
}

void RttLogStep(CpuContext* cpu, uint32_t frames) {
    static bool inLoad = false;
    static uint32_t lastEmit = 0;
    if (!g_rttLog.load() || !cpu) {
        inLoad = false;
        return;
    }
    g_rttFrame = frames;
    uint32_t now = 0, after = 0;
    if (!Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after)) return;
    if (now != 5) {  // SEQ_LOAD (kSeqNames order in TrackPlace)
        if (inLoad) {
            inLoad = false;
            RT_LOGF(RT_TAG_RUNTIME, "[rttlog] left load at frame %u (now %u next %u)\n", frames, now, after);
        }
        return;
    }
    if (inLoad && frames - lastEmit < 60) return;
    inLoad = true;
    lastEmit = frames;

    uint32_t stage = 0xFFFFFFFFu;
    Memory::TryRead32(kSeqWorkStage, stage);
    const int sysLevel = int(CallGuest(cpu, 0x801A7A4C, 0));  // spmarioGetSystemLevel
    const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);        // marioGetPtr
    uint32_t marioFlags = 0;
    int ctrl = -1, keyOff = -1;
    if (Memory::Contains(mw, 0x40)) {
        marioFlags = Memory::Read32(mw);  // MarioWork.flags, the word the [mario] log prints
        ctrl = Memory::Read8(mw + 0x34);
        keyOff = Memory::Read8(mw + 0x35);
    }
    uint32_t msgw = 0, pausewin = 0;
    Memory::TryRead32(kMsgDrvMsgW, msgw);
    Memory::TryRead32(kPauseWinWp, pausewin);
    RT_LOGF(RT_TAG_RUNTIME,
            "[rttlog] frame %u seq load (next %u) stage %u syslvl %d mario %08X flags %08X ctrl %d keyoff %d "
            "msgw %08X pausewin %08X\n",
            frames, after, stage, sysLevel, mw, marioFlags, ctrl, keyOff, msgw, pausewin);
    RttLogWindowWords("[rttlog]", "msgw", msgw);
    RttLogWindowWords("[rttlog]", "pausewin", pausewin);

    uint32_t sub = 0;
    if (Memory::TryRead32(kSeqLoadSubWp, sub) && sub && Memory::Contains(sub, 64)) {
        RT_LOGF(RT_TAG_RUNTIME,
                "[rttlog] loadsub %08X words %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X "
                "%08X %08X\n",
                sub, Memory::Read32(sub), Memory::Read32(sub + 4), Memory::Read32(sub + 8), Memory::Read32(sub + 12),
                Memory::Read32(sub + 16), Memory::Read32(sub + 20), Memory::Read32(sub + 24), Memory::Read32(sub + 28),
                Memory::Read32(sub + 32), Memory::Read32(sub + 36), Memory::Read32(sub + 40), Memory::Read32(sub + 44),
                Memory::Read32(sub + 48), Memory::Read32(sub + 52), Memory::Read32(sub + 56), Memory::Read32(sub + 60));
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "[rttlog] loadsub %08X (unreadable)\n", sub);
    }
    const uint32_t evt = CallGuest(cpu, 0x800D89B4, 0);  // evtGetWork
    if (evt && Memory::Contains(evt, 32)) {
        RT_LOGF(RT_TAG_RUNTIME, "[rttlog] evtwork %08X words %08X %08X %08X %08X %08X %08X %08X %08X\n", evt,
                Memory::Read32(evt), Memory::Read32(evt + 4), Memory::Read32(evt + 8), Memory::Read32(evt + 12),
                Memory::Read32(evt + 16), Memory::Read32(evt + 20), Memory::Read32(evt + 24), Memory::Read32(evt + 28));
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "[rttlog] evtwork %08X (unreadable)\n", evt);
    }
    uint32_t gp = 0;
    if (Memory::TryRead32(kGp, gp) && gp && Memory::Contains(gp, 32)) {
        RT_LOGF(RT_TAG_RUNTIME, "[rttlog] gp %08X words %08X %08X %08X %08X %08X %08X %08X %08X\n", gp,
                Memory::Read32(gp), Memory::Read32(gp + 4), Memory::Read32(gp + 8), Memory::Read32(gp + 12),
                Memory::Read32(gp + 16), Memory::Read32(gp + 20), Memory::Read32(gp + 24), Memory::Read32(gp + 28));
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "[rttlog] gp %08X (unreadable)\n", gp);
    }
}

void Tick(CpuContext* cpu, uint32_t presentedFrames) {
    TrackPlace(cpu, presentedFrames);
    RttLogStep(cpu, presentedFrames);
    Assert57SelfTest(cpu);
    CheatSelfTest(cpu);
    SaveAndCheatsStep(cpu);
    if (cpu) ModRuntime::Frame(cpu);
    // SPM_MARIO_LOG=1: Mario's position / speed / motion / buttons every 15 frames in a map (spm-headers MarioWork).
    static const bool marioLog = std::getenv("SPM_MARIO_LOG") != nullptr;
    uint32_t seq = 0;
    if (marioLog && cpu && presentedFrames % 15 == 0 && Memory::TryRead32(kNowSeq, seq) && seq == 2) {
        CpuContext call = *cpu;
        InvokeIndirectCpu(0x801214EC, &call);  // marioGetPtr
        const uint32_t mw = call.gpr[3];
        if (Memory::Contains(mw, 0x320)) {
            // The map object Mario stands on (hitGetName of MarioWork+0x1C4), e.g. he3_08's statue pedestal "A2D_dai".
            CpuContext hit = *cpu;
            hit.gpr[3] = Memory::Read32(mw + 0x1C4);
            InvokeIndirectCpu(0x80070780, &hit);  // hitGetName
            char ground[24] = {};
            for (int i = 0; i < 23 && Memory::Contains(hit.gpr[3] + i, 1) && (ground[i] = char(Memory::Read8(hit.gpr[3] + i))); ++i) {}
            RT_LOGF(RT_TAG_RUNTIME, "[mario] ground %s\n", ground);
        }
        if (Memory::Contains(mw, 0x320))
            RT_LOGF(RT_TAG_RUNTIME, "[mario] f=%u pos %.2f %.2f %.2f xz %.3f walk %.2f mot %u sub %d held %04X rep %04X joy %u,%u ctrl %u keyoff %u flags %08X dir %.1f wpad %08X\n",
                    presentedFrames, Memory::ReadFloat32(mw + 0x5C), Memory::ReadFloat32(mw + 0x60), Memory::ReadFloat32(mw + 0x64),
                    Memory::ReadFloat32(mw + 0x148), Memory::ReadFloat32(mw + 0x14C), Memory::Read16(mw + 0x2C), int32_t(Memory::Read32(mw + 0x3C)),
                    Memory::Read16(mw + 0x30C), Memory::Read16(mw + 0x310), Memory::Read8(mw + 0x316), Memory::Read8(mw + 0x317),
                    Memory::Read8(mw + 0x34), Memory::Read8(mw + 0x35), Memory::Read32(mw), Memory::ReadFloat32(mw + 0x174),
                    Memory::Contains(Memory::Read32(0x8056D1B8u), 4) ? Memory::Read32(Memory::Read32(0x8056D1B8u)) : 0u);
    }
    // SPM_MARIO_POS=x,y,z: once Mario has been in a map for a second, put him there (tests at a spot; one time).
    static const char* posEnv = std::getenv("SPM_MARIO_POS");
    static uint32_t inMapFrames = 0;
    if (posEnv && cpu && Memory::TryRead32(kNowSeq, seq) && seq == 2 && ++inMapFrames == 60) {
        float p[3] = {};
        if (std::sscanf(posEnv, "%f,%f,%f", &p[0], &p[1], &p[2]) == 3) {
            CpuContext call = *cpu;
            InvokeIndirectCpu(0x801214EC, &call);  // marioGetPtr
            const uint32_t mw = call.gpr[3];
            if (Memory::Contains(mw, 0x70)) {
                for (int i = 0; i < 3; ++i) Memory::WriteFloat32(mw + 0x5C + 4 * i, p[i]);
                RT_LOGF(RT_TAG_RUNTIME, "[mario] placed at %.1f %.1f %.1f\n", p[0], p[1], p[2]);
            }
        }
    }
    ServiceWarp(cpu);
    static Sweep sweep;
    sweep.Tick(cpu, presentedFrames);
    SpmPrepareGraphics::Tick(cpu, presentedFrames);
}
void RequestWarp(const char* map, const char* door, const float* arrivalPos) {
    std::lock_guard<std::mutex> lock(g_warpMutex);
    g_warpHasPos = arrivalPos != nullptr;
    if (arrivalPos) std::memcpy(g_warpPos, arrivalPos, sizeof g_warpPos);
    std::memset(g_warpMap, 0, sizeof g_warpMap);
    std::memset(g_warpDoor, 0, sizeof g_warpDoor);
    std::strncpy(g_warpMap, map, sizeof g_warpMap - 1);
    g_warpHasDoor = door && *door;
    if (g_warpHasDoor) std::strncpy(g_warpDoor, door, sizeof g_warpDoor - 1);
    g_warpPending = true;
}
void AddItem(int itemId) {
    Enqueue([=](CpuContext* cpu) { g_lastResult = int(CallGuest(cpu, 0x8014CB04, uint32_t(itemId))); });      // pouchAddItem
}
void RemoveItem(int itemId) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, 0x8014D458, uint32_t(itemId)); });                          // pouchRemoveItem
}
// Tippi is the game's "guide", not a pouch item: the open-world mod only switches her on, it never adds item 220, and
// she is not in the game's Pixl menu. As a pouch Pixl she showed up there and selecting her crashed (mot_fairy_other.c).
// guideWork.flag0 bit 0x80 turns her off; saving copies it into GSWF 403.
static void SetTippiEnabled(CpuContext* cpu, bool enabled) {
    const uint32_t guide = CallGuest(cpu, 0x8011EFD0, 0);                                                    // guideGetWork
    if (!Memory::Contains(guide, 2)) return;
    const uint16_t flag0 = Memory::Read16(guide);
    Memory::Write16(guide, enabled ? uint16_t(flag0 & ~0x80u) : uint16_t(flag0 | 0x80u));
}
void SetTippi(bool enabled) {
    Enqueue([=](CpuContext* cpu) {
        SetTippiEnabled(cpu, enabled);
        // An item 220 left by an earlier version of this menu: take it out of the Pixl list (slot emptied, rest moved up).
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);                                                // pouchGetPtr
        if (!Memory::Contains(pouch, 0x104)) return;
        const uint32_t base = pouch + 0xC4;
        for (uint32_t i = 0; i < 16; ++i) {
            if (int16_t(Memory::Read16(base + 4 * i + 2)) != 220) continue;
            for (uint32_t j = i; j + 1 < 16; ++j) Memory::Write32(base + 4 * j, Memory::Read32(base + 4 * (j + 1)));
            Memory::Write32(base + 4 * 15, 0);
            break;
        }
    });
}
// The rttlog snapshot at the moment ReturnToTitle's job fires: Mario's flags and
// control bytes, the spmario system level, and the two window work structs' first
// words, all as they stand in live gameplay BEFORE the seqSetSeq below runs. The
// load-step lines show what the file select waits on; this shows what the jump
// carried in. Same reads-only discipline as RttLogStep (the game's pure getters and
// guarded memory reads, no writes). Called only with the switch on; with it off the
// job below checks the flag and does nothing extra at all.
void RttLogInvoke(CpuContext* cpu) {
    const int sysLevel = int(CallGuest(cpu, 0x801A7A4C, 0));  // spmarioGetSystemLevel
    const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);        // marioGetPtr
    uint32_t marioFlags = 0;
    int ctrl = -1, keyOff = -1;
    if (Memory::Contains(mw, 0x40)) {
        marioFlags = Memory::Read32(mw);  // MarioWork.flags, as in RttLogStep
        ctrl = Memory::Read8(mw + 0x34);
        keyOff = Memory::Read8(mw + 0x35);
    }
    uint32_t msgw = 0, pausewin = 0;
    Memory::TryRead32(kMsgDrvMsgW, msgw);
    Memory::TryRead32(kPauseWinWp, pausewin);
    RT_LOGF(RT_TAG_RUNTIME,
            "[rttlog] invoke frame %u mario %08X flags %08X ctrl %d keyoff %d syslvl %d msgw %08X pausewin %08X\n",
            g_rttFrame, mw, marioFlags, ctrl, keyOff, sysLevel, msgw, pausewin);
    RttLogWindowWords("[rttlog] invoke", "msgw", msgw);
    RttLogWindowWords("[rttlog] invoke", "pausewin", pausewin);
}
void ReturnToTitle() {
    Enqueue([](CpuContext* cpu) {
        uint32_t now = 0, after = 0;
        if (!Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after) || now != 2 || after != 2) return;
        if (g_rttLog.load()) RttLogInvoke(cpu);
        CpuContext call = *cpu;
        call.gpr[3] = 1;  // seqSetSeq(SEQ_TITLE, nullptr, nullptr): the game over "quit" path
        call.gpr[4] = 0;
        call.gpr[5] = 0;
        InvokeIndirectCpu(0x8017B3C8, &call);
    });
}
void SetGamePaused(bool paused) {
    static int s_savedLevel = -1;  // the level before the menu froze the game; -1 = not frozen
    // Level 2 is what the game uses while an item-get plays (itemCollect): the world stops. It takes 0..2 only.
    constexpr uint32_t kPauseLevel = 2;
    Enqueue([=](CpuContext* cpu) {
        if (paused && s_savedLevel < 0) {
            s_savedLevel = int(CallGuest(cpu, 0x801A7A4C, 0));                // spmarioGetSystemLevel
            if (uint32_t(s_savedLevel) < kPauseLevel) CallGuest(cpu, 0x801A79D0, kPauseLevel);  // spmarioSystemLevel
        } else if (!paused && s_savedLevel >= 0) {
            if (uint32_t(s_savedLevel) < kPauseLevel) CallGuest(cpu, 0x801A79D0, uint32_t(s_savedLevel));
            s_savedLevel = -1;
        }
    });
}
void CurrentPlace(char* out, size_t size) {
    if (size == 0) return;
    std::lock_guard<std::mutex> lock(g_placeMutex);
    std::snprintf(out, size, "%s", g_place);
}
int Seq() {
    uint32_t now = 0;
    return Memory::TryRead32(kNowSeq, now) ? int(now) : -1;
}
void SaveGame() {
    g_saveStatus = 1;
    Enqueue([](CpuContext* cpu) {
        uint32_t gp = 0;
        if (!Memory::TryRead32(kGp, gp) || !Memory::Contains(gp, 0xE0) || CallGuest(cpu, 0x8023C324, 0) != 0) {  // nandIsExec
            g_saveStatus = 3;
            return;
        }
        const uint32_t file = Memory::Read32(gp + 0xDC);                                     // the save file in play
        if (file > 3) { g_saveStatus = 3; return; }
        CallGuest(cpu, 0x8023C730, file);                                                    // nandUpdateSave
        CallGuest(cpu, 0x8023C4CC, file);                                                    // nandWriteSave
    });
}
int SaveStatus() { return g_saveStatus.load(); }
void SetInfiniteHp(bool on) { g_infiniteHp = on; }
void SetMaxCoins(bool on) { g_maxCoins = on; }
void SetInfiniteFlip(bool on) { g_infiniteFlip = on; }
void SetMaxArcadeTokens(bool on) { g_maxTokens = on; }
void AddScore(int amount) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, 0x8014CA14, uint32_t(amount)); });            // pouchAddXp
}
void AddAttack(int amount) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, 0x8014C8E4, uint32_t(amount)); });            // pouchAddAttack
}
void AddMaxHp(int amount) {
    Enqueue([=](CpuContext* cpu) {
        const int maxHp = int(CallGuest(cpu, 0x8014C9F4, 0)) + amount;                       // pouchGetMaxHp
        CallGuest(cpu, 0x8014C9C0, uint32_t(maxHp));                                         // pouchSetMaxHp
        CallGuest(cpu, 0x8014C920, uint32_t(maxHp));                                         // pouchSetHp: refilled too
    });
}
void SwitchCharacter(int index) {
    Enqueue([=](CpuContext* cpu) {
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                                      // marioGetPtr
        if (!Memory::Contains(mw, 0x40) || int8_t(Memory::Read8(mw + 0x38)) == index) return;
        if (!CallGuest(cpu, 0x8014D100, uint32_t(216 + index))) {                                // pouchCheckHaveItem
            CallGuest(cpu, 0x8014CB04, uint32_t(216 + index));                                   // pouchAddItem: not met yet
            CallGuest(cpu, 0x8014E4BC, uint32_t(216 + index));                                   // pouchMakeCharSelectable
        }
        CallGuest(cpu, 0x80146A7C, uint32_t(index));        // marioChangeCharacter, as the character menu does
        RT_LOGF(RT_TAG_RUNTIME, "cheat: switched to character %d (now %d)\n", index, int8_t(Memory::Read8(mw + 0x38)));
    });
}
void CycleCharacter() {
    Enqueue([](CpuContext* cpu) {
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                                      // marioGetPtr
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);                                   // pouchGetPtr
        if (!Memory::Contains(mw, 0x40) || !Memory::Contains(pouch, 0xC4)) return;
        const int current = int8_t(Memory::Read8(mw + 0x38));
        for (int step = 1; step < 4; ++step) {  // the next character the player has (pouch character slots, +0xB4)
            const int next = (current + step) % 4;
            if (int16_t(Memory::Read16(pouch + 0xB4 + 4 * next + 2)) == 216 + next) {
                CallGuest(cpu, 0x80146A7C, uint32_t(next));                                    // marioChangeCharacter
                return;
            }
        }
    });
}
void AddItemRange(int first, int last) {
    Enqueue([=](CpuContext* cpu) {
        int added = 0;
        for (int id = first; id <= last; ++id)
            if (!CallGuest(cpu, 0x8014D100, uint32_t(id)) && CallGuest(cpu, 0x8014CB04, uint32_t(id))) ++added;  // pouchCheckHaveItem, pouchAddItem
        RT_LOGF(RT_TAG_RUNTIME, "cheat: items %d-%d, %d added\n", first, last, added);
    });
}
bool PauseMenuOpen() {
    // pausewinWp (r13-0x75D0); its first word is the open flag (nonzero while the menu is up or animating).
    uint32_t wp = 0, flags = 0;
    return Memory::TryRead32(0x8056D950, wp) && Memory::TryRead32(wp, flags) && (flags & 1) != 0;
}
void RemoveCharOrPixl(int itemId) {
    // The game has no remove for these slots (pouchRemoveItem only handles item lists): make it not selectable with
    // the game's own call, then empty the slot (Pixls after it move up). The character played is left alone; the Pixl
    // in use hands over to another Pixl first.
    Enqueue([=](CpuContext* cpu) {
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);                                                // pouchGetPtr
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                                                  // marioGetPtr
        if (!Memory::Contains(pouch, 0x104) || !Memory::Contains(mw, 0x40)) return;
        if (itemId == 216 + int8_t(Memory::Read8(mw + 0x38))) return;                                        // the character played
        if (itemId == int(CallGuest(cpu, 0x8014E828, 0))) {                                                  // pouchGetCurPixl
            for (uint32_t i = 1; i < 16; ++i) {   // hand over to another Pixl first, if there is one
                const int other = int16_t(Memory::Read16(pouch + 0xC4 + 4 * i + 2));
                if (other != 0 && other != 220 && other != itemId) {
                    CallGuest(cpu, 0x8014E914, uint32_t(other));                                             // pouchSetPixlSelected
                    break;
                }
            }
        }
        for (const auto [base, count, pixl] : {std::tuple{0xB4u, 4u, false}, std::tuple{0xC4u, 16u, true}}) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t slot = pouch + base + 4 * i;
                if (int16_t(Memory::Read16(slot + 2)) != itemId) continue;
                CallGuest(cpu, pixl ? 0x8014E660u : 0x8014E524u, uint32_t(itemId));                          // pouchMake{Pixl,Char}NotSelectable
                if (itemId == 220) SetTippiEnabled(cpu, false);
                if (!pixl) {   // a character keeps its slot index: the slot is just emptied
                    Memory::Write32(slot, 0);
                    return;
                }
                for (uint32_t j = i; j + 1 < count; ++j) Memory::Write32(pouch + base + 4 * j, Memory::Read32(pouch + base + 4 * (j + 1)));
                Memory::Write32(pouch + base + 4 * (count - 1), 0);
                return;
            }
        }
    });
}
void Unlock(int itemId, bool pixl) {
    if (itemId == 220) {   // Tippi: the guide, not a pouch Pixl
        SetTippi(true);
        return;
    }
    Enqueue([=](CpuContext* cpu) {
        CallGuest(cpu, 0x8014CB04, uint32_t(itemId));                                                         // pouchAddItem
        CallGuest(cpu, pixl ? 0x8014E5A4u : 0x8014E4BCu, uint32_t(itemId));                                   // pouchMake{Pixl,Char}Selectable
    });
}
void AddCoins(int amount) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, 0x8014CA98, uint32_t(amount)); });                          // pouchAddCoin
}
void SetHp(int hp, int maxHp) {
    Enqueue([=](CpuContext* cpu) {
        CallGuest(cpu, 0x8014C9C0, uint32_t(maxHp));                                                          // pouchSetMaxHp
        CallGuest(cpu, 0x8014C920, uint32_t(hp));                                                             // pouchSetHp
    });
}
void SetSystemFlag(int flag, bool on) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, on ? 0x80037E90u : 0x80037F18u, uint32_t(flag)); });         // swSet / swClear
}
void SetGsw(int index, int value) {
    Enqueue([=](CpuContext* cpu) {
        if (index == 0) {                                                                                     // GSW(0) is a full s32 in the game work
            uint32_t gp = 0;
            if (Memory::TryRead32(kGp, gp) && Memory::Contains(gp, 0x144)) Memory::Write32(gp + 0x140, uint32_t(value));
        } else {
            CallGuest(cpu, 0x80037F58, uint32_t(index), uint32_t(value));                                     // swByteSet
        }
    });
}
void ReadSystemFlag(int flag) {
    Enqueue([=](CpuContext* cpu) { g_lastResult = int(CallGuest(cpu, 0x80037ED0, uint32_t(flag))); });         // swGet
}
int Gsw0() {
    uint32_t gp = 0;
    if (!Memory::TryRead32(kGp, gp) || gp < 0x80004000 || !Memory::Contains(gp, 0x144)) return -1;
    return int(Memory::Read32(gp + 0x140));
}
int LastResult() { return g_lastResult.load(); }

namespace {
std::mutex g_stateMutex;
DebugState g_state;
std::atomic<bool> g_statePending{false};
}

void RequestState() {
    if (g_statePending.exchange(true)) return;
    Enqueue([](CpuContext* cpu) {
        DebugState st;
        st.valid = true;
        Memory::TryRead32(kNowSeq, st.seq);
        uint32_t gp = 0;
        if (Memory::TryRead32(kGp, gp) && Memory::Contains(gp, 0x64))
            for (uint32_t i = 0; i < 31; ++i) st.map[i] = static_cast<char>(Memory::Read8(gp + 0x44 + i));
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                     // marioGetPtr
        if (Memory::Contains(mw, 0x320)) {
            for (int i = 0; i < 3; ++i) st.pos[i] = Memory::ReadFloat32(mw + 0x5C + 4 * i);
            st.held = Memory::Read16(mw + 0x30C);
            st.curChar = 216 + int8_t(Memory::Read8(mw + 0x38));                    // marioWork.character
        }
        st.curPixl = int(CallGuest(cpu, 0x8014E828, 0));                        // pouchGetCurPixl
        const uint32_t guide = CallGuest(cpu, 0x8011EFD0, 0);                   // guideGetWork
        st.tippi = Memory::Contains(guide, 2) && !(Memory::Read16(guide) & 0x80u);
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);                  // pouchGetPtr
        if (Memory::Contains(pouch, sizeof st.pouch))
            for (uint32_t i = 0; i < sizeof st.pouch; ++i) st.pouch[i] = Memory::Read8(pouch + i);
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state = st;
        }
        g_statePending = false;
    });
}
bool GetState(DebugState& out) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    out = g_state;
    return g_state.valid;
}
void SetPouch(int offset, int value) {
    Enqueue([=](CpuContext* cpu) {
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);
        if (offset >= 0 && offset + 4 <= 0x6A0 && Memory::Contains(pouch, 0x6A0)) Memory::Write32(pouch + offset, uint32_t(value));
    });
}
void SetVar(int type, int index, int value) {
    if (type == 0) { SetGsw(index, value); return; }
    Enqueue([=](CpuContext* cpu) {
        switch (type) {
        case 1: CallGuest(cpu, value ? 0x80037E90u : 0x80037F18u, uint32_t(index)); break;                   // swSet / swClear
        case 2: CallGuest(cpu, 0x800380C8, uint32_t(index), uint32_t(value)); break;                          // _swByteSet
        case 3: CallGuest(cpu, value ? 0x80038000u : 0x80038088u, uint32_t(index)); break;                    // _swSet / _swClear
        }
    });
}
void ReadVar(int type, int index) {
    Enqueue([=](CpuContext* cpu) {
        switch (type) {
        case 0: g_lastResult = index == 0 ? Gsw0() : int(CallGuest(cpu, 0x80037FDC, uint32_t(index))); break;  // swByteGet
        case 1: g_lastResult = int(CallGuest(cpu, 0x80037ED0, uint32_t(index))); break;                        // swGet
        case 2: g_lastResult = int(CallGuest(cpu, 0x800380D8, uint32_t(index))); break;                        // _swByteGet
        case 3: g_lastResult = int(CallGuest(cpu, 0x80038040, uint32_t(index))); break;                        // _swGet
        }
    });
}

namespace {
std::atomic<bool> g_watch{false};
std::mutex g_watchMutex;
std::vector<std::string> g_watchLog;
uint8_t g_flagPrev[8192];
uint8_t g_gswPrev[512];
bool g_watchPrimed = false;
uint32_t g_watchTick = 0;
float g_savedPos[3];
std::atomic<bool> g_hasSavedPos{false};

void WatchStep(CpuContext* cpu) {
    if (!g_watch) { g_watchPrimed = false; return; }
    if (++g_watchTick % 20) return;
    uint32_t now = 0, after = 0;
    if (!Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after) || now != 2 || after != 2) return;
    std::vector<std::string> changes;
    char line[64];
    for (int n = 0; n < 8192; ++n) {
        const uint8_t v = uint8_t(CallGuest(cpu, 0x80037ED0, uint32_t(n)) & 1);          // swGet
        if (g_watchPrimed && v != g_flagPrev[n]) {
            std::snprintf(line, sizeof line, "GSWF %d: %d -> %d", n, g_flagPrev[n], v);
            changes.emplace_back(line);
        }
        g_flagPrev[n] = v;
    }
    for (int n = 1; n < 512; ++n) {
        const uint8_t v = uint8_t(CallGuest(cpu, 0x80037FDC, uint32_t(n)));              // swByteGet
        if (g_watchPrimed && v != g_gswPrev[n]) {
            std::snprintf(line, sizeof line, "GSW %d: %d -> %d", n, g_gswPrev[n], v);
            changes.emplace_back(line);
        }
        g_gswPrev[n] = v;
    }
    static int lastGsw0 = -1;
    const int gsw0 = Gsw0();
    if (g_watchPrimed && gsw0 != lastGsw0) {
        std::snprintf(line, sizeof line, "GSW 0: %d -> %d", lastGsw0, gsw0);
        changes.emplace_back(line);
    }
    lastGsw0 = gsw0;
    g_watchPrimed = true;
    if (!changes.empty()) {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        for (auto& c : changes) g_watchLog.push_back(std::move(c));
        if (g_watchLog.size() > 400) g_watchLog.erase(g_watchLog.begin(), g_watchLog.begin() + (g_watchLog.size() - 400));
    }
}
}

void SetWatch(bool on) { g_watch = on; }
bool WatchEnabled() { return g_watch.load(); }
std::vector<std::string> GetWatchLog() {
    std::lock_guard<std::mutex> lock(g_watchMutex);
    return g_watchLog;
}
void ClearWatchLog() {
    std::lock_guard<std::mutex> lock(g_watchMutex);
    g_watchLog.clear();
}
void SavePosition() {
    Enqueue([](CpuContext* cpu) {
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);
        if (!Memory::Contains(mw, 0x70)) return;
        for (int i = 0; i < 3; ++i) g_savedPos[i] = Memory::ReadFloat32(mw + 0x5C + 4 * i);
        g_hasSavedPos = true;
    });
}
void SetGravity(int dir) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, 0x8012AA90, uint32_t(dir)); });                           // marioSetGravity
}
void TeleportTo(float x, float y, float z) {
    Enqueue([=](CpuContext* cpu) {
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                                                  // marioGetPtr
        if (!Memory::Contains(mw, 0x70)) return;
        Memory::WriteFloat32(mw + 0x5C, x);
        Memory::WriteFloat32(mw + 0x60, y);
        Memory::WriteFloat32(mw + 0x64, z);
    });
}
void LoadPosition() {
    Enqueue([](CpuContext* cpu) {
        if (!g_hasSavedPos) return;
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);
        if (!Memory::Contains(mw, 0x70)) return;
        for (int i = 0; i < 3; ++i) Memory::WriteFloat32(mw + 0x5C + 4 * i, g_savedPos[i]);
    });
}
bool HasSavedPosition() { return g_hasSavedPos.load(); }
void Heal() {
    Enqueue([](CpuContext* cpu) {
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);
        if (Memory::Contains(pouch, 0x14)) Memory::Write32(pouch + 0x0C, Memory::Read32(pouch + 0x10));
    });
}

namespace {
std::mutex g_flagSnapMutex;
uint8_t g_flagSnap[8192];
std::atomic<bool> g_flagSnapPending{false};
}

void RequestFlagSnapshot() {
    if (g_flagSnapPending.exchange(true)) return;
    Enqueue([](CpuContext* cpu) {
        static uint8_t fresh[8192];
        for (int n = 0; n < 8192; ++n) fresh[n] = uint8_t(CallGuest(cpu, 0x80037ED0, uint32_t(n)) & 1);   // swGet
        {
            std::lock_guard<std::mutex> lock(g_flagSnapMutex);
            std::memcpy(g_flagSnap, fresh, sizeof g_flagSnap);
        }
        g_flagSnapPending = false;
    });
}
bool FlagValue(int flag) {
    if (flag < 0 || flag >= 8192) return false;
    std::lock_guard<std::mutex> lock(g_flagSnapMutex);
    return g_flagSnap[flag] != 0;
}
} // namespace SpmMapSweep

// Anim pose log hook, called from the generated animPoseDrawMtx (func_800496B0) via
// switch/scripts/patch-anim-draw.py, which inserts the consult before the function's
// first block. Log-only in its original form: the hook returned false for every draw,
// so no draw was ever skipped; its job was evidence. (The tuple-keyed suppression
// that evidence bought lives further down, behind the wiisuppress switch.) The
// first attempt at this hook (911c18a) suppressed by
// archive name alone and was reverted (d383d9c) because that also took out the Controls
// screen's remote (whose draws fire the marker the replacement panel opens from) and
// the shake action commands' remote. A suppression that comes back has to key on the
// pose's animation, and that needs a hardware log naming which poses are which first.
//
// The walk mirrors what the draw code itself does: animdrv_wp is SDA-relative in the
// game code (r13 - 32680); the pose array is at AnimWork + 16 with its count at + 20,
// each AnimPose is 392 bytes, its group id is at + 16, the group array is at AnimWork
// + 0 with 16-byte entries whose file entry pointer is at + 8, and FileEntry.path is
// at + 0x20 (64 bytes). Only poses drawn from an n_controler archive are logged (the
// demo Wii Remote's); each distinct combination of the logged fields logs once, with
// the map and sequence it was seen in riding along, so a teaching pose can be told
// apart from the Controls screen's and the shake command's in the same run.
//
// Enrichment (lab-poselog2, still log-only): the line also carries the gate signals
// the Nov 4 source audit named and the pose's animation identity, so one hardware
// session can settle which poses are which. msgwin / pausewin are presence flags
// for the message-window and pause-window work pointers (kMsgDrvMsgW / kPauseWinWp,
// the same variables RttLogStep logs raw): teaching poses are presented under a
// message window, the Controls screen's remote under the pause window. nextseq is
// kNextSeq beside the existing seq. anim is AnimPose + 20, the pose's current
// animation index within its group's animation table -- animPoseSetAnim stores
// the name/index resolver's result there (func_80042DB0: a passthrough index, or
// the strcmp match over the group's AnimData table), and animPoseDrawMtx's own
// anim-switch sequence compares and rewrites it, so the game itself treats +20
// as "the current anim". link is AnimPose + 152, the linked pose slot
// animPoseDrawMtx follows before drawing (bounds-checked against the pose count,
// -1 = no link); when it names a valid slot, lgroup / lanim are the linked pose's
// group id and animation index, the identity of what is actually drawn. All of
// these are table/slot indices, never addresses, so they are stable across boots
// in a way the (group, pose) slot pair alone was never proven to be.
//
// The guide fields come from the Nov 4 logic trace: every message/event
// teaching demo draws through ONE AnimPose owned by guide.c, whose work struct
// (guest 0x804CD398) holds that pose's slot at +8 and the current animation's
// name pointer at +12. guide is 1 when the slot being drawn equals the live
// slot word (gvalid is 0, and guide forced to 0, when that word cannot be
// read), and ganim is the name the guide last set -- the archive's clip name
// (A_1, C_1, ...), so a teaching draw identifies itself by clip. The shake
// command and the Controls window pose the model on separate paths that never
// touch guide work, so a guide=1 row is a teaching draw by construction and a
// guide=0 row in a protected context is the control sample.
//
// The suppression (stderr56 verdict, Nov 5): on hardware the guide predicate
// FAILED as specified -- the Tippi Tattle tutorial's draws read guide 0, and
// ganim read "S_2" in the teaching and protected contexts alike, so the
// 0x804CD3A0/+12 derivation is itself suspect and no enriched field separates
// the contexts. What does, across every capture ever taken, is the (group,
// pose) tuple of the draw itself: teaching (Tippi tutorial) is group 20 pose
// 32 in he1_01, the protected help window is group 8 pose 9 in mac_08, the
// Shooting Star action command is group 19 pose 31, and ordinary gameplay
// draws nothing. Behind the "wiisuppress" dk_diag key (default off), the hook
// therefore skips exactly the (20, 32) n_controler draw and returns true for
// it; each skip is counted and logged (first fire and every 60th), so a
// hardware run proves what was suppressed. No other tuple is suppressed, and
// no enriched field takes part in the predicate. device.cpp forwards the
// switch through spm_wii_suppress_set, the same handshake as poselog.
//
// The switch is the "poselog" dk_diag key: device.cpp forwards it here through
// spm_pose_log_set, from diag_init at boot and from the Diagnostics page at runtime.
namespace {
std::atomic<bool> g_poseLog{false};
std::atomic<bool> g_wiiSuppress{false};
}  // namespace

extern "C" void spm_pose_log_set(bool enabled) { g_poseLog = enabled; }
extern "C" void spm_wii_suppress_set(bool enabled) { g_wiiSuppress = enabled; }

// The "rttlog" dk_diag key's other end: device.cpp forwards it here, from diag_init at
// boot and from diag_set at runtime, the same handshake as spm_pose_log_set above. The
// flag and the logger live with the Tick code (RttLogStep, in the SpmMapSweep namespace
// of this same file).
extern "C" void spm_rtt_log_set(bool enabled) { SpmMapSweep::g_rttLog.store(enabled); }

// The evt NPC guard's log end (Nov 5 npcdrv investigation). patch-npc-guard.py
// rewrites a fixed set of generated evt NPC command handlers in the build copy
// so a name that resolves to no active NPC takes the game's own NoAssert
// lookup and skips the command instead of tripping npcNameToPtr's not-found
// assert (npcdrv.c:7055 -- the panic that ended the Nov 4 PGO training session
// and Brandon's Nov 5 recorder run, both in gn1_01's opening script positioning
// "hiroshi" before he exists). The generated side calls this on the skip so the
// class stays visible in the log. nameAddr is the guest address of the script's
// name string, or 0 when the handler could not preserve it across the call.
// Rate-limited like the wiisuppress fire line above: first miss and every 60th,
// with the running count in the line.
extern "C" void spm_npc_guard_miss(uint32_t nameAddr, const char* site) {
    static std::atomic<uint32_t> misses{0};
    const uint32_t n = misses.fetch_add(1) + 1;
    if (n != 1 && n % 60 != 0) return;
    char place[48] = {};
    SpmMapSweep::CurrentPlace(place, sizeof place);
    char name[33] = {};
    bool named = false;
    if (nameAddr != 0 && Memory::Contains(nameAddr, 32)) {
        named = true;
        for (int i = 0; i < 32; ++i) {
            const uint8_t b = Memory::Read8(nameAddr + uint32_t(i));
            if (b == 0) break;
            if (b < 0x20 || b > 0x7E) { named = false; break; }
            name[i] = static_cast<char>(b);
        }
        if (name[0] == 0) named = false;
    }
    if (named)
        RT_LOGF(RT_TAG_RUNTIME, "[npcdrv] %s: no NPC named \"%s\" -- command skipped (miss %u) at %s\n",
                site, name, n, place);
    else
        RT_LOGF(RT_TAG_RUNTIME,
                "[npcdrv] %s: NPC lookup missed (name @0x%08X unreadable) -- command skipped (miss %u) at %s\n",
                site, nameAddr, n, place);
}

extern "C" bool spm_suppress_anim_pose(CpuContext* ctx) {
    if (ctx == nullptr) return false;
    if (!g_poseLog.load() && !g_wiiSuppress.load()) return false;
    const uint32_t id = ctx->gpr[3];
    uint32_t wp = 0;
    if (!Memory::TryRead32(ctx->gpr[13] - 32680u, wp) || wp == 0) return false;
    uint32_t poses = 0, count = 0;
    if (!Memory::TryRead32(wp + 16, poses) || !Memory::TryRead32(wp + 20, count)) return false;
    if (poses == 0 || id >= count) return false;
    uint32_t groupId = 0;
    if (!Memory::TryRead32(poses + id * 392 + 16, groupId)) return false;
    uint32_t groups = 0;
    if (!Memory::TryRead32(wp, groups) || groups == 0) return false;
    uint32_t file = 0;
    if (!Memory::TryRead32(groups + groupId * 16 + 8, file) || file == 0) return false;
    if (!Memory::Contains(file + 0x20, 64)) return false;
    char path[65] = {};
    for (int i = 0; i < 64; ++i) {
        const uint8_t b = Memory::Read8(file + 0x20 + uint32_t(i));
        if (b == 0) break;
        path[i] = static_cast<char>(b);
    }
    if (path[0] == 0) return false;
    // Every anim pose draw in the game comes through here while wiisuppress is on (the
    // default), so the archive test runs on the copied bytes with no allocation; only an
    // n_controler draw goes on to the string, the place lookup and the logging below.
    const size_t len = std::strlen(path);
    const auto isArchive = [&](const char* name) {
        const size_t n = std::strlen(name);
        return len >= n && std::memcmp(path + len - n, name, n) == 0 && (len == n || path[len - n - 1] == '/');
    };
    if (!isArchive("n_controler") && !isArchive("n_controler-")) return false;
    const std::string p(path);
    char place[48] = {};
    SpmMapSweep::CurrentPlace(place, sizeof place);
    const int seq = SpmMapSweep::Seq();
    // The suppression itself (see the header comment): with wiisuppress on, the one
    // measured teaching tuple is skipped -- the generated draw function returns at
    // once on a true from here. This sits ahead of the dedup table below on purpose:
    // a repeat draw of an already-logged combination must still be skipped, and the
    // table's early return would let it through. The fire line carries the running
    // count, rate-limited to the first fire and every 60th, so a run's log shows
    // exactly where suppression fired and how often without flooding at draw rate.
    // (group, pose) are slot numbers the game hands out as models load, not fixed IDs, so
    // the tuple is only known to mean the Tippi tutorial in the map it was measured in.
    // Elsewhere, or in he1_01 once the tutorial's pose is freed and the slot is reused, the
    // same numbers could land on the Controls screen's remote, and hiding that one stops
    // the replacement Controls panel from opening (its marker fires from that remote's
    // draws). Gating on he1_01 keeps the skip to the map the evidence covers; extending
    // it to another tutorial means a poselog capture of that map first.
    if (g_wiiSuppress.load() && groupId == 20 && id == 32 && std::strncmp(place, "he1_01 ", 7) == 0) {
        static std::atomic<uint32_t> fired{0};
        const uint32_t n = fired.fetch_add(1) + 1;
        if (n == 1 || n % 60 == 0)
            RT_LOGF(RT_TAG_RUNTIME, "[wiisuppress] suppressed group %u pose %u at %s (count %u)\n", groupId, id,
                    place, n);
        return true;
    }
    if (!g_poseLog.load()) return false;
    // Enrichment reads, all TryRead32 like everything else here. Presence flags are
    // 1 = pointer non-null, 0 = null, -1 = unreadable; nextseq is -1 when unreadable
    // (the Seq() convention). anim / link are the raw AnimPose words as signed
    // values (link's -1 is a real value, "no link"); -2 marks an unreadable word.
    uint32_t msgwRaw = 0, pauseRaw = 0, nextRaw = 0;
    const int msgwin = Memory::TryRead32(SpmMapSweep::kMsgDrvMsgW, msgwRaw) ? (msgwRaw != 0 ? 1 : 0) : -1;
    const int pausewin = Memory::TryRead32(SpmMapSweep::kPauseWinWp, pauseRaw) ? (pauseRaw != 0 ? 1 : 0) : -1;
    const int nextseq = Memory::TryRead32(SpmMapSweep::kNextSeq, nextRaw) ? int(nextRaw) : -1;
    const uint32_t poseBase = poses + id * 392;
    uint32_t animRaw = 0, linkRaw = 0;
    const int anim = Memory::TryRead32(poseBase + 20, animRaw) ? int(static_cast<int32_t>(animRaw)) : -2;
    const int link = Memory::TryRead32(poseBase + 152, linkRaw) ? int(static_cast<int32_t>(linkRaw)) : -2;
    int lgroup = -1, lanim = -1;
    if (link >= 0 && uint32_t(link) < count) {
        uint32_t lgRaw = 0, laRaw = 0;
        if (Memory::TryRead32(poses + uint32_t(link) * 392 + 16, lgRaw)) lgroup = int(lgRaw);
        if (Memory::TryRead32(poses + uint32_t(link) * 392 + 20, laRaw)) lanim = int(static_cast<int32_t>(laRaw));
    }
    // The guide match: guide=1 when the slot being drawn is the guide pose's
    // live slot (guide work +8). On any doubt the match is 0 and gvalid is 0,
    // so a bad read can never masquerade as a guide draw. ganim is the guide's
    // current animation name (guide work +12): the archive clip name the
    // guide subsystem last set (A_1, C_1, ...), copied with the same
    // Contains-then-bytes pattern as the file path above, bounded at 24
    // printable-ASCII bytes; when the pointer or the bytes fail those checks
    // it falls back to the pointer's hex form.
    uint32_t guideSlotRaw = 0;
    const int gvalid = Memory::TryRead32(SpmMapSweep::kGuidePoseSlot, guideSlotRaw) ? 1 : 0;
    const int guide = (gvalid && guideSlotRaw == id) ? 1 : 0;
    char ganim[25] = {};
    uint32_t namePtr = 0;
    bool nameOk = Memory::TryRead32(SpmMapSweep::kGuideAnimName, namePtr) && namePtr != 0 &&
                  Memory::Contains(namePtr, 24);
    if (nameOk) {
        int n = 0;
        for (int i = 0; i < 24; ++i) {
            const uint8_t b = Memory::Read8(namePtr + uint32_t(i));
            if (b == 0) break;
            if (b < 0x20 || b > 0x7E) {
                nameOk = false;
                break;
            }
            ganim[n++] = static_cast<char>(b);
        }
        if (n == 0) nameOk = false;
    }
    if (!nameOk) std::snprintf(ganim, sizeof ganim, "0x%08X", namePtr);
    // One log line per distinct combination of every logged field, so a test run
    // names every pose the demo remote is drawn in -- and the same pose seen under
    // a message window and again without one logs twice, which is the point --
    // without flooding the log at 60 draws a second.
    static char seenPath[128][64] = {};
    static uint32_t seenGroup[128] = {};
    static uint32_t seenPose[128] = {};
    static char seenPlace[128][48] = {};
    static int seenMsgwin[128] = {};
    static int seenPausewin[128] = {};
    static int seenNextseq[128] = {};
    static int seenAnim[128] = {};
    static int seenLink[128] = {};
    static int seenLgroup[128] = {};
    static int seenLanim[128] = {};
    static int seenGuide[128] = {};
    static int seenGvalid[128] = {};
    static char seenGanim[128][25] = {};
    static int seenCount = 0;
    for (int i = 0; i < seenCount; ++i)
        if (seenGroup[i] == groupId && seenPose[i] == id && p == seenPath[i] && std::strcmp(place, seenPlace[i]) == 0 &&
            seenMsgwin[i] == msgwin && seenPausewin[i] == pausewin && seenNextseq[i] == nextseq &&
            seenAnim[i] == anim && seenLink[i] == link && seenLgroup[i] == lgroup && seenLanim[i] == lanim &&
            seenGuide[i] == guide && seenGvalid[i] == gvalid && std::strcmp(ganim, seenGanim[i]) == 0)
            return false;
    if (seenCount < 128) {
        std::snprintf(seenPath[seenCount], 64, "%s", p.c_str());
        seenGroup[seenCount] = groupId;
        seenPose[seenCount] = id;
        std::snprintf(seenPlace[seenCount], 48, "%s", place);
        seenMsgwin[seenCount] = msgwin;
        seenPausewin[seenCount] = pausewin;
        seenNextseq[seenCount] = nextseq;
        seenAnim[seenCount] = anim;
        seenLink[seenCount] = link;
        seenLgroup[seenCount] = lgroup;
        seenLanim[seenCount] = lanim;
        seenGuide[seenCount] = guide;
        seenGvalid[seenCount] = gvalid;
        std::snprintf(seenGanim[seenCount], 25, "%s", ganim);
        ++seenCount;
        RT_LOGF(RT_TAG_RUNTIME,
                "[poselog] file '%s' group %u pose %u at %s (seq %d) msgwin %d pausewin %d nextseq %d anim %d "
                "link %d lgroup %d lanim %d guide %d gvalid %d ganim %s\n",
                p.c_str(), groupId, id, place, seq, msgwin, pausewin, nextseq, anim, link, lgroup, lanim, guide,
                gvalid, ganim);
    }
    return false;
}
