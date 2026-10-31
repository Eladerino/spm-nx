// NSMBW_PROFILE=<start s>:<length s> (Windows): a sampling profiler for the game thread. From
// <start> seconds after the first presented frame, for <length> seconds, the thread that presents
// frames is suspended about a thousand times a second and its instruction pointer recorded. At the
// end, samples are mapped back to the translated guest function whose code they fall in (by each
// translated function's host entry point) and the top entries are logged with their guest address,
// so a slow scene can be traced to the game code (or the runtime and aurora code) it spends time in.
//
// Switch: the capture backend is Horizon's (SetSwitchProfileEnabled, below). The "profile" dk_diag
// switch (dk_diag.txt, the dk_cmd channel, the settings menu's Developer row) starts a sampler
// thread that pauses the main thread - the guest runs there; Aurora's frame worker only encodes
// and presents sealed frames - about 200 times a second and records its PC, LR and frame-pointer
// chain. Turning the switch off prints the hot list to stderr.log in the same style as the
// Windows report: top guest functions by leaf samples, then the top call chains.
#include "guest_profiler.h"

#include "abi_bridge.h"
#include "guest_profile_format.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(__SWITCH__)
#include <switch.h>
#endif

extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}

namespace GuestProfiler {
namespace {

#if defined(_WIN32) || defined(__SWITCH__)
// The decomp name for a guest address from the generated symbol tables (SPM's MAP.txt names;
// coverage skews to the PAL build, so an unnamed function just gets its address).
const char* GuestName(uint32_t address) {
    const uint32_t* begin = kGuestMapSymbolAddresses;
    const uint32_t* end = kGuestMapSymbolAddresses + kGuestMapSymbolCount;
    const uint32_t* it = std::lower_bound(begin, end, address);
    return it != end && *it == address ? kGuestMapSymbolNames[it - begin] : "";
}
#endif

#ifdef _WIN32
struct Entry {
    uintptr_t host;
    uint32_t guest;
};

void Report(const std::unordered_map<uintptr_t, uint32_t>& samples, const std::vector<std::vector<uintptr_t>>& stacks,
            uint32_t total) {
    // Every translated function's host entry point, sorted, to find which one a sample falls in.
    std::vector<Entry> entries;
    for (const auto& [lo, hi] : {std::pair{0x80004000u, 0x80319180u}, std::pair{0x80BF691Cu, 0x80C5A648u}}) {  // SPM main.dol text, relF text
        for (uint32_t address = lo; address < hi; address += 4) {
            if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                info != nullptr && info->entryPoint != nullptr && info->address == address) {
                entries.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
    // The engine module (engine_usa.dll / engine_pal.dll since the loader split), not the small loader exe.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&OnFramePresented), &self);
    const auto exeBase = reinterpret_cast<uintptr_t>(self ? self : GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exeBase + dos->e_lfanew);
    const uintptr_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;

    // A host address as a translated guest function, or exe+RVA (resolve with llvm-nm), or [dll].
    const auto describe = [&](uintptr_t rip) -> std::string {
        if (rip >= exeBase && rip < exeEnd) {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            // Translated code sits in its own range: past the last entry point it is runtime code.
            if (it != entries.begin() && it != entries.end()) {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                return text;
            }
            char text[40];
            std::snprintf(text, sizeof(text), "exe+%llX", static_cast<unsigned long long>(rip - exeBase));
            return text;
        }
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &module)) {
            GetModuleFileNameA(module, path, MAX_PATH);
        }
        const char* slash = std::strrchr(path, '\\');
        return std::string("[") + (slash ? slash + 1 : path) + "]";
    };
    // Raw stacks (exe RVAs, "-" for other modules) for offline symbolization: nm the exe, see AGENTS.md.
    if (FILE* raw = std::fopen("profile_stacks.txt", "w")) {
        for (const auto& stack : stacks) {
            for (const uintptr_t rip : stack) {
                if (rip >= exeBase && rip < exeEnd) {
                    std::fprintf(raw, "%llX ", static_cast<unsigned long long>(rip - exeBase));
                } else {
                    std::fputs("- ", raw);
                }
            }
            std::fputc('\n', raw);
        }
        std::fclose(raw);
    }
    // Where each sample's stack enters system code from ours: the first frames inside the exe.
    std::unordered_map<std::string, uint32_t> chains;
    for (const auto& stack : stacks) {
        std::string chain;
        int exeFrames = 0;
        for (const uintptr_t rip : stack) {
            if (rip >= exeBase && rip < exeEnd) {
                chain += (chain.empty() ? "" : " < ") + describe(rip);
                if (++exeFrames == 5) break;
            } else if (exeFrames > 0) {
                break;
            }
        }
        if (stack.empty() || (stack[0] >= exeBase && stack[0] < exeEnd)) {
            continue;  // already in our code: the leaf table covers it
        }
        chains[describe(stack[0]) + " from " + (chain.empty() ? "?" : chain)]++;
    }
    std::vector<std::pair<std::string, uint32_t>> sortedChains(chains.begin(), chains.end());
    std::sort(sortedChains.begin(), sortedChains.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sortedChains.size() && i < 25; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile chain: %5.1f%%  %s\n", 100.0 * sortedChains[i].second / total,
                sortedChains[i].first.c_str());
    }

