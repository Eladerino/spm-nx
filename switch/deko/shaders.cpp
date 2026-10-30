// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The uam worker follows SwitchWaker (https://github.com/centollOS/SwitchWaker) at 92307ff:
// switch/deko/shaders.cpp, itself after SwitchWakerHD's shaders_dk.cpp (one uam thread with a large
// stack, failure dumps, a cache file appended as shaders compile). The registry and the file format
// are spm-nx's: keyed by the WGSL itself, so a cached shader needs no Tint run at all.
// Shaders at run time (dk.h compile_stage):
//   - the registry: every compiled stage by key = FNV-1a 64 of (WGSL, entry point, stage), filled from
//     the cache file at start and by compiles; a stage another thread is compiling is waited for;
//   - a miss: Tint (shader_translate.cpp) on the calling thread under one lock, then uam on the uam
//     worker (uam is not thread-safe and needs 8 MiB of stack), the caller waiting;
//   - the cache file, kCachePath: a header with the compiler id (uam, its patches, the translation
//     revision, this format), then one record per compiled stage, appended as it compiles; a file of
//     another compiler is started again, a record with a bad CRC or cut short is skipped.
//       header  {char magic[8] = "SPMDKSH1", u64 compilerId}
//       record  {u32 'RECD', u32 payloadSize, u32 crc32(payload), payload}
//       payload {u64 key, u8 stage, u8 bindingCount, u16 0, u32 workgroup[3],
//                bindingCount x {u8 kind, u8 slot, u8 group, u8 binding, u16 samplerGroup,
//                                u8 samplerBinding, u8 0},
//                DKSH bytes (the rest)}
#include "dk.h"

#include <pthread.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "uam_api.h"