    std::unordered_map<std::string, uint32_t> byName;
    for (const auto& [rip, count] : samples) {
        std::string key;
        if (rip < exeBase || rip >= exeEnd) {
            HMODULE module = nullptr;
            char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(rip), &module)) {
                GetModuleFileNameA(module, path, MAX_PATH);
            }
            const char* slash = std::strrchr(path, '\\');
            key = std::string("[") + (slash ? slash + 1 : path) + "]";
        } else {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            if (it == entries.begin()) {
                key = "[exe, before translated code]";
            } else {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                key = text;
            }
        }
        byName[key] += count;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(byName.begin(), byName.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    RT_LOGF(RT_TAG_RUNTIME, "profile: %u samples, %zu translated functions\n", total, entries.size());
    for (size_t i = 0; i < sorted.size() && i < 60; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile: %5.1f%%  %s\n", 100.0 * sorted[i].second / total, sorted[i].first.c_str());
    }
}

void Run(HANDLE thread, double startSeconds, double lengthSeconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
    std::unordered_map<uintptr_t, uint32_t> samples;
    std::vector<std::vector<uintptr_t>> stacks;
    uint32_t total = 0;
    auto next = std::chrono::steady_clock::now();
    const auto end = next + std::chrono::duration<double>(lengthSeconds);
    while (std::chrono::steady_clock::now() < end) {
        // About 1000 samples a second; Sleep() is too coarse for that, so wait by yielding.
        next += std::chrono::microseconds(1000);
        while (std::chrono::steady_clock::now() < next) {
            std::this_thread::yield();
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            // No heap use while the game thread is suspended: it may hold the heap lock (deadlock).
            uintptr_t frames[24];
            int depth = 0;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                // The call stack, by the unwind data every x64 function carries.
                for (; depth < 24 && context.Rip != 0; ++depth) {
                    frames[depth] = static_cast<uintptr_t>(context.Rip);
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                    if (function == nullptr) {
                        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);  // a leaf: return address on top
                        context.Rsp += 8;
                        continue;
                    }
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                     &establisher, nullptr);
                }
            }
            ResumeThread(thread);
            if (depth > 0) {
                ++samples[frames[0]];
                ++total;
                stacks.emplace_back(frames, frames + depth);
            }
        }
    }
    Report(samples, stacks, total);
}
#endif