namespace swdk {
namespace {

constexpr const char* kCachePath = "/switch/spm-nx/UserData/dksh_cache.bin";
// The settings menu's "Clear shader cache" row leaves this sentinel (the
// path is hardcoded there too); shaders_init consumes it. See there for why
// the cache is deleted at startup and not when the row is pressed.
constexpr const char* kClearPath = "/switch/spm-nx/UserData/dksh_cache.clear";
constexpr const char* kFailDir = "/switch/spm-nx/UserData/Logs";
constexpr char kMagic[8] = {'S', 'P', 'M', 'D', 'K', 'S', 'H', '1'};
constexpr uint32_t kRecordMagic = 0x44434552;  // "RECD"
constexpr char kCompilerName[] = "uam 1.1.0 (devkitPro/uam 5a5afc2) + switch/uam/PATCHES.md 1-9";
constexpr size_t kWorkerStack = 8u << 20;
constexpr int kFailDumps = 16;

uint64_t fnv(const void* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint64_t compiler_id() {
    uint64_t h = fnv(kCompilerName, sizeof kCompilerName);
    const uint32_t rev = kTranslateRevision;
    h = fnv(&rev, sizeof rev, h);
    return fnv(kMagic, sizeof kMagic, h);
}

const char* stage_name(ShaderStage s) {
    return s == ShaderStage::Vertex ? "vertex" : s == ShaderStage::Fragment ? "fragment" : "compute";
}

// ---- the registry
struct Entry {
    enum State { Pending, Ready, Failed } state = Pending;
    CompiledStage stage;
};
std::mutex g_mutex;
std::condition_variable g_cv;  // an entry left Pending
std::unordered_map<uint64_t, std::unique_ptr<Entry>> g_entries;
ShaderStats g_stats;
std::mutex g_translateMutex;  // Tint: one translation at a time
std::mutex g_fileMutex;
int g_failDumps = 0;

// ---- the uam worker
struct Job {
    ShaderStage stage;
    const std::string* glsl;
    uam::Result result;
    bool done = false;
};
std::mutex g_jobMutex;
std::condition_variable g_jobCv, g_jobDoneCv;
std::deque<Job*> g_jobs;
bool g_workerUp = false;

uam::Stage uam_stage(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return uam::Stage::Vertex;
    case ShaderStage::Fragment: return uam::Stage::Fragment;
    default: return uam::Stage::Compute;
    }
}

void* worker_main(void*) {
    const uint64_t t0 = now_ns();
    uam::init(true);
    static const char kTest[] = "#version 460\nvoid main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    const uam::Result test = uam::compile(uam::Stage::Vertex, kTest);
    dklog("shaders: uam worker started (%zu MiB stack); self-test %s (%zu bytes) in %.1f ms", kWorkerStack >> 20,
          test.ok ? "compiled" : "FAILED", test.dksh.size(), double(now_ns() - t0) / 1e6);
    for (;;) {
        Job* job;
        {
            std::unique_lock<std::mutex> lock(g_jobMutex);
            g_jobCv.wait(lock, [] { return !g_jobs.empty(); });
            job = g_jobs.front();
            g_jobs.pop_front();
        }
        uam::Result res = uam::compile(uam_stage(job->stage), job->glsl->c_str());
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            job->result = std::move(res);
            job->done = true;
        }
        g_jobDoneCv.notify_all();
    }
    return nullptr;
}

uam::Result run_uam(ShaderStage stage, const std::string& glsl) {
    if (!g_workerUp) {
        uam::Result r;
        r.log = "the uam worker did not start";
        return r;
    }
    Job job{stage, &glsl, {}, false};
    {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        g_jobs.push_back(&job);
    }
    g_jobCv.notify_one();
    std::unique_lock<std::mutex> lock(g_jobMutex);
    g_jobDoneCv.wait(lock, [&] { return job.done; });
    return std::move(job.result);
}

void dump_failure(uint64_t key, ShaderStage stage, const std::string& source, const std::string& why,
                  const char* kind) {
    std::lock_guard<std::mutex> lock(g_fileMutex);
    if (g_failDumps >= kFailDumps) return;
    g_failDumps++;
    char path[256];
    snprintf(path, sizeof path, "%s/shaderfail_%016llx_%s.%s", kFailDir, (unsigned long long)key, stage_name(stage),
             kind);
    if (FILE* f = fopen(path, "wb")) {
        fwrite(source.data(), 1, source.size(), f);
        fprintf(f, "\n/* %s */\n", why.c_str());
        fclose(f);
        dklog("shaders: the failing source is in %s", path);
    }
}

// diag shaderdump: one shader's WGSL and the GLSL Tint made from it, saved side by side under the
// shader's cache key so the generated code behind a suspect draw can be read off the card. Called
// on a compile_stage miss only, so each unique shader is written once; a failed write is logged
// and otherwise ignored (the compile carries on either way).
void dump_shader_sources(uint64_t key, const std::string& wgsl, const std::string& glsl) {
    std::lock_guard<std::mutex> lock(g_fileMutex);
    char wgslPath[256], glslPath[256];
    snprintf(wgslPath, sizeof wgslPath, "%s/shaderdump_%016llx.wgsl", kFailDir, (unsigned long long)key);
    snprintf(glslPath, sizeof glslPath, "%s/shaderdump_%016llx.glsl", kFailDir, (unsigned long long)key);
    bool ok = false;
    if (FILE* f = fopen(wgslPath, "wb")) {
        ok = fwrite(wgsl.data(), 1, wgsl.size(), f) == wgsl.size();
        fclose(f);
    }
    if (ok) {
        ok = false;
        if (FILE* f = fopen(glslPath, "wb")) {
            ok = fwrite(glsl.data(), 1, glsl.size(), f) == glsl.size();
            fclose(f);
        }
    }
    if (ok) {
        dklog("diag shaderdump: saved shaderdump_%016llx.wgsl/.glsl (wgsl %zu bytes, glsl %zu bytes)",
              (unsigned long long)key, wgsl.size(), glsl.size());
    } else {
        dklog("diag shaderdump: could not write shaderdump_%016llx.wgsl/.glsl", (unsigned long long)key);
    }
}

template <class T> void put(std::vector<uint8_t>& out, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof v);
}