#if defined(__SWITCH__)
// ---- Switch capture backend ------------------------------------------------------------------
// The mechanism is the one SwitchWaker's in-NRO tooling and the Simpsons Hit & Run Switch port
// prove on Horizon: from a sampler thread, svcSetThreadActivity pauses the target thread,
// svcGetThreadContext3 dumps its registers, and the thread is resumed immediately. The target
// is the main thread (envGetMainThreadHandle): the guest executes there (main.cpp invokes the
// guest entry on it; guest threads are fibers on it). Aurora's frame worker is deliberately
// NOT sampled: it encodes and presents sealed frames, so its PCs would name Aurora and Dawn
// code, not the guest functions this profiler exists to rank. Its cost is already bounded by
// the perftimer stage split.
//
// Rate: 200 samples a second. Each sample holds the game thread paused only for a register
// dump and at most kProfileMaxDepth stack reads (microseconds), so the duty cycle is well
// under a percent even at 15 fps; the pauses can still jitter individual frame times, which
// is why the switch is off by default and a profiling session is not a frame-time measurement.
//
// Failure behaviour: a failed pause or context read skips the sample and counts it; 20 in a
// row stops sampling with an error line. Nothing here can leave the game thread paused: the
// resume runs on every path out of a successful pause, and the resume's own failure is counted
// like any other. Captured samples survive a dead sampler and print when the switch turns off.

constexpr int kProfilePeriodMs = 5;              // 200 samples a second
constexpr uint32_t kProfileMaxSamples = 120000;  // 10 minutes at that rate, then sampling stops
constexpr int kProfileMaxDepth = 20;             // PC, LR, then frame-pointer return addresses
constexpr int kProfileMaxFailures = 20;          // consecutive failed samples before giving up

std::atomic<bool> g_profileRunning{false};
std::thread g_profileThread;
std::mutex g_profileMutex;
std::unordered_map<uintptr_t, uint32_t> g_profileSamples;
std::vector<uintptr_t> g_profileFrames;  // every captured stack's addresses, concatenated
std::vector<uint8_t> g_profileDepths;    // one depth per captured stack
uint32_t g_profileTotal = 0;
uint32_t g_profileFailures = 0;
uintptr_t g_profileBase = 0;  // this module's load base, for nro+ offsets in the report
std::chrono::steady_clock::time_point g_profileStart{};
std::chrono::steady_clock::time_point g_profileEnd{};
bool g_profileEnded = false;

// One sample of the game thread. False when the pause or the context read failed (the caller
// counts it and carries on); the game thread is running again on every return path.
bool ProfileSampleOnce(Handle thread) {
    if (R_FAILED(svcSetThreadActivity(thread, ThreadActivity_Paused))) {
        return false;
    }
    ThreadContext context{};
    const Result read = svcGetThreadContext3(&context, thread);
    uintptr_t frames[kProfileMaxDepth];
    int depth = 0;
    if (R_SUCCEEDED(read)) {
        // Registers and stack memory only while the thread is paused, into stack locals: no
        // heap use here, the paused thread may hold the heap lock (the Windows rule, kept).
        frames[depth++] = context.pc.x;
        if (context.lr != 0 && context.lr != context.pc.x) {
            frames[depth++] = context.lr;
        }
        // The frame-pointer chain: at [fp] the caller's fp, at [fp + 8] its return address
        // (the AArch64 frame record). The guards are the proven capture's: 8-aligned, inside
        // the stack window above SP, strictly increasing, so a garbage or non-frame-pointer
        // link ends the walk instead of wandering into unrelated memory. Generated code is
        // not built with frame pointers in mind, so chains may end early; the leaf PC, which
        // the hot list ranks, never depends on the walk.
        uintptr_t fp = context.fp;
        while (depth < kProfileMaxDepth && fp != 0) {
            if ((fp & 7u) != 0 || fp < context.sp || fp > context.sp + 0x400000) {
                break;
            }
            const uintptr_t next = *reinterpret_cast<const uintptr_t*>(fp);
            const uintptr_t ret = *reinterpret_cast<const uintptr_t*>(fp + 8);
            if (ret != 0 && ret != frames[depth - 1]) {
                frames[depth++] = ret;
            }
            if (next <= fp) {
                break;
            }
            fp = next;
        }
    }
    const Result resumed = svcSetThreadActivity(thread, ThreadActivity_Runnable);
    if (R_FAILED(read) || R_FAILED(resumed)) {
        return false;
    }
    if (depth > 0) {
        std::lock_guard<std::mutex> lock(g_profileMutex);
        ++g_profileSamples[frames[0]];
        g_profileFrames.insert(g_profileFrames.end(), frames, frames + depth);
        g_profileDepths.push_back(static_cast<uint8_t>(depth));
        ++g_profileTotal;
    }
    return true;
}