void append_record(uint64_t key, ShaderStage stage, const CompiledStage& cs, const std::vector<uint8_t>& dksh) {
    std::vector<uint8_t> payload;
    put(payload, key);
    put(payload, uint8_t(stage));
    put(payload, uint8_t(cs.bindings.size()));
    put(payload, uint16_t(0));
    for (uint32_t w : cs.workgroup) put(payload, w);
    for (const SlotBinding& b : cs.bindings) {
        put(payload, uint8_t(b.kind));
        put(payload, b.slot);
        put(payload, b.group);
        put(payload, b.binding);
        put(payload, b.samplerGroup);
        put(payload, b.samplerBinding);
        put(payload, uint8_t(0));
    }
    payload.insert(payload.end(), dksh.begin(), dksh.end());
    std::vector<uint8_t> rec;
    put(rec, kRecordMagic);
    put(rec, uint32_t(payload.size()));
    put(rec, crc32(payload.data(), payload.size()));
    rec.insert(rec.end(), payload.begin(), payload.end());
    std::lock_guard<std::mutex> lock(g_fileMutex);
    FILE* f = fopen(kCachePath, "ab");
    if (!f) {
        static int logged = 0;
        if (logged++ < 3) dklog("shaders: cannot append to %s", kCachePath);
        return;
    }
    fwrite(rec.data(), 1, rec.size(), f);
    fclose(f);
}

void write_header() {
    if (FILE* w = fopen(kCachePath, "wb")) {
        const uint64_t id = compiler_id();
        fwrite(kMagic, 1, sizeof kMagic, w);
        fwrite(&id, 1, sizeof id, w);
        fclose(w);
    }
}

void read_cache_file() {
    const uint64_t t0 = now_ns();
    FILE* f = fopen(kCachePath, "rb");
    if (!f) {
        dklog("shaders: no %s yet: every shader is compiled on first use this run (slow the first time only)",
              kCachePath);
        write_header();
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? size_t(size) : 0);
    const bool read = size > 0 && fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    uint64_t id = 0;
    if (!read || data.size() < 16 || memcmp(data.data(), kMagic, 8) != 0 ||
        (memcpy(&id, data.data() + 8, 8), id != compiler_id())) {
        dklog("shaders: %s is from another compiler or unreadable: started again", kCachePath);
        write_header();
        return;
    }
    size_t at = 16, loaded = 0, bad = 0;
    while (at + 12 <= data.size()) {
        uint32_t magic, len, crc;
        memcpy(&magic, &data[at], 4);
        memcpy(&len, &data[at + 4], 4);
        memcpy(&crc, &data[at + 8], 4);
        if (magic != kRecordMagic || at + 12 + len > data.size()) break;  // cut short: the end
        const uint8_t* p = &data[at + 12];
        at += 12 + len;
        if (crc32(p, len) != crc || len < 24) {
            bad++;
            continue;
        }
        uint64_t key;
        memcpy(&key, p, 8);
        const ShaderStage stage = ShaderStage(p[8]);
        const uint32_t count = p[9];
        auto e = std::make_unique<Entry>();
        memcpy(e->stage.workgroup, p + 12, 12);
        size_t q = 24;
        if (q + count * 8 > len) {
            bad++;
            continue;
        }
        for (uint32_t i = 0; i < count; i++, q += 8) {
            SlotBinding b;
            b.kind = SlotBinding::Kind(p[q]);
            b.slot = p[q + 1];
            b.group = p[q + 2];
            b.binding = p[q + 3];
            memcpy(&b.samplerGroup, p + q + 4, 2);
            b.samplerBinding = p[q + 6];
            e->stage.bindings.push_back(b);
        }
        char name[48];
        snprintf(name, sizeof name, "cached %s %016llx", stage_name(stage), (unsigned long long)key);
        if (!code_load(e->stage.shader, p + q, uint32_t(len - q), name)) {
            bad++;
            continue;
        }
        e->state = Entry::Ready;
        g_entries[key] = std::move(e);
        loaded++;
    }
    g_stats.fromFile = loaded;
    dklog("shaders: %s: %zu compiled stages loaded in %.0f ms (%zu bad records), %u KiB of code", kCachePath, loaded,
          double(now_ns() - t0) / 1e6, bad, code_used() >> 10);
}

}  // namespace