void ProfileSamplerMain() {
    const Handle gameThread = envGetMainThreadHandle();
    {
        MemoryInfo info{};
        uint32_t pageInfo = 0;
        if (R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, reinterpret_cast<uintptr_t>(&ProfileSamplerMain)))) {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            g_profileBase = info.addr;
        }
    }
    int consecutiveFailures = 0;
    auto next = std::chrono::steady_clock::now();
    while (g_profileRunning.load(std::memory_order_acquire)) {
        next += std::chrono::milliseconds(kProfilePeriodMs);
        const auto now = std::chrono::steady_clock::now();
        if (next > now) {
            std::this_thread::sleep_until(next);
        } else {
            next = now;  // fell behind (a long pause, a stall): resync, do not burst
        }
        if (!g_profileRunning.load(std::memory_order_acquire)) {
            break;
        }
        bool capped = false;
        if (ProfileSampleOnce(gameThread)) {
            consecutiveFailures = 0;
        } else {
            {
                std::lock_guard<std::mutex> lock(g_profileMutex);
                ++g_profileFailures;
            }
            if (++consecutiveFailures >= kProfileMaxFailures) {
                RT_LOGF(RT_TAG_RUNTIME,
                        "[profile] error: sampling the game thread failed %d times in a row; sampling stopped, "
                        "toggle profile off for what was captured\n",
                        consecutiveFailures);
                std::fflush(stderr);
                g_profileRunning.store(false, std::memory_order_release);
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            capped = g_profileTotal >= kProfileMaxSamples;
        }
        if (capped) {
            RT_LOGF(RT_TAG_RUNTIME,
                    "[profile] sample cap reached (%u samples); sampling stopped, toggle profile off for the hot list\n",
                    kProfileMaxSamples);
            std::fflush(stderr);
            g_profileRunning.store(false, std::memory_order_release);
            break;
        }
    }
    std::lock_guard<std::mutex> lock(g_profileMutex);
    g_profileEnd = std::chrono::steady_clock::now();
    g_profileEnded = true;
}

// The hot list for the captured session, in the Windows report's shape: a begin line, the top
// 60 leaf functions, the top 25 chains, an end line. Resolution happens here, once, through
// the registry's host lookup (FindByHostAddress exists for exactly this: stack symbolication),
// so the sampler itself never touches the registry or the name tables. The name shown is the
// registry's, falling back to the decomp map name (GuestName) when the registry has none.
void ProfileReport() {
    std::unordered_map<uintptr_t, uint32_t> samples;
    std::vector<std::vector<uintptr_t>> stacks;
    uint32_t total = 0;
    uint32_t failures = 0;
    uintptr_t base = 0;
    double seconds = 0;
    {
        std::lock_guard<std::mutex> lock(g_profileMutex);
        samples = g_profileSamples;
        total = g_profileTotal;
        failures = g_profileFailures;
        base = g_profileBase;
        stacks.reserve(g_profileDepths.size());
        size_t at = 0;
        for (const uint8_t depth : g_profileDepths) {
            stacks.emplace_back(g_profileFrames.begin() + at, g_profileFrames.begin() + at + depth);
            at += depth;
        }
        const auto end = g_profileEnded ? g_profileEnd : std::chrono::steady_clock::now();
        seconds = std::chrono::duration<double>(end - g_profileStart).count();
    }
    if (total == 0) {
        RT_LOGF(RT_TAG_RUNTIME, "[profile] no samples captured\n");
        std::fflush(stderr);
        return;
    }
    std::vector<GuestProfileFormat::Entry> entries;
    {
        std::unordered_map<uintptr_t, bool> resolved;
        const auto resolve = [&](uintptr_t pc) {
            const auto info = TranslatedFunctionRegistry::FindByHostAddress(pc);
            if (!info || info->entryPoint == nullptr) {
                return;
            }
            const uintptr_t host = reinterpret_cast<uintptr_t>(info->entryPoint);
            if (!resolved.emplace(host, true).second) {
                return;
            }
            const char* name = info->name != nullptr && info->name[0] != '\0' ? info->name : GuestName(info->address);
            entries.push_back({host, info->address, name, info->kind == FunctionKind::Native});
        };
        for (const auto& sample : samples) {
            resolve(sample.first);
        }
        for (const auto& stack : stacks) {
            for (const uintptr_t pc : stack) {
                resolve(pc);
            }
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const GuestProfileFormat::Entry& a, const GuestProfileFormat::Entry& b) { return a.host < b.host; });
    RT_LOGF(RT_TAG_RUNTIME,
            "[profile] begin: %u samples in %.1f s at %d Hz from the game thread, %u failed samples, "
            "%zu translated functions\n",
            total, seconds, 1000 / kProfilePeriodMs, failures, entries.size());
    for (const auto& line : GuestProfileFormat::LeafLines(samples, entries, base, total, 60)) {
        RT_LOGF(RT_TAG_RUNTIME, "%s\n", line.c_str());
    }
    for (const auto& line : GuestProfileFormat::ChainLines(stacks, entries, base, total, 25)) {
        RT_LOGF(RT_TAG_RUNTIME, "%s\n", line.c_str());
    }
    for (const auto& line : GuestProfileFormat::InclusiveNativeLines(stacks, entries, base, total, 25)) {
        RT_LOGF(RT_TAG_RUNTIME, "%s\n", line.c_str());
    }
    RT_LOGF(RT_TAG_RUNTIME, "[profile] end\n");
    std::fflush(stderr);
}
#endif

} // namespace

void OnFramePresented() {
#ifdef _WIN32
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    const char* value = std::getenv("NSMBW_PROFILE");
    // SPM: also read "<start>:<length>" from profile.txt beside the executable (the test loop's switch).
    static char fileValue[64] = {};
    if (value == nullptr) {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        path = path.substr(0, path.find_last_of("\\/") + 1) + "profile.txt";
        if (FILE* f = std::fopen(path.c_str(), "r")) {
            if (std::fgets(fileValue, sizeof(fileValue), f)) value = fileValue;
            std::fclose(f);
        }
    }
    double start = 0, length = 0;
    if (value == nullptr || std::sscanf(value, "%lf:%lf", &start, &length) != 2 || length <= 0) {
        return;
    }
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::thread(Run, thread, start, length).detach();
#endif
}

#if defined(__SWITCH__)
void SetSwitchProfileEnabled(bool enabled) {
    if (enabled) {
        if (g_profileRunning.exchange(true, std::memory_order_acq_rel)) {
            RT_LOGF(RT_TAG_RUNTIME, "[profile] already running\n");
            std::fflush(stderr);
            return;
        }
        // A sampler that stopped on its own (failures, the cap) leaves a joinable thread
        // behind; collect it before the object is reused.
        if (g_profileThread.joinable()) {
            g_profileThread.join();
        }
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            g_profileSamples.clear();
            g_profileFrames.clear();
            g_profileDepths.clear();
            g_profileTotal = 0;
            g_profileFailures = 0;
            g_profileBase = 0;
            g_profileEnded = false;
            g_profileStart = std::chrono::steady_clock::now();
        }
        try {
            g_profileThread = std::thread(ProfileSamplerMain);
        } catch (...) {
            g_profileRunning.store(false, std::memory_order_release);
            RT_LOGF(RT_TAG_RUNTIME, "[profile] error: could not start the sampler thread\n");
            std::fflush(stderr);
            return;
        }
        RT_LOGF(RT_TAG_RUNTIME,
                "[profile] started: sampling the game thread at %d Hz; toggle profile off for the hot list\n",
                1000 / kProfilePeriodMs);
        std::fflush(stderr);
        return;
    }
    const bool wasRunning = g_profileRunning.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning && !g_profileThread.joinable()) {
        return;  // never started: nothing to stop or report
    }
    if (g_profileThread.joinable()) {
        g_profileThread.join();
    }
    ProfileReport();
}
#endif

} // namespace GuestProfiler