const CompiledStage* compile_stage(const std::string& wgsl, const std::string& entryPoint, ShaderStage stage) {
    uint64_t key = fnv(wgsl.data(), wgsl.size());
    key = fnv(entryPoint.data(), entryPoint.size(), key);
    const uint8_t st = uint8_t(stage);
    key = fnv(&st, 1, key);
    Entry* e;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        auto it = g_entries.find(key);
        if (it != g_entries.end()) {
            e = it->second.get();
            g_cv.wait(lock, [e] { return e->state != Entry::Pending; });
            return e->state == Entry::Ready ? &e->stage : nullptr;
        }
        auto owned = std::make_unique<Entry>();
        e = owned.get();
        g_entries.emplace(key, std::move(owned));
    }
    const auto finish = [&](Entry::State s) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            e->state = s;
            if (s == Entry::Failed) g_stats.failed++;
        }
        g_cv.notify_all();
        return s == Entry::Ready ? &e->stage : nullptr;
    };

    const uint64_t t0 = now_ns();
    TranslateResult t;
    {
        std::lock_guard<std::mutex> lock(g_translateMutex);
        t = wgsl_to_glsl(wgsl, entryPoint, stage);
    }
    if (!t.ok) {
        dklog("shaders: %s entry '%s' (%016llx) does not translate: %s", stage_name(stage), entryPoint.c_str(),
              (unsigned long long)key, t.error.substr(0, 400).c_str());
        dump_failure(key, stage, wgsl, t.error, "wgsl");
        return finish(Entry::Failed);
    }
    // diag shaderdump: the shaders whose WGSL indexes texture matrices per vertex (in_texmtxidx)
    // are the suspects behind the tree, door and crowd corruption; save their sources for reading.
    if (g_diag.shaderDump)
        dump_shader_sources(key, wgsl, t.glsl);
    const uint64_t t1 = now_ns();
    uam::Result res = run_uam(stage, t.glsl);
    const uint64_t t2 = now_ns();
    if (!res.ok || res.dksh.empty()) {
        dklog("shaders: uam refused %s entry '%s' (%016llx, %.0f ms): %s", stage_name(stage), entryPoint.c_str(),
              (unsigned long long)key, double(t2 - t1) / 1e6, res.log.substr(0, 400).c_str());
        dump_failure(key, stage, t.glsl, res.log, "glsl");
        return finish(Entry::Failed);
    }
    e->stage.bindings = t.bindings;
    memcpy(e->stage.workgroup, t.workgroup, sizeof t.workgroup);
    char name[48];
    snprintf(name, sizeof name, "%s %016llx", stage_name(stage), (unsigned long long)key);
    if (!code_load(e->stage.shader, res.dksh.data(), uint32_t(res.dksh.size()), name)) return finish(Entry::Failed);
    append_record(key, stage, e->stage, res.dksh);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stats.compiled++;
        g_stats.compileNs += t2 - t0;
        if (t2 - t0 > g_stats.compileMaxNs) g_stats.compileMaxNs = t2 - t0;
        if (g_stats.compiled <= 8 || g_stats.compiled % 50 == 0)
            dklog("shaders: %llu compiled this run (%s '%s': Tint %.1f ms, uam %.0f ms)",
                  (unsigned long long)g_stats.compiled, stage_name(stage), entryPoint.c_str(), double(t1 - t0) / 1e6,
                  double(t2 - t1) / 1e6);
    }
    return finish(Entry::Ready);
}

void shaders_init() {
    // A cache clear the settings menu asked for on a previous run. The
    // delete has to happen here, before anything reads or appends to the
    // cache: append_record reopens the file for every shader compiled, all
    // session long, so a delete from inside the session would be undone by
    // the next compile (and the recreated file would have no header, so
    // read_cache_file would discard it at load anyway). Consume the
    // sentinel, delete both files, and let the load below start again.
    if (FILE* sentinel = fopen(kClearPath, "rb")) {
        fclose(sentinel);
        remove(kCachePath);
        remove(kClearPath);
        dklog("shaders: cache cleared on request (settings menu sentinel): starting again");
    }
    read_cache_file();
    translate_init();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kWorkerStack);
    pthread_t thread;
    const int rc = pthread_create(&thread, &attr, worker_main, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        dklog("shaders: cannot start the uam worker (%d): shaders the cache lacks will never draw", rc);
        return;
    }
    pthread_detach(thread);
    g_workerUp = true;
}

ShaderStats shader_stats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_stats;
}

}  // namespace swdk
